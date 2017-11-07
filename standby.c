/* -------------------------------------------------------------------------
 *
 * standby.c
 *
 * standby mode for pg_keeper.
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

#include "pgstat.h"

/* these headers are used by this particular worker's code */
#include "tcop/utility.h"
#include "libpq-int.h"
#include "utils/ps_status.h"

#define	HEARTBEAT_SQL "select 1;"

bool	KeeperMainStandby(void);
void	setupKeeperStandby(void);

static void doPromote(void);
static void doAfterCommand(void);

/* GUC variables */
char	*pgkeeper_after_command;

/* Variables for heartbeat */
static int retry_count;

/*
 * Set up several parameters for standby mode.
 */
void
setupKeeperStandby()
{
	PGconn *con;

	/* Set up variables */
	retry_count = 0;

	/* Connection confirm */
	if(!(con = PQconnectdb(pgkeeper_partner_conninfo)))
		ereport(ERROR,
				(errmsg("could not establish connection to primary server : %s",
						pgkeeper_partner_conninfo)));

	PQfinish(con);

	/* Set process display which is exposed by ps command */
	updateStatus(KEEPER_STANDBY_CONNECTED);

	return;
}

/*
 * Main routine for standby mode. Returning true means that
 * this standby can be promoted later.
 */
bool
KeeperMainStandby(void)
{
	ereport(LOG, (errmsg("started pg_keeper worker as standby mode")));

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
#if PG_VERSION_NUM >= 100000
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pgkeeper_keepalives_time * 1000L,
					   PG_WAIT_EXTENSION);
#else
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pgkeeper_keepalives_time * 1000L);
#endif
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
		 * Pooling to master server. If heartbeat is failed,
		 * increment retry_count.
		 */
		if (!heartbeatServer(pgkeeper_partner_conninfo, retry_count))
			retry_count++;
		else
			retry_count = 0; /* reset count */

		/*
		 * If retry_count is reached to keeper_keepalives_count,
		 * do promote the standby server to master server, and exit.
		 */
		if (retry_count >= pgkeeper_keepalives_count)
		{
			doPromote();

			/* If after command is given, execute it */
			if (pgkeeper_after_command)
				doAfterCommand();

			return true;
		}
	}

	return false;
}

/*
 * Promote standby server using ordinally way which is used by
 * pg_ctl client tool. Put trigger file into $PGDATA, and send
 * SIGUSR1 signal to standby server.
 */
static void
doPromote(void)
{
	char trigger_filepath[MAXPGPATH];
	FILE *fp;

	/* Create promote file newly */
    snprintf(trigger_filepath, 1000, "%s/promote", DataDir);
	if ((fp = fopen(trigger_filepath, "w")) == NULL)
		ereport(ERROR,
				(errmsg("could not create promote file: \"%s\"", trigger_filepath)));

	if (fclose(fp))
		ereport(ERROR,
				(errmsg("could not close promote file: \"%s\"", trigger_filepath)));

	/* Do promote */
	if (kill(PostmasterPid, SIGUSR1) != 0)
		ereport(ERROR,
				(errmsg("failed to send SIGUSR1 signal to postmaster process : %d",
						PostmasterPid)));
	ereport(LOG,
			(errmsg("pg_keeper promoted standby server to primary server")));
}

/*
 * Attempt to execute an external shell command after promotion.
 */
static void
doAfterCommand(void)
{
	int	rc;

	Assert(pgkeeper_after_command);

	ereport(LOG,
			(errmsg("executing after promoting command \"%s\"",
					pgkeeper_after_command)));

	rc = system(pgkeeper_after_command);

	if (rc != 0)
	{
		ereport(LOG,
				(errmsg("failed to execute after promoting command \"%s\"",
						pgkeeper_after_command)));
	}
}

