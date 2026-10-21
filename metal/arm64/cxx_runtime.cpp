/* What libstdc++.a needs from "the system" that krt.cpp did not already
 * provide under these exact libc-ABI names. krt.cpp already has memcpy,
 * memset, memmove, memcmp, strlen, strcmp, strncmp, __cxa_atexit,
 * __dso_handle and __cxa_pure_virtual - duplicating any of those here is a
 * link error, not a fix, so this deliberately does not redeclare them.
 *
 * This is the ARM64 half of the trick metal/runtime.cpp already proved on
 * x86 bare metal: link a real, unmodified libstdc++.a against a freestanding
 * kernel by supplying the handful of C-ABI primitives it assumes exist
 * (memory/string ops, a heap, printf/scanf, strtod, libm, and libstdc++'s
 * own throw-points redirected to metal_panic() so the exception-handling
 * machinery those throw-points would otherwise need - gettext, stderr,
 * fwrite, a real pthread - never actually has to run), then build on real
 * STL containers instead of reinventing them. Ported from that file
 * wholesale where it is architecture-neutral; the two genuinely x86-specific
 * pieces (inline `sqrtsd`/`sqrtss` and the paging helper) are the only
 * things rewritten here rather than copied.
 */
#include <cstddef>
#include <cstdint>
#include <new>
#include "mmu.hpp"

extern "C" [[noreturn]] void metal_panic(const char* what);
extern "C" void* memcpy(void* d, const void* s, size_t n);   /* krt.cpp */
extern "C" void* memset(void* d, int c, size_t n);           /* krt.cpp */
extern "C" size_t strlen(const char* s);                     /* krt.cpp */
extern "C" int strcmp(const char* a, const char* b);         /* krt.cpp */
extern "C" uint64_t metal_ticks();                           /* interrupts.cpp - extern "C" there */
extern "C" uint32_t metal_tick_hz();                          /* interrupts.cpp - extern "C" there */
extern "C" void metal_console_write(const char* s, size_t n);

