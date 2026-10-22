// grain lsp: a Language Server Protocol server on standard input and output,
// for any editor with an LSP client (VS Code, Neovim, Helix, Zed, Emacs).
//
//   diagnostics      every compile error, as the document is opened and edited
//   formatting       grain fmt's layout, only when it compiles to the same program
//   document symbols the file's top-level functions and lets
//   completion       the standard library's names and the file's own top-level ones
//
// Messages are JSON-RPC 2.0 framed with Content-Length headers. Documents are
// kept whole (full sync).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "aios/core/json.hpp"
#include "tools.hpp"

#ifndef GRAIN_VERSION
#define GRAIN_VERSION "0.0.0-dev"
#endif

namespace grain_cli {

namespace g = aios::userland::grain;
namespace json = aios::json;

namespace {

bool read_message(std::string& body) {
  size_t length = 0;
  bool have_length = false;
  std::string line;
  for (;;) {
    line.clear();
    int c;
    while ((c = std::getchar()) != EOF && c != '\n') line.push_back(static_cast<char>(c));
    if (c == EOF) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;   // end of the headers
    const std::string key = "Content-Length:";
    if (line.compare(0, key.size(), key) == 0) {
      length = static_cast<size_t>(std::strtoull(line.c_str() + key.size(), nullptr, 10));
      have_length = true;
    }
  }
  if (!have_length) return false;
  body.resize(length);
  return std::fread(body.data(), 1, length, stdin) == length;
}

void send(const json::Value& msg) {
  const std::string body = msg.dump();
  std::printf("Content-Length: %zu\r\n\r\n", body.size());
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fflush(stdout);
}

void respond(const json::Value& id, json::Value result) {
  json::Value m = json::Value::object();
  m.set("jsonrpc", json::Value::string("2.0"));
  m.set("id", id);
  m.set("result", std::move(result));
  send(m);
}

void respond_error(const json::Value& id, int code, const std::string& message) {
  json::Value e = json::Value::object();
  e.set("code", json::Value::integer(code));
  e.set("message", json::Value::string(message));
  json::Value m = json::Value::object();
  m.set("jsonrpc", json::Value::string("2.0"));
  m.set("id", id);
  m.set("error", std::move(e));
  send(m);
}

void notify(const std::string& method, json::Value params) {
  json::Value m = json::Value::object();
  m.set("jsonrpc", json::Value::string("2.0"));
  m.set("method", json::Value::string(method));
  m.set("params", std::move(params));
  send(m);
}

json::Value position(int64_t line, int64_t character) {
  json::Value p = json::Value::object();
  p.set("line", json::Value::integer(line));
  p.set("character", json::Value::integer(character));
  return p;
}

json::Value line_range(int64_t line) {
  json::Value r = json::Value::object();
  r.set("start", position(line, 0));
  r.set("end", position(line, 100000));   // past the end of a line means its end (LSP 3.17)
  return r;
}

void publish(const std::string& uri, const std::string& text) {
  json::Value diags = json::Value::array();
  const auto compiled = g::compile_source(text, uri);
  for (const auto& d : compiled.errors) {
    json::Value diag = json::Value::object();
    diag.set("range", line_range(d.line > 0 ? d.line - 1 : 0));
    diag.set("severity", json::Value::integer(1));
    diag.set("source", json::Value::string("grain"));
    diag.set("message", json::Value::string(d.message));
    diags.push(std::move(diag));
  }
  json::Value params = json::Value::object();
  params.set("uri", json::Value::string(uri));
  params.set("diagnostics", std::move(diags));
  notify("textDocument/publishDiagnostics", std::move(params));
}

// Top-level `fn name(` and `let name` lines, by bracket depth (the same
// counting grain fmt does).
struct Symbol { std::string name; bool function; int64_t line; };
std::vector<Symbol> top_level_symbols(const std::string& text) {
  std::vector<Symbol> out;
  std::istringstream in(text);
  int depth = 0;
  int64_t n = 0;
  for (std::string line; std::getline(in, line); ++n) {
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && depth == 0) {
      for (const char* kw : {"fn ", "let "}) {
        const size_t len = std::char_traits<char>::length(kw);
        if (line.compare(first, len, kw) != 0) continue;
        size_t i = first + len, j = i;
        while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
        if (j > i) out.push_back({line.substr(i, j - i), kw[0] == 'f', n});
      }
    }
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; continue; }
      if (c == '"' || c == '\'') { quote = c; continue; }
      if (c == '#') break;
      if (c == '{' || c == '[' || c == '(') ++depth;
      else if (c == '}' || c == ']' || c == ')') depth = depth > 0 ? depth - 1 : 0;
    }
  }
  return out;
}

const std::vector<std::string>& library_names() {
  static const std::vector<std::string> names = [] {
    g::Vm vm;
    prepare_vm(vm, {});
    return vm.global_names();
  }();
  return names;
}

}  // namespace

