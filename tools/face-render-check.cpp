// What faces look like, pixel for pixel: each face below is laid out and
// painted with a machine that answers the same way every run (a fixed clock,
// fixed readings, an empty store), and the frame's pixels are hashed.
//
//   aios-face-render-check                  compare with tools/face-render-check.expected
//   aios-face-render-check --write          write that file from this build (review the diff)
//   aios-face-render-check --png <dir>      also write each frame as <dir>/<name>.png, to look at
//
// It is the guard on face's layout and painting: a change meant to move no
// pixel (a refactor) must leave every hash as it was.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/face.hpp"
#include "aios/userland/web.hpp"
#include "face_render_paint.inc"   // how this build paints a Runtime into a buffer

using aios::userland::HostBindings;
using aios::userland::face::Runtime;
using aios::userland::face::parse_face;

namespace {

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const uint8_t kPng1x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x63, 0xf8, 0xff, 0xff, 0x3f, 0x00, 0x05, 0xfe, 0x02, 0xfe, 0xdc, 0xcc, 0x59, 0xe7, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

// A machine that answers the same way every run.
HostBindings fixed_host() {
  HostBindings h;
  static double clock_ms = 1792677909000.0;
  h.now_ms = []() { return clock_ms; };
  h.store_put = [](const std::string&, const std::string&) { return std::string("0000000000000000"); };
  h.store_list = [](const std::string&) { return std::string("[]"); };
  h.store_revision = []() { return uint64_t{1}; };
  h.store_get = [](const std::string&) { return std::string(); };
  h.store_similar = [](const std::string&, int) { return std::string("[]"); };
  h.store_query = [](const std::string&) { return std::string("[]"); };
  h.organ = [](const std::string&, const std::string&) { return std::string(); };
  h.storage_load = []() { return std::string(); };
  h.storage_save = [](const std::string&) {};
  h.say = [](const std::string&) {};
  h.machine = [](const std::string& key) -> std::string {
    static const std::map<std::string, std::string> m = {{"cpus", "8"}, {"arch", "aarch64"}, {"host", "render-check"},
                                                        {"mem_total", "17179869184"}, {"mem_free", "8589934592"},
                                                        {"uptime_s", "3600"}, {"caps", "24"}, {"records", "0"}};
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
  };
  h.read_file = [](const std::string& path) -> std::string {
    if (path == "logo.png") return std::string(reinterpret_cast<const char*>(kPng1x1), sizeof(kPng1x1));
    return std::string();
  };
  h.write_file = [](const std::string&, const std::string&) { return false; };
  h.list_dir = [](const std::string&) { return std::vector<std::string>(); };
  h.http_get = [](const std::string&) { return std::string(); };
  return h;
}

struct Case {
  std::string name;
  std::string source;
  int w, h;
};

const char* kWidgets = R"(face widgets
title "Every widget"
row gap=12
  tile label="Items" value=42 accent=ok
  tile label="Late" value=3 accent=warn
  tile label="Lost" value=0 accent=bad
row gap=8
  field id=sku placeholder="Scan or type a SKU" focus
  field id=qty value="1" width=80
  button "Receive" action=receive primary
  button "Clear" action=clear
row gap=10
  badge text="live" dot
  progress value=64 max=100
  sparkline data=[3, 5, 2, 8, 6, 9, 4]
tabs id=view options="Orders,Stock,People"
panel "Notes" pad=14
  text "A longer line of text that has to wrap inside its panel, with an ellipsis where a single word cannot fit into the width it was given." size=15
  text "dim text" dim
checkbox label="Shipped" id=shipped action=noop
slider id=level min=0 max=100 step=1 action=noop
table [{sku: "A-1", name: "Anchor bolt", qty: 12}, {sku: "B-22", name: "Bracket, steel, long", qty: 4}] columns="sku:SKU name:Name qty:On hand:right" rows=4
)";

const char* kPaint = R"(face painting
title "Painting"
paint height=300
on frame:
  paint rect 20 20 120 80 color="#ff007f"
  paint outline 160 20 120 80 color="#00ffcc"
  paint circle 360 60 40 color="#ffd400"
  paint ellipse 480 60 60 30 color="#44aaff"
  paint line 20 140 560 220 color="#ffffff"
  paint poly 20 260 120 180 220 260 color="#88ff44"
)";

const char* kImage = R"(face dashboard
title "Dashboard"
row gap=16
  image src="logo.png" width=120 height=80
  column gap=8
    heading "AIOS Kernel Monitor"
    text "Real-time operating system display"
)";

uint64_t fnv64(const std::vector<uint32_t>& px) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t p : px) {
    for (int i = 0; i < 4; ++i) {
      h ^= (p >> (8 * i)) & 0xFF;
      h *= 1099511628211ull;
    }
  }
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  bool write = false;
  std::string png_dir;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--write")) write = true;
    else if (!std::strcmp(argv[i], "--png") && i + 1 < argc) png_dir = argv[++i];
  }
  const std::string here = AIOS_SOURCE_ROOT;
  std::vector<Case> cases;
  for (const char* app : {"notes", "order_desk", "system_monitor", "task_tracker"})
    cases.push_back({std::string("app.") + app, read_text(here + "/apps/" + app + ".aos"), 900, 640});
  cases.push_back({"widgets", kWidgets, 900, 700});
  cases.push_back({"painting", kPaint, 700, 420});
  cases.push_back({"image", kImage, 800, 600});

  std::ostringstream report;
  for (const auto& c : cases) {
    if (c.source.empty()) { std::printf("cannot read the source of %s\n", c.name.c_str()); return 2; }
    auto face = parse_face(c.source);
    Runtime rt(face);
    rt.bind(fixed_host());
    rt.load();
    for (int f = 0; f < 3; ++f) rt.frame(16.0, c.w, c.h);
    std::vector<uint32_t> px(static_cast<size_t>(c.w) * static_cast<size_t>(c.h), 0xFF0F1318u);
    face_render_paint(rt, px, c.w, c.h);
    char line[160];
    std::snprintf(line, sizeof line, "%s %dx%d %016llx placed=%zu errors=%zu\n", c.name.c_str(), c.w, c.h,
                  static_cast<unsigned long long>(fnv64(px)), rt.placed().size(), face.errors.size());
    report << line;
    if (!png_dir.empty()) {
      auto png = aios::userland::web::encode_png(px.data(), c.w, c.h);
      std::ofstream(png_dir + "/" + c.name + ".png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
  }
  const std::string expected_path = here + "/tools/face-render-check.expected";
  if (write) {
    std::ofstream(expected_path) << report.str();
    std::printf("%swrote %s\n", report.str().c_str(), expected_path.c_str());
    return 0;
  }
  const std::string expected = read_text(expected_path);
  std::printf("%s", report.str().c_str());
  if (report.str() != expected) {
    std::printf("DIFFERS from %s:\n%s", expected_path.c_str(), expected.c_str());
    return 1;
  }
  std::printf("face render: every frame as expected\n");
  return 0;
}
