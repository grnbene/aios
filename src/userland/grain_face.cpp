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

  static std::string jstr(const std::string& s) {
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
    if (type == "field") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      auto have = web_fields_.find(id);
      const std::string value = (have != web_fields_.end() && web_set_fields_.count(id)) ? have->second : opt_str(v, "value");
      out += ",\"id\":" + jstr(id) + ",\"value\":" + jstr(value);
      if (const Value* e = opt(v, "on_enter"); e && is_callable(*e)) out += ",\"enter\":" + jstr(action_for(*e, "the field \"" + id + "\"'s on_enter"));
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
    Value* label = m->find("label");
    const std::string label_text = label ? label->to_text() : std::string();
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
    if (Value* data = m->find("data"); data && (w->type == "sparkline" || w->type == "chart")) {
      // A line's points, as face reads them: numbers, comma-separated.
      std::string pts;
      for (const auto& v : data->list_items()) { if (!pts.empty()) pts += ","; pts += v.type == Type::Nil ? "0" : v.to_text(); }
      w->attrs.emplace_back("data", pts);
      if (w->type == "chart" && w->attr("height", "").empty()) w->attrs.emplace_back("height", "120");
    } else if (Value* data = m->find("data")) {
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
  vm.define_global("ui", Value::object(Type::Map, ui));
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

bool web_pane_handle_route(Vm& vm, uint64_t id, const std::string& method, const std::string& path,
                           const std::string& query, const std::string& body,
                           int& status, std::string& res_body, std::string& content_type, std::string& error) {
  FacePane* f = web_face(vm, id);
  if (!f || f->routes().type != Type::Map) return false;
  auto rm = as_map(f->routes());
  Value* handler = rm ? rm->find(path) : nullptr;
  if (!handler) handler = rm ? rm->find("*") : nullptr;
  if (!handler || !is_callable(*handler)) return false;

  auto req_m = std::make_shared<MapObj>();
  req_m->put("method", Value::string(method));
  req_m->put("path", Value::string(path));
  req_m->put("query", Value::string(query));
  req_m->put("body", Value::string(body));

  std::vector<Value> args = {Value::object(Type::Map, req_m)};
  Value ret = vm.call(*handler, args, error);
  if (!error.empty()) return false;

  status = 200;
  content_type = "application/json; charset=utf-8";

  if (ret.type == Type::Map) {
    auto m = as_map(ret);
    if (Value* s = m->find("status")) status = (int)s->as_int();
    if (Value* ct = m->find("content_type")) content_type = ct->to_text();
    if (Value* b = m->find("body")) {
      res_body = b->to_text();
    } else if (Value* j = m->find("json")) {
      std::string out;
      FacePane::value_json(*j, out, 0);
      res_body = std::move(out);
    } else {
      std::string out;
      FacePane::value_json(ret, out, 0);
      res_body = std::move(out);
    }
  } else if (ret.type == Type::List) {
    std::string out;
    FacePane::value_json(ret, out, 0);
    res_body = std::move(out);
  } else if (ret.type == Type::Str) {
    res_body = ret.str();
    content_type = "text/plain; charset=utf-8";
  } else {
    std::string out;
    FacePane::value_json(ret, out, 0);
    res_body = std::move(out);
  }
  return true;
}

}  // namespace aios::userland::grain
