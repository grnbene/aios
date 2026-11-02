// What the lane programs' native code shares across architectures (sql_vjit_x64.inc, sql_vjit_a64.inc): the
// switches, the Env the generated function is handed, and the helpers it calls in the interpreter's own terms.
namespace vp {
namespace vjit {
inline bool enabled() {
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_VJIT"); return !(v && *v == '0'); }();
  return on;
}
inline std::atomic<uint64_t>& native_runs() { static std::atomic<uint64_t> n{0}; return n; }   // chunks computed natively
inline bool hoist_enabled() {   // AIOS_SQL_VJIT_HOIST=0: the loop-invariant instructions computed in the loop, as before
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_VJIT_HOIST"); return !(v && *v == '0'); }();
  return on;
}
inline bool check_mode() {
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_VJIT_CHECK"); return v && *v == '1'; }();
  return on;
}

// What the generated function is handed: the leaves' lanes (by instruction index; a pointer per index, null
// where an index is not a leaf of that kind) and the output lane.
struct Env {
  size_t n;                       // +0
  const void* const* lane_i;      // +8   int64 lanes (I64 leaves), or the text references of a ColS leaf
  const void* const* lane_f;      // +16  double lanes (F64 leaves)
  const void* const* lane_n;      // +24  null bytes
  int64_t* out_i;                 // +32
  double* out_f;                  // +40
  uint8_t* out_n;                 // +48
  const LeafDesc* leaves;         // +56  the leaves the code gathers itself, by instruction (LeafDesc, 72 bytes each)
  const size_t* const* rid;       // +64  the chunk's row ids, by source
  uint64_t hoist;                 // +72  1: the invariant instructions are computed once before the loop (Code::hoist_srcs)
};
static_assert(sizeof(LeafDesc) == 72, "LeafDesc layout");
using Fn = int (*)(const Env*);   // 0: done; 1: bail - the interpreter decides the chunk; 2: a column's layout moved

// The helpers the code calls, in the interpreter's own terms.
extern "C" {
inline double vjit_floor(double a) { return std::floor(a); }
inline double vjit_ceil(double a) { return std::ceil(a); }
inline double vjit_fmod(double a, double b) { return std::fmod(a, b); }
inline double vjit_sin(double a) { return std::sin(a); }
inline double vjit_cos(double a) { return std::cos(a); }
inline double vjit_tan(double a) { return std::tan(a); }
inline double vjit_atan(double a) { return std::atan(a); }
inline double vjit_round(double a) { return std::round(a); }
inline double vjit_u64_to_double(int64_t bits) { return (double)(uint64_t)bits; }
// A double's bits as the bit operators read them (vp::eval's u()).
inline int64_t vjit_bits_of_double(double d) {
  d = std::round(d);
  if (d <= -9223372036854775808.0) return (int64_t)((uint64_t)1 << 63);
  if (d >= 18446744073709551615.0) return (int64_t)~(uint64_t)0;
  return d < 0 ? (int64_t)d : (int64_t)(uint64_t)d;
}
// CAST(double AS SIGNED) as the interpreter rounds it (vp::eval's CastI on a double lane); 1: bail.
inline double vjit_pow(double a, double b) { return std::pow(a, b); }
inline double vjit_atan2(double a, double b) { return std::atan2(a, b); }
// Text references compared as the interpreter compares them (pad_compare_ci of the texts they name): -1, 0, 1.
inline int64_t vjit_text_cmp(const void* prog, int64_t a, int64_t b) {
  const Prog* P = static_cast<const Prog*>(prog);
  const int c = pad_compare_ci(P->text_of(a), P->text_of(b));
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}
// The exact-decimal check of Add / Sub / Mul (vp::eval): 1 where the lane may not hold the interpreter's exact value.
inline int64_t vjit_exact_dyadic_bad(double a, double b, double r, int64_t is_mul) {
  return (std::fabs(r) >= 4503599627370496.0 || (is_mul && std::fabs(a) >= 1073741824.0 && std::fabs(b) >= 1073741824.0) ||
          (is_mul && std::fma(a, b, -r) != 0)) ? 1 : 0;
}
inline int vjit_cast_i(double a, int64_t approx, int64_t* out) {
  if (!(std::fabs(a) < 9.2e18)) return 1;
  if (!approx) { *out = (int64_t)std::llround(a); return 0; }
  const double fl = std::floor(a);
  const double frac = a - fl;
  const double tol = std::fabs(a) * 1e-13 + 1e-13;
  if (frac == 0) *out = (int64_t)a;
  else if (std::fabs(frac - 0.5) > tol) *out = (int64_t)std::round(a);
  else { Val r; *out = round_double_as_written(a, 0, &r) ? (int64_t)std::llround(r.num) : (int64_t)std::nearbyint(a); }
  return 0;
}
}

}  // namespace vjit
}  // namespace vp
