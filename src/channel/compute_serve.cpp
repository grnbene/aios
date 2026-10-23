// The compute service. One thread accepts and reads requests; one executor
// turns each into a graph and runs it through the scheduler; a small thread
// per waiting caller delivers the answer or, when the caller's deadline comes
// first, a promise. Finished answers stay in a cache so the same question —
// the dispatch board asks it on every load — costs nothing the second time.
#include "aios/channel/compute_serve.hpp"
#include "aios/core/strbuf.hpp"
#include "aios/channel/compute_wire.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/compute/erp_kernels.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/sidecar/shm_abi.hpp"
#include "aios/userland/sql_database.hpp"
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#if !defined(AIOS_WASI_NO_THREADS)  // (wasm without threads: aios/core/wasi_threads.hpp declares the lock types)
#include <shared_mutex>
#endif
#include <unordered_map>

namespace aios::channel {
namespace {

constexpr size_t kMaxFrame = 4u << 20;

// Fixed-point rendering without the C library: sign, whole part, `decimals`
// digits of fraction, rounded half up.
std::string fmt_fixed(double v, int decimals) {
  if (!(v == v)) return "null";
  const bool neg = v < 0;
  if (neg) v = -v;
  double scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  uint64_t whole = (uint64_t)v;
  uint64_t frac = (uint64_t)((v - (double)whole) * scale + 0.5);
  if (frac >= (uint64_t)scale) { whole += 1; frac = 0; }
  std::string s = std::to_string(whole);
  if (decimals > 0) {
    std::string f = std::to_string(frac);
    while ((int)f.size() < decimals) f = "0" + f;
    s += "." + f;
  }
  return neg ? "-" + s : s;
}

bool read_frame(hal::Connection& c, std::string& out, int timeout_ms) {
  uint8_t hdr[4];
  if (!c.recv_all(MutableByteSpan(hdr, 4), timeout_ms)) return false;
  const uint32_t n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  if (n == 0 || n > kMaxFrame) return false;
  out.resize(n);
  return c.recv_all(MutableByteSpan(reinterpret_cast<uint8_t*>(out.data()), n), timeout_ms);
}

bool write_frame(hal::Connection& c, const std::string& body) {
  std::string f;
  f.reserve(4 + body.size());
  const uint32_t n = (uint32_t)body.size();
  f.push_back((char)(n >> 24)); f.push_back((char)(n >> 16)); f.push_back((char)(n >> 8)); f.push_back((char)n);
  f += body;
  return c.send_all(span_of(f));
}

RpcValue error_answer(const std::string& op, const std::string& why) {
  RpcValue r = RpcValue::object();
  r.set("ok", RpcValue::boolean(false));
  if (!op.empty()) r.set("op", RpcValue::text(op));
  r.set("error", RpcValue::text(why));
  return r;
}

// This service is a network door with only a shared token behind it; the
// database's own write-through/write-behind gateway is not a boundary it
// gets to rely on. So "sql" never reaches execute_query with anything but
// a read, no matter how sync_mode is configured on the attached database.
std::string first_sql_word(const std::string& sql) {
  size_t i = 0;
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  size_t j = i;
  while (j < sql.size() && (std::isalpha((unsigned char)sql[j]))) ++j;
  std::string w = sql.substr(i, j - i);
  for (auto& ch : w) ch = (char)std::toupper((unsigned char)ch);
  return w;
}

bool is_read_only_sql(const std::string& sql) {
  static const std::vector<std::string> kReadWords = {
      "SELECT", "SHOW", "DESCRIBE", "DESC", "EXPLAIN", "WITH", "OPTIMIZE", "VACUUM", "ANALYZE"
  };
  const std::string w = first_sql_word(sql);
  for (auto& r : kReadWords) if (w == r) return true;
  return false;
}

// Replication control: CHANGE REPLICATION SOURCE, START/STOP REPLICA,
// SET REPLICATION WHITELIST.  These mutate the CDC stream, not user data.
bool is_replication_sql(const std::string& sql) {
  const std::string w = first_sql_word(sql);
  if (w == "CHANGE" || w == "START" || w == "STOP") return true;
  if (w == "SET") {
    // Allow SET REPLICATION ... but not arbitrary SET
    std::string upper;
    for (size_t i = 0; i < std::min(sql.size(), (size_t)40); ++i)
      upper += (char)std::toupper((unsigned char)sql[i]);
    return upper.find("REPLICATION") != std::string::npos;
  }
  return false;
}

struct Job {
  std::string op;                 // "tsp_exact" or "route_improve"
  std::string key;
  std::vector<std::string> ids;   // tsp_exact: ids[0] is the start; route_improve: one per stop
  std::vector<double> latlng;     // 2 per point
  bool prune = true;
  // route_improve only
  double start[2] = {0, 0};
  double end[2] = {0, 0};
  int end_mode = 0;               // 0 home, 1 last, 2 custom
  std::atomic<bool> done{false};
  RpcValue answer;                // valid once done
};

struct Waiter {
  hal::Thread thread;
  std::atomic<bool> finished{false};
};

struct Service {
  Kernel& k;
  Surface& worker;
  const ComputeServeOptions& o;
  std::function<void(const std::string&)> log;
  uint32_t lanes;
  userland::SqlDatabase* sql_db;

  hal::Mutex queue_mu;
  hal::CondVar queue_cv;   // a job arrived, or the service is stopping
  std::deque<std::shared_ptr<Job>> queue;

  hal::Mutex cache_mu;
  std::unordered_map<std::string, std::shared_ptr<Job>> cache;
  std::deque<std::string> cache_order;

  aios::hal::SharedMutex sql_rw_mu;  // Scalable concurrency: shared reader lock for SELECTs, exclusive lock for mutations

  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> requests{0}, solved{0}, served_cached{0}, pending{0}, errors{0};

  Service(Kernel& kk, Surface& w, const ComputeServeOptions& oo, std::function<void(const std::string&)> l, uint32_t lanes_, userland::SqlDatabase* db)
      : k(kk), worker(w), o(oo), log(std::move(l)), lanes(lanes_), sql_db(db) {}

