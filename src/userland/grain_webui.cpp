// A grain ui.pane program in a browser. See include/aios/userland/grain_webui.hpp.
//
// A page is the program's widgets as HTML (grain_face.cpp: web_pane_render) in a shell
// that carries a few dozen lines of script. The script sends every field's text with
// each event (a click, Enter, or - for a pane with a "refresh" option - a timer),
// and replaces the widgets with what comes back, keeping the focused field and its
// caret. The server keeps the program's state in a VM per browser session.
//
// The cookie is HttpOnly and SameSite=Strict, and an event must carry
// "X-Grain-UI: 1", which a page on another site cannot add without this server's
// consent, so a link elsewhere cannot click this program's buttons.
#include "aios/userland/grain_webui.hpp"

#include "aios/net/deflate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace aios::userland::grain {

namespace {

using Clock = std::chrono::steady_clock;

std::string random_hex(size_t bytes) {
  std::random_device rd;
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < bytes; ++i) {
    const unsigned v = rd() & 0xFF;
    out += hex[v >> 4];
    out += hex[v & 15];
  }
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size() + 0 && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
      out += (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
      i += 2;
    } else out += s[i];
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_pairs(const std::string& body) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < body.size()) {
    size_t j = body.find('&', i);
    if (j == std::string::npos) j = body.size();
    const std::string pair = body.substr(i, j - i);
    const size_t eq = pair.find('=');
    if (!pair.empty()) out.push_back({url_decode(eq == std::string::npos ? pair : pair.substr(0, eq)), eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1))});
    i = j + 1;
  }
  return out;
}

std::string json_string(const std::string& s) {
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

std::string cookie_value(const std::string& header, const std::string& name) {
  size_t at = 0;
  while (at < header.size()) {
    while (at < header.size() && (header[at] == ' ' || header[at] == ';')) ++at;
    const size_t eq = header.find('=', at);
    if (eq == std::string::npos) break;
    size_t end = header.find(';', eq);
    if (end == std::string::npos) end = header.size();
    if (header.compare(at, eq - at, name) == 0) return header.substr(eq + 1, end - eq - 1);
    at = end + 1;
  }
  return "";
}

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

const char* kStyle = R"CSS(
:root{--bg:#f6f7f9;--panel:#fff;--ink:#14181f;--dim:#5b6573;--line:#d9dde3;--accent:#2f6fed;--accent-ink:#fff;--ok:#1d8a4e;--warn:#b06a00;--bad:#c23030}
@media (prefers-color-scheme:dark){:root{--bg:#0f1318;--panel:#171c23;--ink:#e6e9ee;--dim:#9aa4b2;--line:#2a313b;--accent:#5b9bff;--accent-ink:#0b1220;--ok:#42c581;--warn:#e2a03f;--bad:#ef6b6b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
#app{max-width:var(--w,720px);margin:0 auto;padding:16px}
.w-column{display:flex;flex-direction:column}.w-row{display:flex;flex-direction:row;flex-wrap:wrap;align-items:stretch}
.w-row>.w-field{flex:1 1 160px}
.w-text.dim{color:var(--dim)}.w-text.bold{font-weight:650}
.w-btn{font:inherit;border:1px solid var(--line);background:var(--panel);color:var(--ink);border-radius:8px;padding:7px 14px;cursor:pointer}
.w-btn:hover{border-color:var(--accent)}.w-btn.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-ink);font-weight:600}
.w-field{font:inherit;border:1px solid var(--line);background:var(--panel);color:var(--ink);border-radius:8px;padding:7px 10px;min-width:0}
.w-field:focus{outline:2px solid var(--accent);outline-offset:-1px}
.w-tile{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px 14px;min-width:96px;flex:1 1 0}
.w-tile .tv{font-size:22px;font-weight:650}.w-tile .tl{font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--dim)}
.w-tile.ok .tv{color:var(--ok)}.w-tile.warn .tv{color:var(--warn)}.w-tile.bad .tv{color:var(--bad)}
.w-table{border-collapse:collapse;width:100%;background:var(--panel);border:1px solid var(--line);border-radius:10px;overflow:hidden}
.w-table th{font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--dim);text-align:left;padding:8px 12px;border-bottom:1px solid var(--line)}
.w-table td{padding:7px 12px;border-bottom:1px solid var(--line)}.w-table tr:last-child td{border-bottom:0}
.w-table .r{text-align:right}.w-table .empty{color:var(--dim);text-align:center;padding:18px}
.error{color:var(--bad);white-space:pre-wrap;font-family:ui-monospace,monospace}
.w-heading{font-size:22px;font-weight:700;letter-spacing:-.01em}
.w-badge{display:inline-flex;align-items:center;gap:6px;align-self:flex-start;font-size:11px;font-weight:650;letter-spacing:.05em;text-transform:uppercase;padding:3px 10px;border-radius:999px;color:var(--c,var(--accent));background:color-mix(in srgb,var(--c,var(--accent)) 14%,transparent)}
.w-badge.ok{--c:var(--ok)}.w-badge.warn{--c:var(--warn)}.w-badge.bad{--c:var(--bad)}
.w-badge.dot::before{content:"";width:7px;height:7px;border-radius:50%;background:currentColor}
.w-bar{display:flex;flex-direction:column;gap:5px;min-width:120px;flex:1 1 auto}
.w-bar .bl{display:flex;justify-content:space-between;font-size:12px;color:var(--dim)}.w-bar .bv{font-variant-numeric:tabular-nums;color:var(--ink);font-weight:600}
.w-bar .bt{height:10px;border-radius:6px;background:var(--line);overflow:hidden}.w-bar .bf{height:100%;border-radius:6px;transition:width .5s ease}
.w-spark,.w-chart{position:relative;background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:6px 8px;min-width:120px;flex:1 1 auto}
.w-spark svg,.w-chart svg{width:100%;height:100%;display:block}
.w-chart{padding:10px 12px 10px 44px}.w-chart .ax{position:absolute;left:8px;top:8px;bottom:8px;display:flex;flex-direction:column;justify-content:space-between;font-size:10px;color:var(--dim);font-variant-numeric:tabular-nums}
.w-tile .tv{font-variant-numeric:tabular-nums}
.w-row>.w-column{flex:1 1 0;min-width:0}
.w-bar .bl{gap:12px}.w-bar .bl span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.w-table td .w-badge{font-size:10px;padding:2px 8px}
)CSS";

