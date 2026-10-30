# Writing Grain applications

An app is a directory with your Grain code and an `app.gr` file that tells the host how to run it.

```text
hello/
├── app.gr         # what the app is, ports, settings, hooks
├── main.gr        # where it starts
├── lib/
│   └── words.gr   # shared helper code
└── check.gr       # integration check before cutover
```

To test it locally:
```bash
grain app run ./hello
```

When you are ready to ship it:
```bash
grain app deploy ./hello
```

Related guides:
- [Grain standard library reference](grain-reference.md): complete function reference for all builtins.
- [Running and managing Grain applications](grain-apps.md): deployments, settings, secrets, and rollbacks.
- [Hosting applications with AIOS](aios-serve.md): running with native ASQL and fleet orchestration.

---

## The manifest (`app.gr`)

The manifest sets the app name, port, default settings, and hooks. It must assign a map to `let app`:

```grain
let app = {
  "name": "hello",
  "main": "main.gr",
  "kind": "web",
  "port": 18601,
  "listen": "local",
  "settings": {
    "GREETING": "hello"
  },
  "secrets": [
    "SIGNATURE"
  ],
  "health": {
    "path": "/",
    "ok": [200],
    "timeout": 30
  },
  "check": "check.gr",
  "deployed": "deployed.gr"
}
```

Only `name` is required. The rest have defaults:

| Field | What it does | Default |
|---|---|---|
| `name` | App name (letters, numbers, `-`, `_`) | *required* |
| `description` | Short explanation | `""` |
| `main` | File to run | `"main.gr"` |
| `kind` | `"web"` (answers HTTP) or `"worker"` (background loop) | `"web"` if `port` is set, else `"worker"` |
| `port` | TCP port for web apps | required for web apps |
| `listen` | `"local"` (`127.0.0.1`) or `"all"` (`0.0.0.0`) | `"local"` |
| `settings` | Default environment variables | `{}` |
| `secrets` | Secret names injected at runtime | `[]` |
| `requires` | Host features needed: `["sql"]`, `["files"]`, etc. | `[]` |
| `health` | Health check probe: `path`, `ok` status list, `timeout` (seconds) | `path: "/"`, any status below 500, 30s |
| `check` | Script run against the staging port before live cutover | `""` |
| `deployed` | Script run after the release is live | `""` |
| `restart` | `"always"`, `"on-failure"`, or `"never"` | `"always"` |
| `max_steps` | Instruction limit per request (`0` for none) | `200000000` |
| `max_body` | Max request body size in bytes | 16 MB |

If your app lists features in `requires`:
- `grain host` provides: `grain`, `http`, `db`, `files`, `process`.
- AIOS adds: `aios`, `sql`, `fleet`.

If the host is missing a required feature, `grain app deploy` rejects the deployment upfront.

---

## Web services (`main.gr`)

Use `serve_routes()` to map paths or method-prefixed paths to handler functions:

```grain
import "lib/words.gr" as words

let start = now()

serve_routes({
  "GET /": fn (req) {
    return res_json({
      "greeting": words.greeting(),
      "uptime_s": round(now() - start)
    })
  },

  "GET /items/:id": fn (req) {
    let id = req.params?.id
    return res_json({"item_id": id, "found": true})
  },

  "POST /quote": fn (req) {
    let payload = req.json
    if type(payload) != "map" or not has(payload, "items") {
      return res_error(400, "Invalid payload: 'items' array required")
    }
    return res_json({"quote_id": uuid_v4(), "items": payload.items}, 201)
  },

  "GET /signed": fn (req) {
    return res_json({
      "signed": getenv("SIGNATURE", "(unconfigured)"),
      "authenticated": req.headers?.authorization != nil
    })
  }
})
```

### Routes

A route key is a path (`"/items"`), or a method and a path (`"GET /items"`, `"POST /quote"`). A path can have parts that change: `"GET /items/:id"` matches `/items/42` and hands you `42` as `req.params.id`; `*name` matches the rest of the path.

### The request map (`req`)

