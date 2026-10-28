# ASQL — AIOS In-Memory Relational SQL Engine

**ASQL** is the high-performance, in-memory relational SQL database engine built natively into **AIOS** (All-In-One System) in dependency-free C++20.

It combines in-memory columnar indexing with broad MariaDB/MySQL dialect coverage, MySQL 5.7 wire-protocol compatibility, ACID transactions, and flexible deployment models:
- **Standalone Primary Database**: A pure in-memory SQL database with write-ahead redo logging, periodic checkpoints, and point-in-time snapshot exports. No external database required.
- **Real-Time CDC Replica**: Ingests live MariaDB/MySQL binlogs with sub-millisecond lag and zero RAM bloat, serving as a high-speed read accelerator.
- **Write-Through / Write-Behind Gateway**: Synchronously or asynchronously synchronizes mutations to an external downstream database while serving reads instantly from RAM.

---

## Key Highlights

- **Pure In-Memory Speed**: In-memory tables, plan cache, subquery shape cache, and result cache; benchmarked against MariaDB with `tools/asql-diff-runner.mjs`.
- **Zero External Dependencies**: Pure C++20 implementation. No `libmysqlclient`, `libmariadb`, `sqlite`, or third-party query engines linked.
- **MySQL Wire Protocol (Port 3307)**: Native listener supporting standard clients: MySQL CLI, Navicat, DBeaver, TablePlus, DataGrip, PHP PDO, Python (`pymysql`), Node.js (`mysql2`), and Go.
- **Raw RPC Socket (Port 7311)**: Length-prefixed JSON protocol for low-overhead service-to-service communication.
- **Virtual `information_schema`**: Dynamic emulation of MySQL system metadata tables for transparent GUI and ORM introspection.
- **ACID Transactions & Row Locking**: `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, and `SELECT ... FOR UPDATE` with snapshot isolation and transaction rollback safety.
- **Flexible Write Modes**: Run `local` (self-contained), `write_through` (synchronously confirmed downstream), or `write_behind` (high-velocity local commit with persistent WAL drain).
- **Capability-Based Security**: Cryptographically signed bearer credentials (`CAPABILITY MINT` / `REVOKE`) with offline attenuation alongside traditional named user logins.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                                  CLIENT APPLICATIONS                                   │
│         MySQL CLI · Navicat / DBeaver · PHP (PDO) · Python · Node.js · Raw RPC         │
└───────────────────────────┬────────────────────────────────┬───────────────────────────┘
                            │ MySQL Wire Protocol (port 3307)│ Raw JSON RPC (port 7311)
┌───────────────────────────▼────────────────────────────────▼───────────────────────────┐
│                                       ASQL ENGINE                                      │
│  ┌──────────────────────────────────────────────────────────────────────────────────┐  │
│  │ AST Parser & Execution Planner (Subquery Cache, CTE Evaluator, Window Functions) │  │
│  └────────────────────────────────────────┬─────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────▼─────────────────────────────────────────┐  │
│  │ In-Memory Data Store & Columnar Indices                                          │  │
│  │  • PK / Unique Hash Index   • Probe Index (Hash & Sorted)                        │  │
│  │  • Auto-Increment Tracker   • Dynamic info_schema Views                          │  │
│  └────────────────────────────────────────▲─────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────┴─────────────────────────────────────────┐  │
│  │ Multi-Tenant Database Namespace Isolation (e.g. appdb, analytics, shop)          │  │
│  └──────┬─────────────────────────────────┬─────────────────────────────────▲───────┘  │
│         │                                 │                                 │          │
│  ┌──────▼───────────────────┐     ┌───────▼──────────────────┐     ┌────────┴────────┐ │
│  │ Durability Subsystem     │     │ Write Synchronization    │     │ CDC Consumer    │ │
│  │ • Write-Ahead Redo Log   │     │ • local (self-contained) │     │ (Zero-RAM       │ │
│  │ • Periodic Checkpoints   │     │ • write_through (sync)   │     │  Binlog Ingress)│ │
│  │ • Export Snapshots       │     │ • write_behind (WAL)     │     │                 │ │
│  └──────────────────────────┘     └───────┬──────────────────┘     └────────▲────────┘ │
└───────────────────────────────────────────┼─────────────────────────────────┼──────────┘
                                            │ Forward Writes                  │ COM_BINLOG_DUMP
                                            ▼                                 │
                             ┌─────────────────────────────┐   ┌──────────────┴──────────┐
                             │ Downstream Database/Gateway │   │ Upstream MySQL/MariaDB  │
                             └─────────────────────────────┘   └─────────────────────────┘
```

