//// Array.prototype.slice.call on array-like
console.log(Array.prototype.slice.call({0:1,1:2,length:2}).length===2 ? "ok" : "no")
//// Array.prototype.forEach.call on arguments
var s=0; (function(){ Array.prototype.forEach.call(arguments,function(x){s+=x}) })(1,2,3); console.log(s===6 ? "ok" : "no")
//// Array.prototype.indexOf.call on a string
console.log(Array.prototype.indexOf.call("abc","b")===1 ? "ok" : "no")
//// extend Array.prototype
Array.prototype.sum=function(){var t=0;for(var i=0;i<this.length;i++)t+=this[i];return t}; console.log([1,2,3].sum()===6 ? "ok" : "no")
//// extend String.prototype
String.prototype.twice=function(){return this+this}; console.log("ab".twice()==="abab" ? "ok" : "no")
//// extend Number and Function prototype
Number.prototype.dbl=function(){return this*2}; Function.prototype.hi=function(){return "hi"}; console.log((4).dbl()===8 && (function(){}).hi()==="hi" ? "ok" : "no")
//// Object.prototype extension is seen by plain objects
Object.prototype.zz=function(){return 1}; var o={}; var r=o.zz()===1; delete Object.prototype.zz; console.log(r ? "ok" : "no")
//// Array.isArray and instanceof Array
console.log(Array.isArray([]) && !Array.isArray({}) && [] instanceof Array && !({} instanceof Array) ? "ok" : "no")
//// array methods
console.log([3,1,2].sort().join()==="1,2,3" && [1,2,3].reduceRight(function(a,b){return a+b})===6 && [1,2,3].lastIndexOf(3)===2 && [1,2,3].some(function(x){return x>2}) && [1,2,3].every(function(x){return x>0}) ? "ok" : "no")
//// array length semantics
var a=[1,2,3]; a.length=1; var b=[]; b[4]=1; console.log(a.length===1 && b.length===5 && a[1]===undefined ? "ok" : "no")
//// string methods
console.log("abc".substr(1,1)==="b" && "a-b".split("-").length===2 && " x ".trim()==="x" && "abc".charAt(1)==="b" && "abc"[2]==="c" && "x".concat("y")==="xy" ? "ok" : "no")
//// String and Number conversions
console.log(String(12)==="12" && Number("0x10")===16 && parseInt("0x1f")===31 && parseInt("12px")===12 && parseFloat("3.5e2")===350 && (255).toString(16)==="ff" && (0.5).toFixed(1)==="0.5" ? "ok" : "no")
//// Number constants
console.log(Number.MAX_VALUE>1e300 && Number.MIN_VALUE>0 && isNaN(Number.NaN) && Number.POSITIVE_INFINITY===Infinity && isFinite(1) && !isFinite(Infinity) ? "ok" : "no")
//// Math
console.log(Math.max(1,3)===3 && Math.floor(-1.5)===-2 && Math.round(2.5)===3 && Math.abs(-1)===1 && Math.pow(2,10)===1024 && Math.imul(3,4)===12 ? "ok" : "no")
//// JSON
console.log(JSON.stringify({a:[1,2],b:"x"})==='{"a":[1,2],"b":"x"}' && JSON.parse('{"a":[1,{"b":2}]}').a[1].b===2 ? "ok" : "no")
//// JSON.stringify with replacer and indent
console.log(JSON.stringify({a:1},null,2)==='{\n  "a": 1\n}' ? "ok" : "no")
//// Date
var d=new Date(0); console.log(d.getUTCFullYear()===1970 && new Date(2020,0,15).getMonth()===0 && typeof Date.now()==="number" && new Date("2020-01-02T03:04:05Z").getTime()===1577934245000 ? "ok" : "no")
//// RegExp
console.log(new RegExp("a+","g").test("caat") && /(\d+)-(\d+)/.exec("10-20")[2]==="20" && "aXbX".replace(/X/g,"-")==="a-b-" && "abc".match(/b/).index===1 ? "ok" : "no")
//// RegExp sticky-free: lastIndex and global exec loop
var re=/a/g, n=0; while(re.exec("aaa")) n++; console.log(n===3 ? "ok" : "no")
//// label continue
o:for(var i=0;i<3;i++){for(var j=0;j<3;j++){if(j==1)continue o}} console.log(i===3 ? "ok" : "no")
//// label break
var n=0; a:for(var i=0;i<3;i++){for(var j=0;j<3;j++){n++; if(j==1)break a}} console.log(n===2 ? "ok" : "no")
//// switch with strings and fallthrough
var x="b",r=""; switch(x){case "a":r+="a";case "b":r+="b";case "c":r+="c";break;default:r+="d"} console.log(r==="bc" ? "ok" : "no")
//// typeof
console.log(typeof undeclared==="undefined" && typeof null==="object" && typeof function(){}==="function" && typeof 1==="number" && typeof ""==="string" && typeof {}==="object" && typeof true==="boolean" ? "ok" : "no")
//// comma, void, in, delete
var a=(1,2); console.log(a===2 && void 0===undefined && ("x" in {x:1}) ? "ok" : "no")
//// getter/setter on classes-free ES5 patterns: Object.defineProperties
var o={}; Object.defineProperties(o,{a:{value:1,enumerable:true},b:{get:function(){return 2}}}); console.log(o.a===1 && o.b===2 ? "ok" : "no")
//// Object.entries free: Object.keys order with integer keys
var o={b:1,a:2,2:"x",1:"y"}; console.log(Object.keys(o).join()==="1,2,b,a" ? "ok" : "no")
//// Error types and instanceof
try{ null.x }catch(e){ console.log(e instanceof TypeError && e instanceof Error && typeof e.message==="string" && e.name==="TypeError" ? "ok" : "no") }
//// ReferenceError is catchable
try{ nope }catch(e){ console.log(e instanceof ReferenceError && e.name==="ReferenceError" ? "ok" : "no") }
//// custom error subclass
function E(m){this.message=m} E.prototype=new Error(); E.prototype.name="E"; try{throw new E("x")}catch(e){ console.log(e.message==="x" && e instanceof Error && e instanceof E && String(e)==="E: x" ? "ok" : "no") }
//// Error toString and stack presence
var e=new Error("boom"); console.log(String(e)==="Error: boom" && typeof e.stack==="string" ? "ok" : "no")
//// finally runs and rethrow
var log=[]; try{ try{ throw 1 }finally{ log.push("f") } }catch(x){ log.push("c"+x) } console.log(log.join()==="f,c1" ? "ok" : "no")
//// uncaught-looking error inside function is catchable by caller
function bad(){ undefinedFunction() } try{ bad() }catch(e){ console.log(e instanceof ReferenceError ? "ok" : "no") }
//// Symbol exists
console.log(typeof Symbol==="function" && typeof Symbol.iterator!=="undefined" && Symbol("a")!==Symbol("a") ? "ok" : "no")
//// typed arrays
var u=new Uint8Array(4); u[0]=257; u[1]=-1; console.log(u.length===4 && u[0]===1 && u[1]===255 ? "ok" : "no")
//// ArrayBuffer and views share memory
var b=new ArrayBuffer(8); var a=new Uint8Array(b); var c=new Uint32Array(b); c[0]=0x01020304; console.log(a[0]===4 && a[3]===1 ? "ok" : "no")
//// TextEncoder
console.log(typeof TextEncoder==="function" && new TextEncoder().encode("aé").length===3 ? "ok" : "no")
//// Object.getPrototypeOf on literals and arrays
console.log(Object.getPrototypeOf({})===Object.prototype && Object.getPrototypeOf([])===Array.prototype && Object.getPrototypeOf(function(){})===Function.prototype ? "ok" : "no")
//// Function constructor
var add=new Function("a","b","return a+b"); console.log(add(2,3)===5 ? "ok" : "no")
//// globalThis and window
console.log(typeof globalThis==="object" && globalThis===this && typeof window==="object" ? "ok" : "no")
//// var at top level is a property of the global object
var topLevelVar=3; console.log(this.topLevelVar===3 ? "ok" : "no")
//// assigning an undeclared name makes a global (sloppy mode)
(function(){ implicitGlobal=1 })(); console.log(implicitGlobal===1 && this.implicitGlobal===1 ? "ok" : "no")
//// strict-mode directive does not break anything
(function(){ "use strict"; var x=1; console.log(x===1 ? "ok" : "no") })()
//// getter named like a keyword property and reserved words as property names
var o={class:1,default:2,new:3}; console.log(o.class+o.default+o.new===6 ? "ok" : "no")
//// date parsing and toISOString
console.log(new Date(Date.UTC(2020,0,1)).toISOString()==="2020-01-01T00:00:00.000Z" ? "ok" : "no")
