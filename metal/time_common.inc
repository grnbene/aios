/* Time for the bare-metal runtimes, included inside their extern "C" block
 * after struct tm and struct timespec. Each board supplies
 *   bool metal_wall_unix_ns(int64_t* out)   the wall clock, false until it is known
 *   uint64_t metal_now_ns()                 monotonic nanoseconds since boot
 * and this turns them into time(), the calendar, a time zone, strftime,
 * mktime and clock_gettime. Until the wall clock is known, time() counts up
 * from a fixed build epoch so ordering still holds, and metal_wall_clock_set()
 * says it is not real. */
bool metal_wall_unix_ns(int64_t* out);
uint64_t metal_now_ns();
namespace {
constexpr int64_t kBuildEpoch = 1788393600;      /* 2026-09-02, only until the wall clock is known */
int64_t wall_unix_s() {
  int64_t ns = 0;
  if (metal_wall_unix_ns(&ns)) return ns / 1000000000ll;
  return kBuildEpoch + static_cast<int64_t>(metal_now_ns() / 1000000000ull);
}
/* Days since 1970-01-01 <-> civil date (Howard Hinnant's algorithms). */
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
  m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
}
void fill_tm(int64_t t, struct tm* out) {
  int64_t days = t / 86400, rem = t % 86400;
  if (rem < 0) { rem += 86400; --days; }
  int64_t y; unsigned m, d;
  civil_from_days(days, y, m, d);
  out->tm_year = static_cast<int>(y - 1900); out->tm_mon = static_cast<int>(m - 1); out->tm_mday = static_cast<int>(d);
  out->tm_hour = static_cast<int>(rem / 3600); out->tm_min = static_cast<int>((rem % 3600) / 60); out->tm_sec = static_cast<int>(rem % 60);
  out->tm_wday = static_cast<int>((days % 7 + 11) % 7);          /* 1970-01-01 was a Thursday */
  out->tm_yday = static_cast<int>(days - days_from_civil(y, 1, 1));
  out->tm_isdst = 0;
}

/* Timezone: a fixed offset, or a US zone with its daylight-saving rule
 * (second Sunday of March 02:00 local -> first Sunday of November 02:00). */
struct Zone { const char* name; int std_offset_min; bool us_dst; const char* std_abbr; const char* dst_abbr; };
const Zone kZones[] = {
  {"UTC", 0, false, "UTC", "UTC"},
  {"US/Eastern", -300, true, "EST", "EDT"}, {"US/Central", -360, true, "CST", "CDT"},
  {"US/Mountain", -420, true, "MST", "MDT"}, {"US/Pacific", -480, true, "PST", "PDT"},
  {"US/Arizona", -420, false, "MST", "MST"},
};
const Zone* g_zone = &kZones[0];
int64_t nth_sunday_utc(int64_t year, unsigned month, int nth, int std_off_min, int hour_local) {
  const int64_t first = days_from_civil(year, month, 1);
  const int wday = static_cast<int>((first % 7 + 11) % 7);
  const int64_t day = first + ((7 - wday) % 7) + 7 * (nth - 1);
  return day * 86400 + hour_local * 3600 - static_cast<int64_t>(std_off_min) * 60;
}
bool in_us_dst(int64_t utc, int std_off_min) {
  struct tm t; fill_tm(utc + std_off_min * 60, &t);
  const int64_t y = t.tm_year + 1900;
  const int64_t start = nth_sunday_utc(y, 3, 2, std_off_min, 2);
  const int64_t end = nth_sunday_utc(y, 11, 1, std_off_min + 60, 2);   /* 02:00 daylight time */
  return utc >= start && utc < end;
}
}  // namespace

bool metal_wall_clock_set() { return metal_wall_unix_ns(nullptr); }
bool metal_set_timezone(const char* name) {
  for (const Zone& z : kZones) {
    const char* a = z.name; const char* b = name; while (*a && *a == *b) { ++a; ++b; }
    if (!*a && (!*b || *b == '\n' || *b == '\r' || *b == ' ')) { g_zone = &z; return true; }
  }
  return false;
}
const char* metal_timezone_name() { return g_zone->name; }

