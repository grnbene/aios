# ASQL against MariaDB, CedarDB and DuckDB

500,000 rows, one machine, each engine run alone, two passes. Lower is better except QPS and TPS; each cell spans the
two passes; best in bold (several where ranges overlap).

## Setup

| | |
|---|---|
| Host | Intel i5-7600K (4 cores, 3.8 GHz), 19 GB, Linux 6.17, WD Blue SN570 NVMe, ext4 |
| ASQL | `5.7.44-aios-asql`, commit `bf3bd8e`, 4 lanes, default durability (one `fdatasync` per commit) |
| MariaDB | 10.11.9 (Docker), InnoDB, `innodb_buffer_pool_size=1G`, `innodb_flush_log_at_trx_commit=1` |
| CedarDB | v2026-11-04 (Docker), defaults |
| DuckDB | v1.5.5, in process (C API), database file |

- All engines' data on the same NVMe (a flush costs about 0.5 ms there); containers use bind mounts.
- The table is about 21 MB and fits in every engine's cache; no engine was disk-bound for the queries.
- Machine shared with other work: load average 0.9-2.7 on 4 cores.
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
| Bulk load from CSV (500k rows) | 441-492 | 1,138-1,192 | **278-285** | 417-425 |
| 100 `INSERT`s of 5,000 rows | **1,301-1,417** | 2,043-2,078 | 5,089-5,265 | 12,501-12,735 |
| Full-table aggregate | **0.775-0.791** | 90.4-92.0 | 1.53-1.55 | 3.93-3.96 |
| `GROUP BY` + top 10 | **1.39-1.43** | 116-117 | 2.30-2.56 | 2.13-2.21 |
| Filtered range aggregate | **0.802-2.00** | 76.5-76.6 | **1.22-1.24** | 2.58-2.59 |
| Pivot, 3 `SUM(CASE ...)` | **3.25-3.73** | 178-179 | 4.85-5.24 | 5.99-6.63 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | 41.0 | 416-425 | 89.4-122 | **27.9-36.5** |
| Point lookup, median | **0.051-0.052** | 0.088-0.089 | 0.419-0.443 | 0.280-0.284 |
| Point lookups, one client | **17,430-17,451 QPS** | 10,668-10,828 QPS | 2,208-2,326 QPS | 3,323-3,435 QPS |
| Single-row update, median | **0.365-0.428** | 0.550-0.557 | 0.521 | 1.02-1.04 |
| Single-row updates, one client | **2,070-2,256 TPS** | 1,708-1,720 TPS | 1,854-1,862 TPS | 910-937 TPS |

**`amount DECIMAL(12,2)`**

| Test | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV (500k rows) | 469-491 | 1,129-1,144 | **316-329** | 373-445 |
| 100 `INSERT`s of 5,000 rows | **1,348-1,417** | 2,051-2,165 | 5,203-5,250 | 12,581-12,652 |
| Full-table aggregate | **1.10-1.16** | 156 | 1.55-1.63 | **1.09-1.67** |
| `GROUP BY` + top 10 | 1.84-2.35 | 166-168 | 2.64-3.32 | **1.28-1.83** |
| Filtered range aggregate | 1.41-1.47 | 85.5-90.6 | 1.17-1.25 | **1.06** |
| Pivot, 3 `SUM(CASE ...)` | **3.86-4.00** | 333-335 | 4.46-5.22 | 6.16-7.13 |
| `ROW_NUMBER() OVER (PARTITION BY ...)` | **40.2-41.6** | 441-454 | 57.2-81.7 | **28.0-49.6** |
| Point lookup, median | **0.050** | 0.087-0.091 | 0.443-0.458 | 0.277-0.280 |
| Point lookups, one client | **18,102-18,339 QPS** | 10,305-10,761 QPS | 2,126-2,199 QPS | 3,372-3,439 QPS |
| Single-row update, median | **0.375-0.387** | 0.551-0.556 | 0.522-0.534 | 1.04-1.05 |
| Single-row updates, one client | **2,080-2,081 TPS** | 1,695-1,730 TPS | 1,845-1,857 TPS | 904-912 TPS |

