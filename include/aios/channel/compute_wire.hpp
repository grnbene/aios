#pragma once
// The fixed-width binary form of a compute request. The JSON form is parsed
// into an RpcValue tree and rendered back to text on every call; this form is
// read in place — the server casts the frame's payload to these structs and
// builds the job from the numbers directly, and the answer goes back the same
// way. Same question, same cache key, same answer as the JSON path, so a
// client may mix the two.
//
// On the SHM ring (shm_abi.hpp) a frame of msg_type kTspBinary is one of:
//   TspRequest  { TspRequestHeader, n × TspPoint }
//   TspResponse { TspResponseHeader, n × int64 order }
//
// On the TCP port the same bytes travel inside the usual 4-byte big-endian
// length frame; the server tells the two forms apart by the first four bytes
// (a JSON document starts with '{', this with "ATSP"). A TCP request may
// carry the service token after the points: uint16 length, then the bytes.
//
// Ids are int64: the ERP's stop ids are integers and its depot is "-1". A
// client whose ids are not integers sends the JSON form instead.
//
// Everything is little-endian: shared memory on one machine, or x86 hosts on
// one LAN; the PHP client packs with 'q' / 'e'.
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#define AIOS_PACKED
#else
#define AIOS_PACKED __attribute__((packed))
#endif

namespace aios::channel::wire {

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif

constexpr uint16_t kJson = 1;       // the JSON document form, as before
constexpr uint16_t kTspBinary = 2;  // TspRequest / TspResponse below

constexpr uint32_t kTspRequestMagic = 0x50535441u;   // "ATSP" in memory
constexpr uint32_t kTspResponseMagic = 0x52535441u;  // "ATSR"
constexpr uint16_t kVersion = 1;

enum TspOp : uint8_t { TspExact = 1, PoolGreedy = 2 };
enum TspStatus : uint16_t { Done = 0, Pending = 1, Error = 2 };

struct AIOS_PACKED TspRequestHeader {
  uint32_t magic;        // kTspRequestMagic
  uint16_t version;      // kVersion
  uint8_t op;            // TspOp
  uint8_t prune;         // 0/1
  uint32_t n;            // points that follow, start first
  uint32_t deadline_ms;  // 0: the service default (TCP only; the ring always waits)
};
static_assert(sizeof(TspRequestHeader) == 16, "TspRequestHeader is 16 bytes");

struct AIOS_PACKED TspPoint {
  int64_t id;
  double lat;
  double lng;
};
static_assert(sizeof(TspPoint) == 24, "TspPoint is 24 bytes");

constexpr size_t kDeviceNameBytes = 16;

struct AIOS_PACKED TspResponseHeader {
  uint32_t magic;    // kTspResponseMagic
  uint16_t version;  // kVersion
  uint16_t status;   // TspStatus
  uint32_t n;        // order entries that follow (0 unless Done)
  uint8_t cached;    // the answer came from the finished-answer cache
  uint8_t reserved[3];
  double distance_miles;
  uint64_t evaluated;
  uint64_t pruned;
  double elapsed_ms;
  uint32_t lanes;
  char device[kDeviceNameBytes];  // NUL-padded, truncated if longer
  char error[64];                 // NUL-padded; set when status is Error
};
static_assert(sizeof(TspResponseHeader) == 132, "TspResponseHeader is 132 bytes");

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

inline bool looks_binary(const void* p, size_t len) {
  if (len < sizeof(TspRequestHeader)) return false;
  uint32_t m; std::memcpy(&m, p, 4);
  return m == kTspRequestMagic;
}
inline size_t tsp_request_bytes(uint32_t n) { return sizeof(TspRequestHeader) + (size_t)n * sizeof(TspPoint); }
inline size_t tsp_response_bytes(uint32_t n) { return sizeof(TspResponseHeader) + (size_t)n * sizeof(int64_t); }

}  // namespace aios::channel::wire