  // A read straight off the attached database's hot local tables — no graph,
  // no scheduler, no cache: the same round trip `aios sql query` makes, just
  // reachable from something that isn't aios. Anything that isn't a read
  // never reaches execute_query, no matter what the database's own sync mode
  // is set to (see is_read_only_sql).
  // `db`: the database the caller's connection is in (a multi-tenant ERP names
  // its tenant). Unqualified tables resolve there, and a database this engine
  // does not hold is refused - never answered from another one: a mirror of
  // one tenant answering another tenant's read is the worst wrong answer there
  // is, because nothing downstream can tell it is wrong.
  RpcValue run_sql(const std::string& sql, const std::string& db = std::string()) {
    if (!sql_db) return error_answer("sql", "no database attached to this compute service");
    if (!db.empty()) {
      bool held = false;
      for (const auto& d : sql_db->list_databases()) held |= d == db;
      if (!held) return error_answer("sql", "database not held here: " + db);
    }
    if (!is_read_only_sql(sql) && !is_replication_sql(sql))
      return error_answer("sql", "reads only: SELECT / SHOW / DESCRIBE / EXPLAIN / WITH (+ replication control)");
    userland::SqlQueryResult res;
    {
      std::shared_lock<aios::hal::SharedMutex> g(sql_rw_mu);
      // This is the door the ERP's read fast-path comes in by, so tagging it
      // is what lets a monitor tell application traffic apart from its own.
      userland::SqlDatabase::set_client_tag("rpc");
      res = sql_db->execute_query(sql, db);
    }
    if (!res.success) return error_answer("sql", res.error_message);
    RpcValue r = RpcValue::object();
    r.set("ok", RpcValue::boolean(true));
    r.set("op", RpcValue::text("sql"));
    RpcValue cols = RpcValue::list();
    for (auto& c : res.columns) cols.push(RpcValue::text(c));
    r.set("columns", cols);
    RpcValue rows = RpcValue::list();
    for (auto& row : res.rows) {
      RpcValue rv = RpcValue::list();
      for (auto& v : row.values) rv.push(userland::sql_is_null(v) ? RpcValue() : RpcValue::text(v));
      rows.push(rv);
    }
    r.set("rows", rows);
    r.set("row_count", RpcValue::number((uint64_t)res.rows.size()));
    return r;
  }

  // One request becomes one graph with one Custom operation; the scheduler
  // places it, the device runs it, the report says where and for how long.
  RpcValue solve(Job& j) {
    const int n = (int)j.ids.size();
    auto tin = worker.tensor(DType::F64, {(uint64_t)n, 2});
    if (!tin.ok()) return error_answer("tsp_exact", "input tensor: " + tin.error().str());
    auto tord = worker.tensor(DType::I32, {(uint64_t)n});
    if (!tord.ok()) { (void)worker.release(tin->memory); return error_answer("tsp_exact", "order tensor: " + tord.error().str()); }
    auto tmeta = worker.tensor(DType::F64, {4});
    if (!tmeta.ok()) { (void)worker.release(tin->memory); (void)worker.release(tord->memory); return error_answer("tsp_exact", "meta tensor: " + tmeta.error().str()); }
    auto release_all = [&] { (void)worker.release(tin->memory); (void)worker.release(tord->memory); (void)worker.release(tmeta->memory); };

    auto wr = worker.write(tin->memory, tin->offset, ByteSpan(reinterpret_cast<const uint8_t*>(j.latlng.data()), j.latlng.size() * sizeof(double)));
    if (!wr.ok()) { release_all(); return error_answer("tsp_exact", "write: " + wr.error().str()); }

    auto g = worker.create_graph();
    if (!g.ok()) { release_all(); return error_answer("tsp_exact", "graph: " + g.error().str()); }
    Operation op;
    op.code = OpCode::Custom;
    op.custom_kernel = fnv1a(std::string(erp::kTspExactKernel));
    op.requirement.device = DeviceType::CPU;
    op.requirement.parallelism = lanes;
    op.scalar = j.prune ? 1.0 : 0.0;
    op.inputs.push_back(*tin);
    op.outputs.push_back(*tord);
    op.outputs.push_back(*tmeta);
    auto added = worker.add_operation(*g, op);
    if (!added.ok()) { release_all(); return error_answer("tsp_exact", "operation: " + added.error().str()); }

    auto rep = worker.execute(*g, SchedulePolicy{});
    (void)k.graphs().destroy(*g);   // one graph per question; the store would otherwise keep every one
    if (!rep.ok()) { release_all(); return error_answer("tsp_exact", "execute: " + rep.error().str()); }
    if (!rep->failure.empty()) { release_all(); return error_answer("tsp_exact", "execute: " + rep->failure); }

    auto ord = worker.read(tord->memory, tord->offset, (uint64_t)n * sizeof(int32_t));
    auto meta = worker.read(tmeta->memory, tmeta->offset, 4 * sizeof(double));
    release_all();
    if (!ord.ok() || !meta.ok()) return error_answer("tsp_exact", "read back failed");

    std::vector<int32_t> order(n);
    std::copy(ord->begin(), ord->end(), reinterpret_cast<uint8_t*>(order.data()));
    double m[4];
    std::copy(meta->begin(), meta->end(), reinterpret_cast<uint8_t*>(m));

    RpcValue r = RpcValue::object();
    r.set("ok", RpcValue::boolean(true));
    r.set("op", RpcValue::text("tsp_exact"));
    r.set("status", RpcValue::text("done"));
    RpcValue ids = RpcValue::list();
    for (int i = 0; i < n; ++i) {
      const int32_t idx = order[(size_t)i];
      ids.push(RpcValue::text(idx >= 0 && idx < n ? j.ids[(size_t)idx] : std::string()));
    }
    r.set("order", ids);
    r.set("n", RpcValue::number((uint64_t)n));
    r.set("distance_miles", RpcValue::number(fmt_fixed(m[0], 6)));
    r.set("evaluated", RpcValue::number((uint64_t)m[1]));
    r.set("pruned", RpcValue::number((uint64_t)m[2]));
    r.set("prune", RpcValue::boolean(j.prune));
    r.set("elapsed_ms", RpcValue::number(fmt_fixed((double)rep->elapsed_ns / 1e6, 3)));
    std::string device = rep->placements.empty() ? std::string("?") : rep->placements[0].device_name;
    uint32_t used = rep->placements.empty() ? lanes : rep->placements[0].lanes;
    r.set("device", RpcValue::text(device));
    r.set("lanes", RpcValue::number((uint64_t)used));
    r.set("cached", RpcValue::boolean(false));
    r.set("charged_ms", RpcValue::number(fmt_fixed((double)rep->charged_ns / 1e6, 3)));

    // The decision goes on the record: which points, which order, how many
    // miles, searched how hard, by which entity — an object in the store with
    // provenance, not a log line that scrolls away. A route someone later asks
    // about can be produced, with the question that made it.
    {
      StoredObject rec;
      rec.timestamp_ns = k.now();
      rec.metadata = {{"kind", "erp.route.exact"}, {"op", "tsp_exact"}, {"n", std::to_string(n)},
                      {"miles", fmt_fixed(m[0], 6)}, {"evaluated", std::to_string((uint64_t)m[1])},
                      {"pruned", std::to_string((uint64_t)m[2])}, {"device", device}, {"question", j.key}};
      RpcValue body = RpcValue::object();
      RpcValue pts = RpcValue::list();
      for (int i = 0; i < n; ++i) {
        RpcValue p = RpcValue::object();
        p.set("id", RpcValue::text(j.ids[(size_t)i]));
        p.set("lat", RpcValue::number(fmt_fixed(j.latlng[2 * (size_t)i], 7)));
        p.set("lng", RpcValue::number(fmt_fixed(j.latlng[2 * (size_t)i + 1], 7)));
        pts.push(p);
      }
      body.set("points", pts);
      body.set("answer", r);
      const std::string text = body.render();
      rec.payload.assign(text.begin(), text.end());
      auto stored = worker.store(std::move(rec));
      if (stored.ok()) r.set("record", RpcValue::text(stored->str()));
      else log("record not stored: " + stored.error().str());
    }

    log("tsp_exact n=" + std::to_string(n) + " prune=" + (j.prune ? "1" : "0") + " -> " + fmt_fixed(m[0], 3) +
        " mi, evaluated=" + std::to_string((uint64_t)m[1]) + " pruned=" + std::to_string((uint64_t)m[2]) +
        " in " + fmt_fixed((double)rep->elapsed_ns / 1e6, 2) + " ms on " + device + " lanes=" + std::to_string(used));
    return r;
  }

