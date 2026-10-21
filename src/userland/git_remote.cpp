#include "aios/userland/git_remote.hpp"

#include <chrono>
#include <queue>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "aios/channel/http_client.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/userland/archive.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/net/ssh.hpp"
#include "aios/net/ssh_agent.hpp"
#include "aios/net/ssh_config.hpp"
#include "aios/net/ssh_known_hosts.hpp"

namespace aios::userland {
namespace {

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}

std::string hex20(const uint8_t* p) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(40);
  for (int i = 0; i < 20; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------------
// The byte stream to the remote's upload-pack, whatever carries it
// ---------------------------------------------------------------------------
class Wire {
 public:
  virtual ~Wire() = default;
  virtual Result<void> send(ByteSpan data) = 0;
  virtual Result<void> send_eof() { return Ok(); }
  // Exactly n bytes, or the reason there are not that many.
  virtual Result<ByteBuffer> read_exact(size_t n, int timeout_ms) = 0;
  virtual std::string describe() const = 0;
  // Bytes already received and not yet read (a hint for sizing a buffer; 0 when the wire does not know).
  virtual size_t buffered() const { return 0; }
  // The rest of a side-band stream, when all of it is already here: the band-1 payloads of the frames still unread, up
  // to the flush that ends them, become `pack`, after what it holds already (the frame the caller has just read).
  // Progress (band 2) is added to `progress`; band 3 is the remote's error. False when this wire cannot do it, and
  // then nothing has been read: the caller reads frame by frame as before.
  virtual Result<bool> take_sideband(ByteBuffer& pack, std::string& progress) { return false; }
};

// git:// — the daemon protocol: a request line, then upload-pack speaks.
class DaemonWire final : public Wire {
 public:
  explicit DaemonWire(std::unique_ptr<hal::Connection> c) : c_(std::move(c)) {}
  Result<void> send(ByteSpan data) override {
    if (!c_->send_all(data)) return Result<void>(Status::IoError, "the connection dropped while sending");
    return Ok();
  }
  Result<ByteBuffer> read_exact(size_t n, int timeout_ms) override {
    ByteBuffer out(n);
    if (n == 0) return out;
    if (!c_->recv_all(MutableByteSpan(out.data(), n), timeout_ms)) {
      if (!c_->alive()) return Result<ByteBuffer>(Status::IoError, "the git daemon closed the connection");
      return Result<ByteBuffer>(Status::Busy, "the git daemon sent nothing for " + dec((uint64_t)timeout_ms) + " ms");
    }
    return out;
  }
  std::string describe() const override { return "git://" + c_->peer(); }

 private:
  std::unique_ptr<hal::Connection> c_;
};

// ssh:// — upload-pack runs as a command on the far side of a session channel.
class SshWire final : public Wire {
 public:
  explicit SshWire(std::unique_ptr<net::SshSession> s) : s_(std::move(s)) {}
  Result<void> send(ByteSpan data) override { return s_->send(data); }
  Result<void> send_eof() override { return s_->send_eof(); }
  Result<ByteBuffer> read_exact(size_t n, int timeout_ms) override {
    // Taken from the front by an offset: erasing what was read moved everything after it, once per pkt-line of
    // a pack (four bytes of length, then the line), quadratic in what was buffered.
    while (buf_.size() - at_ < n) {
      if (s_->eof())
        return Result<ByteBuffer>(Status::IoError,
                                 "the remote git-upload-pack ended after " + dec(buf_.size() - at_) + " of " + dec(n) +
                                     " bytes" + (s_->stderr_text().empty() ? "" : ": " + s_->stderr_text()));
      auto chunk = s_->recv(timeout_ms);
      if (!chunk.ok()) return Result<ByteBuffer>(chunk.error());
      if (at_ > 0 && at_ >= buf_.size() / 2) {   // the read part is most of it: drop it before growing
        buf_.erase(buf_.begin(), buf_.begin() + (long)at_);
        at_ = 0;
      }
      buf_.insert(buf_.end(), chunk->begin(), chunk->end());
    }
    ByteBuffer out(buf_.begin() + (long)at_, buf_.begin() + (long)(at_ + n));
    at_ += n;
    if (at_ == buf_.size()) { buf_.clear(); at_ = 0; }
    return out;
  }
  net::SshSession& session() { return *s_; }
  std::string describe() const override { return "ssh " + s_->server_version(); }

 private:
  std::unique_ptr<net::SshSession> s_;
  ByteBuffer buf_;
  size_t at_ = 0;   // buf_[0, at_) has been read
};

// http:// and https:// — git's smart-HTTP transport, stateless RPC (protocol
// v0, or v2 when asked for and the server speaks it). The ref advertisement
// is GET <repo>/info/refs?service=<service>; each round of the conversation
// after it is ONE POST to <repo>/<service> carrying everything sent since the
// last answer, and the answer is read back as the stream. https rides the machine's own TLS client (channel::http_send).
class HttpWire final : public Wire {
 public:
  HttpWire(hal::Host& host, std::string kernel_root, channel::HttpEndpoint repo, std::string service,
           std::vector<std::pair<std::string, std::string>> extra = {})
      : host_(host), root_(std::move(kernel_root)), repo_(std::move(repo)), service_(std::move(service)), extra_(std::move(extra)) {
    while (repo_.path.size() > 1 && repo_.path.back() == '/') repo_.path.pop_back();
  }

  // Ask for protocol v2 (the Git-Protocol header on every request). A server that does not know it ignores the
  // header and answers v0; the caller then calls speak_v0 so the rest of the conversation does not carry it.
  void ask_v2() { v2_ = true; }
  void speak_v0() { v2_ = false; }

  // The advertisement, minus the "# service=" banner and its flush.
  Result<void> open(int timeout_ms, channel::HttpTransportNote* note) {
    channel::HttpRequest rq;
    rq.method = "GET";
    rq.endpoint = repo_;
    rq.endpoint.path = repo_.path + "/info/refs?service=" + service_;
    rq.headers = {{"User-Agent", "git/2.0 (aios)"}, {"Accept", "*/*"}, {"Pragma", "no-cache"}};
    for (auto& h : extra_) rq.headers.push_back(h);
    if (v2_) rq.headers.push_back({"Git-Protocol", "version=2"});
    rq.timeout_ms = timeout_ms;
    rq.host = &host_;
    rq.kernel_root = root_;
    auto r = channel::http_send_noted(rq, note);
    if (!r.ok()) return Result<void>(r.error());
    if (r->status != 200)
      return Result<void>(r->status == 401 || r->status == 403 ? Status::Denied : Status::NotFound,
                          describe() + " answered HTTP " + std::to_string(r->status) + " " + r->reason +
                              " to info/refs?service=" + service_);
    if (r->header("Content-Type").find("application/x-" + service_ + "-advertisement") == std::string::npos)
      return Result<void>(Status::Unsupported, describe() + " is not a smart-HTTP git server (Content-Type '" +
                                                   r->header("Content-Type") +
                                                   "'); the dumb HTTP protocol is not supported");
    in_ = std::move(r->body);
    pos_ = 0;
    // "001e# service=git-upload-pack\n" then "0000".
    auto banner = read_exact(4, timeout_ms);
    if (!banner.ok()) return Result<void>(banner.error());
    size_t n = 0;
    for (uint8_t ch : *banner) {
      int v = hexval((char)ch);
      if (v < 0) return Result<void>(Status::Corrupt, "smart-HTTP advertisement does not start with a pkt-line");
      n = n * 16 + (size_t)v;
    }
    if (n < 4 || pos_ + (n - 4) + 4 > in_.size())
      return Result<void>(Status::Corrupt, "smart-HTTP advertisement is truncated");
    std::string line((const char*)in_.data() + pos_, n - 4);
    pos_ += n - 4;
    // git http-backend leaves the banner out of a v2 answer: it starts with "version 2" instead, which is
    // then left for the caller to read.
    if (v2_ && line.rfind("version 2", 0) == 0) { pos_ = 0; return Ok(); }
    if (line.rfind("# service=" + service_, 0) != 0)
      return Result<void>(Status::Corrupt, "smart-HTTP advertisement names no '# service=" + service_ + "'");
    if (std::memcmp(in_.data() + pos_, "0000", 4) == 0) pos_ += 4;
    return Ok();
  }

  Result<void> send(ByteSpan data) override {
    out_.insert(out_.end(), data.begin(), data.end());
    return Ok();
  }

  Result<ByteBuffer> read_exact(size_t n, int timeout_ms) override {
    while (in_.size() - pos_ < n) {
      if (out_.empty())
        return Result<ByteBuffer>(Status::IoError, "the remote's answer ended after " + dec(in_.size() - pos_) +
                                                       " of " + dec(n) + " bytes");
      if (auto r = post(timeout_ms); !r.ok()) return Result<ByteBuffer>(r.error());
    }
    ByteBuffer out(in_.begin() + (long)pos_, in_.begin() + (long)(pos_ + n));
    pos_ += n;
    if (pos_ == in_.size()) { ByteBuffer().swap(in_); pos_ = 0; }   // all read: give the memory back now, not at the end
    return out;
  }

  size_t buffered() const override { return in_.size() - pos_; }

  // The answer is one buffer, all of it here once the last request is sent, and a pack copied out of it frame by frame
  // held the pack twice (the answer, and the pack being put together) until the end. The frames are stripped where they
  // lie instead: each payload is moved down over the frame headers before it, and the answer's buffer becomes the pack.
  Result<bool> take_sideband(ByteBuffer& pack, std::string& progress) override {
    // What the caller already holds goes in front, over bytes it has read (the frame it came from, at least).
    if (!out_.empty() || in_.empty() || pos_ < pack.size()) return false;
    std::memcpy(in_.data(), pack.data(), pack.size());
    size_t w = pack.size(), r = pos_;
    for (;;) {
      if (r + 4 > in_.size()) return Result<bool>(Status::IoError, "the remote's answer ended inside the pack");
      size_t n = 0;
      for (size_t i = 0; i < 4; ++i) {
        int v = hexval((char)in_[r + i]);
        if (v < 0) return Result<bool>(Status::Corrupt, "not a pkt-line length inside the pack's side-band frames");
        n = n * 16 + (size_t)v;
      }
      if (n == 0) { r += 4; break; }
      if (n < 4 || r + n > in_.size())
        return Result<bool>(Status::IoError, "the remote's answer ended inside a side-band frame");
      if (n > 4) {
        const uint8_t band = in_[r + 4];
        if (band == 1) {
          std::memmove(in_.data() + w, in_.data() + r + 5, n - 5);
          w += n - 5;
        } else if (band == 2) {
          progress.append((const char*)in_.data() + r + 5, n - 5);
        } else if (band == 3) {
          return Result<bool>(Status::IoError, "the remote failed: " + std::string((const char*)in_.data() + r + 5, n - 5));
        } else {
          return Result<bool>(Status::Corrupt, "a side-band frame on band " + dec(band));
        }
      }
      r += n;
    }
    // Anything after the flush ends the answer and is not part of the pack.
    in_.resize(w);
    pack.swap(in_);
    ByteBuffer().swap(in_);
    pos_ = 0;
    return true;
  }

  std::string describe() const override {
    return std::string(repo_.secure ? "https://" : "http://") + repo_.host + ":" + std::to_string(repo_.port) + repo_.path;
  }

 private:
  // One round: POST what has been said since the last answer; the answer
  // becomes what is read next.
  Result<void> post(int timeout_ms) {
    channel::HttpRequest rq;
    rq.method = "POST";
    rq.endpoint = repo_;
    rq.endpoint.path = repo_.path + "/" + service_;
    rq.headers = {{"User-Agent", "git/2.0 (aios)"},
                  {"Content-Type", "application/x-" + service_ + "-request"},
                  {"Accept", "application/x-" + service_ + "-result"}};
    for (auto& h : extra_) rq.headers.push_back(h);
    if (v2_) rq.headers.push_back({"Git-Protocol", "version=2"});
    rq.body = std::move(out_);
    out_.clear();
    rq.timeout_ms = timeout_ms;
    rq.host = &host_;
    rq.kernel_root = root_;
    auto r = channel::http_send(rq);
    if (!r.ok()) return Result<void>(r.error());
    if (r->status != 200)
      return Result<void>(r->status == 401 || r->status == 403 ? Status::Denied : Status::IoError,
                          describe() + " answered HTTP " + std::to_string(r->status) + " " + r->reason + " to " +
                              service_);
    if (pos_ >= in_.size()) {
      in_ = std::move(r->body);   // nothing left unread: the answer is the buffer, not a copy of it
    } else {
      in_.erase(in_.begin(), in_.begin() + (long)pos_);
      in_.insert(in_.end(), r->body.begin(), r->body.end());
    }
    pos_ = 0;
    return Ok();
  }

  hal::Host& host_;
  std::string root_;
  channel::HttpEndpoint repo_;
  std::string service_;
  std::vector<std::pair<std::string, std::string>> extra_;
  ByteBuffer in_, out_;
  size_t pos_ = 0;
  bool v2_ = false;
};

// ---------------------------------------------------------------------------
// pkt-line
// ---------------------------------------------------------------------------
struct Pkt {
  enum Kind { Data, Flush, Delim } kind = Data;
  ByteBuffer payload;
};

Result<Pkt> read_pkt(Wire& w, int timeout_ms) {
  auto head = w.read_exact(4, timeout_ms);
  if (!head.ok()) return Result<Pkt>(head.error());
  int n = 0;
  for (int i = 0; i < 4; ++i) {
    int v = hexval((char)(*head)[(size_t)i]);
    if (v < 0) return Result<Pkt>(Status::Corrupt, "not a pkt-line length: " + std::string((const char*)head->data(), 4));
    n = n * 16 + v;
  }
  Pkt p;
  if (n == 0) { p.kind = Pkt::Flush; return p; }
  if (n == 1) { p.kind = Pkt::Delim; return p; }
  if (n < 4 || n > 65524) return Result<Pkt>(Status::Corrupt, "implausible pkt-line length " + dec((uint64_t)n));
  auto body = w.read_exact((size_t)n - 4, timeout_ms);
  if (!body.ok()) return Result<Pkt>(body.error());
  p.payload = *body;
  return p;
}

// A push's report: its pkt-lines up to the flush. With side-band, band 1 carries the report (pkt-lines of its own),
// band 2 what the remote says to the user (its hooks' words, which git prints as "remote: ..."), band 3 a fatal error.
Result<std::vector<std::string>> read_push_report(Wire& w, int timeout_ms, bool sideband, std::string& messages) {
  std::vector<std::string> lines;
  ByteBuffer inner;
  for (;;) {
    auto p = read_pkt(w, timeout_ms);
    if (!p.ok()) {
      if (!lines.empty() || !inner.empty()) break;   // the remote hung up after answering
      return Result<std::vector<std::string>>(p.error());
    }
    if (p->kind == Pkt::Flush) break;
    if (!sideband) {
      std::string l((const char*)p->payload.data(), p->payload.size());
      while (!l.empty() && l.back() == '\n') l.pop_back();
      lines.push_back(l);
      continue;
    }
    if (p->payload.empty()) continue;
    const uint8_t band = p->payload[0];
    if (band == 1) inner.insert(inner.end(), p->payload.begin() + 1, p->payload.end());
    else messages.append((const char*)p->payload.data() + 1, p->payload.size() - 1);
  }
  for (size_t at = 0; sideband && at + 4 <= inner.size();) {
    int n = 0;
    for (int i = 0; i < 4; ++i) { int v = hexval((char)inner[at + (size_t)i]); if (v < 0) { n = -1; break; } n = n * 16 + v; }
    if (n <= 0 || at + (size_t)n > inner.size()) break;   // the flush (or the end)
    std::string l((const char*)inner.data() + at + 4, (size_t)n - 4);
    while (!l.empty() && l.back() == '\n') l.pop_back();
    lines.push_back(l);
    at += (size_t)n;
  }
  return lines;
}

void write_pkt(ByteBuffer& out, const std::string& line) {
  size_t n = line.size() + 4;
  static const char* d = "0123456789abcdef";
  out.push_back((uint8_t)d[(n >> 12) & 15]);
  out.push_back((uint8_t)d[(n >> 8) & 15]);
  out.push_back((uint8_t)d[(n >> 4) & 15]);
  out.push_back((uint8_t)d[n & 15]);
  out.insert(out.end(), line.begin(), line.end());
}

void write_flush(ByteBuffer& out) {
  const char* f = "0000";
  out.insert(out.end(), f, f + 4);
}

// Protocol v2's section separator.
void write_delim(ByteBuffer& out) {
  const char* f = "0001";
  out.insert(out.end(), f, f + 4);
}

std::string trim_nl(const ByteBuffer& b) {
  std::string s((const char*)b.data(), b.size());
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

// ---------------------------------------------------------------------------
// Packfile
// ---------------------------------------------------------------------------
struct PackCursor {
  ByteSpan d;
  size_t pos = 0;
  bool bad = false;
  uint8_t u8() { if (pos >= d.size()) { bad = true; return 0; } return d[pos++]; }
  uint32_t be32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | u8(); return v; }
};

// The delta interpreter lives in git.cpp (git_apply_delta) so that reading a
// local pack and indexing a fetched one apply deltas the same way.
Result<ByteBuffer> apply_delta(ByteSpan base, ByteSpan delta) { return git_apply_delta(base, delta); }

const char* type_name(int t) {
  switch (t) {
    case 1: return "commit";
    case 2: return "tree";
    case 3: return "blob";
    case 4: return "tag";
    default: return "";
  }
}

}  // namespace

Result<GitUrl> git_parse_url(const std::string& url) {
  GitUrl u;
  if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) {
    auto ep = channel::parse_endpoint(url);
    if (!ep.ok()) return Result<GitUrl>(ep.error());
    u.scheme = ep->secure ? "https" : "http";
    u.host = ep->host;
    u.port = ep->port;
    u.path = ep->path;
    u.user = ep->user;
    u.password = ep->password;  // without it a token-authenticated remote can never be reached
    if (u.path.find('?') != std::string::npos)
      return Result<GitUrl>(Status::Invalid, url + ": a repository URL carries no query string");
    return u;
  }
  if (url.rfind("ssh://", 0) == 0) {
    u.scheme = "ssh";
    std::string rest = url.substr(6);
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return Result<GitUrl>(Status::Invalid, "no path in " + url);
    std::string authority = rest.substr(0, slash);
    u.path = rest.substr(slash);
    size_t at = authority.find('@');
    if (at != std::string::npos) { u.user = authority.substr(0, at); authority = authority.substr(at + 1); }
    size_t colon = authority.find(':');
    if (colon != std::string::npos) {
      uint32_t p = 0;
      for (size_t i = colon + 1; i < authority.size(); ++i) {
        if (authority[i] < '0' || authority[i] > '9') return Result<GitUrl>(Status::Invalid, "bad port in " + url);
        p = p * 10 + (uint32_t)(authority[i] - '0');
      }
      u.port = (uint16_t)p;
      authority = authority.substr(0, colon);
    }
    u.host = authority;
    return u;
  }
  if (url.rfind("git://", 0) == 0) {
    u.scheme = "git";
    std::string rest = url.substr(6);
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return Result<GitUrl>(Status::Invalid, "no path in " + url);
    std::string authority = rest.substr(0, slash);
    u.path = rest.substr(slash);
    size_t colon = authority.find(':');
    if (colon != std::string::npos) {
      uint32_t p = 0;
      for (size_t i = colon + 1; i < authority.size(); ++i) p = p * 10 + (uint32_t)(authority[i] - '0');
      u.port = (uint16_t)p;
      authority = authority.substr(0, colon);
    }
    u.host = authority;
    u.port = u.port ? u.port : 9418;
    return u;
  }
  // scp form: user@host:path — a colon before any slash
  size_t colon = url.find(':');
  size_t slash = url.find('/');
  if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
    u.scheme = "ssh";
    std::string authority = url.substr(0, colon);
    u.path = url.substr(colon + 1);
    size_t at = authority.find('@');
    if (at != std::string::npos) { u.user = authority.substr(0, at); authority = authority.substr(at + 1); }
    u.host = authority;
    return u;
  }
  if (!url.empty() && url[0] == '/') {
    u.scheme = "file";
    u.path = url;
    return u;
  }
  return Result<GitUrl>(Status::Invalid,
                       url + ": not a git URL this machine can reach (ssh://user@host/path, user@host:path, git://host/path or https://host/path)");
}