extern "C" {

/* ---- the string.h surface krt.cpp does not already provide --------------- */
void* memchr(const void* s, int c, size_t n) {
  auto* p = static_cast<const uint8_t*>(s);
  for (; n; --n, ++p) if (*p == static_cast<uint8_t>(c)) return const_cast<uint8_t*>(p);
  return nullptr;
}
char* strchr(const char* s, int c) {
  for (;; ++s) { if (*s == static_cast<char>(c)) return const_cast<char*>(s); if (!*s) return nullptr; }
}
size_t strnlen(const char* s, size_t m) { size_t n = 0; while (n < m && s[n]) ++n; return n; }
char* strncpy(char* d, const char* s, size_t n) { char* r = d; while (n && (*d++ = *s++)) --n; while (n--) *d++ = 0; return r; }
char* strdup(const char* s);  // defined below, after malloc

void abort() { metal_panic("abort()"); }
char __libc_single_threaded = 1;
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
// A thread_local with a destructor makes libstdc++ register a teardown
// callback with glibc; there is no glibc here and no thread that ever
// exits (the kernel runs until power-off), so registering nothing is the
// whole job.
int __cxa_thread_atexit_impl(void (*)(void*), void*, void*) { return 0; }

/* ---- heap: one big page-granular block from the existing allocator, then
 * the same size-class free-list scheme metal/runtime.cpp already proved on
 * x86 - metal_alloc_pages() is page-granular and never frees, fine for
 * mapping BARs but wasteful backing every small std::string/std::vector
 * allocation a linked src/ module makes. */
} // extern "C" (namespace below needs C++ linkage)
namespace {
/* Small objects: power-of-two size classes carved from arenas of 32 MB,
 * with free lists. Large ones (>= 256 KiB) go straight to the page
 * allocator, exactly sized, and are returned to it on free.
 *
 * This used to be ONE fixed 32 MB arena for everything, rounding each block
 * up to a power of two: at 1920x1080 the compositor's 8.3 MB frame buffer
 * took a 16 MB block, its background another, and the next window's 2 MB
 * buffer hit "STL heap exhausted" - a panic that only printed to a serial
 * port, so opening the Shell froze the desktop until the watchdog rebooted
 * the board. The page allocator underneath has ~900 MB. */
constexpr size_t kArenaBytes = 32ull * 1024 * 1024;
constexpr size_t kLargeThreshold = 256 * 1024;
constexpr size_t kLargeClass = 63;               /* header marker for page-backed blocks */
uint8_t* g_heap_cur = nullptr;
uint8_t* g_heap_end = nullptr;
void* g_free_lists[48] = {};
size_t g_heap_used = 0;
int size_class(size_t n) { int c = 0; size_t s = 16; while (s < n) { s <<= 1; ++c; } return c; }
void new_arena() {
  void* base = metal_alloc_pages(kArenaBytes);
  if (!base) metal_panic("cxx_runtime: out of memory (no page frames left for a new arena)");
  g_heap_cur = static_cast<uint8_t*>(base);
  g_heap_end = g_heap_cur + kArenaBytes;
}
/* One core at a time in the heap. It was never locked because only core 0
 * ran C++; with the secondary cores up (smp) a render on core 1 and a console
 * command on core 0 can allocate at the same moment. */
volatile int g_heap_lock = 0;
struct HeapLock {
  HeapLock() { while (__atomic_test_and_set(&g_heap_lock, __ATOMIC_ACQUIRE)) asm volatile("yield"); }
  ~HeapLock() { __atomic_clear(&g_heap_lock, __ATOMIC_RELEASE); }
};
}  // namespace
extern "C" {

void* malloc(size_t n) {
  HeapLock lock;
  if (n == 0) n = 1;
  if (n + 16 >= kLargeThreshold) {
    const size_t bytes = (n + 16 + 4095) & ~static_cast<size_t>(4095);
    auto* base = static_cast<uint8_t*>(metal_alloc_pages(bytes));
    if (!base) metal_panic("cxx_runtime: out of memory for a large allocation");
    reinterpret_cast<size_t*>(base)[0] = kLargeClass;
    reinterpret_cast<size_t*>(base)[1] = bytes;
    g_heap_used += bytes;
    return base + 16;
  }
  if (!g_heap_cur) new_arena();
  int c = size_class(n + 16);
  size_t sz = static_cast<size_t>(16) << c;
  void* p = g_free_lists[c];
  if (p) {
    g_free_lists[c] = *static_cast<void**>(p);
  } else {
    uintptr_t a = (reinterpret_cast<uintptr_t>(g_heap_cur) + 15) & ~static_cast<uintptr_t>(15);
    if (a + sz > reinterpret_cast<uintptr_t>(g_heap_end)) {
      new_arena();                               /* the old arena's tail is simply left */
      a = (reinterpret_cast<uintptr_t>(g_heap_cur) + 15) & ~static_cast<uintptr_t>(15);
    }
    p = reinterpret_cast<void*>(a);
    g_heap_cur = reinterpret_cast<uint8_t*>(a + sz);
  }
  *static_cast<size_t*>(p) = static_cast<size_t>(c);
  g_heap_used += sz;
  return static_cast<uint8_t*>(p) + 16;
}
void free(void* q) {
  if (!q) return;
  HeapLock lock;
  void* p = static_cast<uint8_t*>(q) - 16;
  const size_t c = *static_cast<size_t*>(p);
  if (c == kLargeClass) {
    const size_t bytes = static_cast<size_t*>(p)[1];
    g_heap_used -= bytes;
    metal_free_pages(p, bytes);
    return;
  }
  g_heap_used -= static_cast<size_t>(16) << c;
  *static_cast<void**>(p) = g_free_lists[c];
  g_free_lists[c] = p;
}
void* calloc(size_t a, size_t b) { void* p = malloc(a * b); memset(p, 0, a * b); return p; }
void* realloc(void* p, size_t n) {
  if (!p) return malloc(n);
  const size_t c = *reinterpret_cast<size_t*>(static_cast<uint8_t*>(p) - 16);
  const size_t have = (c == kLargeClass) ? reinterpret_cast<size_t*>(static_cast<uint8_t*>(p) - 16)[1] - 16
                                         : (static_cast<size_t>(16) << c) - 16;
  if (n <= have) return p;
  void* q = malloc(n);
  memcpy(q, p, have);
  free(p);
  return q;
}
size_t cxx_runtime_heap_used() { return g_heap_used; }
char* strdup(const char* s) { size_t n = strlen(s) + 1; char* p = static_cast<char*>(malloc(n)); if (p) memcpy(p, s, n); return p; }

/* ---- stdio: libstdc++'s verbose terminate handler and demangler talk to
 * this; route it to the console. The __throw_* redirects further down mean
 * none of this is ever actually reached in practice - it exists so the
 * link succeeds without needing functexcept.o/vterminate.o's own
 * gettext/stderr/fwrite dependencies pulled in unresolved. */
struct _IO_FILE; typedef struct _IO_FILE FILE;
namespace { int fake_stderr; }
FILE* stderr = reinterpret_cast<FILE*>(&fake_stderr);
void* stdout = nullptr;
void* stdin = nullptr;
int fputs(const char* s, FILE*) { metal_console_write(s, strlen(s)); return 0; }
size_t fwrite(const void* p, size_t sz, size_t n, FILE*) { metal_console_write(static_cast<const char*>(p), sz * n); return n; }
int fputc(int c, FILE*) { char ch = static_cast<char>(c); metal_console_write(&ch, 1); return c; }
int fflush(FILE*) { return 0; }
int putchar(int c) { char ch = static_cast<char>(c); metal_console_write(&ch, 1); return c; }
int puts(const char* s) { metal_console_write(s, strlen(s)); metal_console_write("\n", 1); return 0; }
int dl_iterate_phdr(void*, void*) { return 0; }
int _dl_find_object(void*, void*) { return -1; }  /* "no dynamic loader, use registered frames only" */
int sched_yield() { return 0; }
char* getenv(const char*) { return nullptr; }
char* secure_getenv(const char*) { return nullptr; }
/* No process environment on bare metal: setting one fails, honestly, so the
 * grain setenv() builtin reports an error instead of pretending. */
int setenv(const char*, const char*, int) { return -1; }
int unsetenv(const char*) { return -1; }
unsigned long __getauxval(unsigned long) { return 0; }  /* no ELF auxv on this substrate */

unsigned long __isoc23_strtoul(const char* s, char** end, int base) {
  unsigned long v = 0; if (base == 0) base = 10;
  while (*s == ' ') ++s;
  for (;; ++s) {
    int d;
    if (*s >= '0' && *s <= '9') d = *s - '0';
    else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
    else break;
    if (d >= base) break;
    v = v * static_cast<unsigned long>(base) + static_cast<unsigned long>(d);
  }
  if (end) *end = const_cast<char*>(s);
  return v;
}

}  // extern "C"