long time(long* t) { const long v = static_cast<long>(wall_unix_s()); if (t) *t = v; return v; }
struct tm* gmtime_r(const long* t, struct tm* out) { if (!t || !out) return nullptr; fill_tm(*t, out); return out; }
struct tm* gmtime(const long* t) { static struct tm r; return gmtime_r(t, &r); }
struct tm* localtime_r(const long* t, struct tm* out) {
  if (!t || !out) return nullptr;
  int off = g_zone->std_offset_min;
  const bool dst = g_zone->us_dst && in_us_dst(*t, off);
  if (dst) off += 60;
  fill_tm(*t + off * 60, out);
  out->tm_isdst = dst ? 1 : 0;
  return out;
}
struct tm* localtime(const long* t) { static struct tm r; return localtime_r(t, &r); }
long mktime(struct tm* tm) {
  if (!tm) return -1;
  const int64_t days = days_from_civil(tm->tm_year + 1900, static_cast<unsigned>(tm->tm_mon + 1), static_cast<unsigned>(tm->tm_mday));
  int64_t local = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
  int off = g_zone->std_offset_min;
  if (g_zone->us_dst && in_us_dst(local - off * 60, off)) off += 60;
  return static_cast<long>(local - off * 60);
}
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* t) {
  static const char* kDay[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (!s || !max || !fmt || !t) return 0;
  size_t n = 0;
  auto put = [&](const char* p) { while (*p) { if (n + 1 >= max) return false; s[n++] = *p++; } return true; };
  auto num = [&](int v, int width, char pad) {
    char b[12]; int k = 0; unsigned u = static_cast<unsigned>(v < 0 ? -v : v);
    do { b[k++] = static_cast<char>('0' + u % 10); u /= 10; } while (u);
    while (k < width) b[k++] = pad;
    if (v < 0) b[k++] = '-';
    char o[12]; for (int i = 0; i < k; ++i) o[i] = b[k - 1 - i]; o[k] = 0;
    return put(o);
  };
  for (const char* f = fmt; *f; ++f) {
    if (*f != '%') { char c[2] = {*f, 0}; if (!put(c)) return 0; continue; }
    ++f;
    bool ok = true;
    switch (*f) {
      case 'Y': ok = num(t->tm_year + 1900, 4, '0'); break;
      case 'y': ok = num((t->tm_year + 1900) % 100, 2, '0'); break;
      case 'm': ok = num(t->tm_mon + 1, 2, '0'); break;
      case 'd': ok = num(t->tm_mday, 2, '0'); break;
      case 'e': ok = num(t->tm_mday, 2, ' '); break;
      case 'H': ok = num(t->tm_hour, 2, '0'); break;
      case 'I': ok = num(t->tm_hour % 12 ? t->tm_hour % 12 : 12, 2, '0'); break;
      case 'M': ok = num(t->tm_min, 2, '0'); break;
      case 'S': ok = num(t->tm_sec, 2, '0'); break;
      case 'p': ok = put(t->tm_hour < 12 ? "AM" : "PM"); break;
      case 'a': ok = put(kDay[(t->tm_wday % 7 + 7) % 7]); break;
      case 'b': ok = put(kMon[(t->tm_mon % 12 + 12) % 12]); break;
      case 'j': ok = num(t->tm_yday + 1, 3, '0'); break;
      case 'Z': ok = put(t->tm_isdst ? g_zone->dst_abbr : g_zone->std_abbr); break;
      case 'F': ok = num(t->tm_year + 1900, 4, '0') && put("-") && num(t->tm_mon + 1, 2, '0') && put("-") && num(t->tm_mday, 2, '0'); break;
      case 'T': ok = num(t->tm_hour, 2, '0') && put(":") && num(t->tm_min, 2, '0') && put(":") && num(t->tm_sec, 2, '0'); break;
      case '%': ok = put("%"); break;
      case 0: s[n] = 0; return n;
      default: { char c[3] = {'%', *f, 0}; ok = put(c); }
    }
    if (!ok) return 0;
  }
  s[n] = 0;
  return n;
}
int clock_gettime(int clk, struct timespec* ts) {
  if (!ts) return -1;
  if (clk == 0) {                                  /* CLOCK_REALTIME */
    int64_t ns = 0;
    if (!metal_wall_unix_ns(&ns)) ns = kBuildEpoch * 1000000000ll + static_cast<int64_t>(metal_now_ns());
    ts->tv_sec = static_cast<long>(ns / 1000000000ll);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ll);
    return 0;
  }
  const uint64_t ns = metal_now_ns();              /* MONOTONIC and friends */
  ts->tv_sec = static_cast<long>(ns / 1000000000ull);
  ts->tv_nsec = static_cast<long>(ns % 1000000000ull);
  return 0;
}
