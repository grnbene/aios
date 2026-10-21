#pragma once
// The git wire protocol, spoken by this kernel.
//
// `git clone` is not a program this machine runs — it is a transport (SSH,
// the git daemon protocol or smart HTTP), a pkt-line conversation with the
// remote's upload-pack (protocol v2 over smart HTTP and git:// when the server
// speaks it, v0 otherwise), a packfile the kernel indexes itself (including OFS_DELTA and
// REF_DELTA reconstruction), and objects written into the machine's own object
// database. Every byte moves over hal::net and hal::Host, so a bare-metal
// kernel with its own TCP/IP stack clones the same way a hosted one does.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/git.hpp"

namespace aios::userland {

struct GitRemoteRef {
  std::string name;      // refs/heads/main
  std::string oid;
};

struct GitRemoteOptions {
  int depth = 0;                  // 0 = the whole history; >0 asks the remote to truncate it
  bool deepen_relative = false;   // the depth counts from this repository's shallow commits (--deepen)
  bool include_tag = false;       // include-tag: the remote adds annotated tags pointing into the pack (auto-follow)
  bool quiet = false;             // quiet: ask a receive-pack for no progress (git does when stderr is no terminal)
  // ssh: with ssh_key set, that one key signs and `known_hosts` is the machine's own file (aios-sh's key=). Empty:
  // the destination is taken as OpenSSH takes it, through the person's ssh_config, their agent and key files, and
  // their known_hosts (with the question on the terminal for a host never met).
  std::string ssh_key;
  std::string known_hosts;
  // What ssh prints as a warning on the way (a host key newly recorded, a key file it could not read).
  std::function<void(const std::string&)> ssh_notice;
  std::string ref;                // fetch one ref only; empty means every head
  int timeout_ms = 120000;
  // What to do with what came back. A fetch brings the remote's refs down to
  // refs/remotes/<name>/* and touches nothing else — moving the branch you are
  // standing on, and not the files under it, is what a pull is for.
  bool advance_branches = false;
  // Whether a branch that moves takes the work tree with it. True by default,
  // because a clone must; `fetch` turns it off along with advance_branches.
  bool checkout = true;
  // Push only: the oid the remote ref must still be at (compare-and-swap). If the remote has
  // moved, the push is refused with Status::Conflict and nothing is sent.
  std::string expected_old;
  // Extra request headers for http(s) remotes (git's http.extraHeader), e.g. an Authorization line.
  std::vector<std::pair<std::string, std::string>> http_headers;
  // Fetch: choose what to ask for from what the remote advertised (object ids). When set, the fetch
  // only brings objects in and lands no refs: the caller maps refs itself (refspecs, FETCH_HEAD).
  std::function<std::vector<std::string>(const std::vector<GitRemoteRef>&)> select;
  // A partial clone's object filter (git's --filter): "blob:none", "blob:limit=<n>[kmg]". The remote must offer it.
  std::string filter;
  // Tell the remote nothing about what is here (a partial clone fetching the objects it lacks: they are reachable
  // from commits it has, which the remote would otherwise take as "already there").
  bool no_haves = false;
};

// One ref update in a push. `new_oid` empty deletes the remote ref. `expected_old` (when not empty)
// is a lease: the update is refused unless the remote still holds exactly that.
struct GitPushUpdate {
  std::string new_oid, ref, expected_old;
  bool force = false;
  bool has_lease = false;
  // Filled in by the push:
  std::string remote_old;
  std::string status;        // "ok", "up to date", or why it was refused
  bool ok = false;
};

struct GitFetchReport {
  std::string url, transport, host, path, user;
  uint16_t port = 0;
  std::vector<GitRemoteRef> refs;         // what the remote advertised
  std::string server_capabilities;
  int protocol = 0;                       // 2 when the remote spoke protocol v2 (smart HTTP, git://), else 0
  std::string default_branch;             // from the remote's symref=HEAD, when it says
  std::string head_oid;
  bool diverged = false;                  // the branch could not fast-forward to what came back
  std::string local_before;               // where the local branch was, when it moved
  uint64_t pack_bytes = 0;
  uint64_t objects = 0;                   // objects in the pack
  uint64_t deltas = 0;                    // of those, delta-encoded
  uint64_t written = 0;                   // objects added to this database
  uint64_t files = 0;                     // paths written into the work tree
  uint64_t shallow = 0;                   // commits recorded as the edge of a truncated history
  uint64_t haves = 0;                     // objects this machine told the remote it already had
  uint64_t acks = 0;                      // of those, ones the remote recognised
  std::string host_key_fingerprint;
  std::string server_version;             // the SSH server's identification
  std::string cipher, kex;
};

// What a push did. `up_to_date` means the remote's ref already pointed where
// this machine wanted it; nothing was sent and nothing is wrong.
struct GitPushReport {
  std::string url, transport, host, path, user;
  uint16_t port = 0;
  std::string ref;                        // refs/heads/main
  std::string local_oid, remote_old;      // where it goes, where it was
  uint64_t objects = 0;                   // objects packed and sent
  uint64_t pack_bytes = 0;
  bool created = false;                   // the remote had no such ref
  bool forced = false;                    // it was not a fast-forward and force was given
  bool up_to_date = false;
  std::string unpack_status;              // the remote's own words: "ok", or why not
  std::string ref_status;
  std::string remote_messages;            // what the remote said to the user (side-band 2: its hooks' words)
  std::vector<std::string> advertised;    // the ids of the refs the remote advertised (git sizes its report by them)
  std::string host_key_fingerprint, server_version, cipher, kex;
};

class GitRemote {
 public:
  // Talking to a remote spends the acting entity's network authority, and
  // landing what comes back spends its storage authority; both are checked
  // through the Surface before a socket is opened.
  GitRemote(Kernel& kernel, Surface* actor = nullptr);

