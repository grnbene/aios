// grain's database client, and live query results.
//
//   let db = db_connect("127.0.0.1", 3307, "erp", password, "versioneight")
//   let r = db_query(db, "select stockid, description from stock limit 5")
//   r.rows            [[...], ...]   (strings, nil for NULL)
//   r.records         [{stockid: ..., description: ...}, ...]
//
//   let late = live(db, "select orderno, customer from orders where late = 1")
//   ui.table(late.records)          # stays current: nothing else to write
//
// The MySQL/MariaDB wire protocol (the text protocol: handshake, mysql_native_password
// or caching_sha2_password's fast path, COM_QUERY), spoken over hal::net with nothing
// under it - the same protocol ASQL answers on its MySQL port.
//
// live() keeps a query's result current. Its value is a map whose fields are rewritten
// in place when the result changes, so a view that reads it shows the change, and a web
// app pushes it to every viewer. Against ASQL a tick costs one lookup - ASQL_CHANGES()
// over the query's tables, which moves only when one of them is written - and the
// query itself runs only then. Against another server it runs the query each tick.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/net.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

// ---- the wire -------------------------------------------------------------------

constexpr uint32_t kLongPassword = 0x1, kConnectWithDb = 0x8, kProtocol41 = 0x200, kTransactions = 0x2000,
                   kSecureConnection = 0x8000, kMultiResults = 0x20000, kPluginAuth = 0x80000;

struct Column { std::string name; };

struct QueryResult {
  bool ok = false;
  std::string error;
  std::vector<Column> columns;
  std::vector<std::vector<std::pair<bool, std::string>>> rows;   // (is_null, text)
  uint64_t affected = 0, insert_id = 0;
};

class Session {
 public:
  bool connect(const std::string& host, uint16_t port, const std::string& user, const std::string& pass,
               const std::string& db, int timeout_ms, std::string& error) {
    auto c = hal::net_connect(host, port, timeout_ms);
    if (!c.ok()) { error = "cannot connect to " + host + ":" + std::to_string(port) + ": " + c.error().message; return false; }
    conn_ = std::move(*c);
    std::vector<uint8_t> p;
    if (!read_packet(p, error)) return false;
    if (!p.empty() && p[0] == 0xFF) { error = err_text(p); return false; }
    // Handshake v10
    size_t at = 1;
    const size_t ver_end = std::find(p.begin() + 1, p.end(), 0) - p.begin();
    server_version_ = std::string(p.begin() + 1, p.begin() + (long)ver_end);
    at = ver_end + 1;
    if (at + 4 + 8 + 1 + 2 > p.size()) { error = "a malformed handshake from the server"; return false; }
    at += 4;   // thread id
    std::vector<uint8_t> salt(p.begin() + (long)at, p.begin() + (long)at + 8);
    at += 8 + 1;
    uint32_t caps = p[at] | (p[at + 1] << 8);
    at += 2;
    std::string plugin = "mysql_native_password";
    if (at + 1 + 2 + 2 + 1 + 10 <= p.size()) {
      at += 1 + 2;   // charset, status
      caps |= (uint32_t)(p[at] | (p[at + 1] << 8)) << 16;
      at += 2;
      const size_t data_len = p[at];
      at += 1 + 10;
      const size_t part2 = std::max<size_t>(13, data_len > 8 ? data_len - 8 : 0);
      if (caps & kSecureConnection && at + part2 <= p.size()) {
        salt.insert(salt.end(), p.begin() + (long)at, p.begin() + (long)(at + part2));
        if (!salt.empty() && salt.back() == 0) salt.pop_back();
        at += part2;
      }
      if (caps & kPluginAuth && at < p.size()) {
        const size_t end = std::find(p.begin() + (long)at, p.end(), 0) - p.begin();
        plugin = std::string(p.begin() + (long)at, p.begin() + (long)end);
      }
    }
    if (!(caps & kProtocol41)) { error = "the server is older than MySQL 4.1"; return false; }
    pass_ = pass;
    // HandshakeResponse41
    std::vector<uint8_t> r;
    uint32_t want = kLongPassword | kProtocol41 | kTransactions | kSecureConnection | kMultiResults | kPluginAuth;
    if (!db.empty()) want |= kConnectWithDb;
    put32(r, want);
    put32(r, 16 * 1024 * 1024);
    r.push_back(45);   // utf8mb4_general_ci
    r.insert(r.end(), 23, 0);
    r.insert(r.end(), user.begin(), user.end());
    r.push_back(0);
    const std::vector<uint8_t> auth = scramble(plugin, salt);
    r.push_back((uint8_t)auth.size());
    r.insert(r.end(), auth.begin(), auth.end());
    if (!db.empty()) { r.insert(r.end(), db.begin(), db.end()); r.push_back(0); }
    r.insert(r.end(), plugin.begin(), plugin.end());
    r.push_back(0);
    if (!write_packet(r, error)) return false;
    return finish_auth(plugin, error);
  }

