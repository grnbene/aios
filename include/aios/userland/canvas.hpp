#pragma once
// The canvas: the machine's own lifecycle on the glass, with no windows. ONE
// painter for every display this OS has — the bare-metal framebuffer (virtio-gpu
// today) and a hosted screen — so what a person walks up to is the same machine
// wherever it happens to be running. It lays itself out for the size it is
// given: three columns on a wide display, two on a narrow one, one on a small
// one; and on a very dense panel it paints at half scale and upsamples, so the
// type stays legible at 4K instead of shrinking to a smear.
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include "aios/userland/machine_load.hpp"
#include "aios/core/ids.hpp"
#include "aios/userland/line_edit.hpp"

namespace aios { class Kernel; class Surface; }

namespace aios::userland {

class HardwareCompositor;
struct CanvasState;

// What only the substrate knows, handed in each frame rather than reached for
// from the painter. A hosted kernel fills the label; bare metal fills the rest.
struct CanvasFeed {
  uint64_t ticks = 0, tick_hz = 100, switches = 0;
  uint64_t pages_live = 0, pages_total = 0;
  uint64_t heap_kib = 0;
  uint64_t rx_frames = 0, tx_frames = 0;
  bool net_up = false;
  std::vector<std::string> tasks;   // one line each, already formatted
  std::string right_label;          // if set, the header's right-hand caption
};

// One line of the machine's history: something that came into existence.
struct Minted {
  std::string kind, id, note;
  // The whole 128 bits, as text. The panel prints the short form because that
  // is what fits, but a row a person can CLICK has to name the thing exactly —
  // twelve hex characters is a label, not an identity.
  std::string full;
  uint64_t seen_ms = 0;             // 0 = already here when the canvas opened
};

// What the workstation's two native applications are holding at this instant.
// The canvas has no windows, so it cannot show the editor or the studio; but a
// machine whose lifecycle screen says nothing about the file being edited or
// the query just run is two machines. The compositor fills this from the same
// window state the panes draw from, so the two surfaces cannot drift.
struct WorkstationState {
  bool has_editor = false;
  std::string editor_path;       // empty == nothing opened yet
  size_t editor_lines = 0;
  size_t editor_bytes = 0;
  bool editor_dirty = false;
  int editor_cursor_line = 0;
  size_t editor_tabs = 0;        // how many files the pane has open
  std::string editor_line;       // the text of the line the caret is on
  bool editor_selection = false;
  bool editor_cmdline = false;   // the command line has the keyboard, not the file
  std::string editor_status;     // what the editor said about the last thing it did
  std::string editor_lang;       // the grammar painting the open file
  bool editor_preview = false;   // showing it rendered rather than as source

