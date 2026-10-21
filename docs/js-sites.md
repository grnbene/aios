# JS-built sites triage (agent G5)

Measured with `aios-web-pane-drive` at 1400x900 (fragments = laid-out fragment count).
Owners of the big areas: JS language=S1, DOM=S2, network APIs=S3, layout=S4. Add messages to them below.

| Site | Status | First blocker |
|---|---|---|
| MDN CSS float | renders (448 frags) | none seen |
| github.com/torvalds/linux | renders (779 frags) | title flips to the generic GitHub title after a soft nav; not yet reduced |
| old.reddit.com | thin (4 frags, 0 links) | not yet diagnosed; likely bot/consent interstitial rather than engine |
| news.ycombinator.com/item?id=1 | renders (82 frags, 41 links) | none |
| stackoverflow.com/questions/11227809 | blocked | Cloudflare "Just a moment..." challenge (needs real JS challenge solving) |
| npmjs.com/package/left-pad | empty (0 frags, title only) | client-rendered; first script error not yet read |
| docs.python.org/3/ | renders (195 frags) | none |
| react.dev | partial (754 frags) | page title becomes "Application error: a client-side exception has occurred" (Next.js): a client JS exception in hydration; exact missing API not yet captured |
| vuejs.org | renders (256 frags) | none |
| svelte.dev | renders (311 frags) | none |
| tailwindcss.com | renders (420 frags) | none |
| wikipedia Special:Random | renders (443 frags) | title changes to "Wikipedia, the free encyclopedia" on a later step (random redirect), expected |
| duckduckgo.com/html/?q=aios | renders (129 frags, 101 links) | none |
| offline Preact counter (`scripts/js-web/g5-preact-counter.html`) | PASS | none |

## Fixed
- `tools/web-pane-drive.cpp` step 3b held the page mutex while calling `frame()`, which takes the same non-recursive
  mutex: the drive hung forever on any page with a decoded image (every real site above timed out). Not an engine bug.

## Open
- react.dev, npmjs.com: capture the first script exception (needs the drive to print it) and file the missing API here.
