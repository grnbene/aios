# ASQL against MariaDB, CedarDB and DuckDB

500,000 rows, one machine, each engine run alone, two passes. Lower is better except QPS and TPS; each cell spans the
two passes; best in bold (several where ranges overlap).

## Setup

| | |
|---|---|
| Host | Intel i5-7600K (4 cores, 3.8 GHz), 19 GB, Linux 6.17, WD Blue SN570 NVMe, ext4 |
| ASQL | `5.7.44-aios-asql`, commit `cef63cc` plus the wire-`INSERT` fast path and the pivot changes, 4 lanes, default durability (one `fdatasync` per commit) |
| MariaDB | 10.11.9 (Docker), InnoDB, `innodb_buffer_pool_size=1G`, `innodb_flush_log_at_trx_commit=1` |
| CedarDB | v2026-11-04 (Docker), defaults |
| DuckDB | v1.5.5, in process (C API), database file |

- All engines' data on the same NVMe (a flush costs about 0.5 ms there); containers use bind mounts.
- The table is about 21 MB and fits in every engine's cache; no engine was disk-bound for the queries.
- Machine shared with other work: load average 1.7-2.9 on 4 cores.
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

**`amount DOUBLE`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 976-1,056 | 1,139-1,171 | **309-340** | 400-403 |
| 100 `INSERT`s of 5,000 rows | **1,329-1,353** | 2,080-2,138 | 5,091-5,127 | 12,395-12,482 |
| Full-table aggregate | **0.825-0.853** | 91.3-92.9 | 1.61-1.72 | 2.29-2.32 |
| `GROUP BY` + top 10 | **1.32-1.52** | 115-117 | 2.29-5.45 | **1.44-2.15** |
| Filtered range aggregate | **0.910-1.31** | 78.3-80.7 | **1.23-1.24** | 1.70-2.59 |
| Pivot, 3 `SUM(CASE ...)` | **2.58-2.74** | 182-198 | 4.10-5.09 | 4.08-6.59 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **26.0-26.9** | 413-420 | 61.5-66.8 | 34.8-37.7 |
| Point lookup, median | **0.050-0.051** | 0.089-0.099 | 0.440-0.446 | 0.282-0.283 |
| Point lookups, one client | **17,151-17,817 QPS** | 8,767-10,676 QPS | 2,195-2,207 QPS | 3,324-3,363 QPS |
| Single-row update, median | **0.367-0.392** | 0.548-0.561 | 0.500-0.503 | 1.02-1.07 |
| Single-row updates, one client | **2,093-2,167 TPS** | 1,656-1,758 TPS | 1,920-1,955 TPS | 854-930 TPS |

