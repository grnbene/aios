# ASQL: a smaller footprint without losing speed

Design, 2026-10-28. Follows `asql_memory_reduction.md` (the 2026-10-27 plan) and goes deeper than it.
Measured on the local copy of atl's data (`versioneight`, 808 tables, 10.3M rows, atl's binary aios-829500b6).
Nothing here has been changed yet.

## Where the bytes are

Freshly loaded, the engine counts 581 MB and the process holds 885 MB. On atl, after a day of replication, the
process holds ~2.1 GB and the engine counts ~1.0 GB (see the memory note of 2026-10-27).

| Part | Local, fresh | atl, live | What it is |
|---|---|---|---|
| Table columns | 444 MB | 443 MB | the data, encoded per column |
| of which TEXT dictionaries | 244 MB | — | 71 MB of codes, 176 MB of dictionary strings |
| Primary-key row maps | ~2 MB | 340 MB | `PkMap`, one per table that takes keyed writes (88 tables, 8.8M entries) |
| Column value indexes | 84 MB | 71 MB | `ColIndex`: value → rows, for filters |
| Join probe indexes | 31 MB | 41 MB | hash per join column |
| Allocator slack | ~300 MB | ~1,100 MB | memory glibc holds and does not return |

Three things stand out, and none of them is "compress the columns harder":

1. **The key maps cost 170× more on atl than here, for the same data.**
2. **95 columns hold 32-bit codes for dictionaries of a few values** (36 MB wasted, 22 MB of it in `stockmoves`).
3. **The allocator holds 1.1 GB on atl**, and the two points above are the churn that most likely fragments it.

The column compression the earlier plan proposed is real but small (~60 MB). It is step 3 below, not step 1.

## 1. Key maps: keep the compact form under writes

`PkMap` has two forms (`include/aios/userland/sql_database.hpp:1146`):

- **runs**: integer keys that arrive in order are kept as (first key, first row, count) runs. 8.8M entries fit in
  about 2 MB. This is what a fresh load builds.
- **slots**: an open-addressing hash table, 16 B a slot at ≤70% load, plus for text keys a 4 B offset and the key
  bytes in an arena. About 23 B an entry for integer keys, ~45 B for text ones.

A map leaves runs for slots **for good** (`to_slots`, hpp:1346) when:

- a key arrives out of order (`run_append` → `append_run` fails: `!int_less(last_key, k)`), which for a two-column
  key like `salesorderdetails (orderno, orderlineno)` is **every line added to an existing order**;
- more than a quarter of its keys have been deleted (`erase`, `gone_.size() * 4 > count_ + 256`);
- runs stop paying (`runs_v_.size() * 2 > count_ + 64`), i.e. ids with gaps every other key;
- the key is text (slots from the first entry).

Estimate of atl's maps all in slot form, from the 88 warmed tables' key shapes (local `rowindexes.list`):

| Key shape | Tables | Rows | Slot form |
|---|---|---|---|
| one integer | 43 | 4.66M | ~107 MB |
| several integers | 11 | 1.13M | ~26 MB |
| text | 34 | 3.04M | ~140 MB |
| | | | **~273 MB**, plus rehash slack → the 340 MB atl reports |

So the 340 MB is not the data; it is the form the maps fall into under a day of ordinary writes. Local proof of the
mechanism: `Asql_pk_runs_to_slots` was 53 here before any experiment, 59 after 1M out-of-order pair inserts.
(The counter `Asql_row_index_mem_bytes` cannot be used for this: it is a cached footprint, `sql_database.cpp:24502`,
and read 0 throughout the experiment.)

### Change

Make the compact form survive writes instead of abandoning it:

- **Runs plus an overlay.** Keep the runs; put out-of-order inserts and deletions in a small slot table beside them.
  A lookup checks the overlay (a few thousand entries) then the runs (binary search). Fold the overlay back into the
  runs when it grows past a bound (say 1/16 of the entries) or at a checkpoint. This keeps `salesorderdetails`,
  `debtortranstaxes`, `stockmoves` and the other multi-column tables at a few MB instead of ~25 B a row.
