#undef NDEBUG
// The SQL constructs SQLDoom's renderer and game tic lean on that ASQL lacked, each against what a real server
// answers for the same statement (MariaDB 10.11 where it has the construct, MySQL 8.0 for LATERAL, which MariaDB
// does not have):
//   [1] a row-value IN over a subquery: (x, y) IN (SELECT c, r ...), NULLs and NOT IN as the server has them;
//   [2] WITH (and WITH RECURSIVE) inside a subquery, a derived table and a function body;
//   [3] LATERAL derived tables.
// Each is tried at the top level, in a stored function's body and nested in another query.
#include <iostream>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;

void fail(const std::string& label, const std::string& why) {
  std::cerr << "  [FAIL] " << label << ": " << why << "\n";
  g_failures++;
}

SqlQueryResult q(SqlDatabase& db, const std::string& sql) { return db.execute_gateway_query(sql, "d"); }

void run_ok(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) fail("setup", r.error_message + "\n    " + sql);
}

// Every row, its cells joined by '|', the rows by ';' (NULL as NULL).
std::string rows_of(const SqlQueryResult& r) {
  std::string out;
  for (size_t i = 0; i < r.rows.size(); ++i) {
    if (i) out += ";";
    for (size_t k = 0; k < r.rows[i].values.size(); ++k) out += (k ? "|" : "") + sql_display(r.rows[i].values[k]);
  }
  return out;
}

void expect(SqlDatabase& db, const std::string& label, const std::string& sql, const std::string& want) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) return fail(label, r.error_message);
  const std::string got = rows_of(r);
  if (got != want) return fail(label, "got '" + got + "', want '" + want + "'");
  std::cout << "  [ok] " << label << " = " << got << "\n";
}

void expect_error(SqlDatabase& db, const std::string& label, const std::string& sql, const std::string& want_part) {
  SqlQueryResult r = q(db, sql);
  if (r.success) return fail(label, "succeeded (" + rows_of(r) + "), want an error containing '" + want_part + "'");
  if (r.error_message.find(want_part) == std::string::npos)
    return fail(label, "error '" + r.error_message + "', want one containing '" + want_part + "'");
  std::cout << "  [ok] " << label << ": " << r.error_message << "\n";
}

}  // namespace

