// Language-aware merge on tree-sitter syntax trees (see git_structural.hpp).
#include "aios/userland/git_structural.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "tree_sitter/api.h"

extern "C" {
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_tsx(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_go(void);
const TSLanguage* tree_sitter_rust(void);
const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_java(void);
const TSLanguage* tree_sitter_c_sharp(void);
const TSLanguage* tree_sitter_xml(void);
const TSLanguage* tree_sitter_json(void);
const TSLanguage* tree_sitter_php(void);
const TSLanguage* tree_sitter_sql(void);
const TSLanguage* tree_sitter_yaml(void);
const TSLanguage* tree_sitter_toml(void);
}

namespace aios::userland {

namespace {

constexpr size_t kMaxBytes = 4u << 20;   // bigger files keep the line merge's verdict

std::string lower_ext(std::string_view path) {
  size_t slash = path.find_last_of('/');
  std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot == std::string_view::npos) return "";
  std::string e(base.substr(dot + 1));
  for (auto& ch : e) ch = (char)std::tolower((unsigned char)ch);
  return e;
}

const TSLanguage* language_named(const std::string& name) {
  if (name == "javascript") return tree_sitter_javascript();
  if (name == "typescript") return tree_sitter_typescript();
  if (name == "tsx") return tree_sitter_tsx();
  if (name == "python") return tree_sitter_python();
  if (name == "go") return tree_sitter_go();
  if (name == "rust") return tree_sitter_rust();
  if (name == "c") return tree_sitter_c();
  if (name == "cpp") return tree_sitter_cpp();
  if (name == "java") return tree_sitter_java();
  if (name == "csharp") return tree_sitter_c_sharp();
  if (name == "xml") return tree_sitter_xml();
  if (name == "json") return tree_sitter_json();
  if (name == "php") return tree_sitter_php();
  if (name == "sql") return tree_sitter_sql();
  if (name == "yaml") return tree_sitter_yaml();
  if (name == "toml") return tree_sitter_toml();
  return nullptr;
}

struct ParserDel { void operator()(TSParser* p) const { ts_parser_delete(p); } };
struct TreeDel { void operator()(TSTree* t) const { ts_tree_delete(t); } };
using TreePtr = std::unique_ptr<TSTree, TreeDel>;

struct Doc {
  std::string lang;
  std::string text;
  TreePtr tree;
  std::string_view at(uint32_t a, uint32_t b) const { return std::string_view(text).substr(a, b - a); }
  std::string_view of(TSNode n) const { return at(ts_node_start_byte(n), ts_node_end_byte(n)); }
};

thread_local std::string g_reason;
std::nullopt_t give_up(const char* why) {
  g_reason = why;
  return std::nullopt;
}

std::atomic<uint64_t> g_full{0}, g_incremental{0}, g_cached{0};

// Trees parsed lately, by language and text. A merge reads the same versions again and again (converge previews
// every in-flight change against main after each landing; the outline of a version is asked for by every check that
// touches it), and a tree copy is a reference count, not a parse.
class TreeCache {
 public:
  struct Entry { std::string lang, text; TreePtr tree; };
  TreePtr find(const std::string& lang, std::string_view text) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
      if ((*it)->lang != lang || (*it)->text != text) continue;
      lru_.splice(lru_.begin(), lru_, it);
      return TreePtr(ts_tree_copy(lru_.front()->tree.get()));
    }
    return nullptr;
  }
  void put(const std::string& lang, std::string_view text, const TSTree* tree) {
    std::lock_guard<std::mutex> lock(mu_);
    auto e = std::make_unique<Entry>();
    e->lang = lang;
    e->text.assign(text);
    e->tree.reset(ts_tree_copy(tree));
    bytes_ += e->text.size();
    lru_.push_front(std::move(e));
    while (lru_.size() > 1 && (lru_.size() > kEntries || bytes_ > kBytes)) {
      bytes_ -= lru_.back()->text.size();
      lru_.pop_back();
    }
  }

 private:
  static constexpr size_t kEntries = 64, kBytes = 32u << 20;
  std::mutex mu_;
  std::list<std::unique_ptr<Entry>> lru_;
  size_t bytes_ = 0;
};
TreeCache& tree_cache() { static TreeCache c; return c; }

TSPoint point_at(std::string_view text, size_t upto) {
  TSPoint pt{0, 0};
  for (size_t i = 0; i < upto; ++i) {
    if (text[i] == '\n') { ++pt.row; pt.column = 0; } else ++pt.column;
  }
  return pt;
}

