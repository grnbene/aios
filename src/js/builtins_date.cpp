// Date.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "builtins.hpp"
#include "internal_num.hpp"

namespace aios::js {

namespace {

constexpr double kMsPerDay = 86400000.0;
constexpr double kMaxTime = 8.64e15;

struct DateObject final : Object {
  DateObject() : Object(Class::Date) {}
  double t = NAN;   // the time value: ms since the epoch (UTC), or NaN
};

// ---- calendar arithmetic (proleptic Gregorian) ----

double day_of(double t) { return std::floor(t / kMsPerDay); }
double time_within_day(double t) {
  double r = std::fmod(t, kMsPerDay);
  return r < 0 ? r + kMsPerDay : r;
}

// Days since the epoch of year/month(0-11)/day(1-31); year may be any integer.
double days_from_civil(double y, double m, double d) {
  y += std::floor(m / 12);
  m = std::fmod(m, 12);
  if (m < 0) m += 12;
  const double yy = m < 2 ? y - 1 : y;
  const double era = std::floor(yy / 400);
  const double yoe = yy - era * 400;
  const double mp = std::fmod(m + 10, 12);   // March = 0
  const double doy = std::floor((153 * mp + 2) / 5) + d - 1;
  const double doe = yoe * 365 + std::floor(yoe / 4) - std::floor(yoe / 100) + doy;
  return era * 146097 + doe - 719468;
}

struct Civil { double year; int month, day; };

Civil civil_from_days(double z) {
  z += 719468;
  const double era = std::floor(z / 146097);
  const double doe = z - era * 146097;
  const double yoe = std::floor((doe - std::floor(doe / 1460) + std::floor(doe / 36524) - std::floor(doe / 146096)) / 365);
  double y = yoe + era * 400;
  const double doy = doe - (365 * yoe + std::floor(yoe / 4) - std::floor(yoe / 100));
  const double mp = std::floor((5 * doy + 2) / 153);
  const int d = static_cast<int>(doy - std::floor((153 * mp + 2) / 5) + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) y += 1;
  return {y, m - 1, d};
}

int weekday_of(double t) {
  int w = static_cast<int>(std::fmod(day_of(t) + 4, 7));
  return w < 0 ? w + 7 : w;
}

double make_time(double h, double m, double s, double ms) {
  if (!std::isfinite(h) || !std::isfinite(m) || !std::isfinite(s) || !std::isfinite(ms)) return NAN;
  return std::trunc(h) * 3600000.0 + std::trunc(m) * 60000.0 + std::trunc(s) * 1000.0 + std::trunc(ms);
}

double make_day(double year, double month, double date) {
  if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) return NAN;
  year = std::trunc(year);
  month = std::trunc(month);
  date = std::trunc(date);
  if (std::fabs(year) > 400000 || std::fabs(month) > 4800000) return NAN;
  return days_from_civil(year, month, 1) + date - 1;
}

double make_date(double day, double time) {
  if (!std::isfinite(day) || !std::isfinite(time)) return NAN;
  double tv = day * kMsPerDay + time;
  return std::isfinite(tv) ? tv : NAN;
}

double time_clip(double t) {
  if (!std::isfinite(t) || std::fabs(t) > kMaxTime) return NAN;
  return std::trunc(t) + 0.0;
}

// ---- local time ----

double local_offset_ms(Context& ctx, double utc) {
  if (!ctx.tz_offset_minutes || std::isnan(utc)) return 0;
  return ctx.tz_offset_minutes(utc) * 60000.0;
}

double local_time(Context& ctx, double t) { return t + local_offset_ms(ctx, t); }

double utc_from_local(Context& ctx, double local) {
  if (std::isnan(local)) return NAN;
  double guess = local - local_offset_ms(ctx, local);
  return local - local_offset_ms(ctx, guess);
}

struct Fields {
  double year;
  int month, day, hour, minute, second, ms, weekday;
};

Fields split(double t) {
  Civil c = civil_from_days(day_of(t));
  const double tw = time_within_day(t);
  Fields f;
  f.year = c.year;
  f.month = c.month;
  f.day = c.day;
  f.hour = static_cast<int>(tw / 3600000.0);
  f.minute = static_cast<int>(std::fmod(tw / 60000.0, 60));
  f.second = static_cast<int>(std::fmod(tw / 1000.0, 60));
  f.ms = static_cast<int>(std::fmod(tw, 1000));
  f.weekday = weekday_of(t);
  return f;
}

const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kLongDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kLongMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

std::string pad(long v, int width) {
  std::string s = std::to_string(v < 0 ? -v : v);
  while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
  return s;
}

// ---- parsing ----

bool parse_iso(const std::string& s, double* out, bool* date_only, bool* has_offset, double* offset_ms, bool* invalid) {
  *invalid = false;
  size_t i = 0;
  auto digits = [&](int n, int* v) {
    if (i + static_cast<size_t>(n) > s.size()) return false;
    int r = 0;
    for (int k = 0; k < n; ++k) {
      char c = s[i + static_cast<size_t>(k)];
      if (c < '0' || c > '9') return false;
      r = r * 10 + (c - '0');
    }
    i += static_cast<size_t>(n);
    *v = r;
    return true;
  };
  int y, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0;
  double year;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    const bool neg = s[i] == '-';
    ++i;
    if (!digits(6, &y)) return false;
    if (neg && y == 0) return false;
    year = neg ? -y : y;
  } else {
    if (!digits(4, &y)) return false;
    year = y;
  }
  *date_only = true;
  *has_offset = false;
  *offset_ms = 0;
  if (i < s.size() && s[i] == '-') {
    ++i;
    if (!digits(2, &mo)) return false;
    if (i < s.size() && s[i] == '-') {
      ++i;
      if (!digits(2, &d)) return false;
    }
  }
  if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
    ++i;
    *date_only = false;
    if (!digits(2, &h)) return false;
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    if (!digits(2, &mi)) return false;
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!digits(2, &sec)) return false;
      if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        ++i;
        int n = 0;
        double frac = 0, scale = 100;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
          frac += (s[i] - '0') * scale;
          scale /= 10;
          ++i;
          ++n;
        }
        if (!n) return false;
        ms = static_cast<int>(frac);
      }
    }
    if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
      ++i;
      *has_offset = true;
    } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      const int sign = s[i] == '-' ? -1 : 1;
      ++i;
      int oh, om = 0;
      if (!digits(2, &oh)) return false;
      if (i < s.size() && s[i] == ':') ++i;
      if (i < s.size() && !digits(2, &om)) return false;
      *has_offset = true;
      *offset_ms = sign * (oh * 60 + om) * 60000.0;
    }
  }
  if (i != s.size()) return false;
  if (mo < 1 || mo > 12 || d < 1 || h > 24 || mi > 59 || sec > 59) { *invalid = true; return false; }
  {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year == std::floor(year / 4) * 4 && year != std::floor(year / 100) * 100) || year == std::floor(year / 400) * 400;
    if (d > dim[mo - 1] + (mo == 2 && leap ? 1 : 0)) { *invalid = true; return false; }
  }
  if (h == 24 && (mi || sec || ms)) { *invalid = true; return false; }
  *out = make_date(make_day(year, mo - 1, d), make_time(h, mi, sec, ms));
  return true;
}