/* ---- printf: enough of it to be the C library on this substrate. std::
 * to_string(double) is vsnprintf under the hood, and libstdc++'s demangler
 * and terminate handler use sprintf. */
namespace {
struct Sink {
  char* out; size_t cap; size_t n;
  void put(char c) { if (out && n + 1 < cap) out[n] = c; ++n; }
  void puts_(const char* s) { while (*s) put(*s++); }
};

void emit_uint(Sink& s, unsigned long long v, unsigned base, bool upper, int min_digits) {
  char b[32]; int i = 0;
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do { b[i++] = digits[v % base]; v /= base; } while (v && i < 32);
  while (i < min_digits && i < 32) b[i++] = '0';
  while (i) s.put(b[--i]);
}

#define AIOS_FLOAT_FORMAT 1
#include "../float_common.inc"
#undef AIOS_FLOAT_FORMAT

int format_into(char* out, size_t cap, const char* fmt, __builtin_va_list ap) {
  Sink s{out, cap, 0};
  for (const char* f = fmt; *f; ++f) {
    if (*f != '%') { s.put(*f); continue; }
    ++f;
    bool left = false, zero = false, plus = false, space = false, alt = false;
    for (;; ++f) {
      if (*f == '-') left = true;
      else if (*f == '0') zero = true;
      else if (*f == '+') plus = true;
      else if (*f == ' ') space = true;
      else if (*f == '#') alt = true;
      else if (*f == '\'') {}
      else break;
    }
    int width = 0;
    if (*f == '*') { width = __builtin_va_arg(ap, int); ++f; if (width < 0) { left = true; width = -width; } }
    else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    int prec = -1;
    if (*f == '.') {
      ++f; prec = 0;
      if (*f == '*') { prec = __builtin_va_arg(ap, int); ++f; }
      else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
    }
    int lng = 0;
    for (;; ++f) {
      if (*f == 'l') ++lng;
      else if (*f == 'z' || *f == 'j' || *f == 't') lng = 2;
      else if (*f == 'h' || *f == 'L' || *f == 'q') {}
      else break;
    }
    char conv = *f;
    if (!conv) break;
    if (conv == '%') { s.put('%'); continue; }

    if (conv == 's') {
      const char* t = __builtin_va_arg(ap, const char*);
      if (!t) t = "(null)";
      int len = 0; while (t[len] && (prec < 0 || len < prec)) ++len;
      for (int i = len; !left && i < width; ++i) s.put(' ');
      for (int i = 0; i < len; ++i) s.put(t[i]);
      for (int i = len; left && i < width; ++i) s.put(' ');
      continue;
    }

    char scratch[480];   /* %f of a big double is ~310 digits before any precision */
    Sink item{scratch, sizeof scratch, 0};
    switch (conv) {
      case 'c': item.put(static_cast<char>(__builtin_va_arg(ap, int))); break;
      case 'd': case 'i': {
        long long v = lng >= 2 ? __builtin_va_arg(ap, long long) : (lng == 1 ? static_cast<long long>(__builtin_va_arg(ap, long)) : static_cast<long long>(__builtin_va_arg(ap, int)));
        if (v < 0) { item.put('-'); emit_uint(item, static_cast<unsigned long long>(-(v + 1)) + 1ull, 10, false, prec < 0 ? 1 : prec); }
        else { if (plus) item.put('+'); else if (space) item.put(' '); emit_uint(item, static_cast<unsigned long long>(v), 10, false, prec < 0 ? 1 : prec); }
        break;
      }
      case 'u': {
        unsigned long long v = lng >= 2 ? __builtin_va_arg(ap, unsigned long long) : (lng == 1 ? static_cast<unsigned long long>(__builtin_va_arg(ap, unsigned long)) : static_cast<unsigned long long>(__builtin_va_arg(ap, unsigned int)));
        emit_uint(item, v, 10, false, prec < 0 ? 1 : prec);
        break;
      }
      case 'x': case 'X': case 'o': {
        unsigned long long v = lng >= 2 ? __builtin_va_arg(ap, unsigned long long) : (lng == 1 ? static_cast<unsigned long long>(__builtin_va_arg(ap, unsigned long)) : static_cast<unsigned long long>(__builtin_va_arg(ap, unsigned int)));
        emit_uint(item, v, conv == 'o' ? 8u : 16u, conv == 'X', prec < 0 ? 1 : prec);
        break;
      }
      case 'p': {
        item.puts_("0x");
        emit_uint(item, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(__builtin_va_arg(ap, void*))), 16, false, 1);
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        double v = __builtin_va_arg(ap, double);
        if (!__builtin_signbit(v)) { if (plus) item.put('+'); else if (space) item.put(' '); }
        emit_double(item, v, prec, conv, alt);
        break;
      }
      default: item.put('%'); item.put(conv); break;
    }
    size_t len = item.n < sizeof scratch ? item.n : sizeof scratch - 1;
    for (size_t i = len; !left && static_cast<int>(i) < width; ++i) s.put(zero && scratch[0] != '-' ? '0' : ' ');
    for (size_t i = 0; i < len; ++i) s.put(scratch[i]);
    for (size_t i = len; left && static_cast<int>(i) < width; ++i) s.put(' ');
  }
  if (out && cap) out[s.n < cap ? s.n : cap - 1] = 0;
  return static_cast<int>(s.n);
}

