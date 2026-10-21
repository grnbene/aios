// The account and host name on Windows: the token's user (name and the RID that
// ends its SID) and %USERPROFILE%, which is where Win32-OpenSSH keeps ~/.ssh.
#include "aios/hal/account.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <userenv.h>
#include <vector>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

namespace aios::hal {
namespace {

std::string narrow(const wchar_t* w, int len = -1) {
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
  if (len < 0 && !s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

}  // namespace

Result<LocalAccount> local_account() {
  LocalAccount a;
  wchar_t name[257];
  DWORD n = 257;
  if (!::GetUserNameW(name, &n)) return Result<LocalAccount>(Status::IoError, "GetUserNameW failed");
  a.name = narrow(name);
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD need = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    std::vector<uint8_t> buf(need ? need : 1);
    if (need && ::GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
      PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
      UCHAR count = *::GetSidSubAuthorityCount(sid);
      if (count > 0) a.uid = (uint32_t)*::GetSidSubAuthority(sid, count - 1u);
    }
    DWORD len = 0;
    ::GetUserProfileDirectoryW(token, nullptr, &len);
    if (len) {
      std::vector<wchar_t> dir(len);
      if (::GetUserProfileDirectoryW(token, dir.data(), &len)) a.home = narrow(dir.data());
    }
    ::CloseHandle(token);
  }
  if (a.home.empty()) {
    wchar_t buf[MAX_PATH];
    DWORD got = ::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (got > 0 && got < MAX_PATH) a.home = narrow(buf, (int)got);
  }
  for (char& c : a.home) if (c == '\\') c = '/';
  if (a.home.empty()) return Result<LocalAccount>(Status::NotFound, "this account has no profile directory");
  return a;
}

std::string local_hostname() {
  wchar_t name[256];
  DWORD n = 256;
  if (!::GetComputerNameExW(ComputerNameDnsHostname, name, &n)) return {};
  return narrow(name, (int)n);
}

}  // namespace aios::hal
