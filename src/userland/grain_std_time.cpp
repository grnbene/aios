// grain's dates and times. A moment is a number: seconds since 1970-01-01
// 00:00 UTC (a real, so milliseconds survive). Calendar fields are computed
// here, not by the C library's strftime, whose output differs between macOS,
// Linux and Windows (%e, %z, locale names): a program prints the same date on
// all three.
//
//   now()                                   -> seconds since the epoch, to the millisecond
//   date_parts(t [, zone])                  -> {year, month, day, hour, minute, second,
//                                               millisecond, weekday, yearday, offset}
//   date_format(t, format [, zone])         -> text; %Y %m %d %H %M %S %L %y %j %a %A
//                                               %b %B %p %P %I %e %F %T %z %s %%;
//                                               %-d %-m %-H %-I %-M %-S %-j unpadded
//   date_parse(text [, zone])               -> seconds; ISO 8601 date or date-time
//   date_make(y, mo, d [, h, mi, s] [, zone]) -> seconds
//   date_range(first_day, last_day)         -> ["YYYY-MM-DD", ...], both ends included
//
// zone is "utc" or "local" (the machine's own zone); date_parts, date_format
// and date_make default to local, date_parse to UTC unless the text carries
// its own offset (Z, +05:30). month is 1-12, weekday 0 (Sunday) to 6,
// yearday 1-366, offset the zone's distance from UTC in seconds.
#include <chrono>
#include <cmath>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

// Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar, and back
// (H. Hinnant's algorithms): exact for any year, no table, no time zone.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// The machine's zone offset from UTC at moment t, in seconds (DST included).
int64_t local_offset(int64_t t) {
#if defined(AIOS_METAL)
  (void)t;
  return 0;   // bare metal keeps no zone database: its local time is UTC
#else
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm lt{};
#if defined(_WIN32)
  if (localtime_s(&lt, &tt) != 0) return 0;
#else
  if (!localtime_r(&tt, &lt)) return 0;
#endif
  // The local wall clock read as if it were UTC, minus the moment itself.
  const int64_t wall = days_from_civil(lt.tm_year + 1900, static_cast<unsigned>(lt.tm_mon + 1), static_cast<unsigned>(lt.tm_mday)) * 86400 +
                       lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  return wall - t;
#endif
}

struct Parts {
  int64_t year = 1970;
  unsigned month = 1, day = 1;
  int hour = 0, minute = 0, second = 0, millisecond = 0, weekday = 4, yearday = 1;
  int64_t offset = 0;
};

Parts parts_of(double t, bool local) {
  const double whole = std::floor(t);
  int64_t secs = static_cast<int64_t>(whole);
  Parts p;
  p.millisecond = static_cast<int>(std::lround((t - whole) * 1000.0));
  if (p.millisecond >= 1000) { p.millisecond -= 1000; ++secs; }
  p.offset = local ? local_offset(secs) : 0;
  const int64_t wall = secs + p.offset;
  int64_t days = wall / 86400, rem = wall % 86400;
  if (rem < 0) { rem += 86400; --days; }
  civil_from_days(days, p.year, p.month, p.day);
  p.hour = static_cast<int>(rem / 3600);
  p.minute = static_cast<int>(rem % 3600 / 60);
  p.second = static_cast<int>(rem % 60);
  p.weekday = static_cast<int>(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
  p.yearday = static_cast<int>(days - days_from_civil(p.year, 1, 1) + 1);
  return p;
}

// Wall-clock fields in a zone to a moment. For local time the zone's offset at
// that wall time is found by asking twice (the offset at the UTC reading, then
// at the corrected one), which settles across a DST change.
double moment_of(int64_t y, unsigned mo, unsigned d, int h, int mi, double s, bool local) {
  const double wall = static_cast<double>(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60) + s;
  if (!local) return wall;
  const int64_t guess = static_cast<int64_t>(std::floor(wall));
  int64_t off = local_offset(guess);
  off = local_offset(guess - off);
  return wall - static_cast<double>(off);
}

bool zone_arg(std::vector<Value>& args, size_t at, bool dflt_local, const char* who, std::string& error, bool& local) {
  local = dflt_local;
  if (args.size() <= at) return true;
  if (!want_str(args[at], error, who)) return false;
  const std::string& z = args[at].str();
  if (z == "utc" || z == "UTC") local = false;
  else if (z == "local") local = true;
  else { error = std::string(who) + "(): the zone is \"utc\" or \"local\", not \"" + z + "\""; return false; }
  return true;
}

const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};

