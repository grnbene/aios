// `sat` - the solver as work the substrate places and the fleet shares.
//
// A fleet run is cube-and-conquer. The formula is split on its most frequent
// variables into cubes (partial assignments that together cover every case);
// each named device gets a worker - an entity of its own, spawned under the
// console, holding exactly Schedule and Execute on its one device - that takes
// the next cube and runs sat.solve on it with a conflict budget. A cube that
// runs out of budget is split in two and goes back on the queue.
//
// Nothing is taken on a worker's word:
//   - a model is checked here against the formula before SAT is reported;
//   - an UNSAT cube's DRAT proof is checked by sat.check on a DIFFERENT
//     machine than the one that produced it (when there is one), and the
//     formula is UNSAT only when every leaf of the split tree is, each with a
//     verified proof. The tree covers every assignment by construction (each
//     split adds v and -v), so the leaves together are the whole formula.
#include "sat_cmd.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "aios/compute/sat_kernels.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/machine_sql.hpp"
#include "aios/userland/sat_solver.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios::cli {
namespace {

std::string kv(const std::vector<std::string>& a, const std::string& key, const std::string& def = "") {
  for (const auto& s : a)
    if (s.rfind(key + "=", 0) == 0) return s.substr(key.size() + 1);
  return def;
}
std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) { if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Operation sat_op(const char* kernel, const std::string& selector) {
  Operation op;
  op.code = OpCode::Custom;
  op.custom_kernel = fnv1a(std::string(kernel));
  op.requirement.device = DeviceType::Any;
  op.requirement.device_selector = selector;
  op.requirement.parallelism = 1;
  return op;
}

bool device_runs(Device* d, const char* kernel) {
  Operation op;
  op.code = OpCode::Custom;
  op.custom_kernel = fnv1a(std::string(kernel));
  return d->supports(op);
}

struct CubeResult {
  int status = 0;   // 10 SAT, 20 UNSAT, 0 unknown
  bool proof_left = false;   // the proof stayed on the device that made it (not read back)
  uint64_t proof_len = 0;
  double conflicts = 0, decisions = 0, propagations = 0, ms = 0;
  std::vector<int32_t> model;
  std::string proof;
  bool truncated = false;
  std::string device, error;
};

// One worker: an entity of its own, its tensors, its device.
struct Worker {
  std::string selector;
  uint64_t budget = 0;       // conflicts a cube; 0 = the run's
  bool checks_proofs = true; // false ("!"): solve only - never handed another worker's proof
  EntityID entity;
  std::unique_ptr<Surface> s;
  Tensor formula, cube, model, meta, proof, pin, cmeta;
  uint64_t proof_cap = 0, pin_cap = 0;
  int vars = 0;
  // tallies
  uint64_t cubes = 0, sat = 0, unsat = 0, unknown = 0, checks = 0, checks_failed = 0;
  double busy_ms = 0, conflicts = 0, kernel_ms = 0, check_kernel_ms = 0;
  uint64_t bytes_back = 0;   // proofs / models that came back from its device
  uint64_t solves = 0, in_place_checks = 0;
  std::string device_seen;
};

Result<void> alloc_worker(Kernel& k, Surface& me, Worker& w, const std::vector<int32_t>& flat, int vars, uint64_t proof_cap) {
  auto child = me.spawn("sat worker " + w.selector);
  if (!child.ok()) return child.error();
  w.entity = *child;
  // Exactly what the work needs: schedule graphs, run on this one device.
  auto g1 = me.grant(w.entity, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule);
  if (!g1.ok()) return Result<void>(g1.error().status, "grant schedule: " + g1.error().message);
  // and to leave receipts: what it solved or checked, under its own name
  auto g3 = me.grant(w.entity, ResourceRef{ResourceKind::StorageObject, {}}, R_Write);
  if (!g3.ok()) return Result<void>(g3.error().status, "grant storage: " + g3.error().message);
  size_t granted = 0;
  for (Device* d : k.devices().all()) {
    if (d->name().rfind(w.selector, 0) != 0 && d->id().short_str().rfind(w.selector, 0) != 0) continue;
    if (!device_runs(d, sat::kSolveKernel)) continue;
    auto g2 = me.grant(w.entity, ResourceRef{ResourceKind::ComputeDevice, d->id()}, R_Execute);
    if (!g2.ok()) return Result<void>(g2.error().status, "grant execute on " + d->name() + ": " + g2.error().message);
    ++granted;
  }
  if (!granted) return Result<void>(Status::NotFound, "no device answering to '" + w.selector + "' runs sat.solve");
  w.s = std::make_unique<Surface>(k.as(w.entity));
  w.vars = vars;
  auto t = [&](DType dt, uint64_t n, Tensor* out) -> Result<void> {
    auto r = w.s->tensor(dt, {n});
    if (!r.ok()) return r.error();
    *out = *r;
    return Ok();
  };
  AIOS_TRY(t(DType::I32, flat.size(), &w.formula));
  AIOS_TRY(w.s->write(w.formula.memory, w.formula.offset, ByteSpan(reinterpret_cast<const uint8_t*>(flat.data()), flat.size() * 4)));
  AIOS_TRY(t(DType::I32, (uint64_t)vars + 1, &w.cube));
  AIOS_TRY(t(DType::I32, (uint64_t)vars + 1, &w.model));
  AIOS_TRY(t(DType::F64, 8, &w.meta));
  AIOS_TRY(t(DType::U8, proof_cap, &w.proof));
  w.proof_cap = proof_cap;
  AIOS_TRY(t(DType::F64, 4, &w.cmeta));
  return Ok();
}

Result<void> run_graph(Kernel& k, Surface& s, const Operation& op, std::string* device) {
  auto g = s.create_graph();
  if (!g.ok()) return g.error();
  auto added = s.add_operation(*g, op);
  if (!added.ok()) { (void)k.graphs().destroy(*g); return added.error(); }
  auto rep = s.execute(*g);
  (void)k.graphs().destroy(*g);
  if (!rep.ok()) return rep.error();
  if (!rep->failure.empty()) return Result<void>(Status::IoError, rep->failure);
  if (device && !rep->placements.empty()) *device = rep->placements[0].device_name;
  return Ok();
}

Result<CubeResult> solve_cube(Kernel& k, Worker& w, const std::vector<int>& cube, uint64_t budget, bool leave_proof = false) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::vector<int32_t> c(1 + cube.size());
    c[0] = (int32_t)cube.size();
    for (size_t i = 0; i < cube.size(); ++i) c[i + 1] = cube[i];
    AIOS_TRY(w.s->write(w.cube.memory, w.cube.offset, ByteSpan(reinterpret_cast<const uint8_t*>(c.data()), c.size() * 4)));
    Operation op = sat_op(sat::kSolveKernel, w.selector);
    op.scalar = (double)budget;
    op.inputs = {w.formula, w.cube};
    op.outputs = {w.model, w.meta, w.proof};
    if (leave_proof) op.leave_remote = {0, 0, 1};
    CubeResult r;
    AIOS_TRY(run_graph(k, *w.s, op, &r.device));
    auto meta = w.s->read(w.meta.memory, w.meta.offset, 8 * sizeof(double));
    if (!meta.ok()) return meta.error();
    double m[8];
    std::memcpy(m, meta->data(), sizeof m);
    r.status = (int)m[0];
    r.conflicts = m[1]; r.decisions = m[2]; r.propagations = m[3]; r.ms = m[4];
    r.truncated = m[6] != 0;
    r.proof_len = (uint64_t)m[5];
    r.proof_left = leave_proof;
    if (r.status == 20 && r.truncated) {
      // The proof did not fit. The run is deterministic, so the same cube on
      // the same budget writes the same proof: grow the buffer and redo it.
      (void)w.s->release(w.proof.memory);
      auto bigger = w.s->tensor(DType::U8, {w.proof_cap * 4});
      if (!bigger.ok()) return Result<CubeResult>(bigger.error().status, "proof buffer: " + bigger.error().message);
      w.proof = *bigger;
      w.proof_cap *= 4;
      continue;
    }
    if (r.status == 10) {
      auto md = w.s->read(w.model.memory, w.model.offset, ((uint64_t)w.vars + 1) * 4);
      if (!md.ok()) return md.error();
      r.model.resize((size_t)w.vars + 1);
      std::memcpy(r.model.data(), md->data(), r.model.size() * 4);
    }
    if ((r.status == 20 || r.status == 0) && !leave_proof) {
      const uint64_t n = (uint64_t)m[5];
      if (n) {
        auto pd = w.s->read(w.proof.memory, w.proof.offset, n);
        if (!pd.ok()) return pd.error();
        r.proof.assign(reinterpret_cast<const char*>(pd->data()), pd->size());
      }
    }
    return r;
  }
  return Result<CubeResult>(Status::Unsupported, "the proof kept outgrowing its buffer");
}