// Tells `tree` (parsed from `a`) how `a` became `b`: one edit per hunk of their line diff, so a parse from it reuses
// everything between the hunks. Applied from the last hunk to the first, each in `a`'s coordinates, which the
// edits after it leave unchanged.
void edit_tree(TSTree* tree, std::string_view a, std::string_view b) {
  auto la = git_split_lines(a), lb = git_split_lines(b);
  auto edits = git_diff_lines(la, lb);
  if (edits.size() > 256) {   // a rewrite: one edit spanning what differs
    size_t pre = 0, n = std::min(a.size(), b.size());
    while (pre < n && a[pre] == b[pre]) ++pre;
    size_t suf = 0;
    while (suf < n - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
    TSInputEdit e{(uint32_t)pre, (uint32_t)(a.size() - suf), (uint32_t)(b.size() - suf), point_at(a, pre),
                  point_at(a, a.size() - suf), point_at(b, b.size() - suf)};
    ts_tree_edit(tree, &e);
    return;
  }
  // byte offset and row of each line start
  auto starts = [](const std::vector<std::string_view>& ls) {
    std::vector<uint32_t> s(ls.size() + 1, 0);
    for (size_t i = 0; i < ls.size(); ++i) s[i + 1] = s[i] + (uint32_t)ls[i].size();
    return s;
  };
  auto sa = starts(la), sb = starts(lb);
  for (size_t k = edits.size(); k-- > 0;) {
    const auto& e = edits[k];
    uint32_t start = sa[e.a_start], old_end = sa[e.a_start + e.a_len];
    uint32_t new_len = sb[e.b_start + e.b_len] - sb[e.b_start];
    TSInputEdit ie;
    ie.start_byte = start;
    ie.old_end_byte = old_end;
    ie.new_end_byte = start + new_len;
    ie.start_point = TSPoint{(uint32_t)e.a_start, 0};
    ie.old_end_point = TSPoint{(uint32_t)(e.a_start + e.a_len), 0};
    ie.new_end_point = TSPoint{(uint32_t)(e.a_start + e.b_len), 0};
    // a last line without a newline ends mid-row
    if (e.b_len && e.b_start + e.b_len == lb.size() && !lb.back().empty() && lb.back().back() != '\n')
      ie.new_end_point = TSPoint{(uint32_t)(e.a_start + e.b_len - 1), (uint32_t)lb.back().size()};
    if (e.a_len && e.a_start + e.a_len == la.size() && !la.back().empty() && la.back().back() != '\n')
      ie.old_end_point = TSPoint{(uint32_t)(e.a_start + e.a_len - 1), (uint32_t)la.back().size()};
    ts_tree_edit(tree, &ie);
  }
}

// Parses `text` as `lang_name`. With `from` (another version of the same file, already parsed: the base of a merge),
// the parse is incremental: `from`'s tree is told where the two texts differ (edit_tree) and tree-sitter reparses
// only what those edits touch, reusing the rest.
bool parse(const TSLanguage* lang, std::string_view text, Doc& d, const Doc* from = nullptr) {
  d.text.assign(text);
  if (auto hit = tree_cache().find(d.lang, text)) {
    ++g_cached;
    d.tree = std::move(hit);
    return !ts_node_has_error(ts_tree_root_node(d.tree.get()));
  }
  thread_local std::map<const TSLanguage*, std::unique_ptr<TSParser, ParserDel>> parsers;
  auto& p = parsers[lang];
  if (!p) {
    p.reset(ts_parser_new());
    if (!ts_parser_set_language(p.get(), lang)) { p.reset(); return false; }
  }
  TreePtr old;
  if (from && from->tree && from->lang == d.lang) {
    old.reset(ts_tree_copy(from->tree.get()));
    edit_tree(old.get(), from->text, d.text);
  }
  d.tree.reset(ts_parser_parse_string(p.get(), old.get(), d.text.data(), (uint32_t)d.text.size()));
  if (!d.tree) { ts_parser_reset(p.get()); return false; }
  ++(old ? g_incremental : g_full);
  tree_cache().put(d.lang, text, d.tree.get());
  return !ts_node_has_error(ts_tree_root_node(d.tree.get()));
}

// A child by field name. Field names are looked up once per language and name (the lookup compares strings, and the
// outline asks for the same few fields of every node).
TSNode field(TSNode n, const char* name) {
  thread_local std::map<std::pair<const TSLanguage*, const char*>, TSFieldId> ids;
  const TSLanguage* lang = ts_tree_language(n.tree);
  auto key = std::make_pair(lang, name);
  auto it = ids.find(key);
  if (it == ids.end()) it = ids.emplace(key, ts_language_field_id_for_name(lang, name, (uint32_t)std::strlen(name))).first;
  if (!it->second) return TSNode{};
  return ts_node_child_by_field_id(n, it->second);
}

// Named, non-extra (comments are extras: they travel in the gaps, with what follows them) children.
std::vector<TSNode> units_of(TSNode n, const Doc* d = nullptr) {
  std::vector<TSNode> out;
  uint32_t k = ts_node_child_count(n);
  for (uint32_t i = 0; i < k; ++i) {
    TSNode ch = ts_node_child(n, i);
    if (!ts_node_is_named(ch) || ts_node_is_extra(ch)) continue;
    // XML: the blanks between elements are not units (they travel in the gaps, like comments), only real text is
    if (d && d->lang == "xml" && std::string(ts_node_type(ch)) == "CharData" &&
        d->of(ch).find_first_not_of(" \t\r\n") == std::string_view::npos) continue;
    out.push_back(ch);
  }
  return out;
}

TSNode body_of(TSNode n, int depth = 0);

// Units known only by their text that are members of a collection rather than steps of code: a YAML or TOML list's
// items, a PHP array's values, a SQL script's unnamed statements (an INSERT, an ALTER). Each side's new items are
// kept, in place; where both sides take items out and put others in, and they took out the same one, that is two
// rewrites of one item and the merge gives up (see Merger::level).
bool is_item(const Doc& d, TSNode n) {
  std::string t = ts_node_type(n);
  if (d.lang == "yaml") return t == "block_sequence_item" || t == "flow_node";
  if (d.lang == "toml") return t != "pair" && t != "table" && t != "table_array_element";
  if (d.lang == "php") return t == "array_element_initializer";
  if (d.lang == "sql") return t == "statement";
  return false;
}

// What a unit is known by at its level. Declarations by kind and name; imports and other statements by their text
// (an unnamed unit is only ever identical or different, never "the same thing, edited").
std::string key_of(const Doc& d, TSNode n, bool* keyed, int depth = 0) {
  std::string type = ts_node_type(n);
  if (depth > 8) { *keyed = false; return "text:" + std::string(d.of(n)); }
  if (d.lang == "xml" && type == "element") {
    // an element is known by its tag and the attribute that tells it from its siblings (x:Name, Name, Key, Text, Header...);
    // one with nothing like that is known only by its text, so two different anonymous <Grid>s are never mistaken for one
    TSNode tag = TSNode{};
    for (uint32_t i = 0; i < ts_node_child_count(n); ++i) {
      TSNode ch = ts_node_child(n, i);
      std::string ct = ts_node_type(ch);
      if (ct == "STag" || ct == "EmptyElemTag") { tag = ch; break; }
    }
    if (!ts_node_is_null(tag)) {
      std::string name; std::map<std::string, std::string> attrs;
      for (uint32_t i = 0; i < ts_node_child_count(tag); ++i) {
        TSNode ch = ts_node_child(tag, i);
        std::string ct = ts_node_type(ch);
        if (ct == "Name" && name.empty()) name = std::string(d.of(ch));
        if (ct == "Attribute") {
          std::string an, av;
          for (uint32_t j = 0; j < ts_node_child_count(ch); ++j) {
            TSNode a = ts_node_child(ch, j); std::string at = ts_node_type(a);
            if (at == "Name") an = std::string(d.of(a)); else if (at == "AttValue") av = std::string(d.of(a));
          }
          attrs[an] = av;
        }
      }
      static const char* ids[] = {"x:Name", "Name", "x:Key", "Key", "Header", "Title", "Text", "Content", "Command", "Click", "Source", "ItemsSource", "Value", "Include", "Select"};
      for (const char* id : ids) {
        auto it = attrs.find(id);
        if (it != attrs.end()) { *keyed = true; return "el:" + name + "|" + id + "=" + it->second; }
      }
    }
    // a container with nothing to tell it apart is known by its start tag (the tag and every attribute): what is inside it
    // can then be merged on both sides, and a side that edits its attributes is an edit of what it is (a rename)
    if (!ts_node_is_null(tag) && std::string(ts_node_type(tag)) == "STag") { *keyed = true; return "el:" + std::string(d.of(tag)); }
    *keyed = false;
    return "text:" + std::string(d.of(n));
  }
  for (const char* w : {"declaration", "definition"}) {
    TSNode inner = field(n, w);
    if (!ts_node_is_null(inner)) return type + "/" + key_of(d, inner, keyed, depth + 1);
  }
  if (d.lang == "cpp" && type == "template_declaration") {   // known as what it templates: template<class T> Foo<T>::bar
    for (uint32_t i = 0; i < ts_node_child_count(n); ++i) {
      TSNode ch = ts_node_child(n, i);
      std::string ct = ts_node_type(ch);
      if (ct == "template_parameter_list") continue;
      if (ts_node_is_named(ch) && ct != "comment") return "template/" + key_of(d, ch, keyed, depth + 1);
    }
  }
  *keyed = true;
  static const std::set<std::string> by_text = {"shorthand_property_identifier", "shorthand_property_identifier_pattern",
                                                "preproc_include", "use_declaration", "import_declaration", "import_statement", "using_directive",
                                                "string", "number", "identifier", "dotted_name", "aliased_import"};
  if (d.lang == "csharp" && (type == "field_declaration" || type == "event_field_declaration")) {
    // a field is known by the declarator(s) it declares: `public int a, b;` is the field a, b
    std::function<void(TSNode, std::string&)> names = [&](TSNode x, std::string& acc) {
      if (std::string(ts_node_type(x)) == "variable_declarator") {
        TSNode nm = field(x, "name");
        if (!ts_node_is_null(nm)) acc += (acc.empty() ? "" : ",") + std::string(d.of(nm));
        return;
      }
      for (uint32_t i = 0; i < ts_node_child_count(x); ++i) names(ts_node_child(x, i), acc);
    };
    std::string acc;
    names(n, acc);
    if (!acc.empty()) return type + ":" + acc;
  }
  TSNode name = field(n, "name");
  if (ts_node_is_null(name)) name = field(n, "key");
  if (ts_node_is_null(name)) name = field(n, "path");                                        // Go import_spec
  if (ts_node_is_null(name) && (type == "import_statement" || type == "export_statement")) name = field(n, "source");   // JS
  if (ts_node_is_null(name) && type == "import_from_statement") name = field(n, "module_name");                       // Python
  if (ts_node_is_null(name) && d.lang == "sql" && type == "create_index") name = field(n, "column");   // its own name
  if (ts_node_is_null(name)) {
    // named by their first part: a TOML table ([name]) or key/value pair, a PHP constant, a PHP array's key => value,
    // a SQL CREATE of a table, view, function... (its object reference)
    static const std::set<std::string> first_names = {"table", "table_array_element", "const_element"};
    auto kids = units_of(n, &d);
    if (!kids.empty() && (first_names.count(type) || (type == "pair" && d.lang == "toml") ||
                          (type == "array_element_initializer" && kids.size() == 2 &&
                           d.of(n).find("=>") != std::string_view::npos)))
      name = kids[0];
    if (d.lang == "sql" && type.rfind("create_", 0) == 0)
      for (auto& k : kids) if (std::string(ts_node_type(k)) == "object_reference") { name = k; break; }
  }
  if (!ts_node_is_null(name)) {
    std::string k = type + ":" + std::string(d.of(name));
    TSNode recv = field(n, "receiver");   // Go methods: the same name on two types
    if (!ts_node_is_null(recv)) k += "@" + std::string(d.of(recv));
    if ((d.lang == "java" || d.lang == "csharp") && (type == "method_declaration" || type == "constructor_declaration" || type == "operator_declaration" || type == "conversion_operator_declaration")) {   // overloads
      TSNode params = field(n, "parameters");
      if (!ts_node_is_null(params)) k += std::string(d.of(params));
    }
    return k;
  }
  if (type == "impl_item") {   // Rust: impl [Trait for] Type
    std::string k = "impl:";
    TSNode tr = field(n, "trait");
    if (!ts_node_is_null(tr)) k += std::string(d.of(tr)) + " for ";
    TSNode ty = field(n, "type");
    if (!ts_node_is_null(ty)) return k + std::string(d.of(ty));
  }
  TSNode decl = field(n, "declarator");
  if (!ts_node_is_null(decl)) {   // C: dig to the identifier the declarators wrap
    TSNode fn = TSNode{};   // C++: the function_declarator on the way down; a function is known by its parameters too (overloads)
    for (int i = 0; i < 16; ++i) {
      if (d.lang == "cpp" && ts_node_is_null(fn) && std::string(ts_node_type(decl)) == "function_declarator") fn = decl;
      TSNode next = field(decl, "declarator");
      if (ts_node_is_null(next)) break;
      decl = next;
    }
    std::string k = type + ":" + std::string(d.of(decl));
    if (!ts_node_is_null(fn)) {
      TSNode params = field(fn, "parameters");
      if (!ts_node_is_null(params)) k += std::string(d.at(ts_node_start_byte(params), ts_node_end_byte(fn)));   // (params) const noexcept...
    }
    return k;
  }
  static const std::set<std::string> wrappers = {"lexical_declaration", "variable_declaration", "const_declaration",
                                                 "var_declaration", "type_declaration", "field_declaration",
                                                 "local_variable_declaration", "expression_statement",
                                                 "property_declaration", "statement"};
  if (wrappers.count(type)) {
    auto kids = units_of(n, &d);
    if (kids.size() == 1 || (!kids.empty() && type != "expression_statement")) {
      TSNode k0 = kids[0];
      std::string kt = ts_node_type(k0);
      if (kt == "assignment_expression" || kt == "assignment" || kt == "augmented_assignment") {
        TSNode left = field(k0, "left");
        if (!ts_node_is_null(left)) return type + "=" + std::string(d.of(left));
      }
      // the first part that has a name (past modifiers: a PHP property's visibility)
      for (auto& k : kids) {
        bool inner_keyed = false;
        std::string ik = key_of(d, k, &inner_keyed, depth + 1);
        if (inner_keyed && ik.rfind("item:", 0) != 0) return type + "/" + ik;
        if (type == "expression_statement") break;
      }
    }
  }
  // Go's import ( ... ) block is one unit whatever it holds (its specs merge inside it); Java's import is its text
  if (type == "import_declaration" && !ts_node_is_null(body_of(n))) return type;
  if (by_text.count(type) || type == "namespace_use_declaration")
    return type + ":" + std::string(d.of(n));   // its text is what it is (a name, an import)
  if (depth == 0 && is_item(d, n)) return "item:" + std::string(d.of(n));
  *keyed = false;
  return "text:" + std::string(d.of(n));
}

bool is_list(const TSNode& n) {
  std::string t = ts_node_type(n);
  auto ends = [&](const char* s) { size_t l = std::strlen(s); return t.size() >= l && t.compare(t.size() - l, l, s) == 0; };
  return ends("body") || ends("block") || ends("_list") || t == "object" || t == "array" || t == "statement_block" ||
         t == "compound_statement" || t == "dictionary" || t == "list" || t == "object_type" || t == "class_body" ||
         t == "enum_body" || t == "interface_body" || t == "declaration_list" || t == "program" || t == "module" ||
         t == "named_imports" || t == "export_clause" || t == "import_list" ||
         t == "source_file" || t == "translation_unit" || t == "document" || t == "stream" || t == "block_mapping" ||
         t == "block_sequence" || t == "flow_mapping" || t == "flow_sequence" || t == "table" ||
         t == "table_array_element" || t == "inline_table" || t == "array_creation_expression" ||
         t == "column_definitions" || t == "compilation_unit" || t == "content" || t == "enum_member_declaration_list" || t == "accessor_list";
}

// The list a unit holds its members in (a class's body, an object value, a struct's fields), if it has one.
TSNode body_of(TSNode n, int depth) {
  if (depth > 6) return TSNode{};
  if (is_list(n) && depth > 0) return n;
  for (const char* f : {"body", "declaration", "definition", "value", "type", "right"}) {
    TSNode c = field(n, f);
    if (ts_node_is_null(c)) continue;
    if (is_list(c)) return c;
    TSNode deeper = body_of(c, depth + 1);
    if (!ts_node_is_null(deeper)) return deeper;
  }
  // Go: type_declaration -> type_spec -> struct_type -> field_declaration_list
  auto kids = units_of(n);
  if (kids.size() == 1 && depth < 6) return body_of(kids[0], depth + 1);
  // the one list among the unit's own parts (an import's named imports)
  TSNode found{};
  int lists = 0;
  for (auto& k : kids) if (is_list(k)) { found = k; ++lists; }
  if (lists == 1) return found;
  if (lists == 0)
    for (auto& k : kids)
      for (auto& g : units_of(k)) if (is_list(g)) { found = g; ++lists; }
  return lists == 1 ? found : TSNode{};
}

std::optional<std::string> same3(std::string_view b, std::string_view o, std::string_view t) {
  if (o == t) return std::string(o);
  if (o == b) return std::string(t);
  if (t == b) return std::string(o);
  return std::nullopt;
}

struct Unit {
  std::string key;
  bool keyed = false;
  TSNode node{};
  std::string lead, body;   // the gap before it (separators, comments) and the unit itself
  std::string trailer;      // a ';' right after it that the grammar leaves outside it (a C struct's), part of body
  std::string tail;         // the gap after it (used when it is placed first)
  std::string text() const { return lead + body; }
};
struct Level {
  std::string prefix, suffix;
  std::vector<Unit> units;
  std::map<std::string, size_t> at;
};

Level split(const Doc& d, TSNode list) {
  Level L;
  auto kids = units_of(list, &d);
  uint32_t s = ts_node_start_byte(list), e = ts_node_end_byte(list);
  if (kids.empty()) { L.prefix = std::string(d.at(s, e)); return L; }
  L.prefix = std::string(d.at(s, ts_node_start_byte(kids[0])));
  // where each unit ends: its node, and a ';' straight after it that terminates it
  std::vector<uint32_t> ends;
  for (size_t i = 0; i < kids.size(); ++i) {
    uint32_t end = ts_node_end_byte(kids[i]);
    uint32_t limit = i + 1 < kids.size() ? ts_node_start_byte(kids[i + 1]) : e;
    while (end < limit && d.text[end] == ';') ++end;
    ends.push_back(end);
  }
  std::map<std::string, int> seen;
  for (size_t i = 0; i < kids.size(); ++i) {
    Unit u;
    u.node = kids[i];
    u.key = key_of(d, kids[i], &u.keyed);
    int n = seen[u.key]++;
    if (n) u.key += "#" + std::to_string(n);
    u.lead = i == 0 ? "" : std::string(d.at(ends[i - 1], ts_node_start_byte(kids[i])));
    u.trailer = std::string(d.at(ts_node_end_byte(kids[i]), ends[i]));
    u.body = std::string(d.at(ts_node_start_byte(kids[i]), ends[i]));
    u.tail = i + 1 < kids.size() ? std::string(d.at(ends[i], ts_node_start_byte(kids[i + 1]))) : "";
    L.at[u.key] = L.units.size();
    L.units.push_back(std::move(u));
  }
  L.suffix = std::string(d.at(ends.back(), e));
  return L;
}

struct Merger {
  const Doc &B, &O, &T;
  int depth_limit = 12;

  // A key's fate in level(): its text, the separators it had before and after it, whether it came first, and on which
  // side. A member, not a class local to level(): MSVC 14.44 crashes (C1001) on std::map<std::string, std::optional<T>>
  // with a function-local T.
  struct Kept { std::string text, lead, tail; bool was_first = false; const Level* side = nullptr; };

  std::optional<std::string> unit(TSNode nb, TSNode no, TSNode nt, int depth) {
    if (depth > depth_limit) return give_up("nested too deep");
    // a list itself (a JSON object, an array): its members are the level
    if (is_list(nb) && is_list(no) && is_list(nt)) return level(nb, no, nt, depth + 1);
    TSNode bb = body_of(nb), bo = body_of(no), bt = body_of(nt);
    if (ts_node_is_null(bb) || ts_node_is_null(bo) || ts_node_is_null(bt)) return give_up("a unit both sides changed has no list of members to merge");
    auto head = same3(B.at(ts_node_start_byte(nb), ts_node_start_byte(bb)), O.at(ts_node_start_byte(no), ts_node_start_byte(bo)),
                      T.at(ts_node_start_byte(nt), ts_node_start_byte(bt)));
    auto tail = same3(B.at(ts_node_end_byte(bb), ts_node_end_byte(nb)), O.at(ts_node_end_byte(bo), ts_node_end_byte(no)),
                      T.at(ts_node_end_byte(bt), ts_node_end_byte(nt)));
    if (!head || !tail) return give_up("both sides changed a unit's head or tail");
    auto mid = level(bb, bo, bt, depth + 1);
    if (!mid) return std::nullopt;
    return *head + *mid + *tail;
  }

  static bool order_changed(const Level& base, const Level& side) {
    std::vector<std::string> a, b;
    for (auto& u : base.units) if (side.at.count(u.key)) a.push_back(u.key);
    for (auto& u : side.units) if (base.at.count(u.key)) b.push_back(u.key);
    return a != b;
  }

  static bool touches_unnamed(const Level& base, const Level& side) {
    for (auto& u : side.units) if (!u.keyed && !base.at.count(u.key)) return true;
    for (auto& u : base.units) if (!u.keyed && !side.at.count(u.key)) return true;
    return false;
  }

  // Items (and imports, names, strings: whatever is known by its own text) cannot be told apart from an edit of
  // them: an edited one is one taken out and another put in. So an item both sides took out while either put
  // something in (two new versions of one dependency, a step one side edited and the other deleted) is a conflict,
  // not two items to keep.
  static bool by_own_text(const Unit& u) {
    std::string k = u.key.substr(0, u.key.rfind('#') == std::string::npos ? u.key.size() : u.key.rfind('#'));
    return k == "item:" + u.body || k == std::string(ts_node_type(u.node)) + ":" + u.body;
  }
  // The base units a side took out and put something new in the place of: a rename, or an edit of what the unit is
  // known by (a key, a name). Against the other side deleting that unit too, it is an edit against a deletion.
  static std::set<std::string> renamed(const Level& base, const Level& side) {
    std::set<std::string> out;
    for (size_t i = 0; i < base.units.size(); ++i) {
      const std::string& k = base.units[i].key;
      if (side.at.count(k)) continue;
      size_t pos = 0;
      for (size_t j = i; j-- > 0;) {
        auto it = side.at.find(base.units[j].key);
        if (it != side.at.end()) { pos = it->second + 1; break; }
      }
      if (pos < side.units.size() && !base.at.count(side.units[pos].key)) out.insert(k);
    }
    return out;
  }

  static bool rewrite_same_item(const Level& base, const Level& o, const Level& t) {
    auto adds = [&](const Level& side) {
      for (auto& u : side.units) if (by_own_text(u) && !base.at.count(u.key)) return true;
      return false;
    };
    if (!adds(o) && !adds(t)) return false;
    for (auto& u : base.units)
      if (by_own_text(u) && !o.at.count(u.key) && !t.at.count(u.key)) return true;
    return false;
  }

  std::optional<std::string> level(TSNode lb, TSNode lo, TSNode lt, int depth) {
    Level LB = split(B, lb), LO = split(O, lo), LT = split(T, lt);
    // one unit on every side, of one kind (a document's single object, a wrapper's one child): the same unit
    if (LB.units.size() == 1 && LO.units.size() == 1 && LT.units.size() == 1) {
      std::string kb = ts_node_type(LB.units[0].node);
      // (unless they are named, and named differently: a renamed unit is another unit)
      auto named_apart = [&](const Unit& u) { return u.keyed && LB.units[0].keyed && u.key != LB.units[0].key; };
      if (kb == ts_node_type(LO.units[0].node) && kb == ts_node_type(LT.units[0].node) && !named_apart(LO.units[0]) &&
          !named_apart(LT.units[0])) {
        for (auto* L : {&LB, &LO, &LT}) {
          L->at.clear();
          L->units[0].key = "only:" + kb;
          L->units[0].keyed = true;
          L->at[L->units[0].key] = 0;
        }
      }
    }
    auto prefix = same3(LB.prefix, LO.prefix, LT.prefix);
    auto suffix = same3(LB.suffix, LO.suffix, LT.suffix);
    if (!prefix || !suffix) return give_up("both sides changed what opens or closes a list");
    // two sides both rewriting unnamed statements at one level cannot be told apart from each other: no guess
    if (touches_unnamed(LB, LO) && touches_unnamed(LB, LT)) return give_up("both sides changed unnamed statements at one level");
    if (rewrite_same_item(LB, LO, LT)) return give_up("both sides rewrote one item");
    for (auto& k : renamed(LB, LO)) if (!LT.at.count(k)) return give_up("ours renamed what theirs deleted");
    for (auto& k : renamed(LB, LT)) if (!LO.at.count(k)) return give_up("theirs renamed what ours deleted");
    const bool ro = order_changed(LB, LO), rt = order_changed(LB, LT);
    if (ro && rt) {
      std::vector<std::string> a, b;
      for (auto& u : LO.units) if (LB.at.count(u.key) && LT.at.count(u.key)) a.push_back(u.key);
      for (auto& u : LT.units) if (LB.at.count(u.key) && LO.at.count(u.key)) b.push_back(u.key);
      if (a != b) return give_up("both sides reordered, differently");
    }
    const Level& P = (rt && !ro) ? LT : LO;   // whose order the result keeps
    const Level& S = (rt && !ro) ? LO : LT;
    std::map<std::string, std::optional<Kept>> fate;   // nullopt: deleted
    auto kept = [](const Unit& u, const Level& L, std::string text) {
      return Kept{std::move(text), u.lead, u.tail, L.at.at(u.key) == 0, &L};
    };
    std::set<std::string> keys;
    for (auto* L : {&LB, &LO, &LT}) for (auto& u : L->units) keys.insert(u.key);
    for (auto& k : keys) {
      const Unit* b = LB.at.count(k) ? &LB.units[LB.at[k]] : nullptr;
      const Unit* o = LO.at.count(k) ? &LO.units[LO.at[k]] : nullptr;
      const Unit* t = LT.at.count(k) ? &LT.units[LT.at[k]] : nullptr;
      if (b && o && t) {
        if (o->body == t->body || t->body == b->body) { fate[k] = kept(*o, LO, o->body); continue; }
        if (o->body == b->body) { fate[k] = kept(*t, LT, t->body); continue; }
        auto lead = same3(b->lead, o->lead, t->lead);
        if (!lead) return give_up("both sides changed what precedes a unit");
        auto trailer = same3(b->trailer, o->trailer, t->trailer);
        if (!trailer) return give_up("both sides changed what ends a unit");
        auto inner = unit(b->node, o->node, t->node, depth);
        if (!inner) return std::nullopt;
        Kept x = kept(*o, LO, *inner + *trailer);
        x.lead = *lead;
        fate[k] = x;
      } else if (b && o) {
        if (o->body != b->body) return give_up("ours changed what theirs deleted");   // ours changed what theirs deleted
        fate[k] = std::nullopt;
      } else if (b && t) {
        if (t->body != b->body) return give_up("theirs changed what ours deleted");   // theirs changed what ours deleted
        fate[k] = std::nullopt;
      } else if (b) {
        fate[k] = std::nullopt;
      } else if (o && t) {
        if (o->body != t->body) return give_up("both sides added a unit, differently");   // both added it, differently
        fate[k] = kept(*o, LO, o->body);
      } else if (o) {
        fate[k] = kept(*o, LO, o->body);
      } else {
        fate[k] = kept(*t, LT, t->body);
      }
    }
    // the order: P's, with what only S added placed after what precedes it on S's side
    std::vector<std::string> order;
    for (auto& u : P.units) order.push_back(u.key);
    std::set<std::string> placed(order.begin(), order.end());
    for (size_t i = 0; i < S.units.size(); ++i) {
      const std::string& k = S.units[i].key;
      if (placed.count(k) || LB.at.count(k)) continue;   // in the base and gone from P: P deleted it
      size_t pos = 0;
      auto in_order = [&](size_t j) { return std::find(order.begin(), order.end(), S.units[j].key); };
      if (i > 0 && in_order(i - 1) == order.end() && i + 1 < S.units.size() && in_order(i + 1) != order.end()) {
        // what preceded it on S's side is gone from P (P rewrote it): it goes before what followed it instead
        pos = (size_t)(in_order(i + 1) - order.begin());
      } else {
        for (size_t j = i; j-- > 0;) {
          auto it = in_order(j);
          if (it != order.end()) { pos = (size_t)(it - order.begin()) + 1; break; }
        }
        // after what P itself added there, so both sides' additions at one place read ours-then-theirs
        while (pos < order.size() && !LB.at.count(order[pos]) && !S.at.count(order[pos])) ++pos;
      }
      order.insert(order.begin() + (long)pos, k);
      placed.insert(k);
    }
    std::string out = *prefix;
    bool first = true;
    std::string any_sep;
    for (const Level* L : std::array<const Level*, 3>{&P, &S, &LB})   // what separates units at this level, should nothing nearer say
      for (auto& u : L->units) if (any_sep.empty() && !u.lead.empty()) any_sep = u.lead;
    // The separator before a unit (commas, a statement's ';', blank lines, the comments above it) is the one a
    // version has between it and what now precedes it: where all three have the two together, the side that changed
    // it; else the unit's own side, else either side; and only where no version has the two together, the one it
    // had before whatever preceded it.
    auto adjacent = [](const Level& L, const std::string& a, const std::string& b) -> const Unit* {
      auto ia = L.at.find(a), ib = L.at.find(b);
      if (ia == L.at.end() || ib == L.at.end() || ib->second != ia->second + 1) return nullptr;
      return &L.units[ib->second];
    };
    std::string prev;
    for (auto& k : order) {
      auto it = fate.find(k);
      if (it == fate.end() || !it->second) continue;
      const Kept& f = *it->second;
      if (!first) {
        const Unit *ab = adjacent(LB, prev, k), *ao = adjacent(LO, prev, k), *at = adjacent(LT, prev, k);
        std::optional<std::string> sep;
        if (ab && ao && at) sep = same3(ab->lead, ao->lead, at->lead);
        if (!sep && f.side) if (const Unit* own = adjacent(*f.side, prev, k)) sep = own->lead;
        if (!sep && (ao || at)) sep = (&P == &LO ? (ao ? ao : at) : (at ? at : ao))->lead;
        if (!sep) sep = f.was_first ? (f.tail.empty() ? any_sep : f.tail) : f.lead;
        out += *sep;
      }
      out += f.text;
      first = false;
      prev = k;
    }
    out += *suffix;
    return out;
  }
};

// A line one side deleted and the other left as it was must not be in the result. Units are matched by name, so
// text one side moved into a new unit (a TOML table split in two, a YAML key renamed) while the other side deleted or
// changed it would otherwise come back; that is a conflict, and the merge gives up. Lines that occur more than once
// in a version prove nothing and are not looked at.
bool keeps_deletions(std::string_view base, std::string_view ours, std::string_view theirs, std::string_view merged) {
  auto count = [](std::string_view text) {
    std::map<std::string_view, int> c;
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      if (nl == std::string_view::npos) nl = text.size();
      std::string_view l = text.substr(at, nl - at);
      size_t a = l.find_first_not_of(" \t"), b = l.find_last_not_of(" \t\r,;");
      if (a != std::string_view::npos && b != std::string_view::npos && b >= a) ++c[l.substr(a, b - a + 1)];
      at = nl + 1;
    }
    return c;
  };
  auto cb = count(base), co = count(ours), ct = count(theirs), cm = count(merged);
  auto once = [](const std::map<std::string_view, int>& c, std::string_view l) {
    auto it = c.find(l);
    return it != c.end() && it->second == 1;
  };
  for (auto& [l, n] : cb) {
    if (n != 1 || l.size() < 2) continue;
    bool gone_o = !co.count(l), gone_t = !ct.count(l);
    if ((gone_o && once(ct, l)) || (gone_t && once(co, l)))
      if (cm.count(l)) return false;
  }
  return true;
}

