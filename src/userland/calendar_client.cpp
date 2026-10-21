#include "aios/userland/calendar_client.hpp"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <charconv>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
// std::stoi/std::stoul throw on a bad field, and bare metal is built with
// -fno-exceptions -- a malformed index line (a missing number, a truncated
// write) would fail to even compile there, and on a hosted build the same
// bad line would terminate the process instead of just skipping the field.
// from_chars never throws by design, which is the actual property this
// parser needs, not "somewhere to catch the throw."
int parse_int(const std::string& s, int fallback = 0) {
  int v = fallback;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}
unsigned long parse_hex(const std::string& s, unsigned long fallback) {
  unsigned long v = fallback;
  std::from_chars(s.data(), s.data() + s.size(), v, 16);
  return v;
}
}  // namespace

namespace aios::userland {

namespace {

void ensure_dir(const std::string& path) {
  mkdir(path.c_str(), 0755);
}

std::string trim_str(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string escape_field(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\t' || c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}

}  // namespace

CalendarStore::CalendarStore(const std::string& base_dir)
    : base_dir_(base_dir),
      db_path_(base_dir + "/calendar.idx"),
      events_dir_(base_dir + "/events") {
  ensure_dir(base_dir_);
  ensure_dir(events_dir_);

  categories_ = {
    {"work", "Work", 0xFF0A84FF, true},
    {"personal", "Personal", 0xFF30D158, true},
    {"releases", "Releases", 0xFFBF5AF2, true},
    {"ops", "Operations", 0xFFFF9F0A, true}
  };

  reload();
}

void CalendarStore::reload() {
  events_.clear();
  load_index();
}

void CalendarStore::save_all() {
  save_index();
  for (const auto& ev : events_) {
    save_ics_file(ev);
  }
}

void CalendarStore::load_index() {
  std::ifstream f(db_path_);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    line = trim_str(line);
    if (line.empty() || line[0] == '#') continue;

    std::stringstream ss(line);
    std::string id, title, loc, cat, y_s, m_s, d_s, sh_s, sm_s, dur_s, all_day_s, desc, att_s, rec_s, col_s;
    
    std::getline(ss, id, '\t');
    std::getline(ss, title, '\t');
    std::getline(ss, loc, '\t');
    std::getline(ss, cat, '\t');
    std::getline(ss, y_s, '\t');
    std::getline(ss, m_s, '\t');
    std::getline(ss, d_s, '\t');
    std::getline(ss, sh_s, '\t');
    std::getline(ss, sm_s, '\t');
    std::getline(ss, dur_s, '\t');
    std::getline(ss, all_day_s, '\t');
    std::getline(ss, desc, '\t');
    std::getline(ss, att_s, '\t');
    std::getline(ss, rec_s, '\t');
    std::getline(ss, col_s, '\t');

    if (id.empty()) continue;

    CalendarEvent ev;
    ev.id = id;
    ev.title = title;
    ev.location = loc;
    ev.calendar_id = cat;
    ev.year = parse_int(y_s);
    ev.month = parse_int(m_s);
    ev.day = parse_int(d_s);
    ev.start_hour = parse_int(sh_s);
    ev.start_min = parse_int(sm_s);
    ev.duration_mins = parse_int(dur_s);

    ev.all_day = (all_day_s == "1" || all_day_s == "true");
    ev.description = desc;
    ev.recurrence = rec_s.empty() ? "NONE" : rec_s;
    ev.color = col_s.empty() ? 0xFF0A84FF : parse_hex(col_s, 0xFF0A84FF);
    ev.ics_path = events_dir_ + "/" + id + ".ics";

    std::stringstream att_ss(att_s);
    std::string att;
    while (std::getline(att_ss, att, ',')) {
      att = trim_str(att);
      if (!att.empty()) ev.attendees.push_back(att);
    }

    events_.push_back(ev);
  }
}

void CalendarStore::save_index() {
  std::string tmp_path = db_path_ + ".tmp";
  std::ofstream f(tmp_path);
  if (!f.is_open()) return;

  f << "# AIOS Native Calendar Index (iCal Mirror)\n";
  f << "# id\ttitle\tlocation\tcalendar_id\tyear\tmonth\tday\tstart_hour\tstart_min\tduration\tall_day\tdescription\tattendees\trecurrence\tcolor\n";

  for (const auto& ev : events_) {
    std::string att_str;
    for (size_t i = 0; i < ev.attendees.size(); ++i) {
      if (i > 0) att_str += ",";
      att_str += ev.attendees[i];
    }

    char col_buf[16];
    std::snprintf(col_buf, sizeof(col_buf), "0x%08X", ev.color);

    f << escape_field(ev.id) << "\t"
      << escape_field(ev.title) << "\t"
      << escape_field(ev.location) << "\t"
      << escape_field(ev.calendar_id) << "\t"
      << ev.year << "\t"
      << ev.month << "\t"
      << ev.day << "\t"
      << ev.start_hour << "\t"
      << ev.start_min << "\t"
      << ev.duration_mins << "\t"
      << (ev.all_day ? "1" : "0") << "\t"
      << escape_field(ev.description) << "\t"
      << escape_field(att_str) << "\t"
      << escape_field(ev.recurrence) << "\t"
      << col_buf << "\n";
  }

  f.close();
  rename(tmp_path.c_str(), db_path_.c_str());
}

namespace {
// Civil date <-> day number (Howard Hinnant's algorithms), so an event's end
// can roll over midnight / month / year without depending on libc's
// timegm/gmtime, which bare metal does not have.
long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}
void civil_from_days(long z, int& y, int& m, int& d) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = (int)(yoe + era * 400 + (m <= 2));
}
// RFC 5545 3.3.11: TEXT escapes backslash, ';', ',' and newlines.
std::string ics_text(const std::string& v) {
  std::string o;
  for (char c : v) {
    if (c == '\\' || c == ';' || c == ',') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c != '\r') o += c;
  }
  return o;
}
// One VEVENT, complete: UID, DTSTAMP (required), DTSTART, DTEND, and every
// field the event carries. The .ics file and `calendar export` both write
// this, so the two cannot drift apart again (the export used to emit only
// UID/SUMMARY/DTSTART -- no end, no place, and no DTSTAMP at all).
std::string vevent_block(const CalendarEvent& ev) {
  char buf[64];
  std::string out = "BEGIN:VEVENT\r\n";
  out += "UID:" + ev.id + "@aios.org\r\n";
  {
    const long long now = (long long)std::time(nullptr);
    long days = (long)(now / 86400);
    long secs = (long)(now % 86400);
    int y, m, d;
    civil_from_days(days, y, m, d);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02ld%02ld%02ldZ", y, m, d, secs / 3600, (secs / 60) % 60, secs % 60);
    out += std::string("DTSTAMP:") + buf + "\r\n";
  }
  out += "SUMMARY:" + ics_text(ev.title) + "\r\n";
  if (!ev.location.empty()) out += "LOCATION:" + ics_text(ev.location) + "\r\n";
  if (!ev.description.empty()) out += "DESCRIPTION:" + ics_text(ev.description) + "\r\n";
  const long day0 = days_from_civil(ev.year, ev.month, ev.day);
  int ey, em, ed;
  if (ev.all_day) {
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", ev.year, ev.month, ev.day);
    out += std::string("DTSTART;VALUE=DATE:") + buf + "\r\n";
    civil_from_days(day0 + 1, ey, em, ed);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", ey, em, ed);
    out += std::string("DTEND;VALUE=DATE:") + buf + "\r\n";
  } else {
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d00", ev.year, ev.month, ev.day, ev.start_hour, ev.start_min);
    out += std::string("DTSTART:") + buf + "\r\n";
    const long end_min = (long)ev.start_hour * 60 + ev.start_min + (ev.duration_mins > 0 ? ev.duration_mins : 0);
    civil_from_days(day0 + end_min / 1440, ey, em, ed);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02ld%02ld00", ey, em, ed, (end_min % 1440) / 60, end_min % 60);
    out += std::string("DTEND:") + buf + "\r\n";
  }
  if (!ev.calendar_id.empty()) out += "CATEGORIES:" + ics_text(ev.calendar_id) + "\r\n";
  for (const auto& att : ev.attendees) out += "ATTENDEE;CN=" + ics_text(att) + ":mailto:" + att + "\r\n";
  if (!ev.recurrence.empty() && ev.recurrence != "NONE") out += "RRULE:FREQ=" + ev.recurrence + "\r\n";
  out += "END:VEVENT\r\n";
  return out;
}
}  // namespace

