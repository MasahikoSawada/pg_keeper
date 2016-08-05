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

extern Relation get_rel_from_relname(text *relname, LOCKMODE lockmode,
									 AclMode aclmode);
extern void addNewNode(TupleDesc tupdesc, text *node_name, text *conninfo,
					   bool is_master, bool is_nextmaster, bool is_sync);
extern int decideNextMaster(TupleDesc tupdesc, SPITupleTable tuptable);
extern SPITupleTable *getAllRepNodes(int *num, bool newtx);
extern void spiSQLExec(const char *sql);
extern void updateNextMaster(TupleDesc tupdesc);
extern void updateLocalCache(void);
