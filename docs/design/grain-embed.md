# Grain panes in other pages — design

Status: proposal, 2026-10-28. Nothing built yet.

A Grain pane (`ui.pane`) can only be opened as its own page. The first customer for the warehouse touches report
(`apps/warehouse_touches`) wants it inside `v8/client`, an Angular 15 app, without an iframe. This describes a
`<grain-pane>` custom element that mounts a pane in the host page, and the engine changes it needs.

## How a pane works today (what an embed has to keep)

| Part | Where |
|---|---|
| `GET /` returns a whole document: the engine's CSS, the pane's `css`, the pane's markup in `#app`, and the page script | `grain_webui.cpp:1844-1847` |
| The page script reads `#app` from the document | `grain_webui.cpp:550` |
| An action is a `POST` to `location.pathname + location.search` with `X-Grain-UI: 1`; the answer is `{seq, html, version}`, morphed into `#app` | `grain_webui.cpp:643`, `apply` 629-639, `morph` 568-624 |
| Push: an `EventSource` on the same URL | `grain_webui.cpp:662-663` |
| A newer version of the program reloads the page | `grain_webui.cpp:626` |
| The session is a cookie, `HttpOnly; SameSite=Strict`, read by `session_for` | `grain_webui.cpp:1490, 1564, 1649` |
| `OPTIONS` answers `*` and allows `X-Grain-UI`, but pages and frames carry no `Access-Control-Allow-Origin`, and the CSP says `connect-src 'self'` | `grain_webui.cpp:1625-1630, 1851` |
| A `<script>` in pushed markup does not run (`grain-reference.md:674`); only the scripts in the first page do | the touches page keeps its clock, hour slider, export links and theme switch that way |
| The pane's CSS is global: `:root`, `body`, `h1`, `table`, `.btn` | `apps/warehouse_touches/lib/views.gr` `PAGE_CSS` |

So a page on another origin cannot use a pane today: no CORS on the frames, and the session cookie is not sent
cross-site.

## The design

`<grain-pane src="https://atl-ga-1.tireserv.com/…" query="start=2026-10-01&hours=7-11" theme="dark">`, from one script
the app host serves (`/_ui/embed.js`).

1. **One client, not two.** The page script becomes a module that takes a root element and a base URL. The full page
   and the element both use it, so the two cannot drift.
2. **Shadow DOM for scope.** The pane's CSS goes inside the element's shadow root, with `:root`, `html` and `body`
   rewritten to `:host` when it is served for embedding. The host page's CSS cannot reach the pane, and the pane's
   cannot reach the host's. The host's theme comes in as an attribute.
3. **Base URL.** Every request (open, action, stream) goes to `src`, not to `location`.
4. **A session without a cookie.** In embed mode the first answer returns a session id in the body, and every later
   request sends it in `X-Grain-Session`. Same lifetime and reaping as the cookie session.
5. **Push without `EventSource`.** `EventSource` cannot set headers. The element reads the event stream with `fetch`
   and a stream reader, so it can send the session id and the credential.
6. **CORS by allow-list.** An app setting lists the origins that may embed it. The answer echoes the requesting
   origin with `Vary: Origin`, and allows `X-Grain-UI`, `X-Grain-Session` and `Authorization`. Never `*` with
   credentials.
7. **Auth.** Today atl's nginx checks `key` + `userid` in the URL before the page (`/quickreportauth`). An embed
   sends the same credential on every request, stream included. The pane gets an `"auth": fn(req)` option so the
   app, not only nginx, can refuse a request.
8. **Scripts.** A pane option `"script"`: JavaScript run once when the element mounts, given `root` (the shadow
   root). The touches page's four scripts move into it and use `root` instead of `document`.
9. **Versions.** A new program version re-mounts the element instead of reloading the page.
10. **Host to pane.** The element's attributes become the pane's open query (the existing `on_open(q)`); changing
    one re-opens it. Events out of the pane (a `CustomEvent`) come later.

## Other ways in

| | |
|---|---|
| **iframe** | Works today, no engine work. Own scrolling and theme, the key in the URL. |
| **Same-origin path** | The client's web server proxies `/grain/` to atl. Cookies then work, so 4-6 are not needed for that deployment; 2, 8 and 9 still are. Worth asking whether v8's host can do it. |
| **Native Angular on JSON** | Angular draws the tables from `/quickreport`. Needs a JSON route in atl's nginx and a second copy of the UI. |

## Stages

| | Change | Check |
|---|---|---|
| 0 | This document | read |
| 1 | The page script as a module, page behaviour unchanged. Also: clicks on any element with an action, as `grain-reference.md:668` says. The handler answers `button[data-act]` only (`grain_webui.cpp:649`), so a sortable `<th>` does nothing; that is what broke the touches table sorts. | `grain/tests/web/browser_test.py` in headless Chrome, plus a new case that clicks a `<th>` and fails first |
| 2 | `/_ui/embed.js`, the element, `X-Grain-Session`, fetch-stream push, CORS allow-list, `"auth"` | A page on a second origin mounts the counter app and the live app; an action and a push both arrive |
| 3 | Shadow-root CSS, the `"script"` option, touches converted | The touches pane in that page: sort, pause and live, light and dark, exports |
| 4 | An Angular wrapper component in `v8/client` | In the client; a separate task in that repository |

Each stage is one change that passes before the next starts. Stage 1 alone fixes the `<th>` click for every app.

## Open questions

1. What credential does the client hold for atl: `key` + `userid` as in the URL, or a bearer token?
2. Same-origin proxy path for v8, or cross-origin?
3. Which origins get to embed (the allow-list)?
4. Is Shadow DOM acceptable next to Angular Material? Overlays (date pickers, tooltips) in the pane stay inside its
   root; ones the host opens over it are the host's.

## Risks

- The page script has to keep working as a full page while it is being split: stage 1 is a refactor of code every
  pane runs. Its test has to run before and after.
- Streaming with `fetch` instead of `EventSource` loses the browser's automatic reconnect; the element has to
  reconnect itself, and it has to be tested against atl's nginx (`proxy_buffering off`).
- The pane's markup is built on the server and set as HTML in the host page's DOM. It is the same trust as the page
  today (the app's own output, values escaped), but an embedding page now depends on it.