// A merge of independent edits applies both, so a token (a word, a punctuation mark) whose count only one side
// changed has that side's count in the result, and one neither changed has the base's. Separators are what this
// catches: the text between units (a ',' a ';') travels with the units, and a unit placed somewhere new must not
// bring a second one or leave one behind.
bool keeps_counts(std::string_view base, std::string_view ours, std::string_view theirs, std::string_view merged) {
  auto count = [](std::string_view s) {
    std::map<std::string_view, int> m;
    for (size_t i = 0; i < s.size();) {
      unsigned char c = (unsigned char)s[i];
      if (std::isspace(c)) { ++i; continue; }
      size_t j = i + 1;
      if (std::isalnum(c) || c == '_' || c == '$')
        while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
      ++m[s.substr(i, j - i)];
      i = j;
    }
    return m;
  };
  auto cb = count(base), co = count(ours), ct = count(theirs), cm = count(merged);
  auto n = [](const std::map<std::string_view, int>& c, std::string_view k) {
    auto it = c.find(k);
    return it == c.end() ? 0 : it->second;
  };
  std::set<std::string_view> all;
  for (auto* c : {&cb, &co, &ct, &cm}) for (auto& [k, v] : *c) all.insert(k);
  for (auto k : all) {
    int nb = n(cb, k), no = n(co, k), nt = n(ct, k), nm = n(cm, k);
    if (no == nb && nt == nb && nm != nb) return false;
    if (no != nb && nt == nb && nm != no) return false;
    if (nt != nb && no == nb && nm != nt) return false;
  }
  return true;
}

}  // namespace

