// Two-way sync of calendar, contacts and notes with a Google account. See google_sync.hpp.
#include "aios/userland/google_sync.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#if !defined(AIOS_METAL)
#include <chrono>
#include <thread>
#endif

#include "aios/channel/http_client.hpp"
#include "aios/core/base64.hpp"
#include "aios/net/imap.hpp"
#include "aios/net/mime.hpp"

namespace aios::userland {

namespace {

// --- small things ----------------------------------------------------------------------------------

uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

std::string hex64(uint64_t v) {
  char b[20];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

std::string pad(int v, int w = 2) {
  char b[16];
  std::snprintf(b, sizeof b, "%0*d", w, v);
  return b;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

// The offset from UTC, in minutes, this machine keeps at `epoch`. A machine with no time zone database
// (bare metal) keeps UTC.
int local_offset_minutes(int64_t epoch) {
#if !defined(AIOS_METAL) && !defined(_WIN32)
  std::time_t tt = static_cast<std::time_t>(epoch);
  std::tm tm{};
  if (localtime_r(&tt, &tm)) return static_cast<int>(tm.tm_gmtoff / 60);
#else
  (void)epoch;
#endif
  return 0;
}

std::string offset_text(int minutes) {
  char sign = minutes < 0 ? '-' : '+';
  int a = minutes < 0 ? -minutes : minutes;
  return std::string(1, sign) + pad(a / 60) + ":" + pad(a % 60);
}

std::string path_escape(const std::string& s) {
  std::string o;
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

std::string safe_id(const std::string& s) {
  std::string o;
  for (char c : s) o += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
  return o;
}

// The unique-id header a note carries, so a note found again after the server renumbered its messages is
// the same note.
const char kNoteIdHeader[] = "X-AIOS-Note-ID";

}  // namespace

bool parse_rfc3339(const std::string& t, Rfc3339& o) {
  o = Rfc3339{};
  auto num = [&](size_t pos, size_t n, int& out) {
    if (pos + n > t.size()) return false;
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
      if (t[pos + i] < '0' || t[pos + i] > '9') return false;
      v = v * 10 + (t[pos + i] - '0');
    }
    out = v;
    return true;
  };
  if (t.size() < 10 || t[4] != '-' || t[7] != '-') return false;
  if (!num(0, 4, o.year) || !num(5, 2, o.month) || !num(8, 2, o.day)) return false;
  if (o.month < 1 || o.month > 12 || o.day < 1 || o.day > 31) return false;
  if (t.size() == 10) {
    o.date_only = true;
    o.epoch = days_from_civil(o.year, static_cast<unsigned>(o.month), static_cast<unsigned>(o.day)) * 86400;
    return true;
  }
  if ((t[10] != 'T' && t[10] != 't' && t[10] != ' ') || t.size() < 16 || t[13] != ':') return false;
  if (!num(11, 2, o.hour) || !num(14, 2, o.minute)) return false;
  size_t p = 16;
  if (p < t.size() && t[p] == ':') { if (!num(p + 1, 2, o.second)) return false; p += 3; }
  if (p < t.size() && t[p] == '.') { ++p; while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p; }
  int off = 0;
  if (p < t.size()) {
    if (t[p] == 'Z' || t[p] == 'z') { off = 0; }
    else if (t[p] == '+' || t[p] == '-') {
      int hh = 0, mm = 0;
      if (!num(p + 1, 2, hh)) return false;
      size_t q = p + 3;
      if (q < t.size() && t[q] == ':') ++q;
      if (!num(q, 2, mm)) mm = 0;
      off = (hh * 60 + mm) * (t[p] == '-' ? -1 : 1);
    } else return false;
  }
  o.offset_minutes = off;
  o.epoch = days_from_civil(o.year, static_cast<unsigned>(o.month), static_cast<unsigned>(o.day)) * 86400 + o.hour * 3600 +
            o.minute * 60 + o.second - static_cast<int64_t>(off) * 60;
  return true;
}

std::string format_rfc3339_utc(int64_t epoch) {
  int64_t days = epoch >= 0 ? epoch / 86400 : -((-epoch + 86399) / 86400);
  int64_t rem = epoch - days * 86400;
  int y, m, d;
  civil_from_days(days, y, m, d);
  return pad(y, 4) + "-" + pad(m) + "-" + pad(d) + "T" + pad(static_cast<int>(rem / 3600)) + ":" + pad(static_cast<int>(rem % 3600 / 60)) + ":" +
         pad(static_cast<int>(rem % 60)) + "Z";
}

std::string GoogleSyncReport::summary() const {
  std::string s = "pulled " + std::to_string(pulled_new) + " new, " + std::to_string(pulled_updated) + " updated, " + std::to_string(pulled_deleted) +
                  " deleted; pushed " + std::to_string(pushed_new) + " new, " + std::to_string(pushed_updated) + " updated, " +
                  std::to_string(pushed_deleted) + " deleted";
  if (conflicts) s += "; " + std::to_string(conflicts) + " conflict(s) resolved in Google's favour (your version is in google/conflicts/)";
  if (!problems.empty()) s += "; " + std::to_string(problems.size()) + " problem(s)";
  return s;
}

GoogleSync::GoogleSync(hal::Host& host, std::string root, std::string account, std::string trust_store)
    : host_(host), root_(std::move(root)), dir_(root_ + "/google"), account_(std::move(account)), trust_store_(std::move(trust_store)) {
  while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
  dir_ = root_ + "/google";
}

void GoogleSync::pause_ms(int ms) {
#if !defined(AIOS_METAL)
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#else
  const uint64_t end = host_.monotonic_ns() + static_cast<uint64_t>(ms) * 1000000ull;
  while (host_.monotonic_ns() < end) hal::compute_yield();
#endif
}

Result<GoogleSync::Endpoints> GoogleSync::endpoints() {
  Endpoints e;
  auto raw = host_.read_file(root_ + "/oauth/google.json");
  if (!raw.ok()) return e;   // the real Google
  auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
  if (!doc.ok()) return Result<Endpoints>(Status::Invalid, root_ + "/oauth/google.json: " + doc.error().message);
  const json::Value& src = (*doc)["installed"].is_object() ? (*doc)["installed"] : (*doc)["web"].is_object() ? (*doc)["web"] : *doc;
  if (src["calendar_api"].is_string()) e.calendar = src["calendar_api"].str();
  if (src["people_api"].is_string()) e.people = src["people_api"].str();
  if (src["imap_host"].is_string()) e.imap_host = src["imap_host"].str();
  if (src["imap_port"].is_number()) e.imap_port = static_cast<uint16_t>(src["imap_port"].i64());
  if (src["imap_security"].is_string()) e.imap_security = src["imap_security"].str();
  if (src["smtp_host"].is_string()) e.smtp_host = src["smtp_host"].str();
  if (src["smtp_port"].is_number()) e.smtp_port = static_cast<uint16_t>(src["smtp_port"].i64());
  if (src["smtp_security"].is_string()) e.smtp_security = src["smtp_security"].str();
  while (!e.calendar.empty() && e.calendar.back() == '/') e.calendar.pop_back();
  while (!e.people.empty() && e.people.back() == '/') e.people.pop_back();
  return e;
}

// --- one authorised HTTP call ----------------------------------------------------------------------

Result<GoogleSync::Api> GoogleSync::api(const std::string& method, const std::string& url, const json::Value* body,
                                        const std::vector<std::pair<std::string, std::string>>& headers) {
  OAuth oauth(host_, root_, trust_store_);
  auto ep = channel::parse_endpoint(url);
  if (!ep.ok()) return Result<Api>(ep.error());
  bool refreshed = false;
  std::string last;
  for (int attempt = 0; attempt < 5; ++attempt) {
    auto tok = oauth.access_token("google", account_);
    if (!tok.ok()) return Result<Api>(tok.error());
    channel::HttpRequest req;
    req.method = method;
    req.endpoint = *ep;
    req.headers.push_back({"Authorization", "Bearer " + *tok});
    req.headers.push_back({"Accept", "application/json"});
    for (const auto& h : headers) req.headers.push_back(h);
    if (body) {
      std::string text = body->dump();
      req.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
      req.body = ByteBuffer(text.begin(), text.end());
    }
    req.timeout_ms = 60000;
    req.host = &host_;
    req.kernel_root = root_;
    req.trust_store = trust_store_;
    auto r = channel::http_send(req);
    if (!r.ok()) {
      last = r.error().message;
      if (attempt < 3) { pause_ms(500 << attempt); continue; }
      return Result<Api>(r.error());
    }
    const int st = r->status;
    if (st == 401 && !refreshed) {
      // The token was refused although it looked live: make it stale and let access_token() mint a new one.
      refreshed = true;
      auto cur = oauth.load_token("google", account_);
      if (cur.ok()) { cur->expires_at = 0; (void)oauth.save_token(*cur); }
      continue;
    }
    if (st == 429 || st == 500 || st == 502 || st == 503 || st == 504) {
      last = "HTTP " + std::to_string(st);
      int wait = 1000 << attempt;
      std::string ra = r->header("Retry-After");
      if (!ra.empty()) { int s = std::atoi(ra.c_str()); if (s > 0) wait = std::min(s, 30) * 1000; }
      if (attempt < 4) { pause_ms(wait); continue; }
    }
    Api out;
    out.status = st;
    out.etag = r->header("ETag");
    std::string text = r->text();
    if (!text.empty()) {
      auto doc = json::parse(text);
      if (doc.ok()) out.body = std::move(*doc);
      else if (st >= 200 && st < 300) return Result<Api>(Status::Corrupt, url + " answered " + std::to_string(st) + " with something that is not JSON");
    }
    return out;
  }
  return Result<Api>(Status::IoError, "gave up on " + method + " " + url + " after retries (" + last + ")");
}

// --- state -----------------------------------------------------------------------------------------

Result<json::Value> GoogleSync::load_state() {
  if (state_loaded_) return state_;
  state_ = json::Value::object();
  const std::string path = dir_ + "/state.json";
  if (host_.exists(path)) {
    auto raw = host_.read_file(path);
    if (!raw.ok()) return Result<json::Value>(raw.error());
    auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
    if (!doc.ok()) return Result<json::Value>(Status::Corrupt, path + ": " + doc.error().message);
    state_ = std::move(*doc);
  }
  state_loaded_ = true;
  return state_;
}

Result<void> GoogleSync::save_state(const json::Value& st) {
  if (!host_.exists(dir_)) {
    auto m = host_.make_dir(dir_);
    if (!m.ok() && !host_.exists(dir_)) return m;
  }
  const std::string text = st.dump();
  const std::string tmp = dir_ + "/state.json.tmp";
  if (auto w = host_.write_file(tmp, ByteSpan(reinterpret_cast<const uint8_t*>(text.data()), text.size())); !w.ok()) return w;
  state_ = st;
  return host_.rename_file(tmp, dir_ + "/state.json");
}

void GoogleSync::keep_conflict(const std::string& kind, const std::string& id, const std::string& local_json) {
  const std::string d = dir_ + "/conflicts";
  if (!host_.exists(dir_)) (void)host_.make_dir(dir_);
  if (!host_.exists(d)) (void)host_.make_dir(d);
  const std::string path = d + "/" + kind + "-" + safe_id(id) + "-" + std::to_string(host_.wall_ns() / 1000000000ull) + ".json";
  (void)host_.write_file(path, ByteSpan(reinterpret_cast<const uint8_t*>(local_json.data()), local_json.size()));
}

namespace {

json::Value section_of(const json::Value& st, const std::string& account, const char* name) {
  const json::Value& s = st["accounts"][account][name];
  return s.is_object() ? s : json::Value::object();
}

json::Value with_section(const json::Value& st, const std::string& account, const char* name, json::Value sec) {
  json::Value accounts = st["accounts"].is_object() ? st["accounts"] : json::Value::object();
  json::Value acc = accounts[account].is_object() ? accounts[account] : json::Value::object();
  acc.set(name, std::move(sec));
  accounts.set(account, std::move(acc));
  json::Value out = st.is_object() ? st : json::Value::object();
  out.set("accounts", std::move(accounts));
  return out;
}

// ----- calendar mapping -------------------------------------------------------------------------------

struct CalEntry {
  std::string rid, cal, etag, hash, off;
  int64_t pushed_at = 0;
};

json::Value entry_json(const CalEntry& e) {
  json::Value o = json::Value::object();
  o.set("rid", json::Value::string(e.rid));
  o.set("cal", json::Value::string(e.cal));
  o.set("etag", json::Value::string(e.etag));
  o.set("hash", json::Value::string(e.hash));
  o.set("off", json::Value::string(e.off));
  return o;
}

CalEntry entry_from(const json::Value& o) {
  CalEntry e;
  e.rid = o["rid"].str();
  e.cal = o["cal"].str();
  e.etag = o["etag"].str();
  e.hash = o["hash"].str();
  e.off = o["off"].str();
  return e;
}

std::string event_hash(const CalendarEvent& e) {
  std::string s = e.title + "\x1f" + e.location + "\x1f" + e.description + "\x1f" + std::to_string(e.year) + "-" + std::to_string(e.month) + "-" +
                  std::to_string(e.day) + " " + std::to_string(e.start_hour) + ":" + std::to_string(e.start_min) + "+" +
                  std::to_string(e.duration_mins) + (e.all_day ? "D" : "T");
  for (const auto& a : e.attendees) s += "\x1f" + a;
  return hex64(fnv64(s));
}

std::string event_json(const CalendarEvent& e) {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(e.id));
  o.set("title", json::Value::string(e.title));
  o.set("location", json::Value::string(e.location));
  o.set("description", json::Value::string(e.description));
  o.set("date", json::Value::string(pad(e.year, 4) + "-" + pad(e.month) + "-" + pad(e.day)));
  o.set("time", json::Value::string(pad(e.start_hour) + ":" + pad(e.start_min)));
  o.set("duration_mins", json::Value::integer(e.duration_mins));
  o.set("all_day", json::Value::boolean(e.all_day));
  return o.dump();
}

// A Google event as a local one. Times are the wall-clock time the event is written in.
bool event_to_local(const json::Value& g, CalendarEvent& out, std::string& off_text) {
  const json::Value& st = g["start"];
  const json::Value& en = g["end"];
  Rfc3339 a, b;
  if (st["dateTime"].is_string()) {
    if (!parse_rfc3339(st["dateTime"].str(), a)) return false;
    out.all_day = false;
    out.year = a.year; out.month = a.month; out.day = a.day;
    out.start_hour = a.hour; out.start_min = a.minute;
    off_text = offset_text(a.offset_minutes);
    int dur = 60;
    if (en["dateTime"].is_string() && parse_rfc3339(en["dateTime"].str(), b)) dur = static_cast<int>((b.epoch - a.epoch) / 60);
    out.duration_mins = dur > 0 ? dur : 1;
  } else if (st["date"].is_string()) {
    if (!parse_rfc3339(st["date"].str(), a)) return false;
    out.all_day = true;
    out.year = a.year; out.month = a.month; out.day = a.day;
    out.start_hour = 0; out.start_min = 0;
    off_text.clear();
    int days = 1;
    if (en["date"].is_string() && parse_rfc3339(en["date"].str(), b)) days = static_cast<int>((b.epoch - a.epoch) / 86400);
    out.duration_mins = (days > 0 ? days : 1) * 1440;
  } else {
    return false;
  }
  out.title = g["summary"].is_string() && !g["summary"].str().empty() ? g["summary"].str() : std::string("(No title)");
  out.location = g["location"].str();
  out.description = g["description"].str();
  out.attendees.clear();
  for (const auto& at : g["attendees"].items())
    if (at["email"].is_string() && !at["email"].str().empty()) out.attendees.push_back(at["email"].str());
  out.recurrence = "NONE";
  return true;
}

// A local event as the body of an insert or patch. `off_text` keeps the zone a pulled event came in; a new
// local event takes this machine's.
json::Value event_to_google(const CalendarEvent& e, const std::string& off_text, bool include_attendees) {
  json::Value o = json::Value::object();
  o.set("summary", json::Value::string(e.title));
  o.set("description", json::Value::string(e.description));
  o.set("location", json::Value::string(e.location));
  json::Value st = json::Value::object(), en = json::Value::object();
  const int64_t day0 = days_from_civil(e.year, static_cast<unsigned>(e.month), static_cast<unsigned>(e.day));
  if (e.all_day) {
    int ey, em, ed;
    int64_t days = (e.duration_mins + 1439) / 1440;
    if (days < 1) days = 1;
    civil_from_days(day0 + days, ey, em, ed);
    st.set("date", json::Value::string(pad(e.year, 4) + "-" + pad(e.month) + "-" + pad(e.day)));
    en.set("date", json::Value::string(pad(ey, 4) + "-" + pad(em) + "-" + pad(ed)));
  } else {
    int off = 0;
    if (off_text.size() == 6) off = (std::atoi(off_text.substr(1, 2).c_str()) * 60 + std::atoi(off_text.substr(4, 2).c_str())) * (off_text[0] == '-' ? -1 : 1);
    else off = local_offset_minutes(day0 * 86400 + e.start_hour * 3600 + e.start_min * 60);
    const int64_t start_local = day0 * 86400 + e.start_hour * 3600 + e.start_min * 60;
    const int64_t end_local = start_local + static_cast<int64_t>(e.duration_mins) * 60;
    auto fmt = [&](int64_t local) {
      int64_t d = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
      int64_t r = local - d * 86400;
      int y, m, dd;
      civil_from_days(d, y, m, dd);
      return pad(y, 4) + "-" + pad(m) + "-" + pad(dd) + "T" + pad(static_cast<int>(r / 3600)) + ":" + pad(static_cast<int>(r % 3600 / 60)) + ":" +
             pad(static_cast<int>(r % 60)) + offset_text(off);
    };
    st.set("dateTime", json::Value::string(fmt(start_local)));
    en.set("dateTime", json::Value::string(fmt(end_local)));
  }
  o.set("start", std::move(st));
  o.set("end", std::move(en));
  if (include_attendees) {
    json::Value at = json::Value::array();
    for (const auto& a : e.attendees) {
      json::Value x = json::Value::object();
      x.set("email", json::Value::string(a));
      at.push(std::move(x));
    }
    o.set("attendees", std::move(at));
  }
  return o;
}

bool same_attendees(const std::vector<std::string>& local, const json::Value& remote) {
  std::set<std::string> a(local.begin(), local.end()), b;
  for (const auto& x : remote["attendees"].items()) if (x["email"].is_string()) b.insert(x["email"].str());
  return a == b;
}

}  // namespace

// --- calendar --------------------------------------------------------------------------------------

Result<GoogleSyncReport> GoogleSync::sync_calendar(CalendarStore& cal) {
  auto ep = endpoints();
  if (!ep.ok()) return Result<GoogleSyncReport>(ep.error());
  auto st = load_state();
  if (!st.ok()) return Result<GoogleSyncReport>(st.error());
  GoogleSyncReport rep;

  struct CalInfo { std::string id, name; bool primary = false, writable = false; uint32_t color = 0xFF0A84FF; };
  std::vector<CalInfo> cals;
  {
    auto r = api("GET", ep->calendar + "/users/me/calendarList?minAccessRole=reader&fields=items(id,summary,primary,accessRole,backgroundColor)");
    if (!r.ok()) return Result<GoogleSyncReport>(r.error());
    if (r->status != 200) return Result<GoogleSyncReport>(Status::IoError, "Google Calendar's calendar list answered " + std::to_string(r->status) + ": " + r->body["error"]["message"].str());
    for (const auto& it : r->body["items"].items()) {
      CalInfo c;
      c.id = it["id"].str();
      c.name = it["summary"].str();
      c.primary = it["primary"].boolean_or(false);
      const std::string role = it["accessRole"].str();
      c.writable = role == "owner" || role == "writer";
      const std::string bg = it["backgroundColor"].str();
      if (bg.size() == 7 && bg[0] == '#') c.color = 0xFF000000u | static_cast<uint32_t>(std::strtoul(bg.c_str() + 1, nullptr, 16));
      if (!c.id.empty()) cals.push_back(std::move(c));
    }
  }
  if (cals.empty()) return Result<GoogleSyncReport>(Status::NotFound, "this Google account has no calendars");
  std::string primary_id;
  for (const auto& c : cals) if (c.primary) primary_id = c.id;
  if (primary_id.empty()) for (const auto& c : cals) if (c.writable) { primary_id = c.id; break; }
  CalendarCategory cat;
  cat.id = "google";
  cat.name = "Google";
  cat.color = cals[0].color;
  cal.ensure_category(cat);

  const int64_t now = static_cast<int64_t>(host_.wall_ns() / 1000000000ull);
  const int64_t tmin = now - 30 * 86400, tmax = now + 365 * 86400;

  // Entries from the last sync.
  std::map<std::string, CalEntry> entries;   // local id -> entry
  const json::Value cal_section = section_of(*st, account_, "calendar");   // a named value: the loop below iterates into it
  for (const auto& kv : cal_section["events"].members()) entries[kv.first] = entry_from(kv.second);
  std::map<std::string, std::string> by_remote;   // cal|rid -> local id
  for (const auto& kv : entries) by_remote[kv.second.cal + "|" + kv.second.rid] = kv.first;

  // Pull every calendar's window. A calendar that cannot be listed is left alone, never read as empty.
  struct Remote { std::string cal, rid; json::Value ev; };
  std::vector<Remote> remote;
  std::set<std::string> listed;
  for (const auto& c : cals) {
    std::string page;
    bool ok = true;
    std::vector<Remote> mine;
    do {
      std::string url = ep->calendar + "/calendars/" + path_escape(c.id) +
                        "/events?singleEvents=true&orderBy=startTime&maxResults=250&showDeleted=false&timeMin=" + path_escape(format_rfc3339_utc(tmin)) +
                        "&timeMax=" + path_escape(format_rfc3339_utc(tmax)) +
                        "&fields=items(id,etag,status,summary,description,location,start,end,attendees(email)),nextPageToken";
      if (!page.empty()) url += "&pageToken=" + path_escape(page);
      auto r = api("GET", url);
      if (!r.ok() || r->status != 200) {
        rep.problems.push_back("calendar " + c.name + " could not be listed: " + (r.ok() ? "HTTP " + std::to_string(r->status) : r.error().message));
        ok = false;
        break;
      }
      for (const auto& it : r->body["items"].items()) {
        if (it["status"].str() == "cancelled" || !it["id"].is_string()) continue;
        mine.push_back({c.id, it["id"].str(), it});
      }
      page = r->body["nextPageToken"].str();
    } while (!page.empty());
    if (ok) {
      listed.insert(c.id);
      for (auto& m : mine) remote.push_back(std::move(m));
    }
  }

  std::set<std::string> seen_local;
  for (const auto& r : remote) {
    const std::string key = r.cal + "|" + r.rid;
    const std::string etag = r.ev["etag"].str();
    auto bi = by_remote.find(key);
    std::string local_id = bi != by_remote.end() ? bi->second : "g-" + safe_id(r.rid);
    const CalendarEvent* existing = cal.get_event(local_id);
    CalendarEvent fresh;
    std::string off;
    fresh.id = local_id;
    fresh.calendar_id = "google";
    for (const auto& c : cals) if (c.id == r.cal) fresh.color = c.color;
    if (!event_to_local(r.ev, fresh, off)) { rep.problems.push_back("event " + r.rid + " has no usable start time"); continue; }

    if (bi == by_remote.end()) {
      if (existing) {
        // The state was lost but the event is here: adopt it rather than duplicate it.
        CalEntry e{r.rid, r.cal, etag, event_hash(*existing), off, 0};
        entries[local_id] = e;
        by_remote[key] = local_id;
        seen_local.insert(local_id);
        continue;
      }
      CalendarEvent add = fresh;
      cal.add_event(add);
      entries[local_id] = CalEntry{r.rid, r.cal, etag, event_hash(add), off, 0};
      by_remote[key] = local_id;
      seen_local.insert(local_id);
      rep.pulled_new++;
      continue;
    }
    seen_local.insert(local_id);
    CalEntry& en = entries[local_id];
    if (!existing) {
      // Deleted here since the last sync: delete it there.
      auto d = api("DELETE", ep->calendar + "/calendars/" + path_escape(r.cal) + "/events/" + path_escape(r.rid) + "?sendUpdates=none");
      if (d.ok() && (d->status == 204 || d->status == 200 || d->status == 404 || d->status == 410)) { rep.pushed_deleted++; entries.erase(local_id); by_remote.erase(key); }
      else rep.problems.push_back("could not delete event " + r.rid + ": " + (d.ok() ? "HTTP " + std::to_string(d->status) : d.error().message));
      continue;
    }
    const bool local_changed = event_hash(*existing) != en.hash;
    const bool remote_changed = etag != en.etag;
    if (!local_changed && !remote_changed) continue;
    if (remote_changed && (!local_changed || true)) {
      if (local_changed) { keep_conflict("calendar", local_id, event_json(*existing)); rep.conflicts++; }
      CalendarEvent upd = fresh;
      upd.color = existing->color;
      cal.update_event(upd);
      en.etag = etag;
      en.hash = event_hash(upd);
      en.off = off;
      rep.pulled_updated++;
      continue;
    }
    // Local change only: push it, and only if nobody changed it there meanwhile (If-Match).
    json::Value body = event_to_google(*existing, en.off, !same_attendees(existing->attendees, r.ev));
    auto p = api("PATCH", ep->calendar + "/calendars/" + path_escape(r.cal) + "/events/" + path_escape(r.rid) + "?sendUpdates=none", &body, {{"If-Match", en.etag}});
    if (p.ok() && p->status == 200) {
      en.etag = p->body["etag"].str();
      en.hash = event_hash(*existing);
      rep.pushed_updated++;
    } else if (p.ok() && p->status == 412) {
      keep_conflict("calendar", local_id, event_json(*existing));
      rep.conflicts++;   // changed there while we looked: the next sync pulls theirs
    } else {
      rep.problems.push_back("could not update event " + r.rid + ": " + (p.ok() ? "HTTP " + std::to_string(p->status) + " " + p->body["error"]["message"].str() : p.error().message));
    }
  }

  // Entries whose event is no longer in a window we could read: deleted over there, if it falls in the window.
  std::vector<std::string> gone;
  for (const auto& kv : entries) {
    if (seen_local.count(kv.first) || !listed.count(kv.second.cal)) continue;
    const CalendarEvent* ev = cal.get_event(kv.first);
    if (!ev) { gone.push_back(kv.first); continue; }   // gone on both sides
    const int64_t when = days_from_civil(ev->year, static_cast<unsigned>(ev->month), static_cast<unsigned>(ev->day)) * 86400;
    if (when < tmin || when > tmax) continue;           // outside what we read: says nothing
    if (event_hash(*ev) != kv.second.hash) {
      // Edited here, deleted there: keep the edit by putting it back.
      keep_conflict("calendar", kv.first, event_json(*ev));
      rep.conflicts++;
      json::Value body = event_to_google(*ev, kv.second.off, true);
      auto p = api("POST", ep->calendar + "/calendars/" + path_escape(kv.second.cal) + "/events?sendUpdates=none", &body);
      if (p.ok() && p->status == 200) {
        CalEntry e = kv.second;
        e.rid = p->body["id"].str();
        e.etag = p->body["etag"].str();
        e.hash = event_hash(*ev);
        entries[kv.first] = e;
        rep.pushed_new++;
      } else rep.problems.push_back("could not restore event " + kv.first);
      continue;
    }
    cal.delete_event(kv.first);
    gone.push_back(kv.first);
    rep.pulled_deleted++;
  }
  for (const auto& id : gone) entries.erase(id);

  // Local events that were never sent: new here.
  for (const auto& ev : cal.all_events()) {
    if (entries.count(ev.id)) continue;
    if (ev.id.rfind("g-", 0) == 0) continue;   // a pulled event whose entry is being rebuilt above
    if (primary_id.empty()) break;
    bool writable_primary = false;
    for (const auto& c : cals) if (c.id == primary_id && c.writable) writable_primary = true;
    if (!writable_primary) { rep.problems.push_back("no writable calendar to send new events to"); break; }
    json::Value body = event_to_google(ev, "", true);
    auto p = api("POST", ep->calendar + "/calendars/" + path_escape(primary_id) + "/events?sendUpdates=none", &body);
    if (p.ok() && p->status == 200) {
      CalEntry e;
      e.rid = p->body["id"].str();
      e.cal = primary_id;
      e.etag = p->body["etag"].str();
      e.hash = event_hash(ev);
      entries[ev.id] = e;
      rep.pushed_new++;
    } else {
      rep.problems.push_back("could not send event " + ev.id + ": " + (p.ok() ? "HTTP " + std::to_string(p->status) + " " + p->body["error"]["message"].str() : p.error().message));
    }
  }

  json::Value ev_obj = json::Value::object();
  for (const auto& kv : entries) ev_obj.set(kv.first, entry_json(kv.second));
  json::Value sec = json::Value::object();
  sec.set("events", std::move(ev_obj));
  sec.set("last_sync", json::Value::integer(now));
  if (auto sv = save_state(with_section(*st, account_, "calendar", std::move(sec))); !sv.ok()) return Result<GoogleSyncReport>(sv.error());
  cal.save_all();
  return rep;
}

// --- contacts --------------------------------------------------------------------------------------

namespace {

struct PersonEntry {
  std::string rn, etag, hash, rhash, prev_rhash;
  int64_t pushed_at = 0;
};

std::string card_hash(const ContactCard& c) {
  std::string s;
  for (const std::string* f : {&c.first_name, &c.last_name, &c.company, &c.department, &c.job_title, &c.email, &c.email_work, &c.phone,
                               &c.phone_work, &c.address, &c.city, &c.state, &c.zip, &c.country, &c.notes, &c.birthday}) {
    s += *f;
    s += '\x1f';
  }
  return hex64(fnv64(s));
}

std::string card_json(const ContactCard& c) {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(c.id));
  o.set("name", json::Value::string(c.full_name()));
  o.set("email", json::Value::string(c.email));
  o.set("phone", json::Value::string(c.phone));
  o.set("company", json::Value::string(c.company));
  return o.dump();
}

bool is_work(const std::string& type) { return type == "work"; }

ContactCard card_from_person(const json::Value& p) {
  ContactCard c;
  const json::Value& n = p["names"][static_cast<size_t>(0)];
  c.first_name = n["givenName"].str();
  c.last_name = n["familyName"].str();
  if (c.first_name.empty() && c.last_name.empty()) c.first_name = n["displayName"].str();
  const json::Value& o = p["organizations"][static_cast<size_t>(0)];
  c.company = o["name"].str();
  c.department = o["department"].str();
  c.job_title = o["title"].str();
  for (const auto& e : p["emailAddresses"].items()) {
    const std::string v = e["value"].str();
    if (v.empty()) continue;
    if (is_work(e["type"].str())) { if (c.email_work.empty()) c.email_work = v; }
    else if (c.email.empty()) c.email = v;
  }
  for (const auto& e : p["phoneNumbers"].items()) {
    const std::string v = e["value"].str();
    if (v.empty()) continue;
    if (is_work(e["type"].str())) { if (c.phone_work.empty()) c.phone_work = v; }
    else if (c.phone.empty()) c.phone = v;
  }
  const json::Value& a = p["addresses"][static_cast<size_t>(0)];
  c.address = a["streetAddress"].str();
  c.city = a["city"].str();
  c.state = a["region"].str();
  c.zip = a["postalCode"].str();
  c.country = a["country"].str();
  const json::Value& b = p["birthdays"][static_cast<size_t>(0)]["date"];
  if (b["month"].is_number() && b["day"].is_number())
    c.birthday = (b["year"].is_number() ? pad(static_cast<int>(b["year"].i64()), 4) : std::string("----")) + "-" + pad(static_cast<int>(b["month"].i64())) + "-" + pad(static_cast<int>(b["day"].i64()));
  c.notes = p["biographies"][static_cast<size_t>(0)]["value"].str();
  return c;
}

// The values AIOS holds, written into a person without disturbing what it does not hold: an update
// replaces only the slots it owns (the first personal and first work email, the first personal and first
// work phone, the first name, organisation, address, birthday and biography).
json::Value person_from_card(const ContactCard& c, const json::Value& base) {
  json::Value p = json::Value::object();
  auto slot_list = [&](const char* key, const std::string& personal, const std::string& work) {
    json::Value out = json::Value::array();
    bool done_personal = false, done_work = false;
    for (const auto& e : base[key].items()) {
      const bool w = is_work(e["type"].str());
      if (w && !done_work) {
        done_work = true;
        if (!work.empty()) { json::Value x = e; x.set("value", json::Value::string(work)); out.push(std::move(x)); }
      } else if (!w && !done_personal) {
        done_personal = true;
        if (!personal.empty()) { json::Value x = e; x.set("value", json::Value::string(personal)); out.push(std::move(x)); }
      } else {
        out.push(e);
      }
    }
    if (!done_personal && !personal.empty()) { json::Value x = json::Value::object(); x.set("value", json::Value::string(personal)); x.set("type", json::Value::string("home")); out.push(std::move(x)); }
    if (!done_work && !work.empty()) { json::Value x = json::Value::object(); x.set("value", json::Value::string(work)); x.set("type", json::Value::string("work")); out.push(std::move(x)); }
    return out;
  };
  {
    json::Value names = json::Value::array();
    json::Value n = base["names"][static_cast<size_t>(0)].is_object() ? base["names"][static_cast<size_t>(0)] : json::Value::object();
    n.erase("displayName"); n.erase("displayNameLastFirst"); n.erase("unstructuredName");
    n.set("givenName", json::Value::string(c.first_name));
    n.set("familyName", json::Value::string(c.last_name));
    names.push(std::move(n));
    for (size_t i = 1; i < base["names"].size(); ++i) names.push(base["names"][i]);
    p.set("names", std::move(names));
  }
  p.set("emailAddresses", slot_list("emailAddresses", c.email, c.email_work));
  p.set("phoneNumbers", slot_list("phoneNumbers", c.phone, c.phone_work));
  {
    json::Value orgs = json::Value::array();
    if (!c.company.empty() || !c.department.empty() || !c.job_title.empty()) {
      json::Value o = base["organizations"][static_cast<size_t>(0)].is_object() ? base["organizations"][static_cast<size_t>(0)] : json::Value::object();
      o.set("name", json::Value::string(c.company));
      o.set("department", json::Value::string(c.department));
      o.set("title", json::Value::string(c.job_title));
      orgs.push(std::move(o));
    }
    for (size_t i = 1; i < base["organizations"].size(); ++i) orgs.push(base["organizations"][i]);
    p.set("organizations", std::move(orgs));
  }
  {
    json::Value addrs = json::Value::array();
    if (!c.address.empty() || !c.city.empty() || !c.state.empty() || !c.zip.empty() || !c.country.empty()) {
      json::Value a = base["addresses"][static_cast<size_t>(0)].is_object() ? base["addresses"][static_cast<size_t>(0)] : json::Value::object();
      a.erase("formattedValue");
      a.set("streetAddress", json::Value::string(c.address));
      a.set("city", json::Value::string(c.city));
      a.set("region", json::Value::string(c.state));
      a.set("postalCode", json::Value::string(c.zip));
      a.set("country", json::Value::string(c.country));
      addrs.push(std::move(a));
    }
    for (size_t i = 1; i < base["addresses"].size(); ++i) addrs.push(base["addresses"][i]);
    p.set("addresses", std::move(addrs));
  }
  {
    json::Value bd = json::Value::array();
    int y = 0, m = 0, d = 0;
    if (c.birthday.size() == 10 && c.birthday[4] == '-' && c.birthday[7] == '-') {
      y = c.birthday[0] == '-' ? 0 : std::atoi(c.birthday.substr(0, 4).c_str());
      m = std::atoi(c.birthday.substr(5, 2).c_str());
      d = std::atoi(c.birthday.substr(8, 2).c_str());
    }
    if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
      json::Value b = base["birthdays"][static_cast<size_t>(0)].is_object() ? base["birthdays"][static_cast<size_t>(0)] : json::Value::object();
      b.erase("text");
      json::Value date = json::Value::object();
      if (y) date.set("year", json::Value::integer(y));
      date.set("month", json::Value::integer(m));
      date.set("day", json::Value::integer(d));
      b.set("date", std::move(date));
      bd.push(std::move(b));
    }
    for (size_t i = 1; i < base["birthdays"].size(); ++i) bd.push(base["birthdays"][i]);
    p.set("birthdays", std::move(bd));
  }
  {
    json::Value bio = json::Value::array();
    if (!c.notes.empty()) {
      json::Value b = base["biographies"][static_cast<size_t>(0)].is_object() ? base["biographies"][static_cast<size_t>(0)] : json::Value::object();
      b.set("value", json::Value::string(c.notes));
      b.set("contentType", json::Value::string("TEXT_PLAIN"));
      bio.push(std::move(b));
    }
    for (size_t i = 1; i < base["biographies"].size(); ++i) bio.push(base["biographies"][i]);
    p.set("biographies", std::move(bio));
  }
  return p;
}

const char kPersonFields[] = "names,emailAddresses,phoneNumbers,organizations,addresses,birthdays,biographies";

}  // namespace

