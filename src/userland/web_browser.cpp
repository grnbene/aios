// The thing a window drives: url resolution, the author's stylesheets, one
// page's history, and the two ways a laid-out page can be presented — a
// fragment list for a framebuffer, and lines of text for a console that has
// none. Both come off the SAME layout, so the serial console and the desktop
// pane can never disagree about what the page says.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <new>
#include <vector>
#include "aios/core/base64.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/web_js.hpp"

#include "aios/userland/web.hpp"

namespace aios::userland::web {
namespace {

Result<FetchResult> decode_data_url(const std::string& url) {
  if (url.compare(0, 5, "data:") != 0) return Result<FetchResult>(Status::Invalid, "not a data url");
  size_t comma = url.find(',');
  if (comma == std::string::npos) return Result<FetchResult>(Status::Invalid, "malformed data url");
  std::string meta = url.substr(5, comma - 5);
  std::string payload = url.substr(comma + 1);
  bool is_base64 = false;
  std::string mime = "text/plain;charset=US-ASCII";
  if (!meta.empty()) {
    size_t b64_pos = meta.find(";base64");
    if (b64_pos != std::string::npos) {
      is_base64 = true;
      mime = meta.substr(0, b64_pos);
    } else {
      mime = meta;
    }
  }
  std::string body;
  if (is_base64) {
    auto dec = aios::base64_decode(payload);
    if (!dec.ok()) return Result<FetchResult>(dec.error());
    body.assign(reinterpret_cast<const char*>(dec->data()), dec->size());
  } else {
    for (size_t i = 0; i < payload.size(); ++i) {
      if (payload[i] == '%' && i + 2 < payload.size()) {
        auto nib = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        int n1 = nib(payload[i + 1]), n2 = nib(payload[i + 2]);
        if (n1 >= 0 && n2 >= 0) {
          body += (char)((n1 << 4) | n2);
          i += 2;
          continue;
        }
      }
      body += payload[i];
    }
  }
  FetchResult fr;
  fr.body = std::move(body);
  fr.final_url = url;
  fr.content_type = mime;
  fr.status = 200;
  return Result<FetchResult>(std::move(fr));
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

std::string lowered(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) o += lower(c);
  return o;
}

std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) ++a;
  while (b > a && is_space(s[b - 1])) --b;
  return s.substr(a, b - a);
}

bool has_scheme(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == ':') return i > 0;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.';
    if (!ok) return false;
  }
  return false;
}

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) {
    s.insert(s.begin(), (char)('0' + v % 10));
    v /= 10;
  }
  return s;
}

// Collapse `a/b/../c` to `a/c` and `a/./b` to `a/b`. A server will accept the
// unnormalised form, but the cache key and the `<base>` resolution both want
// one canonical spelling of the same page.
std::string normalise_path(const std::string& path) {
  std::vector<std::string> parts;
  size_t i = 0;
  bool absolute = !path.empty() && path[0] == '/';
  while (i < path.size()) {
    size_t slash = path.find('/', i);
    std::string seg = path.substr(i, (slash == std::string::npos ? path.size() : slash) - i);
    i = slash == std::string::npos ? path.size() : slash + 1;
    if (seg == "." || seg.empty()) continue;
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }
  std::string out = absolute ? "/" : "";
  for (size_t k = 0; k < parts.size(); ++k) {
    out += parts[k];
    if (k + 1 < parts.size()) out += "/";
  }
  if (!path.empty() && path.back() == '/' && (out.empty() || out.back() != '/')) out += "/";
  if (out.empty()) out = "/";
  return out;
}

