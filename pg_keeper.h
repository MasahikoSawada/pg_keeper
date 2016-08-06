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

#define KEEPER_MANAGE_TABLE_NAME "pgkeeper.node_info"
#define KEEPER_NUM_ATTS 5 /* Except for seqno */
#define HEARTBEAT_SQL "SELECT 1"

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
	int	seqno;
	char *name;
	char *conninfo;
	bool is_master;
	bool is_next_master;
	bool is_sync;
} KeeperNode;

/* pg_keeper.c */
extern void	_PG_init(void);
extern void	KeeperMain(Datum);
extern bool	heartbeatServer(const char *conninfo, int r_count);
extern bool execSQL(const char *conninfo, const char *sql);
extern char *KeeperMaster;
extern char *KeeperStandby;
extern sig_atomic_t got_sighup;
extern sig_atomic_t got_sigterm;
extern sig_atomic_t got_sigusr1;

extern char *getStatusPsString(KeeperStatus status);

/* master.c */
extern bool KeeperMainMaster(void);
extern void setupKeeperMaster(void);

/* standby.c */
extern bool	KeeperMainStandby(void);
extern void setupKeeperStandby(void);

/* GUC variables */
extern int	keeper_keepalives_time;
extern int	keeper_keepalives_count;
extern char *keeper_node1_conninfo;
extern char	*keeper_node2_conninfo;
extern char *keeper_after_command;
extern char *keeper_node_name;

/* Variables for cluster management */
extern KeeperStatus	current_status;
extern KeeperNode *KeeperRepNodes;
extern int nKeeperRepNodes;
