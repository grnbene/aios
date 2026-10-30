// A foreign function interface with no library under it: the calling
// convention is done by hand.
//
// On x86-64 System V and on AArch64 the integer arguments and the floating
// point arguments go in two separate register files, each filled in argument
// order, and what does not fit goes on the stack in argument order. So one
// prototype that names every register - six (x86-64) or eight (AArch64)
// integers, eight doubles, then eight stack slots - sets up a call to any
// function that takes up to that many of each; the callee reads the registers
// its own signature names and ignores the rest. Return values come back in
// rax/x0 or xmm0/v0, so there are two shims, one per kind.
//
//   let libc = ffi_open()                           the running program (and so libc)
//   let strlen = ffi_fn(libc, "strlen", "u64(cstr)")
//   strlen("grain")                                 -> 5
//   let snprintf = ffi_fn(libc, "snprintf", "i32(bytes, u64, cstr, ...)")
//
// Types: void i8 u8 i16 u16 i32 u32 i64 u64 f32 f64, ptr (a whole number, or
// bytes to pass their address), cstr (a string in, a string out), bytes (the
// address of a buffer the C code may fill). `...` ends a variadic signature;
// the extra arguments are typed by their value.
//
// A wrong signature is a wrong call: it crashes the process or corrupts it,
// exactly as it would in C. Not supported: structs passed by value, callbacks
// into grain, long double, Windows (so far).
#include "ffi.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace grain_cli {

namespace g = aios::userland::grain;

namespace {

enum class Ty { Void, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Ptr, CStr, Bytes };

struct Sig {
  Ty ret = Ty::Void;
  std::vector<Ty> args;
  bool variadic = false;
};

bool parse_ty(std::string s, Ty& out) {
  size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
  s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
  static const std::pair<const char*, Ty> names[] = {
      {"void", Ty::Void}, {"i8", Ty::I8}, {"u8", Ty::U8}, {"i16", Ty::I16}, {"u16", Ty::U16}, {"i32", Ty::I32},
      {"u32", Ty::U32}, {"i64", Ty::I64}, {"u64", Ty::U64}, {"f32", Ty::F32}, {"f64", Ty::F64}, {"ptr", Ty::Ptr},
      {"cstr", Ty::CStr}, {"bytes", Ty::Bytes},
      {"int", Ty::I32}, {"uint", Ty::U32}, {"long", Ty::I64}, {"ulong", Ty::U64}, {"size", Ty::U64},
      {"float", Ty::F32}, {"double", Ty::F64}, {"char", Ty::I8}, {"bool", Ty::U8}};
  for (const auto& [n, t] : names)
    if (s == n) { out = t; return true; }
  return false;
}

// "i32(cstr, i64, ...)"
bool parse_sig(const std::string& text, Sig& sig, std::string& error, const char* who) {
  const size_t open = text.find('('), close = text.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    error = std::string(who) + "(): a signature looks like \"i32(cstr, i64)\", got \"" + text + "\"";
    return false;
  }
  if (!parse_ty(text.substr(0, open), sig.ret)) { error = std::string(who) + "(): unknown return type \"" + text.substr(0, open) + "\""; return false; }
  const std::string inner = text.substr(open + 1, close - open - 1);
  size_t at = 0;
  while (at <= inner.size() && !inner.empty()) {
    size_t comma = inner.find(',', at);
    if (comma == std::string::npos) comma = inner.size();
    std::string item = inner.substr(at, comma - at);
    at = comma + 1;
    size_t a = item.find_first_not_of(' '), b = item.find_last_not_of(' ');
    item = a == std::string::npos ? "" : item.substr(a, b - a + 1);
    if (item == "...") { sig.variadic = true; break; }
    if (item == "void" && inner.find(',') == std::string::npos) break;
    Ty t;
    if (!parse_ty(item, t) || t == Ty::Void) { error = std::string(who) + "(): unknown argument type \"" + item + "\""; return false; }
    sig.args.push_back(t);
    if (comma == inner.size()) break;
  }
  return true;
}

#if defined(_WIN32)
constexpr bool kSupported = false;
#elif defined(__x86_64__) || defined(__aarch64__)
constexpr bool kSupported = true;
#else
constexpr bool kSupported = false;
#endif

#if defined(__x86_64__)
constexpr int kIntRegs = 6;
#else
constexpr int kIntRegs = 8;
#endif
constexpr int kFpRegs = 8;
constexpr int kStackSlots = 8;

