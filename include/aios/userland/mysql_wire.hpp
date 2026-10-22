// AIOS — dependency-free MariaDB/MySQL native wire-protocol client.
//
// Implements enough of the MySQL client/server protocol to connect to a
// remote MariaDB/MySQL server AND authenticate AND run COM_QUERY and parse
// the returned result set, WITHOUT linking libmariadb and WITHOUT SSL.
// Packets are framed per the "MySQL Internals Manual": a packet is a 3-byte
// little-endian payload length, a 1-byte sequence id, then the payload.
//
// The LENENC_INT (length-encoded integer) format used by row data is the one
// from MariaDB's "Optimized Binary Format": a small value
// (< 0xFB) is stored directly as one byte; 0xFB means NULL; 0xFC is followed
// by a 2-byte little-endian value; 0xFD by a 3-byte value; 0xFE by a 8-byte
// integer; 0xFF is invalid for this purpose.

#pragma once

#include "aios/core/result.hpp"  // aios::Status, aios::Error, aios::Result
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aios/userland/sql_wire.hpp"

namespace aios {

class Kernel;
namespace sidecar { class CapabilityGuard; }
namespace userland { class AsqlUserStore; }
class Sha256 {
 public:
  static std::vector<uint8_t> digest(const uint8_t* data, size_t len);
};

namespace userland {

class SqlDatabase;

// A latin1 column's bytes as UTF-8. MariaDB's latin1 is cp1252, and it
// converts every byte of it, valid-looking UTF-8 sequences included.
std::string cp1252_bytes_to_utf8(std::string_view s);

// Cryptographic scrambles for MySQL auth plugins.
std::vector<uint8_t> mysql_native_scramble(const std::string& password,
                                           const uint8_t* salt, size_t salt_len);
std::vector<uint8_t> caching_sha2_scramble(const std::string& password,
                                           const uint8_t* salt, size_t salt_len);

// Dependency-free MariaDB/MySQL wire-protocol client.
class MariaWireSession : public SqlWireSession {
 public:
  // Host may be empty (treat as "127.0.0.1"), or an IP/host string.
  MariaWireSession(const std::string& host, uint16_t port = 3306);
  ~MariaWireSession() override;

  // No copies.
  MariaWireSession(const MariaWireSession&) = delete;
  MariaWireSession& operator=(const MariaWireSession&) = delete;
  MariaWireSession(MariaWireSession&&) = delete;
  MariaWireSession& operator=(MariaWireSession&&) = delete;

  // Polymorphic connect from SqlWireSession.
  Result<void> connect(const std::string& host, uint16_t port,
                       const std::string& user, const std::string& pass,
                       const std::string& db) override {
    host_ = host.empty() ? "127.0.0.1" : host;
    port_ = port ? port : 3306;
    return connect(user, pass, db);
  }

  // Opens the connection and reads the server greeting handshake.
  // Returns Ok() when the protocol greeting is understood; otherwise a
  // descriptive Status::IoError / Status::Corrupt / Status::Unsupported.
  Result<void> connect(const std::string& user, const std::string& pass,
                       const std::string& database);

  // Sends a COM_QUERY and fully reads the response, decoding a result set
  // if the server returns one (text protocol).
  // With a sink, each result row goes to it as it arrives instead of into
  // the result (which then carries only columns and status); return false
  // to stop receiving - the rest is read and dropped.
  using RowSink = SqlWireRowSink;
  Result<MariaResult> query(const std::string& sql, const RowSink* sink = nullptr) override;

  // Sends COM_INIT_DB to switch the default database on the active session.
  Result<void> select_db(const std::string& db) override;
  // The database the session is in: the one it connected with, or the last
  // select_db() that the server accepted.
  const std::string& database() const override { return db_; }

  // Sends COM_PING to verify the connection is still alive.
  Result<void> ping() override;

  // Immediately discard current state and close the connection.
  void close() override;

  // What dialect this session speaks.
  SqlDialect dialect() const override { return SqlDialect::MariaDB; }

  // The compressed protocol, asked for at the next connect(): 0 off, 1 zlib (CLIENT_COMPRESS), 2 zstd (falls back
  // to zlib when the server has no zstd, to none when it has neither). `level` 0: the algorithm's default.
  void set_compression(uint8_t algo, int level = 0) { comp_want_ = algo; comp_want_level_ = level; }
  // What the open session runs: "" (none), "zlib" or "zstd".
  const char* compression() const { return comp_algo_ == 2 ? "zstd" : comp_algo_ == 1 ? "zlib" : ""; }

