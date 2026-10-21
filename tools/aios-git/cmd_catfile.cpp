// cat-file: object contents, types and sizes, one at a time or streamed (--batch, --batch-check, --batch-command).
#include <algorithm>
#include <cctype>

#include "aios/hal/host.hpp"
#include "attrs.hpp"
#include "hist_util.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const char* kCatFileUsage =
    "usage: git cat-file <type> <object>\n"
    "   or: git cat-file (-e | -p | -t | -s) <object>\n"
    "   or: git cat-file (--textconv | --filters)\n"
    "                    [<rev>:<path|tree-ish> | --path=<path|tree-ish> <rev>]\n"
    "   or: git cat-file (--batch | --batch-check | --batch-command) [--batch-all-objects]\n"
    "                    [--buffer] [--follow-symlinks] [--unordered]\n"
    "                    [--textconv | --filters] [-Z]\n"
    "\n"
    "Check object existence or emit object contents\n"
    "    -e                    check if <object> exists\n"
    "    -p                    pretty-print <object> content\n"
    "\n"
    "Emit [broken] object attributes\n"
    "    -t                    show object type (one of 'blob', 'tree', 'commit', 'tag', ...)\n"
    "    -s                    show object size\n"
    "    --[no-]use-mailmap    use mail map file\n"
    "    --[no-]mailmap ...    alias of --use-mailmap\n"
    "\n"
    "Batch objects requested on stdin (or --batch-all-objects)\n"
    "    --batch[=<format>]    show full <object> or <rev> contents\n"
    "    --batch-check[=<format>]\n"
    "                          like --batch, but don't emit <contents>\n"
    "    -Z                    stdin and stdout is NUL-terminated\n"
    "    --batch-command[=<format>]\n"
    "                          read commands from stdin\n"
    "    --batch-all-objects   with --batch[-check]: ignores stdin, batches all known objects\n"
    "\n"
    "Change or optimize batch output\n"
    "    --[no-]buffer         buffer --batch output\n"
    "    --[no-]follow-symlinks\n"
    "                          follow in-tree symlinks\n"
    "    --[no-]unordered      do not order objects before emitting them\n"
    "\n"
    "Emit object (blob or tree) with conversion or filter (stand-alone, or with batch)\n"
    "    --textconv            run textconv on object's content\n"
    "    --filters             run filters on object's content\n"
    "    --[no-]path blob|tree use a <path> for (--textconv | --filters); Not with 'batch'\n"
    "    --[no-]filter <args>  object filtering\n"
    "\n";

const char* kCatFileHelp =
    "usage: git cat-file <type> <object>\n"
    "   or: git cat-file (-e | -p | -t | -s) <object>\n"
    "   or: git cat-file (--textconv | --filters)\n"
    "                    [<rev>:<path|tree-ish> | --path=<path|tree-ish> <rev>]\n"
    "   or: git cat-file (--batch | --batch-check | --batch-command) [--batch-all-objects]\n"
    "                    [--buffer] [--follow-symlinks] [--unordered]\n"
    "                    [--textconv | --filters] [-Z]\n"
    "\n"
    "Check object existence or emit object contents\n"
    "    -e                    check if <object> exists\n"
    "    -p                    pretty-print <object> content\n"
    "\n"
    "Emit [broken] object attributes\n"
    "    -t                    show object type (one of 'blob', 'tree', 'commit', 'tag', ...)\n"
    "    -s                    show object size\n"
    "    --[no-]use-mailmap    use mail map file\n"
    "    --[no-]mailmap        alias of --use-mailmap\n"
    "\n"
    "Batch objects requested on stdin (or --batch-all-objects)\n"
    "    --batch[=<format>]    show full <object> or <rev> contents\n"
    "    --batch-check[=<format>]\n"
    "                          like --batch, but don't emit <contents>\n"
    "    -Z                    stdin and stdout is NUL-terminated\n"
    "    --batch-command[=<format>]\n"
    "                          read commands from stdin\n"
    "    --batch-all-objects   with --batch[-check]: ignores stdin, batches all known objects\n"
    "\n"
    "Change or optimize batch output\n"
    "    --[no-]buffer         buffer --batch output\n"
    "    --[no-]follow-symlinks\n"
    "                          follow in-tree symlinks\n"
    "    --[no-]unordered      do not order objects before emitting them\n"
    "\n"
    "Emit object (blob or tree) with conversion or filter (stand-alone, or with batch)\n"
    "    --textconv            run textconv on object's content\n"
    "    --filters             run filters on object's content\n"
    "    --[no-]path blob|tree use a <path> for (--textconv | --filters); Not with 'batch'\n"
    "    --[no-]filter <args>  object filtering\n"
    "\n";