  // The sales route builder's optimizer as one graph: stops and the start/end
  // pair in, the visiting order and its miles out.
  RpcValue solve_route_improve(Job& j) {
    const int n = (int)j.ids.size();
    auto tst = worker.tensor(DType::F64, {(uint64_t)n, 2});
    if (!tst.ok()) return error_answer("route_improve", "stops tensor: " + tst.error().str());
    auto tse = worker.tensor(DType::F64, {2, 2});
    if (!tse.ok()) { (void)worker.release(tst->memory); return error_answer("route_improve", "start/end tensor: " + tse.error().str()); }
    auto tord = worker.tensor(DType::I32, {(uint64_t)n});
    auto tmeta = worker.tensor(DType::F64, {4});
    auto release_all = [&] {
      (void)worker.release(tst->memory); (void)worker.release(tse->memory);
      if (tord.ok()) (void)worker.release(tord->memory);
      if (tmeta.ok()) (void)worker.release(tmeta->memory);
    };
    if (!tord.ok() || !tmeta.ok()) { release_all(); return error_answer("route_improve", "output tensors"); }

    double se[4] = {j.start[0], j.start[1], j.end[0], j.end[1]};
    auto w1 = worker.write(tst->memory, tst->offset, ByteSpan(reinterpret_cast<const uint8_t*>(j.latlng.data()), j.latlng.size() * sizeof(double)));
    auto w2 = worker.write(tse->memory, tse->offset, ByteSpan(reinterpret_cast<const uint8_t*>(se), sizeof se));
    if (!w1.ok() || !w2.ok()) { release_all(); return error_answer("route_improve", "write failed"); }

    auto g = worker.create_graph();
    if (!g.ok()) { release_all(); return error_answer("route_improve", "graph: " + g.error().str()); }
    Operation op;
    op.code = OpCode::Custom;
    op.custom_kernel = fnv1a(std::string(erp::kRouteImproveKernel));
    op.requirement.device = DeviceType::CPU;
    op.requirement.parallelism = 1;   // a sequential local search; lanes buy it nothing
    op.scalar = (double)j.end_mode;
    op.inputs.push_back(*tst);
    op.inputs.push_back(*tse);
    op.outputs.push_back(*tord);
    op.outputs.push_back(*tmeta);
    auto added = worker.add_operation(*g, op);
    if (!added.ok()) { release_all(); return error_answer("route_improve", "operation: " + added.error().str()); }
    auto rep = worker.execute(*g, SchedulePolicy{});
    (void)k.graphs().destroy(*g);   // one graph per question; the store would otherwise keep every one
    if (!rep.ok()) { release_all(); return error_answer("route_improve", "execute: " + rep.error().str()); }
    if (!rep->failure.empty()) { release_all(); return error_answer("route_improve", "execute: " + rep->failure); }

    auto ord = worker.read(tord->memory, tord->offset, (uint64_t)n * sizeof(int32_t));
    auto meta = worker.read(tmeta->memory, tmeta->offset, 4 * sizeof(double));
    release_all();
    if (!ord.ok() || !meta.ok()) return error_answer("route_improve", "read back failed");
    std::vector<int32_t> order(n);
    std::copy(ord->begin(), ord->end(), reinterpret_cast<uint8_t*>(order.data()));
    double m[4];
    std::copy(meta->begin(), meta->end(), reinterpret_cast<uint8_t*>(m));

    RpcValue r = RpcValue::object();
    r.set("ok", RpcValue::boolean(true));
    r.set("op", RpcValue::text("route_improve"));
    r.set("status", RpcValue::text("done"));
    RpcValue ids = RpcValue::list();
    for (int i = 0; i < n; ++i) { const int32_t idx = order[(size_t)i]; ids.push(RpcValue::text(idx >= 0 && idx < n ? j.ids[(size_t)idx] : std::string())); }
    r.set("order", ids);
    r.set("n", RpcValue::number((uint64_t)n));
    r.set("total_distance_miles", RpcValue::number(fmt_fixed(m[0], 6)));
    r.set("evaluated", RpcValue::number((uint64_t)m[1]));
    r.set("passes", RpcValue::number((uint64_t)m[2]));
    r.set("end_mode", RpcValue::text(j.end_mode == 1 ? "last" : (j.end_mode == 2 ? "custom" : "home")));
    r.set("elapsed_ms", RpcValue::number(fmt_fixed((double)rep->elapsed_ns / 1e6, 3)));
    std::string device = rep->placements.empty() ? std::string("?") : rep->placements[0].device_name;
    r.set("device", RpcValue::text(device));
    r.set("lanes", RpcValue::number((uint64_t)1));
    r.set("cached", RpcValue::boolean(false));
    r.set("charged_ms", RpcValue::number(fmt_fixed((double)rep->charged_ns / 1e6, 3)));
    log("route_improve n=" + std::to_string(n) + " end=" + r["end_mode"].as_text() + " -> " + fmt_fixed(m[0], 3) + " mi, evaluated=" +
        std::to_string((uint64_t)m[1]) + " passes=" + std::to_string((uint64_t)m[2]) + " in " + fmt_fixed((double)rep->elapsed_ns / 1e6, 2) + " ms on " + device);
    return r;
  }

