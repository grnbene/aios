/* What a program on this machine may ask the kernel for.
 *
 * Twenty-one calls, entered with the syscall instruction: number in rax,
 * arguments in rdi, rsi, rdx, result back in rax. There is no libc here and
 * nothing is linked in — this is the whole interface between a process and this
 * operating system. Every call that touches the machine is checked in ring 0
 * against the authority of the entity the process runs as. */
#ifndef AIOS_USER_H
#define AIOS_USER_H

#define SYS_WRITE     0   /* (fd, buf, len)      -> bytes written */
#define SYS_EXIT      1   /* (code)              -> does not return */
#define SYS_GETPID    2
#define SYS_YIELD     3
#define SYS_TICKS     4   /* ()                  -> the kernel's own clock */
#define SYS_STATE     5   /* (buf, len)          -> a state object, or refused */
#define SYS_OPEN      6   /* (path, len, write)  -> fd */
#define SYS_READ      7   /* (fd, buf, len)      -> bytes read */
#define SYS_CLOSE     8   /* (fd)                -> written back to storage */
#define SYS_SEEK      9   /* (fd, off, whence)   -> new offset */
#define SYS_BRK      10   /* (addr)              -> the break; 0 asks where it is */
#define SYS_MMAP     11   /* (len)               -> a fresh, zeroed, writable region */
#define SYS_FORK     12   /* ()                  -> 0 in the child, its pid in the parent */
#define SYS_EXEC     13   /* (path, len)         -> a different program, same process */
#define SYS_WAIT     14   /* (&status)           -> a finished child's pid; blocks */
#define SYS_KILL     15   /* (pid, signal) */
#define SYS_SIGNAL   16   /* (signal, handler, trampoline) */
#define SYS_SIGRETURN 17  /* ()                  -> back to where the signal interrupted */
#define SYS_GETPPID  18
#define SYS_SLEEP    19   /* (ticks)             -> gives the CPU up until then */
#define SYS_PERSIST  20   /* (buf, len)          -> this machine's storage, if this entity may */
#define SYS_JEPA_PREDICT 21 /* (s_in, a_in, s_out)  -> query kernel JEPA predictor */
#define SYS_JEPA_STATE   22 /* (buf, len)          -> sample kernel continuous state vector */
#define SYS_JEPA_SHM     23 /* withdrawn: always -1 (it leaked a kernel pointer); use SYS_JEPA_STATE */

static inline long sys(long n, long a, long b, long c) {
  long r;
  asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
  return r;
}

static inline long sys_jepa_predict(const float* s_in, const float* a_in, float* s_out) {
  return sys(SYS_JEPA_PREDICT, (long)s_in, (long)a_in, (long)s_out);
}

static inline long sys_jepa_state(float* buf, unsigned long len) {
  return sys(SYS_JEPA_STATE, (long)buf, (long)len, 0);
}

static inline long sys_jepa_shm(void) {
  return sys(SYS_JEPA_SHM, 0, 0, 0);
}


/* Where a signal handler returns to: it asks the kernel to put back the frame
 * the signal interrupted. The kernel pushes this address as the handler's
 * return address, so the handler is an ordinary C function. */
asm(".globl __sigreturn_trampoline\n"
    "__sigreturn_trampoline:\n"
    "  movq $17, %rax\n"
    "  syscall\n");
extern void __sigreturn_trampoline(void);
static inline void on_signal(long sig, void (*handler)(long)) {
  sys(SYS_SIGNAL, sig, (long)handler, (long)__sigreturn_trampoline);
}

static inline unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) ++n; return n; }
static inline void put(const char* s) { sys(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static inline void put_num(long v) {
  char b[24], o[24];
  int n = 0, neg = v < 0;
  unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
  if (!u) b[n++] = '0';
  while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
  if (neg) b[n++] = '-';
  for (int i = 0; i < n; ++i) o[i] = b[n - 1 - i];
  sys(SYS_WRITE, 1, (long)o, n);
}
/* The kernel never returns from this. Asking again if it somehow does is
 * cheaper than trusting a compiler not to delete an empty loop. */
static inline void exit_(long code) { for (;;) sys(SYS_EXIT, code, 0, 0); }

#endif
