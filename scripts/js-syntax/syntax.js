//// ok var let const
var a = 1, b; let c = 2; const d = 3;
//// ok function forms
function f(a, b = 2, ...rest) { return a } var g = function () {}; var h = function* () { yield 1 }; async function i() { await 1 } var j = async function () {};
//// ok arrow forms
var a = x => x; var b = (x, y) => x + y; var c = () => ({}); var d = async x => x; var e = async (x, y) => { await x }; var f = ([a, b], {c}) => a; var g = (a = 1, ...r) => a;
//// ok arrow body object literal and nesting
var f = a => b => c => a + b + c;
//// ok class forms
class A extends B { constructor() { super(); } static s() {} get x() { return 1 } set x(v) {} *gen() {} async am() {} static async *sag() {} ['comp' + 1]() {} #p = 1; static #q = 2; #m() {} static { this.z = 1 } field = 1; static sf = 2; }
//// ok class expression and fields without semicolons
var C = class Named { a = 1
 b = 2
 m() {} }
//// ok template literals
var a = `x${1}y${`nested ${2}`}z`; var b = tag`a${1}b`; var c = String.raw`\n${1}`;
//// ok destructuring
var [a, , b = 2, ...c] = [1, 2, 3]; var {x, y: z, w = 1, ...rest} = o; ({a, b} = o); [a, b] = [b, a]; for (var [k, v] of m) ; for (const {id} of list) ;
//// ok nested destructuring and defaults
var {a: {b: [c, {d = 1}]}} = o;
//// ok spread and rest
f(...a, b, ...c); var x = [...a, ...b]; var y = {...o, p: 1};
//// ok optional chaining and nullish
a?.b; a?.[0]; a?.(); a?.b.c?.d(); a ?? b; a ??= b; a ||= b; a &&= b;
//// ok exponent
var x = 2 ** 3 ** 2; x **= 2; var y = (-2) ** 2;
//// ok regex literals
var r = /ab+c/gi; var s = /[/]\//; var t = a / b / c; var u = x
/y/g;
//// ok regex after keyword and operators
return_ = 1; if (/a/.test(s)) ; var x = a ? /b/ : /c/; var y = [/a/, /b/];
//// ok division vs regex
var a = b / c; var d = (1) / 2; var e = x++ / 2; var f = a[0] / 2;
//// ok control flow
if (a) b; else c; for (;;) break; for (var i = 0; i < 10; i++) continue; for (var k in o) ; for (var v of a) ; while (x) ; do ; while (y); switch (a) { case 1: break; default: }
//// ok labels
outer: for (;;) { inner: for (;;) { break outer; continue outer } }
//// ok try catch finally
try { } catch (e) { } finally { } try { } catch { } try { } finally { } try {} catch ({message}) {}
//// ok getters setters in objects
var o = { get a() { return 1 }, set a(v) {}, get: 1, set: 2, async: 3, async f() {}, *g() {}, async *h() {}, [k]: 1, 'str': 2, 3: 4, __proto__: null };
//// ok object shorthand and keywords as keys
var o = { a, b, if: 1, class: 2, new: 3, function: 4, delete: 5, typeof: 6 }; o.if; o.class; o.new;
//// ok contextual keywords as identifiers
var async = 1, of = 2, get = 3, set = 4, static_ = 5, from = 6, as = 7, target = 8; async + of;
//// ok let as identifier in sloppy mode
var let_ = 1; let
x = 1;
//// ok new forms
new A; new A(); new A.B(); new (a.b()); new a.b.c(1)(2); new new A()();
//// ok comma and conditional
a, b, c; x = a ? b : c ? d : e; x = (a, b);
//// ok in operator
var x = 'a' in o; for (var i = ('a' in o) ? 1 : 2; ;) break;
//// ok for in with initialiser sloppy
for (var i = 0 in {}) ;
//// ok typeof delete void
typeof a; delete a.b; void 0; !a; ~a; -a; +a; ++a; --a; a++; a--;
//// ok ASI cases
var a = 1
var b = 2
a
++b
function f() { return
1 }
//// ok unicode identifiers and escapes
var ünï = 1; var abc = 2; var $_ = 3; var 変数 = 4;
//// ok numeric literals
var a = 0x1F, b = 0o17, c = 0b101, d = 1e3, e = .5, f = 5., g = 1_000_000, h = 0.1e-2, i = 017;
//// ok string escapes
var s = 'a\nb\t\x41A\u{1F600}\'"\
continued';
//// ok line terminators
var a = 1;
var b = 2;
//// ok html comments
var x = 1; <!-- comment
--> also comment
//// ok generator and async generators
function* g() { var x = yield; yield* other(); yield 1; } async function* ag() { yield await 1; for await (const x of y) ; }
//// ok async arrow in expression positions
foo(async () => { await 1 }); var o = { m: async () => 1 };
//// ok class getters named get and static
class A { get() {} set() {} static() {} async() {} static static() {} get get() {} }
//// ok super usage
class A extends B { constructor() { super(); super.m(); super['x']; } m() { return super.m() } static s() { return super.s() } }
//// ok object super in methods
var o = { m() { return super.toString() } };
//// ok function in blocks and if
{ function f() {} } if (a) function g() {}
//// ok exports are not supported but sequences of statements fine
a; b; c;
//// ok tagged template member
a.b`x`; a()`x`; (a)`x`;
//// ok parenthesised patterns and assignments
(a) = 1; (a.b) = 2; [(a), (b.c)] = [1, 2];
//// ok class private in
class A { #x; static has(o) { return #x in o } }
//// ok empty statements and blocks
;;; {} {{}} if (a) {} else {}
//// ok yield as identifier in sloppy non-generator
var yield_ = 1; function f() { var yield = 2 }
//// ok await as identifier outside async
var await_ = 1; function f() { var await = 2 }
//// ok arguments and eval names
function f() { arguments; eval('1'); } var eval_ = 1;
//// ok default param referencing earlier param
function f(a, b = a) {} var g = (a, b = a) => b;
//// ok trailing commas
f(a, b,); function g(a, b,) {} var arr = [1, 2,]; var o = {a: 1, b: 2,};
//// ok regexp with slash in class and flags
var r = /[a/b]+/gimsuy;
//// ok new.target in function
function F() { return new.target }
//// ok do while without semicolon followed by statement
do x++; while (x < 5) y = 1
//// ok object with numeric and computed methods
var o = { 1() {}, 'a b'() {}, [`t`]() {} };
//// ok bad: unterminated string
//// bad unterminated string
var a = 'abc
//// bad missing brace
function f() { 
//// bad invalid assignment target
1 = 2;
//// bad invalid update target
1++;
//// bad const without init
const a;
//// bad let redeclare syntax via destructuring missing init
let [a];
//// bad rest not last in params
function f(...a, b) {}
//// bad rest with default
function f(...a = []) {}
//// bad duplicate param strict
"use strict"; function f(a, a) {}
//// bad strict with
"use strict"; with (a) {}
//// bad strict octal
"use strict"; var a = 017;
//// bad strict eval assign
"use strict"; eval = 1;
//// bad strict delete ident
"use strict"; delete a;
//// bad reserved word as binding
var class = 1;
//// bad enum
var enum = 1;
//// bad return outside function
return 1;
//// bad break outside loop
break;
//// bad continue outside loop
continue;
//// bad undefined label
a: while (1) { break b; }
//// bad continue to non loop label
a: { while (1) continue a; }
//// bad duplicate label
a: a: ;
//// bad new target outside function
new.target;
//// bad super outside method
function f() { super.x }
//// bad super call outside constructor
class A { m() { super() } }
//// bad class duplicate constructor
class A { constructor() {} constructor() {} }
//// bad getter with param
var o = { get a(x) {} };
//// bad setter without param
var o = { set a() {} };
//// bad cover initialiser
var o = { a = 1 };
//// bad arrow after newline
var f = a
=> 1;
//// bad await in non async
function f() { await 1 }
//// bad yield outside generator strict
"use strict"; var yield = 1;
//// bad optional chain assignment
a?.b = 1;
//// bad optional chain template
a?.b`x`;
//// bad mix nullish and or
a ?? b || c;
//// bad unary before exponent
-a ** 2;
//// bad regex unterminated
var r = /abc;
//// bad for of let async
for (async of x) ;
//// bad template unterminated
var s = `abc ${1
//// bad number followed by ident
var a = 3in [];
//// bad invalid destructuring target
[a + 1] = [1];
//// bad rest not last in array pattern
var [...a, b] = c;
//// bad private undeclared
class A { m() { this.#x } }
//// bad private constructor
class A { #constructor; }
//// bad static prototype
class A { static prototype() {} }
//// bad lexical declaration in single statement context
if (a) let x = 1;
//// bad class in single statement context
while (a) class C {}
//// bad invalid escape in identifier
var 0a = 1;
//// bad strict reserved word
"use strict"; var static = 1;
//// bad object literal duplicate-free shorthand reserved
var o = { if };
//// ok async newline then arrow is two statements (ASI)
var f = async
x => x;
//// bad continue label with block
x: { continue x; }