// sat.check on this worker's device: is (formula AND cube) refuted by `proof`?
Result<bool> check_cube(Kernel& k, Worker& w, const std::vector<int>& cube, const std::string& proof, double* ms) {
  std::vector<int32_t> c(1 + cube.size());
  c[0] = (int32_t)cube.size();
  for (size_t i = 0; i < cube.size(); ++i) c[i + 1] = cube[i];
  AIOS_TRY(w.s->write(w.cube.memory, w.cube.offset, ByteSpan(reinterpret_cast<const uint8_t*>(c.data()), c.size() * 4)));
  const uint64_t need = std::max<uint64_t>(proof.size(), 1);
  if (need > w.pin_cap) {
    if (w.pin_cap) (void)w.s->release(w.pin.memory);
    auto t = w.s->tensor(DType::U8, {need});
    if (!t.ok()) return t.error();
    w.pin = *t;
    w.pin_cap = need;
  }
  if (!proof.empty()) AIOS_TRY(w.s->write(w.pin.memory, w.pin.offset, ByteSpan(reinterpret_cast<const uint8_t*>(proof.data()), proof.size())));
  Operation op = sat_op(sat::kCheckKernel, w.selector);
  op.scalar = (double)proof.size();
  op.inputs = {w.formula, w.cube, w.pin};
  op.outputs = {w.cmeta};
  std::string dev;
  AIOS_TRY(run_graph(k, *w.s, op, &dev));
  auto meta = w.s->read(w.cmeta.memory, w.cmeta.offset, 4 * sizeof(double));
  if (!meta.ok()) return meta.error();
  double m[4];
  std::memcpy(m, meta->data(), sizeof m);
  if (ms) *ms = m[2];
  return m[0] == 1.0;
}

