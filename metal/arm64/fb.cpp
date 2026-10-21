/* Framebuffer and framebuffer console (metal/arm64/fb.cpp). */
#include "fb.hpp"
extern "C" void* memcpy(void* d, const void* s, size_t n);   // krt.cpp

#include "fdt.hpp"
#include "kio.hpp"
#include "mmu.hpp"
#include "timer.hpp"
#include "uart.hpp"

#include "../font8x16.h"

#include "rpi_mailbox.hpp"

/* virtio-gpu, for machines where nothing has configured a display for us. */
bool gpu_init();
bool gpu_live();
bool gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
uint32_t gpu_width();
uint32_t gpu_height();
uint32_t* gpu_pixels();
uint64_t gpu_flushes();
uint64_t gpu_flush_ns();
uint64_t gpu_base();
uint32_t gpu_version();

/* ramfb: we allocate the pixels and hand the address to the device, which is
 * the same arrangement a firmware-configured panel gives us. */
bool ramfb_setup(uint32_t* pixels, uint32_t width, uint32_t height);
uint64_t ramfb_fwcfg_base();

namespace {

uint32_t* g_px = nullptr;
uint32_t* g_draw_px = nullptr; /* In-DRAM backbuffer to eliminate scanout flicker */
/* Scanout stores bytes R,G,B (words 0x00BBGGRR) - the firmware granted "RGB"
 * pixel order. Drawing always uses 0x00RRGGBB, so fb_present swaps. */
bool g_scanout_rgb_bytes = false;
uint32_t g_w = 0, g_h = 0;
uint32_t g_stride_px = 0;     /* pixels per line, which is not always width */
FbSource g_src = FbSource::None;
uint32_t g_scale = 1;

/* Console state. */
uint32_t g_cols = 0, g_rows = 0;
uint32_t g_cx = 0, g_cy = 0;
uint32_t g_fg = 0xD0D8E0;
uint32_t g_bg = 0x0B0E14;
bool g_con_live = false;
bool g_in_con = false;        /* re-entry guard: the console must never print */
uint32_t g_dirty_y0 = 0, g_dirty_y1 = 0;
bool g_dirty = false;

constexpr uint32_t GLYPH_W = 8;
constexpr uint32_t GLYPH_H = 16;

inline uint32_t* target_px() { return g_draw_px ? g_draw_px : g_px; }
inline uint32_t target_stride() { return g_draw_px ? g_w : g_stride_px; }

void mark_dirty(uint32_t y0, uint32_t y1) {
  if (!g_dirty) { g_dirty_y0 = y0; g_dirty_y1 = y1; g_dirty = true; return; }
  if (y0 < g_dirty_y0) g_dirty_y0 = y0;
  if (y1 > g_dirty_y1) g_dirty_y1 = y1;
}

void flush_dirty() {
  if (!g_dirty) return;
  fb_present(0, g_dirty_y0, g_w, g_dirty_y1 - g_dirty_y0);
  g_dirty = false;
}

void scroll_one_line() {
  const uint32_t line = GLYPH_H * g_scale;
  const uint32_t moved = (g_h > line) ? g_h - line : 0;
  uint32_t* px = target_px();
  uint32_t stride = target_stride();
  for (uint32_t y = 0; y < moved; ++y) {
    uint32_t* dst = px + static_cast<uint64_t>(y) * stride;
    const uint32_t* src = px + static_cast<uint64_t>(y + line) * stride;
    for (uint32_t x = 0; x < g_w; ++x) dst[x] = src[x];
  }
  fb_fill(0, moved, g_w, g_h - moved, g_bg);
  mark_dirty(0, g_h);
}

}  /* namespace */

bool fb_live()       { return g_px != nullptr; }
FbSource fb_source() { return g_src; }
uint32_t fb_width()  { return g_w; }
uint32_t fb_height() { return g_h; }
uint32_t fb_scale()  { return g_scale; }
uint32_t fbcon_cols(){ return g_cols; }
uint32_t fbcon_rows(){ return g_rows; }

const char* fb_source_name() {
  switch (g_src) {
    case FbSource::VirtioGpu:         return "virtio-gpu, driven by this kernel";
    case FbSource::SimpleFramebuffer: return "simple-framebuffer handed over by firmware";
    case FbSource::Ramfb:             return "ramfb, our memory scanned out by the host";
    case FbSource::RpiMailbox:        return "Raspberry Pi VideoCore Mailbox HDMI Framebuffer";
    default:                          return "none";
  }
}

