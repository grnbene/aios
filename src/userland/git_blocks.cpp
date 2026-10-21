// Block storage (see git_blocks.hpp): building, opening and reading block files.
//
// File layout (little-endian):
//   "AIOSBLK1", u32 version, u32 objects, u32 chunks, u32 frames, u32 externals, u32 commits, u32 sections
//   sections, each u64 length then its bytes:
//     0 ids        objects x 20, sorted
//     1 fanout     256 x u32: how many ids start with a byte <= i
//     2 kinds      objects x u8 (1 commit, 2 tree, 3 blob, 4 tag)
//     3 loc        objects x (u32, u32, u32): commit/tree/tag (frame, offset, length) of its bytes in the frames;
//                  blob (first entry in chunk lists, chunk count, size)
//     4 rows       objects x u32: a commit's row in the commit columns
//     5 commits    rows x (u32 tree ref, u32 first parent, u32 parent count, u32 0, u64 commit time)
//     6 parents    u32 refs
//     7 chunklists u32 chunk numbers
//     8 chunks     chunks x (u32 frame, u32 offset, u32 length)
//     9 frames     frames x (u64 offset into the frame data, u32 compressed, u32 uncompressed length)
//    10 externals  ids this file refers to without holding (a submodule's commit, a blob a partial clone lacks)
//    11 modes      zlib of the tree modes, each followed by a NUL
//    12 names      zlib of the tree entry names, each followed by a NUL
//    13 syntax     zlib of: varint count, then for each source blob its object number, a varint length and its
//                  outline (git_syntax_outline_encode: every named unit's scope, place and digests). Files written
//                  before this section existed have 13 sections and no outlines.
//   frame data (zlib streams), then the SHA-1 of everything before it.
// A ref is an object number, or (objects + n) for external n. A tree's bytes in a frame are varints: its entry count,
// then for each entry the mode's number, the name's number and the ref.
#include "aios/userland/git_blocks.hpp"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/git_remote.hpp"
#include "aios/userland/git_structural.hpp"
#if defined(AIOS_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace aios::userland {

