// Plugins in C or C++: load_plugin("libmine.so") opens a shared library that
// exports grain_plugin_init (include/grain_plugin.h) and lets it register
// natives. A plugin is handed a table of functions rather than linking against
// grain, so one built with any compiler loads into any grain with the same ABI.
#include "plugin.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "grain_plugin.h"

namespace grain_cli {

namespace g = aios::userland::grain;

// What the plugin's `grain_call*` is: the arguments, and where the answer goes.
struct PluginCall {
  g::Vm* vm = nullptr;
  std::vector<g::Value>* args = nullptr;
  g::Value ret;
  std::string error;
};

struct PluginRegistry {
  g::Vm* vm = nullptr;
  std::string library;
  std::vector<std::string> names;
};

namespace {

PluginCall* C(grain_call* c) { return reinterpret_cast<PluginCall*>(c); }

const g::Value* arg(grain_call* c, int i) {
  auto& a = *C(c)->args;
  return i >= 0 && (size_t)i < a.size() ? &a[(size_t)i] : nullptr;
}

int type_of(const g::Value* v) {
  if (!v) return GRAIN_NIL;
  switch (v->type) {
    case g::Type::Nil: return GRAIN_NIL;
    case g::Type::Bool: return GRAIN_BOOL;
    case g::Type::Int: return GRAIN_INT;
    case g::Type::Real: return GRAIN_REAL;
    case g::Type::Str: return GRAIN_STR;
    case g::Type::Bytes: return GRAIN_BYTES;
    case g::Type::List: return GRAIN_LIST;
    case g::Type::Map: return GRAIN_MAP;
    default: return GRAIN_OTHER;
  }
}

int api_argc(grain_call* c) { return (int)C(c)->args->size(); }
int api_arg_type(grain_call* c, int i) { return type_of(arg(c, i)); }
int64_t api_arg_int(grain_call* c, int i) { const g::Value* v = arg(c, i); return v ? (v->type == g::Type::Bool ? (int64_t)v->b : v->as_int()) : 0; }
double api_arg_real(grain_call* c, int i) { const g::Value* v = arg(c, i); return v ? v->as_double() : 0.0; }
const char* api_arg_str(grain_call* c, int i, size_t* len) {
  const g::Value* v = arg(c, i);
  if (!v || v->type != g::Type::Str) { if (len) *len = 0; return nullptr; }
  if (len) *len = v->str().size();
  return v->str().c_str();
}
uint8_t* api_arg_bytes(grain_call* c, int i, size_t* len) {
  const g::Value* v = arg(c, i);
  if (!v || v->type != g::Type::Bytes) { if (len) *len = 0; return nullptr; }
  auto& d = v->bytes_data();
  if (len) *len = d.size();
  return d.data();
}
size_t api_arg_list_len(grain_call* c, int i) { const g::Value* v = arg(c, i); return v && v->type == g::Type::List ? v->list_items().size() : 0; }
const g::Value* list_item(grain_call* c, int i, size_t k) {
  const g::Value* v = arg(c, i);
  if (!v || v->type != g::Type::List || k >= v->list_items().size()) return nullptr;
  return &v->list_items()[k];
}
int api_arg_list_type(grain_call* c, int i, size_t k) { return type_of(list_item(c, i, k)); }
int64_t api_arg_list_int(grain_call* c, int i, size_t k) { const g::Value* v = list_item(c, i, k); return v ? v->as_int() : 0; }
double api_arg_list_real(grain_call* c, int i, size_t k) { const g::Value* v = list_item(c, i, k); return v ? v->as_double() : 0.0; }

void api_return_nil(grain_call* c) { C(c)->ret = g::Value::nil(); }
void api_return_bool(grain_call* c, int v) { C(c)->ret = g::Value::boolean(v != 0); }
void api_return_int(grain_call* c, int64_t v) { C(c)->ret = g::Value::integer(v); }
void api_return_real(grain_call* c, double v) { C(c)->ret = g::Value::real(v); }
void api_return_str(grain_call* c, const char* s, size_t len) { C(c)->ret = g::Value::string(s ? std::string(s, len) : std::string()); }
void api_return_bytes(grain_call* c, const uint8_t* p, size_t len) { C(c)->ret = g::Value::bytes(p ? std::vector<uint8_t>(p, p + len) : std::vector<uint8_t>()); }
void api_error(grain_call* c, const char* message) { C(c)->error = message && *message ? message : "plugin error"; }

// What a registered name is when grain calls it: a bound method whose receiver holds the plugin's function.
g::Value method_plugin_call(g::Vm& vm, g::BoundMethodObj& bm, std::vector<g::Value>& args, std::string& error) {
  auto* rec = static_cast<g::MapObj*>(bm.receiver.obj.get());
  auto fn = reinterpret_cast<grain_native_fn>((uintptr_t)rec->find("fn")->i);
  PluginCall call;
  call.vm = &vm;
  call.args = &args;
  fn(reinterpret_cast<grain_call*>(&call));
  if (!call.error.empty()) error = bm.name + "(): " + call.error;
  return std::move(call.ret);
}

int api_register(grain_registry* r, const char* name, int min_args, int max_args, grain_native_fn fn) {
  auto* reg = reinterpret_cast<PluginRegistry*>(r);
  if (!name || !*name || !fn) return 1;
  auto rec = std::make_shared<g::MapObj>();
  rec->put("fn", g::Value::integer((int64_t)(uintptr_t)fn));
  reg->vm->define_global(name, g::Value::object(g::Type::BoundMethod, std::make_shared<g::BoundMethodObj>(
      g::Value::object(g::Type::Map, rec), name, min_args, max_args, method_plugin_call)));
  reg->names.push_back(name);
  return 0;
}

const grain_api kApi = {
    GRAIN_PLUGIN_ABI, (uint32_t)sizeof(grain_api),
    api_argc, api_arg_type, api_arg_int, api_arg_real, api_arg_str, api_arg_bytes,
    api_arg_list_len, api_arg_list_type, api_arg_list_int, api_arg_list_real,
    api_return_nil, api_return_bool, api_return_int, api_return_real, api_return_str, api_return_bytes,
    api_error, api_register};

using InitFn = int (*)(const grain_api*, grain_registry*);

// load_plugin(path) -> the list of names the plugin registered
g::Value bi_load_plugin(g::Vm& vm, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Str) { error = "load_plugin() wants a path"; return g::Value::nil(); }
  const std::string path = args[0].str();
  InitFn init = nullptr;
#if defined(_WIN32)
  HMODULE h = LoadLibraryA(path.c_str());
  if (!h) { error = "load_plugin(\"" + path + "\"): cannot open the library (error " + std::to_string(GetLastError()) + ")"; return g::Value::nil(); }
  init = reinterpret_cast<InitFn>(GetProcAddress(h, "grain_plugin_init"));
#else
  void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) { const char* why = ::dlerror(); error = "load_plugin(\"" + path + "\"): " + (why ? why : "cannot open the library"); return g::Value::nil(); }
  init = reinterpret_cast<InitFn>(::dlsym(h, "grain_plugin_init"));
#endif
  if (!init) { error = "load_plugin(\"" + path + "\"): the library has no grain_plugin_init (see include/grain_plugin.h)"; return g::Value::nil(); }
  PluginRegistry reg;
  reg.vm = &vm;
  reg.library = path;
  const int rc = init(&kApi, reinterpret_cast<grain_registry*>(&reg));
  if (rc != 0) { error = "load_plugin(\"" + path + "\"): the plugin refused to load (returned " + std::to_string(rc) + ")"; return g::Value::nil(); }
  std::vector<g::Value> names;
  for (const auto& n : reg.names) names.push_back(g::Value::string(n));
  return g::Value::list(std::move(names));   // the library stays loaded: its functions are now globals
}

}  // namespace

void install_plugins(g::Vm& vm) { vm.define_native("load_plugin", 1, 1, bi_load_plugin); }

}  // namespace grain_cli
