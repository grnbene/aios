# ASQL: typed data between query stages

A statement built from stages - a WITH's CTEs, a derived table, a UNION ALL that feeds one - used to hand each
stage's answer to the next as rows of text: the select printed every cell to a string at its emit, and the table
that held the answer parsed each string back into a column store. On a CTE-heavy statement that was about half the
time. Measured on SQLDoom's renderer (89 CTEs, ~1.4 s a frame, 4 cores): ~305 ms printing cells, ~346 ms putting
them back, against ~350 ms of joins.

This note describes the replacement, in stages, and what each one keeps true.

## The rule that keeps it safe

A typed batch (`ColBatch`, `src/userland/sql_colbatch.inc`) is **another form of a result's rows, never another
answer**. Each column is integer lanes, double lanes, dictionary codes with the entries they use, or text; with
its NULLs. `text(r)` of any column is exactly the cell the row path would have held, so any consumer that cannot
use a batch turns it back into rows (`to_rows`) and goes on as before. Nothing is lost by declining.

- Only a consumer that can use a batch asks for one (`ColOutRequest`), for exactly the call it brackets.
- The select that starts first under the request takes it (`take_col_out`): that is the statement's top select,
  so a subquery or a derived table below it never sees the request.
- A select that cannot fill it - a sort, DISTINCT, HAVING, a LIMIT, a grouped emit, an answer from the result
  cache - returns rows, as before. A filled answer is marked `rows_in_batch` and is never stored in the cache.
- `AIOS_SQL_COLUMNAR=0` turns the whole path off.

## Stage 3a: the handoff (done)

Producers fill a batch where they used to project rows:

- the general (join) path's ungrouped emit, and the single-table engine's plain page;
- each item made over every row at once: a stored column gathered by row id (`colbatch_gather`), a compiled
  program's lanes (`colbatch_from_vp`), the interpreter's text otherwise;
- an INT result column whose every value is a canonical integer (a literal `0`, an integer held in a dictionary)
  becomes integer lanes (`colbatch_narrow_int`, through `canonical_int`, so `007` and `-0` stay text).

Consumers:

- a CTE's table (`execute_with`): its columns typed from the batch by the same rules the rows used
  (`cte_columns_with`, `fit_decimal_scales_with` - one copy of the rules, over a value visitor), then filled
  column by column (`colbatch_fill_table`): integers into TypedInt64, doubles into TypedDouble, a dictionary's codes
  through its own dictionary (each entry interned once), text cell by cell - every value normalised as
  `append_rows_bulk` normalises it. An integer goes in typed only for an INT column, a double only for a DOUBLE of
  no fixed scale; anything else as its text.
- UNION ALL: each branch asked for its own batch, put end to end where nothing the row path does afterwards would
  change a cell. A DOUBLE union column's parseable values become doubles (the row path reprints each from its
  double: `'2.50'` becomes `2.5`, `-0` becomes `0`, `1e15` stays `1e15`), an INT column's canonical integers
  become lanes. A DECIMAL column, text that is no number in a number column, or an ORDER BY / LIMIT over the
  union: every batch goes back to rows and the union is made as before.

New ColumnStore members write typed values directly (`begin_typed`, `append_int`, `append_double`,
`append_dict_code`), keeping the null bitmap, zone maps and segment seals as `append_encoded` does.

## Stage 3b: readers that do not print typed cells (done)

A result table's typed columns are read as numbers where the row path's dictionaries were read as text:

- the interpreter reads a typed integer column of a result table as its integer, and a plain DOUBLE column held
  as doubles as its double (its 0 read as `0`, as `cell()` prints it);
- GROUP BY's code-based grouping and the join's integer hash take a result table's typed integer column.

## Measured (SQLDoom E1M1, steady frames, mean of eight, alternating on/off)

| | per frame |
|---|---|
| `AIOS_SQL_COLUMNAR=0` | 1,378 ms |
| on | 1,002 ms (-27%) |

52 of the frame's 62 materialized CTEs go as columns; putting rows back fell from ~346 ms to ~142 ms. The frame's
192,000 bytes are identical either way.

## Oracles

- a CTE suite (115 statements; every column kind, -0, past 1e15, DECIMAL, fixed-scale DOUBLE, numbers held as text,
  NULLs from outer joins, CTE chains, joins and grouping over CTE columns, UNION ALL of matching and mismatched
  kinds), run with the path on and off and with CTE merging on and off: identical output;
- the vectorized-program suite (727 statements): identical;
- the SQLDoom frame's bytes, the asql/aql ctest suite (28/28, on and off), `asql.doom_sql.check`.

## Next stages

- **3c: grouped emits and derived tables.** A GROUP BY's answer and a derived table or LATERAL (`FROM (SELECT ..)
  q`) still hand over rows. Same producer shape: aggregates per group are already Vals, group keys gather from the
  group's first row.
- **3d: the last copy.** The batch is still copied into column stores; a table could adopt its lanes directly
  (an I64 column's vector becomes `int64_data`).
- **4: batched joins.** The join still probes one base row at a time; keys are already in lanes, so the probe
  can run over a batch of them and gather candidates in a tight loop. The next largest cost after the handoff.

Trade-offs taken: the path is opt-out, not opt-in, because every consumer that declines gets exactly the old rows;
code size is 223 lines in one new file (`sql_colbatch.inc`) and about 440 lines of hooks and typed appends in
`sql_database.cpp`; the cost of a decline is the batch's own construction,
which only happens when a consumer asked.
