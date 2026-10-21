// aios-ts-pack: packs the parse tables of the vendored tree-sitter grammars, and proves the packing changed nothing.
//
// A tree-sitter parser.c is mostly tables (the C# grammar's dense parse table alone is 5 MB of an 8 MB object). They
// compress about 13 to 1, so each grammar is compiled from a parser_packed.c whose big tables are zlib streams that
// src/userland/ts_packed.cpp unpacks into heap memory the first time the grammar's tree_sitter_<name>() is called.
// The lexer, the scanner and the small tables are left exactly as generated.
//
//   aios-ts-pack [source-dir] [id ...]            write parser_packed.c for every grammar (or the ids named)
//   aios-ts-pack --verify [source-dir] [id ...]   compare each packed grammar with the original, byte for byte
//                                                 and by parsing a sample; nonzero exit if anything differs
//
// Run through the build: `cmake --build <build> --target ts-pack` (then `ts-pack-verify`). The generated files are
// checked in, so a build needs none of this; ctest's ts-pack.fresh fails when a parser.c has changed since its
// packed file was made.
//
// How it reads the tables: it links the ORIGINAL parser.c of each grammar, compiled under another name (orig.c.in),
// so the C compiler, not this program, evaluates the designated initialisers and macros, and the tables arrive as
// memory. The text of parser.c is then edited: the table definitions are cut out and the TSLanguage's fields are
// pointed at the unpacked copies.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aios/crypto/sha256.hpp"
#include "aios/userland/git.hpp"
#include "tree_sitter/api.h"

struct AiosTsTable {
  const char* name;
  const void* data;
  size_t bytes;
  unsigned stride;
};

extern "C" const void* aios_ts_field(const TSLanguage* l, const char* name);

#define AIOS_TS_ENTRY(id, dir, fn) \
  extern "C" { size_t aios_ts_tables_##id(const AiosTsTable**); const TSLanguage* aios_ts_orig_##id(void); const TSLanguage* fn(void); }
#include "ts-registry.inc"
#undef AIOS_TS_ENTRY

namespace {

struct Grammar {
  std::string id, dir;
  size_t (*tables)(const AiosTsTable**);
  const TSLanguage* (*orig)();
  const TSLanguage* (*packed)();
};

const std::vector<Grammar>& grammars() {
  static const std::vector<Grammar> all = {
#define AIOS_TS_ENTRY(id, dir, fn) {#id, dir, aios_ts_tables_##id, aios_ts_orig_##id, fn},
#include "ts-registry.inc"
#undef AIOS_TS_ENTRY
  };
  return all;
}

#include "samples.inc"

// A table is packed when it is at least this big; the rest are not worth the indirection.
constexpr size_t kMinPackedBytes = 4096;

// TSLanguage field each table feeds (the names tree-sitter's generator gives them).
struct TableField {
  const char* table;
  const char* field;
};
const TableField kFields[] = {
    {"ts_parse_table", "parse_table"},
    {"ts_small_parse_table", "small_parse_table"},
    {"ts_small_parse_table_map", "small_parse_table_map"},
    {"ts_parse_actions", "parse_actions"},
    {"ts_lex_modes", "lex_modes"},
    {"ts_primary_state_ids", "primary_state_ids"},
    {"ts_alias_sequences", "alias_sequences"},
};

const char* field_of(const std::string& table) {
  for (const TableField& f : kFields)
    if (table == f.table) return f.field;
  return nullptr;
}

bool read_file(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "aios-ts-pack: %s\n", msg.c_str());
  std::exit(1);
}

// The index of the } that closes the { at `open`, skipping strings, characters and comments.
size_t match_brace(const std::string& s, size_t open) {
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return i;
    } else if (c == '"' || c == '\'') {
      for (++i; i < s.size() && s[i] != c; ++i)
        if (s[i] == '\\') ++i;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i = s.find("*/", i + 2);
      if (i == std::string::npos) die("unterminated comment");
      ++i;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) return std::string::npos;
    }
  }
  return std::string::npos;
}

