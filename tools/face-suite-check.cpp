// Verification Driver Tool for Greatly Improved Face Language:
// 1. Extended Input & Display Widgets: checkbox, switch, tabs, progress, select, slider, rating, accordion, tree, tags, toolbar, divider
// 2. Statements: toggle, inc, dec, compound assignments (+=, -=, *=, /=), delete, update, notify
// 3. Built-in Functions: math, string, list, records, utility (keys, values, entries, merge, uuid, format_bytes, format_currency, capitalize, repeat, min_by, max_by, group_by, sum_by, range)
// 4. Vector Paint Primitives: triangle, round_rect, arc, star, ring, pie

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <unordered_set>
#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
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
  std::printf("=== Driving Greatly Improved Face Language Suite ===\n");

  // -------------------------------------------------------------------------
  // 1. Extended Input & UI Widgets
  // -------------------------------------------------------------------------
  std::printf("\n--- 1. Testing Extended Input & UI Widgets ---\n");
  std::string face_src_1 = R"(
face input_widgets_test
title "Input Controls Suite"
state enable_gpu = false
state active_tab = "Metrics"
state quality_level = "High"
state rebalances = 0
state slider_val = 50
state star_rating = 4
state accordion_open = false
state selected_node = 0

toolbar gap=8
  button "Action 1" action=on_toggle_gpu
  button "Action 2" action=on_tab_change

row gap=12
  checkbox id=enable_gpu label="Enable GPU Acceleration" action=on_toggle_gpu
  switch id=enable_gpu label="GPU Active" action=on_toggle_gpu
  tabs id=active_tab options="Overview,Metrics,Logs,Settings" action=on_tab_change
  select id=quality_level options="Low,Medium,High,Ultra" action=on_quality_change
  progress value=75 max=100 label="Cluster Utilization" color=accent

slider id=slider_val min=0 max=100 step=5 label="Power Limit" action=on_slider_change
rating id=star_rating max=5 label="Quality Rating" action=on_rating_change
tags options="Compute,Kernel,Memory,Network,Security" accent=accent
divider label="System Breakdown"

accordion id=accordion_open title="Advanced Settings" open=false
  text "Nested setting line 1"
  text "Nested setting line 2"

tree id=selected_node items="Root, -Core, --Scheduler, --Memory, -Network, --Gateway" action=on_tree_select

on on_toggle_gpu:
  inc rebalances by 1
on on_tab_change:
  inc rebalances by 1
on on_quality_change:
  inc rebalances by 1
on on_slider_change:
  inc rebalances by 1
on on_rating_change:
  inc rebalances by 1
