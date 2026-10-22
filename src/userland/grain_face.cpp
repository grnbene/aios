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
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

// column(children) or column(options, children); row the same.
Value box(const char* type, std::vector<Value>& args, std::string& error) {
  const std::string who = std::string("ui.") + type;
  const bool with_opts = args.size() == 2;
  const Value opts = with_opts ? args[0] : Value::nil();
  const Value& kids = with_opts ? args[1] : args[0];
  if (!want_opts(opts, error, who.c_str()) || !want_widgets(kids, error, who.c_str())) return Value::nil();
  auto m = widget_map(type, opts);
  m->put("children", kids);
  return Value::object(Type::Map, m);
}

Value bi_ui_column(Vm&, std::vector<Value>& args, std::string& error) { return box("column", args, error); }
Value bi_ui_row(Vm&, std::vector<Value>& args, std::string& error) { return box("row", args, error); }

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

  bool render_html(Vm& vm, std::string& html, std::string& error) {
    vm_ = &vm;
    web_ = true;
    Value tree = call(view_, error);
    if (!error.empty()) { error = "view(): " + error; return false; }
    actions_.clear();
    ++version_;
    html.clear();
    if (!html_widget(tree, html, error, 0)) { error = "view(): " + error; return false; }
    return true;
  }

  bool web_event(Vm& vm, uint64_t version, const std::string& action,
                 const std::vector<std::pair<std::string, std::string>>& fields, std::string& error) {
    vm_ = &vm;
    web_ = true;
    for (const auto& kv : fields) web_fields_[kv.first] = kv.second;
    if (action.empty() || version != version_) return true;   // only the fields changed, or a page from before the last render
    auto it = actions_.find(action);
    if (it == actions_.end()) return true;
    std::string err;
    const auto [fn, who] = it->second;
    call(fn, err);
    if (!err.empty()) { error = who + ": " + err; return false; }
    return true;
  }

  bool web() const { return web_; }
  std::string web_field(const std::string& id) const { auto it = web_fields_.find(id); return it == web_fields_.end() ? std::string() : it->second; }
  void web_set_field(const std::string& id, const std::string& text) { web_fields_[id] = text; }

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
    if (type == "column" || type == "row") {
      out += "<div class=\"w-" + type + "\" style=\"gap:" + std::to_string(opt_int(v, "gap", 8)) + "px;padding:" + std::to_string(opt_int(v, "pad", 0)) + "px\">";
      if (Value* kids = m->find("children"); kids && kids->type == Type::List)
        for (const auto& c : kids->list_items()) if (!html_widget(c, out, error, depth + 1)) return false;
      out += "</div>";
      return true;
    }
    if (type == "text") {
      std::string cls = "w-text";
      if (opt_flag(v, "dim")) cls += " dim";
      if (opt_flag(v, "bold")) cls += " bold";
      std::string style;
      if (const int size = opt_int(v, "size", 0)) style += "font-size:" + std::to_string(size) + "px;";
      if (const std::string color = safe_css(opt_str(v, "color")); !color.empty()) style += "color:" + color + ";";
      out += "<div class=\"" + cls + "\"" + (style.empty() ? "" : " style=\"" + style + "\"") + ">" + esc(label_text) + "</div>";
      return true;
    }
    if (type == "button") {
      Value* fn = m->find("on_click");
      const std::string act = fn ? action_for(*fn, "the button \"" + label_text + "\"") : std::string();
      out += std::string("<button type=\"button\" class=\"w-btn") + (opt_flag(v, "primary") ? " primary" : "") + "\" data-act=\"" + esc(act) + "\">" + esc(label_text) + "</button>";
      return true;
    }
    if (type == "field") {
      Value* idv = m->find("id");
      const std::string id = idv ? idv->to_text() : std::string();
      auto have = web_fields_.find(id);
      const std::string value = have != web_fields_.end() ? have->second : opt_str(v, "value");
      if (have == web_fields_.end()) web_fields_[id] = value;
      std::string enter;
      if (const Value* e = opt(v, "on_enter"); e && is_callable(*e)) enter = action_for(*e, "the field \"" + id + "\"'s on_enter");
      out += "<input class=\"w-field\" type=\"text\" data-id=\"" + esc(id) + "\" value=\"" + esc(value) + "\" placeholder=\"" + esc(opt_str(v, "placeholder")) + "\"";
      if (!enter.empty()) out += " data-enter=\"" + esc(enter) + "\"";
      if (opt_flag(v, "focus")) out += " data-focus=\"1\"";
      if (const int width = opt_int(v, "width", 0)) out += " style=\"width:" + std::to_string(width) + "px;flex:none\"";
      out += " autocomplete=\"off\">";
      return true;
    }
    if (type == "tile") {
      std::string accent = opt_str(v, "accent");
      std::string cls = "w-tile", style;
      if (accent == "ok" || accent == "warn" || accent == "bad") cls += " " + accent;
      else if (!accent.empty()) style = " style=\"color:" + safe_css(accent) + "\"";
      Value* val = m->find("value");
      out += "<div class=\"" + cls + "\"><div class=\"tv\"" + style + ">" + esc(val ? val->to_text() : std::string()) + "</div><div class=\"tl\">" + esc(label_text) + "</div></div>";
      return true;
    }
    if (type == "table") return html_table(v, out, error);
    error = "ui." + type + " has no web rendering";
    return false;
  }

  // columns "key:Header, key:Header:right"; every field of the first row when left out
  bool html_table(const Value& v, std::string& out, std::string& error) {
    auto m = as_map(v);
    Value* data = m->find("data");
    if (!data || data->type != Type::List) { error = "ui.table() has no rows"; return false; }
    struct Col { std::string key, head; bool right = false; };
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
      cols.push_back(c);
    }
    const auto& rows = data->list_items();
    if (cols.empty() && !rows.empty())
      if (auto first = as_map(rows[0])) for (const auto& kv : first->entries) cols.push_back({kv.first, kv.first, false});
    out += "<table class=\"w-table\"><thead><tr>";
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
        out += std::string("<td") + (c.right ? " class=\"r\"" : "") + ">" + esc(cell ? cell->to_text() : std::string()) + "</td>";
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
  uint64_t version_ = 0;
  int refresh_ = 0;
  std::map<std::string, std::string> web_fields_;

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
    const std::string name = "grain_action_" + std::to_string(actions_.size() + 1);
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
    if (Value* o = m->find("opts"); o && o->type == Type::Map) {
      for (const auto& kv : as_map(*o)->entries) {
        if (kv.first == "on_enter") continue;
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
    if (Value* data = m->find("data")) {
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
  if (Value* r = o->find("refresh"); r && r->is_number()) pane->set_refresh((int)r->as_int());   // a browser redraws this often (seconds); a window already redraws
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
  add("text", 1, 2, bi_ui_text);
  add("button", 2, 3, bi_ui_button);
  add("field", 1, 2, bi_ui_field);
  add("table", 1, 2, bi_ui_table);
  add("tile", 2, 3, bi_ui_tile);
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

bool web_pane_event(Vm& vm, uint64_t id, uint64_t version, const std::string& action,
                    const std::vector<std::pair<std::string, std::string>>& fields, std::string& error) {
  FacePane* f = web_face(vm, id);
  if (!f) { error = "that pane is not a ui.pane"; return false; }
  return f->web_event(vm, version, action, fields, error);
}

}  // namespace aios::userland::grain
