// AIOS — Native PostgreSQL Frontend/Backend Protocol v3 wire client implementation.
#include "aios/userland/pg_wire.hpp"
#include "aios/core/base64.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/md5.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/host.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include <sys/stat.h>
#include <algorithm>
#include <cstring>

namespace aios::userland {

namespace {

// Longer than any message a server sends us (a row of the widest text the
// write-behind reads back is far below it): past it the stream is corrupt.
constexpr uint32_t kMaxMessage = 1u << 30;

uint32_t get_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t get_be16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void put_be32(std::string* s, uint32_t v) {
  s->push_back(static_cast<char>((v >> 24) & 0xFF));
  s->push_back(static_cast<char>((v >> 16) & 0xFF));
  s->push_back(static_cast<char>((v >> 8) & 0xFF));
  s->push_back(static_cast<char>(v & 0xFF));
}

std::string hex_str(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  std::string s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    s.push_back(hex[(data[i] >> 4) & 0xF]);
    s.push_back(hex[data[i] & 0xF]);
  }
  return s;
}

const char* pg_type_name(uint32_t oid) {
  switch (oid) {
    case 16: return "bool";
    case 17: return "bytea";
    case 18: return "char";
    case 19: return "name";
    case 20: return "int8";
    case 21: return "int2";
    case 23: return "int4";
    case 25: return "text";
    case 26: return "oid";
    case 114: return "json";
    case 700: return "float4";
    case 701: return "float8";
    case 790: return "money";
    case 1042: return "bpchar";
    case 1043: return "varchar";
    case 1082: return "date";
    case 1083: return "time";
    case 1114: return "timestamp";
    case 1184: return "timestamptz";
    case 1186: return "interval";
    case 1266: return "timetz";
    case 1700: return "numeric";
    case 2950: return "uuid";
    case 3802: return "jsonb";
    default: return "?";
  }
}

// An ErrorResponse / NoticeResponse: "ERROR 23505: duplicate key value ... (Key (id)=(1) already exists.)"
std::string pg_error_text(const std::vector<uint8_t>& body) {
  std::string severity, code, message, detail, hint;
  size_t pos = 0;
  while (pos < body.size() && body[pos] != 0) {
    const char field = static_cast<char>(body[pos++]);
    const size_t n = strnlen(reinterpret_cast<const char*>(body.data() + pos), body.size() - pos);
    std::string v(reinterpret_cast<const char*>(body.data() + pos), n);
    pos += n + 1;
    if (field == 'V' || (field == 'S' && severity.empty())) severity = v;
    else if (field == 'C') code = v;
    else if (field == 'M') message = v;
    else if (field == 'D') detail = v;
    else if (field == 'H') hint = v;
  }
  std::string out = severity.empty() ? "ERROR" : severity;
  if (!code.empty()) out += " " + code;
  out += ": " + (message.empty() ? std::string("(no message)") : message);
  if (!detail.empty()) out += " (" + detail + ")";
  if (!hint.empty()) out += " [hint: " + hint + "]";
  return out;
}

// "SELECT 4", "INSERT 0 5", "UPDATE 3", "COPY 7": the count at the end, if the tag has one.
uint64_t tag_rows(const std::string& tag) {
  const size_t sp = tag.rfind(' ');
  if (sp == std::string::npos) return 0;
  const char* d = tag.c_str() + sp + 1;
  if (*d < '0' || *d > '9') return 0;
  return std::strtoull(d, nullptr, 10);
}

// A double-quoted identifier, with any " in it doubled.
std::string pg_ident(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"') o += '"'; o += c; }
  return o + "\"";
}

// A single-quoted literal (standard_conforming_strings, on since 9.1).
std::string pg_literal(const std::string& s) {
  std::string o = "'";
  for (char c : s) { if (c == '\'') o += '\''; o += c; }
  return o + "'";
}

}  // namespace

