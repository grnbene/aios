// Panes as real windows on Linux, with no library: this speaks the X11
// protocol itself over the display's socket. It works on X.org and, through
// XWayland, on Wayland desktops.
//
// Each registered pane is a window; every frame its draw callback paints the
// pane's canvas and the canvas is sent to the window with PutImage (only when
// it changed). Input comes back to the pane as the event maps display_sdl.cpp
// documents:
//   {type: "mouse_down" | "mouse_up" | "click", pane, x, y, button: "left" | "middle" | "right"}
//   {type: "mouse_move", pane, x, y}
//   {type: "wheel", pane, dx, dy}   (up and right are positive)
//   {type: "key" | "key_up", pane, key: "a" | "return" | "left" | ..., shift, ctrl, alt, meta}
//   {type: "text", pane, text}
//   {type: "resize", width, height} and {type: "close"} (from grain_builtins.cpp)
// A "click" follows a mouse_up over the window the button went down in.
//
// Typed text is the key's keysym (shift, caps lock, num lock and AltGr
// applied); there are no input methods, so compose sequences and CJK input
// are not supported.
//
// Scripted input (run_panes({script: [...]})) goes to the pane one event a
// frame through the same code a person's input does. With GRAIN_VIDEO=dummy
// (or SDL_VIDEODRIVER=dummy) no display is opened: panes are drawn and
// scripted, and nothing is shown - how the window tests run without a screen.
#include "display_x11.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace grain_cli {

namespace g = aios::userland::grain;

