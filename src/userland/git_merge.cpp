#include "aios/userland/git_merge.hpp"
#include "aios/userland/git_structural.hpp"

#include <algorithm>
#include <climits>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "aios/hal/sync.hpp"

namespace aios::userland {

namespace {

std::string str_of(const ByteBuffer& b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }
ByteSpan span_of(const std::string& s) { return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// Lines become small integers so the diff compares ints, never strings. One
// table is shared by every sequence that is compared against another.
struct LineIds {
  std::unordered_map<std::string_view, int> ids;
  std::vector<int> of(const std::vector<std::string_view>& lines) {
    std::vector<int> out;
    out.reserve(lines.size());
    for (auto l : lines) out.push_back(ids.emplace(l, (int)ids.size()).first->second);
    return out;
  }
};

// xdiff's indent heuristic (xdiffi.c: measure_split, score_add_split, score_cmp): how good a
// place `split` is to start or end a change group, judged by indentation and blank lines.
struct SplitScore { int effective_indent = 0, penalty = 0; };

int line_indent(std::string_view rec) {
  int ret = 0;
  for (char c : rec) {
    if (!std::isspace((unsigned char)c)) return ret;
    if (c == ' ') ret += 1;
    else if (c == '\t') ret += 8 - ret % 8;
    if (ret >= 200) return 200;
  }
  return -1;   // only whitespace
}

void score_split(const std::vector<std::string_view>& recs, long split, SplitScore& s) {
  const long n = (long)recs.size();
  const bool end_of_file = split >= n;
  const int indent_here = end_of_file ? -1 : line_indent(recs[(size_t)split]);
  int pre_blank = 0, pre_indent = -1, post_blank = 0, post_indent = -1;
  for (long i = split - 1; i >= 0; --i) {
    pre_indent = line_indent(recs[(size_t)i]);
    if (pre_indent != -1) break;
    if (++pre_blank == 20) { pre_indent = 0; break; }
  }
  for (long i = split + 1; i < n; ++i) {
    post_indent = line_indent(recs[(size_t)i]);
    if (post_indent != -1) break;
    if (++post_blank == 20) { post_indent = 0; break; }
  }
  if (pre_indent == -1 && pre_blank == 0) s.penalty += 1;      // start of file
  if (end_of_file) s.penalty += 21;
  const int post = indent_here == -1 ? 1 + post_blank : 0;
  const int total_blank = pre_blank + post;
  s.penalty += -30 * total_blank;
  s.penalty += 6 * post;
  const int indent = indent_here != -1 ? indent_here : post_indent;
  const bool any_blanks = total_blank != 0;
  s.effective_indent += indent;
  if (indent == -1 || pre_indent == -1 || indent == pre_indent) return;
  if (indent > pre_indent) s.penalty += any_blanks ? 10 : -4;
  else if (post_indent != -1 && post_indent > indent) s.penalty += any_blanks ? 17 : 24;
  else s.penalty += any_blanks ? 17 : 23;
}

// xdiff's xdl_change_compact. A diff is often ambiguous about WHERE a group of changed lines
// sits: when the line after the group equals its first line, the group can slide down by one
// (and likewise up). git settles every group the same way, so a diff and a merge built on it
// are stable: slide as far up, then as far down as it goes (merging with groups it bumps
// into); then, if on the way it lined up with a change in the other file, go back to the last
// such position; otherwise, with `text` given (the indent heuristic `git diff` runs with),
// take the best-scoring position, and without it stay at the bottom.
void change_compact(const std::vector<int>& rec, std::vector<char>& chg, const std::vector<char>& ochg,
                    const std::vector<std::string_view>* text) {
  const long n = (long)rec.size(), on = (long)ochg.size();
  auto C = [&](long i) { return i >= 0 && i < n && chg[(size_t)i]; };
  auto O = [&](long i) { return i >= 0 && i < on && ochg[(size_t)i]; };
  struct G { long start = 0, end = 0; } g, go;
  while (C(g.end)) ++g.end;
  while (O(go.end)) ++go.end;
  auto g_next = [&]() { if (g.end == n) return false; g.start = g.end + 1; for (g.end = g.start; C(g.end); ++g.end) {} return true; };
  auto o_next = [&]() { if (go.end == on) return false; go.start = go.end + 1; for (go.end = go.start; O(go.end); ++go.end) {} return true; };
  auto o_prev = [&]() { if (go.start == 0) return false; go.end = go.start - 1; for (go.start = go.end; O(go.start - 1); --go.start) {} return true; };
  auto slide_down = [&]() {
    if (g.end < n && rec[(size_t)g.start] == rec[(size_t)g.end]) {
      chg[(size_t)g.start++] = 0;
      chg[(size_t)g.end++] = 1;
      while (C(g.end)) ++g.end;
      return true;
    }
    return false;
  };
  auto slide_up = [&]() {
    if (g.start > 0 && rec[(size_t)g.start - 1] == rec[(size_t)g.end - 1]) {
      chg[(size_t)--g.start] = 1;
      chg[(size_t)--g.end] = 0;
      while (C(g.start - 1)) --g.start;
      return true;
    }
    return false;
  };

  for (;;) {
    if (g.end != g.start) {
      long groupsize, earliest_end, end_matching_other;
      do {
        groupsize = g.end - g.start;
        end_matching_other = -1;
        while (slide_up()) if (!o_prev()) return;
        earliest_end = g.end;
        if (go.end > go.start) end_matching_other = g.end;
        while (slide_down()) {
          if (!o_next()) return;
          if (go.end > go.start) end_matching_other = g.end;
        }
      } while (groupsize != g.end - g.start);

      if (g.end == earliest_end) {
        // it could not move at all
      } else if (end_matching_other != -1) {
        while (go.end == go.start) {
          if (!slide_up() || !o_prev()) return;
        }
      } else if (text) {
        long shift = earliest_end, best_shift = -1;
        SplitScore best;
        if (g.end - groupsize - 1 > shift) shift = g.end - groupsize - 1;
        if (g.end - 100 > shift) shift = g.end - 100;
        for (; shift <= g.end; ++shift) {
          SplitScore score;
          score_split(*text, shift, score);
          score_split(*text, shift - groupsize, score);
          int cmp = 0;
          if (best_shift != -1) {
            int ind = (score.effective_indent > best.effective_indent) - (score.effective_indent < best.effective_indent);
            cmp = 60 * ind + (score.penalty - best.penalty);
          }
          if (best_shift == -1 || cmp <= 0) { best = score; best_shift = shift; }
        }
        while (g.end > best_shift) {
          if (!slide_up() || !o_prev()) return;
        }
      }
    }
    if (!g_next()) break;
    if (!o_next()) return;
  }
}

// git's line diff, as xdiff does it (xprepare.c + xdiffi.c), so the result is the one git
// prints and merges on - which is NOT always the minimal edit script:
//   1. the common head and tail are set aside (xdl_trim_ends);
//   2. lines with no counterpart in the other file are marked changed outright, and lines with
//      very many counterparts are too when they sit among such lines (xdl_cleanup_records);
//   3. what is left goes through Myers' divide and conquer (xdl_recs_cmp / xdl_split), which
//      gives up on optimality when the search gets expensive (the snake and max-cost heuristics);
//   4. each change group is put where git puts it (change_compact above).
// Marks every line of a that is deleted and every line of b that is inserted.
class Myers {
 public:
  // `ta`/`tb`, when given, are the lines themselves: the indent heuristic reads them.
  // `compact` false is xdl_do_diff alone, what the histogram diff falls back to.
  Myers(const std::vector<int>& a, const std::vector<int>& b,
        const std::vector<std::string_view>* ta = nullptr, const std::vector<std::string_view>* tb = nullptr,
        bool compact = true, bool minimal = false)
      : a_(a), b_(b), del_(a.size(), 0), ins_(b.size(), 0) {
    prepare();
    const long n1 = (long)ha1_.size(), n2 = (long)ha2_.size();
    const long ndiags = n1 + n2 + 3;
    kv_.assign((size_t)(2 * ndiags + 2), 0);
    kvdf_ = kv_.data() + (n2 + 1);
    kvdb_ = kv_.data() + ndiags + (n2 + 1);
    mxcost_ = std::max<long>(bogosqrt(ndiags), 256);
    recs_cmp(0, n1, 0, n2, minimal);
    if (!compact) return;
    change_compact(a_, del_, ins_, ta);
    change_compact(b_, ins_, del_, tb);
  }
  const std::vector<char>& deleted() const { return del_; }
  const std::vector<char>& inserted() const { return ins_; }
  std::vector<GitDiffEdit> edits() const { return edits_of(del_, ins_); }
  static std::vector<GitDiffEdit> edits_of(const std::vector<char>& del, const std::vector<char>& ins) {
    std::vector<GitDiffEdit> out;
    size_t i = 0, j = 0, n = del.size(), m = ins.size();
    while (i < n || j < m) {
      if (i < n && j < m && !del[i] && !ins[j]) { ++i; ++j; continue; }
      GitDiffEdit e{i, 0, j, 0};
      while (i < n && del[i]) { ++i; ++e.a_len; }
      while (j < m && ins[j]) { ++j; ++e.b_len; }
      out.push_back(e);
    }
    return out;
  }

 private:
  static constexpr long kSnakeCnt = 20, kHeurMin = 256, kKHeur = 4, kMaxEqLimit = 1024, kSimscanWindow = 100, kKpdisRun = 4;

  static long bogosqrt(long n) {
    long i = 1;
    for (; n > 0; n >>= 2) i <<= 1;
    return i;
  }

  // Whether a line with many matches, at i, sits in a run that is mostly lines with no match.
  static bool clean_mmatch(const std::vector<char>& dis, long i, long s, long e) {
    if (i - s > kSimscanWindow) s = i - kSimscanWindow;
    if (e - i > kSimscanWindow) e = i + kSimscanWindow;
    long r, rdis0 = 0, rpdis0 = 1, rdis1 = 0, rpdis1 = 1;
    for (r = 1; (i - r) >= s; ++r) {
      if (!dis[(size_t)(i - r)]) ++rdis0;
      else if (dis[(size_t)(i - r)] == 2) ++rpdis0;
      else break;
    }
    if (rdis0 == 0) return false;
    for (r = 1; (i + r) <= e; ++r) {
      if (!dis[(size_t)(i + r)]) ++rdis1;
      else if (dis[(size_t)(i + r)] == 2) ++rpdis1;
      else break;
    }
    if (rdis1 == 0) return false;
    rdis1 += rdis0;
    rpdis1 += rpdis0;
    return rpdis1 * kKpdisRun < (rpdis1 + rdis1);
  }

  void prepare() {
    const long n1 = (long)a_.size(), n2 = (long)b_.size();
    int max_id = -1;
    for (int v : a_) max_id = std::max(max_id, v);
    for (int v : b_) max_id = std::max(max_id, v);
    std::vector<long> c1((size_t)(max_id + 1), 0), c2((size_t)(max_id + 1), 0);
    for (int v : a_) ++c1[(size_t)v];
    for (int v : b_) ++c2[(size_t)v];

    long i = 0, lim = std::min(n1, n2);
    for (; i < lim && a_[(size_t)i] == b_[(size_t)i]; ++i) {}
    const long dstart = i;
    long j = 0;
    for (lim -= i; j < lim && a_[(size_t)(n1 - 1 - j)] == b_[(size_t)(n2 - 1 - j)]; ++j) {}
    const long dend1 = n1 - j - 1, dend2 = n2 - j - 1;

    std::vector<char> dis1((size_t)n1 + 1, 0), dis2((size_t)n2 + 1, 0);
    long mlim = std::min(bogosqrt(n1), kMaxEqLimit);
    for (i = dstart; i <= dend1; ++i) {
      long nm = c2[(size_t)a_[(size_t)i]];
      dis1[(size_t)i] = nm == 0 ? 0 : nm >= mlim ? 2 : 1;
    }
    mlim = std::min(bogosqrt(n2), kMaxEqLimit);
    for (i = dstart; i <= dend2; ++i) {
      long nm = c1[(size_t)b_[(size_t)i]];
      dis2[(size_t)i] = nm == 0 ? 0 : nm >= mlim ? 2 : 1;
    }
    for (i = dstart; i <= dend1; ++i) {
      if (dis1[(size_t)i] == 1 || (dis1[(size_t)i] == 2 && !clean_mmatch(dis1, i, dstart, dend1))) {
        rindex1_.push_back(i);
        ha1_.push_back(a_[(size_t)i]);
      } else del_[(size_t)i] = 1;
    }
    for (i = dstart; i <= dend2; ++i) {
      if (dis2[(size_t)i] == 1 || (dis2[(size_t)i] == 2 && !clean_mmatch(dis2, i, dstart, dend2))) {
        rindex2_.push_back(i);
        ha2_.push_back(b_[(size_t)i]);
      } else ins_[(size_t)i] = 1;
    }
  }

  struct Split { long i1 = 0, i2 = 0; bool min_lo = true, min_hi = true; };

  void recs_cmp(long off1, long lim1, long off2, long lim2, bool need_min) {
    for (; off1 < lim1 && off2 < lim2 && ha1_[(size_t)off1] == ha2_[(size_t)off2]; ++off1, ++off2) {}
    for (; off1 < lim1 && off2 < lim2 && ha1_[(size_t)lim1 - 1] == ha2_[(size_t)lim2 - 1]; --lim1, --lim2) {}
    if (off1 == lim1) {
      for (; off2 < lim2; ++off2) ins_[(size_t)rindex2_[(size_t)off2]] = 1;
    } else if (off2 == lim2) {
      for (; off1 < lim1; ++off1) del_[(size_t)rindex1_[(size_t)off1]] = 1;
    } else {
      Split spl;
      split(off1, lim1, off2, lim2, need_min, spl);
      recs_cmp(off1, spl.i1, off2, spl.i2, spl.min_lo);
      recs_cmp(spl.i1, lim1, spl.i2, lim2, spl.min_hi);
    }
  }

  // Where the forward and backward searches meet - or, once the search has cost too much, the
  // most promising point reached so far.
  void split(long off1, long lim1, long off2, long lim2, bool need_min, Split& spl) {
    const int* ha1 = ha1_.data();
    const int* ha2 = ha2_.data();
    long* kvdf = kvdf_;
    long* kvdb = kvdb_;
    const long dmin = off1 - lim2, dmax = lim1 - off2;
    const long fmid = off1 - off2, bmid = lim1 - lim2;
    const bool odd = ((fmid - bmid) & 1) != 0;
    long fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid;
    long d, i1, i2, prev1, best, dd, v, k;
    kvdf[fmid] = off1;
    kvdb[bmid] = lim1;
    for (long ec = 1;; ++ec) {
      bool got_snake = false;
      if (fmin > dmin) kvdf[--fmin - 1] = -1; else ++fmin;
      if (fmax < dmax) kvdf[++fmax + 1] = -1; else --fmax;
      for (d = fmax; d >= fmin; d -= 2) {
        if (kvdf[d - 1] >= kvdf[d + 1]) i1 = kvdf[d - 1] + 1;
        else i1 = kvdf[d + 1];
        prev1 = i1;
        i2 = i1 - d;
        for (; i1 < lim1 && i2 < lim2 && ha1[i1] == ha2[i2]; ++i1, ++i2) {}
        if (i1 - prev1 > kSnakeCnt) got_snake = true;
        kvdf[d] = i1;
        if (odd && bmin <= d && d <= bmax && kvdb[d] <= i1) {
          spl.i1 = i1; spl.i2 = i2; spl.min_lo = spl.min_hi = true;
          return;
        }
      }
      if (bmin > dmin) kvdb[--bmin - 1] = LONG_MAX; else ++bmin;
      if (bmax < dmax) kvdb[++bmax + 1] = LONG_MAX; else --bmax;
      for (d = bmax; d >= bmin; d -= 2) {
        if (kvdb[d - 1] < kvdb[d + 1]) i1 = kvdb[d - 1];
        else i1 = kvdb[d + 1] - 1;
        prev1 = i1;
        i2 = i1 - d;
        for (; i1 > off1 && i2 > off2 && ha1[i1 - 1] == ha2[i2 - 1]; --i1, --i2) {}
        if (prev1 - i1 > kSnakeCnt) got_snake = true;
        kvdb[d] = i1;
        if (!odd && fmin <= d && d <= fmax && i1 <= kvdf[d]) {
          spl.i1 = i1; spl.i2 = i2; spl.min_lo = spl.min_hi = true;
          return;
        }
      }
      if (need_min) continue;

      // Past the heuristic's trigger cost and with a long snake seen: take a diagonal that has
      // got far and ends in a run of matching lines.
      if (got_snake && ec > kHeurMin) {
        for (best = 0, d = fmax; d >= fmin; d -= 2) {
          dd = d > fmid ? d - fmid : fmid - d;
          i1 = kvdf[d];
          i2 = i1 - d;
          v = (i1 - off1) + (i2 - off2) - dd;
          if (v > kKHeur * ec && v > best && off1 + kSnakeCnt <= i1 && i1 < lim1 && off2 + kSnakeCnt <= i2 && i2 < lim2) {
            for (k = 1; ha1[i1 - k] == ha2[i2 - k]; ++k)
              if (k == kSnakeCnt) { best = v; spl.i1 = i1; spl.i2 = i2; break; }
          }
        }
        if (best > 0) { spl.min_lo = true; spl.min_hi = false; return; }
        for (best = 0, d = bmax; d >= bmin; d -= 2) {
          dd = d > bmid ? d - bmid : bmid - d;
          i1 = kvdb[d];
          i2 = i1 - d;
          v = (lim1 - i1) + (lim2 - i2) - dd;
          if (v > kKHeur * ec && v > best && off1 < i1 && i1 <= lim1 - kSnakeCnt && off2 < i2 && i2 <= lim2 - kSnakeCnt) {
            for (k = 0; ha1[i1 + k] == ha2[i2 + k]; ++k)
              if (k == kSnakeCnt - 1) { best = v; spl.i1 = i1; spl.i2 = i2; break; }
          }
        }
        if (best > 0) { spl.min_lo = false; spl.min_hi = true; return; }
      }

      // Enough is enough: take the furthest-reaching path so far.
      if (ec >= mxcost_) {
        long fbest = -1, fbest1 = -1;
        for (d = fmax; d >= fmin; d -= 2) {
          i1 = std::min(kvdf[d], lim1);
          i2 = i1 - d;
          if (lim2 < i2) { i1 = lim2 + d; i2 = lim2; }
          if (fbest < i1 + i2) { fbest = i1 + i2; fbest1 = i1; }
        }
        long bbest = LONG_MAX, bbest1 = LONG_MAX;
        for (d = bmax; d >= bmin; d -= 2) {
          i1 = std::max(off1, kvdb[d]);
          i2 = i1 - d;
          if (i2 < off2) { i1 = off2 + d; i2 = off2; }
          if (i1 + i2 < bbest) { bbest = i1 + i2; bbest1 = i1; }
        }
        if ((lim1 + lim2) - bbest < fbest - (off1 + off2)) {
          spl.i1 = fbest1; spl.i2 = fbest - fbest1; spl.min_lo = true; spl.min_hi = false;
        } else {
          spl.i1 = bbest1; spl.i2 = bbest - bbest1; spl.min_lo = false; spl.min_hi = true;
        }
        return;
      }
    }
  }

  const std::vector<int>& a_;
  const std::vector<int>& b_;
  std::vector<char> del_, ins_;
  std::vector<int> ha1_, ha2_;          // the lines that take part in the search...
  std::vector<long> rindex1_, rindex2_; // ...and where each one sits in its file
  std::vector<long> kv_;
  long* kvdf_ = nullptr;
  long* kvdb_ = nullptr;
  long mxcost_ = 256;
};

// xdiff's histogram diff (xhistogram.c), which merge-ort runs every content merge with. Lines
// are 1-based here, as there. It looks for the longest common run built around the line that
// occurs least often in the first file, splits on it and recurses; where every common line is
// very frequent it falls back to Myers on that stretch. Unlike Myers it sees the whole file:
// xdiff trims no common head or tail and drops no lines before a histogram diff.
class Histogram {
 public:
  Histogram(const std::vector<int>& a, const std::vector<int>& b)
      : a_(a), b_(b), del_(a.size(), 0), ins_(b.size(), 0) {
    diff(1, (long)a.size(), 1, (long)b.size());
  }
  std::vector<char>& deleted() { return del_; }
  std::vector<char>& inserted() { return ins_; }

 private:
  static constexpr long kMaxChainLength = 64;
  struct Region { long begin1 = 0, end1 = 0, begin2 = 0, end2 = 0; };
  struct Rec { long ptr = 0, cnt = 0; };
  struct Index {
    long line1 = 0, end1 = 0, line2 = 0, end2 = 0;
    std::unordered_map<int, size_t> by_id;   // line content -> record
    std::vector<Rec> recs;
    std::vector<size_t> line_map;            // line - line1 -> record
    std::vector<long> next_ptrs;             // line - line1 -> next line with the same content (0: none)
    long cnt = kMaxChainLength + 1;
    bool has_common = false;
    long count_at(long l) const { return recs[line_map[(size_t)(l - line1)]].cnt; }
    long& next_ptr(long l) { return next_ptrs[(size_t)(l - line1)]; }
  };

  int A(long l) const { return a_[(size_t)(l - 1)]; }
  int B(long l) const { return b_[(size_t)(l - 1)]; }

  long try_lcs(Index& ix, Region& lcs, long b_ptr) {
    long b_next = b_ptr + 1;
    auto it = ix.by_id.find(B(b_ptr));
    if (it == ix.by_id.end()) return b_next;
    const Rec& rec = ix.recs[it->second];
    if (rec.cnt > ix.cnt) { ix.has_common = true; return b_next; }
    long as = rec.ptr;
    ix.has_common = true;
    for (;;) {
      long np = ix.next_ptr(as);
      long bs = b_ptr, ae = as, be = bs, rc = rec.cnt;
      while (ix.line1 < as && ix.line2 < bs && A(as - 1) == B(bs - 1)) {
        --as; --bs;
        if (1 < rc) rc = std::min(rc, ix.count_at(as));
      }
      while (ae < ix.end1 && be < ix.end2 && A(ae + 1) == B(be + 1)) {
        ++ae; ++be;
        if (1 < rc) rc = std::min(rc, ix.count_at(ae));
      }
      if (b_next <= be) b_next = be + 1;
      if (lcs.end1 - lcs.begin1 < ae - as || rc < ix.cnt) {
        lcs.begin1 = as; lcs.begin2 = bs; lcs.end1 = ae; lcs.end2 = be;
        ix.cnt = rc;
      }
      if (np == 0) break;
      while (np <= ae) {
        np = ix.next_ptr(np);
        if (np == 0) return b_next;
      }
      as = np;
    }
    return b_next;
  }

  // 1: fall back to Myers; 0: `lcs` holds the split (all zero when nothing is common).
  int find_lcs(Region& lcs, long line1, long count1, long line2, long count2) {
    Index ix;
    ix.line1 = line1; ix.end1 = line1 + count1 - 1;
    ix.line2 = line2; ix.end2 = line2 + count2 - 1;
    ix.line_map.assign((size_t)count1, 0);
    ix.next_ptrs.assign((size_t)count1, 0);
    for (long ptr = ix.end1; ptr >= line1; --ptr) {   // scanA: each record ends up at its first line
      auto [it, fresh] = ix.by_id.emplace(A(ptr), ix.recs.size());
      if (fresh) {
        ix.recs.push_back(Rec{ptr, 1});
      } else {
        Rec& r = ix.recs[it->second];
        ix.next_ptr(ptr) = r.ptr;
        r.ptr = ptr;
        if (r.cnt < INT_MAX) ++r.cnt;
      }
      ix.line_map[(size_t)(ptr - line1)] = it->second;
    }
    for (long b_ptr = line2; b_ptr <= ix.end2;) b_ptr = try_lcs(ix, lcs, b_ptr);
    return ix.has_common && kMaxChainLength < ix.cnt ? 1 : 0;
  }

  void fall_back(long line1, long count1, long line2, long count2) {
    std::vector<int> sa(a_.begin() + (line1 - 1), a_.begin() + (line1 - 1 + count1));
    std::vector<int> sb(b_.begin() + (line2 - 1), b_.begin() + (line2 - 1 + count2));
    Myers m(sa, sb, nullptr, nullptr, false);
    std::copy(m.deleted().begin(), m.deleted().end(), del_.begin() + (line1 - 1));
    std::copy(m.inserted().begin(), m.inserted().end(), ins_.begin() + (line2 - 1));
  }

  void diff(long line1, long count1, long line2, long count2) {
    for (;;) {
      if (count1 <= 0 && count2 <= 0) return;
      if (!count1) { while (count2--) ins_[(size_t)(line2++ - 1)] = 1; return; }
      if (!count2) { while (count1--) del_[(size_t)(line1++ - 1)] = 1; return; }
      Region lcs;
      if (find_lcs(lcs, line1, count1, line2, count2)) { fall_back(line1, count1, line2, count2); return; }
      if (lcs.begin1 == 0 && lcs.begin2 == 0) {
        while (count1--) del_[(size_t)(line1++ - 1)] = 1;
        while (count2--) ins_[(size_t)(line2++ - 1)] = 1;
        return;
      }
      diff(line1, lcs.begin1 - line1, line2, lcs.begin2 - line2);
      const long end1 = line1 + count1 - 1, end2 = line2 + count2 - 1;
      count1 = end1 - lcs.end1; line1 = lcs.end1 + 1;
      count2 = end2 - lcs.end2; line2 = lcs.end2 + 1;
    }
  }

  const std::vector<int>& a_;
  const std::vector<int>& b_;
  std::vector<char> del_, ins_;
};

// xdiff's patience diff (xpatience.c): anchor on the lines that occur exactly once on each side, keep the longest
// run of them that stays in order, and diff what lies between the anchors again; where a stretch has no such line
// it is handed to Myers. Lines are 1-based here, as there.
class Patience {
 public:
  Patience(const std::vector<int>& a, const std::vector<int>& b) : a_(a), b_(b), del_(a.size(), 0), ins_(b.size(), 0) {
    diff(1, (long)a.size(), 1, (long)b.size());
  }
  std::vector<char>& deleted() { return del_; }
  std::vector<char>& inserted() { return ins_; }

 private:
  static constexpr long kNonUnique = -1;
  struct Entry { long line1 = 0, line2 = 0; long next = -1, prev = -1; };

  int A(long l) const { return a_[(size_t)(l - 1)]; }
  int B(long l) const { return b_[(size_t)(l - 1)]; }

  void mark(long line1, long count1, long line2, long count2) {
    for (long i = 0; i < count1; ++i) del_[(size_t)(line1 - 1 + i)] = 1;
    for (long i = 0; i < count2; ++i) ins_[(size_t)(line2 - 1 + i)] = 1;
  }

  void fall_back(long line1, long count1, long line2, long count2) {
    std::vector<int> sa(a_.begin() + (line1 - 1), a_.begin() + (line1 - 1 + count1));
    std::vector<int> sb(b_.begin() + (line2 - 1), b_.begin() + (line2 - 1 + count2));
    Myers m(sa, sb, nullptr, nullptr, false);
    std::copy(m.deleted().begin(), m.deleted().end(), del_.begin() + (line1 - 1));
    std::copy(m.inserted().begin(), m.inserted().end(), ins_.begin() + (line2 - 1));
  }

  void walk(std::vector<Entry>& entries, long first, long line1, long count1, long line2, long count2) {
    const long end1 = line1 + count1, end2 = line2 + count2;
    for (;;) {
      long next1, next2;
      if (first >= 0) {
        next1 = entries[(size_t)first].line1;
        next2 = entries[(size_t)first].line2;
        while (next1 > line1 && next2 > line2 && A(next1 - 1) == B(next2 - 1)) { --next1; --next2; }
      } else {
        next1 = end1;
        next2 = end2;
      }
      while (line1 < next1 && line2 < next2 && A(line1) == B(line2)) { ++line1; ++line2; }
      if (next1 > line1 || next2 > line2) diff(line1, next1 - line1, line2, next2 - line2);
      if (first < 0) return;
      while (entries[(size_t)first].next >= 0 && entries[(size_t)entries[(size_t)first].next].line1 == entries[(size_t)first].line1 + 1 &&
             entries[(size_t)entries[(size_t)first].next].line2 == entries[(size_t)first].line2 + 1)
        first = entries[(size_t)first].next;
      line1 = entries[(size_t)first].line1 + 1;
      line2 = entries[(size_t)first].line2 + 1;
      first = entries[(size_t)first].next;
    }
  }

  void diff(long line1, long count1, long line2, long count2) {
    if (count1 <= 0 && count2 <= 0) return;
    if (!count1) { mark(0, 0, line2, count2); return; }
    if (!count2) { mark(line1, count1, 0, 0); return; }
    std::vector<Entry> entries;
    std::unordered_map<int, size_t> index;
    for (long l = line1; l < line1 + count1; ++l) {
      auto [it, fresh] = index.emplace(A(l), entries.size());
      if (fresh) entries.push_back(Entry{l, 0, -1, -1});
      else entries[it->second].line2 = kNonUnique;
    }
    bool has_matches = false;
    for (long l = line2; l < line2 + count2; ++l) {
      auto it = index.find(B(l));
      if (it == index.end()) continue;
      has_matches = true;
      Entry& e = entries[it->second];
      e.line2 = e.line2 ? kNonUnique : l;
    }
    if (!has_matches) { mark(line1, count1, line2, count2); return; }
    // the longest increasing run of the lines unique on both sides (patience sorting)
    std::vector<size_t> sequence;
    for (size_t i = 0; i < entries.size(); ++i) {
      Entry& e = entries[i];
      if (!e.line2 || e.line2 == kNonUnique) continue;
      long left = -1, right = (long)sequence.size();
      while (left + 1 < right) {
        long middle = left + (right - left) / 2;
        if (entries[sequence[(size_t)middle]].line2 > e.line2) right = middle;
        else left = middle;
      }
      e.prev = left < 0 ? -1 : (long)sequence[(size_t)left];
      size_t at = (size_t)(left + 1);
      if (at == sequence.size()) sequence.push_back(i);
      else sequence[at] = i;
    }
    if (sequence.empty()) { fall_back(line1, count1, line2, count2); return; }
    long entry = (long)sequence.back();
    entries[(size_t)entry].next = -1;
    while (entries[(size_t)entry].prev >= 0) {
      entries[(size_t)entries[(size_t)entry].prev].next = entry;
      entry = entries[(size_t)entry].prev;
    }
    walk(entries, entry, line1, count1, line2, count2);
  }

  const std::vector<int>& a_;
  const std::vector<int>& b_;
  std::vector<char> del_, ins_;
};

std::vector<GitDiffEdit> diff_ids(const std::vector<int>& a, const std::vector<int>& b,
                                  GitDiffAlgorithm alg = GitDiffAlgorithm::Myers,
                                  const std::vector<std::string_view>* ta = nullptr,
                                  const std::vector<std::string_view>* tb = nullptr) {
  if (alg == GitDiffAlgorithm::Myers) return Myers(a, b, ta, tb).edits();
  if (alg == GitDiffAlgorithm::Minimal) return Myers(a, b, ta, tb, true, true).edits();
  if (alg == GitDiffAlgorithm::Patience) {
    Patience p(a, b);
    change_compact(a, p.deleted(), p.inserted(), ta);
    change_compact(b, p.inserted(), p.deleted(), tb);
    return Myers::edits_of(p.deleted(), p.inserted());
  }
  Histogram h(a, b);
  change_compact(a, h.deleted(), h.inserted(), ta);
  change_compact(b, h.inserted(), h.deleted(), tb);
  return Myers::edits_of(h.deleted(), h.inserted());
}

void append_lines(std::string& out, const std::vector<std::string_view>& lines, size_t from, size_t count, bool add_nl) {
  for (size_t k = 0; k < count; ++k) out.append(lines[from + k]);
  if (add_nl && count > 0) {
    auto last = lines[from + count - 1];
    if (last.empty() || last.back() != '\n') out += '\n';
  }
}

bool has_nul(std::string_view s) { return s.find('\0') != std::string_view::npos; }

}  // namespace

std::vector<std::string_view> git_split_lines(std::string_view text) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
    out.push_back(text.substr(start, end - start));
    start = end;
  }
  return out;
}