GitRemote::GitRemote(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

Result<void> GitRemote::may_network() const {
  if (!actor_) return Ok();
  auto r = actor_->may(ResourceRef{ResourceKind::Network, {}}, R_Consume);
  if (!r.ok())
    return Result<void>(Status::Denied, "entity " + actor_->self().short_str() +
                                            " may not reach a remote: " + r.error().str());
  return Ok();
}

namespace {

struct Conversation {
  std::unique_ptr<Wire> wire;
  GitFetchReport report;
  bool stateless = false;   // smart HTTP: each answer is read only after a whole request is sent
  HttpWire* http = nullptr; // the same wire, when it is smart HTTP (to drop the v2 request on a v0 answer)
  bool v2 = false;          // the remote answered in protocol v2 (read_advertisement found "version 2")
  std::vector<std::string> v2_caps;   // its capability lines ("ls-refs=unborn", "fetch=shallow filter", ...)
};

// ---------------------------------------------------------------------------
// ssh as OpenSSH reaches a destination: ssh_config names it, the person's
// agent and key files prove who they are, their known_hosts says who it is
// ---------------------------------------------------------------------------
const std::vector<std::string>& client_host_key_algorithms() {
  static const std::vector<std::string> all = {"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
                                               "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256"};
  return all;
}

// The algorithms a host is known under go first, so a host recorded by its ed25519 key is not met by its RSA key
// and refused as unknown (OpenSSH orders its offer the same way).
std::vector<std::string> host_key_offer(const std::vector<std::string>& known_types) {
  std::vector<std::string> out;
  auto put = [&](const std::string& a) { if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a); };
  for (auto& t : known_types) {
    if (t == "ssh-rsa") { put("rsa-sha2-512"); put("rsa-sha2-256"); }
    else put(t);
  }
  for (auto& a : client_host_key_algorithms()) put(a);
  return out;
}

struct SshRoute {
  net::SshTarget target;
  net::SshConnectOptions options;
};

// One destination ("aar", "git@github-qbcsystems", a ProxyJump hop) made ready to connect.
// `follow_jumps` false for a ProxyJump hop: OpenSSH takes the chain once, from the destination's own list.
Result<SshRoute> ssh_route(Kernel& kernel, const std::string& name, const std::string& user, uint16_t port,
                           const GitRemoteOptions& opt, bool follow_jumps = true) {
  hal::Host& host = kernel.host();
  auto rs = net::ssh_resolve(host, name, user, port);
  if (!rs.ok()) return Result<SshRoute>(rs.error());
  if (!rs->proxy_command.empty() && rs->proxy_command != "none")
    return Result<SshRoute>(Status::Unsupported, name + ": its ProxyCommand runs another program to carry the connection; "
                                                        "this machine's client carries its own (ProxyJump works)");
  SshRoute route;
  route.target.user = rs->user;
  route.target.host = rs->hostname;
  route.target.port = rs->port;
  net::SshConnectOptions& o = route.options;
  o.timeout_ms = rs->connect_timeout_s > 0 ? rs->connect_timeout_s * 1000 : opt.timeout_ms;
  o.kex_algorithms = rs->kex_algorithms;
  o.ciphers = rs->ciphers;

  // Keys, in OpenSSH's order: the files the config names (each one the agent also holds is signed by the agent),
  // then the agent's other keys unless IdentitiesOnly; with no IdentityFile given, the agent's keys come first and
  // the default files after them.
  auto notice = [&](const std::string& m) { if (opt.ssh_notice) opt.ssh_notice(m); };
  std::vector<std::unique_ptr<net::SshSigner>> agent_keys;
  if (rs->pubkey_authentication && rs->identity_agent != "none") {
    std::string where = rs->identity_agent == "SSH_AUTH_SOCK" ? std::string() : rs->identity_agent;
    auto ag = net::SshAgent::connect(where);
    if (ag.ok()) {
      std::shared_ptr<net::SshAgent> shared(std::move(*ag));
      auto ids = shared->identities();
      if (ids.ok()) for (auto& k : *ids) agent_keys.push_back(net::ssh_agent_signer(shared, k));
      else notice("warning: the ssh agent did not list its keys: " + ids.error().message);
    } else if (ag.error().status != Status::NotFound) {
      notice("warning: could not reach the ssh agent: " + ag.error().message);
    }
  }
  std::vector<std::unique_ptr<net::SshSigner>> file_keys;
  if (rs->pubkey_authentication)
    for (auto& path : rs->identity_files) {
      auto s = net::ssh_file_signer(host, path, rs->batch_mode);
      if (s.ok()) file_keys.push_back(std::move(*s));
      else if (s.error().status != Status::NotFound) notice("Load key \"" + path + "\": " + s.error().message);
    }
  std::vector<ByteBuffer> offered;
  auto offer = [&](std::unique_ptr<net::SshSigner>& s) {
    if (!s) return;
    ByteBuffer b = s->public_blob();
    if (std::find(offered.begin(), offered.end(), b) != offered.end()) { s.reset(); return; }
    offered.push_back(std::move(b));
    o.signers.push_back(std::move(s));
  };
  auto agent_for = [&](const ByteBuffer& blob) -> std::unique_ptr<net::SshSigner>* {
    for (auto& a : agent_keys) if (a && a->public_blob() == blob) return &a;
    return nullptr;
  };
  if (rs->identity_files_given) {
    for (auto& f : file_keys) {
      if (auto* a = agent_for(f->public_blob())) offer(*a);
      else offer(f);
    }
    if (!rs->identities_only) for (auto& a : agent_keys) offer(a);
  } else {
    if (!rs->identities_only) for (auto& a : agent_keys) offer(a);
    for (auto& f : file_keys) {
      if (auto* a = agent_for(f->public_blob())) offer(*a);
      else offer(f);
    }
  }

  // The host key: the person's known_hosts files and StrictHostKeyChecking decide, under the name OpenSSH records.
  net::SshHostKeyPolicy policy;
  policy.user_files = rs->user_known_hosts_files;
  policy.global_files = rs->global_known_hosts_files;
  policy.strict = rs->strict_host_key_checking;
  policy.hash = rs->hash_known_hosts;
  policy.batch = rs->batch_mode;
  const std::string recorded = rs->host_key_alias.empty() ? net::ssh_known_hosts_name(rs->hostname, rs->port) : rs->host_key_alias;
  std::vector<std::string> files = policy.user_files;
  files.insert(files.end(), policy.global_files.begin(), policy.global_files.end());
  o.host_key_algorithms = rs->host_key_algorithms.empty()
                              ? host_key_offer(net::ssh_known_host_algorithms(host, files, recorded))
                              : rs->host_key_algorithms;
  auto sink = opt.ssh_notice;
  o.verify_host_key = [&host, policy, recorded, sink](const net::SshHostKey& key) -> Result<void> {
    std::vector<std::string> notes;
    ByteSpan blob(key.blob.data(), key.blob.size());
    auto r = net::ssh_accept_host_key(host, policy, recorded, recorded, net::ssh_blob_type(blob), blob, &notes);
    if (sink) for (auto& n : notes) sink(n);
    return r;
  };

  // ProxyJump: each hop is a destination of its own, connected through the one before it.
  if (follow_jumps && !rs->proxy_jump.empty() && rs->proxy_jump[0] != "none") {
    for (auto& hop : rs->proxy_jump) {
      std::string hop_user, hop_host = hop;
      uint16_t hop_port = 0;
      size_t at = hop_host.rfind('@');
      if (at != std::string::npos) { hop_user = hop_host.substr(0, at); hop_host = hop_host.substr(at + 1); }
      size_t colon = hop_host.rfind(':');
      if (colon != std::string::npos && hop_host.find(']') == std::string::npos) {
        uint32_t p = 0;
        for (size_t i = colon + 1; i < hop_host.size(); ++i) {
          if (hop_host[i] < '0' || hop_host[i] > '9' || p > 65535) return Result<SshRoute>(Status::Invalid, "bad port in ProxyJump " + hop);
          p = p * 10 + (uint32_t)(hop_host[i] - '0');
        }
        hop_port = (uint16_t)p;
        hop_host = hop_host.substr(0, colon);
      }
      if (hop_host.size() > 2 && hop_host.front() == '[' && hop_host.back() == ']') hop_host = hop_host.substr(1, hop_host.size() - 2);
      auto hr = ssh_route(kernel, hop_host, hop_user, hop_port, opt, false);
      if (!hr.ok()) return Result<SshRoute>(hr.error().status, "ProxyJump " + hop + ": " + hr.error().message);
      auto session = std::make_unique<net::SshSession>(host, std::string());
      Result<void> c = Ok();
      if (o.jumps.empty()) {
        c = session->connect(hr->target, hr->options);
      } else {
        auto stream = o.jumps.back()->open_direct_tcpip(hr->target.host, hr->target.port);
        if (!stream.ok()) return Result<SshRoute>(stream.error().status, "ProxyJump " + hop + ": " + stream.error().message);
        c = session->connect_over(std::move(*stream), hr->target, hr->options);
      }
      if (!c.ok()) return Result<SshRoute>(c.error().status, "ProxyJump " + hop + ": " + c.error().message);
      o.jumps.push_back(std::move(session));
    }
  }
  return route;
}

// Either service on the far side: "git-upload-pack" hands history out,
// "git-receive-pack" takes it in. The conversation that follows differs; the
// way it is opened does not.
// `want_v2` asks upload-pack for protocol v2: the Git-Protocol header over smart HTTP, the daemon request's extra
// "version=2" parameter over git://, GIT_PROTOCOL=version=2 in the remote command's environment over ssh (an SSH "env"
// request before the exec, as git sends it). A server that does not know v2, or does not accept the variable, answers
// v0, and is spoken to in v0.
Result<Conversation> open_service(Kernel& kernel, const GitUrl& u, const GitRemoteOptions& opt,
                                  const std::string& service, bool want_v2 = false) {
  Conversation cv;
  cv.report.host = u.host;
  cv.report.path = u.path;
  cv.report.user = u.user;
  cv.report.transport = u.scheme;

  if (u.scheme == "file")
    return Result<Conversation>(Status::Unsupported,
                               "a local path has no " + service + " to talk to: this machine reaches a repository over "
                               "ssh://, git:// or https://");
  if (u.scheme == "http" || u.scheme == "https") {
    channel::HttpEndpoint ep;
    ep.host = u.host;
    ep.port = u.port;
    ep.path = u.path;
    ep.secure = u.scheme == "https";
    ep.user = u.user;
    ep.password = u.password;
    cv.report.port = u.port;
    auto wire = std::unique_ptr<HttpWire>(new HttpWire(kernel.host(), kernel.config().root, ep, service, opt.http_headers));
    if (want_v2) wire->ask_v2();
    channel::HttpTransportNote note;
    if (auto r = wire->open(opt.timeout_ms, ep.secure ? &note : nullptr); !r.ok()) return Result<Conversation>(r.error());
    if (ep.secure) {
      cv.report.kex = note.tls_version;
      cv.report.cipher = note.cipher;
      cv.report.server_version = note.subject;
      cv.report.host_key_fingerprint = note.trust_anchor;
    }
    cv.stateless = true;
    cv.http = wire.get();
    cv.wire = std::move(wire);
    return cv;
  }
  if (u.scheme == "git") {
    cv.report.port = u.port ? u.port : 9418;
    auto c = hal::net_connect(u.host, cv.report.port, opt.timeout_ms);
    if (!c.ok()) return Result<Conversation>(c.error());
    auto wire = std::unique_ptr<Wire>(new DaemonWire(std::move(*c)));
    // "<service> <path>\0host=<host>\0" — the daemon's one request line. Protocol v2 is asked for after one more NUL
    // ("\0version=2\0"), where git puts its extra parameters: a daemon that knows them hands this on to upload-pack as
    // GIT_PROTOCOL, and an older one ignores what follows the host.
    std::string req = service + " " + u.path;
    req.push_back('\0');
    req += "host=" + u.host;
    req.push_back('\0');
    if (want_v2) {
      req.push_back('\0');
      req += "version=2";
      req.push_back('\0');
    }
    ByteBuffer out;
    write_pkt(out, req);
    if (auto r = wire->send(ByteSpan(out.data(), out.size())); !r.ok()) return Result<Conversation>(r.error());
    cv.wire = std::move(wire);
    return cv;
  }

  // ssh
  cv.report.port = u.port ? u.port : 22;
  std::unique_ptr<net::SshSession> session;
  if (opt.ssh_key.empty()) {
    // the person's ssh: their config, agent, keys and known_hosts, as `ssh` would use them
    auto route = ssh_route(kernel, u.host, u.user, u.port, opt);
    if (!route.ok()) return Result<Conversation>(route.error());
    cv.report.host = route->target.host;
    cv.report.user = route->target.user;
    cv.report.port = route->target.port;
    session.reset(new net::SshSession(kernel.host(), std::string()));
    if (auto r = session->connect(route->target, route->options); !r.ok()) return Result<Conversation>(r.error());
  } else {
    auto pem = kernel.host().read_file(opt.ssh_key);
    if (!pem.ok())
      return Result<Conversation>(Status::NotFound, opt.ssh_key + ": no such private key on this machine");
    auto id = net::ssh_parse_private_key(ByteSpan(pem->data(), pem->size()));
    if (!id.ok()) return Result<Conversation>(id.error());

    net::SshTarget t;
    t.user = u.user;
    t.host = u.host;
    t.port = cv.report.port;
    if (t.user.empty()) return Result<Conversation>(Status::Invalid, "no user in the URL (ssh://user@host/path)");

    session.reset(new net::SshSession(kernel.host(), opt.known_hosts));
    if (auto r = session->connect(t, *id, opt.timeout_ms); !r.ok()) return Result<Conversation>(r.error());
  }
  cv.report.host_key_fingerprint = session->host_key().fingerprint;
  cv.report.server_version = session->server_version();
  cv.report.cipher = session->cipher();
  cv.report.kex = session->kex();
  // Quote the path the way ssh does: the remote runs this through a shell.
  std::string quoted = "'";
  for (char c : u.path) {
    if (c == '\'') quoted += "'\\''";
    else quoted += c;
  }
  quoted += "'";
  std::vector<std::pair<std::string, std::string>> env;
  if (want_v2 && service == "git-upload-pack") env.emplace_back("GIT_PROTOCOL", "version=2");
  if (auto r = session->exec(service + " " + quoted, env); !r.ok()) return Result<Conversation>(r.error());
  cv.wire = std::unique_ptr<Wire>(new SshWire(std::move(session)));
  return cv;
}

// Read the ref advertisement: every ref the remote has, and the capabilities it
// declares on the first line. A protocol v2 server (only ever one that was asked, and only when `cv` is
// given) advertises no refs, only capabilities, one per line: those land in cv->v2_caps and cv->v2 is set;
// the refs are then asked for with ls-refs.
Result<void> read_advertisement(Wire& w, GitFetchReport& rep, int timeout_ms, Conversation* cv = nullptr) {
  bool first = true;
  for (;;) {
    auto p = read_pkt(w, timeout_ms);
    if (!p.ok()) return Result<void>(p.error());
    if (p->kind == Pkt::Flush) break;
    std::string line = trim_nl(p->payload);
    if (line.rfind("ERR ", 0) == 0) return Result<void>(Status::Denied, "the remote refused: " + line.substr(4));
    if (first && cv && line == "version 2") {
      cv->v2 = true;
      for (;;) {
        auto c = read_pkt(w, timeout_ms);
        if (!c.ok()) return Result<void>(c.error());
        if (c->kind == Pkt::Flush) break;
        std::string cap = trim_nl(c->payload);
        rep.server_capabilities += (rep.server_capabilities.empty() ? "" : " ") + cap;
        cv->v2_caps.push_back(cap);
      }
      return Ok();
    }
    if (first && line == "version 1") continue;   // protocol v1 is v0 with this line in front
    if (first) {
      size_t nul = line.find('\0');
      if (nul != std::string::npos) {
        rep.server_capabilities = line.substr(nul + 1);
        line = line.substr(0, nul);
        const std::string marker = "symref=HEAD:";
        size_t at = rep.server_capabilities.find(marker);
        if (at != std::string::npos) {
          size_t end = rep.server_capabilities.find(' ', at);
          rep.default_branch = rep.server_capabilities.substr(at + marker.size(),
                                                             end == std::string::npos ? std::string::npos : end - at - marker.size());
        }
      }
      first = false;
    }
    size_t sp = line.find(' ');
    if (sp != 40) continue;
    GitRemoteRef r;
    r.oid = line.substr(0, 40);
    r.name = line.substr(41);
    // An empty repository advertises "capabilities^{}"; a peeled tag ("refs/tags/v1^{}") names the
    // commit an annotated tag points at and is kept (ls-remote shows it; fetch skips it).
    if (r.name == "capabilities^{}") continue;
    rep.refs.push_back(r);
  }
  return Ok();
}

// Whether a v2 server advertised `cap` ("ls-refs", "fetch"), and with `feature` given, whether that is among
// the capability's values ("fetch=shallow filter" has "shallow").
bool v2_has(const Conversation& cv, const std::string& cap, const std::string& feature = "") {
  for (auto& line : cv.v2_caps) {
    if (line != cap && line.rfind(cap + "=", 0) != 0) continue;
    if (feature.empty()) return true;
    std::string values = line.size() > cap.size() ? line.substr(cap.size() + 1) : "";
    for (size_t at = 0; at <= values.size();) {
      size_t sp = values.find(' ', at);
      if (values.substr(at, sp == std::string::npos ? std::string::npos : sp - at) == feature) return true;
      if (sp == std::string::npos) break;
      at = sp + 1;
    }
  }
  return false;
}

// A v2 command's request: "command=<name>", then (after a delimiter) its arguments, then `tail` (arguments
// already written as pkt-lines), then a flush.
Result<void> v2_send_command(Conversation& cv, const std::string& command, const std::vector<std::string>& args,
                             ByteSpan tail = ByteSpan()) {
  // This machine reads and writes SHA-1 objects only: a repository in another format is refused here, by name,
  // rather than misread later.
  for (auto& line : cv.v2_caps)
    if (line.rfind("object-format=", 0) == 0 && line != "object-format=sha1")
      return Result<void>(Status::Unsupported, "the remote's objects are " + line.substr(14) + "; this machine reads sha1 only");
  ByteBuffer req;
  write_pkt(req, "command=" + command + "\n");
  if (v2_has(cv, "object-format")) write_pkt(req, "object-format=sha1\n");
  write_delim(req);
  for (auto& a : args) write_pkt(req, a + "\n");
  req.insert(req.end(), tail.begin(), tail.end());
  write_flush(req);
  return cv.wire->send(ByteSpan(req.data(), req.size()));
}

// ls-refs (protocol v2): the refs whose names start with one of `prefixes` (every ref when there are none),
// listed into the report the way a v0 advertisement would have listed them: HEAD's target as default_branch,
// and an annotated tag's commit as "<tag>^{}".
Result<void> v2_ls_refs(Conversation& cv, const std::vector<std::string>& prefixes, int timeout_ms) {
  if (!v2_has(cv, "ls-refs"))
    return Result<void>(Status::Unsupported, "the remote speaks protocol v2 but offers no ls-refs");
  std::vector<std::string> args{"peel", "symrefs"};
  // An empty repository has a HEAD that points at a branch with no commit;
  // a server that offers "unborn" names that branch ("unborn HEAD
  // symref-target:refs/heads/main"), so a clone of it stands on the branch
  // the remote will grow, not on a guess.
  if (v2_has(cv, "ls-refs", "unborn")) args.push_back("unborn");
  for (auto& p : prefixes) args.push_back("ref-prefix " + p);
  if (auto r = v2_send_command(cv, "ls-refs", args); !r.ok()) return r;
  GitFetchReport& rep = cv.report;
  for (;;) {
    auto p = read_pkt(*cv.wire, timeout_ms);
    if (!p.ok()) return Result<void>(p.error());
    if (p->kind == Pkt::Flush) break;
    if (p->kind == Pkt::Delim) return Result<void>(Status::Corrupt, "ls-refs answered with a section delimiter");
    std::string line = trim_nl(p->payload);
    if (line.rfind("ERR ", 0) == 0) return Result<void>(Status::Denied, "the remote refused: " + line.substr(4));
    if (line.rfind("unborn HEAD", 0) == 0) {
      const std::string marker = " symref-target:";
      size_t at = line.find(marker);
      if (at != std::string::npos) {
        size_t end = line.find(' ', at + marker.size());
        rep.default_branch = line.substr(at + marker.size(), end == std::string::npos ? std::string::npos : end - at - marker.size());
      }
      continue;
    }
    // "<oid> <name>[ symref-target:<ref>][ peeled:<oid>]"
    if (line.size() < 42 || line[40] != ' ') continue;
    GitRemoteRef r;
    r.oid = line.substr(0, 40);
    size_t sp = line.find(' ', 41);
    r.name = line.substr(41, sp == std::string::npos ? std::string::npos : sp - 41);
    std::string peeled;
    while (sp != std::string::npos) {
      size_t next = line.find(' ', sp + 1);
      std::string attr = line.substr(sp + 1, next == std::string::npos ? std::string::npos : next - sp - 1);
      if (attr.rfind("symref-target:", 0) == 0 && r.name == "HEAD") rep.default_branch = attr.substr(14);
      else if (attr.rfind("peeled:", 0) == 0) peeled = attr.substr(7);
      sp = next;
    }
    rep.refs.push_back(r);
    if (peeled.size() == 40) rep.refs.push_back(GitRemoteRef{r.name + "^{}", peeled});
  }
  return Ok();
}

// fetch (protocol v2): one request with every want, the shallow state, the haves and "done" (so the server
// answers with the pack at once, with no rounds of acknowledgments), and an answer in sections: optional
// acknowledgments, shallow-info (the new edges of a truncated history), wanted-refs, packfile-uris, and the
// packfile itself in side-band frames. Sections are separated by delimiters; a flush ends the answer.
Result<void> fetch_pack_v2(Conversation& cv, const GitRemoteOptions& opt, const std::vector<GitRemoteRef>& wants,
                           const std::vector<std::string>& had_shallow,
                           const std::function<size_t(ByteBuffer&)>& collect_haves, ByteBuffer& pack,
                           std::vector<std::string>& shallow_boundary, std::unordered_set<std::string>& unshallowed) {
  GitFetchReport& rep = cv.report;
  const bool shallow = opt.depth > 0;
  if (!v2_has(cv, "fetch")) return Result<void>(Status::Unsupported, "the remote speaks protocol v2 but offers no fetch");
  // In v2 deepen, deepen-relative and the client's shallow lines all belong to the one "shallow" feature.
  if ((shallow || !had_shallow.empty() || opt.deepen_relative) && !v2_has(cv, "fetch", "shallow"))
    return Result<void>(Status::Unsupported, "the remote will not truncate history (no shallow capability)");

  // thin-pack, as in v0: the deltas may lean on objects this repository has, which index-pack appends to the pack.
  std::vector<std::string> args{"thin-pack", "ofs-delta"};
  if (opt.include_tag) args.push_back("include-tag");
  for (auto& r : wants) args.push_back("want " + r.oid);
  for (auto& s : had_shallow) args.push_back("shallow " + s);
  if (shallow) args.push_back("deepen " + dec((uint64_t)opt.depth));
  if (shallow && opt.deepen_relative) args.push_back("deepen-relative");
  if (!opt.filter.empty()) {
    if (!v2_has(cv, "fetch", "filter")) return Result<void>(Status::Unsupported, "the remote does not filter objects (no filter capability)");
    args.push_back("filter " + opt.filter);
  }
  // The haves come as pkt-lines already, and "done" goes last, as git sends it.
  ByteBuffer haves;
  rep.haves = collect_haves(haves);
  write_pkt(haves, "done\n");
  if (auto r = v2_send_command(cv, "fetch", args, ByteSpan(haves.data(), haves.size())); !r.ok()) return r;

  Wire& w = *cv.wire;
  std::string section;   // the one being read; empty before its header line
  for (;;) {
    auto p = read_pkt(w, opt.timeout_ms);
    if (!p.ok()) return Result<void>(p.error());
    if (p->kind == Pkt::Flush) break;
    if (p->kind == Pkt::Delim) { section.clear(); continue; }
    if (section.empty()) {
      section = trim_nl(p->payload);
      if (section.rfind("ERR ", 0) == 0) return Result<void>(Status::Denied, "the remote refused: " + section.substr(4));
      continue;
    }
    if (section == "packfile") {
      if (p->payload.empty()) continue;
      uint8_t band = p->payload[0];
      if (band == 1) {
        const bool first_frame = pack.empty();
        pack.insert(pack.end(), p->payload.begin() + 1, p->payload.end());
        if (first_frame) {
          // The packfile section is the last, so the rest of the answer is its frames and the flush after them.
          std::string progress;
          auto all = w.take_sideband(pack, progress);
          if (!all.ok()) return Result<void>(all.error());
          if (*all) break;
          pack.reserve(pack.size() + w.buffered());   // what the answer holds: no doubling, no copy of a copy
        }
      } else if (band == 3) {
        return Result<void>(Status::IoError,
                            "the remote failed: " + std::string((const char*)p->payload.data() + 1, p->payload.size() - 1));
      }
      continue;   // band 2 is progress, which nothing here shows
    }
    std::string line = trim_nl(p->payload);
    if (line.rfind("ERR ", 0) == 0) return Result<void>(Status::Denied, "the remote refused: " + line.substr(4));
    if (section == "acknowledgments") {
      if (line.rfind("ACK ", 0) == 0) ++rep.acks;
    } else if (section == "shallow-info") {
      if (line.rfind("shallow ", 0) == 0) shallow_boundary.push_back(line.substr(8, 40));
      else if (line.rfind("unshallow ", 0) == 0) unshallowed.insert(line.substr(10, 40));
    }
    // wanted-refs and packfile-uris answer requests this machine does not make; their lines are passed over.
  }
  // Over a connection a v2 server then waits for another command: a flush says there is none.
  if (!cv.stateless) {
    ByteBuffer bye;
    write_flush(bye);
    w.send(ByteSpan(bye.data(), bye.size()));
  }
  w.send_eof();
  return Ok();
}

}  // namespace

