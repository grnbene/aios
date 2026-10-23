// apply, archive, worktree, grep.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <regex>
#include <sstream>

#include "aios/hal/host.hpp"
#include "aios/hal/sync.hpp"
#include "change.hpp"
#include "cli.hpp"
#include "color.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------
namespace {

struct Hunk {
  long old_start = 0, old_count = 0, new_start = 0, new_count = 0;
  std::vector<std::string> lines;   // with their ' ', '-', '+' prefix and no '\n'
  bool old_no_eol = false, new_no_eol = false;
  size_t line_no = 0;               // where it starts in the patch
};
struct BinaryHunk { std::string kind; size_t size = 0; std::string data; };
struct FilePatch {
  std::string old_path, new_path;
  bool is_new = false, is_delete = false, is_rename = false, is_copy = false;
  uint32_t old_mode = 0, new_mode = 0;
  std::string old_oid, new_oid;      // from the "index" line, as abbreviated there
  int similarity = 0;                // of a rename or copy
  std::vector<Hunk> hunks;
  bool binary = false, binary_without_data = false;
  BinaryHunk forward, reverse;
};

std::string unquote(const std::string& s) {
  if (s.size() < 2 || s[0] != '"') return s;
  std::string out;
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    if (s[i] != '\\') { out += s[i]; continue; }
    char e = s[++i];
    if (e >= '0' && e <= '7') { int v = (e - '0') * 64 + (s[i + 1] - '0') * 8 + (s[i + 2] - '0'); out += (char)v; i += 2; }
    else out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'a' ? '\a' : e == 'b' ? '\b' : e == 'f' ? '\f' : e == 'r' ? '\r' : e == 'v' ? '\v' : e;
  }
  return out;
}

std::string strip_components(const std::string& p, int n) {
  std::string s = p;
  for (int i = 0; i < n; ++i) {
    size_t sl = s.find('/');
    if (sl == std::string::npos) return s;
    s = s.substr(sl + 1);
  }
  return s;
}

int de85(char ch) {
  static const char en85[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";
  const char* at = std::strchr(en85, ch);
  return at && ch ? (int)(at - en85) : -1;
}

bool decode_binary_lines(const std::vector<std::string>& lines, std::string& out) {
  for (auto& l : lines) {
    if (l.empty()) continue;
    char lc = l[0];
    size_t len = lc >= 'A' && lc <= 'Z' ? (size_t)(lc - 'A' + 1) : lc >= 'a' && lc <= 'z' ? (size_t)(lc - 'a' + 27) : 0;
    if (!len) return false;
    std::string enc = l.substr(1);
    std::string bytes;
    for (size_t i = 0; i + 5 <= enc.size(); i += 5) {
      uint32_t acc = 0;
      for (int k = 0; k < 5; ++k) {
        int v = de85(enc[i + (size_t)k]);
        if (v < 0) return false;
        acc = acc * 85 + (uint32_t)v;
      }
      for (int k = 3; k >= 0; --k) bytes += (char)((acc >> (k * 8)) & 0xff);
    }
    if (bytes.size() < len) return false;
    out += bytes.substr(0, len);
  }
  return true;
}

std::vector<FilePatch> parse_patch(Ctx& c, const std::string& text, int strip, bool recount) {
  std::vector<std::string> lines;
  {
    std::string cur;
    for (char ch : text) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) lines.push_back(cur);
  }
  std::vector<FilePatch> out;
  FilePatch* cur = nullptr;
  auto path_of = [&](const std::string& raw) -> std::string {
    std::string p = unquote(raw);
    size_t tab = p.find('\t');
    if (tab != std::string::npos && raw[0] != '"') p = p.substr(0, tab);
    if (p == "/dev/null") return "";
    return strip_components(p, strip);
  };
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string l = lines[i];
    if (!l.empty() && l.back() == '\r' && false) l.pop_back();
    if (l.rfind("diff --git ", 0) == 0) {
      out.push_back(FilePatch{});
      cur = &out.back();
      std::string rest = l.substr(11);
      // "a/x b/x": split where both halves name the same path (works for names with spaces)
      if (rest[0] == '"') {
        size_t end = rest.find("\" ", 1);
        cur->old_path = path_of(rest.substr(0, end + 1));
        cur->new_path = path_of(rest.substr(end + 2));
      } else {
        size_t mid = std::string::npos;
        for (size_t k = 0; k < rest.size(); ++k) {
          if (rest[k] != ' ') continue;
          std::string a = strip_components(rest.substr(0, k), strip), b = strip_components(rest.substr(k + 1), strip);
          if (a == b) { mid = k; break; }
        }
        if (mid == std::string::npos) mid = rest.find(" b/");
        if (mid != std::string::npos) {
          for (const std::string& half : {rest.substr(0, mid), rest.substr(mid + 1)})
            if (strip > 0 && (int)std::count(half.begin(), half.end(), '/') < strip)
              die(c, "git diff header lacks filename information when removing " + std::to_string(strip) + " leading pathname component" +
                         (strip == 1 ? "" : "s") + " (line " + std::to_string(i + 1) + ")");
          cur->old_path = strip_components(rest.substr(0, mid), strip);
          cur->new_path = strip_components(rest.substr(mid + 1), strip);
        }
      }
      continue;
    }
    if (l.rfind("--- ", 0) == 0 && i + 1 < lines.size() && lines[i + 1].rfind("+++ ", 0) == 0) {
      if (!cur || !cur->hunks.empty() || cur->binary) { out.push_back(FilePatch{}); cur = &out.back(); }
      std::string a = l.substr(4), b = lines[i + 1].substr(4);
      std::string pa = path_of(a), pb = path_of(b);
      if (pa.empty() && a.rfind("/dev/null", 0) == 0) cur->is_new = true;
      else if (!pa.empty()) cur->old_path = pa;
      if (pb.empty() && b.rfind("/dev/null", 0) == 0) cur->is_delete = true;
      else if (!pb.empty()) cur->new_path = pb;
      if (cur->is_new && cur->old_path.empty()) cur->old_path = cur->new_path;
      if (cur->is_delete && cur->new_path.empty()) cur->new_path = cur->old_path;
      ++i;
      continue;
    }
    if (!cur) continue;
    auto mode_of = [](const std::string& s) { return (uint32_t)std::stoul(s, nullptr, 8); };
    if (l.rfind("old mode ", 0) == 0) { cur->old_mode = mode_of(l.substr(9)); continue; }
    if (l.rfind("new mode ", 0) == 0) { cur->new_mode = mode_of(l.substr(9)); continue; }
    if (l.rfind("deleted file mode ", 0) == 0) { cur->is_delete = true; cur->old_mode = mode_of(l.substr(18)); continue; }
    if (l.rfind("new file mode ", 0) == 0) { cur->is_new = true; cur->new_mode = mode_of(l.substr(14)); continue; }
    if (l.rfind("rename from ", 0) == 0) { cur->is_rename = true; cur->old_path = unquote(l.substr(12)); continue; }
    if (l.rfind("rename to ", 0) == 0) { cur->is_rename = true; cur->new_path = unquote(l.substr(10)); continue; }
    if (l.rfind("copy from ", 0) == 0) { cur->is_copy = true; cur->old_path = unquote(l.substr(10)); continue; }
    if (l.rfind("copy to ", 0) == 0) { cur->is_copy = true; cur->new_path = unquote(l.substr(8)); continue; }
    if (l.rfind("index ", 0) == 0) {
      size_t dots = l.find("..", 6);
      if (dots != std::string::npos) {
        size_t sp_end = l.find(' ', dots);
        cur->old_oid = l.substr(6, dots - 6);
        cur->new_oid = l.substr(dots + 2, sp_end == std::string::npos ? std::string::npos : sp_end - dots - 2);
      }
      size_t sp = l.find(' ', 6);
      if (sp != std::string::npos && cur->old_mode == 0 && cur->new_mode == 0) { cur->old_mode = cur->new_mode = mode_of(l.substr(sp + 1)); }
      continue;
    }
    if (l.rfind("similarity index ", 0) == 0) { cur->similarity = std::atoi(l.c_str() + 17); continue; }
    if (l.rfind("dissimilarity index", 0) == 0) continue;
    if (l.rfind("Binary files ", 0) == 0) { cur->binary = true; cur->binary_without_data = true; continue; }
    if (l == "GIT binary patch") {
      cur->binary = true;
      for (int side = 0; side < 2 && i + 1 < lines.size(); ++side) {
        std::string head = lines[i + 1];
        if (head.rfind("literal ", 0) != 0 && head.rfind("delta ", 0) != 0) break;
        ++i;
        BinaryHunk& bh = side == 0 ? cur->forward : cur->reverse;
        bh.kind = head.substr(0, head.find(' '));
        bh.size = (size_t)std::stoull(head.substr(head.find(' ') + 1));
        std::vector<std::string> body;
        while (i + 1 < lines.size() && !lines[i + 1].empty()) body.push_back(lines[++i]);
        if (i + 1 < lines.size()) ++i;   // the blank line
        std::string z;
        if (!decode_binary_lines(body, z)) die(c, "corrupt binary patch");
        auto inflated = zlib_inflate(ByteSpan((const uint8_t*)z.data(), z.size()));
        if (!inflated.ok()) die(c, "corrupt binary patch: " + inflated.error().message);
        bh.data.assign((const char*)inflated->data(), inflated->size());
      }
      continue;
    }
    if (l.rfind("@@ ", 0) == 0) {
      Hunk h;
      h.line_no = i + 1;
      long a = 0, b = 1, cc = 0, d = 1;
      const char* s = l.c_str() + 3;
      if (std::sscanf(s, "-%ld,%ld +%ld,%ld", &a, &b, &cc, &d) == 4) {}
      else if (std::sscanf(s, "-%ld +%ld,%ld", &a, &cc, &d) == 3) b = 1;
      else if (std::sscanf(s, "-%ld,%ld +%ld", &a, &b, &cc) == 3) d = 1;
      else if (std::sscanf(s, "-%ld +%ld", &a, &cc) == 2) { b = 1; d = 1; }
      else die(c, "corrupt patch at line " + std::to_string(i + 1));
      h.old_start = a; h.old_count = b; h.new_start = cc; h.new_count = d;
      long seen_old = 0, seen_new = 0;
      while (i + 1 < lines.size() && (recount || seen_old < h.old_count || seen_new < h.new_count)) {
        std::string hl = lines[i + 1];
        if (recount) {
          // the counts are not to be trusted: the hunk runs until the next header (or anything that is not a hunk line)
          if (hl.rfind("@@ ", 0) == 0 || hl.rfind("diff ", 0) == 0) break;
          if (hl.rfind("--- ", 0) == 0 && i + 2 < lines.size() && lines[i + 2].rfind("+++ ", 0) == 0) break;
        }
        if (hl.empty()) hl = " ";
        char k = hl[0];
        if (k == '\\') {
          // "\ No newline at end of file": the line before it ends its side without one
          char prev = h.lines.empty() ? ' ' : h.lines.back()[0];
          if (prev == '-') h.old_no_eol = true;
          else if (prev == '+') h.new_no_eol = true;
          else { h.old_no_eol = true; h.new_no_eol = true; }
          ++i;
          continue;
        }
        if (k != ' ' && k != '-' && k != '+') break;
        ++i;
        h.lines.push_back(hl);
        if (k != '+') ++seen_old;
        if (k != '-') ++seen_new;
      }
      if (recount) { h.old_count = seen_old; h.new_count = seen_new; }
      else if (seen_old != h.old_count || seen_new != h.new_count) die(c, "corrupt patch at line " + std::to_string(i + 1));
      // "\ No newline at end of file" after the last line of a side
      while (i + 1 < lines.size() && lines[i + 1].rfind("\\", 0) == 0) {
        ++i;
        // which side the marker belongs to: the line just before it
        char k = h.lines.empty() ? ' ' : h.lines.back()[0];
        if (k == '-') h.old_no_eol = true;
        else if (k == '+') h.new_no_eol = true;
        else { h.old_no_eol = true; h.new_no_eol = true; }
      }
      // a marker can also follow the '-' block when '+' lines come after it
      cur->hunks.push_back(h);
      continue;
    }
  }
  // markers in the middle of a hunk (after a '-' run followed by '+' lines) are handled by re-reading
  return out;
}

