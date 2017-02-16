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
static bool heartbeatServerMaster(int *r_counts);
static bool deleteMaster(void);
static bool updateNewMaster(void);

/* Variables for heartbeat */
static int *retry_counts;

/*
 * Set up several parameters for master mode.
 */
void
setupKeeperMaster()
{
	/* Set process display which is exposed by ps command */
	set_ps_display(getStatusPsString(current_status, 0), false);

	/* Update own cache if pg_keeper is already installed */
	if (checkExtensionInstalled())
	{
		updateLocalCache(false);
		retry_counts = resetRetryCounts(retry_counts);
	}
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
			updateManageTableAccordingToSSNames(true);
		}

		/* If got SIGUSR1, update local cache for KeeperRepNodes */
		if (got_sigusr1)
		{
			got_sigusr1 = false;
			pg_usleep(1 * 1000L * 1000L);

			/* Update own memory and send SIGUSR1 of other standbys indirectly */
			updateLocalCache(true);

			/* Update retry_counts information */
			retry_counts = resetRetryCounts(retry_counts);
		}

		/*
		 * We get started pooling to synchronous standby server
		 * after a standby server connected to master server.
		 */
		if (current_status == KEEPER_MASTER_READY)
		{
			int n_in_table = 0;
			int n_connect_standbys;

			/*
			 * the master server is ready status but after promoted,
			 * we should update new master server.
			 */
			if (promoted && !RecoveryInProgress())
			{
					START_SPI_TRANSACTION();

					/* Update RepConfig data */
					parse_synchronous_standby_names();
					/* Delete old master */
					deleteMaster();
					/* Set new master */
					updateNewMaster();
					/* Update management table */
					updateManageTableAccordingToSSNames(false);

					END_SPI_TRANSACTION();

					/* Update local cache */
					updateLocalCache(false);

					promoted = false;
			}

			/* Check if any standby is already connected */
			getAllRepNodes(&n_in_table, true);
			n_connect_standbys = getNumberOfConnectingStandbys();

			/*
			 * Once enough standbys are connecting to the master server and
			 * all standbys are registered to manage table, start to monitoring.
			 */
			if (n_connect_standbys > 0 && (n_connect_standbys + 1) == n_in_table)
			{
				current_status = KEEPER_MASTER_CONNECTED;
				updateLocalCache(false);
				ereport(LOG,
						(errmsg("pg_keeper connects to standby servers, start monitoring")));
				retry_counts = resetRetryCounts(retry_counts);
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
				/* Change to asynchronous replication */
				changeToAsync();

				/*
				 * After changing to asynchronou replication, reset
				 * state of itself and restart pooling.
				 */
				current_status = KEEPER_MASTER_ASYNC;

				updateLocalCache(false);
				retry_counts = resetRetryCounts(retry_counts);
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
 * Polling to standby servers. Return false iif we could not poll the standbys enough
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

		if (!(heartbeatServer(connstr)))
		{
			/* Increment retry count of this node */
			(r_counts[i])++;

			/* Emit warning log */
			ereport(WARNING,
					(errmsg("pg_keeper failed to poll to \"%s\" at %d time(s)",
							connstr, r_counts[i])));

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

	ret = spiSQLExec(ALTER_SYSTEM_COMMAND, true);

	if (!ret)
		ereport(LOG,
				(errmsg("failed to execute ALTER SYSTEM to change to asynchronous replication")));

	/* Then, send SIGHUP signal to Postmaster process */
	if ((ret = kill(PostmasterPid, SIGHUP)) != 0)
		ereport(ERROR,
				(errmsg("failed to send SIGHUP signal to postmaster process : %d", ret)));
}

/*
 * Delete the master server row from management table.
 */
static bool
deleteMaster(void)
{
#define KEEPER_SQL_DELETE_MASTER "DELETE FROM %s WHERE is_master"
	StringInfoData sql;
	bool ret;

	initStringInfo(&sql);
	appendStringInfo(&sql, KEEPER_SQL_DELETE_MASTER, KEEPER_MANAGE_TABLE_NAME);
	ret = spiSQLExec(sql.data, false);

	return ret;
}

/*
 * Set the standby node marked is_nextmaster to new master server.
 */
static bool
updateNewMaster(void)
{
#define KEEPER_SQL_UPDATE_NEW_MASTER "UPDATE %s SET is_master = true WHERE is_nextmaster"
	StringInfoData sql;
	bool ret;

	initStringInfo(&sql);
	appendStringInfo(&sql, KEEPER_SQL_UPDATE_NEW_MASTER, KEEPER_MANAGE_TABLE_NAME);
	ret = spiSQLExec(sql.data, false);

	return ret;
}
