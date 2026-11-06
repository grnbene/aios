# ASQL against MariaDB, CedarDB and DuckDB

500,000 rows, one machine, each engine run alone, two passes. Lower is better except QPS and TPS; each cell spans the
two passes; best in bold (several where ranges overlap).

## Setup

| | |
|---|---|
| Host | Intel i5-7600K (4 cores, 3.8 GHz), 19 GB, Linux 6.17, WD Blue SN570 NVMe, ext4 |
| ASQL | `5.7.44-aios-asql`, commit `c3cb66c`, 4 lanes, default durability (one `fdatasync` per commit) |
| MariaDB | 10.11.9 (Docker), InnoDB, `innodb_buffer_pool_size=1G`, `innodb_flush_log_at_trx_commit=1` |
| CedarDB | v2026-11-04 (Docker), defaults |
| DuckDB | v1.5.5, in process (C API), database file |

- All engines' data on the same NVMe (a flush costs about 0.5 ms there); containers use bind mounts.
- The table is about 21 MB and fits in every engine's cache; no engine was disk-bound for the queries.
- The machine is shared with other work: load average about 2 on 4 cores during the run.
- ASQL was run on 2026-11-06; the other three engines' figures are from the 2026-11-05 run on the same machine and
  setup (their software did not change).
- Data: `bench_sales(id INT PRIMARY KEY, customer_id INT, category_id INT, amount, trans_date DATE, status VARCHAR(16))`,
  500,000 rows from a fixed seed (`tools/asql-bench/gen.py`), run with `amount DOUBLE` and with `amount DECIMAL(12,2)`
  (`NUMERIC(12,2)` in CedarDB), each a fresh table.
- Timing: per statement at the client (PHP `mysqli`/`pgsql`, one persistent loopback connection; DuckDB around
  `duckdb_query`, in process, so no network). Queries: 1 warm-up, median of 5; ASQL timed with `SQL_NO_CACHE`.
- Every query's rows from every engine were compared with MariaDB's (within 1e-9 relative): all matched.
- Memory and CPU are the engine's own process, read from `/proc/<pid>` (a container's main process): resident memory
  (`VmRSS`; peak is `VmHWM`) and CPU time (user + system, all threads) summed over a phase. Each engine is started
  fresh for each amount type, so peaks are per run. The PHP client's CPU is not counted. Query CPU is measured over
  at least 1 s of repeated runs, as `/proc` counts CPU in 10 ms ticks; "cores" is CPU time divided by wall time.

## Results

Milliseconds unless marked.

**`amount DOUBLE`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | **160-163** | 1,120-1,136 | 274-339 | 379-402 |
| 100 `INSERT`s of 5,000 rows | **1,339-1,353** | 2,098-2,132 | 5,091-5,164 | 12,480-12,544 |
| Full-table aggregate | **0.717-0.796** | 90.8-91.1 | 1.49-1.51 | 2.29-2.33 |
| `GROUP BY` + top 10 | **1.43-1.47** | 115-116 | 2.68-2.75 | **1.36-2.09** |
| Filtered range aggregate | **0.873-1.20** | 76.4-76.8 | **1.10-1.53** | 2.60-2.98 |
| Pivot, 3 `SUM(CASE ...)` | **2.92-4.48** | 178-179 | 4.96-5.19 | 6.25-7.15 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **14.4-15.3** | 408-417 | 59.0-61.1 | 36.8-47.3 |
| Point lookup, median | **0.050-0.053** | 0.090-0.095 | 0.440-0.444 | 0.282-0.283 |
| Point lookups, one client | **15,470-18,451 QPS** | 9,672-10,624 QPS | 2,177-2,230 QPS | 3,334-3,380 QPS |
| Single-row update, median | **0.381-0.427** | 0.568-0.584 | 0.507-0.523 | 1.01-1.07 |
| Single-row updates, one client | **2,018-2,176 TPS** | 1,623-1,679 TPS | 1,825-1,935 TPS | 907-941 TPS |

