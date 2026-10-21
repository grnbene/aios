// The TPM on Windows: not reached (that would be the TBS API, Tbsip_*,
// which this does not call). A machine with no TPM this HAL can see says so
// — tpm.hpp's own contract — rather than producing a quote nobody signed.
#include "aios/hal/tpm.hpp"

namespace aios::hal {

Result<std::string> tpm_open() { return Result<std::string>(Status::Unsupported, "TPM access is not implemented on Windows"); }
std::string tpm_where() { return {}; }
Result<std::vector<uint8_t>> tpm_transmit(const std::vector<uint8_t>&, uint32_t) {
  return Result<std::vector<uint8_t>>(Status::Unsupported, "no TPM open");
}
void tpm_close() {}

}  // namespace aios::hal