// A picture that is SVG markup rather than a raster: an <img src="x.svg"> or
// a `background-image: url(data:image/svg+xml,...)`. Rasterised at a size that
// stays sharp when the page scales it up, while `nat_w`/`nat_h` keep the size
// the markup declares, which is what layout boxes it with.
bool decode_svg_image(const std::string& body, Bitmap& out, int& nat_w, int& nat_h) {
  const size_t head = body.size() < 4096 ? body.size() : 4096;
  if (body.substr(0, head).find("<svg") == std::string::npos) return false;
  NodePtr dom = parse_html(ByteSpan(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  if (!dom) return false;
  Node* svg = dom->tag == "svg" ? dom.get() : dom->first("svg");
  if (!svg) return false;
  auto num = [](const std::string& a) -> float {
    if (a.empty() || a.find('%') != std::string::npos) return 0.0f;
    return (float)std::atof(a.c_str());
  };
  float w = num(svg->attr("width")), h = num(svg->attr("height"));
  if (w <= 0 || h <= 0) {
    // The viewBox is the shape of the drawing when no size is declared.
    std::string vb = svg->attr("viewbox");
    if (vb.empty()) vb = svg->attr("viewBox");
    float v[4] = {0, 0, 0, 0};
    const char* c = vb.c_str();
    for (int k = 0; k < 4 && *c; ++k) {
      char* end = nullptr;
      v[k] = (float)std::strtod(c, &end);
      if (end == c) break;
      c = end;
      while (*c == ',' || *c == ' ') ++c;
    }
    if (v[2] > 0 && v[3] > 0) {
      if (w > 0) h = w * v[3] / v[2];
      else if (h > 0) w = h * v[2] / v[3];
      else { w = v[2]; h = v[3]; }
    }
  }
  if (w <= 0 || h <= 0) { w = 300; h = 150; }
  if (w > 4096) { h = h * 4096 / w; w = 4096; }
  if (h > 4096) { w = w * 4096 / h; h = 4096; }
  nat_w = (int)(w + 0.5f) > 0 ? (int)(w + 0.5f) : 1;
  nat_h = (int)(h + 0.5f) > 0 ? (int)(h + 0.5f) : 1;
  // Render at least 256 on the long side (never past 1024) so a 24-unit icon
  // shown at 200px is not an eight-times blow-up of 24 pixels.
  float scale = 1.0f;
  const float longest = w > h ? w : h;
  if (longest < 256.0f) scale = 256.0f / longest;
  if (longest * scale > 1024.0f) scale = 1024.0f / longest;
  int rw = (int)(w * scale + 0.5f), rh = (int)(h * scale + 0.5f);
  if (rw < 1) rw = 1;
  if (rh < 1) rh = 1;
  auto r = rasterise_svg(*svg, rw, rh, 0xFF000000);
  if (!r.ok()) return false;
  out = std::move(*r);
  return true;
}

}  // namespace

std::string resolve_url(const std::string& base, const std::string& href_raw) {
  std::string href = trimmed(href_raw);
  if (href.empty()) return {};
  // A url the engine will not follow is still returned whole, so the caller
  // can say why rather than showing a broken link.
  std::string low = lowered(href);
  if (low.compare(0, 7, "mailto:") == 0 || low.compare(0, 11, "javascript:") == 0 ||
      low.compare(0, 4, "tel:") == 0 || low.compare(0, 5, "data:") == 0)
    return href;
  if (has_scheme(href)) return href;

  size_t scheme_end = base.find("://");
  if (scheme_end == std::string::npos) return href;
  std::string scheme = base.substr(0, scheme_end);
  size_t host_start = scheme_end + 3;
  size_t host_end = base.find('/', host_start);
  std::string authority = base.substr(host_start, (host_end == std::string::npos ? base.size() : host_end) - host_start);
  std::string base_path = host_end == std::string::npos ? "/" : base.substr(host_end);
  // The query and the fragment of the base are not part of the directory a
  // relative url resolves against.
  size_t cut = base_path.find_first_of("?#");
  if (cut != std::string::npos) base_path = base_path.substr(0, cut);

  if (href.compare(0, 2, "//") == 0) return scheme + ":" + href;
  if (href[0] == '#') return base + (base.find('#') == std::string::npos ? href : std::string());
  if (href[0] == '?') return scheme + "://" + authority + base_path + href;
  if (href[0] == '/') return scheme + "://" + authority + normalise_path(href);

  // Relative to the base's directory.
  size_t last_slash = base_path.rfind('/');
  std::string dir = last_slash == std::string::npos ? "/" : base_path.substr(0, last_slash + 1);
  return scheme + "://" + authority + normalise_path(dir + href);
}

// Fetch several addresses at once, on a few threads of their own. A page's stylesheets, scripts and pictures are
// independent of each other and each costs a whole connection (and, over https, a handshake) on this machine's stack;
// asked for one after another a Wikipedia page was twenty sequential round trips. The result for urls[i] is out[i], so
// the caller keeps document order whatever order the answers arrived in. The fetch function must be callable from
// several threads (kernel_fetch is: a request is a connection of its own).
std::vector<Result<FetchResult>> fetch_many(const Fetch& fetch, const std::vector<std::string>& urls) {
  std::vector<Result<FetchResult>> out;
  out.reserve(urls.size());
  for (size_t i = 0; i < urls.size(); ++i) out.emplace_back(Status::NotFound, "not fetched");
  if (urls.empty() || !fetch) return out;
  if (urls.size() == 1) { out[0] = fetch(urls[0], {}); return out; }
  struct Shared { std::atomic<size_t> next{0}; };
  auto sh = std::make_shared<Shared>();
  auto work = [&, sh]() {
    for (;;) {
      const size_t i = sh->next.fetch_add(1);
      if (i >= urls.size()) return;
      out[i] = fetch(urls[i], {});
    }
  };
  // Bounded: six connections at once is what a browser opens per host, and the cores here are few.
  const size_t helpers = std::min<size_t>(urls.size(), 6) - 1;
  std::vector<hal::Thread> threads;
  for (size_t t = 0; t < helpers; ++t) threads.emplace_back(hal::Thread(work));
  work();                               // the caller works too
  for (auto& t : threads) t.join();
  return out;
}

// A fetch that answers from what was fetched ahead, once each, and from the network for anything else. The scripts
// and stylesheets of a page are known as soon as its HTML is parsed, so they all go out together and the code that
// walks them in document order finds each one already here.
Fetch caching_fetch(const Fetch& inner, std::vector<std::string> urls, std::vector<Result<FetchResult>> got) {
  struct Cache {
    hal::Mutex m;
    std::map<std::string, Result<FetchResult>> by_url;
  };
  auto cache = std::make_shared<Cache>();
  for (size_t i = 0; i < urls.size(); ++i) cache->by_url.emplace(urls[i], std::move(got[i]));
  return [inner, cache](const std::string& url, const FetchOptions& opt) -> Result<FetchResult> {
    if (opt.method.empty() || opt.method == "GET") {
      hal::LockGuard g(cache->m);
      auto it = cache->by_url.find(url);
      if (it != cache->by_url.end()) {
        // A stylesheet stays: the sheets are collected a second time when a script has added markup. A script is
        // taken once, so its body does not live as long as the page.
        const bool css = it->second.ok() && it->second->content_type.find("css") != std::string::npos;
        if (css) return it->second;
        Result<FetchResult> r = std::move(it->second);
        cache->by_url.erase(it);
        return r;
      }
    }
    return inner(url, opt);
  };
}

// The stylesheets collect_author_css will want (at most six, as it does) and the external classic scripts, in
// document order, resolved the way each is later asked for.
std::vector<std::string> prefetch_urls(Node& doc, const std::string& base, bool scripts) {
  std::vector<std::string> urls;
  size_t sheets = 0;
  std::vector<Node*> stack{&doc};
  while (!stack.empty() && urls.size() < 24) {
    Node* n = stack.back();
    stack.pop_back();
    for (size_t i = n->children.size(); i > 0; --i) stack.push_back(n->children[i - 1].get());
    if (n->kind != Node::Kind::Element) continue;
    std::string href;
    if (n->tag == "link" && lowered(n->attr("rel")).find("stylesheet") != std::string::npos) {
      if (sheets >= 6) continue;
      href = resolve_url(base, n->attr("href"));
      if (!href.empty()) ++sheets;
    } else if (scripts && n->tag == "script" && n->has_attr("src") && lowered(n->attr("type")) != "module") {
      href = resolve_url(base, n->attr("src"));
    }
    if (href.empty() || lowered(href).compare(0, 4, "http") != 0) continue;
    if (std::find(urls.begin(), urls.end(), href) == urls.end()) urls.push_back(href);
  }
  return urls;
}

Stylesheet collect_author_css(Node& doc, const std::string& base, const Fetch& fetch) {
  Stylesheet sheet;
  // Walk in document order: a later sheet must win over an earlier one, and
  // the cascade breaks ties by order, so the order they are appended in IS the
  // author's intent.
  std::vector<Node*> stack;
  stack.push_back(&doc);
  std::vector<Node*> flat;
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    flat.push_back(n);
    for (size_t i = n->children.size(); i > 0; --i) stack.push_back(n->children[i - 1].get());
  }

  size_t fetched = 0;
  for (Node* n : flat) {
    if (n->kind != Node::Kind::Element) continue;
    if (n->tag == "style") {
      std::string media = n->attr("media");
      Stylesheet inner = parse_css(n->inner_text());
      if (!media.empty())
        for (auto& r : inner.rules)
          if (r.media.empty()) r.media = media;
      sheet.append(inner);
      continue;
    }
    if (n->tag != "link") continue;
    std::string rel = lowered(n->attr("rel"));
    if (rel.find("stylesheet") == std::string::npos) continue;
    std::string href = resolve_url(base, n->attr("href"));
    if (href.empty() || !fetch) continue;
    // A page with forty stylesheets is a page that will not finish loading in
    // ring 0. Six is what a real document uses.
    if (fetched >= 6) continue;
    ++fetched;
    auto r = fetch(href, {});
    if (!r.ok()) continue;
    Stylesheet inner = parse_css(r->body);
    std::string media = n->attr("media");
    if (!media.empty())
      for (auto& rule : inner.rules)
        if (rule.media.empty()) rule.media = media;
    sheet.append(inner);
  }
  return sheet;
}