[[noreturn]] void usage_msg(Ctx& c, const std::string& msg) {
  c.err << "fatal: " << msg << "\n\n" << kCatFileUsage;
  throw Exit{129};
}

const char* type_of_mode(uint32_t m) { return m == 040000 ? "tree" : m == 0160000 ? "commit" : "blob"; }

std::string mode_text(uint32_t m) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06o", m);
  return buf;
}

struct Obj {
  std::string oid, type;
  ByteBuffer payload;
  uint64_t size = 0;   // the payload's, also when only the header was read (info_only)
  bool info_only = false;
};

class CatFile {
 public:
  explicit CatFile(Ctx& c) : c_(c) {}
  int run();

 private:
  Ctx& c_;
  char opt_ = 0;                       // 't' 's' 'e' 'p', 'c' textconv, 'w' filters
  bool batch_ = false, batch_check_ = false, batch_command_ = false;
  std::string format_;
  bool has_format_ = false;
  bool buffer_set_ = false;   // --buffer / --no-buffer given; git buffers --batch-all-objects by default
  bool all_objects_ = false, buffer_ = false, follow_ = false, unordered_ = false, use_mailmap_ = false;
  char in_delim_ = '\n', out_delim_ = '\n';
  std::string path_arg_;
  bool have_path_ = false;
  char transform_ = 0;
  bool want_rest_ = false;
  Mailmap mailmap_;
  PackSet packs_;
  bool packs_loaded_ = false;
  std::unique_ptr<Attributes> attrs_;

  bool fetch(const std::string& oid, Obj& out);
  // Type and size only, from the object's headers, when nothing else of it is printed.
  bool fetch_info(const std::string& oid, Obj& out);
  bool header_only() const;
  std::string remap(const Obj& o);
  std::string expand(const std::string& fmt, const Obj& o, const std::string& rest);
  void check_format(const std::string& fmt);
  uint64_t disk_size(const std::string& oid, std::string* base);
  std::string transformed(const Obj& o, const std::string& path);
  void batch_one(const std::string& name, const std::string& rest, bool contents);
  void write_object(const std::string& name, const Obj& o, const std::string& rest, bool contents, const std::string& path);
  int run_batch();
  int run_single(const std::string& exp_type, const std::string& name);
  void emit(const std::string& s) { c_.out.write(s.data(), (std::streamsize)s.size()); }
  void done_one() { if (!buffer_) c_.out.flush(); }
  bool split_path(const std::string& name, std::string& path);
  enum class Follow { Found, Missing, Dangling, Loop, NotDir, Outside };
  Follow follow(const std::string& rev, const std::string& path, std::string& oid, std::string& outside);
};

bool CatFile::fetch(const std::string& oid, Obj& out) {
  auto r = c_.repo->read_object(oid);
  if (!r.ok()) {
    if (oid == empty_tree_oid()) { out = Obj{oid, "tree", {}}; return true; }
    return false;
  }
  out.oid = r->oid;
  out.type = r->type;
  out.payload = std::move(r->payload);
  out.size = out.payload.size();
  return true;
}

bool CatFile::header_only() const {
  if (use_mailmap_) return false;
  for (size_t i = 0; i + 1 < format_.size(); ++i) {
    if (format_[i] != '%') continue;
    if (format_[i + 1] == '%') { ++i; continue; }
    if (format_[i + 1] != '(') continue;
    const size_t e = format_.find(')', i);
    if (e == std::string::npos) return false;
    const std::string atom = format_.substr(i + 2, e - i - 2);
    if (atom != "objectname" && atom != "objecttype" && atom != "objectsize" && atom != "objectsize:disk" &&
        atom != "deltabase" && atom != "rest")
      return false;
    i = e;
  }
  return true;
}