std::string pad(int64_t v, int width, char fill = '0') {
  std::string s = std::to_string(v < 0 ? -v : v);
  while (static_cast<int>(s.size()) < width) s.insert(s.begin(), fill);
  return v < 0 ? "-" + s : s;
}

Value bi_now(Vm&, std::vector<Value>&, std::string&) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return Value::real(static_cast<double>(ms) / 1000.0);
}

Value bi_date_parts(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "date_parts")) return Value::nil();
  bool local = true;
  if (!zone_arg(args, 1, true, "date_parts", error, local)) return Value::nil();
  const Parts p = parts_of(args[0].as_double(), local);
  auto m = std::make_shared<MapObj>();
  m->put("year", Value::integer(p.year));
  m->put("month", Value::integer(p.month));
  m->put("day", Value::integer(p.day));
  m->put("hour", Value::integer(p.hour));
  m->put("minute", Value::integer(p.minute));
  m->put("second", Value::integer(p.second));
  m->put("millisecond", Value::integer(p.millisecond));
  m->put("weekday", Value::integer(p.weekday));
  m->put("yearday", Value::integer(p.yearday));
  m->put("offset", Value::integer(p.offset));
  return Value::object(Type::Map, m);
}

Value bi_date_format(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "date_format") || !want_str(args[1], error, "date_format")) return Value::nil();
  bool local = true;
  if (!zone_arg(args, 2, true, "date_format", error, local)) return Value::nil();
  const double t = args[0].as_double();
  const Parts p = parts_of(t, local);
  const std::string& f = args[1].str();
  std::string out;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] != '%' || i + 1 >= f.size()) { out.push_back(f[i]); continue; }
    char c = f[++i];
    // %-d, %-m, %-H, %-I, %-M, %-S, %-j: the number without its leading zeros ("Oct 7, 9:05am")
    if (c == '-') {
      const char n = i + 1 < f.size() ? f[i + 1] : '\0';
      int64_t v = -1;
      switch (n) {
        case 'd': case 'e': v = p.day; break;
        case 'm': v = p.month; break;
        case 'H': v = p.hour; break;
        case 'I': v = p.hour % 12 == 0 ? 12 : p.hour % 12; break;
        case 'M': v = p.minute; break;
        case 'S': v = p.second; break;
        case 'j': v = p.yearday; break;
        default: break;
      }
      if (v < 0) {
        error = std::string("date_format(): %-") + (n ? std::string(1, n) : std::string()) + " has no unpadded form (d e m H I M S j)";
        return Value::nil();
      }
      out += std::to_string(v);
      ++i;
      continue;
    }
    switch (c) {
      case 'P': out += p.hour < 12 ? "am" : "pm"; break;
      case 'Y': out += pad(p.year, 4); break;
      case 'y': out += pad(((p.year % 100) + 100) % 100, 2); break;
      case 'm': out += pad(p.month, 2); break;
      case 'd': out += pad(p.day, 2); break;
      case 'e': out += pad(p.day, 2, ' '); break;
      case 'H': out += pad(p.hour, 2); break;
      case 'I': out += pad(p.hour % 12 == 0 ? 12 : p.hour % 12, 2); break;
      case 'M': out += pad(p.minute, 2); break;
      case 'S': out += pad(p.second, 2); break;
      case 'L': out += pad(p.millisecond, 3); break;
      case 'j': out += pad(p.yearday, 3); break;
      case 'p': out += p.hour < 12 ? "AM" : "PM"; break;
      case 'a': out += std::string(kDays[p.weekday]).substr(0, 3); break;
      case 'A': out += kDays[p.weekday]; break;
      case 'b': out += std::string(kMonths[p.month - 1]).substr(0, 3); break;
      case 'B': out += kMonths[p.month - 1]; break;
      case 'F': out += pad(p.year, 4) + "-" + pad(p.month, 2) + "-" + pad(p.day, 2); break;
      case 'T': out += pad(p.hour, 2) + ":" + pad(p.minute, 2) + ":" + pad(p.second, 2); break;
      case 's': out += std::to_string(static_cast<int64_t>(std::floor(t))); break;
      case 'z': {
        const int64_t o = p.offset < 0 ? -p.offset : p.offset;
        out += (p.offset < 0 ? "-" : "+") + pad(o / 3600, 2) + pad(o % 3600 / 60, 2);
        break;
      }
      case '%': out.push_back('%'); break;
      default:
        error = std::string("date_format(): unknown directive %") + c;
        return Value::nil();
    }
  }
  return Value::string(std::move(out));
}

