/* -------------------------------------------------------------------------
 *
 * standby.c
 *
 * pg_keeper process standby mode.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_keeper.h"

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

#define	HEARTBEAT_SQL "select 1;"

bool	KeeperMainStandby(void);
void	setupKeeperStandby(void);

static void doPromote(void);
static void doAfterCommand(void);
static bool heartbeatPrimaryServer(void);

/* GUC variables */
char	*keeper_primary_conninfo;
char	*keeper_after_command;

/* Variables for hearbeat */
static char conninfo[MAXPGPATH];
static int retry_count;

/*
 * headbeatPrimaryServer()
 *
 * This fucntion does heatbeating to primary server. If could not establish connection
 * to primary server, or primary server didn't reaction, return false.
 */
static bool
heartbeatPrimaryServer(void)
{
	PGconn		*con;
	PGresult 	*res;

	/* Try to connect to primary server */
	if ((con = PQconnectdb(conninfo)) == NULL)
	{
		ereport(LOG,
				(errmsg("Could not establish conenction to primary server at %d time(s)",
						(retry_count + 1))));
		PQfinish(con);
		return false;
	}

	res = PQexec(con, HEARTBEAT_SQL);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		/* Failed to ping to master server, report the number of retrying */
		ereport(LOG,
				(errmsg("could not get tuple from primary server at %d time(s)",
						(retry_count + 1))));
		PQfinish(con);
		return false;
	}

	/* Primary server is alive now */
	PQfinish(con);
	return true;
}

/*
 * Set up several parameters for standby mode.
 */
void
setupKeeperStandby()
{
	PGconn *con;

	/* Set up variables */
	snprintf(conninfo, MAXPGPATH, "%s", keeper_primary_conninfo);
	retry_count = 0;

	/* Connection confirm */
	if(!(con = PQconnectdb(conninfo)))
	{
		ereport(LOG,
				(errmsg("could not establish connection to primary server : %s", conninfo)));
		proc_exit(1);
	}

	PQfinish(con);

	return;
}

/*
 * Main routine for standby mode.
 */
bool
KeeperMainStandby(void)
{
	elog(LOG, "entering pg_keeper standby mode");

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
		}

		/*
		 * Do heartbeat connection to master server. If heartbeat is failed,
		 * increment retry_count..
		 */
		if (!heartbeatPrimaryServer())
			retry_count++;

		/* If retry_count is reached to keeper_keepalives_count,
		 * do promote the standby server to master server, and exit.
		 */
		if (retry_count >= keeper_keepalives_count)
		{
			doPromote();

			/* If after command is given, execute it */
			if (keeper_after_command)
				doAfterCommand();

			return true;
		}
	}

	return true;
}

/*
 * doPromote()
 *
 * Promote standby server using ordinally way which is used by
 * pg_ctl client tool. Put trigger file into $PGDATA, and send
 * SIGUSR1 signal to standby server.
 */
static void
doPromote(void)
{
	char trigger_filepath[MAXPGPATH];
	FILE *fp;

    snprintf(trigger_filepath, 1000, "%s/promote", DataDir);

	if ((fp = fopen(trigger_filepath, "w")) == NULL)
	{
		ereport(LOG,
				(errmsg("could not create promote file: \"%s\"", trigger_filepath)));
		proc_exit(1);
	}

	if (fclose(fp))
	{
		ereport(LOG,
				(errmsg("could not close promote file: \"%s\"", trigger_filepath)));
		proc_exit(1);
	}

	ereport(LOG,
			(errmsg("promote standby server to primary server")));

	/* Do promote */
	if (kill(PostmasterPid, SIGUSR1) != 0)
	{
		ereport(LOG,
				(errmsg("failed to send SIGUSR1 signal to postmaster process : %d",
						PostmasterPid)));
		proc_exit(1);
	}
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