---

## Operating Modes & Write Synchronization

ASQL supports three primary operational architectures and three write synchronization policies:

### 1. Standalone Primary Database
Run ASQL as an independent, primary in-memory SQL database without an upstream or downstream database:
- **Durability**: Every mutating transaction is recorded to an append-only write-ahead redo log (`redo.log`). Sync behavior is configurable via `AIOS_REDO_SYNC` (group commit by default; `0` syncs asynchronously every 200ms).
- **Checkpoints**: The store checkpoints automatically every 20,000 statements or 64 MB of redo (`AIOS_CHECKPOINT_SECS`, default 30s). On startup, ASQL loads the latest checkpoint and replays any trailing redo records.
- **Backups & Snapshots**: Consistent point-in-time snapshots can be exported online with `asql-export.sh` or `EXPORT SNAPSHOT` without interrupting concurrent readers.
- **Running Standalone**:
  ```bash
  # Standalone daemon via aios-engine
  build/aios-engine --root=/opt/aios/root --port=3307 --sql-user=app --sql-pass-file=/opt/aios/mysql.pass

  # Or via aios CLI
  aios --root=/opt/aios/root exec "compute serve sql=1 port=7311 mysql_port=3307 mysql_user=app mysql_pass_file=/opt/aios/mysql.pass"
  ```

### 2. CDC Ingress Replica (Read Accelerator)
ASQL connects to an upstream MariaDB or MySQL instance as a replication slave (`COM_BINLOG_DUMP`):
- **Zero-RAM-Bloat Streaming**: Reads row-based binlog events (`WRITE_ROWS`, `UPDATE_ROWS`, `DELETE_ROWS`) and applies mutations into in-memory columnar vectors within milliseconds.
- **Table Whitelist**: Whitelist specific tables via `CDC_TABLES` (csv or `@file`). Unlisted tables are ignored, bounding memory usage.
- **Automatic Initial Seeding**: Tables listed in `CDC_TABLES` that do not exist locally are automatically snapshotted from the source at startup, recording per-table binlog coordinates before streaming resumes.

### 3. Write Synchronization Modes (`local`, `write_through`, `write_behind`)

When client applications issue write statements (`INSERT`, `UPDATE`, `DELETE`, `REPLACE`, DDL), ASQL's behavior is governed by its sync mode (`WriteSyncMode`):

| Mode | Target | Latency | Consistency | Downstream Failure Behavior |
|---|---|---|---|---|
| **`local`** (Default) | In-Memory + Redo Log | Sub-millisecond | Local ACID | No downstream required. |
| **`write_through`** | Downstream first, then local | 1x Downstream RTT | Synchronous dual-write | Refuses local write; nothing applied. |
| **`write_behind`** | Local first, then async WAL drain | Sub-millisecond | Eventual downstream sync | Queues in WAL; retries in order until up. |

#### Pattern A: `local` (The Default)
Statements run entirely against the local in-memory tables and are made durable by the local redo log and checkpoints:
- **Standalone**: All changes remain within the local ASQL instance.
- **CDC Read Replica**: Local writes are committed to the local in-memory store and are **never replicated upstream**. This provides a safe staging or analytics environment where writes cannot corrupt upstream production data. If upstream subsequently mutates a row that was changed locally, incoming CDC replication overwrites it with upstream truth.

#### Pattern B: `write_through` (Synchronous Forwarding)
Modifying statements (`INSERT`, `UPDATE`, `DELETE`, `CREATE`, `DROP`, `ALTER`, `TRUNCATE`, `RENAME`) and transaction controls (`BEGIN`, `COMMIT`, `ROLLBACK`) are forwarded to the configured `--downstream` server **first**, using the client's own dedicated downstream session:
- The write is applied locally in ASQL only after downstream successfully accepts and executes it.
- If the downstream database is unreachable or rejects the statement (e.g. `Duplicate entry for key 'PRIMARY'`), ASQL immediately refuses the statement with the downstream error and modifies nothing locally.
- Reads (`SELECT`, `WITH`), `SHOW`, `SET`, and `USE` are answered instantly from ASQL in memory.
- Example flag: `--mode=write_through --downstream=127.0.0.1:3306 --downstream-user=app --downstream-db=appdb`.

