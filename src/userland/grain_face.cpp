// Face widgets for grain programs: `ui.column`, `ui.row`, `ui.text`,
// `ui.button`, `ui.field`, `ui.table`, `ui.tile`, and `ui.pane` to show them.
//
//     let p = ui.pane({"title": "Stock", "width": 640, "height": 420, "view": fn () {
//       return ui.column([
//         ui.row([ui.tile("Items", len(items)), ui.tile("Late", late, {"accent": "warn"})]),
//         ui.row({"gap": 8}, [
//           ui.field("sku", {"placeholder": "Scan or type a SKU", "focus": true, "on_enter": receive}),
//           ui.button("Receive", receive, {"primary": true})]),
//         ui.table(items, {"columns": "sku:SKU, name:Name, qty:On hand:right"})])
//     }})
//     run_panes()
//
// A builder returns a plain map ({"widget": "button", ...}) a program can keep,
// pass around or print. The pane calls `view` every frame and lays out what it
// returns with face (face.cpp): the same layout and painting as the faces on
// the aios desktop, from grain values. What is typed into a field stays across
// frames by the field's id; a button's or a field's Enter calls the grain
// function it was given, with the pane's field texts as a map.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "aios/core/json.hpp"
#include "aios/userland/face.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

namespace fc = aios::userland::face;

// ---- the builders ----------------------------------------------------------

std::shared_ptr<MapObj> as_map(const Value& v) {
  return v.type == Type::Map && v.obj ? std::static_pointer_cast<MapObj>(v.obj) : nullptr;
}

bool is_callable(const Value& v) { return v.type == Type::Fn || v.type == Type::Native; }

// A widget map: {"widget": type, "opts": {...}} plus what the type carries.
std::shared_ptr<MapObj> widget_map(const char* type, const Value& opts) {
  auto m = std::make_shared<MapObj>();
  m->put("widget", Value::string(type));
  m->put("opts", opts.type == Type::Map ? opts : Value::object(Type::Map, std::make_shared<MapObj>()));
  return m;
}

bool want_opts(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Nil || v.type == Type::Map) return true;
  error = std::string(who) + "() wants its options as a map, got a " + type_name(v.type);
  return false;
}

bool want_widgets(const Value& v, std::string& error, const char* who) {
  if (!want_list(v, error, who)) return false;
  for (const auto& c : v.list_items()) {
    auto m = as_map(c);
    if (!m || !m->find("widget")) {
      error = std::string(who) + "() wants a list of widgets (ui.text, ui.button, ...), and one is a " + type_name(c.type);
      return false;
    }
  }
  return true;
}

// column(children), column(children, options), or column(options, children); row and card the same.
Value box(const char* type, std::vector<Value>& args, std::string& error) {
  const std::string who = std::string("ui.") + type;
  Value opts = Value::nil();
  Value kids = Value::nil();
  if (args.size() == 2) {
    if (args[0].type == Type::List) {
      kids = args[0];
      opts = args[1];
    } else {
      opts = args[0];
      kids = args[1];
    }
  } else if (!args.empty()) {
    kids = args[0];
  }
  if (!want_opts(opts, error, who.c_str()) || !want_widgets(kids, error, who.c_str())) return Value::nil();
  auto m = widget_map(type, opts);
  m->put("children", kids);
  return Value::object(Type::Map, m);
}

Value bi_ui_column(Vm&, std::vector<Value>& args, std::string& error) { return box("column", args, error); }
Value bi_ui_row(Vm&, std::vector<Value>& args, std::string& error) { return box("row", args, error); }
Value bi_ui_card(Vm&, std::vector<Value>& args, std::string& error) { return box("card", args, error); }
Value bi_ui_divider(Vm&, std::vector<Value>& args, std::string& error) {
  const Value opts = arg_or(args, 0);
  if (!want_opts(opts, error, "ui.divider")) return Value::nil();
  return Value::object(Type::Map, widget_map("divider", opts));
}

// text(words) or text(words, options): size, dim, bold, color, ...
Value bi_ui_text(Vm&, std::vector<Value>& args, std::string& error) {
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.text")) return Value::nil();
  auto m = widget_map("text", opts);
  m->put("label", Value::string(args[0].to_text()));
  return Value::object(Type::Map, m);
}

// button(label, on_click) or button(label, on_click, options): primary, ...
Value bi_ui_button(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.button")) return Value::nil();
  if (!is_callable(args[1])) {
    error = std::string("ui.button() wants a function to call when it is pressed, got a ") + type_name(args[1].type);
    return Value::nil();
  }
  const Value opts = arg_or(args, 2);
  if (!want_opts(opts, error, "ui.button")) return Value::nil();
  auto m = widget_map("button", opts);
  m->put("label", args[0]);
  m->put("on_click", args[1]);
  return Value::object(Type::Map, m);
}

// field(id) or field(id, options): placeholder, value, width, focus, on_enter.
Value bi_ui_field(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.field")) return Value::nil();
  if (args[0].str().empty()) { error = "ui.field() wants an id: the name its text is kept under"; return Value::nil(); }
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.field")) return Value::nil();
  if (auto o = as_map(opts)) {
    if (Value* e = o->find("on_enter"); e && !is_callable(*e)) {
      error = std::string("ui.field(): on_enter wants a function, got a ") + type_name(e->type);
      return Value::nil();
    }
  }
  auto m = widget_map("field", opts);
  m->put("id", args[0]);
  return Value::object(Type::Map, m);
}

// switch(id, [label], [options]): checked, on_change
Value bi_ui_switch(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.switch")) return Value::nil();
  const std::string id = args[0].str();
  std::string label = id;
  Value opts = Value::nil();
  if (args.size() >= 2) {
    if (args[1].type == Type::Str) {
      label = args[1].str();
      opts = arg_or(args, 2);
    } else if (args[1].type == Type::Map) {
      opts = args[1];
    }
  }
  if (!want_opts(opts, error, "ui.switch")) return Value::nil();
  auto m = widget_map("switch", opts);
  m->put("id", Value::string(id));
  m->put("label", Value::string(label));
  return Value::object(Type::Map, m);
}

// checkbox(id, [label], [options]): checked, on_change
Value bi_ui_checkbox(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.checkbox")) return Value::nil();
  const std::string id = args[0].str();
  std::string label = id;
  Value opts = Value::nil();
  if (args.size() >= 2) {
    if (args[1].type == Type::Str) {
      label = args[1].str();
      opts = arg_or(args, 2);
    } else if (args[1].type == Type::Map) {
      opts = args[1];
    }
  }
  if (!want_opts(opts, error, "ui.checkbox")) return Value::nil();
  auto m = widget_map("checkbox", opts);
  m->put("id", Value::string(id));
  m->put("label", Value::string(label));
  return Value::object(Type::Map, m);
}

// slider(id, min, max, [options]): value, step, label, on_change
Value bi_ui_slider(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.slider")) return Value::nil();
  if (args.size() < 3 || !args[1].is_number() || !args[2].is_number()) {
    error = "ui.slider() wants (id, min, max, [opts])";
    return Value::nil();
  }
  const Value opts = arg_or(args, 3);
  if (!want_opts(opts, error, "ui.slider")) return Value::nil();
  auto m = widget_map("slider", opts);
  m->put("id", Value::string(args[0].str()));
  m->put("min", args[1]);
  m->put("max", args[2]);
  return Value::object(Type::Map, m);
}

// select(id, options_list, [options]): value, label, on_change
Value bi_ui_select(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.select")) return Value::nil();
  if (args.size() < 2 || !want_list(args[1], error, "ui.select")) return Value::nil();
  const Value opts = arg_or(args, 2);
  if (!want_opts(opts, error, "ui.select")) return Value::nil();
  auto m = widget_map("select", opts);
  m->put("id", Value::string(args[0].str()));
  m->put("options", args[1]);
  return Value::object(Type::Map, m);
}

// date_picker(id, [options]) or date(id, [options]): value, mode, min, max, presets, label, on_change
Value bi_ui_datepicker(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ui.date_picker")) return Value::nil();
  if (args[0].str().empty()) { error = "ui.date_picker() wants an id"; return Value::nil(); }
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.date_picker")) return Value::nil();
  if (auto o = as_map(opts)) {
    if (Value* e = o->find("on_change"); e && !is_callable(*e)) {
      error = std::string("ui.date_picker(): on_change wants a function, got a ") + type_name(e->type);
      return Value::nil();
    }
  }
  auto m = widget_map("datepicker", opts);
  m->put("id", args[0]);
  return Value::object(Type::Map, m);
}

// table(rows) or table(rows, options): columns, rows (how many show), empty.
Value bi_ui_table(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "ui.table")) return Value::nil();
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.table")) return Value::nil();
  auto m = widget_map("table", opts);
  m->put("data", args[0]);
  return Value::object(Type::Map, m);
}

// tile(label, value) or tile(label, value, options): accent (ok, warn, bad, or a colour).
Value bi_ui_tile(Vm&, std::vector<Value>& args, std::string& error) {
  const Value opts = arg_or(args, 2);
  if (!want_opts(opts, error, "ui.tile")) return Value::nil();
  auto m = widget_map("tile", opts);
  m->put("label", Value::string(args[0].to_text()));
  m->put("value", args[1]);
  return Value::object(Type::Map, m);
}

// heading(words, options): a title.
Value bi_ui_heading(Vm&, std::vector<Value>& args, std::string& error) {
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.heading")) return Value::nil();
  auto m = widget_map("heading", opts);
  m->put("label", Value::string(args[0].to_text()));
  return Value::object(Type::Map, m);
}

// badge(words, options): a small status pill. accent: ok, warn, bad or a colour; dot: true for a leading dot.
Value bi_ui_badge(Vm&, std::vector<Value>& args, std::string& error) {
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.badge")) return Value::nil();
  auto m = widget_map("badge", opts);
  m->put("label", Value::string(args[0].to_text()));
  return Value::object(Type::Map, m);
}

// progress(value, options) / gauge(value, options): a filled bar. max (100), min (0), label, accent.
// A progress bar's label shows the percentage, a gauge's the value.
Value value_bar(const char* type, std::vector<Value>& args, std::string& error) {
  const std::string who = std::string("ui.") + type;
  if (!args[0].is_number()) { error = who + "() wants a number"; return Value::nil(); }
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, who.c_str())) return Value::nil();
  auto m = widget_map(type, opts);
  m->put("value", args[0]);
  return Value::object(Type::Map, m);
}
Value bi_ui_progress(Vm&, std::vector<Value>& args, std::string& error) { return value_bar("progress", args, error); }
Value bi_ui_gauge(Vm&, std::vector<Value>& args, std::string& error) { return value_bar("gauge", args, error); }

// spark(numbers, options) / chart(numbers, options): a line over a series. height, min, max, accent.
// A sparkline is small and bare; a chart is taller and, in a browser, filled under the line.
Value series_widget(const char* type, std::vector<Value>& args, std::string& error) {
  const std::string who = std::string("ui.") + type;
  if (!want_list(args[0], error, who.c_str())) return Value::nil();
  for (const auto& v : args[0].list_items())
    if (!v.is_number() && v.type != Type::Nil) { error = who + "() wants a list of numbers, and one is a " + type_name(v.type); return Value::nil(); }
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, who.c_str())) return Value::nil();
  auto m = widget_map(std::string(type) == "spark" ? "sparkline" : "chart", opts);
  m->put("data", args[0]);
  return Value::object(Type::Map, m);
}
Value bi_ui_spark(Vm&, std::vector<Value>& args, std::string& error) { return series_widget("spark", args, error); }
Value bi_ui_chart(Vm&, std::vector<Value>& args, std::string& error) { return series_widget("chart", args, error); }