// -------------------------------------------------------------- the browser --

Result<void> Browser::visit(const std::string& url, const Metrics& m, int viewport_w, bool record,
                            const FetchOptions& opt) {
  if (!fetch_) return Result<void>(Status::Unsupported, "this browser has nothing to fetch with");
  std::string low = lowered(url);
  if (low.compare(0, 7, "mailto:") == 0)
    return Result<void>(Status::Unsupported, "that is an email address, not a page: " + url.substr(7));
  if (low.compare(0, 11, "javascript:") == 0)
    return Result<void>(Status::Unsupported, "this engine runs no script, so there is nothing behind that link");
  // http(s) over the machine's own stack, or file:, or data:
  if (low.compare(0, 4, "http") != 0 && low.compare(0, 5, "file:") != 0 && low.compare(0, 5, "data:") != 0)
    return Result<void>(Status::Invalid, "not a url this engine can fetch: " + url);

  Result<FetchResult> got = (low.compare(0, 5, "data:") == 0) ? decode_data_url(url) : fetch_(url, opt);
  if (!got.ok()) {
    status_ = got.error().message.empty() ? std::string("could not fetch ") + url : got.error().message;
    return Result<void>(got.error());
  }

  transport_ = got->transport;
  secure_ = low.compare(0, 6, "https:") == 0;

  Document fresh;
  fresh.url = got->final_url.empty() ? url : got->final_url;
  fresh.base = fresh.url;
  fresh.dom = parse_html(ByteSpan(reinterpret_cast<const uint8_t*>(got->body.data()), got->body.size()));

  // `<base href>` moves what every relative url on the page resolves against.
  if (fresh.dom) {
    if (Node* b = fresh.dom->first("base")) {
      std::string href = b->attr("href");
      if (!href.empty()) fresh.base = resolve_url(fresh.url, href);
    }
  }

  // Everything the page links is asked for at once, so the sheets and the scripts cost the slowest of them rather
  // than the sum. First paint waits on that, not on the scripts running: they are already here when it is time.
  Fetch page_fetch = fetch_;
  if (fresh.dom) {
    std::vector<std::string> urls = prefetch_urls(*fresh.dom, fresh.base, scripts_enabled_);
    if (urls.size() > 1) page_fetch = caching_fetch(fetch_, urls, fetch_many(fetch_, urls));
  }
  Stylesheet author;
  if (fresh.dom) author = collect_author_css(*fresh.dom, fresh.base, page_fetch);

  doc_ = std::move(fresh);
  author_ = std::move(author);
  images_.clear();
  last_width_ = viewport_w;

  // Script runs BEFORE the first layout, against the tree layout is about to
  // walk — so a page that builds its content in JS is laid out with that
  // content rather than laid out empty and patched afterwards. layout() is
  // idempotent, so a script that mutates later only costs another pass.
  // A runtime per page: the heap, the listeners and the DOM wrappers all have
  // to outlive the load or a handler registered here is gone by the time
  // anybody clicks. The old one goes with the old document.
  if (scripts_enabled_) {
    // What the page we are leaving saved to localStorage but the pane's throttled flush has not yet handed to the
    // machine: leaving the page is the last chance to keep it.
    if (js_) js_->flush_storage();
    js_ = std::make_shared<JsRuntime>();
    if (has_host_) js_->set_host(host_, viewport_w, view_h_);
    else js_->set_view(viewport_w, view_h_);
    script_ = js_->run(doc_, page_fetch, doc_.base);
    // The author CSS may only exist once script has put the <style> there.
    if (script_.dom_writes > 0 && doc_.dom) author_ = collect_author_css(*doc_.dom, doc_.base, page_fetch);
  } else {
    // No runtime at all for a document we do not trust. Nothing to sandbox.
    if (js_) js_->flush_storage();
    js_.reset();
    script_ = {};
  }

  metrics_ = m;
  have_metrics_ = true;
  layout(doc_, author_, m, viewport_w, view_h_);

  if (record) {
    // A visit from the middle of the history throws the forward entries away,
    // which is what every back button in the world does.
    if (history_index_ >= 0 && (size_t)history_index_ + 1 < history_.size())
      history_.resize((size_t)history_index_ + 1);
    if (history_.empty() || history_.back() != doc_.url) {
      history_.push_back(doc_.url);
      history_index_ = (int)history_.size() - 1;
    }
  }

  status_ = doc_.title + " · " + dec(doc_.fragments.size()) + " box(es), " + dec(doc_.links.size()) + " link(s)";
  return Ok();
}


