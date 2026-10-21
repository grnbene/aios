#pragma once
// An append-only Merkle log as Certificate Transparency keeps one (RFC 9162 section 2.1): leaf hash
// SHA-256(0x00 || data), node hash SHA-256(0x01 || left || right), the tree over the first n leaves split at the
// largest power of two below n. A log that publishes its root and size (a tree head) can prove two things to anyone
// holding only tree heads: that an entry is in it (inclusion), and that a later tree extends an earlier one without
// rewriting it (consistency). Whoever keeps the tree heads they were shown can therefore catch a log, its own signer
// included, that rewrote or dropped history.
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace aios::crypto {

using Hash32 = std::array<uint8_t, 32>;

Hash32 merkle_leaf_hash(std::string_view data);
Hash32 merkle_node_hash(const Hash32& left, const Hash32& right);
// The root of the tree over leaves[0, n) (n <= leaves.size()); SHA-256 of nothing for n = 0.
Hash32 merkle_root(const std::vector<Hash32>& leaves, uint64_t n);
// The audit path for leaf `index` in the tree of size n (index < n).
std::vector<Hash32> merkle_inclusion_proof(const std::vector<Hash32>& leaves, uint64_t index, uint64_t n);
// The proof that the tree of size m is a prefix of the tree of size n (0 < m <= n).
std::vector<Hash32> merkle_consistency_proof(const std::vector<Hash32>& leaves, uint64_t m, uint64_t n);

// Checks, from hashes alone (RFC 9162 2.1.3.2 and 2.1.4.2).
bool merkle_verify_inclusion(const Hash32& leaf, uint64_t index, uint64_t n, const std::vector<Hash32>& proof, const Hash32& root);
bool merkle_verify_consistency(uint64_t m, uint64_t n, const Hash32& root_m, const Hash32& root_n, const std::vector<Hash32>& proof);

}  // namespace aios::crypto
