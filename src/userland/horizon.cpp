#include "aios/userland/horizon.hpp"

#include <algorithm>

#include "aios/userland/git.hpp"

namespace aios::userland {
namespace {

const char* const kMagic = "AIOSHOR1";

void put_u64(ByteBuffer& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
uint64_t get_u64(ByteSpan b, size_t& at) {
  uint64_t v = 0;
  for (int i = 0; i < 8 && at < b.size(); ++i, ++at) v |= (uint64_t)b[at] << (8 * i);
  return v;
}
void put_text(ByteBuffer& b, const std::string& s) {
  put_u64(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}
std::string get_text(ByteSpan b, size_t& at) {
  uint64_t n = get_u64(b, at);
  std::string s;
  for (uint64_t i = 0; i < n && at < b.size(); ++i, ++at) s += (char)b[at];
  return s;
}

bool is_register(ByteSpan b) {
  if (b.size() < 8) return false;
  for (int i = 0; i < 8; ++i) if (b[i] != (uint8_t)kMagic[i]) return false;
  return true;
}

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}

// A score written so it can be read back exactly as it was written. There is no
// strtod on the bare metal and there does not need to be: two decimal places is
// all a rank has ever needed.
std::string score_text(double v) {
  if (v < 0) v = 0;
  uint64_t hundredths = (uint64_t)(v * 100.0 + 0.5);
  return dec(hundredths / 100) + "." + (hundredths % 100 < 10 ? "0" : "") + dec(hundredths % 100);
}
double score_value(const std::string& s) {
  double whole = 0, frac = 0, scale = 1;
  size_t i = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) whole = whole * 10 + (s[i] - '0');
  if (i < s.size() && s[i] == '.') {
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) { scale *= 10; frac += (s[i] - '0') / scale; }
  }
  return whole + frac;
}

// Words that appear in everything and therefore distinguish nothing. A term
// list built without this is a list of "the", "with" and "from", and a horizon
// built on it matches every headline ever written.
bool noise(const std::string& w) {
  static const char* const kNoise[] = {
      "the", "and", "for", "with", "that", "this", "from", "into", "not", "but", "are", "was", "were", "has",
      "have", "had", "its", "it's", "you", "your", "our", "their", "them", "they", "when", "what", "why", "how",
      "who", "all", "any", "can", "will", "would", "should", "could", "one", "two", "new", "now", "out", "off",
      "over", "under", "than", "then", "there", "here", "more", "most", "some", "such", "only", "just", "also",
      "been", "being", "does", "did", "doing", "made", "make", "makes", "get", "gets", "got", "use", "uses",
      "used", "using", "via", "per", "add", "adds", "added", "fix", "fixes", "fixed", "feat", "chore", "docs",
      "test", "tests", "update", "updates", "updated", "remove", "removed", "merge", "commit", "commits",
      "release", "version", "wip", "refactor", "initial", "first", "last", "next", "back", "into", "onto",
      "about", "after", "before", "again", "still", "yet", "own", "way", "ways", "thing", "things", "part",
      "parts", "case", "cases", "line", "lines", "code", "show", "hn", "ask", "says", "said", "instead",
      "rather", "every", "each", "both", "same", "other", "another", "where", "which", "while", "because",
      nullptr};
  for (int i = 0; kNoise[i]; ++i) if (w == kNoise[i]) return true;
  return false;
}

}  // namespace

std::vector<std::string> horizon_terms(const std::string& text) {
  std::vector<std::string> out;
  std::string word;
  auto flush = [&]() {
    if (word.size() >= 3 && word.size() <= 28 && !noise(word)) {
      bool all_digits = true;
      for (char c : word) if (c < '0' || c > '9') { all_digits = false; break; }
      if (!all_digits) out.push_back(word);
    }
    word.clear();
  };
  for (char c : text) {
    if (c >= 'A' && c <= 'Z') { word += (char)(c - 'A' + 'a'); continue; }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '#') { word += c; continue; }
    // A hyphen or a dot inside a word is part of the name: tls-1.3, x86_64.
    if ((c == '-' || c == '.' || c == '_') && !word.empty()) { flush(); continue; }
    flush();
  }
  flush();
  return out;
}

