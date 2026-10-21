#pragma once
// The frame protocol between the external-intelligence adapter and a peer
// process: a 4-byte magic, 1-byte type, 4-byte little-endian length, payload.
// Binary, schema-free, and entirely outside the kernel.
#include <cstdint>
#include <vector>
#include <string>

namespace aios::frame {
constexpr char MAGIC[4] = {'A', 'O', 'S', 'X'};
enum Type : uint8_t { Hello = 1, Perceive = 2, Think = 3, Act = 4, Error = 0x7F };
constexpr uint32_t MAX_PAYLOAD = 64u << 20;
// Blocking helpers over file descriptors; return false on EOF/error.
bool write_frame(int fd, Type t, const std::vector<uint8_t>& payload);
bool read_frame(int fd, Type& t, std::vector<uint8_t>& payload, int timeout_ms);
}  // namespace aios::frame
