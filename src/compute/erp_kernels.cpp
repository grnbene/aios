// The exhaustive route search, as a device kernel. Lanes split the space by
// the first stop after the depot; each lane walks its share depth-first with
// the running cost carried down, so no permutation is ever stored — the thing
// that made the PHP original run out of memory before it ran out of time.
#include "aios/compute/erp_kernels.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace aios::erp {

// DispatchRoute::distance(), operation for operation: PHP's deg2rad is
// (deg / 180.0) * M_PI, the arc is doubled first, then scaled by the radius —
// kept in that order so the double is the one PHP computes.
double haversine_miles(double lat1, double lon1, double lat2, double lon2) {
  if (lat1 == lat2 && lon1 == lon2) return 0.0;
  const double pi = 3.14159265358979323846;
  const double dlat = ((lat2 - lat1) / 180.0) * pi;
  const double dlon = ((lon2 - lon1) / 180.0) * pi;
  const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos((lat1 / 180.0) * pi) * std::cos((lat2 / 180.0) * pi) * std::sin(dlon / 2) * std::sin(dlon / 2);
  const double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
  return 3958.8 * c;
}

Result<PoolGreedyResult> pool_greedy(const double* latlng, int n) {
  if (n < 1) return Result<PoolGreedyResult>(Status::Invalid, "pool_greedy: no points");
  if (n > kPoolGreedyMaxPoints) return Result<PoolGreedyResult>(Status::Invalid, "pool_greedy: too many points");
  PoolGreedyResult out;
  const int stops = n - 1;
  if (stops == 0) return out;
  std::vector<uint8_t> visited((size_t)n, 0);
  out.order.reserve((size_t)stops);
  out.legs.reserve((size_t)stops);
  int at = 0;
  for (int step = 0; step < stops; ++step) {
    // getshortest()'s minkey(): the first key holding the minimum — unless the
    // minimum is zero, when its `$min_val = $min` slip (an undefined $min)
    // lets every later zero win, so the LAST zero-distance key is taken.
    int pick = -1; double best = 0;
    for (int j = 1; j < n; ++j) {
      if (visited[(size_t)j]) continue;
      const double d = haversine_miles(latlng[2 * at], latlng[2 * at + 1], latlng[2 * j], latlng[2 * j + 1]);
      if (pick < 0 || d < best || (d == 0.0 && best == 0.0)) { pick = j; best = d; }
    }
    visited[(size_t)pick] = 1;
    out.order.push_back(pick);
    out.legs.push_back(best);
    out.total += best;
    at = pick;
  }
  return out;
}

namespace {

// A double shared between lanes as its bit pattern, lowered with a CAS so the
// bound only ever tightens.
struct SharedBound {
  std::atomic<uint64_t> bits;
  explicit SharedBound(double v) : bits(0) { uint64_t b; std::memcpy(&b, &v, sizeof b); bits.store(b); }
  double load() const { uint64_t b = bits.load(std::memory_order_relaxed); double v; std::memcpy(&v, &b, sizeof v); return v; }
  void lower(double v) {
    uint64_t nb; std::memcpy(&nb, &v, sizeof nb);
    uint64_t cur = bits.load(std::memory_order_relaxed);
    for (;;) {
      double cv; std::memcpy(&cv, &cur, sizeof cv);
      if (v >= cv) return;
      if (bits.compare_exchange_weak(cur, nb, std::memory_order_relaxed)) return;
    }
  }
};

struct Lane {
  const double* D;
  int n;
  bool prune;
  SharedBound* bound;
  double best = 1e300;
  int32_t best_path[kTspExactMaxPoints];
  int32_t path[kTspExactMaxPoints];
  uint64_t evaluated = 0, pruned = 0;

