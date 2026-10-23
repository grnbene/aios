# grain

A small, fast, embeddable language: closures, first-class functions, lists and
maps, integers that stay integers, a standard library with files, processes,
sockets, JSON, hashing and a 2D canvas, and an AArch64 JIT. It is aios's own
language and also runs on its own on macOS, Linux and Windows.

```grain
fn fib(n) {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}
print("fib(30) =", fib(30), "on", sys.os)
```

## Using it

```
grain hello.gr [args...]     run a program (args holds what follows)
grain repl                   read and run lines until EOF or .quit
grain eval "1 + 2 * 3"       run one line and print its value
grain check *.gr             compile only and report every error
grain dis hello.gr           show the bytecode the compiler emitted
grain --steps 1000000 ...    stop a program after that many instructions
grain --no-jit ...           interpret everything
grain --safe ...             native fn on bytes only: no ptr, extern fn, ffi or plugins
```

Exit status: 0 the program ran to its end, 1 it did not compile, 2 it stopped
on a runtime error, 64 the command line was wrong. A first line starting with
`#!` is a comment, so `#!/usr/bin/env grain` makes a script executable.

## The standard library

| Area | Functions |
|---|---|
| The machine | `sys` (`os`, `arch`, `cpus`, `memory`, `host`, `version`), `args`, `getenv`, `setenv`, `cwd`, `sleep`, `clock`, `time_us` |
| Files | `read_file`, `write_file`, `append_file`, `list_dir`, `delete_file`, `file_exists` (relative to the working directory) |
| Paths and directories | `path_join`, `basename`, `dirname`, `extname`, `path_normalize`, `abspath`, `mkdir`, `rmdir`, `is_dir` |
| Processes | `sys_exec(command)` through the system shell, `exec([program, args...])` without one: `{ok, code, output}` |
| Network | `http_get`, `http_post`, `http_request` (HTTP and HTTPS with certificate verification, redirects); TCP/UDP sockets; a small server (`listen`, `accept`, `read_until`, `write`) |
| Text | `format("{:>8.2f}", x)`, `regex_test`, `regex_match`, `regex_find_all`, `regex_replace`, `regex_split` (ECMAScript patterns), `upper`, `lower`, `trim`, `split`, `join`, `replace`, `pad_left`, ... |
| Data | `json_parse`, `json_stringify`, `base64_*`, `hex_*`, `url_*`, `sort_by`, `group_by`, `map`, `filter`, `reduce`, `zip`, `enumerate`, ... |
| Time | `now`, `date_format`, `date_parse` (ISO 8601), `date_parts`, `date_make` - computed by grain, so a date prints the same on every OS |
| Digests and ids | `sha256`, `sha1`, `sha512`, `sha384`, `md5`, `keccak256`, `hmac_sha256`, `uuid` |
| Errors | `error(message)`, `pcall(fn, args...)` -> `{ok, value, error}` |
| Graphics | a 2D canvas: shapes, gradients, text, images, effects, widgets; saved as PPM or bytes |
| Windows | `create_pane({title, width, height, on_draw, on_event, on_click, on_key})`, then `run_panes()` |
| Widgets | `ui.column`, `ui.row`, `ui.text`, `ui.button`, `ui.field`, `ui.table`, `ui.tile` in a `ui.pane` |

## Live data

Write the data, write the view, and the screen follows the data. Nothing in the
program sends, polls, listens or says how to update.

```grain
let seconds = 0
every(1, fn () { seconds = seconds + 1 })       # the data changes on its own

ui.pane({"title": "Live", "shared": true, "view": fn () {
  return ui.tile("Seconds up", seconds)          # the view just reads it
}})
run_panes()
```

