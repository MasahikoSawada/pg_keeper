pg_keeper
===========

pg_keeper is simplified clustring module for PostgreSQL.

## Overview
pg_keeper is available only on standby server side.

pg_keeper polls to primary server every pg_keeper.keepalive_time
second using simple query 'SELECT 1'.
If pg_keeper failed to poll at pg_keeper.keepalive_count time(s),
pg_keeper will promote the standby server to master server, and then
exit itself.
That is, fail over time can be calculated with this formula.

(F/O time) = pg_keeper.keepalives_time * pg_keeper.keepalives_count

## Paramters
- pg_keeper.primary_conninfo

Specifies a connection string to be used for pg_keeper to connect to master server, which is same as master server specified in recovery.conf.

- pg_keeper.keepalive_time (sec)

Specifies how long interval pg_keeper continues polling.
Deafult value is 5 secound.

- pg_keeper.keepalive_count

Specifies how many times pg_keeper try polling to master server in ordre to promote standby server.
Default value is 1 times.

- pg_keeper.after_command

Specifies shell command that will be called after promoted.

## Supporting platforms

pg_keeper requires PostgreSQL 9.3 or later.

## How to install pg_keeper

```
$ cd pg_keeper
$ make USE_PGXS=1
$ su
# make USE_PGXS=1 install
```

## How to set up pg_keeper

```
$ vi postgresql.conf
shared_preload_libraries = 'pg_keeper'
pg_keeper.keepalive_time = 5
pg_keeper.keepalive_count = 3
pg_keeper.primary_conninfo = 'host=192.168.100.100 port=5432 dbname=postgres'
```
