/* A real interactive userland shell: reads from the stdin syscall (not a
 * toy that only prints once), and forks/execs/waits on the other embedded
 * programs by name - the fork/exec/wait/scheduler machinery this substrate
 * already had, exercised end to end from inside EL0 for the first time
 * rather than only from the kernel's own "run N" command. */
#include "usys.h"

static int read_line(char* buf, int max) {
  int n = 0;
  for (;;) {
    char c;
    long got = uread(0, &c, 1);
    if (got <= 0) { uyield(); continue; }
    if (c == '\r' || c == '\n') { uwrite("\n", 1); break; }
    if ((c == 0x7F || c == 8) && n > 0) { --n; uwrite("\b \b", 3); continue; }
    if (n + 1 < max && c >= 32 && c < 127) { buf[n++] = c; uwrite(&c, 1); }
  }
  buf[n] = '\0';
  return n;
}

void _start(void) {
  uputs("    [el0] shell: pid ");
  uputu((unsigned long)ugetpid());
  uputs(" - real fork/exec/wait, not a toy. Type 'help'.\n");

  char line[128];
  for (;;) {
    uputs("el0$ ");
    read_line(line, (int)sizeof(line));
    if (line[0] == '\0') continue;

    if (ustreq(line, "exit")) uexit(0);

    if (ustreq(line, "help")) {
      uputs("  help            this text\n");
      uputs("  ps              this shell's pid and parent pid\n");
      uputs("  run <name>      fork + exec <name>, then wait for it\n");
      uputs("                  (try: hello, spin, neon, crash, priv)\n");
      uputs("  exit            quit the shell\n");
      continue;
    }
    if (ustreq(line, "ps")) {
      uputs("  pid "); uputu((unsigned long)ugetpid());
      uputs(" ppid "); uputu((unsigned long)ugetppid());
      uputs("\n");
      continue;
    }
    const char* name;
    if (ustarts_with(line, "run ", &name)) {
      if (!*name) { uputs("  usage: run <name>\n"); continue; }
      long child = ufork();
      if (child == 0) {
        uexec(name, ustrlen(name));
        /* uexec() only returns here on failure - success replaces this image. */
        uputs("  exec failed: no such program\n");
        uexit(1);
      }
      if (child < 0) { uputs("  fork failed\n"); continue; }
      long status = 0;
      long who = uwait(&status);
      uputs("  pid "); uputu((unsigned long)who);
      uputs(" exited, status "); uputu((unsigned long)status);
      uputs("\n");
      continue;
    }
    uputs("  unknown command (try 'help')\n");
  }
}
