#pragma once
// Two-way sync of calendar, contacts and notes with a Google account, over the kernel's own HTTPS/TLS
// (src/channel/http_client.cpp, src/net/tls.cpp) and IMAP (src/net/imap.cpp), signed in with OAuth 2.0
// (oauth.hpp). Mail itself is the mail store's: an account with auth="oauth2" syncs and sends through
// IMAP/SMTP with XOAUTH2. Nothing here starts another program.
//
//   Calendar  Google Calendar API v3. Pulls a window (30 days back, a year ahead) with recurring events
//             expanded into their occurrences, pushes local adds, edits and deletes. Edits send no
//             invitations (sendUpdates=none). Times are the wall-clock time in the event's own zone.
//   Contacts  People API v1. Pulls every contact, pushes local adds, edits and deletes. An update changes
//             only the fields AIOS holds and leaves the rest of the person (extra phones, photos) alone.
//   Notes     The "Notes" mailbox of the IMAP account (Gmail shows it as the Notes label), one message per
//             note, readable by Apple Notes and other IMAP note clients.
//
// What was last synced is kept in <root>/google/state.json (ids, etags, hashes; no secrets). A change made
// on both sides since the last sync is resolved in the remote's favour and the local version is kept in
// <root>/google/conflicts/, never silently dropped.

#include <functional>
#include <string>
#include <vector>

#include "aios/core/json.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/calendar_client.hpp"
#include "aios/userland/contacts_client.hpp"
#include "aios/userland/oauth.hpp"

namespace aios::userland {

struct GoogleSyncReport {
  int pulled_new = 0, pulled_updated = 0, pulled_deleted = 0;
  int pushed_new = 0, pushed_updated = 0, pushed_deleted = 0;
  int conflicts = 0;
  std::vector<std::string> problems;   // items that failed; the rest of the sync still ran
  std::string summary() const;
};

// The note records of the machine's object store (kind "note", newest record per id wins), as the desktop's
// Notes app keeps them. `list` returns the JSON array of records, `put` stores one and returns its id.
struct NoteStore {
  std::function<std::string(const std::string& kind)> list;
  std::function<std::string(const std::string& kind, const std::string& json)> put;
};

class GoogleSync {
 public:
  // `account` is the address the sign-in was made with. `root` is the machine's root.
  GoogleSync(hal::Host& host, std::string root, std::string account, std::string trust_store = {});

  Result<GoogleSyncReport> sync_calendar(CalendarStore& cal);
  Result<GoogleSyncReport> sync_contacts(ContactsStore& contacts);
  Result<GoogleSyncReport> sync_notes(NoteStore& notes);

  // Where the APIs are, from <root>/oauth/google.json ("calendar_api", "people_api", "imap_host",
  // "imap_port", "imap_security": "tls"|"starttls"|"none", and the same three for "smtp_"); the real Google
  // endpoints by default.
  struct Endpoints {
    std::string calendar = "https://www.googleapis.com/calendar/v3";
    std::string people = "https://people.googleapis.com/v1";
    std::string imap_host = "imap.gmail.com";
    uint16_t imap_port = 993;
    std::string imap_security = "tls";
    std::string smtp_host = "smtp.gmail.com";
    uint16_t smtp_port = 587;
    std::string smtp_security = "starttls";
  };
  Result<Endpoints> endpoints();

 private:
  struct Api {
    int status = 0;
    json::Value body;
    std::string etag;
  };
  Result<Api> api(const std::string& method, const std::string& url, const json::Value* body = nullptr,
                  const std::vector<std::pair<std::string, std::string>>& headers = {});
  Result<json::Value> load_state();
  Result<void> save_state(const json::Value& st);
  void keep_conflict(const std::string& kind, const std::string& id, const std::string& local_json);
  void pause_ms(int ms);

  hal::Host& host_;
  std::string root_, dir_, account_, trust_store_;
  json::Value state_;
  bool state_loaded_ = false;
};

// RFC 3339 pieces, exposed for tests.
struct Rfc3339 {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int offset_minutes = 0;
  int64_t epoch = 0;
  bool date_only = false;
};
bool parse_rfc3339(const std::string& text, Rfc3339& out);
std::string format_rfc3339_utc(int64_t epoch);

}  // namespace aios::userland
