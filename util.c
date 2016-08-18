/* -------------------------------------------------------------------------
 *
 * util.c
 *
 * Utility routines for pg_keeper.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "pg_keeper.h"

#include "access/xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "libpq-int.h"
#include "replication/walsender.h"
#include "tcop/utility.h"
#include "utils/snapmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "syncrep.h"
#include "util.h"

/*
 * Exec given SQL and handle the error.
 */
bool
spiSQLExec(const char* sql, bool newtx)
{
	int ret;

	if (newtx)
		START_SPI_TRANSACTION();

	ret = SPI_exec(sql, 0);

	if (ret != SPI_OK_SELECT && ret != SPI_OK_UPDATE &&
		ret != SPI_OK_INSERT && ret != SPI_OK_DELETE)
	{
		ereport(WARNING,
				(errmsg("failed to execute CRUD to fetch node_info table, status %d :\"%s\"",
						ret, sql)));
		return false;
	}

	if (newtx)
		END_SPI_TRANSACTION();

	return true;
}

/*
 * Open given relaltion and return Relation.
 */
Relation
get_rel_from_relname(text *relname, LOCKMODE lockmode, AclMode aclmode)
{
	RangeVar *relvar;
	Relation rel;
	AclResult aclresult;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = heap_openrv(relvar, lockmode);

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  aclmode);

	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));
	return rel;
}

/*
 * Add given new node into the management table. This function must be called
 * within transaction.
 */
void
addNewNode(TupleDesc tupdesc, text *node_name, text *conninfo,
		   bool is_master, bool is_nextmaster, bool is_sync)
{
#define KEEPER_SQL_ADDNODE "INSERT INTO %s (name, conninfo, is_master, is_nextmaster, is_sync) VALUES($1, $2, $3, $4, $5)"

	SPIPlanPtr plan;
	StringInfoData sql;
	Oid argtypes[KEEPER_NUM_ATTS];
	Datum values[KEEPER_NUM_ATTS];
	int ret;

	/* Fill argtypes out by Oids of data type */
	argtypes[0] = SPI_gettypeid(tupdesc, 2);
	argtypes[1] = SPI_gettypeid(tupdesc, 3);
	argtypes[2] = SPI_gettypeid(tupdesc, 4);
	argtypes[3] = SPI_gettypeid(tupdesc, 5);
	argtypes[4] = SPI_gettypeid(tupdesc, 6);

	initStringInfo(&sql);
	appendStringInfo(&sql, KEEPER_SQL_ADDNODE, KEEPER_MANAGE_TABLE_NAME);

	plan = SPI_prepare(sql.data, KEEPER_NUM_ATTS, argtypes);

	values[0] = PointerGetDatum(node_name);
	values[1] = PointerGetDatum(conninfo);
	values[2] = BoolGetDatum(is_master);
	values[3] = BoolGetDatum(is_nextmaster);
	values[4] = BoolGetDatum(is_sync);

	ret = SPI_execp(plan, values, NULL, 1);
}

bool
deleteNodeBySeqno(int seqno)
{
#define KEEPER_SQL_DELETE_BY_SEQNO "DELETE FROM %s WHERE seqno = %d"
	StringInfoData sql;
	bool ret;

	initStringInfo(&sql);
	appendStringInfo(&sql, KEEPER_SQL_DELETE_BY_SEQNO, KEEPER_MANAGE_TABLE_NAME,
					 seqno);
	ret = spiSQLExec(sql.data, false);

	return ret;
}

bool
deleteNodeByName(const char *name)
{
#define KEEPER_SQL_DELETE_BY_NAME "DELETE FROM %s WHERE name = '%s'"
	StringInfoData sql;
	bool ret;

	initStringInfo(&sql);
	appendStringInfo(&sql, KEEPER_SQL_DELETE_BY_NAME, KEEPER_MANAGE_TABLE_NAME,
					 name);
	ret = spiSQLExec(sql.data, false);

	return ret;
}

/* Check if pg_keeper is already installed */
bool
checkExtensionInstalled(void)
{
	bool ret = true;
	Oid extension_oid;

	START_SPI_TRANSACTION();

	extension_oid = get_extension_oid("pg_keeper", true);

	if (extension_oid == InvalidOid)
		ret = false;

	END_SPI_TRANSACTION();

	return ret;
}

/*
 * Get the all node information from management table and return it.
 * The number of tuples is stored into num.
 */
SPITupleTable *
getAllRepNodes(int *num, bool newtx)
{
#define KEEPER_SQL_ASTER "SELECT * FROM %s ORDER BY seqno"
	int ret;
	StringInfoData sql;

	if (newtx)
		START_SPI_TRANSACTION();

	/* We check if pg_keeper is already craated first */
	if (get_extension_oid("pg_keeper", true) != InvalidOid)
	{
		initStringInfo(&sql);
		appendStringInfo(&sql, KEEPER_SQL_ASTER, KEEPER_MANAGE_TABLE_NAME);

		ret = SPI_exec(sql.data, 0);

		if (ret != SPI_OK_SELECT)
			ereport(WARNING,
					(errmsg("failed to execute SELECT to fetch node_info table.")));

		*num = SPI_processed;
	}

	if (newtx)
		END_SPI_TRANSACTION();

	return SPI_tuptable;
}