// bars(rows, options): a bar chart, one labelled bar a row, each against the largest. rows: a map of
// label -> number, or a list of [label, number]. Made of gauges, so a window, a browser and a native client
// all draw it. accent, gap.
Value bi_ui_bars(Vm&, std::vector<Value>& args, std::string& error) {
  std::vector<std::pair<std::string, double>> items;
  if (args[0].type == Type::Map) {
    for (const auto& kv : as_map(args[0])->entries) items.push_back({kv.first, kv.second.as_double()});
  } else if (args[0].type == Type::List) {
    for (const auto& r : args[0].list_items()) {
      if (r.type != Type::List || r.list_items().size() < 2) { error = "ui.bars() wants [label, number] pairs or a map"; return Value::nil(); }
      items.push_back({r.list_items()[0].to_text(), r.list_items()[1].as_double()});
    }
  } else { error = std::string("ui.bars() wants a map or a list of pairs, got a ") + type_name(args[0].type); return Value::nil(); }
  const Value opts = arg_or(args, 1);
  if (!want_opts(opts, error, "ui.bars")) return Value::nil();
  double hi = 0;
  for (const auto& it : items) hi = std::max(hi, it.second);
  if (hi <= 0) hi = 1;
  auto om = as_map(opts);
  std::vector<Value> kids;
  for (const auto& it : items) {
    auto bo = std::make_shared<MapObj>();
    bo->put("label", Value::string(it.first));
    bo->put("max", Value::real(hi));
    if (om) for (const char* k : {"accent", "prefix", "suffix"}) if (Value* a = om->find(k)) bo->put(k, *a);
    auto g = widget_map("gauge", Value::object(Type::Map, bo));
    const bool whole = std::fabs(it.second - std::round(it.second)) < 1e-9 && std::fabs(it.second) < 9e15;
    g->put("value", whole ? Value::integer((int64_t)std::llround(it.second)) : Value::real(it.second));
    kids.push_back(Value::object(Type::Map, g));
  }
  auto co = std::make_shared<MapObj>();
  co->put("gap", Value::integer(om && om->find("gap") ? om->find("gap")->as_int() : 10));
  auto col = widget_map("column", Value::object(Type::Map, co));
  col->put("children", Value::list(std::move(kids)));
  return Value::object(Type::Map, col);
}

// ---- grain values as face values -------------------------------------------

fc::Value to_face(const Value& v, int depth = 0) {
  if (depth > 64) return fc::Value::str(v.to_text());
  switch (v.type) {
    case Type::Nil: return fc::Value::null();
    case Type::Bool: return fc::Value::boolean(v.b);
    case Type::Int:
    case Type::Real: return fc::Value::num(v.as_double());
    case Type::Decimal: return fc::Value::str(v.to_text());   // the digits it has, not a double's approximation
    case Type::Str: return fc::Value::str(v.str());
    case Type::List: {
      fc::Value out = fc::Value::make_list();
      for (const auto& e : v.list_items()) out.list->push_back(to_face(e, depth + 1));
      return out;
    }
    case Type::Map: {
      fc::Value out = fc::Value::make_rec();
      for (const auto& kv : as_map(v)->entries) out.rec->emplace_back(kv.first, to_face(kv.second, depth + 1));
      return out;
    }
    default: return fc::Value::str(v.to_text());
  }
}

// An option as a face attribute: words and numbers as written, true/false as a flag.
bool attr_text(const std::string& key, const Value& v, std::string& out, std::string& error) {
  switch (v.type) {
    case Type::Str: out = v.str(); return true;
    case Type::Int:
    case Type::Real: out = v.to_text(); return true;
    case Type::Bool: out = v.b ? "true" : "false"; return true;
    default:
      error = "the option \"" + key + "\" wants a string, a number or true/false, got a " + type_name(v.type);
      return false;
  }
}

// ---- the pane ----------------------------------------------------------------

// Event keys as face names them (grain's are SDL's, lower case).
std::string face_key(const std::string& k, bool ctrl) {
  static const std::map<std::string, std::string> names = {
      {"return", "Enter"}, {"enter", "Enter"}, {"escape", "Escape"}, {"tab", "Tab"}, {"backspace", "Backspace"},
      {"delete", "Delete"}, {"up", "ArrowUp"}, {"down", "ArrowDown"}, {"left", "ArrowLeft"}, {"right", "ArrowRight"},
      {"home", "Home"}, {"end", "End"}, {"pageup", "PageUp"}, {"pagedown", "PageDown"}, {"space", "Space"}};
  if (ctrl && k.size() == 1) return std::string("Ctrl+") + (char)std::toupper((unsigned char)k[0]);
  auto it = names.find(k);
  return it == names.end() ? std::string() : it->second;
}

class FacePane final : public PaneDriver {
 public:
  FacePane(Value view, int width, int height) : view_(std::move(view)), rt_(fc::Face{}), w_(width), h_(height) {
    HostBindings h;
    h.now_ms = []() {
      return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count();
    };
    rt_.bind(std::move(h));
    rt_.set_action_handler([this](const std::string& action) { return run(action); });
  }

  bool draw(Vm& vm, std::vector<uint32_t>& argb, int width, int height, std::string& error) override {
    vm_ = &vm;
    w_ = width;
    h_ = height;
    if (!lay_out(error)) return false;
    rt_.paint(argb, width, height, 0, 0);
    return report(error);
  }

  bool event(Vm& vm, const Value& ev, std::string& error) override {
    vm_ = &vm;
    auto m = as_map(ev);
    if (!m) return true;
    auto num = [&](const char* k) { Value* v = m->find(k); return v ? (int)v->as_int() : 0; };
    auto str = [&](const char* k) { Value* v = m->find(k); return v && v->type == Type::Str ? v->str() : std::string(); };
    const std::string type = str("type");
    if (type == "resize") { w_ = num("width"); h_ = num("height"); return true; }
    // Input before the first frame (a script played at once) meets the face
    // laid out, its fields seeded and focused, as a person's input would.
    if (!loaded_ && !lay_out(error)) return false;
    if (type == "click") rt_.click(num("x"), num("y"));
    else if (type == "mouse_move") rt_.mouse_move(num("x"), num("y"));
    else if (type == "wheel") {
      Value* dy = m->find("dy");
      rt_.mouse_wheel(0, dy ? (int)(dy->as_double() * 40.0) : 0);
    } else if (type == "text") rt_.type(str("text"));
    else if (type == "key") {
      Value* c = m->find("ctrl");
      const std::string name = face_key(str("key"), c && c->type == Type::Bool && c->b);
      if (!name.empty()) { rt_.key(name, true); rt_.key(name, false); }
    }
    return report(error);
  }

  fc::Runtime& runtime() { return rt_; }

  // ---- the same program in a browser: widgets as HTML, events from fields and actions ----

  uint64_t version() const { return version_; }
  int refresh() const { return refresh_; }
  void set_refresh(int s) { refresh_ = s; }
  bool shared() const { return shared_; }
  void set_shared(bool v) { shared_ = v; }
  const std::string& css() const { return css_; }
  void set_css(const std::string& c) { css_ = c; }
  const std::string& theme() const { return theme_; }
  void set_theme(const std::string& t) { theme_ = t; }
  const Value& routes() const { return routes_; }
  void set_routes(const Value& r) { routes_ = r; }
  const Value& middleware() const { return middleware_; }
  void set_middleware(const Value& m) { middleware_ = m; }
  bool workers() const { return workers_; }
  bool api_only() const { return api_only_; }
  void set_api_only(bool v) { api_only_ = v; }
  void set_workers(bool v) { workers_ = v; }

  bool render_html(Vm& vm, std::string& html, std::string& error) {
    vm_ = &vm;
    web_ = true;
    Value tree = call(view_, error);
    if (!error.empty()) { error = "view(): " + error; return false; }
    actions_.clear();
    path_.clear();
    ++version_;
    html.clear();
    if (!html_widget(tree, html, error, 0)) { error = "view(): " + error; return false; }
    return true;
  }

  bool render_tree(Vm& vm, std::string& json, std::string& error) {
    vm_ = &vm;
    web_ = true;
    Value tree = call(view_, error);
    if (!error.empty()) { error = "view(): " + error; return false; }
    actions_.clear();
    path_.clear();
    ++version_;
    json.clear();
    if (!json_widget(tree, json, error, 0)) { error = "view(): " + error; return false; }
    return true;
  }

  static bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
      const unsigned char c = (unsigned char)s[i];
      const size_t n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 4;
      if (n == 4 || (n == 1 && c < 0xC2)) return false;   // a stray continuation byte, 0xF8 and up, or an overlong pair
      uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
      for (size_t k = 1; k <= n; ++k) {
        if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
      }
      if ((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
      i += n + 1;
    }
    return true;
  }

  // Text that is not UTF-8 is read as Windows-1252 (a stored latin1 '®' is 0xAE), so the JSON stays JSON.
  static std::string from_cp1252(const std::string& s) {
    static const uint16_t high[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                      0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    std::string o;
    for (unsigned char c : s) {
      const uint32_t cp = c < 0x80 ? c : c < 0xA0 ? high[c - 0x80] : c;
      if (cp < 0x80) o += (char)cp;
      else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
      else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    return o;
  }

  static std::string jstr(const std::string& raw) {
    const std::string s = valid_utf8(raw) ? raw : from_cp1252(raw);
    std::string o = "\"";
    for (unsigned char c : s) {
      switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
          if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
          else o += (char)c;
      }
    }
    return o + "\"";
  }

  // Any grain value as JSON: table rows and option values.
  static void value_json(const Value& v, std::string& out, int depth) {
    if (depth > 32) { out += "null"; return; }
    switch (v.type) {
      case Type::Nil: out += "null"; return;
      case Type::Bool: out += v.b ? "true" : "false"; return;
      case Type::Int: out += std::to_string(v.i); return;
      case Type::Decimal: out += decimal_text(v); return;
      case Type::Real: {
        if (!(v.d == v.d) || v.d > 1e308 || v.d < -1e308) { out += "null"; return; }
        out += v.to_text();
        return;
      }
      case Type::Str: out += jstr(v.str()); return;
      case Type::List: {
        out += "[";
        bool first = true;
        for (const auto& e : v.list_items()) { if (!first) out += ","; first = false; value_json(e, out, depth + 1); }
        out += "]";
        return;
      }
      case Type::Map: {
        out += "{";
        bool first = true;
        for (const auto& kv : as_map(v)->entries) {
          if (is_callable(kv.second)) continue;
          if (!first) out += ",";
          first = false;
          out += jstr(kv.first) + ":";
          value_json(kv.second, out, depth + 1);
        }
        out += "}";
        return;
      }
      default: out += jstr(v.to_text()); return;
    }
  }

  bool json_widget(const Value& v, std::string& out, std::string& error, int depth) {
    auto m = as_map(v);
    Value* type_v = m ? m->find("widget") : nullptr;
    if (!type_v || type_v->type != Type::Str) {
      error = std::string("a widget (ui.column, ui.text, ...) is expected, got a ") + type_name(v.type);
      return false;
    }
    if (depth > 200) { error = "the widgets nest more than 200 deep"; return false; }
    const std::string& type = type_v->str();
    out += "{\"widget\":" + jstr(type);
    Value* label = m->find("label");
    if (label) out += ",\"label\":" + jstr(label->to_text());
    // the options that are data; a function (on_enter, on_click) becomes an action name
    out += ",\"opts\":{";
    bool first = true;
    if (Value* o = m->find("opts"); o && o->type == Type::Map) {
      for (const auto& kv : as_map(*o)->entries) {
        if (is_callable(kv.second)) continue;
        if (!first) out += ",";
        first = false;
        out += jstr(kv.first) + ":";
        value_json(kv.second, out, 0);
      }
    }
    out += "}";
    if (Value* fn = m->find("on_click")) out += ",\"act\":" + jstr(action_for(*fn, "the button \"" + (label ? label->to_text() : std::string()) + "\""));
    if (type == "field" || type == "datepicker") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      auto have = web_fields_.find(id);
      const std::string value = (have != web_fields_.end() && web_set_fields_.count(id)) ? have->second : opt_str(v, "value");
      out += ",\"id\":" + jstr(id) + ",\"value\":" + jstr(value);
      if (const Value* e = opt(v, "on_enter"); e && is_callable(*e)) out += ",\"enter\":" + jstr(action_for(*e, "the field \"" + id + "\"'s on_enter"));
      if (const Value* a = opt(v, "on_change"); a && is_callable(*a)) out += ",\"act\":" + jstr(action_for(*a, "the datepicker \"" + id + "\"'s on_change"));
    } else if (Value* val = m->find("value")) {
      out += ",\"value\":";
      value_json(*val, out, 0);
    }
    if (Value* data = m->find("data")) { out += ",\"data\":"; value_json(*data, out, 0); }
    if (Value* kids = m->find("children"); kids && kids->type == Type::List) {
      out += ",\"children\":[";
      bool firstk = true;
      const std::string here = path_;
      size_t at = 0;
      for (const auto& c : kids->list_items()) {
        if (!firstk) out += ",";
        firstk = false;
        path_ = here + "/" + std::to_string(at++);
        if (!json_widget(c, out, error, depth + 1)) return false;
      }
      path_ = here;
      out += "]";
    }
    out += "}";
    return true;
  }

