# ERP drive for ASQL

Runs the v8 ERP's own model code (`/repos/v8/server`, `ModelOmsOrders` +
`Weberp\B2b`) against an ASQL wire port and times every statement it sends.
Needs `php8.3-mysql` and a scratch ASQL over a COPY of a real root:

    aios --root COPY exec 'compute serve port=17311 lanes=4 sql=1 sql_db=sql_db mysql_port=13401 mysql_bind=127.0.0.1'

- `php erp-drive.php 13401 [nocache]` — each model call: wall ms, statements,
  ms in SQL, slowest statements. `PASSES=2` (default) shows cold vs warm;
  `ONLY=name` runs one call; `DUMP=dir` writes every statement to dir/.
- `php same.php dir` — every dumped SELECT against :13402 (A) and :13401 (B):
  byte-identical or not.
- `php variants.php PORT stmt.sql` — the statement as sent and with parts
  removed, best of 3, to see where its time goes.
- `php loop.php PORT stmt.sql SECONDS` — load for gdb sampling or trace.

Under PHP 8 some calls throw TypeErrors that PHP 7.4 (production) only warns
about; the SQL has already run and been timed when that happens.
Engine phases: start the server with `AIOS_VX_TRACE=1` and read `phases:`.
Always compare against the previous binary on its own copy of the same root.

## Against MariaDB (same data, same schema)

Load the reference production schema (`ssh <host> mysqldump --no-data <db>`) and the ASQL
copy's rows into a local MariaDB (mysqldump from ASQL piped into mysql), then:

- `php race.php STMT_DIR [N]` - every ERP statement on both, per-statement
  median, as sent and `SQL_NO_CACHE`, with the sum. `x` = MariaDB / ASQL.
- `php cmpreport.php 'SELECT ... ORDER BY k' [skip=3] [tol=0.005]` - one
  query's full result compared cell by cell. Numbers within a tolerance are
  equal (the ERP's amounts are DOUBLE: two engines add them in different
  orders); `skip=` names columns SQL leaves unspecified (a column the query
  neither groups nor aggregates, like `terms` in customerageingreport).
- `php cmpmeta.php STMT_DIR` - every result column's definition (name, original
  name, table, original table, schema, type class, decimals) on both.
- `python3 cmptyped.py STMT_DIR` - the same statements through PyMySQL, which
  converts by declared type: compares value AND Python type per cell.
- `DBHOST= DBUSER= DBPASS= php erp-drive.php PORT nocache` drives the ERP's own
  model code against either server.

Incremental views (customerageingreport): write the same UPDATE / INSERT /
DELETE to both servers and compare the whole report with cmpreport.php after
each - a write to ASQL alone must show up as exactly the changed cells.
Stress it with several readers of the view and of the `WITH dispatched` page
query while a writer runs (run the server under gdb: a heap abort prints its
stack), then compare the patched view with the view's own query recomputed on
the same server.
