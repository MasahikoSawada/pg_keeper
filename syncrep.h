/* -------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * Synchronous replication routines for pg_keeper.
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "replication/syncrep.h"

extern SyncRepConfigData *RepConfig;

extern bool parse_synchronous_standby_names(void);