// ------------------------------------------------------------------ forms --

Node* enclosing_form(Node* n) {
  for (Node* p = n; p; p = p->parent)
    if (p->kind == Node::Kind::Element && p->tag == "form") return p;
  return nullptr;
}

bool is_submit_control(const Node* n) {
  if (!n || n->kind != Node::Kind::Element) return false;
  const std::string type = lowered(n->attr("type"));
  if (n->tag == "button") return type.empty() || type == "submit";
  if (n->tag == "input") return type == "submit" || type == "image";
  return false;
}

namespace {

std::string form_urlencode(const std::string& in) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else if (c == ' ') out += '+';
    else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 0x0F]; }
  }
  return out;
}

// A control's current value. <textarea> carries it as text rather than as an
// attribute, which is the one case where the DOM and the attribute disagree.
std::string control_value(const Node& n) {
  if (n.tag == "textarea" && !n.has_attr("value")) return n.inner_text();
  return n.attr("value");
}

// The form's SUCCESSFUL controls, in document order -- the standard's word for
// the ones actually sent. A control needs a name, a disabled one is skipped, a
// checkbox or radio goes only when checked, and a submit control goes only if
// it is the one that was pressed.
void collect_fields(const Node& n, const Node* submitter, std::string& out) {
  if (n.kind == Node::Kind::Element) {
    const std::string& tag = n.tag;
    const std::string name = n.attr("name");
    const std::string type = lowered(n.attr("type"));
    if (tag == "input" || tag == "textarea" || tag == "select") {
      const bool skip = name.empty() || n.has_attr("disabled") ||
                        type == "file" || type == "reset" ||
                        ((type == "checkbox" || type == "radio") && !n.has_attr("checked")) ||
                        ((type == "submit" || type == "button" || type == "image") && &n != submitter);
      if (!skip) {
        std::string value = control_value(n);
        if ((type == "checkbox" || type == "radio") && value.empty()) value = "on";
        if (!out.empty()) out += '&';
        out += form_urlencode(name) + "=" + form_urlencode(value);
      }
    } else if (tag == "button" && &n == submitter && !name.empty()) {
      if (!out.empty()) out += '&';
      out += form_urlencode(name) + "=" + form_urlencode(n.attr("value"));
    }
  }
  for (const auto& kid : n.children) if (kid) collect_fields(*kid, submitter, out);
}

}  // namespace

