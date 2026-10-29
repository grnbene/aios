#pragma once
// The named character references of HTML, one table for everything that reads HTML: the web engine's
// DOM and the mail reader's text of an HTML message. The HTML 4 set for Latin-1 (every letter with an
// accent a name or address can carry, the currency and typographic signs) plus the common ones from
// HTML 5 (quotes, dashes, ellipsis, arrows, a few Greek letters and symbols). A name not here is left
// as the author wrote it, so `&foo;` shows up as `&foo;` instead of silently vanishing.
#include <cstdint>
#include <string_view>

namespace aios {

// The code point of the named reference `name` (the letters between '&' and ';', case as written).
bool html_named_entity(std::string_view name, uint32_t& cp);

}  // namespace aios
