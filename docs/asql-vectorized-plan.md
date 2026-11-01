# ASQL: finishing vectorization, and a physical plan

Two steps toward analytic speed, ahead of any decision on compiling pipelines (which stays open until these are
done and measured):

1. **Finish vectorization** - typed values through joins, grouping and window functions, flat hash tables, no
   per-row allocation or text on hot paths.
2. **A physical operator plan** - the decisions `execute_parsed_select_single` makes inline written down as a plan,
   and the 5,000-line function split into operators that execute it.

Compiled and vectorized engines land within about 2x of each other (Kersten et al., VLDB 2018); both are far ahead
of an engine that moves values through general objects a row at a time. ASQL is between the two: its expressions
run as vectorized programs (`sql_vprog.inc`), stages hand typed columns to each other (`sql_colbatch.inc`,
docs/asql-typed-pipeline.md), but joins, grouping and window functions still carry rows, `Val`s and node-based
hash maps.

## Where the time is (SQLDoom E1M1, three frames under callgrind, 22.0 billion instructions)

| | share |
|---|---|
| building join indexes (`get_or_build_probe_index`; `unordered_map<double, vector>` a key) | 6.6% |
| window functions (`execute_windowed`) | ~9% |
| vectorized programs (real work) | ~10% |
| malloc / free / memset / memcpy | ~12% |
| number <-> text and exact-decimal conversions (`strtod`, `exact_decimal_of`, `to_string`, `scaled_to_decimal`) | ~6% |
| grouping: rows moved into per-group vectors, keyed by `unordered_map<std::string, ..>` (sql_database.cpp:47939) | see stage 1b |

Phase times (`AIOS_VX_TRACE`, nested so they overlap): the join steps dominate, then emit and source setup.

## The engine as it is

`execute_parsed_select_single` (sql_database.cpp:43812-48807) runs a branch in phases, marked by its trace:

| lines | phase |
|---|---|
| 43812-44782 | setup: sources resolved, CTEs / derived tables / LATERALs materialized |
| 44782-45213 | fast paths; the single-table engine (`try_vx_single_table`, 1,442 lines) |
| 45213-45695 | analysis, semi-joins, the base source's rows |
| 45695-47188 | join steps: per step a method (index probe, batched probe, expression hash, int hash, text hash, small-table, scan) |
| 47188-47437 | expansion, direct-to-client output |
| 47437-48685 | grouping and aggregates |
| 48685-48807 | emit, projection, output |

A joined row is a tuple of row ids (`JoinedParts`, 7 inline, sql_database.cpp:6108) - already the right shape for a
vectorized engine (a selection vector per source). Values are read from column stores by those ids.

## Rules for every stage

- **The same answers.** Every stage is checked by: the four oracle suites (vectorized programs, CTEs, joins,
  independent formulations - 213,578 lines) against the build before, byte for byte; the SQL test set; the SQLDoom
  frame's bytes; and a targeted test for what the stage changes, run in every scope (top level, functions,
  nested). Where a stage has a switch-off, the targeted test runs with it on and off.
- **Measured both ways.** Callgrind instruction counts (deterministic) and alternating wall-clock rounds, against
  the build before; a stage that does not measurably help is not kept.
- **Opt-out switches** for each new path while it settles, as `AIOS_SQL_BATCH_JOIN` / `AIOS_SQL_COLUMNAR` are.
- **No semantic shortcuts.** Exact decimals stay exact, a DOUBLE sum adds in the same order, collations and NULLs
  behave as before; a case a fast path cannot answer exactly goes to the path that does.

## Stage 1: finish vectorization

