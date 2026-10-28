// Checks the shm-fabric channel transport (50 frames published and consumed), compute_serve
// dispatch over a zero-copy SHM ring (ping and an exact TSP), and grain's sidecar builtins end
// to end.

#undef NDEBUG
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cassert>
#include <chrono>
#include <thread>
#include <atomic>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/channel/state_channel.hpp"
#include "aios/channel/compute_serve.hpp"
#include "aios/sidecar/shm_abi.hpp"
#include "aios/userland/grain.hpp"

using namespace aios;
using namespace aios::channel;
using namespace aios::sidecar;
using namespace aios::userland::grain;

static void test_shm_fabric_transport() {
  std::cout << "─── [1] ChannelManager 'shm-fabric' Core Transport Test ────────\n";
  auto host = hal::make_host();
  IdGenerator ids(0x12345678, 1000);
  MemoryFabric memory(*host, ids);
  memory.register_provider(make_system_ram_provider(*host));
  memory.register_provider(make_shared_memory_provider(*host));

  ChannelManager cm(ids, memory);
  cm.register_transport(make_shm_fabric_transport());

  EntityID publisher = ids.next_as<EntityID>();
  EntityID consumer = ids.next_as<EntityID>();

  auto ch_res = cm.create(publisher, 128, 512, 1000, "shm-fabric");
  assert(ch_res.ok() && "Failed to create shm-fabric channel");
  ChannelID ch_id = *ch_res;

  auto sub_res = cm.subscribe(ch_id, consumer);
  assert(sub_res.ok() && "Failed to subscribe to shm-fabric channel");

  // Publish state frames
  for (int i = 0; i < 50; ++i) {
    StateObject obj;
    obj.id = ids.next_as<StateID>();
    obj.kind = StateKind::Working;
    obj.generation = i + 1;
    std::string msg = "event_frame_" + std::to_string(i);
    obj.payload.assign(msg.begin(), msg.end());
    auto pub_res = cm.publish(ch_id, publisher, obj, 2000 + i);
    assert(pub_res.ok() && "Publish to shm-fabric failed");
  }

  // Consume state frames
  for (int i = 0; i < 50; ++i) {
    auto frame_res = cm.consume(ch_id, consumer);
    assert(frame_res.ok() && "Consume from shm-fabric failed");
    std::string msg(frame_res->payload.begin(), frame_res->payload.end());
    std::string expected = "event_frame_" + std::to_string(i);
    assert(msg == expected && "Frame payload mismatch");
    assert(frame_res->generation == (uint64_t)(i + 1));
  }

  uint64_t head = 0, tail = 0, dropped = 0;
  assert(cm.stats(ch_id, head, tail, dropped).ok());
  assert(head == 50 && tail == 0 && dropped == 0);

  std::cout << "  ✔ shm-fabric transport published & consumed 50 frames successfully (head=" 
            << head << ", dropped=" << dropped << ")\n";
}

