// blame: who last touched each line of a file, as git's blame.c works it out: the file's lines start out
// charged to the final commit, and each commit passes the lines it did not change on to its parents.
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <ctime>
#include <memory>
#include <queue>
#include <regex>
#include <set>
#include <tuple>
#include <unordered_map>

#include "aios/userland/git_merge.hpp"
#include "color.hpp"
#include "hist_util.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const char* kBlameUsage =
    "usage: git blame [<options>] [<rev-opts>] [<rev>] [--] <file>\n"
    "\n"
    "    <rev-opts> are documented in git-rev-list(1)\n"
    "\n"
    "    --[no-]incremental    show blame entries as we find them, incrementally\n"
    "    -b                    do not show object names of boundary commits (Default: off)\n"
    "    --[no-]root           do not treat root commits as boundaries (Default: off)\n"
    "    --[no-]show-stats     show work cost statistics\n"
    "    --[no-]progress       force progress reporting\n"
    "    --[no-]score-debug    show output score for blame entries\n"
    "    -f, --[no-]show-name  show original filename (Default: auto)\n"
    "    -n, --[no-]show-number\n"
    "                          show original linenumber (Default: off)\n"
    "    -p, --[no-]porcelain  show in a format designed for machine consumption\n"
    "    --[no-]line-porcelain show porcelain format with per-line commit information\n"
    "    -c                    use the same output mode as git-annotate (Default: off)\n"
    "    -t                    show raw timestamp (Default: off)\n"
    "    -l                    show long commit SHA1 (Default: off)\n"
    "    -s                    suppress author name and timestamp (Default: off)\n"
    "    -e, --[no-]show-email show author email instead of name (Default: off)\n"
    "    -w                    ignore whitespace differences\n"
    "    --[no-]ignore-rev <rev>\n"
    "                          ignore <rev> when blaming\n"
    "    --[no-]ignore-revs-file <file>\n"
    "                          ignore revisions from <file>\n"
    "    --[no-]color-lines    color redundant metadata from previous line differently\n"
    "    --[no-]color-by-age   color lines by age\n"
    "    --[no-]minimal        spend extra cycles to find better match\n"
    "    -S <file>             use revisions from <file> instead of calling git-rev-list\n"
    "    --[no-]contents <file>\n"
    "                          use <file>'s contents as the final image\n"
    "    -C[<score>]           find line copies within and across files\n"
    "    -M[<score>]           find line movements within and across files\n"
    "    -L <range>            process only line range <start>,<end> or function :<funcname>\n"
    "    --[no-]abbrev[=<n>]   use <n> digits to display object names\n"
    "\n";

constexpr uint32_t kOutputAnnotate = 1, kOutputNumber = 2, kOutputName = 4, kOutputPorcelain = 8, kOutputLinePorcelain = 16,
                   kOutputNoAuthor = 32, kOutputEmail = 64, kOutputRawTime = 128, kOutputLong = 256, kOutputBlank = 512,
                   kOutputColorLine = 1024, kOutputAgeColor = 2048;
constexpr int kDefaultMoveScore = 20, kDefaultCopyScore = 40;

struct Ent { int lno = 0, s_lno = 0, n = 0, org = -1; };

struct Origin {
  int commit = 0;
  std::string path, blob;
  uint32_t mode = 0100644;
  int previous = -1;
  std::vector<Ent> sus;
  std::string text;
  bool loaded = false;
};

struct Meta { std::string author, author_mail, committer, committer_mail, summary, atz, ctz; int64_t at = 0, ct = 0; };

struct CommitRec {
  std::string oid;
  std::string tree;
  std::vector<std::string> parents;
  int64_t date = 0;
  bool boundary = false, shown = false, multi_path = false, queued = false;
  std::vector<int> origins;
  GitCommit cm;
  bool fake = false;
  Meta fake_meta;
};

bool is_alnum_ascii(unsigned char ch) { return std::isalnum(ch) != 0; }