Every handler receives a `req` map:
- `req.method`: uppercase method (`"GET"`, `"POST"`, etc.)
- `req.path`: request path (`"/items"`)
- `req.params`: the `:id` parts picked out of the path
- `req.query`: raw query string (`"limit=10"`); `req.query_params` is the same thing already split into a map
- `req.body`: raw body string
- `req.json`: the body already parsed, when it is JSON (use `req.body` and `pcall(json_parse, req.body)` if you want to handle bad JSON yourself)
- `req.headers`: headers with lowercased keys (`req.headers["authorization"]`); `req.headers?.authorization` gives `nil` instead of an error when the header is missing
- `req.remote`: client IP address

### Responses

Return any map, list, or text and Grain sends it as JSON with status 200. For anything more, use the helpers:

```grain
return res_json({"ok": true, "items": [1, 2, 3]})                  # 200
return res_json(quote, 201, {"X-Quote-Id": uuid_v4()})             # status and extra headers
return res_error(400, "Invalid payload: 'items' array required")   # {"error": "...", "status": 400}
```

`res_text`, `res_html` and `res_redirect` work the same way. If you would rather build the response yourself, return a map with `status`, `content_type` and `body` (or `json`):

```grain
return {
  "status": 201,
  "content_type": "text/csv",
  "body": "sku,qty\nA-1,42\n"
}
```

### Middleware

Pass a list of functions as the second argument to `serve_routes` to run code around every route. Each takes `(req, next)` and either answers itself or returns `next(req)`. They run outermost first:

```grain
serve_routes(routes, [
  fn (req, next) {                       # outermost: every response gets the header
    let res = next(req)
    if not has(res, "headers") { res.headers = {} }
    res.headers["X-Served-By"] = "store-api"
    return res
  },
  fn (req, next) {                       # inside it: refuse what has no key
    if req.headers?.authorization != "Bearer " + getenv("API_KEY") {
      return res_error(401, "unauthorized")
    }
    return next(req)
  }
])
```

A 401 returned by the second still passes back out through the first, so it carries the header too.

### Server-sent events and live streaming

`res_sse([{"event": "ping", "data": "pong"}, {"data": {"status": "ok"}}])` answers with `text/event-stream` and those events, formatted and sent as one complete response. It suits a short list you already have. A newline in an `id`, `event` or `retry`, or a stray `\r` in `data`, cannot add a field or an event, and a plain string is always data.

For live streaming responses (such as continuous SSE feeds, long-running progress reports, or log tails), route handlers can accept a `writer` object (`fn (req, writer)` or access `req.writer`):

```grain
serve_routes({
  "GET /feed": fn (req, writer) {
    writer.write_sse({"event": "status", "data": "starting"})
    sleep(100)
    writer.write_sse({"event": "progress", "data": 50})
    sleep(100)
    writer.write_sse({"event": "done", "data": 100})
  },

  "GET /logs": fn (req) {
    req.writer.write("tail line 1\n")
    sleep(100)
    req.writer.write("tail line 2\n")
  }
})
```

The writer object supports:
- `writer.write_sse(event)`: Formats and pushes Server-Sent Events over the open connection.
- `writer.write(chunk)`: Sends a raw body chunk immediately.
- `writer.write_head(status, [headers])`: Sends an explicit HTTP status and headers before data chunks.
- `writer.header(key, val)` and `writer.status(code)`: Set headers or status before the first write; after it they are ignored, because the status line has already gone out.
- `writer.event(name, [data])`: A shorthand for an SSE event with that name; with one argument it is sent as data.
- `writer.close()`: Closes the stream. Writing after it is ignored.
- `writer.is_closed()`: Returns `true` once the client has disconnected or the stream was closed. A long-running handler should check it: `while not writer.is_closed() { ... }` stops when the reader goes away, where a handler that does not check keeps working until it finishes.

A streamed response has no length, so the connection closes when the handler returns. If the handler fails after it has started writing, the client sees the stream end and the error is in the server's log; the status line cannot be taken back.

### WebSockets

A route can answer its own request as a WebSocket with `ws_accept(req)`, then talk both ways with `ws_send` /
`ws_recv` (see the reference). The connection is the route's until the handler returns; then it is closed.

```grain
serve_routes({
  "GET /live": fn (req) {
    let ws = ws_accept(req)
    while true {
      let m = ws_recv(ws, 15000)               # nil when nothing came in 15 s
      if m == nil { ws_send(ws, "still here"); continue }
      if m.type == "close" { break }
      ws_send(ws, "you said " + m.data)
    }
  }
})
```