Result<GitFetchReport> GitRemote::ls_remote(const std::string& url, const GitRemoteOptions& opt) {
  if (auto r = may_network(); !r.ok()) return Result<GitFetchReport>(r.error());
  auto u = git_parse_url(url);
  if (!u.ok()) return Result<GitFetchReport>(u.error());
  auto cv = open_service(kernel_, *u, opt, "git-upload-pack", /*want_v2=*/true);
  if (!cv.ok()) return Result<GitFetchReport>(cv.error());
  cv->report.url = url;
  if (auto r = read_advertisement(*cv->wire, cv->report, opt.timeout_ms, &*cv); !r.ok())
    return Result<GitFetchReport>(r.error());
  // A v2 server advertised no refs, only what it can do: ask it for all of them, which is what v0 sent.
  if (cv->v2) {
    cv->report.protocol = 2;
    if (auto r = v2_ls_refs(*cv, {}, opt.timeout_ms); !r.ok()) return Result<GitFetchReport>(r.error());
    // Over a connection (git://) a v2 server waits for another command; a flush says there is none.
    if (!cv->stateless) {
      ByteBuffer bye;
      write_flush(bye);
      cv->wire->send(ByteSpan(bye.data(), bye.size()));
      cv->wire->send_eof();
    }
    return cv->report;
  }
  // Say goodbye properly: a flush with no wants ends the conversation.
  ByteBuffer bye;
  write_flush(bye);
  cv->wire->send(ByteSpan(bye.data(), bye.size()));
  return cv->report;
}

