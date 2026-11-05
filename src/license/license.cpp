#include "aios/license/license.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>

#include "aios/core/bytes.hpp"
#include "aios/crypto/signed_chain.hpp"
#include "aios/hal/host.hpp"

#ifndef AIOS_TRIAL_DAYS
#define AIOS_TRIAL_DAYS 30
#endif

namespace aios::license {
namespace {

// The issuer's Ed25519 public key (hex). Its private half is made by `aios license keygen` and kept off the
// machines this runs on; only a license signed with it is accepted.
constexpr const char* kIssuerPublicKey = "bf590ad32a518dc274b43edb4a857e2f2ac414691006b1780d15b4a10dd7fbbf";

constexpr int64_t kTrialDays = AIOS_TRIAL_DAYS;
constexpr int64_t kDay = 86400;

// The trial record carries a check value over its two times, so a hand edit of either is seen. The salt is in the
// binary for anyone who reads it: this stops editing the file, not someone who has taken the binary apart.
constexpr const char* kTrialSalt = "aios-trial-record-v1:8f3c1e0b7a";

int64_t now_seconds() { return static_cast<int64_t>(hal::process_host().wall_ns() / 1000000000ull); }

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::string civil_from_days(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", static_cast<long long>(y + (m <= 2)), m, d);
  return buf;
}

// "YYYY-MM-DD" to days since 1970-01-01, or nothing for anything else (including 2026-02-30).
std::optional<int64_t> parse_date(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
  for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
  const int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
  if (m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
  const int64_t days = days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
  if (civil_from_days(days) != s) return std::nullopt;
  return days;
}

std::string home_dir() {
  const char* h = std::getenv("HOME");
  return h && *h ? std::string(h) : std::string();
}

std::optional<std::string> read_text(const std::string& path) {
  hal::Host& host = hal::process_host();
  if (!host.exists(path)) return std::nullopt;
  auto b = host.read_file(path);
  if (!b.ok()) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(b->data()), b->size());
}

std::string dir_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

bool write_text(const std::string& path, const std::string& text, std::string* error = nullptr) {
  hal::Host& host = hal::process_host();
  if (auto d = host.make_dir(dir_of(path)); !d.ok()) {
    if (error) *error = d.error().message;
    return false;
  }
  auto w = host.write_file(path, span_of(text));
  if (!w.ok()) {
    if (error) *error = w.error().message;
    return false;
  }
  return true;
}

// ---- licenses -------------------------------------------------------------------------------------------------

struct License {
  std::string to;
  std::string issued;   // YYYY-MM-DD
  std::string expires;  // YYYY-MM-DD or "never"
};

// The bytes a signature covers: the header line and the three fields, each ending in a newline.
std::string license_body(const License& l) {
  return "aios license 1\nto: " + l.to + "\nissued: " + l.issued + "\nexpires: " + l.expires + "\n";
}

struct Parsed {
  bool ok = false;
  std::string error;
  License license;
};

// Reads and checks a license's text: its form, then the signature against the issuer's key. Not its date.
Parsed parse_license(const std::string& text, const std::string& public_hex) {
  Parsed p;
  std::istringstream in(text);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  auto field = [&](size_t i, const std::string& key, std::string& out) {
    const std::string prefix = key + ": ";
    if (i >= lines.size() || lines[i].compare(0, prefix.size(), prefix) != 0) return false;
    out = lines[i].substr(prefix.size());
    return !out.empty();
  };
  std::string signature;
  if (lines.size() != 5 || lines[0] != "aios license 1" || !field(1, "to", p.license.to) ||
      !field(2, "issued", p.license.issued) || !field(3, "expires", p.license.expires) ||
      !field(4, "signature", signature)) {
    p.error = "not an aios license (expected the lines: aios license 1, to:, issued:, expires:, signature:)";
    return p;
  }
  if (!parse_date(p.license.issued)) {
    p.error = "the issued date is not YYYY-MM-DD";
    return p;
  }
  if (p.license.expires != "never" && !parse_date(p.license.expires)) {
    p.error = "the expires date is neither YYYY-MM-DD nor never";
    return p;
  }
  if (public_hex.size() != 64) {
    p.error = "this build carries no issuer key, so it cannot check a license";
    return p;
  }
  if (!crypto::ed25519_verify_hex(license_body(p.license), signature, public_hex)) {
    p.error = "the signature does not match: the license was changed or was not issued for aios";
    return p;
  }
  p.ok = true;
  return p;
}

// Days from today (UTC) to the license's last day, counting both: 1 on its last day, 0 or less once it has ended.
// Nothing for a license that never ends.
std::optional<int64_t> days_left(const License& l) {
  if (l.expires == "never") return std::nullopt;
  return *parse_date(l.expires) - now_seconds() / kDay + 1;
}

// Where an installed license is looked for, first found wins.
std::vector<std::string> license_paths(const std::string& root) {
  std::vector<std::string> v;
  if (const char* e = std::getenv("AIOS_LICENSE"); e && *e) v.push_back(e);
  if (!root.empty()) v.push_back(root + "/license");
  v.push_back("/etc/aios/license");
  if (const std::string h = home_dir(); !h.empty()) v.push_back(h + "/.aios/license");
  return v;
}

// ---- the trial ------------------------------------------------------------------------------------------------

// Every place the trial start is kept. The earliest start found anywhere counts, so deleting one copy (or using a
// new --root) does not start the trial again; each run writes the record to every place it can.
std::vector<std::string> trial_paths(const std::string& root) {
  std::vector<std::string> v;
  if (!root.empty()) v.push_back(root + "/.aios-trial");
  if (const std::string h = home_dir(); !h.empty()) v.push_back(h + "/.aios/trial");
  v.push_back("/var/tmp/.aios-trial");
  v.push_back("/var/lib/aios/.aios-trial");
  return v;
}

std::string trial_check(int64_t first, int64_t seen) {
  return crypto::sha256_hex(std::string(kTrialSalt) + "|" + std::to_string(first) + "|" + std::to_string(seen));
}

std::string trial_text(int64_t first, int64_t seen) {
  return "aios trial 1\nfirst: " + std::to_string(first) + "\nseen: " + std::to_string(seen) +
         "\ncheck: " + trial_check(first, seen) + "\n";
}

struct TrialRecord {
  int64_t first = 0;
  int64_t seen = 0;
};

// A record as written by trial_text, or nothing when the text is not one or its check value does not match.
std::optional<TrialRecord> parse_trial(const std::string& text) {
  std::istringstream in(text);
  std::string header, first, seen, check;
  if (!std::getline(in, header) || header != "aios trial 1") return std::nullopt;
  if (!std::getline(in, first) || first.rfind("first: ", 0) != 0) return std::nullopt;
  if (!std::getline(in, seen) || seen.rfind("seen: ", 0) != 0) return std::nullopt;
  if (!std::getline(in, check) || check.rfind("check: ", 0) != 0) return std::nullopt;
  TrialRecord r;
  try {
    size_t used = 0;
    r.first = std::stoll(first.substr(7), &used);
    if (used != first.size() - 7) return std::nullopt;
    r.seen = std::stoll(seen.substr(6), &used);
    if (used != seen.size() - 6) return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
  if (check.substr(7) != trial_check(r.first, r.seen)) return std::nullopt;
  return r;
}

struct TrialState {
  bool damaged = false;       // a record is there but is not one this program wrote
  std::string damaged_path;
  bool found = false;
  TrialRecord earliest;       // first: the earliest start; seen: the latest time any run saw
  std::vector<std::string> found_at;
};

TrialState read_trial(const std::string& root) {
  TrialState s;
  for (const std::string& path : trial_paths(root)) {
    auto text = read_text(path);
    if (!text) continue;
    auto r = parse_trial(*text);
    if (!r) {
      if (!s.damaged) s.damaged_path = path;
      s.damaged = true;
      continue;
    }
    s.found_at.push_back(path);
    if (!s.found) {
      s.earliest = *r;
      s.found = true;
    } else {
      s.earliest.first = std::min(s.earliest.first, r->first);
      s.earliest.seen = std::max(s.earliest.seen, r->seen);
    }
  }
  return s;
}

// The trial's days left (counting today) from its start and the latest time seen; a clock set back gains nothing,
// because the latest time any run saw stands in for a "now" earlier than it.
int64_t trial_days_left(const TrialRecord& r, int64_t now) {
  const int64_t at = std::max(now, r.seen);
  return kTrialDays - (at - r.first) / kDay;
}

std::string trial_end_date(const TrialRecord& r) { return civil_from_days((r.first + kTrialDays * kDay) / kDay); }

const char* kHowToLicense = "install a license with: aios license install FILE";

}  // namespace

Verdict check(const std::string& root) {
  Verdict v;
  // An installed license decides on its own: a valid one is the answer, a bad or ended one refuses, and the trial
  // is not a way around either.
  for (const std::string& path : license_paths(root)) {
    auto text = read_text(path);
    if (!text) continue;
    Parsed p = parse_license(*text, kIssuerPublicKey);
    if (!p.ok) {
      v.message = "aios: the license at " + path + " is not valid: " + p.error;
      return v;
    }
    auto left = days_left(p.license);
    if (left && *left <= 0) {
      v.message = "aios: the license for " + p.license.to + " ended on " + p.license.expires + "; " + kHowToLicense;
      return v;
    }
    v.ok = true;
    if (left && *left <= 14)
      v.message = "aios: the license for " + p.license.to + " ends on " + p.license.expires + " (" +
                  std::to_string(*left) + (*left == 1 ? " day" : " days") + " left)";
    return v;
  }

  const int64_t now = now_seconds();
  TrialState s = read_trial(root);
  if (s.damaged) {
    v.message = "aios: the trial record at " + s.damaged_path + " has been changed; " + kHowToLicense;
    return v;
  }
  TrialRecord r = s.found ? s.earliest : TrialRecord{now, now};
  const int64_t left = trial_days_left(r, now);
  if (left <= 0) {
    v.message = "aios: the " + std::to_string(kTrialDays) + "-day trial ended on " + trial_end_date(r) + "; " +
                kHowToLicense;
    return v;
  }
  r.seen = std::max(r.seen, now);
  int written = 0;
  for (const std::string& path : trial_paths(root))
    if (write_text(path, trial_text(r.first, r.seen))) ++written;
  // The trial starts only once it is on record somewhere; otherwise every run would be a first run.
  if (!s.found && written == 0) {
    v.message = "aios: cannot record the trial start (none of " + trial_paths(root).front() +
                ", ~/.aios/trial, /var/tmp/.aios-trial is writable); " + kHowToLicense;
    return v;
  }
  v.ok = true;
  if (left <= 7)
    v.message = "aios: trial, " + std::to_string(left) + (left == 1 ? " day" : " days") + " left (ends " +
                trial_end_date(r) + "); " + kHowToLicense;
  return v;
}

namespace {

void usage(std::ostream& out) {
  out << "aios license — the license a public build runs under\n\n"
         "  aios license status                 this build, the license in use or the trial's days left\n"
         "  aios license install FILE           check FILE and install it (/etc/aios/license, else ~/.aios/license)\n"
         "  aios license verify FILE            check FILE without installing it\n\n"
         "issuer only (needs the private signing key):\n"
         "  aios license keygen KEYFILE         make a new signing key (refuses to overwrite); prints its public key\n"
         "  aios license issue --key KEYFILE --to NAME (--days N | --until YYYY-MM-DD | --forever) [--out FILE]\n"
         "                                      write a signed license (to stdout without --out)\n\n"
         "A license is looked for in $AIOS_LICENSE, ROOT/license, /etc/aios/license, then ~/.aios/license.\n";
}

void describe(const License& l, std::ostream& out) {
  out << "  for:     " << l.to << "\n  issued:  " << l.issued << "\n  expires: " << l.expires;
  if (auto left = days_left(l)) {
    if (*left <= 0) out << " (ended)";
    else out << " (" << *left << (*left == 1 ? " day" : " days") << " left)";
  }
  out << "\n";
}

int cmd_status(const std::string& root, std::ostream& out) {
#ifdef AIOS_LICENSE_REQUIRED
  out << "build: public (" << kTrialDays << "-day trial, then a license)\n";
#else
  out << "build: private (runs without a license; nothing below is enforced)\n";
#endif
  for (const std::string& path : license_paths(root)) {
    auto text = read_text(path);
    if (!text) continue;
    Parsed p = parse_license(*text, kIssuerPublicKey);
    out << "license: " << path << "\n";
    if (!p.ok) {
      out << "  not valid: " << p.error << "\n";
      return kRefused;
    }
    describe(p.license, out);
    auto left = days_left(p.license);
    return left && *left <= 0 ? kRefused : 0;
  }
  out << "license: none installed\n";
  TrialState s = read_trial(root);
  if (s.damaged) {
    out << "trial: record at " << s.damaged_path << " has been changed (refused)\n";
    return kRefused;
  }
  if (!s.found) {
    out << "trial: not started (the first run starts " << kTrialDays << " days)\n";
    return 0;
  }
  const int64_t left = trial_days_left(s.earliest, now_seconds());
  out << "trial: started " << civil_from_days(s.earliest.first / kDay) << ", ";
  if (left <= 0) out << "ended " << trial_end_date(s.earliest) << "\n";
  else out << left << (left == 1 ? " day" : " days") << " left (ends " << trial_end_date(s.earliest) << ")\n";
  for (const std::string& p : s.found_at) out << "  recorded at " << p << "\n";
  return left <= 0 ? kRefused : 0;
}

int cmd_verify(const std::string& file, bool install, std::ostream& out, std::ostream& err) {
  auto text = read_text(file);
  if (!text) {
    err << "aios license: cannot read " << file << "\n";
    return 1;
  }
  Parsed p = parse_license(*text, kIssuerPublicKey);
  if (!p.ok) {
    err << "aios license: " << file << " is not valid: " << p.error << "\n";
    return kRefused;
  }
  auto left = days_left(p.license);
  if (left && *left <= 0) {
    err << "aios license: " << file << " ended on " << p.license.expires << "\n";
    return kRefused;
  }
  if (!install) {
    out << "valid\n";
    describe(p.license, out);
    return 0;
  }
  std::vector<std::string> targets{"/etc/aios/license"};
  if (const std::string h = home_dir(); !h.empty()) targets.push_back(h + "/.aios/license");
  std::string last_error;
  for (const std::string& t : targets) {
    if (write_text(t, *text, &last_error)) {
      out << "installed " << t << "\n";
      describe(p.license, out);
      return 0;
    }
  }
  err << "aios license: cannot write " << targets.back() << ": " << last_error << "\n";
  return 1;
}

int cmd_keygen(const std::string& keyfile, std::ostream& out, std::ostream& err) {
  if (hal::process_host().exists(keyfile)) {
    err << "aios license: " << keyfile << " already exists; a new key would orphan every license it signed\n";
    return 1;
  }
  if (auto d = hal::process_host().make_dir(dir_of(keyfile)); !d.ok()) {
    err << "aios license: " << d.error().message << "\n";
    return 1;
  }
  crypto::Ed25519Signer signer;
  if (auto r = signer.open(keyfile); !r.ok()) {
    err << "aios license: " << r.error().message << "\n";
    return 1;
  }
  out << "signing key: " << keyfile << " (keep it private and backed up)\n"
      << "public key:  " << signer.public_hex() << "\n";
  return 0;
}

int cmd_issue(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  std::string keyfile, to, until, outfile;
  int64_t days = -1;
  bool forever = false;
  for (size_t i = 2; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= args.size()) return false;
      dst = args[++i];
      return true;
    };
    std::string n;
    if (a == "--key" && value(keyfile)) continue;
    if (a == "--to" && value(to)) continue;
    if (a == "--until" && value(until)) continue;
    if (a == "--out" && value(outfile)) continue;
    if (a == "--forever") { forever = true; continue; }
    if (a == "--days" && value(n)) {
      try {
        size_t used = 0;
        days = std::stoll(n, &used);
        if (used != n.size() || days < 1) days = -2;
      } catch (...) {
        days = -2;
      }
      if (days == -2) {
        err << "aios license: --days takes a whole number of days, 1 or more\n";
        return 2;
      }
      continue;
    }
    err << "aios license: unexpected argument " << a << "\n";
    usage(err);
    return 2;
  }
  if (keyfile.empty() || to.empty() || (int(forever) + int(days > 0) + int(!until.empty())) != 1) {
    err << "aios license: issue needs --key, --to and exactly one of --days, --until, --forever\n";
    return 2;
  }
  if (to.find('\n') != std::string::npos || to.find('\r') != std::string::npos) {
    err << "aios license: --to must be one line\n";
    return 2;
  }
  if (!hal::process_host().exists(keyfile)) {
    err << "aios license: no signing key at " << keyfile << " (make one with: aios license keygen " << keyfile << ")\n";
    return 1;
  }
  crypto::Ed25519Signer signer;
  if (auto r = signer.open(keyfile); !r.ok()) {
    err << "aios license: " << r.error().message << "\n";
    return 1;
  }
  if (std::string(kIssuerPublicKey) != signer.public_hex())
    err << "aios license: warning: " << keyfile << " is not the key this build accepts; "
        << "its licenses will be refused here\n";
  const int64_t today = now_seconds() / kDay;
  License l;
  l.to = to;
  l.issued = civil_from_days(today);
  if (forever) l.expires = "never";
  else if (days > 0) l.expires = civil_from_days(today + days - 1);
  else {
    auto d = parse_date(until);
    if (!d) {
      err << "aios license: --until takes YYYY-MM-DD\n";
      return 2;
    }
    if (*d < today) {
      err << "aios license: --until " << until << " is already past\n";
      return 2;
    }
    l.expires = until;
  }
  const std::string body = license_body(l);
  const std::string text = body + "signature: " + signer.sign_hex(body) + "\n";
  if (outfile.empty()) {
    out << text;
    return 0;
  }
  std::string error;
  if (!write_text(outfile, text, &error)) {
    err << "aios license: cannot write " << outfile << ": " << error << "\n";
    return 1;
  }
  out << "wrote " << outfile << "\n";
  describe(l, out);
  return 0;
}

}  // namespace

int command(const std::vector<std::string>& args, const std::string& root, std::ostream& out, std::ostream& err) {
  const std::string sub = args.size() > 1 ? args[1] : "status";
  if (sub == "status" && args.size() <= 2) return cmd_status(root, out);
  if ((sub == "install" || sub == "verify") && args.size() == 3) return cmd_verify(args[2], sub == "install", out, err);
  if (sub == "keygen" && args.size() == 3) return cmd_keygen(args[2], out, err);
  if (sub == "issue") return cmd_issue(args, out, err);
  if (sub == "help" || sub == "--help" || sub == "-h") {
    usage(out);
    return 0;
  }
  usage(err);
  return 2;
}

}  // namespace aios::license
