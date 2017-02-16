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
char	*keeper_node2_conninfo;
char	*keeper_after_command;

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
	if(!(con = PQconnectdb(KeeperMaster)))
		ereport(ERROR,
				(errmsg("could not establish connection to primary server : %s",
						KeeperMaster)));

	PQfinish(con);

	/* Set process display which is exposed by ps command */
	current_status = KEEPER_STANDBY_CONNECTED;
	set_ps_display(getStatusPsString(current_status), false);

	return;
}

/*
 * Main routine for standby mode.
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
		 * Pooling to master server. If heartbeat is failed,
		 * increment retry_count..
		 */
		if (!heartbeatServer(KeeperMaster, retry_count))
			retry_count++;
		else
			retry_count = 0; /* reset count */

		/*
		 * If retry_count is reached to keeper_keepalives_count,
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

	elog(LOG,"promote standby server to primary server");

	/* Do promote */
	if (kill(PostmasterPid, SIGUSR1) != 0)
		ereport(ERROR,
				(errmsg("failed to send SIGUSR1 signal to postmaster process : %d",
						PostmasterPid)));
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