**1a. Flat join indexes.** `SqlProbeIndex::by_number` is an `unordered_map<double, std::vector<uint32_t>>`: a node
and a vector a distinct key, a hash and two dependent loads a probe. A `NumberIndex` holds the same mapping as a
flat open-addressed table of keys with each key's rows in one shared array (CSR); readers use `find(key)` returning
a span. Writers that maintain an index incrementally (row writes to base tables) keep working on a map form, and
the flat form is rebuilt from it when next read after a change; an index built whole (a statement's result tables:
most of SQLDoom's) is built flat directly from typed columns - a counting pass and a fill, no map. ~15 read sites
move to the accessor.

**1b. Hash aggregation.** GROUP BY over typed key columns: a group id per row from a flat hash of the keys (lanes,
not strings); COUNT, SUM, MIN, MAX, AVG of typed lanes folded into per-group accumulators in row order (so a
DOUBLE sum adds in the same order as now; an exact SUM in 128-bit integers at the column's scale). Rows are not
moved into per-group vectors. Anything else - DISTINCT aggregates, GROUP_CONCAT, user aggregates, HAVING over
non-compiled expressions - stays on the current path for that statement.

**1c. Window functions.** Measure, then: partition and order keys as typed lanes sorted once (a permutation, not
rows), running aggregates and ranking over the permutation.

**1d. Text and exact-number conversions on hot paths.** Profile-driven: each remaining `strtod` /
`exact_decimal_of` / `to_string` hot spot traced to its caller and replaced by a typed read where the answer is
provably the same (as the CTE typing and join index fixes were).

**1e. Allocation.** Join row buffers, group vectors and per-statement temporaries from a per-statement arena where
the profile shows malloc; measured per change.

## Stage 2: a physical plan

**2a. The plan, written down.** `PhysicalPlan`: per branch, its sources (scan / materialized CTE / derived /
LATERAL), the base source and its selection, each join step with its kind and chosen method and keys, grouping
(method, keys, aggregates), window, sort, limit, projection. Made by a planner function from what the current code
decides inline - the same decisions, moved, not new ones - and printed by `EXPLAIN`. Checked by comparing the plan
with what the trace says ran, over the oracle suites and a SQLDoom frame.

**2b. Operators.** One phase at a time, the code that runs it moves out of `execute_parsed_select_single` into an
operator function that takes its plan node and the rows, in its own file (`sql_sources.cpp`, `sql_join.cpp`,
`sql_group.cpp`, `sql_window.cpp`, `sql_emit.cpp` - the split of docs/asql-source-layout.md). Each move is checked as
the file split's steps are, including the object-code and timing comparison.

**2c. The driver.** What is left of `execute_parsed_select_single` walks the plan.

### Stage 2a, done: the plan as recorded

`sql_plan.inc`: `PlanStep` / `PhysicalPlan`, a thread-local sink (`PlanScope`) and `plan_add` at each decision point
of `execute_parsed_select_single` and `execute_with` - sources (table / cte / result table), the single-table
engine, the base rows, every join step with its method (index probe, batched, expression hash, int hash, text hash,
small table, scan) and ON text, the grouping method (integer lanes, star columnar, hash of key text), DISTINCT, the
sort, the emit form (typed columns / client sink / rows, with the limit), and each CTE, derived table, LATERAL and
series as a step with its own nested plan. Recording is off (a null check) unless EXPLAIN asked, measured identical
on every suite. `EXPLAIN <select>` runs the statement with the answer discarded - the plan is decided from the data
at hand - and prints one line a step with rows and time; the table of invented text it printed before is gone.
Known gaps, for 2b: a subquery that fills an IN / EXISTS set appears as its own flat steps, not nested; the
single-table engine is one step with no inner detail; a window statement's inner plan follows the `window` step.

### Stage 2b, in progress: operators out of the select function

Each phase's code moves verbatim into a static operator function taking its inputs as a plan struct, in
`sql_database.cpp` for now: the operators use its file-local types (`Val`, `SqlExpr`, `EvalCtx`, `JoinedRow`), and
moving those into engine-internal headers is the file split's own later step (docs/asql-source-layout.md, 5-6).
Each move is checked as the file split's steps are: a sorted-line diff shows only glue, per-function object code
against the build before, the suites, the frame's bytes and alternating frame times.

- **emit** (done): `EmitPlan` + `emit_result` - DISTINCT, the sort, the projection header, the output form.
  `execute_parsed_select_single` 177,561 -> 173,659 bytes of code, `emit_result` 7,298; 5,025 of 5,099 functions
  the same size (the 74 changed are the sort template instances renamed with `OutRow`, and EXPLAIN); frames
  520/523/506 -> 518/533/516 ms.
- **group** (done): `GroupPlan` + `group_result` - the 921-line grouped branch (lane, star columnar and general
  grouping, HAVING, the groups' ORDER BY .. LIMIT short cuts, the grouped emit); returns an error to hand back or
  nothing to go on to the emit. The select function 173,659 -> 137,276 bytes, `group_result` 44,327; 4,934 of 5,045
  functions the same size; frames 497/491/517 -> 490/517/493 ms.
- **join** (done): `JoinPlan` + `join_steps` - the 1,375-line loop over the join steps (each step's own conditions,
  key and method choice, probe, outer-join padding, conjuncts in lanes after the step). The select function
  137,276 -> 89,801 bytes, `join_steps` 53,923; 4,933 of 5,046 functions the same size; frames 522/517/510 ->
  521/510/503 ms. One step is still one iteration of that loop; splitting the step into its own operator with a
  `JoinStepPlan` node comes with the compiler's needs.
- **project** (done): `ProjectPlan` + `project_rows` - the ungrouped statement's 391-line projection (stored
  columns from their cells, items as lanes, the interpreter for the rest, parallel chunks, the bounded emit, the
  batch and sink forms). The select function 89,801 -> 78,167 bytes, `project_rows` 22,425; 4,969 of 5,060
  functions the same size; frames 499/520/522 -> 532/502/521 ms.
- **base** (done): `BasePlan` + `base_rows` - the 628-line base selection (the base source's WHERE conjuncts in
  lanes or through probe indexes and zone maps, semi-joins against the joined tables, rows as ids alone where the
  joins can take them). The select function 78,167 -> 63,657 bytes, `base_rows` 21,008; 4,975 of 5,048 functions
  the same size; frames 497/508/503 -> 496/497/512 ms.
- what is left (2,000 lines): **setup** - the sources resolved and materialized (CTEs, derived tables, LATERAL,
  merged views), the branch rewritten, the join order fixed (983 lines, reads only the branch) - which is the
  planner, not an operator: its output is the state the operators take; the fast paths and the single-table
  engine's call; the star join; the residual WHERE and the select-list expansion. Step 2c makes the setup's output
  a `SelectRun` struct the operators' plans point into, and the function the driver that walks it.

## Stage 3: compiled pipelines over the plan (decided: both)

### 3a, done: native code for the lane programs (x86-64)

`sql_vjit_x64.inc`. The interpreter ran each instruction of a program as a pass over the chunk's rows and allocated
that instruction's lanes: K passes and K allocations a chunk. The native code is one loop over the rows: every
non-leaf instruction's value in a stack slot, the leaves (columns the interpreter's vectorized readers decode,
constants) read from their lanes, only the output lane written. It keeps the interpreter's rules exactly - the NULL
rule of each operator, integer overflow and a non-finite double bail out of the chunk and the interpreter runs it
and reports as before, the libm functions and the casts the interpreter calls are called (not re-derived), -0 and
NaN handled as the interpreter's comparisons do. Text (a reference into a dictionary or the literals, an I64 lane) is compared through the
interpreter's own comparison and passed through CASE / COALESCE / NULLIF; exact-decimal arithmetic keeps the
interpreter's exactness test (a helper); POW / ATAN2 call libm as the interpreter does. Not covered (the program
runs interpreted as before): tuple IN, text in arithmetic or as a truth value, programs past 256 instructions.
`AIOS_SQL_VJIT=0` turns it off. No AArch64 emitter yet: this machine has no AArch64 toolchain or emulator to test
one on, and an emitter that cannot be run against the interpreter is not shipped.

Correctness: `AIOS_SQL_VJIT_CHECK=1` computes every chunk both ways and compares bit for bit (a mismatch, a bail
the interpreter would accept, or an answer the interpreter would refuse aborts, naming the operator). Under it the
four oracle suites, the lane GROUP BY / mixed CASE / DECIMAL tests, the SQLDoom frame and the SQL test set all run
to the same output as before, and `asql.vjit.check` (tools/asql-vjit-check.cpp) runs 600 random expression
trees a day over random and extreme data, text included, under the same check. Measured: SQLDoom E1M1 medians
549/514/508 -> 485/488/491 ms at first, 497/495/504 -> 483/480/479 with the wider coverage; of the frame's
programs only 12 leaf-only ones (nothing to compute) and 3 tuple-IN ones still interpret.