std::vector<GitDiffEdit> git_diff_lines(const std::vector<std::string_view>& a,
                                        const std::vector<std::string_view>& b) {
  LineIds ids;
  auto ia = ids.of(a);
  auto ib = ids.of(b);
  return diff_ids(ia, ib);
}

std::vector<GitDiffEdit> git_diff_lines_indent(const std::vector<std::string_view>& a,
                                               const std::vector<std::string_view>& b,
                                               const std::vector<std::string_view>* shown_a,
                                               const std::vector<std::string_view>* shown_b) {
  LineIds ids;
  auto ia = ids.of(a);
  auto ib = ids.of(b);
  return Myers(ia, ib, shown_a ? shown_a : &a, shown_b ? shown_b : &b).edits();
}

// -- unified hunks ----------------------------------------------------------------

namespace {
// How many leading bytes of s are whole UTF-8 characters, by git's own test (utf8.c
// pick_one_utf8_char: no overlongs, no surrogates, no U+FFFE/U+FFFF, nothing past U+10FFFF).
size_t utf8_valid_prefix(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  auto u = [&](size_t k) { return (unsigned char)s[k]; };
  while (i < n) {
    const unsigned char c = u(i);
    const size_t rem = n - i;
    size_t w;
    if (c < 0x80) w = 1;
    else if ((c & 0xe0) == 0xc0) {
      if (rem < 2 || (u(i + 1) & 0xc0) != 0x80 || (c & 0xfe) == 0xc0) break;
      w = 2;
    } else if ((c & 0xf0) == 0xe0) {
      if (rem < 3 || (u(i + 1) & 0xc0) != 0x80 || (u(i + 2) & 0xc0) != 0x80 ||
          (c == 0xe0 && (u(i + 1) & 0xe0) == 0x80) || (c == 0xed && (u(i + 1) & 0xe0) == 0xa0) ||
          (c == 0xef && u(i + 1) == 0xbf && (u(i + 2) & 0xfe) == 0xbe))
        break;
      w = 3;
    } else if ((c & 0xf8) == 0xf0) {
      if (rem < 4 || (u(i + 1) & 0xc0) != 0x80 || (u(i + 2) & 0xc0) != 0x80 || (u(i + 3) & 0xc0) != 0x80 ||
          (c == 0xf0 && (u(i + 1) & 0xf0) == 0x80) || (c == 0xf4 && u(i + 1) > 0x8f) || c > 0xf4)
        break;
      w = 4;
    } else break;
    i += w;
  }
  return i;
}
}  // namespace

