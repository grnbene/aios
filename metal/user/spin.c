/* Never makes a system call while it works. The only thing that can take the
 * CPU away from this loop is the timer interrupt, so two of these finishing —
 * and the kernel accounting ticks to both — is preemption or it is nothing. */
#include "aios.h"

void _start(void) {
  long me = sys(SYS_GETPID, 0, 0, 0);
  put("spin "); put_num(me); put(" starting at tick "); put_num(sys(SYS_TICKS, 0, 0, 0)); put("\n");
  volatile long acc = 0;
  for (long i = 0; i < 120000000L; ++i) acc += i;      /* no syscall in here */
  put("spin "); put_num(me); put(" finished at tick "); put_num(sys(SYS_TICKS, 0, 0, 0)); put("\n");
  exit_(acc ? 0 : 1);
}