  // The whole open-order pool in the board's own order: depot + N stops in,
  // visiting order and each leg out.
  RpcValue solve_pool_greedy(Job& j) {
    const int n = (int)j.ids.size();
    auto tin = worker.tensor(DType::F64, {(uint64_t)n, 2});
    if (!tin.ok()) return error_answer("pool_greedy", "input tensor: " + tin.error().str());
    auto tord = worker.tensor(DType::I32, {(uint64_t)std::max(1, n - 1)});
    auto tleg = worker.tensor(DType::F64, {(uint64_t)std::max(1, n - 1)});
    auto tmeta = worker.tensor(DType::F64, {4});
    auto release_all = [&] {
      (void)worker.release(tin->memory);
      if (tord.ok()) (void)worker.release(tord->memory);
      if (tleg.ok()) (void)worker.release(tleg->memory);
      if (tmeta.ok()) (void)worker.release(tmeta->memory);
    };
    if (!tord.ok() || !tleg.ok() || !tmeta.ok()) { release_all(); return error_answer("pool_greedy", "output tensors"); }
    auto wr = worker.write(tin->memory, tin->offset, ByteSpan(reinterpret_cast<const uint8_t*>(j.latlng.data()), j.latlng.size() * sizeof(double)));
    if (!wr.ok()) { release_all(); return error_answer("pool_greedy", "write: " + wr.error().str()); }
    auto g = worker.create_graph();
    if (!g.ok()) { release_all(); return error_answer("pool_greedy", "graph: " + g.error().str()); }
    Operation op;
    op.code = OpCode::Custom;
    op.custom_kernel = fnv1a(std::string(erp::kPoolGreedyKernel));
    op.requirement.device = DeviceType::CPU;
    op.requirement.parallelism = 1;
    op.inputs.push_back(*tin);
    op.outputs.push_back(*tord); op.outputs.push_back(*tleg); op.outputs.push_back(*tmeta);
    auto added = worker.add_operation(*g, op);
    if (!added.ok()) { release_all(); return error_answer("pool_greedy", "operation: " + added.error().str()); }
    auto rep = worker.execute(*g, SchedulePolicy{});
    (void)k.graphs().destroy(*g);   // one graph per question; the store would otherwise keep every one
    if (!rep.ok()) { release_all(); return error_answer("pool_greedy", "execute: " + rep.error().str()); }
    if (!rep->failure.empty()) { release_all(); return error_answer("pool_greedy", "execute: " + rep->failure); }
    const int stops = n - 1;
    auto ord = worker.read(tord->memory, tord->offset, (uint64_t)std::max(1, stops) * sizeof(int32_t));
    auto leg = worker.read(tleg->memory, tleg->offset, (uint64_t)std::max(1, stops) * sizeof(double));
    auto meta = worker.read(tmeta->memory, tmeta->offset, 4 * sizeof(double));
    release_all();
    if (!ord.ok() || !leg.ok() || !meta.ok()) return error_answer("pool_greedy", "read back failed");
    std::vector<int32_t> order((size_t)std::max(1, stops));
    std::vector<double> legs((size_t)std::max(1, stops));
    std::copy(ord->begin(), ord->end(), reinterpret_cast<uint8_t*>(order.data()));
    std::copy(leg->begin(), leg->end(), reinterpret_cast<uint8_t*>(legs.data()));
    double m[4];
    std::copy(meta->begin(), meta->end(), reinterpret_cast<uint8_t*>(m));

    RpcValue r = RpcValue::object();
    r.set("ok", RpcValue::boolean(true));
    r.set("op", RpcValue::text("pool_greedy"));
    r.set("status", RpcValue::text("done"));
    RpcValue ids = RpcValue::list(), lg = RpcValue::list();
    for (int i = 0; i < stops; ++i) {
      const int32_t idx = order[(size_t)i];
      ids.push(RpcValue::text(idx >= 1 && idx < n ? j.ids[(size_t)idx] : std::string()));
      lg.push(RpcValue::number(fmt_fixed(legs[(size_t)i], 9)));
    }
    r.set("order", ids);
    r.set("legs", lg);
    r.set("n", RpcValue::number((uint64_t)stops));
    r.set("total_miles", RpcValue::number(fmt_fixed(m[0], 6)));
    r.set("elapsed_ms", RpcValue::number(fmt_fixed((double)rep->elapsed_ns / 1e6, 3)));
    std::string device = rep->placements.empty() ? std::string("?") : rep->placements[0].device_name;
    r.set("device", RpcValue::text(device));
    r.set("cached", RpcValue::boolean(false));
    r.set("charged_ms", RpcValue::number(fmt_fixed((double)rep->charged_ns / 1e6, 3)));
    log("pool_greedy n=" + std::to_string(stops) + " -> " + fmt_fixed(m[0], 3) + " mi in " + fmt_fixed((double)rep->elapsed_ns / 1e6, 2) + " ms on " + device);
    return r;
  }

  void executor_loop() {
    while (!stopping.load()) {
      std::shared_ptr<Job> j;
      {
        // Woken by a job or by stop; the bound is only for a notify that
        // raced the wait. Polling every millisecond cost a thousand wake-ups
        // a second with nothing to do.
        std::unique_lock<hal::Mutex> g(queue_mu);
        if (queue.empty()) queue_cv.wait_for_ms(g, 50);
        if (!queue.empty()) { j = queue.front(); queue.pop_front(); }
      }
      if (!j) continue;
      j->answer = j->op == "route_improve" ? solve_route_improve(*j) : (j->op == "pool_greedy" ? solve_pool_greedy(*j) : solve(*j));
      if (j->answer.has("ok") && j->answer["ok"].truthy()) ++solved; else ++errors;
      j->done.store(true);
    }
  }

  // The answer a job holds, marked as coming from the cache.
  static RpcValue cached_copy(const RpcValue& a) {
    RpcValue r = RpcValue::object();
    for (auto& f : a.fields()) r.set(f.first, f.second);
    r.set("cached", RpcValue::boolean(true));
    return r;
  }

