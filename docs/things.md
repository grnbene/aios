# Things: a practical guide

A **Thing** is a query whose answer ASQL keeps up to date for you. You write the query once; every time a row it reads changes, the engine works out which keys that touches and recomputes just those. You read the answer with `SHOW THING`, and it is never stale.

This guide walks through one example from start to finish - open orders in a warehouse - and shows how to use each feature along the way. The full syntax and every option is in [asql.md](asql.md#things); this page is the "how do I use it" version.

## If you know other systems

A Thing is an **incrementally maintained materialized view**: a stored query result that stays correct as the
data under it changes, without being recomputed from scratch. The idea shows up in many places under other names:

| Where you may have met it | What it is called there | How a Thing compares |
|---|---|---|
| PostgreSQL | `MATERIALIZED VIEW` | Postgres recomputes the whole view when you run `REFRESH MATERIALIZED VIEW`. A Thing is never refreshed by hand, and recomputes only the keys a write touched. |
| Oracle | materialized view with `REFRESH FAST ON COMMIT` | The closest match: kept current at commit from change logs. A Thing also acts on its values, keeps their history and explains them (below). |
| SQL Server | indexed view | Maintained inside every write, but with strict limits on the query (no outer joins, no `MAX`/`MIN`, and more). A Thing's query can be any `SELECT ... GROUP BY` over the key. |
| MySQL / MariaDB | no built-in equivalent; usually a summary table kept by triggers or a cron job | A Thing replaces that hand-written trigger or batch job, and cannot drift from the query it stands for. |
| Streaming systems (Materialize, ksqlDB, Flink SQL) | a materialized view or table over a stream | Same idea, but they run as a separate service fed by a change stream. A Thing lives in the database the data is already in. |
| Spreadsheets | a formula cell | The analogy most people already have: change an input cell and every total that depends on it updates. A Thing is that, for SQL. |
| UI frameworks (MobX, Vue, Svelte) | computed / derived values | Same contract: declare how a value is derived, and it is kept current when its inputs change. |

The extra features map to familiar ideas too:

| Thing feature | Known elsewhere as |
|---|---|
| `WHEN ... DO` | a trigger, or a rule in a rules engine, but on a computed value (fires when an order *becomes* fully picked, not on each row written) |
| `SHADOW` then `LIVE` | a dry-run or "shadow mode" rollout: run the new logic beside the old, compare, then switch |
| `AS OF` | temporal tables (SQL:2011 system versioning; SQL Server's `FOR SYSTEM_TIME AS OF`; Oracle Flashback Query), for the Thing's values |
| `EXPLAIN THING` | data lineage or provenance: which inputs produced this number |
| `WHAT IF` | running a change in a transaction and rolling it back, with the downstream effects reported for you |
| `INTO` | a table kept equal to a view, so other views (and ordinary SQL) can build on it |

## The example

```sql
CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, wh VARCHAR(4), qty INT, picked INT, price DECIMAL(10,2));
```

Each row is one line of an order: how many items were ordered, how many have been picked so far, and the price.

## 1. Make a Thing

```sql
CREATE THING orders KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, COUNT(*) AS line_count, SUM(qty) AS qty, SUM(picked) AS picked, SUM(qty * price) AS total
     FROM lines GROUP BY orderno;

SHOW THING orders;
```

This maintains one live row per order. An `UPDATE lines SET picked = ...` updates the order it belongs to before the statement returns.

To inspect recent changes and what triggered them:

```sql
SHOW THING HISTORY orders '200';
```

## 2. Let it act - carefully

A Thing can trigger an action when a condition becomes true. For example: when an order is fully picked, mark it ready in the ERP. The current values of the Thing are available to the action as `THING.<label>`, and `KEY` is the order number.

### Start in shadow mode

In shadow mode, the action is evaluated and recorded exactly as it would run, but it is not sent to the target database:

```sql
CREATE THING ready KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, COUNT(*) AS line_count, SUM(qty) AS qty, SUM(picked) AS picked FROM lines GROUP BY orderno
  WHEN picked >= qty
  DO UPDATE salesorders SET orderstatusid = 21, lines = THING.line_count WHERE orderno = KEY
  TO erp SHADOW;
```

`erp` is a connection profile in `connections.ini`. Watch the actions it would take:

```sql
SHOW THING SHADOW ready;
```

Every line is a statement it would have run, with the real numbers filled in.

### Promote to live

When you are satisfied with the previewed actions:

```sql
SET THING ready LIVE;
```

The profile needs `things_write=1` before this is allowed—a Thing writes only where explicitly permitted. Going live acts only on orders that become ready from that point forward; it does not replay what it would have done in shadow mode.

You can hold back actions again at any time with `SET THING ready SHADOW`. Both settings survive engine restarts.

### Rules for actions

- **Edge-triggered**: An action fires once each time its condition becomes true, not on every subsequent update while it stays true.
- **Transaction isolation**: A Thing reacts only to committed data. If someone updates `lines` inside a transaction and rolls it back, the Thing never sees the change, and no action is fired.

## 3. "Why does it say that?"

Two diagnostic questions come up constantly:

### What was the state at a given moment? (Time travel)

Every change to a Thing is kept on disk for 30 days by default:

```sql
SHOW THING orders AS OF '2026-10-27 14:15:00';
SHOW THING orders AS OF '2026-10-27 14:15:00' KEYS ('123');
```

Each row shows the values as they stood at that moment, when they were set (`changed_at`), and what caused the change (`cause`).

- Adjust retention: `SET THING orders KEEP 90 DAYS` (or `KEEP 0 DAYS` to disable).
- Storage: History is kept in append logs on disk (capped at 256 MB per Thing), keeping memory footprint minimal.

### Which rows make up this value? (Lineage)

```sql
EXPLAIN THING orders '123';
```

This returns all source rows behind order 123, plus a `thing.<label>` column showing each individual row's contribution to aggregates (`SUM`, `COUNT`, `MIN`, `MAX`, `AVG`). The individual shares add up directly to what the Thing shows.

## 4. Try a change before you make it (What-If previews)

Before running a bulk update or migration, preview what it would do to your Things:

```sql
WHAT IF UPDATE lines SET picked = qty WHERE wh = 'a';
```

The output lists every order that would change, its values before and after, and any action it would fire. Nothing is written—not to the source tables, not to the Things, and not to any action target.

Several statements can be previewed in a single batch:

```sql
WHAT IF 'UPDATE lines SET qty = 4 WHERE id = 1; DELETE FROM lines WHERE orderno = 300';
```

> [!TIP]
> Use the quoted form when using the `mysql` command-line client. That client splits input on semicolons before sending it to the server, so without quotes it would preview the first statement and **execute the second statement for real**.

## 5. Build Things on other Things

Often one Thing serves as the input to another: for example, overall warehouse load is an aggregate over open orders. Rather than duplicating order logic across queries, give the first Thing a managed table:

```sql
-- Drop the earlier standalone Thing if replacing it with a managed-table Thing
DROP THING orders;

CREATE THING orders KEY orderno PARTITION BY orderno READS lines INTO shop.orders_t
  AS SELECT orderno, SUM(qty) AS qty, MAX(wh) AS wh FROM lines GROUP BY orderno;

CREATE THING load KEY wh PARTITION BY wh READS orders_t
  AS SELECT wh, SUM(CAST(qty AS SIGNED)) AS units, COUNT(*) AS orders FROM orders_t GROUP BY wh;
```

`shop.orders_t` is a real database table that ASQL keeps in sync with `orders`:
- A write to `lines` flows through `orders`, into `orders_t`, and into `load` before the write statement returns.
- Standard SQL queries, BI tools, and reports can read and join `orders_t` like any other table.
- The table is managed by the Thing: client writes (`INSERT`, `UPDATE`, `DELETE`) are rejected, and `DROP THING orders` drops the table automatically.
- Values are stored as text (matching `SHOW THING` output), so cast them when comparing numerically.
- Circular dependencies between Things are detected and prevented at `CREATE` time.

## When a Thing is the right tool

Reach for a Thing when **the same question is asked over and over, and the answer must be current**:

- **Status per business object.** Per order: lines picked of lines ordered. Per customer: open balance. Per item: on
  hand and on order. Per warehouse: units still to pick today. These are read on every page load, and recomputing
  them from the raw tables each time is what makes pages slow.
- **Dashboards and boards that refresh constantly.** A board polled every few seconds reads the Thing, which is
  already computed, instead of running the aggregate again. `WAIT THING` lets a page wait for a change rather than
  poll at all.
- **Acting on a condition.** "When an order is fully picked, mark it ready." "When stock falls under its reorder
  point, raise a purchase line." Without a Thing this is a polling job that rescans everything, or a trigger that
  has to recompute the same aggregate itself.
- **Explaining numbers to people.** When "the dashboard is wrong" tickets are common, `AS OF` and `EXPLAIN THING`
  answer them from the database instead of from guesswork.
- **Logic that several reports share.** Define "an open order" once as a Thing with `INTO`, and build the
  warehouse, customer and driver views on it.

## When it is not

- **One-off and ad-hoc queries.** If a question is asked once, just run the query: a Thing pays to keep the answer
  current on every write, which only pays off when the answer is read often.
- **Questions shaped by each request.** "Sales between these two dates someone picked on a form" changes with every
  request. Keep a Thing per day or per hour (as `touch_hour` does) and add up the days the request asks for, rather
  than one Thing per possible range.
- **Values that change on nearly every write anyway.** If almost every write touches almost every key, there is
  little left to save, and the Thing adds work to each write.
- **Tables a Thing would read whole.** A Thing reacts by key when the table carries its partition column. A table
  it joins without that column (a price list, say) makes every change to it recompute the whole Thing. That is fine
  for small lookup tables, and expensive for big, busy ones.
- **Exact, transactional reads of your own uncommitted writes.** A Thing shows committed data only; inside an open
  transaction, query the tables directly.

## Operational considerations

- **Targeted updates**: When source data changes, ASQL recomputes only the specific keys affected by that write rather than re-evaluating the full query.
- **Append-only history**: Time travel logs (`AS OF`) use compact on-disk appends per changed key.
- **Managed tables**: `INTO` updates only modified keys in the target table once changes settle.
- **Lightweight previews**: `WHAT IF` evaluates changes in a temporary transaction that rolls back immediately.

## Where to read more

- Reference syntax and all options: [asql.md - Things](asql.md#things)
- Time travel storage and pruning internals: [design/thing-time-travel.md](design/thing-time-travel.md)
- What-If preview transaction mechanics: [design/thing-what-if.md](design/thing-what-if.md)
- Cascading updates and cycle detection: [design/thing-into.md](design/thing-into.md)
