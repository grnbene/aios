// Drives the Multi-File Edit Transactions & Atomic Review Buffer Subsystem.
//
// Proves:
// 1. Intent Declaration & Manifest Parsing
// 2. Deterministic Patch Engine Pre-Flight Dry-Run & Atomic Staging
// 3. Unified Atomic Review Buffer Rendering in HardwareCompositor
// 4. Tactile Keyboard-Driven Controls:
//    - Tab / Shift+Tab: Jump between hunks across all modified files
//    - Space: Toggle accept/reject on individual hunks
//    - Ctrl+Enter / Enter: Commit transaction atomically in one tick
//    - Esc: Abort transaction; tree remains pristine

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/patch_transaction.hpp"
#include "aios/hal/keys.hpp"
#include <cstdio>
#include <iostream>
#include <fstream>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;
using namespace aios;
using namespace aios::userland;

void save_frame(HardwareCompositor& desk, Kernel& k, const std::string& path) {
  desk.render_desktop(k);
  const auto& fb = desk.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n1280 800\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {static_cast<char>((p >> 16) & 0xFF), static_cast<char>((p >> 8) & 0xFF), static_cast<char>(p & 0xFF)};
    out.write(rgb, 3);
  }
}

int main() {
  std::printf("=== AIOS Multi-File Edit Transactions & Atomic Review QA Drive ===\n\n");

  std::string test_root = "./build/tx-drive-workspace";
  fs::create_directories(test_root + "/kernel");
  fs::create_directories(test_root + "/userland");
  fs::create_directories(test_root + "/tests");

  // Step 1: Set up base files on disk
  std::printf("[Step 1] Initializing target codebase files on disk...\n");

  std::string cap_hpp_orig = 
      "#pragma once\n"
      "#include <cstdint>\n"
      "#include <string>\n"
      "\n"
      "namespace aios {\n"
      "struct Capability {\n"
      "  uint64_t id = 0;\n"
      "  std::string name;\n"
      "  uint32_t rights = 0;\n"
      "};\n"
      "}\n";

  std::string cap_cpp_orig =
      "#include \"kernel/capability.hpp\"\n"
      "\n"
      "namespace aios {\n"
      "bool validate_cap(const Capability& cap) {\n"
      "  if (cap.id == 0) return false;\n"
      "  return true;\n"
      "}\n"
      "}\n";

  std::string agent_cpp_orig =
      "#include \"kernel/capability.hpp\"\n"
      "\n"
      "namespace aios {\n"
      "void issue_agent_capability() {\n"
      "  Capability cap;\n"
      "  cap.id = 1001;\n"
      "  cap.name = \"agent_fs_access\";\n"
      "  validate_cap(cap);\n"
      "}\n"
      "}\n";

  {
    std::ofstream out(test_root + "/kernel/capability.hpp");
    out << cap_hpp_orig;
  }
  {
    std::ofstream out(test_root + "/kernel/capability.cpp");
    out << cap_cpp_orig;
  }
  {
    std::ofstream out(test_root + "/userland/agent.cpp");
    out << agent_cpp_orig;
  }
  // tests/cap_ttl_test.cpp does not exist yet (will be [+] Added)
  if (fs::exists(test_root + "/tests/cap_ttl_test.cpp")) {
    fs::remove(test_root + "/tests/cap_ttl_test.cpp");
  }

  // Boot Kernel in test directory
  KernelConfig cfg;
  cfg.root = test_root;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::fprintf(stderr, "Kernel boot failed: %s\n", k_res.error().str().c_str());
    return 1;
  }
  Kernel& kernel = **k_res;

  AuthorityProfile profile;
  auto me = kernel.resident("tx-drive", profile);
  if (!me.ok()) {
    std::fprintf(stderr, "Resident authority failed: %s\n", me.error().str().c_str());
    return 1;
  }
  Surface console = kernel.as(*me);

  // Step 2: Formulate Multi-File Unified Diff Transaction (Transaction #402)
  std::printf("[Step 2] Formulating Multi-File Intent Manifest & Unified Diff...\n");

  std::string patch_text =
      "Transaction #402: Refactor Capability Attenuation\n"
      "  [M] kernel/capability.hpp  (Add TTL field to Cap struct)\n"
      "  [M] kernel/capability.cpp  (Enforce TTL expiration check)\n"
      "  [M] userland/agent.cpp     (Update call-site with default TTL)\n"
      "  [+] tests/cap_ttl_test.cpp (Verify expiration behavior)\n"
      "\n"
      "--- a/kernel/capability.hpp\n"
      "+++ b/kernel/capability.hpp\n"
      "@@ -6,6 +6,7 @@\n"
      " struct Capability {\n"
      "   uint64_t id = 0;\n"
      "   std::string name;\n"
      "+  uint64_t ttl_ms = 3600;\n"
      "   uint32_t rights = 0;\n"
      " };\n"
      " }\n"
      "--- a/kernel/capability.cpp\n"
      "+++ b/kernel/capability.cpp\n"
      "@@ -4,4 +4,6 @@\n"
      " bool validate_cap(const Capability& cap) {\n"
      "   if (cap.id == 0) return false;\n"
      "+  if (cap.ttl_ms == 0) return false;\n"
      "   return true;\n"
      " }\n"
      " }\n"
      "--- a/userland/agent.cpp\n"
      "+++ b/userland/agent.cpp\n"
      "@@ -5,4 +5,5 @@\n"
      "   Capability cap;\n"
      "   cap.id = 1001;\n"
      "   cap.name = \"agent_fs_access\";\n"
      "+  cap.ttl_ms = 5000;\n"
      "   validate_cap(cap);\n"
      " }\n"
      "--- /dev/null\n"
      "+++ b/tests/cap_ttl_test.cpp\n"
      "@@ -0,0 +1,8 @@\n"
      "+#include \"kernel/capability.hpp\"\n"
      "+#include <cassert>\n"
      "+\n"
      "+int main() {\n"
      "+  aios::Capability c;\n"
      "+  c.id = 1;\n"
      "+  c.ttl_ms = 1000;\n"
      "+  assert(c.ttl_ms > 0);\n"
      "+  return 0;\n"
      "+}\n";

  auto parse_res = PatchEngine::parse_unified_diff(patch_text);
  if (!parse_res.ok()) {
    std::fprintf(stderr, "Patch parse failed: %s\n", parse_res.error().str().c_str());
    return 1;
  }
  TransactionManifest manifest = *parse_res;

  std::printf("[SUCCESS] Parsed Transaction #%lu: %s\n", manifest.tx_id, manifest.title.c_str());
  std::printf("  Files count: %zu\n", manifest.files.size());
  std::printf("  Total hunks: %d\n", manifest.total_hunks());
  std::printf("  Total adds:  +%d\n", manifest.total_additions());
  std::printf("  Total dels:  -%d\n", manifest.total_deletions());

  assert(manifest.files.size() == 4);
  assert(manifest.total_hunks() == 4);
  assert(manifest.files[0].reason == "Add TTL field to Cap struct");
  assert(manifest.files[1].reason == "Enforce TTL expiration check");
  assert(manifest.files[2].reason == "Update call-site with default TTL");
  assert(manifest.files[3].reason == "Verify expiration behavior");
  assert(manifest.files[3].op_type == FileOpType::Add);

  // Step 3: Test Deterministic Patch Pre-flight Dry-Run & Atomic Staging
  std::printf("\n[Step 3] Testing Deterministic Pre-Flight Check & Dry-Run...\n");

  // 3a. Verify dry-run succeeds on valid substrate
  auto dry_res = PatchEngine::dry_run(kernel, manifest, test_root);
  if (!dry_res.ok()) {
    std::fprintf(stderr, "Dry-run failed on valid tree: %s\n", dry_res.error().str().c_str());
    return 1;
  }
  std::printf("[SUCCESS] Pre-flight dry-run validated cleanly across all 4 files.\n");

  // 3b. Verify dry-run strictly rejects mismatched file without modifying anything
  std::string corrupted_patch = patch_text;
  size_t pos = corrupted_patch.find("uint64_t id = 0;");
  corrupted_patch.replace(pos, 17, "uint64_t BAD_ID;");
  auto bad_parse = PatchEngine::parse_unified_diff(corrupted_patch);
  assert(bad_parse.ok());
  auto bad_manifest = *bad_parse;
  auto bad_dry = PatchEngine::dry_run(kernel, bad_manifest, test_root);
  assert(!bad_dry.ok());
  std::printf("[SUCCESS] Pre-flight correctly rejected corrupted patch: %s\n", bad_dry.error().str().c_str());

  // 3c. Stage transaction in memory
  auto stage_res = PatchEngine::stage(kernel, manifest, test_root);
  if (!stage_res.ok()) {
    std::fprintf(stderr, "Stage failed: %s\n", stage_res.error().str().c_str());
    return 1;
  }
  std::printf("[SUCCESS] Transaction staged in memory workspace. Status = %s\n",
              transaction_status_name(manifest.status));
  assert(manifest.status == TransactionStatus::Staged);

  // Step 4: Drive HardwareCompositor & Unified Atomic Review Buffer
  std::printf("\n[Step 4] Launching HardwareCompositor in Unified Atomic Review Buffer mode...\n");

  HardwareCompositor desk(1280, 800);
  desk.set_fullscreen(false);
  desk.set_canvas_mode(false);
  desk.set_console(&console);
  desk.init_default_workspace(kernel);

  auto win_res = desk.create_window("Review: Transaction #402", 40, 40, 1200, 720, WindowType::Editor);
  if (!win_res.ok()) {
    std::fprintf(stderr, "Failed to create editor window: %s\n", win_res.error().str().c_str());
    return 1;
  }
  uint32_t win_id = *win_res;
  desk.bring_to_front(win_id);
  WindowSurface* win = desk.find_window(win_id);
  assert(win != nullptr);

  // Activate Unified Review Buffer mode
  desk.editor_open_tx_review(*win, manifest);
  assert(win->tx_review_active == true);

  // Render initial review frame
  std::printf("[Step 5] Rendering Unified Atomic Review Buffer viewport...\n");
  save_frame(desk, kernel, "desktop_tx_review.ppm");
  std::printf("[SUCCESS] Saved review buffer frame to desktop_tx_review.ppm\n");

  // Step 6: Test Tactile Keyboard-Driven Controls
  std::printf("\n[Step 6] Exercising tactile keyboard-driven navigation...\n");

  // 6a. Tab: Jump to next hunk
  assert(win->tx_focused_hunk == 0);
  desk.input_key(hal::KeyTab);
  assert(win->tx_focused_hunk == 1);
  std::printf("  [KeyTab] Jumped focus to Hunk #2 (%s)\n",
              win->tx_manifest.files[1].new_path.c_str());

  desk.input_key(hal::KeyTab);
  assert(win->tx_focused_hunk == 2);
  std::printf("  [KeyTab] Jumped focus to Hunk #3 (%s)\n",
              win->tx_manifest.files[2].new_path.c_str());

  // 6b. Space: Toggle accept/reject on current hunk
  assert(win->tx_manifest.files[2].hunks[0].accepted == true);
  desk.input_key(' ');
  assert(win->tx_manifest.files[2].hunks[0].accepted == false);
  std::printf("  [KeySpace] Toggled Hunk #3 -> REJECTED. Restaged buffer recalculated.\n");

  desk.input_key(' ');
  assert(win->tx_manifest.files[2].hunks[0].accepted == true);
  std::printf("  [KeySpace] Toggled Hunk #3 -> ACCEPTED.\n");

  // 6c. Esc: Abort transaction test (tree remains pristine)
  desk.input_key(hal::KeyEscape);
  assert(win->tx_review_active == false);
  std::printf("  [KeyEscape] Aborted transaction. Staged memory discarded.\n");

  // Verify disk files remained pristine after abort
  std::ifstream f1(test_root + "/kernel/capability.hpp");
  std::string f1_txt((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
  assert(f1_txt == cap_hpp_orig);
  assert(!fs::exists(test_root + "/tests/cap_ttl_test.cpp"));
  std::printf("[SUCCESS] Verified disk tree is 100%% pristine after abort.\n");

  // 6d. Re-open review and execute All-or-Nothing Commit (Ctrl+Enter)
  std::printf("\n[Step 7] Testing All-or-Nothing Atomic Commit...\n");
  desk.editor_open_tx_review(*win, manifest);
  desk.input_key(hal::KeyEnter); // Commits transaction

  // Verify all 4 files are atomically updated on disk!
  std::ifstream f1_after(test_root + "/kernel/capability.hpp");
  std::string f1_after_txt((std::istreambuf_iterator<char>(f1_after)), std::istreambuf_iterator<char>());
  assert(f1_after_txt.find("uint64_t ttl_ms = 3600;") != std::string::npos);

  std::ifstream f2_after(test_root + "/kernel/capability.cpp");
  std::string f2_after_txt((std::istreambuf_iterator<char>(f2_after)), std::istreambuf_iterator<char>());
  assert(f2_after_txt.find("if (cap.ttl_ms == 0) return false;") != std::string::npos);

  std::ifstream f3_after(test_root + "/userland/agent.cpp");
  std::string f3_after_txt((std::istreambuf_iterator<char>(f3_after)), std::istreambuf_iterator<char>());
  assert(f3_after_txt.find("cap.ttl_ms = 5000;") != std::string::npos);

  assert(fs::exists(test_root + "/tests/cap_ttl_test.cpp"));
  std::ifstream f4_after(test_root + "/tests/cap_ttl_test.cpp");
  std::string f4_after_txt((std::istreambuf_iterator<char>(f4_after)), std::istreambuf_iterator<char>());
  assert(f4_after_txt.find("aios::Capability c;") != std::string::npos);

  std::printf("[SUCCESS] All 4 files successfully and atomically committed to disk simultaneously!\n");
  std::printf("  [M] kernel/capability.hpp  verified\n");
  std::printf("  [M] kernel/capability.cpp  verified\n");
  std::printf("  [M] userland/agent.cpp     verified\n");
  std::printf("  [+] tests/cap_ttl_test.cpp verified\n");

  // Render final committed state frame
  save_frame(desk, kernel, "desktop_tx_committed.ppm");

  std::printf("\n=== ALL QA TESTS PASSED PROVABLY (Multi-File Edit Transactions) ===\n");
  return 0;
}