Next in 3: the operators' own loops (the batched join probe, the lane GROUP BY's hash and folds) generated per
pipeline with the program's code fused in, so a chunk is not written to lanes between them; AArch64 emitter.

### Between 3a and 3b: what the profile said to remove first

A three-frame profile after 3a (15.0 billion instructions) put ~1.0 billion in work no reader used: zone maps
built for the statement's own result tables (`extend_zone_map` on 1.15 million appended cells, `build_zone_maps`
from `adopt_codes` and `optimize_encoding`), and text cells of those tables normalised, interned and appended one
at a time (`colbatch_fill_table` -> `ColumnStore::push_back`). Measured first: across three frames the renderer
filled 273 result tables (108 of them over 16,384 rows, 1.3 million rows in all) and the zone maps pruned 0
chunks (`Asql_zone_map_pruned` before and after).

Done: `SqlTable::result_table` (`mark_result_table`, set where a CTE, derived table, LATERAL, UNION step, window
step or view-as-rows table is made, and by every fill from a batch or result rows) puts `zone_maps_off` on the
table's columns, and `build_zone_maps` leaves such a column without stats - every reader already takes a missing
zone map as "may match", and a checkpoint never sees these tables. Text or dictionary-coded cells going into a
REAL result column are parsed once per distinct text and the column typed (TypedDouble, as an F64 batch is),
when every entry is a finite number other than -0 - what a typed cell prints is exactly `sql_normalize_value`'s
text; anything else keeps the text path. Fill time per three frames 146 -> 66 ms (text into DECIMAL 140 -> 53
ns a row, codes into REAL 145 -> 53, text into REAL 653 -> 136); SQLDoom E1M1 medians 469/457/465 -> 424/444/426
ms, alternating; frame bytes, the four oracle suites (213,644 lines), the lane / mixed / decimal checks and an
edge-case set for text into REAL ('', 'inf', '-0', '1e500', 'nan', '0x10', ' 1', '1.', '.5', 20-digit integers)
identical to the build before.

Still there for a Dec batch kind (DECIMAL cells travel as text between stages, 370,000 of them a frame at 53 ns):
the biggest fill item left; and `active_data_tick`'s re-encoding of the game's written tables (0.6 billion
instructions on a background thread per three frames), which contends with the frame's workers.

Next by the same profile: `predicate_true` from the join probe, 0.8 billion instructions - the ON clause's
residual conjuncts interpreted once per candidate row. The trace named the join: `LEFT JOIN flat_textures fl ON
NOT p.is_sky AND fl.name = p.tex_name`, 56,000 candidates, 24-29 ms of ON checks a frame. Done, in three parts:

- A conjunct the base rows alone decided (`base_refused`, which already padded or dropped the rows it refused)
  is true for every candidate of a row it kept, so it leaves the per-candidate check.
- The conjuncts that compile as programs are checked over a chunk's candidates at once in lanes: the probe
  leaves a chunk's candidates in a sink (`OnSink`) instead of checking each, `flush_sink` runs each program over
  them and then emits the survivors in base order with what follows the ON (the matched mark, the prefilter,
  the pushed WHERE conjuncts), padding a base row no candidate passed as the per-row probe pads one. A program
  that refuses a value hands that conjunct to the interpreter, which reports as before. The rest stay per
  candidate; the trace line `ON conjuncts: N decided by the base rows, N in lanes, N per candidate` says how
  each join split.
- `text_col = text_col` over two dictionary-coded columns compiles to two CodeI leaves holding each dictionary
  entry's collation class (one class per collate_key, worked out once over both dictionaries) and an integer
  Eq / Ne, all native; it was each row's two texts through pad_compare_ci (~350 ns a row). And `Lanes` reads a
  column already laid out flat (a typed column's values, a 32-bit dictionary's codes) in place, and decodes a
  column whole only when it is at most 16x the row set it serves (4,096 rows at least): under a flat 65,536 floor a
  program run over a chunk of 256 candidates decoded a 56,000-row result column whole, once per chunk.

Measured, three frames: ON checks 92 -> 39 ms (the flat-texture join 29 -> 7 ms a frame, most of what is left
being the trace's own two clock reads per candidate); SQLDoom E1M1 medians 456/463/450 -> 413/428/440 ms against
the build before both steps, alternating.

### The join step's rows extended in place

With the ON checks gone, a lookup join's time was the rows themselves: a LEFT JOIN of 56,000 rows against a texel
table that met no candidate at all spent ~110 ns a row copying each base row into its chunk's list with the
NULL part appended, then again into the step's output (and freeing the input): ~6 ms a step, and a frame runs
many such steps. The step's join phase was 807 ms of three frames.

Done: the probes put a chunk's result into parts (`ChunkOut`: each base row's one joined row, or NULL-padded, or
dropped) and the base rows are extended where they are - a `parts.push_back` each, the dropped ones compacted
away, the input vector becoming the output. A base row meeting a second candidate turns its chunk to list mode
(the rows themselves, in probe order, as before), so a 1:N join makes exactly the rows it made; chunks are joined
back in order either way. A candidate's row is only made when the interpreter has to see it (per-candidate ON
conjuncts, pushed WHERE conjuncts). A one-row table joined with nothing to check is appended the same way. Also
in this step: identical leaves of a program are one leaf (a 55-instruction key expression read `rs.sky_rows`
three times: 21 leaf reads a row -> 14, 55 -> 44 instructions), and a projection's programs share one `Lanes` -
the row ids per source and the decoded columns worked out once for all its items.

Measured, three frames: join steps 1,157 -> 531 ms; a 56,000-row LEFT JOIN with no match 6.0 -> 1.3 ms; SQLDoom
E1M1 medians 449/458/462 -> 357/340/360 ms against the build before these three steps (from 5cab6ef),
alternating - 2.1 -> 2.8 frames a second. Frame bytes, the four oracle suites, the lane / mixed / decimal checks,
the text-equality and residual-ON sets, and a set for the in-place step (one, two and no candidates mixed
across chunks, LEFT / inner / RIGHT, pushed WHERE on padded and matched rows, a one-row table then nine more
joins past the inline parts, text keys with a lane conjunct, a GROUP BY over the step) are identical to the
build before.

