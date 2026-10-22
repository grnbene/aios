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

What the program can see of the machine: `sys` (`os`, `arch`, `cpus`,
`memory`, `host`, `version`), `args`, files relative to the working directory
(`read_file`, `write_file`, `append_file`, `list_dir`, `delete_file`,
`file_exists`), `sys_exec(command)` through the system shell and
`exec([program, args...])` without one, TCP/UDP sockets and a small HTTP-style
server (`listen`, `accept`, `read_until`, `write`).

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
- the `grain` command itself (`grain/tests/cli`)

After a deliberate change in behaviour, rewrite the expectations with
`cmake --build build-grain --target grain-update-expected` and review the diff
before committing it.

## Inside aios

`aios grain run|eval|repl|dis|serve` is the same language with aios's natives
added (`install_builtins` = `install_core_builtins` + `install_aios_builtins`):
the sidecar's C ABI, the SAT solver, the MariaDB wire client and ASQL. A
standalone grain has the portable standard library only.

## Status

| | macOS arm64 | macOS x86_64 | Linux x86_64 | Linux arm64 | Windows x64 |
|---|---|---|---|---|---|
| interpreter | yes | yes | yes | yes | yes |
| JIT | not yet (needs `MAP_JIT`) | no backend yet | no backend yet | yes (checked under qemu by tools/grain-jit/check.sh) | no backend yet |

Built and tested so far: macOS arm64 (47 of 47 cases). The others are set up
(this README, `.github/workflows/grain.yml`, `scripts/build-linux.sh`) and not
yet built.
