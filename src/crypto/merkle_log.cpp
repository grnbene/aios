#include "aios/crypto/merkle_log.hpp"

#include "aios/crypto/sha256.hpp"

namespace aios::crypto {

namespace {
// The largest power of two smaller than n (n > 1).
uint64_t split_point(uint64_t n) {
  uint64_t k = 1;
  while (k << 1 < n) k <<= 1;
  return k;
}

Hash32 subtree(const std::vector<Hash32>& leaves, uint64_t lo, uint64_t hi) {
  if (hi - lo == 1) return leaves[lo];
  uint64_t k = split_point(hi - lo);
  return merkle_node_hash(subtree(leaves, lo, lo + k), subtree(leaves, lo + k, hi));
}

void path(const std::vector<Hash32>& leaves, uint64_t m, uint64_t lo, uint64_t hi, std::vector<Hash32>& out) {
  if (hi - lo <= 1) return;
  uint64_t k = split_point(hi - lo);
  if (m < k) {
    path(leaves, m, lo, lo + k, out);
    out.push_back(subtree(leaves, lo + k, hi));
  } else {
    path(leaves, m - k, lo + k, hi, out);
    out.push_back(subtree(leaves, lo, lo + k));
  }
}

// RFC 9162 2.1.4.1 SUBPROOF(m, D[lo:hi], b)
void subproof(const std::vector<Hash32>& leaves, uint64_t m, uint64_t lo, uint64_t hi, bool b, std::vector<Hash32>& out) {
  uint64_t n = hi - lo;
  if (m == n) {
    if (!b) out.push_back(subtree(leaves, lo, hi));
    return;
  }
  uint64_t k = split_point(n);
  if (m <= k) {
    subproof(leaves, m, lo, lo + k, b, out);
    out.push_back(subtree(leaves, lo + k, hi));
  } else {
    subproof(leaves, m - k, lo + k, hi, false, out);
    out.push_back(subtree(leaves, lo, lo + k));
  }
}
}  // namespace

Hash32 merkle_leaf_hash(std::string_view data) {
  std::vector<uint8_t> buf;
  buf.reserve(data.size() + 1);
  buf.push_back(0);
  buf.insert(buf.end(), data.begin(), data.end());
  return sha256(ByteSpan(buf.data(), buf.size()));
}

Hash32 merkle_node_hash(const Hash32& left, const Hash32& right) {
  uint8_t buf[65];
  buf[0] = 1;
  for (int i = 0; i < 32; ++i) { buf[1 + i] = left[i]; buf[33 + i] = right[i]; }
  return sha256(ByteSpan(buf, sizeof buf));
}

Hash32 merkle_root(const std::vector<Hash32>& leaves, uint64_t n) {
  if (n == 0) return sha256(ByteSpan());
  return subtree(leaves, 0, n);
}

std::vector<Hash32> merkle_inclusion_proof(const std::vector<Hash32>& leaves, uint64_t index, uint64_t n) {
  std::vector<Hash32> out;
  if (index < n && n <= leaves.size()) path(leaves, index, 0, n, out);
  return out;
}

std::vector<Hash32> merkle_consistency_proof(const std::vector<Hash32>& leaves, uint64_t m, uint64_t n) {
  std::vector<Hash32> out;
  if (m > 0 && m <= n && n <= leaves.size()) subproof(leaves, m, 0, n, true, out);
  return out;
}

bool merkle_verify_inclusion(const Hash32& leaf, uint64_t index, uint64_t n, const std::vector<Hash32>& proof, const Hash32& root) {
  if (index >= n) return false;
  uint64_t fn = index, sn = n - 1;
  Hash32 r = leaf;
  for (const auto& p : proof) {
    if (sn == 0) return false;
    if ((fn & 1) || fn == sn) {
      r = merkle_node_hash(p, r);
      if (!(fn & 1)) while (fn != 0 && !(fn & 1)) { fn >>= 1; sn >>= 1; }
    } else {
      r = merkle_node_hash(r, p);
    }
    fn >>= 1;
    sn >>= 1;
  }
  return sn == 0 && r == root;
}

bool merkle_verify_consistency(uint64_t m, uint64_t n, const Hash32& root_m, const Hash32& root_n, const std::vector<Hash32>& proof) {
  if (m == 0 || m > n) return false;
  if (m == n) return proof.empty() && root_m == root_n;
  if (proof.empty()) return false;
  std::vector<Hash32> p = proof;
  // m a power of two: the first tree's root is the first node of the path
  if ((m & (m - 1)) == 0) p.insert(p.begin(), root_m);
  uint64_t fn = m - 1, sn = n - 1;
  while (fn & 1) { fn >>= 1; sn >>= 1; }
  Hash32 fr = p[0], sr = p[0];
  for (size_t i = 1; i < p.size(); ++i) {
    if (sn == 0) return false;
    if ((fn & 1) || fn == sn) {
      fr = merkle_node_hash(p[i], fr);
      sr = merkle_node_hash(p[i], sr);
      if (!(fn & 1)) while (fn != 0 && !(fn & 1)) { fn >>= 1; sn >>= 1; }
    } else {
      sr = merkle_node_hash(sr, p[i]);
    }
    fn >>= 1;
    sn >>= 1;
  }
  return fr == root_m && sr == root_n && sn == 0;
}

}  // namespace aios::crypto
