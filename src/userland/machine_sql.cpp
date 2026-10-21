// The machine's SQL engine, and SQL across the fleet over the node link - see
// machine_sql.hpp.
#include "aios/userland/machine_sql.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/node/node.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/sql_things.hpp"
#include "aios/userland/task_ledger.hpp"
#include "../node/protocol.hpp"

namespace aios::userland {

namespace {
struct Engine {
  std::unique_ptr<TaskLedger> ledger;
  std::unique_ptr<SqlDatabase> db;
  bool tried = false;
  SqlDatabase* external = nullptr;   // an engine the embedder owns (compute serve's mirror)
  bool read_only = false;            // fleet peers may only read it
};

// Statements that only read: what a peer may run against a read-only engine.
bool reads_only(const std::string& sql) {
  size_t i = 0;
  while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == '\r' || sql[i] == '(')) ++i;
  std::string w;
  while (i < sql.size() && std::isalpha((unsigned char)sql[i])) w += (char)std::toupper((unsigned char)sql[i++]);
  if (w == "SELECT" || w == "SHOW" || w == "DESCRIBE" || w == "DESC" || w == "EXPLAIN" || w == "WITH") return true;
  // A Thing's report (CALL THING) and its controls change no table: a fleet
  // peer may send them to a read-only mirror too.
  if (w == "CALL" || w == "PAUSE" || w == "RESUME" || w == "REFRESH" || w == "FIRE" || w == "RETRY" || w == "SET") {
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    std::string n;
    while (i < sql.size() && std::isalpha((unsigned char)sql[i])) n += (char)std::toupper((unsigned char)sql[i++]);
    return n == "THING" || (w == "CALL" && n == "SAT");
  }
  return false;
}
hal::Mutex g_engines_mu;
std::map<Kernel*, Engine>& engines() { static std::map<Kernel*, Engine> m; return m; }

PeerReach& peer_reach() { static PeerReach r; return r; }
PeerRoster& peer_roster() { static PeerRoster r; return r; }
std::vector<Id128> roster_of(Kernel& k) { return peer_roster() ? peer_roster()(k) : k.fleet_roster(); }

// The default reach: the kernel's own fleet membership (hosted kernels).
Result<std::shared_ptr<RemoteNode>> kernel_reach(Kernel& k, const std::string& want) {
  if (!k.in_fleet()) return Result<std::shared_ptr<RemoteNode>>(Status::Busy, "not in a fleet - `fleet join <hub> <port> <key|@FILE>` first");
  std::vector<Id128> hits;
  for (auto& id : k.fleet_roster()) if (id.str().compare(0, want.size(), want) == 0) hits.push_back(id);
  if (hits.size() != 1)
    return Result<std::shared_ptr<RemoteNode>>(Status::NotFound, (hits.empty() ? "no fleet member starts with " : "more than one fleet member starts with ") + want);
  auto at = k.reach(hits[0]);
  if (!at.ok()) return at.error();
  return k.nodes()[*at];
}

// A query result on the node link: the grid a statement produced, or its
// affected-row count. Values travel as the engine holds them (NULL included).
void encode_result(const SqlQueryResult& r, Writer& w) {
  w.u8(r.is_result_set ? 1 : 0);
  w.u64(r.rows_affected);
  w.u32(static_cast<uint32_t>(r.columns.size()));
  for (size_t i = 0; i < r.columns.size(); ++i) {
    w.str(r.columns[i]);
    w.u8(static_cast<uint8_t>(i < r.column_types.size() ? r.column_types[i] : SqlDataType::Text));
  }
  w.u32(static_cast<uint32_t>(r.rows.size()));
  for (const auto& row : r.rows) {
    w.u32(static_cast<uint32_t>(row.values.size()));
    for (const auto& v : row.values) w.str(v);
  }
}