  RpcValue ping() const {
    RpcValue r = RpcValue::object();
    r.set("ok", RpcValue::boolean(true));
    r.set("op", RpcValue::text("ping"));
    r.set("kernel", RpcValue::text(k.self().short_str()));
    RpcValue devs = RpcValue::list();
    // The devices registered so far: a liveness check must not be the call that opens the GPU runtime (on a CUDA
    // machine that compiles kernels for seconds, and the first ping timed out). A job that needs one loads them.
    for (auto* d : k.devices().all_loaded()) {
      RpcValue dv = RpcValue::object();
      dv.set("name", RpcValue::text(d->name()));
      dv.set("type", RpcValue::text(device_type_name(d->type())));
      dv.set("lanes", RpcValue::number((uint64_t)d->capacity().lanes));
      devs.push(dv);
    }
    r.set("devices", devs);
    r.set("lanes", RpcValue::number((uint64_t)lanes));
    RpcValue ops = RpcValue::list();
    ops.push(RpcValue::text("ping")); ops.push(RpcValue::text("stats")); ops.push(RpcValue::text("tsp_exact")); ops.push(RpcValue::text("route_improve")); ops.push(RpcValue::text("pool_greedy"));
    if (sql_db) ops.push(RpcValue::text("sql"));
    r.set("ops", ops);
    r.set("max_exact_points", RpcValue::number((uint64_t)o.max_exact_points));
    return r;
  }

  RpcValue stats() {
    RpcValue r = RpcValue::object();
    r.set("ok", RpcValue::boolean(true));
    r.set("op", RpcValue::text("stats"));
    r.set("requests", RpcValue::number(requests.load()));
    r.set("solved", RpcValue::number(solved.load()));
    r.set("served_cached", RpcValue::number(served_cached.load()));
    r.set("pending", RpcValue::number(pending.load()));
    r.set("errors", RpcValue::number(errors.load()));
    size_t cached; { hal::LockGuard g(cache_mu); cached = cache.size(); }
    r.set("cache_entries", RpcValue::number((uint64_t)cached));
    r.set("graphs_held", RpcValue::number((uint64_t)k.graphs().list().size()));
    r.set("entity", RpcValue::text(worker.self().short_str()));
    return r;
  }

  // Parse a tsp_exact (or pool_greedy) request into a job, or explain what is wrong with it.
  Result<std::shared_ptr<Job>> parse_tsp(const RpcValue& req, bool pool = false) {
    const char* opname = pool ? "pool_greedy" : "tsp_exact";
    if (!req.has("start") || !req.has("stops")) return Result<std::shared_ptr<Job>>(Status::Invalid, std::string(opname) + " needs start and stops");
    const RpcValue& start = req["start"];
    const RpcValue& stops = req["stops"];
    if (stops.kind() != RpcValue::Kind::List) return Result<std::shared_ptr<Job>>(Status::Invalid, "stops must be a list");
    const size_t n = 1 + stops.size();
    if (n < 2) return Result<std::shared_ptr<Job>>(Status::Invalid, "nothing to route: no stops");
    if (pool) {
      if ((int)n > erp::kPoolGreedyMaxPoints) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many points for the pool");
    } else {
      if ((int)n > o.max_exact_points) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many points for exact search: " + std::to_string(n) + " > " + std::to_string(o.max_exact_points));
      if ((int)n > erp::kTspExactMaxPoints) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many points to exhaust");
    }

    auto j = std::make_shared<Job>();
    j->op = opname;
    j->prune = req.has("prune") ? req["prune"].truthy() : true;
    auto take = [&](const RpcValue& p, const char* what) -> Result<void> {
      if (p.kind() != RpcValue::Kind::Object || !p.has("lat") || !p.has("lng")) return Result<void>(Status::Invalid, std::string(what) + " needs lat and lng");
      const double lat = p["lat"].as_double(1e9), lng = p["lng"].as_double(1e9);
      if (!(lat >= -90 && lat <= 90) || !(lng >= -180 && lng <= 180)) return Result<void>(Status::Invalid, std::string(what) + " has coordinates off the map");
      j->ids.push_back(p.has("id") ? p["id"].as_text() : std::to_string(j->ids.size()));
      j->latlng.push_back(lat); j->latlng.push_back(lng);
      return Ok();
    };
    AIOS_TRY(take(start, "start"));
    for (size_t i = 0; i < stops.size(); ++i) AIOS_TRY(take(stops[i], "stop"));
    key_tsp_job(*j, pool);
    return j;
  }

  // The cache key is the question. For the exact search it is independent
  // of the order the stops were listed in — the same points from the same
  // start have the same shortest path. The pool's greedy walk breaks ties by
  // listing order, so there the order is part of the question. Both the JSON
  // and the binary request forms key through here, so the same points from
  // either form hit the same cached answer.
  static void key_tsp_job(Job& j, bool pool) {
    const size_t n = j.ids.size();
    std::vector<std::string> parts;
    for (size_t i = 1; i < n; ++i) parts.push_back(j.ids[i] + "@" + fmt_fixed(j.latlng[2 * i], 7) + "," + fmt_fixed(j.latlng[2 * i + 1], 7));
    if (!pool) std::sort(parts.begin(), parts.end());
    std::string key = j.op + "|" + (j.prune ? "p|" : "f|") + j.ids[0] + "@" + fmt_fixed(j.latlng[0], 7) + "," + fmt_fixed(j.latlng[1], 7);
    for (auto& p : parts) { key += "|"; key += p; }
    j.key = key;
  }

  // The binary request form (compute_wire.hpp): the frame is read in place,
  // no document is parsed. Validation is the same as parse_tsp's.
  Result<std::shared_ptr<Job>> parse_tsp_binary(const void* payload, size_t len) {
    using namespace wire;
    if (len < sizeof(TspRequestHeader)) return Result<std::shared_ptr<Job>>(Status::Invalid, "short binary request");
    TspRequestHeader h;
    std::memcpy(&h, payload, sizeof h);
    if (h.magic != kTspRequestMagic || h.version != kVersion) return Result<std::shared_ptr<Job>>(Status::Invalid, "not a binary tsp request");
    const bool pool = h.op == PoolGreedy;
    if (!pool && h.op != TspExact) return Result<std::shared_ptr<Job>>(Status::Invalid, "unknown binary op");
    const size_t n = h.n;
    if (n < 2) return Result<std::shared_ptr<Job>>(Status::Invalid, "nothing to route: no stops");
    if (len < tsp_request_bytes(h.n)) return Result<std::shared_ptr<Job>>(Status::Invalid, "binary request shorter than its point count");
    if (pool) {
      if ((int)n > erp::kPoolGreedyMaxPoints) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many points for the pool");
    } else {
      if ((int)n > o.max_exact_points) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many points for exact search: " + std::to_string(n) + " > " + std::to_string(o.max_exact_points));
      if ((int)n > erp::kTspExactMaxPoints) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many points to exhaust");
    }
    auto j = std::make_shared<Job>();
    j->op = pool ? "pool_greedy" : "tsp_exact";
    j->prune = h.prune != 0;
    j->ids.reserve(n); j->latlng.reserve(2 * n);
    const uint8_t* p = static_cast<const uint8_t*>(payload) + sizeof(TspRequestHeader);
    for (size_t i = 0; i < n; ++i, p += sizeof(TspPoint)) {
      TspPoint pt;
      std::memcpy(&pt, p, sizeof pt);
      if (!(pt.lat >= -90 && pt.lat <= 90) || !(pt.lng >= -180 && pt.lng <= 180)) return Result<std::shared_ptr<Job>>(Status::Invalid, std::string(i == 0 ? "start" : "stop") + " has coordinates off the map");
      j->ids.push_back(std::to_string(pt.id));
      j->latlng.push_back(pt.lat); j->latlng.push_back(pt.lng);
    }
    key_tsp_job(*j, pool);
    return j;
  }

