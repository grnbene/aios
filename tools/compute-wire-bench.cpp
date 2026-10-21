// A/B: the same tsp_exact question over a running compute service's SHM
// ring, once as the JSON document (msg_type 1: parsed into an RpcValue and
// rendered back as text) and once as the fixed-width binary form (msg_type 2:
// read in place). Proves the two forms give the same answer, then times warm
// round-trips of each. Reports time, not pass counts.
//
//   aios compute serve port=17312 shm_ring=aios_wire_bench &
//   aios-compute-wire-bench aios_wire_bench [iters]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "aios/channel/compute_wire.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/sidecar/shm_abi.hpp"

using namespace aios::channel;
using namespace aios::sidecar;
using Clock = std::chrono::steady_clock;

namespace {

// A real-shaped question: a Buffalo depot and nine stops around it.
struct P { uint64_t id; double lat, lng; };
const std::vector<P> kPoints = {
  {1, 42.8864, -78.8784}, {130307256, 42.9140, -78.8300}, {130307257, 42.9500, -78.7000},
  {130307258, 42.8600, -78.7500}, {130307259, 42.9900, -78.8100}, {130307260, 42.8100, -78.9000},
  {130307261, 42.9300, -78.9500}, {130307262, 42.8700, -78.6500}, {130307263, 43.0200, -78.9000},
  {130307264, 42.7900, -78.8000},
};

std::string json_request() {
  std::string s = "{\"op\":\"tsp_exact\",\"start\":{\"id\":\"" + std::to_string(kPoints[0].id) + "\",\"lat\":" +
                  std::to_string(kPoints[0].lat) + ",\"lng\":" + std::to_string(kPoints[0].lng) + "},\"stops\":[";
  for (size_t i = 1; i < kPoints.size(); ++i) {
    if (i > 1) s += ",";
    s += "{\"id\":\"" + std::to_string(kPoints[i].id) + "\",\"lat\":" + std::to_string(kPoints[i].lat) +
         ",\"lng\":" + std::to_string(kPoints[i].lng) + "}";
  }
  return s + "],\"prune\":true}";
}

std::string binary_request() {
  using namespace wire;
  std::string s(tsp_request_bytes((uint32_t)kPoints.size()), '\0');
  TspRequestHeader h{};
  h.magic = kTspRequestMagic; h.version = kVersion; h.op = TspExact; h.prune = 1; h.n = (uint32_t)kPoints.size();
  std::memcpy(&s[0], &h, sizeof h);
  uint8_t* p = reinterpret_cast<uint8_t*>(&s[sizeof h]);
  for (const P& pt : kPoints) {
    TspPoint w{static_cast<int64_t>(pt.id), pt.lat, pt.lng};
    std::memcpy(p, &w, sizeof w);
    p += sizeof w;
  }
  return s;
}

// One round-trip on the ring; returns the response bytes.
std::string round_trip(ShmRingChannel& req, ShmRingChannel& resp, uint16_t type, uint32_t stream, const std::string& body) {
  if (!req.push_frame(type, stream, body.data(), body.size())) return {};
  std::string out;
  for (int spin = 0; spin < 20000000; ++spin) {
    bool got = resp.pop_frame([&](const ShmFrameHeader& h, const void* ptr, size_t len) {
      if (h.stream_id == stream) out.assign(reinterpret_cast<const char*>(ptr), len);
    });
    if (got && !out.empty()) return out;
    if (!got) std::this_thread::yield();
  }
  return {};
}

struct Answer { std::vector<uint64_t> order; double miles = 0; bool ok = false; };

Answer decode_json(const std::string& s) {
  Answer a;
  auto r = rpc_parse(s);
  if (!r.ok() || !r->has("order")) return a;
  for (const RpcValue& v : (*r)["order"].items()) a.order.push_back(v.as_u64());
  a.miles = (*r)["distance_miles"].as_double();
  a.ok = (*r)["status"].as_text() == "done";
  return a;
}

Answer decode_binary(const std::string& s) {
  using namespace wire;
  Answer a;
  if (s.size() < sizeof(TspResponseHeader)) return a;
  TspResponseHeader h;
  std::memcpy(&h, s.data(), sizeof h);
  if (h.magic != kTspResponseMagic || h.status != Done || s.size() < tsp_response_bytes(h.n)) return a;
  a.order.resize(h.n);
  std::memcpy(a.order.data(), s.data() + sizeof h, h.n * sizeof(uint64_t));
  a.miles = h.distance_miles;
  a.ok = true;
  return a;
}

struct Stat { double mean_us, p50_us, p99_us; };
Stat stat(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  double sum = 0; for (double x : v) sum += x;
  return {sum / v.size(), v[v.size() / 2], v[(size_t)(v.size() * 0.99)]};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <ring-name> [iters]\n", argv[0]); return 1; }
  const std::string ring = argv[1];
  const int iters = argc > 2 ? std::atoi(argv[2]) : 2000;
  ShmRingChannel req(ring, 4 * 1024 * 1024, false);
  ShmRingChannel resp(ring + "_resp", 4 * 1024 * 1024, false);

  const std::string jreq = json_request(), breq = binary_request();
  std::printf("question: %zu points  json request %zu B  binary request %zu B\n", kPoints.size(), jreq.size(), breq.size());

  // Cold: the first call solves; both forms must land on the same answer.
  uint32_t stream = (uint32_t)(Clock::now().time_since_epoch().count() & 0xffffff) + 1;
  auto t0 = Clock::now();
  std::string jraw = round_trip(req, resp, wire::kJson, stream++, jreq);
  Answer aj = decode_json(jraw);
  double cold_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  Answer ab = decode_binary(round_trip(req, resp, wire::kTspBinary, stream++, breq));
  if (!aj.ok || !ab.ok) { std::fprintf(stderr, "no answer: json ok=%d binary ok=%d\n%s\n", aj.ok, ab.ok, jraw.c_str()); return 1; }
  const bool same = aj.order == ab.order && std::fabs(aj.miles - ab.miles) < 1e-5;
  std::printf("cold solve %.1f ms; parity json==binary: %s (%.4f mi, %zu ids)\n", cold_ms, same ? "yes" : "NO", aj.miles, aj.order.size());
  if (!same) return 2;

  // Warm: the answer is cached, so this is the cost of the request form itself.
  std::vector<double> tj, tb;
  size_t jresp = 0, bresp = 0;
  // Interleaved one for one, so the server's idle poll (it sleeps 1 ms when
  // the ring is empty) lands on both forms alike.
  for (int i = 0; i < iters; ++i) {
    auto a = Clock::now();
    std::string r = round_trip(req, resp, wire::kJson, stream++, jreq);
    tj.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    jresp = r.size();
    a = Clock::now();
    r = round_trip(req, resp, wire::kTspBinary, stream++, breq);
    tb.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    bresp = r.size();
  }
  Stat sj = stat(tj), sb = stat(tb);
  std::printf("warm round-trips x%d over the SHM ring (cached answer: transport + encoding only)\n", iters);
  std::printf("  json    mean %8.1f us  p50 %8.1f us  p99 %8.1f us  (resp %zu B)\n", sj.mean_us, sj.p50_us, sj.p99_us, jresp);
  std::printf("  binary  mean %8.1f us  p50 %8.1f us  p99 %8.1f us  (resp %zu B)\n", sb.mean_us, sb.p50_us, sb.p99_us, bresp);
  std::printf("  binary/json p50: %.2fx\n", sb.p50_us / sj.p50_us);

  // Encode+decode alone, no ring, no service: the serialization tax in isolation.
  const int n = 20000;
  auto e0 = Clock::now();
  for (int i = 0; i < n; ++i) { auto r = rpc_parse(jreq); auto a = decode_json(jraw); volatile size_t sink = r->size() + a.order.size(); (void)sink; }
  double json_us = std::chrono::duration<double, std::micro>(Clock::now() - e0).count() / n;
  std::string braw = round_trip(req, resp, wire::kTspBinary, stream++, breq);
  e0 = Clock::now();
  for (int i = 0; i < n; ++i) { std::string b = binary_request(); Answer a = decode_binary(braw); volatile size_t sink = b.size() + a.order.size(); (void)sink; }
  double bin_us = std::chrono::duration<double, std::micro>(Clock::now() - e0).count() / n;
  std::printf("client encode+decode alone: json %.2f us/req, binary %.2f us/req (%.0fx)\n", json_us, bin_us, json_us / bin_us);
  return 0;
}