namespace {

// ---- event maps ----------------------------------------------------------

g::Value event_map(const char* type, uint64_t pane) {
  auto m = std::make_shared<g::MapObj>();
  m->put("type", g::Value::string(type));
  m->put("pane", g::Value::integer(static_cast<int64_t>(pane)));
  return g::Value::object(g::Type::Map, m);
}

g::MapObj& fields(const g::Value& v) { return *static_cast<g::MapObj*>(v.obj.get()); }

int64_t int_field(const g::Value& ev, const char* name, int64_t dflt = 0) {
  const g::Value* v = fields(ev).find(name);
  return v && v->is_number() ? v->as_int() : dflt;
}

std::string str_field(const g::Value& ev, const char* name) {
  const g::Value* v = fields(ev).find(name);
  return v && v->type == g::Type::Str ? v->str() : std::string();
}

bool flag_field(const g::Value& ev, const char* name) {
  const g::Value* v = fields(ev).find(name);
  return v && v->truthy();
}

// ---- keysyms --------------------------------------------------------------

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// The character of a legacy keysym (Cyrillic, Greek, Arabic, Hebrew, Thai, ...), or 0.
uint32_t keysym_unicode(uint32_t ks) {
  struct Entry { uint32_t keysym, cp; };
  static const Entry table[] = {
#include "keysym_unicode.inc"
  };
  const Entry* end = table + sizeof table / sizeof table[0];
  const Entry* it = std::lower_bound(table, end, ks, [](const Entry& e, uint32_t k) { return e.keysym < k; });
  return it != end && it->keysym == ks ? it->cp : 0;
}

// The name a key event carries: what SDL called the key, lower-cased.
std::string key_name(uint32_t ks) {
  if (ks == 0x20) return "space";
  if (ks > 0x20 && ks < 0x7f) return std::string(1, static_cast<char>(std::tolower(static_cast<int>(ks))));
  if (ks >= 0xa0 && ks <= 0xff) { std::string s; append_utf8(s, ks); return s; }
  if ((ks & 0xff000000u) == 0x01000000u) { std::string s; append_utf8(s, ks & 0xffffffu); return s; }
  if (const uint32_t cp = keysym_unicode(ks)) { std::string s; append_utf8(s, cp); return s; }
  if (ks >= 0xffbe && ks <= 0xffd5) return "f" + std::to_string(ks - 0xffbe + 1);
  if (ks >= 0xffb0 && ks <= 0xffb9) return "keypad " + std::to_string(ks - 0xffb0);
  switch (ks) {
    case 0xff08: return "backspace";
    case 0xff09: return "tab";
    case 0xff0d: return "return";
    case 0xff1b: return "escape";
    case 0xffff: return "delete";
    case 0xff50: return "home";
    case 0xff51: return "left";
    case 0xff52: return "up";
    case 0xff53: return "right";
    case 0xff54: return "down";
    case 0xff55: return "pageup";
    case 0xff56: return "pagedown";
    case 0xff57: return "end";
    case 0xff63: return "insert";
    case 0xff8d: return "keypad enter";
    case 0xffaa: return "keypad *";
    case 0xffab: return "keypad +";
    case 0xffad: return "keypad -";
    case 0xffae: return "keypad .";
    case 0xffaf: return "keypad /";
    case 0xffe1: return "left shift";
    case 0xffe2: return "right shift";
    case 0xffe3: return "left ctrl";
    case 0xffe4: return "right ctrl";
    case 0xffe5: return "capslock";
    case 0xffe9: return "left alt";
    case 0xffea: return "right alt";
    case 0xffeb: return "left gui";
    case 0xffec: return "right gui";
    case 0xff7f: return "numlock";
    case 0xff14: return "scrolllock";
    case 0xff13: return "pause";
    case 0xff61: return "printscreen";
    case 0xff67: return "menu";
    default: return "unknown";
  }
}

// The keysym for a key name a script gives, so a scripted key is the same
// event a typed one is. The inverse of key_name for the names it can produce.
uint32_t keysym_of(std::string name) {
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::map<std::string, uint32_t> named = {
      {"space", 0x20},     {"backspace", 0xff08},  {"tab", 0xff09},        {"return", 0xff0d},
      {"enter", 0xff0d},   {"escape", 0xff1b},     {"delete", 0xffff},     {"home", 0xff50},
      {"left", 0xff51},    {"up", 0xff52},         {"right", 0xff53},      {"down", 0xff54},
      {"pageup", 0xff55},  {"pagedown", 0xff56},   {"end", 0xff57},        {"insert", 0xff63},
      {"left shift", 0xffe1}, {"right shift", 0xffe2}, {"left ctrl", 0xffe3}, {"right ctrl", 0xffe4},
      {"left alt", 0xffe9},   {"right alt", 0xffea},   {"left gui", 0xffeb},  {"right gui", 0xffec},
      {"capslock", 0xffe5},   {"numlock", 0xff7f},     {"keypad enter", 0xff8d}};
  auto it = named.find(name);
  if (it != named.end()) return it->second;
  if (name.size() == 1 && static_cast<unsigned char>(name[0]) > 0x20 && static_cast<unsigned char>(name[0]) < 0x7f)
    return static_cast<unsigned char>(name[0]);
  if (name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
    const int n = std::atoi(name.c_str() + 1);
    if (n >= 1 && n <= 24) return 0xffbe + static_cast<uint32_t>(n) - 1;
  }
  return 0;
}

// The code point a keysym types, or 0.
uint32_t keysym_char(uint32_t ks) {
  if (ks >= 0x20 && ks < 0x7f) return ks;
  if (ks >= 0xa0 && ks <= 0xff) return ks;
  if ((ks & 0xff000000u) == 0x01000000u) return ks & 0xffffffu;
  if (const uint32_t cp = keysym_unicode(ks)) return cp;
  if (ks >= 0xffb0 && ks <= 0xffb9) return '0' + (ks - 0xffb0);
  if (ks == 0xffaa || ks == 0xffab || ks == 0xffad || ks == 0xffae || ks == 0xffaf || ks == 0xffbd) return ks - 0xff80;
  if (ks == 0xff80) return ' ';
  return 0;
}

// ---- the protocol ---------------------------------------------------------

// X11 core request opcodes and event codes used here.
enum : uint8_t {
  kCreateWindow = 1, kDestroyWindow = 4, kMapWindow = 8, kConfigureWindow = 12, kInternAtom = 16,
  kChangeProperty = 18, kCreateGC = 55, kFreeGC = 60, kPutImage = 72, kGetKeyboardMapping = 101,
};
enum : uint8_t {
  kKeyPress = 2, kKeyRelease = 3, kButtonPress = 4, kButtonRelease = 5, kMotionNotify = 6,
  kExpose = 12, kConfigureNotify = 22, kClientMessage = 33, kMappingNotify = 34,
};

struct Message {
  uint8_t b[32] = {0};
  std::vector<uint8_t> extra;   // a reply's data past the first 32 bytes
};

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
int16_t rds16(const uint8_t* p) { return static_cast<int16_t>(rd16(p)); }

class Out {   // one request, built little-endian
 public:
  Out& u8(uint32_t v) { d_.push_back(static_cast<uint8_t>(v)); return *this; }
  Out& u16(uint32_t v) { u8(v); return u8(v >> 8); }
  Out& u32(uint32_t v) { u16(v); return u16(v >> 16); }
  Out& bytes(const void* p, size_t n) { const auto* c = static_cast<const uint8_t*>(p); d_.insert(d_.end(), c, c + n); return *this; }
  Out& pad() { while (d_.size() % 4) d_.push_back(0); return *this; }
  // The request's length field (units of 4 bytes) is bytes 2-3.
  const std::vector<uint8_t>& finish() {
    pad();
    const uint16_t n = static_cast<uint16_t>(d_.size() / 4);
    d_[2] = static_cast<uint8_t>(n);
    d_[3] = static_cast<uint8_t>(n >> 8);
    return d_;
  }
  size_t size() const { return d_.size(); }
 private:
  std::vector<uint8_t> d_;
};

struct Conn {
  int fd = -1;
  uint32_t id_base = 0, id_mask = 0, next_id = 1;
  uint32_t root = 0;
  uint16_t max_request_words = 0;
  uint8_t min_keycode = 8, max_keycode = 255;
  uint32_t wm_protocols = 0, wm_delete = 0, net_wm_name = 0, utf8_string = 0;
  std::vector<uint8_t> inbuf;
  std::deque<Message> events;
  std::vector<uint32_t> keymap;   // keysyms, keysyms_per_keycode of them per keycode from min_keycode
  int keysyms_per_keycode = 0;
  bool keymap_stale = true;
  std::string fatal;   // an X error reported by the server

