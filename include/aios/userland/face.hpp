#pragma once
// A face: what a dream looks like and does, in this machine's own words.
//
// Not a web page. A face is a small document the mind writes — widgets in an
// indented tree, reactive bindings, and rules — that the compositor lays out
// and paints itself, every frame, from the machine's own state and store. There
// is no DOM, no cascade, no script engine between the description and the
// pixels: a `table item` is a table of the store's `item` records, laid out by
// this file with columns that fit, and it cannot draw one word over another
// because the layout never gives two words the same pixels.
//
//     face scanner
//     title "Warehouse scanner"
//     kind item key=sku
//     let items = list(item)
//     state mode = "in"
//     row gap=12
//       tile label="Items" value=count(items)
//       tile label="On hand" value=sum(items, qty)
//     row gap=8
//       field id=sku placeholder="Scan or type a SKU" focus enter=scan
//       field id=qty value="1" width=80 enter=scan
//       button "Receive" action=scan primary
//     table items columns="sku:SKU name:Name qty:On hand:right" rows=12 empty="nothing on file"
//     on scan:
//       if sku == "": say "scan a SKU first"; focus sku
//       else:
//         put item {sku: sku, qty: num(qty)}
//         set sku = ""
//         focus sku
//
// The whole language is in face_language_reference(); the mind writes against
// that text, and `face look` says exactly what a face would draw and what it
// got wrong before anyone sees it.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aios/userland/web.hpp"   // HostBindings, FontKey

namespace aios::userland {

namespace face {

// ---- values -----------------------------------------------------------------

struct Value {
  enum class K : uint8_t { Null, Num, Str, Bool, List, Rec } k = K::Null;
  double n = 0;
  std::string s;
  bool b = false;
  std::shared_ptr<std::vector<Value>> list;
  std::shared_ptr<std::vector<std::pair<std::string, Value>>> rec;

  static Value null() { return Value{}; }
  static Value num(double x) { Value v; v.k = K::Num; v.n = x; return v; }
  static Value str(std::string x) { Value v; v.k = K::Str; v.s = std::move(x); return v; }
  static Value boolean(bool x) { Value v; v.k = K::Bool; v.b = x; return v; }
  static Value make_list() { Value v; v.k = K::List; v.list = std::make_shared<std::vector<Value>>(); return v; }
  static Value make_rec() { Value v; v.k = K::Rec; v.rec = std::make_shared<std::vector<std::pair<std::string, Value>>>(); return v; }
  const Value* field(const std::string& name) const;
  void set_field(const std::string& name, Value v);
  bool truthy() const;
  std::string text() const;      // how it reads on the glass
  double number() const;
};

std::string to_json(const Value& v);
Value from_json(const std::string& text);

// ---- the document -----------------------------------------------------------

struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Stmt;
using StmtP = std::shared_ptr<Stmt>;

struct Widget {
  std::string type;                                  // row column grid panel text tile table list field button gauge spacer paint
  std::string label;                                 // the positional string, if any
  std::vector<std::pair<std::string, std::string>> attrs;   // as written
  std::vector<std::pair<std::string, ExprP>> exprs;         // attribute values that are expressions
  std::vector<std::string> words;                    // bare positional tokens (grid tracks)
  std::vector<std::shared_ptr<Widget>> children;
  int line = 0;
  std::string attr(const std::string& k, const std::string& def = {}) const;
  bool flag(const std::string& k) const;
  int attr_int(const std::string& k, int def) const;
};

struct TypeField {
  std::string name;
  std::string type_name;   // String, Num, Int, Bool, UUID, Currency, Timestamp, List, Rec, Enum
  bool required = true;
  std::vector<std::string> enum_values;
};

struct TypeDef {
  std::string name;
  std::vector<TypeField> fields;
  int line = 0;
};

struct ComponentDef {
  std::string name;
  std::vector<std::pair<std::string, ExprP>> params;  // param name -> default expr (if any)
  std::vector<std::shared_ptr<Widget>> body;
  int line = 0;
};

struct Handler {
  std::string event;     // load frame key <action> task
  std::string key;       // for `on key <Key>`
  std::vector<StmtP> body;
};

struct Face {
  std::string name;
  std::string title;
  std::vector<std::pair<std::string, ExprP>> lets;    // reactive, recomputed every frame in order
  std::vector<std::pair<std::string, ExprP>> states;  // persistent, initial value
  std::vector<std::pair<std::string, std::string>> kinds;  // `kind item key=sku`: a kind with identity — list() reads current rows
  std::vector<TypeDef> types;                         // structural schemas
  std::vector<ComponentDef> components;               // user-defined components
  std::vector<std::string> imports;                   // imported modules/files
  std::vector<Handler> handlers;
  std::vector<std::shared_ptr<Widget>> widgets;
  std::vector<std::string> errors;                    // what could not be understood, with line numbers
  std::string source;
};

Face parse_face(const std::string& source);

// ---- the runtime ------------------------------------------------------------

struct PaintCmd {
  enum class K : uint8_t { Clear, Rect, Outline, Circle, Line, Text, Poly, Ellipse, Curve, Grad, Tensor } k = K::Rect;
  double a = 0, b = 0, c = 0, d = 0;
  double e = 0;                  // a stroke's width; a gradient's direction
  uint32_t color = 0xFFFFFFFF;
  uint32_t color2 = 0xFFFFFFFF;  // the far end of a gradient
  std::vector<float> pts;        // poly: x,y,x,y,…
  std::string text;
  std::string hit_id;            // ID for canvas hit-testing
  std::string hit_action;        // action triggered on clicking this shape
  std::vector<double> tensor_data; // tensor matrix/data
  int tensor_w = 0, tensor_h = 0;
  int px = 15;
};

// What one frame's layout put where. Kept for hit-testing and for the look.
struct Placed {
  const Widget* w = nullptr;
  int x = 0, y = 0, width = 0, height = 0;
  std::string text;        // what was drawn, if text
  bool cut = false;        // the text was cut with an ellipsis
  bool box = false;        // the widget's whole control box (a field, a button, the paint), for hit-testing
  int px = 0;
  std::string hit_action;  // action on hit
};

class Runtime {
 public:
  explicit Runtime(Face f);
  const Face& face() const { return face_; }