  bool query(const std::string& sql, QueryResult& out, std::string& error) {
    out = QueryResult{};
    seq_ = 0;
    std::vector<uint8_t> cmd{0x03};
    cmd.insert(cmd.end(), sql.begin(), sql.end());
    if (!write_packet(cmd, error)) return false;
    std::vector<uint8_t> p;
    if (!read_packet(p, error)) return false;
    if (p.empty()) { error = "an empty answer from the server"; return false; }
    if (p[0] == 0xFF) { out.error = err_text(p); return true; }
    if (p[0] == 0x00) {   // OK: no result set
      size_t at = 1;
      out.affected = lenenc(p, at);
      out.insert_id = lenenc(p, at);
      out.ok = true;
      drain_more_results(p, error);
      return error.empty();
    }
    size_t at = 0;
    const uint64_t ncols = lenenc(p, at);
    for (uint64_t k = 0; k < ncols; ++k) {
      if (!read_packet(p, error)) return false;
      size_t c = 0;
      for (int skip = 0; skip < 4; ++skip) lenstr(p, c);   // catalog, schema, table, org_table
      out.columns.push_back({lenstr(p, c)});
    }
    if (!read_packet(p, error)) return false;   // EOF after the column definitions
    for (;;) {
      if (!read_packet(p, error)) return false;
      if (!p.empty() && p[0] == 0xFE && p.size() < 9) break;   // EOF: the end of the rows
      if (!p.empty() && p[0] == 0xFF) { out.error = err_text(p); return true; }
      std::vector<std::pair<bool, std::string>> row;
      size_t c = 0;
      for (uint64_t k = 0; k < ncols; ++k) {
        if (c < p.size() && p[c] == 0xFB) { row.push_back({true, ""}); ++c; }
        else row.push_back({false, lenstr(p, c)});
      }
      out.rows.push_back(std::move(row));
    }
    out.ok = true;
    drain_more_results(p, error);
    return error.empty();
  }

  void close() {
    if (!conn_) return;
    std::string ignore;
    seq_ = 0;
    write_packet({0x01}, ignore);   // COM_QUIT
    conn_->close();
    conn_.reset();
  }
  bool open() const { return conn_ != nullptr; }
  const std::string& server_version() const { return server_version_; }

 private:
  std::unique_ptr<hal::Connection> conn_;
  uint8_t seq_ = 0;
  std::string pass_;
  std::string server_version_;

