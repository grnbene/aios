#pragma once
// IMAP4rev1 (RFC 3501) client over the kernel's own sockets and TLS. UIDs only for message commands,
// literals both ways, SASL XOAUTH2 (Google, Microsoft) as well as LOGIN and PLAIN, modified UTF-7
// mailbox names. Nothing is shelled out to; it builds for the bare-metal image too (no exceptions).

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/net/line_stream.hpp"

namespace aios::net {

// A datum out of a response: an atom, a string (quoted or literal, contents decoded), NIL, or a list.
struct ImapVal {
  enum class T : uint8_t { Nil, Atom, Str, List };
  T t = T::Nil;
  std::string s;
  std::vector<ImapVal> l;

  bool is_list() const { return t == T::List; }
  bool is_nil() const { return t == T::Nil; }
  bool is_text() const { return t == T::Atom || t == T::Str; }
  uint64_t num() const;                                     // an atom or string read as a number; 0 if it is not one
  // In a list of alternating name, value, ... (a FETCH answer): the value after `name`, or nullptr.
  const ImapVal* item(std::string_view name) const;
};

// One untagged response: "* 12 FETCH (...)", "* SEARCH 4 7", "* OK [UIDVALIDITY 3] ...".
struct ImapResponse {
  std::string kind;          // upper-cased: FETCH, SEARCH, EXISTS, OK, LIST, ...
  bool has_number = false;   // "* 12 EXISTS": the 12
  uint32_t number = 0;
  std::string code;          // for OK/NO/BAD/PREAUTH/BYE: the bracketed response code's name, upper-cased
  ImapVal code_arg;          // its first argument, when it has one
  std::string text;          // for OK/NO/BAD: the human text after the code
  std::vector<ImapVal> args; // everything else on the line, in order
};

struct ImapResult {
  std::string status;                   // OK, NO or BAD
  std::string code;                     // response code of the tagged line, upper-cased, if any
  ImapVal code_arg;                     // its first argument
  std::vector<ImapVal> code_more;       // the rest ([APPENDUID 38505 3955]: 38505 is code_arg, 3955 here)
  std::string text;
  std::vector<ImapResponse> untagged;
  bool ok() const { return status == "OK"; }
};

struct ImapSelect {
  uint32_t exists = 0, recent = 0, uidnext = 0, uidvalidity = 0;
  uint64_t highestmodseq = 0;
  bool read_only = false;
  std::vector<std::string> flags;
};

struct ImapFetched {
  uint32_t seq = 0;
  uint32_t uid = 0;
  bool has_flags = false;
  std::vector<std::string> flags;
  std::string internaldate;                         // as the server wrote it: "17-Jul-1996 02:44:25 -0700"
  uint64_t size = 0;                                // RFC822.SIZE
  std::map<std::string, std::string> sections;      // "BODY[]", "BODY[HEADER.FIELDS (FROM TO)]" -> bytes (name upper-cased, <partial> dropped)
  std::string gm_msgid, gm_thrid;                   // Gmail extensions, when asked for
  std::vector<std::string> gm_labels;
};

struct ImapMailbox {
  std::string name;                    // UTF-8
  std::string delimiter;
  std::vector<std::string> attrs;      // \Noselect, \Sent, \Drafts, \Trash, \All, \Junk, \Flagged ...
  bool has_attr(std::string_view a) const;
};

// A string as an IMAP quoted string (backslash and quote escaped). Text with CR, LF, NUL or 8-bit bytes
// needs a literal instead; imap_needs_literal says so.
std::string imap_quote(std::string_view s);
bool imap_needs_literal(std::string_view s);
// Mailbox names on the wire are modified UTF-7 (RFC 3501 5.1.3).
std::string imap_utf7_encode(std::string_view utf8);
std::string imap_utf7_decode(std::string_view wire);
// "17-Jul-1996 02:44:25 -0700" to epoch seconds; false when it is not that.
bool imap_parse_internaldate(std::string_view text, int64_t& epoch);

class ImapClient {
 public:
  ImapClient(hal::Host& host, std::string kernel_root, int timeout_ms = 30000, std::string trust_store = {});

  Result<void> connect(const std::string& host, uint16_t port, Security security);
  Result<void> login(const std::string& user, const std::string& password);
  // SASL PLAIN when the server offers it without LOGINDISABLED.
  Result<void> authenticate_plain(const std::string& user, const std::string& password);
  // The SASL XOAUTH2 mechanism: Google and Microsoft accept an OAuth2 access token this way.
  Result<void> authenticate_xoauth2(const std::string& user, const std::string& access_token);
  bool has_capability(std::string_view name) const;
  const std::vector<std::string>& capabilities() const { return caps_; }

  Result<ImapSelect> select(const std::string& mailbox_utf8, bool read_only);
  Result<uint32_t> uidvalidity(const std::string& mailbox_utf8);   // STATUS
  Result<std::vector<uint32_t>> uid_search(const std::string& criteria);   // criteria exactly as the protocol writes them
  // `set` like "5", "4:9,12", "1:*"; `items` like "(UID FLAGS BODY.PEEK[])".
  Result<std::vector<ImapFetched>> uid_fetch(const std::string& set, const std::string& items);
  // mode: "+FLAGS", "-FLAGS" or "FLAGS"; flags like "\\Seen \\Flagged".
  Result<void> uid_store(const std::string& set, const std::string& mode, const std::string& flags);
  // The new message's UID when the server says (UIDPLUS), else 0.
  Result<uint32_t> append(const std::string& mailbox_utf8, const std::string& flags, const std::string& message);
  Result<void> uid_copy(const std::string& set, const std::string& mailbox_utf8);
  Result<void> uid_expunge(const std::string& set);   // needs UIDPLUS; otherwise EXPUNGE of everything \Deleted
  Result<void> expunge();
  Result<std::vector<ImapMailbox>> list(const std::string& reference = "", const std::string& pattern = "*");
  Result<void> create_mailbox(const std::string& mailbox_utf8);
  Result<void> noop();
  Result<void> logout();
  void close() { stream_.close(); }

  // For anything not wrapped above. `args` are sent as written, after the tag and command word.
  Result<ImapResult> command(const std::string& line);

  // A short description of the last failure from the server (the tagged NO/BAD text).
  const std::string& last_server_text() const { return last_text_; }

 private:
  Result<ImapResult> run(const std::string& line, const std::string& literal = {}, bool literal_follows = false);
  Result<ImapResponse> read_untagged(const std::string& line);
  Result<void> after_auth(ImapResult& r);
  Result<void> check(const Result<ImapResult>& r, const char* what);
  void note_caps(const ImapResult& r);
  std::string next_tag();

  LineStream stream_;
  std::vector<std::string> caps_;
  uint32_t tag_ = 0;
  std::string last_text_;
};

}  // namespace aios::net