// Sends the fields with every event, swaps the widgets in, keeps the focused field and its caret.
const char* kScript = R"JS(
(function(){
  var app=document.getElementById('app'),seq=app.dataset.seq,busy=false,again=null;
  function fields(){var p=new URLSearchParams();app.querySelectorAll('input[data-id]').forEach(function(i){p.append('f.'+i.dataset.id,i.value)});return p}
  function apply(d){
    if(d.html===undefined)return;
    seq=String(d.seq);
    var a=document.activeElement,id=a&&a.dataset&&a.dataset.id,s=a&&a.selectionStart,e=a&&a.selectionEnd;
    app.innerHTML=d.html;
    if(id){var n=app.querySelector('input[data-id="'+id.replace(/"/g,'\\"')+'"]');if(n){n.focus();try{n.setSelectionRange(s,e)}catch(x){}}}
    else{var f=app.querySelector('input[data-focus]');if(f&&!document.activeElement.dataset.id)f.focus()}
  }
  function send(action){
    if(busy){again=action;return}
    busy=true;var p=fields();p.append('action',action||'');
    fetch(location.pathname,{method:'POST',headers:{'X-Grain-UI':'1','Content-Type':'application/x-www-form-urlencoded'},body:p.toString(),credentials:'same-origin'})
      .then(function(r){return r.json()}).then(function(d){
        if(d.html!==undefined){if(String(d.seq)!==seq)apply(d)}
        else if(d.error){app.insertAdjacentHTML('afterbegin','<div class="error"></div>');app.firstChild.textContent=d.error}
      }).catch(function(){}).then(function(){busy=false;if(again!==null){var x=again;again=null;send(x)}});
  }
  app.addEventListener('click',function(e){var b=e.target.closest('[data-act]');if(b)send(b.dataset.act)});
  app.addEventListener('keydown',function(e){if(e.key==='Enter'&&e.target.dataset&&e.target.dataset.enter){e.preventDefault();send(e.target.dataset.enter)}});
  var f=app.querySelector('input[data-focus]');if(f)f.focus();
  // The server pushes whenever the data changes - a timer, another viewer, anything. EventSource reconnects by itself.
  if(window.EventSource){var es=new EventSource(location.pathname);es.onmessage=function(m){try{var d=JSON.parse(m.data);if(String(d.seq)!==seq&&!(document.activeElement&&document.activeElement.dataset&&document.activeElement.dataset.id&&busy))apply(d)}catch(x){}}}
  else{var r=parseInt(app.dataset.refresh||'0',10);if(r>0)setInterval(function(){if(!document.hidden)send('')},r*1000)}
})();
)JS";

// ---- a pane drawn by hand (create_pane with on_draw): its frames as PNG, pushed as they change ----------

uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool made = false;
  if (!made) {
    for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
    made = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// A frame as PNG: RGB, each row filtered by "up" (UI frames are mostly flat, so the rows repeat), deflated
// with the tree's own encoder.
std::string png_of(const std::vector<uint32_t>& argb, int w, int h) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t)h * ((size_t)w * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(2);   // Up
    for (int x = 0; x < w; ++x) {
      const uint32_t p = argb[(size_t)y * (size_t)w + (size_t)x];
      const uint32_t q = y ? argb[(size_t)(y - 1) * (size_t)w + (size_t)x] : 0;
      raw.push_back((uint8_t)(((p >> 16) & 0xFF) - ((q >> 16) & 0xFF)));
      raw.push_back((uint8_t)(((p >> 8) & 0xFF) - ((q >> 8) & 0xFF)));
      raw.push_back((uint8_t)((p & 0xFF) - (q & 0xFF)));
    }
  }
  std::string out = "\x89PNG\r\n\x1a\n";
  auto be32 = [](std::string& o, uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((char)(v >> (8 * i))); };
  auto chunk = [&](const char* type, const std::string& data) {
    be32(out, (uint32_t)data.size());
    std::string td = std::string(type, 4) + data;
    out += td;
    be32(out, crc32_of(reinterpret_cast<const uint8_t*>(td.data()), td.size()));
  };
  std::string ihdr;
  be32(ihdr, (uint32_t)w);
  be32(ihdr, (uint32_t)h);
  ihdr += std::string("\x08\x02\x00\x00\x00", 5);   // 8 bits, RGB, deflate, no interlace
  chunk("IHDR", ihdr);
  aios::ByteBuffer z{0x78, 0x01};
  net::deflate_raw_append(aios::ByteSpan(raw.data(), raw.size()), 3, z);
  uint32_t a1 = 1, a2 = 0;
  for (uint8_t b : raw) { a1 = (a1 + b) % 65521; a2 = (a2 + a1) % 65521; }
  const uint32_t adler = (a2 << 16) | a1;
  for (int i = 3; i >= 0; --i) z.push_back((uint8_t)(adler >> (8 * i)));
  chunk("IDAT", std::string(z.begin(), z.end()));
  chunk("IEND", "");
  return out;
}

std::string base64_of(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = (uint8_t)in[i] << 16;
    if (i + 1 < in.size()) v |= (uint8_t)in[i + 1] << 8;
    o += t[v >> 18]; o += t[(v >> 12) & 63];
    o += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
    o += '=';
  }
  return o;
}