void CalendarStore::save_ics_file(const CalendarEvent& ev) {
  std::string path = events_dir_ + "/" + ev.id + ".ics";
  std::ofstream f(path);
  if (!f.is_open()) return;
  f << "BEGIN:VCALENDAR\r\n";
  f << "VERSION:2.0\r\n";
  f << "PRODID:-//AIOS//AetherCalendar 1.0//EN\r\n";
  f << vevent_block(ev);
  f << "END:VCALENDAR\r\n";
}

void CalendarStore::delete_ics_file(const std::string& id) {
  std::string path = events_dir_ + "/" + id + ".ics";
  unlink(path.c_str());
}


std::vector<CalendarEvent> CalendarStore::all_events() const {
  std::vector<CalendarEvent> res;
  for (const auto& ev : events_) {
    if (is_category_visible(ev.calendar_id)) {
      res.push_back(ev);
    }
  }
  std::sort(res.begin(), res.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
    if (a.year != b.year) return a.year < b.year;
    if (a.month != b.month) return a.month < b.month;
    if (a.day != b.day) return a.day < b.day;
    if (a.start_hour != b.start_hour) return a.start_hour < b.start_hour;
    return a.start_min < b.start_min;
  });
  return res;
}

std::vector<CalendarEvent> CalendarStore::events_for_day(int year, int month, int day) const {
  std::vector<CalendarEvent> res;
  for (const auto& ev : events_) {
    if (ev.year == year && ev.month == month && ev.day == day && is_category_visible(ev.calendar_id)) {
      res.push_back(ev);
    }
  }
  std::sort(res.begin(), res.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
    if (a.start_hour != b.start_hour) return a.start_hour < b.start_hour;
    return a.start_min < b.start_min;
  });
  return res;
}

