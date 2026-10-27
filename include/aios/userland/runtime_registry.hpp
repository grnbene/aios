// What grain runtimes are running on this machine, for `grain ps`.
//
// Each process that runs grain - `grain run` / `grain serve`, an app host (`grain host`, `aios serve`), an ASQL
// engine (`aios compute serve sql=1`), `aios exec "grain ..."` - writes a record of itself to a directory private
// to its user: $XDG_RUNTIME_DIR/grain, or /tmp/grain-<uid> (mode 0700, each record 0600, written whole and renamed
// into place). The record says what it runs, from which root, the ports it listens on, and what it holds (an app
// host its apps, an engine its Things), refreshed every few seconds by the process itself. It is removed at a
// normal exit; a record whose process is gone (or whose pid is now another process) is ignored and cleared by the
// next reader. No socket, no password: the reader only reads files its own user wrote.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace aios::userland::runtime {

// The directory the records are in ("" where there is none: no writable place).
std::string registry_dir();

// This process is `kind` ("grain run", "grain serve", "app host", "asql", ...) running `what` (a program, a
// command line) from `root` (its root or home directory, "" for none). Writes the record; later calls change it.
void announce(const std::string& kind, const std::string& what, const std::string& root = "");
// A port this process listens on, and what for ("control", "web", "mysql", an app's name). Removed with forget_port.
void add_port(int port, const std::string& label);
void forget_port(int port);
// A field of the record kept current by the process: `fill` returns its JSON (a list or map), called now and then
// every `seconds` on a thread of the registry's own. One per key; a later call replaces it.
void keep_current(const std::string& key, std::function<std::string()> fill, int seconds = 3);
// Stops keeping `key` (waits for a fill in progress): call it before what the fill reads goes away.
void stop_keeping(const std::string& key);
// Removes this process's record (also done at exit).
void withdraw();

// The homes of the app hosts running on this machine for this user (their records' roots), oldest first.
std::vector<std::string> app_host_homes();

// `grain ps`: the live records, and on Linux the grain / aios processes of this user that wrote none (an older
// binary, or a program run some other way) with the ports they listen on. json: the records as one JSON list.
std::string ps_text(bool json = false);

}  // namespace aios::userland::runtime
