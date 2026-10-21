/* Freestanding C runtime for the AArch64 bare-metal kernel.
 *
 * aarch64-elf-gcc ships libgcc and nothing else — no libc, no libstdc++, not
 * even a memcpy. GCC still emits calls to memcpy/memset/memmove/memcmp from
 * ordinary struct assignments and array initialisation under -ffreestanding,
 * so these four are not optional: without them the kernel links with undefined
 * symbols or, worse, links against a stray definition and faults later.
 */
#include "kio.hpp"
#include "uart.hpp"

/* Word-at-a-time, every access ALIGNED. These were byte loops: memcpy moved
 * 344 MB/s on the Pi (58x the host, against ~4x for plain compute - see
 * `bench`), and every string, vector, blit and packet in the kernel goes
 * through them. Alignment is not optional: the framebuffer is Device memory,
 * where an unaligned access faults, so when source and destination disagree
 * mod 8 the source is read as aligned words and shifted into place (reading
 * at most the rest of the aligned word holding the last byte - never past a
 * page). no-tree-loop-distribute-patterns keeps GCC from recognising these
 * loops as memcpy/memset and compiling them into calls to themselves. */
#define KRT_MEM __attribute__((optimize("no-tree-loop-distribute-patterns")))
namespace {
using u64 = unsigned long long;
using u8 = unsigned char;
inline bool al8(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 7) == 0; }
}  // namespace

extern "C" {

KRT_MEM void* memset(void* s, int c, size_t n) {
  auto* p = static_cast<u8*>(s);
  const u8 v = static_cast<u8>(c);
  while (n && !al8(p)) { *p++ = v; --n; }
  const u64 w = 0x0101010101010101ull * v;
  auto* q = reinterpret_cast<u64*>(p);
  while (n >= 64) { q[0] = w; q[1] = w; q[2] = w; q[3] = w; q[4] = w; q[5] = w; q[6] = w; q[7] = w; q += 8; n -= 64; }
  while (n >= 8) { *q++ = w; n -= 8; }
  p = reinterpret_cast<u8*>(q);
  while (n--) *p++ = v;
  return s;
}

KRT_MEM void* memcpy(void* dest, const void* src, size_t n) {
  auto* d = static_cast<u8*>(dest);
  const auto* s = static_cast<const u8*>(src);
  if (n >= 16) {
    while (!al8(d)) { *d++ = *s++; --n; }
    auto* dw = reinterpret_cast<u64*>(d);
    if (al8(s)) {
      const auto* sw = reinterpret_cast<const u64*>(s);
      while (n >= 64) {
        const u64 a = sw[0], b = sw[1], c = sw[2], e = sw[3], f = sw[4], g = sw[5], h = sw[6], i = sw[7];
        dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e; dw[4] = f; dw[5] = g; dw[6] = h; dw[7] = i;
        dw += 8; sw += 8; n -= 64;
      }
      while (n >= 8) { *dw++ = *sw++; n -= 8; }
      s = reinterpret_cast<const u8*>(sw);
    } else {
      const unsigned sh = static_cast<unsigned>(reinterpret_cast<uintptr_t>(s) & 7) * 8;
      const auto* sw = reinterpret_cast<const u64*>(reinterpret_cast<uintptr_t>(s) & ~static_cast<uintptr_t>(7));
      u64 lo = *sw++;
      size_t words = 0;
      while (n >= 8) { const u64 hi = *sw++; *dw++ = (lo >> sh) | (hi << (64 - sh)); lo = hi; n -= 8; ++words; }
      s += words * 8;
    }
    d = reinterpret_cast<u8*>(dw);
  }
  while (n--) *d++ = *s++;
  return dest;
}

KRT_MEM void* memmove(void* dest, const void* src, size_t n) {
  auto* d = static_cast<u8*>(dest);
  const auto* s = static_cast<const u8*>(src);
  if (d == s || n == 0) return dest;
  /* Forward is safe whenever the destination starts below the source:
   * memcpy loads each block before storing over anything it still needs. */
  if (d < s || d >= s + n) return memcpy(dest, src, n);
  d += n; s += n;
  if (((reinterpret_cast<uintptr_t>(d) ^ reinterpret_cast<uintptr_t>(s)) & 7) == 0) {
    while (n && !al8(d)) { *--d = *--s; --n; }
    auto* dw = reinterpret_cast<u64*>(d);
    const auto* sw = reinterpret_cast<const u64*>(s);
    while (n >= 8) { *--dw = *--sw; n -= 8; }
    d = reinterpret_cast<u8*>(dw); s = reinterpret_cast<const u8*>(sw);
  }
  while (n--) *--d = *--s;
  return dest;
}

KRT_MEM int memcmp(const void* a, const void* b, size_t n) {
  const auto* x = static_cast<const u8*>(a);
  const auto* y = static_cast<const u8*>(b);
  if (al8(x) && al8(y)) {
    while (n >= 8 && *reinterpret_cast<const u64*>(x) == *reinterpret_cast<const u64*>(y)) { x += 8; y += 8; n -= 8; }
  }
  while (n--) {
    if (*x != *y) return static_cast<int>(*x) - static_cast<int>(*y);
    ++x; ++y;
  }
  return 0;
}

size_t strlen(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return static_cast<int>(static_cast<unsigned char>(*a)) -
         static_cast<int>(static_cast<unsigned char>(*b));
}

int strncmp(const char* a, const char* b, size_t n) {
  while (n && *a && *a == *b) { ++a; ++b; --n; }
  if (n == 0) return 0;
  return static_cast<int>(static_cast<unsigned char>(*a)) -
         static_cast<int>(static_cast<unsigned char>(*b));
}

/* -fno-stack-protector is in the build flags, but a toolchain built with
 * --enable-default-ssp will still reference these from libgcc paths. */
uintptr_t __stack_chk_guard = 0x5a5a5a5a5a5a5a5aull;
void __stack_chk_fail() {
  kputs("\n[panic] stack check failed\n");
  for (;;) asm volatile("wfe");
}

void __cxa_pure_virtual() {
  kputs("\n[panic] pure virtual call\n");
  for (;;) asm volatile("wfe");
}

int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void* __dso_handle = nullptr;

}  /* extern "C" */

