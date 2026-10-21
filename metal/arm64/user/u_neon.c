/* Keeps a value in a NEON register across enough work to be preempted several
 * times. If the trap path's q0-q31 save list is wrong, this comes back wrong. */
#include "usys.h"

void _start(void) {
  register float vsum __asm__("s8");
  __asm__ volatile("fmov s8, #1.0" : "=w"(vsum));
  float step = 0.0f;
  for (unsigned long i = 0; i < 20000000UL; ++i) {
    __asm__ volatile("" : "+w"(vsum) : "w"(step) : "memory");
  }
  float out;
  __asm__ volatile("fmov %s0, s8" : "=w"(out));
  uputs("    [el0] s8 survived the run: ");
  uputu((unsigned long)(out * 1000.0f));
  uputs(" (expected 1000)\n");
  uexit(0);
}
