// AIOS — Native Microsoft TDS 7.4 wire client.
// Connects to MS SQL Server (port 1433): PRELOGIN (with TLS when the session's
// TLS mode and the server agree), LOGIN7, then SQL batches, their token
// streams read into SqlWireResult - every column type SQL Server returns for
// a batch, the (MAX) types' PLP chunks, and null-bitmap rows.
#pragma once

#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"
#include "aios/userland/sql_wire.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aios::userland {

class TdsWireSession : public SqlWireSession {
 public:
  TdsWireSession(const std::string& host = "127.0.0.1", uint16_t port = 1433);
  ~TdsWireSession() override;

  TdsWireSession(const TdsWireSession&) = delete;
  TdsWireSession& operator=(const TdsWireSession&) = delete;
  TdsWireSession(TdsWireSession&&) = delete;
  TdsWireSession& operator=(TdsWireSession&&) = delete;

  // Connects and authenticates against MS SQL Server via TDS 7.4 (SQL Server 2012 and later).
  Result<void> connect(const std::string& host, uint16_t port,
                       const std::string& user, const std::string& pass,
                       const std::string& db) override;

  // Convenience 3-arg connect with pre-configured host/port.
  Result<void> connect(const std::string& user, const std::string& pass,
                       const std::string& db);

  // Switches active database context via `USE [db]`.
  Result<void> select_db(const std::string& db) override;

  // Runs a SQL batch and returns tabular results or streams them to a sink.
  Result<SqlWireResult> query(const std::string& sql, const SqlWireRowSink* sink = nullptr) override;

  // Verifies connection health.
  Result<void> ping() override;

  // Closes connection.
  void close() override;

  bool connected() const override { return conn_ != nullptr && conn_->alive(); }
  SqlDialect dialect() const override { return SqlDialect::MsSql; }
  const std::string& database() const override { return db_; }
  const std::string& server_version() const override { return server_version_; }
  bool tls_active() const { return tls_active_; }
  const std::string& tls_note() const { return tls_note_; }
  uint32_t packet_size() const { return packet_size_; }

  // A token stream's column, as COLMETADATA describes it (tds_wire.cpp).
  struct Col {
    std::string name;
    uint8_t type = 0;
    uint32_t max_len = 0;
    uint8_t precision = 0, scale = 0;
    uint32_t collation = 0;   // LCID and flags; bit 26 = UTF-8
    uint8_t sort_id = 0;
    bool plp = false;         // a (MAX) type or xml: its value comes in chunks
  };

 private:
  Result<void> read_exact(uint8_t* buf, size_t n);
  Result<void> send_exact(const uint8_t* buf, size_t n);

  // One message: split into packets of the negotiated size, EOM on the last.
  Result<void> send_message(uint8_t type, const uint8_t* payload, size_t len);
  // One whole message: its packets' payloads joined.
  Result<std::vector<uint8_t>> read_message(uint8_t* out_type = nullptr);

  Result<void> open_transport(uint8_t encryption, uint8_t* server_encryption);
  Result<void> start_tls();
  Result<void> do_login7();
  // ENVCHANGE: the database, packet size, collation and transaction it reports.
  void env_change(const uint8_t* p, size_t len);

  std::unique_ptr<hal::Connection> conn_;
  std::string host_;
  uint16_t port_ = 1433;
  std::string user_;
  std::string pass_;
  std::string db_;
  std::string server_version_;
  uint8_t packet_seq_ = 1;
  uint32_t packet_size_ = 4096;
  uint64_t txn_descriptor_ = 0;   // the open transaction's, from ENVCHANGE; 0 outside one
  bool tls_active_ = false;
  std::string tls_note_;
};

}  // namespace aios::userland