  bool connected() const override { return conn_ != nullptr && conn_->alive(); }
  const std::string& server_version() const override { return server_version_; }
  uint32_t thread_id() const { return thread_id_; }
  const std::string& server_caps() const { return server_caps_; }

 private:
  Result<void> read_exact(uint8_t* buf, size_t n);
  Result<size_t> read_packet(std::vector<uint8_t>* out, uint8_t* seq = nullptr);
  Result<void> write_packet(const uint8_t* data, size_t n, uint8_t seq);
  bool send_raw(const uint8_t* data, size_t n);   // through the compressed frames once they are on

  // Sends a MySQL wire command with the provided body.
  Result<void> send_command(uint8_t cmd, const std::string& body);

  // Greeting, authenticate, and result-set parsing.
  Result<void> parse_handshake(const std::vector<uint8_t>& pkt);
  Result<void> auth(const std::string& user, const std::string& pass,
                    const std::string& db);

  std::unique_ptr<hal::Connection> conn_;
  uint16_t port_ = 3306;
  std::string host_;
  std::string user_;
  std::string pass_;
  std::string db_;

  std::string server_version_;
  uint32_t thread_id_ = 0;
  std::string server_caps_;
  uint8_t seq_ = 0;
  bool have_auth_ = false;
  uint32_t negotiated_capabilities_ = 0;
  uint8_t server_auth_plugin_len_ = 0;
  std::string server_auth_plugin_;
  std::vector<uint8_t> auth_plugin_data_;
  uint8_t comp_want_ = 0, comp_algo_ = 0, comp_seq_ = 0;
  int comp_want_level_ = 0, comp_level_ = 0;
  std::vector<uint8_t> comp_in_, comp_frame_, comp_out_;
  size_t comp_in_at_ = 0;
};

// Standard MySQL wire protocol server for AIOS.
// Allows any standard MySQL client (mysql CLI, DBeaver, JDBC, Node, Python)
// to connect into AIOS and execute standard SQL and ASQL queries.
class MysqlWireServer {
 public:
  MysqlWireServer(Kernel& kernel, SqlDatabase& db, uint16_t port = 3306,
                  const std::string& bind_addr = "127.0.0.1");
  ~MysqlWireServer();

  // No copies.
  MysqlWireServer(const MysqlWireServer&) = delete;
  MysqlWireServer& operator=(const MysqlWireServer&) = delete;

  // Starts the listening socket.
  Result<void> start();

  // Closes the listener and stops any running background server thread.
  void stop();

  // Non-blocking poll: accepts and serves pending connections.
  Result<void> poll_and_serve(int timeout_ms = 50);

  // Starts a dedicated background thread to accept and serve connections.
  void start_background();

  // Stops the background thread.
  void stop_background();

  // Serves one connected client session until disconnection.
  void serve_client(hal::Connection& client_conn);

  bool is_running() const { return listener_ != nullptr || unix_listener_ != nullptr; }
  uint16_t port() const { return listener_ ? listener_->port() : port_; }
  const std::string& bind_addr() const { return bind_addr_; }