  ~Conn() { if (fd >= 0) ::close(fd); }

  uint32_t alloc_id() {
    int shift = 0;
    while (shift < 32 && !((id_mask >> shift) & 1)) ++shift;
    return id_base | ((next_id++ << shift) & id_mask);
  }

  bool send_all(const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off < d.size()) {
      const ssize_t n = ::send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  // Reads what is available (or waits up to wait_ms for some) into inbuf.
  // False when the server closed the connection.
  bool fill(int wait_ms) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, wait_ms) <= 0) return true;
    uint8_t tmp[65536];
    const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    inbuf.insert(inbuf.end(), tmp, tmp + n);
    return true;
  }

  // One whole message from inbuf, if there is one. A reply carries its extra
  // data, so it needs all of it.
  bool take(Message& m) {
    if (inbuf.size() < 32) return false;
    size_t total = 32;
    if ((inbuf[0] & 0x7f) == 1) total += static_cast<size_t>(rd32(&inbuf[4])) * 4;
    if (inbuf.size() < total) return false;
    std::memcpy(m.b, inbuf.data(), 32);
    m.extra.assign(inbuf.begin() + 32, inbuf.begin() + static_cast<long>(total));
    inbuf.erase(inbuf.begin(), inbuf.begin() + static_cast<long>(total));
    return true;
  }

  // Queues every event the server has sent; an error ends the run.
  bool pump(int wait_ms) {
    if (!fill(wait_ms)) { fatal = "the display closed the connection"; return false; }
    Message m;
    while (take(m)) {
      const uint8_t code = m.b[0] & 0x7f;
      if (code == 0) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "X error %u (request %u, resource 0x%x)", m.b[1], m.b[10], rd32(m.b + 4));
        fatal = buf;
        return false;
      }
      if (code == 1) continue;   // a reply nobody waits for
      events.push_back(m);
    }
    return true;
  }

  // The reply to the next request that has one; events seen on the way queue up.
  bool reply(Message& out) {
    for (;;) {
      Message m;
      while (take(m)) {
        const uint8_t code = m.b[0] & 0x7f;
        if (code == 1) { out = m; return true; }
        if (code == 0) {
          char buf[96];
          std::snprintf(buf, sizeof buf, "X error %u (request %u)", m.b[1], m.b[10]);
          fatal = buf;
          return false;
        }
        events.push_back(m);
      }
      if (!fill(5000)) { fatal = "the display stopped answering"; return false; }
    }
  }
};

// ---- connecting -------------------------------------------------------------

struct Address {
  std::string path;       // the unix socket
  std::string number;     // the display number, as text
};

bool parse_display(const char* d, Address& a, std::string& error) {
  if (!d || !*d) { error = "DISPLAY is not set"; return false; }
  std::string s = d;
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos) { error = "DISPLAY \"" + s + "\" is not a display"; return false; }
  const std::string host = s.substr(0, colon);
  if (!host.empty() && host != "unix") { error = "DISPLAY \"" + s + "\" is on another machine; only this machine's displays are supported"; return false; }
  size_t i = colon + 1;
  size_t j = i;
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
  if (j == i) { error = "DISPLAY \"" + s + "\" has no display number"; return false; }
  a.number = s.substr(i, j - i);
  a.path = "/tmp/.X11-unix/X" + a.number;
  return true;
}

int connect_socket(const Address& a) {
  for (int abstract = 0; abstract < 2; ++abstract) {   // the file first, then the Linux abstract socket
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    socklen_t len;
    if (abstract) {
      std::memcpy(sa.sun_path + 1, a.path.c_str(), std::min(a.path.size(), sizeof(sa.sun_path) - 2));
      len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + std::min(a.path.size(), sizeof(sa.sun_path) - 2));
    } else {
      std::strncpy(sa.sun_path, a.path.c_str(), sizeof(sa.sun_path) - 1);
      len = sizeof sa;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), len) == 0) return fd;
    ::close(fd);
  }
  return -1;
}

// The MIT-MAGIC-COOKIE-1 for this display from the user's Xauthority file.
std::string read_cookie(const std::string& number) {
  std::string path;
  if (const char* x = std::getenv("XAUTHORITY"); x && *x) path = x;
  else if (const char* h = std::getenv("HOME"); h && *h) path = std::string(h) + "/.Xauthority";
  if (path.empty()) return "";
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string data;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
  std::fclose(f);
  char host[256] = {0};
  ::gethostname(host, sizeof host - 1);
  size_t p = 0;
  auto u16be = [&](uint16_t& v) { if (p + 2 > data.size()) return false; v = static_cast<uint16_t>((static_cast<uint8_t>(data[p]) << 8) | static_cast<uint8_t>(data[p + 1])); p += 2; return true; };
  auto str = [&](std::string& s) { uint16_t l; if (!u16be(l) || p + l > data.size()) return false; s = data.substr(p, l); p += l; return true; };
  for (;;) {
    uint16_t family;
    std::string addr, num, name, cookie;
    if (!u16be(family) || !str(addr) || !str(num) || !str(name) || !str(cookie)) return "";
    const bool local = family == 256 /* FamilyLocal */ && (addr == host || addr.empty());
    const bool wild = family == 65535;
    if (name == "MIT-MAGIC-COOKIE-1" && (num.empty() || num == number) && (local || wild || family == 0)) return cookie;
  }
}

