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
`SDL_VIDEODRIVER=dummy`.

Windows need SDL2 when grain is built (`brew install sdl2`, `apt install
libsdl2-dev`, `vcpkg install sdl2:x64-windows-static`); without it grain builds
the same and `run_panes()` says it has no display.

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
grain build app.gr -o app                       # one executable: the runtime with app.gr packed onto it
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
- **Programs with windows**: a program that opens windows needs SDL2 wherever it
  runs, unless grain was built with `-DGRAIN_SDL2_STATIC=ON`, which compiles
  SDL2 2.32.10 from its release and links it in. Use that for releases.

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
`tools/grain-desktop/check.sh` on the aios desktop). Linux and Windows are set
up (`grain/CMakeLists.txt`, `scripts/build-linux.sh`) and not yet built.
