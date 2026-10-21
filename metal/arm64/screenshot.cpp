/* Remote screenshot over TCP (metal/arm64/screenshot.cpp).
 *
 * There is no way to see what this board's HDMI output actually looks like
 * from off-board except a camera pointed at a monitor - which is how "the
 * desktop looks like garbage" got reported and how "what does it actually
 * look like" got answered with a guess instead of a fact. This is the fix:
 * connect to tcp/42426 and the board sends back a 24-bit BMP of the exact
 * pixels on screen right now (fb_pixels(), the same buffer fb_blit() and
 * the console painted into - not a redraw, a read of what is already
 * there), then closes. From a laptop on the same LAN:
 *   nc <pi-ip> 42426 > shot.bmp
 * BMP, not PNG: no compression to implement, a fixed 54-byte header, and
 * every image viewer already opens it.
 */
#include "fb.hpp"
#include "kio.hpp"
#include "net.hpp"

namespace {
constexpr uint16_t SCREENSHOT_PORT = 42426;
constexpr uint32_t MAX_W = 1920, MAX_H = 1200;
constexpr uint32_t MAX_ROW = ((MAX_W * 3 + 3) / 4) * 4;
/* Sized for the QOI worst case (5 bytes/pixel), which the encoder checks
 * against - the old BMP-sized buffer made every 1080p screenshot come back
 * empty once the kernel started rendering at the display's native size. */
uint8_t g_bmp[static_cast<size_t>(MAX_W) * MAX_H * 5 + 64];  /* .bss, ~11.5MB of 944MB */
int g_listen_sock = -1;

void put_u32le(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
void put_u16le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Every byte here has to cross this stack's TCP send path, and that path is
 * genuinely slow - TCP_SOCKET_BUF_SIZE (net.hpp) caps the unacked-data
 * window at 8KB, and a full 1280x720 24-bit BMP (2.7MB) measured ~7.5KB/s
 * on real hardware: six minutes for one screenshot. Fixing that throughput
 * is a real, separate project (window scaling, why draining an acked 8KB
 * chunk isn't near-instant on a LAN); it is not what "let me see the
 * screen" needs today. Downsampling by SCALE (nearest-neighbour, point
 * sampling - no filtering to implement) cuts the payload by SCALE^2 and
 * turns "six minutes" into single-digit seconds while staying legible
 * enough to read window chrome and tab labels. */
constexpr uint32_t SCALE = 4;

/* Encodes a SCALE-downsampled fb_pixels() (0x00RRGGBB, fb_stride() pixels/
 * row) into g_bmp as a bottom-up 24-bit BMP, BGR triplets, each row padded
 * to a 4-byte boundary - exactly what the BMP format requires, not a
 * convenience choice. Returns the total byte length, or 0 if there is no
 * live framebuffer or it is bigger than this function can encode. */
size_t encode_bmp() {
  if (!fb_live()) return 0;
  const uint32_t full_w = fb_width(), full_h = fb_height(), stride = fb_stride();
  if (full_w == 0 || full_h == 0 || full_w > MAX_W || full_h > MAX_H) return 0;
  const uint32_t w = full_w / SCALE, h = full_h / SCALE;
  if (w == 0 || h == 0) return 0;
  const uint32_t row_bytes = ((w * 3 + 3) / 4) * 4;
  const uint32_t pixel_bytes = row_bytes * h;
  const uint32_t file_len = 54 + pixel_bytes;

  uint8_t* p = g_bmp;
  p[0] = 'B'; p[1] = 'M';
  put_u32le(p + 2, file_len);
  put_u32le(p + 6, 0);
  put_u32le(p + 10, 54);          /* pixel data offset */
  put_u32le(p + 14, 40);          /* BITMAPINFOHEADER size */
  put_u32le(p + 18, w);
  put_u32le(p + 22, h);           /* positive height: bottom-up rows, standard BMP */
  put_u16le(p + 26, 1);           /* planes */
  put_u16le(p + 28, 24);          /* bpp */
  put_u32le(p + 30, 0);           /* BI_RGB, no compression */
  put_u32le(p + 34, pixel_bytes);
  put_u32le(p + 38, 2835);        /* ~72 DPI, cosmetic only */
  put_u32le(p + 42, 2835);
  put_u32le(p + 46, 0);
  put_u32le(p + 50, 0);

  const uint32_t* src = fb_pixels();
  uint8_t* out = g_bmp + 54;
  for (uint32_t y = 0; y < h; ++y) {
    const uint32_t* row = src + static_cast<uint64_t>(full_h - 1 - y * SCALE) * stride;  /* bottom-up */
    uint8_t* dst = out + static_cast<uint64_t>(y) * row_bytes;
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t px = row[x * SCALE];
      dst[x * 3 + 0] = static_cast<uint8_t>(px);         /* B */
      dst[x * 3 + 1] = static_cast<uint8_t>(px >> 8);     /* G */
      dst[x * 3 + 2] = static_cast<uint8_t>(px >> 16);    /* R */
    }
    for (uint32_t pad = w * 3; pad < row_bytes; ++pad) dst[pad] = 0;
  }
  return file_len;
}
/* Full-resolution QOI ("Quite OK Image", qoiformat.org) of the screen.
 * Lossless and trivial to encode; the dashboard is mostly flat colour, so
 * runs and index hits collapse it by an order of magnitude - which matters
 * at this TCP stack's few KB/s, where a full BMP takes ~11 minutes and the
 * 1/4-scale one drops too many pixels to read any text. */
constexpr uint16_t QOI_PORT = 42427;
int g_qoi_listen = -1;

size_t encode_qoi() {
  if (!fb_live()) return 0;
  const uint32_t w = fb_width(), h = fb_height(), stride = fb_scanout_stride();
  if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h * 4 + 22 > sizeof(g_bmp)) return 0;
  uint8_t* o = g_bmp;
  size_t n = 0;
  auto be32 = [&](uint32_t v) { o[n++] = static_cast<uint8_t>(v >> 24); o[n++] = static_cast<uint8_t>(v >> 16);
                                 o[n++] = static_cast<uint8_t>(v >> 8);  o[n++] = static_cast<uint8_t>(v); };
  o[n++] = 'q'; o[n++] = 'o'; o[n++] = 'i'; o[n++] = 'f';
  be32(w); be32(h);
  o[n++] = 3;   /* RGB */
  o[n++] = 0;   /* sRGB */