FormRequest form_request(const Node& form, const Node* submitter,
                         const std::string& base, const std::string& doc_url) {
  std::string body;
  collect_fields(form, submitter, body);

  std::string action = form.attr("action");
  if (action.empty()) action = doc_url;
  FormRequest req;
  req.url = resolve_url(base, action);

  if (lowered(form.attr("method")) == "post") {
    req.options.method = "POST";
    req.options.body = body;
    req.options.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
  } else {
    // GET replaces the query string rather than appending to it, which is what
    // a form does when the page it is on already had one.
    const size_t q = req.url.find('?');
    if (q != std::string::npos) req.url = req.url.substr(0, q);
    if (!body.empty()) req.url += "?" + body;
  }
  return req;
}

Result<void> Browser::go(const std::string& url, const Metrics& m, int viewport_w) {
  return visit(url, m, viewport_w, true);
}

Result<void> Browser::submit_form(const std::string& url, const FetchOptions& opt,
                                  const Metrics& m, int viewport_w) {
  return visit(url, m, viewport_w, true, opt);
}

Result<void> Browser::follow(size_t link_index, const Metrics& m, int viewport_w) {
  if (link_index >= doc_.links.size()) return Result<void>(Status::NotFound, "no such link on this page");
  std::string href = doc_.links[link_index].href;
  // A same-page anchor is not a visit: the page is already here.
  size_t hash = href.find('#');
  if (hash != std::string::npos) {
    std::string without = href.substr(0, hash);
    if (without.empty() || without == doc_.url) {
      status_ = "that link points into this page";
      return Ok();
    }
    href = without;
  }
  return visit(href, m, viewport_w, true);
}

