// The account and host name on Linux and macOS: the password database entry for
// the real uid, as OpenSSH reads it.
#include "aios/hal/account.hpp"

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <string>

namespace aios::hal {

Result<LocalAccount> local_account() {
  const uid_t uid = ::getuid();
#if defined(AIOS_STATIC_LINK)
  // A static glibc's getpwuid_r loads the host's NSS libraries at run time: /etc/passwd is read here instead
  // (name:password:uid:gid:gecos:home:shell).
  {
    FILE* f = std::fopen("/etc/passwd", "re");
    if (!f) return Result<LocalAccount>(Status::NotFound, "uid " + std::to_string(uid) + ": /etc/passwd cannot be read");
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
      std::vector<std::string> fld;
      std::string cur;
      for (const char* p = line; *p && *p != '\n'; ++p) { if (*p == ':') { fld.push_back(cur); cur.clear(); } else cur += *p; }
      fld.push_back(cur);
      if (fld.size() < 7 || fld[2] != std::to_string(uid)) continue;
      std::fclose(f);
      LocalAccount a;
      a.name = fld[0];
      a.home = fld[5];
      a.uid = (uint32_t)uid;
      return a;
    }
    std::fclose(f);
    return Result<LocalAccount>(Status::NotFound, "uid " + std::to_string(uid) + " has no entry in /etc/passwd");
  }
#else
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
#endif
}

std::string local_hostname() {
  char name[256];
  if (::gethostname(name, sizeof name) != 0) return {};
  name[sizeof name - 1] = '\0';
  return name;
}

}  // namespace aios::hal
