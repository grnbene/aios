// The keyboard as a table. No <sstream> and no exceptions: this file is in the
// bare-metal CORE glob.
#include "aios/userland/key_bindings.hpp"
#include "aios/hal/keys.hpp"
#include <cstdlib>

namespace aios::userland {
namespace {

struct ActionFace { UiAction a; const char* slug; const char* help; };
const ActionFace kActions[] = {
  {UiAction::None,           "none",           "nothing; use it to take a key away"},
  {UiAction::FaceToggle,     "face.toggle",    "swap between the start page and the workspace"},
  {UiAction::FaceCycle,      "face.cycle",     "walk the desk, the folded panels and the bare scene"},
  {UiAction::FaceCollapse,   "face.collapse",  "fold the panels down to their headers"},
  {UiAction::SceneNext,      "scene.next",     "the next scene on the display"},
  {UiAction::SceneOcean,     "scene.ocean",    "the tide under a moon"},
  {UiAction::SceneCyber,     "scene.cyber",    "the lattice waking in a wavefront"},
  {UiAction::SceneOrb,       "scene.orb",      "the plasma core in its halo"},
  {UiAction::PaneShell,      "pane.shell",     "raise the shell"},
  {UiAction::PaneMachine,    "pane.machine",   "raise the machine pane"},
  {UiAction::PaneTasks,      "pane.tasks",     "raise the tasks pane"},
  {UiAction::PaneEditor,     "pane.editor",    "raise the editor"},
  {UiAction::PaneDatabase,   "pane.database",  "raise the database studio"},
  {UiAction::PaneWeb,        "pane.web",       "raise the web pane"},
  {UiAction::PaneMail,       "pane.mail",      "raise the mail client"},
  {UiAction::PaneContacts,   "pane.contacts",  "raise the contacts card manager"},
  {UiAction::PaneCalendar,   "pane.calendar",  "raise the calendar schedule"},
  {UiAction::PaneApps,       "pane.apps",      "raise the face applications studio"},
  {UiAction::PaneInspector,  "pane.inspector", "raise the universal in-desktop behavioral inspector"},
  {UiAction::MindStop,       "mind.stop",      "stop whatever the machine is running"},
  {UiAction::MachinePower,   "machine.power",  "open the power panel, which asks before it acts"},
  {UiAction::MachineRestart, "machine.restart","restart the machine"},
  {UiAction::PaletteToggle,  "palette.toggle", "open or toggle the fuzzy command palette launcher"},
  {UiAction::SnapLeft,       "window.snap_left","snap focused window to left half (50/50)"},
  {UiAction::SnapRight,      "window.snap_right","snap focused window to right half (50/50)"},
  {UiAction::SnapMaximize,   "window.maximize", "maximize / snap focused window to fullscreen"},
  {UiAction::Tile2Col,       "layout.tile2",   "auto-tile windows into 2 columns"},
  {UiAction::Tile3Col,       "layout.tile3",   "auto-tile windows into 3 columns"},
  {UiAction::WorkspaceCode,  "workspace.code", "switch to code workspace (editor + shell)"},
  {UiAction::WorkspaceComms, "workspace.comms","switch to comms workspace (mail + calendar)"},
  {UiAction::WorkspaceAdmin, "workspace.admin","switch to admin workspace (machine + database)"},
  {UiAction::WindowClose,    "window.close",   "close the focused window"},
  {UiAction::WindowCloseAll, "window.close_all","close all windows to blank slate dashboard"},
  {UiAction::DrawToggle,     "draw.toggle",    "the drawing surface: a black sheet the finger paints on"},
  {UiAction::DrawClear,      "draw.clear",     "take every stroke off the drawing surface"},
  {UiAction::DrawUndo,       "draw.undo",      "take the last stroke back off the drawing surface"},
  {UiAction::Screenshot,     "desktop.screenshot","keep what the screen is showing: a png the machine writes itself"},
};

struct NamedKey { const char* name; int key; };
const NamedKey kNamedKeys[] = {
  {"Escape", hal::KeyEscape}, {"Esc", hal::KeyEscape},
  {"Tab", hal::KeyTab}, {"Enter", hal::KeyEnter}, {"Return", hal::KeyEnter},
  {"Backspace", hal::KeyBackspace}, {"Delete", hal::KeyDelete}, {"Space", ' '},
  {"Up", hal::KeyUp}, {"Down", hal::KeyDown}, {"Left", hal::KeyLeft}, {"Right", hal::KeyRight},
  {"Home", hal::KeyHome}, {"End", hal::KeyEnd},
  {"PageUp", hal::KeyPageUp}, {"PageDown", hal::KeyPageDown},
  {"F1", hal::KeyF1}, {"F2", hal::KeyF2}, {"F3", hal::KeyF3}, {"F4", hal::KeyF4},
  {"F5", hal::KeyF5}, {"F6", hal::KeyF6}, {"F7", hal::KeyF7}, {"F8", hal::KeyF8},
  {"F9", hal::KeyF9}, {"F10", hal::KeyF10}, {"F11", hal::KeyF11}, {"F12", hal::KeyF12},
};

char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool same_ci(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i) if (lower_c(a[i]) != lower_c(b[i])) return false;
  return i == a.size() && !b[i];
}

}  // namespace