Result<void> Browser::reload(const Metrics& m, int viewport_w) {
  if (doc_.url.empty()) return Result<void>(Status::NotFound, "nothing loaded to reload");
  return visit(doc_.url, m, viewport_w, false);
}

Result<void> Browser::back(const Metrics& m, int viewport_w) {
  if (!can_go_back()) return Result<void>(Status::NotFound, "nothing behind this page");
  --history_index_;
  return visit(history_[(size_t)history_index_], m, viewport_w, false);
}

Result<void> Browser::forward(const Metrics& m, int viewport_w) {
  if (!can_go_forward()) return Result<void>(Status::NotFound, "nothing ahead of this page");
  ++history_index_;
  return visit(history_[(size_t)history_index_], m, viewport_w, false);
}

// The topmost element whose box contains this point. Later fragments are
// painted over earlier ones, so the last match is the one a person sees and
// therefore the one they clicked.
Node* Browser::node_at(int x, int y) const {
  Node* hit = nullptr;
  for (const auto& f : doc_.fragments) {
    if (!f.node) continue;
    if (x < f.x || x >= f.x + f.w || y < f.y || y >= f.y + f.h) continue;
    hit = f.node;
  }
  return hit;
}

void Browser::relayout(const Metrics& m, int viewport_w) {
  if (!doc_.dom) return;
  last_width_ = viewport_w;
  metrics_ = m;
  have_metrics_ = true;
  layout(doc_, author_, m, viewport_w, view_h_);
  // The decoded images survive a reflow; only their boxes moved.
  size_t n = images_.size();
  images_.resize(n);
}

int Browser::link_at(int x, int y) const {
  // Last match wins: links laid out later are the ones drawn on top.
  int found = -1;
  for (size_t i = 0; i < doc_.links.size(); ++i) {
    const Link& l = doc_.links[i];
    if (x >= l.x && x < l.x + l.w && y >= l.y && y < l.y + l.h) found = (int)i;
  }
  return found;
}

