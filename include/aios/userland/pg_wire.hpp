// AIOS — Native PostgreSQL Frontend/Backend Protocol v3 wire client.
// Connects to PostgreSQL (port 5432), negotiates TLS (SSLRequest) as the
// session's TLS mode asks, authenticates with SCRAM-SHA-256 (the server's
// signature checked), MD5 or a cleartext password, and runs statements over
// the simple query protocol.
#pragma once

#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"
#include "aios/userland/sql_wire.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aios::userland {

class PgWireSession : public SqlWireSession {
 public:
  PgWireSession(const std::string& host = "127.0.0.1", uint16_t port = 5432);
  ~PgWireSession() override;

  PgWireSession(const PgWireSession&) = delete;
  PgWireSession& operator=(const PgWireSession&) = delete;
  PgWireSession(PgWireSession&&) = delete;
  PgWireSession& operator=(PgWireSession&&) = delete;

  // Connects and authenticates against PostgreSQL v3 server.
  Result<void> connect(const std::string& host, uint16_t port,
                       const std::string& user, const std::string& pass,
                       const std::string& db) override;

  // Convenience 3-arg connect with pre-configured host/port.
  Result<void> connect(const std::string& user, const std::string& pass,
                       const std::string& db);

  // A MariaDB database is a PostgreSQL database when one has the name (the
  // session reconnects to it), else a schema (search_path); neither is an error.
  Result<void> select_db(const std::string& db) override;

  // Runs a query and streams or collects rows.
  Result<SqlWireResult> query(const std::string& sql, const SqlWireRowSink* sink = nullptr) override;

  // Verifies connection health.
  Result<void> ping() override;

  // Closes connection.
  void close() override;

  bool connected() const override { return conn_ != nullptr && conn_->alive(); }
  SqlDialect dialect() const override { return SqlDialect::Postgres; }
  const std::string& database() const override { return db_; }
  const std::string& server_version() const override { return server_version_; }
  int backend_pid() const { return backend_pid_; }
  // Whether the connection is encrypted, and with what (or why it is not).
  bool tls_active() const { return tls_active_; }
  const std::string& tls_note() const { return tls_note_; }
  char transaction_status() const { return transaction_status_; }

 private:
  Result<void> read_exact(uint8_t* buf, size_t n);
  Result<void> send_exact(const uint8_t* buf, size_t n);
  // One backend message: its type and body.
  Result<void> read_message(char* type, std::vector<uint8_t>* body);
  Result<void> send_message(char type, const std::string& body);

  Result<void> open_transport();
  Result<void> start_tls();
  Result<void> send_startup();
  Result<void> handle_auth();
  Result<void> scram_sha256(const std::vector<uint8_t>& mechanisms);
  Result<void> fail_closed(Result<void> r) { close(); return r; }

  std::unique_ptr<hal::Connection> conn_;
  std::string host_;
  uint16_t port_ = 5432;
  std::string user_;
  std::string pass_;
  std::string db_;
  std::string server_version_;
  int backend_pid_ = 0;
  int backend_key_ = 0;
  char transaction_status_ = 'I';
  bool tls_active_ = false;
  std::string tls_note_;
};

}  // namespace aios::userland
