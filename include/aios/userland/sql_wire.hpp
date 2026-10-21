// AIOS — Abstract SqlWireSession interface and dialect definitions.
// Universal wire session contract for MariaDB/MySQL, PostgreSQL, and MS SQL (TDS).
#pragma once

#include "aios/core/result.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aios::userland {

// Relational dialect supported by the gateway and wire sessions.
enum class SqlDialect {
  MariaDB,   // MariaDB / MySQL (wire port 3306)
  Postgres,  // PostgreSQL Frontend/Backend v3 (wire port 5432)
  MsSql      // Microsoft SQL Server TDS 7.4+ (wire port 1433)
};

inline const char* sql_dialect_name(SqlDialect d) {
  switch (d) {
    case SqlDialect::MariaDB: return "mariadb";
    case SqlDialect::Postgres: return "postgres";
    case SqlDialect::MsSql: return "mssql";
  }
  return "unknown";
}

inline Result<SqlDialect> parse_sql_dialect(const std::string& name) {
  if (name == "mariadb" || name == "mysql") return SqlDialect::MariaDB;
  if (name == "postgres" || name == "postgresql" || name == "pg") return SqlDialect::Postgres;
  if (name == "mssql" || name == "tds" || name == "tsql" || name == "sqlserver") return SqlDialect::MsSql;
  return Result<SqlDialect>(Status::Invalid, "unknown sql dialect: " + name);
}

// A single value returned in a wire result row.
struct SqlWireValue {
  bool is_null = false;
  std::string bytes;
};

// Protocol description of a result column.
struct SqlWireColumn {
  std::string catalog;
  std::string schema;
  std::string table;
  std::string org_table;
  std::string name;
  std::string org_name;
  uint16_t charset = 0;
  std::string type;       // human-readable type or "?"
  uint32_t length = 0;
  uint8_t flags = 0;
  uint8_t decimals = 0;
};

// Full result set from a wire query: columns + text rows + execution stats.
struct SqlWireResult {
  std::vector<SqlWireColumn> columns;
  std::vector<std::vector<SqlWireValue>> rows;
  uint64_t affected_rows = 0;
  uint64_t last_insert_id = 0;
  std::string message;         // e.g. "Query OK, 3 rows affected"
  std::string error_message;   // error text when ok == false
  bool has_result_set = false;
  bool ok = true;              // false when server returned an error packet
};

// Backward-compatible aliases for existing MariaWireSession code.
using MariaValue = SqlWireValue;
using MariaColumn = SqlWireColumn;
using MariaResult = SqlWireResult;

// Streaming row consumer callback.
using SqlWireRowSink = std::function<bool(const std::vector<SqlWireColumn>&, std::vector<SqlWireValue>&)>;

// Abstract wire session interface implemented by MariaWireSession,
// TdsWireSession, and PgWireSession.
class SqlWireSession {
 public:
  virtual ~SqlWireSession() = default;

  // Connects to downstream relational database with specified credentials.
  virtual Result<void> connect(const std::string& host, uint16_t port,
                               const std::string& user, const std::string& pass,
                               const std::string& db) = 0;

  // Switches active database / catalog / schema context.
  virtual Result<void> select_db(const std::string& db) = 0;

  // Executes an arbitrary SQL statement, discarding tabular results.
  virtual Result<void> execute(const std::string& sql) {
    auto r = query(sql, nullptr);
    if (!r.ok()) return Result<void>(r.error());
    if (!r->ok) return Result<void>(Status::Invalid, r->error_message);
    return Ok();
  }

  // Executes a query and returns tabular results or streams them to a sink.
  virtual Result<SqlWireResult> query(const std::string& sql, const SqlWireRowSink* sink = nullptr) = 0;

  // Verifies the session connection is alive.
  virtual Result<void> ping() {
    return execute("SELECT 1");
  }

  // Returns true if the underlying transport is currently open and healthy.
  virtual bool connected() const = 0;

  // Closes the transport and resets session state.
  virtual void close() = 0;

  // Returns the dialect spoken by this wire session.
  virtual SqlDialect dialect() const = 0;

  // Active database name.
  virtual const std::string& database() const = 0;

  // Server banner / version reported during handshake.
  virtual const std::string& server_version() const = 0;

  // Connection timeout in milliseconds.
  virtual void set_read_timeout_ms(int ms) { read_timeout_ms_ = ms; }
  virtual int read_timeout_ms() const { return read_timeout_ms_; }

  // Transport encryption, set before connect(): "disable" never; "prefer"
  // (the default) when the server offers it, else in the clear; "require",
  // "verify-ca" or "verify-full" TLS or no connection. The certificate is
  // always checked against `ca_file` (a PEM bundle; empty = the machine's
  // trust store) and the host name - under "prefer" a server whose
  // certificate does not verify is used in the clear, as libpq's prefer would
  // use it unverified. Sessions without TLS refuse anything but disable/prefer.
  virtual void set_tls(const std::string& mode, const std::string& ca_file) {
    tls_mode_ = mode.empty() ? "prefer" : mode;
    tls_ca_ = ca_file;
  }

 protected:
  bool tls_required() const { return tls_mode_ == "require" || tls_mode_ == "verify-ca" || tls_mode_ == "verify-full"; }
  int read_timeout_ms_ = 10000;
  std::string tls_mode_ = "prefer";
  std::string tls_ca_;
};

// Factory to instantiate the appropriate wire session implementation.
std::shared_ptr<SqlWireSession> create_sql_wire_session(
    SqlDialect dialect, const std::string& host, uint16_t port);

}  // namespace aios::userland
