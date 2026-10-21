#include "aios/userland/gomod.hpp"

#include <algorithm>
#include <map>

#include "aios/channel/http_client.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/core/base64.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/userland/npm.hpp"    // the semver comparison, already written and already driven

namespace aios::userland {
namespace {

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

Result<channel::HttpResponse> fetch(Kernel& kernel, const std::string& url, int timeout_ms,
                                    channel::HttpTransportNote* note) {
  auto ep = channel::parse_endpoint(url);
  if (!ep.ok()) return Result<channel::HttpResponse>(ep.error());
  channel::HttpRequest req;
  req.endpoint = *ep;
  req.timeout_ms = timeout_ms;
  req.host = &kernel.host();
  req.kernel_root = kernel.config().root;
  auto r = channel::http_send_noted(req, note);
  if (!r.ok()) return r;
  if (r->status == 301 || r->status == 302 || r->status == 307 || r->status == 308) {
    std::string to = r->header("Location");
    if (!to.empty() && to != url) {
      if (to[0] == '/') {
        auto base = channel::parse_endpoint(url);
        if (base.ok()) to = std::string("https://") + base->host + to;
      }
      return fetch(kernel, to, timeout_ms, note);
    }
  }
  if (r->status != 200)
    return Result<channel::HttpResponse>(Status::NotFound,
                                        url + ": the proxy answered " + dec((uint64_t)r->status) + " " + r->reason);
  return r;
}

Result<void> make_dirs(hal::Host& h, const std::string& path) {
  std::string acc;
  for (size_t i = 0; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') {
      acc = path.substr(0, i);
      if (!acc.empty() && acc != "/") h.make_dir(acc);
    }
  return Ok();
}

// Go's own comparison, near enough for selection: the shapes it uses are
// semver with a leading v, and the pseudo-versions sort by their date because
// that is where their numbers are.
int go_version_compare(const std::string& a, const std::string& b) {
  auto strip = [](const std::string& v) { return (!v.empty() && v[0] == 'v') ? v.substr(1) : v; };
  return semver_compare(strip(a), strip(b));
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

std::vector<std::string> fields(const std::string& line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    if (j > i) out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

std::vector<std::string> lines_of(ByteSpan text) {
  std::vector<std::string> out;
  std::string acc;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') { out.push_back(acc); acc.clear(); }
    else acc += (char)text[i];
  }
  if (!acc.empty()) out.push_back(acc);
  return out;
}

}  // namespace

std::string go_escape(const std::string& path) {
  std::string out;
  for (char c : path) {
    if (c >= 'A' && c <= 'Z') { out += '!'; out += (char)(c - 'A' + 'a'); }
    else out += c;
  }
  return out;
}

std::string go_dirhash_one(const std::string& name, ByteSpan content) {
  auto h = crypto::sha256(content);
  std::string line = hex_of(ByteSpan(h.data(), h.size())) + "  " + name + "\n";
  auto outer = crypto::sha256(ByteSpan((const uint8_t*)line.data(), line.size()));
  return "h1:" + base64_encode(ByteSpan(outer.data(), outer.size()));
}

std::string go_dirhash(const std::vector<ArchiveEntry>& files) {
  // The listing is what is hashed, not the archive: every file's own sha256
  // beside its name, sorted by name. Two proxies that pack the same source
  // differently still produce the same h1.
  std::vector<std::pair<std::string, std::string>> lines;
  for (const auto& f : files) {
    if (f.is_dir) continue;
    if (f.path.find('\n') != std::string::npos) continue;
    auto h = crypto::sha256(ByteSpan(f.data.data(), f.data.size()));
    lines.push_back({f.path, hex_of(ByteSpan(h.data(), h.size())) + "  " + f.path + "\n"});
  }
  std::sort(lines.begin(), lines.end(),
            [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
              return a.first < b.first;
            });
  crypto::SHA256Context ctx;
  crypto::sha256_init(ctx);
  for (const auto& l : lines) crypto::sha256_update(ctx, (const uint8_t*)l.second.data(), l.second.size());
  uint8_t out[32];
  crypto::sha256_final(ctx, out);
  return "h1:" + base64_encode(ByteSpan(out, 32));
}

Result<GoModFile> parse_go_mod(ByteSpan text) {
  GoModFile f;
  bool in_require = false, in_other_block = false;
  for (const auto& raw : lines_of(text)) {
    std::string line = raw;
    // A comment is a comment, except that "// indirect" is how a requirement
    // says it is nobody's direct business.
    bool indirect = line.find("// indirect") != std::string::npos;
    size_t comment = line.find("//");
    if (comment != std::string::npos) line = line.substr(0, comment);
    line = trim(line);
    if (line.empty()) continue;
    if (in_other_block) { if (line == ")") in_other_block = false; continue; }
    if (in_require) {
      if (line == ")") { in_require = false; continue; }
      auto p = fields(line);
      if (p.size() >= 2) f.require.push_back({unquote(p[0]), unquote(p[1]), indirect});
      continue;
    }
    auto p = fields(line);
    if (p[0] == "module" && p.size() >= 2) { f.module = unquote(p[1]); continue; }
    if (p[0] == "go" && p.size() >= 2) { f.go = p[1]; continue; }
    if (p[0] == "require") {
      if (p.size() >= 2 && p[1] == "(") { in_require = true; continue; }
      if (p.size() >= 3) f.require.push_back({unquote(p[1]), unquote(p[2]), indirect});
      continue;
    }
    // exclude, replace, retract and toolchain lines belong to a build this
    // machine is not performing; a block of them is skipped whole.
    if ((p[0] == "exclude" || p[0] == "replace" || p[0] == "retract") && p.size() >= 2 && p[1] == "(")
      in_other_block = true;
  }
  if (f.module.empty()) return Result<GoModFile>(Status::Invalid, "this go.mod names no module");
  return f;
}

GoModules::GoModules(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

Result<std::vector<std::string>> GoModules::versions(const std::string& path, std::string& latest,
                                                     const GoOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the module proxy"); !r.ok())
    return Result<std::vector<std::string>>(r.error());
  auto list = fetch(kernel_, opt.proxy + "/" + go_escape(path) + "/@v/list", opt.timeout_ms, nullptr);
  if (!list.ok()) return Result<std::vector<std::string>>(list.error());
  std::vector<std::string> out;
  for (const auto& l : lines_of(ByteSpan(list->body.data(), list->body.size()))) {
    std::string v = trim(l);
    if (!v.empty()) out.push_back(v);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return go_version_compare(a, b) < 0;
  });
  auto newest = fetch(kernel_, opt.proxy + "/" + go_escape(path) + "/@latest", opt.timeout_ms, nullptr);
  if (newest.ok()) {
    auto v = channel::rpc_parse(ByteSpan(newest->body.data(), newest->body.size()));
    if (v.ok()) latest = (*v)["Version"].as_text();
  }
  if (out.empty() && latest.empty())
    return Result<std::vector<std::string>>(Status::NotFound, path + ": the proxy lists no versions");
  return out;
}


namespace {

// Every file under `at`, named relative to `prefix`, so a tree on disk can be
// hashed by the same rule the archive was.
void walk_tree(hal::Host& h, const std::string& at, const std::string& prefix,
               const std::vector<std::string>& stop_at, std::vector<ArchiveEntry>& out) {
  auto names = h.list_dir(at);
  if (!names.ok()) return;
  for (const auto& name : *names) {
    if (name == "." || name == "..") continue;
    std::string path = at + "/" + name;
    std::string rel = prefix.empty() ? name : prefix + "/" + name;
    bool skip = false;
    for (const auto& s : stop_at) if (rel == s) skip = true;   // another module's tree begins here
    if (skip) continue;
    auto info = h.stat(path);
    if (!info.ok()) continue;
    if (info->is_dir) { walk_tree(h, path, rel, stop_at, out); continue; }
    auto data = h.read_file(path);
    if (!data.ok()) continue;
    ArchiveEntry e;
    e.path = rel;
    e.data = *data;
    out.push_back(e);
  }
}

}  // namespace

Result<GoReport> GoModules::download(const std::string& dir, const std::string& spec, const GoOptions& opt) {
  if (auto r = may(actor_, ResourceKind::Network, R_Consume, "reach the module proxy"); !r.ok())
    return Result<GoReport>(r.error());
  if (auto r = may(actor_, ResourceKind::StorageObject, R_Write, "write modules into this machine"); !r.ok())
    return Result<GoReport>(r.error());

  auto& h = kernel_.host();
  GoReport rep;
  rep.proxy = opt.proxy;
  rep.sumdb = opt.verify_sums ? opt.sumdb : std::string("not consulted — the proxy is being taken at its word");
  rep.target = dir + "/vendor";
  uint64_t t0 = h.monotonic_ns();

  std::string path = spec, query;
  size_t at = spec.rfind('@');
  if (at != std::string::npos) { path = spec.substr(0, at); query = spec.substr(at + 1); }
  if (path.empty()) return Result<GoReport>(Status::Invalid, "name the module to download");

  channel::HttpTransportNote note;
  auto resolve = [&](const std::string& p, const std::string& q) -> Result<std::string> {
    if (q.empty() || q == "latest") {
      auto r = fetch(kernel_, opt.proxy + "/" + go_escape(p) + "/@latest", opt.timeout_ms, &note);
      if (!r.ok()) return Result<std::string>(r.error());
      auto v = channel::rpc_parse(ByteSpan(r->body.data(), r->body.size()));
      if (!v.ok()) return Result<std::string>(v.error());
      std::string got = (*v)["Version"].as_text();
      if (got.empty()) return Result<std::string>(Status::NotFound, p + ": the proxy names no latest version");
      return got;
    }
    if (q.size() > 1 && q[0] == 'v' && q.find('.') != std::string::npos && q.find('.') != q.rfind('.')) return q;
    // A partial query ("v1", "v1.5") takes the highest version that starts with it.
    std::string latest;
    auto all = versions(p, latest, opt);
    if (!all.ok()) return Result<std::string>(all.error());
    std::string best;
    for (const auto& v : *all)
      if (v.compare(0, q.size(), q) == 0 && (best.empty() || go_version_compare(v, best) > 0)) best = v;
    if (best.empty()) return Result<std::string>(Status::NotFound, p + ": no version answers to " + q);
    return best;
  };

  auto first = resolve(path, query);
  if (!first.ok()) return Result<GoReport>(first.error());

  // Minimal version selection: walk the requirement graph, and for each module
  // keep the highest version anything asked for. Nothing is "resolved" to a
  // newer release than some go.mod actually names.
  std::map<std::string, std::string> selected;
  std::map<std::string, ByteBuffer> gomods;         // "<path>@<version>" -> the go.mod bytes
  std::vector<std::pair<std::string, std::string>> queue{{path, *first}};
  while (!queue.empty()) {
    auto [p, v] = queue.front();
    queue.erase(queue.begin());
    auto have = selected.find(p);
    if (have != selected.end() && go_version_compare(have->second, v) >= 0) continue;
    if (have == selected.end() && selected.size() >= opt.max_modules)
      return Result<GoReport>(Status::Exhausted,
                             "stopped after " + dec(opt.max_modules) + " modules; this requirement graph is larger "
                             "than this machine was told to take");
    selected[p] = v;
    std::string key = p + "@" + v;
    if (!gomods.count(key)) {
      auto r = fetch(kernel_, opt.proxy + "/" + go_escape(p) + "/@v/" + go_escape(v) + ".mod", opt.timeout_ms, &note);
      if (!r.ok()) return Result<GoReport>(r.error());
      if (rep.tls.empty()) { rep.tls = note.tls_version + " " + note.cipher; rep.trust_anchor = note.trust_anchor; }
      gomods[key] = r->body;
    }
    if (!opt.with_dependencies) continue;
    auto parsed = parse_go_mod(ByteSpan(gomods[key].data(), gomods[key].size()));
    if (!parsed.ok()) continue;   // a module older than go.mod itself has none to read
    for (const auto& req : parsed->require)
      if (!req.path.empty() && !req.version.empty()) queue.push_back({req.path, req.version});
  }

  ChecksumDatabase db(kernel_, opt.sumdb, opt.sumdb_key.empty() ? kGoSumDatabaseKey : opt.sumdb_key, opt.timeout_ms);
  std::vector<std::string> sum_lines;
  std::string modules_txt;

  for (const auto& [p, v] : selected) {
    GoModule m;
    m.path = p;
    m.version = v;
    m.explicitly_asked = (p == path);
    m.origin = opt.proxy;

    std::string key = p + "@" + v;
    m.mod_hash = go_dirhash_one("go.mod", ByteSpan(gomods[key].data(), gomods[key].size()));

    auto zip = fetch(kernel_, opt.proxy + "/" + go_escape(p) + "/@v/" + go_escape(v) + ".zip", opt.timeout_ms, &note);
    if (!zip.ok()) return Result<GoReport>(zip.error());
    if (rep.tls.empty()) { rep.tls = note.tls_version + " " + note.cipher; rep.trust_anchor = note.trust_anchor; }
    m.bytes = zip->body.size();
    rep.downloaded_bytes += m.bytes;

    auto files = unzip(ByteSpan(zip->body.data(), zip->body.size()));
    if (!files.ok()) return Result<GoReport>(files.error());
    m.zip_hash = go_dirhash(*files);

    if (opt.verify_sums) {
      auto rec = db.lookup(go_escape(p), go_escape(v), &rep.tree);
      if (!rec.ok()) return Result<GoReport>(rec.error());
      if (rec->hash != m.zip_hash)
        return Result<GoReport>(Status::Corrupt,
                               key + ": the module this proxy served hashes to " + m.zip_hash + ", and the checksum "
                               "database recorded " + rec->hash + " — the two are not the same module");
      if (!rec->mod_hash.empty() && rec->mod_hash != m.mod_hash)
        return Result<GoReport>(Status::Corrupt, key + ": its go.mod does not match the recorded hash");
      m.sum_verified = true;
    }

    std::string prefix = p + "@" + v + "/";
    std::string base = rep.target + "/" + p;
    std::vector<std::string> package_dirs;
    for (const auto& e : *files) {
      if (e.is_dir) continue;
      if (e.path.compare(0, prefix.size(), prefix) != 0) continue;
      std::string rel = e.path.substr(prefix.size());
      if (rel.empty() || rel.find("..") != std::string::npos) continue;
      std::string full = base + "/" + rel;
      size_t slash = full.rfind('/');
      if (slash != std::string::npos) make_dirs(h, full.substr(0, slash));
      if (auto r = h.write_file(full, ByteSpan(e.data.data(), e.data.size())); !r.ok())
        return Result<GoReport>(r.error());
      ++m.files;
      // What a build would call a package: a directory holding Go source, with
      // the directories Go itself never builds left out.
      if (rel.size() > 3 && rel.compare(rel.size() - 3, 3, ".go") == 0) {
        std::string d = (rel.rfind('/') == std::string::npos) ? std::string() : rel.substr(0, rel.rfind('/'));
        bool ignored = false;
        for (size_t i = 0; i <= d.size(); ++i)
          if (i == 0 || i == d.size() || d[i] == '/') {
            size_t start = (i == 0) ? 0 : i + 1;
            if (start < d.size() && (d[start] == '_' || d[start] == '.')) ignored = true;
          }
        if (d == "testdata" || d.find("/testdata") != std::string::npos) ignored = true;
        std::string pkg = d.empty() ? p : p + "/" + d;
        if (!ignored && std::find(package_dirs.begin(), package_dirs.end(), pkg) == package_dirs.end())
          package_dirs.push_back(pkg);
      }
    }
    std::sort(package_dirs.begin(), package_dirs.end());
    m.packages = package_dirs;

    sum_lines.push_back(p + " " + v + " " + m.zip_hash);
    sum_lines.push_back(p + " " + v + "/go.mod " + m.mod_hash);

    auto parsed = parse_go_mod(ByteSpan(gomods[key].data(), gomods[key].size()));
    modules_txt += "# " + p + " " + v + "\n";
    if (m.explicitly_asked) {
      modules_txt += "## explicit";
      if (parsed.ok() && !parsed->go.empty()) modules_txt += "; go " + parsed->go;
      modules_txt += "\n";
    }
    for (const auto& pkg : package_dirs) modules_txt += pkg + "\n";
    rep.modules.push_back(m);
  }

  std::sort(sum_lines.begin(), sum_lines.end());
  std::string sum;
  for (const auto& l : sum_lines) sum += l + "\n";
  make_dirs(h, dir);
  if (auto r = h.write_file(dir + "/go.sum", ByteSpan((const uint8_t*)sum.data(), sum.size())); !r.ok())
    return Result<GoReport>(r.error());
  if (auto r = h.write_file(rep.target + "/modules.txt",
                            ByteSpan((const uint8_t*)modules_txt.data(), modules_txt.size())); !r.ok())
    return Result<GoReport>(r.error());

  rep.took_ns = h.monotonic_ns() - t0;
  return rep;
}

Result<GoReport> GoModules::verify(const std::string& dir, const GoOptions& opt) {
  if (auto r = may(actor_, ResourceKind::StorageObject, R_Read, "read the modules on this machine"); !r.ok())
    return Result<GoReport>(r.error());
  auto& h = kernel_.host();
  GoReport rep;
  rep.proxy = opt.proxy;
  rep.target = dir + "/vendor";
  uint64_t t0 = h.monotonic_ns();

  auto sum = h.read_file(dir + "/go.sum");
  if (!sum.ok())
    return Result<GoReport>(Status::NotFound, dir + "/go.sum: nothing here records what these modules should hash to");

  std::map<std::string, std::string> want_zip, want_mod;
  std::vector<std::string> roots;
  for (const auto& line : lines_of(ByteSpan(sum->data(), sum->size()))) {
    auto f = fields(line);
    if (f.size() != 3) continue;
    if (f[1].size() > 7 && f[1].compare(f[1].size() - 7, 7, "/go.mod") == 0) {
      want_mod[f[0] + "@" + f[1].substr(0, f[1].size() - 7)] = f[2];
    } else {
      want_zip[f[0] + "@" + f[1]] = f[2];
      roots.push_back(f[0]);
    }
  }
  if (want_zip.empty()) return Result<GoReport>(Status::Invalid, dir + "/go.sum records no modules");

  for (const auto& [key, want] : want_zip) {
    size_t at = key.rfind('@');
    GoModule m;
    m.path = key.substr(0, at);
    m.version = key.substr(at + 1);
    // Another module's tree may live inside this one's directory; it is that
    // module's business, not this one's.
    std::vector<std::string> stop_at;
    for (const auto& other : roots)
      if (other != m.path && other.size() > m.path.size() && other.compare(0, m.path.size(), m.path) == 0 &&
          other[m.path.size()] == '/')
        stop_at.push_back(other.substr(m.path.size() + 1));
    std::vector<ArchiveEntry> files;
    walk_tree(h, rep.target + "/" + m.path, key, stop_at, files);
    if (files.empty())
      return Result<GoReport>(Status::NotFound, key + ": go.sum records it, and it is not in the vendor tree");
    m.files = files.size();
    for (const auto& f : files) m.bytes += f.data.size();
    m.zip_hash = go_dirhash(files);
    auto mod = want_mod.find(key);
    if (mod != want_mod.end()) m.mod_hash = mod->second;
    if (m.zip_hash != want)
      return Result<GoReport>(Status::Corrupt,
                             key + ": what is on disk hashes to " + m.zip_hash + ", and go.sum says " + want +
                             " — this tree has been altered since it was downloaded");
    m.sum_verified = true;
    rep.modules.push_back(m);
  }
  rep.took_ns = h.monotonic_ns() - t0;
  return rep;
}

}  // namespace aios::userland
