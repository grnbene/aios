// Checks LOAD DATA [LOCAL] INFILE as MariaDB reads it: the file's formats, NULLs and escapes, defaults for missing
// fields, SET and @variables, IGNORE n LINES, duplicate keys (error, IGNORE, REPLACE), that a load is one statement
// (a failure part-way - in the second batch of 16,384 rows too - leaves none of its rows, alone or inside a
// transaction), the rules for reading a file on the server (AIOS_SQL_SECURE_FILE_PRIV: unset refuses, no way out of
// the directory through .. or a link), LOCAL, the result cache not keeping a count across a load, and the rows
// surviving a restart.
// Before LOAD DATA existed every statement here failed with "unsupported statement: LOAD".
#undef NDEBUG
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/asql_capability.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;
void expect(bool ok, const std::string& what) {
  ++checks;
  if (!ok) { ++failures; std::printf("  [FAIL] %s\n", what.c_str()); }
  else std::printf("  [ok] %s\n", what.c_str());
}
void expect_eq(const std::string& got, const std::string& want, const std::string& what) {
  ++checks;
  if (got != want) { ++failures; std::printf("  [FAIL] %s: got [%s], want [%s]\n", what.c_str(), got.c_str(), want.c_str()); }
  else std::printf("  [ok] %s: %s\n", what.c_str(), got.c_str());
}
SqlQueryResult q(SqlDatabase& db, const std::string& sql) { return db.execute_gateway_query(sql); }
SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}
// Every cell of every row: cells joined by '|', rows by ';'.
std::string all(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  std::string s;
  for (const auto& row : r.rows) {
    if (!s.empty()) s += ';';
    for (size_t i = 0; i < row.values.size(); ++i) s += (i ? "|" : "") + sql_display(row.values[i]);
  }
  return s;
}
std::string cell(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  return r.rows.empty() || r.rows[0].values.empty() ? std::string("?") : sql_display(r.rows[0].values[0]);
}
void put(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary);
  f << bytes;
}
bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

