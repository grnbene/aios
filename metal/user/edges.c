/* The edges of the system-call boundary, each one a way a process used to get
 * the kernel to fault, corrupt its own tables, leak an address, or kill the
 * process for something it was entitled to do. Every line it prints is an
 * answer the kernel gave; `host run /bin/edges` and read them. */
#include "aios.h"

#define USER_STACK_TOP 0x8010000000L

static volatile long in_first = 0, second_ran = 0, second_nested = 0;
static void second(long sig) { second_ran = sig; if (in_first) second_nested = 1; }
static void first(long sig) {
  in_first = 1;
  /* Signal 12 arrives while this handler is still running: it must wait for
   * this one's sigreturn, not end the process. */
  sys(SYS_KILL, sys(SYS_GETPID, 0, 0, 0), 12, 0);
  for (int i = 0; i < 3; ++i) sys(SYS_YIELD, 0, 0, 0);
  in_first = 0;
}

void _start(void) {
  /* Ordered so the calls that could take the machine down on an unfixed
   * kernel come last. */
  /* 5. The shared-region call. */
  long p = sys(SYS_JEPA_SHM, 0, 0, 0);
  put("jepa shm: "); put_num(p); put("\n");
  /* 4. mmap a length that wraps mmap_next past the end of the address space. */
  long m1 = sys(SYS_MMAP, -0x8020000000L, 0, 0);
  long m2 = sys(SYS_MMAP, 4096, 0, 0);
  put("mmap wrapping length: "); put_num(m1); put(", next mmap at "); put_num(m2); put("\n");

  /* 3. SYS_JEPA_STATE with a count whose byte size wraps to 4: the last 4
   * bytes of the stack, and nothing mapped after them. */
  long s = sys(SYS_JEPA_STATE, USER_STACK_TOP - 4, (1L << 62) + 1, 0);
  put("jepa state, wrapped count at the stack top: "); put_num(s); put("\n");

  /* 2. A pointer the process does not have. */
  long w = sys(SYS_WRITE, 1, 0x8050000000L, 16);
  put("write from an unmapped address: "); put_num(w); put("\n");

  /* 1. A caught signal during a handler. */
  on_signal(10, first);
  on_signal(12, second);
  sys(SYS_KILL, sys(SYS_GETPID, 0, 0, 0), 10, 0);
  for (int i = 0; i < 20 && !second_ran; ++i) sys(SYS_YIELD, 0, 0, 0);
  put("nested signal: "); put(second_ran ? (second_nested ? "ran INSIDE the first handler\n" : "held until the first handler returned, then delivered\n") : "never delivered\n");

  put("edges: done\n");
  exit_(0);
}
