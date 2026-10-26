// grain's byte buffers: the mutable `bytes` value and what is done with one -
// typed reads and writes at an offset (bytes_get / bytes_set), struct-style
// pack / unpack, and copy, fill, find, hex and concatenation. This is the
// low-level layer: binary files, wire protocols, pixels, headers.
//
//   let b = bytes(8)                       8 zero bytes
//   bytes_set(b, 0, "u32be", 0xCAFEBABE)   a big-endian 32-bit word at offset 0
//   bytes_get(b, 0, "u16le")               two bytes read the other way round
//   pack(">HH4s", 80, 443, "grain")        -> bytes; unpack(">HH", b) -> [80, 443]
//
// Every access is bounds-checked and fails loudly: a script that thinks it
// wrote past the end must never be wrong about it.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"
#include "aios/net/deflate.hpp"

namespace aios::userland::grain {

namespace {

constexpr int64_t kMaxBytes = int64_t(1) << 31;

bool want_bytes(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Bytes) return true;
  error = std::string(who) + "() wants bytes, got a " + type_name(v.type);
  return false;
}

bool want_int(const Value& v, std::string& error, const char* who, const char* what) {
  if (v.type == Type::Int) return true;
  error = std::string(who) + "() wants a whole number for " + what + ", got a " + type_name(v.type);
  return false;
}

// "u32le", "i16be", "f64le", "u8": a number's width, kind and byte order.
struct NumType {
  int size = 0;
  bool is_signed = false, is_float = false, big = false;
};

bool parse_type(const std::string& t, NumType& out, std::string& error, const char* who) {
  if (t.size() < 2 || (t[0] != 'u' && t[0] != 'i' && t[0] != 'f')) {
    error = std::string(who) + "(): \"" + t + "\" is not a type; try u8 i8 u16le u16be i32le u32be i64le u64le f32le f64be ...";
    return false;
  }
  out = NumType{};
  out.is_float = t[0] == 'f';
  out.is_signed = t[0] == 'i';
  size_t digits = 1;
  while (digits < t.size() && t[digits] >= '0' && t[digits] <= '9') ++digits;
  const int bits = std::atoi(t.substr(1, digits - 1).c_str());
  const std::string order = t.substr(digits);
  const bool ok_bits = out.is_float ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 32 || bits == 64);
  if (!ok_bits) { error = std::string(who) + "(): \"" + t + "\" has no such width"; return false; }
  out.size = bits / 8;
  if (out.size == 1 && order.empty()) return true;
  if (order == "le") out.big = false;
  else if (order == "be") out.big = true;
  else { error = std::string(who) + "(): \"" + t + "\" needs a byte order: add le or be"; return false; }
  return true;
}

bool in_range(int64_t off, int64_t n, size_t size) { return off >= 0 && n >= 0 && (uint64_t)off + (uint64_t)n <= size; }

uint64_t load(const uint8_t* p, int size, bool big) {
  uint64_t v = 0;
  for (int k = 0; k < size; ++k) {
    const uint8_t byte = p[big ? k : size - 1 - k];
    v = (v << 8) | byte;
  }
  return v;
}

void store(uint8_t* p, int size, bool big, uint64_t v) {
  for (int k = 0; k < size; ++k) p[big ? size - 1 - k : k] = (uint8_t)(v >> (8 * k));
}

Value read_number(const uint8_t* p, const NumType& t) {
  const uint64_t raw = load(p, t.size, t.big);
  if (t.is_float) {
    if (t.size == 4) { uint32_t u = (uint32_t)raw; float f; std::memcpy(&f, &u, 4); return Value::real(f); }
    double d; std::memcpy(&d, &raw, 8); return Value::real(d);
  }
  if (!t.is_signed || t.size == 8) return Value::integer((int64_t)raw);   // u64 above 2^63 reads as its negative twin
  const int shift = 64 - 8 * t.size;
  return Value::integer((int64_t)(raw << shift) >> shift);
}

