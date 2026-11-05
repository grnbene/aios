# ASQL against MariaDB, CedarDB and DuckDB

500,000 rows, one machine, each engine run alone, two passes. Lower is better except QPS and TPS; each cell spans the
two passes; best in bold (several where ranges overlap).

## Setup

| | |
|---|---|
| Host | Intel i5-7600K (4 cores, 3.8 GHz), 19 GB, Linux 6.17, WD Blue SN570 NVMe, ext4 |
| ASQL | `5.7.44-aios-asql`, commit `cef63cc`, 4 lanes, default durability (one `fdatasync` per commit) |
| MariaDB | 10.11.9 (Docker), InnoDB, `innodb_buffer_pool_size=1G`, `innodb_flush_log_at_trx_commit=1` |
| CedarDB | v2026-11-04 (Docker), defaults |
| DuckDB | v1.5.5, in process (C API), database file |

- All engines' data on the same NVMe (a flush costs about 0.5 ms there); containers use bind mounts.
- Machine shared with other work: load average 1.7-2.8 on 4 cores.
- Data: `bench_sales(id INT PRIMARY KEY, customer_id INT, category_id INT, amount, trans_date DATE, status VARCHAR(16))`,
  500,000 rows from a fixed seed (`tools/asql-bench/gen.py`), run with `amount DOUBLE` and with `amount DECIMAL(12,2)`
  (`NUMERIC(12,2)` in CedarDB), each a fresh table.
- Timing: per statement at the client (PHP `mysqli`/`pgsql`, one persistent loopback connection; DuckDB around
  `duckdb_query`, in process, so no network). Queries: 1 warm-up, median of 5; ASQL timed with `SQL_NO_CACHE`.
- Every query's rows from every engine were compared with MariaDB's (within 1e-9 relative): all matched.

## Results

**`amount DOUBLE`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 972-1,023 | 1,137-1,214 | **284-293** | 378 |
| 100 `INSERT`s of 5,000 rows | 3,207-3,237 | **2,008-2,076** | 5,071-5,160 | 12,485-12,578 |
| Full-table aggregate | **0.785-0.798** | 91.1-91.7 | 1.41-1.59 | 2.33-4.36 |
| `GROUP BY` + top 10 | **1.42-1.68** | 116-117 | 2.20-2.62 | **1.44-2.19** |
| Filtered range aggregate | **0.911-1.21** | 77.9-78.8 | 1.24-1.28 | 1.72-2.54 |
| Pivot, 3 `SUM(CASE ...)` | 5.09-6.70 | 179-181 | **3.95-4.04** | 7.20-7.46 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **22.3-22.5** | 416-426 | 50.9-53.2 | 37.0-37.7 |
| Point lookup, median | **0.050** | 0.088-0.089 | 0.442-0.445 | 0.294 |
| Point lookups, one client | **18,044-18,474 QPS** | 10,504-10,768 QPS | 2,152-2,207 QPS | 3,131-3,181 QPS |
| Single-row update, median | **0.364-0.424** | 0.562-0.574 | 0.493-0.498 | 1.03 |
| Single-row updates, one client | **2,024-2,248 TPS** | 1,664-1,730 TPS | 1,923-1,955 TPS | 924-927 TPS |

**`amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 893-968 | 1,112-1,235 | **272-327** | 383-384 |
| 100 `INSERT`s of 5,000 rows | 2,682-3,058 | **1,994-2,066** | 5,170-5,176 | 12,659-12,669 |
| Full-table aggregate | 2.13-3.98 | 159 | **1.68-2.24** | **1.10-2.10** |
| `GROUP BY` + top 10 | 2.67-5.02 | 170 | 2.46-2.75 | **1.29-1.99** |
| Filtered range aggregate | 1.64-3.01 | 86.6-89.8 | **1.13-1.20** | **1.08-1.58** |
| Pivot, 3 `SUM(CASE ...)` | 6.97-12.8 | 338-350 | **4.25** | 5.72-6.82 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **23.4-25.0** | 445-450 | 53.3-65.3 | 38.3-46.1 |
| Point lookup, median | **0.056** | 0.087 | 0.438-0.447 | 0.284-0.287 |
| Point lookups, one client | **16,130-16,444 QPS** | 10,695-10,869 QPS | 2,144-2,217 QPS | 3,313-3,421 QPS |
| Single-row update, median | **0.377-0.455** | 0.542-0.549 | 0.507-0.538 | 1.05-1.06 |
| Single-row updates, one client | **1,865-2,007 TPS** | 1,722-1,771 TPS | **1,729-1,922 TPS** | 888-913 TPS |

- ASQL leads or ties on analytics, window functions, point lookups and single-row updates with a DOUBLE amount.
- It trails on file loading (CedarDB 3x, DuckDB 2.6x faster), wire `INSERT`s (MariaDB 1.6x faster) and the pivot
  (CedarDB 1.3-1.7x faster).
- With DECIMAL, the full aggregate, `GROUP BY`, range query and pivot take 1.0-3.9x the fastest engine's time.

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
| Point lookup, DOUBLE | 0.084-0.089 | 0.155-0.172 | 0.658-0.824 | 0.523-0.548 |
| Update, DOUBLE | 0.932-1.01 | 0.767-1.02 | 0.821-0.931 | 1.46-2.66 |

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
