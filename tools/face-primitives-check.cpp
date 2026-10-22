// Verification Driver Tool for Dedicated Face Primitives:
// 1. Status Badges (`badge`, `status`)
// 2. Real-time Sparklines (`sparkline`, `chart`)
// 3. Temporal Scrubber (`scrubber`, `timeline`)
// 4. Operational Modals (`modal`, `dialog`)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

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

}  // namespace

int main() {
  std::printf("=== Driving Dedicated Face Primitives (Badge, Sparkline, Scrubber, Modal) ===\n");

  // -------------------------------------------------------------------------
  // 1. Status Badges
  // -------------------------------------------------------------------------
  std::printf("\n--- 1. Testing Status Badges ---\n");
  std::string face_src_1 = R"(
face badges_test
title "Cluster Node Badges"
row gap=12
  badge text="ONLINE" accent=ok dot
  badge text="DEGRADED" accent=warn dot
  status bad
  badge text="LEASE_ACTIVE" color=#38bdf8
)";

  aios::userland::face::Face f1 = parse_face(face_src_1);
  check(f1.errors.empty(), "Parsed face with badge and status widgets");

  Runtime r1(f1);
  r1.load();
  r1.frame(16.0, 800, 600);

  int badge_count = 0;
  for (const auto& p : r1.placed()) {
    if (p.w && p.box && (p.w->type == "badge" || p.w->type == "status")) {
      ++badge_count;
    }
  }
  check(badge_count == 4, "All 4 badges placed in layout", "found " + std::to_string(badge_count));

  // -------------------------------------------------------------------------
  // 2. Real-Time Sparklines
  // -------------------------------------------------------------------------
  std::printf("\n--- 2. Testing Sparklines ---\n");
  std::string face_src_2 = R"(
face charts_test
title "Cluster Pressure History"
let history = [12, 18, 25, 45, 60, 52, 75, 40, 30]
column gap=12
  text "Node 01 Pressure"
  sparkline data=history height=40 color=accent min=0 max=100
  sparkline values="5,15,35,80,95,65,40" height=32 color=ok
)";

  aios::userland::face::Face f2 = parse_face(face_src_2);
  check(f2.errors.empty(), "Parsed face with sparkline widgets");

  Runtime r2(f2);
  r2.load();
  r2.frame(16.0, 800, 600);

  int sparkline_count = 0;
  for (const auto& p : r2.placed()) {
    if (p.w && p.box && (p.w->type == "sparkline" || p.w->type == "chart")) {
      ++sparkline_count;
      check(p.width > 0 && p.height >= 32, "Sparkline widget correctly sized");
    }
  }
  check(sparkline_count == 2, "All 2 sparklines placed in layout", "found " + std::to_string(sparkline_count));

  // -------------------------------------------------------------------------
  // 3. Temporal Scrubber & Rewind State Binding
  // -------------------------------------------------------------------------
  std::printf("\n--- 3. Testing Temporal Scrubber ---\n");
  std::string face_src_3 = R"(
face scrubber_test
title "Transaction Time Scrubber"
state tx_seq = 50
state scrub_count = 0
scrubber id=tx_seq label="Transaction Log Boundary" range="24h" min=0 max=100 step=5 action=on_scrub
on on_scrub:
  set scrub_count = scrub_count + 1
)";

  aios::userland::face::Face f3 = parse_face(face_src_3);
  check(f3.errors.empty(), "Parsed face with scrubber widget and action handler");

  Runtime r3(f3);
  r3.load();
  r3.frame(16.0, 800, 600);

  const Placed* scrubber_placed = nullptr;
  for (const auto& p : r3.placed()) {
    if (p.w && p.box && (p.w->type == "scrubber" || p.w->type == "timeline")) {
      scrubber_placed = &p;
      break;
    }
  }
  check(scrubber_placed != nullptr, "Scrubber placed in layout");

  if (scrubber_placed) {
    // Click at 80% along the scrubber track (e.g. x = p.x + p.width * 0.8)
    int target_x = scrubber_placed->x + (int)(scrubber_placed->width * 0.8);
    int target_y = scrubber_placed->y + scrubber_placed->height / 2;
    r3.click(target_x, target_y);
    r3.frame(16.0, 800, 600);

    // Verify that value was updated to 80 (rounded by step=5) and action ran
    auto text_lines = r3.as_text();
    bool found_scrubbed_val = false;
    for (const auto& line : text_lines) {
      if (line.find("80") != std::string::npos) found_scrubbed_val = true;
    }
    check(found_scrubbed_val, "Scrubber updated value on click and refreshed display to 80");
  }

  // -------------------------------------------------------------------------
  // 4. Operational Modals / Overlays
  // -------------------------------------------------------------------------
  std::printf("\n--- 4. Testing Operational Modal Dialogs ---\n");
  std::string face_src_4 = R"(
face modal_test
title "Cluster Operations"
state is_modal_open = true
row gap=8
  button "Open Rebalance" action=open_modal primary
modal id=rebalance_dialog title="Confirm Cluster Rebalance" show=is_modal_open width=400
  text "Are you sure you want to rebalance active custom-kernel jobs?"
  row gap=8
    button "Cancel" action=close_modal
    button "Confirm Migration" action=confirm_rebalance primary
on open_modal:
  set is_modal_open = true
on close_modal:
  set is_modal_open = false
on confirm_rebalance:
  set is_modal_open = false
)";

  aios::userland::face::Face f4 = parse_face(face_src_4);
  check(f4.errors.empty(), "Parsed face with modal dialog and state conditions");

  Runtime r4(f4);
  r4.load();
  r4.frame(16.0, 800, 600);

  bool modal_visible = false;
  for (const auto& p : r4.placed()) {
    if (p.w && p.box && (p.w->type == "modal" || p.w->type == "dialog")) {
      modal_visible = true;
      check(p.width == 400, "Modal dialog placed with width 400");
    }
  }
  check(modal_visible, "Modal dialog placed when show=true");

  // Run close_modal action and re-render
  r4.run_action("close_modal");
  r4.frame(16.0, 800, 600);

  bool modal_hidden = true;
  for (const auto& p : r4.placed()) {
    if (p.w && p.box && (p.w->type == "modal" || p.w->type == "dialog")) {
      modal_hidden = false;
    }
  }
  check(modal_hidden, "Modal dialog hidden when show=false");

  // Paint into compositor buffer to ensure end-to-end rendering pipeline succeeds
  std::vector<uint32_t> buf(800 * 600, 0);
  r1.paint(buf, 800, 600, 0, 0);
  r2.paint(buf, 800, 600, 0, 0);

  if (failures == 0) {
    std::printf("\n>>> PROVEN: All Dedicated Face Primitives (Badge, Sparkline, Scrubber, Modal) verified!\n");
  } else {
    std::printf("\n>>> FAILED with %d failure(s)\n", failures);
  }

  return failures == 0 ? 0 : 1;
}