const Bitmap* Browser::image(const std::string& src) {
  // A picture the page drew itself -- an inline <svg> -- before the ones it
  // asked the network for.
  for (const auto& kv : doc_.inline_images)
    if (kv.first == src) return &kv.second;

  for (auto& e : images_)
    if (e.first == src) return e.second.empty() ? nullptr : &e.second;
  return nullptr;
}

bool Browser::has_unfetched_images(int y0, int y1) const {
  for (const auto& f : doc_.fragments) {
    if (f.kind != Fragment::Kind::Image || f.src.empty()) continue;
    if (f.y + (f.h > 0 ? f.h : 24) < y0 || f.y >= y1) continue;
    bool known = false;
    for (const auto& e : images_)
      if (e.first == f.src) { known = true; break; }
    if (!known) return true;
  }
  return false;
}

void Browser::fetch_images(size_t limit) { fetch_images_range(limit, -(1 << 28), 1 << 28); }

bool Browser::fetch_images_range(size_t limit, int y0, int y1) {
  // Three passes, and the order is the point. Fetching a picture waits on the
  // network, and while it waits the pane may lay the page out again (it does,
  // four times a second, for a page that is still arriving) -- which replaces
  // the fragment list this function is walking. Holding a reference into that
  // list across a fetch was a use-after-free: submitting a search whose answer
  // had pictures crashed the desktop with a bad_alloc, only at full speed.
  // So: decide what is wanted and copy the addresses; fetch with nothing held;
  // then size the boxes against whatever the document is NOW.
  std::vector<std::string> wanted;
  for (const auto& f : doc_.fragments) {
    if (f.kind != Fragment::Kind::Image || f.src.empty()) continue;
    if (f.y + (f.h > 0 ? f.h : 24) < y0 || f.y >= y1) continue;
    bool known = false;
    for (const auto& e : images_)
      if (e.first == f.src) { known = true; break; }
    for (const auto& w : wanted)
      if (w == f.src) { known = true; break; }
    if (!known && wanted.size() < limit) wanted.push_back(f.src);
  }

  // The pictures come down together; decoding stays in order, one at a time.
  std::vector<std::string> remote;
  for (const std::string& src : wanted)
    if (src.compare(0, 5, "data:") != 0 && fetch_) remote.push_back(src);
  std::vector<Result<FetchResult>> fetched = fetch_many(fetch_, remote);
  size_t next_remote = 0;
  bool learned_size = false;
  for (const std::string& src : wanted) {
    Bitmap bm;
    int nat_w = 0, nat_h = 0;
    // A picture is somebody else's bytes. One that claims a size nobody could
    // allocate must cost the page that picture, not the whole browser.
    try {
      std::string body;
      bool got = false;
      if (src.compare(0, 5, "data:") == 0) {
        auto r = decode_data_url(src);
        if (r.ok()) { body = std::move(r->body); got = true; }
      } else if (fetch_) {
        auto& r = fetched[next_remote++];
        if (r.ok()) { body = std::move(r->body); got = true; }
      }
      if (got) {
        auto dec_r = decode_image(ByteSpan(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
        if (dec_r.ok()) bm = std::move(*dec_r);
        else if (!decode_svg_image(body, bm, nat_w, nat_h)) bm = Bitmap();
      }
    } catch (const std::bad_alloc&) {
      bm = Bitmap();
    }
    if (std::getenv("AIOS_IMG_DEBUG")) std::fprintf(stderr, "img %s -> %dx%d\n", src.substr(0, 100).c_str(), bm.w, bm.h);
    if (!bm.empty()) {
      if (nat_w <= 0) { nat_w = bm.w; nat_h = bm.h; }
      doc_.image_sizes.push_back({src, {nat_w, nat_h}});
      learned_size = true;
    }
    images_.push_back({src, std::move(bm)});
  }

  // A box the last layout could not size (an <img> with no width or height)
  // now has a size to be laid out with. Flow the page again so the text after
  // it moves out of the way, instead of the picture landing on top of it.
  if (learned_size && have_metrics_) {
    bool unsized = false;
    for (const auto& f : doc_.fragments)
      if (f.kind == Fragment::Kind::Image && !f.bg && !f.mask && !f.src.empty() && (f.w <= 0 || f.h <= 0)) {
        unsized = true;
        break;
      }
    if (unsized) {
      layout(doc_, author_, metrics_, last_width_ > 0 ? last_width_ : 800, view_h_);
      return true;
    }
  }

  for (auto& f : doc_.fragments) {
    if (f.kind != Fragment::Kind::Image || f.src.empty()) continue;
    // A page that gives an `<img>` no dimensions gets the picture's own, which
    // is not knowable until it is decoded. Setting the box here rather than
    // reflowing means the following content does not move — an honest
    // trade: the image lands in the space its line already had.
    const Bitmap* bm = image(f.src);
    if (!bm || f.bg || f.mask) continue;
    int max_w = doc_.content_width > 40 ? doc_.content_width - 36 : 200;
    if (f.w <= 0 && f.h <= 0) {
      f.w = bm->w;
      f.h = bm->h;
    } else if (f.w <= 0) {
      f.w = bm->h > 0 ? bm->w * f.h / bm->h : bm->w;
    } else if (f.h <= 0) {
      f.h = bm->w > 0 ? bm->h * f.w / bm->w : bm->h;
    }
    if (f.w > max_w && f.w > 0) {
      f.h = f.h * max_w / f.w;
      f.w = max_w;
    }
  }
  return false;
}

// The laid-out page as text. Fragments are in paint order, not reading order,
// so they are bucketed by the line they landed on and sorted across it — which
// is why a table comes out as rows and a two-column layout comes out as two
// columns rather than as interleaved noise.
std::vector<std::string> Browser::as_text(int columns) const {
  if (columns < 20) columns = 20;
  struct Piece {
    int x, y;
    std::string text;
    bool link;
  };
  std::vector<Piece> pieces;
  for (const auto& f : doc_.fragments) {
    if (f.kind == Fragment::Kind::Text) {
      if (f.text.empty()) continue;
      pieces.push_back({f.x, f.y, f.text, f.link >= 0});
      continue;
    }
    if (f.kind == Fragment::Kind::Image) {
      std::string label = f.alt.empty() ? std::string("image") : f.alt;
      pieces.push_back({f.x, f.y, "[" + label + "]", f.link >= 0});
      continue;
    }
    if (f.kind == Fragment::Kind::Rule) {
      pieces.push_back({f.x, f.y, std::string("--------------------------------"), false});
      continue;
    }
  }
  // Insertion sort by (y, x): the list is already nearly in this order, so it
  // is close to linear, and it avoids pulling a comparator sort into a
  // freestanding build.
  for (size_t i = 1; i < pieces.size(); ++i) {
    Piece p = pieces[i];
    size_t j = i;
    while (j > 0 && (pieces[j - 1].y > p.y || (pieces[j - 1].y == p.y && pieces[j - 1].x > p.x))) {
      pieces[j] = pieces[j - 1];
      --j;
    }
    pieces[j] = p;
  }

  std::vector<std::string> lines;
  size_t i = 0;
  while (i < pieces.size()) {
    int line_y = pieces[i].y;
    std::string line;
    // Everything within a few pixels of the same top is on the same line: two
    // fragments on one line box can differ by a baseline shift.
    while (i < pieces.size() && pieces[i].y - line_y < 8) {
      const Piece& p = pieces[i];
      // Indentation, so a list looks like a list and a table cell keeps its
      // column. One space per 8px, which matches the console's cell.
      int want = p.x / 8;
      if ((int)line.size() < want) line.append((size_t)(want - (int)line.size()), ' ');
      else if (!line.empty() && line.back() != ' ') line += ' ';
      line += p.text;
      ++i;
    }
    // Wrap what will not fit, at a space where there is one.
    while ((int)line.size() > columns) {
      int cut = columns;
      while (cut > columns / 2 && line[(size_t)cut] != ' ') --cut;
      if (cut <= columns / 2) cut = columns;
      lines.push_back(line.substr(0, (size_t)cut));
      size_t rest = (size_t)cut;
      while (rest < line.size() && line[rest] == ' ') ++rest;
      line = "  " + line.substr(rest);
    }
    lines.push_back(line);
  }
  return lines;
}

}  // namespace aios::userland::web
