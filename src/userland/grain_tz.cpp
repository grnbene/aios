// Named time zones: TZif files (RFC 8536) and the POSIX TZ rule in their footer, read in-tree.
#include "grain_tz.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>

#include "aios/hal/host.hpp"

namespace aios::userland::grain::tz {

namespace {

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t year_of(int64_t t) {
  int64_t days = t / 86400;
  if (t % 86400 < 0) --days;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  return static_cast<int64_t>(yoe) + era * 400 + (mp >= 10 ? 1 : 0);
}

bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// The local wall-clock second (counted as if it were UTC) a rule's change falls on in `year`.
int64_t change_wall(const Rule::When& w, int64_t year) {
  int64_t day = 0;
  if (w.kind == 'J') {
    day = days_from_civil(year, 1, 1) + w.day - 1 + (leap(year) && w.day >= 60 ? 1 : 0);
  } else if (w.kind == 'D') {
    day = days_from_civil(year, 1, 1) + w.day;
  } else {
    static const int kLen[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int64_t first = days_from_civil(year, (unsigned)w.month, 1);
    const int wd_first = (int)(((first % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    int64_t d = first + (w.weekday - wd_first + 7) % 7 + 7 * (w.week - 1);
    const int len = kLen[w.month - 1] + (w.month == 2 && leap(year) ? 1 : 0);
    while (d >= first + len) d -= 7;   // week 5: the last one in the month
    day = d;
  }
  return day * 86400 + w.time;
}

int64_t rule_offset(const Rule& r, int64_t t) {
  if (!r.has_dst) return r.std_offset;
  const int64_t year = year_of(t + r.std_offset);
  const int64_t start = change_wall(r.start, year) - r.std_offset;
  const int64_t end = change_wall(r.end, year) - r.dst_offset;
  const bool dst = start < end ? (t >= start && t < end) : !(t >= end && t < start);   // the south: DST spans new year
  return dst ? r.dst_offset : r.std_offset;
}

// [+-]hh[:mm[:ss]] as seconds; hours up to 167 (TZif v3 times).
bool parse_hms(const std::string& s, size_t& i, int64_t& out) {
  int sign = 1;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) sign = s[i++] == '-' ? -1 : 1;
  auto num = [&](int64_t& v, int max_digits) {
    const size_t start = i;
    v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && (int)(i - start) < max_digits) v = v * 10 + (s[i++] - '0');
    return i > start;
  };
  int64_t h = 0, m = 0, sec = 0;
  if (!num(h, 3) || h > 167) return false;
  if (i < s.size() && s[i] == ':') { ++i; if (!num(m, 2) || m > 59) return false; }
  if (i < s.size() && s[i] == ':') { ++i; if (!num(sec, 2) || sec > 59) return false; }
  out = sign * (h * 3600 + m * 60 + sec);
  return true;
}

bool parse_name(const std::string& s, size_t& i) {
  if (i < s.size() && s[i] == '<') {
    const size_t close = s.find('>', i);
    if (close == std::string::npos) return false;
    i = close + 1;
    return true;
  }
  const size_t start = i;
  while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
  return i - start >= 3;
}

bool parse_when(const std::string& s, size_t& i, Rule::When& w) {
  auto num = [&](int& v) {
    const size_t start = i;
    v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    return i > start;
  };
  if (i < s.size() && s[i] == 'M') {
    ++i;
    w.kind = 'M';
    if (!num(w.month) || i >= s.size() || s[i++] != '.' || !num(w.week) || i >= s.size() || s[i++] != '.' || !num(w.weekday)) return false;
    if (w.month < 1 || w.month > 12 || w.week < 1 || w.week > 5 || w.weekday > 6) return false;
  } else if (i < s.size() && s[i] == 'J') {
    ++i;
    w.kind = 'J';
    if (!num(w.day) || w.day < 1 || w.day > 365) return false;
  } else {
    w.kind = 'D';
    if (!num(w.day) || w.day > 365) return false;
  }
  w.time = 7200;
  if (i < s.size() && s[i] == '/') {
    ++i;
    if (!parse_hms(s, i, w.time)) return false;
  }
  return true;
}

std::string search_dir() {
  if (const char* d = std::getenv("TZDIR"); d && *d) return d;
  static const char* const kDirs[] = {"/usr/share/zoneinfo", "/usr/lib/zoneinfo", "/usr/share/lib/zoneinfo", "/etc/zoneinfo"};
  for (const char* d : kDirs)
    if (hal::process_host().exists(std::string(d) + "/UTC") || hal::process_host().exists(std::string(d) + "/Etc/UTC")) return d;
  return "";
}

bool valid_name(const std::string& n) {
  if (n.empty() || n.size() > 255 || n[0] == '/' || n[0] == '.') return false;
  for (char c : n)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '+' || c == '.'))
      return false;
  return n.find("..") == std::string::npos;
}

}  // namespace

int64_t Zone::offset_at(int64_t t) const {
  if (transitions.empty() || t >= transitions.back()) {
    if (has_rule) return rule_offset(rule, t);
    if (transitions.empty()) return offsets.empty() ? 0 : offsets[0];
  }
  if (t < transitions.front()) return offsets.empty() ? 0 : offsets[0];
  const size_t i = static_cast<size_t>(std::upper_bound(transitions.begin(), transitions.end(), t) - transitions.begin()) - 1;
  return offsets[type_of[i]];
}

bool parse_rule(const std::string& s, Rule& r) {
  size_t i = 0;
  r = Rule{};
  int64_t v = 0;
  if (!parse_name(s, i) || !parse_hms(s, i, v)) return false;
  r.std_offset = -v;   // POSIX counts west of Greenwich as positive
  if (i == s.size()) return true;
  if (!parse_name(s, i)) return false;
  r.has_dst = true;
  r.dst_offset = r.std_offset + 3600;
  if (i < s.size() && s[i] != ',') {
    if (!parse_hms(s, i, v)) return false;
    r.dst_offset = -v;
  }
  if (i == s.size()) {   // no rule written: the US one, as POSIX implementations assume
    r.start = Rule::When{'M', 3, 2, 0, 0, 7200};
    r.end = Rule::When{'M', 11, 1, 0, 0, 7200};
    return true;
  }
  if (s[i++] != ',' || !parse_when(s, i, r.start) || i >= s.size() || s[i++] != ',' || !parse_when(s, i, r.end)) return false;
  return i == s.size();
}

bool parse_tzif(const std::string& d, Zone& z, std::string& error) {
  auto be32 = [&](size_t at) { return (int64_t)(int32_t)(((uint32_t)(uint8_t)d[at] << 24) | ((uint32_t)(uint8_t)d[at + 1] << 16) | ((uint32_t)(uint8_t)d[at + 2] << 8) | (uint8_t)d[at + 3]); };
  auto be64 = [&](size_t at) { uint64_t v = 0; for (int k = 0; k < 8; ++k) v = (v << 8) | (uint8_t)d[at + k]; return (int64_t)v; };
  auto header = [&](size_t at, int64_t c[6]) {
    if (d.size() < at + 44 || d.compare(at, 4, "TZif") != 0) return false;
    for (int k = 0; k < 6; ++k) c[k] = be32(at + 20 + 4 * k);
    for (int k = 0; k < 6; ++k) if (c[k] < 0 || c[k] > 100000) return false;
    return true;
  };
  int64_t c[6];   // isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt
  if (!header(0, c)) { error = "not a TZif file"; return false; }
  const char version = d[4];
  size_t at = 44;
  int tsize = 4;
  if (version >= '2') {   // skip the 32-bit block: the 64-bit one after it says the same for every year
    at += c[3] * 4 + c[3] + c[4] * 6 + c[5] + c[2] * 8 + c[1] + c[0];
    if (!header(at, c)) { error = "a TZif file whose second header is missing"; return false; }
    at += 44;
    tsize = 8;
  }
  const int64_t timecnt = c[3], typecnt = c[4];
  const size_t need = at + timecnt * tsize + timecnt + typecnt * 6 + c[5] + c[2] * (tsize + 4) + c[1] + c[0];
  if (typecnt < 1 || d.size() < need) { error = "a TZif file cut short"; return false; }
  z = Zone{};
  for (int64_t k = 0; k < timecnt; ++k) z.transitions.push_back(tsize == 8 ? be64(at + k * 8) : be32(at + k * 4));
  at += timecnt * tsize;
  for (int64_t k = 0; k < timecnt; ++k) {
    const uint8_t ty = (uint8_t)d[at + k];
    if (ty >= typecnt) { error = "a TZif transition to a type that is not there"; return false; }
    z.type_of.push_back(ty);
  }
  at += timecnt;
  for (int64_t k = 0; k < typecnt; ++k) z.offsets.push_back(be32(at + k * 6));
  at = need;
  if (version >= '2' && at < d.size() && d[at] == '\n') {
    const size_t end = d.find('\n', at + 1);
    if (end != std::string::npos && end > at + 1) z.has_rule = parse_rule(d.substr(at + 1, end - at - 1), z.rule);
  }
  return true;
}

std::shared_ptr<const Zone> load(const std::string& name, std::string& error) {
#if defined(AIOS_METAL)
  (void)name;
  error = "this machine keeps no time zone database";
  return nullptr;
#else
  static std::mutex mu;
  static std::map<std::string, std::shared_ptr<const Zone>> cache;
  std::lock_guard<std::mutex> lk(mu);
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  if (!valid_name(name)) { error = "\"" + name + "\" is not a time zone's name"; return nullptr; }
  const std::string dir = search_dir();
  if (dir.empty()) { error = "this machine has no time zone database (/usr/share/zoneinfo)"; return nullptr; }
  auto data = hal::process_host().read_file(dir + "/" + name);
  if (!data.ok()) { error = "no time zone called \"" + name + "\""; return nullptr; }
  auto z = std::make_shared<Zone>();
  std::string why;
  if (!parse_tzif(std::string(reinterpret_cast<const char*>(data->data()), data->size()), *z, why)) {
    error = "time zone \"" + name + "\": " + why;
    return nullptr;
  }
  cache[name] = z;
  return z;
#endif
}

}  // namespace aios::userland::grain::tz