void fb_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
  uint32_t* px = target_px();
  if (!px || x >= g_w || y >= g_h) return;
  px[static_cast<uint64_t>(y) * target_stride() + x] = rgb;
}

void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
  uint32_t* px = target_px();
  if (!px) return;
  if (x >= g_w || y >= g_h) return;
  if (x + w > g_w) w = g_w - x;
  if (y + h > g_h) h = g_h - y;
  uint32_t stride = target_stride();
  for (uint32_t r = 0; r < h; ++r) {
    uint32_t* line = px + static_cast<uint64_t>(y + r) * stride + x;
    for (uint32_t c = 0; c < w; ++c) line[c] = rgb;
  }
}

void fb_clear(uint32_t rgb) { fb_fill(0, 0, g_w, g_h, rgb); }

void fb_line_h(uint32_t x, uint32_t y, uint32_t w, uint32_t rgb) {
  fb_fill(x, y, w, 1, rgb);
}

void fb_line_v(uint32_t x, uint32_t y, uint32_t h, uint32_t rgb) {
  fb_fill(x, y, 1, h, rgb);
}

void fb_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill_rgb, uint32_t border_rgb) {
  if (fill_rgb != 0xFFFFFFFFu) fb_fill(x, y, w, h, fill_rgb);
  if (border_rgb != 0xFFFFFFFFu && w > 0 && h > 0) {
    fb_line_h(x, y, w, border_rgb);
    fb_line_h(x, y + h - 1, w, border_rgb);
    fb_line_v(x, y, h, border_rgb);
    fb_line_v(x + w - 1, y, h, border_rgb);
  }
}

void fb_round_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill_rgb, uint32_t border_rgb) {
  if (w < 4 || h < 4) {
    fb_box(x, y, w, h, fill_rgb, border_rgb);
    return;
  }
  if (fill_rgb != 0xFFFFFFFFu) {
    fb_fill(x + 1, y, w - 2, 1, fill_rgb);
    fb_fill(x, y + 1, w, h - 2, fill_rgb);
    fb_fill(x + 1, y + h - 1, w - 2, 1, fill_rgb);
  }
  if (border_rgb != 0xFFFFFFFFu) {
    fb_line_h(x + 1, y, w - 2, border_rgb);
    fb_line_h(x + 1, y + h - 1, w - 2, border_rgb);
    fb_line_v(x, y + 1, h - 2, border_rgb);
    fb_line_v(x + w - 1, y + 1, h - 2, border_rgb);
  }
}

void fb_box_shadow(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t depth) {
  if (!g_px || depth == 0) return;
  for (uint32_t d = 1; d <= depth; ++d) {
    uint32_t shadow_col = 0x05070A;
    fb_line_h(x + d + 2, y + h + d - 1, w - 2, shadow_col);
    fb_line_v(x + w + d - 1, y + d + 2, h - 2, shadow_col);
  }
}

void fb_chip(uint32_t x, uint32_t y, const char* text, uint32_t text_col, uint32_t bg_col, uint32_t border_col) {
  if (!text) return;
  uint32_t len = 0;
  while (text[len]) ++len;
  uint32_t chip_w = len * 8 + 16;
  uint32_t chip_h = 20;
  fb_round_box(x, y, chip_w, chip_h, bg_col, border_col);
  fb_text(x + 8, y + 2, text, text_col, 0xFFFFFFFFu, 1);
}

void fb_draw_cursor(uint32_t x, uint32_t y) {
  static const uint16_t CURSOR_MASK[16] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780
  };
  static const uint16_t CURSOR_BODY[16] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7E00, 0x7C00, 0x4C00, 0x0600, 0x0600, 0x0300, 0x0300
  };

  if (!g_px || x >= g_w || y >= g_h) return;
  /* Shadow */
  for (uint32_t r = 0; r < 16; ++r) {
    if (y + r + 1 >= g_h) break;
    uint16_t m = CURSOR_MASK[r];
    for (uint32_t c = 0; c < 16; ++c) {
      if (x + c + 1 >= g_w) break;
      if ((m >> (15 - c)) & 1) {
        fb_pixel(x + c + 1, y + r + 1, 0x05070A);
      }
    }
  }
  /* Cursor body & border */
  for (uint32_t r = 0; r < 16; ++r) {
    if (y + r >= g_h) break;
    uint16_t m = CURSOR_MASK[r];
    uint16_t b = CURSOR_BODY[r];
    for (uint32_t c = 0; c < 16; ++c) {
      if (x + c >= g_w) break;
      if ((m >> (15 - c)) & 1) {
        fb_pixel(x + c, y + r, ((b >> (15 - c)) & 1) ? 0xFFFFFF : 0x0B0E14);
      }
    }
  }
}

