# Design: one ASQL query across fleet machines

Status: **design only, nothing built.** Written 2026-10-23.

Today one `SELECT` runs on one machine. ASQL's parallelism is `SqlThreadPool`
over one process's cores (`sql_database.hpp:491`). The fleet can send a whole
statement to another member (`sql_on`, `… ON <machine>`, `SHOW … ON ALL`),
but nothing splits one query and merges the parts. This page designs that,
taking `sat fleet` (`cli/sat_cmd.cpp`) as the model.

---

## What `sat fleet` gives us to copy

| `sat fleet` | Here |
|---|---|
| `on=SEL,SEL,…`: the operator names every worker, nothing implicit | `on=` names every machine; no default pool |
| One worker thread per selector, one shared queue, `depth = log2(4 × workers)` cubes up front | One thread per machine, a queue of more slices than machines, so a fast machine takes more |
| A cube that runs out of budget is split and re-queued | A slice that fails or times out is re-queued for another machine (every replica can run any slice) |
| Nothing taken on a worker's word: models checked here, proofs checked on a **different** machine | Every reply carries its snapshot position; one slice in four is re-run on a different machine and must match exactly |
| A ledger of runs and cubes in SQL, receipts per leaf | A ledger of runs and slices, and which machine ran and checked each one |
| Workers run an existing kernel (`sat.solve`) | Workers run the **existing engine** on a rewritten statement; no second executor |

---

## The case it is for: replicas of one source

The production shape is `compute serve sql=1` / `asql-serve` with binlog CDC
from one MariaDB (`docs/asql.md`, "Replicating a MariaDB"). Every such member
already holds **every row of every whitelisted table**. That decides most of
the design:

- **No data moves.** A slice is a predicate, not a copy of the rows.
- **Joins need no shuffle.** Each worker joins its slice of one table (the
  *driving* table) against its own full copies of the others.
- **Any machine can run any slice**, so failure and straggler handling is just
  "give it to someone else", as with cubes.

A second case, where each machine holds *different* rows (per-site data), is
at the end. It reuses everything except the slicing and the fence.

---

## 1. Slicing: by primary key, not by row position

Row ids are positions in `col_data`. They shift on `DELETE` (rows are
compacted, `erase_rows` `sql_database.cpp:19150`) and differ between mirrors,
since each mirror applied the stream in its own physical layout. A positional
range on machine A is not the same rows on machine B. So slices are **ranges of
the driving table's primary key**:

- The coordinator is itself a mirror. It reads the PK column and picks
  `W × 4` split points (quantiles), so slices have about equal row counts.
- A slice is `pk >= a AND pk < b`, ANDed onto the driving table's WHERE.
  Composite keys compare as tuples on the leading column, then the rest.
- **Cost caveat:** `PkMap` is a hash (`sql_database.hpp:1129`), so there is no
  ordered range scan. Pruning comes from the PK column's zone maps (min/max per
  2048-row segment, `segment_may_match`). An auto-increment table loaded in
  order prunes almost perfectly. A table with random keys still makes every
  worker test the PK predicate over the whole column, but only once, cheaply;
  the expensive filters, joins and aggregates run on its slice only. Phase 1
  measures this. If it is too weak, the fallback is an ordered PK side-index
  built per table on first use.
- A table with no primary key is not sliced (the query runs on one machine and
  says why).

## 2. Consistency: every slice reads the same binlog position

Mirrors lag differently. A count split over two machines 200 transactions apart
is wrong, and wrong silently. So a fleet query runs behind a **fence**:

1. The coordinator asks each chosen member for its applied position
   (`SHOW REPLICA STATUS`) and source identity (source server UUID plus
   database). Members of different sources are refused up front.
2. It picks `P` = the furthest applied position among them.
3. Each member **applies up to exactly `P` and holds** there, the way
   `START REPLICA UNTIL SOURCE_LOG_POS` stops, at a transaction boundary
   (Xid). A member already past `P`: the coordinator raises `P` to that member's
   position and the others catch up to it. Positions only move forward, so
   this converges in one or two rounds.
4. Slices run. CDC apply already takes the exclusive statement lock
   (`docs/asql.md`, Transactions), so nothing changes under a read anyway; the
   fence makes that hold across **all** of a query's slices, not one.
5. The coordinator releases the fence; the members catch up.

Every slice reply carries the position it read at. A reply at any other
position is rejected, never merged. The hold is bounded (`fence_ms=`, default
a few seconds); a member that cannot reach `P` in time is dropped from this run
and its slices go to the others.

The cost is replication lag on those members for the length of the query. That
is the honest price of a consistent answer, and the reason this is only for
queries long enough to be worth it.

