#include "aios/userland/lsp_client.hpp"

#if defined(AIOS_METAL) || defined(_WIN32)
// A language server is another PROCESS, on another machine's terms: this file
// asks a host for fork, execvp, pipe, dup2, waitpid and kill. A bare-metal AI OS
// has no host to ask, so on the metal the editor simply has no language server —
// every call here is a no-op that answers "nothing found" and the editor keeps
// its own syntax highlighting and navigation. Windows takes the same stub for
// now: the real body below is written straight to fork/execvp/pipe, which this
// host does not have (see hal::process for the Windows-native equivalent that
// a real port would sit on).
//
// This has to be a stub rather than an omission because compositor.hpp holds an
// LspClient, so the symbols must exist. Leaving the hosted body compiled for the
// metal target is what broke `metal/build.sh`: the POSIX calls dragged in
// libstdc++'s iostream and basic_file members, and the link died on writev,
// poll, ioctl and lseek64 — with nothing in the error naming this file.
namespace aios::userland {

LspClient::LspClient(LspServerKind kind, const std::string& binary_cmd, const std::string& workspace_root)
    : kind_(kind), binary_cmd_(binary_cmd), workspace_root_(workspace_root) {}
LspClient::~LspClient() {}

bool LspClient::start() { return false; }
void LspClient::stop() { running_ = false; }

void LspClient::did_open(const std::string&, SourceLang, const std::string&) {}
void LspClient::did_change(const std::string&, const std::string&, int) {}
void LspClient::did_save(const std::string&) {}
void LspClient::did_close(const std::string&) {}

// The callback fires with an empty result rather than never firing at all: a
// caller that waits for an answer before drawing would otherwise hang forever.
uint64_t LspClient::request_completion(const std::string&, int, int,
                                       std::function<void(const std::vector<LspCompletionItem>&)> cb) {
  if (cb) cb({});
  return 0;
}
uint64_t LspClient::request_definition(const std::string&, int, int,
                                       std::function<void(const std::vector<LspLocation>&)> cb) {
  if (cb) cb({});
  return 0;
}
uint64_t LspClient::request_hover(const std::string&, int, int,
                                  std::function<void(const LspHoverInfo&)> cb) {
  if (cb) cb(LspHoverInfo{});
  return 0;
}
uint64_t LspClient::request_references(const std::string&, int, int,
                                       std::function<void(const std::vector<LspLocation>&)> cb) {
  if (cb) cb({});
  return 0;
}

void LspClient::poll_responses() {}
void LspClient::send_json_rpc(const std::string&) {}
void LspClient::handle_message(const std::string&) {}
void LspClient::reader_loop() {}

LspClientManager& LspClientManager::instance() {
  static LspClientManager m;
  return m;
}
std::shared_ptr<LspClient> LspClientManager::get_or_start_client(SourceLang, const std::string&) {
  return nullptr;
}
std::string LspClientManager::server_for(SourceLang) { return ""; }
void LspClient::write_frame(const std::string&) {}
void LspClientManager::shutdown_all() {}

}  // namespace aios::userland

#else

#include "aios/channel/rpc_value.hpp"
#include "aios/hal/sync.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <vector>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace aios::userland {

using channel::RpcValue;
using channel::rpc_parse;

namespace {

std::string path_to_uri(const std::string& path) {
  if (path.rfind("file://", 0) == 0) return path;
  if (!path.empty() && path[0] == '/') return "file://" + path;
  // A relative path is relative to where we run; the server has to be told
  // the real place, or file:///x.py names a file at the filesystem root.
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd))) return "file://" + std::string(cwd) + "/" + path;
  return "file:///" + path;
}

std::string uri_to_path(const std::string& uri) {
  if (uri.rfind("file://", 0) == 0) return uri.substr(7);
  return uri;
}

std::string get_binary_for_lang(LspServerKind kind) {
  switch (kind) {
    case LspServerKind::Clangd: return "clangd";
    case LspServerKind::TsServer: return "typescript-language-server";
    case LspServerKind::PyLsp: return "pylsp";
    case LspServerKind::Gopls: return "gopls";
    case LspServerKind::RustAnalyzer: return "rust-analyzer";
    default: return "";
  }
}

