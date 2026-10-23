// ssh_config(5), read as OpenSSH's readconf.c reads it. The file(s) are taken
// top to bottom; a Host or Match line decides whether the lines after it apply;
// for most keywords the first value obtained is kept, and the few list keywords
// (IdentityFile, CertificateFile, SendEnv, SetEnv) accumulate. A command-line
// user or port (here: the URL's) is obtained before any file, so it wins. When a
// Match final is seen, the files are read a second time with final true, the
// values already obtained kept. Paths and tokens are expanded after reading, with
// the final host name, port and user, as ssh does when it uses them.
#include "aios/net/ssh_config.hpp"
#include "aios/core/numparse.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "aios/crypto/sha1.hpp"
#include "aios/hal/account.hpp"
#include "aios/hal/process.hpp"
#include "aios/hal/sync.hpp"

namespace aios::net {
namespace {

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// fnmatch(3) without flags beyond what ssh needs: * ? and [...] classes ([!...] negates). `nocase` for host names.
bool glob(const std::string& pat, size_t p, const std::string& s, size_t i, bool nocase) {
  auto eq = [&](char a, char b) { return nocase ? std::tolower((unsigned char)a) == std::tolower((unsigned char)b) : a == b; };
  while (p < pat.size()) {
    char c = pat[p];
    if (c == '*') {
      while (p < pat.size() && pat[p] == '*') ++p;
      if (p == pat.size()) return true;
      for (size_t k = i; k <= s.size(); ++k)
        if (glob(pat, p, s, k, nocase)) return true;
      return false;
    }
    if (i >= s.size()) return false;
    if (c == '?') { ++p; ++i; continue; }
    if (c == '[') {
      size_t q = p + 1;
      bool neg = q < pat.size() && (pat[q] == '!' || pat[q] == '^');
      if (neg) ++q;
      bool hit = false, closed = false;
      for (size_t first = q; q < pat.size(); ++q) {
        if (pat[q] == ']' && q > first) { closed = true; break; }
        if (q + 2 < pat.size() && pat[q + 1] == '-' && pat[q + 2] != ']') {
          char lo = pat[q], hi = pat[q + 2], ch = s[i];
          if (nocase) { lo = (char)std::tolower((unsigned char)lo); hi = (char)std::tolower((unsigned char)hi); ch = (char)std::tolower((unsigned char)ch); }
          if (ch >= lo && ch <= hi) hit = true;
          q += 2;
        } else if (eq(pat[q], s[i])) {
          hit = true;
        }
      }
      if (!closed) { if (!eq('[', s[i])) return false; ++p; ++i; continue; }
      if (hit == neg) return false;
      p = q + 1;
      ++i;
      continue;
    }
    if (!eq(c, s[i])) return false;
    ++p;
    ++i;
  }
  return i == s.size();
}

// The arguments of a config line, as OpenSSH's argv_split takes them: whitespace-separated, "double" or 'single'
// quoted words keep their spaces, a backslash escapes the next character, and an unquoted word starting with '#'
// ends the line. False for an unterminated quote.
bool split_args(const std::string& s, std::vector<std::string>& out) {
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && is_space(s[i])) ++i;
    if (i >= s.size()) break;
    if (s[i] == '#') break;
    std::string w;
    char quote = 0;
    while (i < s.size()) {
      char c = s[i];
      if (quote) {
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == quote || s[i + 1] == '\\')) { w += s[i + 1]; i += 2; continue; }
        if (c == quote) { quote = 0; ++i; continue; }
        w += c;
        ++i;
        continue;
      }
      if (is_space(c)) break;
      if (c == '"' || c == '\'') { quote = c; ++i; continue; }
      if (c == '\\' && i + 1 < s.size()) { w += s[i + 1]; i += 2; continue; }
      w += c;
      ++i;
    }
    if (quote) return false;
    out.push_back(w);
  }
  return true;
}

std::vector<std::string> split_on(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(sep, i);
    if (j == std::string::npos) j = s.size();
    out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

// convtime(3): 90, 1m30s, 2h, 1d, 1w. -1 for anything else.
long convtime(const std::string& s) {
  if (s.empty()) return -1;
  long total = 0, cur = 0;
  bool digits = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); digits = true; if (cur > 100000000L) return -1; continue; }
    if (!digits) return -1;
    long mult;
    switch (c | 32) {
      case 's': mult = 1; break;
      case 'm': mult = 60; break;
      case 'h': mult = 3600; break;
      case 'd': mult = 86400; break;
      case 'w': mult = 604800; break;
      default: return -1;
    }
    total += cur * mult;
    cur = 0;
    digits = false;
  }
  return total + cur;
}

