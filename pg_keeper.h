/* -------------------------------------------------------------------------
 *
 * pg_keeper.h
 *
 * Header file for pg_keeper.
 *
 * -------------------------------------------------------------------------
 */

/* These are always necessary for a bgworker */
#include "access/xlog.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

#include "tcop/utility.h"
#include "libpq-int.h"

typedef enum KeeperStatus
{
	KEEPER_STANDBY_READY = 0,
	KEEPER_STANDBY_CONNECTED,
	KEEPER_STANDBY_ALONE,
	KEEPER_MASTER_READY,
	KEEPER_MASTER_CONNECTED,
	KEEPER_MASTER_ASYNC
} KeeperStatus;

typedef struct KeeperNode
{
	char *conninfo;
	bool is_master;
	bool is_next_master;
	bool is_sync;
} KeeperNode;

typedef struct KeeperShmem
{
	KeeperStatus current_status;
	slock_t		mutex;	/* mutex for editing data on shmem */
	bool		sync_mode;	/* we are using synchronous replication? */
} KeeperShmem;

/* pg_keeper.c */
extern void	_PG_init(void);
extern void _PG_fini(void);
extern void	KeeperMain(Datum);
extern bool	heartbeatServer(const char *conninfo, int r_count);
extern bool execSQL(const char *conninfo, const char *sql);
extern char *KeeperMaster;
extern char *KeeperStandby;
extern KeeperShmem	*keeperShmem;
sig_atomic_t got_sighup;
sig_atomic_t got_sigterm;

extern void updateStatus(KeeperStatus status);

/* master.c */
extern bool KeeperMainMaster(void);
extern void setupKeeperMaster(void);

/* standby.c */
extern bool	KeeperMainStandby(void);
extern void setupKeeperStandby(void);

/* GUC variables */
extern int	pgkeeper_keepalives_time;
extern int	pgkeeper_keepalives_count;
extern char *pgkeeper_partner_conninfo;
extern char *pgkeeper_my_conninfo;
extern char *pgkeeper_after_command;
