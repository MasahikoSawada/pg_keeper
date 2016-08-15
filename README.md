pg_keeper 2.0
===========

pg_keeper is a simplified clustering module for PostgreSQL, to promote a standby server to master in a 2 servers cluster.

The license of pg_keeper is [PostgreSQL License](https://opensource.org/licenses/postgresql). (same as BSD License)

## Prerequisite
pg_keeper requires a master and hot standby servers in PostgreSQL 9.3 or later, on a Linux OS.
pg_keeper requires to have already the replication in place.

## Concept
There are some HA modules that support PostgreSQL. These allows us to set up flexibly, on the other hand require a lot of setting items. In many cases we are satisfied with clustering having minimum functionality such as polling to each other and doing failover in case of failure.

pg_keeper provides simple and minimum clustering functionality. It would be not enough for some users but you can easily set it up and manage it. For instance, we can use pg_keeper by setting a few GUC parameter. Since pg_keeper works as a bgworker process we don't need to monitor HA process.

## Overview
pg_keeper can work among the master server and more than one standby server using streaming replication.
pg_keeper runs on both master and standby server as two modes; master mode and standby mode.
The pg_keeper mode is determined automatially by itself.

- master mode

master mode of pg_keeper queries all standby servers at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get enough result to contiue synchronous replication after a certain number of tries, pg_keeper will change replication mode to asynchronous replcation so that backend process can avoid to wait infinity.

- standby mode

standby mode of pg_keeper queries the primary server at fixed intervals using a simple query 'SELECT 1'.
If pg_keeper fails to get any result after a certain number of tries, pg_keeper will promote the standby it runs on to mastet.
After promoting to master server, pg_keeper switches from standby mode to master mode automatically.

With this, fail over time can be calculated with this formula.

```
(F/O time) = pg_keeper.keepalives_time * pg_keeper.keepalives_count
```

## Requirement
There are following requirement to use pg_keeper.

+ pg_keeper.node_name has to be unique and same as application_name which will be used for streaming replication.
	+ That is, the application_name used for streaming replication should be unique.
+ `hot_standby` has to be enable on all servers.
+ `max_worker_processes` should be > 1.
+ `*` is not allowed to set to `synchronous_standby_names`.

## GUC paramters
Note that the paramteres with (*) are mandatory options.

### pg_keeper.node_name(*)
Specifies node name string to be used for cluster management. This values have to be unique and same as application name spcified in recovery.conf.

### pg_keeper.keepalive_time (sec)
Specifies how long interval pg_keeper continues polling. 5 second by default.

### pg_keeper.keepalive_count
Specifies how many times pg_keeper try polling to master server in ordre to promote standby server. 1 time by default.

### pg_keeper.after_command
Specifies shell command that will be called after promoted.

## Magagement Table (pgkeeper.node_info)
pg_keeper manages the all nodes on pgkeeper.node_info table. It's not allowed to modify this table directly. If you want to add new node or delete node then you can use provied pg_keeper's function.

|Column|Description|
|:----:|:---------:|
|seqno|Sequential number for each node (1..n)|
|name| Node name|
|conninfo|Connection info used fo hearbeat|
|is_master|True if the master server|
|is_nextmaser|True if the next master server after fail over|
|is_sync|True if the node is connecting as a synchronous standby|

## Functions

### pgkeeper.add_node(node_name text, conninfo text)
Register new active node to cluster management. Return true if registering node is successfuly done.

## pgkeeper.del_node(node_name text)
Remove node by node name. Return true if removing node is successfuly done.

## pgkeeper.del_node(seqno text)
Remove node by seqno. Return true if removing node is successfuly done.

## pgkeeper.indirect_polling(conninfo text)
Poll to given node, which is used for heartbeat to master server. Return true if the connection between executing node and given node is available.

## pgkeeper.indirect_kill(signal text)
Send given signal to pg_keeper process on executed server, which is used for propagation of the modify. `SIGUSR1` and `SIGUSR2` are available.

## Tested platforms
pg_keeper has been built and tested on following platforms:

| Category | Module Name |
|:--------:|:-----------:|
|OS|CentOS 6.5|
|PostgreSQL|9.5, 9.6beta3|

pg_keeper probably can work with PostgreSQL 9.3 or later, but not tested yet.

Reporting of building or testing pg_keeper on some platforms are very welcome.

## How to set up pg_keeper

### Installation
pg_keeper needs to be installed into both master server and standby servers.

```
$ cd pg_keeper
$ make USE_PGXS=1
$ su
# make USE_PGXS=1 install
```

### Configration
For example, we set up two servers; pgserver1 and pgserver2. pgserver1 is the first master server and pgserver2 is the first standby server. We need to install pg_keeper in both servers and configure some parameters as follows.

- On first master server
```
$ vi postgresql.conf
shared_preload_libraries = 'pg_keeper'
pg_keeper.node_name = 'pgserver1'
pg_keeper.keepalive_time = 5
pg_keeper.keepalive_count = 3
```

- On first standby server(s)
```
$ vi postgresql.conf
shared_preload_libraries = 'pg_keeper'
pg_keeper.node_name = 'pgserver2'
pg_keeper.keepalive_time = 5
pg_keeper.keepalive_count = 3
```

### Starting servers
We should start master server first that pg_keeper is installed. The master server's pg_keeper process will be launched when master server got started.
Once pg_keeper on standby server connected master's pg_keeper process it will start to monitoring.

### Node Registration
**The master server have to be added at first**. (Because pg_keeper ragard the first registerd server as a maseter server)

```
=# SELECT pgkeeper.add_node('pgserver1', 'host=pgserver1 port=5432 dbname=postgres');
 add_node
 ----------
  t
  (1 row)
=# SELECT * FROM pgkeeper.node_info;
 seqno |  name  |                  conninfo                | is_master | is_nextmaster | is_sync
-------+--------+------------------------------------------+-----------+---------------+---------
     1 | master | host=pgserver1 port=5432 dbname=postgres | t         | f             | f
```

After registered the master server, register the standby server(s).

```
=# SELECT pgkeeper.add_node('pgserver2', 'host=pgserver2 port=5432 dbname=postgres');
 add_node
 ----------
  t
  (1 row)
=# SELECT * FROM pgkeeper.node_info;
 seqno |    name   |                  conninfo                | is_master | is_nextmaster | is_sync
-------+-----------+------------------------------------------+-----------+---------------+---------
     1 | pgserver1 | host=pgserver1 port=5432 dbname=postgres | t         | f             | f
     2 | pgserver2 | host=pgserver1 port=5432 dbname=postgres | f         | t             | t
```

After registered any standby server, the master server being to poll to all standby servers.

### Status

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