// The page of a drawn pane: the frame as an image, mouse and keys sent back as the pane's own events.
const char* kCanvasScript = R"JS(
(function(){
  var img=document.getElementById('cv'),seq=img.dataset.seq;
  function apply(d){if(d.png){seq=String(d.seq);img.src='data:image/png;base64,'+d.png}}
  function post(p){
    fetch(location.pathname,{method:'POST',headers:{'X-Grain-UI':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(p).toString(),credentials:'same-origin'})
      .then(function(r){return r.json()}).then(function(d){if(String(d.seq)!==seq)apply(d)}).catch(function(){});
  }
  function pos(e){var r=img.getBoundingClientRect();return{x:Math.round((e.clientX-r.left)*img.naturalWidth/r.width),y:Math.round((e.clientY-r.top)*img.naturalHeight/r.height)}}
  var names={0:'left',1:'middle',2:'right'};
  img.addEventListener('mousedown',function(e){e.preventDefault();var p=pos(e);post({type:'mouse_down',x:p.x,y:p.y,button:names[e.button]||'left'})});
  img.addEventListener('mouseup',function(e){var p=pos(e);post({type:'mouse_up',x:p.x,y:p.y,button:names[e.button]||'left'})});
  img.addEventListener('contextmenu',function(e){e.preventDefault()});
  img.addEventListener('wheel',function(e){e.preventDefault();post({type:'wheel',dx:String(-Math.sign(e.deltaX)),dy:String(-Math.sign(e.deltaY))})},{passive:false});
  var keys={Enter:'return',Escape:'escape',Backspace:'backspace',Tab:'tab',' ':'space',ArrowLeft:'left',ArrowRight:'right',ArrowUp:'up',ArrowDown:'down',Delete:'delete',Home:'home',End:'end',PageUp:'pageup',PageDown:'pagedown'};
  document.addEventListener('keydown',function(e){
    var k=keys[e.key]||(e.key.length===1?e.key.toLowerCase():null);if(!k)return;
    if(keys[e.key]||e.ctrlKey||e.metaKey)e.preventDefault();
    post({type:'key',key:k,shift:e.shiftKey?'1':'',ctrl:e.ctrlKey?'1':'',alt:e.altKey?'1':'',meta:e.metaKey?'1':''});
    if(e.key.length===1&&!e.ctrlKey&&!e.metaKey&&!e.altKey)post({type:'text',text:e.key});
  });
  if(window.EventSource){var es=new EventSource(location.pathname);es.onmessage=function(m){try{var d=JSON.parse(m.data);if(String(d.seq)!==seq)apply(d)}catch(x){}}}
})();
)JS";

}  // namespace