`ws_accept` refuses a request that is not a WebSocket upgrade, so one route can answer both. The app host (`grain
host`, `aios serve`) lends routes their connection; a proxy in front (nginx) has to pass `Upgrade` and `Connection`
through for it to reach the app.

### How requests run across workers

Each incoming request runs in a worker VM on its own thread:
- The handler gets a copy of top-level variables and functions as they were when the request arrived.
- Any variables the handler modifies stay inside that worker and won't affect other requests.
- Sockets and database connections cannot cross threads. Open your database handle (via `db_connect()` or `sql()`) inside the handler, not as a global.
- If routes need to mutate shared in-memory variables sequentially, pass `{"workers": false}`:
  ```grain
  serve_routes(routes, {"workers": false})
  ```

---

## Language ergonomics and syntax

### String interpolation (`$"..."`)

Format strings with interpolated expressions using `$"..."`:
```grain
let name = "Grain"
let balance = decimal("12345.67")

print($"Welcome to {name}!")
print($"Current balance: {balance:,.2f}")
```
Format specifiers like `{expr:spec}` desugar directly to Grain's exact formatter `format("{:spec}", expr)`.

### Optional chaining (`?.`)

Safely navigate nested maps or objects without throwing errors if an intermediate key is missing or `nil`:
```grain
let auth = req?.headers?.authorization
let city = user?.profile?.address?.city
```
If any segment evaluates to `nil`, the expression evaluates to `nil`.

### Destructuring assignments

Unpack maps and lists into local or global variables:
```grain
# Map destructuring
let { id, sku, price } = get_item()

# Map destructuring with renaming
let { sku: item_sku, price: unit_price } = get_item()

# List destructuring
let [ head, next ] = [10, 20, 30]
```

### Module imports and namespacing

Keep imported symbols isolated and avoid global namespace collisions:
```grain
# Namespaced import: symbols accessible via namespace map
import "lib/store.gr" as store
let quote = store.calculate_totals(items, rate)

# Selective import: unpack only desired functions
import { calculate_totals, format_quote } from "lib/store.gr"
```

---

## Background workers

If an app doesn't take incoming HTTP traffic, set `kind: "worker"` (or leave `port` out of `app.gr`). The host runs `main.gr` as a background loop:

```grain
import "lib/queue.gr" as queue

print("Worker started")

while not is_stopping() {
  queue.process_next()
  sleep(1000)    # milliseconds: sleeps 1 second, unblocking immediately on stop or restart
}

print("Worker received stop signal, shutting down cleanly.")
```

`is_stopping()` (or `should_stop()`) returns `true` when the host initiates a shutdown or deployment rollover. `sleep(ms)` yields gracefully and returns immediately when a stop signal arrives, allowing the worker loop to perform cleanup before the host process terminates.

---

## Standard library reference

### Identifiers, timestamps, and navigation
- **`uuid_v4()`**: Generates an RFC 4122 version 4 UUID string (`"c2b64d30-b991-4cf1-8594-c78216d6118b"`).
- **`iso8601([timestamp])`**: Formats the current UTC time (or a Unix timestamp) as ISO 8601 (`"2026-10-24T16:00:00Z"`).
- **`get(map_or_list, key_or_index, [default_val])`**: Safe lookup returning `default_val` (or `nil`) if not found.
- **`get_in(obj, [keys...], [default_val])`**: Deep nested traversal through maps and lists with default fallback.

### Path utilities
- **`path_join(parts...)`**: Joins directory components with `/`.
- **`path_basename(path)`**: Returns the trailing file or folder name.
- **`path_dirname(path)`**: Returns the parent directory path.
- **`path_ext(path)`**: Returns the file extension (e.g. `".json"`).

### Native test assertions
- **`assert_eq(actual, expected, [message])`**: Asserts equality (`a.equals(b)`), printing failures or throwing on mismatch.
- **`assert_ne(actual, expected, [message])`**: Asserts inequality.
- **`assert_true(cond, [message])`**: Asserts condition is truthy.
- **`assert_false(cond, [message])`**: Asserts condition is falsy.

---

## Settings, secrets, and files

### Reading settings and secrets