namespace {

// A pack of at least this many objects is kept as a pack (objects/pack/pack-<id>.pack and its .idx); a smaller one is
// unpacked into loose objects, so a repository pushed to many times does not fill with tiny packs. git's own
// transfer.unpackLimit.
const uint32_t kKeepPackAt = 100;

// Keeps a fetched pack as it arrived, beside an index built for it, instead of writing every object out as a file of
// its own: each object is inflated once to learn its id (a delta is applied to its base first), and nothing is
// compressed again. `pack` is the pack without its trailer, which follows it in the caller's buffer.
//
// A thin pack (what a client pushing here sends when this repository already holds the bases of its deltas, and what a
// fetch that asked for thin-pack receives) has deltas whose base is not in it. With `fix_thin` those bases are read
// from this repository and appended to the pack whole, its object count and checksum are rewritten to match, and it is
// kept as one pack, as git's `index-pack --fix-thin` does: unpacking it instead wrote a loose file for every object.
// Status::Unsupported when a base is in neither (or `fix_thin` is off): the caller then unpacks it the long way.
Result<void> keep_pack(Git& repo, hal::Host& host, ByteSpan pack, uint32_t count, uint64_t* deltas_out,
                       std::string* base_out = nullptr, bool fix_thin = false, uint32_t* appended_out = nullptr) {
  struct Entry {
    uint64_t off = 0;
    size_t data = 0, end = 0;     // where its zlib stream starts and where the entry ends
    int type = 0;                 // as stored: 1..4, 6 (offset delta), 7 (ref delta)
    int final_type = 0;           // what the object is, once known
    uint64_t base_off = 0;        // type 6
    uint8_t base_id[20] = {};     // type 7
    uint8_t id[20] = {};
    bool known = false;
  };
  std::vector<Entry> entries(count);
  std::unordered_map<uint64_t, uint32_t> at_offset;    // pack offset -> entry
  std::unordered_map<std::string, uint32_t> by_id;      // raw 20-byte id -> entry
  at_offset.reserve(count * 2);
  by_id.reserve(count * 2);

  auto name = [&](Entry& e, int type, const ByteBuffer& payload) {
    std::string header = std::string(type_name(type)) + " " + dec(payload.size());
    crypto::SHA1Context ctx;
    crypto::sha1_init(ctx);
    crypto::sha1_update(ctx, (const uint8_t*)header.data(), header.size() + 1);   // with its terminating NUL
    crypto::sha1_update(ctx, payload.data(), payload.size());
    crypto::sha1_final(ctx, e.id);
    e.final_type = type;
    e.known = true;
  };

  // ---- first pass: where every entry is, and the id of every whole object ----
  PackCursor pc{pack};
  pc.pos = 12;
  uint64_t deltas = 0;
  for (uint32_t i = 0; i < count; ++i) {
    Entry& e = entries[i];
    e.off = pc.pos;
    uint8_t c = pc.u8();
    e.type = (c >> 4) & 7;
    uint64_t size = c & 15;
    int shift = 4;
    while (c & 0x80) {
      c = pc.u8();
      size |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
      if (pc.bad) break;
    }
    if (pc.bad) return Result<void>(Status::Corrupt, "the pack ends inside an object header");
    if (e.type == 6) {
      uint8_t b = pc.u8();
      uint64_t ofs = b & 0x7f;
      while (b & 0x80) {
        b = pc.u8();
        ofs = ((ofs + 1) << 7) | (b & 0x7f);
        if (pc.bad) break;
      }
      if (pc.bad || ofs > e.off) return Result<void>(Status::Corrupt, "an offset delta points before the pack");
      e.base_off = e.off - ofs;
    } else if (e.type == 7) {
      if (pc.pos + 20 > pack.size()) return Result<void>(Status::Corrupt, "the pack ends inside a delta base id");
      std::memcpy(e.base_id, pack.data() + pc.pos, 20);
      pc.pos += 20;
    } else if (e.type < 1 || e.type > 4) {
      return Result<void>(Status::Corrupt, "the pack holds an object of unknown type " + dec((uint64_t)e.type));
    }
    e.data = pc.pos;
    size_t end = 0;
    auto raw = zlib_inflate_at(pack, pc.pos, &end, (size_t)std::min<uint64_t>(size, (uint64_t)256 << 20));
    if (!raw.ok()) return Result<void>(raw.error());
    if (raw->size() != size)
      return Result<void>(Status::Corrupt, "object " + dec(i) + " says " + dec(size) + " bytes, inflated to " + dec(raw->size()));
    pc.pos = end;
    e.end = end;
    at_offset[e.off] = i;
    if (e.type > 4) ++deltas;   // (whole objects are named in the second pass, on every core, not here on one)
  }
  if (pc.pos != pack.size()) return Result<void>(Status::Corrupt, "the pack holds more than the objects it counts");

  // ---- second pass: the deltas, each object built exactly once ----
  // Every delta hangs under its base, so the pack is a forest with the whole objects at the roots. Each tree is walked
  // from its root down: a base is held only until its last delta has been applied to it (a file's versions are a
  // chain, so two of them are in memory at a time, however long the chain). Rebuilding each object from the root of
  // its chain instead inflated and patched the same large files over and over.
  std::vector<std::vector<uint32_t>> kids(count);
  std::unordered_map<std::string, std::vector<uint32_t>> by_base_id;   // ref deltas, by the id they wait for
  for (uint32_t i = 0; i < count; ++i) {
    const Entry& e = entries[i];
    if (e.type == 6) {
      auto b = at_offset.find(e.base_off);
      if (b == at_offset.end() || b->second >= i) return Result<void>(Status::Corrupt, "an offset delta names a base this pack never had");
      kids[b->second].push_back(i);
    } else if (e.type == 7) {
      by_base_id[std::string((const char*)e.base_id, 20)].push_back(i);
    }
  }
  auto adopt = [&](uint32_t i) {   // the ref deltas that were waiting for entry i's id become its children
    auto w = by_base_id.find(std::string((const char*)entries[i].id, 20));
    if (w == by_base_id.end()) return;
    kids[i].insert(kids[i].end(), w->second.begin(), w->second.end());
    by_base_id.erase(w);
  };
  struct Frame { uint32_t i; int type; ByteBuffer payload; size_t next = 0; };
  // The two maps are shared between the threads below (a ref delta is adopted by whichever tree names its base), so
  // those touches are under a lock; an entry itself belongs to one tree.
  hal::Mutex shared;
  // One tree, from its root (whose whole payload is given) down. `stop` is set when another tree has failed: its error
  // is the one reported, and this one gives up quietly.
  auto grow = [&](uint32_t root, int type, ByteBuffer whole, const std::atomic<bool>& stop) -> Result<void> {
    name(entries[root], type, whole);
    {
      hal::LockGuard g(shared);
      by_id[std::string((const char*)entries[root].id, 20)] = root;
      adopt(root);
    }
    if (kids[root].empty()) return Ok();
    std::vector<Frame> stack;
    stack.push_back(Frame{root, type, std::move(whole)});
    while (!stack.empty()) {
      if (stop.load()) return Ok();
      if (stack.back().next >= kids[stack.back().i].size()) { stack.pop_back(); continue; }
      const uint32_t c = kids[stack.back().i][stack.back().next++];
      Entry& e = entries[c];
      if (e.known) return Result<void>(Status::Corrupt, "the pack's deltas form a loop");
      auto delta = zlib_inflate_at(pack, e.data, nullptr);
      if (!delta.ok()) return Result<void>(delta.error());
      Frame& f = stack.back();
      auto full = apply_delta(ByteSpan(f.payload.data(), f.payload.size()), ByteSpan(delta->data(), delta->size()));
      if (!full.ok()) return Result<void>(full.error());
      const int t = f.type;
      if (f.next >= kids[f.i].size()) ByteBuffer().swap(f.payload);   // its last delta: the base is done with
      name(e, t, *full);
      {
        hal::LockGuard g(shared);
        by_id[std::string((const char*)e.id, 20)] = c;
        adopt(c);
      }
      if (!kids[c].empty()) stack.push_back(Frame{c, t, std::move(*full)});   // (f is not used after this)
    }
    return Ok();
  };
  // The trees are independent of each other, so they are walked on several threads (as git's own index-pack does):
  // most of the work is inflating and hashing, a core each.
  std::vector<uint32_t> roots;
  for (uint32_t r = 0; r < count; ++r) if (entries[r].type <= 4) roots.push_back(r);
  std::atomic<size_t> next_root{0};
  std::atomic<bool> failed{false};
  Result<void> first_error = Ok();
  auto walk = [&]() {
    for (;;) {
      const size_t at = next_root.fetch_add(1);
      if (at >= roots.size() || failed.load()) return;
      const uint32_t root = roots[at];
      auto whole = zlib_inflate_at(pack, entries[root].data, nullptr);
      Result<void> r = whole.ok() ? grow(root, entries[root].type, std::move(*whole), failed) : Result<void>(whole.error());
      if (!r.ok()) {
        hal::LockGuard g(shared);
        if (!failed.exchange(true)) first_error = std::move(r);
        return;
      }
    }
  };
#if defined(__wasi__)
  walk();
#else
  {
    unsigned n = hal::hardware_threads();
    n = n == 0 ? 1 : n > 8 ? 8 : n;
    if (count < 256) n = 1;   // not worth starting threads for
    std::vector<hal::Thread> pool;
    for (unsigned t = 1; t < n; ++t) pool.emplace_back(walk);
    walk();
    for (auto& t : pool) t.join();
  }
#endif
  if (failed.load()) return first_error;

  // ---- a thin pack: the bases it leans on, read from this repository and appended whole ----
  // One at a time, in id order, each tree grown before the next base is looked at: a base that one of those trees
  // builds is then found in the pack and not appended a second time (git's fix-thin goes the same way). A base this
  // repository does not hold waits for a later round, in case a tree grown meanwhile builds it.
  ByteBuffer tail;   // the appended objects, as they follow the pack
  if (!by_base_id.empty()) {
    if (!fix_thin) return Result<void>(Status::Unsupported, "a delta's base is not in the pack");
    std::vector<std::string> todo;
    for (auto& w : by_base_id) todo.push_back(w.first);
    std::sort(todo.begin(), todo.end());
    const std::atomic<bool> no_stop{false};
    while (!todo.empty()) {
      std::vector<std::string> later;
      for (auto& id : todo) {
        if (!by_base_id.count(id)) continue;   // built by a tree grown before it
        auto o = repo.read_object(hex20((const uint8_t*)id.data()));
        if (!o.ok()) { later.push_back(id); continue; }
        int type = 0;
        for (int t = 1; t <= 4; ++t) if (o->type == type_name(t)) type = t;
        if (!type) return Result<void>(Status::Corrupt, "a delta's base is a " + o->type + ", which a pack cannot hold");
        Entry e;
        e.off = pack.size() + tail.size();
        e.type = type;
        // The entry header (the type and the size, four bits of it and then seven at a time), then the payload deflated.
        uint64_t size = o->payload.size();
        uint8_t first = (uint8_t)((type << 4) | (size & 15));
        size >>= 4;
        tail.push_back(size ? (uint8_t)(first | 0x80) : first);
        while (size) {
          uint8_t b = (uint8_t)(size & 0x7f);
          size >>= 7;
          tail.push_back(size ? (uint8_t)(b | 0x80) : b);
        }
        ByteBuffer z = zlib_deflate(ByteSpan(o->payload.data(), o->payload.size()));
        tail.insert(tail.end(), z.begin(), z.end());
        e.end = (size_t)(pack.size() + tail.size());
        entries.push_back(e);
        kids.emplace_back();
        if (auto r = grow((uint32_t)(entries.size() - 1), type, std::move(o->payload), no_stop); !r.ok()) return r;
        if (by_base_id.count(id))
          return Result<void>(Status::Corrupt, "this repository's copy of " + hex20((const uint8_t*)id.data()) +
                                                   " does not hash to that id");
      }
      if (later.size() == todo.size())
        return Result<void>(Status::Unsupported, "a delta's base is in neither the pack nor this repository");
      todo = std::move(later);
    }
  }
  const size_t total = entries.size();
  // Anything still unnamed leans on an object that is in neither.
  for (size_t i = 0; i < total; ++i)
    if (!entries[i].known) return Result<void>(Status::Unsupported, "a delta's base is not in the pack");

  // ---- the index (version 2): fan-out, ids in order, each entry's CRC-32, offsets, both checksums ----
  std::vector<uint32_t> order(total);
  for (uint32_t i = 0; i < (uint32_t)total; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return std::memcmp(entries[a].id, entries[b].id, 20) < 0; });
  ByteBuffer idx;
  idx.reserve(8 + 1024 + total * 28 + 40 + 64);
  auto put32 = [&](uint32_t v) { idx.push_back((uint8_t)(v >> 24)); idx.push_back((uint8_t)(v >> 16)); idx.push_back((uint8_t)(v >> 8)); idx.push_back((uint8_t)v); };
  const uint8_t magic[4] = {0xff, 't', 'O', 'c'};
  idx.insert(idx.end(), magic, magic + 4);
  put32(2);
  {
    uint32_t fan[256] = {};
    for (size_t i = 0; i < total; ++i) ++fan[entries[i].id[0]];
    uint32_t run = 0;
    for (int b = 0; b < 256; ++b) { run += fan[b]; put32(run); }
  }
  for (uint32_t i : order) idx.insert(idx.end(), entries[i].id, entries[i].id + 20);
  for (uint32_t i : order) {
    const Entry& e = entries[i];
    const size_t len = e.end - (size_t)e.off;
    put32(crc32(e.off < pack.size() ? ByteSpan(pack.data() + e.off, len) : ByteSpan(tail.data() + (e.off - pack.size()), len)));
  }
  std::vector<uint64_t> large;
  for (uint32_t i : order) {
    if (entries[i].off < 0x80000000ull) put32((uint32_t)entries[i].off);
    else { put32(0x80000000u | (uint32_t)large.size()); large.push_back(entries[i].off); }
  }
  for (uint64_t o : large) { put32((uint32_t)(o >> 32)); put32((uint32_t)o); }

  // The pack's own checksum is the 20 bytes that follow `pack` in the caller's buffer (checked there, or just
  // computed): not hashed a second time. A pack that had bases appended has a new count in its header and new bytes
  // at its end, so its checksum is worked out again, from the new header on.
  uint8_t head[12];
  std::memcpy(head, pack.data(), 12);
  std::array<uint8_t, 20> pack_sum;
  if (tail.empty()) {
    std::memcpy(pack_sum.data(), pack.data() + pack.size(), 20);
  } else {
    const uint32_t n = (uint32_t)total;
    head[8] = (uint8_t)(n >> 24); head[9] = (uint8_t)(n >> 16); head[10] = (uint8_t)(n >> 8); head[11] = (uint8_t)n;
    crypto::SHA1Context ctx;
    crypto::sha1_init(ctx);
    crypto::sha1_update(ctx, head, 12);
    crypto::sha1_update(ctx, pack.data() + 12, pack.size() - 12);
    crypto::sha1_update(ctx, tail.data(), tail.size());
    crypto::sha1_final(ctx, pack_sum.data());
  }
  idx.insert(idx.end(), pack_sum.begin(), pack_sum.end());
  auto idx_sum = crypto::sha1(ByteSpan(idx.data(), idx.size()));
  idx.insert(idx.end(), idx_sum.begin(), idx_sum.end());