struct WebUi::Session {
  std::mutex mu;                 // one event, timer or render at a time; the VM is not thread safe
  std::unique_ptr<Vm> vm;
  std::shared_ptr<void> keep;    // what the host's setup made (its files)
  std::vector<std::string> printed;
  std::string version;           // the program's version this session runs
  uint64_t pane = 0;
  WebPaneInfo info;
  Clock::time_point last = Clock::now();
  Clock::time_point last_look = Clock::now();

  // What the viewers see, and a counter that moves when it changes.
  uint64_t seq = 1;
  std::string last_tree;         // the widgets as data: what changed is decided on this
  std::string last_html;
  uint64_t html_seq = 0;         // the seq last_html is for
  std::mutex push_mu;
  std::condition_variable push_cv;
  int streams = 0;               // open event streams: a session with a viewer is not idle

  // A pane drawn by hand: its last frame, and which mouse buttons went down on it (for click).
  bool canvas = false;
  uint64_t pixels_hash = 0;
  std::string png_b64;
  Clock::time_point last_draw = Clock::now();
  bool button_down[4] = {false, false, false, false};
};

WebUi::WebUi(Host host) : host_(std::move(host)) {}

WebUi::~WebUi() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  stop_cv_.notify_all();
  if (ticker_.joinable()) ticker_.join();
}

size_t WebUi::sessions() const { std::lock_guard<std::mutex> lock(mu_); return sessions_.size() + (shared_ ? 1 : 0); }

void WebUi::report(const std::string& msg) const {
  if (log) log(msg);
  else std::fprintf(stderr, "grain-ui: %s\n", msg.c_str());
}

void WebUi::reap_locked() {
  const auto now = Clock::now();
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const bool watched = it->second->streams > 0;
    if (!watched && std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last).count() > host_.idle_seconds) it = sessions_.erase(it);
    else ++it;
  }
  while (sessions_.size() >= host_.max_sessions && !sessions_.empty()) {   // the least recently used goes
    auto oldest = sessions_.begin();
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) if (it->second->last < oldest->second->last) oldest = it;
    sessions_.erase(oldest);
  }
}