`every(seconds, fn)` and `after(seconds, fn)` make data that changes by itself
(`cancel(id)` stops one; `wait(seconds)` sleeps while they fire, for a script with
no window). Whatever runs the program fires them: a window's frame loop, `grain
serve`, aios-webserver. A window redraws every frame, so a change shows at once.
For a browser or a native client the server does the watching: when the data
behind a view changes - a timer, another viewer pressing a button, anything - the
widgets are re-rendered, and if the screen is different every viewer is sent it
(server-sent events, to a browser and to `grain open` alike; a native client asks
only when a server will not stream). A change that does not alter the screen sends nothing.

A pane made with `"shared": true` is one copy of the program for everyone, so
every viewer sees the same live data and one viewer's change reaches the others
as it happens (`examples/live.gr` is a clock and a message board that does just
that). Without it each visitor has a copy of their own, as before.

A view that reads the clock instead of data (an uptime computed inside `view`)
has nothing that changes, so give the pane `"refresh": n` and the server looks
again every n seconds. `examples/live.gr` is the model: data that changes, a
view that shows it.

## Streams a server pushes

```grain
let id = events("https://example.com/feed", {"headers": {"authorization": token}}, fn (data) {
  latest = json_parse(data)          # each event's data, on the program's own thread
})
events_close(id)
```

`events` holds a server-sent-events connection on a thread of its own (HTTP or
HTTPS, chunked or not) and hands each event's data to the function as it
arrives, through the same loop that fires timers - a window's frames, `grain
serve`, the web server, or `wait()`. A dropped connection is made again a second
later; a refused one is reported once, as `{"error": ...}`. `grain open` follows a
served program this way: the server pushes the new widgets and the window draws
them, with nothing asked.

## Databases, and live query results

`examples/ops_board.gr` is the whole idea in one program: an operations board over
ASQL queries that every window and browser keeps current as `examples/ops_feed.gr`
plays a busy store. The board has no code that sends, polls or listens.

![The live operations board](../screenshots/grain-ops-board.png)

```grain
let db = db_connect("127.0.0.1", 3307, "erp", password, "versioneight")   # ASQL, MariaDB or MySQL
let r = db_query(db, "SELECT stockid, description FROM stockmaster LIMIT 5")
r.rows        # [["A-1", "Widget"], ...]   (strings; nil for NULL)
r.records     # [{stockid: "A-1", description: "Widget"}, ...]

