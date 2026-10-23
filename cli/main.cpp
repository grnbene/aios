// The Linux substrate's main: stdout as the sink, files from the filesystem,
// aios-boot as the host tool when present. Nothing here is the command surface.
#include "cli.hpp"
#include "model_controller.h"
#include "aios/userland/compositor.hpp"
#include "aios/userland/machine_sql.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "aios/hal/sync.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <cerrno>
#include "../tools/aios-git/git_cli.hpp"

int aios_webserver_run(int argc, char** argv);   // tools/aios-webserver.cpp
#ifdef AIOS_HAVE_SH
int aios_sh_main(int argc, char** argv);          // tools/aios-sh/main.cpp
#endif

using namespace aios;
using namespace aios::cli;

static void usage() {
  std::cout <<
    "aios — one binary: a SQL database, git, ssh, a web engine, mail and a desktop\n"
    "\n"
    "usage: aios [--root DIR] [-q] <mode> [args]\n"
    "       aios git ...      full git in the current directory (no --root, no kernel); same as aios-git\n"
    "       aios sh ...       the POSIX shell and utilities; same as aios-sh\n"
    "       aios webserver CONFIG   vhost HTTP(S) server with PHP-FPM; same as aios-webserver\n"
    "       aios [--root DIR] <command> [args]      any command below runs directly: aios git status\n"
    "  --root DIR   where state lives (identity, objects, repos, SQL store, keys).\n"
    "               Default: $AIOS_ROOT, else ./aios-root\n"
    "\n"
    "modes\n"
    "  exec 'cmd' ['cmd'..]   run commands and exit; stops at the first failure\n"
    "  shell                  interactive prompt (same commands)\n"
    "  run FILE.aos           run a script of commands; stops at the first unexpected failure\n"
    "  info                   machine and kernel summary\n"
    "  models [NAME]          list what each configured model endpoint serves; with NAME, which host serves it\n"
    "  attend [SECONDS]       carry out the standing directives (`directive add`) while idle\n"
    "  desktop | gui          the desktop: [--tui] [--windowed WxH] [--browser] [--open url] [--dump f.ppm]\n"
    "                         headless: --headless --frames N [--open URL] [--do frame:type:TEXT]\n"
    "                         [--do frame:key:Enter] [--do frame:click:x,y] [--do frame:shot:PATH.png]\n"
    "                         [--scene NAME] [--no-panels]\n"
    "  canvas-dump WxH f.ppm [FRAMES]   paint the canvas for that display size to a file\n"
    "\n"
    "commands (inside exec, shell or run; `aios exec help` lists them all)\n"
    "  database   aios exec 'compute serve sql=1 mysql_port=3307 mysql_user=root mysql_pass_file=FILE'\n"
    "                       serve ASQL over the MySQL wire; add sql_db=DIR for the store, and\n"
    "                       cdc_host= cdc_port= cdc_user= cdc_pass_file= cdc_db= cdc_tables= to mirror MariaDB/MySQL\n"
    "             aios exec 'sql serve port=3307' | sql query \"SQL\" | sql console | sql import | sql dump\n"
    "             aios-engine --help      the database as its own daemon (--sql-user, --sql-pass-file, --upstream ...)\n"
    "             clients: aios-asql-console (SQL prompt), aios-asql-top (live monitor)\n"
    "  git        aios git init | add | commit | log | blame | rebase | branch | switch ...   (as real git)\n"
    "                       the full git CLI, working in the current directory; also clone, fetch, pull, push (see `aios git help`)\n"
    "             inside exec/shell, `git` is the console's own command (repos registered under --root: git repos, git use NAME);\n"
    "             anything it doesn't do itself runs the same full git in the current repo\n"
    "  ssh        aios exec 'ssh keygen'                make this root's identity key; prints the public key\n"
    "             aios exec 'ssh USER@HOST[:PORT] CMD' [key=PATH] [passphrase=@FILE|env:NAME]\n"
    "             ssh known                             hosts this root has seen\n"
    "  web        aios exec 'browser URL'               fetch, lay out and read a page   (follow N | back)\n"
    "             aios webserver CONFIG                 vhost HTTP(S) server: static files, TLS (SNI), PHP-FPM over FastCGI;\n"
    "                                                   CONFIG holds `vhost NAME` ... `}` blocks with listen PORT [ssl] [loopback],\n"
    "                                                   cert, key, root, index, php HOST:PORT, max_body_size, deny\n"
    "  shell      aios sh -c 'cmd' | aios sh script.sh   POSIX shell with its own utilities\n"
    "  mail       aios exec 'mail account add --email A --user U --pass P --imap HOST:PORT --smtp HOST:PORT'\n"
    "             mail sync | list | read ID | send TO SUBJECT BODY | search QUERY | account test|list|remove\n"
    "             IMAP and SMTP are the kernel's own (TLS, STARTTLS, LOGIN/PLAIN/XOAUTH2); nothing else is started\n"
    "  google     aios exec 'google auth you@gmail.com'      sign in once (OAuth 2.0 in a browser); the tokens are kept encrypted\n"
    "             google sync [mail|calendar|contacts|notes]  two-way sync of Gmail, Google Calendar, contacts and notes\n"
    "             google status | watch [every=300] | logout | auth finish '<address>'\n"
    "             needs your own OAuth client: see docs/google-mail.md\n"
    "  other      contacts, calendar, task, npm, pip, cargo, go, js, grain, wallet, chain ...\n"
    "\n"
    "examples\n"
    "  cd ~/proj && aios git init && aios git add . && aios git commit -m first\n"
    "  aios --root /srv/aios exec 'ssh keygen' 'ssh me@host uptime'\n"
    "  aios --root /srv/aios exec 'compute serve sql=1 mysql_port=3307 mysql_user=root mysql_pass_file=/srv/aios/pw'\n"
    "  aios-asql-console -P 3307 -u root -p PASSWORD -e 'SHOW DATABASES'\n"
    "  aios webserver /etc/aios/web.conf\n";
}