namespace {

bool is_blank_line(std::string_view line, bool ws_flags) {
  if (!ws_flags) return line.size() <= 1;   // just "\n" (or nothing)
  for (char ch : line) if (!std::isspace((unsigned char)ch)) return false;
  return true;
}

// The text two lines are compared by when whitespace is (partly) ignored, in the spirit of xdiff's
// xdl_hash_record_with_whitespace and xdl_recmatch: the newline never counts.
std::string whitespace_key(std::string_view line, const GitDiffTextOptions& o) {
  std::string key;
  size_t end = line.size();
  if (end && line[end - 1] == '\n') --end;
  if (o.ignore_cr_at_eol && end && line[end - 1] == '\r') --end;
  size_t i = 0;
  while (i < end) {
    char ch = line[i];
    if (!std::isspace((unsigned char)ch)) { key += ch; ++i; continue; }
    size_t run_end = i;
    while (run_end < end && std::isspace((unsigned char)line[run_end])) ++run_end;
    const bool at_eol = run_end == end;
    if (o.ignore_all_space) { /* dropped */ }
    else if (o.ignore_space_change) { if (!at_eol) key += ' '; }
    else if (o.ignore_space_at_eol) { if (!at_eol) key.append(line.substr(i, run_end - i)); }
    else key.append(line.substr(i, run_end - i));
    i = run_end;
  }
  return key;
}

}  // namespace