  bool web_event(Vm& vm, uint64_t version, const std::string& action,
                 const std::vector<std::pair<std::string, std::string>>& fields, std::string& error) {
    vm_ = &vm;
    web_ = true;
    for (const auto& kv : fields) web_fields_[kv.first] = kv.second;
    (void)version;
    if (action.empty()) return true;   // only the fields changed
    auto it = actions_.find(action);
    if (it == actions_.end()) return true;
    std::string err;
    const auto [fn, who] = it->second;
    call(fn, err);
    for (const auto& kv : fields) {
      if (web_set_fields_.find(kv.first) == web_set_fields_.end()) {
        web_fields_.erase(kv.first);
      }
    }
    if (!err.empty()) { error = who + ": " + err; return false; }
    return true;
  }

  bool web() const { return web_; }
  std::string web_field(const std::string& id) const { auto it = web_fields_.find(id); return it == web_fields_.end() ? std::string() : it->second; }
  void web_set_field(const std::string& id, const std::string& text) {
    web_fields_[id] = text;
    web_set_fields_.insert(id);
  }

  static std::string nice_num(double x) {
    if (!(x == x)) return "";
    const bool neg = x < 0;
    double a = neg ? -x : x;
    const bool whole = std::fabs(a - std::round(a)) < 1e-9;
    char buf[64];
    if (whole) std::snprintf(buf, sizeof buf, "%.0f", a);
    else std::snprintf(buf, sizeof buf, "%.2f", a);
    std::string digits = buf, frac;
    const size_t dot = digits.find('.');
    if (dot != std::string::npos) { frac = digits.substr(dot); digits.resize(dot); while (!frac.empty() && (frac.back() == '0' || frac.back() == '.')) frac.pop_back(); }
    std::string grouped;
    for (size_t i = 0; i < digits.size(); ++i) {
      if (i && (digits.size() - i) % 3 == 0) grouped += ',';
      grouped += digits[i];
    }
    return (neg ? "-" : "") + grouped + frac;
  }