// sat.check on the device that made the proof, with the proof it left there:
// the proof buffer is the solve's own output tensor, still staged on the peer,
// so nothing but the cube crosses the link.
Result<bool> check_in_place(Kernel& k, Worker& w, const std::vector<int>& cube, uint64_t proof_len, double* ms) {
  std::vector<int32_t> c(1 + cube.size());
  c[0] = (int32_t)cube.size();
  for (size_t i = 0; i < cube.size(); ++i) c[i + 1] = cube[i];
  AIOS_TRY(w.s->write(w.cube.memory, w.cube.offset, ByteSpan(reinterpret_cast<const uint8_t*>(c.data()), c.size() * 4)));
  Operation op = sat_op(sat::kCheckKernel, w.selector);
  op.scalar = (double)proof_len;
  op.inputs = {w.formula, w.cube, w.proof};
  op.outputs = {w.cmeta};
  std::string dev;
  AIOS_TRY(run_graph(k, *w.s, op, &dev));
  auto meta = w.s->read(w.cmeta.memory, w.cmeta.offset, 4 * sizeof(double));
  if (!meta.ok()) return meta.error();
  double m[4];
  std::memcpy(m, meta->data(), sizeof m);
  if (ms) *ms = m[2];
  return m[0] == 1.0;
}

// ---- the ledger: every run in the machine's own ASQL, with Things on it ----
// sat.runs / sat.cubes / sat.workers, a row per cube as it finishes (a busy
// run is thousands of writes a minute), and two Things that read them live:
//   sat_run      per run: cubes, refuted, verified, failed - an alert when a
//                proof fails, or a run finishes with leaves not verified
//   sat_machine  per machine: cubes, conflicts, kernel microseconds
// Codes, not words, in the columns the Things read: status 10 SAT, 20 UNSAT,
// 0 split; verdict 1 verified, -1 failed, 0 not yet.
struct Ledger {
  userland::SqlDatabase* db = nullptr;
  std::string run, why;
  bool ok = false;
  // Rows are written by a thread of the ledger's own, in batches, one
  // transaction (one durable flush) per batch: a worker hands its row over
  // and goes back to solving - it never waits on a disk.
  hal::Mutex qmu;
  std::vector<std::string> pending;
  bool stopping = false;
  hal::Thread writer;
  uint64_t batches = 0, statements = 0, failures = 0;
  void post(std::string sql) { hal::LockGuard g(qmu); pending.push_back(std::move(sql)); }
  void start() {
    writer = hal::Thread([this] {
      for (;;) {
        std::vector<std::string> batch;
        bool last = false;
        { hal::LockGuard g(qmu); batch.swap(pending); last = stopping; }
        if (!batch.empty()) {
          exec("BEGIN");
          for (const auto& q : batch) { if (!exec(q)) failures++; statements++; }
          exec("COMMIT");
          batches++;
        }
        if (last && batch.empty()) return;
        if (batch.empty()) hal::sleep_ms(50);
      }
    });
  }
  void finish() {
    { hal::LockGuard g(qmu); stopping = true; }
    if (writer.joinable()) writer.join();
  }
  ~Ledger() { finish(); }
  static std::string q(const std::string& v) {
    std::string o = "'";
    for (char c : v) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
    return o + "'";
  }
  bool exec(const std::string& sql) {
    if (!db) return false;
    auto r = db->execute_local_query(sql, "sat");
    if (!r.success) { why = r.error_message + " (" + sql.substr(0, 80) + ")"; return false; }
    return true;
  }
  bool has_thing(const std::string& name) {
    auto r = db->execute_local_query("SHOW THINGS", "sat");
    if (!r.success) return false;
    for (const auto& row : r.rows) if (!row.values.empty() && row.values[0] == name) return true;
    return false;
  }
  void open(Kernel& k, const std::string& run_id) {
    db = userland::machine_sql(k);
    if (!db) { why = "no SQL engine on this machine"; return; }
    run = run_id;
    ok = exec("CREATE DATABASE IF NOT EXISTS sat") &&
         exec("CREATE TABLE IF NOT EXISTS runs (run VARCHAR(40) PRIMARY KEY, file VARCHAR(255), vars INT, clauses INT, "
              "started DATETIME, finished DATETIME, result INT, wall_ms BIGINT, workers INT, leaves INT, verified INT, "
              "cross_checked INT, receipt VARCHAR(40))") &&
         exec("CREATE TABLE IF NOT EXISTS cubes (id VARCHAR(64) PRIMARY KEY, run VARCHAR(40), lits VARCHAR(1024), worker INT, "
              "machine VARCHAR(80), status INT, conflicts BIGINT, kernel_us BIGINT, proof_bytes BIGINT, proof_fp VARCHAR(16), "
              "checked_on VARCHAR(80), verdict INT, receipt VARCHAR(40))") &&
         exec("CREATE TABLE IF NOT EXISTS workers (id VARCHAR(64) PRIMARY KEY, run VARCHAR(40), worker INT, selector VARCHAR(80), "
              "machine VARCHAR(80), entity VARCHAR(40), cubes INT, busy_ms BIGINT, solve_ms BIGINT, check_ms BIGINT, bytes_back BIGINT)");
    if (!ok) return;
    // sat_run's definition has changed (version 2: no alarm on a SAT run); a
    // root that declared the old one gets it replaced. Its values are derived
    // from runs and cubes, so nothing is lost by declaring it again.
    ok = exec("CREATE TABLE IF NOT EXISTS ledger_meta (k VARCHAR(40) PRIMARY KEY, v INT)");
    if (!ok) return;
    {
      auto r = db->execute_local_query("SELECT v FROM ledger_meta WHERE k = 'sat_run'", "sat");
      const bool current = r.success && !r.rows.empty() && !r.rows[0].values.empty() && r.rows[0].values[0] == "2";
      if (!current && has_thing("sat_run")) ok = exec("DROP THING sat_run");
      if (ok && !current) ok = exec("REPLACE INTO ledger_meta (k, v) VALUES ('sat_run', 2)");
      if (!ok) return;
    }
    if (!has_thing("sat_run"))
      ok = exec("CREATE THING sat_run GRAIN KEY run READS cubes, runs WATCH cubes(run), runs(run) AS '"
                "fn enroll() { let out = [] for r in read(\"SELECT run FROM runs\") { push(out, r.run) } return out } "
                "fn shared() { return 1 } "
                "fn things(ks, sh) { let out = {} for k in ks { "
                "let c = read(\"SELECT COUNT(*) AS n, SUM(CASE WHEN status = 20 THEN 1 ELSE 0 END) AS unsat, "
                "SUM(CASE WHEN verdict = 1 THEN 1 ELSE 0 END) AS ok, SUM(CASE WHEN verdict = -1 THEN 1 ELSE 0 END) AS bad "
                "FROM cubes WHERE run = ?\", k)[0] "
                "let rs = read(\"SELECT result, finished, wall_ms FROM runs WHERE run = ?\", k) "
                "let v = {cubes: c.n, refuted: c.unsat, verified: c.ok, failed: c.bad} "
                "if len(rs) > 0 { v.result = rs[0].result v.wall_ms = rs[0].wall_ms } "
                "if c.bad != nil and c.bad > 0 { v.alert = \"a proof failed to verify\" } "
                // Only an UNSAT answer rests on its leaves: a SAT run is proven by
                // its model (checked against every clause) and stops before the
                // refuted cubes it no longer needs are checked.
                "else if len(rs) > 0 and rs[0].finished != nil and rs[0].result == 20 and c.ok != c.unsat { v.alert = \"finished UNSAT with leaves not verified\" } "
                "out[k] = v } return out }'");
    if (ok && !has_thing("sat_machine"))
      ok = exec("CREATE THING sat_machine KEY machine PARTITION BY id READS cubes AS "
                "SELECT machine, COUNT(*) AS cubes, SUM(conflicts) AS conflicts, SUM(kernel_us) AS kernel_us FROM cubes GROUP BY machine");
    if (ok) start();
  }
  void cube(const std::string& id, const std::vector<int>& lits, size_t worker, const std::string& machine, int status,
            double conflicts, double kernel_ms, uint64_t proof_bytes, const std::string& fp, const std::string& checked_on,
            int verdict, const std::string& receipt) {
    if (!ok) return;
    std::string l;
    for (int x : lits) { if (!l.empty()) l += ' '; l += std::to_string(x); }
    post("REPLACE INTO cubes VALUES (" + q(id) + ", " + q(run) + ", " + q(l) + ", " + std::to_string(worker) + ", " + q(machine) + ", " +
         std::to_string(status) + ", " + std::to_string((uint64_t)conflicts) + ", " + std::to_string((uint64_t)(kernel_ms * 1000)) + ", " +
         std::to_string(proof_bytes) + ", " + q(fp) + ", " + q(checked_on) + ", " + std::to_string(verdict) + ", " + q(receipt) + ")");
  }
  void verdict(const std::string& id, const std::string& checked_on, int v, const std::string& receipt) {
    if (!ok) return;
    post("UPDATE cubes SET checked_on = " + q(checked_on) + ", verdict = " + std::to_string(v) + ", receipt = " + q(receipt) + " WHERE id = " + q(id));
  }
};

