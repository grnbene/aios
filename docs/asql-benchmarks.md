# ASQL against MariaDB, CedarDB and DuckDB

500,000 rows, one machine, each engine run alone, two passes. Lower is better except QPS and TPS; each cell spans the
two passes; best in bold (several where ranges overlap).

## Setup

| | |
|---|---|
| Host | Intel i5-7600K (4 cores, 3.8 GHz), 19 GB, Linux 6.17, WD Blue SN570 NVMe, ext4 |
| ASQL | `5.7.44-aios-asql`, commit `96b80bf` plus the `DECIMAL` aggregate, lazy-accelerator and scan-scheduling changes, 4 lanes, default durability (one `fdatasync` per commit) |
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
| Bulk load from CSV (500k rows) | 402-724 | 1,126-1,139 | **374-375** | 379-386 |
| 100 `INSERT`s of 5,000 rows | **1,336-1,642** | 2,065-2,181 | 5,228-5,313 | 12,522-12,609 |
| Full-table aggregate | **0.798-0.822** | 91.5-92.6 | 1.63-2.36 | 2.32-4.00 |
| `GROUP BY` + top 10 | **1.47-3.27** | 118 | 2.67-2.73 | **1.38-2.19** |
| Filtered range aggregate | **0.999-1.28** | 77.1-79.8 | **1.16-1.27** | 1.66-2.79 |
| Pivot, 3 `SUM(CASE ...)` | **3.10-4.04** | 178-181 | **4.02-5.12** | 4.19-7.02 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 43.4-45.5 | 416-433 | 49.0-75.0 | **35.4-38.0** |
| Point lookup, median | **0.051-0.057** | 0.090-0.104 | 0.438-0.443 | 0.278-0.284 |
| Point lookups, one client | **13,151-17,310 QPS** | 8,725-10,533 QPS | 2,157-2,228 QPS | 3,394-3,408 QPS |
| Single-row update, median | **0.312-0.451** | 0.523-0.533 | 0.491-0.510 | 1.05-1.07 |
| Single-row updates, one client | **1,982-2,396 TPS** | 1,764-1,791 TPS | 1,913-1,981 TPS | 875-907 TPS |