PgWireSession::PgWireSession(const std::string& host, uint16_t port)
    : host_(host.empty() ? "127.0.0.1" : host), port_(port ? port : 5432) {}

PgWireSession::~PgWireSession() {
  close();
}

void PgWireSession::close() {
  if (conn_) {
    if (conn_->alive()) {
      const uint8_t term[5] = {'X', 0, 0, 0, 4};   // Terminate
      (void)conn_->send_all(ByteSpan(term, 5));
    }
    conn_->close();
    conn_.reset();
  }
  tls_active_ = false;
}

Result<void> PgWireSession::read_exact(uint8_t* buf, size_t n) {
  if (!conn_ || !conn_->alive()) return Result<void>(Status::IoError, "no open PostgreSQL connection");
  if (!conn_->recv_all(MutableByteSpan(buf, n), read_timeout_ms_))
    return Result<void>(Status::IoError, conn_->alive() ? "timed out reading from the PostgreSQL server"
                                                        : "the PostgreSQL server closed the connection");
  return Ok();
}

Result<void> PgWireSession::send_exact(const uint8_t* buf, size_t n) {
  if (!conn_ || !conn_->alive()) return Result<void>(Status::IoError, "no open PostgreSQL connection");
  if (!conn_->send_all(ByteSpan(buf, n))) return Result<void>(Status::IoError, "failed to send to the PostgreSQL server");
  return Ok();
}

Result<void> PgWireSession::read_message(char* type, std::vector<uint8_t>* body) {
  uint8_t hdr[5];
  auto r = read_exact(hdr, 5);
  if (!r.ok()) return r;
  *type = static_cast<char>(hdr[0]);
  const uint32_t len = get_be32(hdr + 1);
  if (len < 4 || len > kMaxMessage) return Result<void>(Status::Corrupt, "corrupt PostgreSQL message length " + std::to_string(len));
  body->resize(len - 4);
  if (len > 4) return read_exact(body->data(), body->size());
  return Ok();
}

Result<void> PgWireSession::send_message(char type, const std::string& body) {
  std::string m;
  m.reserve(body.size() + 5);
  m.push_back(type);
  put_be32(&m, static_cast<uint32_t>(body.size() + 4));
  m += body;
  return send_exact(reinterpret_cast<const uint8_t*>(m.data()), m.size());
}

Result<void> PgWireSession::connect(const std::string& host, uint16_t port, const std::string& user, const std::string& pass,
                                    const std::string& db) {
  host_ = host.empty() ? "127.0.0.1" : host;
  port_ = port ? port : 5432;
  return connect(user, pass, db);
}