/* Return the number of connecting standbys */
int
getNumberOfConnectingStandbys(void)
{
#define KEEPER_SQL_STAT_REPLICATION "SELECT * FROM pg_stat_replication"
	int n_standbys = 0;

	START_SPI_TRANSACTION();

	spiSQLExec(KEEPER_SQL_STAT_REPLICATION, false);
	n_standbys = SPI_processed;

	END_SPI_TRANSACTION();

	return n_standbys;
}

/*
 * Update its own local cache information about RepNodes. Note that thi
 * fucntion bein new transaction, so could not be called in transaction.
 * while updating own cache, send SIGUSR1 indirectly to all stnadbys if
 * propagete is true.
 */
void
updateLocalCache(bool propagate)
{
#define KEEPER_SQL_INDIRECT_KILL "SELECT pgkeeper.indirect_kill('SIGUSR1')"
	SPITupleTable *tuptable;
	TupleDesc tupdesc;
	int i;
	int num;
	Relation rel;

	START_SPI_TRANSACTION();

	/* Prepare relation */
	rel = get_rel_from_relname(cstring_to_text(KEEPER_MANAGE_TABLE_NAME), AccessShareLock,
							   ACL_SELECT);
	tupdesc = rel->rd_att;

	/* Get node information from table */
	tuptable = getAllRepNodes(&num, false);

	if (num == 0)
	{
		/* table is empty */
	}

	/* Intialize */
	if (KeeperRepNodes)
		free(KeeperRepNodes);

	KeeperRepNodes = malloc(sizeof(KeeperNode) * num);

	for (i = 0; i < num; i++)
	{
		HeapTuple tuple = tuptable->vals[i];
		bool isNull;

		KeeperRepNodes[i].seqno = SPI_getbinval(tuple, tupdesc, 1, &isNull);
		KeeperRepNodes[i].name = strdup(SPI_getvalue(tuple, tupdesc, 2));
		KeeperRepNodes[i].conninfo = strdup(SPI_getvalue(tuple, tupdesc, 3));
		KeeperRepNodes[i].is_master = SPI_getbinval(tuple, tupdesc, 4, &isNull);
		KeeperRepNodes[i].is_nextmaster = SPI_getbinval(tuple, tupdesc, 5, &isNull);
		KeeperRepNodes[i].is_sync = SPI_getbinval(tuple, tupdesc, 6, &isNull);

		/* Send kill indirectly except for master server */
		if (propagate && !(KeeperRepNodes[i].is_master))
			execSQL(KeeperRepNodes[i].conninfo, KEEPER_SQL_INDIRECT_KILL, NULL);
	}

	nKeeperRepNodes = num;

	relation_close(rel, AccessShareLock);

	END_SPI_TRANSACTION();

	set_ps_display(getStatusPsString(current_status, nKeeperRepNodes), false);
	ereport(LOG, (errmsg("pg_keeper updates own cache, currently number of nodes is %d",
						 nKeeperRepNodes)));
}

/*
 * Pfree and palloc again the given retry_counts.
 */
int *
resetRetryCounts(int *retry_counts)
{
	if (retry_counts)
		pfree(retry_counts);

	retry_counts = palloc0(sizeof(int) * nKeeperRepNodes);

	return retry_counts;
}

/*
 * Return true if give name is regarded as the next master.
 */
bool
isNextMaster(const char *name)
{
	int i;
	bool ret = false;

	for (i = 0; i < nKeeperRepNodes; i++)
	{
		KeeperNode *node = &(KeeperRepNodes[i]);

		if (node->is_nextmaster &&
			pg_strcasecmp(name, node->name) == 0)
		{
			ret = true;
			break;
		}
	}

	/* Return true if there is any result by SQL */
	return ret;
}

/* Convert boolean string to bool value */
bool
str_to_bool(const char *string)
{
	if (pg_strcasecmp(string, "true") == 0 ||
		pg_strcasecmp(string, "on") == 0 ||
		pg_strcasecmp(string, "1") == 0 ||
		pg_strcasecmp(string, "t") == 0)
		return true;
	else
		return false;
}

/*
 * It's assumed that parse_synrep_names is already called
 *
 *
 *
 * Update is_sync, is_nextmaster
 */
/*
 * Determine the next master in case of failover and set the flag to a node. The next
 * master will selected using following priorities.
 * 1. The standby listed in synchronous_standby_names. Left is higher priority meaning
 * that the standby having higher sync_priority on pg_stat_replication will be selected.
 * 2. The standby listed on top of the management table meaning that if there are no sync
 * connecting standby, we select the fixed node which is listed on top of the management
 * table.
 */
