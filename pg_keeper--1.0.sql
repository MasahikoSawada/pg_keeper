/* pg_keeper/pg_keepe--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_keeper" to load this file. \quit

-- Create pg_keeper schema
CREATE SCHEMA pgkeeper;

-- Register ndoe management table
CREATE TABLE pgkeeper.node_info(
seqno		serial,
name		text,
conninfo	text,
is_master	bool,
is_nextmaster	bool,
is_sync		bool
);

-- Register node management functions
CREATE FUNCTION pgkeeper.add_node(
node_name text,
conninfo text
)
RETURNS bool
AS 'MODULE_PATHNAME', 'add_node'
LANGUAGE C STRICT
PARALLEL UNSAFE;

CREATE FUNCTION pgkeeper.del_node(
node_name text
)
RETURNS bool
AS 'MODULE_PATHNAME', 'del_node'
LANGUAGE C STRICT
PARALLEL UNSAFE;

CREATE FUNCTION pgkeeper.del_node(
seqno	integer
)
RETURNS bool
AS 'MODULE_PATHNAME', 'del_node_by_seqno'
LANGUAGE C STRICT
PARALLEL UNSAFE;

CREATE FUNCTION pgkeeper.indirect_polling(
conninfo text
)
RETURNS BOOL
AS 'MODULE_PATHNAME', 'indirect_polling'
LANGUAGE C STRICT
PARALLEL UNSAFE;

CREATE FUNCTION pgkeeper.indirect_kill(
signal text
)
RETURNS BOOL
AS 'MODULE_PATHNAME', 'indirect_kill'
LANGUAGE C STRICT
PARALLEL UNSAFE;
