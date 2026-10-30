# grain VM 2: faster than PHP on every row

The goal is set by `tools/grain-vs-php/run.sh`: every row of both tables at or under 1.00 (grain / PHP 8.3 with
opcache, no PHP JIT) on this machine and on the fleet's, with the language unchanged, no app changed, and no less
reliable than today. This replaces the stages 3-5 of [grain-faster-than-php.md](grain-faster-than-php.md), which
measurement showed would leave string work 1.5-2x behind.

## Where it starts (2026-10-30, after the quick fixes up to 500d93f)

| workload | grain / PHP | | operation, 1M times | grain / PHP |
|---|---|---|---|---|
| fib27 | 1.45 | | empty loop pass | 4.00 |
| loop10M | 1.26 | | `str(i)` | 3.41 |
| concat1M | 2.89 | | `len(x)` (a builtin call) | 5.86 |
| records200k | 1.36 | | `"x" + s` | 4.00 |
| sort200k | 1.04 | | `push(list, v)` | 3.1-3.4 |
| json50k | 1.68 | | `join` 1M | 1.13 |
| regex100k | 4.96 | | | |
| split5MB | 3.23 | | | |
| html20k | 12.00 | | | |

perf across the workloads: ~30% in JIT code (half of it moving 32-byte values between the value stack and locals,
a sixth `lock`-prefixed reference counting), ~17% glibc malloc/free, ~10% the generic call and interpreter paths,
the rest spread through builtins.

## Why the current VM cannot get there

- **A value is 32 bytes**: a type byte, an 8-byte payload and a `std::shared_ptr<Obj>` (`include/aios/userland/
  grain.hpp`, `Value`). Every push, local read, list slot and argument moves 32 bytes and touches a control block.
- **Reference counts are `shared_ptr`'s**: atomic once the process has a second thread, which an app host always
  has, and the JIT always uses `lock` instructions (`grain_jit_x64.cpp`, `ref_inc`).
- **A string is two allocations** (the shared_ptr block holding a `std::string`, then its buffer past 15 bytes), has
  no cached hash, and is copied to make a substring or a key. PHP's is one allocation with its hash, and constants
  and keys are interned.
- **A map is a vector of `(std::string, Value)` pairs** with an index past 8 entries: every key is a copy of a
  string, every lookup compares bytes.
- **The bytecode is a stack machine**: `a = b + c` is four instructions that each move a 32-byte value; a register
  machine does it in one with no moves.
- **A builtin gets a `std::vector<Value>&`** its arguments are moved into, and each call goes through
  `jit_call -> invoke`.

None of these is a local fix; together they are the gap.

## The design

### Values: 16 bytes

```
struct Value {          // 16 bytes, trivially copyable bits plus a refcount discipline
  uint8_t  tag;         // nil bool int real decimal str-inline str list map fn native ... (as today's Type)
  uint8_t  len;         // an inline string's length (0..14)
  uint8_t  bytes[6];    // inline string bytes 0..5
  union { int64_t i; double d; Obj* obj; char more[8]; };   // or inline string bytes 6..13
};
```

Ints stay 64-bit (no NaN-boxing: grain's ints are int64 and must stay exact). A string of up to 14 bytes is held
in the value itself: `str(i)`, most map keys, most words never allocate. Copying a value is copying 16 bytes and,
for a heap object, one increment.

### Heap objects: an intrusive header, owner-thread reference counts

```
struct Obj { uint32_t refs; uint8_t type; uint8_t flags; uint16_t gc; };   // flags: SHARED, IMMORTAL, TRACKED
```

- `refs` is a plain integer. An object is touched by one thread - the VM that made it - unless it is marked SHARED,
  and then (only then) the count is atomic. The JIT tests the flag byte it already has in cache.
- The places an object reaches another thread are few and named, and each marks what it hands over:
  1. a worker's copy (`grain_std_parallel.cpp`: strings, ids, decimals and natives are shared, everything else is
     copied),
  2. the module cache and the web server's script pool (`grain_builtins.cpp` copies of a compiled function keep its
     constants; `src/net/grain_serve.cpp` lends one script to one request at a time under a lock),
  3. constants of a compiled chunk generally: made IMMORTAL (never counted, freed with the chunk).
- A debug build stores the owner thread in the header and checks it on every count change; the whole suite, the
  JIT checks and the soak run in that build. A crossing nobody marked fails a test instead of corrupting memory.

### Strings

One allocation: header, length, cached hash, bytes. Immutable, as today. Constants and map keys are interned per
VM, so equal keys compare by pointer first. `Value::str()` returns `std::string_view` (today `const std::string&`,
1572 call sites: migrated mechanically, see the stages).