// The forgiving parser for "Jan 15, 2020 10:00 PM", "15 January 2020", "Wed, 15 Jan 2020 10:00:00 GMT", "1/15/2020"...
bool parse_legacy(const std::string& in, double* out, bool* has_offset, double* offset_ms) {
  *has_offset = false;
  *offset_ms = 0;
  double year = NAN;
  int month = -1, day = -1, hour = 0, minute = 0, second = 0, ms = 0;
  bool pm = false, am = false, have_time = false;
  std::vector<int> nums;
  size_t i = 0;
  const size_t n = in.size();
  while (i < n) {
    char c = in[i];
    if (c == '(') {   // a comment
      int depth = 1;
      ++i;
      while (i < n && depth) { if (in[i] == '(') ++depth; else if (in[i] == ')') --depth; ++i; }
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c))) {
      size_t j = i;
      std::string w;
      while (j < n && (std::isalpha(static_cast<unsigned char>(in[j])) || in[j] == '.')) { if (in[j] != '.') w.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(in[j])))); ++j; }
      i = j;
      if (w == "am") { am = true; continue; }
      if (w == "pm") { pm = true; continue; }
      if (w == "z" || w == "gmt" || w == "utc" || w == "ut") {
        *has_offset = true;
        // an offset may follow
        if (i < n && (in[i] == '+' || in[i] == '-')) {
          const int sign = in[i] == '-' ? -1 : 1;
          size_t k = i + 1;
          int hh = 0, mm = 0, d = 0;
          while (k < n && std::isdigit(static_cast<unsigned char>(in[k])) && d < 2) { hh = hh * 10 + (in[k] - '0'); ++k; ++d; }
          if (k < n && in[k] == ':') ++k;
          d = 0;
          while (k < n && std::isdigit(static_cast<unsigned char>(in[k])) && d < 2) { mm = mm * 10 + (in[k] - '0'); ++k; ++d; }
          *offset_ms = sign * (hh * 60 + mm) * 60000.0;
          i = k;
        }
        continue;
      }
      if (w == "t" && have_time == false) continue;
      bool matched = false;
      if (w.size() >= 3) {
        for (int m = 0; m < 12; ++m) {
          std::string lm = kLongMonths[m];
          for (char& ch : lm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          if (lm.compare(0, w.size() < lm.size() ? w.size() : lm.size(), w, 0, std::string::npos) == 0 && w.size() <= lm.size() && (w.size() == 3 || w == lm)) { month = m; matched = true; break; }
        }
        if (!matched) {
          for (int d = 0; d < 7; ++d) {
            std::string ld = kLongDays[d];
            for (char& ch : ld) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (w.size() <= ld.size() && ld.compare(0, w.size(), w) == 0) { matched = true; break; }
          }
        }
      }
      if (!matched) return false;
      continue;
    }
    if (c == '+' || c == '-') {
      // a UTC offset after a time, or a date separator
      if (have_time && i + 1 < n && std::isdigit(static_cast<unsigned char>(in[i + 1])) && (c == '+' || nums.size() >= 2 || month >= 0)) {
        const int sign = c == '-' ? -1 : 1;
        size_t k = i + 1;
        int hh = 0, mm = 0, d = 0;
        while (k < n && std::isdigit(static_cast<unsigned char>(in[k])) && d < 2) { hh = hh * 10 + (in[k] - '0'); ++k; ++d; }
        if (k < n && in[k] == ':') ++k;
        d = 0;
        while (k < n && std::isdigit(static_cast<unsigned char>(in[k])) && d < 2) { mm = mm * 10 + (in[k] - '0'); ++k; ++d; }
        *has_offset = true;
        *offset_ms = sign * (hh * 60 + mm) * 60000.0;
        i = k;
        continue;
      }
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      long v = 0;
      while (j < n && std::isdigit(static_cast<unsigned char>(in[j]))) { v = v * 10 + (in[j] - '0'); ++j; }
      const size_t digits = j - i;
      if (j < n && in[j] == ':') {
        // a time: h:m[:s[.ms]]
        hour = static_cast<int>(v);
        size_t k = j + 1;
        long mv = 0;
        while (k < n && std::isdigit(static_cast<unsigned char>(in[k]))) { mv = mv * 10 + (in[k] - '0'); ++k; }
        minute = static_cast<int>(mv);
        if (k < n && in[k] == ':') {
          ++k;
          long sv = 0;
          while (k < n && std::isdigit(static_cast<unsigned char>(in[k]))) { sv = sv * 10 + (in[k] - '0'); ++k; }
          second = static_cast<int>(sv);
          if (k < n && (in[k] == '.' || in[k] == ',')) {
            ++k;
            double scale = 100, frac = 0;
            while (k < n && std::isdigit(static_cast<unsigned char>(in[k]))) { frac += (in[k] - '0') * scale; scale /= 10; ++k; }
            ms = static_cast<int>(frac);
          }
        }
        have_time = true;
        i = k;
        continue;
      }
      if (j < n && (in[j] == '/' || (in[j] == '-' && digits >= 1 && month < 0 && nums.empty() && j + 1 < n && std::isdigit(static_cast<unsigned char>(in[j + 1]))))) {
        // numeric date: m/d/y, y/m/d, y-m-d
        std::vector<long> parts{v};
        std::vector<size_t> lens{digits};
        const char sep = in[j];
        size_t k = j;
        while (k < n && in[k] == sep) {
          ++k;
          long pv = 0;
          size_t st = k;
          while (k < n && std::isdigit(static_cast<unsigned char>(in[k]))) { pv = pv * 10 + (in[k] - '0'); ++k; }
          if (k == st) return false;
          parts.push_back(pv);
          lens.push_back(k - st);
        }
        if (parts.size() == 3) {
          if (lens[0] >= 3) { year = static_cast<double>(parts[0]); month = static_cast<int>(parts[1]) - 1; day = static_cast<int>(parts[2]); }
          else { month = static_cast<int>(parts[0]) - 1; day = static_cast<int>(parts[1]); year = static_cast<double>(parts[2]); if (lens[2] <= 2) year += parts[2] < 50 ? 2000 : 1900; }
        } else if (parts.size() == 2) {
          month = static_cast<int>(parts[0]) - 1;
          day = static_cast<int>(parts[1]);
        } else {
          return false;
        }
        i = k;
        continue;
      }
      if (digits >= 3 || v > 31) {
        if (std::isnan(year)) year = static_cast<double>(v);
        else return false;
      } else {
        nums.push_back(static_cast<int>(v));
      }
      i = j;
      continue;
    }
    ++i;   // separators: space , . etc
  }
  // assign the leftover small numbers: with a month name, the first is the day, the second (if any) a two-digit year
  for (int v : nums) {
    if (day < 0) day = v;
    else if (std::isnan(year)) year = v < 50 ? 2000 + v : 1900 + v;
    else return false;
  }
  if (month < 0 || std::isnan(year)) return false;
  if (day < 0) day = 1;
  if (pm && hour < 12) hour += 12;
  if (am && hour == 12) hour = 0;
  if (month > 11 || day > 31 || hour > 24 || minute > 59 || second > 59) return false;
  *out = make_date(make_day(year, month, day), make_time(hour, minute, second, ms));
  return true;
}

