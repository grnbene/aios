#include "aios/userland/npm.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "aios/channel/http_client.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/core/base64.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/crypto/sha512.hpp"
#include "aios/userland/git.hpp"   // the inflater this machine already owns

namespace aios::userland {
namespace {

// Three names this machine did not choose: the registry's media type, the
// manifest's filename, and the header a registry expects a client to identify
// itself in. They are spelled once, here, plainly — the harness-smell checker
// exempts these three by name for this file rather than have the code hide
// them from it.
const char* const kMediaType = "application/json";
const char* const kManifestName = "package.json";
const char* const kUserAgentHeader = "User-Agent";

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

uint64_t octal(ByteSpan field) {
  uint64_t v = 0;
  for (uint8_t c : field) {
    if (c == 0 || c == ' ') break;
    if (c < '0' || c > '7') continue;
    v = v * 8 + (uint64_t)(c - '0');
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// gzip: a header this machine skips, the deflate stream it already reads, and a
// trailer it checks the length against.
// ---------------------------------------------------------------------------
Result<ByteBuffer> gunzip(ByteSpan gz) {
  if (gz.size() < 18 || gz[0] != 0x1f || gz[1] != 0x8b)
    return Result<ByteBuffer>(Status::Corrupt, "not a gzip stream");
  if (gz[2] != 8) return Result<ByteBuffer>(Status::Unsupported, "gzip compression method " + dec(gz[2]));
  uint8_t flags = gz[3];
  size_t p = 10;
  if (flags & 0x04) {                                  // FEXTRA
    if (p + 2 > gz.size()) return Result<ByteBuffer>(Status::Corrupt, "truncated gzip extra field");
    size_t xlen = (size_t)gz[p] | ((size_t)gz[p + 1] << 8);
    p += 2 + xlen;
  }
  if (flags & 0x08) { while (p < gz.size() && gz[p]) ++p; ++p; }        // FNAME
  if (flags & 0x10) { while (p < gz.size() && gz[p]) ++p; ++p; }        // FCOMMENT
  if (flags & 0x02) p += 2;                                            // FHCRC
  if (p >= gz.size()) return Result<ByteBuffer>(Status::Corrupt, "truncated gzip header");

  size_t end = 0;
  auto out = inflate_raw(gz, p, &end);
  if (!out.ok()) return out;
  if (end + 8 <= gz.size()) {
    uint32_t isize = (uint32_t)gz[end + 4] | ((uint32_t)gz[end + 5] << 8) | ((uint32_t)gz[end + 6] << 16) |
                     ((uint32_t)gz[end + 7] << 24);
    if ((uint32_t)(out->size() & 0xffffffffu) != isize)
      return Result<ByteBuffer>(Status::Corrupt,
                               "the gzip trailer says " + dec(isize) + " bytes and the stream held " + dec(out->size()));
  }
  return out;
}

// ---------------------------------------------------------------------------
// tar: 512-byte headers, 512-byte blocks, and the two extensions npm's
// tarballs actually use (GNU long names and pax records).
// ---------------------------------------------------------------------------
Result<std::vector<TarEntry>> untar(ByteSpan tar) {
  std::vector<TarEntry> out;
  size_t p = 0;
  std::string pending_long_name;
  while (p + 512 <= tar.size()) {
    ByteSpan h = tar.subspan(p, 512);
    bool all_zero = true;
    for (uint8_t c : h) if (c) { all_zero = false; break; }
    if (all_zero) break;                                  // the end-of-archive marker

    std::string name;
    for (size_t i = 0; i < 100 && h[i]; ++i) name += (char)h[i];
    std::string prefix;
    for (size_t i = 345; i < 500 && h[i]; ++i) prefix += (char)h[i];
    if (!prefix.empty()) name = prefix + "/" + name;
    uint64_t size = octal(h.subspan(124, 12));
    uint32_t mode = (uint32_t)octal(h.subspan(100, 8));
    char type = (char)h[156];
    p += 512;
    size_t data_blocks = (size + 511) / 512 * 512;
    if (p + size > tar.size()) return Result<std::vector<TarEntry>>(Status::Corrupt, "tar entry runs past the end");
    ByteSpan data = tar.subspan(p, size);

    if (type == 'L') {                                    // GNU long name for the NEXT entry
      pending_long_name.assign((const char*)data.data(), data.size());
      while (!pending_long_name.empty() && pending_long_name.back() == '\0') pending_long_name.pop_back();
    } else if (type == 'x' || type == 'g') {              // pax records: "<len> path=<value>\n"
      std::string rec((const char*)data.data(), data.size());
      size_t at = rec.find(" path=");
      if (at != std::string::npos && type == 'x') {
        size_t stop = rec.find('\n', at);
        pending_long_name = rec.substr(at + 6, stop == std::string::npos ? std::string::npos : stop - at - 6);
      }
    } else if (type == '0' || type == 0 || type == '5') {
      TarEntry e;
      e.path = pending_long_name.empty() ? name : pending_long_name;
      pending_long_name.clear();
      e.mode = mode ? mode : 0644;
      e.is_dir = (type == '5');
      if (!e.is_dir) e.data.assign(data.begin(), data.end());
      out.push_back(std::move(e));
    } else {
      pending_long_name.clear();                          // links and devices: not content a package needs
    }
    p += data_blocks;
  }
  if (out.empty()) return Result<std::vector<TarEntry>>(Status::Corrupt, "no files in this archive");
  return out;
}

// ---------------------------------------------------------------------------
// semver, the part a package manager cannot avoid
// ---------------------------------------------------------------------------
namespace {

struct Version {
  uint64_t major = 0, minor = 0, patch = 0;
  std::string pre;                 // "-beta.1", which sorts BELOW the release
  bool ok = false;
};

Version parse_version(const std::string& s) {
  Version v;
  size_t i = 0;
  auto number = [&](uint64_t& out) {
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    out = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') out = out * 10 + (uint64_t)(s[i++] - '0');
    return true;
  };
  if (!number(v.major)) return v;
  if (i < s.size() && s[i] == '.') { ++i; if (!number(v.minor)) return v; }
  if (i < s.size() && s[i] == '.') { ++i; if (!number(v.patch)) return v; }
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) v.pre = s.substr(i);
  v.ok = true;
  return v;
}

}  // namespace

int semver_compare(const std::string& a, const std::string& b) {
  Version x = parse_version(a), y = parse_version(b);
  if (x.major != y.major) return x.major < y.major ? -1 : 1;
  if (x.minor != y.minor) return x.minor < y.minor ? -1 : 1;
  if (x.patch != y.patch) return x.patch < y.patch ? -1 : 1;
  // A prerelease is older than the release it leads to.
  if (x.pre.empty() != y.pre.empty()) return x.pre.empty() ? 1 : -1;
  if (x.pre != y.pre) return x.pre < y.pre ? -1 : 1;
  return 0;
}

namespace {

// A comparator is an operator and a bound; a range is alternatives (||) of
// conjunctions (space-separated), which is exactly how npm writes them.
struct Comparator { char op; Version bound; };   // '>' '<' 'G'(>=) 'L'(<=) '=' '*'

// ^1.2.3, ~1.2.3, "1", "1.2" and "1.2.x" are all shorthand for a pair of
// bounds; expanding them here means one matcher below instead of five.
bool expand(const std::string& token, std::vector<Comparator>& out) {
  if (token.empty() || token == "*" || token == "x" || token == "X" || token == "latest") {
    out.push_back({'*', Version{}});
    return true;
  }
  char op = 0;
  size_t i = 0;
  if (token[0] == '^' || token[0] == '~' || token[0] == '=' || token[0] == 'v') { op = token[0]; i = 1; }
  else if (token.rfind(">=", 0) == 0) { op = 'G'; i = 2; }
  else if (token.rfind("<=", 0) == 0) { op = 'L'; i = 2; }
  else if (token[0] == '>') { op = '>'; i = 1; }
  else if (token[0] == '<') { op = '<'; i = 1; }

  std::string rest = token.substr(i);
  // How much of the version was written decides what a partial means.
  size_t dots = 0;
  bool wild = false;
  for (char c : rest) {
    if (c == '.') ++dots;
    if (c == 'x' || c == 'X' || c == '*') wild = true;
  }
  std::string clean;
  for (char c : rest) { if (c == 'x' || c == 'X' || c == '*') break; clean += c; }
  while (!clean.empty() && clean.back() == '.') clean.pop_back();
  Version v = parse_version(clean);
  if (!v.ok) return false;
  if (wild) dots = clean.empty() ? 0 : (size_t)std::count(clean.begin(), clean.end(), '.');

  auto upper_after = [&](uint64_t major, uint64_t minor, bool bump_minor) {
    Version u;
    if (bump_minor) { u.major = major; u.minor = minor + 1; }
    else { u.major = major + 1; u.minor = 0; }
    u.patch = 0;
    u.ok = true;
    return u;
  };

  if (op == '^') {
    out.push_back({'G', v});
    if (v.major > 0) out.push_back({'<', upper_after(v.major, v.minor, false)});
    else if (v.minor > 0 || dots >= 1) out.push_back({'<', upper_after(0, v.minor, true)});
    else out.push_back({'<', upper_after(0, 0, false)});
    return true;
  }
  if (op == '~') {
    out.push_back({'G', v});
    out.push_back({'<', dots >= 1 ? upper_after(v.major, v.minor, true) : upper_after(v.major, v.minor, false)});
    return true;
  }
  if (op == 'G' || op == 'L' || op == '>' || op == '<') { out.push_back({op, v}); return true; }
  if (op == '=' || op == 'v') { out.push_back({'=', v}); return true; }

  // No operator: a full version is exact, a partial one is a range.
  if (dots >= 2 && !wild) { out.push_back({'=', v}); return true; }
  out.push_back({'G', v});
  out.push_back({'<', dots >= 1 ? upper_after(v.major, v.minor, true) : upper_after(v.major, v.minor, false)});
  return true;
}

int cmp_versions(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
  if (a.pre != b.pre) return a.pre < b.pre ? -1 : 1;
  return 0;
}

bool satisfies(const Version& v, const std::vector<Comparator>& group) {
  for (const auto& c : group) {
    int r = cmp_versions(v, c.bound);
    switch (c.op) {
      case '*': break;
      case '=': if (r != 0) return false; break;
      case 'G': if (r < 0) return false; break;
      case 'L': if (r > 0) return false; break;
      case '>': if (r <= 0) return false; break;
      case '<': if (r >= 0) return false; break;
      default: return false;
    }
  }
  return true;
}

}  // namespace

Result<std::string> semver_pick(const std::vector<std::string>& available, const std::string& range) {
  std::string want;
  for (char c : range) want += (c == '\t') ? ' ' : c;
  while (!want.empty() && want.front() == ' ') want.erase(want.begin());
  while (!want.empty() && want.back() == ' ') want.pop_back();

  // An exact match on what the registry lists wins outright: a tag like "next"
  // or a version with build metadata never needs interpreting.
  for (const auto& v : available)
    if (v == want) return v;

  // Alternatives first, then the conjunction inside each.
  std::vector<std::vector<Comparator>> groups;
  size_t at = 0;
  while (at <= want.size()) {
    size_t bar = want.find("||", at);
    std::string part = want.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
    at = bar == std::string::npos ? want.size() + 1 : bar + 2;
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : part) {
      if (c == ' ') { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } }
      else cur += c;
    }
    if (!cur.empty()) tokens.push_back(cur);
    // ">= 2.1.2" is written with a space as often as ">=2.1.2"; an operator
    // standing alone belongs to the version that follows it.
    for (size_t i = 0; i + 1 < tokens.size();) {
      const std::string& t = tokens[i];
      if (t == ">=" || t == "<=" || t == ">" || t == "<" || t == "=" || t == "^" || t == "~") {
        tokens[i] += tokens[i + 1];
        tokens.erase(tokens.begin() + (long)i + 1);
      } else {
        ++i;
      }
    }
    std::vector<Comparator> group;
    bool ok = true;
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (tokens[i] == "-" && i + 1 < tokens.size() && !group.empty()) {
        // "1.2.3 - 2.3.4": the token before was the floor, the next is the ceiling.
        for (auto& c : group) if (c.op == '=' || c.op == 'G') c.op = 'G';
        Version hi = parse_version(tokens[i + 1]);
        if (!hi.ok) { ok = false; break; }
        group.push_back({'L', hi});
        ++i;
        continue;
      }
      if (!expand(tokens[i], group)) { ok = false; break; }
    }
    if (ok && !group.empty()) groups.push_back(group);
    if (bar == std::string::npos) break;
  }
  if (groups.empty())
    return Result<std::string>(Status::Unsupported, "this machine does not read the version range '" + range + "'");

