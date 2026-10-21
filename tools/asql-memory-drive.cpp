#undef NDEBUG
// Drives the real ASQL store the way the console does -- insert, overwrite,
// delete (tombstone), VACUUM -- and reports the process's own RSS at each
// step. The question it answers is the operational one: after the rows are
// gone, does the machine get the memory back?
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <malloc.h>
#include <dlfcn.h>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"

using namespace aios;
using namespace aios::userland;

static size_t rss_kb() {
  std::ifstream f("/proc/self/statm");
  size_t total = 0, resident = 0;
  if (f >> total >> resident) return resident * (size_t)(sysconf(_SC_PAGESIZE) / 1024);
  return 0;
}

// Anonymous (heap) vs file-backed (the mapped log) resident kB, from /proc/self/status.
static std::string rss_split() {
  std::ifstream f("/proc/self/status");
  std::string k, unit; size_t v = 0, anon = 0, file = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (sscanf(line.c_str(), "RssAnon: %zu", &v) == 1) anon = v;
    else if (sscanf(line.c_str(), "RssFile: %zu", &v) == 1) file = v;
  }
  const struct mallinfo2 mi = mallinfo2();   // live allocations vs free-but-held heap
  return " [anon " + std::to_string(anon / 1024) + " MB, file " + std::to_string(file / 1024) + " MB, malloc in-use " +
         std::to_string(mi.uordblks >> 20) + " MB free-held " + std::to_string(mi.fordblks >> 20) + " MB]";
}

static void mark(const char* what, size_t baseline) {
  // An LD_PRELOAD allocation profiler may export prof_dump(tag); call it when present.
  if (auto dump = reinterpret_cast<void (*)(const char*)>(dlsym(RTLD_DEFAULT, "prof_dump"))) dump(what);
  size_t now = rss_kb();
  std::cout << "  RSS after " << what << ": " << (now / 1024) << " MB"
            << "  (delta " << ((long)now - (long)baseline) / 1024 << " MB)" << rss_split() << "\n";
  std::cout.flush();
}

static uint64_t log_bytes(Kernel& k, const std::string& root) {
  auto info = k.host().stat(root + "/store/objects.log");
  return info.ok() ? info->size : 0;
}

