// git verify-commit [-v] [--raw] <commit>...: checks each commit's SSH signature (gpg.format=ssh) against
// gpg.ssh.allowedSignersFile, as git does: "Good "git" signature for <principal> with ED25519 key SHA256:...".
// A good signature by a key the file does not list is a failure ("No principal matched."), as in git.
#include "aios/userland/git_sign.hpp"
#include "cli.hpp"

namespace aios_git {

using namespace aios::userland;

int cmd_verify_commit(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'v', "verbose"}, {0, "raw"}});
  c.open_repo(false);
  if (p.pos.empty()) usage(c, "usage: git verify-commit [-v | --verbose] [--raw] <commit>...");
  std::string allowed_path = c.config.get("gpg.ssh.allowedSignersFile").value_or("");
  std::string allowed;
  bool have_allowed = false;
  if (!allowed_path.empty()) {
    if (allowed_path.rfind("~/", 0) == 0) allowed_path = slashes(c.getenv("HOME")) + allowed_path.substr(1);
    auto r = c.host().read_file(c.abs_path(allowed_path));
    if (r.ok()) { allowed.assign((const char*)r->data(), r->size()); have_allowed = true; }
  }
  int rc = 0;
  for (auto& spec : p.pos) {
    std::string oid = commit_of(c, spec);
    auto obj = c.repo->read_object(oid);
    if (!obj.ok()) die(c, "cannot read " + spec);
    std::string raw((const char*)obj->payload.data(), obj->payload.size());
    auto split = git_split_signature(raw);
    if (p.has("verbose")) c.out << split.payload;
    if (split.signature.empty()) { c.err << "error: no signature found\n"; rc = 1; continue; }
    if (split.signature.find("-----BEGIN SSH SIGNATURE-----") == std::string::npos) {
      c.err << "error: " << spec << ": not an SSH signature (aios-git checks gpg.format=ssh signatures)\n";
      rc = 1;
      continue;
    }
    if (!have_allowed) {
      c.err << "error: gpg.ssh.allowedSignersFile needs to be configured and exist for ssh signature verification\n";
      rc = 1;
      continue;
    }
    auto chk = sshsig_verify(split.signature, "git", split.payload);
    if (!chk.ok) {
      c.err << "Bad signature" << (chk.fingerprint.empty() ? "" : " with ED25519 key " + chk.fingerprint) << ": " << chk.problem << "\n";
      rc = 1;
      continue;
    }
    auto who = ssh_allowed_principals(allowed, chk.key);
    if (who.empty()) {
      c.err << "Good \"git\" signature with ED25519 key " << chk.fingerprint << "\nNo principal matched.\n";
      rc = 1;
      continue;
    }
    c.err << "Good \"git\" signature for " << who.front() << " with ED25519 key " << chk.fingerprint << "\n";
  }
  return rc;
}

}  // namespace aios_git
