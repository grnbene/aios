// Diffs between snapshots (trees, the index, the work tree) and git's ways of printing them.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_map>

#include "aios/userland/git_structural.hpp"
#include "cli.hpp"
#include "color.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

bool is_binary(const std::string& s) {
  size_t n = std::min<size_t>(s.size(), 8000);
  return std::memchr(s.data(), 0, n) != nullptr;
}

std::pair<long, long> count_changes(const std::string& a, const std::string& b) { return count_changes(a, b, GitDiffTextOptions{}); }

std::pair<long, long> count_changes(const std::string& a, const std::string& b, const GitDiffTextOptions& opt) {
  if (a == b) return {0, 0};
  // git's diffstat diffs with the patch's own context (o->context, -U): with none, xdi_diff trims the common tail
  // first (trim_common_tail), which can move the changes - 1235 lines counted where git counts 1233.
  GitDiffTextOptions o = opt;
  o.function_context = false;
  std::string h = git_unified_hunks(a, b, o);
  long add = 0, del = 0;
  size_t i = 0;
  while (i < h.size()) {
    size_t nl = h.find('\n', i);
    if (nl == std::string::npos) nl = h.size();
    if (nl > i) {
      char ch = h[i];
      if (ch == '+') ++add;
      else if (ch == '-') ++del;
    }
    i = nl + 1;
  }
  return {add, del};
}

// diffcore-delta.c: a file as counts of its spans (a line, or 64 bytes of a longer one) by their hash. What two such
// counts share is "copied", what only the second has is "added" - the damage --dirstat weighs a file by.
std::map<unsigned, unsigned> span_counts(const std::string& data, bool is_text) {
  std::map<unsigned, unsigned> out;
  const unsigned char* buf = (const unsigned char*)data.data();
  size_t sz = data.size();
  unsigned n = 0, accum1 = 0, accum2 = 0;
  constexpr unsigned kHashBase = 107927;
  while (sz) {
    unsigned c = *buf++;
    unsigned old_1 = accum1;
    --sz;
    if (is_text && c == '\r' && sz && *buf == '\n') continue;   // CR of a CRLF is not counted in text
    accum1 = (accum1 << 7) ^ (accum2 >> 25);
    accum2 = (accum2 << 7) ^ (old_1 >> 25);
    accum1 += c;
    if (++n < 64 && c != '\n') continue;
    out[(accum1 + accum2 * 0x61) % kHashBase] += n;
    n = 0;
    accum1 = accum2 = 0;
  }
  if (n > 0) out[(accum1 + accum2 * 0x61) % kHashBase] += n;
  return out;
}

void count_copied_added(const std::string& src, bool src_text, const std::string& dst, bool dst_text,
                        unsigned long& copied, unsigned long& added) {
  auto s = span_counts(src, src_text), d = span_counts(dst, dst_text);
  copied = added = 0;
  auto di = d.begin();
  for (auto& [h, src_cnt] : s) {
    for (; di != d.end() && di->first < h; ++di) added += di->second;
    unsigned dst_cnt = 0;
    if (di != d.end() && di->first == h) { dst_cnt = di->second; ++di; }
    if (src_cnt < dst_cnt) { added += dst_cnt - src_cnt; copied += src_cnt; }
    else copied += dst_cnt;
  }
  for (; di != d.end(); ++di) added += di->second;
}

// diff.c's gather_dirstat: directories by their share of the damage, those at the limit or above printed (and, unless
// cumulative, not counted again in their parents). Files come sorted by name.
struct DirstatFile { std::string name; unsigned long changed; };
unsigned long gather_dirstat(std::vector<DirstatFile>& files, size_t& at, unsigned long changed, const std::string& base,
                             int permille_limit, bool cumulative, std::ostream& out) {
  unsigned long sum = 0;
  unsigned sources = 0;
  while (at < files.size()) {
    const DirstatFile& f = files[at];
    if (f.name.size() < base.size() || f.name.compare(0, base.size(), base) != 0) break;
    size_t slash = f.name.find('/', base.size());
    unsigned long changes;
    if (slash != std::string::npos) {
      changes = gather_dirstat(files, at, changed, f.name.substr(0, slash + 1), permille_limit, cumulative, out);
      ++sources;
    } else {
      changes = f.changed;
      ++at;
      sources += 2;
    }
    sum += changes;
  }
  if (!base.empty() && sources != 1 && sum) {
    const int permille = (int)(sum * 1000 / changed);
    if (permille >= permille_limit) {
      char line[32];
      std::snprintf(line, sizeof line, "%4d.%01d%% ", permille / 10, permille % 10);
      out << line << base << "\n";
      if (!cumulative) return 0;
    }
  }
  return sum;
}

std::optional<GitSyntaxOutline> pair_outline(Ctx& c, const DiffPair& p, bool new_side) {
  const std::string& path = new_side ? p.new_path : p.old_path;
  if (new_side && p.b_text) return git_structural_outline(path, *p.b_text);
  const FileState& f = new_side ? p.b : p.a;
  if (!f.present || f.mode == 0160000) return std::nullopt;
  return c.repo->outline(f.oid, path);
}

std::string pair_text(Ctx& c, const DiffPair& p, bool new_side) {
  if (new_side && p.b_text) return *p.b_text;
  if (!new_side && p.a_text) return *p.a_text;
  const FileState& f = new_side ? p.b : p.a;
  if (!f.present || f.mode == 0160000) return "";
  return blob_text(c, f.oid);
}

namespace {
int parse_color_moved(std::string v);   // --color-moved's values (below, with the move detection)

char status_of(const FileState& a, const FileState& b) {
  if (!a.present) return 'A';
  if (!b.present) return 'D';
  auto kind = [](uint32_t m) { return m == 0120000 ? 1 : m == 0160000 ? 2 : 0; };
  if (kind(a.mode) != kind(b.mode)) return 'T';
  return 'M';
}

std::string mode_str(uint32_t m) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06o", m);
  return buf;
}

// git's diffcore-rename, the parts that decide output: exact matches first, then the best similarity
// at or above the threshold, each source used once.
// diff.c's parse_rename_score: "-M5" is 0.5, "-M55" 0.55, "-M5%" 5%; MAX_SCORE (60000) times that.
int parse_rename_score(const std::string& s) {
  unsigned long num = 0, scale = 1;
  bool dot = false;
  for (char ch : s) {
    if (!dot && ch == '.') { scale = 1; dot = true; }
    else if (ch == '%') { scale = dot ? scale * 100 : 100; break; }
    else if (ch >= '0' && ch <= '9') { if (scale < 100000) { scale *= 10; num = num * 10 + (unsigned long)(ch - '0'); } }
    else break;
  }
  return (int)(num >= scale ? 60000 : 60000 * num / scale);
}