- **Text keys: a sorted key arena with a prefix index** instead of a hash table. The key bytes are already in an
  arena; a sorted arena with one offset per 16 keys (as the dictionary front-coding does, `DictArena::front_code`)
  costs the key bytes plus ~1 B an entry instead of key bytes plus 20 B. Point lookups stay O(log n) on a cache-warm
  structure, which is what MariaDB does with its B-tree anyway. New keys go to the same overlay and fold in.
- **Ints with gaps**: the `runs_v_.size() * 2 > count_` rule gives up at 50% density. A run costs 16 B, a slot
  ~23 B; runs still win down to ~70% of keys being gaps. Lower the bar.

Expected: ~340 MB → ~30–40 MB on atl. Speed: a point lookup becomes an overlay probe plus a binary search over
runs; measure with `tools/asql-erp-bench` (point selects, keyed updates) and the sysbench rows in
`docs/asql-benchmarks.md`. The gate is: no row of that table gets slower by more than a few percent.

## 2. Dictionary codes: do not widen a whole column for one row

How a column gets to 32-bit codes (`ColumnStore::push_back`, `sql_database.cpp:16029`; `set_encoded` failing at
16047):

```
if (encoding != DictBitPacked || bit_width_ != 32) materialize_to_dict_packed();   // the WHOLE column to 32 bits
```

A write that does not fit the column's current width (a value past the dictionary's power of two, or a column that
was RLE/FOR) re-materialises every row at 32 bits. Narrowing back happens only in `maintain_storage()`
(`:22546`), which runs only inside a checkpoint (`flush_impl`, `:22850`), checkpoints run only after writes
(`:22529`), and the narrowing has a **40 ms budget per checkpoint** (`AIOS_STORE_MAINTAIN_MS`, `:22722`) shared with
segment packing, walking an `unordered_map` of 808 tables from the start each time.

Measured here: `stockmoves.userid` (61 values), `.serialno` (2 values), `.loccode` (5 values) and eight more of its
columns are at 4.07–4.20 B a row; packed they are 0.1–0.8 B a row. 95 such columns, 48 MB stored, 12 MB needed.
A fresh `INSERT`-filled table starts wide too (100k rows, 11 values → 4.24 B/row) and stays wide until a checkpoint
reaches it.

On atl the same column goes wide, narrows at some later checkpoint, goes wide again at the next odd write: each
cycle allocates and frees `rows × 4` bytes a column (2.3 MB per `stockmoves` column, 11 columns). That is the
allocator churn behind the slack.

### Change

- **Widen by one bit, not to 32.** When a code does not fit, repack to `code_bits_for(dict.size())`, which is
  usually the current width plus one, in place. That is one pass over `packed_data`, the same cost as the 32-bit
  materialisation, and it never needs narrowing afterwards. `pack_column_codes` and `repack_codes` already do the
  pass.
- **Appends past the end: a wide tail.** For an append-only table (stockmoves, gltrans) keep the last segment of
  rows at 32 bits and the sealed rows packed; the segment headers (`seg_hdr`, `SegFOR`) already give per-segment
  widths. Then an append never touches sealed rows.
- **Narrowing as a fallback, not the fix:** keep `maintain_storage` for what is left, but resume where it stopped
  instead of restarting, so every table is reached.

Expected: ~36 MB back directly, and the end of the widen/narrow churn, which is where I expect most of atl's
1.1 GB of slack to come from. Verify the slack claim before building anything on it (step 0 below).

## 3. Long-text dictionaries: compress them in blocks

Dictionaries are 176 MB of the 244 MB of TEXT storage. Long entries (JSON options, notes, request bodies) are
already symbol-coded (`code_dictionary`, `:16417`), which gets about 2.7×. Block compression of the dictionary gets
far more, measured on the local copy with 64 KB deflate blocks over the distinct values:

