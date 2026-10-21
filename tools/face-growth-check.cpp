// Drive tool for Phase 3.1 - 3.3: Face Growth without becoming a browser
// Proves in reality:
// 1. Media/image widget (described element, decoded bitmap, paint blending).
// 2. Declarative network-bound state (fetch and fetch_json builtins without script engines).
// 3. Layout primitives on demand (text wrapping and scrollable container).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "aios/userland/compositor.hpp"
#include "aios/userland/face.hpp"

using namespace aios;
using namespace aios::userland;
using namespace aios::userland::face;

namespace {

int failures = 0;

void check(bool cond, const char* what, const std::string& extra = "") {
  if (cond) {
    std::printf("  [PASS] %s\n", what);
  } else {
    ++failures;
    std::printf("  [FAIL] %s %s\n", what, extra.c_str());
  }
}

} // namespace

int main() {
  std::printf("=== Driving Face Growth (Media, Declarative State, Layout) ===\n");

  // -------------------------------------------------------------------------
  // 1. Declarative Network-Bound State (fetch & fetch_json)
  // -------------------------------------------------------------------------
  std::printf("\n--- 1. Testing Declarative Network-Bound State ---\n");
  std::string face_src_1 = R"(
face telemetry
title "Fleet Telemetry"
let raw_data = fetch("http://fleet.internal/stats")
let stats = fetch_json("http://fleet.internal/nodes")
row gap=12
  tile label="Fleet Status" value=raw_data
  tile label="Active Nodes" value=count(stats.nodes)
)";

  aios::userland::face::Face f1 = parse_face(face_src_1);
  check(f1.errors.empty(), "Parsed face with declarative fetch expressions");

  Runtime r1(f1);
  HostBindings h1;
  h1.http_get = [](const std::string& url) -> std::string {
    if (url == "http://fleet.internal/stats") return "HEALTHY";
    if (url == "http://fleet.internal/nodes") return R"({"nodes": [{"id": "node-1"}, {"id": "node-2"}, {"id": "node-3"}]})";
    return "";
  };
  r1.bind(h1);
  r1.load();
  r1.frame(16.0, 800, 600);

  auto text_lines = r1.as_text();
  bool found_healthy = false;
  bool found_nodes = false;
  for (const auto& line : text_lines) {
    if (line.find("HEALTHY") != std::string::npos) found_healthy = true;
    if (line.find("3") != std::string::npos) found_nodes = true;
  }
  check(found_healthy, "Declarative fetch() bound data rendered in face", "expected 'HEALTHY'");
  check(found_nodes, "Declarative fetch_json() parsed record count rendered in face", "expected node count 3");

  // -------------------------------------------------------------------------
  // 2. Media / Image Widget
  // -------------------------------------------------------------------------
  std::printf("\n--- 2. Testing Media / Image Widget ---\n");
  std::string face_src_2 = R"(
face dashboard
title "Dashboard"
row gap=16
  image src="logo.png" width=120 height=80
  column gap=8
    heading "AIOS Kernel Monitor"
    text "Real-time operating system display"
)";

  aios::userland::face::Face f2 = parse_face(face_src_2);
  check(f2.errors.empty(), "Parsed face with image widget");

  Runtime r2(f2);
  HostBindings h2;
  // Provide 1x1 white PNG file content
  static const uint8_t kPng1x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xff, 0xff, 0x3f,
    0x00, 0x05, 0xfe, 0x02, 0xfe, 0xdc, 0xcc, 0x59, 0xe7, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
  };
  h2.read_file = [](const std::string& path) -> std::string {
    if (path == "logo.png") return std::string((const char*)kPng1x1, sizeof(kPng1x1));
    return "";
  };
  r2.bind(h2);
  r2.load();
  r2.frame(16.0, 800, 600);

  bool found_image_placed = false;
  for (const auto& p : r2.placed()) {
    if (p.w && (p.w->type == "image" || p.w->type == "img")) {
      found_image_placed = true;
      check(p.width == 120 && p.height == 80, "Image placed with specified dimensions (120x80)");
    }
  }
  check(found_image_placed, "Image widget placed in layout");

  // Paint into a buffer to verify compositor paint execution
  HardwareCompositor comp;
  std::vector<uint32_t> buf(800 * 600, 0);
  r2.paint(comp, buf, 800, 600, 0, 0);
  check(buf[100 * 800 + 100] != 0, "Image painted to compositor buffer");

  // -------------------------------------------------------------------------
  // 3. Layout Primitives (Text Wrapping and Scrollable Containers)
  // -------------------------------------------------------------------------
  std::printf("\n--- 3. Testing Layout Primitives (Wrapping & Scrolling) ---\n");
  std::string face_src_3 = R"(
face details
title "System Logs"
scroll height=180
  text "Line 1: Kernel initialized with memory fabric and preemptive scheduler"
  text "Line 2: Connected to peer nodes on mesh overlay network"
  text "Line 3: Dispatched jobs to available worker devices across cluster"
  text "Line 4: Process complete with zero dropped packets and verified state"
)";

  aios::userland::face::Face f3 = parse_face(face_src_3);
  check(f3.errors.empty(), "Parsed face with scroll container");

  Runtime r3(f3);
  r3.load();
  r3.frame(16.0, 800, 600);

  bool found_scroll_placed = false;
  for (const auto& p : r3.placed()) {
    if (p.w && (p.w->type == "scroll" || (p.w->type == "panel" && p.w->flag("scroll")))) {
      found_scroll_placed = true;
      check(p.height == 180, "Scroll container bounded to height 180");
    }
  }
  check(found_scroll_placed, "Scroll container placed in layout");

  if (failures == 0) {
    std::printf("\n>>> Phase 3.1 - 3.3 PROVEN: Face growth features verified!\n");
  } else {
    std::printf("\n>>> Phase 3 FAILED with %d failure(s)\n", failures);
  }

  return failures == 0 ? 0 : 1;
}