Result<void> PgWireSession::open_transport() {
  const bool unix_sock = !host_.empty() && host_[0] == '/';
  // A directory is where the server keeps its socket, as libpq reads one: <dir>/.s.PGSQL.<port>.
  std::string path = host_;
  struct stat st;
  if (unix_sock && ::stat(host_.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) path = host_ + "/.s.PGSQL." + std::to_string(port_);
  auto c = unix_sock ? hal::net_connect_unix(path, 5000) : hal::net_connect(host_, port_, 5000);
  if (!c.ok())
    return Result<void>(c.status(), "failed to connect to PostgreSQL at " + (unix_sock ? path : (host_ + ":" + std::to_string(port_))) +
                                        ": " + c.error().message);
  conn_ = std::move(*c);
  return Ok();
}

// SSLRequest: 'S' and the handshake on the same socket, 'N' and the clear.
Result<void> PgWireSession::start_tls() {
  static const uint8_t ssl_request[8] = {0, 0, 0, 8, 0x04, 0xD2, 0x16, 0x2F};   // 80877103
  auto s = send_exact(ssl_request, sizeof ssl_request);
  if (!s.ok()) return s;
  uint8_t answer = 0;
  s = read_exact(&answer, 1);
  if (!s.ok()) return s;
  if (answer == 'N') {
    if (tls_required()) return Result<void>(Status::Denied, "the PostgreSQL server does not offer TLS and sslmode=" + tls_mode_ + " needs it");
    tls_note_ = "the server does not offer TLS";
    return Ok();
  }
  if (answer != 'S') return Result<void>(Status::Corrupt, "unexpected answer to SSLRequest");
  hal::Host& host = hal::process_host();
  const std::string store = tls_ca_.empty() ? net::default_trust_store(host, "/nonexistent") : tls_ca_;
  auto tls = std::make_unique<net::TlsClient>(host, store);
  const std::string peer = conn_->peer();
  auto h = tls->connect_over(std::move(conn_), host_, port_, read_timeout_ms_);
  if (!h.ok()) return Result<void>(h.status(), "TLS with the PostgreSQL server: " + h.error().message);
  tls_note_ = tls->info().version + " " + tls->info().cipher + ", certificate of " + tls->info().subject;
  conn_ = std::make_unique<net::TlsClientConnection>(std::move(tls), peer);
  tls_active_ = true;
  return Ok();
}

Result<void> PgWireSession::connect(const std::string& user, const std::string& pass, const std::string& db) {
  close();
  user_ = user.empty() ? "postgres" : user;
  pass_ = pass;
  db_ = db.empty() ? "postgres" : db;
  tls_note_.clear();

  const bool unix_sock = !host_.empty() && host_[0] == '/';
  if (tls_mode_ != "disable" && unix_sock && tls_required())
    return Result<void>(Status::Invalid, "sslmode=" + tls_mode_ + " over a unix socket: PostgreSQL offers no TLS there");
  auto o = open_transport();
  if (!o.ok()) return o;
  if (tls_mode_ != "disable" && !unix_sock) {
    auto t = start_tls();
    if (!t.ok()) {
      close();
      // prefer: a certificate that does not verify, or a handshake this client
      // cannot finish, is the clear connection prefer would have made anyway.
      if (tls_required()) return t;
      tls_note_ = "in the clear: " + t.error().message;
      const std::string note = tls_note_;
      o = open_transport();
      if (!o.ok()) return o;
      tls_note_ = note;
    }
  } else {
    tls_note_ = tls_mode_ == "disable" ? "sslmode=disable" : "unix socket";
  }

  auto sr = send_startup();
  if (!sr.ok()) return fail_closed(sr);
  auto ar = handle_auth();
  if (!ar.ok()) return fail_closed(ar);
  return Ok();
}

Result<void> PgWireSession::send_startup() {
  std::string body;
  put_be32(&body, 0x00030000);   // protocol 3.0
  auto param = [&](const std::string& k, const std::string& v) {
    body += k;
    body.push_back(0);
    body += v;
    body.push_back(0);
  };
  param("user", user_);
  param("database", db_);
  param("client_encoding", "UTF8");
  param("application_name", "AIOS ASQL");
  body.push_back(0);
  std::string m;
  put_be32(&m, static_cast<uint32_t>(body.size() + 4));
  m += body;
  return send_exact(reinterpret_cast<const uint8_t*>(m.data()), m.size());
}

Result<void> PgWireSession::handle_auth() {
  bool authenticated = false;
  for (;;) {
    char type = 0;
    std::vector<uint8_t> body;
    auto r = read_message(&type, &body);
    if (!r.ok()) return r;
    if (type == 'R') {
      if (body.size() < 4) return Result<void>(Status::Corrupt, "short PostgreSQL authentication message");
      const uint32_t kind = get_be32(body.data());
      if (kind == 0) {   // AuthenticationOk
        authenticated = true;
      } else if (kind == 3) {   // cleartext password
        if (!tls_active_ && (host_.empty() || host_[0] != '/') && tls_required())
          return Result<void>(Status::Denied, "the server asks for the password in the clear");
        std::string p = pass_;
        p.push_back(0);
        auto s = send_message('p', p);
        if (!s.ok()) return s;
      } else if (kind == 5) {   // MD5: "md5" + md5(md5(password + user) + salt)
        if (body.size() < 8) return Result<void>(Status::Corrupt, "short PostgreSQL MD5 salt");
        const std::string pu = pass_ + user_;
        auto h1 = crypto::md5(ByteSpan(reinterpret_cast<const uint8_t*>(pu.data()), pu.size()));
        std::string step2 = hex_str(h1.data(), 16);
        step2.append(reinterpret_cast<const char*>(body.data() + 4), 4);
        auto h2 = crypto::md5(ByteSpan(reinterpret_cast<const uint8_t*>(step2.data()), step2.size()));
        std::string p = "md5" + hex_str(h2.data(), 16);
        p.push_back(0);
        auto s = send_message('p', p);
        if (!s.ok()) return s;
      } else if (kind == 10) {   // SASL: the mechanisms it offers
        auto s = scram_sha256(std::vector<uint8_t>(body.begin() + 4, body.end()));
        if (!s.ok()) return s;
      } else if (kind == 2 || kind == 7 || kind == 9) {
        return Result<void>(Status::Unsupported, std::string("the PostgreSQL server wants ") +
                                                     (kind == 2 ? "Kerberos V5" : kind == 7 ? "GSSAPI" : "SSPI") +
                                                     " authentication, which this client does not do (use scram-sha-256 or md5 in pg_hba.conf)");
      } else {
        return Result<void>(Status::Unsupported, "unsupported PostgreSQL authentication request " + std::to_string(kind));
      }
    } else if (type == 'S') {   // ParameterStatus
      const char* p = reinterpret_cast<const char*>(body.data());
      const size_t n = strnlen(p, body.size());
      if (n < body.size()) {
        const std::string name(p, n);
        const std::string val(p + n + 1, strnlen(p + n + 1, body.size() - n - 1));
        if (name == "server_version") server_version_ = val;
      }
    } else if (type == 'K') {   // BackendKeyData
      if (body.size() >= 8) {
        backend_pid_ = static_cast<int>(get_be32(body.data()));
        backend_key_ = static_cast<int>(get_be32(body.data() + 4));
      }
    } else if (type == 'Z') {   // ReadyForQuery
      if (!body.empty()) transaction_status_ = static_cast<char>(body[0]);
      if (!authenticated) return Result<void>(Status::Corrupt, "the PostgreSQL server was ready before authentication finished");
      return Ok();
    } else if (type == 'E') {
      return Result<void>(Status::Denied, "PostgreSQL: " + pg_error_text(body));
    } else if (type == 'N' || type == 'v') {
      // A notice, or NegotiateProtocolVersion (a minor version or option it
      // does not know): neither stops the startup.
    } else {
      return Result<void>(Status::Corrupt, std::string("unexpected PostgreSQL message '") + type + "' during startup");
    }
  }
}

// SCRAM-SHA-256 (RFC 5802 / 7677), as PostgreSQL runs it: no channel binding
// ("n,,"), the user name left empty (the server takes the startup packet's),
// and the server's final signature checked - a server that does not know the
// password cannot produce it, so a stolen address cannot pretend to be it.
Result<void> PgWireSession::scram_sha256(const std::vector<uint8_t>& mechanisms) {
  bool offered = false;
  for (size_t pos = 0; pos < mechanisms.size() && mechanisms[pos] != 0;) {
    const size_t n = strnlen(reinterpret_cast<const char*>(mechanisms.data() + pos), mechanisms.size() - pos);
    if (std::string(reinterpret_cast<const char*>(mechanisms.data() + pos), n) == "SCRAM-SHA-256") offered = true;
    pos += n + 1;
  }
  if (!offered) return Result<void>(Status::Unsupported, "the PostgreSQL server offers no SASL mechanism this client has (it has SCRAM-SHA-256)");

  uint8_t raw[18];
  hal::process_host().random(MutableByteSpan(raw, sizeof raw));
  const std::string c_nonce = base64_encode(ByteSpan(raw, sizeof raw));   // 24 printable characters, no ','
  const std::string client_first_bare = "n=,r=" + c_nonce;
  const std::string client_first = "n,," + client_first_bare;

  std::string initial = "SCRAM-SHA-256";
  initial.push_back(0);
  put_be32(&initial, static_cast<uint32_t>(client_first.size()));
  initial += client_first;
  auto s = send_message('p', initial);
  if (!s.ok()) return s;

  auto expect_auth = [&](uint32_t want, std::string* data) -> Result<void> {
    char type = 0;
    std::vector<uint8_t> body;
    auto r = read_message(&type, &body);
    if (!r.ok()) return r;
    if (type == 'E') return Result<void>(Status::Denied, "PostgreSQL: " + pg_error_text(body));
    if (type != 'R' || body.size() < 4 || get_be32(body.data()) != want)
      return Result<void>(Status::Corrupt, "unexpected message in the SCRAM exchange");
    data->assign(reinterpret_cast<const char*>(body.data() + 4), body.size() - 4);
    return Ok();
  };

  std::string server_first;
  s = expect_auth(11, &server_first);   // SASLContinue
  if (!s.ok()) return s;
  std::string nonce, salt_b64;
  long iterations = 0;
  for (size_t at = 0; at < server_first.size();) {
    size_t comma = server_first.find(',', at);
    if (comma == std::string::npos) comma = server_first.size();
    const std::string attr = server_first.substr(at, comma - at);
    if (attr.size() >= 2 && attr[1] == '=') {
      if (attr[0] == 'r') nonce = attr.substr(2);
      else if (attr[0] == 's') salt_b64 = attr.substr(2);
      else if (attr[0] == 'i') iterations = std::strtol(attr.c_str() + 2, nullptr, 10);
      else if (attr[0] == 'm') return Result<void>(Status::Unsupported, "SCRAM: the server asks for an extension this client does not have");
    }
    at = comma + 1;
  }
  if (nonce.size() <= c_nonce.size() || nonce.compare(0, c_nonce.size(), c_nonce) != 0)
    return Result<void>(Status::Denied, "SCRAM: the server's nonce does not extend ours");
  if (salt_b64.empty() || iterations < 1 || iterations > 10000000) return Result<void>(Status::Corrupt, "invalid SCRAM server-first message");
  auto salt = base64_decode(salt_b64);
  if (!salt.ok() || salt->empty()) return Result<void>(Status::Corrupt, "invalid SCRAM salt");

  uint8_t salted[32];
  crypto::pbkdf2_hmac_sha256(ByteSpan(reinterpret_cast<const uint8_t*>(pass_.data()), pass_.size()),
                             ByteSpan(salt->data(), salt->size()), static_cast<uint32_t>(iterations), salted, 32);
  auto hmac = [](ByteSpan key, const std::string& msg) {
    return crypto::hmac_sha256(key, ByteSpan(reinterpret_cast<const uint8_t*>(msg.data()), msg.size()));
  };
  const auto client_key = hmac(ByteSpan(salted, 32), "Client Key");
  const auto stored_key = crypto::sha256(ByteSpan(client_key.data(), client_key.size()));
  const std::string client_final_bare = "c=biws,r=" + nonce;
  const std::string auth_msg = client_first_bare + "," + server_first + "," + client_final_bare;
  const auto client_sig = hmac(ByteSpan(stored_key.data(), stored_key.size()), auth_msg);
  uint8_t proof[32];
  for (int k = 0; k < 32; ++k) proof[k] = client_key[k] ^ client_sig[k];
  s = send_message('p', client_final_bare + ",p=" + base64_encode(ByteSpan(proof, 32)));
  if (!s.ok()) return s;

  std::string server_final;
  s = expect_auth(12, &server_final);   // SASLFinal
  if (!s.ok()) return s;
  if (server_final.rfind("e=", 0) == 0) return Result<void>(Status::Denied, "SCRAM: " + server_final.substr(2));
  if (server_final.rfind("v=", 0) != 0) return Result<void>(Status::Corrupt, "SCRAM: no server signature");
  const auto server_key = hmac(ByteSpan(salted, 32), "Server Key");
  const auto server_sig = hmac(ByteSpan(server_key.data(), server_key.size()), auth_msg);
  std::string want = base64_encode(ByteSpan(server_sig.data(), server_sig.size()));
  std::string got = server_final.substr(2);
  const size_t comma = got.find(',');
  if (comma != std::string::npos) got.resize(comma);
  if (got != want) return Result<void>(Status::Denied, "SCRAM: the server's signature is wrong - it does not know the password");
  return Ok();
}

Result<void> PgWireSession::select_db(const std::string& db) {
  if (db.empty() || db_ == db) return Ok();
  // A database of that name: a PostgreSQL connection is to one database, so
  // the session reconnects to it.
  auto r = query("SELECT 1 FROM pg_database WHERE datname = " + pg_literal(db));
  if (!r.ok()) return Result<void>(r.error());
  if (!r->ok) return Result<void>(Status::Invalid, r->error_message);
  if (!r->rows.empty()) return connect(host_, port_, user_, pass_, db);
  // Else a schema of that name in this one.
  r = query("SELECT 1 FROM pg_namespace WHERE nspname = " + pg_literal(db));
  if (!r.ok()) return Result<void>(r.error());
  if (!r->ok) return Result<void>(Status::Invalid, r->error_message);
  if (r->rows.empty()) return Result<void>(Status::NotFound, "PostgreSQL has no database or schema named " + db);
  auto e = execute("SET search_path TO " + pg_ident(db) + ", public");
  if (!e.ok()) return e;
  db_ = db;
  return Ok();
}

Result<void> PgWireSession::ping() {
  return execute("SELECT 1");
}

Result<SqlWireResult> PgWireSession::query(const std::string& sql, const SqlWireRowSink* sink) {
  if (!conn_ || !conn_->alive()) return Result<SqlWireResult>(Status::IoError, "no open PostgreSQL connection");
  if (sql.find('\0') != std::string::npos) return Result<SqlWireResult>(Status::Invalid, "a statement with a NUL byte in it");
  std::string q = sql;
  q.push_back(0);
  auto s = send_message('Q', q);
  if (!s.ok()) { close(); return Result<SqlWireResult>(s.error()); }

  SqlWireResult res;
  res.ok = true;
  bool sink_ok = true;
  // Anything that breaks off the exchange part way leaves the rest of the
  // answer unread on the socket, where the next statement would read it as
  // its own: the connection is closed instead.
  auto broken = [&](const Error& e) {
    close();
    return Result<SqlWireResult>(e);
  };
  for (;;) {
    char type = 0;
    std::vector<uint8_t> body;
    auto r = read_message(&type, &body);
    if (!r.ok()) return broken(r.error());
    switch (type) {
      case 'T': {   // RowDescription: a new result set (the last one of several is the one kept)
        if (body.size() < 2) return broken(Error{Status::Corrupt, "short RowDescription"});
        const uint16_t n = get_be16(body.data());
        res.columns.clear();
        if (!sink) res.rows.clear();
        res.has_result_set = true;
        size_t pos = 2;
        for (uint16_t i = 0; i < n; ++i) {
          if (pos >= body.size()) return broken(Error{Status::Corrupt, "short RowDescription"});
          SqlWireColumn col;
          const size_t nl = strnlen(reinterpret_cast<const char*>(body.data() + pos), body.size() - pos);
          col.name.assign(reinterpret_cast<const char*>(body.data() + pos), nl);
          pos += nl + 1;
          if (pos + 18 > body.size()) return broken(Error{Status::Corrupt, "short RowDescription"});
          // table oid(4), attribute(2), type oid(4), type size(2), type modifier(4), format(2)
          col.type = pg_type_name(get_be32(body.data() + pos + 6));
          const int32_t typmod = static_cast<int32_t>(get_be32(body.data() + pos + 12));
          if (typmod > 4) col.length = static_cast<uint32_t>(typmod - 4);
          pos += 18;
          res.columns.push_back(std::move(col));
        }
        break;
      }
      case 'D': {   // DataRow
        if (body.size() < 2) return broken(Error{Status::Corrupt, "short DataRow"});
        const uint16_t n = get_be16(body.data());
        std::vector<SqlWireValue> row;
        row.reserve(n);
        size_t pos = 2;
        for (uint16_t i = 0; i < n; ++i) {
          if (pos + 4 > body.size()) return broken(Error{Status::Corrupt, "short DataRow"});
          const int32_t vlen = static_cast<int32_t>(get_be32(body.data() + pos));
          pos += 4;
          SqlWireValue v;
          if (vlen < 0) {
            v.is_null = true;
          } else {
            if (pos + static_cast<size_t>(vlen) > body.size()) return broken(Error{Status::Corrupt, "short DataRow"});
            v.bytes.assign(reinterpret_cast<const char*>(body.data() + pos), static_cast<size_t>(vlen));
            pos += static_cast<size_t>(vlen);
          }
          row.push_back(std::move(v));
        }
        if (sink) { if (sink_ok) sink_ok = (*sink)(res.columns, row); }
        else res.rows.push_back(std::move(row));
        break;
      }
      case 'C': {   // CommandComplete: a statement's tag; several statements' counts add up
        const std::string tag(reinterpret_cast<const char*>(body.data()), strnlen(reinterpret_cast<const char*>(body.data()), body.size()));
        res.message = tag;
        if (tag.rfind("SELECT", 0) != 0) res.affected_rows += tag_rows(tag);
        break;
      }
      case 'E':   // ErrorResponse: the batch stops here; ReadyForQuery still follows
        res.ok = false;
        if (res.error_message.empty()) res.error_message = pg_error_text(body);
        break;
      case 'G': {   // CopyInResponse: COPY ... FROM STDIN has nothing to read from here
        std::string why = "COPY FROM STDIN is not supported through the ASQL gateway";
        why.push_back(0);
        auto f = send_message('f', why);   // CopyFail; the server answers with an ErrorResponse
        if (!f.ok()) return broken(f.error());
        break;
      }
      case 'H':   // CopyOutResponse: the data follows as CopyData until CopyDone
      case 'W':   // CopyBothResponse (replication)
      case 'd':   // CopyData: each line of COPY ... TO STDOUT is a row of one text column
        if (type == 'd' && body.size() > 0) {
          std::vector<SqlWireValue> row(1);
          size_t n = body.size();
          if (n && body[n - 1] == '\n') --n;
          row[0].bytes.assign(reinterpret_cast<const char*>(body.data()), n);
          if (res.columns.empty()) { SqlWireColumn c; c.name = "COPY"; c.type = "text"; res.columns.push_back(c); res.has_result_set = true; }
          if (sink) { if (sink_ok) sink_ok = (*sink)(res.columns, row); }
          else res.rows.push_back(std::move(row));
        }
        break;
      case 'c':   // CopyDone
      case 'I':   // EmptyQueryResponse
      case 'N':   // NoticeResponse
      case 'A':   // NotificationResponse
      case 'S':   // ParameterStatus (a SET changed one)
        break;
      case 'Z':   // ReadyForQuery: the end of this query's answer
        if (!body.empty()) transaction_status_ = static_cast<char>(body[0]);
        return res;
      default:
        return broken(Error{Status::Corrupt, std::string("unexpected PostgreSQL message '") + type + "'"});
    }
  }
}

}  // namespace aios::userland
