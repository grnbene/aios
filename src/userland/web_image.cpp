// PNG, GIF and baseline JPEG, decoded here.
//
// The kernel already had to grow a real inflater to read git's loose objects
// (`zlib_inflate`), which is the expensive half of PNG; what is left is the
// chunk walk, the five scanline filters and the colour types. GIF brings its
// own LZW, which is about forty lines.
//
// Everything is bounds-checked against the buffer a server sent, because that
// is exactly the kind of input that should never be trusted in ring 0: a
// truncated IDAT, an IHDR claiming 40000x40000, a palette index past the end
// of the palette. Each of those comes back as an error, and the page draws the
// image's alt text instead.
#include <cstdlib>
#include <cmath>
#include <cstring>

#include "aios/net/deflate.hpp"
#include "aios/userland/web.hpp"

namespace aios::userland::web {
namespace {

// A picture bigger than this is not going on a 1080p desktop, and allocating
// for it is how a page takes the machine down.
constexpr uint32_t kMaxDim = 8192;
constexpr uint64_t kMaxPixels = 24u * 1024u * 1024u;

uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) {
    s.insert(s.begin(), (char)('0' + v % 10));
    v /= 10;
  }
  return s;
}

// ------------------------------------------------------------------- PNG --

int png_channels(uint8_t colour_type) {
  switch (colour_type) {
    case 0: return 1;   // greyscale
    case 2: return 3;   // truecolour
    case 3: return 1;   // palette index
    case 4: return 2;   // greyscale + alpha
    case 6: return 4;   // truecolour + alpha
    default: return 0;
  }
}

int paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

