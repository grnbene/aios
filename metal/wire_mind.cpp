// The wire mind: the frame protocol (AOSX magic, type, u32 length, payload)
// spoken over the second UART (COM2, 0x2F8). On the other end of that wire sits
// whatever the operator connected — under QEMU, `-serial unix:...` with the
// Assay bridge behind socat (metal/mind-wire.py); on hardware, a cable. The
// kernel sees an Intelligence and nothing else.
#include "aios/intelligence/intelligence.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/core/bytes.hpp"

extern "C" uint64_t metal_tsc_hz();
extern "C" uint64_t metal_rdtsc();

namespace aios {
namespace {

constexpr uint16_t COM2 = 0x2F8;
inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
inline uint8_t inb(uint16_t port) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

enum FrameType : uint8_t { F_Hello = 1, F_Perceive = 2, F_Think = 3, F_Act = 4, F_Error = 0x7f };

struct Wire {
  bool ready = false;
  void init() {
    if (ready) return;
    outb(COM2 + 1, 0x00); outb(COM2 + 3, 0x80); outb(COM2 + 0, 0x01); outb(COM2 + 1, 0x00);   // 115200 (QEMU ignores the rate)
    outb(COM2 + 3, 0x03); outb(COM2 + 2, 0xC7); outb(COM2 + 4, 0x0B);
    ready = true;
  }
  void put(uint8_t b) { while (!(inb(COM2 + 5) & 0x20)) {} outb(COM2, b); }
  int get(uint64_t deadline) { while (metal_rdtsc() < deadline) { if (inb(COM2 + 5) & 1) return inb(COM2); } return -1; }
  bool write_frame(uint8_t t, ByteSpan p) {
    const char* m = "AOSX"; for (int i = 0; i < 4; ++i) put((uint8_t)m[i]);
    put(t); uint32_t n = (uint32_t)p.size(); for (int i = 0; i < 4; ++i) put((uint8_t)(n >> (8 * i)));
    for (auto b : p) put(b);
    return true;
  }
  Result<ByteBuffer> read_frame(uint8_t& t, uint64_t timeout_ms) {
    uint64_t deadline = metal_rdtsc() + (metal_tsc_hz() / 1000) * timeout_ms;
    uint8_t hdr[9];
    for (int i = 0; i < 9; ++i) { int c = get(deadline); if (c < 0) return Result<ByteBuffer>(Status::IoError, i == 0 ? "no reply on the wire within " + std::to_string(timeout_ms) + " ms (is a mind attached to the second serial port?)" : "torn frame header on the wire"); hdr[i] = (uint8_t)c; }
    if (hdr[0] != 'A' || hdr[1] != 'O' || hdr[2] != 'S' || hdr[3] != 'X') return Result<ByteBuffer>(Status::Corrupt, "bad magic on the wire");
    t = hdr[4];
    uint32_t n = (uint32_t)hdr[5] | ((uint32_t)hdr[6] << 8) | ((uint32_t)hdr[7] << 16) | ((uint32_t)hdr[8] << 24);
    if (n > (8u << 20)) return Result<ByteBuffer>(Status::Corrupt, "oversized frame on the wire");
    ByteBuffer p; p.reserve(n);
    for (uint32_t i = 0; i < n; ++i) { int c = get(deadline); if (c < 0) return Result<ByteBuffer>(Status::IoError, "torn frame on the wire"); p.push_back((uint8_t)c); }
    return p;
  }
  Result<ByteBuffer> exchange(uint8_t t, ByteSpan payload, uint64_t timeout_ms) {
    write_frame(t, payload);
    uint8_t rt = 0;
    auto r = read_frame(rt, timeout_ms);
    if (!r.ok()) return r;
    if (rt == F_Error) return Result<ByteBuffer>(Status::IoError, "the mind on the wire reports: " + string_of(*r));
    if (rt != t) return Result<ByteBuffer>(Status::Corrupt, "the mind on the wire replied with the wrong frame type");
    return r;
  }
};

class WireIntelligence : public Intelligence {
 public:
  WireIntelligence(Wire w, std::string name) : w_(w), name_(std::move(name)) {}
  std::string name() const override { return name_; }
  Result<StateID> perceive(Surface& s, const StateObject& context) override {
    auto reply = w_.exchange(F_Perceive, context.payload, 30000);
    if (!reply.ok()) return reply.error();
    return s.create_state(StateKind::Context, *reply);
  }
  Result<GraphID> think(Surface&, const StateObject& perceived) override {
    auto reply = w_.exchange(F_Think, perceived.payload, 600000);   // a model may take its time
    if (!reply.ok()) return reply.error();
    thought_ = *reply;
    return GraphID{};
  }
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph*, const ExecutionReport*) override {
    ByteBuffer req; Writer w(req); w.bytes(perceived.payload); w.bytes(thought_);
    auto reply = w_.exchange(F_Act, req, 30000);
    if (!reply.ok()) return reply.error();
    return s.create_state(StateKind::Working, *reply);
  }
 private:
  Wire w_;
  std::string name_;
  ByteBuffer thought_;
};

}  // namespace

Result<std::unique_ptr<Intelligence>> make_wire_intelligence() {
  Wire w; w.init();
  auto hello = w.exchange(F_Hello, ByteSpan{}, 5000);
  if (!hello.ok()) return hello.error();
  return std::unique_ptr<Intelligence>(new WireIntelligence(w, "wire:" + string_of(*hello)));
}

}  // namespace aios
