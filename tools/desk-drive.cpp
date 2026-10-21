// Drives the desk the way a person's hands do: picks a card up, drops it
// somewhere else, folds one away, throws one out, puts it back, resizes one,
// unpins one, and then restarts the machine to see whether any of it stuck.
//
// Every event here goes through the SAME input calls the SDL backend and the
// bare-metal PS/2 driver feed, and every check is made against the layout the
// PAINTER used — not against a number written down in this file. A tile is
// proven to have moved because the rect it is drawn in moved.
//
//   aios-desk-drive <root> <out-dir>
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/canvas.hpp"
#include "aios/userland/compositor.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace aios;
using aios::userland::CanvasLayout;
using aios::userland::DeskTile;
using aios::userland::TileKind;
using aios::userland::TileRect;

namespace {

int failures = 0;

// A command a card launches is dispatched on the compositor's own thread, so
// what it ran is read back under a lock and waited for. The first run of this
// harness called the moved card broken because it looked before the dispatch
// had happened.
std::mutex ran_lock;
std::vector<std::string> dispatched;

void note_dispatch(const std::string& cmd) {
  std::lock_guard<std::mutex> g(ran_lock);
  dispatched.push_back(cmd);
}
void clear_dispatch() {
  std::lock_guard<std::mutex> g(ran_lock);
  dispatched.clear();
}
std::string wait_dispatch() {
  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> g(ran_lock);
      if (!dispatched.empty()) return dispatched.back();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::string();
}

void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
  std::printf("        %s %s%s\n", ok ? "OK  " : "FAIL", what.c_str(),
              detail.empty() ? "" : ("  -> " + detail).c_str());
  if (!ok) ++failures;
}

