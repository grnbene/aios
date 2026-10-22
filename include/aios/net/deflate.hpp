#pragma once
// DEFLATE (RFC 1951) with nothing linked: the encoder and decoder this tree
// carries for the bare-metal build and for any host without zlib. The
// encoder is zlib's design - LZ77 over a 32 KB window with hash chains and
// lazy matching, then per block whichever of dynamic Huffman codes, the fixed
// ones or a stored block is smallest - so its output is within a few percent
// of zlib's at the same level. The git object store (git.cpp) wraps it in a
// zlib envelope; ASQL's cold text blocks (sql_database.cpp) keep the raw
// stream.
#include <cstddef>
#include <cstdint>
#include <string>

#include "aios/core/bytes.hpp"

namespace aios::net {

// `data` as one raw deflate stream (BFINAL set on its last block) appended
// to `out`; the bytes appended. `level` is zlib's: 1-3 quick, 6 the usual
// balance, 9 smallest and slowest.
size_t deflate_raw_append(ByteSpan data, int level, ByteBuffer& out);
inline ByteBuffer deflate_raw(ByteSpan data, int level = 6) {
  ByteBuffer out;
  out.reserve(data.size() / 2 + 64);
  deflate_raw_append(data, level, out);
  return out;
}

// A raw deflate stream that must inflate to exactly `expect` bytes, into
// `out` (which holds at least that many): libdeflate or zlib where the host
// has them, the decoder below elsewhere. False on a damaged stream or one of
// another size - never a partial result passed off as the data.
bool inflate_raw_exact(ByteSpan data, uint8_t* out, size_t expect);

// The in-tree decoder alone: the stream at `whole[offset..]`, appended to
// `out`; `end_offset` (when given) comes back just past the stream's last
// byte, `expect` (when known) sizes `out` once. False, with the reason in
// `why`, on anything that does not check out.
bool inflate_raw_tree(ByteSpan whole, size_t offset, ByteBuffer& out, size_t* end_offset, size_t expect, std::string* why);

}  // namespace aios::net
