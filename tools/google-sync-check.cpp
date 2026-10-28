// Drives the Google sync (calendar, contacts, notes) against scripts/mock-google.py and scripts/mock-imap.py,
// signed in through the real OAuth flow. Checks what a naive client gets wrong: windows and paging, two-way
// edits and deletes, conflicts, read-only calendars, an expired token in the middle of a sync, a 429 with
// Retry-After, the People API's stale reads after a write, a lost state file, and that an update to a contact
// leaves the fields AIOS does not hold alone.
//   aios-google-sync-check <google-port> <imap-port> <scratch-dir>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

#include "aios/channel/http_client.hpp"
#include "aios/hal/host.hpp"
#include "aios/net/imap.hpp"
#include "aios/net/mime.hpp"
#include "aios/userland/google_sync.hpp"

using namespace aios;
using namespace aios::userland;

static int g_fail = 0;
static std::string g_detail;
static void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) { ++g_fail; if (!g_detail.empty()) std::printf("      last sync: %s\n", g_detail.c_str()); }
}
template <class R> static void d(const R& r) {
  if (!r.ok()) { g_detail = r.error().str(); return; }
  g_detail = r->summary();
  for (const auto& p : r->problems) g_detail += "\n      problem: " + p;
}
static hal::Host* g_host = nullptr;
static std::string g_base;

static json::Value http(const std::string& method, const std::string& path, const json::Value* body = nullptr, int* status = nullptr) {
  auto ep = channel::parse_endpoint(g_base + path);
  channel::HttpRequest req;
  req.method = method;
  req.endpoint = *ep;
  req.host = g_host;
  req.timeout_ms = 10000;
  if (body) { std::string t = body->dump(); req.body = ByteBuffer(t.begin(), t.end()); req.headers.push_back({"Content-Type", "application/json"}); }
  auto r = channel::http_send(req);
  if (!r.ok()) return json::Value();
  if (status) *status = r->status;
  auto d = json::parse(r->text());
  return d.ok() ? *d : json::Value();
}
static json::Value admin(const std::string& op, const json::Value& body) { return http("POST", "/_admin/" + op, &body); }
static json::Value dump() { return http("GET", "/_admin/dump"); }
static int counter(const char* name) { return static_cast<int>(http("GET", "/_state")[name].i64()); }

static json::Value obj() { return json::Value::object(); }
static json::Value S(const std::string& s) { return json::Value::string(s); }

static json::Value timed(const std::string& id, const std::string& summary, const std::string& start, const std::string& end) {
  json::Value e = obj();
  e.set("id", S(id));
  e.set("summary", S(summary));
  json::Value st = obj(), en = obj();
  st.set("dateTime", S(start));
  en.set("dateTime", S(end));
  e.set("start", st);
  e.set("end", en);
  return e;
}

static std::string iso_local(int64_t epoch, const char* off) {   // epoch is shown as wall-clock time in zone `off` ("-04:00")
  int sign = off[0] == '-' ? -1 : 1;
  int mins = (std::atoi(std::string(off + 1, 2).c_str()) * 60 + std::atoi(std::string(off + 4, 2).c_str())) * sign;
  std::string z = format_rfc3339_utc(epoch + mins * 60);   // "....Z" but shifted
  z.pop_back();
  return z + off;
}

