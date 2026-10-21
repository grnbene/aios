#pragma once
// A compute service for programs that are not aios: one TCP port, one JSON
// document per request, one answer. Each request becomes a graph the
// scheduler places and executes; the caller sees only the result. Built for
// the ERP's PHP, which asks for a route and wants the answer inside its own
// request budget or a promise that it is coming.
#include <atomic>
#include <functional>
#include <string>
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland { class SqlDatabase; }

namespace aios::channel {

struct ComputeServeOptions {
  uint16_t port = 7311;
  bool loopback_only = true;   // the machine's own processes, nothing beyond it
  uint32_t lanes = 0;          // 0: every lane the device has
  int max_exact_points = 15;   // start + stops accepted by erp.tsp_exact
  std::string token;           // empty: no token required
  size_t cache_entries = 1024; // finished answers kept for repeat requests
  int frame_timeout_ms = 2000; // to read a whole request from a connected peer
  std::string shm_ring_name;   // optional: name of SHM ring for zero-copy IPC dispatch (e.g. "aios_compute_ring")
};

// Frames are a 4-byte big-endian length followed by that many bytes of JSON,
// both ways. A request is an object with "op" and, per op, its arguments:
//
//   {"op":"ping"}
//   {"op":"tsp_exact","start":{"id":"-1","lat":42.9,"lng":-78.8},
//    "stops":[{"id":"130307256","lat":42.9,"lng":-78.7},...],
//    "deadline_ms":1500,"prune":true,"token":"..."}
//   {"op":"sql","query":"SELECT ...","token":"..."}
//
// Answers carry "ok"; a solve carries "status":"done" with "order" (the ids,
// start first), "distance_miles", "evaluated", "pruned", "elapsed_ms",
// "device", "lanes", "cached" — or "status":"pending" when the deadline passed
// first, in which case the work continues and the same request answers from
// cache once it is done. "sql" answers synchronously with "columns" and
// "rows" (or an error if the statement is not a read, or no database was
// attached to this service).
//
// Serves until `stop` is set; with no stop flag, forever. `sql_db`, when
// given, is a database this service reads from for the "sql" op — reads
// only: a statement that is not SELECT/SHOW/DESCRIBE/EXPLAIN never reaches
// it. The caller owns the database's lifetime and must keep it alive for as
// long as compute_serve runs.
Result<void> compute_serve(Kernel& k, Surface& worker, const ComputeServeOptions& o,
                           std::function<void(const std::string&)> log,
                           std::atomic<bool>* stop = nullptr,
                           aios::userland::SqlDatabase* sql_db = nullptr);

}  // namespace aios::channel
