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
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will change replication mode to asynchronous replication so that backend process can avoid to wait infinity.

- standby mode

standby mode of pg_keeper queries the primary server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will promote the standby it runs on to master.
After promoting to master server, pg_keeper switches from standby mode to master mode automatically.

With this, fail over time can be calculated with this formula.

```
(F/O time) = pg_keeper.keepalives_time * pg_keeper.keepalives_count
```

## GUC parameters
Note that the parameters with (*) are mandatory options.

- pg_keeper.partner_conninfo(*)

  - Specifies a connection string to be used for heart-beat to the partner node.
  - The heart-beat LAN is better to be separated from replication LAN.
  - Also, the NIC for heart-beat LAN is better to use NIC bonding.

- pg_keeper.my_conninfo(*)

  - Specifies a connection string to be used for pg_keeper to do `ALTER SYSTEM` on myself.
  
- pg_keeper.keepalive_time (sec)

  - Specifies how long interval pg_keeper continues polling. 5 seconds by default.

- pg_keeper.keepalive_count

  - Specifies how many times pg_keeper try polling to master server in order to promote standby server. 4 times by default.

- pg_keeper.after_command

  - Specifies shell command that will be called after promoted. Setting stonith command to this parameter is useful for preventing the split-brain syndrome.

## Tested platforms
pg_keeper has been built and tested on following platforms:

| Category | Module Name |
|:--------:|:-----------:|
|OS|CentOS 6.5|
|PostgreSQL|9.5, 9.6|

pg_keeper probably can work with PostgreSQL 9.3 or later, but not tested yet.

Reporting of building or testing pg_keeper on some platforms are very welcome.

## How to set up pg_keeper

### Installation
pg_keeper needs to be installed into both master server and standby server.

```console
$ cd pg_keeper
$ make USE_PGXS=1
$ su
# make USE_PGXS=1 install
```

### Configuration
For example, we set up two servers; pgserver1 and pgserver2. pgserver1 is the first master server and pgserver2 is the first standby server. After created user for replication and set up authentication, we need to install pg_keeper in both servers and configure some parameters as follows.

```console
-- on pgserver1 (first master server)
$ vi $PGDATA/postgresql.conf
wal_level = hot_standby
max_worker_processes = 8 # pg_keeper requires one worker on each side
max_wal_senders = 8 # must be more than 1
hot_standby = on
shared_preload_libraries = 'pg_keeper'
synchronous_standby_names = 'pgserver2' # must use set sync replication mode.
pg_keeper.keepalive_time = 5
pg_keeper.keepalive_count = 3
pg_keeper.my_conninfo = 'host=127.0.0.1 port=5432 dbname=postgres'
pg_keeper.partner_conninfo = 'host=pgserver2 port=5432 dbname=postgres'
```

```console
-- on pgserver2 (first slave server)
$ vi $PGDATA/postgresql.conf
wal_level = hot_standby
max_worker_processes = 8 # pg_keeper requires one worker on each side
max_wal_senders = 8 # must be more than 1
hot_standby = on
shared_preload_libraries = 'pg_keeper'
pg_keeper.keepalive_time = 5
pg_keeper.keepalive_count = 3
pg_keeper.my_conninfo = 'host=127.0.0.1 port=5432 dbname=postgres'
pg_keeper.partner_conninfo = 'host=pgserver1 port=5432 dbname=postgres'
$ vi $PGDATA/recovery.conf
standby_mode = 'on'
recovery_target_timeline = latest
primary_conninfo = 'host=pgserver1 port=5432 user=repl_user application_name=pgserver2'
```

### Starting servers
We should start master server first that pg_keeper is installed in. master server's pg_keeper process will be launched when master server got started, once pg_keeper in standby server connected master's pg_keeper process it will start to work.

After started both master server and slave server, make sure that pg_keeper process is launched successfully in appropriate state on both server.