// Services are pumped from whichever loop this machine happens to be idle in.
// Throttled to a second because a pass is a waitpid per child, and these loops
// spin far faster than that.
static void pump_services(aios::cli::Session& s) {
  if (!s.kernel) return;
  static uint64_t last_ms = 0;
  const uint64_t now_ms = s.kernel->host().monotonic_ns() / 1000000ull;
  if (last_ms && now_ms - last_ms < 1000) return;
  last_ms = now_ms;
  (void)aios::cli::supervise_services(s);
}

int main(int argc, char** argv) {
  std::string root = std::getenv("AIOS_ROOT") ? std::getenv("AIOS_ROOT") : "aios-root";
  std::vector<std::string> args;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--quiet") || !strcmp(argv[i], "-q")) quiet = true;
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      if (args.empty()) { usage(); return 0; }
      args.push_back(argv[i]);
    }
    else args.push_back(argv[i]);
  }
  if (args.empty()) { usage(); return 2; }

  // `aios webserver CONFIG` is the vhost HTTP(S) server, the same code as the aios-webserver binary.
  if (args[0] == "webserver" || args[0] == "aios-webserver") {
    std::vector<char*> wargv;
    wargv.push_back(const_cast<char*>("aios webserver"));
    for (size_t i = 1; i < args.size(); ++i) wargv.push_back(const_cast<char*>(args[i].c_str()));
    return aios_webserver_run((int)wargv.size(), wargv.data());
  }

#ifdef AIOS_HAVE_SH
  // `aios sh [-c CMD | SCRIPT]` is the POSIX shell and utilities (aios-sh), the same code.
  if (args[0] == "sh" || args[0] == "aios-sh") {
    std::vector<char*> sargv;
    sargv.push_back(const_cast<char*>("aios sh"));
    for (size_t i = 1; i < args.size(); ++i) sargv.push_back(const_cast<char*>(args[i].c_str()));
    return aios_sh_main((int)sargv.size(), sargv.data());
  }