bool CatFile::fetch_info(const std::string& oid, Obj& out) {
  auto r = c_.repo->object_info(oid);
  if (!r.ok()) return fetch(oid, out);
  out.oid = oid;
  out.type = r->first;
  out.size = r->second;
  out.payload.clear();
  out.info_only = true;
  return true;
}

// A commit's or tag's header with the idents the mailmap rewrites.
std::string CatFile::remap(const Obj& o) {
  std::string body((const char*)o.payload.data(), o.payload.size());
  if (!use_mailmap_ || (o.type != "commit" && o.type != "tag")) return body;
  size_t end = body.find("\n\n");
  std::string head = end == std::string::npos ? body : body.substr(0, end + 1);
  std::string tail = end == std::string::npos ? "" : body.substr(end + 1);
  std::string out;
  size_t pos = 0;
  while (pos < head.size()) {
    size_t nl = head.find('\n', pos);
    std::string line = head.substr(pos, nl - pos);
    pos = nl + 1;
    const char* keys[] = {"author ", "committer ", "tagger "};
    for (auto k : keys) {
      size_t kl = std::strlen(k);
      if (line.compare(0, kl, k) != 0) continue;
      size_t lt = line.find('<', kl), gt = line.find('>', kl);
      if (lt == std::string::npos || gt == std::string::npos || gt < lt) break;
      std::string name = line.substr(kl, lt - kl), email = line.substr(lt + 1, gt - lt - 1);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      mailmap_.map(name, email);
      line = std::string(k) + name + " <" + email + ">" + line.substr(gt + 1);
      break;
    }
    out += line + "\n";
  }
  return out + tail;
}

uint64_t CatFile::disk_size(const std::string& oid, std::string* base) {
  if (base) base->assign(40, '0');
  std::string loose = c_.repo->common_dir() + "/objects/" + oid.substr(0, 2) + "/" + oid.substr(2);
  auto st = c_.host().stat(loose);
  if (st.ok() && !st->is_dir) return st->size;
  for (auto& alt : c_.repo->alternate_object_dirs()) {
    auto s2 = c_.host().stat(alt + "/" + oid.substr(0, 2) + "/" + oid.substr(2));
    if (s2.ok() && !s2->is_dir) return s2->size;
  }
  if (!packs_loaded_) { packs_.load(c_); packs_loaded_ = true; }
  PackedInfo pi;
  if (packs_.find(oid, pi)) {
    if (base && !pi.base.empty()) *base = pi.base;
    return pi.disk;
  }
  return 0;
}

void CatFile::check_format(const std::string& fmt) {
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') continue;
    if (i + 1 < fmt.size() && fmt[i + 1] == '%') { ++i; continue; }
    if (i + 1 >= fmt.size() || fmt[i + 1] != '(') continue;
    size_t e = fmt.find(')', i + 1);
    if (e == std::string::npos) die(c_, "bad cat-file format: element '" + fmt.substr(i + 1) + "' does not end in ')'");
    std::string atom = fmt.substr(i + 2, e - i - 2);
    if (atom == "rest") want_rest_ = true;
    else if (atom != "objectname" && atom != "objecttype" && atom != "objectsize" && atom != "objectsize:disk" && atom != "deltabase")
      die(c_, "bad cat-file format: " + fmt);
    i = e;
  }
}

std::string CatFile::expand(const std::string& fmt, const Obj& o, const std::string& rest) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') { out += fmt[i]; continue; }
    if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out += '%'; ++i; continue; }
    if (i + 1 >= fmt.size() || fmt[i + 1] != '(') { out += '%'; continue; }
    size_t e = fmt.find(')', i + 1);
    std::string atom = fmt.substr(i + 2, e - i - 2);
    i = e;
    if (atom == "objectname") out += o.oid;
    else if (atom == "objecttype") out += o.type;
    else if (atom == "objectsize") out += std::to_string(use_mailmap_ ? remap(o).size() : o.info_only ? o.size : o.payload.size());
    else if (atom == "objectsize:disk") out += std::to_string(disk_size(o.oid, nullptr));
    else if (atom == "deltabase") { std::string b; disk_size(o.oid, &b); out += b; }
    else if (atom == "rest") out += rest;
  }
  return out;
}

bool CatFile::split_path(const std::string& name, std::string& path) {
  size_t colon = name.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  path = name.substr(colon + 1);
  while (path.rfind("./", 0) == 0) path = path.substr(2);
  return true;
}