// Returns the UTC time value, or NaN.
double parse_date(Context& ctx, const std::u16string& s16) {
  std::string s;
  for (char16_t c : s16) s.push_back(c < 128 ? static_cast<char>(c) : '?');
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  s = s.substr(b, e - b);
  double t, off;
  bool date_only, has_off, invalid;
  if (parse_iso(s, &t, &date_only, &has_off, &off, &invalid)) {
    if (date_only || has_off) return time_clip(t - off);
    return time_clip(utc_from_local(ctx, t));
  }
  if (invalid) return NAN;
  if (parse_legacy(s, &t, &has_off, &off)) {
    if (has_off) return time_clip(t - off);
    return time_clip(utc_from_local(ctx, t));
  }
  return NAN;
}

// ---- formatting ----

std::string tz_offset_string(double offset_ms, bool colon) {
  const long total = static_cast<long>(offset_ms / 60000.0);
  const long a = total < 0 ? -total : total;
  return std::string(total < 0 ? "-" : "+") + pad(a / 60, 2) + (colon ? ":" : "") + pad(a % 60, 2);
}

std::string tz_name(Context& ctx, double utc) {
  if (ctx.tz_name) return ctx.tz_name(utc);
  const double off = local_offset_ms(ctx, utc);
  if (off == 0) return "Coordinated Universal Time";
  return "GMT" + tz_offset_string(off, true);
}

