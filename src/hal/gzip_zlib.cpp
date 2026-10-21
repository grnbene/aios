// The gzip stream encoder on a host that has zlib: dynamic Huffman at level 6.
// Bare metal and hosts without zlib use the portable encoder in src/net/gzip.cpp.
#if defined(AIOS_HAVE_ZLIB)
#include "aios/net/gzip.hpp"
#include <zlib.h>
#if defined(AIOS_HAVE_LIBDEFLATE)
#include <libdeflate.h>
#endif

namespace aios::net {


// zlib's deflate at level 6 with gzip framing (windowBits 15 + 16): dynamic
// Huffman blocks and lazy matching, what nginx's gzip_comp_level 6 produces.
// The fixed-Huffman encoder above sent bootstrap.css as 24,900 bytes where
// nginx sends 21,327, and the 20k-customer JSON as 3.67 MB against 3.01 MB,
// slower as well. Still one gzip member for the whole body, built as it
// streams (see gzip.hpp for why); Z_NO_FLUSH lets zlib hold output until it
// has a block's worth, as nginx does, so a small chunk may return nothing yet.
struct GzipStreamEncoder::Impl {
  z_stream zs{};
  bool ok = false;
  Impl() { ok = deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK; }
  ~Impl() { if (ok) deflateEnd(&zs); }
  ByteBuffer run(ByteSpan data, int flush) {
    ByteBuffer out;
    if (!ok) return out;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    zs.avail_in = (uInt)data.size();
    uint8_t buf[64 * 1024];
    do {
      zs.next_out = buf;
      zs.avail_out = sizeof(buf);
      int rc = deflate(&zs, flush);
      if (rc == Z_STREAM_ERROR) { ok = false; break; }
      out.insert(out.end(), buf, buf + (sizeof(buf) - zs.avail_out));
    } while (zs.avail_out == 0 || (flush == Z_FINISH && zs.avail_in > 0));
    return out;
  }
};

GzipStreamEncoder::GzipStreamEncoder() : impl_(std::make_unique<Impl>()) {}
GzipStreamEncoder::~GzipStreamEncoder() = default;

ByteBuffer GzipStreamEncoder::feed(ByteSpan data, bool sync_flush) {
  return impl_->run(data, sync_flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
}
ByteBuffer GzipStreamEncoder::finish() { return impl_->run(ByteSpan(), Z_FINISH); }


namespace {
// One inflate stream per thread and window format, reset between objects: inflateInit2 and inflateEnd allocated and
// freed zlib's state and its 32 KB window for every object, a third of the time of a header scan over a pack.
struct Inflater {
  z_stream zs{};
  bool ready = false;
  int bits = 0;
  ~Inflater() { if (ready) inflateEnd(&zs); }
  z_stream* get(int window_bits) {
    if (ready && bits == window_bits) { if (inflateReset(&zs) == Z_OK) return &zs; inflateEnd(&zs); ready = false; }
    if (ready) { inflateEnd(&zs); ready = false; }
    zs = z_stream{};
    if (inflateInit2(&zs, window_bits) != Z_OK) return nullptr;
    ready = true;
    bits = window_bits;
    return &zs;
  }
};
z_stream* inflater(int window_bits) {
  thread_local Inflater raw, z;
  return (window_bits < 0 ? raw : z).get(window_bits);
}
}  // namespace

// One raw deflate stream (no zlib or gzip header) from the start of `data`:
// what a git object or pack entry holds. `consumed` is how many input bytes
// the stream took. False when zlib refuses it; the caller then decodes it
// itself.
bool zlib_inflate_raw(ByteSpan data, ByteBuffer* out, size_t* consumed, size_t expect) {
  z_stream* zp = inflater(-15);
  if (!zp) return false;
  z_stream& zs = *zp;
  out->clear();
  // `data` may be a whole pack with this object somewhere at its start: the
  // buffer starts small and doubles, never sized from the input.
  out->resize(expect ? expect : 16384);
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
  zs.avail_in = (uInt)std::min<size_t>(data.size(), 0xFFFFFFFFu);
  size_t have = 0;
  int rc = Z_OK;
  for (;;) {
    if (have == out->size()) out->resize(out->size() * 2);
    zs.next_out = reinterpret_cast<Bytef*>(out->data() + have);
    zs.avail_out = (uInt)std::min<size_t>(out->size() - have, 0xFFFFFFFFu);
    rc = inflate(&zs, Z_NO_FLUSH);
    have = out->size() - zs.avail_out;
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK && rc != Z_BUF_ERROR) return false;
    if (rc == Z_BUF_ERROR && zs.avail_in == 0 && zs.avail_out != 0) return false;   // truncated
  }
  out->resize(have);
  if (consumed) *consumed = (size_t)zs.total_in;
  return true;
}

// A zlib stream (header, deflate, Adler-32) from the start of `data`, the checksum checked by zlib as it inflates:
// one pass, where inflating raw and summing the output again was two. `consumed` includes the checksum.
bool zlib_inflate_known(ByteSpan data, uint8_t* out, size_t expect, size_t* consumed, size_t* got) {
#if defined(AIOS_HAVE_LIBDEFLATE)
  struct D {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    ~D() { if (d) libdeflate_free_decompressor(d); }
  };
  thread_local D dec;
  if (!dec.d) return false;
  size_t in_used = 0, out_used = 0;
  // the whole stream in one call; the Adler-32 is checked by libdeflate. A stream longer than its header said is
  // refused here (INSUFFICIENT_SPACE) and left to zlib, which reports it the way it always has.
  if (libdeflate_zlib_decompress_ex(dec.d, data.data(), data.size(), out, expect, &in_used, &out_used) != LIBDEFLATE_SUCCESS)
    return false;
  if (consumed) *consumed = in_used;
  if (got) *got = out_used;
  return true;
#else
  (void)data; (void)out; (void)expect; (void)consumed; (void)got;
  return false;
#endif
}

bool zlib_inflate_z(ByteSpan data, ByteBuffer* out, size_t* consumed, size_t expect) {
#if defined(AIOS_HAVE_LIBDEFLATE)
  if (expect) {
    out->resize(expect);
    size_t got = 0;
    if (zlib_inflate_known(data, out->data(), expect, consumed, &got)) { out->resize(got); return true; }
  }
#endif
  z_stream* zp = inflater(15);
  if (!zp) return false;
  z_stream& zs = *zp;
  // Exactly the expected size when it is known (a git object's header says it): one allocation, and a stream
  // that ends where it should needs no second call to find its end.
  out->resize(expect ? expect + 1 : 4096);
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
  zs.avail_in = (uInt)std::min<size_t>(data.size(), 0xFFFFFFFFu);
  size_t have = 0;
  for (;;) {
    if (have == out->size()) out->resize(out->size() * 2);
    zs.next_out = reinterpret_cast<Bytef*>(out->data() + have);
    zs.avail_out = (uInt)std::min<size_t>(out->size() - have, 0xFFFFFFFFu);
    const int rc = inflate(&zs, Z_NO_FLUSH);
    have = out->size() - zs.avail_out;
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK && rc != Z_BUF_ERROR) return false;
    if (rc == Z_BUF_ERROR && zs.avail_in == 0 && zs.avail_out != 0) return false;
  }
  out->resize(have);
  if (consumed) *consumed = (size_t)zs.total_in;
  return true;
}

// The first `max` bytes a zlib stream inflates to (fewer when it is shorter): a loose object's header, a delta's
// sizes. Nothing past them is inflated.
bool zlib_inflate_prefix(ByteSpan data, uint8_t* out, size_t max, size_t* got) {
  z_stream* zp = inflater(15);
  if (!zp) return false;
  z_stream& zs = *zp;
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
  zs.avail_in = (uInt)std::min<size_t>(data.size(), 0xFFFFFFFFu);
  zs.next_out = out;
  zs.avail_out = (uInt)max;
  int rc = Z_OK;
  while (zs.avail_out > 0 && rc == Z_OK) rc = inflate(&zs, Z_SYNC_FLUSH);
  *got = max - zs.avail_out;
  return rc == Z_OK || rc == Z_STREAM_END || (rc == Z_BUF_ERROR && *got > 0);
}

}  // namespace aios::net
#endif