  // An account the server will accept. With none added the server refuses
  // every connection rather than accepting every connection: this port used to
  // send OK without ever looking at the password, so `-u nobody -pWRONG` got a
  // session and could read customer records.
  void add_account(const std::string& user, const std::string& password);
  // With no account configured, whether a client on this same host may still
  // connect. On by default so an embedded or local-only engine keeps working;
  // a remote client is refused either way, and once an account exists its
  // password is always checked even on the loopback.
  void set_trust_loopback(bool trust) { trust_loopback_ = trust; }
  // Accept capability credentials (asql_capability.hpp): a client that connects
  // as user "cap" sends one as its password, over mysql_clear_password because
  // the credential is verified by signature, not looked up. The guard outlives
  // the server. Off (every "cap" login refused) until a guard is set.
  void set_capability_guard(sidecar::CapabilityGuard* guard) { cap_guard_ = guard; }
  // A credential crosses the wire in the clear, so it is accepted only from
  // this host unless the operator says the link is private enough.
  void set_capability_remote(bool allow) { cap_remote_ = allow; }
  // Named users (asql_capability.hpp, AsqlUserStore): `-u name -p<password>`
  // over mysql_native_password, the session then holding that user's
  // credential. The store outlives the server. Needs a guard as well.
  void set_capability_users(AsqlUserStore* users) { cap_users_ = users; install_user_lister(); }
  void install_user_lister();   // mysql.user's rows, for the engine
  // Also listen on a unix-domain socket at `path` (set before start()).
  void set_unix_socket(const std::string& path) { unix_path_ = path; }
  const std::string& unix_socket() const { return unix_path_; }
  // Whether to listen on TCP as well (set before start()). Off with a unix socket set: the socket is the only
  // way in, for a sandbox that allows sockets in its own folder and no network at all.
  void set_tcp(bool on) { tcp_ = on; }
  // Honour each client's connection charset the way MariaDB does: a latin1
  // connection (handshake byte, SET NAMES, SET character_set_*) is sent cp1252
  // bytes and its SQL text is read as cp1252, while the store stays UTF-8.
  // Off by default: a host whose clients were tuned against the engine's
  // always-UTF-8 output must not change underneath them.
  void set_charset_aware(bool on) { charset_aware_ = on; }
  // TLS on the wire (set before start()): the greeting offers SSL, and a
  // client that asks for it (SSLRequest) finishes the handshake encrypted -
  // TLS 1.3 or 1.2, ECDHE, AES-GCM / ChaCha20-Poly1305 (net::TlsServer).
  // require: a client on another host that does not ask is refused, as
  // MariaDB's require_secure_transport refuses it (the unix socket and the
  // loopback are not).
  void set_tls(const std::string& cert_chain_pem, const std::string& key_pem, bool require) {
    tls_cert_ = cert_chain_pem; tls_key_ = key_pem; tls_require_ = require;
  }
  bool tls_enabled() const { return !tls_cert_.empty() && !tls_key_.empty(); }
  size_t account_count() const { return accounts_.size(); }
  void set_max_connections(size_t max_conns);
  size_t max_connections() const;

  struct SlaveInfo {
    uint32_t server_id = 0;
    std::string host;
    std::string user;
    uint16_t port = 0;
    uint32_t master_id = 0;
    uint64_t registered_time_ns = 0;
  };

  static void register_slave(const SlaveInfo& s);
  static void unregister_slave(uint32_t server_id);
  static std::vector<SlaveInfo> get_registered_slaves();

 private:
  // user -> password. Compared by scrambling the server's own salt with the
  // stored password and matching that against what the client sent, so the
  // password itself never crosses the wire.
  std::unordered_map<std::string, std::string> accounts_;
  bool trust_loopback_ = true;
  sidecar::CapabilityGuard* cap_guard_ = nullptr;
  bool cap_remote_ = false;
  AsqlUserStore* cap_users_ = nullptr;
  bool auth_ok(const std::string& user, const std::vector<uint8_t>& response,
               const uint8_t* salt, bool from_loopback) const;

  Kernel& kernel_;
  SqlDatabase& db_;
  uint16_t port_ = 3306;
  std::string bind_addr_ = "127.0.0.1";
  std::unique_ptr<hal::Listener> listener_;
  std::string unix_path_;
  bool tcp_ = true;
  bool charset_aware_ = false;
  std::string tls_cert_, tls_key_;
  bool tls_require_ = false;
  std::unique_ptr<hal::Listener> unix_listener_;
  std::unique_ptr<hal::Thread> unix_thread_;
  Result<void> poll_and_serve_on(hal::Listener& listener, int timeout_ms);
  std::atomic<bool> running_{false};
  std::unique_ptr<hal::Thread> bg_thread_;
  uint32_t next_conn_id_ = 1;
  std::atomic<int> active_clients_{0};
  hal::Mutex clients_mu_;

  struct ClientSession;
  std::vector<std::unique_ptr<ClientSession>> sessions_;

  Result<void> send_packet(hal::Connection& conn, const uint8_t* data, size_t n, uint8_t seq);
  // A reply is queued per client thread and written once (see mysql_wire.cpp).
  bool wire_queue(hal::Connection& conn, ByteSpan bytes);
  bool wire_flush(hal::Connection& conn);
  Result<size_t> recv_packet(hal::Connection& conn, std::vector<uint8_t>* out, uint8_t* seq = nullptr,
                             const std::function<void()>& on_idle = nullptr);
  Result<void> send_ok(hal::Connection& conn, uint8_t seq, uint64_t affected_rows = 0,
                       uint64_t last_insert_id = 0, uint16_t status = 0x0002,
                       const std::string& msg = "");
  Result<void> send_err(hal::Connection& conn, uint8_t seq, uint16_t err_code,
                        const std::string& sqlstate, const std::string& msg);
};

}  // namespace userland
}  // namespace aios