#endif

  // `aios git ...` is git: the full git CLI, in the current directory, with no kernel boot and no
  // --root. (Inside exec / shell / run, `git` is the console's own command, which hands anything it
  // does not do itself to this same CLI.)
  if (args[0] == "git" || args[0] == "aios-git") {
    char cwdbuf[4096];
    std::string cwd = ::getcwd(cwdbuf, sizeof cwdbuf) ? std::string(cwdbuf) : std::string(".");
    std::vector<std::string> gargs(args.begin(), args.end());
    gargs[0] = "git";
    std::ios::sync_with_stdio(false);
    int rc = aios_git::run(gargs, cwd, std::cout, std::cerr, std::cin);
    std::cout.flush();
    std::cerr.flush();
    return rc;
  }

  KernelConfig cfg;
  cfg.root = root;
  if (const char* s = std::getenv("AIOS_FLEET_SLOTS")) { long v = std::atol(s); if (v > 0) cfg.slots = (uint32_t)v; }
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::cerr << "boot failed: " << k.error().str() << "\n"; return 1; }
  Session s;
  s.kernel = std::move(*k);
  // Destroyed before `s` (and the kernel in it) on every way out of main: the
  // machine's SQL engine - its clock thread, a mirror's pull in flight - is
  // closed while the kernel it uses still exists.
  struct MachineSqlShutdown {
    Kernel* k;
    ~MachineSqlShutdown() { if (k) aios::userland::machine_sql_shutdown(*k); }
  } machine_sql_shutdown_guard{s.kernel.get()};
  s.quiet = quiet;
  // Colour only for a person at a terminal: a pipe, a file or a log gets plain
  // text, and NO_COLOR (https://no-color.org) or TERM=dumb turns it off too.
  {
    const char* nc = std::getenv("NO_COLOR");
    const char* term = std::getenv("TERM");
    s.colour = isatty(STDOUT_FILENO) && !(nc && *nc) && !(term && std::string(term) == "dumb");
  }
  // The console is an entity. Not: human → console → kernel; but: human →
  // console entity → its capabilities → kernel.
  if (auto b = bind_console(s); !b.ok()) { std::cerr << "console entity: " << b.error().str() << "\n"; return 1; }
  Out out([](const std::string& t) { fwrite(t.data(), 1, t.size(), stdout); fflush(stdout); });
  out.set_colour(s.colour);
  s.out = &out;
  if (const char* tool = std::getenv("AIOS_HOST_TOOL")) {
    std::string toolpath = tool;
    s.host = [toolpath](const std::vector<std::string>& cmd, Out& o, Surface&) -> Result<void> {
      o.flush();
      pid_t pid = fork();
      if (pid == 0) { std::vector<char*> av{const_cast<char*>(toolpath.c_str())}; for (auto& a : cmd) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr); execv(av[0], av.data()); _exit(127); }
      int st = 0; waitpid(pid, &st, 0);
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return Result<void>(Status::IoError, "host tool exited with status " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1));
      return Ok();
    };
  } else {
    s.host = [](const std::vector<std::string>& cmd, Out& o, Surface&) -> Result<void> {
      if (cmd.empty()) return Result<void>(Status::Invalid, "empty host command");
      o.flush();
      pid_t pid = fork();
      if (pid == 0) {
        std::vector<char*> av;
        for (auto& a : cmd) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
      }
      int st = 0; waitpid(pid, &st, 0);
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return Result<void>(Status::IoError, "host command exited with status " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1));
      }
      return Ok();
    };
  }

  // Re-establish fleet membership on every kernel (re)start when configured via
  // env — a supervised kernel then rejoins the hub after any restart, natively
  // (no front-door lock, no external agent). AIOS_FLEET_HUB is "host" or
  // "host:port" (default port 7420); AIOS_FLEET_KEY is the cluster key.
  if (const char* hub = std::getenv("AIOS_FLEET_HUB")) {
    const char* fkey = std::getenv("AIOS_FLEET_KEY");
    if (*hub && fkey && *fkey) {
      std::string h = hub; auto colon = h.find(':');
      std::string fhost = colon == std::string::npos ? h : h.substr(0, colon);
      std::string fport = colon == std::string::npos ? std::string("7420") : h.substr(colon + 1);
      // A leading space keeps a boot line out of `history` (and the key with it).
      // `retry`: a hub that is down at boot is attached when it comes up.
      auto j = run_line(s, " fleet join " + fhost + " " + fport + " " + std::string(fkey) + " retry");
      if (!j.ok()) std::cerr << "fleet: could not join " << h << " at boot: " << j.error().str() << "\n";
    }
  }

  // Restore what the world model has already learned, for the same reason.
  // Weights and AdamW moments live in the kernel's root; without this every
  // start re-seeds them from the constructor's RNG and the machine relearns
  // this substrate from nothing, every boot, forever. No-op before the first
  // checkpoint exists; says so out loud if one exists and will not load.
  run_line(s, " jepa reboot-resume");
  // The boot lines are the machine's own, not the caller's: `run` counts and
  // `history` lists only what was asked of it.
  s.commands = 0;
  s.failures = 0;

  const std::string& mode = args[0];
  if (mode == "canvas-dump") {
    // canvas-dump WxH path.ppm [frames] — paint one frame of the canvas for a
    // display of that size and write it out. No GPU, no window: the painter,
    // checked. With a frame count it paints that many and reports what each
    // frame cost, which is how the desktop's frame budget is measured without
    // a display: the painter is the whole of the desktop's per-frame work in
    // canvas mode, so its wall clock IS the frame time.
    if (args.size() < 3) { std::cerr << "canvas-dump WxH path.ppm [frames]\n"; return 2; }
    uint32_t cw = 0, ch = 0;
    if (sscanf(args[1].c_str(), "%ux%u", &cw, &ch) != 2 || !cw || !ch) { std::cerr << "size must look like 1920x1080\n"; return 2; }
    int frames = args.size() > 3 ? atoi(args[3].c_str()) : 1;
    if (frames < 1) frames = 1;
    aios::userland::HardwareCompositor surface(cw, ch);
    aios::userland::CanvasState st;
    uint64_t now_ms = s.kernel->host().monotonic_ns() / 1000000ull;
    st.sweep(*s.kernel, now_ms);
    std::vector<uint32_t> buf;
    std::vector<double> ms;
    ms.reserve(frames);
    for (int i = 0; i < frames; ++i) {
      uint64_t t0 = s.kernel->host().monotonic_ns();
      aios::userland::paint_canvas(surface, buf, cw, ch, *s.kernel, *s.me, aios::userland::hosted_canvas_feed(*s.kernel), now_ms, st);
      ms.push_back((s.kernel->host().monotonic_ns() - t0) / 1e6);
    }
    if (!aios::userland::write_ppm(args[2], buf, cw, ch)) { std::cerr << "could not write " << args[2] << "\n"; return 1; }
    std::cout << "canvas " << cw << "x" << ch << " -> " << args[2] << "\n";
    if (frames > 1) {
      std::vector<double> sorted = ms;
      std::sort(sorted.begin(), sorted.end());
      double sum = 0; for (double v : ms) sum += v;
      double median = sorted[sorted.size() / 2];
      printf("frames=%d  first=%.2fms  min=%.2fms  median=%.2fms  max=%.2fms  mean=%.2fms  -> %.1f fps\n",
             frames, ms.front(), sorted.front(), median, sorted.back(), sum / frames,
             median > 0 ? 1000.0 / median : 0.0);
    }
    return 0;
  }
  if (mode == "desktop" || mode == "gui") {
    // desktop [--tui] [--windowed WxH] [--dump path.ppm [--frames N]]
    // Default: fullscreen at the display's real mode, the canvas as the face.
    bool force_tui = false, windowed = false;
    uint32_t ww = 0, wh = 0;
    std::string dump; int frames = 3;
    bool headless = false;
    std::string start_url;
    std::vector<std::string> scripted;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--tui" || args[i] == "-t") force_tui = true;
      else if (args[i] == "--windowed" || args[i] == "-w" || args[i] == "--window" || args[i] == "-window") {
        windowed = true;
        if (i + 1 < args.size() && sscanf(args[i + 1].c_str(), "%ux%u", &ww, &wh) == 2 && ww && wh) {
          ++i;
        } else if (!ww || !wh) {
          ww = 1440;
          wh = 900;
        }
      }
      else if (args[i] == "--size" && i + 1 < args.size()) {
        if (sscanf(args[++i].c_str(), "%ux%u", &ww, &wh) == 2 && ww && wh) {
          windowed = true;
        }
      }
      else if (args[i] == "--dump" && i + 1 < args.size()) dump = args[++i];
      else if (args[i] == "--frames" && i + 1 < args.size()) frames = atoi(args[++i].c_str());
      // No display at all: paint into the framebuffer and dump. `--open` puts a
      // url on the web pane first; `--do frame:type:text`, `--do frame:key:Enter`,
      // `--do frame:click:x,y`, `--do frame:move:x,y` (the pointer, no press) and
      // `--do frame:down:x,y` / `--do frame:up:x,y` (a drag) and
      // `--do frame:wheel:n` and `--do frame:ctrl:l` drive it, so a page can be typed into and looked at with nobody in front
      // of the machine.
      else if (args[i] == "--headless") headless = true;
      else if (args[i] == "--open" && i + 1 < args.size()) start_url = args[++i];
      else if (args[i] == "--do" && i + 1 < args.size()) scripted.push_back(args[++i]);
      else if (args[i] == "--no-panels" || args[i] == "--hide-panels") setenv("AOS_HIDE_PANELS", "1", 1);
      else if (args[i] == "--browser" || args[i] == "--browser-only") { setenv("AOS_BROWSER_ONLY", "1", 1); setenv("AIOS_NO_GPU_DISPLAY", "1", 1); windowed = true; }
      else if (args[i] == "--collapsed" || args[i] == "--collapsed-panels") setenv("AOS_COLLAPSED_PANELS", "1", 1);
      // The environment variable is the live path — canvas.cpp reads it. These
      // also wrote a scene file at one host's absolute path, which no other
      // machine reads, and which only the kernel (not yet built at argv time)
      // can site correctly. `display show` does that properly.
      else if (args[i] == "--scene" && i + 1 < args.size()) { setenv("AOS_DESKTOP_SCENE", args[++i].c_str(), 1); }
      else if (args[i] == "--scene=cyber" || args[i] == "--cyber") { setenv("AOS_DESKTOP_SCENE", "cyber", 1); }
      else if (args[i] == "--scene=orb" || args[i] == "--orb") { setenv("AOS_DESKTOP_SCENE", "orb", 1); }
      else if (args[i] == "--scene=beach" || args[i] == "--beach") { setenv("AOS_DESKTOP_SCENE", "beach", 1); }
    }
    aios::userland::HardwareCompositor compositor(windowed ? ww : 0, windowed ? wh : 0);
    compositor.set_fullscreen(!windowed);
    compositor.set_console(s.me.get());
    // The frame count stands on its own: a headless run without --dump still
    // runs --frames frames (and dumps to the default path). It used to be
    // passed only alongside --dump, so `--frames 30` alone ran 3.
    compositor.set_dump(dump, frames > 0 ? frames : 3);
    if (headless) compositor.set_headless(true);
    if (!start_url.empty()) compositor.set_start_url(start_url);
    for (const auto& step : scripted) compositor.add_scripted_input(step);
    auto dispatcher = [&s](const std::string& cmd, aios::userland::OutputCallback on_line) {
      Out captured_out([&on_line](const std::string& chunk) {
        std::stringstream ss(chunk);
        std::string l;
        while (std::getline(ss, l)) {
          if (!l.empty() && l.back() == '\r') l.pop_back();
          on_line(l);
        }
      });
      captured_out.set_colour(false);
      auto* old_out = s.out;
      s.out = &captured_out;
      auto r = run_line(s, cmd);
      if (!r.ok()) on_line("error: " + r.error().str());
      s.out = old_out;
    };
    // How Stop reaches the mind: the desktop sets the session's interrupt, and
    // the cognitive loop checks it wherever it can yield.
    compositor.set_interrupt([&s]() { s.interrupt_mind(); });
    // The desktop is the long-lived process on this machine, so it is what
    // pumps the service manager. Without this `restart=always` meant nothing:
    // the policy was recorded and never acted on.
    compositor.set_ticker([&s]() { (void)aios::cli::supervise_services(s); });
    // Deliberately NOT starting boot services here. Each process has its own
    // manager, so a desktop and a resident both starting them means two of
    // every service — two processes on one port, one of which loses. The
    // resident is the machine's always-on process and is the single owner; the
    // desktop still supervises anything started from its own shell pane.
    return compositor.run_desktop(*s.kernel, dispatcher, force_tui) ? 0 : 1;
  }
  if (mode == "info") { return run_line(s, "info").ok() ? 0 : 1; }
  if (mode == "exec") {
    for (size_t i = 1; i < args.size(); ++i) { auto r = run_line(s, args[i]); if (!r.ok()) { std::cerr << (s.colour ? "\033[31m" : "") << "FAILED: " << r.error().str() << (s.colour ? "\033[0m" : "") << "\n"; return 1; } }
    return 0;
  }
  if (mode == "run") {
    if (args.size() < 2) { usage(); return 2; }
    std::ifstream in(args[1]);
    if (!in) { std::cerr << "cannot read " << args[1] << "\n"; return 1; }
    std::stringstream ss; ss << in.rdbuf();
    auto r = run_script(s, ss.str(), args[1]);
    // The exit status says what the summary line says: any unexpected failure
    // the run counted is a failed run, not only the one that stopped it.
    const bool clean = r.ok() && s.failures == 0;
    std::cout << (clean ? (s.colour ? "\033[32m" : "") : (s.colour ? "\033[31m" : "")) << args[1] << ": " << s.commands << " commands, " << s.failures << " unexpected failure(s)" << (s.colour ? "\033[0m" : "") << "\n";
    return clean ? 0 : 1;
  }
  if (mode == "shell") {
    std::cout << (s.colour ? "\033[1;36m" : "") << "Kernel " << (s.colour ? "\033[0m\033[36m" : "") << s.kernel->self().short_str() << (s.colour ? "\033[0m" : "") << " · console entity " << s.me->self().short_str() << " · root " << root << " · type help or ask a question directly\n";
    (void)start_boot_services(s);
    std::string line;
    while (true) {
      // Anything standing that has come due runs here, before the prompt is
      // offered again: work that was ordered once, happening without being
      // asked for again.
      run_due_directives(s);
      std::cout << (s.colour ? "\033[1;32maios> \033[0m" : "aios> ") << std::flush;
      // Wait for a line WITHOUT going deaf. A resident machine spends most of its
      // life with nobody typing, and that is exactly when its standing directives
      // are meant to run — so while the input is quiet, keep carrying out whatever
      // has come due. Otherwise a daily order on a headless machine waits forever
      // for someone to press a key. Anything already buffered in cin is read
      // first: a line that arrived alongside the last one must not sit unseen
      // while poll() reports the descriptor idle.
      static std::string input_buf;
      bool got_line = false;
      for (;;) {
        auto nl = input_buf.find('\n');
        if (nl != std::string::npos) {
          line = input_buf.substr(0, nl);
          input_buf.erase(0, nl + 1);
          got_line = true;
          break;
        }
        pollfd pf{STDIN_FILENO, POLLIN, 0};
        int pr = ::poll(&pf, 1, 250);
        if (pr > 0) {
          char buf[4096];
          ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
          if (n > 0) {
            input_buf.append(buf, static_cast<size_t>(n));
            continue;
          } else if (n == 0) {
            got_line = false;
            break;
          }
        }
        if (pr < 0 && errno != EINTR) { got_line = false; break; }
        run_due_directives(s);
        pump_services(s);
        if (s.kernel->service()) s.kernel->service()->poll();
      }
      if (!got_line) break;
      // Strip ANSI escape sequences and orphan mouse escape artifacts (like \033[A or orphan [A)
      std::string clean;
      for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == 27) {
          if (i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && ((line[i] >= '0' && line[i] <= '9') || line[i] == ';' || line[i] == '?' || line[i] == '<')) i++;
          }
          continue;
        }
        if (clean.empty() && (line.compare(i, 2, "[A") == 0 || line.compare(i, 2, "[B") == 0)) { i++; continue; }
        clean.push_back(line[i]);
      }
      line = clean;
      if (line.empty()) continue;
      if (line == "quit" || line == "exit") break;
      auto r = run_line(s, line);
      if (!r.ok()) std::cout << (s.colour ? "\033[31merror: " : "error: ") << r.error().str() << (s.colour ? "\033[0m" : "") << "\n";
    }
    return 0;
  }
  // Leave it running. This is the mode where the machine does what it was told
  // to do while nobody is at it: nothing is typed, and the standing directives
  // come due and are carried out.
  if (mode == "attend") {
    uint64_t seconds = args.size() > 1 ? (uint64_t)std::strtoull(args[1].c_str(), nullptr, 10) : 0;
    std::cout << (s.colour ? "\033[1;36m" : "") << "attending" << (s.colour ? "\033[0m" : "") << " — kernel "
              << s.kernel->self().short_str() << ", console " << s.me->self().short_str()
              << (seconds ? ", for " + std::to_string(seconds) + "s" : ", until stopped") << "\n";
    // Commands to run before attending, so a node can be served for the
    // fleet from one invocation: `aios attend 0 "node serve 7700 <token>"`.
    for (size_t i = 2; i < args.size(); ++i) {
      auto r = run_line(s, args[i]);
      if (!r.ok()) { std::cerr << (s.colour ? "\033[31m" : "") << "FAILED: " << args[i] << ": " << r.error().str() << (s.colour ? "\033[0m" : "") << "\n"; return 1; }
    }
    uint64_t started = s.kernel->host().monotonic_ns();
    for (;;) {
      run_due_directives(s);
      pump_services(s);
      if (s.kernel->service()) s.kernel->service()->poll();
      hal::sleep_ms(200);
      if (seconds && (s.kernel->host().monotonic_ns() - started) / 1000000000ull >= seconds) break;
    }
    std::cout << "attended for " << (s.kernel->host().monotonic_ns() - started) / 1000000000ull << "s\n";
    return 0;
  }
  // `aios models` — a standalone ModelController surface (list/set/start/
  // stop/reload/status) that mirrors the GUI's /api/models + /api/config
  // pair, exercised here directly against real in-memory state without
  // needing a live kernel session.
  if (mode == "models") {
    // What is actually reachable: every endpoint in the roster is asked, and
    // a model is listed because an endpoint named it just now. With a model
    // name given, say which host serves it (exit 1 when none does).
    aios::cli::ModelController mc;
    size_t up = mc.discover();
    std::cout << mc.render();
    if (args.size() > 1) {
      std::string where;
      std::string url = mc.endpoint_for(args[1], &where);
      if (url.empty()) { std::cout << args[1] << ": not served by any reachable endpoint\n"; return 1; }
      std::cout << args[1] << ": served by " << where << " at " << url << "\n";
    }
    return up ? 0 : 1;
  }
  // Direct command invocation fallback: allow running any AIOS command directly
  // (e.g. 'aios voice status', 'aios task list', 'aios voice speak ...')
  std::string cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) cmd += " ";
    if (args[i].find(' ') != std::string::npos || args[i].find('\t') != std::string::npos ||
        args[i].find('"') != std::string::npos || args[i].find('\'') != std::string::npos) {
      std::string escaped = "\"";
      for (char ch : args[i]) {
        if (ch == '"' || ch == '\\') escaped += '\\';
        escaped += ch;
      }
      escaped += "\"";
      cmd += escaped;
    } else {
      cmd += args[i];
    }
  }
  auto r = run_line(s, cmd);
  if (!r.ok()) {
    std::cerr << (s.colour ? "\033[31merror: " : "error: ") << r.error().str() << (s.colour ? "\033[0m" : "") << "\n";
    return 1;
  }
  return 0;
}
