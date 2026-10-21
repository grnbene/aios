// The inner loops of the language model, included by llm.cpp once per
// instruction set it builds them for; see the dispatch there.
// Expects v8f, v8h, v8u and bf16_to_f32 in scope.
// __builtin_memcpy, not std::memcpy: a fortified memcpy cannot be inlined into
// a function built for another target, and an out-of-line call per load is
// slower than the arithmetic it feeds.
static inline __attribute__((always_inline)) v8f load8(const float* p) { v8f v; __builtin_memcpy(&v, p, 32); return v; }
static inline __attribute__((always_inline)) v8f load8_bf16(const uint16_t* p) {
  v8h h;
  __builtin_memcpy(&h, p, 16);
  const v8u u = __builtin_convertvector(h, v8u) << 16;
  v8f f;
  __builtin_memcpy(&f, &u, 32);
  return f;
}
static inline __attribute__((always_inline)) float hsum(v8f v) { return ((v[0] + v[4]) + (v[1] + v[5])) + ((v[2] + v[6]) + (v[3] + v[7])); }

static float dot_bf16(const uint16_t* w, const float* x, uint32_t n) {
  v8f a = {}, b = {};
  uint32_t i = 0;
  for (; i + 16 <= n; i += 16) { a += load8_bf16(w + i) * load8(x + i); b += load8_bf16(w + i + 8) * load8(x + i + 8); }
  for (; i + 8 <= n; i += 8) a += load8_bf16(w + i) * load8(x + i);
  float s = hsum(a + b);
  for (; i < n; ++i) s += bf16_to_f32(w[i]) * x[i];
  return s;
}

static float dot_f32(const float* w, const float* x, uint32_t n) {
  v8f a = {}, b = {};
  uint32_t i = 0;
  for (; i + 16 <= n; i += 16) { a += load8(w + i) * load8(x + i); b += load8(w + i + 8) * load8(x + i + 8); }
  for (; i + 8 <= n; i += 8) a += load8(w + i) * load8(x + i);
  float s = hsum(a + b);
  for (; i < n; ++i) s += w[i] * x[i];
  return s;
}

// Three rows against four tokens: seven loads feed twelve multiply-adds, and
// the twelve sums plus the row and token values fit the sixteen AVX registers.
static void dot3x4_f32(const float* r0, const float* r1, const float* r2, const float* x0, const float* x1,
                const float* x2, const float* x3, uint32_t n, float* out) {
  v8f a00 = {}, a01 = {}, a02 = {}, a03 = {}, a10 = {}, a11 = {}, a12 = {}, a13 = {}, a20 = {}, a21 = {}, a22 = {}, a23 = {};
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const v8f y0 = load8(x0 + i), y1 = load8(x1 + i), y2 = load8(x2 + i), y3 = load8(x3 + i);
    v8f w = load8(r0 + i);
    a00 += w * y0; a01 += w * y1; a02 += w * y2; a03 += w * y3;
    w = load8(r1 + i);
    a10 += w * y0; a11 += w * y1; a12 += w * y2; a13 += w * y3;
    w = load8(r2 + i);
    a20 += w * y0; a21 += w * y1; a22 += w * y2; a23 += w * y3;
  }
  float s[12] = {hsum(a00), hsum(a01), hsum(a02), hsum(a03), hsum(a10), hsum(a11), hsum(a12), hsum(a13),
                 hsum(a20), hsum(a21), hsum(a22), hsum(a23)};
  const float* rs[3] = {r0, r1, r2};
  const float* xs[4] = {x0, x1, x2, x3};
  for (; i < n; ++i)
    for (int r = 0; r < 3; ++r)
      for (int t = 0; t < 4; ++t) s[r * 4 + t] += rs[r][i] * xs[t][i];
  for (int k = 0; k < 12; ++k) out[k] = s[k];
}

// out += w * v over n lanes.
static void axpy_f32(float w, const float* v, float* out, uint32_t n) {
  uint32_t i = 0;
  const v8f wv = {w, w, w, w, w, w, w, w};
  for (; i + 8 <= n; i += 8) { v8f o = load8(out + i); o += wv * load8(v + i); __builtin_memcpy(out + i, &o, 32); }
  for (; i < n; ++i) out[i] += w * v[i];
}

static void widen_bf16(const uint16_t* w, float* out, uint32_t n) {
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) { const v8f v = load8_bf16(w + i); __builtin_memcpy(out + i, &v, 32); }
  for (; i < n; ++i) out[i] = bf16_to_f32(w[i]);
}