bool scan_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

}  // namespace
extern "C" double strtod(const char* s, char** end);
namespace {
int scan_from(const char* s, const char* fmt, __builtin_va_list ap) {
  int assigned = 0;
  while (*fmt) {
    if (scan_space(*fmt)) { while (scan_space(*s)) ++s; ++fmt; continue; }
    if (*fmt != '%') { if (*s != *fmt) return assigned; ++s; ++fmt; continue; }
    ++fmt;
    if (*fmt == '%') { if (*s != '%') return assigned; ++s; ++fmt; continue; }
    bool suppress = false;
    if (*fmt == '*') { suppress = true; ++fmt; }
    int width = 0;
    while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
    int longs = 0;
    while (*fmt == 'l') { ++longs; ++fmt; }
    const char conv = *fmt ? *fmt++ : '\0';

    if (conv == 'c') {
      if (!*s) return assigned;
      if (!suppress) { *__builtin_va_arg(ap, char*) = *s; ++assigned; }
      ++s;
      continue;
    }
    if (conv != 's') while (scan_space(*s)) ++s;

    if (conv == 's') {
      while (scan_space(*s)) ++s;
      if (!*s) return assigned;
      char* dst = suppress ? nullptr : __builtin_va_arg(ap, char*);
      int n = 0;
      while (*s && !scan_space(*s) && (width == 0 || n < width)) { if (dst) dst[n] = *s; ++s; ++n; }
      if (dst) { dst[n] = '\0'; ++assigned; }
      continue;
    }

    const char* start = s;
    bool neg = false;
    if (conv == 'd' || conv == 'f' || conv == 'e' || conv == 'g') { if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; } }
    if (conv == 'd' || conv == 'u' || conv == 'x') {
      const int base = conv == 'x' ? 16 : 10;
      unsigned long long v = 0;
      int digits = 0;
      for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && (*s | 32) >= 'a' && (*s | 32) <= 'f') d = (*s | 32) - 'a' + 10;
        else break;
        if (width && digits >= width) break;
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d); ++s; ++digits;
      }
      if (!digits) { s = start; return assigned; }
      if (!suppress) {
        const long long sv = neg ? -static_cast<long long>(v) : static_cast<long long>(v);
        if (longs >= 2) *__builtin_va_arg(ap, long long*) = sv;
        else if (longs == 1) *__builtin_va_arg(ap, long*) = static_cast<long>(sv);
        else if (conv == 'd') *__builtin_va_arg(ap, int*) = static_cast<int>(sv);
        else *__builtin_va_arg(ap, unsigned*) = static_cast<unsigned>(v);
        ++assigned;
      }
      continue;
    }
    if (conv == 'f' || conv == 'e' || conv == 'g') {
      char* after = nullptr;
      double v = strtod(start, &after);             /* the same correctly rounded parse */
      if (after == start) { s = start; return assigned; }
      s = after;
      (void)neg;
      if (!suppress) { if (longs) *__builtin_va_arg(ap, double*) = v; else *__builtin_va_arg(ap, float*) = static_cast<float>(v); ++assigned; }
      continue;
    }
    return assigned;
  }
  return assigned;
}
}  // namespace

