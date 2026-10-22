// An example plugin: a dot product and a byte checksum, natively.
//   c++ -O2 -shared -fPIC -I grain/include grain/examples/plugin/vec.cpp -o libvec.so
//   grain: load_plugin("./libvec.so"); print(dot([1, 2, 3], [4, 5, 6]))
#include <cstddef>
#include <cstdint>

#include "grain_plugin.h"

static void dot(grain_call* c) {
  if (grain_arg_type(c, 0) != GRAIN_LIST || grain_arg_type(c, 1) != GRAIN_LIST) { grain_error(c, "wants two lists"); return; }
  const size_t n = grain_arg_list_len(c, 0);
  if (n != grain_arg_list_len(c, 1)) { grain_error(c, "the lists differ in length"); return; }
  double sum = 0;
  for (size_t k = 0; k < n; ++k) sum += grain_arg_list_real(c, 0, k) * grain_arg_list_real(c, 1, k);
  grain_return_real(c, sum);
}

static void fnv1a(grain_call* c) {
  size_t len = 0;
  const uint8_t* p = grain_arg_bytes(c, 0, &len);
  if (!p && grain_arg_type(c, 0) != GRAIN_BYTES) { grain_error(c, "wants bytes"); return; }
  uint64_t h = 14695981039346656037ull;
  for (size_t k = 0; k < len; ++k) { h ^= p[k]; h *= 1099511628211ull; }
  grain_return_int(c, (int64_t)h);
}

static void upcase(grain_call* c) {   // fills the caller's buffer in place
  size_t len = 0;
  uint8_t* p = grain_arg_bytes(c, 0, &len);
  if (!p && grain_arg_type(c, 0) != GRAIN_BYTES) { grain_error(c, "wants bytes"); return; }
  for (size_t k = 0; k < len; ++k) if (p[k] >= 'a' && p[k] <= 'z') p[k] -= 32;
  grain_return_int(c, (int64_t)len);
}

GRAIN_PLUGIN(reg) {
  grain_register(reg, "dot", 2, 2, dot);
  grain_register(reg, "fnv1a", 1, 1, fnv1a);
  grain_register(reg, "upcase", 1, 1, upcase);
  return 0;
}