// The value must fit the field: a script that writes 300 into a byte is wrong.
bool write_number(uint8_t* p, const NumType& t, const Value& v, std::string& error, const char* who) {
  if (t.is_float) {
    if (!v.is_number()) { error = std::string(who) + "() wants a number to store, got a " + type_name(v.type); return false; }
    if (t.size == 4) { const float f = (float)v.as_double(); uint32_t u; std::memcpy(&u, &f, 4); store(p, 4, t.big, u); }
    else { const double d = v.as_double(); uint64_t u; std::memcpy(&u, &d, 8); store(p, 8, t.big, u); }
    return true;
  }
  if (v.type != Type::Int) { error = std::string(who) + "() wants a whole number to store, got a " + type_name(v.type); return false; }
  if (t.size < 8) {
    const int bits = 8 * t.size;
    const int64_t lo = t.is_signed ? -(int64_t(1) << (bits - 1)) : 0;
    const int64_t hi = t.is_signed ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
    if (v.i < lo || v.i > hi) {
      error = std::string(who) + "(): " + std::to_string(v.i) + " does not fit " + std::to_string(bits) + " bits (" +
              std::to_string(lo) + ".." + std::to_string(hi) + ")";
      return false;
    }
  }
  store(p, t.size, t.big, (uint64_t)v.i);
  return true;
}

// bytes(n) | bytes(string) | bytes(list of 0..255) | bytes(bytes) (a copy)
Value bi_bytes(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& a = args[0];
  if (a.type == Type::Int) {
    if (a.i < 0 || a.i > kMaxBytes) { error = "bytes(): a size from 0 to 2147483648, got " + std::to_string(a.i); return Value::nil(); }
    return Value::bytes(std::vector<uint8_t>((size_t)a.i, 0));
  }
  if (a.type == Type::Str) return Value::bytes(std::vector<uint8_t>(a.str().begin(), a.str().end()));
  if (a.type == Type::Bytes) return Value::bytes(a.bytes_data());
  if (a.type == Type::List) {
    std::vector<uint8_t> out;
    for (const Value& item : a.list_items()) {
      if (item.type != Type::Int || item.i < 0 || item.i > 255) { error = "bytes(): a list of whole numbers from 0 to 255, found " + item.repr(); return Value::nil(); }
      out.push_back((uint8_t)item.i);
    }
    return Value::bytes(std::move(out));
  }
  error = std::string("bytes() wants a size, string, list or bytes, got a ") + type_name(a.type);
  return Value::nil();
}

// bytes_str(b, from?, to?) -> the bytes as a string (text if they are text)
Value bi_bytes_str(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_str")) return Value::nil();
  const auto& d = args[0].bytes_data();
  const int64_t from = args.size() > 1 ? args[1].as_int() : 0;
  const int64_t to = args.size() > 2 ? args[2].as_int() : (int64_t)d.size();
  if (from < 0 || to < from || (size_t)to > d.size()) { error = "bytes_str(): range " + std::to_string(from) + ".." + std::to_string(to) + " is outside " + std::to_string(d.size()) + " bytes"; return Value::nil(); }
  return Value::string(std::string(d.begin() + from, d.begin() + to));
}

Value bi_bytes_hex(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_hex")) return Value::nil();
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (uint8_t b : args[0].bytes_data()) { out += hex[b >> 4]; out += hex[b & 15]; }
  return Value::string(std::move(out));
}

Value bi_bytes_from_hex(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "bytes_from_hex")) return Value::nil();
  std::vector<uint8_t> out;
  int hi = -1;
  for (char c : args[0].str()) {
    if (c == ' ' || c == '\n' || c == ':') continue;
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0) { error = std::string("bytes_from_hex(): \"") + c + "\" is not a hex digit"; return Value::nil(); }
    if (hi < 0) hi = v; else { out.push_back((uint8_t)(hi * 16 + v)); hi = -1; }
  }
  if (hi >= 0) { error = "bytes_from_hex(): an odd number of hex digits"; return Value::nil(); }
  return Value::bytes(std::move(out));
}

// bytes_get(b, offset, "u32le") -> number
Value bi_bytes_get(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_get") || !want_int(args[1], error, "bytes_get", "the offset") || !want_str(args[2], error, "bytes_get")) return Value::nil();
  NumType t;
  if (!parse_type(args[2].str(), t, error, "bytes_get")) return Value::nil();
  const auto& d = args[0].bytes_data();
  if (!in_range(args[1].i, t.size, d.size())) {
    error = "bytes_get(): " + args[2].str() + " at " + std::to_string(args[1].i) + " is outside bytes of " + std::to_string(d.size());
    return Value::nil();
  }
  return read_number(d.data() + args[1].i, t);
}

