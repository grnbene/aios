// gitattributes: parsing, precedence and macros.
#include "attrs.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "aios/hal/host.hpp"

namespace aios_git {

using namespace aios;

std::string AttrValue::shown() const {
  switch (kind) {
    case Set: return "set";
    case Unset: return "unset";
    case Text: return text;
    default: return "unspecified";
  }
}

namespace {

bool valid_attr_name(const std::string& n) {
  if (n.empty() || n[0] == '-') return false;
  for (char ch : n) if (!(ch == '-' || ch == '.' || ch == '_' || std::isalnum((unsigned char)ch))) return false;
  return true;
}

// The first word of a line, unquoted when it starts with '"'; `rest` is what follows.
bool take_pattern(const std::string& line, size_t& at, std::string& out) {
  size_t i = at;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size()) return false;
  out.clear();
  if (line[i] == '"') {
    ++i;
    while (i < line.size() && line[i] != '"') {
      if (line[i] == '\\' && i + 1 < line.size()) {
        char n = line[++i];
        if (n >= '0' && n <= '7' && i + 2 < line.size()) {
          out += (char)(((n - '0') << 6) | ((line[i + 1] - '0') << 3) | (line[i + 2] - '0'));
          i += 3;
          continue;
        }
        switch (n) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          default: out += n; break;
        }
        ++i;
        continue;
      }
      out += line[i++];
    }
    if (i >= line.size()) return false;
    at = i + 1;
    return true;
  }
  size_t s = i;
  while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
  out = line.substr(s, i - s);
  at = i;
  return true;
}

}  // namespace

void Attributes::parse_into(const std::string& text, const std::string& base, bool top, std::vector<Line>& out) {
  const std::string label = base.empty() ? ".gitattributes" : base + ".gitattributes";
  size_t pos = 0;
  int lineno = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t at = 0;
    std::string pat;
    if (!take_pattern(line, at, pat) || pat[0] == '#') continue;
    Line l;
    l.base = base;
    if (pat.rfind("[attr]", 0) == 0) {
      if (!top) continue;
      l.macro = true;
      l.macro_name = pat.substr(6);
      if (!valid_attr_name(l.macro_name)) {
        c_.err << l.macro_name << " is not a valid attribute name: " << label << ":" << lineno << "\n";
        continue;
      }
    } else {
      if (pat[0] == '!') {
        c_.err << "Negative patterns are ignored in git attributes\nUse '\\!' for literal leading exclamation.\n";
        continue;
      }
      if (pat[0] == '\\' && pat.size() > 1 && pat[1] == '!') pat = pat.substr(1);
      if (!pat.empty() && pat.back() == '/') continue;
      l.pattern = pat;
      size_t slash = pat.find('/');
      l.anchored = slash != std::string::npos;
      if (l.anchored && pat[0] == '/') l.pattern = pat.substr(1);
    }
    while (at < line.size()) {
      while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
      if (at >= line.size()) break;
      size_t s = at;
      while (at < line.size() && line[at] != ' ' && line[at] != '\t') ++at;
      std::string tok = line.substr(s, at - s);
      AttrValue v;
      std::string name = tok;
      if (tok[0] == '-') { v.kind = AttrValue::Unset; name = tok.substr(1); }
      else if (tok[0] == '!') { v.kind = AttrValue::Unspecified; name = tok.substr(1); }
      else {
        size_t eq = tok.find('=');
        if (eq != std::string::npos) { v.kind = AttrValue::Text; v.text = tok.substr(eq + 1); name = tok.substr(0, eq); }
        else v.kind = AttrValue::Set;
      }
      if (!valid_attr_name(name)) {
        c_.err << name << " is not a valid attribute name: " << label << ":" << lineno << "\n";
        continue;
      }
      l.attrs.push_back({name, v});
    }
    out.push_back(std::move(l));
  }
}

namespace {
std::vector<std::string> split_dirs(const std::string& path) {
  std::vector<std::string> dirs = {""};
  size_t pos = 0;
  while ((pos = path.find('/', pos)) != std::string::npos) { dirs.push_back(path.substr(0, pos + 1)); ++pos; }
  return dirs;
}
}  // namespace

const std::vector<Attributes::Line>& Attributes::file_lines(const std::string& dir) {
  auto it = by_dir_.find(dir);
  if (it != by_dir_.end()) return it->second;
  std::vector<Line> lines;
  std::string text;
  bool have = false;
  const std::string rel = dir + ".gitattributes";
  if (!source_tree_.empty()) {
    std::string oid = tree_blob(source_tree_, rel);
    if (!oid.empty()) { text = blob_text(c_, oid); have = true; }
  } else {
    if (!cached_ && !c_.top.empty()) {
      auto t = c_.host().read_file(c_.top + "/" + rel);
      if (t.ok()) { text.assign((const char*)t->data(), t->size()); have = true; }
    }
    if (!have) {
      for (auto& e : c_.repo->index()) if (e.path == rel) { text = blob_text(c_, e.oid); have = true; break; }
    }
  }
  if (have) parse_into(text, dir, dir.empty(), lines);
  for (auto& l : lines) if (l.macro) { macros_[l.macro_name] = l.attrs; note_names(l); }
  else note_names(l);
  return by_dir_.emplace(dir, std::move(lines)).first->second;
}