std::string date_part(const Fields& f) {
  return std::string(kDays[f.weekday]) + " " + kMonths[f.month] + " " + pad(f.day, 2) + " " + (f.year < 0 ? "-" + pad(static_cast<long>(-f.year), 6) : pad(static_cast<long>(f.year), 4));
}

std::string time_part(Context& ctx, double utc, const Fields& f) {
  const double off = local_offset_ms(ctx, utc);
  return pad(f.hour, 2) + ":" + pad(f.minute, 2) + ":" + pad(f.second, 2) + " GMT" + tz_offset_string(off, false) + " (" + tz_name(ctx, utc) + ")";
}

std::string iso_string(double t) {
  Fields f = split(t);
  std::string y = (f.year >= 0 && f.year <= 9999) ? pad(static_cast<long>(f.year), 4) : (f.year < 0 ? "-" : "+") + pad(static_cast<long>(std::fabs(f.year)), 6);
  return y + "-" + pad(f.month + 1, 2) + "-" + pad(f.day, 2) + "T" + pad(f.hour, 2) + ":" + pad(f.minute, 2) + ":" + pad(f.second, 2) + "." + pad(f.ms, 3) + "Z";
}

// ---- the object ----

DateObject* this_date(Context& ctx, const Value& v, const char* method) {
  if (v.is_object() && v.as_object()->cls == Class::Date) return static_cast<DateObject*>(v.as_object());
  ctx.throw_type_error(std::string("this is not a Date object."));
  (void)method;
  return nullptr;
}

double now_ms(Context& ctx) {
  if (ctx.now_ms) return std::floor(ctx.now_ms());
  return 0;
}

Value date_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    double t = now_ms(ctx);
    Fields f = split(local_time(ctx, t));
    return Str(ctx, date_part(f) + " " + time_part(ctx, t, f));
  }
  double tv;
  if (a.argc == 0) {
    tv = now_ms(ctx);
  } else if (a.argc == 1) {
    Value v = a[0];
    if (v.is_object() && v.as_object()->cls == Class::Date) {
      tv = static_cast<DateObject*>(v.as_object())->t;
    } else {
      Value p;
      JS_TRY(ctx.to_primitive(v, Hint::Default, &p));
      if (p.is_string()) tv = parse_date(ctx, p.as_string()->str());
      else {
        double d;
        JS_TRY(ctx.to_number(p, &d));
        tv = time_clip(d);
      }
    }
  } else {
    double n[7] = {NAN, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < 7 && i < a.argc; ++i) JS_TRY(ctx.to_number(a.argv[i], &n[i]));
    double y = n[0];
    if (!std::isnan(y)) {
      double yi = std::trunc(y);
      if (yi >= 0 && yi <= 99) y = 1900 + yi;
    }
    double local = make_date(make_day(y, n[1], n[2]), make_time(n[3], n[4], n[5], n[6]));
    tv = time_clip(utc_from_local(ctx, local));
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.date_proto, &proto));
  DateObject* d = ctx.heap.make<DateObject>();
  d->proto = proto;
  d->t = tv;
  return Value::Obj(d);
}

Value date_now(Context& ctx, CallArgs&) { return Value::Number(now_ms(ctx)); }

Value date_parse(Context& ctx, CallArgs& a) {
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  return Value::Number(parse_date(ctx, s->str()));
}

Value date_utc(Context& ctx, CallArgs& a) {
  double n[7] = {NAN, 0, 1, 0, 0, 0, 0};
  for (int i = 0; i < 7 && i < a.argc; ++i) JS_TRY(ctx.to_number(a.argv[i], &n[i]));
  double y = n[0];
  if (!std::isnan(y)) {
    double yi = std::trunc(y);
    if (yi >= 0 && yi <= 99) y = 1900 + yi;
  }
  return Value::Number(time_clip(make_date(make_day(y, n[1], n[2]), make_time(n[3], n[4], n[5], n[6]))));
}

// ---- getters ----

enum Field { F_YEAR, F_MONTH, F_DATE, F_DAY, F_HOURS, F_MINUTES, F_SECONDS, F_MS };

double field_of(const Fields& f, Field which) {
  switch (which) {
    case F_YEAR: return f.year;
    case F_MONTH: return f.month;
    case F_DATE: return f.day;
    case F_DAY: return f.weekday;
    case F_HOURS: return f.hour;
    case F_MINUTES: return f.minute;
    case F_SECONDS: return f.second;
    default: return f.ms;
  }
}

Value getter(Context& ctx, CallArgs& a, Field which, bool utc) {
  DateObject* d = this_date(ctx, a.this_value, "get");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Value::Number(NAN);
  return Value::Number(field_of(split(utc ? d->t : local_time(ctx, d->t)), which));
}

#define GETTER(name, field, utc) \
  Value name(Context& ctx, CallArgs& a) { return getter(ctx, a, field, utc); }
