/* fork and then become a different program. Same process, same pid, same open
 * files, same authority — a new address space and a new image. */
#include "aios.h"

void _start(void) {
  long kid = sys(SYS_FORK, 0, 0, 0);
  if (kid == 0) {
    put("child "); put_num(sys(SYS_GETPID, 0, 0, 0)); put(" is about to become /bin/hello\n");
    sys(SYS_EXEC, (long)"/bin/hello", slen("/bin/hello"), 0);
    put("exec came back, which means it failed\n");
    exit_(9);
  }
  long status = 0;
  long done = sys(SYS_WAIT, (long)&status, 0, 0);
  put("the program that was here exited "); put_num(status);
  put(" as pid "); put_num(done); put("\n");
  exit_(status == 7 ? 0 : 1);
}