extern "C" {
int vsnprintf(char* out, size_t cap, const char* fmt, __builtin_va_list ap) { return format_into(out, cap, fmt, ap); }
int __vsnprintf_chk(char* out, size_t cap, int, size_t, const char* fmt, __builtin_va_list ap) { return format_into(out, cap, fmt, ap); }
int vsprintf(char* out, const char* fmt, __builtin_va_list ap) { return format_into(out, static_cast<size_t>(-1), fmt, ap); }
int snprintf(char* out, size_t cap, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, cap, fmt, ap); __builtin_va_end(ap); return n; }
int sprintf(char* out, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, static_cast<size_t>(-1), fmt, ap); __builtin_va_end(ap); return n; }
int vsscanf(const char* s, const char* fmt, __builtin_va_list ap) { return scan_from(s, fmt, ap); }
int sscanf(const char* s, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = scan_from(s, fmt, ap); __builtin_va_end(ap); return n; }
int __isoc23_sscanf(const char* s, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = scan_from(s, fmt, ap); __builtin_va_end(ap); return n; }
int __isoc99_sscanf(const char* s, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = scan_from(s, fmt, ap); __builtin_va_end(ap); return n; }
char* getcwd(char* buf, size_t size) { if (!buf || size < 2) return nullptr; buf[0] = '/'; buf[1] = '\0'; return buf; }
int __snprintf_chk(char* out, size_t cap, int, size_t, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, cap, fmt, ap); __builtin_va_end(ap); return n; }
int __sprintf_chk(char* out, int, size_t cap, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, cap, fmt, ap); __builtin_va_end(ap); return n; }
int fprintf(FILE* f, const char* fmt, ...) { char b[512]; __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(b, sizeof b, fmt, ap); __builtin_va_end(ap); fputs(b, f); return n; }
int printf(const char* fmt, ...) { char b[512]; __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(b, sizeof b, fmt, ap); __builtin_va_end(ap); metal_console_write(b, strlen(b)); return n; }

