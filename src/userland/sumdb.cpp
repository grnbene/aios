#include "aios/userland/sumdb.hpp"

#include <array>
#include <vector>

#include "aios/channel/http_client.hpp"
#include "aios/core/base64.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/sha256.hpp"

namespace aios::userland {

// The key the Go distribution itself carries for its checksum database. It is
// the whole root of this: a proxy, a network and the database itself may all
// lie, and none of them can produce a tree head this key did not sign.
const char* const kGoSumDatabaseKey =
    "sum.golang.org+033de0ae+Ac4zctda0e5eza+HJyk9SxEdh+s3Ux18htTTAD8OuAn8";

namespace {

std::string dec(int64_t v) {
  if (!v) return "0";
  std::string s;
  bool neg = v < 0;
  uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
  while (u) { s.insert(s.begin(), (char)('0' + u % 10)); u /= 10; }
  if (neg) s.insert(s.begin(), '-');
  return s;
}

// The kernel core may not reach libc's parsers, so decimals are read here.
bool parse_dec(const std::string& text, int64_t& out) {
  if (text.empty()) return false;
  int64_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

std::string pad3(int64_t v) {
  std::string s = dec(v);
  while (s.size() < 3) s.insert(s.begin(), '0');
  return s;
}

// A tile index is written in three-digit groups, every group but the last
// prefixed with x, so a log with billions of records is still a shallow tree of
// directories: 1234 is "x001/234".
std::string tile_index_path(int64_t n) {
  std::vector<std::string> parts;
  if (n == 0) parts.push_back("000");
  while (n > 0) { parts.push_back(pad3(n % 1000)); n /= 1000; }
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& p = parts[parts.size() - 1 - i];
    if (i) out += "/";
    out += (i + 1 < parts.size()) ? "x" + p : p;
  }
  return out;
}

void node_of(const uint8_t* left, const uint8_t* right, uint8_t out[32]) {
  crypto::SHA256Context ctx;
  crypto::sha256_init(ctx);
  uint8_t prefix = 0x01;
  crypto::sha256_update(ctx, &prefix, 1);
  crypto::sha256_update(ctx, left, 32);
  crypto::sha256_update(ctx, right, 32);
  crypto::sha256_final(ctx, out);
}

void leaf_of(ByteSpan data, uint8_t out[32]) {
  crypto::SHA256Context ctx;
  crypto::sha256_init(ctx);
  uint8_t prefix = 0x00;
  crypto::sha256_update(ctx, &prefix, 1);
  crypto::sha256_update(ctx, data.data(), data.size());
  crypto::sha256_final(ctx, out);
}

int64_t largest_power_of_two_below(int64_t n) {
  int64_t k = 1;
  while (k * 2 < n) k *= 2;
  return k;
}

int level_of_width(int64_t w) {
  int l = 0;
  while ((int64_t(1) << l) < w) ++l;
  return l;
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) { if (start < text.size()) out.push_back(text.substr(start)); break; }
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

std::vector<std::string> split_ws(const std::string& line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    size_t j = i;
    while (j < line.size() && line[j] != ' ') ++j;
    if (j > i) out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

}  // namespace

// A note is text, a blank line, then one signature line per signer. The bytes
// the signature covers are the text INCLUDING its final newline and nothing
// after it — get that wrong and every signature in the world fails.
Result<std::string> note_verify(const std::string& note, const std::string& verifier_key, std::string& signer) {
  size_t plus = verifier_key.find('+');
  size_t plus2 = (plus == std::string::npos) ? std::string::npos : verifier_key.find('+', plus + 1);
  if (plus2 == std::string::npos)
    return Result<std::string>(Status::Invalid, "a verifier key is <name>+<hash>+<base64 key>");
  std::string name = verifier_key.substr(0, plus);
  std::string want_hash = verifier_key.substr(plus + 1, plus2 - plus - 1);
  auto key = base64_decode(verifier_key.substr(plus2 + 1));
  if (!key.ok()) return Result<std::string>(key.error());
  if (key->size() != 33 || (*key)[0] != 1)
    return Result<std::string>(Status::Invalid, "this machine verifies ed25519 notes; that key is a different algorithm");

  // The key's own hash, recomputed, so a mistyped key is caught here rather
  // than as a mysterious bad signature.
  std::string signed_name = name + "\n";
  ByteBuffer material(signed_name.begin(), signed_name.end());
  material.insert(material.end(), key->begin(), key->end());
  auto kh = crypto::sha256(ByteSpan(material.data(), material.size()));
  static const char* d = "0123456789abcdef";
  std::string hash_text;
  for (int i = 0; i < 4; ++i) { hash_text += d[kh[i] >> 4]; hash_text += d[kh[i] & 15]; }
  if (hash_text != want_hash)
    return Result<std::string>(Status::Invalid, "verifier key " + name + ": its own hash does not match the key");

  size_t sep = note.find("\n\n");
  if (sep == std::string::npos)
    return Result<std::string>(Status::Invalid, "this is not a signed note: no blank line before the signatures");
  std::string text = note.substr(0, sep + 1);
  std::string sigs = note.substr(sep + 2);

  crypto::Point32 pub{};
  for (int i = 0; i < 32; ++i) pub[i] = (*key)[i + 1];

  for (const auto& line : lines_of(sigs)) {
    // Each signature line opens with an em dash, then the signer, then the
    // signature; anything else in the block is not ours to read.
    if (line.size() < 4 || (uint8_t)line[0] != 0xE2 || (uint8_t)line[1] != 0x80 || (uint8_t)line[2] != 0x94) continue;
    auto parts = split_ws(line.substr(3));
    if (parts.size() != 2 || parts[0] != name) continue;
    auto raw = base64_decode(parts[1]);
    if (!raw.ok() || raw->size() != 68) continue;
    for (int i = 0; i < 4; ++i)
      if ((*raw)[i] != kh[i]) return Result<std::string>(Status::Denied, "the note is signed by a different key than " + name);
    crypto::Sig64 sig{};
    for (int i = 0; i < 64; ++i) sig[i] = (*raw)[i + 4];
    if (!crypto::ed25519_verify(ByteSpan((const uint8_t*)text.data(), text.size()), sig, pub))
      return Result<std::string>(Status::Denied, name + " did not sign this: the ed25519 signature does not verify");
    signer = name;
    return text;
  }
  return Result<std::string>(Status::Denied, "nothing in this note is signed by " + name);
}

ChecksumDatabase::ChecksumDatabase(Kernel& kernel, std::string base, std::string verifier_key, int timeout_ms)
    : kernel_(kernel), base_(std::move(base)), key_(std::move(verifier_key)), timeout_ms_(timeout_ms) {}

Result<ByteBuffer> ChecksumDatabase::fetch(const std::string& path) {
  auto ep = channel::parse_endpoint(base_ + path);
  if (!ep.ok()) return Result<ByteBuffer>(ep.error());
  channel::HttpRequest req;
  req.endpoint = *ep;
  req.timeout_ms = timeout_ms_;
  req.host = &kernel_.host();
  req.kernel_root = kernel_.config().root;
  auto r = channel::http_send(req);
  if (!r.ok()) return Result<ByteBuffer>(r.error());
  if (r->status == 404 || r->status == 410)
    return Result<ByteBuffer>(Status::NotFound, path + ": the checksum database has no such record");
  if (r->status != 200)
    return Result<ByteBuffer>(Status::NotFound,
                             path + ": the checksum database answered " + dec(r->status) + " " + r->reason);
  return r->body;
}

Result<ByteBuffer> ChecksumDatabase::tile(int level, int64_t n, int width) {
  std::string path = "/tile/8/" + dec(level) + "/" + tile_index_path(n);
  if (width > 0 && width < 256) path += ".p/" + dec(width);
  auto have = tiles_.find(path);
  if (have != tiles_.end()) return have->second;
  auto got = fetch(path);
  if (!got.ok()) return got;
  tiles_[path] = *got;
  return got;
}

// The hash of the complete subtree of 2^level leaves at `index`. Only the rows
// at multiples of eight are published; anything between is folded up here from
// the row below it, which is exactly what a tile is for.
Result<void> ChecksumDatabase::node_hash(int level, int64_t index, int64_t tree_size, uint8_t out[32]) {
  int group = level / 8, within = level % 8;
  int64_t base = index << within;
  int64_t n = base / 256;
  size_t off = (size_t)(base % 256);
  size_t count = (size_t)1 << within;
  int64_t complete = tree_size >> (8 * group);          // whole nodes on that row
  int width = (int)((n + 1) * 256 <= complete ? 256 : complete - n * 256);
  if (width <= 0 || (size_t)width < off + count)
    return Result<void>(Status::Invalid, "the log does not hold that node yet");
  auto data = tile(group, n, width);
  if (!data.ok()) return Result<void>(data.error());
  if (data->size() < (off + count) * 32)
    return Result<void>(Status::Corrupt, "a tile came back short of the hashes it must contain");
  std::vector<std::array<uint8_t, 32>> row(count);
  for (size_t i = 0; i < count; ++i)
    for (int b = 0; b < 32; ++b) row[i][b] = (*data)[(off + i) * 32 + b];
  while (row.size() > 1) {
    std::vector<std::array<uint8_t, 32>> up(row.size() / 2);
    for (size_t i = 0; i < up.size(); ++i) node_of(row[2 * i].data(), row[2 * i + 1].data(), up[i].data());
    row.swap(up);
  }
  for (int b = 0; b < 32; ++b) out[b] = row[0][b];
  return Ok();
}

Result<void> ChecksumDatabase::subtree_hash(int64_t lo, int64_t hi, int64_t tree_size, uint8_t out[32]) {
  int64_t width = hi - lo;
  if (width <= 0) return Result<void>(Status::Invalid, "an empty range has no hash");
  if ((width & (width - 1)) == 0 && lo % width == 0) return node_hash(level_of_width(width), lo / width, tree_size, out);
  // The log's right edge is not a whole subtree; split it the way the tree was
  // built and hash the pieces.
  int64_t k = largest_power_of_two_below(width);
  uint8_t left[32], right[32];
  if (auto r = subtree_hash(lo, lo + k, tree_size, left); !r.ok()) return r;
  if (auto r = subtree_hash(lo + k, hi, tree_size, right); !r.ok()) return r;
  node_of(left, right, out);
  return Ok();
}

Result<SumRecord> ChecksumDatabase::lookup(const std::string& module, const std::string& version, SumTree* tree) {
  auto body = fetch("/lookup/" + module + "@" + version);
  if (!body.ok()) return Result<SumRecord>(body.error());
  std::string text((const char*)body->data(), body->size());

  size_t first = text.find('\n');
  size_t sep = text.find("\n\n");
  if (first == std::string::npos || sep == std::string::npos || sep <= first)
    return Result<SumRecord>(Status::Corrupt, "the lookup answer is not shaped like a record and a signed tree");
  SumRecord rec;
  if (!parse_dec(text.substr(0, first), rec.id))
    return Result<SumRecord>(Status::Corrupt, "the lookup answer does not open with a record number");
  std::string record = text.substr(first + 1, sep + 1 - (first + 1));   // the leaf, byte for byte
  std::string note = text.substr(sep + 2);

  std::string signer;
  auto head = note_verify(note, key_, signer);
  if (!head.ok()) return Result<SumRecord>(head.error());
  auto head_lines = lines_of(*head);
  int64_t size = 0;
  if (head_lines.size() < 3 || !parse_dec(head_lines[1], size))
    return Result<SumRecord>(Status::Corrupt, "the signed tree head is not shaped like one");
  auto root = base64_decode(head_lines[2]);
  if (!root.ok() || root->size() != 32)
    return Result<SumRecord>(Status::Corrupt, "the signed tree head carries no usable root hash");
  if (rec.id >= size)
    return Result<SumRecord>(Status::Corrupt, "the record is numbered beyond the tree that was signed");

  // The signature covers the tree head and nothing else. Fold this record's
  // leaf hash up through the log's own tiles; if it does not arrive at the root
  // that was signed, the record is not in the tree and is worth nothing.
  uint8_t acc[32];
  leaf_of(ByteSpan((const uint8_t*)record.data(), record.size()), acc);
  struct Step { bool right; int64_t lo, hi; };
  std::vector<Step> path;
  {
    int64_t lo = 0, hi = size, i = rec.id;
    while (hi - lo > 1) {
      int64_t k = largest_power_of_two_below(hi - lo);
      if (i < lo + k) { path.push_back({true, lo + k, hi}); hi = lo + k; }
      else            { path.push_back({false, lo, lo + k}); lo = lo + k; }
    }
  }
  for (size_t s = path.size(); s > 0; --s) {
    const Step& step = path[s - 1];
    uint8_t sibling[32], folded[32];
    if (auto r = subtree_hash(step.lo, step.hi, size, sibling); !r.ok()) return Result<SumRecord>(r.error());
    if (step.right) node_of(acc, sibling, folded);
    else            node_of(sibling, acc, folded);
    for (int b = 0; b < 32; ++b) acc[b] = folded[b];
  }
  for (int b = 0; b < 32; ++b)
    if (acc[b] != (*root)[b])
      return Result<SumRecord>(Status::Denied,
                              module + "@" + version + ": the record does not prove into the tree " + signer +
                              " signed — this database is showing a split view, and nothing it says can be trusted");

  for (const auto& line : lines_of(record)) {
    auto f = split_ws(line);
    if (f.size() != 3) continue;
    if (f[1].size() > 7 && f[1].compare(f[1].size() - 7, 7, "/go.mod") == 0) {
      rec.mod_hash = f[2];
    } else {
      rec.module = f[0];
      rec.version = f[1];
      rec.hash = f[2];
    }
  }
  if (rec.hash.empty())
    return Result<SumRecord>(Status::Corrupt, "the record proved into the tree but names no module hash");
  if (tree) {
    tree->origin = head_lines[0];
    tree->size = size;
    tree->root = head_lines[2];
    tree->signer = signer;
    tree->tiles_read = tiles_.size();
  }
  return rec;
}

}  // namespace aios::userland