  static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }

  static uint64_t lenenc(const std::vector<uint8_t>& p, size_t& at) {
    if (at >= p.size()) return 0;
    const uint8_t b = p[at++];
    auto take = [&](int n) { uint64_t v = 0; for (int i = 0; i < n && at < p.size(); ++i) v |= (uint64_t)p[at++] << (8 * i); return v; };
    if (b < 0xFB) return b;
    if (b == 0xFC) return take(2);
    if (b == 0xFD) return take(3);
    if (b == 0xFE) return take(8);
    return 0;
  }
  static std::string lenstr(const std::vector<uint8_t>& p, size_t& at) {
    const uint64_t n = lenenc(p, at);
    const size_t take = (size_t)std::min<uint64_t>(n, p.size() - std::min(at, p.size()));
    std::string s(p.begin() + (long)at, p.begin() + (long)(at + take));
    at += take;
    return s;
  }
  static std::string err_text(const std::vector<uint8_t>& p) {
    if (p.size() < 3) return "the server refused";
    const int code = p[1] | (p[2] << 8);
    size_t at = 3;
    if (at < p.size() && p[at] == '#') at += 6;
    return std::string(p.begin() + (long)std::min(at, p.size()), p.end()) + " (" + std::to_string(code) + ")";
  }

  bool read_packet(std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    for (;;) {
      uint8_t head[4];
      if (!conn_ || !conn_->recv_all(MutableByteSpan(head, 4), 30000)) { error = "the database connection closed"; return false; }
      const size_t n = head[0] | (head[1] << 8) | (head[2] << 16);
      seq_ = (uint8_t)(head[3] + 1);
      const size_t was = out.size();
      out.resize(was + n);
      if (n && !conn_->recv_all(MutableByteSpan(out.data() + was, n), 30000)) { error = "the database connection closed"; return false; }
      if (n < 0xFFFFFF) return true;   // a payload of exactly 16 MiB - 1 continues in the next packet
    }
  }
  bool write_packet(const std::vector<uint8_t>& payload, std::string& error) {
    std::vector<uint8_t> out{(uint8_t)payload.size(), (uint8_t)(payload.size() >> 8), (uint8_t)(payload.size() >> 16), seq_++};
    out.insert(out.end(), payload.begin(), payload.end());
    if (!conn_ || !conn_->send_all(ByteSpan(out.data(), out.size()))) { error = "the database connection closed"; return false; }
    return true;
  }

  std::vector<uint8_t> scramble(const std::string& plugin, const std::vector<uint8_t>& salt) const {
    if (pass_.empty()) return {};
    const ByteSpan pw(reinterpret_cast<const uint8_t*>(pass_.data()), pass_.size());
    if (plugin == "caching_sha2_password") {
      // SHA256(pass) XOR SHA256(SHA256(SHA256(pass)) + salt)
      const auto h1 = crypto::sha256(pw);
      const auto h2 = crypto::sha256(ByteSpan(h1.data(), h1.size()));
      std::vector<uint8_t> buf(h2.begin(), h2.end());
      buf.insert(buf.end(), salt.begin(), salt.end());
      const auto h3 = crypto::sha256(ByteSpan(buf.data(), buf.size()));
      std::vector<uint8_t> out(32);
      for (int i = 0; i < 32; ++i) out[i] = h1[i] ^ h3[i];
      return out;
    }
    // mysql_native_password: SHA1(pass) XOR SHA1(salt + SHA1(SHA1(pass)))
    const auto h1 = crypto::sha1(pw);
    const auto h2 = crypto::sha1(ByteSpan(h1.data(), h1.size()));
    std::vector<uint8_t> buf(salt.begin(), salt.begin() + (long)std::min<size_t>(20, salt.size()));
    buf.insert(buf.end(), h2.begin(), h2.end());
    const auto h3 = crypto::sha1(ByteSpan(buf.data(), buf.size()));
    std::vector<uint8_t> out(20);
    for (int i = 0; i < 20; ++i) out[i] = h1[i] ^ h3[i];
    return out;
  }

  bool finish_auth(std::string plugin, std::string& error) {
    for (int round = 0; round < 4; ++round) {
      std::vector<uint8_t> p;
      if (!read_packet(p, error)) return false;
      if (p.empty()) { error = "an empty answer to the login"; return false; }
      if (p[0] == 0x00) return true;
      if (p[0] == 0xFF) { error = err_text(p); return false; }
      if (p[0] == 0xFE) {   // switch to another method, with a new salt
        const size_t end = std::find(p.begin() + 1, p.end(), 0) - p.begin();
        plugin = std::string(p.begin() + 1, p.begin() + (long)end);
        std::vector<uint8_t> salt(p.begin() + (long)std::min(end + 1, p.size()), p.end());
        if (!salt.empty() && salt.back() == 0) salt.pop_back();
        if (plugin != "mysql_native_password" && plugin != "caching_sha2_password") { error = "the server wants the " + plugin + " login, which grain does not speak"; return false; }
        if (!write_packet(scramble(plugin, salt), error)) return false;
        continue;
      }
      if (p[0] == 0x01 && p.size() >= 2) {
        if (p[1] == 0x03) continue;   // caching_sha2: fast path accepted, an OK follows
        if (p[1] == 0x04) { error = "this account needs caching_sha2_password's full login (TLS or RSA), which grain does not do yet; use mysql_native_password for it"; return false; }
      }
      error = "an unexpected answer to the login";
      return false;
    }
    error = "the login did not finish";
    return false;
  }

  // CLIENT_MULTI_RESULTS: a statement may answer with more than one result; only the first is kept.
  void drain_more_results(std::vector<uint8_t>& last, std::string& error) {
    for (int guard = 0; guard < 64; ++guard) {
      size_t at = 1;
      if (last.empty()) return;
      uint16_t status = 0;
      if (last[0] == 0x00) { lenenc(last, at); lenenc(last, at); if (at + 2 <= last.size()) status = last[at] | (last[at + 1] << 8); }
      else if (last[0] == 0xFE && last.size() >= 5) status = last[3] | (last[4] << 8);
      if (!(status & 0x0008)) return;   // SERVER_MORE_RESULTS_EXISTS
      QueryResult skip;
      if (!read_packet(last, error)) return;
      if (last.empty() || last[0] == 0x00 || last[0] == 0xFF) continue;
      size_t c = 0;
      const uint64_t n = lenenc(last, c);
      for (uint64_t k = 0; k < n + 1; ++k) if (!read_packet(last, error)) return;
      do { if (!read_packet(last, error)) return; } while (!(last.size() < 9 && !last.empty() && last[0] == 0xFE));
    }
  }
};

