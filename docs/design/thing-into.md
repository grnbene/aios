# Things built on Things: `CREATE THING ... INTO <db>.<table>`

## The problem

A warehouse's load is a sum over its open orders; the open orders are already a Thing. Today the second Thing has
to repeat the first one's query (the same joins, the same WHERE), and the two drift apart when one is changed.
`LINKS` lets a claims Thing name a resource Thing, but nothing lets a Thing read another Thing's values.

## The design

A row Thing (one row per key, or partitioned sums) can say `INTO <db>.<table>`: the engine keeps that table equal to
the Thing - one row per key, the key's column and one column per value, named by the Thing's labels. A Thing that
`READS` that table is built on the first one, through the same change notices any table write sends. Reports can
read and join the table like any other, which they cannot do with `SHOW THING`.

- **The table is the Thing's.** It is created by `CREATE THING .. INTO` (it must not exist yet), with the key as its
  primary key and every column text, as a Thing's values are text. It is dropped with the Thing. A client may read
  it; a client's write to it is refused (`table x is kept by Thing y`).
- **When it is written.** After each settle of the Things, the rows of every Thing with `INTO` that changed are
  written (an upsert per changed key, a delete per key that went away), with the Thing lock let go: a write
  notifies the Things, which take that lock. The Things those writes reach settle in a further pass; passes repeat
  until none is pending, at most 8 deep.
- **No cycles.** At `CREATE`, a Thing whose `READS` reach its own `INTO` table through other Things' `INTO` tables
  is refused, naming the chain.
- **Restarts.** The table is saved with the store; the Thing with its own state file. A crash between the two is
  mended at load: each Thing with `INTO` compares its table with itself and writes the difference.
- **Transactions.** The writes are the engine's own, outside any client transaction: they follow what was
  committed (a Thing reacts only to committed changes).

## Not in this step

- Grain Things, claims and resources: not one row of values per key.
- Typed columns. Everything is text, as `SHOW THING` gives it; a reader casts (`CAST(total AS DECIMAL(12,2))`).
- `INTO` on a Thing that already exists (`ALTER THING`). Drop and create it with `INTO`.