What the measurements say is left in the programs: leaf gathering ~2.5 ns a leaf read and the native code ~1 ns
an instruction a row, every intermediate through a stack slot (a store-to-load dependency on each). Values in
registers and the leaves gathered by the native code itself are the next codegen step (3b).

### LATERAL generate_series expanded by the join step

The fresh profile (11.8 billion instructions for three frames, down from 15.0) put the next weight in the
renderer's pixel expansions: `CROSS JOIN LATERAL generate_series(ps.y0, ps.y1) AS y` and its kin (`plane_pixels`,
`view_holes`, `thing_pixels`), 20-23 ms each a frame. A lateral series was made by `materialize_lateral`: a
DISTINCT query over the earlier sources for the bound values (re-running their joins), a series table filled per
distinct value with a hidden key column, and the join `t.__lateral_key = __LATKEY(cols)` probed per base row
through the interpreter, each output row then copied into a chunk list and again into the step (a 1:N join is
list mode). `view_holes` spent 21 ms making 53,760 candidate pixels to find there were no holes.

Done: a generate_series whose arguments name earlier sources' columns is a `SeriesSpec` on its source
(`QuerySource::series`; the series table is the statement's own, one column) and the join step expands it: the
bounds over every base row at once where they compile (integer lanes: the range straight from the integers;
otherwise their text, or the interpreter's, through the same rule as `materialize_series` - `series_range_of`,
shared), each base row's rows made in order with the numbers adopted into the series column (integers typed;
decimals at the largest scale as text). A CROSS JOIN drops a base row whose range is empty, a LEFT JOIN pads it;
an inner join's ON and the pushed WHERE conjuncts go through `vp_deferred` where they compile, a LEFT JOIN's ON
decides each candidate. `materialize_lateral` stays for LATERAL subqueries and for a series without earlier
references (`materialize_series`).

Measured: SQLDoom E1M1 medians 458/480/474 -> 319/334/330 ms against the build before today's commits
(5cab6ef), alternating - 2.1 -> 3.0 frames a second; `view_holes` 21 -> ~3 ms, `plane_pixels` 23 -> ~4 ms.
Frame bytes, the four oracle suites, the lane / mixed / decimal checks and a generate_series set (integer,
decimal and NULL bounds, steps including 0 and negative, empty ranges under CROSS and LEFT, `AS g(n)` naming,
`ON true`, ON and WHERE on the series value, two laterals in one FROM, bounds over two earlier sources, a
non-lateral series, under and over 64 base rows, text bounds refused) are identical to the build before.

Still from this profile: `project_rows` 19% (programs and gathering), `execute_windowed` 11% and the interpreted
`group_result` 10% (which statements take them is the next thing to measure), `get_or_build_probe_index` 4.7%
rebuilt per statement for result tables, `colbatch_fill_table` 4.3% (DECIMAL text), and at startup
`JEPAWorldModel` drawing a million normal variates.

### GROUP_CONCAT in lanes; the lane GROUP BY's key sort

The frame's final statement, `SELECT GROUP_CONCAT(rgb ORDER BY pix SEPARATOR '') FROM framebuffer`, interpreted
two expressions for each of 64,000 rows and stable-sorted 64,000 Vals: 27 ms a frame. Done: with no DISTINCT,
the values and keys come from lanes - a value that is a TEXT column of a source straight from its cells (the text
the interpreter's Val holds for one; a program's text leaf refuses a result column whose text can read as a
number, which `rgb`'s binary triples do), a key that is a typed number column read as it is (a bare column has
no program), anything else through its program - the row numbers stable-sorted by the keys as numbers (NULL in
any key, a text key, DISTINCT or an ordinal past the arguments: the row way), the texts joined. 27 -> 6.6 ms.
And the lane GROUP BY's order over one key sorts packed (NULL flag, value, group) records instead of comparing
through the groups' first rows into the lanes (53,760 groups).

Measured: SQLDoom E1M1 medians 446/465/471 -> 291/293/291 ms against the build before today's commits
(5cab6ef), alternating - 2.1 -> 3.4 frames a second.

### 3b, first part: results in registers; FLOOR and CEIL in line

