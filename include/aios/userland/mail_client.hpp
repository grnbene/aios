#pragma once
#include <cstdint>
#include "aios/hal/sync.hpp"
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#if !defined(AIOS_METAL)
#include <atomic>
#include <mutex>
#endif
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"

namespace aios::userland {

struct MailAttachment {
  std::string filename;
  size_t size_bytes = 0;
  std::string mime_type = "application/octet-stream";
};

struct MailMessage {
  std::string id;
  std::string message_id;
  std::string folder = "inbox";    // "inbox", "flagged", "drafts", "sent", "archive", "trash", "directives"
  std::string account = "AIOS Core";
  std::string from_name;
  std::string from_addr;
  std::vector<std::string> to;
  std::vector<std::string> cc;
  std::vector<std::string> bcc;
  std::string subject;
  std::string date_str;
  uint64_t timestamp = 0;
  std::string snippet;
  std::string body;
  std::string eml_path;
  // The message has an html part on disk beside its .eml, written from the
  // sender's own markup rather than a regex-stripped rendering of it.
  bool has_html = false;
  // The .eml beside this message is the server's own bytes, not something we
  // serialised. Nothing may overwrite it.
  bool raw_on_disk = false;
  // Where this message lives on the IMAP server. Sync is UID-based: a UID is
  // stable for the life of a mailbox, so an incremental poll asks only for
  // UIDs above the highest one seen, and a body the vacuum has evicted can be
  // fetched back by it. 0 means unknown (a row written before V3).
  uint32_t uid = 0;
  size_t raw_size = 0;
  bool is_unread = false;
  bool is_flagged = false;
  bool is_draft = false;
  std::vector<MailAttachment> attachments;

  std::string initials() const {
    if (!from_name.empty()) {
      std::string res;
      bool next_is_init = true;
      for (char c : from_name) {
        if (c == ' ' || c == '-' || c == '.') {
          next_is_init = true;
        } else if (next_is_init && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
          res += (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
          next_is_init = false;
          if (res.size() >= 2) break;
        }
      }
      if (!res.empty()) return res;
    }
    if (!from_addr.empty()) {
      char c = from_addr[0];
      if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
      return std::string(1, c);
    }
    return "??";
  }
};

struct MailFolder {
  std::string id;
  std::string name;
  std::string icon_name;
  size_t unread_count = 0;
  size_t total_count = 0;
};

struct MailAccount {
  // No made-up defaults: a MailAccount that nobody configured is empty, so a
  // machine with no accounts says so instead of naming one that doesn't exist.
  std::string id;
  std::string display_name;
  std::string email;
  std::string provider = "custom"; // "icloud", "gmail", "outlook", "fastmail", "yahoo", "custom"
  std::string incoming_server;
  uint16_t incoming_port = 993;
  std::string incoming_protocol = "imap";
  std::string outgoing_server;
  uint16_t outgoing_port = 587;
  std::string outgoing_protocol = "smtp";
  bool use_tls = true;
  std::string username;
  std::string password;
  // "password": log in with `password` (an app password, or whatever the server takes). "oauth2": sign in
  // once with `aios google auth` (OAuth 2.0, see oauth.hpp); the tokens live in the encrypted OAuth
  // vault under the root, `password` stays empty, and IMAP/SMTP log in with SASL XOAUTH2.
  std::string auth = "password";
  bool is_default = true;
  bool online = true;
  std::string last_sync_status = "Never synced";
  // Epoch seconds of the last sync that actually fetched. 0 means this account
  // has never completed one -- the status line has to say so rather than
  // claim a freshness nothing established.
  uint64_t last_sync_at = 0;

  static MailAccount make_preset(const std::string& provider_name,
                                 const std::string& email_addr,
                                 const std::string& pass = "",
                                 const std::string& disp_name = "");
};

struct MailSearchQuery {
  std::string raw;
  std::vector<std::string> any_terms;
  std::vector<std::string> from_terms;
  std::vector<std::string> to_terms;
  std::vector<std::string> subject_terms;
  std::vector<std::string> body_terms;
  std::string folder;
  std::string account;
  int has_attachment = -1; // -1: any, 1: must have, 0: must not
  int is_unread = -1;      // -1: any, 1: unread, 0: read
  int is_flagged = -1;     // -1: any, 1: flagged, 0: unflagged

