#pragma once
#include <string>

namespace aios::js {
std::string number_to_ascii(double d);                 // Number::toString(10)
std::string number_to_radix(double d, int radix);      // Number.prototype.toString(radix), radix 2..36
bool is_js_whitespace(char16_t c);                     // WhiteSpace and LineTerminator
bool parse_decimal_ascii(const std::string& s, double* out);   // StrDecimalLiteral, no surrounding space
}  // namespace aios::js
