/* -------------------------------------------------------------------------
 *
 * util.h
 *
 * Header file for util.c
 *
 * -------------------------------------------------------------------------
 */

/* These are always necessary for a bgworker */
#include "utils/builtins.h"
#include "access/htup_details.h"
#include "tcop/utility.h"
#include "libpq-int.h"
#include "executor/spi.h"
#include "utils/builtins.h"

#define BUFSIZE 8192

/* Macro for SPI start or end transaction */
#define START_SPI_TRANSACTION() \
	{ \
		SetCurrentStatementStartTimestamp(); \
		StartTransactionCommand(); \
		SPI_connect(); \
		PushActiveSnapshot(GetTransactionSnapshot()); \
	} while(0)
#define END_SPI_TRANSACTION() \
	{ \
		SPI_finish(); \
		PopActiveSnapshot(); \
		CommitTransactionCommand(); \
	} while(0)

/* Function prototypes */
extern Relation get_rel_from_relname(text *relname, LOCKMODE lockmode,
									 AclMode aclmode);
extern void addNewNode(TupleDesc tupdesc, text *node_name, text *conninfo,
					   bool is_master, bool is_nextmaster, bool is_sync);
extern bool deleteNodeBySeqno(int seqno);
extern bool deleteNodeByName(const char *name);
extern int decideNextMaster(TupleDesc tupdesc, SPITupleTable tuptable);
extern SPITupleTable *getAllRepNodes(int *num, bool newtx);
extern bool spiSQLExec(const char *sql, bool newtx);
extern void updateNextMaster(TupleDesc tupdesc);
extern void updateLocalCache(bool propagate);
extern int *resetRetryCounts(int *retry_counts);
extern bool isNextMaster(const char *name);
extern bool str_to_bool(const char *string);
extern bool checkExtensionInstalled(void);
extern bool updateManageTableAccordingToSSNames(bool newtx);
extern int getNumberOfConnectingStandbys(void);