const char* action_slug(UiAction a) {
  for (const auto& f : kActions) if (f.a == a) return f.slug;
  return "none";
}

const char* action_help(UiAction a) {
  for (const auto& f : kActions) if (f.a == a) return f.help;
  return "";
}

bool parse_action(const std::string& s, UiAction& out) {
  for (const auto& f : kActions) if (same_ci(s, f.slug)) { out = f.a; return true; }
  return false;
}

std::string chord_name(int key) {
  for (const auto& n : kNamedKeys) if (n.key == key) return n.name;
  // Ctrl+<letter> arrives as the control code that letter names.
  if (key >= 1 && key <= 26 && key != hal::KeyBackspace && key != hal::KeyTab &&
      key != hal::KeyEnter) {
    std::string s = "Ctrl+";
    s += (char)('A' + key - 1);
    return s;
  }
  if (key > 32 && key < 127) return std::string(1, (char)key);
  return std::string();
}

bool parse_chord(const std::string& s, int& key) {
  if (s.empty()) return false;
  for (const auto& n : kNamedKeys) if (same_ci(s, n.name)) { key = n.key; return true; }
  if (s.size() == 6 && (s[0] == 'C' || s[0] == 'c') && s[4] == '+') {
    // "Ctrl+R"
    if (same_ci(s.substr(0, 5), "ctrl+")) {
      const char c = lower_c(s[5]);
      if (c >= 'a' && c <= 'z') { key = c - 'a' + 1; return true; }
    }
  }
  if (s.size() == 1) {
    // A bare printable. Lower-cased, because a keyboard reports the letter and
    // this machine has no shift state to distinguish "r" from "R" as a chord.
    const char c = lower_c(s[0]);
    if (c > 32 && c < 127) { key = c; return true; }
  }
  return false;
}

KeyBindings KeyBindings::defaults() {
  KeyBindings b;
  // Exactly what the hardcoded branches did, so nothing changes until somebody
  // rebinds something. The point of this table is not new behaviour, it is
  // that the behaviour has a name and can be altered.
  b.bind(hal::KeyF1, UiAction::PaneShell);
  b.bind(hal::KeyF2, UiAction::PaneMachine);
  b.bind(hal::KeyF3, UiAction::PaneTasks);
  b.bind(hal::KeyF4, UiAction::PaneEditor);
  b.bind(hal::KeyF5, UiAction::PaneDatabase);
  b.bind(hal::KeyF6, UiAction::PaneWeb);
  b.bind(hal::KeyF7, UiAction::PaneMail);
  b.bind(hal::KeyF8, UiAction::PaneContacts);
  b.bind(hal::KeyF9, UiAction::PaneCalendar);
  b.bind(hal::KeyF10, UiAction::PaneApps);
  b.bind(hal::KeyF11, UiAction::PaneInspector);
  b.bind(hal::KeyEscape, UiAction::MindStop);
  b.bind(hal::KeyCtrlC, UiAction::MindStop);
  b.bind(hal::KeyCtrlR, UiAction::MachineRestart);
  b.bind(hal::KeyCtrlP, UiAction::PaletteToggle);
  b.bind(hal::KeyCtrlK, UiAction::PaletteToggle);
  b.bind(hal::KeyCtrlW, UiAction::WindowClose);
  b.bind(' ', UiAction::FaceCycle);
  b.bind('p', UiAction::FaceCycle);
  b.bind('h', UiAction::FaceCycle);
  b.bind('c', UiAction::FaceCollapse);
  b.bind('n', UiAction::SceneNext);
  b.bind('1', UiAction::SceneOcean);
  b.bind('2', UiAction::SceneCyber);
  b.bind('3', UiAction::SceneOrb);
  b.bind('r', UiAction::MachinePower);
  // The drawing surface. F12 because it is the one function key no pane had
  // claimed and it is the only chord that reaches the table from BOTH faces —
  // a printable key only gets there on the start page, where the workspace
  // hands letters to whatever has the keyboard instead.
  b.bind(hal::KeyF12, UiAction::DrawToggle);
  b.bind('d', UiAction::DrawToggle);
  b.bind('x', UiAction::DrawClear);
  // z for undo, because that is the key every hand already reaches for.
  b.bind('z', UiAction::DrawUndo);
  // The shutter. 's' because every function key is already a pane or the
  // sheet, and because this one has to work on the drawing surface, where a
  // printable is the only thing that arrives at all on a hosted backend.
  b.bind('s', UiAction::Screenshot);
  return b;
}

