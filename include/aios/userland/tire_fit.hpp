// Tires as solids, and how many of one fit a bin: shared by ASQL (TIRE_FIT, TIRE_WIDTH_MM, TIRE_DIAMETER_MM) and
// grain (tire_size, tire_fit), so a capacity worked out in a query and one worked out in a program agree.
//
// A tire is a cylinder: its section width (across the tread) and its overall diameter, in millimetres, from its size:
//   metric        [P|LT|ST|T]WWW/AA[ZR|R|D|B|-]RR[.5]   225/45R17, LT265/70R17, P225/45ZR17, 225/45-17, 225/45 R 17
//                 diameter = RR inches + 2 sidewalls of WWW * AA / 100
//   no aspect     WWW[R|-]RR                            195R15, 11R22.5: the old 82% sidewall
//   flotation     DD[X|x]WW.WW[R|-]RR                   33X12.50R15: diameter DD inches, width WW.WW inches
// A trailing load index and speed rating (91V), and an LT after the rim, are read past. Any other form is not a size.
//
// How many fit a bin of width W (across its opening), depth D and height H, the best of:
//   flat     lying down in stacks of floor(H / width), the stacks' circles packed on the bin's floor (rows straight or
//            offset by half a tire, whichever holds more, either way round)
//   across   standing on the tread, faces to the opening's sides: floor(W / width) a row, floor(D / diameter) rows,
//            floor(H / diameter) high
//   deep     standing on the tread, faces to the opening: floor(D / width) a row, floor(W / diameter) rows, floor(H /
//            diameter) high
// No clearance is assumed: a bin's usable size is what is given.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace aios::userland::tire {

struct Size { double width_mm = 0, diameter_mm = 0; bool ok = false; };

inline Size parse_size(const std::string& in) {
  std::string s;
  for (char c : in) if (!std::isspace((unsigned char)c)) s += (char)std::toupper((unsigned char)c);
  size_t i = 0;
  auto num = [&](double* out) -> bool {
    const size_t st = i;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
    if (i == st) return false;
    *out = std::atof(s.substr(st, i - st).c_str());
    return *out > 0;
  };
  // the service prefix
  for (const char* p : {"LT", "ST", "P", "T"}) {
    const size_t n = std::char_traits<char>::length(p);
    if (s.compare(0, n, p) == 0 && n < s.size() && std::isdigit((unsigned char)s[n])) { i = n; break; }
  }
  // a rim: two digits at most and an optional fraction (22.5) - what follows is the load index and speed rating
  auto rim_num = [&](double* out) -> bool {
    const size_t st = i;
    while (i < s.size() && i - st < 2 && std::isdigit((unsigned char)s[i])) ++i;
    if (i == st) return false;
    if (i + 1 < s.size() && s[i] == '.' && std::isdigit((unsigned char)s[i + 1])) { i += 2; }
    *out = std::atof(s.substr(st, i - st).c_str());
    return *out > 0;
  };
  double a = 0;
  if (!num(&a)) return {};
  Size out;
  // the construction letters between the section and the rim
  auto construction = [&]() -> bool {
    bool any = false;
    while (i < s.size() && (s[i] == 'Z' || s[i] == 'R' || s[i] == 'D' || s[i] == 'B' || s[i] == '-')) { ++i; any = true; }
    return any;
  };
  if (i < s.size() && s[i] == 'X') {   // flotation: diameter x width
    ++i;
    double wd = 0, rim = 0;
    if (!num(&wd) || !construction() || !rim_num(&rim)) return {};
    if (a < 15 || a > 60 || wd <= 0 || wd > 30 || rim < 8 || rim > 30 || rim >= a) return {};
    out.diameter_mm = a * 25.4;
    out.width_mm = wd * 25.4;
    out.ok = true;
    return out;
  }
  double aspect = 82, rim = 0;
  if (i < s.size() && s[i] == '/') {
    ++i;
    if (!num(&aspect)) return {};
  }
  if (!construction() || !rim_num(&rim)) return {};
  if (a < 80 || a > 500 || aspect < 20 || aspect > 120 || rim < 8 || rim > 30) {
    // 11R22.5: a section in inches (commercial)
    if (a >= 6 && a < 25 && rim >= 15 && rim <= 30) a *= 25.4;
    else return {};
  }
  out.width_mm = a;
  out.diameter_mm = rim * 25.4 + 2.0 * a * aspect / 100.0;
  out.ok = true;
  return out;
}

// circles of diameter d on a w x l floor: straight rows, or rows offset by half a circle and closer by sqrt(3)/2
inline long circles_in_rect(double w, double l, double d) {
  if (d <= 0 || w < d || l < d) return 0;
  const long straight = (long)std::floor(w / d) * (long)std::floor(l / d);
  long best = straight;
  for (int turn = 0; turn < 2; ++turn) {
    const double across = turn ? l : w, along = turn ? w : l;
    const long rows = 1 + (long)std::floor((along - d) / (d * std::sqrt(3.0) / 2.0) + 1e-9);
    const long full = (long)std::floor(across / d + 1e-9), offset = (long)std::floor((across - d / 2.0) / d + 1e-9);
    long n = 0;
    for (long r = 0; r < rows; ++r) n += (r % 2 == 0) ? full : offset;
    best = std::max(best, n);
  }
  return best;
}

struct Fit { long count = 0; const char* mode = "none"; };

inline Fit fit(double bin_w, double bin_d, double bin_h, double width, double diameter) {
  Fit f;
  if (!(bin_w > 0 && bin_d > 0 && bin_h > 0 && width > 0 && diameter > 0)) return f;
  const long stack = (long)std::floor(bin_h / width + 1e-9);
  const long flat = stack * circles_in_rect(bin_w, bin_d, diameter);
  const long high = (long)std::floor(bin_h / diameter + 1e-9);
  const long across = (long)std::floor(bin_w / width + 1e-9) * (long)std::floor(bin_d / diameter + 1e-9) * high;
  const long deep = (long)std::floor(bin_d / width + 1e-9) * (long)std::floor(bin_w / diameter + 1e-9) * high;
  f.count = flat; f.mode = "flat";
  if (across > f.count) { f.count = across; f.mode = "across"; }
  if (deep > f.count) { f.count = deep; f.mode = "deep"; }
  if (f.count == 0) f.mode = "none";
  return f;
}

}  // namespace aios::userland::tire