std::vector<std::string> split_keep(const std::string& s, bool& last_no_eol) {
  std::vector<std::string> out;
  size_t i = 0;
  last_no_eol = false;
  while (i < s.size()) {
    size_t nl = s.find('\n', i);
    if (nl == std::string::npos) { out.push_back(s.substr(i)); last_no_eol = true; break; }
    out.push_back(s.substr(i, nl - i));
    i = nl + 1;
  }
  return out;
}

struct ApplyOpts {
  bool unidiff_zero = false;     // --unidiff-zero: a hunk without context may sit anywhere its line numbers say
  bool ignore_ws = false;        // --ignore-space-change / --ignore-whitespace
  bool fix_ws = false;           // --whitespace=fix
  bool reject = false;           // --reject: apply what fits, hand back the hunks that do not
  bool keep_cr = false;          // the file being patched has CRLF line endings: a CR at the end of a line is no whitespace error
};

// What --whitespace=fix does to a line a patch adds.
std::string fix_whitespace(const std::string& line, bool keep_cr) {
  std::string s = line;
  const bool had_cr = keep_cr && !s.empty() && s.back() == '\r';
  if (had_cr) s.pop_back();
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
  size_t indent = 0;
  while (indent < s.size() && (s[indent] == ' ' || s[indent] == '\t')) ++indent;
  std::string lead = s.substr(0, indent), out;
  // spaces right before a tab in the indent go
  for (size_t i = 0; i < lead.size(); ++i) {
    if (lead[i] == ' ') {
      size_t j = i;
      while (j < lead.size() && lead[j] == ' ') ++j;
      if (j < lead.size() && lead[j] == '\t') { i = j - 1; continue; }
    }
    out += lead[i];
  }
  return out + s.substr(indent) + (had_cr ? "\r" : "");
}

bool same_ignoring_space(const std::string& a, const std::string& b) {
  auto squash = [](const std::string& s) {
    std::string out;
    bool in_space = false;
    for (char ch : s) {
      if (ch == ' ' || ch == '\t' || ch == '\r') { in_space = true; continue; }
      if (in_space && !out.empty()) out += ' ';
      in_space = false;
      out += ch;
    }
    return out;
  };
  return squash(a) == squash(b);
}

// git's apply_one_fragment: each hunk is looked for from where its new-side line number puts it, then one line
// further on, one line back, two on, and so on; a hunk with no leading context must sit at the start of the file (a
// line number of 0 or 1), one with no trailing context at its end, unless --unidiff-zero says its numbers are exact.
bool apply_hunks(const std::string& pre, const std::vector<Hunk>& hunks, const ApplyOpts& ao, std::string& post, long& fail_line,
                 std::vector<size_t>* rejected = nullptr) {
  bool no_eol = false;
  std::vector<std::string> lines = split_keep(pre, no_eol);
  bool ok = true;
  size_t index = 0;
  for (auto& h : hunks) {
    ++index;
    std::vector<std::string> old_l, new_l;
    size_t leading = 0, trailing = 0;
    bool seen_change = false;
    for (auto& hl : h.lines) {
      if (hl[0] == ' ') { if (!seen_change) ++leading; else ++trailing; }
      else { seen_change = true; trailing = 0; }
      if (hl[0] != '+') old_l.push_back(hl.substr(1));
      if (hl[0] != '-') new_l.push_back(ao.fix_ws && hl[0] == '+' ? fix_whitespace(hl.substr(1), ao.keep_cr) : hl.substr(1));
    }
    (void)leading;
    const bool match_beginning = h.old_start == 0 || (h.old_start == 1 && !ao.unidiff_zero);
    const bool match_end = !ao.unidiff_zero && trailing == 0;
    const long n = (long)lines.size(), nold = (long)old_l.size();
    auto matches_at = [&](long pos) {
      if (pos < 0 || pos + nold > n) return false;
      if (match_end && pos + nold != n) return false;
      if (match_beginning && pos != 0) return false;
      for (size_t k = 0; k < old_l.size(); ++k) {
        const std::string& have = lines[(size_t)pos + k];
        if (have != old_l[k] && !(ao.ignore_ws && same_ignoring_space(have, old_l[k]))) return false;
      }
      // the end-of-file state must agree when the hunk reaches the end
      if (h.old_count && pos + nold == n && no_eol != h.old_no_eol) return false;
      return true;
    };
    long line = h.new_start ? h.new_start - 1 : 0;
    if (match_beginning) line = 0;
    else if (match_end) line = n - nold;
    if (line < 0 || line > n) line = n;
    long found = -1;
    long forwards = line, backwards = line, current = line, step = 0;
    for (;;) {
      if (matches_at(current)) { found = current; break; }
      bool exhausted = false;
      for (;;) {
        if (backwards == 0 && forwards == n) { exhausted = true; break; }
        if (step & 1) {
          if (backwards == 0) { ++step; continue; }
          current = --backwards;
          break;
        }
        if (forwards == n) { ++step; continue; }
        current = ++forwards;
        break;
      }
      if (exhausted) break;
      ++step;
    }
    if (found < 0) {
      if (!ao.reject) { fail_line = h.old_start; return false; }
      if (rejected) rejected->push_back(index - 1);
      if (ok) fail_line = h.old_start;
      ok = false;
      continue;
    }
    bool at_end = found + nold == n;
    lines.erase(lines.begin() + found, lines.begin() + found + nold);
    lines.insert(lines.begin() + found, new_l.begin(), new_l.end());
    if (at_end) no_eol = h.new_no_eol;
  }
  post.clear();
  for (size_t k = 0; k < lines.size(); ++k) {
    post += lines[k];
    if (k + 1 < lines.size() || !no_eol) post += "\n";
  }
  return ok;
}

}  // namespace