A member that is not a CDC mirror (local writes only) has no position. It can
take part only in the "different rows on each machine" mode at the end.

## 3. What gets distributed: one rewrite, two statements

The parser already yields an AST (`ParsedSelectStatement`,
`sql_database.cpp:3257`). There is no plan object, and building one is not
needed. The coordinator inspects the AST and either produces two SQL
statements, or refuses with the reason:

- a **worker statement**, run on each slice;
- a **final statement**, run on the coordinator over the gathered partial rows,
  which it loads into an ephemeral table the way CTEs already are
  (`execute_with`, `sql_database.cpp:45274`).

Both run on the unchanged engine. That is the `sat fleet` principle (workers
run the existing kernel) and it keeps one executor to trust.

### Shapes accepted in v1

| Query shape | Worker statement | Final statement |
|---|---|---|
| `SELECT cols FROM t WHERE …` (no aggregates) | same + slice | `UNION ALL` of slices, re-ordered by PK if the engine's single-machine order is PK order (see open questions) |
| `… GROUP BY g` with `COUNT`, `COUNT(*)`, integer/DECIMAL `SUM`, `MIN`, `MAX` | `SELECT g, COUNT(x), SUM(x), MIN(x), MAX(x) … WHERE <slice> GROUP BY g` | `SELECT g, SUM(c), SUM(s), MIN(m), MAX(M) FROM partials GROUP BY g` + the original HAVING / ORDER BY / LIMIT |
| `AVG(x)` on integer/DECIMAL | ships `SUM(x), COUNT(x)` | `SUM(s) / SUM(c)` at the column's declared scale |
| `COUNT(DISTINCT x)` | `SELECT DISTINCT g, x` | `COUNT(DISTINCT x) … GROUP BY g` (bounded: refused above a row cap, see below) |
| `ORDER BY k LIMIT n OFFSET m` | same + `LIMIT n + m`, no offset | merge, then `ORDER BY k LIMIT n OFFSET m` |
| Inner / left joins with the sliced table as the left-most (driving) table | the whole join + slice on the driving table | as above for whatever sits on top |

The driving table is the largest base table in FROM after WHERE estimates. The
engine joins left-deep in FROM order (`sql_database.cpp:41560-41621` is the
only reorder), so the rewrite puts the sliced table first, which inner joins
allow. Left joins allow it only when the sliced table is already the
preserved (left) side.

### Refused in v1, with the reason printed, never approximated

- **Float `SUM` / `AVG` / `STDDEV` / `VARIANCE`.** The engine sums floats in
  row order on purpose (`AggMode::Ser`, `sql_database.cpp:34205-34218`), so the
  last bits match MariaDB. Partial sums added in another order would not. A
  later opt-in (`exact=off`) could allow it; the default stays exact.
- `GROUP_CONCAT`, `JSON_ARRAYAGG`, `JSON_OBJECTAGG`, `BIT_*` aggregates:
  order-dependent output.
- Window functions, `WITH RECURSIVE`, correlated subqueries that read the
  sliced table, `RIGHT` / `FULL` joins where the sliced table is not the preserved
  side, and `UNION` / `INTERSECT` / `EXCEPT` across branches. Each branch of a
  union could later be distributed on its own.
- Anything that writes.

"Refused" means: the command says `not distributed: <reason>` and does not run
it, unless the caller asked for `else=local`. Then it runs on this machine and
the output says so on its first line. Nothing silently runs somewhere other than
where the operator asked.

### Partial results on the wire

`SqlQueryResult` travels as strings in one frame (`encode_result`,
`machine_sql.cpp`), with a 256 MiB cap (`nodeproto::MAX_PAYLOAD`). Aggregate
partials are small: one row per group per slice. Integers and DECIMALs as
strings are exact. Two guards:

- a per-slice result cap (`max_slice_rows=`), past which the run fails with the
  slice and its size named. This mainly catches `COUNT(DISTINCT)` and
  non-aggregate scans that select most of a table;
- the final ephemeral table is built through the same path CTEs use, so its
  column types come from the worker's `column_types`, not re-guessed.

Plain scans that return most of a big table gain nothing from this. Shipping
the rows costs more than scanning them. The design accepts them only so that
selective filters over big tables benefit.

## 4. The fan-out primitive

`sql_on` and `SHOW … ON ALL` ask members **one after another** under one
global lock (`g_fleet_round_mu`, `machine_sql.cpp`). A fleet query needs the
`sat fleet` loop instead: one thread per member, a shared queue, and
`in_flight` accounting.

A new node opcode, rather than SQL text tricks over `Sql`:

