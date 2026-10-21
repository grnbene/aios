#pragma once
// ssh_config(5), read the way OpenSSH reads it, so a name the person uses with
// `ssh` (an alias from ~/.ssh/config, its HostName, User, Port, IdentityFile)
// means the same thing to this machine's own client.
//
// Semantics are OpenSSH's: the user file then the system file, each read top
// to bottom; for each keyword the FIRST value obtained wins (IdentityFile,
// CertificateFile and the other list keywords accumulate instead); Host
// patterns take `*`, `?` and `!` negation, comma- or space-separated; Match
// takes all, host, originalhost, user, localuser, exec, canonical and final;
// Include takes globs, relative to ~/.ssh for the user file and /etc/ssh for
// the system one; tokens (%h %p %r %u %d %l %L %n %C %i %%) and ~ are expanded
// where ssh_config(5) says they are.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"

namespace aios::net {

struct SshConfigFiles {
  std::string user_file;     // "" = ~/.ssh/config
  std::string system_file;   // "" = /etc/ssh/ssh_config (on Windows, %PROGRAMDATA%/ssh/ssh_config)
  std::string home;          // "" = $HOME (on Windows, %USERPROFILE%)
  std::string local_user;    // "" = the account running this process
  bool read_system = true;
};

// Everything the client acts on, resolved for one destination.
struct SshResolved {
  std::string original_host;        // what was asked for ("aar")
  std::string hostname;             // HostName, tokens expanded; else original_host
  std::string user;                 // User; else the URL's user; else the local account
  uint16_t port = 22;
  std::vector<std::string> identity_files;   // expanded paths, in order; the defaults when none given
  bool identity_files_given = false;         // at least one IdentityFile came from a config file
  bool identities_only = false;
  std::vector<std::string> certificate_files;
  std::string identity_agent;       // IdentityAgent: a socket path, "SSH_AUTH_SOCK", or "none"; "" = $SSH_AUTH_SOCK
  std::vector<std::string> user_known_hosts_files;     // defaults: ~/.ssh/known_hosts ~/.ssh/known_hosts2
  std::vector<std::string> global_known_hosts_files;   // defaults: /etc/ssh/ssh_known_hosts /etc/ssh/ssh_known_hosts2
  std::string strict_host_key_checking = "ask";        // yes | no | ask | accept-new | off
  bool hash_known_hosts = false;
  bool check_host_ip = false;
  std::string host_key_alias;
  std::vector<std::string> host_key_algorithms;        // in preference order; empty = the client's own
  std::vector<std::string> pubkey_accepted_algorithms; // empty = the client's own
  std::vector<std::string> ciphers, kex_algorithms, macs;
  std::vector<std::string> proxy_jump;                 // each "[user@]host[:port]"; empty or "none" = direct
  std::string proxy_command;                           // "" or "none" = none
  int connect_timeout_s = 0;                           // 0 = none
  int server_alive_interval_s = 0;
  int server_alive_count_max = 3;
  int connection_attempts = 1;
  bool batch_mode = false;                             // never prompt
  bool pubkey_authentication = true;
  bool password_authentication = true;
  bool kbd_interactive_authentication = true;
  std::vector<std::string> preferred_authentications;  // empty = the client's own order
  std::string address_family = "any";                  // any | inet | inet6
  std::string bind_address;
  std::vector<std::pair<std::string, std::string>> set_env;  // SetEnv
  std::vector<std::string> send_env;                         // SendEnv patterns
  std::string log_level = "INFO";
};

// Resolve `host` (the name as written in a URL or on a command line) through the config files.
// `user_from_url` and `port_from_url` (0 = none) take precedence over the files, as on ssh's command line.
// An unreadable or malformed file is an error naming the file and line, as ssh reports it.
Result<SshResolved> ssh_resolve(hal::Host& host, const std::string& name, const std::string& user_from_url,
                                uint16_t port_from_url, const SshConfigFiles& files = {});

// The client's default identity files, in the order OpenSSH tries them (id_rsa, id_ecdsa, id_ecdsa_sk,
// id_ed25519, id_ed25519_sk, id_xmss, id_dsa under <home>/.ssh).
std::vector<std::string> ssh_default_identity_files(const std::string& home);

// `pattern` (one Host/Match pattern list, comma-separated, `!` negation) against `name`, as match_pattern_list does.
bool ssh_match_pattern_list(const std::string& name, const std::string& patterns);

}  // namespace aios::net
