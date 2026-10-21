#pragma once
// The Trusted Platform Module, if this machine has one.
//
// A TPM is reached by handing it a command buffer and reading the response it
// writes back — that is the whole interface, and everything above this line is
// the TPM 2.0 protocol rather than the substrate. A machine with no TPM says
// so; it does not produce a quote anyway.
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::hal {

// Opens the first TPM this substrate can offer. Idempotent.
Result<std::string> tpm_open();
// Where the open TPM is, or empty when none is open.
std::string tpm_where();
// One command in, one response out, exactly as the TPM wrote it (header and
// all — the caller speaks TPM 2.0, not this layer).
Result<std::vector<uint8_t>> tpm_transmit(const std::vector<uint8_t>& command, uint32_t timeout_ms = 30000);
void tpm_close();

}  // namespace aios::hal