  bool has_db = false;
  bool db_connected = false;
  size_t db_tables = 0;
  std::string db_selected;
  std::string db_grid;           // what the grid holds: a table, a query, a schema
  std::string db_last_query;
  std::string db_status;
  bool db_error = false;
  size_t db_rows = 0;
  size_t db_columns = 0;
};

// ---------------------------------------------------------------------------
// The desk: what a person may move, fold away, throw out and set up again.
// ---------------------------------------------------------------------------
// Every card and every panel on either face of the canvas is one of these. The
// screen used to be two hardcoded layouts — a stack-local array of six hero
// cards on the start page and a per-column-count if/else for the seven panels —
// so nothing on it could be picked up, and a card removed was a card recompiled
// out. A tile carries its own place, its own size and its own fate instead.
enum class TileKind : uint8_t {
  // The start page's face.
  HeroEditor, HeroDb, HeroWeb, HeroTensor, HeroHorizon, HeroBuild,
  Telemetry, ThoughtStream, MindAnswer, Apps,
  // The panel grid's face.
  ClockCalendar, Performance, Identities, Authority, Directives, Horizon, Brief,
  // What the machine did on its own authority, and how to put it back.
  Ledger, Rewind,
  // An application, on whichever face you are standing on. Every app this
  // machine can open is a url in ONE pane, and that pane lived only on the
  // workspace — so opening anything threw you off the face you were looking at.
  // This tile is that same pane, drawn here. It belongs to NEITHER face and
  // shows on both; it is the one kind the face filter does not apply to.
  AppPane,
  // One identity, opened from the glass. Like the application pane it belongs
  // to neither face and shows on whichever one you are standing on.
  Inspect,
  Count,
};

// Which face a kind belongs to: the start page (true) or the panel grid.
bool tile_is_start_page(TileKind kind);
// Which of the canvas's two faces is up for this state — the start page, or the
// desk of panels. The env this has always honoured is part of the answer.
bool desk_start_page(const CanvasState& st);
// The name that goes in a layout file, and the one that goes in a menu.
const char* tile_slug(TileKind kind);
const char* tile_menu_name(TileKind kind);

struct DeskTile {
  TileKind kind = TileKind::HeroEditor;
  int col = 0;          // which column it flows in
  int slot = 0;         // its order down that column, or across a band
  int span = 1;         // columns wide; span >= the face's column count is a band
  // What height it asks for: > 0 is that many pixels, < 0 is that percentage of
  // the body, 0 is "share what the column has left". The three cases are what
  // the two old layouts were already doing by hand.
  int height = 0;
  bool collapsed = false;
  bool hidden = false;
  // Unpinned from the grid: x/y/w/h are then absolute, in paint space, and the
  // tile floats over everything the grid placed.
  bool floating = false;
  int x = 0, y = 0, w = 0, h = 0;
};

// Where one tile ended up, and where its controls are. The painter draws these
// rects and the compositor hit-tests them, both out of canvas_layout(), so a
// tile that has been dragged somewhere stays clickable where it is drawn.
struct TileRect {
  TileKind kind = TileKind::HeroEditor;
  int index = -1;                   // into CanvasState::tiles
  int x = 0, y = 0, w = 0, h = 0;
  int header_h = 0;                 // the strip a press may grab it by
  int fold_x = 0, fold_y = 0;       // the collapse control
  int shut_x = 0, shut_y = 0;       // the remove control
  int ctl = 0;                      // both controls are ctl x ctl
  int grip = 0;                     // the bottom-right resize corner
  bool floating = false;
  bool contains(int px, int py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
  bool on_fold(int px, int py) const {
    return ctl > 0 && px >= fold_x && px < fold_x + ctl && py >= fold_y && py < fold_y + ctl;
  }
  bool on_shut(int px, int py) const {
    return ctl > 0 && px >= shut_x && px < shut_x + ctl && py >= shut_y && py < shut_y + ctl;
  }
  bool on_grip(int px, int py) const {
    return grip > 0 && px >= x + w - grip && px < x + w && py >= y + h - grip && py < y + h;
  }
};

// Every tile's place for one frame, plus the two things the compositor cannot
// work out for itself: which face is up, and what the paint was scaled by.
struct CanvasLayout {
  bool start_page = true;
  int scale = 1;                    // paint coordinate * scale == screen coordinate
  int iw = 0, ih = 0;               // the size the painter laid out for
  int cols = 1;
  int body_x = 0, body_y = 0, body_w = 0, body_h = 0;
  int gap = 0;        // between tiles stacked down a column
  int col_gap = 0;    // between the columns themselves
  int col_w = 0;      // one column, without its gap
  std::vector<TileRect> tiles;      // grid tiles first, then floating ones on top
  const TileRect* at(int px, int py) const {
    // Last drawn is topmost, so it is asked first.
    for (size_t i = tiles.size(); i-- > 0;) if (tiles[i].contains(px, py)) return &tiles[i];
    return nullptr;
  }
};

// The machine's three faces, in the order the view switcher offers them. They
// are one enum because a background, a switcher chip and a key binding all have
// to mean the same three things — the start page and the desk used to be one
// bool on the canvas and the workspace a separate bool on the compositor, so
// nothing could say "this view" without saying which of the two it meant.
enum class DeskView : uint8_t { StartPage = 0, Desk = 1, Workspace = 2, Count = 3 };

// ---------------------------------------------------------------------------
// The mind's provider and model, on the glass
// ---------------------------------------------------------------------------
// The same two answers the GUI's picker uses, from the same place: the preset
// table in shared/model-presets.cjs for which providers this fleet offers, and
// providers.cjs's listModels() for what a provider actually serves. Neither is
// written down here. A model list compiled into this painter would be a menu
// of names that may not work on the machine in front of you — and the live one
// already differs from the obvious guess (claude-cli serves a
// "claude-fable-5-1[1m]" that no hand-written list had).
std::string machine_conf_value(Kernel& k, const std::string& key);
// What engine this machine's mind is actually running, by its own name. The
// start page used to print this in a header of its own; the one top bar prints
// it now, so both faces report the same machine.
std::string bound_mind_name(Kernel& k);
// The accelerator this machine is actually running on, short enough for a bar:
// the first GPU in the fabric, by the name the device reports, with the
// backend prefix and the vendor's marketing words trimmed. Empty when the
// fabric holds nothing but CPUs — which is a true answer, and different from
// not looking.
std::string accelerator_brief(Kernel& k);

// One preset: a provider this machine can be put on, and the model it falls
// back to when it is chosen without one.
struct MindPreset {
  std::string provider, label, default_model;
};

// The two controls that say what mind this machine is running: a provider and
// a model, each a select. They were rows of chips — sixteen providers and
// however many models a provider serves, all on the glass at once, wrapping
// onto three lines. A list you are choosing ONE thing from is a select; chips
// are for a handful of options you want to see side by side.
//
// ONE function measures both, and the open list under whichever was clicked,
// so the painter draws these rects and the compositor hit-tests them.
struct MindSelect {
  int y = 0, h = 0;
  int prov_x = 0, prov_w = 0;
  int model_x = 0, model_w = 0;
  std::string provider, model;
  // The open list, if one is. `which` is 1 for the provider's, 2 for the
  // model's, 0 for none.
  int which = 0;
  int list_x = 0, list_y = 0, list_w = 0, row_h = 0;
  std::vector<std::string> list;
  bool on_provider(int px, int py) const {
    return py >= y && py < y + h && px >= prov_x && px < prov_x + prov_w;
  }
  bool on_model(int px, int py) const {
    return py >= y && py < y + h && px >= model_x && px < model_x + model_w;
  }
  // Which row of the open list this point is in, or -1.
  int hit_row(int px, int py) const {
    if (!which || list.empty()) return -1;
    if (px < list_x || px >= list_x + list_w) return -1;
    const int i = (py - list_y) / (row_h > 0 ? row_h : 1);
    if (py < list_y || i < 0 || i >= (int)list.size()) return -1;
    return i;
  }
};
MindSelect mind_select(HardwareCompositor& S, Kernel& k, const CanvasState& st, int box_x, int box_w, int row_y);

// What the painter remembers between frames.
struct CanvasState {
  std::vector<Minted> ledger;
  // Everything the sweep has already seen. A vector scanned linearly for every
  // id, every frame, is quadratic in the size of the machine's own store, and
  // the sweep runs on the frame loop -- see CanvasState::sweep.
  std::unordered_set<Id128, Id128Hash> known_ids;
  uint64_t frame = 0;
  bool primed = false;
  // The desk — authority, the ledger, the rewind, the standing orders, the
  // brief — is what this machine has that nothing else does. It used to be
  // hidden behind an unlabelled key while the front page led with an editor, a
  // SQL client and a browser: three things it can only lose at.
  bool hide_panels = false;
  bool collapsed_panels = false;
  bool bare_scene = false;
  // One background per view — 0: Beach & Moon, 1: Cyber Neural Core, 2: Quantum
  // Plasma Orb, 3: pyramid and 4: composed on the display path. This was a
  // single int for the whole machine, so the desk and the start page could
  // never be told apart by what was behind them; a background belongs to the
  // face you are looking at, not to the machine.
  int scene_of[(size_t)DeskView::Count] = {0, 0, 0};
  int scene(DeskView v) const { return scene_of[(size_t)v]; }
  void set_scene(DeskView v, int s) { scene_of[(size_t)v] = s; }
  // And one per windowed section. A section is a desktop: it carries its own
  // wallpaper, the way every other machine's virtual desktops do. This used to
  // be a single scene for "the workspace" — every section wore the same one —
  // and the workspace painter ignored it anyway, so choosing a background
  // there changed nothing at all.
  static constexpr int kSections = 10;
  int section_scene[kSections] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  // Every view at once: what a caller that has no view — a bench, the metal
  // boot's -scene flag — actually means when it names a scene.
  void set_every_scene(int s) { for (int& v : scene_of) v = s; }
  // Set by the display path when it is computing the background in a shader:
  // the painter then leaves those pixels clear and draws only the interface.
  bool background_on_gpu = false;
  // Whether the machine will show a scene it composed itself (asked for in
  // words — "an ocean at dusk" — and turned into geometry the shader draws)
  // rather than one of the three fixed presets. Off falls back to whichever
  // preset the view was already on: a person who does not want the machine
  // painting new scenery can say so once instead of every time it offers.
  bool ai_scenes_enabled = true;
  // Which scene the painter actually settled on this frame — it can come from
  // a file rather than the field above, and the shader has to draw the SAME one
  // the interface was laid out over.
  int scene_effective = 0;
  // What hour the shoreline (scene 0) is at: 0 midnight, 0.25 dawn, 0.5 midday,
  // 0.75 dusk. Night is what that scene always was, so night is the default and
  // a machine that is never told otherwise looks exactly as it did.
  float scene_tod = 0.0f;
  // What the machine is doing to itself, sampled once a second off the frame
  // loop -- see HardwareCompositor::render_canvas. The emblem reads its STATE
  // (one step, no numbers); the top bar prints the figures, and only when the
  // person has turned them on. Off by default: a desktop that shouts numbers
  // at you unasked is a dashboard, not a desktop.
  MachineLoad machine_load;
  bool show_machine_load = false;
  // A scene the machine composed, as the text it was written as. The canvas
  // does not draw it — the shader does — so this is carried, not interpreted:
  // the generation rises when the text changes, and the display path uploads
  // it then and not every frame.
  std::string composed_text;
  uint64_t composed_generation = 0;