  uint32_t index[64] = {};
  uint8_t pr = 0, pg = 0, pb = 0;
  uint32_t run = 0;
  /* The SCANOUT buffer, decoded in its real byte order: this is what the
   * monitor shows. Reading the drawing back buffer showed the colours the
   * code intended while the real screen had red and blue swapped. */
  const uint32_t* src = fb_scanout_pixels();
  const bool rgb_bytes = fb_scanout_rgb_bytes();
  const uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t k = 0; k < total; ++k) {
    uint32_t px = src[(k / w) * stride + (k % w)] & 0x00FFFFFFu;
    if (rgb_bytes) px = (px & 0x00FF00u) | ((px & 0xFFu) << 16) | ((px >> 16) & 0xFFu);
    const uint8_t r = static_cast<uint8_t>(px >> 16), g = static_cast<uint8_t>(px >> 8), b = static_cast<uint8_t>(px);
    if (r == pr && g == pg && b == pb) {
      if (++run == 62 || k + 1 == total) { o[n++] = static_cast<uint8_t>(0xC0 | (run - 1)); run = 0; }
      continue;
    }
    if (run) { o[n++] = static_cast<uint8_t>(0xC0 | (run - 1)); run = 0; }
    const uint32_t h6 = (r * 3u + g * 5u + b * 7u + 255u * 11u) % 64u;
    const uint32_t key = 0xFF000000u | px;
    if (index[h6] == key) {
      o[n++] = static_cast<uint8_t>(h6);
    } else {
      index[h6] = key;
      const int dr = static_cast<int8_t>(r - pr), dg = static_cast<int8_t>(g - pg), db = static_cast<int8_t>(b - pb);
      const int dr_dg = dr - dg, db_dg = db - dg;
      if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
        o[n++] = static_cast<uint8_t>(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
      } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
        o[n++] = static_cast<uint8_t>(0x80 | (dg + 32));
        o[n++] = static_cast<uint8_t>(((dr_dg + 8) << 4) | (db_dg + 8));
      } else {
        o[n++] = 0xFE; o[n++] = r; o[n++] = g; o[n++] = b;
      }
    }
    pr = r; pg = g; pb = b;
  }
  for (int k = 0; k < 7; ++k) o[n++] = 0;
  o[n++] = 1;
  return n;
}

}  /* namespace */

bool screenshot_server_start() {
  g_qoi_listen = net_tcp_listen(QOI_PORT);
  if (g_qoi_listen >= 0) {
    kprintf("[boot] full-resolution screenshot (QOI) on tcp/%u\n", static_cast<unsigned>(QOI_PORT));
  }
  g_listen_sock = net_tcp_listen(SCREENSHOT_PORT);
  if (g_listen_sock < 0) return false;
  kprintf("[boot] screenshot server on tcp/%u - `nc <this-ip> %u > shot.bmp` for the real screen, not a description of it\n",
          static_cast<unsigned>(SCREENSHOT_PORT), static_cast<unsigned>(SCREENSHOT_PORT));
  return true;
}

/* Called every main-loop iteration, same shape as net_poll()/usb_hid_poll() -
 * a no-op until screenshot_server_start() has run, and non-blocking after:
 * net_tcp_accept(..., 0) returns immediately whether or not anyone is
 * connecting. */
void screenshot_server_poll() {
  if (g_qoi_listen >= 0) {
    const int qs = net_tcp_accept(g_qoi_listen, 0);
    if (qs >= 0) {
      const size_t qlen = encode_qoi();
      if (qlen > 0) net_tcp_send_all(qs, g_bmp, qlen, 30000);
      net_tcp_close(qs);
    }
  }
  if (g_listen_sock < 0) return;
  int sock = net_tcp_accept(g_listen_sock, 0);
  if (sock < 0) return;
  size_t len = encode_bmp();
  if (len > 0) net_tcp_send_all(sock, g_bmp, len, 30000);
  net_tcp_close(sock);
}
