// macOS has no TPM. It has a Secure Enclave, which is not one and does not
// answer TPM 2.0 commands, so this substrate has nothing to open. Saying so is
// the point: the attestor above used to produce a "quote" on any machine.
#include "aios/hal/tpm.hpp"

namespace aios::hal {

Result<std::string> tpm_open() {
  return Result<std::string>(Status::NotFound,
      "this machine has no TPM (macOS exposes a Secure Enclave, which does not speak TPM 2.0)");
}
std::string tpm_where() { return {}; }
Result<std::vector<uint8_t>> tpm_transmit(const std::vector<uint8_t>&, uint32_t) {
  return Result<std::vector<uint8_t>>(Status::NotFound, "no TPM is open");
}
void tpm_close() {}

}  // namespace aios::hal