/* ---- console formatting ------------------------------------------------ */

namespace {
/* A SMALL STACK, not a single global: a UDP console command's dispatch
 * (net.cpp's handle_console_packet) wraps dispatch_shell_command() in a
 * capture, and a command that itself blocks and pumps net_poll() from
 * inside that call (compositor_run()'s frame loop, once it started
 * polling net_poll() to keep the console alive while `desktop` runs) can
 * receive and dispatch a SECOND console command before the first one's
 * capture has been stopped - confirmed on real hardware: `desktop` left
 * the console dead to every command sent afterward, including `desktop
 * exit` meant to get out of it, because the nested capture clobbered the
 * outer one's still-in-progress buffer pointer. Depth 4 is generous - real
 * nesting here is two deep (an outer blocking command's own net_poll()
 * dispatching one inner command at a time), not unbounded recursion. */
constexpr size_t kCaptureStackDepth = 4;
struct CaptureFrame { char* buf; size_t cap; size_t len; };
CaptureFrame g_cap_stack[kCaptureStackDepth];
int g_cap_top = -1;  /* -1 = nothing captured right now */

/* While a network console command runs (a capture is open), its output is
 * the reply; the serial UART only gets a mirror of the first 4 KB. The
 * mirror is synchronous at 115200 baud, so a 48 KB reply (a file pulled
 * with `fs get`) spent 4.2 s on the wire nobody was reading before the
 * network got a byte. What is not mirrored is counted, and said, once. */
constexpr size_t kUartMirrorBudget = 4096;
size_t g_uart_left = 0;
size_t g_uart_skipped = 0;
void console_out(const char* s, size_t n) {
  if (g_cap_top < 0) { metal_console_write(s, n); return; }
  const size_t allow = n < g_uart_left ? n : g_uart_left;
  if (allow) metal_console_write(s, allow);
  g_uart_left -= allow;
  g_uart_skipped += n - allow;
}

void cap_append(const char* s, size_t n) {
  if (g_cap_top < 0) return;
  CaptureFrame& f = g_cap_stack[g_cap_top];
  for (size_t i = 0; i < n && f.len + 1 < f.cap; ++i) f.buf[f.len++] = s[i];
}

/* ---- Persistent on-screen kernel log ----
 *
 * Everything kputs/kprintf writes also lands here, split into lines. The
 * serial console is the only other place this text exists, and an operator
 * looking at the HDMI output has no way to read that - which meant every
 * diagnostic this kernel prints was invisible to the person actually
 * standing in front of the machine. */
constexpr size_t KLOG_LINES = 256;
constexpr size_t KLOG_COLS  = 100;

char g_klog[KLOG_LINES][KLOG_COLS];
size_t g_klog_head = 0;   /* next line to write */
size_t g_klog_count = 0;  /* lines written, saturating at KLOG_LINES */
size_t g_klog_col = 0;    /* cursor within the line being built */

void klog_newline() {
  g_klog[g_klog_head][g_klog_col] = '\0';
  g_klog_head = (g_klog_head + 1) % KLOG_LINES;
  if (g_klog_count < KLOG_LINES) ++g_klog_count;
  g_klog_col = 0;
  g_klog[g_klog_head][0] = '\0';
}

void klog_putc(char c) {
  if (c == '\r') return;
  if (c == '\n') { klog_newline(); return; }
  if (g_klog_col + 1 < KLOG_COLS) {
    g_klog[g_klog_head][g_klog_col++] = c;
    g_klog[g_klog_head][g_klog_col] = '\0';
  }
}

void klog_append(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) klog_putc(s[i]);
}
}  /* namespace */