bool connect_display(Conn& c, std::string& error) {
  Address a;
  if (!parse_display(std::getenv("DISPLAY"), a, error)) return false;
  c.fd = connect_socket(a);
  if (c.fd < 0) { error = "cannot connect to the display at " + a.path; return false; }
  const std::string cookie = read_cookie(a.number);
  const char* proto = cookie.empty() ? "" : "MIT-MAGIC-COOKIE-1";
  // The setup request has no length field, so it is built by hand, not by Out.
  std::vector<uint8_t> req;
  auto put16 = [&](uint32_t v) { req.push_back(static_cast<uint8_t>(v)); req.push_back(static_cast<uint8_t>(v >> 8)); };
  req.push_back('l'); req.push_back(0);
  put16(11); put16(0);
  put16(static_cast<uint32_t>(std::strlen(proto))); put16(static_cast<uint32_t>(cookie.size())); put16(0);
  req.insert(req.end(), proto, proto + std::strlen(proto));
  while (req.size() % 4) req.push_back(0);
  req.insert(req.end(), cookie.begin(), cookie.end());
  while (req.size() % 4) req.push_back(0);
  if (!c.send_all(req)) { error = "cannot talk to the display"; return false; }

  // The reply: 8 bytes, then (length * 4) more.
  while (c.inbuf.size() < 8) if (!c.fill(5000) ) { error = "the display did not answer"; return false; }
  const uint8_t status = c.inbuf[0];
  const size_t more = static_cast<size_t>(rd16(&c.inbuf[6])) * 4;
  while (c.inbuf.size() < 8 + more) if (!c.fill(5000)) { error = "the display did not answer"; return false; }
  if (status != 1) {
    const size_t reason_len = c.inbuf[1];
    error = "the display refused the connection: " + std::string(c.inbuf.begin() + 8, c.inbuf.begin() + 8 + static_cast<long>(std::min(reason_len, more)));
    return false;
  }
  const uint8_t* d = c.inbuf.data() + 8;
  c.id_base = rd32(d + 4);
  c.id_mask = rd32(d + 8);
  const size_t vendor = rd16(d + 16);
  c.max_request_words = rd16(d + 18);
  const unsigned nroots = d[20], nformats = d[21];
  const uint8_t image_order = d[22];
  c.min_keycode = d[26];
  c.max_keycode = d[27];
  if (nroots < 1) { error = "the display has no screen"; return false; }
  const uint8_t* fmt = d + 32 + ((vendor + 3) & ~size_t(3));
  const uint8_t* scr = fmt + nformats * 8;
  c.root = rd32(scr);
  const unsigned root_depth = scr[38];
  bool format_ok = false;
  for (unsigned i = 0; i < nformats; ++i)
    if (fmt[i * 8] == root_depth && fmt[i * 8 + 1] == 32) format_ok = true;
  if (image_order != 0 || !format_ok || (root_depth != 24 && root_depth != 32)) {
    error = "the display's pixel format (depth " + std::to_string(root_depth) + ", " + (image_order ? "big" : "little") + "-endian) is not one grain draws to";
    return false;
  }
  c.inbuf.erase(c.inbuf.begin(), c.inbuf.begin() + static_cast<long>(8 + more));
  if (c.max_request_words == 0) c.max_request_words = 4096;
  return true;
}

bool intern(Conn& c, const char* name, uint32_t& atom) {
  Out o;
  o.u8(kInternAtom).u8(0).u16(0).u16(static_cast<uint32_t>(std::strlen(name))).u16(0).bytes(name, std::strlen(name));
  if (!c.send_all(o.finish())) return false;
  Message m;
  if (!c.reply(m)) return false;
  atom = rd32(m.b + 8);
  return true;
}

bool load_keymap(Conn& c) {
  const unsigned count = static_cast<unsigned>(c.max_keycode) - c.min_keycode + 1;
  Out o;
  o.u8(kGetKeyboardMapping).u8(0).u16(0).u8(c.min_keycode).u8(count).u16(0);
  if (!c.send_all(o.finish())) return false;
  Message m;
  if (!c.reply(m)) return false;
  c.keysyms_per_keycode = m.b[1];
  c.keymap.clear();
  for (size_t i = 0; i + 4 <= m.extra.size(); i += 4) c.keymap.push_back(rd32(&m.extra[i]));
  c.keymap_stale = false;
  return c.keysyms_per_keycode > 0;
}

// ---- the display ----------------------------------------------------------

struct Window {
  uint32_t xid = 0, gc = 0;
  int width = 0, height = 0;          // the canvas, as last drawn
  std::vector<uint32_t> last;         // what the window shows now
  bool force = true;                  // paint even if the canvas is unchanged
  bool button_down[8] = {false};
};

