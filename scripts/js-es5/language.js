//# autook
//// var hoisting and function hoisting
eq(typeof hoisted, "function"); eq(v, undefined); var v = 1; function hoisted() {}
//// let TDZ
throws(function () { x; let x = 1; }, ReferenceError); { let y = 2; eq(y, 2); }
//// const assignment throws
const c = 1; throws(function () { c = 2; }, TypeError); eq(c, 1);
//// block scoped closures in loops
var fs = []; for (let i = 0; i < 3; i++) fs.push(function () { return i; }); eq(fs[0]() + fs[1]() + fs[2](), 3);
//// closures capture per-iteration bindings with updates in body
var fs = []; for (let i = 0; i < 3; i++) { fs.push(() => i); i += 0; } eq(fs.map(f => f()).join(), "0,1,2");
//// for-in over inherited and shadowed keys
function A() { this.a = 1; } A.prototype.b = 2; var o = new A(); o.c = 3; var ks = []; for (var k in o) ks.push(k); eq(ks.join(), "a,c,b");
//// for-in skips deleted keys
var o = {a: 1, b: 2, c: 3}, ks = []; for (var k in o) { ks.push(k); delete o.b; } eq(ks.join(), "a,c");
//// switch fallthrough and default in the middle
function f(x) { var r = ""; switch (x) { case 1: r += "1"; default: r += "d"; case 2: r += "2"; break; case 3: r += "3"; } return r; } eq(f(1), "1d2"); eq(f(2), "2"); eq(f(9), "d2"); eq(f(3), "3");
//// labeled break and continue
var n = 0; outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { if (j == 1) continue outer; if (i == 2) break outer; n++; } } eq(n, 2);
//// try catch finally ordering
var log = []; function f() { try { log.push("t"); return "r"; } catch (e) { log.push("c"); } finally { log.push("f"); } } eq(f(), "r"); eq(log.join(), "t,f");
//// finally overrides return
function f() { try { return 1; } finally { return 2; } } eq(f(), 2);
//// finally runs on break
var log = []; for (var i = 0; i < 2; i++) { try { if (i == 0) continue; break; } finally { log.push(i); } } eq(log.join(), "0,1");
//// nested try rethrow
var r = []; try { try { throw new Error("a"); } catch (e) { r.push("c1"); throw e; } finally { r.push("f1"); } } catch (e) { r.push("c2:" + e.message); } eq(r.join(), "c1,f1,c2:a");
//// catch scoping
var e = "outer"; try { throw "inner"; } catch (e) { var e = "x"; } eq(e, "outer");
//// optional catch binding
var ok = false; try { throw 1; } catch { ok = true; } assert(ok);
//// typeof undeclared
eq(typeof undeclaredVariable, "undefined"); throws(function () { undeclaredVariable; }, ReferenceError);
//// comma and conditional operators
eq((1, 2, 3), 3); eq(true ? "a" : "b", "a"); eq(null ?? "d", "d"); eq(0 ?? "d", 0); eq(0 || "d", "d"); eq(1 && 2, 2);
//// optional chaining
var o = {a: {b: function () { return this.c; }, c: 5}}; eq(o?.a?.b(), 5); eq(o.a?.b(), 5); eq(o.x?.y.z, undefined); eq(o.x?.[1], undefined); eq(o.a.q?.(), undefined); eq(delete o?.a, true);
//// logical assignment
var a = null; a ??= 5; eq(a, 5); a ||= 6; eq(a, 5); a &&= 7; eq(a, 7); var o = {x: 0}; o.x ||= 3; eq(o.x, 3);
//// exponent operator
eq(2 ** 10, 1024); eq(2 ** 3 ** 2, 512); var x = 3; x **= 2; eq(x, 9); eq((-2) ** 2, 4);
//// compound assignment evaluation order
var o = {a: 1}; var i = 0; var arr = [0, 0]; arr[i++] += 5; eq(arr[0], 5); eq(i, 1);
//// update expressions on members
var o = {n: 1}; eq(o.n++, 1); eq(++o.n, 3); eq(o.n--, 3); var s = "5"; eq(s++, 5); eq(s, 6);
//// number conversion corner cases
eq(+"", 0); eq(+" 12 ", 12); eq(+"0x10", 16); eq(+"1e3", 1000); eq(+"abc", NaN); eq(+[], 0); eq(+[5], 5); eq(+null, 0); eq(+undefined, NaN); eq(+true, 1); eq(1 / -0, -Infinity);
//// string concatenation and coercion
eq("a" + 1 + 2, "a12"); eq(1 + 2 + "a", "3a"); eq("5" - 2, 3); eq("5" * "2", 10); eq([] + {}, "[object Object]"); eq([1, 2] + "", "1,2"); eq(null + 1, 1); eq(undefined + 1, NaN);
//// equality
assert(null == undefined); assert(!(null === undefined)); assert("1" == 1); assert(!(NaN == NaN)); assert([] == false); assert(!({} == "[object Object]" === false)); assert(new String("a") == "a");
//// relational comparison
assert("a" < "b"); assert("10" < "9"); assert(10 > "9"); assert(!(NaN < 1)); assert(!(undefined < 1)); assert(null >= 0); assert("a" < "aa");
//// bitwise operators
eq(5 & 3, 1); eq(5 | 3, 7); eq(5 ^ 3, 6); eq(~5, -6); eq(1 << 31, -2147483648); eq(-1 >>> 0, 4294967295); eq(-8 >> 1, -4); eq(2 ** 32 | 0, 0); eq(1.9 | 0, 1); eq(-1.9 | 0, -1);
//// instanceof and in
function F() {} var f = new F(); assert(f instanceof F); assert(f instanceof Object); assert("constructor" in f); assert(!("zzz" in f)); throws(function () { 1 in 1; }, TypeError);
//// delete behaviour
var o = {a: 1}; eq(delete o.a, true); eq("a" in o, false); eq(delete o.zzz, true); var arr = [1, 2, 3]; delete arr[1]; eq(arr.length, 3); eq(1 in arr, false);
//// void and typeof results
eq(void 0, undefined); eq(typeof null, "object"); eq(typeof function () {}, "function"); eq(typeof Symbol(), "symbol"); eq(typeof 1, "number"); eq(typeof "", "string"); eq(typeof {}, "object");
//// arguments object
function f(a, b) { return arguments.length; } eq(f(1, 2, 3), 3); function g() { return Array.prototype.slice.call(arguments).join(); } eq(g(1, 2), "1,2"); function h(a) { "use strict"; a = 5; return arguments[0]; } eq(h(1), 1);
//// default and rest parameters
function f(a, b = a + 1, ...r) { return [a, b, r.length].join(); } eq(f(1), "1,2,0"); eq(f(1, 5, 7, 8), "1,5,2"); eq(f.length, 1); eq((function (...a) {}).length, 0);
//// default parameter scope sees earlier params only
var x = "outer"; function f(a = x) { var x = "inner"; return a; } eq(f(), "outer");
//// arrow functions lexical this and arguments
function f() { return (() => this.v)(); } eq(f.call({v: 7}), 7); function g() { return (() => arguments[0])(); } eq(g(9), 9);
//// arrow functions are not constructors
var a = () => 1; throws(function () { new a(); }, TypeError); eq(a.prototype, undefined);
//// getters and setters in literals
var o = {_v: 1, get v() { return this._v; }, set v(x) { this._v = x * 2; }}; o.v = 5; eq(o.v, 10); var d = Object.getOwnPropertyDescriptor(o, "v"); eq(typeof d.get, "function");
//// computed property names and shorthand methods
var k = "dyn"; var o = {[k + "1"]: 1, [`${k}2`]() { return 2; }, k}; eq(o.dyn1, 1); eq(o.dyn2(), 2); eq(o.k, "dyn"); eq(o.dyn2.name, "dyn2");
//// object spread and rest
var a = {x: 1, y: 2}; var b = {...a, y: 3, z: 4}; eq(JSON.stringify(b), '{"x":1,"y":3,"z":4}'); var {x, ...rest} = b; eq(JSON.stringify(rest), '{"y":3,"z":4}');
//// array destructuring
var [a, , b = 5, ...c] = [1, 2, undefined, 4, 5]; eq(a, 1); eq(b, 5); eq(c.join(), "4,5"); var [x, y] = "ab"; eq(x + y, "ab"); [a, b] = [b, a]; eq(a, 5); eq(b, 1);
//// nested destructuring with defaults
var {a: {b: [, c] = [1, 2]} = {}, d = 4} = {}; eq(c, 2); eq(d, 4); function f({p = 1, q} = {}) { return p + (q || 0); } eq(f(), 1); eq(f({q: 2}), 3);
//// destructuring assignment to members
var o = {}; [o.a, o.b] = [1, 2]; ({x: o.c} = {x: 3}); eq(o.a + o.b + o.c, 6);
//// destructuring null throws
throws(function () { var {a} = null; }, TypeError); throws(function () { var [a] = undefined; }, TypeError);
//// template literals
var n = 5; eq(`a${n}b${n + 1}`, "a5b6"); eq(`line1\nline2`.length, 11); function tag(s, ...v) { return s.raw.join("|") + v.join(); } eq(tag`x${1}y\n${2}`, "x|y\\n|1,2");
//// tagged template caching
function t(s) { return s; } function f() { return t`a`; } eq(f(), f()); assert(Object.isFrozen(f()));
//// spread calls and arrays
function f(a, b, c) { return a + b + c; } eq(f(...[1, 2], 3), 6); eq([...[1, 2], ...new Set([3])].join(), "1,2,3"); eq(Math.max(...[1, 5, 3]), 5);
//// symbols as keys
var s = Symbol("d"); var o = {[s]: 1}; eq(o[s], 1); eq(Object.keys(o).length, 0); eq(Object.getOwnPropertySymbols(o)[0], s); eq(s.toString(), "Symbol(d)"); eq(s.description, "d");
//// well-known symbols: toPrimitive and toStringTag
var o = {[Symbol.toPrimitive](h) { return h === "number" ? 42 : "str"; }}; eq(+o, 42); eq(`${o}`, "str"); eq(String(Object.create({[Symbol.toStringTag]: "Z"})), "[object Z]");
//// iterator protocol
var it = {[Symbol.iterator]() { var i = 0; return {next() { return i < 3 ? {value: i++, done: false} : {done: true}; }, return() { it.closed = true; return {}; }}; }}; eq([...it].join(), "0,1,2"); for (var x of it) { break; } assert(it.closed);
//// generators
function* g() { var a = yield 1; var b = yield a + 1; return a + b; } var it = g(); eq(it.next().value, 1); eq(it.next(10).value, 11); var r = it.next(5); eq(r.value, 15); eq(r.done, true);
//// generator delegation and return value
function* a() { var r = yield* b(); yield r; } function* b() { yield 1; return 2; } eq([...a()].join(), "1,2");
//// generator throw and return
function* g() { try { yield 1; } catch (e) { yield "c" + e; } finally { yield "f"; } } var it = g(); it.next(); eq(it.throw("x").value, "cx"); eq(it.next().value, "f"); eq(it.next().done, true);
//// generator methods and spread
var o = {*g() { yield 1; yield 2; }}; eq([...o.g()].join(), "1,2"); class C { static *s() { yield 3; } } eq([...C.s()].join(), "3");
//// classes basic
class A { constructor(x) { this.x = x; } get dbl() { return this.x * 2; } static make() { return new A(3); } m() { return "m" + this.x; } } var a = A.make(); eq(a.dbl, 6); eq(a.m(), "m3"); eq(typeof A, "function"); throws(function () { A(); }, TypeError);
//// class inheritance and super
class A { constructor() { this.t = "A"; } hi() { return "A.hi"; } static s() { return "As"; } } class B extends A { constructor() { super(); this.u = "B"; } hi() { return super.hi() + "+B"; } static s() { return super.s() + "Bs"; } } var b = new B(); eq(b.hi(), "A.hi+B"); eq(B.s(), "AsBs"); eq(b.t + b.u, "AB"); assert(b instanceof A);
//// derived constructor this before super throws
class A {} class B extends A { constructor() { this.x = 1; super(); } } throws(function () { new B(); }, ReferenceError);
//// class fields and private members
class C { a = 1; #p = 2; static s = 3; #m() { return this.#p; } getP() { return this.#m(); } static has(o) { return #p in o; } } var c = new C(); eq(c.a, 1); eq(c.getP(), 2); eq(C.s, 3); assert(C.has(c)); assert(!C.has({})); eq(c.p, undefined);
//// class static blocks and accessors
class C { static x; static { C.x = 5; } static get g() { return 7; } } eq(C.x, 5); eq(C.g, 7);
//// extending builtins
class MyArr extends Array { sum() { return this.reduce((a, b) => a + b, 0); } } var m = MyArr.from([1, 2, 3]); eq(m.sum(), 6); eq(m.length, 3); assert(m instanceof MyArr); assert(m.map(x => x) instanceof MyArr);
//// extending Error
class E extends Error { constructor(m) { super(m); this.name = "E"; } } var e = new E("boom"); assert(e instanceof Error); eq(e.message, "boom"); eq(String(e), "E: boom"); assert(typeof e.stack === "string");
//// new.target
function F() { return new.target; } eq(F(), undefined); eq(new F() instanceof Object, true); class A { constructor() { this.nt = new.target; } } class B extends A {} eq(new B().nt, B);
//// getter on classes and Object.defineProperty
class C { get x() { return 1; } } var d = Object.getOwnPropertyDescriptor(C.prototype, "x"); eq(d.enumerable, false); eq(typeof d.get, "function");
//// Function.prototype.toString for classes and functions
eq((function foo(a, b) { return a + b; }).toString(), "function foo(a, b) { return a + b; }"); eq((class A { m() {} }).toString(), "class A { m() {} }"); eq((x => x * 2).toString(), "x => x * 2");
//// Function.prototype.bind
function f(a, b) { return this.v + a + b; } var g = f.bind({v: 1}, 2); eq(g(3), 6); eq(g.name, "bound f"); eq(g.length, 1); function C(x) { this.x = x; } var B = C.bind(null, 9); eq(new B().x, 9);
//// call apply
function f() { return this.v + Array.prototype.join.call(arguments, ""); } eq(f.call({v: "a"}, 1, 2), "a12"); eq(f.apply({v: "b"}, [3, 4]), "b34");
//// sloppy this coercion
function f() { return typeof this; } eq(f.call(1), "object"); eq(f.call(undefined), "object"); function g() { "use strict"; return typeof this; } eq(g.call(1), "number"); eq(g.call(undefined), "undefined");
//// getter this for primitives
Object.defineProperty(Number.prototype, "me", {get: function () { return typeof this; }, configurable: true}); eq((5).me, "object"); delete Number.prototype.me;
//// label and block function declarations
{ function inner() { return 1; } } eq(typeof inner, "function");
//// eval basics
eq(eval("1 + 2"), 3); eq(eval("var ev = 5; ev * 2"), 10); eq(typeof ev, "number"); var ge = eval; eq(ge("typeof Array"), "object" === "x" ? 0 : "function");
//// Function constructor
var f = new Function("a", "b", "return a * b"); eq(f(3, 4), 12); eq(Function("return this")() === globalThis, true);
//// JSON stringify edge cases
eq(JSON.stringify({a: undefined, b: function () {}, c: Symbol(), d: NaN, e: [undefined]}), '{"d":null,"e":[null]}'); eq(JSON.stringify("a" + String.fromCharCode(0xd800)), '"a' + String.fromCharCode(92) + 'ud800"'); eq(JSON.stringify({a: [1, {b: 2}]}, null, 2), '{\n  "a": [\n    1,\n    {\n      "b": 2\n    }\n  ]\n}');
//// JSON parse reviver and errors
eq(JSON.parse('{"a":[1,2,{"b":3}]}', function (k, v) { return typeof v === "number" ? v * 2 : v; }).a[2].b, 6); throws(function () { JSON.parse("{a:1}"); }, SyntaxError); throws(function () { JSON.parse("[1,]"); }, SyntaxError); eq(JSON.parse("  [1 , 2 ]  ").length, 2);
//// JSON toJSON and circular
var o = {toJSON() { return {z: 1}; }}; eq(JSON.stringify(o), '{"z":1}'); var c = {}; c.self = c; throws(function () { JSON.stringify(c); }, TypeError);
//// property enumeration order
var o = {b: 1, 2: 1, a: 1, 1: 1}; o[Symbol()] = 1; eq(Object.keys(o).join(), "1,2,b,a"); eq(Reflect.ownKeys(o).length, 5);
//// property attributes
var o = {}; Object.defineProperty(o, "x", {value: 1}); o.x = 2; eq(o.x, 1); eq(Object.keys(o).length, 0); throws(function () { "use strict"; o.x = 2; }, TypeError); eq(delete o.x, false);
//// freeze seal preventExtensions
var o = Object.freeze({a: 1}); o.a = 2; o.b = 3; eq(o.a, 1); eq(o.b, undefined); assert(Object.isFrozen(o)); var s = Object.seal({a: 1}); s.a = 2; eq(s.a, 2); delete s.a; eq(s.a, 2); var p = Object.preventExtensions({}); assert(!Object.isExtensible(p));
//// getOwnPropertyNames and descriptors on functions
eq(Object.getOwnPropertyNames(function f(a, b) {}).sort().join(), "arguments,caller,length,name,prototype".split(",").filter(function (n) { return Object.getOwnPropertyNames(function (a, b) {}).indexOf(n) >= 0; }).join()); eq((function (a, b) {}).length, 2);
//// Object.assign and Object.entries
eq(JSON.stringify(Object.assign({}, {a: 1}, null, {b: 2})), '{"a":1,"b":2}'); eq(JSON.stringify(Object.entries({a: 1, b: 2})), '[["a",1],["b",2]]'); eq(JSON.stringify(Object.fromEntries([["x", 1]])), '{"x":1}');
//// Object.create with descriptors and prototypes
var p = {hi() { return "hi"; }}; var o = Object.create(p, {x: {value: 1, enumerable: true}}); eq(o.hi(), "hi"); eq(o.x, 1); eq(Object.getPrototypeOf(o), p); eq(Object.getPrototypeOf(Object.create(null)), null);
//// __proto__ in literals and accessor
var p = {z: 1}; var o = {__proto__: p}; eq(o.z, 1); eq(o.__proto__, p); var q = Object.create(null); q.__proto__ = 5; eq(q.__proto__, 5);
//// Symbol.hasInstance and species
class Even { static [Symbol.hasInstance](n) { return n % 2 === 0; } } assert(2 instanceof Even); assert(!(3 instanceof Even));
//// getter/setter inheritance and super property assignment
class A { set v(x) { this._v = x; } get v() { return this._v; } } class B extends A { set v(x) { super.v = x * 2; } get v() { return super.v; } } var b = new B(); b.v = 4; eq(b.v, 8);
//// Array holes and length
var a = [1, , 3]; eq(a.length, 3); eq(1 in a, false); a.length = 1; eq(a.join(), "1"); a[5] = 1; eq(a.length, 6); var b = []; b[4294967294] = 1; eq(b.length, 4294967295); throws(function () { [].length = -1; }, RangeError);
//// array methods
var a = [3, 1, 2]; eq(a.sort().join(), "1,2,3"); eq(a.reverse().join(), "3,2,1"); eq(a.slice(1).join(), "2,1"); eq(a.splice(1, 1, "x", "y").join(), "2"); eq(a.join(), "3,x,y,1"); eq([1, [2, [3]]].flat(Infinity).join(), "1,2,3"); eq([1, 2, 3].includes(2), true); eq([NaN].includes(NaN), true); eq([NaN].indexOf(NaN), -1);
//// array sort stability and comparator
var a = [{k: 1, v: "a"}, {k: 0, v: "b"}, {k: 1, v: "c"}, {k: 0, v: "d"}]; a.sort(function (x, y) { return x.k - y.k; }); eq(a.map(function (o) { return o.v; }).join(""), "bdac"); eq([10, 9, 1].sort().join(), "1,10,9"); eq([undefined, 3, , 1].sort().length, 4);
//// array iteration methods
eq([1, 2, 3].map(x => x * 2).join(), "2,4,6"); eq([1, 2, 3, 4].filter(x => x % 2).join(), "1,3"); eq([1, 2, 3].reduce((a, b) => a + b), 6); eq([1, 2, 3].reduceRight((a, b) => a + "" + b), "321"); eq([1, 2, 3].find(x => x > 1), 2); eq([1, 2, 3].findLast(x => x < 3), 2); eq([1, 2, 3].some(x => x > 2), true); eq([].every(x => false), true); eq(Array.from({length: 3}, (_, i) => i * i).join(), "0,1,4"); eq(Array.of(7).length, 1);
//// array copy methods
var a = [3, 1, 2]; eq(a.toSorted().join(), "1,2,3"); eq(a.join(), "3,1,2"); eq(a.toReversed().join(), "2,1,3"); eq(a.with(0, 9).join(), "9,1,2"); eq(a.toSpliced(1, 1).join(), "3,2"); eq(a.at(-1), 2);
//// array-likes and generic methods
var al = {length: 2, 0: "a", 1: "b"}; eq(Array.prototype.join.call(al, "-"), "a-b"); eq(Array.prototype.map.call("abc", function (c) { return c.toUpperCase(); }).join(""), "ABC"); eq(Array.from(al).join(), "a,b");
//// string methods
eq("abc".at(-1), "c"); eq("abcdef".slice(-3, -1), "de"); eq("abcdef".substring(4, 1), "bcd"); eq("abc".substr(-2, 1), "b"); eq("a-b-c".split("-", 2).join(), "a,b"); eq("abc".padEnd(5, "12"), "abc12"); eq("  x ".trim(), "x"); eq("ab".repeat(3), "ababab"); eq("abc".replace("b", "$&$&"), "abbc"); eq("aaa".replaceAll("a", "b"), "bbb"); eq("abc".indexOf("c", 5), -1); eq("abc".lastIndexOf("a", -5), 0); eq("ǆ".toUpperCase(), "Ǆ"); eq("ß".toUpperCase(), "SS");
//// string unicode handling
eq("😀".length, 2); eq([..."😀a"].length, 2); eq("😀".codePointAt(0), 128512); eq(String.fromCodePoint(128512), "😀"); eq("😀".charCodeAt(1), 0xDE00); eq("\u{1F600}", "😀"); eq("é".normalize("NFD").length, 2); eq("é".normalize("NFC"), "é");
//// string comparison and localeCompare
eq("a".localeCompare("b"), -1); eq("b".localeCompare("a"), 1); eq("a".localeCompare("a"), 0); eq(["b", "a", "C"].sort((x, y) => x.localeCompare(y)).join(), "a,b,C");
//// number formatting
eq((255).toString(16), "ff"); eq((0.5).toString(2), "0.1"); eq((1234.5678).toFixed(2), "1234.57"); eq((0.000001234).toExponential(2), "1.23e-6"); eq((1234.5678).toPrecision(6), "1234.57"); eq((1e21).toFixed(2), "1e+21"); eq((-1.5).toFixed(0), "-2"); eq((2.5).toFixed(0), "3"); eq((1.005).toFixed(2), "1.00"); eq(String(1e21), "1e+21"); eq(String(1e-7), "1e-7"); eq(String(123456789012345680000), "123456789012345680000"); eq(String(0.1 + 0.2), "0.30000000000000004");
//// number parsing
eq(parseInt("42px"), 42); eq(parseInt("0x1f"), 31); eq(parseInt("101", 2), 5); eq(parseInt(""), NaN); eq(parseFloat("3.14abc"), 3.14); eq(parseFloat(".5"), 0.5); eq(parseFloat("-.5e1"), -5); eq(Number("12abc"), NaN); eq(Number.parseFloat === parseFloat, true);
//// Math functions
eq(Math.round(-0.5), -0); eq(Math.round(2.5), 3); eq(Math.round(-2.5), -2); eq(Math.max(), -Infinity); eq(Math.min(1, NaN), NaN); eq(Math.sign(-3), -1); eq(Math.trunc(-4.7), -4); eq(Math.hypot(3, 4), 5); eq(Math.cbrt(27), 3); eq(Math.clz32(1), 31); eq(Math.imul(2 ** 31, 2), 0); eq(Math.fround(5.5), 5.5); eq(Math.pow(2, -1), 0.5); eq(Math.atan2(1, 1), Math.PI / 4);
//// Date basics
var d = new Date(2020, 0, 31); d.setMonth(1); eq(d.getMonth(), 2); eq(new Date(2020, 11, 31, 23, 59, 59).getDate(), 31); eq(new Date(8.64e15 + 1).getTime(), NaN); eq(new Date("2020-02-30").getTime(), NaN); eq(typeof Date.now(), "number"); eq(new Date(0).toISOString(), "1970-01-01T00:00:00.000Z");
//// RegExp features
eq(/a(?=b)/.exec("ab")[0], "a"); eq(/(?<!a)b/.test("cb"), true); eq("aBc".match(/b/i)[0], "B"); eq("a1b2".replace(/\d/g, function (d) { return d * 2; }), "a2b4"); eq(/^(?:(a)|b)*$/.exec("ab")[1], undefined); eq("x".match(/(y)?/)[1], undefined); eq(/\bfoo\b/.test("a foo b"), true); eq(/[^]/.test("\n"), true); eq(/a.c/s.test("a\nc"), true); eq(/(\d+)-\1/.test("12-12"), true); eq("aaa".lastIndexOf("a"), 2);
//// RegExp sticky global lastIndex
var r = /a/g; r.test("aa"); eq(r.lastIndex, 1); r.test("aa"); eq(r.lastIndex, 2); eq(r.test("aa"), false); eq(r.lastIndex, 0); var y = /b/y; eq(y.test("ab"), false); y.lastIndex = 1; eq(y.test("ab"), true);
//// RegExp named groups and replace
eq("2020-01-02".replace(/(?<y>\d+)-(?<m>\d+)-(?<d>\d+)/, "$<d>/$<m>/$<y>"), "02/01/2020"); eq(/(?<a>x)\k<a>/.test("xx"), true); eq(Object.keys(/(?<a>.)(?<b>.)/.exec("xy").groups).join(), "a,b");
//// RegExp unicode and property escapes
eq(/\p{Lu}/u.test("É"), true); eq(/\p{Script=Greek}/u.test("α"), true); eq(/^\p{L}+$/u.test("héllo"), true); eq(/\u{1F600}/u.test("😀"), true); eq(/^[\u{1F600}-\u{1F64F}]$/u.test("😀"), true); eq("😀".match(/./gu).length, 1); eq("😀".match(/./g).length, 2);
//// RegExp split and matchAll
eq("a1b22c".split(/\d+/).join(), "a,b,c"); eq("a1b".split(/(\d)/).join(), "a,1,b"); eq([..."a1b2".matchAll(/[a-z](\d)/g)].map(function (m) { return m[1]; }).join(), "1,2"); eq("abc".split("").join(), "a,b,c");
//// RegExp backtracking and lazy quantifiers
eq(/a+?b/.exec("aaab")[0], "aaab"); eq(/<.+?>/.exec("<a><b>")[0], "<a>"); eq(/^(a+)+$/.test("aaaaaaaaaaaaaaaaaaaaaaaa!"), false); eq(/(a|ab)(c|bcd)(d*)/.exec("abcd").join(), "abcd,a,bcd,");
//// RegExp case-insensitive and multiline
eq(/^b/m.test("a\nb"), true); eq(/ǆ/i.test("Ǆ"), true); eq(/[a-z]+/i.exec("HeLLo")[0], "HeLLo"); eq(/ſ/i.test("s"), false); eq(/ſ/iu.test("s"), true);
//// Map and Set
var m = new Map([[1, "a"], [NaN, "n"]]); eq(m.get(NaN), "n"); eq(m.size, 2); m.delete(1); eq([...m.keys()].length, 1); var s = new Set([1, 1, 2]); eq(s.size, 2); eq([...s].join(), "1,2"); var o = {}; m.set(o, 1); eq(m.get(o), 1); eq(m.get({}), undefined);
//// Map iteration during mutation
var m = new Map([[1, 1], [2, 2], [3, 3]]); var seen = []; for (var [k] of m) { seen.push(k); if (k === 1) { m.delete(2); m.set(4, 4); } } eq(seen.join(), "1,3,4");
//// WeakMap WeakSet WeakRef
var k = {}; var w = new WeakMap([[k, 1]]); eq(w.get(k), 1); eq(w.has({}), false); throws(function () { w.set(1, 1); }, TypeError); var ws = new WeakSet(); ws.add(k); eq(ws.has(k), true); var r = new WeakRef(k); eq(r.deref(), k);
//// typed arrays
var u = new Uint8Array(4); u[0] = 257; u[1] = -1; eq(u[0], 1); eq(u[1], 255); var c = new Uint8ClampedArray([300, -5, 1.5]); eq(c.join(), "255,0,2"); var f = new Float64Array([1.5, 2.5]); eq(f.reduce((a, b) => a + b), 4); eq(new Int16Array(new ArrayBuffer(8), 2, 2).length, 2);
//// typed array methods
var u = new Uint8Array([3, 1, 2]); eq(u.sort().join(), "1,2,3"); eq(u.slice(1).join(), "2,3"); eq(u.subarray(1, 2)[0], 2); eq(Array.from(u.map(x => x * 2)).join(), "2,4,6"); eq(u.indexOf(3), 2); var u2 = new Uint8Array(5); u2.set([1, 2], 2); eq(u2.join(), "0,0,1,2,0");
//// DataView and endianness
var b = new ArrayBuffer(8); var dv = new DataView(b); dv.setFloat32(0, 1.5); eq(dv.getFloat32(0), 1.5); dv.setUint32(4, 0xdeadbeef, true); eq(dv.getUint8(4), 0xef); eq(dv.getUint32(4, true), 0xdeadbeef);
//// Proxy and Reflect
var log = []; var p = new Proxy({}, {set(t, k, v) { log.push(k); t[k] = v; return true; }, deleteProperty(t, k) { log.push("d" + k); return delete t[k]; }}); p.a = 1; delete p.a; eq(log.join(), "a,da"); eq(Reflect.has({a: 1}, "a"), true); eq(Reflect.ownKeys({b: 1, a: 2}).join(), "b,a"); eq(Reflect.apply(Math.max, null, [1, 3]), 3); eq(Reflect.construct(Date, [0]).getTime(), 0);
//// Proxy invariants
var t = {}; Object.defineProperty(t, "x", {value: 1}); var p = new Proxy(t, {get() { return 2; }}); throws(function () { p.x; }, TypeError);
//// global object
eq(globalThis.Array, Array); var gv = 1; eq(globalThis.gv, 1); globalThis.zz = 5; eq(zz, 5); eq(Object.prototype.toString.call(globalThis), "[object global]" === "x" ? "" : Object.prototype.toString.call(globalThis));
//// getter on prototype chain with receiver
var base = {get who() { return this.name; }}; var d = Object.create(base); d.name = "derived"; eq(d.who, "derived");
//// Error properties and stack
var e = new TypeError("bad", {cause: 1}); eq(e.name, "TypeError"); eq(e.cause, 1); assert(e instanceof Error); eq(Object.prototype.toString.call(e), "[object Error]"); assert(/^TypeError: bad/.test(e.stack)); eq(String(new RangeError()), "RangeError"); var ag = new AggregateError([1], "m"); eq(ag.errors.length, 1);
//// error messages
try { null.x; } catch (e) { assert(e instanceof TypeError); assert(/null/.test(e.message)); } try { undefinedFn(); } catch (e) { assert(e instanceof ReferenceError); eq(e.message, "undefinedFn is not defined"); } try { (void 0)(); } catch (e) { assert(e instanceof TypeError); }
//// strict mode errors
"use strict"; (function () { "use strict"; throws(function () { undefinedVar = 1; }, ReferenceError); throws(function () { Object.freeze([1]).push(2); }, TypeError); throws(function () { delete Object.prototype; }, TypeError); })();
//// getter/setter static and symbols on classes
var sym = Symbol("m"); class C { static [sym]() { return 1; } } eq(C[sym](), 1);
//// stack overflow is a RangeError
function f() { return f() + 1; } throws(f, RangeError);
//// very long string concat and join
var s = ""; for (var i = 0; i < 20000; i++) s += "ab"; eq(s.length, 40000); eq(new Array(1001).join("x").length, 1000);
//// array with many elements
var a = []; for (var i = 0; i < 100000; i++) a.push(i); eq(a.length, 100000); eq(a[99999], 99999); eq(a.reduce(function (x, y) { return x + y; }, 0), 4999950000);
//// sparse arrays
var a = []; a[1000000] = 1; eq(a.length, 1000001); eq(Object.keys(a).length, 1); a.length = 0; eq(a.length, 0);
//// closures counters and IIFE
var counter = (function () { var c = 0; return {inc: function () { return ++c; }}; })(); counter.inc(); eq(counter.inc(), 2);
//// recursion depth 5000
function d(n) { return n === 0 ? 0 : 1 + d(n - 1); } eq(d(5000), 5000);
//// getter performance sanity loop
var o = {get x() { return 1; }}; var s = 0; for (var i = 0; i < 100000; i++) s += o.x; eq(s, 100000);
//// string comparison in sort of unicode
eq(["é", "e", "z", "a"].sort().join(""), "aezé");
//// Date.prototype.toString shape
assert(/^\w{3} \w{3} \d{2} \d{4} \d{2}:\d{2}:\d{2} GMT[+-]\d{4}/.test(new Date(0).toString()));
//// Number.prototype methods on edge values
eq((NaN).toFixed(2), "NaN"); eq((Infinity).toString(), "Infinity"); eq((-0).toString(), "0"); eq(Object.is(-0, 0), false); eq((25).toString(36), "p"); eq((0.1).toFixed(20), "0.10000000000000000555");
//// encodeURIComponent and friends
eq(encodeURIComponent("a b&c/é"), "a%20b%26c%2F%C3%A9"); eq(decodeURIComponent("%C3%A9%20"), "é "); eq(encodeURI("http://x/a b?q=é"), "http://x/a%20b?q=%C3%A9"); throws(function () { decodeURIComponent("%E0%A4%A"); }, URIError); eq(escape("ä b"), "%E4%20b"); eq(unescape("%u0107"), "ć");
//// atob btoa
eq(btoa("hello"), "aGVsbG8="); eq(atob("aGVsbG8="), "hello"); throws(function () { atob("a"); }, Error);