void Attributes::note_names(const Line& l) {
  if (l.macro) note_name(l.macro_name);
  for (auto& [n, v] : l.attrs) { (void)v; note_name(n); }
}

void Attributes::note_name(const std::string& n) {
  if (registry_.insert(n).second) order_.push_back(n);
}

std::string Attributes::tree_blob(const std::string& tree, const std::string& path) {
  std::string cur = tree;
  size_t pos = 0;
  while (true) {
    size_t slash = path.find('/', pos);
    std::string name = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    auto ents = c_.repo->read_tree(cur);
    if (!ents.ok()) return "";
    const GitTreeEntry* hit = nullptr;
    for (auto& e : *ents) if (e.name == name) { hit = &e; break; }
    if (!hit) return "";
    if (slash == std::string::npos) return hit->is_dir() ? "" : hit->oid;
    if (!hit->is_dir()) return "";
    cur = hit->oid;
    pos = slash + 1;
  }
}

void Attributes::ensure_loaded() {
  if (loaded_) return;
  loaded_ = true;
  // git defines `binary` itself before it reads anything
  for (const char* n : {"binary", "diff", "merge", "text"}) note_name(n);
  macros_["binary"] = {{"diff", {AttrValue::Unset, ""}}, {"merge", {AttrValue::Unset, ""}}, {"text", {AttrValue::Unset, ""}}};
  std::string gpath;
  if (auto f = c_.config.get("core.attributesfile")) {
    gpath = *f;
    if (!gpath.empty() && gpath[0] == '~' && c_.hasenv("HOME")) gpath = c_.getenv("HOME") + gpath.substr(1);
  } else if (c_.hasenv("XDG_CONFIG_HOME") && !c_.getenv("XDG_CONFIG_HOME").empty()) {
    gpath = c_.getenv("XDG_CONFIG_HOME") + "/git/attributes";
  } else if (c_.hasenv("HOME")) {
    gpath = c_.getenv("HOME") + "/.config/git/attributes";
  }
  if (!gpath.empty()) {
    auto t = c_.host().read_file(c_.abs_path(gpath));
    if (t.ok()) {
      parse_into(std::string((const char*)t->data(), t->size()), "", true, global_);
      for (auto& l : global_) { if (l.macro) macros_[l.macro_name] = l.attrs; note_names(l); }
    }
  }
  file_lines("");
  auto t = c_.host().read_file(c_.repo->common_dir() + "/info/attributes");
  if (t.ok()) {
    parse_into(std::string((const char*)t->data(), t->size()), "", true, info_);
    for (auto& l : info_) { if (l.macro) macros_[l.macro_name] = l.attrs; note_names(l); }
  }
}

bool Attributes::line_matches(const Line& l, const std::string& path) const {
  if (l.macro) return false;
  std::string rel = path;
  if (!l.base.empty()) {
    if (path.compare(0, l.base.size(), l.base) != 0) return false;
    rel = path.substr(l.base.size());
  }
  const bool icase = c_.repo->settings().ignorecase;
  if (l.anchored) return wildmatch(l.pattern, rel, icase, true);
  size_t slash = rel.rfind('/');
  std::string base = slash == std::string::npos ? rel : rel.substr(slash + 1);
  return wildmatch(l.pattern, base, icase, false);
}

void Attributes::fill_macro(const std::string& name, std::map<std::string, AttrValue>& st, int depth) {
  auto m = macros_.find(name);
  if (m == macros_.end() || depth > 20) return;
  fill_list(m->second, st, depth + 1);
}

void Attributes::fill_list(const std::vector<std::pair<std::string, AttrValue>>& list, std::map<std::string, AttrValue>& st, int depth) {
  for (size_t i = list.size(); i-- > 0;) {
    const auto& [n, v] = list[i];
    if (st.count(n)) continue;
    st[n] = v;
    if (v.kind == AttrValue::Set) fill_macro(n, st, depth);
  }
}

std::map<std::string, AttrValue> Attributes::resolve(const std::string& path) {
  ensure_loaded();
  std::vector<std::string> dirs = split_dirs(path);
  for (auto& d : dirs) file_lines(d);
  std::map<std::string, AttrValue> st;
  auto scan = [&](const std::vector<Line>& lines) {
    for (size_t i = lines.size(); i-- > 0;)
      if (line_matches(lines[i], path)) fill_list(lines[i].attrs, st, 0);
  };
  scan(info_);
  for (size_t d = dirs.size(); d-- > 0;) scan(by_dir_[dirs[d]]);
  scan(global_);
  return st;
}

AttrValue Attributes::get(const std::string& path, const std::string& name) {
  auto st = resolve(path);
  auto it = st.find(name);
  return it == st.end() ? AttrValue{} : it->second;
}

std::vector<std::pair<std::string, AttrValue>> Attributes::all(const std::string& path) {
  auto st = resolve(path);
  std::vector<std::pair<std::string, AttrValue>> out;
  for (auto& n : order_) {
    auto it = st.find(n);
    if (it == st.end() || it->second.kind == AttrValue::Unspecified) continue;
    out.push_back({n, it->second});
  }
  return out;
}

}  // namespace aios_git