int lsp_command(const std::vector<std::string>&) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);   // Content-Length counts bytes; text mode would turn \r\n into \n
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::map<std::string, std::string> docs;
  bool shutdown = false;
  std::string body;
  while (read_message(body)) {
    auto parsed = json::parse(body);
    if (!parsed.ok()) continue;
    const json::Value& msg = *parsed;
    const std::string method = msg["method"].str();
    const bool is_request = msg.has("id");
    const json::Value& id = msg["id"];
    const json::Value& params = msg["params"];

    if (method == "initialize") {
      json::Value caps = json::Value::object();
      json::Value sync = json::Value::object();
      sync.set("openClose", json::Value::boolean(true));
      sync.set("change", json::Value::integer(1));   // full documents
      sync.set("save", json::Value::boolean(true));
      caps.set("textDocumentSync", std::move(sync));
      caps.set("documentFormattingProvider", json::Value::boolean(true));
      caps.set("documentSymbolProvider", json::Value::boolean(true));
      caps.set("completionProvider", json::Value::object());
      json::Value info = json::Value::object();
      info.set("name", json::Value::string("grain"));
      info.set("version", json::Value::string(GRAIN_VERSION));
      json::Value result = json::Value::object();
      result.set("capabilities", std::move(caps));
      result.set("serverInfo", std::move(info));
      respond(id, std::move(result));
    } else if (method == "textDocument/didOpen") {
      const std::string uri = params["textDocument"]["uri"].str();
      docs[uri] = params["textDocument"]["text"].str();
      publish(uri, docs[uri]);
    } else if (method == "textDocument/didChange") {
      const std::string uri = params["textDocument"]["uri"].str();
      const json::Value& changes = params["contentChanges"];
      if (changes.size() > 0) docs[uri] = changes[changes.size() - 1]["text"].str();
      publish(uri, docs[uri]);
    } else if (method == "textDocument/didSave") {
      const std::string uri = params["textDocument"]["uri"].str();
      if (params.has("text")) docs[uri] = params["text"].str();
      publish(uri, docs[uri]);
    } else if (method == "textDocument/didClose") {
      const std::string uri = params["textDocument"]["uri"].str();
      docs.erase(uri);
      json::Value p = json::Value::object();
      p.set("uri", json::Value::string(uri));
      p.set("diagnostics", json::Value::array());
      notify("textDocument/publishDiagnostics", std::move(p));
    } else if (method == "textDocument/formatting") {
      const std::string uri = params["textDocument"]["uri"].str();
      json::Value edits = json::Value::array();
      auto it = docs.find(uri);
      if (it != docs.end()) {
        const std::string formatted = format_source(it->second);
        const auto before = g::compile_source(it->second, uri);
        const auto after = g::compile_source(formatted, uri);
        // Only a document that compiles, and still compiles after: formatting never adds an error.
        if (formatted != it->second && before.ok() && after.ok()) {
          int64_t lines = 0;
          for (char c : it->second) lines += c == '\n';
          json::Value range = json::Value::object();
          range.set("start", position(0, 0));
          range.set("end", position(lines + 1, 0));
          json::Value edit = json::Value::object();
          edit.set("range", std::move(range));
          edit.set("newText", json::Value::string(formatted));
          edits.push(std::move(edit));
        }
      }
      respond(id, std::move(edits));
    } else if (method == "textDocument/documentSymbol") {
      const std::string uri = params["textDocument"]["uri"].str();
      json::Value syms = json::Value::array();
      auto it = docs.find(uri);
      if (it != docs.end()) {
        for (const auto& s : top_level_symbols(it->second)) {
          json::Value sym = json::Value::object();
          sym.set("name", json::Value::string(s.name));
          sym.set("kind", json::Value::integer(s.function ? 12 : 13));   // Function, Variable
          sym.set("range", line_range(s.line));
          sym.set("selectionRange", line_range(s.line));
          syms.push(std::move(sym));
        }
      }
      respond(id, std::move(syms));
    } else if (method == "textDocument/completion") {
      json::Value items = json::Value::array();
      for (const auto& name : library_names()) {
        json::Value item = json::Value::object();
        item.set("label", json::Value::string(name));
        item.set("kind", json::Value::integer(3));   // Function
        items.push(std::move(item));
      }
      auto it = docs.find(params["textDocument"]["uri"].str());
      if (it != docs.end()) {
        for (const auto& s : top_level_symbols(it->second)) {
          json::Value item = json::Value::object();
          item.set("label", json::Value::string(s.name));
          item.set("kind", json::Value::integer(s.function ? 3 : 6));   // Function, Variable
          items.push(std::move(item));
        }
      }
      respond(id, std::move(items));
    } else if (method == "shutdown") {
      shutdown = true;
      respond(id, json::Value::null());
    } else if (method == "exit") {
      return shutdown ? 0 : 1;
    } else if (is_request) {
      respond_error(id, -32601, "grain lsp does not handle " + method);
    }
  }
  return shutdown ? 0 : 1;
}

}  // namespace grain_cli