  static std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
      switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
      }
    }
    return o;
  }

 private:
  // An option read from a widget's opts map.
  static const Value* opt(const Value& widget, const char* key) {
    auto m = as_map(widget);
    Value* o = m ? m->find("opts") : nullptr;
    auto om = o ? as_map(*o) : nullptr;
    return om ? om->find(key) : nullptr;
  }
  static int opt_int(const Value& w, const char* key, int dflt) { const Value* v = opt(w, key); return v && v->is_number() ? (int)v->as_int() : dflt; }
  static std::string opt_str(const Value& w, const char* key) { const Value* v = opt(w, key); return v ? v->to_text() : std::string(); }
  static bool opt_flag(const Value& w, const char* key) { const Value* v = opt(w, key); return v && v->truthy(); }
  static void extra_attrs(const Value& v, std::string& cls, std::string& style) {
    std::string custom_cls = opt_str(v, "class");
    if (custom_cls.empty()) custom_cls = opt_str(v, "cls");
    if (!custom_cls.empty()) cls += " " + esc(custom_cls);
    std::string theme = opt_str(v, "theme");
    if (!theme.empty()) cls += " theme-" + esc(theme);
    std::string custom_style = opt_str(v, "style");
    if (!custom_style.empty()) {
      if (!style.empty() && style.back() != ';') style += ";";
      style += custom_style;
      if (style.back() != ';') style += ";";
    }
  }

  // A colour or CSS keyword that cannot break out of an attribute.
  static std::string safe_css(const std::string& s) {
    std::string o;
    for (char c : s) if (std::isalnum((unsigned char)c) || c == '#' || c == '-' || c == '.' || c == '(' || c == ')' || c == ',' || c == '%' || c == ' ') o += c;
    return o;
  }

  bool html_widget(const Value& v, std::string& out, std::string& error, int depth) {
    auto m = as_map(v);
    Value* type_v = m ? m->find("widget") : nullptr;
    if (!type_v || type_v->type != Type::Str) {
      error = std::string("a widget (ui.column, ui.text, ...) is expected, got a ") + type_name(v.type);
      return false;
    }
    if (depth > 200) { error = "the widgets nest more than 200 deep"; return false; }
    const std::string& type = type_v->str();
    const Value* label_val = m->find("label");
    const std::string label_text = label_val ? label_val->to_text() : std::string();
    if (type == "column" || type == "row" || type == "card") {
      std::string cls = type == "card" ? "w-column w-card" : "w-" + type;
      if (opt_flag(v, "card")) cls += " w-card";
      std::string style;
      const int gap = opt_int(v, "gap", type == "card" ? 14 : 8);
      const int pad = opt_int(v, "pad", type == "card" ? 18 : 0);
      style += "gap:" + std::to_string(gap) + "px;padding:" + std::to_string(pad) + "px;";
      if (const std::string al = safe_css(opt_str(v, "align")); !al.empty()) style += "align-items:" + al + ";";
      if (const std::string ju = safe_css(opt_str(v, "justify")); !ju.empty()) style += "justify-content:" + (ju == "between" ? "space-between" : ju) + ";";
      if (const std::string bg = safe_css(opt_str(v, "bg")); !bg.empty()) style += "background:" + bg + ";";
      if (const std::string border = safe_css(opt_str(v, "border")); !border.empty()) style += "border:" + border + ";";
      if (const int radius = opt_int(v, "radius", 0); radius > 0) style += "border-radius:" + std::to_string(radius) + "px;";
      if (const int w = opt_int(v, "width", 0); w > 0) style += "width:" + std::to_string(w) + "px;flex:none;";
      if (const int h = opt_int(v, "height", 0); h > 0) style += "height:" + std::to_string(h) + "px;";
      if (opt_flag(v, "grow")) style += "flex:1 1 0;min-width:0;";
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">";
      if (Value* kids = m->find("children"); kids && kids->type == Type::List) {
        const std::string here = path_;
        size_t at = 0;
        for (const auto& c : kids->list_items()) {
          path_ = here + "/" + std::to_string(at++);
          if (!html_widget(c, out, error, depth + 1)) return false;
        }
        path_ = here;
      }
      out += "</div>";
      return true;
    }
    if (type == "divider") {
      std::string cls = "w-divider", style;
      extra_attrs(v, cls, style);
      out += "<hr class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">";
      return true;
    }
    if (type == "text") {
      std::string cls = "w-text";
      if (opt_flag(v, "dim")) cls += " dim";
      if (opt_flag(v, "bold")) cls += " bold";
      if (opt_flag(v, "mono")) cls += " mono";
      if (opt_flag(v, "italic")) cls += " italic";
      std::string style;
      if (const int size = opt_int(v, "size", 0)) style += "font-size:" + std::to_string(size) + "px;";
      if (const std::string color = safe_css(opt_str(v, "color")); !color.empty()) {
        if (color == "ok" || color == "warn" || color == "bad" || color == "accent" || color == "dim") style += "color:var(--" + color + ");";
        else style += "color:" + color + ";";
      }
      if (const std::string align = safe_css(opt_str(v, "align")); !align.empty()) style += "text-align:" + align + ";";
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">" + esc(label_text) + "</div>";
      return true;
    }
    if (type == "button") {
      Value* fn = m->find("on_click");
      const std::string act = fn ? action_for(*fn, "the button \"" + label_text + "\"") : std::string();
      std::string cls = "w-btn";
      std::string variant = opt_str(v, "variant");
      if (opt_flag(v, "primary") || variant == "primary") cls += " primary";
      else if (variant == "bad" || variant == "danger") cls += " bad";
      else if (variant == "ok" || variant == "success") cls += " ok";
      else if (variant == "ghost") cls += " ghost";
      else if (variant == "outline") cls += " outline";
      std::string style;
      if (opt_flag(v, "grow")) style += "flex:1 1 auto;";
      extra_attrs(v, cls, style);
      out += "<button type=\"button\" class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + " data-act=\"" + esc(act) + "\">" + esc(label_text) + "</button>";
      return true;
    }
    if (type == "field") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      auto have = web_fields_.find(id);
      const std::string value = (have != web_fields_.end() && web_set_fields_.count(id)) ? have->second : opt_str(v, "value");
      std::string enter;
      if (const Value* e = opt(v, "on_enter"); e && is_callable(*e)) enter = action_for(*e, "the field \"" + id + "\"'s on_enter");
      std::string cls = "w-field", style;
      if (const int width = opt_int(v, "width", 0)) style += "width:" + std::to_string(width) + "px;flex:none;";
      extra_attrs(v, cls, style);
      out += "<input class=\"" + cls + "\" type=\"text\" data-id=\"" + esc(id) + "\" value=\"" + esc(value) + "\" placeholder=\"" + esc(opt_str(v, "placeholder")) + "\"";
      if (!enter.empty()) out += " data-enter=\"" + esc(enter) + "\"";
      if (opt_flag(v, "focus")) out += " data-focus=\"1\"";
      if (!style.empty()) out += " style=\"" + style + "\"";
      out += " autocomplete=\"off\">";
      return true;
    }
    if (type == "switch") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      const std::string label = opt_str(v, "label").empty() ? id : opt_str(v, "label");
      auto have = web_fields_.find(id);
      bool checked = false;
      if (have != web_fields_.end() && web_set_fields_.count(id)) {
        checked = have->second == "1" || have->second == "true";
      } else {
        checked = opt_flag(v, "checked");
      }
      std::string act;
      if (const Value* fn = opt(v, "on_change"); fn && is_callable(*fn)) {
        act = action_for(*fn, "the switch \"" + id + "\"'s on_change");
      }
      std::string cls = "w-switch-wrap", style;
      extra_attrs(v, cls, style);
      out += "<label class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + "><input type=\"checkbox\" class=\"w-switch\" data-id=\"" + esc(id) + "\"";
      if (!act.empty()) out += " data-act=\"" + esc(act) + "\"";
      if (checked) out += " checked";
      out += "><span class=\"w-switch-pill\"></span><span class=\"w-switch-text\">" + esc(label) + "</span></label>";
      return true;
    }
    if (type == "checkbox") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      const std::string label = opt_str(v, "label").empty() ? id : opt_str(v, "label");
      auto have = web_fields_.find(id);
      bool checked = false;
      if (have != web_fields_.end() && web_set_fields_.count(id)) {
        checked = have->second == "1" || have->second == "true";
      } else {
        checked = opt_flag(v, "checked");
      }
      std::string act;
      if (const Value* fn = opt(v, "on_change"); fn && is_callable(*fn)) {
        act = action_for(*fn, "the checkbox \"" + id + "\"'s on_change");
      }
      std::string cls = "w-check-wrap", style;
      extra_attrs(v, cls, style);
      out += "<label class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + "><input type=\"checkbox\" class=\"w-check\" data-id=\"" + esc(id) + "\"";
      if (!act.empty()) out += " data-act=\"" + esc(act) + "\"";
      if (checked) out += " checked";
      out += "><span class=\"w-check-box\"></span><span class=\"w-check-text\">" + esc(label) + "</span></label>";
      return true;
    }
    if (type == "slider") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      Value* minv = m->find("min");
      Value* maxv = m->find("max");
      const double min_val = minv ? minv->as_double() : 0.0;
      const double max_val = maxv ? maxv->as_double() : 100.0;
      double cur_val = opt(v, "value") ? opt(v, "value")->as_double() : min_val;
      auto have = web_fields_.find(id);
      if (have != web_fields_.end() && web_set_fields_.count(id)) {
        cur_val = std::strtod(have->second.c_str(), nullptr);
      }
      const double step = opt(v, "step") ? opt(v, "step")->as_double() : 1.0;
      const std::string label = opt_str(v, "label");
      std::string act;
      if (const Value* fn = opt(v, "on_change"); fn && is_callable(*fn)) {
        act = action_for(*fn, "the slider \"" + id + "\"'s on_change");
      }
      std::string cls = "w-slider-wrap", style;
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">";
      if (!label.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f", cur_val);
        out += "<div class=\"w-slider-head\"><span>" + esc(label) + "</span><span class=\"w-slider-num\">" + std::string(buf) + "</span></div>";
      }
      out += "<input type=\"range\" class=\"w-slider\" data-id=\"" + esc(id) + "\" min=\"" + std::to_string((int)min_val) + "\" max=\"" + std::to_string((int)max_val) + "\" step=\"" + std::to_string((int)step) + "\" value=\"" + std::to_string((int)cur_val) + "\"";
      if (!act.empty()) out += " data-act=\"" + esc(act) + "\"";
      out += "></div>";
      return true;
    }
    if (type == "select") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      const std::string label = opt_str(v, "label");
      std::string cur_val = opt_str(v, "value");
      auto have = web_fields_.find(id);
      if (have != web_fields_.end() && web_set_fields_.count(id)) cur_val = have->second;
      std::string act;
      if (const Value* fn = opt(v, "on_change"); fn && is_callable(*fn)) {
        act = action_for(*fn, "the select \"" + id + "\"'s on_change");
      }
      std::string cls = "w-select-wrap", style;
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">";
      if (!label.empty()) out += "<div class=\"w-select-label\">" + esc(label) + "</div>";
      out += "<select class=\"w-select\" data-id=\"" + esc(id) + "\"";
      if (!act.empty()) out += " data-act=\"" + esc(act) + "\"";
      out += ">";
      if (Value* opts = m->find("options"); opts && opts->type == Type::List) {
        for (const auto& item : opts->list_items()) {
          std::string item_val, item_label;
          if (item.type == Type::Map) {
            auto im = as_map(item);
            Value* iv = im->find("value");
            Value* il = im->find("label");
            item_val = iv ? iv->to_text() : "";
            item_label = il ? il->to_text() : item_val;
          } else if (item.type == Type::List && item.list_items().size() >= 2) {
            item_val = item.list_items()[0].to_text();
            item_label = item.list_items()[1].to_text();
          } else {
            item_val = item.to_text();
            item_label = item_val;
          }
          out += "<option value=\"" + esc(item_val) + "\"" + (item_val == cur_val ? " selected" : "") + ">" + esc(item_label) + "</option>";
        }
      }
      out += "</select></div>";
      return true;
    }
    if (type == "datepicker") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      auto have = web_fields_.find(id);
      const std::string value = (have != web_fields_.end() && web_set_fields_.count(id)) ? have->second : opt_str(v, "value");
      const std::string mode = opt_str(v, "mode").empty() ? "single" : opt_str(v, "mode");
      const std::string label = opt_str(v, "label");
      const std::string placeholder = opt_str(v, "placeholder").empty() ? (mode == "range" ? "Select date range..." : "Select date...") : opt_str(v, "placeholder");
      const std::string min_d = opt_str(v, "min");
      const std::string max_d = opt_str(v, "max");
      const bool presets = opt(v, "presets") ? opt_flag(v, "presets") : (mode == "range");

      std::string act;
      if (const Value* fn = opt(v, "on_change"); fn && is_callable(*fn)) {
        act = action_for(*fn, "the datepicker \"" + id + "\"'s on_change");
      }

      std::string cls = "w-datepicker-wrap", style;
      if (const int width = opt_int(v, "width", 0)) style += "width:" + std::to_string(width) + "px;flex:none;";
      extra_attrs(v, cls, style);

      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + " data-dp=\"1\" data-mode=\"" + esc(mode) + "\"";
      if (!min_d.empty()) out += " data-min=\"" + esc(min_d) + "\"";
      if (!max_d.empty()) out += " data-max=\"" + esc(max_d) + "\"";
      if (presets) out += " data-presets=\"1\"";
      out += ">";
      if (!label.empty()) out += "<div class=\"w-dp-label\">" + esc(label) + "</div>";
      out += "<div class=\"w-dp-box\" tabindex=\"0\" role=\"combobox\" aria-expanded=\"false\">";
      out += "<svg class=\"w-dp-icon\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"3\" y=\"4\" width=\"18\" height=\"18\" rx=\"3\" ry=\"3\"/><line x1=\"16\" y1=\"2\" x2=\"16\" y2=\"6\"/><line x1=\"8\" y1=\"2\" x2=\"8\" y2=\"6\"/><line x1=\"3\" y1=\"10\" x2=\"21\" y2=\"10\"/></svg>";
      out += "<span class=\"w-dp-display" + std::string(value.empty() ? " empty" : "") + "\">" + esc(value.empty() ? placeholder : value) + "</span>";
      if (mode == "range") out += "<span class=\"w-dp-badge\"></span>";
      out += "<button type=\"button\" class=\"w-dp-clear" + std::string(value.empty() ? "" : " show") + "\" title=\"Clear\" aria-label=\"Clear\">&times;</button>";
      out += "<svg class=\"w-dp-chevron\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><polyline points=\"6 9 12 15 18 9\"/></svg>";
      out += "</div>";
      out += "<input type=\"hidden\" class=\"w-dp-val\" data-id=\"" + esc(id) + "\" value=\"" + esc(value) + "\"";
      if (!act.empty()) out += " data-act=\"" + esc(act) + "\"";
      out += "></div>";
      return true;
    }
    if (type == "tile") {
      std::string accent = opt_str(v, "accent");
      std::string cls = "w-tile", style, tv_style;
      if (accent == "ok" || accent == "warn" || accent == "bad") cls += " " + accent;
      else if (!accent.empty()) tv_style = " style=\"color:" + safe_css(accent) + "\"";
      extra_attrs(v, cls, style);
      Value* val = m->find("value");
      std::string sub = opt_str(v, "sub");
      std::string sub_cls = "sub";
      if (accent == "ok" || accent == "warn" || accent == "bad") sub_cls += " " + accent;
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + "><div class=\"tv\"" + tv_style + ">" + esc(val ? val->to_text() : std::string()) + "</div><div class=\"tl\">" + esc(label_text) + "</div>";
      if (!sub.empty()) out += "<div class=\"" + sub_cls + "\">" + esc(sub) + "</div>";
      out += "</div>";
      return true;
    }
    if (type == "table") return html_table(v, out, error);
    if (type == "heading") {
      std::string cls = "w-heading", style;
      if (const int s = opt_int(v, "size", 0); s > 0) style += "font-size:" + std::to_string(s) + "px;";
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">" + esc(label_text) + "</div>";
      return true;
    }
    if (type == "badge") {
      const std::string accent = opt_str(v, "accent");
      std::string cls = "w-badge";
      std::string style;
      if (accent == "ok" || accent == "warn" || accent == "bad") cls += " " + accent;
      else if (!accent.empty()) style = "--c:" + safe_css(accent) + ";";
      if (opt_flag(v, "dot")) cls += " dot";
      extra_attrs(v, cls, style);
      out += "<span class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">" + esc(label_text) + "</span>";
      return true;
    }
    if (type == "progress" || type == "gauge") {
      Value* val = m->find("value");
      const double x = val ? val->as_double() : 0;
      const Value* mx = opt(v, "max");
      const Value* mn = opt(v, "min");
      const double hi = mx && mx->is_number() ? mx->as_double() : 100.0, lo = mn && mn->is_number() ? mn->as_double() : 0.0;
      double frac = hi > lo ? (x - lo) / (hi - lo) : 0;
      frac = std::max(0.0, std::min(1.0, frac));
      const std::string label = opt_str(v, "label");
      const std::string accent = opt_str(v, "accent");
      std::string fill = "var(--accent)";
      if (accent == "ok" || accent == "warn" || accent == "bad") fill = "var(--" + accent + ")";
      else if (!accent.empty()) fill = safe_css(accent);
      char pct[32];
      std::snprintf(pct, sizeof pct, "%.1f", frac * 100.0);
      std::string cls = "w-bar", style;
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">";
      if (!label.empty() || type == "progress") {
        const std::string shown = type == "progress" ? std::to_string((int)(frac * 100.0 + 0.5)) + "%"
                                                     : opt_str(v, "prefix") + (val && val->is_number() ? nice_num(val->as_double()) : val ? val->to_text() : std::string()) + opt_str(v, "suffix");
        out += "<div class=\"bl\"><span>" + esc(label) + "</span><span class=\"bv\">" + esc(shown) + "</span></div>";
      }
      out += "<div class=\"bt\"><div class=\"bf\" style=\"width:" + std::string(pct) + "%;background:" + fill + "\"></div></div></div>";
      return true;
    }
    if (type == "sparkline" || type == "chart") {
      Value* data = m->find("data");
      std::vector<double> pts;
      if (data && data->type == Type::List) for (const auto& x : data->list_items()) pts.push_back(x.type == Type::Nil ? 0.0 : x.as_double());
      const bool big = type == "chart";
      const int h = opt_int(v, "height", big ? 120 : 36);
      const std::string accent = opt_str(v, "accent");
      std::string stroke = "var(--accent)";
      if (accent == "ok" || accent == "warn" || accent == "bad") stroke = "var(--" + accent + ")";
      else if (!accent.empty()) stroke = safe_css(accent);
      std::string cls = "w-" + std::string(big ? "chart" : "spark");
      std::string style = "height:" + std::to_string(h) + "px;";
      extra_attrs(v, cls, style);
      out += "<div class=\"" + cls + "\" style=\"" + style + "\">";
      if (pts.size() >= 2) {
        double lo = pts[0], hi = pts[0];
        for (double p : pts) { lo = std::min(lo, p); hi = std::max(hi, p); }
        if (const Value* mn = opt(v, "min"); mn && mn->is_number()) lo = mn->as_double();
        if (const Value* mx = opt(v, "max"); mx && mx->is_number()) hi = mx->as_double();
        if (hi - lo < 1e-9) { hi += 1; lo -= 1; }
        // A 1000 x 100 box stretched to the widget: the line keeps its width at any size.
        std::string line;
        char b[64];
        for (size_t i = 0; i < pts.size(); ++i) {
          const double px = (double)i * 1000.0 / (double)(pts.size() - 1);
          const double py = 96.0 - (pts[i] - lo) / (hi - lo) * 92.0;
          std::snprintf(b, sizeof b, "%s%.1f,%.1f", i ? " " : "", px, py);
          line += b;
        }
        out += "<svg viewBox=\"0 0 1000 100\" preserveAspectRatio=\"none\">";
        if (big) out += "<polygon points=\"0,100 " + line + " 1000,100\" fill=\"" + stroke + "\" fill-opacity=\"0.14\"/>";
        out += "<polyline points=\"" + line + "\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + std::string(big ? "2.5" : "2") +
               "\" vector-effect=\"non-scaling-stroke\" stroke-linejoin=\"round\"/></svg>";
        if (big) {
          char lab[96];
          std::snprintf(lab, sizeof lab, "<div class=\"ax\"><span>%s</span><span>%s</span></div>", esc(opt_str(v, "prefix") + nice_num(hi)).c_str(), esc(opt_str(v, "prefix") + nice_num(lo)).c_str());
          out += lab;
        }
      }
      out += "</div>";
      return true;
    }
    error = "ui." + type + " has no web rendering";
    return false;
  }

  // columns "key:Header, key:Header:right"; every field of the first row when left out
  bool html_table(const Value& v, std::string& out, std::string& error) {
    auto m = as_map(v);
    Value* data = m->find("data");
    if (!data || data->type != Type::List) { error = "ui.table() has no rows"; return false; }
    struct Col { std::string key, head; bool right = false; bool badge = false; };
    std::vector<Col> cols;
    const std::string spec = opt_str(v, "columns");
    size_t at = 0;
    while (at < spec.size()) {
      size_t comma = spec.find(',', at);
      if (comma == std::string::npos) comma = spec.size();
      std::string item = spec.substr(at, comma - at);
      at = comma + 1;
      std::vector<std::string> parts;
      size_t p = 0;
      while (p <= item.size()) {
        size_t colon = item.find(':', p);
        if (colon == std::string::npos) colon = item.size();
        std::string part = item.substr(p, colon - p);
        const size_t b = part.find_first_not_of(' '), e = part.find_last_not_of(' ');
        parts.push_back(b == std::string::npos ? "" : part.substr(b, e - b + 1));
        p = colon + 1;
      }
      if (parts.empty() || parts[0].empty()) continue;
      Col c;
      c.key = parts[0];
      c.head = parts.size() > 1 && !parts[1].empty() ? parts[1] : parts[0];
      c.right = parts.size() > 2 && parts[2] == "right";
      c.badge = parts.size() > 2 && parts[2] == "badge";
      cols.push_back(c);
    }
    const auto& rows = data->list_items();
    if (cols.empty() && !rows.empty())
      if (auto first = as_map(rows[0])) for (const auto& kv : first->entries) cols.push_back({kv.first, kv.first, false});
    std::string tbl_cls = "w-table", tbl_style;
    extra_attrs(v, tbl_cls, tbl_style);
    out += "<table class=\"" + tbl_cls + "\"" + (tbl_style.empty() ? "" : " style=\"" + tbl_style + "\"") + "><thead><tr>";
    for (const auto& c : cols) out += std::string("<th") + (c.right ? " class=\"r\"" : "") + ">" + esc(c.head) + "</th>";
    out += "</tr></thead><tbody>";
    const int limit = opt_int(v, "rows", 0);
    int shown = 0;
    for (const auto& row : rows) {
      if (limit > 0 && shown >= limit) break;
      auto rm = as_map(row);
      out += "<tr>";
      for (const auto& c : cols) {
        Value* cell = rm ? rm->find(c.key) : nullptr;
        const std::string text = cell ? cell->to_text() : std::string();
        if (c.badge && !text.empty()) {
          // badges: {"open": "warn", ...} names each value's colour; otherwise the accent, or none
          std::string accent;
          if (const Value* bm = opt(v, "badges"); bm && bm->type == Type::Map)
            if (Value* a = as_map(*bm)->find(text)) accent = a->to_text();
          std::string cls = "w-badge", style;
          if (accent == "ok" || accent == "warn" || accent == "bad") cls += " " + accent;
          else if (!accent.empty()) style = " style=\"--c:" + safe_css(accent) + "\"";
          out += "<td><span class=\"" + cls + "\"" + style + ">" + esc(text) + "</span></td>";
        } else {
          out += std::string("<td") + (c.right ? " class=\"r\"" : "") + ">" + esc(text) + "</td>";
        }
      }
      out += "</tr>";
      ++shown;
    }
    if (rows.empty()) {
      const std::string empty = opt_str(v, "empty");
      out += "<tr><td class=\"empty\" colspan=\"" + std::to_string(std::max<size_t>(cols.size(), 1)) + "\">" + esc(empty.empty() ? "nothing here" : empty) + "</td></tr>";
    }
    out += "</tbody></table>";
    return true;
  }

  bool web_ = false;
  std::string path_;           // where the widget being rendered sits (for stable action names)
  bool shared_ = false;
  uint64_t version_ = 0;
  int refresh_ = 0;
  std::map<std::string, std::string> web_fields_;
  std::unordered_set<std::string> web_set_fields_;
  std::string css_;
  std::string theme_;
  Value routes_ = Value::nil();
  Value middleware_ = Value::nil();
  bool api_only_ = false;  // no page: only its routes answer (serve_routes)
  bool workers_ = false;   // routes run in worker VMs, not one at a time in this one

 private:
  // The program's widgets as this frame's face document, laid out at the pane's size.
  bool lay_out(std::string& error) {
    Value tree = call(view_, error);
    if (!error.empty()) { error = "view(): " + error; return false; }
    fc::Face f;
    f.name = "grain";
    actions_.clear();
    provided_ = 0;
    auto root = build(tree, error, 0);
    if (!root) { error = "view(): " + error; return false; }
    f.widgets.push_back(root);
    if (!loaded_) {
      rt_.adopt(std::move(f));
      rt_.load();
      loaded_ = true;
    } else {
      rt_.adopt(std::move(f));
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = last_ ? std::chrono::duration<double, std::milli>(now - *last_).count() : 16.0;
    last_ = now;
    rt_.frame(dt, w_, h_);
    return true;
  }

  // Calls a view or action function with the field texts when it takes one argument.
  Value call(const Value& fn, std::string& error) {
    std::vector<Value> args;
    if (fn.type == Type::Fn && fn.obj && fn.obj->is_closure) {
      const auto* c = static_cast<const ClosureObj*>(fn.obj.get());
      if (c->fn && c->fn->arity == 1) args.push_back(fields());
    }
    return vm_->call(fn, args, error);
  }

  Value fields() {
    auto m = std::make_shared<MapObj>();
    if (web_) { for (const auto& kv : web_fields_) m->put(kv.first, Value::string(kv.second)); }
    else for (const auto& kv : rt_.field_vars()) m->put(kv.first, Value::string(kv.second));
    return Value::object(Type::Map, m);
  }

  // A face action the tree named: call its grain function. An error stops the
  // event and is the program's.
  bool run(const std::string& action) {
    auto it = actions_.find(action);
    if (it == actions_.end()) return false;
    std::string err;
    const auto [fn, who] = it->second;
    call(fn, err);
    if (!err.empty() && pending_.empty()) pending_ = who + ": " + err;
    return true;
  }

  bool report(std::string& error) {
    if (!pending_.empty()) { error = pending_; pending_.clear(); return false; }
    for (const auto& f : rt_.take_faults()) { error = f; return false; }
    return true;
  }

  // `who` names it in an error: the button "Receive", the field "sku"'s on_enter.
  std::string action_for(const Value& fn, const std::string& who) {
    std::string name;
    if (web_) {
      // In a browser (or a native client) several viewers share one program, and each render happens when the data
      // changes, so an action must keep its name from one render to the next: it is where the widget sits and what
      // it says. An old page pressing a button that has since moved or changed finds no such action and does nothing.
      static const char* hex = "0123456789abcdef";
      size_t h = std::hash<std::string>()(path_ + "|" + who);
      name = "a";
      for (int k = 0; k < 12; ++k) { name += hex[h & 15]; h >>= 4; }
      while (actions_.count(name)) name += "x";
    } else {
      name = "grain_action_" + std::to_string(actions_.size() + 1);
    }
    actions_[name] = {fn, who};
    return name;
  }

  std::shared_ptr<fc::Widget> build(const Value& v, std::string& error, int depth) {
    auto m = as_map(v);
    Value* type_v = m ? m->find("widget") : nullptr;
    if (!type_v || type_v->type != Type::Str) {
      error = std::string("a widget (ui.column, ui.text, ...) is expected, got a ") + type_name(v.type);
      return nullptr;
    }
    if (depth > 200) { error = "the widgets nest more than 200 deep"; return nullptr; }
    auto w = std::make_shared<fc::Widget>();
    w->type = type_v->str();
    if (w->type == "card") w->type = "panel";
    if (Value* o = m->find("opts"); o && o->type == Type::Map) {
      for (const auto& kv : as_map(*o)->entries) {
        if (kv.first == "on_enter") continue;
        // A structured option (a table's "badges" map) is for the renderers that use it; face has no use for it.
        if (kv.second.type == Type::Map || kv.second.type == Type::List) continue;
        std::string text;
        if (!attr_text(kv.first, kv.second, text, error)) { error = "ui." + w->type + "(): " + error; return nullptr; }
        w->attrs.emplace_back(kv.first, text);
      }
      if (Value* e = as_map(*o)->find("on_enter"); e && is_callable(*e)) {
        Value* id = m->find("id");
        w->attrs.emplace_back("enter", action_for(*e, "the field \"" + (id ? id->to_text() : std::string()) + "\"'s on_enter"));
      }
    }
    if (Value* l = m->find("label")) w->label = l->to_text();
    if (Value* id = m->find("id")) w->attrs.emplace_back("id", id->str());
    if (Value* fn = m->find("on_click")) w->attrs.emplace_back("action", action_for(*fn, "the button \"" + w->label + "\""));
    if (Value* val = m->find("value")) w->attrs.emplace_back("value", val->to_text());
    Value* data = m->find("data");
    if (data && (w->type == "sparkline" || w->type == "chart")) {
      // A line's points, as face reads them: numbers, comma-separated.
      std::string pts;
      for (const auto& pt : data->list_items()) { if (!pts.empty()) pts += ","; pts += pt.type == Type::Nil ? "0" : pt.to_text(); }
      w->attrs.emplace_back("data", pts);
      if (w->type == "chart" && w->attr("height", "").empty()) w->attrs.emplace_back("height", "120");
    } else if (data) {
      // A table reads its rows by name, as `table items` reads a let.
      const std::string name = "grain_rows_" + std::to_string(++provided_);
      rt_.provide(name, to_face(*data));
      w->words.push_back(name);
    }
    if (Value* kids = m->find("children"); kids && kids->type == Type::List) {
      for (const auto& c : kids->list_items()) {
        auto child = build(c, error, depth + 1);
        if (!child) return nullptr;
        w->children.push_back(child);
      }
    }
    return w;
  }

  Value view_;
  fc::Runtime rt_;
  Vm* vm_ = nullptr;
  int w_ = 0, h_ = 0;
  std::map<std::string, std::pair<Value, std::string>> actions_;   // action -> (function, who)
  size_t provided_ = 0;
  bool loaded_ = false;
  std::optional<std::chrono::steady_clock::time_point> last_;
  std::string pending_;
};