std::vector<GitDiffHunk> git_diff_hunks(std::string_view a_text, std::string_view b_text, const GitDiffTextOptions& o) {
  const long ctx = std::max(0, o.context);
  const bool ws = o.ignore_all_space || o.ignore_space_change || o.ignore_space_at_eol || o.ignore_cr_at_eol;
  if (ctx == 0 && !o.function_context) {
    // xdiff-interface.c's trim_common_tail: with no context to show, git drops the identical
    // tail of both files in 1024-byte blocks (then gives back the part of the last block after
    // its first newline) before diffing. Fewer lines are counted, so which lines xdiff treats as
    // too frequent to anchor on can change, and with it the diff.
    const size_t blk = 1024;
    size_t trimmed = 0, recovered = 0;
    const size_t smaller = std::min(a_text.size(), b_text.size());
    while (blk + trimmed <= smaller &&
           a_text.substr(a_text.size() - trimmed - blk, blk) == b_text.substr(b_text.size() - trimmed - blk, blk))
      trimmed += blk;
    const size_t at = a_text.size() - trimmed;
    while (recovered < trimmed)
      if (a_text[at + recovered++] == '\n') break;
    a_text.remove_suffix(trimmed - recovered);
    b_text.remove_suffix(trimmed - recovered);
  }
  auto a = git_split_lines(a_text);
  auto b = git_split_lines(b_text);
  LineIds line_ids;
  std::vector<std::string> keys;
  std::vector<int> ia, ib;
  if (ws) {
    keys.reserve(a.size() + b.size());
    std::vector<std::string_view> ka, kb;
    for (auto l : a) keys.push_back(whitespace_key(l, o));
    for (auto l : b) keys.push_back(whitespace_key(l, o));
    for (size_t i = 0; i < a.size(); ++i) ka.push_back(keys[i]);
    for (size_t i = 0; i < b.size(); ++i) kb.push_back(keys[a.size() + i]);
    ia = line_ids.of(ka);
    ib = line_ids.of(kb);
  } else {
    ia = line_ids.of(a);
    ib = line_ids.of(b);
  }
  auto edits = diff_ids(ia, ib, o.algorithm, o.indent_heuristic ? &a : nullptr, o.indent_heuristic ? &b : nullptr);
  std::vector<GitDiffHunk> hunks;
  if (edits.empty()) return hunks;

  // changes made only of blank lines (or lines -I matches) do not start a hunk of their own
  std::vector<char> ignore(edits.size(), 0);
  if (o.ignore_blank_lines || o.ignore_line) {
    for (size_t e = 0; e < edits.size(); ++e) {
      bool all_blank = o.ignore_blank_lines, all_match = (bool)o.ignore_line;
      for (size_t k = 0; k < edits[e].a_len; ++k) {
        if (all_blank && !is_blank_line(a[edits[e].a_start + k], ws)) all_blank = false;
        if (all_match && !o.ignore_line(a[edits[e].a_start + k])) all_match = false;
      }
      for (size_t k = 0; k < edits[e].b_len; ++k) {
        if (all_blank && !is_blank_line(b[edits[e].b_start + k], ws)) all_blank = false;
        if (all_match && !o.ignore_line(b[edits[e].b_start + k])) all_match = false;
      }
      ignore[e] = all_blank || all_match;
    }
  }

  const long n1 = (long)a.size(), n2 = (long)b.size();
  auto is_func_rec = [&](long l) {
    std::string_view r = a[(size_t)l];
    return !r.empty() && (std::isalpha((unsigned char)r[0]) || r[0] == '_' || r[0] == '$');
  };
  // xemit.c get_func_line: the nearest function line from `start` towards `limit` (exclusive)
  auto get_func_line = [&](long start, long limit) -> long {
    const long step = start > limit ? -1 : 1;
    for (long l = start; l != limit && 0 <= l && l < n1; l += step)
      if (is_func_rec(l)) return l;
    return -1;
  };
  auto func_text = [&](long l) -> std::string {
    std::string_view r = a[(size_t)l];
    size_t len = std::min<size_t>(r.size(), 80);
    while (len > 0 && std::isspace((unsigned char)r[len - 1])) --len;
    // as git prints it, only the part that is valid UTF-8: the 80-byte cut can split a character
    return std::string(r.substr(0, utf8_valid_prefix(r.substr(0, len))));
  };

  const long max_common = 2 * ctx + std::max(0, o.inter_hunk_context);
  const long max_ignorable = ctx;
  auto I1 = [&](size_t e) { return (long)edits[e].a_start; };
  auto C1 = [&](size_t e) { return (long)edits[e].a_len; };
  auto I2 = [&](size_t e) { return (long)edits[e].b_start; };
  auto C2 = [&](size_t e) { return (long)edits[e].b_len; };
  size_t first = 0;
  long func_prev_line = -1;
  std::string func_line;
  while (first < edits.size()) {
    // xdl_get_hunk: which changes make up the next hunk (the last one of them is `last`)
    size_t scr = first;
    for (size_t p = first; p < edits.size() && ignore[p]; ++p) {
      size_t next = p + 1;
      if (next >= edits.size() || I1(next) - (I1(p) + C1(p)) >= max_ignorable) scr = next;
    }
    if (scr >= edits.size()) break;
    first = scr;
    size_t last = first;
    long ignored = 0;
    for (size_t e = first + 1; e < edits.size(); ++e) {
      long distance = I1(e) - (I1(e - 1) + C1(e - 1));
      if (distance > max_common) break;
      if (distance < max_ignorable && (!ignore[e] || last == e - 1)) { last = e; ignored = 0; }
      else if (distance < max_ignorable && ignore[e]) ignored += C2(e);
      else if (last != e - 1 && I1(e) + ignored - (I1(last) + C1(last)) > max_common) break;
      else if (!ignore[e]) { last = e; ignored = 0; }
      else ignored += C2(e);
    }
    size_t xch = first, xche = last;
    long s1 = std::max(I1(xch) - ctx, 0L), s2 = std::max(I2(xch) - ctx, 0L);
    long e1, e2;
    bool skip_pre = false;
    if (o.function_context) {
      long i1 = I1(xch);
      if (i1 >= n1) {
        // an appended chunk: no more context is wanted when a whole function was added
        for (long i2 = I2(xch); i2 < n2; ++i2) {
          std::string_view r = b[(size_t)i2];
          if (!r.empty() && (std::isalpha((unsigned char)r[0]) || r[0] == '_' || r[0] == '$')) { skip_pre = true; break; }
        }
        if (!skip_pre) i1 = n1 - 1;
      }
      if (!skip_pre) {
        long fs1 = get_func_line(i1, -1);
        if (fs1 < 0) fs1 = 0;
        if (fs1 < s1) { s2 = std::max(s2 - (s1 - fs1), 0L); s1 = fs1; }
      }
    }
    for (;;) {
      long lctx = std::min({ctx, n1 - (I1(xche) + C1(xche)), n2 - (I2(xche) + C2(xche))});
      e1 = I1(xche) + C1(xche) + lctx;
      e2 = I2(xche) + C2(xche) + lctx;
      if (!o.function_context) break;
      long fe1 = get_func_line(I1(xche) + C1(xche), n1);
      if (fe1 < 0) fe1 = n1;
      // the blank lines that lead up to the next function belong to neither
      while (fe1 > e1 && is_blank_line(a[(size_t)fe1 - 1], true)) --fe1;
      if (fe1 > e1) { e2 = std::min(e2 + (fe1 - e1), n2); e1 = fe1; }
      if (xche + 1 < edits.size()) {
        long l = std::min(I1(xche + 1), n1 - 1);
        if (l - ctx <= e1 || get_func_line(l, e1) < 0) { ++xche; continue; }
      }
      break;
    }
    GitDiffHunk h;
    h.old_start = (size_t)s1;
    h.old_count = (size_t)(e1 - s1);
    h.new_start = (size_t)s2;
    h.new_count = (size_t)(e2 - s2);
    long fl = get_func_line(s1 - 1, func_prev_line);
    if (fl >= 0) func_line = func_text(fl);
    func_prev_line = s1 - 1;
    h.func = func_line;
    long i = s2;
    for (; i < I2(xch); ++i) h.lines.push_back({' ', b[(size_t)i]});
    for (size_t e = xch;; ++e) {
      if (e > xch) {
        long s1c = I1(e - 1) + C1(e - 1), s2c = I2(e - 1) + C2(e - 1);
        for (; s1c < I1(e) && s2c < I2(e); ++s1c, ++s2c) h.lines.push_back({' ', b[(size_t)s2c]});
      }
      for (long k = 0; k < C1(e); ++k) h.lines.push_back({'-', a[(size_t)(I1(e) + k)]});
      for (long k = 0; k < C2(e); ++k) h.lines.push_back({'+', b[(size_t)(I2(e) + k)]});
      if (e == xche) break;
    }
    for (long k = I2(xche) + C2(xche); k < e2; ++k) h.lines.push_back({' ', b[(size_t)k]});
    hunks.push_back(std::move(h));
    first = xche + 1;
  }
  return hunks;
}

