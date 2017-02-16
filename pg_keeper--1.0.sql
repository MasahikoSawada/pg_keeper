/* pg_keeper/pg_keepe--2.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_keeper" to load this file. \quit

-- Create pg_keeper schema
CREATE SCHEMA pgkeeper;

-- Register ndoe management table
CREATE TABLE pgkeeper.node_info(
name            text primary key,
conninfo        text
);

CREATE FUNCTION pgkeeper.add_node(node_name text, conninfo text)
RETURNS bool
AS 'MODULE_PATHNAME', 'add_node'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pgkeeper.del_node(node_name text)
RETURNS bool
AS 'MODULE_PATHNAME', 'del_node'
LANGUAGE C STRICT PARALLEL UNSAFE;