std::string git_structural_language(std::string_view path) {
  std::string e = lower_ext(path);
  if (e == "js" || e == "mjs" || e == "cjs" || e == "jsx") return "javascript";
  if (e == "ts" || e == "mts" || e == "cts") return "typescript";
  if (e == "tsx") return "tsx";
  if (e == "py" || e == "pyi") return "python";
  if (e == "go") return "go";
  if (e == "rs") return "rust";
  if (e == "c" || e == "h") return "c";
  if (e == "cpp" || e == "cc" || e == "cxx" || e == "hpp" || e == "hh" || e == "hxx" || e == "ipp") return "cpp";
  if (e == "java") return "java";
  if (e == "cs" || e == "csx") return "csharp";
  if (e == "xml" || e == "axaml" || e == "xaml" || e == "csproj" || e == "props" || e == "targets" || e == "resx" || e == "svg") return "xml";
  if (e == "json") return "json";
  if (e == "php" || e == "phtml") return "php";
  if (e == "sql") return "sql";
  if (e == "yaml" || e == "yml") return "yaml";
  if (e == "toml") return "toml";
  return "";
}

bool git_structural_parses(std::string_view path, std::string_view text) {
  const TSLanguage* lang = language_named(git_structural_language(path));
  if (!lang || text.size() > kMaxBytes) return true;
  Doc d;
  d.lang = git_structural_language(path);
  return parse(lang, text, d);
}