| Column | Entries | Plain | Stored now | 64 KB blocks |
|---|---|---|---|---|
| salesorderdetails.po_options | 30,955 | 70.5 MB | 27.0 MB | 4.1 MB |
| ami_activity.request | 151,626 | 28.2 MB | 9.9 MB | 3.6 MB |
| gltrans.narrative | 274,340 | 13.6 MB | 9.2 MB | 3.0 MB |
| binmovements.variables | 71,703 | 21.7 MB | 7.7 MB | 2.5 MB |
| debtortrans.invtext | 253,666 | 5.7 MB | 7.4 MB | 0.9 MB |
| salesorders.payment_details | 109,309 | 28.9 MB | 5.8 MB | 1.1 MB |
| stockmoves.narrative | 84,616 | 1.3 MB | 3.8 MB | 0.3 MB |
| quote_details.po_options | 2,938 | 8.6 MB | 3.7 MB | 0.4 MB |
| dayendposting.postingdata | 1,661 | 7.4 MB | 3.3 MB | 0.6 MB |
| salesorders.notes | 20,177 | 0.8 MB | 1.5 MB | 0.2 MB |
| **ten columns** | | | **79 MB** | **17 MB** |

(`invtext`, `narrative` and `notes` are stored *larger* than their plain text today: the dictionary's own index
structures outweigh short entries.)

### Change

A third dictionary form beside plain and symbol-coded: **blocked**. Entries in insertion order, ~64 KB of them
per block, each block LZ4 or deflate (both in tree already: `BlockCompressed` uses them, `:18627`), a decoded-block
LRU exactly like `block_lru_cache` (`:15602`). Chosen by `optimize_encoding` only when the average entry is
≥ 128 B and the column is not a sort/filter key (the same guard `code_dictionary` already has for short entries,
whose comment records a 22% ORDER BY slowdown on `inputdate` when decoding got in the way). `dict.view(k)` decodes
one block; a scan that reads every entry of such a column (rare: these are payload columns) decodes blocks in order
once.

Point reads of one row cost one block decode on a miss (~50 µs for 64 KB LZ4) and nothing on a hit. Measure with
the ERP suite; the getOpenOrders-style queries that read `po_options` for a few hundred rows are the case to watch.

Expected: ~60 MB from the ten columns above, perhaps 80 MB across the long tail.

Also from the earlier plan, still valid and small: the date columns held as text runs (`salesorders.date_created`
5.3 MB, `debtortrans.inputdate` 4.9 MB, ~6 more) should take `TemporalSeconds` (Rule 2b, `:18062`). The rule exists
and the data qualifies (every value canonical, range fits); why it loses to RLE on these columns is one debugging
session, ~12 MB.

## 4. Column value indexes (84 MB) and probes (31 MB)

Not this round. `ColIndex` (`:30360`) already stores rows as delta-varint lists per code. The probe indexes are
rebuilt from `probes.list` at start and are what keep joins fast. Leave both until 1–3 are in.

## Order of work and gates

0. **Prove the slack theory on the local copy first** (half a day): run the local engine off atl's replication
   feed (the paused CDC step in the memory note), watch RSS against the engine's count, and confirm the gap grows
   with keyed writes and dictionary widening rather than with reads. If the gap grows for another reason, the
   plan reorders. No engine change until this is done.
1. **Key maps** (biggest: ~300 MB on atl). Own design note with the overlay's fold rule and the text arena layout,
   tests that cover: out-of-order insert, delete past a quarter, text keys, a map that is erased to empty, and the
   degenerate hash the existing tests use. Gate: `asql-erp-bench` point selects and keyed updates within a few
   percent; sysbench `point_select` and `update_index` rows in `docs/asql-benchmarks.md` re-run.
2. **Widen by a bit** (~36 MB plus churn). Tests: a write past the width in each scope (append, update in place,
   bulk apply), and a check that `SHOW ASQL STORAGE` after a write shows the width grew by one bit, not to 32.
   Gate: the insert and update benchmarks, since this moves work from the checkpoint into the write.
3. **Blocked dictionaries** (~60–80 MB). Tests: round trip through save/load, `view(k)` across block edges,
   compaction of a blocked arena, a column that crosses the threshold on append. Gate: the ERP suite on the
   `po_options` readers.
4. **Date columns to TemporalSeconds** (~12 MB). Small; find out why Rule 2b loses, then a test on the real
   column shapes.

Every step on the local copy only. atl gets the binary once a step passes its gate here, with a backup and
md5 as usual.

## Done, 2026-10-28

Steps 1-4 are in (one commit after this doc). What was found on the way, and what the local copy measures.

