#include "aios/userland/feed_parser.hpp"
#include "aios/channel/http_client.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/hal/host.hpp"
#include <algorithm>
#include <set>

namespace aios::userland {

namespace {

// XML character references, so "AT&amp;T" is stored as "AT&T".
std::string decode_entities(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') { out.push_back(s[i]); continue; }
    size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
    std::string name = s.substr(i + 1, semi - i - 1);
    uint32_t cp = 0;
    bool ok = true;
    if (name == "amp") cp = '&';
    else if (name == "lt") cp = '<';
    else if (name == "gt") cp = '>';
    else if (name == "quot") cp = '"';
    else if (name == "apos") cp = '\'';
    else if (name.size() > 1 && name[0] == '#') {
      bool hex = name[1] == 'x' || name[1] == 'X';
      for (size_t k = hex ? 2 : 1; k < name.size(); ++k) {
        char ch = name[k];
        int d = (ch >= '0' && ch <= '9') ? ch - '0'
                : (hex && ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                : (hex && ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (d < 0) { ok = false; break; }
        cp = cp * (hex ? 16 : 10) + (uint32_t)d;
        if (cp > 0x10FFFF) { ok = false; break; }
      }
    } else ok = false;
    if (!ok || cp == 0) { out.push_back('&'); continue; }
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    i = semi;
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

// Finds "<tag" followed by '>', '/', or whitespace (so "<link" never matches "<linkage").
size_t find_open(const std::string& xml, const std::string& tag, size_t from) {
  const std::string open = "<" + tag;
  for (size_t at = xml.find(open, from); at != std::string::npos; at = xml.find(open, at + 1)) {
    size_t after = at + open.size();
    if (after >= xml.size()) return std::string::npos;
    char ch = xml[after];
    if (ch == '>' || ch == '/' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') return at;
  }
  return std::string::npos;
}

std::string extract_tag_value(const std::string& xml, const std::string& tag, size_t start_pos = 0) {
  size_t open_idx = find_open(xml, tag, start_pos);
  if (open_idx == std::string::npos) return "";
  size_t val_start = xml.find('>', open_idx);
  if (val_start == std::string::npos) return "";
  if (xml[val_start - 1] == '/') return "";   // <tag/>: empty
  val_start++;
  size_t close_idx = xml.find("</" + tag + ">", val_start);
  if (close_idx == std::string::npos) return "";
  std::string content = trim(xml.substr(val_start, close_idx - val_start));
  if (content.rfind("<![CDATA[", 0) == 0) {
    size_t cdata_end = content.find("]]>");
    if (cdata_end != std::string::npos) return content.substr(9, cdata_end - 9);
  }
  return decode_entities(content);
}

std::string attribute_of(const std::string& tag_text, const std::string& name) {
  for (char q : {'"', '\''}) {
    std::string key = name + "=" + q;
    size_t at = tag_text.find(key);
    while (at != std::string::npos && at > 0 && tag_text[at - 1] != ' ' && tag_text[at - 1] != '\t' &&
           tag_text[at - 1] != '\n')
      at = tag_text.find(key, at + 1);
    if (at == std::string::npos) continue;
    size_t start = at + key.size();
    size_t end = tag_text.find(q, start);
    if (end == std::string::npos) continue;
    return decode_entities(tag_text.substr(start, end - start));
  }
  return "";
}

// Atom: <link href="..." rel="alternate"/>. The alternate link (or one with no
// rel, which means alternate) is the entry's page; others are enclosures etc.
std::string atom_link(const std::string& entry) {
  std::string fallback;
  for (size_t at = find_open(entry, "link", 0); at != std::string::npos; at = find_open(entry, "link", at + 1)) {
    size_t end = entry.find('>', at);
    if (end == std::string::npos) break;
    std::string tag = entry.substr(at, end - at + 1);
    std::string href = attribute_of(tag, "href");
    if (href.empty()) continue;
    std::string rel = attribute_of(tag, "rel");
    if (rel.empty() || rel == "alternate") return href;
    if (fallback.empty()) fallback = href;
  }
  return fallback;
}

}  // namespace

std::vector<FeedItem> parse_feed_xml(const std::string& xml_content) {
  std::vector<FeedItem> items;
  size_t pos = 0;
  // RSS 2.0 / 0.9x / RDF: <item>
  while (pos < xml_content.length()) {
    size_t item_start = find_open(xml_content, "item", pos);
    if (item_start == std::string::npos) break;
    size_t item_end = xml_content.find("</item>", item_start);
    if (item_end == std::string::npos) break;
    std::string block = xml_content.substr(item_start, item_end - item_start + 7);
    pos = item_end + 7;
    FeedItem item;
    item.title = extract_tag_value(block, "title");
    item.link = extract_tag_value(block, "link");
    item.description = extract_tag_value(block, "description");
    item.pub_date = extract_tag_value(block, "pubDate");
    if (item.pub_date.empty()) item.pub_date = extract_tag_value(block, "dc:date");
    item.guid = extract_tag_value(block, "guid");
    if (item.title.empty() && item.link.empty()) continue;
    items.push_back(item);
  }
  if (!items.empty()) return items;
  // Atom: <entry>
  pos = 0;
  while (pos < xml_content.length()) {
    size_t entry_start = find_open(xml_content, "entry", pos);
    if (entry_start == std::string::npos) break;
    size_t entry_end = xml_content.find("</entry>", entry_start);
    if (entry_end == std::string::npos) break;
    std::string block = xml_content.substr(entry_start, entry_end - entry_start + 8);
    pos = entry_end + 8;
    FeedItem item;
    item.title = extract_tag_value(block, "title");
    item.link = atom_link(block);
    item.description = extract_tag_value(block, "summary");
    if (item.description.empty()) item.description = extract_tag_value(block, "content");
    item.pub_date = extract_tag_value(block, "updated");
    if (item.pub_date.empty()) item.pub_date = extract_tag_value(block, "published");
    item.guid = extract_tag_value(block, "id");
    if (item.title.empty() && item.link.empty()) continue;
    items.push_back(item);
  }
  return items;
}

Result<FeedResult> ingest_rss_feed(Kernel& kernel, Surface* actor, const std::string& feed_url) {
  // Reaching out spends the caller's network authority, keeping what came back
  // its storage authority — the same two checks `sense` makes.
  if (actor) {
    auto may = actor->may(ResourceRef{ResourceKind::Network, {}}, R_Consume);
    if (!may.ok())
      return Result<FeedResult>(Status::Denied, "entity " + actor->self().short_str() +
                                                    " may not reach outside this machine: " + may.error().str());
  }

  // The feed is parsed from the bytes the server sent: `sense` strips markup,
  // and the markup is the feed.
  std::string url = feed_url;
  channel::HttpResponse response;
  for (int hop = 0;; ++hop) {
    auto ep = channel::parse_endpoint(url);
    if (!ep.ok()) return Result<FeedResult>(ep.error());
    channel::HttpRequest req;
    req.endpoint = *ep;
    req.timeout_ms = 60000;
    req.host = &kernel.host();
    req.kernel_root = kernel.config().root;
    req.headers.push_back({"Accept", "application/rss+xml, application/atom+xml, application/xml, text/xml, */*"});
    req.headers.push_back({"User-Agent", "aios/1 (feed reader)"});
    auto r = channel::http_send(req);
    if (!r.ok()) return Result<FeedResult>(r.error());
    if ((r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308) &&
        hop < 5) {
      std::string to = r->header("Location");
      if (!to.empty() && to != url) {
        if (to[0] == '/') to = (ep->secure ? "https://" : "http://") + ep->host + ":" + std::to_string(ep->port) + to;
        url = to;
        continue;
      }
    }
    if (r->status != 200)
      return Result<FeedResult>(Status::NotFound, url + ": it answered " + std::to_string(r->status) + " " + r->reason);
    response = std::move(*r);
    break;
  }
  std::string xml(response.body.begin(), response.body.end());

  FeedResult result;
  result.url = feed_url;
  // The channel/feed title: the first <title> outside any item or entry.
  size_t first_item = std::min(find_open(xml, "item", 0), find_open(xml, "entry", 0));
  std::string head = first_item == std::string::npos ? xml : xml.substr(0, first_item);
  result.feed_title = extract_tag_value(head, "title");
  if (result.feed_title.empty()) result.feed_title = feed_url;
  result.items = parse_feed_xml(xml);
  if (result.items.empty() && find_open(xml, "rss", 0) == std::string::npos &&
      find_open(xml, "feed", 0) == std::string::npos && find_open(xml, "rdf:RDF", 0) == std::string::npos)
    return Result<FeedResult>(Status::Invalid, feed_url + ": not an RSS or Atom document");

  // An item already kept (same feed, same guid or link) is not stored twice.
  std::set<std::string> known;
  for (const auto& id : kernel.store().list()) {
    auto got = kernel.store().get(id);
    if (!got.ok() || got->meta("kind") != "rss_item" || got->meta("feed") != feed_url) continue;
    known.insert(got->meta("guid").empty() ? got->meta("link") : got->meta("guid"));
  }

  for (const auto& item : result.items) {
    std::string key = item.guid.empty() ? item.link : item.guid;
    if (!key.empty() && known.count(key)) { ++result.items_already_kept; continue; }
    std::string payload = "feed_item: " + item.title + "\nlink: " + item.link + "\ndate: " + item.pub_date +
                          "\n\n" + item.description;
    StoredObject obj;
    obj.metadata.push_back({"kind", "rss_item"});
    obj.metadata.push_back({"feed", feed_url});
    obj.metadata.push_back({"title", item.title});
    obj.metadata.push_back({"link", item.link});
    obj.metadata.push_back({"guid", item.guid});
    obj.metadata.push_back({"date", item.pub_date});
    obj.payload = ByteBuffer(payload.begin(), payload.end());
    if (actor) {
      auto stored = actor->store(std::move(obj));   // checks the caller's storage authority
      if (!stored.ok()) return Result<FeedResult>(stored.error());
    } else {
      auto stored = kernel.store().put(std::move(obj));
      if (!stored.ok()) return Result<FeedResult>(stored.error());
    }
    result.items_stored++;
    if (!key.empty()) known.insert(key);
  }
  return result;
}

std::vector<FeedItem> stored_feed_items(Kernel& kernel, const std::string& feed_url) {
  std::vector<FeedItem> out;
  for (const auto& id : kernel.store().list()) {
    auto got = kernel.store().get(id);
    if (!got.ok() || got->meta("kind") != "rss_item") continue;
    if (!feed_url.empty() && got->meta("feed") != feed_url) continue;
    FeedItem it;
    it.title = got->meta("title");
    it.link = got->meta("link");
    it.guid = got->meta("guid");
    it.pub_date = got->meta("date");
    it.description = got->meta("feed");   // which feed it came from
    out.push_back(it);
  }
  return out;
}

}  // namespace aios::userland