// diffcore-rename.c and diff_resolve_rename_copy (git 2.43), on git's scale (MAX_SCORE 60000).
//   sources: the deleted files, with -C the modified files' old sides too, with -C -C every unchanged file as well;
//   destinations: the added files; both in path order.
//   1. exact: the same blob, a source not used yet first, then one of the same basename;
//   2. renames only: a source and a destination whose basenames are each unique pair up at a higher bar;
//   3. the matrix (within diff.renameLimit): each destination's four best sources, all sorted by score, a same
//      basename breaking ties; renames are taken from deleted sources first, then (-C) copies from any.
//   A deleted source used several times is a copy for all but its last destination; a source that stays is a copy.
void find_renames(Ctx& c, std::vector<DiffPair>& pairs, const DiffOptions& o, const Snapshot& old_side) {
  constexpr int kMax = 60000;
  const int minimum = o.rename_score >= 0 ? o.rename_score : o.rename_threshold * kMax / 100;
  const bool copies = o.copies || o.copies_harder;
  struct Src { std::string path; FileState st; long pair = -1; int used = 0; bool deleted = false, unmodified = false; };
  std::vector<Src> srcs;
  std::vector<size_t> dsts;
  std::set<std::string> in_pairs;
  for (size_t i = 0; i < pairs.size(); ++i) {
    const DiffPair& p = pairs[i];
    in_pairs.insert(p.old_path);
    if (p.status == 'U') continue;
    if (p.status == 'A') { dsts.push_back(i); continue; }
    if (p.status == 'D') { srcs.push_back(Src{p.old_path, p.a, (long)i, 0, true, false}); continue; }
    if (copies && p.a.present && p.b.present) srcs.push_back(Src{p.old_path, p.a, (long)i, 1, false, false});
  }
  if (o.copies_harder)
    for (auto& [path, st] : old_side)
      if (!in_pairs.count(path)) srcs.push_back(Src{path, st, -1, 1, false, true});
  if (srcs.empty() || dsts.empty()) return;
  std::stable_sort(srcs.begin(), srcs.end(), [](const Src& x, const Src& y) { return x.path < y.path; });
  std::sort(dsts.begin(), dsts.end(), [&](size_t x, size_t y) { return pairs[x].new_path < pairs[y].new_path; });
  std::vector<long> dst_src(pairs.size(), -1);   // destination pair -> source index, once matched
  std::vector<int> dst_score(pairs.size(), 0);
  auto record = [&](size_t ai, size_t si, int score) { dst_src[ai] = (long)si; dst_score[ai] = score; ++srcs[si].used; };
  auto regular = [](uint32_t m) { return (m & 0170000) == 0100000; };
  auto base_of = [](const std::string& p) { size_t s = p.rfind('/'); return s == std::string::npos ? p : p.substr(s + 1); };

  for (size_t ai : dsts) {   // 1. exact
    long best = -1;
    int best_score = -1;
    for (size_t si = 0; si < srcs.size(); ++si) {
      const Src& sr = srcs[si];
      if (sr.st.oid != pairs[ai].b.oid) continue;
      if ((!regular(sr.st.mode) || !regular(pairs[ai].b.mode)) && sr.st.mode != pairs[ai].b.mode) continue;
      if (sr.used && !copies) continue;
      const int score = (sr.used ? 0 : 1) + (base_of(sr.path) == base_of(pairs[ai].new_path) ? 1 : 0);
      if (score > best_score) { best = (long)si; best_score = score; if (score == 2) break; }
    }
    if (best >= 0) record(ai, (size_t)best, kMax);
  }

  if (minimum < kMax) {
    std::map<size_t, std::string> src_text, dst_text;
    auto stext = [&](size_t si) -> const std::string& {
      auto it = src_text.find(si);
      if (it != src_text.end()) return it->second;
      const Src& sr = srcs[si];
      return src_text[si] = sr.pair >= 0 ? pair_text(c, pairs[(size_t)sr.pair], false) : blob_text(c, sr.st.oid);
    };
    auto dtext = [&](size_t ai) -> const std::string& {
      auto it = dst_text.find(ai);
      if (it != dst_text.end()) return it->second;
      return dst_text[ai] = pair_text(c, pairs[ai], true);
    };
    std::map<size_t, GitSpanCounts> src_counts, dst_counts;
    auto estimate = [&](size_t si, size_t ai, int floor) -> int {   // estimate_similarity
      if (!regular(srcs[si].st.mode) || !regular(pairs[ai].b.mode)) return 0;
      const uint64_t x = stext(si).size(), y = dtext(ai).size();
      const uint64_t big = std::max(x, y), small = std::min(x, y);
      if (big * (uint64_t)(kMax - floor) < (big - small) * (uint64_t)kMax) return 0;
      if (!y) return 0;
      auto sc = src_counts.find(si);
      if (sc == src_counts.end()) sc = src_counts.emplace(si, git_span_counts(stext(si))).first;
      auto dc = dst_counts.find(ai);
      if (dc == dst_counts.end()) dc = dst_counts.emplace(ai, git_span_counts(dtext(ai))).first;
      return git_similarity_score(sc->second, dc->second);
    };
    std::vector<size_t> live;   // the sources still in play (renames: those not used by an exact match)
    auto cull = [&]() {
      live.clear();
      for (size_t si = 0; si < srcs.size(); ++si) if (copies || !srcs[si].used) live.push_back(si);
    };
    cull();
    if (!copies) {   // 2. unique basenames, halfway between the threshold and an exact match
      const int min_basename = minimum + (int)(0.5 * (kMax - minimum));
      std::map<std::string, long> src_base, dst_base;
      for (size_t si : live) {
        auto [it, fresh] = src_base.emplace(base_of(srcs[si].path), (long)si);
        if (!fresh) it->second = -1;
      }
      for (size_t ai : dsts) {
        if (dst_src[ai] >= 0) continue;
        auto [it, fresh] = dst_base.emplace(base_of(pairs[ai].new_path), (long)ai);
        if (!fresh) it->second = -1;
      }
      for (size_t si : live) {
        const std::string b = base_of(srcs[si].path);
        auto d = dst_base.find(b);
        if (d == dst_base.end() || d->second < 0 || src_base[b] < 0) continue;
        const size_t ai = (size_t)d->second;
        if (dst_src[ai] >= 0) continue;
        const int score = estimate(si, ai, min_basename);
        if (score >= min_basename) record(ai, si, score);
      }
      cull();
    }
    size_t nd = 0;
    for (size_t ai : dsts) if (dst_src[ai] < 0) ++nd;
    const size_t ns = live.size();
    long long limit = o.rename_limit >= 0 ? o.rename_limit : c.config.get_int("diff.renamelimit", 1000);
    bool run = nd && ns, skip_unmodified = false;
    if (run && limit > 0 && (unsigned long long)nd * ns > (unsigned long long)(limit * limit)) {   // too_many_rename_candidates
      c.needed_rename_limit = std::max(c.needed_rename_limit, (int)std::max(nd, ns));
      run = false;
      if (o.copies_harder) {
        size_t limited = 0;
        for (size_t si : live) if (!srcs[si].unmodified) ++limited;
        if ((unsigned long long)nd * limited <= (unsigned long long)(limit * limit)) { run = true; skip_unmodified = true; c.degraded_cc_to_c = true; }
      }
    }
    if (run) {   // 3. the matrix
      struct Score { long src = -1, dst = -1; int score = 0, name = 0; };
      auto worse = [](const Score& x, const Score& y) -> bool {   // score_compare(x, y) > 0
        if (x.dst < 0) return y.dst >= 0;
        if (y.dst < 0) return false;
        if (x.score != y.score) return x.score < y.score;
        return x.name < y.name;
      };
      std::vector<Score> mx;
      for (size_t ai : dsts) {
        if (dst_src[ai] >= 0) continue;
        Score m[4];
        for (size_t si : live) {
          if (skip_unmodified && srcs[si].unmodified) continue;
          Score sc;
          sc.score = estimate(si, ai, minimum);
          sc.name = base_of(srcs[si].path) == base_of(pairs[ai].new_path) ? 1 : 0;
          sc.dst = (long)ai;
          sc.src = (long)si;
          int w = 0;   // record_if_better
          for (int k = 1; k < 4; ++k) if (worse(m[k], m[w])) w = k;
          if (worse(m[w], sc)) m[w] = sc;
        }
        for (auto& sc : m) mx.push_back(sc);
      }
      std::stable_sort(mx.begin(), mx.end(), [&](const Score& x, const Score& y) { return worse(y, x); });
      for (int pass = 0; pass < (copies ? 2 : 1); ++pass) {
        for (auto& sc : mx) {
          if (sc.dst < 0 || sc.score < minimum) break;
          if (dst_src[(size_t)sc.dst] >= 0) continue;
          if (pass == 0 && srcs[(size_t)sc.src].used) continue;
          record((size_t)sc.dst, (size_t)sc.src, sc.score);
        }
      }
    }
  }

  // The destinations become renames or copies where they stand; a deleted source that was used goes.
  bool any = false;
  for (size_t ai : dsts) if (dst_src[ai] >= 0) any = true;
  if (!any) return;
  std::vector<int> left(srcs.size());
  for (size_t si = 0; si < srcs.size(); ++si) left[si] = srcs[si].used;
  std::vector<DiffPair> out;
  for (size_t i = 0; i < pairs.size(); ++i) {
    const DiffPair& p = pairs[i];
    if (p.status == 'A' && dst_src[i] >= 0) {
      const Src& sr = srcs[(size_t)dst_src[i]];
      DiffPair r;
      r.old_path = sr.path;
      r.new_path = p.new_path;
      r.a = sr.st;
      r.b = p.b;
      r.b_text = p.b_text;
      if (sr.pair >= 0) r.a_text = pairs[(size_t)sr.pair].a_text;
      r.score = dst_score[i] * 100 / kMax;   // similarity_index
      r.status = --left[(size_t)dst_src[i]] > 0 ? 'C' : 'R';
      out.push_back(std::move(r));
      continue;
    }
    if (p.status == 'D') {
      bool used = false;
      for (auto& sr : srcs) if (sr.deleted && sr.pair == (long)i && sr.used) used = true;
      if (used) continue;
    }
    out.push_back(p);
  }
  pairs = std::move(out);
}

bool keep_by_filter(const DiffPair& p, const std::string& filter) {
  if (filter.empty()) return true;
  bool has_upper = false;
  for (char ch : filter) if (std::isupper((unsigned char)ch)) has_upper = true;
  char st = p.status;
  for (char ch : filter) if (std::islower((unsigned char)ch) && std::toupper((unsigned char)ch) == st) return false;
  if (!has_upper) return true;
  return filter.find(st) != std::string::npos;
}

// --relative: only paths below this directory (from the top of the work tree, ending in '/') are shown, without it
std::string relative_prefix(Ctx& c, const DiffOptions& o) {
  if (!o.relative) return "";
  if (o.relative_to.empty()) return c.prefix;
  std::string p = normalize_path(slashes(o.relative_to));
  while (p.rfind("./", 0) == 0) p = p.substr(2);
  if (p == "." || p.empty()) return "";
  return p.back() == '/' ? p : p + "/";
}

std::string without_prefix(const std::string& path, const std::string& prefix) {
  return !prefix.empty() && path.rfind(prefix, 0) == 0 ? path.substr(prefix.size()) : path;
}

// -S<string>, -G<regex>, --pickaxe-regex and --pickaxe-all
std::vector<DiffPair> pickaxe_filter(Ctx& c, const std::vector<DiffPair>& pairs, const DiffOptions& o) {
  std::regex re;
  const bool want_regex = !o.pickaxe_g.empty() || o.pickaxe_regex;
  if (want_regex) {
    try {
      re = std::regex(o.pickaxe_g.empty() ? o.pickaxe_s : o.pickaxe_g, std::regex::extended);
    } catch (const std::regex_error&) {
      die(c, "invalid regular expression: " + (o.pickaxe_g.empty() ? o.pickaxe_s : o.pickaxe_g));
    }
  }
  auto occurrences = [&](const std::string& text) -> long {
    long n = 0;
    if (!want_regex) {
      for (size_t at = text.find(o.pickaxe_s); at != std::string::npos; at = text.find(o.pickaxe_s, at + std::max<size_t>(1, o.pickaxe_s.size()))) ++n;
      return n;
    }
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) ++n;
    return n;
  };
  std::vector<DiffPair> kept;
  for (auto& p : pairs) {
    std::string ta = pair_text(c, p, false), tb = pair_text(c, p, true);
    bool hit;
    if (!o.pickaxe_g.empty()) {
      if (!o.text && (is_binary(ta) || is_binary(tb))) hit = false;
      else {
        hit = false;
        GitDiffTextOptions to;
        to.context = 0;
        for (auto& h : git_diff_hunks(ta, tb, to)) {
          for (auto& l : h.lines) {
            if (l.tag == ' ') continue;
            std::string line(l.text);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            if (std::regex_search(line, re)) { hit = true; break; }
          }
          if (hit) break;
        }
      }
    } else {
      hit = occurrences(ta) != occurrences(tb);
    }
    if (hit) kept.push_back(p);
  }
  if (o.pickaxe_all && !kept.empty()) return pairs;
  return kept;
}

std::string pprint_rename(const std::string& a, const std::string& b) {
  std::string qa = quote_path(a), qb = quote_path(b);
  if (qa != a || qb != b) return qa + " => " + qb;
  long len_a = (long)a.size(), len_b = (long)b.size();
  long pfx = 0;
  {
    long i = 0;
    while (i < len_a && i < len_b && a[(size_t)i] == b[(size_t)i]) {
      if (a[(size_t)i] == '/') pfx = i + 1;
      ++i;
    }
  }
  long sfx = 0;
  {
    long adj = pfx ? 1 : 0;
    long ia = len_a, ib = len_b;   // positions one past; compare a[ia] (NUL at end matches NUL)
    while (pfx - adj <= ia && pfx - adj <= ib) {
      char ca = ia < len_a ? a[(size_t)ia] : '\0';
      char cb = ib < len_b ? b[(size_t)ib] : '\0';
      if (ca != cb) break;
      if (ca == '/') sfx = len_a - ia;
      --ia;
      --ib;
      if (ia < 0 || ib < 0) break;
    }
  }
  long amid = std::max(0L, len_a - pfx - sfx), bmid = std::max(0L, len_b - pfx - sfx);
  std::string out;
  if (pfx + sfx) { out += a.substr(0, (size_t)pfx); out += '{'; }
  out += a.substr((size_t)pfx, (size_t)amid);
  out += " => ";
  out += b.substr((size_t)pfx, (size_t)bmid);
  if (pfx + sfx) { out += '}'; out += a.substr((size_t)(len_a - sfx)); }
  return out;
}

const char kEn85[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

ByteBuffer deflate_for_patch(const std::string& data) {
  return zlib_deflate(ByteSpan((const uint8_t*)data.data(), data.size()));
}

void binary_body(std::ostream& out, const std::string& data) {
  ByteBuffer z = deflate_for_patch(data);
  out << "literal " << data.size() << "\n";
  size_t pos = 0;
  while (pos < z.size()) {
    size_t bytes = std::min<size_t>(52, z.size() - pos);
    std::string line;
    line += bytes <= 26 ? (char)('A' + bytes - 1) : (char)('a' + bytes - 27);
    size_t left = bytes;
    const uint8_t* d = z.data() + pos;
    while (left) {
      uint32_t acc = 0;
      for (int cnt = 24; cnt >= 0; cnt -= 8) {
        acc |= (uint32_t)*d++ << cnt;
        if (--left == 0) break;
      }
      char buf[5];
      for (int k = 4; k >= 0; --k) { buf[k] = kEn85[acc % 85]; acc /= 85; }
      line.append(buf, 5);
    }
    out << line << "\n";
    pos += bytes;
  }
  out << "\n";
}

int decimal_width(unsigned long long n) {
  int w = 1;
  while (n >= 10) { n /= 10; ++w; }
  return w;
}

}  // namespace