long __isoc23_strtol(const char* s, char** end, int base) {
  while (*s == ' ' || *s == '\t') ++s;
  bool neg = false;
  if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }
  unsigned long v = __isoc23_strtoul(s, end, base);
  return neg ? -static_cast<long>(v) : static_cast<long>(v);
}
long strtol(const char* s, char** end, int base) { return __isoc23_strtol(s, end, base); }
unsigned long strtoul(const char* s, char** end, int base) { return __isoc23_strtoul(s, end, base); }
long long strtoll(const char* s, char** end, int base) { return __isoc23_strtol(s, end, base); }
int atoi(const char* s) { return static_cast<int>(__isoc23_strtol(s, nullptr, 10)); }
long atol(const char* s) { return __isoc23_strtol(s, nullptr, 10); }
long long __isoc23_strtoll(const char* s, char** end, int base) { return __isoc23_strtol(s, end, base); }
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return __isoc23_strtoul(s, end, base); }
unsigned long long strtoull(const char* s, char** end, int base) { return __isoc23_strtoul(s, end, base); }

#define AIOS_FLOAT_PARSE 1
#include "../float_common.inc"
#undef AIOS_FLOAT_PARSE
long double strtold(const char* s, char** end) { return (long double)strtod(s, end); }
float strtof(const char* s, char** end) { return static_cast<float>(strtod(s, end)); }
double atof(const char* s) { return strtod(s, nullptr); }

/* Exception globals live in TLS inside libstdc++; there is one thread here. */
namespace { char cxa_globals[64]; }
void* __cxa_get_globals() { return cxa_globals; }
void* __cxa_get_globals_fast() { return cxa_globals; }
void* __tls_get_addr(void*) { metal_panic("thread-local storage is not available on this substrate"); }
int pthread_mutex_lock(void*) { return 0; }
int pthread_mutex_unlock(void*) { return 0; }
int pthread_once(int*, void (*)(void)) { return 0; }
/* pthread_create/pthread_detach: real implementations in posix_shim.cpp -
 * this used to stub pthread_create as an unconditional failure, which with
 * -fno-exceptions active turns the very first real std::thread(...)
 * construction anywhere in the linked image into a std::terminate, not a
 * graceful "ran nothing". posix_shim.cpp runs the body synchronously instead
 * (see its own comment for why not dispatched to a second SMP core: this
 * substrate's __tls_get_addr() panics, below). */

wchar_t* wmemcpy(wchar_t* d, const wchar_t* s, size_t n) { auto* a = d; auto* b = s; while (n--) *a++ = *b++; return d; }
wchar_t* wmemmove(wchar_t* d, const wchar_t* s, size_t n) { auto* a = d; auto* b = s; if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; } return d; }
wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n) { for (; n; --n, ++s) if (*s == c) return const_cast<wchar_t*>(s); return nullptr; }
size_t wcslen(const wchar_t* s) { size_t n = 0; while (s[n]) ++n; return n; }
wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n) { auto* p = s; while (n--) *p++ = c; return s; }
wchar_t* __wmemset_chk(wchar_t* s, wchar_t c, size_t n, size_t) { return wmemset(s, c, n); }
wchar_t* __wmemcpy_chk(wchar_t* d, const wchar_t* s, size_t n, size_t) { return wmemcpy(d, s, n); }
int wcscmp(const wchar_t* a, const wchar_t* b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }

