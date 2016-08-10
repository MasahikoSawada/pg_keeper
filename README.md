pg_keeper 1.0
===========

pg_keeper is a simplified clustering module for PostgreSQL, to promote a standby server to master in a 2 servers cluster.

The license of pg_keeper is [PostgreSQL License](https://opensource.org/licenses/postgresql). (same as BSD License)

## Prerequisite
pg_keeper requires a master and hot standby servers in PostgreSQL 9.3 or later, on a Linux OS.
pg_keeper requires to have already the replication in place.

## Overview
pg_keeper runs on both master and standby server as two modes; master mode and standby mode.
The pg_keeper mode is determined automatially by itself.

- master mode

master mode of pg_keeper queries the standby server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will change replication mode to asynchronous replcation so that backend process can avoid to wait infinity.

- standby mode

standby mode of pg_keeper queries the primary server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will promote the standby it runs on to mastet.
After promoting to master server, pg_keeper switches from standby mode to master mode automatically.

With this, fail over time can be calculated with this formula.

```
(F/O time) = pg_keeper.keepalives_time * pg_keeper.keepalives_count
```

## GUC paramters
Note that the paramteres with (*) are mandatory options.

- pg_keeper.node1_conninfo(*)

  - Specifies a connection string to be used for pg_keeper to connect to the first master - which is used by standby mode server.
  - It should be the same as the primary_conninfo in recovery.conf on first standby server.

- pg_keeper.node2_conninfo(*)

  - Specifies a connection string to be used for pg_keeper to connect to the first standby- which is used by master mode server.

- pg_keeper.keepalive_time (sec)

  - Specifies how long interval pg_keeper continues polling. 5 second by default.

- pg_keeper.keepalive_count

  - Specifies how many times pg_keeper try polling to master server in ordre to promote standby server. 1 time by default.

- pg_keeper.after_command

  - Specifies shell command that will be called after promoted.

## Tested platforms
pg_keeper has been built and tested on following platforms:

| Category | Module Name |
|:--------:|:-----------:|
|OS|CentOS 6.5|
|PostgreSQL|9.5, 9.6beta2|

pg_keeper probably can work with PostgreSQL 9.3 or later, but not tested yet.

Reporting of building or testing pg_keeper on some platforms are very welcome.

## How to set up pg_keeper

### Installation
pg_keeper needs to be installed into both master server and standby server.

```
$ cd pg_keeper
$ make USE_PGXS=1
$ su
# make USE_PGXS=1 install
```

### Configration
For example, we set up two servers; pgserver1 and pgserver2. pgserver1 is the first master server and pgserver2 is the first standby server. We need to install pg_keeper in both servers and configure some parameters as follows.

```
$ vi postgresql.conf
shared_preload_libraries = 'pg_keeper'
pg_keeper.keepalive_time = 5
pg_keeper.keepalive_count = 3
pg_keeper.node1_conninfo = 'host=pgserver1 port=5432 dbname=postgres'
pg_keeper.node2_conninfo = 'host=pgserver2 port=5432 dbname=postgres'
```
### Starting servers
We should start master server first that pg_keeper is installed in. master server's pg_keeper process will be launched when master server got started, once pg_keeper in standby server connected master's pg_keeper process it will start to work.

### Uninstallation
+ Following commands need to be executed in both master server and standby server.

```
$ cd pg_keeper
$ make USE_PGXS=1
$ su
# make USE_PGXS=1 uninstall
```

+ Remove `pg_keeper` from shared_preload_libraries in postgresql.conf on both servers.

```
$ vi postgresql.conf
shared_preload_libraries = ''
```