// bytes_set(b, offset, "u32le", value) -> b
Value bi_bytes_set(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_set") || !want_int(args[1], error, "bytes_set", "the offset") || !want_str(args[2], error, "bytes_set")) return Value::nil();
  NumType t;
  if (!parse_type(args[2].str(), t, error, "bytes_set")) return Value::nil();
  auto& d = args[0].bytes_data();
  if (!in_range(args[1].i, t.size, d.size())) {
    error = "bytes_set(): " + args[2].str() + " at " + std::to_string(args[1].i) + " is outside bytes of " + std::to_string(d.size());
    return Value::nil();
  }
  if (!write_number(d.data() + args[1].i, t, args[3], error, "bytes_set")) return Value::nil();
  return args[0];
}

// bytes_fill(b, value, from?, to?) -> b
Value bi_bytes_fill(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_fill") || !want_int(args[1], error, "bytes_fill", "the value")) return Value::nil();
  if (args[1].i < 0 || args[1].i > 255) { error = "bytes_fill(): a byte is 0 to 255"; return Value::nil(); }
  auto& d = args[0].bytes_data();
  const int64_t from = args.size() > 2 ? args[2].as_int() : 0;
  const int64_t to = args.size() > 3 ? args[3].as_int() : (int64_t)d.size();
  if (from < 0 || to < from || (size_t)to > d.size()) { error = "bytes_fill(): range " + std::to_string(from) + ".." + std::to_string(to) + " is outside " + std::to_string(d.size()) + " bytes"; return Value::nil(); }
  std::fill(d.begin() + from, d.begin() + to, (uint8_t)args[1].i);
  return args[0];
}

// bytes_copy(dst, dst_offset, src, src_offset?, count?) -> count copied. Overlap is safe.
Value bi_bytes_copy(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_copy") || !want_int(args[1], error, "bytes_copy", "the destination offset") || !want_bytes(args[2], error, "bytes_copy")) return Value::nil();
  auto& dst = args[0].bytes_data();
  const auto& src = args[2].bytes_data();
  const int64_t soff = args.size() > 3 ? args[3].as_int() : 0;
  if (soff < 0 || (size_t)soff > src.size()) { error = "bytes_copy(): source offset " + std::to_string(soff) + " is outside " + std::to_string(src.size()) + " bytes"; return Value::nil(); }
  const int64_t n = args.size() > 4 ? args[4].as_int() : (int64_t)src.size() - soff;
  if (!in_range(soff, n, src.size())) { error = "bytes_copy(): " + std::to_string(n) + " bytes from " + std::to_string(soff) + " run past the source's " + std::to_string(src.size()); return Value::nil(); }
  if (!in_range(args[1].i, n, dst.size())) { error = "bytes_copy(): " + std::to_string(n) + " bytes at " + std::to_string(args[1].i) + " run past the destination's " + std::to_string(dst.size()); return Value::nil(); }
  std::memmove(dst.data() + args[1].i, src.data() + soff, (size_t)n);
  return Value::integer(n);
}

Value bi_bytes_concat(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "bytes_concat")) return Value::nil();
  std::vector<uint8_t> out;
  for (const Value& v : args[0].list_items()) {
    if (v.type != Type::Bytes) { error = std::string("bytes_concat() wants a list of bytes, found a ") + type_name(v.type); return Value::nil(); }
    out.insert(out.end(), v.bytes_data().begin(), v.bytes_data().end());
  }
  return Value::bytes(std::move(out));
}

// bytes_find(b, needle, from?) -> index or -1; the needle is bytes or a string
Value bi_bytes_find(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_bytes(args[0], error, "bytes_find")) return Value::nil();
  std::vector<uint8_t> needle;
  if (args[1].type == Type::Bytes) needle = args[1].bytes_data();
  else if (args[1].type == Type::Str) needle.assign(args[1].str().begin(), args[1].str().end());
  else { error = std::string("bytes_find() looks for bytes or a string, got a ") + type_name(args[1].type); return Value::nil(); }
  const auto& d = args[0].bytes_data();
  const int64_t from = args.size() > 2 ? std::max<int64_t>(0, args[2].as_int()) : 0;
  if ((size_t)from > d.size()) return Value::integer(-1);
  auto it = std::search(d.begin() + from, d.end(), needle.begin(), needle.end());
  return Value::integer(it == d.end() && !needle.empty() ? -1 : (int64_t)(it - d.begin()));
}

