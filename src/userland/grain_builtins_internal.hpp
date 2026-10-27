#pragma once
// The argument checks every grain native starts with, shared by the portable
// standard library (grain_builtins.cpp) and the natives that only exist inside
// aios (grain_aios.cpp). A failed check fills `error` with what was wanted and
// what came; the native then returns nil and the VM raises it at the call.
#include <memory>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

[[maybe_unused]] inline Value arg_or(std::vector<Value>& a, size_t i) { return i < a.size() ? a[i] : Value::nil(); }

inline bool want_list(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::List) return true;
  error = std::string(who) + "() wants a list, got a " + type_name(v.type);
  return false;
}

inline bool want_str(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Str) return true;
  error = std::string(who) + "() wants a string, got a " + type_name(v.type);
  return false;
}

inline bool want_number(const Value& v, std::string& error, const char* who) {
  if (v.is_number()) return true;
  error = std::string(who) + "() wants a number, got a " + type_name(v.type);
  return false;
}

// A number or an exact decimal: for the functions that take either (abs, min, max, round, ...).
inline bool want_numeric(const Value& v, std::string& error, const char* who) {
  if (v.is_number() || v.type == Type::Decimal) return true;
  error = std::string(who) + "() wants a number, got a " + type_name(v.type);
  return false;
}

// A pane drawn and driven by native code instead of grain callbacks (a face
// pane, grain_face.cpp). It still belongs to the VM that made it, and the
// closures it calls are that VM's.
class PaneDriver {
 public:
  virtual ~PaneDriver() = default;
  // Draw the whole pane into `argb` (width x height, row-major ARGB).
  virtual bool draw(Vm& vm, std::vector<uint32_t>& argb, int width, int height, std::string& error) = 0;
  // An input event map, as a grain pane's on_event receives it.
  virtual bool event(Vm& vm, const Value& event, std::string& error) = 0;
};
// Registers a driven pane for `vm` and returns its pane map (as create_pane does).
Value register_driven_pane(Vm& vm, const std::string& title, int width, int height, std::shared_ptr<PaneDriver> driver);
// The driver of one of `vm`'s panes (a pane map, handle or id), or null.
std::shared_ptr<PaneDriver> pane_driver(const Vm& vm, const Value& pane);

// Where TLS certificates are trusted from (set_http_trust_root), for the http and mail builtins.
std::string http_trust_root();
// process_run (grain_std_process.cpp); installed with the core builtins, refusing unless Vm::allow_process.
void install_process_builtins(Vm& vm);
// doc_* (pdf_render, zip_create, unzip, xlsx_create - grain_std_doc.cpp)
void install_doc_builtins(Vm& vm);
// kv_* (grain_std_kv.cpp); installed with the core builtins.
void install_kv_builtins(Vm& vm);
// mail_send (grain_std_mail.cpp); installed by install_http_builtins.
void install_mail_builtins(Vm& vm);

// The JSON json_parse / json_stringify use (grain_builtins.cpp).
Value grain_json_parse(const std::string& text, std::string& err);
std::string grain_json_stringify(const Value& v);

// Safe RFC 7578 Multipart Form Data parsing
struct MultipartResult {
  std::shared_ptr<MapObj> form = std::make_shared<MapObj>();
  std::shared_ptr<MapObj> files = std::make_shared<MapObj>();
};
std::string extract_multipart_boundary(const std::string& content_type);
std::string sanitize_upload_filename(const std::string& raw);
MultipartResult parse_multipart_data(const std::string& body, const std::string& boundary);

}  // namespace aios::userland::grain
