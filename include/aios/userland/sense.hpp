#pragma once
// The machine's own eyes.
//
// Not a browser it opens for someone to look at: a way for this kernel to
// fetch a page over its OWN TLS, read what the page says, and put that in the
// object store as something it now knows. There is no host process in the path
// and no engine to install — the transport is `src/net/tls.cpp` and the HTTP
// client that already fetches from GitHub, npm, PyPI and the Go proxy, so this
// works on the bare metal exactly as it works hosted.
//
// What it extracts is text, not a rendering. A page's script and style are
// dropped, its markup removed, its entities resolved and its whitespace
// collapsed — which is what something reading for meaning wants, and is honest
// about what it is: this machine cannot lay a page out, and does not pretend to.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/storage/object_store.hpp"

namespace aios::userland {

// One outbound link a page carries: where it points and what it was called.
// A page of headlines IS its links, so this is what a scan reads rather than
// the prose around them.
struct PageLink {
  std::string url;
  std::string text;
};

struct Perception {
  std::string url;
  std::string title;
  std::string text;            // what the page says, as text
  uint64_t fetched_bytes = 0;  // what came over the wire
  uint64_t text_bytes = 0;
  std::string tls;             // the transport this machine negotiated
  std::string trust_anchor;
  bool resumed = false;               // the TLS session was resumed from a ticket
  bool client_certificate_sent = false;
  int status = 0;
  ObjectID object;             // what it became in the store, when it was kept
  bool kept = false;
  bool already_known = false;  // the same page, unchanged since it was last seen
  std::vector<PageLink> links;  // filled when the caller asked for them
  uint64_t took_ns = 0;
};

struct SenseOptions {
  int timeout_ms = 300000;   // a real page over a slow link can take minutes; don't kill perception mid-read
  size_t max_text = 24000;     // a page's worth; the store is for meaning, not archives
  bool keep = true;            // put it in the object store
  bool collect_links = false;  // also read where the page points
  // How to turn what a page says into something the store can compare. The
  // embedding belongs to whoever knows how this machine represents meaning, so
  // it is handed in rather than assumed here.
  std::vector<float> (*embed)(const std::string&) = nullptr;
};

class Sense {
 public:
  Sense(Kernel& kernel, Surface* actor = nullptr);

  // Fetch and read. Reaching out spends network authority; keeping what came
  // back spends storage authority, and the two are checked separately.
  Result<Perception> look(const std::string& url, const SenseOptions& opt = {});

 private:
  Kernel& kernel_;
  Surface* actor_ = nullptr;
};

// The reader, on its own and testable on its own: markup in, what the page says
// out. `title` gets the document's title when it has one.
std::string page_text(ByteSpan markup, std::string* title, size_t max_text);

// Every anchor the page carries, in the order it wrote them, with relative
// targets resolved against the page's own address. Fragments, mail and script
// targets are not places this machine can go, and are left out.
std::vector<PageLink> page_links(ByteSpan markup, const std::string& base_url, size_t max_links);

}  // namespace aios::userland
