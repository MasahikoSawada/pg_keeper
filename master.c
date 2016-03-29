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
#include "executor/spi.h"
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

#define ALTER_SYSTEM_COMMAND "ALTER SYSTEM synchronous_standby_names TO '';"

bool	KeeperMainMaster(void);
void	setupKeeperMaster(void);

static void changeToAsync(void);

/* GUC variables */
char	*keeper_slave_conninfo;

/* Variables for heartbeat */
static int retry_count;

/* Other variables */
bool	syncrep_enabled;

/*
 * Set up several parameters for master mode.
 */
void
setupKeeperMaster()
{
	PGconn *con;

	/* Set up variables */
	retry_count = 0;

	/* Confirm connection in advance */
	if (!(con = PQconnectdb(keeper_slave_conninfo)))
	{
		ereport(LOG,
				(errmsg("could not establish connection to slave server : %s",
						keeper_slave_conninfo)));
		proc_exit(1);
	}

	PQfinish(con);

	return;
}


/*
 * Main routine for master mode
 */
bool
KeeperMainMaster(void)
{
	elog(LOG, "entering pg_keeper master mode");

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

		if (!heartbeatServer(keeper_slave_conninfo, retry_count))
			retry_count++;

		if (retry_count >= keeper_keepalives_count)
		{
			changeToAsync();
			break;
		}
	}

	return true;
}

/*
 * Change synchronous replication to *asynchronous* replication
 * using by ALTER SYSTEM command.
 */
static void
changeToAsync(void)
{
	int ret;

    SPI_connect();

	ret = SPI_execute(ALTER_SYSTEM_COMMAND, true, 0);

	if (ret != SPI_OK_UTILITY)
	{
		ereport(LOG,
				(errmsg("failed to execute ALTER SYSTEM to change to asynchronous replication")));
		proc_exit(1);
	}

    SPI_finish();

	if (kill(PostmasterPid, SIGHUP) != 0)
	{
		ereport(LOG,
				(errmsg("failed to send SIGUSR1 signal to postmaster process : %d",
						PostmasterPid)));
		proc_exit(1);
	}
}