#### Pattern C: `write_behind` (Asynchronous WAL Drain)
Writes execute and commit locally in ASQL first for maximum execution speed:
- On commit, mutations are appended to `<store>/write_behind.wal`.
- An internal drain worker wakes every 20ms to replay committed statements downstream in batches of 100 in strict chronological order, marking WAL progress as downstream acknowledges each statement.
- Rolled-back transactions (`ROLLBACK`) are discarded locally and never written to the WAL.
- If downstream becomes unreachable or rejects a statement, unacknowledged statements persist safely across restarts in the WAL. Subsequent statements wait behind the blocked write to preserve serial consistency until downstream recovers.
- Activate via `--mode=write_behind --downstream=...` or environment variable `AIOS_WRITE_BEHIND=1`.

---

## Installing ASQL on a Server

ASQL packages into a self-contained tarball runnable on any x86_64 Linux with glibc 2.17+ (CentOS 7, AlmaLinux 8/9, Rocky, Debian, Ubuntu).

### 1. Build the Package

```bash
cd aios   # top-level checkout
# Compile the engine and operator utilities against glibc 2.17
TARGETS="aios-cli aios-asql-top aios-asql-console" packaging/build-centos7.sh origin/main build-centos7
packaging/pack.sh build-centos7 packaging/dist        # -> packaging/dist/aios-asql-<sha>-linux-x86_64-glibc2.17.tar.gz
```

### 2. Install

```bash
tar xzf aios-asql-*.tar.gz && cd aios-asql-*/
sudo ./install-asql.sh --dry-run       # preview changes without executing
sudo ./install-asql.sh                 # creates user aios, /opt/aios, systemd service
./install-asql.sh --host user@remote   # or deploy remotely over SSH
```

What lands on the host:

```
/opt/aios/bin/aios                 the unified engine
/opt/aios/bin/asql-serve           translates /etc/aios/asql.conf into engine arguments
/opt/aios/bin/aios-asql-console    interactive SQL CLI client
/opt/aios/bin/aios-asql-top        real-time interactive terminal monitor
/opt/aios/bin/asql-export.sh       online snapshot exporter
/opt/aios/bin/asql-import.sh       snapshot importer
/opt/aios/root/sql_db/             database store (tables/, cdc_position, redo.log, things/)
/opt/aios/run/asql.sock            UNIX domain socket for same-host clients
/opt/aios/cdc.pass                 upstream replication password (0600 aios:aios)
/opt/aios/mysql.pass               wire operator password (0600 aios:aios)
/etc/aios/asql.conf                system configuration file
```

### 3. Configure: `/etc/aios/asql.conf`

```bash
sudo vi /etc/aios/asql.conf
echo 'replication-pass' | sudo tee /opt/aios/cdc.pass   >/dev/null
echo 'wire-pass'        | sudo tee /opt/aios/mysql.pass >/dev/null
sudo chown aios:aios /opt/aios/*.pass && sudo chmod 600 /opt/aios/*.pass
/opt/aios/bin/asql-serve --print       # view assembled command string
sudo systemctl restart aios-compute
```

| Key | Default | Meaning |
|---|---|---|
| `AIOS_BIN`, `AIOS_ROOT` | `/opt/aios/bin/aios`, `/opt/aios/root` | Engine binary and kernel data root |
| `RPC_PORT` | `7311` | Raw JSON RPC socket (`compute serve port=`) |
| `LANES` | `8` | Worker lane concurrency |
| `SQL`, `SQL_DB` | `1`, `sql_db` | Enable SQL engine; storage subdirectory |
| `PUBLIC` | `1` | `0` keeps the RPC port on loopback |
| `MYSQL_PORT`, `MYSQL_BIND` | `3307`, `127.0.0.1` | MySQL wire listener; `0.0.0.0` accepts remote connections |
| `MYSQL_USER`, `MYSQL_PASS_FILE` | `app`, `/opt/aios/mysql.pass` | Operator wire account. Without a user, loopback clients enter freely |
| `MYSQL_SOCKET` | `/opt/aios/run/asql.sock` | UNIX socket path (~10 µs lower latency than TCP on localhost) |
| `MYSQL_CHARSET` | empty | `1` honours client charset (e.g. latin1 gets cp1252 bytes) |
| `MYSQL_DB` | empty | Default database for console and top (defaults to `CDC_DB`) |
| `CDC_HOST`, `CDC_PORT` | `127.0.0.1`, `3306` | Upstream MariaDB/MySQL instance. Leave empty for standalone mode |
| `CDC_USER`, `CDC_PASS_FILE` | `replicator`, `/opt/aios/cdc.pass` | Replication credentials (`REPLICATION SLAVE, SELECT`) |
| `CDC_DB` | `appdb` | Source database schema to mirror |
| `CDC_TABLES` | (csv list or `@file`) | Table whitelist. Empty mirrors all tables mentioned in binlog |
| `CDC_LOG_FILE`, `CDC_LOG_POS` | empty | Pin binlog coordinates; empty auto-discovers or resumes saved state |
| `FLEET_HOST`, `FLEET_PORT`, `FLEET_KEY_FILE` | empty | Fleet mesh connection settings (optional cluster read-mesh) |