// The blob as the work tree would hold it (--filters) or as a diff would show it (--textconv).
std::string CatFile::transformed(const Obj& o, const std::string& path) {
  std::string body((const char*)o.payload.data(), o.payload.size());
  if (o.type != "blob") return body;
  if (!attrs_) attrs_ = std::make_unique<Attributes>(c_, false);
  if (transform_ == 'c') {
    AttrValue d = attrs_->get(path, "diff");
    if (d.kind == AttrValue::Text && c_.config.get("diff." + d.text + ".textconv")) unsupported(c_, "cat-file --textconv through a configured driver");
    return body;
  }
  AttrValue filter = attrs_->get(path, "filter");
  if (filter.kind == AttrValue::Text &&
      (c_.config.get("filter." + filter.text + ".smudge") || c_.config.get("filter." + filter.text + ".process")))
    unsupported(c_, "cat-file --filters through a configured filter driver");
  if (attrs_->get(path, "working-tree-encoding").kind == AttrValue::Text) unsupported(c_, "cat-file --filters with working-tree-encoding");
  if (attrs_->get(path, "ident").kind == AttrValue::Set) {
    std::string out;
    size_t pos = 0;
    while (true) {
      size_t at = body.find("$Id", pos);
      if (at == std::string::npos) { out += body.substr(pos); break; }
      size_t end = at + 3;
      if (end < body.size() && body[end] == '$') { out += body.substr(pos, at - pos) + "$Id: " + o.oid + " $"; pos = end + 1; continue; }
      if (end < body.size() && body[end] == ':') {
        size_t close = body.find_first_of("$\n", end);
        if (close != std::string::npos && body[close] == '$') { out += body.substr(pos, at - pos) + "$Id: " + o.oid + " $"; pos = close + 1; continue; }
      }
      out += body.substr(pos, end - pos);
      pos = end;
    }
    body = out;
  }
  // end-of-line conversion on the way out
  AttrValue text = attrs_->get(path, "text"), eol = attrs_->get(path, "eol"), crlf = attrs_->get(path, "crlf");
  enum Act { Undefined, Binary, TextAct, Auto, TextCrlf, TextInput, AutoCrlf, AutoInput } act = Undefined;
  auto from = [&](const AttrValue& v) {
    if (v.kind == AttrValue::Set) return TextAct;
    if (v.kind == AttrValue::Unset) return Binary;
    if (v.kind == AttrValue::Text && v.text == "input") return TextInput;
    if (v.kind == AttrValue::Text && v.text == "auto") return Auto;
    return Undefined;
  };
  act = from(text);
  if (act == Undefined) act = from(crlf);
  if (act != Binary) {
    bool crlf_eol = eol.kind == AttrValue::Text && eol.text == "crlf", lf_eol = eol.kind == AttrValue::Text && eol.text == "lf";
    if (act == Auto && lf_eol) act = AutoInput;
    else if (act == Auto && crlf_eol) act = AutoCrlf;
    else if (lf_eol) act = TextInput;
    else if (crlf_eol) act = TextCrlf;
  }
  std::string ac = c_.config.get("core.autocrlf").value_or("");
  for (auto& ch : ac) ch = (char)std::tolower((unsigned char)ch);
  bool auto_true = ac == "true" || ac == "yes" || ac == "on" || ac == "1", auto_input = ac == "input";
  if (act == Undefined) act = auto_true ? AutoCrlf : auto_input ? AutoInput : Binary;
  std::string core_eol = c_.config.get("core.eol").value_or("");
  bool text_is_crlf = auto_true || (!auto_input && core_eol == "crlf");
  bool to_crlf;
  switch (act) {
    case Binary: to_crlf = false; break;
    case TextCrlf: case AutoCrlf: to_crlf = true; break;
    case TextInput: case AutoInput: to_crlf = false; break;
    default: to_crlf = text_is_crlf; break;
  }
  if (!to_crlf || body.empty()) return body;
  size_t lonelf = 0, lonecr = 0, crlfs = 0, nul = 0, printable = 0, nonprintable = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    unsigned char ch = (unsigned char)body[i];
    if (ch == '\r') { if (i + 1 < body.size() && body[i + 1] == '\n') { ++crlfs; ++i; } else ++lonecr; continue; }
    if (ch == '\n') { ++lonelf; continue; }
    if (ch == 127) { ++nonprintable; continue; }
    if (ch < 32) {
      if (ch == '\b' || ch == '\t' || ch == 033 || ch == 014) ++printable;
      else { if (ch == 0) ++nul; ++nonprintable; }
    } else ++printable;
  }
  if (!body.empty() && (unsigned char)body.back() == 032 && nonprintable) --nonprintable;
  if (!lonelf) return body;
  if (act == Auto || act == AutoInput || act == AutoCrlf) {
    if (lonecr || crlfs) return body;
    if (lonecr || nul || (printable >> 7) < nonprintable) return body;
  }
  std::string out;
  out.reserve(body.size() + lonelf);
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\n' && (i == 0 || body[i - 1] != '\r')) out += '\r';
    out += body[i];
  }
  return out;
}

