# Grain Standard Library Reference

This reference documents the complete built-in standard library of **Grain**, the scripting and systems language embedded in **AIOS** and available standalone via `grain`.

All builtins are implemented in C++20 across `src/userland/grain_*.cpp` and available in global scope without imports.

---

## Table of Contents

1. [Core & Types](#1-core--types)
2. [Numbers, Math & Animation](#2-numbers-math--animation)
3. [Exact Decimals (`decimal`)](#3-exact-decimals-decimal)
4. [Strings, Characters & Formatting](#4-strings-characters--formatting)
5. [Collections: Lists & Maps](#5-collections-lists--maps)
6. [Binary Buffers (`bytes`) & Struct Packing](#6-binary-buffers-bytes--struct-packing)
7. [Date, Time & Timers](#7-date-time--timers)
8. [File System, Paths & I/O](#8-file-system-paths--io)
9. [Regular Expressions (`regex`)](#9-regular-expressions-regex)
10. [Data Encodings (JSON, CSV, Base64, Hex, URL)](#10-data-encodings-json-csv-base64-hex-url)
11. [Concurrency & Parallelism](#11-concurrency--parallelism)
12. [Networking: HTTP, WebSockets & Sockets (TCP/UDP)](#12-networking-http-websockets--sockets-tcpudp)
13. [Databases & ASQL](#13-databases--asql)
14. [Canvas 2D Graphics](#14-canvas-2d-graphics)
15. [Colors & Palettes](#15-colors--palettes)
16. [UI Widgets, Charts & Windows](#16-ui-widgets-charts--windows)
17. [AIOS Kernel, Capabilities & Sidecars](#17-aios-kernel-capabilities--sidecars)
18. [Testing, Assertions & Errors](#18-testing-assertions--errors)

---

## 1. Core & Types

| Function | Signature | Description |
|---|---|---|
| `print` | `print(...args)` | Prints arguments to stdout separated by spaces, followed by a newline. |
| `eprint` | `eprint(...args)` | Prints arguments to stderr separated by spaces, followed by a newline. |
| `type` | `type(val)` | Returns the type name as a string: `"nil"`, `"bool"`, `"int"`, `"real"`, `"decimal"`, `"str"`, `"list"`, `"map"`, `"fn"`, `"bytes"`, `"handle"`. |
| `str` | `str(val)` | Converts a value to its human-readable string representation. |
| `repr` | `repr(val)` | Converts a value to its debug string representation (quoting strings). |
| `int` | `int(val)` | Converts a string, real, or decimal to an integer (truncating toward zero). |
| `real` | `real(val)` | Converts an integer, string, or decimal to a 64-bit IEEE 754 floating-point number. |
| `len` | `len(val)` | Returns the length of a string, list, map, or bytes buffer. |
| `cwd` | `cwd()` | Returns the current working directory path. |
| `getenv` | `getenv(name, [default])` | Reads an environment variable; returns `default` or `nil` if unset. |
| `env` | `env(name, [default])` | An app setting or environment variable; `default` (or `nil`) when it is unset **or empty**. Use it for hosts, ports and names, where `""` never means a value. |
| `secret` | `secret(name, [default])` | `env(name)`, or else the contents of the file `NAME_FILE` names, trimmed: a password kept in a file of its own (mode 600) instead of the environment. Neither set: `default`, or `""`. |
| `setenv` | `setenv(name, val)` | Sets or updates an environment variable for the process. |
| `sleep` | `sleep(ms)` | Pauses the current execution thread for `ms` milliseconds. |
| `clock` | `clock()` | High-resolution wall-clock timestamp in fractional seconds. |
| `time` | `time()` | Current UNIX epoch timestamp in whole seconds. |
| `time_ms` | `time_ms()` | Current UNIX epoch timestamp in milliseconds. |
| `time_us` | `time_us()` | Current UNIX epoch timestamp in microseconds. |
| `gc` | `gc()` | Explicitly triggers cycle collection on the VM heap. |
| `debugger` | `debugger()` | Triggers a breakpoint or enters the interactive debugger if active. |
| `is_stopping` | `is_stopping()` | Returns `true` if a shutdown or termination signal was received. |
| `should_stop` | `should_stop()` | Alias for `is_stopping()`. |

### Structured Application Logging
| Function | Signature | Description |
|---|---|---|
| `log_debug` | `log_debug(msg, [extra_map])` | Emits a debug-level log message with optional key-value metadata map. |
| `log_info` | `log_info(msg, [extra_map])` | Emits an info-level log message with optional key-value metadata map. |
| `log_warn` | `log_warn(msg, [extra_map])` | Emits a warn-level log message with optional key-value metadata map. |
| `log_error` | `log_error(msg, [extra_map])` | Emits an error-level log message with optional key-value metadata map. |
| `log_set_level` | `log_set_level(level_str)` | Sets logging threshold: `"debug"`, `"info"`, `"warn"`, or `"error"`. Default is `"info"`. |
| `log_get_level` | `log_get_level()` | Returns active log level string (`"debug"`, `"info"`, `"warn"`, `"error"`). |
| `log_set_format` | `log_set_format(format_str)` | Configures output format: `"text"` (ISO timestamp & text) or `"json"` (single-line JSON). |
| `log_get_format` | `log_get_format()` | Returns active log format string (`"text"` or `"json"`). |

---

## 2. Numbers, Math & Animation

### Arithmetic & Standard Math
| Function | Signature | Description |
|---|---|---|
| `abs` | `abs(n)` | Returns the absolute value of an integer, real, or decimal. |
| `idiv` | `idiv(a, b)` | Whole-number integer division, truncating toward zero. |
| `min` | `min(...vals)` / `min(list)` | The smallest of numbers, or of strings (by bytes, so `"2026-10-27 08:05"` < `"2026-10-27 14:30"`). `nil` is passed over (all `nil`: `nil`), so a running value can start as `nil`: `first = min(first, t)`. Numbers and strings together are an error. |
| `max` | `max(...vals)` / `max(list)` | The largest; the same rules as `min`. `latest = max(latest, t)`. |
| `floor` | `floor(n)` | Rounds down to the nearest integer. |
| `ceil` | `ceil(n)` | Rounds up to the nearest integer. |
| `round` | `round(n, [places])` | Rounds half away from zero (default 0 decimal places). |
| `trunc` | `trunc(n)` | Truncates decimal digits toward zero. |
| `sign` | `sign(n)` | Returns `1` if positive, `-1` if negative, `0` if zero. |
| `clamp` | `clamp(val, min, max)` | Constrains `val` within the inclusive range `[min, max]`. |
| `sqrt` | `sqrt(n)` | Square root. |
| `pow` | `pow(base, exp)` | Raises `base` to `exp`. |
| `hypot` | `hypot(x, y)` | Euclidean distance $\sqrt{x^2 + y^2}$. |
| `exp` | `exp(n)` | Natural exponential $e^n$. |
| `log` | `log(n)` | Natural logarithm $\ln(n)$. |
| `log2` | `log2(n)` | Base-2 logarithm. |
| `log10` | `log10(n)` | Base-10 logarithm. |
| `pi` | `pi()` | Returns the constant $\pi \approx 3.141592653589793$. |
| `e` | `e()` | Returns Euler's number $e \approx 2.718281828459045$. |

### Trigonometry & Angles
| Function | Signature | Description |
|---|---|---|
| `sin`, `cos`, `tan` | `sin(rad)`, `cos(rad)`, `tan(rad)` | Standard trigonometric functions (radians). |
| `asin`, `acos`, `atan` | `asin(n)`, `acos(n)`, `atan(n)` | Inverse trigonometric functions. |
| `atan2` | `atan2(y, x)` | Four-quadrant arctangent $\text{atan2}(y, x)$. |
| `rad` | `rad(deg)` | Converts degrees to radians. |
| `deg` | `deg(rad)` | Converts radians to degrees. |
| `haversine` | `haversine(lat1, lon1, lat2, lon2)` | Computes great-circle distance between coordinates in kilometers. |

### Random Numbers
| Function | Signature | Description |
|---|---|---|
| `rand` / `random` | `rand([min, [max]])` | Without arguments: `[0.0, 1.0)`. With `max`: integer in `[0, max]`. With `min, max`: integer in `[min, max]`. |
| `seed` | `seed(int_val)` | Seeds the pseudo-random number generator. |

### Animation & Easing
| Function | Signature | Description |
|---|---|---|
| `lerp` | `lerp(a, b, t)` | Linear interpolation $a + (b - a) \times t$. |
| `smoothstep` | `smoothstep(edge0, edge1, x)` | Hermite interpolation between edges for smooth transitions. |
| `spring` | `spring(cur, target, vel, [tension, friction])` | Calculates next position and velocity under a damped spring model. |
| `ease_in` | `ease_in(t)` | Quadratic ease-in acceleration curve ($t \in [0, 1]$). |
| `ease_out` | `ease_out(t)` | Quadratic ease-out deceleration curve ($t \in [0, 1]$). |
| `ease_in_out` | `ease_in_out(t)` | Quadratic ease-in-out S-curve. |

---

## 3. Exact Decimals (`decimal`)

Decimals use a 128-bit integer mantissa with an explicit scale up to 30 digits, preventing IEEE floating-point rounding errors for currency and tax arithmetic.

| Function | Signature | Description |
|---|---|---|
| `decimal` | `decimal(str_or_num, [scale])` | Constructs an exact decimal (e.g. `decimal("19.99")`). |
| `dec_div` | `dec_div(a, b, places)` | Exact decimal division rounded to the specified decimal places. |
| `scale` | `scale(dec)` | Returns the decimal's current fractional scale (places past the decimal point). |
| `decimal_prefix`| `decimal_prefix(str)` | Parses leading number characters from a string into `{decimal, whole}` or `nil`. |
| `round_sig` | `round_sig(dec, n)` | Rounds decimal to `n` significant figures. |
| `number_format` | `number_format(val, [places, pt, sep])` | Formats number with thousands separator and decimal point (PHP style). |
| `is_numeric` | `is_numeric(val)` | Returns `true` if `val` is a number or numeric string. |
| `to_number` | `to_number(val, [default])` | Extracts leading numeric value from string (e.g. `"12.50abc"` $\to 12.50$). |

---

## 4. Strings, Characters & Formatting

### Inspection & Predicates
| Function | Signature | Description |
|---|---|---|
| `char_at` | `char_at(str, index)` | Returns 1-character string at 0-based `index`. |
| `chars` | `chars(str)` | Splits string into a list of 1-character substrings. |
| `ord` | `ord(char_str)` | Unicode code point for the first character. |
| `chr` | `chr(code_point)` | Converts Unicode code point integer into a UTF-8 character string. |
| `utf8_valid` | `utf8_valid(str_or_bytes)` | Returns `true` if the input is valid UTF-8. |
| `is_digit` | `is_digit(str)` | `true` if string is non-empty and contains only digits (`0-9`). |
| `is_alpha` | `is_alpha(str)` | `true` if string contains only ASCII letters (`a-z`, `A-Z`). |
| `is_alnum` | `is_alnum(str)` | `true` if string contains only letters or digits. |
| `is_space` | `is_space(str)` | `true` if string contains only whitespace characters (` \t\r\n`). |
| `contains` | `contains(str, substr)` | Returns `true` if `substr` is found within `str`. |
| `starts_with` | `starts_with(str, prefix)` | Returns `true` if `str` begins with `prefix`. |
| `ends_with` | `ends_with(str, suffix)` | Returns `true` if `str` ends with `suffix`. |
| `index_of` | `index_of(str, sub, [start])` | Returns 0-based index of `sub`, or `-1` if not found. |
| `last_index_of` | `last_index_of(str, sub)` | Returns last index of `sub`, or `-1` if not found. |
| `find` | `find(haystack, needle)` | Alias for `index_of`. |
| `is_blank` | `is_blank(val)` | `true` for `nil` and for a string of only spaces, tabs and line breaks (a column that came back NULL or `''`); `false` for anything else, `0`, `false` and `[]` included. |
| `first_present` | `first_present(a, b, ...)` | The first argument that is not blank (`is_blank`), or `nil`: `first_present(u.name, u.realname, u.userid)`. Unlike `a ?? b`, which only passes over `nil`, it passes over `""` too. |

### Manipulation & Trimming
| Function | Signature | Description |
|---|---|---|
| `upper` | `upper(str)` | Converts string to uppercase. |
| `lower` | `lower(str)` | Converts string to lowercase. |
| `trim` | `trim(str, [chars])` | Strips leading and trailing whitespace (or specific characters). |
| `ltrim` | `ltrim(str, [chars])` | Strips leading whitespace or characters. |
| `rtrim` | `rtrim(str, [chars])` | Strips trailing whitespace or characters. |
| `upper_rtrim` | `upper_rtrim(str)` | Uppercases string and trims trailing whitespace. |
| `pad_left` | `pad_left(str, len, [pad])` | Pads string on the left to `len` characters (default space). |
| `pad_right` | `pad_right(str, len, [pad])`| Pads string on the right to `len` characters. |
| `repeat` | `repeat(str, count)` | Repeats string `count` times. |
| `replace` | `replace(str, old, new)` | Replaces all occurrences of `old` with `new`. |
| `slice` | `slice(str_or_list, start, [end])` | Slices substring or sublist from `start` up to `end`. |
| `split` | `split(str, [delimiter])` | Splits string by delimiter (defaults to whitespace). |
| `join` | `join(list, [glue])` | Joins a list of strings using `glue` (default empty string). |
| `format` | `format(pattern, ...args)` | Python-style format strings (e.g. `format("{:>8.2f}", 12.3)`). |

---

## 5. Collections: Lists & Maps

### List Operations
| Function | Signature | Description |
|---|---|---|
| `range` | `range(start, [stop, [step]])` | Generates a list of numbers from `start` to `stop`. |
| `push` | `push(list, ...items)` | Appends items to the end of a list in-place; returns list. |
| `pop` | `pop(list)` | Removes and returns the last element of the list. |
| `insert` | `insert(list, index, item)` | Inserts `item` at `index` in-place. |
| `remove_at` | `remove_at(list, index)` | Removes element at `index` in-place. |
| `reverse` | `reverse(list)` | Returns a new list with elements in reverse order. |
| `sort` | `sort(list)` | Returns a new list sorted in ascending order. |
| `sort_by` | `sort_by(list, key_fn, ["asc"\|"desc"])` | Sorts by extracted key, stable. Keys are numbers, strings or booleans, or a **list** of them: compared item by item, so ties fall to the next item: `sort_by(users, fn(u) { return [-u.touches, lower(u.userid)] })`. |
| `group_by` | `group_by(list, key_fn)` | Groups items into a map of lists keyed by `key_fn(item)`. |
| `key_by` | `key_by(list, key_fn)` | Maps list into dictionary keyed by `key_fn(item)` (first wins). |
| `sum_by` | `sum_by(list, key_fn, val_fn)` | Groups items and aggregates totals per key. |
| `sum` | `sum(list)` | The items added as `sum_by` adds them: integers stay integers, decimals exact, otherwise a real; `nil` items count nothing; `0` for an empty list. |
| `chunk` | `chunk(list, n)` | The list in pieces of `n` (the last one shorter): `chunk([1,2,3,4,5], 2)` is `[[1,2],[3,4],[5]]`. |
| `partition` | `partition(list, fn)` | `[items fn is true for, the rest]`, each in order. |
| `take_while` | `take_while(list, fn)` | The leading items `fn` is true for. |
| `drop_while` | `drop_while(list, fn)` | Everything after the leading items `fn` is true for. |
| `min_by` / `max_by` | `min_by(list, fn)` | The item whose `fn(item)` is least / greatest (keys ordered as `sort_by` orders them); the first of equals; `nil` for an empty list. |
| `find_index` | `find_index(list, fn)` | The index of the first item `fn` is true for, `-1` when none. |
| `zip_with` | `zip_with(a, b, fn)` | `fn(a[i], b[i])` for each pair, as long as the shorter list. |
| `unique` / `dedup` | `unique(list)` | Returns a list with consecutive or duplicate items removed. |
| `flatten` | `flatten(list, [depth])` | Flattens nested lists up to `depth` levels (default 1). |
| `concat` | `concat(...lists)` | Concatenates multiple lists into a single list. |
| `zip` | `zip(list1, list2)` | Pairs corresponding elements into 2-element sublists `[a, b]`. |
| `enumerate` | `enumerate(list)` | Returns list of `[index, element]` pairs. |
| `take` | `take(list, n)` | Returns the first `n` elements. |
| `drop` | `drop(list, n)` | Returns elements after skipping the first `n`. |
| `count` | `count(list, val_or_fn)` | Counts occurrences of `val`, or items matching `fn(x) == true`. |

### Higher-Order Iteration
| Function | Signature | Description |
|---|---|---|
| `map` | `map(list, fn)` | Transforms each element: `[fn(x) for x in list]`. |
| `flat_map` | `flat_map(list, fn)` | `map` with the results joined into one list: a list result contributes its items, `nil` nothing, any other value itself (one level, as JavaScript's `flatMap`). `flat_map(orders, fn (o) { return o.lines })`. Allowed in Things. |
| `filter` | `filter(list, fn)` | Keeps elements where `fn(x)` is truthy. |
| `reduce` | `reduce(list, fn, [init])` | Folds list with accumulator: `fn(acc, item)`. |
| `each` / `for_each` | `each(list, fn)` | Invokes `fn(item)` on each element for side effects. |
| `any` | `any(list, fn)` | Returns `true` if `fn(x)` returns true for any element. |
| `all` | `all(list, fn)` | Returns `true` if `fn(x)` returns true for every element. |
| `find_if` | `find_if(list, fn)` | Returns the first element where `fn(x)` is true, or `nil`. |

### Map (Dictionary) Operations
| Function | Signature | Description |
|---|---|---|
| `keys` | `keys(map)` | Returns a list of the map's keys. |
| `values` | `values(map)` | Returns a list of the map's values. |
| `entries` | `entries(map)` | Returns list of `[key, value]` pairs. |
| `from_entries` | `from_entries(pairs)` | Creates a map from a list of `[key, value]` pairs. |
| `has` | `has(map, key)` | Returns `true` if `key` exists in `map`. |
| `del` | `del(map, key)` | Deletes `key` from map in-place; returns value. |
| `get` | `get(map_or_list, key_or_idx, [default])` | Safe lookup returning `default` (or `nil`) if not found. |
| `get_in` | `get_in(container, path_list, [default])` | Deep nested traversal over maps and lists with default fallback. |
| `merge` | `merge(...maps)` | Merges multiple maps (later keys overwrite earlier ones). |
| `get_or_put` | `get_or_put(map, key, value)` | `map[key]` when it is there (and not `nil`); otherwise puts `value` at `key` and returns it. The record a key stands for, made the first time it is seen: `get_or_put(users, lower(uid), {"userid": uid, "n": 0}).n += 1`. `value` is worked out before the call either way. |
| `add_into` | `add_into(total, record, [fields])` | Adds `record`'s numbers into `total`'s, field by field and into nested maps (a field `total` lacks starts at `record`'s value; a nested map it lacks starts empty). Text, lists and `nil` are left alone; a total field that is not a number is an error. `fields` names the top-level fields to add. Decimals add exactly. Returns `total`. Things may use it. |
| `add_keys` | `add_keys(map, list, [value])` | Sets `map[item] = value` (default `true`) for every item: a map used as a set, `len(map)` its size. Returns the map. |

---

## 6. Binary Buffers (`bytes`) & Struct Packing

The `bytes` type provides bounds-checked mutable byte buffers for binary protocols, packet parsing, and binary file I/O.

| Function | Signature | Description |
|---|---|---|
| `bytes` | `bytes(size)` | Allocates a mutable buffer of `size` zero bytes. |
| `bytes_get` | `bytes_get(buf, offset, type)` | Reads typed value from `offset`. Types: `"u8"`, `"i8"`, `"u16le"`, `"u16be"`, `"i32le"`, `"u32be"`, `"i64le"`, `"u64le"`, `"f32le"`, `"f64be"`. |
| `bytes_set` | `bytes_set(buf, offset, type, val)` | Writes typed value at `offset` in buffer. |
| `bytes_str` | `bytes_str(buf, [offset, [len]])` | Converts buffer slice to a UTF-8 string. |
| `gzip` | `gzip(data, [level=6])` | Compresses a string or bytes to gzip (RFC 1952) bytes with the in-tree deflate; level 1 (quick) to 9 (smallest). |
| `gunzip` | `gunzip(data, [max_bytes=64MB])` | The bytes a gzip stream holds (every member, each CRC-32 checked); an error on damaged data or past `max_bytes`. `bytes_str(gunzip(x))` for text. |
| `bytes_hex` | `bytes_hex(buf)` | Encodes buffer as lowercase hex string. |
| `bytes_from_hex`| `bytes_from_hex(hex_str)` | Decodes hex string into a `bytes` buffer. |
| `bytes_fill` | `bytes_fill(buf, byte_val, [offset, [len]])` | Fills buffer range with a repeated byte. |
| `bytes_copy` | `bytes_copy(dst, dst_off, src, [src_off, [len]])` | Copies bytes between buffers. |
| `bytes_concat` | `bytes_concat(list_of_bytes)` | Concatenates multiple `bytes` buffers into one. |
| `bytes_find` | `bytes_find(buf, needle_bytes_or_str, [start])` | Finds offset of needle in buffer. |
| `pack` | `pack(fmt, ...values)` | Packs values into `bytes` according to format string (e.g. `">HH4s"`). |
| `unpack` | `unpack(fmt, buf, [offset])` | Unpacks values from buffer according to format string. |
| `pack_size` | `pack_size(fmt)` | Computes byte size needed for a struct pack format string. |

---

## 7. Date, Time & Timers

| Function | Signature | Description |
|---|---|---|
| `now` | `now()` | Returns fractional UNIX epoch timestamp in seconds. |
| `iso8601` | `iso8601([timestamp])` | Formats timestamp as ISO 8601 string (e.g. `"2026-10-26T14:48:02Z"`). Default is current time. |
| `date_parts` | `date_parts(ts, [zone])` | The calendar fields of a timestamp in `zone` (`"local"`, the default, or `"utc"`): `{year, month, day, hour, minute, second, millisecond, weekday, yearday, offset}` - month 1-12, weekday 0 (Sunday) to 6, offset the zone's distance from UTC in seconds. |
| `date_format` | `date_format(ts, fmt, [zone])` | `ts` may be a date string (`"2026-10-27 08:20:11"`, as a database gives it), written as it reads: read and written in one zone (`"utc"` unless given), so its wall time does not move; a string that is not a date is an error. Formats a timestamp (zone `"local"`, the default, or `"utc"`). `%Y %m %d %e %H %I %M %S %L %j %y %a %A %b %B %p %F %T %z %s %%`; `%P` is lowercase am/pm; `%-d %-m %-H %-I %-M %-S %-j` drop the leading zero: `date_format(date_parse("2026-10-07 09:05"), "%b %-d, %Y %-I:%M%P", "utc")` is `"Oct 7, 2026 9:05am"`. |
| `date_parse` | `date_parse(str, [zone])` | An ISO 8601 date or date-time (`YYYY-MM-DD[ T]HH:MM[:SS[.fff]][Z\|+HH:MM]`) as seconds since the epoch. Read in `zone` (`"utc"`, the default, or `"local"`) unless the text carries its own offset. Anything else, or a field out of range (`0000-00-00`), is an error - unless the second argument is a map with `or`: `date_parse(row.stopped, {"or": nil})` gives `nil` for a zero date, `""` or `nil` (the map may also hold `zone`). |
| `date_make` | `date_make(y, m, d, [h, min, s], [zone])` | Seconds since the epoch for a calendar date (and time) in `zone` (`"local"`, the default, or `"utc"`); a field out of range is an error. |
| `day_add` | `day_add(day, n)` | The day `n` calendar days after (`n` < 0: before) a `YYYY-MM-DD` day: `day_add("2026-10-27", -29)` is `"2026-09-28"`. No zone, so no daylight-saving hour. Things may use it. |
| `date_range` | `date_range(first_day, last_day)` | Every calendar day from one `YYYY-MM-DD` to another, both included, as `YYYY-MM-DD` strings; `[]` when the first is after the last. No zone, so no daylight-saving hour to skip or repeat. At most 100000 days. |
| `every` | `every(seconds, fn)` | Recurring timer executing `fn()` every `seconds`. Returns timer ID. |
| `after` | `after(seconds, fn)` | One-shot delayed timer executing `fn()`. Returns timer ID. |
| `cancel` | `cancel(timer_id)` | Cancels an active timer created with `every` or `after`. |
| `wait` | `wait(seconds)` | Non-blocking sleep while firing active background timers. |

---

## 8. File System, Paths & I/O

### File Operations
| Function | Signature | Description |
|---|---|---|
| `read_file` | `read_file(path)` | Reads entire file into a UTF-8 string. |
| `write_file` | `write_file(path, content)` | Writes string to file, replacing existing content. |
| `append_file` | `append_file(path, content)` | Appends string to the end of a file. |
| `read_bytes` | `read_bytes(path)` | Reads file into a mutable `bytes` buffer. |
| `write_bytes` | `write_bytes(path, bytes_buf)`| Writes `bytes` buffer directly to file. |
| `file_exists` | `file_exists(path)` | Returns `true` if file or directory exists. |
| `delete_file` | `delete_file(path)` | Deletes a file. |
| `list_dir` | `list_dir([path])` | Returns a list of filenames in the directory (default `.`). |
| `mkdir` | `mkdir(path, [parents])` | Creates directory; creates parent directories if `parents` is true. |
| `rmdir` | `rmdir(path)` | Removes an empty directory. |
| `is_dir` | `is_dir(path)` | Returns `true` if path exists and is a directory. |

### Path Manipulation
| Function | Signature | Description |
|---|---|---|
| `path_join` | `path_join(...segments)` | Joins path segments using platform separator (`/`). |
| `basename` / `path_basename` | `basename(path)` | Returns the trailing filename component. |
| `dirname` / `path_dirname` | `dirname(path)` | Returns parent directory path. |
| `extname` / `path_ext` | `extname(path)` | Returns file extension including leading dot (e.g. `".gr"`). |
| `path_normalize`| `path_normalize(path)` | Resolves `.` and `..` relative segments cleanly. |
| `abspath` | `abspath(path)` | Returns absolute normalized file path. |

---

## 9. Regular Expressions (`regex`)

ECMAScript-compatible regular expression operations.

| Function | Signature | Description |
|---|---|---|
| `regex_test` | `regex_test(pattern, str, [flags])` | Returns `true` if `str` matches `pattern`. |
| `regex_match` | `regex_match(pattern, str, [flags])` | Returns first match with captured groups as a map, or `nil`. |
| `regex_find_all`| `regex_find_all(pattern, str, [flags])` | Returns list of all matching substrings. |
| `regex_replace` | `regex_replace(pattern, str, repl, [flags])` | Replaces regex matches with replacement string. |
| `regex_split` | `regex_split(pattern, str, [limit])` | Splits string by regex pattern into list of substrings. |

---

## 10. Data Encodings (JSON, CSV, Base64, Hex, URL)

| Function | Signature | Description |
|---|---|---|
| `json_parse` | `json_parse(json_str)` | Parses JSON string into Grain maps, lists, and primitives. |
| `json_stringify`| `json_stringify(val, [options])` | Serializes value to JSON. Options: `{"indent": 2, "ascii": true, "escape_slashes": true}`. |
| `xml_parse` | `xml_parse(xml_str)` | Parses XML into nested map `{tag, attrs, children, text}` with entity decoding and CDATA support. |
| `xml_escape` | `xml_escape(str)` | Escapes special XML characters (`&`, `<`, `>`, `"`, `'`). |
| `xml_unescape` | `xml_unescape(str)` | Unescapes standard and numeric XML entities (e.g. `&amp;`, `&#x2603;`). |
| `html_escape` | `html_escape(val)` | `val` as text safe inside an element or a quoted attribute (`&`, `<`, `>`, `"`, `'` as `&#39;`); `nil` is `""`, a number its text. |
| `el` | `el(tag, [attrs], ...children)` | An element node `{tag, attrs, children}`. The second argument is the attrs when it is `nil` or a map that is not itself a node; otherwise it is the first child. |
| `html_raw` | `html_raw(markup)` | Markup already written, put into a node tree as it is (`&middot;`, a fragment rendered elsewhere). |
| `html_stringify` | `html_stringify(node)` | Writes a node tree as HTML, escaped in one place (see below). |
| `xml_stringify` | `xml_stringify(node)` | Writes a node tree as XML; also writes back what `xml_parse` returns. |

#### Building markup

Build a page or document as nodes and write it out once, instead of joining strings and remembering to escape every value:

```grain
let row = el("tr", el("td", {"class": ["who", is_lead ? "lead" : nil], "data-v": lower(name)},
                       el("b", name), el("small", userid)))
html_stringify(row)   // <tr><td class="who" data-v="ann"><b>Ann &amp; co</b><small>ann</small></td></tr>
```

- **Nodes:** a string or number is text (escaped); `nil` and `false` write nothing, so `cond ? el(...) : nil` works; a list writes each item in turn; `html_raw(s)` writes `s` as it is; a map `{tag, attrs, children}` is an element (what `el` makes and `xml_parse` returns; a parsed `text` comes before the children). `true` is an error.
- **Attributes** are written in the order given. `nil` and `false` leave the attribute out; `true` writes it bare in HTML (`open`) and as `="true"` in XML; a list is its items joined by spaces, blanks skipped (class lists). Names with spaces, quotes or `=` are refused.
- **HTML:** `area base br col embed hr img input link meta source track wbr` have no closing tag (content in one is an error); `<script>` and `<style>` content is written unescaped, with any `</script` inside written as `<\/script` so it cannot end the element early.
- **XML:** an element with no content is written `<name/>`.
| `base64_encode` | `base64_encode(str_or_bytes)` | Encodes string or bytes to standard Base64. |
| `base64_decode` | `base64_decode(b64_str)` | Decodes Base64 string to original UTF-8 string. |
| `hex_encode` | `hex_encode(str_or_bytes)` | Encodes input into lowercase hexadecimal string. |
| `hex_decode` | `hex_decode(hex_str)` | Decodes hex string to raw string. |
| `url_encode` | `url_encode(str)` | Percent-encodes URI component. |
| `url_decode` | `url_decode(str)` | Decodes percent-encoded URI component. |
| `data_to_csv` | `data_to_csv(headers, rows)` | Formats table headers and rows as RFC 4180 CSV string. |
| `data_to_json` | `data_to_json(headers, rows)` | Converts columnar rows into a JSON array of objects. |
| `data_to_sql_insert` | `data_to_sql_insert(table, cols, rows)` | Generates multi-row `INSERT INTO table (cols) VALUES (...)` statement. |
| `uuid` / `uuid_v4` | `uuid_v4()` | Generates a 36-character RFC 4122 Version 4 random UUID. |

### Cryptographic Hashes & Ciphers
| Function | Signature | Description |
|---|---|---|
| `sha256`, `sha1`, `sha512`, `sha384`, `md5`, `keccak256` | `sha256(data)` | Cryptographic digest returned as lowercase hex string. |
| `hmac_sha256` | `hmac_sha256(key, data)` | Computes HMAC-SHA256 signature as hex string. |
| `random_bytes` | `random_bytes(count)` | Cryptographically secure random bytes buffer. |
| `aes_cbc_decrypt` | `aes_cbc_decrypt(key, iv, data)` | AES-CBC decryption with PKCS#7 unpadding (returns `nil` on bad padding). |
| `hash_equals` | `hash_equals(a, b)` | Timing-safe string comparison to protect against side-channel leaks. |
| `bcrypt_hash` | `bcrypt_hash(pass, [cost=10])` | Computes a standard `$2b$` OpenBSD bcrypt password hash with random salt. |
| `bcrypt_verify` | `bcrypt_verify(pass, hash)` | Verifies a password against a `$2a$`, `$2b$` or `$2y$` (PHP `password_hash`) bcrypt hash in constant time. |
| `jwt_sign` | `jwt_sign(payload_map, secret, [expires_in_seconds])` | Creates an RFC 7519 JSON Web Token (HS256) signed with HMAC-SHA256. With `expires_in_seconds`, sets `iat` and `exp` (the caller's map is not changed). An empty secret is an error. |
| `jwt_verify` | `jwt_verify(token, secret)` | Verifies a JWT token signature, expiration (`exp`), and not-before (`nbf`), returning `{"ok": bool, "payload": map, "error": str}`. |

### Document Generation & Archives (`doc.*`)
| Function | Signature | Description |
|---|---|---|
| `doc.csv` / `csv_format` | `doc.csv(cols, rows)` or `doc.csv(records)` | Formats columns/rows or a list of record maps as an RFC 4180 CSV string. |
| `doc.parse_csv` / `csv_parse` | `doc.parse_csv(text, [options])` | Parses RFC 4180 CSV into a list of maps, or `{columns, rows}` when `as_table: true`. |
| `doc.xlsx` / `xlsx_create` | `doc.xlsx(spec)` | Generates an OpenXML `.xlsx` workbook from `{sheet, records, columns, rows, freeze_pane, merges, col_widths}`, or several sheets as `{"sheets": [spec, ...]}` (tab order; names 1-31 characters, none of `: \ / ? * [ ]`, unique). A cell is a value or `{v, s}`; `s` is one of `plain`, `bold`, `note`, `title`, `header`, `section`, `int`, `dec`, `count` (a zero shows as a dash), `contrib_header`, `contrib_int`, `contrib_dec`, `total_text`, `total_int`, `total_dec`, `total_count`, `total_contrib_int`, `total_contrib_dec`, and any of them with `_edge` for a section's first column (a dark left border). Numbers are written exactly. `merges` holds `"B3:D3"` ranges or `[first_row, first_col, last_row, last_col]` lists counted from 1. |
| `doc.pdf` / `pdf_render` | `doc.pdf(html, [css], [options])` | Compiles HTML/CSS into vector PDF 1.4 bytes (`page_width`, `page_height`, margins). |
| `doc.zip` / `zip_create` | `doc.zip(entries, [level])` | Packages a map of `{"filename": data}` into a PKZIP archive buffer. |
| `doc.unzip` / `unzip` | `doc.unzip(zip_data)` | Decompresses PKZIP archive bytes into a map of `{"filename": bytes}`. |

---

## 11. Concurrency & Parallelism

| Function | Signature | Description |
|---|---|---|
| `parallel_map` | `parallel_map(list, fn)` | Maps `fn(item)` in parallel across CPU cores using worker threads. |
| `spawn` | `spawn(fn, ...args)` | Spawns task on a background worker thread; returns a task handle. |
| `await` | `await(task_handle)` | Blocks until background task finishes and returns its value. |
| `task_done` | `task_done(task_handle)` | Returns `true` if the background task has completed execution. |

---

## 12. Networking: HTTP, WebSockets & Sockets (TCP/UDP)

### HTTP Client
| Function | Signature | Description |
|---|---|---|
| `http_get` | `http_get(url, [options])` | Performs HTTP/HTTPS GET request. Returns `{status, body, headers, ok}`. |
| `http_post` | `http_post(url, body, [options])` | Performs HTTP/HTTPS POST request with payload. |
| `http_request` | `http_request(options_map)` | Full request: `{"url": ..., "method": "PUT", "headers": {...}, "body": ...}`. |
| | | The HTTP calls ask for gzip and undo it (the response then reads as plain, its `content-encoding` header dropped). Options: `decompress: false` to get the body as sent, `max_bytes` (default 64 MB) for how large a gzip body may inflate. A grain app served by `aios serve` gzips text-like responses of 1 KB or more for clients that accept it. |
| `process_run` | `process_run(argv, [{cwd, env, inherit_env, timeout_ms=60000, max_output=16MB}])` | Runs a program by argument list (no shell: arguments arrive as written) through the hal process facility. Returns `{ok, code, output (stdout and stderr together), timed_out, truncated}`; a run past `timeout_ms` is killed. The child gets a clean environment (`PATH`, `LANG`, plus `env`) unless `inherit_env: true`. Allowed for scripts run from the command line (`grain`, not `--safe`; `aios grain`); a hosted app only when the machine's operator lists it in the host's `AIOS_APP_PROCESS_RUN` (comma list or `*`; the app's own settings cannot grant it); never in pages `aios-webserver` serves. Note `pcall(f, a_list)` spreads the list into arguments: call it as `pcall(fn () { return process_run([...]) })`. |
| `kv_get` | `kv_get(key, [default])` | The value stored under `key` (a copy), or `default` / nil. `kv_*` is a key-value store for sessions, rate limits and tokens: one per hosted app, shared by its sessions and workers, kept in memory and journaled in the app's data directory (`kv.journal`, replayed at start, compacted as it grows). In a plain `grain` script it is the process's, in memory. |
| `kv_set` | `kv_set(key, value, [ttl_seconds])` | Stores a string, number, bool, list or map (as JSON; at most 1 MB). A nil value deletes. Keys are 1 to 1024 bytes. |
| `kv_del` | `kv_del(key)` | Removes a key; whether it was there. |
| `kv_incr` | `kv_incr(key, [by=1], [ttl_seconds])` | Adds to a whole-number value atomically and returns it; a missing key starts at 0 and takes the ttl (a rate-limit window). |
| `kv_expire` | `kv_expire(key, ttl_seconds)` | Sets a key's expiry; whether it was there. |
| `kv_ttl` | `kv_ttl(key)` | Seconds left, -1 for no expiry, nil when missing. |
| `kv_keys` | `kv_keys([prefix])` | The live keys (with that prefix), sorted. |
| `kv_mget` | `kv_mget(keys)` | Their values in order, nil for the missing. |
| `mail_send` | `mail_send({host, port, security, user, password, from, from_name, to, cc, bcc, reply_to, subject, text, html, attachments, timeout_ms})` | Sends email through aios's own SMTP client (kernel sockets and TLS): STARTTLS on 587 (TLS on 465) unless `security` says `"tls"`, `"starttls"` or `"none"`; signs in when `user` is given. `to`/`cc`/`bcc` are an address or a list (bcc delivered, never named); `text` and/or `html`; `attachments` a list of `{filename, data (string or bytes), type}`. UTF-8 subjects, names and filenames are encoded. Returns `{ok, message_id, recipients}`; a line break in any header value, a bad address, or a password over `"none"` to another machine is an error. Keep the password in the app's settings: `getenv("SMTP_PASSWORD")`. |
| `http_parse_multipart` | `http_parse_multipart(body, content_type_or_boundary)` | Parses RFC 7578 multipart form payloads into `{"form": {...}, "files": {...}}` with strict path traversal stripping and size guards. |

### Server-Sent Events (SSE) Client
| Function | Signature | Description |
|---|---|---|
| `events` | `events(url, [options], on_event_fn)` | Connects to an SSE endpoint and calls `on_event_fn(data)` for each message. Returns stream ID. |
| `events_close` | `events_close(stream_id)` | Closes an active SSE connection. |

### Raw TCP Sockets
| Function | Signature | Description |
|---|---|---|
| `tcp_connect` | `tcp_connect(host, port, [timeout_ms])` | Opens TCP connection. Returns TCP socket handle. |
| `tcp_send` | `tcp_send(handle, data_str_or_bytes)` | Transmits data over socket. |
| `tcp_recv` | `tcp_recv(handle, [max_bytes, [timeout_ms]])` | Reads incoming data from socket. |
| `tcp_close` | `tcp_close(handle)` | Closes TCP connection. |

### Raw UDP Sockets
| Function | Signature | Description |
|---|---|---|
| `udp_socket` | `udp_socket()` | Creates an unbound UDP datagram socket. |
| `udp_bind` | `udp_bind(handle, host, port)` | Binds UDP socket to local interface and port. |
| `udp_send` | `udp_send(handle, host, port, data)` | Transmits datagram to destination. |
| `udp_recv` | `udp_recv(handle, [max_bytes, [timeout_ms]])` | Receives incoming datagram payload. |
| `udp_recv_from`| `udp_recv_from(handle, [max_bytes, [timeout_ms]])` | Receives payload returning `{data, host, port}`. |
| `udp_port` | `udp_port(handle)` | Returns bound local port. |
| `udp_close` | `udp_close(handle)` | Closes UDP socket. |

### Web Route Helpers
| Function | Signature | Description |
|---|---|---|
| `serve_routes` | `serve_routes(routes, [middleware_or_opts])` | Registers HTTP route table (`{"GET /path": fn(req) {...}}`). |
| `res_json` | `res_json(val, [status, [headers]])` | Returns HTTP response map formatted as JSON. |
| `res_text` | `res_text(text, [status, [headers]])` | Returns plain text HTTP response map. |
| `res_html` | `res_html(html, [status, [headers]])` | Returns HTML HTTP response map. |
| `res_error` | `res_error(status, [message])` | Returns a JSON error response `{error, status}` (a status that is not a number is 500; the message defaults to `"Internal Error"`). |
| `res_download` | `res_download(body, filename, [headers])` | A file to save: `Content-Disposition: attachment` with `filename` made safe (a non-ASCII name also as `filename*`), the content type from the extension (pdf, xlsx, docx, csv, json, txt, html, xml, zip, gz, png, jpg, svg; otherwise `application/octet-stream`) and no caching. `body` is a string or bytes; `headers` are added last and win. |
| `res_redirect` | `res_redirect(location, [status])` | Returns redirect response map (default 302). |
| `res_sse` | `res_sse(events_list, [headers])` | Returns response formatted as `text/event-stream`. |
| `on_machine` | `on_machine(machine_id, fn)` | Dispatches route execution to a specific fleet node. |

---

## 13. Databases & ASQL

Grain interacts with relational data and the ASQL engine through three distinct access mechanisms:
1. **In-Process & Fleet Mesh Engine (`sql`, `sql_on`)**: Queries executed directly against the local kernel's `SqlDatabase` instance without TCP/socket overhead, or routed across the P2P fleet mesh (`src/userland/machine_sql.cpp`).
2. **Wire Protocol Client (`db_*`, `live`)**: Full client speaking MySQL/MariaDB wire protocol to ASQL (e.g. port 3307), MariaDB, or MySQL (`src/userland/grain_std_db.cpp`, `src/userland/grain_aios.cpp`).
3. **Loopback One-Shot Helpers (`asql_query`, `asql_exec`)**: Ad-hoc query/execution helpers connecting to loopback ASQL on `127.0.0.1:3307` (`src/userland/grain_aios.cpp`).

### In-Process & Fleet Mesh SQL Engine
*Defined in [`src/userland/machine_sql.cpp`](file:///repos/aios/src/userland/machine_sql.cpp#L333-L355) (installed via `install_sql_builtins`).*

| Function | Signature | Description |
|---|---|---|
| `sql` | `sql(statement)` | Executes directly in the machine's in-process `SqlDatabase` (`machine_sql.cpp:333`). Result sets return a list of row maps with typed numbers (`Int` -> integer, `Real`/`Decimal` -> real) and `NULL` -> `nil`. Non-queries return `{"affected": n}`. Throws a Grain runtime error on failure (catch with `pcall`). |
| `sql_on` | `sql_on(peer_prefix, statement)` | Executes statement on remote fleet node identified by peer ID prefix (`machine_sql.cpp:342`). Returns the same list of maps or `{"affected": n}`. Remote writes require `fleet_sql_writes_allowed` on the target. Throws on failure. |

### MySQL, MariaDB & ASQL Wire Client
*Defined in [`src/userland/grain_std_db.cpp`](file:///repos/aios/src/userland/grain_std_db.cpp#L351-L711) (installed via `install_db_builtins`) and [`src/userland/grain_aios.cpp`](file:///repos/aios/src/userland/grain_aios.cpp#L333-L571) (`install_aios_builtins`).*

| Function | Signature | Description |
|---|---|---|
| `db_connect` | `db_connect(host, port, user, password, [database])` or `db_connect(cfg)` | `cfg` is a map `{host, port, user, password, db}` (`database` for `db`; port 3306 when left out). Connects via MySQL wire protocol (`grain_std_db.cpp:351`, `grain_aios.cpp:333`). Returns `{"ok": bool, "conn": <handle>, "server_version": str, "error": str}`. Either the returned map or the inner `conn` handle can be passed to subsequent `db_*` calls. |
| `db_query` | `db_query(conn, sql, [params], [options])` | Executes query with optional `?` parameter binding list and options map (`grain_std_db.cpp:474`). Strings are escaped, `nil` binds as `NULL`, lists expand for `IN (?)`. Options: `{rows: bool, records: bool, decimals: bool}`. Returns `{"ok": bool, "columns": [...], "rows": [[...]], "records": [{...}], "affected_rows": int, "last_insert_id": int, "error": str}`. |
| `thing_rows` | `thing_rows(conn, name, [keys])` | An ASQL Thing's rows - `SHOW THING name [KEYS (...)]` - as maps, the columns that hold JSON (a grain Thing's lists and maps) read back as values. `keys` (text or numbers) are quoted here; `[]` gives `[]`. A name that is not one, or a failed query, is an error. Works with either `db_query` (grain's records, aios's columns and rows). |
| `db_transaction` | `db_transaction(conn, fn)` | Runs callback `fn(conn)` in a transaction (`grain_std_db.cpp:517`). Manages `START TRANSACTION`/`COMMIT` (or nested `SAVEPOINT grain_sp_<depth>`). Automatically rolls back on runtime error and re-raises. Returns callback result. |
| `db_escape` | `db_escape(str)` | Escapes special characters (`\0`, `\n`, `\r`, `\\`, `'`, `"`, `\x1a`) for MySQL string literals (`grain_std_db.cpp:463`). |
| `db_quote` | `db_quote(val)` | Formats value as a safe SQL literal (`grain_std_db.cpp:468`): numbers, `NULL` for `nil`, `'escaped'` for strings. |
| `db_close` | `db_close(conn)` | Closes connection socket and frees session resources (`grain_std_db.cpp:512`, `grain_aios.cpp:441`). |
| `db_ping` | `db_ping(conn)` | Pings server to verify connection vitality (`grain_aios.cpp:433`). Returns boolean. |
| `db_connected` | `db_connected(conn)` | Checks if connection handle remains open and active (`grain_aios.cpp:448`). Returns boolean. |
| `db_select_db` | `db_select_db(conn, db_name)` | Switches default database on the active connection (`grain_aios.cpp:425`). Returns boolean. |
| `db_tables` | `db_tables(conn, [db_name])` | Returns list of table name strings in database (`grain_aios.cpp:454`). |
| `db_columns` | `db_columns(conn, table)` | Returns list of column metadata maps (`name`, `type`, `null`, `key`, `default`, `extra`) for table (`grain_aios.cpp:478`). |
| `db_schema` | `db_schema(conn)` | Introspects full schema of the database, returning `map[table_name] -> [column_maps]` (`grain_aios.cpp:502`). |
| `db_build_update` | `db_build_update(table, data_map, key_col, key_val)` | Constructs parameterized UPDATE statement and parameter values map `{"sql": str, "params": [...]}` (`grain_aios.cpp:538`). |
| `live` | `live(conn, query_or_tables, [callback])` | Subscribes to real-time mutations on ASQL via `SELECT ASQL_CHANGES(...)` timer polling (`grain_std_db.cpp:640`). Triggers callback or updates reactive view only when table tokens change. |

### Loopback ASQL Wire Helpers & Reactive Data
*Defined in [`src/userland/grain_aios.cpp`](file:///repos/aios/src/userland/grain_aios.cpp#L573-L714) (`install_aios_builtins`).*

| Function | Signature | Description |
|---|---|---|
| `asql_query` | `asql_query(sql, [port=3307], [login])` | Convenience helper connecting to loopback `127.0.0.1:port` (`grain_aios.cpp:649`). Executes query and returns list of row maps `[{"col": val, ...}, ...]`. Returns `nil` on error. Optional login map: `{"user": "...", "password": "...", "database": "..."}`. |
| `asql_exec` | `asql_exec(sql, [port=3307], [login])` | Convenience helper connecting to loopback `127.0.0.1:port` (`grain_aios.cpp:686`). Executes DDL/DML and returns `{"ok": bool, "affected_rows": int, "last_insert_id": int, "error": str}`. |
| `thing_conflicts` | `thing_conflicts(thing_name, [key], [port], [opts])` | Inspects conflict records for an ASQL reactive Thing (`grain_aios.cpp:573`). |

### SQL Tooling & Parsing Primitives
*Defined in [`src/userland/grain_builtins.cpp`](file:///repos/aios/src/userland/grain_builtins.cpp#L6294-L6568) (installed in `install_core_builtins`).*

| Function | Signature | Description |
|---|---|---|
| `sql_tokenize` | `sql_tokenize(sql_str)` | Lexes SQL statement into token maps `[{"type": str, "text": str}, ...]` (`grain_builtins.cpp:6294`). |
| `sql_format` | `sql_format(sql_str)` | Formats and indents SQL query string (`grain_builtins.cpp:6318`). |
| `sql_suggest` | `sql_suggest(sql, [cursor_pos, tables, columns])` | Returns autocomplete suggestions for keywords, tables, and columns (`grain_builtins.cpp:6499`). |
| `sql_generate_ddl` | `sql_generate_ddl(table, cols)` | Generates `CREATE TABLE` DDL statement from column definition maps (`grain_builtins.cpp:6348`). |

---

## 14. Canvas 2D Graphics

Grain includes a full software 2D rendering canvas for generating images, charting data, and rendering window buffers.

### Canvas Creation & Memory
| Function | Signature | Description |
|---|---|---|
| `canvas_create` / `cv_create` | `canvas_create(width, height, [bg_color])` | Allocates a 2D rendering canvas (dimensions 1 to 16,384). |
| `canvas_width` / `canvas_get_width` | `canvas_width(cv)` | Canvas width in pixels. |
| `canvas_height` / `canvas_get_height`| `canvas_height(cv)` | Canvas height in pixels. |
| `canvas_clear` | `canvas_clear(cv, [color])` | Clears canvas buffer to background color (default transparent). |
| `canvas_get_pixel` | `canvas_get_pixel(cv, x, y)` | Reads 32-bit ARGB pixel value at coordinate. |
| `canvas_set_pixel` | `canvas_set_pixel(cv, x, y, color)` | Writes 32-bit ARGB pixel at coordinate. |
| `canvas_get_pixels` | `canvas_get_pixels(cv)` | Returns raw pixel buffer as a `bytes` buffer. |
| `canvas_set_pixels` | `canvas_set_pixels(cv, bytes_buf)` | Copies pixel buffer directly into canvas. |
| `canvas_to_ppm` / `cv_to_ppm` | `canvas_to_ppm(cv)` | Encodes canvas to PPM image format bytes. |
| `canvas_save_ppm` | `canvas_save_ppm(cv, path)` | Writes canvas to disk as a PPM image. |
| `image_decode` | `image_decode(bytes_buf)` | Decodes PNG/JPEG bytes into a canvas instance. |

### Path Building & Shapes
| Function | Signature | Description |
|---|---|---|
| `canvas_begin_path` / `cv_begin_path` | `canvas_begin_path(cv)` | Resets the current path. |
| `canvas_close_path` / `cv_close_path` | `canvas_close_path(cv)` | Closes the current path with a line back to start. |
| `canvas_move_to` / `cv_move_to` | `canvas_move_to(cv, x, y)` | Moves subpath start point to `(x, y)`. |
| `canvas_line_to` / `cv_line_to` | `canvas_line_to(cv, x, y)` | Connects last point to `(x, y)` with a straight line. |
| `canvas_bezier_curve_to` | `canvas_bezier_curve_to(cv, cp1x, cp1y, cp2x, cp2y, x, y)` | Adds cubic Bézier curve to current path. |
| `canvas_quadratic_curve_to` | `canvas_quadratic_curve_to(cv, cpx, cpy, x, y)` | Adds quadratic Bézier curve to current path. |
| `canvas_arc` | `canvas_arc(cv, cx, cy, r, start_rad, end_rad, [ccw])` | Adds circular arc to path. |
| `canvas_stroke` / `cv_stroke` | `canvas_stroke(cv)` | Outlines current path with stroke style. |
| `canvas_fill` / `cv_fill` | `canvas_fill(cv)` | Fills path interior with fill style. |
| `canvas_line` / `cv_draw_line` | `canvas_line(cv, x1, y1, x2, y2)` | Draws line between two points. |
| `canvas_fill_rect` / `cv_fill_rect` | `canvas_fill_rect(cv, x, y, w, h)` | Fills rectangle. |
| `canvas_stroke_rect` / `cv_stroke_rect` | `canvas_stroke_rect(cv, x, y, w, h)` | Outlines rectangle. |
| `canvas_clear_rect` | `canvas_clear_rect(cv, x, y, w, h)` | Clears rectangular region to transparent. |
| `canvas_fill_round_rect` | `canvas_fill_round_rect(cv, x, y, w, h, radius)` | Fills rounded rectangle. |
| `canvas_stroke_round_rect` | `canvas_stroke_round_rect(cv, x, y, w, h, radius)` | Outlines rounded rectangle. |
| `canvas_fill_circle` / `cv_fill_circle` | `canvas_fill_circle(cv, cx, cy, radius)` | Fills circle. |
| `canvas_stroke_circle` / `cv_stroke_circle`| `canvas_stroke_circle(cv, cx, cy, radius)` | Outlines circle. |
| `canvas_fill_ellipse` | `canvas_fill_ellipse(cv, cx, cy, rx, ry)` | Fills ellipse. |
| `canvas_stroke_ellipse` | `canvas_stroke_ellipse(cv, cx, cy, rx, ry)` | Outlines ellipse. |
| `canvas_fill_polygon` | `canvas_fill_polygon(cv, points_list)` | Fills arbitrary polygon defined by `[[x, y], ...]`. |
| `canvas_stroke_polygon` | `canvas_stroke_polygon(cv, points_list)` | Outlines polygon. |

### Text & Metrics
| Function | Signature | Description |
|---|---|---|
| `canvas_draw_text` / `cv_draw_text` | `canvas_draw_text(cv, text, x, y)` | Draws filled text string at coordinates. |
| `canvas_stroke_text` | `canvas_stroke_text(cv, text, x, y)` | Outlines text string at coordinates. |
| `canvas_measure_text` | `canvas_measure_text(cv, text)` | Returns text metrics: `{width, height, ascent, descent}`. |
| `canvas_set_font` | `canvas_set_font(cv, size, [name])` | Configures font face and pixel size. |

### State & Transforms
| Function | Signature | Description |
|---|---|---|
| `canvas_save` | `canvas_save(cv)` | Saves current transform and style state onto the stack. |
| `canvas_restore` | `canvas_restore(cv)` | Restores previous canvas state from stack. |
| `canvas_translate` | `canvas_translate(cv, dx, dy)` | Translates coordinate origin by `(dx, dy)`. |
| `canvas_rotate` | `canvas_rotate(cv, rad)` | Rotates coordinate space by radians. |
| `canvas_scale` | `canvas_scale(cv, sx, sy)` | Scales coordinate space by factors `sx, sy`. |
| `canvas_set_fill_style` | `canvas_set_fill_style(cv, color_or_gradient)` | Sets active fill color or gradient handle. |
| `canvas_set_stroke_style` | `canvas_set_stroke_style(cv, color_or_gradient)` | Sets active outline color or gradient handle. |
| `canvas_set_line_width` | `canvas_set_line_width(cv, width)` | Stroke line width in pixels. |
| `canvas_set_global_alpha` | `canvas_set_global_alpha(cv, alpha)` | Sets opacity factor from `0.0` to `1.0`. |

### Gradients, Image Blitting & Effects
| Function | Signature | Description |
|---|---|---|
| `canvas_create_linear_gradient` | `canvas_create_linear_gradient(x1, y1, x2, y2)` | Creates linear gradient along line. |
| `canvas_create_radial_gradient` | `canvas_create_radial_gradient(cx1, cy1, r1, cx2, cy2, r2)` | Creates radial gradient between circles. |
| `canvas_gradient_add_color_stop` | `canvas_gradient_add_color_stop(gradient, offset, color)` | Adds color stop (`offset` $\in [0, 1]$). |
| `canvas_blit` | `canvas_blit(dst_cv, src_cv, dx, dy, [sx, sy, sw, sh])` | Blits pixels from source canvas to destination. |
| `canvas_blur` | `canvas_blur(cv, radius, [x, y, w, h])` | Applies Gaussian blur filter over region. |
| `canvas_draw_shadow` | `canvas_draw_shadow(cv, x, y, w, h, radius, blur, color)` | Draws drop shadow under rectangular area. |
| `canvas_draw_glow` | `canvas_draw_glow(cv, x, y, w, h, blur, color)` | Draws outer glow aura. |
| `canvas_draw_glass` | `canvas_draw_glass(cv, x, y, w, h, [blur, tint])` | Draws frosted glass / blur backdrop effect. |

---

## 15. Colors & Palettes

All colors in Grain are represented as 32-bit ARGB integers (`0xAARRGGBB`).

| Function | Signature | Description |
|---|---|---|
| `color_rgb` / `rgb` | `color_rgb(r, g, b)` | Combines RGB values ($0-255$) with full opacity (`0xFF`). |
| `color_rgba` / `rgba` | `color_rgba(r, g, b, a)` | Combines RGBA values ($0-255$) into 32-bit integer. |
| `color_hex` | `color_hex(hex_str)` | Parses hex string (`"#RRGGBB"` or `"#RRGGBBAA"`). |
| `color_to_hex` | `color_to_hex(color_int)` | Converts 32-bit color integer to `"#RRGGBB"` hex string. |
| `color_parse` | `color_parse(val)` | Parses color integer, hex string, or named CSS color. |
| `color_hsl` | `color_hsl(h, s, l)` | HSL to 32-bit ARGB ($h \in [0, 360], s, l \in [0, 1]$). |
| `color_hsla` | `color_hsla(h, s, l, a)` | HSLA to 32-bit ARGB. |
| `color_to_hsl` | `color_to_hsl(color_int)` | Converts color integer to `{h, s, l}` map. |
| `color_lighten` | `color_lighten(color, [amount])` | Lightens color luminance (default 0.1). |
| `color_darken` | `color_darken(color, [amount])` | Darkens color luminance (default 0.1). |
| `color_blend` | `color_blend(c1, c2, [factor])` | Blends two colors (factor 0.0 to 1.0; default 0.5). |
| `color_invert` | `color_invert(color)` | Inverts RGB channels. |
| `color_luminance` | `color_luminance(color)` | Calculates perceived relative luminance ($0.0-1.0$). |
| `color_contrast` | `color_contrast(c1, c2)` | Calculates WCAG contrast ratio between two colors ($1.0-21.0$). |
| `palette_analogous` | `palette_analogous(color, [count, [step]])` | Generates list of harmonious analogous colors. |
| `palette_triadic` | `palette_triadic(color)` | Generates 3-color triadic harmony palette. |
| `palette_monochromatic`| `palette_monochromatic(color, [count])` | Generates monochromatic brightness steps. |

---

## 16. UI Widgets, Charts & Windows

### Drawing Primitives
| Function | Signature | Description |
|---|---|---|
| `draw_button` | `draw_button(cv, x, y, w, h, label, [variant, state])` | Renders styled UI button (`"primary"`, `"secondary"`, `"danger"`). |
| `draw_badge` | `draw_badge(cv, x, y, label, [bg, text_col, radius])` | Renders compact status badge / pill. |
| `draw_card` | `draw_card(cv, x, y, w, h, [radius, bg, border, shadow])` | Renders container card with border and shadow. |
| `draw_progress_bar` | `draw_progress_bar(cv, x, y, w, h, pct, [fill, bg, rad])` | Renders progress bar indicator ($0.0-1.0$). |
| `draw_sparkline` | `draw_sparkline(cv, x, y, w, h, data, [line_col, fill, w])`| Renders compact inline trend graph. |
| `draw_bar_chart` | `draw_bar_chart(cv, x, y, w, h, data, [labels, bar_col, bg])`| Renders multi-series bar chart. |
| `draw_donut_chart` | `draw_donut_chart(cv, cx, cy, outer_r, inner_r, data, [colors])` | Renders proportional donut / pie chart. |
| `draw_metric_tile` | `draw_metric_tile(cv, x, y, w, h, title, val, [unit, delta, good, spark])` | Renders KPI dashboard metric tile. |
| `draw_icon` | `draw_icon(cv, x, y, size, icon_name, [col, width])` | Renders vector icon (check, cross, arrow, gear, database, etc.). |
| `draw_window_frame`| `draw_window_frame(cv, x, y, w, h, title, [sub, is_dark])` | Renders title bar, border, and window chrome. |
| `draw_menu_bar` | `draw_menu_bar(cv, x, y, w, items, [active_idx, is_dark])` | Renders top application menu bar. |
| `draw_toolbar` | `draw_toolbar(cv, x, y, w, tools, [active_id, is_dark])` | Renders horizontal action icon toolbar. |
| `draw_tab_bar` | `draw_tab_bar(cv, x, y, w, tabs, [active_idx, is_dark])` | Renders navigation tab strip. |
| `draw_nav_tree` | `draw_nav_tree(cv, x, y, w, h, nodes, [selected, scroll_y, dark])` | Renders expandable tree hierarchy view. |
| `draw_data_grid` | `draw_data_grid(cv, x, y, w, h, cols, rows, [options])` | Renders interactive tabular data grid. |
| `draw_sql_editor` | `draw_sql_editor(cv, x, y, w, h, code, [cur_l, cur_c, scroll, dark])` | Renders syntax-highlighted SQL editor buffer. |
| `draw_status_bar` | `draw_status_bar(cv, x, y, w, h, host, db, stats, enc, [conn, dark])` | Renders bottom application status line. |
| `draw_autocomplete_popup` | `draw_autocomplete_popup(cv, x, y, w, items, active_idx, [dark])` | Renders popup suggestion list. |

### Window Panes Lifecycle
| Function | Signature | Description |
|---|---|---|
| `create_pane` / `register_pane` | `create_pane(options_map)` | Registers a window or live browser pane (`{title, width, height, on_draw, on_event}`). |
| `unregister_pane` / `window_close` | `unregister_pane(pane_id)` | Closes and cleans up a window pane. |
| `list_panes` | `list_panes()` | Returns list of currently active pane IDs. |
| `pane_render` | `pane_render(pane_id, [force])` | Invalidates and redraws a pane. |
| `pane_dispatch_event` | `pane_dispatch_event(pane_id, event, [data])` | Delivers click, keyboard, or resize event to pane handler. |
| `pane_get_canvas` | `pane_get_canvas(pane_id)` | Returns the underlying drawing canvas for a pane. |
| `pane_set_bounds` | `pane_set_bounds(pane_id, x, y, [w, h])` | Updates position and dimensions of a pane. |
| `pane_get` | `pane_get(pane_id)` | Returns pane state metadata dictionary. |
| `run_panes` | `run_panes([timeout_ms])` | Starts the window system event dispatch loop. |

### Markup in a `ui.pane` view

A `ui.pane` view may return `el()` / `html_raw()` nodes (see *Building markup*) anywhere a widget goes, and `ui.*`
widgets may sit inside the markup. In a browser (`grain serve`, an app host) the page is the web UI's: sessions,
actions, the pushed re-render and its in-place update all work as for widgets.

- **Actions:** a function in `on_click` (any element), `on_change` (an `input` or `select`) or `on_enter` is called
  like a button's, with the fields as its argument: `el("th", {"on_click": fn() { desc = not desc }}, "Touches")`.
  Any other function-valued attribute is an error.
- **Fields:** `bind: "id"` makes an input a field: its value travels with every action (`fn(f) { start = f.start }`)
  and is kept across renders like a `ui.field`'s.
- **State** such as a sort or a range lives in the program (one copy per viewer unless the pane is `"shared"`), so
  a re-render keeps it; markup pushed into an open page is merged in place, and a `<script>` inside it does not run.
- **`css`** in the pane's options is added to the page.
- **Databases with `"workers": true`:** each route runs in a VM copied from the program, and a connection cannot be
  copied. A view that reads a database opens its own connection per render (`let c = db_connect(cfg)` ... `db_close(c)`)
  rather than keeping one in a global its routes also use (the copy of such a route fails: "a grain_db cannot be
  sent to a worker").
- **`watch`** for a Thing: `"watch": {"thing": "touch_day", "db": DB_CFG, "timeout": 10}` - connect with `db_connect(DB_CFG)`,
  `WAIT THING touch_day SINCE <since> TIMEOUT 10`, close, return the clock: the function below, written for you.
- **`watch`** in the pane's options is `fn(since)`: the host calls it on a thread of its own, in a VM of its own,
  again each time it returns, with what it returned last time (`nil` the first time). It should wait until the
  data may have changed (`WAIT THING t SINCE <since> TIMEOUT 10`) and return something that moves when it does
  (the Thing's clock). When the value moves, every open page's view is looked at again and pushed if it changed;
  no page script and no polling. A failing watch is retried after 5 s; one that returns at once is called at most
  4 times a second. Stopping the app waits for a call in progress, so keep its timeout short.
- **`machine`** in the pane's options gives the whole pane - page, actions, push and routes - to that fleet member's
  copy of the app (`docs/aios-serve.md`); `""` or this machine: here.
- A native client (`application/x-grain-ui+json`) gets markup as `{"widget": "html", "html": ...}`; a desktop
  face window shows its text for now (`docs/design/one-app-every-surface.md`, stage 4).

---

## 17. AIOS Kernel, Capabilities & Sidecars

These builtins provide direct access to the AIOS capability system, microkernel primitives, and acceleration sidecars.

### Microkernel & Capabilities
| Function | Signature | Description |
|---|---|---|
| `id128` | `id128([hi, lo])` | Generates or constructs a 128-bit kernel entity identity. |
| `Capability` / `capability` | `Capability(holder, resource, rights, [opts])` | Constructs an explicit kernel capability object. |
| `cap_mint` | `cap_mint(target, res, rights, [opts])` | Mints a child capability derived from current authority. |
| `MemoryObject` / `memory_allocate` | `memory_allocate(size, [opts])` | Allocates managed kernel memory object. |
| `StateChannel` / `channel_create` | `channel_create(size, [opts])` | Creates shared-memory ring channel for inter-entity IPC. |
| `StateFrame` / `state_frame` | `state_frame(payload, [opts])` | Constructs an immutable state snapshot frame. |

### Sidecar Services
| Function | Signature | Description |
|---|---|---|
| `sat` | `sat(clauses, [options])` | Native CDCL SAT solver: solves propositional formulas, returning model or proof. |
| `sidecar_haversine` | `sidecar_haversine(lat1, lon1, lat2, lon2)` | Fast SIMD sidecar distance calculation. |
| `sidecar_tsp` | `sidecar_tsp(coordinates_list)` | Traveling Salesperson solver via acceleration sidecar. |
| `sidecar_sha256` | `sidecar_sha256(data)` | High-throughput hardware-accelerated SHA256 sidecar. |
| `sidecar_keccak256` | `sidecar_keccak256(data)` | High-throughput hardware-accelerated Keccak256 sidecar. |
| `sidecar_eth_addr` | `sidecar_eth_addr(pubkey)` | Derives Ethereum address from secp256k1 public key. |
| `sidecar_btc_addr` | `sidecar_btc_addr(pubkey, [net])` | Derives Bitcoin address (mainnet/testnet) from public key. |
| `sidecar_parse_eml` | `sidecar_parse_eml(raw_eml)` | Parses RFC 5322 email MIME structure. |
| `sidecar_parse_ical` | `sidecar_parse_ical(raw_ical)` | Parses iCalendar events. |
| `sidecar_parse_vcard`| `sidecar_parse_vcard(raw_vcard)` | Parses vCard contact records. |
| `sidecar_parse_feed` | `sidecar_parse_feed(raw_xml)` | Parses RSS / Atom syndication feeds. |
| `sidecar_svg_png_len`| `sidecar_svg_png_len(svg, [w, h])` | Fast rasterization of SVG to PNG. |
| `sidecar_pdf_len` | `sidecar_pdf_len(pdf_data, [opts])` | Inspects and measures PDF documents. |
| `sidecar_cap_mint` | `sidecar_cap_mint(target, res, rights, [opts])` | Sidecar capability minting service. |
| `sidecar_cap_attenuate`| `sidecar_cap_attenuate(cap_id, narrower_rights)` | Attenuates existing capability to narrower rights. |
| `sidecar_patch_check`| `sidecar_patch_check(patch_diff)` | AST-aware validation of source code patches. |

---

## 18. Testing, Assertions & Errors

| Function | Signature | Description |
|---|---|---|
| `assert` | `assert(condition, [message])` | Raises an assertion error if `condition` is falsy. |
| `assert_eq` | `assert_eq(actual, expected, [msg])` | Asserts that `actual == expected`. |
| `assert_ne` | `assert_ne(a, b, [msg])` | Asserts that `a != b`. |
| `assert_true` | `assert_true(condition, [msg])` | Asserts that condition evaluates strictly to `true`. |
| `assert_false` | `assert_false(condition, [msg])`| Asserts that condition evaluates strictly to `false`. |
| `error` / `panic` | `error([message])` | Terminates execution with a runtime error message. |
| `try_call` / `pcall` | `try_call(fn, ...args)` | Protected call: runs `fn` catching runtime errors. Returns `{"ok": bool, "value": val, "error": str}`. |
| `is_ok` | `is_ok(result)` | Returns `true` if protected call result succeeded (`result.ok == true`). |
| `is_err` | `is_err(result)` | Returns `true` if protected call result caught an error. |