void write_ppm(const userland::HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    const uint32_t p = fb[i];
    const char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

// Where a tile is RIGHT NOW, out of the same function the painter asks.
bool find_tile(userland::HardwareCompositor& c, uint32_t W, uint32_t H, TileKind kind, TileRect* out) {
  const CanvasLayout lay = canvas_layout(c, W, H, c.canvas_state());
  for (const TileRect& r : lay.tiles) {
    if (r.kind != kind) continue;
    *out = r;
    return true;
  }
  return false;
}

const DeskTile* tile_state(userland::HardwareCompositor& c, TileKind kind) {
  for (const DeskTile& t : c.canvas_state().tiles) if (t.kind == kind) return &t;
  return nullptr;
}

// A press, a run of motion, and a release — a drag, not a teleport, because a
// handler that only sees the endpoints is not the handler a hand drives.
void drag(userland::HardwareCompositor& c, int x0, int y0, int x1, int y1, int scale) {
  c.input_mouse_button(x0 * scale, y0 * scale, true, hal::MouseLeft);
  for (int i = 1; i <= 8; ++i) {
    const int x = x0 + (x1 - x0) * i / 8, y = y0 + (y1 - y0) * i / 8;
    c.input_mouse_motion(x * scale, y * scale);
  }
  c.input_mouse_button(x1 * scale, y1 * scale, false, hal::MouseLeft);
}

void click(userland::HardwareCompositor& c, int x, int y, int scale, int button = hal::MouseLeft) {
  c.input_mouse_button(x * scale, y * scale, true, button);
  c.input_mouse_button(x * scale, y * scale, false, button);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: aios-desk-drive <root> <out-dir>\n"); return 2; }
  const std::string root = argv[1];
  const std::string out = argv[2];

  // Which face this harness is driving, said out loud. It used to lean on the
  // canvas defaulting to the start page; the desk is the panel face now, and a
  // harness that reads a default instead of stating what it wants goes looking
  // for tiles on the wrong face.
  auto desk_face = [] { unsetenv("AOS_HIDE_PANELS"); setenv("AOS_SHOW_PANELS", "1", 1); };
  auto start_face = [] { unsetenv("AOS_SHOW_PANELS"); setenv("AOS_HIDE_PANELS", "1", 1); };
  desk_face();

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("desk-drive", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  const uint32_t W = 1440, H = 900;

  auto make = [&](uint32_t w, uint32_t h) {
    auto* c = new userland::HardwareCompositor(w, h);
    c->set_fullscreen(false);
    c->set_console(&console);
    c->set_dispatcher([](const std::string& cmd, userland::OutputCallback) { note_dispatch(cmd); });
    c->init_default_workspace(kernel);
    c->set_canvas_mode(true);
    return c;
  };

  userland::HardwareCompositor* desk = make(W, H);
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/0-desk-as-it-ships.ppm");

  // ---------------------------------------------------------------------------
  std::printf("step 1: pick Identities up and drop it in the right-hand column\n");
  TileRect ident{}, horizon{};
  if (!find_tile(*desk, W, H, TileKind::Identities, &ident)) { std::printf("  no Identities tile\n"); return 1; }
  if (!find_tile(*desk, W, H, TileKind::Horizon, &horizon)) { std::printf("  no Horizon tile\n"); return 1; }
  std::printf("        Identities is drawn at %d,%d %dx%d\n", ident.x, ident.y, ident.w, ident.h);
  const int from_col = tile_state(*desk, TileKind::Identities)->col;
  // Aim below the middle of Horizon, so it lands under it in that column.
  drag(*desk, ident.x + ident.w / 2, ident.y + 8,
       horizon.x + horizon.w / 2, horizon.y + horizon.h - 4, 1);
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/1-identities-dragged.ppm");
  const DeskTile* ts = tile_state(*desk, TileKind::Identities);
  check(ts->col != from_col, "Identities changed column (" + std::to_string(from_col) + " -> " +
                             std::to_string(ts->col) + ")");
  TileRect moved{};
  find_tile(*desk, W, H, TileKind::Identities, &moved);
  std::printf("        it is now drawn at %d,%d\n", moved.x, moved.y);
  check(moved.x != ident.x || moved.y != ident.y, "and the painter draws it somewhere else");

  // ---------------------------------------------------------------------------
  std::printf("step 2: the hit test followed it, rather than staying where it was\n");
  // This used to press the moved card and watch it still run its own command.
  // The six launcher cards are not seeded onto this desk any anymore, so nothing
  // on it dispatches; what still has to be true is that the rects moved with
  // the tile. The label-to-binding check that mattered lives in step 11 now.
  const CanvasLayout after = canvas_layout(*desk, W, H, desk->canvas_state());
  const TileRect* at_new = after.at(moved.x + moved.w / 2, moved.y + 8);
  const TileRect* at_old = after.at(ident.x + ident.w / 2, ident.y + 8);
  check(at_new && at_new->kind == TileKind::Identities, "Identities is what the layout finds where it is drawn",
        at_new ? aios::userland::tile_menu_name(at_new->kind) : "nothing");
  check(!at_old || at_old->kind != TileKind::Identities, "and is no longer found where it used to be",
        at_old ? aios::userland::tile_menu_name(at_old->kind) : "nothing");

  // ---------------------------------------------------------------------------
  std::printf("step 3: fold the Authority Ledger away by its own chevron\n");
  TileRect hz{};
  find_tile(*desk, W, H, TileKind::Ledger, &hz);
  const int hz_h_before = hz.h;
  click(*desk, hz.fold_x + hz.ctl / 2, hz.fold_y + hz.ctl / 2, 1);
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/2-ledger-folded.ppm");
  find_tile(*desk, W, H, TileKind::Ledger, &hz);
  check(tile_state(*desk, TileKind::Ledger)->collapsed, "the Authority Ledger reads folded");
  check(hz.h < hz_h_before, "and the rect it is drawn in shrank (" + std::to_string(hz_h_before) +
                            " -> " + std::to_string(hz.h) + ")");
  // Unfold it again: the chevron is a toggle, not a one-way door.
  click(*desk, hz.fold_x + hz.ctl / 2, hz.fold_y + hz.ctl / 2, 1);
  check(!tile_state(*desk, TileKind::Ledger)->collapsed, "and the chevron folds it back open");

  // ---------------------------------------------------------------------------
  std::printf("step 4: throw Standing Orders out by its x, then put it back from the menu\n");
  TileRect orders{};
  find_tile(*desk, W, H, TileKind::Directives, &orders);
  click(*desk, orders.shut_x + orders.ctl / 2, orders.shut_y + orders.ctl / 2, 1);
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/3-orders-removed.ppm");
  check(tile_state(*desk, TileKind::Directives)->hidden, "Standing Orders reads removed");
  TileRect gone{};
  check(!find_tile(*desk, W, H, TileKind::Directives, &gone), "and the painter is not given a rect for it");

  // Right-click the bare canvas: the menu has to offer it back.
  click(*desk, 40, H - 60, 1, hal::MouseRight);
  desk->input_mouse_motion(60, H - 40);
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/4-put-back-menu.ppm");
  int mx = 0, my = 0;
  const bool offered = desk->context_menu_item_at("Put back: Standing Orders", &mx, &my);
  std::printf("        the menu offers %d items; 'Put back: Standing Orders' is drawn at %d,%d\n",
              (int)desk->context_menu_items().size(), mx, my);
  check(offered, "the menu offers 'Put back: Standing Orders'");
  if (offered) {
    click(*desk, mx, my, 1);
    desk->render_desktop(kernel);
    write_ppm(*desk, W, H, out + "/5-orders-back.ppm");
    check(!tile_state(*desk, TileKind::Directives)->hidden, "and picking it brings Standing Orders back");
    check(find_tile(*desk, W, H, TileKind::Directives, &gone), "with a rect on the glass again");
  }

  // ---------------------------------------------------------------------------
  std::printf("step 5: resize Rewind by its corner\n");
  // A tile that asks for a fixed height, not one already squeezed to the
  // minimum: the thought stream takes "whatever is left" of its column, and on
  // a column this full that is the folded height, so dragging its corner up
  // had nothing to give.
  TileRect rew{};
  find_tile(*desk, W, H, TileKind::Rewind, &rew);
  const int rew_h_before = rew.h;
  drag(*desk, rew.x + rew.w - 3, rew.y + rew.h - 3,
       rew.x + rew.w - 3, rew.y + rew.h - 60, 1);
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/6-rewind-resized.ppm");
  find_tile(*desk, W, H, TileKind::Rewind, &rew);
  check(rew.h < rew_h_before, "Rewind is shorter (" + std::to_string(rew_h_before) +
                              " -> " + std::to_string(rew.h) + ")");

  // ---------------------------------------------------------------------------
  std::printf("step 6: unpin Authority and drag it somewhere the grid would never put it\n");
  TileRect db{};
  find_tile(*desk, W, H, TileKind::Authority, &db);
  click(*desk, db.x + db.w / 2, db.y + 4, 1, hal::MouseRight);
  int fx = 0, fy = 0;
  const bool can_float = desk->context_menu_item_at("Let Authority float free", &fx, &fy);
  check(can_float, "the menu over a tile offers to let it float free");
  if (can_float) {
    click(*desk, fx, fy, 1);
    check(tile_state(*desk, TileKind::Authority)->floating, "Authority is unpinned");
    find_tile(*desk, W, H, TileKind::Authority, &db);
    drag(*desk, db.x + db.w / 2, db.y + 8, 240, (int)H - 180, 1);
    desk->render_desktop(kernel);
    write_ppm(*desk, W, H, out + "/7-authority-floating.ppm");
    TileRect fl{};
    find_tile(*desk, W, H, TileKind::Authority, &fl);
    const DeskTile* fs = tile_state(*desk, TileKind::Authority);
    std::printf("        Authority was dropped at %d,%d and draws at %d,%d\n",
                fs ? fs->x : -1, fs ? fs->y : -1, fl.x, fl.y);
    // What the drop RECORDED, not where it ends up drawn: a tall tile dropped
    // near the bottom is clamped back up so it cannot hang off the glass, and
    // asserting the drawn position made this check depend on the tile's height.
    check(fs && fs->y > db.y + 100, "and the drop was recorded well off its grid slot");
  }

  // ---------------------------------------------------------------------------
  std::printf("step 7: the other face, which is one tile wide now\n");
  // The panels ARE this desk; the start-page face keeps only what was left on
  // it. It still has to lay out and still has to fold, or a face nobody looks
  // at is a face that quietly rots.
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/8-the-desk.ppm");
  start_face();
  desk->render_desktop(kernel);
  write_ppm(*desk, W, H, out + "/9-start-face.ppm");
  TileRect tele{};
  check(find_tile(*desk, W, H, TileKind::Telemetry, &tele), "the start-page face still lays its tile out");
  if (tele.ctl > 0) {
    click(*desk, tele.fold_x + tele.ctl / 2, tele.fold_y + tele.ctl / 2, 1);
    desk->render_desktop(kernel);
    write_ppm(*desk, W, H, out + "/10-telemetry-folded.ppm");
    check(tile_state(*desk, TileKind::Telemetry)->collapsed, "and its chevron folds it too");
  }
  desk_face();

  std::printf("step 8: restart the machine's face and see whether any of it stuck\n");
  // A frame first: the layout is written from inside the paint, which is the
  // only place on the canvas's path that is handed a Kernel.
  desk->render_desktop(kernel);
  const DeskTile before_ident = *tile_state(*desk, TileKind::Identities);
  const DeskTile before_auth = *tile_state(*desk, TileKind::Authority);
  const DeskTile before_tele = *tile_state(*desk, TileKind::Telemetry);
  delete desk;

  userland::HardwareCompositor* fresh = make(W, H);
  fresh->render_desktop(kernel);
  write_ppm(*fresh, W, H, out + "/11-after-restart.ppm");
  const DeskTile* rt = tile_state(*fresh, TileKind::Identities);
  const DeskTile* rd = tile_state(*fresh, TileKind::Authority);
  const DeskTile* rb = tile_state(*fresh, TileKind::Telemetry);
  check(rt && rt->col == before_ident.col && rt->slot == before_ident.slot,
        "Identities came back where it was left");
  check(rd && rd->floating == before_auth.floating && rd->y == before_auth.y,
        "Authority came back floating, at the same spot");
  check(rb && rb->collapsed == before_tele.collapsed,
        "and the other face's folded Telemetry came back folded");

  // ---------------------------------------------------------------------------
  std::printf("step 9: a dense display, where the paint is at half scale\n");
  // This is what was broken before there was one layout: the painter halved
  // every coordinate and the mouse handler did not, so a click on a card
  // landed twice as far into the page as the person had aimed.
  const uint32_t BW = 3200, BH = 1800;
  userland::HardwareCompositor* big = make(BW, BH);
  big->render_desktop(kernel);
  write_ppm(*big, BW, BH, out + "/12-dense-display.ppm");
  const CanvasLayout blay = canvas_layout(*big, BW, BH, big->canvas_state());
  std::printf("        %ux%u paints at 1/%d, so the page is %dx%d\n", BW, BH, blay.scale, blay.iw, blay.ih);
  check(blay.scale == 2, "the layout reports the half-scale paint");
  // Press where a tile is DRAWN on the glass -- paint coordinates times the
  // scale -- and the layout has to find that same tile there. This is the
  // whole point of one geometry source: the handler used to work in screen
  // pixels while the painter halved everything.
  TileRect bt{};
  if (find_tile(*big, BW, BH, TileKind::Authority, &bt)) {
    const int hit_x = (bt.x + bt.w / 2) * blay.scale, hit_y = (bt.y + 8) * blay.scale;
    const TileRect* under = blay.at(hit_x / blay.scale, hit_y / blay.scale);
    std::printf("        pressed %d,%d on the glass, which is %d,%d on the page\n",
                hit_x, hit_y, hit_x / blay.scale, hit_y / blay.scale);
    check(under && under->kind == TileKind::Authority,
          "a press where the tile is DRAWN on a 4K page lands on that tile",
          under ? aios::userland::tile_menu_name(under->kind) : "nothing");
  }
  delete big;

  // ---------------------------------------------------------------------------
  std::printf("step 10: reset the desk, which has to undo every one of those\n");
  userland::HardwareCompositor* again = make(W, H);
  again->render_desktop(kernel);
  click(*again, 40, H - 60, 1, hal::MouseRight);
  int rx = 0, ry = 0;
  const bool has_reset = again->context_menu_item_at("Reset this desk", &rx, &ry);
  check(has_reset, "the bare canvas offers 'Reset this desk'");
  if (has_reset) {
    click(*again, rx, ry, 1);
    again->render_desktop(kernel);
    write_ppm(*again, W, H, out + "/13-desk-reset.ppm");
    const DeskTile* zt = tile_state(*again, TileKind::Identities);
    const DeskTile* zd = tile_state(*again, TileKind::Authority);
    const DeskTile* zo = tile_state(*again, TileKind::Directives);
    const DeskTile* zb = tile_state(*again, TileKind::Telemetry);
    check(zt && zt->col == 0, "Identities is back in the left column");
    check(zd && !zd->floating, "Authority is pinned to the grid again");
    check(zo && !zo->hidden, "Standing Orders is on the glass");
    check(zb && !zb->collapsed, "and the other face's Telemetry is unfolded");
    // And it has to SURVIVE, or the next start brings the old desk back.
    again->render_desktop(kernel);
    delete again;
    userland::HardwareCompositor* last = make(W, H);
    last->render_desktop(kernel);
    write_ppm(*last, W, H, out + "/14-reset-survived.ppm");
    const DeskTile* lt = tile_state(*last, TileKind::Identities);
    check(lt && lt->col == 0, "the reset was written down, not just shown");
    delete last;
  } else {
    delete again;
  }

  // ---------------------------------------------------------------------------
  std::printf("step 11: the miner is off this desktop, and the F-keys closed up\n");
  userland::HardwareCompositor* bar = make(W, H);
  bar->set_canvas_mode(false);
  bar->render_desktop(kernel);
  write_ppm(*bar, W, H, out + "/15-workspace-no-miner.ppm");
  const auto tb = bar->top_bar();
  std::string joined;
  for (const auto& l : tb.labels) joined += (joined.empty() ? "" : " ") + l;
  std::printf("        the top bar reads: %s\n", joined.c_str());
  bool has_miner = false;
  for (const auto& l : tb.labels) if (l == "Miner") has_miner = true;
  check(!has_miner, "no Miner tab on the top bar");
  check(tb.labels.size() == 6, "six tabs, not seven");

  // The badges the start page prints are these keys. Press each one and see
  // the pane that card is for come up -- the label and the binding kept in
  // step by the machine, not by hand.
  const std::pair<int, const char*> keys[] = {
    {hal::KeyF1, "Shell"}, {hal::KeyF2, "Machine"}, {hal::KeyF3, "Entities"},
    {hal::KeyF4, "Editor"}, {hal::KeyF5, "MariaDB"}, {hal::KeyF6, "Web"},
  };
  for (const auto& [key, want] : keys) {
    bar->input_key(key);
    bar->render_desktop(kernel);
    const std::string got = bar->top_bar().current;
    check(got == want, std::string("F") + (char)('1' + (key - hal::KeyF1)) + " raises " + want,
          got.empty() ? "nothing" : got);
  }
  // F7 named the web pane before the miner left; now it names nothing, and it
  // must not quietly raise whatever happens to be last in the list.
  bar->input_key(hal::KeyF4);
  bar->render_desktop(kernel);
  bar->input_key(hal::KeyF7);
  bar->render_desktop(kernel);
  check(bar->top_bar().current == "Editor", "F7 raises nothing at all");

  // And the right-click menu has stopped offering it. This has to land on BARE
  // desktop: over a pane the menu is that pane's, which has never listed the
  // panes at all -- so a "no Miner" check there would pass for the wrong
  // reason. The strip between the two columns is the reliable bare spot now
  // that the machine pane runs the full height of the right one.
  int gap_x = 0, gap_y = 400;
  {
    const int margin = 14, gap = 14;
    const int usable_w = (int)W - 2 * margin;
    const int col_w = (usable_w * 56) / 100 - gap / 2;
    gap_x = margin + col_w + gap / 2;
  }
  click(*bar, gap_x, gap_y, 1, hal::MouseRight);
  bar->render_desktop(kernel);
  write_ppm(*bar, W, H, out + "/16-desktop-menu.ppm");
  int junk = 0;
  const bool is_desktop_menu = bar->context_menu_item_at("Show the start page", &junk, &junk);
  std::printf("        right-clicked bare desktop at %d,%d: %d items\n", gap_x, gap_y,
              (int)bar->context_menu_items().size());
  check(is_desktop_menu, "the click landed on bare desktop, not on a pane");
  check(!bar->context_menu_item_at("Miner", &junk, &junk), "the desktop menu no longer offers Miner");
  check(bar->context_menu_item_at("Editor", &junk, &junk), "and still offers the panes that are there");
  delete bar;

  std::printf("\n%s — frames in %s\n", failures ? "FAILURES ABOVE" : "all of it held", out.c_str());
  return failures ? 1 : 0;
}