  void walk(int depth, uint32_t visited, double cost) {
    if (depth == n) {
      ++evaluated;
      if (cost < best) {
        best = cost;
        std::memcpy(best_path, path, sizeof(int32_t) * (size_t)n);
        if (prune) bound->lower(cost);
      }
      return;
    }
    if (prune && cost >= bound->load()) { ++pruned; return; }
    const int prev = path[depth - 1];
    for (int j = 1; j < n; ++j) {
      if (visited & (1u << j)) continue;
      path[depth] = j;
      walk(depth + 1, visited | (1u << j), cost + D[prev * n + j]);
    }
  }
};

// Nearest-neighbour path from the start: not an answer, an upper bound the
// search can cut against from its first step instead of its first leaf.
double greedy_bound(const double* D, int n) {
  uint32_t visited = 1u;
  int at = 0;
  double cost = 0;
  for (int step = 1; step < n; ++step) {
    int pick = -1;
    for (int j = 1; j < n; ++j) if (!(visited & (1u << j)) && (pick < 0 || D[at * n + j] < D[at * n + pick])) pick = j;
    cost += D[at * n + pick];
    visited |= (1u << pick);
    at = pick;
  }
  return cost;
}

}  // namespace

Result<TspExactResult> tsp_exact(const double* latlng, int n, uint32_t lanes, bool prune) {
  if (n < 1) return Result<TspExactResult>(Status::Invalid, "tsp_exact: no points");
  if (n > kTspExactMaxPoints) return Result<TspExactResult>(Status::Invalid, "tsp_exact: too many points to exhaust");
  TspExactResult out;
  if (n == 1) { out.order = {0}; out.evaluated = 1; return out; }

  std::vector<double> D((size_t)n * n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      D[(size_t)i * n + j] = haversine_miles(latlng[2 * i], latlng[2 * i + 1], latlng[2 * j], latlng[2 * j + 1]);

  SharedBound bound(prune ? greedy_bound(D.data(), n) + 1e-9 : 1e300);
  const uint32_t first_stops = (uint32_t)(n - 1);
  lanes = std::max<uint32_t>(1, std::min(lanes, first_stops));

  std::vector<Lane> work(lanes);
  for (auto& L : work) { L.D = D.data(); L.n = n; L.prune = prune; L.bound = &bound; L.path[0] = 0; }

  auto run_lane = [&](uint32_t li) {
    Lane& L = work[li];
    for (int f = 1; f < n; ++f) {
      if ((uint32_t)(f - 1) % lanes != li) continue;
      L.path[1] = f;
      L.walk(2, 1u | (1u << f), L.D[f]);
    }
  };

  if (lanes == 1) {
    run_lane(0);
  } else {
    std::vector<hal::Thread> ts;
    for (uint32_t li = 0; li < lanes; ++li) ts.emplace_back([&, li] { run_lane(li); });
    for (auto& t : ts) t.join();
  }

  // Lowest cost wins; on an exact tie the lane holding the earlier first stop,
  // which is the earlier permutation in index order.
  uint32_t bi = 0;
  for (uint32_t li = 1; li < lanes; ++li) if (work[li].best < work[bi].best) bi = li;
  for (auto& L : work) { out.evaluated += L.evaluated; out.pruned += L.pruned; }
  if (work[bi].best >= 1e299) return Result<TspExactResult>(Status::Invalid, "tsp_exact: no path found");
  out.distance = work[bi].best;
  out.order.assign(work[bi].best_path, work[bi].best_path + n);
  return out;
}

namespace {

// PHP's deg2rad is (deg / 180.0) * M_PI, and its haversine multiplies
// 3958.8 * 2 before the arc — kept in that order so every intermediate
// double rounds the way the report's does, and a 1e-9 acceptance test lands
// on the same side.
inline double php_deg2rad(double d) { return (d / 180.0) * 3.14159265358979323846; }
double sales_haversine(double lat1, double lon1, double lat2, double lon2) {
  const double dLat = php_deg2rad(lat2 - lat1);
  const double dLon = php_deg2rad(lon2 - lon1);
  const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                   std::cos(php_deg2rad(lat1)) * std::cos(php_deg2rad(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
  return 3958.8 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

struct Improver {
  const double* P;          // stops, lat/lng pairs
  int n;
  double s_lat, s_lng;
  bool has_end;
  double e_lat, e_lng;
  uint64_t evaluated = 0;
  // Every leg the search can ask for, computed once. A haversine between the
  // same two points is the same double every time, so summing lookups in the
  // report's order gives the report's total; what changes is that a 120-stop
  // day no longer recomputes thirty million arcs.
  std::vector<double> D;        // D[a*n+b]: stop a → stop b
  std::vector<double> from_s;   // start → stop
  std::vector<double> to_e;     // stop → end

  void prepare() {
    D.assign((size_t)n * n, 0.0);
    from_s.assign((size_t)n, 0.0);
    to_e.assign((size_t)n, 0.0);
    for (int a = 0; a < n; ++a) {
      from_s[(size_t)a] = sales_haversine(s_lat, s_lng, P[2 * a], P[2 * a + 1]);
      if (has_end) to_e[(size_t)a] = sales_haversine(P[2 * a], P[2 * a + 1], e_lat, e_lng);
      for (int b = 0; b < n; ++b) D[(size_t)a * n + b] = sales_haversine(P[2 * a], P[2 * a + 1], P[2 * b], P[2 * b + 1]);
    }
  }

  // The length of a visiting order given as a function from position to stop.
  // Legs are summed start → first → … → last → end, in that order, exactly as
  // the report sums them, so the double that comes out is the report's double.
  template <typename At>
  double tour_of(int m, At at) {
    ++evaluated;
    if (m == 0) return 0.0;
    int32_t prev = at(0);
    double d = from_s[(size_t)prev];
    for (int k = 1; k < m; ++k) { const int32_t cur = at(k); d += D[(size_t)prev * n + cur]; prev = cur; }
    if (has_end) d += to_e[(size_t)prev];
    return d;
  }
  double tour(const std::vector<int32_t>& seq) { return tour_of((int)seq.size(), [&](int k) { return seq[(size_t)k]; }); }

  // salesRouteImprove, move for move. A candidate is never built: reversing
  // seq[i..j], or lifting seq[i..i+L) and dropping it at k, is a mapping from
  // position to stop, and the tour is summed through that mapping. Only an
  // accepted candidate is written out.
  uint32_t improve(std::vector<int32_t>& seq) {
    const int len = (int)seq.size();
    if (len < 3 || len > kRouteImproveMaxPoints) return 0;
    double best = tour(seq);
    int guard = 0;
    bool improved = true;
    uint32_t passes = 0;
    while (improved && guard++ < 80) {
      ++passes;
      improved = false;
      for (int i = 0; i < len - 1; ++i) {
        for (int j = i + 1; j < len; ++j) {
          const double l = tour_of(len, [&](int k) { return (k >= i && k <= j) ? seq[(size_t)(j - (k - i))] : seq[(size_t)k]; });
          if (l + 1e-9 < best) { std::reverse(seq.begin() + i, seq.begin() + j + 1); best = l; improved = true; }
        }
      }
      bool restart = false;
      for (int L = 1; L <= 3 && !restart; ++L) {
        for (int i = 0; i + L <= len && !restart; ++i) {
          const int m = len - L;                      // the rest, without the run
          auto rest_at = [&](int q) { return q < i ? seq[(size_t)q] : seq[(size_t)(q + L)]; };
          for (int k = 0; k <= m; ++k) {
            const double l = tour_of(len, [&](int p) { return p < k ? rest_at(p) : (p < k + L ? seq[(size_t)(i + (p - k))] : rest_at(p - L)); });
            if (l + 1e-9 < best) {
              std::vector<int32_t> next((size_t)len);
              for (int p = 0; p < len; ++p) next[(size_t)p] = p < k ? rest_at(p) : (p < k + L ? seq[(size_t)(i + (p - k))] : rest_at(p - L));
              seq = std::move(next);
              best = l; improved = true; restart = true;
              break;
            }
          }
        }
      }
    }
    return passes;
  }
};

}  // namespace

Result<RouteImproveResult> route_improve(const double* stops_latlng, int n, const double* start_latlng,
                                          RouteEnd end_mode, const double* end_latlng) {
  if (n < 0) return Result<RouteImproveResult>(Status::Invalid, "route_improve: bad count");
  RouteImproveResult out;
  if (n == 0) return out;
  Improver im;
  im.P = stops_latlng; im.n = n;
  im.s_lat = start_latlng[0]; im.s_lng = start_latlng[1];
  if (end_mode == RouteEnd::Last) { im.has_end = false; im.e_lat = im.e_lng = 0; }
  else if (end_mode == RouteEnd::Custom) { im.has_end = true; im.e_lat = end_latlng[0]; im.e_lng = end_latlng[1]; }
  else { im.has_end = true; im.e_lat = im.s_lat; im.e_lng = im.s_lng; }
  im.prepare();

  // Nearest-neighbour seed from the start; ties go to the earlier stop, as
  // PHP's strict `<` over the remaining list does.
  std::vector<int32_t> unvisited(n);
  for (int i = 0; i < n; ++i) unvisited[i] = i;
  std::vector<int32_t> seq;
  int32_t at = -1;   // -1: still at the start
  while (!unvisited.empty()) {
    size_t ni = 0; double nd = 1e300;
    for (size_t idx = 0; idx < unvisited.size(); ++idx) {
      const int32_t s = unvisited[idx];
      const double d = at < 0 ? im.from_s[(size_t)s] : im.D[(size_t)at * n + s];
      if (d < nd) { nd = d; ni = idx; }
    }
    at = unvisited[ni];
    seq.push_back(at);
    unvisited.erase(unvisited.begin() + (long)ni);
  }

  out.passes = im.improve(seq);
  out.distance = im.tour(seq);
  out.evaluated = im.evaluated;
  out.order = std::move(seq);
  return out;
}

Result<void> register_erp_kernels(Device& dev) {
  auto r3 = dev.register_kernel(kPoolGreedyKernel, [](KernelArgs& a) -> Result<void> {
    if (a.op.inputs.size() < 1 || a.op.outputs.size() < 3) return Result<void>(Status::Invalid, "erp.pool_greedy needs 1 input and 3 outputs");
    const Tensor& tin = a.op.inputs[0];
    const Tensor& tord = a.op.outputs[0];
    const Tensor& tleg = a.op.outputs[1];
    const Tensor& tmeta = a.op.outputs[2];
    if (tin.dtype != DType::F64 || tin.rank() != 2 || tin.shape[1] != 2) return Result<void>(Status::Invalid, "erp.pool_greedy input must be F64 [n,2]");
    const int n = (int)tin.shape[0];
    if (n < 1) return Result<void>(Status::Invalid, "erp.pool_greedy needs a depot");
    if (tord.dtype != DType::I32 || tord.elements() < (uint64_t)(n - 1)) return Result<void>(Status::Invalid, "erp.pool_greedy order output must be I32 [n-1]");
    if (tleg.dtype != DType::F64 || tleg.elements() < (uint64_t)(n - 1)) return Result<void>(Status::Invalid, "erp.pool_greedy legs output must be F64 [n-1]");
    if (tmeta.dtype != DType::F64 || tmeta.elements() < 4) return Result<void>(Status::Invalid, "erp.pool_greedy meta output must be F64 [4]");
    const double* latlng = reinterpret_cast<const double*>(a.inputs[0].base + tin.offset);
    auto r = pool_greedy(latlng, n);
    if (!r.ok()) return r.error();
    int32_t* order = reinterpret_cast<int32_t*>(a.outputs[0].base + tord.offset);
    double* legs = reinterpret_cast<double*>(a.outputs[1].base + tleg.offset);
    double* meta = reinterpret_cast<double*>(a.outputs[2].base + tmeta.offset);
    for (int i = 0; i < n - 1; ++i) { order[i] = r->order[(size_t)i]; legs[i] = r->legs[(size_t)i]; }
    meta[0] = r->total; meta[1] = (double)n; meta[2] = 0; meta[3] = 0;
    return Ok();
  });
  if (!r3.ok()) return r3;
  auto r2 = dev.register_kernel(kRouteImproveKernel, [](KernelArgs& a) -> Result<void> {
    if (a.op.inputs.size() < 2 || a.op.outputs.size() < 2) return Result<void>(Status::Invalid, "erp.route_improve needs 2 inputs and 2 outputs");
    const Tensor& tst = a.op.inputs[0];
    const Tensor& tse = a.op.inputs[1];
    const Tensor& tord = a.op.outputs[0];
    const Tensor& tmeta = a.op.outputs[1];
    if (tst.dtype != DType::F64 || tst.rank() != 2 || tst.shape[1] != 2) return Result<void>(Status::Invalid, "erp.route_improve stops must be F64 [n,2]");
    if (tse.dtype != DType::F64 || tse.elements() < 4) return Result<void>(Status::Invalid, "erp.route_improve start/end must be F64 [2,2]");
    const int n = (int)tst.shape[0];
    if (tord.dtype != DType::I32 || tord.elements() < (uint64_t)n) return Result<void>(Status::Invalid, "erp.route_improve order output must be I32 [n]");
    if (tmeta.dtype != DType::F64 || tmeta.elements() < 4) return Result<void>(Status::Invalid, "erp.route_improve meta output must be F64 [4]");
    const double* stops = reinterpret_cast<const double*>(a.inputs[0].base + tst.offset);
    const double* se = reinterpret_cast<const double*>(a.inputs[1].base + tse.offset);
    const int mode = (int)a.op.scalar;
    const RouteEnd end = mode == 1 ? RouteEnd::Last : (mode == 2 ? RouteEnd::Custom : RouteEnd::Home);
    auto r = route_improve(stops, n, se, end, se + 2);
    if (!r.ok()) return r.error();
    int32_t* order = reinterpret_cast<int32_t*>(a.outputs[0].base + tord.offset);
    double* meta = reinterpret_cast<double*>(a.outputs[1].base + tmeta.offset);
    for (int i = 0; i < n; ++i) order[i] = r->order[(size_t)i];
    meta[0] = r->distance; meta[1] = (double)r->evaluated; meta[2] = (double)r->passes; meta[3] = 0;
    return Ok();
  });
  if (!r2.ok()) return r2;
  return dev.register_kernel(kTspExactKernel, [](KernelArgs& a) -> Result<void> {
    if (a.op.inputs.size() < 1 || a.op.outputs.size() < 2) return Result<void>(Status::Invalid, "erp.tsp_exact needs 1 input and 2 outputs");
    const Tensor& tin = a.op.inputs[0];
    const Tensor& tord = a.op.outputs[0];
    const Tensor& tmeta = a.op.outputs[1];
    if (tin.dtype != DType::F64 || tin.rank() != 2 || tin.shape[1] != 2) return Result<void>(Status::Invalid, "erp.tsp_exact input must be F64 [n,2]");
    const int n = (int)tin.shape[0];
    if (tord.dtype != DType::I32 || tord.elements() < (uint64_t)n) return Result<void>(Status::Invalid, "erp.tsp_exact order output must be I32 [n]");
    if (tmeta.dtype != DType::F64 || tmeta.elements() < 4) return Result<void>(Status::Invalid, "erp.tsp_exact meta output must be F64 [4]");
    const double* latlng = reinterpret_cast<const double*>(a.inputs[0].base + tin.offset);
    auto r = tsp_exact(latlng, n, a.lanes, a.op.scalar != 0.0);
    if (!r.ok()) return r.error();
    int32_t* order = reinterpret_cast<int32_t*>(a.outputs[0].base + tord.offset);
    double* meta = reinterpret_cast<double*>(a.outputs[1].base + tmeta.offset);
    for (int i = 0; i < n; ++i) order[i] = r->order[(size_t)i];
    meta[0] = r->distance; meta[1] = (double)r->evaluated; meta[2] = (double)r->pruned; meta[3] = 0;
    return Ok();
  });
}

}  // namespace aios::erp
