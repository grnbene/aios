#include "aios/userland/sense.hpp"

#include <cstring>

#include "aios/channel/http_client.hpp"
#include "aios/compute/device.hpp"
#include "aios/userland/canvas.hpp"
#include "aios/crypto/sha256.hpp"

#if !defined(AIOS_METAL)
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <vector>
extern char** environ;
#endif

namespace aios::userland {
namespace {

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}

bool starts_with_ci(ByteSpan b, size_t at, const char* what) {
  for (size_t i = 0; what[i]; ++i) {
    if (at + i >= b.size()) return false;
    char c = (char)b[at + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != what[i]) return false;
  }
  return true;
}

// The handful of entities a page actually uses in its prose. Anything else is
// left as it was written rather than guessed at.
bool entity(const std::string& name, std::string& out) {
  if (name == "amp") { out = "&"; return true; }
  if (name == "lt") { out = "<"; return true; }
  if (name == "gt") { out = ">"; return true; }
  if (name == "quot") { out = "\""; return true; }
  if (name == "apos" || name == "#39") { out = "'"; return true; }
  if (name == "nbsp" || name == "#160") { out = " "; return true; }
  if (name == "mdash" || name == "#8212") { out = "—"; return true; }
  if (name == "ndash" || name == "#8211") { out = "–"; return true; }
  if (name == "hellip" || name == "#8230") { out = "…"; return true; }
  if (name == "rsquo" || name == "#8217") { out = "'"; return true; }
  if (name == "lsquo" || name == "#8216") { out = "'"; return true; }
  if (name == "ldquo" || name == "#8220") { out = "\""; return true; }
  if (name == "rdquo" || name == "#8221") { out = "\""; return true; }
  return false;
}


// A page writes most of its links relative to itself. Resolving them here is
// what makes a link something this machine can actually go and fetch.
std::string resolve_url(const std::string& base, const std::string& href) {
  if (href.empty()) return {};
  if (starts_with_ci(ByteSpan((const uint8_t*)href.data(), href.size()), 0, "http://") ||
      starts_with_ci(ByteSpan((const uint8_t*)href.data(), href.size()), 0, "https://"))
    return href;
  size_t scheme = base.find("://");
  if (scheme == std::string::npos) return {};
  if (href.size() > 1 && href[0] == '/' && href[1] == '/') return base.substr(0, scheme + 1) + href;
  size_t host_end = base.find('/', scheme + 3);
  std::string root = host_end == std::string::npos ? base : base.substr(0, host_end);
  if (href[0] == '/') return root + href;
  std::string dir = host_end == std::string::npos ? base + "/" : base.substr(0, base.rfind('/') + 1);
  return dir + href;
}

// The value of an attribute on the tag starting at `at`, quoted or bare.
std::string attribute(ByteSpan b, size_t at, const char* name) {
  size_t end = at;
  while (end < b.size() && b[end] != '>') ++end;
  size_t n = std::strlen(name);
  for (size_t i = at; i + n + 1 < end; ++i) {
    if (!starts_with_ci(b, i, name)) continue;
    char before = (char)b[i - 1];
    if (before != ' ' && before != '\t' && before != '\n' && before != '\r') continue;
    size_t j = i + n;
    while (j < end && (b[j] == ' ' || b[j] == '\t')) ++j;
    if (j >= end || b[j] != '=') continue;
    ++j;
    while (j < end && (b[j] == ' ' || b[j] == '\t')) ++j;
    char quote = 0;
    if (j < end && (b[j] == '"' || b[j] == '\'')) { quote = (char)b[j]; ++j; }
    std::string v;
    while (j < end) {
      char c = (char)b[j];
      if (quote ? c == quote : (c == ' ' || c == '\t')) break;
      v += c;
      ++j;
    }
    return v;
  }
  return {};
}

}  // namespace

