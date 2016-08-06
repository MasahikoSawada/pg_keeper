/* -------------------------------------------------------------------------
 *
 * standby.c
 *
 * standb mode for pg_keeper.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_keeper.h"
#include "syncrep.h"
#include "util.h"

/* These are always necessary for a bgworker */
#include "access/xlog.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "tcop/utility.h"
#include "libpq-int.h"
#include "utils/ps_status.h"

bool	KeeperMainStandby(void);
void	setupKeeperStandby(void);

static bool doPromote(void);
static void doAfterCommand(void);
static bool heartbeatServerStandby(int *retry_counts);

/* GUC variables */
char	*keeper_after_command;

/* Variables for heartbeat */
static int *retry_counts;

/*
 * Set up several parameters for standby mode.
 */
void
setupKeeperStandby()
{
	/* Initialize */
	retry_counts = resetRetryCounts(retry_counts);

	/* Set process display which is exposed by ps command */
	current_status = KEEPER_STANDBY_CONNECTED;

	/* Initialize own cache if pg_keeper is already installed */
	if (checkExtensionInstalled())
		updateLocalCache(false);
}

/*
 * Main routine for standby mode.
 */
bool
KeeperMainStandby(void)
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

		/* If got SIGUSR1, update local cache for KeeperRepNodes */
		if (got_sigusr1)
		{
			got_sigusr1 = false;
			pg_usleep(5 * 1000L * 100L);

			/* Update own memeory */
			updateLocalCache(false);

			/* Update retry_counts information */
			retry_counts = resetRetryCounts(retry_counts);
		}

		/*
		 * Pooling to master server. If heartbeat is failed, increment retry_count.
		 * As a result of polling, if retry_count is reached to
		 * keeper_keepalives_count, do promote the standby server to master server,
		 * and exit.
		 */
		if (!heartbeatServerStandby(retry_counts))
		{
			bool promoted;

			/* Promote */
			promoted = doPromote();

			if (promoted)
			{
				/* If after command is given, execute it */
				if (keeper_after_command)
					doAfterCommand();
			}

			/* Change to status of this node to master mode */
			current_status = KEEPER_MASTER_READY;
			updateLocalCache(false);
			return true;
		}
	}

	return true;
}

/*
 * Promote standby server using ordinally way which is used by
 * pg_ctl client tool. Put trigger file into $PGDATA, and send
 * SIGUSR1 signal to standby server.
 */
static bool
doPromote(void)
{
	char trigger_filepath[MAXPGPATH];
	FILE *fp;
	bool promote;

	promote = isNextMaster(keeper_node_name);

	if (promote)
	{
		/* Create promote file newly */
		snprintf(trigger_filepath, 1000, "%s/promote", DataDir);
		if ((fp = fopen(trigger_filepath, "w")) == NULL)
			ereport(ERROR,
					(errmsg("could not create promote file: \"%s\"", trigger_filepath)));

		if (fclose(fp))
			ereport(ERROR,
					(errmsg("could not close promote file: \"%s\"", trigger_filepath)));

		elog(LOG,"promote standby server to primary server");

		/* Do promote */
		if (kill(PostmasterPid, SIGUSR1) != 0)
			ereport(ERROR,
					(errmsg("failed to send SIGUSR1 signal to postmaster process : %d",
							PostmasterPid)));

		return true;
	}

	return false;
}

/*
 * Attempt to execute an external shell command after promotion.
 */
static void
doAfterCommand(void)
{
	int	rc;

	Assert(keeper_after_command);

	ereport(LOG,
			(errmsg("executing after promoting command \"%s\"",
					keeper_after_command)));

	rc = system(keeper_after_command);

	if (rc != 0)
	{
		ereport(LOG,
				(errmsg("failed to execute after promoting command \"%s\"",
						keeper_after_command)));
	}
}

/*
 * heartbeatServerStandby()
 * Polling to master server directly and indirectly via other standbys. Return false
 * iif we could not poll to master via all standbys including itself.
 */
static bool
heartbeatServerStandby(int *retry_counts)
{
#define KEEPER_SQL_INDIRECT_POOLING "SELECT pgkeeper.indirect_polling('%s')"
	int i;
	char *master_conninfo;
	bool retry_count_reached = false;

	/* Get master server connection information */
	for (i = 0; i < nKeeperRepNodes; i++)
	{
		KeeperNode *node = &(KeeperRepNodes[i]);

		if (node->is_master)
		{
			master_conninfo = node->conninfo;
			break;
		}
	}

	/*
	 * Polling to the all servers. Return if pg_keeper made a dicision to
	 * not be able to continue steaming replication. The standby server always
	 * polling to master via all standby server indirectly. If all result of polling
	 * via all standbys is false more then keeper_keepalives_count in a row the
	 * we decide to promote. That's a our promoting policy.
	 */
	for (i = 0; i < nKeeperRepNodes; i++)
	{
		KeeperNode *node = &(KeeperRepNodes[i]);
		char *connstr = node->conninfo;
		bool ret;
		char sql[BUFSIZE];
		bool indirect_ret;

		/*
		 * We are not insterested in master server directly beacause
		 * polling to master will be executed by execute indirectly
		 * polling by itself.
		 */
		if (node->is_master)
			continue;

		/* Polling to master indirectly via other standby including itself */
		snprintf(sql, BUFSIZE, KEEPER_SQL_INDIRECT_POOLING, master_conninfo);
		ret = execSQL(connstr, sql, &indirect_ret);

		if (!ret)
		{
			/* Emit warning log */
			ereport(LOG,
					(errmsg("pg_keeper failed to poll to \"%s\" at %d time(s)",
							connstr, retry_counts[i])));

			/* Neighbor standby migit be not available, ignore this result */
			continue;
		}

		if (!indirect_ret)
		{
			/* Neighbor standby says that the master server might be not available */
			(retry_counts[i])++;

			/* Emit warning log */
			ereport(LOG,
					(errmsg("pg_keeper could not poll to the master server via \"%s\" at %d time(s)",
							connstr, retry_counts[i])));

			/* Check if retry_counts exceeds the threshold */
			if (retry_counts[i] > keeper_keepalives_count)
				retry_count_reached = true;
			else
				retry_count_reached = false;

			continue;
		}

		/* Success to connect to the master indirectly */
		retry_counts[i] = 0;
	}

	/*
	 * retry_count_reached is true, which means this standby could not connect not only
	 * the master but also other standbys could not connect to master server as well.
	 */
	if (retry_count_reached)
		return false;

	return true;
}
