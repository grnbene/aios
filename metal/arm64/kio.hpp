#pragma once
/* Console formatting for the AArch64 bare-metal kernel.
 *
 * There is no libc under this kernel, so the only way a number reaches the
 * serial port is through here. Keeping it in one place is also what stops the
 * boot log from being a wall of string literals: everything printed below is
 * something the machine measured.
 */
#include <cstddef>
#include <cstdint>

void kputc(char c);
void kputs(const char* s);
/* Unsigned value in `base` (2..16), left-padded with `pad` to `width`. */
void kputu(uint64_t v, unsigned base, unsigned width, char pad);
void kputi(int64_t v);
/* %s %c %d %i %u %x %p %% — width as %8u / %04x, and `l`/`z` accepted and
 * ignored because everything here is already 64-bit. */
void kprintf(const char* fmt, ...);

/* Bytes as a human-sized string, into a caller-owned buffer. */
const char* khuman(uint64_t bytes, char* buf, size_t n);

/* Console output capture, for anything that wants to grab what a shell
 * command prints instead of (as well as) the physical UART - e.g. the
 * network console replying to a remote command over UDP. Every kputc/kputs
 * call between start and stop is mirrored into `buf`, still going to the
 * real console too. Properly nestable (a small fixed-depth stack, see
 * krt.cpp) - a command that blocks and pumps net_poll() itself (the
 * `desktop` compositor's own frame loop, so the console stays alive while
 * it runs) can receive and dispatch a second command before the first
 * one's capture has stopped, and each nested start/stop only ever affects
 * its own frame. */
void kconsole_capture_start(char* buf, size_t capacity);
size_t kconsole_capture_stop();

/* The last 256 lines this kernel printed, newest first, for showing on the
 * framebuffer. Nobody watching the HDMI output can read the serial console,
 * so without this every diagnostic the kernel prints is invisible to the
 * person actually in front of the machine.
 *   skip_newest - scroll back by this many lines
 *   prefix      - only lines starting with this (e.g. "[eth]"), or nullptr
 * Returns how many pointers were filled. */
size_t kconsole_log(const char** out_lines, size_t max_lines, size_t skip_newest,
                    const char* prefix);