std::string page_text(ByteSpan markup, std::string* title, size_t max_text) {
  std::string out;
  std::string pending_entity;
  bool in_entity = false;
  bool grabbing_title = false;
  bool have_title = false;   // the document's title is the FIRST <title>; an inline SVG icon's <title>Lock</title> is not the page's name
  size_t i = 0;
  auto emit = [&](char c) {
    if (out.size() >= max_text) return;
    // One space is as good as forty, and a page is mostly whitespace.
    if (c == ' ' || c == '\t' || c == '\r') {
      if (!out.empty() && out.back() != ' ' && out.back() != '\n') out += ' ';
      return;
    }
    if (c == '\n') {
      if (!out.empty() && out.back() != '\n') { while (!out.empty() && out.back() == ' ') out.pop_back(); out += '\n'; }
      return;
    }
    out += c;
  };

  while (i < markup.size()) {
    if (markup[i] == '<') {
      // Script and style are not what the page says; skip to their close tag.
      const char* skip_to = nullptr;
      if (starts_with_ci(markup, i + 1, "script")) skip_to = "</script";
      else if (starts_with_ci(markup, i + 1, "style")) skip_to = "</style";
      else if (starts_with_ci(markup, i + 1, "head") && !starts_with_ci(markup, i + 1, "header")) skip_to = nullptr;
      if (skip_to) {
        size_t j = i + 1;
        while (j < markup.size() && !starts_with_ci(markup, j, skip_to)) ++j;
        i = j;
        while (i < markup.size() && markup[i] != '>') ++i;
        if (i < markup.size()) ++i;
        continue;
      }
      bool closing = i + 1 < markup.size() && markup[i + 1] == '/';
      if (!closing && !have_title && starts_with_ci(markup, i + 1, "title")) { grabbing_title = true; have_title = true; if (title) title->clear(); }
      if (closing && starts_with_ci(markup, i + 2, "title")) grabbing_title = false;
      // A block-level tag is a line break; everything else just goes away.
      const char* breaks[] = {"p", "br", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
                              "section", "article", "header", "footer", "ul", "ol", "table", "pre", "blockquote"};
      size_t name_at = closing ? i + 2 : i + 1;
      for (const char* b : breaks) {
        size_t n = std::strlen(b);
        if (!starts_with_ci(markup, name_at, b)) continue;
        char after = name_at + n < markup.size() ? (char)markup[name_at + n] : '>';
        if (after == '>' || after == ' ' || after == '/' || after == '\t' || after == '\n') { emit('\n'); break; }
      }
      while (i < markup.size() && markup[i] != '>') ++i;
      if (i < markup.size()) ++i;
      continue;
    }
    char c = (char)markup[i++];
    if (c == '&') { in_entity = true; pending_entity.clear(); continue; }
    if (in_entity) {
      if (c == ';' || pending_entity.size() > 8) {
        std::string what;
        if (c == ';' && entity(pending_entity, what)) { for (char e : what) grabbing_title && title ? (void)(*title += e) : emit(e); }
        else { emit('&'); for (char e : pending_entity) emit(e); if (c != ';') emit(c); }
        in_entity = false;
        continue;
      }
      pending_entity += c;
      continue;
    }
    if (grabbing_title && title) { if (c != '\n' && c != '\r') *title += c; continue; }
    emit(c);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  // The title as it is actually written, with its padding gone.
  if (title) {
    size_t a = 0, b = title->size();
    while (a < b && ((*title)[a] == ' ' || (*title)[a] == '\t')) ++a;
    while (b > a && ((*title)[b - 1] == ' ' || (*title)[b - 1] == '\t')) --b;
    *title = title->substr(a, b - a);
  }
  return out;
}


std::vector<PageLink> page_links(ByteSpan markup, const std::string& base_url, size_t max_links) {
  std::vector<PageLink> out;
  size_t i = 0;
  while (i < markup.size() && out.size() < max_links) {
    if (markup[i] != '<' || !starts_with_ci(markup, i + 1, "a")) { ++i; continue; }
    char after = i + 2 < markup.size() ? (char)markup[i + 2] : '>';
    if (after != ' ' && after != '\t' && after != '\n' && after != '\r') { ++i; continue; }
    std::string href = attribute(markup, i + 2, "href");
    size_t open_end = i;
    while (open_end < markup.size() && markup[open_end] != '>') ++open_end;
    if (open_end >= markup.size()) break;
    ++open_end;
    // The anchor's own text, however much markup is nested inside it.
    size_t close = open_end;
    while (close < markup.size() && !(markup[close] == '<' && starts_with_ci(markup, close + 1, "/a"))) ++close;
    std::string text;
    if (close > open_end)
      text = page_text(ByteSpan(markup.data() + open_end, close - open_end), nullptr, 400);
    for (char& c : text) if (c == '\n' || c == '\t') c = ' ';
    // Collapse what the newlines left behind.
    std::string tidy;
    for (char c : text) {
      if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) continue;
      tidy += c;
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    i = close > open_end ? close : open_end;
    if (href.empty() || href[0] == '#') continue;
    if (starts_with_ci(ByteSpan((const uint8_t*)href.data(), href.size()), 0, "mailto:") ||
        starts_with_ci(ByteSpan((const uint8_t*)href.data(), href.size()), 0, "javascript:"))
      continue;
    std::string url = resolve_url(base_url, href);
    if (url.empty() || tidy.empty()) continue;
    out.push_back(PageLink{url, tidy});
  }
  return out;
}

Sense::Sense(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

Result<Perception> Sense::look(const std::string& url, const SenseOptions& opt) {
  if (actor_) {
    auto may = actor_->may(ResourceRef{ResourceKind::Network, {}}, R_Consume);
    if (!may.ok())
      return Result<Perception>(Status::Denied, "entity " + actor_->self().short_str() +
                                                    " may not look outside this machine: " + may.error().str());
  }
  Perception p;
  p.url = url;
  uint64_t t0 = kernel_.host().monotonic_ns();

  if (url == "frame" || url == "desktop" || url == "screen" || url == "canvas" || url == "about:frame" || url == "frame://desktop") {
    p.url = "frame://desktop";
    p.title = "AIOS Hardware Compositor Framebuffer Desktop";
    p.tls = "internal-framebuffer";
    p.status = 200;
    // Every reading below is asked of the kernel. The previous text printed a
    // 1920x1080 @ 60 FPS display, a GTX 1080 Ti with 28 lanes and "157/157
    // assertions green" as literals, on any machine, including ones with no GPU.
    const std::string mind = bound_mind_name(kernel_);
    const size_t n_caps = kernel_.capabilities().list({}).size();
    std::string devices;
    for (Device* d : kernel_.devices().all()) {
      if (!d) continue;
      if (!devices.empty()) devices += "  |  ";
      devices += d->name() + " (" + std::to_string(d->capacity().lanes) + " lanes)";
    }
    std::string text;
    text += "[AIOS HARDWARE COMPOSITOR FRAMEBUFFER PERCEPTION]\n";
    text += "Header Navigation: AIOS HYPERVISOR | <clock> | MIND: " + (mind.empty() ? std::string("OFFLINE") : mind) +
            " | " + std::to_string(n_caps) + " CAPS | " + (devices.empty() ? std::string("no compute device") : devices) +
            " | [RESTART]\n";
    text += "Centered Input Bar: aios::mind> > Ask or command AIOS resident mind... | [Ask Mind]\n";
    text += "Workstation panes: [F1] Shell, [F2] Machine, [F3] Tasks, [F4] Editor, [F5] DB Studio, "
            "[F6] Web, [F7] Mail, [F8] Contacts, [F9] Calendar, [F10] Apps, [F11] Inspector\n";
    text += "Compute devices: " + (devices.empty() ? std::string("none registered") : devices) + "\n";
#if !defined(AIOS_METAL)
    // Into this machine's own run directory, not one host's home. No shell:
    // scrot gets the path as an argument, so a root with a space or a quote
    // in it is just a path.
    const std::string shot_dir = run_dir(kernel_);
    (void)kernel_.host().make_dir(shot_dir);
    const std::string shot = shot_dir + "/desktop_now.png";
    bool shot_ok = false;
    {
      std::vector<std::string> envs;
      bool has_display = false;
      for (char** e = environ; e && *e; ++e) {
        if (std::strncmp(*e, "DISPLAY=", 8) == 0) has_display = true;
        envs.push_back(*e);
      }
      if (!has_display) envs.push_back("DISPLAY=:0");
      std::vector<char*> envp;
      for (auto& e : envs) envp.push_back(const_cast<char*>(e.c_str()));
      envp.push_back(nullptr);
      std::string a0 = "scrot", a1 = "-z", a2 = "-o";
      std::string a3 = shot;
      char* argv[] = {a0.data(), a1.data(), a2.data(), a3.data(), nullptr};
      posix_spawn_file_actions_t fa;
      posix_spawn_file_actions_init(&fa);
      posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
      pid_t pid = -1;
      if (::posix_spawnp(&pid, "scrot", &fa, nullptr, argv, envp.data()) == 0) {
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        shot_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
      }
      posix_spawn_file_actions_destroy(&fa);
    }
    if (shot_ok) {
      text += "Live Framebuffer Image: " + shot + "\n";
    } else {
      text += "Live Framebuffer Image: unavailable (no X display to capture)\n";
    }
#else
    text += "Live Framebuffer Image: none on metal (the compositor owns the framebuffer directly)\n";
#endif

    p.text = text;
    p.fetched_bytes = text.size();
    p.text_bytes = text.size();

    if (opt.keep && actor_) {
      auto may = actor_->may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write);
      if (may.ok()) {
        StoredObject o;
        o.payload = ByteBuffer(p.text.begin(), p.text.end());
        if (opt.embed) o.embedding = opt.embed(p.title + "\n" + p.text);
        o.metadata.push_back({"url", p.url});
        o.metadata.push_back({"title", p.title});
        o.metadata.push_back({"seen", "sense"});
        auto id = actor_->store(std::move(o));
        if (id.ok()) {
          p.object = *id;
          p.kept = true;
        }
      }
    }
    p.took_ns = kernel_.host().monotonic_ns() - t0;
    return p;
  }

  auto ep = channel::parse_endpoint(url);
  if (!ep.ok()) return Result<Perception>(ep.error());
  channel::HttpRequest req;
  req.endpoint = *ep;
  req.timeout_ms = opt.timeout_ms;
  req.host = &kernel_.host();
  req.kernel_root = kernel_.config().root;
  req.headers.push_back({"Accept", "text/html,text/plain,*/*"});
  // A machine that will not say what it is gets shown the door by a good many
  // servers. This one says exactly what it is.
  req.headers.push_back({"User-Agent", "aios/1 (+bare-metal kernel; the machine's own eyes)"});
  channel::HttpTransportNote note;
  auto r = channel::http_send_noted(req, &note);
  if (!r.ok()) return Result<Perception>(r.error());
  p.tls = note.tls_version.empty() ? std::string("cleartext") : note.tls_version + " " + note.cipher;
  p.trust_anchor = note.trust_anchor;
  p.resumed = note.resumed;
  p.client_certificate_sent = note.client_certificate_sent;
  p.status = r->status;
  if (r->status == 301 || r->status == 302 || r->status == 307 || r->status == 308) {
    std::string to = r->header("Location");
    if (!to.empty() && to != url) {
      if (to[0] == '/') to = (ep->secure ? "https://" : "http://") + ep->host + to;
      return look(to, opt);
    }
  }
  if (r->status != 200)
    return Result<Perception>(Status::NotFound, url + ": it answered " + dec((uint64_t)r->status) + " " + r->reason);

  p.fetched_bytes = r->body.size();
  p.text = page_text(ByteSpan(r->body.data(), r->body.size()), &p.title, opt.max_text);
  if (opt.collect_links)
    p.links = page_links(ByteSpan(r->body.data(), r->body.size()), url, 600);
  p.text_bytes = p.text.size();
  if (p.title.empty()) p.title = url;

  if (opt.keep && actor_) {
    auto may = actor_->may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write);
    if (!may.ok())
      return Result<Perception>(Status::Denied,
                                "entity " + actor_->self().short_str() + " looked, but may not keep what it saw: " +
                                    may.error().str());
    // The same page, unchanged, is not a second thing this machine knows.
    auto digest = crypto::sha256(ByteSpan((const uint8_t*)p.text.data(), p.text.size()));
    static const char* hexd = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : digest) { hex += hexd[b >> 4]; hex += hexd[b & 15]; }
    for (const auto& known : kernel_.store().list()) {
      auto got = kernel_.store().get(known);
      if (!got.ok()) continue;
      if (got->meta("url") == url && got->meta("digest") == hex) {
        p.object = known;
        p.kept = true;
        p.already_known = true;
        p.took_ns = kernel_.host().monotonic_ns() - t0;
        return p;
      }
    }
    StoredObject o;
    o.payload = ByteBuffer(p.text.begin(), p.text.end());
    if (opt.embed) o.embedding = opt.embed(p.title + "\n" + p.text);
    o.metadata.push_back({"url", url});
    o.metadata.push_back({"title", p.title});
    o.metadata.push_back({"digest", hex});
    o.metadata.push_back({"seen", "sense"});
    auto id = actor_->store(std::move(o));
    if (!id.ok()) return Result<Perception>(id.error());
    p.object = *id;
    p.kept = true;
  }
  p.took_ns = kernel_.host().monotonic_ns() - t0;
  return p;
}

}  // namespace aios::userland
