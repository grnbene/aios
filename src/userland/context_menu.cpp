// The right-click menu.
//
// A pointer with two buttons and only one of them wired is a pointer that is
// half there. What this menu offers is what can actually be done to the thing
// under it, and every entry reaches a command the desktop already has — the
// menu is a second way in, not a second implementation.
#include "aios/userland/compositor.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/clipboard.hpp"

#include <algorithm>

namespace aios::userland {
namespace {
constexpr int kMenuWidth = 258;
constexpr int kMenuRow = 24;
constexpr int kMenuGap = 9;
constexpr int kMenuPad = 6;

int menu_height(const std::vector<std::string>& items) {
  int h = kMenuPad * 2;
  for (const auto& it : items) h += it.empty() ? kMenuGap : kMenuRow;
  return h;
}
}  // namespace

void HardwareCompositor::open_context_menu(int x, int y) {
  bool have_clip = false;
  {
    auto clip = hal::clipboard_get();
    if (clip.ok() && !clip->empty()) have_clip = true;
  }

  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  menu_ = ContextMenu{};
  menu_.open = true;
  menu_.hover = -1;

  WindowSurface* over = nullptr;
  if (!canvas_mode_) {
    for (size_t i = windows_.size(); i-- > 0;) {
      WindowSurface& w = windows_[i];
      if (!w.visible || w.minimized) continue;
      if (w.bounds.contains(x, y)) { over = &w; break; }
    }
  }
  menu_.window_id = over ? over->window_id : 0;

  if (canvas_mode_) {
    // What can be done to the tile under the pointer, if there is one. The
    // rects come from the layout the painter used, so an item here acts on the
    // tile a person actually right-clicked rather than on where it used to be.
    const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
    const TileRect* over_tile = lay.at(x / lay.scale, y / lay.scale);
    menu_.tile_index = -1;
    menu_.items.clear();
    if (over_tile && over_tile->index >= 0 && over_tile->index < (int)canvas_state_.tiles.size()) {
      menu_.tile_index = over_tile->index;
      const DeskTile& t = canvas_state_.tiles[(size_t)over_tile->index];
      const std::string name = tile_menu_name(t.kind);
      menu_.items.push_back(t.collapsed ? "Unfold " + name : "Fold " + name + " away");
      menu_.items.push_back("Remove " + name);
      menu_.items.push_back(t.floating ? "Pin " + name + " to the grid"
                                       : "Let " + name + " float free");
      menu_.items.push_back("");
    }
    menu_.items.push_back(have_clip ? "Paste into the prompt" : "Paste (clipboard is empty)");
    menu_.items.push_back(canvas_state_.prompt.empty() ? "Copy the prompt (empty)" : "Copy the prompt");
    menu_.items.push_back("");
    menu_.items.push_back("Scene: Ocean at Dusk");
    menu_.items.push_back("Scene: Moonlit Beach");
    menu_.items.push_back("Scene: Cyber Matrix");
    menu_.items.push_back("Scene: Quantum Orb");
    menu_.items.push_back("Scene: Desert Pyramids");
    menu_.items.push_back("Next background scene (B)");
    menu_.items.push_back("Show the workspace");
    // Everything thrown off THIS face, offered back. A removed tile with no
    // way home is a tile deleted, which is not what an x on a card means.
    bool any_gone = false;
    for (const DeskTile& t : canvas_state_.tiles) {
      if (!t.hidden || tile_is_start_page(t.kind) != lay.start_page) continue;
      if (!any_gone) { menu_.items.push_back(""); any_gone = true; }
      menu_.items.push_back(std::string("Put back: ") + tile_menu_name(t.kind));
    }
    menu_.items.push_back("");
    menu_.items.push_back("Reset this desk");
    menu_.items.push_back("Restart the machine");
  } else if (over) {
    bool has_sel = (over->type == WindowType::Terminal && terminal_has_selection(*over)) ||
                   (over->type == WindowType::Editor && editor_has_selection(*over));
    bool has_line = !over->input.empty();
    menu_.items.clear();
    if (has_sel) {
      menu_.items.push_back("Copy selection");
    }
    menu_.items.push_back(has_line ? "Copy the line" : "Copy the line (empty)");
    menu_.items.push_back(have_clip ? "Paste at the cursor" : "Paste (clipboard is empty)");
    menu_.items.push_back(has_line ? "Cut the line" : "Cut the line (empty)");
    menu_.items.push_back("");
    menu_.items.push_back("Copy this pane's output");
    menu_.items.push_back("Clear this pane");
    menu_.items.push_back("");
    menu_.items.push_back("Close this pane");
  } else {
    menu_.items = {"Show the start page", "", "Terminal", "Machine", "Tasks", "Editor", "Database"};
  }

  // Open at the pointer, but never off the screen: a menu whose entries are
  // past the edge cannot be picked from.
  int mh = menu_height(menu_.items);
  menu_.x = std::min(x, std::max(0, (int)width_ - kMenuWidth - 2));
  menu_.y = std::min(y, std::max(0, (int)height_ - mh - 2));
}

bool HardwareCompositor::context_menu_click(int x, int y) {
  std::string chosen;
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    if (!menu_.open) return false;
    menu_.open = false;
    int mh = menu_height(menu_.items);
    // A click outside dismisses it and goes no further, the way every menu on
    // every other machine behaves.
    if (x < menu_.x || x >= menu_.x + kMenuWidth || y < menu_.y || y >= menu_.y + mh) return true;
    int at = menu_.y + kMenuPad;
    for (const auto& it : menu_.items) {
      int hgt = it.empty() ? kMenuGap : kMenuRow;
      if (y >= at && y < at + hgt && !it.empty()) { chosen = it; break; }
      at += hgt;
    }
  }
  if (!chosen.empty()) context_menu_invoke(chosen);
  return true;
}