void CatFile::write_object(const std::string& name, const Obj& o, const std::string& rest, bool contents, const std::string& path) {
  (void)name;
  std::string head = expand(format_, o, rest);
  head += out_delim_;
  emit(head);
  if (contents) {
    if (transform_) {
      if (o.type == "blob" && path.empty()) die(c_, "missing path for '" + o.oid + "'");
      emit(transformed(o, path));
    } else if (use_mailmap_ && (o.type == "commit" || o.type == "tag")) {
      emit(remap(o));
    } else {
      c_.out.write((const char*)o.payload.data(), (std::streamsize)o.payload.size());
    }
    emit(std::string(1, out_delim_));
  }
  done_one();
}

// The path inside a tree with in-tree symbolic links followed, as get_tree_entry_follow_symlinks does it.
CatFile::Follow CatFile::follow(const std::string& rev, const std::string& path_in, std::string& oid, std::string& outside) {
  auto base = resolve(c_, rev);
  if (!base) return Follow::Missing;
  std::string root = peel_to(c_, *base, "tree");
  if (root.empty()) return Follow::Missing;
  std::vector<std::string> dirs = {root};
  std::string name = path_in;
  if (name.rfind("./", 0) == 0 || name.rfind("../", 0) == 0) name = c_.to_repo_path(name);
  int follows = 40;
  bool followed = false;
  for (;;) {
    size_t slash = name.find('/');
    std::string comp = name.substr(0, slash);
    bool has_rest = slash != std::string::npos;
    std::string rest = has_rest ? name.substr(slash + 1) : "";
    if (comp == "..") {
      if (dirs.size() == 1) { outside = name; return Follow::Outside; }
      dirs.pop_back();
      if (!has_rest) { oid = dirs.back(); return Follow::Found; }
      name = rest;
      continue;
    }
    if (comp.empty() && !has_rest) { oid = dirs.back(); return Follow::Found; }
    auto ents = c_.repo->read_tree(dirs.back());
    const GitTreeEntry* hit = nullptr;
    if (ents.ok()) for (auto& e : *ents) if (e.name == comp) { hit = &e; break; }
    if (!hit) return followed ? Follow::Dangling : Follow::Missing;
    if (hit->is_dir()) {
      if (!has_rest) { oid = hit->oid; return Follow::Found; }
      dirs.push_back(hit->oid);
      name = rest;
      continue;
    }
    if (hit->mode != 0120000) {
      if (!has_rest) { oid = hit->oid; return Follow::Found; }
      return Follow::NotDir;
    }
    if (follows-- == 0) return Follow::Loop;
    followed = true;
    Obj link;
    if (!fetch(hit->oid, link)) return Follow::Missing;
    std::string target((const char*)link.payload.data(), link.payload.size());
    if (!target.empty() && target[0] == '/') { outside = target; return Follow::Outside; }
    if (has_rest) while (target.size() > 1 && target.back() == '/') target.pop_back();
    name = has_rest ? target + "/" + rest : target;
    while (name.rfind("../", 0) == 0) {
      if (dirs.size() == 1) { outside = name; return Follow::Outside; }
      dirs.pop_back();
      name = name.substr(3);
    }
  }
}

