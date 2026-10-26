#pragma once
// grain apps: a program packaged with what it needs to run, kept in versions, hosted, watched and controlled -
// on any machine grain runs on (grain_app.cpp). Nothing here needs aios: a grain on its own hosts apps from a
// directory (DirStore) and is controlled over grain's own encrypted link. aios hosts the same apps and adds what
// only it has (the fleet, its SQL engine, Things) through the Store, the capabilities and the VM setup it gives.
//
// An app is a directory with an `app.gr` that sets `app` to a map:
//
//   let app = {
//     "name": "stickerfeed",            // letters, digits, - and _
//     "main": "service.gr",             // what runs (default main.gr)
//     "kind": "web",                    // "web": its ui.pane is served on `port`; "worker": it runs on its own
//     "port": 8571, "listen": "local",  // "local" (127.0.0.1) or "all"
//     "settings": {"ASQL_HOST": "127.0.0.1"},   // what getenv() gives it, unless this machine says otherwise
//     "secrets": ["ASQL_PASSWORD"],     // names it needs; their values are set on each machine, never packaged
//     "requires": ["asql"],             // what the host must have: a host without it refuses the deploy
//     "health": {"path": "/", "ok": [200, 401, 404]},   // asked after every start (default: any status below 500)
//     "check": "checks.gr",             // its check(app) runs after a deploy: true, or what went wrong
//     "deployed": "deployed.gr",        // its deployed(app) runs last, once the version is up (and again for the
//                                       // version a failed deploy goes back to): true or nothing, else what failed
//     "restart": "always"               // or "on-failure", "never"
//   }
//
// A version is the app's files as a bundle, named by their hash: deploying the same files twice is one version,
// and a rollback is a switch back to a version kept.
#include <atomic>
#include <iosfwd>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/grain_webui.hpp"

namespace aios::userland::grain::apps {

// An app's files: path (relative, '/'-separated) -> contents. `id` is their hash; `name` the manifest's.
struct Bundle {
  std::string name;
  std::string id;
  std::map<std::string, std::string> files;
  std::string encode() const;
  static Result<Bundle> decode(const std::string& bytes);
  void seal_id();   // sets id from the files
};

// The directory's files (hidden ones, and anything under a hidden directory, left out), named from its app.gr.
Result<Bundle> pack_dir(hal::Host& host, const std::string& dir);

struct Manifest {
  std::string name, main = "main.gr", kind = "web", listen = "local", restart = "always", check, deployed, description;
  int port = 0;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<std::string> secrets, needs;   // needs: the manifest's "requires"
  std::string health_path = "/";
  std::vector<int> health_ok;              // empty: any status below 500
  int health_timeout_s = 30;
  uint64_t max_steps = 200000000;          // instructions one request or render may run (0 = unlimited)
  size_t max_body = 16u << 20;
};

// Runs the bundle's app.gr (no sockets, no files but the bundle's, a step limit) and reads its `app`.
Result<Manifest> read_manifest(const Bundle& b);

// Every .gr in the bundle compiles; what does not, named.
Result<void> compile_all(const Bundle& b);

// What a machine keeps of its apps.
struct AppRecord {
  std::string current;                                      // the version running (or that would run)
  std::vector<std::pair<std::string, uint64_t>> history;    // versions switched to, oldest first, with when (wall ns)
  bool running = false;                                     // what the operator asked for: start/stop survive a restart
};

class Store {
 public:
  virtual ~Store() = default;
  virtual Result<void> put_version(const Bundle& b) = 0;
  virtual Result<Bundle> get_version(const std::string& app, const std::string& id) = 0;
  virtual std::vector<std::string> versions(const std::string& app) = 0;
  virtual std::vector<std::string> apps() = 0;
  virtual AppRecord record(const std::string& app) = 0;
  virtual Result<void> put_record(const std::string& app, const AppRecord& r) = 0;
  // This machine's settings for an app; `secret` ones are kept sealed.
  virtual std::map<std::string, std::string> settings(const std::string& app, bool secret) = 0;
  virtual Result<void> put_settings(const std::string& app, const std::map<std::string, std::string>& values, bool secret) = 0;
  virtual void append_log(const std::string& app, const std::string& line) = 0;
  virtual std::vector<std::string> read_log(const std::string& app, size_t last_n) = 0;
  // Where the app may keep files of its own (relative paths in read_file/write_file); "" when there is none.
  virtual std::string data_dir(const std::string& app) = 0;
  virtual Result<void> remove_app(const std::string& app) = 0;
};

// Apps kept in a directory: <home>/apps/<name>/{versions/<id>.app, record.json, settings.json, secrets.sealed,
// data/, log}. Secrets are sealed (AES-256-GCM) with <home>/machine.key, which only its owner can read.
std::unique_ptr<Store> make_dir_store(hal::Host& host, const std::string& home);

struct AppStatus {
  std::string name, version, state, last_error, kind;
  int port = 0;
  uint64_t since_ns = 0;     // wall clock of the last start
  uint64_t restarts = 0, requests = 0, failures = 0;
};

// Runs a machine's apps in this process: each one's VMs on threads of their own, a web app's HTTP served over
// the host layer's sockets (hal::net, so wherever grain runs), restarts by its policy, its output kept as its log.
class Host {
 public:
  struct Options {
    // Gives a VM of an app its builtins (the embedder's: grain's own CLI, or aios). Called for every VM it starts.
    std::function<void(Vm&)> setup_vm;
    std::vector<std::string> capabilities;   // what an app's `requires` may name ("grain" always)
    std::string machine;                     // this machine's name, in status and logs
    // A web app's route given to a fleet member (on_machine): its request answered there, by the embedder's link
    // (aios: the fleet). false when `member` is this machine, so the route answers here. Unset: every route here.
    std::function<bool(const std::string& app, const std::string& member, const WebUi::Request& req, WebUi::Response& res)> forward;
  };
  // What a forwarded request is answered with on the member it was given to: this host's own app `app`, asked over
  // its local port as a client would ask it. The caller's link carries the request in and the answer out.
  Result<WebUi::Response> answer_forwarded(const std::string& app, const WebUi::Request& req);
  Host(hal::Host& host, Store& store, Options opt);
  ~Host();

