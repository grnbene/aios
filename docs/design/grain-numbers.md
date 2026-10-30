# grain's numbers: exact by default

A grain program that adds up prices gets the right total, a program that rounds gets one rounding rule, and a
number prints as what it is. Not PHP's float fudging, not Python's binary floats: what someone doing sums by hand
would get.

## What was wrong (2026-10-30)

| | grain gave | right |
|---|---|---|
| `0.1 + 0.2 == 0.3` | false | true |
| `int(4.35 * 100)` | 434 (`4.35 * 100` printed as `435`) | 435 |
| `round(1.005, 2)` / `number_format(1.005, 2)` | 1.0 / "1.01" | 1.01, both |
| `round(2.675, 2)` / `format("{:.2f}", 2.675)` | 2.68 / "2.67" | 2.68, both |
| `str(123456789.123456)` | "123456789.123" (12 digits) | "123456789.123456" |
| ten `+= 0.1` | 0.9999999999999999, printed "1" | 1.0 |
| `9223372036854775807 + 1` | -9223372036854775808 (wrapped) | 9223372036854775808 |
| `sqrt(decimal("2"))` | an error; `as_double()` of a decimal was 0.0 | 1.4142135623730951 |

## The kinds

- **int**: a whole number, 64-bit, exact.
- **decimal**: an exact decimal - a 128-bit integer and a scale (digits after the point, 0..30), 38 significant
  digits. Money, quantities, percentages, anything typed with a point.
- **real**: a binary floating-point number (IEEE double), inexact. What `sqrt`, `sin`, `log`, `exp`, a measurement
  from a sensor or a canvas coordinate is.

## The rules

1. **Literals.** `12` is an int. `19.99` is a decimal, exactly as written: `1.50` keeps its two places. A literal
   with an exponent (`1e9`, `6.02e23`, `2.5e-3`) is a real: scientific notation is for scientific numbers.
2. **int arithmetic never wraps.** `+ - *` of ints that would pass 64 bits give the exact decimal; past 38 digits
   it is an error ("number too large"), never a wrong answer. `%`, `idiv` and bit operations stay ints.
3. **`/` is exact.** int or decimal over int or decimal is an int when it divides evenly (`10 / 2` is `5`), a
   decimal when the quotient ends (`7 / 2` is `3.5`, `10.00 / 4` is `2.50`), and otherwise the exact fraction: held
   as numerator and denominator, so `(1 / 3) * 3` is `1` and `2 / 3 * 3` is `2` (a quotient cut to 12 places would
   give `2.000000000001`). A fraction prints to 12 places (more if its operands had more), rounds and formats exactly,
   and becomes a decimal again when arithmetic leaves its denominator only 2s and 5s. Past 128 bits, or a denominator
   past 10^36, it is rounded to 30 places. JSON writes a fraction to its printed places. `dec_div(a, b, places)`
   rounds the exact quotient once. Division by zero is an error.
4. **Mixing with a real gives a real.** `decimal * real`, `int + real`: the decimal or int becomes the nearest real
   and the answer is a real. No error: a real is inexact and the answer says so by being one.
5. **Comparison is by value.** `1 == 1.0 == 1.00`; an int and a decimal compare exactly; a real against a decimal
   or int compares as reals.
6. **One rounding rule.** A rounded zero has no sign (`0.00`, never `-0.00`). `round(x, places = 0, mode = "half_up")`, and `number_format` and `format("{:.Nf}")` round
   the same way. `half_up` is half away from zero (2.5 -> 3, -2.5 -> -3), as money is rounded; the other modes are
   `half_even` (banker's), `half_down`, `up` (away from zero), `down` (toward zero), `floor`, `ceil`. A decimal
   rounds exactly. A real rounds the shortest decimal that reads back as it - the number it prints as - so
   `round(2.675e0, 2)` is `2.68`, not the `2.67` its binary expansion gives. `round(x)` is an int; `round(x, n)`
   is a decimal with exactly `n` places (`round(2.5, 2)` prints `2.50`).
7. **Printing says what a number is.** An int prints its digits; a decimal prints its digits with its scale
   (`59.97`, `1.50`); a real prints the shortest text that reads back as the same real, with a point or an
   exponent so it never looks like an int (`0.30000000000000004`, `3.0`, `1e+16`). `str`, `print`, `repr`, JSON
   and string interpolation agree.
8. **Conversions are explicit and exact.** `int(x)` truncates toward zero; `floor` / `ceil` give ints;
   `decimal(x)` of a real takes the shortest text that reads back as it (`decimal(0.1e0)` is `0.1`); `real(x)`
   gives the nearest real; `to_number("19.99")` is a decimal.
9. **JSON keeps what it was sent.** `json_parse` reads `19.99` as a decimal, `1.5e3` as a real, an integer past 64
   bits as a decimal (up to 38 digits); `json_stringify` writes a decimal's digits exactly and a real's shortest.
10. **Everything that takes a number takes all three.** Builtins that want a number (canvas coordinates, `sleep`,
    `sqrt`) accept an int, a decimal or a real; math functions with an irrational answer (`sqrt`, `sin`, `log`,
    `exp`, `pow` with a fractional exponent) return a real; `abs`, `min`, `max`, `sum` keep the kind.

## Not changed

- Ints are still 64-bit on the fast path; the JIT's int arithmetic gains an overflow check (one `jo`) that goes to
  the slow path.
- `decimal("...")`, `dec_div`, `scale`, `round_sig`, `number_format` keep their names and arguments.

## ASQL

The engine already keeps DECIMAL columns exact in most places (`sql_active_data.cpp` sums in units of 1e-9,
`sql_database.cpp` ROUND in decimal half away from zero). The same work for ASQL is a sweep against the local
MariaDB 10.11: every arithmetic, aggregate and rounding function on DECIMAL, DOUBLE and integer columns and
literals, compared to the cent and the last digit, each difference fixed.

## Done (2026-10-30)

Steps 1-6: 08afe12 (decimals in every builtin, mixing), ec49f9e (printing), b9828f1 (one rounding), 57222d8 (ints
never wrap; x64 and arm64 JITs), 81a4c1d (exact /, fractions), and the literals and JSON step. Checked against
Python (repr, the decimal module's quantize in all seven modes, its ints), on the x64 and arm64 JITs, and every app
compiles. ASQL's sweep against the local MariaDB is next.

## Speed

A decimal today is a heap object per value; with decimals the default that matters. VM 2 (grain-vm2.md) holds a
decimal of up to 64 bits of mantissa inside the 16-byte value, so `19.99 * 3` allocates nothing. Until then the
decimal operations are made allocation-light where they can be.