std::vector<DiffPair> diff_snapshots(Ctx& c, const Snapshot& a, const Snapshot& b, const Pathspec& ps, const DiffOptions& o,
                                     const std::map<std::string, std::string>* b_texts) {
  std::vector<DiffPair> pairs;
  auto ia = a.begin(), ib = b.begin();
  auto emit = [&](const std::string& path, const FileState* x, const FileState* y) {
    if (!ps.match(path)) return;
    FileState fa = x ? *x : FileState{};
    FileState fb = y ? *y : FileState{};
    if (fa.present && fb.present && fa.oid == fb.oid && fa.mode == fb.mode) return;
    DiffPair p;
    p.old_path = p.new_path = path;
    p.a = fa;
    p.b = fb;
    p.status = status_of(fa, fb);
    if (b_texts && fb.present) {
      auto t = b_texts->find(path);
      if (t != b_texts->end()) p.b_text = t->second;
    }
    pairs.push_back(p);
  };
  while (ia != a.end() || ib != b.end()) {
    if (ib == b.end() || (ia != a.end() && ia->first < ib->first)) { emit(ia->first, &ia->second, nullptr); ++ia; }
    else if (ia == a.end() || ib->first < ia->first) { emit(ib->first, nullptr, &ib->second); ++ib; }
    else { emit(ia->first, &ia->second, &ib->second); ++ia; ++ib; }
  }
  if (o.renames || o.copies || o.copies_harder) find_renames(c, pairs, o, a);
  std::vector<DiffPair> kept;
  const std::string rel = relative_prefix(c, o);
  for (auto& p : pairs) {
    if (!keep_by_filter(p, o.filter)) continue;
    if (!rel.empty() && p.new_path.rfind(rel, 0) != 0 && p.old_path.rfind(rel, 0) != 0) continue;
    kept.push_back(p);
  }
  if (!o.pickaxe_s.empty() || !o.pickaxe_g.empty() || o.has_pickaxe_s) kept = pickaxe_filter(c, kept, o);
  std::stable_sort(kept.begin(), kept.end(), [](const DiffPair& x, const DiffPair& y) {
    const std::string& px = x.status == 'D' ? x.old_path : x.new_path;
    const std::string& py = y.status == 'D' ? y.old_path : y.new_path;
    if (px != py) return px < py;
    return x.status == 'D' && y.status != 'D';
  });
  return kept;
}

bool diff_stat_and_patch(const DiffOptions& o) {
  bool stat = o.format == DiffOptions::Stat, patch = o.format == DiffOptions::Patch;
  for (auto f : o.extra) {
    if (f == DiffOptions::Stat) stat = true;
    if (f == DiffOptions::Patch) patch = true;
  }
  return stat && patch;
}

GitDiffTextOptions diff_text_options(Ctx& c, const DiffOptions& o) {
  GitDiffTextOptions t;
  t.context = o.context;
  t.inter_hunk_context = o.inter_hunk;
  t.function_context = o.function_context;
  t.indent_heuristic = o.indent_heuristic;
  t.ignore_space_at_eol = o.ignore_space_at_eol;
  t.ignore_space_change = o.ignore_space_change;
  t.ignore_all_space = o.ignore_all_space;
  t.ignore_cr_at_eol = o.ignore_cr_at_eol;
  t.ignore_blank_lines = o.ignore_blank_lines;
  std::string alg = o.algorithm.empty() ? c.config.get("diff.algorithm").value_or("") : o.algorithm;
  if (alg == "minimal") t.algorithm = GitDiffAlgorithm::Minimal;
  else if (alg == "patience") t.algorithm = GitDiffAlgorithm::Patience;
  else if (alg == "histogram") t.algorithm = GitDiffAlgorithm::Histogram;
  if (!o.ignore_matching.empty()) {
    auto res = std::make_shared<std::vector<std::regex>>();
    for (auto& r : o.ignore_matching) {
      try {
        res->push_back(std::regex(r, std::regex::extended));
      } catch (const std::regex_error&) {
        die(c, "invalid regular expression: " + r);
      }
    }
    t.ignore_line = [res](std::string_view line) {
      std::string s(line);
      if (!s.empty() && s.back() == '\n') s.pop_back();
      for (auto& re : *res) if (std::regex_search(s, re)) return true;
      return false;
    };
  }
  return t;
}

