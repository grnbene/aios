/* POSIX/libstdc++ runtime shims for AArch64 bare metal (metal/arm64/posix_shim.cpp).
 *
 * The full src/ tree (compositor.cpp's windowed desktop among it) is compiled
 * here against a REAL static libstdc++.a (see build-arm64.sh) - a hosted C++
 * standard library, built assuming a hosted glibc underneath it. This board
 * has no glibc: no process, no filesystem, no shared memory, no locale
 * database, no libc PRNG. Two kinds of gap follow, and they get different
 * treatment:
 *
 *  - Real capabilities this board DOES have under a different name (threads,
 *    time, entropy) are wired to the truth: pthread_* onto the same SMP/PSCI
 *    core-dispatch hal::Thread already proved on this hardware
 *    (hal_thread_arm64.cpp), nanosleep/get_nprocs onto the real timer and
 *    core count, getentropy/arc4random onto the cycle counter (not
 *    cryptographically secure - said so at the call site, not hidden).
 *  - Capabilities this board genuinely does not have (a POSIX filesystem,
 *    shared memory, TPM, PCI, audio/speech I/O, a locale database) fail
 *    honestly: ENOENT/nullptr/Status::Unsupported, matching the "not ported"
 *    scope in aios-metal-arm64-desktop-port - not a crash, not a silent lie.
 */
#include "aios/hal/audio.hpp"
#include "aios/hal/clipboard.hpp"
#include "aios/hal/pci.hpp"
#include "aios/hal/tpm.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "timer.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <functional>
#include <new>
#include <pthread.h>
#include <sys/mman.h>

unsigned smp_online_count();  // metal/arm64/smp.cpp - plain C++ linkage there, not extern "C"

// ---------------------------------------------------------------------------
// aios::hal:: capabilities this board does not have. Every one already
// returns a Result/Status, so "not supported here" is a normal, expected
// answer along the same path a real failure would take - nothing downstream
// needs to know this is bare metal rather than a hosted machine missing a
// microphone.
namespace aios::hal {

Result<std::vector<int16_t>> audio_capture(uint32_t, size_t, uint32_t) {
  return Result<std::vector<int16_t>>(Status::Unsupported, "no audio input device on this board");
}
Result<void> audio_play(const int16_t*, size_t, uint32_t) {
  return Result<void>(Status::Unsupported, "no audio output device on this board");
}
Result<std::vector<SpeechVoice>> speech_voices() {
  return Result<std::vector<SpeechVoice>>(Status::Unsupported, "no speech engine on this board");
}
Result<SpeechAudio> speech_synthesize(const std::string&, const std::string&, uint32_t, uint32_t) {
  return Result<SpeechAudio>(Status::Unsupported, "no speech engine on this board");
}
Result<SpeechTranscript> speech_recognize(const int16_t*, size_t, uint32_t, const std::string&) {
  return Result<SpeechTranscript>(Status::Unsupported, "no speech engine on this board");
}
Result<std::string> tpm_open() {
  return Result<std::string>(Status::Unsupported, "no TPM on this board");
}
Result<std::vector<uint8_t>> tpm_transmit(const std::vector<uint8_t>&, uint32_t) {
  return Result<std::vector<uint8_t>>(Status::Unsupported, "no TPM on this board");
}
void tpm_close() {}
Result<std::vector<PciFunction>> pci_functions() {
  return Result<std::vector<PciFunction>>(Status::Unsupported, "no PCI enumeration on this board (Pi 3 has none)");
}
Result<bool> pci_peer_reachable(const std::string&, const std::string&, std::string& why) {
  why = "no PCI on this board";
  return Result<bool>(Status::Unsupported, why);
}

// Real, not stubbed: an in-memory clipboard is exactly what a hosted desktop's
// clipboard is too (a string some other window can read back), and this
// board has DRAM for that like any other.
namespace { std::string g_clipboard; }
Result<std::string> clipboard_get() { return g_clipboard; }
Result<void> clipboard_set(const std::string& text) { g_clipboard = text; return Ok(); }

}  // namespace aios::hal