// Display width of UTF-8 text, as git's utf8_strwidth counts it (wide East Asian characters two, combining none).
int utf8_width(const std::string& s) {
  int w = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char ch = (unsigned char)s[i];
    uint32_t cp;
    size_t len;
    if (ch < 0x80) { cp = ch; len = 1; }
    else if ((ch & 0xe0) == 0xc0 && i + 1 < s.size()) { cp = ((ch & 0x1fu) << 6) | (s[i + 1] & 0x3fu); len = 2; }
    else if ((ch & 0xf0) == 0xe0 && i + 2 < s.size()) { cp = ((ch & 0x0fu) << 12) | ((s[i + 1] & 0x3fu) << 6) | (s[i + 2] & 0x3fu); len = 3; }
    else if ((ch & 0xf8) == 0xf0 && i + 3 < s.size()) { cp = ((ch & 0x07u) << 18) | ((s[i + 1] & 0x3fu) << 12) | ((s[i + 2] & 0x3fu) << 6) | (s[i + 3] & 0x3fu); len = 4; }
    else { ++w; ++i; continue; }
    i += len;
    if (cp == 0) continue;
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) continue;
    bool zero = (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x483 && cp <= 0x489) || (cp >= 0x591 && cp <= 0x5bd) ||
                (cp >= 0x610 && cp <= 0x61a) || (cp >= 0x64b && cp <= 0x65f) || (cp >= 0x200b && cp <= 0x200f) ||
                (cp >= 0x20d0 && cp <= 0x20f0) || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f) || cp == 0xfeff;
    if (zero) continue;
    bool wide = (cp >= 0x1100 && cp <= 0x115f) || cp == 0x2329 || cp == 0x232a || (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
                (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
                (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
                (cp >= 0x1f900 && cp <= 0x1f9ff) || (cp >= 0x20000 && cp <= 0x3fffd);
    w += wide ? 2 : 1;
  }
  return w;
}

int digits_of(long n) {
  int d = 1;
  while (n >= 10) { n /= 10; ++d; }
  return d;
}

// A date in the form --date= or blame.date names; the strftime ones are cut here.
std::string blame_date(int64_t t, const std::string& tz, const std::string& mode) {
  if (mode.rfind("format:", 0) == 0) {
    std::string fmt = mode.substr(7), expanded;
    for (size_t i = 0; i < fmt.size(); ++i) {
      if (fmt[i] == '%' && i + 1 < fmt.size() && (fmt[i + 1] == 'z' || fmt[i + 1] == 'Z')) {
        if (fmt[i + 1] == 'z') expanded += tz;
        ++i;
        continue;
      }
      expanded += fmt[i];
    }
    int sign = tz.size() >= 5 && tz[0] == '-' ? -1 : 1;
    int mins = tz.size() >= 5 ? ((tz[1] - '0') * 10 + (tz[2] - '0')) * 60 + (tz[3] - '0') * 10 + (tz[4] - '0') : 0;
    time_t shifted = (time_t)(t + (int64_t)sign * mins * 60);
    struct tm tmv;
    gmtime_r(&shifted, &tmv);
    char buf[512];
    size_t n = std::strftime(buf, sizeof buf, expanded.c_str(), &tmv);
    return std::string(buf, n);
  }
  return format_date(t, tz, mode);
}

// Columns a date takes: blame pads it to the width of its format's longest form.
size_t date_width(const std::string& mode) {
  std::string m = mode;
  if (m.size() > 6 && m.compare(m.size() - 6, 6, "-local") == 0) m = m.substr(0, m.size() - 6);
  if (m == "default") return 30;
  if (m == "iso" || m == "iso8601" || m == "iso-strict" || m == "iso8601-strict") return 25;
  if (m == "rfc" || m == "rfc2822") return 31;
  if (m == "short" || m == "unix") return 10;
  if (m == "relative") return 22;
  if (m == "raw") return 16;
  if (m.rfind("format:", 0) == 0) return blame_date(0, "+0000", mode).size();
  return 25;
}

class Blame {
 public:
  explicit Blame(Ctx& c) : c_(c) {}
  int run();

 private:
  Ctx& c_;
  uint32_t opt_ = 0;
  bool incremental_ = false, show_root_ = false, ignore_ws_ = false, first_parent_ = false;
  bool move_ = false, copy_ = false, copy_harder_ = false;
  int move_score_ = kDefaultMoveScore, copy_score_ = kDefaultCopyScore;
  int abbrev_ = -1;            // -1: auto
  std::string date_mode_ = "iso";
  std::vector<std::string> ranges_;
  std::string path_, contents_from_, rev_spec_;
  int64_t max_age_ = -1;
  bool has_max_age_ = false;

  std::vector<CommitRec> commits_;
  std::unordered_map<std::string, int> commit_ix_;
  std::vector<Origin> origins_;
  std::map<std::pair<int, std::string>, int> origin_ix_;
  std::unordered_map<std::string, std::vector<GitTreeEntry>> tree_cache_;
  struct PathHit { bool found = false; std::string oid; uint32_t mode = 0; };
  std::unordered_map<std::string, PathHit> path_memo_;   // tree id + '\0' + path below it
  std::unordered_map<std::string, Meta> meta_cache_;
  Mailmap mailmap_;
  std::string final_text_;
  std::vector<std::string_view> final_lines_;
  std::vector<Ent> done_;
  std::priority_queue<std::tuple<int64_t, int64_t, int>> queue_;
  int64_t seq_ = 0;
  std::set<std::string> hidden_;
  int final_commit_ = -1;

  [[noreturn]] void bad_usage() {
    c_.err << kBlameUsage;
    throw Exit{129};
  }
  [[noreturn]] void range_usage() {
    std::string full = kBlameUsage;
    c_.err << full.substr(0, full.find('\n') + 1);
    throw Exit{129};
  }
  void parse_args(std::vector<std::string>& revs);
  int commit_of(const std::string& oid);
  int origin_of(int commit, const std::string& path);
  const std::vector<GitTreeEntry>& tree_entries(const std::string& tree);
  bool find_path(const std::string& tree, const std::string& path, std::string& oid, uint32_t& mode);
  bool find_path_from(const std::string& tree, const std::string& path, size_t pos, std::string& oid, uint32_t& mode);
  const std::string& text_of(int org);
  void drop_text(int org);
  std::vector<GitDiffEdit> diff_texts(const std::string& a, const std::string& b);
  void put_commit(int commit);
  void queue_blames(int org, std::vector<Ent> ents);
  int find_origin(int parent, int org);
  int find_rename(int parent, int org);
  void pass_whole(int org, int porigin);
  void pass_to_parent(int org, int porigin);
  void pass_blame(int org);
  int score_of(const Ent& e) const;
  void filter_small(std::vector<Ent>& small, std::vector<Ent>& source, int thr);
  void best_run(const Ent& e, int porg, Ent* split, bool* found);
  void find_move(std::vector<std::pair<int, Ent>>& blamed, std::vector<Ent>& toosmall, int org, int porigin);
  void find_copy(std::vector<std::pair<int, Ent>>& blamed, std::vector<Ent>& toosmall, int org, int parent, int porigin);
  void assign();
  const Meta& meta(int commit);
  void load_ranges(long lines, std::vector<std::pair<long, long>>& out);
  void emit_incremental(const Ent& e);
  void emit_details(int org, bool repeat, bool incremental_always);
  void output();
  std::string git_name(const std::string& p) { return quote_path(p, c_.config.get_bool("core.quotepath", true)); }
  long parse_loc(const std::string& spec, size_t& pos, long begin, bool is_end, bool& have, bool anchor_from_start);
  bool parse_range_arg(const std::string& arg, long lines, long anchor, long& begin, long& end);
  bool func_range(const std::string& arg, long lines, long anchor, long& begin, long& end);
  int fake_commit(const std::string& from, const std::string& base, bool external);
};

void Blame::parse_args(std::vector<std::string>& revs) {
  std::vector<std::string> pos;
  std::vector<std::string> rest(c_.args.begin() + 1, c_.args.end());
  bool dashdash = false;
  std::vector<std::string> after;
  auto need = [&](size_t& i, const std::string& name) -> std::string {
    if (i + 1 >= rest.size()) { c_.err << "error: option `" << name << "' requires a value\n"; c_.err << kBlameUsage; throw Exit{129}; }
    return rest[++i];
  };
  for (size_t i = 0; i < rest.size(); ++i) {
    const std::string a = rest[i];
    if (dashdash) { after.push_back(a); continue; }
    if (a == "--") { dashdash = true; continue; }
    if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      std::string name = a.substr(2), val;
      bool has_val = false;
      size_t eq = name.find('=');
      if (eq != std::string::npos) { val = name.substr(eq + 1); name = name.substr(0, eq); has_val = true; }
      static const char* longs[] = {"incremental", "root", "show-stats", "progress", "score-debug", "show-name", "show-number", "porcelain",
                                    "line-porcelain", "show-email", "ignore-rev", "ignore-revs-file", "color-lines", "color-by-age", "minimal",
                                    "contents", "abbrev", "date", "first-parent", "since", "after", "reverse", "not", "all", "help"};
      std::string full = name;
      bool negated = false;
      {
        std::string base = name;
        if (base.rfind("no-", 0) == 0) { base = base.substr(3); negated = true; }
        std::vector<std::string> hits;
        bool exact = false;
        for (auto l : longs) {
          if (base == l) { exact = true; hits = {l}; break; }
          if (std::string(l).rfind(base, 0) == 0) hits.push_back(l);
        }
        if (!exact && hits.size() != 1) {
          if (hits.size() > 1) { c_.err << "error: ambiguous option: " << name << "\n"; c_.err << kBlameUsage; throw Exit{129}; }
          c_.err << "error: unknown option `" << a << "'\n";
          c_.err << kBlameUsage;
          throw Exit{129};
        }
        full = hits[0];
      }
      if (full == "help") { c_.out << kBlameUsage; throw Exit{129}; }
      if (full == "incremental") incremental_ = !negated;
      else if (full == "root") show_root_ = !negated;
      else if (full == "show-name") { if (negated) opt_ &= ~kOutputName; else opt_ |= kOutputName; }
      else if (full == "show-number") { if (negated) opt_ &= ~kOutputNumber; else opt_ |= kOutputNumber; }
      else if (full == "porcelain") { if (negated) opt_ &= ~kOutputPorcelain; else opt_ |= kOutputPorcelain; }
      else if (full == "line-porcelain") { if (negated) opt_ &= ~(kOutputPorcelain | kOutputLinePorcelain); else opt_ |= kOutputPorcelain | kOutputLinePorcelain; }
      else if (full == "show-email") { if (negated) opt_ &= ~kOutputEmail; else opt_ |= kOutputEmail; }
      else if (full == "progress") {}
      else if (full == "minimal") {}
      else if (full == "contents") { contents_from_ = negated ? "" : (has_val ? val : need(i, "contents")); }
      else if (full == "abbrev") {
        if (negated) abbrev_ = -1;
        else if (!has_val) abbrev_ = -1;
        else {
          long v = std::strtol(val.c_str(), nullptr, 10);
          if (v && v < 4) v = 4;
          else if (v > 40) v = 40;
          abbrev_ = (int)v;
        }
      }
      else if (full == "date") {
        std::string v = has_val ? val : need(i, "date");
        if (negated) v = "iso";
        static const char* known[] = {"default", "iso", "iso8601", "iso-strict", "iso8601-strict", "short", "relative", "raw", "unix", "rfc", "rfc2822",
                                      "default-local", "iso-local", "iso8601-local", "iso-strict-local", "short-local", "relative-local", "raw-local",
                                      "unix-local", "rfc-local", "local"};
        bool ok = v.rfind("format:", 0) == 0;
        for (auto k : known) if (v == k) ok = true;
        if (v == "human" || v.rfind("human-local", 0) == 0 || v == "auto:human") unsupported(c_, "blame --date=" + v);
        if (!ok) die(c_, "unknown date format " + v);
        date_mode_ = v == "local" ? "default-local" : v;
      }
      else if (full == "first-parent") first_parent_ = !negated;
      else if (full == "since" || full == "after") {
        std::string v = has_val ? val : need(i, full);
        int64_t t; std::string tz;
        if (!parse_date(v, t, tz)) die(c_, "invalid date format: " + v);
        max_age_ = t; has_max_age_ = true;
      }
      else if (full == "ignore-rev" || full == "ignore-revs-file") unsupported(c_, "blame --" + full);
      else if (full == "show-stats" || full == "score-debug") unsupported(c_, "blame --" + full);
      else if (full == "color-lines") { if (negated) opt_ &= ~kOutputColorLine; else opt_ |= kOutputColorLine; }
      else if (full == "color-by-age") { if (negated) opt_ &= ~kOutputAgeColor; else opt_ |= kOutputAgeColor; }
      else if (full == "reverse") unsupported(c_, "blame --reverse");
      else if (full == "not" || full == "all") unsupported(c_, "blame --" + full);
      continue;
    }
    if (a.size() > 1 && a[0] == '-' && a != "-") {
      for (size_t j = 1; j < a.size(); ++j) {
        char ch = a[j];
        std::string tail = a.substr(j + 1);
        switch (ch) {
          case 'b': opt_ |= kOutputBlank; break;
          case 'f': opt_ |= kOutputName; break;
          case 'n': opt_ |= kOutputNumber; break;
          case 'p': opt_ |= kOutputPorcelain; break;
          case 'c': opt_ |= kOutputAnnotate; break;
          case 't': opt_ |= kOutputRawTime; break;
          case 'l': opt_ |= kOutputLong; break;
          case 's': opt_ |= kOutputNoAuthor; break;
          case 'e': opt_ |= kOutputEmail; break;
          case 'w': ignore_ws_ = true; break;
          case 'h': c_.out << kBlameUsage; throw Exit{129};
          case 'L': ranges_.push_back(tail.empty() ? need(i, "L") : tail); j = a.size(); break;
          case 'S': unsupported(c_, "blame -S");
          case 'M': case 'C': {
            bool digits = !tail.empty();
            for (char d : tail) if (!std::isdigit((unsigned char)d)) digits = false;
            if (!tail.empty() && !digits) { c_.err << "error: switch `" << ch << "' expects a numerical value\n"; c_.err << kBlameUsage; throw Exit{129}; }
            if (ch == 'M') { move_ = true; if (digits) move_score_ = std::atoi(tail.c_str()); }
            else {
              if (copy_) copy_harder_ = true;
              copy_ = true; move_ = true;
              if (digits) copy_score_ = std::atoi(tail.c_str());
            }
            j = a.size();
            break;
          }
          default:
            c_.err << "error: unknown option `" << a << "'\n";
            c_.err << kBlameUsage;
            throw Exit{129};
        }
      }
      continue;
    }
    pos.push_back(a);
  }
  if (dashdash) {
    if (after.size() != 1) bad_usage();
    path_ = after[0];
    revs = pos;
  } else {
    if (pos.empty()) bad_usage();
    if (pos.size() == 2) {
      // the old "file rev" order is taken when the last one is a revision
      auto last = resolve(c_, pos[1]);
      if (last && !peel_to(c_, *last, "commit").empty() && !resolve(c_, pos[0])) std::swap(pos[0], pos[1]);
    }
    path_ = pos.back();
    pos.pop_back();
    revs = pos;
  }
}