static std::optional<CalendarEvent> find_title(const CalendarStore& c, const std::string& title) {
  for (const auto& e : c.all_events()) if (e.title == title) return e;
  return std::nullopt;
}
static std::optional<ContactCard> find_contact(const ContactsStore& c, const std::string& first) {
  for (const auto& e : c.all_contacts()) if (e.first_name == first) return e;
  return std::nullopt;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 4) { std::fprintf(stderr, "usage: %s <google-port> <imap-port> <scratch-dir>\n", argv[0]); return 2; }
  g_base = std::string("http://127.0.0.1:") + argv[1];
  const std::string imap_port = argv[2];
  const std::string root = argv[3];
  auto host = hal::make_host();
  g_host = host.get();
  (void)host->make_dir(root);
  (void)host->make_dir(root + "/oauth");
  const std::string account = "test@example.com";

  // ---- the sign-in, through the real flow ----
  {
    std::string cfg = "{\"installed\":{\"client_id\":\"mock-client.apps.googleusercontent.com\",\"client_secret\":\"mock-secret\","
                      "\"auth_uri\":\"" + g_base + "/auth\",\"token_uri\":\"" + g_base + "/token\",\"userinfo_uri\":\"" + g_base + "/userinfo\","
                      "\"revoke_uri\":\"" + g_base + "/revoke\",\"calendar_api\":\"" + g_base + "/calendar/v3\",\"people_api\":\"" + g_base + "/v1\","
                      "\"imap_host\":\"localhost\",\"imap_port\":" + imap_port + ",\"imap_security\":\"none\"}}";
    (void)host->write_file(root + "/oauth/google.json", ByteSpan(reinterpret_cast<const uint8_t*>(cfg.data()), cfg.size()));
  }
  OAuth oauth(*host, root);
  auto cfg = oauth.load_config("google");
  if (!cfg.ok()) { std::printf("FAIL  config: %s\n", cfg.error().message.c_str()); return 1; }
  auto lis = hal::net_listen(0, true);
  const std::string redirect = "http://127.0.0.1:" + std::to_string((*lis)->port()) + "/";
  auto st = oauth.begin(*cfg, redirect, account);
  std::thread br([&] {
    auto ep = channel::parse_endpoint(st->url);
    channel::HttpRequest rq; rq.endpoint = *ep; rq.host = host.get();
    auto r = channel::http_send(rq);
    if (r.ok() && r->status == 302) { auto e2 = channel::parse_endpoint(r->header("Location")); channel::HttpRequest r2; r2.endpoint = *e2; r2.host = host.get(); (void)channel::http_send(r2); }
  });
  auto red = oauth.wait_for_redirect(**lis, 15000);
  br.join();
  auto tok = red.ok() ? oauth.finish(*cfg, st->pending, *red) : Result<OAuthToken>(red.error());
  check(tok.ok() && oauth.save_token(*tok).ok() && tok->account == account, "signed in through the OAuth flow against the mock");
  if (!tok.ok()) return 1;

  GoogleSync sync(*host, root, account);
  const int64_t now = static_cast<int64_t>(host->wall_ns() / 1000000000ull);

  // =========================== CALENDAR ===========================
  CalendarStore cal(root + "/calendar");
  const int64_t d1 = now + 2 * 86400, d3 = now + 3 * 86400;
  // A: timed, in a zone of its own, with attendees. B: all day. C: later. Old: outside the window. T: a read-only calendar.
  json::Value A = timed("evA", "Planning", iso_local(d1, "-04:00"), iso_local(d1 + 3600, "-04:00"));
  { json::Value at = json::Value::array(); json::Value x = obj(); x.set("email", S("x@example.com")); at.push(x); json::Value y = obj(); y.set("email", S("y@example.com")); at.push(y); A.set("attendees", at); A.set("description", S("agenda: one\ntwo")); A.set("location", S("Room 4")); }
  json::Value B = obj();
  { B.set("id", S("evB")); B.set("summary", S("Holiday")); json::Value s = obj(), e = obj(); s.set("date", S(format_rfc3339_utc(now + 5 * 86400).substr(0, 10))); e.set("date", S(format_rfc3339_utc(now + 7 * 86400).substr(0, 10))); B.set("start", s); B.set("end", e); }
  json::Value C = timed("evC", "Review", iso_local(d3, "+02:00"), iso_local(d3 + 5400, "+02:00"));
  json::Value Old = timed("evOld", "Ancient", iso_local(now - 90 * 86400, "+00:00"), iso_local(now - 90 * 86400 + 3600, "+00:00"));
  json::Value T = timed("evT", "Team standup", iso_local(d1 + 7200, "+00:00"), iso_local(d1 + 9000, "+00:00"));
  for (auto& pr : {std::make_pair("sean@example.com", A), {"sean@example.com", B}, {"sean@example.com", C}, {"sean@example.com", Old}, {"team@group.calendar.google.com", T}}) {
    json::Value body = obj(); body.set("cal", S(pr.first)); body.set("event", pr.second); admin("put_event", body);
  }

  auto r1 = sync.sync_calendar(cal); d(r1);
  check(r1.ok() && r1->pulled_new == 4 && r1->problems.empty(), "first calendar sync pulls the four events in the window, not the one from 90 days ago" + (r1.ok() ? " (" + r1->summary() + ")" : r1.error().message));
  auto a = find_title(cal, "Planning");
  Rfc3339 pa; parse_rfc3339(iso_local(d1, "-04:00"), pa);
  check(a && a->year == pa.year && a->month == pa.month && a->day == pa.day && a->start_hour == pa.hour && a->start_min == pa.minute && a->duration_mins == 60 && !a->all_day,
        "a timed event keeps the wall-clock time it was written in (zone -04:00), not this machine's");
  check(a && a->attendees.size() == 2 && a->attendees[0] == "x@example.com" && a->location == "Room 4" && a->description == "agenda: one\ntwo", "attendees and location come across");
  auto b = find_title(cal, "Holiday");
  check(b && b->all_day && b->duration_mins == 2 * 1440, "an all-day event spanning two days is all-day, 2880 minutes");
  auto c0 = find_title(cal, "Review");
  check(c0 && c0->duration_mins == 90, "a 90-minute event is 90 minutes");
  check(find_title(cal, "Team standup").has_value(), "events of a second (read-only) calendar come across too");
  check(!find_title(cal, "Ancient").has_value(), "the event from 90 days ago is outside the window and not pulled");
  bool in_cat = false;
  for (const auto& cc : cal.categories()) if (cc.id == "google") in_cat = true;
  check(in_cat, "a Google category exists for the pulled events");

  auto r2 = sync.sync_calendar(cal); d(r2);
  check(r2.ok() && r2->pulled_new + r2->pulled_updated + r2->pulled_deleted + r2->pushed_new + r2->pushed_updated + r2->pushed_deleted + r2->conflicts == 0,
        "a second sync with nothing changed changes nothing (idempotent)");

  // local edit goes up, with no invitation emails
  {
    CalendarEvent e = *a;
    e.title = "Planning v2";
    cal.update_event(e);
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->pushed_updated == 1 && r->conflicts == 0, "a local edit is pushed");
    bool found = false, attendees_kept = false;
    const json::Value dmp = dump();
    for (const auto& ev : dmp["cals"]["sean@example.com"].items()) if (ev["id"].str() == "evA") { found = ev["summary"].str() == "Planning v2"; attendees_kept = ev["attendees"].size() == 2; }
    check(found, "Google has the new title");
    check(attendees_kept, "an edit that did not touch the attendees leaves the guest list (and their replies) alone");
    bool all_none = true;
    const json::Value dmp2 = dump();
    for (const auto& u : dmp2["send_updates"].items()) if (u.str() != "none") all_none = false;
    check(all_none && dump()["send_updates"].size() > 0, "every write said sendUpdates=none: an unattended sync sends no invitations");
  }
  // remote edit comes down
  {
    json::Value body = obj(); body.set("cal", S("sean@example.com"));
    json::Value ev = C; ev.set("summary", S("Review (moved)")); body.set("event", ev); admin("put_event", body);
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->pulled_updated == 1 && find_title(cal, "Review (moved)"), "an edit made on Google comes down");
  }
  // both sides edit
  {
    auto e0 = find_title(cal, "Planning v2");
    CalendarEvent e = *e0; e.title = "Planning (mine)"; cal.update_event(e);
    json::Value body = obj(); body.set("cal", S("sean@example.com")); json::Value ev = A; ev.set("summary", S("Planning (theirs)")); body.set("event", ev); admin("put_event", body);
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->conflicts == 1 && find_title(cal, "Planning (theirs)") && !find_title(cal, "Planning (mine)"), "edited on both sides: Google's version wins, and it is said out loud");
    bool kept = false;
    auto lst = host->list_dir(root + "/google/conflicts");
    if (lst.ok()) for (const auto& f : *lst) if (f.find("calendar-") == 0) kept = true;
    check(kept, "the losing local version is kept in google/conflicts/, not dropped");
  }
  // deletes both ways
  {
    auto hb = find_title(cal, "Holiday");
    cal.delete_event(hb->id);
    json::Value body = obj(); body.set("cal", S("sean@example.com")); body.set("id", S("evC")); admin("delete_event", body);
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->pushed_deleted == 1 && r->pulled_deleted == 1, "a delete here goes up, a delete there comes down");
    bool b_gone = true;
    const json::Value dmp = dump();
    for (const auto& ev : dmp["cals"]["sean@example.com"].items()) if (ev["id"].str() == "evB") b_gone = false;
    check(b_gone && !find_title(cal, "Review (moved)"), "both are gone on both sides");
  }
  // a new local event goes up, to the primary calendar, and stays put
  {
    CalendarEvent e;
    e.title = "Dentist";
    e.calendar_id = "personal";
    Rfc3339 t; parse_rfc3339(iso_local(now + 4 * 86400, "+00:00"), t);
    e.year = t.year; e.month = t.month; e.day = t.day; e.start_hour = 9; e.start_min = 30; e.duration_mins = 45;
    cal.add_event(e);
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->pushed_new == 1, "a new local event is sent");
    bool there = false;
    const json::Value dmp = dump();
    for (const auto& ev : dmp["cals"]["sean@example.com"].items()) if (ev["summary"].str() == "Dentist") {
      there = ev["start"]["dateTime"].str().find("T09:30:00") != std::string::npos && ev["end"]["dateTime"].str().find("T10:15:00") != std::string::npos;
    }
    check(there, "with its start and end as the local wall-clock time and the 45-minute duration");
    auto r3 = sync.sync_calendar(cal); d(r3);
    check(r3.ok() && r3->pushed_new == 0 && r3->pulled_new == 0, "and is not sent again or pulled back as a duplicate");
  }
  // read-only calendar: an edit cannot go up; it is reported and nothing breaks
  {
    auto tt = find_title(cal, "Team standup");
    if (!tt) { check(false, "the read-only calendar event exists to be edited"); std::printf("%s (%d failed)\n", "FAILED", g_fail); return 1; }
    CalendarEvent e = *tt; e.title = "Team standup (edited)"; cal.update_event(e);
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->problems.size() == 1 && r->problems[0].find("could not update") != std::string::npos, "an edit to a read-only calendar's event is reported as a problem, not swallowed");
    check(find_title(cal, "Team standup (edited)").has_value(), "and the local edit is still there");
  }
  // paging
  {
    admin("page_size", [] { json::Value v = obj(); v.set("n", json::Value::integer(2)); return v; }());
    for (int i = 0; i < 7; ++i) {
      json::Value body = obj(); body.set("cal", S("sean@example.com"));
      body.set("event", timed("pg" + std::to_string(i), "Paged " + std::to_string(i), iso_local(now + (10 + i) * 86400, "+00:00"), iso_local(now + (10 + i) * 86400 + 1800, "+00:00")));
      admin("put_event", body);
    }
    auto r = sync.sync_calendar(cal); d(r);
    int n = 0;
    for (const auto& e : cal.all_events()) if (e.title.rfind("Paged ", 0) == 0) ++n;
    check(r.ok() && r->pulled_new == 7 && n == 7, "seven events served two to a page are all pulled");
    admin("page_size", [] { json::Value v = obj(); v.set("n", json::Value::integer(250)); return v; }());
  }
  // rate limit
  {
    admin("fail_next", [] { json::Value v = obj(); v.set("code", json::Value::integer(429)); v.set("retry", json::Value::integer(1)); return v; }());
    auto t0 = std::chrono::steady_clock::now();
    auto r = sync.sync_calendar(cal); d(r);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(r.ok() && ms >= 900, "a 429 with Retry-After: 1 is waited out and the sync completes (took " + std::to_string(ms) + " ms)");
  }
  // the access token dies mid-use
  {
    int before = counter("token_refresh");
    http("POST", "/_expire");
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && counter("token_refresh") == before + 1, "an access token the server no longer honours is refreshed and the call retried");
  }
  // state lost
  {
    size_t before = cal.all_events().size();
    (void)host->remove(root + "/google/state.json");
    auto r = sync.sync_calendar(cal); d(r);
    check(r.ok() && r->pulled_new == 0 && cal.all_events().size() == before, "with the state file gone, events are adopted, not duplicated");
  }

  // =========================== CONTACTS ===========================
  ContactsStore people(root + "/contacts");
  json::Value p1 = obj();
  p1.set("resourceName", S("people/c1"));
  { json::Value n = json::Value::array(); json::Value x = obj(); x.set("givenName", S("Ada")); x.set("familyName", S("Lovelace")); x.set("displayName", S("Ada Lovelace")); n.push(x); p1.set("names", n); }
  { json::Value e = json::Value::array(); json::Value a1 = obj(); a1.set("value", S("ada@home.example")); a1.set("type", S("home")); e.push(a1);
    json::Value a2 = obj(); a2.set("value", S("ada@work.example")); a2.set("type", S("work")); e.push(a2);
    json::Value a3 = obj(); a3.set("value", S("ada-third@other.example")); a3.set("type", S("other")); e.push(a3); p1.set("emailAddresses", e); }
  { json::Value ph = json::Value::array(); json::Value q1 = obj(); q1.set("value", S("+1 555 0100")); q1.set("type", S("mobile")); ph.push(q1);
    json::Value q2 = obj(); q2.set("value", S("+1 555 0199")); q2.set("type", S("work")); ph.push(q2); p1.set("phoneNumbers", ph); }
  { json::Value o = json::Value::array(); json::Value x = obj(); x.set("name", S("Analytical Engines")); x.set("title", S("Programmer")); o.push(x); p1.set("organizations", o); }
  { json::Value ad = json::Value::array(); json::Value x = obj(); x.set("streetAddress", S("12 Byron Rd")); x.set("city", S("London")); x.set("postalCode", S("W1")); x.set("country", S("UK")); ad.push(x); p1.set("addresses", ad); }
  { json::Value births = json::Value::array(); json::Value x = obj(); json::Value d = obj(); d.set("year", json::Value::integer(1815)); d.set("month", json::Value::integer(12)); d.set("day", json::Value::integer(10)); x.set("date", d); births.push(x); p1.set("birthdays", births); }
  { json::Value ph = json::Value::array(); json::Value x = obj(); x.set("url", S("https://photos.example/ada.jpg")); ph.push(x); p1.set("photos", ph); }   // not a field AIOS holds
  json::Value p2 = obj(); p2.set("resourceName", S("people/c2"));
  { json::Value n = json::Value::array(); json::Value x = obj(); x.set("givenName", S("Grace")); x.set("familyName", S("Hopper")); n.push(x); p2.set("names", n); }
  { json::Value e = json::Value::array(); json::Value a1 = obj(); a1.set("value", S("grace@navy.example")); e.push(a1); p2.set("emailAddresses", e); }
  json::Value p3 = obj(); p3.set("resourceName", S("people/c3"));
  { json::Value n = json::Value::array(); json::Value x = obj(); x.set("givenName", S("Alan")); n.push(x); p3.set("names", n); }
  admin("put_person", p1); admin("put_person", p2); admin("put_person", p3);

  auto c1 = sync.sync_contacts(people); d(c1);
  check(c1.ok() && c1->pulled_new == 3 && c1->problems.empty(), "first contacts sync pulls three people" + (c1.ok() ? " (" + c1->summary() + ")" : c1.error().message));
  auto ada = find_contact(people, "Ada");
  check(ada && ada->last_name == "Lovelace" && ada->email == "ada@home.example" && ada->email_work == "ada@work.example" && ada->phone == "+1 555 0100" &&
            ada->phone_work == "+1 555 0199" && ada->company == "Analytical Engines" && ada->job_title == "Programmer" && ada->city == "London" && ada->country == "UK" &&
            ada->birthday == "1815-12-10",
        "name, personal and work email and phone, organisation, address and birthday map to the right fields");
  auto c2 = sync.sync_contacts(people); d(c2);
  check(c2.ok() && c2->pulled_new + c2->pulled_updated + c2->pushed_updated + c2->pushed_new + c2->conflicts == 0, "a second contacts sync changes nothing");

  // local edit: only what AIOS holds changes; the third email and the photo survive
  {
    ContactCard e = *ada; e.first_name = "Augusta"; e.phone = "+1 555 0111"; e.notes = "first programmer";
    people.update_contact(e);
    auto r = sync.sync_contacts(people); d(r);
    check(r.ok() && r->pushed_updated == 1, "a local contact edit is pushed");
    json::Value p;
    const json::Value dmp = dump();
    for (const auto& x : dmp["people"].items()) if (x["resourceName"].str() == "people/c1") p = x;
    check(p["names"][static_cast<size_t>(0)]["givenName"].str() == "Augusta" && p["phoneNumbers"][static_cast<size_t>(0)]["value"].str() == "+1 555 0111" &&
              p["biographies"][static_cast<size_t>(0)]["value"].str() == "first programmer",
          "Google has the edited name, phone and note");
    check(p["emailAddresses"].size() == 3 && p["emailAddresses"][static_cast<size_t>(2)]["value"].str() == "ada-third@other.example",
          "the third email address AIOS has no slot for is still there");
    check(p["photos"].size() == 1, "and so is the photo: an update replaces only the fields it owns");
    check(p["phoneNumbers"].size() == 2 && p["phoneNumbers"][static_cast<size_t>(1)]["value"].str() == "+1 555 0199", "the work phone is untouched");
  }
  // a stale read right after our write must not undo it
  {
    admin("stale_reads", [] { json::Value v = obj(); v.set("n", json::Value::integer(1)); return v; }());
    auto r = sync.sync_contacts(people); d(r);
    auto a2 = find_contact(people, "Augusta");
    check(r.ok() && a2 && a2->phone == "+1 555 0111" && r->pulled_updated == 0, "the People API serving a read from before our write is recognised: the local edit is not reverted");
  }
  // remote edit comes down
  {
    json::Value q = p2;
    { json::Value n = json::Value::array(); json::Value x = obj(); x.set("givenName", S("Grace")); x.set("familyName", S("Murray Hopper")); n.push(x); q.set("names", n); }
    admin("put_person", q);
    auto r = sync.sync_contacts(people); d(r);
    auto g = find_contact(people, "Grace");
    check(r.ok() && r->pulled_updated == 1 && g && g->last_name == "Murray Hopper", "an edit made on Google comes down");
  }
  // deletes both ways, and new
  {
    auto g = find_contact(people, "Grace");
    people.delete_contact(g->id);
    json::Value del = obj(); del.set("resourceName", S("people/c3")); admin("delete_person", del);
    ContactCard nc; nc.first_name = "Linus"; nc.email = "linus@example.org"; nc.company = "Kernel";
    people.add_contact(nc);
    auto r = sync.sync_contacts(people); d(r);
    check(r.ok() && r->pushed_deleted == 1 && r->pulled_deleted == 1 && r->pushed_new == 1, "delete here goes up, delete there comes down, a new contact is sent");
    bool linus = false, grace_gone = true;
    const json::Value dmp = dump();
    for (const auto& x : dmp["people"].items()) { if (x["names"][static_cast<size_t>(0)]["givenName"].str() == "Linus") linus = x["emailAddresses"][static_cast<size_t>(0)]["value"].str() == "linus@example.org"; if (x["resourceName"].str() == "people/c2") grace_gone = false; }
    check(linus && grace_gone && !find_contact(people, "Alan"), "Google has Linus and no Grace; Alan is gone here");
  }
  // both sides edit
  {
    auto a2 = find_contact(people, "Augusta");
    ContactCard e = *a2; e.company = "Mine Ltd"; people.update_contact(e);
    json::Value p;
    const json::Value dmp = dump();
    for (const auto& x : dmp["people"].items()) if (x["resourceName"].str() == "people/c1") p = x;
    json::Value o = json::Value::array(); json::Value x = obj(); x.set("name", S("Theirs Inc")); o.push(x); p.set("organizations", o);
    admin("put_person", p);
    auto r = sync.sync_contacts(people); d(r);
    auto a3 = find_contact(people, "Augusta");
    check(r.ok() && r->conflicts == 1 && a3 && a3->company == "Theirs Inc", "edited on both sides: Google's version wins and a conflict is counted");
  }
  // paging
  {
    admin("page_size", [] { json::Value v = obj(); v.set("n", json::Value::integer(2)); return v; }());
    for (int i = 0; i < 5; ++i) {
      json::Value q = obj(); q.set("resourceName", S("people/c" + std::to_string(500 + i)));
      json::Value n = json::Value::array(); json::Value x = obj(); x.set("givenName", S("Page" + std::to_string(i))); n.push(x); q.set("names", n);
      admin("put_person", q);
    }
    auto r = sync.sync_contacts(people); d(r);
    int n = 0;
    for (const auto& c : people.all_contacts()) if (c.first_name.rfind("Page", 0) == 0) ++n;
    check(r.ok() && r->pulled_new == 5 && n == 5, "five contacts served two to a page are all pulled");
    admin("page_size", [] { json::Value v = obj(); v.set("n", json::Value::integer(1000)); return v; }());
  }

  // =========================== NOTES ===========================
  std::vector<std::string> note_records;   // newest first, as the store lists them
  NoteStore ns;
  ns.list = [&](const std::string&) { std::string s = "["; for (size_t i = 0; i < note_records.size(); ++i) { if (i) s += ","; s += note_records[i]; } return s + "]"; };
  ns.put = [&](const std::string&, const std::string& j) { note_records.insert(note_records.begin(), j); return std::string("id"); };
  auto note = [&](const std::string& id, const std::string& title, const std::string& body, bool del = false) {
    json::Value o = obj(); o.set("id", S(id)); o.set("title", S(title)); o.set("cat", S("Personal")); o.set("body", S(body)); o.set("deleted", json::Value::boolean(del));
    note_records.insert(note_records.begin(), o.dump());
  };
  note("NOTE-1", "Shopping", "milk\n\neggs\ncaf\xc3\xa9 \xe2\x82\xac" "5");
  note("NOTE-2", "Ideas", "one line");
  net::ImapClient probe(*host, root, 10000);
  (void)probe.connect("localhost", static_cast<uint16_t>(std::atoi(imap_port.c_str())), net::Security::None);
  auto probe_tok = oauth.access_token("google", account);
  (void)probe.authenticate_xoauth2(account, probe_tok.ok() ? *probe_tok : "");

  auto n1 = sync.sync_notes(ns); d(n1);
  check(n1.ok() && n1->pushed_new == 2 && n1->problems.empty(), "first notes sync creates the Notes mailbox and stores both notes" + (n1.ok() ? " (" + n1->summary() + ")" : n1.error().message));
  {
    auto sel = probe.select("Notes", true);
    check(sel.ok() && sel->exists == 2, "the Notes mailbox holds two messages");
    auto got = probe.uid_fetch("1:*", "(UID BODY.PEEK[])");
    bool shop = false, html = false, hdr = false;
    if (got.ok()) for (const auto& f : *got) {
      auto it = f.sections.find("BODY[]");
      if (it == f.sections.end()) continue;
      auto p = net::mime::parse(it->second);
      if (p.decoded_header("Subject") == "Shopping") {
        shop = [&] { std::string t; for (size_t i = 0; i < p.text.size(); ++i) if (!(p.text[i] == '\r' && i + 1 < p.text.size() && p.text[i + 1] == '\n')) t += p.text[i]; return t; }() == std::string("Shopping\nmilk\n\neggs\ncaf\xc3\xa9 \xe2\x82\xac") + "5";
        html = p.html.find("<div>milk</div>") != std::string::npos && p.html.find("<div><br></div>") != std::string::npos;
        hdr = p.header("X-AIOS-Note-ID") == "NOTE-1" && p.header("X-Uniform-Type-Identifier") == "com.apple.mail-note";
      }
    }
    check(shop && html && hdr, "a note is stored as a message: the title as subject, the text, an HTML rendering, and the Apple Notes marker");
  }
  auto n2 = sync.sync_notes(ns); d(n2);
  check(n2.ok() && n2->pulled_new + n2->pushed_new + n2->pushed_updated + n2->pulled_updated + n2->conflicts == 0, "a second notes sync changes nothing");

  note("NOTE-1", "Shopping", "milk\n\neggs\nbread");
  auto n3 = sync.sync_notes(ns); d(n3);
  check(n3.ok() && n3->pushed_updated == 1, "an edited note is stored as its new version");
  {
    auto sel = probe.select("Notes", true);
    check(sel.ok() && sel->exists == 2, "and the old version is gone from the mailbox (still two messages)");
    auto tsel = probe.select("Trash", true);
    check(tsel.ok() && tsel->exists == 1, "it went to Trash, not just out of the label");
  }
  // a note made by another client, HTML only, no marker headers
  {
    std::string foreign = "From: me@example.com\r\nSubject: From my phone\r\nMessage-ID: <phone-1@icloud.example>\r\nDate: Fri, 16 Oct 2026 12:00:00 +0000\r\n"
                          "MIME-Version: 1.0\r\nContent-Type: text/html; charset=utf-8\r\nX-Uniform-Type-Identifier: com.apple.mail-note\r\n\r\n"
                          "<html><body><div>From my phone</div><div>first line</div><div><br></div><div>after a gap &amp; more</div></body></html>\r\n";
    (void)probe.select("Notes", false);
    auto up = probe.append("Notes", "\\Seen", foreign);
    auto r = sync.sync_notes(ns); d(r);
    check(up.ok() && r.ok() && r->pulled_new == 1, "a note written by another client appears here");
    auto latest = json::parse(ns.list("note"));
    bool ok = false;
    for (const auto& n : latest->items()) if (n["title"].str() == "From my phone") ok = n["body"].str() == "first line\n\nafter a gap & more" && !n["deleted"].boolean_or(false);
    check(ok, "its HTML becomes lines of text (title not repeated, the gap kept, entities decoded)");
  }
  // delete here
  {
    note("NOTE-2", "Ideas", "one line", true);
    auto r = sync.sync_notes(ns); d(r);
    auto sel = probe.select("Notes", true);
    check(r.ok() && r->pushed_deleted == 1 && sel.ok() && sel->exists == 2, "a note deleted here is deleted there");
  }
  // delete there
  {
    (void)probe.select("Notes", false);
    auto found = probe.uid_search("SUBJECT \"From my phone\"");
    if (found.ok() && !found->empty()) { (void)probe.uid_store(std::to_string(found->back()), "+FLAGS", "\\Deleted"); (void)probe.uid_expunge(std::to_string(found->back())); }
    auto r = sync.sync_notes(ns); d(r);
    bool deleted_here = false;
    auto latest = json::parse(ns.list("note"));
    for (const auto& n : latest->items()) if (n["title"].str() == "From my phone") { deleted_here = n["deleted"].boolean_or(false); break; }
    check(r.ok() && r->pulled_deleted == 1 && deleted_here, "a note deleted there is marked deleted here");
  }

  // a sign-in the provider has revoked
  {
    http("POST", "/_revoke");
    auto cur = oauth.load_token("google", account);
    cur->expires_at = 1;
    (void)oauth.save_token(*cur);
    auto r = sync.sync_calendar(cal); d(r);
    check(!r.ok() && r.error().status == Status::Expired, "a revoked sign-in is reported as Expired: sign in again");
  }

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
