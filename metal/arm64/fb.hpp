#pragma once
/* The display: a linear framebuffer, wherever it came from, plus the text
 * console painted into it.
 *
 * Two sources, because the two targets that matter get a display two different
 * ways. On QEMU virt (and on any board where nothing has touched the display
 * controller) the kernel drives virtio-gpu itself. On a board whose firmware
 * already brought the panel up, the device tree carries a `simple-framebuffer`
 * node and the kernel just paints into memory someone else configured — which
 * is the whole point of going to a board with working firmware before going to
 * one where the display controller is ours to write.
 */
#include <cstddef>
#include <cstdint>

enum class FbSource : uint8_t { None, VirtioGpu, SimpleFramebuffer, Ramfb, RpiMailbox };

/* Bring up whichever display this machine offers. Safe to call with neither. */
bool fb_init();
bool fb_live();
FbSource fb_source();
const char* fb_source_name();
uint32_t fb_width();
uint32_t fb_height();
uint32_t fb_scale();          /* glyph magnification chosen from the panel size */

/* 0x00RRGGBB. */
void fb_pixel(uint32_t x, uint32_t y, uint32_t rgb);
void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_clear(uint32_t rgb);
void fb_line_h(uint32_t x, uint32_t y, uint32_t w, uint32_t rgb);
void fb_line_v(uint32_t x, uint32_t y, uint32_t h, uint32_t rgb);
void fb_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill_rgb, uint32_t border_rgb);
void fb_round_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill_rgb, uint32_t border_rgb);
void fb_box_shadow(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t depth);
void fb_gradient_v(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t top_rgb, uint32_t bot_rgb);
void fb_chip(uint32_t x, uint32_t y, const char* text, uint32_t text_col, uint32_t bg_col, uint32_t border_col);
void fb_draw_cursor(uint32_t x, uint32_t y);

/* Cursor overlay. fb_cursor_move repaints only the ~18x18 patch the pointer
 * occupies, instead of forcing a full-screen redraw for every motion report.
 * Call fb_cursor_invalidate() after any full repaint - the saved background
 * underneath is stale at that point. */
void fb_cursor_invalidate();
void fb_cursor_hide();
void fb_cursor_show(uint32_t x, uint32_t y);
void fb_cursor_move(uint32_t x, uint32_t y);

/* Push a rectangle to the glass. On a simple-framebuffer this is free. */
void fb_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void fb_present_all();

/* Copy a full 0x00RRGGBB buffer (w x h, tightly packed) onto the display and
 * present it. For a compositor that renders into its own offscreen buffer
 * (aios::userland::HardwareCompositor::framebuffer()) rather than drawing
 * through fb_pixel/fb_fill one call at a time. */
void fb_blit(const uint32_t* argb, uint32_t w, uint32_t h);

/* Read-back for screenshot.cpp: the pixels actually on screen right now
 * (the backbuffer when one is in use, the scanout buffer otherwise) and the
 * stride to read them at - NOT always == fb_width(), see fb.cpp's g_stride_px. */
const uint32_t* fb_pixels();
/* The buffer actually scanned out to HDMI, and whether it stores bytes
 * R,G,B (words 0x00BBGGRR) - so a screenshot can show what the monitor
 * shows, not what the drawing code intended. */
const uint32_t* fb_scanout_pixels();
bool fb_scanout_rgb_bytes();
uint32_t fb_scanout_stride();
uint32_t fb_stride();

/* Text, in the 8x16 font the x86 framebuffer console already uses. */
void fb_text(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, uint32_t scale);

/* Anti-aliased proportional text (Inter / JetBrains Mono atlases, see
 * metal/fonts/gen_fonts.py). y is the top of the line box, as for fb_text. */
enum class Face : uint8_t { Body, Medium, Semibold, Small, Large, Mono };
/* Draws s and returns the x just past the last glyph. */
uint32_t fb_text_aa(uint32_t x, uint32_t y, const char* s, uint32_t rgb, Face face);
uint32_t fb_text_aa_width(const char* s, Face face);
/* Width of the first n characters. */
uint32_t fb_text_aa_width_n(const char* s, size_t n, Face face);

/* The console. fbcon_write is what metal_console_write mirrors into, so the
 * shell appears on the screen and the serial port at the same time. */
void fbcon_write(const char* s, size_t n);
void fbcon_reset();
/* Stop/resume painting the console into the framebuffer. `gfx` suspends it so
 * its own result lines do not land on top of the pattern it just drew. */
void fbcon_suspend();
bool fbcon_is_live();
/* The Pi's scanout itself (for drawing a pointer straight onto the glass),
 * or null when there is no plain 0x00RRGGBB scanout to draw on. */
uint32_t* fb_scanout(uint32_t* stride_px);
void fbcon_resume();
/* Push anything drawn but not yet sent to the glass. Console output is flushed
 * a line at a time — a flush per character would be two virtio round trips per
 * glyph — so a prompt, which has no trailing newline, needs this before the
 * shell blocks for input or it never appears on screen. */
void fbcon_flush();
uint32_t fbcon_cols();
uint32_t fbcon_rows();

void fb_report();
void fb_demo();
