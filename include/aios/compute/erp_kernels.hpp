#pragma once
// Compute kernels for the ERP bridge: the exhaustive open-path route search the
// dispatch board wants but could never finish in PHP, registered as Custom
// operations on a Device so the scheduler places and charges them like any
// other work.
#include <cstdint>
#include <vector>
#include "aios/compute/device.hpp"
#include "aios/core/result.hpp"

namespace aios::erp {

// Great-circle miles. The same formula and the same 3958.8-mile radius the
// ERP's DispatchRoute::distance() uses, so a leg costs exactly what PHP says
// it costs and the two agree on which route is shortest.
double haversine_miles(double lat1, double lon1, double lat2, double lon2);

struct TspExactResult {
  std::vector<int32_t> order;   // indices into the input; order[0] == 0, the start
  double distance = 0;          // miles along the path; no return leg
  uint64_t evaluated = 0;       // complete paths scored
  uint64_t pruned = 0;          // partial paths abandoned by the bound
};

// Every ordering of points 1..n-1 after the fixed start 0, scored as the sum of
// consecutive haversine legs, minimum kept. This is the search the dispatch
// board's DispatchRoute::compute() performs — an open path, drop and run, no
// return to the depot — done without materialising a single permutation.
//
// `prune` abandons a partial path the moment it already costs more than the
// best complete one. The optimum is provably the same; what changes is how
// many paths have to be scored to be sure of it. Ties between equal-length
// routes resolve to the first found in index order.
Result<TspExactResult> tsp_exact(const double* latlng, int n, uint32_t lanes, bool prune);

// Beyond this the space is too large to exhaust in a request; the caller
// should be in a heuristic regime.
constexpr int kTspExactMaxPoints = 17;

// The kernel's name as a graph refers to it: fnv1a(name) is Operation::custom_kernel.
constexpr const char* kTspExactKernel = "erp.tsp_exact";

struct PoolGreedyResult {
  std::vector<int32_t> order;   // stop indices (1..n-1 of the input) in visiting order
  std::vector<double> legs;     // miles from the previous point to each
  double total = 0;
};

// The OMS board's own ordering of the open-order pool — getshortest(): from the
// depot, repeatedly the nearest unvisited order, first on a tie — with the same
// haversine and the same tie rule, for pools far beyond what exact search can
// take. Row 0 of the input is the depot. No matrix is stored: a thousand orders
// is a million arcs, computed as they are needed.
Result<PoolGreedyResult> pool_greedy(const double* latlng, int n);
constexpr const char* kPoolGreedyKernel = "erp.pool_greedy";
constexpr int kPoolGreedyMaxPoints = 20000;

struct RouteImproveResult {
  std::vector<int32_t> order;   // indices into the stops, in visiting order
  double distance = 0;          // start → stops → end (when there is an end), in miles
  uint64_t evaluated = 0;       // candidate tours scored
  uint32_t passes = 0;          // improvement passes made
};

enum class RouteEnd : uint8_t { Home = 0, Last = 1, Custom = 2 };   // return to start / one-way / a chosen point

// The sales route builder's optimizer, move for move: nearest-neighbour seed
// from the start, then 2-opt segment reversals and Or-opt relocations of runs
// of one to three stops, first improvement, until a pass gains nothing or
// eighty passes have run. Every candidate is rescored end to end exactly as
// the PHP does — same haversine, same summation order, same 1e-9 acceptance —
// so the answer is the one the report would have produced, sooner.
Result<RouteImproveResult> route_improve(const double* stops_latlng, int n, const double* start_latlng,
                                          RouteEnd end_mode, const double* end_latlng);

constexpr const char* kRouteImproveKernel = "erp.route_improve";
constexpr int kRouteImproveMaxPoints = 120;   // the report's own ceiling; above it the seed stands

// Register every ERP kernel on a device.
//
// erp.tsp_exact:
//   in[0]   F64 [n,2]   lat,lng per point; row 0 is the start
//   out[0]  I32 [n]     visiting order as indices; out[0][0] == 0
//   out[1]  F64 [4]     distance_miles, evaluated, pruned, 0
//   scalar  != 0        prune
//
// erp.pool_greedy:
//   in[0]   F64 [n,2]   lat,lng per point; row 0 is the depot
//   out[0]  I32 [n-1]   visiting order as indices into in[0]
//   out[1]  F64 [n-1]   miles from the previous point to each
//   out[2]  F64 [4]     total_miles, n, 0, 0
//
// erp.route_improve:
//   in[0]   F64 [n,2]   lat,lng per stop
//   in[1]   F64 [2,2]   row 0 start, row 1 end (read only for RouteEnd::Custom)
//   out[0]  I32 [n]     visiting order as indices into in[0]
//   out[1]  F64 [4]     distance_miles, evaluated, passes, 0
//   scalar              RouteEnd as a number: 0 home, 1 last, 2 custom
Result<void> register_erp_kernels(Device& dev);

}  // namespace aios::erp