**`amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | **139-151** | 1,135-1,165 | 268-331 | 390-434 |
| 100 `INSERT`s of 5,000 rows | **1,341-1,352** | 2,066-2,123 | 5,189-5,302 | 12,709-12,723 |
| Full-table aggregate | **0.528-0.765** | 157 | 1.72-1.75 | 1.61-1.64 |
| `GROUP BY` + top 10 | **0.845-1.11** | 171-175 | 2.66-2.71 | 1.88-2.10 |
| Filtered range aggregate | **0.665-0.826** | 86.2-86.6 | 1.10-1.18 | 1.56-1.59 |
| Pivot, 3 `SUM(CASE ...)` | **2.26-3.43** | 334-346 | 4.20-4.24 | 6.89-7.18 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **16.7-17.9** | 441-449 | 50.9-62.9 | 28.1-32.7 |
| Point lookup, median | **0.049-0.050** | 0.089-0.090 | 0.453-0.461 | 0.280-0.283 |
| Point lookups, one client | **18,065-18,222 QPS** | 10,293-10,872 QPS | 2,018-2,139 QPS | 3,414-3,429 QPS |
| Single-row update, median | **0.358-0.375** | 0.545-0.567 | 0.518-0.529 | 1.04-1.05 |
| Single-row updates, one client | **2,148-2,230 TPS** | 1,685-1,737 TPS | 1,754-1,841 TPS | 895-919 TPS |

ASQL is fastest on every test with both types except two, where its range overlaps another engine's:

- DOUBLE `GROUP BY` + top 10: ASQL took 1.43-1.47 ms, DuckDB 1.36 ms in one pass and 2.09 ms in the other.
- DOUBLE range query: ASQL took 0.87-1.20 ms, CedarDB 1.10-1.53 ms.

How far ahead, against the next fastest engine:

- 1.5x or more: CSV load (1.7-2.4x over CedarDB), wire `INSERT`s (1.5-1.6x over MariaDB), full aggregate (1.9-3.1x),
  DECIMAL `GROUP BY` (1.7-2.5x over DuckDB), window function (1.6-3.3x over DuckDB), point lookups (5.3-5.8x over
  DuckDB).
- Less, or not in every pass: DECIMAL range query (1.3-1.8x over CedarDB), DECIMAL pivot (1.2-1.9x over CedarDB),
  DOUBLE pivot (1.1-1.8x over CedarDB), single-row updates (1.2-1.5x over CedarDB).

## Memory and CPU

Resident memory of the engine's process, in MB (range over the two passes):

| Resident memory (RSS), MB | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Idle, empty database | 36-37 | 116 | 178 | 32 |
| Peak during the run, DOUBLE | 186 | 242-247 | 452-453 | 132-134 |
| Peak during the run, DECIMAL | 181 | 233-234 | 466-470 | 134-136 |
| After the run, DOUBLE | 133 | 242-247 | 452-453 | 78-80 |
| After the run, DECIMAL | 137-138 | 233-234 | 466-470 | 78-80 |

ASQL's peak is the CSV load. After the run it holds about 135 MB: more than DuckDB (78-80), a little over half of
MariaDB, under a third of CedarDB.

Server CPU time per phase (mean of the two passes; "cores" = CPU time / wall time):

| Server CPU, DOUBLE | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 275 ms, 1.7 cores | 1,180 ms, 1.0 cores | 380 ms, 1.2 cores | 593 ms, 1.5 cores |
| 100 `INSERT`s of 5,000 rows | 1,990 ms, 1.5 cores | 2,400 ms, 1.1 cores | 5,990 ms, 1.2 cores | 12,455 ms, 1.0 cores |
| Full-table aggregate, per run | 1.53 ms, 2.0 cores | 93.1 ms, 1.0 cores | 3.61 ms, 2.2 cores | 8.17 ms, 3.2 cores |
| `GROUP BY` + top 10, per run | 3.94 ms, 2.3 cores | 116 ms, 1.0 cores | 5.38 ms, 2.1 cores | 4.51 ms, 2.1 cores |
| Filtered range aggregate, per run | 2.24 ms, 2.0 cores | 76.7 ms, 1.0 cores | 2.30 ms, 1.6 cores | 5.97 ms, 2.1 cores |
| Pivot, per run | 8.39 ms, 2.7 cores | 180 ms, 1.0 cores | 11.7 ms, 2.4 cores | 16.0 ms, 2.3 cores |
| Window function, per run | 25.6 ms, 1.7 cores | 411 ms, 1.0 cores | 171 ms, 2.3 cores | 97.3 ms, 2.7 cores |
| Point lookup, each | 56 µs, 0.9 cores | 56 µs, 0.6 cores | 390 µs, 0.9 cores | 425 µs, 1.4 cores |
| Single-row update, each | 200 µs, 0.4 cores | 125 µs, 0.2 cores | 558 µs, 1.0 cores | 854 µs, 0.8 cores |

| Server CPU, DECIMAL | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 260 ms, 1.8 cores | 1,185 ms, 1.0 cores | 370 ms, 1.2 cores | 675 ms, 1.6 cores |
| 100 `INSERT`s of 5,000 rows | 2,220 ms, 1.6 cores | 2,405 ms, 1.1 cores | 6,290 ms, 1.2 cores | 12,666 ms, 1.0 cores |
| Full-table aggregate, per run | 2.10 ms, 2.9 cores | 156 ms, 1.0 cores | 3.49 ms, 1.8 cores | 3.41 ms, 2.0 cores |
| `GROUP BY` + top 10, per run | 3.16 ms, 3.2 cores | 172 ms, 1.0 cores | 5.45 ms, 1.9 cores | 4.01 ms, 2.0 cores |
| Filtered range aggregate, per run | 2.49 ms, 3.1 cores | 86.2 ms, 1.0 cores | 2.21 ms, 1.7 cores | 3.31 ms, 1.8 cores |
| Pivot, per run | 9.01 ms, 2.7 cores | 342 ms, 1.0 cores | 12.3 ms, 2.7 cores | 15.0 ms, 2.5 cores |
| Window function, per run | 25.9 ms, 1.5 cores | 444 ms, 1.0 cores | 175 ms, 3.1 cores | 102 ms, 3.3 cores |
| Point lookup, each | 54 µs, 1.0 cores | 54 µs, 0.6 cores | 407 µs, 0.8 cores | 426 µs, 1.5 cores |
| Single-row update, each | 195 µs, 0.4 cores | 115 µs, 0.2 cores | 558 µs, 1.0 cores | 816 µs, 0.7 cores |

- ASQL spends the least CPU on the CSV load, the wire `INSERT`s, the full aggregate, `GROUP BY`, the pivot and the
  window function with both types, and on the DOUBLE range query.
- It spends more than another engine in two places: the DECIMAL range query (2.5 ms against CedarDB's 2.2) and
  single-row updates (195-200 µs against MariaDB's 115-125). It is still faster on the clock in both: the range query
  runs on about 3 cores, and an update's median is 0.36-0.43 ms against MariaDB's 0.55-0.58.
- Point lookups cost the same CPU as MariaDB (54-56 µs).

## Test queries

Load: `LOAD DATA INFILE` / `COPY` of the 21 MB CSV; and 100 `INSERT ... VALUES` of 5,000 rows each (autocommit).

```sql
-- full aggregate
SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM bench_sales
-- group by, top 10
SELECT category_id, COUNT(*) AS n, SUM(amount) AS total FROM bench_sales GROUP BY category_id ORDER BY total DESC LIMIT 10
-- range
SELECT COUNT(*), SUM(amount) FROM bench_sales WHERE trans_date BETWEEN '2024-03-01' AND '2024-08-31' AND amount > 100
-- pivot
SELECT category_id,
  SUM(CASE WHEN status = 'shipped'  THEN amount ELSE 0 END) AS s,
  SUM(CASE WHEN status = 'pending'  THEN amount ELSE 0 END) AS p,
  SUM(CASE WHEN status = 'returned' THEN amount ELSE 0 END) AS r