void CatFile::batch_one(const std::string& name_in, const std::string& rest_in, bool contents) {
  std::string name = name_in, rest = rest_in;
  std::string why;
  std::optional<std::string> oid;
  std::string path;
  size_t colon = name.find(':');
  if (follow_ && colon != std::string::npos && colon > 0) {
    std::string found, outside;
    Follow f = follow(name.substr(0, colon), name.substr(colon + 1), found, outside);
    const std::string d(1, out_delim_);
    if (f == Follow::Outside) {
      emit("symlink " + std::to_string(outside.size()) + d + outside + d);
      done_one();
      return;
    }
    if (f == Follow::Dangling || f == Follow::Loop || f == Follow::NotDir) {
      const char* word = f == Follow::Dangling ? "dangling " : f == Follow::Loop ? "loop " : "notdir ";
      emit(word + std::to_string(name.size()) + d + name + d);
      done_one();
      return;
    }
    if (f == Follow::Found) oid = found;
  } else {
    oid = resolve(c_, name, &why);
  }
  if (!oid) {
    if (why.find("is ambiguous") != std::string::npos && why.rfind("short object ID", 0) == 0) emit(name + " ambiguous" + std::string(1, out_delim_));
    else emit(name + " missing" + std::string(1, out_delim_));
    done_one();
    return;
  }
  Obj o;
  if (!(!contents && header_only() ? fetch_info(*oid, o) : fetch(*oid, o))) {
    emit(name + " missing" + std::string(1, out_delim_));
    done_one();
    return;
  }
  path = rest;
  write_object(name, o, rest, contents, path);
}

int CatFile::run_batch() {
  if (all_objects_) {
    if (!buffer_set_) buffer_ = true;
    std::set<std::string> ids;
    for (auto& i : c_.repo->local_loose_ids()) ids.insert(i);
    for (auto& i : c_.repo->local_packed_ids()) ids.insert(i);
    for (auto& i : c_.repo->local_block_ids()) ids.insert(i);
    const bool contents = batch_ && !batch_check_ && !batch_command_;
    const bool info = !contents && header_only();
    for (auto& oid : ids) {
      Obj o;
      if (!(info ? fetch_info(oid, o) : fetch(oid, o))) { emit(oid + " missing" + std::string(1, out_delim_)); done_one(); continue; }
      write_object(oid, o, "", contents, "");
    }
    c_.out.flush();
    return 0;
  }
  std::string line;
  for (;;) {
    if (!buffer_) c_.out.flush();
    if (!std::getline(c_.in, line, in_delim_)) break;
    if (in_delim_ == '\n' && !line.empty() && line.back() == '\r') line.pop_back();
    if (batch_command_) {
      size_t sp = line.find(' ');
      std::string cmd = line.substr(0, sp);
      bool has_arg = sp != std::string::npos;
      std::string arg = has_arg ? line.substr(sp + 1) : "";
      if (cmd != "contents" && cmd != "info" && cmd != "flush") die(c_, "unknown command: '" + line + "'");
      if (cmd == "flush") {
        if (!buffer_) die(c_, "flush is only for --buffer mode");
        if (has_arg) die(c_, "flush takes no arguments");
        c_.out.flush();
        continue;
      }
      if (!has_arg) die(c_, cmd + " requires arguments");
      std::string rest;
      std::string name = arg;
      if (want_rest_) {
        size_t w = arg.find_first_of(" \t\n\v\f\r");
        if (w != std::string::npos) { name = arg.substr(0, w); rest = arg.substr(w + 1); }
      }
      batch_one(name, rest, cmd == "contents");
      continue;
    }
    std::string name = line, rest;
    if (want_rest_) {
      size_t w = line.find_first_of(" \t\n\v\f\r");
      if (w != std::string::npos) { name = line.substr(0, w); rest = line.substr(w + 1); }
    }
    batch_one(name, rest, !batch_check_);
  }
  c_.out.flush();
  return 0;
}