std::shared_ptr<Session> session_of(const Value& v) {
  const Value* c = &v;
  if (v.type == Type::Map) c = static_cast<MapObj*>(v.obj.get())->find("conn");
  if (!c || c->type != Type::Extern) return nullptr;
  auto ext = std::static_pointer_cast<ExternObj>(c->obj);
  return ext->kind_name == "grain_db" ? std::static_pointer_cast<Session>(ext->ptr) : nullptr;
}

Value result_value(const QueryResult& r) {
  auto out = std::make_shared<MapObj>();
  out->put("ok", Value::boolean(r.ok && r.error.empty()));
  out->put("error", Value::string(r.error));
  std::vector<Value> cols;
  for (const auto& c : r.columns) cols.push_back(Value::string(c.name));
  std::vector<Value> rows, records;
  for (const auto& row : r.rows) {
    std::vector<Value> cells;
    auto rec = std::make_shared<MapObj>();
    for (size_t k = 0; k < row.size(); ++k) {
      Value cell = row[k].first ? Value::nil() : Value::string(row[k].second);
      cells.push_back(cell);
      if (k < r.columns.size()) rec->put(r.columns[k].name, cell);
    }
    rows.push_back(Value::list(std::move(cells)));
    records.push_back(Value::object(Type::Map, rec));
  }
  out->put("columns", Value::list(std::move(cols)));
  out->put("rows", Value::list(std::move(rows)));
  out->put("records", Value::list(std::move(records)));
  out->put("affected_rows", Value::integer((int64_t)r.affected));
  out->put("last_insert_id", Value::integer((int64_t)r.insert_id));
  return Value::object(Type::Map, out);
}