Result<SqlQueryResult> decode_result(Reader& r) {
  SqlQueryResult out;
  out.is_result_set = r.u8() != 0;
  out.rows_affected = r.u64();
  uint32_t nc = r.u32();
  for (uint32_t i = 0; i < nc && r.ok(); ++i) {
    out.columns.push_back(r.str());
    out.column_types.push_back(static_cast<SqlDataType>(r.u8()));
  }
  uint32_t nr = r.u32();
  for (uint32_t i = 0; i < nr && r.ok(); ++i) {
    SqlRow row;
    uint32_t nv = r.u32();
    for (uint32_t j = 0; j < nv && r.ok(); ++j) row.values.push_back(r.str());
    out.rows.push_back(std::move(row));
  }
  if (!r.ok()) return Result<SqlQueryResult>(Status::Corrupt, "malformed SQL reply");
  return out;
}
}  // namespace

// The Things' view of the fleet (ON <machine>, SHOW ... ON ALL): this
// kernel's roster, each member asked over its node link.
hal::Mutex g_fleet_round_mu;          // held for a whole fleet round
std::atomic<bool> g_fleet_closed{false};

void install_things_fleet(Kernel& k) {
  Kernel* kp = &k;
  set_things_fleet([kp](const std::string& target, const std::string& sql, std::vector<ThingFleetAnswer>* out) {
    if (g_fleet_closed) return;
    hal::LockGuard round(g_fleet_round_mu);
    if (g_fleet_closed) return;
    // Bounded per member and in total: a member that has stopped answering
    // must not hold a SHOW ... ON ALL, or a mirror's pull, for minutes - on
    // the Pi a pull runs on the board's one loop, and nothing else runs
    // meanwhile.
#if defined(AIOS_METAL)
    // The Pi reads its hub link only between rounds: a round must stay short
    // or the hub's frames to it back up (see RelayHub's writer queue).
    constexpr uint64_t kBudgetNs = 4000000000ull;
    constexpr int kPeerMs = 3000;
#else
    constexpr uint64_t kBudgetNs = 20000000000ull;
    constexpr int kPeerMs = 8000;
#endif
    // Each round starts one member further on, so a budget that runs out
    // before the end of the roster still reaches everyone over a few rounds.
    static size_t rotate = 0;
    std::vector<Id128> roster = roster_of(*kp);
    if (!roster.empty()) std::rotate(roster.begin(), roster.begin() + (rotate++ % roster.size()), roster.end());
    const uint64_t t0 = kp->host().monotonic_ns();
    for (const auto& id : roster) {
      if (g_fleet_closed) break;
      const std::string full = id.str();
      if (target != "ALL" && full.compare(0, target.size(), target) != 0) continue;
      ThingFleetAnswer a;
      a.machine = id.short_str();
      if (kp->host().monotonic_ns() - t0 > kBudgetNs) {
        a.error = "skipped: this fleet round's time budget was used up";
        out->push_back(std::move(a));
        continue;
      }
      auto r = sql_on_peer(*kp, full, sql, kPeerMs);
      if (r.ok()) {
        a.ok = true;
        a.result = std::make_shared<SqlQueryResult>(std::move(*r));
      } else {
        a.error = r.error().message;
      }
      out->push_back(std::move(a));
    }
  });
}

void set_machine_sql(Kernel& k, SqlDatabase* db, bool read_only) {
  {
    hal::LockGuard g(g_engines_mu);
    Engine& e = engines()[&k];
    e.external = db;
    e.read_only = read_only;
  }
  install_things_fleet(k);
}

SqlDatabase* machine_sql_if_open(Kernel& k) {
  hal::LockGuard g(g_engines_mu);
  auto it = engines().find(&k);
  if (it == engines().end()) return nullptr;
  return it->second.external ? it->second.external : it->second.db.get();
}

SqlDatabase* machine_sql(Kernel& k) {
  hal::LockGuard g(g_engines_mu);
  Engine& e = engines()[&k];
  // An engine the process already runs on this store is THE engine: opening a
  // second one on the same files served a startup snapshot that never saw
  // what the first one replicated since (st's mirror, over the fleet).
  if (e.external) return e.external;
  if (!e.db && !e.tried) {
    e.tried = true;
    // init() reloads every table off the store, so the engine is opened once
    // for the life of the kernel and kept, not rebuilt per statement.
    auto ledger = std::make_unique<TaskLedger>(k);
    (void)ledger->init("tasks");
    auto db = std::make_unique<SqlDatabase>(k, ledger.get());
    if (db->init("sql_db").ok()) { e.ledger = std::move(ledger); e.db = std::move(db); install_things_fleet(k); }
  }
  return e.db.get();
}

