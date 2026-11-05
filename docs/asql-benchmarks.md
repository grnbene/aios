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
| Bulk load from CSV (500k rows) | 383-462 | 1,113-1,137 | **383** | 388-405 |
| 100 `INSERT`s of 5,000 rows | **1,332-1,391** | 2,043-2,045 | 5,126-5,332 | 12,459-12,607 |
| Full-table aggregate | **0.783-0.851** | 90.8-91.0 | 1.61-1.67 | 4.13-4.37 |
| `GROUP BY` + top 10 | **1.33-1.80** | 117-119 | 2.67-2.74 | **1.39-2.09** |
| Filtered range aggregate | **0.836-0.985** | 77.5-77.6 | 1.21-1.35 | 1.68-2.88 |
| Pivot, 3 `SUM(CASE ...)` | **3.42-4.00** | 178-180 | 4.97-6.75 | 4.05-6.85 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 42.6-43.2 | 413-421 | 54.3-80.8 | **28.1-28.2** |
| Point lookup, median | **0.051-0.052** | 0.091-0.092 | 0.425-0.451 | 0.280-0.295 |
| Point lookups, one client | **15,544-16,637 QPS** | 10,434-10,456 QPS | 2,165-2,289 QPS | 2,813-3,408 QPS |
| Single-row update, median | **0.371-0.433** | 0.524-0.536 | 0.496-0.499 | 1.04 |
| Single-row updates, one client | **2,004-2,099 TPS** | 1,752-1,780 TPS | 1,906-1,933 TPS | 913-925 TPS |

**`amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 452-499 | 1,107-1,136 | **286-312** | 371-376 |
| 100 `INSERT`s of 5,000 rows | **1,369-1,437** | 2,043-2,045 | 5,213-5,227 | 12,482-12,632 |
| Full-table aggregate | **0.871-1.06** | 156-157 | 1.64-1.70 | 1.10-1.69 |
| `GROUP BY` + top 10 | 1.89-1.94 | 168-169 | 2.37-2.67 | **1.26-1.29** |
| Filtered range aggregate | **1.14-1.15** | 86.7-87.2 | 1.20-1.45 | 1.58-1.79 |
| Pivot, 3 `SUM(CASE ...)` | **4.05-4.53** | 335 | 6.92-7.09 | 6.41-7.14 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 42.2-43.8 | 442-444 | 68.2-87.7 | **34.4-34.5** |
| Point lookup, median | **0.049-0.051** | 0.090-0.093 | 0.421-0.446 | 0.277-0.280 |
| Point lookups, one client | **16,991-17,736 QPS** | 9,978-10,170 QPS | 2,165-2,308 QPS | 3,419-3,439 QPS |
| Single-row update, median | **0.353-0.361** | 0.546-0.550 | 0.513-0.520 | 1.04-1.06 |
| Single-row updates, one client | **2,087-2,139 TPS** | 1,750-1,757 TPS | 1,852-1,895 TPS | 904-923 TPS |

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
| Idle, empty database | 37 | 116-117 | 176-178 | 32 |
| Peak during the run, DOUBLE | 220 | 244-268 | 445-456 | 126-133 |
| Peak during the run, DECIMAL | 216-218 | 234-235 | 465-476 | 128-135 |
| After the run, DOUBLE | 123-124 | 244-268 | 442-456 | 78-79 |
| After the run, DECIMAL | 127-131 | 234-235 | 462-476 | 78 |

An empty ASQL server is 36-37 MB resident (DuckDB's is 32). Its peak (about 220 MB) is the CSV load, which now converts
rows to their stored form as it parses them; the 500,000-row table itself adds about 20 MB. After the run it holds
123-131 MB, about a quarter of CedarDB's and half of MariaDB's.

Server CPU time per phase (mean of the two passes; "cores" = CPU time / wall time):

| Server CPU, DOUBLE | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 795 ms, 1.9 cores | 1,175 ms, 1.0 cores | 430 ms, 1.1 cores | 533 ms, 1.3 cores |
| 100 `INSERT`s of 5,000 rows | 1,585 ms, 1.2 cores | 2,345 ms, 1.1 cores | 6,695 ms, 1.3 cores | 12,451 ms, 1.0 cores |
| Full-table aggregate, per run | 0.93 ms, 1.1 cores | 92.3 ms, 1.0 cores | 3.58 ms, 2.0 cores | 8.96 ms, 2.5 cores |
| `GROUP BY` + top 10, per run | 2.85 ms, 1.8 cores | 119 ms, 1.0 cores | 5.37 ms, 1.8 cores | 4.39 ms, 2.4 cores |
| Filtered range aggregate, per run | 2.09 ms, 2.0 cores | 77.7 ms, 1.0 cores | 2.21 ms, 1.6 cores | 5.63 ms, 2.8 cores |
| Pivot, per run | 6.85 ms, 1.6 cores | 178 ms, 1.0 cores | 11.6 ms, 2.0 cores | 14.6 ms, 2.9 cores |
| Window function, per run | 43.3 ms, 1.0 cores | 415 ms, 1.0 cores | 178 ms, 2.8 cores | 102 ms, 3.3 cores |
| Point lookup, each | 56 µs, 0.9 cores | 54 µs, 0.6 cores | 396 µs, 0.9 cores | 431 µs, 1.3 cores |
| Single-row update, each | 205 µs, 0.4 cores | 112 µs, 0.2 cores | 530 µs, 1.0 cores | 800 µs, 0.7 cores |

| Server CPU, DECIMAL | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 885 ms, 1.9 cores | 1,185 ms, 1.1 cores | 380 ms, 1.3 cores | 573 ms, 1.5 cores |
| 100 `INSERT`s of 5,000 rows | 1,730 ms, 1.2 cores | 2,395 ms, 1.2 cores | 6,630 ms, 1.3 cores | 12,487 ms, 1.0 cores |
| Full-table aggregate, per run | 2.90 ms, 2.5 cores | 156 ms, 1.0 cores | 3.49 ms, 2.0 cores | 3.32 ms, 2.7 cores |
| `GROUP BY` + top 10, per run | 5.07 ms, 2.9 cores | 170 ms, 1.0 cores | 5.43 ms, 1.9 cores | 3.88 ms, 2.3 cores |
| Filtered range aggregate, per run | 4.04 ms, 3.0 cores | 87.1 ms, 1.0 cores | 2.14 ms, 1.4 cores | 3.40 ms, 1.8 cores |
| Pivot, per run | 10.2 ms, 2.5 cores | 333 ms, 1.0 cores | 12.2 ms, 2.2 cores | 15.6 ms, 2.4 cores |
| Window function, per run | 43.8 ms, 1.0 cores | 449 ms, 1.0 cores | 174 ms, 2.4 cores | 96.8 ms, 2.5 cores |
| Point lookup, each | 55 µs, 1.0 cores | 55 µs, 0.6 cores | 397 µs, 0.9 cores | 410 µs, 1.4 cores |
| Single-row update, each | 208 µs, 0.4 cores | 108 µs, 0.2 cores | 558 µs, 1.0 cores | 861 µs, 0.8 cores |

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
| Point lookup, DOUBLE | 0.135-0.145 | 0.142-0.144 | 0.610-0.675 | 0.434-1.37 |
| Update, DOUBLE | 1.03-1.10 | 0.929-1.49 | 0.794-1.03 | 1.96-3.08 |

## ASQL's result cache

Repeating a query without `SQL_NO_CACHE` is answered from the cache in 0.02-0.11 ms. The other engines have no
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
