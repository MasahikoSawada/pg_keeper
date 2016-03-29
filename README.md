pg_keeper
===========

pg_keeper is a simplified clustering module for PostgreSQL, to promote a standby server to master in a 2 servers cluster.

## Prerequisite
pg_keeper requires a master and hot standby servers in PostgreSQL 9.3 or later, on a Linux OS.
pg_keeper requires to have already the replication in place.

## Overview
pg_keeper runs both master and standby server as two modes; master mode and standby mode.

- master mode

master mode of pg_keeper queries the standby server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will change replication mode to asynchronous replcation so that backend process can avoid to wait infinity.

- standby mode

standby mode of pg_keeper queries the primary server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will promote the standby it runs on to master, and then exits itself.

With this, fail over time can be calculated with this formula.

```
(F/O time) = pg_keeper.keepalives_time * pg_keeper.keepalives_count
```

## Paramters
- pg_keeper.primary_conninfo(*)

Specifies a connection string to be used for pg_keeper to connect to the master - which should be the same as the master server specified in recovery.conf.

- pg_keeper.slave_conninfo(*)
Specifies a connection string to be used for pg_keeper to connect to the standby.

- pg_keeper.keepalive_time (sec)

Specifies how long interval pg_keeper continues polling.
Deafult value is 5 secound.

- pg_keeper.keepalive_count

Specifies how many times pg_keeper try polling to master server in ordre to promote standby server.
Default value is 1 times.

- pg_keeper.after_command

Specifies shell command that will be called after promoted.

Note that the paramteres with '*' are mandatory options.

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
pg_keeper.standby_conninfo = 'host=192.168.100.200 port=5342 dbname=postgres'
```
