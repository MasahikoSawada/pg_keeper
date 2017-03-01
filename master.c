/* -------------------------------------------------------------------------
 *
 * master.c
 *
 * master mode for pg_keeper.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_keeper.h"

/* These are always necessary for a bgworker */
#include "access/xlog.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "replication/syncrep.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "libpq-int.h"
#include "tcop/utility.h"
#include "utils/snapmgr.h"
#include "utils/ps_status.h"


#define SQL_CHANGE_TO_ASYNC			"ALTER SYSTEM SET synchronous_standby_names TO '';"

bool	KeeperMainMaster(void);
void	setupKeeperMaster(void);

static void changeToAsync(void);
static bool checkStandbyIsConnected(void);

/* Variables for heartbeat */
static int retry_count;

/* GUC variables */
char	*keeper_node1_conninfo;

/* Other variables */
bool	standby_connected;

/* Set up several parameters for master mode */
void
setupKeeperMaster()
{
	/* Set up variable */
	retry_count = 0;

	/* Set process display which is exposed by ps command */
	updateStatus(KEEPER_MASTER_READY);

	/*
	 * There migth be a entry in this server if this server is
	 * starting up after failover and recovered. So reset it.
	 */
	if (!execSQL(pgkeeper_my_conninfo, "ALTER SYSTEM RESET synchronous_standby_names"))
		ereport(ERROR,
				(errmsg("failed to execute ALTER SYSTEM to change to reset")));

	return;
}

/*
 * Main routine for master mode
 */
bool
KeeperMainMaster(void)
{
	ereport(LOG, (errmsg("started pg_keeper worker as master mode")));

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm)
	{
		int		rc;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pgkeeper_keepalives_time * 1000L);
		ResetLatch(&MyProc->procLatch);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			return false;

		/* If got SIGHUP, reload the configuration file */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);

			if (SyncRepStandbyNames != NULL && SyncRepStandbyNames[0] != '\0')
			{
				SpinLockAcquire(&keeperShmem->mutex);
				keeperShmem->sync_mode = true;
				SpinLockRelease(&keeperShmem->mutex);
			}
		}

		/*
		 * We get started pooling to synchronous standby server
		 * after a standby server connected to master server.
		 */
		if (!standby_connected)
		{
			standby_connected = checkStandbyIsConnected();

			/* Standby connected */
			if (standby_connected)
			{
				if (keeperShmem->sync_mode)
					updateStatus(KEEPER_MASTER_CONNECTED);
				else
					updateStatus(KEEPER_MASTER_ASYNC);

				ereport(LOG, (errmsg("pg_keeper connects to standby server")));
				retry_count = 0;
			}
		}
		if (keeperShmem->sync_mode)
		{
			/*
			 * Pooling to standby server. If heartbeat is failed,
			 * increment retry_count.
			 */
			if (!heartbeatServer(pgkeeper_partner_conninfo, retry_count))
				retry_count++;
			else
				retry_count = 0; /* reset count */

			/*
			 * Change to asynchronous replication using ALTER SYSTEM
			 * command iff master server could not connect to standby server
			 * more than pgkeeper_keepalives_count counts *in a row*.
			 */
			if (retry_count >= pgkeeper_keepalives_count)
			{
				changeToAsync();

				/*
				 * After changing to asynchronou replication, reset
				 * state of itself and restart pooling.
				 */
				updateStatus(KEEPER_MASTER_ASYNC);
				standby_connected = false;
			}
		}
		/* nothing to do if in async mode */
	}

	return true;
}

/*
 * Change synchronous replication to *asynchronous* replication
 * using by ALTER SYSTEM command up to 5 times.
 */
static void
changeToAsync(void)
{
	int ret;

	elog(LOG, "pg_keeper changes replication mode to asynchronous replication");

	if (!execSQL(pgkeeper_my_conninfo, SQL_CHANGE_TO_ASYNC))
		ereport(ERROR,
				(errmsg("failed to execute ALTER SYSTEM to change to asynchronous replication")));

	/* Then, send SIGHUP signal to Postmaster process */
	if ((ret = kill(PostmasterPid, SIGHUP)) != 0)
		ereport(ERROR,
				(errmsg("failed to send SIGHUP signal to postmaster process : %d", ret)));
}

/*
 * Check if synchronous standby server has conncted to master server
 * through checking pg_stat_replication system view via SPI.
 */
static bool
checkStandbyIsConnected()
{
	int ret;
	bool found;
	StringInfo sql = makeStringInfo();

	appendStringInfo(sql, "SELECT * FROM pg_stat_replication");
	if (keeperShmem->sync_mode)
		appendStringInfo(sql, " WHERE sync_state = 'sync'");

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_exec(sql->data, 0);

	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
				(errmsg("failed to execute SELECT to confirm connecting standby server")));

	found = SPI_processed == 1;

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	return found;
}