// The algorithm names OpenSSH accepts in each list (a name it does not know is a configuration error there, and
// here). This client offers only those it implements; the session filters the rest out.
const std::set<std::string>& known_key_algorithms() {
  static const std::set<std::string> s = {
      "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256",
      "ssh-rsa", "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com", "ssh-ed25519-cert-v01@openssh.com",
      "ecdsa-sha2-nistp256-cert-v01@openssh.com", "ecdsa-sha2-nistp384-cert-v01@openssh.com",
      "ecdsa-sha2-nistp521-cert-v01@openssh.com", "sk-ssh-ed25519-cert-v01@openssh.com",
      "sk-ecdsa-sha2-nistp256-cert-v01@openssh.com", "rsa-sha2-512-cert-v01@openssh.com",
      "rsa-sha2-256-cert-v01@openssh.com", "ssh-rsa-cert-v01@openssh.com", "webauthn-sk-ecdsa-sha2-nistp256@openssh.com"};
  return s;
}
const std::set<std::string>& known_ciphers() {
  static const std::set<std::string> s = {"chacha20-poly1305@openssh.com", "aes128-ctr", "aes192-ctr", "aes256-ctr",
                                          "aes128-gcm@openssh.com", "aes256-gcm@openssh.com", "aes128-cbc", "aes192-cbc",
                                          "aes256-cbc", "3des-cbc"};
  return s;
}
const std::set<std::string>& known_kex() {
  static const std::set<std::string> s = {"mlkem768x25519-sha256", "sntrup761x25519-sha512", "sntrup761x25519-sha512@openssh.com",
                                          "curve25519-sha256", "curve25519-sha256@libssh.org", "ecdh-sha2-nistp256",
                                          "ecdh-sha2-nistp384", "ecdh-sha2-nistp521", "diffie-hellman-group-exchange-sha256",
                                          "diffie-hellman-group-exchange-sha1", "diffie-hellman-group16-sha512",
                                          "diffie-hellman-group18-sha512", "diffie-hellman-group14-sha256",
                                          "diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1"};
  return s;
}
const std::set<std::string>& known_macs() {
  static const std::set<std::string> s = {"hmac-sha2-256", "hmac-sha2-512", "hmac-sha1", "umac-64@openssh.com",
                                          "umac-128@openssh.com", "hmac-sha2-256-etm@openssh.com",
                                          "hmac-sha2-512-etm@openssh.com", "hmac-sha1-etm@openssh.com",
                                          "umac-64-etm@openssh.com", "umac-128-etm@openssh.com"};
  return s;
}
// This client's own defaults, which a +, - or ^ list modifies (they are what the session offers when nothing is set).
const std::vector<std::string>& client_defaults(const std::string& kw) {
  static const std::vector<std::string> hk = {"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
                                              "rsa-sha2-512", "rsa-sha2-256"};
  static const std::vector<std::string> ci = {"chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com", "aes128-gcm@openssh.com"};
  static const std::vector<std::string> kx = {"curve25519-sha256", "curve25519-sha256@libssh.org", "ecdh-sha2-nistp256",
                                              "ecdh-sha2-nistp384", "ecdh-sha2-nistp521"};
  static const std::vector<std::string> mc = {"hmac-sha2-256"};
  if (kw == "ciphers") return ci;
  if (kw == "kexalgorithms") return kx;
  if (kw == "macs") return mc;
  return hk;
}

// One file's worth of the options, every one "not yet obtained" until a line gives it.
struct Options {
  std::optional<std::string> hostname, user, identity_agent, strict, host_key_alias, proxy_command, address_family,
      bind_address, log_level, tag, ignore_unknown;
  std::optional<uint16_t> port;
  std::optional<bool> identities_only, hash_known_hosts, check_host_ip, batch_mode, pubkey, password, kbd;
  std::optional<long> connect_timeout, alive_interval, alive_count, attempts;
  std::optional<std::vector<std::string>> user_known_hosts, global_known_hosts, proxy_jump, host_key_algorithms,
      pubkey_algorithms, ciphers, kex, macs, preferred_auth;
  std::vector<std::string> identity_files, certificate_files, send_env;
  std::vector<std::pair<std::string, std::string>> set_env;
};