// ISO 8601: YYYY-MM-DD, then optionally T or a space, HH:MM, :SS, .fraction,
// and Z or +HH:MM / -HH:MM / +HHMM. Anything else, or a field out of range,
// is an error - a date that is guessed at is worse than none.
Value bi_date_parse(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "date_parse")) return Value::nil();
  bool local = false;
  if (!zone_arg(args, 1, false, "date_parse", error, local)) return Value::nil();
  const std::string& s = args[0].str();
  size_t i = 0;
  auto bad = [&]() { error = "date_parse(): \"" + s + "\" is not an ISO 8601 date (YYYY-MM-DD[THH:MM[:SS[.fff]]][Z|+HH:MM])"; return Value::nil(); };
  auto digits = [&](int n, int64_t& v) {
    if (i + static_cast<size_t>(n) > s.size()) return false;
    v = 0;
    for (int k = 0; k < n; ++k) {
      const char c = s[i + static_cast<size_t>(k)];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    i += static_cast<size_t>(n);
    return true;
  };
  int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  double frac = 0;
  if (!digits(4, y) || i >= s.size() || s[i++] != '-' || !digits(2, mo) || i >= s.size() || s[i++] != '-' || !digits(2, d)) return bad();
  if (mo < 1 || mo > 12) return bad();
  static const int kLen[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (d < 1 || d > kLen[mo - 1] + (mo == 2 && leap(y) ? 1 : 0)) return bad();
  bool has_offset = false;
  int64_t offset = 0;
  if (i < s.size()) {
    if (s[i] != 'T' && s[i] != 't' && s[i] != ' ') return bad();
    ++i;
    if (!digits(2, h) || i >= s.size() || s[i++] != ':' || !digits(2, mi)) return bad();
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!digits(2, sec)) return bad();
      if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        ++i;
        double scale = 0.1;
        const size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { frac += (s[i] - '0') * scale; scale /= 10; ++i; }
        if (i == start) return bad();
      }
    }
    if (h > 23 || mi > 59 || sec > 60) return bad();   // 60: a leap second, as ISO allows
    if (i < s.size()) {
      if (s[i] == 'Z' || s[i] == 'z') {
        ++i;
        has_offset = true;
      } else if (s[i] == '+' || s[i] == '-') {
        const int sign = s[i++] == '-' ? -1 : 1;
        int64_t oh = 0, om = 0;
        if (!digits(2, oh)) return bad();
        if (i < s.size() && s[i] == ':') ++i;
        if (!digits(2, om) || oh > 23 || om > 59) return bad();
        offset = sign * (oh * 3600 + om * 60);
        has_offset = true;
      }
    }
    if (i != s.size()) return bad();
  }
  const double secs = static_cast<double>(sec) + frac;
  if (has_offset) return Value::real(moment_of(y, static_cast<unsigned>(mo), static_cast<unsigned>(d), static_cast<int>(h), static_cast<int>(mi), secs, false) - static_cast<double>(offset));
  return Value::real(moment_of(y, static_cast<unsigned>(mo), static_cast<unsigned>(d), static_cast<int>(h), static_cast<int>(mi), secs, local));
}