std::string git_unified_hunks(std::string_view a_text, std::string_view b_text, const GitDiffTextOptions& o) {
  std::string out;
  auto range = [](size_t s, size_t n) {
    std::string r = std::to_string(n ? s + 1 : s);
    if (n != 1) r += "," + std::to_string(n);
    return r;
  };
  for (auto& h : git_diff_hunks(a_text, b_text, o)) {
    out += "@@ -" + range(h.old_start, h.old_count) + " +" + range(h.new_start, h.new_count) + " @@";
    if (!h.func.empty()) out += " " + h.func;
    out += "\n";
    for (auto& l : h.lines) {
      out += l.tag;
      out.append(l.text);
      if (l.text.empty() || l.text.back() != '\n') out += "\n\\ No newline at end of file\n";
    }
  }
  return out;
}

std::string git_unified_hunks(std::string_view a_text, std::string_view b_text, int context) {
  GitDiffTextOptions o;
  o.context = context;
  return git_unified_hunks(a_text, b_text, o);
}

// -- three-way merge ----------------------------------------------------------------
//
// A transcription of xdiff's xdl_do_merge, xdl_refine_conflicts,
// xdl_simplify_non_conflicts and xdl_fill_merge_buffer, on our diff. `i0/chg0`
// are base coordinates, `i1/chg1` ours, `i2/chg2` theirs. Mode 0 is a conflict,
// 1 takes ours, 2 takes theirs, 4 means both sides made the same change.

namespace {

struct MergeRegion { int mode; long i0, chg0, i1, chg1, i2, chg2; };

void append_region(std::vector<MergeRegion>& c, int mode, long i0, long chg0, long i1, long chg1, long i2, long chg2) {
  if (!c.empty()) {
    MergeRegion& m = c.back();
    if (i1 <= m.i1 + m.chg1 || i2 <= m.i2 + m.chg2) {
      if (mode != m.mode) m.mode = 0;
      m.chg0 = i0 + chg0 - m.i0;
      m.chg1 = i1 + chg1 - m.i1;
      m.chg2 = i2 + chg2 - m.i2;
      return;
    }
  }
  c.push_back(MergeRegion{mode, i0, chg0, i1, chg1, i2, chg2});
}

bool lines_contain_alnum(const std::vector<std::string_view>& lines, long from, long count) {
  for (long k = 0; k < count; ++k)
    for (char ch : lines[(size_t)(from + k)])
      if (std::isalnum((unsigned char)ch)) return true;
  return false;
}

}  // namespace

GitMerge3 git_merge3(std::string_view base_text, std::string_view ours_text, std::string_view theirs_text,
                     const GitMerge3Options& opt) {
  GitMerge3 res;
  // One side unchanged, or both the same: nothing to merge.
  if (ours_text == theirs_text || base_text == theirs_text) { res.merged = std::string(ours_text); return res; }
  if (base_text == ours_text) { res.merged = std::string(theirs_text); return res; }
  if (has_nul(base_text) || has_nul(ours_text) || has_nul(theirs_text)) {
    // Binary: there are no lines to merge. git's ll_binary_merge takes a side
    // when told which (union falls back to ours), and otherwise conflicts.
    res.binary = true;
    res.merged = std::string(opt.favor == GitMergeFavor::Theirs ? theirs_text : ours_text);
    if (opt.favor == GitMergeFavor::None) {
      res.clean = false;
      res.conflicts.push_back(GitMergeConflict{0, std::string(ours_text), std::string(theirs_text)});
    }
    return res;
  }

  auto base = git_split_lines(base_text);
  auto ours = git_split_lines(ours_text);
  auto theirs = git_split_lines(theirs_text);
  LineIds ids;
  auto ib = ids.of(base), io = ids.of(ours), it = ids.of(theirs);
  auto x1 = diff_ids(ib, io, opt.algorithm);   // base -> ours
  auto x2 = diff_ids(ib, it, opt.algorithm);   // base -> theirs

  auto same_lines = [&](size_t o_from, size_t t_from, size_t count) {
    for (size_t k = 0; k < count; ++k)
      if (io[o_from + k] != it[t_from + k]) return false;
    return true;
  };

  std::vector<MergeRegion> c;
  size_t p1 = 0, p2 = 0;
  while (p1 < x1.size() && p2 < x2.size()) {
    const auto& s1 = x1[p1];
    const auto& s2 = x2[p2];
    long s1i1 = (long)s1.a_start, s1c1 = (long)s1.a_len, s1i2 = (long)s1.b_start, s1c2 = (long)s1.b_len;
    long s2i1 = (long)s2.a_start, s2c1 = (long)s2.a_len, s2i2 = (long)s2.b_start, s2c2 = (long)s2.b_len;
    if (s1i1 + s1c1 < s2i1) {
      append_region(c, 1, s1i1, s1c1, s1i2, s1c2, s2i2 - s2i1 + s1i1, s1c1);
      ++p1;
      continue;
    }
    if (s2i1 + s2c1 < s1i1) {
      append_region(c, 2, s2i1, s2c1, s1i2 - s1i1 + s2i1, s2c1, s2i2, s2c2);
      ++p2;
      continue;
    }
    if (s1i1 != s2i1 || s1c1 != s2c1 || s1c2 != s2c2 || !same_lines(s1.b_start, s2.b_start, s1.b_len)) {
      long off = s1i1 - s2i1;
      long ffo = off + s1c1 - s2c1;
      long i0 = s1i1, i1 = s1i2, i2 = s2i2;
      if (off > 0) { i0 -= off; i1 -= off; } else { i2 += off; }
      long chg0 = s1i1 + s1c1 - i0;
      long chg1 = s1i2 + s1c2 - i1;
      long chg2 = s2i2 + s2c2 - i2;
      if (ffo < 0) { chg0 -= ffo; chg1 -= ffo; } else { chg2 += ffo; }
      append_region(c, 0, i0, chg0, i1, chg1, i2, chg2);
    }
    long e1 = s1i1 + s1c1, e2 = s2i1 + s2c1;
    if (e1 >= e2) ++p2;
    if (e2 >= e1) ++p1;
  }
  const long delta1 = (long)ours.size() - (long)base.size();
  const long delta2 = (long)theirs.size() - (long)base.size();
  for (; p1 < x1.size(); ++p1) {
    const auto& s = x1[p1];
    append_region(c, 1, (long)s.a_start, (long)s.a_len, (long)s.b_start, (long)s.b_len, (long)s.a_start + delta2, (long)s.a_len);
  }
  for (; p2 < x2.size(); ++p2) {
    const auto& s = x2[p2];
    append_region(c, 2, (long)s.a_start, (long)s.a_len, (long)s.a_start + delta1, (long)s.a_len, (long)s.b_start, (long)s.b_len);
  }

  // Refine: diff the two sides of each conflict; what they agree on leaves it.
  std::vector<MergeRegion> refined;
  for (const auto& m : c) {
    if (m.mode != 0 || m.chg1 == 0 || m.chg2 == 0) { refined.push_back(m); continue; }
    std::vector<int> a(io.begin() + m.i1, io.begin() + m.i1 + m.chg1);
    std::vector<int> b(it.begin() + m.i2, it.begin() + m.i2 + m.chg2);
    auto sub = diff_ids(a, b, opt.algorithm);
    if (sub.empty()) { MergeRegion same = m; same.mode = 4; refined.push_back(same); continue; }
    for (const auto& e : sub)
      refined.push_back(MergeRegion{0, m.i0, m.chg0, m.i1 + (long)e.a_start, (long)e.a_len, m.i2 + (long)e.b_start, (long)e.b_len});
  }

  // Join conflicts close enough together that splitting them only confuses.
  std::vector<MergeRegion> joined;
  for (const auto& m : refined) {
    if (!joined.empty()) {
      MergeRegion& prev = joined.back();
      long begin = prev.i1 + prev.chg1, end = m.i1;
      if (prev.mode == 0 && m.mode == 0 &&
          !(end - begin > 3 && (!opt.join_non_alnum || lines_contain_alnum(ours, begin, end - begin)))) {
        prev.chg0 = m.i0 + m.chg0 - prev.i0;
        prev.chg1 = m.i1 + m.chg1 - prev.i1;
        prev.chg2 = m.i2 + m.chg2 - prev.i2;
        continue;
      }
    }
    joined.push_back(m);
  }

  // Write it out, in ours' coordinates.
  std::string& out = res.merged;
  const int favor_mode = opt.favor == GitMergeFavor::Ours ? 1 : opt.favor == GitMergeFavor::Theirs ? 2
                         : opt.favor == GitMergeFavor::Union ? 3 : 0;
  const int ms = opt.marker_size > 0 ? opt.marker_size : 7;
  long i = 0;
  for (auto m : joined) {
    if (m.mode == 0 && favor_mode) m.mode = favor_mode;
    if (m.mode == 0) {
      append_lines(out, ours, (size_t)i, (size_t)(m.i1 - i), false);
      out += std::string((size_t)ms, '<') + (opt.ours_label.empty() ? std::string() : " " + opt.ours_label) + "\n";
      std::string o, t;
      append_lines(o, ours, (size_t)m.i1, (size_t)m.chg1, false);
      append_lines(t, theirs, (size_t)m.i2, (size_t)m.chg2, false);
      append_lines(out, ours, (size_t)m.i1, (size_t)m.chg1, true);
      out += std::string((size_t)ms, '=') + "\n";
      append_lines(out, theirs, (size_t)m.i2, (size_t)m.chg2, true);
      out += std::string((size_t)ms, '>') + (opt.theirs_label.empty() ? std::string() : " " + opt.theirs_label) + "\n";
      res.conflicts.push_back(GitMergeConflict{(size_t)m.i1, std::move(o), std::move(t)});
    } else if (m.mode & 3) {
      append_lines(out, ours, (size_t)i, (size_t)(m.i1 - i), false);
      if (m.mode & 1) append_lines(out, ours, (size_t)m.i1, (size_t)m.chg1, (m.mode & 2) != 0);
      if (m.mode & 2) append_lines(out, theirs, (size_t)m.i2, (size_t)m.chg2, false);
    } else {
      continue;
    }
    i = m.i1 + m.chg1;
  }
  append_lines(out, ours, (size_t)i, ours.size() - (size_t)i, false);
  res.clean = res.conflicts.empty();
  return res;
}