class X11Display final : public g::Display {
 public:
  bool run(g::Vm& vm, const g::DisplayRun& opts, std::string& error) override {
    const char* v = std::getenv("GRAIN_VIDEO");
    const char* s = std::getenv("SDL_VIDEODRIVER");
    headless_ = (v && std::string(v) == "dummy") || (s && std::string(s) == "dummy");
    if (!headless_) {
      std::string why;
      if (!connect_display(conn_, why)) { error = "run_panes(): cannot open the display: " + why; return false; }
      if (!intern(conn_, "WM_PROTOCOLS", conn_.wm_protocols) || !intern(conn_, "WM_DELETE_WINDOW", conn_.wm_delete) ||
          !intern(conn_, "_NET_WM_NAME", conn_.net_wm_name) || !intern(conn_, "UTF8_STRING", conn_.utf8_string)) {
        error = "run_panes(): cannot open the display: " + conn_.fatal;
        return false;
      }
      if (!load_keymap(conn_)) { error = "run_panes(): cannot read the keyboard layout: " + conn_.fatal; return false; }
    }
    const bool ok = loop(vm, opts, error);
    if (!headless_) {
      for (auto& [id, w] : windows_) destroy(w);
    }
    windows_.clear();
    conn_ = Conn();
    return ok;
  }

 private:
  Conn conn_;
  bool headless_ = false;
  std::map<uint64_t, Window> windows_;
  uint64_t next_fake_ = 1;

  void destroy(Window& w) {
    if (headless_ || !w.xid) return;
    Out a;
    a.u8(kFreeGC).u8(0).u16(0).u32(w.gc);
    conn_.send_all(a.finish());
    Out b;
    b.u8(kDestroyWindow).u8(0).u16(0).u32(w.xid);
    conn_.send_all(b.finish());
  }

  uint64_t pane_of(uint32_t xid) const {
    for (const auto& [id, w] : windows_)
      if (w.xid == xid) return id;
    return 0;
  }

  bool create(const g::PaneInfo& p, Window& w, std::string& error) {
    w.width = p.width;
    w.height = p.height;
    if (headless_) return true;
    Conn& c = conn_;
    w.xid = c.alloc_id();
    w.gc = c.alloc_id();
    constexpr uint32_t kEvents = 0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x8000 | 0x20000;   // keys, buttons, motion, expose, structure
    Out cw;
    cw.u8(kCreateWindow).u8(0).u16(0).u32(w.xid).u32(c.root).u16(0).u16(0)
        .u16(static_cast<uint32_t>(p.width)).u16(static_cast<uint32_t>(p.height)).u16(0)
        .u16(1 /* InputOutput */).u32(0 /* CopyFromParent */).u32(0x800 /* event-mask */).u32(kEvents);
    Out gc;
    gc.u8(kCreateGC).u8(0).u16(0).u32(w.gc).u32(w.xid).u32(0);
    if (!c.send_all(cw.finish()) || !c.send_all(gc.finish())) { error = "run_panes(): cannot open a window for \"" + p.title + "\""; return false; }
    set_title(w, p.title);
    Out proto;   // ask to be told when the window is closed, instead of being disconnected
    proto.u8(kChangeProperty).u8(0).u16(0).u32(w.xid).u32(c.wm_protocols).u32(4 /* ATOM */).u8(32).pad().u32(1).u32(c.wm_delete);
    // A class lets desktops group the window and find its launcher.
    static const char cls[] = "grain\0Grain";
    Out klass;
    klass.u8(kChangeProperty).u8(0).u16(0).u32(w.xid).u32(67 /* WM_CLASS */).u32(31 /* STRING */).u8(8).pad().u32(sizeof cls).bytes(cls, sizeof cls);
    Out map;
    map.u8(kMapWindow).u8(0).u16(0).u32(w.xid);
    if (!c.send_all(proto.finish()) || !c.send_all(klass.finish()) || !c.send_all(map.finish())) {
      error = "run_panes(): the display closed while opening \"" + p.title + "\"";
      return false;
    }
    return true;
  }

  void set_title(const Window& w, const std::string& title) {
    Out a;   // WM_NAME
    a.u8(kChangeProperty).u8(0).u16(0).u32(w.xid).u32(39 /* WM_NAME */).u32(31 /* STRING */).u8(8).pad().u32(static_cast<uint32_t>(title.size())).bytes(title.data(), title.size());
    Out b;   // _NET_WM_NAME, which is UTF-8
    b.u8(kChangeProperty).u8(0).u16(0).u32(w.xid).u32(conn_.net_wm_name).u32(conn_.utf8_string).u8(8).pad().u32(static_cast<uint32_t>(title.size())).bytes(title.data(), title.size());
    conn_.send_all(a.finish());
    conn_.send_all(b.finish());
  }

  // Windows for new panes; none for panes the program closed.
  bool sync(const g::Vm& vm, std::string& error) {
    const auto panes = g::pane_list(vm);
    for (auto it = windows_.begin(); it != windows_.end();) {
      const bool alive = std::any_of(panes.begin(), panes.end(), [&](const g::PaneInfo& p) { return p.id == it->first; });
      if (alive) { ++it; continue; }
      destroy(it->second);
      it = windows_.erase(it);
    }
    for (const auto& p : panes) {
      if (windows_.count(p.id)) continue;
      Window w;
      if (!create(p, w, error)) return false;
      windows_[p.id] = std::move(w);
    }
    return true;
  }

