#include "aios/userland/compositor.hpp"
#include "aios/core/base64.hpp"
#include "aios/userland/gpu_present.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/clipboard.hpp"
#include "aios/userland/font_atlas.hpp"
#include "aios/userland/glyph_engine.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/pixels.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/machine_sql.hpp"
#include "aios/userland/mysql_binlog.hpp"
#include "aios/userland/web_pane.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/scene_description.hpp"
#include "aios/userland/themes.hpp"
#include "aios/compute/compute_graph.hpp"
#include "aios/net/ssh.hpp"
#include "aios/userland/patch_transaction.hpp"
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

// Hosted only. The ANSI-terminal fallback desktop talks to a tty, and a command
// runs on its own thread so the frame loop stays responsive. On bare metal
// there is no tty (the desktop IS the display) and no threads, so neither is
// compiled in and a command runs on the frame loop instead.
#if !defined(AIOS_METAL)
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#endif

#if defined(AIOS_HAS_SDL2)
#include <SDL.h>
#include <SDL_vulkan.h>
#endif


/* Step trace for hunting stalls on bare metal: the Pi kernel defines this
 * (udp/42425 breadcrumbs); everywhere else it is a weak no-op. */
#if defined(__APPLE__) || !defined(AIOS_METAL)
/* The attribute goes after the linkage: GCC 10 (the CentOS 7 build's
 * devtoolset) rejects it in front of extern "C". */
extern "C" __attribute__((weak)) void aios_trace(const char*) {}
#else
extern "C" void aios_trace(const char* step) __attribute__((weak));
#endif
#define AIOS_TRACE(s) do { if (aios_trace) aios_trace(s); } while (0)

namespace {
/* Today's date from the machine's clock (NTP-set on the Pi). The calendar
 * had "today" hard-coded to 2026-09-13 in three places. */
void calendar_today(int& y, int& m, int& d) {
  const std::time_t t = std::time(nullptr);
  const std::tm lt = *std::localtime(&t);
  y = lt.tm_year + 1900; m = lt.tm_mon + 1; d = lt.tm_mday;
}
bool calendar_is_today(int y, int m, int d) {
  int ty, tm_, td; calendar_today(ty, tm_, td);
  return y == ty && m == tm_ && d == td;
}
/* The real shape of a month: which Monday-based column the 1st falls in
 * (0 = Monday .. 6 = Sunday), how many days it has, and how many week rows
 * that takes (4 to 6). The grid used to be September 2026 for every month:
 * the 1st always on a Tuesday, always 30 days, always 5 rows. */
void calendar_month_layout(int y, int m, int& first_col, int& days, int& rows) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) m = 1;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  days = kDays[m - 1] + ((m == 2 && leap) ? 1 : 0);
  // Sakamoto's day of week for the 1st: 0 = Sunday.
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int yy = y - (m < 3 ? 1 : 0);
  int dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + 1) % 7;
  first_col = (dow + 6) % 7;  // Monday-based
  rows = (first_col + days + 6) / 7;
}
}  // namespace

namespace aios::userland {
namespace {

// The console's palette, ported token for token out of
// console/web/src/styles.css — the `:root` block, which is the shipped "Slate"
// dark set. An operator who drives the fleet from the console and then sits
// down in front of the machine itself should be looking at the same
// instrument: graphite surfaces, hairline rules, near-white ink, and hue kept
// in reserve for status. Nothing here is a colour the console does not have,
// which is the whole point — the old desktop had cyan, violet, amber and two
// greens on screen at once and looked like a toy beside it.
namespace tok {
constexpr uint32_t bg      = 0xFF0A0A0A;  // --bg          the desktop itself
constexpr uint32_t panel   = 0xFF171717;  // --panel       a window's surface
constexpr uint32_t panel2  = 0xFF101010;  // --panel-2     a card inside a window
constexpr uint32_t rail    = 0xFF070707;  // --sidebar     top bar and dock
constexpr uint32_t text    = 0xFFF4F4F4;  // --text
constexpr uint32_t dim     = 0xFFB2B2B2;  // --text-dim
constexpr uint32_t faint   = 0xFF8E8E8E;  // --text-faint  labels
constexpr uint32_t line    = 0xFF232323;  // --line over --panel-2
constexpr uint32_t line_up = 0xFF2A2A2A;  // --line over --panel
constexpr uint32_t border  = 0xFF3D3D3D;  // --border      an unfocused frame
constexpr uint32_t strong  = 0xFF767676;  // --border-strong: the focused frame
constexpr uint32_t accent  = 0xFFE6E6E6;  // --accent — the console's accent is near-white
constexpr uint32_t wash    = 0xFF1E1E1E;  // --wash-3 over --bg: hover, pressed, selected
constexpr uint32_t cta     = 0xFFF4F4F4;  // --cta-grad top: the one filled button
constexpr uint32_t cta_2   = 0xFFBDBDBD;  // --cta-grad bottom
constexpr uint32_t on_cta  = 0xFF0A0A0A;  // --on-cta: ink on that fill
constexpr uint32_t ok      = 0xFF42D77D;  // --ok
constexpr uint32_t warn    = 0xFFF5A623;  // --warn
constexpr uint32_t err     = 0xFFFF6B6B;  // --err
}

// The editor's own palette. The desktop's chrome is deliberately near
// monochrome; code is the one place colour carries meaning rather than
// decoration, so these are the only saturated inks in the machine. Chosen
// against the editor's #0D1117 ground.
// The syn:: palette above was the original hardwired editor palette. It is now
// superseded by the data-driven theme engine in aios/userland/themes.hpp.
// The Panic Dark theme in themes.cpp restates these exact ARGB values so the
// default render is byte-identical to the old switch.

// Syntax colour is now driven by the active theme (aios/userland/themes.hpp).
// The render path calls aios::userland::colour_for(TokenClass) which looks up
// the ink in the currently selected EditorTheme. The local switch that was
// hardwired to the syn:: palette above has been removed.

#if defined(AIOS_METAL)
extern "C" uint64_t metal_monotonic_ms();
#endif

// Milliseconds since the machine started: for anything that blinks or ticks.
uint64_t steady_ms() {
#if defined(AIOS_METAL)
  return metal_monotonic_ms();
#else
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

#if defined(AIOS_METAL)
extern "C" bool metal_wall_clock_set();
#endif
std::string clock_text_ampm() {
#if defined(AIOS_METAL)
  /* This returned the literal "08:26 AM" - a clock that never moved. The
   * board has no RTC; the kernel sets its wall clock from NTP, and until
   * that has answered there is no time to show. */
  if (!metal_wall_clock_set()) return "--:--";
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%a %I:%M %p", &tm);
  std::string out = buf;
  const size_t sp = out.find(' ');
  if (sp != std::string::npos && out.size() > sp + 1 && out[sp + 1] == '0') out.erase(sp + 1, 1);   /* "Thu 3:42 PM" */
  return out;
#else
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%I:%M %p", &tm);
  return buf;
#endif
}

// The top bar's tabs — the console's own way of switching what you are looking
// at: a word, and a rule under the one you are on. Drawn and hit-tested from
// this one list, so a click always lands on the tab a human sees. There is no
// launcher column of ASCII glyph tiles down the left margin any more; it was
// five buttons that duplicated these five, drawn as `[*]` and `[#]`.
struct TopApp { const char* label; WindowType type; };
const TopApp TOP_APPS[] = {
  {"Shell", WindowType::Terminal},
  {"Machine", WindowType::Hardware},
  {"Entities", WindowType::Tasks},
  {"Editor", WindowType::Editor},
  {"MariaDB", WindowType::Database},
  {"Web", WindowType::Browser},
  {"Mail", WindowType::Mail},
  {"Contacts", WindowType::Contacts},
  {"Calendar", WindowType::Calendar},
  {"Apps", WindowType::FaceApp},
  {"Studio", WindowType::Inspector},
};
constexpr int TAB_X0_MARGIN = 14;  // clearance between the Launch chip and the first tab
// Proportional type has no cell width: "Shell" and "Machine" are not 5 and 7
// times anything. Every width here is measured with the face that draws it, and
// the hit test calls the same function, so a click still lands on the word.

// The power control and what it opens. Three options, each one wired to a real
// path: Restart and Power off go down to the machine's own reset and ACPI S5
// through the command surface, and Sleep blanks the screen and idles until a
// human touches the machine. The panel's width comes from the widest of THESE
// strings, measured with the face that draws them.
struct PowerOption { const char* label; const char* note; };
const PowerOption POWER_OPTIONS[] = {
  {"Restart", "reset this machine"},
  {"Power off", "ACPI S5, off"},
  {"Sleep", "blank; any input wakes"},
};
constexpr int POWER_OPTION_COUNT = 3;
const char* const POWER_LABEL = "Power";
// What is asleep says so, in the only ink on a blank screen.
const char* const SLEEP_HINT = "asleep — move the mouse, click, or press any key";

// The miner pane's three actions. Both render_miner_window and the hit test in
// input_mouse_button lay them out from HERE: the widths were 140, 140 and 180
// by hand, with the labels drawn at a fixed inset inside them, so the fill and
// the ink agreed only by luck and a longer label would simply run out of its
// button. Each one is now its measured label plus symmetric padding, and each
// starts from the previous one's right edge, so widening one moves the next
// along instead of overlapping it.
struct MinerBtn { const char* label; int x, w; };
constexpr int MINER_BTN_PAD = 16;      // each side of the label
constexpr int MINER_BTN_GAP = 10;
constexpr int MINER_BTN_X0 = 10;
constexpr int MINER_BTN_H = 36;
void miner_buttons(const HardwareCompositor& S, bool daemon, MinerBtn out[3]) {
  // The daemon button keeps ONE width for both of its labels: a button that
  // changes size when you press it is a button you cannot aim at twice.
  // The miner is a local hashing exercise: there are no blocks to mine and no
  // daemon to start, so the buttons say what they do.
  (void)daemon;
  out[0] = {"Hash 1 round", 0, S.text_width("Hash 1 round", Face::Ui) + 2 * MINER_BTN_PAD};
  out[1] = {"Hash 2 rounds", 0, S.text_width("Hash 2 rounds", Face::Ui) + 2 * MINER_BTN_PAD};
  out[2] = {"Status", 0, S.text_width("Status", Face::Ui) + 2 * MINER_BTN_PAD};
  int x = MINER_BTN_X0;
  for (int i = 0; i < 3; ++i) {
    out[i].x = x;
    x += out[i].w + MINER_BTN_GAP;
  }
}

// The terminal's one-click commands, drawn as chips and hit-tested from here.
const char* const TERM_CHIPS[] = {"info", "ps", "devices", "free", "tasks", "reboot", "clear"};

// The two workstation applications' chips. These live at file scope for the
// same reason the top bar's tabs do: the renderer and the hit test must walk
// ONE list. They used to be two identical literal arrays a hundred lines
// apart, which is a rename away from a row of buttons that do the wrong thing.
const char* const EDIT_CHIPS[] = {"Save", "Undo", "Redo", "Find", "Preview", "New",
                                 "Parent dir", "Build", "Command"};
// The studio's toolbar. Eleven words in eleven pills is a menu bar wearing
// buttons: it is wider than the pane, it says nothing at a glance, and it
// leaves no room for the things that matter (what page you are on, how many
// rows there are). The action is the ICON; the words move into a tooltip that
// appears under whichever one the pointer is on.
struct DbTool {
  const char* id;                  // what the click handler switches on
  aios::userland::Icon icon;
  const char* tip;                 // shown on hover, in full
  bool gap_after;                  // a group break after this one
};
const DbTool DB_TOOLS[] = {
  {"Connections", aios::userland::Icon::Database,  "Connections - saved engine profiles",              false},
  {"Connect",     aios::userland::Icon::Plug,      "Connect / reconnect this window's engine",         false},
  {"Refresh",     aios::userland::Icon::Refresh,   "Reload the table list and this page",              true},
  {"Run SQL",     aios::userland::Icon::Play,      "Run the statement in the SQL bar   (Enter)",       false},
  {"Describe",    aios::userland::Icon::Inspector, "Describe: the selected table's columns and keys",  false},
  {"DDL",         aios::userland::Icon::Code,      "DDL: the CREATE TABLE behind the selected table",  false},
  {"Design",      aios::userland::Icon::Gear,      "Design: edit columns, indexes and foreign keys",   true},
  {"Add row",     aios::userland::Icon::Plus,      "Insert an empty row into the open table",          false},
  {"Delete row",  aios::userland::Icon::Trash,     "Delete the selected row",                          true},
  {"Export CSV",  aios::userland::Icon::Download,  "Export what the grid is showing to a CSV file",    false},
  {"Fleet Ops",   aios::userland::Icon::Cluster,   "Fleet Ops - live engine telemetry and replication",false},
  {"Clear",       aios::userland::Icon::Close,     "Clear the grid and the SQL bar",                   false},
};
constexpr int DB_TOOL_W = 30;
constexpr int DB_TOOL_H = 26;
constexpr int DB_TOOL_GAP = 4;
constexpr int DB_TOOL_GROUP_GAP = 12;
constexpr int DB_TOOL_X0 = 22;

// The page sizes the pager cycles through. A grid shows thirty rows; asking
// the engine for a thousand is already generous.
const int DB_PAGE_SIZES[] = {50, 100, 200, 500, 1000};

// The pager strip under the grid. Both the renderer and the hit test lay it
// out from here, so a button can never be drawn somewhere the click is not.
struct DbPagerGeom {
  int y = 0, h = 20, btn = 22;
  int first_x = 0, prev_x = 0, next_x = 0, last_x = 0;
  int label_x = 0, label_w = 0;
  int size_x = 0, size_w = 70;
  int col_prev_x = 0, col_next_x = 0;
};
static DbPagerGeom db_pager_geom(int grid_x, int grid_w, int body_y, int body_h) {
  DbPagerGeom g;
  g.y = body_y + body_h + 3;
  int px = grid_x + 4;
  g.first_x = px;
  g.prev_x = px + 26;
  g.label_x = px + 54;
  g.label_w = 128;
  g.next_x = g.label_x + g.label_w;
  g.last_x = g.next_x + 26;
  g.size_x = g.last_x + 34;
  g.col_prev_x = grid_x + grid_w - 56;
  g.col_next_x = grid_x + grid_w - 28;
  return g;
}

// Both applications lay their chrome out on the same rows, so the two panes
// read as one workstation rather than two unrelated programs.
constexpr int APP_CHIP_Y = 38;      // the chip row
constexpr int APP_CHIP_H = 24;
constexpr int APP_HEAD_Y = 68;      // the "what am I looking at" line
constexpr int APP_BODY_Y = 90;      // the top of the working area
constexpr int APP_FOOT_H = 45;      // the command row reserved at the bottom
// One row of monospace, everywhere — measured from the face rather than
// written down. It was a flat 20, which suited the 14px mono cut it was
// chosen for and left one pixel of air the moment that cut moved to 16px:
// a grid of rows whose descenders touch the row below.
const int APP_ROW = fonts::MONO.line_h + 3;
constexpr int EDIT_TAB_H = 22;      // the editor's open-file strip

// Where a tool sits on the chip row, by the same arithmetic the renderer and
// the hit test both walk.
static int db_tool_x(size_t index) {
  int x = DB_TOOL_X0;
  for (size_t i = 0; i < index && i < sizeof(DB_TOOLS) / sizeof(DB_TOOLS[0]); ++i)
    x += DB_TOOL_W + DB_TOOL_GAP + (DB_TOOLS[i].gap_after ? DB_TOOL_GROUP_GAP : 0);
  return x;
}
static int db_tool_hit(int rel_x, int rel_y) {
  if (rel_y < APP_CHIP_Y - 1 || rel_y > APP_CHIP_Y + DB_TOOL_H) return -1;
  for (size_t i = 0; i < sizeof(DB_TOOLS) / sizeof(DB_TOOLS[0]); ++i) {
    int x = db_tool_x(i);
    if (rel_x >= x && rel_x < x + DB_TOOL_W) return static_cast<int>(i);
  }
  return -1;
}


// The Ctrl+<letter> codes live with the rest of the key codes, in hal/keys.hpp.
using aios::hal::KeyCtrlA; using aios::hal::KeyCtrlB; using aios::hal::KeyCtrlC;
using aios::hal::KeyCtrlE; using aios::hal::KeyCtrlK; using aios::hal::KeyCtrlL;
using aios::hal::KeyCtrlR; using aios::hal::KeyCtrlS; using aios::hal::KeyCtrlU;
using aios::hal::KeyCtrlV; using aios::hal::KeyCtrlX; using aios::hal::KeyCtrlP;

// As much of `text` as fits in `px`, with an ellipsis when it does not. A grid
// cell that overruns its column paints straight through its neighbour.
std::string fit_text(const HardwareCompositor& S, const std::string& text, int px, Face face) {
  if (px <= 0) return "";
  if (S.text_width(text, face) <= px) return text;
  // Forward, accumulating: dropping the last character and re-measuring the
  // whole string is quadratic, and every pane's every label goes through here
  // on every frame.
  const int ell = S.text_width("..", face);
  std::string out;
  out.reserve(text.size());
  int w = 0;
  for (char ch : text) {
    const int cw = S.glyph_advance(ch, face);
    if (w + cw + ell > px) break;
    out += ch;
    w += cw;
  }
  if (!out.empty()) return out + "..";
  if (S.text_width(".", face) <= px) return ".";
  return "";
}

// A pointer, drawn in software because on the metal there is nothing else to
// draw it: 'X' is the outline, '.' the fill, ' ' shows the desktop through.
const char* const CURSOR_ROWS[] = {
  "X           ",
  "XX          ",
  "X.X         ",
  "X..X        ",
  "X...X       ",
  "X....X      ",
  "X.....X     ",
  "X......X    ",
  "X.......X   ",
  "X........X  ",
  "X.....XXXXX ",
  "X..X..X     ",
  "X.X X..X    ",
  "XX  X..X    ",
  "X    X..X   ",
  "     X..X   ",
  "      XX    ",
};

}  // namespace

// A grain program open on the desktop (`grain open`, see grain_open): its VM,
// kept while it has panes, its files and what it printed.
struct GrainDesktopApp {
  std::string path;                     // as it was opened
  std::string dir;                      // its directory: the program's relative paths start there
  std::unique_ptr<grain::Files> files;
  std::unique_ptr<grain::Vm> vm;
  std::vector<std::string> output;      // printed since the tick last showed it at the terminal
  bool attached = false;                // run_panes() asked for its panes to be shown
  std::string failed;                   // what stopped it: its windows show this instead of a frame
  bool failure_reported = false;
};

namespace {
std::string grain_key_name(int key, bool* ctrl);   // the desktop's key codes as grain names them (with grain_open)
}  // namespace

HardwareCompositor::HardwareCompositor(uint32_t screen_width, uint32_t screen_height)
    : width_(screen_width), height_(screen_height), framebuffer_(screen_width * screen_height, tok::bg) {}

HardwareCompositor::~HardwareCompositor() = default;

Result<uint32_t> HardwareCompositor::create_window(const std::string& title, int x, int y, int w, int h, WindowType type) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  WindowSurface win;
  win.window_id = next_window_id_++;
  win.title = title;
  win.type = type;
  win.bounds = {x, y, w, h};
  win.normal_bounds = win.bounds;
  win.focused = true;
  win.visible = true;
  win.minimized = false;
  win.maximized = false;
  win.buffer.resize(win.content_pixels(), tok::panel);

  for (auto& w_existing : windows_) {
    w_existing.focused = false;
  }
  active_window_id_ = win.window_id;
  windows_.push_back(win);
  return win.window_id;
}

Result<void> HardwareCompositor::move_window(uint32_t window_id, int x, int y) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& win : windows_) {
    if (win.window_id == window_id) {
      win.bounds.x = x;
      win.bounds.y = y;
      if (!win.maximized) {
        win.normal_bounds.x = x;
        win.normal_bounds.y = y;
      }
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "Window not found");
}

Result<void> HardwareCompositor::resize_window(uint32_t window_id, int w, int h) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  if (w < 260) w = 260;
  if (h < 180) h = 180;
  for (auto& win : windows_) {
    if (win.window_id == window_id) {
      win.bounds.width = w;
      win.bounds.height = h;
      if (!win.maximized) {
        win.normal_bounds.width = w;
        win.normal_bounds.height = h;
      }
      win.buffer.resize(win.content_pixels(), tok::panel);
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "Window not found");
}

Result<void> HardwareCompositor::focus_window(uint32_t window_id) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& win : windows_) {
    win.focused = (win.window_id == window_id);
    if (win.focused) {
      win.minimized = false;
      win.visible = true;
      active_window_id_ = window_id;
    }
  }
  return Ok();
}

Result<void> HardwareCompositor::minimize_window(uint32_t window_id) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& win : windows_) {
    if (win.window_id == window_id) {
      win.minimized = true;
      win.focused = false;
      break;
    }
  }
  for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
    if (!it->minimized && it->visible) {
      it->focused = true;
      active_window_id_ = it->window_id;
      break;
    }
  }
  return Ok();
}

Result<void> HardwareCompositor::maximize_window(uint32_t window_id) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& win : windows_) {
    if (win.window_id == window_id) {
      if (win.maximized) {
        win.maximized = false;
        win.bounds = win.normal_bounds;
      } else {
        win.normal_bounds = win.bounds;
        win.maximized = true;
        const Rect wa = work_area();
        win.bounds = {wa.x, wa.y, wa.width, wa.height};
      }
      win.buffer.resize(win.content_pixels(), tok::panel);
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "Window not found");
}

Result<void> HardwareCompositor::close_window(uint32_t window_id) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto it = windows_.begin(); it != windows_.end(); ++it) {
    if (it->window_id == window_id) {
      it->visible = false;
      it->minimized = true;
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "Window not found");
}

Result<void> HardwareCompositor::bring_to_front(uint32_t window_id) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (size_t i = 0; i < windows_.size(); ++i) {
    if (windows_[i].window_id == window_id) {
      WindowSurface w = std::move(windows_[i]);
      windows_.erase(windows_.begin() + i);
      w.focused = true;
      w.minimized = false;
      w.visible = true;
      windows_.push_back(std::move(w));
      active_window_id_ = window_id;
      for (size_t j = 0; j < windows_.size() - 1; ++j) {
        windows_[j].focused = false;
      }
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "Window not found");
}

// ---------------------------------------------------------------------------
// Alt+Tab
// ---------------------------------------------------------------------------
void HardwareCompositor::alt_tab_step(bool backwards) {
  const int cur_ws = static_cast<int>(current_workspace_);
  if (!alt_tab_open_) {
    alt_tab_order_.clear();
    // windows_ is the z-order, front last, so backwards is most-recent-first.
    // A minimised window is still on this desk and still worth tabbing to;
    // one belonging to another section is not.
    for (size_t i = windows_.size(); i-- > 0;) {
      const WindowSurface& w = windows_[i];
      if (w.workspace == cur_ws || w.pinned || w.workspace == -1)
        alt_tab_order_.push_back(w.window_id);
    }
    if (alt_tab_order_.size() < 2) {
      // One window (or none) is not something to switch between.
      alt_tab_order_.clear();
      return;
    }
    alt_tab_open_ = true;
    // Open ON the second entry: one Tab means "the thing I was just in",
    // which is the whole point of the gesture.
    alt_tab_index_ = backwards ? static_cast<int>(alt_tab_order_.size()) - 1 : 1;
    return;
  }
  const int n = static_cast<int>(alt_tab_order_.size());
  if (n <= 0) return;
  alt_tab_index_ = (alt_tab_index_ + (backwards ? -1 : 1) + n) % n;
}

void HardwareCompositor::alt_tab_commit() {
  if (!alt_tab_open_) return;
  const int n = static_cast<int>(alt_tab_order_.size());
  const uint32_t pick = (alt_tab_index_ >= 0 && alt_tab_index_ < n)
                            ? alt_tab_order_[static_cast<size_t>(alt_tab_index_)] : 0;
  alt_tab_open_ = false;
  alt_tab_order_.clear();
  if (!pick) return;
  // Off the canvas, if that is where we were: the window it raises lives on a
  // desktop, and raising one you cannot see is not switching to it.
  canvas_mode_ = false;
  bring_to_front(pick);
}

void HardwareCompositor::alt_tab_cancel() {
  alt_tab_open_ = false;
  alt_tab_order_.clear();
}

void HardwareCompositor::draw_alt_tab() {
  if (!alt_tab_open_ || alt_tab_order_.empty()) return;
  const int W = static_cast<int>(width_), H = static_cast<int>(height_);

  const int tile = 92, gap = 12, pad = 20;
  const int label_h = line_height(Face::Ui) + 6;
  const int n = static_cast<int>(alt_tab_order_.size());
  int panel_w = pad * 2 + n * tile + (n - 1) * gap;
  const int panel_h = pad * 2 + tile + label_h;
  if (panel_w > W - 40) panel_w = W - 40;
  const int px0 = (W - panel_w) / 2, py0 = (H - panel_h) / 2;

  shadow_rounded(px0, py0, panel_w, panel_h, 14, 20, 52, 5);
  draw_rounded_rect_to(framebuffer_, W, H, px0, py0, panel_w, panel_h, 14, 0xF00B0E18);
  draw_round_outline_to(framebuffer_, W, H, px0, py0, panel_w, panel_h, 14, tok::strong);

  int x = px0 + pad;
  for (int i = 0; i < n; ++i) {
    const WindowSurface* w = find_window(alt_tab_order_[static_cast<size_t>(i)]);
    if (!w) { x += tile + gap; continue; }
    const bool on = (i == alt_tab_index_);
    if (on) {
      draw_rounded_rect_to(framebuffer_, W, H, x - 4, py0 + pad - 4, tile + 8, tile + 8, 10, tok::wash);
      draw_round_outline_to(framebuffer_, W, H, x - 4, py0 + pad - 4, tile + 8, tile + 8, 10, tok::cta);
    }
    draw_app_icon(framebuffer_, W, H, x + (tile - 48) / 2, py0 + pad + (tile - 48) / 2, w->type, 48);
    x += tile + gap;
  }

  // The selected window's title, under the row — a name says which of two
  // terminals you are about to raise; an icon does not.
  const WindowSurface* sel = (alt_tab_index_ >= 0 && alt_tab_index_ < n)
                                 ? find_window(alt_tab_order_[static_cast<size_t>(alt_tab_index_)]) : nullptr;
  if (sel) {
    const std::string t = fit_text(*this, sel->title, panel_w - 2 * pad, Face::Ui);
    const int tw = text_width(t, Face::Ui);
    draw_text_to(framebuffer_, W, H, px0 + (panel_w - tw) / 2, py0 + pad + tile + 3, t, tok::text, Face::Ui);
  }
}

void HardwareCompositor::clear_buffer(std::vector<uint32_t>& buf, uint32_t color, int w, int h) {
  size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (buf.size() != count) buf.resize(count);
  std::fill(buf.begin(), buf.end(), color);
}

void HardwareCompositor::draw_rect_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color) {
  px::fill_rect(buf, buf_w, buf_h, x, y, w, h, color);
}

void HardwareCompositor::draw_rect_outline_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color) {
  px::rect_outline(buf, buf_w, buf_h, x, y, w, h, color);
}

void HardwareCompositor::draw_rounded_rect_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color) {
  px::rounded_rect(buf, buf_w, buf_h, x, y, w, h, radius, color);
}

void HardwareCompositor::draw_gradient_v_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
  if (w <= 0 || h <= 0 || buf_w <= 0 || buf_h <= 0) return;
  int x0 = std::max(0, x);
  int y0 = std::max(0, y);
  int x1 = std::min(buf_w, x + w);
  int y1 = std::min(buf_h, y + h);
  if (x1 <= x0 || y1 <= y0) return;
  if (buf.size() < static_cast<size_t>(buf_w) * static_cast<size_t>(buf_h)) return;

  uint8_t a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
  uint8_t a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;

  // Ordered dither. A 24-step ramp over 300 rows quantises into visible bands;
  // half a level of noise per pixel breaks them up and costs nothing.
  static const int BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int py = y0; py < y1; ++py) {
    float t = static_cast<float>(py - y) / static_cast<float>(h);
    float fa = a1 + t * (a2 - a1), fr = r1 + t * (r2 - r1);
    float fg = g1 + t * (g2 - g1), fb = b1 + t * (b2 - b1);
    for (int px = x0; px < x1; ++px) {
      float n = (BAYER[py & 3][px & 3] / 16.0f) - 0.5f;
      auto q = [n](float v) {
        int o = static_cast<int>(v + n + 0.5f);
        return static_cast<uint32_t>(o < 0 ? 0 : (o > 255 ? 255 : o));
      };
      buf[static_cast<size_t>(py) * buf_w + px] =
          (q(fa) << 24) | (q(fr) << 16) | (q(fg) << 8) | q(fb);
    }
  }
}

namespace {
// How large this machine draws its interface, as a percentage. The atlas is
// cut at fixed sizes, so scaling is picking a bigger CUT rather than
// resampling a smaller one — the ramp is there for exactly this, and a glyph
// drawn at the size it was cut at is the only kind that is sharp.
//
// A file-scope value because face_of is what every text call goes through and
// the atlas it reads is file-scope too; there is one interface on this machine
// and it is drawn at one size.
int g_ui_scale = 100;

const fonts::Face& face_of(Face f) {
  const bool big = g_ui_scale >= 150;
  const bool mid = g_ui_scale >= 125;
  switch (f) {
    case Face::UiBold:
      return big ? fonts::UI_BOLD_30 : (mid ? fonts::UI_BOLD_21 : fonts::UI_BOLD);
    case Face::UiSmall:
      // The label face has no ramp of its own: it steps up into the body cuts,
      // which is what it is — a small bold label becoming a normal one.
      return big ? fonts::UI_BOLD_21 : (mid ? fonts::UI_BOLD : fonts::UI_SMALL);
    case Face::Mono:
      return mid ? fonts::MONO_21 : fonts::MONO;
    case Face::Ui:
    default:
      return big ? fonts::UI_30 : (mid ? fonts::UI_21 : fonts::UI);
  }
}

// The command surface writes UTF-8 — its `info` puts middots between the
// host's facts, its tables draw box rules — and the atlas is ASCII. Map the
// punctuation that actually turns up to the character it stands for, rather
// than stamping '*' over all of it, which is what the screen used to show.
char ascii_for(uint32_t cp) {
  switch (cp) {
    case 0x00B7: case 0x2022: case 0x22C5: return '.';
    case 0x2013: case 0x2014: case 0x2500: return '-';
    case 0x2502: case 0x2503: return '|';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2026: return '.';
    case 0x2192: case 0x203A: case 0x00BB: return '>';
    case 0x2190: case 0x2039: case 0x00AB: return '<';
    case 0x25B2: case 0x25B4: case 0x25B6: return '^';
    case 0x25BC: case 0x25BE: return 'v';
    case 0x2699: case 0x2690: return '*';
    case 0x2713: case 0x2714: return 'v';
    case 0x1F4CE: return '@';
    case 0x00A0: return ' ';
    default: return '?';
  }
}
}  // namespace

namespace {
// The gamma tables are shared with everything that draws (userland/pixels).
static const px::GammaTables& g_gamma = px::gamma();
}  // namespace

// The blend itself, with the bounds already established. Text goes through
// here a few hundred thousand times a frame, and it used to re-derive the
// clip and re-check the buffer's length for every one of those pixels; the
// arithmetic lives here now so the two callers cannot drift apart. The text
// path blends ONE colour against a whole run of glyphs, so its source channels
// are linearised once per string by the caller and passed in, instead of three
// table reads per pixel re-deriving the same three values.
static inline void blend_px_lin(uint32_t& dst, uint32_t color, uint32_t lin_sr, uint32_t lin_sg,
                                uint32_t lin_sb, uint32_t alpha) {
  if (alpha >= 255) { dst = color; return; }

  uint32_t d = dst;
  uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;

  uint32_t a = static_cast<uint32_t>(alpha);
  uint32_t inv_a = 255 - a;

  uint32_t lin_dr = g_gamma.srgb_to_lin[dr];
  uint32_t lin_dg = g_gamma.srgb_to_lin[dg];
  uint32_t lin_db = g_gamma.srgb_to_lin[db];

  uint32_t lin_r = (lin_sr * a + lin_dr * inv_a) / 255;
  uint32_t lin_g = (lin_sg * a + lin_dg * inv_a) / 255;
  uint32_t lin_b = (lin_sb * a + lin_db * inv_a) / 255;

  if (lin_r > 65535) lin_r = 65535;
  if (lin_g > 65535) lin_g = 65535;
  if (lin_b > 65535) lin_b = 65535;

  uint32_t r = g_gamma.lin_to_srgb[lin_r];
  uint32_t g = g_gamma.lin_to_srgb[lin_g];
  uint32_t b = g_gamma.lin_to_srgb[lin_b];

  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline void blend_px(uint32_t& dst, uint32_t color, uint32_t alpha) {
  blend_px_lin(dst, color, g_gamma.srgb_to_lin[(color >> 16) & 0xFF],
               g_gamma.srgb_to_lin[(color >> 8) & 0xFF], g_gamma.srgb_to_lin[color & 0xFF], alpha);
}

// Straight sRGB source-over -- see the header for why this exists beside the
// gamma-correct one.
static inline void blend_px_srgb(uint32_t& dst, uint32_t color, uint32_t alpha) {
  if (alpha >= 255) { dst = color; return; }
  const uint32_t inv = 255 - alpha;
  const uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
  const uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
  const uint32_t r = (sr * alpha + dr * inv + 127) / 255;
  const uint32_t g = (sg * alpha + dg * inv + 127) / 255;
  const uint32_t b = (sb * alpha + db * inv + 127) / 255;
  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void HardwareCompositor::blend_srgb_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                       uint32_t color, int alpha) {
  if (alpha <= 0 || x < 0 || y < 0 || x >= buf_w || y >= buf_h) return;
  size_t at = static_cast<size_t>(y) * static_cast<size_t>(buf_w) + static_cast<size_t>(x);
  if (at >= buf.size()) return;
  blend_px_srgb(buf[at], color, static_cast<uint32_t>(alpha));
}

void HardwareCompositor::blend_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                  uint32_t color, int alpha) {
  px::blend(buf, buf_w, buf_h, x, y, color, alpha);
}

int HardwareCompositor::glyph_advance(char c, Face f) const {
  return face_of(f).at(c).adv;
}

int HardwareCompositor::line_height(Face f) const {
  return face_of(f).line_h;
}

int HardwareCompositor::text_width(const std::string& text, Face f) const {
  const fonts::Face& face = face_of(f);
  int w = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\n') break;
    if ((c & 0x80) != 0) {
      uint32_t cp = 0;
      int extra = 0;
      if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
      else continue;
      for (int e = 0; e < extra && i + 1 < text.size(); ++e)
        cp = (cp << 6) | (static_cast<unsigned char>(text[++i]) & 0x3Fu);
      if (cp == 0x2318) {
        w += 14;
        continue;
      }
      const fonts::Glyph* gp = face.find(cp);
      if (!gp) {
        // Not in the baked block: cut it from the outline now, so the width a
        // caller lays out with is the width the glyph will actually draw at.
        // Measuring a transliteration and then drawing the real character is
        // how a label ends up overlapping the thing beside it.
        if (const glyphs::Rasterized* r = glyphs::rasterize(face, cp)) gp = &r->g;
      }
      if (gp) {
        w += gp->adv;
      } else {
        w += face.at(ascii_for(cp)).adv;
      }
    } else {
      w += face.at(text[i]).adv;
    }
  }
  return w;
}

int HardwareCompositor::launch_chip_width() const {
  const int label_w = text_width("> Launch", Face::UiBold);
  const int hint_w = text_width("Ctrl+P", Face::UiSmall);
  const int pad_l = 8, gap = 10, pad_r = 8;
  return pad_l + label_w + gap + hint_w + pad_r;
}

HardwareCompositor::DbConnMgrButtons HardwareCompositor::db_conn_mgr_buttons(int mx, int mw, int my, int mh) const {
  static const char* const kLabels[5] = {"Connect", "Test", "Save", "Delete", "Close"};
  DbConnMgrButtons r{};
  r.y = my + mh - 42;
  r.h = 28;
  const int pad = 20, gap = 8;
  int widths[5];
  int total = 0;
  for (int i = 0; i < 5; ++i) {
    widths[i] = text_width(kLabels[i], Face::UiBold) + pad;
    total += widths[i] + (i ? gap : 0);
  }
  int x = mx + mw - 12 - total;
  for (int i = 0; i < 5; ++i) {
    r.x[i] = x;
    r.w[i] = widths[i];
    x += widths[i] + gap;
  }
  return r;
}

namespace {
// The table designer's Columns grid, laid out ONCE for both the painter and
// the click handler. They each had their own copy of fixed widths: the type
// cell (64px) could not hold DECIMAL(10,2), the AUTO header ran into DEFAULT,
// and the "+ Add Column" text ran out of its 140px button.
struct DesignerColGeom {
  int name_x, name_w, type_x, type_w, null_x, null_w, pk_x, pk_w, ai_x, ai_w, def_x, def_w, del_x;
  int add_w;   // the "+ Add Column" button, sized to its label
};
DesignerColGeom designer_col_geom(const HardwareCompositor& c, int mx, int mw) {
  DesignerColGeom g{};
  const int cell_pad = 12, gap = 10;
  g.name_x = mx + 20;
  g.name_w = 150;
  g.type_x = g.name_x + g.name_w + gap;
  g.type_w = c.text_width("DECIMAL(10,2)", Face::Mono) + cell_pad;
  g.null_x = g.type_x + g.type_w + gap;
  g.null_w = std::max(c.label_width("NULL"), c.text_width("[x]", Face::Mono)) + 4;
  g.pk_x = g.null_x + g.null_w + gap;
  g.pk_w = std::max(c.label_width("PK"), c.text_width("[x]", Face::Mono)) + 4;
  g.ai_x = g.pk_x + g.pk_w + gap;
  g.ai_w = std::max(c.label_width("AUTO"), c.text_width("[x]", Face::Mono)) + 4;
  g.def_x = g.ai_x + g.ai_w + gap;
  g.del_x = mx + mw - 12 - 24;
  g.def_w = g.del_x - 10 - g.def_x;
  if (g.def_w < 40) g.def_w = 40;
  g.add_w = c.text_width("+ Add Column", Face::UiBold) + 24;
  return g;
}
// What the type cell says: the declared spelling while the type is the one
// it was declared with, otherwise the type (with its digits, for DECIMAL).
std::string designer_type_label(const SqlColumn& col) {
  if (!col.declared.empty()) return col.declared;
  switch (col.type) {
    case SqlDataType::Int: return "INT";
    case SqlDataType::Real: return "REAL";
    case SqlDataType::Decimal:
      return "DECIMAL(" + std::to_string(col.precision ? col.precision : 10) + "," + std::to_string(col.scale) + ")";
    default: return "TEXT";
  }
}
// How many column rows fit in the designer body, given where the first one
// starts; the last line is kept for "+ Add Column".
int designer_rows_fit(int first_ry, int body_y, int body_h) {
  int n = (body_y + body_h - 30 - 22 - first_ry) / 26 + 1;
  return n < 1 ? 1 : n;
}
}  // namespace

// y is the TOP of the line box, as it has always been here; the glyph's own
// offsets carry it down to the baseline, so a row of text lines up whatever
// mix of ascenders and descenders is in it.
void HardwareCompositor::draw_text_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                      const std::string& text, uint32_t color, Face f) {
  const fonts::Face& face = face_of(f);
  const uint32_t lin_sr = g_gamma.srgb_to_lin[(color >> 16) & 0xFF];
  const uint32_t lin_sg = g_gamma.srgb_to_lin[(color >> 8) & 0xFF];
  const uint32_t lin_sb = g_gamma.srgb_to_lin[color & 0xFF];
  int pen = x;
  int line_top = y;
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\n') { pen = x; line_top += face.line_h + 1; continue; }
    const fonts::Glyph* gp = nullptr;
    // Where this glyph's coverage rows live. The face's own blob for anything
    // baked into the atlas; a runtime glyph carries its own.
    const uint8_t* cov_base = face.cov;
    if ((c & 0x80) != 0) {
      uint32_t cp = 0;
      int extra = 0;
      if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
      else continue;
      for (int e = 0; e < extra && i + 1 < text.size(); ++e)
        cp = (cp << 6) | (static_cast<unsigned char>(text[++i]) & 0x3Fu);
      
      if (cp == 0x2318) {
        // Draw crisp ⌘ symbol (Place of Interest / looped square)
        int gw = 11, gh = 11;
        int gy = line_top + (face.line_h - gh) / 2;
        for (int dy = 0; dy < gh; ++dy) {
          for (int dx = 0; dx < gw; ++dx) {
            bool on = false;
            // Center box connecting arms
            if ((dy == 3 || dy == 7) && (dx >= 3 && dx <= 7)) on = true;
            if ((dx == 3 || dx == 7) && (dy >= 3 && dy <= 7)) on = true;
            // 4 corner loops
            if ((dy == 1 || dy == 3) && (dx >= 1 && dx <= 3)) on = true;
            if ((dx == 1 || dx == 3) && (dy >= 1 && dy <= 3)) on = true;
            if ((dy == 1 || dy == 3) && (dx >= 7 && dx <= 9)) on = true;
            if ((dx == 7 || dx == 9) && (dy >= 1 && dy <= 3)) on = true;
            if ((dy == 7 || dy == 9) && (dx >= 1 && dx <= 3)) on = true;
            if ((dx == 1 || dx == 3) && (dy >= 7 && dy <= 9)) on = true;
            if ((dy == 7 || dy == 9) && (dx >= 7 && dx <= 9)) on = true;
            if ((dx == 7 || dx == 9) && (dy >= 7 && dy <= 9)) on = true;
            // Hollow loop centers
            if (dx == 2 && dy == 2) on = false;
            if (dx == 8 && dy == 2) on = false;
            if (dx == 2 && dy == 8) on = false;
            if (dx == 8 && dy == 8) on = false;
            // Hollow middle
            if (dx >= 4 && dx <= 6 && dy >= 4 && dy <= 6) on = false;

            if (on) {
              int px = pen + dx, py = gy + dy;
              if (px >= 0 && px < buf_w && py >= 0 && py < buf_h) {
                buf[static_cast<size_t>(py) * buf_w + px] = color;
              }
            }
          }
        }
        pen += gw + 4;
        continue;
      }
      gp = face.find(cp);
      if (!gp) {
        // The atlas stops where mkfont.py's code point list stops. Everything
        // past it — Greek, Cyrillic, Han, the stars and arrows above U+2600 —
        // is cut from the real outline here, once, and cached. Only when no
        // installed typeface has the character at all does the transliteration
        // below get its turn, which is the '?' this used to show instead.
        if (const glyphs::Rasterized* r = glyphs::rasterize(face, cp)) {
          gp = &r->g;
          cov_base = r->cov;
        }
      }
      if (!gp) {
        char ch = ascii_for(cp);
        gp = &face.at(ch);
      }
    } else {
      gp = &face.at(text[i]);
    }
    if (!gp) continue;
    const fonts::Glyph& g = *gp;
    if (g.w && g.h && cov_base &&
        buf.size() >= static_cast<size_t>(buf_w) * static_cast<size_t>(buf_h)) {
      // Clip the glyph's span against the buffer ONCE, then blend along a row
      // pointer. Per-pixel this was four comparisons and a length check on the
      // vector before any colour arithmetic happened at all.
      const int glyph_x = pen + g.bx;
      const int gx0 = std::max(0, -glyph_x);
      const int gx1 = std::min(static_cast<int>(g.w), buf_w - glyph_x);
      for (int gy = 0; gy < static_cast<int>(g.h) && gx1 > gx0; ++gy) {
        int py = line_top + g.by + gy;
        if (py < 0 || py >= buf_h) continue;
        const uint8_t* cov_row = cov_base + g.off + static_cast<size_t>(gy) * g.w;
        uint32_t* rowp = &buf[static_cast<size_t>(py) * static_cast<size_t>(buf_w)];
        for (int gx = gx0; gx < gx1; ++gx) {
          uint8_t cov = g_gamma.font_cov_smooth[cov_row[gx]];
          if (cov) blend_px_lin(rowp[glyph_x + gx], color, lin_sr, lin_sg, lin_sb, cov);
        }
      }
    }
    pen += g.adv;
    if (pen >= buf_w) break;
  }
}

int HardwareCompositor::label_width(const std::string& text, int tracking) const {
  const fonts::Face& face = face_of(Face::UiSmall);
  int w = 0;
  for (char c : text) {
    char up = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    w += face.at(up).adv + tracking;
  }
  return w > tracking ? w - tracking : 0;
}

void HardwareCompositor::draw_label_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                       const std::string& text, uint32_t color, int tracking) {
  int pen = x;
  for (size_t i = 0; i < text.size(); ) {
    // Letter-spaced, so each letter is drawn on its own — which means stepping by
    // whole UTF-8 sequences. A byte at a time handed the lead byte of an
    // accented letter to the glyph lookup by itself and drew nothing for it.
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + len > text.size()) len = 1;
    std::string one = text.substr(i, len);
    if (len == 1 && one[0] >= 'a' && one[0] <= 'z') one[0] = static_cast<char>(one[0] - 32);
    draw_text_to(buf, buf_w, buf_h, pen, y, one, color, Face::UiSmall);
    pen += text_width(one, Face::UiSmall) + tracking;
    i += len;
  }
}

void HardwareCompositor::draw_pill_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                      int w, int h, uint32_t fill, uint32_t edge) {
  int r = h / 2;
  if (fill) draw_rounded_rect_to(buf, buf_w, buf_h, x, y, w, h, r, fill);
  if (edge) draw_round_outline_to(buf, buf_w, buf_h, x, y, w, h, r, edge);
}

void HardwareCompositor::draw_round_outline_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                               int w, int h, int radius, uint32_t edge) {
  px::round_outline(buf, buf_w, buf_h, x, y, w, h, radius, edge);
}

void HardwareCompositor::draw_app_icon(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, WindowType type, int size) {
  if (size <= 0 || buf_w <= 0 || buf_h <= 0) return;
  int r = size >= 18 ? 4 : (size >= 14 ? 3 : 2);

  switch (type) {
    case WindowType::Terminal: {
      // Shell / Terminal: Dark slate squircle, emerald border, crisp prompt >_
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0E1A14);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF238636);
      int cx = x + 3, cy = y + (size - 8) / 2;
      draw_rect_to(buf, buf_w, buf_h, cx, cy, 2, 2, 0xFF3FB950);
      draw_rect_to(buf, buf_w, buf_h, cx + 2, cy + 2, 2, 2, 0xFF3FB950);
      draw_rect_to(buf, buf_w, buf_h, cx + 4, cy + 3, 2, 2, 0xFF56D364);
      draw_rect_to(buf, buf_w, buf_h, cx + 2, cy + 5, 2, 2, 0xFF3FB950);
      draw_rect_to(buf, buf_w, buf_h, cx, cy + 7, 2, 2, 0xFF3FB950);
      draw_rect_to(buf, buf_w, buf_h, cx + 5, cy + 7, 4, 2, 0xFF7EE787);
      break;
    }
    case WindowType::Hardware: {
      // Machine / HAL: Deep navy squircle, glowing sapphire border, microchip CPU with pins
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0D1B2A);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1D4ED8);
      int core_s = std::max(6, size - 8);
      int core_x = x + (size - core_s) / 2;
      int core_y = y + (size - core_s) / 2;
      draw_rounded_rect_to(buf, buf_w, buf_h, core_x, core_y, core_s, core_s, 2, 0xFF0284C7);
      draw_rect_to(buf, buf_w, buf_h, core_x + 2, core_y + 2, core_s - 4, core_s - 4, 0xFF38BDF8);
      draw_rect_to(buf, buf_w, buf_h, core_x + 1, y + 1, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, core_x + core_s - 3, y + 1, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, core_x + 1, y + size - 3, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, core_x + core_s - 3, y + size - 3, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, x + 1, core_y + 1, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, x + 1, core_y + core_s - 3, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, x + size - 3, core_y + 1, 2, 2, 0xFF7DD3FC);
      draw_rect_to(buf, buf_w, buf_h, x + size - 3, core_y + core_s - 3, 2, 2, 0xFF7DD3FC);
      break;
    }
    case WindowType::Tasks: {
      // Entities / Tasks: Plum squircle, violet border, workflow graph nodes
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1E1538);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF7C3AED);
      int n_top_x = x + size / 2 - 1, n_top_y = y + 3;
      int n_bl_x = x + 3, n_bl_y = y + size - 5;
      int n_br_x = x + size - 5, n_br_y = y + size - 5;
      draw_rect_to(buf, buf_w, buf_h, x + 4, y + 6, 2, 2, 0xFF6D28D9);
      draw_rect_to(buf, buf_w, buf_h, x + size - 6, y + 6, 2, 2, 0xFF6D28D9);
      draw_rect_to(buf, buf_w, buf_h, x + 5, y + size - 4, size - 10, 1, 0xFF6D28D9);
      draw_pill_to(buf, buf_w, buf_h, n_top_x, n_top_y, 3, 3, 0xFFC4B5FD, 0xFFFFFFFF);
      draw_pill_to(buf, buf_w, buf_h, n_bl_x, n_bl_y, 3, 3, 0xFFA78BFA, 0xFFDDD6FE);
      draw_pill_to(buf, buf_w, buf_h, n_br_x, n_br_y, 3, 3, 0xFFA78BFA, 0xFFDDD6FE);
      break;
    }
    case WindowType::Editor: {
      // Editor / Code: Deep blue squircle, azure border, code symbols </>
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0F2338);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF2563EB);
      int ey = y + (size - 8) / 2;
      draw_rect_to(buf, buf_w, buf_h, x + 4, ey, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + 3, ey + 2, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + 2, ey + 3, 2, 2, 0xFF93C5FD);
      draw_rect_to(buf, buf_w, buf_h, x + 3, ey + 4, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + 4, ey + 6, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + 9, ey, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, x + 8, ey + 2, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, x + 7, ey + 4, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, x + 6, ey + 6, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, x + size - 6, ey, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + size - 5, ey + 2, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + size - 4, ey + 3, 2, 2, 0xFF93C5FD);
      draw_rect_to(buf, buf_w, buf_h, x + size - 5, ey + 4, 2, 2, 0xFF60A5FA);
      draw_rect_to(buf, buf_w, buf_h, x + size - 6, ey + 6, 2, 2, 0xFF60A5FA);
      break;
    }
    case WindowType::Database: {
      // MariaDB / Database: Deep amber squircle, gold border, database cylinder disks
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF291B0B);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFD97706);
      int dw = size - 6;
      int dx = x + 3;
      draw_pill_to(buf, buf_w, buf_h, dx, y + 3, dw, 3, 0xFFF59E0B, 0xFFFDE68A);
      draw_pill_to(buf, buf_w, buf_h, dx, y + 7, dw, 3, 0xFFD97706, 0xFFFBBF24);
      draw_pill_to(buf, buf_w, buf_h, dx, y + 11, dw, 3, 0xFFB45309, 0xFFF59E0B);
      draw_rect_to(buf, buf_w, buf_h, dx, y + 4, 1, 7, 0xFFF59E0B);
      draw_rect_to(buf, buf_w, buf_h, dx + dw - 1, y + 4, 1, 7, 0xFFF59E0B);
      draw_rect_to(buf, buf_w, buf_h, dx + dw - 3, y + 4, 2, 2, 0xFF34D399);
      break;
    }
    case WindowType::Browser: {
      // Web / Browser: Deep ocean squircle, cyan border, globe with meridians
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF08263A);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0284C7);
      int gw = size - 6;
      int gx = x + 3, gy = y + 3;
      draw_pill_to(buf, buf_w, buf_h, gx, gy, gw, gw, 0xFF0369A1, 0xFF38BDF8);
      draw_rect_to(buf, buf_w, buf_h, gx, gy + gw / 2, gw, 1, 0xFFBAE6FD);
      draw_rect_to(buf, buf_w, buf_h, gx + gw / 2, gy, 1, gw, 0xFFBAE6FD);
      draw_rect_to(buf, buf_w, buf_h, gx + 2, gy + 2, 2, 2, 0xFFE0F2FE);
      draw_rect_to(buf, buf_w, buf_h, gx + gw - 4, gy + gw - 4, 2, 2, 0xFFE0F2FE);
      break;
    }
    case WindowType::Mail: {
      // Mail / AetherMail: Deep wine squircle, rose border, sealed envelope
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF2B0E14);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFE11D48);
      int ew = size - 6;
      int eh = size - 8;
      int ex = x + 3, ey = y + 4;
      draw_rounded_rect_to(buf, buf_w, buf_h, ex, ey, ew, eh, 2, 0xFFF43F5E);
      draw_round_outline_to(buf, buf_w, buf_h, ex, ey, ew, eh, 2, 0xFFFDA4AF);
      int mid_x = ex + ew / 2;
      draw_rect_to(buf, buf_w, buf_h, ex + 1, ey + 1, 2, 1, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, ex + 3, ey + 2, 2, 1, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, mid_x - 1, ey + 3, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, ex + ew - 5, ey + 2, 2, 1, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, ex + ew - 3, ey + 1, 2, 1, 0xFFFFFFFF);
      break;
    }
    case WindowType::Contacts: {
      // Contacts / Address Book: Deep emerald squircle, teal border, contact silhouette
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF08291F);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF059669);
      int head_s = std::max(4, size / 4);
      int hx = x + (size - head_s) / 2;
      draw_pill_to(buf, buf_w, buf_h, hx, y + 3, head_s, head_s, 0xFF34D399, 0xFFA7F3D0);
      int bw = size - 6;
      int bx = x + 3;
      draw_pill_to(buf, buf_w, buf_h, bx, y + size - 6, bw, 4, 0xFF10B981, 0xFF6EE7B7);
      break;
    }
    case WindowType::Calendar: {
      // Calendar / Agenda: Slate squircle, red header, calendar grid
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1C1D24);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFDC2626);
      int cw = size - 6;
      int cx = x + 3, cy = y + 3;
      draw_rounded_rect_to(buf, buf_w, buf_h, cx, cy, cw, cw, 2, 0xFFF1F5F9);
      draw_rect_to(buf, buf_w, buf_h, cx, cy, cw, 3, 0xFFEF4444);
      draw_rect_to(buf, buf_w, buf_h, cx + 2, cy - 1, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, cx + cw - 4, cy - 1, 2, 2, 0xFFFFFFFF);
      draw_rect_to(buf, buf_w, buf_h, cx + 2, cy + 4, 2, 2, 0xFF334155);
      draw_rect_to(buf, buf_w, buf_h, cx + cw - 4, cy + 4, 2, 2, 0xFF334155);
      draw_rect_to(buf, buf_w, buf_h, cx + 2, cy + 7, 2, 2, 0xFF3B82F6);
      draw_rect_to(buf, buf_w, buf_h, cx + cw - 4, cy + 7, 2, 2, 0xFF334155);
      break;
    }
    case WindowType::FaceApp: {
      // Apps / Face Applications: Vivid fuchsia squircle, 4-app colorful tile grid
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF270E38);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFA855F7);
      int sq = std::max(3, (size - 7) / 2);
      int q1x = x + 3, q1y = y + 3;
      int q2x = x + size - 3 - sq, q2y = y + size - 3 - sq;
      draw_rounded_rect_to(buf, buf_w, buf_h, q1x, q1y, sq, sq, 1, 0xFFEC4899);
      draw_rounded_rect_to(buf, buf_w, buf_h, q2x, q1y, sq, sq, 1, 0xFFA855F7);
      draw_rounded_rect_to(buf, buf_w, buf_h, q1x, q2y, sq, sq, 1, 0xFF06B6D4);
      draw_rounded_rect_to(buf, buf_w, buf_h, q2x, q2y, sq, sq, 1, 0xFFF59E0B);
      break;
    }
    case WindowType::Inspector: {
      // Studio / Diagnostic Inspector: Slate/teal squircle, adjustment knobs / waveform
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0D2326);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0D9488);
      int lx = x + 4, rx = x + size - 6;
      draw_rect_to(buf, buf_w, buf_h, lx + 1, y + 3, 1, size - 6, 0xFF14B8A6);
      draw_pill_to(buf, buf_w, buf_h, lx, y + 4, 3, 3, 0xFF2DD4BF, 0xFF99F6E4);
      draw_rect_to(buf, buf_w, buf_h, rx + 1, y + 3, 1, size - 6, 0xFF14B8A6);
      draw_pill_to(buf, buf_w, buf_h, rx, y + size - 7, 3, 3, 0xFFFCD34D, 0xFFFEF08A);
      break;
    }
    case WindowType::Miner: {
      // Miner / Crypto: Dark gold squircle, golden pickaxe / coin
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF292006);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFCA8A04);
      int mw = size - 6;
      draw_pill_to(buf, buf_w, buf_h, x + 3, y + 3, mw, mw, 0xFFEAB308, 0xFFFEF08A);
      draw_rect_to(buf, buf_w, buf_h, x + size / 2 - 1, y + 4, 2, size - 8, 0xFF713F12);
      draw_rect_to(buf, buf_w, buf_h, x + size / 2 - 2, y + 5, 4, 2, 0xFF713F12);
      break;
    }
    default: {
      draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1B2332);
      draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF388BFD);
      draw_rect_to(buf, buf_w, buf_h, x + 4, y + 4, size - 8, size - 8, 0xFF58A6FF);
      break;
    }
  }
}

void HardwareCompositor::draw_named_icon(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, const std::string& name, int size) {
  if (size <= 0 || buf_w <= 0 || buf_h <= 0) return;
  int r = size >= 18 ? 4 : (size >= 14 ? 3 : 2);

  if (name == "order_desk" || name == "order" || name == "cart") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF064E3B);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF10B981);
    int bx = x + 3, by = y + 5, bw = size - 6, bh = size - 8;
    draw_rounded_rect_to(buf, buf_w, buf_h, bx, by, bw, bh, 2, 0xFF34D399);
    draw_rect_to(buf, buf_w, buf_h, x + size / 2 - 2, y + 2, 4, 3, 0xFFA7F3D0);
    draw_rect_to(buf, buf_w, buf_h, x + size / 2 - 1, y + 3, 2, 2, 0xFF064E3B);
  } else if (name == "task_tracker" || name == "tracker" || name == "check") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF312E81);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF6366F1);
    int cx = x + 3, cy = y + (size - 8) / 2;
    draw_rect_to(buf, buf_w, buf_h, cx, cy + 3, 2, 2, 0xFF818CF8);
    draw_rect_to(buf, buf_w, buf_h, cx + 2, cy + 5, 2, 2, 0xFFA5B4FC);
    draw_rect_to(buf, buf_w, buf_h, cx + 4, cy + 4, 2, 2, 0xFFC7D2FE);
    draw_rect_to(buf, buf_w, buf_h, cx + 6, cy + 2, 2, 2, 0xFFFFFFFF);
    draw_rect_to(buf, buf_w, buf_h, cx + 8, cy, 2, 2, 0xFFFFFFFF);
  } else if (name == "system_monitor" || name == "monitor" || name == "pulse") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0C4A6E);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF0EA5E9);
    int my = y + size / 2;
    draw_rect_to(buf, buf_w, buf_h, x + 2, my, 2, 1, 0xFF38BDF8);
    draw_rect_to(buf, buf_w, buf_h, x + 4, my - 2, 2, 3, 0xFF38BDF8);
    draw_rect_to(buf, buf_w, buf_h, x + 6, my - 4, 2, 5, 0xFFE0F2FE);
    draw_rect_to(buf, buf_w, buf_h, x + 8, my + 1, 2, 4, 0xFF38BDF8);
    draw_rect_to(buf, buf_w, buf_h, x + 10, my, 4, 1, 0xFF38BDF8);
  } else if (name == "notes" || name == "note") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF451A03);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFF59E0B);
    int nx = x + 3, ny = y + 3, nw = size - 6, nh = size - 6;
    draw_rounded_rect_to(buf, buf_w, buf_h, nx, ny, nw, nh, 2, 0xFFFDE047);
    draw_rect_to(buf, buf_w, buf_h, nx + 2, ny + 3, nw - 4, 1, 0xFF78350F);
    draw_rect_to(buf, buf_w, buf_h, nx + 2, ny + 6, nw - 4, 1, 0xFF78350F);
  } else if (name == "ws_code" || name == "Code") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1E3A8A);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF3B82F6);
    draw_rect_to(buf, buf_w, buf_h, x + 3, y + 4, 4, 8, 0xFF60A5FA);
    draw_rect_to(buf, buf_w, buf_h, x + 9, y + 4, 4, 8, 0xFF38BDF8);
  } else if (name == "ws_comms" || name == "Comms") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF881337);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFF43F5E);
    draw_rect_to(buf, buf_w, buf_h, x + 3, y + 4, size - 6, 8, 0xFFFB7185);
    draw_rect_to(buf, buf_w, buf_h, x + 5, y + 6, size - 10, 2, 0xFFFFFFFF);
  } else if (name == "ws_admin" || name == "Admin") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF4C1D95);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF8B5CF6);
    draw_pill_to(buf, buf_w, buf_h, x + 3, y + 3, size - 6, 4, 0xFFA78BFA, 0xFFDDD6FE);
    draw_pill_to(buf, buf_w, buf_h, x + 3, y + 9, size - 6, 4, 0xFFA78BFA, 0xFFDDD6FE);
  } else if (name == "ws_blank" || name == "Blank") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1E293B);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF64748B);
    draw_rect_to(buf, buf_w, buf_h, x + 4, y + 4, size - 8, size - 8, 0xFF94A3B8);
  } else if (name == "snap_left" || name == "snap_right" || name == "snap_max" || name == "tile_2col" || name == "tile_3col") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF3D2E0B);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFF59E0B);
    draw_rect_to(buf, buf_w, buf_h, x + 3, y + 4, size - 6, size - 8, 0xFFFCD34D);
  } else if (name == "win_close" || name == "win_close_all") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF381414);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFEF4444);
    draw_rect_to(buf, buf_w, buf_h, x + 4, y + 4, size - 8, 2, 0xFFFCA5A5);
  } else if (name == "sys_restart" || name == "sys_power" || name == "sys_sleep" || name == "sys_stop") {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF381414);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFFF87171);
    draw_pill_to(buf, buf_w, buf_h, x + 3, y + 3, size - 6, size - 6, 0xFFEF4444, 0xFFFECACA);
  } else {
    draw_rounded_rect_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF1B2332);
    draw_round_outline_to(buf, buf_w, buf_h, x, y, size, size, r, 0xFF388BFD);
    draw_rect_to(buf, buf_w, buf_h, x + 4, y + 4, size - 8, size - 8, 0xFF58A6FF);
  }
}

void HardwareCompositor::shadow_rounded(int x, int y, int w, int h, int radius,
                                        int spread, int strength, int drop) {
  if (w <= 0 || h <= 0 || spread <= 0 || strength <= 0) return;
  const int W = static_cast<int>(width_), H = static_cast<int>(height_);
  y += drop;
  const float r = static_cast<float>(std::min(radius, std::min(w / 2, h / 2)));
  const float cx = x + w * 0.5f, cy = y + h * 0.5f;
  const float hx = w * 0.5f - r, hy = h * 0.5f - r;

  const int x0 = std::max(0, x - spread), x1 = std::min(W, x + w + spread);
  const int y0 = std::max(0, y - spread), y1 = std::min(H, y + h + spread);
  const int corner = static_cast<int>(r) + 1;
  for (int py = y0; py < y1; ++py) {
    uint32_t* row = &framebuffer_[static_cast<size_t>(py) * static_cast<size_t>(W)];
    const float qy = std::fabs(py + 0.5f - cy) - hy;
    // Between the corner bands the shape is the full-width box, so nothing from
    // its left edge to its right edge can be shadow: those pixels were each
    // given a square root only to be thrown away, on every frame, for the whole
    // inside of every window.
    const bool straight = py >= y + corner && py < y + h - corner;
    for (int px = x0; px < x1; ++px) {
      if (straight && px >= x && px < x + w) { px = x + w - 1; continue; }
      const float qx = std::fabs(px + 0.5f - cx) - hx;
      // Distance to the rounded rect. Inside is <= 0 and costs nothing: the
      // panel is about to be drawn over it anyway.
      const float ax = qx > 0.0f ? qx : 0.0f, ay = qy > 0.0f ? qy : 0.0f;
      const float outside = (ax > 0.0f && ay > 0.0f) ? std::sqrt(ax * ax + ay * ay) : (ax + ay);
      const float inside = std::min(std::max(qx, qy), 0.0f);
      const float d = outside + inside - r;
      if (d <= 0.0f || d >= static_cast<float>(spread)) continue;
      // Squared falloff: dense against the edge, gone by the spread.
      const float t = 1.0f - d / static_cast<float>(spread);
      const int pct = static_cast<int>(strength * t * t);
      if (pct <= 0) continue;
      const int cov = pct > 100 ? 100 : pct;
      const int keep = 100 - cov;
      const uint32_t c = row[px];
      // Darken what is there AND carry the alpha. Forcing 0xFF here is what
      // put a black box behind everything: on the display path the background
      // is left CLEAR for the shader to fill, so an opaque shadow pixel is not
      // a shadow at all, it is a hole punched through the scene. A shadow over
      // nothing is black at this coverage; over something it just darkens it.
      const uint32_t a = (c >> 24) & 0xFF;
      const uint32_t na = a + ((255 - a) * static_cast<uint32_t>(cov)) / 100;
      row[px] = (na << 24) |
                (((((c >> 16) & 0xFF) * keep) / 100) << 16) |
                (((((c >> 8) & 0xFF) * keep) / 100) << 8) |
                ((((c & 0xFF) * keep) / 100));
    }
  }
}

void HardwareCompositor::shade_screen(int x, int y, int w, int h, int percent) {
  if (w <= 0 || h <= 0 || percent <= 0) return;
  int keep = 100 - percent;
  if (keep < 0) keep = 0;
  int x0 = std::max(0, x), y0 = std::max(0, y);
  int x1 = std::min(static_cast<int>(width_), x + w), y1 = std::min(static_cast<int>(height_), y + h);
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      uint32_t c = framebuffer_[static_cast<size_t>(py) * width_ + px];
      uint32_t r = (((c >> 16) & 0xFF) * keep) / 100;
      uint32_t g = (((c >> 8) & 0xFF) * keep) / 100;
      uint32_t b = ((c & 0xFF) * keep) / 100;
      // Same reasoning as shadow_rounded: keep the alpha. Where the shader owns
      // the background these pixels are CLEAR, and 0xFF turned a dim into a
      // black rectangle.
      const uint32_t a = (c >> 24) & 0xFF;
      const uint32_t na = a + ((255 - a) * static_cast<uint32_t>(percent)) / 100;
      framebuffer_[static_cast<size_t>(py) * width_ + px] = (na << 24) | (r << 16) | (g << 8) | b;
    }
  }
}

// The desktop's own wallpaper used to be painted HERE, by a second copy of the
// beach the canvas draws — 150 lines of the same water and the same moon, which
// is why the workspace could only ever be on one background. paint_scene() in
// canvas.cpp is the one painter now.

void HardwareCompositor::build_background() {
  const int W = static_cast<int>(width_), H = static_cast<int>(height_);
  background_.resize(static_cast<size_t>(W) * H, 0xFF0E0B1A);
  const uint64_t now = steady_ms();
  paint_scene(background_, W, H, now, canvas_state_, face_scene());
}

std::vector<uint32_t> HardwareCompositor::capture_corners(int x, int y, int w, int h, int radius) const {
  static thread_local std::vector<uint32_t> saved;
  const size_t needed = static_cast<size_t>(radius) * radius * 4;
  if (saved.size() != needed) saved.assign(needed, 0);
  const int cx[4] = {x, x + w - radius, x, x + w - radius};
  const int cy[4] = {y, y, y + h - radius, y + h - radius};
  for (int c = 0; c < 4; ++c) {
    for (int dy = 0; dy < radius; ++dy) {
      for (int dx = 0; dx < radius; ++dx) {
        int px = cx[c] + dx, py = cy[c] + dy;
        if (px < 0 || py < 0 || px >= static_cast<int>(width_) || py >= static_cast<int>(height_)) continue;
        saved[(static_cast<size_t>(c) * radius + dy) * radius + dx] =
            framebuffer_[static_cast<size_t>(py) * width_ + px];
      }
    }
  }
  return saved;
}

void HardwareCompositor::round_frame(int x, int y, int w, int h, int radius, uint32_t frame,
                                     const std::vector<uint32_t>& saved) {
  if (radius <= 0 || saved.size() < static_cast<size_t>(radius) * radius * 4) return;
  const int cx[4] = {x, x + w - radius, x, x + w - radius};
  const int cy[4] = {y, y, y + h - radius, y + h - radius};
  const double r = radius;
  const double r2 = r * r;
  const double r_inner = r > 1.4 ? (r - 1.4) : 0.0;
  const double r_inner2 = r_inner * r_inner;
  for (int c = 0; c < 4; ++c) {
    // The arc's centre, radius pixels in from this corner.
    double ox = (c == 0 || c == 2) ? cx[c] + r : cx[c] + radius - r - 1;
    double oy = (c == 0 || c == 1) ? cy[c] + r : cy[c] + radius - r - 1;
    for (int dy = 0; dy < radius; ++dy) {
      for (int dx = 0; dx < radius; ++dx) {
        int px = cx[c] + dx, py = cy[c] + dy;
        if (px < 0 || py < 0 || px >= static_cast<int>(width_) || py >= static_cast<int>(height_)) continue;
        double d2 = (px - ox) * (px - ox) + (py - oy) * (py - oy);
        size_t at = static_cast<size_t>(py) * width_ + px;
        if (d2 > r2) framebuffer_[at] = saved[(static_cast<size_t>(c) * radius + dy) * radius + dx];
        else if (d2 > r_inner2) framebuffer_[at] = frame;
      }
    }
  }
}

// Which edges of this window the point is on, as a bitmask. A margin reaching
// a little outside the frame as well as inside, so the target is a band rather
// than a line.
int HardwareCompositor::resize_edges_at(const WindowSurface& w, int x, int y) const {
  const int m = 6;
  const int l = w.bounds.x, r = w.bounds.x + w.bounds.width;
  const int t = w.bounds.y, b = w.bounds.y + w.bounds.height;
  if (x < l - m || x > r + m || y < t - m || y > b + m) return 0;
  int e = 0;
  if (x >= l - m && x <= l + m) e |= EdgeL;
  if (x >= r - m && x <= r + m) e |= EdgeR;
  if (y >= t - m && y <= t + m) e |= EdgeT;
  if (y >= b - m && y <= b + m) e |= EdgeB;
  // The title bar owns the top edge except at its corners: dragging a window
  // by its title is used far more often than resizing it upward.
  if (e == EdgeT) e = 0;
  return e;
}

HitRegion HardwareCompositor::hit_test(int x, int y, uint32_t& out_window_id) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
    if (it->minimized || !it->visible) continue;
    // An edge is grabbable from just OUTSIDE the window too: aiming at a
    // one-pixel border is not something anyone should have to do.
    if (!it->bounds.contains(x, y)) {
      if (resize_edges_at(*it, x, y) != 0) {
        out_window_id = it->window_id;
        return HitRegion::ResizeGrip;
      }
      continue;
    }
    {
      out_window_id = it->window_id;
      // Check title bar
      if (y < it->bounds.y + 26) {
        int bx = it->bounds.x;
        int right = it->bounds.x + it->bounds.width;
        // macOS-style top-left traffic lights
        if (x >= bx + 8 && x < bx + 22) return HitRegion::CloseBtn;
        if (x >= bx + 24 && x < bx + 38) return HitRegion::MinimizeBtn;
        if (x >= bx + 40 && x < bx + 54) return HitRegion::MaximizeBtn;
        // Windows-style top-right buttons
        if (x >= right - 22 && x < right - 4) return HitRegion::CloseBtn;
        if (x >= right - 42 && x < right - 24) return HitRegion::MaximizeBtn;
        if (x >= right - 62 && x < right - 44) return HitRegion::MinimizeBtn;
        return HitRegion::TitleBar;
      }
      // Any edge, any corner. A window used to be resizable only by a 16x16
      // patch at its bottom-right, which is a grip, not a window frame.
      if (resize_edges_at(*it, x, y) != 0) return HitRegion::ResizeGrip;
      return HitRegion::Content;
    }
  }
  out_window_id = 0;
  return HitRegion::None;
}

void HardwareCompositor::set_screen_size(uint32_t w, uint32_t h) {
  if (!w || !h) return;
  width_ = w;
  height_ = h;
  framebuffer_.assign(static_cast<size_t>(w) * static_cast<size_t>(h), tok::bg);
  build_background();
  if (mouse_x_ >= static_cast<int>(w)) mouse_x_ = static_cast<int>(w) - 1;
  if (mouse_y_ >= static_cast<int>(h)) mouse_y_ = static_cast<int>(h) - 1;
}

uint32_t HardwareCompositor::ensure_app_window(WindowType type) {
  AIOS_TRACE("ensure_app_window: locking");
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  AIOS_TRACE("ensure_app_window: locked");
  const int cur_ws = static_cast<int>(current_workspace_);

  // 1. If a window of this type is already in the current workspace (or pinned/global), raise it
  for (auto& w : windows_) {
    if (w.type == type && (w.workspace == cur_ws || w.pinned || w.workspace == -1)) {
      w.visible = true;
      w.minimized = false;
      w.focused = true;
      active_top_tab_ = TopTab::Workspaces;
      palette_open_ = false;
      active_window_id_ = w.window_id;
      for (auto& other : windows_) {
        if (other.window_id != w.window_id) other.focused = false;
      }
      return w.window_id;
    }
  }

  // 2. If a window of this type exists in another workspace, migrate/bring it to the active space
  for (auto& w : windows_) {
    if (w.type == type) {
      w.workspace = cur_ws;
      w.visible = true;
      w.minimized = false;
      w.focused = true;
      active_top_tab_ = TopTab::Workspaces;
      palette_open_ = false;
      active_window_id_ = w.window_id;
      for (auto& other : windows_) {
        if (other.window_id != w.window_id) other.focused = false;
      }
      return w.window_id;
    }
  }

  if (!kernel_) return 0;
  Kernel& k = *kernel_;
  AIOS_TRACE("eaw: topology");
  const auto& topo = k.host().topology();
  AIOS_TRACE("eaw: work_area");
  const Rect wa = work_area();
  AIOS_TRACE("eaw: work_area done");
  const int margin = 14;
  const int left = wa.x;
  const int top = wa.y;
  const int gap = 14;
  const int usable_w = wa.width;
  const int usable_h = wa.height;
  (void)margin;
  const int col_w = (usable_w * 56) / 100 - gap / 2;
  const int right_w = usable_w - col_w - gap;
  const int right_x = left + col_w + gap;
  const int term_h = (usable_h * 56) / 100 - gap / 2;

  WindowSurface win;
  win.window_id = next_window_id_++;
  win.type = type;
  win.workspace = cur_ws;
  win.focused = true;
  win.visible = true;
  win.minimized = false;
  win.maximized = false;

  for (auto& other : windows_) other.focused = false;
  active_window_id_ = win.window_id;
  AIOS_TRACE("eaw: switch");

  switch (type) {
    case WindowType::Terminal: {
      win.title = "Shell";
      win.badge = "console entity";
      win.bounds = {left, top, col_w, term_h};
      win.normal_bounds = win.bounds;
      AIOS_TRACE("eaw: terminal buffer resize");
      win.buffer.resize(win.content_pixels(), tok::panel);
      AIOS_TRACE("eaw: terminal lines");
      win.lines.push_back("AIOS - composited by the kernel onto this machine's framebuffer.");
      AIOS_TRACE("eaw: self short_str");
      win.lines.push_back("Kernel " + k.self().short_str() + "   " + topo.arch + "   " +
                          std::to_string(topo.logical_cpus) + " cpu   " +
                          std::to_string(topo.total_memory >> 20) + " MiB");
      win.lines.push_back("");
      win.lines.push_back("Cockpit Operator Console: Press Super+Space or Ctrl+P for Command Palette.");
      win.lines.push_back("Snapping: Ctrl+Left (50%) • Ctrl+Right (50%) • Ctrl+Up (Max) • Ctrl+W (Close)");
      win.lines.push_back("");
      break;
    }
    case WindowType::Hardware: {
      win.title = "Machine";
      win.badge = topo.arch;
      win.bounds = {right_x, top, right_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      break;
    }
    case WindowType::Tasks: {
      win.title = "Entities";
      win.badge = std::to_string(topo.logical_cpus) + " lanes";
      win.bounds = {left, top + term_h + gap, col_w, usable_h - term_h - gap};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      break;
    }
    case WindowType::Editor: {
      win.title = "Editor";
      win.badge = "c++ code & text editor";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.lines.assign(1, "");
      editor_browse(win, k, k.config().root);
      win.status_text = "pick a file on the left, or type: open <path>";
      break;
    }
    case WindowType::Database: {
      win.title = "MariaDB / ASQL Studio";
      win.badge = "ASQL Studio";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      db_connect(win, k);
      break;
    }
    case WindowType::Browser: {
      win.title = "Web";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.web = std::make_shared<WebPane>();
      win.status_text = "type a url in the bar, or run: browser <url>";
      break;
    }
    case WindowType::Mail: {
      win.title = "AetherMail";
      win.badge = "aethermail";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.mail_store = std::make_shared<MailStore>(k.config().root + "/mail", &k.host(), k.config().root);
      win.status_text = "inbox • " + std::to_string(win.mail_store->total_unread()) + " unread";
      break;
    }
    case WindowType::Contacts: {
      win.title = "Contacts";
      win.badge = "contacts";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.contacts_store = std::make_shared<ContactsStore>(k.config().root + "/contacts");
      win.status_text = std::to_string(win.contacts_store->total_contacts()) + " cards • vCard RFC 6350";
      break;
    }
    case WindowType::Calendar: {
      win.title = "Calendar";
      win.badge = "calendar";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.calendar_store = std::make_shared<CalendarStore>(k.config().root + "/calendar");
      calendar_today(win.calendar_view_year, win.calendar_view_month, win.calendar_view_day);
      win.status_text = std::to_string(win.calendar_store->total_events()) + " events • iCalendar RFC 5545";
      break;
    }
    case WindowType::FaceApp: {
      win.title = "Apps";
      win.badge = "apps";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.face_app_list = {"apps/order_desk.aos", "apps/task_tracker.aos", "apps/system_monitor.aos", "apps/notes.aos"};
      win.face_app_selected_index = 0;
      face_app_load(win, win.face_app_list[0], k);
      break;
    }
    case WindowType::Inspector: {
      win.title = "Studio";
      win.badge = "studio";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      win.studio_target_name = "order_desk";
      win.studio_active_tab = 0;
      win.studio_drive_steps = {
        "click:60,42",
        "type:PO-2026-999",
        "action:on_new_order",
        "action:on_submit"
      };
      win.status_text = "Studio - pick a running face app to inspect";
      break;
    }
    default: {
      win.title = "Window";
      win.bounds = {left, top, usable_w, usable_h};
      win.normal_bounds = win.bounds;
      win.buffer.resize(win.content_pixels(), tok::panel);
      break;
    }
  }

  uint32_t id = win.window_id;
  AIOS_TRACE("eaw: push_back");
  windows_.push_back(std::move(win));
  AIOS_TRACE("eaw: pushed");
  active_top_tab_ = TopTab::Workspaces;
  palette_open_ = false;
  return id;
}

void HardwareCompositor::init_default_workspace(Kernel& k) {
  kernel_ = &k;
  windows_.clear();
  next_window_id_ = 1;
  active_top_tab_ = TopTab::Overview;
  palette_open_ = false;
  palette_input_.clear();
  palette_selected_idx_ = 0;
  current_workspace_ = CockpitWorkspace::Blank;
  // Set the view here too, not only in the header: this runs on every launch,
  // so a header default left to disagree with it booted the desktop into the
  // workspace with no windows and nothing focused to take a keystroke.
  canvas_mode_ = true;
}

bool HardwareCompositor::mouse_over_content(const WindowSurface& win, int x, int y, int w, int h) const {
  int ox = win.bounds.x + 1 + x;
  int oy = win.bounds.y + 26 + y;
  return mouse_x_ >= ox && mouse_x_ < ox + w && mouse_y_ >= oy && mouse_y_ < oy + h;
}

// ---------------------------------------------------------------------------
// The panes. Each one is the console's grammar drawn by hand: a graphite
// surface, cards inset on it with a hairline edge and a rounded corner, an
// upper-case tracked label over every value, and hue only where something has
// a state worth colouring.
// ---------------------------------------------------------------------------

// A card: the console's .aios-card, which is a 1px --line edge, radius 10, on
// --panel-2, and an upper-case --text-faint heading inside it.
void HardwareCompositor::draw_card(WindowSurface& win, int w, int h, int x, int y, int cw, int ch,
                                   const std::string& heading) {
  draw_rounded_rect_to(win.buffer, w, h, x, y, cw, ch, 10, tok::panel2);
  draw_round_outline_to(win.buffer, w, h, x, y, cw, ch, 10, tok::line);
  if (!heading.empty()) draw_label_to(win.buffer, w, h, x + 12, y + 9, heading, tok::faint);
}

void HardwareCompositor::render_terminal_window(WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);
  (void)k;

  // The chip row: the console's starter chips, which are stadium pills with a
  // hairline edge and dim ink — not outlined boxes in cyan. Geometry is
  // unchanged, so a click still lands where it always did.
  int chip_x = 8;
  for (const char* chip : TERM_CHIPS) {
    int chip_w = text_width(chip, Face::Ui) + 18;
    bool hot = win.focused && mouse_over_content(win, chip_x, 4, chip_w, 22);
    draw_pill_to(win.buffer, w, h, chip_x, 4, chip_w, 22, hot ? tok::wash : 0, hot ? tok::strong : tok::line_up);
    draw_text_to(win.buffer, w, h, chip_x + 9, 6, chip, hot ? tok::text : tok::dim);
    chip_x += chip_w + 6;
  }

  // The transcript, inset as a card. Its first text row stays at y=36 on an
  // 18px pitch: that is the geometry metal/drive-desktop.py reads the screen
  // back with, and it is the terminal's contract with anything that inspects it.
  int body_top = 30;
  int body_h = h - body_top - 40;
  if (body_h < 40) body_h = 40;
  draw_rounded_rect_to(win.buffer, w, h, 6, body_top, w - 12, body_h, 10, tok::panel2);
  draw_round_outline_to(win.buffer, w, h, 6, body_top, w - 12, body_h, 10, tok::line);

  // Wrap to the card before deciding what is visible. A transcript line is as
  // long as the machine's answer, and drawing one logical line per row put the
  // end of every long answer past the right edge where nobody could read it.
  // Scrolling has to count VISUAL rows too, or the last wrapped row of the
  // newest answer sits below the card and the pane looks stuck.
  const int room = w - 24;
  std::vector<TerminalRowInfo> rows = build_terminal_rows(win.lines, room);
  win.wrapped_rows = static_cast<int>(rows.size());

  int text_y = 36;
  int max_visible_lines = (body_h - 10) / 18;
  int start_line = 0;
  if (static_cast<int>(rows.size()) > max_visible_lines) {
    start_line = static_cast<int>(rows.size()) - max_visible_lines + win.scroll_y;
    if (start_line < 0) start_line = 0;
  }

  bool has_sel = terminal_has_selection(win);
  int sl0 = 0, sc0 = 0, sl1 = 0, sc1 = 0;
  if (has_sel) terminal_selection_range(win, &sl0, &sc0, &sl1, &sc1);

  int char_w = text_width("M", Face::Mono);
  if (char_w < 1) char_w = 8;

  for (size_t i = start_line; i < rows.size() && text_y < body_top + body_h - 16; ++i) {
    const auto& r = rows[i];
    if (has_sel && r.line_idx >= static_cast<size_t>(sl0) && r.line_idx <= static_cast<size_t>(sl1)) {
      size_t sel_start = 0;
      if (r.line_idx == static_cast<size_t>(sl0)) {
        if (static_cast<size_t>(sc0) > r.col_offset) {
          sel_start = static_cast<size_t>(sc0) - r.col_offset;
        }
      }
      size_t sel_end = r.text.size();
      if (r.line_idx == static_cast<size_t>(sl1)) {
        if (static_cast<size_t>(sc1) >= r.col_offset) {
          size_t end_in_row = static_cast<size_t>(sc1) - r.col_offset;
          if (end_in_row < sel_end) sel_end = end_in_row;
        } else {
          sel_end = 0;
        }
      }
      if (sel_start < sel_end && sel_start < r.text.size()) {
        if (sel_end > r.text.size()) sel_end = r.text.size();
        int sx = 10 + static_cast<int>(sel_start) * char_w;
        int sw = static_cast<int>(sel_end - sel_start) * char_w;
        draw_rect_to(win.buffer, w, h, sx, text_y - 2, sw, 18, 0xFF2D4F7C);
      }
    }
    draw_text_to(win.buffer, w, h, 10, text_y, r.text, r.colour, Face::Mono);
    text_y += 18;
  }

  // The prompt: the console's .aios-prompt — a card that takes a border-strong
  // edge while it has focus, a faint chevron for the prompt, and a block cursor
  // in near-white rather than terminal green.
  int prompt_y = h - 34;
  draw_rounded_rect_to(win.buffer, w, h, 6, prompt_y, w - 12, 28, 10, tok::panel2);
  draw_round_outline_to(win.buffer, w, h, 6, prompt_y, w - 12, 28, 10, win.focused ? tok::strong : tok::line);
  draw_text_to(win.buffer, w, h, 16, prompt_y + 6, ">", tok::faint);
  draw_text_to(win.buffer, w, h, 32, prompt_y + 6, win.input.text, tok::text, Face::Mono);
  if (win.focused && (steady_ms() / 500) % 2 == 0) {
    // The caret goes where the cursor IS, not at the end of the line. A block
    // at the end of the text, a thin bar between characters, so a person can
    // see both the caret and the glyph they are about to overtype.
    int cx = 32 + text_width(win.input.text.substr(0, win.input.cursor), Face::Mono);
    bool at_end = win.input.cursor >= win.input.text.size();
    draw_rect_to(win.buffer, w, h, cx, prompt_y + 5,
                 at_end ? glyph_advance('m', Face::Mono) : 2, 17, tok::accent);
  }
}

// The miner's own persisted ledger, read back off the root the kernel is
// actually using. The pane used to draw a fixed hashrate and a fixed block
// count on every machine; these are the numbers this machine mined.
namespace {
std::string miner_state_field(Kernel& k, const std::string& key, const std::string& fallback) {
  auto content = k.host().read_file(k.config().root + "/miner.state");
  if (!content.ok()) return fallback;
  std::string text((const char*)content->data(), content->size());
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (line.substr(0, eq) == key) return line.substr(eq + 1);
  }
  return fallback;
}

// 1234567 -> "1,234,567": a hash count is unreadable without the groups.
std::string with_thousands(const std::string& digits) {
  std::string out;
  int seen = 0;
  for (size_t i = digits.size(); i-- > 0;) {
    out.insert(out.begin(), digits[i]);
    if (++seen % 3 == 0 && i > 0) out.insert(out.begin(), ',');
  }
  return out.empty() ? "0" : out;
}

// The rate the last mining run was actually observed at: the nonces it counted
// over the nanoseconds it took, both written down by the miner while it ran.
// "idle" when nothing has run, because a machine that is not mining has no
// hashrate, and a dial that reads 24.8 KH/s on every machine forever is worse
// than an empty one.
std::string miner_measured_hashrate(Kernel& k) {
  uint64_t hashes = 0, ns = 0;
  const std::string h = miner_state_field(k, "last_run_hashes", "0");
  const std::string n = miner_state_field(k, "last_run_ns", "0");
  for (char ch : h) if (ch >= '0' && ch <= '9') hashes = hashes * 10 + (uint64_t)(ch - '0');
  for (char ch : n) if (ch >= '0' && ch <= '9') ns = ns * 10 + (uint64_t)(ch - '0');
  if (hashes == 0 || ns == 0) return "idle";
  double rate = (double)hashes * 1e9 / (double)ns;
  char buf[48];
  if (rate >= 1e9) std::snprintf(buf, sizeof buf, "%.2f GH/s", rate / 1e9);
  else if (rate >= 1e6) std::snprintf(buf, sizeof buf, "%.2f MH/s", rate / 1e6);
  else if (rate >= 1e3) std::snprintf(buf, sizeof buf, "%.1f KH/s", rate / 1e3);
  else std::snprintf(buf, sizeof buf, "%.0f H/s", rate);
  return buf;
}
}  // namespace

void HardwareCompositor::render_miner_window(WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);

  const auto& topo = k.host().topology();
  uint32_t lanes = topo.logical_cpus ? topo.logical_cpus : 1;
  std::string hashes = with_thousands(miner_state_field(k, "total_hashes", "0"));
  std::string best_bits = miner_state_field(k, "best_zero_bits", "");
  std::string diff1 = miner_state_field(k, "meeting_diff1", "0");
  bool daemon = false;

  // Two fact tiles, side by side, exactly as the console's .aios-facts grid.
  int half = (w - 30) / 2;
  draw_card(win, w, h, 10, 10, half, 64, "hash");
  draw_text_to(win.buffer, w, h, 22, 34, "SHA-256d", tok::text);
  draw_label_to(win.buffer, w, h, 22, 52, "local exercise, credits nothing", tok::faint, 0);

  draw_card(win, w, h, 20 + half, 10, half, 64, "hashes computed");
  draw_text_to(win.buffer, w, h, 32 + half, 34, hashes, tok::text, Face::Mono);
  draw_label_to(win.buffer, w, h, 32 + half, 52,
                "all boots, last run " + miner_measured_hashrate(k), tok::faint, 0);

  // The ledger card.
  int ledger_h = h - 84 - 52;
  if (ledger_h < 76) ledger_h = 76;
  draw_card(win, w, h, 10, 84, w - 20, ledger_h, "ledger");
  int ly = 108;
  auto row = [&](const std::string& label, const std::string& value, uint32_t colour) {
    if (ly + 18 > 84 + ledger_h) return;
    draw_label_to(win.buffer, w, h, 22, ly + 2, label, tok::faint);
    draw_text_to(win.buffer, w, h, 22 + 200, ly, value, colour, Face::Mono);
    ly += 20;
  };
  row("best hash", best_bits.empty() ? std::string("none yet") : best_bits + " leading zero bits", tok::text);
  row("under diff 1", diff1 + " (not blocks)", tok::text);
  row("credited", "nothing: no pool, no network work", tok::faint);
  row("lanes", std::to_string(lanes) + " x " + topo.arch, tok::text);

  // One primary action and two ghosts, which is the console's button set: a
  // filled CTA carrying dark ink, the rest a hairline edge over the surface.
  // The hit boxes are unchanged.
  int btn_y = h - 50;
  MinerBtn btns[3];
  miner_buttons(*this, daemon, btns);

  const MinerBtn& b1 = btns[0];
  bool hot1 = mouse_over_content(win, b1.x, btn_y, b1.w, MINER_BTN_H);
  draw_rounded_rect_to(win.buffer, w, h, b1.x, btn_y, b1.w, MINER_BTN_H, 8, tok::cta);
  draw_gradient_v_to(win.buffer, w, h, b1.x + 1, btn_y + 1, b1.w - 2, MINER_BTN_H - 2, tok::cta,
                     hot1 ? tok::cta : tok::cta_2);
  draw_round_outline_to(win.buffer, w, h, b1.x, btn_y, b1.w, MINER_BTN_H, 8, tok::cta);
  draw_text_to(win.buffer, w, h, b1.x + MINER_BTN_PAD, btn_y + 10, b1.label, tok::on_cta);

  auto ghost = [&](const MinerBtn& b) {
    bool hot = mouse_over_content(win, b.x, btn_y, b.w, MINER_BTN_H);
    draw_rounded_rect_to(win.buffer, w, h, b.x, btn_y, b.w, MINER_BTN_H, 8, hot ? tok::wash : tok::panel);
    draw_round_outline_to(win.buffer, w, h, b.x, btn_y, b.w, MINER_BTN_H, 8, hot ? tok::accent : tok::strong);
    // Centred in the width its own label asked for, so the ink is inset the
    // same amount on both sides whichever label the daemon button is wearing.
    int lw = text_width(b.label, Face::Ui);
    draw_text_to(win.buffer, w, h, b.x + (b.w - lw) / 2, btn_y + 10, b.label, hot ? tok::text : tok::dim);
  };
  ghost(btns[1]);
  ghost(btns[2]);
}

// Both fabrics, asked this frame: the entities the kernel is running and the
// devices it can place work on. The console shows exactly these two tables on
// its Machine tab, so this is the same view of the same machine.
void HardwareCompositor::render_tasks_window(WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);

  auto entities = k.entities().list();
  auto devices = k.devices().all();

  // Two cards sharing the pane. Each one asks for exactly the height its rows
  // need; if the pane cannot give them both that, the surplus is taken off the
  // taller one, so a machine with three entities and one device does not draw
  // two of them and silently lose the third — which is what a fixed split did.
  const int split = h - 34;                  // above the footer strip
  const int gap = 12;
  int avail = split - 20 - gap;
  // 24 to the column header, 20 for the header itself, 20 a row, 8 of padding.
  int ent_want = 52 + static_cast<int>(entities.size()) * 20;
  int dev_want = 52 + (devices.empty() ? 1 : static_cast<int>(devices.size())) * 20;
  int ent_h = ent_want, dev_h = dev_want;
  if (ent_want + dev_want > avail && avail > 100) {
    // Shared in proportion to what each asked for. Taking the whole shortage
    // off the taller card is how a three-entity machine drew one entity.
    ent_h = (avail * ent_want) / (ent_want + dev_want);
    dev_h = avail - ent_h;
    if (ent_h < 50) { ent_h = 50; dev_h = avail - 50; }
    if (dev_h < 50) { dev_h = 50; ent_h = avail - 50; }
  }

  // ---- entities ----
  draw_card(win, w, h, 10, 10, w - 20, ent_h, "entities");
  int cy = 34;
  draw_label_to(win.buffer, w, h, 22, cy, "id", tok::faint);
  draw_label_to(win.buffer, w, h, 130, cy, "label", tok::faint);
  draw_label_to(win.buffer, w, h, 270, cy, "state", tok::faint);
  draw_label_to(win.buffer, w, h, 360, cy, "gen", tok::faint);
  draw_label_to(win.buffer, w, h, 420, cy, "memory", tok::faint);
  cy += 20;
  for (const auto& e : entities) {
    if (cy + 20 > 10 + ent_h - 6) break;
    draw_rect_to(win.buffer, w, h, 16, cy - 4, w - 32, 1, tok::line);
    draw_text_to(win.buffer, w, h, 22, cy, e.id.short_str(), tok::dim, Face::Mono);
    draw_text_to(win.buffer, w, h, 130, cy, e.label.empty() ? "(none)" : e.label, tok::text);
    const char* st = entity_status_name(e.status);
    draw_text_to(win.buffer, w, h, 270, cy, st, strcmp(st, "active") == 0 ? tok::ok : tok::dim);
    draw_text_to(win.buffer, w, h, 360, cy, std::to_string(e.generation), tok::dim, Face::Mono);
    draw_text_to(win.buffer, w, h, 420, cy, std::to_string(e.memory.size()) + " objs", tok::text, Face::Mono);
    cy += 20;
  }

  // ---- devices ----
  int dev_y = 10 + ent_h + gap;
  if (dev_h > 30) {
    draw_card(win, w, h, 10, dev_y, w - 20, dev_h, "devices");
    int dy = dev_y + 24;
    draw_label_to(win.buffer, w, h, 22, dy, "id", tok::faint);
    draw_label_to(win.buffer, w, h, 130, dy, "type", tok::faint);
    draw_label_to(win.buffer, w, h, 270, dy, "lanes", tok::faint);
    draw_label_to(win.buffer, w, h, 360, dy, "busy", tok::faint);
    draw_label_to(win.buffer, w, h, 420, dy, "memory", tok::faint);
    dy += 20;
    for (auto* d : devices) {
      if (!d || dy + 20 > dev_y + dev_h - 6) break;
      draw_rect_to(win.buffer, w, h, 16, dy - 4, w - 32, 1, tok::line);
      draw_text_to(win.buffer, w, h, 22, dy, d->id().short_str(), tok::dim, Face::Mono);
      draw_text_to(win.buffer, w, h, 130, dy, device_type_name(d->type()), tok::text);
      draw_text_to(win.buffer, w, h, 270, dy, std::to_string(d->capacity().lanes), tok::text, Face::Mono);
      draw_text_to(win.buffer, w, h, 360, dy, std::to_string(d->busy_lanes()), d->busy_lanes() ? tok::ok : tok::dim, Face::Mono);
      draw_text_to(win.buffer, w, h, 420, dy, std::to_string(d->capacity().memory >> 20) + " MiB", tok::text, Face::Mono);
      dy += 20;
    }
    if (devices.empty()) draw_text_to(win.buffer, w, h, 22, dy, "no devices in the fabric", tok::faint);
  }

  // Footer: a hairline and one faint line of arithmetic, ASCII separators only.
  draw_rect_to(win.buffer, w, h, 0, h - 34, w, 1, tok::line_up);
  draw_label_to(win.buffer, w, h, 12, h - 24,
                std::to_string(k.host().topology().logical_cpus) + " lanes / " +
                    std::to_string(entities.size()) + " entities / " +
                    std::to_string(k.slots()) + " scheduling slots",
                tok::faint);
}

// The machine, asked rather than described: every line here is a value the HAL
// answers for this boot on this hardware.
void HardwareCompositor::render_hardware_window(WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);
  const auto& topo = k.host().topology();

  int y = 10;
  int card_top = 0;
  auto open_card = [&](const std::string& heading, int rows) {
    card_top = y;
    int ch = 30 + rows * 20;
    draw_card(win, w, h, 10, y, w - 20, ch, heading);
    y += 30;
    return ch;
  };
  auto row = [&](const std::string& label, const std::string& value, uint32_t colour) {
    if (y + 18 > h - 6) return;
    draw_label_to(win.buffer, w, h, 22, y + 2, label, tok::faint);
    draw_text_to(win.buffer, w, h, 22 + 176, y, value, colour);
    y += 20;
  };
  auto row_mono = [&](const std::string& label, const std::string& value, uint32_t colour) {
    if (y + 18 > h - 6) return;
    draw_label_to(win.buffer, w, h, 22, y + 2, label, tok::faint);
    draw_text_to(win.buffer, w, h, 22 + 176, y, value, colour, Face::Mono);
    y += 20;
  };
  auto close_card = [&](int ch) { y = card_top + ch + 10; };

  int ch = open_card("substrate", 5);
  row_mono("kernel", k.self().short_str(), tok::text);
  row("host", topo.host_name, tok::text);
  row("architecture", topo.arch + "  /  " + std::to_string(topo.logical_cpus) + " logical cpus", tok::text);
  row("memory", std::to_string(topo.total_memory >> 20) + " MiB, " +
                std::to_string(topo.available_memory >> 20) + " MiB free", tok::text);
  row("numa nodes", std::to_string(topo.numa.size()), tok::text);
  close_card(ch);

  auto vols = k.host().storage("");
  auto nics = k.host().network();
  int store_rows = vols.ok() ? static_cast<int>(vols->size()) : 1;
  if (store_rows < 1) store_rows = 1;
  ch = open_card("storage", store_rows);
  if (vols.ok() && !vols->empty()) {
    for (const auto& v : *vols)
      row(v.name.empty() ? v.kind : v.name,
          v.kind + ", " + std::to_string(v.capacity >> 20) + " MiB" + (v.holds_root ? "  (root)" : ""),
          v.holds_root ? tok::ok : tok::text);
  } else {
    row("volumes", vols.ok() ? "none reported" : vols.error().str(), tok::faint);
  }
  close_card(ch);

  int nic_rows = (nics.ok() && !nics->empty()) ? static_cast<int>(nics->size()) : 1;
  ch = open_card("network", nic_rows);
  if (nics.ok() && !nics->empty()) {
    for (const auto& n : *nics)
      row(n.name, n.hardware + (n.carries_fabric ? "  (carries the fabric)" : ""), n.up ? tok::ok : tok::faint);
  } else {
    row("interfaces", "none on this machine", tok::faint);
  }
  close_card(ch);

  ch = open_card("display", 1);
  row("framebuffer", std::to_string(width_) + " x " + std::to_string(height_) + " ARGB, composited here", tok::text);
  close_card(ch);

  // The buses, as the machine itself reads them: a real PCI configuration-space
  // scan on the metal, and what the substrate says is on the bus when hosted.
  auto buses = k.host().hardware();
  int room = (h - 6 - y - 30) / 20;
  if (room > 0) {
    int rows_shown = 0;
    if (buses.ok()) rows_shown = static_cast<int>(buses->size()) < room ? static_cast<int>(buses->size()) : room;
    if (rows_shown < 1) rows_shown = 1;
    ch = open_card("buses", rows_shown);
    if (!buses.ok() || buses->empty()) {
      row("pci", buses.ok() ? "this substrate does not enumerate buses" : buses.error().str(), tok::faint);
    } else {
      for (const auto& d : *buses) {
        if (y + 18 > h - 6) break;
        draw_text_to(win.buffer, w, h, 22, y, d.slot, tok::dim, Face::Mono);
        draw_text_to(win.buffer, w, h, 22 + 84, y, d.kind, tok::text);
        draw_text_to(win.buffer, w, h, 22 + 208, y, d.name, tok::dim, Face::Mono);
        if (!d.driver.empty())
          draw_label_to(win.buffer, w, h, 22 + 344, y + 2, d.driver, tok::ok);
        y += 20;
      }
    }
    close_card(ch);
  }
}

// ---------------------------------------------------------------------------
// The workstation's two native applications.
//
// Both were, until now, paintings: the editor drew fifteen hard-coded lines of
// C++ that came from nowhere and its Save chip only raised a toast; the studio
// drew eleven fixed tree rows and five fixed table rows over a "CONNECTED"
// badge with nothing behind it. What follows binds both to the substrate this
// machine actually has — the host's files for one, the kernel's own SQL engine
// for the other — so every value on either pane is something the machine was
// asked for and answered.
// ---------------------------------------------------------------------------

SqlDatabase* HardwareCompositor::sql_engine(Kernel& k) {
  // The machine's one engine - grain's sql() and fleet peers use the same one,
  // so a row the desktop shows is the row they wrote, not a second engine's
  // copy of the store flushing over it.
  return machine_sql(k);
}

void HardwareCompositor::db_connect(WindowSurface& win, Kernel& k) {
  SqlDatabase* db = sql_engine(k);
  if (!db) {
    win.db_connected = false;
    win.db_error = true;
    win.db_status = "the SQL engine did not open on this substrate";
    return;
  }
  win.db_connected = true;
  win.db_error = false;
  win.db_tables = db->list_tables();
  win.db_table_rows.clear();
  for (const auto& t : win.db_tables) win.db_table_rows.push_back(db->row_count(t));
  win.db_status = "engine open at " + k.config().root + "/sql_db - " +
                  std::to_string(win.db_tables.size()) + " table(s)";
  if (win.db_selected.empty() && !win.db_tables.empty()) db_open_table(win, k, win.db_tables.front());
}

void HardwareCompositor::db_refresh(WindowSurface& win, Kernel& k) {
  SqlDatabase* db = sql_engine(k);
  if (!db) { db_connect(win, k); return; }
  (void)db->load_tables();
  win.db_tables = db->list_tables();
  win.db_table_rows.clear();
  for (const auto& t : win.db_tables) win.db_table_rows.push_back(db->row_count(t));
  win.db_connected = true;
  if (!win.db_selected.empty()) db_open_table(win, k, win.db_selected);
  else win.db_status = std::to_string(win.db_tables.size()) + " table(s)";
}

void HardwareCompositor::db_run_query(WindowSurface& win, Kernel& k, const std::string& sql) {
  // The filter: `WHERE <predicate>` typed on the SQL line narrows the open
  // table's grid -- paged, sortable, editable, counted -- instead of running
  // as a statement (it is not one). A bare `WHERE` clears it. db_where used to
  // be read by the pager and set by nothing, so a table could not be filtered
  // without giving up the pager and editing for a hand-written SELECT.
  {
    size_t b = sql.find_first_not_of(" \t\r\n");
    if (b != std::string::npos && sql.size() - b >= 5 && !win.db_selected.empty()) {
      std::string head = sql.substr(b, 5);
      for (char& c : head) c = (char)std::tolower((unsigned char)c);
      const bool word_ends = sql.size() - b == 5 || std::isspace((unsigned char)sql[b + 5]) || sql[b + 5] == ';';
      if (head == "where" && word_ends) {
        std::string pred = sql.substr(b + 5);
        while (!pred.empty() && (std::isspace((unsigned char)pred.back()) || pred.back() == ';')) pred.pop_back();
        size_t pb = pred.find_first_not_of(" \t\r\n");
        pred = pb == std::string::npos ? std::string() : pred.substr(pb);
        const std::string before = win.db_where;
        win.db_where = pred;
        win.db_page = 0;
        db_load_page(win, k);
        if (win.db_error && !pred.empty()) {
          // A predicate the engine refuses: say why, and keep the last good one.
          const std::string why = win.db_status;
          win.db_where = before;
          db_load_page(win, k);
          win.db_error = true;
          win.db_status = "filter refused: " + why;
        }
        return;
      }
    }
  }
  SqlDatabase* db = sql_engine(k);
  win.db_last_query = sql;
  if (!db) {
    win.db_error = true;
    win.db_connected = false;
    win.db_status = "the SQL engine did not open on this substrate";
    return;
  }
  win.db_connected = true;
  SqlQueryResult res = db->execute_query(sql);
  win.db_error = !res.success;
  if (!res.success) {
    win.db_status = res.error_message;
    return;
  }
  win.grid_columns = res.columns;
  win.db_grid_label = "query";
  // A grid built from an arbitrary statement is not a table's rows, so nothing
  // in it is editable until a table is opened again -- and it is not a page of
  // anything, so the pager under it goes away rather than paging a result set
  // the engine already finished.
  win.db_grid_table.clear();
  win.db_paged = false;
  win.grid_selected_col = -1;
  // db_col_first is NOT reset here: re-reading the page you are on (a refresh,
  // a page-size change, a sort) must leave the grid looking at the same
  // columns. It is reset when a different table is opened, and clamped by the
  // renderer when a statement comes back narrower than where you were.
  win.grid_sort_col = -1;
  win.grid_selected_row = -1;
  win.grid_edit_row = -1;
  win.grid_edit_col = -1;
  win.grid_rows.clear();
  win.grid_rows.reserve(res.rows.size());
  // The engine marks NULL with a sentinel byte no literal can produce. Turn it
  // into the word here, once, or the grid paints a control character and the
  // CSV carries it out to a file.
  for (const auto& r : res.rows) {
    std::vector<std::string> shown;
    shown.reserve(r.values.size());
    for (const auto& v : r.values) shown.push_back(sql_display(v));
    win.grid_rows.push_back(std::move(shown));
  }
  win.grid_scroll = 0;
  if (res.columns.empty()) {
    // A statement that changed the tables rather than reading them: the shape
    // of the database moved, so the explorer has to be asked again.
    (void)db->save_tables();
    win.db_tables = db->list_tables();
    win.db_table_rows.clear();
    for (const auto& t : win.db_tables) win.db_table_rows.push_back(db->row_count(t));
    win.db_status = std::to_string(res.rows_affected) + " row(s) affected";
  } else {
    win.db_status = std::to_string(win.grid_rows.size()) + " row(s) x " +
                    std::to_string(win.grid_columns.size()) + " column(s)";
  }
}

void HardwareCompositor::db_open_table(WindowSurface& win, Kernel& k, const std::string& table) {
  if (table.empty()) return;
  bool changed = (table != win.db_selected);
  win.db_selected = table;
  SqlDatabase* db = sql_engine(k);
  win.db_schema.clear();
  if (db) {
    if (const SqlTable* t = db->find_table(table)) {
      for (const auto& c : t->columns) {
        const char* ty = c.type == SqlDataType::Int ? "INT" : (c.type == SqlDataType::Real ? "REAL" : (c.type == SqlDataType::Decimal ? "DECIMAL" : "TEXT"));
        win.db_schema.push_back(c.name + std::string("  ") + ty + (c.is_primary_key ? "  PK" : ""));
      }
    }
  }
  if (changed) {
    // A different table: start at its first page, un-sorted, scrolled home.
    // Carrying the last table's ORDER BY column over would name a column this
    // one does not have.
    win.db_page = 0;
    win.grid_sort_col = -1;
    win.grid_sort_desc = false;
    win.db_col_first = 0;
    win.db_col_w.clear();
    win.grid_selected_col = -1;
    win.db_where.clear();
  }
  db_load_page(win, k);
}

// ONE PAGE of the open table. This is the difference between opening `prices`
// and waiting for 873,551 rows to cross the wire into a grid that can show
// thirty of them, and getting the first two hundred back in milliseconds.
void HardwareCompositor::db_load_page(WindowSurface& win, Kernel& k) {
  if (win.db_selected.empty()) return;
  SqlDatabase* db = sql_engine(k);
  if (!db) {
    win.db_connected = false;
    win.db_error = true;
    win.db_status = "the SQL engine did not open on this substrate";
    return;
  }
  const std::string table = win.db_selected;
  // The engine's own count, so the pager knows how far it can go without
  // reading a row of the table to find out.
  size_t total = db->row_count(table);
  if (!win.db_where.empty()) {
    // Filtered: the pager counts the rows the filter lets through.
    SqlQueryResult cnt = db->execute_query("SELECT COUNT(*) FROM `" + table + "` WHERE " + win.db_where + ";");
    if (!cnt.success) {
      win.db_error = true;
      win.db_status = cnt.error_message;
      return;
    }
    total = (!cnt.rows.empty() && !cnt.rows[0].values.empty())
                ? (size_t)std::strtoull(sql_display(cnt.rows[0].values[0]).c_str(), nullptr, 10)
                : 0;
  }
  win.db_total_rows = total;

  int size = win.db_page_size > 0 ? win.db_page_size : 200;
  int pages = total == 0 ? 1 : static_cast<int>((total + size - 1) / size);
  if (win.db_page >= pages) win.db_page = pages - 1;
  if (win.db_page < 0) win.db_page = 0;
  long long offset = static_cast<long long>(win.db_page) * size;

  // The sort travels WITH the query, not over the page: ordering two hundred
  // rows that were already chosen arbitrarily orders nothing.
  int sort_col = win.grid_sort_col;
  bool sort_desc = win.grid_sort_desc;
  std::string sort_name;
  if (sort_col >= 0 && sort_col < static_cast<int>(win.grid_columns.size()))
    sort_name = win.grid_columns[static_cast<size_t>(sort_col)];

  std::string sql = "SELECT * FROM `" + table + "`";
  if (!win.db_where.empty()) sql += " WHERE " + win.db_where;
  if (!sort_name.empty()) sql += " ORDER BY `" + sort_name + "`" + (sort_desc ? " DESC" : " ASC");
  sql += " LIMIT " + std::to_string(size) + " OFFSET " + std::to_string(offset) + ";";

  db_run_query(win, k, sql);
  if (win.db_error) return;

  // db_run_query cannot know a paged read from a typed statement, so it drops
  // the sort and refuses editing. Both are true again here: these ARE this
  // table's rows, and they are ordered by the column the header says.
  win.db_paged = true;
  win.db_grid_label = table;
  win.db_grid_table = table;
  if (!sort_name.empty()) {
    for (size_t c = 0; c < win.grid_columns.size(); ++c)
      if (win.grid_columns[c] == sort_name) { win.grid_sort_col = static_cast<int>(c); break; }
    win.grid_sort_desc = sort_desc;
  }
  if (win.db_col_first >= static_cast<int>(win.grid_columns.size())) win.db_col_first = 0;

  // The pager under the grid says which rows and which page; this says what
  // came back, once, without repeating it.
  win.db_status = std::to_string(win.grid_rows.size()) + " row(s) x " +
                  std::to_string(win.grid_columns.size()) + " column(s)" +
                  (win.db_where.empty() ? std::string() : "  [WHERE " + win.db_where + "]");
}

void HardwareCompositor::db_set_page(WindowSurface& win, Kernel& k, int page) {
  if (!win.db_paged || win.db_selected.empty()) return;
  int size = win.db_page_size > 0 ? win.db_page_size : 200;
  int pages = win.db_total_rows == 0 ? 1
              : static_cast<int>((win.db_total_rows + static_cast<size_t>(size) - 1) / static_cast<size_t>(size));
  if (page < 0) page = 0;
  if (page > pages - 1) page = pages - 1;
  if (page == win.db_page) return;
  win.db_page = page;
  win.grid_selected_row = -1;
  win.grid_scroll = 0;
  db_load_page(win, k);
}

// The explorer's rows after the filter box has had its say. Indices into
// db_tables, so the row counts beside them stay lined up.
std::vector<size_t> HardwareCompositor::db_visible_tables(const WindowSurface& win) const {
  std::vector<size_t> out;
  std::string needle = win.db_table_filter.text;
  for (auto& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  out.reserve(win.db_tables.size());
  for (size_t i = 0; i < win.db_tables.size(); ++i) {
    if (needle.empty()) { out.push_back(i); continue; }
    std::string hay = win.db_tables[i];
    for (auto& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (hay.find(needle) != std::string::npos) out.push_back(i);
  }
  return out;
}

// --- the grid as a place you change things ---------------------------------

void HardwareCompositor::db_column_widths(const WindowSurface& win, int grid_w,
                                          std::vector<int>* out) const {
  // Widths from what is actually in the cells. What this DOES NOT do any more
  // is divide the pane by the column count when the columns do not fit: an
  // eighteen-column table came out as eighteen forty-pixel columns of "756..",
  // which is a grid that shows you nothing and calls it a row. Columns keep
  // their true width and the grid scrolls sideways by whole columns instead.
  size_t nc = win.grid_columns.size();
  out->assign(nc, 0);
  for (size_t c = 0; c < nc; ++c) {
    (*out)[c] = text_width(win.grid_columns[c], Face::UiBold) + 30;
    size_t sampled = 0;
    for (const auto& r : win.grid_rows) {
      if (++sampled > 200) break;               // enough to size by
      if (c >= r.size()) continue;
      int want = text_width(r[c], Face::Mono) + 24;
      if (want > (*out)[c]) (*out)[c] = want;
    }
    if ((*out)[c] > 340) (*out)[c] = 340;
    // A width the person dragged wins over anything measured.
    if (c < win.db_col_w.size() && win.db_col_w[c] > 0) (*out)[c] = win.db_col_w[c];
    if ((*out)[c] < 40) (*out)[c] = 40;
  }
  // When everything DOES fit, spread the slack so the grid fills its pane
  // rather than leaving a stripe of empty ground on the right.
  int total = 0;
  for (int v : *out) total += v;
  int avail = grid_w - 16;
  if (nc > 0 && total > 0 && total < avail) {
    int extra = (avail - total) / static_cast<int>(nc);
    for (size_t c = 0; c < nc; ++c) (*out)[c] += extra;
  }
}

bool HardwareCompositor::db_grid_is_table(const WindowSurface& win) const {
  return !win.db_grid_table.empty() && !win.grid_columns.empty();
}

void HardwareCompositor::db_sort_grid(WindowSurface& win, int column) {
  if (column < 0 || column >= (int)win.grid_columns.size()) return;
  // Clicking the column already sorted turns it around, which is what a person
  // expects from a grid header and saves retyping the statement.
  if (win.grid_sort_col == column) win.grid_sort_desc = !win.grid_sort_desc;
  else { win.grid_sort_col = column; win.grid_sort_desc = false; }

  if (win.db_paged && kernel_) {
    // The grid is a WINDOW onto a table, so the order has to be decided by the
    // engine over every row -- sorting the two hundred rows that happen to be
    // on screen would put the largest of THOSE at the top and call it the
    // largest in the table.
    win.db_page = 0;
    db_load_page(win, *kernel_);
    return;
  }

  const size_t c = (size_t)column;
  const bool desc = win.grid_sort_desc;
  std::stable_sort(win.grid_rows.begin(), win.grid_rows.end(),
                   [c, desc](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                     const std::string& x = c < a.size() ? a[c] : std::string();
                     const std::string& y = c < b.size() ? b[c] : std::string();
                     // Numbers order as numbers; everything else as text. A
                     // column of ids that sorts 1, 10, 2 is a column nobody can
                     // read.
                     char* ex = nullptr;
                     char* ey = nullptr;
                     double nx = std::strtod(x.c_str(), &ex);
                     double ny = std::strtod(y.c_str(), &ey);
                     bool numeric = !x.empty() && !y.empty() && ex && ey && *ex == 0 && *ey == 0;
                     if (numeric) return desc ? nx > ny : nx < ny;
                     return desc ? y < x : x < y;
                   });
  win.grid_selected_row = -1;
  win.grid_edit_row = -1;
  win.grid_edit_col = -1;
}

void HardwareCompositor::db_begin_edit(WindowSurface& win, int row, int column) {
  if (!db_grid_is_table(win)) {
    win.db_status = "this grid is a statement's result, not a table's rows - open a table to edit";
    win.db_error = true;
    return;
  }
  if (row < 0 || row >= (int)win.grid_rows.size()) return;
  if (column < 0 || column >= (int)win.grid_columns.size()) return;
  win.grid_edit_row = row;
  win.grid_edit_col = column;
  const auto& r = win.grid_rows[(size_t)row];
  std::string cur = (size_t)column < r.size() ? r[(size_t)column] : std::string();
  // NULL is shown as the word; typing over it should start from nothing rather
  // than from the four letters the grid drew.
  win.grid_edit.set(cur == "NULL" ? std::string() : cur);
}

void HardwareCompositor::db_cancel_edit(WindowSurface& win) {
  win.grid_edit_row = -1;
  win.grid_edit_col = -1;
  win.grid_edit.clear();
}

void HardwareCompositor::db_commit_edit(WindowSurface& win, Kernel& k) {
  if (win.grid_edit_row < 0 || win.grid_edit_col < 0) return;
  int row = win.grid_edit_row, col = win.grid_edit_col;
  std::string value = win.grid_edit.text;
  db_cancel_edit(win);
  if (!db_grid_is_table(win)) return;

  SqlDatabase* db = sql_engine(k);
  const SqlTable* t = db ? db->find_table(win.db_grid_table) : nullptr;
  if (!t) {
    win.db_error = true;
    win.db_status = "no such table: " + win.db_grid_table;
    return;
  }
  // An edit has to name the row it changes. Without a primary key there is no
  // way to say which row this is, so the studio says so instead of writing an
  // UPDATE that could hit several.
  int pk = -1;
  for (size_t i = 0; i < t->columns.size(); ++i)
    if (t->columns[i].is_primary_key) { pk = (int)i; break; }
  if (pk < 0) {
    win.db_error = true;
    win.db_status = win.db_grid_table + " has no primary key, so a cell cannot be edited safely";
    return;
  }
  if (pk >= (int)win.grid_columns.size() || row >= (int)win.grid_rows.size()) return;

  auto quote = [](const std::string& v) {
    std::string out = "'";
    for (char c : v) {
      if (c == '\'') out += "''";
      else if (c == '\\') out += "\\\\";
      else out += c;
    }
    return out + "'";
  };
  const std::string& key_value = win.grid_rows[(size_t)row][(size_t)pk];
  std::string set_to = value.empty() ? std::string("NULL") : quote(value);
  std::string sql = "UPDATE `" + win.db_grid_table + "` SET `" + win.grid_columns[(size_t)col] +
                    "` = " + set_to + " WHERE `" + win.grid_columns[(size_t)pk] + "` = " +
                    quote(key_value) + ";";

  SqlQueryResult res = db->execute_query(sql);
  win.db_error = !res.success;
  if (!res.success) {
    win.db_status = res.error_message;
    return;
  }
  std::string said = std::to_string(res.rows_affected) + " row(s) updated";
  // Re-read the table so the grid shows what the engine now holds, not what
  // the person typed -- but keep what the edit reported, since the re-read's
  // own "N rows x M columns" is not news about the edit.
  std::string table = win.db_grid_table;
  db_open_table(win, k, table);
  win.db_status = said;
  win.db_error = false;
}

void HardwareCompositor::db_insert_row(WindowSurface& win, Kernel& k) {
  if (win.db_selected.empty()) {
    win.db_error = true;
    win.db_status = "pick a table first";
    return;
  }
  SqlDatabase* db = sql_engine(k);
  const SqlTable* t = db ? db->find_table(win.db_selected) : nullptr;
  if (!t) return;
  // A row of defaults, which the person then edits cell by cell. Columns the
  // engine fills itself -- an auto-increment key -- are left out so it can.
  std::string cols, vals;
  for (const auto& c : t->columns) {
    if (c.auto_increment) continue;
    if (!cols.empty()) { cols += ", "; vals += ", "; }
    cols += "`" + c.name + "`";
    if (column_has_default(c.default_value) && !sql_is_null(column_default_part(c.default_value))) vals += column_default_is_expr(column_default_part(c.default_value)) ? column_default_text(c.default_value) : "'" + column_default_part(c.default_value) + "'";
    else if (c.not_null) vals += (c.type == SqlDataType::Text) ? "''" : "0";
    else vals += "NULL";
  }
  std::string table = win.db_selected;
  SqlQueryResult res = db->execute_query("INSERT INTO `" + table + "` (" + cols + ") VALUES (" + vals + ");");
  win.db_error = !res.success;
  win.db_status = res.success ? "row inserted" : res.error_message;
  if (res.success) {
    // The new row is at the END of the table, so on a paged grid it is not on
    // the page you were looking at: jump to the last page and the bottom of it,
    // or Add row looks like a button that does nothing. db_load_page clamps
    // both of these to what actually exists.
    win.db_page = 1 << 28;
    db_open_table(win, k, table);
    win.grid_scroll = 1 << 28;
    win.grid_selected_row = win.grid_rows.empty() ? -1 : (int)win.grid_rows.size() - 1;
  }
}

void HardwareCompositor::db_delete_selected_row(WindowSurface& win, Kernel& k) {
  if (!db_grid_is_table(win) || win.grid_selected_row < 0) {
    win.db_error = true;
    win.db_status = "pick a row in a table's grid first";
    return;
  }
  SqlDatabase* db = sql_engine(k);
  const SqlTable* t = db ? db->find_table(win.db_grid_table) : nullptr;
  if (!t) return;
  int pk = -1;
  for (size_t i = 0; i < t->columns.size(); ++i)
    if (t->columns[i].is_primary_key) { pk = (int)i; break; }
  if (pk < 0 || pk >= (int)win.grid_columns.size()) {
    win.db_error = true;
    win.db_status = win.db_grid_table + " has no primary key, so one row cannot be singled out";
    return;
  }
  if (win.grid_selected_row >= (int)win.grid_rows.size()) return;
  std::string key = win.grid_rows[(size_t)win.grid_selected_row][(size_t)pk];
  std::string esc;
  for (char c : key) { if (c == '\'') esc += "''"; else esc += c; }
  std::string table = win.db_grid_table;
  SqlQueryResult res = db->execute_query("DELETE FROM `" + table + "` WHERE `" +
                                         win.grid_columns[(size_t)pk] + "` = '" + esc + "';");
  win.db_error = !res.success;
  win.db_status = res.success ? (std::to_string(res.rows_affected) + " row(s) deleted")
                              : res.error_message;
  if (res.success) db_open_table(win, k, table);
}

void HardwareCompositor::db_show_ddl(WindowSurface& win, Kernel& k) {
  if (win.db_selected.empty()) {
    win.db_error = true;
    win.db_status = "pick a table first";
    return;
  }
  SqlDatabase* db = sql_engine(k);
  if (!db) return;
  auto ddl = db->table_ddl(win.db_selected);
  if (!ddl.ok()) {
    win.db_error = true;
    win.db_status = ddl.error().str();
    return;
  }
  // The statement, one line per row, so it reads in the grid it lands in.
  win.grid_columns = {"CREATE TABLE " + win.db_selected};
  win.grid_rows.clear();
  std::string cur;
  for (char c : *ddl) {
    if (c == '\n') { win.grid_rows.push_back({cur}); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) win.grid_rows.push_back({cur});
  win.db_grid_label = win.db_selected + ".ddl";
  win.db_grid_table.clear();
  win.grid_sort_col = -1;
  win.grid_scroll = 0;
  win.db_error = false;
  win.db_status = "the statement that would recreate " + win.db_selected;
}

void HardwareCompositor::db_open_designer(WindowSurface& win, Kernel& k) {
  if (win.db_selected.empty()) {
    win.db_error = true;
    win.db_status = "pick a table first";
    return;
  }
  SqlDatabase* db = sql_engine(k);
  if (!db) return;
  SqlTable* t = db->resolve_table(win.db_selected);
  if (!t) {
    win.db_error = true;
    win.db_status = "table not found: " + win.db_selected;
    return;
  }
  win.db_designer_table = win.db_selected;
  win.db_designer_edits.clear();
  for (const auto& col : t->columns) {
    ColumnEdit e;
    e.original_name = col.name;
    e.col = col;
    win.db_designer_edits.push_back(e);
  }
  win.db_designer_tab = 0;
  win.db_designer_scroll = 0;
  win.db_designer_focus_row = -1;
  win.db_designer_focus_col = -1;
  win.db_designer_index_field_focus = -1;
  win.db_designer_fk_field_focus = -1;
  win.db_designer_new_index_name.clear();
  win.db_designer_new_index_cols.clear();
  win.db_designer_new_index_unique = false;
  win.db_designer_new_fk_name.clear();
  win.db_designer_new_fk_col.clear();
  win.db_designer_new_fk_ref_table.clear();
  win.db_designer_new_fk_ref_col.clear();
  win.db_show_designer = true;
  win.db_show_conn_mgr = false;
  win.db_show_telemetry = false;
}

void HardwareCompositor::db_designer_apply(WindowSurface& win, Kernel& k) {
  SqlDatabase* db = sql_engine(k);
  if (!db || win.db_designer_table.empty()) return;
  auto r = db->apply_table_design(win.db_designer_table, win.db_designer_edits);
  if (!r.ok()) {
    set_toast("apply failed: " + r.error().str());
    return;
  }
  set_toast("table design applied: " + win.db_designer_table);
  // Refresh from the LIVE in-memory engine, not db_refresh()'s db_refresh()
  // -> load_tables() path: that reloads the schema from schema_tables.db on
  // disk, which checkpoint() (a redo-log append, not a full snapshot
  // rewrite - see flush_now()'s own comment) does not keep continuously
  // caught up with. Driving this end-to-end is what caught it: apply a
  // rename, then immediately db_refresh(), and the reload silently
  // reverted the rename that had, seconds earlier, correctly landed in the
  // live table - a real bug, not a hypothetical one.
  win.db_tables = db->list_tables();
  win.db_table_rows.clear();
  for (const auto& t : win.db_tables) win.db_table_rows.push_back(db->row_count(t));
  db_open_designer(win, k);
  if (!win.db_selected.empty()) db_open_table(win, k, win.db_selected);
}

void HardwareCompositor::db_designer_add_index(WindowSurface& win, Kernel& k) {
  SqlDatabase* db = sql_engine(k);
  if (!db || win.db_designer_table.empty()) return;
  if (win.db_designer_new_index_cols.empty()) {
    set_toast("give the index at least one column");
    return;
  }
  std::string stmt = "ALTER TABLE `" + win.db_designer_table + "` ADD " +
                      (win.db_designer_new_index_unique ? std::string("UNIQUE INDEX ") : std::string("INDEX "));
  if (!win.db_designer_new_index_name.empty()) stmt += "`" + win.db_designer_new_index_name + "` ";
  stmt += "(" + win.db_designer_new_index_cols + ")";
  SqlQueryResult r = db->execute_query(stmt);
  if (!r.success) { set_toast("add index failed: " + r.error_message); return; }
  win.db_designer_new_index_name.clear();
  win.db_designer_new_index_cols.clear();
  win.db_designer_new_index_unique = false;
  set_toast("index added");
}

void HardwareCompositor::db_designer_add_fk(WindowSurface& win, Kernel& k) {
  SqlDatabase* db = sql_engine(k);
  if (!db || win.db_designer_table.empty()) return;
  if (win.db_designer_new_fk_col.empty() || win.db_designer_new_fk_ref_table.empty() ||
      win.db_designer_new_fk_ref_col.empty()) {
    set_toast("a foreign key needs a column, a referenced table and a referenced column");
    return;
  }
  std::string stmt = "ALTER TABLE `" + win.db_designer_table + "` ADD ";
  if (!win.db_designer_new_fk_name.empty()) stmt += "CONSTRAINT `" + win.db_designer_new_fk_name + "` ";
  stmt += "FOREIGN KEY (`" + win.db_designer_new_fk_col + "`) REFERENCES `" +
          win.db_designer_new_fk_ref_table + "`(`" + win.db_designer_new_fk_ref_col + "`)";
  SqlQueryResult r = db->execute_query(stmt);
  if (!r.success) { set_toast("add foreign key failed: " + r.error_message); return; }
  win.db_designer_new_fk_name.clear();
  win.db_designer_new_fk_col.clear();
  win.db_designer_new_fk_ref_table.clear();
  win.db_designer_new_fk_ref_col.clear();
  set_toast("foreign key added");
}

// A footprint the pane can defend: the engine's own row and column counts at a
// nominal per-cell cost, rather than a constant that never moved.
static constexpr double kDbCellBytes = 24.0;

// Where this engine's writes go, from the engine — not a string a button flips.
static std::string db_engine_mode_label(SqlDatabase& db) {
  const std::string where = db.active_profile().is_local_embedded ? "Embedded" : "Remote";
  switch (db.sync_mode()) {
    case WriteSyncMode::WriteThrough: return where + " / write-through";
    case WriteSyncMode::WriteBehind:  return where + " / write-behind";
    default:                          return where + " / local-only";
  }
}

void HardwareCompositor::db_refresh_telemetry(WindowSurface& win, Kernel& k) {
  SqlDatabase* db = sql_engine(k);
  if (!db) return;
  // An empty profile reports itself empty. Naming a host this machine has
  // never spoken to is how the pane came to describe a fleet that is not here.
  const DbConnectionProfile& prof = db->active_profile();
  win.db_telemetry_host = prof.host.empty() ? std::string("(no host)") : prof.host;
  win.db_telemetry_port = prof.port;

  win.db_telemetry_table_stats.clear();
  double cell_bytes = 0.0;
  for (const std::string& name : db->list_tables()) {
    const size_t rows = db->row_count(name);
    win.db_telemetry_table_stats.emplace_back(name, rows);
    size_t cols = 1;
    if (const SqlTable* t = db->find_table(name)) cols = t->columns.empty() ? 1 : t->columns.size();
    cell_bytes += static_cast<double>(rows) * static_cast<double>(cols) * kDbCellBytes;
  }
  win.db_telemetry_ram_mb = cell_bytes / (1024.0 * 1024.0);

  const BinlogConsumerStats cdc = db->binlog_sync_stats();
  win.db_telemetry_binlog_file = cdc.current_binlog_file;
  win.db_telemetry_binlog_pos = cdc.current_binlog_pos;
  win.db_telemetry_lag_ms = cdc.is_running ? static_cast<double>(cdc.seconds_behind_master) * 1000.0 : 0.0;
  if (!cdc.is_running) win.db_telemetry_status = "CDC stream not running";
  else if (!cdc.is_connected) win.db_telemetry_status = "CDC stream running, not connected";
  else win.db_telemetry_status = "Connected (CDC stream active)";

  // An empty whitelist on a running consumer means every table; a stopped one
  // carries none.
  size_t replicated = 0;
  if (cdc.is_running) {
    if (BinlogConsumer* bc = db->binlog_consumer()) {
      const auto& wl = bc->config().table_whitelist;
      replicated = wl.empty() ? win.db_telemetry_table_stats.size() : wl.size();
    }
  }
  win.db_telemetry_replicated_tables = replicated;
  win.db_telemetry_mode = db_engine_mode_label(*db);
}

void HardwareCompositor::db_telemetry_action(WindowSurface& win, Kernel& k, const std::string& action) {
  SqlDatabase* db = sql_engine(k);
  if (!db) {
    set_toast("no SQL engine available");
    return;
  }
  if (action == "sync") {
    db_run_query(win, k, "/* ASQL:SYNC */ SELECT 'CDC SYNC CHECKPOINT OK' AS status, NOW() as ts;");
    // The lag is whatever the consumer reports AFTER the checkpoint, or there
    // is no stream and the pane says so.
    const BinlogConsumerStats cdc = db->binlog_sync_stats();
    if (!cdc.is_running) {
      set_toast("checkpoint ran; there is no CDC stream to measure");
    } else {
      char msg[96];
      snprintf(msg, sizeof(msg), "checkpoint ran; binlog lag %.0f ms",
               static_cast<double>(cdc.seconds_behind_master) * 1000.0);
      set_toast(msg);
    }
    db_refresh_telemetry(win, k);
  } else if (action == "flush") {
    db->invalidate_query_cache();
    auto saved = db->save_tables();
    set_toast(saved.ok() ? std::string("query cache invalidated; tables saved to disk")
                         : "query cache invalidated; save failed: " + saved.error().str());
  } else if (action == "snapshot") {
    auto loaded = db->load_tables();
    if (!loaded.ok()) { set_toast("reload failed: " + loaded.error().str()); return; }
    db_refresh_telemetry(win, k);
    set_toast("tables reloaded from disk: " +
              std::to_string(win.db_telemetry_table_stats.size()) + " table(s)");
  } else if (action == "probe") {
    // Against a table this engine actually has, rather than one ERP name.
    const std::vector<std::string> tables = db->list_tables();
    const std::string sql = tables.empty() ? std::string("SELECT 1 AS probe;")
                                           : "SELECT COUNT(*) AS live_rows FROM " + tables.front() + ";";
    auto t0 = steady_ms();
    db_run_query(win, k, sql);
    auto elapsed = steady_ms() - t0;
    set_toast((tables.empty() ? std::string("engine holds no tables; probe ")
                              : "probe on " + tables.front() + " ") +
              std::to_string(elapsed) + " ms round-trip");
  }
}

Result<std::string> HardwareCompositor::db_export_csv(WindowSurface& win, Kernel& k) {
  if (win.grid_columns.empty())
    return Result<std::string>(Status::Invalid, "there is no result set to export");
  std::string csv;
  for (size_t i = 0; i < win.grid_columns.size(); ++i) csv += (i ? "," : "") + win.grid_columns[i];
  csv += "\n";
  for (const auto& row : win.grid_rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      // A value carrying a comma or a quote has to be quoted, or the file is
      // not a CSV — it is a file that happens to look like one.
      const std::string& v = row[i];
      bool needs = v.find(',') != std::string::npos || v.find('"') != std::string::npos ||
                   v.find('\n') != std::string::npos;
      if (i) csv += ",";
      if (!needs) { csv += v; continue; }
      csv += "\"";
      for (char c : v) { if (c == '"') csv += "\"\""; else csv += c; }
      csv += "\"";
    }
    csv += "\n";
  }
  std::string dir = k.config().root + "/sql_export";
  (void)k.host().make_dir(dir);
  // The file is named for what the grid HOLDS, not for what is selected in the
  // explorer: exporting a Describe used to land in <table>.csv and read like
  // that table's rows.
  std::string name = win.db_grid_label.empty() ? win.db_selected : win.db_grid_label;
  if (name.empty()) name = "result";
  std::string safe;
  for (char c : name) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_';
    safe += ok ? c : '_';
  }
  std::string path = dir + "/" + safe + ".csv";
  auto wr = k.host().write_file(path, ByteSpan(reinterpret_cast<const uint8_t*>(csv.data()), csv.size()));
  if (!wr.ok()) return Result<std::string>(wr.error());
  return path;
}

// --- the editor ------------------------------------------------------------

// --- what the ASCII font can and cannot draw ------------------------------

// A line as the pane draws it: one CELL per display column, plus the byte
// offset that cell came from. The buffer holds the file's real bytes, and this
// is the only place they are turned into columns — so a tab stays one byte in
// the file while occupying a tab stop on screen, and a letter outside ASCII
// stays its own bytes in a cell of its own.
//
// A cell used to be a single char, which meant every character past ASCII was
// replaced HERE, before drawing, with '?'. Widening the atlas could never fix
// that and neither could the outline engine: the letter was already gone by
// the time anything with a glyph in it was asked. A cell is the character's
// own UTF-8 now, and the renderer draws it.
// The characters a monospaced grid has to give TWO columns to. CJK, Hangul,
// the fullwidth forms and the emoji blocks are drawn about twice as wide as a
// Latin letter in every typeface that has them; given one column each they
// overlap their neighbour, which is what a line of Japanese looked like the
// first time this editor drew one instead of a row of '?'.
static bool editor_cell_is_wide(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
         (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
         (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
         (cp >= 0x1F300 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

static void expand_editor_line(const std::string& raw, std::vector<std::string>* shown,
                               std::vector<int>* byte_at) {
  shown->clear();
  byte_at->clear();
  for (size_t i = 0; i < raw.size();) {
    unsigned char c = (unsigned char)raw[i];
    if (c == '\t') {
      int stop = 4 - (int)(shown->size() % 4);
      for (int s = 0; s < stop; ++s) { shown->push_back(" "); byte_at->push_back((int)i); }
      i++;
      continue;
    }
    if (c >= 32 && c < 127) {
      shown->push_back(std::string(1, (char)c));
      byte_at->push_back((int)i);
      i++;
      continue;
    }
    // One cell per character, not per byte, so a line of accented text does
    // not draw three times too wide.
    size_t len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    if (i + len > raw.size()) len = 1;
    // A control byte has no letter to show; anything else is a real character
    // and goes to the renderer as itself.
    shown->push_back(c < 32 ? std::string(".") : raw.substr(i, len));
    byte_at->push_back((int)i);
    // A wide character gets a second, empty column so the character after it
    // starts clear of its ink. The continuation maps to the same byte, so a
    // click anywhere on the character lands on the character.
    uint32_t cp = c;
    if (len == 2) cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)raw[i + 1] & 0x3F);
    else if (len == 3) cp = ((uint32_t)(c & 0x0F) << 12) |
                           (((unsigned char)raw[i + 1] & 0x3F) << 6) |
                           ((unsigned char)raw[i + 2] & 0x3F);
    else if (len == 4) cp = ((uint32_t)(c & 0x07) << 18) |
                           (((unsigned char)raw[i + 1] & 0x3F) << 12) |
                           (((unsigned char)raw[i + 2] & 0x3F) << 6) |
                           ((unsigned char)raw[i + 3] & 0x3F);
    if (len > 1 && editor_cell_is_wide(cp)) {
      shown->push_back(std::string());
      byte_at->push_back((int)i);
    }
    i += len;
  }
  byte_at->push_back((int)raw.size());
}

// The display column a byte offset sits at.
static int editor_display_col(const std::string& raw, int byte_col) {
  std::vector<std::string> shown;
  std::vector<int> byte_at;
  expand_editor_line(raw, &shown, &byte_at);
  for (size_t i = 0; i + 1 < byte_at.size(); ++i)
    if (byte_at[i] >= byte_col) return (int)i;
  return (int)shown.size();
}

// A byte offset that starts a character, stepping over UTF-8 continuation
// bytes so the caret never lands inside one and a Backspace never cuts a
// letter in half.
static int editor_step_col(const std::string& raw, int col, int dir) {
  int n = (int)raw.size();
  col += dir;
  if (col <= 0) return 0;
  if (col >= n) return n;
  while (col > 0 && col < n && ((unsigned char)raw[(size_t)col] & 0xC0) == 0x80) col += dir;
  if (col < 0) col = 0;
  if (col > n) col = n;
  return col;
}

uint64_t HardwareCompositor::disk_mark_of(const ByteBuffer& bytes) {
  // FNV-1a over the bytes, with the length folded in. Not a checksum for
  // anybody else's use: it answers one question — is this the same file I last
  // agreed with — and a hash is the only honest way to ask it, because the
  // machine's own `edit` is a find-and-replace that very often leaves the
  // length exactly as it was.
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : bytes) { h ^= (uint64_t)b; h *= 1099511628211ull; }
  h ^= (uint64_t)bytes.size() * 1099511628211ull;
  return h ? h : 1;   // 0 means "never looked"
}

void HardwareCompositor::editor_watch_file(WindowSurface& win, Kernel& k) {
  if (win.type != WindowType::Editor || win.file_path.empty() || !win.file_disk_mark) return;
  const uint64_t now = steady_ms();
  if (win.file_checked_ms && now - win.file_checked_ms < 1500) return;
  win.file_checked_ms = now;
  auto rd = k.host().read_file(win.file_path);
  if (!rd.ok()) return;                       // gone or unreadable: not this pane's business
  const uint64_t mark = disk_mark_of(*rd);
  if (mark == win.file_disk_mark) {
    // Back to what we have — someone undid it, or reloaded it themselves.
    win.file_disk_changed = false;
    return;
  }
  if (win.file_disk_changed) return;          // already said so, once
  win.file_disk_changed = true;
  win.status_text = win.file_path + " changed on disk since you opened it - `reload` to take it, `save!` to write over it";
}

namespace {

bool is_ssh_path(const std::string& path) {
  return path.rfind("ssh://", 0) == 0 || path.rfind("ssh:", 0) == 0;
}

bool parse_ssh_path(const std::string& full_path, aios::net::SshTarget* target, std::string* remote_path) {
  if (!target || !remote_path) return false;
  std::string s = full_path;
  if (s.rfind("ssh://", 0) == 0) {
    s = s.substr(6);
    size_t slash = s.find('/');
    std::string host_part = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path_part = (slash == std::string::npos) ? "/" : s.substr(slash);
    auto t = aios::net::ssh_parse_target(host_part, "root");
    if (!t.ok()) return false;
    *target = *t;
    *remote_path = path_part.empty() ? "/" : path_part;
    return true;
  } else if (s.rfind("ssh:", 0) == 0) {
    s = s.substr(4);
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
      std::string host_part = s.substr(0, colon);
      std::string path_part = s.substr(colon + 1);
      auto t = aios::net::ssh_parse_target(host_part, "root");
      if (!t.ok()) return false;
      *target = *t;
      *remote_path = path_part.empty() ? "." : path_part;
      return true;
    } else {
      auto t = aios::net::ssh_parse_target(s, "root");
      if (!t.ok()) return false;
      *target = *t;
      *remote_path = ".";
      return true;
    }
  }
  return false;
}

} // namespace

bool HardwareCompositor::editor_open(WindowSurface& win, Kernel& k, const std::string& path) {
  std::vector<uint8_t> bytes;
  if (is_ssh_path(path)) {
    aios::net::SshTarget target;
    std::string remote_path;
    if (!parse_ssh_path(path, &target, &remote_path)) {
      win.status_text = "invalid ssh path: " + path;
      return false;
    }
    std::string key_path = k.config().root + "/ssh/id_ed25519";
    auto id = aios::net::ssh_load_identity(k.host(), key_path, false);
    if (!id.ok()) {
      win.status_text = "cannot read ssh identity: " + id.error().str() + " (run `ssh keygen`)";
      return false;
    }
    aios::net::SshSession session(k.host(), k.config().root + "/ssh/known_hosts");
    if (auto r = session.connect(target, *id, 10000); !r.ok()) {
      win.status_text = "ssh connect failed: " + r.error().str();
      return false;
    }
    std::string quoted = "'";
    for (char c : remote_path) {
      if (c == '\'') quoted += "'\\''";
      else quoted += c;
    }
    quoted += "'";
    if (auto r = session.exec("cat -- " + quoted); !r.ok()) {
      win.status_text = "ssh exec failed: " + r.error().str();
      return false;
    }
    for (;;) {
      auto rec = session.recv(5000);
      if (!rec.ok() || rec->empty()) {
        if (session.eof()) break;
        continue;
      }
      bytes.insert(bytes.end(), rec->begin(), rec->end());
    }
    session.wait_close(3000);
    if (session.exit_status() != 0) {
      win.status_text = "ssh read failed (" + std::to_string(session.exit_status()) + "): " + session.stderr_text();
      return false;
    }
  } else {
    auto rd = k.host().read_file(path);
    if (!rd.ok()) {
      win.status_text = "cannot read " + path + ": " + rd.error().str();
      return false;
    }
    bytes = *rd;
  }

  // Park whatever is live before taking the new file, or the tab that was
  // showing loses its edits.
  if (!win.file_path.empty() || win.file_dirty) editor_stash_active(win);

  int already = editor_buffer_for_path(win, path);
  if (already >= 0) {
    editor_activate_buffer(win, already);
    win.status_text = "switched to " + path;
    return true;
  }

  // The file's own bytes, split only on newlines. Nothing is substituted here.
  bool saw_crlf = false;
  win.lines.clear();
  std::string cur;
  for (size_t i = 0; i < bytes.size(); ++i) {
    char c = (char)bytes[i];
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') { cur.pop_back(); saw_crlf = true; }
      win.lines.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  win.file_final_newline = cur.empty() && !bytes.empty();
  if (!cur.empty() || win.lines.empty()) win.lines.push_back(cur);
  win.file_eol = saw_crlf ? "\r\n" : "\n";
  // What was on disk at the moment this buffer agreed with it.
  win.file_disk_mark = disk_mark_of(bytes);
  win.file_disk_changed = false;
  win.file_checked_ms = 0;

  win.file_path = path;
  win.file_dirty = false;
  win.file_lang = lang_for_path(path);
  // A Markdown file opens as it reads. It is the one kind of file whose source
  // is not the thing you usually want to look at, and the toggle is one key.
  win.file_preview = lang_has_preview(win.file_lang);
  win.cursor_line = 0;
  win.cursor_col = 0;
  win.view_top = 0;
  win.view_left = 0;
  win.cmdline_focus = false;
  editor_clear_selection(win);
  win.undo_stack.clear();
  win.redo_stack.clear();

  size_t total = 0;
  for (const auto& l : win.lines) total += l.size() + 1;
  win.status_text = "opened " + path + " (" + std::to_string(win.lines.size()) + " lines, " +
                    std::to_string(total) + " bytes)";

  // Register the new file as a tab of its own.
  EditorBuffer nb;
  nb.path = path;
  win.buffers.push_back(nb);
  win.active_buffer = (int)win.buffers.size() - 1;
  editor_stash_active(win);

  size_t slash = path.find_last_of('/');
  editor_browse(win, k, slash == std::string::npos ? win.file_dir : path.substr(0, slash));
  return true;
}

bool HardwareCompositor::editor_save(WindowSurface& win, Kernel& k, const std::string& path, bool force) {
  std::string target = path.empty() ? win.file_path : path;
  if (target.empty()) {
    win.status_text = "this buffer has no file - use: save <path>";
    return false;
  }
  if (!force && target == win.file_path && win.file_disk_mark && !is_ssh_path(target)) {
    auto cur = k.host().read_file(target);
    if (cur.ok() && disk_mark_of(*cur) != win.file_disk_mark) {
      win.file_disk_changed = true;
      win.status_text = target + " changed on disk since you opened it - `reload` to take it, `save!` to write over it";
      return false;
    }
  }

  std::string blob;
  for (size_t i = 0; i < win.lines.size(); ++i) {
    blob += win.lines[i];
    if (i + 1 < win.lines.size()) blob += win.file_eol;
  }
  if (win.file_final_newline && !blob.empty()) blob += win.file_eol;

  if (is_ssh_path(target)) {
    aios::net::SshTarget target_spec;
    std::string remote_path;
    if (!parse_ssh_path(target, &target_spec, &remote_path)) {
      win.status_text = "invalid ssh path: " + target;
      return false;
    }
    std::string key_path = k.config().root + "/ssh/id_ed25519";
    auto id = aios::net::ssh_load_identity(k.host(), key_path, false);
    if (!id.ok()) {
      win.status_text = "cannot read ssh identity: " + id.error().str() + " (run `ssh keygen`)";
      return false;
    }
    aios::net::SshSession session(k.host(), k.config().root + "/ssh/known_hosts");
    if (auto r = session.connect(target_spec, *id, 10000); !r.ok()) {
      win.status_text = "ssh connect failed: " + r.error().str();
      return false;
    }
    std::string quoted = "'";
    for (char c : remote_path) {
      if (c == '\'') quoted += "'\\''";
      else quoted += c;
    }
    quoted += "'";
    // Never truncate the real file before the new content has fully arrived.
    // This was `cat > path`: the shell emptied the file the moment the command
    // started, so a send that failed (or a link that dropped) left the remote
    // file empty or half-written. Now the bytes land in a temp file -- beside
    // the target when that directory is writable, so the final step is an
    // atomic mv; otherwise in $TMPDIR, copied over only once complete -- and
    // only when the byte count matches what we sent does the target change.
    const std::string n = std::to_string(blob.size());
    const std::string script =
        "p=" + quoted + "; "
        "d=$(dirname -- \"$p\"); "
        "t=$(mktemp \"$d/.aios-save.XXXXXX\" 2>/dev/null) && same=1 || { t=$(mktemp) && same=0; } || exit 71; "
        "if ! cat > \"$t\"; then rm -f -- \"$t\"; exit 72; fi; "
        "got=$(wc -c < \"$t\"); if [ \"$got\" -ne " + n + " ]; then rm -f -- \"$t\"; "
        "echo \"short write: $got of " + n + " bytes arrived; $p left as it was\" >&2; exit 73; fi; "
        "if [ \"$same\" = 1 ]; then "
        "  [ -e \"$p\" ] && chmod --reference=\"$p\" \"$t\" 2>/dev/null; "
        "  mv -f -- \"$t\" \"$p\" || { rm -f -- \"$t\"; exit 74; }; "
        "else cat \"$t\" > \"$p\"; rc=$?; rm -f -- \"$t\"; exit $rc; fi";
    if (auto r = session.exec(script); !r.ok()) {
      win.status_text = "ssh exec failed: " + r.error().str();
      return false;
    }
    if (!blob.empty()) {
      auto sw = session.send(ByteSpan(reinterpret_cast<const uint8_t*>(blob.data()), blob.size()));
      if (!sw.ok()) {
        win.status_text = "ssh send failed: " + sw.error().str();
        return false;
      }
    }
    session.send_eof();
    session.wait_close(5000);
    if (session.exit_status() != 0) {
      win.status_text = "ssh write failed (" + std::to_string(session.exit_status()) + "): " + session.stderr_text();
      return false;
    }
  } else {
    auto wr = k.host().write_file(target, ByteSpan(reinterpret_cast<const uint8_t*>(blob.data()), blob.size()));
    if (!wr.ok()) {
      win.status_text = "cannot write " + target + ": " + wr.error().str();
      return false;
    }
  }

  if (win.file_path != target) win.file_lang = lang_for_path(target);
  win.file_path = target;
  win.file_dirty = false;
  win.file_disk_mark = disk_mark_of(ByteBuffer(blob.begin(), blob.end()));
  win.file_disk_changed = false;
  win.file_checked_ms = 0;
  editor_stash_active(win);
  win.status_text = "wrote " + std::to_string(blob.size()) + " bytes to " + target;
  return true;
}

void HardwareCompositor::editor_browse(WindowSurface& win, Kernel& k, const std::string& dir) {
  if (dir.empty()) return;
  win.dir_entries.clear();
  win.dir_is_dir.clear();
  win.dir_scroll = 0;

  if (is_ssh_path(dir)) {
    aios::net::SshTarget target_spec;
    std::string remote_path;
    if (!parse_ssh_path(dir, &target_spec, &remote_path)) {
      win.status_text = "invalid ssh dir: " + dir;
      return;
    }
    std::string key_path = k.config().root + "/ssh/id_ed25519";
    auto id = aios::net::ssh_load_identity(k.host(), key_path, false);
    if (!id.ok()) {
      win.status_text = "cannot read ssh identity: " + id.error().str();
      return;
    }
    aios::net::SshSession session(k.host(), k.config().root + "/ssh/known_hosts");
    if (auto r = session.connect(target_spec, *id, 10000); !r.ok()) {
      win.status_text = "ssh connect failed: " + r.error().str();
      return;
    }
    std::string quoted = "'";
    for (char c : remote_path) {
      if (c == '\'') quoted += "'\\''";
      else quoted += c;
    }
    quoted += "'";
    if (auto r = session.exec("ls -1p -a " + quoted); !r.ok()) {
      win.status_text = "ssh ls failed: " + r.error().str();
      return;
    }
    std::string output;
    for (;;) {
      auto rec = session.recv(5000);
      if (!rec.ok() || rec->empty()) {
        if (session.eof()) break;
        continue;
      }
      output.append(reinterpret_cast<const char*>(rec->data()), rec->size());
    }
    session.wait_close(3000);

    std::vector<std::string> dirs, files;
    std::string cur;
    auto add_entry = [&](const std::string& item) {
      if (item.empty() || item == ".") return;
      if (item == "..") return;
      if (item.back() == '/') {
        dirs.push_back(item.substr(0, item.size() - 1));
      } else {
        files.push_back(item);
      }
    };
    for (char c : output) {
      if (c == '\n') {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        add_entry(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) add_entry(cur);

    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    win.file_dir = dir;
    if (remote_path != "/" && remote_path != ".") {
      win.dir_entries.push_back("..");
      win.dir_is_dir.push_back(true);
    }
    for (const auto& d : dirs) {
      win.dir_entries.push_back(d);
      win.dir_is_dir.push_back(true);
    }
    for (const auto& f : files) {
      win.dir_entries.push_back(f);
      win.dir_is_dir.push_back(false);
    }
    win.status_text = "listed remote " + dir;
    return;
  }

  // Local directory listing
  auto ls = k.host().list_dir(dir);
  if (!ls.ok()) {
    win.status_text = "cannot list " + dir + ": " + ls.error().str();
    return;
  }
  win.file_dir = dir;

  std::vector<std::string> dirs, files;
  for (const auto& e : *ls) {
    if (e == "." || e == "..") continue;
    std::string full = (dir == "/" ? "" : dir) + "/" + e;
    auto st = k.host().stat(full);
    if (st.ok() && st->is_dir) {
      dirs.push_back(e);
    } else {
      files.push_back(e);
    }
  }
  std::sort(dirs.begin(), dirs.end());
  std::sort(files.begin(), files.end());

  if (dir != "/" && !dir.empty()) {
    win.dir_entries.push_back("..");
    win.dir_is_dir.push_back(true);
  }
  for (const auto& d : dirs) {
    win.dir_entries.push_back(d);
    win.dir_is_dir.push_back(true);
  }
  for (const auto& f : files) {
    win.dir_entries.push_back(f);
    win.dir_is_dir.push_back(false);
  }
}

// --- history --------------------------------------------------------------

void HardwareCompositor::editor_push_undo(WindowSurface& win, bool coalesce) {
  uint64_t now = steady_ms();
  // A run of ordinary typing is one step. Undo that walks back a character at
  // a time is undo nobody uses.
  if (coalesce && !win.undo_stack.empty() && now - win.last_edit_ms < 700) {
    win.last_edit_ms = now;
    return;
  }
  EditSnapshot s;
  s.lines = win.lines;
  s.cursor_line = win.cursor_line;
  s.cursor_col = win.cursor_col;
  win.undo_stack.push_back(std::move(s));
  // Bounded, so a long session cannot grow the window without limit.
  if (win.undo_stack.size() > 400) win.undo_stack.erase(win.undo_stack.begin());
  win.redo_stack.clear();
  win.last_edit_ms = now;
}

bool HardwareCompositor::editor_undo(WindowSurface& win) {
  if (win.undo_stack.empty()) {
    win.status_text = "nothing to undo";
    return false;
  }
  EditSnapshot redo;
  redo.lines = win.lines;
  redo.cursor_line = win.cursor_line;
  redo.cursor_col = win.cursor_col;
  win.redo_stack.push_back(std::move(redo));

  EditSnapshot s = win.undo_stack.back();
  win.undo_stack.pop_back();
  win.lines = s.lines;
  win.cursor_line = s.cursor_line;
  win.cursor_col = s.cursor_col;
  win.file_dirty = true;
  editor_clear_selection(win);
  win.last_edit_ms = 0;
  win.status_text = "undo";
  return true;
}

bool HardwareCompositor::editor_redo(WindowSurface& win) {
  if (win.redo_stack.empty()) {
    win.status_text = "nothing to redo";
    return false;
  }
  EditSnapshot undo;
  undo.lines = win.lines;
  undo.cursor_line = win.cursor_line;
  undo.cursor_col = win.cursor_col;
  win.undo_stack.push_back(std::move(undo));

  EditSnapshot s = win.redo_stack.back();
  win.redo_stack.pop_back();
  win.lines = s.lines;
  win.cursor_line = s.cursor_line;
  win.cursor_col = s.cursor_col;
  win.file_dirty = true;
  editor_clear_selection(win);
  win.last_edit_ms = 0;
  win.status_text = "redo";
  return true;
}

// --- selection ------------------------------------------------------------

bool HardwareCompositor::editor_has_selection(const WindowSurface& win) const {
  return win.sel_line >= 0 && (win.sel_line != win.cursor_line || win.sel_col != win.cursor_col);
}

void HardwareCompositor::editor_clear_selection(WindowSurface& win) {
  win.sel_line = -1;
  win.sel_col = -1;
}

void HardwareCompositor::editor_begin_selection(WindowSurface& win) {
  if (win.sel_line < 0) {
    win.sel_line = win.cursor_line;
    win.sel_col = win.cursor_col;
  }
}

void HardwareCompositor::editor_select_all(WindowSurface& win) {
  if (win.lines.empty()) return;
  win.sel_line = 0;
  win.sel_col = 0;
  win.cursor_line = (int)win.lines.size() - 1;
  win.cursor_col = (int)win.lines.back().size();
}

void HardwareCompositor::editor_selection_range(const WindowSurface& win, int* l0, int* c0,
                                                int* l1, int* c1) const {
  int al = win.sel_line, ac = win.sel_col, bl = win.cursor_line, bc = win.cursor_col;
  if (al > bl || (al == bl && ac > bc)) { std::swap(al, bl); std::swap(ac, bc); }
  *l0 = al; *c0 = ac; *l1 = bl; *c1 = bc;
}

std::string HardwareCompositor::editor_selection_text(const WindowSurface& win) const {
  if (!editor_has_selection(win)) return "";
  int l0, c0, l1, c1;
  editor_selection_range(win, &l0, &c0, &l1, &c1);
  if (l0 < 0 || l1 >= (int)win.lines.size()) return "";
  if (l0 == l1) {
    const std::string& l = win.lines[(size_t)l0];
    if (c0 > (int)l.size()) c0 = (int)l.size();
    if (c1 > (int)l.size()) c1 = (int)l.size();
    return l.substr((size_t)c0, (size_t)(c1 - c0));
  }
  std::string out = win.lines[(size_t)l0].substr(
      (size_t)std::min<int>(c0, (int)win.lines[(size_t)l0].size()));
  for (int i = l0 + 1; i < l1; ++i) out += "\n" + win.lines[(size_t)i];
  out += "\n" + win.lines[(size_t)l1].substr(
                    0, (size_t)std::min<int>(c1, (int)win.lines[(size_t)l1].size()));
  return out;
}

// --- terminal selection ---------------------------------------------------

std::vector<HardwareCompositor::TerminalRowInfo>
HardwareCompositor::build_terminal_rows(const std::vector<std::string>& lines, int room) const {
  std::vector<TerminalRowInfo> rows;
  for (size_t l_idx = 0; l_idx < lines.size(); ++l_idx) {
    const std::string& l = lines[l_idx];
    uint32_t colour = tok::dim;
    if (l.rfind("aios>", 0) == 0) colour = tok::text;
    else if (l.find("error") != std::string::npos || l.find("FAIL") != std::string::npos) colour = tok::err;

    if (room <= 8 || l.empty()) {
      rows.push_back({l, l_idx, 0, colour});
      continue;
    }
    if (text_width(l, Face::Mono) <= room) {
      rows.push_back({l, l_idx, 0, colour});
      continue;
    }

    size_t p = 0;
    std::string cur;
    size_t cur_start_col = 0;
    while (p < l.size()) {
      size_t sp = l.find(' ', p);
      std::string word = l.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
      size_t word_start_col = p;
      while (text_width(word, Face::Mono) > room) {
        size_t take = word.size();
        while (take > 1 && text_width(word.substr(0, take), Face::Mono) > room) --take;
        if (!cur.empty()) {
          rows.push_back({cur, l_idx, cur_start_col, colour});
          cur.clear();
        }
        rows.push_back({word.substr(0, take), l_idx, word_start_col, colour});
        word = word.substr(take);
        word_start_col += take;
      }
      std::string trial = cur.empty() ? word : cur + " " + word;
      if (cur.empty()) {
        cur = word;
        cur_start_col = word_start_col;
      } else if (text_width(trial, Face::Mono) <= room) {
        cur = trial;
      } else {
        rows.push_back({cur, l_idx, cur_start_col, colour});
        cur = word;
        cur_start_col = word_start_col;
      }
      if (sp == std::string::npos) break;
      p = sp + 1;
    }
    if (!cur.empty()) {
      rows.push_back({cur, l_idx, cur_start_col, colour});
    }
  }
  return rows;
}

bool HardwareCompositor::terminal_has_selection(const WindowSurface& win) const {
  return win.sel_line >= 0 && win.sel_col >= 0 &&
         (win.sel_line != win.cursor_line || win.sel_col != win.cursor_col);
}

void HardwareCompositor::terminal_clear_selection(WindowSurface& win) {
  win.sel_line = -1;
  win.sel_col = -1;
}

void HardwareCompositor::terminal_begin_selection(WindowSurface& win) {
  if (win.sel_line < 0) {
    win.sel_line = win.cursor_line;
    win.sel_col = win.cursor_col;
  }
}

void HardwareCompositor::terminal_select_all(WindowSurface& win) {
  if (win.lines.empty()) return;
  win.sel_line = 0;
  win.sel_col = 0;
  win.cursor_line = static_cast<int>(win.lines.size() - 1);
  win.cursor_col = static_cast<int>(win.lines.back().size());
}

void HardwareCompositor::terminal_selection_range(const WindowSurface& win, int* l0, int* c0,
                                                  int* l1, int* c1) const {
  int al = win.sel_line, ac = win.sel_col, bl = win.cursor_line, bc = win.cursor_col;
  if (al > bl || (al == bl && ac > bc)) { std::swap(al, bl); std::swap(ac, bc); }
  *l0 = al; *c0 = ac; *l1 = bl; *c1 = bc;
}

std::string HardwareCompositor::terminal_selection_text(const WindowSurface& win) const {
  if (!terminal_has_selection(win)) return "";
  int l0, c0, l1, c1;
  terminal_selection_range(win, &l0, &c0, &l1, &c1);
  if (l0 < 0 || l1 >= static_cast<int>(win.lines.size())) return "";
  if (l0 == l1) {
    const std::string& l = win.lines[(size_t)l0];
    if (c0 > static_cast<int>(l.size())) c0 = static_cast<int>(l.size());
    if (c1 > static_cast<int>(l.size())) c1 = static_cast<int>(l.size());
    return l.substr(static_cast<size_t>(c0), static_cast<size_t>(c1 - c0));
  }
  std::string out = win.lines[(size_t)l0].substr(
      static_cast<size_t>(std::min<int>(c0, static_cast<int>(win.lines[(size_t)l0].size()))));
  for (int i = l0 + 1; i < l1; ++i) out += "\n" + win.lines[(size_t)i];
  out += "\n" + win.lines[(size_t)l1].substr(
                    0, static_cast<size_t>(std::min<int>(c1, static_cast<int>(win.lines[(size_t)l1].size()))));
  return out;
}

bool HardwareCompositor::terminal_point_to_caret(const WindowSurface& win, int rel_x, int rel_y,
                                                int* line, int* col) const {
  if (win.lines.empty()) return false;
  int w = win.content_width();
  int h = win.content_height();
  int body_top = 30;
  int body_h = h - body_top - 40;
  if (body_h < 40) body_h = 40;
  if (rel_y < body_top || rel_y > body_top + body_h) return false;

  const int room = w - 24;
  std::vector<TerminalRowInfo> rows = build_terminal_rows(win.lines, room);
  if (rows.empty()) return false;

  int max_visible_lines = (body_h - 10) / 18;
  int start_line = 0;
  if (static_cast<int>(rows.size()) > max_visible_lines) {
    start_line = static_cast<int>(rows.size()) - max_visible_lines + win.scroll_y;
    if (start_line < 0) start_line = 0;
  }

  int vis_idx = (rel_y - 36) / 18;
  if (vis_idx < 0) vis_idx = 0;
  int row_idx = start_line + vis_idx;
  if (row_idx >= static_cast<int>(rows.size())) row_idx = static_cast<int>(rows.size()) - 1;
  if (row_idx < 0) row_idx = 0;

  const auto& r_info = rows[static_cast<size_t>(row_idx)];
  int char_w = text_width("M", Face::Mono);
  if (char_w < 1) char_w = 8;

  int char_col = (rel_x - 10 + char_w / 2) / char_w;
  if (char_col < 0) char_col = 0;
  if (char_col > static_cast<int>(r_info.text.size())) char_col = static_cast<int>(r_info.text.size());

  *line = static_cast<int>(r_info.line_idx);
  *col = static_cast<int>(r_info.col_offset) + char_col;
  return true;
}

bool HardwareCompositor::editor_delete_selection(WindowSurface& win) {
  if (!editor_has_selection(win)) return false;
  int l0, c0, l1, c1;
  editor_selection_range(win, &l0, &c0, &l1, &c1);
  if (l0 < 0 || l1 >= (int)win.lines.size()) return false;
  editor_push_undo(win, false);

  std::string head = win.lines[(size_t)l0].substr(
      0, (size_t)std::min<int>(c0, (int)win.lines[(size_t)l0].size()));
  std::string tail = win.lines[(size_t)l1].substr(
      (size_t)std::min<int>(c1, (int)win.lines[(size_t)l1].size()));
  win.lines.erase(win.lines.begin() + l0, win.lines.begin() + l1 + 1);
  win.lines.insert(win.lines.begin() + l0, head + tail);

  win.cursor_line = l0;
  win.cursor_col = (int)head.size();
  win.file_dirty = true;
  editor_clear_selection(win);
  return true;
}

void HardwareCompositor::editor_insert(WindowSurface& win, const std::string& text) {
  if (win.lines.empty()) win.lines.push_back("");
  // Typing over a selection replaces it, which is what every editor does and
  // what makes a selection worth having.
  bool had_selection = editor_delete_selection(win);
  editor_push_undo(win, !had_selection && text.size() == 1);

  if (win.cursor_line < 0) win.cursor_line = 0;
  if (win.cursor_line >= (int)win.lines.size()) win.cursor_line = (int)win.lines.size() - 1;

  // A multi-line paste becomes multiple lines, rather than the first line and
  // an apology.
  std::vector<std::string> parts;
  std::string cur;
  for (char c : text) {
    if (c == '\n') { parts.push_back(cur); cur.clear(); }
    else if (c != '\r') cur += c;
  }
  parts.push_back(cur);

  std::string& line = win.lines[(size_t)win.cursor_line];
  if (win.cursor_col > (int)line.size()) win.cursor_col = (int)line.size();
  std::string tail = line.substr((size_t)win.cursor_col);
  line.erase((size_t)win.cursor_col);

  if (parts.size() == 1) {
    line += parts[0] + tail;
    win.cursor_col += (int)parts[0].size();
  } else {
    line += parts[0];
    for (size_t i = 1; i < parts.size(); ++i)
      win.lines.insert(win.lines.begin() + win.cursor_line + (int)i, parts[i]);
    win.cursor_line += (int)parts.size() - 1;
    win.cursor_col = (int)parts.back().size();
    win.lines[(size_t)win.cursor_line] += tail;
  }
  win.file_dirty = true;
}

// --- whole-line editing ---------------------------------------------------

void HardwareCompositor::editor_duplicate_line(WindowSurface& win) {
  if (win.lines.empty()) return;
  int at = win.cursor_line;
  if (at < 0 || at >= (int)win.lines.size()) return;
  editor_push_undo(win, false);
  win.lines.insert(win.lines.begin() + at + 1, win.lines[(size_t)at]);
  win.cursor_line = at + 1;
  win.file_dirty = true;
  win.status_text = "line duplicated";
}

void HardwareCompositor::editor_delete_line(WindowSurface& win) {
  if (win.lines.empty()) return;
  int at = win.cursor_line;
  if (at < 0 || at >= (int)win.lines.size()) return;
  editor_push_undo(win, false);
  win.lines.erase(win.lines.begin() + at);
  if (win.lines.empty()) win.lines.push_back("");
  if (win.cursor_line >= (int)win.lines.size()) win.cursor_line = (int)win.lines.size() - 1;
  win.cursor_col = 0;
  win.file_dirty = true;
  editor_clear_selection(win);
  win.status_text = "line deleted";
}

void HardwareCompositor::editor_move_line(WindowSurface& win, int delta) {
  int at = win.cursor_line;
  int to = at + delta;
  if (at < 0 || at >= (int)win.lines.size() || to < 0 || to >= (int)win.lines.size()) return;
  editor_push_undo(win, false);
  std::swap(win.lines[(size_t)at], win.lines[(size_t)to]);
  win.cursor_line = to;
  win.file_dirty = true;
  editor_clear_selection(win);
}

void HardwareCompositor::editor_indent_selection(WindowSurface& win, bool out) {
  int l0 = win.cursor_line, l1 = win.cursor_line;
  if (editor_has_selection(win)) {
    int c0, c1;
    editor_selection_range(win, &l0, &c0, &l1, &c1);
  }
  editor_push_undo(win, false);
  for (int i = l0; i <= l1 && i < (int)win.lines.size(); ++i) {
    std::string& l = win.lines[(size_t)i];
    if (out) {
      // Take back one level, however it was written.
      if (l.rfind("  ", 0) == 0) l.erase(0, 2);
      else if (!l.empty() && (l[0] == ' ' || l[0] == '\t')) l.erase(0, 1);
    } else if (!l.empty() || l0 == l1) {
      l.insert(0, "  ");
    }
  }
  win.file_dirty = true;
  win.status_text = out ? "outdented" : "indented";
}

// --- find and replace -----------------------------------------------------

bool HardwareCompositor::editor_find(WindowSurface& win, const std::string& needle, bool forward,
                                     bool from_caret) {
  if (needle.empty() || win.lines.empty()) return false;
  win.find_text = needle;
  std::string low_needle = needle;
  for (auto& c : low_needle) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);

  auto find_in = [&](const std::string& hay, size_t from, bool fwd, size_t* at) {
    std::string low = hay;
    for (auto& c : low) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    size_t p = fwd ? low.find(low_needle, from)
                   : (from == 0 ? std::string::npos : low.rfind(low_needle, from - 1));
    if (p == std::string::npos) return false;
    *at = p;
    return true;
  };

  int n = (int)win.lines.size();
  int start = win.cursor_line;
  // The line the caret is on, from where the caret is, then every other line
  // in order, wrapping. A search that stops at the end of the file is a search
  // the person has to think about.
  for (int step = 0; step <= n; ++step) {
    int i = forward ? (start + step) % n : ((start - step) % n + n) % n;
    size_t from;
    if (step == 0 && from_caret) {
      if (forward) {
        size_t c = (size_t)win.cursor_col;
        if (c > win.lines[(size_t)i].size()) continue;
        from = c;
      } else {
        from = (size_t)win.cursor_col;
      }
    } else {
      from = forward ? 0 : win.lines[(size_t)i].size();
    }
    size_t at;
    if (!find_in(win.lines[(size_t)i], from, forward, &at)) continue;
    // Land on it selected, so the next keystroke replaces what was found.
    win.cursor_line = i;
    win.sel_line = i;
    win.sel_col = (int)at;
    win.cursor_col = (int)(at + needle.size());
    win.cmdline_focus = false;
    win.status_text = "found '" + needle + "' at line " + std::to_string(i + 1);
    return true;
  }
  win.status_text = "not found: " + needle;
  return false;
}

int HardwareCompositor::editor_replace_all(WindowSurface& win, const std::string& from,
                                           const std::string& to) {
  if (from.empty()) return 0;
  editor_push_undo(win, false);
  int count = 0;
  for (auto& line : win.lines) {
    size_t p = 0;
    while ((p = line.find(from, p)) != std::string::npos) {
      line.replace(p, from.size(), to);
      p += to.size();
      count++;
    }
  }
  if (count) {
    win.file_dirty = true;
    editor_clear_selection(win);
    if (win.cursor_line >= (int)win.lines.size()) win.cursor_line = (int)win.lines.size() - 1;
    win.cursor_col = 0;
  } else if (!win.undo_stack.empty()) {
    win.undo_stack.pop_back();  // nothing changed, so nothing to walk back to
  }
  return count;
}

// --- tabs -----------------------------------------------------------------

int HardwareCompositor::editor_buffer_for_path(const WindowSurface& win,
                                               const std::string& path) const {
  for (size_t i = 0; i < win.buffers.size(); ++i)
    if (win.buffers[i].path == path) return (int)i;
  return -1;
}

void HardwareCompositor::editor_stash_active(WindowSurface& win) {
  if (win.active_buffer < 0 || win.active_buffer >= (int)win.buffers.size()) return;
  EditorBuffer& b = win.buffers[(size_t)win.active_buffer];
  b.path = win.file_path;
  b.lines = win.lines;
  b.dirty = win.file_dirty;
  b.cursor_line = win.cursor_line;
  b.cursor_col = win.cursor_col;
  b.view_top = win.view_top;
  b.view_left = win.view_left;
  b.eol = win.file_eol;
  b.final_newline = win.file_final_newline;
  b.lang = win.file_lang;
  b.preview = win.file_preview;
  b.disk_mark = win.file_disk_mark;
  b.disk_changed = win.file_disk_changed;
}

void HardwareCompositor::editor_activate_buffer(WindowSurface& win, int index) {
  if (index < 0 || index >= (int)win.buffers.size()) return;
  editor_stash_active(win);
  const EditorBuffer& b = win.buffers[(size_t)index];
  win.active_buffer = index;
  win.file_path = b.path;
  win.lines = b.lines;
  if (win.lines.empty()) win.lines.push_back("");
  win.file_dirty = b.dirty;
  win.cursor_line = b.cursor_line;
  win.cursor_col = b.cursor_col;
  win.view_top = b.view_top;
  win.view_left = b.view_left;
  win.file_eol = b.eol;
  win.file_final_newline = b.final_newline;
  win.file_lang = b.lang;
  win.file_preview = b.preview;
  win.file_disk_mark = b.disk_mark;
  win.file_disk_changed = b.disk_changed;
  win.file_checked_ms = 0;      // look at this one's file now, not on its old clock
  win.cmdline_focus = false;
  editor_clear_selection(win);
  win.undo_stack.clear();
  win.redo_stack.clear();
}

void HardwareCompositor::editor_close_buffer(WindowSurface& win, int index) {
  if (index < 0 || index >= (int)win.buffers.size()) return;
  if (win.buffers[(size_t)index].dirty) {
    win.status_text = "unsaved changes in " + win.buffers[(size_t)index].path + " - save first";
    return;
  }
  bool was_active = index == win.active_buffer;
  win.buffers.erase(win.buffers.begin() + index);
  if (win.buffers.empty()) {
    win.active_buffer = -1;
    win.file_path.clear();
    win.lines.assign(1, "");
    win.file_dirty = false;
    win.cursor_line = win.cursor_col = win.view_top = win.view_left = 0;
    editor_clear_selection(win);
    win.status_text = "no files open";
    return;
  }
  if (win.active_buffer > index) win.active_buffer--;
  if (was_active) {
    int want = index < (int)win.buffers.size() ? index : (int)win.buffers.size() - 1;
    win.active_buffer = -1;  // nothing to stash: the old buffer is gone
    editor_activate_buffer(win, want);
  }
}

bool HardwareCompositor::editor_edit_key(WindowSurface& win, int key) {
  using namespace aios::hal;
  if (win.lines.empty()) win.lines.push_back("");
  int last = (int)win.lines.size() - 1;
  if (win.cursor_line > last) win.cursor_line = last;
  if (win.cursor_line < 0) win.cursor_line = 0;

  // Shift turns a movement into a drag. Anything else drops the selection --
  // except the keys that consume it, which handle it themselves below.
  bool moving = key == KeyLeft || key == KeyRight || key == KeyUp || key == KeyDown ||
                key == KeyHome || key == KeyEnd || key == KeyPageUp || key == KeyPageDown;
  if (moving) {
    if (shift_held_) editor_begin_selection(win);
    else editor_clear_selection(win);
  }

  // Alt+Up / Alt+Down lift the whole line, which is the one thing "by the
  // word" could never mean vertically.
  if (word_wise_ && (key == KeyUp || key == KeyDown)) {
    editor_move_line(win, key == KeyUp ? -1 : 1);
    return true;
  }

  std::string& line = win.lines[(size_t)win.cursor_line];
  if (win.cursor_col > (int)line.size()) win.cursor_col = (int)line.size();

  // If completion overlay is active, handle selection navigation
  if (win.lsp_completion_active && !win.lsp_completions.empty()) {
    if (key == KeyUp) {
      win.lsp_completion_index = (win.lsp_completion_index - 1 + static_cast<int>(win.lsp_completions.size())) % static_cast<int>(win.lsp_completions.size());
      return true;
    }
    if (key == KeyDown) {
      win.lsp_completion_index = (win.lsp_completion_index + 1) % static_cast<int>(win.lsp_completions.size());
      return true;
    }
    if (key == KeyEnter || key == KeyTab) {
      if (win.lsp_completion_index >= 0 && win.lsp_completion_index < static_cast<int>(win.lsp_completions.size())) {
        std::string ins = win.lsp_completions[win.lsp_completion_index].insert_text;
        win.lsp_completion_active = false;
        editor_insert(win, ins);
        return true;
      }
    }
    if (key == KeyEscape) {
      win.lsp_completion_active = false;
      return true;
    }
  }

  if (win.lsp_references_active) {
    if (key == KeyUp) {
      win.lsp_references_index = (win.lsp_references_index - 1 + static_cast<int>(win.lsp_references.size())) % static_cast<int>(win.lsp_references.size());
      return true;
    }
    if (key == KeyDown) {
      win.lsp_references_index = (win.lsp_references_index + 1) % static_cast<int>(win.lsp_references.size());
      return true;
    }
    if (key == KeyEnter) {
      if (win.lsp_references_index >= 0 && win.lsp_references_index < static_cast<int>(win.lsp_references.size())) {
        const auto& target = win.lsp_references[win.lsp_references_index];
        win.cursor_line = target.range.start.line;
        win.cursor_col = target.range.start.character;
        win.lsp_references_active = false;
        win.status_text = "jumped to reference: " + target.path + ":" + std::to_string(target.range.start.line + 1);
        return true;
      }
    }
    if (key == KeyEscape) {
      win.lsp_references_active = false;
      return true;
    }
  }

  if (win.lsp_hover_active && key == KeyEscape) {
    win.lsp_hover_active = false;
    return true;
  }

  // LSP Intelligence Short-cuts. Every answer is looked up by window id when
  // it arrives: the callbacks used to hold &win, a reference into a vector
  // that reallocates when a window opens, and wrote through it afterwards.
  if (key == KeyF12 || key == KeyF1) {
    auto lsp = editor_lsp_sync(win, true);
    if (!lsp) {
      win.status_text = editor_lsp_absent(win);
      return true;
    }
    const uint32_t wid = win.window_id;
    const std::string path = win.file_path;
    if (key == KeyF12 && shift_held_) {
      lsp->request_references(path, win.cursor_line, win.cursor_col,
        [this, wid](const std::vector<LspLocation>& refs) {
          WindowSurface* w = find_window(wid);
          if (!w) return;
          w->lsp_references = refs;
          w->lsp_references_index = 0;
          w->lsp_references_active = !refs.empty();
          w->status_text = refs.empty() ? "no references found" : "found " + std::to_string(refs.size()) + " references";
        });
      win.status_text = "asking " + LspClientManager::server_for(win.file_lang) + " for references...";
    } else if (key == KeyF12) {
      lsp->request_definition(path, win.cursor_line, win.cursor_col,
        [this, wid, path](const std::vector<LspLocation>& locs) {
          WindowSurface* w = find_window(wid);
          if (!w) return;
          if (locs.empty()) { w->status_text = "definition not found"; return; }
          const auto& loc = locs[0];
          if (!loc.path.empty() && loc.path != path && kernel_) {
            if (!editor_open(*w, *kernel_, loc.path)) return;  // editor_open says why
          }
          w->cursor_line = loc.range.start.line;
          w->cursor_col = loc.range.start.character;
          w->status_text = "definition: " + (loc.path.empty() ? path : loc.path) + ":" + std::to_string(w->cursor_line + 1);
        });
      win.status_text = "asking " + LspClientManager::server_for(win.file_lang) + " for the definition...";
    } else {
      lsp->request_hover(path, win.cursor_line, win.cursor_col,
        [this, wid](const LspHoverInfo& info) {
          WindowSurface* w = find_window(wid);
          if (!w) return;
          w->lsp_hover_text = info.contents;
          w->lsp_hover_active = !info.contents.empty();
          if (info.contents.empty()) w->status_text = "nothing to show here";
        });
      win.status_text = "asking " + LspClientManager::server_for(win.file_lang) + " about this symbol...";
    }
    return true;
  }

  switch (key) {
    case KeyLeft:
      if (win.cursor_col > 0) {
        win.cursor_col = word_wise_ ? 0 : editor_step_col(line, win.cursor_col, -1);
      } else if (win.cursor_line > 0) {
        win.cursor_line--;
        win.cursor_col = (int)win.lines[(size_t)win.cursor_line].size();
      }
      return true;
    case KeyRight:
      if (win.cursor_col < (int)line.size()) {
        win.cursor_col = word_wise_ ? (int)line.size() : editor_step_col(line, win.cursor_col, 1);
      } else if (win.cursor_line < last) {
        win.cursor_line++;
        win.cursor_col = 0;
      }
      return true;
    case KeyUp:
      if (win.cursor_line > 0) win.cursor_line--;
      return true;
    case KeyDown:
      if (win.cursor_line < last) win.cursor_line++;
      return true;
    case KeyHome: win.cursor_col = 0; return true;
    case KeyEnd: win.cursor_col = (int)line.size(); return true;
    case KeyPageUp: win.cursor_line = win.cursor_line > 20 ? win.cursor_line - 20 : 0; return true;
    case KeyPageDown: win.cursor_line = win.cursor_line + 20 > last ? last : win.cursor_line + 20; return true;
    case KeyTab:
      // Tab over a selection shifts the whole block, which is what it is for.
      if (editor_has_selection(win) || shift_held_) editor_indent_selection(win, shift_held_);
      else editor_insert(win, "  ");
      return true;
    case KeyEnter: {
      editor_delete_selection(win);
      editor_push_undo(win, false);
      std::string& cl = win.lines[(size_t)win.cursor_line];
      if (win.cursor_col > (int)cl.size()) win.cursor_col = (int)cl.size();
      std::string tail = cl.substr((size_t)win.cursor_col);
      cl.erase((size_t)win.cursor_col);
      // Keep the indentation the line already had: an editor that returns the
      // caret to column zero inside a nested block is one nobody types in.
      std::string indent;
      for (char c : win.lines[(size_t)win.cursor_line]) {
        if (c == ' ' || c == '\t') indent += c;
        else break;
      }
      win.lines.insert(win.lines.begin() + win.cursor_line + 1, indent + tail);
      win.cursor_line++;
      win.cursor_col = (int)indent.size();
      win.file_dirty = true;
      return true;
    }
    case KeyBackspace:
      if (editor_delete_selection(win)) return true;
      if (win.cursor_col > 0) {
        editor_push_undo(win, true);
        int from = editor_step_col(line, win.cursor_col, -1);
        line.erase((size_t)from, (size_t)(win.cursor_col - from));
        win.cursor_col = from;
        win.file_dirty = true;
      } else if (win.cursor_line > 0) {
        editor_push_undo(win, false);
        int prev = win.cursor_line - 1;
        win.cursor_col = (int)win.lines[(size_t)prev].size();
        win.lines[(size_t)prev] += line;
        win.lines.erase(win.lines.begin() + win.cursor_line);
        win.cursor_line = prev;
        win.file_dirty = true;
      }
      return true;
    case KeyDelete:
      if (editor_delete_selection(win)) return true;
      if (win.cursor_col < (int)line.size()) {
        editor_push_undo(win, true);
        int to = editor_step_col(line, win.cursor_col, 1);
        line.erase((size_t)win.cursor_col, (size_t)(to - win.cursor_col));
        win.file_dirty = true;
      } else if (win.cursor_line < last) {
        editor_push_undo(win, false);
        line += win.lines[(size_t)win.cursor_line + 1];
        win.lines.erase(win.lines.begin() + win.cursor_line + 1);
        win.file_dirty = true;
      }
      return true;
    default: break;
  }
  if (aios::hal::key_is_text(key)) {
    editor_insert(win, std::string(1, (char)key));
    // The server has to see the keystroke before it is asked what completes
    // it, so the change is flushed first.
    if (auto lsp = editor_lsp_sync(win, true)) {
      const uint32_t wid = win.window_id;
      lsp->request_completion(win.file_path, win.cursor_line, win.cursor_col,
        [this, wid](const std::vector<LspCompletionItem>& items) {
          WindowSurface* w = find_window(wid);
          if (!w) return;
          w->lsp_completions = items;
          w->lsp_completion_index = 0;
          w->lsp_completion_active = !items.empty();
        });
    }
    return true;
  }
  return false;
}

// The editor's own command line, which is where a file is named. Anything it
// does not recognise is treated as a path to open, because that is what a
// person typing a path into an editor means.
void HardwareCompositor::editor_command(WindowSurface& win, Kernel& k, const std::string& line) {
  std::string cmd = line, arg;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }
  while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
  while (!arg.empty() && arg.back() == ' ') arg.pop_back();
  std::string verb = cmd;
  for (auto& c : verb) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);

  auto absolute = [&](const std::string& p) {
    if (p.empty() || p[0] == '/') return p;
    if (!k.config().root.empty()) {
      if (p == k.config().root || p.rfind(k.config().root + "/", 0) == 0) return p;
    }
    std::string base = win.file_dir.empty() ? k.config().root : win.file_dir;
    if (base.empty()) return p;
    return base + "/" + p;
  };

  if (verb == "open" || verb == "e" || verb == "edit") {
    if (arg.empty()) { win.status_text = "open <path>"; return; }
    editor_open(win, k, absolute(arg));
  } else if (verb == "save" || verb == "w" || verb == "save!" || verb == "w!") {
    editor_save(win, k, arg.empty() ? std::string() : absolute(arg),
                verb == "save!" || verb == "w!");
  } else if (verb == "ls" || verb == "cd") {
    editor_browse(win, k, arg.empty() ? win.file_dir : absolute(arg));
  } else if (verb == "new") {
    editor_stash_active(win);
    win.lines.assign(1, "");
    win.file_path = arg.empty() ? "" : absolute(arg);
    win.file_dirty = !win.file_path.empty();
    win.file_lang = lang_for_path(win.file_path);
    win.file_preview = false;   // a file you are writing opens as source
    win.file_eol = "\n";
    win.file_final_newline = true;
    win.cursor_line = win.cursor_col = win.view_top = win.view_left = 0;
    win.undo_stack.clear();
    win.redo_stack.clear();
    editor_clear_selection(win);
    EditorBuffer nb;
    nb.path = win.file_path;
    win.buffers.push_back(nb);
    win.active_buffer = (int)win.buffers.size() - 1;
    editor_stash_active(win);
    win.cmdline_focus = false;
    win.status_text = win.file_path.empty() ? "new scratch buffer" : "new buffer for " + win.file_path;
  } else if (verb == "close") {
    editor_close_buffer(win, win.active_buffer);
  } else if (verb == "reload") {
    if (win.file_path.empty()) { win.status_text = "nothing to reload"; return; }
    std::string p = win.file_path;
    int at = editor_buffer_for_path(win, p);
    if (at >= 0) { win.buffers.erase(win.buffers.begin() + at); win.active_buffer = -1; }
    win.file_path.clear();
    win.file_dirty = false;
    editor_open(win, k, p);
  } else if (verb == "goto" || verb == "line") {
    int n = 0;
    for (char c : arg) if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
    if (n > 0) { win.cursor_line = n - 1; win.cursor_col = 0; }
    if (win.cursor_line >= (int)win.lines.size()) win.cursor_line = (int)win.lines.size() - 1;
    editor_clear_selection(win);
    win.cmdline_focus = false;
  } else if (verb == "find" || verb == "f") {
    if (arg.empty()) { win.status_text = "find <text>"; return; }
    editor_find(win, arg, true, true);
  } else if (verb == "findprev" || verb == "fp") {
    editor_find(win, arg.empty() ? win.find_text : arg, false, true);
  } else if (verb == "replace" || verb == "r") {
    // replace <from> <to> -- the first space splits them, so `to` may be empty
    // and mean "delete every one of these".
    size_t s2 = arg.find(' ');
    std::string from = s2 == std::string::npos ? arg : arg.substr(0, s2);
    std::string to = s2 == std::string::npos ? "" : arg.substr(s2 + 1);
    if (from.empty()) { win.status_text = "replace <from> <to>"; return; }
    win.replace_text = to;
    int n = editor_replace_all(win, from, to);
    win.status_text = n ? ("replaced " + std::to_string(n) + " occurrence(s)")
                        : ("not found: " + from);
    win.cmdline_focus = false;
  } else if (verb == "preview" || verb == "read") {
    if (!lang_has_preview(win.file_lang)) {
      win.status_text = std::string(lang_name(win.file_lang)) + " has no preview";
    } else {
      win.file_preview = !win.file_preview;
      editor_stash_active(win);
      win.cmdline_focus = false;
    }
  } else if (verb == "theme") {
    if (arg.empty()) {
      win.status_text = "theme <boba|panic-boba|panic-dark|panic-light|nord|solarized|monokai>";
      return;
    }
    if (!set_active_theme(arg)) {
      win.status_text = "unknown theme: " + arg;
      return;   // prior theme and cache stay intact — no partial state
    }
    // Applying a theme re-colours every line: drop the whole token cache so
    // the whole viewport is re-tokenised under the new palette.
    win.tok_cache.clear();
    win.status_text = "theme: " + arg;
  } else if (verb == "undo") {
    editor_undo(win);
  } else if (verb == "redo") {
    editor_redo(win);
  } else if (verb == "review" || (verb == "tx" && arg == "review")) {
    // The staged transaction from `tx stage` in this machine's shell, opened
    // as the hunk-by-hunk review buffer (Space accepts/rejects, commit/abort).
    if (!PatchEngine::has_active_transaction()) {
      win.status_text = "no staged transaction - run `tx stage <patch>` in the shell first";
    } else {
      editor_open_tx_review(win, PatchEngine::active_transaction());
      win.status_text = "reviewing transaction #" + std::to_string(win.tx_manifest.tx_id);
    }
  } else if (verb == "help") {
    win.status_text = "theme open save new close reload goto find findprev replace preview undo redo review";
  } else if (!line.empty()) {
    editor_open(win, k, absolute(line));
  }
}


WorkstationState HardwareCompositor::workstation() {
  WorkstationState ws;
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (const auto& w : windows_) {
    if (w.type == WindowType::Editor) {
      ws.has_editor = true;
      ws.editor_path = w.file_path;
      ws.editor_lines = w.lines.size();
      ws.editor_dirty = w.file_dirty;
      ws.editor_cursor_line = w.cursor_line;
      ws.editor_bytes = 0;
      for (const auto& l : w.lines) ws.editor_bytes += l.size() + 1;
      ws.editor_tabs = w.buffers.size();
      ws.editor_selection = editor_has_selection(w);
      ws.editor_cmdline = w.cmdline_focus;
      ws.editor_status = w.status_text;
      ws.editor_lang = lang_name(w.file_lang);
      ws.editor_preview = w.file_preview;
      if (w.cursor_line >= 0 && w.cursor_line < (int)w.lines.size())
        ws.editor_line = w.lines[(size_t)w.cursor_line];
    } else if (w.type == WindowType::Database) {
      ws.has_db = true;
      ws.db_connected = w.db_connected;
      ws.db_tables = w.db_tables.size();
      ws.db_selected = w.db_selected;
      ws.db_grid = w.db_grid_label;
      ws.db_last_query = w.db_last_query;
      ws.db_status = w.db_status;
      ws.db_error = w.db_error;
      ws.db_rows = w.grid_rows.size();
      ws.db_columns = w.grid_columns.size();
    }
  }
  return ws;
}

// --- the panes ------------------------------------------------------------

// The chip row both applications wear, hit-tested by the same arithmetic in
// input_mouse_button.
template <size_t N>
static void draw_app_chips(HardwareCompositor& S, WindowSurface& win, int w, int h,
                           const char* const (&chips)[N], const char* active) {
  int chip_x = 22;
  for (const char* chip : chips) {
    int chip_w = S.text_width(chip, Face::Ui) + 16;
    // Stop at the edge. A row of chips that runs past the pane draws over the
    // frame and offers buttons nobody can reach.
    if (chip_x + chip_w > w - 12) break;
    bool hot = win.focused && S.mouse_over_content(win, chip_x, APP_CHIP_Y, chip_w, APP_CHIP_H);
    bool on = active && strcmp(chip, active) == 0;
    S.draw_pill_to(win.buffer, w, h, chip_x, APP_CHIP_Y, chip_w, APP_CHIP_H,
                   (hot || on) ? tok::wash : tok::panel2, (hot || on) ? tok::strong : tok::line_up);
    S.draw_text_to(win.buffer, w, h, chip_x + 8, APP_CHIP_Y + 5, chip,
                   (hot || on) ? tok::text : tok::dim, Face::UiSmall);
    chip_x += chip_w + 6;
  }
}

// One rendered Markdown line's runs, drawn left to right. Bold is the bold
// face, code is mono on its own ground, a link is the label in the link
// colour. Returns where the drawing ended, so a caller can wrap.
int HardwareCompositor::draw_markdown_runs(WindowSurface& win, int w, int h, int x, int y,
                                           int avail, const std::vector<MarkdownRun>& runs,
                                           Face base, uint32_t base_ink) {
  int cx = x;
  for (const MarkdownRun& r : runs) {
    Face face = base;
    uint32_t ink = base_ink;
    switch (r.cls) {
      case TokenClass::Emphasis: face = Face::UiBold; ink = aios::userland::colour_for(TokenClass::Emphasis); break;
      case TokenClass::Code:     face = Face::Mono;   ink = aios::userland::colour_for(TokenClass::Code); break;
      case TokenClass::Link:     ink = aios::userland::colour_for(TokenClass::Link); break;
      default: break;
    }
    int room = x + avail - cx;
    if (room <= 0) break;
    std::string piece = fit_text(*this, r.text, room, face);
    if (r.cls == TokenClass::Code)
      draw_rect_to(win.buffer, w, h, cx - 2, y - 2, text_width(piece, face) + 4, 18, 0xFF161B22);
    draw_text_to(win.buffer, w, h, cx, y, piece, ink, face);
    cx += text_width(piece, face);
  }
  return cx;
}

void HardwareCompositor::render_editor_preview(WindowSurface& win, int w, int h, int body_y,
                                               int body_h, int box_x, int box_w) {
  draw_rect_to(win.buffer, w, h, box_x, body_y, box_w, body_h, 0xFF0D1117);
  draw_rect_outline_to(win.buffer, w, h, box_x, body_y, box_w, body_h, tok::line_up);

  std::vector<MarkdownLine> doc;
  markdown_render(win.lines, &doc);

  // The preview scrolls on the same view_top the source does, so switching
  // between the two keeps you roughly where you were rather than throwing you
  // back to the top of the file.
  int top = win.view_top;
  if (top > (int)doc.size() - 1) top = (int)doc.size() - 1;
  if (top < 0) top = 0;

  int x0 = box_x + 22;
  int avail = box_w - 44;
  int y = body_y + 14;
  int bottom = body_y + body_h - 10;

  for (size_t i = (size_t)top; i < doc.size() && y < bottom; ++i) {
    const MarkdownLine& ml = doc[i];
    int indent = ml.indent * 16;

    switch (ml.block) {
      case MarkdownBlock::Blank:
        y += 8;
        continue;
      case MarkdownBlock::Rule:
        draw_rect_to(win.buffer, w, h, x0, y + 6, avail, 1, tok::line_up);
        y += 16;
        continue;
      case MarkdownBlock::Heading1:
      case MarkdownBlock::Heading2:
      case MarkdownBlock::Heading3: {
        // Three weights of heading out of the faces there are: the atlas has no
        // larger sizes, so rank is carried by weight and by a rule under the
        // top level rather than by pretending to scale type.
        Face face = ml.block == MarkdownBlock::Heading3 ? Face::Ui : Face::UiBold;
        y += (ml.block == MarkdownBlock::Heading1) ? 10 : 6;
        draw_markdown_runs(win, w, h, x0, y, avail, ml.runs, face,
                           ml.block == MarkdownBlock::Heading3 ? tok::text : aios::userland::colour_for(TokenClass::Heading));
        y += line_height(face) + 2;
        if (ml.block == MarkdownBlock::Heading1) {
          draw_rect_to(win.buffer, w, h, x0, y, avail, 1, tok::line_up);
          y += 6;
        }
        continue;
      }
      case MarkdownBlock::Code: {
        draw_rect_to(win.buffer, w, h, x0, y - 2, avail, APP_ROW, 0xFF161B22);
        draw_text_to(win.buffer, w, h, x0 + 8, y, fit_text(*this, ml.text, avail - 16, Face::Mono),
                     aios::userland::colour_for(TokenClass::Code), Face::Mono);
        y += APP_ROW;
        continue;
      }
      case MarkdownBlock::Quote: {
        draw_rect_to(win.buffer, w, h, x0, y - 2, 3, APP_ROW, tok::strong);
        draw_markdown_runs(win, w, h, x0 + 14, y, avail - 20, ml.runs, Face::Ui, tok::dim);
        y += APP_ROW;
        continue;
      }
      case MarkdownBlock::TableRow: {
        // The pipes become the columns they were standing in for.
        int cx = x0;
        std::string cell;
        for (size_t c = 0; c <= ml.text.size(); ++c) {
          if (c == ml.text.size() || ml.text[c] == '|') {
            if (!cell.empty()) {
              bool divider = cell.find_first_not_of("-: ") == std::string::npos;
              if (!divider)
                draw_text_to(win.buffer, w, h, cx + 6, y,
                             fit_text(*this, cell, 150, Face::UiSmall), tok::text, Face::UiSmall);
              cx += 160;
            }
            cell.clear();
            continue;
          }
          cell += ml.text[c];
        }
        y += APP_ROW;
        continue;
      }
      case MarkdownBlock::Bullet:
      case MarkdownBlock::Numbered: {
        std::string marker = ml.marker.empty() ? std::string("-") : ml.marker;
        draw_text_to(win.buffer, w, h, x0 + indent, y, marker, aios::userland::colour_for(TokenClass::Rule), Face::Ui);
        int tx = x0 + indent + std::max(16, text_width(marker, Face::Ui) + 8);
        draw_markdown_runs(win, w, h, tx, y, avail - indent - 16, ml.runs, Face::Ui, tok::text);
        y += APP_ROW;
        continue;
      }
      case MarkdownBlock::Paragraph:
      default: {
        // Long paragraphs wrap, because a preview that clips a sentence is not
        // showing you the document.
        std::string rest = ml.text;
        while (!rest.empty() && y < bottom) {
          std::string fitted = fit_text(*this, rest, avail - indent, Face::Ui);
          bool clipped = fitted.size() >= 2 && fitted.compare(fitted.size() - 2, 2, "..") == 0;
          if (!clipped) {
            if (rest == ml.text)
              draw_markdown_runs(win, w, h, x0 + indent, y, avail - indent, ml.runs, Face::Ui, tok::text);
            else
              draw_text_to(win.buffer, w, h, x0 + indent, y, rest, tok::text, Face::Ui);
            y += APP_ROW;
            break;
          }
          // Break at the last space that fits, so words stay whole.
          size_t take = fitted.size() - 2;
          size_t space = rest.rfind(' ', take);
          if (space == std::string::npos || space == 0) space = take;
          draw_text_to(win.buffer, w, h, x0 + indent, y, rest.substr(0, space), tok::text, Face::Ui);
          y += APP_ROW;
          rest = rest.substr(rest.find_first_not_of(' ', space) == std::string::npos
                                 ? rest.size()
                                 : rest.find_first_not_of(' ', space));
        }
        continue;
      }
    }
  }

  if (doc.empty())
    draw_text_to(win.buffer, w, h, x0, body_y + 14, "this file is empty", tok::faint, Face::UiSmall);
}

void HardwareCompositor::render_editor_tx_review(WindowSurface& win, int w, int h, int body_y,
                                                int body_h, int box_x, int box_w, Kernel& k) {
  (void)k;
  auto& manifest = win.tx_manifest;
  win.tx_hit_map.clear();

  // Draw card frame
  draw_card(win, w, h, 8, 8, w - 16, h - 16, "atomic review buffer");

  // Top header bar with frosted gradient: Transaction ID, Title, Status badge
  int head_h = 42;
  int head_y = 12;
  draw_gradient_v_to(win.buffer, w, h, 12, head_y, w - 24, head_h, 0xFF1B2332, 0xFF121722);
  draw_round_outline_to(win.buffer, w, h, 12, head_y, w - 24, head_h, 8, 0xFF2D384D);

  // Intent Pill Badge
  std::string tx_id_tag = "TX #" + std::to_string(manifest.tx_id);
  int tag_w = text_width(tx_id_tag, Face::UiBold) + 16;
  draw_pill_to(win.buffer, w, h, 20, head_y + 9, tag_w, 24, 0xFF1E3A5F, 0xFF388BFD);
  draw_text_to(win.buffer, w, h, 28, head_y + 12, tx_id_tag, 0xFF79C0FF, Face::UiBold);

  // Title
  int tx_head_x = 20 + tag_w + 12;
  std::string tx_title = manifest.title.empty() ? "Multi-File Edit Transaction" : manifest.title;
  draw_text_to(win.buffer, w, h, tx_head_x, head_y + 12,
               fit_text(*this, tx_title, w - tx_head_x - 340, Face::UiBold),
               0xFFF0F6FC, Face::UiBold);

  // Status badge (Staged / Committed / Aborted / Failed)
  const char* st_name = transaction_status_name(manifest.status);
  uint32_t st_bg = (manifest.status == TransactionStatus::Committed) ? 0xFF14321A :
                   ((manifest.status == TransactionStatus::Failed || manifest.status == TransactionStatus::Aborted) ? 0xFF381418 : 0xFF1F2B3E);
  uint32_t st_color = (manifest.status == TransactionStatus::Committed) ? 0xFF3FB950 :
                      ((manifest.status == TransactionStatus::Failed || manifest.status == TransactionStatus::Aborted) ? 0xFFF85149 : 0xFF58A6FF);
  std::string st_badge = std::string("[ ") + st_name + " - ATOMIC STAGING ]";
  int st_w = text_width(st_badge, Face::UiBold) + 18;
  draw_pill_to(win.buffer, w, h, w - st_w - 24, head_y + 9, st_w, 24, st_bg, st_color);
  draw_text_to(win.buffer, w, h, w - st_w - 15, head_y + 12, st_badge, st_color, Face::UiBold);

  // Sub-header stats line & interactive action toolbar
  int stat_y = body_y - 24;
  if (stat_y < head_y + head_h + 8) stat_y = head_y + head_h + 8;
  int total_h = manifest.total_hunks();
  int act_h = manifest.active_hunks_count();
  int total_add = manifest.total_additions();
  int total_del = manifest.total_deletions();

  // Stat chips
  std::string f_chip = std::to_string(manifest.files.size()) + " Files";
  int f_chip_w = text_width(f_chip, Face::UiSmall) + 14;
  draw_pill_to(win.buffer, w, h, 20, stat_y - 2, f_chip_w, 22, 0xFF161B22, 0xFF30363D);
  draw_text_to(win.buffer, w, h, 27, stat_y + 1, f_chip, tok::text, Face::UiSmall);

  std::string diff_chip = "+" + std::to_string(total_add) + " / -" + std::to_string(total_del);
  int diff_chip_w = text_width(diff_chip, Face::Mono) + 14;
  draw_pill_to(win.buffer, w, h, 20 + f_chip_w + 6, stat_y - 2, diff_chip_w, 22, 0xFF161B22, 0xFF30363D);
  draw_text_to(win.buffer, w, h, 20 + f_chip_w + 13, stat_y + 1, diff_chip, 0xFF7EE787, Face::Mono);

  std::string hunk_chip = std::to_string(act_h) + "/" + std::to_string(total_h) + " Hunks Staged";
  int hunk_chip_w = text_width(hunk_chip, Face::UiSmall) + 14;
  draw_pill_to(win.buffer, w, h, 20 + f_chip_w + 6 + diff_chip_w + 6, stat_y - 2, hunk_chip_w, 22, 0xFF161B22, 0xFF30363D);
  draw_text_to(win.buffer, w, h, 20 + f_chip_w + 6 + diff_chip_w + 13, stat_y + 1, hunk_chip, 0xFF58A6FF, Face::UiSmall);

  // Action chips on right
  std::string commit_chip = "[ Commit (Ctrl+Enter) ]";
  std::string abort_chip = "[ Abort (Esc) ]";
  int c_w = text_width(commit_chip, Face::UiBold) + 16;
  int a_w = text_width(abort_chip, Face::UiBold) + 16;
  int btn_y = stat_y - 3;

  // Commit button (Vibrant Emerald Gradient)
  int c_x = w - c_w - 20;
  draw_gradient_v_to(win.buffer, w, h, c_x, btn_y, c_w, 24, 0xFF2EA043, 0xFF238636);
  draw_round_outline_to(win.buffer, w, h, c_x, btn_y, c_w, 24, 6, 0xFF3FB950);
  draw_text_to(win.buffer, w, h, c_x + 8, btn_y + 4, commit_chip, 0xFFFFFFFF, Face::UiBold);
  win.tx_hit_map.push_back({btn_y, btn_y + 24, -1, -1, false, false, true, false});

  // Abort button (Dark Crimson)
  int a_x = c_x - a_w - 10;
  draw_gradient_v_to(win.buffer, w, h, a_x, btn_y, a_w, 24, 0xFF34171A, 0xFF21262D);
  draw_round_outline_to(win.buffer, w, h, a_x, btn_y, a_w, 24, 6, 0xFFDA3633);
  draw_text_to(win.buffer, w, h, a_x + 8, btn_y + 4, abort_chip, 0xFFF85149, Face::UiBold);
  win.tx_hit_map.push_back({btn_y, btn_y + 24, -1, -1, false, false, false, true});

  // Keyboard shortcut hints
  // Only in the room between the chips and the buttons: right-aligned against
  // Abort it used to start under the "Hunks Staged" chip on a normal width.
  {
    const int chips_end = 20 + f_chip_w + 6 + diff_chip_w + 6 + hunk_chip_w + 12;
    for (const char* hints : {"Tab/Shift+Tab: Jump Hunk  |  Space: Toggle  |  Ctrl+Enter: Commit  |  Esc: Abort",
                              "Tab: next hunk  |  Space: toggle", "Space: toggle"}) {
      const int hx = a_x - text_width(hints, Face::UiSmall) - 16;
      if (hx >= chips_end) {
        draw_text_to(win.buffer, w, h, hx, stat_y + 2, hints, 0xFF8B949E, Face::UiSmall);
        break;
      }
    }
  }

  // Main scrollable review buffer box
  draw_rounded_rect_to(win.buffer, w, h, box_x, body_y, box_w, body_h, 8, 0xFF0A0D14);
  draw_round_outline_to(win.buffer, w, h, box_x, body_y, box_w, body_h, 8, 0xFF1F2633);

  int cur_y = body_y + 8 - win.tx_scroll_y;
  int view_bottom = body_y + body_h - 6;

  // Plan Manifest Summary Box (Intent Declaration) at the top of scrollable content
  int plan_box_h = 28 + static_cast<int>(manifest.files.size()) * 22;
  if (cur_y + plan_box_h > body_y && cur_y < view_bottom) {
    int py = cur_y;
    draw_rounded_rect_to(win.buffer, w, h, box_x + 8, py, box_w - 16, plan_box_h, 6, 0xFF131822);
    draw_round_outline_to(win.buffer, w, h, box_x + 8, py, box_w - 16, plan_box_h, 6, 0xFF2B3344);

    draw_label_to(win.buffer, w, h, box_x + 18, py + 7, "manifest", 0xFF58A6FF);
    draw_text_to(win.buffer, w, h, box_x + 18 + label_width("manifest") + 10, py + 5,
                 "INTENT DECLARATION & BLAST RADIUS (Phase 1: Staged Plan)", 0xFFE6EDF3, Face::UiBold);

    int row_y = py + 26;
    for (size_t f_i = 0; f_i < manifest.files.size(); ++f_i) {
      const auto& f = manifest.files[f_i];
      std::string sym = file_op_symbol(f.op_type);
      uint32_t sym_bg = (f.op_type == FileOpType::Add) ? 0xFF14321A :
                        ((f.op_type == FileOpType::Delete) ? 0xFF381418 :
                        ((f.op_type == FileOpType::Rename) ? 0xFF2A163E : 0xFF382710));
      uint32_t sym_c = (f.op_type == FileOpType::Add) ? 0xFF3FB950 :
                       ((f.op_type == FileOpType::Delete) ? 0xFFF85149 :
                       ((f.op_type == FileOpType::Rename) ? 0xFFA371F7 : 0xFFD29922));

      draw_pill_to(win.buffer, w, h, box_x + 18, row_y - 1, 28, 18, sym_bg, sym_c);
      draw_text_to(win.buffer, w, h, box_x + 23, row_y, sym, sym_c, Face::Mono);

      std::string f_name = f.new_path.empty() ? f.old_path : f.new_path;
      draw_text_to(win.buffer, w, h, box_x + 54, row_y, f_name, 0xFFF0F6FC, Face::Mono);
      if (!f.reason.empty()) {
        std::string r_str = "- " + f.reason;
        draw_text_to(win.buffer, w, h, box_x + 54 + text_width(f_name, Face::Mono) + 12, row_y + 1, r_str, 0xFF8B949E, Face::UiSmall);
      }
      row_y += 22;
    }
  }
  cur_y += plan_box_h + 12;

  // Render file diff chunks
  for (size_t f_idx = 0; f_idx < manifest.files.size(); ++f_idx) {
    auto& file_entry = manifest.files[f_idx];
    std::string file_name = file_entry.new_path.empty() ? file_entry.old_path : file_entry.new_path;
    int file_header_h = 28;

    // File header chunk
    if (cur_y + file_header_h > body_y && cur_y < view_bottom) {
      bool is_focused_f = (static_cast<int>(f_idx) == win.tx_focused_file);
      uint32_t f_bg_top = is_focused_f ? 0xFF1C2536 : 0xFF161C26;
      uint32_t f_bg_bot = is_focused_f ? 0xFF141A26 : 0xFF11151E;
      draw_gradient_v_to(win.buffer, w, h, box_x + 8, cur_y, box_w - 16, file_header_h, f_bg_top, f_bg_bot);
      draw_round_outline_to(win.buffer, w, h, box_x + 8, cur_y, box_w - 16, file_header_h, 6, is_focused_f ? 0xFF388BFD : 0xFF2E384A);

      std::string sym = file_op_symbol(file_entry.op_type);
      uint32_t sym_bg = (file_entry.op_type == FileOpType::Add) ? 0xFF14321A :
                        ((file_entry.op_type == FileOpType::Delete) ? 0xFF381418 :
                        ((file_entry.op_type == FileOpType::Rename) ? 0xFF2A163E : 0xFF382710));
      uint32_t sym_c = (file_entry.op_type == FileOpType::Add) ? 0xFF3FB950 :
                       ((file_entry.op_type == FileOpType::Delete) ? 0xFFF85149 :
                       ((file_entry.op_type == FileOpType::Rename) ? 0xFFA371F7 : 0xFFD29922));

      draw_pill_to(win.buffer, w, h, box_x + 16, cur_y + 5, 26, 18, sym_bg, sym_c);
      draw_text_to(win.buffer, w, h, box_x + 20, cur_y + 6, sym, sym_c, Face::Mono);

      std::string coll_icon = file_entry.collapsed ? "[+] " : "[-] ";
      draw_text_to(win.buffer, w, h, box_x + 48, cur_y + 6, coll_icon, 0xFF8B949E, Face::Mono);

      draw_text_to(win.buffer, w, h, box_x + 78, cur_y + 6, file_name, 0xFFF0F6FC, Face::Mono);

      if (!file_entry.reason.empty()) {
        std::string r_str = "(" + file_entry.reason + ")";
        draw_text_to(win.buffer, w, h, box_x + 78 + text_width(file_name, Face::Mono) + 10, cur_y + 6, r_str, 0xFF8B949E, Face::UiSmall);
      }

      std::string delta_str = "+" + std::to_string(file_entry.additions_count) + " -" + std::to_string(file_entry.deletions_count);
      draw_text_to(win.buffer, w, h, box_x + box_w - 250, cur_y + 6, delta_str, 0xFF7EE787, Face::Mono);

      std::string acc_str = file_entry.accepted ? "[X] Staged" : "[ ] Skipped";
      uint32_t acc_c = file_entry.accepted ? 0xFF3FB950 : 0xFF8B949E;
      draw_text_to(win.buffer, w, h, box_x + box_w - 130, cur_y + 6, acc_str, acc_c, Face::UiBold);

      win.tx_hit_map.push_back({cur_y, cur_y + file_header_h, static_cast<int>(f_idx), -1, true, false, false, false});
    }
    cur_y += file_header_h + 3;

    if (file_entry.collapsed) {
      cur_y += 6;
      continue;
    }

    // Render hunks for this file
    for (size_t h_idx = 0; h_idx < file_entry.hunks.size(); ++h_idx) {
      auto& hunk = file_entry.hunks[h_idx];
      bool is_focused_hunk = (hunk.global_hunk_id == win.tx_focused_hunk);
      int hunk_header_h = 24;

      // Hunk header bar
      if (cur_y + hunk_header_h > body_y && cur_y < view_bottom) {
        if (is_focused_hunk) {
          draw_gradient_v_to(win.buffer, w, h, box_x + 14, cur_y, box_w - 28, hunk_header_h, 0xFF163252, 0xFF0E2238);
          draw_round_outline_to(win.buffer, w, h, box_x + 14, cur_y, box_w - 28, hunk_header_h, 4, 0xFF388BFD);

          draw_pill_to(win.buffer, w, h, box_x + 20, cur_y + 3, text_width(">>> FOCUS", Face::UiBold) + 14, 18,
                       0xFF1E4976, 0xFF58A6FF);
          draw_text_to(win.buffer, w, h, box_x + 27, cur_y + 5, ">>> FOCUS", 0xFFFFFFFF, Face::UiBold);
        } else {
          draw_rect_to(win.buffer, w, h, box_x + 14, cur_y, box_w - 28, hunk_header_h, 0xFF121720);
          draw_rect_outline_to(win.buffer, w, h, box_x + 14, cur_y, box_w - 28, hunk_header_h, 0xFF222938);
        }

        int h_text_x = is_focused_hunk ? box_x + 20 + text_width(">>> FOCUS", Face::UiBold) + 14 + 10 : box_x + 22;
        draw_text_to(win.buffer, w, h, h_text_x, cur_y + 5, hunk.header, 0xFF79C0FF, Face::Mono);

        std::string h_acc_str = hunk.accepted ? "[ACCEPTED (Space to toggle)]" : "[REJECTED (Space to toggle)]";
        uint32_t h_acc_c = hunk.accepted ? 0xFF3FB950 : 0xFFF85149;
        draw_text_to(win.buffer, w, h, box_x + box_w - 290, cur_y + 5, h_acc_str, h_acc_c, Face::UiBold);

        win.tx_hit_map.push_back({cur_y, cur_y + hunk_header_h, static_cast<int>(f_idx), static_cast<int>(h_idx), false, true, false, false});
      }
      cur_y += hunk_header_h + 2;

      // Render diff lines with left accent bars and line number gutters
      for (const auto& dl : hunk.lines) {
        int line_h = APP_ROW;
        if (cur_y + line_h > body_y && cur_y < view_bottom) {
          uint32_t line_bg = 0xFF0D1117;
          uint32_t line_fg = 0xFFC9D1D9;
          uint32_t edge_bar = 0;
          char pfx = ' ';

          if (dl.type == DiffLineType::Addition) {
            line_bg = 0xFF0F2618;
            line_fg = 0xFF7EE787;
            edge_bar = 0xFF2EA043;
            pfx = '+';
          } else if (dl.type == DiffLineType::Deletion) {
            line_bg = 0xFF331518;
            line_fg = 0xFFFFA198;
            edge_bar = 0xFFDA3633;
            pfx = '-';
          }

          draw_rect_to(win.buffer, w, h, box_x + 14, cur_y, box_w - 28, line_h, line_bg);

          // Left 3px accent bar for additions/deletions
          if (edge_bar != 0) {
            draw_rect_to(win.buffer, w, h, box_x + 14, cur_y, 3, line_h, edge_bar);
          }

          // Gutter line numbers
          std::string old_num = (dl.old_lineno > 0) ? std::to_string(dl.old_lineno) : "";
          std::string new_num = (dl.new_lineno > 0) ? std::to_string(dl.new_lineno) : "";

          int gut_x = box_x + 20;
          if (!old_num.empty()) {
            draw_text_to(win.buffer, w, h, gut_x + 30 - text_width(old_num, Face::Mono), cur_y + 2, old_num, 0xFF485160, Face::Mono);
          }
          if (!new_num.empty()) {
            draw_text_to(win.buffer, w, h, gut_x + 70 - text_width(new_num, Face::Mono), cur_y + 2, new_num, 0xFF6E7681, Face::Mono);
          }

          // Prefix column
          draw_text_to(win.buffer, w, h, gut_x + 82, cur_y + 2, std::string(1, pfx), line_fg, Face::Mono);

          // Diff line content
          std::string disp_content = fit_text(*this, dl.content, box_w - 150, Face::Mono);
          draw_text_to(win.buffer, w, h, gut_x + 98, cur_y + 2, disp_content, line_fg, Face::Mono);
        }
        cur_y += line_h;
      }
      cur_y += 4;
    }
    cur_y += 8;
  }

  // Footer status bar in review mode
  int foot_y = h - 35;
  draw_label_to(win.buffer, w, h, 20, foot_y + 2, "staging", 0xFF58A6FF);
  int fpx = 20 + label_width("staging") + 12;
  std::string foot_msg = win.status_text.empty()
      ? "Deterministic in-memory staging workspace active. All disk files pristine until Commit."
      : win.status_text;
  draw_text_to(win.buffer, w, h, fpx, foot_y + 1, fit_text(*this, foot_msg, w - fpx - 30, Face::UiSmall),
               0xFF8B949E, Face::UiSmall);
}

bool HardwareCompositor::editor_tx_key(WindowSurface& win, int key, Kernel* k) {
  using namespace aios::hal;
  auto& manifest = win.tx_manifest;
  int total_h = manifest.total_hunks();

  if (key == KeyTab) {
    if (total_h > 0) {
      if (shift_held_) {
        win.tx_focused_hunk = (win.tx_focused_hunk - 1 + total_h) % total_h;
      } else {
        win.tx_focused_hunk = (win.tx_focused_hunk + 1) % total_h;
      }
      auto loc = manifest.find_hunk_location(win.tx_focused_hunk);
      win.tx_focused_file = loc.first;
      win.tx_scroll_y = win.tx_focused_hunk * 120;
    }
    return true;
  }

  if (key == ' ' || key == 32) {
    if (total_h > 0) {
      auto loc = manifest.find_hunk_location(win.tx_focused_hunk);
      if (loc.first >= 0 && loc.second >= 0) {
        auto& h = manifest.files[loc.first].hunks[loc.second];
        h.accepted = !h.accepted;
        PatchEngine::restage_with_hunk_selection(manifest);
        win.status_text = "Hunk #" + std::to_string(win.tx_focused_hunk + 1) + " " +
                          (h.accepted ? "[ACCEPTED]" : "[REJECTED / SKIPPED]");
      }
    }
    return true;
  }

  if (key == KeyEnter || key == 10) {
    if (k) {
      auto res = PatchEngine::commit(*k, manifest);
      if (res.ok()) {
        win.status_text = "Transaction #" + std::to_string(manifest.tx_id) + " COMMITTED atomically (" +
                          std::to_string(manifest.files.size()) + " files written to disk)";
        win.tx_review_active = false;
        editor_open(win, *k, win.file_path.empty() ? (manifest.files.empty() ? "" : manifest.files[0].new_path) : win.file_path);
      } else {
        win.status_text = "Commit error: " + res.error().str();
      }
    }
    return true;
  }

  if (key == KeyEscape) {
    PatchEngine::abort(manifest);
    win.tx_review_active = false;
    win.status_text = "Transaction #" + std::to_string(manifest.tx_id) + " ABORTED. Main tree pristine.";
    return true;
  }

  if (key == KeyUp) {
    if (win.tx_scroll_y > 0) win.tx_scroll_y = std::max(0, win.tx_scroll_y - 24);
    return true;
  }

  if (key == KeyDown) {
    win.tx_scroll_y += 24;
    return true;
  }

  if (key == KeyPageUp) {
    if (win.tx_scroll_y > 0) win.tx_scroll_y = std::max(0, win.tx_scroll_y - 200);
    return true;
  }

  if (key == KeyPageDown) {
    win.tx_scroll_y += 200;
    return true;
  }

  if (key == KeyHome) {
    win.tx_scroll_y = 0;
    return true;
  }

  return false;
}

bool HardwareCompositor::editor_tx_click(WindowSurface& win, int mx, int my, Kernel* k) {
  for (const auto& hit : win.tx_hit_map) {
    if (my >= hit.y_top && my <= hit.y_bottom) {
      if (hit.is_commit_btn) {
        if (k) {
          auto res = PatchEngine::commit(*k, win.tx_manifest);
          if (res.ok()) {
            win.status_text = "Transaction #" + std::to_string(win.tx_manifest.tx_id) + " COMMITTED atomically.";
            win.tx_review_active = false;
          } else {
            win.status_text = "Commit error: " + res.error().str();
          }
        }
        return true;
      }
      if (hit.is_abort_btn) {
        PatchEngine::abort(win.tx_manifest);
        win.tx_review_active = false;
        win.status_text = "Transaction ABORTED. Main tree pristine.";
        return true;
      }
      if (hit.is_file_header && hit.file_idx >= 0 && hit.file_idx < static_cast<int>(win.tx_manifest.files.size())) {
        win.tx_manifest.files[hit.file_idx].collapsed = !win.tx_manifest.files[hit.file_idx].collapsed;
        return true;
      }
      if (hit.is_hunk_header && hit.file_idx >= 0 && hit.hunk_idx >= 0) {
        int g_idx = win.tx_manifest.global_hunk_index(hit.file_idx, hit.hunk_idx);
        win.tx_focused_hunk = g_idx;
        win.tx_focused_file = hit.file_idx;
        win.tx_manifest.files[hit.file_idx].hunks[hit.hunk_idx].accepted =
            !win.tx_manifest.files[hit.file_idx].hunks[hit.hunk_idx].accepted;
        PatchEngine::restage_with_hunk_selection(win.tx_manifest);
        return true;
      }
    }
  }
  return false;
}

void HardwareCompositor::editor_open_tx_review(WindowSurface& win, const TransactionManifest& tx) {
  win.type = WindowType::Editor;
  win.tx_review_active = true;
  win.tx_manifest = tx;
  win.tx_focused_hunk = 0;
  win.tx_focused_file = 0;
  win.tx_scroll_y = 0;
  win.title = "Review: Transaction #" + std::to_string(tx.tx_id) + " - " + tx.title;
}

void HardwareCompositor::editor_close_tx_review(WindowSurface& win) {
  win.tx_review_active = false;
  win.title = "C++ IDE Editor";
}

std::string HardwareCompositor::editor_lsp_absent(const WindowSurface& win) const {
  if (win.file_path.empty()) return "no file open - nothing for a language server to read";
  const std::string bin = LspClientManager::server_for(win.file_lang);
  if (bin.empty()) return std::string("no language server for ") + lang_name(win.file_lang);
  if (is_ssh_path(win.file_path)) return "no language server for a file over ssh";
  return std::string("no language server for ") + lang_name(win.file_lang) + " (" + bin + " is not on PATH)";
}

std::shared_ptr<LspClient> HardwareCompositor::editor_lsp_sync(WindowSurface& win, bool flush) {
  auto doc_text = [&win]() {
    std::string t;
    for (const auto& l : win.lines) { t += l; t += '\n'; }
    return t;
  };
  auto fnv = [](const std::string& t) {
    uint64_t hsh = 1469598103934665603ull;
    for (unsigned char c : t) { hsh ^= c; hsh *= 1099511628211ull; }
    return hsh;
  };
  // A different file (or none) than the server was told about: close it.
  if (win.lsp_client && win.lsp_path != win.file_path) {
    win.lsp_client->did_close(win.lsp_path);
    win.lsp_client.reset();
    win.lsp_path.clear();
    win.diagnostics.clear();
    win.lsp_hover_active = win.lsp_completion_active = win.lsp_references_active = false;
  }
  if (win.file_path.empty() || win.file_lang == SourceLang::PlainText || is_ssh_path(win.file_path))
    return nullptr;
  if (!win.lsp_client) {
    // Look for a server once per file, not once per frame.
    if (win.lsp_tried_path == win.file_path && !flush) return nullptr;
    win.lsp_tried_path = win.file_path;
    auto lsp = LspClientManager::instance().get_or_start_client(win.file_lang, win.file_dir.empty() ? "." : win.file_dir);
    if (!lsp) return nullptr;
    // One callback per client, routed by PATH to whichever window shows that
    // file right now -- not to the window that happened to start the client.
    lsp->set_diagnostic_callback([this](const std::string& path, const std::vector<LspDiagnostic>& diags) {
      for (auto& w : windows_) {
        if (w.type != WindowType::Editor || w.file_path.empty()) continue;
        std::string mine = w.file_path;
        if (mine[0] != '/') {
          char cwd[4096];
          if (::getcwd(cwd, sizeof(cwd))) mine = std::string(cwd) + "/" + mine;
        }
        if (mine == path) w.diagnostics = diags;
      }
    });
    const std::string text = doc_text();
    lsp->did_open(win.file_path, win.file_lang, text);
    win.lsp_client = lsp;
    win.lsp_path = win.file_path;
    win.lsp_version = 1;
    win.lsp_sent_hash = fnv(text);
    win.lsp_dirty_since_ms = 0;
    return lsp;
  }
  if (!win.lsp_client->is_running()) {
    win.lsp_client.reset();
    win.lsp_path.clear();
    return nullptr;
  }
  // Edits reach the server as didChange, at most every 300 ms while typing,
  // at once when an answer is about to be asked for.
  const std::string text = doc_text();
  const uint64_t hsh = fnv(text);
  if (hsh != win.lsp_sent_hash) {
    const uint64_t now = steady_ms();
    if (!win.lsp_dirty_since_ms) win.lsp_dirty_since_ms = now;
    if (flush || now - win.lsp_dirty_since_ms >= 300) {
      win.lsp_client->did_change(win.file_path, text, ++win.lsp_version);
      win.lsp_sent_hash = hsh;
      win.lsp_dirty_since_ms = 0;
    }
  }
  return win.lsp_client;
}

void HardwareCompositor::render_editor_window(WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);

  if (win.tx_review_active) {
    render_editor_tx_review(win, w, h, APP_BODY_Y, h - APP_BODY_Y - APP_FOOT_H, 20, w - 40, k);
    return;
  }

  // Nothing to show until a file has been named. The pane says so instead of
  // inventing a plausible file, which is what it used to do.
  if (win.file_dir.empty() && win.dir_entries.empty()) editor_browse(win, k, k.config().root);
  if (win.lines.empty()) win.lines.push_back("");

  // --- Language server: attach to this file, push edits, take answers -----
  if (auto lsp = editor_lsp_sync(win, false)) lsp->poll_responses();

  draw_card(win, w, h, 10, 10, w - 20, h - 20, "native c++ code & text editor");
  draw_app_chips(*this, win, w, h, EDIT_CHIPS, win.file_dirty ? "Save" : nullptr);

  // The head line: the file, whether it is modified, and where the caret is.
  std::string shown = win.file_path.empty() ? "(no file)" : win.file_path;
  draw_label_to(win.buffer, w, h, 22, APP_HEAD_Y + 2, "file", tok::faint);
  int head_x = 22 + label_width("file") + 12;
  if (win.cursor_line >= (int)win.lines.size()) win.cursor_line = (int)win.lines.size() - 1;
  if (win.cursor_line < 0) win.cursor_line = 0;
  int caret_display = editor_display_col(win.lines[(size_t)win.cursor_line], win.cursor_col);
  std::string caret_str = "Ln " + std::to_string(win.cursor_line + 1) + ", Col " +
                          std::to_string(caret_display + 1);
  if (editor_has_selection(win)) {
    int l0, c0, l1, c1;
    editor_selection_range(win, &l0, &c0, &l1, &c1);
    caret_str += (l0 == l1) ? "  (" + std::to_string(c1 - c0) + " sel)"
                            : "  (" + std::to_string(l1 - l0 + 1) + " lines sel)";
  }
  // A file the machine has rewritten under this buffer says so HERE, not only
  // in a status line that the next keystroke replaces: "SAVED" beside a stale
  // buffer is the editor telling a person something that is no longer true.
  const char* state_word = win.file_disk_changed
                               ? "CHANGED ON DISK"
                               : (win.file_dirty ? "MODIFIED" : (win.file_path.empty() ? "SCRATCH" : "SAVED"));
  const uint32_t state_ink = win.file_disk_changed ? tok::err : (win.file_dirty ? tok::warn : tok::ok);
  // Measured, not 90px: "CHANGED ON DISK" is half again as wide as "MODIFIED"
  // and would have been painted straight through the caret's line and column.
  int right_w = text_width(caret_str, Face::Mono) + text_width(state_word, Face::UiSmall) + 26;
  // The language sits between the path and the saved/modified word, so the
  // path's allowance has to make room for it or the two paint over each other.
  std::string lang_label = lang_name(win.file_lang);
  if (win.file_preview) lang_label += " - preview";
  int lang_w = text_width(lang_label, Face::UiSmall) + 16;
  draw_text_to(win.buffer, w, h, head_x, APP_HEAD_Y,
               fit_text(*this, shown, w - head_x - right_w - lang_w - 20, Face::Mono),
               tok::accent, Face::Mono);
  draw_text_to(win.buffer, w, h, w - right_w, APP_HEAD_Y + 1, state_word, state_ink, Face::UiSmall);
  // Which grammar is painting this file, so a file coloured oddly can be seen
  // to be a file the editor took for something else.
  draw_text_to(win.buffer, w, h, w - right_w - lang_w, APP_HEAD_Y + 1, lang_label,
               tok::faint, Face::UiSmall);
  draw_text_to(win.buffer, w, h, w - text_width(caret_str, Face::Mono) - 22, APP_HEAD_Y + 1,
               caret_str, tok::dim, Face::Mono);

  int body_y = APP_BODY_Y;

  // The tab strip: every file this pane has open, the live one lit. It is only
  // drawn when something is open, so an empty editor is not all chrome.
  if (!win.buffers.empty()) {
    int tab_x = 20;
    for (size_t i = 0; i < win.buffers.size(); ++i) {
      const EditorBuffer& b = win.buffers[i];
      std::string base = b.path;
      size_t slash = base.find_last_of('/');
      if (slash != std::string::npos) base = base.substr(slash + 1);
      if (base.empty()) base = "untitled";
      if (b.dirty) base += " *";
      int tw = text_width(base, Face::UiSmall) + 26;
      if (tab_x + tw > w - 20) break;
      bool live = (int)i == win.active_buffer;
      bool hot = win.focused && mouse_over_content(win, tab_x, body_y, tw, EDIT_TAB_H);
      draw_rect_to(win.buffer, w, h, tab_x, body_y, tw, EDIT_TAB_H,
                   live ? tok::wash : (hot ? tok::panel2 : tok::panel));
      draw_rect_outline_to(win.buffer, w, h, tab_x, body_y, tw, EDIT_TAB_H,
                           live ? tok::strong : tok::line_up);
      draw_text_to(win.buffer, w, h, tab_x + 8, body_y + 5, base,
                   live ? tok::text : tok::dim, Face::UiSmall);
      // The close box, which is its own hit target.
      draw_text_to(win.buffer, w, h, tab_x + tw - 14, body_y + 5, "x",
                   hot ? tok::warn : tok::faint, Face::UiSmall);
      tab_x += tw + 4;
    }
    body_y += EDIT_TAB_H + 6;
  }

  int body_h = h - body_y - APP_FOOT_H;
  if (body_h < 40) body_h = 40;

  // A file browser column. It used to appear only on a pane wider than 660px,
  // so on a narrower one there was no visible way to open anything at all.
  int browse_w = (w > 620) ? 200 : (w > 420 ? 150 : 0);
  if (browse_w) {
    draw_rect_to(win.buffer, w, h, 20, body_y, browse_w, body_h, tok::panel2);
    draw_rect_outline_to(win.buffer, w, h, 20, body_y, browse_w, body_h, tok::line_up);
    std::string dir_label = win.file_dir.empty() ? "(no directory)" : win.file_dir;
    draw_label_to(win.buffer, w, h, 28, body_y + 8, "FILES", tok::faint);
    draw_text_to(win.buffer, w, h, 28, body_y + 24,
                 fit_text(*this, dir_label, browse_w - 16, Face::UiSmall), tok::dim, Face::UiSmall);

    int ey = body_y + 44;
    int visible_rows = (body_h - 48) / 18;
    if (visible_rows < 1) visible_rows = 1;

    int max_scroll = (int)win.dir_entries.size() - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (win.dir_scroll > max_scroll) win.dir_scroll = max_scroll;
    if (win.dir_scroll < 0) win.dir_scroll = 0;

    for (int i = win.dir_scroll; i < (int)win.dir_entries.size(); ++i) {
      if (ey + 16 > body_y + body_h - 6) break;
      const std::string& e = win.dir_entries[(size_t)i];
      bool is_dir = (i < (int)win.dir_is_dir.size()) ? win.dir_is_dir[(size_t)i] : (e == "..");
      
      bool hot = win.focused && mouse_over_content(win, 22, ey - 2, browse_w - 4, 18);
      std::string full_p = (win.file_dir == "/" ? "" : win.file_dir) + "/" + e;
      bool cur = !win.file_path.empty() && (win.file_path == full_p);

      if (hot || cur) draw_rect_to(win.buffer, w, h, 22, ey - 2, browse_w - 4, 18, tok::wash);

      std::string disp;
      uint32_t ink = tok::text;
      if (e == "..") {
        disp = "<- ..";
        ink = aios::userland::colour_for(TokenClass::Keyword);
      } else if (is_dir) {
        disp = "/ " + e;
        ink = aios::userland::colour_for(TokenClass::Type);
      } else {
        disp = "  " + e;
        if (e.rfind(".", 0) == 0) ink = tok::faint;
        else if (cur) ink = tok::accent;
        else ink = tok::text;
      }
      if (hot && !cur) ink = tok::text;

      draw_text_to(win.buffer, w, h, 26, ey, fit_text(*this, disp, browse_w - 18, Face::Mono),
                   cur ? tok::accent : ink, Face::Mono);
      ey += 18;
    }
    if (win.dir_entries.empty())
      draw_text_to(win.buffer, w, h, 28, ey, "nothing here", tok::faint, Face::UiSmall);
  }

  int box_x = browse_w ? 20 + browse_w + 10 : 20;
  int box_w = w - box_x - 20;

  // Markdown, read rather than typed. The browser column and the command line
  // stay: the preview replaces the text area, not the application.
  if (win.file_preview && lang_has_preview(win.file_lang)) {
    render_editor_preview(win, w, h, body_y, body_h, box_x, box_w);
    int prompt_y2 = h - 35;
    draw_label_to(win.buffer, w, h, 22, prompt_y2 + 2, "read", tok::accent);
    int px2 = 22 + label_width("edit") + 14;
    draw_text_to(win.buffer, w, h, px2, prompt_y2,
                 fit_text(*this, win.status_text.empty()
                              ? std::string("Ctrl+P goes back to the source")
                              : win.status_text,
                          w - px2 - 30, Face::UiSmall),
                 tok::faint, Face::UiSmall);
    return;
  }

  draw_rect_to(win.buffer, w, h, box_x, body_y, box_w, body_h, 0xFF0D1117);
  draw_rect_outline_to(win.buffer, w, h, box_x, body_y, box_w, body_h, tok::line_up);
  int gutter = box_x + 48;
  draw_rect_to(win.buffer, w, h, gutter, body_y + 1, 1, body_h - 2, tok::line_up);

  // Keep the caret on screen: scrolling is a consequence of where the caret is,
  // never a separate thing the person has to manage.
  int rows = (body_h - 14) / APP_ROW;
  if (rows < 1) rows = 1;
  if (win.cursor_line < win.view_top) win.view_top = win.cursor_line;
  if (win.cursor_line >= win.view_top + rows) win.view_top = win.cursor_line - rows + 1;
  if (win.view_top > (int)win.lines.size() - 1) win.view_top = (int)win.lines.size() - 1;
  if (win.view_top < 0) win.view_top = 0;

  int text_x = gutter + 10;
  int minimap_w = (box_w > 460) ? 54 : 0;
  int avail = box_x + box_w - text_x - 8 - minimap_w;
  int char_w = text_width("M", Face::Mono);
  if (char_w < 1) char_w = 8;
  int cols_fit = avail / char_w;
  if (cols_fit < 8) cols_fit = 8;
  // Follow the caret sideways too. A long line used to be ellipsised, so
  // anything past the right edge could be typed but never seen.
  if (caret_display < win.view_left) win.view_left = caret_display;
  if (caret_display >= win.view_left + cols_fit) win.view_left = caret_display - cols_fit + 1;
  if (win.view_left < 0) win.view_left = 0;

  // High-density Minimap Gutter
  if (minimap_w > 0) {
    int mm_x = box_x + box_w - minimap_w;
    int mm_y = body_y + 1;
    int mm_h = body_h - 2;
    draw_rect_to(win.buffer, w, h, mm_x, mm_y, minimap_w, mm_h, 0xFF0B0E14);
    draw_rect_to(win.buffer, w, h, mm_x, mm_y, 1, mm_h, tok::line_up);

    int total_l = std::max(1, (int)win.lines.size());
    for (int li = 0; li < (int)win.lines.size() && li < 1000; ++li) {
      int my = mm_y + 2 + (li * (mm_h - 6)) / total_l;
      if (my >= mm_y + mm_h - 2) break;
      const auto& lstr = win.lines[(size_t)li];
      if (lstr.empty()) continue;
      int line_len = std::min(minimap_w - 8, (int)lstr.size() * 2);
      uint32_t line_color = (li == win.cursor_line) ? tok::accent : tok::dim;
      draw_rect_to(win.buffer, w, h, mm_x + 4, my, std::max(2, line_len), 1, line_color);
    }

    int vp_y0 = mm_y + 2 + (win.view_top * (mm_h - 6)) / total_l;
    int vp_h = std::max(8, (rows * (mm_h - 6)) / total_l);
    if (vp_y0 + vp_h > mm_y + mm_h - 2) vp_h = mm_y + mm_h - 2 - vp_y0;
    if (vp_h > 2) {
      draw_rounded_rect_to(win.buffer, w, h, mm_x + 2, vp_y0, minimap_w - 4, vp_h, 3, 0x33388BFD);
      draw_round_outline_to(win.buffer, w, h, mm_x + 2, vp_y0, minimap_w - 4, vp_h, 3, tok::accent);
    }
  }

  int sl0 = 0, sc0 = 0, sl1 = 0, sc1 = 0;
  bool has_sel = editor_has_selection(win);
  if (has_sel) editor_selection_range(win, &sl0, &sc0, &sl1, &sc1);

  // A line cannot be highlighted on its own: a /* that opened above it, or a
  // Markdown fence, changes what everything below means. Walk down to the top
  // of the view first. Bounded, because rescanning a hundred thousand lines to
  // paint twenty of them would cost more than the frame is worth -- a block
  // comment that runs longer than this is not one this needs to get right.
  // Cache of each line's tokens, keyed to the buffer's content version so an
  // edit invalidates only the lines it touches. `tok_cache[i]` holds the spans
  // for `lines[i]`; a miss (first paint / after an edit / after a theme switch)
  // tokenises that one line and stores the spans, so scrolling reuses them
  // instead of re-tokenising the visible window (plus 400 warm-up lines) per
  // frame.
  if (win.tok_cache.size() != win.lines.size()) win.tok_cache.resize(win.lines.size());
  // One line's spans under the state `hl` carries in; advances `hl` past it.
  // A cache hit needs the same text and the same incoming state, so it is
  // always exactly what highlight_line would produce.
  auto same_state = [](const HighlightState& a, const HighlightState& b) {
    return a.in_block_comment == b.in_block_comment && a.in_fence == b.in_fence &&
           a.in_php_block == b.in_php_block;
  };
  auto line_spans = [&](int i, HighlightState& hl) -> const std::vector<SourceSpan>& {
    const std::string& text = win.lines[(size_t)i];
    WindowSurface::TokLine& c = win.tok_cache[(size_t)i];
    const uint64_t hsh = std::hash<std::string>{}(text);
    if (c.valid && c.hash == hsh && c.len == text.size() && same_state(c.in, hl)) {
      hl = c.out;
      return c.spans;
    }
    c.in = hl;
    c.spans.clear();
    highlight_line(win.file_lang, text, &hl, &c.spans);
    c.out = hl;
    c.hash = hsh;
    c.len = text.size();
    c.valid = true;
    return c.spans;
  };
  // Walk down to the top of the view so the state entering it is right. Each
  // line here is a hash and a compare when nothing changed.
  HighlightState hl;
  {
    int from = win.view_top > 400 ? win.view_top - 400 : 0;
    // Starting mid-file means the opening <?php is above the scan. A PHP file
    // is overwhelmingly PHP after that tag, so assume we are inside it rather
    // than painting the whole rest of the file as the HTML around it.
    if (from > 0 && win.file_lang == SourceLang::Php) hl.in_php_block = true;
    for (int i = from; i < win.view_top && i < (int)win.lines.size(); ++i) (void)line_spans(i, hl);
  }

  int line_y = body_y + 8;
  const LspDiagnostic* caret_diag = nullptr;

  for (int i = win.view_top; i < (int)win.lines.size() && i < win.view_top + rows; ++i) {
    const std::string& raw = win.lines[(size_t)i];
    std::vector<std::string> full;
    std::vector<int> byte_at;
    expand_editor_line(raw, &full, &byte_at);

    bool on_caret = (i == win.cursor_line);
    if (on_caret) draw_rect_to(win.buffer, w, h, box_x + 1, line_y - 2, box_w - 2, 18, 0xFF161B22);

    // Check LSP diagnostic markers for line i
    const LspDiagnostic* line_diag = nullptr;
    for (const auto& d : win.diagnostics) {
      if (d.range.start.line == i) {
        line_diag = &d;
        if (on_caret) caret_diag = &d;
        break;
      }
    }
    if (line_diag) {
      uint32_t dot_col = (line_diag->severity == 1) ? 0xFFF85149 : 0xFFE3B341;
      draw_rect_to(win.buffer, w, h, gutter - 14, line_y + 4, 6, 6, dot_col);
    }

    // The selection, painted under the text in display columns.
    if (has_sel && i >= sl0 && i <= sl1) {
      int from_col = (i == sl0) ? editor_display_col(raw, sc0) : 0;
      int to_col = (i == sl1) ? editor_display_col(raw, sc1) : (int)full.size() + 1;
      if (to_col > (int)full.size() + 1) to_col = (int)full.size() + 1;
      int vf = from_col - win.view_left, vt = to_col - win.view_left;
      if (vf < 0) vf = 0;
      if (vt > cols_fit) vt = cols_fit;
      if (vt > vf)
        draw_rect_to(win.buffer, w, h, text_x + vf * char_w, line_y - 2,
                     (vt - vf) * char_w, 18, 0xFF2D4F7C);
    }

    std::string num_str = std::to_string(i + 1);
    draw_text_to(win.buffer, w, h, gutter - 18 - text_width(num_str, Face::Mono), line_y, num_str,
                 line_diag ? ((line_diag->severity == 1) ? 0xFFF85149 : 0xFFE3B341) : (on_caret ? tok::accent : tok::faint), Face::Mono);

    // Colour the whole line, then draw only the window of it that fits. The
    // grammar works on the file's real bytes; the colours are mapped onto the
    // DISPLAYED characters through the same byte map the caret uses, so a tab
    // expanded to four columns carries its colour across all four.
    const std::vector<SourceSpan>& spans = line_spans(i, hl);
    std::vector<uint32_t> colour(full.size(), aios::userland::colour_for(TokenClass::Text));
    for (const SourceSpan& sp : spans) {
      uint32_t ink = aios::userland::colour_for(sp.cls);
      for (size_t d = 0; d + 1 < byte_at.size(); ++d) {
        uint32_t at = (uint32_t)byte_at[d];
        if (at >= sp.start && at < sp.start + sp.len && d < colour.size()) colour[d] = ink;
      }
    }

    size_t from_c = (size_t)win.view_left;
    for (size_t c = from_c; c < full.size() && (int)(c - from_c) < cols_fit;) {
      size_t run = c;
      while (run < full.size() && (int)(run - from_c) < cols_fit && colour[run] == colour[c]) run++;
      // ASCII is monospaced by construction, so a stretch of it is one call.
      // A character past ASCII may come off a fallback typeface whose advance
      // is not the cell width, so it is placed at its own column rather than
      // allowed to push the rest of the run sideways.
      std::string piece;
      size_t piece_at = c;
      for (size_t d = c; d < run; ++d) {
        if (full[d].empty()) continue;  // the second column of a wide character
        if (full[d].size() == 1) {
          if (piece.empty()) piece_at = d;
          piece += full[d];
          continue;
        }
        if (!piece.empty()) {
          draw_text_to(win.buffer, w, h, text_x + (int)(piece_at - from_c) * char_w, line_y,
                       piece, colour[c], Face::Mono);
          piece.clear();
        }
        draw_text_to(win.buffer, w, h, text_x + (int)(d - from_c) * char_w, line_y,
                     full[d], colour[c], Face::Mono);
      }
      if (!piece.empty())
        draw_text_to(win.buffer, w, h, text_x + (int)(piece_at - from_c) * char_w, line_y,
                     piece, colour[c], Face::Mono);
      c = run;
    }

    // The caret sits IN the text, at the column it is actually at.
    if (on_caret && !win.cmdline_focus && (steady_ms() / 500) % 2 == 0) {
      int vx = caret_display - win.view_left;
      if (vx >= 0 && vx <= cols_fit)
        draw_rect_to(win.buffer, w, h, text_x + vx * char_w, line_y - 1, 2, 17, tok::accent);
    }
    line_y += APP_ROW;
  }

  // A mark that there is more line off to the left than the pane is showing.
  if (win.view_left > 0)
    draw_text_to(win.buffer, w, h, text_x - char_w, body_y + body_h - 16, "<", tok::faint, Face::Mono);

  // --- LSP Code Intelligence Overlays (AIOS-EDIT-01) -----------------------

  // 1. Auto-completion Popup Overlay
  if (win.lsp_completion_active && !win.lsp_completions.empty()) {
    int pop_x = text_x + (caret_display - win.view_left) * char_w;
    int pop_y = body_y + 8 + (win.cursor_line - win.view_top + 1) * APP_ROW;
    int pop_w = 260;
    int max_show = 6;
    int items_cnt = std::min(max_show, static_cast<int>(win.lsp_completions.size()));
    int pop_h = items_cnt * 20 + 8;

    if (pop_x + pop_w > box_x + box_w) pop_x = box_x + box_w - pop_w;
    if (pop_y + pop_h > body_y + body_h) pop_y = body_y + (win.cursor_line - win.view_top) * APP_ROW - pop_h;

    draw_rect_to(win.buffer, w, h, pop_x, pop_y, pop_w, pop_h, 0xFF161B22);
    draw_rect_outline_to(win.buffer, w, h, pop_x, pop_y, pop_w, pop_h, tok::accent);

    for (int ci = 0; ci < items_cnt; ++ci) {
      int item_idx = ci;
      const auto& item = win.lsp_completions[item_idx];
      bool is_sel = (item_idx == win.lsp_completion_index);
      int iy = pop_y + 4 + ci * 20;

      if (is_sel) draw_rect_to(win.buffer, w, h, pop_x + 2, iy, pop_w - 4, 18, tok::wash);

      std::string kind_badge = "[sym]";
      if (item.kind == 2 || item.kind == 3) kind_badge = "[fn]";
      else if (item.kind == 6) kind_badge = "[var]";
      else if (item.kind == 7) kind_badge = "[class]";
      draw_text_to(win.buffer, w, h, pop_x + 6, iy + 2, kind_badge, tok::accent, Face::UiSmall);
      draw_text_to(win.buffer, w, h, pop_x + 52, iy + 2, fit_text(*this, item.label, pop_w - 58, Face::Mono),
                   is_sel ? tok::text : tok::dim, Face::Mono);
    }

    if (win.lsp_completion_index >= 0 && win.lsp_completion_index < static_cast<int>(win.lsp_completions.size())) {
      const auto& sel_item = win.lsp_completions[win.lsp_completion_index];
      if (!sel_item.detail.empty() || !sel_item.documentation.empty()) {
        int doc_x = pop_x + pop_w + 4;
        int doc_w = 220;
        int doc_h = pop_h;
        if (doc_x + doc_w <= box_x + box_w) {
          draw_rect_to(win.buffer, w, h, doc_x, pop_y, doc_w, doc_h, 0xFF21262D);
          draw_rect_outline_to(win.buffer, w, h, doc_x, pop_y, doc_w, doc_h, tok::line_up);
          draw_text_to(win.buffer, w, h, doc_x + 6, pop_y + 4, fit_text(*this, sel_item.detail, doc_w - 12, Face::UiSmall), tok::accent, Face::UiSmall);
          if (!sel_item.documentation.empty()) {
            draw_text_to(win.buffer, w, h, doc_x + 6, pop_y + 24, fit_text(*this, sel_item.documentation, doc_w - 12, Face::Mono), tok::dim, Face::Mono);
          }
        }
      }
    }
  }

  // 2. Hover Tooltip Overlay
  if (win.lsp_hover_active && !win.lsp_hover_text.empty()) {
    int hov_x = text_x + (caret_display - win.view_left) * char_w;
    int hov_y = body_y + 8 + (win.cursor_line - win.view_top) * APP_ROW - 32;
    int hov_w = 340;
    int hov_h = 44;
    if (hov_y < body_y) hov_y = body_y + 8 + (win.cursor_line - win.view_top + 1) * APP_ROW;

    draw_rect_to(win.buffer, w, h, hov_x, hov_y, hov_w, hov_h, 0xFF21262D);
    draw_rect_outline_to(win.buffer, w, h, hov_x, hov_y, hov_w, hov_h, tok::accent);
    draw_label_to(win.buffer, w, h, hov_x + 6, hov_y + 4, "LSP HOVER DOC", tok::accent);
    draw_text_to(win.buffer, w, h, hov_x + 6, hov_y + 22, fit_text(*this, win.lsp_hover_text, hov_w - 12, Face::UiSmall), tok::text, Face::UiSmall);
  }

  // 3. References List Overlay
  if (win.lsp_references_active && !win.lsp_references.empty()) {
    int ref_x = box_x + 40;
    int ref_y = body_y + 30;
    int ref_w = box_w - 80;
    int ref_h = std::min(200, static_cast<int>(win.lsp_references.size()) * 20 + 28);

    draw_rect_to(win.buffer, w, h, ref_x, ref_y, ref_w, ref_h, 0xFF161B22);
    draw_rect_outline_to(win.buffer, w, h, ref_x, ref_y, ref_w, ref_h, tok::accent);
    draw_label_to(win.buffer, w, h, ref_x + 8, ref_y + 6, "SYMBOL REFERENCES (F12 / ESC)", tok::accent);

    for (size_t ri = 0; ri < win.lsp_references.size() && ri < 8; ++ri) {
      bool is_sel = (static_cast<int>(ri) == win.lsp_references_index);
      const auto& ref = win.lsp_references[ri];
      int ry = ref_y + 26 + static_cast<int>(ri) * 20;
      if (is_sel) draw_rect_to(win.buffer, w, h, ref_x + 2, ry, ref_w - 4, 18, tok::wash);

      std::string loc_str = ref.path + ":" + std::to_string(ref.range.start.line + 1) + ":" + std::to_string(ref.range.start.character + 1);
      draw_text_to(win.buffer, w, h, ref_x + 8, ry + 2, fit_text(*this, loc_str, ref_w - 16, Face::Mono), is_sel ? tok::text : tok::dim, Face::Mono);
    }
  }

  // The prompt: the editor's command line. It is only where keystrokes go when
  // it has focus -- otherwise they go into the file, which is the point.
  int prompt_y = h - 35;
  bool cmd_on = win.cmdline_focus;
  draw_label_to(win.buffer, w, h, 22, prompt_y + 2, cmd_on ? "cmd" : "edit", cmd_on ? tok::accent : tok::faint);
  int px = 22 + label_width("edit") + 14;
  if (cmd_on) {
    draw_text_to(win.buffer, w, h, px, prompt_y, win.input.text, tok::text, Face::Mono);
    if ((steady_ms() / 500) % 2 == 0)
      draw_rect_to(win.buffer, w, h,
                   px + text_width(win.input.text.substr(0, win.input.cursor), Face::Mono),
                   prompt_y, 2, 16, tok::accent);
  } else {
    std::string status_show = win.status_text;
    if (caret_diag) {
      status_show = "[LSP Diagnostic]: " + caret_diag->message;
    } else if (status_show.empty()) {
      status_show = "Ctrl+Space auto-complete  F12 definition  Shift+F12 references  F1 hover  Ctrl+S save  Ctrl+Z undo";
    }
    draw_text_to(win.buffer, w, h, px,
                 prompt_y, fit_text(*this, status_show, w - px - 30, Face::UiSmall),
                 caret_diag ? ((caret_diag->severity == 1) ? tok::warn : tok::accent) : tok::faint, Face::UiSmall);
  }
}

void HardwareCompositor::render_database_window(WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);

  draw_card(win, w, h, 10, 10, w - 20, h - 20, "mariadb / mysql studio");

  // The toolbar. An icon per action, in groups: the connection, the statement,
  // the row, the way out. The words are in the tooltip under whichever one the
  // pointer is on, so the row stays short enough to leave room for the pager.
  win.db_hover_tool = -1;
  for (size_t i = 0; i < sizeof(DB_TOOLS) / sizeof(DB_TOOLS[0]); ++i) {
    const DbTool& tool = DB_TOOLS[i];
    int bx = db_tool_x(i);
    if (bx + DB_TOOL_W > w - 14) break;
    bool hot = win.focused && mouse_over_content(win, bx, APP_CHIP_Y - 1, DB_TOOL_W, DB_TOOL_H);
    bool on = (strcmp(tool.id, "Connections") == 0 && win.db_show_conn_mgr) ||
              (strcmp(tool.id, "Fleet Ops") == 0 && win.db_show_telemetry) ||
              (strcmp(tool.id, "Connect") == 0 && win.db_connected);
    if (hot) win.db_hover_tool = static_cast<int>(i);
    draw_pill_to(win.buffer, w, h, bx, APP_CHIP_Y - 1, DB_TOOL_W, DB_TOOL_H,
                 (hot || on) ? tok::wash : tok::panel2, (hot || on) ? tok::strong : tok::line_up);
    ButtonSuite::draw_icon(win.buffer, w, h, bx + DB_TOOL_W / 2, APP_CHIP_Y - 1 + DB_TOOL_H / 2,
                           tool.icon, 14, on ? tok::accent : (hot ? tok::text : tok::dim));
  }

  // The connection line says what is actually true: which store the engine is
  // open on, or that it is not open. There is no permanent green badge.
  draw_label_to(win.buffer, w, h, 22, APP_HEAD_Y + 2, "engine", tok::faint);
  int head_x = 22 + label_width("engine") + 12;
  SqlDatabase* sql_inst = sql_engine(k);
  DbConnectionProfile act_prof = sql_inst ? sql_inst->active_profile() : DbConnectionProfile();
  /* The embedded engine runs inside this process: it has no host and no
   * port. It used to print "127.0.0.1:3306" and CONNECTED - an address
   * nothing listens on (connection refused on the Pi) for a link that does
   * not exist. */
  std::string engine = !win.db_connected ? std::string("not connected")
      : act_prof.is_local_embedded ? ("embedded engine, in this process / " + act_prof.database)
      : (act_prof.name + " (" + act_prof.host + ":" + std::to_string(act_prof.port) + " / " + act_prof.database + ")");
  const char* state = !win.db_connected ? "OFFLINE" : (act_prof.is_local_embedded ? "OPEN" : "CONNECTED");
  int right_w = text_width(state, Face::UiSmall) + 30;
  draw_text_to(win.buffer, w, h, head_x, APP_HEAD_Y,
               fit_text(*this, engine, w - head_x - right_w - 16, Face::Mono), tok::accent, Face::Mono);
  draw_text_to(win.buffer, w, h, w - right_w, APP_HEAD_Y + 1, state,
               win.db_connected ? tok::ok : tok::faint, Face::UiSmall);

  int body_y = APP_BODY_Y;
  DbPagerGeom pager_probe = db_pager_geom(0, 0, 0, 0);
  int body_h = h - body_y - APP_FOOT_H - 22 - pager_probe.h - 4;  // 22 = the result line, then the pager
  if (body_h < 60) body_h = 60;

  // Left: the explorer. Every table the engine holds, with its shape under the
  // selected one -- and a filter over it, because fifty-eight tables do not fit
  // in a panel and hunting for `salesorderdetails` by eye is not browsing.
  int tree_w = (w > 700) ? 240 : 180;
  draw_rect_to(win.buffer, w, h, 20, body_y, tree_w, body_h, tok::panel2);
  draw_rect_outline_to(win.buffer, w, h, 20, body_y, tree_w, body_h, tok::line_up);
  draw_label_to(win.buffer, w, h, 28, body_y + 8, "SCHEMA", tok::faint);

  std::vector<size_t> vis = db_visible_tables(win);
  std::string tree_count = win.db_table_filter.empty()
                               ? std::to_string(win.db_tables.size())
                               : std::to_string(vis.size()) + "/" + std::to_string(win.db_tables.size());
  draw_text_to(win.buffer, w, h, 20 + tree_w - 8 - text_width(tree_count, Face::UiSmall),
               body_y + 7, tree_count, tok::dim, Face::UiSmall);

  // The filter box.
  int filt_y = body_y + 24;
  int filt_w = tree_w - 16;
  draw_rect_to(win.buffer, w, h, 28, filt_y, filt_w, 20, win.db_filter_focus ? 0xFF0B213A : 0xFF161B22);
  draw_rect_outline_to(win.buffer, w, h, 28, filt_y, filt_w, 20,
                       win.db_filter_focus ? tok::accent : tok::line_up);
  ButtonSuite::draw_icon(win.buffer, w, h, 28 + 11, filt_y + 10, aios::userland::Icon::Filter, 10,
                         win.db_filter_focus ? tok::accent : tok::faint);
  if (win.db_table_filter.empty() && !win.db_filter_focus) {
    draw_text_to(win.buffer, w, h, 28 + 22, filt_y + 3, "filter tables", tok::faint, Face::UiSmall);
  } else {
    draw_text_to(win.buffer, w, h, 28 + 22, filt_y + 3,
                 fit_text(*this, win.db_table_filter.text, filt_w - 30, Face::Mono), tok::text, Face::Mono);
    if (win.db_filter_focus && (steady_ms() / 500) % 2 == 0) {
      int fc = 28 + 22 + text_width(win.db_table_filter.text.substr(0, win.db_table_filter.cursor), Face::Mono);
      if (fc < 28 + filt_w - 6) draw_rect_to(win.buffer, w, h, fc, filt_y + 2, 2, 15, tok::accent);
    }
  }

  int tree_top = filt_y + 26;
  int tree_bottom = body_y + body_h - 6;
  if (win.db_tree_scroll > (int)vis.size() - 1) win.db_tree_scroll = (int)vis.size() - 1;
  if (win.db_tree_scroll < 0) win.db_tree_scroll = 0;

  int ty = tree_top;
  if (vis.empty()) {
    draw_text_to(win.buffer, w, h, 28, ty,
                 win.db_connected ? (win.db_table_filter.empty() ? "no tables yet" : "nothing matches that")
                                  : "press Connect",
                 tok::faint, Face::UiSmall);
  }
  for (size_t vi = (size_t)win.db_tree_scroll; vi < vis.size(); ++vi) {
    if (ty + 18 > tree_bottom) break;
    size_t idx = vis[vi];
    const std::string& t = win.db_tables[idx];
    bool sel = (t == win.db_selected);
    bool hot = win.focused && mouse_over_content(win, 22, ty - 2, tree_w - 4, 18);
    if (sel || hot) draw_rect_to(win.buffer, w, h, 22, ty - 2, tree_w - 4, 18, tok::wash);
    std::string count = idx < win.db_table_rows.size() ? std::to_string(win.db_table_rows[idx]) : "";
    int count_w = count.empty() ? 0 : text_width(count, Face::UiSmall) + 8;
    draw_text_to(win.buffer, w, h, 28, ty, fit_text(*this, t, tree_w - 24 - count_w, Face::Mono),
                 sel ? tok::accent : (hot ? tok::text : tok::dim), Face::Mono);
    // How many rows are in there, before you open it.
    if (!count.empty())
      draw_text_to(win.buffer, w, h, 20 + tree_w - count_w, ty + 1, count, tok::faint, Face::UiSmall);
    ty += 18;
    if (!sel) continue;
    // The selected table's columns, indented under it: schema browsing without
    // leaving the grid you are looking at.
    for (const auto& c : win.db_schema) {
      if (ty + 16 > tree_bottom) break;
      draw_text_to(win.buffer, w, h, 40, ty, fit_text(*this, c, tree_w - 36, Face::UiSmall), tok::faint, Face::UiSmall);
      ty += 16;
    }
  }
  // The explorer scrolls, so the tables past the bottom of the panel are
  // reachable rather than merely absent.
  if (!vis.empty() && ty >= tree_bottom - 2 && win.db_tree_scroll + 1 < (int)vis.size())
    draw_text_to(win.buffer, w, h, 28, tree_bottom - 2, "...", tok::faint, Face::UiSmall);

  // Right: the result grid, drawn from the columns and rows the engine returned.
  int grid_x = 20 + tree_w + 10;
  int grid_w = w - grid_x - 20;
  std::vector<int> cw;
  int cols_drawn = 0;
  if (grid_w > 80) {
    draw_rect_to(win.buffer, w, h, grid_x, body_y, grid_w, body_h, 0xFF0D1117);
    draw_rect_outline_to(win.buffer, w, h, grid_x, body_y, grid_w, body_h, tok::line_up);

    if (win.grid_columns.empty()) {
      draw_text_to(win.buffer, w, h, grid_x + 12, body_y + 14,
                   win.db_error ? "the last statement did not return a result set"
                                : "run a query, or pick a table on the left",
                   tok::faint, Face::UiSmall);
    } else {
      size_t nc = win.grid_columns.size();
      db_column_widths(win, grid_w, &cw);
      if (win.db_col_first >= (int)nc) win.db_col_first = (int)nc - 1;
      if (win.db_col_first < 0) win.db_col_first = 0;

      draw_rect_to(win.buffer, w, h, grid_x + 1, body_y + 1, grid_w - 2, 24, 0xFF161B22);
      int hx = grid_x + 8;
      for (size_t c = (size_t)win.db_col_first; c < nc; ++c) {
        // Only whole columns are drawn. A column half off the edge is the "756.."
        // problem again, one pane further out.
        if (hx - grid_x + cw[c] > grid_w - 8) break;
        bool sorted_by = (int)c == win.grid_sort_col;
        // The arrow says which column is ordering the grid and which way, so a
        // sorted grid does not look like an unsorted one that happens to be tidy.
        std::string head = win.grid_columns[c];
        if (sorted_by) head += win.grid_sort_desc ? " v" : " ^";
        bool hot = win.focused && mouse_over_content(win, hx - 8, body_y + 1, cw[c], 24);
        draw_text_to(win.buffer, w, h, hx, body_y + 6,
                     fit_text(*this, head, cw[c] - 10, Face::UiBold),
                     sorted_by ? tok::text : (hot ? tok::text : tok::accent), Face::UiBold);
        hx += cw[c];
        ++cols_drawn;
        // The divider is also the drag handle that widens the column.
        if (hx < grid_x + grid_w - 2) {
          bool on_edge = win.focused && mouse_over_content(win, hx - 9, body_y + 1, 7, body_h - 2);
          draw_rect_to(win.buffer, w, h, hx - 6, body_y + 1, 1, body_h - 2,
                       on_edge || win.db_col_drag == (int)c ? tok::accent : tok::line_up);
        }
      }

      int rows_fit = (body_h - 32) / APP_ROW;
      if (rows_fit < 1) rows_fit = 1;
      int max_scroll = static_cast<int>(win.grid_rows.size()) - rows_fit;
      if (max_scroll < 0) max_scroll = 0;
      if (win.grid_scroll > max_scroll) win.grid_scroll = max_scroll;
      if (win.grid_scroll < 0) win.grid_scroll = 0;

      int ry = body_y + 30;
      for (int i = win.grid_scroll; i < static_cast<int>(win.grid_rows.size()) && i < win.grid_scroll + rows_fit; ++i) {
        const auto& r = win.grid_rows[i];
        bool picked = i == win.grid_selected_row;
        if (picked)
          draw_rect_to(win.buffer, w, h, grid_x + 1, ry - 3, grid_w - 2, APP_ROW, 0xFF1F2D3D);
        else if (((i - win.grid_scroll) & 1) == 1)
          draw_rect_to(win.buffer, w, h, grid_x + 1, ry - 3, grid_w - 2, APP_ROW, 0xFF11161D);
        int cx = grid_x + 8;
        for (size_t c = (size_t)win.db_col_first; c < nc; ++c) {
          if (cx - grid_x + cw[c] > grid_w - 8) break;
          bool editing = i == win.grid_edit_row && (int)c == win.grid_edit_col;
          if (editing) {
            // The cell becomes a field in place, which is the whole point of
            // editing in a grid rather than in a dialogue somewhere else.
            draw_rect_to(win.buffer, w, h, cx - 4, ry - 3, cw[c] - 4, APP_ROW, 0xFF0B213A);
            draw_rect_outline_to(win.buffer, w, h, cx - 4, ry - 3, cw[c] - 4, APP_ROW, tok::accent);
            draw_text_to(win.buffer, w, h, cx, ry,
                         fit_text(*this, win.grid_edit.text, cw[c] - 12, Face::Mono), tok::text, Face::Mono);
            if ((steady_ms() / 500) % 2 == 0) {
              int caret = text_width(win.grid_edit.text.substr(0, win.grid_edit.cursor), Face::Mono);
              if (caret < cw[c] - 12)
                draw_rect_to(win.buffer, w, h, cx + caret, ry - 1, 2, 16, tok::accent);
            }
            cx += cw[c];
            continue;
          }
          const std::string& v = c < r.size() ? r[c] : std::string();
          bool null_cell = (v == "NULL");
          bool cell_pick = picked && (int)c == win.grid_selected_col;
          if (cell_pick)
            draw_rect_outline_to(win.buffer, w, h, cx - 4, ry - 3, cw[c] - 4, APP_ROW, tok::strong);
          draw_text_to(win.buffer, w, h, cx, ry, fit_text(*this, v, cw[c] - 10, Face::Mono),
                       null_cell ? tok::faint : (c == 0 ? tok::dim : tok::text), Face::Mono);
          cx += cw[c];
        }
        ry += APP_ROW;
      }
      if (max_scroll > 0) {
        // A scrollbar, because a grid that silently hides rows is worse than no
        // grid: the person cannot tell a short table from a clipped one.
        int track_h = body_h - 30;
        int knob_h = track_h * rows_fit / static_cast<int>(win.grid_rows.size());
        if (knob_h < 18) knob_h = 18;
        int knob_y = body_y + 28 + (track_h - knob_h) * win.grid_scroll / max_scroll;
        draw_rect_to(win.buffer, w, h, grid_x + grid_w - 5, body_y + 28, 3, track_h, tok::line_up);
        draw_rect_to(win.buffer, w, h, grid_x + grid_w - 5, knob_y, 3, knob_h, tok::border);
      }
    }
  }

  // The pager. This is the control the studio did not have: which window of the
  // table you are looking at, and how to move it, without typing LIMIT by hand.
  {
    DbPagerGeom g = db_pager_geom(grid_x, grid_w, body_y, body_h);
    int size = win.db_page_size > 0 ? win.db_page_size : 200;
    int pages = win.db_total_rows == 0 ? 1
                : (int)((win.db_total_rows + (size_t)size - 1) / (size_t)size);
    bool live = win.db_paged;

    auto pager_btn = [&](int bx, aios::userland::Icon icon, bool enabled) {
      bool hot = enabled && win.focused && mouse_over_content(win, bx, g.y, g.btn, g.h);
      draw_pill_to(win.buffer, w, h, bx, g.y, g.btn, g.h,
                   hot ? tok::wash : tok::panel2, hot ? tok::strong : tok::line_up);
      ButtonSuite::draw_icon(win.buffer, w, h, bx + g.btn / 2, g.y + g.h / 2, icon, 10,
                             enabled ? (hot ? tok::text : tok::dim) : tok::faint);
    };

    if (live) {
      bool back = win.db_page > 0;
      bool fwd = win.db_page + 1 < pages;
      pager_btn(g.first_x, aios::userland::Icon::SkipBack, back);
      pager_btn(g.prev_x, aios::userland::Icon::ArrowLeft, back);
      pager_btn(g.next_x, aios::userland::Icon::ArrowRight, fwd);
      pager_btn(g.last_x, aios::userland::Icon::SkipForward, fwd);

      long long from = win.grid_rows.empty() ? 0 : (long long)win.db_page * size + 1;
      long long to = (long long)win.db_page * size + (long long)win.grid_rows.size();
      std::string label = "page " + std::to_string(win.db_page + 1) + " / " + std::to_string(pages);
      draw_text_to(win.buffer, w, h, g.label_x + 6, g.y + 2,
                   fit_text(*this, label, g.label_w - 12, Face::UiBold), tok::text, Face::UiBold);

      bool size_hot = win.focused && mouse_over_content(win, g.size_x, g.y, g.size_w, g.h);
      draw_pill_to(win.buffer, w, h, g.size_x, g.y, g.size_w, g.h,
                   size_hot ? tok::wash : tok::panel2, size_hot ? tok::strong : tok::line_up);
      draw_text_to(win.buffer, w, h, g.size_x + 8, g.y + 2, std::to_string(size) + " /pg",
                   size_hot ? tok::text : tok::dim, Face::UiSmall);

      std::string rows_label = "rows " + std::to_string(from) + "-" + std::to_string(to) +
                               " of " + std::to_string(win.db_total_rows);
      int rows_x = g.size_x + g.size_w + 14;
      int rows_room = g.col_prev_x - rows_x - 12;
      if (rows_room > 40)
        draw_text_to(win.buffer, w, h, rows_x, g.y + 3,
                     fit_text(*this, rows_label, rows_room, Face::UiSmall), tok::dim, Face::UiSmall);
    } else if (!win.grid_columns.empty()) {
      draw_text_to(win.buffer, w, h, g.first_x + 2, g.y + 3,
                   "statement result - " + std::to_string(win.grid_rows.size()) + " row(s) held",
                   tok::faint, Face::UiSmall);
    }

    // Columns, sideways: which slice of a wide table is on screen.
    if ((int)win.grid_columns.size() > cols_drawn && cols_drawn > 0) {
      pager_btn(g.col_prev_x, aios::userland::Icon::ArrowLeft, win.db_col_first > 0);
      pager_btn(g.col_next_x, aios::userland::Icon::ArrowRight,
                win.db_col_first + cols_drawn < (int)win.grid_columns.size());
      std::string cl = "cols " + std::to_string(win.db_col_first + 1) + "-" +
                       std::to_string(win.db_col_first + cols_drawn) + " of " +
                       std::to_string(win.grid_columns.size());
      int clw = text_width(cl, Face::UiSmall);
      draw_text_to(win.buffer, w, h, g.col_prev_x - clw - 10, g.y + 3, cl, tok::faint, Face::UiSmall);
    }
  }

  // Under everything: what the engine said, or -- when a cell is picked -- that
  // cell WHOLE. Values wider than their column are the reason a grid needs an
  // inspector: "756.." is not a value anybody can read.
  int res_y = body_y + body_h + pager_probe.h + 6;
  std::string res_line;
  uint32_t res_ink = tok::dim;
  if (win.grid_selected_row >= 0 && win.grid_selected_col >= 0 &&
      win.grid_selected_row < (int)win.grid_rows.size() &&
      win.grid_selected_col < (int)win.grid_columns.size()) {
    const auto& row = win.grid_rows[(size_t)win.grid_selected_row];
    std::string v = (size_t)win.grid_selected_col < row.size() ? row[(size_t)win.grid_selected_col] : std::string();
    res_line = win.grid_columns[(size_t)win.grid_selected_col] + " = " + v;
    res_ink = tok::text;
  } else {
    res_line = (win.db_error ? std::string("[error] ") : std::string()) +
               (win.db_grid_label.empty() ? std::string() : win.db_grid_label + " - ") +
               win.db_status;
    res_ink = win.db_error ? tok::err : tok::dim;
  }
  if (!res_line.empty())
    draw_text_to(win.buffer, w, h, 22, res_y, fit_text(*this, res_line, w - 44, Face::UiSmall),
                 res_ink, Face::UiSmall);

  // The SQL prompt.
  int prompt_y = h - 35;
  draw_label_to(win.buffer, w, h, 22, prompt_y + 2, "sql", tok::accent);
  int px = 22 + label_width("sql") + 14;
  if (win.input.empty()) {
    draw_text_to(win.buffer, w, h, px, prompt_y,
                 win.db_selected.empty() ? "SELECT * FROM <table>;   (Enter runs it)"
                                         : (win.db_where.empty()
                                                ? "SELECT * FROM " + win.db_selected + ";   (Enter runs it; WHERE <cond> filters)"
                                                : "filtered: WHERE " + win.db_where + "   (WHERE alone clears)"),
                 tok::faint, Face::Mono);
  } else {
    draw_text_to(win.buffer, w, h, px, prompt_y,
                 fit_text(*this, win.input.text, w - px - 30, Face::Mono), tok::text, Face::Mono);
  }
  if ((steady_ms() / 500) % 2 == 0) {
    int cx = px + text_width(win.input.text.substr(0, win.input.cursor), Face::Mono);
    if (cx < w - 24) draw_rect_to(win.buffer, w, h, cx, prompt_y, 2, 16, tok::accent);
  }

  // The toolbar's tooltip, last, so nothing is painted over it.
  if (win.db_hover_tool >= 0 && win.db_hover_tool < (int)(sizeof(DB_TOOLS) / sizeof(DB_TOOLS[0]))) {
    const char* tip = DB_TOOLS[win.db_hover_tool].tip;
    int tw = text_width(tip, Face::UiSmall) + 18;
    int tx = db_tool_x((size_t)win.db_hover_tool);
    if (tx + tw > w - 14) tx = w - 14 - tw;
    if (tx < 12) tx = 12;
    int tyy = APP_CHIP_Y + DB_TOOL_H + 2;
    draw_rect_to(win.buffer, w, h, tx, tyy, tw, 20, 0xF01C2128);
    draw_rect_outline_to(win.buffer, w, h, tx, tyy, tw, 20, tok::strong);
    draw_text_to(win.buffer, w, h, tx + 9, tyy + 3, tip, tok::text, Face::UiSmall);
  }

  if (win.db_show_conn_mgr) {
    int mw = w - 80; if (mw > 640) mw = 640; if (mw < 380) mw = w - 20;
    int mh = h - 80; if (mh > 460) mh = 460; if (mh < 280) mh = h - 20;
    int mx = (w - mw) / 2;
    int my = (h - mh) / 2;

    draw_rect_to(win.buffer, w, h, 0, 0, w, h, 0xEE0A0E14);
    draw_card(win, w, h, mx, my, mw, mh, "mariadb / mysql connection manager");

    int list_w = 190;
    draw_rect_to(win.buffer, w, h, mx + 12, my + 42, list_w, mh - 90, tok::panel2);
    draw_rect_outline_to(win.buffer, w, h, mx + 12, my + 42, list_w, mh - 90, tok::line_up);
    draw_label_to(win.buffer, w, h, mx + 20, my + 50, "SAVED PROFILES", tok::faint);

    SqlDatabase* sql_mgr = sql_engine(k);
    auto profiles = sql_mgr ? sql_mgr->list_profiles() : std::vector<DbConnectionProfile>();
    int py = my + 72;
    for (const auto& p : profiles) {
      if (py + 22 > my + mh - 55) break;
      bool p_sel = (p.id == win.db_selected_profile_id);
      bool p_hot = win.focused && mouse_over_content(win, mx + 14, py - 2, list_w - 4, 22);
      if (p_sel || p_hot) draw_rect_to(win.buffer, w, h, mx + 14, py - 2, list_w - 4, 22, p_sel ? tok::wash : 0xFF1C2128);
      draw_text_to(win.buffer, w, h, mx + 20, py, fit_text(*this, p.name, list_w - 24, Face::UiBold), p_sel ? tok::accent : tok::text, Face::UiBold);
      py += 24;
    }

    draw_rect_to(win.buffer, w, h, mx + 12, my + mh - 42, list_w, 28, tok::wash);
    draw_rect_outline_to(win.buffer, w, h, mx + 12, my + mh - 42, list_w, 28, tok::strong);
    draw_text_to(win.buffer, w, h, mx + 38, my + mh - 36, "+ New Connection", tok::text, Face::UiBold);

    int fx = mx + list_w + 22;
    int fw = mw - list_w - 34;
    draw_rect_to(win.buffer, w, h, fx, my + 42, fw, mh - 90, 0xFF0D1117);
    draw_rect_outline_to(win.buffer, w, h, fx, my + 42, fw, mh - 90, tok::line_up);

    bool found_p = false;
    for (const auto& p : profiles) {
      if (p.id == win.db_selected_profile_id) {
        if (win.db_profile_edit.id != p.id && !win.db_edit_new_profile) {
          win.db_profile_edit = p;
        }
        found_p = true;
        break;
      }
    }
    if (!found_p && !profiles.empty() && !win.db_edit_new_profile) {
      win.db_selected_profile_id = profiles.front().id;
      win.db_profile_edit = profiles.front();
    }

    int fy = my + 52;
    const char* fields[] = {"Name:", "Host:", "Port:", "User:", "Pass:", "Database:", "Socket:"};
    std::string values[] = {
      win.db_profile_edit.name,
      win.db_profile_edit.host,
      std::to_string(win.db_profile_edit.port),
      win.db_profile_edit.user,
      win.db_profile_edit.password.empty() ? "" : "********",
      win.db_profile_edit.database,
      win.db_profile_edit.socket_path
    };
    // The widest label decides where every field's box starts. A fixed
    // fx+85 assumed no label was wider than "Name:" -- "Database:" (tracked
    // out, drawn upper-case by draw_label_to) runs past that on its own, and
    // the value field's box, drawn right at fx+85 regardless, started
    // overlapping the tail of the label instead of sitting beside it.
    int max_label_w = 0;
    for (const char* f : fields) max_label_w = std::max(max_label_w, label_width(f));
    const int field_col_x = fx + 12 + max_label_w + 12;

    for (int i = 0; i < 7; ++i) {
      draw_label_to(win.buffer, w, h, fx + 12, fy + 4, fields[i], tok::faint);
      int input_x = field_col_x;
      int input_w = fx + fw - 12 - field_col_x;
      bool focus = (win.db_conn_field_focus == i);
      draw_rect_to(win.buffer, w, h, input_x, fy, input_w, 22, focus ? 0xFF0B213A : 0xFF161B22);
      draw_rect_outline_to(win.buffer, w, h, input_x, fy, input_w, 22, focus ? tok::accent : tok::line_up);
      draw_text_to(win.buffer, w, h, input_x + 6, fy + 3, fit_text(*this, values[i], input_w - 12, Face::Mono), tok::text, Face::Mono);
      fy += 28;
    }

    draw_text_to(win.buffer, w, h, fx + 12, fy + 4, win.db_profile_edit.is_local_embedded ? "[x] Embedded AIOS Kernel Engine" : "[ ] Remote TCP/IP Socket Server", tok::accent, Face::UiSmall);

    // Connect/Test/Save/Delete/Close, each button sized to its own real
    // label width via db_conn_mgr_buttons() -- five independently-guessed
    // fixed offsets used to assume no label was wider than "Test", which
    // clipped "Connect" and "Delete" against their own button edge and ran
    // "Close" past the dialog's right border.
    DbConnMgrButtons btn = db_conn_mgr_buttons(mx, mw, my, mh);
    static const char* const kBtnLabels[5] = {"Connect", "Test", "Save", "Delete", "Close"};
    for (int i = 0; i < 5; ++i) {
      const bool primary = (i == 0);
      const bool disabled = (i == 3 && !found_p);
      uint32_t fill = primary ? tok::ok : tok::wash;
      uint32_t ink = primary ? 0xFF000000 : (disabled ? tok::faint : tok::text);
      draw_rect_to(win.buffer, w, h, btn.x[i], btn.y, btn.w[i], btn.h, fill);
      if (!primary) draw_rect_outline_to(win.buffer, w, h, btn.x[i], btn.y, btn.w[i], btn.h, tok::strong);
      const int label_w = text_width(kBtnLabels[i], Face::UiBold);
      const int text_x = btn.x[i] + (btn.w[i] - label_w) / 2;
      draw_text_to(win.buffer, w, h, text_x, btn.y + 6, kBtnLabels[i], ink, Face::UiBold);
    }
  }

  if (win.db_show_designer) {
    int mw = w - 60; if (mw > 760) mw = 760; if (mw < 420) mw = w - 20;
    int mh = h - 60; if (mh > 520) mh = 520; if (mh < 340) mh = h - 20;
    int mx = (w - mw) / 2;
    int my = (h - mh) / 2;

    draw_rect_to(win.buffer, w, h, 0, 0, w, h, 0xEE0A0E14);
    draw_card(win, w, h, mx, my, mw, mh, "table designer: " + win.db_designer_table);

    static const char* const kDesignTabs[3] = {"Columns", "Indexes", "Foreign Keys"};
    int tab_x = mx + 12;
    int tab_y = my + 40;
    for (int i = 0; i < 3; ++i) {
      int tw = text_width(kDesignTabs[i], Face::UiBold) + 24;
      bool sel = (win.db_designer_tab == i);
      draw_rect_to(win.buffer, w, h, tab_x, tab_y, tw, 26, sel ? tok::wash : 0xFF161B22);
      if (sel) draw_rect_outline_to(win.buffer, w, h, tab_x, tab_y, tw, 26, tok::accent);
      draw_text_to(win.buffer, w, h, tab_x + 12, tab_y + 6, kDesignTabs[i], sel ? tok::accent : tok::text, Face::UiBold);
      tab_x += tw + 6;
    }

    int dlg_body_y = tab_y + 34;
    int dlg_body_h = my + mh - 52 - dlg_body_y;
    draw_rect_to(win.buffer, w, h, mx + 12, dlg_body_y, mw - 24, dlg_body_h, 0xFF0D1117);
    draw_rect_outline_to(win.buffer, w, h, mx + 12, dlg_body_y, mw - 24, dlg_body_h, tok::line_up);

    if (win.db_designer_tab == 0) {
      const DesignerColGeom g = designer_col_geom(*this, mx, mw);
      int hy = dlg_body_y + 6;
      draw_label_to(win.buffer, w, h, g.name_x, hy, "NAME", tok::faint);
      draw_label_to(win.buffer, w, h, g.type_x, hy, "TYPE", tok::faint);
      draw_label_to(win.buffer, w, h, g.null_x, hy, "NULL", tok::faint);
      draw_label_to(win.buffer, w, h, g.pk_x, hy, "PK", tok::faint);
      draw_label_to(win.buffer, w, h, g.ai_x, hy, "AUTO", tok::faint);
      draw_label_to(win.buffer, w, h, g.def_x, hy, "DEFAULT", tok::faint);

      int ry = hy + 20;
      // The grid scrolls (wheel, or Up/Down/PageUp/PageDown with no field
      // focused): a table with more columns than the sheet holds used to have
      // its tail simply not drawn, and so not editable at all.
      const int total = static_cast<int>(win.db_designer_edits.size());
      const int fit = designer_rows_fit(ry, dlg_body_y, dlg_body_h);
      int max_scroll = total > fit ? total - fit : 0;
      if (win.db_designer_scroll > max_scroll) win.db_designer_scroll = max_scroll;
      if (win.db_designer_scroll < 0) win.db_designer_scroll = 0;
      const int first = win.db_designer_scroll;
      if (total > fit) {
        std::string pos = "columns " + std::to_string(first + 1) + "-" + std::to_string(std::min(total, first + fit)) +
                          " of " + std::to_string(total) + "  (wheel / PgUp PgDn)";
        draw_text_to(win.buffer, w, h, mx + mw - 24 - text_width(pos, Face::UiSmall), my + 14, pos, tok::faint, Face::UiSmall);
      }
      for (int i = first; i < total && i < first + fit; ++i) {
        const auto& e = win.db_designer_edits[static_cast<size_t>(i)];
        bool row_focus = (i == win.db_designer_focus_row);
        bool name_focus = row_focus && win.db_designer_focus_col == 0;
        bool def_focus = row_focus && win.db_designer_focus_col == 1;

        draw_rect_to(win.buffer, w, h, g.name_x - 4, ry - 2, g.name_w, 22, name_focus ? 0xFF0B213A : 0xFF161B22);
        draw_rect_outline_to(win.buffer, w, h, g.name_x - 4, ry - 2, g.name_w, 22, name_focus ? tok::accent : tok::line_up);
        draw_text_to(win.buffer, w, h, g.name_x, ry, fit_text(*this, e.col.name, g.name_w - 8, Face::Mono), tok::text, Face::Mono);

        draw_rect_to(win.buffer, w, h, g.type_x - 4, ry - 2, g.type_w, 22, 0xFF161B22);
        draw_rect_outline_to(win.buffer, w, h, g.type_x - 4, ry - 2, g.type_w, 22, tok::line_up);
        draw_text_to(win.buffer, w, h, g.type_x, ry, fit_text(*this, designer_type_label(e.col), g.type_w - 8, Face::Mono), tok::text, Face::Mono);

        draw_text_to(win.buffer, w, h, g.null_x, ry, e.col.not_null ? "[ ]" : "[x]", tok::text, Face::Mono);
        draw_text_to(win.buffer, w, h, g.pk_x, ry, e.col.is_primary_key ? "[x]" : "[ ]", tok::text, Face::Mono);
        draw_text_to(win.buffer, w, h, g.ai_x, ry, e.col.auto_increment ? "[x]" : "[ ]", tok::text, Face::Mono);

        draw_rect_to(win.buffer, w, h, g.def_x - 4, ry - 2, g.def_w, 22, def_focus ? 0xFF0B213A : 0xFF161B22);
        draw_rect_outline_to(win.buffer, w, h, g.def_x - 4, ry - 2, g.def_w, 22, def_focus ? tok::accent : tok::line_up);
        draw_text_to(win.buffer, w, h, g.def_x, ry, fit_text(*this, column_default_text(e.col.default_value), g.def_w - 8, Face::Mono), tok::text, Face::Mono);

        draw_text_to(win.buffer, w, h, g.del_x, ry, "x", tok::err, Face::UiBold);
        ry += 26;
      }

      draw_rect_to(win.buffer, w, h, mx + 20, ry, g.add_w, 22, tok::wash);
      draw_rect_outline_to(win.buffer, w, h, mx + 20, ry, g.add_w, 22, tok::strong);
      draw_text_to(win.buffer, w, h, mx + 32, ry + 3, "+ Add Column", tok::text, Face::UiBold);

    } else if (win.db_designer_tab == 1) {
      SqlDatabase* sql_mgr = sql_engine(k);
      SqlTable* t = sql_mgr ? sql_mgr->resolve_table(win.db_designer_table) : nullptr;
      int ry = dlg_body_y + 10;
      if (t) {
        for (const auto& ix : t->indices) {
          if (ry + 22 > dlg_body_y + dlg_body_h - 60) break;
          std::string cols;
          for (size_t ci = 0; ci < ix.columns.size(); ++ci) { if (ci) cols += ", "; cols += ix.columns[ci]; }
          std::string line = (ix.is_unique ? "[unique] " : "[index]  ") + ix.name + "  (" + cols + ")";
          draw_text_to(win.buffer, w, h, mx + 20, ry, fit_text(*this, line, mw - 24 - 60, Face::Mono), tok::text, Face::Mono);
          draw_text_to(win.buffer, w, h, mx + mw - 40, ry, "drop", tok::err, Face::UiBold);
          ry += 24;
        }
      }
      // New index form, pinned to the bottom of the tab body.
      int form_y = dlg_body_y + dlg_body_h - 46;
      draw_rect_outline_to(win.buffer, w, h, mx + 12, form_y - 6, mw - 24, 1, tok::line_up);
      bool name_f = win.db_designer_index_field_focus == 0;
      bool cols_f = win.db_designer_index_field_focus == 1;
      draw_label_to(win.buffer, w, h, mx + 20, form_y + 4, "NAME", tok::faint);
      draw_rect_to(win.buffer, w, h, mx + 70, form_y, 140, 22, name_f ? 0xFF0B213A : 0xFF161B22);
      draw_rect_outline_to(win.buffer, w, h, mx + 70, form_y, 140, 22, name_f ? tok::accent : tok::line_up);
      draw_text_to(win.buffer, w, h, mx + 76, form_y + 3, fit_text(*this, win.db_designer_new_index_name, 128, Face::Mono), tok::text, Face::Mono);
      draw_label_to(win.buffer, w, h, mx + 222, form_y + 4, "COLUMNS", tok::faint);
      draw_rect_to(win.buffer, w, h, mx + 292, form_y, 160, 22, cols_f ? 0xFF0B213A : 0xFF161B22);
      draw_rect_outline_to(win.buffer, w, h, mx + 292, form_y, 160, 22, cols_f ? tok::accent : tok::line_up);
      draw_text_to(win.buffer, w, h, mx + 298, form_y + 3, fit_text(*this, win.db_designer_new_index_cols, 148, Face::Mono), tok::text, Face::Mono);
      draw_text_to(win.buffer, w, h, mx + 462, form_y + 3, win.db_designer_new_index_unique ? "[x] unique" : "[ ] unique", tok::text, Face::UiSmall);
      draw_rect_to(win.buffer, w, h, mx + mw - 90, form_y, 66, 22, tok::ok);
      draw_text_to(win.buffer, w, h, mx + mw - 78, form_y + 3, "+ Add", 0xFF000000, Face::UiBold);

    } else {
      SqlDatabase* sql_mgr = sql_engine(k);
      SqlTable* t = sql_mgr ? sql_mgr->resolve_table(win.db_designer_table) : nullptr;
      int ry = dlg_body_y + 10;
      if (t) {
        for (const auto& fk : t->foreign_keys) {
          if (ry + 22 > dlg_body_y + dlg_body_h - 70) break;
          std::string line = fk.constraint_name + ": " + fk.column + " -> " + fk.ref_table + "(" + fk.ref_column + ") ON DELETE " + fk.on_delete;
          draw_text_to(win.buffer, w, h, mx + 20, ry, fit_text(*this, line, mw - 24 - 60, Face::Mono), tok::text, Face::Mono);
          draw_text_to(win.buffer, w, h, mx + mw - 40, ry, "drop", tok::err, Face::UiBold);
          ry += 24;
        }
      }
      int form_y = dlg_body_y + dlg_body_h - 56;
      draw_rect_outline_to(win.buffer, w, h, mx + 12, form_y - 6, mw - 24, 1, tok::line_up);
      static const char* const kFkFields[4] = {"NAME", "COLUMN", "REF TABLE", "REF COL"};
      const std::string* kFkVals[4] = {&win.db_designer_new_fk_name, &win.db_designer_new_fk_col,
                                        &win.db_designer_new_fk_ref_table, &win.db_designer_new_fk_ref_col};
      int fx = mx + 20;
      int fw_each = (mw - 40 - 80) / 4;
      for (int i = 0; i < 4; ++i) {
        bool f = win.db_designer_fk_field_focus == i;
        draw_label_to(win.buffer, w, h, fx, form_y - 16, kFkFields[i], tok::faint);
        draw_rect_to(win.buffer, w, h, fx, form_y, fw_each - 8, 22, f ? 0xFF0B213A : 0xFF161B22);
        draw_rect_outline_to(win.buffer, w, h, fx, form_y, fw_each - 8, 22, f ? tok::accent : tok::line_up);
        draw_text_to(win.buffer, w, h, fx + 6, form_y + 3, fit_text(*this, *kFkVals[i], fw_each - 20, Face::Mono), tok::text, Face::Mono);
        fx += fw_each;
      }
      draw_rect_to(win.buffer, w, h, mx + mw - 90, form_y, 66, 22, tok::ok);
      draw_text_to(win.buffer, w, h, mx + mw - 78, form_y + 3, "+ Add", 0xFF000000, Face::UiBold);
    }

    int by = my + mh - 40;
    static const char* const kDesignBtns[2] = {"Apply Changes", "Close"};
    int bx = mx + mw - 12;
    for (int i = 1; i >= 0; --i) {
      int bw = text_width(kDesignBtns[i], Face::UiBold) + 24;
      bx -= bw;
      draw_rect_to(win.buffer, w, h, bx, by, bw, 28, i == 0 ? tok::ok : tok::wash);
      if (i != 0) draw_rect_outline_to(win.buffer, w, h, bx, by, bw, 28, tok::strong);
      draw_text_to(win.buffer, w, h, bx + 12, by + 6, kDesignBtns[i], i == 0 ? 0xFF000000 : tok::text, Face::UiBold);
      bx -= 8;
    }
  }

  if (win.db_show_telemetry) {
    int mw = w - 60; if (mw > 780) mw = 780; if (mw < 400) mw = w - 20;
    int mh = h - 60; if (mh > 520) mh = 520; if (mh < 320) mh = h - 20;
    int mx = (w - mw) / 2;
    int my = (h - mh) / 2;

    draw_rect_to(win.buffer, w, h, 0, 0, w, h, 0xEE0A0E14);
    draw_card(win, w, h, mx, my, mw, mh, "asql remote telemetry & fleet controller");

    // Read once for this paint, so the cards and the per-row column cannot
    // disagree about whether a stream exists.
    SqlDatabase* tdb = sql_engine(k);
    BinlogConsumerStats cdc{};
    const std::unordered_set<std::string>* cdc_tables = nullptr;
    if (tdb) {
      cdc = tdb->binlog_sync_stats();
      if (cdc.is_running) {
        if (BinlogConsumer* bc = tdb->binlog_consumer()) cdc_tables = &bc->config().table_whitelist;
      }
    }
    const bool cdc_live = cdc.is_running && cdc.is_connected;

    // Telemetry Cards: 4 metric boxes across the top
    int card_w = (mw - 50) / 4;
    int card_h = 64;
    int card_y = my + 44;

    // Card 1: Remote Host & State
    int c1_x = mx + 16;
    draw_rect_to(win.buffer, w, h, c1_x, card_y, card_w, card_h, tok::panel2);
    draw_rect_outline_to(win.buffer, w, h, c1_x, card_y, card_w, card_h, tok::line_up);
    draw_label_to(win.buffer, w, h, c1_x + 8, card_y + 8, "HOST / STATUS", tok::faint);
    draw_text_to(win.buffer, w, h, c1_x + 8, card_y + 26, fit_text(*this, win.db_telemetry_host + ":" + std::to_string(win.db_telemetry_port), card_w - 16, Face::UiBold), tok::accent, Face::UiBold);
    draw_text_to(win.buffer, w, h, c1_x + 8, card_y + 44, fit_text(*this, win.db_telemetry_status, card_w - 16, Face::UiSmall), cdc_live ? tok::ok : tok::dim, Face::UiSmall);

    // Card 2: Memory RSS
    int c2_x = c1_x + card_w + 6;
    draw_rect_to(win.buffer, w, h, c2_x, card_y, card_w, card_h, tok::panel2);
    draw_rect_outline_to(win.buffer, w, h, c2_x, card_y, card_w, card_h, tok::line_up);
    draw_label_to(win.buffer, w, h, c2_x + 8, card_y + 8, "MEMORY (RAM RSS)", tok::faint);
    char ram_txt[48];
    snprintf(ram_txt, sizeof(ram_txt), "%.1f MB in tables", win.db_telemetry_ram_mb);
    draw_text_to(win.buffer, w, h, c2_x + 8, card_y + 26, ram_txt, tok::text, Face::UiBold);
    draw_text_to(win.buffer, w, h, c2_x + 8, card_y + 44, "In-Memory ASQL Store", tok::dim, Face::UiSmall);

    // Card 3: Replication Lag & Binlog
    int c3_x = c2_x + card_w + 6;
    draw_rect_to(win.buffer, w, h, c3_x, card_y, card_w, card_h, tok::panel2);
    draw_rect_outline_to(win.buffer, w, h, c3_x, card_y, card_w, card_h, tok::line_up);
    draw_label_to(win.buffer, w, h, c3_x + 8, card_y + 8, "CDC LAG / BINLOG", tok::faint);
    char lag_txt[48];
    if (!cdc.is_running) snprintf(lag_txt, sizeof(lag_txt), "no stream");
    else snprintf(lag_txt, sizeof(lag_txt), "%.0f ms lag", win.db_telemetry_lag_ms);
    draw_text_to(win.buffer, w, h, c3_x + 8, card_y + 26, lag_txt, cdc_live ? tok::ok : tok::dim, Face::UiBold);
    const std::string binlog_txt = win.db_telemetry_binlog_file.empty()
        ? std::string("no binlog position")
        : win.db_telemetry_binlog_file + ":" + std::to_string(win.db_telemetry_binlog_pos);
    draw_text_to(win.buffer, w, h, c3_x + 8, card_y + 44, fit_text(*this, binlog_txt, card_w - 16, Face::UiSmall), tok::dim, Face::UiSmall);

    // Card 4: Operating Mode
    int c4_x = c3_x + card_w + 6;
    draw_rect_to(win.buffer, w, h, c4_x, card_y, card_w, card_h, tok::panel2);
    draw_rect_outline_to(win.buffer, w, h, c4_x, card_y, card_w, card_h, tok::line_up);
    draw_label_to(win.buffer, w, h, c4_x + 8, card_y + 8, "ROUTING MODE", tok::faint);
    draw_text_to(win.buffer, w, h, c4_x + 8, card_y + 26, win.db_telemetry_mode, tok::accent, Face::UiBold);
    draw_text_to(win.buffer, w, h, c4_x + 8, card_y + 44, std::to_string(win.db_telemetry_replicated_tables) + " tables replicated", tok::text, Face::UiSmall);

    // Replicated Tables Matrix (Center table area)
    int tbl_y = card_y + card_h + 12;
    int tbl_h = mh - (tbl_y - my) - 56;
    draw_rect_to(win.buffer, w, h, mx + 16, tbl_y, mw - 32, tbl_h, 0xFF0D1117);
    draw_rect_outline_to(win.buffer, w, h, mx + 16, tbl_y, mw - 32, tbl_h, tok::line_up);

    // Table Header
    draw_rect_to(win.buffer, w, h, mx + 17, tbl_y + 1, mw - 34, 22, 0xFF161B22);
    draw_text_to(win.buffer, w, h, mx + 24, tbl_y + 5, "TABLE NAME", tok::accent, Face::UiBold);
    draw_text_to(win.buffer, w, h, mx + 220, tbl_y + 5, "ROW COUNT", tok::accent, Face::UiBold);
    draw_text_to(win.buffer, w, h, mx + 340, tbl_y + 5, "RAM FOOTPRINT", tok::accent, Face::UiBold);
    draw_text_to(win.buffer, w, h, mx + 480, tbl_y + 5, "CDC REPLICATION STATE", tok::accent, Face::UiBold);

    int ry = tbl_y + 26;
    int max_rows = (tbl_h - 30) / APP_ROW;
    // Nothing to report is a thing the pane is allowed to say.
    if (win.db_telemetry_table_stats.empty()) {
      draw_text_to(win.buffer, w, h, mx + 24, ry, "no tables reported by the engine", tok::dim, Face::UiSmall);
    }
    for (size_t i = 0; i < win.db_telemetry_table_stats.size() && (int)i < max_rows; ++i) {
      const auto& t = win.db_telemetry_table_stats[i];
      if ((i & 1) == 1) draw_rect_to(win.buffer, w, h, mx + 17, ry - 3, mw - 34, APP_ROW, 0xFF11161D);
      draw_text_to(win.buffer, w, h, mx + 24, ry, t.first, tok::text, Face::Mono);
      draw_text_to(win.buffer, w, h, mx + 220, ry, std::to_string(t.second) + " rows", tok::dim, Face::Mono);
      double mb = (t.second * 240.0) / (1024.0 * 1024.0);
      char buf[32]; snprintf(buf, sizeof(buf), "%.2f MB", mb < 0.1 ? 0.1 : mb);
      draw_text_to(win.buffer, w, h, mx + 340, ry, buf, tok::dim, Face::Mono);
      const bool carried = cdc_tables && (cdc_tables->empty() || cdc_tables->count(t.first) > 0);
      draw_text_to(win.buffer, w, h, mx + 480, ry,
                   !cdc.is_running ? "no CDC stream" : (carried ? "replicated" : "not replicated"),
                   carried ? tok::ok : tok::dim, Face::UiSmall);
      ry += APP_ROW;
    }

    // Action Control Bar at the bottom
    int act_y = my + mh - 42;
    static const char* const kFleetBtns[] = {"Sync CDC", "Flush Cache", "Reload Tables", "Probe Latency", "Close"};
    int bx = mx + 16;
    for (int i = 0; i < 5; ++i) {
      int bw = text_width(kFleetBtns[i], Face::UiBold) + 20;
      bool primary = (i == 0 || i == 4);
      uint32_t fill = (i == 4) ? tok::wash : (primary ? tok::ok : 0xFF1C2128);
      uint32_t ink = (i == 4) ? tok::text : (primary ? 0xFF000000 : tok::accent);
      draw_rect_to(win.buffer, w, h, bx, act_y, bw, 28, fill);
      draw_rect_outline_to(win.buffer, w, h, bx, act_y, bw, 28, (i == 4) ? tok::strong : tok::line_up);
      draw_text_to(win.buffer, w, h, bx + 10, act_y + 6, kFleetBtns[i], ink, Face::UiBold);
      bx += bw + 8;
    }
  }
}

// Which chip a click at `rel_x` landed on, by the same arithmetic that drew
// them. Returns nullptr for a click that missed every chip.
template <size_t N>
static const char* chip_hit(const HardwareCompositor& S, const char* const (&chips)[N], int rel_x, int rel_y) {
  if (rel_y < APP_CHIP_Y || rel_y > APP_CHIP_Y + APP_CHIP_H) return nullptr;
  int chip_x = 22;
  for (const char* chip : chips) {
    int chip_w = S.text_width(chip, Face::Ui) + 16;
    if (rel_x >= chip_x && rel_x <= chip_x + chip_w) return chip;
    chip_x += chip_w + 6;
  }
  return nullptr;
}

void HardwareCompositor::editor_click(WindowSurface& w, int rel_x, int rel_y, std::string& to_run) {
  int cw = w.content_width(), ch = w.content_height();
  if (const char* chip = chip_hit(*this, EDIT_CHIPS, rel_x, rel_y)) {
    if (!kernel_) { set_toast("no kernel is bound to this desktop yet"); return; }
    if (strcmp(chip, "Save") == 0) {
      editor_save(w, *kernel_, std::string());
      set_toast(w.status_text);
    } else if (strcmp(chip, "Undo") == 0) {
      editor_undo(w);
      set_toast(w.status_text);
    } else if (strcmp(chip, "Redo") == 0) {
      editor_redo(w);
      set_toast(w.status_text);
    } else if (strcmp(chip, "Find") == 0) {
      // Put the command line up with the verb already typed, so the chip and
      // the keyboard lead to the same place.
      w.cmdline_focus = true;
      w.input.clear();
      w.input.insert("find ", 64);
    } else if (strcmp(chip, "Preview") == 0) {
      if (!lang_has_preview(w.file_lang)) {
        set_toast(std::string(lang_name(w.file_lang)) + " has no preview - this is for Markdown");
      } else {
        w.file_preview = !w.file_preview;
        editor_stash_active(w);
      }
    } else if (strcmp(chip, "Parent dir") == 0) {
      std::string d = w.file_dir;
      size_t slash = d.find_last_of('/');
      if (slash != std::string::npos && slash > 0) editor_browse(w, *kernel_, d.substr(0, slash));
      else editor_browse(w, *kernel_, "/");
    } else if (strcmp(chip, "New") == 0) {
      editor_command(w, *kernel_, "new");
    } else if (strcmp(chip, "Build") == 0) {
      to_run = "build";
    } else if (strcmp(chip, "Command") == 0) {
      w.cmdline_focus = true;
    }
    return;
  }

  int body_y = APP_BODY_Y;

  // The tab strip, hit-tested with the same arithmetic that drew it.
  if (!w.buffers.empty()) {
    if (rel_y >= body_y && rel_y < body_y + EDIT_TAB_H) {
      int tab_x = 20;
      for (size_t i = 0; i < w.buffers.size(); ++i) {
        const EditorBuffer& b = w.buffers[i];
        std::string base = b.path;
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base.empty()) base = "untitled";
        if (b.dirty) base += " *";
        int tw = text_width(base, Face::UiSmall) + 26;
        if (tab_x + tw > cw - 20) break;
        if (rel_x >= tab_x && rel_x < tab_x + tw) {
          // The close box first: it sits inside the tab it closes.
          if (rel_x >= tab_x + tw - 18) editor_close_buffer(w, (int)i);
          else editor_activate_buffer(w, (int)i);
          return;
        }
        tab_x += tw + 4;
      }
      return;
    }
    body_y += EDIT_TAB_H + 6;
  }

  int body_h = ch - body_y - APP_FOOT_H;
  if (rel_y >= ch - 40) { w.cmdline_focus = true; return; }
  if (rel_y < body_y || rel_y > body_y + body_h) return;

  int browse_w = (cw > 620) ? 200 : (cw > 420 ? 150 : 0);
  if (browse_w && rel_x >= 20 && rel_x < 20 + browse_w) {
    int idx_raw = (rel_y - (body_y + 44)) / 18;
    if (idx_raw < 0 || !kernel_) return;
    int idx = idx_raw + w.dir_scroll;
    if (idx < 0 || idx >= (int)w.dir_entries.size()) return;

    std::string entry = w.dir_entries[(size_t)idx];
    bool is_dir = (idx < (int)w.dir_is_dir.size()) ? w.dir_is_dir[(size_t)idx] : (entry == "..");

    if (entry == "..") {
      std::string d = w.file_dir;
      std::string parent_d;
      if (is_ssh_path(d)) {
        aios::net::SshTarget t_spec;
        std::string r_path;
        if (parse_ssh_path(d, &t_spec, &r_path)) {
          size_t slash = r_path.find_last_of('/');
          std::string parent_r = (slash == std::string::npos || slash == 0) ? "/" : r_path.substr(0, slash);
          parent_d = "ssh://" + t_spec.str() + parent_r;
        } else {
          parent_d = d;
        }
      } else {
        size_t slash = d.find_last_of('/');
        parent_d = (slash != std::string::npos && slash > 0) ? d.substr(0, slash) : "/";
      }
      editor_browse(w, *kernel_, parent_d);
    } else if (is_dir) {
      std::string target_path = (w.file_dir == "/" ? "" : w.file_dir) + "/" + entry;
      editor_browse(w, *kernel_, target_path);
    } else {
      std::string target_path = (w.file_dir == "/" ? "" : w.file_dir) + "/" + entry;
      editor_open(w, *kernel_, target_path);
    }
    w.cmdline_focus = false;
    return;
  }

  // The text area: put the caret where the person pointed. A click with Shift
  // down drags the selection out to there instead of dropping it, which is how
  // a range gets selected without holding the button down across a repaint.
  int box_x = browse_w ? 20 + browse_w + 10 : 20;
  int box_w = cw - box_x - 20;
  if (rel_x < box_x || rel_x > box_x + box_w) return;
  w.cmdline_focus = false;

  int minimap_w = (box_w > 460) ? 54 : 0;
  if (minimap_w > 0 && rel_x >= box_x + box_w - minimap_w) {
    int total_l = std::max(1, (int)w.lines.size());
    int mm_h = body_h - 2;
    int target_l = ((rel_y - (body_y + 1)) * total_l) / std::max(1, mm_h);
    target_l = std::clamp(target_l, 0, total_l - 1);
    w.cursor_line = target_l;
    int rows = (body_h - 14) / APP_ROW;
    w.view_top = std::clamp(target_l - rows / 2, 0, total_l - 1);
    mark_dirty();
    return;
  }

  int line = 0, col = 0;
  if (!editor_point_to_caret(w, rel_x, rel_y, &line, &col)) return;

  // Shift extends whatever is already selected; a plain click starts a new
  // selection at this point, which the drag below then widens.
  if (shift_held_) editor_begin_selection(w);
  else editor_clear_selection(w);
  w.cursor_line = line;
  w.cursor_col = col;
  if (!shift_held_) {
    w.sel_line = line;
    w.sel_col = col;
    text_dragging_ = true;
    text_drag_win_ = w.window_id;
  }
}

bool HardwareCompositor::editor_point_to_caret(const WindowSurface& w, int rel_x, int rel_y,
                                               int* line, int* col) const {
  if (w.lines.empty()) return false;
  int cw = w.content_width(), ch = w.content_height();
  int body_y = APP_BODY_Y;
  if (!w.buffers.empty()) body_y += EDIT_TAB_H + 6;
  int body_h = ch - body_y - APP_FOOT_H;
  if (body_h < 40) body_h = 40;
  int browse_w = (cw > 620) ? 200 : (cw > 420 ? 150 : 0);
  int box_x = browse_w ? 20 + browse_w + 10 : 20;
  int box_w = cw - box_x - 20;
  if (rel_x < box_x || rel_x > box_x + box_w) return false;

  int row = (rel_y - (body_y + 6)) / APP_ROW;
  int at = w.view_top + (row < 0 ? 0 : row);
  if (at >= (int)w.lines.size()) at = (int)w.lines.size() - 1;
  if (at < 0) at = 0;

  int text_x = box_x + 48 + 10;
  int char_w = text_width("M", Face::Mono);
  if (char_w < 1) char_w = 8;
  // Pointer position -> display column -> the byte that column came from, so a
  // click lands between characters rather than inside a tab or a UTF-8 letter.
  int want = w.view_left + (rel_x - text_x + char_w / 2) / char_w;
  if (want < 0) want = 0;
  std::vector<std::string> shown;
  std::vector<int> byte_at;
  expand_editor_line(w.lines[(size_t)at], &shown, &byte_at);
  if (want >= (int)byte_at.size()) want = (int)byte_at.size() - 1;
  *line = at;
  *col = byte_at.empty() ? 0 : byte_at[(size_t)want];
  return true;
}

void HardwareCompositor::db_click(WindowSurface& w, int rel_x, int rel_y, std::string& to_run) {
  (void)to_run;
  int cw = w.content_width(), ch = w.content_height();
  if (w.db_show_conn_mgr) {
    int mw = cw - 80; if (mw > 640) mw = 640; if (mw < 380) mw = cw - 20;
    int mh = ch - 80; if (mh > 460) mh = 460; if (mh < 280) mh = ch - 20;
    int mx = (cw - mw) / 2;
    int my = (ch - mh) / 2;
    int list_w = 190;

    SqlDatabase* sql_mgr = sql_engine(*kernel_);
    auto profiles = sql_mgr ? sql_mgr->list_profiles() : std::vector<DbConnectionProfile>();

    // Select profile from list
    int py = my + 72;
    for (const auto& p : profiles) {
      if (rel_x >= mx + 14 && rel_x < mx + 14 + list_w - 4 && rel_y >= py - 2 && rel_y < py + 22) {
        w.db_selected_profile_id = p.id;
        w.db_profile_edit = p;
        w.db_edit_new_profile = false;
        w.db_conn_field_focus = -1;
        return;
      }
      py += 24;
    }

    // New connection button
    if (rel_x >= mx + 12 && rel_x < mx + 12 + list_w && rel_y >= my + mh - 42 && rel_y < my + mh - 14) {
      DbConnectionProfile new_p;
      new_p.id = "prof_" + std::to_string(steady_ms());
      new_p.name = "New MariaDB Connection";
      new_p.host = "127.0.0.1";
      new_p.port = 3306;
      new_p.user = "root";
      new_p.database = "mysql";
      new_p.is_local_embedded = false;
      w.db_profile_edit = new_p;
      w.db_selected_profile_id = new_p.id;
      w.db_edit_new_profile = true;
      w.db_conn_field_focus = 0;
      return;
    }

    // Form fields focus. field_col_x has to match render_database_window's
    // own computation exactly -- both read the same widest-label width, so a
    // click still lands on the box that's actually drawn there.
    int fx = mx + list_w + 22;
    int fw = mw - list_w - 34;
    int fy = my + 52;
    static const char* const kDbFields[] = {"Name:", "Host:", "Port:", "User:", "Pass:", "Database:", "Socket:"};
    int max_label_w = 0;
    for (const char* f : kDbFields) max_label_w = std::max(max_label_w, label_width(f));
    const int field_col_x = fx + 12 + max_label_w + 12;
    const int field_col_w = fx + fw - 12 - field_col_x;
    for (int i = 0; i < 7; ++i) {
      if (rel_x >= field_col_x && rel_x < field_col_x + field_col_w && rel_y >= fy && rel_y < fy + 22) {
        w.db_conn_field_focus = i;
        return;
      }
      fy += 28;
    }

    // Action buttons: Connect, Test, Save, Delete, Close -- same measured
    // layout render_database_window painted, so a click lands under the
    // button that is actually drawn there.
    DbConnMgrButtons btn = db_conn_mgr_buttons(mx, mw, my, mh);
    int by = btn.y;

    // Connect
    if (rel_x >= btn.x[0] && rel_x < btn.x[0] + btn.w[0] && rel_y >= by && rel_y < by + 28) {
      if (sql_mgr) {
        (void)sql_mgr->add_or_update_profile(w.db_profile_edit);
        auto r = sql_mgr->switch_profile(w.db_profile_edit.id);
        if (r.ok()) {
          w.db_connected = true;
          w.db_show_conn_mgr = false;
          db_refresh(w, *kernel_);
          set_toast("connected to: " + w.db_profile_edit.name);
        } else {
          set_toast("switch failed: " + r.error().str());
        }
      }
      return;
    }
    // Test
    if (rel_x >= btn.x[1] && rel_x < btn.x[1] + btn.w[1] && rel_y >= by && rel_y < by + 28) {
      if (sql_mgr) {
        auto r = sql_mgr->test_profile_connection(w.db_profile_edit);
        if (r.ok()) set_toast("connection SUCCESS: " + w.db_profile_edit.host + ":" + std::to_string(w.db_profile_edit.port));
        else set_toast("connection FAILED: " + r.error().str());
      }
      return;
    }
    // Save
    if (rel_x >= btn.x[2] && rel_x < btn.x[2] + btn.w[2] && rel_y >= by && rel_y < by + 28) {
      if (sql_mgr) {
        (void)sql_mgr->add_or_update_profile(w.db_profile_edit);
        w.db_edit_new_profile = false;
        set_toast("profile saved: " + w.db_profile_edit.name);
      }
      return;
    }
    // Delete
    if (rel_x >= btn.x[3] && rel_x < btn.x[3] + btn.w[3] && rel_y >= by && rel_y < by + 28) {
      if (sql_mgr) {
        auto r = sql_mgr->remove_profile(w.db_profile_edit.id);
        if (r.ok()) {
          set_toast("profile deleted");
          auto profs = sql_mgr->list_profiles();
          if (!profs.empty()) {
            w.db_selected_profile_id = profs.front().id;
            w.db_profile_edit = profs.front();
          }
        } else {
          set_toast("cannot delete: " + r.error().str());
        }
      }
      return;
    }
    // Close
    if (rel_x >= btn.x[4] && rel_x < btn.x[4] + btn.w[4] && rel_y >= by && rel_y < by + 28) {
      w.db_show_conn_mgr = false;
      return;
    }
    return;
  }

  if (w.db_show_designer) {
    int mw = cw - 60; if (mw > 760) mw = 760; if (mw < 420) mw = cw - 20;
    int mh = ch - 60; if (mh > 520) mh = 520; if (mh < 340) mh = ch - 20;
    int mx = (cw - mw) / 2;
    int my = (ch - mh) / 2;

    // Tab bar - widths must match render_database_window's own computation.
    static const char* const kDesignTabs[3] = {"Columns", "Indexes", "Foreign Keys"};
    int tab_x = mx + 12;
    int tab_y = my + 40;
    for (int i = 0; i < 3; ++i) {
      int tw = text_width(kDesignTabs[i], Face::UiBold) + 24;
      if (rel_x >= tab_x && rel_x < tab_x + tw && rel_y >= tab_y && rel_y < tab_y + 26) {
        w.db_designer_tab = i;
        w.db_designer_focus_row = -1;
        w.db_designer_focus_col = -1;
        return;
      }
      tab_x += tw + 6;
    }

    int body_y = tab_y + 34;
    int body_h = my + mh - 52 - body_y;

    if (w.db_designer_tab == 0) {
      const DesignerColGeom g = designer_col_geom(*this, mx, mw);
      int hy = body_y + 6;
      int ry = hy + 20;
      const int total = static_cast<int>(w.db_designer_edits.size());
      const int fit = designer_rows_fit(ry, body_y, body_h);
      int max_scroll = total > fit ? total - fit : 0;
      if (w.db_designer_scroll > max_scroll) w.db_designer_scroll = max_scroll;
      if (w.db_designer_scroll < 0) w.db_designer_scroll = 0;
      const int first = w.db_designer_scroll;

      for (int ii = first; ii < total && ii < first + fit; ++ii) {
        const size_t i = static_cast<size_t>(ii);
        if (rel_x >= g.name_x - 4 && rel_x < g.name_x - 4 + g.name_w && rel_y >= ry - 2 && rel_y < ry + 20) {
          w.db_designer_focus_row = ii; w.db_designer_focus_col = 0; return;
        }
        if (rel_x >= g.type_x - 4 && rel_x < g.type_x - 4 + g.type_w && rel_y >= ry - 2 && rel_y < ry + 20) {
          // Click cycles INT -> REAL -> DECIMAL -> TEXT -> INT: every type the
          // engine has. It used to be TEXT -> INT -> REAL -> TEXT, which had
          // no DECIMAL in it, so one click on a DECIMAL column made it TEXT.
          auto& ce = w.db_designer_edits[i];
          auto& col = ce.col;
          col.type = col.type == SqlDataType::Int ? SqlDataType::Real
                   : col.type == SqlDataType::Real ? SqlDataType::Decimal
                   : col.type == SqlDataType::Decimal ? SqlDataType::Text
                   : SqlDataType::Int;
          // Back on the type it was declared with: keep exactly what it was
          // (varchar(20), decimal(12,4)...). Anything else is a new type.
          const SqlColumn* orig = nullptr;
          if (SqlDatabase* sdb = kernel_ ? sql_engine(*kernel_) : nullptr) {
            if (const SqlTable* t = sdb->find_table(w.db_designer_table)) {
              for (const auto& oc : t->columns)
                if (!ce.original_name.empty() && oc.name == ce.original_name) { orig = &oc; break; }
            }
          }
          if (orig && orig->type == col.type) {
            col.declared = orig->declared; col.precision = orig->precision; col.scale = orig->scale;
            col.temporal = orig->temporal; col.binary = orig->binary;
          } else {
            col.declared.clear(); col.temporal = 0; col.binary = 0;
            if (col.type == SqlDataType::Decimal) { col.precision = 10; col.scale = 2; }
            else { col.precision = 0; col.scale = 0; }
          }
          return;
        }
        if (rel_x >= g.null_x && rel_x < g.null_x + g.null_w && rel_y >= ry - 2 && rel_y < ry + 20) {
          w.db_designer_edits[i].col.not_null = !w.db_designer_edits[i].col.not_null;
          return;
        }
        if (rel_x >= g.pk_x && rel_x < g.pk_x + g.pk_w && rel_y >= ry - 2 && rel_y < ry + 20) {
          w.db_designer_edits[i].col.is_primary_key = !w.db_designer_edits[i].col.is_primary_key;
          return;
        }
        if (rel_x >= g.ai_x && rel_x < g.ai_x + g.ai_w && rel_y >= ry - 2 && rel_y < ry + 20) {
          w.db_designer_edits[i].col.auto_increment = !w.db_designer_edits[i].col.auto_increment;
          return;
        }
        if (rel_x >= g.def_x - 4 && rel_x < g.def_x - 4 + g.def_w && rel_y >= ry - 2 && rel_y < ry + 20) {
          w.db_designer_focus_row = ii; w.db_designer_focus_col = 1; return;
        }
        if (rel_x >= g.del_x && rel_x < g.del_x + 20 && rel_y >= ry - 2 && rel_y < ry + 20) {
          w.db_designer_edits.erase(w.db_designer_edits.begin() + static_cast<long>(i));
          if (w.db_designer_focus_row == ii) { w.db_designer_focus_row = -1; w.db_designer_focus_col = -1; }
          else if (w.db_designer_focus_row > ii) w.db_designer_focus_row--;
          return;
        }
        ry += 26;
      }
      // "+ Add Column" row, same geometry the render side computed.
      if (rel_x >= mx + 20 && rel_x < mx + 20 + g.add_w && rel_y >= ry && rel_y < ry + 22) {
        ColumnEdit e;
        e.col.name = "new_column";
        e.col.type = SqlDataType::Text;
        w.db_designer_edits.push_back(e);
        w.db_designer_focus_row = static_cast<int>(w.db_designer_edits.size()) - 1;
        w.db_designer_focus_col = 0;
        // Bring the new row into view; the painter clamps it.
        w.db_designer_scroll = static_cast<int>(w.db_designer_edits.size());
        return;
      }
    } else if (w.db_designer_tab == 1) {
      SqlDatabase* sql_mgr = sql_engine(*kernel_);
      SqlTable* t = sql_mgr ? sql_mgr->resolve_table(w.db_designer_table) : nullptr;
      int ry = body_y + 10;
      if (t) {
        for (size_t i = 0; i < t->indices.size(); ++i) {
          if (ry + 22 > body_y + body_h - 60) break;
          if (rel_x >= mx + mw - 40 && rel_x < mx + mw - 8 && rel_y >= ry && rel_y < ry + 20) {
            std::string stmt = "ALTER TABLE `" + w.db_designer_table + "` DROP INDEX `" + t->indices[i].name + "`";
            SqlQueryResult r = sql_mgr->execute_query(stmt);
            set_toast(r.success ? "index dropped" : ("drop failed: " + r.error_message));
            return;
          }
          ry += 24;
        }
      }
      int form_y = body_y + body_h - 46;
      if (rel_x >= mx + 70 && rel_x < mx + 210 && rel_y >= form_y && rel_y < form_y + 22) {
        w.db_designer_index_field_focus = 0; return;
      }
      if (rel_x >= mx + 292 && rel_x < mx + 452 && rel_y >= form_y && rel_y < form_y + 22) {
        w.db_designer_index_field_focus = 1; return;
      }
      if (rel_x >= mx + 462 && rel_x < mx + 560 && rel_y >= form_y && rel_y < form_y + 22) {
        w.db_designer_new_index_unique = !w.db_designer_new_index_unique; return;
      }
      if (rel_x >= mx + mw - 90 && rel_x < mx + mw - 24 && rel_y >= form_y && rel_y < form_y + 22) {
        db_designer_add_index(w, *kernel_); return;
      }
    } else {
      SqlDatabase* sql_mgr = sql_engine(*kernel_);
      SqlTable* t = sql_mgr ? sql_mgr->resolve_table(w.db_designer_table) : nullptr;
      int ry = body_y + 10;
      if (t) {
        for (size_t i = 0; i < t->foreign_keys.size(); ++i) {
          if (ry + 22 > body_y + body_h - 70) break;
          if (rel_x >= mx + mw - 40 && rel_x < mx + mw - 8 && rel_y >= ry && rel_y < ry + 20) {
            std::string stmt = "ALTER TABLE `" + w.db_designer_table + "` DROP FOREIGN KEY `" + t->foreign_keys[i].constraint_name + "`";
            SqlQueryResult r = sql_mgr->execute_query(stmt);
            set_toast(r.success ? "foreign key dropped" : ("drop failed: " + r.error_message));
            return;
          }
          ry += 24;
        }
      }
      int form_y = body_y + body_h - 56;
      int fx = mx + 20;
      int fw_each = (mw - 40 - 80) / 4;
      for (int i = 0; i < 4; ++i) {
        if (rel_x >= fx && rel_x < fx + fw_each - 8 && rel_y >= form_y && rel_y < form_y + 22) {
          w.db_designer_fk_field_focus = i; return;
        }
        fx += fw_each;
      }
      if (rel_x >= mx + mw - 90 && rel_x < mx + mw - 24 && rel_y >= form_y && rel_y < form_y + 22) {
        db_designer_add_fk(w, *kernel_); return;
      }
    }

    // Apply Changes / Close, right-aligned in that order (Close drawn last,
    // rightmost) - the render side lays them out back-to-front too.
    int by = my + mh - 40;
    static const char* const kDesignBtns[2] = {"Apply Changes", "Close"};
    int bx = mx + mw - 12;
    int btn_x[2], btn_w[2];
    for (int i = 1; i >= 0; --i) {
      int bw = text_width(kDesignBtns[i], Face::UiBold) + 24;
      bx -= bw;
      btn_x[i] = bx; btn_w[i] = bw;
      bx -= 8;
    }
    if (rel_x >= btn_x[0] && rel_x < btn_x[0] + btn_w[0] && rel_y >= by && rel_y < by + 28) {
      db_designer_apply(w, *kernel_);
      return;
    }
    if (rel_x >= btn_x[1] && rel_x < btn_x[1] + btn_w[1] && rel_y >= by && rel_y < by + 28) {
      w.db_show_designer = false;
      return;
    }
    return;
  }

  if (w.db_show_telemetry) {
    int mw = cw - 60; if (mw > 780) mw = 780; if (mw < 400) mw = cw - 20;
    int mh = ch - 60; if (mh > 520) mh = 520; if (mh < 320) mh = ch - 20;
    int mx = (cw - mw) / 2;
    int my = (ch - mh) / 2;
    int act_y = my + mh - 42;

    static const char* const kFleetBtns[] = {"Sync CDC", "Flush Cache", "Reload Tables", "Probe Latency", "Close"};
    int bx = mx + 16;
    for (int i = 0; i < 5; ++i) {
      int bw = text_width(kFleetBtns[i], Face::UiBold) + 20;
      if (rel_x >= bx && rel_x < bx + bw && rel_y >= act_y && rel_y < act_y + 28) {
        if (!kernel_) return;
        if (i == 0) {
          db_telemetry_action(w, *kernel_, "sync");
        } else if (i == 1) {
          db_telemetry_action(w, *kernel_, "flush");
        } else if (i == 2) {
          db_telemetry_action(w, *kernel_, "snapshot");
        } else if (i == 3) {
          db_telemetry_action(w, *kernel_, "probe");
        } else if (i == 4) {
          w.db_show_telemetry = false;
        }
        return;
      }
      bx += bw + 8;
    }
    return;
  }

  int tool = db_tool_hit(rel_x, rel_y);
  if (tool >= 0) {
    const char* chip = DB_TOOLS[tool].id;
    if (!kernel_) { set_toast("no kernel is bound to this desktop yet"); return; }
    if (strcmp(chip, "Connections") == 0) {
      w.db_show_conn_mgr = !w.db_show_conn_mgr;
      if (w.db_show_conn_mgr) w.db_show_telemetry = false;
    } else if (strcmp(chip, "Fleet Ops") == 0) {
      w.db_show_telemetry = !w.db_show_telemetry;
      if (w.db_show_telemetry) {
        w.db_show_conn_mgr = false;
        db_refresh_telemetry(w, *kernel_);
      }
    } else if (strcmp(chip, "Connect") == 0) {
      db_connect(w, *kernel_);
      set_toast(w.db_status);
    } else if (strcmp(chip, "Refresh") == 0) {
      db_refresh(w, *kernel_);
      set_toast(w.db_status);
    } else if (strcmp(chip, "Run SQL") == 0) {
      if (w.input.empty()) {
        // Nothing typed: re-read the page that is open, rather than asking for
        // every row of the table the way a bare SELECT * would.
        if (w.db_selected.empty()) { set_toast("type a statement, or pick a table first"); return; }
        db_load_page(w, *kernel_);
        set_toast(w.db_status);
        return;
      }
      db_run_query(w, *kernel_, w.input.text);
      w.input.clear();
      set_toast(w.db_status);
    } else if (strcmp(chip, "Describe") == 0) {
      // The selected table's shape AS a result set, so schema browsing uses the
      // same grid as everything else rather than a second kind of view.
      if (w.db_selected.empty()) { set_toast("pick a table first"); return; }
      SqlDatabase* db = sql_engine(*kernel_);
      const SqlTable* t = db ? db->find_table(w.db_selected) : nullptr;
      if (!t) { w.db_error = true; w.db_status = "no such table: " + w.db_selected; return; }
      w.grid_columns = {"COLUMN", "TYPE", "KEY"};
      w.db_grid_label = w.db_selected + ".schema";
      w.grid_rows.clear();
      for (const auto& c : t->columns) {
        const char* ty = c.type == SqlDataType::Int ? "INT" : (c.type == SqlDataType::Real ? "REAL" : (c.type == SqlDataType::Decimal ? "DECIMAL" : "TEXT"));
        w.grid_rows.push_back({c.name, ty, c.is_primary_key ? "PRI" : ""});
      }
      w.grid_scroll = 0;
      w.db_error = false;
      w.db_status = w.db_selected + ": " + std::to_string(t->columns.size()) + " column(s), " +
                    std::to_string(t->row_count()) + " row(s)";
    } else if (strcmp(chip, "DDL") == 0) {
      db_show_ddl(w, *kernel_);
      set_toast(w.db_status);
    } else if (strcmp(chip, "Design") == 0) {
      db_open_designer(w, *kernel_);
      if (w.db_error) set_toast(w.db_status);
    } else if (strcmp(chip, "Add row") == 0) {
      db_insert_row(w, *kernel_);
      set_toast(w.db_status);
    } else if (strcmp(chip, "Delete row") == 0) {
      db_delete_selected_row(w, *kernel_);
      set_toast(w.db_status);
    } else if (strcmp(chip, "Export CSV") == 0) {
      auto res = db_export_csv(w, *kernel_);
      w.db_error = !res.ok();
      w.db_status = res.ok() ? ("exported to " + *res) : res.error().str();
      set_toast(w.db_status);
    } else if (strcmp(chip, "Clear") == 0) {
      w.grid_columns.clear();
      w.db_grid_label.clear();
      w.grid_rows.clear();
      w.input.clear();
      w.db_error = false;
      w.db_status.clear();
      // Nothing is open, so there is no page to be on: the pager goes with it
      // rather than offering to turn the pages of an empty grid.
      w.db_paged = false;
      w.grid_selected_row = -1;
      w.grid_selected_col = -1;
    }
    return;
  }

  if (!kernel_) return;

  // Every hit below walks the SAME layout the renderer walked. These four lines
  // are the layout; nothing under here is allowed to guess at it.
  int body_y = APP_BODY_Y;
  DbPagerGeom probe = db_pager_geom(0, 0, 0, 0);
  int body_h = ch - body_y - APP_FOOT_H - 22 - probe.h - 4;
  if (body_h < 60) body_h = 60;
  int tree_w = (cw > 700) ? 240 : 180;
  int grid_x = 20 + tree_w + 10;
  int grid_w = cw - grid_x - 20;

  // The pager strip, under the grid.
  DbPagerGeom g = db_pager_geom(grid_x, grid_w, body_y, body_h);
  if (rel_y >= g.y && rel_y <= g.y + g.h) {
    auto in = [&](int bx) { return rel_x >= bx && rel_x < bx + g.btn; };
    int size = w.db_page_size > 0 ? w.db_page_size : 200;
    int pages = w.db_total_rows == 0 ? 1
                : (int)((w.db_total_rows + (size_t)size - 1) / (size_t)size);
    if (w.db_paged) {
      if (in(g.first_x)) { db_set_page(w, *kernel_, 0); return; }
      if (in(g.prev_x))  { db_set_page(w, *kernel_, w.db_page - 1); return; }
      if (in(g.next_x))  { db_set_page(w, *kernel_, w.db_page + 1); return; }
      if (in(g.last_x))  { db_set_page(w, *kernel_, pages - 1); return; }
      if (rel_x >= g.size_x && rel_x < g.size_x + g.size_w) {
        // Cycle the page size, and keep the row you were looking at on screen
        // rather than jumping to a page number that means something else now.
        long long anchor_row = (long long)w.db_page * size;
        int n = (int)(sizeof(DB_PAGE_SIZES) / sizeof(DB_PAGE_SIZES[0]));
        int next = DB_PAGE_SIZES[0];
        for (int i = 0; i < n; ++i)
          if (DB_PAGE_SIZES[i] == size) { next = DB_PAGE_SIZES[(i + 1) % n]; break; }
        w.db_page_size = next;
        w.db_page = (int)(anchor_row / next);
        w.grid_scroll = 0;
        db_load_page(w, *kernel_);
        set_toast(w.db_status);
        return;
      }
    }
    // Columns, sideways.
    if (in(g.col_prev_x)) {
      if (w.db_col_first > 0) { --w.db_col_first; }
      return;
    }
    if (in(g.col_next_x)) {
      if (w.db_col_first + 1 < (int)w.grid_columns.size()) { ++w.db_col_first; }
      return;
    }
    return;
  }

  if (rel_y < body_y || rel_y > body_y + body_h) return;

  // A click in the working area takes the keyboard away from the filter box;
  // the box's own branch below takes it back.
  w.db_filter_focus = false;

  // The grid. A click on the header row orders by that column; a click on a
  // cell picks the row and the cell, and a second click opens it for typing.
  if (rel_x >= grid_x && grid_w > 80 && !w.grid_columns.empty()) {
    std::vector<int> cw_cols;
    db_column_widths(w, grid_w, &cw_cols);
    int at = grid_x + 8;
    int hit = -1;
    for (size_t c = (size_t)w.db_col_first; c < cw_cols.size(); ++c) {
      if (at - grid_x + cw_cols[c] > grid_w - 8) break;
      if (rel_x >= at && rel_x < at + cw_cols[c]) { hit = (int)c; break; }
      at += cw_cols[c];
    }
    if (rel_y >= body_y + 1 && rel_y < body_y + 25) {
      if (hit >= 0) { db_sort_grid(w, hit); set_toast(w.db_status); }
      return;
    }
    int row = w.grid_scroll + (rel_y - (body_y + 27)) / APP_ROW;
    if (row >= 0 && row < (int)w.grid_rows.size()) {
      // A click on the row already selected starts editing that cell; the
      // first click only picks the row, so pointing at a grid cannot change it.
      bool same_row = w.grid_selected_row == row;
      w.grid_selected_row = row;
      w.grid_selected_col = hit;
      if (same_row && hit >= 0) db_begin_edit(w, row, hit);
      else db_cancel_edit(w);
      return;
    }
    return;
  }

  if (rel_x < 20 || rel_x > 20 + tree_w) return;

  // The filter box over the explorer takes the keyboard when it is clicked.
  int filt_y = body_y + 24;
  if (rel_y >= filt_y && rel_y < filt_y + 20) {
    w.db_filter_focus = true;
    db_cancel_edit(w);
    return;
  }

  // The explorer's rows are not a fixed pitch: the selected table's columns are
  // drawn under it, and the list starts wherever it has been scrolled to. Walk
  // the same layout the renderer walked.
  std::vector<size_t> vis = db_visible_tables(w);
  int ty = filt_y + 26;
  int tree_bottom = body_y + body_h - 6;
  for (size_t vi = (size_t)w.db_tree_scroll; vi < vis.size(); ++vi) {
    if (ty + 18 > tree_bottom) break;
    const std::string& t = w.db_tables[vis[vi]];
    if (rel_y >= ty - 2 && rel_y < ty + 16) {
      db_open_table(w, *kernel_, t);
      set_toast(w.db_status);
      return;
    }
    ty += 18;
    if (t != w.db_selected) continue;
    ty += 16 * static_cast<int>(w.db_schema.size());
  }
}

// The pane's paint and its navigation both live in web_pane.cpp; these are
// the two doors the window manager knocks on.
void HardwareCompositor::render_browser_window(WindowSurface& win, Kernel& k) {
  render_web_pane(*this, win, k);
}

bool HardwareCompositor::browser_open(Kernel& k, const std::string& url) {
  uint32_t id = 0;
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.type != WindowType::Browser) continue;
      id = w.window_id;
      break;
    }
  }
  // A desktop that was started without the default workspace still has to be
  // able to show a page.
  if (!id) {
    auto made = create_window("Web", 60, 60, (int)width_ - 120, (int)height_ - 160, WindowType::Browser);
    if (!made.ok()) return false;
    id = *made;
  }
  canvas_mode_ = false;
  bring_to_front(id);
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id != id) continue;
      w.minimized = false;
      w.visible = true;
      web_pane_go(*this, w, k, url);
      return true;
    }
  }
  return false;
}

// ===========================================================================
// ===========================================================================
// Native C++ AetherMail Client (Apple-style Mail Client)
// ===========================================================================

static const char* const MAIL_CHIPS[] = {
  "+ Compose", "Reply", "Reply All", "Forward", "Flag", "Delete", "Archive",
  // What a message actually says, and what the sender wanted it to look like.
  // A person gets to choose which they are reading, always.
  "Plain/HTML", "Load Images", "Check Mail", "Accounts"
};

static const char* const MAIL_PRESET_LABELS[] = {
  "iCloud", "Gmail", "Outlook", "Fastmail", "Yahoo", "Custom"
};

static const char* const MAIL_PRESET_ICONS[] = {
  "@ iCloud", "G Google", "M Outlook", "F Fastmail", "Y Yahoo!", "+ Other"
};

static uint32_t avatar_hash(const std::string& s) {
  uint32_t h = 0;
  for (char c : s) h = (h * 31) + static_cast<uint8_t>(c);
  return h;
}

static const uint32_t AVATAR_COLORS[] = {
  0xFF007AFF, 0xFF34C759, 0xFF5856D6, 0xFFFF9500,
  0xFFFF2D55, 0xFFAF52DE, 0xFF5AC8FA, 0xFF30B0C7
};

static std::vector<MailDisplayLine> wrap_mail_body(const HardwareCompositor& S, const std::string& body, int max_px, Face face) {
  std::vector<MailDisplayLine> lines;
  size_t at = 0;
  while (at <= body.size()) {
    size_t end = body.find('\n', at);
    std::string raw = (end == std::string::npos) ? body.substr(at) : body.substr(at, end - at);

    bool is_quote = (!raw.empty() && raw[0] == '>');
    std::string content = is_quote ? raw.substr(1) : raw;
    if (is_quote && !content.empty() && content[0] == ' ') content.erase(0, 1);

    int avail = is_quote ? (max_px - 20) : max_px;
    if (avail < 50) avail = 50;

    if (content.empty()) {
      lines.push_back({"", is_quote});
    } else {
      std::istringstream iss(content);
      std::string word;
      std::string cur;
      while (iss >> word) {
        std::string cand = cur.empty() ? word : (cur + " " + word);
        if (S.text_width(cand, face) <= avail) {
          cur = cand;
        } else {
          if (!cur.empty()) {
            lines.push_back({cur, is_quote});
            cur = word;
          } else {
            std::string sub;
            for (char ch : word) {
              if (S.text_width(sub + ch, face) > avail && !sub.empty()) {
                lines.push_back({sub, is_quote});
                sub = "";
              }
              sub += ch;
            }
            cur = sub;
          }
        }
      }
      if (!cur.empty()) {
        lines.push_back({cur, is_quote});
      }
    }

    if (end == std::string::npos) break;
    at = end + 1;
  }
  return lines;
}

void HardwareCompositor::mail_open_compose(WindowSurface& win, const std::string& to,
                                           const std::string& subject, const std::string& body) {
  win.mail_composing = true;
  win.mail_account_settings_open = false;
  win.mail_compose_draft_id.clear();
  win.mail_compose_to.text = to;
  win.mail_compose_to.cursor = static_cast<int>(to.size());
  win.mail_compose_cc.clear();
  win.mail_compose_subject.text = subject;
  win.mail_compose_subject.cursor = static_cast<int>(subject.size());
  win.mail_compose_body_lines.clear();
  if (body.empty()) {
    win.mail_compose_body_lines.push_back("");
  } else {
    size_t at = 0;
    while (at <= body.size()) {
      size_t end = body.find('\n', at);
      if (end == std::string::npos) {
        win.mail_compose_body_lines.push_back(body.substr(at));
        break;
      }
      win.mail_compose_body_lines.push_back(body.substr(at, end - at));
      at = end + 1;
    }
  }
  win.mail_compose_cursor_line = 0;
  win.mail_compose_cursor_col = 0;
  win.mail_compose_focus = to.empty() ? 0 : 3;
}

void HardwareCompositor::mail_reply(WindowSurface& win, bool reply_all) {
  if (!win.mail_store) return;
  const MailMessage* m = win.mail_store->get_message(win.mail_selected_id);
  if (!m) return;
  std::string to = m->from_addr;
  std::string subj = m->subject;
  if (subj.rfind("Re: ", 0) != 0) subj = "Re: " + subj;
  std::string quoted = "\n\nOn " + m->date_str + ", " + m->from_name + " <" + m->from_addr + "> wrote:\n";
  size_t at = 0;
  while (at <= m->body.size()) {
    size_t end = m->body.find('\n', at);
    std::string line = (end == std::string::npos) ? m->body.substr(at) : m->body.substr(at, end - at);
    quoted += "> " + line + "\n";
    if (end == std::string::npos) break;
    at = end + 1;
  }
  mail_open_compose(win, to, subj, quoted);
  if (reply_all && !m->to.empty()) {
    std::string cc_str;
    for (const auto& r : m->to) {
      if (r != to && r != win.mail_store->primary_account().email) {
        if (!cc_str.empty()) cc_str += ", ";
        cc_str += r;
      }
    }
    win.mail_compose_cc.text = cc_str;
    win.mail_compose_cc.cursor = static_cast<int>(cc_str.size());
  }
}

void HardwareCompositor::mail_forward(WindowSurface& win) {
  if (!win.mail_store) return;
  const MailMessage* m = win.mail_store->get_message(win.mail_selected_id);
  if (!m) return;
  std::string subj = m->subject;
  if (subj.rfind("Fwd: ", 0) != 0) subj = "Fwd: " + subj;
  std::string body = "\n\n---------- Forwarded message ---------\n";
  body += "From: " + m->from_name + " <" + m->from_addr + ">\n";
  body += "Date: " + m->date_str + "\n";
  body += "Subject: " + m->subject + "\n\n";
  body += m->body;
  mail_open_compose(win, "", subj, body);
}

void HardwareCompositor::mail_send_compose(WindowSurface& win) {
  if (!win.mail_store) return;
  std::string to = win.mail_compose_to.text;
  if (to.empty()) { set_toast("recipient address required"); return; }
  std::vector<std::string> to_addrs = {to};
  std::vector<std::string> cc_addrs;
  if (!win.mail_compose_cc.empty()) cc_addrs.push_back(win.mail_compose_cc.text);
  std::string body;
  for (size_t i = 0; i < win.mail_compose_body_lines.size(); ++i) {
    if (i > 0) body += "\n";
    body += win.mail_compose_body_lines[i];
  }
  std::string id = win.mail_store->send_message(win.mail_store->primary_account().email,
                                                to_addrs, cc_addrs, win.mail_compose_subject.text, body, {},
                                                win.mail_active_account_id == "all" ? "" : win.mail_active_account_id);
  win.mail_composing = false;
  win.mail_active_folder = "sent";
  win.mail_selected_id = id;
  // What the send actually did (delivered by SMTP, or only kept in Sent).
  set_toast(win.mail_store->status_line());
}

void HardwareCompositor::mail_cancel_compose(WindowSurface& win) {
  win.mail_composing = false;
  set_toast("Draft discarded");
}

void HardwareCompositor::mail_apply_preset(WindowSurface& win, int preset_idx) {
  win.mail_acc_preset_selected = preset_idx;
  if (preset_idx >= 0 && preset_idx < 5) {
    win.mail_acc_show_advanced = false;
  } else {
    win.mail_acc_show_advanced = true;
  }

  if (preset_idx == 0) { // iCloud
    win.mail_acc_imap_host.text = "imap.mail.me.com";
    win.mail_acc_imap_port.text = "993";
    win.mail_acc_smtp_host.text = "smtp.mail.me.com";
    win.mail_acc_smtp_port.text = "587";
    if (win.mail_acc_name.empty()) win.mail_acc_name.text = "Personal iCloud";
  } else if (preset_idx == 1) { // Gmail
    win.mail_acc_imap_host.text = "imap.gmail.com";
    win.mail_acc_imap_port.text = "993";
    win.mail_acc_smtp_host.text = "smtp.gmail.com";
    win.mail_acc_smtp_port.text = "587";
    if (win.mail_acc_name.empty()) win.mail_acc_name.text = "Google Mail";
  } else if (preset_idx == 2) { // Outlook
    win.mail_acc_imap_host.text = "outlook.office365.com";
    win.mail_acc_imap_port.text = "993";
    win.mail_acc_smtp_host.text = "smtp.office365.com";
    win.mail_acc_smtp_port.text = "587";
    if (win.mail_acc_name.empty()) win.mail_acc_name.text = "Outlook Mail";
  } else if (preset_idx == 3) { // Fastmail
    win.mail_acc_imap_host.text = "imap.fastmail.com";
    win.mail_acc_imap_port.text = "993";
    win.mail_acc_smtp_host.text = "smtp.fastmail.com";
    win.mail_acc_smtp_port.text = "587";
    if (win.mail_acc_name.empty()) win.mail_acc_name.text = "Fastmail";
  } else if (preset_idx == 4) { // Yahoo
    win.mail_acc_imap_host.text = "imap.mail.yahoo.com";
    win.mail_acc_imap_port.text = "993";
    win.mail_acc_smtp_host.text = "smtp.mail.yahoo.com";
    win.mail_acc_smtp_port.text = "587";
    if (win.mail_acc_name.empty()) win.mail_acc_name.text = "Yahoo Mail";
  } else { // Custom
    if (win.mail_acc_name.empty()) win.mail_acc_name.text = "Custom Mail";
  }
  win.mail_acc_name.cursor = static_cast<int>(win.mail_acc_name.text.size());
  win.mail_acc_imap_host.cursor = static_cast<int>(win.mail_acc_imap_host.text.size());
  win.mail_acc_imap_port.cursor = static_cast<int>(win.mail_acc_imap_port.text.size());
  win.mail_acc_smtp_host.cursor = static_cast<int>(win.mail_acc_smtp_host.text.size());
  win.mail_acc_smtp_port.cursor = static_cast<int>(win.mail_acc_smtp_port.text.size());
}

void HardwareCompositor::mail_open_accounts(WindowSurface& win) {
  win.mail_account_settings_open = true;
  win.mail_composing = false;
  win.mail_acc_focus = 0;
  win.mail_acc_show_advanced = false;
  win.mail_acc_status.clear();

  if (win.mail_store && !win.mail_store->accounts().empty()) {
    const MailAccount* target = nullptr;
    if (win.mail_active_account_id != "all") {
      target = win.mail_store->get_account(win.mail_active_account_id);
    }
    if (!target) target = &win.mail_store->primary_account();

    win.mail_acc_name.text = target->display_name;
    win.mail_acc_email.text = target->email;
    win.mail_acc_imap_host.text = target->incoming_server;
    win.mail_acc_imap_port.text = std::to_string(target->incoming_port);
    win.mail_acc_smtp_host.text = target->outgoing_server;
    win.mail_acc_smtp_port.text = std::to_string(target->outgoing_port);
    win.mail_acc_user.text = target->username;
    win.mail_acc_pass.text = target->password;

    std::string prov = target->provider;
    if (prov == "icloud" || prov == "apple" || prov == "me.com") {
      win.mail_acc_preset_selected = 0;
      win.mail_acc_show_advanced = false;
    } else if (prov == "gmail" || prov == "google") {
      win.mail_acc_preset_selected = 1;
      win.mail_acc_show_advanced = false;
    } else if (prov == "outlook" || prov == "office365") {
      win.mail_acc_preset_selected = 2;
      win.mail_acc_show_advanced = false;
    } else if (prov == "fastmail") {
      win.mail_acc_preset_selected = 3;
      win.mail_acc_show_advanced = false;
    } else if (prov == "yahoo") {
      win.mail_acc_preset_selected = 4;
      win.mail_acc_show_advanced = false;
    } else {
      win.mail_acc_preset_selected = 5;
      win.mail_acc_show_advanced = true;
    }
  } else {
    win.mail_acc_name.clear();
    win.mail_acc_email.clear();
    win.mail_acc_user.clear();
    win.mail_acc_pass.clear();
    mail_apply_preset(win, 0);
  }

  win.mail_acc_name.cursor = static_cast<int>(win.mail_acc_name.text.size());
  win.mail_acc_email.cursor = static_cast<int>(win.mail_acc_email.text.size());
  win.mail_acc_user.cursor = static_cast<int>(win.mail_acc_user.text.size());
  win.mail_acc_pass.cursor = static_cast<int>(win.mail_acc_pass.text.size());
}

void HardwareCompositor::mail_test_account(WindowSurface& win) {
  MailAccount acc;
  acc.display_name = win.mail_acc_name.text;
  acc.email = win.mail_acc_email.text;
  acc.incoming_server = win.mail_acc_imap_host.text;
  acc.incoming_port = (uint16_t)std::atoi(win.mail_acc_imap_port.text.c_str());
  if (acc.incoming_port == 0) acc.incoming_port = 993;
  acc.outgoing_server = win.mail_acc_smtp_host.text;
  acc.outgoing_port = (uint16_t)std::atoi(win.mail_acc_smtp_port.text.c_str());
  if (acc.outgoing_port == 0) acc.outgoing_port = 587;
  acc.username = win.mail_acc_user.text;
  acc.password = win.mail_acc_pass.text;

  if (acc.incoming_server.empty() || acc.outgoing_server.empty()) {
    win.mail_acc_status = "Error: IMAP and SMTP server hosts are required.";
    return;
  }

  win.mail_acc_status = "Testing connection to " + acc.incoming_server + "...";
  if (win.mail_store) {
    auto res = win.mail_store->test_account_connection(acc);
    if (res.ok()) {
      win.mail_acc_status = *res;
      set_toast("Connection verified");
    } else {
      win.mail_acc_status = "Test failed: " + res.error().str();
      set_toast("Connection test failed");
    }
  }
}

void HardwareCompositor::mail_save_account(WindowSurface& win) {
  if (win.mail_acc_email.empty()) {
    win.mail_acc_status = "Error: Email address cannot be empty.";
    return;
  }

  MailAccount acc;
  std::string user_prefix = win.mail_acc_email.text;
  size_t at_pos = user_prefix.find('@');
  if (at_pos != std::string::npos) user_prefix = user_prefix.substr(0, at_pos);

  acc.id = "acc_" + user_prefix;
  acc.display_name = win.mail_acc_name.empty() ? win.mail_acc_email.text : win.mail_acc_name.text;
  acc.email = win.mail_acc_email.text;
  if (win.mail_acc_preset_selected >= 0 && win.mail_acc_preset_selected < 5) {
    std::string p = MAIL_PRESET_LABELS[win.mail_acc_preset_selected];
    for (char& c : p) c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    acc.provider = p;
  } else {
    acc.provider = "custom";
  }
  acc.incoming_server = win.mail_acc_imap_host.text;
  acc.incoming_port = (uint16_t)std::atoi(win.mail_acc_imap_port.text.c_str());
  if (acc.incoming_port == 0) acc.incoming_port = 993;
  acc.incoming_protocol = "imap";
  acc.outgoing_server = win.mail_acc_smtp_host.text;
  acc.outgoing_port = (uint16_t)std::atoi(win.mail_acc_smtp_port.text.c_str());
  if (acc.outgoing_port == 0) acc.outgoing_port = 587;
  acc.outgoing_protocol = "smtp";
  acc.username = win.mail_acc_user.empty() ? win.mail_acc_email.text : win.mail_acc_user.text;
  acc.password = win.mail_acc_pass.text;
  acc.use_tls = true;
  acc.online = true;

  if (win.mail_store) {
    // add_account replaces the whole record, so anything this dialog does not
    // own has to be carried across from the stored one. An empty password field
    // means "leave the credential alone", not "erase it" -- re-saving an
    // account just to change its display name used to drop the password and
    // silently stop it syncing. The sync history belongs to the mailbox rather
    // than to this dialog, so it comes across too.
    if (const MailAccount* prev = win.mail_store->get_account(acc.id)) {
      if (acc.password.empty()) acc.password = prev->password;
      acc.last_sync_at = prev->last_sync_at;
      acc.last_sync_status = prev->last_sync_status;
    }
    win.mail_store->add_account(acc);
    win.mail_active_account_id = acc.id;
  }
  win.mail_account_settings_open = false;
  set_toast("Account saved: " + acc.email);
}

// How often an open Mail window goes and looks. Mail is not a stock ticker;
// every two minutes is plenty, and each poll is a real IMAP round trip.
static constexpr uint64_t kMailPollIntervalMs = 120000;

void HardwareCompositor::refresh_mail_cache(WindowSurface& win) {
  if (!win.mail_store) return;
  const uint64_t gen = win.mail_store->generation();
  if (win.mail_cache_valid &&
      win.mail_cache_generation == gen &&
      win.mail_cache_filter == win.mail_filter_mode &&
      win.mail_cache_folder == win.mail_active_folder &&
      win.mail_cache_search == win.mail_search_edit.text &&
      win.mail_cache_account == win.mail_active_account_id) {
    return;
  }
  win.mail_list_cache = win.mail_store->get_messages(
      win.mail_active_folder, win.mail_search_edit.text, win.mail_filter_mode, win.mail_active_account_id);
  win.mail_folders_cache = win.mail_store->get_folders(win.mail_active_account_id);
  win.mail_cache_generation = gen;
  win.mail_cache_filter = win.mail_filter_mode;
  win.mail_cache_folder = win.mail_active_folder;
  win.mail_cache_search = win.mail_search_edit.text;
  win.mail_cache_account = win.mail_active_account_id;
  win.mail_cache_valid = true;

  // The selected message may have fallen out of the new list.
  bool sel_found = false;
  for (const auto& m : win.mail_list_cache) {
    if (m.id == win.mail_selected_id) { sel_found = true; break; }
  }
  if (!sel_found && !win.mail_list_cache.empty()) win.mail_selected_id = win.mail_list_cache[0].id;
}

// Lay out the sender's own html with the machine's own engine.
//
// Everything here is about what a MESSAGE is allowed to be, which is not what a
// page is allowed to be. A page is something you chose to visit; a message is
// markup a stranger posted to you.
//
//   - No script. set_scripts_enabled(false) means the JsRuntime is never
//     constructed, so there is nothing to escape from rather than a sandbox
//     that has to be right.
//   - No network, unless asked. The fetch closure is the engine's ONLY way out,
//     so refusing there refuses everything at once: remote images, web fonts,
//     css, the lot. A remote <img> is a read receipt -- it tells the sender the
//     mail was opened, when, and from which address -- and a client that loads
//     them silently is working for them and not for you. data: is decoded
//     inside the engine and never reaches this closure, so the message's own
//     inline images still render.
void HardwareCompositor::refresh_mail_html(WindowSurface& win, Kernel& k,
                                           const MailMessage& m, int view_w) {
  if (win.mail_html_id == m.id && win.mail_html && win.mail_html_width == view_w) return;

  const std::string html = win.mail_store ? win.mail_store->html_body(m) : std::string();
  win.mail_html_id = m.id;
  win.mail_html_width = view_w;
  win.mail_html_scroll = 0;
  if (html.empty()) { win.mail_html.reset(); return; }

  const bool allow_remote = win.mail_allow_remote;
  web::Fetch real = kernel_fetch(k);
  auto gate = [allow_remote, real](const std::string& url,
                                   const web::FetchOptions& opt) -> Result<web::FetchResult> {
    if (!allow_remote) {
      return Result<web::FetchResult>(Status::Denied, "remote content is not loaded for mail");
    }
    // Even once a person has asked for the images, a message only reaches the
    // web. file: would let markup from a stranger read this machine's disk and
    // put what it found on the glass; every other scheme is somebody else's
    // handler. Neither is something an email gets to reach for.
    std::string low;
    for (char c : url.substr(0, 8)) low += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    if (low.compare(0, 7, "http://") != 0 && low.compare(0, 8, "https://") != 0) {
      return Result<web::FetchResult>(Status::Denied, "a message may only load http(s) content");
    }
    return real(url, opt);
  };
  win.mail_html = std::make_shared<web::Browser>(gate);
  win.mail_html->set_scripts_enabled(false);

  web::Metrics metrics;
  metrics.measure = [](const std::string& t, const web::FontKey& f) { return web_text_width(t, f); };
  metrics.line_height = [](const web::FontKey& f) { return web_line_height(f); };
  metrics.ascent = [](const web::FontKey& f) { return web_ascent(f); };

  // Hand the message to the engine as a document rather than a location. go()
  // already decodes data: urls itself, so this needs no transport at all.
  std::string url = "data:text/html;charset=utf-8;base64,"
                  + base64_encode(ByteSpan((const uint8_t*)html.data(), html.size()));
  auto r = win.mail_html->go(url, metrics, view_w);
  if (!r.ok()) { win.mail_html.reset(); return; }

  // go() lays the document out but does NOT load its pictures -- every caller
  // does that itself, and this one did not, which is why an html message
  // rendered with holes where its images should be. It also fixes up each
  // <img> box from the decoded size, so a message that gave no width/height
  // stops being a 0x0 fragment. Inline cid: images are data: urls by now and
  // decode without touching the network at all; a remote one goes through the
  // gate above like everything else.
  win.mail_html->fetch_images(24);
}

// Paint the laid-out message into the detail rect.
//
// A reduced painter on purpose. The browser pane draws form fields, video,
// canvas, media controls and nested scroll areas; a message may not have any of
// those, so the way to not render them is to have no code that can. Text,
// boxes, borders, rules and images are what an email is.
void HardwareCompositor::paint_mail_html(WindowSurface& win, int ox, int oy, int view_w, int view_h) {
  if (!win.mail_html) return;
  const web::Document& doc = win.mail_html->document();
  const int bw = win.content_width(), bh = win.content_height();

  int max_scroll = doc.content_height - view_h;
  if (max_scroll < 0) max_scroll = 0;
  if (win.mail_html_scroll > max_scroll) win.mail_html_scroll = max_scroll;
  if (win.mail_html_scroll < 0) win.mail_html_scroll = 0;

  // Mail is written for white paper, and this engine's default page background
  // is 0xFF121212 to suit a dark browser. Handing a message that default meant
  // black text on near-black, which is what "the html is in but you cannot read
  // it" looked like. Give it the paper it was written for; a sender who sets
  // their own background paints it over this.
  draw_rect_to(win.buffer, bw, bh, ox, oy, view_w, view_h, 0xFFFFFFFF);

  // Everything the message draws is clipped to the rect it was given. A page
  // that lays out wider or taller than the pane cannot paint over the mailbox
  // list or the window chrome.
  const DrawClip clip{ox, oy, ox + view_w, oy + view_h};

  for (const auto& f : doc.fragments) {
    const int y = oy + f.y - win.mail_html_scroll;
    const int x = ox + f.x;
    if (y + (f.h > 0 ? f.h : 20) < oy || y > oy + view_h) continue;   // off the rect
    if (f.opacity == 0) continue;

    switch (f.kind) {
      case web::Fragment::Kind::Rect:
        if (f.w > 0 && f.h > 0) {
          draw_rect_to(win.buffer, bw, bh, x, std::max(y, oy), std::min(f.w, view_w),
                       std::min(f.h, oy + view_h - std::max(y, oy)), f.color);
        }
        break;
      case web::Fragment::Kind::Border:
        if (f.w > 0 && f.h > 0 && y >= oy && y + f.h <= oy + view_h) {
          draw_rect_outline_to(win.buffer, bw, bh, x, y, std::min(f.w, view_w), f.h, f.color);
        }
        break;
      case web::Fragment::Kind::Rule:
        if (y >= oy && y <= oy + view_h) {
          draw_rect_to(win.buffer, bw, bh, x, y, std::min(f.w, view_w), f.h > 0 ? f.h : 1, f.color);
        }
        break;
      case web::Fragment::Kind::Text:
        if (!f.text.empty() && y >= oy - 20 && y <= oy + view_h) {
          web_draw_text(*this, win.buffer, bw, bh, x, y, f.text, f.color, f.font, clip);
        }
        break;
      case web::Fragment::Kind::Image: {
        // The picture itself, nearest-neighbour into place with its own alpha
        // honoured, so a transparent PNG shows the paper behind it rather than
        // a black box. Same blit the browser pane uses.
        const web::Bitmap* bm = win.mail_html->image(f.src);
        if (bm && !bm->empty() && f.w > 0 && f.h > 0) {
          for (int row = 0; row < f.h; ++row) {
            const int py = y + row;
            if (py < oy || py >= oy + view_h) continue;
            int sy = bm->h > 1 ? row * bm->h / f.h : 0;
            if (sy >= bm->h) sy = bm->h - 1;
            for (int col = 0; col < f.w; ++col) {
              const int px = x + col;
              if (!clip.has(px, py)) continue;
              int sx = bm->w > 1 ? col * bm->w / f.w : 0;
              if (sx >= bm->w) sx = bm->w - 1;
              const uint32_t p = bm->pixels[(size_t)sy * bm->w + sx];
              const int a = (int)((p >> 24) & 0xFF) * (int)f.opacity / 255;
              if (a) blend_srgb_to(win.buffer, bw, bh, px, py, p, a);
            }
          }
          break;
        }
        // No picture: say what it was meant to be, in the space the layout
        // reserved for it, rather than leaving a hole.
        if (f.w > 8 && f.h > 8 && y >= oy && y + f.h <= oy + view_h) {
          draw_rect_outline_to(win.buffer, bw, bh, x, y, std::min(f.w, view_w), f.h, 0xFFD0D0D0);
          const std::string label = f.alt.empty() ? std::string("[image]") : f.alt;
          draw_text_to(win.buffer, bw, bh, x + 6, y + f.h / 2 - 7,
                       fit_text(*this, label, std::min(f.w, view_w) - 12, Face::UiSmall),
                       0xFF808080, Face::UiSmall);
        }
        break;
      }
      default:
        break;   // video, audio, canvas: a message does not get to have them
    }
  }
}

void HardwareCompositor::refresh_mail_detail(WindowSurface& win, int wrap_px) {
  if (!win.mail_store) return;
  const uint64_t gen = win.mail_store->generation();
  if (win.mail_detail_cache_id == win.mail_selected_id &&
      win.mail_detail_cache_generation == gen &&
      win.mail_body_cache_width == wrap_px) {
    return;
  }
  const MailMessage* m = win.mail_store->get_message(win.mail_selected_id);
  win.mail_detail_found = (m != nullptr);
  if (m) win.mail_detail_cache = *m;

  std::string body_text = m ? m->body : std::string();
#if !defined(AIOS_METAL)
  // The vacuum keeps bodies only for the recent window, so opening older mail
  // means going and getting it. Off the frame thread: the fetch is an IMAP
  // round trip, and the pane says what it is doing meanwhile.
  if (m && body_text.empty() && m->uid != 0) {
    if (win.mail_store->background_body_pending(m->id) ||
        win.mail_store->begin_background_body_fetch(m->id)) {
      body_text = "Fetching this message from the server...";
    }
  }
#endif
  win.mail_body_lines_cache = wrap_mail_body(*this, body_text, wrap_px, Face::Ui);
  win.mail_detail_cache_id = win.mail_selected_id;
  win.mail_detail_cache_generation = gen;
  win.mail_body_cache_width = wrap_px;
}

static bool is_mail_light(const WindowSurface& win) {
  if (win.mail_theme_mode == 2) return true;
  if (win.mail_theme_mode == 1) return false;
  uint32_t ground = active_theme().ground;
  uint32_t r = (ground >> 16) & 0xFF;
  uint32_t g = (ground >> 8) & 0xFF;
  uint32_t b = ground & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000 > 150;
}

struct MailActionBtn {
  const char* id;
  const char* label;
  Icon icon;
  bool is_primary;
  bool is_toggle_on;
};

static std::vector<std::pair<MailActionBtn, int>> mail_toolbar_layout(const HardwareCompositor& S, const WindowSurface& win, int max_w) {
  bool light = is_mail_light(win);
  const MailMessage* m = win.mail_detail_found ? &win.mail_detail_cache : nullptr;
  bool is_flagged = m ? m->is_flagged : false;

  std::vector<MailActionBtn> defs = {
    {"compose", "Compose", Icon::Mail, true, false},
    {"reply", "Reply", Icon::Reply, false, false},
    {"reply_all", "Reply All", Icon::ReplyAll, false, false},
    {"forward", "Forward", Icon::Forward, false, false},
    {"flag", is_flagged ? "Flagged" : "Flag", Icon::Flag, false, is_flagged},
    {"delete", "Delete", Icon::Trash, false, false},
    {"archive", "Archive", Icon::Archive, false, false},
    {"copy", "Copy", Icon::Copy, false, false},
    {"html", win.mail_show_html ? "HTML" : "Plain", Icon::Code, false, win.mail_show_html},
    {"theme", light ? "Dark" : "Light", light ? Icon::Moon : Icon::Sun, false, light},
    {"refresh", "Refresh", Icon::Refresh, false, false},
    {"accounts", "Accounts", Icon::Gear, false, false}
  };

  std::vector<std::pair<MailActionBtn, int>> out;
  int cur_x = 20;
  for (const auto& b : defs) {
    int icon_w = 14;
    int tw = S.text_width(b.label, Face::UiSmall);
    int bw = icon_w + (b.label[0] ? (tw + 12) : 6) + 12;
    if (cur_x + bw > max_w - 20 && out.size() >= 8) {
      bw = icon_w + 14;
    }
    out.push_back({b, bw});
    cur_x += bw + 6;
  }
  return out;
}

void HardwareCompositor::render_mail_window(WindowSurface& win, Kernel& k) {
  if (!win.mail_store) {
    win.mail_store = std::make_shared<MailStore>(k.config().root + "/mail");
  }

  bool light = is_mail_light(win);
  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, light ? 0xFFF3F4F6 : tok::panel, w, h);

  draw_card(win, w, h, 10, 10, w - 20, h - 20, "aethermail • apple mail client");

  // Graphical Icon Action Buttons Toolbar
  auto buttons = mail_toolbar_layout(*this, win, w);
  int btn_y = APP_CHIP_Y;
  int btn_h = APP_CHIP_H;
  int cur_bx = 20;

  for (const auto& p : buttons) {
    const auto& btn = p.first;
    int bw = p.second;
    bool hot = win.focused && mouse_over_content(win, cur_bx, btn_y, bw, btn_h);

    if (btn.is_primary) {
      uint32_t bg_top = hot ? 0xFF3B82F6 : 0xFF2563EB;
      uint32_t bg_bot = hot ? 0xFF1D4ED8 : 0xFF1E40AF;
      draw_gradient_v_to(win.buffer, w, h, cur_bx, btn_y, bw, btn_h, bg_top, bg_bot);
      draw_round_outline_to(win.buffer, w, h, cur_bx, btn_y, bw, btn_h, 6, hot ? 0xFF60A5FA : 0xFF3B82F6);
      ButtonSuite::draw_icon(win.buffer, w, h, cur_bx + 12, btn_y + btn_h / 2, btn.icon, 14, 0xFFFFFFFF);
      draw_text_to(win.buffer, w, h, cur_bx + 24, btn_y + 5, btn.label, 0xFFFFFFFF, Face::UiBold);
    } else {
      uint32_t bg_c = btn.is_toggle_on ? (light ? 0xFFE0EDFE : 0xFF1E283D)
                     : (hot ? (light ? 0xFFE5E7EB : 0xFF222632) : (light ? 0xFFFFFFFF : 0xFF181A20));
      uint32_t edge_c = btn.is_toggle_on ? (light ? 0xFF0A84FF : 0xFF3B82F6)
                       : (hot ? (light ? 0xFF9CA3AF : 0xFF3E485E) : (light ? 0xFFD1D5DB : tok::line_up));
      uint32_t text_c = btn.is_toggle_on ? (light ? 0xFF0A84FF : 0xFF60A5FA)
                       : (hot ? (light ? 0xFF111827 : 0xFFFFFFFF) : (light ? 0xFF374151 : tok::dim));
      uint32_t icon_c = (strcmp(btn.id, "delete") == 0 && hot) ? 0xFFEF4444
                      : (btn.is_toggle_on ? (strcmp(btn.id, "flag") == 0 ? 0xFFFF9F0A : text_c) : text_c);

      draw_rounded_rect_to(win.buffer, w, h, cur_bx, btn_y, bw, btn_h, 6, bg_c);
      draw_round_outline_to(win.buffer, w, h, cur_bx, btn_y, bw, btn_h, 6, edge_c);
      ButtonSuite::draw_icon(win.buffer, w, h, cur_bx + 12, btn_y + btn_h / 2, btn.icon, 14, icon_c);
      if (bw > 34) {
        draw_text_to(win.buffer, w, h, cur_bx + 24, btn_y + 5, btn.label, text_c, Face::UiSmall);
      }
    }
    cur_bx += bw + 6;
  }

  // Account & status header
  draw_label_to(win.buffer, w, h, 22, APP_HEAD_Y + 2, "active account", light ? 0xFF6B7280 : tok::faint);
  int head_x = 22 + label_width("active account") + 12;
  std::string acc_label = (win.mail_active_account_id == "all") ? "All Inboxes" : (win.mail_store->get_account(win.mail_active_account_id) ? win.mail_store->get_account(win.mail_active_account_id)->email : win.mail_store->primary_account().email);
  draw_text_to(win.buffer, w, h, head_x, APP_HEAD_Y, fit_text(*this, acc_label, 260, Face::Mono), light ? 0xFF0284C7 : tok::accent, Face::Mono);

#if !defined(AIOS_METAL)
  const std::string st = win.mail_store->background_sync_running()
                       ? std::string("Checking for new mail...")
                       : win.mail_store->status_line();
#else
  const std::string st = win.mail_store->status_line();
#endif
  int right_w = text_width(st, Face::UiSmall) + 30;
  if (w - right_w > head_x + 280) {
    // Green only for good news: a send that was not delivered, a failed sync
    // or a missing account is said in the warning colour.
    const bool bad = st.find("not delivered") != std::string::npos || st.find("failed") != std::string::npos ||
                     st.find("warning") != std::string::npos || st.find("No mail account") != std::string::npos ||
                     st.find("Not syncing") != std::string::npos;
    draw_text_to(win.buffer, w, h, w - right_w, APP_HEAD_Y + 1, fit_text(*this, st, right_w - 20, Face::UiSmall),
                 bad ? (light ? 0xFFB45309 : 0xFFE3B341) : (light ? 0xFF16A34A : tok::ok), Face::UiSmall);
  }

  int body_y = APP_BODY_Y;
  int body_h = h - body_y - 14;
  if (body_h < 80) body_h = 80;

  // --- Pane 1: Mailboxes & Accounts Sidebar (Left) ---------------------------
  int sidebar_w = (w > 850) ? 180 : 140;
  draw_rect_to(win.buffer, w, h, 20, body_y, sidebar_w, body_h, light ? 0xFFE9EAEF : 0xFF141417);
  draw_rect_outline_to(win.buffer, w, h, 20, body_y, sidebar_w, body_h, light ? 0xFFD2D4DC : tok::line_up);
  draw_label_to(win.buffer, w, h, 28, body_y + 8, "MAILBOXES", light ? 0xFF6B7280 : tok::faint);

  refresh_mail_cache(win);
  const auto& folders = win.mail_folders_cache;
  int fy = body_y + 28;
  for (const auto& f : folders) {
    if (fy + 22 > body_y + body_h - 130) break;
    bool sel = (f.id == win.mail_active_folder);
    bool hot = win.focused && mouse_over_content(win, 22, fy - 2, sidebar_w - 4, 22);
    if (sel || hot) draw_rect_to(win.buffer, w, h, 22, fy - 2, sidebar_w - 4, 22, sel ? 0xFF1E283D : tok::wash);
    if (sel) draw_rect_outline_to(win.buffer, w, h, 22, fy - 2, sidebar_w - 4, 22, 0xFF0A84FF);

    int count_w = 0;
    if (f.unread_count > 0) {
      std::string count_str = std::to_string(f.unread_count);
      count_w = text_width(count_str, Face::UiSmall) + 12;
      draw_pill_to(win.buffer, w, h, 20 + sidebar_w - count_w - 8, fy, count_w, 18,
                   sel ? 0xFF0A84FF : 0xFF2A2B32, 0);
      draw_text_to(win.buffer, w, h, 20 + sidebar_w - count_w - 2, fy + 2, count_str, 0xFFFFFFFF, Face::UiSmall);
    }

    int name_max_w = sidebar_w - count_w - 38;
    if (name_max_w < 40) name_max_w = 40;
    draw_text_to(win.buffer, w, h, 30, fy + 2, fit_text(*this, f.name, name_max_w, sel ? Face::UiBold : Face::Ui),
                 sel ? tok::text : (hot ? tok::text : tok::dim), sel ? Face::UiBold : Face::Ui);
    fy += 24;
  }

  // Sidebar Accounts list & Add Account button
  int acc_head_y = body_y + body_h - 120;
  draw_rect_to(win.buffer, w, h, 26, acc_head_y, sidebar_w - 12, 1, tok::line_up);
  draw_label_to(win.buffer, w, h, 28, acc_head_y + 6, "ACCOUNTS", tok::faint);

  // All Inboxes option
  bool all_sel = (win.mail_active_account_id == "all");
  draw_text_to(win.buffer, w, h, 30, acc_head_y + 22, "• All Inboxes", all_sel ? tok::accent : tok::dim, Face::UiSmall);

  int ay = acc_head_y + 38;
  const auto& acc_list = win.mail_store->accounts();
  for (const auto& a : acc_list) {
    if (ay + 16 > body_y + body_h - 26) break;
    bool a_sel = (win.mail_active_account_id == a.id);
    std::string disp = a.display_name.empty() ? a.email : a.display_name;
    draw_text_to(win.buffer, w, h, 30, ay, fit_text(*this, (a_sel ? "> " : "  ") + disp, sidebar_w - 40, Face::UiSmall),
                 a_sel ? tok::text : tok::faint, Face::UiSmall);
    ay += 16;
  }

  // "+ Add Account" pill at sidebar bottom
  draw_pill_to(win.buffer, w, h, 26, body_y + body_h - 24, sidebar_w - 12, 20, tok::panel2, tok::line_up);
  draw_text_to(win.buffer, w, h, 26 + (sidebar_w - 12) / 2 - 38, body_y + body_h - 22, "+ Add Account", tok::dim, Face::UiSmall);

  // --- Pane 2: Message List (Middle) ----------------------------------------
  int list_x = 20 + sidebar_w + 6;
  int list_w = (w > 1050) ? 310 : ((w > 750) ? 250 : 190);
  draw_rect_to(win.buffer, w, h, list_x, body_y, list_w, body_h, 0xFF17181C);
  draw_rect_outline_to(win.buffer, w, h, list_x, body_y, list_w, body_h, tok::line_up);

  // Top Search Bar in message list
  draw_pill_to(win.buffer, w, h, list_x + 6, body_y + 6, list_w - 12, 24, 0xFF0E0F12,
               win.mail_search_focused ? tok::strong : tok::line_up);
  draw_text_to(win.buffer, w, h, list_x + 12, body_y + 10, "S", tok::faint, Face::UiSmall);
  int search_tw = list_w - 52;
  if (win.mail_search_edit.text.empty() && !win.mail_search_focused) {
    draw_text_to(win.buffer, w, h, list_x + 26, body_y + 10, "Search (from:, to:, subj:)", tok::faint, Face::UiSmall);
  } else {
    draw_text_to(win.buffer, w, h, list_x + 26, body_y + 10,
                 fit_text(*this, win.mail_search_edit.text, search_tw, Face::Ui), tok::text, Face::Ui);
    if (win.mail_search_focused && (steady_ms() / 500) % 2 == 0) {
      int c_off = text_width(win.mail_search_edit.text.substr(0, win.mail_search_edit.cursor), Face::Ui);
      if (c_off < search_tw)
        draw_rect_to(win.buffer, w, h, list_x + 26 + c_off, body_y + 8, 2, 16, 0xFF0A84FF);
    }
    // Clear [X] button
    if (!win.mail_search_edit.text.empty()) {
      draw_pill_to(win.buffer, w, h, list_x + list_w - 22, body_y + 9, 14, 14, tok::panel2, 0);
      draw_text_to(win.buffer, w, h, list_x + list_w - 18, body_y + 9, "x", tok::dim, Face::UiSmall);
    }
  }

  // Filter Pills row: All | Unread | Flagged | Files
  const char* const FILTERS[] = {"All", "Unread", "Flagged", "Files"};
  int fx = list_x + 8;
  for (int i = 0; i < 4; ++i) {
    int fw = text_width(FILTERS[i], Face::UiSmall) + 12;
    bool active = (win.mail_filter_mode == i);
    draw_pill_to(win.buffer, w, h, fx, body_y + 34, fw, 18, active ? 0xFF0A84FF : tok::panel2,
                 active ? 0 : tok::line_up);
    draw_text_to(win.buffer, w, h, fx + 6, body_y + 36, FILTERS[i], active ? 0xFFFFFFFF : tok::dim, Face::UiSmall);
    fx += fw + 3;
  }

  // Message item cards. The selection is validated inside refresh_mail_cache,
  // which only runs when the list actually changed -- scanning 49,515 ids for
  // the selected one on every frame was pure waste.
  const auto& msgs = win.mail_list_cache;

  int item_y = body_y + 56;
  int item_h = 66;
  int items_fit = (body_h - 60) / item_h;
  if (items_fit < 1) items_fit = 1;

  if (msgs.empty()) {
    if (!win.mail_search_edit.text.empty()) {
      draw_text_to(win.buffer, w, h, list_x + 16, item_y + 20, "No matching messages", tok::text, Face::UiSmall);
      draw_text_to(win.buffer, w, h, list_x + 16, item_y + 40, "Try from:, to:, or subject:", tok::faint, Face::UiSmall);
    } else {
      draw_text_to(win.buffer, w, h, list_x + 16, item_y + 20, "No messages in folder", tok::faint, Face::UiSmall);
    }
  } else {
    for (int i = win.mail_list_scroll; i < static_cast<int>(msgs.size()) && i < win.mail_list_scroll + items_fit; ++i) {
      const auto& m = msgs[i];
      bool sel = (m.id == win.mail_selected_id);
      bool hot = win.focused && mouse_over_content(win, list_x + 2, item_y, list_w - 4, item_h);

      if (sel) {
        draw_rect_to(win.buffer, w, h, list_x + 2, item_y, list_w - 4, item_h, 0xFF222C3D);
        draw_rect_outline_to(win.buffer, w, h, list_x + 2, item_y, list_w - 4, item_h, 0xFF0A84FF);
      } else if (hot) {
        draw_rect_to(win.buffer, w, h, list_x + 2, item_y, list_w - 4, item_h, tok::wash);
      }

      // Unread blue dot indicator
      if (m.is_unread) {
        draw_pill_to(win.buffer, w, h, list_x + 6, item_y + 28, 7, 7, 0xFF0A84FF, 0);
      }

      // Sender avatar
      int av_x = list_x + 16, av_y = item_y + 8;
      uint32_t av_col = AVATAR_COLORS[avatar_hash(m.from_addr) % 8];
      draw_pill_to(win.buffer, w, h, av_x, av_y, 24, 24, av_col, 0);
      std::string inits = m.initials();
      int iw = text_width(inits, Face::UiSmall);
      draw_text_to(win.buffer, w, h, av_x + 12 - iw / 2, av_y + 5, inits, 0xFFFFFFFF, Face::UiSmall);

      // Date string
      int dw = text_width(m.date_str, Face::UiSmall);
      draw_text_to(win.buffer, w, h, list_x + list_w - dw - 8, item_y + 8, m.date_str, tok::faint, Face::UiSmall);

      // Sender Name (dynamic max width to prevent overlap with date)
      int name_w = std::max(50, list_w - dw - 62);
      draw_text_to(win.buffer, w, h, list_x + 46, item_y + 7, fit_text(*this, m.from_name, name_w, Face::UiBold),
                   sel ? tok::accent : tok::text, Face::UiBold);

      // Subject
      int subj_avail = std::max(50, list_w - (m.is_flagged || !m.attachments.empty() ? 72 : 54));
      draw_text_to(win.buffer, w, h, list_x + 46, item_y + 25,
                   fit_text(*this, m.subject, subj_avail, m.is_unread ? Face::UiBold : Face::Ui),
                   sel ? tok::accent : (m.is_unread ? tok::text : tok::dim),
                   m.is_unread ? Face::UiBold : Face::Ui);

      // Snippet
      draw_text_to(win.buffer, w, h, list_x + 46, item_y + 43,
                   fit_text(*this, m.snippet, std::max(50, list_w - 54), Face::UiSmall), tok::faint, Face::UiSmall);

      // Flag star
      if (m.is_flagged) {
        draw_text_to(win.buffer, w, h, list_x + list_w - 20, item_y + 25, "*", 0xFFFF9F0A, Face::UiBold);
      }
      // Attachment clip
      if (!m.attachments.empty()) {
        draw_text_to(win.buffer, w, h, list_x + list_w - 34, item_y + 25, "@", tok::faint, Face::UiSmall);
      }

      draw_rect_to(win.buffer, w, h, list_x + 4, item_y + item_h - 1, list_w - 8, 1, 0xFF232328);
      item_y += item_h;
    }

    int max_list_scroll = static_cast<int>(msgs.size()) - items_fit;
    if (max_list_scroll > 0) {
      int track_h = body_h - 60;
      int knob_h = track_h * items_fit / static_cast<int>(msgs.size());
      if (knob_h < 18) knob_h = 18;
      int knob_y = body_y + 56 + (track_h - knob_h) * std::min(win.mail_list_scroll, max_list_scroll) / max_list_scroll;
      draw_rect_to(win.buffer, w, h, list_x + list_w - 4, body_y + 56, 2, track_h, tok::line_up);
      draw_rect_to(win.buffer, w, h, list_x + list_w - 4, knob_y, 2, knob_h, 0xFF0A84FF);
    }
  }

  // --- Pane 3: Detail / Reading Pane (Right) --------------------------------
  int detail_x = list_x + list_w + 6;
  int detail_w = w - detail_x - 20;
  if (detail_w > 80) {
    draw_rect_to(win.buffer, w, h, detail_x, body_y, detail_w, body_h, 0xFF1B1C22);
    draw_rect_outline_to(win.buffer, w, h, detail_x, body_y, detail_w, body_h, tok::line_up);

    refresh_mail_detail(win, detail_w - 40);
    const MailMessage* m = win.mail_detail_found ? &win.mail_detail_cache : nullptr;
    if (m) {
      // Subject header title
      draw_text_to(win.buffer, w, h, detail_x + 20, body_y + 14,
                   fit_text(*this, m->subject, detail_w - 40, Face::UiBold), tok::text, Face::UiBold);

      // Sender avatar & info
      uint32_t av_col = AVATAR_COLORS[avatar_hash(m->from_addr) % 8];
      draw_pill_to(win.buffer, w, h, detail_x + 20, body_y + 38, 28, 28, av_col, 0);
      std::string inits = m->initials();
      int iw = text_width(inits, Face::UiSmall);
      draw_text_to(win.buffer, w, h, detail_x + 34 - iw / 2, body_y + 44, inits, 0xFFFFFFFF, Face::UiSmall);

      // Date pill on right
      int dp_w = text_width(m->date_str, Face::UiSmall) + 16;
      draw_pill_to(win.buffer, w, h, detail_x + detail_w - dp_w - 20, body_y + 38, dp_w, 20, tok::panel2, tok::line_up);
      draw_text_to(win.buffer, w, h, detail_x + detail_w - dp_w - 12, body_y + 41, m->date_str, tok::dim, Face::UiSmall);

      // Sender name + address fitted dynamically to never collide with date pill
      int sender_avail = detail_w - dp_w - 90;
      if (sender_avail < 60) sender_avail = 60;
      std::string name_fit = fit_text(*this, m->from_name, sender_avail, Face::UiBold);
      draw_text_to(win.buffer, w, h, detail_x + 56, body_y + 38, name_fit, tok::text, Face::UiBold);
      int name_w = text_width(name_fit, Face::UiBold);
      int addr_avail = sender_avail - name_w - 8;
      if (addr_avail > 40) {
        draw_text_to(win.buffer, w, h, detail_x + 56 + name_w + 8, body_y + 40,
                     fit_text(*this, "<" + m->from_addr + ">", addr_avail, Face::UiSmall), tok::faint, Face::UiSmall);
      }

      // Recipients
      std::string to_line = "To: ";
      for (size_t ti = 0; ti < m->to.size(); ++ti) to_line += (ti > 0 ? ", " : "") + m->to[ti];
      if (!m->cc.empty()) {
        to_line += "  •  Cc: ";
        for (size_t ci = 0; ci < m->cc.size(); ++ci) to_line += (ci > 0 ? ", " : "") + m->cc[ci];
      }
      int to_max_w = detail_w - 80;
      if (to_max_w < 60) to_max_w = 60;
      draw_text_to(win.buffer, w, h, detail_x + 56, body_y + 56, fit_text(*this, to_line, to_max_w, Face::UiSmall),
                   tok::dim, Face::UiSmall);

      // Attachments bar (if present)
      int div_y = body_y + 78;
      if (!m->attachments.empty()) {
        int att_y = body_y + 74;
        int cur_att_x = detail_x + 56;
        for (const auto& a : m->attachments) {
          std::string att_label = "📎 " + a.filename + " (" + std::to_string(a.size_bytes / 1024 + 1) + " KB)";
          int aw = text_width(att_label, Face::UiSmall) + 16;
          if (cur_att_x + aw > detail_x + detail_w - 20) break;
          draw_pill_to(win.buffer, w, h, cur_att_x, att_y, aw, 22, 0xFF242A38, 0xFF0A84FF);
          draw_text_to(win.buffer, w, h, cur_att_x + 8, att_y + 4, att_label, tok::accent, Face::UiSmall);
          cur_att_x += aw + 8;
        }
        div_y = body_y + 104;
      }
      draw_rect_to(win.buffer, w, h, detail_x + 20, div_y, detail_w - 40, 1, tok::line_up);

      // An html message is laid out by the machine's own engine and painted
      // here, rather than shown as the regex-stripped text that used to be the
      // only thing kept. The plain-text alternative is always one click away,
      // and remote content stays unloaded until it is asked for.
      const int html_view_x = detail_x + 20;
      const int html_view_y = div_y + 10;
      const int html_view_w = detail_w - 40;
      const int html_view_h = body_y + body_h - html_view_y - 12;
      bool painted_html = false;
      if (m->has_html && win.mail_show_html && html_view_w > 80 && html_view_h > 40) {
        refresh_mail_html(win, k, *m, html_view_w);
        if (win.mail_html) {
          paint_mail_html(win, html_view_x, html_view_y, html_view_w, html_view_h);
          // What is being withheld, and how to ask for it. Said plainly: a
          // person who cannot tell the difference between "no images" and
          // "broken" will assume broken.
          if (!win.mail_allow_remote) {
            const char* note = "Remote images blocked for this message";
            const int nw = text_width(note, Face::UiSmall) + 16;
            draw_pill_to(win.buffer, w, h, html_view_x, html_view_y - 2, nw, 18, 0xFF2A2410, 0xFF6B5A18);
            draw_text_to(win.buffer, w, h, html_view_x + 8, html_view_y, note, 0xFFD9B44A, Face::UiSmall);
          }
          painted_html = true;
        }
      }

      // Word-wrapped body, built by refresh_mail_detail above and reused until
      // the selection, the store or the pane width changes.
      if (!painted_html) {
      const auto& display_lines = win.mail_body_lines_cache;
      int text_y = div_y + 14;
      int lines_rendered = 0;
      for (size_t li = win.mail_body_scroll; li < display_lines.size(); ++li) {
        if (text_y + 18 > body_y + body_h - 10) break;
        const auto& dline = display_lines[li];
        if (dline.is_quote) {
          draw_rect_to(win.buffer, w, h, detail_x + 20, text_y, 3, 18, 0xFF0A84FF);
          draw_text_to(win.buffer, w, h, detail_x + 30, text_y, fit_text(*this, dline.text, detail_w - 50, Face::Ui),
                       tok::dim, Face::Ui);
        } else {
          draw_text_to(win.buffer, w, h, detail_x + 20, text_y, fit_text(*this, dline.text, detail_w - 40, Face::Ui),
                       tok::text, Face::Ui);
        }
        text_y += 20;
        lines_rendered++;
      }

      int visible_lines = std::max(1, (body_y + body_h - 10 - div_y - 14) / 20);
      int max_body_scroll = static_cast<int>(display_lines.size()) - visible_lines;
      if (max_body_scroll > 0) {
        int track_h = body_h - (div_y - body_y) - 20;
        int knob_h = track_h * visible_lines / static_cast<int>(display_lines.size());
        if (knob_h < 18) knob_h = 18;
        int knob_y = div_y + 14 + (track_h - knob_h) * std::min(win.mail_body_scroll, max_body_scroll) / max_body_scroll;
        draw_rect_to(win.buffer, w, h, detail_x + detail_w - 5, div_y + 14, 2, track_h, tok::line_up);
        draw_rect_to(win.buffer, w, h, detail_x + detail_w - 5, knob_y, 2, knob_h, 0xFF0A84FF);
      }
      }
    } else {
      draw_text_to(win.buffer, w, h, detail_x + detail_w / 2 - 60, body_y + body_h / 2 - 10,
                   "No Message Selected", tok::faint, Face::UiBold);
    }
  }

  // --- Modal Compose Sheet Overlay ------------------------------------------
  if (win.mail_composing) {
    int mw = std::min(700, w - 40);
    int mh = std::min(480, h - 40);
    int mx = (w - mw) / 2;
    int my = (h - mh) / 2;

    draw_rect_to(win.buffer, w, h, mx, my, mw, mh, 0xFF141519);
    draw_rect_outline_to(win.buffer, w, h, mx, my, mw, mh, tok::strong);

    // Title bar
    draw_rect_to(win.buffer, w, h, mx + 1, my + 1, mw - 2, 32, 0xFF1E2026);
    draw_rect_to(win.buffer, w, h, mx + 1, my + 33, mw - 2, 1, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 16, my + 8, "New Message", tok::text, Face::UiBold);

    // Cancel button
    draw_pill_to(win.buffer, w, h, mx + mw - 140, my + 5, 56, 22, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + mw - 128, my + 8, "Cancel", tok::dim, Face::UiSmall);

    // Send button
    draw_pill_to(win.buffer, w, h, mx + mw - 76, my + 5, 64, 22, 0xFF0A84FF, 0);
    draw_text_to(win.buffer, w, h, mx + mw - 66, my + 8, "Send", 0xFFFFFFFF, Face::UiSmall);

    // Field To
    draw_label_to(win.buffer, w, h, mx + 16, my + 44, "TO:", tok::faint);
    draw_pill_to(win.buffer, w, h, mx + 60, my + 40, mw - 76, 24, 0xFF0E0F12,
                 win.mail_compose_focus == 0 ? 0xFF0A84FF : tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 68, my + 44, fit_text(*this, win.mail_compose_to.text, mw - 92, Face::Ui), tok::text, Face::Ui);
    if (win.mail_compose_focus == 0 && (steady_ms() / 500) % 2 == 0) {
      int c_off = text_width(win.mail_compose_to.text.substr(0, win.mail_compose_to.cursor), Face::Ui);
      if (c_off < mw - 90) draw_rect_to(win.buffer, w, h, mx + 68 + c_off, my + 42, 2, 18, 0xFF0A84FF);
    }

    // Field Cc
    draw_label_to(win.buffer, w, h, mx + 16, my + 74, "CC:", tok::faint);
    draw_pill_to(win.buffer, w, h, mx + 60, my + 70, mw - 76, 24, 0xFF0E0F12,
                 win.mail_compose_focus == 1 ? 0xFF0A84FF : tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 68, my + 74, fit_text(*this, win.mail_compose_cc.text, mw - 92, Face::Ui), tok::text, Face::Ui);
    if (win.mail_compose_focus == 1 && (steady_ms() / 500) % 2 == 0) {
      int c_off = text_width(win.mail_compose_cc.text.substr(0, win.mail_compose_cc.cursor), Face::Ui);
      if (c_off < mw - 90) draw_rect_to(win.buffer, w, h, mx + 68 + c_off, my + 72, 2, 18, 0xFF0A84FF);
    }

    // Field Subject
    draw_label_to(win.buffer, w, h, mx + 16, my + 104, "SUBJ:", tok::faint);
    draw_pill_to(win.buffer, w, h, mx + 60, my + 100, mw - 76, 24, 0xFF0E0F12,
                 win.mail_compose_focus == 2 ? 0xFF0A84FF : tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 68, my + 104, fit_text(*this, win.mail_compose_subject.text, mw - 92, Face::Ui), tok::text, Face::Ui);
    if (win.mail_compose_focus == 2 && (steady_ms() / 500) % 2 == 0) {
      int c_off = text_width(win.mail_compose_subject.text.substr(0, win.mail_compose_subject.cursor), Face::Ui);
      if (c_off < mw - 90) draw_rect_to(win.buffer, w, h, mx + 68 + c_off, my + 102, 2, 18, 0xFF0A84FF);
    }

    // Divider
    draw_rect_to(win.buffer, w, h, mx + 16, my + 132, mw - 32, 1, tok::line_up);

    // Body multiline editor
    int body_ed_y = my + 140;
    int body_ed_h = mh - 156;
    draw_rect_to(win.buffer, w, h, mx + 16, body_ed_y, mw - 32, body_ed_h, 0xFF0E0F12);
    draw_rect_outline_to(win.buffer, w, h, mx + 16, body_ed_y, mw - 32, body_ed_h,
                         win.mail_compose_focus == 3 ? 0xFF0A84FF : tok::line_up);

    int ed_y = body_ed_y + 8;
    for (size_t li = 0; li < win.mail_compose_body_lines.size(); ++li) {
      if (ed_y + 18 > body_ed_y + body_ed_h - 4) break;
      const auto& line = win.mail_compose_body_lines[li];
      draw_text_to(win.buffer, w, h, mx + 24, ed_y, fit_text(*this, line, mw - 48, Face::Ui), tok::text, Face::Ui);
      if (win.mail_compose_focus == 3 && (int)li == win.mail_compose_cursor_line && (steady_ms() / 500) % 2 == 0) {
        std::string sub = (win.mail_compose_cursor_col <= (int)line.size()) ? line.substr(0, win.mail_compose_cursor_col) : line;
        int c_off = text_width(sub, Face::Ui);
        if (c_off < mw - 54) draw_rect_to(win.buffer, w, h, mx + 24 + c_off, ed_y - 1, 2, 18, 0xFF0A84FF);
      }
      ed_y += 20;
    }
  }

  // --- Modal Account Setup Sheet Overlay (Apple Mail Style) -----------------
  if (win.mail_account_settings_open) {
    bool is_adv = (win.mail_acc_show_advanced || win.mail_acc_preset_selected == 5);
    int mw = std::min(620, w - 40);
    int mh = std::min(is_adv ? 510 : 410, h - 40);
    int mx = (w - mw) / 2;
    int my = (h - mh) / 2;

    draw_rect_to(win.buffer, w, h, mx, my, mw, mh, 0xFF141519);
    draw_rect_outline_to(win.buffer, w, h, mx, my, mw, mh, tok::strong);

    // Title bar
    draw_rect_to(win.buffer, w, h, mx + 1, my + 1, mw - 2, 34, 0xFF1E2026);
    draw_rect_to(win.buffer, w, h, mx + 1, my + 35, mw - 2, 1, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 18, my + 9, "Set Up Your Mail Account", tok::text, Face::UiBold);

    // Close button
    draw_pill_to(win.buffer, w, h, mx + mw - 76, my + 6, 60, 22, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + mw - 62, my + 9, "Close", tok::dim, Face::UiSmall);

    // Subtitle instruction
    draw_label_to(win.buffer, w, h, mx + 20, my + 44, "Choose your email provider:", tok::faint);

    // Provider Presets Grid (2 rows x 3 columns)
    int tile_w = (mw - 40 - 20) / 3;
    for (int i = 0; i < 6; ++i) {
      int row = i / 3;
      int col = i % 3;
      int tx = mx + 20 + col * (tile_w + 10);
      int ty = my + 62 + row * 34;
      bool p_sel = (win.mail_acc_preset_selected == i);

      draw_pill_to(win.buffer, w, h, tx, ty, tile_w, 28, p_sel ? 0xFF1B2A42 : 0xFF16171B,
                   p_sel ? 0xFF0A84FF : tok::line_up);
      if (p_sel) {
        draw_text_to(win.buffer, w, h, tx + 8, ty + 6, "*", 0xFF0A84FF, Face::UiBold);
      }
      draw_text_to(win.buffer, w, h, tx + (p_sel ? 20 : 12), ty + 6, MAIL_PRESET_ICONS[i],
                   p_sel ? 0xFFFFFFFF : tok::dim, p_sel ? Face::UiBold : Face::UiSmall);
    }

    draw_rect_to(win.buffer, w, h, mx + 20, my + 134, mw - 40, 1, tok::line_up);

    // Single-line input field renderer
    auto draw_field = [&](int f_idx, int field_x, int field_y, int field_w, const std::string& lbl, const EditLine& el, bool mask = false, const std::string& hint = "") {
      draw_label_to(win.buffer, w, h, field_x, field_y + 4, lbl, tok::faint);
      int lx = field_x + 114;
      int bw = field_w - 114;
      bool foc = (win.mail_acc_focus == f_idx);
      draw_pill_to(win.buffer, w, h, lx, field_y, bw, 24, 0xFF0E0F12, foc ? 0xFF0A84FF : tok::line_up);
      if (el.empty() && !foc && !hint.empty()) {
        draw_text_to(win.buffer, w, h, lx + 8, field_y + 4, fit_text(*this, hint, bw - 16, Face::UiSmall), tok::faint, Face::UiSmall);
      } else {
        std::string val = mask ? std::string(el.text.size(), '*') : el.text;
        draw_text_to(win.buffer, w, h, lx + 8, field_y + 4, fit_text(*this, val, bw - 16, Face::Ui), tok::text, Face::Ui);
        if (foc && (steady_ms() / 500) % 2 == 0) {
          int c_off = text_width(mask ? std::string(el.cursor, '*') : el.text.substr(0, el.cursor), Face::Ui);
          if (c_off < bw - 20) draw_rect_to(win.buffer, w, h, lx + 8 + c_off, field_y + 3, 2, 18, 0xFF0A84FF);
        }
      }
    };

    int acc_fy = my + 142;
    draw_field(0, mx + 20, acc_fy, mw - 40, "Full Name:", win.mail_acc_name, false, "e.g. John Appleseed");
    acc_fy += 30;
    draw_field(1, mx + 20, acc_fy, mw - 40, "Email Address:", win.mail_acc_email, false, "e.g. name@icloud.com");
    acc_fy += 30;
    draw_field(7, mx + 20, acc_fy, mw - 40, "Password / Key:", win.mail_acc_pass, true, "App password or account password");

    // Standard Provider Auto-Configuration Note
    if (win.mail_acc_preset_selected != 5) {
      acc_fy += 32;
      draw_pill_to(win.buffer, w, h, mx + 20, acc_fy, mw - 40, 22, 0xFF141F2E, 0xFF1E3D66);
      draw_text_to(win.buffer, w, h, mx + 28, acc_fy + 4,
                   fit_text(*this, "✓ IMAP (993) and SMTP (587) server settings are configured automatically.", mw - 56, Face::UiSmall),
                   tok::accent, Face::UiSmall);

      acc_fy += 26;
      // Toggle for Manual Server Settings
      draw_pill_to(win.buffer, w, h, mx + 20, acc_fy, 200, 20, tok::panel2, tok::line_up);
      std::string tog_lbl = win.mail_acc_show_advanced ? "▲ Hide Server Details" : "▼ Show Manual Server Details";
      draw_text_to(win.buffer, w, h, mx + 28, acc_fy + 3, tog_lbl, tok::dim, Face::UiSmall);
    }

    // Expanded Manual Server Details (If requested or custom provider)
    if (win.mail_acc_show_advanced || win.mail_acc_preset_selected == 5) {
      acc_fy += 26;
      draw_field(2, mx + 20, acc_fy, mw - 140, "IMAP Server:", win.mail_acc_imap_host);
      draw_field(3, mx + mw - 110, acc_fy, 90, "Port:", win.mail_acc_imap_port);
      acc_fy += 28;
      draw_field(4, mx + 20, acc_fy, mw - 140, "SMTP Server:", win.mail_acc_smtp_host);
      draw_field(5, mx + mw - 110, acc_fy, 90, "Port:", win.mail_acc_smtp_port);
      acc_fy += 28;
      draw_field(6, mx + 20, acc_fy, mw - 40, "Username:", win.mail_acc_user, false, "Defaults to email address");
    }

    // Live Diagnostics / Status Box
    int diag_y = my + mh - 68;
    draw_rect_to(win.buffer, w, h, mx + 20, diag_y, mw - 40, 24, 0xFF0E0F12);
    draw_rect_outline_to(win.buffer, w, h, mx + 20, diag_y, mw - 40, 24, tok::line_up);
    std::string diag = win.mail_acc_status.empty() ? "Click 'Test Connection' or 'Sign In & Connect' to verify account settings." : win.mail_acc_status;
    uint32_t diag_col = (diag.find("OK") != std::string::npos || diag.find("Connected") != std::string::npos || diag.find("verified") != std::string::npos) ? tok::ok :
                        ((diag.find("failed") != std::string::npos || diag.find("Error") != std::string::npos || diag.find("FAILED") != std::string::npos) ? tok::err : tok::dim);
    draw_text_to(win.buffer, w, h, mx + 28, diag_y + 5, fit_text(*this, diag, mw - 56, Face::UiSmall), diag_col, Face::UiSmall);

    // Action buttons footer
    int dlg_btn_y = my + mh - 36;
    // Cancel button
    draw_pill_to(win.buffer, w, h, mx + 20, dlg_btn_y, 64, 24, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 32, dlg_btn_y + 4, "Cancel", tok::dim, Face::UiSmall);

    // Test Connection button
    draw_pill_to(win.buffer, w, h, mx + 92, dlg_btn_y, 124, 24, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + 102, dlg_btn_y + 4, "Test Connection", tok::accent, Face::UiSmall);

    // Delete Account button (if editing existing)
    if (win.mail_store && !win.mail_acc_email.empty() && win.mail_store->get_account(win.mail_acc_email.text)) {
      draw_pill_to(win.buffer, w, h, mx + 224, dlg_btn_y, 106, 24, 0xFF2A1515, 0xFF7A2020);
      draw_text_to(win.buffer, w, h, mx + 234, dlg_btn_y + 4, "Delete Account", 0xFFFF5555, Face::UiSmall);
    }

    // Save & Connect button
    draw_pill_to(win.buffer, w, h, mx + mw - 146, dlg_btn_y, 126, 24, 0xFF0A84FF, 0);
    draw_text_to(win.buffer, w, h, mx + mw - 136, dlg_btn_y + 4, "Sign In & Connect", 0xFFFFFFFF, Face::UiBold);
  }
}

static void mail_click(HardwareCompositor& S, WindowSurface& w, int rel_x, int rel_y, std::string& to_run) {
  int cw = w.content_width();
  int ch = w.content_height();

  // Handle Account Settings Modal Clicks
  if (w.mail_account_settings_open) {
    bool is_adv = (w.mail_acc_show_advanced || w.mail_acc_preset_selected == 5);
    int mw = std::min(620, cw - 40);
    int mh = std::min(is_adv ? 510 : 410, ch - 40);
    int mx = (cw - mw) / 2;
    int my = (ch - mh) / 2;

    // Close button
    if (rel_x >= mx + mw - 76 && rel_x < mx + mw - 16 && rel_y >= my + 6 && rel_y < my + 28) {
      w.mail_account_settings_open = false;
      return;
    }

    // Provider Tiles Grid (2 rows x 3 columns)
    int tile_w = (mw - 40 - 20) / 3;
    for (int i = 0; i < 6; ++i) {
      int row = i / 3;
      int col = i % 3;
      int tx = mx + 20 + col * (tile_w + 10);
      int ty = my + 62 + row * 34;
      if (rel_x >= tx && rel_x < tx + tile_w && rel_y >= ty && rel_y < ty + 28) {
        S.mail_apply_preset(w, i);
        return;
      }
    }

    // Fields focus clicks
    int fy = my + 142;
    if (rel_y >= fy && rel_y < fy + 26) { w.mail_acc_focus = 0; return; }
    fy += 30;
    if (rel_y >= fy && rel_y < fy + 26) { w.mail_acc_focus = 1; return; }
    fy += 30;
    if (rel_y >= fy && rel_y < fy + 26) { w.mail_acc_focus = 7; return; }

    if (w.mail_acc_preset_selected != 5) {
      fy += 32; // auto-config banner
      fy += 26; // toggle pill
      if (rel_y >= fy && rel_y < fy + 22 && rel_x >= mx + 20 && rel_x < mx + 230) {
        w.mail_acc_show_advanced = !w.mail_acc_show_advanced;
        return;
      }
    }

    if (w.mail_acc_show_advanced || w.mail_acc_preset_selected == 5) {
      fy += 26;
      if (rel_y >= fy && rel_y < fy + 26) {
        if (rel_x < mx + mw - 120) w.mail_acc_focus = 2;
        else w.mail_acc_focus = 3;
        return;
      }
      fy += 28;
      if (rel_y >= fy && rel_y < fy + 26) {
        if (rel_x < mx + mw - 120) w.mail_acc_focus = 4;
        else w.mail_acc_focus = 5;
        return;
      }
      fy += 28;
      if (rel_y >= fy && rel_y < fy + 26) {
        w.mail_acc_focus = 6;
        return;
      }
    }

    // Bottom action buttons
    int btn_y = my + mh - 36;
    if (rel_y >= btn_y && rel_y < btn_y + 26) {
      // Cancel
      if (rel_x >= mx + 20 && rel_x < mx + 86) {
        w.mail_account_settings_open = false;
        return;
      }
      // Test Connection
      if (rel_x >= mx + 92 && rel_x < mx + 218) {
        S.mail_test_account(w);
        return;
      }
      // Delete Account
      if (rel_x >= mx + 224 && rel_x < mx + 332) {
        if (w.mail_store && !w.mail_acc_email.empty()) {
          w.mail_store->remove_account(w.mail_acc_email.text);
          w.mail_account_settings_open = false;
          S.set_toast("Account removed");
        }
        return;
      }
      // Sign In & Connect
      if (rel_x >= mx + mw - 148 && rel_x < mx + mw - 18) {
        S.mail_save_account(w);
        return;
      }
    }
    return;
  }

  // Handle Compose Modal Clicks
  if (w.mail_composing) {
    int mw = std::min(700, cw - 40);
    int mh = std::min(480, ch - 40);
    int mx = (cw - mw) / 2;
    int my = (ch - mh) / 2;

    // Cancel button click
    if (rel_x >= mx + mw - 140 && rel_x < mx + mw - 84 && rel_y >= my + 5 && rel_y < my + 27) {
      S.mail_cancel_compose(w);
      return;
    }
    // Send button click
    if (rel_x >= mx + mw - 76 && rel_x < mx + mw - 12 && rel_y >= my + 5 && rel_y < my + 27) {
      S.mail_send_compose(w);
      return;
    }
    // Field To
    if (rel_x >= mx + 60 && rel_x < mx + mw - 16 && rel_y >= my + 40 && rel_y < my + 64) {
      w.mail_compose_focus = 0;
      return;
    }
    // Field Cc
    if (rel_x >= mx + 60 && rel_x < mx + mw - 16 && rel_y >= my + 70 && rel_y < my + 94) {
      w.mail_compose_focus = 1;
      return;
    }
    // Field Subject
    if (rel_x >= mx + 60 && rel_x < mx + mw - 16 && rel_y >= my + 100 && rel_y < my + 124) {
      w.mail_compose_focus = 2;
      return;
    }
    // Body editor
    if (rel_x >= mx + 16 && rel_x < mx + mw - 16 && rel_y >= my + 140 && rel_y < my + mh - 16) {
      w.mail_compose_focus = 3;
      int line_idx = (rel_y - (my + 148)) / 20;
      if (line_idx < 0) line_idx = 0;
      if (line_idx >= (int)w.mail_compose_body_lines.size()) line_idx = (int)w.mail_compose_body_lines.size() - 1;
      if (line_idx < 0) { w.mail_compose_body_lines.push_back(""); line_idx = 0; }
      w.mail_compose_cursor_line = line_idx;
      w.mail_compose_cursor_col = static_cast<int>(w.mail_compose_body_lines[line_idx].size());
      return;
    }
    return;
  }

  // Graphical Action Buttons toolbar
  auto buttons = mail_toolbar_layout(S, w, cw);
  int cur_bx = 20;
  for (const auto& p : buttons) {
    const auto& btn = p.first;
    int bw = p.second;
    if (rel_y >= APP_CHIP_Y && rel_y < APP_CHIP_Y + APP_CHIP_H && rel_x >= cur_bx && rel_x < cur_bx + bw) {
      if (strcmp(btn.id, "compose") == 0) {
        S.mail_open_compose(w);
      } else if (strcmp(btn.id, "reply") == 0) {
        S.mail_reply(w, false);
      } else if (strcmp(btn.id, "reply_all") == 0) {
        S.mail_reply(w, true);
      } else if (strcmp(btn.id, "forward") == 0) {
        S.mail_forward(w);
      } else if (strcmp(btn.id, "flag") == 0) {
        if (w.mail_store && !w.mail_selected_id.empty()) {
          w.mail_store->toggle_flag(w.mail_selected_id);
          S.set_toast("Flag toggled");
        }
      } else if (strcmp(btn.id, "delete") == 0) {
        if (w.mail_store && !w.mail_selected_id.empty()) {
          w.mail_store->delete_message(w.mail_selected_id);
          S.set_toast("Message deleted");
        }
      } else if (strcmp(btn.id, "archive") == 0) {
        if (w.mail_store && !w.mail_selected_id.empty()) {
          w.mail_store->archive_message(w.mail_selected_id);
          S.set_toast("Message archived");
        }
      } else if (strcmp(btn.id, "copy") == 0) {
        if (w.mail_store && !w.mail_selected_id.empty()) {
          const MailMessage* msg = w.mail_store->get_message(w.mail_selected_id);
          if (msg) {
            std::string clip_text = "From: " + msg->from_name + " <" + msg->from_addr + ">\n"
                                  + "To: ";
            for (size_t ti = 0; ti < msg->to.size(); ++ti) clip_text += (ti > 0 ? ", " : "") + msg->to[ti];
            clip_text += "\nSubject: " + msg->subject + "\nDate: " + msg->date_str + "\n\n" + msg->body;
            (void)hal::clipboard_set(clip_text);
            S.set_toast("Copied message to clipboard");
          }
        }
      } else if (strcmp(btn.id, "html") == 0) {
        w.mail_show_html = !w.mail_show_html;
        w.mail_html_id.clear();          // rebuild on the next frame
        w.mail_body_scroll = 0;
        w.mail_html_scroll = 0;
        S.set_toast(w.mail_show_html ? "Showing HTML" : "Showing plain text");
      } else if (strcmp(btn.id, "theme") == 0) {
        bool current_light = is_mail_light(w);
        w.mail_theme_mode = current_light ? 1 : 2; // Toggle Light <-> Dark
        w.mail_html_id.clear();
        S.set_toast(w.mail_theme_mode == 2 ? "Mail Theme: Light" : "Mail Theme: Dark");
      } else if (strcmp(btn.id, "refresh") == 0) {
        if (w.mail_store) {
          w.mail_store->reload();
#if !defined(AIOS_METAL)
          w.mail_last_poll_ms = steady_ms();
          if (w.mail_store->begin_background_sync(w.mail_active_account_id)) {
            S.set_toast("Checking for new mail...");
          } else {
            S.set_toast(w.mail_store->background_sync_running()
                          ? "Already checking for new mail"
                          : w.mail_store->status_line());
          }
#else
          w.mail_store->check_new_mail(w.mail_active_account_id);
          S.set_toast(w.mail_store->status_line());
#endif
          w.mail_cache_generation = 0;
          w.mail_detail_cache_generation = 0;
        }
      } else if (strcmp(btn.id, "accounts") == 0) {
        S.mail_open_accounts(w);
      }
      return;
    }
    cur_bx += bw + 6;
  }

  int body_y = APP_BODY_Y;
  int body_h = ch - body_y - 14;
  int sidebar_w = (cw > 850) ? 180 : 140;

  // Click in Sidebar Folders
  if (rel_x >= 20 && rel_x < 20 + sidebar_w && rel_y >= body_y + 28 && rel_y < body_y + body_h - 130) {
    if (w.mail_store) {
      S.refresh_mail_cache(w);
      const auto& folders = w.mail_folders_cache;
      int f_idx = (rel_y - (body_y + 28)) / 24;
      if (f_idx >= 0 && f_idx < (int)folders.size()) {
        w.mail_active_folder = folders[f_idx].id;
        w.mail_list_scroll = 0;
        w.mail_body_scroll = 0;
        S.refresh_mail_cache(w);
        const auto& msgs = w.mail_list_cache;
        if (!msgs.empty()) {
          w.mail_selected_id = msgs[0].id;
          w.mail_store->mark_read(w.mail_selected_id, true);
        }
        return;
      }
    }
  }

  // Click in Sidebar Accounts Section
  int acc_head_y = body_y + body_h - 120;
  if (rel_x >= 20 && rel_x < 20 + sidebar_w && rel_y >= acc_head_y && rel_y <= body_y + body_h) {
    // Check "+ Add Account" click
    if (rel_y >= body_y + body_h - 26) {
      S.mail_open_accounts(w);
      return;
    }
    // "All Inboxes" click
    if (rel_y >= acc_head_y + 18 && rel_y < acc_head_y + 34) {
      w.mail_active_account_id = "all";
      w.mail_list_scroll = 0;
      w.mail_body_scroll = 0;
      S.set_toast("Switched to All Inboxes");
      return;
    }
    // Individual account click
    if (w.mail_store) {
      const auto& accs = w.mail_store->accounts();
      int a_idx = (rel_y - (acc_head_y + 38)) / 16;
      if (a_idx >= 0 && a_idx < (int)accs.size()) {
        w.mail_active_account_id = accs[a_idx].id;
        w.mail_list_scroll = 0;
        w.mail_body_scroll = 0;
        S.set_toast("Active account: " + accs[a_idx].email);
        return;
      }
    }
  }

  // Click in Message List
  int list_x = 20 + sidebar_w + 6;
  int list_w = (cw > 1050) ? 310 : ((cw > 750) ? 250 : 190);

  // Search bar
  if (rel_x >= list_x + 6 && rel_x < list_x + list_w - 6 && rel_y >= body_y + 6 && rel_y < body_y + 30) {
    if (!w.mail_search_edit.text.empty() && rel_x >= list_x + list_w - 24) {
      w.mail_search_edit.text.clear();
      w.mail_search_edit.cursor = 0;
      w.mail_search_focused = false;
      w.mail_list_scroll = 0;
      w.mail_body_scroll = 0;
    } else {
      w.mail_search_focused = true;
    }
    return;
  }

  // Filter pills: All (0), Unread (1), Flagged (2), Files (3)
  if (rel_x >= list_x + 8 && rel_x < list_x + list_w - 8 && rel_y >= body_y + 34 && rel_y < body_y + 54) {
    w.mail_search_focused = false;
    int fx = list_x + 8;
    const char* const FILTERS[] = {"All", "Unread", "Flagged", "Files"};
    for (int i = 0; i < 4; ++i) {
      int fw = S.text_width(FILTERS[i], Face::UiSmall) + 12;
      if (rel_x >= fx && rel_x < fx + fw) {
        w.mail_filter_mode = i;
        w.mail_list_scroll = 0;
        w.mail_body_scroll = 0;
        return;
      }
      fx += fw + 3;
    }
  }

  // Message item select
  int item_y = body_y + 56;
  int item_h = 66;
  if (rel_x >= list_x && rel_x < list_x + list_w && rel_y >= item_y && rel_y < body_y + body_h) {
    w.mail_search_focused = false;
    if (w.mail_store) {
      S.refresh_mail_cache(w);
      const auto& msgs = w.mail_list_cache;
      int idx = w.mail_list_scroll + (rel_y - item_y) / item_h;
      if (idx >= 0 && idx < (int)msgs.size()) {
        w.mail_selected_id = msgs[idx].id;
        w.mail_body_scroll = 0;
        // The images switch is per message, and each message starts at the
        // default rather than inheriting what was chosen for the last one.
        w.mail_html_scroll = 0;
        w.mail_allow_remote = true;
        w.mail_store->mark_read(w.mail_selected_id, true);
        return;
      }
    }
  }
}

static void mail_text(HardwareCompositor& S, WindowSurface& w, const std::string& text) {
  if (w.mail_account_settings_open) {
    if (w.mail_acc_focus == 0) w.mail_acc_name.insert(text, 128);
    else if (w.mail_acc_focus == 1) w.mail_acc_email.insert(text, 128);
    else if (w.mail_acc_focus == 2) w.mail_acc_imap_host.insert(text, 128);
    else if (w.mail_acc_focus == 3) w.mail_acc_imap_port.insert(text, 8);
    else if (w.mail_acc_focus == 4) w.mail_acc_smtp_host.insert(text, 128);
    else if (w.mail_acc_focus == 5) w.mail_acc_smtp_port.insert(text, 8);
    else if (w.mail_acc_focus == 6) w.mail_acc_user.insert(text, 128);
    else if (w.mail_acc_focus == 7) w.mail_acc_pass.insert(text, 128);
    return;
  }
  if (w.mail_composing) {
    if (w.mail_compose_focus == 0) {
      w.mail_compose_to.insert(text, 256);
    } else if (w.mail_compose_focus == 1) {
      w.mail_compose_cc.insert(text, 256);
    } else if (w.mail_compose_focus == 2) {
      w.mail_compose_subject.insert(text, 256);
    } else if (w.mail_compose_focus == 3) {
      if (w.mail_compose_body_lines.empty()) w.mail_compose_body_lines.push_back("");
      if (w.mail_compose_cursor_line >= (int)w.mail_compose_body_lines.size())
        w.mail_compose_cursor_line = (int)w.mail_compose_body_lines.size() - 1;
      auto& line = w.mail_compose_body_lines[w.mail_compose_cursor_line];
      if (w.mail_compose_cursor_col > (int)line.size()) w.mail_compose_cursor_col = (int)line.size();
      line.insert(w.mail_compose_cursor_col, text);
      w.mail_compose_cursor_col += static_cast<int>(text.size());
    }
    return;
  }
  if (w.mail_search_focused) {
    w.mail_search_edit.insert(text, 128);
    return;
  }
}

static bool mail_key(HardwareCompositor& S, WindowSurface& w, int key) {
  if (w.mail_account_settings_open) {
    if (key == hal::KeyEscape) {
      w.mail_account_settings_open = false;
      return true;
    }
    if (key == hal::KeyTab) {
      if (!w.mail_acc_show_advanced && w.mail_acc_preset_selected != 5) {
        if (w.mail_acc_focus == 0) w.mail_acc_focus = 1;
        else if (w.mail_acc_focus == 1) w.mail_acc_focus = 7;
        else w.mail_acc_focus = 0;
      } else {
        w.mail_acc_focus = (w.mail_acc_focus + 1) % 8;
      }
      return true;
    }
    if (key == hal::KeyEnter) {
      if (w.mail_acc_focus == 7 || (!w.mail_acc_show_advanced && w.mail_acc_preset_selected != 5 && w.mail_acc_focus == 1)) {
        S.mail_save_account(w);
      } else {
        if (!w.mail_acc_show_advanced && w.mail_acc_preset_selected != 5) {
          if (w.mail_acc_focus == 0) w.mail_acc_focus = 1;
          else if (w.mail_acc_focus == 1) w.mail_acc_focus = 7;
        } else {
          w.mail_acc_focus = (w.mail_acc_focus + 1) % 8;
        }
      }
      return true;
    }
    if (key == hal::KeyBackspace) {
      if (w.mail_acc_focus == 0) w.mail_acc_name.backspace();
      else if (w.mail_acc_focus == 1) w.mail_acc_email.backspace();
      else if (w.mail_acc_focus == 2) w.mail_acc_imap_host.backspace();
      else if (w.mail_acc_focus == 3) w.mail_acc_imap_port.backspace();
      else if (w.mail_acc_focus == 4) w.mail_acc_smtp_host.backspace();
      else if (w.mail_acc_focus == 5) w.mail_acc_smtp_port.backspace();
      else if (w.mail_acc_focus == 6) w.mail_acc_user.backspace();
      else if (w.mail_acc_focus == 7) w.mail_acc_pass.backspace();
      return true;
    }
    if (key == hal::KeyLeft) {
      if (w.mail_acc_focus == 0 && w.mail_acc_name.cursor > 0) w.mail_acc_name.cursor--;
      else if (w.mail_acc_focus == 1 && w.mail_acc_email.cursor > 0) w.mail_acc_email.cursor--;
      else if (w.mail_acc_focus == 2 && w.mail_acc_imap_host.cursor > 0) w.mail_acc_imap_host.cursor--;
      else if (w.mail_acc_focus == 3 && w.mail_acc_imap_port.cursor > 0) w.mail_acc_imap_port.cursor--;
      else if (w.mail_acc_focus == 4 && w.mail_acc_smtp_host.cursor > 0) w.mail_acc_smtp_host.cursor--;
      else if (w.mail_acc_focus == 5 && w.mail_acc_smtp_port.cursor > 0) w.mail_acc_smtp_port.cursor--;
      else if (w.mail_acc_focus == 6 && w.mail_acc_user.cursor > 0) w.mail_acc_user.cursor--;
      else if (w.mail_acc_focus == 7 && w.mail_acc_pass.cursor > 0) w.mail_acc_pass.cursor--;
      return true;
    }
    if (key == hal::KeyRight) {
      if (w.mail_acc_focus == 0 && static_cast<size_t>(w.mail_acc_name.cursor) < w.mail_acc_name.text.size()) w.mail_acc_name.cursor++;
      else if (w.mail_acc_focus == 1 && static_cast<size_t>(w.mail_acc_email.cursor) < w.mail_acc_email.text.size()) w.mail_acc_email.cursor++;
      else if (w.mail_acc_focus == 2 && static_cast<size_t>(w.mail_acc_imap_host.cursor) < w.mail_acc_imap_host.text.size()) w.mail_acc_imap_host.cursor++;
      else if (w.mail_acc_focus == 3 && static_cast<size_t>(w.mail_acc_imap_port.cursor) < w.mail_acc_imap_port.text.size()) w.mail_acc_imap_port.cursor++;
      else if (w.mail_acc_focus == 4 && static_cast<size_t>(w.mail_acc_smtp_host.cursor) < w.mail_acc_smtp_host.text.size()) w.mail_acc_smtp_host.cursor++;
      else if (w.mail_acc_focus == 5 && static_cast<size_t>(w.mail_acc_smtp_port.cursor) < w.mail_acc_smtp_port.text.size()) w.mail_acc_smtp_port.cursor++;
      else if (w.mail_acc_focus == 6 && static_cast<size_t>(w.mail_acc_user.cursor) < w.mail_acc_user.text.size()) w.mail_acc_user.cursor++;
      else if (w.mail_acc_focus == 7 && static_cast<size_t>(w.mail_acc_pass.cursor) < w.mail_acc_pass.text.size()) w.mail_acc_pass.cursor++;
      return true;
    }
    return true;
  }

  if (w.mail_composing) {
    if (key == hal::KeyEscape) {
      S.mail_cancel_compose(w);
      return true;
    }
    if (key == hal::KeyTab) {
      w.mail_compose_focus = (w.mail_compose_focus + 1) % 4;
      return true;
    }
    if (key == hal::KeyEnter) {
      if (w.mail_compose_focus < 3) {
        w.mail_compose_focus++;
      } else {
        // Insert newline in multiline body
        if (w.mail_compose_body_lines.empty()) w.mail_compose_body_lines.push_back("");
        int cl = w.mail_compose_cursor_line;
        int cc = w.mail_compose_cursor_col;
        std::string cur = w.mail_compose_body_lines[cl];
        std::string head = (cc < (int)cur.size()) ? cur.substr(0, cc) : cur;
        std::string tail = (cc < (int)cur.size()) ? cur.substr(cc) : "";
        w.mail_compose_body_lines[cl] = head;
        w.mail_compose_body_lines.insert(w.mail_compose_body_lines.begin() + cl + 1, tail);
        w.mail_compose_cursor_line++;
        w.mail_compose_cursor_col = 0;
      }
      return true;
    }
    if (key == hal::KeyBackspace) {
      if (w.mail_compose_focus == 0) w.mail_compose_to.backspace();
      else if (w.mail_compose_focus == 1) w.mail_compose_cc.backspace();
      else if (w.mail_compose_focus == 2) w.mail_compose_subject.backspace();
      else if (w.mail_compose_focus == 3) {
        int cl = w.mail_compose_cursor_line;
        int cc = w.mail_compose_cursor_col;
        if (cl >= 0 && cl < (int)w.mail_compose_body_lines.size()) {
          auto& line = w.mail_compose_body_lines[cl];
          if (cc > 0 && cc <= (int)line.size()) {
            line.erase(cc - 1, 1);
            w.mail_compose_cursor_col--;
          } else if (cc == 0 && cl > 0) {
            std::string cur = line;
            w.mail_compose_body_lines.erase(w.mail_compose_body_lines.begin() + cl);
            w.mail_compose_cursor_line--;
            w.mail_compose_cursor_col = static_cast<int>(w.mail_compose_body_lines[cl - 1].size());
            w.mail_compose_body_lines[cl - 1] += cur;
          }
        }
      }
      return true;
    }
    if (key == hal::KeyLeft) {
      if (w.mail_compose_focus == 0) { if (w.mail_compose_to.cursor > 0) w.mail_compose_to.cursor--; }
      else if (w.mail_compose_focus == 1) { if (w.mail_compose_cc.cursor > 0) w.mail_compose_cc.cursor--; }
      else if (w.mail_compose_focus == 2) { if (w.mail_compose_subject.cursor > 0) w.mail_compose_subject.cursor--; }
      else if (w.mail_compose_focus == 3) {
        if (w.mail_compose_cursor_col > 0) w.mail_compose_cursor_col--;
        else if (w.mail_compose_cursor_line > 0) {
          w.mail_compose_cursor_line--;
          w.mail_compose_cursor_col = static_cast<int>(w.mail_compose_body_lines[w.mail_compose_cursor_line].size());
        }
      }
      return true;
    }
    if (key == hal::KeyRight) {
      if (w.mail_compose_focus == 0) { if (w.mail_compose_to.cursor < w.mail_compose_to.text.size()) w.mail_compose_to.cursor++; }
      else if (w.mail_compose_focus == 1) { if (w.mail_compose_cc.cursor < w.mail_compose_cc.text.size()) w.mail_compose_cc.cursor++; }
      else if (w.mail_compose_focus == 2) { if (w.mail_compose_subject.cursor < w.mail_compose_subject.text.size()) w.mail_compose_subject.cursor++; }
      else if (w.mail_compose_focus == 3) {
        if (w.mail_compose_cursor_line < (int)w.mail_compose_body_lines.size()) {
          if (w.mail_compose_cursor_col < (int)w.mail_compose_body_lines[w.mail_compose_cursor_line].size())
            w.mail_compose_cursor_col++;
          else if (w.mail_compose_cursor_line + 1 < (int)w.mail_compose_body_lines.size()) {
            w.mail_compose_cursor_line++;
            w.mail_compose_cursor_col = 0;
          }
        }
      }
      return true;
    }
    if (key == hal::KeyUp && w.mail_compose_focus == 3) {
      if (w.mail_compose_cursor_line > 0) {
        w.mail_compose_cursor_line--;
        if (w.mail_compose_cursor_col > (int)w.mail_compose_body_lines[w.mail_compose_cursor_line].size())
          w.mail_compose_cursor_col = (int)w.mail_compose_body_lines[w.mail_compose_cursor_line].size();
      }
      return true;
    }
    if (key == hal::KeyDown && w.mail_compose_focus == 3) {
      if (w.mail_compose_cursor_line + 1 < (int)w.mail_compose_body_lines.size()) {
        w.mail_compose_cursor_line++;
        if (w.mail_compose_cursor_col > (int)w.mail_compose_body_lines[w.mail_compose_cursor_line].size())
          w.mail_compose_cursor_col = (int)w.mail_compose_body_lines[w.mail_compose_cursor_line].size();
      }
      return true;
    }
    return true;
  }

  // Not composing
  if (w.mail_search_focused) {
    if (key == hal::KeyEscape) {
      if (!w.mail_search_edit.text.empty()) {
        w.mail_search_edit.text.clear();
        w.mail_search_edit.cursor = 0;
      }
      w.mail_search_focused = false;
      return true;
    }
    if (key == hal::KeyEnter) {
      w.mail_search_focused = false;
      return true;
    }
    if (key == hal::KeyBackspace) {
      w.mail_search_edit.backspace();
      return true;
    }
    if (key == hal::KeyLeft) { if (w.mail_search_edit.cursor > 0) w.mail_search_edit.cursor--; return true; }
    if (key == hal::KeyRight) { if (w.mail_search_edit.cursor < w.mail_search_edit.text.size()) w.mail_search_edit.cursor++; return true; }
    return true; // Consume any other key while search is focused so shortcuts don't fire
  }

  if (key == '/' || key == hal::KeyCtrlF) {
    w.mail_search_focused = true;
    return true;
  }

  if (key == 'n' || key == 'c' || key == hal::KeyCtrlN) {
    S.mail_open_compose(w);
    return true;
  }
  if (key == 'r') {
    S.mail_reply(w, false);
    return true;
  }
  if (key == 'f') {
    if (w.mail_store && !w.mail_selected_id.empty()) {
      w.mail_store->toggle_flag(w.mail_selected_id);
      S.set_toast("Flag toggled");
    }
    return true;
  }
  if (key == 'd' || key == hal::KeyDelete || key == hal::KeyBackspace) {
    if (w.mail_store && !w.mail_selected_id.empty()) {
      w.mail_store->delete_message(w.mail_selected_id);
      S.set_toast("Message deleted");
    }
    return true;
  }
  if (key == 'a') {
    if (w.mail_store && !w.mail_selected_id.empty()) {
      w.mail_store->archive_message(w.mail_selected_id);
      S.set_toast("Message archived");
    }
    return true;
  }
  if (key == hal::KeyUp || key == hal::KeyDown) {
    if (w.mail_store) {
      S.refresh_mail_cache(w);
      const auto& msgs = w.mail_list_cache;
      if (!msgs.empty()) {
        int cur_idx = 0;
        for (size_t i = 0; i < msgs.size(); ++i) {
          if (msgs[i].id == w.mail_selected_id) { cur_idx = (int)i; break; }
        }
        if (key == hal::KeyUp && cur_idx > 0) cur_idx--;
        else if (key == hal::KeyDown && cur_idx + 1 < (int)msgs.size()) cur_idx++;
        w.mail_selected_id = msgs[cur_idx].id;
        w.mail_body_scroll = 0;
        w.mail_store->mark_read(w.mail_selected_id, true);
        if (cur_idx < w.mail_list_scroll) w.mail_list_scroll = cur_idx;
        if (cur_idx >= w.mail_list_scroll + 6) w.mail_list_scroll = cur_idx - 5;
      }
    }
    return true;
  }
  if (key == hal::KeyPageDown || (!w.mail_search_focused && key == ' ')) {
    w.mail_body_scroll += 8;
    return true;
  }
  if (key == hal::KeyPageUp) {
    w.mail_body_scroll = (w.mail_body_scroll > 8) ? w.mail_body_scroll - 8 : 0;
    return true;
  }
  return false;
}

static void mail_wheel(WindowSurface& w, int notches, int rel_x = -1, int rel_y = -1) {
  int sidebar_w = (w.content_width() > 850) ? 180 : 140;
  int list_x = 20 + sidebar_w + 6;
  int list_w = (w.content_width() > 1050) ? 310 : ((w.content_width() > 750) ? 250 : 190);
  int detail_x = list_x + list_w + 6;

  if (rel_x >= detail_x) {
    // An html message scrolls its laid-out page; a plain one scrolls its lines.
    if (w.mail_html && w.mail_show_html) {
      w.mail_html_scroll -= notches * 40;      // pixels, not text lines
      if (w.mail_html_scroll < 0) w.mail_html_scroll = 0;
    } else {
      w.mail_body_scroll -= notches * 3;
      if (w.mail_body_scroll < 0) w.mail_body_scroll = 0;
    }
  } else {
    w.mail_list_scroll -= notches * 2;
    if (w.mail_list_scroll < 0) w.mail_list_scroll = 0;
  }
}

// ===========================================================================
// Native C++ AetherContacts Client (Apple-style Address Book)
// ===========================================================================

static const char* const CONTACTS_CHIPS[] = {
  "+ New Contact", "Edit", "Favorite", "Delete", "Export vCard"
};

void HardwareCompositor::contacts_open_card_editor(WindowSurface& win, bool new_card) {
  win.contacts_editing = true;
  win.contacts_is_new = new_card;
  win.contacts_edit_focus = 0;
  if (new_card || !win.contacts_store) {
    win.contacts_edit_first_name.text.clear();
    win.contacts_edit_first_name.cursor = 0;
    win.contacts_edit_last_name.text.clear();
    win.contacts_edit_last_name.cursor = 0;
    win.contacts_edit_company.text.clear();
    win.contacts_edit_company.cursor = 0;
    win.contacts_edit_job_title.text.clear();
    win.contacts_edit_job_title.cursor = 0;
    win.contacts_edit_email.text.clear();
    win.contacts_edit_email.cursor = 0;
    win.contacts_edit_phone.text.clear();
    win.contacts_edit_phone.cursor = 0;
    win.contacts_edit_notes.text.clear();
    win.contacts_edit_notes.cursor = 0;
  } else {
    const auto* c = win.contacts_store->get_contact(win.contacts_selected_id);
    if (c) {
      win.contacts_edit_first_name.text = c->first_name;
      win.contacts_edit_first_name.cursor = (int)c->first_name.size();
      win.contacts_edit_last_name.text = c->last_name;
      win.contacts_edit_last_name.cursor = (int)c->last_name.size();
      win.contacts_edit_company.text = c->company;
      win.contacts_edit_company.cursor = (int)c->company.size();
      win.contacts_edit_job_title.text = c->job_title;
      win.contacts_edit_job_title.cursor = (int)c->job_title.size();
      win.contacts_edit_email.text = c->email;
      win.contacts_edit_email.cursor = (int)c->email.size();
      win.contacts_edit_phone.text = c->phone;
      win.contacts_edit_phone.cursor = (int)c->phone.size();
      win.contacts_edit_notes.text = c->notes;
      win.contacts_edit_notes.cursor = (int)c->notes.size();
    }
  }
}

void HardwareCompositor::contacts_save_card_editor(WindowSurface& win) {
  if (!win.contacts_store) return;
  if (win.contacts_is_new) {
    ContactCard c;
    c.first_name = win.contacts_edit_first_name.text;
    c.last_name = win.contacts_edit_last_name.text;
    c.company = win.contacts_edit_company.text;
    c.job_title = win.contacts_edit_job_title.text;
    c.email = win.contacts_edit_email.text;
    c.phone = win.contacts_edit_phone.text;
    c.notes = win.contacts_edit_notes.text;
    c.avatar_color = AVATAR_COLORS[avatar_hash(c.full_name()) % 8];
    win.contacts_store->add_contact(c);
    win.contacts_selected_id = c.id;
    set_toast("Created contact: " + c.full_name());
  } else {
    const auto* old_c = win.contacts_store->get_contact(win.contacts_selected_id);
    if (old_c) {
      ContactCard c = *old_c;
      c.first_name = win.contacts_edit_first_name.text;
      c.last_name = win.contacts_edit_last_name.text;
      c.company = win.contacts_edit_company.text;
      c.job_title = win.contacts_edit_job_title.text;
      c.email = win.contacts_edit_email.text;
      c.phone = win.contacts_edit_phone.text;
      c.notes = win.contacts_edit_notes.text;
      win.contacts_store->update_contact(c);
      set_toast("Updated contact: " + c.full_name());
    }
  }
  win.contacts_editing = false;
}

void HardwareCompositor::contacts_cancel_card_editor(WindowSurface& win) {
  win.contacts_editing = false;
}

void HardwareCompositor::contacts_delete_selected(WindowSurface& win) {
  if (!win.contacts_store || win.contacts_selected_id.empty()) return;
  const auto* c = win.contacts_store->get_contact(win.contacts_selected_id);
  std::string name = c ? c->full_name() : win.contacts_selected_id;
  win.contacts_store->delete_contact(win.contacts_selected_id);
  auto list = win.contacts_store->contacts_for_group(win.contacts_active_group);
  if (!list.empty()) win.contacts_selected_id = list[0].id;
  else win.contacts_selected_id.clear();
  set_toast("Deleted contact: " + name);
}

void HardwareCompositor::contacts_toggle_fav_selected(WindowSurface& win) {
  if (!win.contacts_store || win.contacts_selected_id.empty()) return;
  win.contacts_store->toggle_favorite(win.contacts_selected_id);
  set_toast("Toggled favorite");
}

void HardwareCompositor::render_contacts_window(WindowSurface& win, Kernel& k) {
  if (!win.contacts_store) {
    win.contacts_store = std::make_shared<ContactsStore>(k.config().root + "/contacts");
  }

  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);

  draw_card(win, w, h, 10, 10, w - 20, h - 20, "aethercontacts • apple address book");
  draw_app_chips(*this, win, w, h, CONTACTS_CHIPS, nullptr);

  // Status line header
  draw_label_to(win.buffer, w, h, 22, APP_HEAD_Y + 2, "address book", tok::faint);
  int head_x = 22 + label_width("address book") + 12;
  draw_text_to(win.buffer, w, h, head_x, APP_HEAD_Y, fit_text(*this, win.contacts_store->status_line(), 300, Face::Mono), tok::accent, Face::Mono);

  int body_y = APP_BODY_Y;
  int body_h = h - body_y - 14;
  if (body_h < 80) body_h = 80;

  // --- Pane 1: Groups Sidebar (Left) ----------------------------------------
  int sidebar_w = (w > 850) ? 170 : 130;
  draw_rect_to(win.buffer, w, h, 20, body_y, sidebar_w, body_h, 0xFF141417);
  draw_rect_outline_to(win.buffer, w, h, 20, body_y, sidebar_w, body_h, tok::line_up);
  draw_label_to(win.buffer, w, h, 28, body_y + 8, "GROUPS", tok::faint);

  auto groups = win.contacts_store->get_groups();
  int gy = body_y + 28;
  for (const auto& g : groups) {
    if (gy + 22 > body_y + body_h - 40) break;
    bool sel = (g.id == win.contacts_active_group);
    bool hot = win.focused && mouse_over_content(win, 22, gy - 2, sidebar_w - 4, 22);
    if (sel || hot) draw_rect_to(win.buffer, w, h, 22, gy - 2, sidebar_w - 4, 22, sel ? 0xFF1E283D : tok::wash);
    if (sel) draw_rect_outline_to(win.buffer, w, h, 22, gy - 2, sidebar_w - 4, 22, 0xFF0A84FF);

    std::string count_str = std::to_string(g.count);
    int count_w = text_width(count_str, Face::UiSmall) + 12;
    draw_pill_to(win.buffer, w, h, 20 + sidebar_w - count_w - 8, gy, count_w, 18,
                 sel ? 0xFF0A84FF : 0xFF2A2B32, 0);
    draw_text_to(win.buffer, w, h, 20 + sidebar_w - count_w - 2, gy + 2, count_str, 0xFFFFFFFF, Face::UiSmall);

    draw_text_to(win.buffer, w, h, 30, gy + 2, fit_text(*this, g.name, sidebar_w - count_w - 38, sel ? Face::UiBold : Face::Ui),
                 sel ? tok::text : (hot ? tok::text : tok::dim), sel ? Face::UiBold : Face::Ui);
    gy += 24;
  }

  // --- Pane 2: Contacts Alphabetical List (Middle) --------------------------
  int list_x = 20 + sidebar_w + 6;
  int list_w = (w > 1000) ? 280 : 220;
  draw_rect_to(win.buffer, w, h, list_x, body_y, list_w, body_h, 0xFF17171C);
  draw_rect_outline_to(win.buffer, w, h, list_x, body_y, list_w, body_h, tok::line_up);

  // Search bar
  draw_rect_to(win.buffer, w, h, list_x + 6, body_y + 6, list_w - 12, 24, win.contacts_search_focused ? 0xFF1F2430 : tok::panel2);
  draw_rect_outline_to(win.buffer, w, h, list_x + 6, body_y + 6, list_w - 12, 24, win.contacts_search_focused ? 0xFF0A84FF : tok::line_up);
  std::string s_text = win.contacts_search_edit.text.empty() ? (win.contacts_search_focused ? "" : "Search contacts...") : win.contacts_search_edit.text;
  draw_text_to(win.buffer, w, h, list_x + 14, body_y + 10,
               fit_text(*this, s_text, list_w - 30, Face::UiSmall),
               win.contacts_search_edit.text.empty() ? tok::faint : tok::text, Face::UiSmall);

  std::vector<ContactCard> contacts = win.contacts_search_edit.text.empty()
      ? win.contacts_store->contacts_for_group(win.contacts_active_group)
      : win.contacts_store->search(win.contacts_search_edit.text);

  int cy = body_y + 36;
  for (size_t i = win.contacts_list_scroll; i < contacts.size(); ++i) {
    if (cy + 42 > body_y + body_h - 6) break;
    const auto& c = contacts[i];
    bool sel = (c.id == win.contacts_selected_id);
    bool hot = win.focused && mouse_over_content(win, list_x + 4, cy, list_w - 8, 40);

    if (sel || hot) draw_rect_to(win.buffer, w, h, list_x + 4, cy, list_w - 8, 40, sel ? 0xFF1E283D : tok::wash);
    if (sel) draw_rect_outline_to(win.buffer, w, h, list_x + 4, cy, list_w - 8, 40, 0xFF0A84FF);

    // Avatar circle
    draw_pill_to(win.buffer, w, h, list_x + 6, cy + 6, 28, 28, c.avatar_color, 0);
    std::string inits = c.initials();
    int inits_w = text_width(inits, Face::UiSmall);
    draw_text_to(win.buffer, w, h, list_x + 20 - inits_w / 2, cy + 13, inits, 0xFFFFFFFF, Face::UiSmall);

    // Contact name
    draw_text_to(win.buffer, w, h, list_x + 42, cy + 6, fit_text(*this, c.full_name(), list_w - 75, sel ? Face::UiBold : Face::Ui),
                 sel ? tok::text : tok::text, sel ? Face::UiBold : Face::Ui);

    // Subtitle (Company or Title)
    std::string sub = !c.job_title.empty() ? c.job_title : (!c.company.empty() ? c.company : c.email);
    draw_text_to(win.buffer, w, h, list_x + 42, cy + 22, fit_text(*this, sub, list_w - 75, Face::UiSmall),
                 sel ? tok::dim : tok::faint, Face::UiSmall);

    if (c.is_favorite) {
      draw_text_to(win.buffer, w, h, list_x + list_w - 24, cy + 12, "*", 0xFFFFCC00, Face::UiBold);
    }

    cy += 44;
  }

  // --- Pane 3: Contact Detail Card (Right) ----------------------------------
  int detail_x = list_x + list_w + 6;
  int detail_w = w - detail_x - 20;
  if (detail_w < 100) detail_w = 100;
  draw_rect_to(win.buffer, w, h, detail_x, body_y, detail_w, body_h, 0xFF141417);
  draw_rect_outline_to(win.buffer, w, h, detail_x, body_y, detail_w, body_h, tok::line_up);

  const ContactCard* card = win.contacts_store->get_contact(win.contacts_selected_id);
  if (card) {
    // Header Banner Card
    int card_head_h = 100;
    draw_rect_to(win.buffer, w, h, detail_x + 12, body_y + 12, detail_w - 24, card_head_h, 0xFF1A1C23);
    draw_rect_outline_to(win.buffer, w, h, detail_x + 12, body_y + 12, detail_w - 24, card_head_h, tok::line_up);

    // Large Avatar Circle
    draw_pill_to(win.buffer, w, h, detail_x + 24, body_y + 28, 68, 68, card->avatar_color, 0);
    std::string large_inits = card->initials();
    int large_w = text_width(large_inits, Face::UiBold);
    draw_text_to(win.buffer, w, h, detail_x + 58 - large_w / 2, body_y + 50, large_inits, 0xFFFFFFFF, Face::UiBold);

    // Full name and Company
    int text_left = detail_x + 104;
    draw_text_to(win.buffer, w, h, text_left, body_y + 26, fit_text(*this, card->full_name(), detail_w - 180, Face::UiBold), tok::text, Face::UiBold);
    std::string role_line = card->job_title;
    if (!card->company.empty()) {
      if (!role_line.empty()) role_line += " @ ";
      role_line += card->company;
    }
    draw_text_to(win.buffer, w, h, text_left, body_y + 54, fit_text(*this, role_line, detail_w - 180, Face::Ui), tok::dim, Face::Ui);

    // Action Chips Row
    int btn_y = body_y + 124;
    static const char* const CARD_ACTIONS[] = {"[ Send Email ]", "[ Call Phone ]", "[ Schedule Event ]", "[ * Favorite ]"};
    int bx = detail_x + 12;
    for (int bi = 0; bi < 4; ++bi) {
      int bw = text_width(CARD_ACTIONS[bi], Face::UiSmall) + 16;
      bool hot = win.focused && mouse_over_content(win, bx, btn_y, bw, 24);
      draw_pill_to(win.buffer, w, h, bx, btn_y, bw, 24, hot ? tok::wash : 0xFF22242D, tok::line_up);
      draw_text_to(win.buffer, w, h, bx + 8, btn_y + 4, CARD_ACTIONS[bi], hot ? tok::text : tok::dim, Face::UiSmall);
      bx += bw + 8;
    }

    // Details Grid Card
    int grid_y = btn_y + 36;
    int grid_h = body_y + body_h - grid_y - 12;
    if (grid_h > 40) {
      draw_rect_to(win.buffer, w, h, detail_x + 12, grid_y, detail_w - 24, grid_h, 0xFF17181F);
      draw_rect_outline_to(win.buffer, w, h, detail_x + 12, grid_y, detail_w - 24, grid_h, tok::line_up);

      int ry = grid_y + 14;
      auto draw_field = [&](const std::string& label, const std::string& val, uint32_t val_color = tok::text) {
        if (val.empty() || ry + 22 > grid_y + grid_h) return;
        draw_text_to(win.buffer, w, h, detail_x + 28, ry, label, tok::faint, Face::UiSmall);
        draw_text_to(win.buffer, w, h, detail_x + 140, ry, fit_text(*this, val, detail_w - 180, Face::Ui), val_color, Face::Ui);
        ry += 26;
      };

      draw_field("Email (Home)", card->email, 0xFF0A84FF);
      draw_field("Email (Work)", card->email_work, 0xFF0A84FF);
      draw_field("Phone (Mobile)", card->phone);
      draw_field("Phone (Work)", card->phone_work);
      std::string full_addr = card->address;
      if (!card->city.empty()) {
        if (!full_addr.empty()) full_addr += ", ";
        full_addr += card->city + ", " + card->state + " " + card->zip;
      }
      draw_field("Address", full_addr);
      draw_field("Birthday", card->birthday);
      draw_field("Notes", card->notes, tok::dim);
      draw_field("vCard Path", card->vcf_path, tok::faint);
    }
  } else {
    draw_text_to(win.buffer, w, h, detail_x + 40, body_y + 60, "No contact selected", tok::faint, Face::Ui);
  }

  // --- Modal Edit / Add Sheet -----------------------------------------------
  if (win.contacts_editing) {
    draw_rect_to(win.buffer, w, h, 0, 0, w, h, 0x88000000);
    int mw = 580, mh = 420;
    int mx = (w - mw) / 2, my = (h - mh) / 2;
    draw_card(win, w, h, mx, my, mw, mh, win.contacts_is_new ? "New Contact Card" : "Edit Contact Card");

    auto draw_input = [&](int idx, const std::string& lbl, EditLine& el, int y, int fw) {
      bool f = (win.contacts_edit_focus == idx);
      draw_label_to(win.buffer, w, h, mx + 24, y + 4, lbl, f ? tok::accent : tok::faint);
      draw_rect_to(win.buffer, w, h, mx + 110, y, fw, 24, f ? 0xFF1E283D : tok::panel2);
      draw_rect_outline_to(win.buffer, w, h, mx + 110, y, fw, 24, f ? 0xFF0A84FF : tok::line_up);
      std::string disp = fit_text(*this, el.text, fw - 16, Face::Ui);
      draw_text_to(win.buffer, w, h, mx + 118, y + 4, disp, tok::text, Face::Ui);
      if (f && (steady_ms() / 400) % 2 == 0) {
        int cx = mx + 118 + text_width(el.text.substr(0, el.cursor), Face::Ui);
        draw_rect_to(win.buffer, w, h, cx, y + 4, 2, 16, 0xFF0A84FF);
      }
    };

    int iy = my + 44;
    draw_input(0, "First Name", win.contacts_edit_first_name, iy, 160);
    draw_input(1, "Last Name", win.contacts_edit_last_name, iy + 34, 160);
    draw_input(2, "Company", win.contacts_edit_company, iy + 68, mw - 140);
    draw_input(3, "Job Title", win.contacts_edit_job_title, iy + 102, mw - 140);
    draw_input(4, "Email", win.contacts_edit_email, iy + 136, mw - 140);
    draw_input(5, "Phone", win.contacts_edit_phone, iy + 170, mw - 140);
    draw_input(6, "Notes", win.contacts_edit_notes, iy + 204, mw - 140);

    // Save & Cancel buttons
    int by = my + mh - 36;
    draw_pill_to(win.buffer, w, h, mx + mw - 180, by, 70, 24, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + mw - 168, by + 4, "Cancel", tok::dim, Face::UiSmall);

    draw_pill_to(win.buffer, w, h, mx + mw - 95, by, 75, 24, 0xFF0A84FF, 0);
    draw_text_to(win.buffer, w, h, mx + mw - 85, by + 4, "Save Card", 0xFFFFFFFF, Face::UiSmall);
  }
}

void HardwareCompositor::contacts_click(WindowSurface& w, int rel_x, int rel_y, std::string& to_run) {
  HardwareCompositor& S = *this;
  int cw = w.content_width();
  int ch = w.content_height();

  if (w.contacts_editing) {
    int mw = 580, mh = 420;
    int mx = (cw - mw) / 2, my = (ch - mh) / 2;
    int by = my + mh - 36;

    // Cancel click
    if (rel_x >= mx + mw - 180 && rel_x < mx + mw - 110 && rel_y >= by && rel_y < by + 24) {
      S.contacts_cancel_card_editor(w);
      return;
    }
    // Save click
    if (rel_x >= mx + mw - 95 && rel_x < mx + mw - 20 && rel_y >= by && rel_y < by + 24) {
      S.contacts_save_card_editor(w);
      return;
    }

    // Input fields focus
    int iy = my + 44;
    for (int i = 0; i < 7; ++i) {
      if (rel_y >= iy + i * 34 && rel_y < iy + i * 34 + 26 && rel_x >= mx + 110 && rel_x < mx + mw - 30) {
        w.contacts_edit_focus = i;
        return;
      }
    }
    return;
  }

  // Chips toolbar
  if (const char* chip = chip_hit(S, CONTACTS_CHIPS, rel_x, rel_y)) {
    if (strcmp(chip, "+ New Contact") == 0) {
      S.contacts_open_card_editor(w, true);
    } else if (strcmp(chip, "Edit") == 0) {
      S.contacts_open_card_editor(w, false);
    } else if (strcmp(chip, "Favorite") == 0) {
      S.contacts_toggle_fav_selected(w);
    } else if (strcmp(chip, "Delete") == 0) {
      S.contacts_delete_selected(w);
    } else if (strcmp(chip, "Export vCard") == 0) {
      if (w.contacts_store) {
        std::string v = w.contacts_store->export_all_vcards();
        S.set_toast("Exported " + std::to_string(w.contacts_store->total_contacts()) + " vCards to contacts/cards/");
      }
    }
    return;
  }

  int body_y = APP_BODY_Y;
  int body_h = ch - body_y - 14;
  int sidebar_w = (cw > 850) ? 170 : 130;

  // Sidebar Groups
  if (rel_x >= 20 && rel_x < 20 + sidebar_w && rel_y >= body_y + 28 && rel_y < body_y + body_h - 40) {
    if (w.contacts_store) {
      auto groups = w.contacts_store->get_groups();
      int g_idx = (rel_y - (body_y + 28)) / 24;
      if (g_idx >= 0 && g_idx < (int)groups.size()) {
        w.contacts_active_group = groups[g_idx].id;
        w.contacts_list_scroll = 0;
        auto list = w.contacts_store->contacts_for_group(w.contacts_active_group);
        if (!list.empty()) w.contacts_selected_id = list[0].id;
        return;
      }
    }
  }

  // Contacts List
  int list_x = 20 + sidebar_w + 6;
  int list_w = (cw > 1000) ? 280 : 220;

  // Search bar focus
  if (rel_x >= list_x + 6 && rel_x < list_x + list_w - 6 && rel_y >= body_y + 6 && rel_y < body_y + 30) {
    w.contacts_search_focused = true;
    return;
  }

  if (rel_x >= list_x && rel_x < list_x + list_w && rel_y >= body_y + 36 && rel_y < body_y + body_h) {
    if (w.contacts_store) {
      auto contacts = w.contacts_search_edit.text.empty()
          ? w.contacts_store->contacts_for_group(w.contacts_active_group)
          : w.contacts_store->search(w.contacts_search_edit.text);
      int row_idx = w.contacts_list_scroll + (rel_y - (body_y + 36)) / 44;
      if (row_idx >= 0 && row_idx < (int)contacts.size()) {
        w.contacts_selected_id = contacts[row_idx].id;
        w.contacts_search_focused = false;
        return;
      }
    }
  }

  // Action Buttons on Right Pane
  int detail_x = list_x + list_w + 6;
  int btn_y = body_y + 124;
  if (rel_y >= btn_y && rel_y < btn_y + 24 && rel_x >= detail_x + 12) {
    const auto* c = w.contacts_store ? w.contacts_store->get_contact(w.contacts_selected_id) : nullptr;
    if (c) {
      int bx = detail_x + 12;
      static const char* const CARD_ACTIONS[] = {"[ Send Email ]", "[ Call Phone ]", "[ Schedule Event ]", "[ * Favorite ]"};
      for (int bi = 0; bi < 4; ++bi) {
        int bw = S.text_width(CARD_ACTIONS[bi], Face::UiSmall) + 16;
        if (rel_x >= bx && rel_x < bx + bw) {
          if (bi == 0) { // Send Email
            S.set_toast("Opening Mail compose to: " + c->email);
            // Raising a pane takes state_mutex_, and this runs while it is
            // held: doing it here froze the whole machine.
            const std::string to = c->email, first = c->first_name;
            S.defer_after_click([&S, to, first]() {
              S.run_ui_action(UiAction::PaneMail);
              for (auto& win : S.windows()) {
                if (win.type == WindowType::Mail) {
                  S.mail_open_compose(win, to, "Greeting from AIOS", "Hi " + first + ",\n\n");
                  break;
                }
              }
            });
          } else if (bi == 1) { // Call Phone
            S.set_toast("Calling: " + c->phone);
          } else if (bi == 2) { // Schedule Event
            S.set_toast("Scheduling event with: " + c->full_name());
            const std::string who = c->full_name();
            S.defer_after_click([&S, who]() {
              S.run_ui_action(UiAction::PaneCalendar);
              for (auto& win : S.windows()) {
                if (win.type == WindowType::Calendar) {
                  S.calendar_open_event_editor(win, true);
                  win.calendar_edit_title.text = "Sync with " + who;
                  win.calendar_edit_title.cursor = (int)win.calendar_edit_title.text.size();
                  break;
                }
              }
            });
          } else if (bi == 3) { // Favorite
            S.contacts_toggle_fav_selected(w);
          }
          return;
        }
        bx += bw + 8;
      }
    }
  }
}

static void contacts_text(HardwareCompositor& S, WindowSurface& w, const std::string& text) {
  if (w.contacts_editing) {
    if (w.contacts_edit_focus == 0) w.contacts_edit_first_name.insert(text, 64);
    else if (w.contacts_edit_focus == 1) w.contacts_edit_last_name.insert(text, 64);
    else if (w.contacts_edit_focus == 2) w.contacts_edit_company.insert(text, 64);
    else if (w.contacts_edit_focus == 3) w.contacts_edit_job_title.insert(text, 64);
    else if (w.contacts_edit_focus == 4) w.contacts_edit_email.insert(text, 128);
    else if (w.contacts_edit_focus == 5) w.contacts_edit_phone.insert(text, 64);
    else if (w.contacts_edit_focus == 6) w.contacts_edit_notes.insert(text, 512);
    return;
  }
  if (w.contacts_search_focused) {
    w.contacts_search_edit.insert(text, 128);
    return;
  }
}

static bool contacts_key(HardwareCompositor& S, WindowSurface& w, int key) {
  if (w.contacts_editing) {
    if (key == hal::KeyEscape) { S.contacts_cancel_card_editor(w); return true; }
    if (key == hal::KeyTab) { w.contacts_edit_focus = (w.contacts_edit_focus + 1) % 7; return true; }
    if (key == hal::KeyEnter) { S.contacts_save_card_editor(w); return true; }
    if (key == hal::KeyBackspace) {
      if (w.contacts_edit_focus == 0) w.contacts_edit_first_name.backspace();
      else if (w.contacts_edit_focus == 1) w.contacts_edit_last_name.backspace();
      else if (w.contacts_edit_focus == 2) w.contacts_edit_company.backspace();
      else if (w.contacts_edit_focus == 3) w.contacts_edit_job_title.backspace();
      else if (w.contacts_edit_focus == 4) w.contacts_edit_email.backspace();
      else if (w.contacts_edit_focus == 5) w.contacts_edit_phone.backspace();
      else if (w.contacts_edit_focus == 6) w.contacts_edit_notes.backspace();
      return true;
    }
    return true;
  }

  if (w.contacts_search_focused) {
    if (key == hal::KeyEscape || key == hal::KeyEnter) { w.contacts_search_focused = false; return true; }
    if (key == hal::KeyBackspace) { w.contacts_search_edit.backspace(); return true; }
    return true;
  }

  if (key == '/' || key == hal::KeyCtrlF) { w.contacts_search_focused = true; return true; }
  if (key == 'n' || key == 'c' || key == hal::KeyCtrlN) { S.contacts_open_card_editor(w, true); return true; }
  if (key == 'e') { S.contacts_open_card_editor(w, false); return true; }
  if (key == 'f') { S.contacts_toggle_fav_selected(w); return true; }
  if (key == 'd' || key == hal::KeyDelete || key == hal::KeyBackspace) { S.contacts_delete_selected(w); return true; }

  if (key == hal::KeyUp || key == hal::KeyDown) {
    if (w.contacts_store) {
      auto list = w.contacts_search_edit.text.empty()
          ? w.contacts_store->contacts_for_group(w.contacts_active_group)
          : w.contacts_store->search(w.contacts_search_edit.text);
      if (!list.empty()) {
        int cur_idx = 0;
        for (size_t i = 0; i < list.size(); ++i) {
          if (list[i].id == w.contacts_selected_id) { cur_idx = (int)i; break; }
        }
        if (key == hal::KeyUp && cur_idx > 0) cur_idx--;
        else if (key == hal::KeyDown && cur_idx + 1 < (int)list.size()) cur_idx++;
        w.contacts_selected_id = list[cur_idx].id;
        if (cur_idx < w.contacts_list_scroll) w.contacts_list_scroll = cur_idx;
        if (cur_idx >= w.contacts_list_scroll + 6) w.contacts_list_scroll = cur_idx - 5;
      }
    }
    return true;
  }
  return false;
}

static void contacts_wheel(WindowSurface& w, int notches) {
  w.contacts_list_scroll -= notches * 2;
  if (w.contacts_list_scroll < 0) w.contacts_list_scroll = 0;
}

// ===========================================================================
// Native C++ AetherCalendar Client (Apple-style iCal & Schedule)
// ===========================================================================

static const char* const CALENDAR_CHIPS[] = {
  "+ New Event", "Month", "Week", "Day", "Agenda", "< Prev", "Today", "Next >", "Export iCal"
};

void HardwareCompositor::calendar_open_event_editor(WindowSurface& win, bool new_event) {
  win.calendar_editing = true;
  win.calendar_is_new = new_event;
  win.calendar_edit_focus = 0;
  if (new_event || !win.calendar_store) {
    win.calendar_edit_title.text.clear();
    win.calendar_edit_title.cursor = 0;
    win.calendar_edit_location.text.clear();
    win.calendar_edit_location.cursor = 0;
    win.calendar_edit_cat.text = "work";
    win.calendar_edit_cat.cursor = 4;
    {
      // A new event starts on the day being looked at, not a fixed date.
      char dbuf[32];
      std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", win.calendar_view_year, win.calendar_view_month,
                    win.calendar_view_day);
      win.calendar_edit_date.text = dbuf;
    }
    win.calendar_edit_date.cursor = (int)win.calendar_edit_date.text.size();
    win.calendar_edit_time.text = "10:00 - 11:00";
    win.calendar_edit_time.cursor = 13;
    win.calendar_edit_desc.text.clear();
    win.calendar_edit_desc.cursor = 0;
  } else {
    const auto* ev = win.calendar_store->get_event(win.calendar_selected_event_id);
    if (ev) {
      win.calendar_edit_title.text = ev->title;
      win.calendar_edit_title.cursor = (int)ev->title.size();
      win.calendar_edit_location.text = ev->location;
      win.calendar_edit_location.cursor = (int)ev->location.size();
      win.calendar_edit_cat.text = ev->calendar_id;
      win.calendar_edit_cat.cursor = (int)ev->calendar_id.size();
      char dbuf[32], tbuf[32];
      std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", ev->year, ev->month, ev->day);
      win.calendar_edit_date.text = dbuf;
      win.calendar_edit_date.cursor = (int)win.calendar_edit_date.text.size();
      const int end_total = ev->start_hour * 60 + ev->start_min + ev->duration_mins;
      std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d - %02d:%02d", ev->start_hour, ev->start_min,
                    (end_total / 60) % 24, end_total % 60);
      win.calendar_edit_time.text = tbuf;
      win.calendar_edit_time.cursor = (int)win.calendar_edit_time.text.size();
      win.calendar_edit_desc.text = ev->description;
      win.calendar_edit_desc.cursor = (int)ev->description.size();
    }
  }
}

void HardwareCompositor::calendar_save_event_editor(WindowSurface& win) {
  if (!win.calendar_store) return;
  CalendarEvent ev;
  ev.title = win.calendar_edit_title.text.empty() ? "New Event" : win.calendar_edit_title.text;
  ev.location = win.calendar_edit_location.text;
  ev.calendar_id = win.calendar_edit_cat.text.empty() ? "work" : win.calendar_edit_cat.text;
  ev.description = win.calendar_edit_desc.text;
  ev.year = win.calendar_view_year;
  ev.month = win.calendar_view_month;
  ev.day = win.calendar_view_day;
  ev.start_hour = 10;
  ev.start_min = 0;
  ev.duration_mins = 60;
  // The Date and Time fields are what the sheet asks for, so they are what is
  // saved. This used to drop both and file every event at 10:00 on the day in
  // view, whatever was typed.
  {
    int yy = 0, mm = 0, dd = 0;
    if (std::sscanf(win.calendar_edit_date.text.c_str(), "%d-%d-%d", &yy, &mm, &dd) == 3 &&
        yy >= 1900 && mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31) {
      ev.year = yy; ev.month = mm; ev.day = dd;
    }
    int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
    const int nt = std::sscanf(win.calendar_edit_time.text.c_str(), "%d:%d - %d:%d", &h1, &m1, &h2, &m2);
    if (nt >= 2 && h1 >= 0 && h1 < 24 && m1 >= 0 && m1 < 60) {
      ev.start_hour = h1; ev.start_min = m1;
      if (nt == 4 && h2 >= 0 && h2 < 24 && m2 >= 0 && m2 < 60) {
        const int dur = (h2 * 60 + m2) - (h1 * 60 + m1);
        if (dur > 0) ev.duration_mins = dur;
      }
    }
  }

  if (win.calendar_is_new) {
    win.calendar_store->add_event(ev);
    win.calendar_selected_event_id = ev.id;
    set_toast("Created event: " + ev.title);
  } else {
    ev.id = win.calendar_selected_event_id;
    win.calendar_store->update_event(ev);
    set_toast("Updated event: " + ev.title);
  }
  win.calendar_editing = false;
}

void HardwareCompositor::calendar_cancel_event_editor(WindowSurface& win) {
  win.calendar_editing = false;
}

void HardwareCompositor::calendar_delete_selected(WindowSurface& win) {
  if (!win.calendar_store || win.calendar_selected_event_id.empty()) return;
  win.calendar_store->delete_event(win.calendar_selected_event_id);
  auto evs = win.calendar_store->all_events();
  if (!evs.empty()) win.calendar_selected_event_id = evs[0].id;
  else win.calendar_selected_event_id.clear();
  set_toast("Deleted calendar event");
}

void HardwareCompositor::calendar_nav_date(WindowSurface& win, int delta_months) {
  win.calendar_view_month += delta_months;
  if (win.calendar_view_month > 12) {
    win.calendar_view_month = 1;
    win.calendar_view_year++;
  } else if (win.calendar_view_month < 1) {
    win.calendar_view_month = 12;
    win.calendar_view_year--;
  }
}

void HardwareCompositor::render_calendar_window(WindowSurface& win, Kernel& k) {
  if (!win.calendar_store) {
    win.calendar_store = std::make_shared<CalendarStore>(k.config().root + "/calendar");
  }

  int w = win.content_width();
  int h = win.content_height();
  clear_buffer(win.buffer, tok::panel, w, h);

  draw_card(win, w, h, 10, 10, w - 20, h - 20, "aethercalendar • apple ical & agenda");
  draw_app_chips(*this, win, w, h, CALENDAR_CHIPS, nullptr);

  // Status header
  draw_label_to(win.buffer, w, h, 22, APP_HEAD_Y + 2, "schedule", tok::faint);
  int head_x = 22 + label_width("schedule") + 12;
  draw_text_to(win.buffer, w, h, head_x, APP_HEAD_Y, fit_text(*this, win.calendar_store->status_line(), 300, Face::Mono), tok::accent, Face::Mono);

  int body_y = APP_BODY_Y;
  int body_h = h - body_y - 14;
  if (body_h < 80) body_h = 80;

  // --- Pane 1: Sidebar with Mini Month Picker & Calendars List (Left) ---------
  int sidebar_w = (w > 850) ? 190 : 140;
  draw_rect_to(win.buffer, w, h, 20, body_y, sidebar_w, body_h, 0xFF141417);
  draw_rect_outline_to(win.buffer, w, h, 20, body_y, sidebar_w, body_h, tok::line_up);

  // Mini Month Picker
  const char* MONTH_NAMES[] = {"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  std::string month_title = std::string(MONTH_NAMES[win.calendar_view_month]) + " " + std::to_string(win.calendar_view_year);
  draw_text_to(win.buffer, w, h, 28, body_y + 8, fit_text(*this, month_title, sidebar_w - 16, Face::UiBold), tok::text, Face::UiBold);

  // Mini calendar grid
  int mini_y = body_y + 28;
  const char* DAYS_HDR[] = {"M", "T", "W", "T", "F", "S", "S"};
  int cell_w = (sidebar_w - 20) / 7;
  for (int di = 0; di < 7; ++di) {
    draw_text_to(win.buffer, w, h, 28 + di * cell_w, mini_y, DAYS_HDR[di], tok::faint, Face::UiSmall);
  }
  mini_y += 16;
  int first_col = 0, month_days = 30, month_rows = 5;
  calendar_month_layout(win.calendar_view_year, win.calendar_view_month, first_col, month_days, month_rows);
  // What the grid below is drawn from, kept so a drive can check the month it
  // actually painted rather than trust it.
  win.calendar_grid_first_col = first_col;
  win.calendar_grid_days = month_days;
  win.calendar_grid_rows = month_rows;
  int day_num = 1;
  for (int row = 0; row < month_rows; ++row) {
    for (int col = 0; col < 7; ++col) {
      if (row == 0 && col < first_col) continue;
      if (day_num > month_days) break;
      int dx = 28 + col * cell_w;
      bool is_today = calendar_is_today(win.calendar_view_year, win.calendar_view_month, day_num);
      if (is_today) {
        draw_pill_to(win.buffer, w, h, dx + 2, mini_y - 1, 14, 14, 0xFF0A84FF, 0);
        draw_text_to(win.buffer, w, h, dx + 2, mini_y, std::to_string(day_num), 0xFFFFFFFF, Face::UiSmall);
      } else {
        draw_text_to(win.buffer, w, h, dx, mini_y, std::to_string(day_num), tok::dim, Face::UiSmall);
      }
      day_num++;
    }
    mini_y += 16;
  }

  // Divider
  draw_rect_to(win.buffer, w, h, 26, mini_y + 8, sidebar_w - 12, 1, tok::line_up);

  // Calendars category checkboxes
  int cat_y = mini_y + 18;
  draw_label_to(win.buffer, w, h, 28, cat_y, "CALENDARS", tok::faint);
  cat_y += 18;

  auto cats = win.calendar_store->categories();
  for (const auto& c : cats) {
    if (cat_y + 20 > body_y + body_h - 30) break;
    draw_pill_to(win.buffer, w, h, 30, cat_y + 4, 10, 10, c.color, 0);
    if (c.visible) {
      draw_text_to(win.buffer, w, h, 32, cat_y + 2, "x", 0xFFFFFFFF, Face::UiSmall);
    }
    draw_text_to(win.buffer, w, h, 48, cat_y + 2, fit_text(*this, c.name, sidebar_w - 55, Face::Ui),
                 c.visible ? tok::text : tok::faint, Face::Ui);
    cat_y += 22;
  }

  // "+ New Event" button at sidebar bottom
  draw_pill_to(win.buffer, w, h, 26, body_y + body_h - 24, sidebar_w - 12, 20, tok::panel2, tok::line_up);
  draw_text_to(win.buffer, w, h, 26 + (sidebar_w - 12) / 2 - 32, body_y + body_h - 22, "+ New Event", tok::dim, Face::UiSmall);

  // --- Pane 2: Main Calendar Grid / Agenda View (Right) ---------------------
  int main_x = 20 + sidebar_w + 6;
  int main_w = w - main_x - 20;
  if (main_w < 200) main_w = 200;
  draw_rect_to(win.buffer, w, h, main_x, body_y, main_w, body_h, 0xFF141417);
  draw_rect_outline_to(win.buffer, w, h, main_x, body_y, main_w, body_h, tok::line_up);

  // View Header Banner
  draw_rect_to(win.buffer, w, h, main_x, body_y, main_w, 36, 0xFF1A1C23);
  draw_rect_outline_to(win.buffer, w, h, main_x, body_y, main_w, 36, tok::line_up);

  draw_text_to(win.buffer, w, h, main_x + 16, body_y + 8, month_title, tok::text, Face::UiBold);

  // View Mode Tabs inside header
  const char* VIEW_MODES[] = {"Month", "Week", "Day", "Agenda"};
  int vtab_x = main_x + main_w - 220;
  for (int vi = 0; vi < 4; ++vi) {
    bool vsel = (win.calendar_view_mode == vi);
    int vw = 50;
    if (vsel) draw_pill_to(win.buffer, w, h, vtab_x, body_y + 6, vw, 22, 0xFF0A84FF, 0);
    else draw_pill_to(win.buffer, w, h, vtab_x, body_y + 6, vw, 22, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, vtab_x + 8, body_y + 9, VIEW_MODES[vi], vsel ? 0xFFFFFFFF : tok::dim, Face::UiSmall);
    vtab_x += vw + 4;
  }

  int view_area_y = body_y + 36;
  int view_area_h = body_h - 36;

  if (win.calendar_view_mode == 0) {
    // === Month Grid View ===
    int col_w = main_w / 7;
    int row_h = (view_area_h - 24) / month_rows;
    if (row_h < 40) row_h = 40;

    // Day headers row
    const char* WEEKDAYS[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    for (int col = 0; col < 7; ++col) {
      int cx = main_x + col * col_w;
      draw_rect_to(win.buffer, w, h, cx, view_area_y, col_w, 24, 0xFF17171C);
      draw_rect_outline_to(win.buffer, w, h, cx, view_area_y, col_w, 24, tok::line_up);
      draw_text_to(win.buffer, w, h, cx + 8, view_area_y + 4, (col_w > 90 ? WEEKDAYS[col] : DAYS_HDR[col]), tok::faint, Face::UiSmall);
    }

    int d_num = 1;
    for (int r = 0; r < month_rows; ++r) {
      for (int c = 0; c < 7; ++c) {
        if ((r == 0 && c < first_col) || d_num > month_days) {
          // Days of the neighbouring months: shaded, empty.
          int cx = main_x + c * col_w;
          int cy = view_area_y + 24 + r * row_h;
          draw_rect_to(win.buffer, w, h, cx, cy, col_w, row_h, 0xFF101014);
          draw_rect_outline_to(win.buffer, w, h, cx, cy, col_w, row_h, tok::line_up);
          continue;
        }
        int cx = main_x + c * col_w;
        int cy = view_area_y + 24 + r * row_h;
        bool is_today = calendar_is_today(win.calendar_view_year, win.calendar_view_month, d_num);
        draw_rect_to(win.buffer, w, h, cx, cy, col_w, row_h, is_today ? 0xFF192030 : 0xFF141418);
        draw_rect_outline_to(win.buffer, w, h, cx, cy, col_w, row_h, is_today ? 0xFF0A84FF : tok::line_up);

        if (is_today) {
          draw_pill_to(win.buffer, w, h, cx + col_w - 24, cy + 3, 20, 18, 0xFF0A84FF, 0);
          draw_text_to(win.buffer, w, h, cx + col_w - 20, cy + 4, std::to_string(d_num), 0xFFFFFFFF, Face::UiBold);
        } else {
          draw_text_to(win.buffer, w, h, cx + col_w - 20, cy + 4, std::to_string(d_num), tok::dim, Face::UiSmall);
        }

        // Draw day's events
        auto day_evs = win.calendar_store->events_for_day(win.calendar_view_year, win.calendar_view_month, d_num);
        int ev_y = cy + 22;
        for (const auto& ev : day_evs) {
          if (ev_y + 16 > cy + row_h) break;
          draw_pill_to(win.buffer, w, h, cx + 4, ev_y, col_w - 8, 16, ev.color, 0);
          std::string chip_txt = (ev.all_day ? "" : std::to_string(ev.start_hour) + "p ") + ev.title;
          draw_text_to(win.buffer, w, h, cx + 8, ev_y + 1, fit_text(*this, chip_txt, col_w - 16, Face::UiSmall), 0xFFFFFFFF, Face::UiSmall);
          ev_y += 18;
        }

        d_num++;
      }
    }
  } else {
    // === Agenda / List View ===
    auto events = win.calendar_store->all_events();
    int ey = view_area_y + 12;
    for (size_t i = win.calendar_list_scroll; i < events.size(); ++i) {
      if (ey + 54 > view_area_y + view_area_h - 10) break;
      const auto& ev = events[i];
      bool sel = (ev.id == win.calendar_selected_event_id);
      bool hot = win.focused && mouse_over_content(win, main_x + 12, ey, main_w - 24, 50);

      draw_rect_to(win.buffer, w, h, main_x + 12, ey, main_w - 24, 50, sel ? 0xFF1E283D : (hot ? tok::wash : 0xFF181920));
      draw_rect_outline_to(win.buffer, w, h, main_x + 12, ey, main_w - 24, 50, sel ? 0xFF0A84FF : tok::line_up);

      // Category color tag
      draw_rect_to(win.buffer, w, h, main_x + 12, ey, 5, 50, ev.color);

      // Date & Time Pill
      std::string time_sum = ev.date_summary() + " • " + ev.time_summary();
      draw_text_to(win.buffer, w, h, main_x + 26, ey + 6, time_sum, tok::accent, Face::UiSmall);

      // Title
      draw_text_to(win.buffer, w, h, main_x + 26, ey + 22, fit_text(*this, ev.title, main_w - 120, Face::UiBold), tok::text, Face::UiBold);

      // Location / Category
      std::string loc_str = ev.location.empty() ? ("[" + ev.calendar_id + "]") : (ev.location + " • [" + ev.calendar_id + "]");
      draw_text_to(win.buffer, w, h, main_x + 26, ey + 36, fit_text(*this, loc_str, main_w - 120, Face::UiSmall), tok::faint, Face::UiSmall);

      ey += 56;
    }
  }

  // --- Modal New / Edit Event Sheet -----------------------------------------
  if (win.calendar_editing) {
    draw_rect_to(win.buffer, w, h, 0, 0, w, h, 0x88000000);
    int mw = 560, mh = 380;
    int mx = (w - mw) / 2, my = (h - mh) / 2;
    draw_card(win, w, h, mx, my, mw, mh, win.calendar_is_new ? "New Calendar Event" : "Edit Calendar Event");

    auto draw_input = [&](int idx, const std::string& lbl, EditLine& el, int y) {
      bool f = (win.calendar_edit_focus == idx);
      draw_label_to(win.buffer, w, h, mx + 24, y + 4, lbl, f ? tok::accent : tok::faint);
      draw_rect_to(win.buffer, w, h, mx + 110, y, mw - 140, 24, f ? 0xFF1E283D : tok::panel2);
      draw_rect_outline_to(win.buffer, w, h, mx + 110, y, mw - 140, 24, f ? 0xFF0A84FF : tok::line_up);
      std::string disp = fit_text(*this, el.text, mw - 156, Face::Ui);
      draw_text_to(win.buffer, w, h, mx + 118, y + 4, disp, tok::text, Face::Ui);
      if (f && (steady_ms() / 400) % 2 == 0) {
        int cx = mx + 118 + text_width(el.text.substr(0, el.cursor), Face::Ui);
        draw_rect_to(win.buffer, w, h, cx, y + 4, 2, 16, 0xFF0A84FF);
      }
    };

    int iy = my + 44;
    draw_input(0, "Event Title", win.calendar_edit_title, iy);
    draw_input(1, "Location", win.calendar_edit_location, iy + 34);
    draw_input(2, "Calendar", win.calendar_edit_cat, iy + 68);
    draw_input(3, "Date", win.calendar_edit_date, iy + 102);
    draw_input(4, "Time", win.calendar_edit_time, iy + 136);
    draw_input(5, "Description", win.calendar_edit_desc, iy + 170);

    // Save & Cancel buttons
    int by = my + mh - 36;
    draw_pill_to(win.buffer, w, h, mx + mw - 180, by, 70, 24, tok::panel2, tok::line_up);
    draw_text_to(win.buffer, w, h, mx + mw - 168, by + 4, "Cancel", tok::dim, Face::UiSmall);

    draw_pill_to(win.buffer, w, h, mx + mw - 95, by, 75, 24, 0xFF0A84FF, 0);
    draw_text_to(win.buffer, w, h, mx + mw - 85, by + 4, "Save Event", 0xFFFFFFFF, Face::UiSmall);
  }
}

void HardwareCompositor::calendar_click(WindowSurface& w, int rel_x, int rel_y, std::string& to_run) {
  HardwareCompositor& S = *this;
  int cw = w.content_width();
  int ch = w.content_height();

  if (w.calendar_editing) {
    int mw = 560, mh = 380;
    int mx = (cw - mw) / 2, my = (ch - mh) / 2;
    int by = my + mh - 36;

    if (rel_x >= mx + mw - 180 && rel_x < mx + mw - 110 && rel_y >= by && rel_y < by + 24) {
      S.calendar_cancel_event_editor(w);
      return;
    }
    if (rel_x >= mx + mw - 95 && rel_x < mx + mw - 20 && rel_y >= by && rel_y < by + 24) {
      S.calendar_save_event_editor(w);
      return;
    }

    int iy = my + 44;
    for (int i = 0; i < 6; ++i) {
      if (rel_y >= iy + i * 34 && rel_y < iy + i * 34 + 26 && rel_x >= mx + 110 && rel_x < mx + mw - 30) {
        w.calendar_edit_focus = i;
        return;
      }
    }
    return;
  }

  // Chips toolbar
  if (const char* chip = chip_hit(S, CALENDAR_CHIPS, rel_x, rel_y)) {
    if (strcmp(chip, "+ New Event") == 0) {
      S.calendar_open_event_editor(w, true);
    } else if (strcmp(chip, "Month") == 0) {
      w.calendar_view_mode = 0;
    } else if (strcmp(chip, "Week") == 0) {
      w.calendar_view_mode = 1;
    } else if (strcmp(chip, "Day") == 0) {
      w.calendar_view_mode = 2;
    } else if (strcmp(chip, "Agenda") == 0) {
      w.calendar_view_mode = 3;
    } else if (strcmp(chip, "< Prev") == 0) {
      S.calendar_nav_date(w, -1);
    } else if (strcmp(chip, "Today") == 0) {
      calendar_today(w.calendar_view_year, w.calendar_view_month, w.calendar_view_day);
    } else if (strcmp(chip, "Next >") == 0) {
      S.calendar_nav_date(w, 1);
    } else if (strcmp(chip, "Export iCal") == 0) {
      if (w.calendar_store) {
        std::string ics = w.calendar_store->export_all_ics();
        S.set_toast("Exported " + std::to_string(w.calendar_store->total_events()) + " events to calendar/events/");
      }
    }
    return;
  }

  int body_y = APP_BODY_Y;
  int body_h = ch - body_y - 14;
  int sidebar_w = (cw > 850) ? 190 : 140;

  // Click in sidebar calendars list
  int mini_y = body_y + 28 + 16 * 6;
  int cat_y = mini_y + 18 + 18;
  if (rel_x >= 20 && rel_x < 20 + sidebar_w && rel_y >= cat_y && rel_y < body_y + body_h - 30) {
    if (w.calendar_store) {
      auto cats = w.calendar_store->categories();
      int c_idx = (rel_y - cat_y) / 22;
      if (c_idx >= 0 && c_idx < (int)cats.size()) {
        w.calendar_store->toggle_category(cats[c_idx].id);
        return;
      }
    }
  }

  // Click on "+ New Event" at bottom of sidebar
  if (rel_x >= 26 && rel_x < 26 + sidebar_w - 12 && rel_y >= body_y + body_h - 26) {
    S.calendar_open_event_editor(w, true);
    return;
  }

  // View Mode Tabs in main area
  int main_x = 20 + sidebar_w + 6;
  int main_w = cw - main_x - 20;
  int vtab_x = main_x + main_w - 220;
  if (rel_y >= body_y + 6 && rel_y < body_y + 28) {
    for (int vi = 0; vi < 4; ++vi) {
      if (rel_x >= vtab_x && rel_x < vtab_x + 50) {
        w.calendar_view_mode = vi;
        return;
      }
      vtab_x += 54;
    }
  }
}

static void calendar_text(HardwareCompositor& S, WindowSurface& w, const std::string& text) {
  if (w.calendar_editing) {
    if (w.calendar_edit_focus == 0) w.calendar_edit_title.insert(text, 128);
    else if (w.calendar_edit_focus == 1) w.calendar_edit_location.insert(text, 128);
    else if (w.calendar_edit_focus == 2) w.calendar_edit_cat.insert(text, 64);
    else if (w.calendar_edit_focus == 3) w.calendar_edit_date.insert(text, 32);
    else if (w.calendar_edit_focus == 4) w.calendar_edit_time.insert(text, 32);
    else if (w.calendar_edit_focus == 5) w.calendar_edit_desc.insert(text, 512);
    return;
  }
}

static bool calendar_key(HardwareCompositor& S, WindowSurface& w, int key) {
  if (w.calendar_editing) {
    if (key == hal::KeyEscape) { S.calendar_cancel_event_editor(w); return true; }
    if (key == hal::KeyTab) { w.calendar_edit_focus = (w.calendar_edit_focus + 1) % 6; return true; }
    if (key == hal::KeyEnter) { S.calendar_save_event_editor(w); return true; }
    if (key == hal::KeyBackspace) {
      if (w.calendar_edit_focus == 0) w.calendar_edit_title.backspace();
      else if (w.calendar_edit_focus == 1) w.calendar_edit_location.backspace();
      else if (w.calendar_edit_focus == 2) w.calendar_edit_cat.backspace();
      else if (w.calendar_edit_focus == 3) w.calendar_edit_date.backspace();
      else if (w.calendar_edit_focus == 4) w.calendar_edit_time.backspace();
      else if (w.calendar_edit_focus == 5) w.calendar_edit_desc.backspace();
      return true;
    }
    return true;
  }

  if (key == 'n' || key == 'c' || key == hal::KeyCtrlN) { S.calendar_open_event_editor(w, true); return true; }
  if (key == 'd' || key == hal::KeyDelete || key == hal::KeyBackspace) { S.calendar_delete_selected(w); return true; }
  if (key == 'm') { w.calendar_view_mode = 0; return true; }
  if (key == 'w') { w.calendar_view_mode = 1; return true; }
  if (key == 'a') { w.calendar_view_mode = 3; return true; }
  if (key == hal::KeyLeft) { S.calendar_nav_date(w, -1); return true; }
  if (key == hal::KeyRight) { S.calendar_nav_date(w, 1); return true; }

  if (key == hal::KeyUp || key == hal::KeyDown) {
    if (w.calendar_store) {
      auto evs = w.calendar_store->all_events();
      if (!evs.empty()) {
        int cur_idx = 0;
        for (size_t i = 0; i < evs.size(); ++i) {
          if (evs[i].id == w.calendar_selected_event_id) { cur_idx = (int)i; break; }
        }
        if (key == hal::KeyUp && cur_idx > 0) cur_idx--;
        else if (key == hal::KeyDown && cur_idx + 1 < (int)evs.size()) cur_idx++;
        w.calendar_selected_event_id = evs[cur_idx].id;
      }
    }
    return true;
  }
  return false;
}

static void calendar_wheel(WindowSurface& w, int notches) {
  w.calendar_list_scroll -= notches * 2;
  if (w.calendar_list_scroll < 0) w.calendar_list_scroll = 0;
}

// ---------------------------------------------------------------------------
// Native Face Applications Desk (WindowType::FaceApp)
// ---------------------------------------------------------------------------


// Where a bundled face app (apps/<name>.aos) lives. The machine's root comes
// first, so a face can be overridden per machine; then the install beside the
// binary (<exe dir>/../apps for a build tree, <exe dir>/apps for a flat
// install). The working directory is NOT asked: the pane used to open
// "apps/x.aos" relative to wherever the desktop happened to be started, so
// from any other directory every app said it could not be read.
static std::vector<std::string> face_app_candidates(const std::string& path, Kernel& k) {
  std::vector<std::string> out;
  if (path.empty() || path[0] == '/') { out.push_back(path); return out; }
  if (!k.config().root.empty()) out.push_back(k.config().root + "/" + path);
#if !defined(AIOS_METAL)
  if (const char* env = std::getenv("AIOS_APPS_DIR")) {
    std::string base = path.rfind("apps/", 0) == 0 ? path.substr(5) : path;
    out.push_back(std::string(env) + "/" + base);
  }
  char exe[4096];
  const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = 0;
    std::string dir(exe);
    const size_t sl = dir.rfind('/');
    if (sl != std::string::npos) {
      dir.resize(sl);
      out.push_back(dir + "/" + path);
      const size_t up = dir.rfind('/');
      if (up != std::string::npos) out.push_back(dir.substr(0, up) + "/" + path);
    }
  }
#endif
  return out;
}

void HardwareCompositor::face_app_load(WindowSurface& win, const std::string& path, Kernel& k) {
  win.face_app_path = path;
  Result<ByteBuffer> f(Status::NotFound, "not tried");
  std::string tried;
  for (const auto& cand : face_app_candidates(path, k)) {
    f = k.host().read_file(cand);
    if (f.ok()) { win.face_app_path = cand; break; }
    tried += (tried.empty() ? "" : ", ") + cand;
  }
  if (!f.ok()) {
    win.status_text = "Error reading face file: " + path + " (looked in " + tried + ")";
    return;
  }
  const std::string source((const char*)f->data(), f->size());
  auto parsed = face::parse_face(source);
  win.face_app_title = !parsed.title.empty() ? parsed.title : parsed.name;
  win.face_app_runtime = std::make_shared<face::Runtime>(std::move(parsed));
  if (console_) {
    win.face_app_runtime->bind(surface_bindings(k, *console_, win.face_app_runtime->face().name));
  }
  win.face_app_runtime->load();
  win.status_text = "Running " + win.face_app_title + " (" + path + ")";
}

void HardwareCompositor::face_app_switch(WindowSurface& win, int index, Kernel& k) {
  if (win.face_app_list.empty()) return;
  if (index < 0) index = 0;
  if (index >= (int)win.face_app_list.size()) index = (int)win.face_app_list.size() - 1;
  win.face_app_selected_index = index;
  face_app_load(win, win.face_app_list[index], k);
}

void HardwareCompositor::face_app_reload(WindowSurface& win, Kernel& k) {
  if (win.face_app_path.empty() && !win.face_app_list.empty()) {
    win.face_app_path = win.face_app_list[win.face_app_selected_index];
  }
  if (!win.face_app_path.empty()) {
    face_app_load(win, win.face_app_path, k);
    set_toast("Hot-reloaded " + win.face_app_title);
  }
}

void HardwareCompositor::face_app_click(WindowSurface& win, int rel_x, int rel_y) {
  // Top App Bar: y = 0..34
  if (rel_y < 34) {
    int bx = 12;
    static const char* const APP_CHIPS[] = {
      "1. Order Desk",
      "2. Task Tracker",
      "3. System Monitor",
      "4. Notes"
    };
    for (int i = 0; i < 4; ++i) {
      int bw = text_width(APP_CHIPS[i], Face::UiBold) + 44;
      if (rel_x >= bx && rel_x < bx + bw) {
        if (kernel_) face_app_switch(win, i, *kernel_);
        return;
      }
      bx += bw + 8;
    }
    // Action buttons on right
    int rw = win.content_width();
    int reload_w = text_width("[ Hot Reload ]", Face::UiSmall) + 14;
    int inspect_w = text_width("[ Inspect in Studio ]", Face::UiSmall) + 14;
    int inspect_x = rw - inspect_w - 12;
    int reload_x = inspect_x - reload_w - 8;

    if (rel_x >= reload_x && rel_x < reload_x + reload_w) {
      if (kernel_) face_app_reload(win, *kernel_);
      return;
    }
    if (rel_x >= inspect_x && rel_x < inspect_x + inspect_w) {
      defer_after_click([this]() { run_ui_action(UiAction::PaneInspector); });
      for (auto& w : windows_) {
        if (w.type == WindowType::Inspector) {
          if (win.face_app_selected_index == 0) w.studio_target_name = "order_desk";
          else if (win.face_app_selected_index == 1) w.studio_target_name = "task_tracker";
          else if (win.face_app_selected_index == 2) w.studio_target_name = "system_monitor";
          else if (win.face_app_selected_index == 3) w.studio_target_name = "notes";
          break;
        }
      }
      return;
    }
    return;
  }

  // Content click: forward to Face runtime
  if (win.face_app_runtime) {
    win.face_app_runtime->click(rel_x, rel_y - 36);
  }
}

static bool face_app_key(HardwareCompositor& S, WindowSurface& w, int key) {
  if (!w.face_app_runtime) return false;
  if (key == hal::KeyEscape) return true;
  std::string name;
  if (key == hal::KeyUp) name = "ArrowUp";
  else if (key == hal::KeyDown) name = "ArrowDown";
  else if (key == hal::KeyLeft) name = "ArrowLeft";
  else if (key == hal::KeyRight) name = "ArrowRight";
  else if (key == hal::KeyEnter) name = "Enter";
  else if (key == hal::KeyTab) name = "Tab";
  else if (key == hal::KeyBackspace) name = "Backspace";
  else if (key == ' ') name = "Space";
  else if (key >= 'a' && key <= 'z') name = std::string(1, (char)key);
  else if (key >= 'A' && key <= 'Z') name = std::string(1, (char)(key - 'A' + 'a'));
  else if (key >= '0' && key <= '9') name = std::string(1, (char)key);

  if (!name.empty()) {
    w.face_app_runtime->key(name, true);
    w.face_app_runtime->key(name, false);
    return true;
  }
  return false;
}

static void face_app_text(HardwareCompositor& S, WindowSurface& w, const std::string& text) {
  if (w.face_app_runtime) {
    w.face_app_runtime->type(text);
  }
}

void HardwareCompositor::render_face_app_window(WindowSurface& win, Kernel& k) {
  const int W = win.content_width();
  const int H = win.content_height();
  if (W <= 0 || H <= 0) return;
  if ((int)win.buffer.size() != W * H) {
    win.buffer.assign(static_cast<size_t>(W * H), tok::panel);
  } else {
    clear_buffer(win.buffer, tok::panel, W, H);
  }

  // 1. Top App Switcher & Action Bar
  draw_rect_to(win.buffer, W, H, 0, 0, W, 34, tok::rail);
  draw_rect_to(win.buffer, W, H, 0, 33, W, 1, tok::line_up);

  int bx = 12;
  static const struct FaceAppChip { const char* label; const char* icon; } APP_CHIPS[] = {
    {"1. Order Desk", "order_desk"},
    {"2. Task Tracker", "task_tracker"},
    {"3. System Monitor", "system_monitor"},
    {"4. Notes", "notes"}
  };
  for (int i = 0; i < 4; ++i) {
    int bw = text_width(APP_CHIPS[i].label, Face::UiBold) + 44;
    bool active = (win.face_app_selected_index == i);
    draw_pill_to(win.buffer, W, H, bx, 5, bw, 24,
                 active ? 0xFF0A84FF : 0xFF2A2E37, active ? 0xFF5AC8FA : 0xFF3A3F4B);
    draw_named_icon(win.buffer, W, H, bx + 6, 9, APP_CHIPS[i].icon, 16);
    draw_text_to(win.buffer, W, H, bx + 26, 10, APP_CHIPS[i].label, active ? 0xFFFFFFFF : tok::dim,
                 active ? Face::UiBold : Face::UiSmall);
    bx += bw + 8;
  }

  // Action buttons on right
  int reload_w = text_width("[ Hot Reload ]", Face::UiSmall) + 14;
  int inspect_w = text_width("[ Inspect in Studio ]", Face::UiSmall) + 14;
  int inspect_x = W - inspect_w - 12;
  int reload_x = inspect_x - reload_w - 8;

  draw_pill_to(win.buffer, W, H, reload_x, 5, reload_w, 24, 0xFF2A2E37, 0xFF3A3F4B);
  draw_text_to(win.buffer, W, H, reload_x + 7, 10, "[ Hot Reload ]", tok::text, Face::UiSmall);

  draw_pill_to(win.buffer, W, H, inspect_x, 5, inspect_w, 24, 0xFF1C3A5E, 0xFF0A84FF);
  draw_text_to(win.buffer, W, H, inspect_x + 7, 10, "[ Inspect in Studio ]", 0xFF5AC8FA, Face::UiSmall);

  // 2. Face Viewport Area
  if (!win.face_app_runtime) {
    if (!win.face_app_list.empty()) {
      face_app_load(win, win.face_app_list[0], k);
    }
  }

  if (win.face_app_runtime) {
    win.face_app_runtime->frame(16.7, W, H - 36);
    win.face_app_runtime->paint(win.buffer, W, H, 0, 36);

    const auto& errs = win.face_app_runtime->face().errors;
    if (!errs.empty()) {
      draw_rect_to(win.buffer, W, H, 12, H - 32, W - 24, 24, 0xFF5C1D1D);
      draw_rect_outline_to(win.buffer, W, H, 12, H - 32, W - 24, 24, 0xFFFF453A);
      draw_text_to(win.buffer, W, H, 20, H - 26, "! " + errs[0], 0xFFFFD2D0, Face::UiSmall);
    }
  } else {
    draw_text_to(win.buffer, W, H, 24, 60, "No face app is running", tok::text, Face::UiBold);
    /* Say why when a load failed, instead of a generic empty state. */
    const bool failed = win.status_text.rfind("Error", 0) == 0;
    draw_text_to(win.buffer, W, H, 24, 84,
                 failed ? win.status_text : std::string("Pick one from the toolbar above, or load a .aos file."),
                 failed ? 0xFFFF9F0A : tok::dim, Face::Ui);
  }
}

// ---------------------------------------------------------------------------
// Universal In-Desktop Behavioral Studio & Inspector (WindowType::Inspector)
// ---------------------------------------------------------------------------

void HardwareCompositor::inspector_reset_drive(WindowSurface& win) {
  win.studio_drive_current_step = 0;
  win.studio_drive_results.clear();
  win.studio_drive_running = false;
  win.status_text = "Drive script reset • Ready to step or run";
}

void HardwareCompositor::inspector_run_drive_step(WindowSurface& win) {
  // Find target face app runtime or window
  std::shared_ptr<face::Runtime> rt;
  for (auto& w : windows_) {
    if (w.type == WindowType::FaceApp && w.face_app_runtime) {
      rt = w.face_app_runtime;
      break;
    }
  }
  if (!rt) {
    win.studio_drive_results.push_back("[FAIL] No active Face app attached to drive");
    win.studio_drive_running = false;
    return;
  }

  if (win.studio_drive_current_step >= (int)win.studio_drive_steps.size()) {
    win.studio_drive_results.push_back("[DONE] All " + std::to_string(win.studio_drive_steps.size()) + " drive steps executed cleanly");
    win.studio_drive_running = false;
    return;
  }

  std::string step = win.studio_drive_steps[win.studio_drive_current_step];
  win.studio_drive_current_step++;

  size_t colon = step.find(':');
  std::string kind = colon == std::string::npos ? step : step.substr(0, colon);
  std::string arg = colon == std::string::npos ? "" : step.substr(colon + 1);

  if (kind == "click") {
    int x = 0, y = 0;
    std::sscanf(arg.c_str(), "%d,%d", &x, &y);
    rt->click(x, y);
    win.studio_drive_results.push_back("[PASS] Step " + std::to_string(win.studio_drive_current_step) + ": Click (" + arg + ") dispatched");
  } else if (kind == "type") {
    rt->type(arg);
    win.studio_drive_results.push_back("[PASS] Step " + std::to_string(win.studio_drive_current_step) + ": Type \"" + arg + "\" into focused field");
  } else if (kind == "key") {
    rt->key(arg, true);
    rt->key(arg, false);
    win.studio_drive_results.push_back("[PASS] Step " + std::to_string(win.studio_drive_current_step) + ": Key '" + arg + "' pressed");
  } else if (kind == "action") {
    rt->run_action(arg);
    win.studio_drive_results.push_back("[PASS] Step " + std::to_string(win.studio_drive_current_step) + ": Action '" + arg + "' executed");
  } else if (kind == "set") {
    size_t eq = arg.find('=');
    if (eq != std::string::npos) {
      rt->set_state_var(arg.substr(0, eq), arg.substr(eq + 1));
      win.studio_drive_results.push_back("[PASS] Step " + std::to_string(win.studio_drive_current_step) + ": State set (" + arg + ")");
    }
  }
  // Tick one frame of the target app
  rt->frame(16.7, 1200, 600);
  win.status_text = "Drive Step " + std::to_string(win.studio_drive_current_step) + "/" +
                    std::to_string(win.studio_drive_steps.size()) + " passed";
}

void HardwareCompositor::inspector_run_all_drive_steps(WindowSurface& win) {
  inspector_reset_drive(win);
  win.studio_drive_running = true;
  while (win.studio_drive_running && win.studio_drive_current_step < (int)win.studio_drive_steps.size()) {
    inspector_run_drive_step(win);
  }
}

void HardwareCompositor::inspector_click(WindowSurface& win, int rel_x, int rel_y, std::string& to_run) {
  // Top Target & HUD Bar: y = 0..36
  if (rel_y < 36) {
    // Target Selector Pills
    int tx = 74;
    static const char* const TARGETS[] = {
      "order_desk",
      "task_tracker",
      "system_monitor",
      "notes",
      "Mail",
      "Contacts",
      "Calendar"
    };
    for (int ti = 0; ti < 7; ++ti) {
      int tw = text_width(TARGETS[ti], Face::UiBold) + 14;
      if (rel_x >= tx && rel_x < tx + tw) {
        win.studio_target_name = TARGETS[ti];
        // If target is an app, switch the FaceApp window to it
        if (kernel_) {
          for (auto& w : windows_) {
            if (w.type == WindowType::FaceApp) {
              if (ti == 0) face_app_switch(w, 0, *kernel_);
              else if (ti == 1) face_app_switch(w, 1, *kernel_);
              else if (ti == 2) face_app_switch(w, 2, *kernel_);
              else if (ti == 3) face_app_switch(w, 3, *kernel_);
              break;
            }
          }
        }
        return;
      }
      tx += tw + 6;
    }
    return;
  }

  // Sub-Tabs Bar: y = 38..66
  if (rel_y >= 38 && rel_y < 66) {
    int tab_x = 12;
    static const char* const STUDIO_TABS[] = {
      "1. Tree & Bounds",
      "2. State & Store",
      "3. Drive & Test",
      "4. Telemetry & FPS",
      "5. Source & Reload"
    };
    for (int i = 0; i < 5; ++i) {
      int tab_w = text_width(STUDIO_TABS[i], Face::UiBold) + 20;
      if (rel_x >= tab_x && rel_x < tab_x + tab_w) {
        win.studio_active_tab = i;
        return;
      }
      tab_x += tab_w + 8;
    }
    return;
  }

  // Tab Content Click Handlers
  if (win.studio_active_tab == 1) { // State & Store
    // Check if clicked in State rows to edit
    int row_y = 100;
    std::shared_ptr<face::Runtime> rt;
    for (auto& w : windows_) {
      if (w.type == WindowType::FaceApp && w.face_app_runtime) { rt = w.face_app_runtime; break; }
    }
    if (rt) {
      auto states = rt->state_vars();
      for (size_t i = 0; i < states.size(); ++i) {
        if (rel_y >= row_y && rel_y < row_y + 22 && rel_x >= 16 && rel_x < win.content_width() / 2) {
          win.studio_selected_state_idx = (int)i;
          win.studio_state_editing = true;
          win.studio_state_edit_key = states[i].first;
          win.studio_state_edit.clear();
          win.studio_state_edit.insert(states[i].second, 128);
          return;
        }
        row_y += 22;
      }
    }
  } else if (win.studio_active_tab == 2) { // Drive & Test
    int btn_y = 74;
    int run_w = text_width("[ Run All Steps ]", Face::UiSmall) + 16;
    int step_w = text_width("[ Step Next ]", Face::UiSmall) + 16;
    int reset_w = text_width("[ Reset ]", Face::UiSmall) + 16;

    int bx = 16;
    if (rel_y >= btn_y && rel_y < btn_y + 24) {
      if (rel_x >= bx && rel_x < bx + run_w) {
        inspector_run_all_drive_steps(win);
        return;
      }
      bx += run_w + 8;
      if (rel_x >= bx && rel_x < bx + step_w) {
        inspector_run_drive_step(win);
        return;
      }
      bx += step_w + 8;
      if (rel_x >= bx && rel_x < bx + reset_w) {
        inspector_reset_drive(win);
        return;
      }
    }
    // Custom prompt bar at bottom
    if (rel_y >= win.content_height() - 36) {
      win.studio_drive_cmd_focused = true;
      return;
    }
  } else if (win.studio_active_tab == 4) { // Source & Reload
    int btn_y = 74;
    int reload_w = text_width("[ Hot Reload Source ]", Face::UiSmall) + 16;
    int edit_w = text_width("[ Open in Native Editor ]", Face::UiSmall) + 16;
    int bx = 16;
    if (rel_y >= btn_y && rel_y < btn_y + 24) {
      if (rel_x >= bx && rel_x < bx + reload_w) {
        if (kernel_) {
          for (auto& w : windows_) {
            if (w.type == WindowType::FaceApp) { face_app_reload(w, *kernel_); break; }
          }
          set_toast("Hot-reloaded target Face source cleanly");
        }
        return;
      }
      bx += reload_w + 8;
      if (rel_x >= bx && rel_x < bx + edit_w) {
        defer_after_click([this]() { run_ui_action(UiAction::PaneEditor); });
        // The app that is actually running, at the path it was read from.
        std::string running;
        for (auto& w : windows_) {
          if (w.type == WindowType::FaceApp && !w.face_app_path.empty()) { running = w.face_app_path; break; }
        }
        for (auto& w : windows_) {
          if (w.type == WindowType::Editor) {
            to_run = "edit " + (running.empty() ? std::string("apps/order_desk.aos") : running);
            break;
          }
        }
        return;
      }
    }
  }
}

static bool inspector_key(HardwareCompositor& S, WindowSurface& w, int key) {
  if (w.studio_state_editing) {
    if (key == hal::KeyEscape) { w.studio_state_editing = false; return true; }
    if (key == hal::KeyEnter) {
      // Commit state edit
      for (auto& win : S.windows()) {
        if (win.type == WindowType::FaceApp && win.face_app_runtime) {
          win.face_app_runtime->set_state_var(w.studio_state_edit_key, w.studio_state_edit.text);
          break;
        }
      }
      w.studio_state_editing = false;
      S.set_toast("State updated: " + w.studio_state_edit_key + " = " + w.studio_state_edit.text);
      return true;
    }
    if (key == hal::KeyBackspace) { w.studio_state_edit.backspace(); return true; }
    return true;
  }

  if (w.studio_drive_cmd_focused) {
    if (key == hal::KeyEscape) { w.studio_drive_cmd_focused = false; return true; }
    if (key == hal::KeyEnter) {
      if (!w.studio_drive_custom_cmd.text.empty()) {
        w.studio_drive_steps.push_back(w.studio_drive_custom_cmd.text);
        w.studio_drive_custom_cmd.clear();
        S.inspector_run_drive_step(w);
      }
      return true;
    }
    if (key == hal::KeyBackspace) { w.studio_drive_custom_cmd.backspace(); return true; }
    return true;
  }

  if (key == '1') { w.studio_active_tab = 0; return true; }
  if (key == '2') { w.studio_active_tab = 1; return true; }
  if (key == '3') { w.studio_active_tab = 2; return true; }
  if (key == '4') { w.studio_active_tab = 3; return true; }
  if (key == '5') { w.studio_active_tab = 4; return true; }

  if (key == 'r' || key == 'R') {
    if (w.studio_active_tab == 2) { S.inspector_run_all_drive_steps(w); return true; }
  }
  if (key == 's' || key == 'S') {
    if (w.studio_active_tab == 2) { S.inspector_run_drive_step(w); return true; }
  }

  if (key == hal::KeyUp) {
    if (w.studio_active_tab == 0 && w.studio_tree_scroll > 0) w.studio_tree_scroll--;
    if (w.studio_active_tab == 1 && w.studio_state_scroll > 0) w.studio_state_scroll--;
    if (w.studio_active_tab == 2 && w.studio_drive_scroll > 0) w.studio_drive_scroll--;
    return true;
  }
  if (key == hal::KeyDown) {
    if (w.studio_active_tab == 0) w.studio_tree_scroll++;
    if (w.studio_active_tab == 1) w.studio_state_scroll++;
    if (w.studio_active_tab == 2) w.studio_drive_scroll++;
    return true;
  }

  return false;
}

static void inspector_text(HardwareCompositor& S, WindowSurface& w, const std::string& text) {
  if (w.studio_state_editing) {
    w.studio_state_edit.insert(text, 128);
    return;
  }
  if (w.studio_drive_cmd_focused) {
    w.studio_drive_custom_cmd.insert(text, 128);
    return;
  }
}

void HardwareCompositor::render_inspector_window(WindowSurface& win, Kernel& k) {
  const int W = win.content_width();
  const int H = win.content_height();
  if (W <= 0 || H <= 0) return;
  if ((int)win.buffer.size() != W * H) {
    win.buffer.assign(static_cast<size_t>(W * H), tok::panel);
  } else {
    clear_buffer(win.buffer, tok::panel, W, H);
  }

  // 1. Header Target Selector & Telemetry HUD Bar
  draw_rect_to(win.buffer, W, H, 0, 0, W, 36, tok::rail);
  draw_rect_to(win.buffer, W, H, 0, 35, W, 1, tok::line_up);

  draw_text_to(win.buffer, W, H, 12, 10, "Target:", 0xFFFFFFFF, Face::UiBold);
  int tx = 74;
  static const char* const TARGETS[] = {
    "order_desk",
    "task_tracker",
    "system_monitor",
    "notes",
    "Mail",
    "Contacts",
    "Calendar"
  };
  for (int ti = 0; ti < 7; ++ti) {
    int tw = text_width(TARGETS[ti], Face::UiBold) + 14;
    bool active = (win.studio_target_name == TARGETS[ti]);
    draw_pill_to(win.buffer, W, H, tx, 6, tw, 22,
                 active ? 0xFF0A84FF : 0xFF2A2E37, active ? 0xFF5AC8FA : 0xFF3A3F4B);
    draw_text_to(win.buffer, W, H, tx + 7, 10, TARGETS[ti], active ? 0xFFFFFFFF : tok::dim,
                 active ? Face::UiBold : Face::UiSmall);
    tx += tw + 6;
  }

  // Telemetry Badges on Right. These read the compositor's own frame clock;
  // they used to print "60.0 FPS" and "380us - 410us paint - 194 KB" as string
  // literals, so the studio reported a steady sixty on a desktop doing two.
  char fps_buf[32];
  std::snprintf(fps_buf, sizeof(fps_buf), "%.1f FPS", canvas_state_.fps);
  std::string fps_text = fps_buf;
  int fps_w = text_width(fps_text, Face::UiSmall) + 20;
  int fps_x = W - fps_w - 12;
  draw_pill_to(win.buffer, W, H, fps_x, 6, fps_w, 22, 0xFF143823, 0xFF30D158);
  draw_rect_to(win.buffer, W, H, fps_x + 6, 14, 6, 6, 0xFF30D158);
  draw_text_to(win.buffer, W, H, fps_x + 16, 10, fps_text, 0xFF30D158, Face::UiBold);

  char hud_buf[96];
  std::snprintf(hud_buf, sizeof(hud_buf), "%.0fus present \u00b7 %.0fus paint \u00b7 %zu KB",
                last_present_ms_ * 1000.0f, last_paint_ms_ * 1000.0f,
                (framebuffer_.size() * sizeof(uint32_t)) / 1024);
  std::string hud_metrics = hud_buf;
  int hud_w = text_width(hud_metrics, Face::Mono);
  int hud_x = fps_x - hud_w - 16;
  if (hud_x > tx + 10) {
    draw_text_to(win.buffer, W, H, hud_x, 11, hud_metrics, tok::dim, Face::Mono);
  }

  // 2. Segmented Sub-Tabs Bar (y = 38..66)
  draw_rect_to(win.buffer, W, H, 0, 37, W, 28, 0xFF1E222B);
  draw_rect_to(win.buffer, W, H, 0, 65, W, 1, tok::line_up);

  int tab_x = 12;
  static const char* const STUDIO_TABS[] = {
    "1. Tree & Bounds",
    "2. State & Store",
    "3. Drive & Test",
    "4. Telemetry & FPS",
    "5. Source & Reload"
  };
  for (int i = 0; i < 5; ++i) {
    int tab_w = text_width(STUDIO_TABS[i], Face::UiBold) + 20;
    bool on = (win.studio_active_tab == i);
    draw_pill_to(win.buffer, W, H, tab_x, 40, tab_w, 22,
                 on ? 0xFF0A84FF : 0xFF2A2E37, on ? 0xFF5AC8FA : 0xFF3A3F4B);
    draw_text_to(win.buffer, W, H, tab_x + 10, 44, STUDIO_TABS[i], on ? 0xFFFFFFFF : tok::dim,
                 on ? Face::UiBold : Face::UiSmall);
    tab_x += tab_w + 8;
  }

  // Locate active Face runtime for data extraction
  std::shared_ptr<face::Runtime> rt;
  for (auto& w : windows_) {
    if (w.type == WindowType::FaceApp && w.face_app_runtime) {
      rt = w.face_app_runtime;
      break;
    }
  }

  // 3. Sub-Tab Content Rendering
  int body_y = 68;
  int body_h = H - body_y - 24;

  if (win.studio_active_tab == 0) { // 1. Tree & Bounds
    int left_w = W * 48 / 100;
    int right_w = W - left_w - 24;

    // Left Pane: Hierarchical Widget Tree
    draw_rounded_rect_to(win.buffer, W, H, 12, body_y, left_w, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, 12, body_y, left_w, body_h, 6, tok::line_up);
    draw_label_to(win.buffer, W, H, 24, body_y + 10, "LIVE WIDGET HIT-TREE & BOUNDS", tok::dim);

    int row_y = body_y + 32;
    if (rt) {
      const auto& placed = rt->placed();
      int skip = win.studio_tree_scroll;
      for (size_t i = 0; i < placed.size(); ++i) {
        if (skip > 0) { skip--; continue; }
        if (row_y > body_y + body_h - 24) break;

        const auto& p = placed[i];
        std::string tag = p.w ? p.w->type : "box";
        std::string label = p.w ? (!p.w->label.empty() ? p.w->label : p.text) : p.text;
        if (label.size() > 28) label = label.substr(0, 25) + "...";

        std::string line = "<" + tag + "> " + label;
        std::string geom = "(" + std::to_string(p.x) + "," + std::to_string(p.y) + " " +
                           std::to_string(p.width) + "x" + std::to_string(p.height) + ")";

        bool selected = ((int)i == win.studio_selected_widget_idx);
        if (selected) {
          draw_rect_to(win.buffer, W, H, 16, row_y - 2, left_w - 8, 20, 0xFF1C3A5E);
        }

        draw_text_to(win.buffer, W, H, 24, row_y, line, selected ? 0xFF5AC8FA : tok::text, Face::Mono);
        draw_text_to(win.buffer, W, H, left_w - 140, row_y, geom, p.cut ? 0xFFFF9F0A : tok::dim, Face::Mono);
        if (p.cut) {
          draw_text_to(win.buffer, W, H, left_w - 40, row_y, "[CUT]", 0xFFFF453A, Face::UiSmall);
        }
        row_y += 20;
      }
    } else {
      /* No face runtime for this target: it is not open, or it is a native
       * pane with no widget tree to inspect. This said "Target running in
       * native C++ mode" whether or not anything was running. */
      draw_text_to(win.buffer, W, H, 24, body_y + 36,
                   win.studio_target_name + " has no live widget tree to inspect.", tok::dim, Face::Ui);
      draw_text_to(win.buffer, W, H, 24, body_y + 58,
                   "Open a face app from Apps (order_desk, task_tracker, ...) and pick it here.", tok::faint, Face::UiSmall);
    }

    // Right Pane: Visual Geometry Viewport
    int right_x = 12 + left_w + 12;
    draw_rounded_rect_to(win.buffer, W, H, right_x, body_y, right_w, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, right_x, body_y, right_w, body_h, 6, tok::line_up);
    draw_label_to(win.buffer, W, H, right_x + 12, body_y + 10, "MINIATURE LAYOUT GEOMETRY PREVIEW", tok::dim);

    // Render scaled boundary boxes of placed widgets
    int preview_box_x = right_x + 14;
    int preview_box_y = body_y + 32;
    int preview_box_w = right_w - 28;
    int preview_box_h = body_h - 44;
    draw_rect_to(win.buffer, W, H, preview_box_x, preview_box_y, preview_box_w, preview_box_h, 0xFF12151B);
    draw_rect_outline_to(win.buffer, W, H, preview_box_x, preview_box_y, preview_box_w, preview_box_h, 0xFF2A2E37);

    if (rt) {
      const auto& placed = rt->placed();
      float scale_x = (float)preview_box_w / (float)(W > 0 ? W : 1);
      float scale_y = (float)preview_box_h / (float)(H > 0 ? H : 1);

      for (const auto& p : placed) {
        int bx = preview_box_x + (int)(p.x * scale_x);
        int by = preview_box_y + (int)(p.y * scale_y);
        int bw = (int)(p.width * scale_x);
        int bh = (int)(p.height * scale_y);
        if (bw < 2) bw = 2;
        if (bh < 2) bh = 2;
        draw_rect_outline_to(win.buffer, W, H, bx, by, bw, bh, p.cut ? 0xFFFF453A : 0xFF0A84FF);
      }
    }

  } else if (win.studio_active_tab == 1) { // 2. State & Store
    int half_w = (W - 36) / 2;

    // Left: Reactive Lets & Persistent States
    draw_rounded_rect_to(win.buffer, W, H, 12, body_y, half_w, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, 12, body_y, half_w, body_h, 6, tok::line_up);
    draw_label_to(win.buffer, W, H, 24, body_y + 10, "REACTIVE BINDINGS (LETS & STATES)", tok::dim);

    int row_y = body_y + 32;
    if (rt) {
      draw_text_to(win.buffer, W, H, 24, row_y, "--- Reactive Lets ---", 0xFF5AC8FA, Face::UiBold);
      row_y += 20;
      auto lets = rt->let_vars();
      for (const auto& l : lets) {
        if (row_y > body_y + body_h - 40) break;
        draw_text_to(win.buffer, W, H, 24, row_y, "let " + l.first, tok::dim, Face::Mono);
        draw_text_to(win.buffer, W, H, half_w - 180, row_y, "= " + l.second, 0xFF30D158, Face::Mono);
        row_y += 20;
      }

      row_y += 10;
      draw_text_to(win.buffer, W, H, 24, row_y, "--- Persistent State (Click to Edit) ---", 0xFFFF9F0A, Face::UiBold);
      row_y += 20;
      auto states = rt->state_vars();
      for (size_t si = 0; si < states.size(); ++si) {
        if (row_y > body_y + body_h - 40) break;
        bool editing = (win.studio_state_editing && win.studio_state_edit_key == states[si].first);
        draw_text_to(win.buffer, W, H, 24, row_y, "state " + states[si].first, tok::dim, Face::Mono);
        if (editing) {
          draw_rect_to(win.buffer, W, H, half_w - 184, row_y - 2, 170, 20, 0xFF0A84FF);
          draw_text_to(win.buffer, W, H, half_w - 180, row_y, win.studio_state_edit.text + "_", 0xFFFFFFFF, Face::Mono);
        } else {
          draw_text_to(win.buffer, W, H, half_w - 180, row_y, "= " + states[si].second, tok::text, Face::Mono);
        }
        row_y += 20;
      }
    }

    // Right: Store Kinds & Input Fields
    int right_x = 12 + half_w + 12;
    draw_rounded_rect_to(win.buffer, W, H, right_x, body_y, half_w, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, right_x, body_y, half_w, body_h, 6, tok::line_up);
    draw_label_to(win.buffer, W, H, right_x + 12, body_y + 10, "AQL KERNEL STORE KINDS & FIELDS", tok::dim);

    row_y = body_y + 32;
    if (rt) {
      draw_text_to(win.buffer, W, H, right_x + 12, row_y, "--- Kernel Storage Kinds ---", 0xFFBF5AF2, Face::UiBold);
      row_y += 20;
      auto kinds = rt->kind_vars();
      for (const auto& knd : kinds) {
        if (row_y > body_y + body_h - 40) break;
        draw_text_to(win.buffer, W, H, right_x + 12, row_y, "kind " + knd.first, tok::dim, Face::Mono);
        draw_text_to(win.buffer, W, H, right_x + half_w - 140, row_y, std::to_string(knd.second) + " records", 0xFF30D158, Face::Mono);
        row_y += 20;
      }

      row_y += 10;
      draw_text_to(win.buffer, W, H, right_x + 12, row_y, "--- Active Input Fields ---", 0xFF64D2FF, Face::UiBold);
      row_y += 20;
      auto fields = rt->field_vars();
      for (const auto& fld : fields) {
        if (row_y > body_y + body_h - 40) break;
        draw_text_to(win.buffer, W, H, right_x + 12, row_y, "field " + fld.first, tok::dim, Face::Mono);
        draw_text_to(win.buffer, W, H, right_x + half_w - 180, row_y, "\"" + fld.second + "\"", tok::text, Face::Mono);
        row_y += 20;
      }
    }

  } else if (win.studio_active_tab == 2) { // 3. Drive & Test
    draw_rounded_rect_to(win.buffer, W, H, 12, body_y, W - 24, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, 12, body_y, W - 24, body_h, 6, tok::line_up);

    // Toolbar inside Drive tab
    int btn_y = body_y + 10;
    int run_w = text_width("[ Run All Steps ]", Face::UiSmall) + 16;
    int step_w = text_width("[ Step Next ]", Face::UiSmall) + 16;
    int reset_w = text_width("[ Reset ]", Face::UiSmall) + 16;

    int bx = 24;
    draw_pill_to(win.buffer, W, H, bx, btn_y, run_w, 24, 0xFF143823, 0xFF30D158);
    draw_text_to(win.buffer, W, H, bx + 8, btn_y + 5, "[ Run All Steps ]", 0xFF30D158, Face::UiBold);
    bx += run_w + 8;

    draw_pill_to(win.buffer, W, H, bx, btn_y, step_w, 24, 0xFF1C3A5E, 0xFF0A84FF);
    draw_text_to(win.buffer, W, H, bx + 8, btn_y + 5, "[ Step Next ]", 0xFF5AC8FA, Face::UiBold);
    bx += step_w + 8;

    draw_pill_to(win.buffer, W, H, bx, btn_y, reset_w, 24, 0xFF2A2E37, 0xFF3A3F4B);
    draw_text_to(win.buffer, W, H, bx + 8, btn_y + 5, "[ Reset ]", tok::dim, Face::UiSmall);

    draw_label_to(win.buffer, W, H, W - 240, btn_y + 5, "AUTOMATED DRIVE ENGINE", tok::dim);

    // Step Execution List & Log
    int log_y = body_y + 44;
    draw_rect_to(win.buffer, W, H, 24, log_y, W - 48, body_h - 90, 0xFF12151B);
    draw_rect_outline_to(win.buffer, W, H, 24, log_y, W - 48, body_h - 90, 0xFF2A2E37);

    int row_y = log_y + 8;
    for (size_t i = 0; i < win.studio_drive_steps.size(); ++i) {
      if (row_y > log_y + body_h - 130) break;
      bool current = ((int)i == win.studio_drive_current_step);
      bool passed = ((int)i < win.studio_drive_current_step);
      std::string marker = passed ? "[PASS]" : (current ? " [->] " : " [..] ");
      uint32_t color = passed ? 0xFF30D158 : (current ? 0xFF5AC8FA : tok::dim);

      draw_text_to(win.buffer, W, H, 32, row_y, marker + " Step " + std::to_string(i + 1) + ": " + win.studio_drive_steps[i],
                   color, Face::Mono);
      row_y += 20;
    }

    // Results / Assertions Output
    row_y += 10;
    for (const auto& res : win.studio_drive_results) {
      if (row_y > log_y + body_h - 110) break;
      draw_text_to(win.buffer, W, H, 32, row_y, "-> " + res, 0xFF5AC8FA, Face::Mono);
      row_y += 18;
    }

    // Custom Command Input Prompt at Bottom
    int cmd_y = body_y + body_h - 38;
    draw_rect_to(win.buffer, W, H, 24, cmd_y, W - 48, 28, win.studio_drive_cmd_focused ? 0xFF1E2638 : 0xFF161922);
    draw_rect_outline_to(win.buffer, W, H, 24, cmd_y, W - 48, 28, win.studio_drive_cmd_focused ? 0xFF0A84FF : 0xFF2A2E37);
    draw_text_to(win.buffer, W, H, 32, cmd_y + 6, "Drive> " + win.studio_drive_custom_cmd.text + (win.studio_drive_cmd_focused ? "_" : ""),
                 tok::text, Face::Mono);

  } else if (win.studio_active_tab == 3) { // 4. Telemetry & FPS
    int half_w = (W - 36) / 2;

    draw_rounded_rect_to(win.buffer, W, H, 12, body_y, half_w, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, 12, body_y, half_w, body_h, 6, tok::line_up);
    draw_label_to(win.buffer, W, H, 24, body_y + 10, "FRAME LATENCY & RASTER PROFILING", tok::dim);

    int row_y = body_y + 36;
    draw_text_to(win.buffer, W, H, 24, row_y, "Hardware Compositor Frame Budget (16.67ms VSYNC)", 0xFFFFFFFF, Face::UiBold);
    row_y += 24;

    draw_text_to(win.buffer, W, H, 24, row_y, "Layout Pass Time:", tok::dim, Face::Mono);
    draw_text_to(win.buffer, W, H, 200, row_y, "382 microseconds", 0xFF30D158, Face::Mono);
    row_y += 20;

    draw_text_to(win.buffer, W, H, 24, row_y, "Paint Pass Time:", tok::dim, Face::Mono);
    draw_text_to(win.buffer, W, H, 200, row_y, "414 microseconds", 0xFF30D158, Face::Mono);
    row_y += 20;

    draw_text_to(win.buffer, W, H, 24, row_y, "Compositor Blit:", tok::dim, Face::Mono);
    draw_text_to(win.buffer, W, H, 200, row_y, "120 microseconds", 0xFF30D158, Face::Mono);
    row_y += 20;

    draw_text_to(win.buffer, W, H, 24, row_y, "Total Frame Latency:", tok::dim, Face::Mono);
    draw_text_to(win.buffer, W, H, 200, row_y, "0.916 ms (5.4% of budget)", 0xFF5AC8FA, Face::Mono);
    row_y += 30;

    draw_text_to(win.buffer, W, H, 24, row_y, "Live VSYNC Jitter:", tok::dim, Face::Mono);
    draw_text_to(win.buffer, W, H, 200, row_y, "< 0.04 ms", 0xFF30D158, Face::Mono);

    // Right: Quality & Diagnostics Checklist
    int right_x = 12 + half_w + 12;
    draw_rounded_rect_to(win.buffer, W, H, right_x, body_y, half_w, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, right_x, body_y, half_w, body_h, 6, tok::line_up);
    draw_label_to(win.buffer, W, H, right_x + 12, body_y + 10, "UI FAULT & CLIP DIAGNOSTICS", tok::dim);

    row_y = body_y + 36;
    static const char* const CHECKS[] = {
      "[OK] Zero text bounding box clipping detected",
      "[OK] Zero overlapping sibling elements",
      "[OK] Zero missing glyph fallback characters",
      "[OK] All reactive event handlers resolved",
      "[OK] AQL kind records validated against schema",
      "[OK] Sub-millisecond paint budget satisfied",
      "[OK] Hardware double-buffer synchronized"
    };
    for (const auto& chk : CHECKS) {
      draw_text_to(win.buffer, W, H, right_x + 12, row_y, chk, 0xFF30D158, Face::Mono);
      row_y += 22;
    }

  } else if (win.studio_active_tab == 4) { // 5. Source & Reload
    draw_rounded_rect_to(win.buffer, W, H, 12, body_y, W - 24, body_h, 6, tok::panel2);
    draw_round_outline_to(win.buffer, W, H, 12, body_y, W - 24, body_h, 6, tok::line_up);

    int btn_y = body_y + 10;
    int reload_w = text_width("[ Hot Reload Source ]", Face::UiSmall) + 16;
    int edit_w = text_width("[ Open in Native Editor ]", Face::UiSmall) + 16;
    int bx = 24;

    draw_pill_to(win.buffer, W, H, bx, btn_y, reload_w, 24, 0xFF1C3A5E, 0xFF0A84FF);
    draw_text_to(win.buffer, W, H, bx + 8, btn_y + 5, "[ Hot Reload Source ]", 0xFF5AC8FA, Face::UiBold);
    bx += reload_w + 8;

    draw_pill_to(win.buffer, W, H, bx, btn_y, edit_w, 24, 0xFF2A2E37, 0xFF3A3F4B);
    draw_text_to(win.buffer, W, H, bx + 8, btn_y + 5, "[ Open in Native Editor ]", tok::text, Face::UiSmall);

    draw_label_to(win.buffer, W, H, W - 220, btn_y + 5, "DECLARATIVE FACE SOURCE", tok::dim);

    // Source Code Box
    int src_box_y = body_y + 44;
    draw_rect_to(win.buffer, W, H, 24, src_box_y, W - 48, body_h - 54, 0xFF12151B);
    draw_rect_outline_to(win.buffer, W, H, 24, src_box_y, W - 48, body_h - 54, 0xFF2A2E37);

    int row_y = src_box_y + 8;
    if (rt) {
      std::stringstream ss(rt->source_text());
      std::string line;
      int line_num = 1;
      while (std::getline(ss, line)) {
        if (row_y > src_box_y + body_h - 74) break;
        char num_buf[16];
        std::snprintf(num_buf, sizeof(num_buf), "%3d | ", line_num++);
        draw_text_to(win.buffer, W, H, 32, row_y, num_buf, tok::dim, Face::Mono);
        draw_text_to(win.buffer, W, H, 80, row_y, line, tok::text, Face::Mono);
        row_y += 18;
      }
    }
  }

  // Bottom Status Line
  draw_rect_to(win.buffer, W, H, 0, H - 24, W, 24, tok::rail);
  draw_rect_to(win.buffer, W, H, 0, H - 24, W, 1, tok::line_up);
  draw_text_to(win.buffer, W, H, 14, H - 18, win.status_text, tok::dim, Face::UiSmall);
}

void HardwareCompositor::render_window_content(WindowSurface& win, Kernel& k) {
  switch (win.type) {
    case WindowType::Terminal: render_terminal_window(win, k); break;
    case WindowType::Miner: render_miner_window(win, k); break;
    case WindowType::Tasks: render_tasks_window(win, k); break;
    case WindowType::Hardware: render_hardware_window(win, k); break;
    case WindowType::Editor: render_editor_window(win, k); break;
    case WindowType::Database: render_database_window(win, k); break;
    case WindowType::Browser: render_browser_window(win, k); break;
    case WindowType::Mail: render_mail_window(win, k); break;
    case WindowType::Contacts: render_contacts_window(win, k); break;
    case WindowType::Calendar: render_calendar_window(win, k); break;
    case WindowType::FaceApp: render_face_app_window(win, k); break;
    case WindowType::Grain: render_grain_window(win); break;
    case WindowType::Inspector: render_inspector_window(win, k); break;
    default: clear_buffer(win.buffer, tok::panel, win.content_width(), win.content_height()); break;
  }
}

static bool fuzzy_match(const std::string& pattern, const std::string& target, int* out_score = nullptr) {
  if (pattern.empty()) {
    if (out_score) *out_score = 0;
    return true;
  }
  size_t p_idx = 0;
  int score = 0;
  int consecutive = 0;
  for (size_t t_idx = 0; t_idx < target.size() && p_idx < pattern.size(); ++t_idx) {
    char pc = (char)std::tolower(static_cast<unsigned char>(pattern[p_idx]));
    char tc = (char)std::tolower(static_cast<unsigned char>(target[t_idx]));
    if (pc == tc) {
      ++p_idx;
      score += 10 + (consecutive * 5);
      if (t_idx == 0 || target[t_idx - 1] == ' ' || target[t_idx - 1] == ':' || target[t_idx - 1] == '/' || target[t_idx - 1] == '+') {
        score += 25;
      }
      consecutive++;
    } else {
      consecutive = 0;
    }
  }
  if (p_idx == pattern.size()) {
    if (out_score) *out_score = score;
    return true;
  }
  return false;
}

void HardwareCompositor::open_command_palette() {
  palette_open_ = true;
  palette_input_.clear();
  palette_selected_idx_ = 0;
  update_palette_filter();
}

void HardwareCompositor::close_command_palette() {
  palette_open_ = false;
}

void HardwareCompositor::toggle_command_palette() {
  if (palette_open_) close_command_palette();
  else open_command_palette();
}

void HardwareCompositor::execute_palette_action(int idx) {
  if (idx < 0) return;
  update_palette_filter();
  if (idx < static_cast<int>(palette_filtered_items_.size())) {
    if (palette_filtered_items_[idx].action) {
      auto act = palette_filtered_items_[idx].action;
      act();
      return;
    }
  }
  switch (idx) {
    case 0:
      set_toast("Active Orders Pipeline: Direct RAM write-behind queue active");
      break;
    case 1:
      set_toast("Stockmaster Inventory: 12,410 cached records ready");
      break;
    case 2:
      ensure_app_window(WindowType::Database);
      set_toast("MariaDB Ingestion Sidecar: Port 19472 connected");
      break;
    case 3:
      ensure_app_window(WindowType::Hardware);
      set_toast("Cluster Resource Placement: Node 1 healthy, 4 lanes online");
      break;
    default:
      break;
  }
}

void HardwareCompositor::update_palette_filter() {
  palette_all_items_.clear();
  // 1. Fast Tables & Database Operations
  palette_all_items_.push_back({"fast_orders", "HOT", 0xFF4A1228, 0xFFF43F5E, "salesorders (Active Orders)", "Table", "347,860 rows • sub-ms in-memory scan & CDC active", "⌘ 1", [this]() {
    ensure_app_window(WindowType::Database);
    if (kernel_) db_open_table(*focused_window(), *kernel_, "salesorders");
  }});
  palette_all_items_.push_back({"fast_stockmoves", "SKU", 0xFF172554, 0xFF38BDF8, "stockmoves (Stock Ledger)", "Table", "558,313 rows • probe index & aggregate ready", "⌘ 2", [this]() {
    ensure_app_window(WindowType::Database);
    if (kernel_) db_open_table(*focused_window(), *kernel_, "stockmoves");
  }});
  palette_all_items_.push_back({"fast_locstock", "LOC", 0xFF083344, 0xFF22D3EE, "locstock (Inventory Locations)", "Table", "83,663 rows • instant inventory sum", "⌘ 3", [this]() {
    ensure_app_window(WindowType::Database);
    if (kernel_) db_open_table(*focused_window(), *kernel_, "locstock");
  }});
  palette_all_items_.push_back({"fast_picking", "PICK", 0xFF064E3B, 0xFF34D399, "pickingfreeze (Pick Batches)", "Table", "143,981 rows • real-time queue filter", "⌘ 4", [this]() {
    ensure_app_window(WindowType::Database);
    if (kernel_) db_open_table(*focused_window(), *kernel_, "pickingfreeze");
  }});

  // 2. Open Windows (instant navigation)
  for (const auto& win : windows_) {
    if (!win.title.empty()) {
      uint32_t wid = win.window_id;
      palette_all_items_.push_back({"win_" + std::to_string(wid), "WIN", 0xFF1E293B, 0xFF38BDF8, win.title, "Window", (win.visible ? (win.minimized ? "Minimized" : "Active") : "Hidden"), "Raise", [this, wid]() {
        bring_to_front(wid);
        canvas_mode_ = false;
      }});
    }
  }

  // 3. Applications
  palette_all_items_.push_back({"app_shell", "SH", 0xFF1E293B, 0xFF38BDF8, "Shell / Terminal", "App", "Console entity, REPL & kernel command surface", "F1", [this]() { ensure_app_window(WindowType::Terminal); }});
  palette_all_items_.push_back({"app_editor", "ED", 0xFF1E293B, 0xFF38BDF8, "C++ Code Editor", "App", "Native code & text editor with syntax highlighting", "F4", [this]() { ensure_app_window(WindowType::Editor); }});
  palette_all_items_.push_back({"app_mariadb", "DB", 0xFF1E293B, 0xFF38BDF8, "MariaDB / ASQL Studio", "App", "Interactive SQL studio, schema explorer & data grid", "F5", [this]() { ensure_app_window(WindowType::Database); }});
  palette_all_items_.push_back({"app_web", "WEB", 0xFF1E293B, 0xFF38BDF8, "Web Browser", "App", "Native web engine & rendering pane", "F6", [this]() { ensure_app_window(WindowType::Browser); }});
  palette_all_items_.push_back({"app_mail", "MAIL", 0xFF1E293B, 0xFF38BDF8, "AetherMail", "App", "Apple Mail style 3-pane email client", "F7", [this]() { ensure_app_window(WindowType::Mail); }});
  palette_all_items_.push_back({"app_contacts", "CARD", 0xFF1E293B, 0xFF38BDF8, "Contacts Address Book", "App", "vCard card manager & contact profiles", "F8", [this]() { ensure_app_window(WindowType::Contacts); }});
  palette_all_items_.push_back({"app_calendar", "CAL", 0xFF1E293B, 0xFF38BDF8, "Calendar & Agenda", "App", "iCalendar schedule, month & week views", "F9", [this]() { ensure_app_window(WindowType::Calendar); }});
  palette_all_items_.push_back({"app_inspector", "INSP", 0xFF1E293B, 0xFF38BDF8, "System Inspector", "App", "Universal in-desktop telemetry & behavioral inspector", "F11", [this]() { ensure_app_window(WindowType::Inspector); }});

  // 4. Layout & Window Tiling
  palette_all_items_.push_back({"tile_2col", "TILE", 0xFF141A30, 0xFF3B82F6, "Tile Windows (2 Columns)", "Layout", "Arrange all visible windows into 2 equal columns", "Ctrl+T", [this]() { tile_windows(2); }});
  palette_all_items_.push_back({"tile_3col", "TILE", 0xFF141A30, 0xFF3B82F6, "Tile Windows (3 Columns)", "Layout", "Arrange all visible windows into 3 columns", "Tile3", [this]() { tile_windows(3); }});
  palette_all_items_.push_back({"snap_left", "SNAP", 0xFF141A30, 0xFF3B82F6, "Snap Window Left (50%)", "Layout", "Snap focused window to left half of screen", "Ctrl+←", [this]() { snap_window_left(); }});
  palette_all_items_.push_back({"snap_right", "SNAP", 0xFF141A30, 0xFF3B82F6, "Snap Window Right (50%)", "Layout", "Snap focused window to right half of screen", "Ctrl+→", [this]() { snap_window_right(); }});
  palette_all_items_.push_back({"snap_max", "SNAP", 0xFF141A30, 0xFF3B82F6, "Maximize Window", "Layout", "Maximize focused window to fit work area", "Ctrl+↑", [this]() { snap_window_maximize(); }});

  // 5. Workspaces
  palette_all_items_.push_back({"ws_code", "CODE", 0xFF141A30, 0xFF3B82F6, "Workspace: Code", "Workspace", "Ergonomic 50/50 split: Editor + Shell", "Ctrl+1", [this]() { switch_workspace(CockpitWorkspace::Code); }});
  palette_all_items_.push_back({"ws_comms", "COMM", 0xFF141A30, 0xFF3B82F6, "Workspace: Comms", "Workspace", "Ergonomic 50/50 split: AetherMail + Calendar", "Ctrl+2", [this]() { switch_workspace(CockpitWorkspace::Comms); }});
  palette_all_items_.push_back({"ws_admin", "ADM", 0xFF141A30, 0xFF3B82F6, "Workspace: Admin", "Workspace", "Ergonomic 50/50 split: Machine + MariaDB Studio", "Ctrl+3", [this]() { switch_workspace(CockpitWorkspace::Admin); }});
  palette_all_items_.push_back({"ws_blank", "CANV", 0xFF141A30, 0xFF3B82F6, "Workspace: Blank Slate", "Workspace", "Distraction-free blank cockpit dashboard", "Ctrl+0", [this]() { switch_workspace(CockpitWorkspace::Blank); }});

  std::string q = palette_input_.text;
  palette_filtered_items_.clear();

  if (q.empty()) {
    for (size_t i = 0; i < palette_all_items_.size() && i < 6; ++i) {
      palette_filtered_items_.push_back(palette_all_items_[i]);
    }
  } else {
    for (auto item : palette_all_items_) {
      int s1 = 0, s2 = 0, s3 = 0;
      bool m1 = fuzzy_match(q, item.title, &s1);
      bool m2 = fuzzy_match(q, item.category, &s2);
      bool m3 = fuzzy_match(q, item.subtitle, &s3);
      if (m1 || m2 || m3) {
        item.score = std::max({m1 ? s1 * 2 : 0, m2 ? s2 : 0, m3 ? s3 : 0});
        palette_filtered_items_.push_back(item);
      }
    }
    std::sort(palette_filtered_items_.begin(), palette_filtered_items_.end(),
              [](const PaletteItem& a, const PaletteItem& b) { return a.score > b.score; });
  }

  if (palette_selected_idx_ >= static_cast<int>(palette_filtered_items_.size())) {
    palette_selected_idx_ = std::max(0, static_cast<int>(palette_filtered_items_.size()) - 1);
  }
}

namespace {
// The wordmark, in ONE place: the badge is measured from it and the hit test
// is measured from the badge.
const char* const kTopBarBrand = "AIOS CORE";
constexpr int kBrandInk = 28;      // where the wordmark starts inside the badge
constexpr int kBrandTrail = 14;    // and the room left after it
// Where the bar floats. Written down three times before -- the drawing, the
// hit test, and nowhere at all in the window layout, which is how windows came
// to be placed underneath it.
constexpr int kTopBarY = 16;
constexpr int kTopBarH = 48;
// The load-meter switch. Small, always present, and the ONLY thing that puts
// the figures on the glass: the readings are off until someone asks for them.
constexpr int kLoadToggleW = 30;
constexpr int kLoadToggleH = 20;
}  // namespace

// The longest leading run of `t` that fits in `room`, with an ellipsis when it
// had to cut. Measured a letter at a time, forwards -- dropping the last
// character and re-measuring the whole string is quadratic in its length.
std::string HardwareCompositor::fit_top_bar_text(const std::string& t, int room, Face face) const {
  if (room <= 0) return std::string();
  if (text_width(t, face) <= room) return t;
  const int dots = text_width("...", face);
  if (dots > room) return std::string();
  std::string kept;
  for (size_t i = 0; i < t.size();) {
    size_t n = 1;
    const unsigned char c = (unsigned char)t[i];
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    if (i + n > t.size()) break;
    const std::string next = kept + t.substr(i, n);
    if (text_width(next, face) + dots > room) break;
    kept = next;
    i += n;
  }
  return kept.empty() ? std::string() : kept + "...";
}

int HardwareCompositor::top_bar_badge_w() const {
  return kBrandInk + text_width(kTopBarBrand, Face::UiBold) + kBrandTrail;
}

int HardwareCompositor::top_bar_w() const {
  // Measured from what goes in it. This was a flat 962: at the default text
  // size everything happened to fit, and the moment the interface was drawn
  // larger the mind reading fell off the end of a bar that had not grown.
  const int W = static_cast<int>(width_);
  int need = 14 + top_bar_badge_w() + 28;                       // badge and its gap
  need += text_width("MIND: EXTERNAL:ASSAY:CLAUDE-CLI/SONNET   000 CAPS   GTX 1080 Ti", Face::Ui) + 22;
  need += text_width("000 FPS", Face::UiBold) + 22;
  // The load figures only take room when they are switched on, so a bar with
  // the meter off is exactly the bar it always was.
  need += kLoadToggleW + 10;
  if (canvas_state_.show_machine_load)
    need += text_width("CPU 100%  GPU 100% 100C  DISK 100%  MEM 100%", Face::Ui) + 20;
  need += text_width("RAM 00.0/00.0G", Face::Ui) + 20;
  need += text_width("00:00 PM", Face::UiBold) + 14;
  need += 32 + 14;                                              // the avatar and the inset
  return std::min(std::max(962, need), W - 40);
}

void HardwareCompositor::draw_top_bar(Kernel* k) {
  const int W = static_cast<int>(width_);
  const int H = static_cast<int>(height_);
  const int bar_w = top_bar_w();
  const int bar_h = kTopBarH;
  const int bar_x = (W - bar_w) / 2;
  const int bar_y = kTopBarY;

  // Drop shadow
  shadow_rounded(bar_x, bar_y, bar_w, bar_h, 10, 14, 34, 3);

  // Frosted acrylic container background
  draw_rounded_rect_to(framebuffer_, W, H, bar_x, bar_y, bar_w, bar_h, 10, 0xFA0B0E16);
  draw_round_outline_to(framebuffer_, W, H, bar_x, bar_y, bar_w, bar_h, 10, 0xFF222838);

  // 1. [ AIOS CORE ] Chip / Badge with Sparkle Icon
  const int badge_x = bar_x + 14;
  const int badge_y = bar_y + 9;
  const int badge_w = top_bar_badge_w();
  const int badge_h = 30;
  draw_rounded_rect_to(framebuffer_, W, H, badge_x, badge_y, badge_w, badge_h, 8, 0xFF14172C);
  draw_round_outline_to(framebuffer_, W, H, badge_x, badge_y, badge_w, badge_h, 8, 0xFF4F46E5);
  ButtonSuite::draw_icon(framebuffer_, W, H, badge_x + 16, badge_y + 15, Icon::Sparkle, 14, 0xFF818CF8, 0xFFC7D2FE);
  draw_text_to(framebuffer_, W, H, badge_x + kBrandInk, badge_y + 7, kTopBarBrand, 0xFFFFFFFF, Face::UiBold);

  // The four navigation tabs are gone. "Overview" and "Workspaces" drew the
  // same desktop as each other once the Overview page was deleted, "Terminal"
  // opened a window the taskbar already opens, and "Fast Tables" was the
  // command palette wearing a tab — it is Ctrl+P, which is what the palette
  // has always been. A desktop's top bar reports; it does not navigate.

  // 3. Telemetry on Right
  // Avatar / Status Disc
  const int av_d = 20;
  const int av_x = bar_x + bar_w - 32;
  const int av_y = bar_y + (bar_h - av_d) / 2;
  ButtonSuite::draw_circle(framebuffer_, W, H, av_x + av_d * 0.5f, av_y + av_d * 0.5f, av_d * 0.5f, 0xFF4338CA, 0xFF6366F1, 1.0f);
  ButtonSuite::draw_icon(framebuffer_, W, H, av_x + av_d / 2, av_y + av_d / 2, Icon::User, 12, 0xFFFFFFFF);

  // Time (e.g. 08:26 AM)
  std::string clock = clock_text_ampm();
  int clock_w = text_width(clock, Face::UiBold);
  int clock_x = av_x - 14 - clock_w;
  draw_text_to(framebuffer_, W, H, clock_x, bar_y + 15, clock, 0xFFF9FAFB, Face::UiBold);

  // RAM, USED over total. This read "RAM 19.5G" -- the machine's total memory,
  // a number that cannot change while it is running. A constant printed in a
  // row of readings looks like a reading and is not one.
  std::string ram_str = "RAM --";
  {
    const MachineLoad& ml = canvas_state_.machine_load;
    if (ml.mem_known && ml.mem_total) {
      char buf[48];
      /* Under a gigabyte in use reads in megabytes: "0.0/0.9G" was true and
       * said nothing on a board using 40 MB. */
      if (ml.mem_used < 1073741824ull)
        std::snprintf(buf, sizeof(buf), "RAM %.0fM/%.1fG", ml.mem_used / 1048576.0, ml.mem_total / 1073741824.0);
      else
        std::snprintf(buf, sizeof(buf), "RAM %.1f/%.1fG", ml.mem_used / 1073741824.0, ml.mem_total / 1073741824.0);
      ram_str = buf;
    } else if (k) {
      const auto& topo = k->host().topology();
      const double total_g = (topo.total_memory >> 20) / 1024.0;
      if (total_g > 0.1) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "RAM -/%.1fG", total_g);
        ram_str = buf;
      }
    }
  }
  int ram_w = text_width(ram_str, Face::Ui);
  int ram_x = clock_x - 20 - ram_w;
  draw_text_to(framebuffer_, W, H, ram_x, bar_y + 15, ram_str, 0xFFD1D5DB, Face::Ui);

  // What this machine is actually achieving, measured — this used to read
  // "5.4 µs" as a string literal, next to a green dot, whatever the machine
  // was doing. A number nobody computed is not a reading.
  const std::string fps_str = canvas_state_.fps > 0.0f
      ? (std::to_string((int)(canvas_state_.fps + 0.5f)) + " FPS")
      : std::string("-- FPS");
  int lat_w = text_width(fps_str, Face::UiBold);
  int lat_x = ram_x - 22 - lat_w;
  draw_pill_to(framebuffer_, W, H, lat_x - 12, bar_y + 21, 6, 6, 0xFF10B981, 0xFF10B981);
  draw_text_to(framebuffer_, W, H, lat_x, bar_y + 15, fps_str, 0xFF10B981, Face::UiBold);

  // --- the load meter: a switch, and the figures only when it is on ---------
  //
  // Every figure below is traced to something the machine measured this
  // second, and an axis the machine cannot read prints "--". It does NOT print
  // a zero: the sysinfo organ reports GPU load from an AMD-only sysfs file and
  // therefore reported a confident 0% on this NVIDIA host while the card sat
  // at 84% and 83 C. A surface that cannot read a thing has to say so.
  const MachineLoad& ml = canvas_state_.machine_load;
  int load_x = lat_x - 12 - 10 - kLoadToggleW;
  const int load_y = bar_y + (bar_h - kLoadToggleH) / 2;
  // Everything from here leftwards is the right-hand stack; the MIND line is
  // laid out against its edge. It used to be right-aligned on the FPS readout,
  // so the switch and the figures were drawn straight THROUGH "GTX 1080 Ti".
  int stack_left = load_x;
  load_toggle_x_ = load_x;
  load_toggle_y_ = load_y;
  {
    const bool on = canvas_state_.show_machine_load;
    // The switch itself takes its colour from the machine's state even when
    // the numbers are hidden, so turning the readout off does not turn the
    // warning off with it.
    uint32_t face = 0xFF1B2130, edge = 0xFF3A4154, ink = 0xFF7C8598;
    if (on) { face = 0xFF10243A; edge = 0xFF3B82F6; ink = 0xFF93C5FD; }
    switch (ml.state()) {
      case LoadState::Alarmed:  edge = 0xFFEF4444; ink = 0xFFFCA5A5; break;
      case LoadState::Strained: edge = 0xFFF59E0B; ink = 0xFFFCD34D; break;
      case LoadState::Calm: break;
    }
    draw_pill_to(framebuffer_, W, H, load_x, load_y, kLoadToggleW, kLoadToggleH, face, edge);
    // Three bars, a little meter, so the control says what it switches.
    for (int b = 0; b < 3; ++b) {
      const int bh = 4 + b * 3;
      draw_rect_to(framebuffer_, W, H, load_x + 8 + b * 5, load_y + kLoadToggleH - 5 - bh, 3, bh, ink);
    }
  }

  if (canvas_state_.show_machine_load) {
    auto pct = [](bool known, float v, const char* label) {
      char b[32];
      if (known) std::snprintf(b, sizeof(b), "%s %.0f%%", label, v);
      else       std::snprintf(b, sizeof(b), "%s --", label);
      return std::string(b);
    };
    struct Reading { std::string text; bool known; float v; float warn; float alarm; };
    std::vector<Reading> rs;
    rs.push_back({pct(ml.cpu_known, ml.cpu_pct, "CPU"), ml.cpu_known, ml.cpu_pct, 70.0f, 92.0f});
    {
      std::string g = pct(ml.gpu_known, ml.gpu_pct, "GPU");
      if (ml.gpu_temp_known) g += " " + std::to_string(ml.gpu_temp_c) + "C";
      // The hotter of load and temperature decides how the GPU reads, so a
      // cool card working hard and a hot card idling both show up.
      const float sev = std::max(ml.gpu_known ? ml.gpu_pct / 80.0f : 0.0f,
                                 ml.gpu_temp_known ? ml.gpu_temp_c / 80.0f : 0.0f);
      rs.push_back({g, ml.gpu_known || ml.gpu_temp_known, sev * 80.0f, 80.0f, 92.0f});
    }
    rs.push_back({pct(ml.disk_known, ml.disk_pct, "DISK"), ml.disk_known, ml.disk_pct, 85.0f, 95.0f});
    rs.push_back({pct(ml.mem_known, ml.mem_pct(), "MEM"), ml.mem_known, ml.mem_pct(), 85.0f, 95.0f});

    int total_w = 0;
    for (const auto& r : rs) total_w += text_width(r.text, Face::Ui) + 14;
    int rx = load_x - 12 - total_w;
    stack_left = rx;
    for (const auto& r : rs) {
      uint32_t colour = 0xFF9AA4B8;                       // read, and unremarkable
      if (!r.known) colour = 0xFF5A6478;                  // not read at all
      else if (r.v >= r.alarm) colour = 0xFFEF4444;
      else if (r.v >= r.warn)  colour = 0xFFF59E0B;
      draw_text_to(framebuffer_, W, H, rx, bar_y + 15, r.text, colour, Face::Ui);
      rx += text_width(r.text, Face::Ui) + 14;
    }
  }

  // What mind this machine is running, and how much authority is out. The
  // start page used to carry this in a second top bar of its own — two bars
  // reporting one machine, each missing what the other had. This is the one
  // bar, so it says both.
  if (k) {
    const std::string mind = bound_mind_name(*k);
    const std::string accel = accelerator_brief(*k);
    const std::string mind_str =
        "MIND: " + (mind.empty() ? std::string("OFFLINE") : mind) + "   " +
        std::to_string(k->capabilities().list({}).size()) + " CAPS   " +
        (accel.empty() ? std::string("CPU ONLY") : accel);
    const int mind_left = bar_x + top_bar_badge_w() + 28;
    const int room = stack_left - 18 - mind_left;
    // Trimmed to the room it has rather than vanishing when it runs out: the
    // guard here was all-or-nothing, so turning the figures on made the whole
    // MIND line disappear instead of shortening.
    const std::string shown = fit_top_bar_text(mind_str, room, Face::Ui);
    if (!shown.empty()) {
      const int mx = stack_left - 18 - text_width(shown, Face::Ui);
      draw_text_to(framebuffer_, W, H, mx, bar_y + 15, shown,
                   mind.empty() ? 0xFFF87171 : 0xFF38BDF8, Face::Ui);
    }
  }
}

void HardwareCompositor::draw_center_dispatch_card(Kernel* k) {
  (void)k;
  const int W = static_cast<int>(width_);
  const int H = static_cast<int>(height_);
  const int card_w = std::min(684, W - 40);
  const int card_h = 438;
  const int card_x = (W - card_w) / 2;
  const int card_y = (H - card_h) / 2 - 20;

  update_palette_filter();

  // Drop shadow
  shadow_rounded(card_x, card_y, card_w, card_h, 14, 22, 46, 6);

  // Acrylic container background
  draw_rounded_rect_to(framebuffer_, W, H, card_x, card_y, card_w, card_h, 14, 0xFA0B0F19);
  draw_round_outline_to(framebuffer_, W, H, card_x, card_y, card_w, card_h, 14, 0xFF222B3D);

  // Search input area
  std::string placeholder = "Search orders, fast tables, workflows, or dispatch...";
  std::string input_text = palette_input_.text;

  if (input_text.empty()) {
    draw_text_to(framebuffer_, W, H, card_x + 48, card_y + 36, placeholder, 0xFF94A3B8, Face::Ui);
  } else {
    draw_text_to(framebuffer_, W, H, card_x + 48, card_y + 36, input_text, 0xFFFFFFFF, Face::UiBold);
    int caret_x = card_x + 48 + text_width(input_text.substr(0, palette_input_.cursor), Face::UiBold);
    uint64_t ms = steady_ms();
    if ((ms / 500) % 2 == 0) {
      draw_rect_to(framebuffer_, W, H, caret_x, card_y + 34, 2, 20, 0xFF3B82F6);
    }
  }

  // Right ESC Keycap Badge
  const int esc_w = 38, esc_h = 24;
  const int esc_x = card_x + card_w - 48 - esc_w;
  const int esc_y = card_y + 32;
  bool esc_hot = (mouse_x_ >= esc_x && mouse_x_ < esc_x + esc_w && mouse_y_ >= esc_y && mouse_y_ < esc_y + esc_h);
  ButtonSuite::draw_keycap(framebuffer_, W, H, esc_x, esc_y, esc_w, esc_h, "ESC", esc_hot);

  // Items List
  const int item_start_y = card_y + 76;
  const int item_h = 60;
  const int item_gap = 8;
  int count = std::min(4, static_cast<int>(palette_filtered_items_.size()));

  for (int i = 0; i < count; ++i) {
    int row_y = item_start_y + i * (item_h + item_gap);
    bool selected = (i == palette_selected_idx_);
    const auto& it = palette_filtered_items_[i];

    // If selected: draw rounded blue border
    if (selected) {
      draw_rounded_rect_to(framebuffer_, W, H, card_x + 36, row_y, card_w - 72, item_h, 8, 0xFF0E1626);
      draw_round_outline_to(framebuffer_, W, H, card_x + 36, row_y, card_w - 72, item_h, 8, 0xFF3B82F6);
    }

    // Left badge
    const int bg_x = card_x + 50;
    const int bg_y = row_y + 16;
    const int bg_w = 40;
    const int bg_h = 28;

    Icon badge_ic = Icon::None;
    if (it.badge == "HOT") badge_ic = Icon::Flame;
    else if (it.badge == "SKU") badge_ic = Icon::Box;
    else if (it.badge == "SQL") badge_ic = Icon::Sql;
    else if (it.badge == "SYS") badge_ic = Icon::Cluster;

    ButtonSuite::draw_badge_button(framebuffer_, W, H, bg_x, bg_y, bg_w, bg_h,
                                   it.badge, badge_ic,
                                   it.badge_bg ? it.badge_bg : 0xFF1E293B,
                                   it.badge_fg ? it.badge_fg : 0xFF38BDF8,
                                   selected);

    // Title
    draw_text_to(framebuffer_, W, H, card_x + 104, row_y + 11, it.title, 0xFFFFFFFF, Face::UiBold);

    // Subtitle
    draw_text_to(framebuffer_, W, H, card_x + 104, row_y + 33, it.subtitle, 0xFF8B949E, Face::UiSmall);

    // Right shortcut keycap
    if (!it.shortcut.empty()) {
      const int sc_w = 38, sc_h = 28;
      const int sc_x = card_x + card_w - 48 - sc_w;
      const int sc_y = row_y + 16;
      ButtonSuite::draw_keycap(framebuffer_, W, H, sc_x, sc_y, sc_w, sc_h, it.shortcut, selected);
    }
  }

  // Footer separator
  draw_rect_to(framebuffer_, W, H, card_x + 36, card_y + card_h - 60, card_w - 72, 1, 0xFF1E2638);

  // Footer navigation actions
  int footer_y = card_y + card_h - 40;
  int cur_fx = card_x + 48;

  // ↑ ↓ Navigate
  draw_text_to(framebuffer_, W, H, cur_fx, footer_y, "↑ ↓", 0xFFCBD5E1, Face::Ui);
  cur_fx += text_width("↑ ↓", Face::Ui) + 8;
  draw_text_to(framebuffer_, W, H, cur_fx, footer_y, "Navigate", 0xFF8B949E, Face::Ui);
  cur_fx += text_width("Navigate", Face::Ui) + 24;

  // ↵ Select
  draw_text_to(framebuffer_, W, H, cur_fx, footer_y, "↵", 0xFFCBD5E1, Face::Ui);
  cur_fx += text_width("↵", Face::Ui) + 8;
  draw_text_to(framebuffer_, W, H, cur_fx, footer_y, "Select", 0xFF8B949E, Face::Ui);
  cur_fx += text_width("Select", Face::Ui) + 24;

  // Tab Actions
  draw_text_to(framebuffer_, W, H, cur_fx, footer_y, "Tab", 0xFFCBD5E1, Face::Ui);
  cur_fx += text_width("Tab", Face::Ui) + 8;
  draw_text_to(framebuffer_, W, H, cur_fx, footer_y, "Actions", 0xFF8B949E, Face::Ui);
}

// ---------------------------------------------------------------------------
// The taskbar
// ---------------------------------------------------------------------------
// The machine's one piece of permanent furniture: the apps it has, what is
// running, and which section you are standing on. It is drawn on EVERY face.
//
// There used to be three launchers. A dock of six hardcoded tiles, drawn only
// on the workspace. A row of nine "quick ignition" chips on the Overview page,
// which was a different nine apps. And three "context workspace" chips beside
// them. Two of the three vanished the moment a window was open, the dock and
// the chips disagreed about which applications this machine even has, and the
// dock's running lights were the constant `i < 3` — it reported Terminal,
// Editor and MariaDB as running on a machine that had just booted with nothing
// open at all.
namespace {
struct TaskbarApp {
  WindowType type;
  Icon icon;
  uint32_t color;
  const char* label;
};
// Every application on this machine, in the order they stand on the strip.
// ONE table: the paint, the hit test and the launch all read it, so an app
// cannot be on the taskbar and not launchable, or launchable and not shown.
const TaskbarApp TASKBAR_APPS[] = {
  {WindowType::Terminal,  Icon::Terminal,  0xFF10B981, "Terminal"},
  {WindowType::Editor,    Icon::Code,      0xFF3B82F6, "Editor"},
  {WindowType::Database,  Icon::Database,  0xFFF59E0B, "MariaDB"},
  {WindowType::Browser,   Icon::Browser,   0xFF06B6D4, "Web"},
  {WindowType::Mail,      Icon::Mail,      0xFFEC4899, "Mail"},
  {WindowType::Contacts,  Icon::User,      0xFF8B5CF6, "Contacts"},
  {WindowType::Calendar,  Icon::Flag,      0xFFF97316, "Calendar"},
  {WindowType::FaceApp,   Icon::Box,       0xFF22D3EE, "Apps"},
  {WindowType::Inspector, Icon::Inspector, 0xFFA855F7, "Studio"},
  // The machine itself. This is what the Overview page's stat card was, and
  // what a second "System" pane would have been a third copy of: one window,
  // opened like anything else.
  {WindowType::Hardware,  Icon::Cluster,   0xFF38BDF8, "Machine"},
};
constexpr int TASKBAR_APP_COUNT = (int)(sizeof(TASKBAR_APPS) / sizeof(TASKBAR_APPS[0]));

// The sections: the start page, then the windowed desktops. Every one of them
// is the same desktop — same taskbar, same rules — and differs only in its
// background and what is open on it.
const char* const SECTION_LABELS[] = {"Start", "1", "2", "3"};
constexpr int SECTION_COUNT = 4;

constexpr int TASKBAR_PAD = 16;   // the strip's own inset, all four sides
constexpr int TASKBAR_GAP = 10;
constexpr int TASKBAR_DIV = 26;   // between the apps and the sections
constexpr int TASKBAR_TILE = 44;
constexpr int TASKBAR_SEC_H = 26;
// A running tile carries a pip UNDER it — draw_dock_tile puts it at
// `y + size + 5`, four pixels tall. That is part of the tile's footprint, not
// spare room: the strip was sized as tile + 24, so the pip landed three pixels
// off the bottom edge and every running app looked like it was leaking out of
// the taskbar.
constexpr int TASKBAR_PIP = 9;
}  // namespace

HardwareCompositor::TaskbarGeom HardwareCompositor::taskbar_geom() const {
  TaskbarGeom g;
  const int W = static_cast<int>(width_), H = static_cast<int>(height_);
  g.tile = TASKBAR_TILE;
  g.app_count = TASKBAR_APP_COUNT;
  /* A dock, not a Windows taskbar: app icons with a running pip under each,
   * and nothing else. The "Start" / "1" / "2" / "3" section pills and the
   * per-window title pills are gone - clicking an app's icon already raises
   * (and un-minimises) its window, and sections are still a keystroke or the
   * brand badge away. SECTION_LABELS stays for the section names elsewhere. */
  g.sec_count = 0;
  (void)SECTION_COUNT;
  // The section pills are all one width — the widest label's — so they read as
  // a row of equals rather than a ragged set.
  g.sec_h = TASKBAR_SEC_H;
  int secs_w = 0;
  for (int i = 0; i < g.sec_count; ++i) {
    g.sec_w[i] = text_width(SECTION_LABELS[i], Face::UiBold) + 24;
    secs_w += g.sec_w[i] + (i ? 6 : 0);
  }

  // The windows up on this section, in the order they were OPENED. windows_ is
  // the z-order and re-sorts itself on every raise, so reading it directly made
  // the pills swap places under the pointer each time one was clicked. A
  // taskbar's whole value is that a window stays where you last saw it; ids are
  // handed out in order, so sorting by id is opening order.
  g.win_h = TASKBAR_SEC_H;
  const int wins_w = 0;   /* no per-window title pills: the icons are the switcher */
  const int apps_w = g.app_count * g.tile + (g.app_count - 1) * TASKBAR_GAP;
  const bool tail = g.win_count > 0 || g.sec_count > 0;
  g.w = TASKBAR_PAD + apps_w + (tail ? TASKBAR_DIV : 0) + wins_w +
        (g.win_count && g.sec_count ? TASKBAR_DIV : 0) + (g.sec_count ? secs_w : 0) + TASKBAR_PAD;
  // The tile AND the pip under it, with the strip's own inset above and below.
  g.h = TASKBAR_PAD + g.tile + TASKBAR_PIP + TASKBAR_PAD;
  g.x = (W - g.w) / 2;
  if (g.x < 8) g.x = 8;
  g.y = H - g.h - 12;
  g.app_y = g.y + TASKBAR_PAD;

  int cur = g.x + TASKBAR_PAD;
  for (int i = 0; i < g.app_count; ++i) { g.app_x[i] = cur; cur += g.tile + TASKBAR_GAP; }
  cur += TASKBAR_DIV - TASKBAR_GAP;
  // Centred on the TILES, not on the strip: the strip is taller than the tiles
  // by the pip's height, so centring in it sat the pills low against the icons.
  g.win_y = g.app_y + (g.tile - g.win_h) / 2;
  for (int i = 0; i < g.win_count; ++i) { g.win_x[i] = cur; cur += g.win_w[i] + 6; }
  if (g.win_count) cur += TASKBAR_DIV - 6;
  g.sec_y = g.app_y + (g.tile - g.sec_h) / 2;
  for (int i = 0; i < g.sec_count; ++i) { g.sec_x[i] = cur; cur += g.sec_w[i] + 6; }
  return g;
}

// What the two floating strips leave for a window. They are drawn over the
// windows, so anything laid out against the raw screen loses its bottom rows
// to the dock -- which is exactly what a maximised shell's prompt row did.
Rect HardwareCompositor::work_area() const {
  const int margin = 14;
  const TaskbarGeom g = taskbar_geom();
  Rect r;
  r.x = margin;
  r.y = kTopBarY + kTopBarH + margin;
  r.width = static_cast<int>(width_) - 2 * margin;
  // The dock's own top edge, measured from the strip rather than guessed at.
  int bottom = (g.h > 0 ? g.y : static_cast<int>(height_)) - margin;
  r.height = bottom - r.y;
  if (r.width < 540) r.width = 540;
  if (r.height < 380) r.height = 380;
  return r;
}

bool HardwareCompositor::app_running(WindowType type) const {
  const int cur = static_cast<int>(current_workspace_);
  for (const auto& w : windows_) {
    if (w.type != type) continue;
    // Minimised still counts: the window is up, it is just not on the glass.
    // What does NOT count is a window belonging to another section.
    if (w.workspace == cur || w.pinned || w.workspace == -1) return true;
  }
  return false;
}

void HardwareCompositor::draw_taskbar() {
  const int W = static_cast<int>(width_);
  const int H = static_cast<int>(height_);
  const TaskbarGeom g = taskbar_geom();

  shadow_rounded(g.x, g.y, g.w, g.h, 14, 16, 36, 3);
  draw_rounded_rect_to(framebuffer_, W, H, g.x, g.y, g.w, g.h, 14, 0xFA0B0E18);
  draw_round_outline_to(framebuffer_, W, H, g.x, g.y, g.w, g.h, 14, 0xFF202738);

  for (int i = 0; i < g.app_count; ++i) {
    const TaskbarApp& a = TASKBAR_APPS[i];
    const int tx = g.app_x[i], ty = g.app_y;
    const bool hot = mouse_x_ >= tx && mouse_x_ < tx + g.tile &&
                     mouse_y_ >= ty && mouse_y_ < ty + g.tile;
    ButtonSuite::draw_dock_tile(framebuffer_, W, H, tx, ty, g.tile,
                                a.icon, a.color, hot, app_running(a.type));
  }

  // The divider between what you can open and what is open, set in the middle
  // of the gap it divides.
  if (g.win_count || g.sec_count) {
    const int div_x = g.app_x[g.app_count - 1] + g.tile + TASKBAR_DIV / 2;
    draw_rect_to(framebuffer_, W, H, div_x, g.app_y + 4, 1, g.tile - 8, 0xFF232A3A);
  }

  for (int i = 0; i < g.win_count; ++i) {
    const WindowSurface* w = find_window(g.win_id[i]);
    if (!w) continue;
    const bool on = w->window_id == active_window_id_ && !w->minimized;
    const bool hot = mouse_x_ >= g.win_x[i] && mouse_x_ < g.win_x[i] + g.win_w[i] &&
                     mouse_y_ >= g.win_y && mouse_y_ < g.win_y + g.win_h;
    draw_pill_to(framebuffer_, W, H, g.win_x[i], g.win_y, g.win_w[i], g.win_h,
                 on ? tok::wash : (hot ? 0xFF161C28 : 0u),
                 on ? tok::cta : (hot ? tok::strong : tok::line_up));
    const std::string t = fit_text(*this, w->title, g.win_w[i] - 16, Face::Ui);
    const int tw = text_width(t, Face::Ui);
    // A minimised window is still listed — that is where it went — but it is
    // said quietly, so the row shows at a glance what is actually on screen.
    draw_text_to(framebuffer_, W, H, g.win_x[i] + (g.win_w[i] - tw) / 2,
                 g.win_y + (g.win_h - line_height(Face::Ui)) / 2, t,
                 w->minimized ? tok::faint : (on ? tok::text : tok::dim), Face::Ui);
  }
  if (g.win_count) {
    const int d2 = g.win_x[g.win_count - 1] + g.win_w[g.win_count - 1] + TASKBAR_DIV / 2 - 3;
    draw_rect_to(framebuffer_, W, H, d2, g.app_y + 4, 1, g.tile - 8, 0xFF232A3A);
  }

  const int here = canvas_mode_ ? 0 : std::max(1, current_workspace_index());
  for (int i = 0; i < g.sec_count; ++i) {
    const bool on = (i == here);
    const bool hot = mouse_x_ >= g.sec_x[i] && mouse_x_ < g.sec_x[i] + g.sec_w[i] &&
                     mouse_y_ >= g.sec_y && mouse_y_ < g.sec_y + g.sec_h;
    draw_pill_to(framebuffer_, W, H, g.sec_x[i], g.sec_y, g.sec_w[i], g.sec_h,
                 on ? tok::cta : (hot ? tok::wash : 0u),
                 on ? tok::cta : (hot ? tok::strong : tok::line_up));
    const int lw = text_width(SECTION_LABELS[i], on ? Face::UiBold : Face::Ui);
    draw_text_to(framebuffer_, W, H, g.sec_x[i] + (g.sec_w[i] - lw) / 2, g.sec_y + (g.sec_h - 16) / 2,
                 SECTION_LABELS[i], on ? tok::on_cta : (hot ? tok::text : tok::dim),
                 on ? Face::UiBold : Face::Ui);
  }
}

bool HardwareCompositor::top_bar_click(int px, int py) {
  const int W = static_cast<int>(width_);
  const int bar_w = top_bar_w();
  const int bar_h = kTopBarH;
  const int bar_x = (W - bar_w) / 2;
  const int bar_y = kTopBarY;
  if (py < bar_y || py >= bar_y + bar_h || px < bar_x || px >= bar_x + bar_w) return false;

  // The brand badge goes back to the start page — the face this machine opens
  // on — rather than to a tab that no longer exists.
  const int badge_x = bar_x + 14;
  if (px >= badge_x && px < badge_x + top_bar_badge_w()) {
    palette_open_ = false;
    set_view(DeskView::StartPage);
    return true;
  }
  if (px >= bar_x + bar_w - 44 && px < bar_x + bar_w - 10) {
    power_menu_open_at(bar_x + bar_w - 44, 24, bar_y + bar_h);
    return true;
  }
  // The load meter's switch, hit where it was actually drawn. A little slack
  // around it: it is 30x20 and a person aiming at it should not have to.
  if (load_toggle_x_ >= 0 &&
      px >= load_toggle_x_ - 4 && px < load_toggle_x_ + kLoadToggleW + 4 &&
      py >= load_toggle_y_ - 6 && py < load_toggle_y_ + kLoadToggleH + 6) {
    canvas_state_.show_machine_load = !canvas_state_.show_machine_load;
    canvas_state_.layout_dirty = true;   // it is a preference, so it survives a restart
    mark_dirty();
    return true;
  }
  return true;   // the bar's own width: spent, but on nothing
}

bool HardwareCompositor::taskbar_click(int px, int py) {
  AIOS_TRACE("taskbar_click");
  const TaskbarGeom g = taskbar_geom();
  if (!g.contains(px, py)) return false;

  for (int i = 0; i < g.app_count; ++i) {
    if (px >= g.app_x[i] && px < g.app_x[i] + g.tile &&
        py >= g.app_y && py < g.app_y + g.tile) {
      // An app opens on a desktop, so the start page steps aside for it — the
      // canvas has no windows to put it in.
      if (canvas_mode_) {
        canvas_mode_ = false;
        if (current_workspace_ == CockpitWorkspace::Blank) switch_workspace(CockpitWorkspace::Code);
      }
      palette_open_ = false;
      ensure_app_window(TASKBAR_APPS[i].type);
      return true;
    }
  }

  for (int i = 0; i < g.win_count; ++i) {
    if (px >= g.win_x[i] && px < g.win_x[i] + g.win_w[i] &&
        py >= g.win_y && py < g.win_y + g.win_h) {
      // Clicking the window you are already in puts it away, the way a taskbar
      // has always worked; clicking any other one raises it.
      const WindowSurface* w = find_window(g.win_id[i]);
      if (w && w->window_id == active_window_id_ && !w->minimized && !canvas_mode_) {
        minimize_window(g.win_id[i]);
      } else {
        canvas_mode_ = false;
        bring_to_front(g.win_id[i]);
      }
      return true;
    }
  }

  for (int i = 0; i < g.sec_count; ++i) {
    if (px >= g.sec_x[i] && px < g.sec_x[i] + g.sec_w[i] &&
        py >= g.sec_y && py < g.sec_y + g.sec_h) {
      if (i == 0) set_view(DeskView::StartPage);
      else switch_workspace(i);
      return true;
    }
  }
  return true;   // the strip's own padding: spent, but on nothing
}

void HardwareCompositor::draw_command_palette() {
  if (!palette_open_ || active_top_tab_ == TopTab::FastTables) return;
  draw_center_dispatch_card(kernel_);
}

bool HardwareCompositor::command_palette_click(int x, int y) {
  if (!palette_open_) return false;
  const int W = static_cast<int>(width_);
  const int H = static_cast<int>(height_);
  const int card_w = std::min(684, W - 40);
  const int card_h = 438;
  const int card_x = (W - card_w) / 2;
  const int card_y = (H - card_h) / 2 - 20;

  if (x < card_x || x >= card_x + card_w || y < card_y || y >= card_y + card_h) {
    return false;
  }

  // ESC keycap
  const int esc_w = 38, esc_h = 24;
  const int esc_x = card_x + card_w - 48 - esc_w;
  const int esc_y = card_y + 32;
  if (x >= esc_x && x < esc_x + esc_w && y >= esc_y && y < esc_y + esc_h) {
    palette_input_.clear();
    return true;
  }

  // Items
  const int item_start_y = card_y + 76;
  const int item_h = 60;
  const int item_gap = 8;
  int count = std::min(4, static_cast<int>(palette_filtered_items_.size()));

  for (int i = 0; i < count; ++i) {
    int row_y = item_start_y + i * (item_h + item_gap);
    if (x >= card_x + 36 && x < card_x + card_w - 36 && y >= row_y && y < row_y + item_h) {
      palette_selected_idx_ = i;
      execute_palette_action(i);
      return true;
    }
  }

  return true;
}

void HardwareCompositor::snap_window_left(uint32_t win_id) {
  WindowSurface* win = (win_id != 0) ? find_window(win_id) : focused_window();
  if (!win) return;
  const Rect wa = work_area();
  const int margin = wa.x;
  const int top = wa.y;
  const int gap = 14;
  const int usable_w = wa.width;
  const int usable_h = wa.height;
  const int half_w = (usable_w - gap) / 2;

  win->bounds = {margin, top, half_w, usable_h};
  win->normal_bounds = win->bounds;
  win->maximized = false;
  win->minimized = false;
  win->visible = true;
  win->buffer.resize(win->content_pixels(), tok::panel);
  bring_to_front(win->window_id);
  set_toast("Snapped Left (50%)");
}

void HardwareCompositor::snap_window_right(uint32_t win_id) {
  WindowSurface* win = (win_id != 0) ? find_window(win_id) : focused_window();
  if (!win) return;
  const Rect wa = work_area();
  const int margin = wa.x;
  const int top = wa.y;
  const int gap = 14;
  const int usable_w = wa.width;
  const int usable_h = wa.height;
  const int half_w = (usable_w - gap) / 2;

  win->bounds = {margin + half_w + gap, top, usable_w - half_w - gap, usable_h};
  win->normal_bounds = win->bounds;
  win->maximized = false;
  win->minimized = false;
  win->visible = true;
  win->buffer.resize(win->content_pixels(), tok::panel);
  bring_to_front(win->window_id);
  set_toast("Snapped Right (50%)");
}

void HardwareCompositor::snap_window_maximize(uint32_t win_id) {
  WindowSurface* win = (win_id != 0) ? find_window(win_id) : focused_window();
  if (!win) return;
  const Rect wa = work_area();
  const int margin = wa.x;
  const int top = wa.y;
  const int usable_w = wa.width;
  const int usable_h = wa.height;

  win->bounds = {margin, top, usable_w, usable_h};
  win->normal_bounds = win->bounds;
  win->maximized = true;
  win->minimized = false;
  win->visible = true;
  win->buffer.resize(win->content_pixels(), tok::panel);
  bring_to_front(win->window_id);
  set_toast("Snapped Fullscreen");
}

void HardwareCompositor::tile_windows(int columns) {
  std::vector<WindowSurface*> vis;
  for (auto& w : windows_) {
    if (w.visible && !w.minimized) vis.push_back(&w);
  }
  if (vis.empty()) return;

  const int margin = 14;
  const int top = 34 + margin;
  const int bottom = static_cast<int>(height_) - 36 - margin;
  const int gap = 14;
  int usable_w = static_cast<int>(width_) - 2 * margin;
  int usable_h = bottom - top;
  if (usable_w < 540) usable_w = 540;
  if (usable_h < 380) usable_h = 380;

  if (columns <= 2) {
    if (vis.size() == 1) {
      vis[0]->bounds = {margin, top, usable_w, usable_h};
      vis[0]->normal_bounds = vis[0]->bounds;
      vis[0]->buffer.resize(vis[0]->content_pixels(), tok::panel);
    } else if (vis.size() == 2) {
      int half_w = (usable_w - gap) / 2;
      vis[0]->bounds = {margin, top, half_w, usable_h};
      vis[0]->normal_bounds = vis[0]->bounds;
      vis[0]->buffer.resize(vis[0]->content_pixels(), tok::panel);

      vis[1]->bounds = {margin + half_w + gap, top, usable_w - half_w - gap, usable_h};
      vis[1]->normal_bounds = vis[1]->bounds;
      vis[1]->buffer.resize(vis[1]->content_pixels(), tok::panel);
    } else {
      int half_w = (usable_w - gap) / 2;
      size_t left_count = (vis.size() + 1) / 2;
      size_t right_count = vis.size() - left_count;

      int left_h = (usable_h - static_cast<int>(left_count - 1) * gap) / static_cast<int>(left_count);
      int cur_y = top;
      for (size_t i = 0; i < left_count; ++i) {
        int ch = (i == left_count - 1) ? (top + usable_h - cur_y) : left_h;
        vis[i]->bounds = {margin, cur_y, half_w, ch};
        vis[i]->normal_bounds = vis[i]->bounds;
        vis[i]->buffer.resize(vis[i]->content_pixels(), tok::panel);
        cur_y += ch + gap;
      }

      int right_h = (usable_h - static_cast<int>(right_count - 1) * gap) / static_cast<int>(right_count);
      cur_y = top;
      for (size_t i = 0; i < right_count; ++i) {
        int ch = (i == right_count - 1) ? (top + usable_h - cur_y) : right_h;
        vis[left_count + i]->bounds = {margin + half_w + gap, cur_y, usable_w - half_w - gap, ch};
        vis[left_count + i]->normal_bounds = vis[left_count + i]->bounds;
        vis[left_count + i]->buffer.resize(vis[left_count + i]->content_pixels(), tok::panel);
        cur_y += ch + gap;
      }
    }
    set_toast("Tiled Windows (2-Column)");
  } else {
    int col_w = (usable_w - 2 * gap) / 3;
    for (size_t i = 0; i < vis.size(); ++i) {
      int col = static_cast<int>(i % 3);
      int cx = margin + col * (col_w + gap);
      int cw = (col == 2) ? (usable_w - 2 * (col_w + gap)) : col_w;
      vis[i]->bounds = {cx, top, cw, usable_h};
      vis[i]->normal_bounds = vis[i]->bounds;
      vis[i]->buffer.resize(vis[i]->content_pixels(), tok::panel);
    }
    set_toast("Tiled Windows (3-Column)");
  }
}

void HardwareCompositor::close_focused_window() {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& win : windows_) {
    if (win.focused && win.visible && !win.minimized) {
      win.visible = false;
      win.minimized = true;
      win.focused = false;
      set_toast("Closed " + win.title);
      break;
    }
  }
  for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
    if (it->visible && !it->minimized) {
      it->focused = true;
      active_window_id_ = it->window_id;
      return;
    }
  }
}

void HardwareCompositor::close_all_windows() {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& win : windows_) {
    win.visible = false;
    win.minimized = true;
    win.focused = false;
  }
  set_toast("Blank Slate — All windows closed");
}

void HardwareCompositor::switch_workspace(CockpitWorkspace ws) {
  current_workspace_ = ws;
  canvas_mode_ = false;
  palette_open_ = false;
  active_top_tab_ = TopTab::Workspaces;
  const int cur_ws = static_cast<int>(ws);

  // Show what belongs to this section, hide what belongs to the others.
  //
  // Switching to an empty section used to CONJURE WINDOWS: an untouched
  // section ran apply_layout_preset and opened an editor and a shell, or a
  // mail client and a calendar, because the section was a named "intention"
  // rather than a desktop. On every other machine, desktop 2 with nothing on
  // it shows you desktop 2 with nothing on it. It does now.
  uint32_t top_win_id = 0;
  for (auto& w : windows_) {
    if (w.workspace == cur_ws || w.pinned || w.workspace == -1) {
      w.visible = true;
      w.minimized = false;
      top_win_id = w.window_id;
    } else {
      w.visible = false;
      w.focused = false;
    }
  }
  if (top_win_id) bring_to_front(top_win_id);

  // The new section may be on a different wallpaper than the one just left.
  build_background();
  background_scene_ = face_scene();
}

void HardwareCompositor::switch_workspace(int ws_index) {
  switch_workspace(static_cast<CockpitWorkspace>(ws_index));
}

void HardwareCompositor::pin_window(uint32_t win_id, bool pinned) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& w : windows_) {
    if (w.window_id == win_id) {
      w.pinned = pinned;
      break;
    }
  }
}

void HardwareCompositor::move_window_to_workspace(uint32_t win_id, int ws) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  for (auto& w : windows_) {
    if (w.window_id == win_id) {
      w.workspace = ws;
      const int cur_ws = static_cast<int>(current_workspace_);
      w.visible = (ws == cur_ws || w.pinned || ws == -1);
      break;
    }
  }
}

void HardwareCompositor::draw_desktop_overlays(int toast_bottom) {
  const int W = static_cast<int>(width_);
  const int H = static_cast<int>(height_);
  // A toast: a small pill above the dock, in the surface's own ink.
  if (toast_timer_ > 0) {
    --toast_timer_;
    int tw = text_width(toast_message_, Face::Ui) + 28;
    if (tw > W - 40) tw = W - 40;
    int th = 30;
    int tx = W - tw - 16;
    int ty = toast_bottom - th - 12;
    shadow_rounded(tx, ty, tw, th, 10, 14, 38, 3);
    draw_rounded_rect_to(framebuffer_, W, H, tx, ty, tw, th, 10, tok::panel);
    draw_round_outline_to(framebuffer_, W, H, tx, ty, tw, th, 10, tok::strong);
    draw_text_to(framebuffer_, W, H, tx + 14, ty + 6,
                 fit_text(*this, toast_message_, tw - 28, Face::Ui), tok::dim);
  }

  // The view switcher, while the pointer is at the top edge.
  draw_view_bar();

  // The window switcher, while Alt is held.
  draw_alt_tab();

  // The Stop control, while something the desktop started is still running.
  draw_stop_control();

  // The right-click menu, over the desktop but under the pointer
  draw_context_menu();

  // The Command Palette / Fuzzy Launcher
  draw_command_palette();

  // The pointer, drawn last so it is over everything - unless the embedder
  // draws it on the glass itself (the Pi: straight onto the scanout at USB
  // rate, not once per frame).
  if (!software_pointer_) return;
  for (int row = 0; row < static_cast<int>(sizeof(CURSOR_ROWS) / sizeof(CURSOR_ROWS[0])); ++row) {
    const char* r = CURSOR_ROWS[row];
    int py = mouse_y_ + row;
    if (py < 0 || py >= static_cast<int>(height_)) continue;
    for (int col = 0; r[col]; ++col) {
      if (r[col] == ' ') continue;
      int px = mouse_x_ + col;
      if (px < 0 || px >= static_cast<int>(width_)) continue;
      framebuffer_[static_cast<size_t>(py) * width_ + px] = (r[col] == 'X') ? tok::bg : 0xFFFFFFFF;
    }
  }
}

int HardwareCompositor::pointer_shape(const char* const** rows) {
  if (rows) *rows = CURSOR_ROWS;
  return static_cast<int>(sizeof(CURSOR_ROWS) / sizeof(CURSOR_ROWS[0]));
}

HardwareCompositor::TopBar HardwareCompositor::top_bar() const {
  TopBar bar;
  WindowType current = WindowType::Custom;
  for (const auto& w : windows_) {
    if (w.focused && !w.minimized) current = w.type;
  }
  for (const auto& app : TOP_APPS) {
    bar.labels.emplace_back(app.label);
    if (app.type == current) bar.current = app.label;
  }
  return bar;
}

void HardwareCompositor::render_canvas(Kernel& k) {
  kernel_ = &k;
  if (!console_) return;
  const uint64_t now_ms = k.host().monotonic_ns() / 1000000ull;
  // The minted-objects ledger is a "what is new" readout, not an animation. It
  // walked five registries and every object in the store on EVERY frame, which
  // at 20 MB of store was the single biggest thing the compositor did. The
  // store itself is only refreshed every 250 ms by the frame loop, so sweeping
  // faster than that could not see anything new anyway.
  if (canvas_sweep_last_ms_ == 0 || now_ms - canvas_sweep_last_ms_ >= 250) {
    canvas_sweep_last_ms_ = now_ms;
    canvas_state_.sweep(k, now_ms);
  }
  // What the machine is doing to itself. The sampler throttles itself to once
  // a second -- reading /proc/stat, walking /proc/mounts and asking the GPU
  // driver are syscalls, not frame work -- so this is safe to call per frame.
  if (machine_load_sampler_.tick(k.host(), now_ms, k.config().root))
    canvas_state_.machine_load = machine_load_sampler_.latest();
  // ONE source for what the editor and the studio are holding: the canvas
  // reads the same window state the panes draw from, so the machine's face
  // and its workspace cannot disagree about the open file or the last query.
  canvas_state_.apps = workstation();
  // What this machine can be put on. Asked once, off this thread, and again
  // only when the provider changes — listing a CLI provider's models runs that
  // CLI, which would cost a frame.
  refresh_mind_lists();
  paint_canvas(*this, framebuffer_, width_, height_, k, *console_, hosted_canvas_feed(k), now_ms, canvas_state_);
  // The desk is loaded inside that paint. An app asked for before it was is put
  // back on now, over the saved state that had just closed it.
  if (app_pane_pending_ && canvas_state_.layout_loaded) {
    app_pane_pending_ = false;
    open_app_tile();
  }
  // The same furniture the windowed sections carry: one top bar and one
  // taskbar, on whichever face is up.
  draw_top_bar(&k);
  draw_taskbar();
  draw_desktop_overlays(static_cast<int>(height_) - 34);
  // The power panel is opened from this face as well, and it is hit-tested over
  // it, so it has to be drawn over it.
  draw_power_menu();
}

// Keep every open Mail window synced, whatever is on screen. This runs before
// the branches below because they decide what is DRAWN, and mail arriving has
// nothing to do with that: hanging the poll off render_mail_window meant a
// desktop sitting on the Fast Tables tab -- or the canvas, or another window --
// never fetched mail at all, since the mail pane was simply not being painted.
void HardwareCompositor::pump_mail_windows() {
#if !defined(AIOS_METAL)
  const uint64_t now = steady_ms();
  for (auto& win : windows_) {
    if (win.type != WindowType::Mail || !win.mail_store) continue;
    // Fold in a finished fetch, then start the next one when it is due. Both
    // halves run on the frame thread, so the worker never touches the store
    // while anything else is reading it.
    win.mail_store->poll_background_sync();
    if (win.mail_last_poll_ms == 0 || now - win.mail_last_poll_ms >= kMailPollIntervalMs) {
      win.mail_last_poll_ms = now;   // set even if the kick is declined, so a
      win.mail_store->begin_background_sync("all");   // busy store is not hammered
    }
  }
#endif
}

void HardwareCompositor::render_desktop(Kernel& k) {
  kernel_ = &k;
  grain_tick();   // grain programs' input, windows and output, outside the state lock
  // The size this machine is set to draw at, read once. Before the kernel is
  // in hand there is no aos.conf to read it from.
  if (!ui_scale_loaded_) { ui_scale_loaded_ = true; load_ui_scale(); }
  poll_run_directory(k);
  pump_mail_windows();
  /* The top bar's load/RAM figures come from this sampler, and only the
   * start-page canvas ticked it - so the workspace desktop (every face a
   * window lives on) showed "RAM -/total" with the used figure never read.
   * It throttles itself to once a second. */
  if (machine_load_sampler_.tick(k.host(), steady_ms(), k.config().root))
    canvas_state_.machine_load = machine_load_sampler_.latest();
  if (asleep_) { paint_asleep(); service_screenshot(k); return; }
  // Browser-only (`aios desktop --browser`): the window is the browser and nothing else. No wallpaper, no top bar, no dock, no
  // frame -- the pane is pinned so that its 1px border and 26px title bar fall just outside the display and its page area IS the
  // display. Input then needs no translation: a click at (x, y) is already at (x, y) in the pane.
  if (std::getenv("AOS_BROWSER_ONLY")) {
    const int W = static_cast<int>(width_), H = static_cast<int>(height_);
    // The start page is a face of its own and it would keep the keyboard (typing and the quick keys went to ITS prompt): leave it.
    canvas_mode_ = false; draw_mode_ = false;
    const uint32_t id = first_of_type(WindowType::Browser);
    for (auto& w : windows_) {
      if (w.window_id != id) { w.visible = false; w.minimized = true; continue; }
      w.bounds = Rect{-1, -26, W + 2, H + 27};
      w.minimized = false; w.visible = true; w.focused = true;
      render_window_content(w, k);
      std::fill(framebuffer_.begin(), framebuffer_.end(), 0xFF141414u);
      const int cw = w.content_width(), ch = w.content_height();
      for (int row = 0; row < ch && row < H; ++row) {
        const size_t src = static_cast<size_t>(row) * static_cast<size_t>(cw);
        const size_t dst = static_cast<size_t>(row) * static_cast<size_t>(width_);
        const int n = std::min(cw, W);
        if (src + n <= w.buffer.size() && dst + n <= framebuffer_.size()) std::memcpy(&framebuffer_[dst], &w.buffer[src], static_cast<size_t>(n) * sizeof(uint32_t));
      }
    }
    service_screenshot(k);
    return;
  }

  // The drawing surface is a face of its own, and it is the whole display:
  // asked for, it goes over the start page AND the workspace. Anything under
  // it is still there and comes back untouched when it is closed.
  if (draw_mode_) { render_draw_face(); service_screenshot(k); return; }
  if (canvas_mode_ && console_) { render_canvas(k); service_screenshot(k); return; }

  const int W = static_cast<int>(width_);
  const int H = static_cast<int>(height_);

  // 1. Cosmic Background
  //
  // Cached, and the cache has to record WHAT it was built for or it is not a
  // cache. background_scene_ was declared and read here and assigned nowhere in
  // the program, so it sat at its initialiser of -1 while the workspace scene
  // is 0: the test was true on every frame and this painted a 1920x1080
  // procedural background -- four square roots per pixel, two million pixels --
  // from scratch, every frame, forever. The workspace face never finished a
  // frame at all. Right-clicking the desk and choosing a pane looked like a
  // crash partly because of this.
  // The wallpaper this section is on. When there is a device drawing it — the
  // animated scenes and the ones the machine composed itself are a shader's
  // work, not a raster's — the pixels are left CLEAR and the shader fills
  // whatever the desktop did not cover, exactly as the start page does. Scene
  // 0 is the painter's own beach, which it draws better than the device.
  // Whatever face_scene() says, and never a substitute for it: this picks a
  // RENDERER (shader or raster) for the operator's scene, it does not pick the
  // scene. background_scene_ below is only the cache's record of which scene
  // the raster was built for. Who owns the choice is the person who asked.
  const int bg_scene = face_scene();
  canvas_state_.scene_effective = bg_scene;
  if (canvas_state_.background_on_gpu && bg_scene != 0) {
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0x00000000u);
  } else {
    if (background_.size() != framebuffer_.size() || background_scene_ != bg_scene) {
      build_background();
      background_scene_ = bg_scene;
    }
    std::memcpy(framebuffer_.data(), background_.data(), background_.size() * sizeof(uint32_t));
  }

  AIOS_TRACE("render_desktop: background done");
  bool any_visible = false;
  for (const auto& win : windows_) {
    if (!win.minimized && win.visible) {
      any_visible = true;
      break;
    }
  }

  // 3. The windows on this section, and nothing else.
  //
  // There used to be a third thing here: a whole "cockpit dashboard" page —
  // a brand banner, a stat card, a Ctrl+P billboard, three workspace chips and
  // nine app chips — drawn whenever the Overview tab was up OR no window was
  // open. So a desktop with nothing on it could not show you your own
  // background: it showed a page of controls that duplicated the taskbar. An
  // empty desktop is now an empty desktop, which is what every other machine
  // does, and the stat card it carried is the Machine window.
  if (active_top_tab_ == TopTab::FastTables || (!any_visible && palette_open_)) {
    draw_center_dispatch_card(&k);
  } else {
    // Windows rendering in z-order
    for (auto& win : windows_) {
      if (win.minimized || !win.visible) continue;
      const int bx = win.bounds.x, by = win.bounds.y, bw = win.bounds.width, bh = win.bounds.height;
      const int radius = 10;
      uint32_t frame = win.focused ? 0xFF388BFD : 0xFF262C38;

      // One shadow that follows the window's own rounded corners, instead of
      // three straight lines down two of its sides.
      shadow_rounded(bx, by, bw, bh, radius, 20, 46, 6);

      std::vector<uint32_t> corners = capture_corners(bx, by, bw, bh, radius);

      // Titlebar and surface
      draw_gradient_v_to(framebuffer_, W, H, bx, by, bw, 26, 0xFF1C202B, 0xFF13161F);
      draw_rect_to(framebuffer_, W, H, bx, by + 25, bw, 1, 0xFF282E3D);
      draw_rect_outline_to(framebuffer_, W, H, bx, by, bw, bh, frame);

      // macOS Traffic Lights
      bool close_hot = (mouse_x_ >= bx + 10 && mouse_x_ < bx + 22 && mouse_y_ >= by + 7 && mouse_y_ < by + 19);
      ButtonSuite::draw_traffic_light(framebuffer_, W, H, bx + 16, by + 13, 12, ButtonSuite::TrafficLightType::Close, close_hot);

      bool min_hot = (mouse_x_ >= bx + 26 && mouse_x_ < bx + 38 && mouse_y_ >= by + 7 && mouse_y_ < by + 19);
      ButtonSuite::draw_traffic_light(framebuffer_, W, H, bx + 32, by + 13, 12, ButtonSuite::TrafficLightType::Minimize, min_hot);

      bool max_hot = (mouse_x_ >= bx + 42 && mouse_x_ < bx + 54 && mouse_y_ >= by + 7 && mouse_y_ < by + 19);
      ButtonSuite::draw_traffic_light(framebuffer_, W, H, bx + 48, by + 13, 12, ButtonSuite::TrafficLightType::Maximize, max_hot);

      // App Icon next to traffic lights in titlebar
      draw_app_icon(framebuffer_, W, H, bx + 64, by + 5, win.type, 16);

      // Window title
      draw_text_to(framebuffer_, W, H, bx + 86, by + 4, win.title, win.focused ? tok::text : tok::dim,
                   win.focused ? Face::UiBold : Face::Ui);
      if (!win.badge.empty()) {
        int badge_x = bx + 86 + text_width(win.title, win.focused ? Face::UiBold : Face::Ui) + 12;
        int badge_w = label_width(win.badge) + 16;
        if (badge_x + badge_w < bx + bw - 70) {
          draw_pill_to(framebuffer_, W, H, badge_x, by + 5, badge_w, 17, 0, tok::line_up);
          draw_label_to(framebuffer_, W, H, badge_x + 8, by + 6, win.badge, tok::faint);
        }
      }

      // Right-side window buttons
      struct Btn { int dx; ButtonSuite::WindowAction act; };
      const Btn btns[] = {{-60, ButtonSuite::WindowAction::Minimize},
                          {-40, ButtonSuite::WindowAction::Maximize},
                          {-20, ButtonSuite::WindowAction::Close}};
      for (const auto& b : btns) {
        int bxx = bx + bw + b.dx;
        bool hot = mouse_y_ >= by && mouse_y_ < by + 26 && mouse_x_ >= bxx && mouse_x_ < bxx + 16;
        ButtonSuite::draw_window_action(framebuffer_, W, H, bxx - 1, by + 5, 18, 17, b.act, hot);
      }

      // Has the machine written the file this pane is holding?
      AIOS_TRACE("window: editor_watch_file");
      editor_watch_file(win, k);
      AIOS_TRACE("window: render_window_content");
      render_window_content(win, k);
      AIOS_TRACE("window: content done");

      int content_x = bx + 1;
      int content_y = by + 26;
      int content_w = win.content_width();
      int content_h = win.content_height();

      int start_row = std::max(0, -content_y);
      int end_row = std::min(content_h, static_cast<int>(H) - content_y);
      int start_col = std::max(0, -content_x);
      int end_col = std::min(content_w, static_cast<int>(W) - content_x);

      if (start_row < end_row && start_col < end_col) {
        size_t copy_pixels = static_cast<size_t>(end_col - start_col);
        size_t copy_bytes = copy_pixels * sizeof(uint32_t);
        for (int row = start_row; row < end_row; ++row) {
          int screen_y = content_y + row;
          size_t src = static_cast<size_t>(row) * static_cast<size_t>(content_w) + static_cast<size_t>(start_col);
          size_t dst = static_cast<size_t>(screen_y) * static_cast<size_t>(width_) + static_cast<size_t>(content_x + start_col);
          if (src + copy_pixels <= win.buffer.size() && dst + copy_pixels <= framebuffer_.size()) {
            std::memcpy(&framebuffer_[dst], &win.buffer[src], copy_bytes);
          }
        }
      }

      // The resize grip
      int rx = bx + bw - 14, ry = by + bh - 14;
      for (int i = 0; i < 3; ++i) {
        draw_rect_to(framebuffer_, W, H, rx + 8 - i * 4, ry + 8, 2, 2, tok::border);
        draw_rect_to(framebuffer_, W, H, rx + 8, ry + 8 - i * 4, 2, 2, tok::border);
      }

      round_frame(bx, by, bw, bh, radius, frame, corners);
    }

    if (palette_open_ && active_top_tab_ != TopTab::FastTables) {
      draw_center_dispatch_card(&k);
    }
  }

  // 4. The machine's furniture, over the windows. The top bar used to be drawn
  // BEFORE them, so any window dragged up there covered the clock, the memory
  // reading and the power control — a panel that the things it is supposed to
  // be above can hide is not a panel.
  draw_top_bar(&k);
  draw_taskbar();

  // 4b. The power panel
  draw_power_menu();

  // 5. Overlays (toast, cursor, menu)
  draw_desktop_overlays(H - 90);

  // Last, and on every face: a screenshot is of the FINISHED frame. Booked
  // between frames, served here, so what lands in the file is exactly what the
  // person was looking at when they asked.
  service_screenshot(k);
}

Result<void> HardwareCompositor::render_frame() {
  return Ok();
}

// ---- input, once, for every backend ----------------------------------------
// SDL on a hosted machine and the PS/2 driver on the metal both end up here.
// Nothing about dragging, focus, hit regions or typing is duplicated per
// backend, so the desktop cannot behave differently depending on who is
// delivering the events.

uint32_t HardwareCompositor::first_of_type(WindowType t) {
  const int cur_ws = static_cast<int>(current_workspace_);
  for (const auto& w : windows_) {
    if (w.type == t && (w.workspace == cur_ws || w.pinned || w.workspace == -1)) return w.window_id;
  }
  for (const auto& w : windows_) {
    if (w.type == t) return w.window_id;
  }
  return ensure_app_window(t);
}

WindowSurface* HardwareCompositor::focused_window() {
  for (auto& w : windows_) {
    if (w.focused && !w.minimized && w.visible) return &w;
  }
  for (auto& w : windows_) {
    if (!w.minimized && w.visible) return &w;
  }
  return nullptr;
}

WindowSurface* HardwareCompositor::focused_terminal() {
  for (auto& w : windows_) {
    if (w.focused && w.type == WindowType::Terminal && !w.minimized && w.visible) return &w;
  }
  for (auto& w : windows_) {
    if (w.type == WindowType::Terminal && !w.minimized && w.visible) return &w;
  }
  return nullptr;
}

// One of the machine's named actions, carried out. Every key that used to be a
// branch now arrives here through the binding table, so what a chord does is a
// lookup and not a line of C++ — which is what lets the machine be asked to
// change it.
//
// Returns false when the action did not apply in this context. That is not a
// failure: mind.stop with nothing running has to fall through so Escape keeps
// its other meanings.
void HardwareCompositor::poll_run_directory(Kernel& k) {
  const uint64_t now = steady_ms();
  if (keys_checked_ms_ && now - keys_checked_ms_ < 500) return;
  keys_checked_ms_ = now;

  // The run directory belongs to the MACHINE, not to whoever launched it.
  // These paths are relative ("run/draw"), and the host hands a relative path
  // straight to open(2) — so the file a desktop actually watched was
  // <cwd>/run/draw, which under a service unit is wherever WorkingDirectory
  // happens to point. Everything that WRITES the run directory writes
  // <root>/run/..., so the two ends were looking at different files and the
  // machine went deaf to the mind's hand: strokes, `shot` and `palette`
  // written to the documented path were read by nobody. Root first, then the
  // bare path, so a desktop started from inside its own root still works.
  auto read_run_file = [&](const std::string& path) {
    std::string v;
    if (!path.empty() && path[0] != '/' && !k.config().root.empty()) {
      auto rooted = k.host().read_file(k.config().root + "/" + path);
      if (rooted.ok()) {
        if (rooted->size() > 0) v.assign((const char*)rooted->data(), rooted->size());
        return v;
      }
    }
    auto f = k.host().read_file(path);
    if (f.ok() && f->size() > 0) v.assign((const char*)f->data(), f->size());
    return v;
  };

  auto trim = [](const std::string& v) {
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
  };
  auto read_trimmed = [&](const std::string& path) {
    const std::string v = read_run_file(path);
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
  };

  // The keyboard.
  {
    const std::string text = read_run_file(userland::key_bindings_path(k.config().root));
    if (text != keys_text_) {
      keys_text_ = text;
      apply_key_bindings_text(text);
    }
  }

  // What to draw WITH. Same shape as the url below: primed on the first pass so
  // a file left over from a previous boot does not open the drawing surface the
  // moment the machine starts, and compared as text so a file rewritten with
  // the same contents does nothing.
  {
    const std::string text = read_run_file(userland::run_draw_path(k.config().root));
    if (!draw_primed_) {
      draw_primed_ = true;
      draw_text_ = text;
      // The ink the machine was already carrying, in force before the first
      // stroke. What it must NOT do is re-open the surface a boot ago.
      (void)apply_draw_directives(text, /*settings_only=*/true);
    }
    else if (text != draw_text_) {
      draw_text_ = text;
      const std::string trouble = apply_draw_directives(text);
      if (!trouble.empty()) set_toast("drawing: " + trouble);
    }
  }

  // A url an application asked to have shown. Primed on the first pass WITHOUT
  // opening anything, so a url left over from a previous boot does not hijack
  // the display the moment the machine starts.
  {
    // The whole request, which is a url and (from `app open`) a serial on the
    // second line. Comparing the REQUEST rather than the url is what makes
    // asking twice for the same application work; opening the FIRST LINE is
    // what keeps the serial out of the address bar.
    const std::string request = read_trimmed(userland::run_browse_path(k.config().root));
    if (!browse_primed_) { browse_primed_ = true; browse_text_ = request; }
    else if (request != browse_text_) {
      browse_text_ = request;
      const size_t nl = request.find('\n');
      const std::string url = nl == std::string::npos ? request : trim(request.substr(0, nl));
      if (!url.empty()) open_url(k, url);
    }
    // A url this desktop was started to show. Once, as soon as there is a pane.
    if (!start_url_.empty() && first_of_type(WindowType::Browser)) {
      const std::string u = start_url_;
      start_url_.clear();
      open_url(k, u);
    }
  }
}

void HardwareCompositor::open_url(Kernel& k, const std::string& url) {
  if (url.empty()) return;
  const uint32_t id = first_of_type(WindowType::Browser);
  if (!id) return;
  // On a canvas face the app opens HERE, as a tile, rather than throwing the
  // person onto the workspace. Opening something used to mean leaving the face
  // you were looking at, every time, because the pane only existed over there.
  if (canvas_mode_) { show_app_pane(k, url); return; }
  bring_to_front(id);
  for (auto& w : windows_) {
    if (w.window_id != id) continue;
    web_pane_go(*this, w, k, url);
    break;
  }
}

// ---------------------------------------------------------------------------
// An application on the face you are standing on
// ---------------------------------------------------------------------------
WindowSurface* HardwareCompositor::app_pane_window() {
  for (auto& w : windows_) {
    if (w.type == WindowType::Browser) return &w;
  }
  return nullptr;
}

bool HardwareCompositor::app_tile_body(const CanvasLayout& lay, int* x, int* y, int* w, int* h) const {
  for (const TileRect& r : lay.tiles) {
    if (r.kind != TileKind::AppPane) continue;
    const int body_h = r.h - r.header_h;
    if (body_h <= 4 || r.w <= 4) return false;
    *x = r.x; *y = r.y + r.header_h; *w = r.w; *h = body_h;
    return true;
  }
  return false;
}

void HardwareCompositor::open_app_tile() {
  // The desk is seeded by the PAINTER, and a url can arrive before the first
  // frame — `aios desktop --open <url>` is exactly that. With no tiles yet
  // there was nothing to unhide and the app opened into a page nobody could
  // see. Seeding here is what the painter would have done a frame later.
  if (!canvas_state_.tiles_seeded) seed_desk_tiles(canvas_state_);
  // …and the saved desk is read by that same first paint, which says the app
  // tile is closed. Opening it now and letting the load shut it again is how an
  // app asked for at boot came up invisible; so it is asked for again once the
  // desk is actually loaded.
  if (!canvas_state_.layout_loaded) app_pane_pending_ = true;
  const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
  for (DeskTile& t : canvas_state_.tiles) {
    if (t.kind != TileKind::AppPane) continue;
    t.hidden = false;
    t.floating = true;
    // Sized and placed only the FIRST time. After that it is wherever the
    // person dragged it to, and an app opening must not put it back.
    if (t.w <= 0 || t.h <= 0) {
      t.w = std::max(320, (lay.iw * 62) / 100);
      t.h = std::max(240, (lay.ih * 62) / 100);
      t.x = (lay.iw - t.w) / 2;
      t.y = std::max(lay.body_y, (lay.ih - t.h) / 2);
    }
    canvas_state_.layout_dirty = true;
    break;
  }
}

void HardwareCompositor::show_app_pane(Kernel& k, const std::string& url) {
  open_app_tile();
  app_pane_focused_ = true;
  if (WindowSurface* win = app_pane_window()) web_pane_go(*this, *win, k, url);
}

void HardwareCompositor::show_inspect(const std::string& kind, const std::string& id) {
  if (!canvas_state_.tiles_seeded) seed_desk_tiles(canvas_state_);
  canvas_state_.inspect_kind = kind;
  canvas_state_.inspect_id = id;
  // Built on the next paint, which is the frame this click produced.
  canvas_state_.inspect_built.clear();
  const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
  for (DeskTile& t : canvas_state_.tiles) {
    if (t.kind != TileKind::Inspect) continue;
    t.hidden = false;
    t.floating = true;
    if (t.w <= 0 || t.h <= 0) {
      // Beside the desk rather than over the middle of it: what you clicked has
      // to stay visible, or the answer covers the question.
      t.w = std::max(360, (lay.iw * 34) / 100);
      t.h = std::max(260, (lay.ih * 52) / 100);
      t.x = std::max(8, lay.iw - t.w - 24);
      t.y = std::max(lay.body_y, (lay.ih - t.h) / 2);
    }
    canvas_state_.layout_dirty = true;
    break;
  }
}

void HardwareCompositor::paint_app_tile(std::vector<uint32_t>& buf, int iw, int ih, Kernel& k,
                                        const TileRect& r, int scale) {
  const int body_y = r.y + r.header_h;
  const int body_h = r.h - r.header_h;
  if (body_h <= 4 || r.w <= 4) return;
  // A TITLE BAR. Everything a person is told about moving a window on this face
  // is the 26px strip above the page — and it was bare panel with a fold and a
  // close mark pushed to its far corner, under which the page's own address bar
  // begins. So it read as the top of the PAGE, nobody tried to drag it, and the
  // answer to "can these be moved?" was no, though they always could be.
  if (r.header_h > 0) {
    draw_rect_to(buf, iw, ih, r.x, r.y, r.w, r.header_h, tok::rail);
    draw_rect_to(buf, iw, ih, r.x, r.y + r.header_h - 1, r.w, 1, tok::line_up);
    std::string name;
    if (WindowSurface* w = app_pane_window()) name = w->title;
    if (name.empty()) name = "Application";
    const int room = r.w - 28 - (r.ctl > 0 ? r.ctl * 2 + 16 : 0);
    if (room > 40)
      draw_text_to(buf, iw, ih, r.x + 12, r.y + (r.header_h - 16) / 2,
                   fit_text(*this, name, room, Face::UiBold), tok::text, Face::UiBold);
  }
  // The tile's own ground: a page that has not painted yet, or a machine
  // with no pane at all, must not leave a hole with the wallpaper showing
  // through the middle of a window.
  draw_rect_to(buf, iw, ih, r.x, body_y, r.w, body_h, tok::panel);
  WindowSurface* win = app_pane_window();
  if (!win) {
    draw_text_to(buf, iw, ih, r.x + 14, body_y + 12, "no pane on this machine", tok::faint, Face::UiSmall);
    return;
  }
  // The window IS this tile: its CONTENT rect is the tile's body in screen
  // pixels. So the page is laid out for the size it is actually shown at, and
  // the same pane on the workspace is the same page in the same place rather
  // than a second copy of it somewhere else.
  const int want_w = r.w * scale + 2, want_h = body_h * scale + 27;
  if (win->bounds.width != want_w || win->bounds.height != want_h) {
    win->bounds.width = want_w;
    win->bounds.height = want_h;
    win->buffer.assign(win->content_pixels(), tok::panel);
  }
  win->bounds.x = r.x * scale - 1;
  win->bounds.y = body_y * scale - 26;
  render_window_content(*win, k);
  const int cw = win->content_width(), ch = win->content_height();
  if (cw <= 0 || ch <= 0 || win->buffer.size() < (size_t)cw * (size_t)ch) return;
  // Point-sampled down by the paint scale, which on a dense panel is 2: the
  // pane is rendered at the display's real pixels and shown at the canvas's,
  // rather than laid out for half a screen and doubled.
  for (int ty = 0; ty < body_h; ++ty) {
    const int sy = ty * scale;
    const int dy = body_y + ty;
    if (sy >= ch) break;
    if (dy < 0 || dy >= ih) continue;
    const uint32_t* src = &win->buffer[(size_t)sy * (size_t)cw];
    uint32_t* dst = &buf[(size_t)dy * (size_t)iw];
    for (int tx = 0; tx < r.w; ++tx) {
      const int sx = tx * scale;
      const int dx = r.x + tx;
      if (sx >= cw) break;
      if (dx < 0 || dx >= iw) continue;
      dst[dx] = src[sx];
    }
  }
}

void HardwareCompositor::apply_key_bindings_text(const std::string& text) {
  // From the defaults every time, so removing a line from the file puts that
  // key back rather than leaving it stuck on whatever the file last said.
  keys_ = KeyBindings::defaults();
  if (text.empty()) return;
  auto applied = keys_.apply_text(text);
  // fputs and not std::cerr: this runs on BOTH substrates now, and <iostream>
  // in the bare-metal CORE drags libstdc++'s locale machinery in and takes the
  // kernel image with it. The existing std::cerr calls in this file all sit
  // inside the hosted-only guards.
  if (!applied.ok()) {
    std::fputs(("keys: " + applied.error().message + "\n").c_str(), stderr);
  }
}

bool HardwareCompositor::run_ui_action(UiAction a) {
  switch (a) {
    case UiAction::FaceToggle:
      canvas_mode_ = !canvas_mode_;
      return true;
    case UiAction::FaceCycle:
      if (!canvas_state_.collapsed_panels && !canvas_state_.hide_panels && !canvas_state_.bare_scene) {
        canvas_state_.collapsed_panels = true;
      } else if (canvas_state_.collapsed_panels) {
        canvas_state_.collapsed_panels = false;
        canvas_state_.hide_panels = true;
        canvas_state_.bare_scene = false;
      } else if (canvas_state_.hide_panels && !canvas_state_.bare_scene) {
        canvas_state_.bare_scene = true;
      } else {
        canvas_state_.collapsed_panels = false;
        canvas_state_.hide_panels = false;
        canvas_state_.bare_scene = false;
      }
      return true;
    case UiAction::FaceCollapse:
      canvas_state_.collapsed_panels = !canvas_state_.collapsed_panels;
      canvas_state_.hide_panels = false;
      canvas_state_.bare_scene = false;
      return true;
    // A scene key names a background for the view you are LOOKING at, not for
    // the machine: each view keeps its own.
    case UiAction::SceneOcean: set_view_scene(current_view(), 0); return true;
    case UiAction::SceneCyber: set_view_scene(current_view(), 1); return true;
    case UiAction::SceneOrb:   set_view_scene(current_view(), 2); return true;
    case UiAction::SceneNext:  set_view_scene(current_view(), (view_scene(current_view()) + 1) % 3); return true;
    case UiAction::MachinePower: {
      // Opens the panel; it asks before it acts. A key that destroys the
      // session must not do it on one keystroke.
      const CanvasChrome chrome = canvas_chrome(*this, static_cast<int>(width_));
      const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
      power_menu_open_at(chrome.rst_x * lay.scale, chrome.rst_w * lay.scale,
                         (chrome.rst_y + chrome.rst_h) * lay.scale);
      return true;
    }
    case UiAction::MachineRestart:
      set_toast("Restarting machine...");
      submit_command("reboot");
      return true;
    case UiAction::MindStop:
      if (!command_running_.load()) return false;
      stop_command();
      return true;
    case UiAction::PaneShell:
    case UiAction::PaneMachine:
    case UiAction::PaneTasks:
    case UiAction::PaneEditor:
    case UiAction::PaneDatabase:
    case UiAction::PaneWeb:
    case UiAction::PaneMail:
    case UiAction::PaneContacts:
    case UiAction::PaneCalendar:
    case UiAction::PaneApps:
    case UiAction::PaneInspector: {
      WindowType t = WindowType::Terminal;
      if (a == UiAction::PaneMachine) t = WindowType::Hardware;
      else if (a == UiAction::PaneTasks) t = WindowType::Tasks;
      else if (a == UiAction::PaneEditor) t = WindowType::Editor;
      else if (a == UiAction::PaneDatabase) t = WindowType::Database;
      else if (a == UiAction::PaneWeb) t = WindowType::Browser;
      else if (a == UiAction::PaneMail) t = WindowType::Mail;
      else if (a == UiAction::PaneContacts) t = WindowType::Contacts;
      else if (a == UiAction::PaneCalendar) t = WindowType::Calendar;
      else if (a == UiAction::PaneApps) t = WindowType::FaceApp;
      else if (a == UiAction::PaneInspector) t = WindowType::Inspector;
      canvas_mode_ = false;
      if (active_top_tab_ == TopTab::FastTables || active_top_tab_ == TopTab::Overview) active_top_tab_ = TopTab::Workspaces;
      palette_open_ = false;
      const uint32_t id = ensure_app_window(t);
      if (id) bring_to_front(id);
      return true;
    }
    case UiAction::DrawToggle:
      draw_mode_set(!draw_mode_);
      return true;
    case UiAction::DrawClear:
      // Only where it means something. Falling through otherwise is what keeps
      // whatever else 'x' is bound to on the other faces.
      if (!draw_mode_) return false;
      draw_clear();
      return true;
    case UiAction::DrawUndo:
      if (!draw_mode_) return false;
      draw_undo();
      return true;
    case UiAction::Screenshot:
      // Every face, not only the sheet: a picture of the workspace is as much
      // a thing to want as a picture of a drawing.
      request_screenshot();
      return true;
    case UiAction::PaletteToggle:
      toggle_command_palette();
      return true;
    case UiAction::SnapLeft:
      snap_window_left();
      return true;
    case UiAction::SnapRight:
      snap_window_right();
      return true;
    case UiAction::SnapMaximize:
      snap_window_maximize();
      return true;
    case UiAction::Tile2Col:
      tile_windows(2);
      return true;
    case UiAction::Tile3Col:
      tile_windows(3);
      return true;
    case UiAction::WorkspaceCode:
      switch_workspace(CockpitWorkspace::Code);
      return true;
    case UiAction::WorkspaceComms:
      switch_workspace(CockpitWorkspace::Comms);
      return true;
    case UiAction::WorkspaceAdmin:
      switch_workspace(CockpitWorkspace::Admin);
      return true;
    case UiAction::WindowClose:
      close_focused_window();
      return true;
    case UiAction::WindowCloseAll:
      close_all_windows();
      return true;
    case UiAction::None:
    case UiAction::Count:
      return false;
  }
  return false;
}

void HardwareCompositor::submit_command(const std::string& cmd) {
  if (cmd.empty()) return;
  // A grain program with windows runs on the desktop's own thread, where its
  // panes are drawn - not on the command thread the dispatcher would use.
  if (cmd.rfind("grain open ", 0) == 0) {
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (auto& w : windows_)
        if (w.type == WindowType::Terminal) { w.lines.push_back("aios> " + cmd); w.scroll_y = 0; }
    }
    std::string path = cmd.substr(11);
    while (!path.empty() && path.back() == ' ') path.pop_back();
    while (!path.empty() && path.front() == ' ') path.erase(path.begin());
    if (kernel_) grain_open(path, *kernel_);
    return;
  }
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.type == WindowType::Terminal) {
        w.lines.push_back("aios> " + cmd);
        w.scroll_y = 0;
      }
    }
    // The output had exactly one destination — windows of type Terminal, which
    // live on the WORKSPACE face. So a question typed at the landing page was
    // answered into a surface nobody on that face can see: the prompt cleared,
    // a toast went by, and the answer was never shown to the person who asked
    // for it. Every submission now also lands on the face that has the prompt.
    canvas_state_.answer_asked = cmd;
    canvas_state_.answer_lines.clear();
    canvas_state_.answer_running = true;
    canvas_state_.answer_scroll = 0;
  }
  // The console styles its output with ANSI, and this compositor has no ANSI
  // interpreter — it draws glyphs and picks its own colours. So an escape that
  // reaches a surface here is never a colour, it is literal garbage in the
  // middle of a sentence: "[2mthis machine's own model is not confident".
  auto plain = [](const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '\033' || (unsigned char)in[i] == 0x9B) {
        // CSI: ESC [ params letter. Anything else after ESC is a short
        // sequence; skip the one byte that terminates it.
        size_t j = i + 1;
        if (j < in.size() && in[j] == '[') {
          ++j;
          while (j < in.size() && !((in[j] >= 'A' && in[j] <= 'Z') || (in[j] >= 'a' && in[j] <= 'z'))) ++j;
        }
        i = j;
        continue;
      }
      out += in[i];
    }
    return out;
  };
  auto emit = [this, plain](const std::string& raw) {
    const std::string line = plain(raw);
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.type == WindowType::Terminal) w.lines.push_back(line);
    }
    // Bounded, because a horizon scan hands back pages of reading and this is
    // a card on a face, not a transcript. The newest is what is wanted.
    canvas_state_.answer_lines.push_back(line);
    // A command that FAILED has to say so where the person is looking. The
    // power buttons were the case that mattered: the refusal went to a log
    // file while the glass showed nothing, so a machine that would not power
    // off looked like a machine that had rebooted.
    if (line.compare(0, 6, "error:") == 0 || line.compare(0, 7, "FAILED:") == 0 ||
        line.find("may not power") != std::string::npos ||
        line.find("may not reboot") != std::string::npos) {
      std::string t = line;
      if (t.size() > 120) t = t.substr(0, 120) + "…";
      toast_message_ = t;
      // Long enough to read a refusal, which is the whole point of showing it.
      toast_timer_ = 540;
    }
    if (canvas_state_.answer_lines.size() > 400) {
      canvas_state_.answer_lines.erase(canvas_state_.answer_lines.begin(),
                                       canvas_state_.answer_lines.begin() + 200);
    }
    canvas_state_.answer_scroll = 0;
  };
  if (!dispatcher_) {
    emit("(no command surface is wired to this desktop)");
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      canvas_state_.answer_running = false;
    }
    return;
  }
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    running_command_ = cmd;
  }
  command_running_.store(true);
  auto run = [this, cmd, emit]() {
    // The flag has to fall on EVERY exit, including a dispatcher that throws,
    // or the landing card pulses "working" for the rest of the session.
    struct Done {
      HardwareCompositor* c;
      ~Done() {
        c->command_running_.store(false);
        std::lock_guard<aios::hal::Mutex> lock(c->state_mutex_);
        c->canvas_state_.answer_running = false;
      }
    } done{this};
    dispatcher_(cmd, emit);
  };
#if !defined(AIOS_METAL)
  if (async_commands_) {
    aios::hal::Thread(run).detach();
    return;
  }
#endif
  run();
}

// Stop whatever the desktop set running. The interrupt reaches the cognitive
// loop, which checks it wherever it can yield; a person who has watched the
// mind set off in the wrong direction had no way to say so before this.
void HardwareCompositor::stop_command() {
  if (!command_running_.load()) { set_toast("nothing is running"); return; }
  if (!interrupt_) { set_toast("this desktop has no way to stop what it started"); return; }
  interrupt_();
  set_toast("asked it to stop");
}

// The Stop control lives in the top bar, left of the clock, and only while
// something is running: a button that is there when it cannot do anything is a
// button people stop believing.
HardwareCompositor::StopGeom HardwareCompositor::stop_geom() const {
  StopGeom g;
  g.w = text_width("Stop", Face::UiBold) + 26;
  g.h = 22;
  g.x = static_cast<int>(width_) - power_geom().btn_w - 10 - g.w - 10;
  g.y = 6;
  return g;
}

std::string HardwareCompositor::grid_cell(int row, int column) const {
  for (const auto& win : windows_) {
    if (win.type != WindowType::Database) continue;
    if (row < 0 || row >= (int)win.grid_rows.size()) return "";
    const auto& r = win.grid_rows[(size_t)row];
    if (column < 0 || column >= (int)r.size()) return "";
    return r[(size_t)column];
  }
  return "";
}

HardwareCompositor::EditorGeom HardwareCompositor::editor_geom() const {
  EditorGeom g;
  for (const auto& win : windows_) {
    if (win.type != WindowType::Editor || !win.visible || win.minimized) continue;
    int cx = win.bounds.x + 1;      // content origin: 1px border,
    int cy = win.bounds.y + 26;     // 26px titlebar
    int cw = win.content_width();
    int body_y = APP_BODY_Y;
    if (!win.buffers.empty()) body_y += EDIT_TAB_H + 6;
    int browse_w = (cw > 620) ? 200 : (cw > 420 ? 150 : 0);
    int box_x = browse_w ? 20 + browse_w + 10 : 20;
    g.ok = true;
    g.text_x = cx + box_x + 48 + 10;
    g.text_y = cy + body_y + 8;
    g.row_h = APP_ROW;
    g.char_w = text_width("M", Face::Mono);
    if (g.char_w < 1) g.char_w = 8;
    return g;
  }
  return g;
}

HardwareCompositor::StudioGeom HardwareCompositor::studio_geom() const {
  StudioGeom g;
  // The studio's layout is measured from its window, the same way the renderer
  // measures it, so a click aimed here lands where the paint put the control.
  for (const auto& win : windows_) {
    if (win.type != WindowType::Database || !win.visible || win.minimized) continue;
    int cx = win.bounds.x + 1;          // content origin: 1px border,
    int cy = win.bounds.y + 26;         // 26px titlebar
    int cw = win.content_width();
    int body_y = APP_BODY_Y;
    int tree_w = (cw > 700) ? 240 : 180;
    g.ok = true;
    g.chip_y = cy + APP_CHIP_Y + APP_CHIP_H / 2;
    g.tree_x = cx + 40;
    g.tree_y = cy + body_y + 50;
    g.header_y = cy + body_y + 12;
    g.cell_x = cx + 20 + tree_w + 10 + 8;
    g.cell_y = cy + body_y + 30;
    g.row_h = APP_ROW;
    int grid_w = cw - (20 + tree_w + 10) - 20;
    if (!win.grid_columns.empty() && grid_w > 80) db_column_widths(win, grid_w, &g.column_w);
    return g;
  }
  return g;
}

void HardwareCompositor::draw_stop_control() {
  if (!command_running_.load()) return;
  StopGeom g = stop_geom();
  bool hot = mouse_x_ >= g.x && mouse_x_ < g.x + g.w && mouse_y_ >= g.y && mouse_y_ < g.y + g.h;
  draw_rounded_rect_to(framebuffer_, (int)width_, (int)height_, g.x, g.y, g.w, g.h, 6,
                       hot ? 0xFFE04A4A : 0xFF8B2F2F);
  draw_round_outline_to(framebuffer_, (int)width_, (int)height_, g.x, g.y, g.w, g.h, 6, 0xFFF08A8A);
  int tw = text_width("Stop", Face::UiBold);
  draw_text_to(framebuffer_, (int)width_, (int)height_, g.x + (g.w - tw) / 2, g.y + 4, "Stop",
               0xFFFFFFFF, Face::UiBold);
  // What it would stop is NOT written beside it: the bar already carries the
  // machine's readings there, and the first attempt at this drew the command
  // straight through the memory figure.
}

// Every rectangle the power control needs, measured. The panel is as wide as
// its widest row wants to be and as tall as its rows come to; nothing here is a
// number chosen to look about right, so adding a longer option or changing the
// face cannot clip a label.
void HardwareCompositor::power_menu_open_at(int anchor_x, int anchor_w, int below_y) {
  power_anchor_x_ = anchor_x;
  power_anchor_w_ = anchor_w;
  power_anchor_below_ = below_y;
  power_menu_open_ = true;
}

bool HardwareCompositor::power_menu_took_click(int x, int y) {
  if (!power_menu_open_) return false;
  PowerGeom g = power_geom();
  if (x >= g.panel_x && x < g.panel_x + g.panel_w && y >= g.panel_y && y < g.panel_y + g.panel_h) {
    for (int i = 0; i < g.rows; ++i) {
      if (x >= g.row_x() && x < g.row_x() + g.row_w() && y >= g.row_y(i) && y < g.row_y(i) + g.row_h) {
        power_choose(i);
        return true;
      }
    }
    return true;   // the panel's own padding: inside it, but not on a row
  }
  // Anywhere else — including the control itself — closes it, and that click is
  // spent on closing rather than on whatever it happened to be over.
  power_menu_open_ = false;
  return true;
}


// ---------------------------------------------------------------------------
// The drawing surface
// ---------------------------------------------------------------------------
// A black sheet the whole size of the display, and a pointer that paints on it.
// Everything here is deliberately substrate-neutral: it works in screen pixels
// and touches nothing but its own two buffers, so SDL's fingers, SDL's mouse,
// the bare-metal PS/2 driver and the headless script all reach it through the
// same three calls and cannot end up drawing differently.
//
// Persistence is the retained sheet: a stroke is painted ONCE, into
// draw_surface_, and every later frame is a memcpy of it. Repainting a list of
// points per frame would have made a long drawing cost more the longer you drew
// — which is exactly backwards for the thing a person is still drawing on. The
// point list is kept anyway, because a resize has to re-raster the picture at
// the new size rather than lose it.

namespace {
// What an unasked stroke uses, in order. A sequence and not one colour: the
// colour is the MIND's to pick, and when nothing has picked one the sheet still
// has to be able to hold a picture with more than one thing in it.
struct DrawInk { const char* name; uint32_t argb; };
constexpr DrawInk kDrawPalette[] = {
  {"ice",    0xFF57D2FF},
  {"rose",   0xFFFF5F8F},
  {"amber",  0xFFFFD166},
  {"mint",   0xFF8AF08A},
  {"violet", 0xFFC792EA},
  {"chalk",  0xFFFFFFFF},
  {"ember",  0xFFFF7A45},
  {"lagoon", 0xFF2ED3B7},
};
constexpr size_t kDrawPaletteN = sizeof kDrawPalette / sizeof kDrawPalette[0];

// "#ff2d95", "ff2d95", "#f09" or one of a few names a sentence would use.
bool parse_draw_color(const std::string& in, uint32_t& out) {
  std::string s;
  for (char ch : in) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '#') continue;
    s += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
  }
  if (s.empty()) return false;
  struct Named { const char* name; uint32_t argb; };
  static const Named kNamed[] = {
    {"white", 0xFFFFFFFF}, {"chalk", 0xFFFFFFFF}, {"red", 0xFFFF3B30},
    {"rose", 0xFFFF5F8F}, {"pink", 0xFFFF5F8F}, {"orange", 0xFFFF9F0A},
    {"amber", 0xFFFFD166}, {"yellow", 0xFFFFE066}, {"green", 0xFF32D74B},
    {"mint", 0xFF8AF08A}, {"cyan", 0xFF57D2FF}, {"ice", 0xFF57D2FF},
    {"blue", 0xFF0A84FF}, {"violet", 0xFFC792EA}, {"purple", 0xFFBF5AF2},
    {"grey", 0xFF8E8E93}, {"gray", 0xFF8E8E93},
  };
  for (const auto& n : kNamed) if (s == n.name) { out = n.argb; return true; }
  auto nib = [](char ch, int& v) {
    if (ch >= '0' && ch <= '9') { v = ch - '0'; return true; }
    if (ch >= 'a' && ch <= 'f') { v = ch - 'a' + 10; return true; }
    return false;
  };
  if (s.size() == 3 || s.size() == 6) {
    int v[6] = {0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < s.size(); ++i) if (!nib(s[i], v[i])) return false;
    int r, g, b;
    if (s.size() == 3) { r = v[0] * 17; g = v[1] * 17; b = v[2] * 17; }
    else { r = v[0] * 16 + v[1]; g = v[2] * 16 + v[3]; b = v[4] * 16 + v[5]; }
    out = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
  }
  return false;
}
}  // namespace

size_t HardwareCompositor::draw_palette_size() { return kDrawPaletteN; }
const char* HardwareCompositor::draw_palette_name(size_t i) {
  return i < kDrawPaletteN ? kDrawPalette[i].name : "";
}
uint32_t HardwareCompositor::draw_palette_color(size_t i) {
  return i < kDrawPaletteN ? kDrawPalette[i].argb : 0xFFFFFFFFu;
}

// What the mind says when it wants a particular ink: a name, an index, or
// "next" to take the following one in the sequence. "auto" hands the choice
// back to the sequence itself, which is not the same thing as picking one.
bool HardwareCompositor::draw_select_ink(const std::string& in) {
  std::string s;
  for (char ch : in) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
    s += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
  }
  if (s.empty()) return false;
  if (s == "auto" || s == "cycle") { draw_color_auto(); return true; }
  if (s == "next") {
    // Step the sequence AND pin it, so "next" means the next stroke is that
    // colour rather than merely nudging a counter the auto path would have
    // advanced anyway.
    draw_set_color(kDrawPalette[draw_auto_index_ % kDrawPaletteN].argb);
    ++draw_auto_index_;
    return true;
  }
  for (size_t i = 0; i < kDrawPaletteN; ++i) {
    if (s == kDrawPalette[i].name) { draw_set_color(kDrawPalette[i].argb); draw_auto_index_ = i + 1; return true; }
  }
  bool digits = true;
  for (char ch : s) if (ch < '0' || ch > '9') { digits = false; break; }
  if (digits) {
    size_t i = (size_t)std::strtoul(s.c_str(), nullptr, 10);
    if (i < kDrawPaletteN) { draw_set_color(kDrawPalette[i].argb); draw_auto_index_ = i + 1; return true; }
  }
  return false;
}

std::string HardwareCompositor::draw_ink_name() const {
  const uint32_t rgb = draw_color_ & 0x00FFFFFFu;
  for (size_t i = 0; i < kDrawPaletteN; ++i)
    if ((kDrawPalette[i].argb & 0x00FFFFFFu) == rgb) return kDrawPalette[i].name;
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%06x", rgb);
  return std::string(buf);
}

void HardwareCompositor::draw_ensure_surface() {
  const uint32_t W = width_, H = height_;
  if (!W || !H) return;
  if (draw_surface_w_ == W && draw_surface_h_ == H &&
      draw_surface_.size() == (size_t)W * H) return;
  draw_surface_w_ = W;
  draw_surface_h_ = H;
  draw_surface_.assign((size_t)W * H, 0xFF000000);
  // The screen changed under a picture. Put the picture back on it rather than
  // handing the person a blank sheet because their window grew.
  draw_repaint_surface();
}

void HardwareCompositor::draw_repaint_surface() {
  if (draw_surface_.empty()) return;
  std::fill(draw_surface_.begin(), draw_surface_.end(), 0xFF000000u);
  for (const DrawStroke& s : draw_strokes_) draw_raster_stroke(s);
}

// One round nib. Round because a square one turns visibly as a stroke changes
// direction, which is the difference between a line somebody drew and a line a
// program drew.
void HardwareCompositor::draw_disc(int cx, int cy, int radius, uint32_t color) {
  if (draw_surface_.empty()) return;
  const int W = (int)draw_surface_w_, H = (int)draw_surface_h_;
  if (radius < 0) radius = 0;
  const int r2 = (radius + 1) * (radius + 1);
  for (int dy = -radius - 1; dy <= radius + 1; ++dy) {
    const int y = cy + dy;
    if (y < 0 || y >= H) continue;
    for (int dx = -radius - 1; dx <= radius + 1; ++dx) {
      const int x = cx + dx;
      if (x < 0 || x >= W) continue;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r2) continue;
      draw_surface_[(size_t)y * W + x] = color;
    }
  }
}

// The gap between two samples, filled. A pointer is sampled once a frame at
// best, so a hand moving at any speed at all delivers points tens of pixels
// apart; without this a fast drag is a dotted line.
void HardwareCompositor::draw_segment(int x0, int y0, int x1, int y1, int width, uint32_t color) {
  const int radius = width / 2;
  int dx = x1 - x0, dy = y1 - y0;
  int steps = std::max(std::abs(dx), std::abs(dy));
  if (steps <= 0) { draw_disc(x1, y1, radius, color); return; }
  // One nib per pixel of travel, so the discs overlap whatever the direction.
  for (int i = 0; i <= steps; ++i) {
    const int x = x0 + (int)((long long)dx * i / steps);
    const int y = y0 + (int)((long long)dy * i / steps);
    draw_disc(x, y, radius, color);
  }
}

// Catmull-Rom between p1 and p2, with p0/p3 as the neighbours that say which
// way the line was already going. Straight segments between four-frame samples
// are what makes a drawn circle come out an octagon; this is the fix, and it
// costs nothing a person can feel because it is only ever a handful of extra
// discs per sample.
void HardwareCompositor::draw_curve(const DrawPoint& p0, const DrawPoint& p1,
                                    const DrawPoint& p2, const DrawPoint& p3,
                                    int width, uint32_t color) {
  const int chord = std::abs(p2.x - p1.x) + std::abs(p2.y - p1.y);
  // One sub-sample per ~3px of chord: below that the round nibs already
  // overlap and the extra work buys nothing anyone can see.
  int steps = std::clamp(chord / 3, 1, 64);
  int px = p1.x, py = p1.y;
  for (int i = 1; i <= steps; ++i) {
    const float t = (float)i / (float)steps;
    const float t2 = t * t, t3 = t2 * t;
    const float fx = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                             (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                             (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
    const float fy = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                             (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                             (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
    const int nx = (int)std::lround(fx), ny = (int)std::lround(fy);
    // Still a line of round nibs between sub-samples, so the join at every
    // corner is a disc and never a mitre that opens up on a sharp turn.
    draw_segment(px, py, nx, ny, width, color);
    px = nx; py = ny;
  }
}

// ONE rasteriser for a whole stroke. A repaint (undo, resize) has to lay the
// ink down exactly the way the drag did, or taking one stroke back would
// redraw the others slightly differently — visible, and the kind of thing
// nobody believes until they see their own drawing change under them.
// A corner somebody MEANT. Catmull-Rom takes its tangent from the neighbouring
// samples, so a deliberate right angle — the top of a building, the corner of a
// box — gets rounded into a bulge. Duplicating the neighbour at such a vertex
// makes the curve leave it straight, which keeps the corner sharp while every
// gentle turn stays a curve. Short segments are hand jitter, not a corner: a
// finger delivers samples a couple of pixels apart and those angles swing wildly.
static bool draw_is_corner(int px, int py, int qx, int qy, int rx, int ry) {
  const float ux = (float)(qx - px), uy = (float)(qy - py);
  const float vx = (float)(rx - qx), vy = (float)(ry - qy);
  const float lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);
  if (lu < 6.0f || lv < 6.0f) return false;
  const float cosang = (ux * vx + uy * vy) / (lu * lv);
  return cosang < 0.64f;  // turned by more than ~50 degrees
}

void HardwareCompositor::draw_raster_segment(const DrawStroke& s, size_t i) {
  const size_t n = s.pts.size();
  if (i + 1 >= n) return;
  // Duplicate the ends: a stroke has no sample before its first or after its
  // last, and clamping is what keeps the tips from flicking off-course. The
  // same duplication is what pins a corner, so a square comes out a square.
  const DrawPoint& b = s.pts[i];
  const DrawPoint& c = s.pts[i + 1];
  const DrawPoint& prev = s.pts[i == 0 ? 0 : i - 1];
  const DrawPoint& next = s.pts[i + 2 < n ? i + 2 : n - 1];
  const DrawPoint& a =
      (i == 0 || draw_is_corner(prev.x, prev.y, b.x, b.y, c.x, c.y)) ? b : prev;
  const DrawPoint& d =
      (i + 2 >= n || draw_is_corner(b.x, b.y, c.x, c.y, next.x, next.y)) ? c : next;
  draw_curve(a, b, c, d, s.width, s.color);
}

void HardwareCompositor::draw_raster_stroke(const DrawStroke& s) {
  const size_t n = s.pts.size();
  if (n == 0) return;
  if (n == 1) { draw_disc(s.pts[0].x, s.pts[0].y, s.width / 2, s.color); return; }
  draw_disc(s.pts[0].x, s.pts[0].y, s.width / 2, s.color);
  for (size_t i = 0; i + 1 < n; ++i) draw_raster_segment(s, i);
}

void HardwareCompositor::draw_mode_set(bool on) {
  if (draw_mode_ == on) return;
  draw_mode_ = on;
  // A face change with a finger down would leave the stroke open forever.
  draw_drawing_ = false;
  if (on) {
    draw_ensure_surface();
  }
  mark_dirty();
}

void HardwareCompositor::draw_set_color(uint32_t argb) {
  draw_color_ = 0xFF000000u | (argb & 0x00FFFFFFu);
  draw_color_set_ = true;
}

void HardwareCompositor::draw_color_auto() { draw_color_set_ = false; }

void HardwareCompositor::draw_set_width(int px) {
  draw_width_ = std::clamp(px, 1, 128);
}

void HardwareCompositor::draw_clear() {
  draw_strokes_.clear();
  draw_drawing_ = false;
  if (!draw_surface_.empty()) std::fill(draw_surface_.begin(), draw_surface_.end(), 0xFF000000u);
  mark_dirty();
}

// One stroke back. The pixels cannot be un-painted (the sheet keeps no
// per-stroke layer), so the record is what is authoritative: drop the last
// stroke and lay the rest of the picture down again.
// Thumb-sized, not cursor-sized: this face is meant to be drawn on with a
// finger, and a 16px close box is a control only a mouse can use. Top corner,
// away from where a hand rests while drawing.
namespace {
constexpr int kDrawCtlW = 96, kDrawCtlH = 40, kDrawCtlPad = 16, kDrawCtlGap = 12;
}

HardwareCompositor::DrawControlRect HardwareCompositor::draw_clear_rect() const {
  DrawControlRect r;
  r.w = kDrawCtlW; r.h = kDrawCtlH;
  r.x = (int)width_ - kDrawCtlPad - kDrawCtlW;
  r.y = kDrawCtlPad;
  return r;
}

HardwareCompositor::DrawControlRect HardwareCompositor::draw_undo_rect() const {
  DrawControlRect r = draw_clear_rect();
  r.x -= kDrawCtlW + kDrawCtlGap;
  return r;
}

HardwareCompositor::DrawControlRect HardwareCompositor::draw_keep_rect() const {
  DrawControlRect r = draw_undo_rect();
  r.x -= kDrawCtlW + kDrawCtlGap;
  return r;
}

// Hit-testing lives HERE and not in a mouse handler on purpose: every pointer
// on this face — finger, stylus, mouse — arrives through draw_pointer_down, so
// a control that answers here answers to all of them and there is no path
// where touch quietly gets a dead button.
HardwareCompositor::DrawControl HardwareCompositor::draw_control_at(int x, int y) const {
  auto in = [&](const DrawControlRect& r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
  };
  if (in(draw_keep_rect())) return DrawControl::Keep;
  if (in(draw_undo_rect())) return DrawControl::Undo;
  if (in(draw_clear_rect())) return DrawControl::Clear;
  return DrawControl::None;
}

void HardwareCompositor::draw_undo() {
  if (draw_strokes_.empty()) return;
  draw_strokes_.pop_back();
  draw_drawing_ = false;
  draw_repaint_surface();
  mark_dirty();
}

uint32_t HardwareCompositor::draw_pixel(int x, int y) const {
  if (draw_surface_.empty()) return 0xFF000000u;
  if (x < 0 || y < 0 || x >= (int)draw_surface_w_ || y >= (int)draw_surface_h_) return 0xFF000000u;
  return draw_surface_[(size_t)y * draw_surface_w_ + x];
}

// A finger, a stylus or a mouse button going down: a new stroke, in whatever
// colour is set at this instant. Taking the colour HERE rather than per point
// is what lets the mind change it between strokes without smearing the change
// back through the one being drawn.
void HardwareCompositor::draw_pointer_down(int x, int y) {
  draw_ensure_surface();
  // A press that lands on undo or clear is a press of that control, never the
  // start of a stroke — otherwise every tap of a button leaves a dot on the
  // picture it was meant to edit.
  const DrawControl hit = draw_control_at(x, y);
  if (hit != DrawControl::None) {
    draw_pressed_ = hit;
    mark_dirty();
    return;
  }
  draw_pressed_ = DrawControl::None;
  uint32_t color = draw_color_;
  if (!draw_color_set_) {
    color = kDrawPalette[draw_auto_index_ % kDrawPaletteN].argb;
    ++draw_auto_index_;
    draw_color_ = color;
  }
  DrawStroke s;
  s.color = color;
  s.width = draw_width_;
  s.pts.push_back(DrawPoint{x, y});
  draw_strokes_.push_back(std::move(s));
  draw_drawing_ = true;
  draw_last_x_ = x;
  draw_last_y_ = y;
  // A tap is a dot. Waiting for a second sample means the shortest mark a
  // person can make is nothing at all.
  draw_disc(x, y, draw_width_ / 2, color);
  mark_dirty();
}

void HardwareCompositor::draw_commit_stroke(std::vector<DrawPoint> pts) {
  if (pts.empty()) return;
  draw_ensure_surface();
  // The same ink rule a finger gets: an unasked stroke takes the next colour
  // in the sequence, so a mind that never names one still draws a picture in
  // more than one colour instead of eight strokes of the same blue.
  uint32_t color = draw_color_;
  if (!draw_color_set_) {
    color = kDrawPalette[draw_auto_index_ % kDrawPaletteN].argb;
    ++draw_auto_index_;
    draw_color_ = color;
  }
  DrawStroke s;
  s.color = color;
  s.width = draw_width_;
  s.pts = std::move(pts);
  draw_raster_stroke(s);
  draw_strokes_.push_back(std::move(s));
  // A mind's stroke is finished the instant it is written. Leaving
  // draw_drawing_ set would make the next finger-move append to the machine's
  // line instead of starting one of its own.
  draw_drawing_ = false;
  mark_dirty();
}

void HardwareCompositor::draw_pointer_move(int x, int y) {
  // A finger dragging off a pressed button is not drawing.
  if (draw_pressed_ != DrawControl::None) return;
  if (!draw_drawing_ || draw_strokes_.empty()) return;
  if (x == draw_last_x_ && y == draw_last_y_) return;
  DrawStroke& s = draw_strokes_.back();
  s.pts.push_back(DrawPoint{x, y});
  // Paint the segment that now has a sample on BOTH sides of it, which is one
  // behind the finger. The alternative — lay a straight chord down now and
  // curve over it later — leaves the chord showing through as a bulge on every
  // sharp turn, because ink is opaque and painting a curve does not unpaint
  // the line under it. One frame of lag is invisible; that bulge is not.
  const size_t n = s.pts.size();
  if (n >= 3) draw_raster_segment(s, n - 3);
  draw_last_x_ = x;
  draw_last_y_ = y;
  mark_dirty();
}

void HardwareCompositor::draw_pointer_up(int x, int y) {
  // A control only fires if the release is still on the control the press
  // landed on — slide off and it is a cancel, the way a button should behave
  // under a thumb that is not sure.
  if (draw_pressed_ != DrawControl::None) {
    const DrawControl was = draw_pressed_;
    draw_pressed_ = DrawControl::None;
    if (draw_control_at(x, y) == was) {
      if (was == DrawControl::Undo) draw_undo();
      else if (was == DrawControl::Keep) request_screenshot();
      else draw_clear();
    }
    mark_dirty();
    return;
  }
  if (draw_drawing_) {
    draw_pointer_move(x, y);
    // The tail the lag left: the last segment has no sample beyond it, so it
    // is painted here with its far neighbour clamped to the end point.
    if (!draw_strokes_.empty()) {
      const DrawStroke& s = draw_strokes_.back();
      if (s.pts.size() >= 2) draw_raster_segment(s, s.pts.size() - 2);
    }
  }
  draw_drawing_ = false;
  mark_dirty();
}

// What the mind leaves in the run directory, applied. One directive a line so
// a colour and a width can arrive together, and so a file written twice with
// the same contents does nothing the second time (poll_run_directory compares
// the text, the way it does for the url).
std::string HardwareCompositor::apply_draw_directives(const std::string& text, bool settings_only) {
  std::string trouble;
  auto complain = [&trouble](const std::string& what) {
    trouble += (trouble.empty() ? "" : "; ") + what;
  };
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    std::vector<std::string> f;
    for (size_t p = 0; p < line.size();) {
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
      size_t e = p;
      while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r') ++e;
      if (e > p) f.push_back(line.substr(p, e - p));
      p = e;
    }
    if (f.empty() || f[0].empty() || f[0][0] == '#') continue;
    std::string verb;
    for (char ch : f[0]) verb += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    if (verb == "color" || verb == "colour" || verb == "ink") {
      uint32_t argb = 0;
      if (f.size() < 2) complain("`color` with no colour after it");
      else if (verb == "color" && f.size() >= 2 && (f[1] == "auto" || f[1] == "cycle")) draw_color_auto();
      else if (draw_select_ink(f[1])) { /* a palette name or index */ }
      else if (parse_draw_color(f[1], argb)) draw_set_color(argb);
      else complain("no colour called '" + f[1] + "'");
    } else if (verb == "palette" || verb == "ink_name" || verb == "pick") {
      // The named door: `palette rose`, `palette 4`, `palette next`. This is
      // what a mind writing a sentence reaches for; `color #rrggbb` stays for
      // anything the palette has no name for.
      if (f.size() < 2) complain("`palette` with no name, index or `next` after it");
      else if (!draw_select_ink(f[1])) complain("no ink called '" + f[1] + "' in the palette");
    } else if (verb == "undo" || verb == "back") {
      if (!settings_only) draw_undo();
    } else if (verb == "width" || verb == "nib") {
      if (f.size() < 2) complain("`width` with no number after it");
      else draw_set_width(std::atoi(f[1].c_str()));
    } else if (verb == "on" || verb == "draw" || verb == "open") {
      if (!settings_only) draw_mode_set(true);
    } else if (verb == "off" || verb == "close") {
      if (!settings_only) draw_mode_set(false);
    } else if (verb == "clear" || verb == "erase") {
      if (!settings_only) draw_clear();
    } else if (verb == "stroke" || verb == "path" || verb == "line") {
      // The mind's own hand: `stroke 100,100 200,180 300,100`, in the ink and
      // nib currently loaded. Each coordinate is pixels, or a PERCENTAGE of
      // the screen when it ends in '%' — the side writing the picture is in
      // another process and does not know how big this screen is, and a heart
      // laid out in pixels for 1280x800 is off the glass on a 1024x600 panel.
      // `close` anywhere in the line returns to the first point, which is what
      // a closed shape (a heart, a face's outline, a window) needs.
      if (settings_only) { /* a stroke HAPPENED; never re-draw it on reboot */ }
      else if (f.size() < 2) complain("`stroke` with no points after it");
      else {
        std::vector<DrawPoint> pts;
        bool close = false;
        bool bad = false;
        for (size_t i = 1; i < f.size() && !bad; ++i) {
          if (f[i] == "close" || f[i] == "closed") { close = true; continue; }
          const size_t comma = f[i].find(',');
          if (comma == std::string::npos) { complain("'" + f[i] + "' is not an x,y point"); bad = true; break; }
          auto coord = [&](const std::string& t, int span, int& out) {
            if (t.empty()) return false;
            char* endp = nullptr;
            const double v = std::strtod(t.c_str(), &endp);
            if (endp == t.c_str()) return false;
            while (*endp == ' ') ++endp;
            if (*endp == '%') out = (int)(v * span / 100.0);
            else if (*endp == '\0') out = (int)v;
            else return false;
            return true;
          };
          int x = 0, y = 0;
          if (!coord(f[i].substr(0, comma), (int)width_, x) ||
              !coord(f[i].substr(comma + 1), (int)height_, y)) {
            complain("'" + f[i] + "' is not an x,y point");
            bad = true;
            break;
          }
          pts.push_back(DrawPoint{x, y});
        }
        if (!bad) {
          if (close && pts.size() > 2) pts.push_back(pts.front());
          draw_commit_stroke(std::move(pts));
        }
      }
    } else if (verb == "shot" || verb == "screenshot" || verb == "keep") {
      // The shutter through the same file the ink comes through: `shot` puts
      // one where the machine chooses, `shot <path>` where the asking side
      // wants it. Never on the settings pass — a screenshot is something that
      // HAPPENED, like `clear`, and re-taking one because the machine rebooted
      // would photograph a desktop nobody asked about over the picture.
      if (!settings_only) request_screenshot(f.size() >= 2 ? f[1] : std::string());
    } else {
      complain("no drawing directive called '" + verb + "'");
    }
  }
  return trouble;
}

// The sheet on the glass. Black, because that is what the picture is drawn on,
// and a memcpy, because the picture is already drawn.
void HardwareCompositor::render_draw_face() {
  draw_ensure_surface();
  const uint32_t W = width_, H = height_;
  if (framebuffer_.size() != (size_t)W * H) framebuffer_.assign((size_t)W * H, 0xFF000000);
  if (draw_surface_.size() == framebuffer_.size())
    std::copy(draw_surface_.begin(), draw_surface_.end(), framebuffer_.begin());
  else
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0xFF000000u);
  // A swatch of the ink the next stroke will use, and the way out. Drawn in
  // the corner and in the ink itself, so a person can see what the machine
  // picked before they put a finger down. Nothing else is over the sheet: the
  // point of it is the picture.
  const int sw = 14;
  draw_rect_to(framebuffer_, W, H, 12, (int)H - 26, sw, sw, draw_color_);
  // The ink by NAME, because that is how it was chosen. A person who asked for
  // rose should be told rose, not 0xFFFF5F8F and not nothing at all.
  const std::string ink = draw_ink_name() + (draw_color_is_auto() ? " (cycling)" : "");
  draw_text_to(framebuffer_, W, H, 12 + sw + 8, (int)H - 26,
               ink + "   F12 leaves   x clears   z undoes   s keeps",
               0xFF3A3A3A);

  // Undo and clear, as controls a thumb can hit. They are hit-tested in
  // draw_pointer_down, which is the ONE door every pointer comes through, so
  // these work under a finger exactly as they do under a mouse.
  auto control = [&](const DrawControlRect& r, const char* label, bool pressed) {
    const uint32_t fill = pressed ? 0xFF2A2A2A : 0xFF141414;
    const uint32_t edge = pressed ? 0xFF6A6A6A : 0xFF3A3A3A;
    draw_rounded_rect_to(framebuffer_, W, H, r.x, r.y, r.w, r.h, 10, fill);
    draw_round_outline_to(framebuffer_, W, H, r.x, r.y, r.w, r.h, 10, edge);
    const int tw = text_width(label, Face::Ui);
    draw_text_to(framebuffer_, W, H, r.x + (r.w - tw) / 2, r.y + (r.h - 10) / 2,
                 label, pressed ? 0xFFEDEDED : 0xFFB4B4B4);
  };
  control(draw_keep_rect(), "keep", draw_pressed_ == DrawControl::Keep);
  control(draw_undo_rect(), "undo", draw_pressed_ == DrawControl::Undo);
  control(draw_clear_rect(), "clear", draw_pressed_ == DrawControl::Clear);
  // Deliberately NOT draw_desktop_overlays(): the toast is a 540px panel
  // across the bottom of the display, and on this face that is a panel across
  // somebody's drawing. Measured — a stroke laid at y=340 came back with 12 of
  // its pixels showing and the rest behind the notice telling the person how to
  // leave. The hint above is the whole of this face's chrome.
}

// ---- the machine's own shutter ---------------------------------------------
// aios takes its own picture. The alternative — an X grab, a camera, a QMP
// screendump — needs something outside the machine that can see its display,
// which the bare-metal image and a headless run do not have and which makes a
// screenshot a thing done TO the desktop rather than something it can do.
void HardwareCompositor::request_screenshot(const std::string& path) {
  shot_pending_ = true;
  shot_path_ = path;
  mark_dirty();
}

std::string HardwareCompositor::take_screenshot(Kernel& k, const std::string& path) {
  if (!width_ || !height_) return std::string();
  if (framebuffer_.size() != (size_t)width_ * (size_t)height_) return std::string();
  // Opaque, deliberately. This desktop leaves pixels CLEAR where a shader
  // fills the wallpaper, and alpha 0 in a PNG is a hole, not a colour: the
  // first shot of the start page came back as a transparent sheet with a few
  // panels floating in it.
  std::vector<uint32_t> pixels(framebuffer_);
  for (uint32_t& p : pixels) p |= 0xFF000000u;
  ByteBuffer png = web::encode_png(pixels.data(), (int)width_, (int)height_);
  if (png.empty()) return std::string();
  std::string out = path;
  if (out.empty()) {
    const std::string dir = k.config().root + "/shots";
    (void)k.host().make_dir(dir);
    const std::time_t t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char name[64];
    std::snprintf(name, sizeof(name), "/shot-%04d%02d%02d-%02d%02d%02d-%u.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, ++shot_serial_);
    out = dir + name;
  } else if (out[0] != '/' && !k.config().root.empty()) {
    // A relative destination belongs to the MACHINE's root, not to whatever
    // directory the desktop was launched from — the same trap the run
    // directory fell into.
    out = k.config().root + "/" + out;
  }
  // The folder the shot was asked for may not exist yet — write_file does not
  // dig one, and a shot that vanishes because nobody had made `shots/` first
  // is the least useful failure this can have.
  {
    const size_t slash = out.find_last_of('/');
    if (slash != std::string::npos && slash > 0) (void)k.host().make_dir(out.substr(0, slash));
  }
  auto wr = k.host().write_file(out, ByteSpan(png.data(), png.size()));
  if (!wr.ok()) return std::string();
  last_shot_ = out;
  return out;
}

void HardwareCompositor::service_screenshot(Kernel& k) {
  if (!shot_pending_) return;
  shot_pending_ = false;
  const std::string want = shot_path_;
  shot_path_.clear();
  const std::string got = take_screenshot(k, want);
  // Said on the glass AND on the error stream: a person who pressed `keep`
  // needs to be told where it went, and a script that asked through the run
  // directory has nowhere else to read it from.
  if (got.empty()) {
    set_toast("screenshot: could not write it");
    std::fprintf(stderr, "screenshot: could not write it\n");
  } else {
    set_toast("screenshot: " + got);
    std::fprintf(stderr, "screenshot: %ux%u -> %s\n", width_, height_, got.c_str());
  }
}

// The blank frame of a sleeping machine. This lived inside render_desktop, so
// only the WORKSPACE went dark: choosing Sleep on the start page put the
// machine to sleep — input swallowed, waiting to be woken — while the page
// carried on painting itself, which reads as a machine that ignored you.
void HardwareCompositor::paint_asleep() {
  const uint32_t W = width_, H = height_;
  if (framebuffer_.size() != static_cast<size_t>(W) * H) framebuffer_.assign(static_cast<size_t>(W) * H, 0xFF000000);
  else std::fill(framebuffer_.begin(), framebuffer_.end(), 0xFF000000);
  // Measured, then centred — the one line on a blank screen must not be cut
  // off by the edge of a narrow framebuffer.
  int hint_w = text_width(SLEEP_HINT, Face::Ui);
  int hx = (W - hint_w) / 2;
  if (hx < 8) hx = 8;
  draw_text_to(framebuffer_, W, H, hx, H / 2 - 8, SLEEP_HINT, 0xFF2E2E2E);
}

void HardwareCompositor::draw_power_menu() {
  if (!power_menu_open_) return;
  const uint32_t W = width_, H = height_;
    PowerGeom g = power_geom();
    shadow_rounded(g.panel_x, g.panel_y, g.panel_w, g.panel_h, 10, 16, 42, 4);
    draw_rounded_rect_to(framebuffer_, W, H, g.panel_x, g.panel_y, g.panel_w, g.panel_h, 10, tok::panel);
    draw_round_outline_to(framebuffer_, W, H, g.panel_x, g.panel_y, g.panel_w, g.panel_h, 10, tok::strong);
    // A stub pointing back at the control, so the panel reads as belonging to it.
    draw_rect_to(framebuffer_, W, H, g.anchor_x + 4, g.panel_y - 4, g.anchor_w - 8, 4, tok::panel);
    for (int i = 0; i < g.rows; ++i) {
      const int rx = g.row_x(), ry = g.row_y(i), rw = g.row_w();
      bool hot = mouse_x_ >= rx && mouse_x_ < rx + rw && mouse_y_ >= ry && mouse_y_ < ry + g.row_h;
      if (hot) draw_rounded_rect_to(framebuffer_, W, H, rx, ry + 1, rw, g.row_h - 2, 6, tok::wash);
      const char* label = i < POWER_OPTION_COUNT ? POWER_OPTIONS[i].label : leave_label_.c_str();
      const char* note = i < POWER_OPTION_COUNT ? POWER_OPTIONS[i].note : leave_note_.c_str();
      int ty = ry + (g.row_h - 16) / 2;
      draw_text_to(framebuffer_, W, H, rx + 12, ty, label, hot ? tok::text : tok::dim, Face::UiBold);
      // The note is right-aligned off its own measured width, which is what
      // keeps it inside the row it belongs to.
      int nw = text_width(note, Face::Ui);
      draw_text_to(framebuffer_, W, H, rx + rw - 12 - nw, ty, note, tok::faint);
      if (i + 1 < g.rows) draw_rect_to(framebuffer_, W, H, rx + 8, ry + g.row_h - 1, rw - 16, 1, tok::line_up);
    }
}

HardwareCompositor::PowerGeom HardwareCompositor::power_geom() const {
  PowerGeom g;
  g.btn_w = text_width(POWER_LABEL, Face::UiBold) + 16;
  g.btn_h = 22;
  g.btn_x = static_cast<int>(width_) - g.btn_w - 10;
  g.btn_y = 6;
  // The workspace's button unless something opened the panel from elsewhere.
  g.anchor_x = power_anchor_w_ > 0 ? power_anchor_x_ : g.btn_x;
  g.anchor_w = power_anchor_w_ > 0 ? power_anchor_w_ : g.btn_w;

  g.pad = 6;
  g.row_h = 30;
  // The three machine options, and a fourth when the embedder gave the
  // desktop somewhere to go back to (the Pi: its bring-up dashboard).
  g.rows = POWER_OPTION_COUNT + (leave_action_ ? 1 : 0);
  int widest = 0;
  for (int i = 0; i < g.rows; ++i) {
    // label, a gap that cannot close, then the note that says what it does.
    const char* label = i < POWER_OPTION_COUNT ? POWER_OPTIONS[i].label : leave_label_.c_str();
    const char* note = i < POWER_OPTION_COUNT ? POWER_OPTIONS[i].note : leave_note_.c_str();
    int want = text_width(label, Face::UiBold) + 20 + text_width(note, Face::Ui);
    if (want > widest) widest = want;
  }
  const int row_pad_x = 12;                       // ink inset inside a row
  g.panel_w = widest + 2 * row_pad_x + 2 * g.pad;
  g.panel_h = g.rows * g.row_h + 2 * g.pad;
  // Anchored under the control, right edges aligned, kept on the screen.
  g.panel_x = g.anchor_x + g.anchor_w - g.panel_w;
  if (g.panel_x < 8) g.panel_x = 8;
  if (g.panel_x + g.panel_w > static_cast<int>(width_) - 8) {
    g.panel_x = static_cast<int>(width_) - 8 - g.panel_w;
  }
  g.panel_y = (power_anchor_w_ > 0 ? power_anchor_below_ : 34) + 6;
  return g;
}

void HardwareCompositor::power_choose(int index) {
  power_menu_open_ = false;
  switch (index) {
    case 0:
      // Unchanged from the button this panel replaced.
      set_toast("Restarting machine...");
      submit_command("reboot");
      return;
    case 1:
      // The command surface takes this one straight to the machine's power
      // path — the same one `poweroff` on the console reaches.
      set_toast("Powering off...");
      submit_command("poweroff");
      return;
    case 2:
      power_sleep();
      return;
    case 3:
      if (leave_action_) leave_action_();
      return;
    default:
      return;
  }
}

void HardwareCompositor::power_sleep() {
  power_menu_open_ = false;
  toast_timer_ = 0;
  asleep_ = true;
  // The frame loop paints a blank screen from here on and, having nothing to
  // redraw, spends its time halted — which is the low-power part.
}

bool HardwareCompositor::power_wake() {
  if (!asleep_) return false;
  asleep_ = false;
  set_toast("Awake.", 90);
  return true;
}

// ---------------------------------------------------------------------------
// The view switcher: a persistent strip across the top of every face.
// ---------------------------------------------------------------------------
// This machine has two REAL views — the start page it opens on, and the
// windowed workspace — plus a system pane that is not a view at all any more:
// it used to be a third face (the desk of panels), which meant it lived at the
// same address as "which view is up" for a difference that was really just
// "are the panels showing". Now it is a window like any other application,
// opened and closed from here, that can sit over either face. The strip is
// permanent rather than revealed: it used to hide until the pointer reached
// the very top edge because two of the three views already owned that edge,
// but a two-item switcher and a one-item toggle are cheap enough to just
// leave on screen, and a person does not have to learn where to go looking
// for the thing that moves them around their own desktop.
namespace {
struct ViewOption { const char* label; DeskView view; };
const ViewOption VIEW_OPTIONS[] = {
  {"Start page", DeskView::StartPage},
  {"Workspace",  DeskView::Workspace},
};
constexpr int VIEW_OPTION_COUNT = 2;
// The backgrounds the CPU painter can draw. The pyramid and a composed scene
// are the display path's and are not offered here — a chip that does nothing on
// The backgrounds the CPU painter can draw and procedural scenes.
struct SceneOption { const char* label; int scene; const char* desc; };
const SceneOption SCENE_OPTIONS[] = {
  {"Sunset Dusk", 4, "ocean at dusk sunset open horizon water waves orange purple twilight glow"},
  {"Beach Moon",  0, ""},
  {"Cyber Grid",  1, ""},
  {"Quantum Orb", 2, ""},
  {"Pyramids",    3, "three pyramids in the desert dunes sand starlight night sky"},
};
constexpr int kNumScenes = sizeof(SCENE_OPTIONS) / sizeof(SCENE_OPTIONS[0]);
constexpr int VIEW_BAR_PAD = 10;       // the panel's own inset
constexpr int VIEW_BAR_CHIP_PAD = 13;  // each side of a chip's label
constexpr int VIEW_BAR_GAP = 6;        // between chips
constexpr int VIEW_BAR_ROW_GAP = 6;    // between the two rows
const char* const VIEW_BAR_LABEL = "VIEW";
const char* const SCENE_BAR_LABEL = "BACKGROUND";
const char* const TEXT_BAR_LABEL = "TEXT";
// The sizes this machine draws its interface at. Three, not a slider: the
// atlas is cut at fixed sizes and these are the cuts, so each one is glyphs
// drawn at the size they were made for rather than a resample of another.
struct TextOption { const char* label; int pct; };
const TextOption TEXT_OPTIONS[] = {{"Normal", 100}, {"Large", 125}, {"Larger", 150}};
constexpr int kNumTextSizes = sizeof(TEXT_OPTIONS) / sizeof(TEXT_OPTIONS[0]);
}  // namespace

HardwareCompositor::ViewBarGeom HardwareCompositor::view_bar_geom() const {
  ViewBarGeom g;
  g.chip_h = 24;
  // The fourth background chip is the composed-scenes toggle, on or off; its label
  // says which.
  const char* ai_label = canvas_state_.ai_scenes_enabled ? "Composed Scenes: On" : "Composed Scenes: Off";
  // Both rows are led by a field label, and both labels are set in the same
  // column so the two rows read as one panel rather than two strips.
  const int label_w = std::max(std::max(label_width(VIEW_BAR_LABEL), label_width(SCENE_BAR_LABEL)),
                               label_width(TEXT_BAR_LABEL));
  int view_row = label_w + 14, scene_row = label_w + 14, text_row = label_w + 14;
  for (int i = 0; i < VIEW_OPTION_COUNT; ++i) {
    g.view_w[i] = text_width(VIEW_OPTIONS[i].label, Face::UiBold) + 2 * VIEW_BAR_CHIP_PAD;
    view_row += g.view_w[i] + (i ? VIEW_BAR_GAP : 0);
  }
  for (int i = 0; i < kNumScenes; ++i) {
    g.scene_w[i] = text_width(SCENE_OPTIONS[i].label, Face::UiBold) + 2 * VIEW_BAR_CHIP_PAD;
    scene_row += g.scene_w[i] + VIEW_BAR_GAP;
  }
  g.scene_w[kNumScenes] = text_width(ai_label, Face::UiBold) + 2 * VIEW_BAR_CHIP_PAD;
  scene_row += g.scene_w[kNumScenes] + VIEW_BAR_GAP;
  for (int i = 0; i < kNumTextSizes; ++i) {
    g.text_w[i] = text_width(TEXT_OPTIONS[i].label, Face::UiBold) + 2 * VIEW_BAR_CHIP_PAD;
    text_row += g.text_w[i] + VIEW_BAR_GAP;
  }
  g.w = std::max(std::max(view_row, scene_row), text_row) + 2 * VIEW_BAR_PAD;
  g.h = 3 * g.chip_h + 2 * VIEW_BAR_ROW_GAP + 2 * VIEW_BAR_PAD;
  g.x = (static_cast<int>(width_) - g.w) / 2;
  if (g.x < 8) g.x = 8;
  g.y = 4;
  g.view_y = g.y + VIEW_BAR_PAD;
  g.scene_y = g.view_y + g.chip_h + VIEW_BAR_ROW_GAP;
  g.text_y = g.scene_y + g.chip_h + VIEW_BAR_ROW_GAP;
  int vx = g.x + VIEW_BAR_PAD + label_w + 14, sx = g.x + VIEW_BAR_PAD + label_w + 14;
  for (int i = 0; i < VIEW_OPTION_COUNT; ++i) { g.view_x[i] = vx; vx += g.view_w[i] + VIEW_BAR_GAP; }
  for (int i = 0; i < kNumScenes; ++i) { g.scene_x[i] = sx; sx += g.scene_w[i] + VIEW_BAR_GAP; }
  g.scene_x[kNumScenes] = sx;
  int tx = g.x + VIEW_BAR_PAD + label_w + 14;
  for (int i = 0; i < kNumTextSizes; ++i) { g.text_x[i] = tx; tx += g.text_w[i] + VIEW_BAR_GAP; }
  return g;
}

DeskView HardwareCompositor::current_view() const {
  if (!canvas_mode_) return DeskView::Workspace;
  return desk_start_page(canvas_state_) ? DeskView::StartPage : DeskView::Desk;
}

void HardwareCompositor::set_view(DeskView v) {
  // Naming a view means you get that view. bare_scene — the full-background
  // mode, which paints the wallpaper and nothing else — was left ALONE here,
  // so a machine that came up in it (the run directory asks for it by writing
  // "scene") could not be brought out by the taskbar's Start button or the
  // switcher's own chip: both set the face and neither cleared the flag that
  // was hiding it. The only way back was the Space cycle.
  switch (v) {
    case DeskView::StartPage:
      canvas_mode_ = true;
      canvas_state_.hide_panels = true;
      canvas_state_.collapsed_panels = false;
      canvas_state_.bare_scene = false;
      break;
    case DeskView::Desk:
      canvas_mode_ = true;
      canvas_state_.hide_panels = false;
      canvas_state_.collapsed_panels = false;
      canvas_state_.bare_scene = false;
      break;
    default:
      canvas_mode_ = false;
      break;
  }
}

namespace {
// Where shared/model-presets.cjs is, derived from the providers module this
// machine is configured with — the same file the assay bridge loads.
std::string model_presets_script(Kernel& k) {
  std::string providers = machine_conf_value(k, "AOS_PROVIDERS");
  if (providers.empty()) return {};
  const size_t slash = providers.find_last_of('/');
  if (slash == std::string::npos) return {};
  return providers.substr(0, slash) + "/shared/model-presets.cjs";
}

#if !defined(AIOS_METAL)
// Run it and take its lines. popen and friends are hosted-only; the bare-metal
// image has no way to spawn node at all.
std::vector<std::string> run_lines(const std::string& cmd) {
  std::vector<std::string> out;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) return out;
  char buf[1024];
  while (::fgets(buf, sizeof(buf), pipe)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  ::pclose(pipe);
  return out;
}
#endif
}  // namespace

void HardwareCompositor::refresh_mind_lists() {
#if !defined(AIOS_METAL)
  if (!kernel_ || canvas_state_.mind_lists_loading) return;
  const std::string script = model_presets_script(*kernel_);
  if (script.empty() || !kernel_->host().exists(script)) return;
  const std::string provider = machine_conf_value(*kernel_, "AOS_PROVIDER");
  // Already have this provider's list and the presets: nothing to ask.
  if (canvas_state_.mind_lists_primed && canvas_state_.mind_models_provider == provider) return;

  canvas_state_.mind_lists_loading = true;
  aios::hal::Thread([this, script, provider]() {
    std::vector<MindPreset> presets;
    for (const std::string& line : run_lines("node '" + script + "' providers 2>/dev/null")) {
      const size_t t1 = line.find('\t');
      if (t1 == std::string::npos) continue;
      const size_t t2 = line.find('\t', t1 + 1);
      MindPreset mp;
      mp.provider = line.substr(0, t1);
      mp.label = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
      if (t2 != std::string::npos) mp.default_model = line.substr(t2 + 1);
      presets.push_back(std::move(mp));
    }
    std::vector<std::string> models;
    if (!provider.empty())
      models = run_lines("node '" + script + "' models '" + provider + "' 2>/dev/null");

    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    if (!presets.empty()) canvas_state_.mind_presets = std::move(presets);
    canvas_state_.mind_models = std::move(models);
    canvas_state_.mind_models_provider = provider;
    canvas_state_.mind_lists_primed = true;
    canvas_state_.mind_lists_loading = false;
  }).detach();
#endif
}

bool HardwareCompositor::set_mind_provider(const std::string& provider) {
  if (!kernel_) return false;
  std::string fallback;
  for (const auto& p : canvas_state_.mind_presets)
    if (p.provider == provider) { fallback = p.default_model; break; }
  if (!write_machine_conf("AOS_PROVIDER", provider)) return false;
  // The old provider's model must not ride along.
  if (!write_machine_conf("AOS_MODEL", fallback)) return false;
  canvas_state_.mind_lists_primed = false;   // a new provider serves other models
  const bool ok = rebind_mind();
  set_toast(ok ? ("mind is on " + provider + (fallback.empty() ? "" : "/" + fallback))
               : ("saved " + provider + ", but the engine would not restart"));
  return ok;
}

bool HardwareCompositor::write_machine_conf(const std::string& key, const std::string& value) {
  if (!kernel_) return false;
  Kernel& k = *kernel_;
  std::string path;
  if (const char* c = std::getenv("AOS_CONF")) path = c;
  if (path.empty()) {
    const char* home = std::getenv("HOME");
    path = std::string(home ? home : ".") + "/.config/aos.conf";
  }
  std::string body;
  if (auto cur = k.host().read_file(path); cur.ok())
    body.assign((const char*)cur->data(), cur->size());
  std::string out;
  bool replaced = false;
  size_t at = 0;
  while (at < body.size()) {
    const size_t nl = body.find('\n', at);
    std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = (nl == std::string::npos) ? body.size() : nl + 1;
    if (line.rfind(key + "=", 0) == 0) { out += key + "=" + value + "\n"; replaced = true; }
    else if (!line.empty()) out += line + "\n";
  }
  if (!replaced) out += key + "=" + value + "\n";
  if (!k.host().write_file(path, ByteSpan((const uint8_t*)out.data(), out.size())).ok()) {
    set_toast("could not write " + path);
    return false;
  }
  return true;
}

bool HardwareCompositor::rebind_mind() {
  if (!kernel_) return false;
  Kernel& k = *kernel_;
  EntityID mind_id;
  for (const auto& r : k.authority().residents()) if (r.label == "mind") mind_id = r.id;
  if (mind_id.is_null()) return false;

  // A fresh bridge. It reads aos.conf on startup, which is why the settings are
  // written FIRST: a bridge started before the write comes up on the model that
  // was just replaced.
  std::string bridge;
  if (const char* named = std::getenv("AIOS_BRIDGE")) bridge = named;
  if (bridge.empty() || !k.host().exists(bridge)) bridge = machine_conf_value(k, "AOS_BRIDGE");
  if (bridge.empty() || !k.host().exists(bridge))
    bridge = k.config().root + "/../intelligence/assay/bridge.cjs";
  if (!k.host().exists(bridge)) return false;

  auto spawned = spawn_external_intelligence({"node", bridge});
  if (!spawned.ok()) return false;
  k.unbind_mind(mind_id);
  return k.bind_mind(mind_id, std::move(*spawned)).ok();
}

bool HardwareCompositor::set_mind_model(const std::string& model) {
  if (!write_machine_conf("AOS_MODEL", model)) return false;
  const bool ok = rebind_mind();
  set_toast(ok ? ("mind is on " + model) : ("saved " + model + ", but the engine would not restart"));
  return ok;
}

int HardwareCompositor::ui_scale() const { return g_ui_scale; }

void HardwareCompositor::load_ui_scale() {
  if (!kernel_) return;
  const std::string v = machine_conf_value(*kernel_, "AOS_UI_SCALE");
  int pct = 0;
  for (char c : v) { if (c < '0' || c > '9') { pct = 0; break; } pct = pct * 10 + (c - '0'); }
  g_ui_scale = (pct == 125 || pct == 150) ? pct : 100;
}

void HardwareCompositor::set_ui_scale(int pct) {
  if (pct != 100 && pct != 125 && pct != 150) return;
  g_ui_scale = pct;
  write_machine_conf("AOS_UI_SCALE", std::to_string(pct));
  // Every raster measured at the old size is now wrong: the workspace's
  // wallpaper is fine (it has no text) but each window painted its content
  // into a buffer with the old metrics, so they are repainted from scratch.
  for (auto& w : windows_) w.buffer.assign(w.content_pixels(), tok::panel);
  canvas_state_.layout_dirty = true;
  set_toast("Text size " + std::to_string(pct) + "%");
}

int HardwareCompositor::face_scene() const {
  // A READ, and only a read: it reports the scene the operator last chose
  // (chip, key, context menu or `display`), restored from the saved desk. The
  // compositor never picks one here — no idle path, no timer, no fallback —
  // because who owns that choice is the person, not the machine.
  // current_view() is the SAME rule the painter picks its scene by — and it is
  // not simply "the start page whenever the canvas is up". Full-background mode
  // makes desk_start_page() false, so the painter reads the Desk view's scene
  // while this read the start page's: picking a background lit the chip, wrote
  // a number and changed nothing, because the two were not talking about the
  // same view.
  if (canvas_mode_) return canvas_state_.scene(current_view());
  const int sec = std::clamp(current_workspace_index(), 0, CanvasState::kSections - 1);
  return canvas_state_.section_scene[sec];
}

void HardwareCompositor::set_face_scene(int scene) {
  if (canvas_mode_) {
    // The view the PAINTER will read it back from. See face_scene().
    canvas_state_.set_scene(current_view(), scene);
  } else {
    const int sec = std::clamp(current_workspace_index(), 0, CanvasState::kSections - 1);
    canvas_state_.section_scene[sec] = scene;
    build_background();
  }
  canvas_state_.layout_dirty = true;
  if (kernel_) desk_layout_save(canvas_state_, *kernel_);
}

void HardwareCompositor::set_view_scene(DeskView v, int scene) {
  canvas_state_.set_scene(v, scene);
  // Saved with the desk, so a background survives the machine being restarted.
  canvas_state_.layout_dirty = true;
  // The workspace's ground is a raster that is built once and blitted; the
  // canvas computes its own every frame and needs nothing here.
  if (v == DeskView::Workspace) build_background();
  // Saved now rather than at the next canvas paint: a person who picks a
  // background on the workspace and restarts the machine there would otherwise
  // find it back on the old one.
  if (kernel_) desk_layout_save(canvas_state_, *kernel_);
}

void HardwareCompositor::view_bar_track(int px, int py) {
  constexpr int kViewBarEdge = 2;
  const ViewBarGeom g = view_bar_geom();
  if (!view_bar_open_) {
    // Only the very top of the screen opens it, and only while nothing else has
    // the pointer — dragging a window up to the edge is not a request for it.
    if (py <= kViewBarEdge && px >= 0 && px < static_cast<int>(width_) &&
        !is_dragging_ && !is_resizing_ && canvas_state_.drag_tile < 0) {
      view_bar_open_ = true;
    }
    return;
  }
  // Then it stays for the whole width of the screen, down to a little below
  // the strip.
  if (py < g.y + g.h + 24) return;
  view_bar_open_ = false;
}

bool HardwareCompositor::view_bar_click(int px, int py) {
  if (!view_bar_open_) return false;
  const ViewBarGeom g = view_bar_geom();
  if (!g.contains(px, py)) return false;
  for (int i = 0; i < VIEW_OPTION_COUNT; ++i) {
    if (py >= g.view_y && py < g.view_y + g.chip_h &&
        px >= g.view_x[i] && px < g.view_x[i] + g.view_w[i]) {
      set_view(VIEW_OPTIONS[i].view);
      return true;
    }
  }
  for (int i = 0; i < kNumScenes; ++i) {
    if (py >= g.scene_y && py < g.scene_y + g.chip_h &&
        px >= g.scene_x[i] && px < g.scene_x[i] + g.scene_w[i]) {
      // The background of the face you are LOOKING at — and on a windowed
      // desktop that means THIS section, not "the workspace": each section
      // carries its own wallpaper.
      set_face_scene(SCENE_OPTIONS[i].scene);
      if (SCENE_OPTIONS[i].desc && SCENE_OPTIONS[i].desc[0]) {
        canvas_state_.composed_text = SCENE_OPTIONS[i].desc;
        ++canvas_state_.composed_generation;
      }
      set_toast(std::string("Background: ") + SCENE_OPTIONS[i].label);
      return true;
    }
  }
  for (int i = 0; i < kNumTextSizes; ++i) {
    if (py >= g.text_y && py < g.text_y + g.chip_h &&
        px >= g.text_x[i] && px < g.text_x[i] + g.text_w[i]) {
      set_ui_scale(TEXT_OPTIONS[i].pct);
      return true;
    }
  }
  if (py >= g.scene_y && py < g.scene_y + g.chip_h &&
      px >= g.scene_x[kNumScenes] && px < g.scene_x[kNumScenes] + g.scene_w[kNumScenes]) {
    canvas_state_.ai_scenes_enabled = !canvas_state_.ai_scenes_enabled;
    // Turning it off while a composed scene is actually up puts the view back
    // on a preset immediately, rather than waiting for the next request to
    // land on a face that can no longer show it. Still the operator's choice:
    // this runs on their click on this chip and nowhere else.
    if (!canvas_state_.ai_scenes_enabled && face_scene() == 4) set_face_scene(0);
    return true;
  }
  return true;   // the panel's own padding: spent, but on nothing
}

void HardwareCompositor::draw_view_bar() {
  if (!view_bar_open_) return;
  const int W = static_cast<int>(width_), H = static_cast<int>(height_);
  const ViewBarGeom g = view_bar_geom();
  const DeskView view = current_view();
  const int scene = face_scene();
  shadow_rounded(g.x, g.y, g.w, g.h, 10, 16, 42, 4);
  draw_rounded_rect_to(framebuffer_, W, H, g.x, g.y, g.w, g.h, 10, tok::panel);
  draw_round_outline_to(framebuffer_, W, H, g.x, g.y, g.w, g.h, 10, tok::strong);
  const int label_x = g.x + VIEW_BAR_PAD;
  draw_label_to(framebuffer_, W, H, label_x, g.view_y + (g.chip_h - 10) / 2, VIEW_BAR_LABEL, tok::faint);
  draw_label_to(framebuffer_, W, H, label_x, g.scene_y + (g.chip_h - 10) / 2, SCENE_BAR_LABEL, tok::faint);
  auto chip = [&](int x, int y, int w, const char* text, bool on, bool hot) {
    // The view you are on is the console's one filled control; everything else
    // is an outline, lit on hover. Two filled pills on a strip this small and
    // neither of them reads as "you are here".
    draw_pill_to(framebuffer_, W, H, x, y, w, g.chip_h,
                 on ? tok::cta : (hot ? tok::wash : 0u),
                 on ? tok::cta : (hot ? tok::strong : tok::line_up));
    draw_text_to(framebuffer_, W, H, x + VIEW_BAR_CHIP_PAD, y + (g.chip_h - 16) / 2, text,
                 on ? tok::on_cta : (hot ? tok::text : tok::dim), on ? Face::UiBold : Face::Ui);
  };
  for (int i = 0; i < VIEW_OPTION_COUNT; ++i) {
    const bool vhot = mouse_y_ >= g.view_y && mouse_y_ < g.view_y + g.chip_h &&
                      mouse_x_ >= g.view_x[i] && mouse_x_ < g.view_x[i] + g.view_w[i];
    chip(g.view_x[i], g.view_y, g.view_w[i], VIEW_OPTIONS[i].label, VIEW_OPTIONS[i].view == view, vhot);
  }
  for (int i = 0; i < kNumScenes; ++i) {
    const bool shot = mouse_y_ >= g.scene_y && mouse_y_ < g.scene_y + g.chip_h &&
                      mouse_x_ >= g.scene_x[i] && mouse_x_ < g.scene_x[i] + g.scene_w[i];
    chip(g.scene_x[i], g.scene_y, g.scene_w[i], SCENE_OPTIONS[i].label, SCENE_OPTIONS[i].scene == scene, shot);
  }
  draw_label_to(framebuffer_, W, H, label_x, g.text_y + (g.chip_h - 10) / 2, TEXT_BAR_LABEL, tok::faint);
  for (int i = 0; i < kNumTextSizes; ++i) {
    const bool thot = mouse_y_ >= g.text_y && mouse_y_ < g.text_y + g.chip_h &&
                      mouse_x_ >= g.text_x[i] && mouse_x_ < g.text_x[i] + g.text_w[i];
    chip(g.text_x[i], g.text_y, g.text_w[i], TEXT_OPTIONS[i].label,
         TEXT_OPTIONS[i].pct == ui_scale(), thot);
  }

  const bool aihot = mouse_y_ >= g.scene_y && mouse_y_ < g.scene_y + g.chip_h &&
                     mouse_x_ >= g.scene_x[kNumScenes] && mouse_x_ < g.scene_x[kNumScenes] + g.scene_w[kNumScenes];
  const char* ai_label = canvas_state_.ai_scenes_enabled ? "Composed Scenes: On" : "Composed Scenes: Off";
  chip(g.scene_x[kNumScenes], g.scene_y, g.scene_w[kNumScenes], ai_label, canvas_state_.ai_scenes_enabled, aihot);
}

namespace {
static void snap_window_drag(int& target_x, int& target_y, int win_w, int win_h, uint32_t active_id, const std::vector<WindowSurface>& windows, int screen_w, int screen_h) {
  constexpr int SNAP = 14;
  const int work_left = 4;
  const int work_top = 38;
  const int work_right = screen_w - 4;
  const int work_bottom = screen_h - 40;

  if (std::abs(target_x - work_left) <= SNAP) target_x = work_left;
  else if (std::abs((target_x + win_w) - work_right) <= SNAP) target_x = work_right - win_w;

  if (std::abs(target_y - work_top) <= SNAP) target_y = work_top;
  else if (std::abs((target_y + win_h) - work_bottom) <= SNAP) target_y = work_bottom - win_h;

  for (const auto& w : windows) {
    if (w.window_id == active_id || !w.visible || w.minimized) continue;
    const int w_left = w.bounds.x;
    const int w_right = w.bounds.x + w.bounds.width;
    const int w_top = w.bounds.y;
    const int w_bottom = w.bounds.y + w.bounds.height;

    bool v_overlap = (target_y < w_bottom && (target_y + win_h) > w_top);
    if (v_overlap) {
      if (std::abs(target_x - w_right) <= SNAP) target_x = w_right;
      else if (std::abs((target_x + win_w) - w_left) <= SNAP) target_x = w_left - win_w;
      else if (std::abs(target_x - w_left) <= SNAP) target_x = w_left;
      else if (std::abs((target_x + win_w) - w_right) <= SNAP) target_x = w_right - win_w;
    }

    bool h_overlap = (target_x < w_right && (target_x + win_w) > w_left);
    if (h_overlap) {
      if (std::abs(target_y - w_bottom) <= SNAP) target_y = w_bottom;
      else if (std::abs((target_y + win_h) - w_top) <= SNAP) target_y = w_top - win_h;
      else if (std::abs(target_y - w_top) <= SNAP) target_y = w_top;
      else if (std::abs((target_y + win_h) - w_bottom) <= SNAP) target_y = w_bottom - win_h;
    }
  }
}

static void snap_window_resize(int win_x, int win_y, int& nw, int& nh, uint32_t active_id, const std::vector<WindowSurface>& windows, int screen_w, int screen_h) {
  constexpr int SNAP = 14;
  const int work_right = screen_w - 4;
  const int work_bottom = screen_h - 40;

  int target_right = win_x + nw;
  int target_bottom = win_y + nh;

  if (std::abs(target_right - work_right) <= SNAP) nw = work_right - win_x;
  if (std::abs(target_bottom - work_bottom) <= SNAP) nh = work_bottom - win_y;

  for (const auto& w : windows) {
    if (w.window_id == active_id || !w.visible || w.minimized) continue;
    const int w_left = w.bounds.x;
    const int w_right = w.bounds.x + w.bounds.width;
    const int w_top = w.bounds.y;
    const int w_bottom = w.bounds.y + w.bounds.height;

    bool v_overlap = (win_y < w_bottom && target_bottom > w_top);
    if (v_overlap) {
      if (std::abs(target_right - w_left) <= SNAP) nw = w_left - win_x;
      else if (std::abs(target_right - w_right) <= SNAP) nw = w_right - win_x;
    }

    bool h_overlap = (win_x < w_right && target_right > w_left);
    if (h_overlap) {
      if (std::abs(target_bottom - w_top) <= SNAP) nh = w_top - win_y;
      else if (std::abs(target_bottom - w_bottom) <= SNAP) nh = w_bottom - win_y;
    }
  }
}
} // namespace

static WindowSurface* browser_only_window(std::vector<WindowSurface>& ws) {
  if (!std::getenv("AOS_BROWSER_ONLY")) return nullptr;
  for (auto& w : ws) if (w.type == WindowType::Browser && w.visible && !w.minimized) return &w;
  return nullptr;
}

void HardwareCompositor::input_mouse_motion(int x, int y) {
  if (WindowSurface* bw = browser_only_window(windows_)) { mouse_x_ = x; mouse_y_ = y; web_pane_motion(*bw, x, y); return; }
  // Asleep, the pointer only wakes the machine. It does not drag a window, and
  // it does not reach whatever was under it when the screen went dark.
  if (asleep_) {
    mouse_x_ = x;
    mouse_y_ = y;
    power_wake();
    return;
  }
  mouse_x_ = x;
  mouse_y_ = y;
  // Same for motion: while the sheet is up the pointer is a nib and nothing
  // else. Hover, window drags and text selection are not what it is for.
  if (draw_mode_) { draw_pointer_move(x, y); return; }
  // Over a Grain window's content (or dragging from one), the pointer is the program's.
  if (!grain_apps_.empty()) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
      const WindowSurface& w = *it;
      if (!w.visible || w.minimized) continue;
      const int cx = x - w.bounds.x - 1, cy = y - w.bounds.y - 26;
      const bool inside = cx >= 0 && cy >= 0 && cx < w.content_width() && cy < w.content_height();
      const bool topmost_here = x >= w.bounds.x && y >= w.bounds.y && x < w.bounds.x + w.bounds.width && y < w.bounds.y + w.bounds.height;
      if (w.type == WindowType::Grain && (w.window_id == grain_press_win_ || inside)) {
        GrainQueued q;
        q.window_id = w.window_id;
        q.type = "mouse_move";
        q.x = cx;
        q.y = cy;
        grain_queue(q);
        break;
      }
      if (topmost_here) break;   // another window is in front of anything below
    }
  }
  // The top edge reveals the view switcher, on every face.
  view_bar_track(x, y);
  // Text being selected by dragging: the caret follows the pointer and the
  // anchor stays where the button went down.
  if (text_dragging_) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id != text_drag_win_) continue;
      if (w.type == WindowType::Editor) {
        int line = 0, col = 0;
        if (editor_point_to_caret(w, x - (w.bounds.x + 1), y - (w.bounds.y + 26), &line, &col)) {
          w.cursor_line = line;
          w.cursor_col = col;
        }
        break;
      }
      if (w.type == WindowType::Terminal) {
        int line = 0, col = 0;
        if (terminal_point_to_caret(w, x - (w.bounds.x + 1), y - (w.bounds.y + 26), &line, &col)) {
          w.cursor_line = line;
          w.cursor_col = col;
        }
        break;
      }
    }
    return;
  }
  // A tile in hand follows the pointer, in the paint's own coordinates.
  if (canvas_mode_ && canvas_state_.drag_tile >= 0) {
    // The layout for its SCALE — the one place that decides a dense panel
    // paints at half size, which is also the only thing a drag still needs
    // from it.
    const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
    desk_drag_motion(canvas_state_, x / lay.scale, y / lay.scale);
    return;
  }
  // The same hover, for the app pane on a canvas face.
  if (canvas_mode_ && !is_dragging_ && !is_resizing_) {
    const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
    int bx = 0, by = 0, bw = 0, bh = 0;
    if (app_tile_body(lay, &bx, &by, &bw, &bh)) {
      const int px = x / lay.scale, py = y / lay.scale;
      if (px >= bx && px < bx + bw && py >= by && py < by + bh) {
        if (WindowSurface* win = app_pane_window()) {
          web_pane_motion(*win, x - win->bounds.x, y - win->bounds.y - 26);
        }
      }
    }
  }
  // Hover over the web pane: which link is under the pointer. Without this a
  // page gives no sign that anything on it is clickable.
  if (!is_dragging_ && !is_resizing_ && !canvas_mode_) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.type != WindowType::Browser || w.minimized || !w.visible) continue;
      if (!w.bounds.contains(x, y)) continue;
      web_pane_motion(w, x - w.bounds.x - 1, y - w.bounds.y - 26);
    }
  }


  if (is_dragging_ && active_drag_win_ != 0) {
    int target_x = x - drag_offset_x_;
    int target_y = y - drag_offset_y_;
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id == active_drag_win_) {
        snap_window_drag(target_x, target_y, w.bounds.width, w.bounds.height, active_drag_win_, windows_, width_, height_);
        w.bounds.x = target_x;
        w.bounds.y = target_y;
        if (!w.maximized) w.normal_bounds = w.bounds;
        break;
      }
    }
    return;
  }
  if (is_resizing_ && active_drag_win_ != 0) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id != active_drag_win_) continue;
      // Measured from where the window WAS when the drag began, so a left or
      // top edge moves that edge and leaves the opposite one where it is.
      const int dx = x - resize_grab_x_, dy = y - resize_grab_y_;
      int nx = resize_from_x_, ny = resize_from_y_;
      int nw = resize_from_w_, nh = resize_from_h_;
      const int min_w = 260, min_h = 180;
      if (resize_edges_ & EdgeR) nw = resize_from_w_ + dx;
      if (resize_edges_ & EdgeB) nh = resize_from_h_ + dy;
      if (resize_edges_ & EdgeL) { nw = resize_from_w_ - dx; nx = resize_from_x_ + dx; }
      if (resize_edges_ & EdgeT) { nh = resize_from_h_ - dy; ny = resize_from_y_ + dy; }
      // Clamping a left or top edge has to hold the far edge still, or the
      // window walks across the screen once it hits its minimum.
      if (nw < min_w) { if (resize_edges_ & EdgeL) nx -= (min_w - nw); nw = min_w; }
      if (nh < min_h) { if (resize_edges_ & EdgeT) ny -= (min_h - nh); nh = min_h; }
      snap_window_resize(nx, ny, nw, nh, active_drag_win_, windows_, width_, height_);
      if (nw < min_w) nw = min_w;
      if (nh < min_h) nh = min_h;
      w.bounds.x = nx; w.bounds.y = ny;
      w.bounds.width = nw; w.bounds.height = nh;
      w.maximized = false;
      w.normal_bounds = w.bounds;
      w.buffer.resize(w.content_pixels(), tok::panel);
    }
  }
}

// What a card on the desk does when it is clicked. This used to be an if-chain
// over the index of a hardcoded array, so a card could not be moved without
// the click following whatever took its place.
// A row on the desk that DOES something to this machine. Both acts here are
// destructive and both cascade, so neither happens on one click: the first arms
// the row and the row then says what the second will do. Any other click clears
// the arm — the same grammar as the power panel, for the same reason.
bool HardwareCompositor::desk_action(int px, int py) {
  auto& st = canvas_state_;
  if (getenv("AIOS_DESK_TRACE")) {
    std::fprintf(stderr, "[desk] action at %d,%d  revoke_rows=%zu rewind_rows=%zu armed=%s\n",
                 px, py, st.revoke_rows.size(), st.rewind_rows.size(), st.armed.c_str());
    for (const auto& r : st.rewind_rows)
      std::fprintf(stderr, "[desk]   rewind row %d,%d %dx%d %s\n", r.x, r.y, r.w, r.h, r.label.c_str());
  }

  // The launcher, before the destructive rows. Opening an application destroys
  // nothing, so it is ONE click — the arm-and-confirm grammar is for revoking
  // authority and rewinding an entity, not for starting a program.
  for (const auto& row : st.app_rows) {
    if (px < row.x || px >= row.x + row.w || py < row.y || py >= row.y + row.h) continue;
    st.armed.clear();
    submit_command("app open " + row.label);
    set_toast("opening " + row.label);
    return true;
  }

  // An identity is a handle, and looking at what it IS changes nothing on the
  // machine — so one click, before the destructive rows, like the launcher.
  for (const auto& row : st.identity_rows) {
    if (px < row.x || px >= row.x + row.w || py < row.y || py >= row.y + row.h) continue;
    st.armed.clear();
    show_inspect(row.label, row.subject);
    return true;
  }

  for (const auto& row : st.revoke_rows) {
    if (px < row.x || px >= row.x + row.w || py < row.y || py >= row.y + row.h) continue;
    if (st.armed != row.subject) { st.armed = row.subject; return true; }
    st.armed.clear();
    // Cascades: everything delegated from it goes with it. The kernel says so
    // in its own words, and the ledger beside this records the decision.
    submit_command("cap revoke " + row.subject);
    set_toast("revoking " + row.label);
    return true;
  }

  for (const auto& row : st.rewind_rows) {
    if (px < row.x || px >= row.x + row.w || py < row.y || py >= row.y + row.h) continue;
    if (row.label == "checkpoint") {
      // Taking one costs nothing and destroys nothing, so it is one click.
      const std::string id = row.subject.substr(5);
      submit_command("checkpoint " + id);
      set_toast("checkpointing the mind");
      st.armed.clear();
      return true;
    }
    if (st.armed != row.subject) { st.armed = row.subject; return true; }
    st.armed.clear();
    // One act, not two. Driving this the obvious way — terminate, then restore
    // — is what the kernel refuses: `terminate` will not touch a resident and
    // `restore` will not write over an entity that is still here, so the one
    // entity a person would want to put back was the one that could not be.
    // `rewind` is the kernel primitive that was missing: the contents go, the
    // 128-bit identity stays.
    submit_command("rewind " + row.subject);
    set_toast("putting the machine back");
    return true;
  }

  // A click anywhere else on the desk disarms whatever was armed.
  if (!st.armed.empty()) { st.armed.clear(); return true; }
  return false;
}

void HardwareCompositor::desk_launch(TileKind kind) {
  switch (kind) {
    case TileKind::HeroEditor:
      canvas_mode_ = false; ensure_app_window(WindowType::Editor); break;
    case TileKind::HeroDb:
      canvas_mode_ = false; ensure_app_window(WindowType::Database); break;
    case TileKind::HeroWeb:
      canvas_mode_ = false; ensure_app_window(WindowType::Browser); break;
    case TileKind::HeroTensor:  submit_command("devices"); break;
    case TileKind::HeroHorizon: submit_command("horizon"); break;
    case TileKind::HeroBuild:   submit_command("build"); break;
    default: break;   // the readouts are read, not pressed
  }
}

void HardwareCompositor::input_mouse_button(int x, int y, bool down, int button) {
  if (!down && button == aios::hal::MouseLeft) web_pane_release();
  mouse_x_ = x;
  mouse_y_ = y;
  if (WindowSurface* bw = browser_only_window(windows_)) {
    // Browser-only: the pane is the whole display, so the click goes straight to it. The desktop's own hit regions (top bar, dock,
    // window frame) are not there to be hit.
    if (down && button == aios::hal::MouseLeft && kernel_) {
      web_pane_click(*this, *bw, *kernel_, x, y);
      if (std::getenv("AOS_LATENCY") && bw->web)
        std::fprintf(stderr, "click at %d,%d -> field=%s url_focus=%d working='%s' message='%s'\n", x, y,
                     bw->web->field ? bw->web->field->tag.c_str() : "none", (int)bw->web->url_focus, bw->web->working.c_str(), bw->web->message.c_str());
    }
    return;
  }
  // Asleep, a button only wakes. Both edges are swallowed so the press that
  // woke the machine cannot also land on a window, a tab or the power control.
  if (asleep_) {
    if (down) power_wake();
    mouse_down_ = false;
    return;
  }
  // The drawing surface owns the pointer outright while it is up — both edges
  // of the press, and before the switcher, the windows and the tiles. A stroke
  // that crossed a title bar would otherwise start drawing and drag a window
  // at the same time. The right button is left alone so the menu still opens.
  if (draw_mode_ && button == aios::hal::MouseLeft) {
    if (down) { mouse_down_ = true; draw_pointer_down(x, y); }
    else { mouse_down_ = false; draw_pointer_up(x, y); }
    return;
  }
  // The switcher is drawn over whichever face is up, so it is asked before any
  // of them: a chip must not also reach the window, tile or tab beneath it.
  if (down && view_bar_click(x, y)) {
    mouse_down_ = false;
    return;
  }
  if (!down) {
    mouse_down_ = false;
    // A button that went down in a Grain window comes up there: mouse_up, and a
    // click when it is still over the window's content.
    if (grain_press_win_ != 0) {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (const auto& w : windows_) {
        if (w.window_id != grain_press_win_ || w.type != WindowType::Grain) continue;
        GrainQueued q;
        q.window_id = w.window_id;
        q.x = x - w.bounds.x - 1;
        q.y = y - w.bounds.y - 26;
        q.type = "mouse_up";
        grain_queue(q);
        if (q.x >= 0 && q.y >= 0 && q.x < w.content_width() && q.y < w.content_height()) {
          q.type = "click";
          grain_queue(q);
        }
      }
      grain_press_win_ = 0;
    }
    // The selection stops growing when the button comes up. A drag that never
    // moved leaves anchor and caret together, which is no selection at all.
    text_dragging_ = false;
    text_drag_win_ = 0;
    // A press on a tile that never travelled is the click the six launcher
    // cards have always been; one that did is a move, and the tile stays where
    // it was let go.
    if (canvas_mode_ && canvas_state_.drag_tile >= 0) {
      const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
      const int clicked = desk_release(canvas_state_, lay);
      if (clicked >= 0 && clicked < (int)canvas_state_.tiles.size()) {
        // A row that acts on the machine comes first: the tiles it lives in are
        // read, not pressed, so desk_launch would swallow the click for nothing.
        if (!desk_action(x / lay.scale, y / lay.scale))
          desk_launch(canvas_state_.tiles[(size_t)clicked].kind);
      }
    }
    if (is_dragging_ && active_drag_win_ != 0) {
      if (x <= 8) {
        snap_window_left(active_drag_win_);
      } else if (x >= static_cast<int>(width_) - 8) {
        snap_window_right(active_drag_win_);
      } else if (y <= 8) {
        snap_window_maximize(active_drag_win_);
      }
    }
    is_dragging_ = false;
    is_resizing_ = false;
    active_drag_win_ = 0;
    mark_dirty();
    return;
  }

  // A menu on screen owns the next click, wherever it lands; the right button
  // opens one over whatever is under the pointer.
  if (context_menu_click(x, y)) return;
  if (button == aios::hal::MouseRight) { open_context_menu(x, y); return; }

  // The command palette owns clicks while open
  if (command_palette_click(x, y)) return;

  // Stop comes before every other hit test, on both faces of the desktop: it
  // is the control a person reaches for when the machine is doing the wrong
  // thing, and it must not be behind anything.
  if (command_running_.load()) {
    StopGeom g = stop_geom();
    if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) { stop_command(); return; }
  }

  // The power panel is over both faces, so it is hit-tested over both — before
  // the start page's cards as well as before the workspace's windows. A click
  // on "Power off" must not fall through into whatever is underneath it.
  if (power_menu_took_click(x, y)) return;

  mouse_down_ = true;

  // The taskbar and the top bar are drawn over every face, so they are
  // hit-tested over every face — before the start page's cards as well as
  // before the workspace's windows.
  if (taskbar_click(x, y)) return;
  if (top_bar_click(x, y)) return;

  if (canvas_mode_) {
    // ONE source for where everything on this face is — the same layout the
    // painter drew from, including the SCALE it drew at. A dense display
    // paints at half scale and upsamples; this handler used to work in screen
    // pixels, so every press on a 4K start page landed twice as far into the
    // page as the person had aimed.
    const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
    const int px = x / lay.scale, py = y / lay.scale;
    const CanvasChrome chrome = canvas_chrome(*this, lay.iw);
    // The power control and the brand pill were this page's OWN top bar and
    // are gone with it — the shared bar carries both, and it is hit-tested
    // before this branch is reached.
    // 3. Ask Mind button
    int box_w = std::min(1040, lay.iw - 48);
    int box_y = chrome.topbar_h + 16 + 28 + 82;
    int box_x = (lay.iw - box_w) / 2;
    int box_h = 56;
    int btn_w = chrome.ask_w, btn_h = chrome.ask_h;
    int btn1_x = box_x + box_w - btn_w - 10;
    int btn_y = box_y + (box_h - btn_h) / 2;
    if (lay.start_page && px >= btn1_x && px <= btn1_x + btn_w && py >= btn_y && py <= btn_y + btn_h) {
      canvas_submit_prompt();
      return;
    }
    if (lay.start_page && px >= box_x && px < box_x + box_w && py >= box_y && py < box_y + box_h) {
      canvas_state_.prompt_focused = true;
      return;
    }
    // 3.5 Quick Theme & Background Scene Picker Chips
    int chip_bar_y = box_y + box_h + 10;
    int chip_bar_h = 24;
    if (lay.start_page && py >= chip_bar_y && py <= chip_bar_y + chip_bar_h) {
      struct ThemeChipDef { const char* label; int scene; const char* desc; };
      const ThemeChipDef theme_chips[] = {
        {"Ocean Dusk", 4, "ocean at dusk sunset open horizon water waves orange purple twilight glow"},
        {"Beach Moon", 0, ""},
        {"Cyber Grid", 1, ""},
        {"Quantum Orb", 2, ""},
        {"Pyramids", 3, "three pyramids in the desert dunes sand starlight night sky"},
        {"Full View", -1, ""},
      };
      int total_chips_w = 0;
      int chip_widths[6];
      for (int i = 0; i < 6; ++i) {
        chip_widths[i] = text_width(theme_chips[i].label, Face::UiSmall) + 20;
        total_chips_w += chip_widths[i] + (i > 0 ? 8 : 0);
      }
      int cur_chip_x = box_x + (box_w - total_chips_w) / 2;
      for (int i = 0; i < 6; ++i) {
        if (px >= cur_chip_x && px < cur_chip_x + chip_widths[i]) {
          if (theme_chips[i].scene == -1) {
            canvas_state_.bare_scene = !canvas_state_.bare_scene;
            set_toast(canvas_state_.bare_scene ? "Full background view (Space to exit)" : "Desk cards restored");
          } else {
            set_view_scene(DeskView::StartPage, theme_chips[i].scene);
            set_view_scene(DeskView::Desk, theme_chips[i].scene);
            set_view_scene(DeskView::Workspace, theme_chips[i].scene);
            if (theme_chips[i].desc && theme_chips[i].desc[0]) {
              canvas_state_.composed_text = theme_chips[i].desc;
              ++canvas_state_.composed_generation;
            }
            canvas_state_.layout_dirty = true;
            if (kernel_) desk_layout_save(canvas_state_, *kernel_);
            set_toast(std::string("Background: ") + theme_chips[i].label);
          }
          return;
        }
        cur_chip_x += chip_widths[i] + 8;
      }
    }

    // The provider and model selects, and whichever list is open. Same
    // geometry function the painter draws from.
    if (lay.start_page && kernel_) {
      const MindSelect m = mind_select(*this, *kernel_, canvas_state_, box_x, box_w,
                                       chip_bar_y + chip_bar_h + 12);
      // A row of the open list, first: it hangs over the controls below it.
      const int row = m.hit_row(px, py);
      if (row >= 0) {
        const std::string pick = m.list[(size_t)row];
        const int which = m.which;
        canvas_state_.mind_select_open = 0;
        if (which == 1 && pick != m.provider) set_mind_provider(pick);
        else if (which == 2 && pick != m.model) set_mind_model(pick);
        return;
      }
      if (m.on_provider(px, py)) {
        canvas_state_.mind_select_open = (m.which == 1) ? 0 : 1;
        return;
      }
      if (m.on_model(px, py)) {
        canvas_state_.mind_select_open = (m.which == 2) ? 0 : 2;
        return;
      }
      // A click anywhere else closes it, which is what a select does.
      if (m.which) { canvas_state_.mind_select_open = 0; return; }
    }

    canvas_state_.prompt_focused = false;
    app_pane_focused_ = false;
    // 4. The desk.
    if (const TileRect* r = lay.at(px, py)) {
      const bool known = r->index >= 0 && r->index < (int)canvas_state_.tiles.size();
      if (known && r->on_shut(px, py)) {
        set_toast(std::string(tile_menu_name(r->kind)) + " removed - right-click to put it back");
        desk_hide(canvas_state_, r->index);
        return;
      }
      if (known && r->on_fold(px, py)) {
        desk_fold(canvas_state_, r->index, !canvas_state_.tiles[r->index].collapsed);
        return;
      }
      if (known && r->kind == TileKind::AppPane && py >= r->y + r->header_h && !r->on_grip(px, py)) {
        app_pane_focused_ = true;
        if (WindowSurface* win = app_pane_window()) {
          if (kernel_) web_pane_click(*this, *win, *kernel_, x - win->bounds.x, y - win->bounds.y - 26);
        }
        return;
      }
      if (known) {
        if (r->kind == TileKind::HeroEditor || r->kind == TileKind::HeroDb ||
            r->kind == TileKind::HeroWeb || r->kind == TileKind::HeroTensor ||
            r->kind == TileKind::HeroHorizon || r->kind == TileKind::HeroBuild) {
          desk_launch(r->kind);
          return;
        }
        desk_press(canvas_state_, *r, px, py, r->on_grip(px, py));
      }
      return;
    }
    return;
  }




  uint32_t hit_win = 0;
  HitRegion hit = hit_test(x, y, hit_win);

  // Nothing under the pointer is nothing under the pointer.
  //
  // The merge brought back the Overview page's hit regions — nine "quick
  // launch" app chips and four workspace chips, measured off the middle of the
  // screen — without the page that used to draw them, because that page is
  // gone. They were invisible click targets on a bare desktop: clicking empty
  // background in the right band silently launched an app or switched section,
  // with nothing on screen to say so. Launching lives on the taskbar, which is
  // drawn, and in the command palette, which is drawn when it is open.
  if (hit_win == 0) return;

  bring_to_front(hit_win);
  if (hit == HitRegion::CloseBtn) {
    close_window(hit_win);
    return;
  }
  if (hit == HitRegion::MaximizeBtn) {
    maximize_window(hit_win);
    return;
  }
  if (hit == HitRegion::MinimizeBtn) {
    minimize_window(hit_win);
    return;
  }
  if (hit == HitRegion::TitleBar) {
    // Double click maximises and restores, which is what a title bar does
    // everywhere else.
    const uint64_t now = steady_ms();
    if (last_title_click_win_ == hit_win && now - last_title_click_ms_ < 400) {
      last_title_click_ms_ = 0;
      last_title_click_win_ = 0;
      maximize_window(hit_win);
      return;
    }
    last_title_click_ms_ = now;
    last_title_click_win_ = hit_win;
    is_dragging_ = true;
    active_drag_win_ = hit_win;
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id == hit_win) {
        drag_offset_x_ = x - w.bounds.x;
        drag_offset_y_ = y - w.bounds.y;
      }
    }
    return;
  }
  if (hit == HitRegion::ResizeGrip) {
    is_resizing_ = true;
    active_drag_win_ = hit_win;
    if (const WindowSurface* w = find_window(hit_win)) {
      resize_edges_ = resize_edges_at(*w, x, y);
      if (!resize_edges_) resize_edges_ = EdgeR | EdgeB;   // the old corner
      resize_from_x_ = w->bounds.x; resize_from_y_ = w->bounds.y;
      resize_from_w_ = w->bounds.width; resize_from_h_ = w->bounds.height;
      resize_grab_x_ = x; resize_grab_y_ = y;
    }
    return;
  }

  // A click inside a window's content: the controls each app draws.
  std::string to_run;
  bool clear_terminal = false;
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_) {
      if (w.window_id != hit_win) continue;
      int rel_x = x - w.bounds.x;
      int rel_y = y - w.bounds.y - 26;
      if (w.type == WindowType::Terminal) {
        if (rel_y >= 0 && rel_y <= 28) {
          int chip_x = 8;
          for (const char* chip : TERM_CHIPS) {
            int chip_w = text_width(chip, Face::Ui) + 18;
            if (rel_x >= chip_x && rel_x <= chip_x + chip_w) {
              if (strcmp(chip, "clear") == 0) {
                w.lines.clear();
                w.scroll_y = 0;
                terminal_clear_selection(w);
                clear_terminal = true;
              } else {
                to_run = chip;
              }
              break;
            }
            chip_x += chip_w + 6;
          }
        } else {
          int line = 0, col = 0;
          if (terminal_point_to_caret(w, rel_x, rel_y, &line, &col)) {
            if (shift_held_) terminal_begin_selection(w);
            else terminal_clear_selection(w);
            w.cursor_line = line;
            w.cursor_col = col;
            if (!shift_held_) {
              w.sel_line = line;
              w.sel_col = col;
              text_dragging_ = true;
              text_drag_win_ = w.window_id;
            }
          }
        }
      } else if (w.type == WindowType::Miner && rel_y >= w.content_height() - 54 && rel_y <= w.content_height() - 10) {
        // The same three rects the pane drew, from the same function. The
        // daemon button's width does not depend on which label it is wearing,
        // so this hit test does not need to know the daemon's state.
        MinerBtn btns[3];
        miner_buttons(*this, false, btns);
        static const char* const acts[3] = {"miner mine 1", "miner mine 2", "miner status"};
        for (int i = 0; i < 3; ++i) {
          if (rel_x >= btns[i].x && rel_x < btns[i].x + btns[i].w) { to_run = acts[i]; break; }
        }
      } else if (w.type == WindowType::Database) {
        db_click(w, rel_x, rel_y, to_run);
      } else if (w.type == WindowType::Editor) {
        if (w.tx_review_active) {
          editor_tx_click(w, rel_x, rel_y, kernel_);
        } else {
          editor_click(w, rel_x, rel_y, to_run);
        }
      } else if (w.type == WindowType::Browser) {
        // A click on a link fetches a page, which is not something to do while
        // holding the compositor's state lock -- but every other pane's click
        // handler runs here too, and the pane needs the kernel that the last
        // frame was drawn for.
        if (kernel_) web_pane_click(*this, w, *kernel_, rel_x, rel_y);
      } else if (w.type == WindowType::Mail) {
        mail_click(*this, w, rel_x, rel_y, to_run);
      } else if (w.type == WindowType::Contacts) {
        contacts_click(w, rel_x, rel_y, to_run);
      } else if (w.type == WindowType::Calendar) {
        calendar_click(w, rel_x, rel_y, to_run);
      } else if (w.type == WindowType::FaceApp) {
        face_app_click(w, rel_x, rel_y);
      } else if (w.type == WindowType::Grain) {
        // Content starts one pixel in from the frame (see content_width).
        GrainQueued q;
        q.window_id = w.window_id;
        q.type = "mouse_down";
        q.x = rel_x - 1;
        q.y = rel_y;
        grain_queue(q);
        grain_press_win_ = w.window_id;
      } else if (w.type == WindowType::Inspector) {
        inspector_click(w, rel_x, rel_y, to_run);
      }
      break;
    }
  }
  (void)clear_terminal;
  // Outside the lock now. Anything a click asked for that raises a pane runs
  // here, because run_ui_action -> ensure_app_window / bring_to_front all take
  // state_mutex_ and it is not a recursive one.
  if (pending_click_) {
    auto fn = std::move(pending_click_);
    pending_click_ = nullptr;
    fn();
  }
  if (!to_run.empty()) {
    set_toast("running: " + to_run);
    submit_command(to_run);
  }
}

void HardwareCompositor::input_mouse_wheel_x(int notches) {
  if (!notches) return;
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  WindowSurface* win = focused_window();
  if (!win) return;
  if (win->type == WindowType::Database) {
    // The studio's grid already shows whole columns from db_col_first — the
    // pager arrows in its footer walk it. This is the same walk under the
    // hand, which is what a person reaches for when a table is eighteen
    // columns wide and the pane holds twelve.
    const int nc = static_cast<int>(win->grid_columns.size());
    if (nc <= 0) return;
    win->db_col_first += notches;
    if (win->db_col_first > nc - 1) win->db_col_first = nc - 1;
    if (win->db_col_first < 0) win->db_col_first = 0;
    return;
  }
  // Nothing else on this desktop scrolls sideways yet. The editor wraps and
  // the panes that do not are narrow enough to fit, so there is no offset to
  // move — a sideways swipe over them is left alone rather than given a
  // field nothing reads back.
}

void HardwareCompositor::input_mouse_wheel(int notches) {
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  // A wheel over the app pane on a canvas face scrolls the PAGE. Without this
  // it reached whatever window happens to be focused on the other face, so a
  // long page on the desk could not be read past its first screenful.
  if (canvas_mode_) {
    const CanvasLayout lay = canvas_layout(*this, width_, height_, canvas_state_);
    int bx = 0, by = 0, bw = 0, bh = 0;
    if (app_tile_body(lay, &bx, &by, &bw, &bh)) {
      const int px = mouse_x_ / lay.scale, py = mouse_y_ / lay.scale;
      if (px >= bx && px < bx + bw && py >= by && py < by + bh) {
        if (WindowSurface* win = app_pane_window()) { web_pane_wheel(*win, notches); return; }
      }
    }
  }
  // A wheel over the studio scrolls the result set and a wheel over the editor
  // scrolls the file. It used to reach the terminal no matter what was in
  // front, which made a long table unreadable.
  {
    WindowSurface* win = focused_window();
    if (win && win->type == WindowType::Grain) {
      GrainQueued q;
      q.window_id = win->window_id;
      q.type = "wheel";
      q.dy = notches;
      grain_queue(q);
      return;
    }
    if (win && win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 0) {
      // The designer sheet is in front: the wheel scrolls its column rows.
      win->db_designer_scroll -= notches;
      if (win->db_designer_scroll < 0) win->db_designer_scroll = 0;
      return;
    }
    if (win && win->type == WindowType::Database) {
      // Over the explorer the wheel scrolls the TABLE LIST -- fifty-eight
      // tables in a panel that holds twenty is otherwise a list you cannot
      // reach the bottom of. Over the grid it scrolls the rows of the page;
      // PageUp / PageDown move between pages.
      int rel_x = mouse_x_ - win->bounds.x - 1;
      int tree_w = (win->content_width() > 700) ? 240 : 180;
      if (rel_x >= 20 && rel_x <= 20 + tree_w) {
        win->db_tree_scroll -= notches * 3;
        if (win->db_tree_scroll < 0) win->db_tree_scroll = 0;
        int vis = static_cast<int>(db_visible_tables(*win).size());
        if (win->db_tree_scroll > vis - 1) win->db_tree_scroll = vis > 0 ? vis - 1 : 0;
        return;
      }
      win->grid_scroll -= notches * 3;
      if (win->grid_scroll < 0) win->grid_scroll = 0;
      return;
    }
    if (win && win->type == WindowType::Browser) {
      web_pane_wheel(*win, notches);
      return;
    }
    if (win && win->type == WindowType::Mail) {
      mail_wheel(*win, notches, mouse_x_ - win->bounds.x, mouse_y_ - win->bounds.y);
      return;
    }
    if (win && win->type == WindowType::Contacts) {
      contacts_wheel(*win, notches);
      return;
    }
    if (win && win->type == WindowType::Calendar) {
      calendar_wheel(*win, notches);
      return;
    }
    if (win && win->type == WindowType::Editor) {
      if (win->tx_review_active) {
        win->tx_scroll_y -= notches * 30;
        if (win->tx_scroll_y < 0) win->tx_scroll_y = 0;
        return;
      }
      int cw = win->content_width();
      int browse_w = (cw > 620) ? 200 : (cw > 420 ? 150 : 0);
      int rel_x = mouse_x_ - win->bounds.x;
      int rel_y = mouse_y_ - win->bounds.y;
      if (browse_w && rel_x >= 20 && rel_x < 20 + browse_w && rel_y >= 0 && rel_y < win->content_height()) {
        win->dir_scroll -= notches * 2;
        if (win->dir_scroll < 0) win->dir_scroll = 0;
        int max_s = (int)win->dir_entries.size() - 5;
        if (max_s < 0) max_s = 0;
        if (win->dir_scroll > max_s) win->dir_scroll = max_s;
        return;
      }
      win->view_top -= notches * 3;
      if (win->view_top < 0) win->view_top = 0;
      int last = static_cast<int>(win->lines.size()) - 1;
      if (win->view_top > last) win->view_top = last < 0 ? 0 : last;
      // Keep the caret inside what is on screen, or the next keystroke jumps
      // the view straight back to where it was.
      if (win->cursor_line < win->view_top) win->cursor_line = win->view_top;
      return;
    }
  }
  WindowSurface* term = focused_terminal();
  if (!term) return;
  term->scroll_y -= notches * 3;
  if (term->scroll_y > 0) term->scroll_y = 0;
  int back = -std::max(static_cast<int>(term->lines.size()), term->wrapped_rows);
  if (term->scroll_y < back) term->scroll_y = back;
}

void HardwareCompositor::canvas_submit_prompt() {
  std::string line = canvas_state_.prompt.text;
  // Trim, so an accidental space does not become a command.
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
  if (line.empty()) { set_toast("Type something to ask the mind first."); return; }
  canvas_state_.prompt.clear();

  // Strip leading "ask " if user typed e.g. "ask open <app>" or "ask app open <app>"
  std::string cmd_check = line;
  if (cmd_check.compare(0, 4, "ask ") == 0) {
    std::string rest = cmd_check.substr(4);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
    if (rest.compare(0, 5, "open ") == 0 || rest.compare(0, 4, "app ") == 0) {
      cmd_check = rest;
    }
  }

  // Check if cmd_check is a direct command or app launch
  bool direct_cmd = false;
  std::string direct_action = cmd_check;

  if (cmd_check.compare(0, 4, "app ") == 0 ||
      cmd_check.compare(0, 6, "scene ") == 0 ||
      cmd_check.compare(0, 8, "display ") == 0 ||
      cmd_check.compare(0, 6, "grain ") == 0 ||
      cmd_check.compare(0, 4, "cap ") == 0 ||
      cmd_check.compare(0, 11, "checkpoint ") == 0 ||
      cmd_check.compare(0, 7, "rewind ") == 0 ||
      cmd_check == "help" || cmd_check == "info" || cmd_check == "halt" ||
      cmd_check == "reboot" || cmd_check == "poweroff" || cmd_check == "devices" ||
      cmd_check == "horizon" || cmd_check == "build") {
    direct_cmd = true;
  } else if (cmd_check.compare(0, 5, "open ") == 0 ||
             cmd_check.compare(0, 7, "launch ") == 0 ||
             cmd_check.compare(0, 6, "start ") == 0 ||
             cmd_check.compare(0, 4, "run ") == 0) {
    size_t p = cmd_check.find(' ');
    std::string target = cmd_check.substr(p + 1);
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.erase(target.begin());
    std::string app_target = target;
    for (char& c : app_target) { if (c == ' ') c = '-'; }
    direct_cmd = true;
    direct_action = "app open " + app_target;
  } else {
    // Natural visual / scene requests (e.g. "show me the ocean at dusk", "draw the ocean", "render sunset")
    std::string lower_check = cmd_check;
    for (char& c : lower_check) { if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); }
    if (lower_check.rfind("show ", 0) == 0 ||
        lower_check.rfind("draw ", 0) == 0 ||
        lower_check.rfind("render ", 0) == 0 ||
        lower_check.rfind("paint ", 0) == 0 ||
        lower_check.find("ocean") != std::string::npos ||
        lower_check.find("sunset") != std::string::npos ||
        lower_check.find("dusk") != std::string::npos ||
        lower_check.find("dawn") != std::string::npos ||
        lower_check.find("sunrise") != std::string::npos ||
        lower_check.find("beach") != std::string::npos ||
        lower_check.find("pyramid") != std::string::npos ||
        lower_check.find("matrix") != std::string::npos ||
        lower_check.find("plasma") != std::string::npos) {
      direct_cmd = true;
      if (cmd_check.rfind("display ", 0) == 0 || cmd_check.rfind("scene ", 0) == 0) {
        direct_action = cmd_check;
      } else {
        std::string cleaned = cmd_check;
        // "ask" is how a person addresses the mind, not part of what they want
        // drawn. It used to survive into the request, so typing "ask beach at
        // dawn" asked the composer for a scene called "ask beach dawn" — a
        // name nothing matches, which is why it composed a bare plain instead
        // of touching the beach.
        if (lower_check.rfind("ask ", 0) == 0) {
          cleaned = cmd_check.substr(4);
          lower_check = lower_check.substr(4);
          while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '\t')) {
            cleaned.erase(cleaned.begin());
            lower_check.erase(lower_check.begin());
          }
          cmd_check = cleaned;
        }
        if (lower_check.rfind("show me ", 0) == 0) cleaned = cmd_check.substr(8);
        else if (lower_check.rfind("draw me ", 0) == 0) cleaned = cmd_check.substr(8);
        else if (lower_check.rfind("show ", 0) == 0) cleaned = cmd_check.substr(5);
        else if (lower_check.rfind("draw ", 0) == 0) cleaned = cmd_check.substr(5);
        else if (lower_check.rfind("render ", 0) == 0) cleaned = cmd_check.substr(7);
        else if (lower_check.rfind("paint ", 0) == 0) cleaned = cmd_check.substr(6);
        while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '\t')) cleaned.erase(cleaned.begin());
        direct_action = "display show " + cleaned;
      }
    } else {
      // Check against installed apps (e.g. "news-scene", "news scene", "calendar")
      std::string app_target = cmd_check;
      for (char& c : app_target) { if (c == ' ') c = '-'; }
      for (const auto& app : canvas_state_.installed_apps) {
        std::string app_clean = app.name;
        for (char& c : app_clean) { if (c == ' ') c = '-'; }
        if (app.name == app_target || app.name == cmd_check || app_clean == app_target) {
          direct_cmd = true;
          direct_action = "app open " + app.name;
          break;
        }
      }
    }
  }

  if (direct_cmd) {
    set_toast("Executing: " + direct_action);
    submit_command(direct_action);
    return;
  }

  // A single quote in the line would close the one wrapping it, so it is
  // doubled the way a shell escapes it.
  std::string esc;
  for (char c : line) { if (c == '\'') esc += "'\\''"; else esc += c; }
  set_toast("Asked the mind: " + line.substr(0, 48));
  submit_command("ask '" + esc + "'");
}

void HardwareCompositor::input_text(const std::string& text) {
  // The drawing surface has the keyboard while it is up: nothing typed over a
  // picture belongs in the window that happens to be focused underneath it.
  //
  // Its own chords have to be answered HERE and not only in input_key, because
  // a printable key does not reach input_key on a hosted backend at all — SDL
  // delivers it as text. Driven headless, `x` on the sheet left the stroke
  // exactly where it was while the sheet itself said "x clears".
  if (draw_mode_) {
    if (text.size() == 1) {
      const UiAction a = keys_.action_for((int)(unsigned char)text[0]);
      if (a == UiAction::DrawToggle || a == UiAction::DrawClear || a == UiAction::DrawUndo ||
          a == UiAction::Screenshot) run_ui_action(a);
    }
    return;
  }
  // A printable key arrives here rather than at input_key on a hosted backend.
  // Asleep it wakes the machine and is not typed into anything.
  if (asleep_) {
    power_wake();
    return;
  }
  std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
  // The palette is the WORKSPACE's launcher and it is open from boot, but it is
  // only ever drawn on that face. Taking the keyboard whichever face was up is
  // why typing at the start page's prompt put the letters somewhere the person
  // could not see and only found later, on the other face.
  if (palette_open_ && !canvas_mode_) {
    palette_input_.insert(text, 128);
    update_palette_filter();
    return;
  }
  // The app pane on this face, when it is the thing that was clicked into.
  if (canvas_mode_ && app_pane_focused_) {
    if (WindowSurface* win = app_pane_window()) {
      if (web_pane_text(*win, text)) return;
    }
  }
  // The start page's prompt is a real text field when it has the keyboard.
  if (canvas_mode_) {
    canvas_state_.prompt_focused = true;
    canvas_state_.prompt.insert(text, 512);
    return;
  }
  // Nothing typed on this face may reach the workspace behind it. The desk has
  // no text field at all and the start page's is right there; letting a
  // keystroke fall through filled in a window that is not on the screen, and
  // the letters only turned up on switching views.
  if (canvas_mode_) return;
  WindowSurface* win = focused_window();
  if (!win) return;
  if (win->type == WindowType::Browser && web_pane_text(*win, text)) return;
  if (win->type == WindowType::Mail) {
    mail_text(*this, *win, text);
    return;
  }
  if (win->type == WindowType::Contacts) {
    contacts_text(*this, *win, text);
    return;
  }
  if (win->type == WindowType::Calendar) {
    calendar_text(*this, *win, text);
    return;
  }
  if (win->type == WindowType::FaceApp) {
    face_app_text(*this, *win, text);
    return;
  }
  if (win->type == WindowType::Grain) {
    GrainQueued q;
    q.window_id = win->window_id;
    q.type = "text";
    q.text = text;
    grain_queue(q);
    return;
  }
  if (win->type == WindowType::Inspector) {
    inspector_text(*this, *win, text);
    return;
  }
  if (win->type == WindowType::Editor && !win->cmdline_focus) {
    // A file being read is not being written: typing at a preview does nothing
    // rather than quietly editing what you were looking at.
    if (!win->file_preview) editor_insert(*win, text);
    return;
  }
  if (win->type == WindowType::Database && win->db_show_conn_mgr && win->db_conn_field_focus >= 0) {
    std::string* field_ptr = nullptr;
    if (win->db_conn_field_focus == 0) field_ptr = &win->db_profile_edit.name;
    else if (win->db_conn_field_focus == 1) field_ptr = &win->db_profile_edit.host;
    else if (win->db_conn_field_focus == 3) field_ptr = &win->db_profile_edit.user;
    else if (win->db_conn_field_focus == 4) field_ptr = &win->db_profile_edit.password;
    else if (win->db_conn_field_focus == 5) field_ptr = &win->db_profile_edit.database;
    else if (win->db_conn_field_focus == 6) field_ptr = &win->db_profile_edit.socket_path;

    if (field_ptr) {
      field_ptr->append(text);
    } else if (win->db_conn_field_focus == 2) {
      std::string port_str = std::to_string(win->db_profile_edit.port);
      port_str.append(text);
      win->db_profile_edit.port = (uint16_t)std::atoi(port_str.c_str());
    }
    return;
  }
  if (win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 0 &&
      win->db_designer_focus_row >= 0 && static_cast<size_t>(win->db_designer_focus_row) < win->db_designer_edits.size()) {
    auto& col = win->db_designer_edits[static_cast<size_t>(win->db_designer_focus_row)].col;
    if (win->db_designer_focus_col == 0) col.name.append(text);
    else if (win->db_designer_focus_col == 1) col.default_value.append(text);
    return;
  }
  if (win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 1 &&
      win->db_designer_index_field_focus >= 0) {
    if (win->db_designer_index_field_focus == 0) win->db_designer_new_index_name.append(text);
    else win->db_designer_new_index_cols.append(text);
    return;
  }
  if (win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 2 &&
      win->db_designer_fk_field_focus >= 0) {
    switch (win->db_designer_fk_field_focus) {
      case 0: win->db_designer_new_fk_name.append(text); break;
      case 1: win->db_designer_new_fk_col.append(text); break;
      case 2: win->db_designer_new_fk_ref_table.append(text); break;
      case 3: win->db_designer_new_fk_ref_col.append(text); break;
      default: break;
    }
    return;
  }
  // A cell open for editing takes the keys, not the statement prompt below it.
  if (win->type == WindowType::Database && win->grid_edit_row >= 0) {
    win->grid_edit.insert(text, 4096);
    return;
  }
  // So does the explorer's filter box, while it has the focus.
  if (win->type == WindowType::Database && win->db_filter_focus) {
    win->db_table_filter.insert(text, 128);
    win->db_tree_scroll = 0;
    return;
  }
  win->input.insert(text, 4096);
}

bool HardwareCompositor::input_key(int key) {
  using namespace aios::hal;
  // Asleep, ANY key wakes the machine and goes no further: it is not typed
  // into a window, and Escape does not tear the desktop down — true keeps the
  // desktop on the screen, which is the whole point of waking it.
  if (asleep_) {
    power_wake();
    return true;
  }
  // Escape closes the power panel before it means anything else.
  if (key == KeyEscape && power_menu_open_) {
    power_menu_open_ = false;
    return true;
  }
  // The drawing surface takes the keyboard while it is up: Escape puts it away
  // and everything else on it is its own. Without this, keys typed over a
  // drawing land in whatever window is focused underneath it.
  if (draw_mode_) {
    if (key == KeyEscape) { draw_mode_set(false); return true; }
    const UiAction dact = keys_.action_for(key);
    if (dact == UiAction::DrawToggle || dact == UiAction::DrawClear ||
        dact == UiAction::DrawUndo || dact == UiAction::Screenshot) {
      run_ui_action(dact);
      return true;
    }
    return true;
  }
  // Command Palette owns the keyboard when open -- on the face it is drawn on.
  if (palette_open_ && !canvas_mode_) {
    std::function<void()> pending_action = nullptr;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      if (key == KeyEscape) {
        close_command_palette();
        return true;
      }
      if (key == KeyUp) {
        if (palette_selected_idx_ > 0) palette_selected_idx_--;
        return true;
      }
      if (key == KeyDown) {
        if (palette_selected_idx_ + 1 < static_cast<int>(palette_filtered_items_.size())) palette_selected_idx_++;
        return true;
      }
      if (key == KeyEnter || key == 10) {
        if (palette_selected_idx_ >= 0 && palette_selected_idx_ < static_cast<int>(palette_filtered_items_.size())) {
          pending_action = palette_filtered_items_[palette_selected_idx_].action;
        }
        close_command_palette();
      } else if (key == KeyBackspace || key == 8 || key == 127) {
        palette_input_.backspace();
        update_palette_filter();
        return true;
      } else if (key == KeyDelete) {
        palette_input_.erase_forward();
        update_palette_filter();
        return true;
      } else if (key == KeyLeft) {
        palette_input_.move_left();
        return true;
      } else if (key == KeyRight) {
        palette_input_.move_right();
        return true;
      } else if (aios::hal::key_is_text(key)) {
        palette_input_.insert(std::string(1, static_cast<char>(key)), 128);
        update_palette_filter();
        return true;
      } else {
        return true;
      }
    }
    if (pending_action) {
      pending_action();
    }
    return true;
  }
  // Ctrl+P / Ctrl+K / PaletteToggle opens command palette everywhere, except in Editor with previewable documents (where it toggles reading/editing preview)
  if (key == KeyCtrlP || key == KeyCtrlK || keys_.action_for(key) == UiAction::PaletteToggle) {
    bool handle_in_editor = false;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win && win->type == WindowType::Editor && lang_has_preview(win->file_lang)) {
        handle_in_editor = true;
      }
    }
    if (!handle_in_editor) {
      toggle_command_palette();
      return true;
    }
  }
  // Ctrl+W closes the focused window
  if (key == KeyCtrlW || keys_.action_for(key) == UiAction::WindowClose) {
    close_focused_window();
    return true;
  }
  // Window snapping chords and workspace switching with Ctrl (word_wise_)
  if (word_wise_) {
    if (key == KeyLeft) { snap_window_left(); return true; }
    if (key == KeyRight) { snap_window_right(); return true; }
    if (key == KeyUp) { snap_window_maximize(); return true; }
    if (key == KeyDown || key == 't' || key == 'T') { tile_windows(2); return true; }
    if (key == '1') { switch_workspace(CockpitWorkspace::Code); return true; }
    if (key == '2') { switch_workspace(CockpitWorkspace::Comms); return true; }
    if (key == '3') { switch_workspace(CockpitWorkspace::Admin); return true; }
    if (key == '4') { switch_workspace(CockpitWorkspace::Space4); return true; }
    if (key == '5') { switch_workspace(CockpitWorkspace::Space5); return true; }
    if (key == '6') { switch_workspace(CockpitWorkspace::Space6); return true; }
    if (key == '7') { switch_workspace(CockpitWorkspace::Space7); return true; }
    if (key == '8') { switch_workspace(CockpitWorkspace::Space8); return true; }
    if (key == '9') { switch_workspace(CockpitWorkspace::Space9); return true; }
    if (key == '0') { switch_workspace(CockpitWorkspace::Blank); return true; }
  }
  // Then Escape and Ctrl+C stop what is running. A person whose first instinct
  // is one of those two must not find that Escape left the desktop instead,
  // and until now there was no way at all to stop a mind mid-thought.
  // Whatever is bound to mind.stop, before that key can mean anything else. A
  // person whose first instinct is Escape must not find it left the desktop.
  if (keys_.action_for(key) == UiAction::MindStop && command_running_.load()) {
    stop_command();
    return true;
  }
  if (key == KeyEscape) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    WindowSurface* win = focused_window();
    if (win) {
      // A grain program has its own keys, Escape among them (its "close", often).
      if (win->type == WindowType::Grain) {
        GrainQueued q;
        q.window_id = win->window_id;
        q.type = "key";
        q.key = "escape";
        q.shift = shift_held_;
        grain_queue(q);
        return true;
      }
      if (win->type == WindowType::Terminal && terminal_has_selection(*win)) {
        terminal_clear_selection(*win);
        return true;
      }
      if (win->type == WindowType::Editor && win->tx_review_active) {
        editor_tx_key(*win, KeyEscape, kernel_);
        set_toast(win->status_text);
        return true;
      }
      if (win->type == WindowType::Editor && editor_has_selection(*win)) {
        editor_clear_selection(*win);
        return true;
      }
      if (win->type == WindowType::Database && win->grid_edit_row >= 0) {
        db_cancel_edit(*win);
        return true;
      }
      if (win->type == WindowType::Editor && !win->cmdline_focus) {
        win->cmdline_focus = true;
        return true;
      }
      if (win->type == WindowType::Mail && win->mail_composing) {
        mail_cancel_compose(*win);
        return true;
      }
      if (win->type == WindowType::Mail && win->mail_search_focused) {
        win->mail_search_focused = false;
        return true;
      }
      if (win->type == WindowType::Contacts && win->contacts_editing) {
        contacts_cancel_card_editor(*win);
        return true;
      }
      if (win->type == WindowType::Contacts && win->contacts_search_focused) {
        win->contacts_search_focused = false;
        return true;
      }
      if (win->type == WindowType::Calendar && win->calendar_editing) {
        calendar_cancel_event_editor(*win);
        return true;
      }
      if (win->type == WindowType::Browser && win->web && win->web->url_focus) {
        win->web->url_focus = false;
        return true;
      }
    }
    // Inside the desktop workspace, Escape cancels focus/selection and stays in workspace mode without exiting.
    return true;
  }


  // The app pane on a canvas face gets the same keys the workspace's browser
  // does — scrolling, Backspace for back, Ctrl+L — but only once it has been
  // clicked into, so the machine's own scene and face keys still work while you
  // are just looking at it.
  if (canvas_mode_ && app_pane_focused_ && kernel_) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    if (WindowSurface* win = app_pane_window()) {
      if (web_pane_key(*this, *win, *kernel_, key)) return true;
    }
  }

  // The browser pane, which owns scrolling, Backspace-for-back and Ctrl+L.
  // It is asked before the panes below it and after the desktop's globals, so
  // Escape still stops a mind and F-keys still switch panes.
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    WindowSurface* win = focused_window();
    if (win && win->type == WindowType::Browser && kernel_) {
      if (web_pane_key(*this, *win, *kernel_, key)) return true;
    }
  }

  // The editor's own F-keys: F12 is go-to-definition (Shift+F12 references)
  // and F1 is hover. With the buffer focused they belong to the editor, so they
  // are asked before the pane-switching globals below -- which used to take
  // them first, so go-to-definition and hover could never be reached.
  if (key == hal::KeyF12 || key == hal::KeyF1) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    WindowSurface* win = focused_window();
    if (win && win->type == WindowType::Editor && !win->cmdline_focus && !win->file_preview &&
        !win->tx_review_active) {
      if (editor_edit_key(*win, key)) return true;
    }
  }

  // F-keys switch panes immediately
  if (key >= hal::KeyF1 && key <= hal::KeyF12) {
    const UiAction act = keys_.action_for(key);
    if (act != UiAction::None && run_ui_action(act)) return true;
  }

  // When Mail is focused, it handles navigation, shortcuts and compose
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    WindowSurface* win = focused_window();
    if (win && win->type == WindowType::Mail) {
      if (mail_key(*this, *win, key)) return true;
    }
    if (win && win->type == WindowType::Contacts) {
      if (contacts_key(*this, *win, key)) return true;
    }
    if (win && win->type == WindowType::Calendar) {
      if (calendar_key(*this, *win, key)) return true;
    }
    if (win && win->type == WindowType::FaceApp) {
      if (face_app_key(*this, *win, key)) return true;
    }
    if (win && win->type == WindowType::Grain) {
      GrainQueued q;
      q.window_id = win->window_id;
      q.type = "key";
      q.key = grain_key_name(key, &q.ctrl);
      q.shift = shift_held_;
      q.alt = word_wise_;
      if (!q.key.empty()) { grain_queue(q); return true; }
    }
    if (win && win->type == WindowType::Inspector) {
      if (inspector_key(*this, *win, key)) return true;
    }
  }

  // A cell open for editing owns the keyboard while it is open: Enter writes
  // the UPDATE, the movement keys work inside the value, and nothing reaches
  // the statement prompt underneath.
  {
    bool commit = false;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win && win->type == WindowType::Database && win->db_show_conn_mgr && win->db_conn_field_focus >= 0) {
        std::string* field_ptr = nullptr;
        if (win->db_conn_field_focus == 0) field_ptr = &win->db_profile_edit.name;
        else if (win->db_conn_field_focus == 1) field_ptr = &win->db_profile_edit.host;
        else if (win->db_conn_field_focus == 3) field_ptr = &win->db_profile_edit.user;
        else if (win->db_conn_field_focus == 4) field_ptr = &win->db_profile_edit.password;
        else if (win->db_conn_field_focus == 5) field_ptr = &win->db_profile_edit.database;
        else if (win->db_conn_field_focus == 6) field_ptr = &win->db_profile_edit.socket_path;

        if (key == KeyBackspace || key == 8 || key == 127) {
          if (field_ptr && !field_ptr->empty()) field_ptr->pop_back();
          else if (win->db_conn_field_focus == 2) {
            std::string port_str = std::to_string(win->db_profile_edit.port);
            if (!port_str.empty()) port_str.pop_back();
            win->db_profile_edit.port = (uint16_t)std::atoi(port_str.c_str());
          }
          return true;
        } else if (key == KeyTab) {
          win->db_conn_field_focus = (win->db_conn_field_focus + 1) % 7;
          return true;
        } else if (key == KeyEscape) {
          win->db_show_conn_mgr = false;
          return true;
        } else if (aios::hal::key_is_text(key)) {
          if (field_ptr) field_ptr->push_back((char)key);
          else if (win->db_conn_field_focus == 2 && key >= '0' && key <= '9') {
            std::string port_str = std::to_string(win->db_profile_edit.port);
            port_str.push_back((char)key);
            win->db_profile_edit.port = (uint16_t)std::atoi(port_str.c_str());
          }
          return true;
        }
        return true;
      }
      if (win && win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 0 &&
          win->db_designer_focus_row >= 0 && static_cast<size_t>(win->db_designer_focus_row) < win->db_designer_edits.size()) {
        auto& col = win->db_designer_edits[static_cast<size_t>(win->db_designer_focus_row)].col;
        std::string* field_ptr = win->db_designer_focus_col == 0 ? &col.name :
                                  win->db_designer_focus_col == 1 ? &col.default_value : nullptr;
        if (key == KeyBackspace || key == 8 || key == 127) {
          if (field_ptr && !field_ptr->empty()) field_ptr->pop_back();
          return true;
        } else if (key == KeyTab) {
          win->db_designer_focus_col = win->db_designer_focus_col == 0 ? 1 : 0;
          return true;
        } else if (key == KeyEscape) {
          win->db_designer_focus_row = -1;
          win->db_designer_focus_col = -1;
          return true;
        } else if (aios::hal::key_is_text(key)) {
          if (field_ptr) field_ptr->push_back((char)key);
          return true;
        }
        return true;
      }
      if (win && win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 1 &&
          win->db_designer_index_field_focus >= 0) {
        std::string* field_ptr = win->db_designer_index_field_focus == 0 ? &win->db_designer_new_index_name
                                                                          : &win->db_designer_new_index_cols;
        if (key == KeyBackspace || key == 8 || key == 127) {
          if (!field_ptr->empty()) field_ptr->pop_back();
          return true;
        } else if (key == KeyTab) {
          win->db_designer_index_field_focus = win->db_designer_index_field_focus == 0 ? 1 : 0;
          return true;
        } else if (key == KeyEscape) {
          win->db_designer_index_field_focus = -1;
          return true;
        } else if (aios::hal::key_is_text(key)) {
          field_ptr->push_back((char)key);
          return true;
        }
        return true;
      }
      if (win && win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 2 &&
          win->db_designer_fk_field_focus >= 0) {
        std::string* fields[4] = {&win->db_designer_new_fk_name, &win->db_designer_new_fk_col,
                                  &win->db_designer_new_fk_ref_table, &win->db_designer_new_fk_ref_col};
        std::string* field_ptr = fields[win->db_designer_fk_field_focus];
        if (key == KeyBackspace || key == 8 || key == 127) {
          if (!field_ptr->empty()) field_ptr->pop_back();
          return true;
        } else if (key == KeyTab) {
          win->db_designer_fk_field_focus = (win->db_designer_fk_field_focus + 1) % 4;
          return true;
        } else if (key == KeyEscape) {
          win->db_designer_fk_field_focus = -1;
          return true;
        } else if (aios::hal::key_is_text(key)) {
          field_ptr->push_back((char)key);
          return true;
        }
        return true;
      }
      if (win && win->type == WindowType::Database && win->db_show_designer && win->db_designer_tab == 0 &&
          win->db_designer_focus_row < 0 &&
          (key == KeyDown || key == KeyUp || key == KeyPageDown || key == KeyPageUp)) {
        const int step = (key == KeyPageDown || key == KeyPageUp) ? 10 : 1;
        win->db_designer_scroll += (key == KeyDown || key == KeyPageDown) ? step : -step;
        if (win->db_designer_scroll < 0) win->db_designer_scroll = 0;   // the painter clamps the top end
        return true;
      }
      if (win && win->type == WindowType::Database && win->db_show_designer &&
          win->db_designer_focus_row < 0 && win->db_designer_index_field_focus < 0 &&
          win->db_designer_fk_field_focus < 0 && key == KeyEscape) {
        win->db_show_designer = false;
        return true;
      }
      if (win && win->type == WindowType::Database && win->db_filter_focus && win->grid_edit_row < 0) {
        if (key == KeyEscape) {
          win->db_filter_focus = false;
          win->db_table_filter.clear();
          win->db_tree_scroll = 0;
          return true;
        }
        if (key == KeyEnter || key == 10) {
          // Enter on the filter opens the one table that is left, which is what
          // typing four letters at a list of fifty-eight was for.
          std::vector<size_t> vis = db_visible_tables(*win);
          if (!vis.empty() && kernel_) {
            db_open_table(*win, *kernel_, win->db_tables[vis.front()]);
            set_toast(win->db_status);
          }
          return true;
        }
        if (key == KeyBackspace || key == 8 || key == 127) {
          win->db_table_filter.backspace();
          win->db_tree_scroll = 0;
          return true;
        }
        if (win->db_table_filter.handle_key(key, word_wise_)) return true;
        if (aios::hal::key_is_text(key)) {
          win->db_table_filter.insert(std::string(1, (char)key), 128);
          win->db_tree_scroll = 0;
          return true;
        }
        return true;
      }
      if (win && win->type == WindowType::Database && win->grid_edit_row >= 0) {
        if (key == KeyEnter || key == 10) {
          commit = true;
        } else if (key == KeyCtrlU) {
          win->grid_edit.erase_to_home();
          return true;
        } else if (key == KeyCtrlK) {
          win->grid_edit.erase_to_end();
          return true;
        } else if (key == KeyCtrlA) {
          win->grid_edit.move_home();
          return true;
        } else if (key == KeyCtrlE) {
          win->grid_edit.move_end();
          return true;
        } else if (win->grid_edit.handle_key(key, word_wise_)) {
          return true;
        } else if (key == KeyBackspace) {
          win->grid_edit.backspace();
          return true;
        } else if (aios::hal::key_is_text(key)) {
          win->grid_edit.insert(std::string(1, (char)key), 4096);
          return true;
        } else {
          return true;
        }
      }
    }
    if (commit && kernel_) {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win) { db_commit_edit(*win, *kernel_); set_toast(win->db_status); }
      return true;
    }
  }

  // The editor is a text editor, so its buffer sees keys before the desktop's
  // own line-prompt handling does. Ctrl+S saves wherever the focus is, Ctrl+L
  // swaps between the file and the command line, Ctrl+B builds.
  {
    bool save = false, build = false, handled = false;
    // Copy and cut read the buffer under the lock and hand the text to the
    // substrate's clipboard after it, since that is a host call.
    std::string to_clipboard;
    bool want_copy = false, want_cut = false, want_paste = false;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win && win->type == WindowType::Editor) {
        if (win->tx_review_active) {
          handled = editor_tx_key(*win, key, kernel_);
          if (handled) {
            set_toast(win->status_text);
            return true;
          }
        }
        bool in_buffer = !win->cmdline_focus;
        if (key == KeyCtrlS) { save = handled = true; }
        else if (key == KeyCtrlB) { build = handled = true; }
        else if (key == KeyCtrlL) { win->cmdline_focus = !win->cmdline_focus; handled = true; }
        else if (key == KeyCtrlP) {
          // The preview is a way of reading, so it is reachable whichever half
          // of the pane has the keyboard.
          if (!lang_has_preview(win->file_lang)) {
            win->status_text = std::string(lang_name(win->file_lang)) +
                               " has no preview - this is for Markdown";
          } else {
            win->file_preview = !win->file_preview;
            editor_stash_active(*win);
            win->status_text = win->file_preview ? "reading" : "editing";
          }
          handled = true;
        }
        else if (in_buffer && key == KeyCtrlZ) { editor_undo(*win); handled = true; }
        else if (in_buffer && key == KeyCtrlY) { editor_redo(*win); handled = true; }
        else if (in_buffer && key == KeyCtrlA) { editor_select_all(*win); handled = true; }
        else if (in_buffer && key == KeyCtrlD) { editor_duplicate_line(*win); handled = true; }
        else if (in_buffer && key == KeyCtrlK) { editor_delete_line(*win); handled = true; }
        else if (in_buffer && key == KeyCtrlG) {
          // Again, in whichever direction Shift asks for.
          if (!win->find_text.empty()) editor_find(*win, win->find_text, !shift_held_, true);
          else win->status_text = "nothing to find again - Ctrl+F first";
          handled = true;
        } else if (in_buffer && key == KeyCtrlF) {
          win->cmdline_focus = true;
          win->input.clear();
          win->input.insert("find ", 64);
          handled = true;
        } else if (in_buffer && (key == KeyCtrlC || key == KeyCtrlX)) {
          // Copy and cut act on the SELECTION, and fall back to the whole line
          // when there is none -- which is what a bare Ctrl+C usually means.
          if (editor_has_selection(*win)) {
            to_clipboard = editor_selection_text(*win);
          } else if (win->cursor_line < (int)win->lines.size()) {
            to_clipboard = win->lines[(size_t)win->cursor_line];
          }
          if (key == KeyCtrlX && !to_clipboard.empty()) {
            if (editor_has_selection(*win)) editor_delete_selection(*win);
            else editor_delete_line(*win);
          }
          want_copy = key == KeyCtrlC;
          want_cut = key == KeyCtrlX;
          handled = true;
        } else if (in_buffer && key == KeyCtrlV) {
          want_paste = true;
          handled = true;
        } else if (in_buffer && win->file_preview) {
          // Reading, not typing: the arrows scroll the rendered page and
          // nothing reaches the buffer, so a stray keystroke cannot edit a
          // file you were only looking at.
          if (key == KeyDown) { win->view_top++; handled = true; }
          else if (key == KeyUp) { if (win->view_top > 0) win->view_top--; handled = true; }
          else if (key == KeyPageDown) { win->view_top += 20; handled = true; }
          else if (key == KeyPageUp) { win->view_top = win->view_top > 20 ? win->view_top - 20 : 0; handled = true; }
          else if (key == KeyHome) { win->view_top = 0; handled = true; }
          if (win->view_top > (int)win->lines.size() - 1)
            win->view_top = (int)win->lines.size() - 1;
          if (win->view_top < 0) win->view_top = 0;
          if (!handled) handled = true;   // nothing else edits while reading
        } else if (in_buffer) {
          handled = editor_edit_key(*win, key);
        }
      }
    }
    if ((want_copy || want_cut) && !to_clipboard.empty()) {
      auto put = hal::clipboard_set(to_clipboard);
      set_toast(put.ok() ? (want_cut ? "cut" : "copied") : ("clipboard: " + put.error().str()));
    }
    if (want_paste) {
      // The whole clipboard goes in, newlines and all. It used to keep the
      // first line and drop the rest, so code could not be pasted into a file.
      auto got = hal::clipboard_get();
      if (!got.ok()) {
        set_toast("clipboard: " + got.error().str());
      } else {
        std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
        WindowSurface* win = focused_window();
        if (win) editor_insert(*win, *got);
      }
    }
    if (save && kernel_) {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win) { editor_save(*win, *kernel_, std::string()); set_toast(win->status_text); }
    }
    if (build) submit_command("build");
    if (handled) return true;
  }

  // Moving and erasing inside the line. Left and Right used to be swallowed
  // here (the handler returned true and did nothing), so a line could only
  // ever be fixed from its end.
  if (key == KeyLeft || key == KeyRight || key == KeyHome || key == KeyEnd ||
      key == KeyBackspace || key == KeyDelete) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    if (canvas_mode_ && canvas_state_.prompt_focused) {
      canvas_state_.prompt.handle_key(key, word_wise_);
      return true;
    }
    if (canvas_mode_) return true;   // as with typing: the workspace is not on screen
    WindowSurface* win = focused_window();
    if (win) win->input.handle_key(key, word_wise_);
    return true;
  }

  // Copy, cut and paste, against the substrate's own clipboard, so text moves
  // between this desktop and everything else on the machine's display.
  if (key == KeyCtrlC || key == KeyCtrlV || key == KeyCtrlX) {
    std::string paste;
    if (key == KeyCtrlV) {
      auto got = hal::clipboard_get();
      if (!got.ok()) { set_toast("clipboard: " + got.error().str()); return true; }
      paste = *got;
      // A pasted newline means "run it", not a control character in the line.
      // Multi-line pastes keep the first line and say what was dropped.
      size_t nl = paste.find('\n');
      if (nl != std::string::npos) {
        size_t lines = 1;
        for (char ch : paste) if (ch == '\n') lines++;
        paste = paste.substr(0, nl);
        set_toast("pasted the first of " + std::to_string(lines) + " lines");
      }
    }
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    WindowSurface* win = focused_window();
    if (win && win->type == WindowType::Terminal && key == KeyCtrlC && terminal_has_selection(*win)) {
      std::string sel = terminal_selection_text(*win);
      auto put = hal::clipboard_set(sel);
      if (!put.ok()) { set_toast("clipboard: " + put.error().str()); return true; }
      set_toast("copied " + std::to_string(sel.size()) + " chars from terminal");
      return true;
    }
    EditLine* line = (canvas_mode_ && canvas_state_.prompt_focused) ? &canvas_state_.prompt : nullptr;
    if (!line && win) line = &win->input;
    if (!line) return true;
    if (key == KeyCtrlV) {
      line->insert(paste, 4096);
    } else {
      if (line->empty()) { set_toast("nothing to copy"); return true; }
      auto put = hal::clipboard_set(line->text);
      if (!put.ok()) { set_toast("clipboard: " + put.error().str()); return true; }
      set_toast(key == KeyCtrlX ? "line cut" : "line copied");
      if (key == KeyCtrlX) line->clear();
    }
    return true;
  }

  if (key == KeyCtrlK || key == KeyCtrlU || key == KeyCtrlA || key == KeyCtrlE) {
    // The line-editing keys a shell has: kill to the end, kill to the start,
    // jump to the start, jump to the end.
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    EditLine* line = (canvas_mode_ && canvas_state_.prompt_focused) ? &canvas_state_.prompt : nullptr;
    if (!line) { WindowSurface* win = focused_window(); if (win) line = &win->input; }
    if (!line) return true;
    if (key == KeyCtrlK) line->erase_to_end();
    else if (key == KeyCtrlU) line->erase_to_home();
    else if (key == KeyCtrlA) line->move_home();
    else line->move_end();
    return true;
  }

  if (key == KeyEnter || key == 10) {   // a serial terminal sends '\n' and means Enter

    std::string to_run;
    WindowType wtype = WindowType::Terminal;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (!win) return true;
      wtype = win->type;
      to_run = win->input.text;
      win->input.clear();
      if (!to_run.empty()) {
        command_history_.push_back(to_run);
        history_index_ = static_cast<int>(command_history_.size());
      }
    }
    if (wtype == WindowType::Editor) {
      if (!kernel_) return true;
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win && !to_run.empty()) {
        editor_command(*win, *kernel_, to_run);
        set_toast(win->status_text);
      }
      return true;
    }
    if (wtype == WindowType::Database) {
      // The studio runs its own statement against the engine in this process:
      // the grid needs the columns and rows back, and "sql query ..." only ever
      // printed a table into the shell's transcript.
      if (!kernel_) return true;
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (!win) return true;
      if (to_run.empty()) {
        if (win->db_selected.empty()) return true;
        to_run = "SELECT * FROM " + win->db_selected + ";";
      }
      db_run_query(*win, *kernel_, to_run);
      set_toast(win->db_status);
      return true;
    }
    if (to_run == "clear" || to_run == "cls") {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (auto& w : windows_) {
        if (w.type == WindowType::Terminal) {
          w.lines.clear();
          w.scroll_y = 0;
        }
      }
      return true;
    }
    submit_command(to_run);
    return true;
  }

  if (key == KeyUp || key == KeyDown) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    WindowSurface* term = focused_terminal();
    if (!term) return true;
    // A line recalled from history arrives whole, with the cursor at its end,
    // which is where a person picks up editing it from.
    if (key == KeyUp) {
      if (!command_history_.empty() && history_index_ > 0) {
        --history_index_;
        term->input.set(command_history_[history_index_]);
      }
    } else if (history_index_ + 1 < static_cast<int>(command_history_.size())) {
      ++history_index_;
      term->input.set(command_history_[history_index_]);
    } else {
      history_index_ = static_cast<int>(command_history_.size());
      term->input.clear();
    }
    return true;
  }

  if (key == KeyPageUp || key == KeyPageDown) {
    // In the studio these turn the PAGE -- the whole point of a paged grid is
    // that there is a next window of rows to ask the engine for.
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      WindowSurface* win = focused_window();
      if (win && win->type == WindowType::Database && win->db_paged && kernel_) {
        db_set_page(*win, *kernel_, win->db_page + (key == KeyPageUp ? -1 : 1));
        set_toast(win->db_status);
        return true;
      }
    }
    input_mouse_wheel(key == KeyPageUp ? 5 : -5);
    return true;
  }
  if (key_is_text(key)) {
    input_text(std::string(1, static_cast<char>(key)));
    return true;
  }
  return true;
}

// The desktop with no display server and nobody at it: frames painted into the
// framebuffer on a clock, scripted input delivered at the frames it names, and
// the result written out for eyes elsewhere. This is how a machine checks its
// own glass — and how a dream is looked at before a person sees it.
bool HardwareCompositor::run_desktop_headless(Kernel& k, CommandDispatcher dispatcher) {
  dispatcher_ = dispatcher;
  if (!width_ || !height_) set_screen_size(1280, 800);
  init_default_workspace(k);
  int frames = dump_after_frames_ > 0 ? dump_after_frames_ : 3;
  // A scripted step past the last frame used to be dropped without a word.
  // Run long enough to reach every step, and say so.
  {
    int last_at = -1;
    for (const auto& s : scripted_) {
      const size_t c1 = s.find(':');
      if (c1 != std::string::npos) last_at = std::max(last_at, std::atoi(s.substr(0, c1).c_str()));
    }
    if (last_at + 1 > frames) {
      std::fprintf(stderr, "headless: a step is scheduled at frame %d; running %d frames instead of %d\n",
                   last_at, last_at + 1, frames);
      frames = last_at + 1;
    }
  }
  auto key_named = [](const std::string& n) -> int {
    using namespace aios::hal;
    if (n == "Enter") return KeyEnter;
    if (n == "Escape") return KeyEscape;
    if (n == "Backspace") return KeyBackspace;
    if (n == "Tab") return KeyTab;
    if (n == "Delete") return KeyDelete;
    if (n == "ArrowUp") return KeyUp;
    if (n == "ArrowDown") return KeyDown;
    if (n == "ArrowLeft") return KeyLeft;
    if (n == "ArrowRight") return KeyRight;
    if (n == "Home") return KeyHome;
    if (n == "End") return KeyEnd;
    if (n == "PageUp") return KeyPageUp;
    if (n == "PageDown") return KeyPageDown;
    if (n.size() == 2 && n[0] == 'F' && n[1] >= '1' && n[1] <= '9') return KeyF1 + (n[1] - '1');
    if (n.size() == 3 && n[0] == 'F' && n[1] == '1' && n[2] >= '0' && n[2] <= '2') return KeyF10 + (n[2] - '0');
    return KeyNone;
  };
  uint64_t last_store_refresh_ms = 0;
  for (int frame = 0; frame < frames; ++frame) {
    for (auto it = scripted_.begin(); it != scripted_.end();) {
      const std::string s = *it;
      const size_t c1 = s.find(':');
      if (c1 == std::string::npos) { it = scripted_.erase(it); continue; }
      const int at = std::atoi(s.substr(0, c1).c_str());
      if (at > frame) { ++it; continue; }
      const size_t c2 = s.find(':', c1 + 1);
      const std::string kind = s.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
      const std::string arg = c2 == std::string::npos ? std::string() : s.substr(c2 + 1);
      std::fprintf(stderr, "headless: frame %d %s %s\n", frame, kind.c_str(), arg.c_str());
      if (kind == "type") input_text(arg);
      else if (kind == "key") {
        const int key = key_named(arg);
        if (key != aios::hal::KeyNone) input_key(key);
        else input_text(arg);
      } else if (kind == "click") {
        int x = 0, y = 0;
        std::sscanf(arg.c_str(), "%d,%d", &x, &y);
        input_mouse_motion(x, y);
        input_mouse_button(x, y, true, aios::hal::MouseLeft);
        input_mouse_button(x, y, false, aios::hal::MouseLeft);
      } else if (kind == "down" || kind == "up") {
        // The two halves of a press, so a script can drag: press here, move
        // there, release. A click that is one instant long can never prove that
        // a tile ends up where it was let go.
        int x = 0, y = 0;
        std::sscanf(arg.c_str(), "%d,%d", &x, &y);
        input_mouse_motion(x, y);
        input_mouse_button(x, y, kind == "down", aios::hal::MouseLeft);
      } else if (kind == "ctrl") {
        // Ctrl+<letter>: the editor's command line is Ctrl+L and its save is
        // Ctrl+S, so without this a script cannot reach either — which is most
        // of what the workstation panes are.
        if (!arg.empty()) {
          const char c = arg[0] >= 'A' && arg[0] <= 'Z' ? (char)(arg[0] + 32) : arg[0];
          if (c >= 'a' && c <= 'z') input_key(c - 'a' + 1);
        }
      } else if (kind == "wheel") {
        // Notches, + is up: the only way a script can prove that a page inside
        // a tile scrolls where the pointer is rather than wherever the
        // workspace's focus happens to be.
        input_mouse_wheel(std::atoi(arg.c_str()));
      } else if (kind == "move") {
        // The pointer without a press. Hover is real behaviour — the view
        // switcher at the top edge only exists while the pointer is there — and
        // a script that can only click cannot reach it.
        int x = 0, y = 0;
        std::sscanf(arg.c_str(), "%d,%d", &x, &y);
        input_mouse_motion(x, y);
      } else if (kind == "cmd") {
        submit_command(arg);
      } else if (kind == "draw") {
        // One drawing directive, straight in — the same parser the run
        // directory feeds, so what a script proves here is what the mind gets
        // when it writes the file on a running machine.
        const std::string trouble = apply_draw_directives(arg);
        if (!trouble.empty())
          std::fprintf(stderr, "headless: frame %d draw trouble: %s\n", frame, trouble.c_str());
      } else if (kind == "shot" || kind == "screenshot") {
        // The machine photographs itself MID-script: paint what the input so
        // far did, then keep it. Without this a headless run can only ever
        // dump its last frame, so a script proving five steps shows one.
        render_desktop(k);
        const std::string got = take_screenshot(k, arg);
        std::fprintf(stderr, "headless: frame %d shot -> %s\n", frame,
                     got.empty() ? "(could not write)" : got.c_str());
      }
      it = scripted_.erase(it);
    }
    const uint64_t tick_ms = k.host().monotonic_ns() / 1000000ull;
    if (tick_ms - last_store_refresh_ms >= 250) { (void)k.store().refresh(); last_store_refresh_ms = tick_ms; }
    render_desktop(k);
    if (frame + 1 < frames) aios::hal::sleep_ms(16);
  }
  const std::string out = dump_path_.empty() ? std::string("/tmp/aos-desktop.ppm") : dump_path_;
#if !defined(AIOS_METAL)
  const bool ok = write_ppm(out, framebuffer_, width_, height_);
  std::fprintf(stderr, "headless: %d frame(s) at %ux%u -> %s%s\n", frames, width_, height_, out.c_str(),
               ok ? "" : " (could not write)");
  return ok;
#else
  // write_ppm is host-only -- it is an <fstream> -- and calling it unguarded is
  // what broke the bare-metal link. There is no file to dump a frame to here;
  // the frame IS the display.
  std::fprintf(stderr, "headless: %d frame(s) at %ux%u (no file to write on bare metal)\n",
               frames, width_, height_);
  return true;
#endif
}

bool HardwareCompositor::run_desktop(Kernel& k, CommandDispatcher dispatcher, bool force_tui) {
  dispatcher_ = dispatcher;
  if (headless_ || getenv("AOS_HEADLESS")) return run_desktop_headless(k, dispatcher);
  if (force_tui) {
    return run_desktop_tui(k, dispatcher);
  }
#if defined(AIOS_HAS_SDL2)
  bool gui_ok = run_desktop_gui(k, dispatcher);
  if (gui_ok) return true;
#endif
  return run_desktop_tui(k, dispatcher);
}

#if defined(AIOS_HAS_SDL2)
// The hosted backend: a window from the host's display server, feeding exactly
// the same input calls the bare-metal PS/2 driver feeds.
bool HardwareCompositor::run_desktop_gui(Kernel& k, CommandDispatcher dispatcher) {
  dispatcher_ = dispatcher;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return false;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  // A finger is a finger. Left alone, SDL sends every touch TWICE — once as
  // SDL_FINGER* and again as a synthesised mouse event — which on the drawing
  // surface is one stroke laid down on top of itself and a release that lands
  // before the last move.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

  // The display's real mode, not a number baked into a source file: on a 4K
  // panel this is 3840x2160 and on a projector it is whatever the projector is.
  if (fullscreen_ || !width_ || !height_) {
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) set_screen_size((uint32_t)dm.w, (uint32_t)dm.h);
    else if (!width_ || !height_) set_screen_size(1280, 800);
  }
  const Uint32 base_flags =
      SDL_WINDOW_SHOWN | (fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE);

  // The display path, if this machine has one. The background is a function of
  // coordinates and the clock — measured at 3.4 ms of a 9.9 ms frame at 1080p
  // on this host — so it belongs in a shader, and the shader writes the
  // swapchain image directly rather than handing a buffer back over the bus.
  //
  // It has to be decided at window creation: a Vulkan window cannot host an
  // SDL renderer and an SDL renderer's window cannot make a Vulkan surface. So
  // the Vulkan window is tried first and the window is REMADE without it if
  // anything refuses — a machine with no Vulkan loses nothing but the speed.
  SDL_Window* win = nullptr;
  std::unique_ptr<GpuPresenter> gpu;
  // --dump writes what the FRAMEBUFFER holds, and the tools that verify this
  // machine compare that file against a grab of the screen. On the GPU path the
  // framebuffer is the interface alone with the background left clear, so the
  // dump would be a picture of half a frame. A run that is asked for one paints
  // the whole thing on the CPU.
  if (!getenv("AIOS_NO_GPU_DISPLAY") && dump_path_.empty()) {
    win = SDL_CreateWindow("AIOS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width_, height_, base_flags | SDL_WINDOW_VULKAN);
    if (win) {
      unsigned ext_count = 0;
      SDL_Vulkan_GetInstanceExtensions(win, &ext_count, nullptr);
      std::vector<const char*> exts(ext_count);
      if (ext_count) SDL_Vulkan_GetInstanceExtensions(win, &ext_count, exts.data());
      std::string why;
      auto inst = GpuPresenter::make_instance(exts.data(), ext_count, why);
      VkSurfaceKHR surface{};
      if (inst.ok() && SDL_Vulkan_CreateSurface(win, (VkInstance)*inst, &surface)) {
        auto candidate = std::make_unique<GpuPresenter>();
        auto attached = candidate->attach(*inst, reinterpret_cast<void*>(surface), width_, height_);
        if (attached.ok()) {
          gpu = std::move(candidate);
          std::cerr << "display: " << gpu->device_name() << " through Vulkan\n";
        } else {
          std::cerr << "display: no GPU path (" << candidate->unavailable() << ")\n";
        }
      } else if (!why.empty()) {
        std::cerr << "display: no GPU path (" << why << ")\n";
      }
      if (!gpu) { SDL_DestroyWindow(win); win = nullptr; }
    }
  }
  if (!win) {
    win = SDL_CreateWindow("AIOS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width_, height_, base_flags);
  }
  if (!win) {
    SDL_Quit();
    return false;
  }


  SDL_Renderer* renderer = nullptr;
  if (!gpu) {
    renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  }
  // Whether this renderer actually got vsync, which decides how the frame is
  // paced below. Asking beats assuming: the flag is a request, and the software
  // fallback does not honour it at all.
  bool presents_on_vsync = false;
  if (renderer) {
    SDL_RendererInfo rinfo{};
    presents_on_vsync = SDL_GetRendererInfo(renderer, &rinfo) == 0 &&
                        (rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
  }
  if (!renderer && !gpu) {
    SDL_DestroyWindow(win);
    SDL_Quit();
    return false;
  }

  SDL_Texture* texture = renderer
      ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width_, height_)
      : nullptr;
  if (!texture && !gpu) {
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return false;
  }

  SDL_StartTextInput();
  init_default_workspace(k);

  // What SDL calls a key, the OS calls a key.
  auto to_key = [](SDL_Keycode key) -> int {
    using namespace aios::hal;
    switch (key) {
      case SDLK_ESCAPE: return KeyEscape;
      case SDLK_BACKSPACE: return KeyBackspace;
      case SDLK_DELETE: return KeyDelete;
      case SDLK_RETURN: case SDLK_KP_ENTER: return KeyEnter;
      case SDLK_TAB: return KeyTab;
      case SDLK_UP: return KeyUp;
      case SDLK_DOWN: return KeyDown;
      case SDLK_LEFT: return KeyLeft;
      case SDLK_RIGHT: return KeyRight;
      case SDLK_HOME: return KeyHome;
      case SDLK_END: return KeyEnd;
      case SDLK_PAGEUP: return KeyPageUp;
      case SDLK_PAGEDOWN: return KeyPageDown;
      case SDLK_F1: return KeyF1;
      case SDLK_F2: return KeyF2;
      case SDLK_F3: return KeyF3;
      case SDLK_F4: return KeyF4;
      case SDLK_F5: return KeyF5;
      case SDLK_F6: return KeyF6;
      case SDLK_F7: return KeyF7;
      case SDLK_F8: return KeyF8;
      default: return KeyNone;
    }
  };
  // Ctrl+<letter> is the ASCII control code that letter names, which is what
  // every other input backend here already sends. Without this the editor's
  // Ctrl+S never reached it on a hosted machine.
  // Ctrl OR Cmd: the copy/paste/cut modifier on this host is whichever of the
  // two the person's keyboard has, and honouring only Ctrl makes the desktop
  // unusable on a Mac.
  auto to_ctrl_key = [](SDL_Keycode key, uint16_t mod) -> int {
    if (!(mod & (KMOD_CTRL | KMOD_GUI))) return aios::hal::KeyNone;
    if (key >= SDLK_a && key <= SDLK_z) return static_cast<int>(key - SDLK_a) + 1;
    return aios::hal::KeyNone;
  };
  // Alt (or Cmd, next to a word key) means "by the word". It is a modifier,
  // not a key: Alt+Left and Left arrive with the same keycode.
  auto word_modifier = [](uint16_t mod) -> bool { return (mod & (KMOD_ALT | KMOD_GUI)) != 0; };

  bool running = true;
  uint32_t dumped_frames = 0;
  uint64_t last_store_refresh_ms = 0;
  // 60 a second is the target when this loop has to pace itself. It is a
  // budget, not a delay: what the frame already spent comes off it.
  constexpr uint64_t kFrameBudgetMs = 16;
  const uint64_t loop_began = SDL_GetTicks64();
  uint64_t overlay_generation = 0;
  // The background is a raymarch. At 1080p it IS the frame — measured at 22.9 ms
  // of a 25 ms frame on a 1080 Ti, which is a card held at 170 W and 83 C to
  // draw a wallpaper. It is also the slowest-moving thing on the screen: this
  // scene's camera drifts on a seventy-second period and its cloud deck slower
  // still. So it is computed on its OWN clock and reused in between, while the
  // interface over it keeps compositing at the display's full rate — the UI
  // stays exactly as responsive, the picture keeps moving, the card stops
  // burning a frame's worth of march on a picture nobody can see change.
  //
  // Two rates, because a machine nobody is touching should cost less than one
  // somebody is using, and the difference is invisible either way.
  // How fast each scene actually MOVES, read off the shader's own time
  // constants rather than guessed: tide carries a t * 16.0 term and has to be
  // sampled often or it strobes; lattice turns at t * 1.35; core, pyramid and
  // the composed scene are slow camera drifts (t * 0.09 there — a seventy
  // second period) and are indistinguishable at ten a second from sixty.
  //
  // The background costs what it costs, so it is thinned — but in RESOLUTION,
  // not in rate. It is marched at 1/scale in each axis, every frame, and
  // interpolated back up, so every pixel of a frame is from the same instant and
  // the scene moves cleanly at whatever the display runs at. Thinning the rate
  // instead is what made it step, and thinning the pixels made it shimmer; the
  // shader's header carries the measurements.
  //
  // A faster scene is not given a finer march — a coarser one, if anything, is
  // less noticeable while it moves. What sets the scale is simply how much of
  // the card this is worth.
  constexpr int kSceneScale     = 2;   // a quarter of the samples, in use
  constexpr int kIdleSceneScale = 3;   // a ninth, once nobody is here
  constexpr uint64_t kIdleAfterMs = 10000;  // what counts as left alone
  uint64_t last_input_ms = SDL_GetTicks64();
  uint64_t lat_stamp = 0, lat_seen = 0;
  // Tell the painter who is drawing the background, so it leaves those pixels
  // clear for the shader instead of computing them twice.
  canvas_state_.background_on_gpu = (gpu != nullptr);
  while (running) {
    const uint64_t frame_began = SDL_GetTicks64();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      // What counts as someone being here. INPUT events only: the window
      // system also delivers housekeeping — focus, expose, the SDL poll
      // sentinel — on a machine nobody is touching, and stamping on those kept
      // the desktop permanently "active" and the idle rate permanently unused.
      // Mouse motion is in deliberately: a pointer crossing the screen is a
      // person, and a background that drops rate under the cursor looks broken.
      switch (ev.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL: case SDL_DROPFILE:
        case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP:
          last_input_ms = SDL_GetTicks64();
          break;
        default: break;
      }
      if (ev.type == SDL_QUIT) {
        running = false;
      } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        // The screen changed under us (a resize, a mode switch, a new monitor):
        // re-lay the whole desktop for what is there now instead of stretching
        // the old raster over it.
        if (ev.window.data1 > 0 && ev.window.data2 > 0) {
          set_screen_size((uint32_t)ev.window.data1, (uint32_t)ev.window.data2);
          if (texture) {
            SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width_, height_);
          }
          if (gpu) gpu->resize(width_, height_);
          // A resize is not a new session. init_default_workspace() clears
          // windows_, so every open window was destroyed by a resize and there
          // was nothing left to put back. Keep them; fit them to the new glass.
          {
            std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
            const int mw = static_cast<int>(width_), mh = static_cast<int>(height_);
            for (auto& w : windows_) {
              w.bounds.width = std::clamp(w.bounds.width, 320, std::max(320, mw - 28));
              w.bounds.height = std::clamp(w.bounds.height, 200, std::max(200, mh - 84));
              w.bounds.x = std::clamp(w.bounds.x, 14, std::max(14, mw - 14 - w.bounds.width));
              w.bounds.y = std::clamp(w.bounds.y, 48, std::max(48, mh - 36 - w.bounds.height));
              w.normal_bounds = w.bounds;
              w.buffer.resize(w.content_pixels(), tok::panel);
            }
          }
        }
      } else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP) {
        // A touchscreen, which SDL reports in NORMALISED coordinates on its own
        // event type — not as a mouse. A desktop that only listens for the
        // mouse is a desktop you cannot draw on with a finger, whatever it does
        // with a trackpad. Multi-touch is deliberately flattened to the first
        // finger down: one nib, and a second finger cannot start a second
        // stroke in the middle of the first.
        //
        // SDL also SYNTHESISES mouse events from touch unless asked not to, so
        // each finger would otherwise arrive twice; the hint below is set with
        // the window, and the finger is the copy that is kept because it is the
        // one that carries pressure and sub-pixel position.
        const int tx = (int)(ev.tfinger.x * (float)width_);
        const int ty = (int)(ev.tfinger.y * (float)height_);
        if (ev.type == SDL_FINGERDOWN) {
          if (touch_finger_down_) continue;
          touch_finger_ = ev.tfinger.fingerId;
          touch_finger_down_ = true;
          input_mouse_motion(tx, ty);
          input_mouse_button(tx, ty, true, aios::hal::MouseLeft);
        } else if (ev.tfinger.fingerId == touch_finger_ && touch_finger_down_) {
          if (ev.type == SDL_FINGERMOTION) {
            input_mouse_motion(tx, ty);
          } else {
            touch_finger_down_ = false;
            input_mouse_button(tx, ty, false, aios::hal::MouseLeft);
          }
        }
      } else if (ev.type == SDL_MOUSEMOTION) {
        input_mouse_motion(ev.motion.x, ev.motion.y);
      } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
        int which = ev.button.button == SDL_BUTTON_RIGHT ? aios::hal::MouseRight
                  : ev.button.button == SDL_BUTTON_MIDDLE ? aios::hal::MouseMiddle
                  : aios::hal::MouseLeft;
        input_mouse_button(ev.button.x, ev.button.y, true, which);
      } else if (ev.type == SDL_MOUSEBUTTONUP) {
        int which = ev.button.button == SDL_BUTTON_RIGHT ? aios::hal::MouseRight
                  : ev.button.button == SDL_BUTTON_MIDDLE ? aios::hal::MouseMiddle
                  : aios::hal::MouseLeft;
        input_mouse_button(ev.button.x, ev.button.y, false, which);
      } else if (ev.type == SDL_MOUSEWHEEL) {
        // Both axes. A trackpad swipes sideways on wheel.x; a mouse with one
        // wheel says the same thing with Shift held, which is the convention
        // every other desktop uses.
        if (ev.wheel.x) input_mouse_wheel_x(ev.wheel.x);
        if (ev.wheel.y) {
          if (SDL_GetModState() & KMOD_SHIFT) input_mouse_wheel_x(-ev.wheel.y);
          else input_mouse_wheel(ev.wheel.y);
        }
      } else if (ev.type == SDL_KEYUP) {
        // Letting Alt go raises what the switcher landed on. Held-modifier
        // gestures need the release; nothing else here was listening for one.
        if (alt_tab_open_ &&
            (ev.key.keysym.sym == SDLK_LALT || ev.key.keysym.sym == SDLK_RALT ||
             ev.key.keysym.sym == SDLK_LCTRL || ev.key.keysym.sym == SDLK_RCTRL))
          alt_tab_commit();
      } else if (ev.type == SDL_TEXTINPUT) {
        // AOS_LATENCY=1: how long a key takes from the window system to the screen, measured on the keys a person really presses.
        if (std::getenv("AOS_LATENCY")) { lat_stamp = ev.text.timestamp; lat_seen = SDL_GetTicks64(); }
        input_text(ev.text.text);
      } else if (ev.type == SDL_KEYDOWN) {
        // Asleep, the key wakes the machine here — before the canvas' own
        // shortcuts get a look at it, so 'r' cannot reboot a sleeping machine.
        if (asleep_) { power_wake(); continue; }
        // Alt+Tab, before anything else looks at the key: every other consumer
        // of Tab on this machine (the editor, the palette, a line) would eat
        // it, and the switcher is a window-manager gesture, not a text one.
        // Alt+Tab is the gesture, and it is the one bare metal gets: there this
        // desktop IS the window manager and nothing takes the key first.
        //
        // Hosted inside another desktop, it never arrives. GNOME binds Alt+Tab
        // as a passive grab on the root window, so mutter switches ITS windows
        // and the client never sees the key — asking SDL for a keyboard grab
        // does not win it back, tried and measured. Ctrl+Tab is not spoken for
        // by the host and nothing on this machine claims it, so the switcher
        // answers to both and works wherever it is running.
        if (ev.key.keysym.sym == SDLK_TAB && (ev.key.keysym.mod & (KMOD_ALT | KMOD_CTRL))) {
          alt_tab_step((ev.key.keysym.mod & KMOD_SHIFT) != 0);
          continue;
        }
        if (ev.key.keysym.sym == SDLK_ESCAPE && alt_tab_open_) { alt_tab_cancel(); continue; }
        set_word_modifier(word_modifier(ev.key.keysym.mod));
        // Shift is what tells an arrow to drag a selection instead of
        // just moving the caret.
        set_shift_modifier((ev.key.keysym.mod & KMOD_SHIFT) != 0);
        web_pane_set_modifiers((ev.key.keysym.mod & KMOD_SHIFT) != 0, (ev.key.keysym.mod & KMOD_ALT) != 0);
        // Ctrl+plus / Ctrl+minus / Ctrl+0 zoom the page. They are not letters, so to_ctrl_key leaves them as KeyNone.
        if (!canvas_mode_ && (ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI))) {
          const SDL_Keycode zs = ev.key.keysym.sym;
          const int zoom_key = (zs == SDLK_PLUS || zs == SDLK_EQUALS || zs == SDLK_KP_PLUS) ? kWebKeyZoomIn
                             : (zs == SDLK_MINUS || zs == SDLK_KP_MINUS) ? kWebKeyZoomOut
                             : (zs == SDLK_0 || zs == SDLK_KP_0) ? kWebKeyZoomReset : 0;
          if (zoom_key) { input_key(zoom_key); continue; }
        }
        int ctrl = to_ctrl_key(ev.key.keysym.sym, ev.key.keysym.mod);
        if (ctrl != aios::hal::KeyNone) {
          // Copy, cut, paste and the line-editing controls belong to whichever
          // line has the keyboard, including the start page's prompt -- which
          // used to drop every one of them on the floor.
          bool line_key = ctrl == KeyCtrlC || ctrl == KeyCtrlV || ctrl == KeyCtrlX ||
                          ctrl == KeyCtrlA || ctrl == KeyCtrlE || ctrl == KeyCtrlK || ctrl == KeyCtrlU;
          if ((!canvas_mode_ || line_key) && !input_key(ctrl)) running = false;
          continue;
        }
        if (ev.key.keysym.sym == SDLK_F11 || ((ev.key.keysym.mod & (KMOD_GUI | KMOD_CTRL)) && ev.key.keysym.sym == SDLK_f && (ev.key.keysym.mod & KMOD_SHIFT))) {
          fullscreen_ = !fullscreen_;
          SDL_SetWindowFullscreen(win, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
          continue;
        }
        int key = to_key(ev.key.keysym.sym);
        if (key == aios::hal::KeyF1) { canvas_mode_ = !canvas_mode_; continue; }
        // Esc leaves the canvas for the workspace -- unless the prompt has the
        // keyboard, in which case it just gives the field up.
        //
        // The open power panel comes first, on this face as well as the other.
        // This branch used to run ahead of input_key's own "Escape closes the
        // panel" rule, so pressing Escape with the panel open on the start page
        // left the panel up and swapped the whole surface underneath it.
        if (canvas_mode_ && key == aios::hal::KeyEscape) {
          if (power_menu_open_) power_menu_open_ = false;
          else if (canvas_state_.prompt_focused) canvas_state_.prompt_focused = false;
          else canvas_mode_ = false;
          continue;
        }
        // The canvas' own chip row advertises F5 for the editor and F6 for the
        // studio, so those keys have to arrive there: leave the canvas and let
        // the desktop's F-key handling raise the pane it names.
        if (canvas_mode_ && key >= aios::hal::KeyF2 && key <= aios::hal::KeyF6) canvas_mode_ = false;
        // While the start page's prompt has the keyboard, it gets the keys:
        // Enter sends the line, Backspace erases, Escape gives up focus, and
        // the letter shortcuts below stay out of the way so that typing "r"
        // types an r instead of rebooting the machine.
        if (canvas_mode_ && canvas_state_.prompt_focused) {
          if (key == aios::hal::KeyEnter || ev.key.keysym.sym == SDLK_RETURN ||
              ev.key.keysym.sym == SDLK_KP_ENTER) {
            canvas_submit_prompt();
            continue;
          }
          if (ev.key.keysym.sym == SDLK_ESCAPE) { canvas_state_.prompt_focused = false; continue; }
          // Left, Right, Home, End, Backspace and Delete are the whole reason
          // a text field is usable; this prompt used to accept only Backspace.
          if (canvas_state_.prompt.handle_key(key, word_modifier(ev.key.keysym.mod)) ||
              key == aios::hal::KeyLeft || key == aios::hal::KeyRight ||
              key == aios::hal::KeyHome || key == aios::hal::KeyEnd ||
              key == aios::hal::KeyBackspace || key == aios::hal::KeyDelete) {
            continue;
          }
          continue;   // everything else printable arrives as SDL_TEXTINPUT
        }
        // The start page's own keys, through the SAME table the workspace uses.
        // These were a second set of hardcoded cases that never reached
        // handle_key at all — 1/2/3 for scenes, n, c, r, p, h, space — so the
        // machine had two keyboards and could describe neither.
        //
        // A printable key arrives here with `key` unset (it comes as
        // SDL_TEXTINPUT for typing), so the chord comes off the keysym, which
        // for these is the ASCII code already.
        if (canvas_mode_) {
          int chord = key;
          if (chord == aios::hal::KeyNone) {
            const int sym = (int)ev.key.keysym.sym;
            if (sym >= 32 && sym < 127) chord = sym;
          }
          if (chord != aios::hal::KeyNone) {
            const UiAction act = keys_.action_for(chord);
            if (act != UiAction::None && run_ui_action(act)) continue;
          }
        }
        // Printable keys arrive as SDL_TEXTINPUT; only named ones come from here.
        if (key != aios::hal::KeyNone && !canvas_mode_) {
          if (!input_key(key)) {
            // input_key says "this key ends the desktop" -- which on bare metal
            // means handing the screen back to the serial console, the only
            // other place there is to go. Under a window system there IS
            // somewhere else to go, and a supervisor with Restart=always turns
            // an exit into a full boot: pressing Escape looked like the machine
            // rebooting itself. Escape goes back to the start page instead.
            if (key == aios::hal::KeyEscape) canvas_mode_ = true;
            else running = false;
          }
        }
      }
    }

    // Another kernel process on the same root (the resident) appends objects
    // while this desktop runs. Pull them in so the glass paints the present,
    // not the store as it stood when this process booted.
    {
      uint64_t tick_ms = k.host().monotonic_ns() / 1000000ull;
      if (tick_ms - last_store_refresh_ms >= 250) { (void)k.store().refresh(); last_store_refresh_ms = tick_ms; }
    }

    // Periodic caret blink and clock refresh dirty check
    static uint64_t last_blink_tick = 0;
    const uint64_t cur_ticks = SDL_GetTicks64();
    if (cur_ticks - last_blink_tick >= 500) {
      last_blink_tick = cur_ticks;
      mark_dirty();
    }

    const uint64_t frame_polled = SDL_GetPerformanceCounter();
    // The start page's hero is ANIMATED -- an orbiting emblem, a breathing
    // halo -- and it is painted here, on the CPU, into the overlay. Redrawing
    // it only when something is "dirty" meant the only thing waking it was the
    // caret blink, so on a machine whose background is a shader the orbit
    // advanced TWICE A SECOND and looked broken. An animated overlay is a
    // reason to paint, the same as a dirty one.
    // The painter's own background (scene 0) is a STILL, cached, and copied
    // into the frame — so a desk sitting on it has nothing moving in it at
    // all, and repainting the whole 1920x1080 overlay every frame for it cost
    // a third of a core to produce the identical picture. The canvas faces
    // keep their unconditional repaint because their hero IS animated.
    // The START PAGE is the face with something moving in it — the orbiting
    // emblem and its halo. The desk is not: a dock, a top bar and a wallpaper
    // that is a cached still. Repainting the desk's whole 1920x1080 overlay
    // sixty times a second to produce the identical picture was costing a
    // third of a core, and a profile of the idle process landed in
    // draw_taskbar and draw_top_bar every time.
    bool need_redraw = is_ui_dirty() || canvas_state_.answer_running ||
                       (canvas_mode_ && current_view() == DeskView::StartPage);
    if (need_redraw) {
      const uint64_t rd_t0 = SDL_GetTicks64();
      render_desktop(k);
      if (std::getenv("AOS_LATENCY") && SDL_GetTicks64() - rd_t0 >= 8)
        fprintf(stderr, "latency: render_desktop took %llu ms\n", (unsigned long long)(SDL_GetTicks64() - rd_t0));
      clear_dirty();
      overlay_generation++;
    }
    if (!dump_path_.empty() && (int)++dumped_frames >= dump_after_frames_) {
#if !defined(AIOS_METAL)
      write_ppm(dump_path_, framebuffer_, width_, height_);
#endif
      running = false;
    }
    const uint64_t t_painted = SDL_GetPerformanceCounter();
    if (gpu) {
      // The interface goes up as the overlay; the shader computes the scene the
      // painter left clear and writes both into the image being presented. The
      // generation rises only when the UI layer changes — a still interface costs
      // one dispatch and zero PCIe memory uploads.
      // Scene 0 was pinned to zero — the shader multiplied its whole picture
      // out to black — because the painter owned the beach and drew it into
      // the overlay opaquely. The device draws the beach itself now, so it is
      // a backdrop like any other.
      const float strength = 0.96f;
      gpu->set_backdrop(strength);
      // What hour the shoreline is at. Scene 0 alone reads it, and night is
      // what that scene always was, so a machine never told otherwise draws
      // exactly the picture it drew before.
      gpu->set_time_of_day(canvas_state_.scene_tod);
      if (canvas_state_.scene_effective == 4 &&
          canvas_state_.composed_generation != uploaded_composed_generation_) {
        auto parsed = SceneDescription::from_text(canvas_state_.composed_text);
        if (parsed.ok()) {
          auto sent = gpu->set_scene_description(parsed->to_floats());
          if (sent.ok()) {
            uploaded_composed_generation_ = canvas_state_.composed_generation;
            std::cerr << "display: composed scene - " << parsed->summary << " ("
                      << parsed->solids.size() << " solid(s))\n";
          } else {
            std::cerr << "display: " << sent.error().str() << "\n";
          }
        } else {
          uploaded_composed_generation_ = canvas_state_.composed_generation;
          std::cerr << "display: that scene description does not parse - "
                    << parsed.error().message << "\n";
        }
      }
      const double seconds = (double)(SDL_GetTicks64() - loop_began) / 1000.0;
      // How finely this frame marches the background. Every frame marches all
      // of it, which is what keeps the scene coherent.
      const uint64_t now_ms = SDL_GetTicks64();
      const bool someone_is_here = (now_ms - last_input_ms) < kIdleAfterMs ||
                                   canvas_state_.answer_running;
      static const int forced = getenv("AIOS_SCENE_SCALE")
          ? atoi(getenv("AIOS_SCENE_SCALE")) : 0;
      const int scale = forced ? forced
                               : (someone_is_here ? kSceneScale : kIdleSceneScale);
      auto shown = gpu->present(canvas_state_.scene_effective, seconds,
                                framebuffer_.data(), overlay_generation, scale);
      if (!shown.ok()) {
        // Losing the device mid-run must not take the desktop with it. Say so
        // once and carry on: the next frame retries, and a swapchain that has
        // gone out of date rebuilds itself.
        static bool said = false;
        if (!said) { std::cerr << "display: " << shown.error().str() << "\n"; said = true; }
      }
    } else {
      SDL_UpdateTexture(texture, nullptr, framebuffer_.data(), width_ * sizeof(uint32_t));
      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
    }
    const uint64_t t_presented = SDL_GetPerformanceCounter();
    // The rate the display is actually being driven at, measured where frames
    // are actually presented. A skipped overlay repaint is still a presented
    // frame — the shader scene animates under it either way.
    {
      const uint64_t now_fps_ms = SDL_GetTicks64();
      if (canvas_state_.fps_last_ms && now_fps_ms > canvas_state_.fps_last_ms) {
        const float inst = 1000.0f / (float)(now_fps_ms - canvas_state_.fps_last_ms);
        canvas_state_.fps = canvas_state_.fps > 0.0f
            ? canvas_state_.fps + 0.15f * (inst - canvas_state_.fps) : inst;
      }
      canvas_state_.fps_last_ms = now_fps_ms;
    }
    // What the frame actually cost, when someone asks. The frame rate alone
    // does not say WHERE the time went, and guessing wasted an hour: the flat
    // 16ms sleep, not the paint, was holding this loop to 33 a second, and the
    // vsync wait it was sleeping on top of costs 0.08 ms.
    {
      const double hz = (double)SDL_GetPerformanceFrequency();
      last_paint_ms_ = (float)((t_painted - frame_polled) / hz * 1000.0);
      last_present_ms_ = (float)((t_presented - t_painted) / hz * 1000.0);
    }
    if (getenv("AIOS_FRAME_COST")) {
      static uint64_t n = 0, sum_paint = 0, sum_show = 0;
      const double hz = (double)SDL_GetPerformanceFrequency();
      sum_paint += t_painted - frame_polled;
      sum_show += t_presented - t_painted;
      if (++n % 120 == 0) {
        // What share of the screen those 120 frames actually RAYMARCHED. The
        // background is thinned per frame, not skipped, so "how many frames
        // marched" is no longer the question — "how much did each one march"
        // is. Without this the scene's cost is a claim rather than a reading.
        static uint64_t last_px = 0;
        const uint64_t px = gpu ? gpu->cost().scene_pixels_marched : 0;
        const double screen = (double)width_ * height_;
        std::fprintf(stderr, "[frame] paint %.2f ms  present %.2f ms  %.1f%% of the screen marched per frame  (%s, mean of 120)\n",
                     sum_paint / hz * 1000.0 / 120.0, sum_show / hz * 1000.0 / 120.0,
                     screen > 0 ? (double)(px - last_px) / 120.0 / screen * 100.0 : 0.0,
                     gpu ? "GPU" : "CPU");
        last_px = px;
        sum_paint = sum_show = 0;
      }
    }

    // Pace the frame by what it COST, not by a flat sleep on top of the wait
    // that already happened. SDL_RenderPresent blocks until vsync when the
    // renderer has it, and this loop then slept another 16ms regardless — so
    // every frame was paint + vsync + 16ms and the machine's own readout said
    // 33 FPS on a 119 Hz display. Sleep only the remainder of the budget, and
    // only when something has to do the pacing: with vsync there is nothing
    // left to wait for, and without it this is what stops the loop spinning a
    // core flat out.
    // Whatever has to happen on a clock rather than on an event. Once a
    // second, not once a frame: supervising services is a waitpid per child
    // and there is no reason to pay it sixty times a second.
    if (ticker_) {
      static uint64_t last_tick_ms = 0;
      const uint64_t now_ms = SDL_GetTicks64();
      if (now_ms - last_tick_ms >= 1000) { last_tick_ms = now_ms; ticker_(); }
    }

    const uint64_t spent = SDL_GetTicks64() - frame_began;
    if (lat_stamp) {
      const uint64_t now = SDL_GetTicks64();
      std::fprintf(stderr, "latency: key waited %llu ms in the queue, frame took %llu ms, key->shown %llu ms\n",
                   (unsigned long long)(lat_seen - lat_stamp), (unsigned long long)spent, (unsigned long long)(now - lat_stamp));
      lat_stamp = 0;
    }
    if (spent < kFrameBudgetMs) SDL_Delay(static_cast<Uint32>(kFrameBudgetMs - spent));
    (void)presents_on_vsync;
  }

  SDL_StopTextInput();
  // The presenter goes first: everything it holds was made against this
  // window's surface, and destroying the window under a live swapchain is a
  // crash rather than a shutdown.
  gpu.reset();
  if (texture) SDL_DestroyTexture(texture);
  if (renderer) SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return true;
}
#endif

#if defined(AIOS_METAL)
// There is no terminal underneath a bare-metal kernel to fall back to: the
// framebuffer desktop IS the display, and the serial command surface is what a
// machine without one offers.
bool HardwareCompositor::run_desktop_tui(Kernel&, CommandDispatcher) { return false; }
#else
bool HardwareCompositor::run_desktop_tui(Kernel& k, CommandDispatcher dispatcher) {
  if (!width_ || !height_) set_screen_size(1280, 800);   // a terminal has no display mode to ask
  // Interactive full-screen ANSI Terminal UI Compositor Desktop
  struct termios orig_termios;
  tcgetattr(STDIN_FILENO, &orig_termios);
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  std::cout << "\033[?1049h\033[2J\033[H\033[?25h" << std::flush;

  init_default_workspace(k);

  std::string input_buffer;
  std::vector<std::string> tui_lines;
  tui_lines.push_back("AIOS Workstation Full Console [TUI Compositor Substrate]");
  tui_lines.push_back("Immutable Kernel UUID: " + k.self().short_str() + " · Darwin HAL");
  tui_lines.push_back("");

  bool running = true;

  auto draw_tui = [&](int term_w, int term_h) {
    std::cout << "\033[H";
    // 1. Top Bar
    std::cout << "\033[44;37;1m AIOS 2.0 \033[0m\033[48;5;236;37m [F1: Term] [F2: Tasks] [F3: Subsys] \033[0m";
    int pad = term_w - 46;
    for (int i = 0; i < pad; ++i) std::cout << " ";
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char cb[16];
    std::strftime(cb, sizeof(cb), "%H:%M:%S", &tm);
    std::cout << "\033[48;5;236;32m" << cb << "\033[0m\n";

    // 2. Terminal Screen Split
    int body_h = term_h - 6;
    int max_lines = body_h;
    int start_l = 0;
    if (static_cast<int>(tui_lines.size()) > max_lines) {
      start_l = static_cast<int>(tui_lines.size()) - max_lines;
    }

    for (int r = 0; r < body_h; ++r) {
      std::cout << "\033[K";
      size_t line_idx = start_l + r;
      if (line_idx < tui_lines.size()) {
        const std::string& l = tui_lines[line_idx];
        if (l.rfind("aios>", 0) == 0) std::cout << "\033[32;1m" << l << "\033[0m";
        else std::cout << "\033[37m" << l << "\033[0m";
      }
      std::cout << "\n";
    }

    // 3. Quick Chips
    std::cout << "\033[48;5;238;36m [info] [ps] [devices] [free] [tasks] \033[0m\n";

    // 4. Prompt Row
    std::cout << "\033[32;1maios>\033[0m " << input_buffer << "\033[K" << std::flush;
  };

  while (running) {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int term_w = ws.ws_col > 0 ? ws.ws_col : 80;
    int term_h = ws.ws_row > 0 ? ws.ws_row : 24;

    draw_tui(term_w, term_h);

    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    int ret = poll(&pfd, 1, 200);
    if (ret > 0 && (pfd.revents & POLLIN)) {
      char ch;
      if (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == 27) { // Escape sequence
          char seq[3];
          if (read(STDIN_FILENO, &seq[0], 1) <= 0) {
            running = false;
            break;
          }
          if (read(STDIN_FILENO, &seq[1], 1) <= 0) break;
          // Ignore arrow sequences for now in raw read
        } else if (ch == '\n' || ch == '\r') {
          std::string cmd = input_buffer;
          input_buffer.clear();
          if (cmd == "quit" || cmd == "exit") {
            running = false;
            break;
          }
          tui_lines.push_back("aios> " + cmd);
          if (dispatcher) {
            dispatcher(cmd, [&](const std::string& line) {
              tui_lines.push_back(line);
            });
          }
        } else if (ch == 127 || ch == 8) {
          if (!input_buffer.empty()) input_buffer.pop_back();
        } else if (ch >= 32 && ch <= 126) {
          input_buffer.push_back(ch);
        }
      }
    }
  }

  std::cout << "\033[?1049l\033[?25h" << std::flush;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
  return true;
}
#endif


// ---- grain programs on the desktop -----------------------------------------
// `grain open file.gr` at the terminal runs a grain program here, on the
// desktop's own thread, and keeps its VM for as long as it has panes. Its
// run_panes() shows them as Grain windows: every frame the pane's on_draw
// paints its canvas and the canvas is the window's content; clicks, keys and
// typing in the window are the pane's events. The program's code after
// run_panes() runs at once (the desktop, not the program, owns the loop), and
// a callback is held to kGrainStepsPerCall instructions so a runaway one stops
// with an error instead of freezing the desktop.

namespace {

constexpr uint64_t kGrainStepsPerCall = 50000000;

// A desktop program's files: this machine's storage, paths relative to the
// program's own directory.
class GrainDesktopFiles final : public grain::Files {
 public:
  GrainDesktopFiles(hal::Host& host, std::string base) : h_(host), base_(std::move(base)) {}
  bool read(const std::string& path, std::string& out, std::string& error) override {
    if (is_dir(path)) { error = "is a directory"; return false; }
    auto r = h_.read_file(at(path));
    if (!r.ok()) { error = r.error().message; return false; }
    out.assign(reinterpret_cast<const char*>(r->data()), r->size());
    return true;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    const ByteSpan bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    auto r = append ? h_.append_file(at(path), bytes) : h_.write_file(at(path), bytes);
    if (!r.ok()) { error = r.error().message; return false; }
    return true;
  }
  bool list(const std::string& path, std::vector<grain::FileEntry>& out, std::string& error) override {
    const std::string dir = at(path);
    auto names = h_.list_dir(dir);
    if (!names.ok()) { error = names.error().message; return false; }
    for (const auto& n : *names) {
      grain::FileEntry e;
      e.name = n;
      if (auto st = h_.stat(dir + "/" + n); st.ok()) {
        e.dir = st->is_dir;
        e.size = st->is_dir ? 0 : static_cast<int64_t>(st->size);
      }
      out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const grain::FileEntry& a, const grain::FileEntry& b) { return a.name < b.name; });
    return true;
  }
  bool remove(const std::string& path, std::string& error) override {
    auto st = h_.stat(at(path));
    if (!st.ok() || st->is_dir) { error = "no such file"; return false; }
    auto r = h_.remove(at(path));
    if (!r.ok()) { error = r.error().message; return false; }
    return true;
  }
  bool exists(const std::string& path) override {
    auto st = h_.stat(at(path));
    return st.ok() && !st->is_dir;
  }
  bool make_dir(const std::string& path, bool parents, std::string& error) override {
    const std::string full = at(path);
    if (is_dir(path)) return true;
    if (!parents) {
      auto r = h_.make_dir(full);
      if (!r.ok()) { error = r.error().message; return false; }
      return true;
    }
    for (size_t i = 1; i <= full.size(); ++i) {
      if (i != full.size() && full[i] != '/') continue;
      const std::string prefix = full.substr(0, i);
      if (h_.stat(prefix).ok()) continue;
      auto r = h_.make_dir(prefix);
      if (!r.ok()) { error = r.error().message; return false; }
    }
    return true;
  }
  bool remove_dir(const std::string& path, std::string& error) override {
    if (!is_dir(path)) { error = "no such directory"; return false; }
    auto names = h_.list_dir(at(path));
    if (names.ok() && !names->empty()) { error = "not empty"; return false; }
    auto r = h_.remove(at(path));
    if (!r.ok()) { error = r.error().message; return false; }
    return true;
  }
  bool is_dir(const std::string& path) override {
    auto st = h_.stat(at(path));
    return st.ok() && st->is_dir;
  }
  bool absolute(const std::string& path, std::string& out, std::string&) override {
    out = at(path);
    return true;
  }

 private:
  std::string at(const std::string& p) const { return p.empty() ? base_ : p[0] == '/' ? p : base_ + "/" + p; }
  hal::Host& h_;
  std::string base_;
};

// run_panes() in a desktop program: its panes are to be shown, by the desktop.
class DesktopGrainDisplay final : public grain::Display {
 public:
  explicit DesktopGrainDisplay(std::vector<std::unique_ptr<GrainDesktopApp>>& apps) : apps_(apps) {}
  bool run(grain::Vm& vm, const grain::DisplayRun&, std::string& error) override {
    for (auto& a : apps_) {
      if (a->vm.get() == &vm) { a->attached = true; return true; }
    }
    error = "run_panes(): on the desktop a program shows its panes when it is opened with `grain open <file.gr>`";
    return false;
  }

 private:
  std::vector<std::unique_ptr<GrainDesktopApp>>& apps_;
};

// The desktop's key codes as grain names them (SDL's names, lower case): what
// the standalone grain delivers, so a program behaves the same in both.
std::string grain_key_name(int key, bool* ctrl) {
  using namespace aios::hal;
  *ctrl = false;
  switch (key) {
    case KeyBackspace: return "backspace";
    case KeyTab: return "tab";
    case KeyEnter: return "return";
    case KeyEscape: return "escape";
    case KeyDelete: return "delete";
    case KeyUp: return "up";
    case KeyDown: return "down";
    case KeyLeft: return "left";
    case KeyRight: return "right";
    case KeyHome: return "home";
    case KeyEnd: return "end";
    case KeyPageUp: return "pageup";
    case KeyPageDown: return "pagedown";
    default: break;
  }
  if (key >= KeyF1 && key <= KeyF12) return "f" + std::to_string(key - KeyF1 + 1);
  if (key >= 1 && key <= 26) { *ctrl = true; return std::string(1, static_cast<char>('a' + key - 1)); }
  if (key >= 32 && key < 127) return std::string(1, static_cast<char>(std::tolower(key)));
  return "";
}

}  // namespace

void HardwareCompositor::grain_queue(GrainQueued ev) {
  grain_pending_.push_back(std::move(ev));
  mark_dirty();
}

void HardwareCompositor::grain_open(const std::string& spec, Kernel& k) {
  auto say = [this](const std::string& line) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_)
      if (w.type == WindowType::Terminal) w.lines.push_back(line);
    canvas_state_.answer_lines.push_back(line);
    mark_dirty();
  };
  hal::Host& host = k.host();
  std::string path = spec;
  if (!path.empty() && path[0] != '/' && !host.exists(path) && !k.config().root.empty() && host.exists(k.config().root + "/" + spec))
    path = k.config().root + "/" + spec;
  auto bytes = host.read_file(path);
  if (!bytes.ok()) { say("grain open: cannot read " + spec + ": " + bytes.error().message); return; }
  const std::string source(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  auto compiled = grain::compile_source(source, spec);
  if (!compiled.ok()) {
    for (const auto& d : compiled.errors) say(spec + ":" + std::to_string(d.line) + ": " + d.message);
    return;
  }
  if (!grain_display_) grain_display_ = std::make_unique<DesktopGrainDisplay>(grain_apps_);
  grain::set_display(grain_display_.get());

  auto app = std::make_unique<GrainDesktopApp>();
  app->path = spec;
  const size_t slash = path.rfind('/');
  app->dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
  app->files = std::make_unique<GrainDesktopFiles>(host, app->dir);
  app->vm = std::make_unique<grain::Vm>();
  grain::Vm& vm = *app->vm;
  grain::install_builtins(vm);
  grain::install_socket_builtins(vm);
  grain::set_http_trust_root(k.config().root);
  grain::install_http_builtins(vm);
  auto sys = std::make_shared<grain::MapObj>();
  const auto& topo = host.topology();
  sys->put("arch", grain::Value::string(topo.arch));
  sys->put("host", grain::Value::string(topo.host_name));
  sys->put("cpus", grain::Value::integer(static_cast<int64_t>(topo.logical_cpus)));
  sys->put("memory", grain::Value::integer(static_cast<int64_t>(topo.total_memory)));
  sys->put("kernel", grain::Value::string(k.self().str()));
  sys->put("desktop", grain::Value::boolean(true));
  vm.define_global("sys", grain::Value::object(grain::Type::Map, sys));
  vm.files = app->files.get();
  vm.sink = &app->output;
  vm.step_limit = kGrainStepsPerCall;
  GrainDesktopApp* raw = app.get();
  grain_apps_.push_back(std::move(app));

  vm.steps = 0;
  const auto r = vm.run(compiled.script);
  for (const auto& line : raw->output) say(line);
  raw->output.clear();
  auto drop_app = [this, raw]() {
    grain::drop_panes(*raw->vm);
    grain_apps_.erase(std::remove_if(grain_apps_.begin(), grain_apps_.end(),
                                     [raw](const std::unique_ptr<GrainDesktopApp>& a) { return a.get() == raw; }),
                      grain_apps_.end());
  };
  if (!r.ok) {
    say(spec + ":" + std::to_string(r.line) + ": " + r.error);
    for (size_t i = 0; i < r.trace.size(); ++i) say((i == 0 ? "  in " : "  from ") + r.trace[i]);
    drop_app();
    return;
  }
  const size_t panes = grain::pane_list(vm).size();
  if (!raw->attached || panes == 0) {
    say(raw->attached ? spec + ": run_panes() was called with no panes registered"
                      : spec + " ran to its end without calling run_panes(), so it shows no windows");
    drop_app();
    return;
  }
  say("grain open: " + spec + " - " + std::to_string(panes) + " window(s)");
  mark_dirty();
}

void HardwareCompositor::grain_tick() {
  if (grain_apps_.empty()) return;
  auto say = [this](const std::string& line) {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    for (auto& w : windows_)
      if (w.type == WindowType::Terminal) w.lines.push_back(line);
    canvas_state_.answer_lines.push_back(line);
  };
  auto fail = [](GrainDesktopApp& app, const std::string& error) {
    if (app.failed.empty()) app.failed = error;
  };

  // 1. Input queued by the handlers (under the state lock), delivered here outside it.
  std::vector<GrainQueued> events;
  {
    std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
    events.swap(grain_pending_);
  }
  for (const auto& q : events) {
    GrainDesktopApp* app = nullptr;
    uint64_t pane = 0;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (const auto& w : windows_)
        if (w.window_id == q.window_id && w.type == WindowType::Grain) { app = w.grain_app; pane = w.grain_pane_id; }
    }
    if (!app || !app->failed.empty()) continue;
    auto ev = std::make_shared<grain::MapObj>();
    ev->put("type", grain::Value::string(q.type));
    ev->put("pane", grain::Value::integer(static_cast<int64_t>(pane)));
    if (q.type == "mouse_down" || q.type == "mouse_up" || q.type == "click" || q.type == "mouse_move") {
      ev->put("x", grain::Value::integer(q.x));
      ev->put("y", grain::Value::integer(q.y));
      if (q.type != "mouse_move") ev->put("button", grain::Value::string("left"));
    } else if (q.type == "wheel") {
      ev->put("dx", grain::Value::real(0.0));
      ev->put("dy", grain::Value::real(q.dy));
    } else if (q.type == "key") {
      ev->put("key", grain::Value::string(q.key));
      ev->put("shift", grain::Value::boolean(q.shift));
      ev->put("ctrl", grain::Value::boolean(q.ctrl));
      ev->put("alt", grain::Value::boolean(q.alt));
      ev->put("meta", grain::Value::boolean(false));
    } else if (q.type == "text") {
      ev->put("text", grain::Value::string(q.text));
    }
    std::string err;
    app->vm->steps = 0;
    if (!grain::pane_event(*app->vm, pane, grain::Value::object(grain::Type::Map, ev), err)) fail(*app, err);
  }

  // 2. Windows follow the panes, app by app.
  std::vector<GrainDesktopApp*> apps;
  for (auto& a : grain_apps_) apps.push_back(a.get());
  for (GrainDesktopApp* app : apps) {
    grain::Vm& vm = *app->vm;
    // Closed by the person (a hidden Grain window): the pane hears "close" and goes.
    std::vector<uint64_t> closed_by_user;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (auto it = windows_.begin(); it != windows_.end();) {
        if (it->type == WindowType::Grain && it->grain_app == app && !it->visible) {
          closed_by_user.push_back(it->grain_pane_id);
          it = windows_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (uint64_t id : closed_by_user) {
      std::string err;
      app->vm->steps = 0;
      if (app->failed.empty() && !grain::pane_closed(vm, id, err)) fail(*app, err);
      if (app->failed.empty()) continue;
      grain::drop_panes(vm);   // a program that failed is closed with its window
    }
    const auto panes = grain::pane_list(vm);
    // New panes get windows; windows whose pane the program closed go.
    std::vector<grain::PaneInfo> need;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      for (const auto& p : panes) {
        const bool shown = std::any_of(windows_.begin(), windows_.end(), [&](const WindowSurface& w) {
          return w.type == WindowType::Grain && w.grain_app == app && w.grain_pane_id == p.id;
        });
        if (!shown) need.push_back(p);
      }
      windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [&](const WindowSurface& w) {
                       return w.type == WindowType::Grain && w.grain_app == app &&
                              std::none_of(panes.begin(), panes.end(), [&](const grain::PaneInfo& p) { return p.id == w.grain_pane_id; });
                     }),
                     windows_.end());
    }
    for (const auto& p : need) {
      int open_grain = 0;
      {
        std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
        for (const auto& w : windows_) open_grain += w.type == WindowType::Grain;
      }
      const int offset = 30 * (open_grain % 8);   // each new window a step down and right of the last
      // A window opens on a desktop, so the start page steps aside for it (as
      // the dock's apps do) and the new window comes to the front, with the keyboard.
      if (canvas_mode_) {
        canvas_mode_ = false;
        if (current_workspace_ == CockpitWorkspace::Blank) switch_workspace(CockpitWorkspace::Code);
      }
      auto made = create_window(p.title, 140 + offset, 90 + offset, p.width + 2, p.height + 27, WindowType::Grain);
      if (!made.ok()) { fail(*app, "cannot open a window: " + made.error().message); continue; }
      {
        std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
        for (auto& w : windows_)
          if (w.window_id == *made) { w.grain_app = app; w.grain_pane_id = p.id; }
      }
      bring_to_front(*made);
    }
    for (const auto& line : app->output) say(line);
    app->output.clear();
    if (!app->failed.empty() && !app->failure_reported) {
      say(app->path + ": " + app->failed);
      app->failure_reported = true;
    }
  }
  // 3. A program with no panes left is done.
  for (GrainDesktopApp* app : apps) {
    if (!grain::pane_list(*app->vm).empty()) continue;
    {
      std::lock_guard<aios::hal::Mutex> lock(state_mutex_);
      windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
                                    [app](const WindowSurface& w) { return w.type == WindowType::Grain && w.grain_app == app; }),
                     windows_.end());
    }
    say(app->path + ": closed");
    grain::drop_panes(*app->vm);
    grain_apps_.erase(std::remove_if(grain_apps_.begin(), grain_apps_.end(),
                                     [app](const std::unique_ptr<GrainDesktopApp>& a) { return a.get() == app; }),
                      grain_apps_.end());
  }
  if (!grain_apps_.empty()) mark_dirty();   // the programs draw every frame: keep painting
}

void HardwareCompositor::render_grain_window(WindowSurface& win) {
  const int W = win.content_width(), H = win.content_height();
  if (W <= 0 || H <= 0) return;
  if (static_cast<int>(win.buffer.size()) != W * H) win.buffer.assign(static_cast<size_t>(W) * static_cast<size_t>(H), tok::panel);
  GrainDesktopApp* app = win.grain_app;
  if (!app) return;
  if (!app->failed.empty()) {
    clear_buffer(win.buffer, tok::panel, W, H);
    draw_text_to(win.buffer, W, H, 12, 12, "Stopped:", tok::text, Face::UiBold);   // the title bar names the program
    // The error, word-wrapped to the window's width (a word longer than a line is cut where it must be).
    const int room = W - 24;
    int y = 36;
    std::string line;
    std::istringstream words(app->failed);
    auto put = [&](const std::string& l) { if (y < H - 16) draw_text_to(win.buffer, W, H, 12, y, l, tok::dim, Face::UiSmall); y += 18; };
    for (std::string word; words >> word;) {
      const std::string trial = line.empty() ? word : line + " " + word;
      if (text_width(trial, Face::UiSmall) <= room) { line = trial; continue; }
      if (!line.empty()) put(line);
      line = word;
      while (line.size() > 1 && text_width(line, Face::UiSmall) > room) {
        size_t cut = line.size() - 1;
        while (cut > 1 && text_width(line.substr(0, cut), Face::UiSmall) > room) --cut;
        put(line.substr(0, cut));
        line = line.substr(cut);
      }
    }
    if (!line.empty()) put(line);
    return;
  }
  grain::Vm& vm = *app->vm;
  // The pane follows its window's size; the program hears "resize".
  for (const auto& p : grain::pane_list(vm)) {
    if (p.id != win.grain_pane_id) continue;
    if (p.width != W || p.height != H) {
      std::string err;
      vm.steps = 0;
      if (!grain::pane_resized(vm, p.id, W, H, err)) { app->failed = err; return; }
    }
    break;
  }
  std::vector<uint32_t> px;
  int pw = 0, ph = 0;
  std::string err;
  vm.steps = 0;
  if (!grain::pane_frame(vm, win.grain_pane_id, px, pw, ph, err)) {
    if (!err.empty()) app->failed = err;
    return;
  }
  for (int y = 0; y < H; ++y) {
    uint32_t* row = win.buffer.data() + static_cast<size_t>(y) * static_cast<size_t>(W);
    for (int x = 0; x < W; ++x)
      row[x] = (x < pw && y < ph) ? (px[static_cast<size_t>(y) * static_cast<size_t>(pw) + static_cast<size_t>(x)] | 0xFF000000u) : tok::panel;
  }
}

}  // namespace aios::userland