Result<Bitmap> decode_png(ByteSpan bytes) {
  if (bytes.size() < 8) return Result<Bitmap>(Status::Invalid, "png: too short to be one");
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (std::memcmp(bytes.data(), kSig, 8) != 0) return Result<Bitmap>(Status::Invalid, "png: bad signature");

  uint32_t width = 0, height = 0;
  uint8_t depth = 0, colour_type = 0, interlace = 0;
  ByteBuffer idat;
  uint32_t palette[256];
  size_t palette_count = 0;
  bool have_trns = false;
  uint8_t trns_index = 0;
  uint16_t trns_grey = 0;
  uint16_t trns_rgb[3] = {0, 0, 0};
  bool have_ihdr = false;

  size_t at = 8;
  while (at + 8 <= bytes.size()) {
    uint32_t len = be32(bytes.data() + at);
    if (len > bytes.size()) return Result<Bitmap>(Status::Invalid, "png: a chunk claims more bytes than arrived");
    const uint8_t* type = bytes.data() + at + 4;
    size_t data_at = at + 8;
    if (data_at + len + 4 > bytes.size()) {
      // Truncated. What is already in hand may still be a whole image if IEND
      // is what went missing, so stop reading rather than refusing outright.
      break;
    }
    const uint8_t* data = bytes.data() + data_at;

    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (len < 13) return Result<Bitmap>(Status::Invalid, "png: short IHDR");
      width = be32(data);
      height = be32(data + 4);
      depth = data[8];
      colour_type = data[9];
      interlace = data[12];
      have_ihdr = true;
      if (!width || !height) return Result<Bitmap>(Status::Invalid, "png: zero-sized");
      if (width > kMaxDim || height > kMaxDim)
        return Result<Bitmap>(Status::Invalid, "png: " + dec(width) + "x" + dec(height) + " is larger than this machine will decode");
      if ((uint64_t)width * height > kMaxPixels)
        return Result<Bitmap>(Status::Invalid, "png: too many pixels to decode here");
      if (!png_channels(colour_type)) return Result<Bitmap>(Status::Invalid, "png: colour type " + dec(colour_type));
      if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
        return Result<Bitmap>(Status::Invalid, "png: bit depth " + dec(depth));
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      palette_count = len / 3;
      if (palette_count > 256) palette_count = 256;
      for (size_t i = 0; i < palette_count; ++i)
        palette[i] = argb(255, data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      have_trns = true;
      if (colour_type == 3) {
        for (size_t i = 0; i < len && i < palette_count; ++i)
          palette[i] = (palette[i] & 0x00FFFFFF) | ((uint32_t)data[i] << 24);
        trns_index = 0;
      } else if (colour_type == 0 && len >= 2) {
        trns_grey = (uint16_t)((data[0] << 8) | data[1]);
      } else if (colour_type == 2 && len >= 6) {
        for (int i = 0; i < 3; ++i) trns_rgb[i] = (uint16_t)((data[i * 2] << 8) | data[i * 2 + 1]);
      }
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), data, data + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    at = data_at + len + 4;
  }
  (void)trns_index;

  if (!have_ihdr) return Result<Bitmap>(Status::Invalid, "png: no IHDR");
  if (idat.empty()) return Result<Bitmap>(Status::Invalid, "png: no image data");
  if (colour_type == 3 && !palette_count) return Result<Bitmap>(Status::Invalid, "png: palette image with no palette");

  auto raw_r = net::zlib_inflate(ByteSpan(idat.data(), idat.size()));
  if (!raw_r.ok()) return Result<Bitmap>(Status::Invalid, std::string("png: ") + raw_r.error().message);
  const ByteBuffer& raw = *raw_r;

  const int channels = png_channels(colour_type);
  const int bits_per_pixel = channels * depth;
  const size_t bpp_bytes = (size_t)(bits_per_pixel + 7) / 8;   // filter distance, min 1

  // Adam7 stores the picture as seven smaller sub-images, each filtered on its
  // own; a plain PNG is the one pass that starts at 0,0 and steps by 1.
  struct Pass { uint32_t x0, y0, dx, dy; };
  static const Pass kPlain[1] = {{0, 0, 1, 1}};
  static const Pass kAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                 {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
  const Pass* passes = interlace ? kAdam7 : kPlain;
  const int pass_count = interlace ? 7 : 1;

  Bitmap out;
  out.w = (int)width;
  out.h = (int)height;
  out.pixels.assign((size_t)width * height, 0);

  size_t raw_at = 0;
  for (int pi = 0; pi < pass_count; ++pi) {
    const Pass& ps = passes[pi];
    if (ps.x0 >= width || ps.y0 >= height) continue;
    const uint32_t pw = (width - ps.x0 + ps.dx - 1) / ps.dx;
    const uint32_t ph = (height - ps.y0 + ps.dy - 1) / ps.dy;
    const size_t stride = ((size_t)pw * (size_t)bits_per_pixel + 7) / 8;
    if (raw.size() < raw_at + (stride + 1) * (size_t)ph)
      return Result<Bitmap>(Status::Invalid, "png: image data is " + dec(raw.size()) + " bytes, short of what the header describes");
    const uint8_t* praw = raw.data() + raw_at;
    raw_at += (stride + 1) * (size_t)ph;

    // Unfilter in place into one contiguous buffer of scanlines.
    ByteBuffer lines(stride * (size_t)ph, 0);
    for (uint32_t y = 0; y < ph; ++y) {
      uint8_t filter = praw[(stride + 1) * y];
      const uint8_t* src = praw + (stride + 1) * y + 1;
      uint8_t* cur = lines.data() + stride * y;
      const uint8_t* prev = y ? lines.data() + stride * (y - 1) : nullptr;
      for (size_t i = 0; i < stride; ++i) {
        int a = i >= bpp_bytes ? cur[i - bpp_bytes] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= bpp_bytes) ? prev[i - bpp_bytes] : 0;
        int x = src[i];
        switch (filter) {
          case 0: cur[i] = (uint8_t)x; break;
          case 1: cur[i] = (uint8_t)(x + a); break;
          case 2: cur[i] = (uint8_t)(x + b); break;
          case 3: cur[i] = (uint8_t)(x + ((a + b) >> 1)); break;
          case 4: cur[i] = (uint8_t)(x + paeth(a, b, c)); break;
          default: return Result<Bitmap>(Status::Invalid, "png: filter type " + dec(filter));
        }
      }
    }

    // Pull one sample out of a scanline, whatever the bit depth is, scaled to
    // 0..255 (or left as the raw index for a palette image).
    auto sample = [&](const uint8_t* line, uint32_t index) -> uint32_t {
      if (depth == 8) return line[index];
      if (depth == 16) return line[index * 2];              // the high byte is the picture
      uint32_t per_byte = 8u / depth;
      uint32_t byte = line[index / per_byte];
      uint32_t shift = (per_byte - 1 - (index % per_byte)) * depth;
      uint32_t mask = (1u << depth) - 1u;
      return (byte >> shift) & mask;
    };
    const uint32_t max_value = depth >= 8 ? 255u : (1u << depth) - 1u;

    for (uint32_t y = 0; y < ph; ++y) {
      const uint8_t* line = lines.data() + stride * y;
      for (uint32_t x = 0; x < pw; ++x) {
        uint32_t px = 0;
        switch (colour_type) {
          case 0: {
            uint32_t g = sample(line, x);
            uint8_t v = (uint8_t)(max_value == 255 ? g : g * 255 / max_value);
            uint8_t a = (have_trns && g == trns_grey) ? 0 : 255;
            px = argb(a, v, v, v);
            break;
          }
          case 2: {
            uint32_t r = sample(line, x * 3), g = sample(line, x * 3 + 1), b = sample(line, x * 3 + 2);
            uint8_t a = (have_trns && r == trns_rgb[0] && g == trns_rgb[1] && b == trns_rgb[2]) ? 0 : 255;
            px = argb(a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
            break;
          }
          case 3: {
            uint32_t i = sample(line, x);
            px = i < palette_count ? palette[i] : 0xFF000000;
            break;
          }
          case 4: {
            uint32_t g = sample(line, x * 2), a = sample(line, x * 2 + 1);
            uint8_t v = (uint8_t)(max_value == 255 ? g : g * 255 / max_value);
            px = argb((uint8_t)a, v, v, v);
            break;
          }
          case 6: {
            uint32_t r = sample(line, x * 4), g = sample(line, x * 4 + 1), b = sample(line, x * 4 + 2),
                     a = sample(line, x * 4 + 3);
            px = argb((uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
            break;
          }
          default: break;
        }
        out.pixels[(size_t)(ps.y0 + y * ps.dy) * width + (ps.x0 + x * ps.dx)] = px;
      }
    }
  }
  return out;
}

// ------------------------------------------------------------------- GIF --

// One frame is enough: a page's logos and icons are single-frame GIFs, and an
// animation's first frame is a picture of the animation.
Result<Bitmap> decode_gif(ByteSpan b) {
  if (b.size() < 13) return Result<Bitmap>(Status::Invalid, "gif: too short");
  if (std::memcmp(b.data(), "GIF87a", 6) != 0 && std::memcmp(b.data(), "GIF89a", 6) != 0)
    return Result<Bitmap>(Status::Invalid, "gif: bad signature");

  size_t at = 6;
  uint32_t screen_w = (uint32_t)(b[at] | (b[at + 1] << 8));
  uint32_t screen_h = (uint32_t)(b[at + 2] | (b[at + 3] << 8));
  uint8_t flags = b[at + 4];
  at += 7;
  if (!screen_w || !screen_h || screen_w > kMaxDim || screen_h > kMaxDim)
    return Result<Bitmap>(Status::Invalid, "gif: " + dec(screen_w) + "x" + dec(screen_h));

  uint32_t global[256];
  size_t global_count = 0;
  if (flags & 0x80) {
    global_count = (size_t)1 << ((flags & 0x07) + 1);
    if (at + global_count * 3 > b.size()) return Result<Bitmap>(Status::Invalid, "gif: truncated colour table");
    for (size_t i = 0; i < global_count; ++i)
      global[i] = argb(255, b[at + i * 3], b[at + i * 3 + 1], b[at + i * 3 + 2]);
    at += global_count * 3;
  }

  int transparent = -1;

  for (;;) {
    if (at >= b.size()) return Result<Bitmap>(Status::Invalid, "gif: ran out before an image");
    uint8_t block = b[at++];
    if (block == 0x3B) return Result<Bitmap>(Status::Invalid, "gif: no image in the file");
    if (block == 0x21) {
      if (at >= b.size()) break;
      uint8_t label = b[at++];
      // A graphic control extension carries the transparent index.
      bool gce = label == 0xF9;
      bool first_sub = true;
      while (at < b.size()) {
        uint8_t n = b[at++];
        if (!n) break;
        if (at + n > b.size()) return Result<Bitmap>(Status::Invalid, "gif: truncated extension");
        if (gce && first_sub && n >= 4 && (b[at] & 0x01)) transparent = b[at + 3];
        first_sub = false;
        at += n;
      }
      continue;
    }
    if (block != 0x2C) return Result<Bitmap>(Status::Invalid, "gif: unexpected block");

    if (at + 9 > b.size()) return Result<Bitmap>(Status::Invalid, "gif: truncated image descriptor");
    uint32_t iw = (uint32_t)(b[at + 4] | (b[at + 5] << 8));
    uint32_t ih = (uint32_t)(b[at + 6] | (b[at + 7] << 8));
    uint8_t iflags = b[at + 8];
    at += 9;
    if (!iw || !ih || iw > kMaxDim || ih > kMaxDim) return Result<Bitmap>(Status::Invalid, "gif: bad frame size");
    if ((uint64_t)iw * ih > kMaxPixels) return Result<Bitmap>(Status::Invalid, "gif: frame too large");

    uint32_t local[256];
    size_t local_count = 0;
    if (iflags & 0x80) {
      local_count = (size_t)1 << ((iflags & 0x07) + 1);
      if (at + local_count * 3 > b.size()) return Result<Bitmap>(Status::Invalid, "gif: truncated local table");
      for (size_t i = 0; i < local_count; ++i)
        local[i] = argb(255, b[at + i * 3], b[at + i * 3 + 1], b[at + i * 3 + 2]);
      at += local_count * 3;
    }
    const uint32_t* table = local_count ? local : global;
    size_t table_count = local_count ? local_count : global_count;
    if (!table_count) return Result<Bitmap>(Status::Invalid, "gif: no colour table");
    bool interlaced = (iflags & 0x40) != 0;

    if (at >= b.size()) return Result<Bitmap>(Status::Invalid, "gif: no LZW data");
    uint8_t min_code = b[at++];
    if (min_code < 2 || min_code > 11) return Result<Bitmap>(Status::Invalid, "gif: LZW code size " + dec(min_code));

    // Gather the sub-blocks into one bitstream.
    ByteBuffer lzw;
    while (at < b.size()) {
      uint8_t n = b[at++];
      if (!n) break;
      if (at + n > b.size()) break;
      lzw.insert(lzw.end(), b.data() + at, b.data() + at + n);
      at += n;
    }

    // LZW, the GIF variant: variable-width codes, a clear code and an end
    // code, and the dictionary grows one entry per emitted code.
    const uint16_t clear_code = (uint16_t)(1u << min_code);
    const uint16_t end_code = (uint16_t)(clear_code + 1);
    std::vector<uint16_t> prefix(4096, 0);
    std::vector<uint8_t> suffix(4096, 0);
    for (uint16_t i = 0; i < clear_code; ++i) suffix[i] = (uint8_t)i;
    uint16_t next_code = (uint16_t)(end_code + 1);
    int code_size = min_code + 1;
    int prev = -1;

    std::vector<uint8_t> indices;
    indices.reserve((size_t)iw * ih);
    uint32_t bit = 0;
    std::vector<uint8_t> run;

    auto read_code = [&](uint16_t& out) -> bool {
      uint32_t value = 0;
      for (int i = 0; i < code_size; ++i) {
        size_t byte = (bit + (uint32_t)i) >> 3;
        if (byte >= lzw.size()) return false;
        uint32_t b_bit = (lzw[byte] >> ((bit + (uint32_t)i) & 7)) & 1u;
        value |= b_bit << i;
      }
      bit += (uint32_t)code_size;
      out = (uint16_t)value;
      return true;
    };

    uint16_t code = 0;
    while (indices.size() < (size_t)iw * ih && read_code(code)) {
      if (code == clear_code) {
        next_code = (uint16_t)(end_code + 1);
        code_size = min_code + 1;
        prev = -1;
        continue;
      }
      if (code == end_code) break;

      // Walk the dictionary back to the root, then emit it forwards.
      run.clear();
      uint16_t walk = code;
      if (code == next_code && prev >= 0) {
        // The "code not yet in the dictionary" case: it is the previous
        // string plus its own first byte.
        walk = (uint16_t)prev;
      } else if (code > next_code || (code > end_code && code >= next_code)) {
        break;   // a corrupt stream, not something to guess at
      }
      uint16_t guard = 0;
      while (walk >= clear_code && guard++ < 4096) {
        run.push_back(suffix[walk]);
        walk = prefix[walk];
      }
      run.push_back(suffix[walk]);
      uint8_t first = run.back();
      for (size_t i = run.size(); i > 0; --i) indices.push_back(run[i - 1]);
      if (code == next_code && prev >= 0) indices.push_back(first);

      if (prev >= 0 && next_code < 4096) {
        prefix[next_code] = (uint16_t)prev;
        suffix[next_code] = first;
        ++next_code;
        if (next_code == (1u << code_size) && code_size < 12) ++code_size;
      }
      prev = code;
    }

    Bitmap out;
    out.w = (int)iw;
    out.h = (int)ih;
    out.pixels.assign((size_t)iw * ih, 0);
    // GIF interlace: four passes down the image, which is the only reason a
    // deinterlace step exists here at all.
    static const int kStart[4] = {0, 4, 2, 1};
    static const int kStep[4] = {8, 8, 4, 2};
    size_t src = 0;
    if (interlaced) {
      for (int pass = 0; pass < 4; ++pass) {
        for (uint32_t y = (uint32_t)kStart[pass]; y < ih; y += (uint32_t)kStep[pass]) {
          for (uint32_t x = 0; x < iw; ++x) {
            if (src >= indices.size()) break;
            uint8_t i = indices[src++];
            out.pixels[(size_t)y * iw + x] =
                (transparent >= 0 && i == (uint8_t)transparent) ? 0u : (i < table_count ? table[i] : 0xFF000000u);
          }
        }
      }
    } else {
      for (size_t i = 0; i < out.pixels.size() && i < indices.size(); ++i) {
        uint8_t v = indices[i];
        out.pixels[i] = (transparent >= 0 && v == (uint8_t)transparent) ? 0u
                                                                       : (v < table_count ? table[v] : 0xFF000000u);
      }
    }
    return out;
  }
  return Result<Bitmap>(Status::Invalid, "gif: no frame decoded");
}

// ------------------------------------------------------------------ JPEG --
//
// Baseline sequential, which is what a photograph on the web is. Every stage is
// here because there is nowhere else to get it: a Huffman decoder over a
// bit-stuffed entropy stream, dequantisation, a separable inverse DCT, chroma
// upsampling and the YCbCr conversion.
//
// Progressive JPEG keeps every block's coefficients across scans and runs the
// IDCT once at the end (jpeg_prog_scan), so it shares the colour stage below.
//
// The same rule as PNG and GIF applies to every read: a file that a server sent
// is not to be trusted with a pointer. Every table index, every component id
// and every MCU is checked against what the headers actually declared.

constexpr int kZigZag[64] = {
   0,  1,  8, 16,  9,  2,  3, 10,
  17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63,
};

// A canonical Huffman table, in the form the standard's DECODE procedure wants:
// the smallest code of each length, the largest, and where that length's values
// start in the value list.
struct JpegHuff {
  bool present = false;
  int mincode[17] = {0};
  int maxcode[18] = {0};
  int valptr[17] = {0};
  uint8_t vals[256] = {0};

  bool build(const uint8_t* counts, const uint8_t* values, size_t nvals) {
    if (nvals > 256) return false;
    for (size_t i = 0; i < nvals; ++i) vals[i] = values[i];
    int code = 0, k = 0;
    for (int len = 1; len <= 16; ++len) {
      valptr[len] = k;
      mincode[len] = code;
      code += counts[len - 1];
      k += counts[len - 1];
      maxcode[len] = code - 1;
      if (counts[len - 1] == 0) maxcode[len] = -1;
      code <<= 1;
      if (k > (int)nvals) return false;
    }
    maxcode[17] = 0x7FFFFFFF;
    present = true;
    return true;
  }
};

// The entropy-coded stream: MSB-first bits, with an 0xFF byte followed by 0x00
// meaning a literal 0xFF, and any other marker ending the run.
struct JpegBits {
  const uint8_t* p;
  size_t n;
  size_t at;
  uint32_t buf = 0;
  int have = 0;
  bool hit_marker = false;

  JpegBits(const uint8_t* data, size_t len, size_t start) : p(data), n(len), at(start) {}

  int next_bit() {
    if (have == 0) {
      if (at >= n) { hit_marker = true; return 0; }
      uint8_t b = p[at++];
      if (b == 0xFF) {
        if (at < n && p[at] == 0x00) ++at;          // stuffed literal
        else { hit_marker = true; at = at ? at - 1 : 0; return 0; }
      }
      buf = b;
      have = 8;
    }
    --have;
    return (int)((buf >> have) & 1u);
  }

  int receive(int bits) {
    int v = 0;
    for (int i = 0; i < bits; ++i) v = (v << 1) | next_bit();
    return v;
  }

  // The standard's EXTEND: a `bits`-long value whose top bit is 0 is negative.
  static int extend(int v, int bits) {
    if (!bits) return 0;
    return v < (1 << (bits - 1)) ? v - (1 << bits) + 1 : v;
  }

  int decode(const JpegHuff& h) {
    int code = next_bit();
    int len = 1;
    while (len <= 16 && code > h.maxcode[len]) {
      code = (code << 1) | next_bit();
      ++len;
    }
    if (len > 16) return -1;
    const int idx = h.valptr[len] + code - h.mincode[len];
    if (idx < 0 || idx >= 256) return -1;
    return h.vals[idx];
  }

  void restart() {
    have = 0;
    hit_marker = false;
    // Step over the RSTn marker itself, and any fill bytes before it.
    while (at + 1 < n && p[at] == 0xFF && p[at + 1] == 0xFF) ++at;
    if (at + 1 < n && p[at] == 0xFF && p[at + 1] >= 0xD0 && p[at + 1] <= 0xD7) at += 2;
  }
};

// Separable inverse DCT. Two passes of the 1-D transform over an 8x8 block,
// which is 1024 multiplies instead of the 4096 the definition asks for, and
// close enough to the fast integer ones for a picture on a page.
void jpeg_idct(const int* in, const uint16_t* quant, uint8_t* out, int out_stride) {
  static float cosines[8][8];
  static bool built = false;
  if (!built) {
    for (int x = 0; x < 8; ++x)
      for (int u = 0; u < 8; ++u) {
        // cos((2x+1) u pi / 16), with the 1/sqrt(2) folded into u == 0.
        const float angle = (float)((2 * x + 1) * u) * 3.14159265358979f / 16.0f;
        float c = 1.0f;
        // A small cosine by series is not worth it here; std::cos is available
        // in both builds because the compositor already uses it.
        c = cosf(angle);
        cosines[x][u] = (u == 0 ? 0.353553390593f : 0.5f) * c;
      }
    built = true;
  }

  float tmp[64];
  // Rows: each of the 8 rows of coefficients becomes 8 samples.
  for (int v = 0; v < 8; ++v) {
    for (int x = 0; x < 8; ++x) {
      float sum = 0;
      for (int u = 0; u < 8; ++u) {
        const int coef = in[v * 8 + u] * (int)quant[v * 8 + u];
        if (coef) sum += (float)coef * cosines[x][u];
      }
      tmp[v * 8 + x] = sum;
    }
  }
  // Columns.
  for (int x = 0; x < 8; ++x) {
    for (int y = 0; y < 8; ++y) {
      float sum = 0;
      for (int v = 0; v < 8; ++v) sum += tmp[v * 8 + x] * cosines[y][v];
      int s = (int)(sum + 128.5f);
      if (s < 0) s = 0;
      if (s > 255) s = 255;
      out[y * out_stride + x] = (uint8_t)s;
    }
  }
}

struct JpegComponent {
  int id = 0, h = 1, v = 1, tq = 0;
  int td = 0, ta = 0;
  int dc_pred = 0;
  int px_w = 0, px_h = 0;          // this component's own plane
  std::vector<uint8_t> plane;
  int bw = 0, bh = 0;              // progressive: blocks across and down, padded to whole MCUs
  std::vector<int16_t> coef;       // progressive: every block's coefficients, natural order, kept across scans
};

// One progressive scan (ITU T.81 G.1.2): it adds to the coefficients already
// in `coef` -- the DC bits or a band of AC bits of a block, first approximation
// or one more bit of refinement -- so the picture only exists once every scan
// has been folded in. Returns false when the entropy data stops making sense;
// what was gathered so far is still a usable (blurrier) picture.
bool jpeg_prog_scan(JpegBits& bits, JpegComponent** sc, int ns, int ss, int se, int ah, int al,
                    int restart_interval, int hmax, int vmax, uint32_t width, uint32_t height, int mcus_x,
                    int mcus_y, const JpegHuff* dc_tables, const JpegHuff* ac_tables) {
  int eobrun = 0;
  for (int i = 0; i < ns; ++i) sc[i]->dc_pred = 0;

  // A scan over one component walks that component's own blocks, not the
  // MCU grid (which is padded for the other components' sampling).
  int units_x = mcus_x, units_y = mcus_y;
  if (ns == 1) {
    const JpegComponent& c = *sc[0];
    const uint32_t cw = (width * (uint32_t)c.h + (uint32_t)hmax - 1) / (uint32_t)hmax;
    const uint32_t ch = (height * (uint32_t)c.v + (uint32_t)vmax - 1) / (uint32_t)vmax;
    units_x = (int)((cw + 7) / 8);
    units_y = (int)((ch + 7) / 8);
  }

  static const auto& zz = kZigZag;
  bool ok = true;
  auto block = [&](JpegComponent& c, int bx, int by) {
    if (bx >= c.bw || by >= c.bh) return;
    int16_t* blk = &c.coef[((size_t)by * (size_t)c.bw + (size_t)bx) * 64];
    if (ss == 0) {                                   // DC scan
      if (ah == 0) {
        const int t = bits.decode(dc_tables[c.td]);
        if (t < 0 || t > 15) { ok = false; return; }
        c.dc_pred += JpegBits::extend(bits.receive(t), t);
        blk[0] = (int16_t)(c.dc_pred * (1 << al));
      } else if (bits.next_bit()) {
        blk[0] = (int16_t)(blk[0] | (1 << al));
      }
      return;
    }
    const JpegHuff& ht = ac_tables[c.ta];
    if (ah == 0) {                                   // AC, first approximation
      if (eobrun > 0) { --eobrun; return; }
      for (int k = ss; k <= se;) {
        const int rs = bits.decode(ht);
        if (rs < 0) { ok = false; return; }
        const int r = rs >> 4, sz = rs & 0x0F;
        if (sz == 0) {
          if (r < 15) {
            eobrun = (1 << r) - 1;
            if (r) eobrun += bits.receive(r);
            return;
          }
          k += 16;
          continue;
        }
        k += r;
        if (k > 63) return;
        blk[zz[k]] = (int16_t)(JpegBits::extend(bits.receive(sz), sz) * (1 << al));
        ++k;
      }
      return;
    }
    // AC refinement: one more bit for coefficients already non-zero, and new
    // +-1 coefficients dropped into the zero runs between them.
    const int p1 = 1 << al, m1 = -(1 << al);
    int k = ss;
    if (eobrun == 0) {
      for (; k <= se; ++k) {
        const int rs = bits.decode(ht);
        if (rs < 0) { ok = false; return; }
        int r = rs >> 4;
        const int sz = rs & 0x0F;
        int val = 0;
        if (sz) {
          val = bits.next_bit() ? p1 : m1;
        } else if (r != 15) {
          eobrun = 1 << r;
          if (r) eobrun += bits.receive(r);
          break;
        }
        do {
          int16_t& cf = blk[zz[k]];
          if (cf != 0) {
            if (bits.next_bit() && (cf & p1) == 0) cf = (int16_t)(cf + (cf >= 0 ? p1 : m1));
          } else if (--r < 0) {
            break;
          }
          ++k;
        } while (k <= se);
        if (val && k <= se) blk[zz[k]] = (int16_t)val;
      }
    }
    if (eobrun > 0) {
      for (; k <= se; ++k) {
        int16_t& cf = blk[zz[k]];
        if (cf != 0 && bits.next_bit() && (cf & p1) == 0) cf = (int16_t)(cf + (cf >= 0 ? p1 : m1));
      }
      --eobrun;
    }
  };

  int since_restart = 0;
  for (int uy = 0; uy < units_y && ok; ++uy) {
    for (int ux = 0; ux < units_x && ok; ++ux) {
      if (restart_interval && since_restart == restart_interval) {
        bits.restart();
        for (int i = 0; i < ns; ++i) sc[i]->dc_pred = 0;
        eobrun = 0;
        since_restart = 0;
      }
      ++since_restart;
      if (ns == 1) {
        block(*sc[0], ux, uy);
      } else {
        for (int i = 0; i < ns; ++i)
          for (int by = 0; by < sc[i]->v; ++by)
            for (int bx = 0; bx < sc[i]->h; ++bx) block(*sc[i], ux * sc[i]->h + bx, uy * sc[i]->v + by);
      }
    }
  }
  return ok;
}

Result<Bitmap> decode_jpeg(ByteSpan bytes) {
  const uint8_t* p = bytes.data();
  const size_t n = bytes.size();
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return Result<Bitmap>(Status::Invalid, "jpeg: bad signature");

  uint16_t quant[4][64];
  std::memset(quant, 0, sizeof(quant));
  bool quant_set[4] = {false, false, false, false};
  JpegHuff dc_tables[4], ac_tables[4];
  std::vector<JpegComponent> comps;
  uint32_t width = 0, height = 0;
  int restart_interval = 0;
  size_t at = 2;
  bool progressive = false, any_scan = false;

  // Planes -> RGB, shared by the sequential and progressive paths.
  auto build_bitmap = [&](int hmax, int vmax) -> Bitmap {
    Bitmap out;
    out.w = (int)width;
    out.h = (int)height;
    out.pixels.assign((size_t)width * height, 0);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        // A component sampled at this pixel. Luma is 1:1 and read
        // straight; a subsampled chroma plane is interpolated between the
        // two samples either side of the pixel CENTRE -- the triangle
        // filter every other decoder uses. Nearest-neighbour here left a
        // visible chroma staircase down every hard colour edge in a 4:2:0
        // photograph, which is most photographs.
        auto sample = [&](const JpegComponent& c) -> int {
          if (c.h == hmax && c.v == vmax) {
            const int cx = (int)x < c.px_w ? (int)x : c.px_w - 1;
            const int cy = (int)y < c.px_h ? (int)y : c.px_h - 1;
            return c.plane[(size_t)cy * c.px_w + cx];
          }
          // Pixel centre in this plane's coordinates, minus half a sample.
          const float fx = ((float)x + 0.5f) * (float)c.h / (float)hmax - 0.5f;
          const float fy = ((float)y + 0.5f) * (float)c.v / (float)vmax - 0.5f;
          int x0 = (int)(fx < 0 ? 0 : fx), y0 = (int)(fy < 0 ? 0 : fy);
          float tx = fx - (float)x0, ty = fy - (float)y0;
          if (fx < 0) { x0 = 0; tx = 0; }
          if (fy < 0) { y0 = 0; ty = 0; }
          const int x1 = x0 + 1 < c.px_w ? x0 + 1 : x0;
          const int y1 = y0 + 1 < c.px_h ? y0 + 1 : y0;
          if (x0 >= c.px_w) x0 = c.px_w - 1;
          if (y0 >= c.px_h) y0 = c.px_h - 1;
          const float a00 = (float)c.plane[(size_t)y0 * c.px_w + x0];
          const float a10 = (float)c.plane[(size_t)y0 * c.px_w + x1];
          const float a01 = (float)c.plane[(size_t)y1 * c.px_w + x0];
          const float a11 = (float)c.plane[(size_t)y1 * c.px_w + x1];
          const float top = a00 + (a10 - a00) * tx;
          const float bot = a01 + (a11 - a01) * tx;
          return (int)(top + (bot - top) * ty + 0.5f);
        };
        int r, g, b;
        if (comps.size() == 1) {
          r = g = b = sample(comps[0]);
        } else {
          const float Y = (float)sample(comps[0]);
          const float Cb = (float)sample(comps[1]) - 128.0f;
          const float Cr = (float)sample(comps[2]) - 128.0f;
          r = (int)(Y + 1.402f * Cr + 0.5f);
          g = (int)(Y - 0.344136f * Cb - 0.714136f * Cr + 0.5f);
          b = (int)(Y + 1.772f * Cb + 0.5f);
        }
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        out.pixels[(size_t)y * width + x] = argb(255, (uint8_t)r, (uint8_t)g, (uint8_t)b);
      }
    }
    return out;
  };

  while (at + 1 < n) {
    if (p[at] != 0xFF) { ++at; continue; }          // fill byte or resync
    const uint8_t marker = p[at + 1];
    at += 2;
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0xD9) break;                      // EOI
    if (at + 1 >= n) return Result<Bitmap>(Status::Invalid, "jpeg: truncated at a segment header");
    const size_t seg_len = ((size_t)p[at] << 8) | p[at + 1];
    if (seg_len < 2 || at + seg_len > n) return Result<Bitmap>(Status::Invalid, "jpeg: a segment runs past the file");
    const uint8_t* seg = p + at + 2;
    const size_t seg_n = seg_len - 2;

    switch (marker) {
      case 0xC0: case 0xC1: case 0xC2: {            // SOF0/SOF1 baseline, SOF2 progressive
        progressive = marker == 0xC2;
        if (seg_n < 6) return Result<Bitmap>(Status::Invalid, "jpeg: short SOF");
        if (seg[0] != 8) return Result<Bitmap>(Status::Unsupported, "jpeg: only 8-bit samples are decoded here");
        height = ((uint32_t)seg[1] << 8) | seg[2];
        width = ((uint32_t)seg[3] << 8) | seg[4];
        const int ncomp = seg[5];
        if (!width || !height) return Result<Bitmap>(Status::Invalid, "jpeg: zero-sized");
        if (width > kMaxDim || height > kMaxDim)
          return Result<Bitmap>(Status::Invalid, "jpeg: " + dec(width) + "x" + dec(height) + " is larger than this machine will decode");
        if ((uint64_t)width * height > kMaxPixels)
          return Result<Bitmap>(Status::Invalid, "jpeg: too many pixels to decode here");
        if (ncomp != 1 && ncomp != 3)
          return Result<Bitmap>(Status::Unsupported, "jpeg: " + dec((uint64_t)ncomp) + " components (only greyscale and YCbCr)");
        if (seg_n < 6 + (size_t)ncomp * 3) return Result<Bitmap>(Status::Invalid, "jpeg: short component list");
        comps.clear();
        for (int i = 0; i < ncomp; ++i) {
          JpegComponent c;
          c.id = seg[6 + i * 3];
          c.h = seg[7 + i * 3] >> 4;
          c.v = seg[7 + i * 3] & 0x0F;
          c.tq = seg[8 + i * 3];
          if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.tq > 3)
            return Result<Bitmap>(Status::Invalid, "jpeg: a component declares sampling this decoder will not follow");
          comps.push_back(c);
        }
        if (progressive) {
          int hm = 1, vm = 1;
          for (const auto& c : comps) { if (c.h > hm) hm = c.h; if (c.v > vm) vm = c.v; }
          const int mx = (int)((width + hm * 8 - 1) / (hm * 8)), my = (int)((height + vm * 8 - 1) / (vm * 8));
          for (auto& c : comps) {
            c.bw = mx * c.h;
            c.bh = my * c.v;
            c.coef.assign((size_t)c.bw * (size_t)c.bh * 64, 0);
          }
        }
        break;
      }
      case 0xC3: case 0xC5: case 0xC6: case 0xC7:
      case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
        return Result<Bitmap>(Status::Unsupported, "jpeg: an arithmetic or hierarchical mode this decoder does not read");
      case 0xC4: {                                  // DHT
        size_t i = 0;
        while (i + 17 <= seg_n) {
          const uint8_t tc = seg[i] >> 4, th = seg[i] & 0x0F;
          if (th > 3 || tc > 1) return Result<Bitmap>(Status::Invalid, "jpeg: a Huffman table names a slot that does not exist");
          size_t total = 0;
          for (int k = 0; k < 16; ++k) total += seg[i + 1 + k];
          if (total > 256 || i + 17 + total > seg_n)
            return Result<Bitmap>(Status::Invalid, "jpeg: a Huffman table runs past its segment");
          JpegHuff& into = tc ? ac_tables[th] : dc_tables[th];
          if (!into.build(seg + i + 1, seg + i + 17, total))
            return Result<Bitmap>(Status::Invalid, "jpeg: a Huffman table is not canonical");
          i += 17 + total;
        }
        break;
      }
      case 0xDB: {                                  // DQT
        size_t i = 0;
        while (i < seg_n) {
          const uint8_t pq = seg[i] >> 4, tq = seg[i] & 0x0F;
          if (tq > 3) return Result<Bitmap>(Status::Invalid, "jpeg: a quantisation table names a slot that does not exist");
          const size_t need = 1 + (pq ? 128u : 64u);
          if (i + need > seg_n) return Result<Bitmap>(Status::Invalid, "jpeg: a quantisation table runs past its segment");
          for (int k = 0; k < 64; ++k) {
            const uint16_t v = pq ? (uint16_t)(((uint16_t)seg[i + 1 + k * 2] << 8) | seg[i + 2 + k * 2])
                                  : (uint16_t)seg[i + 1 + k];
            quant[tq][kZigZag[k]] = v;
          }
          quant_set[tq] = true;
          i += need;
        }
        break;
      }
      case 0xDD:                                    // DRI
        if (seg_n >= 2) restart_interval = ((int)seg[0] << 8) | seg[1];
        break;
      case 0xDA: {                                  // SOS: the scan itself
        if (comps.empty()) return Result<Bitmap>(Status::Invalid, "jpeg: a scan before the frame header");
        if (seg_n < 1) return Result<Bitmap>(Status::Invalid, "jpeg: short SOS");
        const int ns = seg[0];
        if (progressive) {
          if (ns < 1 || ns > (int)comps.size() || seg_n < 1 + (size_t)ns * 2 + 3)
            return Result<Bitmap>(Status::Invalid, "jpeg: short scan header");
          JpegComponent* sc[4] = {nullptr, nullptr, nullptr, nullptr};
          for (int i = 0; i < ns; ++i) {
            const int id = seg[1 + i * 2];
            const int tt = seg[2 + i * 2];
            for (auto& q : comps) if (q.id == id) sc[i] = &q;
            if (!sc[i]) return Result<Bitmap>(Status::Invalid, "jpeg: the scan names a component the frame did not");
            sc[i]->td = tt >> 4;
            sc[i]->ta = tt & 0x0F;
            if (sc[i]->td > 3 || sc[i]->ta > 3)
              return Result<Bitmap>(Status::Invalid, "jpeg: the scan names a table slot that does not exist");
          }
          const int ss = seg[1 + ns * 2], se = seg[2 + ns * 2];
          const int ah = seg[3 + ns * 2] >> 4, al = seg[3 + ns * 2] & 0x0F;
          if (ss > se || se > 63 || al > 13 || (ss == 0 && se != 0) || (ss > 0 && ns != 1))
            return Result<Bitmap>(Status::Invalid, "jpeg: a progressive scan with impossible bands");
          for (int i = 0; i < ns; ++i)
            if ((ss == 0 && ah == 0 && !dc_tables[sc[i]->td].present) || (ss > 0 && !ac_tables[sc[i]->ta].present))
              return Result<Bitmap>(Status::Invalid, "jpeg: a component uses a Huffman table that never arrived");
          int hmax = 1, vmax = 1;
          for (const auto& c : comps) { if (c.h > hmax) hmax = c.h; if (c.v > vmax) vmax = c.v; }
          const int mcus_x = (int)((width + hmax * 8 - 1) / (hmax * 8));
          const int mcus_y = (int)((height + vmax * 8 - 1) / (vmax * 8));
          JpegBits bits(p, n, at + seg_len);
          any_scan = true;
          const bool good = jpeg_prog_scan(bits, sc, ns, ss, se, ah, al, restart_interval, hmax, vmax, width, height,
                                           mcus_x, mcus_y, dc_tables, ac_tables);
          // Back to the next real marker: skip stuffed 0xFF00 and RSTn that
          // the scan itself did not consume.
          at = bits.at;
          while (at + 1 < n && !(p[at] == 0xFF && p[at + 1] != 0x00 && !(p[at + 1] >= 0xD0 && p[at + 1] <= 0xD7))) ++at;
          if (!good) { at = n; continue; }          // keep the blur gathered so far
          continue;
        }
        if (ns != (int)comps.size())
          return Result<Bitmap>(Status::Unsupported, "jpeg: a scan over some of the components, which is progressive's shape");
        if (seg_n < 1 + (size_t)ns * 2 + 3) return Result<Bitmap>(Status::Invalid, "jpeg: short scan header");
        for (int i = 0; i < ns; ++i) {
          const int id = seg[1 + i * 2];
          const int tt = seg[2 + i * 2];
          JpegComponent* c = nullptr;
          for (auto& q : comps) if (q.id == id) c = &q;
          if (!c) return Result<Bitmap>(Status::Invalid, "jpeg: the scan names a component the frame did not");
          c->td = tt >> 4;
          c->ta = tt & 0x0F;
          if (c->td > 3 || c->ta > 3) return Result<Bitmap>(Status::Invalid, "jpeg: the scan names a table slot that does not exist");
        }

        int hmax = 1, vmax = 1;
        for (const auto& c : comps) { if (c.h > hmax) hmax = c.h; if (c.v > vmax) vmax = c.v; }
        const int mcu_w = hmax * 8, mcu_h = vmax * 8;
        const int mcus_x = (int)((width + mcu_w - 1) / mcu_w);
        const int mcus_y = (int)((height + mcu_h - 1) / mcu_h);

        for (auto& c : comps) {
          c.px_w = mcus_x * c.h * 8;
          c.px_h = mcus_y * c.v * 8;
          c.plane.assign((size_t)c.px_w * c.px_h, 128);
          c.dc_pred = 0;
          if (!quant_set[c.tq]) return Result<Bitmap>(Status::Invalid, "jpeg: a component uses a quantisation table that never arrived");
          if (!dc_tables[c.td].present || !ac_tables[c.ta].present)
            return Result<Bitmap>(Status::Invalid, "jpeg: a component uses a Huffman table that never arrived");
        }

        JpegBits bits(p, n, at + seg_len);
        int since_restart = 0;
        int coeffs[64];
        for (int my = 0; my < mcus_y; ++my) {
          for (int mx = 0; mx < mcus_x; ++mx) {
            if (restart_interval && since_restart == restart_interval) {
              bits.restart();
              for (auto& c : comps) c.dc_pred = 0;
              since_restart = 0;
            }
            ++since_restart;
            for (auto& c : comps) {
              for (int by = 0; by < c.v; ++by) {
                for (int bx = 0; bx < c.h; ++bx) {
                  std::memset(coeffs, 0, sizeof(coeffs));
                  const int t = bits.decode(dc_tables[c.td]);
                  if (t < 0 || t > 15) return Result<Bitmap>(Status::Invalid, "jpeg: a DC coefficient did not decode");
                  c.dc_pred += JpegBits::extend(bits.receive(t), t);
                  coeffs[0] = c.dc_pred;
                  for (int k = 1; k < 64;) {
                    const int rs = bits.decode(ac_tables[c.ta]);
                    if (rs < 0) return Result<Bitmap>(Status::Invalid, "jpeg: an AC coefficient did not decode");
                    const int run = rs >> 4, size = rs & 0x0F;
                    if (size == 0) {
                      if (run != 15) break;          // EOB
                      k += 16;
                      continue;
                    }
                    k += run;
                    if (k > 63) break;
                    coeffs[kZigZag[k]] = JpegBits::extend(bits.receive(size), size);
                    ++k;
                  }
                  const int ox = (mx * c.h + bx) * 8;
                  const int oy = (my * c.v + by) * 8;
                  if (ox + 8 > c.px_w || oy + 8 > c.px_h) continue;
                  jpeg_idct(coeffs, quant[c.tq], &c.plane[(size_t)oy * c.px_w + ox], c.px_w);
                }
              }
            }
          }
        }

        return Result<Bitmap>(build_bitmap(hmax, vmax));
      }
      default:
        break;                                      // APPn, COM, anything else
    }
    at += seg_len;
  }
  if (progressive && any_scan) {
    int hmax = 1, vmax = 1;
    for (const auto& c : comps) { if (c.h > hmax) hmax = c.h; if (c.v > vmax) vmax = c.v; }
    int coeffs[64];
    for (auto& c : comps) {
      if (!quant_set[c.tq]) return Result<Bitmap>(Status::Invalid, "jpeg: a component uses a quantisation table that never arrived");
      c.px_w = c.bw * 8;
      c.px_h = c.bh * 8;
      c.plane.assign((size_t)c.px_w * c.px_h, 128);
      for (int by = 0; by < c.bh; ++by)
        for (int bx = 0; bx < c.bw; ++bx) {
          const int16_t* blk = &c.coef[((size_t)by * (size_t)c.bw + (size_t)bx) * 64];
          for (int k = 0; k < 64; ++k) coeffs[k] = blk[k];
          jpeg_idct(coeffs, quant[c.tq], &c.plane[(size_t)by * 8 * c.px_w + (size_t)bx * 8], c.px_w);
        }
    }
    return Result<Bitmap>(build_bitmap(hmax, vmax));
  }
  return Result<Bitmap>(Status::Invalid, "jpeg: unexpected eof");
}

