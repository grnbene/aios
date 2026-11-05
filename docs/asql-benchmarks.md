# ASQL against MariaDB, CedarDB and DuckDB: 2026-11-05

**Engines, run on the same machine one after another**:
- **ASQL** `5.7.44-aios-asql`, commit `1874ce2` plus the changes committed with this file (`aios` md5 `bba27169`),
  8 lanes, native on macOS. Started with `AIOS_SQL_SECURE_FILE_PRIV` set to the data folder, which `LOAD DATA INFILE`
  needs. Default commit durability (`F_FULLFSYNC`; see Test 4).
- **MariaDB** `10.11.9-MariaDB-ubu2204` (InnoDB), Docker image `mariadb:10.11.9`, `innodb_buffer_pool_size=1G`,
  otherwise defaults (`innodb_flush_log_at_trx_commit=1`, query cache off).
- **CedarDB** `v2026-11-04` ("PostgreSQL 16.3 compatible", arm64), Docker image `cedardb/cedardb:latest`, defaults.
- **DuckDB** `v1.5.5 (Variegata) d8cdaa33fd`, in process through its C API, on a database file.

Tests 1-4 were measured in this run. Test 5A (the ERP reports) and DuckDB's side of Test 5B are from the 2026-11-04
run (ASQL `1874ce2`), not repeated: the DuckDB copy of summitholding they need was not on hand. The harness is
`tools/asql-bench/` (its README has the commands); the 2026-11-04 figures are in this file's history
(`git show 1874ce2:docs/asql-benchmarks.md`).

---

## Contents