### Maps

An ordered hash in one block, as PHP's array is: entries in insertion order (key, value, hash) and a slot table
after 8 entries. Keys are string values (inline or interned), so a field read with a constant key is a pointer or
16-byte compare. Lists stay a contiguous array of values, now 16 bytes each.

### Memory

Per-thread size classes (16..512 bytes) carved from 64 KB pages, a remote-free list for SHARED objects freed on
another thread, and empty pages given back to the OS from the app host's existing trim tick
(`grain_webui.cpp` `return_free_memory`), so the 2026-10-29 memory work - sessions reaped, memory returned - holds.
Larger blocks go to malloc. The cycle collector keeps its trial deletion, using the header's `gc` bits.

### Bytecode and interpreter

Register bytecode with 32-bit instructions (Lua 5 style): locals and temporaries are registers of the frame,
`ADD r3, r1, r2`, `ADDK r1, r1, k`, `GETFIELD r2, r1, k`, `CALL r0, nargs`. Dispatch by computed goto. Instructions
quicken in place after their first run (`ADD` -> `ADD_II` when both were ints, `GETFIELD` -> with an inline cache of
the slot). The parser and AST are unchanged; the compiler's back end is rewritten.

### Builtins

`Value fn(Vm&, const Value* args, int argc, Err&)`: the arguments are read where they are, in the caller's
registers. The 611 existing natives keep working through an adapter that copies them into a vector, and the hot
ones (len, str, push, join, split, el, the string and map functions, JSON, regex) move first. The C plugin ABI
(`grain/include/grain_plugin.h`) is unchanged.

### JIT

Tier 1: a template JIT over the register bytecode with type guards from the quickened instructions, for x64 and
arm64 (the Pi), registers of the frame in memory, ints and reals unboxed inside a guarded basic block. Tier 2, if
tier 1 leaves loops short: ints and reals kept in machine registers across a loop, with a deopt to the interpreter
when a guard fails. `native fn` stays as it is.

## Stages

Each stage builds, passes everything below, is no slower than the stage before on `run.sh`, and is committed alone.
The checks every stage runs: build-grain ctest (all `.nojit` variants), the engine's grain/apps/things/web tests,
`tools/grain-jit/check.sh` on x64 (arm64 on the Pi from stage 8), `grain app check` on every app, `run.sh`.

| # | Stage | Size |
|---|---|---|
| 0 | `tools/grain-vs-php` (done, 156df82); a differential runner: every test program and app check under the old and the new build, outputs compared | days |
| 1 | `Value::str()` -> `std::string_view` and the call sites, nothing else changing | 1-2 weeks |
| 2 | The `Obj` header and intrusive counts in place of `shared_ptr` (still 32-byte values), SHARED/IMMORTAL marking at the named crossings, the debug owner check; the JIT's counting rewritten for it | 2-3 weeks |
| 3 | 16-byte values, inline short strings, the single-block string; both JITs' templates for the new layout | 2-3 weeks |
| 4 | The map; interned keys | 1-2 weeks |
| 5 | The allocator, with the trim hook | 1-2 weeks |
| 6 | The builtin ABI; hot builtins moved | 1-2 weeks |
| 7 | Register bytecode, the new compiler back end and interpreter, quickening (JIT off for it until 8) | 4-6 weeks |
| 8 | Tier-1 JIT for the register bytecode, x64 then arm64 | 4-6 weeks |
| 9 | Tier 2, if the table still needs it | 4+ weeks |

Months in all. After stage 5 the string, record and JSON rows should be near PHP; after 8 every row should be
under it; 9 is for loops and fib if they are not.

## Reliability

- The debug owner check (stage 2 on) and ASan/UBSan builds of the suite at every stage.
- Fuzzing what takes outside input: JSON, the markup parser, regex patterns, HTTP heads, WebSocket frames, the grain
  parser itself.
- A soak at stages 5 and 8: an app with a JSON API, an HTML page and a route on a local ASQL, under the same load for
  hours in grain and in PHP (php-fpm), comparing errors, p99 latency and memory over time.

## Risks

- **Reference-count mistakes are memory corruption.** The owner check exists to turn them into test failures; no
  stage after 2 ships without the suite passing in that build.
- **The arm64 JIT needs the Pi** to test; until stage 8 reaches arm64 the Pi runs the interpreter for new bytecode.
- **The wasm build** (`grain/wasm`) has no JIT and must keep building at every stage.
- **Between stages the code carries both shapes** (an adapter, an accessor): each such bridge is removed in the
  stage after the one that made it.
