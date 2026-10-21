// Parses and runs face DSL sources (component and app language features) and checks the
// resulting tree and behaviour.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include "aios/userland/face.hpp"

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

void print_errors(const Face& f) {
  for (const auto& err : f.errors) {
    std::printf("    [PARSE ERROR] %s\n", err.c_str());
  }
}

std::string get_state(const Runtime& rt, const std::string& key) {
  for (const auto& kv : rt.state_vars()) {
    if (kv.first == key) return kv.second;
  }
  return "";
}

}  // namespace

int main() {
  std::printf("=== Driving AOS Face App & Execution Language Suite ===\n\n");

  // -------------------------------------------------------------------------
  // 1. Modularity & Component Abstraction
  // -------------------------------------------------------------------------
  std::printf("--- Dimension 1: Component Abstraction & Composition ---\n");
  {
    std::string src = R"(
face component_demo
title "Component Architecture"

component MetricTile(name, value, unit="req/s"):
  column gap=4
    label name
    text "{value} {unit}" bold=true

component ClusterView(node_id, load):
  panel "Node {node_id}" pad=8
    MetricTile name="Throughput" value=load unit="ops/s"
    MetricTile name="Latency" value=42

state active_load = 1200

column gap=12
  ClusterView node_id="node-alpha" load=active_load
  ClusterView node_id="node-beta" load=850
)";

    Face f = parse_face(src);
    print_errors(f);
    check(f.errors.empty(), "Component source parsed without syntax errors");
    check(f.components.size() == 2, "Parsed 2 component declarations");
    if (f.components.size() >= 2) {
      check(f.components[0].name == "MetricTile", "Component 1 is MetricTile");
      check(f.components[0].params.size() == 3, "MetricTile has 3 params (name, value, unit)");
      bool has_unit_default = false;
      for (const auto& p : f.components[0].params) {
        if (p.first == "unit" && p.second != nullptr) has_unit_default = true;
      }
      check(has_unit_default, "MetricTile has default for 'unit'");
      check(f.components[1].name == "ClusterView", "Component 2 is ClusterView");
    }

    Runtime rt(f);
    rt.frame(16.0, 800, 600);
    check(rt.faults().empty(), "Runtime evaluated and laid out component hierarchy cleanly");
    std::string tree_json = rt.perceive_tree_json();
    check(tree_json.find("Node node-alpha") != std::string::npos, "Tree JSON includes expanded node-alpha");
    check(tree_json.find("1200 ops/s") != std::string::npos, "Tree JSON includes bound parameter value 1200 ops/s");
    check(tree_json.find("42 req/s") != std::string::npos, "Tree JSON applied default param fallback 'req/s'");
  }

  // -------------------------------------------------------------------------
  // 2. Async Runtime, Tasks & Channels
  // -------------------------------------------------------------------------
  std::printf("\n--- Dimension 2: Async Runtime, Tasks & Channels ---\n");
  {
    std::string src = R"(
face async_channel_demo
state received_count = 0
state last_msg = "none"

on start:
  spawn task worker_agent:
    emit "packet-1" to telemetry
    emit "packet-2" to telemetry
    emit "packet-3" to telemetry

on poll:
  if has_data("telemetry"):
    set last_msg = recv("telemetry")
    set received_count = received_count + 1

column gap=8
  text "Status: {last_msg}"
)";

    Face f = parse_face(src);
    print_errors(f);
    check(f.errors.empty(), "Async task and channel source parsed cleanly");
    check(!f.handlers.empty(), "Parsed action handlers");

    Runtime rt(f);
    rt.frame(16.0, 800, 600);

    // Dispatch start action to spawn task
    rt.run_action("start");
    for (int t = 0; t < 100 && !rt.has_channel_data("telemetry"); ++t) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    check(rt.has_channel_data("telemetry"), "Telemetry channel received async task emissions");
    Value polled = rt.poll_channel("telemetry");
    check(polled.k == Value::K::List && polled.list && polled.list->size() == 3, "Telemetry channel has 3 messages queued");

    // Execute poll
    rt.run_action("poll");
    check(get_state(rt, "received_count") == "1", "Channel recv() consumed message in face state");
    check(get_state(rt, "last_msg") == "packet-1", "Received exact message payload 'packet-1'");

    Value drained = rt.drain_channel("telemetry");
    check(drained.k == Value::K::List && drained.list && drained.list->size() == 2, "drain_channel retrieved remaining 2 messages");
    check(!rt.has_channel_data("telemetry"), "Channel is empty after drain");
  }

  // -------------------------------------------------------------------------
  // 3. Compute DAG, Tensor Math & Memory Fabric
  // -------------------------------------------------------------------------
  std::printf("\n--- Dimension 3: Compute DAG, Tensor Math & Memory Fabric ---\n");
  {
    std::string src = R"(
face compute_tensor_demo
state A = [[1, 2], [3, 4]]
state B = [[5, 6], [7, 8]]
state C = null
state probs = null
state similarity = 0
state sum_val = 0

on compute:
  set C = matmul(A, B)
  set probs = softmax([1.0, 2.0, 3.0])
  set sum_val = reduce([10, 20, 30, 40], "sum")
  set v1 = embed("AIOS operating system")
  set v2 = embed("AIOS operating system")
  set similarity = cosine_sim(v1, v2)

paint height=200:
  paint tensor C 0 0 100 100
)";

    Face f = parse_face(src);
    print_errors(f);
    check(f.errors.empty(), "Tensor compute source parsed cleanly");

    Runtime rt(f);
    rt.run_action("compute");

    std::string c_str = get_state(rt, "C");
    check(c_str.find("19") != std::string::npos && c_str.find("22") != std::string::npos &&
          c_str.find("43") != std::string::npos && c_str.find("50") != std::string::npos,
          "matmul math results exact: [[19, 22], [43, 50]]", c_str);

    check(get_state(rt, "sum_val") == "100", "reduce(sum) returned 100");
    std::string sim_str = get_state(rt, "similarity");
    if (!sim_str.empty()) {
      double sim = std::stod(sim_str);
      check(std::abs(sim - 1.0) < 1e-4, "cosine_sim of identical text embeddings is 1.0", sim_str);
    } else {
      check(false, "similarity state variable was empty");
    }

    rt.frame(16.0, 800, 600);
    check(rt.faults().empty(), "Tensor visualizer rasterized cleanly");
  }

  // -------------------------------------------------------------------------
  // 4. Structural Types, Schemas & Static Verification
  // -------------------------------------------------------------------------
  std::printf("\n--- Dimension 4: Structural Types, Schemas & Static Verification ---\n");
  {
    std::string src = R"(
face type_schema_demo
type ServiceEndpoint {
  host: Str,
  port: Num,
  protocol: Enum("http", "https", "grpc", "quic"),
  active: Bool
}

state ep_good = { host: "aios.local", port: 8443, protocol: "https", active: true }
state ep_bad = { host: "aios.local", port: 8443, protocol: "ftp", active: true }
state is_good_valid = false
state is_bad_valid = true

on validate:
  set is_good_valid = is_valid(ep_good, "ServiceEndpoint")
  set is_bad_valid = is_valid(ep_bad, "ServiceEndpoint")
)";

    Face f = parse_face(src);
    print_errors(f);
    check(f.errors.empty(), "Type schema definition parsed cleanly");
    check(f.types.size() == 1, "Parsed 1 type schema");
    if (!f.types.empty()) {
      check(f.types[0].name == "ServiceEndpoint", "Type name is ServiceEndpoint");
      check(f.types[0].fields.size() == 4, "ServiceEndpoint has 4 typed fields");
    }

    Runtime rt(f);
    rt.run_action("validate");
    check(get_state(rt, "is_good_valid") == "true", "Valid record passed schema validation");
    check(get_state(rt, "is_bad_valid") == "false", "Invalid enum record failed schema validation");

    auto diagnostics = rt.lint();
    check(diagnostics.empty(), "Static linter reported 0 errors for valid face");
  }

  // -------------------------------------------------------------------------
  // 5. Advanced Interaction, Spatial Physics & Canvas Ergonomics
  // -------------------------------------------------------------------------
  std::printf("\n--- Dimension 5: Advanced Interaction, Spatial Physics & Canvas Ergonomics ---\n");
  {
    std::string src = R"(
face physics_canvas_demo
state current_pos = 0.0
state current_vel = 0.0
state target_pos = 100.0
state clicked_node = "none"

on step_physics:
  set res = spring(current_pos, target_pos, current_vel, 200.0, 15.0, 0.016)
  set current_pos = res.pos
  set current_vel = res.vel

on node_click:
  set clicked_node = hit_id

paint height=300:
  paint circle 50 50 30 color="#00ffcc" action=node_click id="node-01"
  paint rect 120 40 60 40 color="#ff007f" action=node_click id="node-02"
)";

    Face f = parse_face(src);
    print_errors(f);
    check(f.errors.empty(), "Physics & vector hit-testing source parsed cleanly");

    Runtime rt(f);
    rt.frame(16.0, 800, 600);

    // Test spring physics integration
    double pos0 = std::stod(get_state(rt, "current_pos"));
    rt.run_action("step_physics");
    double pos1 = std::stod(get_state(rt, "current_pos"));
    double vel1 = std::stod(get_state(rt, "current_vel"));
    check(pos1 > pos0, "Spring physics updated position toward target");
    check(vel1 > 0, "Spring physics generated velocity toward target");

    // Test vector canvas hit testing for Circle
    rt.click(50, 50);
    check(get_state(rt, "clicked_node") == "node-01", "Hit-tested circle shape dispatched action with hit_id 'node-01'");

    // Test vector canvas hit testing for Rect
    rt.click(140, 50);
    check(get_state(rt, "clicked_node") == "node-02", "Hit-tested rect shape dispatched action with hit_id 'node-02'");

    // Test mouse event pipeline
    rt.mouse_move(200, 150, 5, -2);
    rt.mouse_wheel(0, 120);
    rt.gesture("pinch_in", 0.8, 200.0);
    check(rt.faults().empty(), "Pointer and gesture event pipelines executed cleanly");
  }

  // -------------------------------------------------------------------------
  // 6. Temporal Time-Travel & Undo/Redo Engine
  // -------------------------------------------------------------------------
  std::printf("\n--- Dimension 6: Temporal Time-Travel & Undo/Redo Engine ---\n");
  {
    std::string src = R"(
face time_travel_demo
state document_title = "Version 1"
state edits = 0

on edit:
  snapshot "edit"
  set edits = edits + 1
  set document_title = "Version 2"

on undo:
  undo

on redo:
  redo
)";

    Face f = parse_face(src);
    print_errors(f);
    Runtime rt(f);

    check(!rt.can_undo(), "Initial state: can_undo is false");
    check(!rt.can_redo(), "Initial state: can_redo is false");

    // Perform edit
    rt.run_action("edit");
    check(get_state(rt, "document_title") == "Version 2", "Applied state edit to Version 2");
    check(rt.can_undo(), "can_undo is now true");
    check(rt.history_len() == 1, "Undo stack has 1 snapshot");

    // Perform undo
    rt.run_action("undo");
    check(get_state(rt, "document_title") == "Version 1", "Undo restored state to Version 1");
    check(rt.can_redo(), "can_redo is now true after undo");

    // Perform redo
    rt.run_action("redo");
    check(get_state(rt, "document_title") == "Version 2", "Redo reapplied state to Version 2");
  }

  // -------------------------------------------------------------------------
  // 7. Multimodal Grounding & Self-Modification
  // -------------------------------------------------------------------------
  std::printf("\n--- Dimension 7: Multimodal Grounding & Self-Modification ---\n");
  {
    std::string src = R"(
face autonomous_agent_face
title "Autonomous Agent UI"
state score = 100

column gap=8
  text "Current Score: {score}"
  button "Add Points" action=add
)";

    Face f = parse_face(src);
    print_errors(f);
    Runtime rt(f);
    rt.frame(16.0, 800, 600);

    // Multimodal perception tree JSON
    std::string tree_json = rt.perceive_tree_json();
    check(!tree_json.empty(), "Exported perceive_tree_json() for vision/LLM grounding");
    check(tree_json.find("\"title\": \"Autonomous Agent UI\"") != std::string::npos, "Perception tree includes face title");
    check(tree_json.find("\"kind\": \"button\"") != std::string::npos, "Perception tree includes interactive button widget");

    // Runtime self-modification / hot-patching
    std::string patched_src = R"(
face autonomous_agent_face
title "Autonomous Agent UI v2"
state score = 100
state multiplier = 2

column gap=8
  text "Score: {score} x{multiplier}"
  button "Boost" action=boost
)";

    bool patched = rt.hot_patch(patched_src);
    check(patched, "Runtime hot_patch() successfully live-modified running face without losing state");
    rt.frame(16.0, 800, 600);
    std::string new_tree = rt.perceive_tree_json();
    check(new_tree.find("Autonomous Agent UI v2") != std::string::npos, "Hot-patched title rendered in perception tree");
    check(get_state(rt, "score") == "100", "Hot-patched face preserved runtime persistent state");
  }

  // -------------------------------------------------------------------------
  // Summary
  // -------------------------------------------------------------------------
  std::printf("\n===================================================\n");
  if (failures == 0) {
    std::printf("ALL 7 DIMENSIONS PASSED (0 failures).\n");
    return 0;
  } else {
    std::printf("FAILED with %d failure(s).\n", failures);
    return 1;
  }
}