// pane(options): title, width, height, view (a function returning the widgets).
Value bi_ui_pane(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto o = as_map(args[0]);
  if (!o) { error = std::string("ui.pane() wants its options as a map, got a ") + type_name(args[0].type); return Value::nil(); }
  Value* view = o->find("view");
  // An API with no page (serve_routes) has routes and nothing to show.
  Value no_view;
  bool api_only = false;
  if (!view && (o->find("routes") || o->find("api"))) {
    no_view = Value::object(Type::Native, std::make_shared<NativeObj>("no_view", 0, 0, [](Vm& v, std::vector<Value>&, std::string& e) {
      std::vector<Value> none{Value::list({})};
      return bi_ui_column(v, none, e);
    }));
    view = &no_view;
    api_only = true;
  }
  if (!view || !is_callable(*view)) {
    error = "ui.pane() wants \"view\": a function that returns the widgets to show";
    return Value::nil();
  }
  std::string title;
  if (Value* t = o->find("title")) title = t->to_text();
  int w = 640, h = 480;
  if (Value* v = o->find("width")) { if (!want_number(*v, error, "ui.pane")) return Value::nil(); w = (int)v->as_int(); }
  if (Value* v = o->find("height")) { if (!want_number(*v, error, "ui.pane")) return Value::nil(); h = (int)v->as_int(); }
  auto pane = std::make_shared<FacePane>(*view, w, h);
  if (Value* sh = o->find("shared")) pane->set_shared(sh->truthy());   // one copy of the program for every visitor: they all see the same live data
  if (Value* r = o->find("refresh"); r && r->is_number()) pane->set_refresh((int)r->as_int());   // a browser redraws this often (seconds); a window already redraws
  if (Value* c = o->find("css")) pane->set_css(c->to_text());
  else if (Value* s = o->find("style")) pane->set_css(s->to_text());
  if (Value* th = o->find("theme")) pane->set_theme(th->to_text());
  if (Value* r = o->find("routes")) pane->set_routes(*r);
  else if (Value* a = o->find("api")) pane->set_routes(*a);
  if (Value* mw = o->find("middleware")) pane->set_middleware(*mw);
  pane->set_api_only(api_only);
  if (Value* wk = o->find("workers")) pane->set_workers(wk->truthy());   // each route request in a VM of its own, on its own thread: a slow one does not hold up the rest
  return register_driven_pane(vm, title, w, h, pane);
}