- **DOUBLE**: ASQL is fastest on the wire `INSERT`s (MariaDB takes 1.4-1.6x as long), full aggregate, `GROUP BY`, pivot,
  point lookups and single-row updates, and level with CedarDB on the range query. It is behind on two: file loading
  (1.6-1.8x behind CedarDB, 1.0-1.2x behind DuckDB, 2.3-2.7x faster than MariaDB) and the window function (1.1-1.5x
  behind DuckDB, 2.2-3.0x ahead of CedarDB).
- **DECIMAL**: ASQL is fastest or level on the wire `INSERT`s (MariaDB takes 1.5-1.6x as long), full aggregate (level
  with DuckDB), pivot (1.1-1.9x faster than CedarDB and DuckDB), window function (level with DuckDB), point lookups and
  updates. It is behind on file loading (1.4-1.6x behind CedarDB, 1.1-1.3x behind DuckDB, 2.3-2.4x faster than MariaDB),
  `GROUP BY` (up to 1.8x behind DuckDB, 1.1-1.8x ahead of CedarDB) and the range query (1.1-1.3x behind CedarDB, 1.3-1.4x
  behind DuckDB).
- ASQL's figures on this machine vary between runs, by up to 2x (the DOUBLE range query 0.80 ms in one pass, 2.0 ms in the other, on the same build).
  The scans whose totals do not depend on the order of additions hand morsels to whichever thread is free; the others
  keep a fixed share a thread, so that a DOUBLE total is the same every run. Several of the engine's threads were often
  seen sharing a core on this machine (it is shared; see Setup), which would explain it, but that is not proven.

## Memory and CPU

Resident memory of the engine's process, in MB (range over the two passes):

| Resident memory (RSS), MB | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Idle, empty database | 36-37 | 116 | 177-178 | 32 |
| Peak during the run, DOUBLE | 222-223 | 240-242 | 446-475 | 131-136 |
| Peak during the run, DECIMAL | 216-219 | 234-260 | 432-471 | 132-135 |
| After the run, DOUBLE | 129-136 | 240-242 | 446-475 | 78-80 |
| After the run, DECIMAL | 124-125 | 234-260 | 432-471 | 78-80 |