std::string git_structural_last_reason() { return g_reason; }

std::optional<GitMerge3> git_structural_merge(std::string_view path, std::string_view base, std::string_view ours,
                                              std::string_view theirs) {
  g_reason = "no grammar reads this file";
  std::string lang_name = git_structural_language(path);
  const TSLanguage* lang = language_named(lang_name);
  if (!lang) return std::nullopt;
  if (base.size() > kMaxBytes || ours.size() > kMaxBytes || theirs.size() > kMaxBytes) return std::nullopt;
  for (auto v : {base, ours, theirs}) if (v.find('\0') != std::string_view::npos) return give_up("a NUL byte");
  Doc b, o, t;
  b.lang = o.lang = t.lang = lang_name;
  if (!parse(lang, base, b) || !parse(lang, ours, o, &b) || !parse(lang, theirs, t, &b)) return give_up("a version does not parse");
  TSNode rb = ts_tree_root_node(b.tree.get()), ro = ts_tree_root_node(o.tree.get()), rt = ts_tree_root_node(t.tree.get());
  auto lead = same3(b.at(0, ts_node_start_byte(rb)), o.at(0, ts_node_start_byte(ro)), t.at(0, ts_node_start_byte(rt)));
  auto trail = same3(b.at(ts_node_end_byte(rb), (uint32_t)b.text.size()), o.at(ts_node_end_byte(ro), (uint32_t)o.text.size()),
                     t.at(ts_node_end_byte(rt), (uint32_t)t.text.size()));
  if (!lead || !trail) return give_up("both sides changed the file's start or end");
  Merger m{b, o, t};
  auto mid = m.level(rb, ro, rt, 0);
  if (!mid) return std::nullopt;
  GitMerge3 r;
  r.clean = true;
  r.merged = *lead + *mid + *trail;
  if (std::getenv("AIOS_STRUCTURAL_DEBUG")) std::fprintf(stderr, "%s\n", r.merged.c_str());
  if (!keeps_deletions(base, ours, theirs, r.merged)) return give_up("a line one side deleted would come back");
  if (!keeps_counts(base, ours, theirs, r.merged)) return give_up("a token's count would not be what the sides made it");
  // what comes out must be the language still
  Doc check;
  check.lang = lang_name;
  if (!parse(lang, r.merged, check, &o)) {
    if (std::getenv("AIOS_STRUCTURAL_DEBUG")) std::fprintf(stderr, "%s", r.merged.c_str());
    return give_up("the result does not parse");
  }
  return r;
}