  // A finished answer in its binary form (compute_wire.hpp), straight from the
  // job's fields — no text is rendered.
  static std::string render_tsp_binary(const RpcValue& ans, bool cached) {
    using namespace wire;
    TspResponseHeader h{};
    h.magic = kTspResponseMagic; h.version = kVersion;
    const std::string status = ans.has("status") ? ans["status"].as_text() : std::string();
    const bool ok = ans.has("ok") && ans["ok"].truthy();
    h.status = !ok ? Error : (status == "done" ? Done : Pending);
    h.cached = cached ? 1 : 0;
    std::vector<int64_t> order;
    if (h.status == Done) {
      h.distance_miles = ans.has("distance_miles") ? ans["distance_miles"].as_double() : 0.0;
      h.evaluated = ans.has("evaluated") ? ans["evaluated"].as_u64() : 0;
      h.pruned = ans.has("pruned") ? ans["pruned"].as_u64() : 0;
      h.elapsed_ms = ans.has("elapsed_ms") ? ans["elapsed_ms"].as_double() : 0.0;
      h.lanes = ans.has("lanes") ? (uint32_t)ans["lanes"].as_u64() : 0;
      if (ans.has("device")) std::strncpy(h.device, ans["device"].as_text().c_str(), sizeof h.device - 1);
      if (ans.has("order")) for (const RpcValue& v : ans["order"].items()) order.push_back(aios::to_i64(v.as_text().c_str()));
    } else if (h.status == Error && ans.has("error")) {
      std::strncpy(h.error, ans["error"].as_text().c_str(), sizeof h.error - 1);
    }
    h.n = (uint32_t)order.size();
    std::string out(tsp_response_bytes(h.n), '\0');
    std::memcpy(&out[0], &h, sizeof h);
    if (!order.empty()) std::memcpy(&out[sizeof h], order.data(), order.size() * sizeof(int64_t));
    return out;
  }

  // The "pending" promise in binary form: the deadline passed, the work goes on.
  static std::string render_tsp_pending() {
    using namespace wire;
    TspResponseHeader h{};
    h.magic = kTspResponseMagic; h.version = kVersion; h.status = Pending;
    return std::string(reinterpret_cast<const char*>(&h), sizeof h);
  }

  // Parse a route_improve request: stops, a start, and how the day ends.
  Result<std::shared_ptr<Job>> parse_route_improve(const RpcValue& req) {
    if (!req.has("start") || !req.has("stops")) return Result<std::shared_ptr<Job>>(Status::Invalid, "route_improve needs start and stops");
    const RpcValue& stops = req["stops"];
    if (stops.kind() != RpcValue::Kind::List) return Result<std::shared_ptr<Job>>(Status::Invalid, "stops must be a list");
    if (stops.size() > 500) return Result<std::shared_ptr<Job>>(Status::Invalid, "too many stops");
    auto j = std::make_shared<Job>();
    j->op = "route_improve";
    auto point = [&](const RpcValue& p, const char* what, double* out) -> Result<void> {
      if (p.kind() != RpcValue::Kind::Object || !p.has("lat") || !p.has("lng")) return Result<void>(Status::Invalid, std::string(what) + " needs lat and lng");
      const double lat = p["lat"].as_double(1e9), lng = p["lng"].as_double(1e9);
      if (!(lat >= -90 && lat <= 90) || !(lng >= -180 && lng <= 180)) return Result<void>(Status::Invalid, std::string(what) + " has coordinates off the map");
      out[0] = lat; out[1] = lng;
      return Ok();
    };
    AIOS_TRY(point(req["start"], "start", j->start));
    const std::string mode = req.has("end_mode") ? req["end_mode"].as_text() : std::string("home");
    if (mode == "last") j->end_mode = 1;
    else if (mode == "custom") {
      if (!req.has("end") || req["end"].kind() != RpcValue::Kind::Object) return Result<std::shared_ptr<Job>>(Status::Invalid, "end_mode custom needs end");
      AIOS_TRY(point(req["end"], "end", j->end));
      j->end_mode = 2;
    } else j->end_mode = 0;
    for (size_t i = 0; i < stops.size(); ++i) {
      double ll[2];
      AIOS_TRY(point(stops[i], "stop", ll));
      j->ids.push_back(stops[i].has("id") ? stops[i]["id"].as_text() : std::to_string(i));
      j->latlng.push_back(ll[0]); j->latlng.push_back(ll[1]);
    }
    // The stops' order is part of the question here: the seed walks them as given.
    std::string key = "route_improve|" + mode + "|" + fmt_fixed(j->start[0], 7) + "," + fmt_fixed(j->start[1], 7) + "|" + fmt_fixed(j->end[0], 7) + "," + fmt_fixed(j->end[1], 7);
    for (size_t i = 0; i < j->ids.size(); ++i) key += "|" + j->ids[i] + "@" + fmt_fixed(j->latlng[2 * i], 7) + "," + fmt_fixed(j->latlng[2 * i + 1], 7);
    j->key = key;
    return j;
  }