### 4. Verify

```bash
systemctl status aios-compute
journalctl -u aios-compute -f     # view startup, indexing, and CDC logs
aios-asql-top -1                  # snapshot of tables, rows, RSS, cache, CDC lag
aios-asql-console -e "SHOW STATUS LIKE 'Asql_%'"
aios-asql-console                 # interactive SQL prompt
mysql -S /opt/aios/run/asql.sock -u app -p"$(cat /opt/aios/mysql.pass)" appdb
```

### 5. Moving Data Between Servers

```bash
# On source server: creates atomic snapshot archive + .sha256
asql-export.sh -o /tmp/asql.tar.gz

# On target server: verifies checksum, stops service, swaps store, resumes
asql-import.sh /tmp/asql.tar.gz
```

---

## Users and Permissions: Capability Credentials

ASQL provides two authentication mechanisms:
1. **Operator Account** (`MYSQL_USER`): Defined in `asql.conf` with full administrative authority across all schemas.
2. **Capability Credentials**: Cryptographically signed bearer tokens that attenuate authority (restricting accessible tables, rights, time-to-live, and maximum row budget).

```bash
# Operator mints a capability credential (over SQL or CLI)
CAPABILITY MINT 'shop.orders,shop.customers' RIGHTS 'read' TTL 600 ROWS 1000;   -- returns signed bearer token
CAPABILITY REVOKE '<credential>';

# Holder attenuates credential offline without contacting server
aios asqlcap attenuate <credential> tables=shop.orders rights=read ttl=60 rows=100

# Client connects with username "cap" and token as password
mysql -h 127.0.0.1 -P 3307 -u cap --enable-cleartext-plugin -p"<credential>" shop
```

### Named Users

Named users provide standard login credentials backed by an underlying capability:

```sql
-- Create user restricted to specific tables and rights
CREATE USER 'reporter' IDENTIFIED BY 'secret' TABLES 'analytics.*' RIGHTS 'read' TTL 86400 ROWS 50000;
ALTER USER 'reporter' IDENTIFIED BY 'newsecret';
ALTER USER 'reporter' RIGHTS 'read,write';
SHOW USERS;
DROP USER 'reporter';
```

```bash
# Connect using standard MySQL drivers
mysql -h 127.0.0.1 -P 3307 -u reporter -psecret analytics
```

- **Scope & Rights**: Scope is `db.*` or `db.table`. Rights: `read` (`SELECT`), `write` (`INSERT`, `UPDATE`, `REPLACE`), `delete` (`DELETE`), `execute` (stored functions), and `admin` (DDL).
- **Row Budget**: Enforces a strict limit on cumulative rows returned or mutated per session.

---

## Running by Hand (Development & Testing)

```bash
# Run standalone in-memory database without CDC
aios --root /tmp/asql-dev exec "compute serve port=7801 lanes=8 sql=1 \
  mysql_port=14501 mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=/opt/aios/mysql.pass"

# Run standalone engine binary directly with write-through forwarding
build/aios-engine --root=/tmp/asql-engine --port=13593 --bind=127.0.0.1 \
  --mode=write_through --downstream=127.0.0.1:3306 --downstream-user=app --downstream-db=appdb

# Execute single statement against a datadir without starting a daemon
aios --root /tmp/asql-dev exec 'sql query "SELECT * FROM appdb.users LIMIT 5"'
```

### Core Environment Knobs

- `AIOS_NO_ACCEL=1`: Disable GPU initialization (saves ~0.7s startup and 400 MB RAM).
- `AIOS_SQL_PROBE_MB`: Join/probe index cache budget in MB (default 2048).
- `AIOS_SQL_CACHE_MB`: Result cache budget in MB (default 1024; `0` disables).
- `AIOS_QUERY_MEMORY_MB`: Intermediate query memory limit before queuing (default 2048).
- `AIOS_CHECKPOINT_SECS`: Minimum seconds between automatic checkpoints (default 30).
- `AIOS_REDO_SYNC=0`: Group commit writes to log and sync asynchronously every 200ms (MariaDB `innodb_flush_log_at_trx_commit=2` equivalent). Default (`1`) syncs synchronously to storage.
- `AIOS_TXN_CONCURRENT=1`: Row-level concurrent transactions rather than global writer lock.
- `AIOS_WRITE_BEHIND=1`: Force write-behind WAL mode.