// db_connect(host, port, user, password, database?) -> {ok, conn, error, server_version}
Value bi_db_connect(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "db_connect") || !want_number(args[1], error, "db_connect")) return Value::nil();
  auto s = std::make_shared<Session>();
  std::string why;
  auto out = std::make_shared<MapObj>();
  const int64_t port = args[1].as_int();
  if (port < 1 || port > 65535) { error = "db_connect(): a port from 1 to 65535"; return Value::nil(); }
  if (!s->connect(args[0].str(), (uint16_t)port, args[2].to_text(), args[3].type == Type::Nil ? "" : args[3].to_text(),
                  args.size() > 4 && args[4].type != Type::Nil ? args[4].to_text() : "", 10000, why)) {
    out->put("ok", Value::boolean(false));
    out->put("error", Value::string(why));
    return Value::object(Type::Map, out);
  }
  out->put("ok", Value::boolean(true));
  out->put("error", Value::string(""));
  out->put("server_version", Value::string(s->server_version()));
  out->put("conn", Value::object(Type::Extern, std::make_shared<ExternObj>("grain_db", s)));
  return Value::object(Type::Map, out);
}

// db_query(db, sql) -> {ok, error, columns, rows, records, affected_rows, last_insert_id}
Value bi_db_query(Vm&, std::vector<Value>& args, std::string& error) {
  auto s = session_of(args[0]);
  if (!s || !s->open()) { error = "db_query() wants a connection from db_connect() that is still open"; return Value::nil(); }
  if (!want_str(args[1], error, "db_query")) return Value::nil();
  QueryResult r;
  std::string why;
  if (!s->query(args[1].str(), r, why)) { error = "db_query(): " + why; return Value::nil(); }
  return result_value(r);
}

Value bi_db_close(Vm&, std::vector<Value>& args, std::string&) {
  if (auto s = session_of(args[0])) s->close();
  return Value::nil();
}

// ---- live(): a query whose result stays current -------------------------------------

