#pragma once
#include <string>
#include "aios/hal/sync.hpp"
#include <vector>
#include <map>
#include <utility>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include "aios/core/result.hpp"
#include "aios/userland/source_lang.hpp"

namespace aios::userland {

struct LspPosition {
  int line = 0;       // 0-based
  int character = 0;  // 0-based character/column offset
};

struct LspRange {
  LspPosition start;
  LspPosition end;
};

struct LspLocation {
  std::string uri;
  std::string path;
  LspRange range;
  std::string line_text;
};

struct LspDiagnostic {
  LspRange range;
  int severity = 1; // 1: Error, 2: Warning, 3: Information, 4: Hint
  std::string code;
  std::string source;
  std::string message;
};

struct LspCompletionItem {
  std::string label;
  int kind = 0; // 1: Text, 2: Method, 3: Function, 6: Variable, 7: Class, etc.
  std::string detail;
  std::string documentation;
  std::string insert_text;
};

struct LspHoverInfo {
  std::string contents;
  LspRange range;
};

enum class LspServerKind {
  Clangd,        // C / C++
  TsServer,      // JS / TS
  PyLsp,         // Python
  Gopls,         // Go
  RustAnalyzer,  // Rust
  Generic
};

class LspClient {
 public:
  LspClient(LspServerKind kind, const std::string& binary_cmd, const std::string& workspace_root);
  ~LspClient();

  bool start();
  void stop();
  bool is_running() const { return running_; }

  // Document sync notifications
  void did_open(const std::string& path, SourceLang lang, const std::string& content);
  void did_change(const std::string& path, const std::string& content, int version = 1);
  void did_save(const std::string& path);
  void did_close(const std::string& path);

  // Async requests (returns request ID)
  uint64_t request_completion(const std::string& path, int line, int col,
                             std::function<void(const std::vector<LspCompletionItem>&)> cb);
  uint64_t request_definition(const std::string& path, int line, int col,
                             std::function<void(const std::vector<LspLocation>&)> cb);
  uint64_t request_hover(const std::string& path, int line, int col,
                         std::function<void(const LspHoverInfo&)> cb);
  uint64_t request_references(const std::string& path, int line, int col,
                            std::function<void(const std::vector<LspLocation>&)> cb);

  void set_diagnostic_callback(std::function<void(const std::string& path, const std::vector<LspDiagnostic>&)> cb) {
    on_diagnostics_ = cb;
  }

  // Poll for incoming responses (safe for GUI frame loop)
  void poll_responses();

 private:
  void send_json_rpc(const std::string& json_str);
  void write_frame(const std::string& json_str);
  void handle_message(const std::string& msg);
  void reader_loop();

  LspServerKind kind_;
  std::string binary_cmd_;
  std::string workspace_root_;
  std::atomic<bool> running_{false};
  int pid_ = -1;
  int fd_in_ = -1;  // write pipe to process stdin
  int fd_out_ = -1; // read pipe from process stdout

  aios::hal::Thread reader_thread_;
  std::atomic<uint64_t> next_id_{1};
  aios::hal::Mutex mutex_;

  struct ResponseQueueItem {
    std::string message;
  };
  std::vector<ResponseQueueItem> response_queue_;

  std::map<uint64_t, std::function<void(const std::vector<LspCompletionItem>&)>> pending_completions_;
  std::map<uint64_t, std::function<void(const std::vector<LspLocation>&)>> pending_definitions_;
  std::map<uint64_t, std::function<void(const LspHoverInfo&)>> pending_hovers_;
  std::map<uint64_t, std::function<void(const std::vector<LspLocation>&)>> pending_references_;

  std::function<void(const std::string& path, const std::vector<LspDiagnostic>&)> on_diagnostics_;
  std::map<std::string, std::string> open_docs_; // path -> content cache
  // Until the server answers `initialize`, everything else is held here.
  bool initialized_ = false;
  std::vector<std::string> held_until_init_;
};

class LspClientManager {
 public:
  static LspClientManager& instance();

  // nullptr when no server for `lang` is on PATH -- there is no stand-in.
  std::shared_ptr<LspClient> get_or_start_client(SourceLang lang, const std::string& workspace_root);
  // The program that serves `lang` ("clangd", "pylsp", ...), "" for none.
  static std::string server_for(SourceLang lang);
  void shutdown_all();

 private:
  LspClientManager() = default;
  aios::hal::Mutex mutex_;
  std::map<std::pair<SourceLang, std::string>, std::shared_ptr<LspClient>> clients_;
};

} // namespace aios::userland