int main() {
  std::cout << "=== asql: the SQL SQLDoom needs ===\n";
  const CheckRoot check_root("/tmp/aios-asql-doom-sql");   // this run's own (see check-root.hpp)
  KernelConfig cfg;
  cfg.root = check_root.path;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::cerr << "Failed to boot kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = *k_res.value();
  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  run_ok(db, "CREATE DATABASE d");
  run_ok(db, "CREATE TABLE a (x INT, y INT, s VARCHAR(10))");
  run_ok(db, "INSERT INTO a VALUES (1,1,'p'),(1,2,'q'),(2,1,'r'),(3,NULL,'s'),(NULL,1,'t')");
  run_ok(db, "CREATE TABLE w (c INT, r INT, solid INT)");
  run_ok(db, "INSERT INTO w VALUES (1,1,1),(1,2,0),(2,1,1),(9,9,1),(NULL,5,1)");

  std::cout << "\n[1] (x, y) IN (SELECT ...)\n";
  const std::string solid = "(SELECT c, r FROM w WHERE solid)";
  expect(db, "IN", "SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE (x,y) IN " + solid, "p,r");
  expect(db, "NOT IN", "SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE (x,y) NOT IN " + solid, "q");
  expect(db, "as values, NULLs", "SELECT s, (x,y) IN " + solid + " AS v, (x,y) NOT IN " + solid + " AS nv FROM a ORDER BY s",
         "p|1|0;q|0|1;r|1|0;s|NULL|NULL;t|NULL|NULL");
  expect(db, "a NULL the other column contradicts",
         "SELECT s, (x,y) IN (SELECT c, r FROM w WHERE solid AND c IS NOT NULL) AS v FROM a ORDER BY s", "p|1;q|0;r|1;s|0;t|NULL");
  expect(db, "against no rows",
         "SELECT (1,1) IN (SELECT c, r FROM w WHERE 0), (1,1) NOT IN (SELECT c, r FROM w WHERE 0), (NULL,1) IN (SELECT c, r FROM w WHERE 0)",
         "0|1|0");
  expect(db, "compared as = compares",
         "SELECT (1,'1') IN (SELECT c, r FROM w), ('1',1.0) IN (SELECT c, r FROM w), (2,1) IN (SELECT c+0.0, r FROM w)", "1|1|1");
  expect(db, "correlated", "SELECT s FROM a WHERE (x, y) IN (SELECT w.c, w.r FROM w WHERE w.c = a.x AND w.solid = 1) ORDER BY s", "p;r");
  expect(db, "with another condition", "SELECT COUNT(*) FROM a WHERE ((x,y) IN (SELECT c, r FROM w)) AND s <> 'q'", "2");
  expect(db, "expressions on the left", "SELECT s FROM a WHERE (x+0, y*1) IN " + solid + " ORDER BY s", "p;r");
  expect(db, "over a UNION", "SELECT COUNT(*) FROM a WHERE (x, y) IN (SELECT c, r FROM w UNION SELECT 3, 7)", "3");
  expect(db, "nested in a scalar subquery",
         "SELECT (SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE (a.x, a.y) IN " + solid + ") AS nested", "p,r");
  run_ok(db, "CREATE FUNCTION f(px INT, py INT) RETURNS INT RETURN (px, py) IN " + solid);
  expect(db, "in a function body", "SELECT f(1,1), f(1,2), f(9,9), f(NULL,5)", "1|0|1|NULL");
  expect_error(db, "fewer columns", "SELECT (x,y) IN (SELECT c FROM w) FROM a LIMIT 1", "Operand should contain 2 column(s)");
  expect_error(db, "more columns", "SELECT (x,y,s) IN (SELECT c, r FROM w) FROM a LIMIT 1", "Operand should contain 3 column(s)");
  // The same text three times: the answer cache, and the subquery's kept rows, give the same answer.
  for (int i = 0; i < 3; ++i) expect(db, "repeated #" + std::to_string(i + 1), "SELECT COUNT(*) FROM a WHERE (x,y) IN " + solid, "2");

  std::cout << "\n[2] WITH inside a subquery\n";
  expect(db, "scalar subquery", "SELECT (WITH c AS (SELECT 2 AS v) SELECT v * 3 FROM c)", "6");
  expect(db, "recursive, in a derived table",
         "SELECT SUM(n) FROM (WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 4) SELECT n FROM r) t", "10");
  expect(db, "correlated",
         "SELECT s, (WITH c AS (SELECT y FROM a a2 WHERE a2.x = a.x) SELECT COUNT(*) FROM c) AS n FROM a ORDER BY s",
         "p|2;q|2;r|1;s|1;t|0");
  expect(db, "in IN", "SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE x IN (WITH c AS (SELECT 2 AS v UNION SELECT 3) SELECT v FROM c)", "r,s");
  run_ok(db, "CREATE FUNCTION tri(n INT) RETURNS INT RETURN "
             "(WITH RECURSIVE r(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < n) SELECT SUM(i) FROM r)");
  expect(db, "in a function body", "SELECT tri(4), tri(1)", "10|1");
  run_ok(db, "CREATE VIEW cv AS WITH d1 AS (SELECT s, x * 2 AS w FROM a), d2 AS (SELECT s, w + 1 AS w FROM d1) SELECT s, w FROM d2");
  expect(db, "a view that opens with WITH", "SELECT GROUP_CONCAT(w ORDER BY s) FROM cv", "3,3,5,7");
  expect(db, "joined", "SELECT a.s, cv.w FROM a JOIN cv ON cv.s = a.s WHERE a.x = 1 ORDER BY a.s", "p|3;q|3");
  // A view (or a CTE the engine merges as one) with a WHERE on the padded side of a LEFT JOIN keeps the rows it has
  // no match for: its WHERE belongs to the join, not the statement.
  run_ok(db, "CREATE VIEW solid_w AS SELECT c, r FROM w WHERE solid = 1");
  expect(db, "LEFT JOIN a filtered view", "SELECT a.s, sw.r FROM a LEFT JOIN solid_w sw ON sw.c = a.x AND sw.r = a.y ORDER BY a.s",
         "p|1;q|NULL;r|1;s|NULL;t|NULL");
  expect(db, "LEFT JOIN a filtered CTE",
         "WITH sw AS (SELECT c, r FROM w WHERE solid = 1) SELECT a.s, sw.r FROM a LEFT JOIN sw ON sw.c = a.x AND sw.r = a.y ORDER BY a.s",
         "p|1;q|NULL;r|1;s|NULL;t|NULL");
  // [NOT] EXISTS correlated on two columns, against a CTE's table and LATERAL series (the renderer's view_holes):
  // run as a row-value [NOT] IN, hashed once. Each value is what the correlated form gives.
  expect(db, "NOT EXISTS on two columns over a CTE and series",
         "WITH fp AS (SELECT c AS x, r AS y FROM w WHERE solid = 1) "
         "SELECT COUNT(*), MIN(sx.x * 10 + sy.y) FROM generate_series(1, 3) AS sx(x) CROSS JOIN generate_series(1, 2) AS sy(y) "
         "WHERE NOT EXISTS (SELECT 1 FROM fp f WHERE f.x = sx.x AND f.y = sy.y)",
         "4|12");
  expect(db, "EXISTS on two columns, the outer nullable",
         "SELECT GROUP_CONCAT(a.s ORDER BY a.s) FROM a WHERE EXISTS (SELECT 1 FROM w WHERE w.c = a.x AND w.r = a.y AND w.solid = 1)", "p,r");
  expect(db, "NOT EXISTS on two columns, the outer nullable (NULLs are kept)",
         "SELECT GROUP_CONCAT(a.s ORDER BY a.s) FROM a WHERE NOT EXISTS (SELECT 1 FROM w WHERE w.c = a.x AND w.r = a.y AND w.solid = 1)", "q,s,t");
  expect(db, "a CTE read twice is one table",
         "WITH sw AS (SELECT c, r FROM w WHERE solid = 1) SELECT (SELECT COUNT(*) FROM sw) + (SELECT SUM(r) FROM sw)", "20");

  std::cout << "\n[3] LATERAL and generate_series\n";
  expect(db, "generate_series", "SELECT GROUP_CONCAT(n ORDER BY n) FROM generate_series(1, 5) AS g(n)", "1,2,3,4,5");
  expect(db, "a negative step", "SELECT GROUP_CONCAT(g ORDER BY g DESC) FROM generate_series(10, 1, -3) AS g", "10,7,4,1");
  expect(db, "decimal steps", "SELECT GROUP_CONCAT(n ORDER BY n) FROM generate_series(0.5, 2, 0.5) AS g(n)", "0.5,1.0,1.5,2.0");
  expect(db, "empty", "SELECT COUNT(*) FROM generate_series(3, 1) AS g", "0");
  expect(db, "a NULL bound", "SELECT COUNT(*) FROM generate_series(1, NULL) AS g", "0");
  expect(db, "aggregate per outer row",
         "SELECT a.s, t.m FROM a CROSS JOIN LATERAL (SELECT MAX(w.r) AS m FROM w WHERE w.c = a.x) t ORDER BY a.s",
         "p|2;q|2;r|1;s|NULL;t|NULL");
  expect(db, "rows per outer row, comma join",
         "SELECT a.s, t.r FROM a, LATERAL (SELECT w.r FROM w WHERE w.c = a.x) t ORDER BY a.s, t.r", "p|1;p|2;q|1;q|2;r|1");
  expect(db, "LEFT JOIN LATERAL pads",
         "SELECT a.s, t.r FROM a LEFT JOIN LATERAL (SELECT w.r FROM w WHERE w.c = a.x) t ON TRUE ORDER BY a.s, t.r",
         "p|1;p|2;q|1;q|2;r|1;s|NULL;t|NULL");
  expect(db, "LATERAL generate_series",
         "SELECT a.s, g.n FROM a CROSS JOIN LATERAL generate_series(1, a.x) g(n) WHERE a.s IN ('p', 'r') ORDER BY a.s, g.n",
         "p|1;r|1;r|2");
  expect(db, "generate_series reading an earlier source, LATERAL unwritten",
         "SELECT SUM(g) FROM a, generate_series(1, a.x) AS g", "11");
  expect(db, "a LATERAL reading a LATERAL",
         "SELECT COUNT(*) FROM a, LATERAL (SELECT a.x * 2 AS d) t1, LATERAL generate_series(1, t1.d) g(n)", "14");
  expect(db, "* leaves out the join key", "SELECT * FROM (SELECT 1 AS k) z CROSS JOIN LATERAL (SELECT z.k + 1 AS j) t", "1|2");
  expect(db, "nested in a scalar subquery",
         "SELECT (SELECT COUNT(*) FROM a CROSS JOIN LATERAL (SELECT w.r FROM w WHERE w.c = a.x) t)", "5");
  expect(db, "a derived table's column list", "SELECT p + q FROM (SELECT 1, 2) AS t(p, q)", "3");
  expect(db, "a LATERAL inside a LATERAL reads both outer rows",
         "SELECT a.s, t.m FROM a CROSS JOIN LATERAL (SELECT MAX(q.v) AS m FROM w CROSS JOIN LATERAL "
         "(SELECT w.r + a.x AS v) q WHERE w.c = a.x) t WHERE a.x IS NOT NULL ORDER BY a.s",
         "p|3;q|3;r|3;s|NULL");
  run_ok(db, "CREATE FUNCTION tris(p INT) RETURNS INT RETURN (SELECT SUM(g.n) FROM a CROSS JOIN LATERAL generate_series(1, a.x + p) g(n))");
  expect(db, "in a function body", "SELECT tris(0), tris(1)", "11|22");
  expect_error(db, "step 0", "SELECT * FROM generate_series(1, 5, 0) AS g", "cannot be 0");
  expect_error(db, "too many column names", "SELECT * FROM (SELECT 1 AS a) AS t(x, y)", "names for");
  expect_error(db, "LATERAL on a table", "SELECT * FROM a, LATERAL w", "LATERAL takes");

  // SQLDoom's render_settings has a column `near`; AQL's NEAR clause was found by the text "NEAR " anywhere in a
  // SELECT or WITH, so these went to the object store.
  std::cout << "\n[4] near is a name, not AQL's NEAR\n";
  expect(db, "a column named near", "SELECT near + 1 FROM (SELECT 1 AS near) t", "2");
  expect(db, "qualified, in a WITH", "WITH s AS (SELECT 2 AS near) SELECT s.near * 2 FROM s", "4");
  expect(db, "in a comment", "WITH s AS (SELECT 3 AS x) -- the near clip plane\nSELECT x FROM s", "3");
  expect(db, "in a string", "SELECT 'near the end'", "near the end");

  // The PostgreSQL SQLDoom's game tic is written in, where MySQL has no spelling of it (each value is what
  // PostgreSQL 16 answers, written as MySQL writes it: true as 1).
  std::cout << "\n[5] PostgreSQL's spellings\n";
  expect(db, "x::type",
         "SELECT 2.5e0::int, 3.5e0::int, (-2.5e0)::int, 2.5::int, '42'::int + 1, 1::boolean, 'yes'::bool, 'f'::boolean, "
         "12.345::numeric, 12.345::numeric(5,1), 42::text, 'abc'::varchar(2), (3 + 4)::text, -x::bigint "
         "FROM a WHERE s = 'r'",
         "2|4|-2|3|43|1|1|0|12.345|12.3|42|ab|7|-2");
  expect(db, "::float8 divides as a float", "SELECT 7::float8 / 2 = 3.5", "1");
  expect(db, "::an enum type is the label", "SELECT 'stand'::actor_state", "stand");
  expect(db, "::type nested", "SELECT (SELECT MAX(y)::text FROM a) AS m", "2");
  run_ok(db, "CREATE FUNCTION pgc(v DOUBLE) RETURNS INT RETURN v::int");
  expect(db, "::type in a function body", "SELECT pgc(2.5), pgc(3.5)", "2|4");

  expect(db, "IS [NOT] DISTINCT FROM",
         "SELECT s, x IS DISTINCT FROM 1, x IS NOT DISTINCT FROM NULL FROM a ORDER BY s", "p|0|0;q|0|0;r|1|0;s|1|0;t|1|1");
  expect(db, "IS DISTINCT FROM a subquery", "SELECT COUNT(*) FROM a WHERE x IS DISTINCT FROM (SELECT 2)", "4");

  expect(db, "BOOL_AND / BOOL_OR / EVERY", "SELECT BOOL_AND(x > 0), BOOL_OR(x > 2), BOOL_AND(x > 1), EVERY(y = 1) FROM a",
         "1|1|0|0");
  expect(db, "BOOL_OR grouped, NULL for only NULLs",
         "SELECT x, BOOL_OR(y = 2) FROM a WHERE x IS NOT NULL GROUP BY x ORDER BY x", "1|1;2|0;3|NULL");
  expect(db, "BOOL_OR as a window",
         "SELECT s, BOOL_OR(y = 2) OVER (ORDER BY s ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) FROM a ORDER BY s",
         "p|0;q|1;r|1;s|1;t|1");
  expect(db, "ARG_MAX / ARG_MIN", "SELECT ARG_MAX(s, x * 10 + y), ARG_MIN(s, x + y), MAX_BY(s, x) FROM a", "r|p|s");
  expect(db, "ARG_MAX grouped", "SELECT x, ARG_MAX(s, y) FROM a WHERE x IS NOT NULL GROUP BY x ORDER BY x", "1|q;2|r;3|NULL");
  expect(db, "string_agg", "SELECT string_agg(s, '-' ORDER BY s DESC), string_agg(CASE WHEN x > 1 THEN s END, '+' ORDER BY s) FROM a",
         "t-s-r-q-p|r+s");
  expect(db, "string_agg of bytes, ''::bytea", "SELECT HEX(string_agg(UNHEX(LPAD(HEX(x), 2, '0')), ''::bytea ORDER BY s)) FROM a",
         "01010203");
  expect(db, "FILTER (WHERE ..)",
         "SELECT COUNT(*) FILTER (WHERE x = 1), SUM(y) FILTER (WHERE x IS NOT NULL), "
         "string_agg(s, '' ORDER BY s) FILTER (WHERE y = 1) FROM a",
         "2|4|prt");
  expect(db, "FILTER grouped", "SELECT y, COUNT(*) FILTER (WHERE x > 1) FROM a GROUP BY y ORDER BY y", "NULL|1;1|1;2|0");
  run_ok(db, "CREATE FUNCTION nbig() RETURNS INT RETURN (SELECT COUNT(*) FILTER (WHERE x > 1) FROM a)");
  expect(db, "FILTER in a function body", "SELECT nbig()", "2");

  run_ok(db, "CREATE TABLE kv (k INT PRIMARY KEY, v INT, n INT)");
  run_ok(db, "INSERT INTO kv VALUES (1,10,0),(2,20,0)");
  run_ok(db, "INSERT INTO kv VALUES (1,99,0),(3,30,0) ON CONFLICT (k) DO NOTHING");
  run_ok(db, "INSERT INTO kv VALUES (2,25,0) ON CONFLICT (k) DO UPDATE SET v = EXCLUDED.v + kv.v, n = kv.n + 1");
  // every SET reads the row as it was: v and n swap
  run_ok(db, "INSERT INTO kv VALUES (3,0,0) ON CONFLICT (k) DO UPDATE SET v = kv.n, n = kv.v");
  run_ok(db, "INSERT INTO kv VALUES (1,5,0),(2,5,0) ON CONFLICT (k) DO UPDATE SET v = EXCLUDED.v WHERE kv.v > 20");
  expect(db, "ON CONFLICT", "SELECT GROUP_CONCAT(CONCAT(k,':',v,':',n) ORDER BY k) FROM kv", "1:10:0,2:5:1,3:0:30");
  run_ok(db, "INSERT INTO kv SELECT k + 2, 1, 1 FROM kv ON CONFLICT DO NOTHING");
  expect(db, "INSERT .. SELECT .. ON CONFLICT DO NOTHING", "SELECT COUNT(*), SUM(k) FROM kv", "5|15");
  run_ok(db, "CREATE PROCEDURE upk(IN kk INT) BEGIN INSERT INTO kv VALUES (kk, 1, 0) ON CONFLICT (k) DO UPDATE SET n = kv.n + 1; END");
  run_ok(db, "CALL upk(1)");
  run_ok(db, "CALL upk(1)");
  expect(db, "ON CONFLICT in a procedure", "SELECT n FROM kv WHERE k = 1", "2");
  // WITH .. INSERT INTO t in a routine: t is the table written, not a SELECT .. INTO variable
  run_ok(db, "CREATE PROCEDURE wins() BEGIN WITH s AS (SELECT 40 AS k) INSERT INTO kv (k, v, n) SELECT k, 4, 4 FROM s; END");
  run_ok(db, "CALL wins()");
  expect(db, "WITH .. INSERT INTO in a procedure", "SELECT v FROM kv WHERE k = 40", "4");

  run_ok(db, "CREATE TABLE pts (id INT PRIMARY KEY, x INT, tot INT)");
  run_ok(db, "INSERT INTO pts VALUES (1,1,0),(2,2,0),(3,9,0)");
  run_ok(db, "UPDATE pts p SET tot = q.n, x = p.x * 10 FROM (SELECT x, COUNT(*) AS n FROM a GROUP BY x) q WHERE q.x = p.x");
  expect(db, "UPDATE .. FROM", "SELECT id, x, tot FROM pts ORDER BY id", "1|10|2;2|20|1;3|9|0");
  run_ok(db, "UPDATE pts SET tot = tot + w.r FROM w WHERE w.c = pts.id AND w.solid = 1");
  expect(db, "UPDATE .. FROM, no alias", "SELECT id, x, tot FROM pts ORDER BY id", "1|10|3;2|20|2;3|9|0");
  run_ok(db, "CREATE PROCEDURE bump() BEGIN UPDATE pts p SET tot = p.tot + w.c FROM w WHERE w.c = p.id; END");
  run_ok(db, "CALL bump()");
  expect(db, "UPDATE .. FROM in a procedure", "SELECT id, tot FROM pts ORDER BY id", "1|4;2|4;3|0");
  run_ok(db, "DELETE FROM pts p WHERE p.id = 3");
  run_ok(db, "DELETE FROM pts AS p WHERE p.tot = 4 AND p.id = 2");
  expect(db, "DELETE FROM t alias", "SELECT GROUP_CONCAT(id) FROM pts", "1");
  run_ok(db, "DELETE FROM kv k USING w WHERE w.c = k.k AND w.solid = 1");
  expect(db, "DELETE FROM t alias USING ..", "SELECT GROUP_CONCAT(k ORDER BY k) FROM kv", "3,4,5,40");

  run_ok(db, "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')");
  run_ok(db, "CREATE TABLE person (name VARCHAR(10), m mood)");
  run_ok(db, "INSERT INTO person VALUES ('a', 'ok')");
  run_ok(db, "ALTER TABLE person ADD COLUMN m2 mood");
  expect(db, "CREATE TYPE .. AS ENUM", "SELECT m, m2, 'happy'::mood FROM person", "ok|NULL|happy");
  {
    SqlQueryResult r = q(db, "SHOW CREATE TABLE person");
    const std::string ddl = r.success && !r.rows.empty() && r.rows[0].values.size() > 1 ? r.rows[0].values[1] : std::string();
    if (ddl.find("enum('sad','ok','happy')") == std::string::npos) fail("the column is the enum", ddl);
    else std::cout << "  [ok] the column is the enum\n";
  }
  expect_error(db, "CREATE TYPE twice", "CREATE TYPE mood AS ENUM ('x')", "already exists");
  run_ok(db, "DROP TYPE mood");
  run_ok(db, "DROP TYPE IF EXISTS mood");
  expect_error(db, "DROP TYPE of none", "DROP TYPE mood", "does not exist");

  expect(db, "|| is OR", "SELECT 'a' || 'b', 0 || 1", "0|1");
  run_ok(db, "SET sql_mode = 'PIPES_AS_CONCAT'");
  expect(db, "|| under PIPES_AS_CONCAT", "SELECT 'a' || 'b', 0 || 1", "ab|01");
  expect(db, "|| with NULL, and nested", "SELECT NULL || 'x', (SELECT s || '!' FROM a WHERE s = 'p')", "NULL|p!");
  run_ok(db, "CREATE FUNCTION cat2(a1 TEXT, b1 TEXT) RETURNS TEXT RETURN a1 || b1");
  run_ok(db, "SET sql_mode = DEFAULT");
  expect(db, "|| is OR again (the same text)", "SELECT 'a' || 'b', 0 || 1", "0|1");
  run_ok(db, "CREATE FUNCTION or2(a1 INT, b1 INT) RETURNS INT RETURN a1 || b1");
  expect(db, "a function keeps the sql_mode it was made under", "SELECT cat2('x', 'y'), or2(0, 1)", "xy|1");

  expect(db, "TRUNC, CHR, GET_BYTE", "SELECT TRUNC(2.7), TRUNC(-2.7), TRUNC(1.2345, 2), CHR(65), GET_BYTE(X'0a0bff', 2)", "2|-2|1.23|A|255");
  expect_error(db, "GET_BYTE past the end", "SELECT GET_BYTE(X'0a', 1)", "out of valid range");
  // A routine's variable holds what its declared type holds (MySQL converts a value assigned to one).
  run_ok(db, "CREATE FUNCTION third(p DOUBLE) RETURNS DOUBLE BEGIN DECLARE x DOUBLE DEFAULT p; DECLARE n INT DEFAULT 2.6; "
             "SET x = x / 3; SET n = n + 0.4; RETURN x * 3 + n; END");
  expect(db, "a DOUBLE variable divides as a double, an INT one rounds", "SELECT third(2) = 5", "1");

  // sql_mode PG_DIVISION (ASQL's): PostgreSQL's `/`.
  expect(db, "/ is MySQL's", "SELECT 7 / 2, -7 / 2", "3.5000|-3.5000");
  run_ok(db, "SET sql_mode = 'PG_DIVISION'");
  expect(db, "PG_DIVISION: integers truncate", "SELECT 7 / 2, -7 / 2, 7.0 / 2 = 3.5, 1 / 3.0 > 0.33333333, 7e0 / 2", "3|-3|1|1|3.5");
  expect(db, "PG_DIVISION over rows", "SELECT s, x / 2 FROM a ORDER BY s", "p|0;q|0;r|1;s|1;t|NULL");
  expect(db, "PG_DIVISION in a CTE and SUM", "WITH c AS (SELECT x / 2 AS h FROM a) SELECT GROUP_CONCAT(h ORDER BY h), SUM(h), "
                                             "(SELECT SUM(x / 2) FROM a) FROM c",
         "0,0,1,1|2|2");
  expect_error(db, "PG_DIVISION: by zero is an error", "SELECT 1 / 0", "division by zero");
  run_ok(db, "CREATE FUNCTION half(n INT) RETURNS INT RETURN n / 2 * 2");
  run_ok(db, "SET sql_mode = DEFAULT");
  expect(db, "a function keeps PG_DIVISION", "SELECT half(7), 7 / 2", "6|3.5000");

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
