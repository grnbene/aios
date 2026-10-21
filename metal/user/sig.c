/* A signal, caught. This installs a handler, says it is ready, and sleeps —
 * giving the CPU up entirely — until something signals it. */
#include "aios.h"

static volatile long caught = 0;

static void handler(long sig) {
  put("caught signal "); put_num(sig); put(" and the handler ran on my own stack\n");
  caught = sig;
}

void _start(void) {
  on_signal(15, handler);
  put("pid "); put_num(sys(SYS_GETPID, 0, 0, 0)); put(" is waiting for a signal\n");
  for (int i = 0; i < 400 && !caught; ++i) sys(SYS_SLEEP, 1, 0, 0);
  if (!caught) { put("nothing arrived\n"); exit_(1); }
  put("returning from the handler put me back where I was; exiting\n");
  exit_(caught);
}