bool executable_in_path(const std::string& name) {
  if (name.empty()) return false;
  const char* path_env = std::getenv("PATH");
  if (!path_env) return false;
  std::string paths = path_env;
  std::stringstream ss(paths);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    std::string full = dir + "/" + name;
    if (access(full.c_str(), X_OK) == 0) return true;
  }
  return false;
}

} // namespace

LspClient::LspClient(LspServerKind kind, const std::string& binary_cmd, const std::string& workspace_root)
    : kind_(kind), binary_cmd_(binary_cmd.empty() ? get_binary_for_lang(kind) : binary_cmd),
      workspace_root_(workspace_root.empty() ? "." : workspace_root) {}

LspClient::~LspClient() {
  stop();
}

bool LspClient::start() {
  if (running_.load()) return true;

  // A language server is a real program on PATH, or there is none. There used
  // to be an "in-process daemon" here that answered every request itself --
  // hover "Symbol documentation at Ln N", a definition one line up, every
  // non-empty line as a reference -- and the editor showed that as if a server
  // had said it. With no server the client now does not start, and the editor
  // says "no language server for <lang>".
  if (!executable_in_path(binary_cmd_)) return false;

  int pipe_in[2];  // client -> server stdin
  int pipe_out[2]; // server stdout -> client
  if (pipe(pipe_in) < 0) return false;
  if (pipe(pipe_out) < 0) { close(pipe_in[0]); close(pipe_in[1]); return false; }
  fcntl(pipe_in[1], F_SETFD, FD_CLOEXEC);
  fcntl(pipe_out[0], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) {
    close(pipe_in[0]); close(pipe_in[1]);
    close(pipe_out[0]); close(pipe_out[1]);
    return false;
  }

  if (pid == 0) {
    dup2(pipe_in[0], STDIN_FILENO);
    dup2(pipe_out[1], STDOUT_FILENO);
    // Server logging must not scribble over the console the desktop owns.
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
    close(pipe_in[0]);
    close(pipe_out[1]);
    // Each server's own way of saying "speak LSP on stdio": clangd, pylsp,
    // gopls and rust-analyzer do it with no argument (pylsp rejects --stdio);
    // typescript-language-server needs --stdio.
    std::vector<std::string> argv_s = {binary_cmd_};
    if (kind_ == LspServerKind::TsServer) argv_s.push_back("--stdio");
    std::vector<char*> args;
    for (auto& a : argv_s) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(binary_cmd_.c_str(), args.data());
    _exit(127);
  }

  close(pipe_in[0]);
  close(pipe_out[1]);
  fd_in_ = pipe_in[1];
  fd_out_ = pipe_out[0];
  pid_ = pid;
  running_.store(true);
  initialized_ = false;
  reader_thread_ = aios::hal::Thread([this]() { reader_loop(); });

  RpcValue params = RpcValue::object();
  params.set("processId", RpcValue::number((uint64_t)getpid()));
  params.set("rootUri", RpcValue::text(path_to_uri(workspace_root_)));
  params.set("rootPath", RpcValue::text(workspace_root_));
  params.set("capabilities", RpcValue::object());

  RpcValue req = RpcValue::object();
  req.set("jsonrpc", RpcValue::text("2.0"));
  req.set("id", RpcValue::number(0));
  req.set("method", RpcValue::text("initialize"));
  req.set("params", params);
  write_frame(req.render());  // the one message allowed before the answer
  return true;
}

void LspClient::stop() {
  if (!running_.load()) return;
  running_.store(false);

  if (fd_in_ >= 0) { close(fd_in_); fd_in_ = -1; }
  if (fd_out_ >= 0) { close(fd_out_); fd_out_ = -1; }
  if (pid_ > 0) {
    kill(pid_, SIGTERM);
    int st;
    bool reaped = false;
    for (int i = 0; i < 50 && !reaped; ++i) {
      if (waitpid(pid_, &st, WNOHANG) == pid_) reaped = true;
      else usleep(10000);
    }
    if (!reaped) { kill(pid_, SIGKILL); waitpid(pid_, &st, 0); }
    pid_ = -1;
  }
  if (reader_thread_.joinable()) reader_thread_.join();
}

