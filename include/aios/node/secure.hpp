#pragma once
// Authenticated encryption for node links: ChaCha20-Poly1305 (RFC 8439) with a
// key derived from the shared node token, fresh per-session keys from both
// sides' randomness, per-direction nonces and strictly increasing sequence
// numbers. No external library; the kernel's native framing runs inside.
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/host.hpp"

namespace aios::crypto {
using Key = std::array<uint8_t, 32>;
using Nonce = std::array<uint8_t, 12>;
void chacha20_xor(const Key& key, const Nonce& nonce, uint32_t counter, uint8_t* data, size_t n);
std::array<uint8_t, 16> poly1305(const std::array<uint8_t, 32>& key, ByteSpan msg);
ByteBuffer aead_encrypt(const Key& key, const Nonce& nonce, ByteSpan aad, ByteSpan plaintext);            // ct || tag
Result<ByteBuffer> aead_decrypt(const Key& key, const Nonce& nonce, ByteSpan aad, ByteSpan ct_and_tag);
Key derive_key(const std::string& token, ByteSpan salt);   // stretched: 4096 ChaCha20 iterations
}  // namespace aios::crypto

namespace aios {
// A frame link: type + payload in, type + payload out. Plain for the handshake,
// secure afterwards. Both sides speak nodeproto frames through it.
class Link {
 public:
  virtual ~Link() = default;
  virtual bool send(uint8_t type, ByteSpan payload) = 0;
  virtual bool recv(uint8_t& type, ByteBuffer& payload, int timeout_ms) = 0;
  virtual std::string peer() const = 0;
  // The peer reads deflated frames (it advertised nodeproto::kFeatDeflate): large frames go compressed from here on.
  virtual void set_compress(bool) {}
};
std::unique_ptr<Link> make_plain_link(std::shared_ptr<hal::Connection> c);
// Wraps a link: every frame becomes one encrypted frame; sequence numbers must increase.
std::unique_ptr<Link> make_secure_link(std::shared_ptr<hal::Connection> c, const crypto::Key& session, bool initiator);
// Handshake (both directions authenticate the token; derives the session key).
// This side's handshake payload feature word (kFeatureMagic + bits) and whether a peer's payload advertises deflate.
ByteBuffer link_feature_payload();
bool link_peer_deflates(ByteSpan payload);
Result<crypto::Key> secure_handshake_initiator(Link& plain, const std::string& token, hal::Host& host, const Id128& my_kernel, Id128& peer_kernel, ByteBuffer& peer_hello_payload, ByteSpan my_hello_payload);
Result<crypto::Key> secure_handshake_responder(Link& plain, const std::string& token, hal::Host& host, const Id128& my_kernel, Id128& peer_kernel, ByteBuffer& peer_hello_payload, ByteSpan my_hello_payload);
Result<crypto::Key> secure_handshake_responder_any(Link& plain, const std::vector<std::string>& tokens, hal::Host& host, const Id128& my_kernel, Id128& peer_kernel, ByteBuffer& peer_hello_payload, ByteSpan my_hello_payload);
}  // namespace aios
