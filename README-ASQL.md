# ASQL — AIOS In-Memory Relational SQL Engine

**ASQL** is the high-performance, in-memory relational SQL database engine built natively into **AIOS** (All-In-One System) in dependency-free C++20.

It combines the raw speed of in-memory columnar indexing with broad MariaDB/MySQL dialect coverage (see [Engine Capabilities & Differences](#1-engine-capabilities--differences-from-mariadb--innodb)), wire-protocol compatibility, and real-time zero-copy MySQL Binlog CDC (Change Data Capture) replication.

---

## Key Highlights

- **Performance**: in-memory tables, a plan cache and a result cache; measure it against your own MariaDB with `tools/asql-diff-runner.mjs` (see "Measuring it" below).
- **Dependency-Free**: Pure C++20 implementation. No `libmysqlclient`, `libmariadb`, `sqlite`, or third-party query engines linked.
- **MySQL Wire Protocol Server (Port 3307)**: Native listener supporting standard MySQL clients, CLI, Navicat, DBeaver, TablePlus, DataGrip, PHP PDO, Python, Node.js, and Go.
- **Virtual `information_schema`**: Emulates MySQL system metadata tables dynamically for out-of-the-box GUI/IDE introspection.
- **Real-Time CDC Replication**: Zero-RAM-bloat MySQL Binlog v4 consumer with ingress table filtering and automatic position tracking.
- **Fast Correlated Subqueries & AST Cache**: Subquery parse tree caching, singleton `GROUP BY` fast-paths, and dynamic probe index acceleration.
- **ACID Transactions**: `BEGIN`, `COMMIT`, `ROLLBACK` with snapshot isolation and transaction rollback safety.
- **Dual Interface**: Speak standard MySQL wire protocol on port 3307 or raw length-prefixed JSON socket RPC on port 7311.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           CLIENT APPLICATIONS                           │
│   MySQL CLI · Navicat / DBeaver · PHP (PDO) · Python · Node.js · RPC    │
└───────────────────┬─────────────────────────────────┬───────────────────┘
                    │ MySQL Wire Protocol (port 3307) │ Raw RPC (port 7311)
┌───────────────────▼─────────────────────────────────▼───────────────────┐
│                               ASQL ENGINE                               │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ AST Parser & Execution Planner (Subquery Cache, CTE Evaluator)    │  │
│  └─────────────────────────────────┬─────────────────────────────────┘  │
│  ┌─────────────────────────────────▼─────────────────────────────────┐  │
│  │ In-Memory Data Store & Columnar Indices                           │  │
│  │  • PK / Unique Hash Index   • Probe Index (Hash & Sorted)         │  │
│  │  • Auto-Increment Tracker   • Dynamic info_schema Views           │  │
│  └─────────────────────────────────▲─────────────────────────────────┘  │
│  ┌─────────────────────────────────┴─────────────────────────────────┐  │
│  │ Multi-Tenant Database Namespace Isolation (versioneight, hairlines)│  │
│  └─────────────────────────────────▲─────────────────────────────────┘  │
│                                    │                                    │
│  ┌─────────────────────────────────┴─────────────────────────────────┐  │
│  │ Real-Time MySQL Binlog CDC Stream Consumer (Zero-RAM Ingress)     │  │
│  └─────────────────────────────────▲─────────────────────────────────┘  │
└────────────────────────────────────┼────────────────────────────────────┘
                                     │ COM_BINLOG_DUMP
                    ┌────────────────┴────────────────┐
                    │     Upstream MySQL / MariaDB    │
                    └─────────────────────────────────┘
```

---

## Installing ASQL on a server

ASQL ships as one tarball. Build it once, install it on any x86_64 Linux with
glibc 2.17 or newer (CentOS 7 / AlmaLinux 8 and 9 / Ubuntu), two commands.

### 1. Build the package (on arch, in a container — no build box needed)

```bash
cd aios   # the checkout
# the engine plus the two operator tools, linked against CentOS 7's glibc 2.17
TARGETS="aios-cli aios-asql-top aios-asql-console" packaging/build-centos7.sh origin/main build-centos7
packaging/pack.sh build-centos7 packaging/dist        # -> packaging/dist/aios-asql-<sha>-linux-x86_64-glibc2.17.tar.gz
```

`build-centos7.sh` builds a git ref (never the dirty working tree) in the
manylinux2014 image, about 8 minutes; `pack.sh` adds the scripts, the unit
template, the example conf, this README and a MANIFEST (sha256 + glibc floor).
`build-portable.sh` does the same on an el7 host over ssh when docker is not
available.

### 2. Install

```bash
tar xzf aios-asql-*.tar.gz && cd aios-asql-*/
sudo ./install-asql.sh --dry-run       # prints every change, makes none
sudo ./install-asql.sh                 # user aios, /opt/aios, /etc/aios/asql.conf, systemd unit, start
./install-asql.sh --host sn            # or: ship it over ssh and install there
```

The installer is idempotent: it backs up the binary it replaces, never
overwrites an existing `/etc/aios/asql.conf` or password file, load-probes the
new binary before swapping it in, and rolls back if the service does not come
up (judged by the unit's start timestamp, not `is-active`). `--no-service`
lays down the files, user and conf without systemd (a container, or a host you
run by hand); `--no-start` installs and enables but leaves the service alone.

What lands:

    /opt/aios/bin/aios                 the engine
    /opt/aios/bin/asql-serve           turns /etc/aios/asql.conf into the engine's arguments
    /opt/aios/bin/aios-asql-console    interactive SQL client (reads the conf: no flags needed)
    /opt/aios/bin/aios-asql-top        live monitor (same)
    /opt/aios/bin/asql-export.sh, asql-import.sh   move the data between hosts
    /opt/aios/root/sql_db/             the store (tables/, cdc_position, indexes.list, things/)
    /opt/aios/run/asql.sock            unix socket for same-host clients
    /opt/aios/cdc.pass  mysql.pass  compute.token     secrets, 0600 aios:aios
    /etc/aios/asql.conf                the only file that differs between hosts

### 3. Configure: `/etc/aios/asql.conf`

The first install seeds it from `packaging/asql.conf.example`; every later
install keeps it. Then write the two passwords:

```bash
sudo vi /etc/aios/asql.conf            # CDC_HOST, CDC_USER, CDC_DB, CDC_TABLES, MYSQL_USER
echo 'replication-password' | sudo tee /opt/aios/cdc.pass   >/dev/null   # the CDC_USER's MariaDB password
echo 'wire-password'        | sudo tee /opt/aios/mysql.pass >/dev/null   # what clients log in to ASQL with
sudo chown aios:aios /opt/aios/*.pass && sudo chmod 600 /opt/aios/*.pass
/opt/aios/bin/asql-serve --print       # the exact command the unit will run
sudo systemctl restart aios-compute
```

| Key | Default | Meaning |
|---|---|---|
| `AIOS_BIN`, `AIOS_ROOT` | `/opt/aios/bin/aios`, `/opt/aios/root` | engine and kernel root |
| `RPC_PORT` | `7311` | raw JSON RPC socket (`compute serve port=`) |
| `LANES` | `8` | worker lanes |
| `SQL`, `SQL_DB` | `1`, `sql_db` | enable the engine; store directory under the root |
| `PUBLIC` | `1` | `0` keeps the RPC port on loopback |
| `MYSQL_PORT`, `MYSQL_BIND` | `3307`, `127.0.0.1` | MySQL-wire listener; `0.0.0.0` to accept remote clients |
| `MYSQL_USER`, `MYSQL_PASS_FILE` | `erp`, `/opt/aios/mysql.pass` | the one wire account. Without a user the engine binds loopback only and takes any password |
| `MYSQL_SOCKET` | `/opt/aios/run/asql.sock` | unix socket; ~10 us less per statement than TCP for PHP on the same box |
| `MYSQL_CHARSET` | empty | `1` = honour each client's charset like MariaDB (a latin1 client gets cp1252 bytes) |
| `MYSQL_DB` | empty | default database for the console and top (else `CDC_DB`) |
| `TOKEN_FILE` | `/opt/aios/compute.token` | RPC auth token |
| `CDC_HOST`, `CDC_PORT`, `CDC_USER`, `CDC_PASS_FILE` | `127.0.0.1`, `3306`, none (required with `CDC_HOST`), `/opt/aios/cdc.pass` | the MariaDB to mirror; the user needs `REPLICATION SLAVE, REPLICATION CLIENT, SELECT` |
| `CDC_DB` | `versioneight` | the schema the whitelist names live in |
| `CDC_TABLES` | (list) | csv of tables, or `@/path/file` (one per line, `#` comments). Missing tables are copied from the source at start (one snapshot, position remembered per table), then the binlog stream continues from there. Empty = mirror every table the binlog mentions |
| `CDC_LOG_FILE`, `CDC_LOG_POS` | empty | pin a binlog position (a `mysqldump --master-data=2` snapshot); empty = the saved position, or the source's current one for a store that never streamed |
| `FLEET_HOST`, `FLEET_PORT`, `FLEET_KEY_FILE` | empty | all three set = also join the fleet hub (retrying in the background while it is down; SQL starts regardless); fleet peers may only read |

MariaDB must have `log_bin=ON` with `binlog_format=ROW` (`SHOW VARIABLES LIKE 'log_bin'`).

### 4. Verify

```bash
systemctl status aios-compute
journalctl -u aios-compute -f     # "compute cdc: streaming from 127.0.0.1:3306", "asql: warmed N column and M join indexes"
aios-asql-top -1                  # one snapshot: rows, tables, RSS, cache, CDC lag
aios-asql-console -e "SHOW STATUS LIKE 'Asql_%'"
aios-asql-console                 # interactive; SHOW DATABASES; USE x; SELECT ...
mysql -S /opt/aios/run/asql.sock -u erp -p"$(cat /opt/aios/mysql.pass)" versioneight
```

Both tools read `/etc/aios/asql.conf` (or `$ASQL_CONF`, or `--conf FILE`) for
the socket, user, password file and database, so on the installed host they
take no flags; `-S`, `-h`, `-P`, `-u`, `-p`, `-d` override, `--no-conf` ignores
the file. From another machine: `aios-asql-console -h 10.0.0.5 -P 3307 -u erp -p ...`
(needs `MYSQL_BIND=0.0.0.0` on the server).

### 5. Moving the data to another server

```bash
asql-export.sh -o /tmp/asql.tar.gz                 # on the source: EXPORT SNAPSHOT, packed, + .sha256
asql-import.sh /tmp/asql.tar.gz                    # on the target (ASQL installed): verifies, stops, swaps, starts
```

The export is a consistent snapshot taken by the running engine (hard-linked
generation, writers wait milliseconds). The import keeps an existing store
aside (`--force`) rather than deleting it and drops the source's replication
position unless `--keep-replication-state`. The `-o` extension picks the
compression (`.tar.gz` gzip, `.tar.zst` zstd, `.tar` none); with no `-o`, zstd
when installed, gzip otherwise (CentOS 7 has only gzip). Cell sets travel with
the tables and Things.

### 6. Upgrading

Build a new package, run the same `install-asql.sh`. The binary it replaces is
kept as `/opt/aios/bin/aios.bak-<stamp>` — delete old ones now and then, each
is 18 MB. Rollback is `cp` the backup over `bin/aios` and `systemctl restart`.

---

## Users and permissions: capability credentials

ASQL has one configured account (`MYSQL_USER`, the operator: the ERP and the
tools use it, full authority) and, beside it, **capability credentials** for
everything else - agents, scripts, a person who needs one table for ten
minutes. There is no `mysql.user` table, no GRANT and no root: a credential is
a bearer string that says what it may touch, with which rights, until when and
how many rows, and is signed with the machine's capability key
(`<root>/capability.key`, or `AIOS_CAPABILITY_KEY`; machines that must accept
each other's credentials share it).

```bash
# operator, over the wire (or `aios asqlcap mint ...` on the host)
CAPABILITY MINT 'shop.orders,shop.customers' RIGHTS 'read' TTL 600 ROWS 1000;   -- -> credential
CAPABILITY REVOKE '<credential>';                  -- takes effect at the holder's next statement

# anyone holding a credential can narrow it, keyless, offline
aios asqlcap attenuate <credential> tables=shop.orders rights=read ttl=60 rows=100

# the client: user "cap", the credential as the password
mysql -h 127.0.0.1 -P 3307 -u cap --enable-cleartext-plugin -p"<credential>" shop
```

### Named users

A credential is the authority; a **named user** is a login name and a password
standing in front of one, for a person or a program that wants `-u sg -psecret`
from an ordinary client (no clear-text plugin, nothing to paste). The session
it opens is a capability session holding that user's credential, nothing more.

```sql
-- operator, over the wire
CREATE USER 'sg' IDENTIFIED BY 'secret' TABLES 'shop.*' RIGHTS 'read,write,delete,execute,admin';
CREATE USER 'report' IDENTIFIED BY 'r' TABLES 'shop.orders,shop.customers' TTL 86400 ROWS 10000;  -- rights default to read
ALTER USER 'sg' IDENTIFIED BY 'newsecret';         -- password only
ALTER USER 'sg' RIGHTS 'read';                     -- re-grants (new credential, old one revoked)
SHOW USERS;                                        -- User, Tables, Rights, Expires, Rows, Status
DROP USER 'sg';                                    -- revokes; a live session ends at its next statement
```

```bash
mysql -h 127.0.0.1 -P 3307 -u sg -psecret shop     # any client, any driver

# the same file from the host (<root>/capability-users; the server re-reads it when it changes)
aios asqlcap user add sg password=secret tables=shop.* rights=read,write
aios asqlcap user password sg password=newsecret
aios asqlcap user list
aios asqlcap user del sg    # stops new logins; a session already open runs until it reconnects
```

A user's TTL defaults to never. The password is stored as its `mysql_native_password`
double-SHA1, the credential beside it, in `<root>/capability-users` (0600).
`cap` and the operator name are not user names.

* **Scope** is `db.table` or `db.*`. **Rights**: `read` (SELECT), `write`
  (INSERT/UPDATE/REPLACE), `delete` (DELETE), `execute` (stored functions),
  `admin` (DDL, and only over a whole `db.*`). A view is granted by its own
  name; it reads what it reads (definer rights), which is how to hand out a
  projection.
* **Attenuation** appends caveats to a chained MAC, so a holder can only ever
  narrow (fewer tables, fewer rights, earlier expiry, smaller row budget);
  stripping a caveat fails verification. Revoking a root revokes everything
  derived from it, and survives a restart (`<root>/capability-state`).
* **Enforcement**: every statement is read with the executor's tokenizer
  before anything runs. A verb outside SELECT/WITH, INSERT/REPLACE/UPDATE,
  DELETE, DDL, SET (not GLOBAL), USE, transaction control is refused (SHOW,
  DESCRIBE, EXPLAIN, CALL, PREPARE, LOAD, file output, other users, the
  `information_schema`/`mysql`/`sys` schemas). Every table in a table
  position, and every identifier that names a table or view, must be in
  scope; a second statement in one request is refused. A column that shares a
  table's name counts as the table - refuses a little too much, never too
  little. Prepared statements are checked at prepare and again at execute.
* **Row budget** counts rows returned or changed over the session. A result
  that would pass it is not sent; a write that crosses it has already been
  applied (autocommit), and the session is then cut off. It bounds what comes
  back, not the work the engine does to produce it.
* **Transport**: the credential is verified by signature, so it travels as
  `mysql_clear_password` and is accepted from this host only. Set
  `mysql_cap_remote=1` only on a link you trust (there is no TLS here), and
  keep TTLs short. `mysql_capabilities=0` turns the login off.
* `SET SESSION capability = '<credential>'` swaps a session to another
  credential (and resets its row count); an operator session that does it
  gives up its full authority for that connection.
* Not provided: row-level predicates (a tenant filter) - rewriting statement
  text safely needs the AST, so the gate refuses rather than guesses.

## Running by hand (development)

```bash
aios --root /tmp/asql-root exec "compute serve port=7801 lanes=8 sql=1 mysql_port=14501 mysql_bind=127.0.0.1 \
  mysql_user=asql mysql_pass_file=/opt/aios/mysql.pass mysql_socket=/tmp/a.sock"
```

`compute serve` takes the same keys as the conf, lower-cased
(`cdc_host= cdc_user= cdc_pass_file= cdc_db= cdc_tables=@file ...`). Set
`AIOS_NO_ACCEL=1` (the unit does): without it every start spends ~0.7 s and
400 MB bringing up OpenCL/CUDA/Vulkan. Memory knobs, all environment:
`AIOS_SQL_PROBE_MB` (join/probe index budget, default 2048),
`AIOS_MALLOC_MMAP_MB` (allocations of this size and up go to mmap and are
returned on free; default 1, 0 = glibc's dynamic threshold),
`AIOS_MALLOC_ARENAS` (cap on malloc arenas), `AIOS_TRIM_TRACE=1` (log every
trim), `AIOS_SQL_CACHE_MB` (result cache budget, default 1024; `0` turns the
result cache off, which is what a benchmark of the engine itself wants),
`AIOS_SQL_SHAPES=0` (parse every statement afresh instead of keeping one
parsed tree per statement shape per thread), `AIOS_TABLE_LOCKS=0` (every
write under the exclusive statement lock; by default a single-table
INSERT / UPDATE / DELETE locks only its table, see "Transactions" in
docs/asql.md), `AIOS_CHECKPOINT_SECS` (a checkpoint by statement count
no more often than this, default 30), `AIOS_CHECKPOINT_POOL_MB` (the
column copies a checkpoint keeps for the next one to copy into, default
512; a dictionary's appended tail is all the next snapshot copies), `AIOS_QUERY_MEMORY_MB` (memory the statements running at once may reserve for
their intermediates before a read waits its turn, default 2048, 0 = no limit;
see "Admission" in docs/asql.md), `AIOS_MAX_CONNECTIONS` (default 151),
`AIOS_STORE_MAINTAIN_MS`
(the time each storage-maintenance step - narrowing dictionary codes, coding
large dictionaries - may take per checkpoint, default 40; see "Storage
compression" in docs/asql.md), `AIOS_REDO_SYNC=0` (a statement is
answered once its record is in the log file and a thread syncs the log
every 200 ms - MariaDB's innodb_flush_log_at_trx_commit=2; the default
answers only once the record is on the medium). `aios --root R exec 'sql query "SELECT ..."'` runs one statement
against a store with no server.

---
## Client Integration Guide

### 1. MySQL CLI / Shell

```bash
# same host: the unix socket, with the account from /etc/aios/asql.conf
mysql -S /opt/aios/run/asql.sock -u erp -p"$(cat /opt/aios/mysql.pass)" versioneight
# from elsewhere (server has MYSQL_BIND=0.0.0.0)
mysql -h 10.0.0.5 -P 3307 -u erp -p versioneight
```

The wire has ONE account: `MYSQL_USER` with the first line of
`MYSQL_PASS_FILE`, and its password is always checked, local or not. With no
account configured the engine binds loopback only and lets loopback clients in
(`mysql_strict_loopback=1` closes even that).

### 2. PHP (PDO)

```php
$dsn = "mysql:unix_socket=/opt/aios/run/asql.sock;dbname=versioneight;charset=utf8mb4";
$pdo = new PDO($dsn, "erp", trim(file_get_contents("/opt/aios/mysql.pass")), [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
]);

$stmt = $pdo->query("SELECT orderno, debtorno, ord_date FROM salesorders WHERE status = 1 LIMIT 10");
$orders = $stmt->fetchAll();
```

### 3. Python (`pymysql`)

```python
import pymysql

conn = pymysql.connect(
    host="127.0.0.1",
    port=3307,
    user="root",
    password="",
    database="versioneight",
    cursorclass=pymysql.cursors.DictCursor
)

with conn.cursor() as cursor:
    cursor.execute("SELECT COUNT(*) AS total FROM salesorders")
    print(cursor.fetchone())
```

### 4. Node.js (`mysql2`)

```javascript
import mysql from 'mysql2/promise';

const conn = await mysql.createConnection({
  host: '127.0.0.1',
  port: 3307,
  user: 'root',
  database: 'versioneight'
});

const [rows] = await conn.query('SELECT orderno, ord_date FROM salesorders LIMIT 5');
console.log(rows);
```

### 5. GUI Clients (Navicat / DBeaver / TablePlus / DataGrip)

- **Connection Type**: MySQL
- **Host**: `127.0.0.1` (or server IP)
- **Port**: `3307`
- **User**: the conf's `MYSQL_USER`
- **Password**: the first line of `MYSQL_PASS_FILE`
- **Database**: `versioneight` (or schema name)
- Dynamic virtual `information_schema` automatically populates table lists, schemas, and column types.

### 6. Raw Socket Interface (Port 7311)

For internal high-throughput microservices, connect via TCP to port 7311:

- **Protocol**: 4-byte big-endian length prefix followed by JSON payload.
- **Request** (read-only; include the token when the server was started with one):
  ```json
  {"op": "sql", "query": "SELECT orderno, debtorno FROM salesorders WHERE status = 1"}
  ```
- **Response**:
  ```json
  {"ok": true, "op": "sql", "columns": ["orderno", "debtorno"],
   "rows": [["1001", "CUST42"], ["1002", "CUST88"]], "row_count": 2}
  ```

---

## Data Import & Initial Ingestion

### Import from `.sql` Dump

```bash
# Import a mysqldump file into ASQL
aios --root /opt/aios/root exec "sql import file=/path/to/backup.sql"

# Import SQL text directly
aios --root /opt/aios/root exec 'sql import sql="CREATE TABLE t1 (id INT PRIMARY KEY, val TEXT); INSERT INTO t1 VALUES (1, '\''hello'\'');"'
```

### Initial CDC Ingestion Workflow

Normally nothing to do: with `CDC_HOST` and `CDC_TABLES` set, the engine
copies every whitelisted table it does not have from the source at start (one
consistent snapshot, the binlog position remembered per table) and streams
from there; `journalctl -u aios-compute` shows `compute sql: seed ...` and
`whitelist - N of N table(s) present`. To seed from a dump instead:

1. **Take consistent snapshot**:
   ```bash
   mysqldump -u replicator -p --single-transaction --master-data=2 \
     versioneight salesorders salesorderdetails stockmaster locstock pickingfreeze debtorsmaster stocktrans \
     > /tmp/asql_initial.sql
   ```
2. **Inspect binlog position in dump**:
   ```bash
   head -n 30 /tmp/asql_initial.sql | grep "CHANGE MASTER TO"
   # Example: MASTER_LOG_FILE='mysql-bin.000124', MASTER_LOG_POS=3019482
   ```
3. **Import into ASQL**:
   ```bash
   aios --root /opt/aios/root exec "sql import file=/tmp/asql_initial.sql"
   ```
4. **Start daemon with pinned CDC coordinates** (`cdc_log_file` + `cdc_log_pos`).

---

## SQL Dialect & Feature Reference

ASQL supports the standard SQL dialect used in MySQL/MariaDB applications:

### DDL (Schema Definition)
- `CREATE TABLE [IF NOT EXISTS] ... (col TYPE [NOT NULL] [DEFAULT val] [AUTO_INCREMENT] [PRIMARY KEY], ...)`
- `CREATE TABLE t LIKE src` (columns, keys and options; not foreign keys) and `CREATE TABLE t [AS] SELECT ...`
- `DROP TABLE [IF EXISTS]`
- `TRUNCATE [TABLE]`
- `ALTER TABLE ... ADD COLUMN / DROP COLUMN / MODIFY COLUMN / RENAME TO`
- `CREATE VIEW ... AS SELECT ...` / `DROP VIEW`

### DML (Data Manipulation)
- `SELECT [DISTINCT] [expr AS alias, ...] FROM tbl [JOIN ...] [WHERE ...] [GROUP BY ... [HAVING ...]] [ORDER BY ... [ASC|DESC]] [LIMIT n [OFFSET m]]`
- `INSERT INTO tbl [(cols...)] VALUES (vals...), (...) [ON DUPLICATE KEY UPDATE ...]`
- `REPLACE INTO tbl [(cols...)] VALUES (...)`
- `UPDATE tbl SET col = val, ... [WHERE ...] [LIMIT n]`
- `DELETE FROM tbl [WHERE ...] [LIMIT n]`

### Joins & Advanced Querying
- `INNER JOIN`, `LEFT [OUTER] JOIN`, `RIGHT [OUTER] JOIN`, `CROSS JOIN`
- `UNION` / `UNION ALL`
- Common Table Expressions (CTEs): `WITH [RECURSIVE] cte_name AS (SELECT ...) SELECT ...`
- Subqueries: Scalar subqueries, `IN (SELECT ...)`, `EXISTS (SELECT ...)`, Correlated subqueries

### Functions & Operators
- **Math**: `+`, `-`, `*`, `/`, `%`, `ABS`, `ROUND`, `CEIL`, `FLOOR`, `MOD`, `POW`, `SQRT`
- **String**: `CONCAT`, `SUBSTRING`/`SUBSTR`, `LENGTH`, `UPPER`/`UCASE`, `LOWER`/`LCASE`, `TRIM`, `LTRIM`, `RTRIM`, `REPLACE`, `LEFT`, `RIGHT`, `LIKE`, `NOT LIKE`
- **Date/Time**: `NOW()`, `CURDATE()`, `CURTIME()`, `DATE()`, `YEAR()`, `MONTH()`, `DAY()`, `DATE_ADD()`, `DATE_SUB()`, `DATEDIFF()`, `DATE_FORMAT()`
- **Logic & Flow**: `CASE WHEN ... THEN ... ELSE ... END`, `IF(cond, t, f)`, `IFNULL(val, default)`, `COALESCE(...)`, `IS NULL`, `IS NOT NULL`, `IN (...)`, `BETWEEN ... AND ...`
- **Aggregates**: `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `AVG(col)`, `MIN(col)`, `MAX(col)`, `GROUP_CONCAT(col [SEPARATOR '...'])`

---

## Measuring it against your MariaDB

`tools/asql-diff-runner.mjs` runs each query on both servers, times it, and
diffs the results, so a speedup is reported only where the answers match:

```bash
node tools/asql-diff-runner.mjs --query "SELECT COUNT(*) FROM salesorders WHERE orderstatusid = 1"
node tools/asql-diff-runner.mjs --file queries.sql
```

Time with the result cache off (`SQL_NO_CACHE`) so a cached answer is not
counted as speed, and compare on the same data on both sides.

sysbench's OLTP scripts run unchanged against the wire (prepared statements
included): `--mysql-port=3307 --mysql-user=... --mysql-db=...` with `prepare`,
`run` and `cleanup` as against MariaDB. Writes are made durable by a redo
record per statement, synced in groups across sessions (one `fdatasync` covers
every statement waiting on it), and the store is checkpointed every 20,000
statements or 64 MB of redo; restart replays what the last checkpoint missed.

Transactions take one writer lock for their whole life unless the server runs
with `AIOS_TXN_CONCURRENT=1` (rows are owned as they are written; see
"Transactions" in docs/asql.md). The packaged unit sets it. With several
sessions each wrapping their statements in `BEGIN ... COMMIT` - what every ORM
and sysbench do - it matters: without it, four sessions' read-only
transactions ran at a quarter of one session's throughput, serialized on that
lock. The numbers, and what was changed to get them, are in
docs/asql-benchmarks.md.

---

## Diagnostic & Test Tools

The AIOS build includes built-in verification and benchmark test binaries in `build/`:

```bash
# Build test targets
cmake --build build -j$(nproc) --target \
  aios-asql-binlog-sync-check \
  aios-asql-enhancements-check \
  aios-asql-import-check \
  aios-asql-sqlfeatures-check \
  aios-mysql-check

# Run verification checks
./build/aios-asql-binlog-sync-check
./build/aios-asql-enhancements-check
./build/aios-asql-import-check
./build/aios-asql-sqlfeatures-check
./build/aios-mysql-check
```

---

## Troubleshooting & FAQ

### 1. Engine Capabilities & Differences from MariaDB / InnoDB

#### Native Features Supported
The following advanced features **all run natively** in ASQL:
- **Stored Programs**: `CREATE PROCEDURE`, `CREATE FUNCTION`, `CREATE TRIGGER` (firing on `INSERT`, `UPDATE`, `DELETE`), and `CREATE EVENT`, with support for `IF`, `CASE`, `WHILE`, `REPEAT`, `LOOP`, `LEAVE`, `ITERATE`, `DECLARE CURSOR`, `DECLARE HANDLER`, and `SIGNAL` with `SQLSTATE`. Stored routines are persisted in `<root>/programs.db`.
- **Row-Level Locking**: `SELECT ... FOR UPDATE`, `FOR SHARE`, and `LOCK IN SHARE MODE` with `NOWAIT`, `SKIP LOCKED`, and `WAIT n`. Row ownership is tracked per transaction, allowing concurrent readers and writers without whole-table lockouts.
- **Window Functions**: Full SQL window functions (`OVER`, `PARTITION BY`, `ORDER BY`, `ROWS/RANGE`, `ROW_NUMBER`, `RANK`, `DENSE_RANK`, `LAG`, `LEAD`, `FIRST_VALUE`, `LAST_VALUE`, `SUM`, `AVG`, `COUNT`, `MIN`, `MAX`).
- **Joins & Set Operations**: `FULL OUTER JOIN`, `CROSS JOIN`, `NATURAL JOIN`, `USING`, and `UNION` / `UNION ALL`.
- **Transactions & Integrity**: `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, and `FOREIGN KEY` constraints (`CASCADE`, `SET NULL`).

#### Permissive Mode (By Design)
By default, ASQL runs in **permissive mode** (matching MariaDB with `sql_mode = ''`). Declared constraints (`NOT NULL`, `VARCHAR(n)` string lengths, integer ranges, `UNSIGNED`, calendar dates, `ENUM` members) are parsed and displayed, but type casting and truncation are handled permissively. This ensures maximum out-of-the-box compatibility with real-world ERP systems and legacy application frameworks without failing on minor data mismatches.
- If your environment requires strict validation, export `AIOS_SQL_STRICT=1` (enforces MariaDB `STRICT_TRANS_TABLES` behavior).
- `PRIMARY KEY` and `UNIQUE` constraints are **always enforced** regardless of mode.

#### User Management & Permissions
ASQL does not use the legacy `mysql.user` table or complex `GRANT` syntax. Instead, user access is cleanly divided into:
1. **Operator / Root Account**: Configured via `MYSQL_USER` and `MYSQL_PASS_FILE` in `asql.conf` (full administrative rights over all databases).
2. **Named Users**: Managed directly over the MySQL wire protocol via:
   ```sql
   CREATE USER 'name' IDENTIFIED BY 'password' TABLES 'db.*,db.table' [RIGHTS 'read,write,delete,execute,admin'] [TTL <seconds>] [ROWS <n>];
   ALTER USER 'name' [IDENTIFIED BY 'password'] [TABLES '...'] [RIGHTS '...'] [TTL <seconds>] [ROWS <n>];
   DROP USER 'name';
   SHOW USERS;
   ```
3. **Capability Bearer Credentials**: Cryptographically signed bearer tokens (`CAPABILITY MINT`, `CAPABILITY REVOKE`, user `cap`) that can be attenuated offline. Users are stored in `<root>/capability-users` (mode `0600`).

#### CDC Local Mirror & Staging Workflow (No Write-Back)
ASQL supports real-time incoming binlog replication from an upstream MariaDB/MySQL instance (`CDC_HOST`, `CDC_PORT`, `CDC_DB`, `CDC_TABLES`):
- **Real-Time Data Freshness**: The engine tails the upstream binlog (e.g. `mariadb-bin.NNNNNN`) and applies changes into in-memory columnar vectors within milliseconds.
- **Local Read/Write Testing**: Applications can execute 100% of their reads and writes against local ASQL. Local writes are committed to the local store and are **not pushed back upstream** to the production database.
- **Safety**: Because local writes do not replicate upstream, testing and development workflows cannot corrupt production data. If upstream subsequently mutates a row that was modified locally, replication will apply the upstream transaction (`cdc mirror: replication overwrites local writes`), ensuring local data tracks upstream truth.

#### Operational Notes
- `SELECT ... LIMIT n` without `ORDER BY` returns the first n rows in in-memory insertion order. (In InnoDB, order depends on which B-Tree index is traversed; both behaviors conform to SQL standards—always specify `ORDER BY` when order matters).
- `KILL [QUERY | CONNECTION]` terminates a running read (`QUERY`) or disconnects a client session (`CONNECTION`). Atomic write statements run to completion.
- Multi-table `DELETE` with `ORDER BY` or `LIMIT` is refused (same as MySQL).
- `INSERT`, `UPDATE`, or `DELETE` through a SQL `VIEW` is refused (`write the base table`).
- Window functions nested inside another window function are refused.

### 2. Decimal formatting
`DECIMAL(p,s)` values are formatted to their declared scale, as MariaDB does (`12.5` in a
`DECIMAL(10,4)` reads back as `12.5000`).

### 3. Data drift or CDC re-sync
`aios-asql-console -e "SELECT COUNT(*) FROM t"` against MariaDB's count tells.
To resync one table: stop the service, delete that table's file from
`/opt/aios/root/sql_db/tables/` (its name is the hex of `db.table`), start:
a whitelisted table that is not in the store is copied afresh from the source
and the stream picks it up at that snapshot's position. To resync everything: stop, move
`sql_db/tables` and `sql_db/cdc_position` aside, start.

### 4. The service exits 78 on every start
`asql-serve` cannot read `/etc/aios/asql.conf`: the directory must be
traversable by user `aios` (`root:aios 0750`, the file `0640`).

### 5. `Asql_cache_hits` stays at 0
`SELECT SQL_NO_CACHE` bypasses the result cache on purpose; a write to a
table invalidates the entries that read it. `SHOW STATUS LIKE 'Asql_cache%'`
gives entries and bytes against `Asql_cache_budget_bytes`.

### 6. RSS is well above `Asql_memory_bytes`
`Asql_memory_bytes` is the tables, dictionaries and their per-column caches;
`Asql_probe_index_mem_bytes` the join/probe indexes; `Asql_cache_bytes` the
result cache. The rest is a statement's transient intermediates, which the
engine hands back at the statement's end (`AIOS_TRIM_TRACE=1` logs each
trim); a persistent gap means a leak, report it with `SHOW ENGINE ASQL STATUS`.
