#pragma once
// Mailers for ASQL: a named way to send email, so a Cells act can mail someone when a row arrives
// (sql_cells.hpp: ON RISE <bit> MAIL <mailer> TO '...' SUBJECT '...' BODY '...').
//
//   CREATE MAILER <name> OAUTH '<provider>' ACCOUNT '<email>' [HOST '<smtp host>'] [PORT <n>]
//                        [SECURITY 'starttls'|'tls'|'none'] [FROM '<address>'] [FROM_NAME '<name>']
//   CREATE MAILER <name> USER '<user>' PASSWORD SECRET <secret> HOST '<smtp host>' [PORT <n>]
//                        [SECURITY ...] FROM '<address>' [FROM_NAME '<name>']
//   CREATE MAILER <name> HOST '127.0.0.1' [PORT <n>] FROM '<address>'      (a relay on this machine, no sign-in)
//   DROP MAILER <name>
//   SHOW MAILERS                     name, server, how it signs in, from; never a password or token
//
// OAUTH signs in with the account the machine already signed in to (`aios google auth <email>`, which keeps the
// tokens in <root>/oauth); each send asks for a live access token and signs in with SASL XOAUTH2. For provider
// 'google' the server defaults to smtp.gmail.com:587 and FROM to the account. PASSWORD SECRET names a
// CREATE SECRET value (sql_secrets.hpp), opened only to send. A password or token is never sent without TLS to
// another machine. Declarations are kept in <store>/mailers/<name>.mailer; they name a secret, never hold one.
#include <mutex>
#include <string>
#include <vector>

#include "aios/net/mail_message.hpp"
#include "aios/userland/sql_secrets.hpp"

namespace aios::hal { class Host; }

namespace aios::userland {

class MailerStore {
 public:
  struct Mailer {
    std::string name, host, security, oauth_provider, oauth_account, user, password_secret, from, from_name;
    int port = 587;
    std::string declaration, created;
    std::string auth_text() const;   // "oauth google you@x", "password (secret s)", "none"
  };

  MailerStore(aios::hal::Host& host, std::string kernel_root, std::string dir, SecretStore& secrets)
      : host_(host), kernel_root_(std::move(kernel_root)), dir_(std::move(dir)), secrets_(secrets) {}

  // "CREATE MAILER", "DROP MAILER" or "SHOW MAILERS" (the first two words, upper case).
  static bool handles(const std::string& head_two_words);

  void load();   // the declarations on disk; one that no longer parses is skipped and reported on stderr
  bool create(const std::string& sql, std::string* err);
  bool drop(const std::string& name, std::string* err);
  bool has(const std::string& name);
  std::vector<Mailer> list();

  // Builds the message (from and from_name default to the mailer's) and sends it. False with *err on any
  // failure; on success *message_id is the Message-ID.
  bool send(const std::string& name, aios::net::MailParts parts, std::string* message_id, std::string* err);

 private:
  bool parse(const std::string& sql, Mailer* m, std::string* err);
  void save(const Mailer& m);

  aios::hal::Host& host_;
  std::string kernel_root_, dir_;
  SecretStore& secrets_;
  std::mutex mu_;
  std::vector<Mailer> mailers_;
};

}  // namespace aios::userland