FROM bench_sales GROUP BY category_id ORDER BY category_id
-- window
SELECT COUNT(*), SUM(amount) FROM (
  SELECT amount, ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY trans_date DESC, id DESC) AS rn
  FROM bench_sales) t WHERE rn = 1
-- point lookup (10,000, random ids)
SELECT id, customer_id, amount, status FROM bench_sales WHERE id = N
-- update (2,000, random ids, autocommit)
UPDATE bench_sales SET amount = amount + 0.01 WHERE id = N
```

## ERP reports on real data

Three of the ERP's report pages, run on a snapshot of a production `km` database (`asql-km-20261102`: `stockmoves` 2.8M
rows, `debtortrans` 670k, and the `stocktrans` view that joins nine tables, 1.64M rows). ASQL and DuckDB on the same
box, each alone; DuckDB holds the same tables copied through its `mysql_scanner` and the same view. Median ms of 5
timed runs after one warm-up, two passes, in ms.

| Report | ASQL | DuckDB |
|---|---|---|
| `thisoverlast`: sales, cost and units by month, 36 `SUM(CASE ...)` over the view | **342-395** | **362-405** |
| `nopurchase`: customers with no purchase in 60 days but one in 365, four correlated subqueries each | **127-137** | 609-672 |
| `brandbylocation`: quantity and total per brand over the view | **552-606** | **564-655** |

ASQL commit `43827b0`. ASQL is ahead on all three in both passes. On `nopurchase` it is about 5x faster; on the two
reports over the view the lead is 3-8%, inside the run-to-run spread.

Statements are in `tools/asql-bench/erp/`. The data is private, so this part cannot be reproduced from the repository.

How the answers compare:

- The rows agree. The money columns differ by cents, because ASQL rounds a DOUBLE as the ERP's PHP does (15
  significant digits, then half away from zero; `docs/asql.md`, "Rounding"). Over all 1,642,699 non-NULL lines of the
  view ASQL equals that rule exactly. MariaDB 10.11.9 and DuckDB each differ from it, in different lines: for the same
  formula MariaDB's own result differs from ASQL's in 4,793 lines and from DuckDB's in 5,029, each by one cent. A
  100,000-line cash account's total differs by a few dollars between engines for that reason.
- `nopurchase` ends in `LIMIT 100` with no `ORDER BY`, so each engine may return a different 100 debtors. Without the
  `LIMIT` (and with the same timestamp bound on DuckDB's side) both return the same 919 debtors.
- Earlier figures for these reports (2026-11-04) were on a different data set (summitholding, 12.5M rows) and are not
  comparable.

## Latency tails

p99, ms.

| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| Point lookup, DOUBLE | **0.090-0.131** | 0.135-0.190 | 0.641-0.780 | 0.444-0.504 |
| Point lookup, DECIMAL | **0.082-0.084** | 0.134-0.179 | 0.735-0.966 | 0.425-0.429 |
| Update, DOUBLE | **0.943-1.01** | 1.05-1.16 | **0.674-0.988** | 1.52-1.57 |
| Update, DECIMAL | **0.744-0.808** | 0.998-1.01 | 0.909-1.25 | 2.29-2.92 |

## ASQL's result cache

Repeating a query without `SQL_NO_CACHE` is answered from the cache in 0.02-0.09 ms. The other engines have no
result cache; every figure above is a computed answer.

## Storage

500,000 rows with a DOUBLE amount (measured on a Mac, 2026-11-04; not repeated here):

| Engine | Size |
|---|---|
| ASQL | 3.1 MB (table file on disk) |
| DuckDB | 8.0 MB |
| MariaDB | 37.7 MB (`.ibd`) |
| CedarDB | 39.7 MB (`pg_total_relation_size`) |

## Reproducing

`tools/asql-bench/README.md` has the commands (`bench.php` for the three wire engines with `AMOUNT=decimal|double`,
`duckbench.c` for DuckDB, `cmp.php` for the answer check). For the containers bind-mount the data folders from the
same disk as ASQL's root.