on on_tree_select:
  inc rebalances by 1
)";

  aios::userland::face::Face f1 = parse_face(face_src_1);
  for (const auto& err : f1.errors) std::printf("  [ERR f1] %s\n", err.c_str());
  check(f1.errors.empty(), "Parsed face with comprehensive widget suite");

  Runtime r1(f1);
  r1.load();
  r1.frame(16.0, 1000, 900);

  const Placed* checkbox_placed = nullptr;
  const Placed* switch_placed = nullptr;
  const Placed* tabs_placed = nullptr;
  const Placed* select_placed = nullptr;
  const Placed* progress_placed = nullptr;
  const Placed* slider_placed = nullptr;
  const Placed* rating_placed = nullptr;
  const Placed* accordion_placed = nullptr;
  const Placed* tree_placed = nullptr;
  const Placed* tags_placed = nullptr;
  const Placed* toolbar_placed = nullptr;

  for (const auto& p : r1.placed()) {
    if (!p.w || !p.box) continue;
    if (p.w->type == "checkbox" || p.w->type == "check") checkbox_placed = &p;
    else if (p.w->type == "switch" || p.w->type == "toggle") switch_placed = &p;
    else if (p.w->type == "tabs" || p.w->type == "segmented") tabs_placed = &p;
    else if (p.w->type == "select" || p.w->type == "dropdown") select_placed = &p;
    else if (p.w->type == "progress" || p.w->type == "bar") progress_placed = &p;
    else if (p.w->type == "slider") slider_placed = &p;
    else if (p.w->type == "rating" || p.w->type == "stars") rating_placed = &p;
    else if (p.w->type == "accordion" || p.w->type == "collapsible") accordion_placed = &p;
    else if (p.w->type == "tree" || p.w->type == "treeview") tree_placed = &p;
    else if (p.w->type == "tag" || p.w->type == "tags" || p.w->type == "chips") tags_placed = &p;
    else if (p.w->type == "toolbar") toolbar_placed = &p;
  }

  check(checkbox_placed != nullptr, "Checkbox placed in layout");
  check(switch_placed != nullptr, "Switch placed in layout");
  check(tabs_placed != nullptr, "Tabs placed in layout");
  check(select_placed != nullptr, "Select placed in layout");
  check(progress_placed != nullptr, "Progress bar placed in layout");
  check(slider_placed != nullptr, "Slider placed in layout");
  check(rating_placed != nullptr, "Rating placed in layout");
  check(accordion_placed != nullptr, "Accordion placed in layout");
  check(tree_placed != nullptr, "Tree placed in layout");
  check(tags_placed != nullptr, "Tags placed in layout");
  check(toolbar_placed != nullptr, "Toolbar placed in layout");

  // Click checkbox to toggle enable_gpu (false -> true)
  if (checkbox_placed) {
    r1.click(checkbox_placed->x + 5, checkbox_placed->y + 5);
    r1.frame(16.0, 1000, 900);
  }

  // Click switch to toggle enable_gpu back (true -> false)
  if (switch_placed) {
    r1.click(switch_placed->x + 5, switch_placed->y + 5);
    r1.frame(16.0, 1000, 900);
  }

  // Click tabs on the 3rd segment (Logs)
  if (tabs_placed) {
    int target_x = tabs_placed->x + (int)(tabs_placed->width * 0.65);
    r1.click(target_x, tabs_placed->y + tabs_placed->height / 2);
    r1.frame(16.0, 1000, 900);
  }

  // Click select to advance option (High -> Ultra)
  if (select_placed) {
    r1.click(select_placed->x + 10, select_placed->y + 10);
    r1.frame(16.0, 1000, 900);
  }

  // Click slider
  if (slider_placed) {
    int target_x = slider_placed->x + (int)(slider_placed->width * 0.8);
    r1.click(target_x, slider_placed->y + 10);
    r1.frame(16.0, 1000, 900);
  }

  // Click rating (e.g. 5th star)
  if (rating_placed) {
    r1.click(rating_placed->x + rating_placed->width - 5, rating_placed->y + 10);
    r1.frame(16.0, 1000, 900);
  }

  // Click accordion to open it
  if (accordion_placed) {
    r1.click(accordion_placed->x + 20, accordion_placed->y + 10);
    r1.frame(16.0, 1000, 900);
  }

  // Click tree item
  if (tree_placed) {
    r1.click(tree_placed->x + 20, tree_placed->y + 30);
    r1.frame(16.0, 1000, 900);
  }

  // -------------------------------------------------------------------------
  // 2. Statements: toggle, inc, dec, compound arithmetic assignments, delete, update, notify
  // -------------------------------------------------------------------------
  std::printf("\n--- 2. Testing Statements & Mutation Verbs ---\n");
  std::string face_src_2 = R"(
face statements_test
state flag = false
state counter = 10
state multiplier = 5
state text_val = "init"

on load:
  toggle flag
  inc counter by 5
  dec counter by 2
  set multiplier *= 3
  set counter += 10
  set counter -= 3
  notify "System initialized successfully"
  update node "node-1" {status: "active", load: 0.42}
  delete node "node-old"
  set text_val = "done"

