#undef NDEBUG
// Drives the store the way the CONSOLE does -- write a conversation blob, then
// READ it back over and over -- and reports what the hot set thinks it is
// holding against what the process actually costs the machine.
//
// asql-memory-drive only writes, so its hot set stays empty and it cannot see
// this: chi-il-1's aios-storage-server sits at 2.4 GB of anonymous heap for
// ~1 GB of live data, with a 512 MB hot budget. The question this answers is
// whether the budget is being honoured and the memory is going somewhere else,
// or whether the budget itself is not what it says.
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <vector>
#ifdef __linux__
#include <malloc.h>
#include <sys/prctl.h>
#endif
#include <unistd.h>
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

int main(int argc, char** argv) {
  const size_t rows = argc > 1 ? std::stoul(argv[1]) : 2600;
  const size_t payload_bytes = argc > 2 ? std::stoul(argv[2]) : 393216;
  const size_t passes = argc > 3 ? std::stoul(argv[3]) : 3;

  // AIOS_NO_THP=1 turns transparent huge pages off for THIS process only.
  // On a host with THP set to "always" a 384KB blob can fault in a 2MB page,
  // so the same data costs several times its size in RSS. Running the same
  // binary both ways on the same box isolates that cost exactly.
#ifdef PR_SET_THP_DISABLE
  if (const char* v = std::getenv("AIOS_NO_THP"); v && v[0] == '1') {
    if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0) == 0) std::cout << "(THP disabled for this process)\n";
    else std::cout << "(PR_SET_THP_DISABLE failed)\n";
  }
#endif
  char root_dir[] = "/tmp/aios-read-pressure-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); return 1; }
  std::cout << "=== ASQL read pressure === root=" << root_dir
            << " rows=" << rows << " payload=" << payload_bytes << "B passes=" << passes << "\n";

  KernelConfig cfg;
  cfg.root = root_dir;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();
  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  auto report = [&](const char* what) {
    std::cout << "  " << what
              << ": RSS=" << (rss_kb() / 1024) << " MB"
              << "  hot=" << (kernel.store().hot_bytes() >> 20) << " MB"
              << " (" << kernel.store().hot_count() << " objs)"
              << "  budget=" << (kernel.store().hot_budget() >> 20) << " MB"
              << "  live=" << kernel.store().count() << "\n";
    std::cout.flush();
  };

  report("at rest");
  const std::string blob(payload_bytes, 'x');

  std::cout << "[1] inserting " << rows << " rows the console's way...\n";
  for (size_t i = 0; i < rows; i += 50) {
    std::string q = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES ";
    for (size_t j = i; j < std::min(rows, i + 50); ++j) {
      if (j > i) q += ",";
      q += "('row:" + std::to_string(j) + "','conv','" + blob + "')";
    }
    q += ";";
    auto r = db.execute_gateway_query(q);
    if (!r.success) { std::cerr << "insert failed: " << r.error_message << "\n"; return 1; }
  }
  report("after insert");

  // The real read the console bridge issues, one key at a time.
  for (size_t p = 1; p <= passes; ++p) {
    for (size_t i = 0; i < rows; ++i) {
      const std::string sql =
          "SELECT * FROM OBJECTS WHERE meta.kind = \"conv\" AND meta.key = \"row:" +
          std::to_string(i) + "\" ORDER BY timestamp DESC LIMIT 1;";
      auto r = db.execute_gateway_query(sql);
      if (!r.success) { std::cerr << "read failed: " << r.error_message << "\n"; return 1; }
    }
    std::cout << "[2." << p << "] read pass " << p << " over all " << rows << " keys\n";
    report("after read pass");
  }

#ifdef __linux__
  malloc_trim(0);
#endif
  report("after malloc_trim");

  const size_t live_mb = (rows * payload_bytes) >> 20;
  std::cout << "=== done === " << (rss_kb() / 1024) << " MB resident for "
            << live_mb << " MB of live payload, hot budget "
            << (kernel.store().hot_budget() >> 20) << " MB\n";
  return 0;
}