int Blame::commit_of(const std::string& oid) {
  auto it = commit_ix_.find(oid);
  if (it != commit_ix_.end()) return it->second;
  CommitRec r;
  r.oid = oid;
  r.cm = read_commit(c_, oid);
  r.tree = r.cm.tree;
  r.parents = r.cm.parents;
  r.date = (int64_t)r.cm.committer_time;
  r.boundary = hidden_.count(oid) != 0;
  commits_.push_back(std::move(r));
  int ix = (int)commits_.size() - 1;
  commit_ix_[oid] = ix;
  return ix;
}

int Blame::origin_of(int commit, const std::string& path) {
  auto key = std::make_pair(commit, path);
  auto it = origin_ix_.find(key);
  if (it != origin_ix_.end()) return it->second;
  Origin o;
  o.commit = commit;
  o.path = path;
  origins_.push_back(std::move(o));
  int ix = (int)origins_.size() - 1;
  origin_ix_[key] = ix;
  if (!commits_[(size_t)commit].origins.empty()) commits_[(size_t)commit].multi_path = true;
  commits_[(size_t)commit].origins.push_back(ix);
  return ix;
}

const std::vector<GitTreeEntry>& Blame::tree_entries(const std::string& tree) {
  auto it = tree_cache_.find(tree);
  if (it != tree_cache_.end()) return it->second;
  auto r = c_.repo->read_tree(tree);
  std::vector<GitTreeEntry> v;
  if (r.ok()) v = *r;
  return tree_cache_.emplace(tree, std::move(v)).first->second;
}

bool Blame::find_path(const std::string& tree, const std::string& path, std::string& oid, uint32_t& mode) {
  // Each step remembered by (tree, rest of the path): a commit that left the file's directory alone finds it in one
  // small read of its root tree. Whole trees were parsed and kept for every commit (a 600-entry src/ per commit of
  // redis' history), and freeing them was a sixth of a blame.
  std::string cur = tree;
  size_t pos = 0;
  while (true) {
    std::string key = cur;
    key += '\0';
    key.append(path, pos, std::string::npos);
    if (auto m = path_memo_.find(key); m != path_memo_.end()) {
      if (!m->second.found) return false;
      oid = m->second.oid;
      mode = m->second.mode;
      return true;
    }
    size_t slash = path.find('/', pos);
    std::string_view name = std::string_view(path).substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    std::string hit_oid;
    uint32_t hit_mode = 0;
    PathHit res;
    if (!c_.repo->tree_lookup(cur, name, hit_oid, hit_mode)) {
      res.found = false;
    } else if (slash == std::string::npos) {
      res = PathHit{true, hit_oid, hit_mode};
    } else if (hit_mode != 040000) {
      res.found = false;
    } else {
      std::string sub_oid;
      uint32_t sub_mode = 0;
      bool ok = find_path_from(hit_oid, path, slash + 1, sub_oid, sub_mode);
      res = PathHit{ok, sub_oid, sub_mode};
    }
    path_memo_.emplace(std::move(key), res);
    if (!res.found) return false;
    oid = res.oid;
    mode = res.mode;
    return true;
  }
}

bool Blame::find_path_from(const std::string& tree, const std::string& path, size_t pos, std::string& oid, uint32_t& mode) {
  return find_path(tree, path.substr(pos), oid, mode);
}

const std::string& Blame::text_of(int org) {
  Origin& o = origins_[(size_t)org];
  if (!o.loaded) {
    o.text = blob_text(c_, o.blob);
    o.loaded = true;
  }
  return o.text;
}