/* ---- Cursor as an overlay, not a reason to repaint the screen ----
 *
 * The desktop's only drawing primitive is a full repaint, so every mouse
 * motion report redrew all 1280x720 pixels - roughly a million pixel writes
 * plus every glyph on screen, in software, per report. That put the loop's
 * iteration rate under the repaint cost, which is what made the pointer feel
 * sluggish and dropped keystrokes along with it.
 *
 * Saving the ~18x18 patch under the cursor and restoring it on the next move
 * makes pointer motion cost a few hundred pixels instead of a million. */
constexpr uint32_t CUR_DIM = 18;
uint32_t g_cur_save[CUR_DIM * CUR_DIM];
bool     g_cur_saved = false;
uint32_t g_cur_sx = 0, g_cur_sy = 0, g_cur_sw = 0, g_cur_sh = 0;

void fb_cursor_invalidate() { g_cur_saved = false; }

void fb_cursor_hide() {
  if (!g_cur_saved) return;
  uint32_t* px = target_px();
  if (!px) { g_cur_saved = false; return; }
  const uint32_t stride = target_stride();
  for (uint32_t r = 0; r < g_cur_sh; ++r) {
    for (uint32_t c = 0; c < g_cur_sw; ++c) {
      px[static_cast<uint64_t>(g_cur_sy + r) * stride + g_cur_sx + c] =
          g_cur_save[r * CUR_DIM + c];
    }
  }
  fb_present(g_cur_sx, g_cur_sy, g_cur_sw, g_cur_sh);
  g_cur_saved = false;
}

void fb_cursor_show(uint32_t x, uint32_t y) {
  uint32_t* px = target_px();
  if (!px || x >= g_w || y >= g_h) return;
  uint32_t cw = CUR_DIM, chh = CUR_DIM;
  if (x + cw > g_w) cw = g_w - x;
  if (y + chh > g_h) chh = g_h - y;
  const uint32_t stride = target_stride();
  for (uint32_t r = 0; r < chh; ++r) {
    for (uint32_t c = 0; c < cw; ++c) {
      g_cur_save[r * CUR_DIM + c] = px[static_cast<uint64_t>(y + r) * stride + x + c];
    }
  }
  g_cur_sx = x; g_cur_sy = y; g_cur_sw = cw; g_cur_sh = chh;
  g_cur_saved = true;
  fb_draw_cursor(x, y);
  fb_present(x, y, cw, chh);
}

void fb_cursor_move(uint32_t x, uint32_t y) {
  fb_cursor_hide();
  fb_cursor_show(x, y);
}

void fb_gradient_v(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t top_rgb, uint32_t bot_rgb) {
  if (!g_px || h == 0) return;
  /* Signed: with unsigned math a gradient that gets DARKER downwards (every
   * panel title bar, the top bar) wrapped (r1 - r0) to ~4 billion, and the
   * bars rendered bright yellow / cream with unreadable white text. */
  const int32_t r0 = (top_rgb >> 16) & 0xFF, g0 = (top_rgb >> 8) & 0xFF, b0 = top_rgb & 0xFF;
  const int32_t r1 = (bot_rgb >> 16) & 0xFF, g1 = (bot_rgb >> 8) & 0xFF, b1 = bot_rgb & 0xFF;
  const int32_t hh = static_cast<int32_t>(h);
  for (uint32_t r = 0; r < h; ++r) {
    const int32_t ri = static_cast<int32_t>(r);
    const uint32_t red = static_cast<uint32_t>(r0 + ((r1 - r0) * ri) / hh) & 0xFFu;
    const uint32_t grn = static_cast<uint32_t>(g0 + ((g1 - g0) * ri) / hh) & 0xFFu;
    const uint32_t blu = static_cast<uint32_t>(b0 + ((b1 - b0) * ri) / hh) & 0xFFu;
    const uint32_t color = (red << 16) | (grn << 8) | blu;
    fb_fill(x, y + r, w, 1, color);
  }
}