GETTER(date_get_full_year, F_YEAR, false) GETTER(date_get_month, F_MONTH, false) GETTER(date_get_date, F_DATE, false) GETTER(date_get_day, F_DAY, false)
GETTER(date_get_hours, F_HOURS, false) GETTER(date_get_minutes, F_MINUTES, false) GETTER(date_get_seconds, F_SECONDS, false) GETTER(date_get_ms, F_MS, false)
GETTER(date_get_utc_full_year, F_YEAR, true) GETTER(date_get_utc_month, F_MONTH, true) GETTER(date_get_utc_date, F_DATE, true) GETTER(date_get_utc_day, F_DAY, true)
GETTER(date_get_utc_hours, F_HOURS, true) GETTER(date_get_utc_minutes, F_MINUTES, true) GETTER(date_get_utc_seconds, F_SECONDS, true) GETTER(date_get_utc_ms, F_MS, true)
#undef GETTER

Value date_get_year(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "getYear");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Value::Number(NAN);
  return Value::Number(split(local_time(ctx, d->t)).year - 1900);
}

Value date_get_time(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "getTime");
  if (!d) return Value::Exception();
  return Value::Number(d->t);
}

Value date_get_timezone_offset(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "getTimezoneOffset");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Value::Number(NAN);
  return Value::Number(-local_offset_ms(ctx, d->t) / 60000.0 + 0.0);
}

// ---- setters ----

// which: first field being set; count: how many arguments it accepts.
Value setter(Context& ctx, CallArgs& a, Field first, int max_args, bool utc) {
  DateObject* d = this_date(ctx, a.this_value, "set");
  if (!d) return Value::Exception();
  double t = d->t;
  std::vector<double> args;
  for (int i = 0; i < max_args && i < a.argc; ++i) {
    double v;
    JS_TRY(ctx.to_number(a.argv[i], &v));
    args.push_back(v);
  }
  if (a.argc == 0) args.push_back(NAN);
  if (std::isnan(t)) {
    if (first != F_YEAR) return Value::Number(NAN);
    t = 0;
  } else if (!utc) {
    t = local_time(ctx, t);
  }
  Fields f = split(t);
  double year = f.year, month = f.month, date = f.day, hour = f.hour, minute = f.minute, second = f.second, ms = f.ms;
  double* slots[] = {&year, &month, &date, nullptr, &hour, &minute, &second, &ms};
  int idx = static_cast<int>(first);
  for (double v : args) {
    while (idx < 8 && !slots[idx]) ++idx;
    if (idx >= 8) break;
    *slots[idx++] = v;
  }
  double nt = make_date(make_day(year, month, date), make_time(hour, minute, second, ms));
  nt = time_clip(utc ? nt : utc_from_local(ctx, nt));
  d->t = nt;
  return Value::Number(nt);
}

#define SETTER(name, field, n, utc) \
  Value name(Context& ctx, CallArgs& a) { return setter(ctx, a, field, n, utc); }
SETTER(date_set_full_year, F_YEAR, 3, false) SETTER(date_set_month, F_MONTH, 2, false) SETTER(date_set_date, F_DATE, 1, false)
SETTER(date_set_hours, F_HOURS, 4, false) SETTER(date_set_minutes, F_MINUTES, 3, false) SETTER(date_set_seconds, F_SECONDS, 2, false) SETTER(date_set_ms, F_MS, 1, false)
SETTER(date_set_utc_full_year, F_YEAR, 3, true) SETTER(date_set_utc_month, F_MONTH, 2, true) SETTER(date_set_utc_date, F_DATE, 1, true)
SETTER(date_set_utc_hours, F_HOURS, 4, true) SETTER(date_set_utc_minutes, F_MINUTES, 3, true) SETTER(date_set_utc_seconds, F_SECONDS, 2, true) SETTER(date_set_utc_ms, F_MS, 1, true)
#undef SETTER

Value date_set_time(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "setTime");
  if (!d) return Value::Exception();
  double v;
  JS_TRY(ctx.to_number(a[0], &v));
  d->t = time_clip(v);
  return Value::Number(d->t);
}

Value date_set_year(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "setYear");
  if (!d) return Value::Exception();
  double y;
  JS_TRY(ctx.to_number(a[0], &y));
  if (std::isnan(y)) { d->t = NAN; return Value::Number(NAN); }
  double yi = std::trunc(y);
  if (yi >= 0 && yi <= 99) yi += 1900;
  double t = std::isnan(d->t) ? 0 : local_time(ctx, d->t);
  Fields f = split(t);
  double nt = make_date(make_day(yi, f.month, f.day), make_time(f.hour, f.minute, f.second, f.ms));
  d->t = time_clip(utc_from_local(ctx, nt));
  return Value::Number(d->t);
}

// ---- conversions ----

Value date_to_string(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "toString");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Str(ctx, "Invalid Date");
  Fields f = split(local_time(ctx, d->t));
  return Str(ctx, date_part(f) + " " + time_part(ctx, d->t, f));
}

Value date_to_date_string(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "toDateString");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Str(ctx, "Invalid Date");
  return Str(ctx, date_part(split(local_time(ctx, d->t))));
}

