# ASQL: the SQL engine, its MySQL wire, replication, Things and Cells

ASQL is the relational engine inside AIOS. Tables live in memory and are saved
to the kernel root. It speaks the MySQL wire protocol, so the stock `mysql`
client, PDO and the usual GUI tools can connect to it. It can follow a MariaDB
through the binlog. On top of the tables it keeps two kinds of live object:

- **Things**: grouped query results that stay current as rows change, and can
  act when a condition becomes true.
- **Cells**: small per-key counters and bits that follow every row change, and
  act when a bit rises or falls.

Every example below was run against the already-built binaries in `build/`
(`build/aios`, `build/aios-engine`, `build/aios-server`,
`build/aios-asql-console`, `build/aios-asql-top`) on arch. Each run used a
throwaway root under `/tmp/aios-docs-asql/`, `AIOS_NO_ACCEL=1`, ports
13410-13429 and `127.0.0.1`. The client was the stock `mysql` CLI (MariaDB
10.11). Output is trimmed but not edited.

Sources: `src/userland/sql_database.cpp`, `include/aios/userland/sql_database.hpp`,
`src/userland/mysql_wire.cpp`, `src/userland/mysql_binlog.cpp`,
`include/aios/userland/mysql_binlog.hpp`, `src/userland/sql_things.cpp`,
`include/aios/userland/sql_things.hpp`, `src/userland/sql_cells.cpp`,
`include/aios/userland/sql_cells.hpp`, `src/channel/compute_serve.cpp`,
`cli/commands.cpp` (`cmd_sql`, `compute serve`), `tools/aios-engine.cpp`,
`tools/aios-server.cpp`, `tools/asql-console.cpp`, `tools/asql-top.cpp`,
`packaging/`.

---

## Four ways to start it

| Entry point | Store directory | Wire accounts | Unix socket | Replication | Use it for |
|---|---|---|---|---|---|
| `aios exec "sql serve ..."` | `<root>/sql_db` | none (loopback only) | no | no | a quick local server |
| `aios exec "compute serve sql=1 ..."` | `<root>/<sql_db=>` (default `sql_db`) | one (`mysql_user=`) | `mysql_socket=` | `cdc_host=` with seeding | **the production path**. The packaged service runs this. |
| `build/aios-engine` | `<root>/<--upstream-db>` (default `versioneight`) | one (`--sql-user=`) | `--socket=` | `--upstream=` with seeding (`--tables=`) | standalone engine |
| `build/aios-server` | `<root>/<--db>` (default `aios_gateway`; an existing `sql_db` when only that is there) | none (loopback only) | no | no | a gateway with write-through or write-behind to a downstream |

They share one engine (`SqlDatabase`) and one wire server (`MysqlWireServer`).
The wire reports itself as `5.7.44-aios-asql`.

Set `AIOS_NO_ACCEL=1` on every one of them. Without it the kernel boot brings
up OpenCL, CUDA and Vulkan for nothing. `aios-server` sets it on its own if it
is unset.

### `sql serve`: the quickest server

```
$ AIOS_NO_ACCEL=1 aios --root /tmp/aios-docs-asql/r1 exec "sql serve port=13410 bind=127.0.0.1"
[SUCCESS] AIOS ASQL / MySQL Native Wire Server listening on 127.0.0.1:13410 (Press Ctrl+C to stop)
Ready for Navicat, DBeaver, TablePlus, and MySQL CLI connections.
```

`port=` defaults to 3307 and `bind=` to `0.0.0.0`. It takes no account. The
wire's rule with no account is that only loopback clients get in, with any
user name and any password (`mysql_wire.cpp:1831`). A remote client is refused
even when `bind=0.0.0.0`.

```
$ mysql -h 127.0.0.1 -P 13410 -u anyone --table -e "SELECT VERSION(); SHOW DATABASES;"
+------------------+
| VERSION()        |
+------------------+
| 5.7.44-aios-asql |
+------------------+
+--------------------+
| Database           |
+--------------------+
| aios_db            |
| information_schema |
| mysql              |
| performance_schema |
| sys                |
+--------------------+
```

The default database is `aios_db`. `CREATE DATABASE`, `USE` and
`db.table` names work as in MySQL:

```
$ mysql -h 127.0.0.1 -P 13410 -u anyone --table -e "CREATE DATABASE docs; USE docs;
  CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT, customer VARCHAR(20), amount DECIMAL(10,2), status INT DEFAULT 0);
  INSERT INTO orders (customer, amount, status) VALUES ('acme', 10.50, 0), ('acme', 4.25, 1), ('globex', 99.00, 0);
  SELECT customer, SUM(amount) s, COUNT(*) n FROM orders GROUP BY customer ORDER BY customer;"
+----------+-------+------+
| customer | s     | n    |
+----------+-------+------+
| acme     | 14.75 |    2 |
| globex   | 99.00 |    1 |
+----------+-------+------+
```

Stop it with SIGTERM or Ctrl+C. Data survives process termination: restarting
the engine on the same root replays the redo log under `sql_db/` at startup,
restoring every committed transaction.

### `compute serve sql=1`: the production path

