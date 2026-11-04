# ASQL — AIOS In-Memory Relational SQL Engine

**ASQL** is the high-performance, in-memory relational SQL engine built natively into **AIOS** (All-In-One System) in dependency-free C++20.

It combines in-memory columnar execution, ACID write-ahead durability, full MySQL 5.7 wire protocol compatibility, and PostgreSQL dialect extensions with two novel reactive primitives: **Things** (live, stateful materialized views) and **Cells** (per-key event-trigger bitsets and counters).

---

## Table of Contents

- [Key Highlights](#key-highlights)
- [Architecture Overview](#architecture-overview)
- [30-Second Quickstart](#30-second-quickstart)
- [Client Integration & Usage](#client-integration--usage)
  - [1. Native Grain Language (AIOS First-Class API)](#1-native-grain-language-aios-first-class-api)
  - [2. Standard MySQL Wire Protocol (CLI, Python, Node.js, PHP, Go)](#2-standard-mysql-wire-protocol-cli-python-nodejs-php-go)
  - [3. GUI Tools (DBeaver, DataGrip, TablePlus, Navicat)](#3-gui-tools-dbeaver-datagrip-tableplus-navicat)
  - [4. Low-Overhead JSON RPC (Port 7311)](#4-low-overhead-json-rpc-port-7311)
- [Reactive Primitives: Things and Cells](#reactive-primitives-things-and-cells)
  - [Things: Stateful Grouped Aggregates](#things-stateful-grouped-aggregates)
  - [Cells: Per-Key Bits, Counters & Webhook/Email Triggers](#cells-per-key-bits-counters--webhookemail-triggers)
- [Operating Modes & Write Synchronization](#operating-modes--write-synchronization)
  - [1. Standalone Primary Database](#1-standalone-primary-database)
  - [2. Real-Time CDC Replica (Binlog Read Accelerator)](#2-real-time-cdc-replica-binlog-read-accelerator)
  - [3. Write Synchronization Modes (`local`, `write_through`, `write_behind`)](#3-write-synchronization-modes-local-write_through-write_behind)
- [Security: Capabilities & Named Users](#security-capabilities--named-users)
- [SQL Dialect Coverage](#sql-dialect-coverage)
- [Production Deployment & Administration](#production-deployment--administration)
  - [Packaging & Installation](#packaging--installation)
  - [Configuration (`/etc/aios/asql.conf`)](#configuration-etcaiosasqlconf)
  - [Operator Tools & Acceptance Checks](#operator-tools--acceptance-checks)
  - [Online Backup & Restore (`asql-export` / `asql-import`)](#online-backup--restore-asql-export--asql-import)
- [Troubleshooting & FAQ](#troubleshooting--faq)

---

## Key Highlights

- **Pure In-Memory Speed**: Sub-microsecond reads, columnar indexing, plan caching, and vectorization with zero external dependencies (no `libmysqlclient`, `sqlite`, or external query engines).
- **Dual Protocols**: Speaks standard MySQL 5.7 wire protocol on port `3307` and UNIX domain sockets, alongside a length-prefixed raw JSON RPC service on port `7311`.
- **First-Class Grain Integration**: In-process direct execution via `sql()`, client networking via `db_connect()` and `db_query()`, safe `?` parameterized bindings, nested savepoint transactions via `db_transaction()`, and reactive invalidation via `live()`.
- **Reactive Primitives (Things & Cells)**: Materialized query aggregates that update incrementally on mutation, plus per-key bitsets and counters that trigger HTTP webhooks (`NOTIFY`) and emails (`MAIL`) automatically.
- **Dialect Versatility**: MySQL 5.7 / MariaDB dialect coverage plus PostgreSQL syntax (`::type` casts, `generate_series`, `LATERAL`, `ON CONFLICT DO UPDATE`, `FILTER (WHERE ...)`), window functions, and recursive CTEs.
- **ACID Durability**: Group-committed write-ahead redo logging (`redo.log`), background checkpoints, and instant crash recovery.
- **Flexible Write Modes**: Run self-contained (`local`), synchronously write-through to an upstream/downstream database (`write_through`), or queue asynchronously via WAL drain (`write_behind`).
- **Cryptographic Capability Security**: Attenuable, bearer-token credentials (`CAPABILITY MINT` / `REVOKE`) with table, column, row-budget, and TTL restrictions alongside standard named users.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                                  CLIENT APPLICATIONS                                   │
│    Native Grain Apps · MySQL CLI · DBeaver / Navicat · Python · Node.js · PHP · Go     │
└───────────────────────────┬────────────────────────────────┬───────────────────────────┘
                            │ MySQL Wire Protocol (port 3307)│ Raw JSON RPC (port 7311)
                            │ or UNIX Domain Socket          │
┌───────────────────────────▼────────────────────────────────▼───────────────────────────┐
│                                       ASQL ENGINE                                      │
│  ┌──────────────────────────────────────────────────────────────────────────────────┐  │
│  │ AST Parser & Execution Planner (Subquery Cache, CTE Evaluator, Window Functions) │  │
│  └────────────────────────────────────────┬─────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────▼─────────────────────────────────────────┐  │
│  │ In-Memory Data Store & Columnar Indices                                          │  │
│  │  • PK / Unique Hash Index   • Probe Index (Hash & Sorted)                        │  │
│  │  • Auto-Increment Tracker   • Dynamic info_schema Views                          │  │
│  └──────────────┬─────────────────────────┬─────────────────────────┬───────────────┘  │
│                 │                         │                         │                  │
│  ┌──────────────▼───────────┐  ┌──────────▼──────────────┐  ┌───────▼───────────────┐  │
│  │ Things Subsystem         │  │ Cells Subsystem         │  │ Multi-Tenant Schemas  │  │
│  │ • Live Grouped Queries   │  │ • Per-Key Bitsets       │  │ (appdb, shop, stats)  │  │
│  │ • Incremental Updates    │  │ • Automated Webhooks    │  │                       │  │
│  │ • WAIT THING / WHAT IF   │  │ • OAuth Mail Triggers   │  │                       │  │
│  └──────────────────────────┘  └─────────────────────────┘  └───────┬───────────────┘  │
│                                                                     │                  │
│  ┌──────────────────────────┐  ┌─────────────────────────┐  ┌───────▼───────────────┐  │
│  │ Durability Subsystem     │  │ Write Synchronization   │  │ CDC Consumer          │  │
│  │ • Write-Ahead Redo Log   │  │ • local (self-contained)│  │ (Zero-RAM             │  │
│  │ • Periodic Checkpoints   │  │ • write_through (sync)  │  │  Binlog Ingress)      │  │
│  │ • Export Snapshots       │  │ • write_behind (WAL)    │  │                       │  │
│  └──────────────────────────┘  └──────────┬──────────────┘  └───────▲───────────────┘  │
└───────────────────────────────────────────┼─────────────────────────┼──────────────────┘
                                            │ Forward Writes          │ COM_BINLOG_DUMP
                                            ▼                         │
                             ┌─────────────────────────────┐   ┌──────┴──────────────────┐
                             │ Downstream Database/Gateway │   │ Upstream MySQL/MariaDB  │
                             └─────────────────────────────┘   └─────────────────────────┘
```

---

## 30-Second Quickstart

### 1. Launch a Local ASQL Server
Run a self-contained local server on loopback with no external dependencies:

```bash
# Starts ASQL MySQL wire listener on port 3307 and a throwaway data root
AIOS_NO_ACCEL=1 aios --root /tmp/asql-quickstart exec "sql serve port=3307 bind=127.0.0.1"
```

### 2. Connect and Query
With no account configured, loopback connections (`127.0.0.1`) are granted unrestricted local access under any username:

```bash
# Connect with standard mysql CLI
mysql -h 127.0.0.1 -P 3307 -u root -e "
  CREATE DATABASE shop;
  USE shop;
  CREATE TABLE products (
    id INT PRIMARY KEY AUTO_INCREMENT,
    sku VARCHAR(32) NOT NULL,
    price DECIMAL(10,2) NOT NULL,
    stock INT DEFAULT 0
  );
  INSERT INTO products (sku, price, stock) VALUES ('BOLT-01', 0.45, 1200), ('NUT-01', 0.25, 3400);
  SELECT sku, price, stock FROM products WHERE stock > 1500;
"
```

Output:
```
+--------+-------+-------+
| sku    | price | stock |
+--------+-------+-------+
| NUT-01 |  0.25 |  3400 |
+--------+-------+-------+
```

Data commits to `/tmp/asql-quickstart/sql_db/` with durability logs and survives server restarts.

---

## Client Integration & Usage

### 1. Native Grain Language (AIOS First-Class API)

Grain scripts have three direct access mechanisms to ASQL:

#### A. In-Process Pointer Access: `sql()` and `sql_on()`
When running inside the AIOS runtime (`aios grain ...`, system services, or bare-metal), Grain accesses `SqlDatabase` directly in memory without socket serialization overhead:

```grain
// Result queries return a list of maps:
let items = sql("SELECT id, sku, price FROM shop.products WHERE price < 1.00")
for item in items {
  print(item.sku + ": $" + str(item.price))
}

// Data modification statements return {"affected": <integer>}:
let res = sql("UPDATE shop.products SET stock = stock + 100 WHERE sku = 'BOLT-01'")
print("Updated " + str(res.affected) + " records")

// Fleet mesh query across peer nodes:
let cluster_stats = sql_on("worker-node-2", "SELECT COUNT(*) AS total FROM logs")
```

#### B. Client Network Protocol: `db_connect()`, `db_query()`, `db_transaction()`
For standalone Grain processes, background workers, or external network calls:

```grain
// Connect via TCP or UNIX domain socket:
let res = db_connect("127.0.0.1", 3307, "app", "secretpass", "shop")
if !res.ok {
  print("Connection error: " + res.error)
  return nil
}
let db = res.conn

// Safe Parameter Binding with ? placeholders:
let low_stock = db_query(db, "SELECT sku, stock FROM products WHERE stock < ?", [500])
for row in low_stock.records {
  print(row.sku + " has low stock: " + str(row.stock))
}

// Automatic list expansion for SQL IN (?):
let skus = ["BOLT-01", "NUT-01", "WASH-02"]
let batch = db_query(db, "SELECT sku, price FROM products WHERE sku IN (?)", [skus])

// ACID Transactions with automatic rollback and nested savepoints:
let order_id = db_transaction(db, fn(tx) {
  let ins = db_query(tx, "INSERT INTO orders (created_at, total) VALUES (NOW(), ?)", [45.20])
  if !ins.ok { error("Failed to insert order: " + ins.error) }
  
  let deduct = db_query(tx, "UPDATE products SET stock = stock - 10 WHERE sku = ?", ["BOLT-01"])
  if !deduct.ok { error("Failed stock update: " + deduct.error) }
  
  return ins.last_insert_id
})
```

#### C. Reactive Polling: `live()`
Drive reactive dashboards without querying heavy tables repeatedly. `live()` polls ASQL's mutation tracker via `ASQL_CHANGES()` and re-evaluates the callback only when underlying table tokens change:

```grain
live(db, ["products", "orders"], fn() {
  let snapshot = db_query(db, "SELECT COUNT(*) AS pending FROM orders WHERE status = 'open'")
  render_dashboard(snapshot.records[0].pending)
})
```

---

### 2. Standard MySQL Wire Protocol (CLI, Python, Node.js, PHP, Go)

ASQL speaks native MySQL 5.7 wire protocol on port `3307` and over local UNIX domain sockets:

#### Python (`pymysql` / `mysqlclient`)
```python
import pymysql

conn = pymysql.connect(
    host="127.0.0.1",
    port=3307,
    user="app",
    password="wirepassword",
    database="shop",
    cursorclass=pymysql.cursors.DictCursor
)

with conn.cursor() as cursor:
    cursor.execute("SELECT sku, price FROM products WHERE stock > %s", (100,))
    rows = cursor.fetchall()
    for row in rows:
        print(f"{row['sku']}: ${row['price']}")
```

#### Node.js (`mysql2`)
```javascript
import mysql from 'mysql2/promise';

const pool = mysql.createPool({
  host: '127.0.0.1',
  port: 3307,
  user: 'app',
  password: 'wirepassword',
  database: 'shop',
  connectionLimit: 10
});

const [rows] = await pool.query('SELECT sku, price FROM products WHERE stock > ?', [100]);
console.log(rows);
```

#### PHP (PDO)
```php
$dsn = "mysql:host=127.0.0.1;port=3307;dbname=shop;charset=utf8mb4";
// Or UNIX socket: "mysql:unix_socket=/opt/aios/run/asql.sock;dbname=shop;charset=utf8mb4"
$pdo = new PDO($dsn, "app", "wirepassword", [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
]);

$stmt = $pdo->prepare("SELECT sku, price FROM products WHERE stock > :stock");
$stmt->execute(['stock' => 100]);
$products = $stmt->fetchAll();
```

---

### 3. GUI Tools (DBeaver, DataGrip, TablePlus, Navicat)

ASQL dynamically synthesizes MySQL `information_schema` views (`tables`, `columns`, `schemata`, `views`, etc.). You can connect standard database GUI tools directly:

1. **Driver**: MySQL
2. **Host / Port**: `127.0.0.1` / `3307`
3. **Database**: `shop` (or your configured schema)
4. **User / Password**: Configured operator account or capability token

---

### 4. Low-Overhead JSON RPC (Port 7311)

For microservices that avoid SQL drivers, ASQL exposes a length-prefixed JSON RPC endpoint on port `7311`:
* **Framing**: 4-byte big-endian payload length + UTF-8 JSON body.
* **Safety**: RPC `sql` operations are strictly **read-only** (`SELECT`, `SHOW`, `DESCRIBE`, `EXPLAIN`, `WITH`). Writes are rejected at the gate.

```bash
# Request payload:
# {"op": "sql", "db": "shop", "token": "tok123", "query": "SELECT sku, price FROM products LIMIT 2"}

# Response:
# {"ok": true, "columns": ["sku", "price"], "rows": [["BOLT-01", "0.45"], ["NUT-01", "0.25"]], "row_count": 2}
```

---

## Reactive Primitives: Things and Cells

Beyond standard relational tables, ASQL maintains two reactive in-memory constructs:

### Things: Stateful Grouped Aggregates

A **Thing** is a live, materialized grouped query result that updates incrementally in memory as underlying rows mutate. Things avoid repeated table scans and can notify listeners or take programmatic action when conditions become true:

```sql
-- Define a Thing that maintains real-time item counts and balances per department:
CREATE THING dept_summary ON transactions KEY dept_id AS
  SELECT dept_id, COUNT(*) AS txn_count, SUM(amount) AS total_spent
  FROM transactions
  GROUP BY dept_id;

-- Read instantaneous materialized state:
SHOW THING dept_summary;
SHOW THING dept_summary KEYS (10, 20);

-- Inspect underlying source rows responsible for a specific aggregate:
EXPLAIN THING dept_summary KEY 10;

-- Wait for changes without polling (blocking until mutation or timeout):
WAIT THING dept_summary SINCE '2026-11-04 12:00:00' TIMEOUT 5000;

-- Preview the exact impact of a pending write before executing it:
WHAT IF INSERT INTO transactions (dept_id, amount) VALUES (10, 250.00)
ON THING dept_summary;
```

---

### Cells: Per-Key Bits, Counters & Webhook/Email Triggers

**Cells** track small per-key bitsets (flags) and integer counters that react to row updates. When a bit rises or falls, Cells execute automated actions, including templated HTTPS webhooks and authenticated emails:

```sql
-- Track order fulfillment steps per order_id:
CREATE CELLS order_progress ON orders KEY order_id (
  scanned BIT DEFAULT 0,
  packed BIT DEFAULT 0,
  shipped BIT DEFAULT 0,
  item_count INT DEFAULT 0
);

-- Store an encrypted secret (AES-256-GCM) for notification payloads:
CREATE SECRET slack_webhook AS 'https://hooks.slack.com/services/T00/B00/XXXX';

-- Automatically dispatch an HTTPS POST when the 'shipped' bit flips from 0 to 1:
ON RISE shipped NOTIFY '{{secret:slack_webhook}}'
  HEADER 'Content-Type: application/json'
  BODY '{"text": "Order #{{key}} has shipped! Total items: {{item_count}}"}';

-- Configure an OAuth mailer and send an automated email alert on threshold:
CREATE MAILER alerts_mailer OAUTH 'google' ACCOUNT 'admin@example.com';

ON RISE alert_triggered MAIL alerts_mailer
  TO 'ops@example.com'
  SUBJECT 'Fulfillment Warning for Order #{{key}}'
  BODY 'Order #{{key}} has flagged an exception.';
```

---

## Operating Modes & Write Synchronization

ASQL supports three primary operational architectures and three write synchronization policies:

| Mode | Target | Write Latency | Downstream Dependency | Downstream Failure Behavior |
|---|---|---|---|---|
| **`local`** (Default) | In-Memory + Local Redo Log | Sub-millisecond | None | Completely independent. |
| **`write_through`** | Downstream first, then local | 1x Downstream RTT | Synchronous | Refuses write locally; nothing committed. |
| **`write_behind`** | Local first, then async WAL drain | Sub-millisecond | Asynchronous | Commits locally; queues in persistent WAL and retries. |

### 1. Standalone Primary Database
Run ASQL as an independent, primary in-memory SQL database without an upstream or downstream database:
- **Write-Ahead Redo Log**: Every mutating transaction appends to `redo.log`. Sync behavior is controlled by `AIOS_REDO_SYNC` (default group commit).
- **Checkpoints**: Automatically snapshots to disk every 20,000 statements or 64 MB of redo (`AIOS_CHECKPOINT_SECS`, default 30s). On boot, ASQL loads the snapshot and replays trailing redo entries.
- **Starting Standalone**:
  ```bash
  # Standalone service daemon:
  build/aios-engine --root=/opt/aios/root --port=3307 --sql-user=app --sql-pass-file=/opt/aios/mysql.pass
  ```

### 2. Real-Time CDC Replica (Binlog Read Accelerator)
Connect ASQL to an upstream MariaDB or MySQL instance as a replication slave (`COM_BINLOG_DUMP`):
- **Zero-RAM-Bloat Ingress**: Streams row-based binlog events (`WRITE_ROWS`, `UPDATE_ROWS`, `DELETE_ROWS`) directly into columnar in-memory tables.
- **Table Whitelist**: Replicate only specified tables (`CDC_TABLES=orders,products`), ignoring unneeded tables to bound memory usage.
- **Automatic Seeding**: Missing tables are snapshotted automatically over the wire during startup before binlog streaming begins.

### 3. Write Synchronization Modes (`local`, `write_through`, `write_behind`)
- **`local`**: Writes remain strictly local to ASQL. In CDC replica mode, local writes are committed in-memory and **never written upstream**, providing a safe local staging/analytics sandbox.
- **`write_through`**: Mutations are executed on the downstream MySQL/MariaDB server first; once confirmed, they are applied to ASQL's in-memory tables.
- **`write_behind`**: Writes commit locally in sub-milliseconds, appending to `<store>/write_behind.wal`. A background worker drains transactions downstream sequentially every 20ms.

---

## Security: Capabilities & Named Users

ASQL supports two complementary authentication models:

### 1. Cryptographic Capability Credentials
Capability credentials are cryptographically signed bearer tokens that attenuate authority (restricting accessible schemas, operations, execution duration, and row budget):

```sql
-- Mint a restricted credential over SQL or CLI:
CAPABILITY MINT 'shop.products,shop.orders' RIGHTS 'read' TTL 3600 ROWS 5000;
-- Returns a signed bearer token

-- Revoke a compromised token:
CAPABILITY REVOKE '<token>';
```

Holders can attenuate credentials **offline** without contacting the database:
```bash
# Attenuate token to a single table, shorter TTL, and lower row limit:
aios asqlcap attenuate <token> tables=shop.products rights=read ttl=300 rows=100
```

Clients log in with username `"cap"` and the token as password:
```bash
mysql -h 127.0.0.1 -P 3307 -u cap --enable-cleartext-plugin -p"<token>" shop
```

### 2. Named Users
Standard database logins bound to explicit permission scopes:

```sql
CREATE USER 'reporter' IDENTIFIED BY 'rep_secret' TABLES 'shop.*' RIGHTS 'read' TTL 86400 ROWS 50000;
ALTER USER 'reporter' RIGHTS 'read,write';
SHOW USERS;
DROP USER 'reporter';
```

---

## SQL Dialect Coverage

ASQL implements a rich relational SQL dialect:

* **DDL**: `CREATE DATABASE`, `USE`, `CREATE/DROP/ALTER TABLE`, `PRIMARY KEY` (including composite keys), `AUTO_INCREMENT`, `UNIQUE`, `DEFAULT`, `CREATE VIEW`, `CREATE TYPE ... AS ENUM`.
* **DML**: `SELECT`, `INSERT ... VALUES`, `INSERT ... ON DUPLICATE KEY UPDATE`, `REPLACE INTO`, `UPDATE`, `DELETE`, multi-table updates (`UPDATE a JOIN b ... SET`), and multi-table deletes.
* **Joins**: `INNER JOIN`, `LEFT/RIGHT/FULL [OUTER] JOIN`, `CROSS JOIN`, `NATURAL JOIN`, `JOIN ... USING (...)`, and comma joins with automatic hash-join optimization.
* **CTEs & Subqueries**: Correlated scalar subqueries, `WITH`, and `WITH RECURSIVE` (with configurable `max_recursive_iterations`).
* **Window Functions**: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `PERCENT_RANK()`, `CUME_DIST()`, `NTILE()`, `LAG()`, `LEAD()`, `FIRST_VALUE()`, `LAST_VALUE()`, `NTH_VALUE()`, with `PARTITION BY`, `ORDER BY`, and `ROWS/RANGE BETWEEN` frame specifications.
* **PostgreSQL Extensions**:
  * Cast operator `expr::type` (`int`, `numeric(p,s)`, `text`, `boolean`, `timestamp`).
  * `a IS [NOT] DISTINCT FROM b`.
  * `generate_series(start, stop [, step])` and `LATERAL` joins.
  * `INSERT ... ON CONFLICT (cols) DO NOTHING | DO UPDATE SET ...`
  * Aggregate filters: `agg(...) FILTER (WHERE ...)`.
* **JSON Functions**: `JSON_EXTRACT`, `JSON_ARRAYAGG`, `JSON_OBJECTAGG`, `JSON_SET`, `JSON_REPLACE`, `JSON_REMOVE`, `JSON_CONTAINS_PATH`, etc.
* **Stored Procedures & Triggers**: `CREATE PROCEDURE`, `CREATE FUNCTION`, `CREATE TRIGGER` (`BEFORE`/`AFTER`), control flow (`IF`, `WHILE`, `LOOP`, `CURSOR`, `HANDLER`).

---

## Production Deployment & Administration

### Packaging & Installation
Build a portable release binary against glibc 2.17+ to run across all enterprise Linux distributions (CentOS 7, AlmaLinux 8/9, Rocky, Debian, Ubuntu):

```bash
# Build binary package
TARGETS="aios-cli aios-asql-top aios-asql-console" packaging/build-centos7.sh origin/main build-centos7
packaging/pack.sh build-centos7 packaging/dist

# Install on target host
tar xzf aios-asql-*.tar.gz && cd aios-asql-*/
sudo ./install-asql.sh                 # provisions user 'aios', /opt/aios, systemd unit
sudo systemctl restart aios-compute
```

### Configuration (`/etc/aios/asql.conf`)
The central service configuration file:

```ini
AIOS_ROOT=/opt/aios/root
SQL=1
SQL_DB=sql_db
RPC_PORT=7311
PUBLIC=1
LANES=8

# MySQL Wire Listener
MYSQL_PORT=3307
MYSQL_BIND=0.0.0.0
MYSQL_USER=app
MYSQL_PASS_FILE=/opt/aios/mysql.pass
MYSQL_SOCKET=/opt/aios/run/asql.sock

# CDC Ingress (optional, for MariaDB/MySQL replication)
CDC_HOST=127.0.0.1
CDC_PORT=3306
CDC_USER=replicator
CDC_PASS_FILE=/opt/aios/cdc.pass
CDC_DB=shop
CDC_TABLES=products,orders,inventory
```

### Operator Tools & Acceptance Checks

* **Acceptance Verification (`asql-check`)**:
  ```bash
  /opt/aios/bin/asql-check            # full verification of mirror parity & things
  /opt/aios/bin/asql-check --quick    # quick status of service, wire socket, and binlog lag
  ```
* **Interactive Terminal Top (`aios-asql-top`)**:
  ```bash
  aios-asql-top                       # real-time screen showing active queries, memory, and CDC lag
  aios-asql-top -1                    # one-line snapshot for monitoring scripts
  ```
* **Interactive CLI Prompt (`aios-asql-console`)**:
  ```bash
  aios-asql-console                   # direct SQL prompt
  aios-asql-console -e "SHOW STATUS LIKE 'Asql_%'"
  ```

### Online Backup & Restore (`asql-export` / `asql-import`)

Create point-in-time consistent backups while queries are running:

```bash
# Export consistent snapshot archive (includes tables, Things, and Cells):
packaging/asql-export.sh -o /tmp/asql-backup.tar.gz

# Restore snapshot to another host/root:
packaging/asql-import.sh /tmp/asql-backup.tar.gz
```

---

## Troubleshooting & FAQ

### 1. Permissive vs. Strict SQL Mode
By default, ASQL runs in permissive mode (`sql_mode = ''`). To enforce strict validation (rejecting truncation and invalid zero-dates), set:
```bash
export AIOS_SQL_STRICT=1
```
`PRIMARY KEY` and `UNIQUE` constraints are always enforced regardless of mode.

### 2. Permission Denied on Loopback Connection
If you configured `MYSQL_USER` and `MYSQL_PASS_FILE`, all connections (including loopback and UNIX socket) **must provide the valid password**. If no user is configured, loopback enters freely, but remote clients are blocked.

### 3. Exit Code 78 on Startup
Exit code `78` from `asql-serve` indicates that `/etc/aios/asql.conf` is missing or unreadable by the `aios` service user. Verify file ownership and permissions (`root:aios 0640`).

### 4. Forcing CDC Table Resync
To force ASQL to re-seed a table from upstream:
```bash
sudo systemctl stop aios-compute
rm -f /opt/aios/root/sql_db/tables/<hex_db_table_name>*
sudo systemctl start aios-compute
```
ASQL will snapshot the table anew from upstream before resuming replication.

---

*For detailed internal design and engine specifications, see [docs/asql-internals.md](docs/asql-internals.md), [docs/asql-source-layout.md](docs/asql-source-layout.md), and [docs/asql-benchmarks.md](docs/asql-benchmarks.md).*