int cmd_apply(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "binary"}, {0, "check"}, {0, "cached"}, {0, "index"}, {'p', "", Arg::Required},
                                         {'R', "reverse"}, {'v', "verbose"}, {'q', "quiet"}, {0, "stat"}, {0, "numstat"}, {0, "summary"},
                                         {0, "whitespace", Arg::Required}, {0, "reject"}, {'3', "3way"}, {0, "unidiff-zero"},
                                         {0, "allow-empty"}, {0, "recount"}, {0, "directory", Arg::Required}, {0, "exclude", Arg::Required},
                                         {0, "include", Arg::Required}, {0, "no-add"}, {0, "apply"}, {0, "inaccurate-eof"}, {'C', "", Arg::Required},
                                         {0, "ignore-space-change"}, {0, "ignore-whitespace"}, {0, "intent-to-add"}, {'N', ""}, {'z', ""},
                                         {0, "allow-overlap"}, {0, "build-fake-ancestor", Arg::Required}});
  if (p.has("no-add") || p.has("build-fake-ancestor")) unsupported(c, "this apply option");
  std::string whitespace = "warn";
  if (p.has("whitespace")) {
    whitespace = p.get("whitespace");
    if (whitespace != "nowarn" && whitespace != "warn" && whitespace != "error-all" && whitespace != "error" && whitespace != "fix" &&
        whitespace != "strip")
      die(c, "unrecognized whitespace option '" + whitespace + "'");
    if (whitespace == "strip") whitespace = "fix";
  } else if (auto cfg = c.config.get("apply.whitespace")) {
    whitespace = *cfg;
  }
  const bool threeway = p.has("3way");
  bool cached = p.has("cached"), index = p.has("index") || cached || threeway, check = p.has("check"), reverse = p.has("reverse");
  if (threeway && p.has("reject")) die(c, "--3way and --reject cannot be used together");
  if (cached && p.has("index")) die(c, "--cached and --index cannot be used together");
  const bool want_stat = p.has("stat"), want_numstat = p.has("numstat"), want_summary = p.has("summary");
  const bool report_only = want_stat || want_numstat || want_summary || check;
  const bool do_apply = p.has("apply") || !report_only;
  int strip = p.has("p") ? std::atoi(p.get("p").c_str()) : 1;
  bool in_repo = c.try_open_repo();
  if (index && !in_repo) die(c, std::string(threeway ? "--3way" : "--index") + " outside a repository");
  std::string text;
  std::vector<std::string> inputs = p.pos;
  inputs.insert(inputs.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  std::string patch_name = "<stdin>";
  if (inputs.empty() || (inputs.size() == 1 && inputs[0] == "-")) text = read_all(c.in);
  else for (auto& f : inputs) {
    auto r = c.host().read_file(c.abs_path(f));
    if (!r.ok()) { c.err << "error: can't open patch '" << f << "': No such file or directory\n"; return 128; }
    text.append((const char*)r->data(), r->size());
    patch_name = f;
  }
  auto patches = parse_patch(c, text, strip, p.has("recount"));
  if (p.has("directory")) {
    std::string dir = p.get("directory");
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    if (!dir.empty())
      for (auto& fp : patches) {
        if (!fp.old_path.empty()) fp.old_path = dir + "/" + fp.old_path;
        if (!fp.new_path.empty()) fp.new_path = dir + "/" + fp.new_path;
      }
  }
  if (p.has("include") || p.has("exclude")) {
    std::vector<FilePatch> kept;
    for (auto& fp : patches) {
      const std::string& path = fp.new_path.empty() ? fp.old_path : fp.new_path;
      bool skip = false;
      for (auto& pat : p.all("exclude")) if (wildmatch(pat, path, false, false)) skip = true;
      if (p.has("include")) {
        bool any = false;
        for (auto& pat : p.all("include")) if (wildmatch(pat, path, false, false)) any = true;
        if (!any) skip = true;
      }
      if (!skip) kept.push_back(fp);
    }
    patches = kept;
  }
  if (patches.empty()) {
    if (p.has("allow-empty") || p.has("include") || p.has("exclude")) return 0;
    c.err << "error: No valid patches in input (allow with \"--allow-empty\")\n";
    return 128;
  }
  if (reverse) {
    for (auto& fp : patches) {
      std::swap(fp.old_path, fp.new_path);
      std::swap(fp.old_mode, fp.new_mode);
      std::swap(fp.is_new, fp.is_delete);
      std::swap(fp.forward, fp.reverse);
      std::swap(fp.old_oid, fp.new_oid);
      for (auto& h : fp.hunks) {
        std::swap(h.old_start, h.new_start);
        std::swap(h.old_count, h.new_count);
        std::swap(h.old_no_eol, h.new_no_eol);
        // '-' and '+' change places, and a '+' run must come after the '-' run it replaces
        std::vector<std::string> out, minus, plus;
        auto flush = [&]() { for (auto& m : minus) out.push_back(m); for (auto& pl : plus) out.push_back(pl); minus.clear(); plus.clear(); };
        for (auto& l : h.lines) {
          if (l[0] == ' ') { flush(); out.push_back(l); }
          else if (l[0] == '+') minus.push_back("-" + l.substr(1));
          else plus.push_back("+" + l.substr(1));
        }
        flush();
        h.lines = out;
      }
    }
  }
  // what the patch says about itself: printed once it is known to fit (at once when that is not asked)
  auto report = [&]() {
    if (!(want_stat || want_numstat || want_summary)) return;
    std::vector<DiffStatEntry> stats;
    for (auto& fp : patches) {
      DiffStatEntry s;
      const std::string& old_name = fp.old_path.empty() ? fp.new_path : fp.old_path;
      const std::string& new_name = fp.new_path.empty() ? fp.old_path : fp.new_path;
      (void)old_name;
      s.name = quote_path(new_name);
      if (fp.binary) {
        s.binary = true;
      } else {
        for (auto& h : fp.hunks)
          for (auto& l : h.lines) {
            if (l[0] == '+') ++s.add;
            else if (l[0] == '-') ++s.del;
          }
      }
      s.interesting = true;
      stats.push_back(s);
    }
    if (want_stat) {
      // apply lays its own diffstat out: names padded to the longest (50 at most), a count in five columns, a graph that
      // keeps the line within 70 columns
      size_t max_len = 0;
      long max_change = 0, total_add = 0, total_del = 0;
      for (auto& st : stats) {
        max_len = std::max(max_len, st.name.size());
        max_change = std::max(max_change, st.add + st.del);
        total_add += st.add;
        total_del += st.del;
      }
      for (auto& st : stats) {
        long width = (long)std::min<size_t>(max_len, 50);
        std::string name = st.name;
        if ((long)name.size() > width) {
          size_t from = name.size() + 3 - (size_t)width;
          size_t slash = name.find('/', from);
          name = "..." + name.substr(slash == std::string::npos ? from : slash);
        }
        c.out << " " << name << std::string((size_t)std::max<long>(0, width - (long)name.size()), ' ') << " |";
        if (st.binary) { c.out << "  Bin\n"; continue; }
        long room = width + max_change > 70 ? 70 - width : max_change;
        long add = st.add, del = st.del;
        if (max_change > 0) {
          long total = ((add + del) * room + max_change / 2) / max_change;
          add = (add * room + max_change / 2) / max_change;
          del = total - add;
        }
        char count[16];
        std::snprintf(count, sizeof count, "%5ld", st.add + st.del);
        c.out << count << " " << std::string((size_t)std::max<long>(0, add), '+') << std::string((size_t)std::max<long>(0, del), '-') << "\n";
      }
      c.out << " " << stats.size() << (stats.size() == 1 ? " file changed" : " files changed");
      if (total_add || total_del == 0) c.out << ", " << total_add << (total_add == 1 ? " insertion(+)" : " insertions(+)");
      if (total_del || total_add == 0) c.out << ", " << total_del << (total_del == 1 ? " deletion(-)" : " deletions(-)");
      c.out << "\n";
    }
    if (want_numstat) {
      for (size_t i = 0; i < stats.size(); ++i) {
        const auto& fp = patches[i];
        if (stats[i].binary) c.out << "-\t-\t";
        else c.out << stats[i].add << "\t" << stats[i].del << "\t";
        const std::string& new_name = fp.new_path.empty() ? fp.old_path : fp.new_path;
        if (p.has("z")) c.out << new_name << '\0';
        else c.out << stats[i].name << "\n";
      }
    }
    if (want_summary) {
      auto mode_str = [](uint32_t m) { char b[16]; std::snprintf(b, sizeof b, "%06o", m); return std::string(b); };
      for (auto& fp : patches) {
        if (fp.is_new) c.out << " create mode " << mode_str(fp.new_mode ? fp.new_mode : 0100644) << " " << quote_path(fp.new_path) << "\n";
        else if (fp.is_delete) c.out << " delete mode " << mode_str(fp.old_mode ? fp.old_mode : 0100644) << " " << quote_path(fp.old_path) << "\n";
        else if (fp.is_rename || fp.is_copy) {
          c.out << (fp.is_rename ? " rename " : " copy ") << rename_display(fp.old_path, fp.new_path) << " (" << fp.similarity << "%)\n";
          if (fp.old_mode && fp.new_mode && fp.old_mode != fp.new_mode)
            c.out << " mode change " << mode_str(fp.old_mode) << " => " << mode_str(fp.new_mode) << " " << quote_path(fp.new_path) << "\n";
        } else if (fp.old_mode && fp.new_mode && fp.old_mode != fp.new_mode) {
          c.out << " mode change " << mode_str(fp.old_mode) << " => " << mode_str(fp.new_mode) << " " << quote_path(fp.new_path) << "\n";
        }
      }
    }
  };
  // --stat, --numstat and --summary alone only read the patch: whether it fits is not asked
  if (!do_apply && !check) { report(); return 0; }
  // where files are read from and written to
  std::string base = in_repo && !c.top.empty() ? c.top : c.cwd;
  std::map<std::string, const GitIndexEntry*> idx;
  if (in_repo) for (auto& e : c.repo->index()) idx[e.path] = &e;
  struct Result1 {
    std::string path;
    bool remove = false;
    std::string content;
    uint32_t mode = 0100644;
    bool conflict = false;                    // --3way left markers: stages 1-3 go in the index
    std::string base_oid, ours_oid, theirs_oid;
    uint32_t ours_mode = 0100644;
    std::string reject_text;                  // --reject: the hunks that did not fit
  };
  std::vector<Result1> results;
  std::map<std::string, std::string> pending;   // later patches in the same input see earlier results
  std::set<std::string> deleted;
  bool failed = false;
  ApplyOpts ao;
  ao.unidiff_zero = p.has("unidiff-zero");
  ao.ignore_ws = p.has("ignore-space-change") || p.has("ignore-whitespace");
  ao.fix_ws = whitespace == "fix";
  ao.reject = p.has("reject");
  if (reverse) std::reverse(patches.begin(), patches.end());
  // whitespace the patch adds: warned about, refused (error) or removed (fix)
  long ws_errors = 0;
  if (whitespace == "warn" || whitespace == "error" || whitespace == "error-all") {
    for (auto& fp : patches) {
      if (fp.binary) continue;
      bool crlf_in_old = false;
      for (auto& h : fp.hunks)
        for (auto& l : h.lines) if (l[0] != '+' && !l.empty() && l.back() == '\r') crlf_in_old = true;
      for (auto& h : fp.hunks) {
        size_t line_no = h.line_no;
        for (auto& l : h.lines) {
          ++line_no;
          if (l[0] != '+') continue;
          std::string body = l.substr(1);
          std::string why;
          if (!body.empty() && (body.back() == ' ' || body.back() == '\t' || (body.back() == '\r' && !crlf_in_old))) why = "trailing whitespace";
          size_t ind = 0;
          while (ind < body.size() && body[ind] == ' ') ++ind;
          if (ind > 0 && ind < body.size() && body[ind] == '\t') why += (why.empty() ? "" : ", ") + std::string("space before tab in indent");
          if (why.empty()) continue;
          ++ws_errors;
          if (ws_errors <= 5) c.err << patch_name << ":" << line_no << ": " << why << ".\n+" << body << "\n";
        }
      }
    }
    if (ws_errors > 5) c.err << "warning: squelched " << ws_errors - 5 << " whitespace error" << (ws_errors - 5 == 1 ? "" : "s") << "\n";
    if (ws_errors) {
      if (whitespace == "warn") c.err << "warning: " << ws_errors << " line" << (ws_errors == 1 ? "" : "s") << " add" << (ws_errors == 1 ? "s" : "") << " whitespace errors.\n";
      else { c.err << "error: " << ws_errors << " line" << (ws_errors == 1 ? "" : "s") << " add" << (ws_errors == 1 ? "s" : "") << " whitespace errors.\n"; return 1; }
    }
  }
  for (auto& fp : patches) {
    std::string op = fp.old_path, np = fp.new_path;
    if (op.empty()) op = np;
    if (np.empty()) np = op;
    std::string pre;
    bool exists = false;
    uint32_t cur_mode = 0100644;
    std::string pre_oid;
    if (!fp.is_new) {
      if (pending.count(op)) { pre = pending[op]; exists = true; }
      else if (deleted.count(op)) exists = false;
      else if (cached) {
        auto it = idx.find(op);
        if (it != idx.end()) { pre = blob_text(c, it->second->oid); exists = true; cur_mode = it->second->mode; pre_oid = it->second->oid; }
      } else {
        auto r = c.host().read_file(join(base, op));
        if (r.ok()) { pre.assign((const char*)r->data(), r->size()); exists = true; }
        if (index) {
          auto it = idx.find(op);
          if (it == idx.end()) { c.err << "error: " << op << ": does not exist in index\n"; failed = true; continue; }
          cur_mode = it->second->mode;
          pre_oid = it->second->oid;
          if (exists && hash_blob(pre) != it->second->oid) { c.err << "error: " << op << ": does not match index\n"; failed = true; continue; }
        }
      }
      if (!exists) { c.err << "error: " << op << ": No such file or directory\n"; failed = true; continue; }
    } else {
      bool in_index = index && idx.count(np) > 0 && !deleted.count(np);
      bool on_disk = !cached && c.host().exists(join(base, np)) && !deleted.count(np);
      if (in_index || on_disk || pending.count(np)) {
        c.err << "error: " << np << ": already exists in " << (in_index ? "index" : "working directory") << "\n";
        failed = true;
        continue;
      }
    }
    std::string post;
    std::string reject_text;
    bool conflict = false;
    std::string base_oid, theirs_oid;
    if (fp.binary) {
      if (fp.binary_without_data) {
        c.err << "error: cannot apply binary patch to '" << np << "' without full index line\nerror: " << np << ": patch does not apply\n";
        failed = true;
        continue;
      }
      if (fp.forward.kind == "literal") post = fp.forward.data;
      else if (fp.forward.kind == "delta") {
        auto r = git_apply_delta(ByteSpan((const uint8_t*)pre.data(), pre.size()), ByteSpan((const uint8_t*)fp.forward.data.data(), fp.forward.data.size()));
        if (!r.ok()) { c.err << "error: binary patch does not apply to '" << np << "'\nerror: " << np << ": patch does not apply\n"; failed = true; continue; }
        post.assign((const char*)r->data(), r->size());
      } else if (!fp.is_delete) die(c, "binary patch without data for " + np);
      if (fp.forward.kind == "literal" && post.size() != fp.forward.size) die(c, "binary patch for " + np + " has the wrong size");
    } else {
      long fail = 0;
      std::vector<size_t> rejected;
      ApplyOpts file_ao = ao;
      for (auto& hk : fp.hunks)
        for (auto& l : hk.lines) if (l[0] != '+' && !l.empty() && l.back() == '\r') file_ao.keep_cr = true;
      if (!apply_hunks(pre, fp.hunks, file_ao, post, fail, &rejected)) {
        if (ao.reject && !rejected.empty()) {
          c.err << "Applying patch " << np << " with " << rejected.size() << " reject" << (rejected.size() == 1 ? "" : "s") << "...\n";
          std::ostringstream rej;
          rej << "diff a/" << np << " b/" << np << "\t(rejected hunks)\n";
          for (size_t k : rejected) {
            const Hunk& h = fp.hunks[k];
            auto range = [](long s, long cnt) { return std::to_string(s) + (cnt == 1 ? "" : "," + std::to_string(cnt)); };
            rej << "@@ -" << range(h.old_start, h.old_count) << " +" << range(h.new_start, h.new_count) << " @@\n";
            for (auto& l : h.lines) rej << l << "\n";
            c.err << "Rejected hunk #" << k + 1 << ".\n";
          }
          reject_text = rej.str();
          failed = true;
        } else if (threeway) {
          // the preimage the patch was made against, from its "index" line
          const std::string& want_old = fp.old_oid;
          const bool named = !want_old.empty() && want_old.find_first_not_of('0') != std::string::npos;
          auto object = named ? c.repo->read_object(want_old) : Result<GitObject>(Status::NotFound, "no preimage named");
          if (fp.is_new || !object.ok()) {
            c.err << "error: patch failed: " << op << ":" << fail << "\nerror: repository lacks the necessary blob to perform 3-way merge.\nerror: " << op << ": patch does not apply\n";
            failed = true;
            continue;
          }
          std::string base_text((const char*)object->payload.data(), object->payload.size()), theirs_text;
          long f2 = 0;
          if (!apply_hunks(base_text, fp.hunks, ApplyOpts{}, theirs_text, f2)) {
            c.err << "error: patch failed: " << op << ":" << fail << "\nerror: " << op << ": patch does not apply\n";
            failed = true;
            continue;
          }
          c.err << "error: patch failed: " << op << ":" << fail << "\nFalling back to three-way merge...\n";
          GitMerge3Options mo;
          auto merged = git_merge3(base_text, pre, theirs_text, mo);
          post = merged.merged;
          conflict = !merged.clean;
          base_oid = object->oid;
          theirs_oid = hash_blob(theirs_text);
          c.err << "Applied patch to '" << np << "' " << (conflict ? "with conflicts" : "cleanly") << ".\n";
          if (conflict) failed = true;
        } else {
          c.err << "error: patch failed: " << op << ":" << fail << "\nerror: " << op << ": patch does not apply\n";
          failed = true;
          continue;
        }
      }
    }
    if (fp.is_delete) {
      if (!post.empty() && !fp.binary) { c.err << "error: removal patch leaves file contents\nerror: " << op << ": patch does not apply\n"; failed = true; continue; }
      Result1 r;
      r.path = op;
      r.remove = true;
      results.push_back(r);
      pending.erase(op);
      deleted.insert(op);
      continue;
    }
    uint32_t mode = fp.new_mode ? fp.new_mode : (fp.old_mode ? fp.old_mode : cur_mode);
    if (fp.is_rename && op != np) {
      Result1 r;
      r.path = op;
      r.remove = true;
      results.push_back(r);
      deleted.insert(op);
      pending.erase(op);
    }
    Result1 r;
    r.path = np;
    r.content = post;
    r.mode = mode;
    r.conflict = conflict;
    r.base_oid = base_oid;
    r.theirs_oid = theirs_oid;
    r.ours_oid = pre_oid;
    r.ours_mode = cur_mode;
    r.reject_text = reject_text;
    results.push_back(r);
    pending[np] = post;
    deleted.erase(np);
    if (p.has("verbose")) c.err << "Checking patch " << np << "...\n";
  }
  if (failed && !ao.reject && !threeway) return 1;
  report();
  if (!do_apply || check) return failed ? 1 : 0;
  auto& h = c.host();
  std::vector<GitIndexEntry> next = in_repo ? c.repo->index() : std::vector<GitIndexEntry>{};
  std::vector<GitIndexEntry> unmerged = in_repo ? c.repo->unmerged() : std::vector<GitIndexEntry>{};
  for (auto& r : results) {
    if (!cached) {
      std::string full = join(base, r.path);
      if (r.remove) {
        (void)h.remove(full);
        for (std::string d = dirname_of(r.path); !d.empty(); d = dirname_of(d)) {
          auto names = h.list_dir(join(base, d));
          bool empty = names.ok();
          if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") empty = false;
          if (!empty || !h.remove(join(base, d)).ok()) break;
        }
      } else {
        (void)h.make_dir(dirname_of(full));
        auto w = h.write_file(full, ByteSpan((const uint8_t*)r.content.data(), r.content.size()));
        if (!w.ok()) die(c, "unable to write file '" + r.path + "': " + w.error().message);
        if (r.mode == 0100755) h.set_access(full, hal::Host::FileAccess::Executable);
        if (!r.reject_text.empty()) {
          std::string rej_path = full + ".rej";
          (void)h.write_file(rej_path, ByteSpan((const uint8_t*)r.reject_text.data(), r.reject_text.size()));
        }
      }
    }
    if (index) {
      next.erase(std::remove_if(next.begin(), next.end(), [&](const GitIndexEntry& e) { return e.path == r.path; }), next.end());
      if (r.conflict) {
        // the three sides of the merge, as stages 1 (base), 2 (the file as it was) and 3 (the patch applied to the base)
        auto stage = [&](uint8_t n, const std::string& oid, uint32_t mode) {
          if (oid.empty()) return;
          GitIndexEntry e{r.path, oid, mode, 0, 0};
          e.stage = n;
          unmerged.push_back(e);
        };
        stage(1, r.base_oid, r.ours_mode);
        stage(2, r.ours_oid, r.ours_mode);
        stage(3, r.theirs_oid, r.mode);
      } else if (!r.remove) {
        auto w = c.repo->write_object("blob", ByteSpan((const uint8_t*)r.content.data(), r.content.size()));
        if (!w.ok()) die(c, w.error().message);
        GitIndexEntry e{r.path, *w, r.mode, r.content.size(), 0};
        if (!cached) { auto st = h.stat(join(base, r.path)); if (st.ok()) e.mtime_ns = st->mtime_ns; }
        if (p.has("intent-to-add") || p.has("N")) {
          if (!idx.count(r.path)) {
            auto empty = c.repo->write_object("blob", ByteSpan{});
            if (empty.ok()) { e.oid = *empty; e.size = 0; e.intent_to_add = true; }
          }
        }
        next.push_back(e);
      }
    }
    if (p.has("verbose")) c.err << "Applied patch " << r.path << (r.conflict ? " with conflicts.\n" : " cleanly.\n");
    if (r.conflict) c.out << "U " << r.path << "\n";
  }
  if (index) {
    c.repo->set_index(next);
    c.repo->set_unmerged(unmerged);
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
  }
  return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// archive
// ---------------------------------------------------------------------------
namespace {
void tar_octal(char* field, size_t width, uint64_t v) {
  std::string s;
  do { s.insert(s.begin(), (char)('0' + (v & 7))); v >>= 3; } while (v);
  while (s.size() < width - 1) s.insert(s.begin(), '0');
  std::memcpy(field, s.data(), std::min(s.size(), width - 1));
  field[width - 1] = 0;
}
void tar_header(std::string& out, const std::string& name, char type, uint32_t mode, uint64_t size, uint64_t mtime,
                const std::string& link = "") {
  std::string path = name;
  std::string pax;
  auto pax_rec = [&](const std::string& key, const std::string& val) {
    std::string body = " " + key + "=" + val + "\n";
    size_t len = body.size();
    size_t total = len + std::to_string(len).size();
    if (std::to_string(total).size() != std::to_string(len).size()) total = len + std::to_string(total).size();
    pax += std::to_string(total) + body;
  };
  char prefix[156] = {0};
  std::string short_name = path;
  if (path.size() > 100) {
    // ustar's prefix field holds up to 155 bytes before a '/'
    size_t split = path.rfind('/', 155);
    if (split != std::string::npos && split <= 155 && path.size() - split - 1 <= 100 && split > 0) {
      std::memcpy(prefix, path.data(), split);
      short_name = path.substr(split + 1);
    } else {
      pax_rec("path", path);
      short_name = path.substr(0, 100);
    }
  }
  if (link.size() > 100) pax_rec("linkpath", link);
  if (!pax.empty()) {
    tar_header(out, "pax_header/" + std::to_string(pax.size()), 'x', 0644, pax.size(), mtime);
    out += pax;
    out.append((512 - pax.size() % 512) % 512, '\0');
  }
  char h[512];
  std::memset(h, 0, sizeof h);
  std::memcpy(h, short_name.data(), std::min<size_t>(short_name.size(), 100));
  tar_octal(h + 100, 8, mode);
  tar_octal(h + 108, 8, 0);
  tar_octal(h + 116, 8, 0);
  tar_octal(h + 124, 12, size);
  tar_octal(h + 136, 12, mtime);
  std::memset(h + 148, ' ', 8);
  h[156] = type;
  std::memcpy(h + 157, link.data(), std::min<size_t>(link.size(), 100));
  std::memcpy(h + 257, "ustar", 6);
  std::memcpy(h + 263, "00", 2);
  std::memcpy(h + 265, "root", 4);
  std::memcpy(h + 297, "root", 4);
  std::memcpy(h + 345, prefix, 155);
  unsigned sum = 0;
  for (int i = 0; i < 512; ++i) sum += (unsigned char)h[i];
  char ck[8];
  std::snprintf(ck, sizeof ck, "%06o", sum);
  std::memcpy(h + 148, ck, 6);
  h[154] = 0;
  h[155] = ' ';
  out.append(h, 512);
}
}  // namespace

int cmd_archive(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "format", Arg::Required}, {0, "prefix", Arg::Required}, {'o', "output", Arg::Required},
                                         {'v', "verbose"}, {0, "worktree-attributes"}, {0, "remote", Arg::Required}, {'l', "list"},
                                         {0, "add-file", Arg::Required}, {0, "add-virtual-file", Arg::Required}, {0, "mtime", Arg::Required},
                                         {'0', ""}, {'9', ""}});
  if (p.has("list")) { c.out << "tar\n"; return 0; }
  if (p.has("remote") || p.has("add-file") || p.has("add-virtual-file") || p.has("worktree-attributes")) unsupported(c, "archive --remote/--add-file");
  std::string fmt = p.get("format", "tar");
  if (p.has("output") && !p.has("format")) {
    std::string o = p.get("output");
    if (o.size() > 4 && o.compare(o.size() - 4, 4, ".zip") == 0) fmt = "zip";
    else if (o.size() > 7 && o.compare(o.size() - 7, 7, ".tar.gz") == 0) fmt = "tar.gz";
    else if (o.size() > 4 && o.compare(o.size() - 4, 4, ".tgz") == 0) fmt = "tgz";
  }
  if (fmt != "tar") unsupported(c, "archive --format=" + fmt + " (tar only)");
  c.open_repo(false);
  if (p.pos.empty()) usage(c, "usage: git archive [<options>] <tree-ish> [<path>...]");
  std::string spec = p.pos[0];
  std::string oid = must_resolve(c, spec);
  std::string commit = peel_to(c, oid, "commit");
  std::string tree = peel_to(c, oid, "tree");
  if (tree.empty()) die(c, "not a tree object: " + spec);
  uint64_t mtime = commit.empty() ? (uint64_t)now_seconds() : read_commit(c, commit).committer_time;
  if (p.has("mtime")) { int64_t t; std::string tz; if (!parse_date(p.get("mtime"), t, tz)) die(c, "invalid --mtime"); mtime = (uint64_t)t; }
  std::string prefix = p.get("prefix");
  std::vector<std::string> paths(p.pos.begin() + 1, p.pos.end());
  long umask = c.config.get_int("tar.umask", 002);
  std::string out;
  // To standard output the tar goes out a file at a time once a stretch of it has built up, so a reader that stops
  // (`git archive HEAD | tar t | head`) stops the walk; to --output it is written whole, replacing the file at once.
  const bool to_stdout = !p.has("output");
  uint64_t spilled = 0;
  auto spill = [&]() {
    if (!to_stdout || out.size() < (1u << 20)) return;
    c.out.write(out.data(), (std::streamsize)out.size());
    spilled += out.size();
    out.clear();
  };
  if (!commit.empty()) {
    std::string body = "52 comment=" + commit + "\n";
    tar_header(out, "pax_global_header", 'g', 0666, body.size(), mtime);
    out += body;
    out.append((512 - body.size() % 512) % 512, '\0');
  }
  if (!prefix.empty() && prefix.back() == '/') tar_header(out, prefix, '5', (uint32_t)(0777 & ~umask), 0, mtime);
  std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& toid, const std::string& base) {
    auto ents = c.repo->read_tree(toid);
    if (!ents.ok()) die(c, ents.error().message);
    for (auto& e : *ents) {
      std::string path = base.empty() ? e.name : base + "/" + e.name;
      bool selected = paths.empty();
      bool leads = false;
      for (auto& sp : paths) {
        std::string s = sp;
        while (!s.empty() && s.back() == '/') s.pop_back();
        if (path == s || path.rfind(s + "/", 0) == 0) selected = true;
        if (s.rfind(path + "/", 0) == 0) leads = true;
      }
      if (!selected && !leads) continue;
      if (e.is_dir()) {
        if (selected) tar_header(out, prefix + path + "/", '5', (uint32_t)(0777 & ~umask), 0, mtime);
        walk(e.oid, path);
        continue;
      }
      if (!selected) continue;
      if (e.mode == 0160000) { tar_header(out, prefix + path + "/", '5', (uint32_t)(0777 & ~umask), 0, mtime); continue; }
      std::string data = blob_text(c, e.oid);
      if (e.mode == 0120000) { tar_header(out, prefix + path, '2', 0777, 0, mtime, data); continue; }
      uint32_t mode = (uint32_t)((e.mode == 0100755 ? 0777 : 0666) & ~umask);
      tar_header(out, prefix + path, '0', mode, data.size(), mtime);
      out += data;
      out.append((512 - data.size() % 512) % 512, '\0');
      spill();
    }
  };
  walk(tree, "");
  out.append(1024, '\0');
  out.append((10240 - (spilled + out.size()) % 10240) % 10240, '\0');
  if (!to_stdout) {
    auto w = c.host().write_file(c.abs_path(p.get("output")), ByteSpan((const uint8_t*)out.data(), out.size()));
    if (!w.ok()) die(c, "could not create archive file '" + p.get("output") + "': " + w.error().message);
  } else {
    c.out.write(out.data(), (std::streamsize)out.size());
  }
  return 0;
}