void LspClient::write_frame(const std::string& json_str) {
  if (fd_in_ < 0) return;
  std::string frame = "Content-Length: " + std::to_string(json_str.size()) + "\r\n\r\n" + json_str;
  size_t off = 0;
  while (off < frame.size()) {
    ssize_t n = write(fd_in_, frame.data() + off, frame.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;  // the server went away; is_running() will say so
    off += (size_t)n;
  }
}

void LspClient::send_json_rpc(const std::string& json_str) {
  // Nothing but `initialize` may reach a server before it has answered it, so
  // everything else waits here until the answer comes back.
  if (!initialized_) { held_until_init_.push_back(json_str); return; }
  write_frame(json_str);
}

void LspClient::reader_loop() {
  char buf[4096];
  std::string stream_buffer;

  while (running_.load() && fd_out_ >= 0) {
    ssize_t n = read(fd_out_, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { running_.store(false); break; }
    stream_buffer.append(buf, static_cast<size_t>(n));

    for (;;) {
      size_t pos = stream_buffer.find("\r\n\r\n");
      if (pos == std::string::npos) break;

      std::string header = stream_buffer.substr(0, pos);
      size_t cl_pos = header.find("Content-Length: ");
      if (cl_pos == std::string::npos) {
        stream_buffer.erase(0, pos + 4);
        continue;
      }
      size_t len = static_cast<size_t>(std::stoul(header.substr(cl_pos + 16)));
      if (stream_buffer.size() < pos + 4 + len) break; // Need more data

      std::string body = stream_buffer.substr(pos + 4, len);
      stream_buffer.erase(0, pos + 4 + len);

      {
        std::lock_guard<aios::hal::Mutex> lock(mutex_);
        response_queue_.push_back({body});
      }
    }
  }
}

void LspClient::did_open(const std::string& path, SourceLang lang, const std::string& content) {
  open_docs_[uri_to_path(path_to_uri(path))] = content;

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));
  doc.set("languageId", RpcValue::text(lang_name(lang)));
  doc.set("version", RpcValue::number(1));
  doc.set("text", RpcValue::text(content));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);

  RpcValue notif = RpcValue::object();
  notif.set("jsonrpc", RpcValue::text("2.0"));
  notif.set("method", RpcValue::text("textDocument/didOpen"));
  notif.set("params", params);

  send_json_rpc(notif.render());
}

void LspClient::did_change(const std::string& path, const std::string& content, int version) {
  open_docs_[uri_to_path(path_to_uri(path))] = content;

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));
  doc.set("version", RpcValue::number((uint64_t)version));

  RpcValue change = RpcValue::object();
  change.set("text", RpcValue::text(content));

  RpcValue changes = RpcValue::list();
  changes.push(change);

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);
  params.set("contentChanges", changes);

  RpcValue notif = RpcValue::object();
  notif.set("jsonrpc", RpcValue::text("2.0"));
  notif.set("method", RpcValue::text("textDocument/didChange"));
  notif.set("params", params);

  send_json_rpc(notif.render());
}

void LspClient::did_save(const std::string& path) {
  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);

  RpcValue notif = RpcValue::object();
  notif.set("jsonrpc", RpcValue::text("2.0"));
  notif.set("method", RpcValue::text("textDocument/didSave"));
  notif.set("params", params);

  send_json_rpc(notif.render());
}

void LspClient::did_close(const std::string& path) {
  open_docs_.erase(path);

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);

  RpcValue notif = RpcValue::object();
  notif.set("jsonrpc", RpcValue::text("2.0"));
  notif.set("method", RpcValue::text("textDocument/didClose"));
  notif.set("params", params);

  send_json_rpc(notif.render());
}

uint64_t LspClient::request_completion(const std::string& path, int line, int col,
                                       std::function<void(const std::vector<LspCompletionItem>&)> cb) {
  uint64_t req_id = next_id_.fetch_add(1);
  {
    std::lock_guard<aios::hal::Mutex> lock(mutex_);
    pending_completions_[req_id] = cb;
  }

  RpcValue pos = RpcValue::object();
  pos.set("line", RpcValue::number((uint64_t)line));
  pos.set("character", RpcValue::number((uint64_t)col));

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);
  params.set("position", pos);

  RpcValue req = RpcValue::object();
  req.set("jsonrpc", RpcValue::text("2.0"));
  req.set("id", RpcValue::number(req_id));
  req.set("method", RpcValue::text("textDocument/completion"));
  req.set("params", params);

  send_json_rpc(req.render());
  return req_id;
}

