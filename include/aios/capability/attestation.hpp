#pragma once
// Hardware attestation: what this machine can prove about itself, signed by a
// key it cannot export.
//
// Everything here is TPM 2.0 on the wire. A machine with no TPM produces no
// quote and says why — it does not produce one anyway, which is what the code
// this replaced did: PCR digests were SHA-256 over the literal string
// "TPM2_PCR_<n>_BOOT_MEASUREMENT_LOG_SHA256", and the "RSA-2048 / ECC P-256
// quote signature" was that digest XORed with the loop index.
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::capability {

struct PcrValue {
  uint32_t index = 0;
  std::array<uint8_t, 32> digest{};   // SHA-256 bank
};

struct TPMQuote {
  // The attestation structure the TPM signed, byte for byte. Everything below
  // is parsed out of it; this is what a verifier must check the signature over.
  std::vector<uint8_t> attested;
  std::vector<uint8_t> signature;      // ECDSA r||s, 32 bytes each for P-256
  std::vector<uint8_t> signer_public;  // 0x04 || X || Y, the AK that signed
  std::vector<PcrValue> pcrs;          // the PCRs the quote covers, as read
  std::array<uint8_t, 32> pcr_digest{};// the digest inside the attestation
  std::vector<uint8_t> nonce;          // qualifying data the caller supplied
  uint64_t clock = 0;                  // the TPM's own clock, from the quote
  uint32_t reset_count = 0;
  uint16_t algorithm = 0;              // TPM_ALG_ID of the signature
  bool verified = false;               // the signature checked against signer_public
};

class HardwareAttestor {
 public:
  HardwareAttestor();
  ~HardwareAttestor();

  // Opens the machine's TPM and asks it what it is. Safe to call repeatedly.
  Result<void> attach();
  bool available() const { return available_; }
  // One line naming what the root of trust actually is, or why there is none.
  std::string describe_root_of_trust() const;

  // Reads the SHA-256 bank of these PCRs, as the TPM holds them right now.
  Result<std::vector<PcrValue>> read_pcrs(const std::vector<uint32_t>& indices);
  // Bytes from the TPM's own generator — the cheapest proof a TPM is answering.
  Result<std::vector<uint8_t>> random(uint16_t bytes);

  // A real quote: a primary restricted signing key in the endorsement
  // hierarchy signs an attestation over the named PCRs and the caller's nonce.
  Result<TPMQuote> generate_tpm2_quote(const std::vector<uint32_t>& pcr_indices = {0, 1, 2, 3, 7},
                                       const std::vector<uint8_t>& nonce = {});
  // Checks a quote against itself: the magic, the nonce, the signature over
  // the attestation, and that the PCR digest inside it is the digest of the
  // PCRs reported beside it.
  Result<bool> verify_quote(const TPMQuote& quote, const std::vector<uint8_t>& expected_nonce = {});
  // The same check with no TPM involved: a verifier is usually a machine that has none.
  // The PCR digest the TPM signed, read from the attested bytes (empty if malformed).
  static std::vector<uint8_t> signed_pcr_digest(const TPMQuote& quote);
  static Result<bool> verify_quote_standalone(const TPMQuote& quote, const std::vector<uint8_t>& expected_nonce = {});

  const std::string& manufacturer() const { return manufacturer_; }
  const std::string& firmware() const { return firmware_; }

 private:
  bool available_ = false;
  std::string where_;
  std::string manufacturer_;
  std::string firmware_;
  std::string why_not_;
};

}  // namespace aios::capability