int main(int argc, char** argv) {
  // No AIOS_NO_ACCEL here: accelerators register lazily (DeviceFabric), so a
  // process that only runs SQL holds no GPU runtime. This drive measures that.
  size_t rows = argc > 1 ? std::stoul(argv[1]) : 20000;
  size_t payload_bytes = argc > 2 ? std::stoul(argv[2]) : 4096;

  char root_dir[] = "/tmp/aios-memory-drive-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); return 1; }
  std::cout << "=== ASQL memory drive === root=" << root_dir
            << " rows=" << rows << " payload=" << payload_bytes << "B\n";

  KernelConfig cfg;
  cfg.root = root_dir;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();
  std::cout << "  RSS after kernel boot: " << (rss_kb() / 1024) << " MB\n";
  TaskLedger ledger(kernel);
  ledger.init("tasks");
  std::cout << "  RSS after ledger init: " << (rss_kb() / 1024) << " MB\n";
  SqlDatabase db(kernel, &ledger);
  std::cout << "  RSS after SqlDatabase ctor: " << (rss_kb() / 1024) << " MB\n";
  db.init("sql_db");

  const size_t base = rss_kb();
  std::cout << "  RSS at rest: " << (base / 1024) << " MB\n";

  const std::string blob(payload_bytes, 'x');

  auto insert_batch = [&](size_t from, size_t to, const char* kind) {
    std::string q = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES ";
    for (size_t i = from; i < to; ++i) {
      if (i > from) q += ",";
      q += "('row:" + std::to_string(i) + "','" + kind + "','" + blob + "')";
    }
    q += ";";
    auto r = db.execute_gateway_query(q);
    if (!r.success) { std::cerr << "insert failed: " << r.error_message << "\n"; exit(1); }
  };

  std::cout << "[1] inserting " << rows << " rows...\n";
  for (size_t i = 0; i < rows; i += 100) insert_batch(i, std::min(rows, i + 100), "telemetry");
  mark("insert", base);
  std::cout << "  objects.log: " << (log_bytes(kernel, root_dir) >> 20) << " MB, store count="
            << kernel.store().count() << " hot=" << (kernel.store().hot_bytes() >> 20) << " MB\n";

  std::cout << "[2] overwriting every row once (a second version per key)...\n";
  for (size_t i = 0; i < rows; i += 100) insert_batch(i, std::min(rows, i + 100), "telemetry2");
  mark("overwrite", base);
  std::cout << "  objects.log: " << (log_bytes(kernel, root_dir) >> 20) << " MB, store count="
            << kernel.store().count() << " hot=" << (kernel.store().hot_bytes() >> 20) << " MB\n";

  std::cout << "[3] deleting half the rows (tombstones, as the console bridge does)...\n";
  for (size_t i = 0; i < rows / 2; i += 100) {
    std::string q = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD, STATUS) VALUES ";
    for (size_t j = i; j < std::min(rows / 2, i + 100); ++j) {
      if (j > i) q += ",";
      q += "('row:" + std::to_string(j) + "','tombstone','','__aios_tombstone__')";
    }
    q += ";";
    auto r = db.execute_gateway_query(q);
    if (!r.success) { std::cerr << "delete failed: " << r.error_message << "\n"; exit(1); }
  }
  if (std::getenv("DRIVE_TRIM")) malloc_trim(0);   // experiment: what an idle trim would hand back
  mark("delete", base);
  if (std::getenv("DRIVE_SHOW_STATUS")) {   // what the engine itself counts, beside the process RSS
    auto st = db.execute_gateway_query("SHOW STATUS LIKE 'Asql_memory%';");
    std::cout << st.formatted_table << "\n";
  }
  std::cout << "  objects.log: " << (log_bytes(kernel, root_dir) >> 20) << " MB, store count="
            << kernel.store().count() << " hot=" << (kernel.store().hot_bytes() >> 20) << " MB\n";

  std::cout << "[4] VACUUM OBJECTS...\n";
  const uint64_t before_log = log_bytes(kernel, root_dir);
  const size_t before_rss = rss_kb();
  auto vac = db.execute_gateway_query("VACUUM OBJECTS;");
  if (!vac.success) { std::cerr << "vacuum failed: " << vac.error_message << "\n"; return 1; }
  std::cout << vac.formatted_table << "\n";
  mark("vacuum", base);
  std::cout << "  objects.log: " << (before_log >> 20) << " MB -> "
            << (log_bytes(kernel, root_dir) >> 20) << " MB, store count=" << kernel.store().count()
            << " hot=" << (kernel.store().hot_bytes() >> 20) << " MB\n";
  std::cout << "  RSS across the vacuum: " << (before_rss / 1024) << " MB -> "
            << (rss_kb() / 1024) << " MB\n";

  std::cout << "[5] parity: deleted keys gone, surviving keys intact...\n";
  size_t bad = 0;
  for (size_t i = 0; i < rows; i += std::max<size_t>(1, rows / 200)) {
    auto r = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"row:" + std::to_string(i) + "\"");
    const bool deleted = i < rows / 2;
    size_t got = r.success ? r.rows.size() : 0;
    if (deleted && got != 0) { std::cerr << "  DELETED KEY STILL READABLE: row:" << i << "\n"; bad++; }
    if (!deleted && got != 1) { std::cerr << "  LIVE KEY LOST: row:" << i << " rows=" << got << "\n"; bad++; }
  }
  std::cout << (bad ? "  PARITY FAILED\n" : "  parity ok\n");

  std::cout << "[6] reopening the root in a fresh kernel (the compacted log has to stand on its own)...\n";
  {
    KernelConfig cfg2;
    cfg2.root = root_dir;
    auto k2 = Kernel::boot(cfg2);
    if (!k2.ok()) { std::cerr << "  REOPEN FAILED: " << k2.error().message << "\n"; return 1; }
    Kernel& kernel2 = *k2.value();
    TaskLedger ledger2(kernel2);
    ledger2.init("tasks");
    SqlDatabase db2(kernel2, &ledger2);
    db2.init("sql_db");
    std::cout << "  reopened count=" << kernel2.store().count() << " (expected " << (rows / 2) << ")\n";
    if (kernel2.store().count() != rows / 2) { std::cerr << "  COUNT MISMATCH AFTER REOPEN\n"; bad++; }
    for (size_t i = 0; i < rows; i += std::max<size_t>(1, rows / 200)) {
      auto r = db2.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"row:" + std::to_string(i) + "\"");
      const bool deleted = i < rows / 2;
      size_t got = r.success ? r.rows.size() : 0;
      if (deleted && got != 0) { std::cerr << "  REOPEN: deleted key back from the dead: row:" << i << "\n"; bad++; }
      if (!deleted && got != 1) { std::cerr << "  REOPEN: live key lost: row:" << i << " rows=" << got << "\n"; bad++; }
      if (!deleted && got == 1) {
        const size_t payload_len = r.rows[0].values.size() > 2 ? r.rows[0].values[2].size() : 0;
        if (payload_len != payload_bytes) { std::cerr << "  REOPEN: payload truncated for row:" << i
              << " (" << payload_len << " != " << payload_bytes << ")\n"; bad++; }
      }
    }
    std::cout << (bad ? "  REOPEN PARITY FAILED\n" : "  reopen parity ok\n");
  }
  std::cout << "=== done === final RSS " << (rss_kb() / 1024) << " MB for "
            << (log_bytes(kernel, root_dir) >> 20) << " MB of live data\n";
  return bad ? 1 : 0;
}