namespace {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kSections = 14;
constexpr size_t kMaxOutlined = 1u << 20;   // bigger source files are not outlined ahead of time
constexpr size_t kTextFrame = 48 * 1024;    // small enough that reading one object inflates little more than it
constexpr size_t kChunkFrame = 96 * 1024;
constexpr size_t kChunkMin = 2 * 1024, kChunkMax = 64 * 1024, kChunkWhole = 16 * 1024;
constexpr uint64_t kChunkMask = (1u << 13) - 1;   // ~8 KiB chunks on average

// KBlobDelta: a blob stored as a delta (git's pack delta format) against an earlier-written version of the same path
// in this file; its row is that base's object number, and its loc the delta's (frame, offset, length).
enum Kind : uint8_t { KCommit = 1, KTree = 2, KBlob = 3, KTag = 4, KBlobDelta = 5 };
constexpr int kMaxDeltaDepth = 10;   // reading the newest versions stays cheap: they are whole

ByteBuffer compress(const uint8_t* p, size_t n) {
#if defined(AIOS_HAVE_ZLIB)
  uLongf cap = compressBound((uLong)n);
  ByteBuffer out(cap);
  if (compress2(out.data(), &cap, p, (uLong)n, Z_BEST_COMPRESSION) == Z_OK) { out.resize(cap); return out; }
#endif
  return zlib_deflate(ByteSpan(p, n), 9);
}

void put32(ByteBuffer& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void put64(ByteBuffer& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
void putv(std::string& b, uint64_t v) {
  while (v >= 0x80) { b.push_back((char)(v | 0x80)); v >>= 7; }
  b.push_back((char)v);
}
bool getv(const std::string& b, size_t& at, uint64_t& v) {
  v = 0;
  for (int shift = 0; at < b.size() && shift < 64; shift += 7) {
    uint8_t c = (uint8_t)b[at++];
    v |= (uint64_t)(c & 0x7f) << shift;
    if (!(c & 0x80)) return true;
  }
  return false;
}

std::string hex_of(const uint8_t* p, size_t n = 20) {
  static const char* hx = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += hx[p[i] >> 4]; s += hx[p[i] & 15]; }
  return s;
}
bool raw_of(const std::string& hex, uint8_t out[20]) {
  if (hex.size() != 40) return false;
  for (int i = 0; i < 20; ++i) {
    auto nib = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
    int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

const uint64_t* gear() {
  static uint64_t g[256];
  static bool made = false;
  if (!made) {
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (auto& v : g) {   // splitmix64: any fixed table works, it only has to be the same every time
      x += 0x9e3779b97f4a7c15ull;
      uint64_t z = x;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
      v = z ^ (z >> 31);
    }
    made = true;
  }
  return g;
}

// Content-defined cut points: a rolling gear hash cuts where its low bits are zero (between kChunkMin and kChunkMax).
std::vector<std::pair<size_t, size_t>> chunk_spans(const ByteBuffer& b) {
  std::vector<std::pair<size_t, size_t>> out;
  if (b.size() <= kChunkWhole) { out.push_back({0, b.size()}); return out; }
  const uint64_t* g = gear();
  size_t start = 0;
  uint64_t h = 0;
  for (size_t i = 0; i < b.size(); ++i) {
    h = (h << 1) + g[b[i]];
    size_t len = i + 1 - start;
    if ((len >= kChunkMin && (h & kChunkMask) == 0) || len >= kChunkMax) {
      out.push_back({start, len});
      start = i + 1;
      h = 0;
    }
  }
  if (start < b.size()) out.push_back({start, b.size() - start});
  return out;
}

struct Id20 {
  uint8_t b[20];
  bool operator==(const Id20& o) const { return std::memcmp(b, o.b, 20) == 0; }
};
struct Id20Hash { size_t operator()(const Id20& k) const { size_t h; std::memcpy(&h, k.b, sizeof h); return h; } };
Id20 id_of(const std::string& hex) { Id20 k{}; raw_of(hex, k.b); return k; }
Id20 id_of(const uint8_t* raw) { Id20 k; std::memcpy(k.b, raw, 20); return k; }

struct TreeEntry { std::string mode, name, oid; const uint8_t* raw = nullptr; };
bool parse_tree(const ByteBuffer& p, std::vector<TreeEntry>& out) {
  size_t i = 0;
  while (i < p.size()) {
    size_t sp = i;
    while (sp < p.size() && p[sp] != ' ') ++sp;
    size_t nul = sp;
    while (nul < p.size() && p[nul] != 0) ++nul;
    if (sp >= p.size() || nul + 20 > p.size()) return false;
    TreeEntry e;
    e.mode.assign((const char*)p.data() + i, sp - i);
    e.name.assign((const char*)p.data() + sp + 1, nul - sp - 1);
    e.raw = p.data() + nul + 1;
    out.push_back(std::move(e));
    i = nul + 21;
  }
  return true;
}

// Frames under construction: bytes appended to the current one until it is big enough, then it is compressed.
struct Frames {
  ByteBuffer data;
  std::vector<std::array<uint64_t, 3>> table;   // offset, compressed, uncompressed
  struct Open { std::string bytes; uint32_t index = UINT32_MAX; };
  Open text, chunk;
  uint32_t next_index = 0;
  void seal(Open& o) {
    if (o.index == UINT32_MAX || o.bytes.empty()) return;
    ByteBuffer z = compress((const uint8_t*)o.bytes.data(), o.bytes.size());
    if (table.size() <= o.index) table.resize(o.index + 1);
    table[o.index] = {data.size(), z.size(), o.bytes.size()};
    data.insert(data.end(), z.begin(), z.end());
    o.bytes.clear();
    o.index = UINT32_MAX;
  }
  // Returns (frame, offset).
  std::pair<uint32_t, uint32_t> add(Open& o, const uint8_t* p, size_t n, size_t limit) {
    if (o.index != UINT32_MAX && o.bytes.size() + n > limit && !o.bytes.empty()) seal(o);
    if (o.index == UINT32_MAX) o.index = next_index++;
    uint32_t off = (uint32_t)o.bytes.size();
    o.bytes.append((const char*)p, n);
    return {o.index, off};
  }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------------------------------------------
Result<GitBlockStats> git_build_blocks(Git& repo, const std::vector<std::string>& input, bool outlines) {
  using R = Result<GitBlockStats>;
  GitBlockStats st;
  std::vector<std::string> ids;
  {
    std::unordered_set<std::string> seen;
    for (auto& o : input) if (o.size() == 40 && seen.insert(o).second) ids.push_back(o);
  }
  if (ids.empty()) return R(Status::Invalid, "no objects to store");
  // object numbers: sorted ids
  std::vector<std::string> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  const uint32_t n = (uint32_t)sorted.size();
  std::unordered_map<Id20, uint32_t, Id20Hash> numk;
  numk.reserve(n * 2);
  for (uint32_t i = 0; i < n; ++i) numk[id_of(sorted[i])] = i;
  // an object's number: 0..n-1, or UINT32_MAX when it is not one of the objects being stored
  auto numr = [&](const uint8_t* raw) -> uint32_t { auto it = numk.find(id_of(raw)); return it == numk.end() ? UINT32_MAX : it->second; };
  auto numh = [&](const std::string& hex) -> uint32_t { if (hex.size() != 40) return UINT32_MAX; auto it = numk.find(id_of(hex)); return it == numk.end() ? UINT32_MAX : it->second; };
  struct NumView {
    std::function<uint32_t(const std::string&)> f;
    size_t count(const std::string& h) const { return f(h) != UINT32_MAX; }
    uint32_t operator[](const std::string& h) const { return f(h); }
  } num{numh};
  std::vector<uint8_t> kinds(n, 0);
  uint8_t none_kind = 0;
  auto kind_of = [&](const std::string& oid) -> uint8_t& { uint32_t i = numh(oid); if (i == UINT32_MAX) { none_kind = 0; return none_kind; } return kinds[i]; };
  std::vector<std::string> externals;
  std::unordered_map<Id20, uint32_t, Id20Hash> ext_num;
  auto ref_raw = [&](const uint8_t* raw) -> uint32_t {
    uint32_t i = numr(raw);
    if (i != UINT32_MAX) return i;
    Id20 k = id_of(raw);
    auto e = ext_num.find(k);
    if (e != ext_num.end()) return n + e->second;
    ext_num[k] = (uint32_t)externals.size();
    externals.push_back(hex_of(raw));
    return n + (uint32_t)externals.size() - 1;
  };
  auto ref = [&](const std::string& oid) -> uint32_t { Id20 k = id_of(oid); return ref_raw(k.b); };
  std::vector<std::string> modes, names;
  std::unordered_map<std::string, uint32_t> mode_num, name_num;
  auto dict = [](std::vector<std::string>& v, std::unordered_map<std::string, uint32_t>& m, const std::string& x) {
    auto it = m.find(x);
    if (it != m.end()) return it->second;
    m[x] = (uint32_t)v.size();
    v.push_back(x);
    return (uint32_t)v.size() - 1;
  };
  // Commits and tags are read once and kept (they are small); every tree is read once, on the walk below, and kept
  // encoded; blobs are only read when they are written.
  struct CommitInfo { ByteBuffer payload; std::string tree; std::vector<std::string> parents; uint64_t when = 0; };
  std::unordered_map<std::string, CommitInfo> commits;
  std::unordered_map<std::string, ByteBuffer> tags;
  std::unordered_map<std::string, std::string> tree_enc;
  std::unordered_map<std::string, std::string> path_of;
  std::unordered_map<std::string, size_t> first_seen;
  size_t clock = 0;
  auto take_commit = [&](const std::string& id, ByteBuffer payload) {
    CommitInfo ci;
    std::string text((const char*)payload.data(), payload.size());
    size_t i = 0;
    while (i < text.size()) {
      size_t nl = text.find('\n', i);
      if (nl == std::string::npos || nl == i) break;
      if (text.compare(i, 5, "tree ") == 0) ci.tree = text.substr(i + 5, 40);
      else if (text.compare(i, 7, "parent ") == 0) ci.parents.push_back(text.substr(i + 7, 40));
      else if (text.compare(i, 10, "committer ") == 0) {
        std::string line = text.substr(i, nl - i);
        size_t gt = line.rfind('>');
        if (gt != std::string::npos) ci.when = std::strtoull(line.c_str() + gt + 2, nullptr, 10);
      }
      i = nl + 1;
    }
    st.raw_bytes += payload.size();
    ci.payload = std::move(payload);
    commits[id] = std::move(ci);
  };
  // a tree, encoded on first sight, and what it holds given paths
  std::function<Result<void>(const std::string&, const std::string&)> walk_tree = [&](const std::string& t, const std::string& at) -> Result<void> {
    if (tree_enc.count(t) || !num.count(t)) return Ok();
    auto o = repo.read_object(t);
    if (!o.ok()) return Result<void>(o.error());
    std::vector<TreeEntry> ents;
    if (o->type != "tree" || !parse_tree(o->payload, ents)) return Result<void>(Status::Corrupt, "tree " + t + " does not parse");
    st.raw_bytes += o->payload.size();
    kind_of(t) = KTree;
    path_of[t] = at;
    first_seen[t] = clock++;
    std::string enc;
    putv(enc, ents.size());
    std::vector<uint32_t> refs;
    refs.reserve(ents.size());
    for (auto& e : ents) {
      putv(enc, dict(modes, mode_num, e.mode));
      putv(enc, dict(names, name_num, e.name));
      refs.push_back(ref_raw(e.raw));
      putv(enc, refs.back());
    }
    tree_enc[t] = std::move(enc);
    for (size_t k = 0; k < ents.size(); ++k) {
      auto& e = ents[k];
      uint32_t r = refs[k];
      if (e.mode == "40000" || e.mode == "040000") {
        if (r >= n || tree_enc.count(sorted[r])) continue;
        if (auto w = walk_tree(sorted[r], at.empty() ? e.name : at + "/" + e.name); !w.ok()) return w;
      } else if (e.mode != "160000" && r < n && !kinds[r]) {
        kinds[r] = KBlob;
        path_of[sorted[r]] = at.empty() ? e.name : at + "/" + e.name;
        first_seen[sorted[r]] = clock++;
      }
    }
    return Ok();
  };
  // every commit the refs reach (and tags on the way)
  {
    std::vector<std::string> todo;
    if (auto refs = repo.refs(); refs.ok()) for (auto& r : *refs) todo.push_back(r.oid);
    if (auto h = repo.head_oid(); h.ok()) todo.push_back(*h);
    std::unordered_set<std::string> visited;
    while (!todo.empty()) {
      std::string x = todo.back();
      todo.pop_back();
      if (!num.count(x) || !visited.insert(x).second) continue;
      auto o = repo.read_object(x);
      if (!o.ok()) continue;
      if (o->type == "commit") {
        kind_of(x) = KCommit;
        take_commit(x, std::move(o->payload));
        for (auto& p : commits[x].parents) todo.push_back(p);
      } else if (o->type == "tag") {
        kind_of(x) = KTag;
        std::string text((const char*)o->payload.data(), o->payload.size());
        if (text.rfind("object ", 0) == 0) todo.push_back(text.substr(7, 40));
        st.raw_bytes += o->payload.size();
        tags[x] = std::move(o->payload);
      } else if (o->type == "tree") {
        if (auto r = walk_tree(x, ""); !r.ok()) return R(r.error());
      } else if (o->type == "blob") {
        kind_of(x) = KBlob;
      }
    }
  }
  // newest first: versions of each path are met (and ordered) from the newest down
  std::vector<std::string> commit_order;
  for (auto& [id, ci] : commits) commit_order.push_back(id);
  std::sort(commit_order.begin(), commit_order.end(), [&](const std::string& a, const std::string& b) {
    if (commits[a].when != commits[b].when) return commits[a].when > commits[b].when;
    return a < b;
  });
  for (auto& c : commit_order) if (auto r = walk_tree(commits[c].tree, ""); !r.ok()) return R(r.error());
  // what nothing reaches is kept too, read now to learn what it is
  for (uint32_t i = 0; i < n; ++i) {
    if (kinds[i]) continue;
    auto o = repo.read_object(sorted[i]);
    if (!o.ok()) return R(o.error());
    if (o->type == "commit") { kinds[i] = KCommit; take_commit(sorted[i], std::move(o->payload)); commit_order.push_back(sorted[i]); }
    else if (o->type == "tag") { kinds[i] = KTag; st.raw_bytes += o->payload.size(); tags[sorted[i]] = std::move(o->payload); }
    else if (o->type == "tree") { if (auto r = walk_tree(sorted[i], ""); !r.ok()) return R(r.error()); }
    else if (o->type == "blob") kinds[i] = KBlob;
    else return R(Status::Corrupt, "object " + sorted[i] + " has an unknown type " + o->type);
  }
  auto by_path = [&](uint8_t k) {
    std::vector<std::string> v;
    for (uint32_t i = 0; i < n; ++i) if (kinds[i] == k) v.push_back(sorted[i]);
    std::stable_sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) {
      auto pa = path_of.find(a), pb = path_of.find(b);
      const std::string& sa = pa == path_of.end() ? std::string() : pa->second;
      const std::string& sb = pb == path_of.end() ? std::string() : pb->second;
      if (sa != sb) return sa < sb;
      auto fa = first_seen.find(a), fb = first_seen.find(b);
      return (fa == first_seen.end() ? SIZE_MAX : fa->second) < (fb == first_seen.end() ? SIZE_MAX : fb->second);
    });
    return v;
  };
  std::vector<std::array<uint32_t, 3>> loc(n, std::array<uint32_t, 3>{0, 0, 0});
  std::vector<uint32_t> rows(n, UINT32_MAX);
  ByteBuffer commit_col, parent_col, chunk_lists, chunk_table;
  uint32_t ncommits = 0, nparents = 0, nchunk_refs = 0, nchunks = 0;
  Frames fr;
  for (auto& id : commit_order) {
    CommitInfo& ci = commits[id];
    auto [f, off] = fr.add(fr.text, ci.payload.data(), ci.payload.size(), kTextFrame);
    uint32_t i = num[id];
    loc[i] = {f, off, (uint32_t)ci.payload.size()};
    rows[i] = ncommits++;
    put32(commit_col, ref(ci.tree));
    put32(commit_col, nparents);
    put32(commit_col, (uint32_t)ci.parents.size());
    put32(commit_col, 0);
    put64(commit_col, ci.when);
    for (auto& p : ci.parents) { put32(parent_col, ref(p)); ++nparents; }
    ByteBuffer().swap(ci.payload);
    ++st.commits;
  }
  for (auto& id : by_path(KTag)) {
    ByteBuffer& b = tags[id];
    auto [f, off] = fr.add(fr.text, b.data(), b.size(), kTextFrame);
    loc[num[id]] = {f, off, (uint32_t)b.size()};
    ++st.tags;
  }
  fr.seal(fr.text);
  for (auto& id : by_path(KTree)) {
    std::string& enc = tree_enc[id];
    auto [f, off] = fr.add(fr.text, (const uint8_t*)enc.data(), enc.size(), kTextFrame);
    loc[num[id]] = {f, off, (uint32_t)enc.size()};
    std::string().swap(enc);
    ++st.trees;
  }
  fr.seal(fr.text);
  std::unordered_map<std::string, uint32_t> chunk_ids;   // the chunk's own SHA-1 -> its number
  std::string prev_id, prev_path;
  ByteBuffer prev_payload;
  int prev_depth = 0;
  std::unordered_map<std::string, int> depth_of;
  // every source blob's outline, each version of a path parsed incrementally from the one before it
  std::string syntax;
  uint64_t noutlines = 0;
  std::string syn_prev_path, syn_prev_text;
  for (auto& id : by_path(KBlob)) {
    auto o = repo.read_object(id);
    if (!o.ok()) return R(o.error());
    st.raw_bytes += o->payload.size();
    auto pp = path_of.find(id);
    const std::string path = pp == path_of.end() ? std::string() : pp->second;
    if (outlines && !path.empty() && o->payload.size() <= kMaxOutlined && !git_structural_language(path).empty()) {
      std::string text((const char*)o->payload.data(), o->payload.size());
      auto ol = git_structural_outline(path, text, path == syn_prev_path ? std::string_view(syn_prev_text) : std::string_view());
      if (ol) {
        std::string enc = git_syntax_outline_encode(*ol);
        putv(syntax, num[id]);
        putv(syntax, enc.size());
        syntax += enc;
        ++noutlines;
      }
      syn_prev_path = path;
      syn_prev_text = std::move(text);
    }
    // the version of this path written just before (a newer one: versions run newest first) as a delta base
    if (!prev_id.empty() && !path.empty() && path == prev_path && prev_depth < kMaxDeltaDepth && o->payload.size() > 512) {
      ByteBuffer delta = git_make_delta(ByteSpan(prev_payload.data(), prev_payload.size()), ByteSpan(o->payload.data(), o->payload.size()),
                                        o->payload.size() / 2);
      if (!delta.empty()) {
        auto [f, off] = fr.add(fr.chunk, delta.data(), delta.size(), kChunkFrame);
        uint32_t i = num[id];
        kinds[num[id]] = KBlobDelta;
        loc[i] = {f, off, (uint32_t)delta.size()};
        rows[i] = num[prev_id];
        depth_of[id] = prev_depth + 1;
        ++st.blobs;
        prev_id = id;
        prev_payload.swap(o->payload);
        prev_depth = prev_depth + 1;
        continue;
      }
    }
    prev_id = id;
    prev_path = path;
    prev_depth = 0;
    uint32_t first = nchunk_refs, count = 0;
    for (auto [s, len] : chunk_spans(o->payload)) {
      auto h = crypto::sha1(ByteSpan(o->payload.data() + s, len));
      std::string key((const char*)h.data(), h.size());
      auto it = chunk_ids.find(key);
      uint32_t cn;
      if (it != chunk_ids.end()) cn = it->second;
      else {
        auto [f, off] = fr.add(fr.chunk, o->payload.data() + s, len, kChunkFrame);
        put32(chunk_table, f);
        put32(chunk_table, off);
        put32(chunk_table, (uint32_t)len);
        cn = nchunks++;
        chunk_ids[key] = cn;
      }
      put32(chunk_lists, cn);
      ++nchunk_refs;
      ++count;
    }
    loc[num[id]] = {first, count, (uint32_t)o->payload.size()};
    ++st.blobs;
    prev_payload.swap(o->payload);
  }
  fr.seal(fr.chunk);
  fr.seal(fr.text);
  // the file
  ByteBuffer out;
  const char* magic = "AIOSBLK1";
  out.insert(out.end(), magic, magic + 8);
  put32(out, kVersion);
  put32(out, n);
  put32(out, nchunks);
  put32(out, (uint32_t)fr.table.size());
  put32(out, (uint32_t)externals.size());
  put32(out, ncommits);
  put32(out, kSections);
  auto section = [&](const ByteBuffer& b) { put64(out, b.size()); out.insert(out.end(), b.begin(), b.end()); };
  ByteBuffer s0, s1, s2, s3, s4, s9, s10, s11, s12, s13;
  uint32_t fan[256] = {0};
  for (auto& id : sorted) {
    uint8_t raw[20];
    raw_of(id, raw);
    s0.insert(s0.end(), raw, raw + 20);
    fan[raw[0]]++;
  }
  for (int i = 1; i < 256; ++i) fan[i] += fan[i - 1];
  for (int i = 0; i < 256; ++i) put32(s1, fan[i]);
  for (uint32_t i = 0; i < n; ++i) s2.push_back(kinds[i]);
  for (uint32_t i = 0; i < n; ++i) { put32(s3, loc[i][0]); put32(s3, loc[i][1]); put32(s3, loc[i][2]); }
  for (uint32_t i = 0; i < n; ++i) put32(s4, rows[i]);
  for (auto& f : fr.table) { put64(s9, f[0]); put32(s9, (uint32_t)f[1]); put32(s9, (uint32_t)f[2]); }
  for (auto& e : externals) { uint8_t raw[20]; raw_of(e, raw); s10.insert(s10.end(), raw, raw + 20); }
  auto joined = [](const std::vector<std::string>& v) {
    std::string t;
    for (auto& x : v) { t += x; t.push_back('\0'); }
    return compress((const uint8_t*)t.data(), t.size());
  };
  s11 = joined(modes);
  s12 = joined(names);
  {
    std::string all;
    putv(all, noutlines);
    all += syntax;
    s13 = compress((const uint8_t*)all.data(), all.size());
  }
  for (auto* b : {&s0, &s1, &s2, &s3, &s4, &commit_col, &parent_col, &chunk_lists, &chunk_table, &s9, &s10, &s11, &s12, &s13})
    section(*b);
  put64(out, fr.data.size());
  out.insert(out.end(), fr.data.begin(), fr.data.end());
  auto sum = crypto::sha1(ByteSpan(out.data(), out.size()));
  out.insert(out.end(), sum.begin(), sum.end());
  // written aside, opened, every object read back and checked, and only then given its name
  auto& host = repo.host();
  std::string dir = repo.common_dir() + "/objects/blocks";
  (void)host.make_dir(dir);
  std::string name = dir + "/blk-" + hex_of(sum.data()) + ".blk";
  std::string tmp = name + ".tmp";
  if (auto w = host.write_file(tmp, ByteSpan(out.data(), out.size())); !w.ok()) return R(w.error());
  auto bf = GitBlockFile::open(host, tmp);
  if (!bf.ok()) { (void)host.remove(tmp); return R(bf.error()); }
  for (uint32_t i : (*bf)->storage_order()) {
    auto o = (*bf)->read(i);
    std::string why;
    if (!o.ok()) why = o.error().message;
    else {
      std::string head = o->type + " " + std::to_string(o->payload.size());
      ByteBuffer all(head.begin(), head.end());
      all.push_back(0);
      all.insert(all.end(), o->payload.begin(), o->payload.end());
      auto h = crypto::sha1(ByteSpan(all.data(), all.size()));
      if (hex_of(h.data()) != sorted[i]) why = "reads back as " + hex_of(h.data());
    }
    if (!why.empty()) { (void)host.remove(tmp); return R(Status::Corrupt, "block file check failed for " + sorted[i] + ": " + why); }
  }
  if (auto r = host.rename_file(tmp, name); !r.ok()) { (void)host.remove(tmp); return R(r.error()); }
  st.objects = n;
  st.chunks = nchunks;
  st.chunk_refs = nchunk_refs;
  st.names = names.size();
  st.outlines = noutlines;
  st.frames = fr.table.size();
  st.file_bytes = out.size();
  st.path = name;
  repo.rescan_packs();
  return st;
}

// ---------------------------------------------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------------------------------------------
Result<std::shared_ptr<GitBlockFile>> GitBlockFile::open(hal::Host& host, const std::string& path) {
  using R = Result<std::shared_ptr<GitBlockFile>>;
  auto f = host.read_file(path);
  if (!f.ok()) return R(f.error());
  auto bf = std::shared_ptr<GitBlockFile>(new GitBlockFile());
  bf->path_ = path;
  bf->data_ = std::move(*f);
  const ByteBuffer& d = bf->data_;
  if (d.size() < 8 + 28 + 20 || std::memcmp(d.data(), "AIOSBLK1", 8) != 0) return R(Status::Corrupt, path + " is not a block file");
  const uint8_t* p = d.data() + 8;
  if (get32(p) != kVersion) return R(Status::Unsupported, path + " is a block file of another version");
  bf->n_ = get32(p + 4);
  bf->nchunks_ = get32(p + 8);
  bf->nframes_ = get32(p + 12);
  bf->next_ = get32(p + 16);
  bf->ncommits_ = get32(p + 20);
  uint32_t sections = get32(p + 24);
  size_t at = 8 + 28;
  std::vector<std::pair<const uint8_t*, uint64_t>> sec;
  for (uint32_t i = 0; i <= sections; ++i) {   // the frame data is the last "section"
    if (at + 8 > d.size()) return R(Status::Corrupt, path + " is cut short");
    uint64_t len = get64(d.data() + at);
    at += 8;
    if (at + len > d.size() - 20) return R(Status::Corrupt, path + " is cut short");
    sec.push_back({d.data() + at, len});
    at += len;
  }
  if (sections != kSections && sections != kSections - 1) return R(Status::Corrupt, path + " has an unexpected layout");
  auto need = [&](size_t i, uint64_t len) { return sec[i].second == len; };
  if (!need(0, (uint64_t)bf->n_ * 20) || !need(1, 1024) || !need(2, bf->n_) || !need(3, (uint64_t)bf->n_ * 12) || !need(4, (uint64_t)bf->n_ * 4) ||
      !need(5, (uint64_t)bf->ncommits_ * 24) || !need(8, (uint64_t)bf->nchunks_ * 12) || !need(9, (uint64_t)bf->nframes_ * 16) ||
      !need(10, (uint64_t)bf->next_ * 20))
    return R(Status::Corrupt, path + ": a section has the wrong size");
  bf->oids_ = sec[0].first;
  bf->fanout_ = sec[1].first;
  bf->kinds_ = sec[2].first;
  bf->loc_ = sec[3].first;
  bf->rows_ = sec[4].first;
  bf->commits_ = sec[5].first;
  bf->parents_ = sec[6].first;
  bf->chunk_lists_ = sec[7].first;
  bf->chunks_ = sec[8].first;
  bf->frames_ = sec[9].first;
  bf->ext_ = sec[10].first;
  auto names = [&](size_t i, std::vector<std::string>& out) -> bool {
    auto z = zlib_inflate(ByteSpan(sec[i].first, sec[i].second));
    if (!z.ok()) return false;
    std::string cur;
    for (uint8_t ch : *z) { if (ch == 0) { out.push_back(cur); cur.clear(); } else cur += (char)ch; }
    return true;
  };
  if (!names(11, bf->modes_) || !names(12, bf->names_)) return R(Status::Corrupt, path + ": its dictionaries do not inflate");
  if (sections == kSections) bf->syntax_ = std::string_view((const char*)sec[13].first, sec[13].second);
  // frame offsets are into the frame data section
  bf->frames_data_ = sec[sections].first;
  bf->frames_data_len_ = sec[sections].second;
  return bf;
}

bool GitBlockFile::find(const std::string& oid, uint32_t& idx) const {
  uint8_t want[20];
  if (!raw_of(oid, want)) return false;
  uint32_t lo = want[0] == 0 ? 0 : get32(fanout_ + (size_t)(want[0] - 1) * 4);
  uint32_t hi = get32(fanout_ + (size_t)want[0] * 4);
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    int c = std::memcmp(oids_ + (size_t)mid * 20, want, 20);
    if (c == 0) { idx = mid; return true; }
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return false;
}

std::string GitBlockFile::oid_at(uint32_t idx) const { return hex_of(oids_ + (size_t)idx * 20); }

const uint8_t* GitBlockFile::ref_raw(uint32_t ref) const {
  static const uint8_t zero[20] = {0};
  if (ref < n_) return oids_ + (size_t)ref * 20;
  if (ref - n_ < next_) return ext_ + (size_t)(ref - n_) * 20;
  return zero;
}

std::string GitBlockFile::ref_oid(uint32_t ref) const {
  if (ref < n_) return oid_at(ref);
  if (ref - n_ < next_) return hex_of(ext_ + (size_t)(ref - n_) * 20);
  return std::string(40, '0');
}

void GitBlockFile::ids_with_prefix(const std::string& prefix, std::vector<std::string>& out) const {
  if (prefix.size() < 2) return;
  uint8_t first = 0;
  {
    uint8_t tmp[20] = {0};
    std::string padded = prefix.substr(0, 2) + std::string(38, '0');
    if (!raw_of(padded, tmp)) return;
    first = tmp[0];
  }
  uint32_t lo = first == 0 ? 0 : get32(fanout_ + (size_t)(first - 1) * 4), hi = get32(fanout_ + (size_t)first * 4);
  for (uint32_t i = lo; i < hi; ++i) {
    std::string h = oid_at(i);
    if (h.compare(0, prefix.size(), prefix) == 0) out.push_back(h);
  }
}

Result<std::shared_ptr<ByteBuffer>> GitBlockFile::frame(uint32_t f) const {
  using R = Result<std::shared_ptr<ByteBuffer>>;
  for (auto& [k, v] : cache_) if (k == f) return v;
  if (f >= nframes_) return R(Status::Corrupt, path_ + ": no frame " + std::to_string(f));
  const uint8_t* e = frames_ + (size_t)f * 16;
  uint64_t off = get64(e);
  uint32_t clen = get32(e + 8), ulen = get32(e + 12);
  if (off + clen > frames_data_len_) return R(Status::Corrupt, path_ + ": frame " + std::to_string(f) + " runs past the end");
  std::shared_ptr<ByteBuffer> buf;
#if defined(AIOS_HAVE_ZLIB)
  {
    auto out = std::make_shared<ByteBuffer>(ulen);
    uLongf got = ulen;
    if (uncompress(out->data(), &got, frames_data_ + off, clen) == Z_OK && got == ulen) buf = out;
  }
#endif
  if (!buf) {
    size_t end = 0;
    auto z = zlib_inflate_at(ByteSpan(frames_data_ + off, clen), 0, &end, ulen);
    if (!z.ok()) return R(z.error());
    buf = std::make_shared<ByteBuffer>(std::move(*z));
  }
  cache_.insert(cache_.begin(), {f, buf});
  if (cache_.size() > 32) cache_.pop_back();
  return buf;
}

Result<std::string> GitBlockFile::slice(uint32_t f, uint32_t off, uint32_t len) const {
  auto fb = frame(f);
  if (!fb.ok()) return Result<std::string>(fb.error());
  if ((size_t)off + len > (*fb)->size()) return Result<std::string>(Status::Corrupt, path_ + ": an object runs past its frame");
  return std::string((const char*)(*fb)->data() + off, len);
}

Result<GitObject> GitBlockFile::read(uint32_t idx) const {
  using R = Result<GitObject>;
  if (idx >= n_) return R(Status::NotFound, "no object " + std::to_string(idx));
  GitObject o;
  o.oid = oid_at(idx);
  uint8_t k = kinds_[idx];
  const uint8_t* l = loc_ + (size_t)idx * 12;
  uint32_t a = get32(l), b = get32(l + 4), c = get32(l + 8);
  if (k == KCommit || k == KTag) {
    o.type = k == KCommit ? "commit" : "tag";
    auto s = slice(a, b, c);
    if (!s.ok()) return R(s.error());
    o.payload.assign(s->begin(), s->end());
    return o;
  }
  if (k == KTree) {
    o.type = "tree";
    auto s = slice(a, b, c);
    if (!s.ok()) return R(s.error());
    size_t at = 0;
    uint64_t count = 0;
    if (!getv(*s, at, count)) return R(Status::Corrupt, path_ + ": tree " + o.oid + " is cut short");
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t m = 0, nm = 0, r = 0;
      if (!getv(*s, at, m) || !getv(*s, at, nm) || !getv(*s, at, r) || m >= modes_.size() || nm >= names_.size())
        return R(Status::Corrupt, path_ + ": tree " + o.oid + " is damaged");
      const std::string& ms = modes_[m];
      const std::string& ns = names_[nm];
      o.payload.insert(o.payload.end(), ms.begin(), ms.end());
      o.payload.push_back(' ');
      o.payload.insert(o.payload.end(), ns.begin(), ns.end());
      o.payload.push_back(0);
      const uint8_t* raw = ref_raw((uint32_t)r);
      o.payload.insert(o.payload.end(), raw, raw + 20);
    }
    return o;
  }
  if (k == KBlob) {
    o.type = "blob";
    o.payload.reserve(c);
    for (uint32_t i = 0; i < b; ++i) {
      uint32_t cn = get32(chunk_lists_ + (size_t)(a + i) * 4);
      if (cn >= nchunks_) return R(Status::Corrupt, path_ + ": blob " + o.oid + " names a chunk that is not there");
      const uint8_t* ch = chunks_ + (size_t)cn * 12;
      auto s = slice(get32(ch), get32(ch + 4), get32(ch + 8));
      if (!s.ok()) return R(s.error());
      o.payload.insert(o.payload.end(), s->begin(), s->end());
    }
    if (o.payload.size() != c) return R(Status::Corrupt, path_ + ": blob " + o.oid + " has the wrong size");
    if (o.payload.size() > 512) { last_blob_ = std::make_shared<ByteBuffer>(o.payload); last_blob_idx_ = idx; }
    return o;
  }
  if (k == KBlobDelta) {
    o.type = "blob";
    uint32_t base = get32(rows_ + (size_t)idx * 4);
    if (base >= n_ || base == idx) return R(Status::Corrupt, path_ + ": blob " + o.oid + " names no base");
    if (last_blob_ && last_blob_idx_ == base) {
      auto s = slice(a, b, c);
      if (!s.ok()) return R(s.error());
      auto out = git_apply_delta(ByteSpan(last_blob_->data(), last_blob_->size()), ByteSpan((const uint8_t*)s->data(), s->size()));
      if (!out.ok()) return R(out.error());
      o.payload = std::move(*out);
    } else {
      auto bo = read(base);
      if (!bo.ok()) return R(bo.error());
      auto s = slice(a, b, c);
      if (!s.ok()) return R(s.error());
      auto out = git_apply_delta(ByteSpan(bo->payload.data(), bo->payload.size()), ByteSpan((const uint8_t*)s->data(), s->size()));
      if (!out.ok()) return R(out.error());
      o.payload = std::move(*out);
    }
    // the next version down the chain is usually read next
    last_blob_ = std::make_shared<ByteBuffer>(o.payload);
    last_blob_idx_ = idx;
    return o;
  }
  return R(Status::Corrupt, path_ + ": object " + o.oid + " has an unknown kind");
}

bool GitBlockFile::commit_meta(uint32_t idx, std::string& tree, std::vector<std::string>& parents, uint64_t& commit_time) const {
  if (idx >= n_ || kinds_[idx] != KCommit) return false;
  uint32_t row = get32(rows_ + (size_t)idx * 4);
  if (row >= ncommits_) return false;
  const uint8_t* r = commits_ + (size_t)row * 24;
  tree = ref_oid(get32(r));
  uint32_t first = get32(r + 4), count = get32(r + 8);
  parents.clear();
  for (uint32_t i = 0; i < count; ++i) parents.push_back(ref_oid(get32(parents_ + (size_t)(first + i) * 4)));
  commit_time = get64(r + 16);
  return true;
}

std::vector<uint32_t> GitBlockFile::storage_order() const {
  std::vector<std::pair<uint64_t, uint32_t>> keyed;
  keyed.reserve(n_);
  for (uint32_t i = 0; i < n_; ++i) {
    const uint8_t* l = loc_ + (size_t)i * 12;
    uint64_t frame = 0, off = 0;
    if (kinds_[i] == KBlob) {
      uint32_t first = get32(l), count = get32(l + 4);
      if (count && first < nchunks_ + 0u) {
        uint32_t cn = get32(chunk_lists_ + (size_t)first * 4);
        if (cn < nchunks_) { frame = get32(chunks_ + (size_t)cn * 12); off = get32(chunks_ + (size_t)cn * 12 + 4); }
      }
    } else {
      frame = get32(l);
      off = get32(l + 4);
    }
    keyed.push_back({frame << 32 | off, i});
  }
  std::sort(keyed.begin(), keyed.end());
  std::vector<uint32_t> out;
  out.reserve(n_);
  for (auto& [k, i] : keyed) out.push_back(i);
  return out;
}

std::optional<GitSyntaxOutline> GitBlockFile::outline(uint32_t idx) const {
  std::call_once(syntax_once_, [&] {
    if (syntax_.empty()) return;
    auto z = zlib_inflate(ByteSpan((const uint8_t*)syntax_.data(), syntax_.size()));
    if (!z.ok()) return;
    syntax_bytes_.assign((const char*)z->data(), z->size());
    size_t at = 0;
    uint64_t count = 0, obj = 0, len = 0;
    if (!getv(syntax_bytes_, at, count)) return;
    for (uint64_t i = 0; i < count; ++i) {
      if (!getv(syntax_bytes_, at, obj) || !getv(syntax_bytes_, at, len) || at + len > syntax_bytes_.size()) return;
      syntax_at_[(uint32_t)obj] = {at, len};
      at += len;
    }
  });
  auto it = syntax_at_.find(idx);
  if (it == syntax_at_.end()) return std::nullopt;
  return git_syntax_outline_decode(std::string_view(syntax_bytes_).substr(it->second.first, it->second.second));
}

GitBlockStats GitBlockFile::stats() const {
  GitBlockStats s;
  s.path = path_;
  s.objects = n_;
  s.chunks = nchunks_;
  s.frames = nframes_;
  s.names = names_.size();
  s.file_bytes = data_.size();
  if (!syntax_.empty()) {
    auto z = zlib_inflate(ByteSpan((const uint8_t*)syntax_.data(), syntax_.size()));
    size_t at = 0;
    uint64_t count = 0;
    if (z.ok() && getv(std::string((const char*)z->data(), z->size()), at, count)) s.outlines = count;
  }
  for (uint32_t i = 0; i < n_; ++i) {
    switch (kinds_[i]) {
      case KCommit: ++s.commits; break;
      case KTree: ++s.trees; break;
      case KBlob: ++s.blobs; s.chunk_refs += get32(loc_ + (size_t)i * 12 + 4); break;
      case KBlobDelta: ++s.blobs; ++s.deltas; break;
      case KTag: ++s.tags; break;
    }
  }
  return s;
}

Result<GitBlockStats> git_verify_blocks(const Git& repo, const std::string& path, bool every) {
  auto bf = GitBlockFile::open(repo.host(), path);
  if (!bf.ok()) return Result<GitBlockStats>(bf.error());
  auto& d = (*bf)->data_for_check();
  if (d.size() < 20) return Result<GitBlockStats>(Status::Corrupt, path + " is cut short");
  auto sum = crypto::sha1(ByteSpan(d.data(), d.size() - 20));
  if (std::memcmp(sum.data(), d.data() + d.size() - 20, 20) != 0) return Result<GitBlockStats>(Status::Corrupt, path + ": its checksum does not match");
  GitBlockStats st = (*bf)->stats();
  if (every) {
    for (uint32_t i : (*bf)->storage_order()) {
      auto o = (*bf)->read(i);
      if (!o.ok()) return Result<GitBlockStats>(o.error());
      st.raw_bytes += o->payload.size();
      std::string head = o->type + " " + std::to_string(o->payload.size());
      ByteBuffer all(head.begin(), head.end());
      all.push_back(0);
      all.insert(all.end(), o->payload.begin(), o->payload.end());
      auto h = crypto::sha1(ByteSpan(all.data(), all.size()));
      if (hex_of(h.data()) != o->oid) return Result<GitBlockStats>(Status::Corrupt, path + ": " + o->oid + " reads back as " + hex_of(h.data()));
    }
  }
  return st;
}

}  // namespace aios::userland