  std::string best;
  for (const auto& v : available) {
    Version pv = parse_version(v);
    if (!pv.ok || !pv.pre.empty()) continue;          // a range never means a prerelease
    bool any = false;
    for (const auto& g : groups) if (satisfies(pv, g)) { any = true; break; }
    if (!any) continue;
    if (best.empty() || semver_compare(v, best) > 0) best = v;
  }
  if (best.empty()) return Result<std::string>(Status::NotFound, "no published version satisfies " + range);
  return best;
}

// ---------------------------------------------------------------------------
// The installer
// ---------------------------------------------------------------------------
Npm::Npm(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

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
  req.headers.push_back({kUserAgentHeader, "aios-npm/1"});
  req.headers.push_back({"Accept", as_document ? kMediaType : "*/*"});
  auto r = channel::http_send_noted(req, note);
  if (!r.ok()) return r;
  if (r->status == 301 || r->status == 302 || r->status == 307 || r->status == 308) {
    std::string to = r->header("Location");
    if (!to.empty() && to != url) return fetch(kernel, to, timeout_ms, note, as_document);
  }
  if (r->status != 200)
    return Result<channel::HttpResponse>(Status::NotFound,
                                        url + ": the registry answered " + dec((uint64_t)r->status) + " " + r->reason);
  return r;
}

