// Idents and the mailmap, for the history commands.
#include "hist_util.hpp"

#include <algorithm>
#include <cctype>

#include "aios/hal/host.hpp"

namespace aios_git {

using namespace aios;

namespace {
std::string fold(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}
std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}
}  // namespace

IdentParts split_ident(const std::string& ident) {
  IdentParts p;
  size_t lt = ident.find('<');
  size_t gt = ident.rfind('>');
  if (lt == std::string::npos || gt == std::string::npos || gt < lt) { p.name = ident; return p; }
  p.name = ident.substr(0, lt);
  while (!p.name.empty() && p.name.back() == ' ') p.name.pop_back();
  p.email = ident.substr(lt + 1, gt - lt - 1);
  return p;
}

void Mailmap::add_text(const std::string& text) {
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    // [name] <email> [name] <email>
    std::vector<std::pair<std::string, std::string>> parts;   // name before, email
    size_t at = 0;
    bool bad = false;
    while (at < line.size()) {
      size_t lt = line.find('<', at);
      if (lt == std::string::npos) break;
      size_t gt = line.find('>', lt);
      if (gt == std::string::npos) { bad = true; break; }
      parts.push_back({trimmed(line.substr(at, lt - at)), line.substr(lt + 1, gt - lt - 1)});
      at = gt + 1;
    }
    if (bad || parts.empty() || parts.size() > 2) continue;
    Entry e;
    if (parts.size() == 1) {
      if (parts[0].first.empty()) continue;
      e.new_name = parts[0].first;
      e.has_new_name = true;
      e.old_email = parts[0].second;
    } else {
      if (!parts[0].first.empty()) { e.new_name = parts[0].first; e.has_new_name = true; }
      e.new_email = parts[0].second;
      e.has_new_email = true;
      e.old_email = parts[1].second;
      if (!parts[1].first.empty()) { e.old_name = parts[1].first; e.has_old_name = true; }
    }
    entries_.push_back(e);
  }
}

void Mailmap::load(Ctx& c) {
  entries_.clear();
  auto read_file = [&](const std::string& path) {
    auto t = c.host().read_file(path);
    if (t.ok()) add_text(std::string((const char*)t->data(), t->size()));
  };
  if (!c.top.empty()) read_file(c.top + "/.mailmap");
  else if (auto o = resolve(c, "HEAD:.mailmap")) add_text(blob_text(c, *o));
  if (auto f = c.config.get("mailmap.file")) {
    std::string p = *f;
    if (!p.empty() && p[0] == '~' && c.hasenv("HOME")) p = c.getenv("HOME") + p.substr(1);
    read_file(c.abs_path(p));
  }
  if (auto b = c.config.get("mailmap.blob"))
    if (auto o = resolve(c, *b)) add_text(blob_text(c, *o));
}

bool Mailmap::map(std::string& name, std::string& email) const {
  const std::string fe = fold(email), fn = fold(name);
  const Entry* generic = nullptr;
  const Entry* exact = nullptr;
  for (auto& e : entries_) {
    if (fold(e.old_email) != fe) continue;
    if (e.has_old_name) {
      if (fold(e.old_name) == fn) exact = &e;
    } else {
      generic = &e;
    }
  }
  const Entry* hit = exact ? exact : generic;
  if (!hit) return false;
  if (hit->has_new_email) email = hit->new_email;
  if (hit->has_new_name) name = hit->new_name;
  return true;
}

