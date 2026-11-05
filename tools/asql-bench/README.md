# asql-bench

The harness behind `docs/asql-benchmarks.md`: ASQL, MariaDB 10.11, CedarDB and DuckDB on the same 500,000 rows, and
ASQL against DuckDB on three of the ERP's reports over summitholding. Each engine runs alone, one after another.

Run it from a scratch copy of this folder (it writes `data/`, `results/` and a password file next to itself):

```bash
B=/path/to/scratch/bench; cp -R tools/asql-bench "$B"; cd "$B"
printf '%s' 'Bench-Run-2026x!' > pw; chmod 600 pw     # CedarDB wants 8+ characters, mixed classes
mkdir -p data results && python3 -I gen.py           # data/bench_sales.csv and data/inserts.sql, fixed seed
```

Engines (fresh containers from the images; the data folder is mounted read-only for the bulk loaders):
On a machine whose Docker data root is on a slower disk than the one ASQL and DuckDB use, bind-mount the engines' data
folders from that disk (`-v DIR:/var/lib/mysql`, `-v DIR:/var/lib/cedardb/data`), or the containers measure the disk.

```bash
docker run -d --name asqlbench-maria -p 127.0.0.1:13306:3306 -e MARIADB_ROOT_PASSWORD="$(cat pw)" \
  -v "$B/data:/data:ro" mariadb:10.11.9 --secure-file-priv=/data --innodb-buffer-pool-size=1G
docker run -d --name asqlbench-cedar -p 127.0.0.1:15432:5432 -e CEDAR_PASSWORD="$(cat pw)" \
  -v "$B/data:/data:ro" cedardb/cedardb:latest
# AIOS_SQL_SECURE_FILE_PRIV names the one directory a server-side LOAD DATA INFILE may read (unset: refused)
mkdir asqlroot && AIOS_SQL_SECURE_FILE_PRIV="$B/data" /path/to/aios --root "$B/asqlroot" exec \
  "compute serve port=7399 lanes=8 sql=1 mysql_port=3399 mysql_bind=127.0.0.1 mysql_user=root mysql_pass_file=$B/pw" &
cc -O2 -o duckbench duckbench.c -I/opt/homebrew/opt/duckdb/include -L/opt/homebrew/opt/duckdb/lib -lduckdb \
  -Wl,-rpath,/opt/homebrew/opt/duckdb/lib
```

The 500k-row tests, then the answer check (every query's rows against MariaDB's, numbers within 1e-9 relative) and the
table of both passes:

```bash
# AMOUNT=decimal (the default) or double: the amount column's type, on every engine
for e in asql maria cedar; do AMOUNT=double php bench.php $e > results/$e.json; done
./duckbench duck.db "$B" double > results/duck.json         # without "double": DECIMAL(12,2)
php cmp.php results; php table.php        # table.php reads results/ and results2/ (a second pass)
```

`bench.php` times each statement around the client call that sends it and reads its whole answer, over one
persistent loopback connection (`mysqli` for ASQL and MariaDB, `pgsql` for CedarDB). `duckbench` times
`duckdb_query` in process. For ASQL each query is timed with `SQL_NO_CACHE`, and again without it (`query_cached`):
the second is the result cache answering.

The ERP reports need an ASQL server on a copy of a summitholding root (port 3398) and a DuckDB copy of the same
tables. The copy is made with DuckDB's `mysql_scanner`, reading from that server, then the keys MariaDB's PAD SPACE
collation matches across a trailing space are trimmed, so an exact join in DuckDB matches what the view matches:

```bash
export MYSQL_PWD="$(cat pw)" MYSQL_USER=root MYSQL_HOST=127.0.0.1 MYSQL_TCP_PORT=3398 MYSQL_DATABASE=summitholding
duckdb -csv erp/sh.duckdb < erp/load.sql && duckdb -csv erp/sh.duckdb < erp/load2.sql && duckdb erp/sh.duckdb < erp/sh_view_duck.sql
cp erp/sh.duckdb erp/sh_trim.duckdb && duckdb erp/sh_trim.duckdb < erp/trim.sql
cc -O2 -o duckerp duckerp.c -I/opt/homebrew/opt/duckdb/include -L/opt/homebrew/opt/duckdb/lib -lduckdb \
  -Wl,-rpath,/opt/homebrew/opt/duckdb/lib
php erp.php > erp/asql.1.json; ./duckerp erp/sh_trim.duckdb "$B" > erp/duck.1.json; php erpcmp.php 1
```

`erp/asql*.sql` are the reports as the ERP sends them (with `SQL_NO_CACHE`); `erp/duck*.sql` the same in DuckDB's
dialect. `nopurchase` ends in `LIMIT 100` with no `ORDER BY`, so its 100 rows are any 100 that qualify: compare it
without the `LIMIT`.