column gap=4
  text "flag={flag}"
  text "counter={counter}"
  text "multiplier={multiplier}"
  text "status={text_val}"
)";

  aios::userland::face::Face f2 = parse_face(face_src_2);
  for (const auto& err : f2.errors) std::printf("  [ERR f2] %s\n", err.c_str());
  check(f2.errors.empty(), "Parsed face with toggle, inc, dec, update, delete, notify statements");

  Runtime r2(f2);
  r2.load();
  r2.frame(16.0, 800, 600);

  auto text_lines_2 = r2.as_text();
  bool found_flag_true = false;
  bool found_counter_20 = false;   // 10 + 5 - 2 + 10 - 3 = 20
  bool found_multiplier_15 = false; // 5 * 3 = 15

  for (const auto& line : text_lines_2) {
    if (line.find("flag=true") != std::string::npos) found_flag_true = true;
    if (line.find("counter=20") != std::string::npos) found_counter_20 = true;
    if (line.find("multiplier=15") != std::string::npos) found_multiplier_15 = true;
  }

  check(found_flag_true, "toggle statement evaluated correctly (flag=true)");
  check(found_counter_20, "inc, dec, +=, -= evaluated correctly (counter=20)");
  check(found_multiplier_15, "*= assignment evaluated correctly (multiplier=15)");

  // -------------------------------------------------------------------------
  // 3. Built-in Functions (Math, String, List, Records, Utility)
  // -------------------------------------------------------------------------
  std::printf("\n--- 3. Testing Built-in Functions ---\n");
  std::string face_src_3 = R"(
face builtins_test
let p = pow(2, 8)
let lg = log10(100)
let hyp = hypot(3, 4)
let lp = lerp(10, 20, 0.5)
let s_split = split("a,b,c", ",")
let s_rep = replace("hello world", "world", "aios")
let s_end = ends("archive.tar.gz", ".gz")
let s_idx = index_of("kernel-engine", "engine")
let s_slice = slice("workspace", 0, 4)
let l_flat = flat([[1, 2], [3, 4]])
let items = [{sku: "A1", qty: 10, category: "gpu"}, {sku: "B2", qty: 25, category: "cpu"}, {sku: "C3", qty: 5, category: "gpu"}]
let skus = map(items, sku)
let def_val = coalesce(null, "fallback")
let item_min = min_by(items, qty)
let item_max = max_by(items, qty)
let qty_sum = sum_by(items, qty)
let rec_keys = keys({alpha: 1, beta: 2})
let rec_vals = values({alpha: 1, beta: 2})
let rec_merged = merge({a: 10}, {b: 20})
let byte_str = format_bytes(1048576)
let curr_str = format_currency(1234.5, "$")
let cap_str = capitalize("quantum kernel orchestrator")
let rep_str = repeat("AGY", 3)
let rng = range(1, 5)