namespace {

uint64_t fnv(std::string_view s, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

// What a key reads as in a scope: its name, without the kind before it or the duplicate's number after it.
std::string display_name(const std::string& key) {
  std::string k = key;
  // "wrapper/inner_kind:name": the kinds are separated by '/', and the name (a module path...) may hold more
  size_t cut = std::min(k.find(':'), k.find('='));
  size_t slash = k.rfind('/', cut == std::string::npos ? std::string::npos : cut);
  if (slash != std::string::npos) {
    k = k.substr(slash + 1);
    cut = std::min(k.find(':'), k.find('='));
  }
  std::string name = cut == std::string::npos ? k : k.substr(cut + 1);
  if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
    name = name.substr(1, name.size() - 2);
  return name;
}

struct Outliner {
  const Doc& d;
  std::vector<GitSyntaxUnit>& out;

  static bool recorded(const Unit& u) {
    if (!u.keyed || u.key.rfind("item:", 0) == 0 || u.key.rfind("text:", 0) == 0) return false;
    std::string k = u.key.substr(0, u.key.rfind('#') == std::string::npos ? u.key.size() : u.key.rfind('#'));
    return k != std::string(ts_node_type(u.node)) + ":" + u.body;   // known by its own text: part of what holds it
  }

  // The text of `n` without the units recorded under it and without whitespace: what changed in the unit itself.
  uint64_t shell(TSNode n, const std::vector<size_t>& kids) const {
    uint32_t at = ts_node_start_byte(n), end = ts_node_end_byte(n);
    uint64_t h = fnv(ts_node_type(n));
    auto take = [&](uint32_t from, uint32_t to) {
      for (uint32_t i = from; i < to; ++i) if (!std::isspace((unsigned char)d.text[i]) && d.text[i] != ',' && d.text[i] != ';') h = fnv(std::string_view(&d.text[i], 1), h);
    };
    std::vector<std::pair<uint32_t, uint32_t>> holes;
    for (size_t k : kids) holes.push_back({out[k].start, out[k].end});
    std::sort(holes.begin(), holes.end());
    for (auto& [hs, he] : holes) { take(at, std::min(hs, end)); at = std::max(at, he); }
    take(at, end);
    return h;
  }

  void inside(TSNode n, const std::string& scope, int depth, std::vector<size_t>& direct) {
    if (depth > 12) return;
    if (is_list(n) && depth > 0) { walk(n, scope, depth + 1, direct); return; }
    TSNode body = body_of(n);
    if (!ts_node_is_null(body)) walk(body, scope, depth + 1, direct);
  }

  void walk(TSNode list, const std::string& scope, int depth, std::vector<size_t>& direct) {
    if (depth > 12) return;
    Level L = split(d, list);
    if (L.units.size() == 1 && !recorded(L.units[0])) {   // a document's one value, a wrapper's one child
      inside(L.units[0].node, scope, depth + 1, direct);
      return;
    }
    for (auto& u : L.units) {
      if (!recorded(u)) continue;
      size_t idx = out.size();
      GitSyntaxUnit g;
      g.kind = ts_node_type(u.node);
      g.name = display_name(u.key);
      size_t hash = u.key.rfind('#');
      if (hash != std::string::npos && u.key.find_first_not_of("0123456789", hash + 1) == std::string::npos)
        g.name += u.key.substr(hash);
      g.scope = scope.empty() ? g.name : scope + "." + g.name;
      g.start = ts_node_start_byte(u.node);
      g.end = ts_node_end_byte(u.node);
      g.line = ts_node_start_point(u.node).row + 1;
      g.end_line = ts_node_end_point(u.node).row + 1;
      g.depth = depth;
      g.digest = fnv(u.body);
      out.push_back(std::move(g));
      direct.push_back(idx);
      std::vector<size_t> mine;
      const std::string mine_scope = out[idx].scope;   // `out` grows under it
      inside(u.node, mine_scope, depth, mine);
      out[idx].shell = shell(u.node, mine);
    }
  }
};

std::optional<GitSyntaxOutline> outline_of(std::string_view path, std::string_view text, const Doc* from, Doc* keep,
                                           bool lenient = false) {
  std::string lang_name = git_structural_language(path);
  const TSLanguage* lang = language_named(lang_name);
  if (!lang || text.size() > kMaxBytes || text.find('\0') != std::string_view::npos) return std::nullopt;
  Doc local;
  Doc& d = keep ? *keep : local;
  d.lang = lang_name;
  // lenient: a tree with local errors still outlines (what it misread only makes those units coarser)
  if (!parse(lang, text, d, from) && !(lenient && d.tree)) return std::nullopt;
  GitSyntaxOutline o;
  o.language = lang_name;
  Outliner w{d, o.units};
  std::vector<size_t> top;
  TSNode root = ts_tree_root_node(d.tree.get());
  w.walk(root, "", 0, top);
  // the file's own text, outside every top-level unit (what precedes and follows the root included)
  std::vector<std::pair<uint32_t, uint32_t>> holes;
  for (size_t k : top) holes.push_back({o.units[k].start, o.units[k].end});
  std::sort(holes.begin(), holes.end());
  uint64_t h = fnv("");
  uint32_t at = 0;
  auto take = [&](uint32_t a0, uint32_t a1) {
    for (uint32_t i = a0; i < a1; ++i) if (!std::isspace((unsigned char)d.text[i]) && d.text[i] != ',' && d.text[i] != ';') h = fnv(std::string_view(&d.text[i], 1), h);
  };
  for (auto& [hs, he] : holes) { take(at, hs); at = std::max(at, he); }
  take(at, (uint32_t)d.text.size());
  o.shell = h;
  return o;
}

}  // namespace

std::optional<GitSyntaxOutline> git_structural_outline_lenient(std::string_view path, std::string_view text,
                                                               std::string_view previous) {
  Doc prev;
  prev.lang = git_structural_language(path);
  const TSLanguage* lang = language_named(prev.lang);
  const bool have_prev = lang && !previous.empty() && previous != text && previous.size() <= kMaxBytes &&
                         (parse(lang, previous, prev) || prev.tree);
  return outline_of(path, text, have_prev ? &prev : nullptr, nullptr, true);
}

std::optional<GitSyntaxOutline> git_structural_outline(std::string_view path, std::string_view text,
                                                       std::string_view previous) {
  if (previous.empty() || previous == text) return outline_of(path, text, nullptr, nullptr);
  Doc prev;
  prev.lang = git_structural_language(path);
  const TSLanguage* lang = language_named(prev.lang);
  if (!lang || previous.size() > kMaxBytes || !parse(lang, previous, prev)) return outline_of(path, text, nullptr, nullptr);
  return outline_of(path, text, &prev, nullptr);
}

namespace {
void put_v(std::string& b, uint64_t v) {
  while (v >= 0x80) { b.push_back((char)(v | 0x80)); v >>= 7; }
  b.push_back((char)v);
}
bool get_v(std::string_view b, size_t& at, uint64_t& v) {
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (at >= b.size()) return false;
    uint8_t c = (uint8_t)b[at++];
    v |= (uint64_t)(c & 0x7f) << shift;
    if (!(c & 0x80)) return true;
  }
  return false;
}
void put_s(std::string& b, const std::string& s) { put_v(b, s.size()); b += s; }
bool get_s(std::string_view b, size_t& at, std::string& s) {
  uint64_t n;
  if (!get_v(b, at, n) || at + n > b.size()) return false;
  s.assign(b.substr(at, n));
  at += n;
  return true;
}
void put_64(std::string& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((char)(v >> (8 * i))); }
bool get_64(std::string_view b, size_t& at, uint64_t& v) {
  if (at + 8 > b.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t)(uint8_t)b[at + i] << (8 * i);
  at += 8;
  return true;
}
}  // namespace

