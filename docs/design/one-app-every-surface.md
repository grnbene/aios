# One app, every surface — design (stage 1 in progress)

Status: proposal, 2026-10-27. Stage 1 is in: markup views (`grain/tests/web/markup_test.py`), a whole pane given to a
fleet member (`ui.pane` `"machine"`, `scripts/pane-machine-check.sh`), and the warehouse page as a pane view.

An app declares what it is and what it shows once; the runtime puts it in a browser, a desktop window or a PDF. Today the same report is three programs' worth of plumbing.

## Today

| Surface | How an app reaches it | Where |
|---|---|---|
| Browser | `kind: "web"` + a `port`; routes return HTML strings; the page's own JS (sort, `EventSource`, clock) | `grain_app.cpp:187-189` (only `web` or `worker`), `apps/warehouse_touches/lib/views.gr` |
| Live updates | either the web UI's own push (a `ui.*` view re-rendered after an event, a timer or `refresh` seconds, sent to `/` as a frame and morphed into the page, `grain_webui.cpp:1266-1315`, `kScript`), or a hand-written SSE route (`/quickreportevents`: `WAIT THING`, `writer.event`, page JS swapping `innerHTML`) | |
| Desktop window | a `ui.*` view drawn by `fc::Runtime` (`grain_face.cpp:1250` `build`), or a `.aos` face; the compositor's app list is fixed (`compositor.cpp:1734`) | |
| PDF | a second rendering of the same table as an HTML string for `doc.pdf` (`render_pdf_report`) | |

A `ui.pane` with routes and no `view` is marked `api_only` (`grain_face.cpp:1320-1327`): `/` answers 404 and the
web UI's push is skipped (`grain_webui.cpp:1429, 1501, 1568`). A view must return `ui.*` widgets (`html_widget`,
`grain_face.cpp:725`); markup is refused. So an app with its own look writes it as routes and loses the web UI's
sessions, actions, push and fleet forwarding of the page.

## What it should be

```grain
ui.pane({
  "title": "Warehouse Activity",
  "css": PAGE_CSS,
  "watch": {"thing": "touch_day"},          // re-render when the Thing changes (stage 2)
  "view": fn() { return dashboard(state) },  // el() nodes and/or ui.* widgets
  "routes": {"/quickreportpdf": export_pdf, "/quickreportxlsx": export_xlsx}
})
```

One node tree (`el`, `html_raw`, `ui.*`) is the view. The browser gets it as HTML through the web UI (sessions,
actions, morph, push); the desktop gets it in a `WebPane` window; `doc.pdf` gets the same tree. Behaviour is
declared on the nodes, not written as page JS.

## Stages

Each stage is its own change, tested on its own, and leaves every app working.

1. **Markup in a view.** A view may return `el()` / `html_raw()` nodes anywhere a widget goes (and widgets inside
   them). Function-valued attributes are actions: `on_click` on any element and `on_change` on an `input` /
   `select` become the web UI's `data-act`; `on_enter` its `data-enter`; `bind: "id"` makes an input a field
   (`data-id`, its value kept across renders, read with `ui.field_text(id)` in an action). The native tree client
   gets `{"type": "html", "html": ...}`. A face window (no browser) shows the node's text until stage 4.
   *Proof:* the warehouse page as a `ui.pane` view: range form, sort by header click (server side, per session),
   same markup as today's page.
2. **Re-render on data.** `"watch": {"thing": name}` on a pane: the host keeps one `WAIT THING` per watched Thing
   (on a connection of its own, as `/quickreportevents` does today) and marks every session of the pane to look
   again when it moves; the existing compare-and-push sends a frame only when the view changed. Replaces the
   hand-written SSE route and the page JS that swaps `innerHTML`. Open: a pane whose data is on another member
   (`WAREHOUSE_COMPUTE_ON`) — the watch runs where the data is and wakes the sessions here over the node link
   (a `WAIT THING ... ON <member>` statement, or the AppStream frames carrying a "changed" event).
3. **The manifest says what the app is.** `kind: "app"`: an app with a view. HTTP is a projection, given when wanted:
   `"serve": {"port": 8573, "listen": "local"}` (today's `port` / `listen` read as that). `web` and `worker` keep
   their meaning, so stickerfeed and every deployed app are unchanged.
4. **The desktop opens apps.** The compositor lists the app host's installed apps (not a fixed `.aos` list) and opens
   one in a window whose surface is a `WebPane` fed by the web UI in process (no socket): the same session, frames
   and actions as a browser tab. Needs `drive.dom` passing first — it fails today ("the script did not run"), at
   9b93888 as well — since the page script (`kScript`, morph) must run in `WebPane`.
5. **Documents from the view.** `doc.pdf(view_node, css, options)` takes the node tree, so the PDF is the page's own
   table, not a second rendering of it.

## Decisions and trade-offs

- **Server-side sort and state.** A sort or a range lives in the session (an action changes it, the view reads it),
  not in page JS. Every surface gets it for free, a pushed update keeps it (the morph keeps rows in the order the
  view gave), and the desktop window needs no script for it. Cost: a round trip per click (local: sub-millisecond;
  remote: one request), and a session per viewer (the web UI already keeps one per cookie, reaped when idle).
- **One VM per viewer.** A non-shared pane runs the program per session, so each viewer's view runs its own report.
  For the warehouse page that is one `SHOW THING touch_day KEYS (...)` and a merge (~45 ms for 30 days). A pane whose
  viewers all see the same thing sets `"shared": true` and renders once.
- **Markup and widgets together.** Not a second UI system: `el` nodes are leaves and containers in the same tree, so
  an app can use `ui.table` where it fits and its own markup where it has a look of its own.
- **No page JS required, not forbidden.** `el("script", ...)` still works in a browser; it is the app's to keep. A
  `script` inside a pushed frame does not run (it arrives by morph), so live behaviour belongs on nodes.

## Out of scope here

Grain-side layout widgets drawn natively for HTML nodes (stage 4 uses `WebPane` instead); changing how `.aos` faces
work; the fleet identity and roles work (`docs/design/fleet-identity-keys.md`).