std::vector<std::string> take_diff_options(Ctx& c, const std::vector<std::string>& args, DiffOptions& o, bool* any_format) {
  std::vector<std::string> rest;
  bool fmt = false;
  bool explicit_patch = false;
  std::vector<DiffOptions::Format> chosen;
  auto choose = [&](DiffOptions::Format f) { chosen.push_back(f); fmt = true; };
  if (auto v = c.config.get("diff.renames")) {
    o.renames = *v != "false" && *v != "no" && *v != "off" && *v != "0";
    if (*v == "copy" || *v == "copies") o.copies = true;
  }
  if (auto v = c.config.get("diff.context")) o.context = std::max(0, std::atoi(v->c_str()));
  if (auto v = c.config.get("diff.interhunkcontext")) o.inter_hunk = std::max(0, std::atoi(v->c_str()));
  if (c.config.get_bool("diff.noprefix", false)) o.src_prefix = o.dst_prefix = "";
  o.indent_heuristic = c.config.get_bool("diff.indentheuristic", true);
  // --name=value, or --name value for the options that take one
  auto value_of = [&](size_t& i, const std::string& name, std::string& v) {
    const std::string& a = args[i];
    if (a.rfind(name + "=", 0) == 0) { v = a.substr(name.size() + 1); return true; }
    if (a == name && i + 1 < args.size()) { v = args[++i]; return true; }
    return false;
  };
  // -X<value>, or -X <value>
  auto short_value = [&](size_t& i, char letter, std::string& v) {
    const std::string& a = args[i];
    if (a.size() > 2 && a[0] == '-' && a[1] == letter) { v = a.substr(2); return true; }
    if (a.size() == 2 && a[0] == '-' && a[1] == letter && i + 1 < args.size()) { v = args[++i]; return true; }
    return false;
  };
  auto number = [&](const std::string& what, const std::string& v) {
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (!end || *end || v.empty()) usage(c, "option `" + what + "' expects a numerical value");
    return (int)n;
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    std::string v;
    if (a == "--") { for (; i < args.size(); ++i) rest.push_back(args[i]); break; }
    if (a == "--name-only") choose(DiffOptions::NameOnly);
    else if (a == "--name-status") choose(DiffOptions::NameStatus);
    else if (a == "--numstat") choose(DiffOptions::NumStat);
    else if (a == "--stat" || a.rfind("--stat=", 0) == 0) {
      choose(DiffOptions::Stat);
      if (a.size() > 7) {
        // --stat=<width>[,<name-width>[,<count>]]
        std::string spec = a.substr(7);
        size_t c1 = spec.find(','), c2 = c1 == std::string::npos ? c1 : spec.find(',', c1 + 1);
        o.stat_width = number("stat", spec.substr(0, c1));
        if (c1 != std::string::npos) o.stat_name_width = number("stat", spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
        if (c2 != std::string::npos) o.stat_count = number("stat", spec.substr(c2 + 1));
      }
    } else if (value_of(i, "--stat-width", v)) { o.stat_width = number("stat-width", v); choose(DiffOptions::Stat); }
    else if (value_of(i, "--stat-name-width", v)) { o.stat_name_width = number("stat-name-width", v); choose(DiffOptions::Stat); }
    else if (value_of(i, "--stat-graph-width", v)) { o.stat_graph_width = number("stat-graph-width", v); choose(DiffOptions::Stat); }
    else if (value_of(i, "--stat-count", v)) { o.stat_count = number("stat-count", v); choose(DiffOptions::Stat); }
    else if (a == "--shortstat") choose(DiffOptions::ShortStat);
    else if (a == "--raw") choose(DiffOptions::Raw);
    else if (a == "--summary") choose(DiffOptions::Summary);
    else if (a == "--compact-summary") { choose(DiffOptions::Stat); o.compact_summary = true; }
    else if (a == "--check") choose(DiffOptions::Check);
    else if (a == "--scopes") choose(DiffOptions::Scopes);
    else if (a == "-p" || a == "-u" || a == "--patch") { choose(DiffOptions::Patch); explicit_patch = true; }
    else if (a == "--patch-with-stat") { choose(DiffOptions::Stat); choose(DiffOptions::Patch); explicit_patch = true; }
    else if (a == "--patch-with-raw") { choose(DiffOptions::Raw); choose(DiffOptions::Patch); explicit_patch = true; }
    else if (a == "-s" || a == "--no-patch") { chosen.clear(); fmt = true; choose(DiffOptions::Quiet); }
    else if (a == "--dirstat" || a.rfind("--dirstat=", 0) == 0 || a == "--cumulative" || a == "-X" || a.rfind("--dirstat-by-file", 0) == 0) {
      choose(DiffOptions::DirStat);
      if (a == "--cumulative") o.dirstat_cumulative = true;
      else if (a.rfind("--dirstat-by-file", 0) == 0) o.dirstat = "files";
      else if (a.size() > 10) o.dirstat = a.substr(10);
    } else if (a == "-U" && i + 1 < args.size()) { o.context = number("unified", args[++i]); choose(DiffOptions::Patch); explicit_patch = true; }
    // -U<n> implies --patch (git's enable_patch_output), next to whatever else was asked for
    else if (a.rfind("-U", 0) == 0 && a.size() > 2) { o.context = number("unified", a.substr(2)); choose(DiffOptions::Patch); explicit_patch = true; }
    else if (a.rfind("--unified=", 0) == 0) { o.context = number("unified", a.substr(10)); choose(DiffOptions::Patch); explicit_patch = true; }
    else if (value_of(i, "--inter-hunk-context", v)) o.inter_hunk = number("inter-hunk-context", v);
    else if (a == "-W" || a == "--function-context") o.function_context = true;
    else if (a == "--no-function-context") o.function_context = false;
    else if (a == "--binary") { o.binary = true; o.text = false; choose(DiffOptions::Patch); explicit_patch = true; }
    else if (a == "-z") o.z = true;
    else if (a.rfind("--diff-filter=", 0) == 0) o.filter = a.substr(14);
    else if (a == "-M" || a == "--find-renames") o.renames = true;
    else if (a.rfind("-M", 0) == 0 || a.rfind("--find-renames=", 0) == 0) {
      o.renames = true;
      std::string s = a[1] == 'M' ? a.substr(2) : a.substr(15);
      o.rename_score = parse_rename_score(s);
      o.rename_threshold = o.rename_score * 100 / 60000;
    } else if (a == "-C" || a == "--find-copies") {
      if (o.copies) o.copies_harder = true;
      o.copies = true;
      o.renames = true;
    } else if (a == "--find-copies-harder") { o.copies = o.copies_harder = true; o.renames = true; }
    else if (a.rfind("-C", 0) == 0 || a.rfind("--find-copies=", 0) == 0) {
      o.renames = true;
      if (o.copies) o.copies_harder = true;
      o.copies = true;
      std::string s = a[1] == 'C' ? a.substr(2) : a.substr(14);
      o.rename_score = parse_rename_score(s);
      o.rename_threshold = o.rename_score * 100 / 60000;
    } else if (a == "--no-renames") { o.renames = false; o.copies = o.copies_harder = false; }
    else if (short_value(i, 'l', v)) o.rename_limit = number("l", v);
    else if (a == "--full-index") o.full_index = true;
    else if (a == "-a" || a == "--text") { o.text = true; o.binary = false; }
    else if (a == "--abbrev") o.abbrev = -1;
    else if (a.rfind("--abbrev=", 0) == 0) o.abbrev = std::min(40, std::max(4, number("abbrev", a.substr(9))));
    else if (a == "--no-abbrev") o.abbrev = 40;
    else if (a == "--no-color-moved") o.color_moved = 0;
    else if (a == "--no-color" || a == "--color=never" || a == "--no-color-moved-ws" || a == "--no-ext-diff" ||
             a == "--ext-diff" || a == "--no-textconv" || a == "--textconv" || a == "--myers" || a == "--indent-heuristic" ||
             a == "--ita-invisible-in-index" || a.rfind("--ignore-submodules", 0) == 0 || a == "--no-relative" || a == "--default-prefix") {
      // colour is never written to a pipe here, and myers with the indent heuristic is what git diffs with
      if (a == "--default-prefix") o.src_prefix = "a/", o.dst_prefix = "b/";
      if (a == "--no-relative") o.relative = false;
    } else if (a == "--root") {
      // only diff-tree and show have a root commit to compare with
    } else if (a == "--submodule") o.submodule = "log";
    else if (a.rfind("--submodule=", 0) == 0) o.submodule = a.substr(12);
    else if (a == "--color=auto") {
      if (c.terminal) unsupported(c, "the diff option " + a);
    } else if (a == "--no-indent-heuristic") o.indent_heuristic = false;
    else if (a == "--ita-visible-in-index") o.ita_visible = true;
    else if (a == "--relative") o.relative = true;
    else if (a.rfind("--relative=", 0) == 0) { o.relative = true; o.relative_to = a.substr(11); }
    else if (a == "--no-prefix") { o.src_prefix_none = true; o.src_prefix = o.dst_prefix = ""; }
    else if (value_of(i, "--src-prefix", v)) o.src_prefix = v;
    else if (value_of(i, "--dst-prefix", v)) o.dst_prefix = v;
    else if (value_of(i, "--line-prefix", v)) o.line_prefix = v;
    else if (a == "--minimal") o.algorithm = "minimal";
    else if (a == "--patience") o.algorithm = "patience";
    else if (a == "--histogram") o.algorithm = "histogram";
    else if (value_of(i, "--diff-algorithm", v)) {
      if (v != "myers" && v != "default" && v != "minimal" && v != "patience" && v != "histogram") die(c, "option diff-algorithm accepts \"myers\", \"minimal\", \"patience\" and \"histogram\"");
      o.algorithm = v == "default" ? "myers" : v;
    } else if (a == "-w" || a == "--ignore-all-space") o.ignore_all_space = true;
    else if (a == "-b" || a == "--ignore-space-change") o.ignore_space_change = true;
    else if (a == "--ignore-space-at-eol") o.ignore_space_at_eol = true;
    else if (a == "--ignore-cr-at-eol") o.ignore_cr_at_eol = true;
    else if (a == "--ignore-blank-lines") o.ignore_blank_lines = true;
    else if (short_value(i, 'I', v) || value_of(i, "--ignore-matching-lines", v)) o.ignore_matching.push_back(v);
    else if (short_value(i, 'S', v)) { o.pickaxe_s = v; o.has_pickaxe_s = true; }
    else if (short_value(i, 'G', v)) o.pickaxe_g = v;
    else if (a == "--pickaxe-regex") o.pickaxe_regex = true;
    else if (a == "--pickaxe-all") o.pickaxe_all = true;
    else if (a == "-R") o.reverse = true;
    else if (a == "-D" || a == "--irreversible-delete") o.irreversible_delete = true;
    else if (a == "--word-diff" || a.rfind("--word-diff=", 0) == 0) {
      std::string mode = a == "--word-diff" ? "plain" : a.substr(12);
      if (mode == "plain") o.words = DiffOptions::WordsPlain;
      else if (mode == "porcelain") o.words = DiffOptions::WordsPorcelain;
      else if (mode == "color") { o.words = DiffOptions::WordsColor; c.color = true; }
      else if (mode == "none") o.words = DiffOptions::WordsNone;
      else die(c, "bad --word-diff argument: " + mode);
    } else if (value_of(i, "--word-diff-regex", v)) {
      o.word_regex = v;
      if (o.words == DiffOptions::WordsNone) o.words = DiffOptions::WordsPlain;
    } else if (a == "--color-words" || a.rfind("--color-words=", 0) == 0) {
      // --color-words[=<regex>]: --word-diff=color, colors on whatever the output
      o.words = DiffOptions::WordsColor;
      c.color = true;
      if (a.size() > 14) o.word_regex = a.substr(14);
    } else if (a == "--color-moved" || a.rfind("--color-moved=", 0) == 0) {
      if (a == "--color-moved") {
        int d = 0;
        if (auto cfg = c.config.get("diff.colormoved")) d = std::max(0, parse_color_moved(*cfg));
        o.color_moved = d ? d : 3;
      } else {
        o.color_moved = parse_color_moved(a.substr(14));
        if (o.color_moved < 0) die(c, "bad --color-moved argument: " + a.substr(14));
      }
    } else if (a.rfind("--color-moved-ws=", 0) == 0) {
      if (a.substr(17) != "no") unsupported(c, "the diff option " + a);
    } else if (a == "--color" || a.rfind("--color=", 0) == 0 || a == "-B" || a.rfind("-B", 0) == 0 ||
               a == "--break-rewrites" || a.rfind("--break-rewrites=", 0) == 0 || a.rfind("--output", 0) == 0 || a == "--anchored" ||
               a.rfind("--anchored=", 0) == 0 || a.rfind("--find-object", 0) == 0 || a.rfind("--orderfile", 0) == 0 ||
               a.rfind("-O", 0) == 0 || a.rfind("--skip-to", 0) == 0 || a.rfind("--rotate-to", 0) == 0) {
      unsupported(c, "the diff option " + a);
    } else rest.push_back(a);
  }
  if (!chosen.empty()) {
    // --stat with --patch prints the stat first; --name-only etc. replace the patch
    o.format = chosen[0];
    o.extra.assign(chosen.begin() + 1, chosen.end());
    if (explicit_patch && o.format != DiffOptions::Patch) {
      bool has = false;
      for (auto f : o.extra) if (f == DiffOptions::Patch) has = true;
      if (!has) o.extra.push_back(DiffOptions::Patch);
    }
  }
  if (o.words != DiffOptions::WordsNone) {
    bool has_patch = o.format == DiffOptions::Patch;
    for (auto f : o.extra) if (f == DiffOptions::Patch) has_patch = true;
    if (!has_patch && !fmt) { o.format = DiffOptions::Patch; }
  }
  if (any_format) *any_format = fmt;
  return rest;
}

namespace {

struct WordSpan { size_t begin = 0, end = 0; };

// The words of a text, each as its span; entry 0 is an empty word that stands before the first (as in git's
// diff_words_fill). Without a regex a word is a run of non-space bytes; with one, what the regex matches.
std::vector<WordSpan> split_words(const std::string& text, const std::regex* re) {
  std::vector<WordSpan> words(1);
  size_t i = 0;
  while (i < text.size()) {
    size_t begin = i, end = i;
    if (re) {
      std::smatch m;
      auto from = text.begin() + (long)i;
      if (!std::regex_search(from, text.end(), m, *re)) break;
      begin = i + (size_t)m.position(0);
      end = begin + (size_t)m.length(0);
      size_t nl = text.find('\n', begin);
      if (nl != std::string::npos && nl < end) end = nl;
      if (end <= begin) { i = std::max(end, begin) + 1; continue; }
    } else {
      while (begin < text.size() && std::isspace((unsigned char)text[begin])) ++begin;
      if (begin >= text.size()) break;
      end = begin;
      while (end < text.size() && !std::isspace((unsigned char)text[end])) ++end;
    }
    words.push_back({begin, end});
    i = end;
  }
  return words;
}

struct WordStyle { const char *ctx_pre, *ctx_suf, *old_pre, *old_suf, *new_pre, *new_suf, *newline; };

// fn_out_diff_words_write_helper: each run up to a newline in the style's color (and a reset), then the newline
void write_words(std::ostream& out, const char* pre, const char* suf, const char* newline, const std::string& text, size_t from, size_t to,
                 const std::string& color = std::string()) {
  size_t at = from;
  while (at < to) {
    size_t nl = text.find('\n', at);
    bool has_nl = nl != std::string::npos && nl < to;
    size_t seg_end = has_nl ? nl : to;
    if (seg_end > at) {
      out << color << pre << text.substr(at, seg_end - at) << suf;
      if (!color.empty()) out << kColorReset;
    }
    if (has_nl) out << newline;
    if (!has_nl) break;
    at = nl + 1;
  }
}

// --word-diff: the hunks of a text diff, each run of removed and added lines shown as the words that changed
void print_word_hunks(Ctx& c, const DiffOptions& o, const std::string& ta, const std::string& tb, const GitDiffTextOptions& to,
                      std::ostream& out) {
  const bool porcelain = o.words == DiffOptions::WordsPorcelain;
  const WordStyle style = porcelain ? WordStyle{" ", "\n", "-", "\n", "+", "\n", "~\n"}
                          : o.words == DiffOptions::WordsColor ? WordStyle{"", "", "", "", "", "", "\n"}
                                                               : WordStyle{"", "", "[-", "-]", "{+", "+}", "\n"};
  // with colors: removed words in old's color, added in new's, the rest in context's (diff_words_styles + use_color)
  const DiffPalette* pal = c.color ? &diff_palette(c) : nullptr;
  const std::string old_c = pal ? pal->old_ : "", new_c = pal ? pal->new_ : "", ctx_c = pal ? pal->context : "";
  std::unique_ptr<std::regex> re;
  if (!o.word_regex.empty()) {
    try {
      re = std::make_unique<std::regex>(o.word_regex, std::regex::extended);
    } catch (const std::regex_error&) {
      die(c, "invalid regular expression: " + o.word_regex);
    }
  }
  auto range = [](size_t s, size_t n) {
    std::string r = std::to_string(n ? s + 1 : s);
    if (n != 1) r += "," + std::to_string(n);
    return r;
  };
  std::string minus, plus;
  auto flush = [&]() {
    if (minus.empty() && plus.empty()) return;
    if (plus.empty()) {
      write_words(out, style.old_pre, style.old_suf, style.newline, minus, 0, minus.size(), old_c);
      minus.clear();
      return;
    }
    auto mw = split_words(minus, re.get()), pw = split_words(plus, re.get());
    std::string wa, wb;
    for (size_t i = 1; i < mw.size(); ++i) wa += minus.substr(mw[i].begin, mw[i].end - mw[i].begin) + "\n";
    for (size_t i = 1; i < pw.size(); ++i) wb += plus.substr(pw[i].begin, pw[i].end - pw[i].begin) + "\n";
    GitDiffTextOptions wo;
    wo.context = 0;
    wo.indent_heuristic = false;
    size_t current = 0;
    for (auto& h : git_diff_hunks(wa, wb, wo)) {
      size_t mfirst = h.old_start + (h.old_count ? 1 : 0), pfirst = h.new_start + (h.new_count ? 1 : 0);
      size_t mbegin, mend, pbegin, pend;
      if (h.old_count) { mbegin = mw[mfirst].begin; mend = mw[mfirst + h.old_count - 1].end; } else mbegin = mend = mw[mfirst].end;
      if (h.new_count) { pbegin = pw[pfirst].begin; pend = pw[pfirst + h.new_count - 1].end; } else pbegin = pend = pw[pfirst].end;
      if (current != pbegin) write_words(out, style.ctx_pre, style.ctx_suf, style.newline, plus, current, pbegin, ctx_c);
      if (mbegin != mend) write_words(out, style.old_pre, style.old_suf, style.newline, minus, mbegin, mend, old_c);
      if (pbegin != pend) write_words(out, style.new_pre, style.new_suf, style.newline, plus, pbegin, pend, new_c);
      current = pend;
    }
    if (current != plus.size()) write_words(out, style.ctx_pre, style.ctx_suf, style.newline, plus, current, plus.size(), ctx_c);
    minus.clear();
    plus.clear();
  };
  for (auto& h : git_diff_hunks(ta, tb, to)) {
    const std::string head = "@@ -" + range(h.old_start, h.old_count) + " +" + range(h.new_start, h.new_count) + " @@";
    if (pal) {
      out << pal->frag << head << pal->reset;
      if (!h.func.empty()) out << pal->context << " " << pal->reset << pal->func << h.func << pal->reset;
    } else {
      out << head;
      if (!h.func.empty()) out << " " << h.func;
    }
    out << "\n";
    for (auto& l : h.lines) {
      std::string text(l.text);
      const bool incomplete = text.empty() || text.back() != '\n';
      // xdiff ends an incomplete last line with a newline (its "\ No newline" marker, which word diff skips, follows)
      if (l.tag == '-') { minus += text; if (incomplete) minus += '\n'; }
      else if (l.tag == '+') { plus += text; if (incomplete) plus += '\n'; }
      else {
        flush();
        // a context line: emit_line in the context color (its reset written only when the line has text)
        std::string body = (porcelain ? " " : "") + text;
        if (!body.empty() && body.back() == '\n') body.pop_back();
        if (pal && !body.empty()) out << ctx_c << body << pal->reset;
        else out << body;
        out << "\n" << (porcelain ? "~\n" : "");
      }
    }
    flush();
  }
}

// git diff --check: the problems the added lines of each file bring (core.whitespace at its default)
bool whitespace_check(Ctx& c, const DiffOptions& o, const std::vector<DiffPair>& pairs, const std::string& rel,
                      const GitDiffTextOptions& text_opts, std::ostream& out) {
  bool problem = false;
  auto blank = [](std::string_view line) {
    return std::all_of(line.begin(), line.end(), [](char ch) { return std::isspace((unsigned char)ch); });
  };
  for (auto& p : pairs) {
    if (!p.b.present || p.b.mode == 0160000) continue;
    std::string ta = pair_text(c, p, false), tb = pair_text(c, p, true);
    if (is_binary(tb)) continue;
    const std::string name = o.z ? without_prefix(p.new_path, rel) : quote_path(without_prefix(p.new_path, rel));
    GitDiffTextOptions to = text_opts;
    to.context = 0;
    to.function_context = false;
    std::vector<std::string> eof_lines;
    for (auto& h : git_diff_hunks(ta, tb, to)) {
      size_t line_no = h.new_start;
      for (auto& l : h.lines) {
        if (l.tag == '-') continue;
        ++line_no;
        std::string body(l.text);
        if (!body.empty() && body.back() == '\n') body.pop_back();
        std::vector<std::string> errors;
        if (!body.empty() && (body.back() == ' ' || body.back() == '\t' || body.back() == '\r')) errors.push_back("trailing whitespace");
        size_t indent = 0;
        while (indent < body.size() && body[indent] == ' ') ++indent;
        if (indent > 0 && indent < body.size() && body[indent] == '\t') errors.push_back("space before tab in indent");
        if (!errors.empty()) {
          std::string msg;
          for (size_t k = 0; k < errors.size(); ++k) msg += (k ? ", " : "") + errors[k];
          out << name << ":" << line_no << ": " << msg << ".\n+" << body << "\n";
          problem = true;
          continue;
        }
        const bool marker = body.size() >= 7 && (body.compare(0, 7, "<<<<<<<") == 0 || body.compare(0, 7, ">>>>>>>") == 0) && (body.size() == 7 || body[7] == ' ');
        if (marker || body == "=======") {
          out << name << ":" << line_no << ": leftover conflict marker\n";
          problem = true;
        }
      }
    }
    // blank lines the change adds at the end of the file
    auto old_lines = git_split_lines(ta), new_lines = git_split_lines(tb);
    size_t new_blank = 0, old_blank = 0;
    while (new_blank < new_lines.size() && blank(new_lines[new_lines.size() - 1 - new_blank])) ++new_blank;
    while (old_blank < old_lines.size() && blank(old_lines[old_lines.size() - 1 - old_blank])) ++old_blank;
    if (new_blank > old_blank && ta != tb) {
      out << name << ":" << (new_lines.size() - new_blank + old_blank + 1) << ": new blank line at EOF.\n";
      problem = true;
    }
  }
  return problem;
}

// --color-moved: diff.c's move detection over all the +/- lines of one diff output. The patches are written twice:
// once collecting the lines (with a boundary wherever git's symbol stream has something else between them), then
// marked, then again reading each line's marks in the same order.
enum : unsigned { kMoved = 1, kMovedAlt = 2, kMovedDull = 4 };
struct MovedSym { char kind = 0; std::string line; unsigned flags = 0; };
struct MovedRun {
  int mode = 0;
  bool collecting = false;
  std::vector<MovedSym> syms;
  size_t cursor = 0;
  void boundary() { if (collecting) syms.push_back(MovedSym{}); else ++cursor; }
  unsigned line(char kind, std::string_view text) {
    if (collecting) {
      MovedSym m;
      m.kind = kind;
      m.line.assign(text);
      if (m.line.empty() || m.line.back() != '\n') m.line += '\n';   // xdiff ends an incomplete line too
      syms.push_back(std::move(m));
      return 0;
    }
    return cursor < syms.size() ? syms[cursor++].flags : 0;
  }
};
thread_local MovedRun* g_moved = nullptr;

bool alnum_ascii(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

// add_lines_to_move_detection + mark_color_as_moved (+ dim_moved_lines for dimmed-zebra)
void mark_moved(MovedRun& run) {
  enum { Plain = 1, Blocks = 2, Zebra = 3, ZebraDim = 4 };
  auto& sy = run.syms;
  const long N = (long)sy.size();
  std::vector<unsigned> id((size_t)N, 0);
  std::vector<long> next_line((size_t)N, -1), next_match((size_t)N, -1), add_head, del_head;
  std::unordered_map<std::string_view, unsigned> interned;
  long prev = -1;
  for (long n = 0; n < N; ++n) {
    if (!sy[(size_t)n].kind) { prev = -1; continue; }
    auto [it, fresh] = interned.emplace(std::string_view(sy[(size_t)n].line), (unsigned)add_head.size());
    if (fresh) { add_head.push_back(-1); del_head.push_back(-1); }
    id[(size_t)n] = it->second;
    if (prev >= 0 && sy[(size_t)prev].kind == sy[(size_t)n].kind) next_line[(size_t)prev] = n;
    prev = n;
    auto& head = sy[(size_t)n].kind == '+' ? add_head : del_head;
    next_match[(size_t)n] = head[it->second];
    head[it->second] = n;
  }
  auto adjust_last_block = [&](long n, long block_length) -> int {
    if (run.mode == Plain) return (int)block_length;
    int alnum = 0;
    for (long i = 1; i < block_length + 1; ++i)
      for (char ch : sy[(size_t)(n - i)].line)
        if (alnum_ascii(ch) && ++alnum >= 20) return 1;
    for (long i = 1; i < block_length + 1; ++i) sy[(size_t)(n - i)].flags &= ~(unsigned)(kMoved | kMovedAlt);
    return 0;
  };
  std::vector<long> pmb;
  long n = 0, block_length = 0;
  int flipped = 0;
  char moved_symbol = 0;
  for (n = 0; n < N; ++n) {
    MovedSym& l = sy[(size_t)n];
    const long ln = n;
    long match = -1;
    if (l.kind == '+') match = del_head[id[(size_t)n]];
    else if (l.kind == '-') match = add_head[id[(size_t)n]];
    else flipped = 0;
    if (!pmb.empty() && (match < 0 || l.kind != moved_symbol)) {
      if (!adjust_last_block(n, block_length) && block_length > 1) {
        match = -1;   // rewind: another match may start at the block's second line
        n -= block_length;
      }
      pmb.clear();
      block_length = 0;
      flipped = 0;
    }
    if (match < 0) { moved_symbol = 0; continue; }
    if (run.mode == Plain) { l.flags |= kMoved; continue; }
    size_t j = 0;
    for (size_t i = 0; i < pmb.size(); ++i) {
      const long cur = next_line[(size_t)pmb[i]];
      if (cur >= 0 && id[(size_t)cur] == id[(size_t)ln]) pmb[j++] = cur;
    }
    pmb.resize(j);
    if (pmb.empty()) {
      const int contiguous = adjust_last_block(n, block_length);
      if (!contiguous && block_length > 1) n -= block_length;
      else for (long m = match; m >= 0; m = next_match[(size_t)m]) pmb.push_back(m);
      if (contiguous && !pmb.empty() && moved_symbol == l.kind) flipped = (flipped + 1) % 2;
      else flipped = 0;
      moved_symbol = pmb.empty() ? 0 : l.kind;
      block_length = 0;
    }
    if (!pmb.empty()) {
      ++block_length;
      l.flags |= kMoved;
      if (flipped && run.mode != Blocks) l.flags |= kMovedAlt;
    }
  }
  adjust_last_block(n, block_length);
  if (run.mode != ZebraDim) return;
  const unsigned zebra = kMoved | kMovedAlt;
  for (long k = 0; k < N; ++k) {
    MovedSym& l = sy[(size_t)k];
    if (!l.kind || !(l.flags & kMoved)) continue;
    const MovedSym* pv = k > 0 && sy[(size_t)k - 1].kind ? &sy[(size_t)k - 1] : nullptr;
    const MovedSym* nx = k + 1 < N && sy[(size_t)k + 1].kind ? &sy[(size_t)k + 1] : nullptr;
    if (pv && (pv->flags & zebra) == (l.flags & zebra) && nx && (nx->flags & zebra) == (l.flags & zebra)) { l.flags |= kMovedDull; continue; }
    if (pv && (pv->flags & kMoved) && (pv->flags & kMovedAlt) != (l.flags & kMovedAlt)) continue;
    if (nx && (nx->flags & kMoved) && (nx->flags & kMovedAlt) != (l.flags & kMovedAlt)) continue;
    l.flags |= kMovedDull;
  }
}

// The color of a +/- line by its move marks (emit_diff_symbol_from_struct)
const std::string& moved_color(const DiffPalette& pal, char kind, unsigned f) {
  const bool add = kind == '+';
  if (!(f & kMoved)) return add ? pal.new_ : pal.old_;
  if ((f & kMovedAlt) && (f & kMovedDull)) return add ? pal.new_moved_alt_dim : pal.old_moved_alt_dim;
  if (f & kMovedAlt) return add ? pal.new_moved_alt : pal.old_moved_alt;
  if (f & kMovedDull) return add ? pal.new_moved_dim : pal.old_moved_dim;
  return add ? pal.new_moved : pal.old_moved;
}

// git's --color-moved / diff.colorMoved value: 0 no, 1 plain, 2 blocks, 3 zebra (default), 4 dimmed-zebra, -1 bad
int parse_color_moved(std::string v) {
  std::string l = v;
  for (auto& ch : l) ch = (char)std::tolower((unsigned char)ch);
  if (l == "true" || l == "yes" || l == "on" || l == "1") return 3;
  if (l == "false" || l == "no" || l == "off" || l == "0") return 0;
  if (v == "plain") return 1;
  if (v == "blocks") return 2;
  if (v == "zebra" || v == "default") return 3;
  if (v == "dimmed-zebra" || v == "dimmed_zebra") return 4;
  return -1;
}

// The hunks of a text diff in git's colors (diff.c: emit_hunk_header, emit_line_ws_markup, the blank lines a change
// adds at the end of the file painted as whitespace errors).
std::string colored_hunks(Ctx& c, std::string_view ta, std::string_view tb, const GitDiffTextOptions& to) {
  const DiffPalette& pal = diff_palette(c);
  const unsigned rule = whitespace_rule(c), wseh = ws_error_highlight(c);
  const std::string& reset = pal.reset;
  auto lines_of = [](std::string_view t) -> long {
    if (t.empty()) return 0;
    long n = (long)std::count(t.begin(), t.end(), '\n');
    return t.back() == '\n' ? n : n + 1;
  };
  auto blank = [](const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!git_isspace((unsigned char)p[i])) return false;
    return true;
  };
  auto trailing_blank = [&](std::string_view t) -> long {
    if (t.empty()) return 0;
    long cnt = 0;
    long ptr = (long)t.size() - 1;
    if (t[(size_t)ptr] == '\n') --ptr;
    while (0 < ptr) {
      long prev = ptr;
      while (prev >= 0 && t[(size_t)prev] != '\n') --prev;
      if (!blank(t.data() + prev + 1, (size_t)(ptr - prev))) break;
      ++cnt;
      ptr = prev - 1;
    }
    return cnt;
  };
  long eof_pre = 0, eof_post = 0;
  if (const long l1 = trailing_blank(ta), l2 = trailing_blank(tb); l2 > l1) {
    eof_pre = lines_of(ta) - l1 + 1;
    eof_post = lines_of(tb) - l2 + 1;
  }
  auto range = [](size_t s, size_t n) {
    std::string r = std::to_string(n ? s + 1 : s);
    if (n != 1) r += "," + std::to_string(n);
    return r;
  };
  std::string out;
  // emit_line_ws_markup for a line with its sign; `ws_on` when diff.wsErrorHighlight covers its kind
  auto markup = [&](char sign, const std::string& set, std::string_view text, bool ws_on, bool eof_blank) {
    const std::string& ws = pal.whitespace;
    const bool paint_ws = ws_on && !ws.empty();
    const bool nl = !text.empty() && text.back() == '\n';
    std::string_view body = nl ? text.substr(0, text.size() - 1) : text;
    if (!paint_ws) {
      // emit_line_0(set, NULL, sign, line): the set color is written even when it is empty, and so is the reset
      out += set;
      out += sign;
      out.append(body);
      out += reset;
      if (nl) out += '\n';
      return;
    }
    if (eof_blank) {
      out += ws;
      out += sign;
      out.append(body);
      out += reset;
      if (nl) out += '\n';
      return;
    }
    out += set;
    out += sign;
    out += reset;
    ws_check_emit(out, text.data(), text.size(), rule, set, reset, ws);
  };
  if (g_moved) g_moved->boundary();   // the file's header
  for (auto& h : git_diff_hunks(ta, tb, to)) {
    if (g_moved) g_moved->boundary();
    const std::string head = "@@ -" + range(h.old_start, h.old_count) + " +" + range(h.new_start, h.new_count) + " @@";
    out += pal.frag + head + reset;
    if (!h.func.empty()) {
      out += pal.context + " " + reset;
      out += pal.func + h.func + reset;
    }
    out += "\n";
    long pre = (long)(h.old_count ? h.old_start + 1 : h.old_start), post = (long)(h.new_count ? h.new_start + 1 : h.new_start);
    for (auto& l : h.lines) {
      const std::string_view text(l.text);
      if (l.tag == '+') {
        ++post;
        const bool eof_blank = (rule & kWsBlankAtEof) && eof_pre && eof_post && eof_pre <= pre && eof_post <= post &&
                               blank(text.data(), text.size());
        const unsigned mf = g_moved ? g_moved->line('+', text) : 0;
        markup('+', moved_color(pal, '+', mf), text, wseh & kWsehNew, eof_blank);
      } else if (l.tag == '-') {
        ++pre;
        const unsigned mf = g_moved ? g_moved->line('-', text) : 0;
        markup('-', moved_color(pal, '-', mf), text, wseh & kWsehOld, false);
      } else {
        ++pre, ++post;
        if (g_moved) g_moved->boundary();
        markup(' ', pal.context, text, wseh & kWsehContext, false);
      }
      if (text.empty() || text.back() != '\n') {
        if (g_moved) g_moved->boundary();
        out += "\n";
        ++pre;   // the marker counts as a preimage line, as git's fn_out_consume does
        out += pal.context + "\\ No newline at end of file" + reset + "\n";
      }
    }
  }
  return out;
}

// One file's patch: the "diff --git" header with what it says of modes, renames and ids, then the hunks.
void emit_patch(Ctx& c, const DiffOptions& o, const DiffPair& p, const std::string& rel, const GitDiffTextOptions& text_opts,
                int abbrev_len, std::ostream& out) {
  const std::string on = without_prefix(p.old_path, rel), nn = without_prefix(p.new_path, rel);
  // a reversed diff swaps the two prefixes too
  const std::string& src_prefix = o.reverse ? o.dst_prefix : o.src_prefix;
  const std::string& dst_prefix = o.reverse ? o.src_prefix : o.dst_prefix;
  auto named = [&](const std::string& prefix, const std::string& name) { return quote_path(prefix + name); };
  // a name with a space in it is followed by a tab on the ---/+++ lines
  const std::string ta = pair_text(c, p, false), tb = pair_text(c, p, true);
  const bool submodule = p.a.mode == 0160000 || p.b.mode == 0160000;
  const bool bin = !submodule && !o.text && (is_binary(ta) || is_binary(tb));
  std::ostringstream head;
  bool must_show = false;
  head << "diff --git " << named(src_prefix, on) << " " << named(dst_prefix, nn) << "\n";
  if (p.status == 'A') { head << "new file mode " << mode_str(p.b.mode) << "\n"; must_show = true; }
  else if (p.status == 'D') { head << "deleted file mode " << mode_str(p.a.mode) << "\n"; must_show = true; }
  else if (p.a.mode != p.b.mode) { head << "old mode " << mode_str(p.a.mode) << "\nnew mode " << mode_str(p.b.mode) << "\n"; must_show = true; }
  if (p.status == 'R' || p.status == 'C') {
    const char* verb = p.status == 'R' ? "rename" : "copy";
    head << "similarity index " << p.score << "%\n" << verb << " from " << quote_path(on) << "\n" << verb << " to " << quote_path(nn) << "\n";
    must_show = true;
  }
  const bool same_content = p.a.present && p.b.present && p.a.oid == p.b.oid;
  if (!same_content) {
    const int len = o.full_index || (o.binary && bin) ? 40 : abbrev_len;
    auto id = [&](const FileState& f) { return !f.present ? std::string((size_t)std::min(len, 40), '0') : len >= 40 ? f.oid : c.abbrev(f.oid, len); };
    head << "index " << id(p.a) << ".." << id(p.b);
    if (p.a.present && p.b.present && p.a.mode == p.b.mode) head << " " << mode_str(p.a.mode);
    head << "\n";
  }
  // with colors each header line is meta-colored on its own, the "---"/"+++" tab after the reset
  const bool colored = c.color;
  auto header = [&]() {
    if (!colored) return head.str();
    const DiffPalette& pal = diff_palette(c);
    std::string h, all = head.str();
    size_t at = 0;
    while (at < all.size()) {
      size_t eol = all.find('\n', at);
      if (eol == std::string::npos) eol = all.size();
      h += pal.meta + all.substr(at, eol - at) + pal.reset + "\n";
      at = eol + 1;
    }
    return h;
  };
  auto file_line = [&](const char* mark, bool present, const std::string& prefix, const std::string& name) {
    const std::string shown = present ? named(prefix, name) : "/dev/null";
    const char* tab = present && name.find(' ') != std::string::npos ? "\t" : "";
    if (!colored) return std::string(mark) + shown + tab + "\n";
    const DiffPalette& pal = diff_palette(c);
    return pal.meta + mark + shown + pal.reset + tab + "\n";
  };
  if (same_content) { if (must_show) out << header(); return; }
  if (submodule) {
    if (o.submodule == "log" || o.submodule == "diff") unsupported(c, "diff --submodule=" + o.submodule);
    out << header();
    out << file_line("--- ", p.a.present, src_prefix, on) << file_line("+++ ", p.b.present, dst_prefix, nn);
    const std::string hh = std::string("@@ -") + (p.a.present ? "1" : "0,0") + " +" + (p.b.present ? "1" : "0,0") + " @@";
    if (!colored) {
      out << hh << "\n";
      if (p.a.present) out << "-Subproject commit " << p.a.oid << "\n";
      if (p.b.present) out << "+Subproject commit " << p.b.oid << "\n";
      return;
    }
    // the same lines through the colored hunk writer, as git's own submodule diff goes through xdiff
    GitDiffTextOptions so;
    out << colored_hunks(c, p.a.present ? "Subproject commit " + p.a.oid + "\n" : std::string(),
                         p.b.present ? "Subproject commit " + p.b.oid + "\n" : std::string(), so);
    return;
  }
  if (bin) {
    out << header();
    if (o.binary) {
      out << "GIT binary patch\n";
      binary_body(out, tb);
      binary_body(out, ta);
    } else {
      out << "Binary files " << (p.a.present ? named(src_prefix, on) : "/dev/null") << " and " << (p.b.present ? named(dst_prefix, nn) : "/dev/null")
          << " differ\n";
    }
    return;
  }
  if (ta.empty() && tb.empty()) { out << header(); return; }
  if (o.irreversible_delete && p.status == 'D') { out << header(); return; }
  std::string body;
  if (o.words != DiffOptions::WordsNone) {
    std::ostringstream wb;
    print_word_hunks(c, o, ta, tb, text_opts, wb);
    body = wb.str();
  } else if (colored) {
    body = colored_hunks(c, ta, tb, text_opts);
  } else {
    body = git_unified_hunks(ta, tb, text_opts);
  }
  if (body.empty()) { if (must_show) out << header(); return; }
  out << header();
  out << file_line("--- ", p.a.present, src_prefix, on);
  out << file_line("+++ ", p.b.present, dst_prefix, nn);
  out << body;
}

}  // namespace

namespace {

// Columns a string takes up: one per character (git counts UTF-8 code points the same way for its stat layout).
long text_width(const std::string& s) {
  long w = 0;
  for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) ++w;
  return w;
}

DiffPair reversed(const DiffPair& p) {
  DiffPair r = p;
  std::swap(r.a, r.b);
  std::swap(r.a_text, r.b_text);
  if (p.status == 'A') r.status = 'D';
  else if (p.status == 'D') r.status = 'A';
  std::swap(r.old_path, r.new_path);
  return r;
}

}  // namespace