// ---------------------------------------------------------------------------
// worktree
// ---------------------------------------------------------------------------
int cmd_worktree(Ctx& c) {
  if (c.args.size() < 2) usage(c, "usage: git worktree (add|list|remove|prune) ...");
  std::string sub = c.args[1];
  std::vector<std::string> sargs{"worktree"};
  sargs.insert(sargs.end(), c.args.begin() + 2, c.args.end());
  c.open_repo(false);
  auto& h = c.host();
  std::string common = c.repo->common_dir();
  std::string admin_root = common + "/worktrees";
  auto rmtree = [&](const std::string& path) {
    std::function<void(const std::string&)> rm = [&](const std::string& p2) {
      auto st = h.stat(p2);
      if (!st.ok()) return;
      if (st->is_dir && !st->is_link) {
        auto ns = h.list_dir(p2);
        if (ns.ok()) for (auto& n : *ns) if (n != "." && n != "..") rm(p2 + "/" + n);
      }
      (void)h.remove(p2);
    };
    rm(path);
  };
  auto list_trees = [&]() {
    std::vector<std::pair<std::string, std::string>> out;   // path, admin dir
    std::string main_top = dirname_of(common);
    out.push_back({c.repo->bare() && common.size() && !h.exists(main_top + "/.git") ? common : main_top, ""});
    auto names = h.list_dir(admin_root);
    if (names.ok()) {
      std::vector<std::string> sorted = *names;
      std::sort(sorted.begin(), sorted.end());
      for (auto& n : sorted) {
        if (n == "." || n == "..") continue;
        auto g = h.read_file(admin_root + "/" + n + "/gitdir");
        if (!g.ok()) continue;
        std::string gd((const char*)g->data(), g->size());
        while (!gd.empty() && (gd.back() == '\n' || gd.back() == '\r')) gd.pop_back();
        out.push_back({dirname_of(slashes(gd)), admin_root + "/" + n});
      }
    }
    return out;
  };
  if (sub == "list") {
    auto p = parse_options(c, sargs, 1, {{0, "porcelain"}, {'v', "verbose"}, {'z', ""}, {0, "expire", Arg::Required}});
    auto trees = list_trees();
    size_t width = 0;
    for (auto& [path, admin] : trees) width = std::max(width, path.size());
    for (auto& [path, admin] : trees) {
      std::string head_file = admin.empty() ? c.repo->git_dir() + "/HEAD" : admin + "/HEAD";
      if (admin.empty()) head_file = common + "/HEAD";
      auto hf = h.read_file(head_file);
      std::string head((const char*)(hf.ok() ? hf->data() : nullptr), hf.ok() ? hf->size() : 0);
      while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) head.pop_back();
      std::string ref = head.rfind("ref: ", 0) == 0 ? head.substr(5) : "";
      std::string oid = ref.empty() ? head : read_ref(c, ref).value_or("");
      bool is_bare = admin.empty() && c.repo->bare() && path == common;
      if (p.has("porcelain")) {
        c.out << "worktree " << path << "\n";
        if (is_bare) c.out << "bare\n";
        else {
          c.out << "HEAD " << (oid.empty() ? std::string(40, '0') : oid) << "\n";
          if (ref.empty()) c.out << "detached\n";
          else c.out << "branch " << ref << "\n";
        }
        c.out << "\n";
      } else {
        c.out << path << std::string(width - path.size() + 2, ' ');
        if (is_bare) c.out << "(bare)\n";
        else c.out << (oid.empty() ? std::string(7, '0') : c.abbrev(oid)) << " " << (ref.empty() ? "(detached HEAD)" : "[" + (ref.rfind("refs/heads/", 0) == 0 ? ref.substr(11) : ref) + "]") << "\n";
      }
    }
    return 0;
  }
  if (sub == "add") {
    auto p = parse_options(c, sargs, 1, {{'f', "force"}, {0, "detach"}, {'b', "", Arg::Required}, {'B', "", Arg::Required},
                                          {0, "checkout"}, {0, "no-checkout"}, {0, "lock"}, {0, "reason", Arg::Required}, {'q', "quiet"},
                                          {0, "track"}, {0, "no-track"}, {0, "guess-remote"}, {0, "orphan"}, {0, "relative-paths"}});
    if (p.has("orphan") || p.has("lock")) unsupported(c, "worktree add --orphan/--lock");
    if (p.pos.empty()) usage(c, "usage: git worktree add [<options>] <path> [<commit-ish>]");
    std::string path = c.abs_path(p.pos[0]);
    std::string commitish = p.pos.size() > 1 ? p.pos[1] : "HEAD";
    if (h.exists(path)) {
      auto names = h.list_dir(path);
      bool empty = names.ok();
      if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") empty = false;
      if (!empty) die(c, "'" + p.pos[0] + "' already exists");
    }
    std::string new_branch = p.has("b") ? p.get("b") : p.get("B");
    std::string branch;
    std::string oid;
    // With the change layer on, HEAD is this workspace's @: the change still being edited. A work tree started on it
    // was rebased every time @ changed and carried @'s edits as its own base; it starts on @'s parent instead (as a
    // jj workspace does), on what was last committed.
    auto start_commit = [&](const std::string& spec) {
      std::string o = commit_of(c, spec);
      if (spec == "HEAD" && change::enabled(c) && o == change::wc_commit(c)) {
        const auto& parents = read_commit(c, o).parents;
        if (!parents.empty()) o = parents[0];
      }
      return o;
    };
    if (!new_branch.empty()) {
      if (read_ref(c, "refs/heads/" + new_branch) && !p.has("B")) die(c, "a branch named '" + new_branch + "' already exists");
      oid = start_commit(commitish);
      update_ref_checked(c, "refs/heads/" + new_branch, oid, std::nullopt, "branch: Created from " + commitish);
      branch = new_branch;
    } else if (!p.has("detach") && p.pos.size() > 1 && read_ref(c, "refs/heads/" + commitish)) {
      branch = commitish;
      oid = *read_ref(c, "refs/heads/" + commitish);
      // a branch checked out elsewhere is refused
      for (auto& [wpath, admin] : list_trees()) {
        std::string hfile = admin.empty() ? common + "/HEAD" : admin + "/HEAD";
        auto hf = h.read_file(hfile);
        std::string hs = hf.ok() ? std::string((const char*)hf->data(), hf->size()) : "";
        if (hs.find("refs/heads/" + branch + "\n") != std::string::npos && !p.has("force"))
          die(c, "'" + branch + "' is already used by worktree at '" + wpath + "'");
      }
    } else if (!p.has("detach") && p.pos.size() == 1) {
      // "git worktree add <path>": a new branch named after the directory
      branch = basename_of(path);
      if (read_ref(c, "refs/heads/" + branch)) {
        oid = *read_ref(c, "refs/heads/" + branch);
      } else {
        oid = start_commit("HEAD");
        update_ref_checked(c, "refs/heads/" + branch, oid, std::nullopt, "branch: Created from HEAD");
      }
    } else {
      oid = start_commit(commitish);
    }
    std::string name = basename_of(path);
    std::string admin = admin_root + "/" + name;
    for (int n = 1; h.exists(admin); ++n) admin = admin_root + "/" + name + std::to_string(n);
    (void)h.make_dir(admin);
    (void)h.make_dir(path);
    auto wf = [&](const std::string& f, const std::string& body) {
      auto r = h.write_file(f, ByteSpan((const uint8_t*)body.data(), body.size()));
      if (!r.ok()) die(c, "could not write '" + f + "': " + r.error().message);
    };
    wf(admin + "/gitdir", path + "/.git\n");
    wf(admin + "/commondir", "../..\n");
    wf(admin + "/HEAD", branch.empty() ? oid + "\n" : "ref: refs/heads/" + branch + "\n");
    wf(path + "/.git", "gitdir: " + admin + "\n");
    if (!p.has("quiet")) {
      if (branch.empty()) c.err << "Preparing worktree (detached HEAD " << c.abbrev(oid) << ")\n";
      else if (!new_branch.empty() || p.pos.size() == 1) c.err << "Preparing worktree (new branch '" << branch << "')\n";
      else c.err << "Preparing worktree (checking out '" << branch << "')\n";
    }
    if (!p.has("no-checkout")) {
      Git wt(*c.kernel);
      auto o = wt.open(path);
      if (!o.ok()) die(c, o.error().message);
      wt.set_settings(c.repo->settings());
      auto co = wt.checkout_tree(read_commit(c, oid).tree, true);
      if (!co.ok()) die(c, co.error().message);
      if (!p.has("quiet")) c.out << "HEAD is now at " << c.abbrev(oid) << " " << subject_of(read_commit(c, oid).message) << "\n";
    }
    return 0;
  }
  if (sub == "remove") {
    auto p = parse_options(c, sargs, 1, {{'f', "force"}});
    if (p.pos.size() != 1) usage(c, "usage: git worktree remove [<options>] <worktree>");
    std::string path = c.abs_path(p.pos[0]);
    for (auto& [wpath, admin] : list_trees()) {
      if (admin.empty()) continue;
      if (wpath != path && basename_of(wpath) != p.pos[0]) continue;
      if (!p.has("force")) {
        Git wt(*c.kernel);
        if (wt.open(wpath).ok()) {
          wt.set_settings(c.repo->settings());
          auto st = wt.status();
          if (st.ok() && (!st->staged.empty() || !st->worktree.empty() || !st->untracked.empty()))
            die(c, "'" + p.pos[0] + "' contains modified or untracked files, use --force to delete it");
        }
      }
      rmtree(wpath);
      rmtree(admin);
      return 0;
    }
    die(c, "'" + p.pos[0] + "' is not a working tree");
  }
  if (sub == "prune") {
    auto p = parse_options(c, sargs, 1, {{'n', "dry-run"}, {'v', "verbose"}, {0, "expire", Arg::Required}});
    for (auto& [wpath, admin] : list_trees()) {
      if (admin.empty() || h.exists(wpath + "/.git")) continue;
      if (p.has("verbose") || p.has("dry-run")) c.out << "Removing worktrees/" << basename_of(admin) << ": gitdir file points to non-existent location\n";
      if (!p.has("dry-run")) rmtree(admin);
    }
    return 0;
  }
  unsupported(c, "worktree " + sub);
}

