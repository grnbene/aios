#pragma once
// The account this process runs as, and the machine's name, as the substrate
// knows them. OpenSSH takes `~` from the account's home directory in the
// password database (not from $HOME), and its %u, %i, %l and %L tokens from
// here, so a client that means the same thing by a config file must too.
#include <cstdint>
#include <string>

#include "aios/core/result.hpp"

namespace aios::hal {

struct LocalAccount {
  std::string name;      // the login name
  std::string home;      // its home directory
  uint32_t uid = 0;      // numeric id (on Windows, the RID of the user's SID)
};

// Status::Unsupported where the substrate has no accounts.
Result<LocalAccount> local_account();
// The machine's host name, as gethostname(2) gives it; "" when there is none.
std::string local_hostname();

}  // namespace aios::hal
