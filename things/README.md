# Things

A Thing is a live row the engine keeps for you: declared as SQL, kept current by
the rows that feed it (every write the mirror sees), readable as a document with a
change feed, and - when told to - acting on its own.

Declarations live here (`*.sql`), one statement each; a `--` header is fine. Load
one with `mysql -S /opt/aios/run/asql.sock ... < open_orders_parts.sql`. They are
engine code: `aios/src/userland/sql_things.cpp`.

## Declaring one

```sql
CREATE THING open_orders KEY so.orderno
  AS SELECT ... FROM salesorders so ... GROUP BY so.orderno
  PART trans AS SELECT dt.transno, dt.type FROM debtortrans dt WHERE dt.order_ = KEY
  PART lines AS SELECT * FROM salesorderdetails WHERE orderno = KEY
  WHEN short = 0 DO UPDATE salesorders SET orderstatusid = 21 WHERE orderno = KEY TO st_maria;
```

- `KEY <column>` - one Thing per distinct value; the column is resolved by schema
  (`so.orderno` -> `salesorders.orderno`), its output name is the column's own
  (`orderno`). Every table the SELECT reads is watched; a row of the key's table
  moves that key alone, any other table moves the kind. `PARTITION BY c` and
  `READS t1, t2` exist for the older per-partition aggregate form.
- `PART name AS SELECT ... WHERE <col> = KEY` - a list hanging off the Thing (the
  queries a page ran per order). Rules: the key column must belong to one table of
  the part's FROM (qualify it if two have it); a `GROUP BY` is per key; no `LIMIT`;
  `KEY` may not sit inside a subquery. Parts re-run for the keys a changed row
  names, in batches of 400.
- `WHEN <condition> DO <statement>[; ...] TO <profile>` - the work. The condition
  is SQL over the Thing's own columns. When it comes to hold the statements run,
  `KEY` filled in, as one transaction on the connection profile; once, until the
  condition stops holding, then it is armed again. Each act is a history line and
  a change of the Thing, written to disk at once (a crash cannot do it twice).
  `KEY` after `DUPLICATE`, `PRIMARY`, `UNIQUE`, `FOREIGN` is SQL's own and is left
  alone.

The profile is a `[profile]` block in `<root>/sql_db/connections.ini` marked
`things_write=1`; a Thing writes only where someone said it may, and the file must
be readable by the user the engine runs as.

## Reading one

```
SHOW THINGS                       every kind: things, clock, what it watches, what it does
SHOW THING open_orders            one row per Thing (key, columns, version, last cause)
SHOW THING open_orders JSON       key, stamp, state, document - head fields, then each part as a list,
                                  "_version", "_updated"; rendered once per change, copied after
SHOW THING open_orders JSON SINCE <clock>   only what changed after the clock, departed keys as "gone"
SHOW THING open_orders CLOCK      the kind's clock and count
DROP THING open_orders
```

Read JSON through a library client (mysqli, pymysql). The `mysql` CLI backslash-
escapes the text and appends a `machine` column.

## Proving one

- `aios/tools/erp-drive/v8/board-harness.php` - the v8 open-orders board from the
  Thing against the board's usual path, every entry compared field for field.
- `aios/tools/erp-drive/v8/pick-drive.sh ORDERNO` - one order through picking on
  st, the way the ERP's tables see it; watch `picked_orders` mark it Picked.
- `AIOS_THINGS_TRACE=1` on the server prints each row notice and part settle.

## sticker_item

The sticker feed's rows, one Thing per tire, so a pull reads rows that are already right instead of
building 50,000 of them from the raw tables (seconds of CPU a request). It is a grain Thing, and it ships with
the feed itself: `apps/stickerfeed/things/sticker_item.gr` imports `apps/stickerfeed/lib/feed.gr`, the same
functions the service builds the feed with, so the arithmetic and the quirks are the one set. Deploying the app
(`app deploy apps/stickerfeed ...`) puts it in place: its `deployed.gr` sends the program (imports and all) as
`CREATE OR REPLACE THING`, which builds the new version beside the old one.

Each row holds `mid`, `vitem` and `r_<level>`: the finished JSON object for that price level, byte for
byte what the feed sends. `SHOW THING sticker_item` reads them; the feed (`held_feed`) filters by the
client's exclusions and joins them. A write to a table a row was built from replaces that row, by the
same code: a stock row, a price, a cost, an order line each name their item; an order names its lines';
a brand its items; a table every item is priced from (price levels, product-line rules, remote
warehouses) is a pass over all of them; a client or branch change re-resolves the levels. An item whose
price or special starts or ends on a date carries `wake_at` and is read again then (`DAY_SAFE`), so the
day turning is not a pass.

What each table gave each item is kept (`input()` in the feed's code, `INPUTS` here): `touched()` marks,
in the inputs read from the changed table, the items the write reaches, and a refresh reads only those
again. A stock quantity changed in place (same item, same location, whole numbers) moves the kept sums
by the change with nothing read at all. On chi-il-1: a stock change 0.26 ms of the engine's time, an
order line about 1 ms, a stock row added or removed about 2 ms, a price about 5 ms; a pass over every
item for a price-level change about 5 s, reusing what is kept; the first pass (creating it, or after a
REFRESH) about 10 s. Anything the program cannot account for - a full pass no write asked for, a
touched() that did not finish, an item asked about that no write marked - is read afresh.

The feed uses the Thing when it exists and carries the client's level, and builds the old way when not.

