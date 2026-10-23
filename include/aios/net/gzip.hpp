#pragma once
// gzip (RFC 1952) response compression, using the same LZ77 hash-chain +
// fixed-Huffman approach this codebase's existing DEFLATE encoder uses
// (aios::userland::zlib_deflate, in git.cpp) rather than a new compression
// algorithm — but as an INCREMENTAL encoder, not a whole-buffer one.
//
// An earlier version of this compressed each streamed chunk as its own
// independent gzip member and relied on RFC 1952's "concatenated members
// form one valid stream" rule to make that legal. It is legal — gunzip(1)
// round-trips it correctly — but real HTTP clients don't reliably follow
// that rule for automatic Content-Encoding decompression: libcurl's
// zlib-backed auto-decoder was observed silently stopping after the first
// member, truncating the rest of the body with no error. A 200 response
// with a silently truncated body is worse than not compressing at all, so
// this encoder instead produces exactly one gzip member (one header, one
// CRC32/size trailer over the whole body) built incrementally: each feed()
// call emits a non-final DEFLATE block for that chunk, matches are allowed
// to reference up to 32KB back (including into earlier chunks, via a
// carried-over sliding window), and finish() closes the block/stream and
// appends the trailer computed over everything fed. Memory stays bounded
// to roughly one chunk plus the 32KB window, never the whole response.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"

namespace aios::net {

class GzipStreamEncoder {
 public:
  GzipStreamEncoder();
  ~GzipStreamEncoder();
  // Compresses `data` as the next slice of the body. Returns however many
  // compressed bytes are ready to send now (may be less than a 1:1 ratio
  // would suggest, or even empty, since DEFLATE output isn't chunk-aligned
  // with input — that's fine, just send whatever comes back, in order).
  // When `sync_flush` is true, forces pending output to byte-align and flush
  // immediately (Z_SYNC_FLUSH), ensuring low time-to-first-byte (TTFB).
  ByteBuffer feed(ByteSpan data, bool sync_flush = false);
  // Closes the DEFLATE stream and appends the gzip trailer (CRC-32 + total
  // size, computed incrementally over every feed() call). Call exactly
  // once, after the last feed().
  ByteBuffer finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// One complete gzip stream for a whole buffer known up front — a thin
// convenience wrapper over GzipStreamEncoder for the non-streaming case.
ByteBuffer gzip_compress_whole(ByteSpan data);

#if defined(AIOS_HAVE_ZLIB)
// A raw deflate stream through the system zlib (see gzip_zlib.cpp).
bool zlib_inflate_raw(ByteSpan data, ByteBuffer* out, size_t* consumed, size_t expect);
bool zlib_inflate_z(ByteSpan data, ByteBuffer* out, size_t* consumed, size_t expect);
bool zlib_inflate_prefix(ByteSpan data, uint8_t* out, size_t max, size_t* got);
// A zlib stream that inflates to at most `expect` bytes, into `out`, in one call (libdeflate). False where the host
// has no libdeflate or it refuses the stream: the caller then inflates with zlib.
bool zlib_inflate_known(ByteSpan data, uint8_t* out, size_t expect, size_t* consumed, size_t* got);
#endif
#if defined(AIOS_HAVE_LIBDEFLATE)
// The host's libdeflate (src/hal/gzip_zlib.cpp): a raw deflate stream that inflates to exactly `expect` bytes; and
// `data` compressed raw at `level` into `out`, its size, or 0 when it does not fit in `cap`.
bool host_inflate_raw_exact(ByteSpan data, uint8_t* out, size_t expect);
size_t host_deflate_raw(ByteSpan data, uint8_t* out, size_t cap, int level);
#endif

// Picks gzip when the client's Accept-Encoding header allows it and `mime`
// is a type worth compressing (text/html, application/json, ...); images,
// video, fonts, and already-compressed formats are left alone. Empty string
// means "send uncompressed".
std::string negotiate_content_encoding(const std::string& accept_encoding_header, const std::string& mime);

}  // namespace aios::net