// Re-renders the widgets as data; when they differ from the last render the viewers have something new.
bool WebUi::refresh_locked(Session& s) {
  if (s.canvas) {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    std::string err;
    s.last_draw = Clock::now();
    if (!pane_frame(*s.vm, s.pane, px, w, h, err) || w < 1 || h < 1) { if (!err.empty()) report(err); return false; }
    uint64_t hsh = 1469598103934665603ULL ^ ((uint64_t)w << 32 | (uint32_t)h);
    for (uint32_t p : px) hsh = (hsh ^ p) * 1099511628211ULL;
    if (hsh == s.pixels_hash && !s.png_b64.empty()) return false;   // the same picture: nothing to send
    s.pixels_hash = hsh;
    s.info.width = w;
    s.info.height = h;
    s.png_b64 = base64_of(png_of(px, w, h));
    {
      std::lock_guard<std::mutex> lock(s.push_mu);
      ++s.seq;
    }
    s.push_cv.notify_all();
    return true;
  }
  std::string tree, err;
  uint64_t v = 0;
  if (!web_pane_render_tree(*s.vm, s.pane, tree, v, err)) { report(err); return false; }
  if (tree == s.last_tree) return false;
  s.last_tree = std::move(tree);
  {
    std::lock_guard<std::mutex> lock(s.push_mu);
    ++s.seq;
  }
  s.push_cv.notify_all();
  return true;
}

// The frame for one viewer: {"seq":N,"html":"..."} or {"seq":N,"tree":{...}}.
void WebUi::frame_locked(Session& s, bool tree, std::string& out) {
  if (s.canvas) {
    out = "{\"seq\":" + std::to_string(s.seq) + ",\"canvas\":true,\"width\":" + std::to_string(s.info.width) + ",\"height\":" +
          std::to_string(s.info.height) + ",\"png\":\"" + s.png_b64 + "\"}";
    return;
  }
  if (tree) { out = "{\"seq\":" + std::to_string(s.seq) + ",\"tree\":" + s.last_tree + "}"; return; }
  if (s.html_seq != s.seq || s.last_html.empty()) {
    std::string html, err;
    uint64_t v = 0;
    if (web_pane_render(*s.vm, s.pane, html, v, err)) { s.last_html = std::move(html); s.html_seq = s.seq; }
    else report(err);
  }
  out = "{\"seq\":" + std::to_string(s.seq) + ",\"html\":" + json_string(s.last_html) + "}";
}

void WebUi::start_ticker() {
  if (ticker_.joinable()) return;
  ticker_ = std::thread([this]() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
      stop_cv_.wait_for(lock, std::chrono::milliseconds(50));
      if (stop_) break;
      std::vector<std::shared_ptr<Session>> all;
      for (auto& kv : sessions_) all.push_back(kv.second);
      if (shared_) all.push_back(shared_);
      lock.unlock();
      for (auto& s : all) {
        const bool clocked = s->info.refresh > 0;   // a view that reads the clock: look again every `refresh` seconds
        const bool drawn = s->canvas && s->streams > 0;   // a drawn pane may move by itself: looked at 10 times a second while watched
        if (s->vm->timers.empty() && !clocked && !drawn) continue;
        std::unique_lock<std::mutex> turn(s->mu, std::try_to_lock);
        if (!turn.owns_lock()) continue;   // an event is running; the next tick
        s->vm->steps = 0;
        std::string err;
        bool look = run_timers(*s->vm, err);
        if (clocked && Clock::now() - s->last_look >= std::chrono::seconds(s->info.refresh)) { look = true; s->last_look = Clock::now(); }
        if (drawn && Clock::now() - s->last_draw >= std::chrono::milliseconds(100)) look = true;
        if (look) refresh_locked(*s);
        if (!err.empty()) report(err);
      }
      lock.lock();
    }
  });
}