  // ---- input ----------------------------------------------------------------

  g::Value key_event(const char* type, uint64_t pane, const std::string& key, bool shift, bool ctrl, bool alt, bool meta) {
    g::Value ev = event_map(type, pane);
    fields(ev).put("key", g::Value::string(key));
    fields(ev).put("shift", g::Value::boolean(shift));
    fields(ev).put("ctrl", g::Value::boolean(ctrl));
    fields(ev).put("alt", g::Value::boolean(alt));
    fields(ev).put("meta", g::Value::boolean(meta));
    return ev;
  }

  bool mouse_button(g::Vm& vm, uint64_t pane, bool down, int x, int y, const std::string& button, std::string& error) {
    g::Value ev = event_map(down ? "mouse_down" : "mouse_up", pane);
    fields(ev).put("x", g::Value::integer(x));
    fields(ev).put("y", g::Value::integer(y));
    fields(ev).put("button", g::Value::string(button));
    if (!g::pane_event(vm, pane, ev, error)) return false;
    auto it = windows_.find(pane);
    if (it == windows_.end()) return true;
    const int b = button == "left" ? 1 : button == "middle" ? 2 : button == "right" ? 3 : 0;
    if (down) { it->second.button_down[b] = true; return true; }
    if (!it->second.button_down[b]) return true;
    it->second.button_down[b] = false;
    g::Value click = event_map("click", pane);
    fields(click).put("x", g::Value::integer(x));
    fields(click).put("y", g::Value::integer(y));
    fields(click).put("button", g::Value::string(button));
    return g::pane_event(vm, pane, click, error);
  }

  bool wheel(g::Vm& vm, uint64_t pane, double dx, double dy, std::string& error) {
    g::Value ev = event_map("wheel", pane);
    fields(ev).put("dx", g::Value::real(dx));
    fields(ev).put("dy", g::Value::real(dy));
    return g::pane_event(vm, pane, ev, error);
  }

  bool text(g::Vm& vm, uint64_t pane, const std::string& t, std::string& error) {
    g::Value ev = event_map("text", pane);
    fields(ev).put("text", g::Value::string(t));
    return g::pane_event(vm, pane, ev, error);
  }

  uint32_t keysym(unsigned keycode, int column) const {
    if (keycode < conn_.min_keycode || keycode > conn_.max_keycode || column < 0 || column >= conn_.keysyms_per_keycode) return 0;
    const size_t i = static_cast<size_t>(keycode - conn_.min_keycode) * static_cast<size_t>(conn_.keysyms_per_keycode) + static_cast<size_t>(column);
    return i < conn_.keymap.size() ? conn_.keymap[i] : 0;
  }

  // The keysym a key has in a layout (group 0 or 1) at a level: shifted or not,
  // and with AltGr. The core keymap lists group 1's two levels, group 2's two,
  // then the AltGr levels of each. A layout missing a level falls back to the
  // level below it, then to the first layout.
  uint32_t keysym_at(unsigned keycode, int group, bool shift, bool altgr) const {
    for (const int g : {group, 0}) {
      // What was asked for, then the key without AltGr, then without shift: a
      // layout that does not define a level leaves the key as it is below it.
      const int levels[3] = {(altgr ? 2 : 0) + (shift ? 1 : 0), shift ? 1 : 0, 0};
      for (const int level : levels) {
        const int column = level < 2 ? g * 2 + level : 4 + g * 2 + (level - 2);
        if (const uint32_t ks = keysym(keycode, column)) return ks;
      }
    }
    return 0;
  }

  // Do `lo` and `up` (code points) make a pair Caps Lock should swap? Letters do; a
  // key that is a digit or punctuation in both layers does not.
  static bool case_pair(uint32_t lo, uint32_t up) {
    if (lo < 0x80 && up < 0x80) return lo >= 'a' && lo <= 'z' && up == lo - 32;
    return lo != up;
  }

  // A key press or release from the display: the key event, then (on a press)
  // the text it types.
  bool key(g::Vm& vm, const Message& m, bool press, std::string& error) {
    const uint64_t pane = pane_of(rd32(m.b + 12));
    if (!pane) return true;
    if (conn_.keymap_stale && !load_keymap(conn_)) { error = "run_panes(): " + conn_.fatal; return false; }
    const unsigned code = m.b[1];
    const unsigned state = rd16(m.b + 28);
    const bool shift = state & 1, caps = state & 2, ctrl = state & 4, alt = state & 8, num = state & 0x10, meta = state & 0x40, altgr = state & 0x80;
    // Bits 13-14 of the state are the active layout (the display switches it, e.g. Alt+Shift).
    const int group = ((state >> 13) & 3) < 2 ? static_cast<int>((state >> 13) & 3) : 0;
    const uint32_t base = keysym_at(code, group, false, false);
    if (!base) return true;
    if (!g::pane_event(vm, pane, key_event(press ? "key" : "key_up", pane, key_name(base), shift, ctrl, alt, meta), error)) return false;
    if (!press || ctrl || alt || meta) return true;
    // What the key types: the symbol its layout, shift and AltGr pick.
    uint32_t ks = base;
    const bool keypad = (base >= 0xff95 && base <= 0xff9f) || (base >= 0xffb0 && base <= 0xffb9);
    if (keypad) {
      ks = (num != shift) ? keysym(code, 1) : base;
    } else if (altgr && keysym_at(code, group, shift, true) != keysym_at(code, group, shift, false)) {
      ks = keysym_at(code, group, shift, true);
    } else {
      const uint32_t up = keysym_at(code, group, true, false);
      const bool upper = up && case_pair(keysym_char(base), keysym_char(up)) ? (shift != caps) : shift;
      ks = upper && up ? up : base;
    }
    const uint32_t cp = keysym_char(ks);
    if (!cp) return true;
    std::string t;
    append_utf8(t, cp);
    return text(vm, pane, t, error);
  }