void HardwareCompositor::context_menu_invoke(const std::string& item) {
  using namespace aios::hal;
  bool disabled = item.find("(empty)") != std::string::npos ||
                  item.find("clipboard is empty") != std::string::npos;

  if (item == "Copy selection") {
    std::string sel;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (auto& w : windows_) {
        if (w.window_id != menu_.window_id) continue;
        if (w.type == WindowType::Terminal && terminal_has_selection(w)) {
          sel = terminal_selection_text(w);
        } else if (w.type == WindowType::Editor && editor_has_selection(w)) {
          sel = editor_selection_text(w);
        }
        break;
      }
    }
    if (sel.empty()) { set_toast("nothing selected"); return; }
    auto put = hal::clipboard_set(sel);
    set_toast(put.ok() ? "copied " + std::to_string(sel.size()) + " chars" : "clipboard: " + put.error().str());
    return;
  }
  if (item.rfind("Paste", 0) == 0) {
    if (disabled) { set_toast("the clipboard is empty"); return; }
    input_key(KeyCtrlV);
    return;
  }
  if (item.rfind("Copy the line", 0) == 0 || item.rfind("Copy the prompt", 0) == 0) {
    if (disabled) { set_toast("nothing to copy"); return; }
    input_key(KeyCtrlC);
    return;
  }
  if (item.rfind("Cut the line", 0) == 0) {
    if (disabled) { set_toast("nothing to cut"); return; }
    input_key(KeyCtrlX);
    return;
  }
  if (item == "Copy this pane's output") {
    std::string all;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (auto& w : windows_) {
        if (w.window_id != menu_.window_id) continue;
        for (const auto& l : w.lines) all += l + "\n";
        break;
      }
    }
    if (all.empty()) { set_toast("this pane has printed nothing yet"); return; }
    auto put = hal::clipboard_set(all);
    set_toast(put.ok() ? "copied " + std::to_string(all.size()) + " bytes off this pane"
                       : "clipboard: " + put.error().str());
    return;
  }
  if (item == "Clear this pane") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id != menu_.window_id) continue;
      w.lines.clear();
      w.scroll_y = 0;
      w.scrollback_offset = 0;
      break;
    }
    return;
  }
  if (item == "Close this pane") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id != menu_.window_id) continue;
      w.visible = false;
      break;
    }
    return;
  }
  // The desk's own items. Each acts on menu_.tile_index, which was fixed when
  // the menu opened over that tile.
  if (item.rfind("Fold ", 0) == 0 || item.rfind("Unfold ", 0) == 0) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    if (menu_.tile_index >= 0 && menu_.tile_index < (int)canvas_state_.tiles.size())
      desk_fold(canvas_state_, menu_.tile_index, item.rfind("Fold ", 0) == 0);
    return;
  }
  if (item.rfind("Remove ", 0) == 0) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    desk_hide(canvas_state_, menu_.tile_index);
    return;
  }
  if (item.rfind("Let ", 0) == 0) {
    // Unpinning must not move it: it floats from exactly where the grid had
    // just put it, so the only thing that changes is what happens next.
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
    for (const TileRect& r : lay.tiles) {
      if (r.index != menu_.tile_index) continue;
      desk_unpin(canvas_state_, menu_.tile_index, r);
      break;
    }
    return;
  }
  if (item.rfind("Pin ", 0) == 0) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    desk_pin(canvas_state_, menu_.tile_index);
    return;
  }
  if (item.rfind("Put back: ", 0) == 0) {
    const std::string want = item.substr(10);
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (const DeskTile& t : canvas_state_.tiles) {
      if (want != tile_menu_name(t.kind)) continue;
      desk_show(canvas_state_, t.kind);
      break;
    }
    return;
  }
  if (item == "Reset this desk") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    seed_desk_tiles(canvas_state_);
    canvas_state_.layout_dirty = true;
    set_toast("Desk back to how it ships");
    return;
  }
  if (item == "Restart the machine") { set_toast("Restarting machine..."); submit_command("reboot"); return; }
  // Through set_view, so these name the same three views the switcher does:
  // "Show the start page" used to set canvas_mode_ only, which on a machine
  // showing the desk raised the desk and called it the start page.
  if (item == "Show the workspace") { std::lock_guard<aios::hal::Mutex> lock(state_mutex_); set_view(DeskView::Workspace); return; }
  if (item == "Show the start page") { std::lock_guard<aios::hal::Mutex> lock(state_mutex_); set_view(DeskView::StartPage); return; }
  if (item == "Scene: Ocean at Dusk") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    canvas_state_.set_every_scene(4);
    canvas_state_.composed_text = "ocean at dusk sunset open horizon water waves orange purple twilight glow";
    ++canvas_state_.composed_generation;
    canvas_state_.layout_dirty = true;
    if (kernel_) desk_layout_save(canvas_state_, *kernel_);
    set_toast("Background: Ocean at Dusk");
    return;
  }
  if (item == "Scene: Moonlit Beach") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    canvas_state_.set_every_scene(0);
    canvas_state_.layout_dirty = true;
    if (kernel_) desk_layout_save(canvas_state_, *kernel_);
    set_toast("Background: Moonlit Beach");
    return;
  }
  if (item == "Scene: Cyber Matrix") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    canvas_state_.set_every_scene(1);
    canvas_state_.layout_dirty = true;
    if (kernel_) desk_layout_save(canvas_state_, *kernel_);
    set_toast("Background: Cyber Matrix");
    return;
  }
  if (item == "Scene: Quantum Orb") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    canvas_state_.set_every_scene(2);
    canvas_state_.layout_dirty = true;
    if (kernel_) desk_layout_save(canvas_state_, *kernel_);
    set_toast("Background: Quantum Orb");
    return;
  }
  if (item == "Scene: Desert Pyramids") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    canvas_state_.set_every_scene(3);
    canvas_state_.composed_text = "three pyramids in the desert dunes sand starlight night sky";
    ++canvas_state_.composed_generation;
    canvas_state_.layout_dirty = true;
    if (kernel_) desk_layout_save(canvas_state_, *kernel_);
    set_toast("Background: Desert Pyramids");
    return;
  }
  if (item == "Next scene" || item == "Next background scene (B)") {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    {
      const DeskView v = current_view();
      int next_s = (canvas_state_.scene(v) + 1) % 5;
      canvas_state_.set_every_scene(next_s);
      if (next_s == 4) canvas_state_.composed_text = "ocean at dusk sunset open horizon water waves orange purple twilight glow";
      else if (next_s == 3) canvas_state_.composed_text = "three pyramids in the desert dunes sand starlight night sky";
      canvas_state_.layout_dirty = true;
      if (kernel_) desk_layout_save(canvas_state_, *kernel_);
      set_toast("Background: Scene " + std::to_string(next_s));
    }
    return;
  }

  static const std::pair<const char*, WindowType> panes[] = {
    {"Terminal", WindowType::Terminal}, {"Machine", WindowType::Hardware},
    {"Tasks", WindowType::Tasks},
    {"Editor", WindowType::Editor},     {"Database", WindowType::Database},
  };
  for (const auto& [name, type] : panes) {
    if (item != name) continue;
    // NOTHING here may run while this thread holds state_mutex_.
    //
    // first_of_type() falls through to ensure_app_window() when no window of
    // that type exists yet, and ensure_app_window() takes state_mutex_ itself;
    // so does bring_to_front(). state_mutex_ is a plain aios::hal::Mutex, not a
    // recursive one, so taking it around this block was the frame loop locking
    // against itself and never coming back: right-clicking the desktop and
    // choosing Editor froze the display outright, with the process still alive
    // and the GPU thread spinning, which is what made it look like a crash.
    // Every item in this table did it -- Terminal, Machine, Tasks and Database
    // as much as Editor.
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      canvas_mode_ = false;
      // Raising a pane means leaving the start page, which draws the dispatch
      // card INSTEAD of any window: without this the pane came to the front of
      // a stack nothing was painting.
      if (active_top_tab_ == TopTab::FastTables) active_top_tab_ = TopTab::Workspaces;
      palette_open_ = false;
    }
    uint32_t id = first_of_type(type);
    if (id) {
      {
        std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
        for (auto& w : windows_) if (w.window_id == id) { w.visible = true; w.minimized = false; }
      }
      (void)bring_to_front(id);
    }
    return;
  }
}

