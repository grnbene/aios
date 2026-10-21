/* The EL0 side of the system call ABI.
 *
 * x8 selects, x0-x5 carry arguments, x0 carries the result — the AArch64 Linux
 * convention, because there is no reason to invent a second one and every
 * disassembler already reads this one.
 */
#ifndef AIOS_ARM64_USYS_H
#define AIOS_ARM64_USYS_H

#define SYS_WRITE     0   /* (fd, buf, len)      -> bytes written */
#define SYS_EXIT      1   /* (code)              -> does not return */
#define SYS_GETPID    2
#define SYS_YIELD     3
#define SYS_TICKS     4   /* ()                  -> the kernel's own clock */
#define SYS_STATE     5   /* (buf, len)          -> state object */
#define SYS_OPEN      6   /* (path, len, write)  -> fd */
#define SYS_READ      7   /* (fd, buf, len)      -> bytes read */
#define SYS_CLOSE     8   /* (fd)                -> close fd */
#define SYS_SEEK      9   /* (fd, off, whence)   -> new offset */
#define SYS_BRK      10   /* (addr)              -> break / sbrk */
#define SYS_SBRK     10
#define SYS_MMAP     11   /* (len)               -> anonymous zeroed region */
#define SYS_FORK     12   /* ()                  -> 0 in child, pid in parent */
#define SYS_EXEC     13   /* (path, len)         -> replace image */
#define SYS_WAIT     14   /* (&status)           -> finished child pid */
#define SYS_KILL     15   /* (pid, signal) */
#define SYS_SIGNAL   16   /* (signal, handler, trampoline) */
#define SYS_SIGRETURN 17  /* ()                  -> return from signal */
#define SYS_GETPPID  18
#define SYS_SLEEP    19   /* (ticks)             -> sleep */
#define SYS_PERSIST  20   /* (buf, len)          -> write to storage */
#define SYS_JEPA_PREDICT 21 /* (s_in, a_in, s_out)  -> JEPA prediction */
#define SYS_JEPA_STATE   22 /* (buf, len)          -> state sample */
#define SYS_JEPA_SHM     23 /* ()                  -> shared memory */

static inline long usys(long n, long a0, long a1, long a2) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
  return x0;
}

static inline void uexit(long code) { for (;;) usys(SYS_EXIT, code, 0, 0); }
static inline long uwrite(const char* s, unsigned long n) {
  return usys(SYS_WRITE, 1, (long)s, (long)n);
}
static inline long ugetpid(void)  { return usys(SYS_GETPID, 0, 0, 0); }
static inline long ugetppid(void) { return usys(SYS_GETPPID, 0, 0, 0); }
static inline void uyield(void)   { usys(SYS_YIELD, 0, 0, 0); }
static inline long uticks(void)   { return usys(SYS_TICKS, 0, 0, 0); }
static inline long ubrk(unsigned long addr) { return usys(SYS_BRK, (long)addr, 0, 0); }
static inline long ummap(unsigned long len) { return usys(SYS_MMAP, (long)len, 0, 0); }
static inline long ufork(void)    { return usys(SYS_FORK, 0, 0, 0); }
static inline long uexec(const char* path, unsigned long len) { return usys(SYS_EXEC, (long)path, (long)len, 0); }
static inline long uwait(long* status) { return usys(SYS_WAIT, (long)status, 0, 0); }
static inline long ukill(long pid, long sig) { return usys(SYS_KILL, pid, sig, 0); }
static inline long usleep(long ticks) { return usys(SYS_SLEEP, ticks, 0, 0); }
static inline long uopen(const char* path, unsigned long len, int write) { return usys(SYS_OPEN, (long)path, (long)len, (long)write); }
static inline long uread(long fd, void* buf, unsigned long len) { return usys(SYS_READ, fd, (long)buf, (long)len); }
static inline long uclose(long fd) { return usys(SYS_CLOSE, fd, 0, 0); }
static inline long useek(long fd, long off, int whence) { return usys(SYS_SEEK, fd, off, (long)whence); }

static inline unsigned long ustrlen(const char* s) {
  unsigned long n = 0;
  while (s[n]) ++n;
  return n;
}
static inline void uputs(const char* s) { uwrite(s, ustrlen(s)); }

static inline void uputu(unsigned long v) {
  char b[24];
  int i = 0;
  if (!v) b[i++] = '0';
  while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
  char out[24];
  int j = 0;
  while (i) out[j++] = b[--i];
  uwrite(out, (unsigned long)j);
}

static inline int ustreq(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
/* True if `s` begins with `prefix`; on match, `*rest` is left pointing just
 * past it (so a caller can pull "hello" out of "run hello" without a
 * strtok this freestanding header has no libc to provide). */
static inline int ustarts_with(const char* s, const char* prefix, const char** rest) {
  const char* p = prefix;
  while (*p && *s == *p) { ++s; ++p; }
  if (*p) return 0;
  if (rest) *rest = s;
  return 1;
}

#endif