uint64_t LspClient::request_definition(const std::string& path, int line, int col,
                                       std::function<void(const std::vector<LspLocation>&)> cb) {
  uint64_t req_id = next_id_.fetch_add(1);
  {
    std::lock_guard<aios::hal::Mutex> lock(mutex_);
    pending_definitions_[req_id] = cb;
  }

  RpcValue pos = RpcValue::object();
  pos.set("line", RpcValue::number((uint64_t)line));
  pos.set("character", RpcValue::number((uint64_t)col));

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);
  params.set("position", pos);

  RpcValue req = RpcValue::object();
  req.set("jsonrpc", RpcValue::text("2.0"));
  req.set("id", RpcValue::number(req_id));
  req.set("method", RpcValue::text("textDocument/definition"));
  req.set("params", params);

  send_json_rpc(req.render());
  return req_id;
}

uint64_t LspClient::request_hover(const std::string& path, int line, int col,
                                   std::function<void(const LspHoverInfo&)> cb) {
  uint64_t req_id = next_id_.fetch_add(1);
  {
    std::lock_guard<aios::hal::Mutex> lock(mutex_);
    pending_hovers_[req_id] = cb;
  }

  RpcValue pos = RpcValue::object();
  pos.set("line", RpcValue::number((uint64_t)line));
  pos.set("character", RpcValue::number((uint64_t)col));

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);
  params.set("position", pos);

  RpcValue req = RpcValue::object();
  req.set("jsonrpc", RpcValue::text("2.0"));
  req.set("id", RpcValue::number(req_id));
  req.set("method", RpcValue::text("textDocument/hover"));
  req.set("params", params);

  send_json_rpc(req.render());
  return req_id;
}

uint64_t LspClient::request_references(const std::string& path, int line, int col,
                                      std::function<void(const std::vector<LspLocation>&)> cb) {
  uint64_t req_id = next_id_.fetch_add(1);
  {
    std::lock_guard<aios::hal::Mutex> lock(mutex_);
    pending_references_[req_id] = cb;
  }

  RpcValue pos = RpcValue::object();
  pos.set("line", RpcValue::number((uint64_t)line));
  pos.set("character", RpcValue::number((uint64_t)col));

  RpcValue doc = RpcValue::object();
  doc.set("uri", RpcValue::text(path_to_uri(path)));

  RpcValue ctx = RpcValue::object();
  ctx.set("includeDeclaration", RpcValue::boolean(true));

  RpcValue params = RpcValue::object();
  params.set("textDocument", doc);
  params.set("position", pos);
  params.set("context", ctx);

  RpcValue req = RpcValue::object();
  req.set("jsonrpc", RpcValue::text("2.0"));
  req.set("id", RpcValue::number(req_id));
  req.set("method", RpcValue::text("textDocument/references"));
  req.set("params", params);

  send_json_rpc(req.render());
  return req_id;
}

void LspClient::poll_responses() {
  for (int pass = 0; pass < 5; ++pass) {
    std::vector<ResponseQueueItem> queue;
    {
      std::lock_guard<aios::hal::Mutex> lock(mutex_);
      queue.swap(response_queue_);
    }
    if (queue.empty()) break;

    for (const auto& item : queue) {
      handle_message(item.message);
    }
  }
}

