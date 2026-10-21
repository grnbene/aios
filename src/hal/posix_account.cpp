// The account and host name on Linux and macOS: the password database entry for
// the real uid, as OpenSSH reads it.
#include "aios/hal/account.hpp"

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <unistd.h>
#include <vector>

namespace aios::hal {

Result<LocalAccount> local_account() {
  const uid_t uid = ::getuid();
  long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> buf(size > 0 ? (size_t)size : 16384);
  for (;;) {
    passwd pw{};
    passwd* found = nullptr;
    int rc = ::getpwuid_r(uid, &pw, buf.data(), buf.size(), &found);
    if (rc == ERANGE && buf.size() < (1u << 20)) { buf.resize(buf.size() * 2); continue; }
    if (rc != 0) return Result<LocalAccount>(Status::IoError, std::string("getpwuid_r: ") + std::strerror(rc));
    if (!found) return Result<LocalAccount>(Status::NotFound, "uid " + std::to_string(uid) + " has no entry in the password database");
    LocalAccount a;
    a.name = pw.pw_name ? pw.pw_name : "";
    a.home = pw.pw_dir ? pw.pw_dir : "";
    a.uid = (uint32_t)uid;
    return a;
  }
}

std::string local_hostname() {
  char name[256];
  if (::gethostname(name, sizeof name) != 0) return {};
  name[sizeof name - 1] = '\0';
  return name;
}

}  // namespace aios::hal
