// PkMap's runs mode (sql_database.hpp): an auto-increment table's primary-key map as runs of consecutive keys
// instead of a slot a key. What it answers must be exactly what the slots answer; only the memory differs.
//   - differential: 400 random op sequences (in-order appends with gaps, lookups, erases, re-inserts, writes
//     through operator[], renumbering, text keys, keys out of order, negative keys) checked step by step against
//     a std::map model;
//   - an in-order table stays in runs mode across erases and a renumbering, and costs a few hundred bytes for
//     1M keys (slots: ~24 MB);
//   - what turns it into slots does (a text key, a key out of order, a write through an entry, scattered keys).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>

#include "aios/userland/sql_database.hpp"

using aios::userland::PkMap;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)
#define OK(c, ...) do { if (!(c)) { std::printf("  [FAIL] "); ++failures; } else std::printf("  [ok] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

static bool g_pairs = false;   // the differential round in hand uses (k / 8, k % 8) pair keys
static std::string ik(long long k) {
  if (g_pairs && k >= 0) return std::to_string(k / 8) + '\x1f' + std::to_string(k % 8) + '\x1f';
  return std::to_string(k) + '\x1f';
}

int main() {
  // This checks runs mode itself: on even where the environment turns it off for the engine (AIOS_SQL_PK_RUNS=0).
  unsetenv("AIOS_SQL_PK_RUNS");
  // ---- differential ----
  std::mt19937_64 rng(12345);
  long checks = 0;
  int ended_in_runs = 0;
  for (int round = 0; round < 400 && failures < 20; ++round) {
    g_pairs = round % 3 == 1;
    PkMap m;
    std::map<std::string, uint32_t> ref;
    long long next_key = (long long)(rng() % 1000) - 500;
    uint32_t next_val = (uint32_t)(rng() % 50);
    // Most rounds are well-behaved (in order, few erases: they should stay runs); some are hostile.
    const int hostility = round % 5;   // 0..2 tame, 3 out-of-order sometimes, 4 text keys and writes
    const int ops = 200 + (int)(rng() % 3000);
    for (int op = 0; op < ops; ++op) {
      const int r = (int)(rng() % 100);
      if (r < 55) {   // append, usually the next key/id, sometimes after a gap
        if (rng() % 10 == 0) next_key += 1 + (long long)(rng() % 5);
        if (rng() % 20 == 0) next_val += 1 + (uint32_t)(rng() % 3);
        const std::string k = ik(next_key++);
        const uint32_t v = next_val++;
        auto res = m.emplace(k, v);
        const bool fresh = ref.emplace(k, v).second;
        CHECK(res.second == fresh, "round %d op %d: emplace new=%d, model %d", round, op, res.second, fresh);
        CHECK(res.first->second == ref[k], "round %d op %d: emplace value %u, model %u", round, op, res.first->second, ref[k]);
      } else if (r < 80) {   // lookup, of a key that is there or was or never was
        const long long k = next_key - 1 - (long long)(rng() % (uint64_t)(std::max<long long>(1, (long long)ref.size() + 5)));
        const std::string ks = (rng() % 15 == 0) ? std::string("t") + std::to_string(k) + '\x1f' : ik(k);
        auto it = m.find(ks);
        auto rf = ref.find(ks);
        CHECK((it != m.end()) == (rf != ref.end()), "round %d op %d: find %s found=%d, model %d", round, op, ks.c_str(), it != m.end(), rf != ref.end());
        if (it != m.end() && rf != ref.end()) CHECK(it->second == rf->second, "round %d op %d: find value %u, model %u", round, op, it->second, rf->second);
      } else if (r < 90) {   // erase
        const long long k = next_key - 1 - (long long)(rng() % (uint64_t)(std::max<long long>(1, (long long)ref.size())));
        const std::string ks = ik(k);
        m.erase(ks);
        ref.erase(ks);
      } else if (r < 93) {   // renumber every id: a monotonic map (the row index folding its gaps)
        const uint32_t cut = next_val / 2;
        auto f = [cut](size_t v) { return v > cut ? v - 1 : v; };
        m.remap_values(f);
        for (auto& e : ref) e.second = (uint32_t)f(e.second);
        if (next_val > 0) --next_val;
      } else if (r < 96 && hostility >= 3) {   // a key before the last (re-insert after a delete, an import out of order)
        const long long k = next_key - 1 - (long long)(rng() % 50);
        const std::string ks = ik(k);
        const uint32_t v = next_val++;
        auto res = m.emplace(ks, v);
        const bool fresh = ref.emplace(ks, v).second;
        CHECK(res.second == fresh, "round %d op %d: out-of-order emplace new=%d, model %d", round, op, res.second, fresh);
      } else if (r < 98 && hostility == 4) {   // a write through operator[] (an UPDATE that changes the key)
        const std::string ks = ik(next_key + 1000 + (long long)(rng() % 100));
        const uint32_t v = (uint32_t)(rng() % 100000);
        m[ks] = v;
        ref[ks] = v;
      } else if (hostility == 4) {   // a text key
        const std::string ks = "k" + std::to_string(rng() % 1000) + '\x1f' + "x" + '\x1f';
        const uint32_t v = next_val++;
        m.emplace(ks, v);
        ref.emplace(ks, v);
      }
      CHECK(m.size() == ref.size(), "round %d op %d: size %zu, model %zu", round, op, m.size(), ref.size());
      ++checks;
    }
    // Every model entry is found with its id, and nothing else is.
    for (const auto& e : ref) {
      auto it = m.find(e.first);
      CHECK(it != m.end() && it->second == e.second, "round %d: final find of a model key", round);
    }
    if (m.runs_mode()) ++ended_in_runs;
  }
  g_pairs = false;
  OK(failures == 0, "400 random sequences, %ld steps, every answer the model's (%d ended still in runs)", checks, ended_in_runs);
  OK(ended_in_runs > 100, "the tame sequences stayed in runs: %d", ended_in_runs);

  // ---- an auto-increment table ----
  {
    PkMap m;
    for (long long k = 1; k <= 1000000; ++k) m.emplace(ik(k), (size_t)(k - 1));
    OK(m.runs_mode() && m.size() == 1000000, "1M in-order keys: runs");
    OK(m.memory_bytes() < 4096, "and %zu bytes of map (slots: ~24 MB)", m.memory_bytes());
    auto it = m.find(ik(777777));
    OK(it != m.end() && it->second == 777776 && m.find(ik(0)) == m.end() && m.find(ik(1000001)) == m.end(), "lookups in and out of range");
    for (long long k = 500000; k < 500100; ++k) m.erase(ik(k));
    OK(m.runs_mode() && m.size() == 999900 && m.find(ik(500050)) == m.end() && m.find(ik(500100))->second == 500099,
       "100 deletes: still runs, the deleted keys gone, the rest unmoved");
    // The row index folds its gaps: ids above the deleted ones shift down by 100.
    m.remap_values([](size_t v) { return v >= 500099 ? v - 100 : v; });
    OK(m.runs_mode() && m.find(ik(500100))->second == 499999 && m.find(ik(1000000))->second == 999899 && m.find(ik(10))->second == 9,
       "renumbered: runs again (%zu bytes)", m.memory_bytes());
    // Keys with gaps (MariaDB's ids after old deletes): one run per gap.
    PkMap g;
    for (long long k = 1; k <= 100000; ++k) g.emplace(ik(k * 3), (size_t)k);
    OK(g.runs_mode() && g.memory_bytes() <= 100000 * 16 * 3 / 2 && g.find(ik(300))->second == 100 && g.find(ik(301)) == g.end(),
       "every third key, each its own id: a run a key, still runs (%zu bytes for 100k keys; slots ~2.4 MB)", g.memory_bytes());
  }

  // ---- two-integer keys (an order's lines): packed, and runs when they arrive in order ----
  {
    auto pk2 = [](long long a, long long b) { return std::to_string(a) + '\x1f' + std::to_string(b) + '\x1f'; };
    PkMap m;
    std::map<std::string, uint32_t> ref;
    uint32_t row = 0;
    for (long long order = 1000; order < 101000; ++order)
      for (long long line = 0; line < 1 + order % 4; ++line) { m.emplace(pk2(order, line), row); ref[pk2(order, line)] = row; ++row; }
    OK(m.runs_mode() && m.size() == ref.size(), "%zu (order, line) keys in order: runs", m.size());
    // A run per order (the next order's line 0 does not follow the last line): ~8 bytes a key, a third of slots.
    OK(m.memory_bytes() < ref.size() * 12, "and %zu bytes, %.1f a key (slots: ~24 a key)", m.memory_bytes(), (double)m.memory_bytes() / ref.size());
    size_t bad = 0;
    for (const auto& e : ref) { auto it = m.find(e.first); if (it == m.end() || it->second != e.second) ++bad; }
    OK(bad == 0 && m.find(pk2(1000, 7)) == m.end() && m.find(pk2(999, 0)) == m.end() && m.find(std::to_string(1000) + '\x1f') == m.end(),
       "every pair found with its row, absent pairs and a single key not");
    m.erase(pk2(5000, 1));
    OK(m.find(pk2(5000, 1)) == m.end() && m.find(pk2(5000, 0)) != m.end(), "an erased pair is gone, its neighbour stays");
    // Shapes mixed, or a pair past the packing: text, everything still found.
    PkMap x;
    for (long long k = 0; k < 500; ++k) x.emplace(pk2(k, 1), (size_t)k);
    x.emplace(std::to_string(7) + '\x1f', 900);
    bool all = x.find(std::to_string(7) + '\x1f')->second == 900;
    for (long long k = 0; k < 500; ++k) { auto it = x.find(pk2(k, 1)); all = all && it != x.end() && it->second == (uint32_t)k; }
    OK(!x.runs_mode() && all, "a single-integer key among pairs: text, every key found");
    PkMap y;
    y.emplace(pk2(1, 1), 1);
    y.emplace(pk2(2, 1u << 25), 2);   // second part past 24 bits
    OK(y.find(pk2(2, 1u << 25))->second == 2 && y.find(pk2(1, 1))->second == 1, "a pair past the packing range: text, both found");
    PkMap z;
    z.emplace(pk2(-1, 1), 5);   // a negative part is not packed
    OK(z.find(pk2(-1, 1)) != z.end() && z.find(pk2(-1, 1))->second == 5, "a negative part: text, found");
  }

  // ---- what turns it into slots ----
  {
    PkMap a; for (int k = 0; k < 1000; ++k) a.emplace(ik(k), (size_t)k);
    a.emplace("abc\x1f", 5000);
    OK(!a.runs_mode() && a.find(ik(10))->second == 10 && a.find("abc\x1f")->second == 5000, "a text key: slots, everything still found");
    PkMap b; for (int k = 0; k < 1000; ++k) b.emplace(ik(k), (size_t)k);
    b.emplace(ik(-5), 7000);
    OK(b.runs_mode() && b.find(ik(-5))->second == 7000 && b.find(ik(999))->second == 999, "a key before the last: kept aside, still runs");
    PkMap c; for (int k = 0; k < 1000; ++k) c.emplace(ik(k), (size_t)k);
    c[ik(5)] = 99;
    OK(c.runs_mode() && c.find(ik(5))->second == 99 && c.find(ik(4))->second == 4 && c.find(ik(6))->second == 6 && c.size() == 1000,
       "a write through operator[]: the write kept, its neighbours unmoved, still runs");
    PkMap d; for (int k = 0; k < 1000; ++k) d.emplace(ik(k), (size_t)k);
    size_t walked = 0;
    for (auto e = d.begin(); e != d.end(); ++e) ++walked;
    OK(!d.runs_mode() && walked == 1000, "a walk with begin(): slots, every entry walked");
    PkMap e; for (int k = 0; k < 1000; ++k) e.emplace(ik(k), (size_t)k);
    auto again = e.emplace(ik(500), 1);
    OK(e.runs_mode() && !again.second && again.first->second == 500, "emplacing a key already there: unchanged, still runs");
  }

  // ---- a day of replication: lines added to old orders, keys deleted, keys re-pointed - the map stays runs ----
  // (atl's maps had all turned into slots this way: 340 MB for 8.8M keys that cost ~2 MB as runs.)
  {
    auto pk2 = [](long long a, long long b) { return std::to_string(a) + '\x1f' + std::to_string(b) + '\x1f'; };
    PkMap m;
    std::map<std::string, uint32_t> ref;
    uint32_t row = 0;
    for (long long order = 1; order <= 200000; ++order)
      for (long long line = 1; line <= 1 + order % 3; ++line) { m.emplace(pk2(order, line), row); ref[pk2(order, line)] = row; ++row; }
    const size_t loaded = m.memory_bytes();
    std::mt19937_64 rng(99);
    // 50k lines added to orders already there: every one of them a key before the last.
    for (int i = 0; i < 50000; ++i) {
      const long long order = 1 + (long long)(rng() % 200000);
      const long long line = 10 + (long long)(rng() % 5);
      if (m.emplace(pk2(order, line), row).second) { ref[pk2(order, line)] = row; ++row; }
    }
    OK(m.runs_mode() && m.size() == ref.size(), "50k lines added to old orders: still runs, %zu keys", m.size());
    // 30% of the keys deleted (more than the quarter that used to turn the map into slots).
    size_t deleted = 0;
    for (auto it = ref.begin(); it != ref.end();) {
      if (rng() % 10 < 3) { m.erase(it->first); it = ref.erase(it); ++deleted; } else ++it;
    }
    OK(m.runs_mode() && m.size() == ref.size(), "%zu keys deleted: still runs", deleted);
    // Keys re-pointed at other rows (an UPDATE that moves a row).
    for (int i = 0; i < 20000; ++i) {
      auto it = ref.begin();
      std::advance(it, (long)(rng() % ref.size()));
      m[it->first] = it->second + 1000000u;
      it->second += 1000000u;
    }
    OK(m.runs_mode(), "20k keys re-pointed through operator[]: still runs");
    size_t bad = 0;
    for (const auto& e : ref) { auto it = m.find(e.first); if (it == m.end() || it->second != e.second) ++bad; }
    size_t ghosts = 0;
    for (long long order = 1; order <= 2000; ++order) for (long long line = 1; line <= 14; ++line)
      if (!ref.count(pk2(order, line)) && m.find(pk2(order, line)) != m.end()) ++ghosts;
    OK(bad == 0 && ghosts == 0, "every key answers the model's row (%zu checked), no deleted key answers", ref.size());
    OK(m.memory_bytes() < ref.size() * 16,
       "and %zu bytes for %zu keys (%.1f a key; loaded %.1f; slots ~24, and 53 before this)", m.memory_bytes(), ref.size(), (double)m.memory_bytes() / ref.size(), (double)loaded / (row - 50000));
  }

  std::printf(failures ? "pkmap runs check: %d failure(s)\n" : "pkmap runs check: all passed\n", failures);
  return failures ? 1 : 0;
}
