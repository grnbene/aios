# More databases for grain: Redis, Postgres, SQLite

## The problem

grain talks to one kind of database: MySQL/MariaDB and ASQL, over the MySQL wire protocol
(`src/userland/grain_std_db.cpp`). A program that has to read a Postgres database, keep a counter in Redis, or open
an `.sqlite` file a phone app wrote has no way in. Everything has to be in-tree (no libpq, no hiredis, no
libsqlite3), so each one is a protocol or a file format to write and keep.

## What each one costs

| | Protocol / format | Pieces already in the tree | New code (estimate) | Risk |
|---|---|---|---|---|
| Redis | RESP2/RESP3 over TCP, optional TLS | `hal::net`, TLS client | ~300 lines | low: a line protocol, every command is an array of strings |
| Postgres | Wire protocol v3: startup, auth, simple + extended query, text results, COPY out | `hal::net`, TLS client (`SSLRequest` then TLS), SHA-256, HMAC, PBKDF2-HMAC-SHA256 (SCRAM-SHA-256), MD5 (old `md5` auth) | ~1200 lines | medium: auth variants, type OIDs, error fields, notices arriving mid-stream |
| SQLite, read | File format: header, B-tree pages (table + index), varint records, overflow pages, freelist, WAL file | none beyond files | ~900 lines for reading every row of a table | medium: overflow chains, WAL frames newer than the main file |
| SQLite, SQL | Running SQL against it | ASQL's engine, if rows are loaded into it | large | high: SQLite's SQL dialect and type affinity are not MySQL's |
| SQLite, write | B-tree inserts, page splits, journal / WAL, locking | none | several thousand lines | high: a bug corrupts someone's file |

## The proposal

1. **Redis first.** `redis_connect(host, port, [{password, user, db, tls}])` and `redis(conn, "SET", "k", "v")`: any
   command, any arguments, the reply as grain values (string, int, nil, list, map for RESP3). Pipelining as
   `redis_pipeline(conn, [[...], [...]])`. Pub/sub later, the way `events()` delivers SSE.
2. **Postgres second, in the shape grain's MySQL client already has**, so a program changes its connect line and
   not its queries: `pg_connect({host, port, user, password, database, tls})`, `db_query(conn, sql, params)` returning
   `{rows, records, columns, affected_rows}`. Parameters go through the extended protocol (`Parse/Bind/Execute`), never
   spliced into the SQL. Auth: SCRAM-SHA-256 (the default since Postgres 14), md5, trust. Values come back as text and
   are converted by type OID (int, numeric to `decimal`, bool, timestamps as strings, bytea to bytes). `db_transaction`
   works unchanged on top of BEGIN/COMMIT/ROLLBACK.
3. **SQLite read-only, without SQL.** `sqlite_open(path)`, `sqlite_tables(db)`, `sqlite_rows(db, table, [{limit,
   after}])`: every row of a table as records, read straight from the B-tree, WAL included. Enough to import a file
   into ASQL or a Thing, where the program then has SQL. No writing.
4. **Not proposed:** SQLite's own SQL, and writing SQLite files. Either is a project of its own; neither should be
   started as part of closing a gap list.

## Testing

Each against the real thing, once, by hand, and a checked-in test that needs no server where it can:

- Redis and Postgres: a recorded exchange (bytes in, bytes out) replayed by a small fake server inside the test,
  plus a one-off run against a real server (Redis 7, Postgres 16) noted in the commit. CI does not get a database.
- SQLite: fixture files made by the `sqlite3` command line (overflow rows, an index, a WAL file not yet
  checkpointed) checked into `tests/fixtures/`, read and compared row by row.

## Open questions

- Where the clients live: with `install_db_builtins` (apps, the command line) is the natural place; the network
  stays a capability the host grants, as for MySQL.
- Whether `live()` should work on Postgres: polling only (no `ASQL_CHANGES`), unless LISTEN/NOTIFY is wired up.