std::vector<CalendarEvent> CalendarStore::events_for_month(int year, int month) const {
  std::vector<CalendarEvent> res;
  for (const auto& ev : events_) {
    if (ev.year == year && ev.month == month && is_category_visible(ev.calendar_id)) {
      res.push_back(ev);
    }
  }
  return res;
}

const CalendarEvent* CalendarStore::get_event(const std::string& id) const {
  for (const auto& ev : events_) {
    if (ev.id == id) return &ev;
  }
  return nullptr;
}

bool CalendarStore::add_event(CalendarEvent& ev) {
  if (ev.id.empty()) {
    int max_id = 100;
    for (const auto& e : events_) {
      if (e.id.rfind("ev-", 0) == 0) {
        int n = parse_int(e.id.substr(3), -1);
        if (n > max_id) max_id = n;
      }
    }
    ev.id = "ev-" + std::to_string(max_id + 1);
  }

  if (ev.color == 0) {
    for (const auto& cat : categories_) {
      if (cat.id == ev.calendar_id) {
        ev.color = cat.color;
        break;
      }
    }
  }
  if (ev.color == 0) ev.color = 0xFF0A84FF;

  ev.ics_path = events_dir_ + "/" + ev.id + ".ics";
  events_.push_back(ev);
  save_all();
  return true;
}

bool CalendarStore::update_event(const CalendarEvent& ev) {
  for (auto& e : events_) {
    if (e.id == ev.id) {
      e = ev;
      e.ics_path = events_dir_ + "/" + e.id + ".ics";
      save_all();
      return true;
    }
  }
  return false;
}

bool CalendarStore::delete_event(const std::string& id) {
  auto it = std::remove_if(events_.begin(), events_.end(), [&](const CalendarEvent& e) {
    return e.id == id;
  });
  if (it != events_.end()) {
    events_.erase(it, events_.end());
    delete_ics_file(id);
    save_index();
    return true;
  }
  return false;
}

void CalendarStore::ensure_category(const CalendarCategory& cat) {
  for (const auto& c : categories_) if (c.id == cat.id) return;
  categories_.push_back(cat);
}

void CalendarStore::toggle_category(const std::string& cat_id) {
  for (auto& c : categories_) {
    if (c.id == cat_id) {
      c.visible = !c.visible;
      break;
    }
  }
}

