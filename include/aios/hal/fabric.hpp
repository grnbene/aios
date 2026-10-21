#pragma once
// The AOS fabric: reliable byte streams between kernels directly over Ethernet
// frames (ethertype 0x88B5), no IP, no TCP, no host stack. A FrameDevice is
// what a NIC driver provides; the Fabric multiplexes sessions over it and
// presents hal::Listener / hal::Connection so the node layer is unchanged.
#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "aios/hal/net.hpp"

namespace aios::hal {

using Mac = std::array<uint8_t, 6>;
std::string mac_str(const Mac& m);
bool parse_mac(const std::string& s, Mac& out);

class FrameDevice {
 public:
  virtual ~FrameDevice() = default;
  virtual const Mac& mac() const = 0;
  virtual bool send(const uint8_t* frame, size_t len) = 0;        // a whole Ethernet frame
  virtual int recv(uint8_t* buf, size_t cap) = 0;                 // > 0 = frame length, 0 = nothing pending
  virtual const std::string& name() const = 0;
};

struct FabricStats { uint64_t frames_in = 0, frames_out = 0, retransmits = 0, sessions = 0; };

class Fabric {
 public:
  Fabric(FrameDevice& dev, uint64_t (*now_ns)());
  // Called from whoever owns the CPU: receives pending frames and dispatches them.
  void pump();
  Result<std::unique_ptr<Listener>> listen(uint16_t port);
  Result<std::unique_ptr<Connection>> connect(const Mac& peer, bool broadcast, uint16_t port, int timeout_ms);
  const FabricStats& stats() const { return stats_; }
  std::string describe_sessions() const;   // fabric diagnostics for the host command surface
  void note(const std::string& e) { if (log_.size() >= 40) log_.pop_front(); log_.push_back(e); }
  std::deque<std::string> log_;
  FrameDevice& device() { return dev_; }
  uint64_t now_ns() const { return now_(); }
  struct Session;
  struct Waiting;
  // internal, used by the connection/listener classes
  bool send_frame(const Mac& dst, uint8_t flags, uint32_t session, uint16_t port, uint32_t seq, uint32_t ack, const uint8_t* payload, size_t len);
  void close(uint32_t session);
  std::map<uint32_t, std::shared_ptr<Session>> sessions_;
  std::map<uint16_t, std::deque<std::shared_ptr<Session>>> pending_;   // accepted-but-not-yet-taken, per port
  std::vector<uint16_t> listening_;
  FrameDevice& dev_;
  uint64_t (*now_)();
  FabricStats stats_;
  uint32_t next_session_ = 0x100;
};

}  // namespace aios::hal
