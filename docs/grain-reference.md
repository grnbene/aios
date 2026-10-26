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
| `min` | `min(...nums)` | Returns the minimum of one or more numbers. |
| `max` | `max(...nums)` | Returns the maximum of one or more numbers. |
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
| `sort_by` | `sort_by(list, key_fn, [desc])` | Sorts list by extracted key; optional descending flag. |
| `group_by` | `group_by(list, key_fn)` | Groups items into a map of lists keyed by `key_fn(item)`. |
| `key_by` | `key_by(list, key_fn)` | Maps list into dictionary keyed by `key_fn(item)` (first wins). |
| `sum_by` | `sum_by(list, key_fn, val_fn)` | Groups items and aggregates totals per key. |
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
| `date_parts` | `date_parts(ts, [utc])` | Returns map: `{year, month, day, hour, min, sec, wday, yday, isdst}`. |
| `date_format` | `date_format(fmt, ts, [utc])` | Formats timestamp using `strftime`-style format patterns. |
| `date_parse` | `date_parse(str, [fmt])` | Parses date/time string into fractional UNIX timestamp. |
| `date_make` | `date_make(y, m, d, [h, min, s, utc])` | Constructs UNIX timestamp from year, month, day, etc. |
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
| `res_error` | `res_error(message, [status])` | Returns error response map (default 400). |
| `res_redirect` | `res_redirect(location, [status])` | Returns redirect response map (default 302). |
| `res_sse` | `res_sse(events_list, [headers])` | Returns response formatted as `text/event-stream`. |
| `on_machine` | `on_machine(machine_id, fn)` | Dispatches route execution to a specific fleet node. |

---

## 13. Databases & ASQL

### MySQL & MariaDB Client
| Function | Signature | Description |
|---|---|---|
| `db_connect` | `db_connect(host, port, user, db, [opts])` | Connects to MySQL/MariaDB or ASQL server. Returns DB handle. |
| `db_query` | `db_query(handle, sql, [params, [opts]])` | Executes query; returns list of row maps. |
| `db_escape` | `db_escape(str)` | Escapes special characters for SQL literals. |
| `db_quote` | `db_quote(str)` | Wraps string in single quotes with SQL escaping. |
| `db_close` | `db_close(handle)` | Closes database connection. |
| `db_transaction` | `db_transaction(conn, fn)` | Executes callback within a transaction (`START TRANSACTION` / `COMMIT`). Automatically rolls back on error (`ROLLBACK`). Supports nested calls via `SAVEPOINT`. |
| `db_ping` | `db_ping(handle)` | Pings server to verify connection vitality. |
| `db_connected` | `db_connected(handle)` | Returns `true` if connection handle remains open. |
| `db_select_db` | `db_select_db(handle, db_name)` | Switches default database on the connection. |
| `db_tables` | `db_tables(handle, [db_name])` | Returns list of table names in database. |
| `db_columns` | `db_columns(handle, table)` | Returns column metadata for table. |
| `db_schema` | `db_schema(handle)` | Introspects full schema of the database. |
| `db_build_update` | `db_build_update(table, data_map, key_col, key_val)` | Constructs parameterized UPDATE statement and parameter values. |
| `live` | `live(handle, query_or_thing, [callback])` | Subscribes to real-time binlog row mutations on ASQL. |

### Embedded ASQL Engine
| Function | Signature | Description |
|---|---|---|
| `asql_query` | `asql_query(sql, [params, [options]])` | Executes query directly in the in-tree ASQL engine. |
| `asql_exec` | `asql_exec(sql, [params, [options]])` | Executes non-query DDL/DML in the in-tree ASQL engine. |
| `thing_conflicts` | `thing_conflicts(thing_name, [options])` | Inspects conflict records for an ASQL reactive Thing. |
| `sql_tokenize` | `sql_tokenize(sql_str)` | Tokenizes SQL statement into lexical tokens. |
| `sql_format` | `sql_format(sql_str)` | Formats and indents SQL query string. |
| `sql_suggest` | `sql_suggest(sql, [cursor_pos, schema])` | Auto-complete suggestions for SQL keywords, tables, and columns. |
| `sql_generate_ddl`| `sql_generate_ddl(table, cols)` | Generates CREATE TABLE DDL from column specifications. |

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