// Keywords ssh_config(5) has that change nothing this client does: accepted (their value is still checked by
// OpenSSH, not here), never refused.
const std::set<std::string>& accepted_keywords() {
  static const std::set<std::string> s = {
      "addkeystoagent", "canonicaldomains", "canonicalizefallbacklocal", "canonicalizehostname",
      "canonicalizemaxdots", "canonicalizepermittedcnames", "casignaturealgorithms", "channeltimeout",
      "clearallforwardings", "compression", "controlmaster", "controlpath", "controlpersist", "dynamicforward",
      "enableescapecommandline", "enablesshkeysign", "escapechar", "exitonforwardfailure", "fingerprinthash",
      "forkafterauthentication", "forwardagent", "forwardx11", "forwardx11timeout", "forwardx11trusted", "gatewayports",
      "gssapiauthentication", "gssapidelegatecredentials", "hostbasedacceptedalgorithms", "hostbasedacceptedkeytypes",
      "hostbasedauthentication", "hostbasedkeytypes", "ipqos", "kbdinteractivedevices", "knownhostscommand",
      "localcommand", "localforward", "logverbose", "nohostauthenticationforlocalhost", "numberofpasswordprompts",
      "obscurekeystroketiming", "permitlocalcommand", "permitremoteopen", "pkcs11provider", "proxyusefdpass",
      "refuseconnection", "rekeylimit", "remotecommand", "remoteforward", "requesttty", "requiredrsasize",
      "revokedhostkeys", "securitykeyprovider", "sessiontype", "stdinnull", "streamlocalbindmask",
      "streamlocalbindunlink", "syslogfacility", "tcpkeepalive", "tunnel", "tunneldevice", "updatehostkeys",
      "usekeychain", "usedns", "verifyhostkeydns", "visualhostkey", "warnweakcrypto", "xauthlocation",
      // deprecated or removed: OpenSSH reads them and does nothing
      "cipher", "protocol", "rsaauthentication", "rhostsrsaauthentication", "rhostsauthentication", "fallbacktorsh",
      "usersh", "useprivilegedport", "useroaming", "compressionlevel", "globalknownhostsfile2", "userknownhostsfile2",
      "challengeresponseauthentication", "skeyauthentication", "tisauthentication", "kerberosauthentication",
      "kerberostgtpassing", "afstokenpassing", "dsaauthentication", "smartcarddevice", "gssapikeyexchange",
      "gssapiclientidentity", "gssapiserveridentity", "gssapirenewalforcesrekey", "gssapitrustdns"};
  return s;
}

struct Resolver {
  hal::Host& host;
  hal::LocalAccount account;
  std::string local_host;
  std::string host_arg;        // the name asked for, lowercased (what HostName's %h and the final name use)
  std::string original_host;   // the name as it was typed: Host lines match it case-sensitively, as ssh does
  Options o;
  bool final_pass = false;
  bool want_final = false;
  std::vector<std::string> notes;

  std::string where(const std::string& file, int line) const { return file + " line " + std::to_string(line); }