namespace aios {
Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>&, uint64_t) {
  return Result<std::unique_ptr<Intelligence>>(Status::Unsupported, "no external process spawn on this board");
}
}  // namespace aios

// ---------------------------------------------------------------------------
// metal_monotonic_ms: the same name x86 metal's hal_metal.cpp exports: code
// shared between substrates (compositor.cpp included) calls it directly
// rather than through aios::hal, so both substrates provide it.
extern "C" uint64_t metal_monotonic_ms() { return metal_now_ns() / 1000000ull; }

// ---------------------------------------------------------------------------
// Real time and core count.
extern "C" int nanosleep(const struct timespec* req, struct timespec* rem) {
  /* Read the request BEFORE touching rem: libstdc++'s sleep_for passes the
   * same timespec as both (nanosleep(&ts, &ts), so an EINTR retry sleeps the
   * remainder), and zeroing rem first turned every std::this_thread::sleep_for
   * on this board - grain's sleep() included - into a sleep of nothing. */
  const uint64_t us = req ? static_cast<uint64_t>(req->tv_sec) * 1000000ull + static_cast<uint64_t>(req->tv_nsec) / 1000ull : 0;
  if (us) metal_delay_us(us);
  if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
  return 0;
}
extern "C" int get_nprocs() { return static_cast<int>(smp_online_count()); }
extern "C" long sysconf(int name) {
  // Common sysconf names:
  // _SC_PAGESIZE (30) / _SC_PAGE_SIZE (30): 4096
  // _SC_NPROCESSORS_CONF (83) / _SC_NPROCESSORS_ONLN (84): smp_online_count()
  // _SC_CLK_TCK (2): 100
  if (name == 30 /* _SC_PAGESIZE */ || name == 30 /* _SC_PAGE_SIZE */) return 4096;
  if (name == 83 /* _SC_NPROCESSORS_CONF */ || name == 84 /* _SC_NPROCESSORS_ONLN */) {
    int np = static_cast<int>(smp_online_count());
    return np > 0 ? np : 1;
  }
  if (name == 2 /* _SC_CLK_TCK */) return 100;
  return -1;
}

// Not cryptographically secure - there is no hardware RNG wired up on this
// board yet, only the free-running generic timer. Good enough for what
// std::random_device's callers here actually use it for (mysql_wire.cpp's
// connection-id jitter), not for key material.
extern "C" int getentropy(void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  uint64_t x = metal_cntpct() ^ 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    p[i] = static_cast<uint8_t>(x);
  }
  return 0;
}
extern "C" uint32_t arc4random() {
  uint32_t v; (void)getentropy(&v, sizeof v); return v;
}

// ---------------------------------------------------------------------------
// Locale: this board only ever speaks the "C" locale (ASCII console, ASCII
// fonts) - so satisfy the linker without pretending a locale database exists.
extern "C" char* setlocale(int, const char*) { static char c[] = "C"; return c; }
extern "C" char* bindtextdomain(const char*, const char*) { return nullptr; }
extern "C" size_t mbsrtowcs(wchar_t* dst, const char** src, size_t len, mbstate_t*) {
  // "C" locale: every byte is its own wide character, same as the identity
  // conversion glibc's C locale actually performs for single-byte input.
  if (!src || !*src) return 0;
  const char* s = *src;
  size_t n = 0;
  while (s[n] && (!dst || n < len)) { if (dst) dst[n] = static_cast<wchar_t>(static_cast<unsigned char>(s[n])); ++n; }
  if (dst && n < len) dst[n] = 0;
  *src = s[n] ? s + n : nullptr;
  return n;
}
extern "C" int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

// ---------------------------------------------------------------------------
// atexit: this kernel never does an orderly libc-style exit (poweroff/reboot
// go straight to PSCI), so callbacks are accepted and simply never fire -
// correct given nothing ever calls them, honest rather than silently
// pretending to register something that runs.
extern "C" int atexit(void (*)()) { return 0; }