struct Frame {
  uint64_t i[kIntRegs] = {0};
  double d[kFpRegs] = {0};
  uint64_t s[kStackSlots] = {0};
  int ni = 0, nd = 0, ns = 0;
};

bool push_int(Frame& f, uint64_t v, std::string& error) {
  if (f.ni < kIntRegs) { f.i[f.ni++] = v; return true; }
  if (f.ns < kStackSlots) { f.s[f.ns++] = v; return true; }
  error = "ffi: too many arguments (at most " + std::to_string(kIntRegs + kStackSlots) + " integer-class ones)";
  return false;
}

bool push_fp(Frame& f, uint64_t bits, std::string& error) {
  if (f.nd < kFpRegs) { std::memcpy(&f.d[f.nd++], &bits, 8); return true; }
  if (f.ns < kStackSlots) { f.s[f.ns++] = bits; return true; }
  error = "ffi: too many floating-point arguments";
  return false;
}

bool fits(int64_t v, Ty t) {
  switch (t) {
    case Ty::I8: return v >= -128 && v <= 127;
    case Ty::U8: return v >= 0 && v <= 255;
    case Ty::I16: return v >= -32768 && v <= 32767;
    case Ty::U16: return v >= 0 && v <= 65535;
    case Ty::I32: return v >= INT32_MIN && v <= INT32_MAX;
    case Ty::U32: return v >= 0 && v <= (int64_t)UINT32_MAX;
    default: return true;
  }
}

// One grain value as the argument its type says. Strings are copied to NUL-terminated
// temporaries that live until the call returns.
bool marshal(Frame& f, Ty t, const g::Value& v, std::deque<std::string>& temps, std::string& error, size_t index, const char* name) {
  auto bad = [&](const char* want) {
    error = std::string(name) + "(): argument " + std::to_string(index + 1) + " is a " + g::type_name(v.type) + ", wanted " + want;
    return false;
  };
  switch (t) {
    case Ty::F32: {
      if (!v.is_number() && v.type != g::Type::Decimal) return bad("a number");   // a decimal: its nearest real
      const float x = (float)v.as_double();
      uint32_t u; std::memcpy(&u, &x, 4);
      return push_fp(f, u, error);
    }
    case Ty::F64: {
      if (!v.is_number() && v.type != g::Type::Decimal) return bad("a number");   // a decimal: its nearest real
      const double x = v.as_double();
      uint64_t u; std::memcpy(&u, &x, 8);
      return push_fp(f, u, error);
    }
    case Ty::Ptr: {
      if (v.type == g::Type::Nil) return push_int(f, 0, error);
      if (v.type == g::Type::Bytes) return push_int(f, (uint64_t)(uintptr_t)v.bytes_data().data(), error);
      if (v.type != g::Type::Int) return bad("a whole number, nil or bytes");
      return push_int(f, (uint64_t)v.i, error);
    }
    case Ty::CStr: {
      if (v.type == g::Type::Nil) return push_int(f, 0, error);
      if (v.type == g::Type::Bytes) return push_int(f, (uint64_t)(uintptr_t)v.bytes_data().data(), error);
      if (v.type != g::Type::Str) return bad("a string, nil or bytes");
      temps.push_back(v.str());
      return push_int(f, (uint64_t)(uintptr_t)temps.back().c_str(), error);
    }
    case Ty::Bytes: {
      if (v.type != g::Type::Bytes) return bad("bytes");
      return push_int(f, (uint64_t)(uintptr_t)v.bytes_data().data(), error);
    }
    case Ty::Void: return bad("nothing");
    default: {
      int64_t n;
      if (v.type == g::Type::Int) n = v.i;
      else if (v.type == g::Type::Bool) n = v.b ? 1 : 0;
      else return bad("a whole number");
      if (!fits(n, t)) { error = std::string(name) + "(): argument " + std::to_string(index + 1) + " (" + std::to_string(n) + ") does not fit its type"; return false; }
      // A narrow value in a wider register: sign- or zero-extend as the type says.
      return push_int(f, (uint64_t)n, error);
    }
  }
}