  static MailSearchQuery parse(const std::string& query);
  bool matches(const MailMessage& m) const;
  bool is_empty() const;
  // Whether answering this query needs message bodies read off disk. Only an
  // explicit body:/"has the words" term does -- a plain search runs against the
  // headers and the stored snippet, so typing in the search box never turns
  // into one file read per message in the mailbox.
  bool needs_body() const { return !body_terms.empty(); }
};

// What one fetch produced. Plain data, so it can cross a thread boundary
// without the worker ever touching a MailStore member.
// One message as a sync saw it: the envelope, and the body when the whole message was fetched.
struct MailFetchRecord {
  uint32_t uid = 0;
  std::string message_id, from_name, from_addr, to, cc, subject, date_str;
  uint64_t timestamp = 0;
  std::string snippet, body;
  bool have_body = false;    // the whole message was fetched; `body` is its text
  bool has_html = false;
  bool is_unread = false, is_flagged = false;
  std::vector<MailAttachment> attachments;
};

struct MailSyncOutcome {
  std::string mode = "headers";   // which kind of fetch produced this
  std::string account_id;
  std::string folder;      // the local folder this fetch was for
  bool attempted = false;
  bool ok = false;
  int fetched = 0;
  uint64_t uidvalidity = 0;
  uint32_t max_uid = 0;
  std::string status;      // what to store as last_sync_status
  std::vector<MailFetchRecord> records;
};

// Everything one fetch needs, copied out of the store so the work can run on a
// thread that never reads a member of it.
struct MailSyncJob {
  std::string mode = "headers";   // "headers" | "body" | "search"
  std::string account_id;
  std::string local_folder;       // "inbox" | "sent"
  std::string imap_folder;        // what the server calls it
  std::string server;
  uint16_t port = 993;
  std::string username;
  std::string password;
  bool oauth = false;             // log in with an OAuth2 access token (SASL XOAUTH2); `password` is unused
  std::string oauth_provider;     // which sign-in holds the token ("google")
  std::string oauth_account;      // and under which address
  uint8_t security = 1;           // 0 none, 1 TLS from the first byte, 2 STARTTLS
  uint32_t last_uid = 0;
  int limit = 0;                  // 0 = no cap (a full backfill)
  uint64_t body_since = 0;        // fetch whole messages newer than this
  std::string query;              // search mode
  std::string body_uid;           // body mode
  // Where the worker drops the raw .eml and the decoded .html for each message
  // it fetched whole, named by uid. The owning thread moves them into the
  // store under the message's own id.
  std::string raw_dir;
};

// How far one (account, folder) has been synced. UID-based, so an incremental
// poll asks the server only for what it has gained since.
struct MailFolderSync {
  std::string account_id;
  std::string folder;
  uint64_t uidvalidity = 0;   // changes mean the server renumbered; start over
  uint32_t last_uid = 0;
  bool backfilled = false;    // the deep first pass has completed
};

// The server-side name of a folder we mirror, per provider ("sent" is "[Gmail]/Sent Mail" on Gmail).
std::string mail_imap_folder_name(const MailAccount& acc, const std::string& local_folder);
// The OAuth provider whose sign-in serves an account ("gmail" -> "google").
std::string mail_oauth_provider(const MailAccount& acc);

class MailStore {
 public:
  // `host` is the machine's boundary (sockets, TLS trust store, randomness, the clock): without one the
  // store still reads and writes its files, but cannot reach a server. `kernel_root` is the machine's root
  // (where the OAuth vault and the TLS trust store live); it defaults to the directory above `root_dir`.
  explicit MailStore(const std::string& root_dir = "", hal::Host* host = nullptr, const std::string& kernel_root = "");
  void set_host(hal::Host* host, const std::string& kernel_root = "");
  ~MailStore() = default;

  void reload();
  void save();

  // Storage paths & engine info
  std::string envelope_db_path() const { return root_dir_ + "/envelope.db"; }
  std::string keyring_vault_path() const { return root_dir_ + "/keyring.vault"; }
  std::string accounts_idx_path() const { return root_dir_ + "/accounts.idx"; }
  std::string message_store_dir() const { return root_dir_ + "/store"; }

  // RFC 5322 MIME & EML engine
  static std::string serialize_rfc5322(const MailMessage& m);
  static bool parse_rfc5322(const std::string& raw_eml, MailMessage& out);

