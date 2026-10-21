// Reference external intelligence: a separate process speaking the frame
// protocol. Deterministic so the adapter path can be proven anywhere.
#include "aios/intelligence/frame_protocol.hpp"
#include "aios/core/bytes.hpp"
#include <algorithm>
#include <cctype>
#include <string>

using namespace aios;

int main() {
  frame::Type t;
  std::vector<uint8_t> payload;
  while (frame::read_frame(0, t, payload, -1)) {
    switch (t) {
      case frame::Hello: { std::string n = "peer:reverse"; frame::write_frame(1, frame::Hello, ByteBuffer(n.begin(), n.end())); break; }
      case frame::Perceive: frame::write_frame(1, frame::Perceive, payload); break;
      case frame::Think: {
        std::string s(payload.begin(), payload.end());
        std::reverse(s.begin(), s.end());
        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
        frame::write_frame(1, frame::Think, ByteBuffer(s.begin(), s.end()));
        break;
      }
      case frame::Act: {
        Reader r(payload);
        ByteSpan perceived = r.bytes();
        ByteSpan thought = r.bytes();
        std::string out = "peer saw " + std::to_string(perceived.size()) + " bytes; thought: " + string_of(thought);
        frame::write_frame(1, frame::Act, ByteBuffer(out.begin(), out.end()));
        break;
      }
      default: { std::string e = "unknown frame"; frame::write_frame(1, frame::Error, ByteBuffer(e.begin(), e.end())); }
    }
  }
  return 0;
}