  const std::string dir = repo.common_dir() + "/objects/pack";
  if (auto r = host.make_dir(dir); !r.ok()) return r;
  const std::string base = dir + "/pack-" + hex20(pack_sum.data());
  // The pack first, its index last: a reader finds packs by their index, so a half-written pair is never used.
  // The trailer already follows `pack` in the caller's buffer, so pack and trailer are written straight from it:
  // copying them into one buffer first held a second whole pack in memory at the moment memory peaks. A fixed thin
  // pack goes out in three pieces for the same reason: its new header, the pack's objects as they are, then the
  // appended bases and the new trailer.
  // The pack file is synced on its own, as git does: syncing the whole
  // filesystem (sync_tree) waited on every other program's dirty pages too.
  if (tail.empty()) {
    if (auto r = host.write_file(base + ".pack", ByteSpan(pack.data(), pack.size() + 20)); !r.ok()) return r;
  } else {
    tail.insert(tail.end(), pack_sum.begin(), pack_sum.end());
    if (auto r = host.write_file_unsynced(base + ".pack", ByteSpan(head, 12)); !r.ok()) return r;
    if (auto r = host.append_file_unsynced(base + ".pack", ByteSpan(pack.data() + 12, pack.size() - 12)); !r.ok()) return r;
    if (auto r = host.append_file_unsynced(base + ".pack", ByteSpan(tail.data(), tail.size())); !r.ok()) return r;
    if (auto r = host.sync_file(base + ".pack"); !r.ok()) return r;
  }
  if (auto r = host.write_file(base + ".idx", ByteSpan(idx.data(), idx.size())); !r.ok()) return r;
  repo.rescan_packs();
  if (deltas_out) *deltas_out = deltas;
  if (base_out) *base_out = base;
  if (appended_out) *appended_out = (uint32_t)(total - count);
  return Ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// delta compression (repack): what makes a pack of a file's many versions the size of one of them
// ---------------------------------------------------------------------------
namespace {

// A delta worked out for an object that is not stored as one (git_repack): its base and the delta, already deflated.
struct PlannedDelta { std::string base_oid; uint64_t size = 0; ByteBuffer z; };
// (defined with the rest of the pack writing, further down)
Result<ByteBuffer> build_pack(Git& repo, const std::vector<std::string>& oids, uint64_t& counted,
                              const std::unordered_map<std::string, PlannedDelta>* planned = nullptr, int level = 6);

const size_t kDeltaBlock = 16;        // the base is indexed in blocks of this many bytes; no shorter run is copied
const uint32_t kDeltaPrime = 0x01000193u;

uint32_t delta_hash(const uint8_t* p) {
  uint32_t h = 0;
  for (size_t i = 0; i < kDeltaBlock; ++i) h = h * kDeltaPrime + p[i];
  return h;
}

// Where each block of a base starts, by the hash of its bytes: built once per base, used for every target tried on it.
struct DeltaIndex {
  ByteSpan base;
  std::vector<uint32_t> slots;   // offset + 1 of a block with this hash (0: none); a few probes on a collision
  uint32_t mask = 0;
  uint32_t out_factor = 1;       // kDeltaPrime ^ (kDeltaBlock - 1): what the byte leaving the rolling window was multiplied by
  explicit DeltaIndex(ByteSpan b) : base(b) {
    for (size_t i = 1; i < kDeltaBlock; ++i) out_factor *= kDeltaPrime;
    if (base.size() < kDeltaBlock || base.size() >= 0xfffffff0ull) return;
    size_t blocks = base.size() / kDeltaBlock;
    size_t n = 16;
    while (n < blocks * 2) n <<= 1;
    slots.assign(n, 0);
    mask = (uint32_t)(n - 1);
    // Last block first, so of several identical blocks the earliest ends up found first (it has the most after it).
    for (size_t b2 = blocks; b2-- > 0;) {
      const size_t off = b2 * kDeltaBlock;
      uint32_t at = delta_hash(base.data() + off) & mask;
      for (int probe = 0; probe < 4; ++probe, at = (at + 1) & mask) {
        if (slots[at] == 0 || std::memcmp(base.data() + (slots[at] - 1), base.data() + off, kDeltaBlock) == 0) { slots[at] = (uint32_t)off + 1; break; }
      }
    }
  }
  bool usable() const { return !slots.empty(); }
};

void delta_varint(ByteBuffer& out, uint64_t v) {
  do { uint8_t b = (uint8_t)(v & 0x7f); v >>= 7; if (v) b |= 0x80; out.push_back(b); } while (v);
}

ByteBuffer make_delta(const DeltaIndex& ix, ByteSpan trg, size_t max_size) {
  ByteBuffer out;
  if (!ix.usable() || trg.size() < kDeltaBlock || trg.size() >= 0xfffffff0ull) return out;
  const ByteSpan src = ix.base;
  out.reserve(std::min<size_t>(max_size + 64, trg.size() / 2 + 64));
  delta_varint(out, src.size());
  delta_varint(out, trg.size());
  size_t lit = 0;   // target bytes [lit, pos) are waiting to go out as they are
  auto flush_literals = [&](size_t upto) {
    while (lit < upto) {
      size_t n = std::min<size_t>(127, upto - lit);
      out.push_back((uint8_t)n);
      out.insert(out.end(), trg.begin() + (long)lit, trg.begin() + (long)(lit + n));
      lit += n;
    }
  };
  auto copy = [&](size_t off, size_t len) {
    while (len) {
      size_t n = std::min<size_t>(len, 0x10000);
      size_t cmd_at = out.size();
      out.push_back(0x80);
      uint8_t cmd = 0x80;
      for (int i = 0; i < 4; ++i) { uint8_t b = (uint8_t)(off >> (8 * i)); if (b) { out.push_back(b); cmd |= (uint8_t)(1 << i); } }
      if (n != 0x10000) for (int i = 0; i < 2; ++i) { uint8_t b = (uint8_t)(n >> (8 * i)); if (b) { out.push_back(b); cmd |= (uint8_t)(0x10 << i); } }
      out[cmd_at] = cmd;
      off += n;
      len -= n;
    }
  };
  const uint8_t* t = trg.data();
  const uint8_t* sdata = src.data();
  size_t pos = 0;
  const size_t last = trg.size() - kDeltaBlock;
  uint32_t h = delta_hash(t);
  while (pos <= last) {
    size_t found = SIZE_MAX;
    uint32_t at = h & ix.mask;
    for (int probe = 0; probe < 4 && ix.slots[at]; ++probe, at = (at + 1) & ix.mask) {
      const size_t off = ix.slots[at] - 1;
      if (std::memcmp(sdata + off, t + pos, kDeltaBlock) == 0) { found = off; break; }
    }
    if (found == SIZE_MAX) {
      if (pos == last) break;
      h = (h - t[pos] * ix.out_factor) * kDeltaPrime + t[pos + kDeltaBlock];
      ++pos;
      continue;
    }
    // Grown as far as the two agree, forwards and back into the bytes still waiting.
    size_t len = kDeltaBlock;
    while (found + len < src.size() && pos + len < trg.size() && sdata[found + len] == t[pos + len]) ++len;
    while (found > 0 && pos > lit && sdata[found - 1] == t[pos - 1]) { --found; --pos; ++len; }
    flush_literals(pos);
    copy(found, len);
    pos += len;
    lit = pos;
    if (out.size() > max_size) return ByteBuffer();
    if (pos > last) break;
    h = delta_hash(t + pos);
  }
  flush_literals(trg.size());
  if (out.size() > max_size) return ByteBuffer();
  return out;
}

}  // namespace

ByteBuffer git_make_delta(ByteSpan base, ByteSpan target, size_t max_size) {
  DeltaIndex ix(base);
  return make_delta(ix, target, max_size);
}

namespace {

// Deltas for the objects of `candidates` (ones not stored as a delta already): each is tried against the few before
// it in an order that puts versions of the same file side by side (by type, then file name, then size, largest first),
// and the smallest delta that at least halves it is kept. A chain is at most 50 deep. Every delta is applied back and
// compared before it is trusted.
Result<std::unordered_map<std::string, PlannedDelta>> plan_deltas(Git& repo, const std::vector<std::string>& candidates) {
  using R = Result<std::unordered_map<std::string, PlannedDelta>>;
  std::unordered_map<std::string, PlannedDelta> plan;
  if (candidates.size() < 2) return plan;

  // A name for every blob and tree, from the trees that hold them (any one of them: versions of a file share it).
  std::unordered_map<std::string, std::string> name_of;
  {
    const std::unordered_set<std::string> want(candidates.begin(), candidates.end());
    std::unordered_set<std::string> walked;
    std::vector<std::pair<std::string, std::string>> todo;   // tree id, its path
    for (auto& id : candidates) {
      auto o = repo.read_object(id);
      if (!o.ok() || o->type != "commit") continue;
      auto cm = repo.read_commit(id);
      if (cm.ok()) todo.emplace_back(cm->tree, "");
    }
    while (!todo.empty()) {
      auto [tree, dir] = todo.back();
      todo.pop_back();
      if (!walked.insert(tree).second) continue;
      auto ents = repo.read_tree(tree);
      if (!ents.ok()) continue;
      for (auto& e : *ents) {
        if (e.mode == 0160000) continue;
        const std::string path = dir.empty() ? e.name : dir + "/" + e.name;
        if (want.count(e.oid)) name_of.emplace(e.oid, path);
        if (e.is_dir()) todo.emplace_back(e.oid, path);
      }
    }
  }

  struct Item { std::string oid, type, base_name, name; uint64_t size = 0; int depth = 0; };
  std::vector<Item> items;
  items.reserve(candidates.size());
  for (auto& id : candidates) {
    auto o = repo.read_object(id);
    if (!o.ok()) return R(o.error());
    Item it;
    it.oid = id;
    it.type = o->type;
    it.size = o->payload.size();
    if (auto n = name_of.find(id); n != name_of.end()) {
      it.name = n->second;
      size_t slash = it.name.rfind('/');
      it.base_name = slash == std::string::npos ? it.name : it.name.substr(slash + 1);
    }
    items.push_back(std::move(it));
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.type != b.type) return a.type < b.type;
    if (a.base_name != b.base_name) return a.base_name < b.base_name;
    if (a.name != b.name) return a.name < b.name;
    if (a.size != b.size) return a.size > b.size;
    return a.oid < b.oid;
  });

  struct Loaded { size_t item; ByteBuffer data; std::unique_ptr<DeltaIndex> index; };
  std::vector<Loaded> window;   // the objects a later one may be a delta on, oldest first
  uint64_t window_bytes = 0;
  const size_t kWindow = 10;
  const uint64_t kWindowBytes = 256ull << 20;
  const int kMaxDepth = 50;
  for (size_t i = 0; i < items.size(); ++i) {
    Item& it = items[i];
    auto o = repo.read_object(it.oid);
    if (!o.ok()) return R(o.error());
    ByteBuffer data = std::move(o->payload);
    if (data.size() >= 64 && data.size() < 0x7ff00000ull) {
      ByteBuffer best;
      size_t best_from = SIZE_MAX;
      size_t limit = data.size() / 2 > 20 ? data.size() / 2 - 20 : 0;
      const bool big = data.size() > (1u << 20);
      int tries = 0;
      for (size_t w = window.size(); w-- > 0 && limit > 0;) {
        Loaded& cand = window[w];
        const Item& ci = items[cand.item];
        if (ci.type != it.type || ci.depth >= kMaxDepth) continue;
        if (ci.size < data.size() / 32) continue;   // far too small to hold much of it
        if (big && ++tries > 6) break;              // a large file: its nearest neighbours or nothing
        if (!cand.index) cand.index = std::make_unique<DeltaIndex>(ByteSpan(cand.data.data(), cand.data.size()));
        ByteBuffer d = make_delta(*cand.index, ByteSpan(data.data(), data.size()), limit);
        if (d.empty()) continue;
        best = std::move(d);
        best_from = w;
        limit = best.size() > 1 ? best.size() - 1 : 0;
        if (big && best.size() < data.size() / 32) break;   // good enough: another scan of a large file buys little
      }
      if (best_from != SIZE_MAX) {
        const Loaded& from = window[best_from];
        auto back = git_apply_delta(ByteSpan(from.data.data(), from.data.size()), ByteSpan(best.data(), best.size()));
        if (back.ok() && back->size() == data.size() && std::memcmp(back->data(), data.data(), data.size()) == 0) {
          PlannedDelta pl;
          pl.base_oid = items[from.item].oid;
          pl.size = best.size();
          pl.z = zlib_deflate(ByteSpan(best.data(), best.size()), 9);
          it.depth = items[from.item].depth + 1;
          plan.emplace(it.oid, std::move(pl));
        }
      }
    }
    window_bytes += data.size();
    window.push_back(Loaded{i, std::move(data), nullptr});
    while (window.size() > kWindow || (window.size() > 1 && window_bytes > kWindowBytes)) {
      window_bytes -= window.front().data.size();
      window.erase(window.begin());
    }
  }
  return plan;
}

}  // namespace

Result<GitRepackReport> git_repack(Git& repo, bool all, bool remove_old) {
  using R = Result<GitRepackReport>;
  GitRepackReport rep;
  hal::Host& host = repo.host();
  const std::vector<std::string> loose = repo.local_loose_ids();
  const std::vector<std::string> old_packs = repo.local_packs();
  std::vector<std::string> oids = loose;
  const std::vector<std::string> old_blocks = all ? repo.local_block_files() : std::vector<std::string>{};
  if (all) {
    std::unordered_set<std::string> seen(oids.begin(), oids.end());
    for (auto& id : repo.local_packed_ids()) if (seen.insert(id).second) oids.push_back(id);
    // block files go back into a pack (the only way out of them git itself reads)
    for (auto& id : repo.local_block_ids()) if (seen.insert(id).second) oids.push_back(id);
  }
  // Nothing loose and at most one pack already: it is as packed as this makes it.
  if (oids.empty() || (loose.empty() && old_blocks.empty() && (!all || old_packs.size() <= 1))) return rep;
  std::sort(oids.begin(), oids.end());
  // The objects not stored as a delta yet (every loose one, and a packed one kept whole) are the ones worth trying.
  std::vector<std::string> whole;
  for (auto& id : oids) {
    auto e = repo.packed_entry(id);
    if (!e.ok() || e->type != 7) whole.push_back(id);
  }
  auto plan = plan_deltas(repo, whole);
  if (!plan.ok()) return R(plan.error());
  uint64_t counted = 0;
  auto pack = build_pack(repo, oids, counted, &*plan, 6);
  if (!pack.ok()) return R(pack.error());
  std::string base;
  if (auto k = keep_pack(repo, host, ByteSpan(pack->data(), pack->size() - 20), (uint32_t)oids.size(), nullptr, &base); !k.ok())
    return R(k.error());
  ByteBuffer().swap(*pack);
  rep.objects = oids.size();
  rep.pack = base;
  if (!remove_old) return rep;
  // The new pack and its index are on disk: only now does anything go.
  if (all)
    for (auto& p : old_packs) {
      if (p == base || host.exists(p + ".keep")) continue;
      // The index first: a reader finds a pack by its index, so it never meets an index whose pack has gone.
      for (const char* ext : {".idx", ".pack", ".rev", ".bitmap", ".mtimes"})
        if (host.exists(p + ext)) (void)host.remove(p + ext);
      ++rep.packs_removed;
    }
  for (auto& b : old_blocks) if (host.remove(b).ok()) ++rep.packs_removed;
  const std::string objects = repo.common_dir() + "/objects/";
  for (auto& id : loose)
    if (host.remove(objects + id.substr(0, 2) + "/" + id.substr(2)).ok()) ++rep.loose_removed;
  for (auto& id : loose) (void)host.remove(objects + id.substr(0, 2));   // a fan-out directory left empty (refused when it is not)
  repo.rescan_packs();
  return rep;
}

// Index a packfile into the object database: every whole object written as it is, every delta
// (OFS and REF, including REF deltas whose base comes later) rebuilt on its base first.
Result<void> git_index_pack(Git& repo, ByteSpan pack_span, uint64_t* objects_out, uint64_t* deltas_out, uint64_t* written_out) {
  GitBulkWrite bulk(repo);   // one flush for the whole pack's objects, not one per object
  const ByteSpan pack = pack_span;   // read in place: a copy of a pack is as big as the pack
  uint64_t objects = 0, deltas = 0, written = 0;
  if (pack.size() < 32) return Result<void>(Status::Corrupt, "a packfile too short to be one");

  // --- index the pack ---
  if (std::memcmp(pack.data(), "PACK", 4) != 0)
    return Result<void>(Status::Corrupt, "the pack does not start with PACK");
  {
    // The trailer is the SHA-1 of everything before it: check it before trusting a byte.
    auto sum = crypto::sha1(ByteSpan(pack.data(), pack.size() - 20));
    if (std::memcmp(sum.data(), pack.data() + pack.size() - 20, 20) != 0)
      return Result<void>(Status::Corrupt,
                                   "the packfile's own checksum does not match its contents — the transfer is damaged");
  }
  PackCursor pc{ByteSpan(pack.data(), pack.size() - 20)};
  pc.pos = 4;
  uint32_t version = pc.be32();
  uint32_t count = pc.be32();
  if (version != 2 && version != 3)
    return Result<void>(Status::Unsupported, "packfile version " + dec(version));
  objects = count;

  if (count >= kKeepPackAt) {
    // A thin pack is kept too, with the bases it leans on appended from this repository.
    uint32_t appended = 0;
    auto kept = keep_pack(repo, repo.host(), ByteSpan(pack.data(), pack.size() - 20), count, &deltas, nullptr,
                          /*fix_thin=*/true, &appended);
    if (kept.ok()) {
      if (objects_out) *objects_out = objects;
      if (deltas_out) *deltas_out = deltas;
      if (written_out) *written_out = objects + appended;
      return Ok();
    }
    if (kept.status() != Status::Unsupported) return kept;
    deltas = 0;   // a base in neither the pack nor this repository: unpacked below, which names what is missing
  }

  struct Cached { std::string type; ByteBuffer payload; std::list<size_t>::iterator use; };
  std::unordered_map<size_t, std::string> oid_at;       // pack offset -> object id
  std::unordered_map<size_t, Cached> cache;             // recently used objects, by offset
  std::list<size_t> cache_use;                          // their offsets, the most recently used first
  uint64_t cache_bytes = 0;
  // Objects kept so the deltas on them need no re-read: a bounded amount, and nothing big (a big one is read
  // back from the object database if a delta names it, which is rare). Least recently used goes first, not
  // oldest: a base many deltas lean on (a tree, a much-edited file) stays while it is still being used.
  const uint64_t kCacheCap = 64ull << 20, kCacheMaxObject = 8ull << 20;
  auto remember = [&](size_t off, const std::string& type, ByteBuffer&& payload) {
    if (payload.size() > kCacheMaxObject) return;
    cache_use.push_front(off);
    cache_bytes += payload.size();
    cache[off] = Cached{type, std::move(payload), cache_use.begin()};
    while (cache_bytes > kCacheCap && cache_use.size() > 1) {
      auto it = cache.find(cache_use.back());
      cache_use.pop_back();
      if (it != cache.end()) { cache_bytes -= it->second.payload.size(); cache.erase(it); }
    }
  };

  // A REF_DELTA can name a base that appears later in the pack; those wait.
  struct Pending { size_t offset; std::string base_oid; ByteBuffer delta; };
  std::vector<Pending> pending;

  auto load = [&](const std::string& oid, std::string& type, ByteBuffer& payload) -> bool {
    auto o = repo.read_object(oid);
    if (!o.ok()) return false;
    type = o->type;
    payload = o->payload;
    return true;
  };

  for (uint32_t i = 0; i < count; ++i) {
    size_t obj_off = pc.pos;
    uint8_t c = pc.u8();
    int type = (c >> 4) & 7;
    uint64_t size = c & 15;
    int shift = 4;
    while (c & 0x80) {
      c = pc.u8();
      size |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
      if (pc.bad) break;
    }
    if (pc.bad) return Result<void>(Status::Corrupt, "the pack ends inside an object header");

    size_t base_off = 0;
    std::string base_oid;
    if (type == 6) {
      uint8_t b = pc.u8();
      uint64_t ofs = b & 0x7f;
      while (b & 0x80) {
        b = pc.u8();
        ofs = ((ofs + 1) << 7) | (b & 0x7f);
        if (pc.bad) break;
      }
      if (ofs > obj_off) return Result<void>(Status::Corrupt, "an offset delta points before the pack");
      base_off = obj_off - (size_t)ofs;
    } else if (type == 7) {
      if (pc.pos + 20 > pc.d.size()) return Result<void>(Status::Corrupt, "the pack ends inside a delta base id");
      base_oid = hex20(pc.d.data() + pc.pos);
      pc.pos += 20;
    }

    size_t end = 0;
    auto raw = zlib_inflate_at(pc.d, pc.pos, &end, (size_t)std::min<uint64_t>(size, (uint64_t)256 << 20));   // the header says how big
    if (!raw.ok()) return Result<void>(raw.error());
    pc.pos = end;
    if (type != 6 && type != 7 && raw->size() != size)
      return Result<void>(Status::Corrupt,
                                   "object " + dec(i) + " says " + dec(size) + " bytes, inflated to " + dec(raw->size()));

    if (type >= 1 && type <= 4) {
      auto oid = repo.write_object(type_name(type), ByteSpan(raw->data(), raw->size()));
      if (!oid.ok()) return Result<void>(oid.error());
      oid_at[obj_off] = *oid;
      remember(obj_off, type_name(type), std::move(*raw));
      ++written;
      continue;
    }
    ++deltas;

    // A base found in the cache is used where it lies (and marked as just used); only one read back from the
    // database is copied out.
    std::string base_type;
    ByteBuffer loaded;
    ByteSpan base;
    if (type == 6) {
      auto it = cache.find(base_off);
      if (it != cache.end()) {
        cache_use.splice(cache_use.begin(), cache_use, it->second.use);
        base_type = it->second.type;
        base = ByteSpan(it->second.payload.data(), it->second.payload.size());
      } else {
        auto oit = oid_at.find(base_off);
        if (oit == oid_at.end() || !load(oit->second, base_type, loaded))
          return Result<void>(Status::Corrupt, "an offset delta names a base this pack never had");
        base = ByteSpan(loaded.data(), loaded.size());
      }
    } else {
      if (!load(base_oid, base_type, loaded)) { pending.push_back(Pending{obj_off, base_oid, std::move(*raw)}); continue; }
      base = ByteSpan(loaded.data(), loaded.size());
    }

    auto full = apply_delta(base, ByteSpan(raw->data(), raw->size()));
    if (!full.ok()) return Result<void>(full.error());
    auto oid = repo.write_object(base_type, ByteSpan(full->data(), full->size()));
    if (!oid.ok()) return Result<void>(oid.error());
    oid_at[obj_off] = *oid;
    remember(obj_off, base_type, std::move(*full));   // (base is not used after this: the cache may drop it)
    ++written;
  }

  // Deltas whose base arrived later: resolve until nothing moves.
  while (!pending.empty()) {
    size_t before = pending.size();
    for (size_t i = 0; i < pending.size();) {
      std::string base_type;
      ByteBuffer base_payload;
      if (!load(pending[i].base_oid, base_type, base_payload)) { ++i; continue; }
      auto full = apply_delta(ByteSpan(base_payload.data(), base_payload.size()),
                              ByteSpan(pending[i].delta.data(), pending[i].delta.size()));
      if (!full.ok()) return Result<void>(full.error());
      auto oid = repo.write_object(base_type, ByteSpan(full->data(), full->size()));
      if (!oid.ok()) return Result<void>(oid.error());
      oid_at[pending[i].offset] = *oid;
      ++written;
      pending.erase(pending.begin() + (long)i);
    }
    if (pending.size() == before)
      return Result<void>(Status::Corrupt,
                                   dec(pending.size()) + " delta(s) name a base object the remote never sent");
  }

  if (objects_out) *objects_out = objects;
  if (deltas_out) *deltas_out = deltas;
  if (written_out) *written_out = written;
  return Ok();
}