bool CalendarStore::is_category_visible(const std::string& cat_id) const {
  for (const auto& c : categories_) {
    if (c.id == cat_id) return c.visible;
  }
  return true;
}

std::vector<CalendarEvent> CalendarStore::search(const std::string& query) const {
  if (query.empty()) return all_events();
  std::string q = query;
  std::transform(q.begin(), q.end(), q.begin(), ::tolower);

  std::vector<CalendarEvent> res;
  for (const auto& ev : events_) {
    std::string text = ev.title + " " + ev.location + " " + ev.description;
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    if (text.find(q) != std::string::npos) {
      res.push_back(ev);
    }
  }
  return res;
}

std::string CalendarStore::export_ics(const std::string& id) const {
  const CalendarEvent* ev = get_event(id);
  if (!ev) return "";
  std::ifstream f(ev->ics_path);
  if (!f.is_open()) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string CalendarStore::export_all_ics() const {
  std::stringstream ss;
  ss << "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//AIOS//AetherCalendar 1.0//EN\r\n";
  for (const auto& ev : events_) ss << vevent_block(ev);
  ss << "END:VCALENDAR\r\n";
  return ss.str();
}

bool CalendarStore::import_ics_data(const std::string& ics_text) {
  std::stringstream ss(ics_text);
  std::string line;
  CalendarEvent ev;
  bool in_vevent = false;

  while (std::getline(ss, line)) {
    line = trim_str(line);
    if (line == "BEGIN:VEVENT") {
      ev = CalendarEvent();
      in_vevent = true;
    } else if (line == "END:VEVENT") {
      if (in_vevent && !ev.title.empty()) {
        add_event(ev);
      }
      in_vevent = false;
    } else if (in_vevent) {
      // Property name up to ':' (parameters after ';'), value after it.
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string head = line.substr(0, colon);
      const std::string name = head.substr(0, head.find(';'));
      const std::string val = line.substr(colon + 1);
      auto unescape = [](const std::string& v) {
        std::string o;
        for (size_t i = 0; i < v.size(); ++i) {
          if (v[i] == '\\' && i + 1 < v.size()) {
            ++i;
            o += (v[i] == 'n' || v[i] == 'N') ? '\n' : v[i];
          } else {
            o += v[i];
          }
        }
        return o;
      };
      // YYYYMMDD[THHMMSS[Z]]
      auto parse_dt = [](const std::string& v, int& y, int& mo, int& d, int& h, int& mi) {
        if (v.size() < 8) return false;
        y = parse_int(v.substr(0, 4)); mo = parse_int(v.substr(4, 2)); d = parse_int(v.substr(6, 2));
        h = mi = -1;
        if (v.size() >= 13 && v[8] == 'T') { h = parse_int(v.substr(9, 2)); mi = parse_int(v.substr(11, 2)); }
        return y > 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
      };
      if (name == "SUMMARY") {
        ev.title = unescape(val);
      } else if (name == "LOCATION") {
        ev.location = unescape(val);
      } else if (name == "DESCRIPTION") {
        ev.description = unescape(val);
      } else if (name == "CATEGORIES") {
        ev.calendar_id = unescape(val);
      } else if (name == "DTSTART") {
        int y, mo, d, h, mi;
        if (parse_dt(val, y, mo, d, h, mi)) {
          ev.year = y; ev.month = mo; ev.day = d;
          if (h < 0) { ev.all_day = true; ev.start_hour = 0; ev.start_min = 0; }
          else { ev.all_day = false; ev.start_hour = h; ev.start_min = mi; }
        }
      } else if (name == "DTEND") {
        int y, mo, d, h, mi;
        if (parse_dt(val, y, mo, d, h, mi) && h >= 0) {
          const long dur = (days_from_civil(y, mo, d) - days_from_civil(ev.year, ev.month, ev.day)) * 1440 +
                           (h * 60 + mi) - (ev.start_hour * 60 + ev.start_min);
          if (dur > 0) ev.duration_mins = (int)dur;
        }
      }
    }
  }
  return true;
}

std::string CalendarStore::status_line() const {
  return std::to_string(events_.size()) + " events • iCalendar RFC 5545 Store";
}

}  // namespace aios::userland
