/* The first program this operating system ever ran in ring 3. */
#include "aios.h"

void _start(void) {
  put("hello from user space, pid ");
  put_num(sys(SYS_GETPID, 0, 0, 0));
  put(", the kernel's clock says ");
  put_num(sys(SYS_TICKS, 0, 0, 0));
  put(" ticks\n");
  exit_(7);
}