Result<GitFetchReport> GitRemote::fetch(Git& repo, const std::string& url, const GitRemoteOptions& opt) {
  if (auto r = may_network(); !r.ok()) return Result<GitFetchReport>(r.error());
  auto u = git_parse_url(url);
  if (!u.ok()) return Result<GitFetchReport>(u.error());
  // Over smart HTTP protocol v2 is asked for; a server that answers v0 is spoken to in v0, as before.
  auto cvr = open_service(kernel_, *u, opt, "git-upload-pack", /*want_v2=*/true);
  if (!cvr.ok()) return Result<GitFetchReport>(cvr.error());
  Conversation& cv = *cvr;
  GitFetchReport& rep = cv.report;
  rep.url = url;
  Wire& w = *cv.wire;
  if (auto r = read_advertisement(w, rep, opt.timeout_ms, &cv); !r.ok()) return Result<GitFetchReport>(r.error());
  if (cv.v2) {
    // v2 lists only the refs asked for. A caller that chooses for itself (refspecs) may want any of them; a
    // fetch of one ref needs only that name's spellings; otherwise the branches and tags. HEAD always, for
    // the default branch.
    std::vector<std::string> prefixes;
    if (!opt.select) {
      prefixes.push_back("HEAD");
      if (!opt.ref.empty()) {
        for (const std::string& p : {opt.ref, "refs/heads/" + opt.ref, "refs/tags/" + opt.ref}) prefixes.push_back(p);
      } else {
        prefixes.push_back("refs/heads/");
        prefixes.push_back("refs/tags/");
      }
    }
    if (auto r = v2_ls_refs(cv, prefixes, opt.timeout_ms); !r.ok()) return Result<GitFetchReport>(r.error());
  } else if (cv.http) {
    cv.http->speak_v0();
  }

  // This repository's shallow commits (.git/shallow): the remote is told them, so it knows where
  // this history stops, and a deepening fetch may move them.
  const std::string shallow_file = repo.common_dir() + "/shallow";
  std::vector<std::string> had_shallow;
  if (kernel_.host().exists(shallow_file)) {
    auto f = kernel_.host().read_file(shallow_file);
    if (!f.ok()) return Result<GitFetchReport>(f.error());
    std::string text((const char*)f->data(), f->size());
    for (size_t at = 0; at < text.size();) {
      size_t nl = text.find('\n', at);
      std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      if (line.size() >= 40) had_shallow.push_back(line.substr(0, 40));
      at = nl == std::string::npos ? text.size() : nl + 1;
    }
  }
  const bool shallow = opt.depth > 0;   // a depth was asked for: the remote answers with a shallow list

  // What to ask for.
  std::vector<GitRemoteRef> wants;
  if (opt.select) {
    // The caller decides (refspecs). Objects already here are not asked for again (unless a depth
    // means history below them is wanted); when that leaves nothing, the conversation ends without
    // a pack.
    std::unordered_set<std::string> asked;
    for (auto& oid : opt.select(rep.refs)) {
      if (oid.size() != 40 || !asked.insert(oid).second) continue;
      if (!shallow && repo.has_object(oid)) continue;
      wants.push_back(GitRemoteRef{"", oid});
    }
    if (wants.empty()) {
      ByteBuffer bye;
      write_flush(bye);
      w.send(ByteSpan(bye.data(), bye.size()));
      return rep;
    }
  } else {
    for (auto& r : rep.refs) {
      if (r.name.size() > 3 && r.name.compare(r.name.size() - 3, 3, "^{}") == 0) continue;   // a peeled tag
      if (!opt.ref.empty()) {
        if (r.name == opt.ref || r.name == "refs/heads/" + opt.ref || r.name == "refs/tags/" + opt.ref) wants.push_back(r);
        continue;
      }
      if (r.name.rfind("refs/heads/", 0) == 0 || r.name.rfind("refs/tags/", 0) == 0) wants.push_back(r);
    }
  }
  if (wants.empty() && opt.ref.empty() && rep.refs.empty()) {
    // An empty remote: nothing to fetch, which is not an error (stock git's
    // clone and fetch of one succeed). A clone stands on the branch the
    // remote's HEAD names, where its first push will go.
    if (opt.advance_branches && rep.default_branch.rfind("refs/heads/", 0) == 0 && repo.branch().empty() == false &&
        "refs/heads/" + repo.branch() != rep.default_branch) {
      if (auto r = repo.set_head(rep.default_branch); !r.ok()) return Result<GitFetchReport>(r.error());
    }
    return rep;
  }
  if (wants.empty())
    return Result<GitFetchReport>(Status::NotFound,
                                 opt.ref.empty() ? "the remote advertised no branches or tags"
                                                 : "the remote has no ref " + opt.ref);

  // What this machine already has, so the remote sends only the difference.
  // Without these every fetch is a fresh clone; with them an unchanged remote
  // sends a pack with nothing in it. Both protocols say it the same way.
  auto collect_haves = [&](ByteBuffer& haves) -> size_t {
    size_t have_count = 0;
    if (opt.no_haves) return have_count;
    std::vector<std::string> seen;
    auto add = [&](const std::string& oid) {
      if (oid.size() != 40) return;
      for (auto& s2 : seen) if (s2 == oid) return;
      seen.push_back(oid);
      write_pkt(haves, "have " + oid + "\n");
      ++have_count;
    };
    // A wanted commit that is already in this repository's reach (through an alternate store on the same machine,
    // say) is said to be had: the remote then sends nothing for it, where it used to send its whole history again.
    if (!shallow)
      for (auto& r : wants) if (repo.has_object(r.oid)) add(r.oid);
    auto local = repo.refs();
    if (local.ok())
      for (auto& r : *local) add(r.oid);
    // A few generations back as well, so a diverged branch still finds a base.
    auto history = repo.log(32);
    if (history.ok())
      for (auto& c : *history) add(c.oid);
    return have_count;
  };

  ByteBuffer pack;
  std::vector<std::string> shallow_boundary;
  std::unordered_set<std::string> unshallowed;
  if (cv.v2) {
    rep.protocol = 2;
    if (auto r = fetch_pack_v2(cv, opt, wants, had_shallow, collect_haves, pack, shallow_boundary, unshallowed); !r.ok())
      return Result<GitFetchReport>(r.error());
  } else {
    // thin-pack: a smaller pack, whose deltas may lean on objects this repository has (index-pack appends them).
    const std::string caps_wanted[] = {"side-band-64k", "ofs-delta", "thin-pack"};
    std::string caps;
    for (auto& c : caps_wanted)
      if (rep.server_capabilities.find(c) != std::string::npos) caps += (caps.empty() ? "" : " ") + c;
    if (opt.include_tag && rep.server_capabilities.find("include-tag") != std::string::npos) caps += " include-tag";
    if (rep.server_capabilities.find("side-band-64k") == std::string::npos)
      return Result<GitFetchReport>(Status::Unsupported,
                                   "the remote does not offer side-band-64k, which is how this machine reads the pack "
                                   "in framed pieces");
    if ((shallow || !had_shallow.empty()) && rep.server_capabilities.find("shallow") == std::string::npos)
      return Result<GitFetchReport>(Status::Unsupported, "the remote will not truncate history (no shallow capability)");
    if (opt.deepen_relative) {
      if (rep.server_capabilities.find("deepen-relative") == std::string::npos)
        return Result<GitFetchReport>(Status::Unsupported, "the remote does not support --deepen");
      caps += " deepen-relative";
    }
    caps += " aios/1";

    ByteBuffer req;
    bool firstw = true;
    for (auto& r : wants) {
      write_pkt(req, "want " + r.oid + (firstw ? " " + caps : "") + "\n");
      firstw = false;
    }
    for (auto& s : had_shallow) write_pkt(req, "shallow " + s + "\n");
    if (shallow) write_pkt(req, "deepen " + dec((uint64_t)opt.depth) + "\n");
    if (!opt.filter.empty()) {
      if ((" " + rep.server_capabilities + " ").find(" filter ") == std::string::npos)
        return Result<GitFetchReport>(Status::Unsupported, "the remote does not filter objects (no filter capability)");
      write_pkt(req, "filter " + opt.filter + "\n");
    }
    write_flush(req);
    if (auto r = w.send(ByteSpan(req.data(), req.size())); !r.ok()) return Result<GitFetchReport>(r.error());

    // shallow/unshallow lines, terminated by a flush. These are the commits
    // whose parents this machine deliberately does not have; recording them is
    // what makes the truncated history legal rather than broken. Over smart
    // HTTP they come back at the head of the one answer to the whole request,
    // so they are read after the haves go out, not before.
    auto read_shallow = [&]() -> Result<void> {
      for (;;) {
        auto p = read_pkt(w, opt.timeout_ms);
        if (!p.ok()) return Result<void>(p.error());
        if (p->kind == Pkt::Flush) break;
        std::string line = trim_nl(p->payload);
        if (line.rfind("shallow ", 0) == 0) shallow_boundary.push_back(line.substr(8, 40));
        else if (line.rfind("unshallow ", 0) == 0) unshallowed.insert(line.substr(10, 40));
      }
      return Ok();
    };
    if (shallow && !cv.stateless) {
      if (auto r = read_shallow(); !r.ok()) return Result<GitFetchReport>(r.error());
    }

    // What this machine already has, so the remote sends only the difference.
    ByteBuffer haves;
    rep.haves = collect_haves(haves);
    write_pkt(haves, "done\n");
    if (auto r = w.send(ByteSpan(haves.data(), haves.size())); !r.ok()) return Result<GitFetchReport>(r.error());
    if (shallow && cv.stateless) {
      if (auto r = read_shallow(); !r.ok()) return Result<GitFetchReport>(r.error());
    }

    // The negotiation's answer (ACK/NAK lines), then the pack inside side-band
    // frames. A side-band frame's first byte is 1, 2 or 3; anything larger is
    // still text from the negotiation.
    std::string progress;
    for (;;) {
      auto p = read_pkt(w, opt.timeout_ms);
      if (!p.ok()) return Result<GitFetchReport>(p.error());
      if (p->kind == Pkt::Flush) break;
      if (p->payload.empty()) continue;
      if (p->payload[0] > 3) {
        std::string line = trim_nl(p->payload);
        if (line.rfind("ACK", 0) == 0) ++rep.acks;
        continue;
      }
      uint8_t band = p->payload[0];
      if (band == 1) {
        const bool first_frame = pack.empty();
        pack.insert(pack.end(), p->payload.begin() + 1, p->payload.end());
        if (first_frame) {
          auto all = w.take_sideband(pack, progress);
          if (!all.ok()) return Result<GitFetchReport>(all.error());
          if (*all) break;   // the flush that ends the frames is read too
          pack.reserve(pack.size() + w.buffered());   // what the answer holds: no doubling, no copy of a copy
        }
      }
      else if (band == 2) progress.append((const char*)p->payload.data() + 1, p->payload.size() - 1);
      else if (band == 3)
        return Result<GitFetchReport>(Status::IoError,
                                     "the remote failed: " + std::string((const char*)p->payload.data() + 1, p->payload.size() - 1));
    }
    w.send_eof();
  }
  rep.pack_bytes = pack.size();
  if (pack.empty()) {
    // Nothing to send: this machine already had everything the remote holds.
    // Still land the refs below, in case a ref moved to an object we have.
    rep.objects = 0;
  } else if (pack.size() < 32) {
    return Result<GitFetchReport>(Status::Corrupt, "the remote sent a packfile too short to be one");
  }
  if (!pack.empty()) {
    if (auto r = git_index_pack(repo, ByteSpan(pack.data(), pack.size()), &rep.objects, &rep.deltas, &rep.written); !r.ok())
      return Result<GitFetchReport>(r.error());
  }

  // The new shallow list: what was shallow and was not lifted, and the new edges, sorted as git
  // keeps them. None left (an --unshallow) removes the file.
  if (shallow) {
    std::set<std::string> now;
    for (auto& s : had_shallow) if (!unshallowed.count(s)) now.insert(s);
    for (auto& s : shallow_boundary) now.insert(s);
    if (now.empty()) {
      if (!had_shallow.empty())
        if (auto e = kernel_.host().remove(shallow_file); !e.ok()) return Result<GitFetchReport>(e.error());
    } else {
      std::string text;
      for (auto& oid : now) text += oid + "\n";
      if (auto e = kernel_.host().write_file(shallow_file, span_of(text)); !e.ok()) return Result<GitFetchReport>(e.error());
    }
    rep.shallow = now.size();
  }
  if (opt.select) return rep;      // the caller lands the refs it asked for

  // --- land the refs ---
  // A fetch records what the REMOTE has, under refs/remotes/<name>/*. Local
  // branches are this machine's own and are only moved on the caller's say-so
  // (a clone, which has none yet, or a pull, which asked to advance).
  for (auto& r : wants) {
    // Ref names come from the remote. One we could not safely store is skipped, not fatal.
    if (!Git::valid_ref_name(r.name)) continue;
    if (r.name.rfind("refs/heads/", 0) == 0) {
      std::string remote_ref = "refs/remotes/origin/" + r.name.substr(11);
      if (auto e = repo.update_ref(remote_ref, r.oid); !e.ok()) return Result<GitFetchReport>(e.error());
    } else if (r.name.rfind("refs/tags/", 0) == 0) {
      if (auto e = repo.update_ref(r.name, r.oid); !e.ok()) return Result<GitFetchReport>(e.error());
    }
  }

  std::string head_ref = rep.default_branch;
  if (head_ref.empty()) {
    for (const char* candidate : {"refs/heads/main", "refs/heads/master"}) {
      for (auto& r : wants)
        if (r.name == candidate) head_ref = candidate;
      if (!head_ref.empty()) break;
    }
  }
  if (head_ref.empty()) head_ref = wants.front().name;
  // The branch this repository is standing on decides what a pull advances.
  if (!repo.branch().empty()) {
    std::string on = "refs/heads/" + repo.branch();
    for (auto& r : wants)
      if (r.name == on) head_ref = on;
  }
  for (auto& r : wants)
    if (r.name == head_ref) rep.head_oid = r.oid;
  rep.default_branch = head_ref;
  if (rep.head_oid.empty() || head_ref.rfind("refs/heads/", 0) != 0) return rep;

  auto before = repo.ref_oid(head_ref);
  if (before.ok()) rep.local_before = *before;

  if (!opt.advance_branches) return rep;      // a plain fetch stops here

  if (before.ok() && *before != rep.head_oid) {
    // Only a fast-forward: the branch may move to a commit that already
    // contains where it was. Anything else is two histories, and merging them
    // is not something this machine can do yet — it says so rather than
    // pretending.
    // "Contains" means reachable through ANY parent. This used to follow
    // first parents only, so a local head that the remote had merged in (it
    // sits behind the merge's second parent) was called diverged.
    bool fast_forward = false;
    std::vector<std::string> todo{rep.head_oid};
    std::unordered_set<std::string> seen;
    while (!todo.empty() && seen.size() < 1000000) {
      std::string at = todo.back();
      todo.pop_back();
      if (at.empty() || !seen.insert(at).second) continue;
      if (at == *before) { fast_forward = true; break; }
      auto c = repo.read_commit(at);
      if (!c.ok()) continue;       // the edge of a shallow history
      for (auto& p : c->parents) todo.push_back(p);
    }
    if (!fast_forward) {
      rep.diverged = true;
      return rep;
    }
  }

  if (auto e = repo.update_ref(head_ref, rep.head_oid); !e.ok()) return Result<GitFetchReport>(e.error());
  if (auto e = repo.set_head(head_ref); !e.ok()) return Result<GitFetchReport>(e.error());
  if (opt.checkout) {
    auto n = repo.checkout(rep.head_oid);
    if (!n.ok()) return Result<GitFetchReport>(n.error());
    rep.files = *n;
  }
  return rep;
}