void fb_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (!g_px) return;

  if (g_draw_px) {
    if (x >= g_w || y >= g_h) return;
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;

    for (uint32_t r = 0; r < h; ++r) {
      uint32_t* dst = g_px + static_cast<uint64_t>(y + r) * g_stride_px + x;
      const uint32_t* src = g_draw_px + static_cast<uint64_t>(y + r) * g_w + x;
      if (g_scanout_rgb_bytes) {
        for (uint32_t c = 0; c < w; ++c) {
          const uint32_t v = src[c];
          dst[c] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
        }
        continue;
      }
      /* The Pi's scanout (0x3E8xxxxx) sits in the Device-nGnRnE window,
       * where an unaligned access is an alignment FAULT, not just slow.
       * Pairing pixels from an odd x issued a 64-bit store 4 bytes off
       * alignment - every mouse move that landed the cursor on an odd
       * column killed the kernel on the spot (QEMU's framebuffer is in
       * normal RAM, which is why it never reproduced there). Peel one
       * pixel so the paired stores are always 8-byte aligned. */
      uint32_t c = 0;
      if ((reinterpret_cast<uintptr_t>(dst) & 7u) != 0 && w > 0) {
        dst[0] = src[0];
        c = 1;
      }
      for (; c + 1 < w; c += 2) {
        *reinterpret_cast<uint64_t*>(dst + c) = *reinterpret_cast<const uint64_t*>(src + c);
      }
      for (; c < w; ++c) {
        dst[c] = src[c];
      }
    }
  }

  if (g_src == FbSource::VirtioGpu) {
    gpu_flush(x, y, w, h);
    return;
  }
  /* Ensure all write buffers drain to VideoCore scanout memory */
  asm volatile("dsb sy" ::: "memory");
}

void fb_present_all() { fb_present(0, 0, g_w, g_h); }

const uint32_t* fb_pixels() { return target_px(); }
const uint32_t* fb_scanout_pixels() { return g_px; }
bool fb_scanout_rgb_bytes() { return g_scanout_rgb_bytes; }
uint32_t fb_scanout_stride() { return g_stride_px; }
uint32_t fb_stride() { return target_stride(); }

void fb_blit(const uint32_t* argb, uint32_t w, uint32_t h) {
  if (!g_px || !argb) return;
  if (w > g_w) w = g_w;
  if (h > g_h) h = g_h;
  /* The caller's frame straight onto the Pi's scanout: one copy, rows at a
   * time through memcpy (the scanout is write-combining now, where 8-byte
   * stores are fine at any alignment). This used to copy into the console's
   * DRAM backbuffer and then from it to the scanout - two passes over 8 MB
   * per desktop frame. The backbuffer is left stale; the console is
   * suspended while a full-screen client owns the glass. */
  if (g_src == FbSource::RpiMailbox && !g_scanout_rgb_bytes) {
    for (uint32_t y = 0; y < h; ++y)
      memcpy(g_px + static_cast<uint64_t>(y) * g_stride_px, argb + static_cast<uint64_t>(y) * w, static_cast<size_t>(w) * 4);
    asm volatile("dsb sy" ::: "memory");
    return;
  }
  uint32_t* px = target_px();
  uint32_t stride = target_stride();
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t* dst = px + static_cast<uint64_t>(y) * stride;
    const uint32_t* src = argb + static_cast<uint64_t>(y) * w;
    for (uint32_t x = 0; x < w; ++x) dst[x] = src[x];
  }
  mark_dirty(0, h);
  flush_dirty();
}