void Blame::drop_text(int org) {
  Origin& o = origins_[(size_t)org];
  if (commits_[(size_t)o.commit].fake) return;
  o.text.clear();
  o.text.shrink_to_fit();
  o.loaded = false;
}

std::vector<GitDiffEdit> Blame::diff_texts(const std::string& a_text, const std::string& b_text) {
  // xdi_diff with no context lines drops the identical tail in 1024-byte blocks before it diffs
  std::string_view a = a_text, b = b_text;
  const size_t blk = 1024;
  size_t trimmed = 0, recovered = 0;
  const size_t smaller = std::min(a.size(), b.size());
  while (blk + trimmed <= smaller && a.substr(a.size() - trimmed - blk, blk) == b.substr(b.size() - trimmed - blk, blk)) trimmed += blk;
  const size_t at = a.size() - trimmed;
  while (recovered < trimmed)
    if (a[at + recovered++] == '\n') break;
  a.remove_suffix(trimmed - recovered);
  b.remove_suffix(trimmed - recovered);
  auto la = git_split_lines(a), lb = git_split_lines(b);
  if (!ignore_ws_) return git_diff_lines_indent(la, lb);
  std::vector<std::string> na, nb;
  auto strip = [](std::string_view line) {
    std::string s;
    for (char ch : line) if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\v' && ch != '\f') s += ch;
    return s;
  };
  na.reserve(la.size());
  nb.reserve(lb.size());
  for (auto l : la) na.push_back(strip(l));
  for (auto l : lb) nb.push_back(strip(l));
  std::vector<std::string_view> va(na.begin(), na.end()), vb(nb.begin(), nb.end());
  return git_diff_lines_indent(va, vb, &la, &lb);
}

void Blame::put_commit(int commit) {
  CommitRec& r = commits_[(size_t)commit];
  if (r.queued) return;
  r.queued = true;
  queue_.push({r.date, -(seq_++), commit});
}

void Blame::queue_blames(int org, std::vector<Ent> ents) {
  if (ents.empty()) return;
  Origin& o = origins_[(size_t)org];
  for (auto& e : ents) e.org = org;
  o.sus.insert(o.sus.end(), ents.begin(), ents.end());
  std::stable_sort(o.sus.begin(), o.sus.end(), [](const Ent& a, const Ent& b) { return a.s_lno < b.s_lno; });
  put_commit(o.commit);
}

int Blame::find_origin(int parent, int org) {
  const Origin& o = origins_[(size_t)org];
  std::string blob;
  uint32_t mode;
  if (!find_path(commits_[(size_t)parent].tree, o.path, blob, mode)) return -1;
  if (mode == 040000 || mode == 0160000) return -1;
  if ((mode == 0120000) != (o.mode == 0120000)) return -1;
  int p = origin_of(parent, o.path);
  origins_[(size_t)p].blob = blob;
  origins_[(size_t)p].mode = mode;
  return p;
}

int Blame::find_rename(int parent, int org) {
  const Origin& o = origins_[(size_t)org];
  const CommitRec& cr = commits_[(size_t)o.commit];
  std::string new_tree = cr.tree;
  if (cr.fake) return -1;
  auto renames = git_detect_renames(*c_.repo, commits_[(size_t)parent].tree, new_tree, 50);
  if (!renames.ok()) return -1;
  for (auto& r : *renames) {
    if (r.to != o.path) continue;
    std::string blob;
    uint32_t mode;
    if (!find_path(commits_[(size_t)parent].tree, r.from, blob, mode) || mode == 040000 || mode == 0160000) return -1;
    int p = origin_of(parent, r.from);
    origins_[(size_t)p].blob = blob;
    origins_[(size_t)p].mode = mode;
    return p;
  }
  return -1;
}

void Blame::pass_whole(int org, int porigin) {
  std::vector<Ent> ents;
  ents.swap(origins_[(size_t)org].sus);
  if (!origins_[(size_t)porigin].loaded && origins_[(size_t)org].loaded) {
    origins_[(size_t)porigin].text = std::move(origins_[(size_t)org].text);
    origins_[(size_t)porigin].loaded = true;
    origins_[(size_t)org].loaded = false;
  }
  queue_blames(porigin, std::move(ents));
}

void Blame::pass_to_parent(int org, int porigin) {
  Origin& o = origins_[(size_t)org];
  auto edits = diff_texts(text_of(porigin), text_of(org));
  struct Seg { long b, e; long off; };
  std::vector<Seg> segs;
  long pos_a = 0, pos_b = 0;
  for (auto& ed : edits) {
    if ((long)ed.b_start > pos_b) segs.push_back({pos_b, (long)ed.b_start, (long)ed.a_start - (long)ed.b_start});
    else if ((long)ed.b_start == pos_b && (long)ed.a_start != pos_a) {}
    pos_b = (long)ed.b_start + (long)ed.b_len;
    pos_a = (long)ed.a_start + (long)ed.a_len;
  }
  segs.push_back({pos_b, LONG_MAX, pos_a - pos_b});
  std::vector<Ent> kept, passed;
  for (auto& e : o.sus) {
    long lo = e.s_lno, hi = (long)e.s_lno + e.n;
    long at = lo;
    for (auto& sg : segs) {
      if (sg.e <= at) continue;
      if (sg.b >= hi) break;
      long x = std::max(sg.b, at), y = std::min(sg.e, hi);
      if (x > at) { Ent k = e; k.lno = e.lno + (int)(at - lo); k.s_lno = (int)at; k.n = (int)(x - at); kept.push_back(k); }
      Ent p = e;
      p.lno = e.lno + (int)(x - lo);
      p.s_lno = (int)(x + sg.off);
      p.n = (int)(y - x);
      passed.push_back(p);
      at = y;
      if (at >= hi) break;
    }
    if (at < hi) { Ent k = e; k.lno = e.lno + (int)(at - lo); k.s_lno = (int)at; k.n = (int)(hi - at); kept.push_back(k); }
  }
  o.sus = std::move(kept);
  queue_blames(porigin, std::move(passed));
}

int Blame::score_of(const Ent& e) const {
  int score = 1;
  for (int l = e.lno; l < e.lno + e.n && (size_t)l < final_lines_.size(); ++l)
    for (char ch : final_lines_[(size_t)l]) if (is_alnum_ascii((unsigned char)ch)) ++score;
  return score;
}

void Blame::filter_small(std::vector<Ent>& small, std::vector<Ent>& source, int thr) {
  std::vector<Ent> keep;
  for (auto& e : source) {
    if (score_of(e) <= thr) small.push_back(e);
    else keep.push_back(e);
  }
  source = std::move(keep);
}

// The run of lines of `e` (final-file lines) that appears unchanged in `porg`'s file with the highest score.
void Blame::best_run(const Ent& e, int porg, Ent* split, bool* found) {
  *found = false;
  std::string cut;
  for (int l = e.lno; l < e.lno + e.n; ++l) cut.append(final_lines_[(size_t)l]);
  auto edits = diff_texts(text_of(porg), cut);
  long pos_a = 0, pos_b = 0;
  int best = 0;
  auto consider = [&](long a, long b, long len) {
    if (len <= 0) return;
    Ent run;
    run.lno = e.lno + (int)b;
    run.s_lno = (int)a;
    run.n = (int)len;
    int sc = score_of(run);
    if (!*found || sc >= best) {
      *found = true;
      best = sc;
      split[0] = Ent{e.lno, e.s_lno, (int)b, e.org};
      split[1] = run;
      split[1].org = porg;
      split[2] = Ent{e.lno + (int)(b + len), e.s_lno + (int)(b + len), e.n - (int)(b + len), e.org};
    }
  };
  for (auto& ed : edits) {
    consider(pos_a, pos_b, (long)ed.b_start - pos_b);
    pos_a = (long)ed.a_start + (long)ed.a_len;
    pos_b = (long)ed.b_start + (long)ed.b_len;
  }
  consider(pos_a, pos_b, e.n - pos_b);
}

