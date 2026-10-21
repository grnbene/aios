/* Dereferences nothing at all. Before there was an IDT this stopped the whole
 * machine; now it should end exactly one process. */
#include "aios.h"

void _start(void) {
  put("about to read address zero\n");
  volatile long* nowhere = (volatile long*)0;
  long v = *nowhere;
  put("still here, which would be wrong: ");
  put_num(v);
  exit_(0);
}