namespace {
std::string fleet_sql_conf(Kernel& k) { return k.config().root + "/fleet_sql.conf"; }
}  // namespace

bool fleet_sql_writes_allowed(Kernel& k) {
  auto f = k.host().read_file(fleet_sql_conf(k));
  if (!f.ok()) return false;
  std::string text(reinterpret_cast<const char*>(f->data()), f->size());
  size_t at = 0;
  while (at <= text.size()) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t s0 = line.find_first_not_of(" \t");
    if (s0 != std::string::npos) line = line.substr(s0);
    if (line.rfind("writes=", 0) == 0) {
      std::string v = line.substr(7);
      for (auto& ch : v) ch = (char)std::tolower((unsigned char)ch);
      return v == "1" || v == "on" || v == "yes" || v == "true";
    }
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  return false;
}

Result<void> set_fleet_sql_writes(Kernel& k, bool on) {
  const std::string text = std::string("# fleet peers' SQL through this machine (sql_on, ... ON <machine>)\n") +
                           "# writes=1: they may change tables here; writes=0 (the default): reads only\n" +
                           "writes=" + (on ? "1" : "0") + "\n";
  return k.host().write_file(fleet_sql_conf(k), ByteSpan(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

bool machine_sql_is_fleet_read_only(Kernel& k) {
  {
    hal::LockGuard g(g_engines_mu);
    auto it = engines().find(&k);
    if (it != engines().end() && it->second.read_only) return true;
  }
  return !fleet_sql_writes_allowed(k);
}

void serve_machine_sql(NodeService& node, Kernel& k) {
  Kernel* kp = &k;
  // A joined member's own `sql query` reaches the fleet too (ON ALL, MIRROR):
  // the hook used to wait for the machine engine to be opened by something else.
  install_things_fleet(k);
  node.set_sql_runner([kp](const std::string& sql) -> Result<ByteBuffer> {
    bool ro = false;
    { hal::LockGuard g(g_engines_mu); auto it = engines().find(kp); ro = it != engines().end() && it->second.read_only; }
    if (ro && !reads_only(sql))
      return Result<ByteBuffer>(Status::Denied, "this machine's SQL engine is a read-only mirror: SELECT, SHOW, DESCRIBE and EXPLAIN only");
    // Writes from the fleet are this member's choice, not every key holder's:
    // any member used to INSERT, CREATE and DROP on any other.
    if (!reads_only(sql) && !fleet_sql_writes_allowed(*kp))
      return Result<ByteBuffer>(Status::Denied, "this machine takes only reads from fleet peers (SELECT, SHOW, DESCRIBE, EXPLAIN); "
                                                "its operator can allow writes with `fleet sql writes=on`");
    SqlDatabase* db = machine_sql(*kp);
    if (!db) return Result<ByteBuffer>(Status::Unsupported, "the SQL engine did not open on this machine");
    // The machine's own engine, never a remote profile it happens to have
    // selected: a fleet peer asked THIS machine.
    SqlQueryResult r = db->execute_local_query(sql);
    if (!r.success) return Result<ByteBuffer>(Status::Invalid, r.error_message);
    ByteBuffer out; Writer w(out);
    encode_result(r, w);
    return out;
  });
}

void set_peer_reach(PeerReach reach) { peer_reach() = std::move(reach); }
void set_peer_roster(PeerRoster roster) { peer_roster() = std::move(roster); }

void machine_sql_shutdown(Kernel& k) {
  g_fleet_closed = true;
  { hal::LockGuard wait(g_fleet_round_mu); }   // a round already running finishes first
  std::unique_ptr<SqlDatabase> db;
  std::unique_ptr<TaskLedger> ledger;
  {
    hal::LockGuard g(g_engines_mu);
    auto it = engines().find(&k);
    if (it == engines().end()) return;
    db = std::move(it->second.db);
    ledger = std::move(it->second.ledger);
    engines().erase(it);
  }
  db.reset();       // stops and joins its clock thread
  ledger.reset();
}

Result<SqlQueryResult> sql_on_peer(Kernel& k, const std::string& peer_prefix, const std::string& sql, int timeout_ms) {
  auto node = peer_reach() ? peer_reach()(k, peer_prefix) : kernel_reach(k, peer_prefix);
  if (!node.ok()) return node.error();
  ByteBuffer body; Writer w(body);
  w.str(sql);
  auto rep = (*node)->call(nodeproto::Sql, body, timeout_ms);
  if (!rep.ok()) return rep.error();
  Reader r(*rep);
  return decode_result(r);
}

// ---------------------------------------------------------------------------
// grain: sql(statement), sql_on(peer, statement)
//
// A statement that produces a grid returns a list of maps, one per row, keyed
// by column name - numbers for the engine's Int/Real columns, nil for NULL.
// Anything else returns {affected: n}. A failed statement is a grain error
// (pcall() catches it), not a map with ok=false to forget to check.
// ---------------------------------------------------------------------------
namespace {
Kernel* g_vm_kernel = nullptr;   // NativeFn is a plain function pointer; one kernel per process

grain::Value to_grain(const SqlQueryResult& r) {
  if (!r.is_result_set) {
    auto m = std::make_shared<grain::MapObj>();
    m->put("affected", grain::Value::integer(static_cast<int64_t>(r.rows_affected)));
    return grain::Value::object(grain::Type::Map, m);
  }
  std::vector<grain::Value> rows;
  rows.reserve(r.rows.size());
  for (const auto& row : r.rows) {
    auto m = std::make_shared<grain::MapObj>();
    for (size_t i = 0; i < r.columns.size() && i < row.values.size(); ++i) {
      const std::string& v = row.values[i];
      const SqlDataType t = i < r.column_types.size() ? r.column_types[i] : SqlDataType::Text;
      grain::Value gv;
      if (sql_is_null(v)) gv = grain::Value::nil();
      else if (t == SqlDataType::Int) { char* e = nullptr; long long n = std::strtoll(v.c_str(), &e, 10); gv = (e && *e == 0 && !v.empty()) ? grain::Value::integer(n) : grain::Value::string(v); }
      else if (t == SqlDataType::Real || t == SqlDataType::Decimal) { char* e = nullptr; double d = std::strtod(v.c_str(), &e); gv = (e && *e == 0 && !v.empty()) ? grain::Value::real(d) : grain::Value::string(v); }
      else gv = grain::Value::string(v);
      m->put(r.columns[i], gv);
    }
    rows.push_back(grain::Value::object(grain::Type::Map, m));
  }
  return grain::Value::list(std::move(rows));
}

grain::Value bi_sql(grain::Vm&, std::vector<grain::Value>& args, std::string& error) {
  if (args[0].type != grain::Type::Str) { error = "sql() wants a statement string"; return grain::Value::nil(); }
  SqlDatabase* db = g_vm_kernel ? machine_sql(*g_vm_kernel) : nullptr;
  if (!db) { error = "sql(): the SQL engine did not open on this machine"; return grain::Value::nil(); }
  SqlQueryResult r = db->execute_local_query(args[0].str());
  if (!r.success) { error = "sql(): " + r.error_message; return grain::Value::nil(); }
  return to_grain(r);
}

grain::Value bi_sql_on(grain::Vm&, std::vector<grain::Value>& args, std::string& error) {
  if (args[0].type != grain::Type::Str || args[1].type != grain::Type::Str) { error = "sql_on() wants (peer-id-prefix, statement)"; return grain::Value::nil(); }
  if (!g_vm_kernel) { error = "sql_on(): no kernel"; return grain::Value::nil(); }
  auto r = sql_on_peer(*g_vm_kernel, args[0].str(), args[1].str());
  if (!r.ok()) { error = "sql_on(" + args[0].str() + "): " + r.error().message; return grain::Value::nil(); }
  return to_grain(*r);
}
}  // namespace

void install_sql_builtins(grain::Vm& vm, Kernel& k) {
  g_vm_kernel = &k;
  vm.define_native("sql", 1, 1, bi_sql);
  vm.define_native("sql_on", 2, 2, bi_sql_on);
}

}  // namespace aios::userland
