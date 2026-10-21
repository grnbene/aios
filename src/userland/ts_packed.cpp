// The unpacker behind the packed tree-sitter grammars (third_party/tree-sitter-grammars/*/parser_packed.c, made by
// scripts/pack-ts-grammars.py). A grammar's big tables are stored as byte-plane-shuffled zlib streams; the first call
// to its tree_sitter_<name>() inflates each into heap memory that lives for the rest of the process.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "aios/userland/git.hpp"

extern "C" int aios_ts_unpack(const unsigned char* z, unsigned long zlen, unsigned long rawlen, unsigned stride, void* out) {
  if (!z || !out || stride == 0 || rawlen % stride != 0) return 0;
  auto inflated = aios::userland::zlib_inflate(aios::ByteSpan(z, static_cast<size_t>(zlen)));
  if (!inflated.ok() || inflated->size() != rawlen) return 0;
  // undo the shuffle: plane k holds byte k of every element
  const size_t count = rawlen / stride;
  const uint8_t* planes = inflated->data();
  uint8_t* dst = static_cast<uint8_t*>(out);
  if (stride == 1) {
    std::memcpy(dst, planes, rawlen);
    return 1;
  }
  for (unsigned k = 0; k < stride; ++k) {
    const uint8_t* plane = planes + static_cast<size_t>(k) * count;
    for (size_t i = 0; i < count; ++i) dst[i * stride + k] = plane[i];
  }
  return 1;
}

// The once-only guard for a grammar's tables. `state` is a plain int in the generated C: 0 not started, 1 being
// unpacked, 2 ready. A second thread arriving while the first unpacks waits (a few milliseconds at worst).
extern "C" int aios_ts_begin(int* state) {
  static_assert(sizeof(std::atomic<int>) == sizeof(int) && std::atomic<int>::is_always_lock_free, "the guard is a plain lock-free int");
  auto* s = reinterpret_cast<std::atomic<int>*>(state);
  for (;;) {
    int cur = s->load(std::memory_order_acquire);
    if (cur == 2) return 0;
    int zero = 0;
    if (cur == 0 && s->compare_exchange_strong(zero, 1, std::memory_order_acq_rel, std::memory_order_acquire)) return 1;
  }
}

extern "C" void aios_ts_end(int* state) { reinterpret_cast<std::atomic<int>*>(state)->store(2, std::memory_order_release); }

extern "C" const void* aios_ts_alloc(unsigned long bytes) { return std::malloc(bytes ? bytes : 1); }

// A grammar whose tables cannot be unpacked has no way to parse: the tables are the parser. Failing loudly beats a
// parse tree built from garbage.
extern "C" void aios_ts_fail(void) { std::abort(); }