  std::vector<MailFolder> get_folders(const std::string& account_id = "all") const;
  std::vector<MailMessage> get_messages(const std::string& folder_id = "inbox",
                                        const std::string& search_query = "",
                                        int filter_mode = 0,
                                        const std::string& account_id = "all") const;

  const MailMessage* get_message(const std::string& id) const;
  MailMessage* get_message_mut(const std::string& id);

  bool mark_read(const std::string& id, bool read = true);
  bool toggle_flag(const std::string& id);
  bool move_to_folder(const std::string& id, const std::string& target_folder);
  bool delete_message(const std::string& id);
  bool archive_message(const std::string& id);

  std::string send_message(const std::string& from_addr,
                           const std::vector<std::string>& to_addrs,
                           const std::vector<std::string>& cc_addrs,
                           const std::string& subject,
                           const std::string& body,
                           const std::vector<MailAttachment>& attachments = {},
                           const std::string& account_id = "");

  std::string save_draft(const std::string& draft_id,
                         const std::string& to_str,
                         const std::string& cc_str,
                         const std::string& subject,
                         const std::string& body,
                         const std::string& account_id = "");

  size_t total_unread(const std::string& account_id = "all") const;
  // Bumped whenever the message set changes. A caller that renders this store
  // every frame -- the desktop's mail pane does -- uses it to know when its
  // cached view is stale, instead of rebuilding a copy of the whole mailbox
  // sixty times a second.
  uint64_t generation() const { return generation_; }
  size_t folder_unread(const std::string& folder_id, const std::string& account_id = "all") const;

  // Account management
  const std::vector<MailAccount>& accounts() const { return accounts_; }
  std::vector<MailAccount>& accounts_mut() { return accounts_; }
  const MailAccount& primary_account() const;
  const MailAccount* get_account(const std::string& id_or_email) const;
  MailAccount* get_account_mut(const std::string& id_or_email);
  bool add_account(const MailAccount& acc);
  bool update_account(const MailAccount& acc);
  bool remove_account(const std::string& id_or_email);
  bool set_default_account(const std::string& id_or_email);

  // Network & diagnostics
  static Result<std::string> probe_endpoint(const std::string& host, uint16_t port, int timeout_ms = 2500);
  Result<std::string> test_account_connection(const MailAccount& acc);

  // Built from what actually happened -- the newest per-account sync time, the
  // unread count, and any sync that failed. Never a fixed string: this used to
  // read "Updated just now" whether or not a byte had moved.
  // The sender's own html for a message, as they wrote it, or empty when the
  // message is plain text or its body has been vacuumed.
  std::string html_body(const MailMessage& m) const;

  std::string status_line() const;
  void set_status_line(const std::string& st) { transient_status_ = st; }
  void clear_status_line() { transient_status_.clear(); }

  // Blocking sync. Fine for the CLI; on the desktop it would freeze the frame
  // loop for as long as the IMAP fetch takes, so the window uses the pair below.
  Result<void> check_new_mail(const std::string& account_id = "all");

  // Delete cached bodies past the retention window, then oldest-accessed first
  // until the cache is under its cap. Envelopes are never touched: the index is
  // ~290 bytes a message and is what makes the whole archive searchable.
  // Returns how many .eml files were removed. Purely local, so bare metal has
  // it too.
  size_t vacuum_bodies();
  void set_body_retention(int days, uint64_t cap_bytes) {
    body_retain_days_ = days;
    body_cache_cap_bytes_ = cap_bytes;
  }
  int body_retain_days() const { return body_retain_days_; }
  uint64_t body_cache_cap_bytes() const { return body_cache_cap_bytes_; }
  const std::vector<MailFolderSync>& sync_state() const { return sync_state_; }