char* __nl_langinfo_l(int, void*) { static char empty[] = ""; return empty; }
char* bind_textdomain_codeset(const char*, const char*) { return nullptr; }
void* __uselocale(void*) { return nullptr; }
char* dgettext(const char*, const char* msgid) { return const_cast<char*>(msgid); }
char* gettext(const char* msgid) { return const_cast<char*>(msgid); }
size_t __strftime_l(char*, size_t, const char*, const void*, void*) { return 0; }
size_t __wcsftime_l(wchar_t*, size_t, const wchar_t*, const void*, void*) { return 0; }
int strerror_r(int, char* buf, size_t buflen) { if (buf && buflen) buf[0] = 0; return 0; }
long syscall(long, ...) { return -1; }
float __strtof_l(const char* s, char** end, void*) { return static_cast<float>(strtod(s, end)); }
double __strtod_l(const char* s, char** end, void*) { return strtod(s, end); }
long double strtold_l(const char* s, char** end, void*) { return static_cast<long double>(strtod(s, end)); }
void* __newlocale(int, const char*, void*) { return nullptr; }
void __freelocale(void*) {}
void* __duplocale(void*) { return nullptr; }
size_t wcsnrtombs(char*, const wchar_t**, size_t, size_t, void*) { return static_cast<size_t>(-1); }
size_t wcrtomb(char*, wchar_t, void*) { return static_cast<size_t>(-1); }
size_t mbsnrtowcs(wchar_t*, const char**, size_t, size_t, void*) { return static_cast<size_t>(-1); }
size_t mbrtowc(wchar_t*, const char*, size_t, void*) { return static_cast<size_t>(-1); }
size_t __ctype_get_mb_cur_max() { return 1; }
size_t __mbsnrtowcs_chk(wchar_t*, const char**, size_t, size_t, void*, size_t) { return static_cast<size_t>(-1); }
size_t __mbsrtowcs_chk(wchar_t*, const char**, size_t, void*, size_t) { return static_cast<size_t>(-1); }
int __strcoll_l(const char* a, const char* b, void*) { return strcmp(a, b); }
size_t __strxfrm_l(char* d, const char* s, size_t n, void*) { size_t l = strlen(s); if (d && n) { strncpy(d, s, n); d[n-1] = 0; } return l; }
int __wcscoll_l(const wchar_t*, const wchar_t*, void*) { return 0; }
size_t __wcsxfrm_l(wchar_t*, const wchar_t*, size_t, void*) { return 0; }
void* iconv_open(const char*, const char*) { return reinterpret_cast<void*>(-1); }
size_t iconv(void*, char**, size_t*, char**, size_t*) { return static_cast<size_t>(-1); }
int iconv_close(void*) { return 0; }
typedef uint32_t wint_t;
unsigned long __wctype_l(const char*, void*) { return 0; }
wint_t __towupper_l(wint_t c, void*) { return c; }
wint_t __towlower_l(wint_t c, void*) { return c; }
int __iswctype_l(wint_t, unsigned long, void*) { return 0; }
int wctob(wint_t c) { return c < 128 ? static_cast<int>(c) : -1; }
wint_t btowc(int c) { return c >= 0 && c < 128 ? static_cast<wint_t>(c) : static_cast<wint_t>(-1); }

/* ---- ctype, ASCII only - no locale on this substrate. ---- */
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int isxdigit(int c) { return isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }

/* ---- libm: what src/ actually uses. sqrt/sqrtf use the AArch64 FSQRT
 * instruction directly (the two genuinely architecture-specific lines in
 * this whole file - x86's twin uses SQRTSD/SQRTSS); everything built on top
 * of them is the same portable series/reduction code as the x86 version. */