Result<Bitmap> decode_bmp(ByteSpan bytes) {
  if (bytes.size() < 54) return Result<Bitmap>(Status::Invalid, "bmp: header too short");
  if (bytes[0] != 'B' || bytes[1] != 'M') return Result<Bitmap>(Status::Invalid, "bmp: bad magic");

  auto le16 = [](const uint8_t* p) -> uint16_t {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  };
  auto le32 = [](const uint8_t* p) -> uint32_t {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  };

  uint32_t data_offset = le32(bytes.data() + 10);
  uint32_t dib_header_size = le32(bytes.data() + 14);
  if (dib_header_size < 40 && dib_header_size != 12) {
    return Result<Bitmap>(Status::Invalid, "bmp: unsupported DIB header");
  }

  int32_t w = 0, h = 0;
  uint16_t bpp = 0;
  uint32_t compression = 0;
  if (dib_header_size == 12) {
    w = (int32_t)le16(bytes.data() + 18);
    h = (int32_t)le16(bytes.data() + 20);
    bpp = le16(bytes.data() + 24);
  } else {
    w = (int32_t)le32(bytes.data() + 18);
    h = (int32_t)le32(bytes.data() + 22);
    bpp = le16(bytes.data() + 28);
    compression = le32(bytes.data() + 30);
  }

  if (w <= 0 || w > (int32_t)kMaxDim || h == 0 || std::abs(h) > (int32_t)kMaxDim) {
    return Result<Bitmap>(Status::Invalid, "bmp: invalid dimensions");
  }
  bool top_down = h < 0;
  int32_t abs_h = std::abs(h);
  if ((uint64_t)w * (uint64_t)abs_h > kMaxPixels) {
    return Result<Bitmap>(Status::Invalid, "bmp: image too large");
  }

  if (compression != 0 && compression != 3) {
    return Result<Bitmap>(Status::Unsupported, "bmp: compressed BMP not supported");
  }

  // Palette for 8-bit
  uint32_t palette[256];
  if (bpp == 8) {
    size_t pal_offset = 14 + dib_header_size;
    size_t num_colors = 256;
    if (pal_offset + num_colors * 4 > bytes.size()) {
      return Result<Bitmap>(Status::Invalid, "bmp: truncated palette");
    }
    for (size_t i = 0; i < num_colors; ++i) {
      uint8_t b = bytes[pal_offset + i * 4];
      uint8_t g = bytes[pal_offset + i * 4 + 1];
      uint8_t r = bytes[pal_offset + i * 4 + 2];
      palette[i] = argb(255, r, g, b);
    }
  }

  if (data_offset >= bytes.size()) {
    return Result<Bitmap>(Status::Invalid, "bmp: data offset out of bounds");
  }

  size_t row_stride = ((size_t)w * bpp + 31) / 32 * 4;
  if (data_offset + row_stride * (size_t)abs_h > bytes.size()) {
    return Result<Bitmap>(Status::Invalid, "bmp: truncated pixel data");
  }

  Bitmap out;
  out.w = w;
  out.h = abs_h;
  out.pixels.resize((size_t)w * (size_t)abs_h);

  for (int y = 0; y < abs_h; ++y) {
    int src_y = top_down ? y : (abs_h - 1 - y);
    const uint8_t* row = bytes.data() + data_offset + (size_t)src_y * row_stride;
    uint32_t* dst_row = out.pixels.data() + (size_t)y * (size_t)w;

    if (bpp == 32) {
      for (int x = 0; x < w; ++x) {
        uint8_t b = row[x * 4];
        uint8_t g = row[x * 4 + 1];
        uint8_t r = row[x * 4 + 2];
        uint8_t a = row[x * 4 + 3];
        // If alpha channel is all zeros in 32-bit BMP, treat as opaque
        dst_row[x] = argb(a ? a : 255, r, g, b);
      }
    } else if (bpp == 24) {
      for (int x = 0; x < w; ++x) {
        uint8_t b = row[x * 3];
        uint8_t g = row[x * 3 + 1];
        uint8_t r = row[x * 3 + 2];
        dst_row[x] = argb(255, r, g, b);
      }
    } else if (bpp == 8) {
      for (int x = 0; x < w; ++x) {
        uint8_t idx = row[x];
        dst_row[x] = palette[idx];
      }
    } else {
      return Result<Bitmap>(Status::Unsupported, "bmp: only 8, 24, and 32 bpp supported");
    }
  }

  return Result<Bitmap>(std::move(out));
}

}  // namespace

