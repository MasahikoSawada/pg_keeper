/* -------------------------------------------------------------------------
 *
 * pg_keeper.c
 *
 * Simple clustering extension module for PostgreSQL.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_keeper.h"

/* These are always necessary for a bgworker */
#include "access/xlog.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "replication/syncrep.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/ps_status.h"

/* these headers are used by this particular worker's code */
#include "tcop/utility.h"
#include "libpq-int.h"

PG_MODULE_MAGIC;

#define HEARTBEAT_SQL "SELECT 1"

void	_PG_init(void);
void	KeeperMain(Datum);
bool	heartbeatServer(const char *conninfo, int r_count);
bool	execSQL(const char *conninfo, const char *sql);

static void checkParameter(void);
static char *getStatusPsString(KeeperStatus status);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void pgkeeper_shmem_startup(void);

/* Function for signal handler */
static void pgkeeper_sigterm(SIGNAL_ARGS);
static void pgkeeper_sighup(SIGNAL_ARGS);

/* flags set by signal handlers */
sig_atomic_t got_sighup = false;
sig_atomic_t got_sigterm = false;

/* GUC variables */
int	pgkeeper_keepalives_time;
int	pgkeeper_keepalives_count;
char *pgkeeper_partner_conninfo;
char *pgkeeper_my_conninfo;