---

## Client Integration Guide

### 1. MySQL CLI / Shell

```bash
# Connect via UNIX domain socket
mysql -S /opt/aios/run/asql.sock -u app -p"$(cat /opt/aios/mysql.pass)" appdb

# Connect over TCP
mysql -h 127.0.0.1 -P 3307 -u app -p appdb
```

### 2. PHP (PDO)

```php
$dsn = "mysql:unix_socket=/opt/aios/run/asql.sock;dbname=appdb;charset=utf8mb4";
$pdo = new PDO($dsn, "app", trim(file_get_contents("/opt/aios/mysql.pass")), [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
]);

$stmt = $pdo->query("SELECT id, user_id, total, created_at FROM orders WHERE status = 'pending' LIMIT 10");
$orders = $stmt->fetchAll();
```

### 3. Python (`pymysql`)

```python
import pymysql

conn = pymysql.connect(
    host="127.0.0.1",
    port=3307,
    user="app",
    password=open("/opt/aios/mysql.pass").read().strip(),
    database="appdb",
    cursorclass=pymysql.cursors.DictCursor
)

with conn.cursor() as cursor:
    cursor.execute("SELECT COUNT(*) AS total FROM orders")
    print(cursor.fetchone())
```

### 4. Node.js (`mysql2`)

```javascript
import mysql from 'mysql2/promise';

const conn = await mysql.createConnection({
  host: '127.0.0.1',
  port: 3307,
  user: 'app',
  password: 'your-wire-password',
  database: 'appdb'
});

const [rows] = await conn.query('SELECT id, user_id, status FROM orders LIMIT 5');
console.log(rows);
```

### 5. GUI Clients (Navicat / DBeaver / TablePlus / DataGrip)

- **Connection**: MySQL
- **Host / Port**: `127.0.0.1` / `3307`
- **User / Password**: `MYSQL_USER` and contents of `MYSQL_PASS_FILE`
- **Database**: `appdb`
- Dynamic virtual `information_schema` populates schemas, tables, and column metadata out of the box.

### 6. Raw JSON RPC (Port 7311)

For microservices desiring low serialization overhead:
- **Framing**: 4-byte big-endian length prefix followed by UTF-8 JSON.
- **Request**:
  ```json
  {"op": "sql", "query": "SELECT id, user_id, status FROM orders WHERE status = 'active' LIMIT 2"}
  ```
- **Response**:
  ```json
  {
    "ok": true,
    "op": "sql",
    "columns": ["id", "user_id", "status"],
    "rows": [[1001, "USR42", "active"], [1002, "USR88", "active"]],
    "row_count": 2
  }
  ```

---

## Data Import & Ingestion

### Import from `.sql` Dump

```bash
# Import a mysqldump file into ASQL
aios --root /opt/aios/root exec "sql import file=/path/to/backup.sql"

# Import direct SQL statements
aios --root /opt/aios/root exec 'sql import sql="CREATE TABLE t1 (id INT PRIMARY KEY, val TEXT); INSERT INTO t1 VALUES (1, '\''hello'\'');"'
```

### Initial CDC Seeding from Upstream MariaDB/MySQL

When `CDC_HOST`, `CDC_DB`, and `CDC_TABLES` are configured, ASQL automatically copies missing tables from upstream during startup. To seed manually from a consistent snapshot:

1. **Dump tables with master coordinates**:
   ```bash
   mysqldump -u replicator -p --single-transaction --master-data=2 \
     appdb users orders order_items products inventory > /tmp/asql_initial.sql
   ```
2. **Find coordinates**:
   ```bash
   grep "CHANGE MASTER TO" /tmp/asql_initial.sql
   # e.g.: MASTER_LOG_FILE='mariadb-bin.000124', MASTER_LOG_POS=3019482
   ```
3. **Import and pin coordinates**:
   ```bash
   aios --root /opt/aios/root exec "sql import file=/tmp/asql_initial.sql"
   # Set CDC_LOG_FILE and CDC_LOG_POS in /etc/aios/asql.conf before restarting
   ```