An empty ASQL server is 36-37 MB resident (DuckDB's is 32). Its peak (about 220 MB) is the CSV load, which now converts
rows to their stored form as it parses them; the 500,000-row table itself adds about 20 MB. After the run it holds
124-136 MB, under a third of CedarDB's and about half of MariaDB's.

Server CPU time per phase (mean of the two passes; "cores" = CPU time / wall time):

| Server CPU, DOUBLE | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 810 ms, 1.7 cores | 1,170 ms, 1.0 cores | 350 ms, 1.2 cores | 640 ms, 1.5 cores |
| 100 `INSERT`s of 5,000 rows | 1,585 ms, 1.2 cores | 2,420 ms, 1.2 cores | 6,425 ms, 1.2 cores | 12,454 ms, 1.0 cores |
| Full-table aggregate, per run | 0.91 ms, 1.1 cores | 90.9 ms, 1.0 cores | 3.65 ms, 2.1 cores | 9.00 ms, 2.1 cores |
| `GROUP BY` + top 10, per run | 2.86 ms, 1.8 cores | 116 ms, 1.0 cores | 5.36 ms, 2.0 cores | 4.57 ms, 2.1 cores |
| Filtered range aggregate, per run | 1.97 ms, 1.8 cores | 77.3 ms, 1.0 cores | 2.25 ms, 1.7 cores | 5.77 ms, 2.1 cores |
| Pivot, per run | 6.92 ms, 2.3 cores | 179 ms, 1.0 cores | 11.6 ms, 2.2 cores | 14.9 ms, 2.7 cores |
| Window function, per run | 41.2 ms, 1.0 cores | 414 ms, 1.0 cores | 173 ms, 2.3 cores | 97.8 ms, 2.9 cores |
| Point lookup, each | 56 µs, 1.0 cores | 54 µs, 0.6 cores | 392 µs, 0.9 cores | 435 µs, 1.5 cores |
| Single-row update, each | 190 µs, 0.4 cores | 115 µs, 0.2 cores | 565 µs, 1.0 cores | 815 µs, 0.8 cores |

| Server CPU, DECIMAL | ASQL | MariaDB 10.11 | CedarDB | DuckDB |
|---|---|---|---|---|
| Bulk load from CSV | 910 ms, 1.9 cores | 1,220 ms, 1.1 cores | 395 ms, 1.2 cores | 605 ms, 1.5 cores |
| 100 `INSERT`s of 5,000 rows | 1,730 ms, 1.3 cores | 2,380 ms, 1.1 cores | 6,235 ms, 1.2 cores | 12,578 ms, 1.0 cores |
| Full-table aggregate, per run | 2.90 ms, 2.4 cores | 156 ms, 1.0 cores | 3.51 ms, 2.1 cores | 3.37 ms, 2.2 cores |
| `GROUP BY` + top 10, per run | 5.20 ms, 2.6 cores | 167 ms, 1.0 cores | 5.46 ms, 2.0 cores | 3.81 ms, 2.6 cores |
| Filtered range aggregate, per run | 3.89 ms, 2.5 cores | 86.7 ms, 1.0 cores | 2.25 ms, 1.7 cores | 3.11 ms, 2.7 cores |
| Pivot, per run | 10.2 ms, 2.5 cores | 334 ms, 1.0 cores | 12.2 ms, 2.3 cores | 13.9 ms, 3.0 cores |
| Window function, per run | 41.4 ms, 1.0 cores | 444 ms, 1.0 cores | 175 ms, 2.0 cores | 97.7 ms, 2.6 cores |
| Point lookup, each | 54 µs, 1.0 cores | 54 µs, 0.6 cores | 400 µs, 0.9 cores | 405 µs, 1.4 cores |
| Single-row update, each | 215 µs, 0.4 cores | 112 µs, 0.2 cores | 572 µs, 1.1 cores | 825 µs, 0.7 cores |

- With DOUBLE, ASQL spends the least CPU on all five analytics: the full aggregate takes 0.9 ms against 3.7-91 ms, the
  pivot 6.9 ms against 11.6-179 ms, and the window function runs on one core. With DECIMAL it spends the least on the
  full aggregate (2.9 ms against 3.4-3.5), the pivot (10.2 ms against 12.2-13.9) and the window function, the same as
  CedarDB on `GROUP BY` (5.2 ms against 5.5; DuckDB 3.8) and 1.3-1.7x on the range query (3.9 ms against 2.3-3.1).
- File loading costs ASQL 0.8-0.9 s of CPU against 0.35-0.40 s for CedarDB, 0.6 s for DuckDB and 1.2 s for MariaDB; the
  wire `INSERT`s cost it 1.6-1.7 s, against 2.4 s for MariaDB and 6.2-6.4 s for CedarDB.
- Point lookups cost 54-56 µs of CPU in ASQL and MariaDB, 390-435 µs in CedarDB and DuckDB; a single-row update costs 190-215 µs
  in ASQL, 110-115 µs in MariaDB.

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
timed runs after one warm-up, two passes.

| Report | ASQL | DuckDB |
|---|---|---|
| `thisoverlast`: sales, cost and units by month, 36 `SUM(CASE ...)` over the view | **350-378** | **370-472** |
| `nopurchase`: customers with no purchase in 60 days but one in 365, four correlated subqueries each | **136-145** | 607-749 |
| `brandbylocation`: quantity and total per brand over the view | **534-588** | 592-806 |

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
| Point lookup, DOUBLE | 0.086-0.101 | 0.147-0.171 | 0.598-0.634 | 0.412-0.549 |
| Update, DOUBLE | 0.937-0.939 | 0.990-1.03 | 0.842-0.873 | 2.36-2.70 |

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