int main() {
  std::printf("=== ASQL LOAD DATA INFILE ===\n");
  const CheckRoot check_root("/tmp/aios-asql-load-data");
  const std::string dir = check_root.path + "/files";
  const std::string outside = check_root.path + "/outside";
  ::mkdir(dir.c_str(), 0755);
  ::mkdir(outside.c_str(), 0755);
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "CREATE DATABASE IF NOT EXISTS ld");
    run(db, "USE ld");

    // 0. a server-side file with no AIOS_SQL_SECURE_FILE_PRIV: refused
    put(dir + "/a.tsv", "1\tapple\t1.50\n");
    run(db, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20), price DECIMAL(8,2) NULL)");
    ::unsetenv("AIOS_SQL_SECURE_FILE_PRIV");
    {
      SqlQueryResult r = q(db, "LOAD DATA INFILE '" + dir + "/a.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "secure-file-priv"), "0. no AIOS_SQL_SECURE_FILE_PRIV: a server file is refused");
      expect_eq(cell(db, "SELECT COUNT(*) FROM t"), "0", "0. ... and nothing was loaded");
    }
    ::setenv("AIOS_SQL_SECURE_FILE_PRIV", dir.c_str(), 1);

    // 1. the default format: tab, newline, backslash escapes, \N
    put(dir + "/a.tsv", "1\tapple\t1.50\n2\tbanana\t\\N\n3\ta\\tb\\\\c\t7\n");
    {
      SqlQueryResult r = run(db, "LOAD DATA INFILE '" + dir + "/a.tsv' INTO TABLE t");
      expect_eq(std::to_string(r.rows_affected), "3", "1. rows affected");
      expect_eq(r.ok_info, "Records: 3  Deleted: 0  Skipped: 0  Warnings: 0", "1. the OK info");
      expect_eq(all(db, "SELECT id, name, price FROM t ORDER BY id"), "1|apple|1.50;2|banana|NULL;3|a\tb\\c|7.00",
                "1. default format, \\N is NULL, \\t and \\\\ are a tab and a backslash");
    }

    // 2. CSV: enclosed fields with the terminator in them, a doubled quote, the word NULL
    run(db, "CREATE TABLE csv (id INT, name VARCHAR(40), note VARCHAR(20))");
    put(dir + "/b.csv", "1,\"Smith, John\",x\n2,\"say \"\"hi\"\"\",NULL\n3,plain,\"NULL\"\n");
    run(db, "LOAD DATA INFILE 'b.csv' INTO TABLE csv FIELDS TERMINATED BY ',' OPTIONALLY ENCLOSED BY '\"'");
    expect_eq(all(db, "SELECT id, name, IFNULL(note, '<null>') FROM csv ORDER BY id"),
              "1|Smith, John|x;2|say \"hi\"|<null>;3|plain|NULL",
              "2. enclosed fields: comma inside, doubled quote, unquoted NULL is NULL, quoted \"NULL\" is text (relative file name)");

    // 3. IGNORE 1 LINES, LINES TERMINATED BY, LINES STARTING BY
    run(db, "CREATE TABLE ln (id INT, name VARCHAR(20))");
    put(dir + "/c.txt", "id,name\r\n1,one\r\n2,two\r\n");
    run(db, "LOAD DATA INFILE 'c.txt' INTO TABLE ln FIELDS TERMINATED BY ',' LINES TERMINATED BY '\\r\\n' IGNORE 1 LINES");
    expect_eq(all(db, "SELECT id, name FROM ln ORDER BY id"), "1|one;2|two", "3. IGNORE 1 LINES and CRLF lines");
    put(dir + "/d.txt", "junk\nrow:3,three\nnoise\nrow:4,four\n");
    run(db, "LOAD DATA INFILE 'd.txt' INTO TABLE ln FIELDS TERMINATED BY ',' LINES STARTING BY 'row:'");
    expect_eq(all(db, "SELECT id, name FROM ln WHERE id > 2 ORDER BY id"), "3|three;4|four", "3. LINES STARTING BY skips lines without the prefix");

    // 4. a column list with a variable, and SET
    run(db, "CREATE TABLE st (id INT, name VARCHAR(20), price DECIMAL(8,2))");
    put(dir + "/e.csv", "1,milk,2.50\n2,eggs,3.25\n");
    run(db, "LOAD DATA INFILE 'e.csv' INTO TABLE st FIELDS TERMINATED BY ',' (id, @n, @p) SET name = UPPER(@n), price = @p * 2");
    expect_eq(all(db, "SELECT id, name, price FROM st ORDER BY id"), "1|MILK|5.00;2|EGGS|6.50", "4. (id, @n, @p) SET name = UPPER(@n), price = @p * 2");

    // 5. fewer fields than columns: the column's default, else the empty value; a warning each
    run(db, "CREATE TABLE df (id INT, tag VARCHAR(10) DEFAULT 'none', n INT NOT NULL, d VARCHAR(5) NULL)");
    put(dir + "/f.csv", "1\n2,custom\n");
    {
      SqlQueryResult r = run(db, "LOAD DATA INFILE 'f.csv' INTO TABLE df FIELDS TERMINATED BY ','");
      expect_eq(all(db, "SELECT id, tag, n, IFNULL(d, '<null>') FROM df ORDER BY id"), "1|none|0|<null>;2|custom|0|<null>",
                "5. missing fields: DEFAULT 'none', NOT NULL INT 0, nullable NULL");
      expect(contains(r.ok_info, "Warnings: 2"), "5. each short line is a warning (" + r.ok_info + ")");
    }

    // 6. duplicate keys: error (and nothing of the file), IGNORE, REPLACE
    put(dir + "/g.tsv", "10\tten\t1\n11\televen\t2\n1\tdup\t3\n12\ttwelve\t4\n");
    {
      SqlQueryResult r = q(db, "LOAD DATA INFILE 'g.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "Duplicate"), "6. a duplicate key is an error: " + r.error_message);
      expect_eq(cell(db, "SELECT COUNT(*) FROM t"), "3", "6. ... and the rows before it in the file are gone too (one statement)");
    }
    {
      SqlQueryResult r = run(db, "LOAD DATA INFILE 'g.tsv' IGNORE INTO TABLE t");
      expect_eq(std::to_string(r.rows_affected), "3", "6. IGNORE: rows loaded");
      expect(contains(r.ok_info, "Records: 4") && contains(r.ok_info, "Skipped: 1"), "6. IGNORE: 4 records, 1 skipped (" + r.ok_info + ")");
      expect_eq(cell(db, "SELECT name FROM t WHERE id = 1"), "apple", "6. IGNORE: the existing row stays");
    }
    {
      SqlQueryResult r = run(db, "LOAD DATA INFILE 'g.tsv' REPLACE INTO TABLE t");
      expect_eq(cell(db, "SELECT name FROM t WHERE id = 1"), "dup", "6. REPLACE: the row is the file's");
      expect(contains(r.ok_info, "Records: 4"), "6. REPLACE: 4 records (" + r.ok_info + ")");
      expect_eq(cell(db, "SELECT COUNT(*) FROM t"), "6", "6. REPLACE: 6 rows now");
    }

    // 7. a transaction: ROLLBACK takes the load back, COMMIT keeps it, a failing load takes back only itself
    run(db, "CREATE TABLE tx (id INT PRIMARY KEY, v INT)");
    put(dir + "/h.tsv", "1\t1\n2\t2\n3\t3\n");
    run(db, "BEGIN");
    run(db, "LOAD DATA INFILE 'h.tsv' INTO TABLE tx");
    expect_eq(cell(db, "SELECT COUNT(*) FROM tx"), "3", "7. inside the transaction the rows are there");
    run(db, "ROLLBACK");
    expect_eq(cell(db, "SELECT COUNT(*) FROM tx"), "0", "7. ROLLBACK takes the load back");
    run(db, "BEGIN");
    run(db, "INSERT INTO tx VALUES (100, 100)");
    put(dir + "/h2.tsv", "5\t5\n100\t0\n");
    {
      SqlQueryResult r = q(db, "LOAD DATA INFILE 'h2.tsv' INTO TABLE tx");
      expect(!r.success, "7. a load that meets a duplicate fails inside a transaction");
    }
    expect_eq(all(db, "SELECT id FROM tx ORDER BY id"), "100", "7. ... taking back only its own rows: the transaction's earlier INSERT stays");
    run(db, "LOAD DATA INFILE 'h.tsv' INTO TABLE tx");
    run(db, "COMMIT");
    expect_eq(cell(db, "SELECT COUNT(*) FROM tx"), "4", "7. COMMIT keeps both");

    // 8. more than one batch (16,384 rows): the whole file, and a failure in the second batch takes all of it back
    run(db, "CREATE TABLE big (id INT PRIMARY KEY, grp INT, v DOUBLE)");
    {
      std::string body;
      for (int i = 1; i <= 40000; ++i) body += std::to_string(i) + "\t" + std::to_string(i % 7) + "\t" + std::to_string(i) + ".5\n";
      put(dir + "/big.tsv", body);
      SqlQueryResult r = run(db, "LOAD DATA INFILE 'big.tsv' INTO TABLE big");
      expect_eq(std::to_string(r.rows_affected), "40000", "8. 40,000 rows (three batches)");
      expect_eq(cell(db, "SELECT COUNT(*) FROM big"), "40000", "8. all of them are there");
      expect_eq(cell(db, "SELECT SUM(v) FROM big"), "800040000", "8. their sum (40000 * 40001 / 2 + 40000 * 0.5)");
      std::string dup = body.substr(0, body.find("\n20001\t")) + "\n5\t0\t0.5\n";   // key 5 again, in the second batch
      put(dir + "/big2.tsv", dup);
      run(db, "CREATE TABLE big2 (id INT PRIMARY KEY, grp INT, v DOUBLE)");
      SqlQueryResult f = q(db, "LOAD DATA INFILE 'big2.tsv' INTO TABLE big2");
      expect(!f.success && contains(f.error_message, "Duplicate"), "8. a duplicate in the second batch is an error");
      expect_eq(cell(db, "SELECT COUNT(*) FROM big2"), "0", "8. ... and the first batches are taken back");
    }

    // 9. the rules for a file on the server
    {
      put(outside + "/x.tsv", "50\tout\t1\n");
      SqlQueryResult r = q(db, "LOAD DATA INFILE '../outside/x.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "secure-file-priv"), "9. a .. out of the directory is refused");
      r = q(db, "LOAD DATA INFILE '" + outside + "/x.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "secure-file-priv"), "9. an absolute path outside is refused");
      ::symlink((outside + "/x.tsv").c_str(), (dir + "/link.tsv").c_str());
      r = q(db, "LOAD DATA INFILE 'link.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "secure-file-priv"), "9. a symbolic link inside the directory is refused");
      ::symlink(outside.c_str(), (dir + "/linkdir").c_str());
      r = q(db, "LOAD DATA INFILE 'linkdir/x.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "secure-file-priv"), "9. a path through a linked directory is refused");
      r = q(db, "LOAD DATA INFILE 'nothing.tsv' INTO TABLE t");
      expect(!r.success && contains(r.error_message, "not found"), "9. a file that is not there: " + r.error_message);
      expect_eq(cell(db, "SELECT COUNT(*) FROM t WHERE id = 50"), "0", "9. nothing came in through any of them");
    }

    // 10. LOCAL: the client's file. Not over the wire this process is the client and reads it; over the wire the bytes
    // are what the client sent, or nothing was sent
    run(db, "CREATE TABLE lc (id INT, name VARCHAR(20))");
    put(outside + "/local.csv", "1,from client\n2,second\n");
    run(db, "LOAD DATA LOCAL INFILE '" + outside + "/local.csv' INTO TABLE lc FIELDS TERMINATED BY ','");
    expect_eq(all(db, "SELECT id, name FROM lc ORDER BY id"), "1|from client;2|second", "10. LOCAL reads the client's file (any path)");
    run(db, "DELETE FROM lc");
    {
      const std::string sent = "7,sent over the wire\n8,second\n";
      sql_set_local_infile(true, &sent);
      SqlQueryResult r = q(db, "LOAD DATA LOCAL INFILE 'whatever' INTO TABLE lc FIELDS TERMINATED BY ','");
      sql_set_local_infile(false, nullptr);
      expect(r.success, "10. LOCAL over the wire loads the bytes the client sent");
      expect_eq(all(db, "SELECT id, name FROM lc ORDER BY id"), "7|sent over the wire;8|second", "10. ... and they are the rows");
      sql_set_local_infile(true, nullptr);
      r = q(db, "LOAD DATA LOCAL INFILE 'whatever' INTO TABLE lc");
      sql_set_local_infile(false, nullptr);
      expect(!r.success && contains(r.error_message, "local infile"), "10. a client that sent nothing: " + r.error_message);
    }

    // 11. other errors
    {
      SqlQueryResult r = q(db, "LOAD DATA INFILE 'a.tsv' INTO TABLE nosuch");
      expect(!r.success && contains(r.error_message, "nosuch"), "11. an unknown table: " + r.error_message);
      r = q(db, "LOAD DATA INFILE 'a.tsv' INTO TABLE t (id, nope)");
      expect(!r.success && contains(r.error_message, "nope"), "11. an unknown column: " + r.error_message);
      run(db, "CREATE VIEW vw AS SELECT id FROM t");
      r = q(db, "LOAD DATA INFILE 'a.tsv' INTO TABLE vw");
      expect(!r.success, "11. a view is refused");
    }

    // 12. latin1 file
    run(db, "CREATE TABLE l1 (id INT, name VARCHAR(20))");
    put(dir + "/l1.csv", std::string("1,caf\xE9\n"));
    run(db, "LOAD DATA INFILE 'l1.csv' INTO TABLE l1 CHARACTER SET latin1 FIELDS TERMINATED BY ','");
    expect_eq(cell(db, "SELECT name FROM l1"), "caf\xC3\xA9", "12. CHARACTER SET latin1: the byte E9 is e-acute");

    // 13. the result cache: a count asked twice (so cached) is the new count after a load
    run(db, "CREATE TABLE cc (id INT)");
    put(dir + "/cc.tsv", "1\n2\n3\n");
    cell(db, "SELECT COUNT(*) FROM cc"); cell(db, "SELECT COUNT(*) FROM cc");
    expect_eq(cell(db, "SELECT COUNT(*) FROM cc"), "0", "13. cached count before");
    run(db, "LOAD DATA INFILE 'cc.tsv' INTO TABLE cc");
    expect_eq(cell(db, "SELECT COUNT(*) FROM cc"), "3", "13. the count after a load is the new one, not the cached one");

    // 15. a capability login (scoped rights) cannot run LOAD DATA, whatever rights it carries
    {
      AsqlAuthority auth;
      auth.rights = 0x3f;   // every right
      const std::string why = db.authorize_statement("LOAD DATA INFILE 'a.tsv' INTO TABLE t", auth, "ld");
      expect(why.find("not available under a capability") != std::string::npos, "15. LOAD DATA under a capability: " + why);
    }
  }

  // 14. a restart: the loaded rows are there
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "USE ld");
    expect_eq(cell(db, "SELECT COUNT(*) FROM big"), "40000", "14. after a restart: the 40,000-row load");
    expect_eq(all(db, "SELECT id, name FROM csv ORDER BY id"), "1|Smith, John;2|say \"hi\";3|plain", "14. after a restart: the CSV rows");
    expect_eq(cell(db, "SELECT COUNT(*) FROM tx"), "4", "14. after a restart: the transaction's rows");
  }
  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d LOAD DATA CHECKS FAILED ===\n", failures); return 1; }
  std::printf("=== ALL LOAD DATA CHECKS PASSED ===\n");
  return 0;
}
