# Web Engine JavaScript Compatibility & Triage

Evaluates the in-tree web engine (`aios-web-pane-drive`) against client-rendered and JavaScript-intensive sites rendered at 1400x900 resolution (`frags` denotes the laid-out DOM fragment count).

| Site | Status | Details / Current Blocker |
|---|---|---|
| MDN CSS float | Renders (448 frags) | No blockers observed |
| github.com/torvalds/linux | Renders (779 frags) | Page title flips to generic GitHub title after client-side soft navigation |
| old.reddit.com | Partial (4 frags, 0 links) | Bot/consent interstitial displayed rather than engine rendering failure |
| news.ycombinator.com/item?id=1 | Renders (82 frags, 41 links) | Fully functional |
| stackoverflow.com/questions/11227809 | Blocked | Cloudflare verification challenge ("Just a moment...") |
| npmjs.com/package/left-pad | Minimal (0 frags, title only) | Client-side hydration exception |
| docs.python.org/3/ | Renders (195 frags) | Fully functional |
| react.dev | Partial (754 frags) | Next.js client-side exception during hydration (`Application error`) |
| vuejs.org | Renders (256 frags) | Fully functional |
| svelte.dev | Renders (311 frags) | Fully functional |
| tailwindcss.com | Renders (420 frags) | Fully functional |
| wikipedia Special:Random | Renders (443 frags) | Random redirect handled as expected |
| duckduckgo.com/html/?q=aios | Renders (129 frags, 101 links) | Fully functional |
| Preact counter (`scripts/js-web/g5-preact-counter.html`) | Pass | Local standalone interactive Preact bundle verified |

## Resolved Issues

- **Deadlock on Image Decode (`tools/web-pane-drive.cpp`)**: Step 3b previously held the page mutex while calling `frame()`, which attempted to acquire the same non-recursive mutex. This caused drive timeouts on pages containing decoded images. Resolved by releasing the lock before presentation frames.

## Next Investigation Areas

- **Hydration Exceptions (`react.dev`, `npmjs.com`)**: Trace missing DOM / web runtime APIs during client-side hydration scripts.
