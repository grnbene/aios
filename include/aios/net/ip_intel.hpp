#pragma once
// What the box itself can say about an IP address with no network call: its family, and its scope from the address
// alone (RFC 1918/4193/6598/3927 and loopback). One implementation, shared by the edge server's honeypot, the grain
// runtime (ip_scope / ip_family) and ASQL (IP_SCOPE / IP_FAMILY) so they never disagree. Anything needing ownership
// (datacenter vs residential vs mobile, ASN) needs a dataset and lives with whoever loaded one — not here.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace aios::net {

// "[2001:db8::1]:443" -> "2001:db8::1", "a, b" -> "a", trimmed.
inline std::string ip_clean(const std::string& in) {
  std::string s = in.substr(0, in.find(','));
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  if (a == std::string::npos) return "";
  s = s.substr(a, b - a + 1);
  if (!s.empty() && s.front() == '[') { auto e = s.find(']'); if (e != std::string::npos) return s.substr(1, e - 1); }
  if (std::count(s.begin(), s.end(), ':') == 1) s = s.substr(0, s.find(':'));   // host:port, not IPv6
  return s;
}

inline bool ip_parse4(const std::string& s, uint32_t* out) {
  unsigned a, b, c, d;
  char tail;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  *out = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}

inline std::string ip_family(const std::string& ip) { return ip.find(':') != std::string::npos ? "ipv6" : "ipv4"; }

// loopback / private / cgnat / link-local / unspecified / public / unknown — from the address itself.
inline std::string ip_scope(const std::string& ip_in) {
  const std::string ip = ip_clean(ip_in);
  if (ip.find(':') != std::string::npos) {
    if (ip == "::1") return "loopback";
    if (ip.size() >= 4) {
      std::string p = ip.substr(0, 4);
      if (p >= "fe80" && p <= "febf") return "link-local";   // fe80::/10
      if (ip[0] == 'f' && (ip[1] == 'c' || ip[1] == 'd')) return "private";   // fc00::/7
    }
    return "public";
  }
  uint32_t v;
  if (!ip_parse4(ip, &v)) return "unknown";
  if ((v >> 24) == 127) return "loopback";
  if ((v >> 24) == 10) return "private";
  if ((v >> 20) == 0xAC1) return "private";                 // 172.16/12
  if ((v >> 16) == 0xC0A8) return "private";                // 192.168/16
  if ((v >> 16) == 0xA9FE) return "link-local";             // 169.254/16
  if ((v & 0xFFC00000u) == 0x64400000u) return "cgnat";     // 100.64/10
  if ((v >> 24) == 0) return "unspecified";
  return "public";
}

// routable on the public internet (what an attacker's real infrastructure is, vs loopback/private/cgnat noise).
inline bool ip_is_public(const std::string& ip) {
  const std::string s = ip_scope(ip);
  return s == "public";
}

}  // namespace aios::net