std::string fp_hex(const std::string& bytes) {
  char h[20];
  std::snprintf(h, sizeof h, "%016llx", (unsigned long long)fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())));
  return h;
}

// A cube's receipt: an object in the store whose provenance is the worker
// entity that solved it - what it was asked, what it answered, and who
// checked the answer where.
std::string store_receipt(Kernel& k, Surface& s, const std::string& run, const std::vector<int>& lits, const std::string& machine,
                          int status, double conflicts, uint64_t proof_bytes, const std::string& fp, const std::string& checked_by,
                          const std::string& checked_on, int verdict) {
  StoredObject rec;
  rec.timestamp_ns = k.now();
  std::string l;
  for (int x : lits) { if (!l.empty()) l += ' '; l += std::to_string(x); }
  rec.metadata = {{"kind", "sat.cube"}, {"run", run}, {"cube", l}, {"machine", machine},
                  {"status", status == 10 ? "SAT" : status == 20 ? "UNSAT" : "unknown"}, {"conflicts", std::to_string((uint64_t)conflicts)},
                  {"proof_bytes", std::to_string(proof_bytes)}, {"proof_fnv", fp}, {"checked_by", checked_by},
                  {"checked_on", checked_on}, {"verdict", verdict == 1 ? "verified" : verdict == -1 ? "FAILED" : "none"}};
  auto st = s.store(std::move(rec));
  return st.ok() ? st->str() : std::string();
}

struct Formula {
  int vars = 0;
  std::vector<std::vector<int>> clauses;
  std::vector<int32_t> flat;   // [vars, lits..., 0, ...]
};
Result<Formula> load_formula(Kernel& k, const std::string& path) {
  auto f = k.host().read_file(path);
  if (!f.ok()) return Result<Formula>(f.error().status, "cannot read " + path + ": " + f.error().message);
  Formula out;
  std::string err;
  if (!userland::SatSolver::parse_dimacs(std::string(f->begin(), f->end()), &out.vars, &out.clauses, &err))
    return Result<Formula>(Status::Invalid, path + ": " + err);
  out.flat.push_back(out.vars);
  for (const auto& c : out.clauses) { for (int l : c) out.flat.push_back(l); out.flat.push_back(0); }
  return out;
}