---

## SQL Dialect & Feature Reference

### Schema & Data Definition (DDL)
- `CREATE TABLE [IF NOT EXISTS] ... (col TYPE [NOT NULL] [DEFAULT val] [AUTO_INCREMENT] [PRIMARY KEY], ...)`
- `CREATE TABLE t LIKE src` and `CREATE TABLE t [AS] SELECT ...`
- `DROP TABLE [IF EXISTS]`, `TRUNCATE [TABLE]`
- `ALTER TABLE ... ADD COLUMN / DROP COLUMN / MODIFY COLUMN / RENAME TO`
- `CREATE VIEW ... AS SELECT ...` / `DROP VIEW`

### Queries & Data Manipulation (DML)
- `SELECT [DISTINCT] [expr AS alias, ...] FROM tbl [JOIN ...] [WHERE ...] [GROUP BY ... [HAVING ...]] [ORDER BY ...] [LIMIT n [OFFSET m]]`
- `INSERT INTO tbl [(cols...)] VALUES (...), (...) [ON DUPLICATE KEY UPDATE ...]`
- `REPLACE INTO tbl [(cols...)] VALUES (...)`
- `UPDATE tbl SET col = val, ... [WHERE ...] [LIMIT n]`
- `DELETE FROM tbl [WHERE ...] [LIMIT n]`

### Joins, CTEs & Advanced Querying
- `INNER JOIN`, `LEFT [OUTER] JOIN`, `RIGHT [OUTER] JOIN`, `FULL OUTER JOIN`, `CROSS JOIN`, `NATURAL JOIN`
- `UNION` / `UNION ALL`
- Common Table Expressions (CTEs): `WITH [RECURSIVE] cte_name AS (SELECT ...) SELECT ...`
- Subqueries: Scalar, `IN (SELECT ...)`, `EXISTS (SELECT ...)`, Correlated subqueries
- Window Functions: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `LAG()`, `LEAD()`, `FIRST_VALUE()`, `LAST_VALUE()`, `SUM() OVER (...)`, etc.

### Stored Programs & Triggers
- `CREATE PROCEDURE`, `CREATE FUNCTION`, `CREATE TRIGGER` (`BEFORE`/`AFTER` `INSERT`/`UPDATE`/`DELETE`), `CREATE EVENT`.
- Control flow: `IF`, `CASE`, `WHILE`, `REPEAT`, `LOOP`, `LEAVE`, `ITERATE`, `DECLARE CURSOR`, `DECLARE HANDLER`, `SIGNAL SQLSTATE`.
- Routines persist across restarts in `<root>/programs.db`.

---

## Benchmarks & Verification

Compare queries on both ASQL and MariaDB using the diff runner:

```bash
# Verify identical result sets and compare execution latency
node tools/asql-diff-runner.mjs --query "SELECT COUNT(*) FROM orders WHERE status = 'completed'"
node tools/asql-diff-runner.mjs --file test_queries.sql
```

The test runner validates result set parity across both engines. `sysbench` OLTP workloads run out-of-the-box via standard MySQL wire protocol connections. Detailed benchmarks and architectural optimizations are documented in [docs/asql-benchmarks.md](docs/asql-benchmarks.md).

---

## Troubleshooting & FAQ

### 1. Strict vs. Permissive Mode
By default, ASQL matches MariaDB with `sql_mode = ''` (permissive casting and truncation).
- For strict validation, export `AIOS_SQL_STRICT=1` (enforcing `STRICT_TRANS_TABLES`).
- `PRIMARY KEY` and `UNIQUE` constraints are **always enforced** regardless of mode.

### 2. Resyncing CDC Tables
If local data diverges or a table file is corrupted:
- **Resync one table**: Stop service, delete `/opt/aios/root/sql_db/tables/<hex_of_db.table>`, restart. ASQL fetches a fresh upstream snapshot for that table.
- **Resync entire database**: Stop service, move `/opt/aios/root/sql_db/tables/` and `/opt/aios/root/sql_db/cdc_position` aside, and restart.

### 3. Service Exits with Code 78
Exit code 78 means `asql-serve` cannot read `/etc/aios/asql.conf`. Ensure directory permissions are readable by user `aios` (`root:aios 0750`, file `0640`).

### 4. Query Cache Hits Stay at 0
Queries using `SQL_NO_CACHE` bypass the result cache deliberately. Furthermore, table writes invalidate cached results that reference the mutated table. View status with `SHOW STATUS LIKE 'Asql_cache%'`.
