//# autook
//// literals: decimal, hex, octal, binary, separators, huge
eq(typeof 1n, "bigint"); eq(0n, 0n); eq(String(0xffn), "255"); eq(String(0o17n), "15"); eq(String(0b1011n), "11"); eq(String(1_000_000n), "1000000");
eq(String(123456789012345678901234567890n), "123456789012345678901234567890");
eq(String(0xdeadbeefcafebabedeadbeefn), "68915718020162848921987432175");
//// literals: a property name is its canonical string
var o = { 1n: "a", 0x10n: "b" }; eq(o["1"], "a"); eq(o["16"], "b"); eq(Object.keys(o).join(), "1,16");
//// literals: malformed ones are syntax errors
throws(function () { eval("1.5n"); }, SyntaxError); throws(function () { eval("01n"); }, SyntaxError); throws(function () { eval("1e3n"); }, SyntaxError); throws(function () { eval("0x n"); }, SyntaxError);
//// typeof, wrappers and the prototype chain
eq(typeof Object(1n), "object"); eq(Object.prototype.toString.call(1n), "[object BigInt]"); eq(Object.getPrototypeOf(1n), BigInt.prototype);
eq(BigInt.prototype.constructor, BigInt); eq(BigInt.name, "BigInt"); eq(BigInt.length, 1); eq(Object(2n) * 3n, 6n); eq(typeof Object(1n).valueOf(), "bigint");
//// arithmetic: + - * and negation
eq(1n - 2n, -1n); eq(-(5n), -5n); eq(2n ** 100n, 1267650600228229401496703205376n); eq((-2n) ** 3n, -8n); eq(2n ** 0n, 1n);
eq(123456789012345678901234567890n * 987654321098765432109876543210n, 121932631137021795226185032733622923332237463801111263526900n);
//// arithmetic: division and remainder truncate toward zero
eq(7n / 2n, 3n); eq(-7n / 2n, -3n); eq(7n % 3n, 1n); eq(-7n % 3n, -1n); eq(7n % -3n, 1n);
eq(987654321098765432109876543210n / 123456789n, 8000000073700000670760n); eq(987654321098765432109876543210n % 123456789n, 753570n);
//// division: quotient and remainder agree with multiplication for large operands
var seed = 12345n; function rnd() { seed = (seed * 6364136223846793005n + 1442695040888963407n) % (2n ** 64n); return seed; }
for (var k = 0; k < 300; k++) {
  var a = rnd() * rnd() * rnd() * (k % 2 ? -1n : 1n), b = rnd() * (k % 3 ? rnd() : 1n) + 1n;
  var q = a / b, r = a % b;
  assert(q * b + r === a, "identity " + a + " " + b);
  assert(r === 0n || (r < 0n) === (a < 0n), "remainder sign");
  assert((r < 0n ? -r : r) < b, "remainder size");
}
//// bitwise operators use two's complement on negatives
eq(5n & 3n, 1n); eq(5n | 3n, 7n); eq(5n ^ 3n, 6n); eq(-5n & 3n, 3n); eq(-5n | 3n, -5n); eq(-5n ^ 3n, -8n); eq(-5n & -3n, -7n); eq(~5n, -6n); eq(~-5n, 4n);
eq((-(2n ** 64n)) & 0xffffffffffffffffn, 0n); eq((2n ** 64n - 1n) ^ -1n, -(2n ** 64n));
//// shifts: left, arithmetic right, negative counts, huge counts
eq(1n << 70n, 1180591620717411303424n); eq(-1n << 70n, -1180591620717411303424n); eq((1n << 70n) >> 69n, 2n); eq(-5n >> 1n, -3n); eq(-1n >> 100n, -1n); eq(5n >> 100n, 0n); eq(5n << -1n, 2n); eq(5n >> -2n, 20n);
//// mixing BigInt and Number is a TypeError, and so are +x and >>>
throws(function () { return 1n + 1; }, TypeError); throws(function () { return 1 * 1n; }, TypeError); throws(function () { return +1n; }, TypeError);
throws(function () { return 1n >>> 0n; }, TypeError); throws(function () { return Math.max(1n); }, TypeError); throws(function () { return isNaN(1n); }, TypeError);
//// division by zero, negative exponent, absurd sizes are RangeErrors
throws(function () { return 1n / 0n; }, RangeError); throws(function () { return 1n % 0n; }, RangeError); throws(function () { return 2n ** -1n; }, RangeError);
throws(function () { return 2n ** (2n ** 40n); }, RangeError); throws(function () { return 1n << (2n ** 40n); }, RangeError);
//// comparison with BigInt, Number, string and boolean
assert(1n < 2); assert(2n > 1); assert(1n == 1); assert(!(1n === 1)); assert(1n == "1"); assert(1n < "2"); assert(!(1n < "x")); assert(!(1n > "x")); assert(1n === 1n);
assert(2n > 1.5); assert(2n < 2.5); assert(!(2n == 2.5)); assert(2n ** 64n > 1e19); assert(2n ** 64n < 1.9e19); assert(0n == false); assert(1n == true); assert(0n == ""); assert(1n == Object(1n));
assert(!(NaN < 1n)); assert(!(1n < NaN)); assert(Infinity > 2n ** 2000n); assert(-Infinity < -(2n ** 2000n)); assert(!(1n == Symbol()));
//// sorting and Map/Set keys compare by value
eq([3n, 10n, 2n].sort(function (a, b) { return a < b ? -1 : a > b ? 1 : 0; }).join(), "2,3,10");
var m = new Map(); m.set(1n, "a"); m.set(2n ** 70n, "b"); eq(m.get(1n), "a"); eq(m.get(2n ** 70n), "b"); assert(!m.has(2n)); assert(!m.has(1));
eq(new Set([1n, 1n, 2n, 2n ** 70n, 2n ** 70n]).size, 3); assert([1n, 2n].includes(2n)); eq([1n, 2n].indexOf(2n), 1); assert(Object.is(1n, 1n)); assert(Object.is(0n, -0n));
//// switch uses strict equality
var r; switch (2n) { case 1n: r = "one"; break; case 2n: r = "two"; break; default: r = "other"; } eq(r, "two");
switch (1n) { case 1: r = "number"; break; default: r = "other"; } eq(r, "other");
//// BigInt(): numbers, strings, booleans, and what it refuses
eq(BigInt(10), 10n); eq(BigInt("0x1f"), 31n); eq(BigInt("  42  "), 42n); eq(BigInt(""), 0n); eq(BigInt(true), 1n); eq(BigInt(Number.MAX_SAFE_INTEGER) + 2n, 9007199254740993n);
eq(BigInt(1e21), 1000000000000000000000n); eq(BigInt("-17"), -17n);
throws(function () { BigInt(1.5); }, RangeError); throws(function () { BigInt(NaN); }, RangeError); throws(function () { BigInt("1.5"); }, SyntaxError); throws(function () { BigInt("abc"); }, SyntaxError);
throws(function () { BigInt("-0x1"); }, SyntaxError); throws(function () { BigInt(undefined); }, TypeError); throws(function () { BigInt(null); }, TypeError); throws(function () { BigInt(Symbol()); }, TypeError);
throws(function () { new BigInt(1); }, TypeError);
//// Number(bigint) rounds to nearest, ties to even
eq(Number(123n), 123); eq(Number(2n ** 64n), 18446744073709552000); eq(Number(2n ** 1024n), Infinity); eq(Number(-(2n ** 53n) - 1n), -9007199254740992);
eq(Number(9007199254740993n), 9007199254740992); eq(Number(9007199254740995n), 9007199254740996); eq(Number(2n ** 80n + 1n), 1208925819614629174706176);
//// toString, toLocaleString, string concatenation
eq(String(123n), "123"); eq(123n + "x", "123x"); eq(`${5n}`, "5"); eq((255n).toString(16), "ff"); eq((255n).toString(2), "11111111"); eq((-255n).toString(36), "-73");
eq((1234567890123n).toLocaleString(), "1,234,567,890,123"); eq((-1234n).toLocaleString(), "-1,234"); eq((12n).toLocaleString(), "12");
throws(function () { (1n).toString(1); }, RangeError); eq(parseInt("12n"), 12);
//// asIntN and asUintN
eq(BigInt.asIntN(8, 255n), -1n); eq(BigInt.asIntN(8, 128n), -128n); eq(BigInt.asIntN(8, -129n), 127n); eq(BigInt.asUintN(8, -1n), 255n); eq(BigInt.asUintN(64, -1n), 18446744073709551615n);
eq(BigInt.asIntN(64, 2n ** 63n), -(2n ** 63n)); eq(BigInt.asIntN(0, 5n), 0n); eq(BigInt.asUintN(3, 10n), 2n); eq(BigInt.asIntN(8, -128n), -128n); eq(BigInt.asIntN(200, -5n), -5n);
//// increment, decrement and compound assignment
var x = 5n; x++; eq(x, 6n); ++x; eq(x, 7n); x--; eq(x, 6n); var y = x++; eq(y, 6n); eq(x, 7n); x += 10n; eq(x, 17n); x *= 3n; eq(x, 51n); x **= 2n; eq(x, 2601n); x <<= 2n; eq(x, 10404n); x %= 7n; eq(x, 2n);
var z = 0n; for (var i = 0n; i < 10n; i++) z += i; eq(z, 45n);
//// truthiness and logical operators
assert(!0n); assert(!!1n); eq(0n ? "a" : "b", "b"); eq(1n && "yes", "yes"); eq(0n || "no", "no"); eq(1n ?? 2, 1n);
//// JSON refuses BigInt unless toJSON is supplied
throws(function () { JSON.stringify({ a: 1n }); }, TypeError);
BigInt.prototype.toJSON = function () { return this.toString() + "n"; }; var s = JSON.stringify({ a: 1n, b: [2n] }); delete BigInt.prototype.toJSON; eq(s, '{"a":"1n","b":["2n"]}');
//// BigInt64Array and BigUint64Array
var a = new BigInt64Array(4); a[0] = 5n; a[1] = -5n; a[2] = 2n ** 63n; a[3] = 2n ** 64n + 7n;
eq(a.join(), "5,-5,-9223372036854775808,7"); eq(a.byteLength, 32); eq(BigInt64Array.BYTES_PER_ELEMENT, 8);
var u = new BigUint64Array(a.buffer); eq(u[1], 18446744073709551611n); eq(typeof u[0], "bigint");
throws(function () { a[0] = 1; }, TypeError); throws(function () { new BigInt64Array([1, 2]); }, TypeError); eq((a[0] = "12", a[0]), 12n);
throws(function () { new Float64Array(new BigInt64Array(1)); }, TypeError); throws(function () { new BigInt64Array(new Float64Array(1)); }, TypeError);
throws(function () { new BigInt64Array(2).set(new Int8Array(2)); }, TypeError); throws(function () { new BigInt64Array(2).set([1, 2]); }, TypeError);
eq(new BigInt64Array([3n, 1n, 2n, -1n]).sort().join(), "-1,1,2,3"); eq(new BigUint64Array([3n, 1n, 2n, -1n]).sort().join(), "1,2,3,18446744073709551615");
eq(new BigInt64Array([3n, 1n, 2n]).sort(function (x, y) { return x < y ? 1 : x > y ? -1 : 0; }).join(), "3,2,1");
eq(new BigInt64Array([1n, 2n, 3n]).reverse().join(), "3,2,1"); eq(new BigInt64Array([1n, 2n, 3n]).map(function (x) { return x * 2n; }).join(), "2,4,6");
eq(new BigInt64Array([1n, 2n, 3n]).filter(function (x) { return x > 1n; }).join(), "2,3"); eq(new BigInt64Array([1n, 2n, 3n]).reduce(function (s, v) { return s + v; }, 0n), 6n);
eq(new BigInt64Array(3).fill(7n).join(), "7,7,7"); throws(function () { new BigInt64Array(3).fill(7); }, TypeError);
eq(new BigInt64Array([1n, 2n, 3n]).indexOf(2n), 1); assert(new BigInt64Array([1n, 2n, 3n]).includes(3n)); eq(new BigInt64Array([1n, 2n, 3n]).at(-1), 3n);
eq(new BigInt64Array([1n, 2n, 3n]).with(0, 9n).join(), "9,2,3"); eq(new BigInt64Array([3n, 2n, 1n]).toSorted().join(), "1,2,3"); eq(new BigInt64Array([1n, 2n, 3n]).toReversed().join(), "3,2,1");
eq(BigInt64Array.from([1n, 2n]).join(), "1,2"); eq(BigUint64Array.of(1n, 2n).join(), "1,2"); eq(Object.prototype.toString.call(new BigInt64Array(1)), "[object BigInt64Array]");
var t = new BigInt64Array([5n]); t[0]++; eq(t[0], 6n); t[0] += 10n; eq(t[0], 16n);
//// DataView get/setBigInt64 and get/setBigUint64
var dv = new DataView(new ArrayBuffer(16)); dv.setBigInt64(0, -2n); dv.setBigUint64(8, 2n ** 64n - 1n, true);
eq(dv.getBigInt64(0), -2n); eq(dv.getBigUint64(0), 18446744073709551614n); eq(dv.getBigInt64(0, true), -72057594037927937n); eq(dv.getBigUint64(8, true), 18446744073709551615n); eq(dv.getBigInt64(8, true), -1n);
eq(dv.getUint8(0), 255); eq(dv.getUint8(7), 254); throws(function () { dv.setBigInt64(0, 1); }, TypeError); throws(function () { dv.getBigInt64(9); }, RangeError);
//// structuredClone and Object.assign keep BigInt values
eq(Object.assign({}, { a: 5n }).a, 5n); eq([1n, 2n].concat([3n]).join(), "1,2,3"); eq([5n, 6n].map(String).join(), "5,6");