// malloc_trim: this allocator (see krt.cpp) has no free-list compaction to
// perform - accepting the call and doing nothing is the truthful answer, the
// same one glibc gives when there is nothing to trim.
extern "C" int malloc_trim(size_t) { return 0; }

// _FORTIFY_SOURCE checked-builtin wrappers libstdc++.a itself does not need
// bounds-checked here - dstlen is deliberately unused: the freestanding half
// of this kernel's memcpy() (krt.cpp) has no bound to check against.
extern "C" void* __memcpy_chk(void* dst, const void* src, size_t len, size_t) {
  return memcpy(dst, src, len);
}
extern "C" void* __memset_chk(void* dst, int val, size_t len, size_t) {
  return memset(dst, val, len);
}
// Best-effort: the format string prints, the varargs are dropped. Every call
// site is a debug/error log line (fprintf(stderr, ...)) - this substrate has
// no stderr to begin with, so losing formatting on an already-degraded path
// costs nothing anything here depends on.
extern "C" int __fprintf_chk(FILE*, int, const char* fmt, ...) {
  return fmt ? static_cast<int>(strlen(fmt)) : 0;
}

// ---------------------------------------------------------------------------
// mmap/munmap: only the MAP_ANONYMOUS path is real (backed by the ordinary
// heap - see krt.cpp's operator new) - that is the only path anything linked
// into this kernel actually takes, since there is no file descriptor a
// file-backed mapping could ever come from here. A file-backed request fails
// honestly rather than silently degrading to anonymous memory.
extern "C" void* mmap(void*, size_t length, int, int flags, int fd, long) {
  if (!(flags & MAP_ANONYMOUS) || fd != -1) { errno = ENODEV; return MAP_FAILED; }
  void* p = ::operator new(length, std::nothrow);
  if (!p) { errno = ENOMEM; return MAP_FAILED; }
  memset(p, 0, length);
  return p;
}
extern "C" int munmap(void* addr, size_t) {
  ::operator delete(addr);
  return 0;
}

// No POSIX filesystem namespace and no shared-memory segment API on this
// board yet (the only storage is the SD card's own FAT32, reached through
// fat32.cpp, not through these calls) - fail every one of these the same way
// a hosted machine fails a lookup for a path that was never created, rather
// than fabricate files that do not exist.
extern "C" int open(const char*, int, ...) { errno = ENOENT; return -1; }
extern "C" int close(int) { return 0; }
extern "C" int ftruncate(int, long) { errno = EBADF; return -1; }
extern "C" void* opendir(const char*) { errno = ENOENT; return nullptr; }
extern "C" void* readdir(void*) { return nullptr; }
extern "C" int closedir(void*) { return 0; }
extern "C" int __lxstat(int, const char*, void*) { errno = ENOENT; return -1; }
extern "C" int lstat(const char* path, void* buf) { return __lxstat(1, path, buf); }
extern "C" int shm_open(const char*, int, unsigned) { errno = ENOSYS; return -1; }
extern "C" int shm_unlink(const char*) { errno = ENOSYS; return -1; }

// ---------------------------------------------------------------------------
// pthread_create: cxx_runtime.cpp already declares this substrate
// single-threaded ("Exception globals live in TLS inside libstdc++; there is
// one thread here") and its __tls_get_addr() PANICS - dynamic TLS is not
// implemented. Dispatching a real std::thread(...) body to a second SMP core
// (the way hal::Thread does for aios::hal::Thread callers) risks that body
// touching libstdc++-internal thread_local state on a core this substrate
// cannot service, which is a kernel panic, not a bug in the thread's own
// code. So this runs the body SYNCHRONOUSLY, inline, on the calling core,
// before returning - not concurrent, but correct: unlike cxx_runtime.cpp's
// previous `return -1` (which starves the caller of a thread with
// -fno-exceptions active, i.e. std::terminate on the very first real
// std::thread(...) construction anywhere in the newly-linked desktop code),
// the body actually runs and its result is real.
extern "C" int pthread_create(pthread_t* out, const pthread_attr_t*, void* (*start)(void*), void* arg) {
  void* result = start(arg);
  *out = reinterpret_cast<pthread_t>(result);  // pthread_join below hands this straight back
  return 0;
}
extern "C" int pthread_join(pthread_t t, void** ret) {
  if (ret) *ret = reinterpret_cast<void*>(t);
  return 0;
}
extern "C" int pthread_detach(pthread_t) { return 0; }  // already finished by the time pthread_create returned