Value date_to_time_string(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "toTimeString");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Str(ctx, "Invalid Date");
  return Str(ctx, time_part(ctx, d->t, split(local_time(ctx, d->t))));
}

Value date_to_utc_string(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "toUTCString");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Str(ctx, "Invalid Date");
  Fields f = split(d->t);
  return Str(ctx, std::string(kDays[f.weekday]) + ", " + pad(f.day, 2) + " " + kMonths[f.month] + " " + (f.year < 0 ? "-" + pad(static_cast<long>(-f.year), 6) : pad(static_cast<long>(f.year), 4)) + " " +
                      pad(f.hour, 2) + ":" + pad(f.minute, 2) + ":" + pad(f.second, 2) + " GMT");
}

Value date_to_iso_string(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "toISOString");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) {
    ctx.throw_range_error("Invalid time value");
    return Value::Exception();
  }
  return Str(ctx, iso_string(d->t));
}

Value date_to_json(Context& ctx, CallArgs& a) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  Value tv;
  JS_TRY(ctx.to_primitive(Value::Obj(o), Hint::Number, &tv));
  if (tv.is_number() && !std::isfinite(tv.num)) return Value::Null();
  Value f;
  JS_TRY(o->get(ctx, ctx.atom_ascii("toISOString"), Value::Obj(o), &f));
  if (!f.is_callable()) {
    ctx.throw_type_error("toISOString is not a function");
    return Value::Exception();
  }
  Value r;
  JS_TRY(ctx.call(f, Value::Obj(o), nullptr, 0, &r));
  return r;
}

Value date_value_of(Context& ctx, CallArgs& a) {
  DateObject* d = this_date(ctx, a.this_value, "valueOf");
  if (!d) return Value::Exception();
  return Value::Number(d->t);
}

Value date_to_primitive(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("Date.prototype[Symbol.toPrimitive] called on non-object");
    return Value::Exception();
  }
  Hint hint;
  if (a[0].is_string()) {
    const std::u16string& h = a[0].as_string()->str();
    if (h == u"string" || h == u"default") hint = Hint::String;
    else if (h == u"number") hint = Hint::Number;
    else {
      ctx.throw_type_error("Invalid hint: " + vm_describe(ctx, a[0]));
      return Value::Exception();
    }
  } else {
    ctx.throw_type_error("Invalid hint: " + vm_describe(ctx, a[0]));
    return Value::Exception();
  }
  Object* o = a.this_value.as_object();
  const char* order[2] = {hint == Hint::String ? "toString" : "valueOf", hint == Hint::String ? "valueOf" : "toString"};
  for (const char* name : order) {
    Value f;
    JS_TRY(o->get(ctx, ctx.atom_ascii(name), a.this_value, &f));
    if (f.is_callable()) {
      Value r;
      JS_TRY(ctx.call(f, a.this_value, nullptr, 0, &r));
      if (!r.is_object()) return r;
    }
  }
  ctx.throw_type_error("Cannot convert object to primitive value");
  return Value::Exception();
}

// ---- locale formatting (en-US) ----

struct LocaleOptions {
  bool has_date = false, has_time = false;
  std::string weekday, year, month, day, hour, minute, second;
  int hour12 = -1;   // -1 default (12h in en-US)
  bool utc = false;
  std::string tz_name_style;
  double fixed_offset_ms = NAN;   // a numeric time zone like "+05:30"
  std::string date_style, time_style;
};

bool read_option(Context& ctx, Object* o, const char* name, std::string* out) {
  Value v;
  if (!o->get(ctx, ctx.atom_ascii(name), Value::Obj(o), &v)) return false;
  if (v.is_undefined()) return true;
  JsString* s;
  if (!ctx.to_string(v, &s)) return false;
  *out = s->utf8();
  return true;
}