Every instruction's result went to its stack slot and the next instruction loaded it back: a store-to-load
dependency (~5 cycles) on each link of a chain, which was ~1 ns an instruction a row. Done: a write-through
register cache in the builder. A result still goes to its slot and also into a caller-saved register (R8-R11
for an integer or text lane, xmm5-xmm15 for a double; an F64 instruction's register is an xmm whichever way
its bits were stored, as Neg and Abs store sign-flipped bits through the integer store), and its readers take
the register. An instruction keeps one register through every path that stores it (a CASE's branches), so a
reader sees the same place whichever path ran; a path that leaves it NULL writes no value, and no reader looks
at a NULL's value - exactly as the slots were. A helper call forgets every register; And / Or use R8-R11 as
scratch and forget those first. FLOOR and CEIL are in line for |a| below 2^52 and not zero (the truncation
moved down or up where it passed a; CEIL keeps a's sign, so CEIL(-0.4) is -0 as libm has it); a zero, a NaN,
an infinity or a larger number is already whole.

Found by the differential check over the frames, not by the randomized tool: the Neg-of-a-double cache case
(a program whose Sin and Cos read a negated angle), and CEIL's -0 by the vpeq suite. The tool now also prints
unary operations on a column singly and nested, so a sign of zero or a negated double's bits are compared where
nothing absorbs them (not re-run against the code before the fixes).

Measured: SQLDoom E1M1 medians 471/453/450 -> 284/283/300 ms against the build before today's commits
(5cab6ef), alternating - 2.1 -> 3.5 frames a second; the texel-key programs 4.7 -> 4.5 ms, the five-program
projection 18.1 -> 17.5 ms (their leaves and chunk overhead, not the native loop, are most of what is left).

### 3b, second part: the leaves gathered by the native code

A program's leaves (its column reads and constants) were each a pass in C++ writing a lane - nine bytes a row
a leaf - that the native loop read back; 21 leaf reads a row in the key programs before leaves were shared, ~2.5
ns each. Done: a leaf whose column is laid out flat - a typed int64 or double array, 32-bit dictionary codes
with the leaf's code table - is gathered by the loop itself from a descriptor filled per chunk (`LeafDesc`:
the data, the null bitmap and its bytes, the column's rows, the code table and its NULL flags, the source's
row ids): the row's id from the source's row-id array (kNullRow, a row past the column or a NULL row: NULL, as
the readers have it; a column with no NULL skips the bitmap, as ColI / ColF's readers do), then the value, into
the leaf's slot and register as a computed instruction's. Each descriptor carries the layout kind it was filled
from, and the loop compares it with the kind it was built for before the first row - a column re-encoded since
bails with 2 and the interpreter takes the chunk; `valid()` then recompiles the program for the new layout. A
packed or run-length column (a FOR-encoded id, 8-bit codes) keeps the lane path for that leaf.

Measured: the texel-key programs 4.5 -> 3.7 ms over 56,524 rows, the five-program projection 17.5 -> 11.3 ms;
SQLDoom E1M1 medians 458/467/463 -> 278/276/269 ms against the build before today's commits (5cab6ef),
alternating - 2.1 -> 3.6 frames a second. The differential check over the frames, the randomized tool under
eight seeds, frame bytes, the four oracle suites, the lane / mixed / decimal checks and the CAST set are
identical to the build before.

### The probe's own equality is the probe's; a result table's plain text column as a code-join key

Two more things the trace showed in the texel joins. Their ON clause's key equality (`ft.k = <expression>`)
was still checked for every candidate - in lanes, but every candidate - although the batched probe had found
each candidate by that very number: where the probe is exact (a typed number column found by its number with
no wide values, or dictionary codes mapped by their collation keys), the key's conjunct now leaves the ON
check. And a result table's text column (plane_tex's texture name) could not be a star key because a result
table's "text" may hold numbers the interpreter compares as numbers; one whose dictionary holds no entry that
reads as a number (`text_dict_plain`) is compared as text, so it can - `fl.name = p.tex_name` is a direct code
join now. ft 8.0 -> 5.3 ms, wt 7 -> 5.2 ms a frame; SQLDoom E1M1 medians 475/477/484 -> 277/268/291 ms against
the build before today's commits (5cab6ef).

The batched probe then took direct code joins (its best key probed by dictionary code through the star code
map, the other keys as numbers): fl's per-row probe, a vector of candidates per base row, 9.4 -> 1.4 ms a
frame; medians 456/452/468 -> 274/268/298 ms.

INT result columns that arrived as dictionary codes stayed dictionary-coded, which made them keys "not held as
a number" to the batched probe (bsp_order's, thing_pixels', the ui joins) and lane-path leaves to the programs.
Codes into an INT column whose every entry is a canonical integer are parsed once and the column typed, as an
I64 batch is (and as codes into REAL were). Medians 481/458/450 -> 274/273/261 ms against 5cab6ef. The trace
now names the key and encoding that keep a join off the batched probe; what is left there: Constant-encoded
columns (a map_id that is 1 in every row) and text keys beside the probing one.

Native code, CAST AS SIGNED: a double that is already whole and below 9.2e18 - CAST(FLOOR(x) AS SIGNED), most
casts - is converted in line (cvttsd2si, checked by converting back); the helper decides the rest (a fraction's
rounding as written, the range error). The renderer's key expressions cast four or five times a row and the
call was most of their time: the texel-key programs 6.5-9.8 -> 4.7 ms over 56,524 rows, the five-program
projection 21 -> 18 ms; a frame within the run-to-run spread. The differential check over the frames, the
randomized check under six seeds, and a CAST set (fractions at and near a half, negatives, whole doubles up to
9.2e18 and past it, 2^53+1, -0, 1e300, 1e-300) are identical to the build before. Frame bytes, the four oracle suites, the lane / mixed /
decimal checks and a GROUP_CONCAT set (integer, double, expression and ordinal keys, DESC, two keys, NULL keys
and NULL values, DISTINCT, SEPARATOR, several arguments, string_agg, per-group over GROUP BY, under and over 256
rows, an unsigned key, MD5 of the whole result) are identical to the build before. Frame bytes, the four oracle suites, the lane / mixed / decimal checks,
an edge-case set for text equality (case, trailing spaces, accents, NULL, result-table columns, numeric-looking
text) and one for residual ON conditions (LEFT / RIGHT / inner, base-only and two-sided conjuncts, NULL
conjuncts, numeric-prefix text keys, pushed WHERE with padding, a three-table chain) are identical to the build
before.

Keys beside the probing one, where the batched probe refused the join: a Constant-encoded INT column (a map_id
that is 1 in every row, on either side - a result table's or a declared one's) is its one value, parsed once; a
text key whose both sides are star-keyable (dictionary-coded, plain text) is compared by dictionary code through
the star code map, as the direct code join compares its probing key. One check (`other_keys_match`) serves the
numeric and the direct-code probe; the composite hash is built only over keys read as typed numbers. The three-key
join of bsp_order (map_id, node_id, side) and the ones over player_state.map_id are batched now: 63 -> 118 batched
joins a run, bsp_order 8.6 -> 5.8 ms. Medians 501/502 -> 294/283/287 ms against 5cab6ef (a slower machine
hour than the rows above; the pairs ran alternating). The first version of this skipped the joined side's check
for a Constant base key and read a Constant joined column as 0: frames came out black, which the frame-bytes
comparison caught before anything else did; the isolated join (node_children to node_path_steps) returned 0 rows
for 2228. A set for these keys (Constant both sides equal and unequal, Constant against a varying column either
way, three keys, text beside a number, text beside a Constant, LEFT JOIN padding, LOWER() on the text key, the
keys in WHERE) is identical to the build before, as are frame bytes, the differential native check over the
frames, the four oracle suites, the lane / mixed / decimal checks and the edge sets above.

Native code, loop-invariant instructions: a constant, a leaf gathered from a one-row source (the camera, the
render settings - a CROSS JOIN with one row gives every row the same row id there) and every instruction whose
operands are all such are emitted once before the row loop, into their stack slots, and skipped in it; the loop
keeps a guarded copy for a chunk whose row ids for those sources are not uniform (eval checks them, Env::hoist
says which). SIN(c.view_rad) and COS(c.view_rad) over 56,000 rows were a libm call a row each. Same binary,
AIOS_SQL_VJIT_HOIST=0 against 1 over 100,000 rows: the three plane_world programs 16.8 -> 11.5 ms, SUM(x * SIN(c.a)
+ y * COS(c.a)) 10.5 -> 5.5 ms. At frame level it changes nothing measurable: same binary, hoist off / on alternating,
medians 292/298, 280/285, 299/291 ms - the frame's programs spend their time in the per-row gathers and
operators, not in the invariant calls, so the gain stays with statements whose rows are cheap and whose
invariants are not (the micro-benchmark above). Kept for those; off with the switch where a comparison needs it.
The differential check over the frames, the randomized check under three seeds, frame bytes, the four oracle
suites, the lane / mixed / decimal checks and the edge sets are identical to the build before.

Keys held as dictionaries: ui_static_pixels.x is a DECIMAL(65,0) a CREATE TABLE AS inferred from integer
arithmetic, kept as a dictionary, so the ui join probed it row by row, formatting each decimal (7.8 ms a frame
over 10,240 rows). A dictionary-coded number column - INT or DECIMAL(65,0) declared, or a result table's numeric
text - now joins through its entries read once as numbers (`dict_numbers`, one double per code, kept per column
and dictionary epoch as the star code maps are; refused where an entry is not a whole number a double holds
exactly, and the row path decides as before), on the base side through `numkey` and on the joined side in
`other_keys_match`. The ui join is batched (1.0 ms), 118 -> 187 batched joins a run. An edge set (DECIMAL and
INT dictionaries either side, a text dictionary mixing numbers and words left to the row path, NULL keys, LEFT
JOIN padding, a single and a two-key join) is identical to the build before, as are frame bytes, the differential
native check, the oracle suites, the lane checks and the earlier edge sets.

### ERP shapes against DuckDB

A synthetic ERP set (200k orders, 600k order lines, 500k ledger rows, 150k shipments, 30k products, 20k
customers; the generator, data, query set and both engines' logs live beside the session's scratch files) put
ASQL against DuckDB 2.0 on identical data, every result compared. Ahead: point lookups (11x), NOT EXISTS (6x),
IN subqueries (1.8x), GROUP BY .. HAVING (1.2x). Behind, 6-25x: a grouped join whose ORDER BY is its own
(ORDER BY SUM(..) DESC), a grouped join over text keys, GROUP BY YEAR(), MONTH(), SUM(CASE .. THEN decimal),
a ROW_NUMBER window filtered through a derived table.

The first of these: the lane GROUP BY (lane_group_by) took only integer keys, integer arguments and the keys'
implicit order. It now takes a dictionary-coded text key by its entries' collation classes (dict_classes: two
entries share a class exactly when GROUP BY finds them equal; the group's value is its first row's cell, as the
row path gives it), double arguments for SUM / AVG / MIN / MAX (the same accumulator as the row path, the
extreme row's value), and a statement's own ORDER BY whenever each of its keys is an output item (an alias, an
ordinal, the item's expression): the groups' rows carry sort keys from their cells and the statement's sort
orders them. A plain column item is read from the group's first row's cell and an aggregate item from its
folded value, no interpreter per group. Groups the caller sorts stay in first-appearance order, so rows that
tie come out as the row path's stable sort has them (the cteq oracle caught the key-ordered version on a tie).
ERP: open orders 3-way join 257 -> 172 ms, top customers 90d 137 -> 57 ms, category stock LEFT JOIN 18 -> 17
(DuckDB 34 / 11 / 7); the star-columnar grouped join (line sales by location) is unchanged, its path is next.
A GROUP BY edge set (text keys differing by case and trailing spaces, NULL keys, ORDER BY by alias, ordinal,
expression and a key not selected, HAVING, doubles under MIN / MAX / SUM / AVG, LEFT JOIN groups, LIMIT and
OFFSET) is identical to the build before, as are frame bytes, the four oracle suites, the lane checks and the
earlier edge sets.

The window shape (the latest order per customer: ROW_NUMBER() OVER (PARTITION BY custid ORDER BY orddate DESC)
inside a derived table, WHERE rn = 1): 482 ms against DuckDB's 19. The columnar window path (columnar_window)
was built for exactly this statement but refused it, because an ORDER BY key held as dates (TemporalDays) was
neither a typed number nor a dictionary to it; a date's order is its day number's, so it is a key now (the
refusal sites are numbered in the trace: "columnar window: not taken (n)"). Standalone the statement then took
47 ms instead of 412. As a derived table it still made 200,000 text rows for the outer select to parse back; the
columnar window now fills the consumer's typed batch itself (the kept rows' columns gathered from the store by
row id, the windows as integers, the result columns typed as the table's) when one is asked for: 525 -> 128 ms.
What is left there is the DECIMAL column, which crosses as text both ways (formatted on the way out, parsed on
the way in, ~60 ms of the 128); a decimal batch kind is the next step on this shape. A window edge set (ROW_NUMBER /
RANK / DENSE_RANK / COUNT partitioned by a number or a text, ordered by a date, a datetime, a decimal with NULLs,
ascending and descending, filtered through a derived table, with and without LIMIT) is identical to the build
before, as are frame bytes, the four oracle suites, the lane checks and the earlier edge sets.

SUM(CASE WHEN cond THEN col ELSE 0 END), the ERP report's staple: the single-table path knew the constant form
(THEN 1 ELSE 0, per dictionary code) and took the column form row by row through the interpreter (47 ms over
200k orders; the same sum as SUM(col) WHERE cond: 1.3 ms). It is now the column summed over the rows cond selects
(select_rows, as a WHERE would; a mask per table row), with ELSE 0 counting the other rows as zeros so a sum of
none is 0, not NULL, and prints at the column's scale as the row path's does (the edge set caught "0" for
"0.00" on a DECIMAL column first). The same mask serves the grouped statement (GROUP BY st). ELSE anything else,
AVG and COUNT keep their paths. 34 -> 3.7 ms (DuckDB 1.9). The grouped SUM of a DOUBLE column has a kernel
of its own beside the per-row accumulator; it stood aside for a masked aggregate only after the edge set,
run again after the next change, came back with the unmasked sum - the single-table path's choices shift with
what is warm, so an edge set is rerun with every step, not once. A SUM(CASE) edge set (DECIMAL, INT and DOUBLE columns
with NULLs, ELSE 0 / ELSE NULL / no ELSE, no row matching, compound and text and date conditions, GROUP BY, a WHERE
beside it, ELSE 1 and ELSE col and AVG and COUNT left alone, 0.0 and 0e0) is identical to the build before, as are
frame bytes, the four oracle suites, the lane checks and the earlier edge sets.

GROUP BY YEAR(orddate), MONTH(orddate) (78 ms; DuckDB 3.3) and GROUP BY orddate (31 ms) went row by row: a date
column held as days past a base (TemporalDays, TemporalSeconds) was neither a typed number nor a dictionary to
the single-table path, so its keys, and every expression of it, were evaluated per row. Its codes are a frame
of reference like a narrow FOR column's: up to 65,536 of them (180 years of days) now read per code
(code_space, make_source), the value per day worked out once - 730 evaluations of YEAR() for 200k orders - and
the bare column itself a per-code source, which also gives MIN / MAX / COUNT(DISTINCT) of a date their per-code
path. Monthly totals 78 -> 3.0 ms (DuckDB 3.3), GROUP BY orddate 31 -> 1.2 ms. A date-key edge set (GROUP BY a date, by
YEAR / MONTH / QUARTER / DAYOFWEEK / DATE_FORMAT / DATEDIFF of it, by DATE() and HOUR() of a datetime, NULL dates,
MIN / MAX / COUNT(DISTINCT) of dates and datetimes, a WHERE range beside the GROUP BY, GROUP BY an alias) is
identical to the build before, as are frame bytes, the four oracle suites, the lane checks and the earlier edge
sets.

The grouped join's remaining cost was the lane GROUP BY's own output: 81,000 groups each made into text cells,
the SUM parsed back out of its text for a sort key, the whole set stable-sorted through compare_vals_as, and
every group's first-row name decoded from a front-coded dictionary (a profile: to_chars, view_front_coded,
val_num and the merge sort were most of it). lane_group_by now sorts the groups itself where the statement's
ORDER BY decides: an aggregate key by its value (compare_vals_as, or the double alone where every value is a
plain number below 2^53 - a COUNT, a DOUBLE sum), a dictionary text key by its collation rank (dict_order_rank,
built once per column), a typed number column by its values, anything else by the Val the interpreter gives
the group's first row; a partial sort to offset + limit where that is a small part of the groups, with ties
broken by position as the stable sort breaks them; and only the rows kept are printed (LIMIT 20 of 81,000).
Two more costs went with it: a sum over an integer or DOUBLE lane adds the value itself (SumAcc::add_i64 /
add_approx, the accumulator's own arithmetic without a Val a row: val_exact formatted and parsed each one),
and a dictionary or integer key column that is not far larger than the row set is decoded whole once instead
of a code read per row (read_code at ~370 instructions each). ERP: open orders 3-way join 172 -> 104 ms, top
customers 90d 57 -> 44 (DuckDB 34 / 11); what is left there is the join itself (36-44 ms of the 104) and the
group hashing. The group edge set and the rest are identical to the build before. A profile of the join phase
put a third of its instructions in the output lists' growth by doubling (the moves of 257k rows); the lists are
reserved ahead now (a row per base row and half as many again), which the instruction count rewards and the
clock does not - the probe's time is in the index lookups and the scattered candidate lists, not in the copies.

The decimal batch kind. A DECIMAL column (and a DOUBLE column the encoder holds scaled, DecimalScaled) crossed
between statements as text: formatted cell by cell on the way out of the columnar window (45 ms of the 128), parsed
and interned on the way into the derived table (17 ms). ColBatch::Dec carries the scaled integers with their places
and whether every place prints (a DECIMAL(p,s)) or trailing zeros drop (a DOUBLE held scaled), printing exactly as
ColumnStore::get prints the cell (scaled_decimal_cell, now the one rule for both); colbatch_gather fills it from
the column in a block, and colbatch_fill_table adopts it straight into a DecimalScaled column of the same scale
(ColumnStore::adopt_scaled, the codes packed as the encoder packs them). The window shape: 128 -> 47 ms (DuckDB
19). One answer changed on the way, in the previous step rather than this one, and the decimal edge set made it
visible: SUM of a DECIMAL column through the columnar window's derived table is the exact decimal (7856.05) where
the text-typed derived table gave the double (7856.049999999999) - the typed answer is MariaDB's, and the
text-inferred one is the general window path's remaining gap. A decimal edge set (DECIMAL(10,2), DECIMAL(18,6)
and a DOUBLE held scaled through derived tables and CTEs: sums, averages, extremes, filters, equality with 0,
negatives, NULLs, a join of two derived tables, GROUP BY the decimal, CAST to CHAR, the window shape) is
otherwise identical to the build before, as are frame bytes, the four oracle suites, the lane checks and the
earlier edge sets.

The star-shaped grouped join (line sales by location: 600k order lines to their products, GROUP BY category,
location): 73 ms against DuckDB's 11, in four pieces the trace names - the base selection (14), the unique-key
lookup of every line's product (12-17), the group keys (23) and the sum (15). Three were a read per row through
read_code (a code or integer out of a bit-packed column, ~370 instructions each): the lookup now decodes the
line's product-id column whole once (star_join: Link::whole_ints / whole_codes where the rows cover an eighth of
the column), the star aggregate's key columns likewise (code_of / int_of), and gather_for decodes a run of
consecutive rows as a block. The group-id table was sized to the rows (2M slots for 480 groups) and missed the
cache on every probe; it starts at 1,024 slots and doubles as groups appear - and hashes by the product's top
bits, since its middle bits (>> 20, fine spread over 2M slots) clustered a small table into 87 ms of probing.
An integer SUM with no NULL adds straight into its group. Keys 23 -> 11 ms, aggregates 15 -> 7, the lookup
unchanged at ~17 (the row loop's own work now); 73 -> 61 ms. The base selection's 14 ms were base_rows making a
JoinedRow of every one of the 600k fact rows because the statement had a LIMIT: the row-id form was kept for
statements without one, though a join's base cannot stop early at a LIMIT (the early stop is the single table's);
every joined base of 256 rows or more is ids now. 61 -> 31-37 ms (DuckDB 11); what is left is the lookup loop
(13-16 ms over 600k rows) and the group keys (11).

