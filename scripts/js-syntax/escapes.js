//// ok identifier with unicode escape
var \u0061bc = 2; abc;
//// ok identifier with braces escape
var \u{61}bc = 2;
//// ok string escapes
var s = 'a\n\u0041\u{1F600}\x41';
//// ok string line continuation
var s = 'a\
b';
//// ok template escapes
var s = `a\u0041\x41\${1}`;
//// ok tagged template invalid escape is fine
tag`\u00zz`;
//// bad untagged template invalid escape
var s = `\u00zz`;
//// bad regex flags escape
var r = /a/\u0067;
//// bad bad unicode escape in identifier
var \u00zz = 1;
//// bad escaped keyword
va\u0072 a = 1;
//// ok escaped identifier named like a contextual keyword
var l\u0065t_ = 1;
//// bad strict octal escape
'use strict'; var s = '\07';
//// ok sloppy octal escape
var s = '\07';
//// ok regex with escapes
var r = /\d+\/[\]]/u;
//// ok string containing line separator
var s = 'a b';
//// bad string containing newline
var s = 'a
b';
//// ok identifier with surrogate pair
var 𝓐 = 1;
//// ok legacy octal in sloppy
var a = 017, b = 08, c = 09.5;
//// bad numeric separator at end
var a = 1_;
//// bad double numeric separator
var a = 1__0;
//// bad numeric separator after leading zero hex
var a = 0x_1;
//// ok numeric separators
var a = 1_000.000_1e1_0, b = 0xFF_FF;