void LspClient::handle_message(const std::string& msg) {
  auto parse_res = rpc_parse(msg);
  if (!parse_res.ok()) return;
  const RpcValue& root = *parse_res;

  // Handle Notifications (method present, no id)
  if (root.has("method") && !root.has("id")) {
    std::string method = root["method"].as_text();
    if (method == "textDocument/publishDiagnostics") {
      const RpcValue& params = root["params"];
      std::string uri = params["uri"].as_text();
      std::string path = uri_to_path(uri);

      std::vector<LspDiagnostic> diags;
      if (params.has("diagnostics") && params["diagnostics"].kind() == RpcValue::Kind::List) {
        for (const auto& d : params["diagnostics"].items()) {
          LspDiagnostic diag;
          diag.message = d["message"].as_text();
          diag.severity = static_cast<int>(d["severity"].as_u64(1));
          diag.source = d.has("source") ? d["source"].as_text() : "lsp";

          const RpcValue& r = d["range"];
          diag.range.start.line = static_cast<int>(r["start"]["line"].as_u64());
          diag.range.start.character = static_cast<int>(r["start"]["character"].as_u64());
          diag.range.end.line = static_cast<int>(r["end"]["line"].as_u64());
          diag.range.end.character = static_cast<int>(r["end"]["character"].as_u64());
          diags.push_back(diag);
        }
      }
      if (on_diagnostics_) on_diagnostics_(path, diags);
    }
    return;
  }

  // A request FROM the server (workspace/configuration, progress tokens...).
  // This client implements none of them; answer null so the server is not
  // left waiting.
  if (root.has("method") && root.has("id")) {
    RpcValue res = RpcValue::object();
    res.set("jsonrpc", RpcValue::text("2.0"));
    res.set("id", root["id"]);
    res.set("result", RpcValue());
    write_frame(res.render());
    return;
  }

  // The answer to `initialize`: say `initialized`, then release what waited.
  if (root.has("id") && root["id"].as_u64(99) == 0 && !initialized_) {
    initialized_ = true;
    RpcValue n = RpcValue::object();
    n.set("jsonrpc", RpcValue::text("2.0"));
    n.set("method", RpcValue::text("initialized"));
    n.set("params", RpcValue::object());
    write_frame(n.render());
    std::vector<std::string> held;
    held.swap(held_until_init_);
    for (const auto& m : held) write_frame(m);
    return;
  }

  // An error answer still settles its request -- with nothing -- so a caller
  // waiting on it is told rather than left hanging forever.
  if (root.has("id") && root.has("error") && !root.has("result")) {
    uint64_t id = root["id"].as_u64();
    std::function<void()> settle;
    {
      std::lock_guard<aios::hal::Mutex> lock(mutex_);
      if (auto it = pending_completions_.find(id); it != pending_completions_.end()) { auto cb = it->second; pending_completions_.erase(it); settle = [cb] { if (cb) cb({}); }; }
      else if (auto it2 = pending_definitions_.find(id); it2 != pending_definitions_.end()) { auto cb = it2->second; pending_definitions_.erase(it2); settle = [cb] { if (cb) cb({}); }; }
      else if (auto it3 = pending_hovers_.find(id); it3 != pending_hovers_.end()) { auto cb = it3->second; pending_hovers_.erase(it3); settle = [cb] { if (cb) cb(LspHoverInfo{}); }; }
      else if (auto it4 = pending_references_.find(id); it4 != pending_references_.end()) { auto cb = it4->second; pending_references_.erase(it4); settle = [cb] { if (cb) cb({}); }; }
    }
    if (settle) settle();
    return;
  }

  // Handle Response payloads (id matching pending requests)
  if (root.has("id") && root.has("result")) {
    uint64_t id = root["id"].as_u64();
    const RpcValue& res = root["result"];

    // Check completion response
    auto it_comp = pending_completions_.find(id);
    if (it_comp != pending_completions_.end()) {
      std::vector<LspCompletionItem> items;
      const RpcValue& arr = res.has("items") ? res["items"] : res;
      if (arr.kind() == RpcValue::Kind::List) {
        for (const auto& item : arr.items()) {
          LspCompletionItem ci;
          ci.label = item["label"].as_text();
          ci.detail = item.has("detail") ? item["detail"].as_text() : "";
          ci.insert_text = item.has("insertText") ? item["insertText"].as_text() : ci.label;
          ci.kind = item.has("kind") ? static_cast<int>(item["kind"].as_u64()) : 1;
          if (item.has("documentation")) {
            const RpcValue& doc = item["documentation"];
            ci.documentation = doc.kind() == RpcValue::Kind::Object ? doc["value"].as_text() : doc.as_text();
          }
          items.push_back(ci);
        }
      }
      it_comp->second(items);
      pending_completions_.erase(it_comp);
      return;
    }

    // Check definition response
    auto it_def = pending_definitions_.find(id);
    if (it_def != pending_definitions_.end()) {
      std::vector<LspLocation> locs;
      const RpcValue& arr = (res.kind() == RpcValue::Kind::List) ? res : res;
      if (arr.kind() == RpcValue::Kind::List) {
        for (const auto& item : arr.items()) {
          LspLocation loc;
          loc.uri = item["uri"].as_text();
          loc.path = uri_to_path(loc.uri);
          const RpcValue& r = item["range"];
          loc.range.start.line = static_cast<int>(r["start"]["line"].as_u64());
          loc.range.start.character = static_cast<int>(r["start"]["character"].as_u64());
          loc.range.end.line = static_cast<int>(r["end"]["line"].as_u64());
          loc.range.end.character = static_cast<int>(r["end"]["character"].as_u64());
          locs.push_back(loc);
        }
      } else if (res.kind() == RpcValue::Kind::Object) {
        LspLocation loc;
        loc.uri = res["uri"].as_text();
        loc.path = uri_to_path(loc.uri);
        const RpcValue& r = res["range"];
        loc.range.start.line = static_cast<int>(r["start"]["line"].as_u64());
        loc.range.start.character = static_cast<int>(r["start"]["character"].as_u64());
        loc.range.end.line = static_cast<int>(r["end"]["line"].as_u64());
        loc.range.end.character = static_cast<int>(r["end"]["character"].as_u64());
        locs.push_back(loc);
      }
      it_def->second(locs);
      pending_definitions_.erase(it_def);
      return;
    }

    // Check hover response
    auto it_hov = pending_hovers_.find(id);
    if (it_hov != pending_hovers_.end()) {
      LspHoverInfo hov;
      if (res.kind() == RpcValue::Kind::Object && res.has("contents")) {
        const RpcValue& c = res["contents"];
        auto one = [](const RpcValue& v) { return v.kind() == RpcValue::Kind::Object ? v["value"].as_text() : v.as_text(); };
        if (c.kind() == RpcValue::Kind::List) {
          for (const auto& part : c.items()) {
            std::string t = one(part);
            if (t.empty()) continue;
            if (!hov.contents.empty()) hov.contents += "\n";
            hov.contents += t;
          }
        } else {
          hov.contents = one(c);
        }
      }
      it_hov->second(hov);
      pending_hovers_.erase(it_hov);
      return;
    }

    // Check references response
    auto it_ref = pending_references_.find(id);
    if (it_ref != pending_references_.end()) {
      std::vector<LspLocation> locs;
      if (res.kind() == RpcValue::Kind::List) {
        for (const auto& item : res.items()) {
          LspLocation loc;
          loc.uri = item["uri"].as_text();
          loc.path = uri_to_path(loc.uri);
          const RpcValue& r = item["range"];
          loc.range.start.line = static_cast<int>(r["start"]["line"].as_u64());
          loc.range.start.character = static_cast<int>(r["start"]["character"].as_u64());
          loc.range.end.line = static_cast<int>(r["end"]["line"].as_u64());
          loc.range.end.character = static_cast<int>(r["end"]["character"].as_u64());
          locs.push_back(loc);
        }
      }
      it_ref->second(locs);
      pending_references_.erase(it_ref);
      return;
    }
  }
}

