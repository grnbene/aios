/* Tries to read a kernel-only page and then to touch a system register. Both
 * are the kernel's business; EL0 must not be able to do either. */
#include "usys.h"

void _start(void) {
  uputs("    [el0] reading kernel memory at 0x40080000\n");
  volatile unsigned long* p = (volatile unsigned long*)0x40080000UL;
  unsigned long v = *p;
  uputs("    [el0] read succeeded, value ");
  uputu(v);
  uputs(" - EL0 can see kernel memory, which is a hole\n");
  uexit(0);
}