#if defined(__x86_64__)
using IntShim = uint64_t (*)(uint64_t, ...);
using FpShim = double (*)(uint64_t, ...);
// Variadic prototypes so the caller sets %al (the vector registers in use), which a
// variadic callee such as printf reads.
#define GRAIN_ARGS(f) (f).i[0], (f).i[1], (f).i[2], (f).i[3], (f).i[4], (f).i[5], \
  (f).d[0], (f).d[1], (f).d[2], (f).d[3], (f).d[4], (f).d[5], (f).d[6], (f).d[7], \
  (f).s[0], (f).s[1], (f).s[2], (f).s[3], (f).s[4], (f).s[5], (f).s[6], (f).s[7]
#elif defined(__aarch64__)
using IntShim = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             double, double, double, double, double, double, double, double,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using FpShim = double (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          double, double, double, double, double, double, double, double,
                          uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define GRAIN_ARGS(f) (f).i[0], (f).i[1], (f).i[2], (f).i[3], (f).i[4], (f).i[5], (f).i[6], (f).i[7], \
  (f).d[0], (f).d[1], (f).d[2], (f).d[3], (f).d[4], (f).d[5], (f).d[6], (f).d[7], \
  (f).s[0], (f).s[1], (f).s[2], (f).s[3], (f).s[4], (f).s[5], (f).s[6], (f).s[7]
#endif

g::Value from_int(Ty t, uint64_t raw) {
  switch (t) {
    case Ty::I8: return g::Value::integer((int8_t)raw);
    case Ty::U8: return g::Value::integer((uint8_t)raw);
    case Ty::I16: return g::Value::integer((int16_t)raw);
    case Ty::U16: return g::Value::integer((uint16_t)raw);
    case Ty::I32: return g::Value::integer((int32_t)raw);
    case Ty::U32: return g::Value::integer((uint32_t)raw);
    case Ty::CStr: {
      if (!raw) return g::Value::nil();
      return g::Value::string(std::string(reinterpret_cast<const char*>((uintptr_t)raw)));
    }
    default: return g::Value::integer((int64_t)raw);
  }
}

// The callable ffi_fn returns: a bound method whose receiver holds {ptr, sig, name}.
g::Value method_call(g::Vm&, g::BoundMethodObj& bm, std::vector<g::Value>& args, std::string& error) {
  auto* rec = static_cast<g::MapObj*>(bm.receiver.obj.get());
  void* fn = reinterpret_cast<void*>((uintptr_t)rec->find("ptr")->i);
  Sig sig;
  const std::string& name = rec->find("name")->str();
  if (!parse_sig(rec->find("sig")->str(), sig, error, name.c_str())) return g::Value::nil();
  if (args.size() < sig.args.size() || (!sig.variadic && args.size() > sig.args.size())) {
    error = name + "() takes " + std::to_string(sig.args.size()) + (sig.variadic ? " or more" : "") + " argument(s), got " + std::to_string(args.size());
    return g::Value::nil();
  }
  Frame f;
  std::deque<std::string> temps;
  for (size_t k = 0; k < args.size(); ++k) {
    Ty t;
    if (k < sig.args.size()) t = sig.args[k];
    else {   // a variadic argument is typed by its value
      const g::Value& v = args[k];
      t = v.type == g::Type::Real || v.type == g::Type::Decimal ? Ty::F64 : v.type == g::Type::Str ? Ty::CStr : v.type == g::Type::Bytes || v.type == g::Type::Nil ? Ty::Ptr : Ty::I64;
    }
    if (!marshal(f, t, args[k], temps, error, k, name.c_str())) return g::Value::nil();
  }
#if defined(__APPLE__) && defined(__aarch64__)
  if (sig.variadic || f.ns > 0) { error = name + "(): variadic calls and stack arguments are not supported on Apple Silicon"; return g::Value::nil(); }
#endif
#if defined(__x86_64__) || defined(__aarch64__)
  if (sig.ret == Ty::F64 || sig.ret == Ty::F32) {
    const double r = reinterpret_cast<FpShim>(fn)(GRAIN_ARGS(f));
    if (sig.ret == Ty::F64) return g::Value::real(r);
    uint64_t bits; std::memcpy(&bits, &r, 8);
    const uint32_t lo = (uint32_t)bits;
    float x; std::memcpy(&x, &lo, 4);
    return g::Value::real((double)x);
  }
  const uint64_t r = reinterpret_cast<IntShim>(fn)(GRAIN_ARGS(f));
  if (sig.ret == Ty::Void) return g::Value::nil();
  return from_int(sig.ret, r);
#else
  return g::Value::nil();
#endif
}

bool unsupported(std::string& error, const char* who) {
  if (kSupported) return false;
  error = std::string(who) + "(): the FFI is not available on this platform yet (x86-64 and AArch64 Linux and macOS only)";
  return true;
}

// ffi_open(path?) -> a library handle (a whole number); with no path, the running
// program and everything it is linked to, which includes libc.
g::Value bi_ffi_open(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (unsupported(error, "ffi_open")) return g::Value::nil();
#if !defined(_WIN32)
  const char* path = nullptr;
  if (!args.empty() && args[0].type != g::Type::Nil) {
    if (args[0].type != g::Type::Str) { error = "ffi_open() wants a path or nothing"; return g::Value::nil(); }
    path = args[0].str().c_str();
  }
  void* h = ::dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!h) { const char* why = ::dlerror(); error = std::string("ffi_open(): ") + (why ? why : "cannot open"); return g::Value::nil(); }
  return g::Value::integer((int64_t)(uintptr_t)h);
#else
  (void)args;
  return g::Value::nil();
#endif
}