std::string git_syntax_outline_encode(const GitSyntaxOutline& o) {
  std::string b;
  put_s(b, o.language);
  put_64(b, o.shell);
  put_v(b, o.units.size());
  for (auto& u : o.units) {
    put_s(b, u.scope);
    put_s(b, u.kind);
    put_s(b, u.name);
    put_v(b, u.start);
    put_v(b, u.end - u.start);
    put_v(b, u.line);
    put_v(b, u.end_line - u.line);
    put_v(b, (uint64_t)u.depth);
    put_64(b, u.digest);
    put_64(b, u.shell);
  }
  return b;
}

std::optional<GitSyntaxOutline> git_syntax_outline_decode(std::string_view b) {
  GitSyntaxOutline o;
  size_t at = 0;
  uint64_t n;
  if (!get_s(b, at, o.language) || !get_64(b, at, o.shell) || !get_v(b, at, n) || n > b.size()) return std::nullopt;
  o.units.resize(n);
  for (auto& u : o.units) {
    uint64_t start, len, line, lines, depth;
    if (!get_s(b, at, u.scope) || !get_s(b, at, u.kind) || !get_s(b, at, u.name) || !get_v(b, at, start) ||
        !get_v(b, at, len) || !get_v(b, at, line) || !get_v(b, at, lines) || !get_v(b, at, depth) ||
        !get_64(b, at, u.digest) || !get_64(b, at, u.shell))
      return std::nullopt;
    u.start = (uint32_t)start;
    u.end = (uint32_t)(start + len);
    u.line = (uint32_t)line;
    u.end_line = (uint32_t)(line + lines);
    u.depth = (int)depth;
  }
  if (at != b.size()) return std::nullopt;
  return o;
}

