# ASQL against MariaDB, CedarDB and DuckDB: 2026-11-04

**Engines, run on the same machine one after another**:
- **ASQL** `5.7.44-aios-asql`, commit `42f0419` plus the `LOAD DATA` change (`aios` md5 `4917ece8`), 8 lanes, native
  on macOS. Started with `AIOS_SQL_SECURE_FILE_PRIV` set to the data folder, which `LOAD DATA INFILE` needs.
- **MariaDB** `10.11.9-MariaDB-ubu2204` (InnoDB), Docker image `mariadb:10.11.9`, `innodb_buffer_pool_size=1G`,
  otherwise defaults (`innodb_flush_log_at_trx_commit=1`, query cache off).
- **CedarDB** `v2026-11-04` ("PostgreSQL 16.3 compatible", arm64), Docker image `cedardb/cedardb:latest`, defaults.
- **DuckDB** `v1.5.5 (Variegata) d8cdaa33fd`, in process through its C API, on a database file.

Every figure below was measured in this run. The harness is `tools/asql-bench/` (its README has the commands);
earlier passes against MariaDB are in this file's history (`git show 42f0419:docs/asql-benchmarks.md`).

---

## Contents

- [Machine and method](#machine-and-method)
- [Summary](#summary)
- [Test 1: Loading 500,000 rows](#test-1-loading-500000-rows)
- [Test 2: Analytical queries](#test-2-analytical-queries)
- [Test 3: Point lookups](#test-3-point-lookups)
- [Test 4: Single-row updates](#test-4-single-row-updates)
- [Test 5: The ERP's own data, ASQL against DuckDB](#test-5-the-erps-own-data-asql-against-duckdb)
- [Why ASQL is slower on DOUBLE columns](#why-asql-is-slower-on-double-columns)
- [Storage](#storage)
- [Reproducing it](#reproducing-it)

---

## Machine and method

- **Host**: Apple M3 Pro, 12 cores, 18 GB, macOS 26.2. ASQL and DuckDB ran natively. MariaDB and CedarDB ran in
  Docker Desktop's Linux VM (12 CPUs, 8 GB), reached over a loopback port. **That is not an even field**: the two
  containers' disk syncs go to the VM's virtual disk, ASQL's to the Mac's SSD (Test 4 shows what that costs).
- **Other load**: the machine was in use by other work; the 1-minute load average was 3.9 to 5.0 during the final
  runs (up to 10 in earlier ones). Each test ran twice; the ranges below span the two passes.
- **Data**: `bench_sales`, 500,000 rows from a fixed seed (`tools/asql-bench/gen.py`), 21,034,548 bytes of CSV:
  `id INT PRIMARY KEY, customer_id INT (1-50,000), category_id INT (1-50), amount (1.00-1000.00),
  trans_date DATE (2024-01-01 + 0-729 days), status VARCHAR(16) (5 values)`.
- **The amount column, both ways**: every test ran twice over, once with `amount DOUBLE` and once with
  `amount DECIMAL(12,2)` (`NUMERIC(12,2)` in CedarDB), each a fresh table on every engine. It matters to ASQL (see
  "Why ASQL is slower on DOUBLE columns"). The ERP uses both: in summitholding `stockmoves.price`, `qty`,
  `debtortrans.ovamount` and `salesorderdetails.unitprice` are DOUBLE, and 625 of its columns are DOUBLE or FLOAT
  against 472 DECIMAL.
- **Timing**: per statement, around the client call that sends it and reads its whole answer: PHP `mysqli`
  (ASQL, MariaDB) or `pgsql` (CedarDB) over one persistent connection; for DuckDB around `duckdb_query`, in process
  (no network and no protocol, which favours it in Tests 3 and 4).
- **Queries**: one warm-up run, then 5 timed runs; the figure is the median. ASQL keeps answers in a result cache,
  so its queries were timed with `SQL_NO_CACHE`: computed every time, as the others were. What the cache answers is
  shown separately.
- **Answers**: every query's rows from every engine were compared with MariaDB's (numbers within 1e-9 relative: sums of
  DOUBLEs differ in their last bits with the order they are added in). All five queries matched on all four engines,
  for both amount types and both passes.

---

## Summary

Milliseconds unless marked; lower is better except rows/s, QPS and TPS. Each cell spans the two passes. Best of the
four in bold (a tie in bold for both).

**With `amount DOUBLE`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 1,469-1,478 | 539-622 | 265-275 | **219-222** |
| 100 `INSERT`s of 5,000 rows | 1,853-1,903 | **1,126-1,137** | 3,628-3,642 | 6,363-6,499 |
| Full-table aggregate | 2.63-2.67 | 45.2-45.3 | 3.52-3.61 | **1.30-1.32** |
| `GROUP BY` + top 10 | 3.13-3.28 | 51.4-51.6 | 7.29-7.77 | **0.82-0.83** |
| Filtered range aggregate | 1.08-1.20 | 35.5-35.9 | **0.91-0.92** | **0.92-0.93** |
| Pivot, 3 `SUM(CASE ...)` | 10.1-10.3 | 87.9-88.7 | 11.9-12.5 | **2.98-3.05** |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 11.6-11.8 | 182-195 | 110-113 | **10.6-10.7** |
| Point lookup, median | **0.035** | 0.228-0.236 | 0.347-0.351 | 0.097-0.099 |
| Point lookups, one client | **27,200-27,628 QPS** | 4,118-4,234 | 2,695-2,755 | 9,969-10,092 |
| Single-row update, median | 3.23-3.86 | 0.227-0.241 | 0.360-0.373 | **0.155-0.157** |
| Single-row updates, one client | 281-286 TPS | 3,945-4,156 | 2,594-2,675 | **6,242-6,305** |

**With `amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 1,564-1,580 | 556-600 | 227-301 | **218-219** |
| 100 `INSERT`s of 5,000 rows | 1,942-1,944 | **1,164-1,179** | 3,710-3,795 | 6,411-6,478 |
| Full-table aggregate | 0.68 | 69.6-69.7 | 2.98-3.12 | **0.54-0.55** |
| `GROUP BY` + top 10 | 0.83-0.86 | 74.3 | 7.06-7.10 | **0.73-0.75** |
| Filtered range aggregate | **0.61-0.64** | 40.1-40.2 | 0.82-0.83 | **0.62-0.66** |
| Pivot, 3 `SUM(CASE ...)` | **2.33-2.37** | 153-154 | 11.6-11.9 | 2.78-2.90 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 11.6-11.7 | 209-213 | 112-115 | **10.1-10.4** |
| Point lookup, median | **0.035** | 0.226-0.233 | 0.371-0.378 | 0.096-0.098 |
| Point lookups, one client | **26,325-27,425 QPS** | 4,158-4,296 | 2,567-2,599 | 10,030-10,192 |
| Single-row update, median | 3.77-3.85 | 0.229-0.230 | 0.382-0.387 | **0.162-0.164** |
| Single-row updates, one client | 284-290 TPS | 3,909-4,133 | 2,503-2,513 | **6,010-6,087** |

**On the ERP's own data** (Test 5, 12.5M rows of summitholding):

| | ASQL | DuckDB |
|---|---|---|
| Reports with joins: `thisoverlast`, `nopurchase`, `brandbylocation` | **392-393, 725-759, 826-920 ms** | 493-522, 1,412-1,548, 1,331-1,475 ms |
| Plain scans of `stockmoves`' DOUBLE columns: full, grouped, filtered | 224, 86, 8.3 ms | **13-15, 15-18, 2-3 ms** |

In short:
- **Against MariaDB**: ASQL answers the analytical queries 8.5-33x faster with a DOUBLE amount and 18-102x with a
  DECIMAL one, and a point lookup 6-7x faster. MariaDB loads files 2.4-2.8x faster, takes the wire `INSERT`s 39-40% less
  time, and does single-row updates 14x faster (the update gap is a macOS disk-sync cost, Test 4).
- **Against DuckDB, DECIMAL**: level. DuckDB is 1.1-1.3x faster on the scan, the `GROUP BY` and the window, the range
  query is a tie, and ASQL is 1.2x faster on the pivot.
- **Against DuckDB, DOUBLE**: DuckDB is faster, by 1.1x on the window, 1.2-1.3x on the range query, 2.0x on the full scan,
  3.4x on the pivot and 3.8-4.0x on the `GROUP BY`. On the real `stockmoves` the gap is 3-17x. ASQL adds a DOUBLE column on
  one thread, on purpose; see "Why ASQL is slower on DOUBLE columns".
- **Against CedarDB**: ASQL is faster on every analytical query but the DOUBLE range query (CedarDB 1.2-1.3x faster),
  by up to 10x on the window. CedarDB loads files 5-7x faster and updates rows 9x faster.
- **Where ASQL is ahead on the ERP**: the reports with joins and pivots: 1.3-2.1x faster than DuckDB.
- **Where ASQL is behind**: loading a file (1.5 s for 500k rows against 0.22-0.6 s), single-row updates on a Mac, and
  scans of DOUBLE columns.

---

## Test 1: Loading 500,000 rows

### A. From the CSV file, by the engine's own loader

| Engine | Statement | DOUBLE, ms | DECIMAL, ms |
|---|---|---|---|
| DuckDB | `COPY bench_load FROM '.../bench_sales.csv' (FORMAT csv, HEADER false)` | **219-222** | **218-219** |
| CedarDB | `COPY bench_load FROM '/data/bench_sales.csv' WITH (FORMAT csv)` | 265-275 | 227-301 |
| MariaDB | `LOAD DATA INFILE '/data/bench_sales.csv' INTO TABLE bench_load FIELDS TERMINATED BY ','` | 539-622 | 556-600 |
| ASQL | `LOAD DATA INFILE '.../bench_sales.csv' INTO TABLE bench_load FIELDS TERMINATED BY ','` | 1,469-1,478 | 1,564-1,580 |

`LOAD DATA [LOCAL] INFILE` is new in this run (it was an `unsupported statement` error in the first pass of this
document): the file's rows go through the same path as an `INSERT`, in one atomic statement (README-ASQL.md,
"Loading a file"). 500,000 rows came in every time, and a `kill -9` right after a load lost nothing (the redo log
replayed 33 records, the table had its 500,000 rows and the same sum).

ASQL is 2.4-2.8x slower than MariaDB at this and 6.7-7.2x slower than DuckDB. In a traced run of 500,000 rows, 1.0-1.25 s
of the 1.3-1.5 s was the engine's `INSERT` row path (about 2 µs a row: conversions, key checks, ownership of the
row in the transaction, the column stores); reading and splitting the file was about 0.2 s, building the redo text
0.04 s and the commit 0.02-0.08 s. A path that appends columns in bulk, as `sql copy` does, would be the way to
close the gap; it is not done.

Also measured: copying the same 500,000 rows from the MariaDB container into a new ASQL root with `aios sql copy`:
**1.40-1.72 s** for the whole command, process start included.

### B. Over the wire: 100 `INSERT INTO bench_sales VALUES (...)` statements of 5,000 rows

| Engine | DOUBLE, ms | DECIMAL, ms | rows/s (DOUBLE) |
|---|---|---|---|
| MariaDB | **1,126-1,137** | **1,164-1,179** | 440,000-444,000 |
| ASQL | 1,853-1,903 | 1,942-1,944 | 263,000-270,000 |
| CedarDB | 3,628-3,642 | 3,710-3,795 | 137,000-138,000 |
| DuckDB (in process) | 6,363-6,499 | 6,411-6,478 | 77,000-79,000 |

One connection, autocommit: each statement is one transaction. MariaDB took 39-40% less time than ASQL.

---

## Test 2: Analytical queries

The same SQL text on all four engines; medians of 5 runs after a warm-up, in ms. Each query, DOUBLE then DECIMAL.

### Full-table aggregate
```sql
SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM bench_sales
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | 2.63-2.67 | 45.2-45.3 | 3.52-3.61 | **1.30-1.32** |
| DECIMAL | 0.68 | 69.6-69.7 | 2.98-3.12 | **0.54-0.55** |

### `GROUP BY` and top 10
```sql
SELECT category_id, COUNT(*) AS n, SUM(amount) AS total FROM bench_sales
GROUP BY category_id ORDER BY total DESC LIMIT 10
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | 3.13-3.28 | 51.4-51.6 | 7.29-7.77 | **0.82-0.83** |
| DECIMAL | 0.83-0.86 | 74.3 | 7.06-7.10 | **0.73-0.75** |

### Filtered range aggregate
```sql
SELECT COUNT(*), SUM(amount) FROM bench_sales
WHERE trans_date BETWEEN '2024-03-01' AND '2024-08-31' AND amount > 100
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | 1.08-1.20 | 35.5-35.9 | **0.91-0.92** | **0.92-0.93** |
| DECIMAL | **0.61-0.64** | 40.1-40.2 | 0.82-0.83 | **0.62-0.66** |

### Pivot
```sql
SELECT category_id,
  SUM(CASE WHEN status = 'shipped'  THEN amount ELSE 0 END) AS s,
  SUM(CASE WHEN status = 'pending'  THEN amount ELSE 0 END) AS p,
  SUM(CASE WHEN status = 'returned' THEN amount ELSE 0 END) AS r
FROM bench_sales GROUP BY category_id ORDER BY category_id
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | 10.1-10.3 | 87.9-88.7 | 11.9-12.5 | **2.98-3.05** |
| DECIMAL | **2.33-2.37** | 153-154 | 11.6-11.9 | 2.78-2.90 |

### Window function
```sql
SELECT COUNT(*), SUM(amount) FROM (
  SELECT amount, ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY trans_date DESC, id DESC) AS rn
  FROM bench_sales) t WHERE rn = 1
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | 11.6-11.8 | 182-195 | 110-113 | **10.6-10.7** |
| DECIMAL | 11.6-11.7 | 209-213 | 112-115 | **10.1-10.4** |

### What ASQL's result cache answers

The same statements sent again without `SQL_NO_CACHE`, after two runs to be admitted, took **0.03-0.12 ms** for all five
queries with either amount type. The other engines have no result cache; every comparison above is a computed answer.

---

## Test 3: Point lookups

10,000 `SELECT id, customer_id, amount, status FROM bench_sales WHERE id = N`, random ids, one after another on one
connection (DuckDB: in process).

| Engine | median (DOUBLE) | p99 (DOUBLE) | per second (DOUBLE) | per second (DECIMAL) |
|---|---|---|---|---|
| ASQL | **0.035 ms** | 0.060-0.061 ms | **27,200-27,628** | **26,325-27,425** |
| DuckDB (in process) | 0.097-0.099 ms | 0.123-0.126 ms | 9,969-10,092 | 10,030-10,192 |
| MariaDB | 0.228-0.236 ms | 0.301-0.329 ms | 4,118-4,234 | 4,158-4,296 |
| CedarDB | 0.347-0.351 ms | 0.497-0.543 ms | 2,695-2,755 | 2,567-2,599 |

Almost every id is a different one, so ASQL's result cache answers next to none of these. The MariaDB and CedarDB
figures include the hop into Docker's VM.

---

## Test 4: Single-row updates

2,000 `UPDATE bench_sales SET amount = amount + 0.01 WHERE id = N`, random ids, autocommit, one connection.

| Engine | median (DOUBLE) | p99 (DOUBLE) | per second (DOUBLE) | per second (DECIMAL) |
|---|---|---|---|---|
| DuckDB (in process) | **0.155-0.157 ms** | 0.201-0.209 ms | **6,242-6,305** | **6,010-6,087** |
| MariaDB | 0.227-0.241 ms | 0.424-0.587 ms | 3,945-4,156 | 3,909-4,133 |
| CedarDB | 0.360-0.373 ms | 0.513-0.527 ms | 2,594-2,675 | 2,503-2,513 |
| ASQL | 3.23-3.86 ms | 5.12-5.19 ms | 281-286 | 284-290 |

**Why ASQL is 9-22x slower here** (9x CedarDB, 14x MariaDB, 22x DuckDB). A stack sample of the server during these updates (`sample`, 3 s) has almost all
of each statement's time in `redo_wait_durable` -> `redo_drain_locked` -> `MacHost::append_file` -> `fcntl`: the
redo log write ends in `F_FULLFSYNC` (`src/hal/mac_host.cpp:74`), which on macOS waits for the SSD itself. About
3 ms of each commit is that. MariaDB and CedarDB sync too, but to Docker's virtual disk, which is not the same wait.

Two things in ASQL make it worse than it needs to be on a Mac:
- The redo log is written through `write_at_unsynced` (`src/userland/sql_database.cpp`, `redo_drain_locked`). The Mac
  host has no unsynced write, so the default falls back to `append_file` (`include/aios/hal/host.hpp:103`, `:131`),
  and that syncs. The write meant to be unsynced is a full `F_FULLFSYNC` of its own.
- For the same reason `AIOS_REDO_SYNC=0` (answer once the record is in the file; sync every 200 ms) changes nothing
  on a Mac: measured with it set, 3.07 ms median and 310 updates/s.

Linux has the unsynced write (`src/hal/linux_host.cpp:325`), so this is a macOS figure; ASQL on Linux was not
measured in this run.

---

## Test 5: The ERP's own data, ASQL against DuckDB

### A. Three reports, with joins

The three reports of the tenth pass (2026-11-04), on summitholding: its `stocktrans` view (the 8-way join over
`stockmoves`, 12,461,263 rows, and its parents) behind each.

- **thisoverlast**: sales, cost and units per month for each year since 2024: 36 `SUM(CASE WHEN tranmnth = m ...)`,
  filtered by `categoryid IN (SELECT ..)` and `loccode IN (SELECT ..)`. 3 rows.
- **nopurchase**: customers with no purchase in 60 days but one in 365, with four correlated subqueries each. 100 rows.
- **brandbylocation**: quantity and total per brand, `GROUP BY COALESCE(BRANDNAME, 'UNBRANDED')`. 103 rows.

ASQL: a server on a copy of the local 3307 root (this run's build, port 3398, 8 lanes). DuckDB: a copy of the same
tables read from that server with DuckDB's `mysql_scanner`, and the same view. Statements in `tools/asql-bench/erp/`.
One warm-up run, then 5 timed runs; three passes.

| Report | ASQL | DuckDB | ASQL faster by |
|---|---|---|---|
| thisoverlast | **392-393 ms** | 493-522 ms | 1.3x |
| nopurchase | **725-759 ms** | 1,412-1,548 ms | 1.9-2.1x |
| brandbylocation | **826-920 ms** | 1,331-1,475 ms | 1.4-1.8x |

Single runs outside these ranges: ASQL's `brandbylocation` reached 1.21-1.29 s in some passes, and 1.85 s in the
first pass after the server started (left out: that pass was before the copy below).

**The answers, compared for the first time.** The tenth pass did not compare ASQL's answers with DuckDB's. This run
did, and at first they differed: DuckDB's `stocktrans` had 9,998,831 rows and ASQL's 9,999,676. The cause is how the
two compare text. 1,426 `stockmoves.stockid` values end in a space (`'GKCLGTC208015 '`); MariaDB's collation ignores
trailing spaces, so they join `stockmaster`, and ASQL does the same. DuckDB compares text exactly and drops them.
With those keys trimmed in DuckDB's copy (`tools/asql-bench/erp/trim.sql`), its view has 9,999,676 rows too, and:

- **thisoverlast** and **brandbylocation**: identical, every cell.
- **nopurchase** ends in `LIMIT 100` with no `ORDER BY`, so each engine returns a different 100 of the customers
  that qualify. Run without the `LIMIT`, ASQL gives 12,845 customers and DuckDB 12,862, and every one of ASQL's
  matches DuckDB's row cell for cell. DuckDB's 17 extra customers each last bought exactly 365 days before the run.
  The report compares `trandate >= DATE_SUB(NOW(), INTERVAL 365 DAY)`, which in MariaDB compares the date as
  midnight against a time of day and excludes them. ASQL gives that; DuckDB's version of the statement uses
  `current_date` and includes them.

The DuckDB timings above are on the trimmed copy, where both engines produce the same answer. MariaDB and CedarDB
were not run on the ERP data in this pass.

### B. Plain scans of `stockmoves` (DOUBLE columns)

The reports above are joins and pivots. Three statements with no join over the same 12,461,263-row `stockmoves`,
whose `qty`, `price` and `standardcost` are DOUBLE:

```sql
SELECT COUNT(*), SUM(qty), SUM(price*qty), AVG(price), MIN(trandate), MAX(trandate) FROM stockmoves
SELECT loccode, COUNT(*), SUM(qty) FROM stockmoves GROUP BY loccode ORDER BY loccode
SELECT COUNT(*), SUM(price*qty) FROM stockmoves WHERE trandate BETWEEN '2025-01-01' AND '2025-06-30' AND qty < 0
```

ASQL: the local server on 3307, `SQL_NO_CACHE`, median of 5 after a warm-up. DuckDB: the tenth pass's copy of the
same table, 6 runs (its timer reads to the millisecond).

| Statement | ASQL | DuckDB | DuckDB faster by |
|---|---|---|---|
| Full aggregate | 224 ms (223-226) | 13-15 ms | 15-17x |
| Grouped by `loccode` | 86 ms (86-89) | 15-18 ms | 4.8-5.7x |
| Filtered | 8.3 ms (7.7-8.9) | 2-3 ms | 2.8-4.2x |

Both counted 12,461,263 rows and their `SUM(qty)` agree to 14 digits; `SUM(price*qty)` agrees to 11 (the order the
DOUBLEs are added in; ASQL adds them in row order, as MariaDB does).

**So ASQL's lead on the ERP is in the reports with joins and pivots, not in scanning a DOUBLE column, where DuckDB is
well ahead.**

---

## Why ASQL is slower on DOUBLE columns

The first version of this comparison used a DOUBLE amount and showed ASQL 2-4x behind DuckDB on the scans, where the
tenth pass's reports have it ahead. Tracing one query (`SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount),
MAX(amount) FROM bench_sales`, 500,000 rows) found why:

- A `sample` of the server while it ran that query over and over put all 2,492 samples of the scan kernels
  (`sum_kernel`, `typed_values`, `decode_codes`) on **one thread**: the connection's own. The 11 pool workers were
  parked, and `sum_kernel` is called straight from `vx_aggregate` with no pool frame between.
- The SUM and AVG of a DOUBLE column take the engine's serial pass (`src/userland/sql_database.cpp:39993`, "one group,
  in row order (a DOUBLE's sum: the serial pass)"). That is on purpose: a floating-point sum is not associative, so
  adding in another order gives another last digit (`include/aios/userland/sql_database.hpp:774`: "Stealing is worth
  losing to make an aggregate reproducible"), and this order is MariaDB's, so ASQL's totals match the ERP's own to the
  bit. DuckDB's `SUM(price*qty)` over `stockmoves` differs from ASQL's at the 11th digit (Test 5B).
- The same query on a DECIMAL(12,2) amount takes **0.68 ms instead of 2.63-2.67** (3.9x), and then is 1.25x off
  DuckDB's 0.54-0.55. I did not profile the DECIMAL run; that it is parallel is an inference from the time.
- Making a DOUBLE sum parallel in a fixed lane order would be reproducible from run to run, but would give a different
  last digit from MariaDB's. That is a decision about the ERP's totals, so it is not made here.

One thing tried and thrown away: having the waiting thread take the lanes one at a time, in case the workers were
never getting any (`wait_parallel`). It changed nothing (2.91 ms), because this path never reaches the pool at all.

---

## Storage

The 500,000-row table with a DOUBLE amount, as each engine stores it (measured in the first pass of this run; the
DECIMAL table was not measured). These are not the same measure, so read them with that in mind:

| Engine | Size | What it is |
|---|---|---|
| ASQL | **3.1 MB** | its table file on disk (`sql_db/tables/<table>.g4.tbl`) after `FLUSH TABLES`. In memory `SHOW TABLE STATUS` reports 4.7 MB of data and 6.5 MB of indexes. |
| DuckDB | 8.0 MB | a database file holding only this table, after `COPY` and `CHECKPOINT` |
| MariaDB | 37.7 MB | `bench_sales.ibd`; `data_length` 27.9 MB after `ANALYZE TABLE` |
| CedarDB | 39.7 MB | `pg_total_relation_size('bench_sales')` (its data directory holds 1.6 GB, most of it preallocated) |

The CSV itself is 21.0 MB. ASQL, MariaDB and CedarDB were measured after Tests 1-4 (the 2,000 updates included);
DuckDB on a fresh file.

---

## Reproducing it

`tools/asql-bench/README.md` has the commands: the data generator, the container and server lines, the PHP harness
for the three wire engines (`bench.php`, `AMOUNT=decimal|double`), the C harness for DuckDB (`duckbench.c`), the
answer check (`cmp.php`), and the ERP reports (`erp.php`, `duckerp.c`, `erpcmp.php`, `erp/`).