int CatFile::run_single(const std::string& exp_type, const std::string& name) {
  std::string why;
  auto oid = resolve(c_, name, &why);
  if (!oid) {
    size_t colon = name.find(':');
    if (colon != std::string::npos && colon > 0 && resolve(c_, name.substr(0, colon)))
      die(c_, "path '" + name.substr(colon + 1) + "' does not exist in '" + name.substr(0, colon) + "'");
    if (opt_ == 'e') return 1;
    die(c_, "Not a valid object name " + name);
  }
  Obj o;
  const bool have = fetch(*oid, o);
  if (opt_ == 'e') return have ? 0 : 1;
  if (!have) {
    if (!opt_) die(c_, "git cat-file " + name + ": bad file");
    die(c_, "Not a valid object name " + name);
  }
  std::string path;
  split_path(name, path);
  if (have_path_) path = path_arg_;
  if (opt_ == 't') { c_.out << o.type << "\n"; return 0; }
  if (opt_ == 's') { c_.out << (use_mailmap_ ? remap(o).size() : o.payload.size()) << "\n"; return 0; }
  if (opt_ == 'w' || opt_ == 'c') {
    if (path.empty()) die(c_, std::string("git cat-file ") + (opt_ == 'w' ? "--filters " : "--textconv ") + name + ": <object> must be <sha1:path>");
    transform_ = opt_;
    emit(transformed(o, path));
    return 0;
  }
  if (opt_ == 'p') {
    if (o.type == "tree") {
      auto ents = c_.repo->read_tree(o.oid);
      if (!ents.ok()) die(c_, ents.error().message);
      for (auto& e : *ents) c_.out << mode_text(e.mode) << " " << type_of_mode(e.mode) << " " << e.oid << "\t" << quote_path(e.name) << "\n";
      return 0;
    }
    emit(remap(o));
    return 0;
  }
  // <type> <object>: the object itself, or what it peels to
  const std::string want = exp_type;
  if (want != "blob" && want != "tree" && want != "commit" && want != "tag") die(c_, "invalid object type \"" + want + "\"");
  std::string peeled = peel_to(c_, o.oid, want);
  if (peeled.empty()) die(c_, "git cat-file " + name + ": bad file");
  Obj p = o;
  if (peeled != o.oid && !fetch(peeled, p)) die(c_, "git cat-file " + name + ": bad file");
  if (want == "tree" && p.oid == empty_tree_oid()) return 0;
  emit(remap(p));
  return 0;
}