  // What the machine said to the thing that was asked HERE. The landing page
  // has its own prompt, and submitting from it used to write the answer into
  // the shell pane's transcript — a surface on the other face, which nobody
  // looking at the landing page can see. So you asked, the prompt cleared, and
  // nothing came back. This is that answer, on the face it was asked from.
  // What is installed, read off the machine's own store. Refreshed on a clock
  // rather than every frame: it is a store query, not a pixel.
  struct AppEntry {
    std::string name;
    std::string url;
    bool running = false;
  };
  std::vector<AppEntry> installed_apps;
  uint64_t apps_checked_ms = 0;
  bool apps_primed = false;

  // What this machine may be put on, and what the provider it is on serves.
  // Filled off the paint thread — listing a CLI provider's models runs that
  // CLI, which is far too slow to do inside a frame.
  std::vector<MindPreset> mind_presets;
  std::vector<std::string> mind_models;
  std::string mind_models_provider;      // which provider mind_models is for
  bool mind_lists_loading = false;
  bool mind_lists_primed = false;
  // Which select is showing its list: 0 none, 1 provider, 2 model.
  int mind_select_open = 0;

  std::string answer_asked;
  std::vector<std::string> answer_lines;
  bool answer_running = false;
  // Negative scrolls back through the answer; 0 is pinned to the newest line,
  // which is where a person watching one arrive wants to be.
  int answer_scroll = 0;

