/* -------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * Synchronous replication routines for pg_keeper.
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "miscadmin.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/ps_status.h"

#include "syncrep.h"

SyncRepConfigData *RepConfig = NULL;

bool parse_synchronous_standby_names(void)
{
	int parse_rc;

	syncrep_parse_result = NULL;
	syncrep_parse_error_msg = NULL;

	syncrep_scanner_init(SyncRepStandbyNames);
	parse_rc = syncrep_yyparse();
	syncrep_scanner_finish();

	if (parse_rc != 0 || syncrep_parse_result == NULL)
	{
		GUC_check_errcode(ERRCODE_SYNTAX_ERROR);
		if (syncrep_parse_error_msg)
			GUC_check_errdetail("%s", syncrep_parse_error_msg);
		else
			GUC_check_errdetail("synchronous_standby_names parser failed");
		return false;
	}

	/* GUC extra value must be malloc'd, not palloc'd */
	RepConfig = (SyncRepConfigData *)
		malloc(syncrep_parse_result->config_size);
	if (RepConfig == NULL)
		return false;
	memcpy(RepConfig, syncrep_parse_result, syncrep_parse_result->config_size);

	return true;
}