FacePane* face_pane_of(Vm& vm, const Value& pane, std::string& error, const char* who) {
  auto d = std::dynamic_pointer_cast<FacePane>(pane_driver(vm, pane));
  if (!d) error = std::string(who) + "() wants a pane made by ui.pane()";
  return d.get();
}

// field_text(pane, id): what is typed in that field ("" before it is shown).
Value bi_ui_field_text(Vm& vm, std::vector<Value>& args, std::string& error) {
  FacePane* p = face_pane_of(vm, args[0], error, "ui.field_text");
  if (!p || !want_str(args[1], error, "ui.field_text")) return Value::nil();
  if (p->web()) return Value::string(p->web_field(args[1].str()));
  for (const auto& kv : p->runtime().field_vars())
    if (kv.first == args[1].str()) return Value::string(kv.second);
  return Value::string("");
}

// set_field(pane, id, text): replaces what is typed in that field.
Value bi_ui_set_field(Vm& vm, std::vector<Value>& args, std::string& error) {
  FacePane* p = face_pane_of(vm, args[0], error, "ui.set_field");
  if (!p || !want_str(args[1], error, "ui.set_field")) return Value::nil();
  if (p->web()) { p->web_set_field(args[1].str(), args[2].to_text()); return Value::nil(); }
  p->runtime().set_field_var(args[1].str(), args[2].to_text());
  return Value::nil();
}

// layout(pane): what the last frame put where, in order: one map per widget
// box and per run of text ({"widget", "text", "x", "y", "width", "height",
// "box"}). Empty before the pane is first drawn.
Value bi_ui_layout(Vm& vm, std::vector<Value>& args, std::string& error) {
  FacePane* p = face_pane_of(vm, args[0], error, "ui.layout");
  if (!p) return Value::nil();
  std::vector<Value> out;
  for (const auto& pl : p->runtime().placed()) {
    auto m = std::make_shared<MapObj>();
    m->put("widget", Value::string(pl.w ? pl.w->type : std::string()));
    m->put("text", Value::string(pl.box && pl.w ? pl.w->label : pl.text));
    m->put("x", Value::integer(pl.x));
    m->put("y", Value::integer(pl.y));
    m->put("width", Value::integer(pl.width));
    m->put("height", Value::integer(pl.height));
    m->put("box", Value::boolean(pl.box));
    out.push_back(Value::object(Type::Map, m));
  }
  return Value::list(std::move(out));
}

// html(pane): renders the pane's widgets to HTML string
Value bi_ui_html(Vm& vm, std::vector<Value>& args, std::string& error) {
  FacePane* p = face_pane_of(vm, args[0], error, "ui.html");
  if (!p) return Value::nil();
  std::string html;
  if (!p->render_html(vm, html, error)) return Value::nil();
  return Value::string(html);
}

}  // namespace

void install_face_builtins(Vm& vm) {
  auto ui = std::make_shared<MapObj>();
  auto add = [&](const char* name, int min_args, int max_args, NativeFn fn) {
    ui->put(name, Value::object(Type::Native, std::make_shared<NativeObj>(std::string("ui.") + name, min_args, max_args, fn)));
  };
  add("column", 1, 2, bi_ui_column);
  add("row", 1, 2, bi_ui_row);
  add("card", 1, 2, bi_ui_card);
  add("divider", 0, 1, bi_ui_divider);
  add("text", 1, 2, bi_ui_text);
  add("button", 2, 3, bi_ui_button);
  add("field", 1, 2, bi_ui_field);
  add("switch", 1, 3, bi_ui_switch);
  add("checkbox", 1, 3, bi_ui_checkbox);
  add("slider", 3, 4, bi_ui_slider);
  add("select", 2, 3, bi_ui_select);
  add("date_picker", 1, 2, bi_ui_datepicker);
  add("date", 1, 2, bi_ui_datepicker);
  add("table", 1, 2, bi_ui_table);
  add("tile", 2, 3, bi_ui_tile);
  add("heading", 1, 2, bi_ui_heading);
  add("badge", 1, 2, bi_ui_badge);
  add("progress", 1, 2, bi_ui_progress);
  add("gauge", 1, 2, bi_ui_gauge);
  add("spark", 1, 2, bi_ui_spark);
  add("chart", 1, 2, bi_ui_chart);
  add("bars", 1, 2, bi_ui_bars);
  add("pane", 1, 1, bi_ui_pane);
  add("field_text", 2, 2, bi_ui_field_text);
  add("set_field", 3, 3, bi_ui_set_field);
  add("layout", 1, 1, bi_ui_layout);
  add("html", 1, 1, bi_ui_html);
  vm.define_global("ui", Value::object(Type::Map, ui));
  vm.define_native("__route_invoke", 1, 3, [](Vm& v, std::vector<Value>& args, std::string& error) -> Value {
    if (args.empty()) return Value::nil();
    const Value& handler = args[0];
    const Value& req = args.size() > 1 ? args[1] : Value::nil();
    const Value& writer = args.size() > 2 ? args[2] : Value::nil();
    if (callable_arity(handler) == 2) {
      std::vector<Value> call_args = {req, writer};
      return v.call(handler, call_args, error);
    }
    std::vector<Value> call_args = {req};
    return v.call(handler, call_args, error);
  });
  // serve_routes({"/path": fn(req) {...}}, options?): a program that is only an API. The routes of a pane with
  // nothing to show, shared (one copy of the program) and with workers (each request in a VM of its own, so a
  // slow route does not hold up the others). options: title, workers (false for routes that share the
  // program's globals, one request at a time), and the rest of ui.pane's.
  // on_machine(member, handler): a route value answered by this app on that fleet member (a member id or its
  // prefix, as `app ... on <member>` names one), the request and the answer carried over the fleet link; on that
  // member itself, or with "" or nil for member, the handler answers here.
  vm.define_native("on_machine", 2, 2, [](Vm&, std::vector<Value>& args, std::string& error) -> Value {
    if (args[0].type != Type::Str && args[0].type != Type::Nil) {
      error = std::string("on_machine() wants a member id, got a ") + type_name(args[0].type);
      return Value::nil();
    }
    if (!is_callable(args[1])) { error = "on_machine() wants a handler as well (what answers on that machine)"; return Value::nil(); }
    if (args[0].type == Type::Nil || args[0].str().empty()) return args[1];
    auto m = std::make_shared<MapObj>();
    m->put("__on_machine", args[0]);
    m->put("__handler", args[1]);
    return Value::object(Type::Map, m);
  });
  vm.define_native("serve_routes", 1, 2, [](Vm& v, std::vector<Value>& args, std::string& error) -> Value {
    if (!as_map(args[0])) { error = std::string("serve_routes() wants its routes as a map of path to function, got a ") + type_name(args[0].type); return Value::nil(); }
    auto o = std::make_shared<MapObj>();
    o->put("title", Value::string("api"));
    o->put("shared", Value::boolean(true));
    o->put("workers", Value::boolean(true));
    if (args.size() > 1) {
      if (args[1].type == Type::List) {
        o->put("middleware", args[1]);
      } else {
        auto given = as_map(args[1]);
        if (!given) { error = std::string("serve_routes() wants its options as a map or middleware list, got a ") + type_name(args[1].type); return Value::nil(); }
        for (const auto& kv : given->entries) o->put(kv.first, kv.second);
      }
    }
    o->put("routes", args[0]);
    std::vector<Value> pane_args{Value::object(Type::Map, o)};
    return bi_ui_pane(v, pane_args, error);
  });
}


// ---- the browser side of a pane -------------------------------------------------

namespace {
FacePane* web_face(const Vm& vm, uint64_t id) {
  return dynamic_cast<FacePane*>(pane_driver(vm, Value::integer((int64_t)id)).get());
}
}  // namespace

bool web_pane_first(const Vm& vm, WebPaneInfo& out) {
  for (const auto& p : pane_list(vm)) {
    if (FacePane* f = web_face(vm, p.id)) {
      out.id = p.id;
      out.title = p.title;
      out.width = p.width;
      out.height = p.height;
      out.refresh = f->refresh();
      out.shared = f->shared();
      out.css = f->css();
      out.theme = f->theme();
      return true;
    }
  }
  return false;
}

bool web_pane_api_only(Vm& vm, uint64_t id) {
  FacePane* f = web_face(vm, id);
  return f && f->api_only();
}

bool web_pane_render(Vm& vm, uint64_t id, std::string& html, uint64_t& version, std::string& error) {
  FacePane* f = web_face(vm, id);
  if (!f) { error = "that pane is not a ui.pane"; return false; }
  if (!f->render_html(vm, html, error)) return false;
  version = f->version();
  return true;
}

bool web_pane_render_tree(Vm& vm, uint64_t id, std::string& json, uint64_t& version, std::string& error) {
  FacePane* f = web_face(vm, id);
  if (!f) { error = "that pane is not a ui.pane"; return false; }
  if (!f->render_tree(vm, json, error)) return false;
  version = f->version();
  return true;
}

bool web_pane_event(Vm& vm, uint64_t id, uint64_t version, const std::string& action,
                    const std::vector<std::pair<std::string, std::string>>& fields, std::string& error) {
  FacePane* f = web_face(vm, id);
  if (!f) { error = "that pane is not a ui.pane"; return false; }
  return f->web_event(vm, version, action, fields, error);
}