int CatFile::run() {
  c_.open_repo(false);
  std::vector<std::string> pos;
  std::vector<std::string> args(c_.args.begin() + 1, c_.args.end());
  auto set_opt = [&](char o) {
    if (opt_ && opt_ != o) {
      auto name = [](char x) { return x == 'c' ? std::string("--textconv") : x == 'w' ? std::string("--filters") : std::string("-") + x; };
      c_.err << "error: options '" << name(o) << "' and '" << name(opt_) << "' cannot be used together\n";
      throw Exit{129};
    }
    opt_ = o;
  };
  int batch_options = 0;
  bool dashdash = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (dashdash) { pos.push_back(a); continue; }
    if (a == "--") { dashdash = true; continue; }
    if (a == "-h") { c_.out << kCatFileHelp; throw Exit{129}; }
    if (a == "-e") set_opt('e');
    else if (a == "-p") set_opt('p');
    else if (a == "-t") set_opt('t');
    else if (a == "-s") set_opt('s');
    else if (a == "--textconv") set_opt('c');
    else if (a == "--filters") set_opt('w');
    else if (a == "--use-mailmap" || a == "--mailmap") use_mailmap_ = true;
    else if (a == "--no-use-mailmap" || a == "--no-mailmap") use_mailmap_ = false;
    else if (a == "--batch" || a.rfind("--batch=", 0) == 0) { ++batch_options; batch_ = true; batch_check_ = false; batch_command_ = false; if (a.size() > 7) { format_ = a.substr(8); has_format_ = true; } }
    else if (a == "--batch-check" || a.rfind("--batch-check=", 0) == 0) { ++batch_options; batch_ = true; batch_check_ = true; batch_command_ = false; if (a.size() > 13) { format_ = a.substr(14); has_format_ = true; } }
    else if (a == "--batch-command" || a.rfind("--batch-command=", 0) == 0) { ++batch_options; batch_ = true; batch_command_ = true; batch_check_ = false; if (a.size() > 15) { format_ = a.substr(16); has_format_ = true; } }
    else if (a == "--batch-all-objects") all_objects_ = true;
    else if (a == "--buffer") { buffer_ = true; buffer_set_ = true; }
    else if (a == "--no-buffer") { buffer_ = false; buffer_set_ = true; }
    else if (a == "--follow-symlinks") follow_ = true;
    else if (a == "--no-follow-symlinks") follow_ = false;
    else if (a == "--unordered") unordered_ = true;
    else if (a == "--no-unordered") unordered_ = false;
    else if (a == "-Z") { in_delim_ = '\0'; out_delim_ = '\0'; }
    else if (a == "-z") { in_delim_ = '\0'; }
    else if (a == "--allow-unknown-type") {}
    else if (a == "--path") { if (i + 1 >= args.size()) usage_msg(c_, "option `path' requires a value"); path_arg_ = args[++i]; have_path_ = true; }
    else if (a.rfind("--path=", 0) == 0) { path_arg_ = a.substr(7); have_path_ = true; }
    else if (a == "--no-path") have_path_ = false;
    else if (a == "--filter" || a.rfind("--filter=", 0) == 0) unsupported(c_, "cat-file --filter");
    else if (a.size() > 1 && a[0] == '-') {
      c_.err << (a[1] == '-' ? "error: unknown option `" + a.substr(2) : "error: unknown switch `" + a.substr(1, 1)) << "'\n" << kCatFileHelp;
      throw Exit{129};
    } else pos.push_back(a);
  }
  if (batch_options > 1) { c_.err << "error: only one batch option may be specified\n"; throw Exit{129}; }
  if (all_objects_ && !batch_ && opt_) {
    auto name = [](char x) { return x == 'c' ? std::string("--textconv") : x == 'w' ? std::string("--filters") : std::string("-") + x; };
    c_.err << "error: options '" << name(opt_) << "' and '--batch-all-objects' cannot be used together\n";
    throw Exit{129};
  }
  if (!has_format_) format_ = batch_check_ || batch_command_ ? "%(objectname) %(objecttype) %(objectsize)" : "%(objectname) %(objecttype) %(objectsize)";
  const bool opt_cw = opt_ == 'c' || opt_ == 'w';
  if (batch_) {
    if (opt_ && !opt_cw) usage_msg(c_, std::string("'-") + opt_ + "' is incompatible with batch mode");
    if (opt_cw) transform_ = opt_;
    if (!pos.empty()) usage_msg(c_, "batch modes take no arguments");
  } else {
    if (all_objects_) usage_msg(c_, "'--batch-all-objects' requires a batch mode");
    if (buffer_) usage_msg(c_, "'--buffer' requires a batch mode");
    if (follow_) usage_msg(c_, "'--follow-symlinks' requires a batch mode");
    if (unordered_) usage_msg(c_, "'--unordered' requires a batch mode");
    if (in_delim_ == '\0' && out_delim_ == '\0') usage_msg(c_, "'-Z' requires a batch mode");
  }
  if (have_path_ && !opt_cw) usage_msg(c_, "'--path=<path|tree-ish>' needs '--filters' or '--textconv'");
  if (have_path_ && batch_) usage_msg(c_, "'--path=<path|tree-ish>' needs '--filters' or '--textconv'");
  if (use_mailmap_) mailmap_.load(c_);
  std::string obj_name, exp_type;
  if (!batch_) {
    if (opt_) {
      if (pos.empty()) {
        if (opt_cw) usage_msg(c_, std::string("<rev> required with '") + (opt_ == 'c' ? "--textconv" : "--filters") + "'");
        usage_msg(c_, std::string("<object> required with '-") + opt_ + "'");
      }
      if (pos.size() > 1 && !(opt_cw && have_path_)) usage_msg(c_, "too many arguments");
      obj_name = pos[0];
      if (opt_cw && have_path_) { if (pos.size() != 1) usage_msg(c_, "too many arguments"); }
    } else {
      if (pos.empty()) { c_.err << kCatFileUsage; throw Exit{129}; }
      if (pos.size() != 2) usage_msg(c_, "only two arguments allowed in <type> <object> mode, not " + std::to_string(pos.size()));
      exp_type = pos[0];
      obj_name = pos[1];
    }
  }
  if (batch_) {
    check_format(format_);
    if (transform_) want_rest_ = true;
    if (c_.config.get_bool("core.fsmonitor", false)) {}
    return run_batch();
  }
  if (opt_cw && !have_path_ && obj_name.find(':') == std::string::npos)
    die(c_, "<object>:<path> required, only <object> '" + obj_name + "' given");
  return run_single(exp_type, obj_name);
}

}  // namespace

int cmd_cat_file(Ctx& c) {
  CatFile cf(c);
  return cf.run();
}

}  // namespace aios_git