  std::string match_host() const {
    // Match host compares against HostName once one is obtained (with its %h expanded), else the name asked for
    if (o.hostname) return lower(replace_h(*o.hostname, host_arg));
    return host_arg;
  }
  static std::string replace_h(const std::string& v, const std::string& h) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '%' && i + 1 < v.size()) {
        if (v[i + 1] == 'h') { out += h; ++i; continue; }
        if (v[i + 1] == '%') { out += '%'; ++i; continue; }
      }
      out += v[i];
    }
    return out;
  }

  // match_pattern_list: true when a positive pattern matches and no negated one does
  static bool pattern_list(const std::string& name, const std::string& list, bool nocase) {
    bool got = false;
    for (auto& raw : split_on(list, ',')) {
      std::string p = raw;
      while (!p.empty() && is_space(p.front())) p.erase(p.begin());
      while (!p.empty() && is_space(p.back())) p.pop_back();
      if (p.empty()) continue;
      bool neg = p[0] == '!';
      if (neg) p.erase(p.begin());
      if (glob(p, 0, name, 0, nocase)) {
        if (neg) return false;
        got = true;
      }
    }
    return got;
  }

  Result<bool> run_exec(const std::string& command) {
    hal::ProcessSpawn spec;
    spec.argv = {"/bin/sh", "-c", command};
    auto h = hal::spawn_process(spec);
    if (!h.ok()) return Result<bool>(h.error().status, "Match exec \"" + command + "\": " + h.error().message);
    for (uint32_t waited = 0; waited < 300000; waited += 10) {
      auto st = hal::process_status(*h);
      if (!st.ok()) { (void)hal::release_process(*h); return Result<bool>(st.error()); }
      if (st->exited) {
        (void)hal::release_process(*h);
        return st->exit_code == 0 && st->terminating_signal == 0;
      }
      hal::sleep_ms(10);
    }
    (void)hal::release_process(*h);
    return Result<bool>(Status::Busy, "Match exec \"" + command + "\" did not finish in 300 s");
  }

  Result<bool> match_line(const std::vector<std::string>& args, const std::string& at) {
    using R = Result<bool>;
    if (args.empty()) return R(Status::Invalid, at + ": no Match criteria");
    bool result = true;
    size_t i = 0;
    bool saw_all = false, saw_other = false;
    while (i < args.size()) {
      std::string attrib = lower(args[i++]);
      bool negate = !attrib.empty() && attrib[0] == '!';
      if (negate) attrib.erase(attrib.begin());
      if (attrib == "all") {
        saw_all = true;
        if (negate) result = false;
        continue;
      }
      if (attrib == "canonical" || attrib == "final") {
        if (attrib == "final") want_final = want_final || !negate;
        bool r = final_pass;
        if (r == negate) result = false;
        continue;
      }
      saw_other = true;
      if (i >= args.size()) return R(Status::Invalid, at + ": Missing Match criteria for " + attrib);
      const std::string& arg = args[i++];
      bool r;
      if (attrib == "host") r = pattern_list(match_host(), arg, true);
      else if (attrib == "originalhost") r = pattern_list(original_host, arg, true);
      else if (attrib == "user") r = pattern_list(o.user ? *o.user : account.name, arg, false);
      else if (attrib == "localuser") r = pattern_list(account.name, arg, false);
      else if (attrib == "tagged") r = pattern_list(o.tag ? *o.tag : std::string(), arg, false);
      else if (attrib == "command") r = pattern_list(std::string(), arg, false);
      else if (attrib == "sessiontype") r = pattern_list("exec", arg, false);   // git runs a command on the far side
      else if (attrib == "version") r = pattern_list("AIOS_1.0", arg, false);
      else if (attrib == "exec") {
        auto ran = run_exec(expand_tokens(arg));
        if (!ran.ok()) return R(ran.error().status, at + ": " + ran.error().message);
        r = *ran;
      } else if (attrib == "localnetwork") {
        return R(Status::Unsupported, at + ": Match localnetwork needs each interface's netmask, which this machine's "
                                           "network listing does not give");
      } else {
        return R(Status::Invalid, at + ": Unsupported Match attribute " + attrib);
      }
      if (r == negate) result = false;
    }
    if (saw_all && saw_other) return R(Status::Invalid, at + ": 'all' cannot be combined with other Match attributes");
    return result;
  }

  // The tokens of ssh_config(5)'s TOKENS section, with the values obtained so far.
  std::string expand_tokens(const std::string& v) const {
    const std::string h = o.hostname ? lower(replace_h(*o.hostname, host_arg)) : host_arg;
    const std::string p = std::to_string(o.port ? *o.port : 22);
    const std::string r = o.user ? *o.user : account.name;
    std::string j;
    if (o.proxy_jump && !o.proxy_jump->empty()) for (auto& x : *o.proxy_jump) j += (j.empty() ? "" : ",") + x;
    std::string short_local = local_host.substr(0, local_host.find('.'));
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] != '%' || i + 1 >= v.size()) { out += v[i]; continue; }
      char c = v[++i];
      switch (c) {
        case '%': out += '%'; break;
        case 'h': out += h; break;
        case 'n': out += original_host; break;
        case 'p': out += p; break;
        case 'r': out += r; break;
        case 'u': out += account.name; break;
        case 'd': out += account.home; break;
        case 'i': out += std::to_string(account.uid); break;
        case 'l': out += local_host; break;
        case 'L': out += short_local; break;
        case 'j': out += j; break;
        case 'k': out += o.host_key_alias ? *o.host_key_alias : original_host; break;
        case 'C': {
          std::string in = local_host + h + p + r + j;
          auto d = crypto::sha1(ByteSpan((const uint8_t*)in.data(), in.size()));
          static const char* hx = "0123456789abcdef";
          for (uint8_t b : d) { out += hx[b >> 4]; out += hx[b & 15]; }
          break;
        }
        default: out += '%'; out += c; break;
      }
    }
    return out;
  }
  std::string tilde(const std::string& p) const {
    if (p == "~") return account.home;
    if (p.size() >= 2 && p[0] == '~' && p[1] == '/') return account.home + p.substr(1);
    return p;
  }
  std::string path_of(const std::string& v) const { return tilde(expand_tokens(v)); }

  // A keyword and its arguments; `active` false still checks the line, as ssh does, but takes nothing from it.
  Result<void> apply(const std::string& kw, const std::vector<std::string>& args, const std::string& rest, bool active,
                     const std::string& at) {
    using R = Result<void>;
    auto need = [&](size_t n) -> R {
      if (args.size() < n) return R(Status::Invalid, at + ": missing argument");
      return Ok();
    };
    auto flag = [&](std::optional<bool>& slot) -> R {
      if (auto r = need(1); !r.ok()) return r;
      std::string v = lower(args[0]);
      bool b;
      if (v == "yes" || v == "true") b = true;
      else if (v == "no" || v == "false") b = false;
      else return R(Status::Invalid, at + ": unsupported option \"" + args[0] + "\".");
      if (active && !slot) slot = b;
      return Ok();
    };
    auto first = [&](std::optional<std::string>& slot) -> R {
      if (auto r = need(1); !r.ok()) return r;
      if (active && !slot) slot = args[0];
      return Ok();
    };
    auto number = [&](std::optional<long>& slot, bool time, bool allow_none) -> R {
      if (auto r = need(1); !r.ok()) return r;
      long n;
      if (allow_none && lower(args[0]) == "none") n = 0;
      else n = time ? convtime(args[0]) : (convtime(args[0]) >= 0 && args[0].find_first_not_of("0123456789") == std::string::npos ? (long)aios::num::to_ll(args[0]) : -1);
      if (n < 0) return R(Status::Invalid, at + ": invalid " + (time ? "time" : "number") + " value.");
      if (active && !slot) slot = n;
      return Ok();
    };
    auto algorithms = [&](std::optional<std::vector<std::string>>& slot, const std::set<std::string>& known) -> R {
      if (auto r = need(1); !r.ok()) return r;
      std::string spec = args[0];
      char mod = (spec[0] == '+' || spec[0] == '-' || spec[0] == '^') ? spec[0] : 0;
      std::string list = mod ? spec.substr(1) : spec;
      std::vector<std::string> names = split_on(list, ',');
      for (auto& n : names) {
        bool pattern = n.find_first_of("*?") != std::string::npos;
        if (n.empty() || (!pattern && !known.count(n)) || (pattern && mod != '-'))
          return R(Status::Invalid, at + ": Bad " + kw + " '" + spec + "'.");
      }
      if (!active || slot) return Ok();
      std::vector<std::string> out;
      const auto& base = client_defaults(kw);
      if (mod == '+') { out = base; for (auto& n : names) if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n); }
      else if (mod == '^') { out = names; for (auto& b : base) if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b); }
      else if (mod == '-') { for (auto& b : base) { bool drop = false; for (auto& n : names) if (glob(n, 0, b, 0, false)) drop = true; if (!drop) out.push_back(b); } }
      else out = names;
      slot = out;
      return Ok();
    };

    if (kw == "hostname") return first(o.hostname);
    if (kw == "user") return first(o.user);
    if (kw == "port") {
      if (auto r = need(1); !r.ok()) return r;
      long n = args[0].find_first_not_of("0123456789") == std::string::npos && args[0].size() <= 5 ? (long)aios::num::to_ll(args[0]) : -1;
      if (lower(args[0]) == "ssh") n = 22;
      if (n < 1 || n > 65535) return R(Status::Invalid, at + ": Bad port '" + args[0] + "'.");
      if (active && !o.port) o.port = (uint16_t)n;
      return Ok();
    }
    if (kw == "identityfile" || kw == "certificatefile") {
      if (auto r = need(1); !r.ok()) return r;
      auto& list = kw == "identityfile" ? o.identity_files : o.certificate_files;
      if (list.size() >= 100) return R(Status::Invalid, at + ": too many " + kw + " entries.");
      if (active && std::find(list.begin(), list.end(), args[0]) == list.end()) list.push_back(args[0]);
      return Ok();
    }
    if (kw == "identitiesonly") return flag(o.identities_only);
    if (kw == "hashknownhosts") return flag(o.hash_known_hosts);
    if (kw == "checkhostip") return flag(o.check_host_ip);
    if (kw == "batchmode") return flag(o.batch_mode);
    if (kw == "passwordauthentication") return flag(o.password);
    if (kw == "kbdinteractiveauthentication") return flag(o.kbd);
    if (kw == "pubkeyauthentication") {
      if (auto r = need(1); !r.ok()) return r;
      std::string v = lower(args[0]);
      if (v != "yes" && v != "no" && v != "true" && v != "false" && v != "unbound" && v != "host-bound")
        return R(Status::Invalid, at + ": unsupported option \"" + args[0] + "\".");
      if (active && !o.pubkey) o.pubkey = v != "no" && v != "false";
      return Ok();
    }
    if (kw == "stricthostkeychecking") {
      if (auto r = need(1); !r.ok()) return r;
      std::string v = lower(args[0]), s;
      if (v == "yes" || v == "true") s = "yes";
      else if (v == "no" || v == "false" || v == "off") s = "off";
      else if (v == "ask") s = "ask";
      else if (v == "accept-new") s = "accept-new";
      else return R(Status::Invalid, at + ": unsupported option \"" + args[0] + "\".");
      if (active && !o.strict) o.strict = s;
      return Ok();
    }
    if (kw == "identityagent") return first(o.identity_agent);
    if (kw == "hostkeyalias") return first(o.host_key_alias);
    if (kw == "tag") return first(o.tag);
    if (kw == "ignoreunknown") return first(o.ignore_unknown);
    if (kw == "bindaddress") return first(o.bind_address);
    if (kw == "addressfamily") {
      if (auto r = need(1); !r.ok()) return r;
      std::string v = lower(args[0]);
      if (v != "any" && v != "inet" && v != "inet6") return R(Status::Invalid, at + ": unsupported option \"" + args[0] + "\".");
      if (active && !o.address_family) o.address_family = v;
      return Ok();
    }
    if (kw == "loglevel") {
      if (auto r = need(1); !r.ok()) return r;
      static const std::set<std::string> levels = {"quiet", "fatal", "error", "info", "verbose", "debug", "debug1", "debug2", "debug3"};
      if (!levels.count(lower(args[0]))) return R(Status::Invalid, at + ": unsupported log level '" + args[0] + "'");
      if (active && !o.log_level) o.log_level = args[0];
      return Ok();
    }
    if (kw == "userknownhostsfile" || kw == "globalknownhostsfile") {
      if (auto r = need(1); !r.ok()) return r;
      auto& slot = kw == "userknownhostsfile" ? o.user_known_hosts : o.global_known_hosts;
      if (active && !slot) slot = args;
      return Ok();
    }
    if (kw == "proxyjump") {
      if (auto r = need(1); !r.ok()) return r;
      if (active && !o.proxy_jump) {
        if (lower(args[0]) == "none") o.proxy_jump = std::vector<std::string>();
        else o.proxy_jump = split_on(args[0], ',');
      }
      return Ok();
    }
    if (kw == "proxycommand") {
      if (rest.empty()) return R(Status::Invalid, at + ": missing argument");
      if (active && !o.proxy_command) o.proxy_command = lower(rest) == "none" ? std::string() : rest;
      return Ok();
    }
    if (kw == "connecttimeout") return number(o.connect_timeout, true, true);
    if (kw == "serveraliveinterval") return number(o.alive_interval, true, false);
    if (kw == "serveralivecountmax") return number(o.alive_count, false, false);
    if (kw == "connectionattempts") return number(o.attempts, false, false);
    if (kw == "preferredauthentications") {
      if (auto r = need(1); !r.ok()) return r;
      if (active && !o.preferred_auth) o.preferred_auth = split_on(args[0], ',');
      return Ok();
    }
    if (kw == "hostkeyalgorithms") return algorithms(o.host_key_algorithms, known_key_algorithms());
    if (kw == "pubkeyacceptedalgorithms" || kw == "pubkeyacceptedkeytypes") return algorithms(o.pubkey_algorithms, known_key_algorithms());
    if (kw == "ciphers") return algorithms(o.ciphers, known_ciphers());
    if (kw == "kexalgorithms") return algorithms(o.kex, known_kex());
    if (kw == "macs") return algorithms(o.macs, known_macs());
    if (kw == "sendenv") {
      if (auto r = need(1); !r.ok()) return r;
      if (!active) return Ok();
      for (auto& a : args) {
        if (!a.empty() && a[0] == '-') {
          std::string pat = a.substr(1);
          o.send_env.erase(std::remove_if(o.send_env.begin(), o.send_env.end(), [&](const std::string& e) { return glob(pat, 0, e, 0, false); }), o.send_env.end());
        } else {
          o.send_env.push_back(a);
        }
      }
      return Ok();
    }
    if (kw == "setenv") {
      if (auto r = need(1); !r.ok()) return r;
      for (auto& a : args) {
        size_t eq = a.find('=');
        if (eq == std::string::npos || eq == 0) return R(Status::Invalid, at + ": Invalid environment expression \"" + a + "\"");
        if (!active) continue;
        std::string name = a.substr(0, eq);
        bool have = false;
        for (auto& e : o.set_env) if (e.first == name) have = true;
        if (!have) o.set_env.push_back({name, a.substr(eq + 1)});
      }
      return Ok();
    }
    if (accepted_keywords().count(kw)) return Ok();
    if (o.ignore_unknown && pattern_list(kw, *o.ignore_unknown, true)) return Ok();
    return R(Status::Invalid, at + ": Bad configuration option: " + kw);
  }

  // Every path a glob names, sorted, as glob(3) gives them: wildcards in any component, a leading dot matched only
  // by a pattern that starts with one.
  std::vector<std::string> expand_glob(const std::string& pattern) {
    std::vector<std::string> parts = split_on(pattern, '/');
    std::vector<std::string> found = {pattern.size() && pattern[0] == '/' ? std::string("/") : std::string()};
    for (size_t k = 0; k < parts.size(); ++k) {
      const std::string& part = parts[k];
      if (part.empty()) continue;
      std::vector<std::string> next;
      bool wild = part.find_first_of("*?[") != std::string::npos;
      for (auto& base : found) {
        std::string dir = base.empty() ? "." : base;
        auto join = [&](const std::string& name) { return base.empty() ? name : (base == "/" ? "/" + name : base + "/" + name); };
        if (!wild) { next.push_back(join(part)); continue; }
        auto names = host.list_dir(dir);
        if (!names.ok()) continue;
        std::vector<std::string> sorted = *names;
        std::sort(sorted.begin(), sorted.end());
        for (auto& n : sorted) {
          if (n == "." || n == "..") continue;
          if (n[0] == '.' && part[0] != '.') continue;
          if (glob(part, 0, n, 0, false)) next.push_back(join(n));
        }
      }
      found = std::move(next);
    }
    std::vector<std::string> out;
    for (auto& f : found) if (host.exists(f)) out.push_back(f);
    std::sort(out.begin(), out.end());
    return out;
  }

  // OpenSSH refuses a user config that someone else could have written.
  Result<void> check_permissions(const std::string& path) {
#ifndef _WIN32
    auto st = host.stat(path);
    if (!st.ok()) return Ok();
    if ((st->uid != 0 && st->uid != account.uid) || (st->mode & 022) != 0)
      return Result<void>(Status::Denied, "Bad owner or permissions on " + path);
#else
    (void)path;
#endif
    return Ok();
  }

  // `user_conf`: a relative Include resolves under ~/.ssh (the user's file, or one given as -F), else /etc/ssh.
  Result<void> read_file(const std::string& path, bool user_conf, bool never_match, bool& active, int depth, bool check_perm) {
    if (depth > 16) return Result<void>(Status::Invalid, path + ": Include nested too deeply");
    auto bytes = host.read_file(path);
    if (!bytes.ok()) return Result<void>(Status::NotFound, path);
    if (check_perm) if (auto r = check_permissions(path); !r.ok()) return r;
    const std::string text(bytes->begin(), bytes->end());
    int line_no = 0;
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      std::string line = text.substr(start, end - start);
      start = end + 1;
      ++line_no;
      const std::string at = where(path, line_no);
      // the keyword: up to whitespace or '='
      size_t i = 0;
      while (i < line.size() && is_space(line[i])) ++i;
      if (i >= line.size() || line[i] == '#') continue;
      size_t k = i;
      while (k < line.size() && !is_space(line[k]) && line[k] != '=') ++k;
      std::string kw = lower(line.substr(i, k - i));
      while (k < line.size() && is_space(line[k])) ++k;
      if (k < line.size() && line[k] == '=') ++k;
      while (k < line.size() && is_space(line[k])) ++k;
      std::string rest = line.substr(k);
      while (!rest.empty() && is_space(rest.back())) rest.pop_back();
      std::vector<std::string> args;
      if (!split_args(rest, args)) return Result<void>(Status::Invalid, at + ": unterminated quote");
      if (kw == "host") {
        if (args.empty()) return Result<void>(Status::Invalid, at + ": Missing argument.");
        bool hit = false, refused = false;
        for (auto& a : args) {
          bool neg = !a.empty() && a[0] == '!';
          std::string p = neg ? a.substr(1) : a;
          if (glob(p, 0, original_host, 0, false)) { if (neg) refused = true; else hit = true; }
        }
        active = !never_match && hit && !refused;
        continue;
      }
      if (kw == "match") {
        if (never_match) { active = false; continue; }
        auto m = match_line(args, at);
        if (!m.ok()) return Result<void>(m.error());
        active = *m;
        continue;
      }
      if (kw == "include") {
        if (args.empty()) return Result<void>(Status::Invalid, at + ": Missing argument.");
        const bool was_active = active;
        for (auto& a : args) {
          std::string p = tilde(a);
          if (p.empty()) continue;
          if (p[0] != '/') p = (user_conf ? account.home + "/.ssh" : std::string("/etc/ssh")) + "/" + p;
          for (auto& f : expand_glob(p)) {
            auto st = host.stat(f);
            if (st.ok() && st->is_dir) continue;
            if (auto r = read_file(f, user_conf, never_match || !was_active, active, depth + 1, true); !r.ok()) {
              if (r.error().status == Status::NotFound && r.error().message == f) continue;
              return r;
            }
          }
          active = was_active;
        }
        continue;
      }
      if (args.empty() && kw != "proxycommand") return Result<void>(Status::Invalid, at + ": missing argument");
      if (auto r = apply(kw, args, rest, active && !never_match, at); !r.ok()) return r;
    }
    return Ok();
  }

  Result<void> read_all(const SshConfigFiles& files) {
    bool active = true;
    if (!files.user_file.empty()) {
      // a named file (ssh -F): it must exist, it stands in for the user's own, and nothing else is read
      if (auto r = read_file(files.user_file, true, false, active, 0, false); !r.ok()) {
        if (r.error().status == Status::NotFound && r.error().message == files.user_file)
          return Result<void>(Status::NotFound, "Can't open user config file " + files.user_file);
        return r;
      }
      return Ok();
    }
    const std::string user = account.home + "/.ssh/config";
    if (auto r = read_file(user, true, false, active, 0, true); !r.ok() && !(r.error().status == Status::NotFound && r.error().message == user))
      return r;
    if (files.read_system) {
      active = true;
      std::string sys = files.system_file;
      if (sys.empty()) {
#ifdef _WIN32
        std::string pd = host.env("PROGRAMDATA");
        sys = (pd.empty() ? std::string("C:/ProgramData") : pd) + "/ssh/ssh_config";
#else
        sys = "/etc/ssh/ssh_config";
#endif
      }
      if (auto r = read_file(sys, false, false, active, 0, false); !r.ok() && !(r.error().status == Status::NotFound && r.error().message == sys))
        return r;
    }
    return Ok();
  }
};

}  // namespace

