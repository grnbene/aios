/* Tries to turn interrupts off — an instruction only ring 0 may execute. If
 * this succeeds, the process is not really in ring 3 and the privilege split is
 * decoration. */
#include "aios.h"

void _start(void) {
  put("attempting cli from user space\n");
  asm volatile("cli");
  put("interrupts are off, which means this is NOT ring 3\n");
  exit_(0);
}