g::Value bi_ffi_close(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (unsupported(error, "ffi_close")) return g::Value::nil();
#if !defined(_WIN32)
  if (args[0].type != g::Type::Int) { error = "ffi_close() wants a library handle"; return g::Value::nil(); }
  ::dlclose(reinterpret_cast<void*>((uintptr_t)args[0].i));
#endif
  return g::Value::nil();
}

#if !defined(_WIN32)
void* find_symbol(const g::Value& lib, const std::string& name, std::string& error, const char* who) {
  if (lib.type != g::Type::Nil && lib.type != g::Type::Int) { error = std::string(who) + "() wants a library from ffi_open() (or nil for the program itself)"; return nullptr; }
  void* h = lib.type == g::Type::Int ? reinterpret_cast<void*>((uintptr_t)lib.i) : RTLD_DEFAULT;
  ::dlerror();
  void* p = ::dlsym(h, name.c_str());
  if (!p) { const char* why = ::dlerror(); error = std::string(who) + "(): no symbol \"" + name + "\"" + (why ? std::string(" (") + why + ")" : ""); }
  return p;
}
#endif

// ffi_sym(lib, "name") -> the symbol's address (a function or a variable)
g::Value bi_ffi_sym(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (unsupported(error, "ffi_sym")) return g::Value::nil();
#if !defined(_WIN32)
  if (args[1].type != g::Type::Str) { error = "ffi_sym() wants a name"; return g::Value::nil(); }
  void* p = find_symbol(args[0], args[1].str(), error, "ffi_sym");
  return p ? g::Value::integer((int64_t)(uintptr_t)p) : g::Value::nil();
#else
  return g::Value::nil();
#endif
}

// ffi_fn(lib, "name", "i32(cstr, i64)") -> a function you call like any other
g::Value bi_ffi_fn(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (unsupported(error, "ffi_fn")) return g::Value::nil();
#if !defined(_WIN32)
  if (args[1].type != g::Type::Str || args[2].type != g::Type::Str) { error = "ffi_fn(lib, name, signature) wants strings for the name and signature"; return g::Value::nil(); }
  Sig sig;
  if (!parse_sig(args[2].str(), sig, error, "ffi_fn")) return g::Value::nil();
  void* p = find_symbol(args[0], args[1].str(), error, "ffi_fn");
  if (!p) return g::Value::nil();
  auto rec = std::make_shared<g::MapObj>();
  rec->put("ptr", g::Value::integer((int64_t)(uintptr_t)p));
  rec->put("sig", args[2]);
  rec->put("name", args[1]);
  const int min_args = (int)sig.args.size();
  return g::Value::object(g::Type::BoundMethod, std::make_shared<g::BoundMethodObj>(
      g::Value::object(g::Type::Map, rec), args[1].str(), min_args, sig.variadic ? -1 : min_args, method_call));
#else
  return g::Value::nil();
#endif
}

