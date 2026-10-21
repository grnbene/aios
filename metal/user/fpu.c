/* Floating point across a context switch. The loop is long enough to be
 * interrupted many times, and it holds live values in the SSE registers the
 * whole way. Two of these running at once must produce the same number as one
 * running alone — if the kernel does not save and restore this register file,
 * they read each other's arithmetic and both are wrong. */
#include "aios.h"

void _start(void) {
  double a = 1.0, b = 0.5, c = 0.25;
  for (long i = 0; i < 4000000L; ++i) {
    a = a * 1.0000001 + 0.0000003;
    b = b * 0.9999999 + 0.0000002;
    c = c + a * 0.0000001 - b * 0.0000001;
  }
  long out = (long)(a * 1000000.0) + (long)(b * 1000000.0) + (long)(c * 1000000.0);
  put("fpu "); put_num(sys(SYS_GETPID, 0, 0, 0)); put(" computed "); put_num(out); put("\n");
  exit_(0);
}
