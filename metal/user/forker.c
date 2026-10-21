/* fork, and then wait for what came of it. The child's memory is its own from
 * the first instruction: it writes over the shared array and the parent's copy
 * is untouched. */
#include "aios.h"

static long shared[64];

void _start(void) {
  for (int i = 0; i < 64; ++i) shared[i] = 11;
  long me = sys(SYS_GETPID, 0, 0, 0);
  long kid = sys(SYS_FORK, 0, 0, 0);
  if (kid == 0) {
    for (int i = 0; i < 64; ++i) shared[i] = 22;
    put("child pid "); put_num(sys(SYS_GETPID, 0, 0, 0));
    put(", parent "); put_num(sys(SYS_GETPPID, 0, 0, 0));
    put(", its own copy says "); put_num(shared[0]); put("\n");
    exit_(42);
  }
  long status = 0;
  long done = sys(SYS_WAIT, (long)&status, 0, 0);
  put("parent pid "); put_num(me);
  put(" waited for "); put_num(done);
  put(", it exited "); put_num(status);
  put(", parent's copy still says "); put_num(shared[0]); put("\n");
  exit_(shared[0] == 11 && status == 42 ? 0 : 1);
}
