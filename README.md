pg_keeper
===========

pg_keeper is a simplified failover module for PostgreSQL, to promote a standby server to master in a 2 servers cluster.

## Prerequisite
pg_keeper requires a master and hot standby servers in PostgreSQL 9.3 or later, on a Linux OS.
pg_keeper requires to have already the replication in place.

## Overview
pg_keeper runs only on a standby server. No module is available for the master server.
pg_keeper queries the primary server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will promote the standby it runs on to master, and then
exits itself.

With this, fail over time can be calculated with this formula.
(F/O time) = pg_keeper.keepalives_time * pg_keeper.keepalives_count


## Paramters
- pg_keeper.primary_conninfo

Specifies a connection string to be used for pg_keeper to connect to the master - which should be the same as the master server specified in recovery.conf.

- pg_keeper.keepalive_time (sec)

Specifies how long interval pg_keeper continues polling.
Deafult value is 5 secound.

- pg_keeper.keepalive_count

Specifies how many times pg_keeper try polling to master server in ordre to promote
standby server.
Default value is 1 times.


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
