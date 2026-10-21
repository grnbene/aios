#pragma once
// SMTP submission (RFC 5321, 4954, 3207) over the kernel's own sockets and TLS: EHLO, STARTTLS,
// AUTH PLAIN / LOGIN / XOAUTH2, MAIL/RCPT/DATA with dot-stuffing and CRLF normalisation. Nothing is
// shelled out to; it builds for the bare-metal image too (no exceptions).

#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/net/line_stream.hpp"

namespace aios::net {

struct SmtpReply {
  int code = 0;
  std::string text;   // the lines joined with '\n', codes stripped
};

class SmtpClient {
 public:
  SmtpClient(hal::Host& host, std::string kernel_root, int timeout_ms = 30000, std::string trust_store = {});

  // Greeting, EHLO, and for StartTls the upgrade and a second EHLO.
  Result<void> connect(const std::string& host, uint16_t port, Security security,
                       const std::string& client_name = "aios.local");
  bool has_extension(std::string_view name) const;
  bool has_mechanism(std::string_view mech) const;   // an AUTH mechanism the server listed: PLAIN, LOGIN, XOAUTH2

  Result<void> authenticate_plain(const std::string& user, const std::string& password);
  Result<void> authenticate_login(const std::string& user, const std::string& password);
  Result<void> authenticate_xoauth2(const std::string& user, const std::string& access_token);
  // Whatever the server offers, strongest first: XOAUTH2 is not tried here (it needs a token, not a password).
  Result<void> authenticate(const std::string& user, const std::string& password);

  // `message` is the RFC 5322 text, headers and body; line endings are normalised to CRLF and leading dots
  // doubled. `from` and each recipient are bare addresses.
  Result<void> send(const std::string& from, const std::vector<std::string>& recipients, const std::string& message);
  Result<void> quit();
  void close() { stream_.close(); }
  const std::string& last_reply() const { return last_.text; }

 private:
  Result<SmtpReply> read_reply();
  Result<SmtpReply> command(const std::string& line);
  Result<void> ehlo();
  Result<void> auth_result(const Result<SmtpReply>& r, const char* what);

  LineStream stream_;
  std::vector<std::string> ext_;
  std::string client_name_;
  SmtpReply last_;
};

}  // namespace aios::net
