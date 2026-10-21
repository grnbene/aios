//// prototype method
function A(n){this.n=n} A.prototype.get=function(){return this.n}; var a=new A(7); console.log(a.get()===7 && a instanceof A ? "ok" : "no")
//// prototype chain two levels
function A(){} A.prototype.hi=function(){return "A"}; function B(){} B.prototype=Object.create(A.prototype); B.prototype.constructor=B; var b=new B(); console.log(b.hi()==="A" && b instanceof A && b instanceof B ? "ok" : "no")
//// constructor property
function A(){} var a=new A(); console.log(a.constructor===A && A.prototype.constructor===A ? "ok" : "no")
//// own property shadows prototype
function A(){} A.prototype.x=1; var a=new A(); a.x=2; console.log(a.x===2 && A.prototype.x===1 && new A().x===1 ? "ok" : "no")
//// hasOwnProperty
var o={a:1}; function A(){} A.prototype.b=2; var a=new A(); a.c=3; console.log(o.hasOwnProperty("a") && !o.hasOwnProperty("b") && !a.hasOwnProperty("b") && a.hasOwnProperty("c") ? "ok" : "no")
//// Object.create
var p={a:1}; var o=Object.create(p); console.log(o.a===1 && Object.getPrototypeOf(o)===p ? "ok" : "no")
//// Object.create with null
var o=Object.create(null); o.x=1; console.log(o.x===1 && Object.getPrototypeOf(o)===null ? "ok" : "no")
//// Object.create with descriptors
var o=Object.create({}, {x:{value:5,enumerable:true}}); console.log(o.x===5 ? "ok" : "no")
//// isPrototypeOf
var p={}; var o=Object.create(p); console.log(p.isPrototypeOf(o) && !o.isPrototypeOf(p) ? "ok" : "no")
//// for-in sees inherited enumerable properties
function A(){this.own=1} A.prototype.inh=2; var k=[]; for(var x in new A()) k.push(x); console.log(k.join()==="own,inh" ? "ok" : "no")
//// Object.keys is own only
function A(){this.own=1} A.prototype.inh=2; console.log(Object.keys(new A()).join()==="own" ? "ok" : "no")
//// defineProperty value
var o={}; Object.defineProperty(o,"x",{value:3}); console.log(o.x===3 ? "ok" : "no")
//// defineProperty is not enumerable by default
var o={}; Object.defineProperty(o,"x",{value:3}); console.log(Object.keys(o).length===0 && o.x===3 ? "ok" : "no")
//// defineProperty getter
var o={}; Object.defineProperty(o,"x",{get:function(){return 4}, enumerable:true}); console.log(o.x===4 && Object.keys(o).join()==="x" ? "ok" : "no")
//// defineProperty setter
var o={}, v=0; Object.defineProperty(o,"x",{get:function(){return v}, set:function(n){v=n*2}}); o.x=5; console.log(o.x===10 ? "ok" : "no")
//// defineProperty writable false
var o={}; Object.defineProperty(o,"x",{value:1,writable:false}); o.x=2; console.log(o.x===1 ? "ok" : "no")
//// getter in object literal
var o={get x(){return 9}}; console.log(o.x===9 ? "ok" : "no")
//// setter in object literal
var s=0, o={get x(){return s}, set x(v){s=v+1}}; o.x=4; console.log(o.x===5 ? "ok" : "no")
//// getter on prototype
function A(){} Object.defineProperty(A.prototype,"g",{get:function(){return this.v*2}}); var a=new A(); a.v=4; console.log(a.g===8 ? "ok" : "no")
//// getOwnPropertyDescriptor
var o={a:1}; var d=Object.getOwnPropertyDescriptor(o,"a"); console.log(d.value===1 && d.writable===true && d.enumerable===true && d.configurable===true ? "ok" : "no")
//// getOwnPropertyNames includes non-enumerable
var o={a:1}; Object.defineProperty(o,"b",{value:2}); console.log(Object.getOwnPropertyNames(o).join()==="a,b" ? "ok" : "no")
//// Object.freeze
var o={a:1}; Object.freeze(o); o.a=2; o.b=3; console.log(o.a===1 && o.b===undefined && Object.isFrozen(o) ? "ok" : "no")
//// Object.seal and preventExtensions
var o={a:1}; Object.seal(o); o.a=5; o.b=1; delete o.a; console.log(o.a===5 && o.b===undefined && Object.isSealed(o) ? "ok" : "no")
//// toString tags
var ts=Object.prototype.toString; console.log(ts.call([])==="[object Array]" && ts.call({})==="[object Object]" && ts.call(null)==="[object Null]" && ts.call(undefined)==="[object Undefined]" && ts.call(function(){})==="[object Function]" && ts.call("")==="[object String]" && ts.call(1)==="[object Number]" && ts.call(/x/)==="[object RegExp]" ? "ok" : "no")
//// valueOf and toString in conversions
var o={valueOf:function(){return 41}}; var p={toString:function(){return "s"}}; console.log(o+1===42 && p+""==="s" && `${p}`==="s" ? "ok" : "no")
//// in operator with prototype
function A(){} A.prototype.x=1; console.log(("x" in new A()) && !("y" in new A()) ? "ok" : "no")
//// delete own property
var o={a:1}; console.log(delete o.a && !("a" in o) ? "ok" : "no")
//// instanceof with non-callable throws
try{ ({}) instanceof {}; console.log("no") }catch(e){ console.log(e instanceof TypeError ? "ok" : "no") }
//// new returns object from constructor
function A(){ return {z:1} } console.log(new A().z===1 ? "ok" : "no")
//// new ignores primitive return
function A(){ this.q=1; return 5 } console.log(new A().q===1 ? "ok" : "no")
//// this is the new object in a constructor
function A(){ this.me=this } var a=new A(); console.log(a.me===a ? "ok" : "no")
//// __proto__
var p={a:1}; var o={__proto__:p}; console.log(o.a===1 && o.__proto__===p ? "ok" : "no")
//// Object.setPrototypeOf
var p={a:1}; var o={}; Object.setPrototypeOf(o,p); console.log(o.a===1 ? "ok" : "no")
//// propertyIsEnumerable
var o={a:1}; Object.defineProperty(o,"b",{value:2}); console.log(o.propertyIsEnumerable("a") && !o.propertyIsEnumerable("b") ? "ok" : "no")