bool parse_locale_options(Context& ctx, const Value& opts, int defaults, LocaleOptions* lo) {
  // defaults: 1 date, 2 time, 3 both (used when no component is given)
  if (opts.is_object()) {
    Object* o = opts.as_object();
    if (!read_option(ctx, o, "weekday", &lo->weekday) || !read_option(ctx, o, "year", &lo->year) || !read_option(ctx, o, "month", &lo->month) ||
        !read_option(ctx, o, "day", &lo->day) || !read_option(ctx, o, "hour", &lo->hour) || !read_option(ctx, o, "minute", &lo->minute) ||
        !read_option(ctx, o, "second", &lo->second) || !read_option(ctx, o, "timeZoneName", &lo->tz_name_style) ||
        !read_option(ctx, o, "dateStyle", &lo->date_style) || !read_option(ctx, o, "timeStyle", &lo->time_style))
      return false;
    std::string tz;
    if (!read_option(ctx, o, "timeZone", &tz)) return false;
    if (!tz.empty()) {
      std::string up;
      for (char c : tz) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      if (up == "UTC" || up == "GMT" || up == "ETC/UTC" || up == "ETC/GMT") lo->utc = true;
      else if ((tz[0] == '+' || tz[0] == '-') && tz.size() >= 3) {
        const int sign = tz[0] == '-' ? -1 : 1;
        int hh = std::atoi(tz.substr(1, 2).c_str()), mm = tz.size() >= 6 ? std::atoi(tz.substr(4, 2).c_str()) : 0;
        lo->fixed_offset_ms = sign * (hh * 60 + mm) * 60000.0;
      } else {
        ctx.throw_range_error("Invalid time zone specified: " + tz);
        return false;
      }
    }
    Value h12;
    if (!o->get(ctx, ctx.atom_ascii("hour12"), opts, &h12)) return false;
    if (!h12.is_undefined()) lo->hour12 = Context::to_boolean(h12) ? 1 : 0;
    std::string cycle;
    if (!read_option(ctx, o, "hourCycle", &cycle)) return false;
    if (lo->hour12 < 0 && (cycle == "h23" || cycle == "h24")) lo->hour12 = 0;
    if (lo->hour12 < 0 && (cycle == "h11" || cycle == "h12")) lo->hour12 = 1;
  }
  if (!lo->date_style.empty() || !lo->time_style.empty()) {
    if (lo->date_style == "full") { lo->weekday = "long"; lo->month = "long"; lo->day = "numeric"; lo->year = "numeric"; }
    else if (lo->date_style == "long") { lo->month = "long"; lo->day = "numeric"; lo->year = "numeric"; }
    else if (lo->date_style == "medium") { lo->month = "short"; lo->day = "numeric"; lo->year = "numeric"; }
    else if (lo->date_style == "short") { lo->month = "numeric"; lo->day = "numeric"; lo->year = "2-digit"; }
    if (!lo->time_style.empty()) {
      lo->hour = "numeric";
      lo->minute = "2-digit";
      if (lo->time_style != "short") lo->second = "2-digit";
      if (lo->time_style == "full" || lo->time_style == "long") lo->tz_name_style = "short";
    }
  }
  lo->has_date = !(lo->weekday.empty() && lo->year.empty() && lo->month.empty() && lo->day.empty());
  lo->has_time = !(lo->hour.empty() && lo->minute.empty() && lo->second.empty());
  if (!lo->has_date && !lo->has_time) {
    if (defaults & 1) { lo->year = "numeric"; lo->month = "numeric"; lo->day = "numeric"; lo->has_date = true; }
    if (defaults & 2) { lo->hour = "numeric"; lo->minute = "2-digit"; lo->second = "2-digit"; lo->has_time = true; }
  }
  return true;
}

std::string format_locale(Context& ctx, double t, const LocaleOptions& lo) {
  double offset = lo.utc ? 0 : (std::isnan(lo.fixed_offset_ms) ? local_offset_ms(ctx, t) : lo.fixed_offset_ms);
  Fields f = split(t + offset);
  std::string date;
  if (lo.has_date) {
    std::string month_text;
    const bool text_month = lo.month == "long" || lo.month == "short" || lo.month == "narrow";
    if (lo.month == "long") month_text = kLongMonths[f.month];
    else if (lo.month == "short") month_text = kMonths[f.month];
    else if (lo.month == "narrow") month_text = std::string(1, kLongMonths[f.month][0]);
    else if (lo.month == "2-digit") month_text = pad(f.month + 1, 2);
    else if (lo.month == "numeric") month_text = std::to_string(f.month + 1);
    std::string day_text = lo.day == "2-digit" ? pad(f.day, 2) : lo.day.empty() ? "" : std::to_string(f.day);
    std::string year_text;
    if (lo.year == "2-digit") year_text = pad(static_cast<long>(std::fmod(std::fabs(f.year), 100)), 2);
    else if (!lo.year.empty()) year_text = std::to_string(static_cast<long>(f.year));
    std::string wd;
    if (lo.weekday == "long") wd = kLongDays[f.weekday];
    else if (lo.weekday == "short") wd = kDays[f.weekday];
    else if (lo.weekday == "narrow") wd = std::string(1, kLongDays[f.weekday][0]);
    if (text_month) {
      // "Wednesday, January 15, 2020"
      std::string md = month_text + (day_text.empty() ? "" : " " + day_text);
      date = md + (year_text.empty() ? "" : (day_text.empty() ? " " : ", ") + year_text);
      if (!wd.empty()) date = wd + ", " + date;
    } else {
      std::vector<std::string> parts;
      if (!month_text.empty()) parts.push_back(month_text);
      if (!day_text.empty()) parts.push_back(day_text);
      if (!year_text.empty()) parts.push_back(year_text);
      for (size_t i = 0; i < parts.size(); ++i) date += (i ? "/" : "") + parts[i];
      if (!wd.empty()) date = date.empty() ? wd : wd + ", " + date;
    }
  }
  std::string time;
  if (lo.has_time) {
    const bool h12 = lo.hour12 != 0;
    std::string suffix;
    int hh = f.hour;
    if (!lo.hour.empty() && h12) {
      suffix = hh >= 12 ? " PM" : " AM";
      hh = hh % 12;
      if (hh == 0) hh = 12;
    }
    std::vector<std::string> parts;
    if (!lo.hour.empty()) parts.push_back(lo.hour == "2-digit" || !h12 ? pad(hh, 2) : std::to_string(hh));
    if (!lo.minute.empty()) parts.push_back(lo.hour.empty() && lo.minute != "2-digit" ? std::to_string(f.minute) : pad(f.minute, 2));
    if (!lo.second.empty()) parts.push_back(lo.hour.empty() && lo.minute.empty() && lo.second != "2-digit" ? std::to_string(f.second) : pad(f.second, 2));
    for (size_t i = 0; i < parts.size(); ++i) time += (i ? ":" : "") + parts[i];
    time += suffix;
    if (!lo.tz_name_style.empty()) {
      time += " ";
      if (lo.utc) time += "UTC";
      else if (offset == 0) time += "UTC";
      else time += "GMT" + tz_offset_string(offset, true);
    }
  }
  if (!date.empty() && !time.empty()) return date + (lo.date_style == "full" || lo.date_style == "long" ? " at " : ", ") + time;
  return date.empty() ? time : date;
}