  // Hand the caller a job for this question: the one already answered or in
  // flight, or a fresh one queued for the executor.
  std::shared_ptr<Job> admit(std::shared_ptr<Job> fresh, bool& was_cached) {
    hal::LockGuard g(cache_mu);
    auto it = cache.find(fresh->key);
    if (it != cache.end()) { was_cached = true; return it->second; }
    was_cached = false;
    cache[fresh->key] = fresh;
    cache_order.push_back(fresh->key);
    while (cache.size() > o.cache_entries && !cache_order.empty()) {
      auto victim = cache.find(cache_order.front());
      if (victim != cache.end() && victim->second->done.load()) cache.erase(victim);
      cache_order.pop_front();
    }
    { hal::LockGuard q(queue_mu); queue.push_back(fresh); }
    queue_cv.notify_all();
    return fresh;
  }
};

}  // namespace

Result<void> compute_serve(Kernel& k, Surface& worker, const ComputeServeOptions& o,
                           std::function<void(const std::string&)> log,
                           std::atomic<bool>* stop, userland::SqlDatabase* sql_db) {
  uint32_t lanes = o.lanes;
  if (lanes == 0) {
    for (auto* d : k.devices().of_type(DeviceType::CPU)) lanes = std::max(lanes, d->capacity().lanes);
    if (lanes == 0) lanes = 1;
  }
  auto listener = hal::net_listen(o.port, o.loopback_only);
  if (!listener.ok()) return listener.error();

  Service svc(k, worker, o, log, lanes, sql_db);
  hal::Thread executor([&] { svc.executor_loop(); });
  std::deque<std::unique_ptr<Waiter>> waiters;

  log(std::string("compute service listening on ") + (o.loopback_only ? "127.0.0.1" : "0.0.0.0") + ":" +
      std::to_string((*listener)->port()) + " lanes=" + std::to_string(lanes) + " max_points=" + std::to_string(o.max_exact_points) +
      (o.token.empty() ? "" : " token=required"));

  std::unique_ptr<hal::Thread> shm_worker;
  std::string shm_ring_req = o.shm_ring_name.empty() ? "" : o.shm_ring_name;
  std::string shm_ring_resp = shm_ring_req.empty() ? "" : (shm_ring_req + "_resp");
  std::unique_ptr<sidecar::ShmRingChannel> req_chan;
  std::unique_ptr<sidecar::ShmRingChannel> resp_chan;

  if (!shm_ring_req.empty()) {
    req_chan = std::make_unique<sidecar::ShmRingChannel>(shm_ring_req, 4 * 1024 * 1024, true);
    resp_chan = std::make_unique<sidecar::ShmRingChannel>(shm_ring_resp, 4 * 1024 * 1024, true);
    log("compute service SHM event fabric attached on /dev/shm/" + shm_ring_req);

    shm_worker = std::make_unique<hal::Thread>([&] {
      constexpr uint32_t kHotPolls = 50000;   // ~1 ms of empty polls before sleeping
      uint32_t idle_polls = kHotPolls;        // start asleep, not spinning
      while (!(stop && stop->load()) && !svc.stopping.load()) {
        bool popped = req_chan->pop_frame([&](const sidecar::ShmFrameHeader& hdr, const void* payload_ptr, size_t len) {
          if (hdr.msg_type == wire::kTspBinary) {
            // Fixed-width request, read in place; fixed-width answer back.
            auto job = svc.parse_tsp_binary(payload_ptr, len);
            std::string out;
            if (job.ok()) {
              bool was_cached = false;
              std::shared_ptr<Job> live = svc.admit(*job, was_cached);
              while (!live->done.load() && !svc.stopping.load()) hal::sleep_ms(1);
              out = Service::render_tsp_binary(live->answer, was_cached);
            } else {
              out = Service::render_tsp_binary(error_answer("tsp_exact", job.error().message), false);
            }
            resp_chan->push_frame(hdr.msg_type, hdr.stream_id, out.data(), out.size());
            return;
          }
          std::string body(reinterpret_cast<const char*>(payload_ptr), len);
          auto parsed = rpc_parse(body);
          if (parsed.ok() && parsed->kind() == RpcValue::Kind::Object) {
            const RpcValue& req = *parsed;
            const std::string op = req.has("op") ? req["op"].as_text() : std::string();
            RpcValue ans = RpcValue::object();
            if (op == "ping") {
              ans = svc.ping();
            } else if (op == "stats") {
              ans = svc.stats();
            } else if (op == "sql") {
              const std::string query = req.has("query") ? req["query"].as_text() : std::string();
              if (!query.empty()) ans = svc.run_sql(query, req.has("db") ? req["db"].as_text() : std::string());
              else ans = error_answer(op, "sql needs query");
            } else if (op == "tsp_exact" || op == "route_improve" || op == "pool_greedy") {
              auto job = op == "route_improve" ? svc.parse_route_improve(req) : svc.parse_tsp(req, op == "pool_greedy");
              if (job.ok()) {
                bool was_cached = false;
                std::shared_ptr<Job> live = svc.admit(*job, was_cached);
                while (!live->done.load() && !svc.stopping.load()) hal::sleep_ms(1);
                ans = live->answer;
              } else {
                ans = error_answer(op, job.error().message);
              }
            } else {
              ans = error_answer(op, "unknown op");
            }
            std::string ans_str = ans.render();
            resp_chan->push_frame(hdr.msg_type, hdr.stream_id, ans_str.data(), ans_str.size());
          }
        });
        // A frame that lands while this thread sleeps waits out the whole
        // sleep: measured at ~1.1 ms a round-trip, two orders over the
        // encoding it was blamed on. So after work, poll hot for a while
        // (a client in conversation answers within microseconds) and only
        // back off to the 1 ms sleep once the ring has stayed empty.
        if (popped) {
          idle_polls = 0;
        } else if (++idle_polls < kHotPolls) {
          // spin: each poll is two atomic loads
        } else {
          hal::sleep_ms(1);
        }
      }
    });
  }

  while (!(stop && stop->load())) {
    // Waiters that have delivered their answer are joined here, on the thread that made them.
    for (auto it = waiters.begin(); it != waiters.end();) {
      if ((*it)->finished.load()) { if ((*it)->thread.joinable()) (*it)->thread.join(); it = waiters.erase(it); }
      else ++it;
    }

    auto conn = (*listener)->accept(200);
    if (!conn.ok()) { if (conn.error().status == Status::Busy) continue; log("accept: " + conn.error().str()); hal::sleep_ms(50); continue; }
    std::shared_ptr<hal::Connection> c(std::move(*conn));
    ++svc.requests;

    std::string body;
    if (!read_frame(*c, body, o.frame_timeout_ms)) { ++svc.errors; c->close(); continue; }

    // The binary form (compute_wire.hpp): header, points, optional token.
    // Read in place; the answer goes back as a struct.
    if (wire::looks_binary(body.data(), body.size())) {
      wire::TspRequestHeader bh;
      std::memcpy(&bh, body.data(), sizeof bh);
      const size_t point_bytes = wire::tsp_request_bytes(bh.n);
      std::string token;
      if (body.size() >= point_bytes + 2) {
        uint16_t tl; std::memcpy(&tl, body.data() + point_bytes, 2);
        if (body.size() >= point_bytes + 2 + tl) token.assign(body.data() + point_bytes + 2, tl);
      }
      if (!o.token.empty() && token != o.token) {
        ++svc.errors; write_frame(*c, Service::render_tsp_binary(error_answer("tsp_exact", "unauthorized"), false)); c->close(); continue;
      }
      auto job = svc.parse_tsp_binary(body.data(), std::min(body.size(), point_bytes));
      if (!job.ok()) { ++svc.errors; write_frame(*c, Service::render_tsp_binary(error_answer("tsp_exact", job.error().message), false)); c->close(); continue; }
      int deadline_ms = bh.deadline_ms ? (int)bh.deadline_ms : 2000;
      deadline_ms = std::max(50, std::min(deadline_ms, 60000));
      bool was_cached = false;
      std::shared_ptr<Job> live = svc.admit(*job, was_cached);
      if (was_cached && live->done.load()) {
        ++svc.served_cached;
        write_frame(*c, Service::render_tsp_binary(live->answer, true));
        c->close();
        continue;
      }
      auto w = std::make_unique<Waiter>();
      Waiter* wp = w.get();
      const uint64_t due = k.now() + (uint64_t)deadline_ms * 1000000ull;
      w->thread = hal::Thread([&svc, c, live, wp, due, was_cached, &k] {
        while (!live->done.load() && k.now() < due) hal::sleep_ms(1);
        if (live->done.load()) {
          if (was_cached) ++svc.served_cached;
          write_frame(*c, Service::render_tsp_binary(live->answer, was_cached));
        } else {
          ++svc.pending;
          write_frame(*c, Service::render_tsp_pending());
        }
        c->close();
        wp->finished.store(true);
      });
      waiters.push_back(std::move(w));
      continue;
    }

    auto parsed = rpc_parse(body);
    if (!parsed.ok() || parsed->kind() != RpcValue::Kind::Object) { ++svc.errors; write_frame(*c, error_answer("", "request is not a JSON object").render()); c->close(); continue; }
    const RpcValue& req = *parsed;
    const std::string op = req.has("op") ? req["op"].as_text() : std::string();

    if (!o.token.empty() && (!req.has("token") || req["token"].as_text() != o.token)) {
      ++svc.errors; write_frame(*c, error_answer(op, "unauthorized").render()); c->close(); continue;
    }

    if (op == "ping") { write_frame(*c, svc.ping().render()); c->close(); continue; }
    if (op == "stats") { write_frame(*c, svc.stats().render()); c->close(); continue; }
    if (op == "sql") {
      const std::string query = req.has("query") ? req["query"].as_text() : std::string();
      if (query.empty()) { ++svc.errors; write_frame(*c, error_answer(op, "sql needs query").render()); c->close(); continue; }
      RpcValue ans = svc.run_sql(query, req.has("db") ? req["db"].as_text() : std::string());
      if (ans.has("ok") && ans["ok"].truthy()) ++svc.solved; else ++svc.errors;
      write_frame(*c, ans.render());
      c->close();
      continue;
    }
    // A caller that tried ASQL, was refused, and re-ran the statement against
    // MariaDB is the only party that knows the pairing happened — the refusal
    // and the retry land in two different servers, neither of which sees the
    // other. This op lets it hand that fact back so one log holds both halves.
    if (op == "trace") {
      if (!svc.sql_db) { ++svc.errors; write_frame(*c, error_answer(op, "no database attached").render()); c->close(); continue; }
      const std::string query = req.has("query") ? req["query"].as_text() : std::string();
      if (query.empty()) { ++svc.errors; write_frame(*c, error_answer(op, "trace needs query").render()); c->close(); continue; }
      const std::string reason = req.has("reason") ? req["reason"].as_text() : std::string();
      const std::string origin = req.has("origin") ? req["origin"].as_text() : std::string("erp");
      const double ms = req.has("duration_ms") ? req["duration_ms"].as_double(0.0) : 0.0;
      const uint64_t rows = req.has("rows") ? (uint64_t)req["rows"].as_double(0.0) : 0;
      {
        std::unique_lock<aios::hal::SharedMutex> g(svc.sql_rw_mu);
        userland::SqlDatabase::set_client_tag(origin + "-bounce");
        svc.sql_db->record_query_stat(query, (uint64_t)(ms * 1000.0), rows, false,
                                      reason.empty() ? "bounced to MariaDB" : reason,
                                      "BOUNCED_TO_MYSQL");
      }
      RpcValue r = RpcValue::object();
      r.set("ok", RpcValue::boolean(true));
      r.set("op", RpcValue::text("trace"));
      write_frame(*c, r.render());
      c->close();
      continue;
    }
    if (op != "tsp_exact" && op != "route_improve" && op != "pool_greedy") { ++svc.errors; write_frame(*c, error_answer(op, "unknown op").render()); c->close(); continue; }

    auto job = op == "route_improve" ? svc.parse_route_improve(req) : svc.parse_tsp(req, op == "pool_greedy");
    if (!job.ok()) { ++svc.errors; write_frame(*c, error_answer(op, job.error().message).render()); c->close(); continue; }
    int deadline_ms = req.has("deadline_ms") ? (int)req["deadline_ms"].as_double(2000) : 2000;
    deadline_ms = std::max(50, std::min(deadline_ms, 60000));

    bool was_cached = false;
    std::shared_ptr<Job> live = svc.admit(*job, was_cached);
    if (was_cached && live->done.load()) {
      ++svc.served_cached;
      write_frame(*c, Service::cached_copy(live->answer).render());
      c->close();
      continue;
    }

    // Wait for the executor up to the deadline, then answer or promise.
    auto w = std::make_unique<Waiter>();
    Waiter* wp = w.get();
    const uint64_t due = k.now() + (uint64_t)deadline_ms * 1000000ull;
    w->thread = hal::Thread([&svc, c, live, wp, due, was_cached, &k] {
      while (!live->done.load() && k.now() < due) hal::sleep_ms(1);
      if (live->done.load()) {
        if (was_cached) { ++svc.served_cached; write_frame(*c, Service::cached_copy(live->answer).render()); }
        else write_frame(*c, live->answer.render());
      } else {
        ++svc.pending;
        RpcValue r = RpcValue::object();
        r.set("ok", RpcValue::boolean(true));
        r.set("op", RpcValue::text(live->op));
        r.set("status", RpcValue::text("pending"));
        r.set("n", RpcValue::number((uint64_t)live->ids.size()));
        write_frame(*c, r.render());
      }
      c->close();
      wp->finished.store(true);
    });
    waiters.push_back(std::move(w));
  }

  svc.stopping.store(true);
  svc.queue_cv.notify_all();
  if (shm_worker && shm_worker->joinable()) shm_worker->join();
  if (executor.joinable()) executor.join();
  for (auto& w : waiters) if (w->thread.joinable()) w->thread.join();
  (*listener)->close();
  return Ok();
}

}  // namespace aios::channel