// The diffstat (or just its summary line) of a list of files, as git lays it out for a width.
void print_diffstat(const std::vector<DiffStatEntry>& stats, const DiffOptions& o, bool shortstat, std::ostream& out,
                    const DiffPalette* pal) {
  const std::string add_c = pal ? pal->new_ : "", del_c = pal ? pal->old_ : "", reset_c = pal ? pal->reset : "";
    long total_add = 0, total_del = 0;
    size_t files = 0;
    unsigned long long max_change = 0, max_len = 0;
    int number_width = 0, bin_width = 0;
    size_t count = o.stat_count > 0 ? (size_t)o.stat_count : stats.size();
    size_t shown = 0;
    std::vector<size_t> listed;
    for (size_t i = 0; i < stats.size(); ++i) {
      auto& s = stats[i];
      if (!s.interesting && s.add + s.del == 0 && !s.unmerged) { continue; }
      if (!s.unmerged) ++files;
      if (!shortstat && shown >= count) { total_add += s.binary ? 0 : s.add; total_del += s.binary ? 0 : s.del; continue; }
      ++shown;
      listed.push_back(i);
      std::string label = s.name + (s.comment.empty() ? "" : " (" + s.comment + ")");
      max_len = std::max<unsigned long long>(max_len, (unsigned long long)text_width(label));
      if (s.unmerged) { bin_width = std::max(bin_width, 8); continue; }
      if (s.binary) {
        int w = 14 + decimal_width(s.new_size) + decimal_width(s.old_size);
        bin_width = std::max(bin_width, w);
        number_width = 3;
        continue;
      }
      total_add += s.add;
      total_del += s.del;
      max_change = std::max<unsigned long long>(max_change, (unsigned long long)(s.add + s.del));
    }
    if (!shortstat) {
      int width = o.stat_width > 0 ? o.stat_width : 80 - o.stat_prefix_bytes;
      number_width = std::max(number_width, decimal_width(max_change));
      if (width < 16 + 6 + number_width) width = 16 + 6 + number_width;
      long graph_width = (long)max_change + 4 > bin_width ? (long)max_change : bin_width - 4;
      if (o.stat_graph_width > 0 && o.stat_graph_width < graph_width) graph_width = o.stat_graph_width;
      long name_width = o.stat_name_width > 0 && o.stat_name_width < (long)max_len ? o.stat_name_width : (long)max_len;
      if (name_width + number_width + 6 + graph_width > width) {
        if (graph_width > width * 3 / 8 - number_width - 6) {
          graph_width = width * 3 / 8 - number_width - 6;
          if (graph_width < 6) graph_width = 6;
        }
        if (o.stat_graph_width > 0 && graph_width > o.stat_graph_width) graph_width = o.stat_graph_width;
        if (name_width > width - number_width - 6 - graph_width) name_width = width - number_width - 6 - graph_width;
        else graph_width = width - number_width - 6 - name_width;
      }
      auto scale = [&](long it) -> long {
        if (!it) return 0;
        return 1 + (long)((unsigned long long)it * (unsigned long long)(graph_width - 1) / max_change);
      };
      for (size_t i : listed) {
        auto& s = stats[i];
        std::string name = s.name, pre;
        std::string suffix = s.comment.empty() ? "" : " (" + s.comment + ")";
        long len = name_width;
        if (name_width < text_width(name + suffix)) {
          pre = "...";
          len -= 3;
          if (len < 0) len = 0;
          std::string full = name + suffix;
          // keep the last `len` columns
          size_t cut = full.size();
          long seen = 0;
          while (cut > 0 && seen < len) { --cut; while (cut > 0 && ((unsigned char)full[cut] & 0xC0) == 0x80) --cut; ++seen; }
          name = full.substr(cut);
          size_t slash = name.find('/');
          if (slash != std::string::npos) name = name.substr(slash);
          suffix.clear();
        } else {
          name += suffix;
        }
        long padding = len - text_width(name);
        if (padding < 0) padding = 0;
        out << " " << pre << name << std::string((size_t)padding, ' ') << " | ";
        if (s.unmerged) {
          // git leaves this line unterminated
          out << "Unmerged";
          continue;
        }
        if (s.binary) {
          out << std::string((size_t)std::max(0, number_width - 3), ' ') << "Bin";
          if (s.old_size || s.new_size)
            out << " " << del_c << s.old_size << reset_c << " -> " << add_c << s.new_size << reset_c << " bytes";
          out << "\n";
          continue;
        }
        long add = s.add, del = s.del;
        if (graph_width <= (long)max_change) {
          long total = scale(add + del);
          if (total < 2 && add && del) total = 2;
          if (add < del) { add = scale(add); del = total - add; }
          else { del = scale(del); add = total - del; }
        }
        std::string num = std::to_string(s.add + s.del);
        out << std::string((size_t)std::max(0, number_width - (int)num.size()), ' ') << num << (s.add + s.del ? " " : "");
        if (add > 0) out << add_c << std::string((size_t)add, '+') << reset_c;
        if (del > 0) out << del_c << std::string((size_t)del, '-') << reset_c;
        out << "\n";
      }
      if (shown < files) out << " ...\n";
    }
    if (shortstat) {
      total_add = total_del = 0;
      for (auto& s : stats) if (!s.unmerged && (s.interesting || s.add + s.del)) { if (!s.binary) { total_add += s.add; total_del += s.del; } }
    }
    out << " " << files << (files == 1 ? " file changed" : " files changed");
    if (total_add || total_del == 0) out << ", " << total_add << (total_add == 1 ? " insertion(+)" : " insertions(+)");
    if (total_del || total_add == 0) out << ", " << total_del << (total_del == 1 ? " deletion(-)" : " deletions(-)");
    out << "\n";
}