  // What the remote has, without transferring anything.
  Result<GitFetchReport> ls_remote(const std::string& url, const GitRemoteOptions& opt = {});
  // Create `dir`, pull the history down and check the default branch out.
  Result<GitFetchReport> clone(const std::string& url, const std::string& dir, const GitRemoteOptions& opt = {});
  // Bring an existing repository up to date with the remote.
  Result<GitFetchReport> fetch(Git& repo, const std::string& url, const GitRemoteOptions& opt = {});
  // Send this machine's work the other way: the objects the remote is missing,
  // packed here, and a ref update it either accepts or refuses in its own words.
  // A push that is not a fast-forward is refused unless `force`.
  Result<GitPushReport> push(Git& repo, const std::string& url, const std::string& ref, bool force,
                             const GitRemoteOptions& opt = {});
  // Several updates in one conversation, as `git push <remote> a:b c:d :e` does. Each update reports
  // its own outcome; the result fails only when the conversation itself does.
  Result<GitPushReport> push_updates(Git& repo, const std::string& url, std::vector<GitPushUpdate>& updates,
                                     const GitRemoteOptions& opt = {});

 private:
  Result<void> may_network() const;
  Kernel& kernel_;
  Surface* actor_ = nullptr;
};

// The packfile a push sends: a version-2 pack of whole objects (no deltas), SHA-1 trailer included.
// Exposed so tools/git-soak.cpp can hold what this machine sends against `git index-pack --strict`.
Result<ByteBuffer> git_build_pack(Git& repo, const std::vector<std::string>& oids, uint64_t& counted);

// A delta that rebuilds `target` from `base`, in the packfile's delta format (what git_apply_delta reads): copies
// of runs the two share and the bytes in between. Empty when it would not be smaller than `max_size` bytes (or when
// either side is too big to address): the caller then stores the object whole.
ByteBuffer git_make_delta(ByteSpan base, ByteSpan target, size_t max_size);

// `git repack`: this repository's own objects written into one new pack with its index (the loose ones, and with
// `all` the ones already in packs too: an object stored in a pack is copied as it is, deltas kept). With
// `remove_old`, what the new pack now holds is removed: the loose files, and with `all` the earlier packs (one with a
// .keep beside it is left). The new pack is complete on disk before anything is removed.
struct GitRepackReport {
  uint64_t objects = 0;         // in the new pack (0: there was nothing to pack, and nothing was written)
  uint64_t loose_removed = 0;
  uint64_t packs_removed = 0;
  std::string pack;             // the new pack's path without its extension
};
Result<GitRepackReport> git_repack(Git& repo, bool all, bool remove_old);
// The other way: every object a packfile holds, deltas resolved, written into `repo`.
Result<void> git_index_pack(Git& repo, ByteSpan pack, uint64_t* objects = nullptr, uint64_t* deltas = nullptr,
                            uint64_t* written = nullptr);
// Every object reachable from `tips` that is not reachable from `stop_tips` (commits, trees, blobs and
// annotated tags), in an order a pack can be written in. What a push, a bundle and a local fetch send.
Result<std::vector<std::string>> git_objects_between(const Git& repo, const std::vector<std::string>& tips,
                                                     const std::vector<std::string>& stop_tips);
// A partial clone's filter applied to a list of objects about to be sent (blobs left out, except those in `wants`).
// False when the filter is not one this machine knows.
bool git_filter_objects(const Git& repo, const std::string& filter, const std::vector<std::string>& wants,
                        std::vector<std::string>& objects);
// Copy objects from one repository's database into another's (a fetch or push between two local repositories).
Result<uint64_t> git_copy_objects(const Git& from, Git& to, const std::vector<std::string>& oids);

// The server side of the protocol, for `git http-backend`, `git upload-pack` and `git receive-pack`.
// `in` is everything the client sent (one stateless request), the return value everything to answer.
// `git_protocol` is the client's Git-Protocol header (GIT_PROTOCOL to git http-backend, and in the environment of an
// upload-pack that ssh or a daemon runs): with "version=2" in it,
// upload-pack answers with the v2 capability advertisement, and its requests are then v2 commands (ls-refs, fetch),
// which git_serve_upload_pack recognises by their "command=" line. Without it, or for receive-pack, v0.
std::string git_advertise_refs(const Git& repo, const std::string& service, bool http_banner,
                               const std::string& git_protocol = {});
Result<std::string> git_serve_upload_pack(Git& repo, const std::string& in);
// A ref a push changed (for the hooks a receiving repository runs afterwards).
struct ReceivedRef { std::string old_oid, new_oid, ref; };
// The repository's say on a push before any ref moves (git's pre-receive): given every command that would take
// effect (the objects already in), "" lets them through; anything else declines them all, and is what the pushing
// client is told ("remote: ..." lines, with each ref "pre-receive hook declined").
using GitPreReceive = std::function<std::string(const std::vector<ReceivedRef>& proposed)>;
// `applied`, when given, gets every command that took effect.
Result<std::string> git_serve_receive_pack(Git& repo, const std::string& in, std::vector<ReceivedRef>* applied = nullptr,
                                           const GitPreReceive& pre_receive = nullptr);

// "ssh://user@host:port/path", "user@host:path", "git://host/path" or a local
// path. Anything https is refused with the reason, not a generic failure.
struct GitUrl {
  std::string scheme;    // ssh | git | http | https | file
  std::string user, host, path;
  std::string password;  // http(s) only: from https://user:password@host/...; sent as Basic auth
  uint16_t port = 0;
};
Result<GitUrl> git_parse_url(const std::string& url);

}  // namespace aios::userland