Value locale_impl(Context& ctx, CallArgs& a, int defaults) {
  DateObject* d = this_date(ctx, a.this_value, "toLocaleString");
  if (!d) return Value::Exception();
  if (std::isnan(d->t)) return Str(ctx, "Invalid Date");
  LocaleOptions lo;
  JS_TRY(parse_locale_options(ctx, a[1], defaults, &lo));
  return Str(ctx, format_locale(ctx, d->t, lo));
}

Value date_to_locale_string(Context& ctx, CallArgs& a) { return locale_impl(ctx, a, 3); }
Value date_to_locale_date_string(Context& ctx, CallArgs& a) { return locale_impl(ctx, a, 1); }
Value date_to_locale_time_string(Context& ctx, CallArgs& a) { return locale_impl(ctx, a, 2); }

}  // namespace

void setup_date(Context& ctx) {
  Object* proto = ctx.new_object();
  ctx.intr.date_proto = proto;
  Function* ctor = new_ctor(ctx, "Date", date_ctor, 7, proto);
  ctx.define_method(ctor, "now", date_now, 0);
  ctx.define_method(ctor, "parse", date_parse, 1);
  ctx.define_method(ctor, "UTC", date_utc, 7);
  struct M { const char* n; NativeFn fn; int len; } methods[] = {
      {"getDate", date_get_date, 0}, {"getDay", date_get_day, 0}, {"getFullYear", date_get_full_year, 0}, {"getHours", date_get_hours, 0},
      {"getMilliseconds", date_get_ms, 0}, {"getMinutes", date_get_minutes, 0}, {"getMonth", date_get_month, 0}, {"getSeconds", date_get_seconds, 0},
      {"getTime", date_get_time, 0}, {"getTimezoneOffset", date_get_timezone_offset, 0}, {"getUTCDate", date_get_utc_date, 0}, {"getUTCDay", date_get_utc_day, 0},
      {"getUTCFullYear", date_get_utc_full_year, 0}, {"getUTCHours", date_get_utc_hours, 0}, {"getUTCMilliseconds", date_get_utc_ms, 0},
      {"getUTCMinutes", date_get_utc_minutes, 0}, {"getUTCMonth", date_get_utc_month, 0}, {"getUTCSeconds", date_get_utc_seconds, 0}, {"getYear", date_get_year, 0},
      {"setDate", date_set_date, 1}, {"setFullYear", date_set_full_year, 3}, {"setHours", date_set_hours, 4}, {"setMilliseconds", date_set_ms, 1},
      {"setMinutes", date_set_minutes, 3}, {"setMonth", date_set_month, 2}, {"setSeconds", date_set_seconds, 2}, {"setTime", date_set_time, 1},
      {"setUTCDate", date_set_utc_date, 1}, {"setUTCFullYear", date_set_utc_full_year, 3}, {"setUTCHours", date_set_utc_hours, 4},
      {"setUTCMilliseconds", date_set_utc_ms, 1}, {"setUTCMinutes", date_set_utc_minutes, 3}, {"setUTCMonth", date_set_utc_month, 2},
      {"setUTCSeconds", date_set_utc_seconds, 2}, {"setYear", date_set_year, 1}, {"toDateString", date_to_date_string, 0}, {"toISOString", date_to_iso_string, 0},
      {"toJSON", date_to_json, 1}, {"toLocaleDateString", date_to_locale_date_string, 0}, {"toLocaleString", date_to_locale_string, 0},
      {"toLocaleTimeString", date_to_locale_time_string, 0}, {"toString", date_to_string, 0}, {"toTimeString", date_to_time_string, 0},
      {"toUTCString", date_to_utc_string, 0}, {"valueOf", date_value_of, 0}};
  for (const M& m : methods) ctx.define_method(proto, m.n, m.fn, m.len);
  Value utc;
  ctx.get(proto, ctx.atom_ascii("toUTCString"), &utc);
  ctx.define_data(proto, ctx.atom_ascii("toGMTString"), utc, kHiddenAttrs);
  ctx.define_method(proto, ctx.sym.to_primitive, "[Symbol.toPrimitive]", date_to_primitive, 1, kConfigurable);
}

// Used by Intl.DateTimeFormat.
std::string date_format_locale(Context& ctx, double t, const Value& options, int defaults, bool* ok) {
  LocaleOptions lo;
  *ok = parse_locale_options(ctx, options, defaults, &lo);
  if (!*ok) return "";
  return format_locale(ctx, t, lo);
}

}  // namespace aios::js
