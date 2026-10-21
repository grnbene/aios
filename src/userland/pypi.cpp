#include "aios/userland/pypi.hpp"

#include <cstring>
#include <unordered_map>

#include "aios/channel/http_client.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/userland/archive.hpp"

namespace aios::userland {
namespace {

const char* const kMediaType = "application/json";
const char* const kUserAgentHeader = "User-Agent";
// PyPI's document URLs end in this; another name this machine did not choose.
const char* const kDocumentSuffix = "/json";

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}

std::string hex_of(ByteSpan b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
  return s;
}

std::string lower(std::string s) {
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c += 32;
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> split_on(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

// A distribution name is compared with dashes, underscores and dots all alike,
// and case folded — "Charset_Normalizer" and "charset-normalizer" are one name.
std::string canon(const std::string& name) {
  std::string out;
  for (char c : lower(name)) out += (c == '_' || c == '.') ? '-' : c;
  return out;
}

// ---------------------------------------------------------------------------
// PEP 440
// ---------------------------------------------------------------------------
struct PyVersion {
  uint64_t epoch = 0;
  std::vector<uint64_t> release;
  // Ordering of the qualifiers: dev < pre < release < post.
  int stage = 2;              // 0 dev, 1 pre, 2 release, 3 post
  int pre_kind = 0;           // a=0, b=1, rc=2
  uint64_t stage_num = 0;
  bool ok = false;
};

PyVersion parse_py(const std::string& raw) {
  PyVersion v;
  std::string s = lower(trim(raw));
  if (!s.empty() && s[0] == 'v') s.erase(s.begin());
  size_t i = 0;
  auto number = [&](uint64_t& out) {
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    out = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') out = out * 10 + (uint64_t)(s[i++] - '0');
    return true;
  };
  uint64_t first = 0;
  if (!number(first)) return v;
  if (i < s.size() && s[i] == '!') { v.epoch = first; ++i; if (!number(first)) return v; }
  v.release.push_back(first);
  while (i < s.size() && s[i] == '.') {
    ++i;
    uint64_t n = 0;
    if (!number(n)) break;
    v.release.push_back(n);
  }
  // What follows is a qualifier, in any of the spellings PEP 440 allows.
  std::string rest = s.substr(i);
  auto starts = [&](const char* p) { return rest.compare(0, std::strlen(p), p) == 0; };
  auto tail_number = [&](size_t at) {
    uint64_t n = 0;
    while (at < rest.size() && rest[at] >= '0' && rest[at] <= '9') n = n * 10 + (uint64_t)(rest[at++] - '0');
    return n;
  };
  if (!rest.empty() && (rest[0] == '-' || rest[0] == '_' || rest[0] == '.')) rest.erase(rest.begin());
  if (starts("dev")) { v.stage = 0; v.stage_num = tail_number(3); }
  else if (starts("post") || starts("rev") || starts("r")) {
    v.stage = 3;
    v.stage_num = tail_number(starts("post") ? 4 : (starts("rev") ? 3 : 1));
  } else if (starts("rc") || starts("c")) { v.stage = 1; v.pre_kind = 2; v.stage_num = tail_number(starts("rc") ? 2 : 1); }
  else if (starts("alpha") || starts("a")) { v.stage = 1; v.pre_kind = 0; v.stage_num = tail_number(starts("alpha") ? 5 : 1); }
  else if (starts("beta") || starts("b")) { v.stage = 1; v.pre_kind = 1; v.stage_num = tail_number(starts("beta") ? 4 : 1); }
  else if (!rest.empty() && rest[0] == '+') { /* a local version; ignored for ordering here */ }
  v.ok = true;
  return v;
}

}  // namespace

int pep440_compare(const std::string& a, const std::string& b) {
  PyVersion x = parse_py(a), y = parse_py(b);
  if (x.epoch != y.epoch) return x.epoch < y.epoch ? -1 : 1;
  size_t n = x.release.size() > y.release.size() ? x.release.size() : y.release.size();
  for (size_t i = 0; i < n; ++i) {
    uint64_t xi = i < x.release.size() ? x.release[i] : 0;
    uint64_t yi = i < y.release.size() ? y.release[i] : 0;
    if (xi != yi) return xi < yi ? -1 : 1;
  }
  if (x.stage != y.stage) return x.stage < y.stage ? -1 : 1;
  if (x.stage == 1 && x.pre_kind != y.pre_kind) return x.pre_kind < y.pre_kind ? -1 : 1;
  if (x.stage_num != y.stage_num) return x.stage_num < y.stage_num ? -1 : 1;
  return 0;
}

namespace {

bool matches_specifier(const std::string& version, const std::string& op, const std::string& bound) {
  int c = pep440_compare(version, bound);
  if (op == "==") {
    // A trailing ".*" means "this prefix": ==1.4.* admits 1.4.9.
    if (bound.size() > 2 && bound.compare(bound.size() - 2, 2, ".*") == 0) {
      std::string prefix = bound.substr(0, bound.size() - 1);
      return version.compare(0, prefix.size(), prefix) == 0;
    }
    return c == 0;
  }
  if (op == "!=") return c != 0;
  if (op == ">=") return c >= 0;
  if (op == "<=") return c <= 0;
  if (op == ">") return c > 0;
  if (op == "<") return c < 0;
  if (op == "===") return version == bound;
  if (op == "~=") {
    // ~=1.4.2 is >=1.4.2 with the last component free: <1.5.0.
    if (c < 0) return false;
    PyVersion b = parse_py(bound), v = parse_py(version);
    if (b.release.size() < 2) return false;
    for (size_t i = 0; i + 1 < b.release.size(); ++i) {
      uint64_t vi = i < v.release.size() ? v.release[i] : 0;
      if (vi != b.release[i]) return false;
    }
    return true;
  }
  return false;
}

}  // namespace

Result<std::string> pep440_pick(const std::vector<std::string>& available, const std::string& specifiers) {
  std::vector<std::pair<std::string, std::string>> want;    // op, bound
  for (const auto& piece : split_on(specifiers, ',')) {
    std::string s = trim(piece);
    if (s.empty()) continue;
    size_t i = 0;
    std::string op;
    while (i < s.size() && (s[i] == '=' || s[i] == '!' || s[i] == '<' || s[i] == '>' || s[i] == '~')) op += s[i++];
    std::string bound = trim(s.substr(i));
    if (op.empty()) op = "==";                              // a bare version means exactly it
    if (bound.empty()) return Result<std::string>(Status::Invalid, "a version specifier with no version: '" + piece + "'");
    want.push_back({op, bound});
  }

  std::string best;
  bool any_prerelease_wanted = false;
  for (const auto& w : want)
    if (parse_py(w.second).stage != 2) any_prerelease_wanted = true;

  for (const auto& v : available) {
    PyVersion pv = parse_py(v);
    if (!pv.ok) continue;
    if (pv.stage != 2 && !any_prerelease_wanted) continue;   // a prerelease is not picked by accident
    bool all = true;
    for (const auto& w : want)
      if (!matches_specifier(v, w.first, w.second)) { all = false; break; }
    if (!all) continue;
    if (best.empty() || pep440_compare(v, best) > 0) best = v;
  }
  if (best.empty())
    return Result<std::string>(Status::NotFound,
                              specifiers.empty() ? "the index lists no usable version"
                                                 : "no published version satisfies " + specifiers);
  return best;
}

bool wheel_fits(const std::string& filename, const std::string& python, const std::string& platform) {
  if (filename.size() < 5 || filename.compare(filename.size() - 4, 4, ".whl") != 0) return false;
  std::string stem = filename.substr(0, filename.size() - 4);
  auto parts = split_on(stem, '-');
  if (parts.size() < 5) return false;
  // The last three are always the tags; a build number may sit before them.
  const std::string& py_tags = parts[parts.size() - 3];
  const std::string& abi_tags = parts[parts.size() - 2];
  const std::string& plat_tags = parts[parts.size() - 1];

  auto version_parts = split_on(python, '.');
  std::string major = version_parts.size() > 0 ? version_parts[0] : "3";
  std::string minor = version_parts.size() > 1 ? version_parts[1] : "";
  std::string cp = "cp" + major + minor;

  bool py_ok = false;
  for (const auto& t : split_on(py_tags, '.')) {
    if (t == "py" + major || t == "py2.py3" || t == cp) py_ok = true;
    if (t == "py3" && major == "3") py_ok = true;
    // An abi3 wheel built for an older minor still fits a newer interpreter.
    if (t.rfind("cp" + major, 0) == 0 && abi_tags == "abi3") py_ok = true;
  }
  if (!py_ok) return false;

  bool abi_ok = (abi_tags == "none" || abi_tags == "abi3" || abi_tags == cp || abi_tags == cp + "m");
  if (!abi_ok) return false;

  bool plat_ok = false;
  for (const auto& t : split_on(plat_tags, '.')) {
    if (t == "any") { plat_ok = true; continue; }
    if (platform.empty()) continue;
    // "manylinux_2_28_x86_64", "manylinux2014_x86_64", "linux_x86_64": what
    // matters to this machine is the architecture at the end.
    if (t.size() > platform.size() && t.compare(t.size() - platform.size(), platform.size(), platform) == 0 &&
        (t.rfind("linux", 0) == 0 || t.rfind("manylinux", 0) == 0 || t.rfind("musllinux", 0) == 0))
      plat_ok = true;
  }
  return plat_ok;
}

// ---------------------------------------------------------------------------
// The installer
// ---------------------------------------------------------------------------
namespace {

Result<void> may(Surface* actor, ResourceKind kind, uint16_t rights, const char* what) {
  if (!actor) return Ok();
  auto r = actor->may(ResourceRef{kind, {}}, rights);
  if (!r.ok())
    return Result<void>(Status::Denied,
                       "entity " + actor->self().short_str() + " may not " + what + ": " + r.error().str());
  return Ok();
}

// `as_document` decides one header, and that header decides what comes back:
// crates.io answers a download URL asked for as json with a json object NAMING
// the file rather than the file itself, and a client that hashes that gets a
// checksum failure it will never explain.
Result<channel::HttpResponse> fetch(Kernel& kernel, const std::string& url, int timeout_ms,
                                    channel::HttpTransportNote* note, bool as_document = true) {
  auto ep = channel::parse_endpoint(url);
  if (!ep.ok()) return Result<channel::HttpResponse>(ep.error());
  channel::HttpRequest req;
  req.endpoint = *ep;
  req.timeout_ms = timeout_ms;
  req.host = &kernel.host();
  req.kernel_root = kernel.config().root;
  req.headers.push_back({kUserAgentHeader, "aios-pip/1"});
  req.headers.push_back({"Accept", as_document ? kMediaType : "*/*"});
  auto r = channel::http_send_noted(req, note);
  if (!r.ok()) return r;
  if (r->status == 301 || r->status == 302 || r->status == 307 || r->status == 308) {
    std::string to = r->header("Location");
    if (!to.empty() && to != url) return fetch(kernel, to, timeout_ms, note, as_document);
  }
  if (r->status != 200)
    return Result<channel::HttpResponse>(Status::NotFound,
                                        url + ": the index answered " + dec((uint64_t)r->status) + " " + r->reason);
  return r;
}

// One requirement, PEP 508: "name[extra] (>=1,<2) ; marker".
struct Requirement {
  std::string name;
  std::string specifiers;
  bool wanted = true;         // false when a marker rules it out for this machine
};

Requirement parse_requirement(const std::string& raw, const std::string& python) {
  Requirement r;
  std::string s = trim(raw);
  size_t semi = s.find(';');
  std::string marker = semi == std::string::npos ? std::string() : trim(s.substr(semi + 1));
  if (semi != std::string::npos) s = trim(s.substr(0, semi));

  // An extra is only installed when it was asked for, and nothing here asks.
  if (marker.find("extra ==") != std::string::npos || marker.find("extra==") != std::string::npos) r.wanted = false;
  // python_version comparisons are the other marker that changes the answer.
  size_t pv = marker.find("python_version");
  if (r.wanted && pv != std::string::npos) {
    std::string rest = trim(marker.substr(pv + 14));
    std::string op;
    size_t i = 0;
    while (i < rest.size() && (rest[i] == '=' || rest[i] == '<' || rest[i] == '>' || rest[i] == '!')) op += rest[i++];
    std::string bound;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '"' || rest[i] == '\'')) ++i;
    while (i < rest.size() && rest[i] != '"' && rest[i] != '\'' && rest[i] != ' ') bound += rest[i++];
    if (!op.empty() && !bound.empty()) r.wanted = matches_specifier(python, op, bound);
  }

  size_t i = 0;
  auto name_char = [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.';
  };
  while (i < s.size() && name_char(s[i])) ++i;
  r.name = s.substr(0, i);
  std::string rest = trim(s.substr(i));
  if (!rest.empty() && rest[0] == '[') {           // an extra WE are asking for: not supported, so drop it
    size_t close = rest.find(']');
    rest = close == std::string::npos ? std::string() : trim(rest.substr(close + 1));
  }
  if (!rest.empty() && rest[0] == '(') {
    size_t close = rest.find(')');
    rest = close == std::string::npos ? rest.substr(1) : rest.substr(1, close - 1);
  }
  r.specifiers = trim(rest);
  return r;
}

Result<void> make_dirs(hal::Host& h, const std::string& path) {
  std::string acc;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      acc = path.substr(0, i);
      if (!acc.empty() && acc != "/") h.make_dir(acc);
    }
  }
  return Ok();
}

}  // namespace

