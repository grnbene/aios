/* grain_plugin.h - add natives to grain from C or C++ without rebuilding it.
 *
 * Build a shared library that exports `grain_plugin_init`, then in a program:
 *
 *     load_plugin("./libmine.so")
 *     print(dot(bytes_f64s...))        // whatever the plugin registered
 *
 * A plugin sees grain only through the table it is handed, so it needs no
 * symbols from the grain executable and no grain headers beyond this one. The
 * interface is plain C, versioned (GRAIN_PLUGIN_ABI), and a plugin built
 * against an older version keeps working.
 *
 *     #include "grain_plugin.h"
 *
 *     static void add(grain_call* c) {
 *       if (grain_arg_type(c, 0) != GRAIN_INT || grain_arg_type(c, 1) != GRAIN_INT) {
 *         grain_error(c, "add() wants two whole numbers");
 *         return;
 *       }
 *       grain_return_int(c, grain_arg_int(c, 0) + grain_arg_int(c, 1));
 *     }
 *
 *     GRAIN_PLUGIN(reg) { grain_register(reg, "add", 2, 2, add); return 0; }
 *
 * Strings and bytes handed to a call are valid until it returns; copy what you
 * keep. A byte buffer is the buffer itself, not a copy: writing through it fills
 * the grain program's `bytes`. A call that sets no return value returns nil.
 * Calls run on the grain VM's thread: do not block it, and do not call these
 * functions from any other thread.
 */
#ifndef GRAIN_PLUGIN_H
#define GRAIN_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRAIN_PLUGIN_ABI 1

typedef struct grain_call grain_call;
typedef struct grain_registry grain_registry;
typedef void (*grain_native_fn)(grain_call*);

enum grain_type {
  GRAIN_NIL = 0, GRAIN_BOOL, GRAIN_INT, GRAIN_REAL, GRAIN_STR, GRAIN_BYTES, GRAIN_LIST, GRAIN_MAP, GRAIN_OTHER
};

/* The table the host hands to grain_plugin_init. Fields are only ever added at
 * the end, so a plugin checks `abi` and `size` before using a newer one. */
typedef struct grain_api {
  uint32_t abi;
  uint32_t size;
  int (*argc)(grain_call*);
  int (*arg_type)(grain_call*, int i);
  int64_t (*arg_int)(grain_call*, int i);
  double (*arg_real)(grain_call*, int i);
  const char* (*arg_str)(grain_call*, int i, size_t* len);
  uint8_t* (*arg_bytes)(grain_call*, int i, size_t* len);
  size_t (*arg_list_len)(grain_call*, int i);
  int (*arg_list_type)(grain_call*, int i, size_t k);
  int64_t (*arg_list_int)(grain_call*, int i, size_t k);
  double (*arg_list_real)(grain_call*, int i, size_t k);
  void (*return_nil)(grain_call*);
  void (*return_bool)(grain_call*, int v);
  void (*return_int)(grain_call*, int64_t v);
  void (*return_real)(grain_call*, double v);
  void (*return_str)(grain_call*, const char* s, size_t len);
  void (*return_bytes)(grain_call*, const uint8_t* p, size_t len);
  void (*error)(grain_call*, const char* message);
  int (*reg)(grain_registry*, const char* name, int min_args, int max_args, grain_native_fn fn);
} grain_api;

/* Set by GRAIN_PLUGIN before grain_plugin_init runs; one per plugin library. */
static const grain_api* grain__api;

static inline int grain_argc(grain_call* c) { return grain__api->argc(c); }
static inline int grain_arg_type(grain_call* c, int i) { return grain__api->arg_type(c, i); }
static inline int64_t grain_arg_int(grain_call* c, int i) { return grain__api->arg_int(c, i); }
static inline double grain_arg_real(grain_call* c, int i) { return grain__api->arg_real(c, i); }
static inline const char* grain_arg_str(grain_call* c, int i, size_t* len) { return grain__api->arg_str(c, i, len); }
static inline uint8_t* grain_arg_bytes(grain_call* c, int i, size_t* len) { return grain__api->arg_bytes(c, i, len); }
static inline size_t grain_arg_list_len(grain_call* c, int i) { return grain__api->arg_list_len(c, i); }
static inline int grain_arg_list_type(grain_call* c, int i, size_t k) { return grain__api->arg_list_type(c, i, k); }
static inline int64_t grain_arg_list_int(grain_call* c, int i, size_t k) { return grain__api->arg_list_int(c, i, k); }
static inline double grain_arg_list_real(grain_call* c, int i, size_t k) { return grain__api->arg_list_real(c, i, k); }
static inline void grain_return_nil(grain_call* c) { grain__api->return_nil(c); }
static inline void grain_return_bool(grain_call* c, int v) { grain__api->return_bool(c, v); }
static inline void grain_return_int(grain_call* c, int64_t v) { grain__api->return_int(c, v); }
static inline void grain_return_real(grain_call* c, double v) { grain__api->return_real(c, v); }
static inline void grain_return_str(grain_call* c, const char* s, size_t len) { grain__api->return_str(c, s, len); }
static inline void grain_return_bytes(grain_call* c, const uint8_t* p, size_t len) { grain__api->return_bytes(c, p, len); }
static inline void grain_error(grain_call* c, const char* message) { grain__api->error(c, message); }
static inline int grain_register(grain_registry* r, const char* name, int min_args, int max_args, grain_native_fn fn) {
  return grain__api->reg(r, name, min_args, max_args, fn);
}

/* Declares the entry point. The body returns 0, or nonzero to refuse to load. */
#define GRAIN_PLUGIN(registry_name)                                                       \
  static int grain__init(grain_registry* registry_name);                                  \
  GRAIN_PLUGIN_C GRAIN_PLUGIN_EXPORT int grain_plugin_init(const grain_api* api, grain_registry* r) { \
    if (!api || api->abi < GRAIN_PLUGIN_ABI) return 1;                                    \
    grain__api = api;                                                                     \
    return grain__init(r);                                                                \
  }                                                                                       \
  static int grain__init(grain_registry* registry_name)

#ifdef __cplusplus
#define GRAIN_PLUGIN_C extern "C"
#else
#define GRAIN_PLUGIN_C
#endif

#if defined(_WIN32)
#define GRAIN_PLUGIN_EXPORT __declspec(dllexport)
#else
#define GRAIN_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif /* GRAIN_PLUGIN_H */