  // One event from the display to the pane it belongs to.
  bool translate(g::Vm& vm, const Message& m, std::string& error) {
    switch (m.b[0] & 0x7f) {
      case kKeyPress: return key(vm, m, true, error);
      case kKeyRelease: return key(vm, m, false, error);
      case kButtonPress:
      case kButtonRelease: {
        const uint64_t pane = pane_of(rd32(m.b + 12));
        if (!pane) return true;
        const bool down = (m.b[0] & 0x7f) == kButtonPress;
        const int x = rds16(m.b + 24), y = rds16(m.b + 26);
        switch (m.b[1]) {
          case 1: return mouse_button(vm, pane, down, x, y, "left", error);
          case 2: return mouse_button(vm, pane, down, x, y, "middle", error);
          case 3: return mouse_button(vm, pane, down, x, y, "right", error);
          case 4: return down ? wheel(vm, pane, 0, 1, error) : true;     // up
          case 5: return down ? wheel(vm, pane, 0, -1, error) : true;    // down
          case 6: return down ? wheel(vm, pane, -1, 0, error) : true;    // left
          case 7: return down ? wheel(vm, pane, 1, 0, error) : true;     // right
          default: return true;
        }
      }
      case kMotionNotify: {
        const uint64_t pane = pane_of(rd32(m.b + 12));
        if (!pane) return true;
        g::Value ev = event_map("mouse_move", pane);
        fields(ev).put("x", g::Value::integer(rds16(m.b + 24)));
        fields(ev).put("y", g::Value::integer(rds16(m.b + 26)));
        return g::pane_event(vm, pane, ev, error);
      }
      case kExpose: {
        const uint64_t pane = pane_of(rd32(m.b + 4));
        auto it = windows_.find(pane);
        if (it != windows_.end()) it->second.force = true;
        return true;
      }
      case kConfigureNotify: {
        const uint64_t pane = pane_of(rd32(m.b + 8));
        auto it = windows_.find(pane);
        if (it == windows_.end()) return true;
        const int w = rd16(m.b + 20), h = rd16(m.b + 22);
        if (w < 1 || h < 1 || (w == it->second.width && h == it->second.height)) return true;
        it->second.force = true;
        return g::pane_resized(vm, pane, w, h, error);
      }
      case kClientMessage: {
        if (rd32(m.b + 8) != conn_.wm_protocols || rd32(m.b + 12) != conn_.wm_delete) return true;
        const uint64_t pane = pane_of(rd32(m.b + 4));
        return pane ? g::pane_closed(vm, pane, error) : true;
      }
      case kMappingNotify:
        conn_.keymap_stale = true;
        return true;
      default:
        return true;
    }
  }

  // A scripted event, to the pane as the window system would deliver it.
  bool scripted(g::Vm& vm, const g::Value& ev, std::string& error) {
    const std::string type = str_field(ev, "type");
    const uint64_t pane = static_cast<uint64_t>(int_field(ev, "pane"));
    auto it = windows_.find(pane);
    if (it == windows_.end()) { error = "run_panes(): a scripted \"" + type + "\" names pane " + std::to_string(pane) + ", which has no window"; return false; }
    const int x = static_cast<int>(int_field(ev, "x")), y = static_cast<int>(int_field(ev, "y"));
    if (type == "mouse_down" || type == "mouse_up") return mouse_button(vm, pane, type == "mouse_down", x, y, str_field(ev, "button").empty() ? "left" : str_field(ev, "button"), error);
    if (type == "mouse_move") {
      g::Value m = event_map("mouse_move", pane);
      fields(m).put("x", g::Value::integer(x));
      fields(m).put("y", g::Value::integer(y));
      return g::pane_event(vm, pane, m, error);
    }
    if (type == "wheel") {
      const g::Value* dx = fields(ev).find("dx");
      const g::Value* dy = fields(ev).find("dy");
      return wheel(vm, pane, dx && dx->is_number() ? dx->as_double() : 0, dy && dy->is_number() ? dy->as_double() : 0, error);
    }
    if (type == "key" || type == "key_up") {
      const std::string name = str_field(ev, "key");
      if (keysym_of(name) == 0) { error = "run_panes(): a scripted key \"" + name + "\" is not a key grain knows"; return false; }
      return g::pane_event(vm, pane, key_event(type.c_str(), pane, key_name(keysym_of(name)), flag_field(ev, "shift"), flag_field(ev, "ctrl"), flag_field(ev, "alt"), flag_field(ev, "meta")), error);
    }
    if (type == "text") return text(vm, pane, str_field(ev, "text"), error);
    if (type == "resize") {
      const int w = static_cast<int>(int_field(ev, "width", 1)), h = static_cast<int>(int_field(ev, "height", 1));
      if (headless_) {
        it->second.force = true;
        return g::pane_resized(vm, pane, std::max(w, 1), std::max(h, 1), error);
      }
      Out o;   // the size change comes back from the display as its own event
      o.u8(kConfigureWindow).u8(0).u16(0).u32(it->second.xid).u16(0x4 | 0x8).u16(0).u32(static_cast<uint32_t>(std::max(w, 1))).u32(static_cast<uint32_t>(std::max(h, 1)));
      if (!conn_.send_all(o.finish())) { error = "run_panes(): the display closed"; return false; }
      return true;
    }
    if (type == "close") return g::pane_closed(vm, pane, error);
    error = "run_panes(): a scripted event of type \"" + type + "\" is not one a window sends";
    return false;
  }

