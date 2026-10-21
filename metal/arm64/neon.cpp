/* NEON compute kernels (metal/arm64/neon.cpp).
 *
 * Actual Advanced SIMD: fmla by vector element across four columns at a time.
 * The scalar version below is not decoration — it is the oracle the NEON result
 * is checked against, and the two are timed off CNTPCT so "faster" is a
 * measurement rather than a claim.
 */
#include "kio.hpp"
#include "timer.hpp"

#include <arm_neon.h>

namespace {

constexpr unsigned N = 64;

/* Column-major 4x4: C = A * B, four columns of C computed with one fmla chain
 * each. This is the standard AArch64 sgemm micro-kernel. */
void matmul4x4_neon(const float* a, const float* b, float* c) {
  const float32x4_t a0 = vld1q_f32(a + 0);
  const float32x4_t a1 = vld1q_f32(a + 4);
  const float32x4_t a2 = vld1q_f32(a + 8);
  const float32x4_t a3 = vld1q_f32(a + 12);

  for (unsigned j = 0; j < 4; ++j) {
    const float32x4_t bj = vld1q_f32(b + j * 4);
    float32x4_t acc = vmulq_laneq_f32(a0, bj, 0);
    acc = vfmaq_laneq_f32(acc, a1, bj, 1);
    acc = vfmaq_laneq_f32(acc, a2, bj, 2);
    acc = vfmaq_laneq_f32(acc, a3, bj, 3);
    vst1q_f32(c + j * 4, acc);
  }
}

void matmul4x4_scalar(const float* a, const float* b, float* c) {
  for (unsigned j = 0; j < 4; ++j)
    for (unsigned i = 0; i < 4; ++i) {
      float s = 0.0f;
      for (unsigned k = 0; k < 4; ++k) s += a[k * 4 + i] * b[j * 4 + k];
      c[j * 4 + i] = s;
    }
}

void matmul_n_neon(const float* a, const float* b, float* c) {
  for (unsigned j = 0; j < N; ++j) {
    for (unsigned i = 0; i < N; i += 4) {
      float32x4_t acc = vdupq_n_f32(0.0f);
      for (unsigned k = 0; k < N; ++k)
        acc = vfmaq_n_f32(acc, vld1q_f32(a + k * N + i), b[j * N + k]);
      vst1q_f32(c + j * N + i, acc);
    }
  }
}

void matmul_n_scalar(const float* a, const float* b, float* c) {
  for (unsigned j = 0; j < N; ++j)
    for (unsigned i = 0; i < N; ++i) {
      float s = 0.0f;
      for (unsigned k = 0; k < N; ++k) s += a[k * N + i] * b[j * N + k];
      c[j * N + i] = s;
    }
}

float g_a[N * N], g_b[N * N], g_c1[N * N], g_c2[N * N];

bool close(float x, float y) {
  const float d = x > y ? x - y : y - x;
  const float m = (x < 0 ? -x : x) + 1.0f;
  return d <= m * 1e-4f;
}

void print_fixed3(float v) {
  const bool neg = v < 0;
  if (neg) v = -v;
  const uint64_t scaled = static_cast<uint64_t>(v * 1000.0f + 0.5f);
  if (neg) kputc('-');
  kputu(scaled / 1000, 10, 0, ' ');
  kputc('.');
  kputu(scaled % 1000, 10, 3, '0');
}

}  /* namespace */

bool neon_selftest() {
  /* Enough to tell a transpose bug from a working kernel; every entry distinct
   * and no symmetry. */
  const float a[16] = {1, 2, 3, 4,  5, 6, 7, 8,  9, 10, 11, 12,  13, 14, 15, 16};
  const float b[16] = {17, 18, 19, 20,  21, 22, 23, 24,  25, 26, 27, 28,  29, 30, 31, 32};
  float c_neon[16], c_scalar[16];

  matmul4x4_neon(a, b, c_neon);
  matmul4x4_scalar(a, b, c_scalar);

  bool ok4 = true;
  for (unsigned i = 0; i < 16; ++i) if (!close(c_neon[i], c_scalar[i])) ok4 = false;

  kputs("  4x4 fmla-by-element kernel vs scalar oracle:\n    neon   ");
  for (unsigned i = 0; i < 16; ++i) { print_fixed3(c_neon[i]); kputc(' '); }
  kputs("\n    scalar ");
  for (unsigned i = 0; i < 16; ++i) { print_fixed3(c_scalar[i]); kputc(' '); }
  kprintf("\n    agree: %s\n", ok4 ? "yes" : "NO");

  /* A size where the vector unit has something to do. */
  for (unsigned i = 0; i < N * N; ++i) {
    g_a[i] = static_cast<float>((i * 37 % 19)) - 9.0f;
    g_b[i] = static_cast<float>((i * 53 % 23)) - 11.0f;
  }

  const uint64_t f = metal_cntfrq();
  uint64_t t0 = metal_cntpct();
  matmul_n_neon(g_a, g_b, g_c1);
  const uint64_t t_neon = metal_cntpct() - t0;

  t0 = metal_cntpct();
  matmul_n_scalar(g_a, g_b, g_c2);
  const uint64_t t_scalar = metal_cntpct() - t0;

  unsigned bad = 0;
  for (unsigned i = 0; i < N * N; ++i) if (!close(g_c1[i], g_c2[i])) ++bad;

  const uint64_t macs = static_cast<uint64_t>(N) * N * N;
  kprintf("  %ux%u matmul, %u multiply-accumulates\n",
          static_cast<uint64_t>(N), static_cast<uint64_t>(N), macs);
  kprintf("    neon   %u counter ticks (%u us)\n", t_neon, t_neon * 1000000ull / f);
  kprintf("    scalar %u counter ticks (%u us)\n", t_scalar, t_scalar * 1000000ull / f);
  if (t_neon) kprintf("    speedup x100: %u\n", t_scalar * 100ull / t_neon);
  kprintf("    elements disagreeing with the scalar oracle: %u of %u\n",
          static_cast<uint64_t>(bad), static_cast<uint64_t>(N * N));

  /* The trap path saves and restores q0-q31; prove a register survives an
   * interrupt rather than assuming the save list is right. */
  register float32x4_t canary asm("v20") = vdupq_n_f32(1234.5f);
  asm volatile("" : "+w"(canary));
  const uint64_t before = metal_cntpct();
  while (metal_cntpct() - before < f / 50) asm volatile("" ::: "memory");
  asm volatile("" : "+w"(canary));
  float out[4];
  vst1q_f32(out, canary);
  bool kept = true;
  for (unsigned i = 0; i < 4; ++i) if (!close(out[i], 1234.5f)) kept = false;
  kprintf("  v20 held across ~20 ms of timer interrupts: %s (%s",
          kept ? "yes" : "NO", "");
  print_fixed3(out[0]);
  kputs(")\n");
  return ok4 && bad == 0 && kept;
}
