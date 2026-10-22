// The client for a remote ui.pane program: it asks the server for the program's widgets as data
// (Accept: application/x-grain-ui), builds them into this machine's own ui.* widgets - drawn by face,
// the same as a local window - and sends a button press or Enter back as an event. No HTML, CSS or
// JavaScript is involved, and what the user sees is the native window of any other grain program.
//
//   grain open https://example.com/app.gr [--frames N] [--script events.json] [--dump]
//
// --frames/--script play scripted input and --dump prints the widgets' text afterwards: how it is tested.
#include "open_client.hpp"

namespace grain_cli {

const char* const kOpenClientSource = R"GRAIN(
let url = args[0]
let max_frames = 0
let script = []
let dump = false
let i = 1
while i < len(args) {
  if args[i] == "--frames" { max_frames = int(args[i + 1]) i += 2 }
  else if args[i] == "--script" { script = json_parse(read_file(args[i + 1])) i += 2 }
  else if args[i] == "--dump" { dump = true i += 1 }
  else { error("grain open: unknown option " + args[i]) }
}

let cookie = ""
let seq = 0
let tree = nil
let widgets = nil
let notice = ""
let sent = {}
let updates = 0
let ids = []
let info = {"title": url, "width": 640, "height": 480, "refresh": 0}
let pane = nil

# one round trip: the server's answer as data, or nil with `notice` saying why
fn call_server(method, body, target) {
  let headers = {"accept": "application/x-grain-ui", "x-grain-ui": "1"}
  if cookie != "" { headers["cookie"] = cookie }
  if method == "POST" { headers["content-type"] = "application/x-www-form-urlencoded" }
  let r = pcall(fn () { return http_request({"url": target, "method": method, "headers": headers, "body": body, "timeout_ms": 20000}) })
  if not r.ok { notice = "cannot reach " + url + ": " + str(r.error) return nil }
  let resp = r.value
  let sc = resp.headers["set-cookie"]
  if sc != nil { cookie = split(sc, ";")[0] }
  if resp.status == 204 { return nil }     # nothing has changed
  if not resp.ok { notice = "the server answered " + str(resp.status) + " " + resp.reason return nil }
  let d = pcall(fn () { return json_parse(resp.body) })
  if not d.ok or type(d.value) != "map" { notice = "that is not a grain app (expected application/x-grain-ui)" return nil }
  return d.value
}

# the widgets as data -> this machine's widgets
fn build(node) {
  let o = node.opts
  if o == nil { o = {} }
  let w = node.widget
  if w == "column" or w == "row" {
    let kids = []
    if node.children != nil { for c in node.children { push(kids, build(c)) } }
    if w == "column" { return ui.column(o, kids) }
    return ui.row(o, kids)
  }
  if w == "text" { return ui.text(node.label, o) }
  if w == "tile" { return ui.tile(node.label, node.value, o) }
  if w == "table" { return ui.table(node.data == nil ? [] : node.data, o) }
  if w == "button" {
    let act = node.act
    return ui.button(node.label, fn (f) { send(act, f) }, o)
  }
  if w == "field" {
    let oo = {}
    for k in keys(o) { oo[k] = o[k] }
    oo["value"] = node.value
    if node.enter != nil {
      let act = node.enter
      oo["on_enter"] = fn (f) { send(act, f) }
    }
    push(ids, [node.id, node.value])
    return ui.field(node.id, oo)
  }
  return ui.text("[" + w + "?]", {"dim": true})
}

fn take(d) {
  if d == nil { return nil }
  if d.error != nil { notice = d.error }
  if d.tree == nil { return nil }
  seq = d.seq
  if tree != nil { updates += 1 }
  tree = d.tree
  ids = []
  widgets = build(tree)
  # what the program changed in a field (clearing it after Enter) the window must show
  if pane != nil {
    for pair in ids {
      if sent[pair[0]] != nil and sent[pair[0]] != pair[1] { ui.set_field(pane, pair[0], pair[1]) }
    }
  }
}

fn send(action, fields) {
  let parts = ["action=" + url_encode(action)]
  for k in keys(fields) { push(parts, "f." + url_encode(k) + "=" + url_encode(fields[k])) }
  sent = fields
  take(call_server("POST", join(parts, "&"), url))
}

let first = call_server("GET", "", url)
if first == nil { error("grain open: " + notice) }
info = {"title": first.title, "width": first.width, "height": first.height}
take(first)

# The server tells us when the data changed - a timer there, another viewer, anything - and the window follows.
fn poll() {
  let sep = url == nil ? "?" : (find(url, "?") >= 0 ? "&" : "?")
  take(call_server("GET", "", url + sep + "poll=" + str(seq)))
}

fn view(f) {
  if notice != "" and widgets != nil { return ui.column({"gap": 8}, [ui.text(notice, {"color": "#ef6b6b"}), widgets]) }
  if widgets == nil { return ui.text(notice == "" ? "..." : notice) }
  return widgets
}

pane = ui.pane({"title": info.title == "" ? url : info.title, "width": info.width, "height": info.height, "view": view})
every(0.25, poll)
for e in script { e["pane"] = pane.id }
if max_frames > 0 or len(script) > 0 { run_panes({"frames": max_frames > 0 ? max_frames : 60, "script": script}) }
else { run_panes() }

fn flat(node, out) {
  if node.widget == "tile" { push(out, node.label + "=" + str(node.value)) }
  else if node.label != nil { push(out, node.label) }
  if node.widget == "field" { push(out, "[" + ui.field_text(pane, node.id) + "]") }
  if node.widget == "table" and node.data != nil { for row in node.data { push(out, json_stringify(row)) } }
  if node.children != nil { for c in node.children { flat(c, out) } }
}
if dump {
  let lines = []
  flat(tree, lines)
  for l in lines { print(l) }
  print("updates: " + str(updates))
}
)GRAIN";

}  // namespace grain_cli