  // ---- drawing --------------------------------------------------------------

  bool draw(g::Vm& vm, uint64_t id, Window& w, std::string& error) {
    std::vector<uint32_t> px;
    int pw = 0, ph = 0;
    if (!g::pane_frame(vm, id, px, pw, ph, error)) return false;
    if (pw < 1 || ph < 1) return true;
    if (headless_) { w.width = pw; w.height = ph; return true; }
    const bool same = !w.force && pw == w.width && ph == w.height && w.last.size() == px.size() &&
                      std::memcmp(w.last.data(), px.data(), px.size() * sizeof(uint32_t)) == 0;
    w.width = pw;
    w.height = ph;
    if (same) return true;
    // PutImage takes a whole request; a big canvas goes in strips of rows.
    const size_t max_bytes = static_cast<size_t>(conn_.max_request_words) * 4 - 24;
    const int rows = std::max<int>(1, static_cast<int>(max_bytes / (static_cast<size_t>(pw) * 4)));
    for (int y = 0; y < ph; y += rows) {
      const int n = std::min(rows, ph - y);
      Out o;
      o.u8(kPutImage).u8(2 /* ZPixmap */).u16(0).u32(w.xid).u32(w.gc).u16(static_cast<uint32_t>(pw)).u16(static_cast<uint32_t>(n))
          .u16(0).u16(static_cast<uint32_t>(y)).u8(0).u8(24).u16(0);
      // The request must be one block; Out only appends, so write the pixels in.
      o.bytes(px.data() + static_cast<size_t>(y) * static_cast<size_t>(pw), static_cast<size_t>(n) * static_cast<size_t>(pw) * 4);
      if (!conn_.send_all(o.finish())) { error = "run_panes(): the display closed"; return false; }
    }
    w.last = std::move(px);
    w.force = false;
    return true;
  }

  bool loop(g::Vm& vm, const g::DisplayRun& opts, std::string& error) {
    using clock = std::chrono::steady_clock;
    constexpr auto kFrame = std::chrono::milliseconds(16);   // about 60 frames a second
    size_t next_script = 0;
    for (int64_t frame = 0; opts.max_frames <= 0 || frame < opts.max_frames; ++frame) {
      const auto started = clock::now();
      if (!sync(vm, error)) return false;
      if (windows_.empty()) return true;   // every pane closed: run_panes() is done
      if (next_script < opts.script.size() && !scripted(vm, opts.script[next_script++], error)) return false;
      if (!headless_) {
        if (!conn_.pump(0)) { error = "run_panes(): " + conn_.fatal; return false; }
        // Auto-repeat arrives as a release and a press with the same time and
        // key: the release is not real, so it is dropped.
        while (!conn_.events.empty()) {
          Message m = conn_.events.front();
          conn_.events.pop_front();
          if ((m.b[0] & 0x7f) == kKeyRelease && !conn_.events.empty()) {
            const Message& n = conn_.events.front();
            if ((n.b[0] & 0x7f) == kKeyPress && n.b[1] == m.b[1] && rd32(n.b + 4) == rd32(m.b + 4)) continue;
          }
          if (!translate(vm, m, error)) return false;
        }
      }
      if (!sync(vm, error)) return false;
      for (auto& [id, w] : windows_)
        if (!draw(vm, id, w, error)) return false;
      if (headless_) {
        const auto spent = clock::now() - started;
        if (spent < kFrame) std::this_thread::sleep_for(kFrame - spent);
      } else {
        // Wait out the frame, reading what the display sends meanwhile so its buffer never fills.
        for (auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kFrame - (clock::now() - started)); left.count() > 0;
             left = std::chrono::duration_cast<std::chrono::milliseconds>(kFrame - (clock::now() - started)))
          if (!conn_.pump(static_cast<int>(left.count()))) { error = "run_panes(): " + conn_.fatal; return false; }
      }
    }
    return true;
  }
};

}  // namespace

g::Display& x11_display() {
  static X11Display d;
  return d;
}

}  // namespace grain_cli
