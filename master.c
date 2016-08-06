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
#include "syncrep.h"
#include "util.h"

/* These are always necessary for a bgworker */
#include "access/xlog.h"
#include "commands/extension.h"
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
static void resetRetryCounts(void);
static bool heartbeatServerMaster(int *r_counts);

/* Variables for heartbeat */
static int *retry_counts;

/*
 * Set up several parameters for master mode.
 */
void
setupKeeperMaster()
{
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
					   keeper_keepalives_time * 1000L);
		ResetLatch(&MyProc->procLatch);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			return false;

		/* If got SIGHUP, reload the configuration file */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
			parse_synchronous_standby_names();
		}

		if (got_sigusr1)
		{
			got_sigusr1 = false;
			pg_usleep(1 * 1000L * 1000L);

			/* Update own memory */
			updateLocalCache();

			/* Update retry_counts information */
			resetRetryCounts();

			elog(LOG, "pg_keeper update own cache, currently number of nodes is %d",
				 nKeeperRepNodes);
		}

		/*
		 * We get started pooling to synchronous standby server
		 * after a standby server connected to master server.
		 */
		if (current_status == KEEPER_MASTER_READY)
		{
			int num = 0;

			getAllRepNodes(&num, true);

			/* Standby connected */
			if (num > 1)
			{
				current_status = KEEPER_MASTER_CONNECTED;
				set_ps_display(getStatusPsString(current_status), false);
				elog(LOG, "pg_keeper connects to standby server");
				resetRetryCounts();
			}
		}
		else if (current_status == KEEPER_MASTER_CONNECTED)
		{
			/*
			 * Pooling to standby servers. If we could not pool to standby enough
			 * to continue synchronous replication at more than keeper_keepalive_count
			 * counts *in a row*, then change to asynchronous replication using
			 * ALTER SYSTEM.
			 */
			if (!heartbeatServerMaster(retry_counts))
			{
				changeToAsync();

				/*
				 * After changing to asynchronou replication, reset
				 * state of itself and restart pooling.
				 */
				current_status = KEEPER_MASTER_ASYNC;
				set_ps_display(getStatusPsString(current_status), false);
				resetRetryCounts();

				/* XXX : Should we change all is_sync on management table to false? */
			}
		}
		else if (current_status == KEEPER_MASTER_ASYNC)
		{
			/* XXX : Should we continue to pool the all standbys? */
		}
	}

	return true;
}

/*
 * heartbeatServerMaster()
 * Pooling to standby servers. Return false iif we could not pool the standbys enough
 * to continue synchronous replication at more than keeper_keepalive_count *in a row*.
 */
static bool
heartbeatServerMaster(int *r_counts)
{
	int connect_sync = 0;
	int registered_sync = 0;
	int i;
	bool connect_enough = true;
	bool retry_count_reached = false;

	/* Pooling to all nodes listed on KeeperRepNodes */
	for (i = 0; i < nKeeperRepNodes; i++)
	{
		KeeperNode *node = &(KeeperRepNodes[i]);
		char *connstr = node->conninfo;

		/* Not interested in master server */
		if (node->is_master)
			continue;

		/* Not insterested in async standby server as well */
		if (!node->is_sync)
			continue;

		/* Count registred sync node */
		registered_sync++;

		if (!(execSQL(connstr, HEARTBEAT_SQL)))
		{
			(r_counts[i])++;
			ereport(LOG,
					(errmsg("pg_keeper failed to execute pooling %d time(s)", r_counts[i] + 1)));

			/* Check if we could not connect to "sync" standby */
			if (r_counts[i] > keeper_keepalives_count)
				retry_count_reached = true;

			continue;
		}

		/* Success polling, reset retry_counts */
		r_counts[i] = 0;

		/* Keep track of the number of sync standby */
		if (node->is_sync)
			connect_sync++;
	}

	/*
	 * Set the connect_enough false only if the number of registered node is more
	 * than sync standbys required sync replication, but the number connecting
	 * standby is not enough.
	 */
	if (registered_sync >= RepConfig->num_sync &&
		connect_sync < RepConfig->num_sync &&
		retry_count_reached)
		connect_enough = false;

	elog(NOTICE, "total %d, registerd_sync %d, connect_sync %d, required_sync %d, connect_enough %d",
		 nKeeperRepNodes, registered_sync, connect_sync, RepConfig->num_sync, connect_enough);

	return connect_enough;
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
 * Check if synchronous ustandby server has conncted to master server
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

static void
resetRetryCounts(void)
{
	if (retry_counts)
		pfree(retry_counts);

	retry_counts = palloc0(sizeof(int) * nKeeperRepNodes);
}