This is what the packaged unit runs (see [Running it as a service](#running-it-as-a-service)).
It starts the compute RPC service and attaches the engine to it. It can also
start the MySQL wire, a unix socket, and binlog replication.

```
$ printf 'wirepw\n' > r7.pass; printf 'tok123\n' > r7.token
$ AIOS_NO_ACCEL=1 aios --root /tmp/aios-docs-asql/r7 exec "compute serve port=13417 lanes=2 sql=1 \
    mysql_port=13418 mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=/tmp/aios-docs-asql/r7.pass \
    mysql_socket=/tmp/aios-docs-asql/r7.sock token_file=/tmp/aios-docs-asql/r7.token"
compute sql op attached: sql_db (1 table(s), reads only)
compute mysql wire server listening on unix socket /tmp/aios-docs-asql/r7.sock
compute mysql wire server listening on 127.0.0.1:13418 (password required for user app)
compute service as entity f217dd9403f1 over 1 cpu device(s); kernels: erp.tsp_exact, erp.route_improve, erp.pool_greedy; sql op: read-only; mysql wire: read-write
compute service listening on 127.0.0.1:13417 lanes=2 max_points=15 token=required
```

```
$ mysql -S /tmp/aios-docs-asql/r7.sock -u app -pwirepw --table -e "CREATE DATABASE shop; USE shop;
  CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR(30)); INSERT INTO items VALUES (1,'bolt'),(2,'nut'); SELECT * FROM items;"
+------+------+
| id   | name |
+------+------+
|    1 | bolt |
|    2 | nut  |
+------+------+
$ mysql -S /tmp/aios-docs-asql/r7.sock -u app -e "select 1"
ERROR 1045 (28000): Access denied for user 'app'
```

These keys are read in `cli/commands.cpp` (`compute serve`, about lines 2261-2482):

| Key | Default | Meaning |
|---|---|---|
| `port=` | 7311 | compute RPC port (length-prefixed JSON, see below) |
| `public=1` | off | RPC listens on `0.0.0.0`. Without it the RPC port is loopback only. |
| `lanes=`, `max_points=`, `shm_ring=` | | compute-service knobs, not SQL |
| `token=` / `token_file=` | none | RPC requests must carry `"token"` |
| `sql=1` | off | attach the engine. Without it there is no SQL at all. |
| `sql_db=` | `sql_db` | store directory under the root |
| `mysql_port=` (or `port_mysql=`) | 3307 | MySQL wire TCP port |
| `mysql_bind=` | `0.0.0.0` | forced to `127.0.0.1` when `mysql_user=` is not set |
| `mysql_loopback=1` | | same as `mysql_bind=127.0.0.1` |
| `mysql_user=`, `mysql_pass=` / `mysql_pass_file=` | none | the one wire account. The file's first line is the password. |
| `mysql_strict_loopback=1` | off | with no account, refuse loopback clients too |
| `mysql_socket=` | none | also listen on this unix socket |
| `mysql_charset=1` | off | honour each client's charset like MariaDB (a latin1 client is sent cp1252) |
| `cdc_host=`, `cdc_port=`, `cdc_user=`, `cdc_pass=` / `cdc_pass_file=`, `cdc_db=`, `cdc_tables=`, `cdc_log_file=`, `cdc_log_pos=` | | replication, see below |

The wire has one account. Once that account has a password, the password is
always checked, even from loopback and even on the socket. The socket file is
created mode `srw-rw-rw-`. Access is by password, not by file mode.

**The RPC `sql` op only reads.** It is a 4-byte big-endian length followed by
a JSON object. It takes `SELECT`, `SHOW`, `DESCRIBE`, `EXPLAIN`, `WITH` and
replication control, and refuses everything else (`compute_serve.cpp:175`):

```
$ python3 rpc.py '{"op":"sql","token":"tok123","db":"shop","query":"SELECT id, name FROM items ORDER BY id"}'
{"ok":true,"op":"sql","columns":["id","name"],"rows":[["1","bolt"],["2","nut"]],"row_count":2}
$ python3 rpc.py '{"op":"sql","token":"tok123","db":"shop","query":"DELETE FROM items"}'
{"ok":false,"op":"sql","error":"reads only: SELECT / SHOW / DESCRIBE / EXPLAIN / WITH (+ replication control)"}
$ python3 rpc.py '{"op":"sql","query":"SELECT 1"}'
{"ok":false,"op":"sql","error":"unauthorized"}
```

`rpc.py` is a ten-line client: `struct.pack(">I", len(body)) + body` out, the
same framing back. `"db"` names the database. A database the engine does not
hold is refused, never answered from another one.

### `aios-engine`: the standalone engine

```
$ build/aios-engine --help
Usage: build/aios-engine [options]
  --upstream=<target>       MariaDB/MySQL upstream for CDC (e.g. 127.0.0.1:3306 or /run/mysqld/mysqld.sock)
  --upstream-user=<user>    Upstream replication username (default: root)
  --upstream-pass=<pass>    Upstream replication password
  --upstream-pass-file=<f>  Upstream replication password, first line of a file
  --upstream-db=<db>        Upstream database name (default: versioneight)
  --server-id=<n>           Replica server id (default: derived from this root's identity)
  --tables=<t1,t2,...>      Comma-separated whitelist of tables to mirror; a listed table
                            not in the store yet is copied from the upstream first
  --port=<port>             MySQL Wire Protocol TCP port (default: 3307)
  --bind=<ip>               MySQL Wire Protocol bind IP (default: 0.0.0.0)
  --socket=<path>           Also listen on this UNIX domain socket (default: none)
  --sql-user=<user>         Account a client logs in with (required unless --trust-loopback)
  --sql-pass=<pass>         Its password (visible in ps; prefer --sql-pass-file)
  --sql-pass-file=<file>    Its password, first line of a file
  --trust-loopback          Let loopback clients in without an account
  --shm-ring=<name> --telemetry-slots=<N> --watch-fs=<dir>
  --root=<dir>              AIOS data root directory (default: /tmp/aios-engine-data)
  --mode=<mode>             Storage sync mode: local (default), write_behind or write_through;
                            both of the latter need --downstream
  --downstream=<target>     Where write_behind/write_through deliver writes: host:port, a
                            socket path, or 'upstream' (the --upstream server and account)
  --downstream-user=<user>  --downstream-pass=<p>  --downstream-pass-file=<f>
  --downstream-db=<db>      Downstream account and database (default: the upstream's)
  --listen-gate             Open no listener until replication is connected and caught up
                            (needs --upstream)
  --benchmark               Run internal self-diagnostic throughput benchmark on startup
```

An option it does not know is refused (`[!] unknown option: --sokcet=/x`), as
is a `--mode` other than the three, a gateway mode with no `--downstream`, and
`--listen-gate` with no `--upstream`. With no account and no
`--trust-loopback` it refuses to open the port, even on 127.0.0.1:

```
$ AIOS_NO_ACCEL=1 build/aios-engine --root=/tmp/aios-docs-asql/r3 --port=13413 --bind=127.0.0.1
[!] Refusing to open 127.0.0.1:13413 with no account: pass --sql-user= and --sql-pass-file=, or --trust-loopback for a local-only engine.
```

With an account:

```
$ printf 's3cret\n' > r3pass; chmod 600 r3pass
$ AIOS_NO_ACCEL=1 build/aios-engine --root=/tmp/aios-docs-asql/r3 --port=13413 --bind=127.0.0.1 \
    --sql-user=app --sql-pass-file=/tmp/aios-docs-asql/r3pass
  ✓ MySQL Native Wire Gateway listening on 127.0.0.1:13413
$ mysql -h 127.0.0.1 -P 13413 -u app -pwrong -e "select 1"
ERROR 1045 (28000): Access denied for user 'app'
$ mysql -h 127.0.0.1 -P 13413 -u app -ps3cret -e "CREATE TABLE t (id INT PRIMARY KEY, v TEXT); INSERT INTO t VALUES (1,'a'); SELECT * FROM t"
id	v
1	a
```

With `--trust-loopback` and no account it prints
`! No SQL account set: only loopback clients will be let in.`

On SIGTERM it stops the CDC stream and the wire, then checkpoints:

```
[aios-engine] Initiating graceful shutdown...
  ✓ MySQL Wire Server stopped.
  ✓ Store checkpointed to disk.
[aios-engine] Shutdown complete. Goodbye.
```

Things to know about `aios-engine`:

- The store goes in `<root>/<--upstream-db>`, which is `<root>/versioneight` by
  default. Even so, a client still lands in database `aios_db`.
- `--socket=PATH` opens a unix socket beside the TCP port
  (`✓ MySQL Native Wire Gateway listening on unix socket PATH`); `mysql -S PATH`
  reaches the same engine. With no `--socket` there is none.
- `--listen-gate` holds both listeners back until replication has applied the
  master's binlog up to where `SHOW MASTER STATUS` stood when the gate began
  (`✓ Listen gate open: replication caught up after N ms`).
- `--shm*`, `--telemetry-slots` and `--watch-fs` have no effect on SQL.
- It prints to stdout fully buffered when redirected. Use `stdbuf -oL` if you
  tail a log.

### The capability key

`aios-engine` needs a capability master key (`tools/aios-engine.cpp:75-99`):

1. `AIOS_CAPABILITY_KEY`, if set and non-empty, is used as is. No file is
   written.
2. Otherwise it reads the first line of `<root>/capability.key`.
3. Otherwise it writes 32 random bytes as 64 hex characters there, under
   `umask 077`, and says so once:

```
  • Capability key       : generated /tmp/aios-docs-asql/r4/capability.key (keep it private; share it only between machines that must trust each other)
$ ls -la r4/capability.key
-rw------- 1 arch arch 65 Oct  1 18:01 r4/capability.key
```

A second start on the same root reuses the key without a message. With
`AIOS_CAPABILITY_KEY=deadbeef`, no `capability.key` appeared in a fresh root.
If the key can be neither read nor written, the engine exits:
`[!] No capability key: set AIOS_CAPABILITY_KEY or make <root>/capability.key readable and writable`.
Machines that must accept each other's tokens share the file.

### Running one statement with no server

`aios sql` opens `<root>/sql_db` directly. Do not run it against a store a
running server has open.

```
$ AIOS_NO_ACCEL=1 aios --root /tmp/aios-docs-asql/r7 exec 'sql query "SELECT * FROM shop.items"'
[SQL RESULT]
+----+------+
| id | name |
+----+------+
| 1  | bolt |
| 2  | nut  |
+----+------+
2 row(s) in set

$ printf "CREATE TABLE t1 (id INT PRIMARY KEY, val VARCHAR(10));\nINSERT INTO t1 VALUES (1,'hello'),(2,'it''s');\n" > dump.sql
$ AIOS_NO_ACCEL=1 aios --root /tmp/aios-docs-asql/r8 exec "sql import file=/tmp/aios-docs-asql/dump.sql" 'sql query "SELECT * FROM t1"'
Importing MySQL/MariaDB SQL file: /tmp/aios-docs-asql/dump.sql
[IMPORT SUCCESS]
MySQL / MariaDB Import Summary:
  Statements: 2 total, 2 executed, 0 empty/comments.
  Rows Affected: 2
  Tables Available: 2
```

The `sql` group (`cli/commands.cpp:7928`; aliases `asql`, `mariadb`, `db`):

    sql query "<sql>"           one statement (also: sql exec)
    sql import file=<dump.sql>  a mysqldump file (also: sql pull)
    sql import sql="<stmts>"
    sql tables                  tables of the default database
    sql console [db=<name>]     an in-process REPL
    sql serve [port=3307] [bind=0.0.0.0]
    sql push / sql sync         task-ledger export and sync

### The operator tools

`aios-asql-console` and `aios-asql-top` are wire clients. They read
`/etc/aios/asql.conf` (or `$ASQL_CONF`, or `--conf FILE`) for the socket, user,
password file and database. `--no-conf` ignores the conf. `-S -h -P -u -p -d`
override it.

```
$ aios-asql-console --no-conf -P 13410 -u x -d docs -e "SELECT customer, amount FROM orders WHERE status = 0"
+----------+--------+
| customer | amount |
+----------+--------+
| acme     | 11.00  |
| acme     | 4.25   |
...
$ aios-asql-top --no-conf -P 13410 -u x -1
  ● ONLINE | Target: 127.0.0.1:13410 | User: x | Time: 2026-10-01 18:04:15
  Uptime: 247s | Active DB: aios_db
    Total Queries     : 42 [Sel: 26 | Ins: 5 | Upd: 7 | Del: 0]
    Tables Loaded     : 3 tables | Total Rows: 9 rows
  LIVE STATEMENT ACTIVITY STREAM:
    [OK     0.04ms |     4 rows] SELECT customer, amount FROM orders WHERE status = 0
```

`-1` prints one snapshot. Without it, `top` refreshes every `-i` ms (default
1000). The same figures are available over the wire with
`SHOW STATUS LIKE 'Asql_%'`, `SHOW ENGINE ASQL STATUS`, `SHOW ASQL QUERIES`,
`SHOW ASQL ERRORS` and `SHOW ASQL SLOW`.

---

## Connecting and querying from Grain

Grain programs interact with ASQL through three distinct access mechanisms, depending on whether code runs in-process inside the kernel, connects over the network via wire protocol, or executes ad-hoc loopback queries:

```
+-------------------------------------------------------------------------------+
|                             Grain Applications                                |
+-------------------------------------------------------------------------------+
         |                                |                            |
         | 1. In-Process (Kernel/Mesh)    | 2. Wire Protocol (TCP)     | 3. Loopback
         v                                v                            v
  sql() / sql_on()               db_connect() / db_query()    asql_query() / asql_exec()
  [src/userland/machine_sql.cpp] [src/userland/grain_std_db.cpp] [src/userland/grain_aios.cpp]
         |                                |                            |
         | Direct C++ Pointer             | MySQL Wire (e.g. :3307)    | Loopback TCP
         v                                v                            v
  +-----------------------------------------------------------------------------+
  |                          ASQL Engine (SqlDatabase)                          |
  +-----------------------------------------------------------------------------+
```

### 1. In-process engine: `sql()` and `sql_on()`
*Source: [`src/userland/machine_sql.cpp:333-355`](file:///repos/aios/src/userland/machine_sql.cpp#L333-L355)*

When Grain runs inside the AIOS kernel (`aios grain ...`, bare metal, or system services), `install_sql_builtins` binds `sql()` directly to the process's `SqlDatabase` instance. This completely bypasses TCP sockets, auth handshakes, and wire serialization.

- **Result set queries** (`SELECT`, `SHOW`, `EXPLAIN`): Returns a list of maps, one per row.
  - Number types are mapped to native Grain numbers: `SqlDataType::Int` becomes integer, `Real` / `Decimal` become real numbers.
  - `NULL` becomes `nil`. Text and dates become strings.
- **Non-query statements** (`INSERT`, `UPDATE`, `DELETE`, DDL): Returns a map `{"affected": <integer>}`.
- **Errors**: Failures throw a Grain runtime error (not a map with `ok=false` to overlook). Catch errors with `pcall()`.

```grain
// In-process query: zero socket overhead, sub-microsecond execution
let rows = sql("SELECT stockid, description, standardcost FROM stockmaster WHERE discontinued = 0 LIMIT 10")
for row in rows {
  print(row.stockid + ": " + str(row.standardcost))
}

// Data modification: returns affected row count
let res = sql("UPDATE pickingfreeze SET status = 'DONE' WHERE id = 1042")
print("Updated " + str(res.affected) + " rows")

// Fleet mesh query across peer nodes:
let peer_rows = sql_on("remote-node", "SELECT COUNT(*) AS total FROM recscan")
```

### 2. Wire client protocol: `db_connect()`, `db_query()`, `db_transaction()`
*Source: [`src/userland/grain_std_db.cpp:351-570`](file:///repos/aios/src/userland/grain_std_db.cpp#L351-L570)*

For standalone Grain programs (`grain run ...`, `grain serve ...`), background workers, or connecting across network boundaries, Grain provides a zero-dependency MySQL 5.7 wire client. It connects to ASQL's native wire port (default 3307) or external MariaDB/MySQL instances.

#### Connecting (`db_connect`)
```grain
// Signature: db_connect(host, port, user, password, [database])
let res = db_connect("127.0.0.1", 3307, "root", getenv("DB_PASS"), "versioneight")
if !res.ok {
  print("Connection failed: " + res.error)
  return nil
}
let db = res.conn   // or pass `res` directly to db_* functions
```
The result map contains `{"ok": true, "conn": <handle>, "server_version": "5.7.44-aios-asql", "error": ""}`. Either the returned map or the inner `conn` handle can be passed as the connection argument to all subsequent `db_*` builtins (`grain_std_db.cpp:322`).

> [!IMPORTANT]
> **Thread & Connection Rules** ([`docs/grain-app-writing.md:224`](file:///repos/aios/docs/grain-app-writing.md#L224)): Sockets and database connections cannot cross thread boundaries. In multi-worker `ui.pane` web services, open the database connection inside request handlers or worker tasks, never in global module scope. Always call `db_close(db)` when finished.

#### Safe Parameter Binding (`db_query`)
Queries use `?` placeholders with safe parameter list binding (`grain_std_db.cpp:474-505`). Values are escaped automatically; `nil` binds as `NULL`, booleans as `1`/`0`, and nested lists automatically expand into comma-separated runs for SQL `IN (?)`:

```grain
// Parameterized query with ? placeholders
let items = db_query(db, "SELECT * FROM stockmaster WHERE category = ? AND standardcost > ?", ["TIRES", 50.0])

// Lists expand automatically for IN (?) clauses
let ids = ["T100", "T200", "T300"]
let matches = db_query(db, "SELECT stockid, description FROM stockmaster WHERE stockid IN (?)", [ids])

// Memory optimization: omit `rows` when only `records` are needed
let fast = db_query(db, "SELECT stockid FROM stockmaster", {"rows": false})
```

`db_query` returns:
- `ok`: boolean success flag.
- `error`: error string (empty if ok).
- `columns`: list of column name strings `["col1", "col2", ...]`.
- `rows`: list of lists in column order `[[val1, val2], ...]`.
- `records`: list of row maps `[{"col1": val1, "col2": val2}, ...]`.
- `affected_rows`: integer count of modified rows.
- `last_insert_id`: integer ID for `AUTO_INCREMENT` inserts.

#### Transactions with Nested Savepoints (`db_transaction`)
`db_transaction(conn, fn)` wraps operations in an ACID transaction (`grain_std_db.cpp:517`):
- Top-level call begins with `START TRANSACTION` and commits with `COMMIT`.
- Nested calls within the closure automatically use `SAVEPOINT grain_sp_<depth>`.
- If an unhandled error or exception occurs in the callback, it automatically issues `ROLLBACK` (or `ROLLBACK TO SAVEPOINT`) and rethrows the error.

```grain
let result = db_transaction(db, fn(tx) {
  let ins = db_query(tx, "INSERT INTO recscan (scan_date, user, qty) VALUES (NOW(), ?, ?)", ["JACK", 10.0])
  if !ins.ok { error("Failed insert: " + ins.error) }
  return ins.last_insert_id
})
```

### 3. Loopback one-shot helpers: `asql_query()` and `asql_exec()`
*Source: [`src/userland/grain_aios.cpp:649-714`](file:///repos/aios/src/userland/grain_aios.cpp#L649-L714)*

For administrative scripts or quick CLI pipelines running on the same host as ASQL, `asql_query` and `asql_exec` manage connection lifecycle automatically:

```grain
// asql_query(sql, [port=3307], [login]) -> returns list of row maps directly
let users = asql_query("SELECT userid, realname FROM www_users")
for u in users {
  print(u.userid + ": " + u.realname)
}

// asql_exec(sql, [port=3307], [login]) -> returns {ok, affected_rows, last_insert_id, error}
let res = asql_exec("DELETE FROM pickingfreeze WHERE status = 'CANCELLED'")
if res.ok {
  print("Removed " + str(res.affected_rows) + " rows")
}
```

### 4. Reactive polling: `live()`
*Source: [`src/userland/grain_std_db.cpp:640-699`](file:///repos/aios/src/userland/grain_std_db.cpp#L640-L699)*

Instead of repeatedly querying heavy tables, Grain's `live()` primitive polls ASQL's mutation tracker via `SELECT ASQL_CHANGES('table1', 'table2')`. The underlying query is only re-evaluated when a table token actually moves, driving reactive dashboards with negligible CPU overhead.

---

## Connection profiles: `connections.ini`

A profile names another MySQL-protocol server the engine may connect to. Three
features use profiles:

- Thing and Cell acts (`DO ... TO <profile>`)
- event Things (`WRITES TO <profile>`)
- selecting a remote profile as the active one

The file is `<root>/<store dir>/connections.ini`, for example
`<root>/sql_db/connections.ini`. It is read once, at engine init
(`sql_database.cpp:10516`). Edit it and then restart the engine.

**There are no built-in remote profiles.** The only profile that always exists
is `local_embedded`, which is this engine itself. Every remote profile comes
from the file (`sql_database.cpp:10582`). The engine rewrites the file itself
when a profile is added or removed through the API. One appeared in a fresh
root holding only `local_embedded`.

The format is `[profile]` blocks of `key=value` lines:

```ini
[profile]
id=sink
name=Second ASQL on 13411
host=127.0.0.1
port=13411
user=docs
password=
database=sinkdb
things_write=1
```

| Field | Default | Meaning |
|---|---|---|
| `id` | (required) | the name `TO <id>` uses. A block without `id`, or with `id=local_embedded`, is skipped. |
| `name` | | display name |
| `host`, `port` | `127.0.0.1`, `3306` | TCP target |
| `socket_path` | | a unix socket. When set, every session to the profile (acts, `WRITES TO`, the active profile, the reachability probe) dials it instead of `host:port`. |
| `user`, `password`, `database` | `root`, empty, `aios_db` | login |
| `password_file` | | a file whose first line is the password. The file keeps only the path (`password=` is not written when this is set). |
| `use_ssl`, `ssh_host`, `ssh_port`, `ssh_user` | | stored, but **not implemented**: a profile with `use_ssl=1` or an `ssh_host` is refused (`profile 'x' asks for SSL, which ASQL's client does not implement; refusing to connect in plain text ...`) rather than connected in the clear. Run the tunnel yourself and point `host`/`port` (or `socket_path`) at it. |
| `things_write` | `0` | `1`/`true`: Things and Cells may write here. Without it every act is refused at `CREATE` time. |
| `is_local_embedded` | `0` | |

A `password=` sits in this file in plain text; prefer `password_file=`. The
engine writes the file owner-only (0600) whenever it saves it, as
`scripts/aios-setup.sh` does; an older file is read as it is.

---

## `ASQL_CHANGES()`: has a table been written

`SELECT ASQL_CHANGES('orders', 'order_lines')` returns a token that changes
whenever any of the named tables is written: an INSERT moves its row count, an
UPDATE the written column's version, a DELETE (or a rebuild) the table's
`mutation_seq`, and the token carries all three per table. A table that does not
exist reads as `-`. It reads no rows, and the statement is never cached (the
query cache would otherwise keep its first answer, since the statement names no
table in a FROM). grain's `live()` asks it every tick and reruns its query only
when it moves.

## Write modes: local, write-through, write-behind

`WriteSyncMode` has three values (`sql_database.hpp:26`). What each one does is
decided in `execute_gateway_query` (`sql_database.cpp:13205-13280`):

A write-behind statement the downstream refuses is retried, and the ones queued
behind it wait, so their order is kept; `aios-engine` reports the stuck count and
the last error every minute.

- **`local` (the default).** A statement runs against the in-memory tables and
  is made durable by the redo log and checkpoints. If no downstream is
  configured, nothing leaves the process.
- **`write_through` (Pattern A).** A statement that changes data or schema
  (`INSERT`, `REPLACE`, `UPDATE`, `DELETE`, `CREATE`, `DROP`, `ALTER`,
  `TRUNCATE`, `RENAME`) and transaction control (`BEGIN`, `COMMIT`,
  `ROLLBACK`, ...) go to the downstream **first**, on this client's own
  connection to it, in the client's current database, and are applied here
  only once the downstream has taken them. A statement the downstream refuses
  or cannot be reached for is refused here too, with nothing applied:
  `Duplicate entry '1' for key 'PRIMARY' (write-through: refused by the
  downstream; nothing applied here)`. A `ROLLBACK` undoes the transaction on
  both sides. `SHOW`, `SET`, `USE` and `SELECT` are answered here.
- **`write_behind` (Pattern B).** The same statements run here first. One that
  succeeds is appended to `<store>/write_behind.wal` (inside a transaction:
  at its `COMMIT`; a `ROLLBACK` drops them) and a drain worker (every 20 ms,
  batches of 100) replays them on the downstream in order, acknowledging each
  in the WAL once the downstream has taken it. One that fails here is not
  sent anywhere. Unacknowledged statements survive a restart and drain when
  the downstream is back. A statement the downstream refuses holds the ones
  behind it (order is kept); `aios-engine` reports it every minute
  (`[aios-write-behind] N statement(s) not yet delivered downstream; last error: ...`).
  `AIOS_WRITE_BEHIND=1` also turns this mode on.

Both gateway modes need a downstream and refuse a write when there is none.
`aios-engine --mode=write_through|write_behind` requires
`--downstream=host:port|socket|upstream` (`upstream` reuses the `--upstream`
server and account; `--downstream-user/-pass/-pass-file/-db` override them).
`aios-server --downstream host:port` sets one too (`tools/aios-server.cpp`,
user `root`, empty password, database `downstream`). `compute serve` never
sets one.

Driven, `aios-engine` write-through into a second ASQL on 13592:

```
$ build/aios-engine --root=… --port=13593 --bind=127.0.0.1 --trust-loopback \
    --mode=write_through --downstream=127.0.0.1:13592 --downstream-user=root --downstream-db=w
$ mysql -h 127.0.0.1 -P 13593 -u root -e "create database w; use w; create table o(id int primary key, v text);
    insert into o values (1,'a'); begin; insert into o values (2,'b'); rollback;
    begin; insert into o values (3,'c'); commit; create index iv on o(v); insert into o values (1,'dup')"
ERROR 1062 (23000) at line 1: Duplicate entry '1' for key 'PRIMARY' (write-through: refused by the downstream; nothing applied here)
$ mysql -h 127.0.0.1 -P 13592 -u root -e "select * from w.o"      # the downstream: 1 a, 3 c (2 rolled back on both)
```

With the downstream stopped, a write answers `write-through: downstream
unavailable: ... Connection refused (nothing applied here)` and the row is not
in the local table either.

Driven, write-behind with the downstream stopped part way: the insert made
while it was down was kept in the WAL through a restart
(`[!] write-behind WAL not fully delivered (kept for the next start)`) and
delivered when both were back up. `SHOW`, `SET` and a rolled-back insert never
reach the WAL.

---

## Replicating a MariaDB (binlog CDC)

The `aios-engine` path was driven against a scratch MariaDB 10.11 started on
its own datadir (see below). The `compute serve` path is read from
`mysql_binlog.cpp`, `sql_database.cpp` and `cli/commands.cpp`.

### What the upstream needs

- **Binary logging on, `binlog_format=ROW`.** The consumer applies only
  `WRITE_ROWS`, `UPDATE_ROWS` and `DELETE_ROWS` events (v1 and v2,
  `mysql_binlog.cpp:1197-1202`). `QUERY_EVENT`s are used only for table and
  view DDL (`CREATE`, `ALTER`, `TRUNCATE`, `DROP` on whitelisted tables,
  `CREATE`/`ALTER`/`DROP VIEW`). An `INSERT`, `UPDATE` or `DELETE` that the
  server logs as a statement, as `MIXED` and `STATEMENT` formats do, never
  reaches the mirror.
- **`binlog_row_image=FULL`** for any table without a primary key. Updates and
  deletes find their row by the primary-key columns in the before image. A
  table with no primary key is matched on every column of the before image
  (`mysql_binlog.cpp:1907-1920`).
- **A transaction's rows are applied together.** Its inserts into one table go
  as one multi-row `INSERT ... ON DUPLICATE KEY UPDATE c = VALUES(c)` (row by
  row again if that fails, so a bad row costs only itself); deletes by a
  one-column key as one `DELETE ... WHERE k IN (...)`; deletes from a table
  without a key as one `DELETE FROM t MATCHING (v1, ...), (...)` - an ASQL
  form that removes one row per tuple, the row whose every column (in the
  table's order) equals it as `=` compares, found in one pass from the first
  row. A delete per row scanned the table and rewrote every column each time:
  a source that deletes and reloads a 31M-row log table nightly left the
  mirror days behind.
- **Checksums do not matter.** The consumer sends
  `SET @master_binlog_checksum='NONE'` and a 5 s heartbeat before
  `COM_BINLOG_DUMP`.
- **A user with `REPLICATION SLAVE`** for `COM_REGISTER_SLAVE` and
  `COM_BINLOG_DUMP`, **`REPLICATION CLIENT`** for `SHOW MASTER STATUS` /
  `SHOW BINARY LOGS`, and **`SELECT`** on the tables, which the seeding copy
  reads. `README-ASQL.md` lists the same three.
- **TCP or a unix socket.** A host that is a path (`/run/mysqld/mysqld.sock`)
  is dialled as a unix socket, by the stream and by the seeding copy alike.

### The `compute serve` path (seeds, then streams)

```
aios --root R exec "compute serve sql=1 mysql_port=3307 mysql_user=erp mysql_pass_file=/opt/aios/mysql.pass \
  cdc_host=127.0.0.1 cdc_port=3306 cdc_user=repl cdc_pass_file=/opt/aios/cdc.pass \
  cdc_db=shopdb cdc_tables=@/etc/aios/whitelist.tables"
```

- `cdc_tables=` names a **file**, one table per line, `#` comments. A leading
  `@` is stripped. A bare csv is not a file, so it is ignored.
  `packaging/asql-serve` writes the conf's csv into
  `$AIOS_ROOT/cdc_tables.list` for that reason.
- **Seeding.** Each whitelisted table not yet in the store is copied from the
  source in one consistent snapshot, before the stream starts, and that
  snapshot's binlog position is saved per table (`seed_positions`). The log
  shows `compute sql: seed ...` and
  `compute sql: whitelist - N of M table(s) present`. Tables already present
  get their column metadata re-read from the source.
- **The stream** is then set up through SQL the engine also accepts on the
  wire: `CHANGE REPLICATION SOURCE TO SOURCE_HOST=..., SOURCE_PORT=...,
  SOURCE_USER=..., SOURCE_PASSWORD=..., SOURCE_DATABASE=...
  [, SOURCE_LOG_FILE=..., SOURCE_LOG_POS=...]`, then
  `SET REPLICATION WHITELIST = t1, t2, ...` and `START REPLICA`. The
  `MASTER_*` spellings, `START/STOP SLAVE` and
  `SET REPLICATION LIMIT t = N` (FIFO-trim a table to N rows) are accepted too
  (`sql_database.cpp:13978-14125`).
- **Position.** An explicit `cdc_log_file`/`cdc_log_pos` wins. Use this for a
  `mysqldump --single-transaction --master-data=2` seed imported with
  `sql import`. Otherwise the stream resumes from `<store>/cdc_position`. Only
  a store that never streamed starts at the source's current position.
- **Replica id.** It is derived from the store's kernel identity, so two
  mirrors of one source do not knock each other off (`sql_database.cpp:27474`).
- **One source schema.** Row events for other databases on the same server are
  dropped (`in_source_schema`). An empty whitelist means every table in that
  schema the binlog mentions. Rows for a table the mirror does not hold are
  skipped with
  `[cdc] db.t: changes arrive but the mirror holds no such table (not seeded?) - skipped`.
- With `cdc_host` set, the banner says
  `mysql wire: read-write (cdc mirror: replication overwrites local writes)`.
  A local write to a mirrored table is overwritten by the next binlog change
  to that row.
- `SHOW REPLICA STATUS` (or `SHOW SLAVE STATUS`) reports the stream, and
  `SHOW ASQL MIRROR [table]` compares each table with the source (this is what
  `asql-check` uses). `AIOS_CDC_TRACE=1` logs every applied statement.

### The `aios-engine` path

```
build/aios-engine --upstream=10.0.0.5:3306 --upstream-user=repl --upstream-pass-file=/path/cdc.pass \
  --upstream-db=shopdb --tables=orders,lines --sql-user=app --sql-pass-file=/path/pass [--listen-gate]
```

It starts the stream through the engine's own replica start
(`start_binlog_sync`), as `START REPLICA` does, so:

- A `--tables` table not yet in the store is seeded from the upstream first
  (`seed: a: 2 rows from bin.000001:1609`), its binlog point saved.
- The saved `cdc_position` is resumed from: a row written while the engine
  was stopped arrives on the next start.
- The replica id is derived from the root's identity (`--server-id=` overrides).
- `SHOW REPLICA STATUS` on its wire reports this stream.
- `✓ In-process CDC replication attached to …` is printed only once the
  stream is connected; otherwise
  `[!] CDC not attached to … yet (retrying in the background)`.

Driven against a scratch MariaDB 10.11 (its own datadir, port 13600,
`--log-bin --binlog-format=ROW`), with `--upstream=<its socket>`,
`--tables=a`, `--socket=` and `--listen-gate`: the table was seeded, the gate
opened, a row inserted on the master arrived, and after a stop, an insert on
the master and a start, that row arrived too.

---

## Things

A Thing kind is a grouped query that the engine keeps current. Each distinct
`KEY` value is one Thing. When a row changes in a table the query reads, the
engine re-reads only the partitions that row touched and replaces their
contribution. Things are saved in `<store>/things/<name>.thing` and come back
at start (`asql: 2 Thing kind(s), 4 things back from disk`).

### Syntax (aggregate and row Things)

From `ThingEngine::parse_create` (`sql_things.cpp:873-1150`):

```
CREATE THING <name> KEY <column>
  [SOURCE BY <column>]
  [PARTITION BY <column>]
  [READS <table>[, ...]]
  AS SELECT <key>, <value> [AS label], ... FROM <source> [WHERE ...] GROUP BY <key>[, ...]
  [PART <name> AS SELECT ... WHERE <col> = KEY]...
  [WHEN <condition> DO <statement>[; ...] TO <profile>]
```

- The first select column must be the `KEY`, and `GROUP BY` must start with it.
- `HAVING`, `ORDER BY`, `LIMIT`, `UNION` and `WITH ROLLUP` are refused:
  `a Thing's query cannot use HAVING: partitions could not add up to it`.
- `PARTITION BY` defaults to the key. When the key and the partition are the
  same column, the kind is a **row Thing**: one row per key, and the values can
  be any expression (`AVG`, `MAX`, `CASE`, `t.*`). When they differ, the values
  must be `SUM` or `COUNT`, optionally times a number, so that partitions add
  up.
- `READS` defaults to every table the query names.
- `PART` sub-queries are allowed only on row Things.
- `WHEN ... DO ... TO` is allowed only on a row Thing, and the profile must
  have `things_write=1`. Inside `DO`, outside quotes:
  - the word `KEY` is replaced by the Thing's key. `ON DUPLICATE KEY`,
    `PRIMARY KEY` and similar are left alone.
  - `THING.<label>` is replaced by that value of the Thing that acted, as it
    is when it acts: a number as itself, text quoted, NULL as `NULL`. Write
    `` THING.`a label` `` for a label with a space in it. `THING` is the same
    name the condition sees its values under (`WHEN thing.picked >= thing.qty`).
    A label the Thing does not have is refused at `CREATE`, with the labels it
    does have. A bare label (`VALUES (KEY, line_count)`) is sent as it is
    and means a column on the target.

  The act runs in one transaction on the
  profile, inside the statement that caused it. It fires once each time the
  condition becomes true, including for keys already true at `CREATE`. A
  failed act is recorded in the history and is tried again at the key's next
  change.

The parser also has `RESOURCE`, `CLAIMS` (allocation), `GRAIN` (a grain
program), `AT '<time>' GRAIN` (an event Thing, gated by `AIOS_THING_WRITES=1`)
and mirror kinds. Their shapes are in the error strings at
`sql_things.cpp:2805`, `2839`, `4168` and `4802`. **None of these were driven
here.**

The other statements:

    SHOW THINGS                         every kind
    SHOW THINGS <name> | SHOW THING <name>       every Thing of a kind
    SHOW THING HISTORY <name> '<key>'   what changed it and why
    SHOW THING <name> KEYS ('k', ...)   only those Things of a grain kind, in the order asked (collation match)
    SHOW THING <name> JSON [SINCE n]    one JSON document per Thing; SINCE = only newer stamps (+ keys that left)
    SHOW THING <name> CLOCK             the stamp a client holds after reading everything
    WAIT THING <name> SINCE <clock> [TIMEOUT <s>]   returns when the kind's clock moves (see below)
    SHOW THING ALERTS [name]
    SHOW THING CONFLICTS <name> ['<key>']   each side of each field the sources disagree on
    RESOLVE THING <name> '<key>' <field> TAKE '<source>' | UNRESOLVE
    SHOW THING CONFLICT GROUPS <name> [BY CHANGE]   conflicts that share a cause
    RESOLVE THING <name> * <field>|* TAKE '<source>' | UNRESOLVE | PREFER '<source>'[, ...] | NOPREFER
    CHECK THING <name> ADD|DROP UNIQUE <field> | ADD|DROP TOGETHER <field>, <field>[, ...]
    CHECK THING <name> [APPLY]          are the picks consistent? (SAT; a clash names its minimal cause)
    PAUSE THING <name> | RESUME THING <name> | REFRESH THING <name>
    DROP THING <name>

Results from Thing statements carry an extra `machine` column (this engine's
short identity), because the same statements can be sent `ON <machine>` across
a fleet.

To see a running engine's Things from a shell without connecting to it, use
`grain ps`: it lists each engine on the machine with its Things and their row
counts (see [What's running on this machine](grain-apps.md#whats-running-on-this-machine-grain-ps)).

### `WAIT THING`: told when a Thing changes

`WAIT THING <name> SINCE <clock> [TIMEOUT <seconds>]` returns as soon as the
kind's clock differs from `<clock>`, with one row: `clock` (the new one, to
pass as the next `SINCE`), `changed` (a JSON list of the keys whose Things
changed since, for a grain kind) and `whole` (1 when every Thing may have
changed: `SINCE 0`, a `CREATE OR REPLACE` that gave the kind a new clock, or a
kind that is not grain). `SINCE 0` answers at once with the current clock. With
nothing changed by the timeout (default 30 s, at most 3600) it returns the same
clock and `[]`.

Nothing polls. The engine wakes its waiters when it has committed a change - at
the end of a settle or tick in which a clock moved, after a Thing statement, and
when a replacement takes the name (`ThingEngine::wake_waiters`,
`sql_things.cpp`). The statement runs with no statement lock (it is routed like
`CREATE OR REPLACE THING`, `sql_database.cpp`), so a waiter never holds up a
write or the reaction it is waiting for; it holds only its own connection.
Inside a transaction it is refused.

Driven on a local server (2026-10-27): one client ran `WAIT THING per_who SINCE
2 TIMEOUT 20`; another inserted a scan 1.5 s later (9 ms). The waiter returned
11 ms after the insert with `3 | ["ann"] | 0`. Twenty waiters woke on one insert
(13 ms) with the same answer, a `TIMEOUT 1.5` returned the same clock after 1.5 s,
and a `CREATE OR REPLACE` woke its waiter with `whole = 1`.

### Grain Things

`CREATE THING <name> GRAIN KEY <column> READS <tables> [WATCH <table>(<column>)[, ...]] AS '<program>'`
keeps a program's answer per key. The program defines `enroll()` (the keys that may be Things),
`shared()` (what every key's answer depends on) and `things(keys, shared)` (a map of key to a map of values;
a key it leaves out is not a Thing), and optionally `touched(table, op, before, after, shared)`, which says
what a change to a table means: `nil`, a list of keys, `"all"`, or `{shared: true}`. A key's answer replaces
the old one, so replaying after a crash lands on the same values. `SHOW THING <name>` reads them (a column
per value). `apps/stickerfeed/things/sticker_item.gr` is a full one (50,000 rows kept for the sticker feed).

`CREATE OR REPLACE THING <name> GRAIN ...` gives a grain Thing a new program without taking it away: the new
version is built beside the old one (as `<name>~next`, paused but noting every change), its first pass runs with
no statement lock held - the old Thing is read, and tables are written, all the while - and then it takes the
name and follows the changes made meanwhile through its `touched()`. A version that does not load or build is
refused and the old one stays. A program of several files is sent as one text: an app's deploy hook gives it
`app_program("things/x.gr")`, the file with its imports in place (see `apps/stickerfeed/deployed.gr`), so the
Thing runs the same functions as the service it serves.

In the program: `read(sql, args...)` runs a SELECT inside the engine (each `?` takes the next argument; a
list fills an `IN (?)`) and gives records with numbers as numbers; `read_raw(sql, args...)` is the same with
every value as the text the wire would carry, for a port of code written against the wire. `read_with(sql, params)`
and `read_raw_with(sql, params)` take the `?` values as one list (`nil` for none), as does `read_raw(sql, ...params)`.
Every table either
touches must be in `READS`. The builtins a program may name are fixed, as its answers must not depend on
the clock or the machine: the text, list and math ones, `json_parse`, `json_stringify`, `sort_by`, `upper_rtrim`,
`format`, the `regex_*` set (`regex_test`, `regex_match`, `regex_find_all`, `regex_replace`, `regex_split`), and the exact-decimal set (`decimal`, `dec_div`, `scale`, `decimal_prefix`, `round_sig`, `number_format`, `is_numeric`, `to_number`, `idiv`), and `group_by`, `key_by`, `sum_by`; `is_blank`, `first_present`, `get`, `get_in`, `get_or_put`, `add_keys`, `add_into`, `date_range`, `day_add`, and the markup set
(`html_escape`, `xml_escape`, `el`, `html_raw`, `html_stringify`, `xml_stringify`). `min` and `max` take strings and pass over `nil`
(`last = max(last, t)`), and `sort_by` takes a list as a key, compared item by item (`[-u.touches, u.userid]`).
The statement's quoting reads backslashes, so a program written for it has none (`chr(9)`, not `"\t"`).

### Conflicts: `SOURCE BY` keeps both sides

When several systems feed the same Thing (a CRM and an ERP both send a
customer's email), the last write normally wins. `SOURCE BY <column>` on a row
Thing keeps every source's row instead. The `GROUP BY` must include the source
column. Each key then has one side per source. A field on which every source
agrees takes that value. A field on which they disagree is a **conflict**: its
value is NULL until someone takes a side, and both sides stay visible.

```
CREATE THING customer KEY id SOURCE BY origin READS customer_feed
  AS SELECT id, origin, email, phone FROM customer_feed GROUP BY id, origin

SHOW THING CONFLICTS customer
| key | field | source  | value        | taken |
| 2   | email | billing | robert@z.com | 0     |
| 2   | email | crm     | robert@z.com | 0     |
| 2   | email | erp     | bob@x.com    | 0     |
| 2   | phone | ...     | ...          | 0     |
| 3   | phone | crm     | 777          | 0     |
| 3   | phone | erp     | 778          | 0     |

RESOLVE THING customer '2' email TAKE 'erp'     -- email = bob@x.com from now on; taken = 1 on erp
RESOLVE THING customer '2' email TAKE 'billing2'  -- refused: names the sources there are
RESOLVE THING customer '2' email UNRESOLVE       -- back in conflict
```

- `SHOW THINGS customer` shows a conflicted field as `email={crm: ..; erp: ..} (conflict)`
  or `-> erp` once taken. `SHOW THINGS` lists the conflicts per Thing, and
  `SHOW THING customer JSON` carries `"_conflicts":[{field, sides, took}]`.
- A pick is recorded in the history (`resolved email: took erp`) and saved with
  the Thing, so it survives a restart.
- When the sources come to agree, the conflict and its pick are cleared. If
  they diverge again later, the field is a conflict again with no stale pick.
- `SOURCE BY` is allowed only on row Things (key = partition).

From grain, `thing_conflicts(kind, [key], [port], [opts])` returns these
conflicts as `{key, field, taken, by, sides: [{source, value, taken}]}` (`by` is
"hand" or "rule"), and
`draw_conflict(canvas, x, y, w, h, conflict)` draws one side by side, with a
"Take <source>" toggle per side. It returns the toggles' hit rectangles.
`examples/grain/conflicts.gr` is the whole app. It lists a Thing's conflicts
as panes, and a click on a toggle runs `RESOLVE THING .. TAKE`:

```
ASQL_PORT=33071 ASQL_PASSWORD=secret ASQL_DB=crm THING=customer CLICK=erp \
  aios grain run examples/grain/conflicts.gr
2 email: billing=robert@z.com, crm=robert@z.com, erp=bob@x.com  (in conflict)
3 phone: crm=777, erp=778  (in conflict)
took erp for 2 email
after: 2 email  (took erp)
```

Headless, it saves the frames before and after the click
(`conflicts-before.ppm`, `conflicts-after.ppm`). `asql_query` and `asql_exec`
take the same `{user, password, database}` options map as their third
argument.

### Settling conflicts together, and checking the picks

Resolving conflicts one by one does not scale, and many share a cause: the
same two sources disagree on the same field for many customers, or one feed
load raised them all. `SHOW THING CONFLICT GROUPS` groups them by field and by
which sources side together. `BY CHANGE` groups them instead by the change
that made each field a conflict (the history entry where it became one):

```
SHOW THING CONFLICT GROUPS customer
| field | sides             | conflicts | open | by_hand | by_rule | keys       | raised_by                           |
| email | crm | erp         | 4         | 4    | 0       | 0       | 4, 5, 6, 7 | customer_feed insert id 4 (+3 more) |
| phone | crm | erp         | 2         | 2    | 0       | 0       | 3, 6       | customer_feed insert id 2 (+1 more), ... |
| email | billing,crm | erp | 1         | 0    | 1       | 0       | 2          | customer_feed update id 2           |

SHOW THING CONFLICT GROUPS customer BY CHANGE
| raised_by                           | conflicts | open | by_hand | by_rule | fields       | keys       |
| customer_feed insert id 4 (+3 more) | 5         | 5    | 0       | 0       | email, phone | 4, 5, 6, 7 |
```

One decision then settles a whole group:

- `RESOLVE THING customer * email TAKE 'erp'` picks erp on every email
  conflict that erp reports. Things erp doesn't report are listed with
  "erp does not report it" and left alone.
- `RESOLVE THING customer * email UNRESOLVE` undoes those picks.
- `RESOLVE THING customer * email PREFER 'crm', 'erp'` is a **standing rule**.
  Every email conflict, now and whenever a new one arises, takes the first of
  those sources that reports the Thing. A pick by hand still wins over the
  rule. Field `*` is a rule for every field that has no rule of its own.
  `NOPREFER` drops the rule.

Each one is a single entry in every affected Thing's history (`rule: email
prefers crm`). A Thing a rule settles shows `-> crm (by rule)` there, and
`SHOW THING CONFLICTS` marks it `taken=1, how=rule`. Rules are saved with the
kind and survive a restart.

**Checking the picks against each other.** Picks and rules can be fine on
their own and wrong together. Constraints say what must hold across them:

```
CHECK THING customer ADD UNIQUE email              -- no two customers end up with one email
CHECK THING customer ADD TOGETHER email, phone     -- a customer's email and phone come from one source
CHECK THING customer                               -- consistent?
CHECK THING customer APPLY                         -- ... and take a consistent choice for every open conflict
```

`CHECK THING` builds a SAT problem:

- one variable per (Thing, field, side) of every conflict on a constrained
  field, with exactly one side taken;
- every pick and rule as a unit clause;
- UNIQUE as "not both", for any two Things that would end with the same value;
- TOGETHER as "the side taken for one field brings that source's value for the
  other".

It is solved with `solve_verified`. On SAT, a choice for each open conflict
comes back, its model checked against every clause, and `APPLY` takes it. On
UNSAT, the minimal unsat core is translated back into the picks, rules and
constraints that cannot all hold, and verified as for `CALL SAT`. From the
drive here, a rule preferring crm gave customer 8 the email customer 5
already had:

```
| CLASH | these cannot all hold (minimal core of 3 clauses: refuted (proof verified), and each one needed (a checked model of the rest)): |
| CLASH | rule: email prefers crm (customer 5)                             |
| CLASH | rule: email prefers crm (customer 8)                             |
| CLASH | UNIQUE email: customer 5 and 8 cannot both have email eve@x.com  |
```

With the clash fixed by hand on one customer, `APPLY` picked phone from crm
for customer 6, because its email came from crm by rule (TOGETHER), and the
history says `check: took a choice consistent with UNIQUE email; TOGETHER
email, phone`. A pick that breaks a TOGETHER shows up the same way:
`pick: customer 6 email took erp`, `pick: customer 6 phone took crm`,
`TOGETHER email, phone: customer 6 takes email and phone from one source`.
Constraints are saved with the kind. `CHECK` reports a clash but does not
refuse the RESOLVE that caused it.

### Worked example: a running total per customer

The table is `docs.orders` from earlier. Use `-vv` to see the message a
`CREATE` returns.

```
$ mysql -h 127.0.0.1 -P 13410 -u x --table docs -e "CREATE THING cust_totals KEY customer READS orders
    AS SELECT customer, SUM(amount) AS total, COUNT(*) AS n FROM orders WHERE status = 0 GROUP BY customer"
$ mysql ... -e "SHOW THINGS"
+-------------+--------+---------+-----------+-----------------+------------+------------------+----------------------------------------------+---------------------+--------+----------+--------------+
| thing       | things | version | reactions | full_recomputes | partitions | links            | waiting_for                                  | declared            | state  | host     | machine      |
+-------------+--------+---------+-----------+-----------------+------------+------------------+----------------------------------------------+---------------------+--------+----------+--------------+
| cust_totals | 2      | 2       | 0         | 1               | 2          | orders(customer) | a change to orders (the customer it touches) | 2026-10-01 17:59:44 | active | arch-qbc | 123c811b5114 |
+-------------+--------+---------+-----------+-----------------+------------+------------------+----------------------------------------------+---------------------+--------+----------+--------------+
$ mysql ... -e "SHOW THING cust_totals"
+----------+-------+------+---------+------------+---------------------+-------------+-----...
| customer | total | n    | version | partitions | updated_at          | last_change | ...
+----------+-------+------+---------+------------+---------------------+-------------+-----...
| acme     | 11.00 | 1    | 1       | 1          | 2026-10-01 17:59:44 | created     | ...
| globex   | 99.00 | 1    | 1       | 1          | 2026-10-01 17:59:44 | created     | ...
```

(Order 1's amount had been changed to 11.00 earlier. Order 2, `acme` 4.25, has
`status = 1`.)

Change rows and read it again:

```
$ mysql ... -e "INSERT INTO orders (customer, amount, status) VALUES ('acme', 5.00, 0), ('initech', 7.00, 0);
                UPDATE orders SET status = 0 WHERE id = 2;"
$ mysql ... -e "SHOW THING cust_totals"
| customer | total | n    | version | partitions | updated_at          | last_change                           |
| acme     | 20.25 | 3    | 3       | 1          | 2026-10-01 17:59:49 | orders update customer acme           |
| globex   | 99.00 | 1    | 1       | 1          | 2026-10-01 17:59:44 | created                               |
| initech  | 7.00  | 1    | 1       | 1          | 2026-10-01 17:59:49 | orders insert customer acme (+1 more) |
$ mysql ... -e "SHOW THING HISTORY cust_totals 'acme'"
+---------------------+---------------------------------------+-------------------+-------------------+
| at                  | cause                                 | before            | after             |
+---------------------+---------------------------------------+-------------------+-------------------+
| 2026-10-01 17:59:44 | created                               |                   | total=11.00 | n=1 |
| 2026-10-01 17:59:49 | orders insert customer acme (+1 more) | total=11.00 | n=1 | total=16.00 | n=2 |
| 2026-10-01 17:59:49 | orders update customer acme           | total=16.00 | n=2 | total=20.25 | n=3 |
+---------------------+---------------------------------------+-------------------+-------------------+
```

A key whose rows all stop matching stops being a Thing:

```
$ mysql ... -e "UPDATE orders SET status = 1 WHERE customer = 'globex'"
$ mysql ... -e "SHOW THING cust_totals JSON"
| key     | stamp | state | json                                                                                        |
| acme    | 5     | live  | {"customer":"acme","total":"20.25","n":"3","_version":3,"_updated":"2026-10-01 17:59:49"}   |
| initech | 4     | live  | {"customer":"initech","total":"7.00","n":"1","_version":1,"_updated":"2026-10-01 17:59:49"} |
```

### Worked example: a row Thing that acts

`connections.ini` holds the `sink` profile shown earlier: a second ASQL on
13411 with `things_write=1`. It has a table
`sinkdb.ready_orders (orderno INT PRIMARY KEY, n INT)`. The orders' lines are
in `docs.lines (orderno, line, qty, picked, price)`.

```
$ mysql ... -vv -e "CREATE THING order_pick KEY orderno PARTITION BY orderno READS lines
    AS SELECT orderno, COUNT(*) AS line_count, SUM(qty) AS qty, SUM(picked) AS picked
       FROM lines GROUP BY orderno
    WHEN picked >= qty DO INSERT INTO ready_orders (orderno, n) VALUES (KEY, 1) TO sink"
Thing order_pick created: 2 things over 2 partitions.
$ mysql -h 127.0.0.1 -P 13411 -u docs sinkdb -e "SELECT * FROM ready_orders"
orderno	n
100	1                      <- already fully picked at CREATE: it acted then
$ mysql ... -e "UPDATE lines SET picked = qty WHERE orderno = 200"
$ mysql -h 127.0.0.1 -P 13411 -u docs sinkdb -e "SELECT * FROM ready_orders"
orderno	n
100	1
200	1
$ mysql ... -e "SHOW THING HISTORY order_pick '200'"
| 2026-10-01 18:01:03 | created                  |                                 | line_count=1 | qty=5 | picked=0 |
| 2026-10-01 18:01:04 | lines update orderno 200 | line_count=1 | qty=5 | picked=0 | line_count=1 | qty=5 | picked=5 |
| 2026-10-01 18:01:04 | acted                    | WHEN picked >= qty              | DO INSERT INTO ready_orders (orderno, n) VALUES (KEY, 1) TO sink |
```

`PAUSE THING x` records changes but does not react. `RESUME THING x` re-reads
the kind in full. `REFRESH THING x` forces a full re-read.

---

## Cells

A cell set keeps a few small values per key of one table, and updates them
from each changed row. No query runs on the row path. It stores the values
only, not the rows. A bit set acts when it rises or falls. Sets are saved in
`<store>/cells/<name>.cells` (the declaration plus what each key's acts last
saw). Values are rebuilt from the table at start.

### Syntax

From `CellEngine::parse_create` (`sql_cells.cpp:250-395`):

```
CREATE CELLS <name> ON [db.]<table> KEY <column> (
    <cell> COUNT [WHEN <condition>],
    <cell> SUM(<expr>) [WHEN <condition>],
    <cell> MIN(<expr>) [WHEN <condition>],
    <cell> MAX(<expr>) [WHEN <condition>],
    <cell> BIT ALL|ANY|NONE <condition>,
    ...
)
[ACT EXISTING]
[ON RISE|FALL <bit> DO <statement>[; ...] TO <profile>]...
[ON RISE|FALL <bit> NOTIFY '<http(s) url>' | 'local:<handler>']...

SHOW CELLS                        every set: keys, bits on, bytes, changes, acts
SHOW CELLS <name>                 values per key
SHOW CELLS <name> WHERE <bit>     only keys with that bit on
SHOW CELLS <name> KEY '<key>'     one key
SHOW CELLS <name> ACTS            the last acts: when, key, which edge, ok / error
DROP CELLS <name>
```

- `COUNT` is written bare: `COUNT(*)` is refused.
- `BIT ALL c` means the key has rows and every one meets `c`. `ANY c` means at
  least one does. `NONE c` means it has rows and none does. A set holds at most
  64 bits.
- An edge is judged at the end of the statement or the replicated transaction,
  so a bit that flickers inside one transaction does not act.
- Acts run on the set's own worker thread, outside the statement.
- `DO ... TO <profile>` runs its statements in one transaction on the profile,
  with `KEY` replaced as for Things. The profile must exist and have
  `things_write=1` when you run `CREATE`.
- `NOTIFY` POSTs JSON of the edge and the key's current values. A non-2xx
  answer counts as a failure.
- `NOTIFY 'local:<handler>'` hands the same JSON to a handler registered in
  the same process (`CellEngine::register_local`), on the set's worker
  thread, with no network. A program that runs ASQL in process (converge's
  engine does) reacts to its own data this way. No handler, or one that
  returns false, counts as a failure.
- By default, a bit already on at `CREATE` counts as acted on. `ACT EXISTING`
  acts on those too.
- A failed act leaves the bit un-acted, so the key's next change tries again.
- Queued acts are journaled with the set and run again after a crash. An act
  can therefore repeat: make `DO` statements idempotent, and drop repeated
  `id`s in a `NOTIFY` receiver.

### Worked example: picking progress per order, with NOTIFY

The listener is a ten-line Python `BaseHTTPRequestHandler` on 13412 that
prints each POST body.

```
$ mysql ... -vv -e "CREATE TABLE lines (orderno INT, line INT, qty INT, picked INT DEFAULT 0, price DECIMAL(10,2), PRIMARY KEY (orderno, line));
INSERT INTO lines VALUES (100,1,2,0,1.50),(100,2,1,0,3.00),(200,1,5,5,2.00);
CREATE CELLS picking ON lines KEY orderno (
  n       COUNT,
  done    COUNT WHEN picked >= qty,
  value   SUM(qty * price),
  biggest MAX(qty),
  ready   BIT ALL picked >= qty,
  started BIT ANY picked > 0
) ON RISE ready NOTIFY 'http://127.0.0.1:13412/edge'
  ON FALL ready NOTIFY 'http://127.0.0.1:13412/edge';"
...
cells picking on docs.lines: 2 keys, 1 with a bit on, built in 0 ms, 540 bytes

$ mysql ... -e "SHOW CELLS picking"
+---------+------+------+------+-------+---------+-------+---------+
| orderno | rows | n    | done | value | biggest | ready | started |
+---------+------+------+------+-------+---------+-------+---------+
| 100     | 2    | 2    | 0    | 6.00  | 2       | 0     | 0       |
| 200     | 1    | 1    | 1    | 10.00 | 5       | 1     | 1       |
+---------+------+------+------+-------+---------+-------+---------+
```

Pick order 100 one line at a time, then add a line to it:

```
$ mysql ... -e "UPDATE lines SET picked = 2 WHERE orderno = 100 AND line = 1"
$ mysql ... -e "SHOW CELLS picking KEY '100'"
| 100     | 2    | 2    | 1    | 6.00  | 2       | 0     | 1       |
$ mysql ... -e "UPDATE lines SET picked = 1 WHERE orderno = 100 AND line = 2"
$ mysql ... -e "SHOW CELLS picking WHERE ready"
| 100     | 2    | 2    | 2    | 6.00  | 2       | 1     | 1       |
| 200     | 1    | 1    | 1    | 10.00 | 5       | 1     | 1       |
$ mysql ... -e "INSERT INTO lines VALUES (100,3,4,0,0.25)"
$ mysql ... -e "SHOW CELLS picking; SHOW CELLS picking ACTS"
| 100     | 3    | 3    | 2    | 7.00  | 4       | 0     | 1       |
...
+---------------------+------+------------+--------+
| at                  | key  | act        | result |
+---------------------+------+------------+--------+
| 2026-10-01 18:00:26 | 100  | RISE ready | ok     |
| 2026-10-01 18:00:26 | 100  | FALL ready | ok     |
+---------------------+------+------------+--------+
```

What the listener received:

```
/edge {"cells":"picking","key":"100","bit":"ready","edge":"rise","id":1,"values":{"n":"2","done":"2","value":"6.00","biggest":"2","ready":"1","started":"1"}}
/edge {"cells":"picking","key":"100","bit":"ready","edge":"fall","id":2,"values":{"n":"3","done":"2","value":"7.00","biggest":"4","ready":"0","started":"1"}}
```

### Worked example: DO ... TO a profile

```
$ mysql ... -vv -e "CREATE CELLS shipping ON lines KEY orderno (
  ready BIT ALL picked >= qty
) ACT EXISTING
  ON RISE ready DO INSERT INTO ready_orders (orderno, n) VALUES (KEY, 1) ON DUPLICATE KEY UPDATE n = n + 1 TO sink
  ON FALL ready DO DELETE FROM ready_orders WHERE orderno = KEY TO sink"
cells shipping on docs.lines: 2 keys, 1 with a bit on, built in 0 ms, 302 bytes
$ mysql -h 127.0.0.1 -P 13411 -u docs sinkdb -e "SELECT * FROM ready_orders"
orderno n
200     1                                  <- ACT EXISTING acted on the bit already on
$ mysql ... -e "UPDATE lines SET picked = 4 WHERE orderno = 100 AND line = 3"   # 100 becomes ready
$ mysql ... -e "UPDATE lines SET picked = 0 WHERE orderno = 200"                # 200 stops being ready
$ mysql -h 127.0.0.1 -P 13411 -u docs sinkdb -e "SELECT * FROM ready_orders"
orderno n
100     1
$ mysql ... -e "SHOW CELLS shipping ACTS"
| 2026-10-01 18:00:35 | 200  | RISE ready | ok     |
| 2026-10-01 18:00:36 | 100  | RISE ready | ok     |
| 2026-10-01 18:00:36 | 200  | FALL ready | ok     |
```

The parser's own error messages:

```
CREATE CELLS bad ON lines KEY orderno ( c COUNT(*) )
ERROR 1064 (42000): cell c: COUNT [WHEN c], SUM(e) [WHEN c], MIN(e), MAX(e) or BIT ALL|ANY|NONE c
CREATE CELLS bad ON lines KEY orderno ( r BIT ALL picked >= qty ) ON RISE r DO DELETE FROM x TO nowhere
ERROR 1064 (42000): TO nowhere: no such connection profile
SHOW CELLS picking WHERE n
ERROR 1064 (42000): SHOW CELLS picking WHERE <bit>: no bit n
DROP CELLS shipping
cells shipping dropped
```

After a SIGTERM and restart, `SHOW CELLS picking` came back with the same
values, and the listener received no repeated edges.

---

## Active data: filters, caches, links, views

Things and Cells keep answers current by key. Active data does it for the shapes a dashboard keeps asking:
a WHERE, a set of totals, a join, a GROUP BY. Each is declared once, built from the table, and then
adjusted by every row change - the before and after image of an INSERT, UPDATE or DELETE - so a read finds
its answer made. Definitions are saved under `<root>/active` and built again at start; after the redo log
replays, every object is rebuilt from the tables as they stand.

```sql
CREATE ACTIVE FILTER open_orders ON salesorders WHERE orderstatusid = 20 AND hold = 0;
CREATE ACTIVE CACHE today AS SELECT COUNT(*) AS n, SUM(total) AS revenue FROM salesorders WHERE trandate = '2026-10-20';
CREATE ACTIVE LINK lines_order ON salesorderdetails(orderno) REFERENCES salesorders(orderno);
CREATE ACTIVE VIEW by_rep AS SELECT salesman, COUNT(*) AS orders, SUM(total) AS revenue FROM salesorders GROUP BY salesman;
SHOW ACTIVE FILTERS | CACHES | LINKS | VIEWS | STATUS | TIERING;
DROP ACTIVE FILTER|CACHE|LINK|VIEW [IF EXISTS] name;
```

- **Filter.** A bitset of the rows meeting the condition. A SELECT whose WHERE is that condition (the same
  text, white space and the case of keywords aside) reads the bits and evaluates nothing. The condition is
  checked by the engine's own row evaluator, so NULLs and collations match a scan. INSERT and UPDATE set or
  clear one bit. Rows after a DELETE change position, so a DELETE, `OPTIMIZE TABLE`, a rollback that puts a
  table back, or an ALTER marks the filter stale; the next read that wants it rebuilds it once.
- **Cache.** One row of `COUNT(*)`, `COUNT(x)`, `SUM`, `AVG`, `MIN`, `MAX` over one table, with an optional
  WHERE. The same SELECT text is answered from it (a string literal's case counts: `'Open'` is not
  `'open'`). Sums are exact decimals, printed to the scale of their values; AVG carries four more places,
  as MariaDB's does. MIN and MAX follow inserts directly; when a delete or update removes the current
  extreme the cache reads the table once to find the next. No GROUP BY, DISTINCT, JOIN, alias or HAVING
  (use a view for GROUP BY).
- **Link.** Integer key columns of two tables, the parent unique. A join on exactly those columns follows
  the row numbers - child to parent, or parent to its children - instead of hashing or probing the keys.
  The usual join checks still run on the candidates, so inner and outer joins, extra ON conditions and
  WHERE answer as before. Text keys are not linked (collations and number/text comparison are the join's
  own business); a link that finds a non-integer or duplicate parent key reports `unusable` in
  `SHOW ACTIVE LINKS` and the join falls back to its index.
- **View.** A table named `name` holding `SELECT <group columns>, <aggregates> FROM t [WHERE ...] GROUP BY
  <columns>`. Every aggregate takes an alias (`AS name`), which becomes its column. A write adjusts the
  groups its rows belong to and the table is updated for those groups when the statement settles, the way
  Things and Cells react; the SELECT is not run again. Query the view like any table. Dropping the view's
  own table ends the view; a table with that name must not exist when the view is created.

Dropping or restoring a source table leaves its objects in place, idle, and they are rebuilt when the table
is back. `SHOW ACTIVE TIERING` lists, per table, how long it has been unwritten: after 5 seconds its delta
buffer is folded into the columns, after 60 seconds (tables up to a million rows) its columns are
re-encoded in place, rows keeping their positions. Both happen in the clock thread under the statement lock.

## Transactions: rollback, and running them side by side

**Writes side by side (table-scoped locks).** With `AIOS_TXN_CONCURRENT=1`
a single-table `INSERT`, `UPDATE` or `DELETE` whose text names no subquery,
join or second table runs under the shared statement lock with an
exclusive lock on its one table, so writes to different tables, and reads
of other tables, run beside it; a reader of the written table waits for
the writer. A statement whose table has triggers, Things or a view watching
it, or that turns out to need a second table, takes the exclusive statement
lock instead, as every other write (DDL, multi-table writes, ROLLBACK, CDC,
a checkpoint) does. `AIOS_TABLE_LOCKS=0` turns this off. Eight sessions of
sysbench write_only went from 1,418 to 3,775 transactions a second with it
(MariaDB 5,996 on the same box).

**ROLLBACK undoes only the transaction's own changes.** Each row change a
transaction makes is logged as it happens: the inserted row, the deleted row,
or a cell's value before and after. ROLLBACK applies the inverses newest first,
finding each row by its primary key (or by the whole row when there is none).
`ROLLBACK TO SAVEPOINT` and a failed statement's trigger frame do the same.

If another session has written a cell since:
- a number gets only this transaction's change taken off: the stock an invoice
  took goes back, and a sale made meanwhile stays made;
- anything else keeps the later value.

The undo statements fire no triggers and write no redo. `AIOS_UNDO_TRACE=1`
prints each one.

Before this, a transaction kept a copy of every table it touched (even ones it
only read) and ROLLBACK restored that copy. That threw away other sessions'
committed writes: driven against the ERP, a POS sale made while an invoice was
open vanished when the invoice rolled back. No session folds the redo log into
the store while any transaction is open, so uncommitted rows never reach disk.

**`AIOS_TXN_CONCURRENT=1`: no global transaction lock.** By default a
transaction holds the engine's one writer lock from BEGIN to COMMIT, including
every round trip its client makes. With this switch on (in the server's
environment; the packaged unit sets it), transactions run side by side, and
each row is owned by the transaction that writes it. In this mode BEGIN and
COMMIT share the statement lock like reads do, so one session's transaction
boundaries never drain the others' reads: with four sysbench sessions in
`BEGIN ... COMMIT` read-only transactions, the default lock serialized them to
a quarter of one session's throughput.

A `BEGIN` while this session's transaction is still open commits that one
first, as MariaDB does (an implicit commit); it used to be refused.

Here is who waits for whom:

| Who wants the row | It goes ahead when |
|---|---|
| a relative update (`col = col + k`, `col = col - k`, k a constant) | no other open transaction has taken the row: relative updates share it, and a rollback takes off exactly its own k |
| any other write (absolute SET, a new key, a delete), `SELECT ... FOR UPDATE` | no other open transaction has the row at all |
| `FOR SHARE` / `LOCK IN SHARE MODE` | like a relative update |
| an autocommit write | the same rules, without keeping the row |
| a read | always: it sees the latest value |

A statement that runs into a row another transaction holds behaves like this:
- it is taken back, waits for that transaction to end outside the statement
  lock, and runs again;
- a wait that closes a cycle is a deadlock: error 1213, and the waiting
  transaction is rolled back;
- after 50 seconds it gives up with error 1205.

An UPDATE that finds its row by the primary key takes the row before reading
it, as InnoDB locks the record. So
`SET x = IF(x = seen, new, x) WHERE pk = ...` waits for a transaction that holds
the row, instead of reading its uncommitted value. Locking reads are never
answered from the result cache.

Limits:
- rows of a table with no primary key have no owner;
- FOR UPDATE claims rows of the statement's first table only;
- a duplicate key against another transaction's uncommitted insert is an error
  at once (InnoDB would wait);
- reads see uncommitted values (READ UNCOMMITTED), as this engine's readers
  always have;
- the redo log records statements in commit order, so two sessions' absolute
  writes to one row could replay in a different order. Ownership prevents
  exactly that pairing.

Driven with the ERP's own `B2b::createInvoice` (`tools/erp-drive/invoice/`),
with a 0.5 ms round trip per statement, 8 workers invoicing 40 orders while a
POS terminal rang up sales:

|                     | writer lock (default) | `AIOS_TXN_CONCURRENT=1` |
|---------------------|-----------------------|-------------------------|
| 40 invoices         | 6.61 s                | 1.42 s                  |
| POS sale, p90 / max | 958 ms / 1066 ms      | 18 ms / 51 ms           |
| books               | exact                 | exact                   |

In both modes:
- 40 invoices with every fifth failing part way, alongside 300 POS sales, left
  every sale kept, no trace of the failed invoices, and stock exact to the unit;
- the state was identical after a restart;
- three clicks on one order (at once, or one after another) gave one invoice.

`tools/erp-drive/invoice/concurrency.php` drives each rule over real
connections.

## Durability: redo log, group commit, checkpoints

`AIOS_REDO_SYNC=0` makes a statement return once its record is in the log
file, with a thread putting the log on the medium every 200 ms - what
MariaDB's `innodb_flush_log_at_trx_commit=2` does. A crash can then lose
the last fifth of a second of writes; a mirror fed by CDC loses nothing,
because it re-reads them from the source's binlog from the position its
last checkpoint saved. The default (1) answers only once the record is on
the medium, and the numbers in `asql-benchmarks.md` are taken that way;
with `0` a single session's write transactions run twice as fast (1,924
against 929 sysbench write_only transactions a second on the benchmark
box).

Every write statement is appended to `redo.log` under the store root before it
is answered; the store's table files are rewritten at a checkpoint (every
20,000 statements or 64 MB of redo, and at shutdown), and a restart loads the
files and replays the log. A kill -9 after a statement's OK loses nothing;
rows of a transaction that had not committed are not replayed.

A checkpoint holds the exclusive lock only for its snapshot: the changed
tables' columns are copied and `redo.log` is synced and renamed to
`redo.g<N>.prev`. The copies of the last checkpoint are kept per table
(`AIOS_CHECKPOINT_POOL_MB`, default 512) and the next snapshot copies into
them: a dictionary arena only grows between compactions, so a copy that
shares the arena's generation takes the appended tail alone, and the
codes and null bitmaps are copied whole. Four 100k-row sysbench tables
(90 MB, 18 MB of text per table) snapshot in 3 ms that way, 60 ms copied
whole (the first checkpoint after a start, or past the pool's budget);
then the lock goes, and a thread serializes the copies, writes the table
files and the manifest for generation N, and deletes the rotated log. A
kill before the manifest lands leaves the old manifest and the rotated log,
and the next start replays every `redo.g<N>.prev` above the manifest's
generation, oldest first, then `redo.log` (tested by killing the server
inside the write: every acknowledged row is back, none twice). Writing the
files under the lock stalled every session 300-800 ms per checkpoint;
`EXPORT SNAPSHOT`, shutdown and replay still write inline and return once
the store is on the medium. Maintenance before the snapshot is bounded too:
zone-map segments a delete left without stats are refreshed for 8 ms a
checkpoint rather than 16 per column (130-150 ms), and a dictionary
compaction keeps the zone maps (they hold values, not codes) and builds the
compacted arena straight from the old one (6-15 ms a column, from 50).
The lock is held 12-14 ms per checkpoint under sysbench, the copy of its
files runs beside the statements, and the store a restart loads is
byte-for-byte what a full copy would have written (checked by checksumming
every column before a kill -9 and after the restart, pool on and off).

A big table that only had rows deleted or cells changed is not copied
whole either. Each table keeps a journal of its writes since the last
checkpoint: the rows a statement erased, the cells it set, the rows it
appended. At the checkpoint those writes go onto the end of the table's
file as one part (a "part of writes", checked by its own hash), with the
manifest recording the file's new valid length. A restart loads the
file's image and replays its parts in order through the table's own
erase, set and append paths. The journal is dropped, and the table written
whole as before, when any of these holds:

- the table changed in a way the journal does not record (TRUNCATE, or
  anything else that rewrites it other than by erase, set and append);
- the journal passed 64 MB;
- the rows appended as parts, or the rows erased and cells set as parts,
  would pass a quarter of the rows the file was written with (at least
  50,000);
- the file already holds 64 parts.

Maintenance that only changes how a column is held (narrowed codes, a
coded dictionary) does not drop it, since it changes no values.
`AIOS_STORE_NO_OPS_PARTS=1` turns this off. A table whose file already
holds parts of writes is then written whole at the next checkpoint, even
if nothing changed it, so a store can go back to a binary that predates
parts of writes: start once with the setting, let one checkpoint run
(or shut down cleanly), and the files are whole again.
`tools/asql-ops-parts-check.cpp` covers deletes, updates, writes to rows
an append part added, an unrecorded write, 70 checkpoints in a row, a
second reopen, and the fold back with the setting.

The log is written into space set aside ahead of it: a fresh `redo.log`
is preallocated to the log's maximum (64 MB, `fallocate`) and each record
is written at the log's offset, so a COMMIT's `fdatasync` flushes data
only. An append grew the file by every record, and each fdatasync then
committed the size change through ext4's journal as well as the data -
two flushes for one commit (the kernel's share of a write statement was
37 us, 24-32 now; a single session's write transactions went from 1,170 to
1,257 a second, level with MariaDB). The offset is taken and the bytes
written under one lock, so records land in order and no acknowledged
record ever sits beyond a hole; the space after the last record reads as
zeros, where replay stops. The write is on a descriptor the host keeps
open (an open and a close around every write before); the `fdatasync`
that makes it durable happens after the lock is released, and is shared: the
first statement waiting for a sync runs it, every statement that appended
before it started is covered by it, and the ones that arrive meanwhile share
the next. Inside a transaction only COMMIT waits (its record is the last),
and the transaction's records go out in one append at COMMIT. One sync per
statement, held under the writer lock, was a third of the server's time; a
checkpoint every 2,000 statements, rewriting the table under the lock,
stalled every session 180-540 ms twice a second under sysbench. A COMMIT
costs the medium's fdatasync (110-150 us on an ordinary virtual disk) plus
about 100 us; it used to cost two syncs, because a sync thread started one
a moment before the COMMIT's own record landed.

## Admission: query memory and connections

Eight dashboards sending their reports at the same moment used to build
eight sets of joins, groups and sorts side by side, and the process's peak
memory was eight reports' worth. Admission keeps it to a budget instead:

- **Query memory.** A statement a client sends reserves the memory of each
  large intermediate just before building it (the rows a scan or join
  keeps, a hash table over a joined table, group tables, projected rows, a
  sort's buffer, its result, and every view, derived table, window or union
  result it materializes). Where the reservations of all running statements
  would pass `AIOS_QUERY_MEMORY_MB` (default 2048; 0 = no limit;
  `SET GLOBAL asql_query_memory_mb = N`, `SELECT @@asql_query_memory_mb`), a
  read waits - in arrival order, before it allocates - until running ones
  finish. The statement that has held a reservation longest never waits, so
  one always finishes and the rest follow; a statement larger than the whole
  budget runs once it is the only one holding any. Everything a statement
  reserved is released when it ends. A point lookup or a small result
  reserves nothing (pieces under 1 MB are not counted) and never waits.
  Writes, statements inside a transaction, and the engine's own statements
  (replication, Things) are counted but never wait. A waiting statement shows
  `waiting for query memory` in `SHOW PROCESSLIST` and ends on `KILL QUERY`;
  no wait lasts more than 30 s (past that it proceeds over the budget, so a
  statement held up behind a lock it needs cannot hang).
  `SHOW STATUS LIKE 'Asql_query_memory%'`: the budget, what is reserved now,
  the peak, how many wait, how many have waited and for how long in all.
- **Connections.** Past `max_connections` (default 151, MariaDB's;
  `AIOS_MAX_CONNECTIONS`, `SET GLOBAL max_connections = N`) a new connection
  gets error 1040 `Too many connections` and is closed;
  `Connection_errors_max_connections` counts them.

`compute serve` and `sql serve` take `query_memory_mb=` and
`max_connections=`; `aios-engine` takes `--query-memory-mb=` and
`--max-connections=`. `aios-asql-admission-check` drives both over the wire:
eight reports against a budget one of them fills ran one after another (up
to seven waiting, every answer the same as alone) while point lookups beside
them never waited.

## Storage compression: table files and columns

Measured on a copy of the asql-cleve-live CDC mirror (1,703 tables, 17,575
columns, 1.18 billion cells), loaded fresh, against the build before this:

| | before | after |
|---|---|---|
| table files on disk | 2.42 GB | 963 MB |
| columns in memory (`estimate_memory_bytes`) | 2,340 MB | 1,819 MB |
| of which dictionary codes | 943 MB | 664 MB |
| of which dictionary entry text | 612 MB | 412 MB |
| zone maps | 76 MB | 53 MB |

Every column's cells, numeric readings, an equality and a range probe, and
a COUNT/MIN/MAX/SUM over every table answered the same before and after
(`aios-asql-store-verify digest`, compared field by field).

**Table files** (`*.tbl`) are written packed: the serialized table cut into
1 MiB chunks, each LZ4-compressed (or kept as is when that is no smaller)
with a 64-bit hash of its plain bytes, which loading checks; chunks decode
in parallel. Inside, the format is AQT4: AQT3 plus each column's segment
layout and coded dictionary. A store written before this loads as it is;
its tables are rewritten packed about 256 MB of files per checkpoint, so the
first checkpoint after an upgrade does not copy the whole store under the
lock. An older binary cannot read a packed or AQT4 file: it skips the
table. Going back to an older build means restoring a snapshot taken before
the upgrade.

**Columns.** Besides the encodings before (dictionary, RLE, FOR, scaled
decimal, FSST, LZ4 blocks):

- *SegmentFOR*, for INT columns: every 2048-row segment has its own base
  and bit width. A span wider than 32 bits (epoch milliseconds, 64-bit ids)
  had no frame-of-reference encoding at all; a column of sequential ids
  costs ~11 bits a row whatever its overall span. Chosen when FOR cannot
  hold the column or when it is 40% smaller than FOR. A text column of
  digits is never typed this way (it compares as text).
- *ALP*, for DOUBLE columns whose values print back as stored: per segment
  the power of ten that turns the most values into integers exactly
  (`k / 10^e` is the stored double, bit for bit), those integers
  frame-of-reference coded, the rest kept whole as exceptions. Chosen over
  eight bytes a row when it is under three quarters of that, and over a
  dictionary when under half of it.
- *Dictionary codes at the dictionary's width.* A column that took a write
  it could not hold encoded became a dictionary of 32-bit codes and, being
  written to, was never re-encoded: most of the mirror's dictionary columns.
  Each checkpoint's maintenance narrows such codes to 8 or 16 bits as the
  dictionary allows (writes widen them again as it grows).
- *Coded dictionaries.* A dictionary of 1 MB or more whose entries average
  32 bytes or more (JSON, notes, addresses) is coded with a symbol table
  trained on its own entries (up to 255 symbols of 1-8 bytes, each written
  as one byte), a few MB per checkpoint, kept only where it saves a fifth or
  more. An entry decodes on its own; new entries stay plain until they add
  up to 256 KB. Shorter entries (dates, codes) stay plain: they are the ones
  sorted and range-compared, which reads every entry per statement.
  `LIKE '%x%'` over a coded dictionary decodes it once per statement (the
  mirror's `po_options`: 11 ms before, 28 ms coded).
- *Zone maps* are 96 bytes a segment, not 136 (five fields nothing read are
  gone), and a text bound longer than 32 bytes is kept as its first
  characters; MIN/MAX from zone maps reads such a segment instead of trusting
  its bound.

Narrowing and coding run within `AIOS_STORE_MAINTAIN_MS` each (default 40)
of every checkpoint's maintenance, under the exclusive lock; on the mirror
they settled after nine checkpoints at a large budget. `AIOS_STORE_TRACE=1`
logs each step.

## Strict mode: `AIOS_SQL_STRICT=1`

Off by default: the MariaDB this engine mirrors runs with `sql_mode` empty,
and its applications' INSERTs lean on that (a NOT NULL column left out of an
INSERT is zero-filled, a 10-character value goes into a VARCHAR(5), 999 into
a TINYINT). With `AIOS_SQL_STRICT=1` in the server's environment a client's
INSERT or UPDATE is refused the way MariaDB's `STRICT_TRANS_TABLES` refuses
it, with MariaDB's message:

| value | answer |
|---|---|
| NULL into NOT NULL; a NOT NULL column with no DEFAULT left out | `Column 'x' cannot be null` / `Field 'x' doesn't have a default value` |
| more characters than `VARCHAR(n)` / `CHAR(n)` (counted as UTF-8 characters) | `Data too long for column 'x' at row 1` |
| outside `TINYINT` .. `BIGINT`, `UNSIGNED` below zero, past a `DECIMAL(p,s)`'s digits | `Out of range value for column 'x' at row 1` |
| text where a number goes | `Incorrect integer value: 'abc' for column 'x' at row 1` |
| a date not on the calendar (`2026-02-31`), an hour of 24 | `Incorrect date value: ... for column 'x' at row 1` |
| a value an `ENUM` / `SET` does not have | `Data truncated for column 'x' at row 1` |

`UNSIGNED` and the members of an `ENUM`/`SET` are now kept with the column
(`SHOW CREATE TABLE` shows `int(10) unsigned`, `enum('a','b')`). Rows that
arrive by CDC, replay or seeding are never checked: the source decided them.
Foreign keys are enforced as InnoDB enforces them, per session
`FOREIGN_KEY_CHECKS` (dumps turn it off for their load): a child row's key must
be in its parent (1452, NULL keys aside); RESTRICT / NO ACTION refuses deleting
or re-keying a parent row a child names (1451); CASCADE and SET NULL run as
statements of their own after the parent's change, logged and rolled back like
any other. The replication stream applies CASCADE / SET NULL (MariaDB does not
binlog what a cascade changed) but checks nothing; redo replay, seeds and a
parent table this store does not have are not checked either.
`AIOS_SQL_FK=0` turns enforcement off. A table with foreign keys writes under
the exclusive statement lock.

Keys and statements: UPDATE checks PRIMARY KEY and UNIQUE row by row as InnoDB
does (`SET id = id + 1` over 1, 2, 3 fails on the first row); a row write that
fails part way is taken back whole, inside a transaction too (the transaction
stays open), and its redo record is never written.

## Prepared statements, typed columns, DISTINCT

- `COM_STMT_PREPARE` of a SELECT answers with the result's column
  definitions, as MariaDB does. They come from running the statement with
  every placeholder NULL and `LIMIT 0`, once per statement text and database,
  kept until the next DDL. libmysqlclient and sysbench decide from this count
  whether a result set follows; without it the connection went out of sync on
  the second prepared statement. A SHOW or CALL still announces none.
- An INT column filled by INSERT is dictionary-coded text until a
  checkpoint gives it a typed encoding (once). Typed, a key compares as a
  number, is indexed in value order, and `BETWEEN` / `<` / `>` on it reads
  the index: `id BETWEEN a AND b` over 100k rows went from 5.2 ms to 0.14 ms.
- `SELECT DISTINCT` over one table without GROUP BY runs on the vectorized
  path (it fell to the join path, whose probe index every write invalidated:
  60 ms a call inside a write transaction), and streams its rows like any
  other single-table SELECT.
- Statement shapes: a plain SELECT's parsed tree is kept per thread under
  its shape - its tokens with every literal a placeholder - and the next
  statement of that shape (`WHERE id = 7`, then `= 9`; every execution of a
  prepared statement) has its literals written into the tree's literal
  nodes instead of being parsed again. Only shapes whose every literal token
  became a literal node or a LIMIT / OFFSET are kept: a subquery (its text is
  held verbatim, literals inside), an ORDER BY ordinal, a CAST width or a
  result column headed by its literal (`SELECT 1` is headed "1") parse every
  time. `AIOS_SQL_SHAPES=0` turns the cache off.
- The result cache answers a statement's exact text from the second time it
  is seen; `AIOS_SQL_CACHE_MB` is its budget (default 1024 MB) and `0` turns
  it off - what a benchmark of the engine itself, against a MariaDB with no
  query cache, wants. The numbers in `asql-benchmarks.md` are taken with it
  off. The cache is 16 shards by key hash, each with its own lock: a miss
  stores its answer and its wire packets under a shard's exclusive lock,
  and with one lock four sessions of never-repeating point lookups ran at
  half the engine's speed (78k a second against 151k with the cache off),
  waiting on each other's stores; sharded, 140k cold and 254k once the
  answers are in.

## What SQL works, and its gaps

These worked over the wire:

- `CREATE/DROP DATABASE`, `USE`, `CREATE TABLE` with `PRIMARY KEY`
  (composite too), `AUTO_INCREMENT`, `DEFAULT`
- `CREATE VIEW`
- `INSERT ... VALUES (...), (...)`, `INSERT ... ON DUPLICATE KEY UPDATE col = VALUES(col)`
- `UPDATE`, `DELETE`
- `BEGIN` / `UPDATE` / `ROLLBACK` (the change was undone)
- `SELECT` with `WHERE`, `GROUP BY`, `ORDER BY`, `LIMIT n OFFSET m`
- joins: `[INNER] JOIN`, `LEFT`/`RIGHT`/`FULL [OUTER] JOIN`, `CROSS JOIN`, and
  - comma joins, `FROM a, b WHERE a.id = b.a_id`: the WHERE's predicates over two
    sources become the join's condition, so they join by hash (a 50k x 50k
    `CROSS JOIN ... WHERE` took 242 s before; as ON, 15 ms)
  - `JOIN t USING (a, b)` and `NATURAL [LEFT|RIGHT] JOIN`; `SELECT *` lists each
    shared column once, first, as MySQL does
  - `STRAIGHT_JOIN` (as the join keyword and as a query modifier) and index hints
    `USE`/`FORCE`/`IGNORE {INDEX|KEY} [FOR ...] (...)`, accepted and ignored:
    the engine picks its own plan
  - nested groups, `a JOIN (b JOIN c ON x) ON y`; a `LEFT`/`RIGHT` join to a group
    runs the group as a derived table, so its rows are NULL-padded together
  - a view of several tables joined after other tables (`productlinepoints p JOIN
    stocktrans s ON p.lineid = s.lineid`): the view's tables join first
- multi-table writes: `UPDATE a JOIN b ... SET`, `UPDATE a, b SET ... WHERE`,
  `DELETE t1[, t2] FROM <tables> [WHERE]` (a target named by alias), and
  `DELETE FROM t1 USING <tables> [WHERE]`; each target needs a primary key
- correlated scalar subqueries
- `WITH` and `WITH RECURSIVE`
- `GROUP_CONCAT(... ORDER BY ... SEPARATOR ...)`; `JSON_ARRAYAGG([DISTINCT] x
  [ORDER BY ...])` and `JSON_OBJECTAGG(k, v)`
- `information_schema.VIEWS` (definition, definer, security, algorithm)
- `JSON_EXTRACT`, `DATE_FORMAT`, `DATEDIFF`, `REGEXP`, `CAST(... AS DECIMAL(p,s))`
- operators: bitwise `& | ^ ~ << >>` (64-bit unsigned, `~0` is
  18446744073709551615), `DIV`, `MOD`, `<=>`, `IS [NOT] TRUE|FALSE|UNKNOWN`,
  `BINARY` (makes `=`, `<`, `LIKE` ... compare bytes), `INTERVAL(N, n1, ...)`.
  A column with no alias is named by its expression as written (`1 + 2`), a
  quoted string by the string (`SELECT 'k'` is column `k`)
- JSON: `JSON_VALID`, `JSON_TYPE`, `JSON_KEYS`, `JSON_DEPTH`, `JSON_QUOTE`,
  `JSON_SET`/`INSERT`/`REPLACE`/`REMOVE`, `JSON_ARRAY_APPEND`/`ARRAY_INSERT`,
  `JSON_MERGE[_PRESERVE]`, `JSON_MERGE_PATCH`, `JSON_SEARCH`,
  `JSON_CONTAINS_PATH`, `JSON_EXISTS`; output laid out as MariaDB's (`{"a": 1}`),
  a document that is not JSON gives NULL
- `GET_LOCK`/`RELEASE_LOCK`/`IS_FREE_LOCK`/`IS_USED_LOCK`/`RELEASE_ALL_LOCKS`:
  held per connection, re-entrant, waited for with the timeout, released when
  the connection closes
- `AES_ENCRYPT`/`AES_DECRYPT` (aes-128-ecb, MySQL's default mode only),
  `ENCODE`/`DECODE`, `COMPRESS`/`UNCOMPRESS`, `SHA2`, `TO_BASE64`/`FROM_BASE64`,
  `CONV`, `MID`, `INSERT()`, `COT`, `BIT_COUNT`, `TIMEDIFF`, `MAKETIME`,
  `CONVERT_TZ` (`+hh:mm` offsets, `UTC`, `SYSTEM`; a named zone is NULL, as on a
  MySQL without zone tables), `NAME_CONST`, `DEFAULT(col)`, `SLEEP` (really
  waits), `BENCHMARK`, `FOUND_ROWS()` (with `SQL_CALC_FOUND_ROWS`: the count
  without the `LIMIT`), `CONNECTION_ID()` and `USER()` from the connection
- `EXPLAIN` (with an ASQL route column)
- `SHOW CREATE TABLE` (with secondary `KEY`s), `DESCRIBE`, `SHOW TABLES`, `SHOW STATUS`, `SHOW ENGINE ASQL STATUS`
- `information_schema.tables` and `SHOW TABLE STATUS`, with `CREATE_TIME` the
  moment `CREATE TABLE` ran, in the same clock and zone as `NOW()`, kept in the
  table's file across restarts (NULL for a table whose file predates it)
- stored programs: `CREATE PROCEDURE` / `FUNCTION` / `TRIGGER` / `EVENT`, then
  `CALL p(..)`, `SELECT f(..)`, triggers firing on INSERT/UPDATE/DELETE; listed
  by `SHOW PROCEDURE STATUS`, `SHOW FUNCTION STATUS`, `SHOW TRIGGERS`,
  `SHOW EVENTS`
- `FULL OUTER JOIN` (unmatched rows of both sides, NULL-padded)
- `SELECT ... FOR UPDATE` / `FOR SHARE` / `LOCK IN SHARE MODE` (see
  "Transactions": rows locked with `AIOS_TXN_CONCURRENT=1`)
- `UPDATE` / `DELETE` with `WHERE x IN (SELECT ...)`, and a (correlated) scalar
  subquery in `SET`
- `LAST_INSERT_ID()` and the inserted id in the OK packet (mysqli `insert_id`,
  PDO `lastInsertId()`): the first id the connection's last INSERT generated;
  `LAST_INSERT_ID(expr)` sets it (`UPDATE seq SET n = LAST_INSERT_ID(n + 1)`)
- `ROW_COUNT()`: what the connection's previous statement changed (-1 after a SELECT)
- window functions (below)
- `CREATE [UNIQUE] INDEX name ON t (cols)` (as `ALTER TABLE t ADD INDEX`)
- `SAVEPOINT`, `RELEASE SAVEPOINT`, `ROLLBACK TO SAVEPOINT` (what came after the
  savepoint is undone, row by row)
- `UNION` / `UNION ALL` / mixed, deduplicating as MariaDB does
- declared column types: `VARCHAR(20)`, `CHAR(3)`, `TEXT`, `BIGINT`,
  `TINYINT(1)`, `DATETIME` read back as declared in `SHOW CREATE TABLE`,
  `DESCRIBE`, `SHOW COLUMNS` and `information_schema.columns`; a primary key
  column is `NOT NULL`
- `EXPORT SNAPSHOT '<name>'` (tables, `things/` and `cells/`)
- `SHOW TABLE STATUS`: `Engine` `InnoDB`, `Row_format` `Dynamic` (what
  `information_schema.tables` and `default_storage_engine` say; a client that
  copies a structure to MariaDB writes that engine), with this engine's sizes

`DECIMAL` keeps its declared scale: a `DECIMAL(10,4)` 12.5 reads back
`12.5000`.

- **DATETIME and DATE comparisons.** Comparisons against text literals accept
  all truncated datetime shapes supported by MariaDB (`YYYY-MM-DD HH:MM:SS`,
  `YYYY-MM-DD HH:MM`, `YYYY-MM-DD HH`, `YYYY-MM-DD`, with or without `T`, and
  fractional seconds). Omitted time components default to zero (`17:00` parses
  as `17:00:00`), properly matching datetime and date values rather than
  degrading to text-as-number comparisons (`tools/asql-datetime-literal-check.cpp`).

### Window functions

`f(...) OVER ([PARTITION BY ...] [ORDER BY ...] [ROWS | RANGE frame])`,
`OVER w` with `WINDOW w AS (...)`, and `OVER (w ORDER BY ...)` building on a
named window.

- ranking: `ROW_NUMBER`, `RANK`, `DENSE_RANK`, `PERCENT_RANK`, `CUME_DIST`,
  `NTILE(n)`;
- offsets: `LAG` and `LEAD (expr [, n [, default]])`;
- values in the frame: `FIRST_VALUE`, `LAST_VALUE`, `NTH_VALUE(expr, n)`;
- aggregates over the frame: `SUM`, `AVG`, `COUNT(*)` / `COUNT(expr)`, `MIN`,
  `MAX`.

Frames:
- `ROWS` or `RANGE BETWEEN` any of `UNBOUNDED PRECEDING`, `n PRECEDING`,
  `CURRENT ROW`, `n FOLLOWING`, `UNBOUNDED FOLLOWING`;
- `RANGE` with an offset needs a single numeric ORDER BY key;
- the default frame is the whole partition without an ORDER BY, and up to the
  current row's last peer with one.

Windows are computed after WHERE, GROUP BY and HAVING, and before DISTINCT,
ORDER BY and LIMIT, so these all work:
- `RANK() OVER (ORDER BY SUM(qty) DESC)` in a grouped query;
- `x / SUM(x) OVER ()`;
- `ORDER BY rn`;
- `t.*, ROW_NUMBER() OVER (...)`;
- the top-N-per-group form,
  `SELECT ... FROM (SELECT ..., ROW_NUMBER() OVER (PARTITION BY g ORDER BY v DESC) rn FROM t) x WHERE rn <= 2`.

`AVG` of integers is a DECIMAL with four more places, as MariaDB returns it. A
window function in WHERE, HAVING, GROUP BY or ON is refused, as in MariaDB.

`tools/proof/asql/window-check.py PORT` runs 39 window queries on ASQL and on
SQLite over the same rows and compares every row: 39 of 39 match.

On 200,000 rows in 500 partitions, timed through PHP (mysqli) including
fetching every row:

| Query | Before tuning | Now |
|---|---|---|
| `ROW_NUMBER() OVER (PARTITION BY g ORDER BY v)` | 769 ms | 209-217 ms |
| `SUM(v) OVER (PARTITION BY g ORDER BY id)` | 980 ms | 246-254 ms |
| `LAG(v) OVER (ORDER BY id)` | 945 ms | 124-144 ms |
| `AVG(v) OVER (... ROWS BETWEEN 5 PRECEDING AND CURRENT ROW)` | 1,177 ms | 249-279 ms |
| the same SELECT with no window (for scale) | | 71-90 ms |

What changed to get there:
- **No intermediate table when it isn't needed.** When the select list only
  names columns and windows and there is no DISTINCT, the rows are put together
  and ordered directly. The intermediate table is only built for expressions
  over windows or DISTINCT, and its cells are no longer re-formatted.
- **Sorting on compact keys.** Sort and partition keys are plain number/text
  arrays, and numeric keys are sorted as packed records.
- **SUM/AVG/COUNT by prefix sums.** These use exact-decimal prefix sums, so any
  frame costs the same.
- **Running MIN/MAX are carried forward** from row to row.
- **Values that are only copied stay as they are.** LAG/LEAD/FIRST/LAST/NTH
  copy their cell unchanged.

`AIOS_VX_TRACE=1` prints each window query's split, e.g.
`window: rows=48ms windows=83ms direct=10ms`.

Refused or wrong:

| Statement | Result |
|---|---|
| a multi-table `DELETE` with `ORDER BY` or `LIMIT` | refused, as in MySQL |
| a USING / NATURAL join over a derived table or view written as `SELECT *` | refused: its columns cannot be told from its text |
| `INSERT` / `UPDATE` / `DELETE` through a view | refused (`The target table v of the UPDATE is not updatable`); write the base table |
| `SHOW REPLICA STATUS` with no replication configured | empty, as MariaDB answers |
| a window function inside another, or `COUNT(DISTINCT x) OVER (...)` | refused, as in MariaDB |

A `SELECT` the engine cannot run is forwarded to a configured downstream
(`execution_route = BOUNCED_TO_MYSQL`). With no downstream, the error comes
back to the client.

---

## Running it as a service
`asql-serve --print` and `asql-export.sh` were driven.

`packaging/aios-compute.service.in` is `Type=simple`, `User=@RUN_USER@` (the
installer uses `aios`), `WorkingDirectory=/opt/aios`,
`Environment=AIOS_NO_ACCEL=1`, `Environment=AIOS_TXN_CONCURRENT=1` (see
"Transactions"), `ExecStart=/opt/aios/bin/asql-serve`,
`Restart=always`, `RestartSec=2`, `NoNewPrivileges=true` and
`ProtectSystem=full`. The installer fills in `MemoryMax` and `ReadWritePaths`
where the host's systemd supports them (>= 231).

`asql-serve` sources `/etc/aios/asql.conf` (or `$ASQL_CONF`) and execs
`aios --root $AIOS_ROOT exec "compute serve ..."`. It passes only the keys that
are non-empty. If the conf is unreadable it exits 78. `--print` shows the exact
command:

```
$ ASQL_CONF=/tmp/aios-docs-asql/test.conf packaging/asql-serve --print
/repos/aios/build/aios --root /tmp/aios-docs-asql/r9 exec "compute serve port=13419 lanes=8 sql=1 mysql_port=13420 mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=/tmp/aios-docs-asql/r7.pass cdc_tables=/tmp/aios-docs-asql/r9/cdc_tables.list"
```

The conf's keys are the `compute serve` keys in upper case:

- `RPC_PORT`, `LANES`, `SQL`, `SQL_DB`, `PUBLIC`, `MAX_POINTS`, `TOKEN_FILE`
- `MYSQL_PORT`, `MYSQL_BIND`, `MYSQL_USER`, `MYSQL_PASS_FILE`,
  `MYSQL_SOCKET`, `MYSQL_CHARSET`
- `CDC_HOST`, `CDC_PORT`, `CDC_USER`, `CDC_PASS_FILE`, `CDC_DB`,
  `CDC_TABLES` (a csv, or a path), `CDC_LOG_FILE`, `CDC_LOG_POS`

`FLEET_HOST`, `FLEET_PORT` and `FLEET_KEY_FILE`, when all three are set, run
`fleet join … retry` first: a hub that is down does not stop SQL from
starting (the join keeps trying in the background). They are in
`asql.conf.example`, empty. `MYSQL_DB` is read only by the console and top tools.
`packaging/asql.conf.example` is the template, and `packaging/README.md` and
`README-ASQL.md` cover the build, install and upgrade steps.

To restart the service and confirm it really restarted, compare the start
timestamp before and after:

    systemctl show -p ExecMainStartTimestamp --value aios-compute
    journalctl -u aios-compute -f

`asql-check [--quick | TABLE]` is the acceptance test. It checks:

- the unit's pid and start time
- that the socket answers as `MYSQL_USER`
- the binlog lag and per-table row and column counts, through
  `SHOW ASQL MIRROR`
- that every `.thing` on disk loaded

`asql-export.sh` asks the running engine for `EXPORT SNAPSHOT`, then packs the
staged copy and writes a `.sha256` beside it:

```
$ packaging/asql-export.sh --conf /tmp/aios-docs-asql/exp.conf -o /tmp/aios-docs-asql/asql-r1.tar.gz
== snapshot
   generation 3, staged at /tmp/aios-docs-asql/r1/sql_db/exports/export-20261001-180426
== pack
   4.0K  /tmp/aios-docs-asql/asql-r1.tar.gz
$ zstd -dc asql-r1.tar.gz | tar tf -
./tables/MANIFEST
./tables/646f63732e6c696e6573.g3.tbl
...
./schema_views.db
./things/cust_totals.thing
./things/order_pick.thing
```

The `-o` name picks the compression: `.tar.zst`/`.tzst` zstd (refused if
`zstd` is not installed), `.tar.gz`/`.tgz` gzip, `.tar` none; any other name is
refused. Without `-o` it is zstd when installed, else gzip. The snapshot
carries `things/` and `cells/`.

`asql-import.sh` on the target verifies the archive, stops the service, swaps
the store and starts it again. Driven with `--no-service`: an export holding a
cell set, imported into a fresh root, served `SHOW CELLS picking` with the
same bits.

---