std::vector<GitScopeChange> git_structural_scope_changes(const GitSyntaxOutline& a, const GitSyntaxOutline& b) {
  std::map<std::string, const GitSyntaxUnit*> ia, ib;
  for (auto& u : a.units) ia.emplace(u.scope, &u);
  for (auto& u : b.units) ib.emplace(u.scope, &u);
  std::vector<GitScopeChange> out;
  if (a.shell != b.shell) out.push_back({"", '~'});
  for (auto& [scope, u] : ia) {
    auto it = ib.find(scope);
    if (it == ib.end()) out.push_back({scope, '-'});
    else if (it->second->shell != u->shell) out.push_back({scope, '~'});
  }
  for (auto& [scope, u] : ib) if (!ia.count(scope)) out.push_back({scope, '+'});
  std::sort(out.begin(), out.end(), [](const GitScopeChange& x, const GitScopeChange& y) { return x.scope < y.scope; });
  return out;
}

std::vector<std::string> git_structural_changed_scopes(const GitSyntaxOutline& a, const GitSyntaxOutline& b) {
  std::vector<std::string> out;
  for (auto& c : git_structural_scope_changes(a, b)) out.push_back(c.scope);
  return out;
}

std::optional<std::vector<std::string>> git_structural_changed_scopes(std::string_view path, std::string_view base,
                                                                      std::string_view side) {
  Doc kept;
  auto a = outline_of(path, base, nullptr, &kept);
  if (!a) return std::nullopt;
  auto b = outline_of(path, side, &kept, nullptr);
  if (!b) return std::nullopt;
  return git_structural_changed_scopes(*a, *b);
}

GitMergeAudit git_structural_audit(std::string_view path, std::string_view base, std::string_view ours,
                                   std::string_view theirs, std::string_view merged) {
  GitMergeAudit out;
  if (git_structural_language(path).empty()) return out;
  for (auto t : {base, ours, theirs, merged})
    if (!git_structural_parses(path, t)) return out;
  // Each version's outline incrementally from the base's tree: one parse plus the edits.
  Doc kept;
  auto b = outline_of(path, base, nullptr, &kept);
  if (!b) return out;
  auto o = outline_of(path, ours, &kept, nullptr);
  auto t = outline_of(path, theirs, &kept, nullptr);
  auto m = outline_of(path, merged, &kept, nullptr);
  if (!o || !t || !m) return out;
  // A unit by its scope: its own text (shell: without the units under it, which are judged on their own). Imports are
  // the file's text, not units anyone owns, and the line merge's union of two sides' imports is what is wanted.
  using Shells = std::map<std::string, uint64_t>;
  auto shells = [](const GitSyntaxOutline& x) {
    Shells s;
    for (auto& u : x.units)
      if (!git_syntax_is_import(u.kind)) s.emplace(u.scope, u.shell);
    return s;
  };
  const Shells sb = shells(*b), so = shells(*o), st = shells(*t), sm = shells(*m);
  std::set<std::string> scopes;
  for (auto* s : {&sb, &so, &st, &sm})
    for (auto& [k, v] : *s) scopes.insert(k);
  auto at = [](const Shells& s, const std::string& k) -> std::optional<uint64_t> {
    auto it = s.find(k);
    if (it == s.end()) return std::nullopt;
    return it->second;
  };
  for (auto& k : scopes) {
    const auto vb = at(sb, k), vo = at(so, k), vt = at(st, k), vm = at(sm, k);
    std::optional<uint64_t> want;
    if (vo == vb) want = vt;            // only theirs changed it (or nobody did)
    else if (vt == vb || vo == vt) want = vo;   // only ours changed it, or both made it the same
    else { out.both.push_back(k); continue; }
    if (vm == want) continue;
    out.ok = false;
    const std::string name = k;
    if (!want && vm) {
      const bool second = name.find('#') != std::string::npos;
      out.reason = second ? "the merge holds a second `" + name.substr(0, name.find('#')) +
                                "` neither side has: both sides added one (a duplicate definition)"
                          : "the merge holds `" + name + "`, which " + (vb ? "a side deleted" : "neither side has");
    } else if (want && !vm) {
      out.reason = "the merge lost `" + name + "`";
    } else {
      out.reason = "`" + name + "` came out of the merge as neither side made it (the lines merged into it)";
    }
    return out;
  }
  return out;
}

bool git_syntax_is_import(const std::string& k) {
  return k.find("import") != std::string::npos || k == "use_declaration" || k == "namespace_use_declaration" ||
         k == "preproc_include" || k == "using_directive";
}

bool git_scopes_overlap(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) return true;   // the file itself
  auto within = [](std::string_view inner, std::string_view outer) {
    return inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 && inner[outer.size()] == '.';
  };
  return a == b || within(a, b) || within(b, a);
}

std::string git_structural_sexp(std::string_view path, std::string_view text, std::string_view from) {
  std::string lang_name = git_structural_language(path);
  const TSLanguage* lang = language_named(lang_name);
  if (!lang) return "";
  Doc base, d;
  base.lang = d.lang = lang_name;
  bool have_from = !from.empty();
  if (have_from) {
    // parse `from` fresh (no cache) so the incremental parse really starts from an edited tree
    std::unique_ptr<TSParser, ParserDel> p(ts_parser_new());
    ts_parser_set_language(p.get(), lang);
    base.text.assign(from);
    base.tree.reset(ts_parser_parse_string(p.get(), nullptr, base.text.data(), (uint32_t)base.text.size()));
    std::unique_ptr<TSParser, ParserDel> q(ts_parser_new());
    ts_parser_set_language(q.get(), lang);
    TreePtr old(ts_tree_copy(base.tree.get()));
    edit_tree(old.get(), from, text);
    d.text.assign(text);
    d.tree.reset(ts_parser_parse_string(q.get(), old.get(), d.text.data(), (uint32_t)d.text.size()));
  } else {
    std::unique_ptr<TSParser, ParserDel> p(ts_parser_new());
    ts_parser_set_language(p.get(), lang);
    d.text.assign(text);
    d.tree.reset(ts_parser_parse_string(p.get(), nullptr, d.text.data(), (uint32_t)d.text.size()));
  }
  if (!d.tree) return "";
  char* sx = ts_node_string(ts_tree_root_node(d.tree.get()));
  std::string r = sx ? sx : "";
  free(sx);
  return r;
}

GitStructuralStats git_structural_stats() { return GitStructuralStats{g_full.load(), g_incremental.load(), g_cached.load()}; }

}  // namespace aios::userland