void Blame::find_move(std::vector<std::pair<int, Ent>>& blamed, std::vector<Ent>& toosmall, int org, int porigin) {
  Origin& o = origins_[(size_t)org];
  std::vector<Ent> unblamed = o.sus, leftover;
  if (unblamed.empty()) return;
  text_of(porigin);
  do {
    std::vector<Ent> next_round;
    for (auto& e : unblamed) {
      Ent split[3];
      bool found;
      best_run(e, porigin, split, &found);
      if (found && move_score_ < score_of(split[1])) {
        blamed.push_back({porigin, split[1]});
        if (split[0].n > 0) next_round.push_back(split[0]);
        if (split[2].n > 0) next_round.push_back(split[2]);
      } else {
        leftover.push_back(e);
      }
    }
    unblamed = std::move(next_round);
    filter_small(toosmall, unblamed, move_score_);
  } while (!unblamed.empty());
  o.sus = std::move(leftover);
}

void Blame::find_copy(std::vector<std::pair<int, Ent>>& blamed, std::vector<Ent>& toosmall, int org, int parent, int porigin) {
  std::vector<Ent> unblamed = origins_[(size_t)org].sus, leftover;
  if (unblamed.empty()) return;
  const std::string org_path = origins_[(size_t)org].path;
  Snapshot a = snapshot_tree(c_, commits_[(size_t)parent].tree);
  Snapshot b = snapshot_tree(c_, commits_[(size_t)origins_[(size_t)org].commit].tree);
  bool harder = copy_harder_ && (porigin < 0 || origins_[(size_t)porigin].path != org_path);
  std::vector<std::pair<std::string, FileState>> cands;
  for (auto& [path, fs] : a) {
    if (fs.mode == 0160000) continue;
    auto it = b.find(path);
    bool same = it != b.end() && it->second.oid == fs.oid && it->second.mode == fs.mode;
    if (same && !harder) continue;
    if (porigin >= 0 && path == origins_[(size_t)porigin].path) continue;
    cands.push_back({path, fs});
  }
  do {
    std::vector<std::array<Ent, 3>> best(unblamed.size());
    std::vector<char> have(unblamed.size(), 0);
    for (auto& [path, fs] : cands) {
      int norg = origin_of(parent, path);
      origins_[(size_t)norg].blob = fs.oid;
      origins_[(size_t)norg].mode = fs.mode;
      text_of(norg);
      for (size_t j = 0; j < unblamed.size(); ++j) {
        Ent split[3];
        bool found;
        best_run(unblamed[j], norg, split, &found);
        if (!found) continue;
        if (have[j] && score_of(split[1]) < score_of(best[j][1])) continue;
        have[j] = 1;
        best[j] = {split[0], split[1], split[2]};
      }
      drop_text(norg);
    }
    std::vector<Ent> next_round;
    for (size_t j = 0; j < unblamed.size(); ++j) {
      if (have[j] && copy_score_ < score_of(best[j][1])) {
        blamed.push_back({best[j][1].org, best[j][1]});
        if (best[j][0].n > 0) next_round.push_back(best[j][0]);
        if (best[j][2].n > 0) next_round.push_back(best[j][2]);
      } else {
        leftover.push_back(unblamed[j]);
      }
    }
    unblamed = std::move(next_round);
    filter_small(toosmall, unblamed, copy_score_);
  } while (!unblamed.empty());
  origins_[(size_t)org].sus = std::move(leftover);
}

void Blame::pass_blame(int org) {
  const int commit = origins_[(size_t)org].commit;
  std::vector<std::string> parents = commits_[(size_t)commit].parents;
  if (first_parent_ && parents.size() > 1) parents.resize(1);
  if (parents.empty()) return;
  std::vector<int> pc;
  for (auto& p : parents) pc.push_back(commit_of(p));
  std::vector<int> sg(pc.size(), -1);
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < pc.size(); ++i) {
      if (sg[i] >= 0) continue;
      int p = pass ? find_rename(pc[i], org) : find_origin(pc[i], org);
      if (p < 0) continue;
      if (origins_[(size_t)p].blob == origins_[(size_t)org].blob) {
        pass_whole(org, p);
        return;
      }
      bool same = false;
      for (size_t j = 0; j < i; ++j) if (sg[j] >= 0 && origins_[(size_t)sg[j]].blob == origins_[(size_t)p].blob) same = true;
      if (!same) sg[i] = p;
    }
  }
  std::vector<std::pair<int, Ent>> blamed;
  std::vector<Ent> toosmall;
  auto finish = [&]() {
    std::map<int, std::vector<Ent>> by;
    for (auto& [po, e] : blamed) by[po].push_back(e);
    for (auto& [po, v] : by) queue_blames(po, v);
    for (auto& e : toosmall) origins_[(size_t)org].sus.push_back(e);
    for (int p : sg) if (p >= 0) drop_text(p);
  };
  for (size_t i = 0; i < pc.size(); ++i) {
    if (sg[i] < 0) continue;
    Origin& o = origins_[(size_t)org];
    if (o.previous < 0) o.previous = sg[i];
    pass_to_parent(org, sg[i]);
    if (origins_[(size_t)org].sus.empty()) { finish(); return; }
  }
  if (move_) {
    filter_small(toosmall, origins_[(size_t)org].sus, move_score_);
    if (!origins_[(size_t)org].sus.empty())
      for (size_t i = 0; i < pc.size(); ++i)
        if (sg[i] >= 0) find_move(blamed, toosmall, org, sg[i]);
  }
  if (copy_) {
    filter_small(toosmall, origins_[(size_t)org].sus, copy_score_);
    if (!origins_[(size_t)org].sus.empty()) {
      for (size_t i = 0; i < pc.size(); ++i) {
        find_copy(blamed, toosmall, org, pc[i], sg[i]);
        if (origins_[(size_t)org].sus.empty()) break;
      }
    }
  }
  finish();
}

const Meta& Blame::meta(int commit) {
  CommitRec& r = commits_[(size_t)commit];
  auto it = meta_cache_.find(r.oid);
  if (it != meta_cache_.end()) return it->second;
  Meta m;
  if (r.fake) {
    m = r.fake_meta;
  } else {
    IdentParts a = split_ident(r.cm.author), cmt = split_ident(r.cm.committer);
    mailmap_.map(a.name, a.email);
    mailmap_.map(cmt.name, cmt.email);
    m.author = a.name;
    m.author_mail = "<" + a.email + ">";
    m.committer = cmt.name;
    m.committer_mail = "<" + cmt.email + ">";
    m.at = (int64_t)r.cm.author_time;
    m.ct = (int64_t)r.cm.committer_time;
    m.atz = r.cm.author_tz;
    m.ctz = r.cm.committer_tz;
    const std::string& msg = r.cm.raw_message;
    size_t at = 0;
    while (at < msg.size() && msg[at] == '\n') ++at;
    size_t eol = msg.find('\n', at);
    m.summary = msg.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
  }
  return meta_cache_.emplace(r.oid, m).first->second;
}