// -- history ------------------------------------------------------------------------

namespace {

struct CommitInfo { std::vector<std::string> parents; uint64_t time = 0; };

// A commit's parents and date, remembered. An id names content, so an entry can never go
// stale; a merge of a criss-cross history asks about the same commits thousands of times.
Result<CommitInfo> commit_info(const Git& repo, const std::string& oid) {
  static hal::Mutex mu;
  static std::unordered_map<std::string, CommitInfo> cache;
  {
    hal::LockGuard lk(mu);
    auto it = cache.find(oid);
    if (it != cache.end()) return it->second;
  }
  CommitInfo info;
  std::string tree;
  // a block file answers from its commit columns, nothing inflated
  if (!repo.commit_meta(oid, tree, info.parents, info.time)) {
    auto c = repo.read_commit(oid);
    if (!c.ok()) return Result<CommitInfo>(c.error());
    info = CommitInfo{c->parents, c->committer_time};
  }
  hal::LockGuard lk(mu);
  if (cache.size() > 2000000) cache.clear();
  cache[oid] = info;
  return info;
}

enum : uint8_t { kParent1 = 1, kParent2 = 2, kStale = 4, kResult = 8 };

// git's paint_down_to_common: walk both histories together, newest commit first, colouring
// each commit with the side(s) it is reachable from. A commit reached from both is a
// candidate; everything below a candidate is stale, and the walk stops as soon as only stale
// commits are left to look at - so it reads the part of history where the two sides differ,
// not the whole of it.
Result<std::vector<std::string>> paint_down_to_common(const Git& repo, const std::string& one, const std::string& two,
                                                      std::unordered_map<std::string, uint8_t>& flags) {
  using R = Result<std::vector<std::string>>;
  struct Item { uint64_t time; uint64_t seq; std::string oid; };
  auto later = [](const Item& x, const Item& y) { return x.time != y.time ? x.time < y.time : x.seq > y.seq; };
  std::vector<Item> heap;
  uint64_t seq = 0;
  auto push = [&](const std::string& oid) -> Result<void> {
    auto ci = commit_info(repo, oid);
    if (!ci.ok()) return Result<void>(ci.error());
    heap.push_back(Item{ci->time, seq++, oid});
    std::push_heap(heap.begin(), heap.end(), later);
    return Ok();
  };
  flags[one] |= kParent1;
  if (auto r = push(one); !r.ok()) return R(r.error());
  flags[two] |= kParent2;
  if (auto r = push(two); !r.ok()) return R(r.error());

  std::vector<std::string> results;
  auto has_nonstale = [&]() {
    for (auto& it : heap) if (!(flags[it.oid] & kStale)) return true;
    return false;
  };
  while (has_nonstale()) {
    std::pop_heap(heap.begin(), heap.end(), later);
    std::string c = std::move(heap.back().oid);
    heap.pop_back();
    uint8_t f = flags[c] & (kParent1 | kParent2 | kStale);
    if (f == (kParent1 | kParent2)) {
      if (!(flags[c] & kResult)) { flags[c] |= kResult; results.push_back(c); }
      f |= kStale;
    }
    auto ci = commit_info(repo, c);
    if (!ci.ok()) return R(ci.error());
    for (auto& p : ci->parents) {
      uint8_t& pf = flags[p];
      if ((pf & f) == f) continue;
      pf |= f;
      if (auto r = push(p); !r.ok()) return R(r.error());
    }
  }
  return results;
}

}  // namespace

Result<bool> git_is_ancestor(const Git& repo, const std::string& ancestor, const std::string& commit) {
  if (ancestor == commit) return true;
  std::unordered_map<std::string, uint8_t> flags;
  auto r = paint_down_to_common(repo, ancestor, commit, flags);
  if (!r.ok()) return Result<bool>(r.error());
  return (flags[ancestor] & kParent2) != 0;
}

Result<std::vector<std::string>> git_merge_bases(const Git& repo, const std::string& a, const std::string& b) {
  using R = Result<std::vector<std::string>>;
  if (a == b) {
    auto ci = commit_info(repo, a);
    if (!ci.ok()) return R(ci.error());
    return std::vector<std::string>{a};
  }
  std::unordered_map<std::string, uint8_t> flags;
  auto painted = paint_down_to_common(repo, a, b, flags);
  if (!painted.ok()) return R(painted.error());
  std::vector<std::string> candidates;
  for (auto& c : *painted) if (!(flags[c] & kStale)) candidates.push_back(c);
  if (candidates.size() > 1) {
    // Dates can lie, so the walk may leave a candidate that another candidate descends from.
    // A common ancestor reachable from another common ancestor is not a best one.
    std::unordered_set<std::string> below;
    std::deque<std::string> q;
    for (auto& c : candidates) {
      auto ci = commit_info(repo, c);
      if (!ci.ok()) return R(ci.error());
      for (auto& p : ci->parents) q.push_back(p);
    }
    while (!q.empty()) {
      std::string c = std::move(q.front());
      q.pop_front();
      if (!below.insert(c).second) continue;
      auto ci = commit_info(repo, c);
      if (!ci.ok()) return R(ci.error());
      for (auto& p : ci->parents) if (!below.count(p)) q.push_back(p);
    }
    std::vector<std::string> best;
    for (auto& c : candidates) if (!below.count(c)) best.push_back(c);
    candidates = std::move(best);
  }
  std::sort(candidates.begin(), candidates.end());
  return candidates;
}

// -- similarity and renames --------------------------------------------------------
//
// diffcore-delta's measure: both files are cut into spans (a line, or 64 bytes
// of one), each span hashed; what dst shares with src is the byte count of the
// spans they have in common. Score = shared / larger size, on git's 0..60000
// scale, so the percentages here are the ones `git diff -M` prints.

namespace {

constexpr int kMaxScore = 60000;

using SpanCounts = GitSpanCounts;

// Whether git treats the bytes as binary: a NUL among the first 8000 (buffer_is_binary's FIRST_FEW_BYTES).
bool binary_for_diffcore(std::string_view s) {
  return std::memchr(s.data(), 0, std::min<size_t>(s.size(), 8000)) != nullptr;
}

// diffcore-delta.c's hash_chars: spans of at most 64 bytes ending at a newline, counted by hash, sorted by hash.
// Collected into a vector and merged after one sort: a hash map per file was most of a rename search's time.
SpanCounts hash_spans(std::string_view buf, bool is_text) {
  std::vector<std::pair<uint32_t, uint32_t>> raw;
  raw.reserve(buf.size() / 24 + 4);
  uint32_t accum1 = 0, accum2 = 0;
  uint32_t n = 0;
  const size_t sz = buf.size();
  for (size_t k = 0; k < sz; ++k) {
    unsigned int c = (unsigned char)buf[k];
    uint32_t old_1 = accum1;
    if (is_text && c == '\r' && k + 1 < sz && buf[k + 1] == '\n') continue;   // CR of CRLF is not content
    accum1 = (accum1 << 7) ^ (accum2 >> 25);
    accum2 = (accum2 << 7) ^ (old_1 >> 25);
    accum1 += c;
    if (++n < 64 && c != '\n') continue;
    raw.emplace_back((accum1 + accum2 * 0x61) % 107927, n);
    n = 0;
    accum1 = accum2 = 0;
  }
  if (n > 0) raw.emplace_back((accum1 + accum2 * 0x61) % 107927, n);
  std::sort(raw.begin(), raw.end(), [](const std::pair<uint32_t, uint32_t>& x, const std::pair<uint32_t, uint32_t>& y) { return x.first < y.first; });
  SpanCounts out;
  out.size = sz;
  for (auto& [h, cnt] : raw) {
    if (!out.spans.empty() && out.spans.back().first == h) out.spans.back().second += cnt;
    else out.spans.emplace_back(h, cnt);
  }
  return out;
}

uint64_t src_copied(const SpanCounts& s, const SpanCounts& d) {
  uint64_t sc = 0;
  size_t i = 0, j = 0;
  while (i < s.spans.size()) {
    while (j < d.spans.size() && d.spans[j].first < s.spans[i].first) ++j;
    uint32_t src_cnt = s.spans[i].second, dst_cnt = 0;
    if (j < d.spans.size() && d.spans[j].first == s.spans[i].first) dst_cnt = d.spans[j++].second;
    sc += src_cnt < dst_cnt ? src_cnt : dst_cnt;
    ++i;
  }
  return sc;
}

// The sizes alone rule a match out (diffcore-rename's quick reject).
bool sizes_reject(uint64_t a, uint64_t b, int minimum_score) {
  const uint64_t max_size = std::max(a, b), base_size = std::min(a, b);
  return max_size * (uint64_t)(kMaxScore - minimum_score) < (max_size - base_size) * (uint64_t)kMaxScore;
}

int score_of(const SpanCounts& a, const SpanCounts& b) {
  const uint64_t max_size = std::max(a.size, b.size);
  if (max_size == 0) return kMaxScore;
  return (int)(src_copied(a, b) * (uint64_t)kMaxScore / max_size);
}

// Score on git's scale, 0 when the sizes alone rule a match out. Each side is hashed as git hashes it: text or
// binary by its own bytes.
int similarity_score(std::string_view a, std::string_view b, int minimum_score) {
  if (sizes_reject(a.size(), b.size(), minimum_score)) return 0;
  if (std::max(a.size(), b.size()) == 0) return kMaxScore;
  return score_of(hash_spans(a, !binary_for_diffcore(a)), hash_spans(b, !binary_for_diffcore(b)));
}

std::string basename_of(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string str_of_blob(const Git& repo, const std::string& oid, Result<void>* err) {
  auto o = repo.read_object(oid);
  if (!o.ok()) { *err = Result<void>(o.error()); return {}; }
  return str_of(o->payload);
}

struct Side { bool present = false; std::string oid; uint32_t mode = 0; };
bool same(const Side& x, const Side& y) {
  if (!x.present || !y.present) return x.present == y.present;
  return x.oid == y.oid && x.mode == y.mode;
}
uint32_t kind_of(uint32_t mode) { return mode & 0170000; }   // regular, symlink, gitlink
bool is_regular(uint32_t mode) { return kind_of(mode) == 0100000; }

using Flat = std::map<std::string, Side>;

Result<Flat> flat(const Git& repo, const std::string& tree) {
  Flat out;
  if (tree.empty()) return out;
  std::vector<GitIndexEntry> ents;
  if (auto r = repo.flatten_tree(tree, "", ents); !r.ok()) return Result<Flat>(r.error());
  for (auto& e : ents) out[e.path] = Side{true, e.oid, e.mode};
  return out;
}

Result<std::string> blob_text(const Git& repo, const std::string& oid) {
  auto o = repo.read_object(oid);
  if (!o.ok()) return Result<std::string>(o.error());
  if (o->type != "blob") return Result<std::string>(Status::Corrupt, oid + " is a " + o->type + ", not a blob");
  return str_of(o->payload);
}

// Paths that left `before` and paths that arrived in `after`, paired the way
// diffcore-rename pairs them.
// `relevant`, when given, says which vanished paths may be an inexact rename's source. A merge
// passes it: merge-ort only looks for where a file went if the other side changed that file
// (otherwise the answer cannot change the result), and that changes which pairs are made.
Result<std::vector<GitRename>> find_renames(const Git& repo, const Flat& before, const Flat& after, int threshold_percent,
                                            const std::function<bool(const std::string&)>* relevant = nullptr) {
  using R = Result<std::vector<GitRename>>;
  std::vector<std::string> gone, arrived;
  for (auto& [p, s] : before) if (!after.count(p)) gone.push_back(p);
  for (auto& [p, s] : after) if (!before.count(p)) arrived.push_back(p);
  std::vector<GitRename> out;
  if (gone.empty() || arrived.empty()) return out;
  std::vector<char> src_used(gone.size(), 0), dst_used(arrived.size(), 0);

  // Identical content first. Among several identical sources, one with the
  // same basename wins, then the first by path.
  for (size_t d = 0; d < arrived.size(); ++d) {
    const Side& ds = after.at(arrived[d]);
    long best = -1;
    for (size_t s = 0; s < gone.size(); ++s) {
      if (src_used[s]) continue;
      const Side& ss = before.at(gone[s]);
      if (ss.oid != ds.oid || kind_of(ss.mode) != kind_of(ds.mode)) continue;
      if (best < 0 || (basename_of(gone[s]) == basename_of(arrived[d]) && basename_of(gone[(size_t)best]) != basename_of(arrived[d])))
        best = (long)s;
    }
    if (best >= 0) {
      src_used[(size_t)best] = dst_used[d] = 1;
      out.push_back(GitRename{gone[(size_t)best], arrived[d], 100});
    }
  }

  // Then by similarity, regular files only, best score first.
  const int minimum = threshold_percent * kMaxScore / 100;
  struct Cand { int score; int name; size_t dst, src; };
  std::vector<Cand> cands;
  std::unordered_map<std::string, std::string> text;
  std::unordered_map<std::string, SpanCounts> spans;   // each blob hashed once for the whole matrix, as git does
  auto content = [&](const Side& side, Result<void>* err) -> const std::string& {
    auto it = text.find(side.oid);
    if (it == text.end()) it = text.emplace(side.oid, str_of_blob(repo, side.oid, err)).first;
    return it->second;
  };
  auto counts = [&](const Side& side, const std::string& t) -> const SpanCounts& {
    auto it = spans.find(side.oid);
    if (it == spans.end()) it = spans.emplace(side.oid, hash_spans(t, !binary_for_diffcore(t))).first;
    return it->second;
  };
  for (size_t d = 0; d < arrived.size(); ++d) {
    if (dst_used[d]) continue;
    const Side& ds = after.at(arrived[d]);
    if (!is_regular(ds.mode)) continue;
    for (size_t s = 0; s < gone.size(); ++s) {
      if (src_used[s]) continue;
      if (relevant && !(*relevant)(gone[s])) continue;
      const Side& ss = before.at(gone[s]);
      if (!is_regular(ss.mode)) continue;
      Result<void> err = Ok();
      const std::string& a = content(ss, &err);
      const std::string& b = content(ds, &err);
      if (!err.ok()) return R(err.error());
      if (sizes_reject(a.size(), b.size(), minimum)) continue;
      int score = std::max(a.size(), b.size()) == 0 ? kMaxScore : score_of(counts(ss, a), counts(ds, b));
      if (score < minimum) continue;
      cands.push_back(Cand{score, basename_of(gone[s]) == basename_of(arrived[d]) ? 1 : 0, d, s});
    }
  }
  std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
    if (x.score != y.score) return x.score > y.score;
    return x.name > y.name;
  });
  for (auto& c : cands) {
    if (dst_used[c.dst] || src_used[c.src]) continue;
    dst_used[c.dst] = src_used[c.src] = 1;
    out.push_back(GitRename{gone[c.src], arrived[c.dst], c.score * 100 / kMaxScore});
  }
  return out;
}

}  // namespace

