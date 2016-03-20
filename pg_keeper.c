/* -------------------------------------------------------------------------
 *
 * pg_keeper.c
 *
 * Simple clustering extension module for PostgreSQL.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

/* These are always necessary for a bgworker */
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

PG_MODULE_MAGIC;

void		_PG_init(void);
void		KeeperMain(Datum);
static void setupKeeper(void);
static void doPromote(void);
static bool heartbeatPrimaryServer(void);

/* Function for signal handler */
static void pg_keeper_sigterm(SIGNAL_ARGS);
static void pg_keeper_sighup(SIGNAL_ARGS);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int	keeper_keepalives_time;
static int	keeper_keepalives_count;
static char	*keeper_primary_conninfo = NULL;

/* Variables for connections */
static char conninfo[MAXPGPATH];

/* Variables for cluster management */
static int retry_count;

typedef struct worktable
{
	const char *schema;
	const char *name;
} worktable;


/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
pg_keeper_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *		Set a flag to let the main loop to reread the config file, and set
 *		our latch to wake it up.
 */
static void
pg_keeper_sighup(SIGNAL_ARGS)
{
	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/*
 * Set up several parameters for a worker process
 */
static void
setupKeeper(void)
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
 * Main routine of pg_keeper.
 */
void
KeeperMain(Datum main_arg)
{
	setupKeeper();
		
	/* Establish signal handlers before unblocking signals */
	pqsignal(SIGHUP, pg_keeper_sighup);
	pqsignal(SIGTERM, pg_keeper_sigterm);
	
	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

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
			proc_exit(1);

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
			proc_exit(0);
		}
	}

	proc_exit(1);
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
 * Entrypoint of this module.
 *
 * We register more than one worker process here, to demonstrate how that can
 * be done.
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* get the configuration */
	DefineCustomIntVariable("pg_keeper.keepalives_time",
							"Specific time between polling to primary server",
							NULL,
							&keeper_keepalives_time,
							5,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_keeper.keepalives_count",
							"Specific retry count until promoting standby server",
							NULL,
							&keeper_keepalives_count,
							1,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("pg_keeper.primary_conninfo",
							"Connection information for primary server",
							NULL,
							&keeper_primary_conninfo,
							"",
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = KeeperMain;
	worker.bgw_notify_pid = 0;
	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_keeper");
	worker.bgw_main_arg = Int32GetDatum(1);
	RegisterBackgroundWorker(&worker);
}