// ---- pack / unpack: Python's struct, in the formats that matter ---------------
//
//   < little-endian (default)   > or ! big-endian
//   b B  8-bit    h H  16-bit    i I  32-bit    q Q  64-bit    f d  float, double
//   Ns   N bytes (a string or bytes, zero-padded or cut)       x  a zero byte
//   a count before a letter repeats it: "4B", "2H"
struct Item {
  char code;
  int count;
};

bool parse_format(const std::string& fmt, bool& big, std::vector<Item>& items, std::string& error, const char* who) {
  big = false;
  size_t i = 0;
  if (i < fmt.size() && (fmt[i] == '<' || fmt[i] == '>' || fmt[i] == '!' || fmt[i] == '=')) { big = fmt[i] == '>' || fmt[i] == '!'; ++i; }
  for (; i < fmt.size(); ++i) {
    if (fmt[i] == ' ') continue;
    int count = 0;
    bool has_count = false;
    while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') { count = count * 10 + (fmt[i] - '0'); has_count = true; ++i; if (count > (1 << 24)) { error = std::string(who) + "(): count too large"; return false; } }
    if (i >= fmt.size()) { error = std::string(who) + "(): the format ends in a count"; return false; }
    const char c = fmt[i];
    if (!std::strchr("bBhHiIqQfdsx", c)) { error = std::string(who) + "(): \"" + c + "\" is not a format letter (b B h H i I q Q f d s x)"; return false; }
    items.push_back({c, has_count ? count : 1});
  }
  return true;
}

int item_size(char c) {
  switch (c) {
    case 'b': case 'B': case 'x': return 1;
    case 'h': case 'H': return 2;
    case 'i': case 'I': case 'f': return 4;
    default: return 8;
  }
}

NumType item_type(char c, bool big) {
  NumType t;
  t.size = item_size(c);
  t.is_signed = c == 'b' || c == 'h' || c == 'i' || c == 'q';
  t.is_float = c == 'f' || c == 'd';
  t.big = big;
  return t;
}

Value bi_pack(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "pack")) return Value::nil();
  bool big;
  std::vector<Item> items;
  if (!parse_format(args[0].str(), big, items, error, "pack")) return Value::nil();
  std::vector<uint8_t> out;
  size_t next = 1;
  for (const Item& it : items) {
    if (it.code == 'x') { out.insert(out.end(), (size_t)it.count, 0); continue; }
    if (it.code == 's') {
      if (next >= args.size()) { error = "pack(): the format wants more values than were given"; return Value::nil(); }
      const Value& v = args[next++];
      std::vector<uint8_t> raw;
      if (v.type == Type::Str) raw.assign(v.str().begin(), v.str().end());
      else if (v.type == Type::Bytes) raw = v.bytes_data();
      else { error = std::string("pack(): ") + std::to_string(it.count) + "s wants a string or bytes, got a " + type_name(v.type); return Value::nil(); }
      raw.resize((size_t)it.count, 0);
      out.insert(out.end(), raw.begin(), raw.end());
      continue;
    }
    for (int k = 0; k < it.count; ++k) {
      if (next >= args.size()) { error = "pack(): the format wants more values than were given"; return Value::nil(); }
      const NumType t = item_type(it.code, big);
      uint8_t cell[8];
      if (!write_number(cell, t, args[next++], error, "pack")) return Value::nil();
      out.insert(out.end(), cell, cell + t.size);
    }
  }
  if (next != args.size()) { error = "pack(): " + std::to_string(args.size() - next) + " more value(s) than the format has"; return Value::nil(); }
  return Value::bytes(std::move(out));
}

// unpack(fmt, bytes, offset?) -> list
Value bi_unpack(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "unpack") || !want_bytes(args[1], error, "unpack")) return Value::nil();
  bool big;
  std::vector<Item> items;
  if (!parse_format(args[0].str(), big, items, error, "unpack")) return Value::nil();
  const auto& d = args[1].bytes_data();
  int64_t at = args.size() > 2 ? args[2].as_int() : 0;
  std::vector<Value> out;
  for (const Item& it : items) {
    const int64_t width = it.code == 's' ? it.count : (int64_t)item_size(it.code) * it.count;
    if (!in_range(at, width, d.size())) { error = "unpack(): the format needs " + std::to_string(width) + " bytes at " + std::to_string(at) + " but there are " + std::to_string(d.size()); return Value::nil(); }
    if (it.code == 'x') { at += width; continue; }
    if (it.code == 's') { out.push_back(Value::bytes(std::vector<uint8_t>(d.begin() + at, d.begin() + at + width))); at += width; continue; }
    const NumType t = item_type(it.code, big);
    for (int k = 0; k < it.count; ++k) { out.push_back(read_number(d.data() + at, t)); at += t.size; }
  }
  return Value::list(std::move(out));
}

