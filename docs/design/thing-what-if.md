# What-if for Things: `WHAT IF <statement>[; ...]`

## The question it answers

"If I run this price update, which Things change, how, and which of their acts would fire?" Before a bulk write
to the ERP's tables, that is the thing nobody can see until it is too late.

## How it works

`WHAT IF` runs the statements in a transaction of its own and always rolls it back. Because a Thing now reacts
only to committed changes (a transaction's change notices are held until COMMIT and dropped at ROLLBACK), the
Things never see the writes. While the transaction is open:

1. The held notices say which Things the writes reach: for a Thing made by a query, the partitions of the rows
   that changed (before and after), or the whole Thing when the table is not linked to it by its partition column.
2. The keys at stake are the Thing's keys in those partitions now, and the keys the Thing's own partition query
   finds in them inside the transaction (a key the write would create).
3. Each such key's values with the write applied come from the Thing's own query, run for those keys inside the
   transaction, where the uncommitted rows are visible.
4. They are compared with the Thing as it stands. For a Thing with `WHEN .. DO`, the condition is evaluated on the
   new values, and where it would come to hold the act is worked out as it would run (`KEY` and `THING.<label>`
   filled in).

Then the transaction is rolled back. Nothing is written to any table, any Thing, its history, its `AS OF` log, or
any act's target.

## What it answers with

One row per key that would change: the Thing, the key, `new` / `changed` / `gone`, the values before and after
(only the values that change), and for an acting Thing `would act: <statements> TO <profile>` or `would re-arm`
(its condition stops holding). A grain Thing that reads a written table is listed once as `would re-run (a grain
Thing: not previewed)`: its values come from its program, not a query that can be run for some keys.

## Writing several statements

The mysql command-line client splits a query on `;` itself, before anything reaches the engine; `WHAT IF a; b`
typed there previews `a` and runs `b`. So several statements are written as one string, `WHAT IF 'a; b'`. A client
that sends the text as one query may use the bare form: the engine's own splitter (`mysql_wire.cpp`) keeps
everything after `WHAT IF` as one statement, so nothing after it runs for real.

## Limits

- `WHAT IF` cannot run inside an open transaction (it needs one of its own to roll back).
- `PART` sub-queries of a row Thing are not compared.
- A write to a table a Thing reads whole (not by its partition column) previews every key of that Thing: correct,
  but it runs the Thing's whole query once.
- The statements' own results are not returned; a statement that fails stops the preview, with its error, and the
  transaction is rolled back as always.
- While it runs, the transaction holds what any transaction holds (in the default mode, the writer lock), for as
  long as the statements and the preview queries take.