Two more aggregate shapes. COUNT(DISTINCT custid) over 150k selected rows put every value into a hash set
(6.6 ms); a typed integer column whose zone maps bound its values to a span under 2^24 counts into a bitset of
the span per group (the code path COUNT(DISTINCT) over dictionary classes already had): 7.5 -> 1.2 ms (DuckDB
3.6). The star join's key map (build) read each joined row's key through read_code and filled a hash table
before deciding the map's form; integer keys are read in a block where the rows cover an eighth of the column
and their range decides first - a span of a few million values is an array filled straight from the keys,
uniqueness checked in it. The rerooted join's order (by base row, then line) is a radix sort of packed keys
instead of std::sort through a lambda. The order-value-range shape (30k orders of a date range to their 92k
lines) is 32 -> 26 ms, in three pieces the trace now names: the index lookup of the lines by 30k keys (9.5 ms:
rows_equal_to_sorted gallops the index once per key), the lookup loop (5.4) and the order (4.4). A
COUNT(DISTINCT) edge set (NULLs, negative values, BIGINTs past the span, several groups, filters that leave
no row, a date GROUP BY) is identical to the build before.

The lane GROUP BY still made a Val of every aggregate for every group (nt_adjust and the exact-sum finish, 81,000
times twice, to print 20 rows): a profile of the function put a quarter of it there. The Val is made where one
is wanted - a HAVING, an item printed, a sort key that is not a plain number; a sort key that is a count, a sum
or average of DOUBLE lanes, or the extreme of a numeric lane below 2^53 reads its double straight from the
accumulators (agg_num), the same double compare_vals_as would compare. Open orders' group phase 60 -> 43-50 ms.

