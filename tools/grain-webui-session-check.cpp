// A web UI session that is dropped (reaped, evicted, or the app stopped) must take its pane with it.
//
// Every request without the session cookie starts a session: the program runs in a VM of its own and its ui.pane
// registers a pane. The pane holds the program's closures, so a pane left registered keeps the whole VM alive. On
// atl's app host the dock board's cookieless requests (nginx's auth check, cross-origin screens) leaked ~4 MB each
// this way until the cgroup killed the host.

#include "aios/userland/grain.hpp"
#include "aios/userland/grain_webui.hpp"

#include <cstdio>
#include <memory>
#include <string>

using namespace aios::userland::grain;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

static const char* kProgram = R"GR(
let big = []
let i = 0
while i < 2000 { push(big, "row " + str(i)); i = i + 1 }
ui.pane({
  "title": "check",
  "view": fn() { return el("div", {}, str(len(big))) },
  "routes": {"/r": fn(req) { return {"status": 200, "content_type": "text/plain", "body": "ok"} }}
})
)GR";

int main() {
  const size_t before = pane_count();
  constexpr size_t kMax = 4;
  {
    WebUi::Host h;
    h.max_sessions = kMax;
    h.load_source = [](std::string& src, std::string& v, std::string&) { src = kProgram; v = "v1"; return true; };
    h.current_version = [](std::string& v, std::string&) { v = "v1"; return true; };
    h.setup = [](Vm& vm) -> std::shared_ptr<void> { install_builtins(vm); return nullptr; };
    WebUi ui(std::move(h));
    ui.log = [](const std::string& m) { std::fprintf(stderr, "webui: %s\n", m.c_str()); };

    int ok_routes = 0, ok_pages = 0;
    for (int i = 0; i < 40; ++i) {   // no cookie: each one a new session, as nginx's auth check and the screens are
      WebUi::Request r;
      r.method = "GET";
      r.path = (i % 2) ? "/r" : "/";
      if (ui.handle(r).status == 200) ++(r.path == "/r" ? ok_routes : ok_pages);
    }
    check(ok_routes == 20 && ok_pages == 20, "40 cookieless requests answered (" + std::to_string(ok_routes) + " routes, " +
                                                 std::to_string(ok_pages) + " pages)");
    check(ui.sessions() <= kMax, "sessions held to the cap: " + std::to_string(ui.sessions()));
    const size_t live = pane_count() - before;
    check(live <= kMax, "panes left registered while running: " + std::to_string(live) + " (at most " + std::to_string(kMax) +
                            ", one per session kept)");
  }
  const size_t after = pane_count() - before;
  check(after == 0, "panes left registered after the web UI is gone: " + std::to_string(after));

  // A pane whose routes run in workers (the dock board, the touches report): callers that never send a cookie back
  // (nginx's auth check, another site's EventSource) share one copy of the program rather than each starting one.
  {
    std::string program = kProgram;
    program.replace(program.find("\"title\": \"check\","), 17, "\"title\": \"check\", \"workers\": true,");
    WebUi::Host h;
    h.max_sessions = kMax;
    h.load_source = [program](std::string& src, std::string& v, std::string&) { src = program; v = "v1"; return true; };
    h.current_version = [](std::string& v, std::string&) { v = "v1"; return true; };
    h.setup = [](Vm& vm) -> std::shared_ptr<void> { install_builtins(vm); return nullptr; };
    WebUi ui(std::move(h));
    ui.log = [](const std::string& m) { std::fprintf(stderr, "webui: %s\n", m.c_str()); };
    int ok = 0;
    bool cookie = false;
    for (int i = 0; i < 30; ++i) {
      WebUi::Request r;
      r.method = "GET";
      r.path = "/r";
      auto res = ui.handle(r);
      if (res.status == 200 && res.body == "ok") ++ok;
      for (const auto& hd : res.headers) if (hd.first == "Set-Cookie") cookie = true;
    }
    check(ok == 30, "30 cookieless route calls on a workers pane answered: " + std::to_string(ok));
    check(!cookie, "no cookie handed to a caller that shares the routes' copy");
    check(ui.sessions() == 1, "one copy of the program for all of them: " + std::to_string(ui.sessions()) + " sessions");
    check(pane_count() - before == 1, "one pane registered: " + std::to_string(pane_count() - before));
    WebUi::Request page;
    page.method = "GET";
    page.path = "/";
    check(ui.handle(page).status == 200 && ui.sessions() == 2, "the page still gets a session of its own");
  }
  check(pane_count() == before, "and none left after: " + std::to_string(pane_count() - before));
  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