// `static const T name[..] = { ... };` as [begin of its line, one past the ;), or false.
bool find_table(const std::string& src, const std::string& name, size_t* begin, size_t* end) {
  for (size_t at = src.find(name + "["); at != std::string::npos; at = src.find(name + "[", at + 1)) {
    if (at == 0 || src[at - 1] != ' ') continue;
    const size_t line = src.rfind('\n', at);
    const size_t start = line == std::string::npos ? 0 : line + 1;
    if (src.compare(start, 13, "static const ") != 0) continue;
    const size_t eq = src.find('=', at);
    const size_t open = src.find('{', eq);
    if (eq == std::string::npos || open == std::string::npos) continue;
    const size_t close = match_brace(src, open);
    if (close == std::string::npos) die("table " + name + " has no end");
    size_t semi = close + 1;
    while (semi < src.size() && (src[semi] == ' ' || src[semi] == '\t')) ++semi;
    if (semi >= src.size() || src[semi] != ';') die("table " + name + " does not end with ;");
    *begin = start;
    *end = semi + 1;
    return true;
  }
  return false;
}

// Byte-plane shuffle: all the first bytes of the elements, then all the second bytes, ... The tables are mostly
// repeated 16-bit state numbers, which deflate far better this way.
std::vector<uint8_t> shuffle(const uint8_t* raw, size_t bytes, unsigned stride) {
  const size_t count = bytes / stride;
  std::vector<uint8_t> out(bytes);
  for (unsigned k = 0; k < stride; ++k)
    for (size_t i = 0; i < count; ++i) out[static_cast<size_t>(k) * count + i] = raw[i * stride + k];
  return out;
}

std::vector<uint8_t> unshuffle(const uint8_t* planes, size_t bytes, unsigned stride) {
  const size_t count = bytes / stride;
  std::vector<uint8_t> out(bytes);
  for (unsigned k = 0; k < stride; ++k)
    for (size_t i = 0; i < count; ++i) out[i * stride + k] = planes[static_cast<size_t>(k) * count + i];
  return out;
}

