// The console's text embedding is the machine's: one implementation, in
// userland, so what `store similar` searches with is what a surface's
// aos.store.put stored. (It used to live here alone, and a page's records
// would have been embedded by a different function than the query.)
#include "cli.hpp"
#include "aios/userland/web_pane.hpp"

namespace aios::cli {
std::vector<float> embed_text(const std::string& text) { return userland::text_embedding(text); }
}  // namespace aios::cli