size_t kconsole_log(const char** out_lines, size_t max_lines, size_t skip_newest,
                    const char* prefix) {
  if (!out_lines || max_lines == 0) return 0;

  /* Walk newest -> oldest. The in-progress line counts as the newest one
   * when it has anything in it, so a message that has not ended in \n yet
   * is still visible. */
  size_t filled = 0;
  size_t skipped = 0;
  const size_t total = g_klog_count + (g_klog_col > 0 ? 1 : 0);

  for (size_t i = 0; i < total && filled < max_lines; ++i) {
    size_t idx;
    if (i == 0 && g_klog_col > 0) {
      idx = g_klog_head;
    } else {
      const size_t back = (g_klog_col > 0) ? i : i + 1;
      idx = (g_klog_head + KLOG_LINES - back) % KLOG_LINES;
    }
    const char* line = g_klog[idx];
    if (line[0] == '\0') continue;

    if (prefix && prefix[0]) {
      size_t k = 0;
      while (prefix[k] && line[k] == prefix[k]) ++k;
      if (prefix[k] != '\0') continue;  /* no match */
    }
    if (skipped < skip_newest) { ++skipped; continue; }
    out_lines[filled++] = line;
  }
  return filled;
}

void kconsole_capture_start(char* buf, size_t capacity) {
  if (g_cap_top + 1 >= static_cast<int>(kCaptureStackDepth)) return;  /* deeper than any real nesting goes - drop silently rather than corrupt the stack */
  if (g_cap_top < 0) { g_uart_left = kUartMirrorBudget; g_uart_skipped = 0; }
  ++g_cap_top;
  CaptureFrame& f = g_cap_stack[g_cap_top];
  f.buf = buf; f.cap = capacity; f.len = 0;
  if (f.buf && f.cap) f.buf[0] = '\0';
}

size_t kconsole_capture_stop() {
  if (g_cap_top < 0) return 0;  /* stop() with nothing on the stack - start() was refused (depth), nothing to report */
  CaptureFrame& f = g_cap_stack[g_cap_top];
  if (f.buf && f.len < f.cap) f.buf[f.len] = '\0';
  size_t n = f.len;
  --g_cap_top;
  if (g_cap_top < 0 && g_uart_skipped) {
    char note[80] = "[console: ";
    size_t k = 10;
    char digits[24]; int d = 0;
    for (size_t v = g_uart_skipped; v || d == 0; v /= 10) digits[d++] = static_cast<char>('0' + v % 10);
    while (d) note[k++] = digits[--d];
    const char* tail = " more bytes went to the network reply only]\n";
    while (*tail && k + 1 < sizeof(note)) note[k++] = *tail++;
    metal_console_write(note, k);
    g_uart_skipped = 0;
  }
  return n;
}