namespace {
// What a route's return value says about the response: a map with status / content_type / body / json, a
// list or number as JSON, a string as text.
void route_response(const Value& ret, int& status, std::string& res_body, std::string& content_type,
                    std::vector<std::pair<std::string, std::string>>* extra_headers = nullptr) {
  status = 200;
  content_type = "application/json; charset=utf-8";
  if (ret.type == Type::Map) {
    auto m = as_map(ret);
    if (Value* s = m->find("status")) {
      if (s->type == Type::Int && s->as_int() >= 100 && s->as_int() <= 599) {
        status = (int)s->as_int();
      }
    }
    if (Value* ct = m->find("content_type")) content_type = ct->to_text();
    if (extra_headers) {
      if (Value* h = m->find("headers")) {
        if (h->type == Type::Map) {
          auto hm = as_map(*h);
          for (const auto& [k, v] : hm->entries) {
            extra_headers->push_back({k, v.to_text()});
          }
        }
      }
    }
    if (Value* b = m->find("body")) {
      if (b->type == Type::Bytes) {
        const auto& d = b->bytes_data();
        res_body.assign(reinterpret_cast<const char*>(d.data()), d.size());
      } else if (b->type == Type::Str) {
        res_body = b->str();
      } else {
        res_body = b->to_text();
      }
    } else if (Value* j = m->find("json")) {
      std::string out;
      FacePane::value_json(*j, out, 0);
      res_body = std::move(out);
    } else {
      std::string out;
      FacePane::value_json(ret, out, 0);
      res_body = std::move(out);
    }
  } else if (ret.type == Type::Str) {
    res_body = ret.str();
    content_type = "text/plain; charset=utf-8";
  } else if (ret.type == Type::Bytes) {
    const auto& d = ret.bytes_data();
    res_body.assign(reinterpret_cast<const char*>(d.data()), d.size());
    content_type = "application/octet-stream";
  } else {
    std::string out;
    FacePane::value_json(ret, out, 0);
    res_body = std::move(out);
  }
}

static std::vector<std::string> split_route_path(const std::string& s) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == '/') ++i;
    if (i >= s.size()) break;
    size_t next = s.find('/', i);
    if (next == std::string::npos) {
      parts.push_back(s.substr(i));
      break;
    }
    parts.push_back(s.substr(i, next - i));
    i = next + 1;
  }
  return parts;
}

static std::string url_decode(const std::string& in, bool plus_as_space = false) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (plus_as_space && in[i] == '+') {
      out.push_back(' ');
    } else if (in[i] == '%' && i + 2 < in.size()) {
      auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h1 = hex_val(in[i + 1]);
      int h2 = hex_val(in[i + 2]);
      if (h1 >= 0 && h2 >= 0) {
        out.push_back((char)((h1 << 4) | h2));
        i += 2;
      } else {
        out.push_back(in[i]);
      }
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

static std::shared_ptr<MapObj> parse_query_params(const std::string& query) {
  auto m = std::make_shared<MapObj>();
  if (query.empty()) return m;
  size_t at = 0;
  while (at < query.size()) {
    size_t amp = query.find('&', at);
    std::string pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
    if (!pair.empty()) {
      size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        std::string k = url_decode(pair.substr(0, eq), true);
        std::string v = url_decode(pair.substr(eq + 1), true);
        m->put(k, Value::string(v));
      } else {
        std::string k = url_decode(pair, true);
        m->put(k, Value::string(""));
      }
    }
    if (amp == std::string::npos) break;
    at = amp + 1;
  }
  return m;
}

static Value json_to_grain_value(const aios::json::Value& j) {
  if (j.is_null()) return Value::nil();
  if (j.is_bool()) return Value::boolean(j.boolean_or());
  if (j.is_number()) {
    double d = j.num();
    int64_t i = j.i64();
    if (d == static_cast<double>(i)) return Value::integer(i);
    return Value::real(d);
  }
  if (j.is_string()) return Value::string(j.str());
  if (j.is_array()) {
    std::vector<Value> items;
    for (const auto& item : j.items()) {
      items.push_back(json_to_grain_value(item));
    }
    return Value::list(std::move(items));
  }
  if (j.is_object()) {
    auto m = std::make_shared<MapObj>();
    for (const auto& [k, v] : j.members()) {
      m->put(k, json_to_grain_value(v));
    }
    return Value::object(Type::Map, m);
  }
  return Value::nil();
}

static std::string to_upper(std::string s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

Value route_request(const std::string& method, const std::string& path, const std::string& query, const std::string& body,
                    const std::vector<std::pair<std::string, std::string>>& headers, const std::string& remote,
                    const std::shared_ptr<MapObj>& params = nullptr) {
  auto req_m = std::make_shared<MapObj>();
  req_m->put("method", Value::string(method));
  req_m->put("path", Value::string(path));
  req_m->put("query", Value::string(query));
  req_m->put("raw_query", Value::string(query));
  req_m->put("query_params", Value::object(Type::Map, parse_query_params(query)));
  req_m->put("params", Value::object(Type::Map, params ? params : std::make_shared<MapObj>()));
  req_m->put("body", Value::string(body));
  if (!body.empty()) {
    auto parsed = aios::json::parse(body);
    if (parsed.ok()) {
      Value jv = json_to_grain_value(*parsed);
      req_m->put("json", jv);
      req_m->put("body_json", jv);
    } else {
      req_m->put("json", Value::nil());
      req_m->put("body_json", Value::nil());
    }
  } else {
    req_m->put("json", Value::nil());
    req_m->put("body_json", Value::nil());
  }
  auto hdr_m = std::make_shared<MapObj>();
  for (const auto& [name, value] : headers) {
    std::string key = name;
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    if (!hdr_m->find(key)) hdr_m->put(key, Value::string(value));   // a repeated header: the first one counts
  }
  req_m->put("headers", Value::object(Type::Map, hdr_m));
  req_m->put("remote", Value::string(remote));

  auto files_m = std::make_shared<MapObj>();
  auto form_m = std::make_shared<MapObj>();

  std::string ct_val;
  for (const auto& [name, value] : headers) {
    std::string key = name;
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    if (key == "content-type") {
      ct_val = value;
      break;
    }
  }

  if (!ct_val.empty()) {
    std::string lower_ct = ct_val;
    for (char& c : lower_ct) c = (char)std::tolower((unsigned char)c);
    if (lower_ct.find("multipart/form-data") != std::string::npos) {
      std::string bnd = extract_multipart_boundary(ct_val);
      if (!bnd.empty()) {
        auto mp = parse_multipart_data(body, bnd);
        files_m = mp.files;
        form_m = mp.form;
      }
    }
  }
  req_m->put("files", Value::object(Type::Map, files_m));
  req_m->put("form", Value::object(Type::Map, form_m));
  return Value::object(Type::Map, req_m);
}

// on_machine(member, handler) makes a route value {"__on_machine": member, "__handler": handler}: the request is
// answered by the same app on that fleet member, or by the handler when this machine is that member (or none).
static const Value& on_machine_handler(const Value& v) {
  if (v.type == Type::Map && v.obj) {
    auto* m = static_cast<MapObj*>(v.obj.get());
    if (m->find("__on_machine")) {
      if (Value* h = m->find("__handler")) return *h;
    }
  }
  return v;
}

const Value* route_match(FacePane* f, const std::string& method, const std::string& path, std::shared_ptr<MapObj>& out_params) {
  out_params = std::make_shared<MapObj>();
  if (!f || f->routes().type != Type::Map) return nullptr;
  auto rm = as_map(f->routes());
  if (!rm) return nullptr;

  const std::string upper_method = to_upper(method);
  auto path_parts = split_route_path(path);

  const Value* best_handler = nullptr;
  int best_score = -1;
  std::map<std::string, std::string> best_params;

  for (const auto& [key, route_value] : rm->entries) {
    // on_machine(member, handler): the handler is what runs here (forwarding is decided before, route_machine)
    const Value& handler = on_machine_handler(route_value);
    if (!is_callable(handler)) continue;

    std::string r_method, r_pattern;
    size_t sp = key.find(' ');
    if (sp != std::string::npos) {
      r_method = to_upper(key.substr(0, sp));
      r_pattern = key.substr(sp + 1);
    } else {
      r_pattern = key;
    }

    if (!r_method.empty() && r_method != upper_method) continue;

    int method_score = r_method.empty() ? 0 : 1000;
    std::map<std::string, std::string> params;
    bool matched = false;
    int path_score = 0;

    if (r_pattern == "*") {
      matched = true;
      path_score = 1;
      params["*"] = path;
    } else if (r_pattern == path) {
      matched = true;
      path_score = 2000;
    } else {
      auto pat_parts = split_route_path(r_pattern);
      size_t i = 0;
      matched = true;
      bool consumed_all = false;
      for (; i < pat_parts.size(); ++i) {
        if (pat_parts[i] == "*") {
          std::string rest;
          for (size_t k = i; k < path_parts.size(); ++k) {
            if (!rest.empty()) rest += "/";
            rest += path_parts[k];
          }
          params["*"] = url_decode(rest);
          path_score += 5;
          consumed_all = true;
          break;
        }
        if (!pat_parts[i].empty() && pat_parts[i][0] == '*' && pat_parts[i].size() > 1) {
          std::string var_name = pat_parts[i].substr(1);
          std::string rest;
          for (size_t k = i; k < path_parts.size(); ++k) {
            if (!rest.empty()) rest += "/";
            rest += path_parts[k];
          }
          params[var_name] = url_decode(rest);
          params["*"] = url_decode(rest);
          path_score += 5;
          consumed_all = true;
          break;
        }
        if (i >= path_parts.size()) { matched = false; break; }
        if (!pat_parts[i].empty() && pat_parts[i][0] == ':') {
          std::string var_name = pat_parts[i].substr(1);
          params[var_name] = url_decode(path_parts[i]);
          path_score += 20;
        } else if (pat_parts[i] == path_parts[i]) {
          path_score += 50;
        } else {
          matched = false;
          break;
        }
      }
      if (matched && !consumed_all && (i < pat_parts.size() || i < path_parts.size())) matched = false;
    }

    if (matched) {
      int total_score = method_score + path_score;
      if (total_score > best_score) {
        best_score = total_score;
        best_handler = &handler;
        best_params = std::move(params);
      }
    }
  }

  if (best_handler) {
    for (const auto& [k, v] : best_params) {
      out_params->put(k, Value::string(v));
    }
    return best_handler;
  }
  return nullptr;
}
}  // namespace

struct RouteWriterState {
  std::mutex mu;
  std::atomic<bool> closed{false};
  bool streamed = false;
  bool headers_sent = false;
  int status = 200;
  std::string content_type;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string buffered_body;
  StreamHeaderCallback on_headers;
  StreamChunkCallback on_chunk;
};

static std::mutex g_writers_mu;
static uint64_t g_next_writer_id = 1;
static std::unordered_map<uint64_t, std::shared_ptr<RouteWriterState>> g_writers;

static uint64_t register_writer(std::shared_ptr<RouteWriterState> st) {
  std::lock_guard<std::mutex> lk(g_writers_mu);
  uint64_t id = g_next_writer_id++;
  g_writers[id] = std::move(st);
  return id;
}

static std::shared_ptr<RouteWriterState> find_writer(uint64_t id) {
  std::lock_guard<std::mutex> lk(g_writers_mu);
  auto it = g_writers.find(id);
  return it != g_writers.end() ? it->second : nullptr;
}

static void unregister_writer(uint64_t id) {
  std::lock_guard<std::mutex> lk(g_writers_mu);
  g_writers.erase(id);
}

static Value method_writer_write(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  if (!st || st->closed.load()) return Value::boolean(false);
  if (args.empty()) return Value::boolean(true);
  std::string chunk = args[0].to_text();

  std::lock_guard<std::mutex> lk(st->mu);
  if (st->closed.load()) return Value::boolean(false);
  st->streamed = true;
  if (!st->headers_sent) {
    st->headers_sent = true;
    if (st->content_type.empty()) st->content_type = "text/plain; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> hdrs = {
      {"Content-Type", st->content_type},
      {"Cache-Control", "no-cache"}
    };
    for (const auto& h : st->headers) hdrs.push_back(h);
    if (st->on_headers) {
      if (!st->on_headers(st->status, hdrs)) {
        st->closed = true;
        return Value::boolean(false);
      }
    }
  }
  if (st->on_chunk) {
    if (!st->on_chunk(chunk)) {
      st->closed = true;
      return Value::boolean(false);
    }
  } else {
    st->buffered_body += chunk;
  }
  return Value::boolean(true);
}