void Blame::emit_details(int org, bool repeat, bool incremental_always) {
  Origin& o = origins_[(size_t)org];
  CommitRec& r = commits_[(size_t)o.commit];
  bool first = repeat || !r.shown;
  if (first) {
    r.shown = true;
    const Meta& m = meta(o.commit);
    c_.out << "author " << m.author << "\nauthor-mail " << m.author_mail << "\nauthor-time " << m.at << "\nauthor-tz " << m.atz
           << "\ncommitter " << m.committer << "\ncommitter-mail " << m.committer_mail << "\ncommitter-time " << m.ct
           << "\ncommitter-tz " << m.ctz << "\nsummary " << m.summary << "\n";
    if (r.boundary) c_.out << "boundary\n";
  }
  if (first || r.multi_path || incremental_always) {
    if (o.previous >= 0) {
      const Origin& p = origins_[(size_t)o.previous];
      c_.out << "previous " << commits_[(size_t)p.commit].oid << " " << git_name(p.path) << "\n";
    }
    c_.out << "filename " << git_name(o.path) << "\n";
  }
}

void Blame::emit_incremental(const Ent& e) {
  const Origin& o = origins_[(size_t)e.org];
  c_.out << commits_[(size_t)o.commit].oid << " " << e.s_lno + 1 << " " << e.lno + 1 << " " << e.n << "\n";
  emit_details(e.org, false, true);
  c_.out.flush();
}

void Blame::assign() {
  while (!queue_.empty()) {
    int commit = std::get<2>(queue_.top());
    queue_.pop();
    commits_[(size_t)commit].queued = false;
    std::vector<int> origs = commits_[(size_t)commit].origins;
    for (int org : origs) {
      if (origins_[(size_t)org].sus.empty()) continue;
      const int cx = origins_[(size_t)org].commit;
      bool date_stop = has_max_age_ && !commits_[(size_t)cx].fake && commits_[(size_t)cx].date < max_age_;
      if (!commits_[(size_t)cx].boundary && !date_stop) {
        pass_blame(org);
      } else {
        commits_[(size_t)cx].boundary = true;
        std::vector<std::string> ps = commits_[(size_t)cx].parents;
        for (auto& p : ps) {
          auto it = commit_ix_.find(p);
          if (it != commit_ix_.end()) commits_[(size_t)it->second].boundary = true;
          else hidden_.insert(p);
        }
      }
      if (commits_[(size_t)cx].parents.empty() && !show_root_) commits_[(size_t)cx].boundary = true;
      std::vector<Ent> mine;
      mine.swap(origins_[(size_t)org].sus);
      for (auto& e : mine) {
        e.org = org;
        done_.push_back(e);
        if (incremental_) emit_incremental(e);
      }
      drop_text(org);
    }
    if (!commits_[(size_t)commit].queued) {
      bool more = false;
      for (int org : commits_[(size_t)commit].origins) if (!origins_[(size_t)org].sus.empty()) more = true;
      if (more) put_commit(commit);
    }
  }
}

void Blame::output() {
  std::sort(done_.begin(), done_.end(), [](const Ent& a, const Ent& b) { return a.lno < b.lno; });
  std::vector<Ent> ents;
  for (auto& e : done_) {
    if (!ents.empty() && ents.back().org == e.org && ents.back().s_lno + ents.back().n == e.s_lno) ents.back().n += e.n;
    else ents.push_back(e);
  }
  const bool porcelain = opt_ & kOutputPorcelain;
  const bool repeat = opt_ & kOutputLinePorcelain;
  const char* raw_mode = "raw";
  if (porcelain) {
    for (auto& e : ents) {
      const Origin& o = origins_[(size_t)e.org];
      const std::string& hex = commits_[(size_t)o.commit].oid;
      c_.out << hex << " " << e.s_lno + 1 << " " << e.lno + 1 << " " << e.n << "\n";
      emit_details(e.org, repeat, false);
      for (int cnt = 0; cnt < e.n; ++cnt) {
        if (cnt) {
          c_.out << hex << " " << e.s_lno + 1 + cnt << " " << e.lno + 1 + cnt << "\n";
          if (repeat) emit_details(e.org, true, false);
        }
        c_.out << '\t';
        c_.out << final_lines_[(size_t)(e.lno + cnt)];
      }
      if (!final_text_.empty() && final_text_.back() != '\n' && (size_t)(e.lno + e.n) == final_lines_.size()) c_.out << '\n';
    }
    return;
  }
  long longest_src = 0, longest_dst = 0;
  size_t longest_file = 0;
  int longest_author = 0;
  int abbrev = abbrev_;
  const bool compute_abbrev = abbrev < 0;
  int auto_abbrev = c_.default_abbrev();
  uint32_t opt = opt_;
  std::set<int> counted;
  for (auto& e : ents) {
    const Origin& o = origins_[(size_t)e.org];
    CommitRec& cr = commits_[(size_t)o.commit];
    if (compute_abbrev && !cr.fake) {
      std::string u = c_.abbrev(cr.oid, auto_abbrev);
      if ((int)u.size() > auto_abbrev) auto_abbrev = (int)u.size();
    }
    if (o.path != path_) opt |= kOutputName;
    longest_file = std::max(longest_file, o.path.size());
    if (!counted.count(o.commit)) {
      counted.insert(o.commit);
      const Meta& m = meta(o.commit);
      int w = utf8_width((opt & kOutputEmail) ? m.author_mail : m.author);
      longest_author = std::max(longest_author, w);
    }
    longest_src = std::max<long>(longest_src, e.s_lno + e.n);
    longest_dst = std::max<long>(longest_dst, e.lno + e.n);
  }
  int max_orig_digits = digits_of(longest_src), max_digits = digits_of(longest_dst);
  if (compute_abbrev) abbrev = auto_abbrev + 1;
  else if (abbrev != 0) abbrev = std::min(abbrev + 1, 40);
  const bool blank_boundary = opt_ & kOutputBlank || c_.config.get_bool("blame.blankboundary", false);
  (void)raw_mode;
  // builtin/blame.c's coloring: --color-lines / --color-by-age, else blame.coloring; never with -c
  if (!(opt & (kOutputColorLine | kOutputAgeColor)))
    if (auto mode = c_.config.get("blame.coloring")) {
      if (*mode == "repeatedLines") opt |= kOutputColorLine;
      else if (*mode == "highlightRecent") opt |= kOutputAgeColor;
      else if (*mode != "none") c_.err << "warning: invalid value for 'blame.coloring': '" << *mode << "'\n";
    }
  if (opt & kOutputAnnotate) opt &= ~(kOutputColorLine | kOutputAgeColor);
  std::string repeated = "\033[36m";
  if (auto v = c_.config.get("color.blame.repeatedlines"))
    if (auto col = parse_git_color(*v)) repeated = *col;
  // parse_color_fields: color, date, color, ... ending in a color; a line's color is the first whose date it is not after
  std::vector<std::pair<int64_t, std::string>> heat;
  if (opt & kOutputAgeColor) {
    const std::string spec = c_.config.get("color.blame.highlightrecent").value_or("blue,12 month ago,white,1 month ago,red");
    bool want_color = true;
    std::string col;
    size_t at = 0;
    while (at <= spec.size()) {
      size_t comma = spec.find(',', at);
      if (comma == std::string::npos) comma = spec.size();
      const std::string item = spec.substr(at, comma - at);
      if (want_color) {
        auto parsed = parse_git_color(item);
        if (!parsed) die(c_, "expecting a color: " + item);
        col = *parsed;
      } else {
        int64_t t = 0;
        if (!parse_approxidate(item, t)) die(c_, "invalid date: " + item);
        heat.emplace_back(t, col);
      }
      want_color = !want_color;
      at = comma + 1;
    }
    if (want_color) die(c_, "must end with a color");
    heat.emplace_back(INT64_MAX, col);
  }
  for (auto& e : ents) {
    const Origin& o = origins_[(size_t)e.org];
    const CommitRec& cr = commits_[(size_t)o.commit];
    const Meta& m = meta(o.commit);
    std::string hex = cr.oid;
    // determine_line_heat: the entry's age color; --color-lines paints the lines after an entry's first
    std::string default_color, color, reset;
    if (opt & kOutputAgeColor) {
      size_t i = 0;
      while (i + 1 < heat.size() && (int64_t)m.at > heat[i].first) ++i;
      default_color = color = heat[i].second;
      reset = kColorReset;
    }
    for (int cnt = 0; cnt < e.n; ++cnt) {
      int length = (opt & kOutputLong) ? 40 : (abbrev == 0 ? 40 : abbrev);
      if (opt & kOutputColorLine) {
        if (cnt > 0) { color = repeated; reset = kColorReset; }
        else { color = default_color; reset = default_color.empty() ? "" : kColorReset; }
      }
      c_.out << color;
      std::string head = hex;
      if (cr.boundary) {
        if (blank_boundary) head.assign((size_t)length, ' ');
        else if (!(opt & kOutputAnnotate)) { --length; c_.out << '^'; }
      }
      c_.out << head.substr(0, (size_t)length);
      std::string when;
      if (opt & kOutputRawTime) {
        when = blame_date(m.at, m.atz, "raw");
      } else {
        when = blame_date(m.at, m.atz, date_mode_);
        size_t shown = (size_t)utf8_width(when);
        if (shown < date_width(date_mode_)) when.append(date_width(date_mode_) - shown, ' ');
      }
      auto pad_left = [](const std::string& s, size_t w) { return s.size() < w ? std::string(w - s.size(), ' ') + s : s; };
      if (opt & kOutputAnnotate) {
        const std::string& name = (opt & kOutputEmail) ? m.author_mail : m.author;
        c_.out << "\t(" << pad_left(name, 10) << "\t" << pad_left(when, 10) << "\t" << e.lno + 1 + cnt << ")";
      } else {
        if (opt & kOutputName) {
          std::string p = o.path;
          if (p.size() < longest_file) p += std::string(longest_file - p.size(), ' ');
          c_.out << " " << p;
        }
        if (opt & kOutputNumber) {
          std::string n = std::to_string(e.s_lno + 1 + cnt);
          c_.out << " " << std::string((size_t)std::max<int>(0, max_orig_digits - (int)n.size()), ' ') << n;
        }
        if (!(opt & kOutputNoAuthor)) {
          const std::string& name = (opt & kOutputEmail) ? m.author_mail : m.author;
          int pad = longest_author - utf8_width(name);
          c_.out << " (" << name << std::string((size_t)std::max(0, pad), ' ') << " " << pad_left(when, 10);
        }
        std::string ln = std::to_string(e.lno + 1 + cnt);
        c_.out << " " << std::string((size_t)std::max<int>(0, max_digits - (int)ln.size()), ' ') << ln << ") ";
      }
      c_.out << reset;
      c_.out << final_lines_[(size_t)(e.lno + cnt)];
    }
    if (!final_text_.empty() && final_text_.back() != '\n' && (size_t)(e.lno + e.n) == final_lines_.size()) c_.out << '\n';
  }
}

