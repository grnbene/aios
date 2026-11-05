#pragma once
// Licensing for the public build. A public build (cmake -DAIOS_PUBLIC_BUILD=ON) runs for AIOS_TRIAL_DAYS (30) from
// the first time it ran on the machine, and after that only with a license: a small text file naming who it is for
// and until when (or "never"), signed with Ed25519 by the private key only the issuer holds. The binary carries the
// public key alone, so it can check a license but nobody can make one from it. A build without the option never
// checks; `aios license` (status, install, verify, keygen, issue) works in both.
//
// What it does not stop: someone who edits the compiled binary to skip the check, or who deletes every trial record
// (see trial_paths) and so starts the 30 days again. Neither is a key they can make or pass on.
#include <ostream>
#include <string>
#include <vector>

namespace aios::license {

struct Verdict {
  bool ok = false;
  // For the person: why it refused, or a reminder (trial days left, a license near its end). Empty when there is
  // nothing to say.
  std::string message;
};

// Whether this machine may run a public build now. `root` is the --root directory ("" when there is none, as for
// aios-webserver). A trial run also records the trial start where it is not recorded yet.
Verdict check(const std::string& root);

// `aios license SUBCOMMAND ...` (args[0] is "license"). Returns the process exit code.
int command(const std::vector<std::string>& args, const std::string& root, std::ostream& out, std::ostream& err);

// Exit code of a refused run (EX_NOPERM).
constexpr int kRefused = 77;

}  // namespace aios::license