// ---------------------------------------------------------------------------
// grep
// ---------------------------------------------------------------------------
namespace {
// GNU BRE (\| \+ \? \( \) \{ \} special, the bare characters literal) as an ERE.
std::string bre_to_ere(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    char ch = in[i];
    if (ch == '\\' && i + 1 < in.size()) {
      char n = in[++i];
      if (n == '(' || n == ')' || n == '{' || n == '}' || n == '|' || n == '+' || n == '?') out += n;
      else { out += '\\'; out += n; }
      continue;
    }
    if (ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == '|' || ch == '+' || ch == '?') { out += '\\'; out += ch; continue; }
    out += ch;
  }
  return out;
}
// A pattern with nothing special in it (any -F pattern, or a regex of plain letters, digits and _-:/ etc.): searched
// as a substring. std::regex on every line of every file took 20 s for 15 names in a 3,500-file repository, where git
// takes 0.1 s; most patterns (names in code) are plain.
bool plain_pattern(const std::string& ere) {
  for (char ch : ere) if (std::strchr(".^$|()[]{}*+?\\", ch)) return false;
  return true;
}
char lower_ascii(char ch) { return (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch; }
bool word_char(char ch) { return std::isalnum((unsigned char)ch) || ch == '_'; }
struct Matcher {
  bool plain = false, word = false, icase = false;
  std::string needle;  // plain: the text (lowercased when icase)
  std::regex re;
  // Each match in s as [begin, end): plain ones by substring, the rest by the regex.
  template <class F> void each(const std::string& s, F&& f) const {
    if (!plain) {
      for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
        if (word) { size_t b = it->position(2), n = it->length(2); if (!f(b, b + n)) return; }
        else if (!f(size_t(it->position()), size_t(it->position() + it->length()))) return;
      }
      return;
    }
    if (needle.empty()) { f(0, 0); return; }
    std::string low;
    const std::string* hay = &s;
    if (icase) { low.resize(s.size()); for (size_t i = 0; i < s.size(); ++i) low[i] = lower_ascii(s[i]); hay = &low; }
    for (size_t at = hay->find(needle); at != std::string::npos; at = hay->find(needle, at + 1)) {
      size_t end = at + needle.size();
      if (word && ((at > 0 && word_char(s[at - 1])) || (end < s.size() && word_char(s[end])))) continue;
      if (!f(at, end)) return;
    }
  }
  bool found(const std::string& s) const {
    if (!plain) { std::smatch m; return std::regex_search(s, m, re); }
    bool hit = false;
    each(s, [&](size_t, size_t) { hit = true; return false; });
    return hit;
  }
  // The first match starting at or after `from` (match_one_pattern on the rest of the line, REG_NOTBOL past its start).
  bool first_from(const std::string& s, size_t from, size_t& b, size_t& e) const {
    if (plain) {
      bool got = false;
      each(s, [&](size_t b2, size_t e2) {
        if (b2 < from) return true;
        b = b2, e = e2, got = true;
        return false;
      });
      return got;
    }
    // a word match needs the character before it: the search starts one earlier, the word itself at `from` or later
    size_t start = word && from > 0 ? from - 1 : from;
    while (start <= s.size()) {
      std::smatch m;
      auto flags = start > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
      if (!std::regex_search(s.begin() + (long)start, s.end(), m, re, flags)) return false;
      const size_t mb = start + (size_t)(word ? m.position(2) : m.position(0));
      const size_t me = mb + (size_t)(word ? m.length(2) : m.length(0));
      if (mb >= from) { b = mb; e = me; return true; }
      start = start + (size_t)m.position(0) + 1;
    }
    return false;
  }
  // Could s hold a match at all: for a plain pattern, the needle is somewhere in it (word edges aside).
  bool maybe_in(const std::string& s) const {
    if (!plain) return true;
    if (!icase) return s.find(needle) != std::string::npos;
    auto it = std::search(s.begin(), s.end(), needle.begin(), needle.end(), [](char a, char b) { return lower_ascii(a) == b; });
    return it != s.end();
  }
};
}  // namespace

