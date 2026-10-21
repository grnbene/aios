/* The smallest thing that proves EL0 is real: a process that can only reach
 * the console through a syscall, because its own mapping has no MMIO in it. */
#include "usys.h"

void _start(void) {
  uputs("    [el0] hello from pid ");
  uputu((unsigned long)ugetpid());
  uputs(", running at EL0 with its own page tables\n");
  uexit(0);
}
