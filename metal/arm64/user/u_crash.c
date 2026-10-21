/* Writes to an address it does not own. The kernel should report a data abort
 * from a lower EL and kill this process without taking the machine down. */
#include "usys.h"

void _start(void) {
  uputs("    [el0] about to write to address 0\n");
  volatile unsigned long* p = (volatile unsigned long*)0;
  *p = 1;
  uputs("    [el0] still alive, which means the mapping was too permissive\n");
  uexit(0);
}