int cmd_grep(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'n', "line-number"}, {'i', "ignore-case"}, {'l', "files-with-matches"}, {'L', "files-without-match"},
                                         {'c', "count"}, {'w', "word-regexp"}, {'v', "invert-match"}, {'E', "extended-regexp"},
                                         {'G', "basic-regexp"}, {'F', "fixed-strings"}, {'P', "perl-regexp"}, {'e', "", Arg::Required},
                                         {0, "cached"}, {'h', ""}, {'H', ""}, {'I', ""}, {'q', "quiet"}, {'o', "only-matching"},
                                         {0, "untracked"}, {0, "no-index"}, {0, "full-name"}, {'a', "text"}, {0, "column"},
                                         {0, "heading"}, {0, "break"}, {'z', "null"}, {0, "max-depth", Arg::Required},
                                         {'A', "after-context", Arg::Required}, {'B', "before-context", Arg::Required},
                                         {'C', "context", Arg::Required}, {0, "and"}, {0, "or"}, {0, "not"}, {0, "all-match"},
                                         {'f', "", Arg::Required}, {0, "color", Arg::Optional}, {0, "recurse-submodules"},
                                         {'p', "show-function"}, {'W', "function-context"}, {'m', "max-count", Arg::Required},
                                         {0, "threads", Arg::Required}, {'O', "open-files-in-pager", Arg::Optional}});
  if (p.has("and") || p.has("or") || p.has("not") || p.has("all-match") || p.has("perl-regexp") || p.has("recurse-submodules") ||
      p.has("open-files-in-pager") || p.has("max-depth"))
    unsupported(c, "this grep option");
  std::vector<std::string> patterns = p.all("e");
  std::vector<std::string> pos = p.pos;
  if (p.has("f")) {
    auto r = c.host().read_file(c.abs_path(p.get("f")));
    if (!r.ok()) die(c, "cannot open '" + p.get("f") + "'");
    std::string t((const char*)r->data(), r->size());
    std::istringstream ss(t);
    std::string l;
    while (std::getline(ss, l)) if (!l.empty()) patterns.push_back(l);
  }
  if (patterns.empty()) {
    if (pos.empty()) die(c, "no pattern given");
    patterns.push_back(pos[0]);
    pos.erase(pos.begin());
  }
  bool no_index = p.has("no-index");
  if (!no_index) c.open_repo(false);
  std::vector<Matcher> res;
  for (auto& pat : patterns) {
    Matcher m;
    m.word = p.has("word-regexp");
    m.icase = p.has("ignore-case");
    std::string ere = p.has("extended-regexp") ? pat : bre_to_ere(pat);
    if (p.has("fixed-strings") || plain_pattern(ere)) {
      m.plain = true;
      m.needle = p.has("fixed-strings") ? pat : ere;
      if (m.icase) for (auto& ch : m.needle) ch = lower_ascii(ch);
    } else {
      std::string re = m.word ? "(^|[^[:alnum:]_])(" + ere + ")([^[:alnum:]_]|$)" : ere;
      try {
        m.re = std::regex(re, std::regex::extended | (m.icase ? std::regex::icase : std::regex::extended));
      } catch (const std::regex_error&) {
        die(c, "invalid regular expression: " + pat);
      }
    }
    res.push_back(std::move(m));
  }
  // revisions, then paths
  std::vector<std::string> revs, paths = p.after_dashdash;
  // before "--": revisions until the first argument that is not one (the paths after "--" do not end them)
  bool seen_path = false;
  std::vector<std::string> pre_paths;
  for (auto& a : pos) {
    if (!no_index && !seen_path && resolve(c, a) && !peel_to(c, *resolve(c, a), "tree").empty()) revs.push_back(a);
    else { seen_path = true; pre_paths.push_back(a); }
  }
  paths.insert(paths.begin(), pre_paths.begin(), pre_paths.end());
  Pathspec ps = no_index ? Pathspec{} : make_pathspec(c, paths);
  long max_count = p.has("max-count") ? std::atol(p.get("max-count").c_str()) : -1;
  bool any = false;
  // grep.c's colors (color.grep.<slot> over git's), used when colors are on: --color, color.grep, color.ui
  struct GrepColors { std::string filename, lineno, sep, match, selected; } gc;
  if (c.color) {
    auto slot = [&](std::initializer_list<const char*> names, const char* def) -> std::string {
      for (const char* n : names)
        if (auto v = c.config.get(std::string("color.grep.") + n))
          if (auto col = parse_git_color(*v)) return *col;
      return def;
    };
    gc.filename = slot({"filename"}, "\033[35m");
    gc.lineno = slot({"linenumber"}, "\033[32m");
    gc.sep = slot({"separator"}, "\033[36m");
    gc.match = slot({"matchselected", "match"}, "\033[1;31m");
    gc.selected = slot({"selected"}, "");
  }
  // output_color: the text in its color with a reset after, plain when the color is empty
  auto paint = [](std::string& o, const std::string& col, std::string_view text) {
    if (col.empty()) { o.append(text); return; }
    o += col;
    o.append(text);
    o += kColorReset;
  };
  // grep_next_match: the earliest match of any pattern from `from`, the longest of those starting there
  auto next_match = [&](const std::string& line, size_t from, size_t& b, size_t& e) {
    bool hit = false;
    for (auto& re : res) {
      size_t mb, me;
      if (!re.first_from(line, from, mb, me)) continue;
      if (hit && (mb > b || (mb == b && me < e))) continue;
      b = mb, e = me, hit = true;
    }
    return hit;
  };
  // One file searched into `o`; true when it matched (what -q and the exit code ask). Free of shared state, so the
  // work tree's files are searched on threads, as git greps (grep.threads, one per core by default).
  const bool o_text = p.has("text"), o_I = p.has("I"), o_invert = p.has("invert-match"), o_quiet = p.has("quiet"),
             o_l = p.has("files-with-matches"), o_L = p.has("files-without-match"), o_count = p.has("count"),
             o_h = p.has("h"), o_n = p.has("line-number"), o_only = p.has("only-matching"), o_null = p.has("null");
  // context and function options (grep.c's pre_context, post_context, funcname, funcbody, heading, file_break)
  auto ctx_num = [&](const char* key) -> long {
    if (!p.has(key)) return -1;
    const std::string v = p.get(key);
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end || n < 0) die(c, std::string("switch `") + (key[0] == 'c' ? 'C' : key[0] == 'a' ? 'A' : 'B') + "' expects a numerical value");
    return n;
  };
  long pre_ctx = 0, post_ctx = 0;
  if (long v = ctx_num("context"); v >= 0) pre_ctx = post_ctx = v;
  if (long v = ctx_num("after-context"); v >= 0) post_ctx = v;
  if (long v = ctx_num("before-context"); v >= 0) pre_ctx = v;
  const bool o_funcname = p.has("show-function"), o_funcbody = p.has("function-context"), o_heading = p.has("heading"),
             o_break = p.has("break"), o_column = p.has("column");
  // A file's first shown line gets a marker: '\x01' for --break's blank line, '\x02' for "--", written only when an
  // earlier file showed lines (git's show_hunk_mark); the files are searched apart, the marks settled as they are joined.
  auto search_into = [&](const std::string& shown, const std::string& text, std::string& o) -> bool {
    // grep_source_1: a binary file is searched line by line like any other (counted with -c, named with -l and -L);
    // only a line it would print becomes "Binary file ... matches", and the file ends there. -I skips it outright.
    const bool binary_only = !o_text && is_binary(text);
    if (binary_only && o_I) return false;
    // No pattern can match anywhere in the file: no line matches (with -v, every line does, so the lines are read).
    // (-L: a file listed is what counts as found, for the exit code, as git has it)
    if (!o_invert && std::none_of(res.begin(), res.end(), [&](const Matcher& m) { return m.maybe_in(text); })) {
      if (o_L) { paint(o, gc.filename, shown); o += o_null ? '\0' : '\n'; return true; }
      return false;
    }
    // the file's lines, 1-based: [starts[n-1], end of line)
    std::vector<size_t> starts;
    for (size_t i = 0; i < text.size();) {
      starts.push_back(i);
      const size_t nl = text.find('\n', i);
      if (nl == std::string::npos) break;
      i = nl + 1;
    }
    const unsigned nlines = (unsigned)starts.size();
    auto line_of = [&](unsigned lno) -> std::string_view {
      const size_t b = starts[lno - 1];
      size_t e = text.find('\n', b);
      if (e == std::string::npos) e = text.size();
      return std::string_view(text).substr(b, e - b);
    };
    auto is_func = [](std::string_view l) { return !l.empty() && (std::isalpha((unsigned char)l[0]) || l[0] == '_' || l[0] == '$'); };
    auto is_empty = [](std::string_view l) { return std::all_of(l.begin(), l.end(), [](char ch) { return git_isspace((unsigned char)ch); }); };
    unsigned last_shown = 0, last_hit = 0;
    long count = 0;
    bool listed = false;
    auto sep_sign = [&](std::string& out, char sign) {
      if (o_null) out += '\0';
      else paint(out, gc.sep, std::string_view(&sign, 1));
    };
    auto line_header = [&](unsigned lno, long cno, char sign) {
      if (o_heading && last_shown == 0) { paint(o, gc.filename, shown); o += '\n'; }
      last_shown = lno;
      if (!o_heading && !o_h) { paint(o, gc.filename, shown); sep_sign(o, sign); }
      if (o_n) { paint(o, gc.lineno, std::to_string(lno)); sep_sign(o, sign); }
      if (o_column && cno) { paint(o, gc.lineno, std::to_string(cno)); sep_sign(o, sign); }
    };
    // show_line: hunk marks, the header, then the text with each match colored (or only the matches, with -o)
    auto show_line = [&](unsigned lno, long cno, char sign) {
      if (o_break && last_shown == 0) {
        if (o.empty() || o[0] != '\x01') o.insert(0, 1, '\x01');
      } else if (pre_ctx || post_ctx || o_funcbody) {
        if (last_shown == 0) { if (o.empty() || o[0] != '\x02') o.insert(0, 1, '\x02'); }
        else if (lno > last_shown + 1) { paint(o, gc.sep, "--"); o += '\n'; }
      }
      const std::string line(line_of(lno));
      if (!o_only) line_header(lno, cno, sign);
      size_t at = 0;
      if (c.color || o_only) {
        const std::string& match_c = gc.match;   // matchSelected and matchContext share git's default
        const std::string line_c = sign == ':' ? gc.selected : "";
        size_t b2 = 0, e2 = 0;
        while (at < line.size() && next_match(line, at, b2, e2)) {
          if (b2 == e2) break;
          if (o_only) line_header(lno, cno, sign);
          else paint(o, line_c, std::string_view(line).substr(at, b2 - at));
          paint(o, match_c, std::string_view(line).substr(b2, e2 - b2));
          if (o_only) o += '\n';
          cno += (long)(e2 - at);
          at = e2;
        }
        if (!o_only) { paint(o, line_c, std::string_view(line).substr(at)); o += '\n'; }
      } else if (!o_only) {
        o += line;
        o += '\n';
      }
    };
    auto show_funcname_line = [&](unsigned lno) {
      while (lno > 1) {
        --lno;
        if (lno <= last_shown) break;
        if (is_func(line_of(lno))) { show_line(lno, 0, '='); break; }
      }
    };
    auto show_pre_context = [&](unsigned lno) {
      unsigned cur = lno, from = 1, funcname_lno = 0;
      bool funcname_needed = o_funcname, comment_needed = false;
      if ((unsigned long)pre_ctx < lno) from = lno - (unsigned)pre_ctx;
      if (from <= last_shown) from = last_shown + 1;
      const unsigned orig_from = from;
      if (o_funcbody) {
        if (is_func(line_of(lno))) comment_needed = true;
        else funcname_needed = true;
        from = last_shown + 1;
      }
      while (cur > 1 && cur > from) {
        --cur;
        const std::string_view l = line_of(cur);
        if (comment_needed && (is_empty(l) || is_func(l))) {
          comment_needed = false;
          from = orig_from;
          if (cur < from) { ++cur; break; }
        }
        if (funcname_needed && is_func(l)) {
          funcname_lno = cur;
          funcname_needed = false;
          if (o_funcbody) comment_needed = true;
          else from = orig_from;
        }
      }
      if (o_funcname && funcname_needed) show_funcname_line(cur);
      for (; cur < lno; ++cur) show_line(cur, 0, cur == funcname_lno ? '=' : '-');
    };
    bool show_function = false;
    unsigned peek = 0;
    for (unsigned lno = 1; lno <= nlines; ++lno) {
      const std::string line(line_of(lno));
      // match_line: whether any pattern matches, and with --column where the earliest one starts
      long col = -1;
      bool hit = false;
      for (auto& re : res) {
        size_t b2, e2;
        if (!o_column) { if (re.found(line)) { hit = true; break; } continue; }
        if (re.first_from(line, 0, b2, e2)) { hit = true; if (col < 0 || (long)b2 < col) col = (long)b2; }
      }
      if (o_invert) hit = !hit;
      if (o_L) { if (hit) return false; continue; }
      if (hit && (max_count < 0 || count < max_count)) {
        ++count;
        if (o_quiet) return true;
        if (o_l) { if (!listed) { paint(o, gc.filename, shown); o += o_null ? '\0' : '\n'; listed = true; } return true; }
        if (o_count) continue;
        if (binary_only) { o += "Binary file "; paint(o, gc.filename, shown); o += " matches\n"; return true; }
        if (pre_ctx || o_funcbody) show_pre_context(lno);
        else if (o_funcname) show_funcname_line(lno);
        long cno = o_invert ? -1 : col;
        if (cno < 0) cno = 0;
        show_line(lno, cno + 1, ':');
        last_hit = lno;
        if (o_funcbody) show_function = true;
        continue;
      }
      if (show_function && (!peek || peek < lno)) {
        // trailing empty lines are not part of a function unless what follows them is
        peek = lno;
        while (peek <= nlines && is_empty(line_of(peek))) ++peek;
        if (peek <= nlines && is_func(line_of(peek))) show_function = false;
      }
      if (show_function || (last_hit && lno <= last_hit + (unsigned long)post_ctx)) show_line(lno, col + 1, '-');
    }
    if (o_L) { paint(o, gc.filename, shown); o += o_null ? '\0' : '\n'; return true; }
    if (o_count && count) {
      if (!o_h) { paint(o, gc.filename, shown); sep_sign(o, ':'); }
      o += std::to_string(count);
      o += '\n';
      return true;
    }
    return last_hit != 0;
  };
  // the files' outputs in order, a hunk mark before a file's lines when an earlier file showed some
  bool lines_before = false;
  auto emit = [&](std::string& o) {
    if (!o.empty() && (o[0] == '\x01' || o[0] == '\x02')) {
      if (lines_before) {
        if (o[0] == '\x01') c.out << '\n';
        else { std::string m; paint(m, gc.sep, "--"); c.out << m << '\n'; }
      }
      lines_before = true;
      c.out.write(o.data() + 1, (std::streamsize)(o.size() - 1));
      return;
    }
    c.out << o;
  };
  auto search = [&](const std::string& shown, const std::string& text) {
    std::string o;
    if (search_into(shown, text, o)) any = true;
    emit(o);
  };
  if (no_index) {
    std::function<void(const std::string&)> walkdir = [&](const std::string& d) {
      auto names = c.host().list_dir(d);
      if (!names.ok()) return;
      std::vector<std::string> sorted = *names;
      std::sort(sorted.begin(), sorted.end());
      for (auto& n : sorted) {
        if (n == "." || n == ".." || n == ".git") continue;
        std::string full = d + "/" + n;
        auto st = c.host().stat(full);
        if (!st.ok()) continue;
        if (st->is_dir) { walkdir(full); continue; }
        auto r = c.host().read_file(full);
        if (r.ok()) search(full.substr(c.cwd.size() + 1), std::string((const char*)r->data(), r->size()));
      }
    };
    if (paths.empty()) walkdir(c.cwd);
    for (auto& pth : paths) {
      std::string full = c.abs_path(pth);
      auto st = c.host().stat(full);
      if (st.ok() && st->is_dir) walkdir(full);
      else if (st.ok()) { auto r = c.host().read_file(full); if (r.ok()) search(pth, std::string((const char*)r->data(), r->size())); }
    }
    return any ? 0 : 1;
  }
  auto display = [&](const std::string& path) { return p.has("full-name") ? path : c.to_display_path(path); };
  if (!revs.empty()) {
    for (auto& r : revs) {
      std::string tree = peel_to(c, must_resolve(c, r), "tree");
      for (auto& [path, f] : snapshot_tree(c, tree)) {
        if (!ps.match(path) || f.mode == 0160000) continue;
        if (ps.empty() && !c.prefix.empty() && path.rfind(c.prefix, 0) != 0) continue;
        search(r + ":" + display(path), blob_text(c, f.oid));
        if (any && p.has("quiet")) return 0;
      }
    }
    return any ? 0 : 1;
  }
  std::vector<std::string> files;
  for (auto& e : c.repo->index()) files.push_back(e.path);
  if (p.has("untracked")) { auto un = c.repo->untracked(false); if (un.ok()) for (auto& u : *un) files.push_back(u); }
  std::sort(files.begin(), files.end());
  std::map<std::string, const GitIndexEntry*> idx;
  for (auto& e : c.repo->index()) idx[e.path] = &e;
  if (!p.has("cached") && !o_quiet) {
    // the work tree: files read and searched on threads, their output written in path order
    std::vector<std::string> todo;
    for (auto& path : files) {
      if (!ps.match(path)) continue;
      if (ps.empty() && !c.prefix.empty() && path.rfind(c.prefix, 0) != 0) continue;
      auto it = idx.find(path);
      if (it != idx.end() && it->second->mode == 0160000) continue;
      todo.push_back(path);
    }
    std::vector<std::string> shown(todo.size()), outs(todo.size());
    for (size_t i = 0; i < todo.size(); ++i) shown[i] = display(todo[i]);
    std::vector<char> matched(todo.size(), 0);
    std::atomic<size_t> next{0};
    auto work = [&]() {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= todo.size()) return;
        bool ok = false;
        std::string text = read_worktree_file(c, todo[i], &ok);
        if (ok) matched[i] = search_into(shown[i], text, outs[i]) ? 1 : 0;
      }
    };
    unsigned n = hal::hardware_threads();
    if (p.has("threads")) n = (unsigned)std::max(1L, std::atol(p.get("threads").c_str()));
    else if (auto t = c.config.get("grep.threads")) { long v = std::atol(t->c_str()); if (v > 0) n = (unsigned)v; }
    n = std::max(1u, std::min<unsigned>(n == 0 ? 1 : n, 32));
    if (todo.size() < 64) n = 1;
    std::vector<hal::Thread> pool;
    for (unsigned t = 1; t < n; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    for (size_t i = 0; i < todo.size(); ++i) {
      if (matched[i]) any = true;
      emit(outs[i]);
    }
    return any ? 0 : 1;
  }
  for (auto& path : files) {
    if (!ps.match(path)) continue;
    if (ps.empty() && !c.prefix.empty() && path.rfind(c.prefix, 0) != 0) continue;
    auto it = idx.find(path);
    if (it != idx.end() && it->second->mode == 0160000) continue;
    std::string text;
    if (p.has("cached")) {
      if (it == idx.end()) continue;
      text = blob_text(c, it->second->oid);
    } else {
      bool ok = false;
      text = read_worktree_file(c, path, &ok);
      if (!ok) continue;
    }
    search(display(path), text);
    if (any && p.has("quiet")) return 0;
  }
  return any ? 0 : 1;
}

}  // namespace aios_git