std::string sha256_hex(const std::string& data) {
  const auto h = aios::crypto::sha256(aios::ByteSpan(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
  return aios::crypto::bytes_to_hex(aios::ByteSpan(h.data(), h.size()));
}

std::string c_array(const std::string& name, const aios::ByteBuffer& data) {
  std::string out = "static const unsigned char " + name + "[" + std::to_string(data.size()) + "] = {\n";
  for (size_t i = 0; i < data.size(); i += 24) {
    for (size_t j = i; j < std::min(data.size(), i + 24); ++j) {
      out += std::to_string(data[j]);
      if (j + 1 < data.size()) out += ',';
    }
    out += '\n';
  }
  return out + "};\n";
}

const char kHelper[] =
    "/* Packed tables: unpacked once, into heap memory that lives as long as the process (the language is a singleton).\n"
    "   The unpacker, the once-only guard and the failure exit are in src/userland/ts_packed.cpp. */\n"
    "extern int aios_ts_unpack(const unsigned char *z, unsigned long zlen, unsigned long rawlen, unsigned stride, void *out);\n"
    "extern const void *aios_ts_alloc(unsigned long bytes);\n"
    "extern int aios_ts_begin(int *state);   /* 1: this caller unpacks; 0: already done (waits if another thread is at it) */\n"
    "extern void aios_ts_end(int *state);\n"
    "extern void aios_ts_fail(void);\n"
    "static const void *aios_ts_table(const unsigned char *z, unsigned long zlen, unsigned long rawlen, unsigned stride) {\n"
    "  void *p = (void *)aios_ts_alloc(rawlen);\n"
    "  if (!p || !aios_ts_unpack(z, zlen, rawlen, stride, p)) aios_ts_fail();\n"
    "  return p;\n"
    "}\n\n";

struct Packed {
  std::string text;
  size_t raw_bytes = 0, packed_bytes = 0;
};

Packed pack(const Grammar& g, const std::string& src) {
  const AiosTsTable* tables = nullptr;
  const size_t n = g.tables(&tables);

  struct Cut {
    const AiosTsTable* table;
    size_t begin, end;
    aios::ByteBuffer z;
  };
  std::vector<Cut> cuts;
  Packed result;
  for (size_t i = 0; i < n; ++i) {
    const AiosTsTable& t = tables[i];
    if (t.bytes < kMinPackedBytes) continue;
    Cut c;
    c.table = &t;
    if (!find_table(src, t.name, &c.begin, &c.end)) die(g.id + ": cannot find the definition of " + t.name + " in parser.c");
    if (t.bytes % t.stride) die(g.id + ": " + t.name + " is not a whole number of elements");
    const auto planes = shuffle(static_cast<const uint8_t*>(t.data), t.bytes, t.stride);
    c.z = aios::userland::zlib_deflate(aios::ByteSpan(planes.data(), planes.size()), 9);
    // the unpacker the build uses must give the table back exactly
    auto back = aios::userland::zlib_inflate(aios::ByteSpan(c.z.data(), c.z.size()));
    if (!back.ok() || back->size() != t.bytes) die(g.id + ": " + t.name + " does not inflate to its own size");
    const auto again = unshuffle(back->data(), t.bytes, t.stride);
    if (std::memcmp(again.data(), t.data, t.bytes) != 0) die(g.id + ": " + t.name + " does not round-trip");
    result.raw_bytes += t.bytes;
    result.packed_bytes += c.z.size();
    cuts.push_back(std::move(c));
  }

  // cut the definitions out, last first so the earlier offsets hold
  std::string out = src;
  std::vector<size_t> order(cuts.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cuts[a].begin > cuts[b].begin; });
  for (size_t i : order) out.replace(cuts[i].begin, cuts[i].end - cuts[i].begin, std::string("/* ") + cuts[i].table->name + ": packed, see the top of this file */");

  // point the TSLanguage's fields at the unpacked tables
  const std::string decl = "static const TSLanguage language = {";
  const size_t lang = out.find(decl);
  if (lang == std::string::npos) die(g.id + ": no `static const TSLanguage language = {`");
  const size_t lang_open = lang + decl.size() - 1;
  const size_t lang_close = match_brace(out, lang_open);
  std::string body = out.substr(lang_open + 1, lang_close - lang_open - 1);
  std::string inits;
  for (const Cut& c : cuts) {
    const char* field = field_of(c.table->name);
    if (!field) die(g.id + ": no TSLanguage field for " + c.table->name);
    const std::string key = std::string(".") + field;
    size_t at = body.find(key + " ");
    if (at == std::string::npos) at = body.find(key + "=");
    if (at == std::string::npos) die(g.id + ": the language has no ." + field);
    const size_t eol = body.find(',', at);
    const std::string old = body.substr(at, eol - at);
    if (old.find(c.table->name) == std::string::npos) die(g.id + ": ." + field + " does not point at " + c.table->name);
    body.replace(at, eol - at, key + " = 0");
    inits += std::string("    *(const void **)&language.") + field + " = aios_ts_table(aios_packed_" + c.table->name + ", " + std::to_string(c.z.size()) + ", " +
             std::to_string(c.table->bytes) + ", " + std::to_string(c.table->stride) + ");\n";
  }
  out = out.substr(0, lang) + "static TSLanguage language = {" + body + "}" + out.substr(lang_close + 1);

  // the unpack, just before the function returns the language
  const size_t ret = out.find("return &language;", lang);
  if (ret == std::string::npos) die(g.id + ": no `return &language;`");
  const size_t ret_line = out.rfind('\n', ret) + 1;
  out.insert(ret_line,
             "  static int aios_state;\n"
             "  if (aios_ts_begin(&aios_state)) {\n" +
                 inits +
                 "    aios_ts_end(&aios_state);\n"
                 "  }\n");

  // the helper goes in front of the function that builds the language
  const size_t fn = out.rfind("TS_PUBLIC");
  if (fn == std::string::npos) die(g.id + ": no TS_PUBLIC function");
  out.insert(fn, kHelper);

  std::string blobs;
  for (const Cut& c : cuts) blobs += c_array(std::string("aios_packed_") + c.table->name, c.z);
  result.text = "/* GENERATED by aios-ts-pack from parser.c, sha256 " + sha256_hex(src) +
                ". Do not edit; run: cmake --build <build> --target ts-pack */\n/* " + std::to_string(result.raw_bytes) +
                " bytes of tables, packed to " + std::to_string(result.packed_bytes) + " */\n" + blobs + "\n" + out;
  return result;
}

bool wanted(const std::vector<std::string>& ids, const std::string& id) { return ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end(); }

// ---- --verify ----------------------------------------------------------------------------------

std::string tree_of(const TSLanguage* lang, const char* source, bool* has_error) {
  TSParser* p = ts_parser_new();
  std::string out;
  if (ts_parser_set_language(p, lang)) {
    TSTree* t = ts_parser_parse_string(p, nullptr, source, static_cast<uint32_t>(std::strlen(source)));
    TSNode root = ts_tree_root_node(t);
    char* s = ts_node_string(root);
    out = s;
    std::free(s);
    *has_error = ts_node_has_error(root);
    ts_tree_delete(t);
  } else {
    out = "<language rejected>";
  }
  ts_parser_delete(p);
  return out;
}

int verify(const std::vector<std::string>& ids) {
  int bad = 0;
  size_t total = 0;
  for (const Grammar& g : grammars()) {
    if (!wanted(ids, g.id)) continue;
    const TSLanguage* orig = g.orig();
    const TSLanguage* packed = g.packed();
    // every table, packed or not, reached through the language the parser hands out
    const AiosTsTable* tables = nullptr;
    const size_t n = g.tables(&tables);
    int table_bad = 0;
    for (size_t i = 0; i < n; ++i) {
      const void* got = aios_ts_field(packed, tables[i].name);
      if (!got || std::memcmp(got, tables[i].data, tables[i].bytes) != 0) {
        std::printf("  %s: %s differs after packing\n", g.id.c_str(), tables[i].name);
        ++table_bad;
      }
      total += tables[i].bytes;
    }
    int tree_bad = 0, tried = 0;
    for (const Sample& s : kSamples) {
      if (g.id != s.id) continue;
      ++tried;
      bool e1 = false, e2 = false;
      const std::string a = tree_of(orig, s.source, &e1), b = tree_of(packed, s.source, &e2);
      if (a != b || e1 != e2) {
        std::printf("  %s: the sample parses differently\n    original: %s\n    packed:   %s\n", g.id.c_str(), a.c_str(), b.c_str());
        ++tree_bad;
      }
    }
    if (!tried) {
      std::printf("  %s: no sample to parse\n", g.id.c_str());
      ++tree_bad;
    }
    std::printf("%-11s %zu tables %s, sample %s\n", g.id.c_str(), n, table_bad ? "DIFFER" : "identical", tree_bad ? "DIFFERS" : "parses the same");
    bad += table_bad + tree_bad;
  }
  std::printf("%s (%.1f MB of tables compared)\n", bad ? "FAILED" : "all grammars match their originals", static_cast<double>(total) / 1048576);
  return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool do_verify = false;
  std::string root = AIOS_TS_SOURCE_ROOT;
  std::vector<std::string> ids;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--verify") do_verify = true;
    else if (a.find('/') != std::string::npos) root = a;
    else ids.push_back(a);
  }
  if (do_verify) return verify(ids);

  size_t raw = 0, packed = 0;
  for (const Grammar& g : grammars()) {
    if (!wanted(ids, g.id)) continue;
    const std::string dir = root + "/third_party/tree-sitter-grammars/" + g.dir;
    std::string src;
    if (!read_file(dir + "/parser.c", &src)) die("cannot read " + dir + "/parser.c");
    const Packed p = pack(g, src);
    std::ofstream out(dir + "/parser_packed.c", std::ios::binary | std::ios::trunc);
    out << p.text;
    if (!out) die("cannot write " + dir + "/parser_packed.c");
    std::printf("%-11s %8.2f MB -> %6.2f MB\n", g.id.c_str(), static_cast<double>(p.raw_bytes) / 1048576, static_cast<double>(p.packed_bytes) / 1048576);
    raw += p.raw_bytes;
    packed += p.packed_bytes;
  }
  std::printf("total       %8.2f MB -> %6.2f MB\n", static_cast<double>(raw) / 1048576, static_cast<double>(packed) / 1048576);
  return 0;
}
