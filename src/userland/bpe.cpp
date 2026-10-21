#include "aios/userland/bpe.hpp"
#include "aios/channel/rpc_value.hpp"
#include <algorithm>
#include <limits>

namespace aios::userland {
namespace {

#include "unicode_classes.inc"

template <size_t N>
bool in_ranges(const uint32_t (&t)[N][2], uint32_t cp) {
  size_t lo = 0, hi = N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < t[mid][0]) hi = mid;
    else if (cp > t[mid][1]) lo = mid + 1;
    else return true;
  }
  return false;
}
bool is_letter(uint32_t cp) { return in_ranges(kLetters, cp); }
bool is_number(uint32_t cp) { return in_ranges(kNumbers, cp); }
bool is_space(uint32_t cp) {
  return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
         cp == 0x205F || cp == 0x3000;
}
bool is_nl(uint32_t cp) { return cp == '\r' || cp == '\n'; }

void put_utf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) s.push_back((char)cp);
  else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { s.push_back((char)(0xE0 | (cp >> 12))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
  else { s.push_back((char)(0xF0 | (cp >> 18))); s.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
}

// One code point and its byte length at s[i]; a stray byte counts as itself so
// invalid UTF-8 still round-trips through the byte mapping.
uint32_t next_cp(const std::string& s, size_t i, size_t& len) {
  const uint8_t c = (uint8_t)s[i];
  auto cont = [&](size_t k) { return i + k < s.size() && ((uint8_t)s[i + k] & 0xC0) == 0x80; };
  if (c < 0x80) { len = 1; return c; }
  if (c >= 0xC2 && c < 0xE0 && cont(1)) { len = 2; return ((c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F); }
  if (c >= 0xE0 && c < 0xF0 && cont(1) && cont(2)) { len = 3; return ((c & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F); }
  if (c >= 0xF0 && c < 0xF5 && cont(1) && cont(2) && cont(3)) { len = 4; return ((c & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) | (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F); }
  len = 1;
  return 0xFFFD0000u | c;   // never a letter, number or space: falls in the punctuation class
}

}  // namespace

Result<BpeTokenizer> BpeTokenizer::from_json(const std::string& json) {
  auto doc = channel::rpc_parse(json);
  if (!doc.ok()) return Result<BpeTokenizer>(Status::Invalid, "tokenizer.json: " + doc.error().message);
  BpeTokenizer t;

  // GPT-2's byte -> printable code point map: bytes that print as themselves
  // keep their value, the rest take 256, 257, ... in order.
  {
    bool direct[256] = {};
    for (int b = 33; b <= 126; ++b) direct[b] = true;
    for (int b = 161; b <= 172; ++b) direct[b] = true;
    for (int b = 174; b <= 255; ++b) direct[b] = true;
    uint32_t extra = 256;
    for (int b = 0; b < 256; ++b) put_utf8(t.byte_char_[b], direct[b] ? (uint32_t)b : extra++);
  }
  std::unordered_map<std::string, uint8_t> char_byte;
  for (int b = 0; b < 256; ++b) char_byte[t.byte_char_[b]] = (uint8_t)b;

  const auto& model = (*doc)["model"];
  if (model.kind() != channel::RpcValue::Kind::Object)
    return Result<BpeTokenizer>(Status::Invalid, "tokenizer.json has no \"model\"");
  int32_t max_id = -1;
  for (const auto& [tok, id] : model["vocab"].fields()) {
    const int32_t i = (int32_t)id.as_u64();
    t.vocab_[tok] = i;
    max_id = std::max(max_id, i);
  }
  const auto& merges = model["merges"];
  int32_t rank = 0;
  for (const auto& m : merges.items()) {
    if (m.kind() == channel::RpcValue::Kind::Text) t.ranks_[m.as_text()] = rank++;
    else if (m.size() == 2) t.ranks_[m[0].as_text() + " " + m[1].as_text()] = rank++;
  }
  for (const auto& a : (*doc)["added_tokens"].items()) {
    const int32_t i = (int32_t)a["id"].as_u64();
    t.added_[a["content"].as_text()] = i;
    max_id = std::max(max_id, i);
  }
  if (max_id < 0) return Result<BpeTokenizer>(Status::Invalid, "tokenizer.json holds no tokens");

  t.bytes_.assign((size_t)max_id + 1, std::string());
  for (const auto& [tok, id] : t.vocab_) {
    std::string raw;
    for (size_t i = 0; i < tok.size();) {
      size_t len = 1;
      next_cp(tok, i, len);
      auto it = char_byte.find(tok.substr(i, len));
      if (it == char_byte.end()) { raw.assign(tok); break; }   // not byte-mapped: keep as written
      raw.push_back((char)it->second);
      i += len;
    }
    t.bytes_[(size_t)id] = raw;
  }
  for (const auto& [content, id] : t.added_) {
    t.bytes_[(size_t)id] = content;
    t.added_by_len_.push_back(content);
  }
  std::sort(t.added_by_len_.begin(), t.added_by_len_.end(),
            [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  return t;
}

int32_t BpeTokenizer::token_id(const std::string& content) const {
  if (auto it = added_.find(content); it != added_.end()) return it->second;
  if (auto it = vocab_.find(content); it != vocab_.end()) return it->second;
  return -1;
}

const std::string& BpeTokenizer::token_bytes(int32_t id) const {
  static const std::string none;
  return id >= 0 && (size_t)id < bytes_.size() ? bytes_[(size_t)id] : none;
}

std::string BpeTokenizer::decode(const std::vector<int32_t>& ids) const {
  std::string out;
  for (int32_t id : ids) out += token_bytes(id);
  return out;
}

// Merge the byte-mapped pieces of one pre-token by lowest rank until none left.
void BpeTokenizer::encode_piece(const std::string& piece, std::vector<int32_t>& out) const {
  std::vector<std::string> parts;
  for (unsigned char b : piece) parts.push_back(byte_char_[b]);
  while (parts.size() > 1) {
    int32_t best = std::numeric_limits<int32_t>::max();
    size_t at = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      auto it = ranks_.find(parts[i] + " " + parts[i + 1]);
      if (it != ranks_.end() && it->second < best) { best = it->second; at = i; }
    }
    if (best == std::numeric_limits<int32_t>::max()) break;
    parts[at] += parts[at + 1];
    parts.erase(parts.begin() + (long)at + 1);
  }
  for (const auto& p : parts) {
    auto it = vocab_.find(p);
    if (it != vocab_.end()) { out.push_back(it->second); continue; }
    // A merge result the vocabulary lacks: fall back to its single bytes.
    for (size_t i = 0; i < p.size();) {
      size_t len = 1;
      next_cp(p, i, len);
      auto b = vocab_.find(p.substr(i, len));
      if (b != vocab_.end()) out.push_back(b->second);
      i += len;
    }
  }
}

// Qwen2's pre-tokenizer pattern, matched by hand (alternatives in order):
//   (?i:'s|'t|'re|'ve|'m|'ll|'d) | [^\r\n\p{L}\p{N}]?\p{L}+ | \p{N}
//   | " "?[^\s\p{L}\p{N}]+[\r\n]* | \s*[\r\n]+ | \s+(?!\S) | \s+
void BpeTokenizer::encode_plain(const std::string& text, std::vector<int32_t>& out) const {
  std::vector<uint32_t> cp;
  std::vector<size_t> at;   // byte offset of each code point, plus the end
  for (size_t i = 0; i < text.size();) {
    size_t len = 1;
    cp.push_back(next_cp(text, i, len));
    at.push_back(i);
    i += len;
  }
  at.push_back(text.size());
  const size_t n = cp.size();
  auto lower = [](uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; };
  auto punct = [&](size_t i) { return i < n && !is_space(cp[i]) && !is_letter(cp[i]) && !is_number(cp[i]); };

  size_t i = 0;
  while (i < n) {
    size_t end = i;
    const uint32_t c = cp[i];
    if (c == '\'' && i + 1 < n) {
      const uint32_t a = lower(cp[i + 1]);
      const uint32_t b = i + 2 < n ? lower(cp[i + 2]) : 0;
      if (a == 's' || a == 't' || a == 'm' || a == 'd') end = i + 2;
      else if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) end = i + 3;
    }
    if (end == i && !is_nl(c) && !is_letter(c) && !is_number(c) && i + 1 < n && is_letter(cp[i + 1])) {
      end = i + 1;
      while (end < n && is_letter(cp[end])) ++end;
    }
    if (end == i && is_letter(c)) {
      end = i;
      while (end < n && is_letter(cp[end])) ++end;
    }
    if (end == i && is_number(c)) end = i + 1;
    if (end == i && (punct(i) || (c == ' ' && punct(i + 1)))) {
      end = (c == ' ') ? i + 1 : i;
      while (punct(end)) ++end;
      while (end < n && is_nl(cp[end])) ++end;
    }
    if (end == i && is_space(c)) {
      size_t run = i;
      while (run < n && is_space(cp[run])) ++run;
      size_t last_nl = n;
      for (size_t k = i; k < run; ++k) if (is_nl(cp[k])) last_nl = k;
      if (last_nl != n) end = last_nl + 1;                          // \s*[\r\n]+
      else if (run == n) end = run;                                // \s+(?!\S) at the end
      else if (run - i > 1) end = run - 1;                         // leave one for the next word
      else end = run;                                              // \s+
    }
    if (end == i) end = i + 1;   // cannot happen; never loop without moving
    encode_piece(text.substr(at[i], at[end] - at[i]), out);
    i = end;
  }
}

std::vector<int32_t> BpeTokenizer::encode(const std::string& text) const {
  std::vector<int32_t> out;
  size_t from = 0, i = 0;
  while (i < text.size()) {
    if (text[i] == '<') {
      const std::string* hit = nullptr;
      for (const auto& a : added_by_len_)
        if (text.compare(i, a.size(), a) == 0) { hit = &a; break; }
      if (hit) {
        if (i > from) encode_plain(text.substr(from, i - from), out);
        out.push_back(added_.at(*hit));
        i += hit->size();
        from = i;
        continue;
      }
    }
    ++i;
  }
  if (from < text.size()) encode_plain(text.substr(from), out);
  return out;
}

}  // namespace aios::userland
