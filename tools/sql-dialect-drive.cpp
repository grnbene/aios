// AIOS — Driver for SqlDialectTranslator (MariaDB -> PostgreSQL / T-SQL).
// Prints each statement's translations; with --pg=host:port:user:pass:db each
// PostgreSQL translation is also run on that server (in order: the tables the
// first statements create are the ones the later ones write), and anything
// the server refuses is a failure. --mssql=... does the same over TDS.
#include "aios/userland/pg_wire.hpp"
#include "aios/userland/sql_dialect.hpp"
#include "aios/userland/tds_wire.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace aios::userland;

namespace {

struct Case {
  const char* name;
  const char* sql;
  const char* check;   // a query whose single value must be `want` afterwards ("" = none)
  const char* want;
};

// The tables the cases write, as ASQL would know them (key, columns, AUTO_INCREMENT).
bool lookup(const std::string& table, SqlDialectTranslator::TableInfo* out) {
  static const std::map<std::string, SqlDialectTranslator::TableInfo> tables = {
      {"dl_claims", {{"claim_id"}, {"claim_id", "patient_id", "paid_amount", "adjudicated", "notes", "created_at"}, "claim_id"}},
      {"dl_tags", {{"k"}, {"k", "n"}, ""}},
  };
  auto it = tables.find(table);
  if (it == tables.end()) return false;
  *out = it->second;
  return true;
}

std::vector<std::string> split(const std::string& s, char c) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    size_t e = s.find(c, at);
    out.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
    if (e == std::string::npos) return out;
    at = e + 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::unique_ptr<SqlWireSession> pg, ms;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--pg=", 0) == 0 || a.rfind("--mssql=", 0) == 0) {
      const bool is_pg = a[2] == 'p';
      auto p = split(a.substr(is_pg ? 5 : 8), ':');
      if (p.size() != 5) { fprintf(stderr, "--pg / --mssql = host:port:user:pass:db\n"); return 2; }
      std::unique_ptr<SqlWireSession> s;
      if (is_pg) s = std::make_unique<PgWireSession>(p[0], (uint16_t)std::atoi(p[1].c_str()));
      else s = std::make_unique<TdsWireSession>(p[0], (uint16_t)std::atoi(p[1].c_str()));
      s->set_tls("prefer", "");
      auto c = s->connect(p[0], (uint16_t)std::atoi(p[1].c_str()), p[2], p[3], p[4]);
      if (!c.ok()) { fprintf(stderr, "connect: %s\n", c.error().message.c_str()); return 1; }
      (is_pg ? pg : ms) = std::move(s);
    }
  }

  const Case cases[] = {
      {"drop", "DROP TABLE IF EXISTS `dl_claims`", "", ""},
      {"drop2", "DROP TABLE IF EXISTS dl_tags", "", ""},
      {"create table: types, AUTO_INCREMENT, inline KEY/UNIQUE, options",
       "CREATE TABLE `dl_claims` (`claim_id` INT(11) UNSIGNED NOT NULL AUTO_INCREMENT, `patient_id` INT NOT NULL COMMENT 'who', "
       "`paid_amount` DECIMAL(10,2) DEFAULT '0.00', `adjudicated` TINYINT(1) DEFAULT 0, `notes` TEXT CHARACTER SET utf8mb4, "
       "`created_at` DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY (`claim_id`), KEY `ix_patient` (`patient_id`), "
       "UNIQUE KEY `uq_patient_paid` (`patient_id`,`paid_amount`)) ENGINE=InnoDB AUTO_INCREMENT=100 DEFAULT CHARSET=utf8mb4 COMMENT='claims'",
       "", ""},
      {"create table if not exists", "CREATE TABLE IF NOT EXISTS dl_tags (k VARCHAR(20) PRIMARY KEY, n INT)", "", ""},
      {"insert: a string that says TRUE, NOW() and a type name is not rewritten",
       "INSERT INTO `dl_claims` (`claim_id`, `patient_id`, `paid_amount`, `adjudicated`, `notes`) VALUES (1, 10, 5.5, TRUE, 'TRUE at NOW() is a DATETIME')",
       "SELECT notes FROM dl_claims WHERE claim_id = 1", "TRUE at NOW() is a DATETIME"},
      {"insert: MySQL backslash escapes", "INSERT INTO dl_claims (claim_id, patient_id, notes) VALUES (2, 11, 'It\\'s a \\\\path\\nline2')",
       "SELECT notes FROM dl_claims WHERE claim_id = 2", "It's a \\path\nline2"},
      {"insert: non-ASCII text", "INSERT INTO dl_claims (claim_id, patient_id, notes) VALUES (3, 12, 'Zoë — naïve ✓')",
       "SELECT notes FROM dl_claims WHERE claim_id = 3", "Zoë — naïve ✓"},
      {"upsert: ON DUPLICATE KEY UPDATE with VALUES() and the old value, functions in VALUES",
       "INSERT INTO dl_claims (claim_id, patient_id, paid_amount) VALUES (1, 10, ROUND(7.25, 1)), (4, 13, 1) "
       "ON DUPLICATE KEY UPDATE paid_amount = paid_amount + VALUES(paid_amount)",
       "SELECT paid_amount FROM dl_claims WHERE claim_id = 1", "12.80"},
      {"insert ignore", "INSERT IGNORE INTO dl_tags (k, n) VALUES ('a', 1), ('a', 2)", "SELECT n FROM dl_tags WHERE k = 'a'", "1"},
      {"replace", "REPLACE INTO dl_tags (k, n) VALUES ('a', 9)", "SELECT n FROM dl_tags WHERE k = 'a'", "9"},
      {"insert ... set", "INSERT INTO dl_tags SET k = 'b', n = 2", "SELECT n FROM dl_tags WHERE k = 'b'", "2"},
      {"update with IF, IFNULL, DATE_ADD", "UPDATE dl_claims SET notes = IF(patient_id > 10, IFNULL(notes, 'none'), 'low'), "
                                           "created_at = DATE_ADD('2026-01-31', INTERVAL 1 MONTH) WHERE claim_id = 4",
       "SELECT CAST(created_at AS DATE) FROM dl_claims WHERE claim_id = 4", "2026-02-28"},
      {"|| is OR in MySQL", "UPDATE dl_tags SET n = 5 WHERE k = 'zz' || k = 'b'", "SELECT n FROM dl_tags WHERE k = 'b'", "5"},
      {"select with LIMIT offset, count", "SELECT k FROM dl_tags ORDER BY k LIMIT 1, 1", "", ""},
      {"date arithmetic", "SELECT claim_id FROM dl_claims WHERE created_at >= DATE_SUB(NOW(), INTERVAL 30 DAY) LIMIT 5", "", ""},
      {"unix timestamp", "SELECT UNIX_TIMESTAMP() > 0", "", ""},
      {"alter table add/modify/index", "ALTER TABLE dl_tags ADD COLUMN note VARCHAR(40) DEFAULT 'x' AFTER n, ADD INDEX ix_n (n), MODIFY n BIGINT NOT NULL", "",
       ""},
      {"set names: nothing to send", "SET NAMES utf8mb4", "", ""},
      {"delete", "DELETE FROM `dl_tags` WHERE `k` = 'b'", "SELECT COUNT(*) FROM dl_tags", "1"},
  };

  int failed = 0;
  for (const auto& c : cases) {
    printf("--- %s\n  MariaDB : %s\n", c.name, c.sql);
    for (int d = 0; d < 2; ++d) {
      const SqlDialect target = d == 0 ? SqlDialect::Postgres : SqlDialect::MsSql;
      SqlWireSession* s = d == 0 ? pg.get() : ms.get();
      auto tr = SqlDialectTranslator::translate_checked(c.sql, SqlDialect::MariaDB, target, lookup);
      printf("  %-8s: %s\n", d == 0 ? "Postgres" : "T-SQL", tr.ok() ? (tr->empty() ? "(nothing to send)" : tr->c_str()) : ("ERROR " + tr.error().message).c_str());
      if (!tr.ok()) { ++failed; continue; }
      if (!s || tr->empty()) continue;
      auto r = s->query(*tr);
      if (!r.ok() || !r->ok) {
        printf("    ✗ the server refused it: %s\n", r.ok() ? r->error_message.c_str() : r.error().message.c_str());
        ++failed;
        continue;
      }
      if (c.check[0]) {
        auto chk = SqlDialectTranslator::translate_checked(c.check, SqlDialect::MariaDB, target, lookup);
        auto v = chk.ok() ? s->query(*chk) : aios::Result<SqlWireResult>(chk.error());
        const std::string got = v.ok() && v->ok && !v->rows.empty() && !v->rows[0].empty() ? v->rows[0][0].bytes : "(no value)";
        if (got != c.want) { printf("    ✗ %s gave [%s], want [%s]\n", c.check, got.c_str(), c.want); ++failed; }
        else printf("    ✓ ran; %s = [%s]\n", c.check, got.c_str());
      } else {
        printf("    ✓ ran\n");
      }
    }
  }
  printf("\n%d failure(s)\n", failed);
  return failed == 0 ? 0 : 1;
}