  // Pull one message's body back from the server, for a body the vacuum has
  // since evicted. Blocking, and meant for opening a single message. On bare
  // metal, where there is no python and no socket to run it over, this reports
  // that rather than being absent -- the command surface is the same build.
  Result<size_t> fetch_body(const std::string& message_id);
  // Ask the server to search. Covers mail we hold no envelope for at all --
  // on Gmail this is X-GM-RAW over All Mail, so the full Gmail query language
  // works. Results are a view of the server, not merged into the local store.
  Result<std::vector<MailMessage>> search_server(const std::string& query,
                                                 const std::string& account_id = "",
                                                 int limit = 200);
#if !defined(AIOS_METAL)
  // The same fetch off the frame thread, for the window: kick it, keep
  // rendering, and poll_background_sync() folds the body in when it lands.
  // Returns false if that message needs no fetch or one is already in flight.
  bool begin_background_body_fetch(const std::string& message_id);
  bool background_body_pending(const std::string& message_id) const;
  // Start a sync on a background thread. The thread touches no member of this
  // object -- it works from copies and hands back plain results -- so the
  // thread that owns the store can keep rendering from it meanwhile.
  // Returns false if a sync is already in flight.
  bool begin_background_sync(const std::string& account_id = "all");
  // Called by the store's owning thread. If a background sync has finished,
  // fold its results in (statuses, timestamps, new messages) and save.
  // Returns true if it merged a completed sync this call.
  bool poll_background_sync();
  bool background_sync_running() const { return sync_running_.load(); }
#endif

 private:
  std::string root_dir_;
  std::vector<MailAccount> accounts_;
  std::vector<MailMessage> messages_;
  std::string transient_status_;
  uint64_t next_id_num_ = 1000;
  bool loaded_from_disk_ = false;
  uint64_t generation_ = 1;
  // total_unread() walks every message, and the status line asks for it on
  // every frame. Memoised against the generation.
  mutable uint64_t unread_cache_generation_ = 0;
  mutable size_t unread_cache_ = 0;

  // Fold one account's freshly fetched inbox (the JSON the sync script wrote)
  // into messages_, matching on RFC 5322 Message-ID so a re-sync updates a
  // message rather than duplicating it -- and so it touches no other account.
  size_t merge_records(const MailAccount& acc, const std::string& folder,
                       const std::vector<MailFetchRecord>& records, const std::string& raw_dir);
  void load_sync_state();
  void save_sync_state() const;
  MailFolderSync& sync_state_for(const std::string& account_id, const std::string& folder);
  std::vector<MailFolderSync> sync_state_;
  // Bodies are the only part of the store that grows without bound: the
  // envelope index for a 49.5k message inbox is 14MB and opens in 0.17s, while
  // its bodies are 312MB. So keep every envelope and vacuum the bodies.
  int body_retain_days_ = 90;
  uint64_t body_cache_cap_bytes_ = 250ull * 1024 * 1024;
  // Messages to pull on the first, deep pass. 0 means the whole mailbox.
  int backfill_limit_ = 0;
  hal::Host* host_ = nullptr;
  std::string kernel_root_;
  mutable aios::hal::Mutex oauth_mu_;   // one token refresh at a time: it reads and rewrites the vault

  // One fetch against the server, natively (src/net/imap.cpp). Safe to call from a worker thread.
  MailSyncOutcome run_job(const MailSyncJob& job);
  // A fresh access token for an OAuth account, refreshed through the provider when it is about to lapse.
  Result<std::string> oauth_access_token(const std::string& provider, const std::string& account);
  // Deliver one message over SMTP; the text for the status line says how it went.
  std::string deliver(const MailAccount& acc, const MailMessage& m);
  std::vector<MailSyncJob> build_sync_jobs(const std::string& account_id);
  MailSyncJob job_for_message(const MailAccount& acc, const MailMessage& m) const;
  void apply_sync_outcomes(const std::vector<MailSyncOutcome>& outcomes);
#if !defined(AIOS_METAL)
  std::atomic<bool> sync_running_{false};
  std::atomic<bool> sync_done_{false};
  mutable aios::hal::Mutex sync_mu_;
  std::vector<MailSyncOutcome> sync_results_;
  std::string body_fetch_id_;   // message whose body is in flight, if any
#endif

  void load_from_disk();
  void save_to_disk();
  void ensure_store_hierarchy();
  void write_eml_file(const MailMessage& m);
  bool read_eml_file(const std::string& path, MailMessage& m) const;
  // Load one message's body from its .eml if it is not already in memory.
  void ensure_body(const MailMessage& m) const;
  std::string sidecar_path(const MailMessage& m, const std::string& ext) const;
  void save_envelope_index();
  bool load_envelope_index();
  void save_accounts_and_vault();
  bool load_accounts_and_vault();
  void migrate_legacy_json();
};

}  // namespace aios::userland
