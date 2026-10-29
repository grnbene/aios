// A face's coverage, inflated the first time it is drawn with (font_atlas.hpp). The generated atlas keeps each
// blob deflated; this is the one place that turns it back into the bytes a Glyph's `off` indexes.
#include "aios/userland/font_atlas.hpp"

#include <cstring>

#include "aios/net/deflate.hpp"

namespace aios::userland::fonts {

const uint8_t* Face::cov() const {
  if (const uint8_t* have = __atomic_load_n(slot, __ATOMIC_ACQUIRE)) return have;
  uint8_t* made = new uint8_t[cov_n ? cov_n : 1];
  // The atlas is generated and checked (tools/font-atlas-check.cpp), so this does not fail; if it ever did, the
  // face draws blank rather than reading past what was inflated.
  if (!aios::net::inflate_raw_exact(ByteSpan(covz, covz_n), made, cov_n)) std::memset(made, 0, cov_n);
  const uint8_t* none = nullptr;
  // Two threads drawing the same face at once both inflate it; the first one kept is the one every caller sees.
  if (!__atomic_compare_exchange_n(slot, &none, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    delete[] made;
    return none;
  }
  return made;
}

}  // namespace aios::userland::fonts
