# ASQL: splitting sql_database.cpp

`src/userland/sql_database.cpp` was one 60,000-line translation unit: the parser, the evaluator, the single-table
(vx) and join engines, the column store, writes, DDL, replication and the store on disk. Any edit to any of it
recompiled all of it (about five minutes on this machine at -O3) and every reader had to find their way through
all of it. This note gives the plan for splitting it into files by subsystem, and the rules that make each step a
pure move with no change in behaviour.

## Rules for every step

- **Code moves; it is not rewritten.** A step's check: every line of the original is in exactly one of the new
  files (a sorted-line diff of the old file against the new ones shows only the added glue).
- **Shared helpers keep one definition.** `src/userland/sql_engine_shared.hpp` (engine-internal, namespace
  `aios::userland::sqle`) declares what more than one engine file uses:
  - trivial helpers and constants (`store_get`, `kSegRows`, ...) are *defined* there, and `sql_database.cpp` takes
    them with a using-declaration where its own copy was;
  - everything else stays defined where it was, in `sql_database.cpp`'s file-local namespaces, and is exported by
    a one-line `sqle::` function at that file's end. Calls inside `sql_database.cpp` still go straight to the
    file-local definition; no call on a query or write path crosses files.
- **A move is not codegen-neutral, so each step is measured.** GCC's inlining budget is per translation unit:
  taking code out of `sql_database.cpp` changes what it inlines in what is left. Each step compares, against an
  object built from the file before the step (all else equal): per-function code sizes, callgrind instructions per
  write statement, and SQLDoom frame times, alternating.
- **Global and thread-local state is never duplicated.** A `static`/`thread_local` that two files need gets one
  definition and an `extern` declaration in the shared header, never a copy in each.
- **Each step is checked by** the SQL test set (`ctest -R "asql|aql|sql"`), the SQLDoom frame's bytes, the
  authority gate and `gen-reference.py --check`.

## Steps

| Step | Moves to | What | Needs from the rest | State |
|---|---|---|---|---|
| 1 | `sql_storage.cpp` | The store on disk: the column-image format (MANIFEST, generation files, parts of later writes), the row-per-cell formats, views, the replication position | ~15 helpers (store codec, segment constants, zone bounds, `for_each_parallel`) | done |
| 2 | `sql_replication.cpp` | Seeding from a MySQL source, schema repair, binlog sync controls | to be listed by the compiler | |
| 3 | `sql_ddl.cpp` | CREATE/ALTER/DROP, SHOW, table design | parser types | |
| 4 | `sql_write.cpp` | INSERT/UPDATE/DELETE, foreign keys, row rules | `Val`, the evaluator's entry points | |
| 5 | `sql_parser.cpp` + `sql_ast.hpp` | Tokenizer, `SqlExpr`, `SqlParser`, rewrites (ROLLUP, derived-table merging) | the AST types move into an engine-internal header first | |
| 6 | `sql_eval.cpp`, `sql_select.cpp`, `sql_vx.cpp`, `sql_columns.cpp` | The evaluator, the general (join) select engine, the single-table engine, the column store | `Val`, `EvalCtx`, `JoinedRow` and the column readers in internal headers | |

Steps 1-4 move `SqlDatabase` members, which the public header already declares, so they need only the helpers
they call. Steps 5-6 move types that the whole engine uses (`Val`, `SqlExpr`, `EvalCtx`). Each of those first gets
a design pass of its own: which types go into internal headers, and which hot calls must stay inlinable within one
file (the evaluator's per-row calls must not become calls between files).

## Step 1, measured

- `sql_storage.cpp`: 1,890 lines; compiles alone in ~11 s, so an edit to it no longer rebuilds the 58,000 lines
  left in `sql_database.cpp`.
- The sorted-line diff of the old file against the two new ones shows only the include block, the export
  wrappers, and the definitions now in `sql_engine_shared.hpp` (each replaced by a using-declaration).
- Object code of what is left in `sql_database.cpp`: 4,907 of 5,077 functions the same size; 170 changed, mostly
  library template instances (sorts, vector growth) and copy/assign members whose inlining moved, a few on query
  paths (`JoinedParts::push_back` 310 -> 648 bytes and `DictArena::operator[]` 149 -> 338: more inlined into them). Measured effect:
  - callgrind instructions, write bench (20,000 rows, 4,000 statements): `write_txn` 546,983 -> 545,430 per
    transaction (-0.3%), `update_index` 82,856 -> 82,697 per statement (-0.2%), the load -0.3%;
  - SQLDoom E1M1, median of frames 2-10, three alternating rounds: 816 / 882 / 875 ms before,
    847 / 868 / 844 ms after - the same within the run-to-run spread.
- Kept in `sql_database.cpp` for now: `load_tables` (its view reload uses the parser) and `save_index_list` (it
  reads the vx engine's column indexes under `StatementLock`). They move with steps 5-6.
