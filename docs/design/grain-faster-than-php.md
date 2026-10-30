# grain faster than PHP

## Where it stands (2026-10-30, this machine, PHP 8.3.6 CLI with opcache, no JIT; grain Release with its JIT)

The same nine workloads in both, median of three runs, ms:

| workload | grain | PHP | |
|---|---|---|---|
| fib(27) | 16 | 12 | 1.3x slower |
| sort 200k ints | 53 | 59 | faster |
| 200k records, grouped | 170 | 156 | 1.1x slower |
| JSON 50k round trip | 120 | 79 | 1.5x slower |
| 10M-step loop | 265 | 177 | 1.5x slower |
| split/trim 5 MB | 135 | 42 | 3.2x slower |
| 1M concat + join | 325 | 112 | 2.9x slower |
| regex replace 100k | 130 | 28 | 4.6x slower |
| HTML table 20k rows (`el()`) | 108 | 8 | 13x slower (29 ms built as strings) |

Per million single operations:

| | grain | PHP |
|---|---|---|
| empty loop pass | 13 ms | 4 ms |
| a builtin call (`len(x)`) | 54 ms | 8 ms |
| `"x" + s` | 89 ms | 21 ms |
| `str(i)` | 111 ms | 28 ms |
| `push(l, v)` | 104 ms | 28 ms |

## Why

Sampled with `GRAIN_PROFILE=native` (perf is not allowed on this machine):

1. **A value is 32 bytes and every object is a `std::shared_ptr`** (`include/aios/userland/grain.hpp`, `Value`).
   Every push, local read and argument copies 32 bytes and touches a control block. The x64 JIT pokes
   libstdc++'s `shared_ptr` layout to retain and release inline; half the time of a `len(x)` loop is that
   generated code.
2. **A string is two allocations**: the `make_shared<StrObj>` block, and the `std::string`'s buffer past 15
   bytes. glibc malloc/free is 20-35% of the string workloads (`_int_malloc`, `unlink_chunk`,
   `malloc_consolidate`). PHP's zend_mm hands out size classes from its own pages.
3. **A builtin call goes through the generic path**: JIT -> `jit_call` -> `Vm::invoke` -> arguments moved into
   a pooled vector -> the function -> the stack resized -> the result pushed. About 40 ns; PHP's is about 4.
4. **Some builtins do more than they need**: `el()` makes a map per node with three string keys,
   `html_stringify` looks them up again; regex converts every text to UTF-16 before matching;
   JSON parsing builds each string a character at a time.

Done already (f5d2d60, 1667480): numbers to text with `to_chars` (concat -15%), and `for x in a..b` no
longer builds the list.

## The plan, in stages that each ship alone, measured against PHP before the next starts

**Stage 1 - builtins (days).** `el` / `html_stringify` (a node as a small struct-backed object, not a map
with string keys), `split` / `trim` / `join` (one allocation per piece, reserve), regex on ASCII text without
the UTF-16 copy, JSON strings parsed by span. Target: HTML 3x, split 2x, regex 2x, JSON 1.3x faster.

**Stage 2 - the call path (days).** The JIT calls a builtin directly when the callee is a native, with the
arguments read where they sit on the stack (a span, not a vector the values are moved into). Every builtin
keeps its signature through an adapter, then moves over. Target: a builtin call under 15 ns.

**Stage 3 - the value (weeks; the big one).** A 16-byte value (tag + payload) with an intrusive, non-atomic
reference count in `Obj` instead of `shared_ptr` (VMs never share objects across threads: workers get deep
copies, which is already the rule), and strings of up to 14 bytes held inside the value. Touches the VM, the
compiler's constants, both JITs' templates, the FFI/plugin ABI and every builtin that takes `str()` by
reference (a `string_view` accessor first, then the switch). Target: concat, records, JSON at or past PHP.

**Stage 4 - the allocator (days, after 3).** Size-class pools per thread for objects and string buffers,
giving whole empty pages back to the OS on the app host's existing trim tick (`return_free_memory`), so the
memory work of 2026-10-29 (sessions reaped, memory returned) still holds.

**Stage 5 - the JIT (weeks).** Integer and real locals kept in registers inside loops, guarded by type, with
a deopt to the interpreter when a guess fails. Target: loops and fib past PHP.

## Reliability, measured, not asserted

- A soak: one app (JSON API + HTML page + a DB-backed route on a local ASQL) under the same load for hours in
  grain and in PHP (php-fpm), comparing errors, p99 latency and memory over time.
- Fuzzing the parsers that take outside input: JSON, the HTML/XML parser, regex patterns, HTTP heads, the
  WebSocket frame reader.
- Every stage above runs both JITs' equivalence checks (`tools/grain-jit/check.sh`: same output, no leaks)
  and the whole suite with `GRAIN_NOJIT=1`.

## Risks

- Stage 3 changes what every native sees. It is staged (accessor first, then representation) so each step
  builds and passes alone, and it is the one stage to stop and review before starting.
- The JITs hard-code `Value`'s layout (asserted at 32 bytes in `grain_jit_x64.cpp`); stage 3 rewrites their
  templates for both x64 and arm64 (the Pi), and arm64 needs a Pi to test on.
