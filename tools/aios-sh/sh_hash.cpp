// sha256sum, sha1sum (aios's own SHA) and md5sum (here: RFC 1321), with -c to check a list.
#include "sh.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "aios/crypto/sha1.hpp"
#include "aios/crypto/sha256.hpp"

namespace aiossh {

namespace {

// MD5's compression of one 64-byte block into the state.
void md5_block(uint32_t h[4], const unsigned char* m) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
      0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
      0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
      0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
      0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t w[16];
  for (int i = 0; i < 16; ++i)
    w[i] = static_cast<uint32_t>(m[i * 4]) | (static_cast<uint32_t>(m[i * 4 + 1]) << 8) | (static_cast<uint32_t>(m[i * 4 + 2]) << 16) |
           (static_cast<uint32_t>(m[i * 4 + 3]) << 24);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
    else { f = c ^ (b | ~d); g = (7 * i) % 16; }
    uint32_t t = d;
    d = c;
    c = b;
    uint32_t x = a + f + K[i] + w[g];
    b = b + ((x << R[i]) | (x >> (32 - R[i])));
    a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

std::string hex(const uint8_t* p, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += k[p[i] >> 4]; s += k[p[i] & 15]; }
  return s;
}

// One of the three digests, fed a block at a time: a file is hashed as it is read, never held whole.
class Hasher {
 public:
  explicit Hasher(const std::string& algo) : kind_(algo == "sha256sum" ? Sha256 : algo == "sha1sum" ? Sha1 : Md5) {
    if (kind_ == Sha256) aios::crypto::sha256_init(sha256_);
    else if (kind_ == Sha1) aios::crypto::sha1_init(sha1_);
  }
  void update(const char* p, size_t n) {
    const uint8_t* u = reinterpret_cast<const uint8_t*>(p);
    if (kind_ == Sha256) { aios::crypto::sha256_update(sha256_, u, n); return; }
    if (kind_ == Sha1) { aios::crypto::sha1_update(sha1_, u, n); return; }
    md5_bytes_ += n;
    while (n) {
      size_t take = std::min(n, sizeof md5_buf_ - md5_fill_);
      std::memcpy(md5_buf_ + md5_fill_, u, take);
      md5_fill_ += take;
      u += take;
      n -= take;
      if (md5_fill_ == sizeof md5_buf_) { md5_block(md5_h_, md5_buf_); md5_fill_ = 0; }
    }
  }
  std::string hex_digest() {
    if (kind_ == Sha256) { uint8_t h[32]; aios::crypto::sha256_final(sha256_, h); return hex(h, sizeof h); }
    if (kind_ == Sha1) { uint8_t h[20]; aios::crypto::sha1_final(sha1_, h); return hex(h, sizeof h); }
    // RFC 1321's padding: 0x80, zeros to 56 bytes into a block, then the length in bits, little-endian
    const uint64_t bits = md5_bytes_ * 8;
    unsigned char pad[72] = {0x80};
    size_t pad_len = (md5_fill_ < 56 ? 56 : 120) - md5_fill_;
    for (int i = 0; i < 8; ++i) pad[pad_len + static_cast<size_t>(i)] = static_cast<unsigned char>((bits >> (8 * i)) & 0xff);
    update(reinterpret_cast<const char*>(pad), pad_len + 8);
    std::string out;
    char buf[3];
    for (uint32_t h : md5_h_) for (int i = 0; i < 4; ++i) { std::snprintf(buf, sizeof buf, "%02x", (h >> (8 * i)) & 0xff); out += buf; }
    return out;
  }

 private:
  enum Kind { Sha256, Sha1, Md5 } kind_;
  aios::crypto::SHA256Context sha256_{};
  aios::crypto::SHA1Context sha1_{};
  uint32_t md5_h_[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  unsigned char md5_buf_[64] = {};
  size_t md5_fill_ = 0;
  uint64_t md5_bytes_ = 0;
};

// The digest of everything left on a stream, read a block at a time.
std::string digest_of(const std::string& algo, Fd f) {
  Hasher h(algo);
  char buf[1 << 16];
  long n;
  while ((n = os::read_fd(f, buf, sizeof buf)) > 0) h.update(buf, static_cast<size_t>(n));
  return h.hex_digest();
}

}  // namespace

int u_sha_md5(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool check = false, quiet = false;
  std::vector<std::string> files;
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "-c" || argv[i] == "--check") check = true;
    else if (argv[i] == "--quiet" || argv[i] == "--status") quiet = true;
    else if (argv[i] == "-b" || argv[i] == "-t" || argv[i] == "--binary" || argv[i] == "--text") {}
    else files.push_back(argv[i]);
  }
  // "-" is standard input; anything else is opened for reading, closed again when done with (or when a write throws)
  auto open = [&](const std::string& f, FdCloser& owned, std::string* e) -> Fd {
    if (f == "-") return io.in;
    std::string p = host_path(st.cwd, f);
    if (os::is_dir(p)) { *e = "Is a directory"; return kNoFd; }
    owned.f = os::open_file(p, os::Open::Read, e);
    return owned.f;
  };
  if (files.empty()) files.push_back("-");
  int status = 0;
  for (const auto& f : files) {
    std::string e;
    FdCloser owned;
    Fd fd = open(f, owned, &e);
    if (fd == kNoFd) { io.ewrite(argv[0] + ": " + f + ": " + e + "\n"); status = 1; continue; }
    if (!check) { io.write(digest_of(argv[0], fd) + "  " + f + "\n"); continue; }
    LineReader list(fd);
    std::string line;
    while (list.next(line)) {
      size_t sp = line.find(' ');
      if (sp == std::string::npos) continue;
      std::string want = line.substr(0, sp), name = line.substr(sp + 1);
      while (!name.empty() && (name[0] == ' ' || name[0] == '*')) name.erase(0, 1);
      if (!name.empty() && name.back() == '\r') name.pop_back();
      FdCloser held;
      Fd target = open(name, held, &e);
      bool ok = target != kNoFd && digest_of(argv[0], target) == want;
      if (!ok) status = 1;
      if (!quiet) io.write(name + (ok ? ": OK\n" : ": FAILED\n"));
    }
  }
  return status;
}

}  // namespace aiossh