### Profiles with line numbers; the next set

A second build tree (build-prof: -O3 -g, ignored by git) gives callgrind source lines at full optimization; the
wall clock in this container swings 2x between identical runs, so the steps below are judged by instruction
counts first and the clock second.

- Single-table GROUP BY by slot (inventory top items: 500k ledger rows, 30k products, SUM, ORDER BY the sum
  LIMIT 10): every group's integer SUM was printed and parsed back (scaled_to_decimal built its digits by
  inserting at the front of a string; then parse_number), and the integer key was read per row through
  read_code. scaled_to_decimal now writes its digits backward into a buffer; an integer SUM below 2^53 is its
  number and std::to_string; the key is decoded a morsel at a time; a SUM / AVG of a typed INT column folds
  straight into isum and cnt. 75 -> 43 million instructions a statement. The trace names the path's stages
  (aggregate: groups by slot - scan, merge, values).
- ORDER BY .. LIMIT over a derived table's typed column (the latest-order-per-customer shape's outer select):
  the "keys as numbers" pass refused any result table; a result table's column that was filled typed
  (TypedInt, TypedDouble, DecimalScaled) holds canonical values and is read as the numbers it prints as.
  Emit 14 -> 0.5 ms.
- The columnar window's number keys and colbatch_gather's integer columns decode in a block where the rows
  cover a fair part of the column (a read per row for three keys over 200k rows).