PyPi::PyPi(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

Result<std::vector<std::string>> PyPi::versions(const std::string& name, std::string& latest, const PyOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the index"); !r.ok())
    return Result<std::vector<std::string>>(r.error());
  auto doc = fetch(kernel_, opt.index + "/pypi/" + name + kDocumentSuffix, opt.timeout_ms, nullptr);
  if (!doc.ok()) return Result<std::vector<std::string>>(doc.error());
  auto value = channel::rpc_parse(ByteSpan(doc->body.data(), doc->body.size()));
  if (!value.ok()) return Result<std::vector<std::string>>(value.error());
  std::vector<std::string> out;
  for (auto& f : (*value)["releases"].fields()) out.push_back(f.first);
  latest = (*value)["info"]["version"].as_text();
  if (out.empty()) return Result<std::vector<std::string>>(Status::NotFound, name + ": the index lists no releases");
  return out;
}

Result<PyReport> PyPi::install(const std::string& dir, const std::string& spec, const PyOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the index"); !r.ok())
    return Result<PyReport>(r.error());
  if (auto r = may(actor_, ResourceKind::StorageObject, R_Write, "write packages into this machine"); !r.ok())
    return Result<PyReport>(r.error());

  auto& h = kernel_.host();
  PyReport rep;
  rep.index = opt.index;
  rep.target = dir;
  uint64_t t0 = h.monotonic_ns();

  std::string platform = opt.platform;
  if (platform.empty()) {
    // The architecture this machine actually is, in the spelling a wheel uses.
    const std::string arch = h.topology().arch;
    platform = (arch == "aarch64" || arch == "arm64") ? "aarch64" : "x86_64";
  }
  rep.environment = "CPython " + opt.python + " on " + platform;

  std::vector<std::pair<std::string, std::string>> queue;   // name, specifiers
  if (!spec.empty()) {
    Requirement r = parse_requirement(spec, opt.python);
    if (r.name.empty()) return Result<PyReport>(Status::Invalid, "'" + spec + "' does not name a package");
    queue.push_back({r.name, r.specifiers});
  } else {
    auto req = h.read_file(dir + "/requirements.txt");
    if (!req.ok())
      return Result<PyReport>(Status::NotFound,
                             dir + "/requirements.txt: nothing here says what to install, and no package was named");
    for (const auto& line : split_on(string_of(ByteSpan(req->data(), req->size())), '\n')) {
      std::string l = trim(line);
      if (l.empty() || l[0] == '#' || l[0] == '-') continue;
      Requirement r = parse_requirement(l, opt.python);
      if (r.wanted && !r.name.empty()) queue.push_back({r.name, r.specifiers});
    }
    if (queue.empty()) return Result<PyReport>(Status::NotFound, dir + "/requirements.txt asks for nothing");
  }

  std::unordered_map<std::string, std::string> done;
  channel::HttpTransportNote note;
  size_t guard = 0;
  while (!queue.empty()) {
    if (++guard > opt.max_packages)
      return Result<PyReport>(Status::Exhausted,
                             "stopped after " + dec(opt.max_packages) + " packages; this dependency graph is larger "
                             "than this machine was told to take");
    auto [name, specifiers] = queue.front();
    queue.erase(queue.begin());
    if (done.count(canon(name))) continue;

    auto doc = fetch(kernel_, opt.index + "/pypi/" + name + kDocumentSuffix, opt.timeout_ms, &note);
    if (!doc.ok()) return Result<PyReport>(doc.error());
    if (rep.tls.empty()) { rep.tls = note.tls_version + " " + note.cipher; rep.trust_anchor = note.trust_anchor; }
    auto value = channel::rpc_parse(ByteSpan(doc->body.data(), doc->body.size()));
    if (!value.ok()) return Result<PyReport>(value.error());

    std::vector<std::string> available;
    for (auto& f : (*value)["releases"].fields())
      if (f.second.size() > 0) available.push_back(f.first);       // a release with no files is not one
    auto chosen = pep440_pick(available, specifiers);
    if (!chosen.ok()) return Result<PyReport>(Status::NotFound, name + ": " + chosen.error().message);

    // The file to fetch: a wheel that fits this interpreter and machine.
    const auto& files = (*value)["releases"][*chosen];
    PyPackage p;
    p.name = name;
    p.version = *chosen;
    std::string sdist_seen;
    for (size_t i = 0; i < files.size(); ++i) {
      const auto& f = files[i];
      std::string filename = f["filename"].as_text();
      if (f["yanked"].truthy()) continue;
      if (filename.size() > 7 && filename.compare(filename.size() - 7, 7, ".tar.gz") == 0) sdist_seen = filename;
      if (!wheel_fits(filename, opt.python, platform)) continue;
      // A pure-Python wheel is preferred over a platform one when both fit.
      bool pure = filename.find("-none-any.whl") != std::string::npos;
      if (p.wheel.empty() || (pure && p.wheel.find("-none-any.whl") == std::string::npos)) {
        p.wheel = filename;
        p.resolved = f["url"].as_text();
        p.sha256 = f["digests"]["sha256"].as_text();
      }
    }
    if (p.wheel.empty()) {
      std::string why = name + " " + *chosen + ": no wheel fits " + rep.environment;
      if (!sdist_seen.empty())
        why += " — the index has " + sdist_seen + ", and a source distribution has to be BUILT, which this machine "
               "does not do (it would mean running the package's own setup code)";
      return Result<PyReport>(Status::Unsupported, why);
    }
    if (p.sha256.empty())
      return Result<PyReport>(Status::Denied, p.wheel + ": the index published no sha256, so it cannot be checked");

    auto whl = fetch(kernel_, p.resolved, opt.timeout_ms, nullptr, false);
    if (!whl.ok()) return Result<PyReport>(whl.error());
    ByteSpan bytes(whl->body.data(), whl->body.size());
    p.bytes = bytes.size();
    rep.downloaded_bytes += p.bytes;
    {
      auto got = crypto::sha256(bytes);
      if (hex_of(ByteSpan(got.data(), got.size())) != lower(p.sha256))
        return Result<PyReport>(Status::Corrupt,
                               p.wheel + ": the download does not match the sha256 the index published for it");
    }

    auto entries = unzip(bytes);
    if (!entries.ok()) return Result<PyReport>(entries.error());
    for (const auto& e : *entries) {
      if (e.path.empty() || e.path.find("..") != std::string::npos) continue;
      // A wheel's root IS the site-packages layout, with one exception: the
      // .data directory holds things for places this machine is not installing to.
      if (e.path.find(".data/") != std::string::npos) continue;
      std::string path = dir + "/" + e.path;
      if (e.is_dir) { make_dirs(h, path); continue; }
      size_t at = path.rfind('/');
      if (at != std::string::npos) make_dirs(h, path.substr(0, at));
      if (auto r = h.write_file(path, ByteSpan(e.data.data(), e.data.size())); !r.ok()) return Result<PyReport>(r.error());
      if (e.mode & 0100) h.set_access(path, hal::Host::FileAccess::Executable);
      ++p.files;
    }
    done[canon(name)] = *chosen;
    rep.installed.push_back(p);

    if (opt.with_dependencies) {
      // What this version says it needs, from the index's own metadata rather
      // than from inside the wheel.
      auto per_version = fetch(kernel_, opt.index + "/pypi/" + name + "/" + *chosen + kDocumentSuffix, opt.timeout_ms, nullptr);
      if (per_version.ok()) {
        auto v2 = channel::rpc_parse(ByteSpan(per_version->body.data(), per_version->body.size()));
        if (v2.ok())
          for (const auto& item : (*v2)["info"]["requires_dist"].items()) {
            Requirement r = parse_requirement(item.as_text(), opt.python);
            if (r.wanted && !r.name.empty() && !done.count(canon(r.name))) queue.push_back({r.name, r.specifiers});
          }
      }
    }
  }

  rep.took_ns = h.monotonic_ns() - t0;
  return rep;
}

}  // namespace aios::userland