double horizon_score(const std::string& title, const std::vector<Interest>& interests,
                     std::vector<std::string>* matched) {
  auto words = horizon_terms(title);
  double total = 0;
  for (const auto& in : interests) {
    bool hit = false;
    for (const auto& w : words) if (w == in.term) { hit = true; break; }
    if (!hit) continue;
    total += in.weight;
    if (matched) {
      bool dup = false;
      for (const auto& m : *matched) if (m == in.term) { dup = true; break; }
      if (!dup) matched->push_back(in.term);
    }
  }
  return total;
}

Horizon::Horizon(Surface& actor) : me_(actor) {}

std::string Horizon::register_path() const { return me_.kernel().config().root + "/horizon/register"; }

ByteBuffer Horizon::encode() const {
  ByteBuffer b(kMagic, kMagic + 8);
  put_u64(b, sources_.size());
  for (const auto& s : sources_) {
    put_text(b, s.name);
    put_text(b, s.url);
    put_u64(b, s.last_ms);
    put_u64(b, s.reads);
    put_u64(b, s.considered);
    put_u64(b, s.kept);
    put_u64(b, s.last_ok ? 1 : 0);
    put_text(b, s.last_result);
  }
  return b;
}

void Horizon::decode(ByteSpan b) {
  sources_.clear();
  if (!is_register(b)) return;
  size_t at = 8;
  uint64_t n = get_u64(b, at);
  for (uint64_t i = 0; i < n && at < b.size(); ++i) {
    HorizonSource s;
    s.name = get_text(b, at);
    s.url = get_text(b, at);
    s.last_ms = get_u64(b, at);
    s.reads = get_u64(b, at);
    s.considered = get_u64(b, at);
    s.kept = get_u64(b, at);
    s.last_ok = get_u64(b, at) != 0;
    s.last_result = get_text(b, at);
    sources_.push_back(s);
  }
}

Result<void> Horizon::attach(bool create_if_missing) {
  if (auto r = me_.may(ResourceRef{ResourceKind::State, {}}, R_Read); !r.ok())
    return Result<void>(Status::Denied,
                        "entity " + me_.self().short_str() + " may not reach the horizon: " + r.error().str());
  for (auto& s : me_.kernel().states().list()) {
    if (!is_register(ByteSpan(s.payload.data(), s.payload.size()))) continue;
    auto got = me_.state(s.id);
    if (!got.ok()) return Result<void>(got.error());
    state_ = s.id;
    adopted_ = true;
    decode(ByteSpan(got->payload.data(), got->payload.size()));
    return Ok();
  }
  auto disk = me_.kernel().host().read_file(register_path());
  if (disk.ok()) decode(ByteSpan(disk->data(), disk->size()));
  if (!create_if_missing)
    return sources_.empty() ? Result<void>(Status::NotFound, "this machine watches nothing yet") : Ok();
  ByteBuffer payload = encode();
  auto id = me_.create_state(StateKind::Persistent, ByteSpan(payload.data(), payload.size()));
  if (!id.ok()) return Result<void>(id.error());
  state_ = *id;
  adopted_ = false;
  return Ok();
}

Result<void> Horizon::commit() {
  ByteBuffer p = encode();
  if (!state_.is_null())
    if (auto r = me_.set_state(state_, ByteSpan(p.data(), p.size())); !r.ok()) return r;
  me_.kernel().host().make_dir(me_.kernel().config().root + "/horizon");
  return me_.kernel().host().write_file(register_path(), ByteSpan(p.data(), p.size()));
}

Result<void> Horizon::add_source(const std::string& name, const std::string& url) {
  if (name.empty()) return Result<void>(Status::Invalid, "a source needs a name to be watched or dropped by");
  if (url.find("://") == std::string::npos)
    return Result<void>(Status::Invalid, "a source is a page this machine can fetch: " + url + " is not an address");
  for (auto& s : sources_)
    if (s.name == name) { s.url = url; return commit(); }
  HorizonSource s;
  s.name = name;
  s.url = url;
  sources_.push_back(s);
  return commit();
}

