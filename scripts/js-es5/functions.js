//// call
function f(a,b){return this.x+a+b} var o={x:1}; console.log(f.call(o,2,3)===6 ? "ok" : "no")
//// apply
function f(a,b){return this.x+a+b} var o={x:1}; console.log(f.apply(o,[2,3])===6 ? "ok" : "no")
//// apply with arguments object
function g(){return h.apply(null, arguments)} function h(a,b){return a*b} console.log(g(6,7)===42 ? "ok" : "no")
//// bind
function f(a,b){return this.x+a+b} var g=f.bind({x:5},1); console.log(g(2)===8 ? "ok" : "no")
//// bind keeps new working
function P(a){this.a=a} var B=P.bind(null,9); var p=new B(); console.log(p.a===9 && p instanceof P ? "ok" : "no")
//// iife call with this
(function(){ console.log(this.q===1 ? "ok" : "no") }).call({q:1})
//// function name and length
function foo(a,b){} console.log(foo.length===2 && foo.name==="foo" ? "ok" : "no")
//// Function global and instanceof Function
console.log((function(){}) instanceof Function && typeof Function==="function" ? "ok" : "no")
//// Function.prototype.call exists
console.log(typeof Function.prototype.call==="function" && typeof Function.prototype.apply==="function" && typeof Function.prototype.bind==="function" ? "ok" : "no")
//// arguments object
function f(){return arguments.length+arguments[0]} console.log(f(5,6)===7 ? "ok" : "no")
//// arguments.callee free: slice of arguments
function f(){return Array.prototype.slice.call(arguments,1).join("-")} console.log(f(1,2,3)==="2-3" ? "ok" : "no")
//// this in plain call is the global object
var g=(function(){return this})(); console.log(g===this && typeof g==="object" ? "ok" : "no")
//// closure in loop
var fs=[]; for(var i=0;i<3;i++){(function(k){fs.push(function(){return k})})(i)} console.log(fs[2]()===2 ? "ok" : "no")
//// recursion and named function expression
var fact=function me(n){return n<2?1:n*me(n-1)}; console.log(fact(5)===120 ? "ok" : "no")
//// hoisting
console.log(typeof hoisted==="function" && later===undefined ? "ok" : "no"); function hoisted(){} var later=1;
//// toString of a function mentions function
console.log(String(function foo(){}).indexOf("function")===0 ? "ok" : "no")
//// an anonymous function assigned to a var may reassign that var (name inference gives no inner binding)
var H, I = function(){ I = function(){}; H = 5 }; I(); var k = function(){ k = 2; return k }; console.log(H===5 && k()===2 && k===2 ? "ok" : "no")
//// the inferred name is still the function's name
var named = function(){}; var o = { m: function(){} }; console.log(named.name==="named" && o.m.name==="m" ? "ok" : "no")