LspClientManager& LspClientManager::instance() {
  static LspClientManager mgr;
  return mgr;
}

namespace {
LspServerKind kind_for(SourceLang lang) {
  if (lang == SourceLang::C || lang == SourceLang::Cpp) return LspServerKind::Clangd;
  if (lang == SourceLang::JavaScript || lang == SourceLang::TypeScript) return LspServerKind::TsServer;
  if (lang == SourceLang::Python) return LspServerKind::PyLsp;
  if (lang == SourceLang::Go) return LspServerKind::Gopls;
  if (lang == SourceLang::Rust) return LspServerKind::RustAnalyzer;
  return LspServerKind::Generic;
}
}  // namespace

std::string LspClientManager::server_for(SourceLang lang) {
  return get_binary_for_lang(kind_for(lang));
}

std::shared_ptr<LspClient> LspClientManager::get_or_start_client(SourceLang lang, const std::string& workspace_root) {
  std::lock_guard<aios::hal::Mutex> lock(mutex_);
  // One server per language PER WORKSPACE: a clangd started for one tree knows
  // nothing about another's compile_commands.json.
  const auto key = std::make_pair(lang, workspace_root.empty() ? std::string(".") : workspace_root);
  auto it = clients_.find(key);
  if (it != clients_.end() && it->second && it->second->is_running()) return it->second;

  const LspServerKind kind = kind_for(lang);
  if (kind == LspServerKind::Generic) return nullptr;
  auto client = std::make_shared<LspClient>(kind, "", key.second);
  if (!client->start()) return nullptr;   // no server on PATH: no client at all
  clients_[key] = client;
  return client;
}

void LspClientManager::shutdown_all() {
  std::lock_guard<aios::hal::Mutex> lock(mutex_);
  for (auto& kv : clients_) {
    if (kv.second) kv.second->stop();
  }
  clients_.clear();
}

} // namespace aios::userland

#endif  // AIOS_METAL
