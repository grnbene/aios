# Things: A Practical Guide

A **Thing** is a query whose result ASQL maintains incrementally. You declare the query once; whenever underlying table data changes, the engine identifies the affected keys and updates only those records. Applications read the live results using `SHOW THING`, ensuring fresh data without repeated aggregate queries or external cache invalidation.

In database terms, a Thing is an **incrementally maintained materialized view** (IVM). Unlike materialized views that are refreshed whole on demand, ASQL hooks table mutations and recomputes only the partitions a write touched. (One exception: a change to a table the Thing joins without its partition column recomputes the whole Thing - see "When to Avoid".)

For complete grammar specifications and engine internals, see [asql.md](asql.md#things).

---

## Example Schema

This guide follows a warehouse order fulfillment example:

```sql
CREATE TABLE lines (
    id INT PRIMARY KEY,
    orderno INT,
    wh VARCHAR(4),
    qty INT,
    picked INT,
    price DECIMAL(10,2)
);
```

Each row represents an order line item: item quantity ordered, quantity picked so far, and unit price.

---

## 1. Creating and Querying a Thing

Define a Thing to track live order summaries across lines:

```sql
CREATE THING orders KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, COUNT(*) AS line_count, SUM(qty) AS qty, SUM(picked) AS picked, SUM(qty * price) AS total
     FROM lines GROUP BY orderno;
```

Key clauses:
- **`KEY orderno`**: Defines the unique identifier for each Thing instance.
- **`PARTITION BY orderno`**: Specifies the partition boundary for change tracking (defaults to the key). When `KEY` and `PARTITION BY` match, it is a row Thing and its values can be any expression (`COUNT`, `SUM`, `AVG`, `MAX`, `CASE`, ...). When they differ, every value must be a `SUM` or `COUNT` (optionally times a number), so that partitions add up exactly.
- **`READS lines`**: Declares source dependencies (defaults to all tables referenced in the query).

### Querying Current State

```sql
SHOW THING orders;
```

When an `UPDATE lines SET picked = ...` commits, the corresponding order summary is updated before the write returns (inside an explicit transaction: at its `COMMIT`).

### Inspecting Change History

To view recent mutations and their causes:

```sql
SHOW THING HISTORY orders '200';
```

Returns the most recent 32 history entries for key `200`: when, the cause (a description such as `lines update orderno 200`, not the SQL statement), and the values before and after. Actions taken (or, in shadow mode, worked out) appear here too.

---

## 2. Automated Actions and Shadow Mode (WHEN ... DO)

Things can trigger automated writes when a condition becomes true (e.g. marking an order ready in an ERP system when picking completes).

Actions support variable substitution:
- `KEY`: Evaluates to the key value (e.g. order number).
- `THING.<label>` (or `` THING.`<label>` ``): Evaluates to the value of that label at trigger time (numbers remain unquoted, text strings are quoted, `NULL` becomes `NULL`).

### Testing Safely with Shadow Mode

To test actions against production writes without executing updates on external systems, suffix the action with `SHADOW`:

```sql
CREATE THING ready KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, COUNT(*) AS line_count, SUM(qty) AS qty, SUM(picked) AS picked FROM lines GROUP BY orderno
  WHEN picked >= qty
  DO UPDATE salesorders SET orderstatusid = 21, lines = THING.line_count WHERE orderno = KEY
  TO erp SHADOW;
```

`erp` references a database profile defined in `connections.ini`. In shadow mode, the engine evaluates the condition and records the formatted SQL statements in the key's history without connecting to the target - so the profile does not need `things_write=1` yet.

Inspect recorded candidate actions:

```sql
SHOW THING SHADOW ready;
```

Each entry displays the exact SQL statement that would execute, with real values substituted.

### Promoting to Live Execution

Once verified, promote the Thing to live execution:

```sql
SET THING ready LIVE;
```

Requirements and behavior:
- The target connection profile must have `things_write=1` enabled in `connections.ini`.
- Going live does not replay historical shadow entries; it acts only on orders transitioning to ready from that point forward.
- **Edge-triggered semantics**: Actions execute once per transition from false to true. Subsequent updates while the condition remains true do not re-fire.
- **Transaction isolation**: Actions evaluate only committed data. Changes rolled back in an open transaction never trigger an action.

To revert to shadow mode:

```sql
SET THING ready SHADOW;
```

Mode configuration persists across server restarts.

---

## 3. Auditing and Diagnostics

### Point-in-Time Reconstruction (Time Travel with AS OF)

Every Thing change is appended to an on-disk history log (`<root>/sql_db/things/<name>.asof`), allowing the Thing to be reconstructed at any moment within its retention (and since the log began - for a Thing created before this feature, since the engine first loaded it):

```sql
SHOW THING orders AS OF '2026-10-27 14:15:00';
SHOW THING orders AS OF '2026-10-27 14:15:00' KEYS ('123');
```

Output includes the values as they stood at that timestamp, the recorded `changed_at` time, and the mutation `cause`.

- **Retention settings**: Configured via `SET THING orders KEEP <n> DAYS` (default is 30 days, capped at 256 MB per Thing - past the cap, the oldest changes are folded away; set `KEEP 0 DAYS` to disable). A time before the kept history is refused, naming the earliest time it can answer.
- **Environment defaults**: Configurable system-wide with `AIOS_THING_KEEP_DAYS` and `AIOS_THING_KEEP_MB`.

### Source-Row Attribution (Lineage with EXPLAIN THING)

To identify which underlying source rows produced an aggregate calculation:

```sql
EXPLAIN THING orders '123';
```

The engine runs the Thing's own query for key `123` without its `GROUP BY`, returning the contributing source rows (its `WHERE` and joins applied) alongside a `thing.<label>` column per simple aggregate: each row's share of a `SUM` or `COUNT` - which add up to the value `SHOW THING` shows - and each row's value for a `MIN`, `MAX` or `AVG`. Values computed some other way (`CASE`, `DISTINCT`, two aggregates combined) get no such column. At most 1,000 rows unless `LIMIT n` says otherwise.

---

## 4. Write Previews and Impact Analysis (WHAT IF)

Before running migrations or bulk updates, preview their downstream effects across all Things without altering state:

```sql
WHAT IF UPDATE lines SET picked = qty WHERE wh = 'a';
```

Output is one row per Thing key that would change, with the columns `thing`, `key`, `change` (`new`, `changed` or `gone`), `before` and `after` (only the values that change), and `act`:
1. `would act: <statements> TO <profile>` for a `WHEN ... DO` that would fire, or `would re-arm` where its condition would stop holding.
2. A grain Thing that reads a written table is listed as `would re-run` (its values come from its program and are not previewed).
3. Nothing is written to source tables, Things, their history, or any action's target. `WHAT IF` is refused inside an open transaction.

### Multi-Statement Previews

Preview multiple related operations as a single batch:

```sql
WHAT IF 'UPDATE lines SET qty = 4 WHERE id = 1; DELETE FROM lines WHERE orderno = 300';
```

> [!TIP]
> Always quote multi-statement batches when using the standard `mysql` CLI. Unquoted semicolons cause the command-line client to split the input into separate queries, previewing the first and executing subsequent statements against live tables.

---

## 5. Chaining Things with Managed Tables (INTO)

When multi-tier analytics or reports share intermediate calculations, persist a Thing's output into a managed table using `INTO <db>.<table>`:

```sql
DROP THING orders;   -- replacing the earlier Thing (DROP THING has no IF EXISTS)

CREATE THING orders KEY orderno PARTITION BY orderno READS lines INTO shop.orders_t
  AS SELECT orderno, SUM(qty) AS qty, MAX(wh) AS wh FROM lines GROUP BY orderno;

CREATE THING load KEY wh PARTITION BY wh READS orders_t
  AS SELECT wh, SUM(CAST(qty AS SIGNED)) AS units, COUNT(*) AS orders FROM orders_t GROUP BY wh;
```

Managed table characteristics:
- **Cascading updates**: A committed change to `lines` propagates through `orders`, into `orders_t`, and on into `load` before the statement returns. The writes to `orders_t` are the engine's own, made after the change commits - never inside a client's open transaction.
- **SQL compatibility**: `shop.orders_t` is a standard table accessible to `SELECT` queries, BI tools, and joins.
- **Managed integrity**: Direct client writes to managed tables (`INSERT`, `REPLACE`, `UPDATE`, `DELETE`, `TRUNCATE`, `ALTER`, `DROP`, `RENAME`, `LOAD DATA`) are rejected, naming the Thing. Dropping the parent Thing (`DROP THING orders`) drops the associated managed table. The table must not exist before `CREATE`.
- **Data types**: Values are stored as text (matching `SHOW THING` formatting); downstream numeric aggregations should use explicit `CAST` expressions.
- **Cycle prevention**: Recursive and circular dependencies between chained Things are validated and rejected at `CREATE` time.

---

## When to Use Things

### Recommended Use Cases

- **High-traffic entity metrics**: Order fulfillment progress, inventory availability, customer balances, and facility throughput queries executed frequently across user sessions.
- **Zero-polling reactive interfaces**: Dashboards and monitoring screens using `WAIT THING <name>` or Grain UI `"watch"` bindings (`"watch": {"thing": "orders"}`) to stream incremental updates without periodic polling.
- **Automated workflow handoffs**: Triggering downstream state changes across ERP/database boundaries with verifiable shadow mode testing.
- **Multi-layer analytical rollups**: Building consistent intermediate tables with `INTO` that serve multiple reports and downstream models.
- **Auditable business state**: Applications requiring historical point-in-time auditing (`AS OF`) or granular aggregate verification (`EXPLAIN THING`).

### When to Avoid

- **Ad-hoc and exploratory queries**: One-off queries incur continuous maintenance overhead on writes without recurring read benefits.
- **Arbitrary dynamic filtering**: Queries filtering across dynamic combinations of non-partitioned columns.
- **Global write patterns without partition locality**: Workloads where individual writes modify nearly every key across the table.
- **Unpartitioned joins on large, volatile tables**: Joining large, frequently-written tables lacking partition alignment forces full Thing recalculation on every write.
- **Transaction-local uncommitted reads**: Things only reflect committed state; to inspect pending modifications within an open transaction, query base tables directly.

---

## Architecture and Performance Invariants

- **Targeted Recomputation**: ASQL's mutation hooks identify the partitions a write changed and re-run the Thing's query for just those partitions, not the whole query (a table read whole is the exception above).
- **Append-only History**: Time travel logs (`.asof`) get one appended line per changed key (written while the Thing's lock is held), rewritten from a later cut-off when they pass their days or size (`AIOS_THING_KEEP_DAYS`, `AIOS_THING_KEEP_MB`). Nothing is kept in memory per change.
- **Deferred Settle**: Managed tables are written after the Things settle, with the Things' own lock let go - a write notifies the Things, which take that lock, so writing while holding it would deadlock. Chained Things settle in further passes (at most 8).
- **Rolled-back Previews**: `WHAT IF` runs the statements in an ordinary transaction of its own, which is always rolled back. While it runs it holds what any transaction holds (in the default mode, the writer lock, so other writers wait), so keep previewed statements modest.

---

## References

- [asql.md - Things Reference](asql.md#things)
- [design/thing-time-travel.md](design/thing-time-travel.md)
- [design/thing-what-if.md](design/thing-what-if.md)
- [design/thing-into.md](design/thing-into.md)