KeeperShmem	*keeperShmem;

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
							&pgkeeper_keepalives_time,
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
							&pgkeeper_keepalives_count,
							4,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("pg_keeper.partner_conninfo",
							   "Connection information for partner server",
							   NULL,
							   &pgkeeper_partner_conninfo,
							   NULL,
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("pg_keeper.my_conninfo",
							   "My connection information used for ALTER SYSTEM",
							   NULL,
							   &pgkeeper_my_conninfo,
							   NULL,
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("pg_keeper.after_command",
							   "Shell command that will be called after promoted",
							   NULL,
							   &pgkeeper_after_command,
							   NULL,
							   PGC_SIGHUP,
							   GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);

	/* Install hook */
    prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgkeeper_shmem_startup;

	/* request additional sharedresource */
	RequestAddinShmemSpace(MAXALIGN(sizeof(KeeperShmem)));

	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
#if PG_VERSION_NUM >= 100000
	strcpy(worker.bgw_library_name, "pg_keeper");
	strcpy(worker.bgw_function_name, "KeeperMain");
#else
	worker.bgw_main = KeeperMain;
#endif
	worker.bgw_notify_pid = 0;

	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_keeper");
	worker.bgw_main_arg = Int32GetDatum(1);
	RegisterBackgroundWorker(&worker);
}

void _PG_fini(void)
{
	/* Uninstall hook */
    shmem_startup_hook = prev_shmem_startup_hook;
}

static void
pgkeeper_shmem_startup(void)
{
	bool found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	keeperShmem = NULL;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	keeperShmem = ShmemInitStruct("pg_keeper",
								  sizeof(KeeperShmem),
								  &found);

	if (!found)
	{
		SpinLockInit(&keeperShmem->mutex);
		keeperShmem->sync_mode = false;
	}

	LWLockRelease(AddinShmemInitLock);
}

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
pgkeeper_sigterm(SIGNAL_ARGS)
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
pgkeeper_sighup(SIGNAL_ARGS)
{
	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/*
 * Entry point for pg_keeper.
 */
void
KeeperMain(Datum main_arg)
{
	int ret;

	/* Sanity check */
	checkParameter();

	/* Determine keeper mode of itself */
	updateStatus(RecoveryInProgress() ? KEEPER_STANDBY_READY : KEEPER_MASTER_READY);

	/* Establish signal handlers before unblocking signals */
	pqsignal(SIGHUP, pgkeeper_sighup);
	pqsignal(SIGTERM, pgkeeper_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
#if PG_VERSION_NUM >= 110000
	BackgroundWorkerInitializeConnection("postgres", NULL, 0);
#else
	BackgroundWorkerInitializeConnection("postgres", NULL);
#endif

exec:

	if (keeperShmem->current_status == KEEPER_MASTER_READY)
	{
		/* Routine for master_mode */
		setupKeeperMaster();
		ret = KeeperMainMaster();
	}
	else if (keeperShmem->current_status == KEEPER_STANDBY_READY)
	{
		/* Routine for standby_mode */
		setupKeeperStandby();
		ret = KeeperMainStandby();

		/*
		 * After promoting is sucessfully done, attempt to re-execute
		 * main routine as master mode in order to avoid to restart
		 * for invoking pg_keeper process again.
		 */
		if (ret)
		{
			/* Change mode to master mode */
			updateStatus(KEEPER_MASTER_READY);

			goto exec;
		}
	}
	else
		ereport(ERROR, (errmsg("invalid keeper mode : \"%d\"", keeperShmem->current_status)));

	proc_exit(ret);
}

/*
 * heartbeatServer()
 *
 * This fucntion does heartbeating to given server using HEARTBEAT_SQL.
 * If could not establish connection to server or server didn't reaction,
 * emits log message and return false.
 */
bool
heartbeatServer(const char *conninfo, int r_count)
{
	if (!(execSQL(conninfo, HEARTBEAT_SQL)))
	{
		ereport(LOG,
				(errmsg("pg_keeper failed to connect %d time(s)", r_count + 1)));
		return false;
	}

	return true;
}

/*
 * Simple function to execute one SQL.
 */
bool
execSQL(const char *conninfo, const char *sql)
{
	PGconn		*con;
	PGresult 	*res;

	/* Try to connect to primary server */
	if ((con = PQconnectdb(conninfo)) == NULL)
	{
		ereport(LOG,
				(errmsg("could not establish conenction to server : \"%s\"",
					conninfo)));

		PQfinish(con);
		return false;
	}

	res = PQexec(con, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK &&
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		/* Failed to ping to master server, report the number of retrying */
		ereport(LOG,
				(errmsg("could not get tuple from server : \"%s\"",
					conninfo)));

		PQfinish(con);
		return false;
	}

	/* Primary server is alive now */
	PQfinish(con);
	return true;
}

/* Check the mandatory parameteres */
static void
checkParameter()
{
	if (!EnableHotStandby)
		ereport(ERROR, (errmsg("hot_standby must be enabled.")));

	if (pgkeeper_partner_conninfo == NULL || pgkeeper_partner_conninfo[0] == '\0')
		ereport(ERROR, (errmsg("pg_keeper.partner_conninfo must be specified.")));

	if (pgkeeper_my_conninfo == NULL || pgkeeper_my_conninfo[0] == '\0')
		ereport(ERROR, (errmsg("pg_keeper.my_conninfo must be specified.")));

	if (SyncRepStandbyNames != NULL && SyncRepStandbyNames[0] != '\0')
	{
		SpinLockAcquire(&keeperShmem->mutex);
		keeperShmem->sync_mode = true;
		SpinLockRelease(&keeperShmem->mutex);
	}
}

static char *
getStatusPsString(KeeperStatus status)
{
	if (status == KEEPER_STANDBY_READY)
		return "(standby:ready)";
	else if (status == KEEPER_STANDBY_CONNECTED)
		return "(standby:connected)";
	else if (status == KEEPER_STANDBY_ALONE)
		return "(standby:alone)";
	else if (status == KEEPER_MASTER_READY)
		return "(master:ready)";
	else if (status == KEEPER_MASTER_CONNECTED)
		return "(master:connected)";
	else if (status == KEEPER_MASTER_ASYNC)
		return "(master:async)";
	else
		ereport(ERROR, (errmsg("Invalid status %d", status)));
}

void
updateStatus(KeeperStatus status)
{
	/* Update statuc in shmem */
	SpinLockAcquire(&keeperShmem->mutex);
	keeperShmem->current_status = status;
	SpinLockRelease(&keeperShmem->mutex);

	/* Then, update process title */
	set_ps_display(getStatusPsString(status), false);
}