bool Blame::func_range(const std::string& arg, long lines, long anchor, long& begin, long& end) {
  std::string a = arg;
  if (!a.empty() && a[0] == '^') { anchor = 1; a = a.substr(1); }
  std::string pat = a.substr(1);
  std::regex re;
  try {
    re = std::regex(pat, std::regex::basic);
  } catch (const std::regex_error&) {
    die(c_, "-L parameter '" + pat + "': invalid regular expression");
  }
  auto is_func = [&](std::string_view l) { return !l.empty() && (std::isalpha((unsigned char)l[0]) || l[0] == '$' || l[0] == '_'); };
  long start = -1;
  for (long l = std::max(1L, anchor); l <= lines; ++l) {
    std::string_view v = final_lines_[(size_t)(l - 1)];
    if (!is_func(v)) continue;
    std::string s(v);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    if (std::regex_search(s, re)) { start = l; break; }
  }
  if (start < 0) die(c_, "-L parameter '" + pat + "' starting at line " + std::to_string(std::max(1L, anchor)) + ": no match");
  long stop = lines;
  for (long l = start + 1; l <= lines; ++l)
    if (is_func(final_lines_[(size_t)(l - 1)])) { stop = l - 1; break; }
  while (stop > start) {
    std::string_view v = final_lines_[(size_t)(stop - 1)];
    bool blank = true;
    for (char ch : v) if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') blank = false;
    if (!blank) break;
    --stop;
  }
  begin = start;
  end = stop;
  return true;
}

long Blame::parse_loc(const std::string& spec, size_t& pos, long begin, bool is_end, bool& have, bool from_start) {
  have = true;
  (void)is_end;
  if (begin > 1 && pos < spec.size() && (spec[pos] == '+' || spec[pos] == '-')) {
    char* term = nullptr;
    long num = std::strtol(spec.c_str() + pos + 1, &term, 10);
    if (term != spec.c_str() + pos + 1) {
      if (num == 0) die(c_, "-L invalid empty range");
      if (spec[pos] == '-') num = -num;
      pos = (size_t)(term - spec.c_str());
      if (num > 0) return begin + num - 2;
      if (-num >= begin) return 1;
      return begin + num;
    }
    have = false;
    return 0;
  }
  char* term = nullptr;
  long num = std::strtol(spec.c_str() + pos, &term, 10);
  if (term != spec.c_str() + pos) {
    if (num <= 0) die(c_, "-L invalid line number: " + std::to_string(num));
    pos = (size_t)(term - spec.c_str());
    return num;
  }
  size_t p = pos;
  if (p < spec.size() && spec[p] == '^' && p + 1 < spec.size() && spec[p + 1] == '/') { from_start = true; ++p; }
  if (p >= spec.size() || spec[p] != '/') { have = false; return 0; }
  size_t t = p + 1;
  while (t < spec.size() && spec[t] != '/') { if (spec[t] == '\\') ++t; ++t; }
  if (t >= spec.size()) { have = false; return 0; }
  std::string pat = spec.substr(p + 1, t - p - 1);
  long line0 = from_start ? 0 : std::max(1L, begin) - 1;
  std::regex re;
  try {
    re = std::regex(pat, std::regex::basic);
  } catch (const std::regex_error&) {
    die(c_, "-L parameter '" + pat + "': invalid regular expression");
  }
  for (long l = line0; l < (long)final_lines_.size(); ++l) {
    std::string s(final_lines_[(size_t)l]);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    if (std::regex_search(s, re)) { pos = t + 1; return l + 1; }
  }
  die(c_, "-L parameter '" + pat + "' starting at line " + std::to_string(line0 + 1) + ": regexec() failed to match");
}

bool Blame::parse_range_arg(const std::string& arg, long lines, long anchor, long& begin, long& end) {
  begin = end = 0;
  if (anchor < 1) anchor = 1;
  if (anchor > lines) anchor = lines + 1;
  if (!arg.empty() && (arg[0] == ':' || (arg[0] == '^' && arg.size() > 1 && arg[1] == ':'))) return func_range(arg, lines, anchor, begin, end);
  size_t pos = 0;
  bool have = false;
  long b = parse_loc(arg, pos, -anchor, false, have, false);
  if (have) begin = b;
  if (pos < arg.size() && arg[pos] == ',') {
    ++pos;
    bool have_end = false;
    long e = parse_loc(arg, pos, begin + 1, true, have_end, false);
    if (have_end) end = e;
  }
  if (pos < arg.size()) return false;
  if (begin && end && end < begin) std::swap(begin, end);
  return true;
}