bool print_diff(Ctx& c, const std::vector<DiffPair>& pairs_in, const DiffOptions& o, std::ostream& out_final) {
  if (pairs_in.empty()) return false;
  std::ostringstream prefixed;
  std::ostream& out = o.line_prefix.empty() ? out_final : prefixed;
  std::vector<DiffPair> pairs = pairs_in;
  if (o.reverse) {
    for (auto& p : pairs) p = reversed(p);
    std::stable_sort(pairs.begin(), pairs.end(), [](const DiffPair& x, const DiffPair& y) {
      const std::string& px = x.status == 'D' ? x.old_path : x.new_path;
      const std::string& py = y.status == 'D' ? y.old_path : y.new_path;
      if (px != py) return px < py;
      return x.status == 'D' && y.status != 'D';
    });
  }
  const std::string rel = relative_prefix(c, o);
  auto shown_name = [&](const std::string& path) { return without_prefix(path, rel); };
  std::vector<DiffOptions::Format> formats{o.format};
  for (auto f : o.extra) if (std::find(formats.begin(), formats.end(), f) == formats.end()) formats.push_back(f);
  // git's own order: the per-file lines, the stats, the summary, then the patch
  std::stable_sort(formats.begin(), formats.end(), [](DiffOptions::Format x, DiffOptions::Format y) {
    auto rank = [](DiffOptions::Format f) {
      switch (f) {
        case DiffOptions::Raw: case DiffOptions::NameOnly: case DiffOptions::NameStatus: case DiffOptions::Check: return 0;
        case DiffOptions::NumStat: return 1;
        case DiffOptions::Stat: return 2;
        case DiffOptions::ShortStat: return 3;
        case DiffOptions::DirStat: return 4;
        case DiffOptions::Summary: return 5;
        case DiffOptions::Patch: return 6;
        default: return 0;
      }
    };
    return rank(x) < rank(y);
  });
  const char term = o.z ? '\0' : '\n';
  auto show_path = [&](const std::string& p) { return o.z ? shown_name(p) : quote_path(shown_name(p)); };
  const int abbrev_len = o.abbrev < 0 ? c.default_abbrev() : o.abbrev;
  auto abbreviated = [&](const std::string& oid, int len) { return len >= 40 ? oid : c.abbrev(oid, len); };
  auto oid_text = [&](const FileState& f, int len) {
    if (!f.present || f.oid.find_first_not_of('0') == std::string::npos) return std::string((size_t)std::min(len, 40), '0');
    return abbreviated(f.oid, len);
  };
  const GitDiffTextOptions text_opts = diff_text_options(c, o);
  using Stat = DiffStatEntry;
  std::vector<Stat> stats;
  bool stats_done = false;
  auto compute_stats = [&]() {
    if (stats_done) return;
    stats_done = true;
    for (auto& p : pairs) {
      Stat s;
      bool is_rename = p.status == 'R' || p.status == 'C' || (!p.old_path.empty() && !p.new_path.empty() && p.old_path != p.new_path);
      s.name = is_rename ? pprint_rename(shown_name(p.old_path), shown_name(p.new_path))
                         : quote_path(shown_name(p.status == 'D' ? p.old_path : p.new_path));
      s.interesting = p.status != 'M' || p.a.mode != p.b.mode;
      if (p.status == 'U') {
        s.name = quote_path(shown_name(p.new_path));
        s.unmerged = true;
        stats.push_back(s);
        continue;
      }
      if (o.compact_summary) {
        auto link = [](uint32_t m) { return m == 0120000; };
        auto exec = [](uint32_t m) { return (m & 0111) != 0; };
        if (p.status == 'A') s.comment = link(p.b.mode) ? "new +l" : exec(p.b.mode) ? "new +x" : "new";
        else if (p.status == 'D') s.comment = link(p.a.mode) ? "gone -l" : exec(p.a.mode) ? "gone -x" : "gone";
        else if (p.a.mode != p.b.mode) {
          if (link(p.a.mode) != link(p.b.mode)) s.comment = link(p.b.mode) ? "mode +l" : "mode -l";
          else if (exec(p.a.mode) != exec(p.b.mode)) s.comment = exec(p.b.mode) ? "mode +x" : "mode -x";
        }
      }
      if (p.a.mode == 0160000 || p.b.mode == 0160000) {
        s.add = p.b.present ? 1 : 0;
        s.del = p.a.present ? 1 : 0;
      } else {
        std::string ta = pair_text(c, p, false), tb = pair_text(c, p, true);
        if (!o.text && (is_binary(ta) || is_binary(tb))) {
          s.binary = true;
          s.interesting = true;
          s.old_size = ta.size();
          s.new_size = tb.size();
        } else {
          auto [ad, de] = count_changes(ta, tb, text_opts);
          s.add = ad;
          s.del = de;
        }
      }
      stats.push_back(s);
    }
  };
  bool wrote_before = false;
  for (auto f : formats) {
    switch (f) {
      case DiffOptions::Quiet:
        break;
      case DiffOptions::NameOnly:
        for (auto& p : pairs) out << show_path(p.status == 'D' ? p.old_path : p.new_path) << term;
        wrote_before = true;
        break;
      case DiffOptions::NameStatus:
        for (auto& p : pairs) {
          if (p.status == 'R' || p.status == 'C') {
            char sc[8];
            std::snprintf(sc, sizeof sc, "%c%03d", p.status, p.score);
            out << sc << (o.z ? '\0' : '\t') << show_path(p.old_path) << (o.z ? '\0' : '\t') << show_path(p.new_path) << term;
          } else {
            out << p.status << (o.z ? '\0' : '\t') << show_path(p.status == 'D' ? p.old_path : p.new_path) << term;
          }
        }
        wrote_before = true;
        break;
      case DiffOptions::Raw:
        for (auto& p : pairs) {
          // a work tree file is not hashed unless it is new: its id shows as zeros
          const bool unhashed_old = p.a_text && !p.b_text && p.status != 'D';
          const bool unhashed_new = p.b_text && !p.a_text && p.status != 'A';
          const std::string zeros((size_t)std::min(abbrev_len, 40), '0');
          out << ":" << mode_str(p.a.present ? p.a.mode : 0) << " " << mode_str(p.b.present ? p.b.mode : 0) << " "
              << (unhashed_old ? zeros : oid_text(p.a, abbrev_len)) << " " << (unhashed_new ? zeros : oid_text(p.b, abbrev_len)) << " ";
          if (p.status == 'R' || p.status == 'C') {
            char sc[8];
            std::snprintf(sc, sizeof sc, "%c%03d", p.status, p.score);
            out << sc << (o.z ? '\0' : '\t') << show_path(p.old_path) << (o.z ? '\0' : '\t') << show_path(p.new_path) << term;
          } else {
            out << p.status << (o.z ? '\0' : '\t') << show_path(p.status == 'D' ? p.old_path : p.new_path) << term;
          }
        }
        wrote_before = true;
        break;
      case DiffOptions::Check:
        if (whitespace_check(c, o, pairs, rel, text_opts, out)) c.diff_check_failed = true;
        wrote_before = true;
        break;
      case DiffOptions::NumStat: {
        compute_stats();
        for (size_t i = 0; i < pairs.size(); ++i) {
          auto& p = pairs[i];
          auto& s = stats[i];
          if (s.binary) out << "-\t-\t";
          else out << s.add << "\t" << s.del << "\t";
          if (s.unmerged) { out << (o.z ? shown_name(p.new_path) + std::string(1, '\0') : s.name + "\n"); continue; }
          bool is_rename = p.status == 'R' || p.status == 'C' || (!p.old_path.empty() && !p.new_path.empty() && p.old_path != p.new_path);
          if (o.z) {
            if (is_rename) out << '\0' << shown_name(p.old_path) << '\0' << shown_name(p.new_path) << '\0';
            else out << shown_name(p.status == 'D' ? p.old_path : p.new_path) << '\0';
          } else {
            out << s.name << "\n";
          }
        }
        wrote_before = true;
        break;
      }
      case DiffOptions::Stat:
      case DiffOptions::ShortStat:
        compute_stats();
        print_diffstat(stats, o, f == DiffOptions::ShortStat, out, c.color ? &diff_palette(c) : nullptr);
        wrote_before = true;
        break;
      case DiffOptions::DirStat: {
        // parse_dirstat_params: diff.dirstat, then the option's own; changes (bytes), lines (the stat's) or files
        std::string mode = "changes";
        int permille = 30;
        bool cumulative = o.dirstat_cumulative;
        auto take = [&](const std::string& params) {
          size_t i = 0;
          while (i <= params.size()) {
            size_t comma = params.find(',', i);
            std::string p = params.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
            i = comma == std::string::npos ? params.size() + 1 : comma + 1;
            if (p.empty()) continue;
            if (p == "changes" || p == "lines" || p == "files") mode = p;
            else if (p == "cumulative") cumulative = true;
            else if (p == "noncumulative") cumulative = false;
            else if (std::isdigit((unsigned char)p[0])) {
              char* end = nullptr;
              int pm = (int)std::strtoul(p.c_str(), &end, 10) * 10;
              if (*end == '.' && std::isdigit((unsigned char)end[1])) {
                pm += end[1] - '0';
                end += 2;
                while (std::isdigit((unsigned char)*end)) ++end;
              }
              if (!*end) permille = pm;
              else die(c, "Failed to parse --dirstat/-X option parameter:\n  Failed to parse dirstat cut-off percentage '" + p + "'");
            } else {
              die(c, "Failed to parse --dirstat/-X option parameter:\n  Unknown dirstat parameter '" + p + "'");
            }
          }
        };
        if (auto cfg = c.config.get("diff.dirstat")) take(*cfg);
        take(o.dirstat);
        std::vector<DirstatFile> files;
        unsigned long changed = 0;
        if (mode == "lines") {
          compute_stats();
          for (size_t i = 0; i < pairs.size(); ++i) {
            if (pairs[i].status == 'U') continue;
            unsigned long damage = (unsigned long)(stats[i].binary ? 0 : stats[i].add + stats[i].del);
            if (stats[i].binary) {
              const unsigned long bytes = (unsigned long)(stats[i].old_size + stats[i].new_size);
              damage = (bytes + 63) / 64;   // git counts a binary file's bytes, 64 to a "line"
            }
            const auto& pr = pairs[i];
            files.push_back({shown_name(pr.status == 'D' ? pr.old_path : pr.new_path), damage});
            changed += damage;
          }
        } else {
          for (auto& pr : pairs) {
            if (pr.status == 'U') continue;
            unsigned long damage;
            if (pr.a.present && pr.b.present && pr.a.oid == pr.b.oid && !pr.a_text && !pr.b_text) damage = 0;
            else if (mode == "files") damage = 1;
            else {
              const std::string ta = pr.a.present ? pair_text(c, pr, false) : std::string();
              const std::string tb = pr.b.present ? pair_text(c, pr, true) : std::string();
              unsigned long copied = 0, added = 0;
              if (pr.a.present && pr.b.present) count_copied_added(ta, !is_binary(ta), tb, !is_binary(tb), copied, added);
              else if (pr.b.present) added = tb.size();
              damage = (ta.size() - copied) + added;
              if (!damage) damage = 1;
            }
            files.push_back({shown_name(pr.status == 'D' ? pr.old_path : pr.new_path), damage});
            changed += damage;
          }
        }
        if (!changed) break;
        std::sort(files.begin(), files.end(), [](const DirstatFile& x, const DirstatFile& y) { return x.name < y.name; });
        size_t at = 0;
        gather_dirstat(files, at, changed, "", permille, cumulative, out);
        wrote_before = true;
        break;
      }
      case DiffOptions::Scopes:
        // the named units each file's change touched (aios-git's own: git has no such view)
        for (auto& p : pairs) {
          const std::string& path = p.status == 'D' ? p.old_path : p.new_path;
          auto oa = p.a.present ? pair_outline(c, p, false) : std::optional<GitSyntaxOutline>(GitSyntaxOutline{});
          auto ob = p.b.present ? pair_outline(c, p, true) : std::optional<GitSyntaxOutline>(GitSyntaxOutline{});
          if (!oa || !ob) { out << show_path(path) << "\t(no outline)" << term; continue; }
          out << show_path(path) << term;
          for (auto& ch : git_structural_scope_changes(*oa, *ob))
            out << "  " << ch.how << " " << (ch.scope.empty() ? "(file)" : ch.scope) << term;
        }
        break;
      case DiffOptions::Summary:
        for (auto& p : pairs) {
          if (p.status == 'U') continue;
          if (p.status == 'A') out << " create mode " << mode_str(p.b.mode) << " " << quote_path(shown_name(p.new_path)) << "\n";
          else if (p.status == 'D') out << " delete mode " << mode_str(p.a.mode) << " " << quote_path(shown_name(p.old_path)) << "\n";
          else if (p.status == 'R' || p.status == 'C') {
            out << (p.status == 'R' ? " rename " : " copy ") << pprint_rename(shown_name(p.old_path), shown_name(p.new_path)) << " (" << p.score << "%)\n";
            if (p.a.mode != p.b.mode) out << " mode change " << mode_str(p.a.mode) << " => " << mode_str(p.b.mode) << " " << quote_path(shown_name(p.new_path)) << "\n";
          } else if (p.a.mode != p.b.mode)
            out << " mode change " << mode_str(p.a.mode) << " => " << mode_str(p.b.mode) << " " << quote_path(shown_name(p.new_path)) << "\n";
        }
        wrote_before = true;
        break;
      case DiffOptions::Patch: {
        if (wrote_before && formats.front() != DiffOptions::Quiet) out << "\n";
        // a type change is a deletion and then an addition
        std::vector<DiffPair> halves;
        for (auto& p : pairs) {
          if (p.status == 'U') { out << "* Unmerged path " << quote_path(shown_name(p.new_path)) << "\n"; continue; }
          if (p.status != 'T') { halves.push_back(p); continue; }
          DiffPair d = p, ad = p;
          d.status = 'D'; d.b = FileState{}; d.b_text.reset();
          ad.status = 'A'; ad.a = FileState{}; ad.a_text.reset();
          halves.push_back(d);
          halves.push_back(ad);
        }
        int moved = o.color_moved;
        if (moved < 0) {
          moved = 0;
          if (auto v = c.config.get("diff.colormoved")) {
            moved = parse_color_moved(*v);
            if (moved < 0) die(c, "bad config variable 'diff.colormoved'");   // git refuses it before running
          }
        }
        if (moved && c.color && o.words == DiffOptions::WordsNone) {
          MovedRun run;
          run.mode = moved;
          run.collecting = true;
          g_moved = &run;
          std::ostringstream sink;
          for (auto& p : halves) emit_patch(c, o, p, rel, text_opts, abbrev_len, sink);
          run.collecting = false;
          mark_moved(run);
          for (auto& p : halves) emit_patch(c, o, p, rel, text_opts, abbrev_len, out);
          g_moved = nullptr;
          break;
        }
        for (auto& p : halves) emit_patch(c, o, p, rel, text_opts, abbrev_len, out);
        break;
      }
    }
  }
  if (!o.line_prefix.empty()) {
    std::string text = prefixed.str();
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      size_t end = nl == std::string::npos ? text.size() : nl + 1;
      out_final << o.line_prefix << text.substr(at, end - at);
      at = end;
    }
  }
  return true;
}

void print_commit_summary(Ctx& c, const std::string& from_tree, const std::string& to_tree, std::ostream& out) {
  DiffOptions o;
  o.format = DiffOptions::ShortStat;
  o.extra = {DiffOptions::Summary};
  auto pairs = diff_trees(c, from_tree, to_tree, Pathspec{}, o);
  if (pairs.empty()) return;
  print_diff(c, pairs, o, out);
}

}  // namespace aios_git

namespace aios_git {
std::string rename_display(const std::string& from, const std::string& to) { return pprint_rename(from, to); }
}  // namespace aios_git
