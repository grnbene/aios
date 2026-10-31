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

## Order

1a, 1b, then 2a (the plan makes 1c-1e and every later change easier to place and to check), 2b/2c phase by phase,
with 1c-1e done inside the operators they belong to. Each sub-stage is its own commit with its measurements.
