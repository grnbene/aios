/* Memory a process asks for while it runs: the break moved, and an anonymous
 * mapping. Both are pages the kernel finds, zeroes and maps into this address
 * space and no other. */
#include "aios.h"

void _start(void) {
  long start = sys(SYS_BRK, 0, 0, 0);
  long moved = sys(SYS_BRK, start + 40960, 0, 0);
  put("break "); put_num(start); put(" -> "); put_num(moved); put("\n");
  volatile char* heap = (volatile char*)start;
  for (int i = 0; i < 40960; ++i) heap[i] = (char)(i & 0x7F);
  int ok = 1;
  for (int i = 0; i < 40960; ++i) if (heap[i] != (char)(i & 0x7F)) ok = 0;
  put(ok ? "wrote and read back 40 KiB of heap\n" : "the heap did not hold what was written\n");

  long at = sys(SYS_MMAP, 65536, 0, 0);
  if (at < 0) { put("mmap refused\n"); exit_(2); }
  volatile long* region = (volatile long*)at;
  int zeroed = 1;
  for (int i = 0; i < 8192; ++i) if (region[i] != 0) zeroed = 0;
  for (int i = 0; i < 8192; ++i) region[i] = i * 3;
  for (int i = 0; i < 8192; ++i) if (region[i] != i * 3) ok = 0;
  put("mmap gave 64 KiB at "); put_num(at);
  put(zeroed ? ", zeroed" : ", NOT zeroed");
  put(ok ? ", and it holds what was written\n" : ", and it does NOT hold what was written\n");
  exit_(ok && zeroed ? 0 : 1);
}