// date_make(year, month, day [, hour, minute, second] [, zone])
Value bi_date_make(Vm&, std::vector<Value>& args, std::string& error) {
  size_t nums = args.size();
  bool local = true;
  if (nums > 3 && args[nums - 1].type == Type::Str) {
    if (!zone_arg(args, nums - 1, true, "date_make", error, local)) return Value::nil();
    --nums;
  }
  if (nums != 3 && nums != 6) { error = "date_make(): wants year, month, day [, hour, minute, second] [, zone]"; return Value::nil(); }
  for (size_t k = 0; k < nums; ++k)
    if (!want_number(args[k], error, "date_make")) return Value::nil();
  const int64_t y = args[0].as_int(), mo = args[1].as_int(), d = args[2].as_int();
  const int64_t h = nums == 6 ? args[3].as_int() : 0, mi = nums == 6 ? args[4].as_int() : 0;
  const double sec = nums == 6 ? args[5].as_double() : 0.0;
  static const int kLen[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (mo < 1 || mo > 12 || d < 1 || d > kLen[mo - 1] + (mo == 2 && leap(y) ? 1 : 0) || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec >= 61) {
    error = "date_make(): " + std::to_string(y) + "-" + std::to_string(mo) + "-" + std::to_string(d) + " " + std::to_string(h) + ":" +
            std::to_string(mi) + " is not a date and time";
    return Value::nil();
  }
  return Value::real(moment_of(y, static_cast<unsigned>(mo), static_cast<unsigned>(d), static_cast<int>(h), static_cast<int>(mi), sec, local));
}

// date_range("2026-10-01", "2026-10-31") -> every day from the first to the last, both included, as YYYY-MM-DD;
// [] when the first is after the last. Calendar days, so no zone and no daylight-saving hour to fall into.
Value bi_date_range(Vm&, std::vector<Value>& args, std::string& error) {
  int64_t day[2];
  for (int k = 0; k < 2; ++k) {
    if (!want_str(args[k], error, "date_range")) return Value::nil();
    const std::string& s = args[k].str();
    auto num = [&](size_t at, size_t n, int64_t& v) {
      v = 0;
      for (size_t j = at; j < at + n; ++j) {
        if (s[j] < '0' || s[j] > '9') return false;
        v = v * 10 + (s[j] - '0');
      }
      return true;
    };
    int64_t y = 0, mo = 0, d = 0;
    static const int kLen[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || mo < 1 || mo > 12 ||
        d < 1 || d > kLen[mo - 1] + (mo == 2 && leap(y) ? 1 : 0)) {
      error = "date_range(): \"" + s + "\" is not a day (YYYY-MM-DD)";
      return Value::nil();
    }
    day[k] = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  }
  constexpr int64_t kMaxDays = 100000;   // ~274 years: past that it is a mistake, not a range
  if (day[1] - day[0] + 1 > kMaxDays) { error = "date_range(): more than 100000 days"; return Value::nil(); }
  auto list = std::make_shared<ListObj>(std::vector<Value>{});
  for (int64_t z = day[0]; z <= day[1]; ++z) {
    int64_t y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(z, y, m, d);
    list->items.push_back(Value::string(pad(y, 4) + "-" + pad(m, 2) + "-" + pad(d, 2)));
  }
  return Value::object(Type::List, list);
}

}  // namespace

// ---- timers: data that changes on its own -------------------------------------------------