Result<GoogleSyncReport> GoogleSync::sync_contacts(ContactsStore& store) {
  auto ep = endpoints();
  if (!ep.ok()) return Result<GoogleSyncReport>(ep.error());
  auto st = load_state();
  if (!st.ok()) return Result<GoogleSyncReport>(st.error());
  GoogleSyncReport rep;
  const int64_t now = static_cast<int64_t>(host_.wall_ns() / 1000000000ull);

  std::map<std::string, PersonEntry> entries;   // local id -> entry
  const json::Value people_section = section_of(*st, account_, "contacts");
  for (const auto& kv : people_section["people"].members()) {
    PersonEntry e;
    e.rn = kv.second["rn"].str();
    e.etag = kv.second["etag"].str();
    e.hash = kv.second["hash"].str();
    e.rhash = kv.second["rhash"].str();
    e.prev_rhash = kv.second["prev_rhash"].str();
    e.pushed_at = kv.second["pushed_at"].i64();
    entries[kv.first] = e;
  }
  std::map<std::string, std::string> by_rn;
  for (const auto& kv : entries) by_rn[kv.second.rn] = kv.first;

  // Pull every contact.
  std::vector<json::Value> people;
  {
    std::string page;
    do {
      std::string url = ep->people + "/people/me/connections?pageSize=1000&personFields=" + path_escape(std::string(kPersonFields) + ",metadata");
      if (!page.empty()) url += "&pageToken=" + path_escape(page);
      auto r = api("GET", url);
      if (!r.ok()) return Result<GoogleSyncReport>(r.error());
      if (r->status != 200) return Result<GoogleSyncReport>(Status::IoError, "the People API answered " + std::to_string(r->status) + ": " + r->body["error"]["message"].str());
      for (const auto& p : r->body["connections"].items())
        if (p["resourceName"].is_string() && !p["metadata"]["deleted"].boolean_or(false)) people.push_back(p);
      page = r->body["nextPageToken"].str();
    } while (!page.empty());
  }

  std::set<std::string> seen;
  for (const auto& p : people) {
    const std::string rn = p["resourceName"].str();
    const std::string etag = p["etag"].str();
    ContactCard remote_card = card_from_person(p);
    const std::string rhash = card_hash(remote_card);
    auto bi = by_rn.find(rn);
    std::string local_id = bi != by_rn.end() ? bi->second : "g-" + safe_id(rn.substr(rn.find('/') == std::string::npos ? 0 : rn.find('/') + 1));
    seen.insert(local_id);
    const ContactCard* existing = store.get_contact(local_id);

    if (bi == by_rn.end()) {
      PersonEntry e;
      e.rn = rn; e.etag = etag; e.rhash = rhash;
      if (existing) { e.hash = card_hash(*existing); entries[local_id] = e; by_rn[rn] = local_id; continue; }
      remote_card.id = local_id;
      store.add_contact(remote_card);
      e.hash = card_hash(remote_card);
      entries[local_id] = e;
      by_rn[rn] = local_id;
      rep.pulled_new++;
      continue;
    }
    PersonEntry& en = entries[local_id];
    if (!existing) {
      auto d = api("DELETE", ep->people + "/" + rn + ":deleteContact");
      if (d.ok() && (d->status == 200 || d->status == 204 || d->status == 404)) { rep.pushed_deleted++; entries.erase(local_id); by_rn.erase(rn); }
      else rep.problems.push_back("could not delete contact " + rn + ": " + (d.ok() ? "HTTP " + std::to_string(d->status) : d.error().message));
      continue;
    }
    const bool local_changed = card_hash(*existing) != en.hash;
    bool remote_changed = rhash != en.rhash;
    if (etag != en.etag && !remote_changed) { en.etag = etag; }   // only metadata moved
    // The People API can serve a read from before our own write for a few minutes: the old content and
    // the old etag. That is not somebody editing it there.
    if (remote_changed && en.pushed_at && now - en.pushed_at < 600 && rhash == en.prev_rhash) remote_changed = false;
    if (!local_changed && !remote_changed) continue;
    if (remote_changed) {
      if (local_changed) { keep_conflict("contact", local_id, card_json(*existing)); rep.conflicts++; }
      ContactCard upd = remote_card;
      upd.id = local_id;
      upd.is_favorite = existing->is_favorite;
      upd.tags = existing->tags;
      upd.avatar_color = existing->avatar_color;
      store.update_contact(upd);
      en.etag = etag; en.rhash = rhash; en.hash = card_hash(upd); en.pushed_at = 0;
      rep.pulled_updated++;
      continue;
    }
    // Local change only.
    json::Value body = person_from_card(*existing, p);
    body.set("etag", json::Value::string(en.etag));
    auto u = api("PATCH", ep->people + "/" + rn + ":updateContact?updatePersonFields=" + path_escape(kPersonFields), &body);
    if (u.ok() && u->status == 200) {
      en.prev_rhash = en.rhash;
      en.etag = u->body["etag"].str();
      en.hash = card_hash(*existing);
      en.rhash = en.hash;   // what is there now is what we sent
      en.pushed_at = now;
      rep.pushed_updated++;
    } else if (u.ok() && (u->status == 400 || u->status == 409 || u->status == 412)) {
      keep_conflict("contact", local_id, card_json(*existing));
      rep.conflicts++;
    } else {
      rep.problems.push_back("could not update contact " + rn + ": " + (u.ok() ? "HTTP " + std::to_string(u->status) + " " + u->body["error"]["message"].str() : u.error().message));
    }
  }

  // Contacts we knew that are no longer in the list: deleted there.
  std::vector<std::string> gone;
  for (const auto& kv : entries) {
    if (seen.count(kv.first)) continue;
    const ContactCard* c = store.get_contact(kv.first);
    if (!c) { gone.push_back(kv.first); continue; }
    if (card_hash(*c) != kv.second.hash) {
      keep_conflict("contact", kv.first, card_json(*c));
      rep.conflicts++;
      json::Value body = person_from_card(*c, json::Value::object());
      auto cr = api("POST", ep->people + "/people:createContact?personFields=" + path_escape(std::string(kPersonFields)), &body);
      if (cr.ok() && cr->status == 200) {
        PersonEntry e = kv.second;
        e.rn = cr->body["resourceName"].str();
        e.etag = cr->body["etag"].str();
        e.hash = card_hash(*c);
        e.rhash = e.hash;
        entries[kv.first] = e;
        rep.pushed_new++;
      } else rep.problems.push_back("could not restore contact " + kv.first);
      continue;
    }
    store.delete_contact(kv.first);
    gone.push_back(kv.first);
    rep.pulled_deleted++;
  }
  for (const auto& id : gone) entries.erase(id);

  // Contacts that were never sent.
  for (const auto& c : store.all_contacts()) {
    if (entries.count(c.id) || c.id.rfind("g-", 0) == 0) continue;
    json::Value body = person_from_card(c, json::Value::object());
    auto cr = api("POST", ep->people + "/people:createContact?personFields=" + path_escape(std::string(kPersonFields)), &body);
    if (cr.ok() && cr->status == 200) {
      PersonEntry e;
      e.rn = cr->body["resourceName"].str();
      e.etag = cr->body["etag"].str();
      e.hash = card_hash(c);
      e.rhash = e.hash;
      entries[c.id] = e;
      rep.pushed_new++;
    } else {
      rep.problems.push_back("could not send contact " + c.id + ": " + (cr.ok() ? "HTTP " + std::to_string(cr->status) + " " + cr->body["error"]["message"].str() : cr.error().message));
    }
  }

  json::Value people_obj = json::Value::object();
  for (const auto& kv : entries) {
    json::Value o = json::Value::object();
    o.set("rn", json::Value::string(kv.second.rn));
    o.set("etag", json::Value::string(kv.second.etag));
    o.set("hash", json::Value::string(kv.second.hash));
    o.set("rhash", json::Value::string(kv.second.rhash));
    o.set("prev_rhash", json::Value::string(kv.second.prev_rhash));
    o.set("pushed_at", json::Value::integer(kv.second.pushed_at));
    people_obj.set(kv.first, std::move(o));
  }
  json::Value sec = json::Value::object();
  sec.set("people", std::move(people_obj));
  sec.set("last_sync", json::Value::integer(now));
  if (auto sv = save_state(with_section(*st, account_, "contacts", std::move(sec))); !sv.ok()) return Result<GoogleSyncReport>(sv.error());
  store.save_all();
  return rep;
}