int git_similarity(std::string_view a, std::string_view b) {
  return similarity_score(a, b, 0) * 100 / kMaxScore;
}

GitSpanCounts git_span_counts(std::string_view text) { return hash_spans(text, !binary_for_diffcore(text)); }

int git_similarity(const GitSpanCounts& a, const GitSpanCounts& b) { return score_of(a, b) * 100 / kMaxScore; }

int git_similarity_score(const GitSpanCounts& a, const GitSpanCounts& b) { return score_of(a, b); }

Result<std::vector<GitRename>> git_detect_renames(const Git& repo, const std::string& before_tree,
                                                  const std::string& after_tree, int threshold_percent) {
  auto b = flat(repo, before_tree);
  if (!b.ok()) return Result<std::vector<GitRename>>(b.error());
  auto a = flat(repo, after_tree);
  if (!a.ok()) return Result<std::vector<GitRename>>(a.error());
  return find_renames(repo, *b, *a, threshold_percent);
}

// -- trees --------------------------------------------------------------------------

Result<GitTreeMerge> git_merge_trees(Git& repo, const std::string& base_tree,
                                     const std::string& ours_tree, const std::string& theirs_tree,
                                     const GitTreeMergeOptions& opt) {
  using R = Result<GitTreeMerge>;
  auto b = flat(repo, base_tree);
  if (!b.ok()) return R(b.error());
  auto o = flat(repo, ours_tree);
  if (!o.ok()) return R(o.error());
  auto t = flat(repo, theirs_tree);
  if (!t.ok()) return R(t.error());

  GitTreeMerge res;
  const bool virtual_base = opt.call_depth > 0;
  std::map<std::string, std::string> ren_o, ren_t;   // base path -> new path
  if (opt.detect_renames) {
    // A file that left one side is worth tracing only if the other side did something to it.
    auto changed_on = [&](const Flat& other) {
      return std::function<bool(const std::string&)>([&](const std::string& path) {
        auto it = other.find(path);
        return it == other.end() || !same(it->second, b->at(path));
      });
    };
    auto rel_o = changed_on(*t), rel_t = changed_on(*o);
    auto ro = find_renames(repo, *b, *o, opt.rename_threshold, &rel_o);
    if (!ro.ok()) return R(ro.error());
    auto rt = find_renames(repo, *b, *t, opt.rename_threshold, &rel_t);
    if (!rt.ok()) return R(rt.error());
    res.renames_ours = std::move(*ro);
    res.renames_theirs = std::move(*rt);
    for (auto& r : res.renames_ours) ren_o[r.from] = r.to;
    for (auto& r : res.renames_theirs) ren_t[r.from] = r.to;
  }
  std::set<std::string> dest_o, dest_t;   // rename targets, which are not "added" files
  for (auto& [f, to] : ren_o) dest_o.insert(to);
  for (auto& [f, to] : ren_t) dest_t.insert(to);

  // One unit of merging: a file as base, ours and theirs have it, wherever each
  // keeps it, and the path it lands at.
  struct Unit { std::string dest, ours_path, theirs_path; Side sb, so, st; };
  std::vector<Unit> units;
  std::map<std::string, std::vector<GitPathConflict>> conflicts_at;   // dest -> conflicts
  std::map<std::string, GitIndexEntry> result;                         // dest -> entry
  auto conflict = [&](const std::string& path, const char* kind, GitMerge3 file = {}) {
    res.clean = false;
    res.conflicts.push_back(GitPathConflict{path, kind, std::move(file)});
  };

  for (auto& [p, sb] : *b) {
    Unit u;
    u.sb = sb;
    if (o->count(p)) { u.ours_path = p; u.so = o->at(p); }
    else if (ren_o.count(p)) { u.ours_path = ren_o[p]; u.so = o->at(u.ours_path); }
    if (t->count(p)) { u.theirs_path = p; u.st = t->at(p); }
    else if (ren_t.count(p)) { u.theirs_path = ren_t[p]; u.st = t->at(u.theirs_path); }
    const bool o_ren = u.so.present && u.ours_path != p, t_ren = u.st.present && u.theirs_path != p;
    if (o_ren && t_ren && u.ours_path != u.theirs_path) {
      // Renamed to two different names: both stay, and the old name is part
      // of the conflict too (git lists all three).
      conflict(p, "rename/rename");
      conflict(u.ours_path, "rename/rename");
      conflict(u.theirs_path, "rename/rename");
      result[u.ours_path] = GitIndexEntry{u.ours_path, u.so.oid, u.so.mode};
      if (!virtual_base) result[u.theirs_path] = GitIndexEntry{u.theirs_path, u.st.oid, u.st.mode};
      continue;
    }
    if ((o_ren && !u.st.present) || (t_ren && !u.so.present)) {
      // Renamed on one side, deleted on the other.
      const std::string& to = o_ren ? u.ours_path : u.theirs_path;
      const Side& kept = o_ren ? u.so : u.st;
      conflict(to, "rename/delete");
      if (virtual_base) result[p] = GitIndexEntry{p, sb.oid, sb.mode};
      else result[to] = GitIndexEntry{to, kept.oid, kept.mode};
      continue;
    }
    u.dest = o_ren ? u.ours_path : t_ren ? u.theirs_path : p;
    units.push_back(std::move(u));
  }
  // Files new on a side (and not a rename's target), and files new on both.
  std::set<std::string> added;
  for (auto& [p, s] : *o) if (!b->count(p) && !dest_o.count(p)) added.insert(p);
  for (auto& [p, s] : *t) if (!b->count(p) && !dest_t.count(p)) added.insert(p);
  for (auto& p : added) {
    Unit u;
    u.dest = p;
    if (o->count(p) && !dest_o.count(p)) { u.ours_path = p; u.so = o->at(p); }
    if (t->count(p) && !dest_t.count(p)) { u.theirs_path = p; u.st = t->at(p); }
    units.push_back(std::move(u));
  }

  // Two units landing on one path: a rename onto a file the other side added,
  // or two files renamed to the same name.
  std::map<std::string, int> landing;
  for (auto& u : units) {
    bool lands = u.so.present || u.st.present;
    if (lands) ++landing[u.dest];
  }

  // What one unit comes to at path p: written into `into`, conflicts recorded as they are found.
  auto process = [&](const Unit& u, const std::string& p, std::map<std::string, GitIndexEntry>& into) -> Result<void> {
      const Side &sb = u.sb, &so = u.so, &st = u.st;
      auto keep = [&](const Side& s) { if (s.present) into[p] = GitIndexEntry{p, s.oid, s.mode}; };
      if (same(so, st)) { keep(so); return Ok(); }
      if (same(so, sb)) { keep(st); return Ok(); }
      if (same(st, sb)) { keep(so); return Ok(); }
      if (!so.present || !st.present) {
        conflict(p, "modify/delete");
        keep(virtual_base ? sb : so);
        return Ok();
      }
      if (kind_of(so.mode) != kind_of(st.mode)) { conflict(p, "type"); keep(virtual_base && sb.present ? sb : so); return Ok(); }

      uint32_t mode;
      bool mode_conflict = false;
      if (so.mode == st.mode) mode = so.mode;
      else if (sb.present && so.mode == sb.mode) mode = st.mode;
      else if (sb.present && st.mode == sb.mode) mode = so.mode;
      else { mode = so.mode; mode_conflict = true; }

      if (so.oid == st.oid) {
        if (mode_conflict) conflict(p, "mode");
        into[p] = GitIndexEntry{p, so.oid, mode};
        return Ok();
      }
      if (!is_regular(mode)) {
        // A symbolic link both sides repointed has no lines to merge, but a favor still says
        // whose target wins - merge-ort takes that side cleanly (not while merging merge bases).
        if (kind_of(mode) == 0120000 && !virtual_base &&
            (opt.favor == GitMergeFavor::Ours || opt.favor == GitMergeFavor::Theirs)) {
          into[p] = GitIndexEntry{p, opt.favor == GitMergeFavor::Ours ? so.oid : st.oid, mode};
          return Ok();
        }
        conflict(p, "content");
        keep(virtual_base && sb.present ? sb : so);
        return Ok();
      }

      std::string base_text;
      if (sb.present) {
        auto bt = blob_text(repo, sb.oid);
        if (!bt.ok()) return Result<void>(bt.error());
        base_text = std::move(*bt);
      }
      auto ot = blob_text(repo, so.oid);
      if (!ot.ok()) return Result<void>(ot.error());
      auto tt = blob_text(repo, st.oid);
      if (!tt.ok()) return Result<void>(tt.error());

      GitMerge3 m;
      std::optional<GitMerge3> driven;
      if (opt.driver) driven = opt.driver(p, base_text, *ot, *tt);
      if (driven) {
        m = std::move(*driven);
      } else {
        GitMerge3Options mo;
        mo.join_non_alnum = false;
      mo.algorithm = GitDiffAlgorithm::Histogram;   // merge-ort merges content with the histogram diff
        mo.favor = opt.favor;
        mo.ours_label = opt.ours_label;
        mo.theirs_label = opt.theirs_label;
        mo.marker_size = 7 + 2 * opt.call_depth;
        m = git_merge3(base_text, *ot, *tt, mo);
      }
      if (!m.clean && !m.binary && opt.structural && !virtual_base) {
        // the lines conflict: the syntax trees may not (two functions added at one place, a member each...)
        if (auto sm = git_structural_merge(p, base_text, *ot, *tt)) m = std::move(*sm);
      } else if (m.clean && !m.binary && opt.structural && !virtual_base) {
        // the lines merged: each unit must still be what the sides made of it (two sides adding one function merge
        // cleanly by lines into a duplicate definition). The syntax trees then decide, and failing that, a conflict.
        auto audit = git_structural_audit(p, base_text, *ot, *tt, m.merged);
        if (!audit.ok) {
          if (auto sm = git_structural_merge(p, base_text, *ot, *tt); sm && sm->clean) {
            m = std::move(*sm);
          } else {
            const std::string mk(7 + 2 * opt.call_depth, '<'), eq(7 + 2 * opt.call_depth, '='), gt(7 + 2 * opt.call_depth, '>');
            auto nl = [](std::string s) { if (!s.empty() && s.back() != '\n') s += '\n'; return s; };
            m.clean = false;
            m.merged = mk + " " + opt.ours_label + " (" + audit.reason + ")\n" + nl(std::string(*ot)) + eq + "\n" +
                       nl(std::string(*tt)) + gt + " " + opt.theirs_label + "\n";
            m.conflicts.push_back(GitMergeConflict{0, std::string(*ot), std::string(*tt)});
          }
        }
      }
      if (m.binary && !m.clean) {
        // A virtual base keeps the base's bytes for a binary it cannot merge.
        if (virtual_base) keep(sb.present ? sb : so); else keep(so);
        conflict(p, "binary", std::move(m));
        return Ok();
      }
      auto oid = repo.write_object("blob", span_of(m.merged));
      if (!oid.ok()) return Result<void>(oid.error());
      if (!m.clean) {
        // The marked-up file is what a virtual base carries (git's virtual
        // ancestor holds the conflict markers); a real merge keeps ours.
        if (virtual_base) into[p] = GitIndexEntry{p, *oid, mode}; else keep(so);
        conflict(p, sb.present ? "content" : "add/add", std::move(m));
        return Ok();
      }
      if (mode_conflict) conflict(p, "mode");
      into[p] = GitIndexEntry{p, *oid, mode};
      res.auto_merged.push_back(p);
    return Ok();
  };

  std::map<std::string, std::vector<const Unit*>> colliding;
  for (auto& u : units) {
    if (landing[u.dest] > 1) { if (u.so.present || u.st.present) colliding[u.dest].push_back(&u); continue; }
    if (auto r = process(u, u.dest, result); !r.ok()) return R(r.error());
  }
  // Two files landing on one path - a rename onto a file the other side added, or two files
  // renamed to the same name. merge-ort settles each one on its own first, then merges the two
  // results as it would two additions: identical is clean, otherwise a two-way content merge.
  for (auto& [p, us] : colliding) {
    Unit v;
    v.dest = p;
    bool placed = us.size() == 2;
    for (const Unit* u : us) {
      if (!placed) break;
      std::map<std::string, GitIndexEntry> one;
      if (auto r = process(*u, p, one); !r.ok()) return R(r.error());
      Side got;
      if (auto it = one.find(p); it != one.end()) { got.present = true; got.oid = it->second.oid; got.mode = it->second.mode; }
      const bool ours_side = u->so.present && u->ours_path == p;
      Side& slot = ours_side ? v.so : v.st;
      if (slot.present) { placed = false; break; }
      slot = got;
      (ours_side ? v.ours_path : v.theirs_path) = p;
    }
    if (!placed) {
      conflict(p, "rename/add");
      const Unit* u = us.front();
      Side keep = u->so.present ? u->so : u->st;
      result[p] = GitIndexEntry{p, keep.oid, keep.mode};
      continue;
    }
    if (auto r = process(v, p, result); !r.ok()) return R(r.error());
  }

  for (auto& [p, e] : result) res.entries.push_back(e);

  // A path cannot be a file and a directory at once.
  std::unordered_set<std::string> files;
  for (auto& e : res.entries) files.insert(e.path);
  std::set<std::string> df;
  for (auto& e : res.entries)
    for (size_t s = e.path.find('/'); s != std::string::npos; s = e.path.find('/', s + 1))
      if (files.count(e.path.substr(0, s))) df.insert(e.path.substr(0, s));
  for (auto& p : df) conflict(p, "file/directory");

  std::sort(res.conflicts.begin(), res.conflicts.end(),
            [](const GitPathConflict& x, const GitPathConflict& y) { return x.path < y.path; });
  // A virtual base is always written: it is an input to the next merge, conflicts and all.
  if (res.clean || virtual_base) {
    auto tree = repo.write_tree_from_entries(res.entries);
    if (!tree.ok()) return R(tree.error());
    res.tree = *tree;
  }
  return res;
}

