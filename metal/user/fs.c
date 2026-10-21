/* File descriptors: the machine's own storage, opened, written, closed, and
 * read back by a different descriptor. Whether any of it is allowed is decided
 * in ring 0 from the authority of the entity this process runs as. */
#include "aios.h"

void _start(void) {
  static const char text[] = "a process wrote this through a file descriptor\n";
  long fd = sys(SYS_OPEN, (long)"/tmp/written-by-a-process", slen("/tmp/written-by-a-process"), 1);
  if (fd < 0) { put("open for writing refused\n"); exit_(2); }
  long n = sys(SYS_WRITE, fd, (long)text, sizeof(text) - 1);
  sys(SYS_CLOSE, fd, 0, 0);
  put("wrote "); put_num(n); put(" bytes to fd "); put_num(fd); put("\n");

  fd = sys(SYS_OPEN, (long)"/tmp/written-by-a-process", slen("/tmp/written-by-a-process"), 0);
  if (fd < 0) { put("could not open it back\n"); exit_(3); }
  static char back[128];
  long got = sys(SYS_READ, fd, (long)back, sizeof(back));
  sys(SYS_CLOSE, fd, 0, 0);
  put("read back "); put_num(got); put(" bytes: ");
  sys(SYS_WRITE, 1, (long)back, got);
  int same = got == (long)(sizeof(text) - 1);
  for (long i = 0; same && i < got; ++i) if (back[i] != text[i]) same = 0;
  put(same ? "the file survived the round trip\n" : "the file came back WRONG\n");
  exit_(same ? 0 : 1);
}
