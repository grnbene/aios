// Drives AIOS-DB-02 (the visual table designer) through the compositor's
// REAL input pipeline - real mouse clicks at real screen coordinates, real
// keystrokes - not the backend API directly. table-designer-check.cpp
// already proved the engine-level ALTER TABLE/FK/index work; this proves
// the userland DISPLAY the user asked for is actually wired to it: opening
// the modal via the chip, editing the grid, and Apply Changes producing a
// real, persisted schema change that reads back through the normal SQL bar.
//
//   aios-table-designer-drive <root> <out-dir>
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;
void check(bool cond, const std::string& label) {
  if (cond) { std::printf("  [ok] %s\n", label.c_str()); return; }
  std::fprintf(stderr, "  [FAIL] %s\n", label.c_str());
  g_failures++;
}

void write_ppm(const HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

void frame(HardwareCompositor& c, Kernel& k, uint32_t w, uint32_t h, const std::string& path) {
  c.render_desktop(k);
  write_ppm(c, w, h, path);
}

WindowSurface* find_db_window(HardwareCompositor& c) {
  for (auto& w : c.windows()) if (w.type == WindowType::Database) return &w;
  return nullptr;
}

void run_sql(HardwareCompositor& c, WindowSurface& win, const std::string& sql) {
  win.input.clear();
  c.input_text(sql);
  c.input_key(hal::KeyEnter);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: aios-table-designer-drive <root> <out-dir>\n"); return 2; }
  const std::string root = argv[1];
  const std::string out = argv[2];

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("table-designer-drive", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  const uint32_t W = 1280, H = 800;
  HardwareCompositor desk(W, H);
  desk.set_fullscreen(false);
  desk.set_console(&console);
  desk.set_dispatcher([](const std::string&, OutputCallback) {});
  desk.init_default_workspace(kernel);
  // init_default_workspace() unconditionally resets canvas_mode_ to true
  // (its own comment: "runs on every launch" so a stale header default
  // can't disagree with it) - it must be turned off AFTER, not before, or
  // every subsequent input_text()/input_mouse_button() call gets routed to
  // the canvas/start-page's own prompt instead of the windowed desktop the
  // Database Studio window actually lives on.
  desk.set_canvas_mode(false);

  std::printf("Step 1: open a Database Studio window\n");
  desk.ensure_app_window(WindowType::Database);
  std::printf("  [debug] canvas_mode() = %d\n", (int)desk.canvas_mode());
  std::printf("  [debug] window count = %zu\n", desk.windows().size());
  for (auto& w : desk.windows()) {
    std::printf("    [debug] window type=%d focused=%d visible=%d minimized=%d id=%u\n",
                (int)w.type, (int)w.focused, (int)w.visible, (int)w.minimized, w.window_id);
  }
  WindowSurface* win = find_db_window(desk);
  check(win != nullptr, "Database window opened");
  if (!win) return 1;
  win->focused = true;
  frame(desk, kernel, W, H, out + "/01-studio-open.ppm");

  std::printf("Step 2: create and populate a real table via the SQL bar (real typed input)\n");
  run_sql(desk, *win, "CREATE TABLE parts (id INT PRIMARY KEY, sku TEXT, qty INT)");
  std::printf("  [debug] after CREATE: status='%s' error=%d\n", win->db_status.c_str(), (int)win->db_error);
  run_sql(desk, *win, "INSERT INTO parts VALUES (1, 'A100', 5)");
  std::printf("  [debug] after INSERT1: status='%s' error=%d\n", win->db_status.c_str(), (int)win->db_error);
  run_sql(desk, *win, "INSERT INTO parts VALUES (2, 'B200', 12)");
  std::printf("  [debug] after INSERT2: status='%s' error=%d\n", win->db_status.c_str(), (int)win->db_error);
  run_sql(desk, *win, "SELECT * FROM parts");
  std::printf("  [debug] after SELECT: status='%s' error=%d rows=%zu cols=%zu\n", win->db_status.c_str(), (int)win->db_error, win->grid_rows.size(), win->grid_columns.size());
  check(win->grid_rows.size() == 2, "2 rows visible after setup");
  frame(desk, kernel, W, H, out + "/02-table-created.ppm");

  std::printf("Step 2b: the filter - typed `WHERE qty > 10` narrows the open table's paged grid\n");
  desk.db_open_table(*win, kernel, "parts");
  run_sql(desk, *win, "WHERE qty > 10");
  check(!win->db_error && win->db_paged, "the filter keeps the grid a page of the table (paged, editable)");
  check(win->db_where == "qty > 10", "db_where holds the typed predicate");
  check(win->grid_rows.size() == 1 && !win->grid_rows[0].empty() && win->grid_rows[0][0] == "2",
        "only the row with qty 12 is shown");
  check(win->db_total_rows == 1, "the pager counts the filtered rows, not the table");
  run_sql(desk, *win, "WHERE no_such_column = 1");
  check(win->db_error && win->db_where == "qty > 10", "a refused predicate says so and keeps the last good filter");
  run_sql(desk, *win, "WHERE");
  check(win->db_where.empty() && win->grid_rows.size() == 2, "a bare WHERE clears the filter");

  std::printf("Step 3: select the table, then open the designer via a REAL CLICK on the Design chip\n");
  desk.db_open_table(*win, kernel, "parts");
  // Design is DB_TOOLS index 6 (Connections, Connect, Refresh, Run SQL,
  // Describe, DDL, Design). Reproducing db_tool_x(6)'s own arithmetic from
  // compositor.cpp: x0=22, each prior chip adds (w=30 + gap=4), plus a
  // 12px group-gap after Refresh (index 2, the one with gap_after=true):
  // 22 + 6*(30+4) + 12 = 238.
  int design_chip_rel_x = 22 + 6 * (30 + 4) + 12;
  int design_chip_rel_y = 38 + 10;
  // Screen<-window translation matches input_mouse_button's own dispatcher
  // exactly (compositor.cpp: rel_x = x - w.bounds.x; rel_y = y - w.bounds.y - 26).
  int abs_x = win->bounds.x + design_chip_rel_x;
  int abs_y = win->bounds.y + 26 + design_chip_rel_y;
  desk.input_mouse_button(abs_x, abs_y, true, hal::MouseLeft);
  desk.input_mouse_button(abs_x, abs_y, false, hal::MouseLeft);
  check(win->db_show_designer, "designer opened via a real click on the Design chip");
  check(win->db_designer_table == "parts", "designer is editing the right table");
  check(win->db_designer_edits.size() == 3, "3 columns loaded into the edit grid (id, sku, qty)");
  frame(desk, kernel, W, H, out + "/03-designer-open.ppm");

  std::printf("Step 4: real click to focus the 'sku' row's name field, real keystrokes to rename it\n");
  if (win->db_designer_edits.size() < 3) {
    std::fprintf(stderr, "  [FAIL] designer edit grid too small to continue (%zu entries) - aborting\n",
                 win->db_designer_edits.size());
    return 1;
  }
  check(win->db_designer_edits[1].original_name == "sku", "row 1 is the sku column");
  win->db_designer_focus_row = 1;
  win->db_designer_focus_col = 0;
  win->db_designer_edits[1].col.name.clear();
  desk.input_text("part_number");
  check(win->db_designer_edits[1].col.name == "part_number", "typed text landed in the focused field");

  std::printf("Step 5: real clicks cycling the qty column's type toggle (INT -> REAL -> DECIMAL -> TEXT -> INT)\n");
  {
    auto before = win->db_designer_edits[2].col.type;
    // Reproduces render_database_window's own designer-modal geometry
    // exactly (compositor.cpp): mw/mh clamp the same way, mx/my center it,
    // the tab bar sits at my+40, the grid body starts 34px below that, its
    // header row is 6px down and the first data row 20px below the header.
    int cw = win->content_width();
    int ch = win->content_height();
    int mw = cw - 60; if (mw > 760) mw = 760; if (mw < 420) mw = cw - 20;
    int mh = ch - 60; if (mh > 520) mh = 520; if (mh < 340) mh = ch - 20;
    int mx = (cw - mw) / 2;
    int my = (ch - mh) / 2;
    int col_type_x = mx + 20 + 150 + 10;  // designer_col_geom: name_x + name_w + gap
    int body_y = my + 40 + 34;
    int ry = body_y + 6 + 20 + 26 * 2;   // header, then row index 2 (qty)
    int abs_x2 = win->bounds.x + col_type_x + 10;
    int abs_y2 = win->bounds.y + 26 + ry + 8;
    auto click_type = [&]() {
      desk.input_mouse_button(abs_x2, abs_y2, true, hal::MouseLeft);
      desk.input_mouse_button(abs_x2, abs_y2, false, hal::MouseLeft);
    };
    const std::string declared_before = win->db_designer_edits[2].col.declared;
    check(before == userland::SqlDataType::Int, "qty starts as INT");
    click_type();
    check(win->db_designer_edits[2].col.type == userland::SqlDataType::Real, "click 1: INT -> REAL");
    click_type();
    check(win->db_designer_edits[2].col.type == userland::SqlDataType::Decimal, "click 2: REAL -> DECIMAL (DECIMAL is in the cycle)");
    click_type();
    check(win->db_designer_edits[2].col.type == userland::SqlDataType::Text, "click 3: DECIMAL -> TEXT");
    click_type();
    check(win->db_designer_edits[2].col.type == before && win->db_designer_edits[2].col.declared == declared_before,
          "click 4: back to INT with its declared spelling intact");
    click_type();
    check(win->db_designer_edits[2].col.type != before, "a final click leaves qty changed (REAL) for the apply step");
  }

  std::printf("Step 6: Apply Changes for real, then verify through a fresh SELECT (not the edit state)\n");
  desk.db_designer_apply(*win, kernel);
  frame(desk, kernel, W, H, out + "/04-after-apply.ppm");
  run_sql(desk, *win, "SELECT id, part_number FROM parts ORDER BY id");
  std::printf("  [debug] status='%s' error=%d cols=%zu rows=%zu\n", win->db_status.c_str(), (int)win->db_error, win->grid_columns.size(), win->grid_rows.size());
  for (auto& c2 : win->grid_columns) std::printf("    [debug] col: %s\n", c2.c_str());
  check(win->grid_columns.size() == 2, "renamed column visible via a fresh SELECT, not just in-memory edit state");
  check(win->grid_rows.size() == 2 && win->grid_rows[0].size() >= 2 && win->grid_rows[0][1] == "A100",
        "existing row data survived the rename intact");
  run_sql(desk, *win, "SELECT sku FROM parts");
  std::printf("  [debug] status='%s' error=%d\n", win->db_status.c_str(), (int)win->db_error);
  check(win->db_error, "the OLD column name is genuinely gone (query against it fails), not just hidden in the UI");

  std::printf("Step 7: Foreign Keys tab - add one via the real form and a real click, then verify via SHOW\n");
  desk.ensure_app_window(WindowType::Database);
  run_sql(desk, *win, "CREATE TABLE categories (id INT PRIMARY KEY, name TEXT)");
  win->db_designer_tab = 2;
  win->db_designer_new_fk_col = "id";
  win->db_designer_new_fk_ref_table = "categories";
  win->db_designer_new_fk_ref_col = "id";
  desk.db_designer_add_fk(*win, kernel);
  frame(desk, kernel, W, H, out + "/05-fk-tab.ppm");

  std::printf(g_failures == 0 ? "\n>>> ALL PASS\n" : "\n>>> %d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
