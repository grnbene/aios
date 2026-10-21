/* Calibration kernels, compiled verbatim into the Pi kernel (`bench` on the
 * console) and into a host program (metal/tools/bench-host.sh), so the two
 * machines run the same instructions and the ratio between them is the
 * hardware's - anything grain loses beyond that ratio is ours to find.
 *
 * Each kernel returns a checksum so the optimizer cannot delete it. The
 * includer supplies bench_now_ns() and bench_report(name, ns, checksum). */
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>

namespace bench_core {

/* Called through a volatile pointer, so the compiler must make every call -
 * it otherwise folds the recursion (and elides malloc/free pairs). */
long cfib_impl(long n);
inline long (*volatile cfib)(long) = cfib_impl;
inline long cfib_impl(long n) { return n < 2 ? n : cfib(n - 1) + cfib(n - 2); }
inline void* (*volatile vmalloc)(size_t) = std::malloc;
inline void (*volatile vfree)(void*) = std::free;

inline void run_all() {
  volatile long sink = 0;
  auto time = [&](const char* name, auto&& fn) {
    const uint64_t t0 = bench_now_ns();
    const long r = fn();
    const uint64_t t1 = bench_now_ns();
    sink = sink + r;
    bench_report(name, t1 - t0, r);
  };

  time("c fib(27)", [] { return cfib(27); });

  time("int loop 50M", [] {
    unsigned long x = 1;
    for (unsigned long i = 0; i < 50000000ul; ++i) x = x * 2862933555777941757ul + 3037000493ul;
    return static_cast<long>(x & 0xffff);
  });

  time("double loop 20M", [] {
    double x = 1.0;
    for (int i = 0; i < 20000000; ++i) x = x * 1.0000001 + 0.5 / (x + 1.0);
    return static_cast<long>(x);
  });

  time("memcpy 64MB", [] {
    static char a[1 << 20], b[1 << 20];
    long s = 0;
    for (int i = 0; i < 64; ++i) { std::memcpy(i & 1 ? a : b, i & 1 ? b : a, sizeof a); s += a[i]; }
    return s;
  });

  time("malloc/free 1M", [] {
    long s = 0;
    for (int i = 0; i < 1000000; ++i) { char* p = static_cast<char*>(vmalloc(16 + (i & 63))); p[0] = static_cast<char>(i); s += p[0]; vfree(p); }
    return s;
  });

  time("shared_ptr copy 10M", [] {
    auto p = std::make_shared<long>(7);
    long s = 0;
    for (int i = 0; i < 10000000; ++i) { std::shared_ptr<long> q = p; s += *q; }
    return s;
  });

  time("string map find 2M", [] {
    std::unordered_map<std::string, long> m;
    const char* names[] = {"fib", "print", "str", "range", "len", "clock", "int", "map"};
    for (long i = 0; i < 8; ++i) m[names[i]] = i;
    const std::string key = "fib";
    long s = 0;
    for (int i = 0; i < 2000000; ++i) s += m.find(key)->second;
    return s;
  });

  time("vector push/pop 20M", [] {
    std::vector<long> v; v.reserve(64);
    long s = 0;
    for (int i = 0; i < 20000000; ++i) { v.push_back(i); if (v.size() > 32) { s += v.back(); v.pop_back(); v.pop_back(); } }
    return s + static_cast<long>(v.size());
  });
}

}  // namespace bench_core