bool HardwareCompositor::context_menu_item_at(const std::string& item, int* x, int* y) const {
  if (!menu_.open) return false;
  int at = menu_.y + kMenuPad;
  for (const auto& it : menu_.items) {
    if (it.empty()) { at += kMenuGap; continue; }
    if (it == item) {
      if (x) *x = menu_.x + kMenuWidth / 2;
      if (y) *y = at + kMenuRow / 2;
      return true;
    }
    at += kMenuRow;
  }
  return false;
}

void HardwareCompositor::draw_context_menu() {
  if (!menu_.open || menu_.items.empty()) return;
  int mh = menu_height(menu_.items);
  draw_rounded_rect_to(framebuffer_, (int)width_, (int)height_, menu_.x + 2, menu_.y + 3,
                       kMenuWidth, mh, 8, 0x40000000);
  draw_rounded_rect_to(framebuffer_, (int)width_, (int)height_, menu_.x, menu_.y, kMenuWidth, mh, 8, 0xFF1B2028);
  draw_round_outline_to(framebuffer_, (int)width_, (int)height_, menu_.x, menu_.y, kMenuWidth, mh, 8, 0xFF39404B);

  int at = menu_.y + kMenuPad;
  for (const auto& it : menu_.items) {
    if (it.empty()) {
      draw_rect_to(framebuffer_, (int)width_, (int)height_, menu_.x + 10, at + kMenuGap / 2,
                   kMenuWidth - 20, 1, 0xFF2E353F);
      at += kMenuGap;
      continue;
    }
    bool disabled = it.find("(empty)") != std::string::npos ||
                    it.find("clipboard is empty") != std::string::npos;
    bool hot = mouse_x_ >= menu_.x && mouse_x_ < menu_.x + kMenuWidth &&
               mouse_y_ >= at && mouse_y_ < at + kMenuRow;
    if (hot && !disabled) {
      draw_rounded_rect_to(framebuffer_, (int)width_, (int)height_, menu_.x + 5, at,
                           kMenuWidth - 10, kMenuRow, 5, 0xFF2A3340);
    }
    draw_text_to(framebuffer_, (int)width_, (int)height_, menu_.x + 16, at + 4, it,
                 disabled ? 0xFF5B646F : (hot ? 0xFFF8FAFC : 0xFFD7DDE5), Face::Ui);
    at += kMenuRow;
  }
}

}  // namespace aios::userland
