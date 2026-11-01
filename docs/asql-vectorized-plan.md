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

Vectorized operators stay the default; a pipeline the plan marks as hot - many rows through a chain of scan,
filter, join probe and aggregate whose expressions already compile as programs - is compiled to native code
(reusing the Grain JITs' emitters: x86-64 and AArch64) and run in place of the operators, with the same answers or
a fall-back to them. Compilation is adaptive, never for a statement that runs once in microseconds. The design,
staging and measurements (against DuckDB on identical data: today 2.7x behind on grouping, 1.3-1.5x on an anti-join)
come when stage 2 is done; stage 2 is its prerequisite, since a compiler needs a plan to compile from.

## Order

1a, 1b, then 2a (the plan makes 1c-1e and every later change easier to place and to check), 2b/2c phase by phase,
with 1c-1e done inside the operators they belong to. Each sub-stage is its own commit with its measurements.