UiAction KeyBindings::action_for(int key) const {
  for (const auto& b : bindings_) if (b.key == key) return b.action;
  return UiAction::None;
}

void KeyBindings::bind(int key, UiAction a) {
  for (auto& b : bindings_) {
    if (b.key == key) { b.action = a; return; }
  }
  bindings_.push_back(Binding{key, a});
}

bool KeyBindings::unbind(int key) {
  for (size_t i = 0; i < bindings_.size(); ++i) {
    if (bindings_[i].key == key) { bindings_.erase(bindings_.begin() + (long)i); return true; }
  }
  return false;
}

std::string KeyBindings::to_text() const {
  std::string t = "# this machine's keyboard. one binding per line:\n#   bind <chord> <action>\n";
  for (const auto& b : bindings_) {
    const std::string name = chord_name(b.key);
    if (name.empty()) continue;
    t += "bind " + name + " " + action_slug(b.action) + "\n";
  }
  return t;
}

Result<void> KeyBindings::apply_text(const std::string& text) {
  size_t at = 0;
  std::string trouble;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    // Split on blanks.
    std::vector<std::string> f;
    for (size_t p = 0; p < line.size();) {
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
      size_t e = p;
      while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r') ++e;
      if (e > p) f.push_back(line.substr(p, e - p));
      p = e;
    }
    if (!f.empty() && f[0].size() && f[0][0] != '#') {
      if (same_ci(f[0], "bind") && f.size() >= 3) {
        int key = 0;
        UiAction a = UiAction::None;
        if (!parse_chord(f[1], key)) trouble += (trouble.empty() ? "" : "; ") + ("no key called '" + f[1] + "'");
        else if (!parse_action(f[2], a)) trouble += (trouble.empty() ? "" : "; ") + ("no action called '" + f[2] + "'");
        else if (a == UiAction::None) unbind(key);
        else bind(key, a);
      } else if (same_ci(f[0], "unbind") && f.size() >= 2) {
        int key = 0;
        if (parse_chord(f[1], key)) unbind(key);
        else trouble += (trouble.empty() ? "" : "; ") + ("no key called '" + f[1] + "'");
      }
    }
    if (at > text.size()) break;
  }
  // The good lines are already applied. A bad one is reported rather than
  // thrown away silently, because the mind writing this file needs to be told
  // when it invented an action name.
  if (!trouble.empty()) return Result<void>(Status::Invalid, trouble);
  return Ok();
}

std::string run_browse_path(const std::string& root) {
  const char* run_env = getenv("AOS_RUN");
  if (run_env && *run_env) return std::string(run_env) + "/browse";
  // The machine's own run directory (what run_dir() gives), not a path
  // relative to whatever directory the process was started in.
  return (root.empty() ? std::string("run") : root + "/run") + "/browse";
}

std::string key_bindings_path(const std::string& root) {
  const char* run_env = getenv("AOS_RUN");
  if (run_env && *run_env) return std::string(run_env) + "/keys";
  // The machine's own run directory (what run_dir() gives), not a path
  // relative to whatever directory the process was started in.
  return (root.empty() ? std::string("run") : root + "/run") + "/keys";
}

std::string run_draw_path(const std::string& root) {
  const char* run_env = getenv("AOS_RUN");
  if (run_env && *run_env) return std::string(run_env) + "/draw";
  // The machine's own run directory (what run_dir() gives), not a path
  // relative to whatever directory the process was started in.
  return (root.empty() ? std::string("run") : root + "/run") + "/draw";
}

}  // namespace aios::userland