// The registry publishes "sha512-<base64>" (or an older hex sha1); a tarball
// that does not hash to it is not the package that was asked for.
Result<void> check_integrity(ByteSpan tarball, const std::string& integrity, const std::string& shasum) {
  if (integrity.rfind("sha512-", 0) == 0) {
    auto want = base64_decode(integrity.substr(7));
    if (!want.ok()) return Result<void>(Status::Corrupt, "the registry's integrity value is not base64");
    auto got = crypto::sha512(tarball);
    if (want->size() != got.size() || std::memcmp(want->data(), got.data(), got.size()) != 0)
      return Result<void>(Status::Corrupt,
                         "the tarball does not match the sha512 the registry published for it");
    return Ok();
  }
  if (!shasum.empty()) {
    auto got = crypto::sha1(tarball);
    if (hex_of(ByteSpan(got.data(), got.size())) != shasum)
      return Result<void>(Status::Corrupt, "the tarball does not match the sha1 the registry published for it");
    return Ok();
  }
  return Result<void>(Status::Denied, "the registry published no hash for this tarball, so it cannot be checked");
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

Result<std::vector<std::string>> Npm::versions(const std::string& name, std::string& latest, const NpmOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the registry"); !r.ok())
    return Result<std::vector<std::string>>(r.error());
  auto doc = fetch(kernel_, opt.registry + "/" + name, opt.timeout_ms, nullptr);
  if (!doc.ok()) return Result<std::vector<std::string>>(doc.error());
  auto doc_value = channel::rpc_parse(ByteSpan(doc->body.data(), doc->body.size()));
  if (!doc_value.ok()) return Result<std::vector<std::string>>(doc_value.error());
  std::vector<std::string> out;
  for (auto& f : (*doc_value)["versions"].fields()) out.push_back(f.first);
  latest = (*doc_value)["dist-tags"]["latest"].as_text();
  if (out.empty()) return Result<std::vector<std::string>>(Status::NotFound, name + ": the registry lists no versions");
  return out;
}

Result<NpmReport> Npm::install(const std::string& dir, const std::string& spec, const NpmOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the registry"); !r.ok())
    return Result<NpmReport>(r.error());
  if (auto r = may(actor_, ResourceKind::StorageObject, R_Write, "write packages into this machine"); !r.ok())
    return Result<NpmReport>(r.error());

  auto& h = kernel_.host();
  NpmReport rep;
  rep.registry = opt.registry;
  rep.root = dir + "/node_modules";
  uint64_t t0 = h.monotonic_ns();

  // What to install: what was asked for, or what the project asks for.
  std::vector<std::pair<std::string, std::string>> queue;    // name, range
  if (!spec.empty()) {
    std::string name = spec, range = "latest";
    size_t at = spec.rfind('@');
    if (at != std::string::npos && at > 0) { name = spec.substr(0, at); range = spec.substr(at + 1); }
    queue.push_back({name, range});
  } else {
    auto pkg = h.read_file(dir + "/" + kManifestName);
    if (!pkg.ok())
      return Result<NpmReport>(Status::NotFound,
                              dir + "/" + kManifestName + ": nothing here says what to install, and no package was named");
    auto doc_value = channel::rpc_parse(ByteSpan(pkg->data(), pkg->size()));
    if (!doc_value.ok()) return Result<NpmReport>(doc_value.error());
    for (const char* section : {"dependencies", "devDependencies"})
      for (auto& f : (*doc_value)[section].fields()) queue.push_back({f.first, f.second.as_text()});
    if (queue.empty()) return Result<NpmReport>(Status::NotFound, dir + "/" + kManifestName + " lists no dependencies");
  }

  std::unordered_map<std::string, std::string> done;         // name -> version installed
  channel::HttpTransportNote note;
  size_t guard = 0;
  while (!queue.empty()) {
    if (++guard > opt.max_packages)
      return Result<NpmReport>(Status::Exhausted,
                              "stopped after " + dec(opt.max_packages) + " packages; this dependency graph is larger "
                              "than this machine was told to take");
    auto [name, range] = queue.front();
    queue.erase(queue.begin());
    if (done.count(name)) continue;

    auto doc = fetch(kernel_, opt.registry + "/" + name, opt.timeout_ms, &note);
    if (!doc.ok()) return Result<NpmReport>(doc.error());
    if (rep.tls.empty()) { rep.tls = note.tls_version + " " + note.cipher; rep.trust_anchor = note.trust_anchor; }
    auto doc_value = channel::rpc_parse(ByteSpan(doc->body.data(), doc->body.size()));
    if (!doc_value.ok()) return Result<NpmReport>(doc_value.error());

    std::vector<std::string> available;
    for (auto& f : (*doc_value)["versions"].fields()) available.push_back(f.first);
    std::string wanted = range;
    if (range == "latest" || range.empty()) {
      std::string tag = (*doc_value)["dist-tags"]["latest"].as_text();
      if (!tag.empty()) wanted = tag;
    }
    auto chosen = semver_pick(available, wanted);
    if (!chosen.ok()) return Result<NpmReport>(Status::NotFound, name + ": " + chosen.error().message);

    const auto& v = (*doc_value)["versions"][*chosen];
    NpmPackage p;
    p.name = name;
    p.version = *chosen;
    p.resolved = v["dist"]["tarball"].as_text();
    p.integrity = v["dist"]["integrity"].as_text();
    std::string shasum = v["dist"]["shasum"].as_text();
    if (p.resolved.empty()) return Result<NpmReport>(Status::NotFound, name + "@" + *chosen + ": no tarball published");

    std::string target = rep.root + "/" + name;
    auto tar_res = fetch(kernel_, p.resolved, opt.timeout_ms, nullptr, false);
    if (!tar_res.ok()) return Result<NpmReport>(tar_res.error());
    ByteSpan gz(tar_res->body.data(), tar_res->body.size());
    p.bytes = gz.size();
    rep.downloaded_bytes += p.bytes;
    if (auto r = check_integrity(gz, p.integrity, shasum); !r.ok())
      return Result<NpmReport>(Status::Corrupt, name + "@" + *chosen + ": " + r.error().message);

    auto tar = gunzip(gz);
    if (!tar.ok()) return Result<NpmReport>(tar.error());
    auto files = untar(ByteSpan(tar->data(), tar->size()));
    if (!files.ok()) return Result<NpmReport>(files.error());

    for (const auto& e : *files) {
      // Every npm tarball is rooted at "package/"; that prefix is the archive's,
      // not the package's.
      std::string rel = e.path;
      size_t slash = rel.find('/');
      if (slash == std::string::npos) continue;
      rel = rel.substr(slash + 1);
      if (rel.empty() || rel.find("..") != std::string::npos) continue;   // never above the target
      std::string path = target + "/" + rel;
      if (e.is_dir) { make_dirs(h, path); continue; }
      size_t at2 = path.rfind('/');
      if (at2 != std::string::npos) make_dirs(h, path.substr(0, at2));
      if (auto r = h.write_file(path, ByteSpan(e.data.data(), e.data.size())); !r.ok()) return Result<NpmReport>(r.error());
      if (e.mode & 0100) h.set_access(path, hal::Host::FileAccess::Executable);
      ++p.files;
    }
    done[name] = *chosen;
    rep.installed.push_back(p);

    if (opt.with_dependencies)
      for (auto& f : v["dependencies"].fields())
        if (!done.count(f.first)) queue.push_back({f.first, f.second.as_text()});
  }

  rep.took_ns = h.monotonic_ns() - t0;
  return rep;
}

}  // namespace aios::userland
