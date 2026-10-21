/* Burns CPU without ever yielding. If this program finishes and the machine is
 * still responsive, the timer took the core away from it — which is the whole
 * point of preemption. */
#include "usys.h"

void _start(void) {
  long t0 = uticks();
  volatile unsigned long acc = 0;
  for (unsigned long i = 0; i < 40000000UL; ++i) acc += i * 3;
  long t1 = uticks();
  uputs("    [el0] spin finished, ");
  uputu((unsigned long)(t1 - t0));
  uputs(" timer ticks elapsed while it never once yielded\n");
  uexit(0);
}