std::shared_ptr<WebUi::Session> WebUi::make_session(std::string& error) {
  std::string source, version;
  if (!host_.load_source(source, version, error)) return nullptr;
  CompileResult compiled = compile_source(source, host_.origin, false);
  if (!compiled.ok()) {
    error = "the program does not compile:";
    for (const auto& d : compiled.errors) error += "\n  " + host_.origin + ":" + std::to_string(d.line) + ": " + d.message;
    return nullptr;
  }
  auto s = std::make_shared<Session>();
  s->version = version;
  s->vm = std::make_unique<Vm>();
  s->vm->sink = &s->printed;
  s->vm->step_limit = host_.max_steps;
  if (host_.setup) s->keep = host_.setup(*s->vm);
  // A window program calls run_panes() and the desktop runs the loop; here the server does, so it returns at once.
  s->vm->define_native("run_panes", 0, 1, [](Vm&, std::vector<Value>&, std::string&) { return Value::nil(); });
  auto run = s->vm->run(compiled.script);
  if (!run.ok) { error = "the program failed: " + run.error + " (line " + std::to_string(run.line) + ")"; return nullptr; }
  if (!web_pane_first(*s->vm, s->info)) {
    // No widgets: a pane it draws itself (create_pane with on_draw). Its frames go to the page as pictures.
    const auto panes = pane_list(*s->vm);
    if (panes.empty()) { error = "the program made no pane, so there is nothing to show"; return nullptr; }
    s->canvas = true;
    s->info.id = panes[0].id;
    s->info.title = panes[0].title;
    s->info.width = panes[0].width;
    s->info.height = panes[0].height;
    s->pane = s->info.id;
    s->vm->steps = 0;
    if (!refresh_locked(*s) && s->png_b64.empty()) { error = "the pane's first frame could not be drawn"; return nullptr; }
    return s;
  }
  s->pane = s->info.id;
  s->vm->steps = 0;
  std::string tree, err;
  uint64_t v = 0;
  if (!web_pane_render_tree(*s->vm, s->pane, tree, v, err)) { error = err; return nullptr; }
  s->last_tree = std::move(tree);
  return s;
}

std::shared_ptr<WebUi::Session> WebUi::session_for(const Request& req, std::string& new_id, std::string& error) {
  std::string version;
  if (!host_.current_version(version, error)) return nullptr;
  const std::string id = cookie_value(req.cookie, host_.cookie_name);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shared_ && shared_->version == version) { shared_->last = Clock::now(); return shared_; }
    if (shared_) shared_.reset();   // the program changed: the shared copy starts over
    auto it = sessions_.find(id);
    if (it != sessions_.end() && it->second->version == version) {
      it->second->last = Clock::now();
      return it->second;
    }
    if (it != sessions_.end()) sessions_.erase(it);
  }
  auto s = make_session(error);   // outside the lock: it runs the program
  if (!s) return nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  if (s->info.shared) {
    if (!shared_) shared_ = s;     // the first to finish starting is the copy everyone shares
    start_ticker();
    return shared_;
  }
  reap_locked();
  new_id = random_hex(16);
  sessions_[new_id] = s;
  start_ticker();
  return s;
}

bool WebUi::open_stream(const Request& req, Stream& out, std::vector<std::pair<std::string, std::string>>& headers,
                        std::string& first, std::string& error) {
  std::string new_id;
  std::shared_ptr<Session> s = session_for(req, new_id, error);
  if (!s) { report(error); return false; }
  headers = {{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}, {"X-Accel-Buffering", "no"}};
  if (!new_id.empty()) headers.push_back({"Set-Cookie", host_.cookie_name + "=" + new_id + "; Path=/; HttpOnly; SameSite=Strict" + (req.https ? "; Secure" : "")});
  out.session = s;
  out.tree = req.tree;
  {
    std::lock_guard<std::mutex> turn(s->mu);
    refresh_locked(*s);
    frame_locked(*s, out.tree, first);
    out.seen = s->seq;
    ++s->streams;
  }
  first = "data: " + first + "\n\n";
  return true;
}

bool WebUi::next_frame(Stream& st, int timeout_ms, std::string& frame) {
  Session& s = *st.session;
  {
    std::unique_lock<std::mutex> lock(s.push_mu);
    s.push_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return s.seq != st.seen; });
    if (s.seq == st.seen) { frame.clear(); return true; }
  }
  std::string body;
  {
    std::lock_guard<std::mutex> turn(s.mu);
    frame_locked(s, st.tree, body);
    st.seen = s.seq;
  }
  frame = "data: " + body + "\n\n";
  return true;
}