namespace {
std::string hex_of(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
}  // namespace

void PackSet::load(Ctx& c) {
  c_ = &c;
  packs_.clear();
  auto& h = c.host();
  std::vector<std::string> dirs = {c.repo->common_dir() + "/objects/pack"};
  for (auto& alt : c.repo->alternate_object_dirs()) dirs.push_back(alt + "/pack");
  for (auto& dir : dirs) {
    auto names = h.list_dir(dir);
    if (!names.ok()) continue;
    std::vector<std::string> sorted = *names;
    std::sort(sorted.begin(), sorted.end());
    for (auto& n : sorted) {
      if (n.size() < 4 || n.compare(n.size() - 4, 4, ".idx") != 0) continue;
      auto idx = h.read_file(dir + "/" + n);
      if (!idx.ok() || idx->size() < 8 + 1024) continue;
      const uint8_t* b = idx->data();
      Pack p;
      p.idx_path = dir + "/" + n;
      p.pack_path = dir + "/" + n.substr(0, n.size() - 4) + ".pack";
      auto st = h.stat(p.pack_path);
      p.pack_size = st.ok() ? st->size : 0;
      const bool v2 = b[0] == 0xff && b[1] == 't' && b[2] == 'O' && b[3] == 'c';
      p.version = v2 ? be32(b + 4) : 1;
      size_t fan = v2 ? 8 : 0;
      uint32_t count = be32(b + fan + 255 * 4);
      if (v2) {
        size_t sha_at = fan + 1024, crc_at = sha_at + (size_t)count * 20, off_at = crc_at + (size_t)count * 4, big_at = off_at + (size_t)count * 4;
        if (big_at > idx->size()) continue;
        for (uint32_t i = 0; i < count; ++i) {
          p.oids.push_back(hex_of(b + sha_at + (size_t)i * 20, 20));
          p.crcs.push_back(be32(b + crc_at + (size_t)i * 4));
          uint32_t o = be32(b + off_at + (size_t)i * 4);
          if (o & 0x80000000u) {
            size_t k = o & 0x7fffffffu;
            if (big_at + (k + 1) * 8 > idx->size()) { p.offsets.push_back(0); continue; }
            p.offsets.push_back(be64(b + big_at + k * 8));
          } else {
            p.offsets.push_back(o);
          }
        }
      } else {
        size_t at = 1024;
        if (at + (size_t)count * 24 > idx->size()) continue;
        for (uint32_t i = 0; i < count; ++i) {
          p.offsets.push_back(be32(b + at + (size_t)i * 24));
          p.oids.push_back(hex_of(b + at + (size_t)i * 24 + 4, 20));
          p.crcs.push_back(0);
        }
      }
      for (size_t i = 0; i < p.offsets.size(); ++i) p.by_offset.push_back({p.offsets[i], i});
      std::sort(p.by_offset.begin(), p.by_offset.end());
      packs_.push_back(std::move(p));
    }
  }
}

PackedInfo PackSet::info(Pack& p, size_t pos) {
  PackedInfo r;
  r.offset = p.offsets[pos];
  r.pack = p.pack_path;
  r.crc = p.crcs[pos];
  auto it = std::upper_bound(p.by_offset.begin(), p.by_offset.end(), std::make_pair(r.offset, SIZE_MAX));
  uint64_t end = it == p.by_offset.end() ? (p.pack_size >= 20 ? p.pack_size - 20 : p.pack_size) : it->first;
  r.disk = end - r.offset;
  auto head = c_->host().read_range(p.pack_path, r.offset, 64);
  if (!head.ok() || head->empty()) return r;
  const uint8_t* b = head->data();
  size_t i = 0;
  uint8_t ch = b[i++];
  r.type = (ch >> 4) & 7;
  r.size = ch & 15;
  int shift = 4;
  while ((ch & 0x80) && i < head->size()) { ch = b[i++]; r.size |= (uint64_t)(ch & 0x7f) << shift; shift += 7; }
  if (r.type == 7 && i + 20 <= head->size()) {
    r.base = hex_of(b + i, 20);
  } else if (r.type == 6) {
    ch = b[i++];
    uint64_t off = ch & 0x7f;
    while ((ch & 0x80) && i < head->size()) { ch = b[i++]; off = ((off + 1) << 7) | (ch & 0x7f); }
    uint64_t base_off = r.offset - off;
    auto f = std::lower_bound(p.by_offset.begin(), p.by_offset.end(), std::make_pair(base_off, (size_t)0));
    if (f != p.by_offset.end() && f->first == base_off) r.base = p.oids[f->second];
  }
  return r;
}

bool PackSet::find(const std::string& oid, PackedInfo& out) {
  for (auto& p : packs_) {
    auto it = std::lower_bound(p.oids.begin(), p.oids.end(), oid);
    if (it == p.oids.end() || *it != oid) continue;
    out = info(p, (size_t)(it - p.oids.begin()));
    return true;
  }
  return false;
}

}  // namespace aios_git