double sqrt(double x) { double r; asm("fsqrt %d0, %d1" : "=w"(r) : "w"(x)); return r; }
float sqrtf(float x) { float r; asm("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r; }
#include "../libm_portable.inc"

namespace { unsigned long long g_rand_state = 0x853C49E6748FEA9Bull; }
int rand() {
  g_rand_state ^= g_rand_state >> 12;
  g_rand_state ^= g_rand_state << 25;
  g_rand_state ^= g_rand_state >> 27;
  return static_cast<int>((g_rand_state * 0x2545F4914F6CDD1Dull) >> 33);
}
void srand(unsigned int seed) { g_rand_state = seed ? static_cast<unsigned long long>(seed) : 0x853C49E6748FEA9Bull; }

/* ---- dummy libc/pthread stubs for libstdc++ references unreferenced at
 * runtime, and time - same fixed build-epoch approach main.cpp/timer.cpp
 * already use for anything that needs a wall clock on a machine with no
 * RTC wired up yet. */
namespace { int g_errno_val = 0; }
int* __errno_location() { return &g_errno_val; }
int ungetc(int, void*) { return -1; }
int fseeko64(void*, int64_t, int) { return -1; }
int64_t ftello64(void*) { return -1; }
int getwc(void*) { return -1; }
int ungetwc(int, void*) { return -1; }
int putwc(int, void*) { return -1; }
int putc(int, void*) { return -1; }
size_t fread(void*, size_t, size_t, void*) { return 0; }
int unlink(const char*) { return -1; }
int rename(const char*, const char*) { return -1; }
int mkdir(const char*, unsigned int) { return -1; }
void* fdopen(int, const char*) { return nullptr; }
int setvbuf(void*, char*, int, size_t) { return 0; }
void* fopen64(const char*, const char*) { return nullptr; }
// No stdio files on bare metal: an open fails, so callers take their no-file path.
void* fopen(const char*, const char*) { return nullptr; }
int ferror(void*) { return 1; }
int fileno(void*) { return -1; }
int fclose(void*) { return 0; }
long read(int, void*, size_t) { return -1; }
long write(int, const void*, size_t) { return -1; }
long writev(int, const void*, int) { return -1; }
int64_t lseek64(int, int64_t, int) { return -1; }
int ioctl(int, unsigned long, ...) { return -1; }
int poll(void*, unsigned long, int) { return 0; }
int fstat64(int, void*) { return -1; }
int getc(void*) { return -1; }

struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon; int tm_year; int tm_wday; int tm_yday; int tm_isdst; };
struct timespec { long tv_sec; long tv_nsec; };

/* ---- Time, for real. The wall clock comes from NTP (net.cpp,
 * metal_wall_unix_ns). Monotonic time is the free-running counter: timer-IRQ
 * ticks never advance on the Pi (no GIC), so steady_clock - and every
 * animation built on it - would read zero forever. */
#include "../time_common.inc"

}  // extern "C"

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n); }
void* operator new(size_t n, std::align_val_t a) {
  size_t al = static_cast<size_t>(a);
  if (al <= 16) return malloc(n);
  void* p = malloc(n + al);
  uintptr_t q = (reinterpret_cast<uintptr_t>(p) + al) & ~(al - 1);
  reinterpret_cast<void**>(q)[-1] = p;
  return reinterpret_cast<void*>(q);
}
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t a) noexcept { if (static_cast<size_t>(a) <= 16) free(p); else free(reinterpret_cast<void**>(p)[-1]); }
void operator delete(void* p, size_t, std::align_val_t a) noexcept { if (static_cast<size_t>(a) <= 16) free(p); else free(reinterpret_cast<void**>(p)[-1]); }

/* The standard library's throw points. src/ compiles without exceptions; a
 * container that would throw is a kernel invariant failure, not a
 * recoverable condition, and redirecting these here means libstdc++.a's own
 * functexcept.o/vterminate.o (which need gettext/stderr/a real terminate
 * handler) are never pulled from the archive at all. */
namespace std {
[[noreturn]] void __throw_bad_alloc() { metal_panic("bad_alloc"); }
[[noreturn]] void __throw_bad_array_new_length() { metal_panic("bad_array_new_length"); }
[[noreturn]] void __throw_bad_function_call() { metal_panic("bad_function_call"); }
[[noreturn]] void __throw_length_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_logic_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_out_of_range(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_out_of_range_fmt(const char* w, ...) { metal_panic(w); }
[[noreturn]] void __throw_invalid_argument(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_bad_cast() { metal_panic("bad_cast"); }
[[noreturn]] void __throw_runtime_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_domain_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_overflow_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_underflow_error(const char* w) { metal_panic(w); }
}  // namespace std