  // ---------------------------------------------------------------------------
  // Acts a person can take from the desk. Both are destructive, so both are
  // ARMED by one click and taken by a second on the same row — the grammar the
  // power panel already uses. Any other click clears the arm.
  // ---------------------------------------------------------------------------
  // One row the painter drew that the compositor can hit-test. Same contract as
  // TileRect: the painter fills these in as it draws, so a row is clickable
  // exactly where it is on the glass.
  struct ActionRow {
    int x = 0, y = 0, w = 0, h = 0;
    std::string subject;      // the capability, or the checkpoint's path
    std::string label;        // what a person is about to do
  };
  std::vector<ActionRow> revoke_rows;
  std::vector<ActionRow> rewind_rows;
  // One row per installed application. There was no way at all to reach an
  // app from the glass: `app open <name>` had to be typed into the shell pane,
  // which means the launcher for this machine's applications was a command
  // line. These rows are the launcher.
  std::vector<ActionRow> app_rows;
  // One row per identity on the Identities panel. Every line on that panel is a
  // 128-bit id of a kind this kernel knows — an entity, a state, a channel, a
  // capability, something in the store — and until now all of them were ink.
  // Looking at a thing destroys nothing, so these are ONE click.
  std::vector<ActionRow> identity_rows;
  std::string armed;          // the subject armed for its second click
  bool contains_row(const std::vector<ActionRow>& rows, const std::string& s) const {
    for (const auto& r : rows) if (r.subject == s) return true;
    return false;
  }