```console
-- On master server
$ tail master.log
LOG:  pg_keeper connects to standby server
$ ps x | grep pg_keeper | grep -v grep
33525 ?        Ss     0:00 postgres: bgworker: pg_keeper   (master mode:connected)
```

```console
-- On standby server
$ ps x | grep pg_keeper | grep -v grep
33613 ?        Ss     0:00 postgres: bgworker: pg_keeper   (standby mode:connected)
```

For more detail of state transition of pg_keeper, please refer [State Transition of pg_keeper](#state_transition) section.

### Handling standby server failure (Automated changing sync replication to async replication)
In case the synchronous standby server crashes, because the master server cannnot replicate data to synchronous standby server the following transaction can not be processed. In this case, pg_keeper on the master server changes synchronous replication to asynchronous replication by changing `synchronous_standby_names` GUC parameter after detected the standby server failure if synchronous replication is enabled.  You can see following server log on the master server.

```console
$ cat master.log
<2016-07-20 09:10:09.855 AST> LOG:  could not get tuple from server : "host=pgserver2 port=5432 dbname=postgres"
<2016-07-20 09:10:09.855 AST> LOG:  pg_keeper failed to connect 1 time(s)
<2016-07-20 09:10:14.859 AST> LOG:  could not get tuple from server : "host=pgserver2 port=5432 dbname=postgres"
<2016-07-20 09:10:14.859 AST> LOG:  pg_keeper failed to connect 2 time(s)
<2016-07-20 09:10:24.867 AST> LOG:  pg_keeper changes replication mode to asynchronous replication
<2016-07-20 09:10:24.884 AST> LOG:  received SIGHUP, reloading configuration files
<2016-07-20 09:10:24.885 AST> LOG:  parameter "synchronous_standby_names" changed to ""
```

After the standby server recovered, you need to set `synchronous_standby_names` parameter on the primary server manually in order to set up streaming replication again.

### Handling master server failure (Automated failover)
In case the master server crashes, the standby server needs to promote to new master server. pg_keeper on the standby server promote it after detecting the master server failure. You can see following server log on the standby server.

```console
$ tail standby.log
<2016-07-20 09:14:30.622 AST>LOG:  could not get tuple from server : "host=pgserver1 port=5432 dbname=postgres"
<2016-07-20 09:14:30.622 AST>LOG:  pg_keeper failed to connect 1 time(s)
<2016-07-20 09:14:35.628 AST>LOG:  could not get tuple from server : "host=pgserver1 port=5432 dbname=postgres"
<2016-07-20 09:14:35.628 AST>LOG:  pg_keeper failed to connect 2 time(s)
<2016-07-20 09:14:45.641 AST>LOG:  pg_keeper promoted standby server to primary server
<2016-07-20 09:14:45.641 AST>LOG:  received promote request
<2016-07-20 09:14:45.641 AST>LOG:  redo done at 0/3000060
<2016-07-20 09:14:45.646 AST>LOG:  selected new timeline ID: 2
<2016-07-20 09:14:45.690 AST>LOG:  archive recovery complete
<2016-07-20 09:14:45.692 AST>LOG:  MultiXact member wraparound protections are now enabled
<2016-07-20 09:14:45.693 AST>LOG:  database system is ready to accept connections
```

## <a name="state_transition"> State Transition
|state|description|
|:---:|:---------:|
|**(standby:ready)**|Waiting for being able to connect to the master server.|
|**(standby:connected)**|Connected to the master server. Heartbeating.|
|**(master:ready)**|Wait for replication connection from standby server.|
|**(master:connected)**|Connected from the standby server. Heartbeating.|
|**(master:async)**|The master server is running as async replication mode.|

## Uninstallation
+ Following commands need to be executed in both master server and standby server.

```console
$ cd pg_keeper
$ make USE_PGXS=1
$ su
# make USE_PGXS=1 uninstall
```

+ Remove `pg_keeper` from shared_preload_libraries in postgresql.conf on both servers.

```console
$ vi postgresql.conf
shared_preload_libraries = ''
```