static void test_compute_serve_shm_dispatch() {
  std::cout << "─── [2] compute_serve Zero-Copy SHM Ring Dispatch Test ────────\n";
  KernelConfig cfg;
  cfg.root = "/tmp/aios_shm_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto booted = Kernel::boot(cfg);
  assert(booted.ok() && "Kernel boot failed");
  Kernel& k = **booted;
  using K = ResourceKind;
  AuthorityProfile profile = {
    {ResourceRef{K::Kernel, {}},        (uint16_t)R_Schedule, 0},
    {ResourceRef{K::ComputeDevice, {}}, (uint16_t)R_Execute, 0},
    {ResourceRef{K::Memory, {}},        (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 1ull << 30},
    {ResourceRef{K::State, {}},         (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 0},
    {ResourceRef{K::StorageObject, {}}, (uint16_t)(R_Read | R_Write), 0},
  };
  auto svc = k.resident("bench-compute", profile);
  assert(svc.ok() && "resident failed");
  Surface worker = k.as(*svc);

  std::string ring_name = "aios_bench_compute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  ComputeServeOptions opt;
  // A port nothing else holds: a fixed one (it was 17311) failed whenever another engine on the machine had it.
  opt.port = [] {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    uint16_t port = 17311;
    if (s >= 0 && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
      port = ntohs(a.sin_port);
    if (s >= 0) ::close(s);
    return port;
  }();
  opt.loopback_only = true;
  opt.shm_ring_name = ring_name;

  std::atomic<bool> stop_flag{false};
  std::thread srv_thread([&] {
    compute_serve(k, worker, opt, [](const std::string&) {}, &stop_flag, nullptr);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Client opens SHM rings
  ShmRingChannel req_ring(ring_name, 4 * 1024 * 1024, false);
  ShmRingChannel resp_ring(ring_name + "_resp", 4 * 1024 * 1024, false);

  // Send Ping over SHM
  std::string ping_req = R"({"op":"ping"})";
  bool pushed = req_ring.push_frame(1, 1001, ping_req.data(), ping_req.size());
  assert(pushed && "Failed to push frame to SHM request ring");

  std::string response_body;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (resp_ring.pop_frame([&](const ShmFrameHeader& hdr, const void* ptr, size_t len) {
      assert(hdr.stream_id == 1001);
      response_body.assign(reinterpret_cast<const char*>(ptr), len);
    })) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(!response_body.empty() && "No response received over SHM ring");
  assert(response_body.find("\"op\":\"ping\"") != std::string::npos);
  std::cout << "  ✔ compute_serve responded over SHM ring: " << response_body << "\n";

  // Send TSP over SHM
  std::string tsp_req = R"({"op":"tsp_exact","start":{"id":"nyc","lat":40.7128,"lng":-74.0060},"stops":[{"id":"philly","lat":39.9526,"lng":-75.1652},{"id":"dc","lat":38.9072,"lng":-77.0369}]})";
  pushed = req_ring.push_frame(1, 1002, tsp_req.data(), tsp_req.size());
  assert(pushed && "Failed to push TSP request to SHM ring");

  std::string tsp_response;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (resp_ring.pop_frame([&](const ShmFrameHeader& hdr, const void* ptr, size_t len) {
      assert(hdr.stream_id == 1002);
      tsp_response.assign(reinterpret_cast<const char*>(ptr), len);
    })) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(!tsp_response.empty() && "No TSP response over SHM ring");
  if (tsp_response.find("\"status\":\"done\"") == std::string::npos) { std::cerr << "TSP response: " << tsp_response << "\n"; std::abort(); }
  std::cout << "  ✔ compute_serve TSP route solved over SHM ring: " << tsp_response << "\n";

  stop_flag = true;
  // Trigger TCP to break accept block
  auto cl = hal::net_connect("127.0.0.1", opt.port, 50);
  if (srv_thread.joinable()) srv_thread.join();
}

static void test_grain_sidecar_builtins() {
  std::cout << "─── [3] Grain VM Sidecar Native Builtins Test ─────────────────\n";

  auto run_script = [](const std::string& src) -> Value {
    auto compiled = compile_source(src, "bench", /*keep_last_value=*/true);
    if (!compiled.ok()) {
      for (auto& d : compiled.errors) std::cerr << "  compile error, line " << d.line << ": " << d.message << "\n";
      std::exit(1);
    }
    Vm vm;
    install_builtins(vm);
    auto res = vm.run(compiled.script);
    if (!res.ok || !res.value.truthy()) { std::cerr << "  run failed, line " << res.line << ": " << res.error << "\n"; std::exit(1); }
    return res.value;
  };

  // 1. Haversine
  {
    std::string code = R"(
      let dist = sidecar_haversine(40.7128, -74.0060, 34.0522, -118.2437);
      assert(dist > 3900000.0 && dist < 4000000.0, "haversine distance check");
      dist;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_haversine returned NYC->LA: " << v.as_double() / 1000.0 << " km\n";
  }

  // 2. TSP
  {
    std::string code = R"(
      let route = sidecar_tsp("40.7128,-74.0060;34.0522,-118.2437;41.8781,-87.6298");
      assert(len(route) == 3, "tsp route length");
      route;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_tsp solved 3-city route: " << v.to_text() << "\n";
  }

  // 3. SHA256 & Keccak256
  {
    std::string code = R"(
      let h1 = sidecar_sha256("aios-kernel");
      let h2 = sidecar_keccak256("aios-kernel");
      assert(len(h1) == 64, "sha256 len");
      assert(len(h2) == 64, "keccak256 len");
      h1;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_sha256 computed: " << v.str() << "\n";
  }

  // 4. Ethereum & Bitcoin Address Derivation
  {
    std::string code = R"(
      let eth = sidecar_eth_addr("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
      assert(len(eth) == 42, "eth address format");
      assert(slice(eth, 0, 2) == "0x", "eth 0x prefix");
      let btc = sidecar_btc_addr("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 0);
      assert(btc == "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", "btc mainnet p2wpkh of the secp256k1 generator");
      eth;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_eth_addr derived: " << v.str() << "\n";
  }

  // 5. MIME Email & iCalendar JSON Parsing
  {
    std::string code = R"(
      let eml = "From: Alice <alice@example.com>\r\nTo: Bob <bob@example.com>\r\nSubject: AIOS Update\r\n\r\nHello AIOS!";
      let eml_json = sidecar_parse_eml(eml);
      assert(find(eml_json, "AIOS Update") >= 0, "eml subject parsed");

      let ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nSUMMARY:Kernel Sync\r\nDTSTART:20260921T120000Z\r\nEND:VEVENT\r\nEND:VCALENDAR";
      let ics_json = sidecar_parse_ical(ics);
      assert(find(ics_json, "Kernel Sync") >= 0, "ical summary parsed");
      eml_json;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_parse_eml & sidecar_parse_ical parsed structured data\n";
  }

  // 6. Headless SVG & PDF Render
  {
    std::string code = R"(
      let svg = "<svg width='100' height='100'><circle cx='50' cy='50' r='40' fill='green'/></svg>";
      let png_bytes = sidecar_svg_png_len(svg, 100, 100);
      assert(png_bytes > 50, "svg png size");

      let html = "<html><body><h1>AIOS Engine</h1></body></html>";
      let pdf_bytes = sidecar_pdf_len(html, "");
      assert(pdf_bytes > 100, "pdf size");
      png_bytes;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_svg_png_len & sidecar_pdf_len rendered vector streams (PNG: " 
              << v.as_int() << " bytes)\n";
  }

  // 7. Capability Minting & Attenuation
  {
    std::string code = R"(
      let cap = sidecar_cap_mint("root_admin", "agent_worker", "/channel/compute", 31);
      assert(len(cap) > 0, "minted cap");
      let child = sidecar_cap_attenuate(cap, 3);
      assert(len(child) > 0, "attenuated child cap");
      child;
    )";
    Value v = run_script(code);
    std::cout << "  ✔ Grain sidecar_cap_mint & sidecar_cap_attenuate produced valid tokens: " << v.str() << "\n";
  }
}

int main() {
  std::cout << "======================================================================\n";
  std::cout << "  AIOS CORE SHM EVENT FABRIC & USERLAND GRAIN INTEGRATION BENCHMARK   \n";
  std::cout << "======================================================================\n";

  test_shm_fabric_transport();
  test_compute_serve_shm_dispatch();
  test_grain_sidecar_builtins();

  std::cout << "\n======================================================================\n";
  std::cout << "  ALL SHM FABRIC & USERLAND SIDECAR INTEGRATION TESTS PASSED (100%)  \n";
  std::cout << "======================================================================\n";
  return 0;
}
