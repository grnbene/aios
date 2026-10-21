#pragma once
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland {

struct FeedItem {
  std::string title;
  std::string link;
  std::string description;
  std::string pub_date;
  std::string guid;
};

struct FeedResult {
  std::string feed_title;
  std::string url;
  std::vector<FeedItem> items;
  size_t items_stored = 0;
  size_t items_already_kept = 0;   // same feed and guid/link, kept by an earlier ingest
};

// Parse RSS or Atom XML contents into FeedItems
std::vector<FeedItem> parse_feed_xml(const std::string& xml_content);

// Fetch an RSS/Atom feed (http, or https over the kernel's TLS), parse the
// document's items, and keep each new one in the object store. With an actor,
// both the fetch and the keep are checked against that actor's authority.
Result<FeedResult> ingest_rss_feed(Kernel& kernel, Surface* actor, const std::string& feed_url);

// The items kept by earlier ingests (all feeds, or one). `description` carries
// the feed url each came from.
std::vector<FeedItem> stored_feed_items(Kernel& kernel, const std::string& feed_url = "");

}  // namespace aios::userland