double wall_ms() {
  return (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
}

namespace {

bool callable(const Value& v) { return v.type == Type::Fn || v.type == Type::Native || v.type == Type::BoundMethod; }

Value add_timer(Vm& vm, std::vector<Value>& args, std::string& error, bool repeat, const char* who) {
  if (!args[0].is_number() || args[0].as_double() < 0) { error = std::string(who) + "() wants a number of seconds"; return Value::nil(); }
  if (!callable(args[1])) { error = std::string(who) + "() wants a function to call"; return Value::nil(); }
  if (vm.worker) { error = std::string(who) + "(): a worker (parallel_map, spawn) has no loop to fire timers; set them in the program itself"; return Value::nil(); }
  if (vm.timers.size() >= 1000) { error = std::string(who) + "(): too many timers"; return Value::nil(); }
  Vm::Timer t;
  t.id = vm.next_timer_id++;
  t.period_ms = repeat ? std::max(args[0].as_double() * 1000.0, 1.0) : 0.0;
  t.due_ms = wall_ms() + args[0].as_double() * 1000.0;
  t.fn = args[1];
  vm.timers.push_back(t);
  return Value::integer((int64_t)t.id);
}

Value bi_every(Vm& vm, std::vector<Value>& args, std::string& error) { return add_timer(vm, args, error, true, "every"); }
Value bi_after(Vm& vm, std::vector<Value>& args, std::string& error) { return add_timer(vm, args, error, false, "after"); }

Value bi_cancel(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Int) { error = "cancel() wants a timer from every() or after()"; return Value::nil(); }
  const auto before = vm.timers.size();
  vm.timers.erase(std::remove_if(vm.timers.begin(), vm.timers.end(), [&](const Vm::Timer& t) { return (int64_t)t.id == args[0].i; }), vm.timers.end());
  return Value::boolean(vm.timers.size() != before);
}

// wait(seconds): sleep, firing timers as they come due - for a program with no window or server loop to do it
Value bi_wait(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!args[0].is_number() || args[0].as_double() < 0) { error = "wait() wants a number of seconds"; return Value::nil(); }
  const double end = wall_ms() + args[0].as_double() * 1000.0;
  for (;;) {
    double next = -1;
    run_timers(vm, error, &next);
    if (!error.empty()) return Value::nil();
    const double now = wall_ms();
    if (now >= end) break;
    double nap = end - now;
    if (next >= 0) nap = std::min(nap, next);
    std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(std::max(nap, 0.5) * 1000.0)));
  }
  return Value::nil();
}

}  // namespace

bool run_timers(Vm& vm, std::string& error, double* until_next_ms) {
  bool ran = false;
  const double now = wall_ms();
  std::vector<Vm::Timer> due;
  for (auto& t : vm.timers)
    if (t.due_ms <= now) due.push_back(t);
  for (auto& t : due) {
    // reschedule (or drop) before calling, so a timer that cancels itself, or is slow, behaves
    for (auto it = vm.timers.begin(); it != vm.timers.end(); ++it) {
      if (it->id != t.id) continue;
      if (it->period_ms > 0) it->due_ms = std::max(it->due_ms + it->period_ms, now);
      else vm.timers.erase(it);
      break;
    }
    std::string err;
    vm.call(t.fn, {}, err);
    ran = true;
    if (!err.empty()) {
      if (error.empty()) error = "a timer's function failed: " + err;
      vm.timers.erase(std::remove_if(vm.timers.begin(), vm.timers.end(), [&](const Vm::Timer& x) { return x.id == t.id; }), vm.timers.end());   // a failing timer is not kept
    }
  }
  if (until_next_ms) {
    *until_next_ms = -1;
    const double after = wall_ms();
    for (const auto& t : vm.timers) *until_next_ms = *until_next_ms < 0 ? std::max(t.due_ms - after, 0.0) : std::min(*until_next_ms, std::max(t.due_ms - after, 0.0));
  }
  return ran;
}

void install_time_builtins(Vm& vm) {
  vm.define_native("every", 2, 2, bi_every);
  vm.define_native("after", 2, 2, bi_after);
  vm.define_native("cancel", 1, 1, bi_cancel);
  vm.define_native("wait", 1, 1, bi_wait);
  vm.define_native("now", 0, 0, bi_now);
  vm.define_native("date_parts", 1, 2, bi_date_parts);
  vm.define_native("date_format", 2, 3, bi_date_format);
  vm.define_native("date_parse", 1, 2, bi_date_parse);
  vm.define_native("date_make", 3, 7, bi_date_make);
  vm.define_native("date_range", 2, 3, bi_date_range);
}

}  // namespace aios::userland::grain