```
SqlSlice = 19        (feature bit kFeatSqlSlice in the handshake, as ExecBatch)
request:  run id, slice id, worker statement, fence position P, deadline ms
reply:    position read at, encoded result, rows scanned, kernel ms
SqlFence = 20
request:  run id, action (hold-at P | release), max hold ms
reply:    position now held, source identity
```

A member without the feature bit is not offered slices (`fleet status` says
so), like a peer without `ExecBatch` today.

**Who may do this.** A peer can already run any `SELECT` on a member. The new
risk is the fence: holding replication is a lever on someone else's machine.
So a member serves `SqlSlice` / `SqlFence` only after its operator runs
`fleet sql pool=on` (kept in `<root>/fleet_sql.conf` beside `writes=`). The
hold is capped by the member's own `max_hold_ms`, whatever the request asks,
and released automatically when the run's link drops.

## 5. Verification: nothing taken on a worker's word

- Every reply's position must equal the fence position.
- **Cross-checks.** One slice in four (as `sat fleet`'s "one proof in four
  comes back") is re-run on a different machine. The two partial results must
  be identical byte for byte. A mismatch fails the run and names both machines
  and the slice. That is mirror drift, and `SHOW ASQL MIRROR` on the pair is
  the next step. This also catches a member running a different build.
- The coordinator checks that the slices' row counts add up to the driving
  table's count at `P` on its own copy.
- **Ledger.** Like `sat`'s ledger: a `runs` row (query, machines, `P`, wall
  time, result fingerprint) and a `slices` row each (range, machine, rows,
  ms, cross-checked by whom). This is what you read after a wrong answer.

## 6. How it is invoked

Same shape as `sat fleet`:

```
sql fleet "<SELECT …>" on=cpu,remote:relay:09223501ef31,remote:relay:7d125b44f246 [slices=N] [fence_ms=N] [else=local] [explain]
```

- `on=` is required and literal. There is no "all members" default, so a
  production box (aat) is never pulled in unless named.
- `explain` prints the real decision and runs nothing: driving table, split
  points, worker and final statements, the fence position, or the reason it
  refuses. Today's `EXPLAIN` prints fixed text (`sql_database.cpp:48531-48547`,
  including a "Radix Hash Join" the engine does not have), so this is new and
  separate.
- grain: `sql_fleet(stmt, machines)` returns rows the way `sql_on` does.
- **Later, not v1:** a MySQL-wire hint (`SELECT /*+ FLEET(…) */ …`) or a
  session variable, so the ERP's own clients can use it. That needs the member
  list in server config, and it waits until the CLI form has proven itself.

## 7. Build order (each phase verified end to end before the next)

1. **Parallel fan-out + `SqlSlice` with no fence and no rewrite.** Run the same
   full statement on N members at once and get N identical results back.
   Measure the per-slice round-trip cost over the relay; that sets the
   smallest query worth distributing.
2. **PK slicing on one machine.** Split a query into `slices=N` ranges on
   **this** machine only, merge, and compare with the unsplit result over the
   benchmark corpus (`docs/asql-benchmarks.md`). This proves the rewrite and
   merge with no network involved.
3. **The fence.** Hold and release on real CDC mirrors; check that held
   positions match and that replication catches up after release.
4. **Fleet run.** Phases 1 to 3 together, plus cross-checks and the ledger.
5. **Correctness bar:** for every corpus query that is distributed, the result
   is identical to the single-machine result at the same position, and matches
   the MariaDB oracle on devops (TEMPORARY tables only). Speed is measured
   on devops and the other non-production servers, never aat.

## Open questions to settle in phase 2

- **Row order without `ORDER BY`.** Single-machine output follows the engine's
  physical row order, and the vectorized group-by numbers groups by first row
  (`sql_database.cpp:35148-35162`). Merged slices are in PK order. Either the
  fleet result is defined as "PK order" (documented as a difference), or a
  query without `ORDER BY` whose single-machine order is not PK order is
  refused. Check against the MariaDB oracle what clients actually rely on.
- **Ties under `ORDER BY … LIMIT`.** Which row wins a tie at the cut-off
  depends on physical order. The same choice as above applies.
- **How big must a query be?** Phase 1's round-trip number, plus the fence
  hold, gives the floor. Below it the command should say "faster here" and,
  with `else=local`, just run it here.

## Different rows on each machine (later)

When each member holds its own rows (per-site tables, not mirrors), a slice is
"all of your rows", there is no fence (each machine's answer is consistent
with itself), and the cross-check is skipped (no other machine can recompute
it). The worker/final rewrite, the opcode, the fan-out and the ledger are the
same. This is `SHOW … ON ALL` extended to `SELECT` with real merging, and it
comes after the replica mode works.
