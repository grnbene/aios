# ASQL against MariaDB 10.11, ninth pass: same-table writes side by side, 2026-10-19

Writes to one table no longer run one at a time: a single-row UPDATE by its
primary key changes its row in place beside the other such writes to the
table, and the redo log costs half what it did per commit. Same box and
settings as before (4 cores, one NVMe, MariaDB with its packaged settings
and `innodb_flush_log_at_trx_commit=1`, ASQL with `AIOS_TXN_CONCURRENT=1` and
its result cache off), except the comparison method: the box's disk and
CPU varied by up to 2x between hours this time (MariaDB's 8-session
point_select measured 143k on the 18th and 54-62k today), so every row
below is the median of 3 runs alternating ASQL and MariaDB 10 s apart, each
run started once the disk had been quiet for 3 s (MariaDB writes back pages
for ~15 s after a write run, which landed on the next run's syncs). The
numbers compare across a row, not with earlier passes.

Every session writing **one** table (sysbench `--tables=1`, tps):

| sessions | update_non_index ASQL \| MariaDB | insert | write_only |
|---|---|---|---|
| 8 | 10,407 \| 11,912 | 9,693 \| 10,891 | 3,523 \| 3,987 |
| 32 | 23,380 \| 17,896 | 19,723 \| 16,818 | 4,703 \| 4,341 |
| 128 | 28,974 \| 21,376 | 23,601 \| 14,961 | 3,736 \| 2,917 |

Before this pass the same table took 18,939 (32) and 18,200 (128)
UPDATEs a second on this box, and 15,492 / 14,800 INSERTs.

Four tables (sysbench's default shape, tps):

| sessions | point_select | write_only | read_write |
|---|---|---|---|
| 8 | 98,814 \| 56,567 | 3,961 \| 3,849 | 1,318 \| 1,176 |
| 32 | 98,758 \| 69,933 | 6,157 \| 4,755 | 1,865 \| 1,341 |
| 128 | 79,958 \| 74,451 | 5,433 \| 4,406 | 1,748 \| 1,477 |

What changed:
- **Row-write mode.** A table's lock has a third mode beside shared and
  exclusive: row writers, any number together, none beside a reader or an
  exclusive writer (and no new one joins while a reader sleeps or an
  exclusive writer waits, so neither starves). An UPDATE of one row by its
  whole primary key, setting constants or `col = col +/- k`, no key, unique
  or shared-dictionary column, on a table with no trigger, watcher or
  foreign key, takes it so. It claims the row, works out and checks every
  value before writing anything, then latches only the columns it sets
  (16 latches a table, taken in order), writes the cells and appends its
  redo record before the latches go - two writes of one column reach the
  log in the order they were applied. A statement that must take back row
  changes after a conflict leaves the group and takes the table exclusive.
  `AIOS_ROW_WRITES=0` turns it off. At 128 sessions on one table it is
  +15-20% on its own (A/B, same build); the rest of the gain is below.
- **The redo log: buffered under the lock, written direct, into written
  blocks.** A record is appended to a buffer (numbered under the buffer's
  lock) and written to the file by the commit that syncs - the write system
  call was a seventh of a row UPDATE's table hold. The file is written with
  O_DIRECT as whole 4 KB blocks (the last partial block kept in memory),
  into a log the checkpoint thread wrote out as zeros ahead of the rotation
  that uses it: on a preallocated log every sync also converted an extent,
  and a small cached write into a block no longer cached read it first.
  Median sync 251-301 us -> 135 us. A commit that finds a sync running and
  nobody else waiting writes its record at once, so the next sync only
  flushes (2 sessions: 3,522 -> 4,430).
- **One waiter runs the next sync.** After a sync, every waiter its records
  had not covered was woken to race for running the next; now only the
  latest is, and its sync covers the others.
- **The table lock sleeps on futex words, without a mutex.** Unlockers of a
  busy table queued on the lock's mutex behind the waiters they had just
  woken: 5,000 stalls a second at 128 sessions.
- **UPDATE / DELETE by key grow a big table's key map** a chunk at a time,
  as INSERT does: without it each found its row through the general
  selection, a sixth of the hold. **INSERT takes its table after parsing
  its rows** (UPDATE and DELETE already did): 128 sessions 14.3k -> 21k.
  Claiming a row reads its key columns, not the whole row.
- **A one-row DELETE keeps the zone maps of the segments after it**, as
  bounds that still hold, widened exactly by the one row that moved in.
  They were dropped, and maintenance restores 16 segments a checkpoint, so
  under a write mix range queries soon scanned most of their table:
  read_write at 128 fell from ~1,800 to ~1,300 a second until a restart.
- Dictionary interning and the number readings of a column's entries:
  each entry decoded once, not three times, under a per-column lock (it
  was one lock for every column of every table), and a probe of the value
  index decodes an entry only when a stored hash byte matches.

Checked: 32 sessions doing `n = n + 1` and constant writes to 10 rows, in
and out of transactions, two readers summing alongside - exact totals, no
torn read - and the same with the server killed (-9) mid-run: the replay is
consistent and holds every acknowledged write. 2,101 range, equality, NULL
and MIN/MAX queries after 3,000 scattered deletes give MariaDB's answers,
and `AIOS_ZONE_VERIFY` reports nothing.

Still behind, and why:
- **One table at 8 sessions, 10-13%.** At 1 session the two are even
  (2,759 | 2,770 UPDATEs); MariaDB pulls ahead from 2 sessions up. Both now
  write the log the same way (4 KB O_DIRECT, then fdatasync, ~110 + 130 us)
  and ASQL uses less CPU per transaction (127 vs 142 us); what is left is
  how many sync cycles fit in a second - ours lose ~80 us between one sync
  and the next to the next leader's wakeup. Tried and not kept: a waiter
  spinning to start the next sync at once (smaller batches, a core spent:
  -35%), a leader running the next sync itself, a log-writer thread, and
  the group-commit wait for autocommit writes - none better than noise.
- **DELETE and INSERT still take their table exclusive.** A DELETE moves
  every later row of every column down one place (~800 KB on a 100k-row
  sysbench table, ~100 us under the lock); concurrent row writers would
  need tombstones or chunked columns that every reader honours. An INSERT
  appends to every column and the key map, which every row writer reads.

# ASQL against MariaDB 10.11, eighth pass: readers past writers, 2026-10-18

The one shape the seventh pass left behind - sysbench read_write at 128-256
sessions - is now ahead, and single-row writes on big tables are two orders
of magnitude faster. Same box and settings as the seventh pass.

| sessions | read_write ASQL | MariaDB | write_only ASQL | MariaDB |
|---|---|---|---|---|
| 8 | 1,888 | 1,514 | 4,775 | 4,909 |
| 32 | 2,287 | 1,627 | 6,987 | 5,586 |
| 128 | 2,177 | 1,783 | 6,800 | 6,410 |
| 256 | 1,871 | 1,545 | 6,185 | 3,194 |
| 512 | 1,590 | fails (max_prepared_stmt_count) | 6,071 | 1,527 |

point_select and read_only stay ahead from 32 sessions up (point_select at 8
is within 4%: 137,902 | 143,712).

Single-row autocommit DELETE / INSERT by key (pymysql, 200 each, includes
the commit's fsync):

| | ASQL before | ASQL now | MariaDB |
|---|---|---|---|
| appdb.inventory_ledger, 1.5M rows | 33,081 / 23,985 us | 414 / 431 us | 16,074 / 7,725 us |
| sbtest1, 100k rows | 581 / 454 us | 275 / 292 us | 345 / 335 us |

What changed:
- **Readers go in past a waiting writer for a grace period.** The table lock
  was phase-fair: once a writer waited, new readers queued behind it while
  the table was still held shared, waiting out the readers ahead and then the
  write. Readers now go in for the first `AIOS_LOCK_READER_GRACE_US` (5,000)
  of a writer's wait; past it new readers wait, so a writer is never starved.
  read_write 256: 1,358 -> 1,934; p95 363 -> 253 ms; write_only unchanged.
- **A big table's key index survives a DELETE.** The map is built a chunk per
  insert; a DELETE refused any map short of the table's last row, walked it
  and dropped it, and the next INSERT built it again (1.5M keys each time).
- **A write parses before taking its table.** The table-scoped write took its
  table exclusive as it resolved the name - before the SET and WHERE were
  parsed. It now takes it at table_write_lock, after the parse.
- **An UPDATE's undo entry holds the key and the changed cell**, not the
  whole row (two long coded strings decoded per sysbench UPDATE).
- **Checkpoints LZ4-pack the tables being written** and deflate a table once
  it has been still for two minutes (deflating every checkpoint was 38% of
  the CPU under write load).
- A scan narrowed by zone maps asks the pool for parallel lanes by the rows
  it will read, not the rest of the table.

# ASQL against MariaDB 10.11, seventh pass: connections and compression, 2026-10-18

First both made better, then compared: many connections, the compressed
client protocol, and storage size. Same box as before (4 cores, 15 GB, one
NVMe), MariaDB 10.11.14 with its packaged settings (thread per connection,
`innodb_flush_log_at_trx_commit=1`, 2 GB buffer pool), ASQL with
`AIOS_TXN_CONCURRENT=1`. sysbench 1.0.20, 4 tables x 100k rows, 15 s runs,
prepared statements, client on the same 4 cores; ASQL's result cache off
(`AIOS_SQL_CACHE_MB=0`) for the engine numbers.

## Many connections (tps; ASQL | MariaDB)

| sessions | point_select | read_only | write_only | read_write |
|---|---|---|---|---|
| 8 | 141,921 \| 143,712 | 6,071 \| 4,871 | 5,212 \| 4,909 | 1,817 \| 1,514 |
| 32 | 171,016 \| 128,013 | 5,834 \| 4,594 | 6,971 \| 5,586 | 1,933 \| 1,627 |
| 128 | 118,414 \| 104,250 | 4,359 \| 3,629 | 6,236 \| 6,410 | 1,555 \| 1,783 |
| 256 | 103,179 \| 96,160 | 3,752 \| 3,074 | 5,989 \| 3,194 | 1,359 \| 1,545 |
| 512 | 85,557 \| 82,243 | 3,131 \| 2,764 | - \| 1,527 | - \| fails (max_prepared_stmt_count) |

2,000 idle connections (pymysql), then a point query on a fresh one:

| | server CPU while idle | memory added | point query p50 / p99 |
|---|---|---|---|
| ASQL before | 16% of a core | +147 MB | 78 / 724 us |
| ASQL now | 0.5% | +67 MB | 82 / 484 us |
| MariaDB | 0% | +44 MB | 85 / 1,341 us |

What changed:
- **A table-scoped write takes its table exclusive once.** It resolved the
  table shared, let go and waited again for the exclusive lock: two sleeps
  and two wakeups per row write on a busy table. write_only at 128 sessions
  4,487 -> 6,236, at 256 3,889 -> 5,989.
- **Idle sessions sleep.** The wait for the next request was 100 ms steps (an
  idle session woke ten times a second); it now blocks, as stop() and KILL
  wake it by shutting the socket.
- **An idle session's thread costs a third.** A 32 KB scratch ring was static
  TLS, zeroed in every thread (now allocated at first use), and the stack its
  last statement touched stayed resident (given back after a second idle).
- Measured and not kept: a statement gate (a few statements running, the rest
  queued) made 256 sessions slower everywhere - every hand-off is the wakeup it
  was meant to save.

Still behind: read_write at 128-256 sessions (-13%). Its reads wait for the
written table's exclusive lock, where InnoDB's readers go on with an older
version of the row.

## The compressed protocol

ASQL had none; it now speaks `CLIENT_COMPRESS` (zlib, through libdeflate) and
MySQL 8's `CLIENT_ZSTD_COMPRESSION_ALGORITHM`, as a server and as a client
(`MariaWireSession::set_compression`, for a seed or CDC pull from a remote
master). Checked with the MariaDB client (`--compress`), the MySQL 8.0.46
client (`--compression-algorithms=zlib|zstd`, zstd levels 1-19), sysbench's
prepared statements and aios-mysql-check.

Four whole enterprise benchmark tables (orders, order_items, products,
customers: 68.6 MB of rows) to the mysql client; bytes counted by a TCP
proxy in front of each server, time best of 5 without it:

| | bytes to the client | ratio | time |
|---|---|---|---|
| MariaDB plain | 68,612,479 | | 1.40 s |
| MariaDB zlib | 25,005,239 | 2.74x | 4.25 s |
| MariaDB zstd | (not supported) | | |
| ASQL plain | 68,612,420 | | 1.06 s |
| ASQL zlib (level 6, as MariaDB) | 23,733,346 | 2.89x | 2.16 s |
| ASQL zstd (the client's level, 3) | 24,097,974 | 2.85x | 2.12 s |

The same rows (sorted, the outputs are identical; order_items comes in
insertion order from ASQL until an OPTIMIZE clusters it by its key, as
InnoDB keeps it).

sysbench read_only, 8 sessions, `--mysql-compression`:

| | off | on | bytes per transaction, on |
|---|---|---|---|
| ASQL | 5,751 tps | 2,477 tps | 20.7 KB (51 KB plain) |
| MariaDB | 4,429 tps | 1,490 tps | |

On one box compression only costs (both sides compress and inflate on the
same four cores); it is for a client across a slow link. `AIOS_WIRE_ZLIB_LEVEL`
sets the zlib level (ERP rows: 5.3x at 1, 5.7x at 6, 6.0x at 9 for 4x the CPU).
Also fixed on the way: a statement seen twice was run unstreamed to cache its
answer, and one past 4,096 rows was built as strings and thrown away each
time (1.82 s -> 1.06 s for the four tables above with the cache on).

## Storage

On disk, after a rebuild of every table (InnoDB: `ALTER ... FORCE` in each
format; ASQL: every column re-encoded, the store written):

| | sbtest (4 x 100k) | ERP (43 tables) |
|---|---|---|
| InnoDB DYNAMIC (data + indexes) | 113.3 MB | 602.1 MB |
| InnoDB DYNAMIC, data only | 103.0 MB | 388.6 MB |
| InnoDB `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` | 79.8 MB | 331.5 MB |
| InnoDB `PAGE_COMPRESSED=1` (zlib) | 48.3 MB | 233.7 MB |
| ASQL before this pass | 40.5 MB | 40.3 MB |
| ASQL now | 35.3 MB | 35.4 MB |
| ASQL in memory (columns, all held) | 56.4 MB | 57.8 MB |

ASQL keeps no index on disk (its key and probe indexes are built when it
opens the store), so the InnoDB data-only line is the closer comparison; it
holds every table in memory compressed, where InnoDB's buffer pool holds
pages as they are on disk (and both forms of a compressed page).

What changed:
- **NULL bitmaps only as far as the last NULL.** Every column had a bit a
  row whether it held NULLs or not: 187 KB of each 1.5M-row column.
- **Long unique text by measured size.** It was always LZ4 blocks; now the
  blocks and a symbol-table-coded dictionary are both measured on a sample
  and the smaller is kept (sysbench's c and pad a third smaller; LZ4's output
  is what the store's deflate cannot shrink).
- Merged before this pass (Sean): deflate-packed checkpoint chunks and object
  payloads, exact code widths, and rows clustered by primary key at OPTIMIZE.

Whole store, fully re-encoded: in-memory columns 172 MB -> 121 MB, on disk
83.7 MB -> 74.0 MB.

# ASQL against MariaDB 10.11, sixth pass: 2026-10-16

The four gaps the fifth pass left open, each closed or measured to its floor.

| shape (ms, median) | MariaDB | ASQL before | ASQL now | |
|---|---|---|---|---|
| correlated `(SELECT MAX(..) .. WHERE b.k = s.k)` per outer row, 20 orders | 0.60 | 0.65 | 0.32-0.38 | 1.6x |
| small `UNION` (distinct) with `ORDER BY` | 0.25-0.30 | 0.33 | 0.17-0.23 | 1.3-1.4x |
| one-row `UPDATE` that changes nothing | 0.082 | - | 0.055 | 1.5x |
| one-row `UPDATE` / `INSERT` / `DELETE` that commits | 0.25-0.37 | - | 0.18-0.33 | 1.0-1.3x |
| commit p99 while a checkpoint rewrites large tables | 3.3-4.0 | 4.4-4.6, with 10-15 ms spikes | 2.8-3.4 | |

What changed:
- **A correlated subquery is answered for the whole page of outer rows at
  once**: the outer keys of the page (4..4096 rows) become an IN list on the
  inner table and one grouped pass answers every row. Grouping uses direct
  ids over the selected rows' key range when it is dense enough, and the
  per-key store is a dense array only when the keys are dense.
- **UNION's ORDER BY / LIMIT run on the union rows themselves** (ordinals or
  labels, ASC/DESC, LIMIT a,b / OFFSET), sorting numbers as numbers only when
  the column is numeric, text with its collation, NULLs first ascending; a
  DECIMAL column is padded to its widest scale, as MariaDB does. The old path
  built a temporary table for this.
- **An UPDATE that changes no row writes no redo and waits for no flush**
  (rows matched but unchanged; with triggers, only when nothing matched).
- **Commits are put ahead of checkpoint I/O**: a commit waiting on its flush
  marks itself, and the checkpoint's paced file writes (1 MB steps) and the
  removal of the old table files (truncated in 4 MB steps rather than
  unlinked at once, which on a `discard` mount stalled the disk for
  10-15 ms) yield to it.
- One-row commits remain bounded by the device flush (~150 us back to back,
  250-300 us after an idle gap on this VM), as MariaDB's are with
  `innodb_flush_log_at_trx_commit=1`.

Found and fixed on the way: **a cached answer survived an INSERT .. SELECT**.
Since the change that stops writes building a cache key, the gateway's
invalidation keyed on "this statement can be cached" and so never ran for a
write; UPDATE and DELETE invalidated again further down, an INSERT .. SELECT
did not, and an IN-subquery count kept its pre-insert value until the next
write. Writes invalidate again; covered by a gaps check through the gateway.
Also a SIGSEGV in `fit_decimal_scales` on a client session (commit 7f18ee6).

24-shape sweep geomean 7.9x, ERP suite 20.5x with 0 mismatches, fuzzer
seeds identical, 112/112 gaps checks.

# ASQL against MariaDB 10.11, fifth pass: 2026-10-16

Same box and method (4 vCPU, MariaDB 10.11.14, interleaved runs, results
diffed against MariaDB's). The shapes that were close to MariaDB, or behind
it, in the 24-shape sweep were taken one at a time: each was traced, the
statement-level waste removed, and the result diffed against MariaDB on the
data, on NULLs and collation ties, and after updates.

| shape (ms; "before" as measured at the start of this pass) | MariaDB | ASQL before | ASQL now | |
|---|---|---|---|---|
| `COUNT(*) WHERE qtyinvoiced < quantity` (column vs column, 1.2M lines) | 188 | 95-200 | 4.7 | 40x |
| `SUM(CASE WHEN status = 0 THEN 1 ELSE 0 END)` x2 over 400k | 77 | 24-45 | 4.8 | 16x |
| `GROUP BY SUBSTRING(stockid, 1, 6)` over 30k items | 12.7 | 13-17 | 0.95 | 13x |
| join GROUP BY .. ORDER BY SUM(..) DESC LIMIT 10 (fortnight of orders x lines) | 140 | 130-145 | 16.6 | 8.5x |
| join ORDER BY orddate DESC .. LIMIT 20 (40k orders x customers) | 57 | 25 | 6.4 | 9.5x |
| `ROW_NUMBER() OVER (PARTITION BY debtorno ...)`, first 30 of 160k | 67 | 31-54 | 9.3 | 7x |
| `CONCAT(name, ' ', currcode) LIKE '%..%'` over 20k | 6.4 | 7-8 | 1.3 | 5x |
| `FROM (SELECT .. GROUP BY ..) x WHERE x.n > 1 ORDER BY .. LIMIT 10` | 3.9 | 10-13 | 0.95 | 4x |
| `INSERT IGNORE` of 5 keys that all exist | 0.2 | 0.41 | 0.09 | 2x |

24-shape sweep geomean: 5.1x (was 3.5x). ERP suite: 14.8x, 0 mismatches.
The point lookups and the one-row statements stay at 1.0-1.5x: both servers
answer them in about a round trip (60 us on this box). A single-row commit
is fsync-bound on both; when a checkpoint is rewriting large table files, a
commit's fdatasync waits behind that I/O on the shared disk (seen as 6-9 ms
inserts in one sweep run straight after a fuzzer had rewritten the four
27 MB sysbench tables).

What changed:
- **Column-vs-column comparisons are a vectorized leaf**: integers (and
  decimals of one scale) compare as integers, anything with a DOUBLE as
  doubles, NULL on either side unknown.
- **Per-code values are kept across statements** for any deterministic
  expression of one dictionary column (`SUBSTRING(x, 1, 6)`, `LOWER(x)`,
  `COALESCE(x, '-')`), keyed by the column and the expression's structure,
  together with their collation classes and ranks. Text ranges (`BETWEEN`,
  `<`, `>`) on a dictionary column are two binary searches over the kept
  ranks instead of a string comparison per dictionary entry.
- **SUM/AVG of an expression read per code** (a CASE over a status) adds a
  precomputed number and exact scaled integer per row, in parallel when all
  the values are whole numbers.
- **Sorting on numbers**: grouped output, join top-N (grouped or not) and
  windows sort on typed values, dictionary collation ranks or a date's rank
  in time, with a counting or radix sort where the keys are dense, instead of
  on copied strings with case-insensitive compares. Only the rows kept are
  projected.
- **Joins start from the filtered side**: `big JOIN small ON .. WHERE <only
  small's columns>` starts from small when its filter keeps under 1/16 of
  big's rows (counted first). big's rows then come off its key index (a
  galloping walk over the sorted keys) rather than a full read.
- **Derived tables take the outer clauses**: the outer WHERE becomes the
  inner GROUP BY's HAVING (on the numbers, per group), and ORDER BY / LIMIT
  go in with it, with the inner GROUP BY keys as tie-breakers so ties come
  out as before.
- **CONCAT(text columns, literals) LIKE** is matched in one buffer per row
  ('%lit%' as a case-folded substring search).
- **An INSERT that inserted nothing** waits for no fsync (unless triggers
  exist, since a BEFORE INSERT trigger fires for an ignored row).
- **Bulk INSERT** of 64k+ rows re-encodes its integer columns at once, so a
  range query right after a 400k-row INSERT .. SELECT takes 0.2 ms, not 110.
- **CREATE TABLE .. LIKE** and **CREATE TABLE .. [AS] SELECT**.
- **Correlated subquery batches are kept across statements** while both
  tables are unchanged: a per-customer COUNT report over another range of
  customers went from 23 ms to 2-3 ms after its first run (MariaDB 67 ms).
- **A scalar subquery returning several rows is error 1242** (MariaDB's
  "Subquery returns more than 1 row"); ASQL used to return the first row.

Found and fixed on the way: a join sort key resolved in a one-table scope
was bound to that scope and then read from the wrong table (wrong order on
a fresh server for `ORDER BY s.tag DESC, b.id LIMIT 3`); covered by gaps
checks that start from an empty database.

Still not ahead after this pass: a correlated MAX per outer row over 20
orders (0.9x at 0.6 ms), small UNIONs (0.9x at 0.3 ms), and one-row
UPDATE/INSERT commits (fsync-bound on both). All three were closed in the
sixth pass above.

# ASQL against MariaDB 10.11, fourth pass: 2026-10-16

Same box and method as the third pass below (4 vCPU, MariaDB 10.11.14,
interleaved runs, every ERP result diffed against MariaDB's). This pass
changed how reads run, not what they return.

## ERP-shaped queries: 6.4x -> 13.4x geometric mean, 0 mismatches

| query | MariaDB ms | ASQL ms | |
|---|---|---|---|
| open orders 3-way join | 1,820 | 47 | 39x (was 9.4x) |
| distinct customers 365d | 524 | 12.7 | 41x |
| order value range (SUM(price*qty)) | 184 | 3.8 | 48x (was 3.0x) |
| stock by line (join) | 494 | 14 | 34x (was 3.4x) |
| top customers 90d (join) | 964 | 34 | 28x (was 4.9x) |
| monthly totals (GROUP BY YEAR, MONTH) | 528 | 19 | 27x (was 12x) |
| line sales by location (join) | 286 | 10.5 | 27x (was 5.8x) |
| inventory top items (GROUP BY .. LIMIT) | 412 | 18 | 22x (was 7.5x) |
| orders w/o shipment (NOT EXISTS) | 144 | 6.6 | 22x (was 3.1x) |
| inventory ledger 30d / 7d line sales (IN subquery) | 199 / 63 | 10.9 / 10.5 | 18x / 6x |
| order holds by document | 358 | 22.5 | 16x (was 3.9x) |
| busy items (HAVING) | 185 | 17.9 | 10x |
| category stock (LEFT JOIN) | 60 | 8.7 | 7x |
| point lookups (order by pk, item history) | 0.3-0.4 | 0.3-0.4 | 1.0-1.1x cold, 1.6-2.2x steady (79 vs 124 us, 70 vs 153 us) |

What changed:
- **Arithmetic inside aggregates runs column-wise.** `SUM(unitprice * quantity)`
  made a value per cell (the DOUBLE printed, then strtod'd back); now the
  expression is compiled to column kernels and summed in row order (same
  doubles, same bits). 555 -> 8 ms over 1.2M rows.
- **Joins on a unique key are lookups, not row-by-row probes.** The joined
  side's key map is built once (dictionary-to-dictionary, cached until either
  dictionary changes; an array for small integer ranges), uniqueness is
  checked while building it, and GROUP BY + aggregates read the per-table row
  arrays directly - no joined row is materialized. A first join that is
  one-to-many (orders -> lines) starts from the many side and the row arrays
  are sorted back into FROM order, so groups, ties and float sums are
  unchanged. A group key that is a table's unique key groups by that row.
- **Joins on an expression key** (`ON UPPER(a) = b`) hash it instead of
  running the ON for every pair (300k x 30k: past ten minutes before, 0.2 s
  now; MariaDB 0.15 s). A key compared with a constant no longer drives the
  probe.
- **GROUP BY** with one small-range key (or several derived from one
  dictionary column) indexes its groups directly; ORDER BY .. LIMIT over many
  groups works out sort keys first and projects only the groups kept.
- **NOT EXISTS / IN (subquery)** on an integer column is a bitmap read
  straight off the inner selection; a dictionary column IN a subquery is
  decided once per code; IS [NOT] NULL reads the NULL bitmap.

## sysbench (4 tables x 100k), ratio to MariaDB

| test | 1 | 8 | 32 | server CPU per query/txn (8 sessions) |
|---|---|---|---|---|
| point_select | 1.9x | 3.2x | 3.3x | 12 us vs 44 us |
| read_only | 2.0x | 2.9x | 2.5x | 249 us vs 925 us |
| write_only | 1.3x | 1.05x | 1.0x | 480 vs 474 us |
| read_write | 1.4x | 1.1x | | 1,480 vs 1,805 us |

The throughput ratios at 8+ sessions are capped by the sysbench client
sharing the 4 vCPUs (it uses ~2 cores against ASQL); per query, ASQL's
server spends a quarter of MariaDB's CPU. A single session's point select
is mostly the kernel's sleep and wake on a virtual machine: a connection now
spins ~30 us for its next request while cores are idle (16.5k -> 24k per
session), and stops spinning once more connections are open than half the
cores.

Writes are level because a write transaction is six round trips and a
commit: per transaction the server spends ~60% of its time in the kernel
(socket wake-ups, futexes, the commit's fdatasync), the same work MariaDB
does. What the engine itself adds is small; the largest piece left is
DELETE shifting every later row of each column (5% of write_only).

## Also in this pass

- `KILL [QUERY|CONNECTION] n` and COM_PROCESS_KILL work: a running read stops
  (1317 Query execution was interrupted) and the session's socket is shut for
  CONNECTION. Before, KILL queued behind the statement it meant to stop.
- `SELECT .. FROM DUAL`.
- `NULL NOT IN (empty subquery)` is TRUE, as in MariaDB.
- A join through an expression of the joined table (`ON UPPER(sm.stockid) =
  ls.stockid`) returned no rows when evaluated in one-table scope first: an
  expression compiled for one table is now always a copy.

# ASQL against MariaDB 10.11, third pass: 2026-10-15/16

Box: 4 vCPU, 15 GB, Ubuntu 24.04, MariaDB 10.11.14 (innodb_buffer_pool 2G,
flush_log_at_trx_commit=1, no query cache), same machine, one at a time,
interleaved run by run (this box drifts by up to 20% over an hour; the ratios
below are from back-to-back runs). ASQL with AIOS_TXN_CONCURRENT=1, as shipped
(result cache on) unless noted.

## Enterprise workload queries (the "22x" class)

1.5M inventory ledger, 400k orders / 1.2M order_items, 300k inventory,
250k shipments and order holds, 30k products, 20k customers; the
benchmark suite's shapes plus joins, NOT EXISTS, LEFT JOIN, HAVING.
Median of 5, `SQL_NO_CACHE` on ASQL, every result diffed against MariaDB's
(0 mismatches):

| query | MariaDB ms | ASQL ms | |
|---|---|---|---|
| inventory ledger 30d line sales (IN subquery) | 419 | 16 | 26x |
| line sales by location (join) | 3,357 | 51 | 66x |
| distinct customers 365d | 403 | 8.8 | 46x |
| top customers 90d (join) | 3,384 | 184 | 18x |
| open orders 3-way join | 1,563 | 157 | 10x |
| monthly totals (GROUP BY YEAR, MONTH) | 389 | 37 | 10x (was 1.5x) |
| orders without shipment (NOT EXISTS) | 89 | 39 | 2.3x (was 0.8x) |
| category stock (LEFT JOIN ... ON ... AND) | 39 | 15 | 2.6x (was 0.3x) |
| item history / order by key | 0.4-0.6 | 0.2-0.5 | 1.3-1.7x |

Geometric mean over the 20 queries: 5.5x before this pass (two slower than
MariaDB), 6.4x after (none slower). What changed:
- a join's ON conditions on the joined table select its rows first, and the
  key index is built over just those (LEFT JOIN 131 -> 13 ms);
- [NOT] EXISTS correlated by one equality runs as [NOT] IN, NULL-exact
  (was one subquery per outer row);
- IN sets are flat open-addressing tables; an IN subquery over a typed
  integer column reads its values off the vectorized selection;
- DOUBLE columns filled by INSERT get the scaled encoding at checkpoint
  (they were dictionary text, read through strtod per row).

## sysbench, ratio to MariaDB (4 tables x 100k rows, 10 s runs)

| test | 1 | 4 | 8 | 16-32 | 64 |
|---|---|---|---|---|---|
| point_select | 1.35x | 2.35x | 2.4-3.7x | 2.0-3.0x | 2.1-2.8x |
| read_only | 1.67x | 2.51x | 2.06-2.6x | 2.1-2.2x | 2.2-2.4x |
| read_write | 1.05x | 1.02x | 1.08-1.14x | 1.05-1.15x | 0.83-0.90x |
| write_only | 0.97-1.08x | 0.98-1.01x | 0.91-0.97x | 1.0x | 0.94-1.06x |

Before this pass write_only and read_write at 4+ sessions failed outright
(1062: concurrent DELETE + INSERT of one key), and read_write at 64 sessions
was 0.71x.

Writes are level, not ahead: per write transaction both servers spend about
the same CPU (ASQL user 155-170 us + kernel 165-185 us, MariaDB 172 + 168),
half of it the kernel's per-statement socket work and the commit's fdatasync
(both ~350 us of disk path per commit on this box). The engine's own share of
a write transaction is 55-60 us in-process (130 before this pass); what is
left over the wire is the wire and gateway layers re-reading each prepared
statement's text (tokenize, parse, checks) - the next thing to take on.

What changed for writes and concurrency:
- concurrent writers wait on a key another transaction holds (a DELETE that
  found nothing, an INSERT meeting another's row) instead of failing;
- group commit with no mutex for waiters (atomics, a sync epoch);
- phase-fair table locks with separate reader/writer queues and a short spin;
- UPDATE / DELETE take their table exclusive once parsed;
- DELETE in a bit-packed column shifts 64-bit words; RowIndex::virt_of is
  one binary search;
- the result cache skips answers reading a table written in the last 50 ms
  (it stored and dropped them: 10-20% of read_write);
- writes skip the cache probe, the result-view lock and the matview lock.

## Correctness found and fixed on the way

- DELETE / UPDATE by key could silently miss its row after a multi-row
  DELETE (the key map's gap ids were computed against a half-updated list).
- UPDATE did not enforce PRIMARY KEY or UNIQUE (duplicate keys possible).
- A failed multi-row INSERT kept the rows before the failing one; a failed
  statement is now taken back whole and never written to redo.
- Foreign keys enforced (were parsed only); `CONSTRAINT x FOREIGN KEY` and
  `CONSTRAINT pk PRIMARY KEY` no longer lose their kind; composite keys are one
  constraint.
- TLS on the wire; `cdc_tables=t1,t2` read as a list (a bare list was silently
  ignored and every table mirrored).

All verified by a randomized DML differential against MariaDB (point/range
deletes, inserts, key updates, failing statements, partly failed and rolled
back transactions, a checkpoint mid-run), concurrent-transfer invariants at 16
sessions, kill -9 recovery, a 50-statement foreign-key battery, and the
repo's check binaries and join cases.

# ASQL, aios-git and aios ssh against MariaDB and stock git: 2026-10-14/15

Box: 4 vCPU, 15 GB, Ubuntu 24.04, MariaDB 10.11.14 (innodb_buffer_pool 2G,
flush_log_at_trx_commit=1, no query cache) and ASQL from this checkout, same
machine, one at a time. sysbench 1.0.20, 4 tables x 100k rows, uniform
random, 10 s runs, prepared statements (sysbench's default). ASQL run with
AIOS_TXN_CONCURRENT=1 and the result cache OFF (AIOS_SQL_CACHE_MB=0), so
every statement is executed, as MariaDB executes every statement. With
the cache ON (the default) point_select at 4 threads is 140k a second on
a cold cache and 254k once the answers are in, read_only 4 threads 7,346;
cold, the cache used to cost half the engine's speed (78k) because every
miss's store took the one cache lock - it is 16 shards now. The two
matrices below were taken back to back after a reboot of the box; the
absolute numbers are lower than earlier in the session (the box runs about a
fifth slower since), the comparison is like for like.

## sysbench, transactions per second (ASQL start of session -> ASQL now | MariaDB)

Three matrices taken back to back on 2026-10-15 (10 s runs): ASQL with the
result cache off (the engine path), MariaDB, and ASQL as shipped (cache on;
a 10 s run of random point lookups is mostly a cold cache - a second run
is 180-250k).

| test | threads | ASQL before | ASQL, cache off | MariaDB | ASQL, cache on |
|---|---|---|---|---|---|
| point_select | 1 | failed (prepare bug) | 17,346 | 15,095 | 16,768 |
| point_select | 4 | failed | 138,325 | 136,680 | 107,885 (cold) |
| point_select | 8 | failed | 130,192 | 102,427 | 110,265 (cold) |
| read_only | 1 | 47 | 838 | 606 | 886 |
| read_only | 4 | ~50 | 6,080 | 4,237 | 6,087 |
| read_only | 8 | ~50 | 4,789 | 4,042 | 5,021 |
| write_only | 1 | 79 | 1,044 | 1,287 | 881 |
| write_only | 4 | failed (dup key) | 3,298 | 3,845 | 2,945 |
| write_only | 8 | failed | 4,364 | 5,368 | 4,139 |
| read_write | 1 | 8 | 405 | 393 | 428 |
| read_write | 4 | 24 | 1,185 | 1,240 | 1,177 |
| read_write | 8 | ~30 | 1,655 | 1,648 | 1,561 |

Reads are ahead at every thread count (read_only by 20-45%), read_write
is level, write_only is behind by 14-19% (see "still behind" below).
Earlier in the session, on the faster box state before a reboot:
point_select 18,632 / 168,140 / 121,587 against 16,202 / 134,982 /
112,595; read_only 858 / 6,437 / 5,245 against 722 / 4,178 / 3,680.

With `AIOS_REDO_SYNC=0` (MariaDB's flush_log_at_trx_commit=2; not what
the table compares) write_only is 1,924 / 3,738 / 4,450 and read_write 8
threads 1,982.

"Before" = the first run after fixing COM_STMT_PREPARE so sysbench could run at all.

Server CPU per query (from /proc, the sysbench run divided out), result
cache off, measured earlier in the session on the faster box state: point
select 4 threads ASQL 13.6 us / MariaDB 17.7-18.5; read_only 4 threads
ASQL 26.3 us / MariaDB 32.3; read_only 1 thread ASQL 47.1 / MariaDB 54.5;
write_only 1 thread ASQL 80 us per statement.

## per statement, single session (microseconds, p50 / p90)

| statement | ASQL before | ASQL now | MariaDB |
|---|---|---|---|
| BEGIN | 89 | 45 / 82 | 39 / 68 |
| SELECT c WHERE id=? | 78 | 48 / 67 | 55 / 83 |
| SELECT c WHERE id BETWEEN ? AND ? (100 rows) | 5,197 | 114 / 144 | 128 / 181 |
| SELECT SUM(k) over that range | 7,707 | 84 / 98 | 96 / 131 |
| ... ORDER BY c | 4,696 | 154 / 177 | 197 / 255 |
| SELECT DISTINCT c ... ORDER BY c | 256 (60,000 inside a write txn) | 161 / 198 | 283 / 392 |
| UPDATE k=k+1 WHERE id=? | 1,411 | 78 / 101 | 104 / 171 |
| UPDATE c=? WHERE id=? | 448 | 70 / 86 | 63 / 91 |
| DELETE WHERE id=? | 7,321 | 106 / 153 | 61 / 75 |
| INSERT | n/a | 77 / 96 | 59 / 74 |
| COMMIT (write txn, fdatasync) | n/a | 496 / 760 | 437 / 638 |
| worst UPDATE latency over 6000 | 537 ms (checkpoint stall; now 60-125 ms, the snapshot copy) | <1 ms | 23 ms |

The medium's own fdatasync on this box: 113-150 us p50.

## second pass: reads past MariaDB, writes closer (what was done)

- Statement-shape cache: a plain SELECT's parsed tree is kept per thread
  under its shape (the tokens with every literal a placeholder); the next
  statement of that shape has its literals written into the tree's literal
  nodes - each remembers the token it came from - instead of being parsed.
  Shapes with a subquery, an ORDER BY ordinal, a CAST width or a column
  headed by a literal are parsed every time. `AIOS_SQL_SHAPES=0` turns it off.
- Expression nodes no longer carry two heap mutexes each (only subquery
  nodes do); the parser's allocations were a quarter of a point lookup.
- Gateway: the engine's own statement forms are read off the tokens (five
  searches of an upper-cased copy before), FOR UPDATE found in one pass,
  the verb compared as a view, the token vector pooled per thread and handed
  down the layers (gateway -> local executor -> the INSERT/UPDATE/DELETE
  parse) instead of the text being tokenized three times; BEGIN / COMMIT go
  straight to the transaction code.
- Collation keys: ASCII decided eight bytes at a time, then a branch-free
  loop the compiler vectorizes; ORDER BY one text column sorts 12-byte
  references into one key arena with memcmp (a key string per row compared
  character by character again was a tenth of the read-only mix's CPU).
- DISTINCT streams like any other single-table SELECT (duplicates found on
  the cells in the columns, hashed), rows stream from the columns to the
  packet with no row object; selection hit lists pooled per thread.
- Column index: the values kept beside the rows (a lookup's binary searches
  read those, not packed bits); rows appended since the build kept in a
  sorted tail; deleted rows become gaps (folded in once there are many)
  instead of every entry being shifted; a new row's id found by binary
  search over the gaps, not a walk.
- DELETE of one row: the rows after it move as one block, the NULL flags
  shift eight bytes at a time; no keep-flag vector, no byte walk.
- Redo log: the descriptor is kept open (an open, write, close per
  statement and an open, fdatasync, close per sync before); a transaction's
  records go out in one append at COMMIT; the sync thread no longer races
  the committing statement (two fdatasyncs per COMMIT, 470 us, where the
  medium takes 150).
- Sockets: plain blocking recv/send by default (io_uring with
  `AIOS_NET_URING=1`); one recv per wait where a non-blocking try, a poll
  and the recv were three system calls.
- Wire: cell bytes appended with one memmove, no zero fill.
- `AIOS_SQL_CACHE_MB=0` turns the result cache off (engine benchmarks).
- Fixed on the way: an embedded caller's second thread could deadlock on
  BEGIN (the gaps check's own test, timing permitting): the writer lock is
  taken before the statement lock there too, as the gateway does.
- Table-scoped writes: a single-table INSERT / UPDATE / DELETE with no
  subquery, join, trigger or watcher locks only its table (exclusive) under
  the shared statement lock; readers take the tables they resolve shared.
  Eight sessions of write_only: 1,418 -> 3,503 tps. `AIOS_TABLE_LOCKS=0`
  turns it off.
- Dictionary compaction: a text column's dictionary kept every value ever
  written (an UPDATE interns the new value, the old entry stayed): a 100k-
  row table updated 250k times was 220 MB in memory and on disk. Entries no
  row uses go at maintenance once a third are dead: 49 MB.
- Checkpoint cadence: a checkpoint rewrites every changed table whole;
  by statement count it fired every few seconds under load. Now no more
  often than `AIOS_CHECKPOINT_SECS` (30) that way, by redo bytes any time.
- Checkpoint off the lock: the lock covers a copy of the changed columns
  and a rename of the redo log; a thread writes the files (500-700 ms)
  while statements run. The copy takes the appended tail of each
  dictionary arena into the last checkpoint's copy (the arenas are 80% of
  the bytes and only grow between compactions): 12-14 ms under the lock
  per checkpoint for the four sysbench tables, 60-90 ms when copied whole.
  The write under the lock was the 216-610 ms worst write latency; it is
  75-200 ms now, a single COMMIT behind the file write every 30 s, and
  the per-second p99 in a checkpoint second is 5-16 ms. Killed
  inside the write, the server restarts from the old manifest plus the
  rotated log (redo.g<N>.prev); the checked case recovers every
  acknowledged row.
- Maintenance before the snapshot, also under the lock: the zone-map
  refresh of delete-touched segments is budgeted at 8 ms a checkpoint
  (was 130-150 ms), a dictionary compaction keeps the zone maps and
  copies the arena directly (6-15 ms a column, was 50 plus a 30 ms zone
  map rebuild).
- `AIOS_REDO_SYNC=0`: a statement returns once its record is in the log
  file, a thread syncs every 200 ms (MariaDB's flush_log_at_trx_commit=2).

Verified by driving: the shipped check binaries (sqlfeatures, enhancements,
gaps, auth, mysql wire, git merge) on every build; seven sessions writing
the same and different tables at once with readers and SHOW statements
beside them, row sums and counts landing exactly where they must; 82 statement shapes with
changing literals answer identically with the shape cache on and off;
prepared (binary) and text protocol agree; DISTINCT agrees across streamed,
built and cached passes; the key index and a full scan agree after sysbench
write runs (deletes and inserts); kill -9 after single statements, a
committed and a rolled-back transaction replays identically.

## what was wrong, and what was done (aios/)

- COM_STMT_PREPARE announced no result columns, so libmysqlclient/sysbench
  never read the result set and the connection went out of sync. The prepare
  reply now carries the column definitions (from a LIMIT 0 probe, cached per
  statement until the next DDL). mysql_wire.cpp.
- INT columns filled by INSERT stayed dictionary-coded text forever; every
  numeric compare went through strtod per row. A checkpoint now gives them a
  typed encoding once (maintain_storage).
- Ranges on a typed key column read the value-ordered column index (two binary
  searches) instead of scanning; BETWEEN/</>/<=/>= leaves, range_cmp.
- DELETE: every row after the deleted one was moved one code at a time, the
  NULL bitmap bit by bit, the whole primary-key map walked to renumber it, the
  column index walked with a search per entry. Now: block memmove of the
  survivors (narrow widths widened once), a byte-wise bitmap shift, a gap list
  in the key index (RowIndex::gaps, row_of/virt_of) so nothing is renumbered,
  a one-pass index shift.
- Zone maps: a cell update or an append into a segment a delete had unsettled
  recomputed all 2048 rows of every column. Left for checkpoint maintenance.
- Redo log: one open+write+fdatasync+close per statement, under the writer
  lock. Now an unsynced append under the lock and a group sync after it: the
  first waiter syncs, the others piggyback; inside a transaction only COMMIT
  waits. Checkpoint every 20,000 statements instead of 2,000 (its table
  rewrite ran under the writer lock: 180-540 ms stalls every half second).
- Text columns oscillated between arena and dictionary encodings on every
  write/checkpoint pair (a 100k-row re-encode each way); a column recoded by
  writes twice stays dictionary-coded.
- SELECT DISTINCT went to the join path whose probe index every write
  invalidated: 60 ms per call inside a write transaction. The vectorized path
  now handles plain DISTINCT.
- COMMIT emptied the whole result cache, and clearing an unordered_map
  zeroes its bucket array: 84% of server CPU under a transaction-per-row
  load. Transaction boundaries no longer invalidate (the statements inside
  already did), swaps release the tables, evicted results are freed off-lock.
- Statement lock: readers now take/release it with one atomic op (mutex only
  when a writer is about). In concurrent mode BEGIN and COMMIT share the lock.
- Column index: rows appended since its build were all candidates, sorted on
  every lookup; now filtered by the leaf's own predicate, and the index is
  rebuilt once the tail passes 512 rows / 1/64.
- Linux build no longer hard-requires the OpenCL SDK header (falls back to the
  stub, as Windows does).

## ssh / git

- `aios ssh` with an ECDSA user key: the legacy auth path signed every non-RSA
  key as ed25519. Fixed; ed25519, ECDSA P-256 and RSA all log in to OpenSSH
  9.6, a stranger's key is refused (re-driven on the plain socket path).
- aios-git clone of an empty remote whose HEAD is `main` landed on `master`;
  it now asks ls-refs for `unborn` and stands on the remote's branch
  (clone, commit, push verified against stock git; fsck --strict clean).
- aios-git push: it used to walk the remote's entire history, every tree and
  blob inflated, to decide what to send (16 s over ssh with a buffer bug);
  then every tree of every historical commit behind each remote ref (300
  trees of 3,000 entries for a one-commit push, 340 ms locally where git
  takes 60). Now the commits behind each remote tip plus the tip's own tree.
  One commit pushed: local 55-62 ms (stock 59), over ssh 229 ms (stock 486).
- Checkout: a file's directory made once per checkout (every prefix of every
  file's path was stat'ed and mkdir'ed - 136k mkdirs on a 3,000-file clone),
  working-tree files written without an fsync each, as git does.
- Inflate goes through the system zlib when linked; a kept pack is synced as
  one file instead of syncfs() on the whole filesystem.
- Over ssh, 38 MB / 3,900-object repository into a fresh directory: clone
  1.54-1.56 s against stock 1.53-1.58 s; bare clone 1.29-1.32 s against
  1.36-1.46 s. (Cloning into a directory whose 3,000 files were just deleted
  costs both a second more on this ext4: inode allocation avoids the
  recently freed ones.) Local status/log/diff/add at or ahead of stock;
  commit 170-200 ms vs 145.

## fixed after the first pass

- CDC: a replicated `DATETIME(n)`, `TIMESTAMP(n)` or `TIME(n)` lost its
  fraction (the decoder read past the bytes), and a negative `TIME` with a
  fraction came out one second off. Seeded and streamed rows now diff clean
  against the source, fractions and `-05:06:07.250` included. (The "double
  encoded text" seen in the first audit was the test's own latin1 client
  inserting into MariaDB; the mirror was byte-faithful.)
- `BEGIN` while a transaction is open commits it first, as MariaDB does.
- `AIOS_SQL_STRICT=1`: constraints enforced with MariaDB's messages (off by
  default - the mirrored MariaDB runs with sql_mode empty). `UNSIGNED` and
  `ENUM`/`SET` members are now kept with the column.
- The packaged unit sets `AIOS_TXN_CONCURRENT=1`.

## still behind MariaDB, and why

- Writes at 4+ sessions (write_only 3,056 tps against 4,225 at 4 threads,
  3,987 against 5,497 at 8; read_write 1,188 against 1,280 at 4 and 1,576
  against 1,670 at 8, both servers measured back to back): with
  table-scoped locks the sessions overlap, and the box (4 cores shared
  with the sysbench client) is CPU-bound in both cases; the gap is the
  server's CPU per write statement, 90-105 us against MariaDB's 53-60
  (user 52-62 against 30-36, kernel 37 against 24-27, from /proc during
  the runs). Where it goes: a DELETE moves every row after the deleted one
  up in every column (~800 KB of memmove on a 100k-row table, 16% of the
  server's user time at one session), the parse of a write statement (no
  shape cache for writes: 7%), the undo image of an UPDATE (the whole row
  as strings), the checkpoint's file writes (8 us of kernel time per
  statement at one session) and the redo log's fdatasync per COMMIT.
  A big table's INSERT now checks its key against the row map (built a
  chunk of 16k rows per INSERT, kept through deletes) instead of the
  column index plus a string-keyed map of every row since the index was
  built, which was the largest single piece of an INSERT.
- Single-session write transactions: level now (1,257 against
  1,210-1,287) with the redo log preallocated and written in place, so a
  COMMIT's fdatasync flushes data only; at 4 and 8 sessions 3,391 and
  4,300 against 3,845 and 5,368. With `AIOS_REDO_SYNC=0` 1,924.
- A checkpoint's snapshot still copies each changed table's codes and
  null bitmaps, and the tail of each dictionary, under the exclusive lock
  (12-14 ms every 30 s under sysbench, 8 of them the zone-map refresh
  budget; the files are written outside it).
- Not addressed: foreign keys are never enforced; no TLS or grants on the
  wire.