namespace {
struct AaGlyph { int16_t xoff, yoff; uint8_t w, h; uint16_t adv64; uint32_t off; };
struct AaFont { const AaGlyph* glyphs; const uint8_t* bits; uint8_t first, count; int16_t baseline, line_h; };
#include "aa_fonts.inc"

const AaFont& aa_font(Face f) {
  switch (f) {
    case Face::Medium:   return kAaFont_medium;
    case Face::Semibold: return kAaFont_semibold;
    case Face::Small:    return kAaFont_small;
    case Face::Large:    return kAaFont_large;
    case Face::Mono:     return kAaFont_mono;
    default:             return kAaFont_body;
  }
}

/* Coverage curve. A gamma lift (as first used here) thickens light-on-dark
 * text but spreads every edge into a soft halo. Rendered at the display's
 * native size, the crisper choice is a mild contrast curve: faint fringe
 * pixels (<~6%) drop out and near-solid ones go solid, so stems keep their
 * weight and edges stay one pixel wide. */
uint8_t g_cov_lut[256];
bool g_cov_ready = false;
void cov_init() {
  for (int i = 0; i < 256; ++i) {
    int v = (i - 14) * 255 / 226;          /* 14 -> 0, 240 -> 255 */
    g_cov_lut[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
  g_cov_ready = true;
}
}  /* namespace */

uint32_t fb_text_aa_width_n(const char* s, size_t n, Face face) {
  const AaFont& f = aa_font(face);
  uint32_t pen64 = 0;
  for (size_t i = 0; s && s[i] && i < n; ++i) {
    unsigned c = static_cast<unsigned char>(s[i]);
    if (c < f.first || c >= static_cast<unsigned>(f.first + f.count)) c = '?';
    pen64 += f.glyphs[c - f.first].adv64;
  }
  return (pen64 + 32) / 64;
}

uint32_t fb_text_aa_width(const char* s, Face face) { return fb_text_aa_width_n(s, ~static_cast<size_t>(0), face); }

uint32_t fb_text_aa(uint32_t x, uint32_t y, const char* s, uint32_t rgb, Face face) {
  uint32_t* px = target_px();
  if (!px || !s) return x;
  if (!g_cov_ready) cov_init();
  const AaFont& f = aa_font(face);
  const uint32_t stride = target_stride();
  const int32_t base_y = static_cast<int32_t>(y) + f.baseline;
  const int32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
  uint64_t pen64 = static_cast<uint64_t>(x) * 64;
  for (const char* p = s; *p; ++p) {
    unsigned c = static_cast<unsigned char>(*p);
    if (c < f.first || c >= static_cast<unsigned>(f.first + f.count)) c = '?';
    const AaGlyph& g = f.glyphs[c - f.first];
    const int32_t gx = static_cast<int32_t>((pen64 + 32) / 64) + g.xoff;
    const int32_t gy = base_y + g.yoff;
    const uint8_t* bm = f.bits + g.off;
    for (int32_t r = 0; r < g.h; ++r) {
      const int32_t yy = gy + r;
      if (yy < 0 || yy >= static_cast<int32_t>(g_h)) continue;
      uint32_t* line = px + static_cast<uint64_t>(yy) * stride;
      for (int32_t q = 0; q < g.w; ++q) {
        const int32_t xx = gx + q;
        if (xx < 0 || xx >= static_cast<int32_t>(g_w)) continue;
        const uint32_t a = g_cov_lut[bm[r * g.w + q]];
        if (!a) continue;
        if (a == 255) { line[xx] = rgb & 0xFFFFFFu; continue; }
        const uint32_t d = line[xx];
        const int32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
        const uint32_t nr = static_cast<uint32_t>(dr + ((sr - dr) * static_cast<int32_t>(a) + 127) / 255);
        const uint32_t ng = static_cast<uint32_t>(dg + ((sg - dg) * static_cast<int32_t>(a) + 127) / 255);
        const uint32_t nb = static_cast<uint32_t>(db + ((sb - db) * static_cast<int32_t>(a) + 127) / 255);
        line[xx] = (nr << 16) | (ng << 8) | nb;
      }
    }
    pen64 += g.adv64;
  }
  return static_cast<uint32_t>((pen64 + 32) / 64);
}

void fb_text(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, uint32_t scale) {
  if (!g_px || !s || !scale) return;
  for (const char* p = s; *p; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    const unsigned idx = (ch >= 32 && ch <= 126) ? ch - 32u : 0u;
    for (uint32_t r = 0; r < GLYPH_H; ++r) {
      const uint8_t bits = FONT8X16[idx][r];
      for (uint32_t c = 0; c < GLYPH_W; ++c) {
        const uint32_t rgb = (bits >> (7 - c)) & 1 ? fg : bg;
        if (rgb == bg && bg == 0xFFFFFFFFu) continue;   /* transparent background */
        for (uint32_t sy = 0; sy < scale; ++sy)
          for (uint32_t sx = 0; sx < scale; ++sx)
            fb_pixel(x + c * scale + sx, y + r * scale + sy, rgb);
      }
    }
    x += GLYPH_W * scale;
  }
}

void fbcon_reset() {
  if (!g_px) return;
  fb_clear(g_bg);
  fb_fill(0, 0, g_w, 4 * g_scale, 0x3DA9FC);
  g_cx = 0;
  g_cy = 1;
  g_dirty = false;
  mark_dirty(0, g_h);
  flush_dirty();
}

void fbcon_flush() {
  if (!g_con_live || g_in_con) return;
  g_in_con = true;
  flush_dirty();
  g_in_con = false;
}

void fbcon_suspend() { g_con_live = false; }
uint32_t* fb_scanout(uint32_t* stride_px) {
  if (stride_px) *stride_px = g_stride_px;
  return (g_src == FbSource::RpiMailbox && !g_scanout_rgb_bytes) ? g_px : nullptr;
}
bool fbcon_is_live() { return g_con_live; }

void fbcon_resume() {
  if (!g_px || g_con_live) return;
  fbcon_reset();
  g_con_live = true;
}

void fbcon_write(const char* s, size_t n) {
  if (!g_con_live || g_in_con) return;
  g_in_con = true;

  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (c == '\r') { g_cx = 0; continue; }
    if (c == '\n') {
      g_cx = 0;
      ++g_cy;
      if (g_cy >= g_rows) { scroll_one_line(); g_cy = g_rows - 1; }
      continue;
    }
    if (c == '\b') { if (g_cx) --g_cx; continue; }
    if (c < 32 || c > 126) continue;

    if (g_cx >= g_cols) {
      g_cx = 0;
      ++g_cy;
      if (g_cy >= g_rows) { scroll_one_line(); g_cy = g_rows - 1; }
    }
    const uint32_t px = g_cx * GLYPH_W * g_scale;
    const uint32_t py = g_cy * GLYPH_H * g_scale;
    const char one[2] = {c, '\0'};
    fb_fill(px, py, GLYPH_W * g_scale, GLYPH_H * g_scale, g_bg);
    fb_text(px, py, one, g_fg, g_bg, g_scale);
    mark_dirty(py, py + GLYPH_H * g_scale);
    ++g_cx;
  }

  /* Flushing every character would mean two virtio round trips per glyph. A
   * line at a time is what makes the console usable rather than a slideshow. */
  bool has_newline = false;
  for (size_t i = 0; i < n; ++i) if (s[i] == '\n') has_newline = true;
  if (has_newline) flush_dirty();

  g_in_con = false;
}

bool fb_init() {
  const FdtInfo& f = fdt_info();

  /* Firmware-configured panel first: if something already lit the display,
   * taking it over is both less work and less likely to turn it off. */
  if (f.fb_base && f.fb_width && f.fb_height) {
    const uint64_t bytes = f.fb_size ? f.fb_size
                                     : static_cast<uint64_t>(f.fb_stride) * f.fb_height;
    /* Outside DRAM it is device memory and has to be mapped as such. */
    if (f.fb_base < f.ram_base || f.fb_base >= f.ram_base + f.ram_size)
      mmu_map(f.fb_base, f.fb_base, bytes, MAP_DEVICE);
    g_px = reinterpret_cast<uint32_t*>(f.fb_base);
    g_w = f.fb_width;
    g_h = f.fb_height;
    g_stride_px = f.fb_stride ? f.fb_stride / 4 : f.fb_width;
    g_src = FbSource::SimpleFramebuffer;
  } else {
    RpiFbInfo rpi_fb;
    /* Render at the display's own resolution. A fixed 1280x720 framebuffer
     * on a 1080p monitor is rescaled 1.5x by the GPU, which softens every
     * edge - no font rendering can undo that. Clamp to 1920x1200 so the
     * software renderer stays fast. */
    uint32_t want_w = 1280, want_h = 720, dw = 0, dh = 0;
    if (rpi_get_display_size(dw, dh) && dw >= 800 && dh >= 600 && dw <= 1920 && dh <= 1200) {
      want_w = dw; want_h = dh;
    }
    kprintf("[fb] display reports %ux%u; framebuffer %ux%u\n", static_cast<uint64_t>(dw), static_cast<uint64_t>(dh),
            static_cast<uint64_t>(want_w), static_cast<uint64_t>(want_h));
    if (rpi_fb_init(want_w, want_h, 32, rpi_fb) || rpi_fb_init(1280, 720, 32, rpi_fb) || rpi_fb_init(1024, 768, 32, rpi_fb)) {
      mmu_map(rpi_fb.arm_addr, rpi_fb.arm_addr, rpi_fb.fb_size, MAP_WC);   /* write-combining; see metal_mmu_init */
      g_px = rpi_fb.pixels;
      g_w = rpi_fb.width;
      g_h = rpi_fb.height;
      g_stride_px = rpi_fb.pitch / 4;
      g_src = FbSource::RpiMailbox;
      g_scanout_rgb_bytes = (rpi_fb.pixel_order == 1);
      kprintf("[fb] firmware pixel order: %s%s\n", rpi_fb.pixel_order == 1 ? "RGB bytes" : "BGR bytes (0x00RRGGBB words)",
              g_scanout_rgb_bytes ? " - swapping red/blue on present" : "");
    } else if (gpu_init()) {
      g_px = gpu_pixels();
      g_w = gpu_width();
      g_h = gpu_height();
      g_stride_px = g_w;
      g_src = FbSource::VirtioGpu;
    } else {
      /* Last: a linear framebuffer we own and something else scans out. */
      constexpr uint32_t W = 1280, H = 800;
      auto* px = static_cast<uint32_t*>(metal_alloc_pages(static_cast<uint64_t>(W) * H * 4));
      if (px && ramfb_setup(px, W, H)) {
        g_px = px;
        g_w = W;
        g_h = H;
        g_stride_px = W;
        g_src = FbSource::Ramfb;
      } else {
        if (px) metal_free_pages(px, static_cast<uint64_t>(W) * H * 4);
        kputs("[fb] no display: no simple-framebuffer, no rpi-mailbox, no virtio-gpu, no ramfb\n");
        return false;
      }
    }
  }

  /* Allocate in-DRAM backbuffer to eliminate scanout flicker */
  const uint64_t fb_bytes = static_cast<uint64_t>(g_w) * g_h * 4;
  g_draw_px = static_cast<uint32_t*>(metal_alloc_pages(fb_bytes));

  /* 8x16 glyphs are unreadable on a panel this size from a car seat, and
   * unreadable in a screenshot - but doubling them on THIS board's actual
   * 1280x720 HDMI output left an 80x22 console: half the usable text of a
   * normal terminal, and every character oversized on a monitor sat at, not
   * across, a desk. 1024 was catching ordinary desk-distance panels that
   * needed the opposite of what this was for; only double once a panel is
   * genuinely large enough that 1x would be the illegible choice instead. */
  g_scale = (g_w >= 1920) ? 2 : 1;
  g_cols = g_w / (GLYPH_W * g_scale);
  g_rows = g_h / (GLYPH_H * g_scale);

  fb_clear(g_bg);

  /* A title bar, so a photograph of the screen says what it is. */
  fb_fill(0, 0, g_w, 4 * g_scale, 0x3DA9FC);
  g_cy = 1;
  g_con_live = true;
  metal_console_mirror(fbcon_write);

  kprintf("[fb] %u x %u via %s, console %u x %u at %ux\n", g_w, g_h, fb_source_name(),
          g_cols, g_rows, g_scale);
  fb_present_all();
  return true;
}

void fb_report() {
  if (!fb_live()) {
    kputs("  no display. Add -device virtio-gpu-device to QEMU, or boot a machine\n"
          "  whose firmware publishes a simple-framebuffer node.\n");
    return;
  }
  char buf[32];
  kprintf("  source     %s\n", fb_source_name());
  if (g_src == FbSource::RpiMailbox) {
    kprintf("  pixel order %s\n", g_scanout_rgb_bytes ? "RGB bytes (firmware refused BGR) - red/blue swapped on present"
                                                      : "BGR bytes = 0x00RRGGBB words, as drawn");
  }
  kprintf("  geometry   %u x %u, stride %u px, %s\n", g_w, g_h, g_stride_px,
          khuman(static_cast<uint64_t>(g_stride_px) * g_h * 4, buf, sizeof(buf)));
  kprintf("  pixels at  %p\n", reinterpret_cast<uint64_t>(g_px));
  kprintf("  console    %u x %u glyphs at %ux magnification\n", g_cols, g_rows, g_scale);
  if (g_src == FbSource::VirtioGpu) {
    kprintf("  virtio-gpu v%u at %p\n", static_cast<uint64_t>(gpu_version()), gpu_base());
    const uint64_t n = gpu_flushes();
    kprintf("  %u flushes, %u us total, %u us each\n", n, gpu_flush_ns() / 1000,
            n ? gpu_flush_ns() / n / 1000 : 0);
  }
  if (g_src == FbSource::Ramfb)
    kprintf("  fw_cfg at  %p\n", ramfb_fwcfg_base());
  const FdtInfo& f = fdt_info();
  if (f.fb_base)
    kprintf("  device tree simple-framebuffer: %p %u x %u stride %u format %s\n",
            f.fb_base, f.fb_width, f.fb_height, f.fb_stride,
            f.fb_format[0] ? f.fb_format : "(unstated)");
}

/* Paint something that can only look right if the geometry, the pixel format
 * and the flush path are all correct, then put the console back. */
void fb_demo() {
  if (!fb_live()) { fb_report(); return; }

  /* Whatever this prints goes to the serial port only until the next command,
   * so the pattern is what is actually on the glass. */
  fbcon_suspend();

  const uint64_t t0 = metal_now_ns();
  fb_clear(0x05070C);

  /* A full-width RGB sweep: wrong channel order shows up instantly. */
  const uint32_t band = g_h / 6;
  for (uint32_t x = 0; x < g_w; ++x) {
    const uint32_t t = x * 255 / (g_w - 1);
    fb_fill(x, band, 1, band / 2, (t << 16));                 /* red   */
    fb_fill(x, band + band / 2, 1, band / 2, (t << 8));       /* green */
    fb_fill(x, band * 2, 1, band / 2, t);                     /* blue  */
    fb_fill(x, band * 2 + band / 2, 1, band / 2, (t << 16) | (t << 8) | t);
  }

  /* Eight saturated bars, so a stuck bit is visible. */
  const uint32_t bars[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                            0xFF00FF, 0xFF0000, 0x0000FF, 0x202020};
  const uint32_t bw = g_w / 8;
  for (uint32_t i = 0; i < 8; ++i) fb_fill(i * bw, band * 3, bw, band, bars[i]);

  /* A one-pixel grid: any stride error turns this into diagonal stripes. */
  for (uint32_t x = 0; x < g_w; x += 32) fb_fill(x, band * 4, 1, band, 0x304050);
  for (uint32_t y = band * 4; y < band * 5 && y < g_h; y += 32) fb_fill(0, y, g_w, 1, 0x304050);

  const uint64_t t_draw = metal_now_ns() - t0;

  fb_text(16, 16, "AIOS - AArch64 bare metal", 0x3DA9FC, 0x05070C, g_scale + 1);
  char line[96];
  {
    /* No snprintf under here; assemble by hand. */
    unsigned i = 0;
    const char* a = "display: ";
    while (*a) line[i++] = *a++;
    const char* b = fb_source_name();
    while (*b && i < sizeof(line) - 1) line[i++] = *b++;
    line[i] = '\0';
  }
  fb_text(16, 16 + 20 * (g_scale + 1), line, 0xB8C4D0, 0x05070C, g_scale);

  const uint64_t t1 = metal_now_ns();
  fb_present_all();
  const uint64_t t_flush = metal_now_ns() - t1;

  const uint64_t bytes = static_cast<uint64_t>(g_w) * g_h * 4;
  char buf[32];
  kprintf("  drew %u x %u (%s) in %u us\n", g_w, g_h, khuman(bytes, buf, sizeof(buf)),
          t_draw / 1000);
  kprintf("  full-screen flush to the glass: %u us", t_flush / 1000);
  if (t_flush) kprintf(" (%u MiB/s)", bytes * 1000000ull / t_flush / 1048576ull);
  kputs("\n  the pattern is on screen; press enter to return to the console\n");
}