let late = live(db, "SELECT orderno, debtorno FROM salesorders WHERE late = 1")
ui.table(late.records)    # stays current; nothing else to write
```

`db_connect` speaks the MySQL wire protocol itself (`mysql_native_password`, and
`caching_sha2_password`'s fast path), with no client library. `live(db, sql)`
returns a map whose `rows`, `records`, `columns`, `error` and `changes` are
rewritten in place whenever the query's answer changes, so any view reading it
redraws, in a window or in every browser at once. Against ASQL a tick is one
`ASQL_CHANGES()` lookup over the tables the query reads (guessed from its FROM
and JOINs, or given as `live(db, sql, {"tables": [...]})`); the query runs only
when one of them was written. Against another server it runs every tick
(`every`, default 1 second). `live.runs` counts how often the query itself ran.

## One program, a window or a web page

A program built from `ui.pane` is written once and runs both ways. The pane's
`view` describes the screen as widgets; a window paints them, and a browser
gets them as HTML.

```
grain counter.gr                   a window (counter.gr: ui.pane + run_panes())
grain serve counter.gr             the same program in a browser, at http://127.0.0.1:8080/
grain build counter.gr -o counter  one file: ./counter is a window, ./counter --web [port] a web page
grain open https://host/app.gr     a program running on a server, drawn in a native window (no HTML)
```

In the browser each visitor gets their own copy of the program (a session, kept
by a cookie; or one shared copy, with `"shared": true`): a button or Enter calls the same grain function it would in a
window, with the field texts, on the server, and the page redraws. State is a
session's own and lives in the program's variables. Your functions, `ui.set_field`
and `ui.field_text` behave as they do in a window. A pane option `"refresh": 5`
makes the server look again every 5 seconds, for a view that reads a clock
instead of data (see Live data). Edit the file and the next page
load starts a session on the new code.

`grain serve` is for your own machine: it binds 127.0.0.1, shows a program's
errors in the page, and the program has everything the standalone grain has.
`aios-webserver` serves the same program to the world with `grain_ui app.gr` in
a vhost ([docs/network.md](../docs/network.md)): files confined to the vhost's
root, errors only in the log, gzip, TLS.

A pane drawn by hand (`create_pane` with `on_draw`) goes to a browser as
pictures: the server draws each frame, sends the ones that changed (as PNG, up to
ten a second while someone watches), and the page's clicks, wheel and keys come
back as the pane's own events (`examples/bounce.gr`); a drag sends the moves
between. `grain open` shows the same pictures in a native window and sends that
window's mouse and keys back the same way. `image_decode(data)` makes a canvas
from PNG, JPEG, GIF or BMP bytes. The page script is a few dozen lines inline, with no
library. The cookie is `HttpOnly` and `SameSite=Strict`, text from the program
is escaped, and an event must carry `X-Grain-UI: 1`, which a page on another
site cannot add, so a link elsewhere cannot press your buttons.

## Going low-level

Four layers, from the safest up. The standard library has the first; the
standalone `grain` program adds the rest (a sandboxed host, such as the aios
desktop or the web server, gets none of them unless it opts in).

### Bytes

`bytes` is a fixed-size run of mutable bytes, shared by reference like a list.
Indexing, `len`, `slice`, `+`, `for` and `==` work on it; every access is
bounds-checked.

```grain
let b = bytes(16)                          # 16 zero bytes; also bytes("text"), bytes([1, 2, 3])
bytes_set(b, 0, "u32be", 0xCAFEBABE)       # a number at an offset, in a byte order
bytes_get(b, 0, "u16le")                   # types: u8 i8 u16le u16be i32le ... u64be f32le f64be
let p = pack(">HH4s", 80, 443, "grain")    # like Python's struct: b B h H i I q Q f d s x, < or >
unpack(">HH", p)                           # -> [80, 443]
bytes_fill(b, 0)  bytes_copy(dst, 0, src)  bytes_find(b, "needle")  bytes_hex(b)  bytes_concat([a, b])
read_bytes("file.bin")  write_bytes("out.bin", b)
```

A `u64` above 2^63 reads as its negative twin: grain's integers are signed
64-bit.

### Native functions

`native fn` compiles a typed slice of grain to machine code - the inner loop you
would otherwise write in C. About 20x faster than the VM on recursive code, and
it follows the C calling convention, so one native function calls another.

```grain
native fn checksum(b: bytes) -> i64 {
  let h = 0
  for i in 0..b.len { h = (h * 31 + b[i]) & 0xFFFFFFFF }
  return h
}
print(checksum(bytes("hello world")))
```

Types are written out and nothing converts silently: `i64`, `f64`, `bool`,
`bytes` (and `ptr`, below). `x as f64` converts. Arithmetic wraps; shifts by 64
or more shift everything out. Statements: `let`, assignment and `+= -= ...`,
`if / else if / else`, `while`, `for i in a..b` (b excluded), `break`,
`continue`, `return`. Expressions: `+ - * / % & | ^ ~ << >>`, comparisons, `and
or not`, `b[i]` (a byte), `b.len`, `sqrt(x)`, `fabs(x)`, and typed access at an
offset: `rd8 rd16 rd32 rd64 rdi8 rdi16 rdi32 rdf64` and `wr8 wr16 wr32 wr64
wrf64` (little-endian, in native byte order). A native function can call
itself and the ones declared above it. It takes and returns numbers and bools;
a `bytes` argument is the caller's buffer, so it can fill one in place.

What stops the program with an error, never the process: division by zero, an
index or `rd`/`wr` outside the bytes, a byte outside 0..255, and recursion past
256 KB of native stack. Not covered: native code does not count against
`--steps`, so a native `while true {}` is a real infinite loop.

The compiler reports mistakes the way grain does, with the line:

```
bad.gr:1: the two sides of an operator: i64 and f64 do not mix (convert with `as`)
bad.gr:3: an if wants a bool, got i64 (compare it, e.g. `x != 0`)
```

x86-64 on Linux and macOS only so far; elsewhere `native fn` says so.

**Unsafe**, which `--safe` turns off: `ptr` (an address), `rd`/`wr` through a
`ptr`, `x as ptr`, and `extern fn name(a: i64, b: f64, s: ptr) -> i32`, which
declares a C function from the running program for native code to call
(`i32` is allowed only there). `grain --safe prog.gr` allows `native fn` on
bytes and nothing else; `--no-native` refuses it altogether.

### Calling C from grain

```grain
let libc = ffi_open()                                  # the program itself, and so libc; or ffi_open("libm.so.6")
let strlen = ffi_fn(libc, "strlen", "u64(cstr)")
print(strlen("grain"))                                 # 5
let snprintf = ffi_fn(libc, "snprintf", "int(bytes, u64, cstr, ...)")
```

Types: `void i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 ptr cstr bytes`. `cstr` is a
string in and a string out; `bytes` passes the address of a buffer the C code may
fill. `...` ends a variadic signature. `ffi_sym`, `ffi_ptr`, `ffi_errno`,
`mem_alloc`, `mem_free`, `mem_read`, `mem_write`, `cstr_at`, `bytes_ptr` cover
addresses. A wrong signature crashes the process, as in C. No structs by value,
callbacks into grain, or Windows yet; x86-64 and AArch64 (no variadic calls
on Apple Silicon).

### Plugins in C and C++

Include `include/grain_plugin.h`, build a shared library, and `load_plugin`:

```c
#include "grain_plugin.h"
static void add(grain_call* c) { grain_return_int(c, grain_arg_int(c, 0) + grain_arg_int(c, 1)); }
GRAIN_PLUGIN(reg) { grain_register(reg, "add", 2, 2, add); return 0; }
```

```
c++ -O2 -shared -fPIC -I grain/include vec.cpp -o libvec.so
grain: load_plugin("./libvec.so")   # registers add(); returns the names
```

A plugin is handed a function table, so it needs no symbols from grain and
survives upgrades (`GRAIN_PLUGIN_ABI`). It can read numbers, strings, bytes
(the caller's buffer, writable) and lists, return numbers, strings and bytes, and
raise errors. `examples/plugin/vec.cpp` is a working one.

## Windows

A pane is a window: its `on_draw(canvas, pane)` paints a frame (about 60 a
second), and input arrives as events. `run_panes()` returns when every pane has
been closed - by its window, or by the program with `unregister_pane`.

```grain
let n = 0
create_pane({
  "title": "Counter", "width": 240, "height": 120,
  "on_draw": fn (cv) {
    canvas_clear(cv, "#1e1e2e")
    canvas_fill_text(cv, "clicked " + str(n) + " times", 20, 60, "#ffffff")
  },
  "on_click": fn (p, e) { n = n + 1 },
  "on_key": fn (p, e) { if e.key == "escape" { unregister_pane(p.id) } }
})
run_panes()
```

Events: `mouse_down`, `mouse_up`, `click` (`x`, `y`, `button`), `mouse_move`,
`wheel` (`dx`, `dy`), `key` and `key_up` (`key`, `shift`, `ctrl`, `alt`,
`meta`), `text` (`text`, what was typed), `resize` (`width`, `height`) and
`close`. Coordinates are the pane's own, on any screen density.
`run_panes({frames: n, script: [events...]})` stops after n frames and plays
the scripted input first - the window test does this headless with
`GRAIN_VIDEO=dummy` (no display is opened; `SDL_VIDEODRIVER=dummy` works too).

Windows on Linux need nothing: grain speaks the X11 protocol itself (it also
works on Wayland desktops, through XWayland), so there is no library to install.
On Windows grain calls the Win32 API directly (user32 and gdi32, which are part
of Windows): no library there either, and typed text is what the keyboard's
layout types, Cyrillic, AltGr and characters outside the BMP included.
`scripts/grain-win-check.sh` cross-compiles grain with MinGW and drives a real
window under Wine. On macOS grain still uses SDL2 when it is built with it
(`brew install sdl2`); without it grain builds the same and `run_panes()` says
it has no display. Linux typed text is the key's
own character in the active keyboard layout (the display's layout switch, e.g.
Alt+Shift, picks it; shift, caps lock and AltGr apply, and Cyrillic, Greek,
Arabic, Hebrew and the other layouts' keys type their own letters). Several
keyboards work at once, and a keyboard or layout change is picked up while the
program runs. There are no input methods, so compose sequences and CJK input
are not supported.

### Widgets

`ui.pane` is a pane whose `view` returns widgets instead of painting pixels.
It is called every frame; face (the aios desktop's own layout and drawing) lays
out what it returns, so a program describes what is on the screen from its
data and never computes a coordinate.

```grain
let items = []
fn add(f) {
  if f.sku == "" { return nil }
  push(items, {"sku": f.sku, "qty": int(f.qty)})
  ui.set_field(p, "sku", "")
}
let p = ui.pane({"title": "Stock", "width": 640, "height": 420, "view": fn () {
  return ui.column({"gap": 12}, [
    ui.row({"gap": 12}, [ui.tile("Items", len(items))]),
    ui.row({"gap": 8}, [
      ui.field("sku", {"placeholder": "SKU", "focus": true, "on_enter": add}),
      ui.field("qty", {"value": "1", "width": 80}),
      ui.button("Add", add, {"primary": true})]),
    ui.table(items, {"columns": "sku:SKU, qty:On hand:right"})])
}})
run_panes()
```

| Builder | Options |
|---|---|
| `ui.column([children])`, `ui.column(options, [children])`, `ui.row` the same | `gap`, `pad` |
| `ui.text(words, options)` | `size`, `dim`, `bold`, `color` |
| `ui.button(label, fn, options)` | `primary` |
| `ui.field(id, options)` | `placeholder`, `value` (its first text), `width`, `focus`, `on_enter` (a function) |
| `ui.table(rows, options)` | `columns` (`"key:Header, key:Header:right, key:Header:badge"`; every field when left out), `rows` (how many show), `empty`, `badges` (value -> accent for a `badge` column) |
| `ui.tile(label, value, options)` | `accent` (`ok`, `warn`, `bad` or a colour) |
| `ui.heading(words, options)` | `size` |
| `ui.badge(words, options)` | `accent`, `dot` (a leading dot) |
| `ui.progress(value, options)` / `ui.gauge(value, options)` | `max` (100), `min` (0), `label`, `accent`, `prefix`, `suffix`; a progress bar shows the percentage, a gauge the value |
| `ui.spark(numbers, options)` / `ui.chart(numbers, options)` | `height`, `min`, `max`, `accent`, `prefix` (the chart's axis) |
| `ui.bars(rows, options)` | a bar chart: a map of label -> number or `[label, number]` pairs, each bar against the largest; `accent`, `prefix`, `suffix`, `gap` |

What is typed into a field stays across frames by its id. A button's function
and a field's `on_enter` are called with the pane's field texts as a map when
they take one argument (`fn (f) { f.sku }`). `ui.field_text(pane, id)` and
`ui.set_field(pane, id, text)` read and replace a field; `ui.layout(pane)` says
where the last frame put each widget. An error in `view` or in a widget's
function stops `run_panes` with it, naming the button or field.

Strings are bytes: `len`, indexing, `slice`, `find` and regex offsets count
bytes of UTF-8.

https verifies certificates against `ssl/ca-certificates.crt` in grain's
per-user directory (`~/Library/Caches/grain`, `~/.cache/grain`,
`%LOCALAPPDATA%\grain`) when you put one there, otherwise the system's store
(on Windows, exported there from the system's root certificates).

## Tools

```
grain test [file | dir]...        run every *_test.gr: each test on its own, failures with their message
grain fmt [--check] <file | dir>  lay source out the one grain way (in place; --check only lists)
grain lsp                         a language server: diagnostics, formatting, symbols, completion
```

A test file registers tests and states what must hold:

```grain
test("parses ISO dates", fn () {
  expect_eq(date_format(date_parse("2026-10-22T14:05:09Z"), "%F", "utc"), "2026-10-22")
})
test("refuses nonsense", fn () { expect_error(fn () { return date_parse("soon") }, "ISO 8601") })
```

`grain fmt` indents two spaces a bracket level (brackets opened together share
one), hangs a continued expression one level, trims trailing space and long
runs of blank lines, and never touches strings or comments. It rewrites a file
only when the result compiles to exactly the same program, so it cannot
change what a program does; one that does not compile is reported and left alone.

`grain lsp` works with any editor's LSP client. For Neovim (0.10+):

```lua
vim.filetype.add({ extension = { gr = "grain" } })
vim.api.nvim_create_autocmd("FileType", { pattern = "grain", callback = function()
  vim.lsp.start({ name = "grain", cmd = { "grain", "lsp" } })
end })
```

For Helix (`languages.toml`):

```toml
[language-server.grain]
command = "grain"
args = ["lsp"]