  // ---------------------------------------------------------------------------
  // The inspector: one identity, and what the kernel knows about it. This is
  // the thing a machine whose furniture is authority and provenance can do and
  // nothing else can — the id on the glass is the handle, not a label beside
  // one. Read by the painter, filled from the kernel on a clock rather than
  // per frame: it walks registries and the object store.
  std::string inspect_kind;          // entity | capability | state | channel | knows
  std::string inspect_id;            // the whole id, as text
  std::string inspect_title;         // what it is, in one line
  // A row of the inspector. When it names another identity it carries it, and
  // the row becomes a handle in turn — an entity to what it produced, an object
  // to what it was derived from. That walk IS the machine's own history; a card
  // that printed the ids and stopped would be a report about it.
  struct InspectRow {
    std::string label, value;
    bool head = false;
    std::string id, kind;            // empty when the row names nothing
  };
  std::vector<InspectRow> inspect_rows;
  uint64_t inspect_checked_ms = 0;
  std::string inspect_built;         // the id these rows were built for

  // A checkpoint that exists on disk: a moment this machine can be put back to.
  struct Snapshot {
    std::string path;         // what restore() is given
    std::string entity;       // whose it is, short form
    std::string label;        // that entity's label, if it is still here
    uint64_t taken_ns = 0;
    uint64_t bytes = 0;
    int ordinal = 0;
  };
  std::vector<Snapshot> snapshots;
  uint64_t snapshots_checked_ms = 0;
  // The two native applications, as the windowed workspace holds them THIS
  // frame. The compositor refreshes it from HardwareCompositor::workstation()
  // before every canvas frame; a surface with no workspace behind it (the
  // `canvas-dump` command, the bare-metal canvas) leaves it empty and the
  // canvas says so rather than guessing.
  WorkstationState apps;
  // The beach scene is a still — nothing in it moves with the clock — so it is
  // painted once for a display size and kept here; later frames are a copy.
  // Invalidated by the size changing, which is the only thing that can change it.
  std::vector<uint32_t> beach_still;
  int beach_w = 0, beach_h = 0;
  // A dense panel is painted at half scale into this scratch buffer and
  // upsampled into the real frame. It is kept between frames: a 4K display
  // means an 8 MiB buffer, and allocating and faulting that in on EVERY frame
  // is a millisecond of the frame budget spent on the allocator.
  std::vector<uint32_t> scale_scratch;
  // The quantum orb's colour is a pure function of its plasma value, and its
  // halo a pure function of the squared distance from the centre — neither
  // moves with the clock. Both are tabulated on the first frame and looked up
  // on every frame after, which is what took six transcendentals per pixel out
  // of the orb scene. Held here rather than in a function-local static: bare
  // metal has no guard variables.
  std::vector<uint32_t> orb_plasma_lut;
  std::vector<uint32_t> orb_glow_lut;
  // The cyber core's brightening is a pure function of the squared distance
  // from the centre: packed per-channel adds, indexed by d² >> 4.
  std::vector<uint32_t> cyber_core_lut;
  // The start page's hero glow breathes: the pulse scales its radius and its
  // amplitude by the same factor, so the factor divides straight out of the
  // square root and what is left — the pixel's distance from the emblem in
  // unit-radius terms — does not move with the clock at all. Tabulated once
  // for one quadrant (the ellipse is symmetric about both axes) and looked up
  // after; a quarter million square roots and divisions a frame become two
  // multiplies and a compare.
  std::vector<float> glow_lut;
  // The thought stream is the TAIL of a log the resident appends to for as
  // long as the machine is up. Reading and splitting the whole file inside the
  // paint cost more than everything else on the start page put together, and
  // got worse every hour the machine ran. The tail is kept here and re-read
  // four times a second — faster than a person reads a line.
  std::vector<std::string> log_tail;
  uint64_t log_checked_ms = 0;
  bool log_primed = false;
  // Which scene the run directory asks for, and when we last looked. The
  // scene file is a filesystem read, and doing one per frame is a syscall in
  // the middle of the paint; a person switching scenes still sees it inside
  // half a second.
  int scene_file_value = -1;
  // The last value that file was ACTED on. The run directory names a scene for
  // the MACHINE and carries no view with it, so a change is applied to every
  // view — whichever one you are in front of shows what was asked for. Only
  // when it CHANGES, so a background picked by hand afterwards is not
  // overwritten half a second later.
  int scene_file_applied = -1;
  // Whether a scene named on the command line (--scene/--beach/--cyber, which
  // set AOS_DESKTOP_SCENE) has been honoured yet. It is a person asking, so it
  // is applied ONCE at startup and then gets out of the way; re-applying it per
  // frame made the machine deaf to every request that came after.
  bool env_scene_applied = false;
  // Whether the run directory has been LOOKED AT yet this run. The first look
  // takes a BASELINE and applies nothing: on a restart the scene is whatever
  // the saved desk says, because that is where the last explicit choice went.
  // This cannot be inferred from scene_file_request being empty — a machine
  // that boots with no scene file leaves it empty, and treating every later
  // look as "the first" made the first real `display show` get swallowed.
  bool scene_file_baselined = false;
  // The whole of the last request read out of the run directory — the scene's
  // name AND the serial written beside it. Compared as text, so asking for the
  // scene the machine is already on is a request like any other.
  std::string scene_file_request;
  // Same for the face the run directory asks for. `display show <name>` writes
  // it once; re-applying it twice a second means the machine cannot be moved
  // off that face at all — the switcher, the space bar and the context menu all
  // set it and it snapped back within half a second, which reads as a desktop
  // that ignores you.
  std::string face_file_applied;
  uint64_t scene_checked_ms = 0;
  // The frame rate the machine is actually achieving, measured from the clock
  // between consecutive paints and smoothed over about a second. The dock used
  // to print the constant "60.0 FPS" whatever the frame cost was.
  // What the person has typed into the start page's prompt, and whether that
  // prompt has the keyboard. The prompt used to paint a fixed placeholder and a
  // blinking caret while accepting nothing at all -- a text field that was a
  // picture of a text field.
  EditLine prompt;
  bool prompt_focused = false;
  // Which view the last frame painted, so that arriving on the start page can
  // hand its one text field the keyboard.
  bool start_page_shown = false;
  uint64_t fps_last_ms = 0;
  float fps = 0.0f;
  float cpu_history[4][60] = {{0}};
  float mem_history[60] = {0};
  uint8_t history_idx = 0;
  // When the sparkline last took a sample. The history advances on a clock, not
  // once per frame -- at frame rate its 60 slots were a two-second window that
  // scrolled faster the better the machine was running.
  uint64_t perf_sampled_ms = 0;
  // The desk itself: every card and panel on both faces, with where a person
  // has put it. Seeded on the first frame to exactly the layout the two
  // hardcoded ones drew, so a machine that has never been rearranged looks the
  // same as it always did.
  std::vector<DeskTile> tiles;
  bool tiles_seeded = false;
  // Read the saved layout once, and write it back when something moves. Both
  // happen inside the paint, which is where a Kernel is at hand.
  bool layout_loaded = false;
  bool layout_dirty = false;
  // What the pointer is doing to a tile. `drag_tile` indexes `tiles`; a press
  // that never moves far enough stays a click on the card, which is what the
  // six launcher cards have always been.
  int drag_tile = -1;
  bool drag_resize = false;
  bool drag_moved = false;
  int drag_grab_x = 0, drag_grab_y = 0;     // pointer, in paint space, at the press
  int drag_off_x = 0, drag_off_y = 0;       // pointer minus the tile's corner
  int drag_from_w = 0, drag_from_h = 0;     // the tile's size at the press
  int drag_at_x = 0, drag_at_y = 0;         // where the pointer is now
  // Notice everything the kernel now holds; what is new lights up.
  void sweep(Kernel& k, uint64_t now_ms);
};

// The start page's chrome, MEASURED. Every one of these rects is sized from
// the label that goes in it, with the face that draws it — a pill whose width
// was picked by hand fits its text only by luck, and "AIOS HYPERVISOR 2.4" had
// already outgrown its 210px pill by a pixel. The painter draws these rects and
// HardwareCompositor::input_mouse_button hit-tests them, both through this one
// function, so a control that grew to fit its label stays clickable where it is
// actually drawn.
struct CanvasChrome {
  int topbar_h = 40;
  int brand_x = 0, brand_y = 0, brand_w = 0, brand_h = 0;
  int rst_x = 0, rst_y = 0, rst_w = 0, rst_h = 0;
  int ask_w = 0, ask_h = 0;
  // Where the bar's centre content may start: clear of the brand pill.
  int left_edge = 0;
  int brand_right() const { return brand_x + brand_w; }
};
CanvasChrome canvas_chrome(HardwareCompositor& surface, int iw);

// Where every tile is, for a display of this size and a desk in this state.
// PURE: it paints nothing and changes nothing, so the painter and the mouse
// handlers can both ask it and cannot disagree. `st` is const on purpose.
CanvasLayout canvas_layout(HardwareCompositor& surface, uint32_t W, uint32_t H, const CanvasState& st);

// What the pointer does to a tile. The compositor owns the events; the grid
// arithmetic lives with the layout, so a drop lands where the hint said.
void desk_press(CanvasState& st, const TileRect& r, int px, int py, bool resize);
// The layout is not an argument any more: a drag no longer asks the grid
// anything, it just follows the pointer.
void desk_drag_motion(CanvasState& st, int px, int py);
// Commit the release. Returns the tile a plain CLICK landed on — a press that
// never travelled — or -1 when the release ended a real drag or resize.
int desk_release(CanvasState& st, const CanvasLayout& L);

void desk_fold(CanvasState& st, int index, bool folded);
void desk_hide(CanvasState& st, int index);
void desk_show(CanvasState& st, TileKind kind);
void desk_unpin(CanvasState& st, int index, const TileRect& at);
void desk_pin(CanvasState& st, int index);

// Put the desk back to the arrangement the machine ships with.
void seed_desk_tiles(CanvasState& st);
// The saved layout, as text and back. Empty text leaves the desk as seeded.
std::string desk_layout_text(const CanvasState& st);
void desk_layout_parse(CanvasState& st, const std::string& text);
// Write it back if anything moved. The canvas does this at the end of every
// paint; the compositor calls it too, because a background chosen on the
// WORKSPACE is a change to the same file and that face has no canvas paint.
void desk_layout_save(CanvasState& st, Kernel& k);

// Where the console writes what the display is to show, and where the
// compositor reads it. ONE definition, deliberately: the writer derived this
// path from the kernel root while the reader used a literal
// "/home/arch/aos/run", so on every machine that is not that host the console
// composed a scene into a file nothing was watching. `display describe` reads
// the WRITER's copy, so it confirmed the picture happily and the glass never
// moved — which is what a mind then reports as "it's on your display now".
std::string run_dir(Kernel& k);

// Paint one frame into `out`, which becomes W×H ARGB.
void paint_canvas(HardwareCompositor& surface, std::vector<uint32_t>& out, uint32_t W, uint32_t H,
                  Kernel& k, Surface& console, const CanvasFeed& feed, uint64_t now_ms, CanvasState& st);

// One background, painted into `out` at W x H. The three scenes the CPU path
// can draw are here and nowhere else, so the workspace's ground and the canvas's
// are the same wallpaper rather than two paintings of the same name. A scene
// this painter has no brush for (the pyramid, a composed one -- both of which
// the display path draws) falls back to the beach.
void paint_scene(std::vector<uint32_t>& out, int W, int H, uint64_t now_ms, CanvasState& st, int scene);


// The header caption a hosted kernel can offer: uptime, entities, objects.
CanvasFeed hosted_canvas_feed(Kernel& k);

#if !defined(AIOS_METAL)
// A frame to disk as a P6 PPM — how a display is checked without a display.
bool write_ppm(const std::string& path, const std::vector<uint32_t>& argb, uint32_t w, uint32_t h);
#endif

}  // namespace aios::userland