// ---------------------------------------------------------------------------
// push — the other direction
// ---------------------------------------------------------------------------
namespace {

// Every object reachable from `commit`, stopping wherever `stop` already holds
// the id. Used twice: to work out what the remote already has, and to work out
// what this machine must therefore send.
Result<void> walk_reachable(const Git& repo, const std::string& commit,
                            const std::unordered_set<std::string>& stop,
                            std::unordered_set<std::string>& seen,
                            std::vector<std::string>* order) {
  std::vector<std::string> commits{commit};
  std::vector<std::string> trees;
  while (!commits.empty()) {
    std::string oid = commits.back();
    commits.pop_back();
    if (oid.empty() || stop.count(oid) || seen.count(oid)) continue;
    auto c = repo.read_commit(oid);
    if (!c.ok()) {
      // A commit that is not in this database is the edge of a shallow history:
      // not an error, simply as far back as this machine can go.
      if (c.status() == Status::NotFound) continue;
      return Result<void>(c.error());
    }
    seen.insert(oid);
    if (order) order->push_back(oid);
    trees.push_back(c->tree);
    for (auto& p : c->parents) commits.push_back(p);
  }
  while (!trees.empty()) {
    std::string oid = trees.back();
    trees.pop_back();
    if (oid.empty() || stop.count(oid) || seen.count(oid)) continue;
    auto ents = repo.read_tree(oid);
    if (!ents.ok()) {
      if (ents.status() == Status::NotFound) continue;
      return Result<void>(ents.error());
    }
    seen.insert(oid);
    if (order) order->push_back(oid);
    for (auto& e : *ents) {
      if (e.mode == 0160000) continue;             // a gitlink names another repository's commit
      if (stop.count(e.oid) || seen.count(e.oid)) continue;
      if (e.is_dir()) { trees.push_back(e.oid); continue; }
      seen.insert(e.oid);
      if (order) order->push_back(e.oid);
    }
  }
  return Ok();
}

// The objects one commit stands on: itself, its tree, every tree and blob
// under it - no parents. What the remote surely has when its ref is at
// `commit`, read without walking its whole history.
Result<void> commit_tree_objects(const Git& repo, const std::string& commit, std::unordered_set<std::string>& seen) {
  auto c = repo.read_commit(commit);
  if (!c.ok()) {
    if (c.status() == Status::NotFound) return Ok();
    return Result<void>(c.error());
  }
  seen.insert(commit);
  std::vector<std::string> trees{c->tree};
  while (!trees.empty()) {
    std::string oid = trees.back();
    trees.pop_back();
    if (oid.empty() || seen.count(oid)) continue;
    auto ents = repo.read_tree(oid);
    if (!ents.ok()) {
      if (ents.status() == Status::NotFound) continue;
      return Result<void>(ents.error());
    }
    seen.insert(oid);
    for (auto& e : *ents) {
      if (e.mode == 0160000) continue;
      if (seen.count(e.oid)) continue;
      if (e.is_dir()) trees.push_back(e.oid); else seen.insert(e.oid);
    }
  }
  return Ok();
}

// Whether `target` is `from` or one of its ancestors: commits only, no trees.
Result<bool> commit_reaches(const Git& repo, const std::string& from, const std::string& target) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> todo{from};
  while (!todo.empty()) {
    std::string oid = todo.back();
    todo.pop_back();
    if (oid.empty() || !seen.insert(oid).second) continue;
    if (oid == target) return true;
    auto c = repo.read_commit(oid);
    if (!c.ok()) {
      if (c.status() == Status::NotFound) continue;
      return Result<bool>(c.error());
    }
    for (auto& p : c->parents) todo.push_back(p);
  }
  return false;
}

uint8_t pack_type_of(const std::string& type) {
  if (type == "commit") return 1;
  if (type == "tree") return 2;
  if (type == "blob") return 3;
  if (type == "tag") return 4;
  return 0;
}

// A version-2 packfile: header, each object, and the SHA-1 of everything as the trailer. An object this repository
// holds in a pack goes out as it is stored, its deflated bytes copied and never inflated: whole, or as the delta it
// is when its base is in this pack too (named by id, which a pack allows wherever the base sits). Anything else (a
// loose object, a delta whose base is not being sent) is read and deflated here. Sending a packed repository is then
// about as much work as copying it, and as many bytes: inflating and recompressing every object made the pack three
// times the repository's size and took over a minute for converge itself.
Result<ByteBuffer> build_pack(Git& repo, const std::vector<std::string>& oids, uint64_t& counted,
                              const std::unordered_map<std::string, PlannedDelta>* planned, int level) {
  ByteBuffer pack;
  const char* magic = "PACK";
  pack.insert(pack.end(), magic, magic + 4);
  auto be32 = [&](uint32_t v) {
    pack.push_back((uint8_t)(v >> 24)); pack.push_back((uint8_t)(v >> 16));
    pack.push_back((uint8_t)(v >> 8));  pack.push_back((uint8_t)v);
  };
  be32(2);
  be32((uint32_t)oids.size());
  counted = 0;
  auto header = [&](uint8_t t, uint64_t size) {
    uint8_t first = (uint8_t)((t << 4) | (size & 0x0f));
    size >>= 4;
    if (size) first |= 0x80;
    pack.push_back(first);
    while (size) {
      uint8_t b = (uint8_t)(size & 0x7f);
      size >>= 7;
      if (size) b |= 0x80;
      pack.push_back(b);
    }
  };
  const std::unordered_set<std::string> sending(oids.begin(), oids.end());
  {
    // Sized once from what will be copied, so a pack of hundreds of megabytes is not grown by doubling.
    size_t expect = 32;
    for (auto& oid : oids)
      if (auto e = repo.packed_entry(oid); e.ok()) expect += e->data.size() + 32;
    pack.reserve(expect);
  }
  // The objects that have to be compressed here (loose ones, and deltas whose base is not being sent) are done first,
  // on every core: each is independent of the others, and compressing is nearly all the work of packing loose objects.
  struct Whole { uint8_t type = 0; uint64_t size = 0; ByteBuffer z; };
  std::unordered_map<std::string, Whole> whole;
  {
    std::vector<std::string> todo;
    for (auto& oid : oids) {
      if (planned && planned->count(oid)) continue;
      auto e = repo.packed_entry(oid);
      if (e.ok() && (e->type != 7 || sending.count(e->base_oid))) continue;
      if (!e.ok() && e.status() != Status::NotFound) return Result<ByteBuffer>(e.error());
      todo.push_back(oid);
    }
    whole.reserve(todo.size() * 2);
    for (auto& oid : todo) whole.emplace(oid, Whole{});   // every slot exists before a thread touches the map
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    hal::Mutex mu, read_mu;
    Result<ByteBuffer> first_error = ByteBuffer();
    auto work = [&]() {
      for (;;) {
        const size_t at = next.fetch_add(1);
        if (at >= todo.size() || failed.load()) return;
        // Reading is one at a time (the repository fills its caches as it reads); the compressing is what runs apart.
        auto o = [&]() { hal::LockGuard g(read_mu); return repo.read_object(todo[at]); }();
        const uint8_t t = o.ok() ? pack_type_of(o->type) : 0;
        if (!o.ok() || !t) {
          hal::LockGuard g(mu);
          if (!failed.exchange(true))
            first_error = o.ok() ? Result<ByteBuffer>(Status::Invalid, todo[at] + " is a " + o->type + ", which has no packfile type") : Result<ByteBuffer>(o.error());
          return;
        }
        Whole& w = whole.find(todo[at])->second;
        w.type = t;
        w.size = o->payload.size();
        w.z = zlib_deflate(ByteSpan(o->payload.data(), o->payload.size()), level);
      }
    };
#if defined(__wasi__)
    work();
#else
    {
      unsigned n = hal::hardware_threads();
      n = n == 0 ? 1 : n > 8 ? 8 : n;
      if (todo.size() < 8) n = 1;
      std::vector<hal::Thread> pool;
      for (unsigned i = 1; i < n; ++i) pool.emplace_back(work);
      work();
      for (auto& t : pool) t.join();
    }
#endif
    if (failed.load()) return first_error;
  }
  auto raw_id = [&](const std::string& hex) {
    uint8_t raw[20];
    for (int i = 0; i < 20; ++i) raw[i] = (uint8_t)((hexval(hex[(size_t)i * 2]) << 4) | hexval(hex[(size_t)i * 2 + 1]));
    pack.insert(pack.end(), raw, raw + 20);
  };
  for (auto& oid : oids) {
    if (planned) {
      if (auto pl = planned->find(oid); pl != planned->end()) {
        header(7, pl->second.size);
        raw_id(pl->second.base_oid);
        pack.insert(pack.end(), pl->second.z.begin(), pl->second.z.end());
        ++counted;
        continue;
      }
    }
    if (auto e = repo.packed_entry(oid); e.ok() && (e->type != 7 || sending.count(e->base_oid))) {
      header((uint8_t)e->type, e->size);
      if (e->type == 7) raw_id(e->base_oid);
      pack.insert(pack.end(), e->data.begin(), e->data.end());
      ++counted;
      continue;
    } else if (!e.ok() && e.status() != Status::NotFound) {
      return Result<ByteBuffer>(e.error());
    }
    auto w = whole.find(oid);
    if (w == whole.end()) return Result<ByteBuffer>(Status::NotFound, "object " + oid + " could not be read");
    header(w->second.type, w->second.size);
    pack.insert(pack.end(), w->second.z.begin(), w->second.z.end());
    ByteBuffer().swap(w->second.z);
    ++counted;
  }
  auto sum = crypto::sha1(ByteSpan(pack.data(), pack.size()));
  pack.insert(pack.end(), sum.begin(), sum.end());
  return pack;
}

const char* kZeroOid = "0000000000000000000000000000000000000000";

}  // namespace

// Packs on their way somewhere (served to a fetch, sent by a push) are compressed at a fast level: they are built while
// the other side waits, and the repository's own repack (level 6) is what keeps a store small.
constexpr int kTransportPackLevel = 3;

Result<ByteBuffer> git_build_pack(Git& repo, const std::vector<std::string>& oids, uint64_t& counted) {
  return build_pack(repo, oids, counted, nullptr, kTransportPackLevel);
}

namespace {
// An annotated tag points at its object: the tag itself is sent, and so is what it names.
std::string peel_tags(const Git& repo, const std::string& oid, std::vector<std::string>* tags) {
  std::string at = oid;
  for (int depth = 0; depth < 16; ++depth) {
    auto o = repo.read_object(at);
    if (!o.ok() || o->type != "tag") return at;
    if (tags) tags->push_back(at);
    std::string body((const char*)o->payload.data(), o->payload.size());
    if (body.rfind("object ", 0) != 0 || body.size() < 47) return at;
    at = body.substr(7, 40);
  }
  return at;
}
}  // namespace

Result<std::vector<std::string>> git_objects_between(const Git& repo, const std::vector<std::string>& tips,
                                                     const std::vector<std::string>& stop_tips) {
  // What the other side has: the commits behind each of its tips (a walk of
  // commits only, where the walk from our tips then stops) and the objects
  // of each tip's own tree. Every tree of every historical commit was read
  // before - 300 trees of 3,000 entries inflated for a push of one commit,
  // 340 ms where git takes 40. An object only an older commit holds is sent
  // again, which the other side takes without harm.
  // What the other side has, found alongside the walk from our tips rather
  // than ahead of it: its tips' ancestry used to be read whole (every
  // commit of a 300-commit history, 32 branches deep, for a push of one
  // commit); now the two walks advance newest-commit-first together, and the
  // remote side is expanded only as far as our walk needs it to know where
  // to stop. A date skew (a parent newer than its child) can let our walk
  // past a commit the other side has: the objects are sent again, which it
  // takes without harm. A day of slop covers the ordinary skew.
  static const bool trace = std::getenv("AIOS_GIT_TRACE") != nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
  std::unordered_set<std::string> stop;
  std::vector<std::string> stop_commits;   // the other side's commit tips, peeled
  struct Dated { uint64_t when; std::string oid; bool operator<(const Dated& o) const { return when < o.when; } };
  std::priority_queue<Dated> remote_q;
  for (auto& s : stop_tips) {
    if (s.size() != 40 || !repo.has_object(s)) continue;
    std::string c = peel_tags(repo, s, nullptr);
    auto cm = repo.read_commit(c);
    if (cm.ok()) {
      if (stop.insert(c).second) remote_q.push({cm->committer_time, c});
      stop_commits.push_back(c);
    } else {
      stop.insert(c);
    }
  }
  if (trace) std::fprintf(stderr, "[git] objects_between: %zu tips read at %.1f ms\n", stop_commits.size(), ms());
  std::unordered_set<std::string> reached;
  {
    constexpr uint64_t kSlop = 0;   // a parent newer than its child: our walk passes it, its objects go again, harmlessly
    std::priority_queue<Dated> our_q;
    std::unordered_set<std::string> visited;
    for (auto& t : tips) {
      if (t.size() != 40) continue;
      std::string c = peel_tags(repo, t, nullptr);
      auto cm = repo.read_commit(c);
      if (cm.ok() && visited.insert(c).second) our_q.push({cm->committer_time, c});
    }
    // Every remote commit at least as new as our frontier (minus the slop) is
    // known before our walk gets there; the rest of the remote history is
    // never read.
    auto expand_remote = [&](uint64_t down_to) -> Result<void> {
      while (!remote_q.empty() && remote_q.top().when + kSlop >= down_to) {
        Dated r = remote_q.top();
        remote_q.pop();
        auto cm = repo.read_commit(r.oid);
        if (!cm.ok()) { if (cm.status() == Status::NotFound) continue; return Result<void>(cm.error()); }
        for (auto& pr : cm->parents) {
          if (pr.empty() || !stop.insert(pr).second) continue;
          auto pc = repo.read_commit(pr);
          if (pc.ok()) remote_q.push({pc->committer_time, pr});
        }
      }
      return Ok();
    };
    while (!our_q.empty()) {
      Dated o = our_q.top();
      our_q.pop();
      if (auto e = expand_remote(o.when); !e.ok()) return Result<std::vector<std::string>>(e.error());
      if (stop.count(o.oid)) { reached.insert(o.oid); continue; }
      auto cm = repo.read_commit(o.oid);
      if (!cm.ok()) continue;   // the edge of a shallow history: walk_reachable says so
      for (auto& pr : cm->parents) {
        if (pr.empty() || !visited.insert(pr).second) continue;
        if (stop.count(pr)) { reached.insert(pr); continue; }
        auto pc = repo.read_commit(pr);
        if (pc.ok()) our_q.push({pc->committer_time, pr});
      }
    }
  }
  // The trees worth knowing are those of the commits our new history grows
  // from: the ones the walk reached. Every remote tip's tree was read before
  // (32 root trees of 3,000 entries inflated for a push of one commit, 40%
  // of the push); a base the walk never reached contributes its commit
  // only, and when nothing was reached at all every tip's tree is taken.
  if (trace) std::fprintf(stderr, "[git] objects_between: walk met the other side at %.1f ms\n", ms());
  size_t bases = 0;
  for (auto& c : stop_commits) {
    if (!reached.empty() && !reached.count(c)) continue;
    if (auto e = commit_tree_objects(repo, c, stop); !e.ok()) return Result<std::vector<std::string>>(e.error());
    ++bases;
  }
  if (trace)
    std::fprintf(stderr, "[git] objects_between: %zu remote tips, %zu reached, %zu base trees read, stop set %zu at %.1f ms\n",
                 stop_commits.size(), reached.size(), bases, stop.size(), ms());
  std::unordered_set<std::string> seen = stop;
  std::vector<std::string> order;
  for (auto& t : tips) {
    if (t.size() != 40) continue;
    std::vector<std::string> tags;
    std::string c = peel_tags(repo, t, &tags);
    for (auto& tag : tags) if (!stop.count(tag) && seen.insert(tag).second) order.push_back(tag);
    auto o = repo.read_object(c);
    if (!o.ok()) return Result<std::vector<std::string>>(Status::NotFound, "object " + c + " is not in this repository");
    if (o->type == "commit") {
      if (auto e = walk_reachable(repo, c, stop, seen, &order); !e.ok()) return Result<std::vector<std::string>>(e.error());
    } else if (o->type == "tree") {
      // walk_reachable starts from commits; a bare tree is walked as a commit's tree would be.
      std::vector<std::string> trees{c};
      while (!trees.empty()) {
        std::string tr = trees.back();
        trees.pop_back();
        if (stop.count(tr) || !seen.insert(tr).second) continue;
        order.push_back(tr);
        auto ents = repo.read_tree(tr);
        if (!ents.ok()) return Result<std::vector<std::string>>(ents.error());
        for (auto& e : *ents) {
          if (e.mode == 0160000 || stop.count(e.oid)) continue;
          if (e.is_dir()) trees.push_back(e.oid);
          else if (seen.insert(e.oid).second) order.push_back(e.oid);
        }
      }
    } else if (!stop.count(c) && seen.insert(c).second) {
      order.push_back(c);
    }
  }
  if (trace) std::fprintf(stderr, "[git] objects_between: %zu object(s) to send at %.1f ms\n", order.size(), ms());
  return order;
}