Result<void> Horizon::remove_source(const std::string& name) {
  for (auto it = sources_.begin(); it != sources_.end(); ++it)
    if (it->name == name) { sources_.erase(it); return commit(); }
  return Result<void>(Status::NotFound, "this machine watches nothing called " + name);
}

// ---------------------------------------------------------------------------
// What this machine cares about
// ---------------------------------------------------------------------------
std::vector<Interest> Horizon::interests() const {
  std::vector<Interest> out;
  auto bump = [&](const std::string& term, double weight, const char* from) {
    for (auto& i : out)
      if (i.term == term) { i.seen++; if (weight > i.weight) { i.weight = weight; i.from = from; } return; }
    Interest i;
    i.term = term;
    i.weight = weight;
    i.from = from;
    i.seen = 1;
    out.push_back(i);
  };

  GitRegistry reg(const_cast<Surface&>(me_));
  if (!reg.attach().ok()) return out;

  // What the machine's repositories are called. A repository name is the
  // strongest signal there is about what its operator is doing.
  size_t looked_at = 0;
  for (const auto& r : reg.repos()) {
    for (const auto& t : horizon_terms(r.name)) bump(t, 4.0, "repository");
    if (looked_at >= 3) continue;         // a horizon is not a census
    ++looked_at;
    Git git(me_.kernel(), const_cast<Surface*>(&me_));
    if (!git.open(r.path).ok()) continue;
    auto log = git.log(120);
    if (!log.ok()) continue;
    for (const auto& c : *log) {
      // The subject line only: a commit body is prose, and prose is noise here.
      std::string subject = c.message.substr(0, c.message.find('\n'));
      for (const auto& t : horizon_terms(subject)) bump(t, 1.0, "commit subjects");
    }
  }

  // Something the machine's own history says over and over is what it is
  // actually working on, and counts for more than something said once.
  for (auto& i : out)
    if (i.from == std::string("commit subjects"))
      i.weight = 1.0 + (double)(i.seen > 8 ? 8 : i.seen) * 0.25;

  std::sort(out.begin(), out.end(), [](const Interest& a, const Interest& b) {
    if (a.weight != b.weight) return a.weight > b.weight;
    if (a.seen != b.seen) return a.seen > b.seen;
    return a.term < b.term;
  });
  // A term said once in a hundred commits is a coincidence, not an interest.
  std::vector<Interest> kept;
  for (const auto& i : out) {
    if (i.from == std::string("commit subjects") && i.seen < 2) continue;
    kept.push_back(i);
    if (kept.size() >= 80) break;
  }
  return kept;
}

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------
Result<ScanReport> Horizon::scan(size_t keep_per_source, double floor) {
  ScanReport rep;
  uint64_t t0 = me_.kernel().host().monotonic_ns();
  if (sources_.empty())
    return Result<ScanReport>(Status::NotFound,
                              "this machine watches nothing — 'horizon watch <name> <url>' gives it somewhere to look");
  // A scan exists to KEEP what it finds. Refusing at the end, after the network
  // has already been spent, would be a worse answer than refusing now.
  if (auto r = me_.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write); !r.ok())
    return Result<ScanReport>(Status::Denied,
                              "entity " + me_.self().short_str() +
                                  " may look at the world, but a scan keeps what it finds and it may not write this "
                                  "machine's object store: " + r.error().str());
  auto interest = interests();
  if (interest.empty())
    return Result<ScanReport>(Status::NotFound,
                              "this machine has no registered repository with commits in it, so it has nothing of its "
                              "own to measure the world against — clone or init one first");

  Sense eyes(me_.kernel(), &me_);
  for (auto& src : sources_) {
    SenseOptions opt;
    opt.keep = false;            // the page itself is not what is worth keeping
    opt.collect_links = true;
    opt.max_text = 4000;
    auto page = eyes.look(src.url, opt);
    src.last_ms = me_.kernel().host().monotonic_ns() / 1000000ull;
    if (!page.ok()) {
      src.last_ok = false;
      src.last_result = page.error().message;
      rep.failures.push_back(src.name + ": " + page.error().message);
      continue;
    }
    ++rep.sources_read;
    src.reads++;
    src.last_ok = true;

    // Score everything the page points at, keep the best few that clear the bar.
    std::vector<Signal> found;
    std::vector<std::string> seen_urls;
    for (const auto& link : page->links) {
      if (link.text.size() < 18 || link.text.size() > 180) continue;
      size_t words = 1;
      for (char c : link.text) if (c == ' ') ++words;
      if (words < 3) continue;
      bool dup = false;
      for (const auto& u : seen_urls) if (u == link.url) { dup = true; break; }
      if (dup) continue;
      seen_urls.push_back(link.url);
      ++rep.considered;
      Signal s;
      s.title = link.text;
      s.url = link.url;
      s.source = src.name;
      s.score = horizon_score(link.text, interest, &s.matched);
      if (s.score < floor) continue;
      found.push_back(s);
    }
    src.considered = seen_urls.size();
    std::sort(found.begin(), found.end(), [](const Signal& a, const Signal& b) { return a.score > b.score; });
    if (found.size() > keep_per_source) found.resize(keep_per_source);

    uint64_t kept_here = 0;
    for (auto& s : found) {
      // The same thing seen again is not a second thing on the horizon.
      bool known = false;
      for (const auto& id : me_.kernel().store().find_meta("url", s.url)) {
        auto got = me_.kernel().store().get(id);
        if (got.ok() && got->meta("kind") == "signal") { s.object = id; known = true; break; }
      }
      if (known) { ++rep.already_known; rep.signals.push_back(s); continue; }
      StoredObject o;
      o.payload = ByteBuffer(s.title.begin(), s.title.end());
      o.metadata.push_back({"kind", "signal"});
      o.metadata.push_back({"url", s.url});
      o.metadata.push_back({"title", s.title});
      o.metadata.push_back({"source", s.source});
      o.metadata.push_back({"score", score_text(s.score)});
      std::string terms;
      for (const auto& m : s.matched) { if (!terms.empty()) terms += " "; terms += m; }
      o.metadata.push_back({"matched", terms});
      o.metadata.push_back({"seen", "horizon"});
      auto id = me_.store(std::move(o));
      if (!id.ok()) {
        rep.failures.push_back(src.name + ": looked, but may not keep what it saw: " + id.error().message);
        break;                                   // authority will not change mid-scan
      }
      s.object = *id;
      s.fresh = true;
      ++kept_here;
      ++rep.kept;
      rep.signals.push_back(s);
    }
    src.kept = kept_here;
    src.last_result = dec(kept_here) + " new of " + dec(src.considered) + " considered";
  }

  std::sort(rep.signals.begin(), rep.signals.end(), [](const Signal& a, const Signal& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.title < b.title;
  });
  rep.took_ms = (me_.kernel().host().monotonic_ns() - t0) / 1000000ull;
  commit();
  return rep;
}

std::vector<Signal> Horizon::signals(size_t max) const {
  std::vector<Signal> out;
  for (const auto& id : me_.kernel().store().find_meta("kind", "signal")) {
    auto got = me_.kernel().store().get(id);
    if (!got.ok()) continue;
    Signal s;
    s.object = id;
    s.title = got->meta("title");
    s.url = got->meta("url");
    s.source = got->meta("source");
    s.score = score_value(got->meta("score"));
    s.first_seen_ms = got->timestamp_ns / 1000000ull;
    std::string terms = got->meta("matched"), word;
    for (char c : terms + " ") {
      if (c == ' ') { if (!word.empty()) s.matched.push_back(word); word.clear(); }
      else word += c;
    }
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const Signal& a, const Signal& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.first_seen_ms > b.first_seen_ms;
  });
  if (out.size() > max) out.resize(max);
  return out;
}

}  // namespace aios::userland