void Blame::load_ranges(long lines, std::vector<std::pair<long, long>>& out) {
  long anchor = 1;
  std::vector<std::pair<long, long>> got;
  for (auto& r : ranges_) {
    long bottom, top;
    if (!parse_range_arg(r, lines, anchor, bottom, top)) range_usage();
    if ((!lines && (top || bottom)) || lines < bottom)
      die(c_, std::string("file ") + path_ + " has only " + std::to_string(lines) + (lines == 1 ? " line" : " lines"));
    if (bottom < 1) bottom = 1;
    if (top < 1 || lines < top) top = lines;
    got.push_back({bottom - 1, top});
    anchor = top + 1;
  }
  std::sort(got.begin(), got.end());
  for (auto& g : got) {
    if (!out.empty() && g.first <= out.back().second) out.back().second = std::max(out.back().second, g.second);
    else out.push_back(g);
  }
}

int Blame::fake_commit(const std::string& from, const std::string& base, bool external) {
  CommitRec r;
  r.fake = true;
  r.oid = std::string(40, '0');
  r.boundary = false;
  if (!base.empty()) r.parents.push_back(base);
  if (!external)
    if (auto m = read_ref(c_, "MERGE_HEAD")) r.parents.push_back(*m);
  int64_t now = now_seconds();
  std::string tz = git_local_tz((uint64_t)now);
  r.date = now;
  const char* who = external ? "External file (--contents)" : "Not Committed Yet";
  const char* mail = external ? "<external.file>" : "<not.committed.yet>";
  r.fake_meta.author = r.fake_meta.committer = who;
  r.fake_meta.author_mail = r.fake_meta.committer_mail = mail;
  r.fake_meta.at = r.fake_meta.ct = now;
  r.fake_meta.atz = r.fake_meta.ctz = tz;
  r.fake_meta.summary = "Version of " + path_ + " from " + from;
  if (!base.empty()) r.tree = read_commit(c_, base).tree;
  commits_.push_back(std::move(r));
  int ix = (int)commits_.size() - 1;
  commit_ix_[commits_[(size_t)ix].oid] = ix;
  return ix;
}

int Blame::run() {
  c_.open_repo(false);
  std::vector<std::string> revs;
  parse_args(revs);
  if (c_.config.get_bool("blame.showemail", false) && !(opt_ & kOutputEmail)) opt_ |= kOutputEmail;
  if (c_.config.get_bool("blame.showroot", false)) show_root_ = true;
  if (auto d = c_.config.get("blame.date")) {
    if (date_mode_ == "iso") {
      if (*d == "human") unsupported(c_, "blame.date=human");
      date_mode_ = *d;
    }
  }
  if (auto ig = c_.config.get("blame.ignorerevsfile")) if (!ig->empty()) unsupported(c_, "blame.ignoreRevsFile");
  if (!c_.config.get_all("blame.ignorerevs").empty()) unsupported(c_, "blame.ignoreRevs");
  mailmap_.load(c_);

  const bool path_slash = path_.size() > 1 && path_.back() == '/';
  std::string rel = c_.to_repo_path(path_);
  if (rel == "..") die(c_, "'" + path_ + "' is outside repository at '" + c_.top + "'");
  if (rel == ".") rel = "";
  path_ = rel;
  const std::string path_shown = rel + (path_slash && !rel.empty() ? "/" : "");

  WalkOptions w;
  std::vector<std::string> leftover = parse_revisions(c_, revs, w, false);
  if (!leftover.empty()) die(c_, "bad revision '" + leftover[0] + "'");
  std::string final_oid;
  bool worktree_final = false;
  if (!w.hide.empty()) {
    WalkOptions h;
    h.tips = w.hide;
    for (auto& cm : walk(c_, h)) hidden_.insert(cm.oid);
  }
  std::vector<std::string> tips;
  for (auto& t : w.tips) if (!hidden_.count(t) && std::find(tips.begin(), tips.end(), t) == tips.end()) tips.push_back(t);
  if (tips.size() > 1) die(c_, "More than one commit to dig from " + tips[0] + " and " + tips[1] + "?");
  std::string base_oid;
  if (!tips.empty()) base_oid = tips[0];
  else if (auto head = read_ref(c_, "HEAD")) base_oid = *head;
  std::string head_blob;
  uint32_t head_mode = 0100644;
  bool in_head = false;
  if (!base_oid.empty()) {
    GitCommit bc = read_commit(c_, base_oid);
    in_head = find_path(bc.tree, path_, head_blob, head_mode) && head_mode != 040000 && head_mode != 0160000;
  }
  if (!contents_from_.empty()) {
    worktree_final = true;
  } else if (tips.empty()) {
    {
      bool in_index = false;
      for (auto& e : c_.repo->index()) if (e.path == path_) in_index = true;
      if (!in_index && !in_head) die(c_, "no such path '" + path_shown + "' in HEAD");
      auto st = c_.host().stat(join(c_.top, path_));
      if (!st.ok()) die(c_, "Cannot lstat '" + path_shown + "': No such file or directory");
      worktree_final = true;
    }
  } else {
    final_oid = tips[0];
  }

  std::string final_blob_oid;
  uint32_t mode = 0100644;
  if (worktree_final) {
    std::string from = contents_from_.empty() ? path_ : contents_from_;
    if (!contents_from_.empty()) {
      if (contents_from_ == "-") {
        final_text_ = read_all(c_.in);
      } else {
        auto t = c_.host().read_file(c_.abs_path(contents_from_));
        if (!t.ok()) die(c_, "Cannot stat '" + contents_from_ + "': No such file or directory");
        final_text_ = std::string((const char*)t->data(), t->size());
      }
      if (in_head) mode = head_mode;
    } else {
      bool ok = false;
      final_text_ = read_worktree_file(c_, path_, &ok);
      if (!ok) die(c_, "Cannot lstat '" + path_shown + "': No such file or directory");
      auto st = c_.host().stat(join(c_.top, path_));
      if (st.ok() && st->is_link) mode = 0120000;
      else if (st.ok() && st->is_exec) mode = 0100755;
    }
    final_commit_ = fake_commit(from, base_oid, !contents_from_.empty());
    final_blob_oid = hash_blob(final_text_);
  } else {
    final_commit_ = commit_of(final_oid);
    std::string oid;
    if (!find_path(commits_[(size_t)final_commit_].tree, path_, oid, mode) || mode == 040000 || mode == 0160000)
      die(c_, "no such path " + path_shown + " in HEAD");
    final_blob_oid = oid;
    final_text_ = blob_text(c_, oid);
  }
  final_lines_ = git_split_lines(final_text_);
  const long nlines = (long)final_lines_.size();

  std::vector<std::pair<long, long>> ranges;
  if (ranges_.empty()) { if (nlines) ranges.push_back({0, nlines}); }
  else load_ranges(nlines, ranges);

  int org = origin_of(final_commit_, path_);
  origins_[(size_t)org].blob = final_blob_oid;
  origins_[(size_t)org].mode = mode;
  if (worktree_final) {
    origins_[(size_t)org].text = final_text_;
    origins_[(size_t)org].loaded = true;
  }
  std::vector<Ent> init;
  for (auto& r : ranges) init.push_back(Ent{(int)r.first, (int)r.first, (int)(r.second - r.first), org});
  if (init.empty()) return 0;
  queue_blames(org, init);
  assign();
  if (!incremental_) output();
  return 0;
}

}  // namespace

int cmd_blame(Ctx& c) {
  Blame b(c);
  return b.run();
}

}  // namespace aios_git