void WebUi::close_stream(Stream& st) {
  if (!st.session) return;
  std::lock_guard<std::mutex> turn(st.session->mu);
  if (st.session->streams > 0) --st.session->streams;
  st.session->last = Clock::now();
  st.session.reset();
}

WebUi::Response WebUi::handle(const Request& req) {
  Response res;
  auto fail = [&](int status, const std::string& why) {
    report(why);
    res.status = status;
    res.headers = {{"Content-Type", "text/plain; charset=utf-8"}, {"X-Content-Type-Options", "nosniff"}};
    res.body = host_.show_errors ? why + "\n" : std::string(status == 403 ? "forbidden" : status == 405 ? "method not allowed" : "internal server error");
    return res;
  };
  if (req.path == "/favicon.ico") { res.status = 204; return res; }   // a browser asks for it unprompted: not a reason to start a session
  const bool is_event = req.path == "/_ui/event" || (req.method == "POST" && req.xhr);
  if (is_event && req.method != "POST") return fail(405, "/_ui/event takes POST");
  if (!is_event && req.method != "GET" && req.method != "HEAD") return fail(405, "only GET and HEAD show the page");
  if (is_event && !req.xhr) return fail(403, "an event must carry X-Grain-UI: 1");

  std::string new_id, error;
  std::shared_ptr<Session> s = session_for(req, new_id, error);
  if (!s) return fail(500, error);
  std::string cookie_header;
  if (!new_id.empty()) cookie_header = host_.cookie_name + "=" + new_id + "; Path=/; HttpOnly; SameSite=Strict" + (req.https ? "; Secure" : "");
  auto finish_headers = [&](const char* type) {
    res.headers = {{"Content-Type", type}, {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}};
    if (!cookie_header.empty()) res.headers.push_back({"Set-Cookie", cookie_header});
  };

  std::lock_guard<std::mutex> turn(s->mu);
  s->last = Clock::now();
  Vm& vm = *s->vm;
  vm.steps = 0;

  if (is_event && s->canvas) {
    // the page's mouse and keys, as the pane's own events (a click follows a mouse_up over where it went down)
    auto m = std::make_shared<MapObj>();
    std::string type;
    for (auto& [k, val] : parse_pairs(req.body)) {
      if (k == "type") type = val;
      if (k == "x" || k == "y") m->put(k, Value::integer(std::strtoll(val.c_str(), nullptr, 10)));
      else if (k == "dx" || k == "dy") m->put(k, Value::real(std::strtod(val.c_str(), nullptr)));
      else if (k == "shift" || k == "ctrl" || k == "alt" || k == "meta") m->put(k, Value::boolean(!val.empty()));
      else m->put(k, Value::string(val));
    }
    static const char* allowed[] = {"mouse_down", "mouse_up", "mouse_move", "wheel", "key", "key_up", "text"};
    bool ok_type = false;
    for (const char* a : allowed) if (type == a) ok_type = true;
    std::string err;
    if (ok_type) {
      m->put("pane", Value::integer((int64_t)s->pane));
      if (!pane_event(vm, s->pane, Value::object(Type::Map, m), err)) report(err);
      Value* b = m->find("button");
      const std::string bn = b && b->type == Type::Str ? b->str() : "left";
      const int bi = bn == "left" ? 1 : bn == "middle" ? 2 : bn == "right" ? 3 : 0;
      if (type == "mouse_down") s->button_down[bi] = true;
      if (type == "mouse_up" && s->button_down[bi]) {
        s->button_down[bi] = false;
        auto c = std::make_shared<MapObj>(*m);
        c->put("type", Value::string("click"));
        if (!pane_event(vm, s->pane, Value::object(Type::Map, c), err)) report(err);
      }
    }
    refresh_locked(*s);
    std::string frame;
    frame_locked(*s, false, frame);
    finish_headers("application/json");
    res.body = frame;
    return res;
  }

  if (is_event) {
    std::string action;
    std::vector<std::pair<std::string, std::string>> fields;
    for (auto& [k, val] : parse_pairs(req.body)) {
      if (k == "action") action = val;
      else if (k.rfind("f.", 0) == 0) fields.push_back({k.substr(2), val});
    }
    std::string err;
    const bool ok = web_pane_event(vm, s->pane, 0, action, fields, err);
    if (!ok) report(err);
    refresh_locked(*s);   // viewers other than this one see the change too
    std::string frame;
    frame_locked(*s, req.tree, frame);
    finish_headers(req.tree ? "application/x-grain-ui+json" : "application/json");
    if (!ok) frame = frame.substr(0, frame.size() - 1) + ",\"error\":" + json_string(host_.show_errors ? err : "the program failed") + "}";
    res.body = frame;
    return res;
  }

  refresh_locked(*s);
  // ?poll=N: a native client asking whether anything changed since N, without a render or a payload
  if (req.query.rfind("poll=", 0) == 0) {
    const uint64_t since = std::strtoull(req.query.c_str() + 5, nullptr, 10);
    if (since == s->seq) { res.status = 204; return res; }
  }
  if (s->canvas) {
    if (req.tree) {   // a native client: this program draws its own pixels, which it does not show yet
      finish_headers("application/x-grain-ui+json");
      res.body = "{\"seq\":" + std::to_string(s->seq) + ",\"title\":" + json_string(s->info.title) + ",\"width\":" + std::to_string(s->info.width) +
                 ",\"height\":" + std::to_string(s->info.height) + ",\"canvas\":true}";
      return res;
    }
    std::string page = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" +
                       esc(s->info.title.empty() ? host_.origin : s->info.title) + "</title><style>" + kStyle +
                       "#cv{display:block;margin:24px auto;max-width:calc(100% - 32px);height:auto;border-radius:10px;box-shadow:0 8px 30px rgba(0,0,0,.35);user-select:none;-webkit-user-drag:none}</style></head><body>"
                       "<img id=\"cv\" alt=\"\" draggable=\"false\" width=\"" + std::to_string(s->info.width) + "\" height=\"" + std::to_string(s->info.height) +
                       "\" data-seq=\"" + std::to_string(s->seq) + "\" src=\"data:image/png;base64," + s->png_b64 + "\"><script>" + kCanvasScript + "</script></body></html>";
    res.headers = {{"Content-Type", "text/html; charset=utf-8"},
                   {"Cache-Control", "no-store"},
                   {"X-Content-Type-Options", "nosniff"},
                   {"Content-Security-Policy", "default-src 'none'; img-src data:; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'"}};
    if (!cookie_header.empty()) res.headers.push_back({"Set-Cookie", cookie_header});
    res.body = std::move(page);
    return res;
  }
  std::string frame;
  frame_locked(*s, req.tree, frame);
  if (req.tree) {
    finish_headers("application/x-grain-ui+json");
    res.body = "{\"seq\":" + std::to_string(s->seq) + ",\"title\":" + json_string(s->info.title) + ",\"width\":" + std::to_string(s->info.width) +
               ",\"height\":" + std::to_string(s->info.height) + ",\"refresh\":" + std::to_string(s->info.refresh) + ",\"shared\":" + (s->info.shared ? "true" : "false") + ",\"tree\":" + s->last_tree + "}";
    return res;
  }
  const int width = s->info.width > 0 ? s->info.width : 720;
  std::string page = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" +
                     esc(s->info.title.empty() ? host_.origin : s->info.title) + "</title><style>" + kStyle + "</style></head><body><div id=\"app\" style=\"--w:" +
                     std::to_string(width) + "px\" data-seq=\"" + std::to_string(s->seq) + "\" data-refresh=\"" + std::to_string(s->info.refresh) + "\">" + s->last_html +
                     "</div><script>" + kScript + "</script></body></html>";
  res.headers = {{"Content-Type", "text/html; charset=utf-8"},
                 {"Cache-Control", "no-store"},
                 {"X-Content-Type-Options", "nosniff"},
                 {"Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'"}};
  if (!cookie_header.empty()) res.headers.push_back({"Set-Cookie", cookie_header});
  res.body = std::move(page);
  return res;
}

}  // namespace aios::userland::grain
