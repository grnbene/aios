#include "shell.hpp"
#include "command_docs.hpp"
#include <algorithm>
#include <functional>
#include <regex>
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/patch_transaction.hpp"

namespace aios::cli {

namespace {

struct Ctx { Session& s; Out& o; Kernel& k; Surface& me; };
void need(Result<void> r) { if (!r.ok()) throw Fail{r.error()}; }
// The machine's own tree. Made the first time it is asked for, so a first
// write on a fresh root (echo > /f, touch, cp, edit) has somewhere to land.
std::string fs_root(Ctx& c) {
  std::string r = c.k.config().root + "/fs";
  if (!c.k.host().exists(r)) (void)c.k.host().make_dir(r);
  return r;
}

// Normalises a shell path against the session cwd into an absolute shell path
// ("/a/b"), then to a host path under the FS root.
std::string norm_shell(const std::string& cwd, const std::string& p) {
  std::string base = (!p.empty() && p[0] == '/') ? "" : cwd;
  std::vector<std::string> parts;
  for (auto& seg : split(base + "/" + p, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") { if (!parts.empty()) parts.pop_back(); continue; }
    parts.push_back(seg);
  }
  std::string out = "/";
  for (size_t i = 0; i < parts.size(); ++i) { out += parts[i]; if (i + 1 < parts.size()) out += "/"; }
  return out;
}
std::string host_of(Ctx& c, const std::string& shell_path) {
  if (shell_path.rfind("/host/", 0) == 0) return shell_path.substr(5);
  if (shell_path == "/host") return "/";
  return fs_root(c) + (shell_path == "/" ? "" : shell_path);
}
std::string cwd(Ctx& c) { auto it = c.s.vars.find("PWD"); return it == c.s.vars.end() ? "/" : it->second; }
void set_cwd(Ctx& c, const std::string& p) { c.s.vars["PWD"] = p; }

std::string human(uint64_t b) {
  if (b >= (1ull << 30)) return fmt_double(b / double(1ull << 30), 6, true, 1) + "G";
  if (b >= (1ull << 20)) return fmt_double(b / double(1ull << 20), 6, true, 1) + "M";
  if (b >= 1024) return fmt_double(b / 1024.0, 6, true, 1) + "K";
  return fmt_u64(b);
}

// Civil date from a wall-clock nanosecond count (UTC), no libc.
std::string iso_date(uint64_t wall_ns) {
  int64_t z = (int64_t)(wall_ns / 1000000000ull);
  int64_t secs = z % 86400; if (secs < 0) secs += 86400;
  int64_t days = (z - secs) / 86400;
  int64_t hh = secs / 3600, mm = (secs % 3600) / 60, ss = secs % 60;
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  uint64_t doe = (uint64_t)(days - era * 146097);
  uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint64_t mp = (5 * doy + 2) / 153;
  uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  auto p2 = [](uint64_t v) { std::string s = fmt_u64(v); return s.size() < 2 ? "0" + s : s; };
  return fmt_u64((uint64_t)y) + "-" + p2(m) + "-" + p2(d) + " " + p2(hh) + ":" + p2(mm) + ":" + p2(ss) + " UTC";
}

bool has_flag(const std::vector<std::string>& a, char f) { for (auto& t : a) if (t.size() >= 2 && t[0] == '-' && t[1] != '-') for (char c : t.substr(1)) if (c == f) return true; return false; }
std::vector<std::string> operands(const std::vector<std::string>& a) { std::vector<std::string> o; for (size_t i = 1; i < a.size(); ++i) if (a[i].empty() || a[i][0] != '-') o.push_back(a[i]); return o; }

}  // namespace

void shell_help(Out& o) {
  o << o.style(Header()) << "The Familiar Shell (mapped onto AOS; storage authority applies):" << o.style(Reset()) << "\n"
       "  pwd · cd · ls [-l] · cat · edit <file> <old> <new> · echo [>|>> file] · touch · mkdir [-p] · rm [-r] · rmdir · mv · cp · tree · stat · find · grep · wc · head · tail\n"
       "  ps (entities) · free (memory fabric) · uname [-a] · hostname · whoami · id · date · uptime · nproc · lsblk · df · dmesg · env · export N=V · unset · clear · history · man <cmd> · which\n";
}

bool shell_command(Session& sess, Out& out, Surface& me, const std::vector<std::string>& a) {
  Ctx c{sess, out, me.kernel(), me};
  const std::string& cmd = a[0];
  auto& o = c.o;
  Kernel& k = c.k;

  // The filesystem is the persistent root: storage authority, held or not.
  // Looking at the machine (ps, free, uname...) is kernel:read.
  static const char* reads[] = {"pwd", "cd", "ls", "cat", "tree", "stat", "find", "grep", "wc", "head", "tail", "which", nullptr};
  static const char* writes[] = {"touch", "mkdir", "rmdir", "rm", "cp", "mv", "edit", nullptr};
  static const char* looks[] = {"ps", "free", "uname", "hostname", "date", "uptime", "nproc", "lsblk", "df", nullptr};
  for (auto** p = reads; *p; ++p) if (cmd == *p) need(me.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Read));
  for (auto** p = writes; *p; ++p) if (cmd == *p) need(me.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write));
  for (auto** p = looks; *p; ++p) if (cmd == *p) need(me.inspect());
  if (cmd == "echo") { bool redir = false; for (auto& t : a) if (t == ">" || t == ">>") redir = true; if (redir) need(me.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write)); }

  auto rd = [&](const std::string& sp) -> ByteBuffer { auto r = k.host().read_file(host_of(c, norm_shell(cwd(c), sp))); if (!r.ok()) fail(r.status(), sp); return *r; };

  if (cmd == "pwd") { o << cwd(c) << "\n"; return true; }

  if (cmd == "cd") {
    auto ops = operands(a);
    std::string target = ops.empty() ? "/" : norm_shell(cwd(c), ops[0]);
    if (target != "/") { auto st = k.host().stat(host_of(c, target)); if (!st.ok()) fail(Status::NotFound, "cd: " + ops[0]); if (!st->is_dir) fail(Status::Invalid, "cd: not a directory: " + ops[0]); }
    set_cwd(c, target);
    return true;
  }

  if (cmd == "ls") {
    bool l = has_flag(a, 'l'); auto ops = operands(a);
    if (ops.empty()) ops.push_back(cwd(c));
    for (size_t i = 0; i < ops.size(); ++i) {   // every operand, like ls does
      std::string sp = norm_shell(cwd(c), ops[i]);
      std::string hp = host_of(c, sp);
      auto st = k.host().stat(hp);
      if (st.ok() && !st->is_dir) { if (l) o << "-  " << W{9} << human(st->size) << " " << ops[i] << "\n"; else o << ops[i] << "\n"; continue; }
      auto e = k.host().list_dir(hp);
      if (!e.ok()) {
        if (sp != "/") fail(e.status(), "ls: " + ops[i]);
        k.host().make_dir(hp);      // the machine's own tree, first time anyone looked
        e = std::vector<std::string>{};
      }
      if (ops.size() > 1) o << (i ? "\n" : "") << ops[i] << ":\n";
      // A listing with nothing in it printed NOTHING, which over a console reads
      // as a machine that did not answer. An empty directory is a fact, and a
      // machine that has one says so — and, at its own root, says where the
      // things it does have actually are.
      if (e->empty()) {
        o << "(empty)\n";
        if (sp == "/") {
          o << "this is the machine's own tree at " << fs_root(c) << ", and nothing has been written into it yet\n";
          o << "  /host          the substrate's filesystem, as this machine sees it\n";
          o << "  git repos      the repositories this kernel holds (under " << k.config().root << "/repos)\n";
        }
        continue;
      }
      std::sort(e->begin(), e->end());
      for (auto& name : *e) {
        if (l) { auto s2 = k.host().stat(hp + "/" + name); bool dir = s2.ok() && s2->is_dir; o << (dir ? "d " : "- ") << W{9} << (dir ? std::string("-") : human(s2.ok() ? s2->size : 0)) << " " << name << "\n"; }
        else o << name << "\n";
      }
    }
    return true;
  }

  if (cmd == "cat") { for (auto& f : operands(a)) { auto b = rd(f); o << std::string(b.begin(), b.end()); } return true; }

  if (cmd == "edit") {
    auto ops = operands(a);
    if (ops.size() < 3) fail(Status::Invalid, "edit <file> <target> <replacement>");
    std::string sp = ops[0];
    std::string hp = host_of(c, norm_shell(cwd(c), sp));
    if (hp.find("include/aios/kernel/") != std::string::npos ||
        hp.find("include/aios/capability/") != std::string::npos ||
        hp.find("src/kernel/") != std::string::npos ||
        hp.find("src/capability/") != std::string::npos) {
      fail(Status::Denied, "the kernel refuses: modifying kernel core code (" + sp + ") is prohibited by machine authority policy (kernel is protected)");
    }
    auto b = rd(sp);
    std::string text(b.begin(), b.end());
    std::string target = ops[1], rep = ops[2];
    auto pos = text.find(target);
    if (pos == std::string::npos) fail(Status::NotFound, "edit: target not found in " + sp);
    size_t count = 0;
    while (pos != std::string::npos) {
      text.replace(pos, target.length(), rep);
      count++;
      pos = text.find(target, pos + rep.length());
    }
    auto w = k.host().write_file(hp, span_of(text));
    if (!w.ok()) fail(w.status(), sp);
    o << "edited " << sp << " (" << count << " replacement" << (count == 1 ? "" : "s") << ")\n";
    return true;
  }

  if (cmd == "tx" || cmd == "transaction") {
    auto ops = operands(a);
    if (ops.empty() || ops[0] == "help" || ops[0] == "--help") {
      o << "AIOS Transaction Engine (Atomic Multi-File Edit Transactions)\n"
        << "  tx plan <patch>   - Pre-flight dry-run and print Intent Declaration\n"
        << "  tx stage <patch>  - Dry-run and stage transaction in memory\n"
        << "  tx commit         - Commit staged transaction atomically to disk\n"
        << "  tx abort          - Abort staged transaction; disk tree remains pristine\n"
        << "  tx show           - Display details of active staged transaction\n"
        << "  tx apply <patch>  - Pre-flight, stage and commit patch atomically in one step\n";
      return true;
    }
    std::string sub = ops[0];
    if (sub == "plan" && ops.size() >= 2) {
      auto b = rd(ops[1]);
      std::string text(b.begin(), b.end());
      auto parsed = aios::userland::PatchEngine::parse_unified_diff(text);
      if (!parsed.ok()) fail(parsed.status(), parsed.error().str());
      auto manifest = *parsed;
      auto dry = aios::userland::PatchEngine::dry_run(k, manifest, fs_root(c));
      o << aios::userland::PatchEngine::format_plan_manifest(manifest);
      if (dry.ok()) o << "[SUCCESS] Pre-flight dry-run check PASSED (" << manifest.files.size() << " files clean)\n";
      else o << "[REJECTED] Pre-flight dry-run FAILED: " << dry.error().str() << "\n";
      return true;
    }
    if (sub == "stage" && ops.size() >= 2) {
      auto b = rd(ops[1]);
      std::string text(b.begin(), b.end());
      auto parsed = aios::userland::PatchEngine::parse_unified_diff(text);
      if (!parsed.ok()) fail(parsed.status(), parsed.error().str());
      auto manifest = *parsed;
      auto res = aios::userland::PatchEngine::stage(k, manifest, fs_root(c));
      if (!res.ok()) fail(res.status(), res.error().str());
      o << "[STAGED] Transaction #" << manifest.tx_id << ": " << manifest.title << "\n"
        << "  " << manifest.files.size() << " files staged in memory. Main tree is pristine.\n"
        << "  Commit with `tx commit` or abort with `tx abort`.\n";
      return true;
    }
    if (sub == "commit") {
      if (!aios::userland::PatchEngine::has_active_transaction()) {
        fail(Status::Invalid, "No active staged transaction. Use `tx stage <patch>` first.");
      }
      auto& m = aios::userland::PatchEngine::active_transaction();
      auto res = aios::userland::PatchEngine::commit(k, m, fs_root(c));
      if (!res.ok()) fail(res.status(), res.error().str());
      o << "[COMMITTED] Transaction #" << m.tx_id << " written atomically to disk ("
        << m.files.size() << " files updated).\n";
      return true;
    }
    if (sub == "abort") {
      if (aios::userland::PatchEngine::has_active_transaction()) {
        auto& m = aios::userland::PatchEngine::active_transaction();
        uint64_t tid = m.tx_id;
        aios::userland::PatchEngine::abort(m);
        o << "[ABORTED] Transaction #" << tid << " discarded from memory. Main tree pristine.\n";
      } else {
        o << "No active staged transaction to abort.\n";
      }
      return true;
    }
    if (sub == "show" || sub == "status" || sub == "list") {
      if (!aios::userland::PatchEngine::has_active_transaction()) {
        o << "No active staged transaction in memory.\n";
        return true;
      }
      auto& m = aios::userland::PatchEngine::active_transaction();
      o << aios::userland::PatchEngine::format_plan_manifest(m)
        << "Status: " << aios::userland::transaction_status_name(m.status) << "\n"
        << "Hunks: " << m.active_hunks_count() << "/" << m.total_hunks() << " accepted\n"
        << "Delta: +" << m.total_additions() << " -" << m.total_deletions() << " lines\n";
      return true;
    }
    if (sub == "apply" && ops.size() >= 2) {
      auto b = rd(ops[1]);
      std::string text(b.begin(), b.end());
      auto res = aios::userland::PatchEngine::apply_patch_atomic(k, text, "Atomic Patch", fs_root(c));
      if (!res.ok()) fail(res.status(), res.error().str());
      o << "[COMMITTED] Patch applied atomically to disk\n";
      return true;
    }
    fail(Status::Invalid, "Unknown tx subcommand: " + sub);
  }

  if (cmd == "patch") {
    auto ops = operands(a);
    if (ops.empty()) fail(Status::Invalid, "Usage: patch <patch_file>");
    auto b = rd(ops[0]);
    std::string text(b.begin(), b.end());
    auto res = aios::userland::PatchEngine::apply_patch_atomic(k, text, "Atomic Patch", fs_root(c));
    if (!res.ok()) fail(res.status(), res.error().str());
    o << "[COMMITTED] Patch applied atomically to disk\n";
    return true;
  }

  if (cmd == "echo") {
    // handled natively too, but support redirection here: echo text > file
    std::string text; std::string redir; bool append = false;
    for (size_t i = 1; i < a.size(); ++i) {
      if (a[i] == ">" && i + 1 < a.size()) { redir = a[i + 1]; ++i; }
      else if (a[i] == ">>" && i + 1 < a.size()) { redir = a[i + 1]; append = true; ++i; }
      else { if (!text.empty()) text += " "; text += a[i]; }
    }
    if (redir.empty()) return false;   // let the native echo print it
    text += "\n";
    std::string hp = host_of(c, norm_shell(cwd(c), redir));
    auto r = append ? k.host().append_file(hp, span_of(text)) : k.host().write_file(hp, span_of(text));
    if (!r.ok()) fail(r.status(), redir);
    return true;
  }

  if (cmd == "touch") { for (auto& f : operands(a)) { std::string hp = host_of(c, norm_shell(cwd(c), f)); if (!k.host().exists(hp)) { auto r = k.host().write_file(hp, {}); if (!r.ok()) fail(r.status(), f); } } return true; }

  if (cmd == "mkdir") {
    bool parents = has_flag(a, 'p');
    for (auto& d : operands(a)) {
      std::string sp = norm_shell(cwd(c), d);
      std::string hp = host_of(c, sp);
      if (parents) {
        if (auto st = k.host().stat(hp); st.ok() && !st->is_dir) fail(Status::Invalid, "mkdir: " + d + ": File exists");
      } else {
        if (k.host().exists(hp)) fail(Status::Invalid, "mkdir: " + d + ": File exists");
        size_t last_slash = sp.find_last_of('/');
        std::string parent_sp = (last_slash == 0) ? "/" : (last_slash != std::string::npos ? sp.substr(0, last_slash) : cwd(c));
        if (!k.host().exists(host_of(c, parent_sp))) fail(Status::NotFound, "mkdir: " + d + ": No such file or directory");
      }
      auto r = k.host().make_dir(hp);
      if (!r.ok()) fail(r.status(), d);
    }
    return true;
  }

  if (cmd == "rmdir") { for (auto& d : operands(a)) { auto r = k.host().remove(host_of(c, norm_shell(cwd(c), d))); if (!r.ok()) fail(r.status(), d); } return true; }

  if (cmd == "rm") {
    bool rec = has_flag(a, 'r');
    for (auto& f : operands(a)) {
      std::string sp = norm_shell(cwd(c), f), hp = host_of(c, sp);
      auto st = k.host().stat(hp);
      if (!st.ok()) fail(Status::NotFound, "rm: " + f);
      if (st->is_dir && rec) {
        std::vector<std::string> stack{sp};
        std::vector<std::string> order;
        while (!stack.empty()) { std::string cur = stack.back(); stack.pop_back(); order.push_back(cur); auto e = k.host().list_dir(host_of(c, cur)); if (e.ok()) for (auto& n : *e) stack.push_back(cur + "/" + n); }
        for (auto it = order.rbegin(); it != order.rend(); ++it) k.host().remove(host_of(c, *it));
      } else { auto r = k.host().remove(hp); if (!r.ok()) fail(r.status(), f); }
    }
    return true;
  }

  if (cmd == "cp" || cmd == "mv") {
    auto ops = operands(a);
    if (ops.size() < 2) fail(Status::Invalid, cmd + " <src> <dst>");
    auto b = rd(ops[0]);
    std::string dst = norm_shell(cwd(c), ops[1]);
    auto dstat = k.host().stat(host_of(c, dst));
    if (dstat.ok() && dstat->is_dir) { std::string base = ops[0]; auto sl = base.find_last_of('/'); if (sl != std::string::npos) base = base.substr(sl + 1); dst += "/" + base; }
    auto w = k.host().write_file(host_of(c, dst), b);
    if (!w.ok()) fail(w.status(), ops[1]);
    if (cmd == "mv") k.host().remove(host_of(c, norm_shell(cwd(c), ops[0])));
    return true;
  }

  if (cmd == "tree") {
    auto ops = operands(a);
    std::string root = ops.empty() ? cwd(c) : norm_shell(cwd(c), ops[0]);
    std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& sp, const std::string& indent) {
      auto e = k.host().list_dir(host_of(c, sp));
      if (!e.ok()) return;
      std::sort(e->begin(), e->end());
      for (size_t i = 0; i < e->size(); ++i) {
        bool last = i + 1 == e->size();
        auto st = k.host().stat(host_of(c, sp + "/" + (*e)[i]));
        o << indent << (last ? "`-- " : "|-- ") << (*e)[i] << ((st.ok() && st->is_dir) ? "/" : "") << "\n";
        if (st.ok() && st->is_dir) walk(sp + "/" + (*e)[i], indent + (last ? "    " : "|   "));
      }
    };
    o << root << "\n"; walk(root, "");
    return true;
  }

  if (cmd == "stat") {
    for (auto& f : operands(a)) { std::string sp = norm_shell(cwd(c), f); auto st = k.host().stat(host_of(c, sp)); if (!st.ok()) fail(st.status(), f); o << sp << "  " << (st->is_dir ? "directory" : "file") << "  " << st->size << " bytes\n"; }
    return true;
  }

  if (cmd == "wc") { for (auto& f : operands(a)) { auto b = rd(f); uint64_t lines = 0, words = 0; bool in = false; for (uint8_t ch : b) { if (ch == '\n') lines++; if (ch == ' ' || ch == '\n' || ch == '\t') in = false; else if (!in) { in = true; words++; } } o << W{6} << fmt_u64(lines) << W{7} << fmt_u64(words) << W{8} << fmt_u64(b.size()) << " " << f << "\n"; } return true; }

  if (cmd == "head" || cmd == "tail") {
    // -n N, -nN and -N set the count; the value after -n is not a file.
    uint64_t n = 10; std::vector<std::string> ops;
    for (size_t i = 1; i < a.size(); ++i) {
      const std::string& t = a[i];
      if (t == "-n") {
        if (i + 1 >= a.size() || !parse_u64(a[i + 1], n)) fail(Status::Invalid, cmd + ": -n needs a line count");
        ++i;
      } else if (t.size() > 2 && t.rfind("-n", 0) == 0) {
        if (!parse_u64(t.substr(2), n)) fail(Status::Invalid, cmd + ": bad line count '" + t.substr(2) + "'");
      } else if (t.size() > 1 && t[0] == '-' && t[1] >= '0' && t[1] <= '9') {
        if (!parse_u64(t.substr(1), n)) fail(Status::Invalid, cmd + ": bad line count '" + t.substr(1) + "'");
      } else if (!t.empty() && t[0] == '-') {
        fail(Status::Invalid, cmd + ": unknown option " + t + " (" + cmd + " [-n N] <file...>)");
      } else {
        ops.push_back(t);
      }
    }
    if (ops.empty()) fail(Status::Invalid, cmd + " [-n N] <file...>");
    for (auto& f : ops) { auto b = rd(f); auto ls = split(std::string(b.begin(), b.end()), '\n'); if (cmd == "head") { for (uint64_t i = 0; i < n && i < ls.size(); ++i) o << ls[i] << "\n"; } else { uint64_t start = ls.size() > n ? ls.size() - n : 0; for (uint64_t i = start; i < ls.size(); ++i) o << ls[i] << "\n"; } }
    return true;
  }

  if (cmd == "grep") {
    // Patterns are POSIX basic regular expressions, as grep reads them; -E
    // extended, -F a literal string, -i ignores case, -v keeps the lines that
    // do not match, -n numbers them. No line selected is exit status 1.
    auto ops = operands(a);
    if (ops.size() < 2) fail(Status::Invalid, "grep [-E|-F] [-i] [-v] [-n] <pattern> <file...>");
    const std::string& pat = ops[0];
    const bool invert = has_flag(a, 'v'), line_no = has_flag(a, 'n'), fixed = has_flag(a, 'F'), icase = has_flag(a, 'i');
    std::regex re;
    if (!fixed) {
      auto flags = has_flag(a, 'E') ? std::regex::extended : std::regex::basic;
      if (icase) flags |= std::regex::icase;
      try { re = std::regex(pat, flags); }
      catch (const std::regex_error& e) { fail(Status::Invalid, "grep: invalid regular expression '" + pat + "': " + e.what()); }
    }
    auto lower = [](std::string t) { for (auto& ch : t) ch = (char)std::tolower((unsigned char)ch); return t; };
    const std::string lpat = lower(pat);
    bool any = false;
    for (size_t fi = 1; fi < ops.size(); ++fi) {
      auto b = rd(ops[fi]);
      uint64_t ln = 0;
      for (auto& line : split(std::string(b.begin(), b.end()), '\n')) {
        ++ln;
        bool match;
        if (fixed) match = icase ? lower(line).find(lpat) != std::string::npos : line.find(pat) != std::string::npos;
        else {
          try { match = std::regex_search(line, re); }
          catch (const std::regex_error& e) { fail(Status::Invalid, "grep: '" + pat + "' on line " + fmt_u64(ln) + " of " + ops[fi] + ": " + e.what()); }
        }
        if (match == invert) continue;
        any = true;
        if (ops.size() > 2) o << ops[fi] << ":";
        if (line_no) o << fmt_u64(ln) << ":";
        o << line << "\n";
      }
    }
    if (!any) fail(Status::NotFound, "");
    return true;
  }

  if (cmd == "find") {
    // The word after -name is the name to look for, not a path to start from.
    std::vector<std::string> ops;
    std::string name;
    for (size_t i = 1; i < a.size(); ++i) {
      if (a[i] == "-name") { if (i + 1 < a.size()) name = a[++i]; continue; }
      if (!a[i].empty() && a[i][0] != '-') ops.push_back(a[i]);
    }
    std::string root = ops.empty() ? cwd(c) : norm_shell(cwd(c), ops[0]);
    std::vector<std::string> stack{root};
    while (!stack.empty()) {
      std::string cur = stack.back(); stack.pop_back();
      if (name.empty() || cur.substr(cur.find_last_of('/') + 1).find(name) != std::string::npos) o << cur << "\n";
      auto e = k.host().list_dir(host_of(c, cur));
      if (e.ok()) { std::sort(e->begin(), e->end()); for (auto it = e->rbegin(); it != e->rend(); ++it) stack.push_back(cur == "/" ? "/" + *it : cur + "/" + *it); }
    }
    return true;
  }

  if (cmd == "clear") { o << "\033[2J\033[H"; return true; }
  if (cmd == "history") { auto it = c.s.vars.find("_history"); if (it != c.s.vars.end()) { uint64_t n = 1; for (auto& line : split(it->second, '\n')) o << W{5} << fmt_u64(n++) << " " << line << "\n"; } return true; }

  if (cmd == "uname") {
    auto& t = k.host().topology();
    if (has_flag(a, 'a')) o << "AIOS " << t.host_name << " " << k.self().short_str() << " " << t.arch << " " << t.logical_cpus << "-cpu AIOS\n";
    else o << "AIOS\n";
    return true;
  }
  if (cmd == "hostname") { o << k.host().topology().host_name << "\n"; return true; }
  if (cmd == "whoami" || cmd == "id") { const Entity* e = k.entities().get(me.self()); o << "entity " << me.self().str() << (e ? " (" + e->label + ")" : "") << "\n"; return true; }
  if (cmd == "date") { o << iso_date(k.now()) << "\n"; return true; }
  if (cmd == "uptime") { uint64_t ms = k.uptime_ns() / 1000000; o << "up " << fmt_u64(ms / 1000) << "." << fmt_u64((ms % 1000) / 100) << "s, " << k.entities().count() << " entities\n"; return true; }
  if (cmd == "nproc") { o << k.host().topology().logical_cpus << "\n"; return true; }

  if (cmd == "free") {
    auto ms = k.memory().stats();
    auto& t = k.host().topology();
    o << o.style(Header()) << W{14} << "MEMORY" << W{14} << "TOTAL" << W{14} << "USED" << o.style(Reset()) << "\n";
    o << W{14} << "System RAM" << W{14} << human(t.total_memory) << W{14} << human(ms.bytes) << "\n";
    o << "Fabric: " << ms.objects << " objects across system/device/shared/persistent/remote, " << ms.migrations << " migrations\n";
    return true;
  }

  if (cmd == "ps") {
    o << o.style(Header()) << W{14} << "ENTITY ID" << W{12} << "STATUS" << W{20} << "LABEL" << "MIND" << o.style(Reset()) << "\n";
    for (auto& e : k.entities().list()) {
      auto* mb = k.mind(e.id);
      o << W{14} << e.id.short_str() << W{12} << entity_status_name(e.status) << W{20} << e.label << (mb ? mb->mind->name() : "-") << "\n";
    }
    return true;
  }

  if (cmd == "lsblk" || cmd == "df") {
    o << "the AOS persistent root is at " << k.config().root << "; use `host disks` / `host volume` for the underlying store\n";
    return true;
  }
  if (cmd == "dmesg") { o << "AOS keeps no ring log; `info` is the live state and the object store is the history\n"; return true; }

  if (cmd == "env") { for (auto& [key, val] : c.s.vars) if (!key.empty() && key[0] != '_') o << key << "=" << val << "\n"; return true; }
  if (cmd == "export" || cmd == "set") {
    if (a.size() < 2) return cmd == "set" ? false : true;   // bare `set` is the native command
    auto eq = a[1].find('=');
    if (eq == std::string::npos) return false;
    c.s.vars[a[1].substr(0, eq)] = a[1].substr(eq + 1);
    return true;
  }
  if (cmd == "unset") { for (size_t i = 1; i < a.size(); ++i) c.s.vars.erase(a[i]); return true; }
  if (cmd == "true") { return true; }
  if (cmd == "false") { fail(Status::Invalid, ""); }
  // The familiar commands this file maps, one list for `man` and `which`.
  static const char* known[] = {"pwd","cd","ls","cat","edit","tx","patch","echo","touch","mkdir","rmdir","rm","cp","mv","tree","stat","wc","head","tail","grep","find","clear","history","uname","hostname","whoami","id","date","uptime","nproc","free","ps","lsblk","df","dmesg","env","export","unset","true","false","man","which"};
  auto is_known = [&](const std::string& w) { for (auto* n : known) if (w == n) return true; return false; };
  if (cmd == "man") {
    if (a.size() < 2) { shell_help(o); return true; }
    const std::string& target = a[1];
    // Each line says what this shell's command does, which is not always what
    // the POSIX one does.
    static const std::pair<const char*, const char*> pages[] = {
      {"pwd", "pwd: print the working directory"},
      {"cd", "cd [dir]: change the working directory; with no argument, to /"},
      {"ls", "ls [-l] [path...]: list directories; -l adds each entry's type and size"},
      {"cat", "cat <file...>: print files"},
      {"edit", "edit <file> <old> <new>: replace every occurrence of <old> in <file> with <new>"},
      {"tx", "tx plan|stage|apply <patch> | tx commit|abort|show: stage a multi-file patch and commit it atomically (`tx help` lists the steps)"},
      {"patch", "patch <patch-file>: apply a unified diff to the tree atomically"},
      {"echo", "echo <text> [> file | >> file]: print text, or write or append it to a file"},
      {"touch", "touch <file...>: create each file that does not exist; an existing file is left as it is"},
      {"mkdir", "mkdir [-p] <dir...>: create directories; without -p the parent must exist and the directory must not, -p creates parents and accepts a directory that exists"},
      {"rmdir", "rmdir <dir...>: remove empty directories"},
      {"rm", "rm [-r] <path...>: remove files; -r removes a directory and everything under it"},
      {"cp", "cp <src> <dst>: copy a file; into <dst> when <dst> is a directory"},
      {"mv", "mv <src> <dst>: move a file; into <dst> when <dst> is a directory"},
      {"tree", "tree [path]: the directory hierarchy under path"},
      {"stat", "stat <path...>: the type and size of each path"},
      {"wc", "wc <file...>: lines, words and bytes in each file"},
      {"head", "head [-n N] <file...>: the first N lines of each file (10 by default)"},
      {"tail", "tail [-n N] <file...>: the last N lines of each file (10 by default)"},
      {"grep", "grep [-E|-F] [-i] [-v] [-n] <pattern> <file...>: lines matching a basic regular expression (-E extended, -F a literal string); exit status 1 when no line is selected"},
      {"find", "find [path] [-name text]: every path under path; -name keeps those whose last component contains text"},
      {"clear", "clear: clear the screen"},
      {"history", "history: the commands typed in this session, numbered"},
      {"uname", "uname [-a]: the system name; -a adds the host name, kernel id, architecture and CPU count"},
      {"hostname", "hostname: the host name"},
      {"whoami", "whoami: this console's entity id and label"},
      {"id", "id: this console's entity id and label (the same as whoami)"},
      {"date", "date: the current date and time"},
      {"uptime", "uptime: how long this kernel has run, and how many entities it holds"},
      {"nproc", "nproc: the number of logical CPUs"},
      {"free", "free: host RAM and how much of it the memory fabric holds"},
      {"ps", "ps: every entity, with its status, label and bound mind"},
      {"lsblk", "lsblk: where the persistent root is; `host disks` and `host volume` show the store under it"},
      {"df", "df: where the persistent root is; `host disks` and `host volume` show the store under it"},
      {"dmesg", "dmesg: AOS keeps no ring log; `info` shows the live state and the object store holds the history"},
      {"env", "env: the session variables"},
      {"export", "export NAME=VALUE: set a session variable"},
      {"unset", "unset NAME...: remove session variables"},
      {"true", "true: succeed"},
      {"false", "false: fail (exit status 1)"},
      {"man", "man [cmd]: this page for a familiar command, the help line for a kernel command; with no argument, the list"},
      {"which", "which <cmd...>: whether each is a familiar shell command"},
    };
    for (const auto& [name, page] : pages)
      if (target == name) { o << page << "\n"; return true; }
    // A kernel command (or one of its aliases): the command in full, as `help <cmd>` shows it.
    if (const CommandDoc* d = find_command_doc(target)) { print_command_help(o, *d); return true; }
    fail(Status::NotFound, "no manual entry for " + target + ": `man` lists the familiar commands, `help` the kernel commands");
  }
  if (cmd == "which") {
    for (size_t i = 1; i < a.size(); ++i) { bool f = is_known(a[i]); o << a[i] << (f ? ": shell builtin" : ": not found") << "\n"; if (!f) fail(Status::NotFound, ""); }
    return true;
  }

  return false;
}

}  // namespace aios::cli
