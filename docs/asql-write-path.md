# ASQL: the write path's cost

Measured with `aios-asql-write-bench` (20,000 rows, sysbench's table, `WB_ONLY=<phase>` for one phase alone) under
callgrind with cache simulation; per-statement figures are the phase's run minus a load-only run, divided by the
statements it ran (warm-up included).

## Where a one-row UPDATE went (`UPDATE sbtest SET k = k + 1 WHERE id = N`, 82,700 instructions)

| | instructions |
|---|---|
| seven gateway layers before the statement runs (transaction and read-only checks, dispatch, statistics) | ~26,000 |
| parsing `k + 1` and `id = N` (fifteen precedence levels, a ~0.8 KB `SqlExpr` per node) | 12,200 |
| writing the cell (`set_cell`; 5,100 of it a 2,048-row zone-map rescan once every ~420 updates) | 12,700 |
| tokenizing the 45-character statement | 9,300 |
| the redo record: append, drain, wait | 6,800 |
| computing the new value | 4,900 |
| table, view and row-index lookups, cache invalidation | ~4,800 |

No function is more than 4%: the cost is spread over string copies, upper/lower-casing, number<->text conversions
and the same facts about the statement worked out again in each layer. The statement also misses the L1
instruction cache ~2,400 times (~10,000 for a five-statement transaction): it runs through ~150 KB of distinct code.

## Changed (each measured; results identical)

| change | where | saved per UPDATE |
|---|---|---|
| an integer literal of up to 15 digits made without `strtod` (exact: below 2^53, `-0` kept) | `parse_number` | 2,030 |
| multi-character operators matched on their characters, not thirteen `string::compare` calls per punctuation token; the parser's keyword/punctuation tests as length, first character, then `memcmp`, inlined (measured together) | `tokenize_sql_uncached`, `SqlParser::is_word` / `is_punct` | ~4,400 |
| the redo block's earlier bytes and its zero padding kept in the aligned buffer between writes, not copied in and zeroed again (~2 KB copied, ~3.5 KB zeroed per write) | `redo_write_direct` | 1,400 |

| per statement | before | after | |
|---|---|---|---|
| update_index | 82,697 | 74,734 | -9.6% |
| update_non_index | 102,192 | 96,449 | -5.6% |
| insert | 106,123 | 95,443 | -10.1% |
| delete_insert | 303,692 | 288,463 | -5.0% |
| write_txn (5 statements) | 545,430 | 522,291 | -4.2% |

Wall clock (RAM disk, 10,000 statements a phase, median of six alternating rounds against the build before):
update_index 11.79 -> 10.41 us, delete_insert 36.72 -> 34.20, write_txn 73.80 -> 68.48; insert and
update_non_index the same within the spread.

Checked: the vectorized-program, CTE, join and independent-formulation oracle suites give byte-identical output
against the build before (213,578 lines); the SQL test set; the SQLDoom frame's bytes; and a crash test - 1,500
inserts of 5 B to 9 KB with updates and deletes, the engine killed with SIGKILL after the last one returned, on
ext4 and on tmpfs (where the direct-write path runs) - recovers the same rows, sums and checksum as a run that was
not killed, as the build before does.

A first form of the parser tests (a `std::string_view` parameter compared with `==`) cost ~1,700 more than it saved:
GCC called the tests and `string_view::compare` out of line. Tried and dropped: skipping the segment rescan when a dictionary only widened its codes. The rescans the bench
does come from another path in `ColumnStore::set`; with no measured effect the change was not kept.

## Next: the structural costs

What is left is not in any one function; the next steps are each a change of shape, to be done one at a time:

1. **Work out the statement once.** Its verb, whether it reads only, starts or ends a transaction, the tables it
   writes: today each of the seven gateway layers derives these again from the text (`first_word_upper` 12 times a
   statement, two tokenizations' worth of checks, ~70 string compares). One `StatementInfo` made at the top and
   passed down. Expected: most of the ~26,000 gateway instructions and a good part of the instruction-cache misses.
2. **A smaller `SqlExpr`.** Every node - a literal `1` included - carries three hash maps, eight strings and a dozen
   shared pointers for subquery memoization; constructing and destroying the six nodes of a one-row UPDATE is ~5,000
   instructions and several KB of memory touched. The subquery state moves to a side object that only subquery
   nodes have. Needs care: those fields are read and filled by parallel workers, and `clone_expr` copies some and
   resets others.
3. **Statements of the same shape parsed once.** An UPDATE that differs from the last only in its literals reuses
   the parsed form with the new values bound - the tokenizing and parsing above (~20,000) for one lookup.
