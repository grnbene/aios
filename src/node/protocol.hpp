#pragma once
#include "aios/core/bytes.hpp"
#include "aios/hal/net.hpp"
namespace aios::nodeproto {
constexpr char MAGIC[4] = {'A', 'O', 'S', 'N'};
enum Type : uint8_t {
  Hello = 1,
  MemAlloc = 2,
  MemWrite = 3,
  MemRead = 4,
  MemRelease = 5,
  Exec = 6,
  ChanCreate = 7,
  ChanPublish = 8,
  ChanConsume = 9,
  Migrate = 10,
  Telemetry = 11,
  Replicate = 12,
  MemPatch = 13,
  StateDelta = 14,
  StateSync = 15,
  Command = 16,     // run one of the node's own commands, reply code + output
  Sql = 17,         // one statement on the node's own SQL engine, reply the result
  // Writes, one operation, reads - in ONE round trip: a remote job over a
  // relay pays one crossing, not five (a peer advertises it: kFeatExecBatch).
  ExecBatch = 18,
  Error = 0x7F
};
constexpr uint32_t MAX_PAYLOAD = 256u << 20;
// What a node says it speaks, in its handshake (after the kernel lists).
constexpr uint32_t kFeatureMagic = 0x54414546u;   // 'FEAT'
constexpr uint32_t kFeatExecBatch = 1u << 0;
inline bool write_frame(hal::Connection& c, uint8_t t, ByteSpan payload) {
  // One send: a node frame is one transport write, so a peer polling with a
  // zero timeout never sees a header without its payload.
  ByteBuffer frame(9 + payload.size());
  std::memcpy(frame.data(), MAGIC, 4); frame[4] = t;
  uint32_t len = (uint32_t)payload.size();
  for (int i = 0; i < 4; ++i) frame[5 + i] = uint8_t(len >> (8 * i));
  if (!payload.empty()) std::memcpy(frame.data() + 9, payload.data(), payload.size());
  return c.send_all(frame);
}
inline bool read_frame(hal::Connection& c, uint8_t& t, ByteBuffer& payload, int timeout_ms) {
  uint8_t hdr[9];
  // A zero-timeout poll starts only once a whole header is present - and
  // from the moment it is consumed the reader is committed to the frame: the
  // payload may still be arriving (a 17 KB request comes in pieces over a
  // relay, and on a polled board it arrives as the board pumps), so it gets
  // a real wait. Reading it with timeout 0 gave up after eating the header,
  // left the stream out of step, and the session never answered again.
  if (timeout_ms == 0 && c.pending() < 9) return false;   // pending() pumps; no whole header yet
  if (!c.recv_all(MutableByteSpan(hdr, 9), timeout_ms == 0 ? 2000 : timeout_ms)) return false;
  if (std::memcmp(hdr, MAGIC, 4) != 0) return false;
  t = hdr[4];
  uint32_t len = 0; for (int i = 0; i < 4; ++i) len |= uint32_t(hdr[5 + i]) << (8 * i);
  if (len > MAX_PAYLOAD) return false;
  payload.resize(len);
  return len == 0 || c.recv_all(payload, timeout_ms == 0 ? 30000 : timeout_ms);
}
}  // namespace aios::nodeproto