**`amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 423-879 | 1,152-1,176 | **283-325** | 422-438 |
| 100 `INSERT`s of 5,000 rows | **1,365-1,695** | 2,033-2,182 | 5,197-5,338 | 12,501-12,678 |
| Full-table aggregate | **1.12-1.73** | 157-159 | **1.60-1.63** | **1.66-1.69** |
| `GROUP BY` + top 10 | **1.46-1.51** | 169 | 2.37-5.51 | 1.96-2.02 |
| Filtered range aggregate | **1.18-1.43** | 86.8-88.3 | **1.16-1.43** | 1.61-1.65 |
| Pivot, 3 `SUM(CASE ...)` | **3.89-4.04** | 334-336 | 4.35-7.10 | 4.18-6.78 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 43.3-45.0 | 445 | 57.4-73.1 | **28.7-36.7** |
| Point lookup, median | **0.050-0.080** | 0.089-0.092 | 0.433-0.439 | 0.282-0.284 |
| Point lookups, one client | **9,575-17,482 QPS** | **9,954-10,574 QPS** | 2,222-2,258 QPS | 3,131-3,443 QPS |
| Single-row update, median | **0.360-0.364** | 0.538-0.557 | 0.512 | 1.04-1.08 |
| Single-row updates, one client | **1,828-2,139 TPS** | 1,696-1,768 TPS | **1,900-1,909 TPS** | 863-908 TPS |

- **DOUBLE**: ASQL is fastest or level on every test: file loading (level with CedarDB and DuckDB, 2.5x faster than
  MariaDB), the wire `INSERT`s (MariaDB takes 1.4-1.5x as long), full aggregate, `GROUP BY` (level with DuckDB), range
  query, pivot, window function, point lookups and single-row updates.
- **DECIMAL**: ASQL is fastest or level on every test but file loading and `GROUP BY`: the wire `INSERT`s, full
  aggregate, range query (level with CedarDB), pivot (1.2-2.3x faster than CedarDB and DuckDB), window function, point
  lookups and updates. `GROUP BY` is 1.4x behind DuckDB (ahead of CedarDB); file loading is 1.5-1.7x behind CedarDB and
  1.2-1.3x behind DuckDB (2.3x faster than MariaDB).
- ASQL's figures on this machine vary between runs, by up to 2x (the range query 0.85 ms or 1.5 ms on the same build).
  The scans whose totals do not depend on the order of additions hand morsels to whichever thread is free; the others
  keep a fixed share a thread, so that a DOUBLE total is the same every run. Several of the engine's threads were often
  seen sharing a core on this machine (it is shared; see Setup), which would explain it, but that is not proven.

## Memory and CPU

Resident memory of the engine's process, in MB (range over the two passes):

| Resident memory (RSS), MB | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Idle, empty database | 37 | 116 | 178 | 32 |
| Peak during the run, DOUBLE | 221-222 | 235-245 | 430-433 | 132-133 |
| Peak during the run, DECIMAL | 217 | 233-237 | 448-451 | 137-138 |
| After the run, DOUBLE | 130-134 | 235-245 | 426-433 | 78-80 |
| After the run, DECIMAL | 125 | 233-237 | 448-451 | 80-82 |

An empty ASQL server is 36-37 MB resident (DuckDB's is 32). Its peak (about 220 MB) is the CSV load, which now converts
rows to their stored form as it parses them; the 500,000-row table itself adds about 20 MB. After the run it holds
123-131 MB, about a quarter of CedarDB's and half of MariaDB's.

Server CPU time per phase (mean of the two passes; "cores" = CPU time / wall time):

| Server CPU, DOUBLE | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 810 ms, 1.4 cores | 1,100 ms, 1.0 cores | 430 ms, 1.1 cores | 521 ms, 1.4 cores |
| 100 `INSERT`s of 5,000 rows | 1,625 ms, 1.1 cores | 2,475 ms, 1.2 cores | 6,115 ms, 1.2 cores | 12,480 ms, 1.0 cores |
| Full-table aggregate, per run | 0.92 ms, 1.0 cores | 90.9 ms, 1.0 cores | 3.59 ms, 2.0 cores | 8.69 ms, 2.5 cores |
| `GROUP BY` + top 10, per run | 2.88 ms, 1.3 cores | 118 ms, 0.9 cores | 5.47 ms, 2.1 cores | 4.32 ms, 2.1 cores |
| Filtered range aggregate, per run | 2.01 ms, 1.6 cores | 77.7 ms, 1.0 cores | 2.35 ms, 1.9 cores | 5.89 ms, 2.3 cores |
| Pivot, per run | 6.43 ms, 1.5 cores | 182 ms, 1.0 cores | 11.6 ms, 2.4 cores | 15.8 ms, 2.4 cores |
| Window function, per run | 43.7 ms, 1.0 cores | 422 ms, 1.0 cores | 177 ms, 2.8 cores | 96.9 ms, 2.5 cores |
| Point lookup, each | 56 µs, 0.8 cores | 56 µs, 0.5 cores | 402 µs, 0.9 cores | 417 µs, 1.4 cores |
| Single-row update, each | 185 µs, 0.4 cores | 110 µs, 0.2 cores | 540 µs, 1.1 cores | 859 µs, 0.8 cores |

| Server CPU, DECIMAL | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 910 ms, 1.4 cores | 1,175 ms, 1.0 cores | 360 ms, 1.2 cores | 641 ms, 1.5 cores |
| 100 `INSERT`s of 5,000 rows | 1,760 ms, 1.2 cores | 2,405 ms, 1.1 cores | 6,395 ms, 1.2 cores | 12,545 ms, 1.0 cores |
| Full-table aggregate, per run | 2.88 ms, 2.5 cores | 157 ms, 1.0 cores | 3.47 ms, 1.9 cores | 3.43 ms, 1.9 cores |
| `GROUP BY` + top 10, per run | 5.14 ms, 2.7 cores | 168 ms, 1.0 cores | 5.37 ms, 2.0 cores | 3.95 ms, 2.0 cores |
| Filtered range aggregate, per run | 4.11 ms, 2.7 cores | 87.1 ms, 1.0 cores | 2.26 ms, 1.7 cores | 3.20 ms, 2.1 cores |
| Pivot, per run | 9.96 ms, 2.2 cores | 335 ms, 1.0 cores | 12.2 ms, 2.6 cores | 14.5 ms, 2.6 cores |
| Window function, per run | 43.2 ms, 1.0 cores | 450 ms, 1.0 cores | 178 ms, 2.5 cores | 99.9 ms, 3.0 cores |
| Point lookup, each | 58 µs, 0.7 cores | 56 µs, 0.6 cores | 390 µs, 0.9 cores | 432 µs, 1.4 cores |
| Single-row update, each | 218 µs, 0.4 cores | 108 µs, 0.2 cores | 550 µs, 1.0 cores | 851 µs, 0.8 cores |

- With DOUBLE, ASQL spends the least CPU on all five analytics: the full aggregate takes 0.9 ms against 3.6-92 ms, the
  pivot 7.0 ms against 11.6-178 ms, and the window function runs on one core. With DECIMAL it spends the least on the
  full aggregate (2.8 ms against 3.3-3.5) and the pivot (10.2 ms against 12.2-15.6), the same as CedarDB or DuckDB on
  `GROUP BY` (5.0 ms against 3.9-5.4) and 1.2-1.8x on the range query (3.9 ms against 2.1-3.4).
- File loading costs ASQL 0.8-0.9 s of CPU against 0.4-0.6 s for CedarDB and DuckDB, and 1.2 s for MariaDB; the wire `INSERT`s cost it
  1.6-1.7 s, against 2.4 s for MariaDB and 6.1-6.5 s for CedarDB.
- Point lookups cost 55 µs of CPU in ASQL and MariaDB, 400 µs in CedarDB and DuckDB; a single-row update costs 185-215 µs
  in ASQL, 110 µs in MariaDB.

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
| Point lookup, DOUBLE | 0.099-0.176 | 0.144-0.331 | 0.640-0.779 | 0.406-0.442 |
| Update, DOUBLE | 1.04-1.08 | 1.06-1.57 | 0.697-0.795 | 1.96-2.87 |

## ASQL's result cache

Repeating a query without `SQL_NO_CACHE` is answered from the cache in 0.02-0.08 ms. The other engines have no
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