A SUM-format edge set (INT, BIGINT past 2^53, DECIMAL(12,3) and DECIMAL(30,10) sums and averages, negatives,
NULLs, zeros at scale, groups ordered by the sum) is identical to the build before, as are frame bytes, the four
oracle suites, the lane checks and the earlier edge sets.

The date-range join (30k orders of two months to their 92k lines, SUM(price * qty)): 24-26 ms warm, against
DuckDB's 4.3. The profile named four pieces: the SUM's column arithmetic over the joined rows read each leaf
through read_code (np_eval over a row list: 18.6M instructions), the 30k order keys and the 92k matched line
ids sorted with std::sort, the 600k-row line key column decoded whole for 92k lookups, and the rerooted rows'
order radix-sorted over 64 bits. Now: a row list that is ascending and packed into a span under eight times
its length (a batch of a join's partners) decodes the span as a block and picks its rows; the order keys skip
the sort when the base rows already hold them ascending; the matched ids sort by a 16-bit-digit radix sort
(radix_sort_u32); the whole-column decode of a link's key waits until the rows read a third of the column; the
order's keys pack to the ids' widths (38 bits, four passes). 24-26 -> 11-13 ms warm. (A cold first statement
also builds the line table's column index, ~90 ms, as before.) A rerooted-join edge set (NULL keys, NULL
values, a single date, an empty range, GROUP BY over the join, LIMIT with and without ORDER BY) is identical
to the build before, as are frame bytes, the four oracle suites, the lane checks and the earlier edge sets.

Two join-probe costs. A 1:N step (85k orders to their 257k lines) reserved each chunk's output list for 1.5 rows
a base row and grew it by doubling past that; the reserve now follows the probing index's own rows per key
(the joined table's rows over its distinct keys, bounded 1.5-16): the step's list growth 41 -> 28M instructions,
the step 133 -> 119M. And a LEFT JOIN whose ON clause carries the joined table's own condition (`ON
s.productid = p.id AND s.onhand > 0`) selected the table's rows by it (js_pass) and then evaluated it again over
every candidate in lanes; where js_pass holds the ON's own conditions alone (no WHERE part folded in), it is now
read before a candidate counts as a match and those conjuncts leave the per-candidate checks. The category-stock
probe 13.8 -> 8.4 ms (its ON checks 4.5 -> 0). An edge set for ON clauses with own conditions (LEFT, INNER and
RIGHT joins; own conditions alone and with a base-row conjunct; a WHERE on the joined table, on IS NULL, and
both; selections empty, small enough to key the index by, and too large for that but prefiltered; NULL values)
is identical to the build before, as are frame bytes, the differential native check, the four oracle suites, the
lane checks and the earlier edge sets.


Vectorized operators stay the default; a pipeline the plan marks as hot - many rows through a chain of scan,
filter, join probe and aggregate whose expressions already compile as programs - is compiled to native code
(reusing the Grain JITs' emitters: x86-64 and AArch64) and run in place of the operators, with the same answers or
a fall-back to them. Compilation is adaptive, never for a statement that runs once in microseconds. The design,
staging and measurements (against DuckDB on identical data: today 2.7x behind on grouping, 1.3-1.5x on an anti-join)
come when stage 2 is done; stage 2 is its prerequisite, since a compiler needs a plan to compile from.

## Order

1a, 1b, then 2a (the plan makes 1c-1e and every later change easier to place and to check), 2b/2c phase by phase,
with 1c-1e done inside the operators they belong to. Each sub-stage is its own commit with its measurements.