- [Machine and method](#machine-and-method)
- [Summary](#summary)
- [Test 1: Loading 500,000 rows](#test-1-loading-500000-rows)
- [Test 2: Analytical queries](#test-2-analytical-queries)
- [Test 3: Point lookups](#test-3-point-lookups)
- [Test 4: Single-row updates](#test-4-single-row-updates)
- [Test 5: The ERP's own data, ASQL against DuckDB](#test-5-the-erps-own-data-asql-against-duckdb)
- [What changed since 2026-11-04](#what-changed-since-2026-11-04)
- [Storage](#storage)
- [Reproducing it](#reproducing-it)

---

## Machine and method

- **Host**: Apple M3 Pro, 12 cores (6 performance, 6 efficiency), 18 GB, macOS 26.2. ASQL and DuckDB ran natively.
  MariaDB and CedarDB ran in Docker Desktop's Linux VM (12 CPUs, 8 GB), reached over a loopback port. **That is not an
  even field**: the two containers' disk syncs go to the VM's virtual disk, ASQL's to the Mac's SSD (Test 4).
- **Other load**: the machine was in use by other work; the 1-minute load average was about 7 during these runs. The
  other engines' ranges are wider than on 2026-11-04 (CedarDB's and DuckDB's most of all); ASQL's held steady. Each
  test ran twice; the ranges below span the two passes.
- **Data**: `bench_sales`, 500,000 rows from a fixed seed (`tools/asql-bench/gen.py`), 21,034,548 bytes of CSV:
  `id INT PRIMARY KEY, customer_id INT (1-50,000), category_id INT (1-50), amount (1.00-1000.00),
  trans_date DATE (2024-01-01 + 0-729 days), status VARCHAR(16) (5 values)`.
- **The amount column, both ways**: every test ran with `amount DOUBLE` and again with `amount DECIMAL(12,2)`
  (`NUMERIC(12,2)` in CedarDB), each a fresh table on every engine. The ERP uses both: in summitholding
  `stockmoves.price`, `qty`, `debtortrans.ovamount` and `salesorderdetails.unitprice` are DOUBLE, and 625 of its
  columns are DOUBLE or FLOAT against 472 DECIMAL.
- **Timing**: per statement, around the client call that sends it and reads its whole answer: PHP `mysqli`
  (ASQL, MariaDB) or `pgsql` (CedarDB) over one persistent connection; for DuckDB around `duckdb_query`, in process
  (no network and no protocol, which favours it in Tests 3 and 4).
- **Queries**: one warm-up run, then 5 timed runs; the figure is the median. ASQL keeps answers in a result cache,
  so its queries were timed with `SQL_NO_CACHE`: computed every time, as the others were.
- **Answers**: every query's rows from every engine were compared with MariaDB's (numbers within 1e-9 relative). All
  five queries matched on all four engines, for both amount types and both passes. ASQL's DOUBLE sums are added in
  row order, as MariaDB adds them, and agree with MariaDB's to the last bit.

---

## Summary

Milliseconds unless marked; lower is better except QPS and TPS. Each cell spans the two passes. Best of the four in
bold (several in bold where their ranges overlap).

**With `amount DOUBLE`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | **256-272** | 637-898 | 446-583 | **233-267** |
| 100 `INSERT`s of 5,000 rows | **1,180-1,213** | 1,364-1,410 | 4,264-5,349 | 6,866-7,689 |
| Full-table aggregate | **0.61-0.62** | 46-49 | 4.72-11.2 | 1.37-2.73 |
| `GROUP BY` + top 10 | **0.82-0.85** | 54-55 | 9.53-31 | **0.84-1.69** |
| Filtered range aggregate | **0.53-0.56** | 38 | 1.06-4.64 | 0.92-1.34 |
| Pivot, 3 `SUM(CASE ...)` | **1.98-2.42** | 92-94 | 15-27 | 3.11-4.82 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **8.67-9.21** | 252-279 | 128-201 | 11-18 |
| Point lookup, median | **0.035-0.038** | 0.183-0.203 | 0.402-0.409 | 0.103-0.116 |
| Point lookups, one client | **24,525-26,619 QPS** | 4,500-4,540 | 2,172-2,295 | 6,254-9,414 |
| Single-row update, median | 3.84-3.90 | 0.217-0.245 | 0.406-0.434 | **0.157-0.177** |
| Single-row updates, one client | 275-287 TPS | 3,550-4,132 | 1,638-2,141 | **5,088-6,207** |

**With `amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 328-350 | 608-809 | 299-401 | **226-232** |
| 100 `INSERT`s of 5,000 rows | **1,245-1,268** | **1,255-1,372** | 3,953-4,243 | 6,624-7,047 |
| Full-table aggregate | 0.66-0.72 | 77-79 | 3.23-3.70 | **0.53-0.57** |
| `GROUP BY` + top 10 | **0.78-0.89** | 76-77 | 8.88-9.43 | **0.74-0.79** |
| Filtered range aggregate | **0.64** | 42 | 1.01-1.23 | **0.63-0.64** |
| Pivot, 3 `SUM(CASE ...)` | **2.47-2.58** | 154-156 | 14-15 | 2.91-2.92 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **9.61-9.66** | 204-242 | 122-128 | 11 |
| Point lookup, median | **0.035-0.037** | 0.205-0.241 | 0.366-0.371 | 0.099-0.103 |
| Point lookups, one client | **25,640-26,104 QPS** | 4,033-4,684 | 2,584-2,646 | 9,501-9,853 |
| Single-row update, median | 3.09-3.67 | 0.223-0.233 | 0.370-0.394 | **0.162-0.164** |
| Single-row updates, one client | 277-297 TPS | 3,770-4,058 | 2,482-2,550 | **5,995-6,030** |

**On the ERP's own data** (Test 5, 12.5M rows of summitholding):

| | ASQL | DuckDB |
|---|---|---|
| Reports with joins: `thisoverlast`, `nopurchase`, `brandbylocation` (2026-11-04) | **392-393, 725-759, 826-920 ms** | 493-522, 1,412-1,548, 1,331-1,475 ms |
| Plain scans of `stockmoves`' DOUBLE columns: full, grouped, filtered | 215-221, **26.5-27.6**, 9.5 ms | **13-15**, 15-18, **2-3 ms** (2026-11-04) |

In short:
- **With a DOUBLE amount** ASQL is the fastest of the four, or level with the fastest, on every test but single-row
  updates: loading a file is level with DuckDB, the wire `INSERT`s are the fastest, and every analytical query and
  point lookup is ahead.
- **With a DECIMAL amount** two gaps are left: loading a file (1.4-1.5x DuckDB's time; a new table's DECIMAL columns
  still start as dictionaries of their text, where INT and DOUBLE columns now start typed) and the full-table
  aggregate (1.2-1.3x DuckDB's). The `GROUP BY` and the range query are level, the pivot, window and lookups ahead.
- **Single-row updates** are the Mac's disk flush: 3 ms of each commit is `F_FULLFSYNC`, where the CPU work is
  0.04 ms (Test 4). With `AIOS_REDO_SYNC=barrier` an update takes 0.185 ms (4,190 a second), ahead of MariaDB and
  CedarDB; that setting is not the default.
- **On the ERP**: the reports with joins are 1.3-2.1x faster than DuckDB (2026-11-04). The grouped scan of
  `stockmoves` went from 86 ms to 27 ms; the full aggregate, whose `SUM(price*qty)` is a sum of an expression, still
  takes the serial path and is about 15x DuckDB's time.

---

## Test 1: Loading 500,000 rows

### A. From the CSV file, by the engine's own loader

| Engine | Statement | DOUBLE, ms | DECIMAL, ms |
|---|---|---|---|
| DuckDB | `COPY bench_load FROM '.../bench_sales.csv' (FORMAT csv, HEADER false)` | **233-267** | **226-232** |
| ASQL | `LOAD DATA INFILE '.../bench_sales.csv' INTO TABLE bench_load FIELDS TERMINATED BY ','` | **256-272** | 328-350 |
| CedarDB | `COPY bench_load FROM '/data/bench_sales.csv' WITH (FORMAT csv)` | 446-583 | 299-401 |
| MariaDB | `LOAD DATA INFILE '/data/bench_sales.csv' INTO TABLE bench_load FIELDS TERMINATED BY ','` | 637-898 | 608-809 |

On 2026-11-04 ASQL took 1,469-1,478 ms here. What made up the difference (all in one atomic statement, as before,
with its undo log, row locks and redo record):
- **Typed columns from the first row.** A new table's INT columns start bit-packed (frame of reference) and its
  DOUBLE columns as plain doubles. They were dictionaries of the values' text until a checkpoint typed them: each
  number formatted, interned and given text statistics on the way in. An INT column is still laid out the way
  `optimize_encoding` chooses at the same insert thresholds and checkpoint as before, so query plans see the same
  encodings.
- **The file read a batch ahead** on a thread of its own, with the batch's logged `INSERT` text, while the insert runs.
- **The rows' values converted in parallel** before the in-order key checks, and the rows appended column by column
  with the columns side by side.
- **The locks on the inserted rows** held in a flat set per transaction and table instead of a map entry each, taken
  beside the column appends and given back at once at the commit (the commit was 65-90 ms of the load).

`kill -9` right after a load still loses nothing. DECIMAL columns are not yet started typed, which is the gap left.

### B. Over the wire: 100 `INSERT INTO bench_sales VALUES (...)` statements of 5,000 rows

| Engine | DOUBLE, ms | DECIMAL, ms |
|---|---|---|
| ASQL | **1,180-1,213** | **1,245-1,268** |
| MariaDB | 1,364-1,410 | **1,255-1,372** |
| CedarDB | 4,264-5,349 | 3,953-4,243 |
| DuckDB (in process) | 6,866-7,689 | 6,624-7,047 |

One connection, autocommit: each statement is one transaction (and so one `F_FULLFSYNC` on ASQL). On 2026-11-04 ASQL
took 1,853-1,943 ms. Besides the load's changes, a literal in a `VALUES` list is read without the twelve levels of the
expression grammar above it.

---

## Test 2: Analytical queries

The same SQL text on all four engines; medians of 5 runs after a warm-up, in ms. Each query, DOUBLE then DECIMAL.

### Full-table aggregate
```sql
SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM bench_sales
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | **0.61-0.62** | 46-49 | 4.72-11.2 | 1.37-2.73 |
| DECIMAL | 0.66-0.72 | 77-79 | 3.23-3.70 | **0.53-0.57** |

### `GROUP BY` and top 10
```sql
SELECT category_id, COUNT(*) AS n, SUM(amount) AS total FROM bench_sales
GROUP BY category_id ORDER BY total DESC LIMIT 10
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | **0.82-0.85** | 54-55 | 9.53-31 | **0.84-1.69** |
| DECIMAL | **0.78-0.89** | 76-77 | 8.88-9.43 | **0.74-0.79** |

### Filtered range aggregate
```sql
SELECT COUNT(*), SUM(amount) FROM bench_sales
WHERE trans_date BETWEEN '2024-03-01' AND '2024-08-31' AND amount > 100
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | **0.53-0.56** | 38 | 1.06-4.64 | 0.92-1.34 |
| DECIMAL | **0.64** | 42 | 1.01-1.23 | **0.63-0.64** |

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
| DOUBLE | **1.98-2.42** | 92-94 | 15-27 | 3.11-4.82 |
| DECIMAL | **2.47-2.58** | 154-156 | 14-15 | 2.91-2.92 |

### Window function
```sql
SELECT COUNT(*), SUM(amount) FROM (
  SELECT amount, ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY trans_date DESC, id DESC) AS rn
  FROM bench_sales) t WHERE rn = 1
```
| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| DOUBLE | **8.67-9.21** | 252-279 | 128-201 | 11-18 |
| DECIMAL | **9.61-9.66** | 204-242 | 122-128 | 11 |

### What ASQL's result cache answers

The same statements sent again without `SQL_NO_CACHE`, after two runs to be admitted, took **0.03-0.05 ms** for all five
queries with either amount type. The other engines have no result cache; every comparison above is a computed answer.

---

## Test 3: Point lookups

10,000 `SELECT id, customer_id, amount, status FROM bench_sales WHERE id = N`, random ids, one after another on one
connection (DuckDB: in process).

| Engine | median (DOUBLE) | p99 (DOUBLE) | per second (DOUBLE) | per second (DECIMAL) |
|---|---|---|---|---|
| ASQL | **0.035-0.038 ms** | 0.092-0.093 ms | **24,525-26,619** | **25,640-26,104** |
| DuckDB (in process) | 0.103-0.116 ms | 0.160-0.771 ms | 6,254-9,414 | 9,501-9,853 |
| MariaDB | 0.183-0.203 ms | 0.815-1.061 ms | 4,500-4,540 | 4,033-4,684 |
| CedarDB | 0.402-0.409 ms | 1.009-1.483 ms | 2,172-2,295 | 2,584-2,646 |

Almost every id is a different one, so ASQL's result cache answers next to none of these. The MariaDB and CedarDB
figures include the hop into Docker's VM.

---

## Test 4: Single-row updates

2,000 `UPDATE bench_sales SET amount = amount + 0.01 WHERE id = N`, random ids, autocommit, one connection.

| Engine | median (DOUBLE) | p99 (DOUBLE) | per second (DOUBLE) | per second (DECIMAL) |
|---|---|---|---|---|
| DuckDB (in process) | **0.157-0.177 ms** | 0.222-0.420 ms | **5,088-6,207** | **5,995-6,030** |
| MariaDB | 0.217-0.245 ms | 0.816-1.087 ms | 3,550-4,132 | 3,770-4,058 |
| CedarDB | 0.406-0.434 ms | 1.318-2.164 ms | 1,638-2,141 | 2,482-2,550 |
| ASQL | 3.84-3.90 ms | 5.01-5.35 ms | 275-287 | 277-297 |
| ASQL, `AIOS_REDO_SYNC=barrier` | 0.185 ms | 0.573 ms | 4,190 | not run |

**The gap is the Mac's disk flush.** A stack sample of the server during these updates has 88% of each statement in
one call: the redo log's `F_FULLFSYNC` (`MacHost::sync_file`), which waits for the SSD itself, about 2.7 ms on this
machine. With the flush taken out (`AIOS_REDO_SYNC=0`) an update takes 0.041 ms, 21,000 a second. MariaDB and CedarDB
sync too, but to Docker's virtual disk, which is not the same wait; DuckDB's 0.16 ms has no per-statement flush in it.

Two things changed here since 2026-11-04:
- The Mac host now has the unsynced redo write and `sync_file` the log expects (`src/hal/mac_host.cpp`). It had
  neither, so every drain of the log was a full flush of its own, group commit synced once a statement, and
  `AIOS_REDO_SYNC=0` did nothing on a Mac.
- `AIOS_REDO_SYNC=barrier` (opt-in) commits with `F_BARRIERFSYNC`: the commit is ordered on the medium rather than
  flushed through the drive's cache, 0.29 ms where `F_FULLFSYNC` is 2.7. A crash of the process loses nothing (a
  `kill -9` after 1,000 such updates lost none); a power cut can lose the newest commits, never keep a later one
  without an earlier one. The default stays the full flush.

Linux has the unsynced write and an `fdatasync` commit, so this is a macOS figure; ASQL on Linux was not measured.

---

## Test 5: The ERP's own data, ASQL against DuckDB

### A. Three reports, with joins (2026-11-04, ASQL `1874ce2`; not repeated)

The three reports of the tenth pass, on summitholding: its `stocktrans` view (the 8-way join over `stockmoves`,
12,461,263 rows, and its parents) behind each.

- **thisoverlast**: sales, cost and units per month for each year since 2024: 36 `SUM(CASE WHEN tranmnth = m ...)`,
  filtered by `categoryid IN (SELECT ..)` and `loccode IN (SELECT ..)`. 3 rows.
- **nopurchase**: customers with no purchase in 60 days but one in 365, with four correlated subqueries each. 100 rows.
- **brandbylocation**: quantity and total per brand, `GROUP BY COALESCE(BRANDNAME, 'UNBRANDED')`. 103 rows.

ASQL: a server on a copy of the local 3307 root (port 3398, 8 lanes). DuckDB: a copy of the same tables read from
that server with DuckDB's `mysql_scanner`, and the same view. Statements in `tools/asql-bench/erp/`. One warm-up run,
then 5 timed runs; three passes.

| Report | ASQL | DuckDB | ASQL faster by |
|---|---|---|---|
| thisoverlast | **392-393 ms** | 493-522 ms | 1.3x |
| nopurchase | **725-759 ms** | 1,412-1,548 ms | 1.9-2.1x |
| brandbylocation | **826-920 ms** | 1,331-1,475 ms | 1.4-1.8x |

The answers, compared: DuckDB's `stocktrans` first had 9,998,831 rows and ASQL's 9,999,676, because 1,426
`stockmoves.stockid` values end in a space and MariaDB's collation (and ASQL) ignores trailing spaces where DuckDB
compares exactly. With those keys trimmed in DuckDB's copy (`tools/asql-bench/erp/trim.sql`) its view has 9,999,676
rows too; `thisoverlast` and `brandbylocation` are identical cell for cell, and `nopurchase` without its `LIMIT 100`
gives ASQL 12,845 customers, every one matching DuckDB's (DuckDB's 17 more each bought exactly 365 days before the run:
the report's `DATE_SUB(NOW(), INTERVAL 365 DAY)` compares a date as midnight in MariaDB, which ASQL follows).

### B. Plain scans of `stockmoves` (DOUBLE columns)

```sql
SELECT COUNT(*), SUM(qty), SUM(price*qty), AVG(price), MIN(trandate), MAX(trandate) FROM stockmoves
SELECT loccode, COUNT(*), SUM(qty) FROM stockmoves GROUP BY loccode ORDER BY loccode
SELECT COUNT(*), SUM(price*qty) FROM stockmoves WHERE trandate BETWEEN '2025-01-01' AND '2025-06-30' AND qty < 0
```

ASQL: this run's build on a copy of the local 3307 root (port 3398), `SQL_NO_CACHE`, median of 5 after a warm-up, two
passes. DuckDB: the 2026-11-04 copy of the same table, 6 runs (its timer reads to the millisecond).

| Statement | ASQL 2026-11-05 | ASQL 2026-11-04 | DuckDB (2026-11-04) |
|---|---|---|---|
| Full aggregate | 215-221 ms | 224 ms | **13-15 ms** |
| Grouped by `loccode` | 26.5-27.6 ms | 86 ms | **15-18 ms** |
| Filtered | 9.5 ms | 8.3 ms | **2-3 ms** |

Both counted 12,461,263 rows; ASQL's answers are the same as on 2026-11-04. The grouped scan is now in parallel (see
below). The full aggregate is not: `SUM(price*qty)` is a sum of an expression, which still goes through the serial
pass, and it is most of that query's time. That is the largest gap left.

---

## What changed since 2026-11-04

**DOUBLE sums, in parallel and in row order.** A floating-point total depends on the order of its additions, and
ASQL adds a DOUBLE column in row order, as MariaDB does, so that its totals are MariaDB's to the last bit. That order
was kept by adding on one thread, with a decode and a generic call per row: the 2026-11-04 "Why ASQL is slower on
DOUBLE columns". Now:
- **One total** (`SUM`/`AVG` of a column): the column is decoded in parallel, and added once, in one tight loop in
  row order (`SUM` and `AVG` of one column share it).
- **By group**: the group of each row is worked out in parallel. Sums with nothing to test per row are then added in
  one tight pass, a group's total kept in a cache-resident array; masked sums (a pivot's `CASE`) are dealt out, in row
  order, to the blocks of groups each thread owns, and each block adds its own rows in order. Either way a group's
  total is the same additions in the same order.
- **A derived table of typed columns** (the window's `rn = 1` rows) is read by the vectorized path instead of a row at
  a time.
- Answers checked against an unchanged build on 19 queries over a table with NULLs, `CASE` masks, filters and derived
  tables, fresh and after a restart: identical, except one sum over a derived `a * 2` column, where the new answer is
  MariaDB's to the last digit and the old one was not.

**Inserts and loads**: see Test 1. Answers, errors, warnings, rollbacks, `LAST_INSERT_ID()` and row locks were checked
against an unchanged build (duplicate keys within and across statements, past the 100,000-row big-table line,
`INSERT IGNORE`, `LOAD DATA` with short and long lines, `\N`, `IGNORE 1 LINES` and a column list, two sessions on rows
an open transaction inserted): identical.

**Found and not changed**: ASQL sums a FLOAT column as exact decimals where MariaDB adds the stored single-precision
values (the last digits differ), and `COUNT(DISTINCT ROUND(a, 1))` over a derived table counts 9,491 where MariaDB
counts 9,494.

---

## Storage

The 500,000-row table with a DOUBLE amount, as each engine stores it, measured on 2026-11-04 and not again in this
run. These are not the same measure, so read them with that in mind:

| Engine | Size | What it is |
|---|---|---|
| ASQL | **3.1 MB** | its table file on disk (`sql_db/tables/<table>.g4.tbl`) after `FLUSH TABLES`. In memory `SHOW TABLE STATUS` reports 4.7 MB of data and 6.5 MB of indexes. |
| DuckDB | 8.0 MB | a database file holding only this table, after `COPY` and `CHECKPOINT` |
| MariaDB | 37.7 MB | `bench_sales.ibd`; `data_length` 27.9 MB after `ANALYZE TABLE` |
| CedarDB | 39.7 MB | `pg_total_relation_size('bench_sales')` (its data directory holds 1.6 GB, most of it preallocated) |

---

## Reproducing it

`tools/asql-bench/README.md` has the commands: the data generator, the container and server lines, the PHP harness
for the three wire engines (`bench.php`, `AMOUNT=decimal|double`), the C harness for DuckDB (`duckbench.c`), the
answer check (`cmp.php`), and the ERP reports (`erp.php`, `duckerp.c`, `erpcmp.php`, `erp/`).
