/* Asks the kernel for two different things. Whether either is allowed is not
 * this program's business: it is the authority of the ENTITY the process runs
 * as, checked in ring 0 at the system-call boundary. The same bytes run twice
 * under two entities therefore produce two different answers, and that is the
 * whole point of the privilege split — the check is not something the program
 * can decline to make. */
#include "aios.h"

void _start(void) {
  static const char payload[] = "written by a user-space process";
  long s = sys(SYS_STATE, (long)payload, sizeof(payload) - 1, 0);
  if (s >= 0) { put("state allocated, id tail "); put_num(s); put("\n"); }
  else put("state refused\n");

  long w = sys(SYS_PERSIST, (long)payload, sizeof(payload) - 1, 0);
  if (w >= 0) { put("wrote "); put_num(w); put(" bytes to this machine's storage\n"); exit_(0); }
  if (w == -2) { put("storage refused: this entity holds no write authority over storage\n"); exit_(13); }
  put("storage failed\n");
  exit_(1);
}