Result<uint64_t> git_copy_objects(const Git& from, Git& to, const std::vector<std::string>& oids) {
  GitBulkWrite bulk(to);
  uint64_t n = 0;
  for (auto& oid : oids) {
    if (to.has_object(oid)) continue;
    // A loose object's file is the object: the same bytes, under the same
    // name, are the object in the other repository too. Inflating it and
    // compressing it again was a quarter of a local push.
    if (to.copy_loose_object_from(from, oid)) { ++n; continue; }
    auto o = from.read_object(oid);
    if (!o.ok()) return Result<uint64_t>(o.error());
    auto w = to.write_object(o->type, ByteSpan(o->payload.data(), o->payload.size()));
    if (!w.ok()) return Result<uint64_t>(w.error());
    if (*w != oid) return Result<uint64_t>(Status::Corrupt, "object " + oid + " rewrote as " + *w);
    ++n;
  }
  return n;
}

Result<GitPushReport> GitRemote::push(Git& repo, const std::string& url, const std::string& refname, bool force,
                                      const GitRemoteOptions& opt) {
  if (auto r = may_network(); !r.ok()) return Result<GitPushReport>(r.error());
  auto u = git_parse_url(url);
  if (!u.ok()) return Result<GitPushReport>(u.error());

  std::string ref = refname;
  if (ref.empty()) {
    if (repo.branch().empty())
      return Result<GitPushReport>(Status::Invalid, "HEAD is detached — name the ref to push");
    ref = "refs/heads/" + repo.branch();
  } else if (ref.rfind("refs/", 0) != 0) {
    ref = "refs/heads/" + ref;
  }
  auto local = repo.ref_oid(ref);
  if (!local.ok())
    return Result<GitPushReport>(Status::NotFound, "this repository has no " + ref + " to push");

  auto cvr = open_service(kernel_, *u, opt, "git-receive-pack");
  if (!cvr.ok()) return Result<GitPushReport>(cvr.error());
  Conversation& cv = *cvr;
  Wire& w = *cv.wire;

  GitPushReport rep;
  rep.url = url;
  rep.transport = cv.report.transport;
  rep.host = cv.report.host;
  rep.path = cv.report.path;
  rep.user = cv.report.user;
  rep.port = cv.report.port;
  rep.host_key_fingerprint = cv.report.host_key_fingerprint;
  rep.server_version = cv.report.server_version;
  rep.cipher = cv.report.cipher;
  rep.kex = cv.report.kex;
  rep.ref = ref;
  rep.local_oid = *local;

  if (auto r = read_advertisement(w, cv.report, opt.timeout_ms); !r.ok()) return Result<GitPushReport>(r.error());
  rep.remote_old = kZeroOid;
  for (auto& r : cv.report.refs)
    if (r.name == ref) rep.remote_old = r.oid;
  rep.created = rep.remote_old == kZeroOid;
  if (!opt.expected_old.empty() && rep.remote_old != opt.expected_old) {
    ByteBuffer bye;
    write_flush(bye);
    w.send(ByteSpan(bye.data(), bye.size()));
    return Result<GitPushReport>(Status::Conflict,
        "compare-and-swap failed: expected remote " + ref + " at " + opt.expected_old + ", but found " + rep.remote_old);
  }

  if (rep.remote_old == rep.local_oid) {
    rep.up_to_date = true;
    rep.unpack_status = "ok";
    rep.ref_status = "up to date";
    ByteBuffer bye;
    write_flush(bye);
    w.send(ByteSpan(bye.data(), bye.size()));
    return rep;
  }

  // What the remote already has, so the pack carries only what it does not:
  // each of its refs' commits and the objects of that commit's tree. (Its
  // whole history was walked here before, every object of it inflated, for
  // a push of one commit: half a second on a 4k-file repository. An object
  // of an older commit that a new commit brings back is sent again, as git
  // itself does.) The walk below stops at those commits.
  // Every remote ref's commit stops the walk; the tree read whole is the one
  // the pushed ref points at now (what the new commits build on) - or, for
  // a ref being created, the first ref's. Every ref's whole tree was read
  // before: a remote with 20 branches inflated 20 trees of 3,000 entries
  // for a push of one commit (340 ms where git takes 40). An object another
  // branch holds is sent again, which the remote takes without harm.
  std::unordered_set<std::string> present;
  std::string base = rep.created ? std::string() : rep.remote_old;
  for (auto& r : cv.report.refs) {
    if (r.oid == kZeroOid || !repo.has_object(r.oid)) continue;   // a ref whose objects this machine never had
    present.insert(r.oid);
    if (base.empty()) base = r.oid;
  }
  if (!base.empty() && repo.has_object(base))
    if (auto e = commit_tree_objects(repo, base, present); !e.ok()) return Result<GitPushReport>(e.error());

  // A push that does not build on what is there rewrites the remote's history.
  if (!rep.created && !repo.has_object(rep.remote_old)) {
    if (!force)
      return Result<GitPushReport>(Status::Conflict,
                                  "not a fast-forward: " + ref + " on the remote is at " + rep.remote_old +
                                      ", which is not in the history being pushed — fetch first, or push with force=1");
    rep.forced = true;
  } else if (!rep.created) {
    // Fast-forward when the remote's tip is an ancestor of ours: a walk of
    // the commits, not of every tree and blob under them.
    auto reaches = commit_reaches(repo, rep.local_oid, rep.remote_old);
    if (!reaches.ok()) return Result<GitPushReport>(reaches.error());
    if (!*reaches) {
      if (!force)
        return Result<GitPushReport>(Status::Conflict,
                                    "not a fast-forward: " + rep.remote_old + " is not an ancestor of " +
                                        rep.local_oid + " — fetch first, or push with force=1");
      rep.forced = true;
    }
  }

  std::unordered_set<std::string> seen = present;
  std::vector<std::string> to_send;
  if (auto e = walk_reachable(repo, rep.local_oid, present, seen, &to_send); !e.ok())
    return Result<GitPushReport>(e.error());
  // to_send may legitimately be empty: a branch created at a commit the remote
  // already holds needs the ref update and a pack with nothing in it.

  uint64_t counted = 0;
  auto pack = build_pack(repo, to_send, counted, nullptr, kTransportPackLevel);
  if (!pack.ok()) return Result<GitPushReport>(pack.error());
  rep.objects = counted;
  rep.pack_bytes = pack->size();

  // report-status only: without side-band the remote's answer arrives as plain
  // pkt-lines, which is exactly what this machine reads best.
  std::string caps;
  if (cv.report.server_capabilities.find("report-status") != std::string::npos) caps = "report-status";
  // The protocol allows a client to name itself here; this kernel does not,
  // for the same reason nothing else in it speaks that vocabulary.

  ByteBuffer req;
  write_pkt(req, rep.remote_old + " " + rep.local_oid + " " + ref + std::string(1, '\0') + caps + "\n");
  write_flush(req);
  req.insert(req.end(), pack->begin(), pack->end());
  if (auto r = w.send(ByteSpan(req.data(), req.size())); !r.ok()) return Result<GitPushReport>(r.error());
  w.send_eof();

  if (caps.find("report-status") == std::string::npos) {
    rep.unpack_status = "not reported (the remote does not offer report-status)";
    rep.ref_status = rep.unpack_status;
  } else {
    for (;;) {
      auto p = read_pkt(w, opt.timeout_ms);
      if (!p.ok()) {
        if (!rep.unpack_status.empty()) break;      // the remote hung up after answering
        return Result<GitPushReport>(p.error());
      }
      if (p->kind == Pkt::Flush) break;
      std::string line = trim_nl(p->payload);
      if (line.rfind("unpack ", 0) == 0) rep.unpack_status = line.substr(7);
      else if (line.rfind("ok ", 0) == 0) rep.ref_status = "ok";
      else if (line.rfind("ng ", 0) == 0) {
        size_t sp = line.find(' ', 3);
        rep.ref_status = sp == std::string::npos ? line.substr(3) : line.substr(sp + 1);
      }
    }
    if (rep.unpack_status != "ok")
      return Result<GitPushReport>(Status::IoError, "the remote could not unpack what this machine sent: " + rep.unpack_status);
    if (rep.ref_status != "ok")
      return Result<GitPushReport>(Status::Denied, "the remote refused " + ref + ": " + rep.ref_status);
  }

  // The remote is where we said: remember that, the way a fetch would.
  if (ref.rfind("refs/heads/", 0) == 0)
    repo.update_ref("refs/remotes/origin/" + ref.substr(11), rep.local_oid);
  return rep;
}

Result<GitPushReport> GitRemote::push_updates(Git& repo, const std::string& url, std::vector<GitPushUpdate>& ups,
                                              const GitRemoteOptions& opt) {
  if (auto r = may_network(); !r.ok()) return Result<GitPushReport>(r.error());
  auto u = git_parse_url(url);
  if (!u.ok()) return Result<GitPushReport>(u.error());
  auto cvr = open_service(kernel_, *u, opt, "git-receive-pack");
  if (!cvr.ok()) return Result<GitPushReport>(cvr.error());
  Conversation& cv = *cvr;
  Wire& w = *cv.wire;
  GitPushReport rep;
  rep.url = url;
  rep.transport = cv.report.transport;
  rep.host = cv.report.host;
  rep.path = cv.report.path;
  if (auto r = read_advertisement(w, cv.report, opt.timeout_ms); !r.ok()) return Result<GitPushReport>(r.error());
  for (auto& r : cv.report.refs) rep.advertised.push_back(r.oid);
  std::unordered_map<std::string, std::string> remote;
  std::vector<std::string> remote_have;
  for (auto& r : cv.report.refs) {
    if (r.oid == kZeroOid) continue;
    remote[r.name] = r.oid;
    if (repo.has_object(r.oid)) remote_have.push_back(r.oid);
  }

  std::vector<std::string> commands, tips;
  bool deletes = false;
  for (auto& up : ups) {
    auto it = remote.find(up.ref);
    up.remote_old = it == remote.end() ? std::string(kZeroOid) : it->second;
    up.ok = false;
    if (up.has_lease && up.remote_old != (up.expected_old.empty() ? std::string(kZeroOid) : up.expected_old)) {
      up.status = "stale info";
      continue;
    }
    if (up.new_oid.empty()) {
      if (up.remote_old == kZeroOid) { up.status = "remote ref does not exist"; continue; }
      if (cv.report.server_capabilities.find("delete-refs") == std::string::npos) {
        up.status = "the remote does not allow deleting refs";
        continue;
      }
      deletes = true;
      commands.push_back(up.remote_old + " " + kZeroOid + " " + up.ref);
      continue;
    }
    if (up.new_oid == up.remote_old) { up.status = "up to date"; up.ok = true; continue; }
    if (up.remote_old != kZeroOid && !up.force && !up.has_lease) {
      if (up.ref.rfind("refs/tags/", 0) == 0) { up.status = "already exists"; continue; }
      if (!repo.has_object(up.remote_old)) { up.status = "fetch first"; continue; }
      std::unordered_set<std::string> reach, none;
      auto peeled = peel_tags(repo, up.new_oid, nullptr);
      auto po = repo.read_object(peeled);
      bool ff = false;
      if (po.ok() && po->type == "commit") {
        if (auto e = walk_reachable(repo, peeled, none, reach, nullptr); !e.ok()) return Result<GitPushReport>(e.error());
        ff = reach.count(up.remote_old) > 0;
      }
      if (!ff) { up.status = "non-fast-forward"; continue; }
    }
    commands.push_back(up.remote_old + " " + up.new_oid + " " + up.ref);
    tips.push_back(up.new_oid);
  }
  if (commands.empty()) {
    ByteBuffer bye;
    write_flush(bye);
    w.send(ByteSpan(bye.data(), bye.size()));
    return rep;
  }

  std::string caps;
  if (cv.report.server_capabilities.find("report-status") != std::string::npos) caps = "report-status";
  if (deletes) caps += caps.empty() ? "delete-refs" : " delete-refs";
  // side-band when the remote offers it: what its hooks say comes back on band 2
  const bool sideband = !caps.empty() && cv.report.server_capabilities.find("side-band-64k") != std::string::npos;
  if (sideband) caps += " side-band-64k";
  if (opt.quiet && cv.report.server_capabilities.find("quiet") != std::string::npos) caps += caps.empty() ? "quiet" : " quiet";
  ByteBuffer req;
  for (size_t i = 0; i < commands.size(); ++i)
    write_pkt(req, commands[i] + (i == 0 ? std::string(1, '\0') + caps : std::string()) + "\n");
  write_flush(req);
  if (!tips.empty()) {
    auto objs = git_objects_between(repo, tips, remote_have);
    if (!objs.ok()) return Result<GitPushReport>(objs.error());
    uint64_t counted = 0;
    auto pack = build_pack(repo, *objs, counted, nullptr, kTransportPackLevel);
    if (!pack.ok()) return Result<GitPushReport>(pack.error());
    rep.objects = counted;
    rep.pack_bytes = pack->size();
    req.insert(req.end(), pack->begin(), pack->end());
  }
  if (auto r = w.send(ByteSpan(req.data(), req.size())); !r.ok()) return Result<GitPushReport>(r.error());
  w.send_eof();

  auto mark = [&](const std::string& ref, bool ok, const std::string& why) {
    for (auto& up : ups)
      if (up.ref == ref && up.status.empty()) { up.ok = ok; up.status = ok ? "ok" : why; }
  };
  if (caps.find("report-status") == std::string::npos) {
    for (auto& up : ups) if (up.status.empty()) { up.ok = true; up.status = "ok"; }
    return rep;
  }
  auto report = read_push_report(w, opt.timeout_ms, sideband, rep.remote_messages);
  if (!report.ok()) return Result<GitPushReport>(report.error());
  for (const std::string& line : *report) {
    if (line.rfind("unpack ", 0) == 0) rep.unpack_status = line.substr(7);
    else if (line.rfind("ok ", 0) == 0) mark(line.substr(3), true, "");
    else if (line.rfind("ng ", 0) == 0) {
      size_t sp = line.find(' ', 3);
      mark(sp == std::string::npos ? line.substr(3) : line.substr(3, sp - 3), false,
           sp == std::string::npos ? "rejected" : line.substr(sp + 1));
    }
  }
  if (rep.unpack_status != "ok")
    return Result<GitPushReport>(Status::IoError, "the remote could not unpack what this machine sent: " + rep.unpack_status);
  for (auto& up : ups) if (up.status.empty()) up.status = "no status reported";
  return rep;
}

Result<GitFetchReport> GitRemote::clone(const std::string& url, const std::string& dir, const GitRemoteOptions& opt) {
  Git repo(kernel_, actor_);
  if (auto r = repo.init(dir); !r.ok()) return Result<GitFetchReport>(r.error());
  // A clone has no local branches to protect: it takes the remote's, and lays
  // the work tree down, unless the caller asked for the database only.
  GitRemoteOptions o = opt;
  o.advance_branches = true;
  return fetch(repo, url, o);
}

}  // namespace aios::userland