static Value method_writer_write_sse(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  if (!st || st->closed.load()) return Value::boolean(false);
  if (args.empty()) return Value::boolean(true);

  std::string chunk;
  if (args[0].type == Type::List) {
    auto l = std::dynamic_pointer_cast<ListObj>(args[0].obj);
    if (l) {
      for (const auto& item : l->items) format_one_sse_event(item, chunk);
    }
  } else {
    format_one_sse_event(args[0], chunk);
  }

  std::lock_guard<std::mutex> lk(st->mu);
  if (st->closed.load()) return Value::boolean(false);
  st->streamed = true;
  if (!st->headers_sent) {
    st->headers_sent = true;
    if (st->content_type.empty()) st->content_type = "text/event-stream; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> hdrs = {
      {"Content-Type", st->content_type},
      {"Cache-Control", "no-cache"},
      {"X-Accel-Buffering", "no"}
    };
    for (const auto& h : st->headers) hdrs.push_back(h);
    if (st->on_headers) {
      if (!st->on_headers(st->status, hdrs)) {
        st->closed = true;
        return Value::boolean(false);
      }
    }
  }
  if (st->on_chunk) {
    if (!st->on_chunk(chunk)) {
      st->closed = true;
      return Value::boolean(false);
    }
  } else {
    st->buffered_body += chunk;
  }
  return Value::boolean(true);
}

static Value method_writer_write_head(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  if (!st || st->closed.load()) return Value::boolean(false);
  int status = !args.empty() && args[0].is_number() ? (int)args[0].as_int() : 200;
  std::lock_guard<std::mutex> lk(st->mu);
  st->status = status;
  if (args.size() > 1 && args[1].type == Type::Map) {
    auto m = as_map(args[1]);
    if (m) {
      for (const auto& kv : m->entries) {
        std::string k_lower = kv.first;
        for (char& c : k_lower) c = (char)std::tolower((unsigned char)c);
        if (k_lower == "content-type") st->content_type = kv.second.to_text();
        else st->headers.push_back({kv.first, kv.second.to_text()});
      }
    }
  }
  st->streamed = true;
  if (!st->headers_sent) {
    st->headers_sent = true;
    if (st->content_type.empty()) st->content_type = "text/plain; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> hdrs = {
      {"Content-Type", st->content_type},
      {"Cache-Control", "no-cache"}
    };
    for (const auto& h : st->headers) hdrs.push_back(h);
    if (st->on_headers) {
      if (!st->on_headers(st->status, hdrs)) {
        st->closed = true;
        return Value::boolean(false);
      }
    }
  }
  return Value::boolean(true);
}

static Value method_writer_header(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  if (!st) return Value::nil();
  if (args.size() >= 2) {
    std::lock_guard<std::mutex> lk(st->mu);
    std::string k = args[0].to_text();
    std::string v = args[1].to_text();
    std::string k_lower = k;
    for (char& c : k_lower) c = (char)std::tolower((unsigned char)c);
    if (k_lower == "content-type") st->content_type = v;
    else st->headers.push_back({k, v});
  }
  return Value::nil();
}

static Value method_writer_status(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  if (!st) return Value::nil();
  if (!args.empty() && args[0].is_number()) {
    std::lock_guard<std::mutex> lk(st->mu);
    st->status = (int)args[0].as_int();
  }
  return Value::nil();
}

static Value method_writer_close(Vm&, BoundMethodObj& bm, std::vector<Value>&, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  if (st) {
    std::lock_guard<std::mutex> lk(st->mu);
    st->closed = true;
  }
  return Value::boolean(true);
}

static Value method_writer_flush(Vm&, BoundMethodObj& bm, std::vector<Value>&, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  return Value::boolean(st && !st->closed.load());
}

static Value method_writer_is_closed(Vm&, BoundMethodObj& bm, std::vector<Value>&, std::string&) {
  uint64_t wid = (uint64_t)bm.receiver.as_int();
  auto st = find_writer(wid);
  return Value::boolean(!st || st->closed.load());
}

static Value make_route_writer(uint64_t wid) {
  auto m = std::make_shared<MapObj>();
  Value rec = Value::integer((int64_t)wid);
  m->put("id", rec);
  m->put("is_stream", Value::boolean(true));
  m->put("write", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.write", 1, 1, method_writer_write)));
  m->put("send", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.send", 1, 1, method_writer_write)));
  m->put("write_sse", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.write_sse", 1, 1, method_writer_write_sse)));
  m->put("event", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.event", 1, 2, [](Vm& vm, BoundMethodObj& bm, std::vector<Value>& args, std::string& err) -> Value {
    if (args.size() == 2) {
      auto ev = std::make_shared<MapObj>();
      ev->put("event", args[0]);
      ev->put("data", args[1]);
      std::vector<Value> sse_args = {Value::object(Type::Map, ev)};
      return method_writer_write_sse(vm, bm, sse_args, err);
    }
    return method_writer_write_sse(vm, bm, args, err);
  })));
  m->put("write_head", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.write_head", 1, 2, method_writer_write_head)));
  m->put("header", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.header", 2, 2, method_writer_header)));
  m->put("status", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.status", 1, 1, method_writer_status)));
  m->put("close", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.close", 0, 0, method_writer_close)));
  m->put("flush", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.flush", 0, 0, method_writer_flush)));
  m->put("is_closed", Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(rec, "writer.is_closed", 0, 0, method_writer_is_closed)));
  return Value::object(Type::Map, m);
}

static Value get_pipeline_runner(Vm& vm, std::string& error) {
  static const char* kPipelineSrc =
    "let __runner = fn(middlewares, handler, req, writer) {\n"
    "  let i = 0\n"
    "  let step = nil\n"
    "  step = fn(r) {\n"
    "    let cur_req = r\n"
    "    if cur_req == nil { cur_req = req }\n"
    "    if i < len(middlewares) {\n"
    "      let m = middlewares[i]\n"
    "      i = i + 1\n"
    "      return m(cur_req, step)\n"
    "    }\n"
    "    return __route_invoke(handler, cur_req, writer)\n"
    "  }\n"
    "  return step(req)\n"
    "}\n"
    "__runner\n";
  static auto compiled = compile_source(kPipelineSrc, "<middleware-runner>", true);
  if (!compiled.ok() || !compiled.script) {
    error = "internal error initializing middleware runner";
    return Value::nil();
  }
  Value runner_script = Value::object(Type::Fn, compiled.script);
  return vm.call(runner_script, {}, error);
}

static Value execute_route_with_middleware(Vm& vm, const Value& middleware, const Value& handler, const Value& req, const Value& writer, std::string& error) {
  auto mw_list = middleware.type == Type::List ? std::dynamic_pointer_cast<ListObj>(middleware.obj) : nullptr;
  if (!mw_list || mw_list->items.empty()) {
    if (callable_arity(handler) == 2) {
      std::vector<Value> args = {req, writer};
      return vm.call(handler, args, error);
    }
    std::vector<Value> args = {req};
    return vm.call(handler, args, error);
  }
  Value runner_fn = get_pipeline_runner(vm, error);
  if (!error.empty()) return Value::nil();
  std::vector<Value> args = {middleware, handler, req, writer};
  return vm.call(runner_fn, args, error);
}

bool web_pane_handle_route(Vm& vm, uint64_t id, const std::string& method, const std::string& path,
                           const std::string& query, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& headers, const std::string& remote,
                           int& status, std::string& res_body, std::string& content_type, std::string& error,
                           std::vector<std::pair<std::string, std::string>>* extra_headers,
                           StreamHeaderCallback on_headers,
                           StreamChunkCallback on_chunk,
                           bool* out_streamed) {
  FacePane* f = web_face(vm, id);
  std::shared_ptr<MapObj> params;
  const Value* handler = route_match(f, method, path, params);
  if (!handler) return false;

  auto st = std::make_shared<RouteWriterState>();
  st->on_headers = std::move(on_headers);
  st->on_chunk = std::move(on_chunk);
  uint64_t wid = register_writer(st);
  Value writer_val = make_route_writer(wid);

  Value req = route_request(method, path, query, body, headers, remote, params);
  if (req.type == Type::Map && req.obj) {
    static_cast<MapObj*>(req.obj.get())->put("writer", writer_val);
  }

  Value ret = execute_route_with_middleware(vm, f ? f->middleware() : Value::nil(), *handler, req, writer_val, error);
  unregister_writer(wid);
  if (!error.empty()) return false;

  if (st->streamed) {
    if (out_streamed) *out_streamed = true;
    if (st->on_chunk) {
      status = st->status;
      res_body.clear();
      content_type = st->content_type;
    } else {
      status = st->status;
      res_body = std::move(st->buffered_body);
      content_type = st->content_type;
      if (extra_headers) {
        for (const auto& h : st->headers) extra_headers->push_back(h);
      }
    }
    return true;
  }
  route_response(ret, status, res_body, content_type, extra_headers);
  return true;
}

std::string web_pane_route_machine(Vm& vm, uint64_t id, const std::string& method, const std::string& path) {
  FacePane* f = web_face(vm, id);
  std::shared_ptr<MapObj> params;
  const Value* handler = route_match(f, method, path, params);
  if (!handler) return "";
  auto rm = as_map(f->routes());
  if (!rm) return "";
  for (const auto& [key, v] : rm->entries) {
    if (v.type != Type::Map || !v.obj) continue;
    auto* m = static_cast<MapObj*>(v.obj.get());
    Value* member = m->find("__on_machine");
    if (member && m->find("__handler") == handler) return member->to_text();
  }
  return "";
}

bool web_pane_route_prepare(Vm& vm, uint64_t id, const std::string& method, const std::string& path,
                            const std::string& query, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers, const std::string& remote,
                            RouteJob& job, std::string& error,
                            StreamHeaderCallback on_headers,
                            StreamChunkCallback on_chunk) {
  FacePane* f = web_face(vm, id);
  if (!f || !f->workers()) return false;
  std::shared_ptr<MapObj> params;
  const Value* handler = route_match(f, method, path, params);
  if (!handler) return false;

  auto st = std::make_shared<RouteWriterState>();
  st->on_headers = std::move(on_headers);
  st->on_chunk = std::move(on_chunk);
  uint64_t wid = register_writer(st);
  job.writer_id = wid;
  job.writer_state = st;

  Value writer_val = make_route_writer(wid);
  Value req = route_request(method, path, query, body, headers, remote, params);
  if (req.type == Type::Map && req.obj) {
    static_cast<MapObj*>(req.obj.get())->put("writer", writer_val);
  }

  auto mw_list = f->middleware().type == Type::List ? std::dynamic_pointer_cast<ListObj>(f->middleware().obj) : nullptr;
  if (mw_list && !mw_list->items.empty()) {
    Value runner_fn = get_pipeline_runner(vm, error);
    if (!error.empty()) { unregister_writer(wid); return false; }
    job.call = prepare_worker_call(vm, runner_fn, {f->middleware(), *handler, req, writer_val}, error);
  } else {
    if (callable_arity(*handler) == 2) {
      job.call = prepare_worker_call(vm, *handler, {req, writer_val}, error);
    } else {
      job.call = prepare_worker_call(vm, *handler, {req}, error);
    }
  }
  if (!job.call) { unregister_writer(wid); error = "route " + path + ": " + error; return false; }
  return true;
}

bool web_pane_route_finish(RouteJob& job, int& status, std::string& res_body, std::string& content_type, std::string& error) {
  auto st = std::static_pointer_cast<RouteWriterState>(job.writer_state);
  if (job.writer_id != 0) unregister_writer(job.writer_id);
  job.writer_id = 0;
  if (!job.call->error.empty()) { error = job.call->error; return false; }

  if (st && st->streamed) {
    job.streamed = true;
    if (st->on_chunk) {
      status = st->status;
      res_body.clear();
      content_type = st->content_type;
    } else {
      status = st->status;
      res_body = std::move(st->buffered_body);
      content_type = st->content_type;
      for (const auto& h : st->headers) job.headers.push_back(h);
    }
    return true;
  }
  route_response(job.call->result, status, res_body, content_type, &job.headers);
  return true;
}

}  // namespace aios::userland::grain