Value bi_pack_size(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "pack_size")) return Value::nil();
  bool big;
  std::vector<Item> items;
  if (!parse_format(args[0].str(), big, items, error, "pack_size")) return Value::nil();
  int64_t n = 0;
  for (const Item& it : items) n += it.code == 's' ? it.count : (int64_t)item_size(it.code) * it.count;
  return Value::integer(n);
}

// A string's or bytes' contents, for the compression builtins.
bool data_of(const Value& v, ByteSpan& out, std::string& error, const char* who) {
  if (v.type == Type::Str) { out = ByteSpan(reinterpret_cast<const uint8_t*>(v.str().data()), v.str().size()); return true; }
  if (v.type == Type::Bytes) { out = ByteSpan(v.bytes_data().data(), v.bytes_data().size()); return true; }
  error = std::string(who) + "() wants a string or bytes, got a " + type_name(v.type);
  return false;
}

// gzip(data [, level]): RFC 1952 bytes, the in-tree deflate (level 1 quick .. 9 smallest, 6 the usual).
Value bi_gzip(Vm&, std::vector<Value>& args, std::string& error) {
  ByteSpan in;
  if (!data_of(args[0], in, error, "gzip")) return Value::nil();
  int level = 6;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "gzip")) return Value::nil();
    if (args[1].as_int() < 1 || args[1].as_int() > 9) { error = "gzip(): level is 1 to 9"; return Value::nil(); }
    level = static_cast<int>(args[1].as_int());
  }
  return Value::bytes(net::gzip_encode(in, level));
}

// gunzip(data [, max_bytes]): the bytes a gzip stream holds (every member, each CRC-checked). Stops with an error past
// max_bytes (default 64 MB) rather than inflating a small upload into gigabytes.
Value bi_gunzip(Vm&, std::vector<Value>& args, std::string& error) {
  ByteSpan in;
  if (!data_of(args[0], in, error, "gunzip")) return Value::nil();
  size_t max_out = 64u << 20;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "gunzip")) return Value::nil();
    if (args[1].as_int() <= 0 || args[1].as_int() > kMaxBytes) { error = "gunzip(): max_bytes is 1 to " + std::to_string(kMaxBytes); return Value::nil(); }
    max_out = static_cast<size_t>(args[1].as_int());
  }
  ByteBuffer out;
  std::string why;
  if (!net::gzip_decode(in, out, max_out, &why)) { error = "gunzip(): " + why; return Value::nil(); }
  return Value::bytes(std::move(out));
}

}  // namespace

void install_bytes_builtins(Vm& vm) {
  vm.define_native("gzip", 1, 2, bi_gzip);
  vm.define_native("gunzip", 1, 2, bi_gunzip);
  vm.define_native("bytes", 1, 1, bi_bytes);
  vm.define_native("bytes_str", 1, 3, bi_bytes_str);
  vm.define_native("bytes_hex", 1, 1, bi_bytes_hex);
  vm.define_native("bytes_from_hex", 1, 1, bi_bytes_from_hex);
  vm.define_native("bytes_get", 3, 3, bi_bytes_get);
  vm.define_native("bytes_set", 4, 4, bi_bytes_set);
  vm.define_native("bytes_fill", 2, 4, bi_bytes_fill);
  vm.define_native("bytes_copy", 3, 5, bi_bytes_copy);
  vm.define_native("bytes_concat", 1, 1, bi_bytes_concat);
  vm.define_native("bytes_find", 2, 3, bi_bytes_find);
  vm.define_native("pack", 1, -1, bi_pack);
  vm.define_native("unpack", 2, 3, bi_unpack);
  vm.define_native("pack_size", 1, 1, bi_pack_size);
}

}  // namespace aios::userland::grain
