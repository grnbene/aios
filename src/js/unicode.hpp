#pragma once
// The slice of Unicode the engine carries in its own tables: case mapping, canonical and compatibility normalisation
// for the scripts the web uses most (Latin, Greek, Cyrillic, Armenian, Hangul, fullwidth forms), and coarse character
// classes. The product has no ICU; outside these ranges characters map to themselves.
#include <cstdint>
#include <string>

namespace aios::js::uni {

// Full case mapping of one code point: up to three code points (e.g. U+00DF -> "SS"). Returns false when the
// character has no mapping (out[0] == cp, *n == 1).
bool to_upper(uint32_t cp, uint32_t out[3], int* n);
bool to_lower(uint32_t cp, uint32_t out[3], int* n);
// Single-code-point mappings, used by regular expressions' case-insensitive matching.
uint32_t simple_upper(uint32_t cp);
uint32_t simple_lower(uint32_t cp);
// Simple case folding (what /i and String.prototype.localeCompare's primary comparison use).
uint32_t fold(uint32_t cp);

std::u16string to_upper_string(const std::u16string& s);
std::u16string to_lower_string(const std::u16string& s);

// form: 0 NFC, 1 NFD, 2 NFKC, 3 NFKD.
std::u16string normalize(const std::u16string& s, int form);

// Coarse general categories, enough for \p{L} \p{Lu} \p{Ll} \p{N} \p{Nd} \p{P} \p{S} \p{Z} \p{M}.
enum class Cat : uint8_t { Other, Lu, Ll, Lt, Lm, Lo, Mn, Mc, Me, Nd, Nl, No, Pc, Pd, Ps, Pe, Pi, Pf, Po, Sm, Sc, Sk, So, Zs, Zl, Zp, Cc, Cf, Cs, Co, Cn };
Cat category(uint32_t cp);
bool is_id_start(uint32_t cp);
bool is_id_continue(uint32_t cp);

// UTF-16 helpers.
inline bool is_high_surrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool is_low_surrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
// The code point at s[i]; *len is 1 or 2.
uint32_t code_point_at(const std::u16string& s, size_t i, int* len);
void append_code_point(std::u16string& s, uint32_t cp);

}  // namespace aios::js::uni
