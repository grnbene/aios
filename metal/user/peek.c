/* The kernel's own mapping is present in every address space — an interrupt
 * taken here has to land on kernel text and a kernel stack. This reads it. The
 * pages are supervisor pages, so the processor should refuse, and the proof
 * that "shared into the address space" is not the same as "reachable" is that
 * this program dies rather than printing a byte of the kernel. */
#include "aios.h"

void _start(void) {
  put("reading the kernel's own memory at 1 MiB\n");
  volatile unsigned char* kernel = (volatile unsigned char*)0x100000;
  unsigned char v = *kernel;
  put("read it — which would mean ring 3 is not enforced: ");
  put_num(v);
  put("\n");
  exit_(0);
}