// The tables a query reads: the names after FROM and JOIN. Good enough to watch them; live(db, sql, {tables: [...]})
// says it outright for a query this does not read right.
std::vector<std::string> tables_of(const std::string& sql) {
  std::vector<std::string> out;
  std::string up = sql;
  for (char& c : up) c = (char)std::toupper((unsigned char)c);
  for (const char* kw : {"FROM", "JOIN"}) {
    size_t at = 0;
    const size_t n = std::strlen(kw);
    while ((at = up.find(kw, at)) != std::string::npos) {
      const bool word = (at == 0 || !std::isalnum((unsigned char)up[at - 1])) && (at + n >= up.size() || !std::isalnum((unsigned char)up[at + n]));
      at += n;
      if (!word) continue;
      size_t b = at;
      while (b < sql.size() && std::isspace((unsigned char)sql[b])) ++b;
      if (b < sql.size() && sql[b] == '(') continue;   // a subquery: its own FROM is found by the scan
      size_t e = b;
      while (e < sql.size() && (std::isalnum((unsigned char)sql[e]) || sql[e] == '_' || sql[e] == '.' || sql[e] == '`')) ++e;
      std::string name = sql.substr(b, e - b);
      name.erase(std::remove(name.begin(), name.end(), '`'), name.end());
      if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
  }
  return out;
}

std::string sql_quote(const std::string& s) {
  std::string o = "'";
  for (char c : s) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
  return o + "'";
}

// One tick of a live query (the method its timer calls).
Value method_live_tick(Vm&, BoundMethodObj& bm, std::vector<Value>&, std::string&) {
  auto* st = static_cast<MapObj*>(bm.receiver.obj.get());
  auto* box = static_cast<MapObj*>(st->find("box")->obj.get());
  auto s = session_of(*st->find("db"));
  if (!s || !s->open()) { box->put("error", Value::string("the connection is closed")); return Value::nil(); }
  std::string why;
  // ASQL: has anything this query reads been written since the last run?
  Value* probe = st->find("probe");
  if (probe && probe->type == Type::Str && !probe->str().empty()) {
    QueryResult m;
    if (s->query(probe->str(), m, why) && m.ok && m.error.empty() && !m.rows.empty() && !m.rows[0].empty()) {
      const std::string marker = m.rows[0][0].second;
      Value* last = st->find("marker");
      if (last && last->type == Type::Str && last->str() == marker) return Value::nil();   // unchanged: nothing to do
      st->put("marker", Value::string(marker));
    } else {
      st->put("probe", Value::string(""));   // not ASQL (or no such function): run the query every tick instead
      box->put("watching", Value::string("polling"));
    }
  }
  QueryResult r;
  box->put("runs", Value::integer(box->find("runs")->as_int() + 1));   // how often the query itself ran
  if (!s->query(st->find("sql")->str(), r, why)) { box->put("error", Value::string(why)); return Value::nil(); }
  Value fresh = result_value(r);
  auto* f = static_cast<MapObj*>(fresh.obj.get());
  // Rewrite the box in place only when the answer differs, so a view reading it changes only then.
  Value* old_rows = box->find("rows");
  const bool same = old_rows && old_rows->equals(*f->find("rows")) && box->find("columns")->equals(*f->find("columns")) &&
                    box->find("error")->equals(*f->find("error"));
  if (same) return Value::nil();
  for (const char* k : {"ok", "error", "columns", "rows", "records"}) box->put(k, *f->find(k));
  box->put("changes", Value::integer(box->find("changes")->as_int() + 1));
  box->put("changed_at", Value::real(wall_ms()));
  return Value::nil();
}

// live(db, sql, options?) -> a map kept current: {rows, records, columns, ok, error, changes, runs, changed_at, watching}
// options: every (seconds between looks, default 1), tables (what the query reads, when the guess is wrong)
Value bi_live(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (vm.worker) { error = "live(): a worker (parallel_map, spawn) has no loop to keep a result current; use db_query, or live() in the program itself"; return Value::nil(); }
  if (!session_of(args[0])) { error = "live() wants a connection from db_connect()"; return Value::nil(); }
  if (!want_str(args[1], error, "live")) return Value::nil();
  double every = 1.0;
  std::vector<std::string> tables = tables_of(args[1].str());
  if (args.size() > 2 && args[2].type == Type::Map) {
    auto* o = static_cast<MapObj*>(args[2].obj.get());
    if (Value* e = o->find("every"); e && e->is_number()) every = std::max(0.05, e->as_double());
    if (Value* t = o->find("tables"); t && t->type == Type::List) {
      tables.clear();
      for (const auto& x : t->list_items()) tables.push_back(x.to_text());
    }
  }
  auto box = std::make_shared<MapObj>();
  for (const char* k : {"columns", "rows", "records"}) box->put(k, Value::list({}));
  box->put("ok", Value::boolean(false));
  box->put("error", Value::string(""));
  box->put("changes", Value::integer(0));
  box->put("runs", Value::integer(0));
  box->put("changed_at", Value::real(0));
  box->put("watching", Value::string(tables.empty() ? "polling" : "asql changes"));
  auto st = std::make_shared<MapObj>();
  st->put("box", Value::object(Type::Map, box));
  st->put("db", args[0]);
  st->put("sql", args[1]);
  std::string probe;
  if (!tables.empty()) {
    probe = "SELECT ASQL_CHANGES(";
    for (size_t k = 0; k < tables.size(); ++k) probe += (k ? ", " : "") + sql_quote(tables[k]);
    probe += ")";
  }
  st->put("probe", Value::string(probe));
  st->put("marker", Value::nil());
  Value tick = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(Value::object(Type::Map, st), "live", 0, 0, method_live_tick));
  std::string ignore;
  std::vector<Value> none;
  method_live_tick(vm, *static_cast<BoundMethodObj*>(tick.obj.get()), none, ignore);   // the first answer, now
  Vm::Timer t;
  t.id = vm.next_timer_id++;
  t.period_ms = every * 1000.0;
  t.due_ms = wall_ms() + t.period_ms;
  t.fn = tick;
  vm.timers.push_back(t);
  box->put("timer", Value::integer((int64_t)t.id));
  return Value::object(Type::Map, box);
}

}  // namespace

void install_db_builtins(Vm& vm) {
  vm.define_native("db_connect", 4, 5, bi_db_connect);
  vm.define_native("db_query", 2, 2, bi_db_query);
  vm.define_native("db_close", 1, 1, bi_db_close);
  vm.define_native("live", 2, 3, bi_live);
}

}  // namespace aios::userland::grain