void kputc(char c) { console_out(&c, 1); cap_append(&c, 1); klog_putc(c); }

void kputs(const char* s) {
  if (!s) { console_out("(null)", 6); cap_append("(null)", 6); klog_append("(null)", 6); return; }
  size_t n = 0;
  while (s[n]) ++n;
  console_out(s, n);
  cap_append(s, n);
  klog_append(s, n);
}

void kputu(uint64_t v, unsigned base, unsigned width, char pad) {
  static const char digits[] = "0123456789abcdef";
  char tmp[65];
  unsigned i = 0;
  if (base < 2 || base > 16) base = 10;
  if (v == 0) tmp[i++] = '0';
  while (v) { tmp[i++] = digits[v % base]; v /= base; }
  while (i < width && i < sizeof(tmp)) tmp[i++] = pad;
  while (i) kputc(tmp[--i]);
}

void kputi(int64_t v) {
  if (v < 0) { kputc('-'); kputu(static_cast<uint64_t>(-v), 10, 0, ' '); }
  else kputu(static_cast<uint64_t>(v), 10, 0, ' ');
}

void kprintf(const char* fmt, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') { kputc(*p); continue; }
    ++p;
    char pad = ' ';
    unsigned width = 0;
    bool left_align = false;
    if (*p == '-') { left_align = true; ++p; }
    if (*p == '0') { pad = '0'; ++p; }
    while (*p >= '0' && *p <= '9') { width = width * 10 + static_cast<unsigned>(*p - '0'); ++p; }
    while (*p == 'l' || *p == 'z' || *p == 'h') ++p;
    switch (*p) {
      case 's': {
        const char* s = __builtin_va_arg(ap, const char*);
        if (!s) s = "(null)";
        size_t slen = 0;
        while (s[slen]) ++slen;
        if (!left_align) {
          while (slen < width) { kputc(' '); ++slen; }
        }
        kputs(s);
        if (left_align) {
          while (slen < width) { kputc(' '); ++slen; }
        }
        break;
      }
      case 'c': kputc(static_cast<char>(__builtin_va_arg(ap, int))); break;
      case 'd':
      case 'i': kputi(__builtin_va_arg(ap, int64_t)); break;
      case 'u': kputu(__builtin_va_arg(ap, uint64_t), 10, width, pad); break;
      case 'x': kputu(__builtin_va_arg(ap, uint64_t), 16, width, pad); break;
      case 'p': kputs("0x"); kputu(__builtin_va_arg(ap, uint64_t), 16, 16, '0'); break;
      case '%': kputc('%'); break;
      case '\0': --p; break;
      default: kputc('%'); kputc(*p); break;
    }
  }
  __builtin_va_end(ap);
}

const char* khuman(uint64_t bytes, char* buf, size_t n) {
  static const char* unit[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  unsigned u = 0;
  uint64_t whole = bytes;
  uint64_t frac = 0;
  while (whole >= 1024 && u < 4) {
    frac = ((whole % 1024) * 10) / 1024;
    whole /= 1024;
    ++u;
  }
  size_t i = 0;
  char tmp[24];
  unsigned t = 0;
  uint64_t w = whole;
  if (w == 0) tmp[t++] = '0';
  while (w) { tmp[t++] = static_cast<char>('0' + w % 10); w /= 10; }
  while (t && i + 1 < n) buf[i++] = tmp[--t];
  if (u > 0 && i + 2 < n) { buf[i++] = '.'; buf[i++] = static_cast<char>('0' + frac); }
  if (i + 1 < n) buf[i++] = ' ';
  for (const char* s = unit[u]; *s && i + 1 < n; ++s) buf[i++] = *s;
  buf[i] = '\0';
  return buf;
}