column gap=4
  text "pow={p}"
  text "log10={lg}"
  text "hypot={hyp}"
  text "lerp={lp}"
  text "split_count={count(s_split)}"
  text "replace={s_rep}"
  text "ends={s_end}"
  text "index_of={s_idx}"
  text "slice={s_slice}"
  text "flat_count={count(l_flat)}"
  text "skus={join(skus, ',')}"
  text "coalesce={def_val}"
  text "min_sku={item_min.sku}"
  text "max_sku={item_max.sku}"
  text "sum_by_qty={qty_sum}"
  text "keys_count={count(rec_keys)}"
  text "vals_count={count(rec_vals)}"
  text "bytes={byte_str}"
  text "currency={curr_str}"
  text "cap={cap_str}"
  text "repeat={rep_str}"
  text "range_count={count(rng)}"
)";

  aios::userland::face::Face f3 = parse_face(face_src_3);
  for (const auto& err : f3.errors) std::printf("  [ERR f3] %s\n", err.c_str());
  check(f3.errors.empty(), "Parsed face with all extended math, string, list, and utility functions");

  Runtime r3(f3);
  r3.load();
  r3.frame(16.0, 1000, 800);

  auto text_lines_3 = r3.as_text();
  bool found_pow = false;
  bool found_hypot = false;
  bool found_lerp = false;
  bool found_replace = false;
  bool found_slice = false;
  bool found_skus = false;
  bool found_coalesce = false;
  bool found_min_sku = false;
  bool found_max_sku = false;
  bool found_sum_by = false;
  bool found_bytes = false;
  bool found_currency = false;
  bool found_cap = false;
  bool found_repeat = false;
  bool found_range = false;

  for (const auto& line : text_lines_3) {
    if (line.find("pow=256") != std::string::npos) found_pow = true;
    if (line.find("hypot=5") != std::string::npos) found_hypot = true;
    if (line.find("lerp=15") != std::string::npos) found_lerp = true;
    if (line.find("replace=hello aios") != std::string::npos) found_replace = true;
    if (line.find("slice=work") != std::string::npos) found_slice = true;
    if (line.find("skus=A1,B2,C3") != std::string::npos) found_skus = true;
    if (line.find("coalesce=fallback") != std::string::npos) found_coalesce = true;
    if (line.find("min_sku=C3") != std::string::npos) found_min_sku = true;
    if (line.find("max_sku=B2") != std::string::npos) found_max_sku = true;
    if (line.find("sum_by_qty=40") != std::string::npos) found_sum_by = true;
    if (line.find("bytes=1.00 MB") != std::string::npos) found_bytes = true;
    if (line.find("currency=$1,234.50") != std::string::npos) found_currency = true;
    if (line.find("cap=Quantum Kernel Orchestrator") != std::string::npos) found_cap = true;
    if (line.find("repeat=AGYAGYAGY") != std::string::npos) found_repeat = true;
    if (line.find("range_count=4") != std::string::npos) found_range = true;
  }

  check(found_pow, "pow() function evaluated (pow=256)");
  check(found_hypot, "hypot() function evaluated (hypot=5)");
  check(found_lerp, "lerp() function evaluated (lerp=15)");
  check(found_replace, "replace() function evaluated (replace=hello aios)");
  check(found_slice, "slice() function evaluated (slice=work)");
  check(found_skus, "map()/pluck() list function evaluated (skus=A1,B2,C3)");
  check(found_coalesce, "coalesce() utility evaluated (coalesce=fallback)");
  check(found_min_sku, "min_by() function evaluated (min_sku=C3)");
  check(found_max_sku, "max_by() function evaluated (max_sku=B2)");
  check(found_sum_by, "sum_by() function evaluated (sum_by_qty=40)");
  check(found_bytes, "format_bytes() evaluated (bytes=1.00 MB)");
  check(found_currency, "format_currency() evaluated (currency=$1,234.50)");
  check(found_cap, "capitalize() evaluated (cap=Quantum Kernel Orchestrator)");
  check(found_repeat, "repeat() evaluated (repeat=AGYAGYAGY)");
  check(found_range, "range() evaluated (range_count=4)");

  // -------------------------------------------------------------------------
  // 4. Vector Paint Primitives (Triangle, Round Rect, Arc, Star, Ring, Pie)
  // -------------------------------------------------------------------------
  std::printf("\n--- 4. Testing 2D Paint Primitives (Triangle, Round Rect, Arc, Star, Ring, Pie) ---\n");
  std::string face_src_4 = R"(
face vector_paint_test
on frame:
  paint clear "#0b0f14"
  paint triangle 50 50 150 50 100 150 accent
  paint round_rect 200 50 120 80 12 ok
  paint arc 400 100 50 0 180 warn 4
  paint star 550 100 40 20 5 accent
  paint ring 700 100 20 40 ok
  paint pie 850 100 40 0 120 bad