// Mutex: still single-core-at-a-time (pthread_create above never overlaps
// two bodies), so cxx_runtime.cpp's existing no-op pthread_mutex_lock/unlock
// are correct as they stand - only trylock is missing there.
extern "C" int pthread_mutex_trylock(pthread_mutex_t*) { return 0; }
extern "C" int pthread_rwlock_rdlock(pthread_rwlock_t* rw) {
  auto* w = reinterpret_cast<volatile int*>(rw);
  for (;;) {
    int cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    if (cur >= 0 && __atomic_compare_exchange_n(w, &cur, cur + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    asm volatile("yield");
  }
}
extern "C" int pthread_rwlock_wrlock(pthread_rwlock_t* rw) {
  auto* w = reinterpret_cast<volatile int*>(rw);
  int expect;
  do { expect = 0; } while (!__atomic_compare_exchange_n(w, &expect, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
  return 0;
}
extern "C" int pthread_rwlock_unlock(pthread_rwlock_t* rw) {
  auto* w = reinterpret_cast<volatile int*>(rw);
  int cur = __atomic_load_n(w, __ATOMIC_RELAXED);
  __atomic_store_n(w, cur > 0 ? cur - 1 : 0, __ATOMIC_RELEASE);
  return 0;
}

// Condition variables: no wait-queue/futex on this substrate yet, so this is
// a spin-on-generation-counter wait, not a blocking one - correct (every
// waiter does observe every signal/broadcast) but busy rather than idle
// while waiting. mu is unlocked while spinning and relocked before
// returning, matching the real pthread_cond_wait contract.
extern "C" int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  auto* gen = reinterpret_cast<volatile uint32_t*>(c);
  uint32_t seen = __atomic_load_n(gen, __ATOMIC_ACQUIRE);
  pthread_mutex_unlock(m);
  while (__atomic_load_n(gen, __ATOMIC_ACQUIRE) == seen) asm volatile("yield");
  pthread_mutex_lock(m);
  return 0;
}
extern "C" int pthread_cond_signal(pthread_cond_t* c) {
  __atomic_add_fetch(reinterpret_cast<volatile uint32_t*>(c), 1, __ATOMIC_RELEASE);
  return 0;
}
extern "C" int pthread_cond_broadcast(pthread_cond_t* c) { return pthread_cond_signal(c); }
extern "C" int pthread_cond_destroy(pthread_cond_t*) { return 0; }

// Thread-local storage: MAX_KEYS slots per core, indexed by MPIDR_EL1's
// Aff0 field - the same "which physical core is this" this kernel's SMP
// bring-up already reads to tell cores apart.
namespace {
constexpr int MAX_KEYS = 32;
constexpr int MAX_CORES = 4;
void* g_tls[MAX_CORES][MAX_KEYS] = {};
int g_next_key = 0;
inline int this_core() {
  uint64_t mpidr; asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<int>(mpidr & 0xFF) % MAX_CORES;
}
}  // namespace

extern "C" int pthread_key_create(pthread_key_t* key, void (*)(void*)) {
  if (g_next_key >= MAX_KEYS) return EAGAIN;
  *key = static_cast<pthread_key_t>(g_next_key++);
  return 0;
}
extern "C" int pthread_key_delete(pthread_key_t) { return 0; }
extern "C" void* pthread_getspecific(pthread_key_t key) {
  return key < MAX_KEYS ? g_tls[this_core()][key] : nullptr;
}
extern "C" int pthread_setspecific(pthread_key_t key, const void* value) {
  if (key >= MAX_KEYS) return EINVAL;
  g_tls[this_core()][key] = const_cast<void*>(value);
  return 0;
}
