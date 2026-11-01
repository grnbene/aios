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


Vectorized operators stay the default; a pipeline the plan marks as hot - many rows through a chain of scan,
filter, join probe and aggregate whose expressions already compile as programs - is compiled to native code
(reusing the Grain JITs' emitters: x86-64 and AArch64) and run in place of the operators, with the same answers or
a fall-back to them. Compilation is adaptive, never for a statement that runs once in microseconds. The design,
staging and measurements (against DuckDB on identical data: today 2.7x behind on grouping, 1.3-1.5x on an anti-join)
come when stage 2 is done; stage 2 is its prerequisite, since a compiler needs a plan to compile from.

## Order

1a, 1b, then 2a (the plan makes 1c-1e and every later change easier to place and to check), 2b/2c phase by phase,
with 1c-1e done inside the operators they belong to. Each sub-stage is its own commit with its measurements.