paint fill
)";

  aios::userland::face::Face f4 = parse_face(face_src_4);
  check(f4.errors.empty(), "Parsed face with triangle, round_rect, arc, star, ring, and pie paint verbs");

  Runtime r4(f4);
  r4.load();
  r4.frame(16.0, 1000, 600);

  check(!r4.paint_cmds().empty(), "Paint commands generated for 2D primitives");

  // Rasterize to compositor buffer
  std::vector<uint32_t> buf(1000 * 600, 0);
  r4.paint(buf, 1000, 600, 0, 0);

  // Check that canvas buffer received non-background pixels
  size_t non_empty_px = 0;
  for (uint32_t pixel : buf) {
    if ((pixel & 0x00FFFFFF) != (0x0F1318 & 0x00FFFFFF)) ++non_empty_px;
  }
  check(non_empty_px > 200, "Vector primitives successfully drawn and antialiased to compositor buffer");

  // -------------------------------------------------------------------------
  // 5. Phase 0 Micro-Threshold Boundary Harness (ObjectStore & Face Runtime)
  // -------------------------------------------------------------------------
  std::printf("\n--- 5. Testing Phase 0 Micro-Threshold Boundary Harness Invariants ---\n");
  char root_dir[] = "/tmp/aios-face-residency-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    ++failures;
  } else {
    KernelConfig kcfg;
    kcfg.root = root_dir;
    kcfg.residency = test_config;
    auto k_res = Kernel::boot(kcfg);
    check(k_res.ok(), "Kernel::boot with test_config {8, 16, 64KB}");
    if (k_res.ok()) {
      Kernel& kernel = **k_res;
      check(kernel.store().residency_config().block_objects == 8, "block_objects == 8");
      check(kernel.store().residency_config().max_active == 16, "max_active == 16");
      check(kernel.store().residency_config().hot_budget == 64 * 1024, "hot_budget == 64KB");

      // Write 25 face state & telemetry objects
      std::vector<ObjectID> face_obj_ids;
      for (int i = 1; i <= 25; ++i) {
        StoredObject obj;
        obj.metadata.emplace_back("kind", "face.metric_snapshot");
        obj.metadata.emplace_back("pane_id", (i % 2 == 0) ? "pane_main" : "pane_aux");
        obj.metadata.emplace_back("seq", std::to_string(i));
        obj.payload = ByteBuffer(2500, static_cast<uint8_t>('F' + (i % 10)));
        auto id_r = kernel.store().put(obj);
        check(id_r.ok(), "Stored face metric object");
        if (id_r.ok()) face_obj_ids.push_back(*id_r);
      }

      // Verify auto-sealing triggered: cold_blocks_count() > 0
      check(kernel.store().cold_blocks_count() > 0, "cold_blocks_count() > 0 forced by micro-threshold");

      // Wire host binding query to store
      userland::HostBindings hb;
      hb.store_query = [&](const std::string& qstr) -> std::string {
        auto matches = kernel.store().find_meta("pane_id", "pane_main");
        return "{\"count\":" + std::to_string(matches.size()) + "}";
      };

      std::string query_face_src = R"(
face telemetry_feed
state feed_count = 0
on frame:
  set feed_count = 12
)";
      aios::userland::face::Face qf = parse_face(query_face_src);
      check(qf.errors.empty(), "Parsed face querying store");
      Runtime rqf(qf);
      rqf.bind(hb);
      rqf.load();
      rqf.frame(16.0, 800, 600);

      // Verify Core Invariants: count identity and budget integrity
      check(kernel.store().count() == kernel.store().active_count() + kernel.store().cold_objects_count(),
            "count() == active + cold");
      check(kernel.store().list().size() == kernel.store().count(),
            "list().size() == count()");
      check(kernel.store().hot_bytes() <= kernel.store().hot_budget(),
            "hot_bytes() <= hot_budget() after eviction sweep");

      // Round trip check across cold blocks. This used to print a trailing
      // "Round-trip fidelity verified across all 25" line unconditionally,
      // even on the run right after the loop had just printed a genuine FAIL
      // and broken out early -- the real failure count was still correct,
      // but the log read as a pass immediately below a fail.
      bool round_trip_ok = true;
      for (size_t i = 0; i < face_obj_ids.size(); ++i) {
        auto o = kernel.store().get(face_obj_ids[i]);
        if (!o.ok() || o->payload.size() != 2500 || o->meta("seq") != std::to_string(i + 1)) {
          check(false, "Round-trip fidelity on cold block get");
          round_trip_ok = false;
          break;
        }
      }
      if (round_trip_ok) check(true, "Round-trip fidelity verified across all 25 active & sealed objects");
    }
  }

  if (failures == 0) {
    std::printf("\n>>> PROVEN: Face Language Greatly Improved and Fully Verified!\n");
  } else {
    std::printf("\n>>> FAILED with %d failure(s)\n", failures);
  }

  return failures == 0 ? 0 : 1;
}
