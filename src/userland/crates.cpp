#include "aios/userland/crates.hpp"

#include <cstring>
#include <unordered_map>

#include "aios/channel/http_client.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/userland/npm.hpp"     // gunzip, untar and the semver matcher, already written

namespace aios::userland {
namespace {

const char* const kMediaType = "application/json";
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
  // crates.io refuses a client that will not say what it is.
  req.headers.push_back({kUserAgentHeader, "aios-crates/1"});
  req.headers.push_back({"Accept", as_document ? kMediaType : "*/*"});
  auto r = channel::http_send_noted(req, note);
  if (!r.ok()) return r;
  if (r->status == 301 || r->status == 302 || r->status == 307 || r->status == 308) {
    std::string to = r->header("Location");
    if (!to.empty() && to != url) {
      // The download redirects to a CDN, and a relative Location is legal.
      if (to[0] == '/') {
        auto base = channel::parse_endpoint(url);
        if (base.ok()) to = std::string("https://") + base->host + to;
      }
      return fetch(kernel, to, timeout_ms, note, as_document);
    }
  }
  if (r->status != 200)
    return Result<channel::HttpResponse>(Status::NotFound,
                                        url + ": the registry answered " + dec((uint64_t)r->status) + " " + r->reason);
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

Crates::Crates(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

Result<std::vector<std::string>> Crates::versions(const std::string& name, std::string& latest,
                                                 const CrateOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the registry"); !r.ok())
    return Result<std::vector<std::string>>(r.error());
  auto doc = fetch(kernel_, opt.registry + "/api/v1/crates/" + name, opt.timeout_ms, nullptr);
  if (!doc.ok()) return Result<std::vector<std::string>>(doc.error());
  auto value = channel::rpc_parse(ByteSpan(doc->body.data(), doc->body.size()));
  if (!value.ok()) return Result<std::vector<std::string>>(value.error());
  std::vector<std::string> out;
  for (const auto& v : (*value)["versions"].items())
    if (!v["yanked"].truthy()) out.push_back(v["num"].as_text());
  latest = (*value)["crate"]["max_stable_version"].as_text();
  if (out.empty()) return Result<std::vector<std::string>>(Status::NotFound, name + ": the registry lists no versions");
  return out;
}

Result<CrateReport> Crates::vendor(const std::string& dir, const std::string& spec, const CrateOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the registry"); !r.ok())
    return Result<CrateReport>(r.error());
  if (auto r = may(actor_, ResourceKind::StorageObject, R_Write, "write sources into this machine"); !r.ok())
    return Result<CrateReport>(r.error());

  auto& h = kernel_.host();
  CrateReport rep;
  rep.registry = opt.registry;
  rep.target = dir + "/vendor";
  uint64_t t0 = h.monotonic_ns();

  std::vector<std::pair<std::string, std::string>> queue;   // name, requirement
  {
    std::string name = spec, req = "*";
    size_t at = spec.find('@');
    if (at != std::string::npos) { name = spec.substr(0, at); req = spec.substr(at + 1); }
    if (name.empty()) return Result<CrateReport>(Status::Invalid, "name the crate to vendor");
    queue.push_back({name, req});
  }

  std::unordered_map<std::string, std::string> done;
  channel::HttpTransportNote note;
  size_t guard = 0;
  while (!queue.empty()) {
    if (++guard > opt.max_packages)
      return Result<CrateReport>(Status::Exhausted,
                                "stopped after " + dec(opt.max_packages) + " crates; this dependency graph is larger "
                                "than this machine was told to take");
    auto [name, req] = queue.front();
    queue.erase(queue.begin());
    if (done.count(name)) continue;

    auto doc = fetch(kernel_, opt.registry + "/api/v1/crates/" + name, opt.timeout_ms, &note);
    if (!doc.ok()) return Result<CrateReport>(doc.error());
    if (rep.tls.empty()) { rep.tls = note.tls_version + " " + note.cipher; rep.trust_anchor = note.trust_anchor; }
    auto value = channel::rpc_parse(ByteSpan(doc->body.data(), doc->body.size()));
    if (!value.ok()) return Result<CrateReport>(value.error());

    std::vector<std::string> available;
    std::unordered_map<std::string, size_t> where;
    const auto& versions = (*value)["versions"];
    for (size_t i = 0; i < versions.size(); ++i) {
      if (versions[i]["yanked"].truthy()) continue;
      std::string num = versions[i]["num"].as_text();
      available.push_back(num);
      where[num] = i;
    }
    // Cargo's requirements are semver, the same shapes npm writes.
    auto chosen = semver_pick(available, req == "*" ? "*" : req);
    if (!chosen.ok()) return Result<CrateReport>(Status::NotFound, name + ": " + chosen.error().message);
    const auto& v = versions[where[*chosen]];

    CratePackage p;
    p.name = name;
    p.version = *chosen;
    p.checksum = v["checksum"].as_text();
    std::string dl = v["dl_path"].as_text();
    if (dl.empty()) return Result<CrateReport>(Status::NotFound, name + " " + *chosen + ": no download path");
    p.resolved = opt.registry + dl;
    if (p.checksum.empty())
      return Result<CrateReport>(Status::Denied, name + " " + *chosen + ": the registry published no checksum");

    auto crate = fetch(kernel_, p.resolved, opt.timeout_ms, nullptr, false);
    if (!crate.ok()) return Result<CrateReport>(crate.error());
    ByteSpan bytes(crate->body.data(), crate->body.size());
    p.bytes = bytes.size();
    rep.downloaded_bytes += p.bytes;
    {
      auto got = crypto::sha256(bytes);
      if (hex_of(ByteSpan(got.data(), got.size())) != p.checksum)
        return Result<CrateReport>(Status::Corrupt,
                                  name + " " + *chosen + ": the archive does not match the checksum crates.io published");
    }

    // A .crate is a gzipped tar rooted at "<name>-<version>/".
    auto tar = gunzip(bytes);
    if (!tar.ok()) return Result<CrateReport>(tar.error());
    auto files = untar(ByteSpan(tar->data(), tar->size()));
    if (!files.ok()) return Result<CrateReport>(files.error());
    std::string target = rep.target + "/" + name + "-" + *chosen;
    for (const auto& e : *files) {
      std::string rel = e.path;
      size_t slash = rel.find('/');
      if (slash == std::string::npos) continue;
      rel = rel.substr(slash + 1);
      if (rel.empty() || rel.find("..") != std::string::npos) continue;
      std::string path = target + "/" + rel;
      if (e.is_dir) { make_dirs(h, path); continue; }
      size_t at = path.rfind('/');
      if (at != std::string::npos) make_dirs(h, path.substr(0, at));
      if (auto r = h.write_file(path, ByteSpan(e.data.data(), e.data.size())); !r.ok()) return Result<CrateReport>(r.error());
      ++p.files;
    }
    done[name] = *chosen;
    rep.vendored.push_back(p);

    if (opt.with_dependencies) {
      auto deps = fetch(kernel_, opt.registry + "/api/v1/crates/" + name + "/" + *chosen + "/dependencies",
                        opt.timeout_ms, nullptr);
      if (deps.ok()) {
        auto dv = channel::rpc_parse(ByteSpan(deps->body.data(), deps->body.size()));
        if (dv.ok())
          for (const auto& d : (*dv)["dependencies"].items()) {
            // Only what a build of this crate needs: dev-dependencies belong to
            // its own tests, and optional ones to features nobody asked for.
            if (d["kind"].as_text() != "normal") continue;
            if (d["optional"].truthy()) continue;
            std::string dn = d["crate_id"].as_text();
            std::string dr = d["req"].as_text();
            if (!dn.empty() && !done.count(dn)) queue.push_back({dn, dr.empty() ? "*" : dr});
          }
      }
    }
  }

  rep.took_ns = h.monotonic_ns() - t0;
  return rep;
}

}  // namespace aios::userland