// Variables in the order a split should take them: most occurrences first.
std::vector<int> split_order(const Formula& f) {
  std::vector<uint64_t> occ((size_t)f.vars + 1, 0);
  for (const auto& c : f.clauses) for (int l : c) occ[(size_t)(l > 0 ? l : -l)]++;
  std::vector<int> vs;
  for (int v = 1; v <= f.vars; ++v) if (occ[(size_t)v]) vs.push_back(v);
  std::stable_sort(vs.begin(), vs.end(), [&](int a, int b) { return occ[(size_t)a] > occ[(size_t)b]; });
  return vs;
}

// Which machine a device is on: a remote device is named
// "remote:<peer address>/<device>"; anything else is this machine.
std::string machine_of(const std::string& device) {
  if (device.rfind("remote:", 0) != 0) return "this machine";
  const size_t slash = device.find('/');
  return device.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
}

void cleanup(Surface& me, std::vector<Worker>& ws) {
  for (auto& w : ws) if (!w.entity.is_null()) (void)me.terminate(w.entity);
}

}  // namespace

Result<void> run_sat_command(Kernel& k, Surface& me, Out& o, const std::vector<std::string>& a) {
  const std::string sub = a.size() > 1 ? a[1] : "devices";
  // Fleet peers' devices join this kernel's fabric when each peer is reached.
  bool remote = sub == "devices";
  for (const auto& x : a) if (x.find("remote") != std::string::npos) remote = true;
  if (remote && k.in_fleet()) {
    k.fleet_sync();
    // A named peer may take a moment to be reached (its handshake rides the
    // hub): wait for its device rather than fail on the first look.
    std::vector<std::string> want;
    for (const auto& x : a)
      if (x.rfind("on=", 0) == 0 || x.rfind("check_on=", 0) == 0)
        for (const auto& sel : split_commas(x.substr(x.find('=') + 1)))
          if (sel.rfind("remote", 0) == 0) want.push_back(sel.substr(0, sel.find_first_of("@!")));
    for (int tries = 0; tries < 15; ++tries) {
      bool all = true;
      for (const auto& sel : want) {
        bool found = false;
        for (Device* d : k.devices().all()) if (d->name().rfind(sel, 0) == 0 && device_runs(d, sat::kSolveKernel)) found = true;
        all = all && found;
      }
      if (all) break;
      hal::sleep_ms(1000);
      k.fleet_sync();
    }
  }

  if (sub == "devices") {
    size_t n = 0;
    for (Device* d : k.devices().all()) {
      const bool solve = device_runs(d, sat::kSolveKernel), check = device_runs(d, sat::kCheckKernel);
      if (!solve && !check) continue;
      o << "  " << W{44} << d->name() << d->id().short_str() << "  lanes " << d->capacity().lanes
        << (solve ? "  sat.solve" : "") << (check ? "  sat.check" : "") << "\n";
      ++n;
    }
    o << n << " device(s) run the solver\n";
    return Ok();
  }

  if (sub == "solve") {
    if (a.size() < 3) return Result<void>(Status::Invalid, "sat solve FILE [on=SEL] [conflicts=N] [check_on=SEL]");
    auto f = load_formula(k, a[2]);
    if (!f.ok()) return f.error();
    std::vector<Worker> ws(1);
    ws[0].selector = kv(a, "on", "cpu");
    const std::string check_on = kv(a, "check_on", "");
    if (!check_on.empty()) { ws.emplace_back(); ws[1].selector = check_on; }
    for (auto& w : ws) {
      auto r = alloc_worker(k, me, w, f->flat, f->vars, 32ull << 20);
      if (!r.ok()) { cleanup(me, ws); return r; }
    }
    const double t0 = now_ms();
    auto r = solve_cube(k, ws[0], {}, std::strtoull(kv(a, "conflicts", "0").c_str(), nullptr, 10));
    if (!r.ok()) { cleanup(me, ws); return r.error(); }
    const double wall = now_ms() - t0;
    o << "c solved on " << r->device << ": " << (uint64_t)r->conflicts << " conflicts, " << (uint64_t)r->decisions
      << " decisions, kernel " << Fixed{1} << r->ms << " ms, wall " << Fixed{1} << wall << " ms\n";
    {
      // Fingerprints of what the search produced: the same formula and budget
      // must give the same bits on every machine (x86 or ARM) - the solver
      // decides nothing by clock, randomness or address.
      char h[64];
      std::snprintf(h, sizeof h, "%016llx", (unsigned long long)fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(r->proof.data()), r->proof.size())));
      o << "c fingerprint: proof " << h << " (" << (uint64_t)r->proof.size() << " bytes), propagations " << (uint64_t)r->propagations;
      if (r->status == 10) {
        std::snprintf(h, sizeof h, "%016llx", (unsigned long long)fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(r->model.data()), r->model.size() * 4)));
        o << ", model " << h;
      }
      o << "\n";
    }
    if (r->status == 10) {
      std::vector<bool> model((size_t)f->vars + 1, false);
      for (int v = 1; v <= f->vars; ++v) model[(size_t)v] = r->model[(size_t)v] > 0;
      const bool ok = userland::SatSolver::check_model(f->clauses, model);
      o << (ok ? "s SATISFIABLE" : "s ERROR (the model does not satisfy the formula)") << "\nc model checked here: " << (ok ? "every clause satisfied" : "FAILED") << "\n";
    } else if (r->status == 20) {
      o << "s UNSATISFIABLE\nc proof: " << (uint64_t)r->proof.size() << " bytes binary DRAT\n";
      Worker& checker = ws.size() > 1 ? ws[1] : ws[0];
      double cms = 0;
      auto v = check_cube(k, checker, {}, r->proof, &cms);
      if (!v.ok()) { cleanup(me, ws); return v.error(); }
      o << "c proof checked by sat.check on " << checker.selector << ": " << (*v ? "VERIFIED" : "NOT VERIFIED") << " (" << Fixed{1} << cms << " ms)\n";
    } else {
      o << "s UNKNOWN (budget spent)\n";
    }
    cleanup(me, ws);
    return Ok();
  }

  if (sub == "fleet") {
    if (a.size() < 3 || kv(a, "on").empty()) return Result<void>(Status::Invalid, "sat fleet FILE on=SEL,SEL,... [budget=N] [depth=D]");
    auto f = load_formula(k, a[2]);
    if (!f.ok()) return f.error();
    const auto sels = split_commas(kv(a, "on"));
    const uint64_t budget = std::strtoull(kv(a, "budget", "20000").c_str(), nullptr, 10);
    const std::vector<int> order = split_order(*f);
    int depth = kv(a, "depth").empty() ? (int)std::ceil(std::log2(4.0 * (double)sels.size())) : std::atoi(kv(a, "depth").c_str());
    depth = std::max(0, std::min<int>(depth, (int)order.size()));

    std::vector<Worker> ws(sels.size());
    for (size_t i = 0; i < sels.size(); ++i) {
      // SEL[@budget][!]: a worker's own conflict budget, and "!" for a
      // machine that should solve but never check (a slow link, a small box)
      std::string sel = sels[i];
      if (!sel.empty() && sel.back() == '!') { ws[i].checks_proofs = false; sel.pop_back(); }
      const size_t at = sel.find('@');
      if (at != std::string::npos) { ws[i].budget = std::strtoull(sel.c_str() + at + 1, nullptr, 10); sel = sel.substr(0, at); }
      ws[i].selector = sel;
      auto r = alloc_worker(k, me, ws[i], f->flat, f->vars, 8ull << 20);
      if (!r.ok()) { cleanup(me, ws); return Result<void>(r.error().status, sels[i] + ": " + r.error().message); }
    }

    Ledger ledger;
    {
      char rid[32];
      std::snprintf(rid, sizeof rid, "%llx", (unsigned long long)k.now());
      ledger.open(k, rid);
      if (ledger.ok)
        ledger.exec("INSERT INTO runs (run, file, vars, clauses, started, workers) VALUES (" + Ledger::q(ledger.run) + ", " + Ledger::q(a[2]) + ", " +
                    std::to_string(f->vars) + ", " + std::to_string(f->clauses.size()) + ", NOW(), " + std::to_string(ws.size()) + ")");
    }
    uint64_t cube_seq = 0;
    std::vector<std::string> receipts;

    struct Cube { std::vector<int> lits; size_t next_split; };
    struct Check { std::vector<int> lits; std::string proof; size_t solver; std::string id; };
    hal::Mutex mu;
    std::deque<Cube> queue;
    std::deque<Check> checks;
    size_t in_flight = 0;
    bool done = false, sat_found = false, failed = false;
    std::string fail_why;
    std::vector<int32_t> sat_model;
    uint64_t leaves_unsat = 0, leaves_verified = 0, splits = 0;
    std::map<std::pair<size_t, size_t>, uint64_t> check_pairs;   // (solver, checker) -> count
    uint64_t verified_in_place = 0;   // on the machine that made them, by the checker code, never moved

    for (uint64_t m = 0; m < (1ull << depth); ++m) {
      Cube c;
      for (int i = 0; i < depth; ++i) c.lits.push_back((m >> i) & 1 ? order[(size_t)i] : -order[(size_t)i]);
      c.next_split = (size_t)depth;
      queue.push_back(c);
    }
    const double t0 = now_ms();
    std::vector<hal::Thread> threads;
    for (size_t wi = 0; wi < ws.size(); ++wi) {
      threads.emplace_back([&, wi] {
        Worker& w = ws[wi];
        for (;;) {
          // Receipts and ledger rows are written after the fleet's lock is let
          // go (declared before any lock, so it runs after each is released).
          struct Later { std::vector<std::function<void()>> fns; ~Later() { for (auto& f : fns) f(); } } later;
          Cube cube; Check chk; int job = 0;   // 1 cube, 2 check
          {
            hal::LockGuard g(mu);
            if (done) return;
            // a proof waiting for a machine other than the one that made it
            size_t checkers = 0;
            for (auto& x : ws) if (x.checks_proofs) ++checkers;
            if (w.checks_proofs || checkers == 0)
              for (auto it = checks.begin(); it != checks.end(); ++it)
                if (it->solver != wi || checkers <= 1) { chk = std::move(*it); checks.erase(it); job = 2; ++in_flight; break; }
            if (!job && !queue.empty()) { cube = std::move(queue.front()); queue.pop_front(); job = 1; ++in_flight; }
            if (!job && in_flight == 0 && checks.empty()) { done = true; return; }
          }
          if (!job) { hal::sleep_ms(20); continue; }
          const double j0 = now_ms();
          if (job == 2) {
            double cms = 0;
            auto v = check_cube(k, w, chk.lits, chk.proof, &cms);
            hal::LockGuard g(mu);
            --in_flight;
            w.checks++;
            w.check_kernel_ms += cms;
            w.busy_ms += now_ms() - j0;
            if (!v.ok() || !*v) {
              w.checks_failed++;
              failed = true; done = true;
              fail_why = v.ok() ? "a proof from " + ws[chk.solver].selector + " did not verify on " + w.selector
                                : "checking on " + w.selector + ": " + v.error().str();
              return;
            }
            leaves_verified++;
            check_pairs[{chk.solver, wi}]++;
            later.fns.push_back([&, lits = chk.lits, from = machine_of(ws[chk.solver].device_seen), bytes = (uint64_t)chk.proof.size(),
                                 fp = fp_hex(chk.proof), id = chk.id] {
              const std::string rc = store_receipt(k, *w.s, ledger.run, lits, from, 20, 0, bytes, fp, w.entity.short_str(), machine_of(w.device_seen), 1);
              if (!rc.empty()) { hal::LockGuard g2(mu); receipts.push_back(rc); }
              ledger.verdict(id, machine_of(w.device_seen), 1, rc);
            });
            continue;
          }
          // A remote worker's proof stays where it was made and is checked
          // there; one in four comes back to be checked on another machine.
          const bool remote_worker = w.selector.rfind("remote", 0) == 0;
          const bool keep_there = remote_worker && (w.solves++ % 4) != 3;
          auto r = solve_cube(k, w, cube.lits, w.budget ? w.budget : budget, keep_there);
          if (r.ok() && r->status == 20 && r->proof_left) {
            double cms = 0;
            auto v = check_in_place(k, w, cube.lits, r->proof_len, &cms);
            hal::LockGuard g(mu);
            --in_flight;
            w.busy_ms += now_ms() - j0;
            w.cubes++; w.unsat++; leaves_unsat++;
            w.conflicts += r->conflicts; w.kernel_ms += r->ms; w.check_kernel_ms += cms;
            if (w.device_seen.empty()) w.device_seen = r->device;
            if (!v.ok() || !*v) {
              failed = true; done = true;
              fail_why = v.ok() ? "a proof from " + w.selector + " did not verify where it was made" : "checking on " + w.selector + ": " + v.error().str();
              return;
            }
            w.in_place_checks++;
            leaves_verified++;
            verified_in_place++;
            later.fns.push_back([&, id = ledger.run + ":" + std::to_string(cube_seq++), mach = machine_of(r->device), lits = cube.lits,
                                 conf = r->conflicts, ms = r->ms, len = r->proof_len] {
              const std::string rc = store_receipt(k, *w.s, ledger.run, lits, mach, 20, conf, len, "(left on " + mach + ")", w.entity.short_str(), mach, 1);
              if (!rc.empty()) { hal::LockGuard g2(mu); receipts.push_back(rc); }
              ledger.cube(id, lits, wi, mach, 20, conf, ms, len, "", mach, 1, rc);
            });
            continue;
          }
          hal::LockGuard g(mu);
          --in_flight;
          w.busy_ms += now_ms() - j0;
          if (!r.ok()) { failed = true; done = true; fail_why = w.selector + ": " + r.error().str(); return; }
          w.cubes++;
          w.conflicts += r->conflicts;
          w.kernel_ms += r->ms;
          w.bytes_back += r->proof.size() + r->model.size() * 4;
          if (w.device_seen.empty()) w.device_seen = r->device;
          if (r->status == 10) {
            w.sat++;
            std::vector<bool> model((size_t)f->vars + 1, false);
            for (int v = 1; v <= f->vars; ++v) model[(size_t)v] = r->model[(size_t)v] > 0;
            if (!userland::SatSolver::check_model(f->clauses, model)) {
              failed = true; done = true; fail_why = "a model from " + w.selector + " does not satisfy the formula";
              return;
            }
            sat_found = true; done = true; sat_model = r->model;
            later.fns.push_back([&, id = ledger.run + ":" + std::to_string(cube_seq++), mach = machine_of(r->device), lits = cube.lits,
                                 conf = r->conflicts, ms = r->ms] {
              const std::string rc = store_receipt(k, *w.s, ledger.run, lits, mach, 10, conf, 0, "", std::string("coordinator"), "coordinator (model checked)", 1);
              if (!rc.empty()) { hal::LockGuard g2(mu); receipts.push_back(rc); }
              ledger.cube(id, lits, wi, mach, 10, conf, ms, 0, "", "coordinator", 1, rc);
            });
            return;
          }
          if (r->status == 20) {
            w.unsat++; leaves_unsat++;
            const std::string id = ledger.run + ":" + std::to_string(cube_seq++);
            ledger.cube(id, cube.lits, wi, machine_of(r->device), 20, r->conflicts, r->ms, r->proof.size(), fp_hex(r->proof), "", 0, "");
            checks.push_back(Check{cube.lits, std::move(r->proof), wi, id});
            continue;
          }
          // budget spent: split on the next variable the cube leaves open
          w.unknown++;
          ledger.cube(ledger.run + ":" + std::to_string(cube_seq++), cube.lits, wi, machine_of(r->device), 0, r->conflicts, r->ms, 0, "", "", 0, "");
          size_t nx = cube.next_split;
          while (nx < order.size() && std::find_if(cube.lits.begin(), cube.lits.end(), [&](int l) { return (l > 0 ? l : -l) == order[nx]; }) != cube.lits.end()) ++nx;
          if (nx >= order.size()) { failed = true; done = true; fail_why = "a cube with every variable assigned ran out of budget"; return; }
          Cube a1 = cube, a2 = cube;
          a1.lits.push_back(order[nx]); a2.lits.push_back(-order[nx]);
          a1.next_split = a2.next_split = nx + 1;
          queue.push_front(a2);
          queue.push_front(a1);
          splits++;
        }
      });
    }
    for (auto& t : threads) t.join();
    const double wall = now_ms() - t0;

    o << "c fleet: " << (uint64_t)ws.size() << " worker(s), budget " << budget << " conflicts a cube, first split depth " << depth
      << ", " << splits << " further splits, wall " << Fixed{1} << wall << " ms\n";
    size_t wn = 0;
    for (auto& w : ws)
      o << "c   worker " << wn++ << " " << W{24} << w.selector << " on " << W{34} << (w.device_seen.empty() ? std::string("-") : w.device_seen)
        << " entity " << w.entity.short_str() << "  cubes " << w.cubes << " (sat " << w.sat << ", unsat " << w.unsat << ", split "
        << w.unknown << ")  checks " << w.checks << "  conflicts " << (uint64_t)w.conflicts << "  busy " << Fixed{0} << w.busy_ms
        << " ms (solving " << Fixed{0} << w.kernel_ms << ", checking " << Fixed{0} << w.check_kernel_ms << ", the rest moving data: "
        << Fixed{0} << std::max(0.0, w.busy_ms - w.kernel_ms - w.check_kernel_ms) << ")  " << w.bytes_back / 1024 << " KB back\n";
    {
      // the run's own record: its result, and every cube receipt it left
      uint64_t cross = 0;
      for (const auto& p : check_pairs) if (machine_of(ws[p.first.first].device_seen) != machine_of(ws[p.first.second].device_seen)) cross += p.second;
      const int result = failed ? -1 : sat_found ? 10 : 20;
      StoredObject rec;
      rec.timestamp_ns = k.now();
      rec.metadata = {{"kind", "sat.run"}, {"run", ledger.run}, {"file", a[2]}, {"result", failed ? "ERROR" : sat_found ? "SAT" : "UNSAT"},
                      {"leaves", std::to_string(leaves_unsat)}, {"verified", std::to_string(leaves_verified)},
                      {"cross_checked", std::to_string(cross)}, {"wall_ms", std::to_string((uint64_t)wall)}, {"workers", std::to_string(ws.size())}};
      std::string list;
      for (const auto& r : receipts) list += r + "\n";
      rec.payload.assign(list.begin(), list.end());
      auto run_rc = me.store(std::move(rec));
      if (ledger.ok) {
        ledger.finish();   // every cube row and verdict first; the run finishes after them
        ledger.exec("UPDATE runs SET finished = NOW(), result = " + std::to_string(result) + ", wall_ms = " + std::to_string((uint64_t)wall) +
                    ", leaves = " + std::to_string(leaves_unsat) + ", verified = " + std::to_string(leaves_verified) + ", cross_checked = " +
                    std::to_string(cross) + ", receipt = " + Ledger::q(run_rc.ok() ? run_rc->str() : "") + " WHERE run = " + Ledger::q(ledger.run));
        for (size_t i = 0; i < ws.size(); ++i) {
          const Worker& w = ws[i];
          ledger.exec("REPLACE INTO workers VALUES (" + Ledger::q(ledger.run + ":" + std::to_string(i)) + ", " + Ledger::q(ledger.run) + ", " +
                      std::to_string(i) + ", " + Ledger::q(w.selector) + ", " + Ledger::q(machine_of(w.device_seen)) + ", " + Ledger::q(w.entity.short_str()) +
                      ", " + std::to_string(w.cubes) + ", " + std::to_string((uint64_t)w.busy_ms) + ", " + std::to_string((uint64_t)w.kernel_ms) + ", " +
                      std::to_string((uint64_t)w.check_kernel_ms) + ", " + std::to_string(w.bytes_back) + ")");
        }
        o << "c ledger: run " << ledger.run << " in ASQL (sat.runs, " << cube_seq << " rows in sat.cubes, sat.workers) in " << ledger.batches
          << " transaction(s), " << ledger.failures << " failed statement(s); Things sat_run, sat_machine\n";
      } else {
        o << "c ledger: not written - " << ledger.why << "\n";
      }
      o << "c receipts: " << (uint64_t)receipts.size() << " cube receipt(s) under the workers' entities; run receipt "
        << (run_rc.ok() ? run_rc->str() : "not stored: " + run_rc.error().str()) << "\n";
    }
    if (failed) {
      o << "s ERROR " << fail_why << "\n";
      cleanup(me, ws);
      return Ok();
    }
    if (sat_found) {
      o << "s SATISFIABLE\nc model checked here against every clause\n";
    } else {
      o << "s UNSATISFIABLE\nc " << leaves_unsat << " leaf cube(s), every one refuted, " << leaves_verified << " proof(s) verified by sat.check";
      uint64_t cross = 0, same = 0;
      std::map<std::pair<std::string, std::string>, uint64_t> by_machine;
      for (const auto& p : check_pairs) {
        const std::string from = machine_of(ws[p.first.first].device_seen), on = machine_of(ws[p.first.second].device_seen);
        (from == on ? same : cross) += p.second;
        by_machine[{from, on}] += p.second;
      }
      o << " (" << cross << " checked on a different machine than solved it, " << same << " on the same machine by another worker, "
        << verified_in_place << " on the machine that made it, never moved)\n";
      for (const auto& p : by_machine) o << "c   proofs from " << p.first.first << " checked on " << p.first.second << ": " << p.second << "\n";
      if (leaves_verified != leaves_unsat) o << "c WARNING: " << (leaves_unsat - leaves_verified) << " proof(s) not checked\n";
    }
    cleanup(me, ws);
    return Ok();
  }
  return Result<void>(Status::Invalid, "sat devices | sat solve FILE [on=SEL] [conflicts=N] [check_on=SEL] | sat fleet FILE on=SEL,... [budget=N] [depth=D]");
}

}  // namespace aios::cli
