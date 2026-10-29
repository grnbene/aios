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
  const int rounds = std::getenv("PKMAP_ROUNDS") ? std::atoi(std::getenv("PKMAP_ROUNDS")) : 400;   // more rounds for a soak
  for (int round = 0; round < rounds && failures < 20; ++round) {
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
  OK(failures == 0, "%d random sequences, %ld steps, every answer the model's (%d ended still in runs)", rounds, checks, ended_in_runs);
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
    std::mt19937_64 churn(99);
    // 50k lines added to orders already there: every one of them a key before the last.
    for (int i = 0; i < 50000; ++i) {
      const long long order = 1 + (long long)(churn() % 200000);
      const long long line = 10 + (long long)(churn() % 5);
      if (m.emplace(pk2(order, line), row).second) { ref[pk2(order, line)] = row; ++row; }
    }
    OK(m.runs_mode() && m.size() == ref.size(), "50k lines added to old orders: still runs, %zu keys", m.size());
    // 30% of the keys deleted (more than the quarter that used to turn the map into slots).
    size_t deleted = 0;
    for (auto it = ref.begin(); it != ref.end();) {
      if (churn() % 10 < 3) { m.erase(it->first); it = ref.erase(it); ++deleted; } else ++it;
    }
    OK(m.runs_mode() && m.size() == ref.size(), "%zu keys deleted: still runs", deleted);
    // Keys re-pointed at other rows (an UPDATE that moves a row).
    for (int i = 0; i < 20000; ++i) {
      auto it = ref.begin();
      std::advance(it, (long)(churn() % ref.size()));
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

  // ---- a two-column key that arrives with its second column the slow one: salesorderdetails (orderlineno, orderno)
  // comes line by line within the order, so packed first-column-first every key was a run of its own; the other
  // packing makes an order's lines one run, and the map adopts it at a fold when that halves the runs ----
  {
    auto pk2 = [](long long a, long long b) { return std::to_string(a) + '\x1f' + std::to_string(b) + '\x1f'; };
    PkMap m;
    std::map<std::string, uint32_t> ref;
    uint32_t row = 0;
    for (long long order = 1; order <= 100000; ++order)
      for (long long line = 1; line <= 3; ++line) { m.emplace(pk2(line, order), row); ref[pk2(line, order)] = row; ++row; }
    OK(m.runs_mode() && m.size() == 300000, "300k (line, order) keys, line the fast column: still runs");
    OK(m.memory_bytes() < 100000 * 16 * 3 / 2 + 65536, "and %zu bytes: a run an order, not a run a key (%.1f a key)", m.memory_bytes(), (double)m.memory_bytes() / 300000);
    size_t bad = 0;
    for (const auto& e : ref) { auto it = m.find(e.first); if (it == m.end() || it->second != e.second) ++bad; }
    OK(bad == 0 && m.find(pk2(4, 50)) == m.end() && m.find(pk2(1, 100001)) == m.end(), "every key found with its row after the re-packing, absent ones not");
    m.erase(pk2(2, 777));
    m[pk2(3, 777)] = 5;
    OK(m.find(pk2(2, 777)) == m.end() && m.find(pk2(3, 777))->second == 5 && m.find(pk2(1, 777))->second == ref[pk2(1, 777)], "erase and re-point still work on the re-packed map");
  }

  // ---- folding the gaps after deletes: entries out, ids above them down, still runs ----
  {
    PkMap m;
    for (long long k = 1; k <= 300000; ++k) m.emplace(ik(k), (size_t)(k - 1));
    // rows 100, 101 and 250000 deleted: their entries go, every id above moves down by the rows gone beneath it
    std::vector<size_t> erased{100, 101, 250000};
    const size_t gone = m.remap_or_erase([&](size_t v) -> size_t {
      auto lb = std::lower_bound(erased.begin(), erased.end(), v);
      if (lb != erased.end() && *lb == v) return PkMap::kDrop;
      return v - (size_t)(lb - erased.begin());
    });
    OK(gone == 3 && m.runs_mode() && m.size() == 299997, "3 rows folded out of 300k keys: still runs, %zu keys", m.size());
    OK(m.find(ik(101)) == m.end() && m.find(ik(102)) == m.end() && m.find(ik(250001)) == m.end(), "the deleted rows' keys are gone");
    OK(m.find(ik(100))->second == 99 && m.find(ik(103))->second == 100 && m.find(ik(250002))->second == 249998 && m.find(ik(300000))->second == 299996,
       "the ids above them moved down (103 -> 100, 300000 -> 299996)");
    OK(m.memory_bytes() < 4096, "and %zu bytes of map", m.memory_bytes());
    // (A walk with begin() is what did this before, and it is one-way.)
    PkMap w;
    for (long long k = 1; k <= 1000; ++k) w.emplace(ik(k), (size_t)(k - 1));
    for (auto e = w.begin(); e != w.end(); ++e) {}
    OK(!w.runs_mode(), "a walk with begin() still turns a map into slots (which is why the fold no longer walks)");
  }

  // ---- sorted mode: a map of text keys (a code, a name, a composite with a text column) as a sorted arena ----
  // What it answers must be exactly what the slots answer; only the memory differs.
  {
    unsetenv("AIOS_SQL_PK_SORTED");
    auto tk = [](uint64_t n) {   // "sku-<n>\x1fwh<n%7>\x1f": text, two columns, ~14 B
      return "sku-" + std::to_string(n) + '\x1f' + "wh" + std::to_string(n % 7) + '\x1f';
    };
    // differential: sorted once, then random ops against a std::map, through folds and back to slots
    std::mt19937_64 srng(777);
    long schecks = 0;
    int sorted_rounds = 0;
    const int srounds = std::getenv("PKMAP_ROUNDS") ? std::max(20, std::atoi(std::getenv("PKMAP_ROUNDS")) / 20) : 20;
    for (int round = 0; round < srounds && failures < 20; ++round) {
      PkMap m;
      std::map<std::string, uint32_t> ref;
      const uint64_t n0 = PkMap::kSortedMin + (srng() % 20000);
      uint32_t next_val = 0;
      while (m.size() < n0) {   // scattered keys, some repeats, until the map is big enough to be sorted
        const std::string k = tk(srng() % (n0 * 2));
        if (m.emplace(k, next_val).second) { ref[k] = next_val; ++next_val; }
      }
      const bool sorted = m.sort_text_keys();
      CHECK(sorted, "round %d: %zu text keys sorted", round, m.size());
      if (sorted) ++sorted_rounds;
      const int ops = 500 + (int)(srng() % 20000);   // past 4096 + n/64: folds happen
      for (int op = 0; op < ops; ++op) {
        const int r = (int)(srng() % 100);
        if (r < 35) {   // a new key, or one that is there
          const std::string k = tk(srng() % (n0 * 3));
          auto res = m.emplace(k, next_val);
          const bool fresh = ref.emplace(k, next_val).second;
          CHECK(res.second == fresh, "sorted round %d op %d: emplace new=%d, model %d", round, op, res.second, fresh);
          CHECK(res.first->second == ref[k], "sorted round %d op %d: emplace value %u, model %u", round, op, res.first->second, ref[k]);
          if (fresh) ++next_val;
        } else if (r < 65) {   // lookup
          const std::string k = (srng() % 10 == 0) ? "zz" + std::to_string(srng() % 100) + '\x1f' : tk(srng() % (n0 * 3));
          auto it = m.find(k);
          auto rf = ref.find(k);
          CHECK((it != m.end()) == (rf != ref.end()), "sorted round %d op %d: find found=%d, model %d", round, op, it != m.end(), rf != ref.end());
          if (it != m.end() && rf != ref.end()) CHECK(it->second == rf->second, "sorted round %d op %d: find value %u, model %u", round, op, it->second, rf->second);
        } else if (r < 80) {   // erase by key, or through the iterator of a find
          const std::string k = tk(srng() % (n0 * 3));
          if (srng() % 2) { m.erase(k); }
          else { auto it = m.find(k); if (it != m.end()) m.erase(it); }
          ref.erase(k);
        } else if (r < 92) {   // a write through operator[]: a re-point, or a new key
          const std::string k = tk(srng() % (n0 * 3));
          const uint32_t v = (uint32_t)(srng() % 1000000);
          m[k] = v;
          ref[k] = v;
        } else if (r < 96) {   // renumber every id
          const uint32_t cut = next_val / 2;
          auto f = [cut](size_t v) { return v > cut ? v - 1 : v; };
          m.remap_values(f);
          for (auto& e : ref) e.second = (uint32_t)f(e.second);
        } else {   // fold the gaps: ids drop out and the rest move down
          std::vector<size_t> erased;
          for (int i = 0; i < 3; ++i) erased.push_back(srng() % std::max<uint32_t>(1, next_val));
          std::sort(erased.begin(), erased.end());
          erased.erase(std::unique(erased.begin(), erased.end()), erased.end());
          auto f = [&](size_t v) -> size_t {
            auto lb = std::lower_bound(erased.begin(), erased.end(), v);
            if (lb != erased.end() && *lb == v) return PkMap::kDrop;
            return v - (size_t)(lb - erased.begin());
          };
          const size_t gone = m.remap_or_erase(f);
          size_t model_gone = 0;
          for (auto it = ref.begin(); it != ref.end();) {
            const size_t nv = f(it->second);
            if (nv == PkMap::kDrop) { it = ref.erase(it); ++model_gone; } else { it->second = (uint32_t)nv; ++it; }
          }
          CHECK(gone == model_gone, "sorted round %d op %d: remap_or_erase dropped %zu, model %zu", round, op, gone, model_gone);
        }
        CHECK(m.size() == ref.size(), "sorted round %d op %d: size %zu, model %zu", round, op, m.size(), ref.size());
        ++schecks;
      }
      for (const auto& e : ref) {
        auto it = m.find(e.first);
        CHECK(it != m.end() && it->second == e.second, "sorted round %d: final find of a model key", round);
      }
      CHECK(m.sorted_mode(), "sorted round %d: still sorted at the end", round);
      // and back to slots (a walk), still exact
      size_t walked = 0;
      for (auto it = m.begin(); it != m.end(); ++it) ++walked;
      CHECK(!m.sorted_mode() && walked == ref.size(), "sorted round %d: a walk turns it into slots with every key (%zu of %zu)", round, walked, ref.size());
      for (const auto& e : ref) {
        auto it = m.find(e.first);
        CHECK(it != m.end() && it->second == e.second, "sorted round %d: every key found as slots", round);
      }
      CHECK(m.sort_text_keys() && m.sorted_mode(), "sorted round %d: and sorted again", round);
    }
    OK(failures == 0, "sorted: %d random sequences, %ld steps, every answer the model's (%d sorted)", srounds, schecks, sorted_rounds);
    // memory: 1M text keys
    PkMap big;
    for (uint64_t i = 0; i < 1000000; ++i) big.emplace(tk(i * 7 + 3), (size_t)i);
    const size_t as_slots = big.memory_bytes();
    OK(big.sort_text_keys(), "1M text keys sorted");
    const size_t as_sorted = big.memory_bytes();
    OK(as_sorted * 2 < as_slots, "and %zu bytes for 1M keys (%.1f a key; slots were %.1f a key)", as_sorted, as_sorted / 1e6, as_slots / 1e6);
    size_t bad = 0;
    for (uint64_t i = 0; i < 1000000; i += 97) { auto it = big.find(tk(i * 7 + 3)); if (it == big.end() || it->second != i) ++bad; }
    size_t ghosts = 0;
    for (uint64_t i = 0; i < 1000; ++i) if (big.find(tk(i * 7 + 4)) != big.end()) ++ghosts;
    OK(bad == 0 && ghosts == 0, "every key found with its row, absent ones not");
    // too small to be worth it: left as slots; AIOS_SQL_PK_SORTED=0: left as slots
    PkMap small;
    for (uint64_t i = 0; i < 100; ++i) small.emplace(tk(i), (size_t)i);
    OK(!small.sort_text_keys() && !small.sorted_mode(), "a map of 100 keys stays slots");
    PkMap ints;
    for (long long k = 1; k <= 10000; ++k) ints.emplace(ik(k), (size_t)k);
    OK(!ints.sort_text_keys() && ints.runs_mode(), "a map of integer keys is not sorted (it is runs)");
  }

  std::printf(failures ? "pkmap runs check: %d failure(s)\n" : "pkmap runs check: all passed\n", failures);
  return failures ? 1 : 0;
}