// ffi_call(address, "i32(cstr)", args...): call an address you hold, such as one from ffi_sym.
g::Value bi_ffi_ptr(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (unsupported(error, "ffi_ptr")) return g::Value::nil();
  if (args[0].type != g::Type::Int || args[0].i == 0) { error = "ffi_ptr(address, signature) wants an address"; return g::Value::nil(); }
  if (args[1].type != g::Type::Str) { error = "ffi_ptr(address, signature) wants a signature"; return g::Value::nil(); }
  Sig sig;
  if (!parse_sig(args[1].str(), sig, error, "ffi_ptr")) return g::Value::nil();
  auto rec = std::make_shared<g::MapObj>();
  rec->put("ptr", args[0]);
  rec->put("sig", args[1]);
  rec->put("name", g::Value::string("ffi_ptr"));
  const int min_args = (int)sig.args.size();
  return g::Value::object(g::Type::BoundMethod, std::make_shared<g::BoundMethodObj>(
      g::Value::object(g::Type::Map, rec), "ffi_ptr", min_args, sig.variadic ? -1 : min_args, method_call));
}

// ---- raw memory ----------------------------------------------------------

g::Value bi_mem_alloc(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Int || args[0].i < 0 || args[0].i > (int64_t(1) << 31)) { error = "mem_alloc() wants a size from 0 to 2147483648"; return g::Value::nil(); }
  void* p = std::calloc(1, (size_t)std::max<int64_t>(args[0].i, 1));
  if (!p) { error = "mem_alloc(): out of memory"; return g::Value::nil(); }
  return g::Value::integer((int64_t)(uintptr_t)p);
}

g::Value bi_mem_free(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Int) { error = "mem_free() wants an address from mem_alloc()"; return g::Value::nil(); }
  std::free(reinterpret_cast<void*>((uintptr_t)args[0].i));
  return g::Value::nil();
}

// mem_read(address, count) -> bytes: a copy of that memory
g::Value bi_mem_read(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Int || args[1].type != g::Type::Int || args[1].i < 0 || args[1].i > (int64_t(1) << 31) || args[0].i == 0) {
    error = "mem_read(address, count) wants a non-null address and a count";
    return g::Value::nil();
  }
  const auto* p = reinterpret_cast<const uint8_t*>((uintptr_t)args[0].i);
  return g::Value::bytes(std::vector<uint8_t>(p, p + args[1].i));
}

// mem_write(address, bytes) -> how many bytes were written
g::Value bi_mem_write(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Int || args[0].i == 0 || args[1].type != g::Type::Bytes) { error = "mem_write(address, bytes) wants a non-null address and bytes"; return g::Value::nil(); }
  const auto& d = args[1].bytes_data();
  std::memcpy(reinterpret_cast<void*>((uintptr_t)args[0].i), d.data(), d.size());
  return g::Value::integer((int64_t)d.size());
}

// cstr_at(address, max?) -> the NUL-terminated string there
g::Value bi_cstr_at(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Int || args[0].i == 0) { error = "cstr_at() wants a non-null address"; return g::Value::nil(); }
  const char* p = reinterpret_cast<const char*>((uintptr_t)args[0].i);
  const int64_t max = args.size() > 1 && args[1].type == g::Type::Int ? args[1].i : INT64_MAX;
  int64_t n = 0;
  while (n < max && p[n]) ++n;
  return g::Value::string(std::string(p, (size_t)n));
}

// bytes_ptr(b) -> the address of a bytes value's storage (good while b is alive)
g::Value bi_bytes_ptr(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Bytes) { error = "bytes_ptr() wants bytes"; return g::Value::nil(); }
  return g::Value::integer((int64_t)(uintptr_t)args[0].bytes_data().data());
}

g::Value bi_ffi_errno(g::Vm&, std::vector<g::Value>&, std::string&) { return g::Value::integer(errno); }

}  // namespace

void install_ffi(g::Vm& vm) {
  vm.define_native("ffi_open", 0, 1, bi_ffi_open);
  vm.define_native("ffi_close", 1, 1, bi_ffi_close);
  vm.define_native("ffi_sym", 2, 2, bi_ffi_sym);
  vm.define_native("ffi_fn", 3, 3, bi_ffi_fn);
  vm.define_native("ffi_ptr", 2, 2, bi_ffi_ptr);
  vm.define_native("ffi_errno", 0, 0, bi_ffi_errno);
  vm.define_native("mem_alloc", 1, 1, bi_mem_alloc);
  vm.define_native("mem_free", 1, 1, bi_mem_free);
  vm.define_native("mem_read", 2, 2, bi_mem_read);
  vm.define_native("mem_write", 2, 2, bi_mem_write);
  vm.define_native("cstr_at", 1, 2, bi_cstr_at);
  vm.define_native("bytes_ptr", 1, 1, bi_bytes_ptr);
}

}  // namespace grain_cli