namespace {

// merge-ort's recursion: with several merge bases, merge them pairwise (oldest
// first) into a virtual commit, and use its tree as the base.
Result<std::string> merge_base_tree(Git& repo, const std::string& a, const std::string& b,
                                    const GitTreeMergeOptions& opt, int depth, std::string* base_out) {
  using R = Result<std::string>;
  auto bases = git_merge_bases(repo, a, b);
  if (!bases.ok()) return R(bases.error());
  if (bases->empty()) return std::string();
  // Oldest first, as merge-ort pops them after reversing git's newest-first list.
  std::vector<std::pair<uint64_t, std::string>> by_time;
  for (auto& c : *bases) {
    auto cm = repo.read_commit(c);
    if (!cm.ok()) return R(cm.error());
    by_time.emplace_back(cm->committer_time, c);
  }
  std::stable_sort(by_time.begin(), by_time.end(),
                   [](const auto& x, const auto& y) { return x.first < y.first; });
  std::string merged = by_time[0].second;
  if (base_out) *base_out = merged;
  for (size_t k = 1; k < by_time.size(); ++k) {
    const std::string& next = by_time[k].second;
    std::string inner_base;
    auto inner_tree = merge_base_tree(repo, merged, next, opt, depth + 1, &inner_base);
    if (!inner_tree.ok()) return R(inner_tree.error());
    auto mc = repo.read_commit(merged);
    if (!mc.ok()) return R(mc.error());
    auto nc = repo.read_commit(next);
    if (!nc.ok()) return R(nc.error());
    GitTreeMergeOptions inner = opt;
    inner.call_depth = depth + 1;
    inner.favor = GitMergeFavor::None;
    inner.ours_label = "Temporary merge branch 1";
    inner.theirs_label = "Temporary merge branch 2";
    auto tm = git_merge_trees(repo, *inner_tree, mc->tree, nc->tree, inner);
    if (!tm.ok()) return R(tm.error());
    // The virtual commit only has to exist long enough to be merged against.
    auto vc = repo.commit_tree(tm->tree, {merged, next}, "merged common ancestors",
                               "aios <kernel@aios>", "aios <kernel@aios>", std::max(mc->committer_time, nc->committer_time));
    if (!vc.ok()) return R(vc.error());
    merged = *vc;
  }
  auto mc = repo.read_commit(merged);
  if (!mc.ok()) return R(mc.error());
  if (base_out) *base_out = merged;
  return mc->tree;
}

}  // namespace

Result<GitCommitMerge> git_merge_commits(Git& repo, const std::string& ours, const std::string& theirs,
                                         const std::string& message, bool allow_fast_forward,
                                         const GitTreeMergeOptions& opt) {
  using R = Result<GitCommitMerge>;
  GitCommitMerge res;
  if (ours == theirs) { res.kind = GitCommitMerge::Kind::UpToDate; res.commit = ours; return res; }
  auto up = git_is_ancestor(repo, theirs, ours);
  if (!up.ok()) return R(up.error());
  if (*up) { res.kind = GitCommitMerge::Kind::UpToDate; res.commit = ours; res.base = theirs; return res; }
  auto ff = git_is_ancestor(repo, ours, theirs);
  if (!ff.ok()) return R(ff.error());
  if (*ff && allow_fast_forward) { res.kind = GitCommitMerge::Kind::FastForward; res.commit = theirs; res.base = ours; return res; }

  auto base_tree = merge_base_tree(repo, ours, theirs, opt, 0, &res.base);
  if (!base_tree.ok()) return R(base_tree.error());
  auto oc = repo.read_commit(ours);
  if (!oc.ok()) return R(oc.error());
  auto tc = repo.read_commit(theirs);
  if (!tc.ok()) return R(tc.error());
  auto tm = git_merge_trees(repo, *base_tree, oc->tree, tc->tree, opt);
  if (!tm.ok()) return R(tm.error());
  res.tree = std::move(*tm);
  if (!res.tree.clean) { res.kind = GitCommitMerge::Kind::Conflicted; return res; }
  auto commit = repo.commit_tree(res.tree.tree, {ours, theirs}, message);
  if (!commit.ok()) return R(commit.error());
  res.kind = GitCommitMerge::Kind::Merged;
  res.commit = *commit;
  return res;
}

}  // namespace aios::userland