std::vector<std::string> ssh_default_identity_files(const std::string& home) {
  // readconf.c's defaults for OpenSSH 10 (DSA is gone), in the order it tries them
  static const char* kDefaults[] = {"id_rsa", "id_ecdsa", "id_ecdsa_sk", "id_ed25519", "id_ed25519_sk", "id_xmss"};
  std::vector<std::string> out;
  for (const char* name : kDefaults) out.push_back(home + "/.ssh/" + name);
  return out;
}

bool ssh_match_pattern_list(const std::string& name, const std::string& patterns) {
  return Resolver::pattern_list(lower(name), patterns, true);
}

Result<SshResolved> ssh_resolve(hal::Host& host, const std::string& name, const std::string& user_from_url,
                                uint16_t port_from_url, const SshConfigFiles& files) {
  auto acct = hal::local_account();
  hal::LocalAccount account;
  if (acct.ok()) account = *acct;
  if (!files.home.empty()) account.home = files.home;
  if (!files.local_user.empty()) account.name = files.local_user;
  if (account.home.empty())
    return Result<SshResolved>(Status::NotFound, "no home directory to find ~/.ssh in" + (acct.ok() ? std::string() : ": " + acct.error().message));
  if (name.empty()) return Result<SshResolved>(Status::Invalid, "no host name to resolve");

  Resolver r{host, account, hal::local_hostname(), lower(name), name, {}, false, false, {}};
  // what a command line gives is obtained before any file is read, so the files cannot override it
  if (!user_from_url.empty()) r.o.user = user_from_url;
  if (port_from_url) r.o.port = port_from_url;
  if (auto rd = r.read_all(files); !rd.ok()) return Result<SshResolved>(rd.error());
  if (r.want_final) {
    r.final_pass = true;
    if (auto rd = r.read_all(files); !rd.ok()) return Result<SshResolved>(rd.error());
  }

  const Options& o = r.o;
  SshResolved out;
  out.original_host = name;
  out.hostname = o.hostname ? lower(Resolver::replace_h(*o.hostname, r.host_arg)) : r.host_arg;
  out.port = o.port ? *o.port : 22;
  // User may hold tokens; they take the final host name and port
  Resolver fin = r;
  fin.o.port = out.port;
  out.user = o.user ? fin.expand_tokens(*o.user) : account.name;
  fin.o.user = out.user;
  out.identity_files_given = !o.identity_files.empty();
  if (out.identity_files_given) for (auto& f : o.identity_files) out.identity_files.push_back(fin.path_of(f));
  else out.identity_files = ssh_default_identity_files(account.home);
  for (auto& f : o.certificate_files) out.certificate_files.push_back(fin.path_of(f));
  out.identities_only = o.identities_only.value_or(false);
  if (o.identity_agent) {
    const std::string& a = *o.identity_agent;
    if (lower(a) == "none") out.identity_agent = "none";
    else if (a == "SSH_AUTH_SOCK") out.identity_agent = "SSH_AUTH_SOCK";
    else if (a.size() > 1 && a[0] == '$') out.identity_agent = host.env(a.substr(1).c_str());
    else out.identity_agent = fin.path_of(a);
  }
  if (o.user_known_hosts) {
    for (auto& f : *o.user_known_hosts) if (lower(f) != "none") out.user_known_hosts_files.push_back(fin.path_of(f));
  } else {
    out.user_known_hosts_files = {account.home + "/.ssh/known_hosts", account.home + "/.ssh/known_hosts2"};
  }
  if (o.global_known_hosts) {
    // taken as written: OpenSSH expands neither ~ nor tokens in the system files
    for (auto& f : *o.global_known_hosts) if (lower(f) != "none") out.global_known_hosts_files.push_back(f);
  } else {
#ifdef _WIN32
    std::string pd = host.env("PROGRAMDATA");
    std::string base = (pd.empty() ? std::string("C:/ProgramData") : pd) + "/ssh/";
    out.global_known_hosts_files = {base + "ssh_known_hosts", base + "ssh_known_hosts2"};
#else
    out.global_known_hosts_files = {"/etc/ssh/ssh_known_hosts", "/etc/ssh/ssh_known_hosts2"};
#endif
  }
  out.strict_host_key_checking = o.strict.value_or("ask");
  out.hash_known_hosts = o.hash_known_hosts.value_or(false);
  out.check_host_ip = o.check_host_ip.value_or(false);
  out.host_key_alias = o.host_key_alias.value_or("");
  out.host_key_algorithms = o.host_key_algorithms.value_or(std::vector<std::string>());
  out.pubkey_accepted_algorithms = o.pubkey_algorithms.value_or(std::vector<std::string>());
  out.ciphers = o.ciphers.value_or(std::vector<std::string>());
  out.kex_algorithms = o.kex.value_or(std::vector<std::string>());
  out.macs = o.macs.value_or(std::vector<std::string>());
  if (o.proxy_jump) out.proxy_jump = *o.proxy_jump;
  out.proxy_command = o.proxy_command.value_or("");
  out.connect_timeout_s = (int)o.connect_timeout.value_or(0);
  out.server_alive_interval_s = (int)o.alive_interval.value_or(0);
  out.server_alive_count_max = (int)o.alive_count.value_or(3);
  out.connection_attempts = (int)o.attempts.value_or(1);
  out.batch_mode = o.batch_mode.value_or(false);
  out.pubkey_authentication = o.pubkey.value_or(true);
  out.password_authentication = o.password.value_or(true);
  out.kbd_interactive_authentication = o.kbd.value_or(true);
  out.preferred_authentications = o.preferred_auth.value_or(std::vector<std::string>());
  out.address_family = o.address_family.value_or("any");
  out.bind_address = o.bind_address.value_or("");
  for (auto& [k, v] : o.set_env) out.set_env.push_back({k, fin.expand_tokens(v)});
  out.send_env = o.send_env;
  out.log_level = o.log_level.value_or("INFO");
  return out;
}

}  // namespace aios::net
