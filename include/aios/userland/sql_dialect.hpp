// AIOS — Native SQL Dialect Translation Engine.
// Rewrites the MariaDB/MySQL statements ASQL records into PostgreSQL or
// T-SQL for a downstream of that kind (write-behind, write-through). It reads
// the statement as tokens, so string literals and quoted names are never
// rewritten - only the SQL around them - and the literals are written the way
// the target reads them (MySQL's backslash escapes decoded; N'...' for T-SQL).
#pragma once

#include "aios/core/result.hpp"
#include "aios/userland/sql_wire.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aios::userland {

class SqlDialectTranslator {
 public:
  // What a statement's table is, for the rewrites that need it: the key an
  // upsert conflicts on, the columns of an INSERT without a column list, the
  // AUTO_INCREMENT column (T-SQL needs IDENTITY_INSERT to write it).
  struct TableInfo {
    std::vector<std::string> key;       // primary key, else the first unique key
    std::vector<std::string> columns;   // in table order
    std::string identity;               // the AUTO_INCREMENT column, if any
  };
  using TableLookup = std::function<bool(const std::string& table, TableInfo* out)>;

  // The statement in the target dialect; "" when it means nothing there (SET
  // NAMES, SET sql_mode, ...: nothing to send). An error when it cannot be
  // written faithfully (an upsert on a table whose key is unknown, ...).
  static Result<std::string> translate_checked(const std::string& sql, SqlDialect source, SqlDialect target,
                                               const TableLookup& lookup = nullptr);

  // The same, for callers that print the translation: an error comes back as
  // "-- untranslatable: <why>".
  static std::string translate(const std::string& sql,
                               SqlDialect source = SqlDialect::MariaDB,
                               SqlDialect target = SqlDialect::MariaDB);

  static std::string to_mssql(const std::string& sql, SqlDialect source = SqlDialect::MariaDB) {
    return translate(sql, source, SqlDialect::MsSql);
  }
  static std::string to_postgres(const std::string& sql, SqlDialect source = SqlDialect::MariaDB) {
    return translate(sql, source, SqlDialect::Postgres);
  }

  // An identifier quoted for the dialect (PostgreSQL's folded to lower case,
  // as its unquoted names are, so `Orders` and Orders are the same table there).
  static std::string quote_identifier(const std::string& ident, SqlDialect dialect);
};

}  // namespace aios::userland