**Corrections to the analysis above**

- The fresh-load figures were misleading. The same export loads at 662 MB of columns / 1,079 MB counted with any
  binary and shrinks to ~412-444 MB over the first checkpoints (dictionary compaction, front coding). Comparisons
  below are fresh load, then four write/checkpoint cycles (20,500 statements each), on the same store.
- The date columns were not stuck because Rule 2b lost: the checkpoint already turned a *dictionary* of dates into
  seconds (`dict_to_temporal`) and skipped a *run-length* one. `date_created` and `inputdate` are RLE.
- The slot form costs 53 B a key after churn (rehash doubling), not 24: the test's 314k keys took 16.8 MB.
- On the live replica (local engine on atl's binlog) the allocator gap went from 234 MB to 558 MB in 100 minutes of
  replication - 64k updates and 15 maps turning into slots in that time. No reads ran against it.

**What changed**

1. `PkMap` keeps its runs under writes: out-of-order keys and re-pointed keys go to a sorted overlay (`late_`),
   erased keys to `gone_`, both folded into the runs once they pass 4,096 + 1/64 of the map. A text key or a walk
   still turns the map into slots; nothing else does, and the "too scattered" rule is gone (a run a key, 16 B, is
   still under a slot's ~23). `tools/asql-pkmap-runs-check.cpp`: 708k differential steps against a `std::map`; 50k
   lines added to old orders, 30% deleted, 20k re-pointed: 13.5 B a key, runs throughout (53 before).
2. A write an encoding cannot take in place makes the column a dictionary at the width it needs (`to_dict_narrow`,
   `widen_for`), and a single-row delete keeps a byte-rounded width; never the whole column at 32 bits.
   `tools/asql-dict-width-check.cpp`: UPDATE of a run-length cell, DELETE, appends - 3, 3 and 5 bits (32 before).
3. Dictionaries of long entries (128 B and up on average, 1 MB and up in all) are held in LZ4 blocks of ~16 KB
   (`DictArena::block_upto`, `blocked_view` with two decoded blocks per thread); one already in symbol codes is
   decoded and blocked once. The checkpoint now runs `code_dictionary` within a 32 MB budget, so columns encoded
   before either form existed get it. Table files are AQT8 (a blocked dictionary has its own marker); an older
   binary refuses an AQT8 file, so a rollback on atl needs the pre-deploy store copy, not just the old binary.
   `tools/asql-dict-blocks-check.cpp`: 14.4 MB of options text held in 1.5 MB, every row back, LIKE and ORDER BY
   as the plain column, 50 appends after blocking, a reload.
4. A run-length column of canonical dates becomes `TemporalSeconds` at the checkpoint like a dictionary one did.
   With it, two paths the seconds encoding lacked: a sort key as integers (ORDER BY date_created 118 ms -> 3.6 ms)
   and the comparison predicate against a date or datetime constant as a number (the text compare per row was
   3.8 ms a month's count against 0.9). `tools/asql-temporal-choice-check.cpp`.

**Measured on the atl export (808 tables, 10.3M rows), same store, same four checkpoint cycles**

| | unmodified HEAD | this change |
|---|---|---|
| columns after checkpoints | 412.4 MB | 375.8 MB |
| counted (`Asql_memory_bytes`) | 467 MB | 430 MB |
| resident | 715 MB | 690 MB |
| salesorderdetails.po_options | 27.0 MB | 8.5 MB (5.8 with deflate blocks; LZ4 chosen for the scans, below) |
| salesorders.payment_details | 5.8 MB | 3.0 MB |
| binmovements.variables | 7.7 MB | 5.3 MB |
| salesorders.date_created / debtortrans.inputdate | RLE 5.0 / 4.7 MB | seconds 1.2 / 1.5 MB |
| key maps (8.8M keys), after a day on atl | ~340 MB (slots) | runs: ~2 MB loaded, ~13 B a key under churn |

Queries (engine-side latency, median of 5, result cache missed), unmodified HEAD -> this change: point selects
by key 0.09-0.13 ms both; `ORDER BY date_created LIMIT` 118 -> 5.1 ms; a month's `COUNT(*)` by date_created
0.94 -> 0.57 ms; `po_options LIKE` 164 -> 98 ms; `payment_details LIKE` 33 -> 36 ms (63 with deflate blocks: a
LIKE decodes every block per statement, which is why the blocks are LZ4); `gltrans narrative LIKE` 46 -> 49 ms;
joins and group-bys within noise. The ASQL suite (23 checks) passes; `asql-row-index-churn-check` now spaces its
ids, since consecutive ids cost 16 B a table and never reached its budget.

The columns figure does not include the key maps or the allocator; on atl those are the larger part (340 MB of
maps in slot form, and the gap that followed them), and this change takes them out by keeping the maps as runs
and not re-materialising columns on writes.

What is still open: the allocator gap itself. The replica measurement says it follows keyed writes and map churn,
which steps 1 and 2 remove, but that is a prediction until the new binary runs under replication (the next thing to
do on the local replica, then atl).

**Later the same night (commits 5768d8a, 4f9e993, 4056d10 and the stall series)**

- A checkpoint right after boot: a store loads at what its files hold (660 MB of columns) and the first
  checkpoint's upkeep takes it to 400 MB; it used to come after 20,000 statements. The warm-up asks for one.
- Folding deleted rows out of a key map used `begin()`, which turned the map into slots for good: every table with
  a deleted row had a fat map after its next checkpoint (289 MB on a fresh load). `PkMap::remap_or_erase`.
- Date predicates prune by the segments' date bounds; a symbol-coded dictionary with no plain tail reaches the
  block step; decoding one before blocking no longer holds 3x its text.
- Measured on the same store and protocol with the night's last engine (01fb45ce): columns 412 -> 375 MB, counted
  467 -> 429 MB, resident 715 -> 688 MB. Queries, unmodified HEAD -> this: `po_options LIKE` 164 -> 63 ms,
  `gltrans narrative LIKE` 46 -> 26, `payment_details LIKE` 33 -> 21 (every LIKE over a coded or blocked
  dictionary now scans it a slice at a time instead of decoding it whole), `ORDER BY date_created LIMIT` 118 -> 5.5,
  a month's date-range count 0.94 -> 0.20, point lookups by key 0.1-0.16 ms both, joins and group-bys within noise.
- A session can raise or drop its read deadline with `SET max_statement_time`; `SHOW STATUS` answers without the
  statement lock and counts the deadline and hang-up kills.
- Under replication the growth is transient: three local replicas of atl ran the night on the new engine through a
  burst of 900k replicated updates; resident rose to ~1.2 GB during it and fell back to counted + ~60 MB once it
  ended (A 741, B 699, C 522 MB at 02:21), with 10 / 41 / 41 maps in slot form against hundreds before.
- Under replication the gap still follows writes with the maps staying runs: three local replicas of atl ran
  overnight on the new engine - default malloc, `MALLOC_ARENA_MAX=2`, and that plus `MALLOC_MMAP_THRESHOLD_=1048576`
  - sampled in `samples.txt` beside each store; the unit's `Environment=` on atl follows from them.

## Deploying to atl

Checked end to end at 03:00: the warehouse app deployed to a local grain host against this engine on a fresh
store (`touch_hour` rebuilt, 10,912 hours, 9.6 s), and its report for October 1-27 gave the known figures (292
cleared orders / 1,043 units, receiving 14,165, picking 1,807 lines).

The table files become AQT8 at the first checkpoint, which the old binary refuses. So, in this order:

1. `scripts/asql-export.sh` on atl (or `cp -a` of `/opt/aios/root-v8/sql_db` with the service stopped) kept beside
   the old binary `/opt/aios/bin/aios-829500b6` as the rollback pair; the export alone is not enough once the new
   binary has checkpointed.
2. Upload the new binary, verify the md5, point `AIOS_BIN` at it, restart `aios-compute`. `SHOW ASQL STORAGE` and
   `SHOW GLOBAL STATUS` (`Asql_memory_bytes`, `Asql_rss_bytes`, `Asql_pk_runs_to_slots`) at +1 min, +1 h, +1 day.
   Expected on atl after a day: `Asql_pk_runs_to_slots` in the tens, not the hundreds; resident well under the old
   2.1 GB.
3. Rollback = stop, put the pre-deploy store copy back, start the old binary. Replication resumes from the copy's
   `cdc_position` (keep it: `--keep-replication-state`).

The stall fixes (commit 4589495) ride along: the hang-up watcher, the 10-minute read deadline
(`AIOS_SQL_MAX_READ_MS` in the unit's environment to change it), lock-free `SHOW PROCESSLIST` and `SELECT 1`.

**Where the key maps stand (measured 02:35, fresh load, after the warm-up)**: 289 MB for 8.8M keys, and they
survive checkpoints. Of that, the 34 text-keyed tables (3.0M keys, slots at ~45 B) are about half, and integer
tables whose ids have gaps from old deletes are a 16-byte run per key for the rest; consecutive ids and the
re-packed pairs cost almost nothing. The old binary held 340 MB at this point and went to ~23 B a key for
everything under a day of writes; this one stays as loaded.

**Text keys as a sorted arena (done 2026-10-29 06:00, commit after 81e96a7)**: `PkMap::sort_text_keys` turns a
text-keyed map of 4,096+ keys into one arena in key order (varint length + bytes), a 32-bit offset and row id a
key, and an index of every 64th key's first 8 bytes that narrows the bisection to a few groups. Writes go to an
overlay (`slate_`: new keys sorted; `sgone_`: erased positions) folded in at the runs-mode threshold; a re-point
writes the id in place; an id past 32 bits or a walk turns the map back to slots (`Asql_pk_sorted_to_slots`), to be
sorted again. `maintain_storage` sorts up to 500k keys a checkpoint, text-keyed slot maps only (integer maps that
fell back to slots - 31 on this store - are not candidates: counting them ate the budget). Measured on the atl
store copy with the full warm-up list: key maps 289 -> 133 MB after the boot checkpoint and six maintaining
checkpoints (19 maps, two small ones left for the next), counted memory 691 -> 577 MB; `prices` 879k keys
113 -> 43 MB in 408 ms, `stockmoves` 574k in 226 ms, everything else under 100 ms. Micro-benchmark on 3M keys:
239 MB -> 73 MB, lookup 0.11 -> 0.71 µs (overlay of 50k: 0.89 µs); the query bench is unchanged (point reads
0.1 ms). Differential soak: 300 sorted sequences, 3.2M steps, every answer the model's. `AIOS_SQL_PK_SORTED=0`
keeps every map as slots.

**The RSS gap, settled (07:20)**: RSS stood 160-190 MB above `Asql_memory_bytes` on every replica whatever the
allocator setting, and on a fresh load. glibc's own figures (`Asql_malloc_live_bytes`, `_mmap_bytes`,
`_free_bytes`, from `mallinfo2`) show live bytes equal to RSS and only 15-50 MB free: not fragmentation, data the
estimate never counted. A per-step memory lap at the start (`AIOS_STORE_TRACE=1`) puts 129 MB of it at "things and
cells loaded": 59,504 Things of two kinds (`sticker_item` 49,800 and `touch_hour` 9,704; 98 MB in their files, of
which 60 MB is history - up to 32 entries a Thing, each a before/after copy of its values). The footprint now
includes `ThingEngine::memory_bytes()` (`Asql_things_mem_bytes`, 126 MB here); the remaining gap on the atl store
copy is 52 MB. The next cut, a design question for the Things rather than the store: the history need not live in
memory (the as-of journal already writes it to disk) - keeping the last one or two entries and reading older ones
from the file would give back ~70 MB on atl.

## What this does not do

- It does not cap or trim the key maps (the earlier plan's phase 1). The engine rewrites `rowindexes.list` on every
  save (`save_index_list`, `:55124`), a cap under `AIOS_SQL_ROWINDEX_MB` cannot evict maps used in the last ten
  minutes (`:50962`, `:52215`), and tables over ~1.4M rows would lose their full map (`:52923`). Keyed writes on
  those tables would slow down, and replication is keyed writes.
- It does not change query plans, the result cache or the probe indexes.
- It does not promise a figure for atl until step 0 has shown what the slack is. The counted parts above add up to
  ~450 MB saved from ~1.0 GB; if the slack follows the churn, the process figure falls by more than that.