**`amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 997-1,140 | 1,114-1,149 | **318-331** | 375-411 |
| 100 `INSERT`s of 5,000 rows | **1,368-1,426** | 2,041-2,063 | 5,148-5,185 | 12,559-12,628 |
| Full-table aggregate | 4.22-4.23 | 157-158 | **1.86-2.14** | **1.80-1.88** |
| `GROUP BY` + top 10 | **1.64-3.14** | 169-173 | 2.67-3.28 | **1.88-2.22** |
| Filtered range aggregate | 1.81-3.09 | 85.6-88.2 | **1.18-1.25** | 1.57-1.65 |
| Pivot, 3 `SUM(CASE ...)` | **3.43-7.51** | 332-338 | **4.26-4.35** | 6.85-7.20 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **26.6** | 449 | 63.7-68.4 | 27.2-34.4 |
| Point lookup, median | **0.051-0.054** | 0.087-0.092 | 0.449-0.452 | 0.283 |
| Point lookups, one client | **14,931-16,324 QPS** | 10,186-10,910 QPS | 2,115-2,155 QPS | 3,428-3,434 QPS |
| Single-row update, median | **0.361-0.367** | 0.549-0.569 | 0.502-0.518 | 1.05 |
| Single-row updates, one client | **2,113-2,157 TPS** | 1,679-1,729 TPS | 1,854-1,918 TPS | 902-911 TPS |

- **DOUBLE**: ASQL is fastest or level on every test but file loading: the wire `INSERT`s (MariaDB takes 1.5-1.6x as
  long), full aggregate, `GROUP BY`, range query, pivot, window function, point lookups and single-row updates. File
  loading is where it trails (CedarDB 2.9-3.4x faster, DuckDB 2.4-2.6x; ASQL is ahead of MariaDB).
- **DECIMAL**: the wire `INSERT`s, window function (level with DuckDB), point lookups and updates are ahead or level,
  and the pivot is level with CedarDB's and DuckDB's (3.4-7.5 ms against 4.3 and 6.9-7.2). The other analytics trail
  CedarDB and DuckDB: the full aggregate by 2.0-2.4x, range query 1.1-2.6x and `GROUP BY` up to 1.7x; loading a file
  is 2.4-3.6x behind. ASQL's DECIMAL figures vary between passes.

## Memory and CPU

Resident memory of the engine's process, in MB (range over the two passes):

| Resident memory (RSS), MB | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Idle, empty database | 454 | 116 | 178 | 33 |
| Peak during the run, DOUBLE | 859-860 | 240-243 | 463-497 | 126-128 |
| Peak during the run, DECIMAL | 854-856 | 230-234 | 457-469 | 126-133 |
| After the run, DOUBLE | 621-640 | 240-243 | 463-497 | 80-81 |
| After the run, DECIMAL | 638 | 230-234 | 457-469 | 79-80 |

ASQL holds its data in memory: an empty server is already 454 MB resident, and it has the largest footprint for this
table (peak 1.7-1.9x CedarDB's, 3.5-3.7x MariaDB's, 6.4-6.8x DuckDB's).

Server CPU time per phase (mean of the two passes; "cores" = CPU time / wall time):

| Server CPU, DOUBLE | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 1,620 ms, 1.6 cores | 1,170 ms, 1.0 cores | 395 ms, 1.2 cores | 557 ms, 1.4 cores |
| 100 `INSERT`s of 5,000 rows | 1,605 ms, 1.2 cores | 2,455 ms, 1.2 cores | 6,140 ms, 1.2 cores | 12,423 ms, 1.0 cores |
| Full-table aggregate, per run | 0.91 ms, 1.1 cores | 90.9 ms, 1.0 cores | 3.62 ms, 1.9 cores | 8.09 ms, 3.3 cores |
| `GROUP BY` + top 10, per run | 2.86 ms, 1.7 cores | 117 ms, 1.0 cores | 5.44 ms, 1.9 cores | 4.33 ms, 2.6 cores |
| Filtered range aggregate, per run | 2.21 ms, 1.8 cores | 78.5 ms, 1.0 cores | 2.37 ms, 1.9 cores | 5.78 ms, 2.6 cores |
| Pivot, per run | 6.40 ms, 1.8 cores | 194 ms, 1.0 cores | 11.6 ms, 2.4 cores | 15.8 ms, 2.3 cores |
| Window function, per run | 28.9 ms, 1.0 cores | 414 ms, 1.0 cores | 177 ms, 2.4 cores | 96.0 ms, 2.6 cores |
| Point lookup, each | 55 µs, 1.0 cores | 57 µs, 0.5 cores | 392 µs, 0.9 cores | 419 µs, 1.4 cores |
| Single-row update, each | 195 µs, 0.4 cores | 115 µs, 0.2 cores | 535 µs, 1.0 cores | 859 µs, 0.8 cores |

| Server CPU, DECIMAL | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 1,735 ms, 1.6 cores | 1,170 ms, 1.0 cores | 380 ms, 1.2 cores | 525 ms, 1.3 cores |
| 100 `INSERT`s of 5,000 rows | 1,745 ms, 1.2 cores | 2,385 ms, 1.2 cores | 6,465 ms, 1.3 cores | 12,505 ms, 1.0 cores |
| Full-table aggregate, per run | 8.10 ms, 2.3 cores | 159 ms, 1.0 cores | 3.47 ms, 1.9 cores | 3.47 ms, 1.9 cores |
| `GROUP BY` + top 10, per run | 6.05 ms, 2.4 cores | 169 ms, 1.0 cores | 5.53 ms, 1.9 cores | 4.02 ms, 1.8 cores |
| Filtered range aggregate, per run | 6.11 ms, 2.4 cores | 87.1 ms, 1.0 cores | 2.21 ms, 1.7 cores | 3.17 ms, 1.9 cores |
| Pivot, per run | 10.7 ms, 2.2 cores | 337 ms, 1.0 cores | 12.3 ms, 2.6 cores | 15.0 ms, 2.3 cores |
| Window function, per run | 30.8 ms, 1.0 cores | 448 ms, 1.0 cores | 181 ms, 3.4 cores | 97.0 ms, 3.0 cores |
| Point lookup, each | 57 µs, 0.9 cores | 54 µs, 0.6 cores | 406 µs, 0.9 cores | 434 µs, 1.5 cores |
| Single-row update, each | 202 µs, 0.4 cores | 112 µs, 0.2 cores | 545 µs, 1.0 cores | 835 µs, 0.8 cores |

- With DOUBLE, ASQL spends the least CPU on all five analytics: the full aggregate takes 0.9 ms against 3.6-91 ms, the
  pivot 6.4 ms against 11.6-194 ms, and the window function runs on one core. With DECIMAL it spends 1.1-2.8x the CPU of
  CedarDB or DuckDB on the aggregates (8.1 ms against 3.5 ms for the full aggregate), but the pivot costs 10.7 ms
  against 12.3-15 ms.
- File loading costs ASQL 1.6-1.7 s of CPU against 0.4-0.6 s for CedarDB and DuckDB; the wire `INSERT`s cost it
  1.6-1.7 s, against 2.4 s for MariaDB and 6.1-6.5 s for CedarDB.
- Point lookups cost 55 µs of CPU in ASQL and MariaDB, 400 µs in CedarDB and DuckDB; a single-row update costs 195-200 µs
  in ASQL, 115 µs in MariaDB.

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

## Latency tails

p99, ms.

| | ASQL | MariaDB | CedarDB | DuckDB |
|---|---|---|---|---|
| Point lookup, DOUBLE | 0.107-0.113 | 0.151-0.231 | 0.668-0.672 | 0.427-0.467 |
| Update, DOUBLE | 0.992-1.21 | 0.926-1.22 | 0.697-0.877 | 1.50-3.43 |

## ASQL's result cache

Repeating a query without `SQL_NO_CACHE` is answered from the cache in 0.02-0.12 ms. The other engines have no
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