  // Starts every app the operator left running, and the watcher that restarts what stops.
  void start_all();
  void shutdown();

  // A new version: kept, switched to, started, health-asked and checked; on any failure the version before comes
  // back. `report` gets what happened, line by line.
  Result<std::string> deploy(const Bundle& b, std::vector<std::string>& report);
  Result<void> start(const std::string& app);
  Result<void> stop(const std::string& app);
  Result<void> restart(const std::string& app);
  Result<std::string> rollback(const std::string& app, std::vector<std::string>& report);
  Result<void> remove(const std::string& app);
  std::vector<AppStatus> status();
  std::vector<std::string> logs(const std::string& app, size_t last_n);
  Result<void> set_settings(const std::string& app, const std::map<std::string, std::string>& values, bool secret,
                            const std::vector<std::string>& unset);
  std::vector<std::string> settings_names(const std::string& app, bool secret);

  // One request, as JSON, answered as JSON: what the control link, a console or another app sends.
  std::string control(const std::string& request_json);

  struct Instance;   // one running version of an app (grain_app.cpp)

 private:
  hal::Host& host_;
  Store& store_;
  Options opt_;
  std::mutex mu_;       // running_
  std::mutex ops_mu_;   // one deploy / start / stop / rollback at a time
  std::map<std::string, std::shared_ptr<Instance>> running_;
  std::atomic<bool> stopping_{false};
  hal::Thread watcher_;

  // port_override: -1 the manifest's port, 0 a spare one (a new version tried beside the running one); `keep`:
  // put it in running_ (a version being tried is not, until it takes over)
  Result<void> launch_locked(const std::string& app, const std::string& version, int port_override = -1,
                             std::shared_ptr<Instance>* made = nullptr, bool keep = true);
  void start_accepting(const std::shared_ptr<Instance>& in);
  Result<void> rebind(const std::shared_ptr<Instance>& in, int port);
  void hand_over(const std::shared_ptr<Instance>& from, const std::shared_ptr<Instance>& to);
  void halt(const std::shared_ptr<Instance>& in);
  Result<void> wait_healthy(const std::shared_ptr<Instance>& in, int timeout_s, std::string& why);
  Result<void> run_check(const std::shared_ptr<Instance>& in, std::string& why);
  // `said`: what it printed (also in the log)
  Result<void> run_hook(const std::shared_ptr<Instance>& in, const std::string& file, const char* fn_name, std::string& why,
                        std::vector<std::string>* said = nullptr);
  void watch();
  void log(const std::string& app, const std::string& line);
};

// The control link: grain's encrypted node link (node/secure.cpp) on a port, opened by anyone holding `token`.
// One frame in (a request), one frame out (the answer), as many as the caller sends.
class ControlServer {
 public:
  ControlServer(hal::Host& host, Host& apps, std::string token);
  ~ControlServer();
  Result<uint16_t> listen(uint16_t port, bool loopback_only);
  void close();

 private:
  hal::Host& host_;
  Host& apps_;
  std::string token_;
  std::unique_ptr<hal::Listener> listener_;
  std::atomic<bool> stop_{false};
  hal::Thread accept_;
};

Result<std::string> call_control(hal::Host& host, const std::string& address, uint16_t port, const std::string& token,
                                 const std::string& request_json, int timeout_ms = 600000);

// `grain app <command> ...` for any embedder (grain_app_cli.cpp): what it prints goes to `out` and `errs`, and it
// returns an exit status. `home` is where this machine's host keeps its apps ("" for GRAIN_HOME / ~/.grain);
// setup_vm and capabilities are for `app run`, which hosts the app in this process.
struct AppCommandEnv {
  std::string home;
  std::function<void(Vm&)> setup_vm;
  std::vector<std::string> capabilities;
};
int run_app_command(hal::Host& host, const std::vector<std::string>& args, const AppCommandEnv& env, std::ostream& out, std::ostream& errs);

// A file of a program with its imports put in place - `import "x.gr"` on a line of its own, relative to the file -
// each file once, read through `read` (an app's own files). What a Thing's program becomes for the engine, which
// keeps a program as one text. In an app's VMs it is the builtin app_program(path).
Result<std::string> inline_imports(const std::string& path, const std::function<bool(const std::string&, std::string&)>& read);

// A machine's control token, made on first use (<home>/host.key, readable by its owner only).
Result<std::string> host_token(hal::Host& host, const std::string& home);

}  // namespace aios::userland::grain::apps