// --- notes -----------------------------------------------------------------------------------------

namespace {

std::string html_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

// The text of a note kept as html by another client: a line per <div>, <br>, <p> or <li>.
std::string note_text_from_html(const std::string& html) {
  std::string with_breaks;
  size_t i = 0;
  while (i < html.size()) {
    if (html[i] == '<') {
      size_t gt = html.find('>', i);
      if (gt == std::string::npos) break;
      std::string tag = html.substr(i + 1, gt - i - 1);
      std::string low;
      for (char c : tag) low += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
      if (low.compare(0, 2, "br") == 0 || low.compare(0, 4, "/div") == 0 || low.compare(0, 2, "/p") == 0 || low.compare(0, 3, "/li") == 0 ||
          low.compare(0, 2, "/h") == 0)
        with_breaks += '\n';
      with_breaks += '<' + tag + '>';
      i = gt + 1;
      continue;
    }
    with_breaks += html[i++];
  }
  std::string text = net::mime::html_to_text(with_breaks);
  // html_to_text trims the ends only; squeeze runs of blank lines to one.
  std::string out;
  int nl = 0;
  for (char c : text) {
    if (c == '\n') { if (++nl <= 2) out += c; }
    else { nl = 0; out += c; }
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

struct NoteRec {
  std::string id, title, cat, body;
  bool deleted = false;
};

std::string note_hash(const NoteRec& n) { return hex64(fnv64(n.title + "\x1f" + n.body + "\x1f" + (n.deleted ? "1" : "0"))); }

// The newest record of each note id.
std::map<std::string, NoteRec> latest_notes(const std::string& list_json) {
  std::map<std::string, NoteRec> out;
  auto doc = json::parse(list_json);
  if (!doc.ok()) return out;
  // store_list returns newest first
  for (const auto& r : doc->items()) {
    const std::string id = r["id"].str();
    if (id.empty() || out.count(id)) continue;
    NoteRec n;
    n.id = id;
    n.title = r["title"].str();
    n.cat = r["cat"].str();
    n.body = r["body"].is_string() ? r["body"].str() : r["text"].str();
    n.deleted = r["deleted"].boolean_or(false);
    out[id] = std::move(n);
  }
  return out;
}

// A message is CRLF text; a note is LF text.
std::string crlf(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
    if (s[i] == '\n') o += "\r\n"; else o += s[i];
  }
  return o;
}

std::string lf(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
    o += s[i];
  }
  return o;
}

std::string note_message(const NoteRec& n, const std::string& created) {
  const std::string boundary = "aios-note-" + hex64(fnv64(n.id + n.body));
  std::string html = "<html><head></head><body style=\"word-wrap: break-word;\"><div>" + html_escape(n.title) + "</div>";
  size_t pos = 0;
  while (pos <= n.body.size()) {
    size_t nl = n.body.find('\n', pos);
    std::string line = n.body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    html += line.empty() ? "<div><br></div>" : "<div>" + html_escape(line) + "</div>";
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  html += "</body></html>";
  std::string subject = n.title.empty() ? std::string("Note") : n.title;
  for (auto& c : subject) if (c == '\r' || c == '\n') c = ' ';
  bool ascii = true;
  for (unsigned char c : subject) if (c >= 0x80) ascii = false;
  std::string subj_hdr = subject;
  if (!ascii) subj_hdr = "=?UTF-8?B?" + aios::base64_encode(aios::ByteSpan(reinterpret_cast<const uint8_t*>(subject.data()), subject.size())) + "?=";
  std::string m;
  m += "Subject: " + subj_hdr + "\r\n";
  m += "Date: " + created + "\r\n";
  m += "Message-ID: <" + safe_id(n.id) + "." + hex64(fnv64(n.title + n.body)) + "@aios.notes>\r\n";
  m += "MIME-Version: 1.0\r\n";
  m += "X-Uniform-Type-Identifier: com.apple.mail-note\r\n";
  m += std::string(kNoteIdHeader) + ": " + n.id + "\r\n";
  if (!n.cat.empty()) m += "X-AIOS-Note-Cat: " + n.cat + "\r\n";
  m += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";
  m += "--" + boundary + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n" + crlf(n.title + "\n" + n.body) + "\r\n";
  m += "--" + boundary + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n" + html + "\r\n";
  m += "--" + boundary + "--\r\n";
  return m;
}

}  // namespace

Result<GoogleSyncReport> GoogleSync::sync_notes(NoteStore& notes) {
  auto ep = endpoints();
  if (!ep.ok()) return Result<GoogleSyncReport>(ep.error());
  auto st = load_state();
  if (!st.ok()) return Result<GoogleSyncReport>(st.error());
  if (!notes.list || !notes.put) return Result<GoogleSyncReport>(Status::Invalid, "no note store to sync");
  GoogleSyncReport rep;
  const int64_t now = static_cast<int64_t>(host_.wall_ns() / 1000000000ull);

  OAuth oauth(host_, root_, trust_store_);
  auto tok = oauth.access_token("google", account_);
  if (!tok.ok()) return Result<GoogleSyncReport>(tok.error());
  net::Security sec = ep->imap_security == "none" ? net::Security::None : ep->imap_security == "starttls" ? net::Security::StartTls : net::Security::Tls;
  net::ImapClient imap(host_, root_, 30000, trust_store_);
  if (auto c = imap.connect(ep->imap_host, ep->imap_port, sec); !c.ok()) return Result<GoogleSyncReport>(c.error());
  if (auto a = imap.authenticate_xoauth2(account_, *tok); !a.ok()) return Result<GoogleSyncReport>(a.error());

  const std::string box = "Notes";
  auto sel = imap.select(box, false);
  if (!sel.ok()) {
    if (auto cr = imap.create_mailbox(box); !cr.ok()) return Result<GoogleSyncReport>(cr.error());
    sel = imap.select(box, false);
    if (!sel.ok()) return Result<GoogleSyncReport>(sel.error());
  }
  std::string trash;   // where deleted notes go, so they do not linger in All Mail (Gmail)
  if (auto lst = imap.list(); lst.ok())
    for (const auto& b : *lst) if (b.has_attr("\\Trash")) trash = b.name;

  struct NEntry { uint32_t uid = 0; std::string hash; };
  std::map<std::string, NEntry> entries;
  const json::Value prev = section_of(*st, account_, "notes");
  const bool same_validity = prev["uidvalidity"].i64() == static_cast<int64_t>(sel->uidvalidity);
  if (same_validity)
    for (const auto& kv : prev["notes"].members()) entries[kv.first] = NEntry{static_cast<uint32_t>(kv.second["uid"].i64()), kv.second["hash"].str()};

  // The server's notes: every message in the mailbox, its note id (ours, or made from its Message-ID).
  struct Remote { uint32_t uid = 0; std::string id, title, body, cat; bool has_body = false; };
  std::map<std::string, Remote> remote;   // by note id
  auto uids = imap.uid_search("ALL");
  if (!uids.ok()) return Result<GoogleSyncReport>(uids.error());
  std::set<uint32_t> known_uids;
  for (const auto& kv : entries) known_uids.insert(kv.second.uid);
  for (size_t i = 0; i < uids->size(); i += 100) {
    std::string set;
    for (size_t k = i; k < uids->size() && k < i + 100; ++k) { if (!set.empty()) set += ','; set += std::to_string((*uids)[k]); }
    auto got = imap.uid_fetch(set, "(UID BODY.PEEK[HEADER.FIELDS (SUBJECT MESSAGE-ID X-AIOS-NOTE-ID X-AIOS-NOTE-CAT)])");
    if (!got.ok()) return Result<GoogleSyncReport>(got.error());
    for (const auto& f : *got) {
      const std::string* hdr = nullptr;
      for (const auto& kv : f.sections) if (kv.first.compare(0, 11, "BODY[HEADER") == 0) hdr = &kv.second;
      if (!hdr || !f.uid) continue;
      net::mime::Parsed p = net::mime::parse(*hdr);
      Remote r;
      r.uid = f.uid;
      r.id = p.header(kNoteIdHeader);
      if (r.id.empty()) {
        const std::string mid = p.header("Message-ID");
        r.id = "NOTE-M" + hex64(fnv64(mid.empty() ? "uid" + std::to_string(f.uid) : mid)).substr(0, 10);
      }
      r.title = p.decoded_header("Subject");
      r.cat = p.header("X-AIOS-Note-Cat");
      auto prior = remote.find(r.id);
      if (prior != remote.end() && prior->second.uid > r.uid) continue;   // two copies: the newer one wins
      remote[r.id] = std::move(r);
    }
  }
  // Bodies of the messages we have not read yet (or whose uid moved).
  auto fetch_body = [&](Remote& r) -> bool {
    auto got = imap.uid_fetch(std::to_string(r.uid), "(UID BODY.PEEK[])");
    if (!got.ok() || got->empty()) return false;
    const std::string* raw = nullptr;
    for (const auto& kv : (*got)[0].sections) if (kv.first == "BODY[]") raw = &kv.second;
    if (!raw) return false;
    net::mime::Parsed p = net::mime::parse(*raw);
    p.text = lf(p.text);
    const bool ours = !p.header(kNoteIdHeader).empty();
    if (ours && !p.text.empty()) {
      // our own format: text/plain is "title\nbody"
      size_t nl = p.text.find('\n');
      r.title = nl == std::string::npos ? p.text : p.text.substr(0, nl);
      r.body = nl == std::string::npos ? std::string() : p.text.substr(nl + 1);
      while (!r.body.empty() && (r.body.back() == '\n' || r.body.back() == '\r')) r.body.pop_back();
      while (!r.title.empty() && r.title.back() == '\r') r.title.pop_back();
    } else {
      std::string text = !p.html.empty() ? note_text_from_html(p.html) : p.text;
      // another client's note: the subject is the title; the first line of the text repeats it
      std::string body = text;
      size_t nl = body.find('\n');
      std::string first = nl == std::string::npos ? body : body.substr(0, nl);
      if (!r.title.empty() && first == r.title) body = nl == std::string::npos ? std::string() : body.substr(nl + 1);
      while (!body.empty() && body.front() == '\n') body.erase(0, 1);
      r.body = body;
      if (r.title.empty()) r.title = first;
    }
    r.has_body = true;
    return true;
  };

  auto local = latest_notes(notes.list("note"));
  auto put_local = [&](const NoteRec& n) {
    json::Value o = json::Value::object();
    o.set("id", json::Value::string(n.id));
    o.set("title", json::Value::string(n.title));
    o.set("cat", json::Value::string(n.cat.empty() ? "Google" : n.cat));
    o.set("body", json::Value::string(n.body));
    o.set("deleted", json::Value::boolean(n.deleted));
    notes.put("note", o.dump());
  };
  auto trash_remote = [&](uint32_t uid) -> bool {
    if (!trash.empty()) { if (!imap.uid_copy(std::to_string(uid), trash).ok()) return false; }
    if (!imap.uid_store(std::to_string(uid), "+FLAGS", "\\Deleted").ok()) return false;
    return imap.uid_expunge(std::to_string(uid)).ok();
  };
  auto push = [&](const NoteRec& n, uint32_t old_uid, NEntry& out) -> bool {
    auto up = imap.append(box, "\\Seen", note_message(n, net::mime::format_rfc5322_date(now)));
    if (!up.ok()) { rep.problems.push_back("could not store note " + n.id + ": " + up.error().message); return false; }
    uint32_t new_uid = *up;
    if (new_uid == 0) {   // no UIDPLUS: find it by its id header
      auto s = imap.uid_search("HEADER " + std::string(kNoteIdHeader) + " " + net::imap_quote(n.id));
      if (s.ok() && !s->empty()) new_uid = s->back();
    }
    if (old_uid && old_uid != new_uid) (void)trash_remote(old_uid);
    out.uid = new_uid;
    out.hash = note_hash(n);
    return true;
  };

  std::set<std::string> handled;
  // 1. what the server has
  for (auto& kv : remote) {
    Remote& r = kv.second;
    handled.insert(r.id);
    auto en = entries.find(r.id);
    auto lo = local.find(r.id);
    const bool uid_known = en != entries.end() && en->second.uid == r.uid;
    if (!r.has_body && (!uid_known || lo == local.end() || !lo->second.deleted)) {
      if (!uid_known || lo == local.end()) { if (!fetch_body(r)) { rep.problems.push_back("could not read note " + r.id); continue; } }
    }
    if (en == entries.end()) {
      if (lo == local.end() || lo->second.deleted) {
        if (!r.has_body && !fetch_body(r)) { rep.problems.push_back("could not read note " + r.id); continue; }
        NoteRec n{r.id, r.title, r.cat, r.body, false};
        put_local(n);
        entries[r.id] = NEntry{r.uid, note_hash(n)};
        rep.pulled_new++;
      } else {
        // The same note id exists both sides and was never linked (the state was lost): the local text stands.
        NoteRec n = lo->second;
        NEntry ne;
        if (r.has_body || fetch_body(r)) {
          NoteRec rn{r.id, r.title, r.cat, r.body, false};
          if (note_hash(rn) != note_hash(n)) { keep_conflict("note", n.id, "{\"title\":" + json::quote(n.title) + ",\"body\":" + json::quote(n.body) + "}"); rep.conflicts++; put_local(rn); n = rn; }
        }
        ne.uid = r.uid; ne.hash = note_hash(n);
        entries[r.id] = ne;
      }
      continue;
    }
    // linked
    if (lo == local.end()) { entries.erase(en); continue; }
    const NoteRec& ln = lo->second;
    const bool local_changed = note_hash(ln) != en->second.hash;
    const bool remote_changed = r.uid != en->second.uid;   // an edit elsewhere is a new message
    if (ln.deleted) {
      if (local_changed) {
        if (trash_remote(r.uid)) { rep.pushed_deleted++; entries.erase(en); }
        else rep.problems.push_back("could not delete note " + r.id + " there");
      }
      continue;
    }
    if (remote_changed) {
      if (!r.has_body && !fetch_body(r)) { rep.problems.push_back("could not read note " + r.id); continue; }
      NoteRec rn{r.id, r.title, r.cat.empty() ? ln.cat : r.cat, r.body, false};
      if (local_changed) { keep_conflict("note", ln.id, "{\"title\":" + json::quote(ln.title) + ",\"body\":" + json::quote(ln.body) + "}"); rep.conflicts++; }
      put_local(rn);
      en->second.uid = r.uid;
      en->second.hash = note_hash(rn);
      rep.pulled_updated++;
    } else if (local_changed) {
      NEntry ne;
      if (push(ln, r.uid, ne)) { en->second = ne; rep.pushed_updated++; }
    }
  }
  // 2. notes we linked that the server no longer has
  std::vector<std::string> drop;
  for (auto& kv : entries) {
    if (handled.count(kv.first)) continue;
    auto lo = local.find(kv.first);
    if (lo == local.end() || lo->second.deleted) { drop.push_back(kv.first); continue; }
    if (note_hash(lo->second) != kv.second.hash) {
      NEntry ne;
      if (push(lo->second, 0, ne)) { kv.second = ne; rep.pushed_new++; handled.insert(kv.first); }
      continue;
    }
    NoteRec gone = lo->second;
    gone.deleted = true;
    put_local(gone);
    drop.push_back(kv.first);
    rep.pulled_deleted++;
  }
  for (const auto& id : drop) entries.erase(id);
  // 3. local notes the server has never seen
  for (auto& kv : local) {
    if (handled.count(kv.first) || entries.count(kv.first) || kv.second.deleted) continue;
    NEntry ne;
    if (push(kv.second, 0, ne)) { entries[kv.first] = ne; rep.pushed_new++; }
  }

  json::Value nobj = json::Value::object();
  for (const auto& kv : entries) {
    json::Value o = json::Value::object();
    o.set("uid", json::Value::integer(kv.second.uid));
    o.set("hash", json::Value::string(kv.second.hash));
    nobj.set(kv.first, std::move(o));
  }
  json::Value sect = json::Value::object();
  sect.set("uidvalidity", json::Value::integer(sel->uidvalidity));
  sect.set("notes", std::move(nobj));
  sect.set("last_sync", json::Value::integer(now));
  (void)imap.logout();
  if (auto sv = save_state(with_section(*st, account_, "notes", std::move(sect))); !sv.ok()) return Result<GoogleSyncReport>(sv.error());
  return rep;
}

}  // namespace aios::userland
