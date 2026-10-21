#pragma once
// The machine's keyboard, as data.
//
// Every key this desktop answered to was a branch: `if (key == KeyCtrlR)`,
// `if (key >= KeyF1 && key <= KeyF6)`, a `static const WindowType by_fkey[]`,
// and a second set of single-letter cases in the SDL loop that never reached
// handle_key at all. So the machine could not list its own bindings, name an
// action, or change one — asked to re-do its keyboard it had nothing to
// operate on, and only a rebuild could alter a key.
//
// A binding is now a chord and the NAME of an action. Naming the actions is
// the part that makes the request expressible: "swap F1 and F2" is only a
// sentence you can act on if there is something for F1 and F2 to be swapped
// BETWEEN.
//
// What is deliberately NOT here: context. Escape inside a database cell means
// "forget this edit" and inside the editor means "leave the buffer" — those
// are what the focused thing does with a key, not what the key is bound to,
// and flattening them into this table would make every one of them rebindable
// into nonsense.
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::userland {

enum class UiAction : uint8_t {
  None = 0,
  FaceToggle,      // the start page and the workspace
  FaceCycle,       // desk -> collapsed -> scene, the way the space bar walks it
  FaceCollapse,    // fold the panels
  SceneNext,
  SceneOcean,
  SceneCyber,
  SceneOrb,
  PaneShell,
  PaneMachine,
  PaneTasks,
  PaneEditor,
  PaneDatabase,
  PaneWeb,
  PaneMail,
  PaneContacts,
  PaneCalendar,
  PaneApps,
  PaneInspector,
  MindStop,        // stop whatever the machine is running
  MachinePower,    // open the power panel; it asks before it acts
  MachineRestart,
  PaletteToggle,   // open or toggle the fuzzy command palette
  SnapLeft,        // snap active window to left 50%
  SnapRight,       // snap active window to right 50%
  SnapMaximize,    // maximize / snap active window fullscreen
  Tile2Col,        // tile windows into 2 columns
  Tile3Col,        // tile windows into 3 columns
  WorkspaceCode,   // switch to Code workspace
  WorkspaceComms,  // switch to Comms workspace
  WorkspaceAdmin,  // switch to Admin workspace
  WindowClose,     // close active window
  WindowCloseAll,  // close all windows to blank slate
  DrawToggle,      // the drawing surface: a black sheet the pointer paints on
  DrawClear,       // take every stroke off that sheet
  DrawUndo,        // take only the LAST stroke off it
  Screenshot,      // the machine's own shutter: write this frame out as a png
  Count,
};

// The name that goes in the bindings file and in a sentence the mind writes.
const char* action_slug(UiAction a);
// One line saying what it does, for `keys` and for the mind's own reading.
const char* action_help(UiAction a);
bool parse_action(const std::string& s, UiAction& out);

// A chord is a key code from hal/keys.hpp — this machine has no separate
// modifier state, because Ctrl+<letter> already arrives as its own code.
std::string chord_name(int key);
bool parse_chord(const std::string& s, int& key);

struct Binding {
  int key = 0;
  UiAction action = UiAction::None;
};

class KeyBindings {
 public:
  // What the machine came with. Loading a file does not start from empty: a
  // file that names two keys must not silently unbind the other fourteen.
  static KeyBindings defaults();

  UiAction action_for(int key) const;
  void bind(int key, UiAction a);
  bool unbind(int key);
  const std::vector<Binding>& all() const { return bindings_; }

  std::string to_text() const;
  // Applied ON TOP of whatever this holds, so a file is a set of changes.
  Result<void> apply_text(const std::string& text);

 private:
  std::vector<Binding> bindings_;
};

// Where the bindings live, beside the scene and the face — the run directory,
// because the compositor is a different process from the console that edits it.
std::string key_bindings_path(const std::string& root = std::string());
// Where a console leaves a url for this machine's own web pane. Beside the
// keyboard and the scene, for the same reason: the process that asks is not
// the process that draws.
std::string run_browse_path(const std::string& root = std::string());
// Where the mind leaves what it wants drawn WITH: one directive per line —
// `color #ff2d95`, `width 8`, `on`, `off`, `clear`. Same run directory, same
// half-second poll, same reason as the two above: the side that picks the
// colour is a different process from the side holding the pixels, and a
// drawing surface whose ink is compiled in is a surface the machine cannot
// have an opinion about.
std::string run_draw_path(const std::string& root = std::string());

}  // namespace aios::userland