Read them with `getenv()`:

```grain
let greeting = getenv("GREETING", "hello")
let db_pass = getenv("DB_PASSWORD")
```

Apps run in an isolated environment: `getenv()` sees only the settings and secrets declared for that app, not the host machine's shell variables.

For machines where passwords live in files on disk:
```grain
fn get_cred(name) {
  let v = getenv(name)
  if v != nil and v != "" { return v }
  let f = getenv(name + "_FILE")
  if f != nil and f != "" and file_exists(f) {
    return trim(read_file(f))
  }
  return ""
}
```

### Where to put files

Relative file operations (`read_file("cache.json")`, `write_file("data.db", b)`) resolve inside the app's persistent data directory: `~/.grain/apps/<name>/data/`. This directory stays intact across upgrades and rollbacks.

### Logs

Call `print(...)` anywhere. The host supervisor captures stdout and stderr and writes them to `~/.grain/apps/<name>/log`. Follow them live with:

```bash
grain app logs hello -f
```

---

## Multi-file projects and imports

Use `import` to bring in other files:

```grain
import "lib/formatting.gr"
import "lib/calculations.gr"
```

Paths resolve relative to the file doing the importing. When you deploy, the host finds all imported files and bundles them into the release package automatically.

### Inlining files with `app_program()`

If you need to turn a file and its imports into a single standalone script string—such as when defining an ASQL Thing or sending code over the fleet:

```grain
let thing_code = app_program("things/stock_item.gr")
```

`app_program()` reads the file, recursively inlines every `import` into one self-contained string, and returns it.

A Thing's program runs inside ASQL with only the pure builtins (lists, text, numbers, decimals, patterns, markup),
the engine's `read` / `read_raw` / `write` family, and what it and its modules define - no clock, no files, no
network. `grain app check` applies that rule to every file under `things/` (and the modules they import) and
refuses the app with the line and the name: `things/x.gr:10: 'now' is not available in a Thing (pure builtins
only: docs/things.md)`. A deploy applies it the same way, so a Thing that would fail in `deployed.gr` never leaves
your machine.

---

## Pre-cutover checks (`check.gr`)

Before swapping ports on a new deployment, the host runs your `check.gr` against the staging port. The file must export a `check(app)` function:

```grain
fn check(app) {
  # app.url is the temporary staging address, e.g. "http://127.0.0.1:42183"
  let res = http_get(app.url + "/")
  if res.status != 200 {
    return "GET / returned " + str(res.status)
  }
  if not contains(res.body, "greeting") {
    return "Missing greeting field"
  }

  # Test unauthorized requests get blocked
  let auth = http_post(app.url + "/admin", "{}", {"headers": {"Content-Type": "application/json"}})
  if auth.status != 401 {
    return "Expected 401 on /admin, got " + str(auth.status)
  }

  return true
}
```

- Return `true` if everything is good.
- Return a string with the error if anything is wrong.
- If `check()` returns an error or throws, the host kills the staging process and aborts the deploy. The live release keeps running without a hiccup.

---

## Post-deploy hook (`deployed.gr`)

After the new version passes checks and takes over the live port, the host runs `deployed(app)`:

```grain
fn deployed(app) {
  print("Running migrations for version", app.version)
  let db = db_connect("127.0.0.1", 3307, "app", getenv("DB_PASS"), "appdb")
  db_query(db, "ALTER TABLE items ADD COLUMN IF NOT EXISTS audited DATETIME")
  return true
}
```

Because `deployed(app)` also runs when you roll back to an earlier release, make sure migrations and schema changes are idempotent (`IF NOT EXISTS`, etc.).

---

## Examples in the repo

- [**`examples/apps/hello`**](../examples/apps/hello/README.md): Minimal working web app with settings, secrets, checks, and modular imports.
- [**`examples/apps/api`**](../examples/apps/api/README.md): REST API service demonstrating route handling, exact decimal financial math, custom status codes, and pre-cutover testing.
- [**`examples/apps/worker`**](../examples/apps/worker/README.md): Long-running background worker loop with configuration settings, persistent state files, and graceful shutdown.
- [**`apps/stickerfeed`**](../apps/stickerfeed/README.md): Production catalog feed service using ASQL Things, bearer auth, and live stock queries.