bool
updateManageTableAccordingToSSNames(bool newtx)
{
#define KEEPER_SQL_ALL_FALSE "UPDATE %s SET is_nextmaster = false, is_sync = false"
#define KEEPER_SQL_SET_NEXT_MASTER "UPDATE %s SET is_nextmaster = true WHERE seqno = %d"
#define KEEPER_SQL_SET_SYNC_STANDBY "UPDATE %s SET is_sync = true WHERE seqno in (%s)"
#define KEEPER_SQL_ALL_STANDBY_NODES "SELECT seqno, name FROM %s WHERE NOT is_master ORDER BY seqno"

	SPITupleTable *tuptable;
	int num;
	int i, i_tup;
	int reserve_seqno = -1;
	char *standby_name;
	int *sync_standbys;
	int n_sync_standbys = 0;
	int next_master_seqno = 0;
	bool got_next_master_seqno = false;
	StringInfoData sql;
	TupleDesc tupdesc;
	Relation rel;

	if (newtx)
		START_SPI_TRANSACTION();

	initStringInfo(&sql);
	rel = get_rel_from_relname(cstring_to_text(KEEPER_MANAGE_TABLE_NAME), AccessShareLock,
							   ACL_SELECT);
	tupdesc = rel->rd_att;

	/* Set false to is_sync and is_nextmaster first */
	appendStringInfo(&sql, KEEPER_SQL_ALL_FALSE, KEEPER_MANAGE_TABLE_NAME);
	spiSQLExec(sql.data, false);
	resetStringInfo(&sql);

	/* Get all standby nodes information from management table */
	appendStringInfo(&sql, KEEPER_SQL_ALL_STANDBY_NODES, KEEPER_MANAGE_TABLE_NAME);
	spiSQLExec(sql.data, false);
	resetStringInfo(&sql);
	num = SPI_processed;

	/*
	 * 2. Set is_nextmaster = true to a appropriate node
	 * We get the standby bein considered as a next master with following priority.
	 *   1. The stnadby listed in synchronous_standby_names. Left is higher priority.
	 *   2. The stnadby listed on top of the management table.
	 */
	tuptable = SPI_tuptable;
	standby_name = RepConfig->member_names;
	sync_standbys = (int *) palloc(sizeof(int) * num);
	for (i = 0; i < RepConfig->nmembers; i++)
	{
		for (i_tup = 0; i_tup < num; i_tup++)
		{
			HeapTuple tuple = tuptable->vals[i_tup];
			bool isNull;

			/* Reserve node having lowest seqno */
			if (reserve_seqno == -1)
				reserve_seqno = SPI_getbinval(tuple, tupdesc, 1, &isNull);

			/* Found next new master server */
			if (pg_strcasecmp(standby_name, SPI_getvalue(tuple, tupdesc, 2)) == 0)
			{
				int seqno =  SPI_getbinval(tuple, tupdesc, 1, &isNull);

				/* Remenger the seqno of nextd master standby */
				if (!got_next_master_seqno)
				{
					next_master_seqno = seqno;
					got_next_master_seqno = true;
				}

				/* Remenber the seqno of sync standby */
				sync_standbys[n_sync_standbys++] = seqno;
			}
		}

		standby_name += strlen(standby_name) + 1;
	}

	/*
	 * If we could not find the next master connecting sync, we select th
	 * top row
	 */
	if (!got_next_master_seqno)
		next_master_seqno = reserve_seqno;

	ereport(DEBUG1, (errmsg("udpate is_nextmaster column of \"%d\" row",
							next_master_seqno)));

	/* Update is_nextmaster column */
	appendStringInfo(&sql, KEEPER_SQL_SET_NEXT_MASTER,
					 KEEPER_MANAGE_TABLE_NAME, next_master_seqno);
	spiSQLExec(sql.data, false);
	resetStringInfo(&sql);

	/*
	 * Update sync standbys information.
	 */
	if (n_sync_standbys > 0)
	{
		StringInfoData buf;
		char delimiter = ' ';

		initStringInfo(&buf);
		for (i = 0; i < n_sync_standbys; i++)
		{
			appendStringInfo(&buf, "%c%d", delimiter, sync_standbys[i]);
			delimiter = ',';
		}

		/* Update is_sync column */
		appendStringInfo(&sql, KEEPER_SQL_SET_SYNC_STANDBY, KEEPER_MANAGE_TABLE_NAME,
						 buf.data);
		spiSQLExec(sql.data, false);

		ereport(DEBUG1, (errmsg("update is_sync columns of \"%s\" seqno row", buf.data)));
	}

	relation_close(rel, AccessShareLock);

	if (newtx)
		END_SPI_TRANSACTION();

	return true;
}