Result<Bitmap> decode_image(ByteSpan bytes) {
  if (bytes.size() < 4) return Result<Bitmap>(Status::Invalid, "not enough bytes to be an image");
  if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P') return decode_png(bytes);
  if (bytes.size() >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') return decode_gif(bytes);
  if (bytes[0] == 0xFF && bytes[1] == 0xD8) return decode_jpeg(bytes);
  if (bytes[0] == 'B' && bytes[1] == 'M') return decode_bmp(bytes);
  if (bytes[0] == '<' || (bytes.size() >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F')) {
    if (bytes[0] == '<') {
      // Direct inline SVG decoding
      NodePtr dom = parse_html(bytes);
      if (dom) {
        Node* svg = dom->first("svg");
        if (!svg && dom->tag == "svg") svg = dom.get();
        if (svg) {
          // The picture's own proportions: the width and height it names, or
          // else its viewBox. With neither, a 140x22 wordmark was squeezed into
          // a 24x24 square. It is rasterised at a size it can be scaled DOWN
          // from -- nearest-neighbour shrinking a big drawing looks like an icon,
          // stretching a small one looks like a mistake.
          float vbw = 0, vbh = 0;
          {
            const std::string vb = svg->attr("viewBox").empty() ? svg->attr("viewbox") : svg->attr("viewBox");
            const char* q = vb.c_str();
            char* end = nullptr;
            float v[4];
            int got = 0;
            for (; got < 4; ++got) {
              while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n') ++q;
              v[got] = std::strtof(q, &end);
              if (end == q) break;
              q = end;
            }
            if (got == 4 && v[2] > 0 && v[3] > 0) { vbw = v[2]; vbh = v[3]; }
          }
          auto px_attr = [&](const char* name) {
            const std::string a = svg->attr(name);
            if (a.empty() || a.find('%') != std::string::npos) return 0.0f;
            return (float)std::atof(a.c_str());
          };
          float fw = px_attr("width"), fh = px_attr("height");
          if (fw <= 0 && fh <= 0) { fw = vbw; fh = vbh; }
          else if (fw <= 0 && vbw > 0) fw = fh * vbw / vbh;
          else if (fh <= 0 && vbw > 0) fh = fw * vbh / vbw;
          if (fw <= 0 || fh <= 0) { fw = 24; fh = 24; }
          const float longest = fw > fh ? fw : fh;
          const float k = longest < 192.0f ? 192.0f / longest : 1.0f;
          int w = std::max(1, (int)(fw * k + 0.5f)), h = std::max(1, (int)(fh * k + 0.5f));
          if (w > 1024) { h = h * 1024 / w; w = 1024; }
          if (h > 1024) { w = w * 1024 / h; h = 1024; }
          return rasterise_svg(*svg, w, h, 0xFFFFFFFF);
        }
      }
    }
    return Result<Bitmap>(Status::Unsupported, "svg or webp container not parsed here");
  }
  return Result<Bitmap>(Status::Unsupported, "an image format this engine does not decode");
}

Bitmap scale_bitmap(const Bitmap& src, int w, int h) {
  Bitmap out;
  if (src.empty() || w <= 0 || h <= 0) return out;
  if (w > (int)kMaxDim) w = (int)kMaxDim;
  if (h > (int)kMaxDim) h = (int)kMaxDim;
  out.w = w;
  out.h = h;
  out.pixels.assign((size_t)w * h, 0);
  for (int y = 0; y < h; ++y) {
    int sy = src.h > 1 ? y * src.h / h : 0;
    if (sy >= src.h) sy = src.h - 1;
    for (int x = 0; x < w; ++x) {
      int sx = src.w > 1 ? x * src.w / w : 0;
      if (sx >= src.w) sx = src.w - 1;
      out.pixels[(size_t)y * w + x] = src.pixels[(size_t)sy * src.w + sx];
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Out again: a picture this machine can hand to something that reads pictures.
// ---------------------------------------------------------------------------
// The engine could read PNG, GIF and JPEG and could not write anything at all,
// so the only way out of the framebuffer was the PPM the dump writes — which
// nothing but this repository can open. A mind that is asked to DRAW cannot see
// what it drew unless the machine can hand it a picture in a format the world
// reads.
//
// Deflate, stored: every block uncompressed, which is a legal zlib stream and
// costs one byte per 65535 plus five. A real encoder belongs here eventually;
// what matters first is that the bytes are a PNG.
namespace {

uint32_t png_crc(const uint8_t* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    built = true;
  }
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void png_be32(ByteBuffer& out, uint32_t v) {
  out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
  out.push_back((uint8_t)(v >> 8));  out.push_back((uint8_t)v);
}

void png_chunk(ByteBuffer& out, const char tag[4], const ByteBuffer& data) {
  png_be32(out, (uint32_t)data.size());
  const size_t at = out.size();
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t)tag[i]);
  out.insert(out.end(), data.begin(), data.end());
  png_be32(out, png_crc(out.data() + at, out.size() - at) ^ 0xFFFFFFFFu);
}

// PNG's predictor. a = left, b = above, c = above-left.
inline uint8_t png_paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return (uint8_t)a;
  return pb <= pc ? (uint8_t)b : (uint8_t)c;
}

// Filter each row five ways and keep whichever has the smallest sum of its
// bytes read as signed — the standard's own heuristic. This matters more than
// the compression that follows it on a picture like these: a gradient under Up
// becomes a field of near-zeros before deflate ever sees it.
void png_filter_rows(const ByteBuffer& px, int w, int h, int bpp, ByteBuffer& out) {
  const size_t stride = (size_t)w * (size_t)bpp;
  ByteBuffer prior(stride, 0), cand(stride), best(stride);
  out.reserve((size_t)h * (stride + 1));
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = px.data() + (size_t)y * stride;
    int best_type = 0;
    long best_score = -1;
    for (int type = 0; type < 5; ++type) {
      for (size_t i = 0; i < stride; ++i) {
        const int a = i >= (size_t)bpp ? row[i - bpp] : 0;
        const int b = prior[i];
        const int c = i >= (size_t)bpp ? prior[i - bpp] : 0;
        int v;
        switch (type) {
          case 1:  v = row[i] - a; break;
          case 2:  v = row[i] - b; break;
          case 3:  v = row[i] - ((a + b) >> 1); break;
          case 4:  v = row[i] - png_paeth(a, b, c); break;
          default: v = row[i];
        }
        cand[i] = (uint8_t)v;
      }
      long score = 0;
      for (size_t i = 0; i < stride; ++i) { const int s = (int8_t)cand[i]; score += s < 0 ? -s : s; }
      if (best_score < 0 || score < best_score) { best_score = score; best_type = type; best.swap(cand); }
    }
    out.push_back((uint8_t)best_type);
    out.insert(out.end(), best.begin(), best.end());
    prior.assign(row, row + stride);
  }
}

// Deflate writes in two bit orders at once: the stream fills a byte from its
// least significant bit up, but a Huffman code goes out from its MOST
// significant bit down. Extra bits are written in stream order; codes reversed.
struct BitOut {
  ByteBuffer& out;
  uint32_t acc = 0;
  int n = 0;
  void put(uint32_t v, int c) {
    if (c <= 0) return;
    acc |= (v & ((1u << c) - 1u)) << n;
    n += c;
    while (n >= 8) { out.push_back((uint8_t)(acc & 0xFF)); acc >>= 8; n -= 8; }
  }
  void code(uint32_t v, int c) {
    uint32_t r = 0;
    for (int i = 0; i < c; ++i) r = (r << 1) | ((v >> i) & 1u);
    put(r, c);
  }
  void flush() { if (n) { out.push_back((uint8_t)(acc & 0xFF)); acc = 0; n = 0; } }
};

// The fixed literal/length tree of RFC 1951 §3.2.6. Nothing has to describe a
// tree in the stream, which is most of a dynamic encoder's complexity for a few
// percent that does not matter here.
void fixed_literal(BitOut& b, int sym) {
  if (sym <= 143)      b.code(0x30u + (uint32_t)sym, 8);
  else if (sym <= 255) b.code(0x190u + (uint32_t)(sym - 144), 9);
  else if (sym <= 279) b.code((uint32_t)(sym - 256), 7);
  else                 b.code(0xC0u + (uint32_t)(sym - 280), 8);
}

const uint16_t kLenBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// LZ77 over a 32 KiB window, matches found down a hash chain on three bytes.
// The chain is walked a bounded number of steps: a picture is mostly long runs
// of one colour, where the first match found is already a good one, and an
// unbounded search turns a seven-megabyte buffer into minutes.
void deflate_fixed(const ByteBuffer& in, ByteBuffer& out) {
  BitOut b{out};
  b.put(1, 1);   // BFINAL
  b.put(1, 2);   // BTYPE = fixed Huffman
  const size_t n = in.size();
  // The chain is a ring the size of the window, not one entry per input byte.
  // Indexed per byte it would be 28 MB for a full pane — most of the heap on a
  // bare-metal machine, to hold positions that are all further back than a
  // match may reach anyway. This is what zlib does.
  constexpr size_t kWindow = 32768;
  std::vector<int> head((size_t)1 << 16, -1);
  std::vector<int> prev(kWindow, -1);
  auto hash3 = [&](size_t i) -> uint32_t {
    return (uint32_t)((((uint32_t)in[i] << 10) ^ ((uint32_t)in[i + 1] << 5) ^ (uint32_t)in[i + 2]) & 0xFFFF);
  };
  auto insert_at = [&](size_t at) {
    if (at + 2 >= n) return;
    const uint32_t h = hash3(at);
    prev[at & (kWindow - 1)] = head[h];
    head[h] = (int)at;
  };
  size_t i = 0;
  while (i < n) {
    int best_len = 0, best_dist = 0;
    if (i + 2 < n) {
      int cand = head[hash3(i)];
      int steps = 0;
      const size_t max_len = std::min<size_t>(258, n - i);
      while (cand >= 0 && steps < 64) {
        const size_t d = i - (size_t)cand;
        if (d == 0 || d > 32768) break;
        size_t l = 0;
        while (l < max_len && in[(size_t)cand + l] == in[i + l]) ++l;
        if ((int)l > best_len) { best_len = (int)l; best_dist = (int)d; if (l == max_len) break; }
        cand = prev[(size_t)cand & (kWindow - 1)];
        ++steps;
      }
    }
    if (best_len >= 3) {
      int lc = 28;
      while (lc > 0 && kLenBase[lc] > best_len) --lc;
      fixed_literal(b, 257 + lc);
      b.put((uint32_t)(best_len - kLenBase[lc]), kLenExtra[lc]);
      int dc = 29;
      while (dc > 0 && kDistBase[dc] > best_dist) --dc;
      b.code((uint32_t)dc, 5);
      b.put((uint32_t)(best_dist - kDistBase[dc]), kDistExtra[dc]);
      // Every position the match covered still enters the chain, or the next
      // match cannot see back past it.
      for (int k = 0; k < best_len; ++k) insert_at(i + (size_t)k);
      i += (size_t)best_len;
    } else {
      insert_at(i);
      fixed_literal(b, in[i]);
      ++i;
    }
  }
  fixed_literal(b, 256);   // end of block
  b.flush();
}

}  // namespace

ByteBuffer encode_png(const uint32_t* argb, int w, int h) {
  ByteBuffer out;
  if (!argb || w <= 0 || h <= 0) return out;
  const uint8_t sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
  out.insert(out.end(), sig, sig + 8);

  ByteBuffer ihdr;
  png_be32(ihdr, (uint32_t)w);
  png_be32(ihdr, (uint32_t)h);
  ihdr.push_back(8);            // 8 bits per channel
  ihdr.push_back(6);            // truecolour with alpha
  ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  png_chunk(out, "IHDR", ihdr);

  // The raw scanlines, RGBA with no filter byte: png_filter_rows chooses a
  // filter per row and writes the byte itself.
  ByteBuffer raw;
  raw.reserve((size_t)h * (size_t)w * 4);
  for (int y = 0; y < h; ++y) {
    const uint32_t* row = argb + (size_t)y * (size_t)w;
    for (int x = 0; x < w; ++x) {
      const uint32_t p = row[x];
      raw.push_back((uint8_t)(p >> 16));
      raw.push_back((uint8_t)(p >> 8));
      raw.push_back((uint8_t)p);
      raw.push_back((uint8_t)(p >> 24));
    }
  }

  // Filtered per row, then deflated. The adler is over the FILTERED bytes —
  // they are what the stream carries and what an inflater reproduces.
  ByteBuffer filtered;
  png_filter_rows(raw, w, h, 4, filtered);

  ByteBuffer z;
  z.push_back(0x78); z.push_back(0x01);          // zlib header, no preset
  deflate_fixed(filtered, z);
  uint32_t a1 = 1, a2 = 0;
  for (uint8_t b : filtered) { a1 = (a1 + b) % 65521; a2 = (a2 + a1) % 65521; }
  png_be32(z, (a2 << 16) | a1);
  png_chunk(out, "IDAT", z);

  png_chunk(out, "IEND", ByteBuffer());
  return out;
}

}  // namespace aios::userland::web
