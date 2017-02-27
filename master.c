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
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "libpq-int.h"
#include "tcop/utility.h"
#include "utils/snapmgr.h"
#include "utils/ps_status.h"


#define ALTER_SYSTEM_COMMAND "ALTER SYSTEM SET synchronous_standby_names TO '';"
#define STAT_REPLICATION_COMMAND "SELECT * FROM pg_stat_replication WHERE sync_state = 'sync';"

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

/*
 * Set up several parameters for master mode.
 */
void
setupKeeperMaster()
{
	/* Set up variable */
	retry_count = 0;

	/* Set process display which is exposed by ps command */
	set_ps_display(getStatusPsString(current_status), false);

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
				current_status = KEEPER_MASTER_CONNECTED;
				set_ps_display(getStatusPsString(current_status), false);
				elog(LOG, "pg_keeper connects to standby server");
				retry_count = 0;
			}
		}
		else
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
				current_status = KEEPER_MASTER_ASYNC;
				set_ps_display(getStatusPsString(current_status), false);
				standby_connected = false;
			}
		}
	}

	return true;
}

/*
 * Change synchronous replication to *asynchronous* replication
 * using by ALTER SYSTEM command up to 5 times.
 * XXX : Could we execute this via SPI instead?
 */
static void
changeToAsync(void)
{
	int ret;

	elog(LOG, "pg_keeper changes replication mode to asynchronous replication");

	/* Attempt to execute ALTER SYSTEM command */
	if (!execSQL(KeeperMaster, ALTER_SYSTEM_COMMAND))
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

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_exec(STAT_REPLICATION_COMMAND, 0);

	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
				(errmsg("failed to execute SELECT to confirm connecting standby server")));

	found = SPI_processed >= 1;

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	return found;
}