[[language]]
name = "grain"
scope = "source.grain"
file-types = ["gr"]
comment-token = "#"
language-servers = ["grain"]
```

## Shipping a program

```
grain build app.gr -o app                       # one executable: the runtime with app.gr packed onto it (./app --web serves a ui.pane program to a browser)
grain build app.gr -o app --include assets      # files the program reads with bundle_file("assets/x.png")
grain build app.gr --app -o App.app             # macOS: an application bundle that can be signed
grain build app.gr --runtime grain.exe -o app   # for another OS: pack onto that OS's grain
```

The program is compiled first, so a build never ships one that does not
compile. The result runs with no grain installed: every argument goes to the
program (`args`). `bundle_file(name)` reads a packed file, `bundle_files()`
lists them. A pack carries a SHA-256, and a damaged one is refused rather
than run.

- **Windows and Linux**: the single file is the thing to ship; sign the .exe
  with signtool after building (the runtime finds its program in front of the
  signature).
- **macOS**: a single file runs as built, but appended bytes fail Apple's
  strict signature check, so it cannot be notarized. `--app` leaves the
  runtime untouched and puts the program in `Contents/Resources`; sign and
  notarize the .app as any other.
- **Programs with windows**: on Linux and Windows nothing is needed beyond the
  file. On macOS a program that opens windows needs SDL2 wherever it runs, unless
  grain was built with `-DGRAIN_SDL2_STATIC=ON`, which compiles SDL2 2.32.10
  from its release and links it in. Use that for releases.

## Building

Needs CMake 3.20 and a C++20 compiler: GCC 10, Clang 12 (Apple Clang 13) or
Visual Studio 2019 16.10 or newer. No other dependencies.

```
cmake -S grain -B build-grain -DCMAKE_BUILD_TYPE=Release
cmake --build build-grain --config Release --parallel
ctest --test-dir build-grain -C Release --output-on-failure
```

The binary is `build-grain/grain` (`build-grain\Release\grain.exe` with Visual
Studio). Run the commands from the root of the aios checkout: grain builds from
the same sources as aios (`src/userland/grain_*`, the host layer in `src/hal`),
so there is one language, never two copies.

- **macOS**: Xcode's command line tools are enough.
- **Windows**: a "Developer PowerShell for VS" prompt; the C runtime is linked
  in, so `grain.exe` needs no redistributable on the machine it runs on.
- **Linux, portable**: on a machine with Docker,
  `grain/scripts/build-linux.sh` builds inside the manylinux2014 image (glibc
  2.17, GCC 10, libstdc++ linked in) and runs the tests there. The binary in
  `grain/dist/` starts on every distribution since CentOS 7.
  `ARCH=aarch64` builds for 64-bit Arm.
- **Linux, local**: the commands above, with GCC 10 or newer. (Ubuntu 20.04's
  default GCC is 9: use `CXX=g++-10`, or the portable build.)

## Tests

`grain/tests` holds golden cases. Each one runs a program and compares its
output and exit status with `grain/tests/expected/`, ignoring lines that report
timings:

- the language's examples (`examples/grain`), with the JIT and with `--no-jit`
- the native-code comparison cases (`tools/grain-jit/cases`)
- the `grain` command itself and the standard library (`grain/tests/cli`)
- the HTTP client against a local server (`grain/tests/net`, needs Python 3);
  `grain run grain/tests/net/tls_live.gr` checks HTTPS against real sites by hand

After a deliberate change in behaviour, rewrite the expectations with
`cmake --build build-grain --target grain-update-expected` and review the diff
before committing it.

## Inside aios

`aios grain run|eval|repl|dis|serve` is the same language with aios's natives
added (`install_builtins` = `install_core_builtins` + `install_aios_builtins`):
the sidecar's C ABI, the SAT solver, the MariaDB wire client and ASQL. A
standalone grain has the portable standard library only.

On the aios desktop, `grain open file.gr` at the terminal runs a program with
windows: its panes are desktop windows (moved, resized, closed like any
other), drawn by its `on_draw` and fed its input. The program is the same as
in the standalone grain, with one difference: `run_panes()` returns at once,
because the desktop, not the program, runs the loop - code after it runs, and
the program lives while it has panes. Its relative paths start at its own
directory. A callback is held to 50 million instructions, so a runaway one
stops with an error in its window instead of freezing the desktop.
`tools/grain-desktop/check.sh` drives this headless.

## Status

| | macOS arm64 | macOS x86_64 | Linux x86_64 | Linux arm64 | Windows x64 |
|---|---|---|---|---|---|
| interpreter | yes | yes | yes | yes | yes |
| JIT | not yet (needs `MAP_JIT`) | no backend yet | no backend yet | yes (checked under qemu by tools/grain-jit/check.sh) | no backend yet |

Built and tested so far: macOS arm64 (57 of 57 cases, a signed .app passing
strict verification, the live HTTPS checks, the window test on a real macOS
window with both Homebrew's sdl2-compat and SDL 2.32, and
`tools/grain-desktop/check.sh` on the aios desktop). Linux x86_64 (Ubuntu 24.04, GCC 13): 58 of 58 cases; windows are drawn through
the X11 protocol with no library (checked on a real X server: typing, clicks,
resize, close), and `grain build` makes a windowed app
(`grain/examples/sysmon.gr`) that runs with no grain installed and links no
library but libc and libstdc++. Windows is set
up (`grain/CMakeLists.txt`) and not yet built.