  // The machine behind the face: store, storage, organs, clock, log.
  void bind(HostBindings h);

  // Run `on load` (once), restoring persisted state first.
  void load();
  // One frame of the face's own time: lets recomputed, `on frame` run, then
  // the layout for a pane this size. Paint afterwards with paint().
  void frame(double dt_ms, int width, int height);
  // Paint the last laid-out frame into a buffer at an offset.
  void paint(std::vector<uint32_t>& buf, int buf_w, int buf_h, int ox, int oy) const;

  // Input & Pointer Pipeline.
  void click(int x, int y);
  void mouse_move(int x, int y, int dx = 0, int dy = 0, uint32_t buttons = 0);
  void mouse_drag(int x, int y, int dx, int dy);
  void mouse_wheel(int dx, int dy);
  void gesture(const std::string& type, double val1 = 0.0, double val2 = 0.0);
  void type(const std::string& text);
  bool key(const std::string& name, bool down);   // true when the face took it
  // Run the `on <action>:` handlers, as a button or a field's Enter would.
  void run_action(const std::string& action);
  bool has_focus() const { return !focus_.empty(); }
  bool is_pane_focused() const { return pane_focused_; }
  void on_focus_change(bool focused);
  void blur();
  void focus();
  void rehydrate_viewport(size_t limit = 30);
  void fault_in_scroll(size_t offset, size_t count);
  // Whether this face has an `on frame` rule at all. The look asks before it
  // reports that nothing moved: a still face is a still face, not a fault.
  bool has_frame_rule() const;

  // Channels, Async & Streaming
  void emit_channel(const std::string& chan, Value val);
  Value poll_channel(const std::string& chan);
  Value drain_channel(const std::string& chan);
  bool has_channel_data(const std::string& chan) const;

  // Temporal Time-Travel & History
  void snapshot_state(const std::string& label = "");
  bool undo_state();
  bool redo_state();
  bool can_undo() const;
  bool can_redo() const;
  size_t history_len() const;
  void history_clear();

  // Perception & multimodal grounding
  std::string perceive_tree_json() const;
  std::vector<std::string> lint() const;

  // AST Hot-Patching & Self-Modification
  bool hot_patch(const std::string& new_source);

  // What happened, for a status line and for the look.
  const std::vector<std::string>& faults() const { return faults_; }
  std::vector<std::string> take_faults() { auto f = faults_; faults_.clear(); return f; }
  const std::string& message() const { return message_; }
  const std::vector<Placed>& placed() const { return placed_; }
  const std::vector<PaintCmd>& paint_cmds() const { return paint_; }
  // The face as lines of text, the way a console would show it.
  std::vector<std::string> as_text() const;
  // Everything the look step reports: parse errors, faults, cut text, glyph gaps, an empty face.
  std::vector<std::string> inspect() const;
  // Which kinds the face read while it ran, and how many rows each returned.
  std::vector<std::pair<std::string, size_t>> reads() const;
  bool storage_dirty() const { return dirty_; }
  void flush_storage();
  // Introspection & Universal Inspector support
  std::vector<std::pair<std::string, std::string>> state_vars() const;
  std::vector<std::pair<std::string, std::string>> let_vars() const;
  std::vector<std::pair<std::string, std::string>> field_vars() const;
  std::vector<std::pair<std::string, size_t>> kind_vars() const;
  void set_state_var(const std::string& name, const std::string& val);
  void set_field_var(const std::string& name, const std::string& val);
  int frame_count() const;
  double last_dt_ms() const;
  const std::string& source_text() const { return face_.source; }
  const std::string& current_focus() const { return focus_; }

  struct Impl;   // the evaluator and the layout in face.cpp work on it directly

 private:
  std::shared_ptr<Impl> impl_;
  Face face_;
  HostBindings host_;
  std::vector<std::string> faults_;
  std::string message_;
  std::vector<Placed> placed_;
  std::vector<PaintCmd> paint_;
  std::string focus_;
  bool dirty_ = false;
  bool loaded_ = false;
  bool pane_focused_ = true;
  int last_w_ = 0, last_h_ = 0;
};

// The language, for the mind: every widget, attribute, function and verb, with
// an example. Measured against this file, not remembered.
const char* face_language_reference();

}  // namespace face
}  // namespace aios::userland
