// PkMap is the primary-key map of a row index. Every key of a text-mode map used to be held twice - a slot and a
// std::string beside it - which is what a 20M-row composite-key table (stockmovestaxes) cost: ~100+ bytes a row,
// 6 GB across the tables of a POS order. This drives it against std::unordered_map through inserts, lookups of
// present and absent keys, erases, re-inserts and the integer -> text switch, then says what a key costs.
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>

#include "aios/userland/sql_database.hpp"

using namespace aios::userland;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}
std::string int_key(uint64_t n) { return std::to_string(n) + '\x1f'; }
std::string comp_key(uint64_t a, uint64_t b) { return std::to_string(a) + '\x1f' + std::to_string(b) + '\x1f'; }
std::string text_key(uint64_t n) { return "A" + std::to_string(n % 100000) + "-LOC" + std::to_string(n / 100000) + '\x1f'; }

// Everything the map says matches the reference: size, every present key's value, absent keys absent.
bool same(PkMap& m, const std::unordered_map<std::string, uint32_t>& ref, const std::vector<std::string>& absent) {
  if (m.size() != ref.size()) return false;
  for (const auto& kv : ref) {
    auto it = m.find(kv.first);
    if (it == m.end() || it->second != kv.second) return false;
  }
  for (const auto& k : absent) if (m.find(k) != m.end() && !ref.count(k)) return false;
  size_t walked = 0;
  for (auto it = m.begin(); it != m.end(); ++it) ++walked;
  return walked == ref.size();
}
}  // namespace

int main() {
  std::mt19937_64 rng(42);

  {  // Integer keys: the 16-byte-a-slot form.
    PkMap m;
    std::unordered_map<std::string, uint32_t> ref;
    std::vector<std::string> absent;
    for (uint64_t i = 0; i < 300000; ++i) { m.emplace(int_key(i * 3), (size_t)i); ref[int_key(i * 3)] = (uint32_t)i; absent.push_back(int_key(i * 3 + 1)); }
    for (uint64_t i = 0; i < 300000; i += 7) { m.erase(int_key(i * 3)); ref.erase(int_key(i * 3)); }
    check(same(m, ref, absent), "integer keys: inserts, erases and absent lookups match std::unordered_map");
  }

  {  // Composite keys (stockmovestaxes: stkmoveno, taxauthid) - text mode.
    PkMap m;
    std::unordered_map<std::string, uint32_t> ref;
    std::vector<std::string> absent;
    for (uint64_t i = 0; i < 300000; ++i) { m.emplace(comp_key(i, i % 5), (size_t)i); ref[comp_key(i, i % 5)] = (uint32_t)i; absent.push_back(comp_key(i, 9)); }
    for (uint64_t i = 0; i < 300000; i += 5) { m.erase(comp_key(i, i % 5)); ref.erase(comp_key(i, i % 5)); }
    for (uint64_t i = 0; i < 300000; i += 10) { m.emplace(comp_key(i, i % 5), (size_t)(i + 1)); ref[comp_key(i, i % 5)] = (uint32_t)(i + 1); }
    check(same(m, ref, absent), "composite keys: inserts, erases, re-inserts and absent lookups match");
    auto dup = m.emplace(comp_key(1, 1), 99);
    check(!dup.second && dup.first->second == 1, "emplace of an existing composite key keeps the old entry");
  }

  {  // Integers first, then a key that is not one: the map switches to text with the earlier keys intact.
    PkMap m;
    std::unordered_map<std::string, uint32_t> ref;
    std::vector<std::string> absent;
    for (uint64_t i = 0; i < 100000; ++i) { m.emplace(int_key(i), (size_t)i); ref[int_key(i)] = (uint32_t)i; }
    m.emplace(std::string("SKU-1\x1f"), 777); ref["SKU-1\x1f"] = 777;
    for (uint64_t i = 100000; i < 200000; ++i) { m.emplace(int_key(i), (size_t)i); ref[int_key(i)] = (uint32_t)i; absent.push_back(int_key(i + 1000000)); }
    check(same(m, ref, absent), "integer keys survive the switch to text and keep working");
  }

  {  // Random mixed operations against the reference.
    PkMap m;
    std::unordered_map<std::string, uint32_t> ref;
    bool ok = true;
    for (int step = 0; step < 1500000 && ok; ++step) {
      const uint64_t k = rng() % 60000;
      const std::string key = text_key(k);
      switch (rng() % 4) {
        case 0: case 1: { auto r = m.emplace(key, (size_t)step); const bool fresh = ref.emplace(key, (uint32_t)step).second; ok = r.second == fresh; break; }
        case 2: { m.erase(key); ref.erase(key); break; }
        default: { auto it = m.find(key); auto jt = ref.find(key); ok = (it == m.end()) == (jt == ref.end()) && (it == m.end() || it->second == jt->second); }
      }
    }
    check(ok && m.size() == ref.size(), "1.5M random insert / erase / find steps on text keys agree with the reference");
  }

  {  // What a key costs.
    const size_t n = 2000000;
    PkMap comp, txt, ints;
    comp.reserve(n); txt.reserve(n); ints.reserve(n);
    for (uint64_t i = 0; i < n; ++i) { comp.emplace(comp_key(12000000 + i, 14), (size_t)i); txt.emplace(text_key(i), (size_t)i); ints.emplace(int_key(i), (size_t)i); }
    const double cb = (double)comp.memory_bytes() / (double)n, tb = (double)txt.memory_bytes() / (double)n, ib = (double)ints.memory_bytes() / (double)n;
    std::printf("bytes a key (sized for 2M): integer %.1f, composite %.1f, text %.1f\n", ib, cb, tb);
    check(cb <= 64.0, "a composite key costs at most 64 bytes");
    check(tb <= 64.0, "a text key costs at most 64 bytes");
  }

  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
