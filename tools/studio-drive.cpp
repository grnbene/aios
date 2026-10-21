// Drives the native Face Applications Desk and Universal In-Desktop
// Behavioral Studio & Inspector through the compositor's input pipeline,
// generating visual frames and asserting state transitions and latency.
//
//   aios-studio-drive <root> <out-dir>

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/face.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace aios;

namespace {

struct StepReport {
  int step_number;
  std::string description;
  std::string file_path;
  double render_time_ms;
  bool verified;
};

void write_ppm(const userland::HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

double record_frame(userland::HardwareCompositor& c, Kernel& k, uint32_t w, uint32_t h, const std::string& path) {
  auto t0 = std::chrono::high_resolution_clock::now();
  c.render_desktop(k);
  auto t1 = std::chrono::high_resolution_clock::now();
  write_ppm(c, w, h, path);
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aios-studio-drive <root> <out-dir>\n");
    return 2;
  }
  const std::string root = argv[1];
  const std::string out = argv[2];

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) {
    std::fprintf(stderr, "Kernel::boot failed: %s\n", k.error().str().c_str());
    return 1;
  }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("studio-drive", profile);
  if (!me.ok()) {
    std::fprintf(stderr, "resident error: %s\n", me.error().str().c_str());
    return 1;
  }
  Surface console = kernel.as(*me);

  const uint32_t W = 1280;
  const uint32_t H = 800;
  userland::HardwareCompositor comp(W, H);
  comp.set_console(&console);
  comp.init_default_workspace(kernel);

  std::vector<StepReport> reports;
  int failures = 0;
  // Every step states what must be true afterwards and checks it. This used to
  // record each step as verified=true and close with "All 9 Verification Steps
  // Passed" whatever happened -- including over a pane that said
  // "No face app is running".
  auto verdict = [&](int n, const std::string& desc, const std::string& file, double ms, bool ok, const std::string& why) {
    reports.push_back({n, desc, file, ms, ok});
    if (!ok) ++failures;
    std::printf("%s (%.2f ms)%s%s\n", ok ? "[PASS]" : "[FAIL]", ms, why.empty() ? "" : " -- ", why.c_str());
  };
  auto find_type = [&](userland::WindowType t) -> userland::WindowSurface* {
    for (auto& w : comp.windows()) if (w.type == t) return &w;
    return nullptr;
  };
  // A hash of the framebuffer, so a tab switch that paints nothing new is caught.
  auto frame_hash = [&]() {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t p : comp.framebuffer()) { h ^= p; h *= 1099511628211ull; }
    return h;
  };
  // The face app is running and is the one asked for.
  auto app_check = [&](int index, std::string& why) {
    auto* w = find_type(userland::WindowType::FaceApp);
    if (!w) { why = "no Apps window"; return false; }
    if (!w->face_app_runtime) { why = "no face running: " + w->status_text; return false; }
    if (w->face_app_selected_index != index) { why = "selected index " + std::to_string(w->face_app_selected_index); return false; }
    if (index < (int)w->face_app_list.size()) {
      std::string want = w->face_app_list[(size_t)index];
      want = want.substr(want.find_last_of('/') + 1);
      if (w->face_app_path.size() < want.size() ||
          w->face_app_path.compare(w->face_app_path.size() - want.size(), want.size(), want) != 0) {
        why = "running " + w->face_app_path + ", asked for " + want;
        return false;
      }
    }
    if (w->face_app_title.empty()) { why = "the face has no title"; return false; }
    why = w->face_app_title + " from " + w->face_app_path;
    return true;
  };

  std::printf("======================================================================\n");
  std::printf("AIOS Face Applications Desk & Universal Behavioral Studio Drive\n");
  std::printf("Target Resolution: %ux%u • Engine: Native C++ Hardware Compositor\n", W, H);
  std::printf("======================================================================\n");

  const char* app_names[] = {"Enterprise Order Desk", "Task Tracker", "System Monitor", "Notes"};
  const char* app_files[] = {"1-face-order-desk.ppm", "2-face-task-tracker.ppm", "3-face-sys-monitor.ppm", "4-face-notes.ppm"};
  std::string prev_title;
  for (int i = 0; i < 4; ++i) {
    std::printf("[STEP %d] Apps: %s... ", i + 1, app_names[i]);
    if (i == 0) {
      comp.input_key(hal::KeyF10);
    } else if (auto* w = find_type(userland::WindowType::FaceApp)) {
      comp.face_app_switch(*w, i, kernel);
    }
    double ms = record_frame(comp, kernel, W, H, out + "/" + app_files[i]);
    std::string why;
    bool ok = app_check(i, why);
    if (ok && i > 0) {
      auto* w = find_type(userland::WindowType::FaceApp);
      if (w->face_app_title == prev_title) { ok = false; why = "title did not change from " + prev_title; }
    }
    if (auto* w = find_type(userland::WindowType::FaceApp)) prev_title = w->face_app_title;
    verdict(i + 1, std::string("Apps: ") + app_names[i], out + "/" + app_files[i], ms, ok, why);
  }

  // Studio tabs: each must exist, be the active tab, and paint something the
  // previous tab did not.
  const char* tab_names[] = {"Tree & Bounds", "State & Store", "Drive & Test", "Telemetry", "Source & Reload"};
  const char* tab_files[] = {"5-studio-tree-bounds.ppm", "6-studio-state-store.ppm", "7-studio-drive-tests.ppm",
                             "8-studio-telemetry-fps.ppm", "9-studio-source-reload.ppm"};
  uint64_t prev_hash = 0;
  for (int t = 0; t < 5; ++t) {
    std::printf("[STEP %d] Studio tab %d: %s... ", t + 5, t, tab_names[t]);
    if (t == 0) comp.input_key(hal::KeyF11);
    auto* insp = find_type(userland::WindowType::Inspector);
    std::string why;
    bool ok = insp != nullptr;
    if (!ok) why = "F11 opened no Studio window";
    if (insp) {
      insp->studio_active_tab = t;
      if (t == 2) comp.inspector_run_all_drive_steps(*insp);
    }
    double ms = record_frame(comp, kernel, W, H, out + "/" + tab_files[t]);
    if (ok) {
      insp = find_type(userland::WindowType::Inspector);
      const uint64_t hsh = frame_hash();
      if (t > 0 && hsh == prev_hash) { ok = false; why = "the tab painted exactly the previous frame"; }
      prev_hash = hsh;
      if (ok && t == 2) {
        // The drive runner reports per step; any FAIL fails this step, and it
        // must have run every step it holds.
        size_t pass = 0, fail = 0;
        for (const auto& r : insp->studio_drive_results) {
          if (r.find("[FAIL]") != std::string::npos) { ++fail; if (why.empty()) why = r; }
          if (r.find("[PASS]") != std::string::npos) ++pass;
        }
        if (insp->studio_drive_steps.empty()) { ok = false; why = "the runner holds no steps"; }
        else if (fail) ok = false;
        else if (pass < insp->studio_drive_steps.size()) {
          ok = false;
          why = std::to_string(pass) + " of " + std::to_string(insp->studio_drive_steps.size()) + " steps reported";
        } else {
          why = std::to_string(pass) + " runner steps passed";
        }
      }
    }
    verdict(t + 5, std::string("Studio: ") + tab_names[t], out + "/" + tab_files[t], ms, ok, why);
  }

  std::printf("----------------------------------------------------------------------\n");
  std::printf("Total Frames Rendered: %zu • %zu passed, %d failed\n", reports.size(),
              reports.size() - (size_t)failures, failures);
  std::printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
  std::printf("======================================================================\n");
  return failures ? 1 : 0;
}
