/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Host-side tests for the script engine: the language (operators, scoping, closures, functions, objects, prototypes, exceptions, loops), the built-ins,
// the limits, and the collector.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Script/Engine.hpp"

using namespace script;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

struct TestHost final : IHost {
  std::vector<std::string> console_lines, alerts, writes;
  std::string page_title = "T";
  void console(int level, StringView text) override { console_lines.push_back(std::to_string(level) + ":" + std::string(text.data(), text.size())); }
  void alert(StringView text) override { alerts.push_back(std::string(text.data(), text.size())); }
  void document_write(StringView text, bool newline) override { writes.push_back(std::string(text.data(), text.size()) + (newline ? "\n" : "")); }
  uint64_t now_ms() override { return 1700000000000ull; }
  StringView location() override { return "http://example.com/page"; }
  StringView title() override { return StringView(page_title.data(), page_title.size()); }
  void set_title(StringView t) override { page_title.assign(t.data(), t.size()); }
  StringView user_agent() override { return "Test/1"; }
};

// Runs a script in a fresh engine; "ERR: ..." if it failed.
static std::string eval(const char* source) {
  TestAllocator a;
  std::string out;
  {
    TestHost host;
    Engine e(a, &host);
    String error(a), result(a);
    if (e.run(source, &error, &result)) out = std::string(result.c_str());
    else out = std::string("ERR: ") + error.c_str();
  }
  if (a.live != 0) {
    printf("   leak (%d cells) after: %s\n", a.live, source);
    ++g_failed;
  }
  return out;
}

static bool is(const char* source, const char* expected) {
  const std::string got = eval(source);
  if (got != expected) {
    printf("   %s\n   gives   %s\n   expects %s\n", source, got.c_str(), expected);
    return false;
  }
  return true;
}

static void test_expressions() {
  CHECK(is("1 + 2 * 3", "7"));
  CHECK(is("(1 + 2) * 3 - 4 / 2", "7"));
  CHECK(is("2 ** 10", "1024"));
  CHECK(is("2 ** 3 ** 2", "512"));
  CHECK(is("7 % 3", "1"));
  CHECK(is("-7 % 3", "-1"));
  CHECK(is("5.5 % 2", "1.5"));
  CHECK(is("0.1 + 0.2", "0.30000000000000004"));
  CHECK(is("1 / 3", "0.3333333333333333"));
  CHECK(is("1 / 0", "Infinity"));
  CHECK(is("-1 / 0", "-Infinity"));
  CHECK(is("0 / 0", "NaN"));
  CHECK(is("1e21", "1e+21"));
  CHECK(is("1e-7", "1e-7"));
  CHECK(is("123456789 * 1000", "123456789000"));
  CHECK(is("0x1F + 0b101 + 0o17", "51"));
  CHECK(is("5 & 3, 5 | 3", "7"));
  CHECK(is("5 ^ 3", "6"));
  CHECK(is("~5", "-6"));
  CHECK(is("1 << 31", "-2147483648"));
  CHECK(is("-1 >>> 0", "4294967295"));
  CHECK(is("-8 >> 1", "-4"));
  CHECK(is("'a' + 1 + 2", "a12"));
  CHECK(is("1 + 2 + 'a'", "3a"));
  CHECK(is("'3' * '4'", "12"));
  CHECK(is("'5' - 2", "3"));
  CHECK(is("+'  42  '", "42"));
  CHECK(is("+'abc'", "NaN"));
  CHECK(is("1 < 2 && 2 < 3", "true"));
  CHECK(is("'b' > 'a'", "true"));
  CHECK(is("'10' < '9'", "true"));
  CHECK(is("10 < '9'", "false"));
  CHECK(is("null == undefined", "true"));
  CHECK(is("null === undefined", "false"));
  CHECK(is("0 == ''", "true"));
  CHECK(is("'1' == 1", "true"));
  CHECK(is("NaN == NaN", "false"));
  CHECK(is("[1] == 1", "true"));
  CHECK(is("0 || 'x'", "x"));
  CHECK(is("0 ?? 'x'", "0"));
  CHECK(is("null ?? 'x'", "x"));
  CHECK(is("1 && 2", "2"));
  CHECK(is("!''", "true"));
  CHECK(is("!!'0'", "true"));
  CHECK(is("true ? 'y' : 'n'", "y"));
  CHECK(is("typeof 1 + typeof 'a' + typeof {} + typeof null + typeof undefined + typeof (() => 1)", "numberstringobjectobjectundefinedfunction"));
  CHECK(is("typeof notDefined", "undefined"));
  CHECK(is("void 0", "undefined"));
  CHECK(is("var a = 1; a++ + ++a", "4"));
  CHECK(is("var a = 5; a += 2; a *= 3; a -= 1; a /= 4; a", "5"));
  CHECK(is("var s = 'x'; s += 'y'; s", "xy"));
  CHECK(is("`a${1 + 1}b${'c'}`", "a2bc"));
  CHECK(is("`line1\\nline2`.length", "11"));
  CHECK(is("'it\\'s'", "it's"));
  CHECK(is("'\\x41\\u0042'", "AB"));
  CHECK(is("1, 2, 3", "3"));
  CHECK(is("'abc'.length + [1, 2].length", "5"));
  CHECK(is("'abc'[1]", "b"));
  CHECK(is("var i = 0; i++; i++; i", "2"));
}

static void test_variables_and_functions() {
  CHECK(is("var x = 1; { var x = 2; } x", "2"));
  CHECK(is("let x = 1; { let x = 2; } x", "1"));
  CHECK(is("const c = 5; c * 2", "10"));
  CHECK(is("function f(a, b) { return a + b; } f(2, 3)", "5"));
  CHECK(is("f(2); function f(a) { return a * 2; }", "4"));   // (hoisted)
  CHECK(is("var f = function(n) { return n <= 1 ? 1 : n * f(n - 1); }; f(10)", "3628800"));
  CHECK(is("var f = function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }; f(5)", "120"));
  CHECK(is("function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } fib(20)", "6765"));
  CHECK(is("function f() { return arguments.length; } f(1, 2, 3)", "3"));
  CHECK(is("function f(a, b = 10) { return a + b; } f(1) + f(1, 2)", "14"));
  CHECK(is("function f(a, ...rest) { return rest.length * 10 + a; } f(1, 2, 3, 4)", "31"));
  CHECK(is("Math.max(...[1, 5, 3])", "5"));
  CHECK(is("[...[1, 2], 3, ...'ab'].length", "5"));
  CHECK(is("var add = (a, b) => a + b; add(2, 3)", "5"));
  CHECK(is("var sq = x => x * x; sq(7)", "49"));
  CHECK(is("var f = () => { return 3; }; f()", "3"));
  CHECK(is("var o = { v: 5, f() { return this.v; } }; o.f()", "5"));
  CHECK(is("var o = { v: 5, f() { return (() => this.v)(); } }; o.f()", "5"));
  CHECK(is("function C() { this.v = 7; } var c = new C(); c.v", "7"));
  CHECK(is("function mk() { var n = 0; return function() { return ++n; }; } var c = mk(); c(); c(); c()", "3"));
  CHECK(is("var fs = []; for (var i = 0; i < 3; i++) { fs.push(function() { return i; }); } fs[0]() + fs[2]()", "6"));
  CHECK(is("function outer() { var a = 1; function inner() { return a + 1; } return inner(); } outer()", "2"));
  CHECK(is("(function() { return typeof this; })()", "object"));
  CHECK(is("function f() { return this; } f.call(5) === 5", "true"));   // (`this` is not boxed)
  CHECK(is("function f(a, b) { return this.x + a + b; } f.call({ x: 1 }, 2, 3)", "6"));
  CHECK(is("function f(a, b) { return this.x + a + b; } f.apply({ x: 1 }, [2, 3])", "6"));
  CHECK(is("function f(a, b) { return this.x + a + b; } var g = f.bind({ x: 1 }, 2); g(3)", "6"));
  CHECK(is("(function(a) { a = 5; return arguments[0]; })(1)", "1"));
  CHECK(is("var r = (function() { try { return 1; } finally { } })(); r", "1"));
  CHECK(is("typeof function() {}", "function"));
  CHECK(is("(function f(){}).name", "f"));
  CHECK(is("(function(a,b){}).length", "2"));
  CHECK(is("var a = 1; function f() { return a; } a = 2; f()", "2"));
  CHECK(is("x = 5; x", "5"));
  CHECK(is("undefinedVar", "ERR: ReferenceError: undefinedVar is not defined"));
}

static void test_control_flow() {
  CHECK(is("var s = 0; for (var i = 1; i <= 10; i++) s += i; s", "55"));
  CHECK(is("var s = 0; for (let i = 0; i < 5; i++) { if (i == 3) continue; s += i; } s", "7"));
  CHECK(is("var s = 0; for (var i = 0; ; i++) { if (i > 4) break; s += i; } s", "10"));
  CHECK(is("var i = 0, s = 0; while (i < 5) { s += i; i++; } s", "10"));
  CHECK(is("var i = 0; do { i++; } while (i < 3); i", "3"));
  CHECK(is("var s = ''; for (var k in { a: 1, b: 2 }) s += k; s", "ab"));
  CHECK(is("var s = ''; for (var k in [5, 6]) s += k; s", "01"));
  CHECK(is("var s = 0; for (var v of [1, 2, 3]) s += v; s", "6"));
  CHECK(is("var s = ''; for (const ch of 'abc') s += ch + '.'; s", "a.b.c."));
  CHECK(is("var s = 0; outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { if (j == 1) continue outer; if (i == 2) break outer; s++; } } s", "2"));
  CHECK(is("function f(x) { switch (x) { case 1: return 'one'; case 2: return 'two'; default: return 'many'; } } f(1) + f(2) + f(3)", "onetwomany"));
  CHECK(is("var r = ''; switch (2) { case 1: r += 'a'; case 2: r += 'b'; case 3: r += 'c'; break; case 4: r += 'd'; } r", "bc"));
  CHECK(is("var r = ''; switch (9) { case 1: r += 'a'; break; default: r += 'z'; case 2: r += 'b'; } r", "zb"));
  CHECK(is("if (0) 1; else if (1) 2; else 3", "2"));
  CHECK(is("var a = []; for (var i = 0; i < 3; i++) for (var j = 0; j < 2; j++) a.push(i * j); a.join()", "0,0,0,1,0,2"));
  CHECK(is("var n = 0; for (var i = 0; i < 100000; i++) n += i % 3; n", "99999"));
  CHECK(is("lbl: { var x = 1; break lbl; x = 2; } x", "1"));
}

static void test_objects() {
  CHECK(is("var o = { a: 1, b: { c: 2 } }; o.b.c + o['a']", "3"));
  CHECK(is("var o = {}; o.x = 1; o['y'] = 2; o.x + o.y", "3"));
  CHECK(is("var k = 'dyn'; var o = { [k]: 5 }; o.dyn", "5"));
  CHECK(is("var a = 1, b = 2; var o = { a, b }; o.a + o.b", "3"));
  CHECK(is("var o = { a: 1 }; delete o.a; 'a' in o", "false"));
  CHECK(is("var o = { a: 1 }; 'a' in o", "true"));
  CHECK(is("var o = { a: 1, b: 2 }; Object.keys(o).join()", "a,b"));
  CHECK(is("var o = { a: 1, b: 2 }; Object.values(o).join()", "1,2"));
  CHECK(is("var o = { a: 1 }; Object.entries(o)[0].join()", "a,1"));
  CHECK(is("var o = Object.assign({}, { a: 1 }, { b: 2 }); o.a + o.b", "3"));
  CHECK(is("var o = { ...{ a: 1 }, b: 2 }; o.a + o.b", "3"));
  CHECK(is("var o = { a: 1 }; o.hasOwnProperty('a') && !o.hasOwnProperty('b')", "true"));
  CHECK(is("var p = { greet() { return 'hi ' + this.n; } }; var o = Object.create(p); o.n = 'bob'; o.greet()", "hi bob"));
  CHECK(is("function A() {} A.prototype.hello = function() { return 'hello'; }; new A().hello()", "hello"));
  CHECK(is("function A() {} var a = new A(); a instanceof A", "true"));
  CHECK(is("function A() {} function B() {} new A() instanceof B", "false"));
  CHECK(is("function A() {} new A().constructor === A", "true"));
  CHECK(is("function Animal(n) { this.n = n; } Animal.prototype.speak = function() { return this.n + ' makes a sound'; }; function Dog(n) { Animal.call(this, n); } Dog.prototype = Object.create(Animal.prototype); Dog.prototype.constructor = Dog; new Dog('rex').speak()", "rex makes a sound"));
  CHECK(is("var o = { toString() { return 'custom'; } }; '' + o", "custom"));
  CHECK(is("String({})", "[object Object]"));
  CHECK(is("var o = Object.freeze({ a: 1 }); o.a = 2; o.a", "1"));
  CHECK(is("var o = { a: { b: null } }; o?.a?.b?.c", "undefined"));
  CHECK(is("var o = null; o?.x", "undefined"));
  CHECK(is("var o = { f() { return 1; } }; o.g?.()", "undefined"));
  CHECK(is("var o = { a: 1 }; o.a += 5; o.a", "6"));
  CHECK(is("var o = { n: 1 }; o.n++; ++o.n; o.n", "3"));
  CHECK(is("var a = [1]; a[0] += 2; a[0]++; a[0]", "4"));
  CHECK(is("var o = {}; o[1] = 'x'; o['1']", "x"));
  CHECK(is("null.x", "ERR: TypeError: Cannot read properties of null (reading 'x')"));
  CHECK(is("undefined.x = 1", "ERR: TypeError: Cannot set properties of undefined (setting 'x')"));
  CHECK(is("var f; f()", "ERR: TypeError: undefined is not a function"));
  CHECK(is("({}).x.y", "ERR: TypeError: Cannot read properties of undefined (reading 'y')"));
  CHECK(is("var o = {a:1,b:{c:[1,2,{d:3}]}}; JSON.stringify(o)", "{\"a\":1,\"b\":{\"c\":[1,2,{\"d\":3}]}}"));
}

static void test_arrays_and_strings() {
  CHECK(is("[1, 2, 3].length", "3"));
  CHECK(is("var a = []; a[3] = 1; a.length", "4"));
  CHECK(is("var a = [1, 2, 3]; a.length = 1; a.join()", "1"));
  CHECK(is("var a = [1]; a.push(2, 3); a.pop(); a.join()", "1,2"));
  CHECK(is("var a = [1, 2, 3]; a.shift() + a.length", "3"));
  CHECK(is("var a = [3]; a.unshift(1, 2); a.join()", "1,2,3"));
  CHECK(is("[1, 2, 3, 4].slice(1, 3).join()", "2,3"));
  CHECK(is("[1, 2, 3, 4].slice(-2).join()", "3,4"));
  CHECK(is("var a = [1, 2, 3, 4, 5]; var r = a.splice(1, 2); r.join() + '|' + a.join()", "2,3|1,4,5"));
  CHECK(is("var a = [1, 4]; a.splice(1, 0, 2, 3); a.join()", "1,2,3,4"));
  CHECK(is("[1, 2].concat([3], 4).join()", "1,2,3,4"));
  CHECK(is("[3, 1, 2].sort().join()", "1,2,3"));
  CHECK(is("[10, 9, 1].sort().join()", "1,10,9"));
  CHECK(is("[10, 9, 1].sort((a, b) => a - b).join()", "1,9,10"));
  CHECK(is("['b', 'a'].sort().reverse().join()", "b,a"));
  CHECK(is("[1, 2, 3].map(x => x * 2).join()", "2,4,6"));
  CHECK(is("[1, 2, 3, 4].filter(x => x % 2).join()", "1,3"));
  CHECK(is("[1, 2, 3].reduce((a, b) => a + b, 10)", "16"));
  CHECK(is("[1, 2, 3].reduce((a, b) => a + b)", "6"));
  CHECK(is("[].reduce((a, b) => a + b)", "ERR: TypeError: Reduce of empty array with no initial value"));
  CHECK(is("var s = 0; [1, 2, 3].forEach(function(x, i) { s += x * i; }); s", "8"));
  CHECK(is("[1, 2, 3].indexOf(2) + [1, 2, 3].indexOf(9)", "0"));
  CHECK(is("[1, 2, 3].includes(3)", "true"));
  CHECK(is("[1, 2, 3].find(x => x > 1) + [1, 2, 3].findIndex(x => x > 1)", "3"));
  CHECK(is("[1, 2, 3].some(x => x > 2) && [1, 2, 3].every(x => x > 0)", "true"));
  CHECK(is("[[1, 2], [3]].flat().join()", "1,2,3"));
  CHECK(is("Array.isArray([]) && !Array.isArray({})", "true"));
  CHECK(is("new Array(3).length", "3"));
  CHECK(is("Array(1, 2).join()", "1,2"));
  CHECK(is("[1, [2, 3]].toString()", "1,2,3"));
  CHECK(is("[1, 2, 3].fill(0).join()", "0,0,0"));
  CHECK(is("'hello'.toUpperCase() + 'WORLD'.toLowerCase()", "HELLOworld"));
  CHECK(is("'hello'.indexOf('l') + 'hello'.lastIndexOf('l')", "5"));
  CHECK(is("'hello'.slice(1, 3) + 'hello'.slice(-2)", "ello"));
  CHECK(is("'hello'.substring(3, 1) + 'hello'.substr(1, 2)", "elel"));
  CHECK(is("'a,b,c'.split(',').length", "3"));
  CHECK(is("'abc'.split('').join('-')", "a-b-c"));
  CHECK(is("'  pad '.trim() + '|'", "pad|"));
  CHECK(is("'abc'.charAt(1) + 'abc'.charCodeAt(0)", "b97"));
  CHECK(is("'aXbX'.replace('X', '-')", "a-bX"));
  CHECK(is("'aXbX'.replaceAll('X', '-')", "a-b-"));
  CHECK(is("'abc'.replace('b', m => m.toUpperCase())", "aBc"));
  CHECK(is("'abc'.startsWith('ab') && 'abc'.endsWith('bc') && 'abc'.includes('b')", "true"));
  CHECK(is("'ab'.repeat(3)", "ababab"));
  CHECK(is("'5'.padStart(3, '0') + 'a'.padEnd(3, '.')", "005a.."));
  CHECK(is("String(123) + String(true) + String(null) + String(undefined)", "123truenullundefined"));
  CHECK(is("String.fromCharCode(72, 105)", "Hi"));
  CHECK(is("'b'.localeCompare('a')", "1"));
  CHECK(is("'abc' < 'abd'", "true"));
}

static void test_builtins() {
  CHECK(is("Math.floor(2.7) + Math.ceil(2.1) + Math.round(2.5) + Math.trunc(-2.7)", "6"));
  CHECK(is("Math.abs(-3) + Math.max(1, 5, 2) + Math.min(4, 2)", "10"));
  CHECK(is("Math.sqrt(16) + Math.pow(2, 8)", "260"));
  CHECK(is("Math.PI.toFixed(5)", "3.14159"));
  CHECK(is("Math.sign(-5)", "-1"));
  CHECK(is("var r = Math.random(); r >= 0 && r < 1", "true"));
  CHECK(is("Math.round(Math.sin(Math.PI / 2) * 1000)", "1000"));
  CHECK(is("(255).toString(16) + (5).toString(2)", "ff101"));
  CHECK(is("(3.14159).toFixed(2) + (2).toFixed(1)", "3.142.0"));
  CHECK(is("parseInt('42px') + parseInt('0x10') + parseInt('101', 2)", "63"));
  CHECK(is("parseFloat('3.5abc') + parseFloat('.5')", "4"));
  CHECK(is("isNaN('x') + isNaN(1)", "1"));
  CHECK(is("Number('12') + Number('') + Number(null) + Number(true)", "13"));
  CHECK(is("Number.isInteger(5) && !Number.isInteger(5.5)", "true"));
  CHECK(is("Number.MAX_SAFE_INTEGER", "9007199254740991"));
  CHECK(is("Boolean(0) + '' + Boolean('a')", "falsetrue"));
  CHECK(is("JSON.stringify([1, 'a', null, undefined, true])", "[1,\"a\",null,null,true]"));
  CHECK(is("JSON.stringify({ a: undefined, b: function() {}, c: 'x\"y' })", "{\"c\":\"x\\\"y\"}"));
  CHECK(is("JSON.stringify({ a: [1, 2] }, null, 2)", "{\n  \"a\": [\n    1,\n    2\n  ]\n}"));
  CHECK(is("var o = JSON.parse('{\"a\":[1,2,{\"b\":null}],\"c\":\"x\\\\ny\",\"d\":-1.5e2,\"e\":true}'); o.a[1] + o.c.length + o.d + o.e + o.a[2].b", "-144"));
  CHECK(is("JSON.parse('[1,')", "ERR: SyntaxError: Unexpected token in JSON at position 3"));
  CHECK(is("JSON.stringify('a')", "\"a\""));
  CHECK(is("var d = new Date(86400000 * 365); d.getFullYear() + '-' + d.getMonth() + '-' + d.getDate()", "1971-0-1"));
  CHECK(is("new Date(0).toISOString()", "1970-01-01T00:00:00.000Z"));
  CHECK(is("Date.now() > 1000", "true"));
  CHECK(is("var e = new Error('boom'); e.message + ':' + e.name + ':' + (e instanceof Error)", "boom:Error:true"));
  CHECK(is("String(new TypeError('bad'))", "TypeError: bad"));
  CHECK(is("typeof window + typeof document + typeof navigator + typeof console", "objectobjectobjectobject"));
  CHECK(is("window === this && globalThis === window", "true"));
  CHECK(is("location.href", "http://example.com/page"));
  CHECK(is("navigator.userAgent", "Test/1"));
  CHECK(is("document.getElementById('x')", "null"));
  CHECK(is("document.querySelectorAll('p').length", "0"));
}

static void test_exceptions() {
  CHECK(is("try { throw new Error('x'); } catch (e) { e.message }", "x"));
  CHECK(is("try { throw 5; } catch (e) { e + 1 }", "6"));
  CHECK(is("try { null.x; } catch (e) { e instanceof TypeError }", "true"));
  CHECK(is("try { undefinedFn(); } catch (e) { e.name }", "ReferenceError"));
  CHECK(is("var r = ''; try { r += 'a'; } finally { r += 'b'; } r", "ab"));
  CHECK(is("var r = ''; try { try { throw 1; } finally { r += 'f'; } } catch (e) { r += 'c' } r", "fc"));
  CHECK(is("var r = ''; try { r += 't'; throw 1; } catch (e) { r += 'c'; } finally { r += 'f'; } r", "tcf"));
  CHECK(is("function f() { try { return 'try'; } finally { g = 'fin'; } } f() + g", "tryfin"));
  CHECK(is("function f() { try { throw 1; } catch (e) { return 'c'; } finally { g = 'f'; } } f() + g", "cf"));
  CHECK(is("function f() { for (var i = 0; i < 5; i++) { try { if (i == 2) break; } finally { n = i; } } return n; } f()", "2"));
  CHECK(is("function f() { var r = 0; for (var i = 0; i < 3; i++) { try { continue; } finally { r++; } } return r; } f()", "3"));
  CHECK(is("function f() { try { try { throw 'in'; } finally { } } catch (e) { return e; } } f()", "in"));
  CHECK(is("function f() { try { return 1; } finally { return 2; } } f()", "2"));
  CHECK(is("var c = 0; function g() { throw 'deep'; } function f() { g(); } try { f(); } catch (e) { e }", "deep"));
  CHECK(is("try { try { throw 1; } catch (e) { throw e + 1; } } catch (e2) { e2 }", "2"));
  CHECK(is("throw new Error('top')", "ERR: Error: top"));
  CHECK(is("throw 'str'", "ERR: str"));
  CHECK(is("function f() { return f(); } try { f(); } catch (e) { e instanceof RangeError }", "true"));
  CHECK(is("var e; try { [].map(5); } catch (x) { e = x.message; } e", "5 is not a function"));
  CHECK(is("1 +", "ERR: SyntaxError: Unexpected token (line 1)"));
  CHECK(is("var 5x = 1", "ERR: SyntaxError: Invalid or unexpected token (line 1)"));
  CHECK(is("class A {}", "ERR: SyntaxError: Classes are not supported (line 1)"));
  CHECK(is("var r = /ab+c/", "ERR: SyntaxError: Regular expression literals are not supported (line 1)"));
}

static void test_limits_and_collector() {
  // A runaway loop is stopped, the engine is usable again, and a script that allocates a lot is collected.
  {
    TestAllocator a;
    {
      TestHost host;
      Engine e(a, &host);
      e.set_limits(200000, 32u << 20);
      String error(a), result(a);
      CHECK(!e.run("while (true) {}", &error, &result) && std::string(error.c_str()).find("ran too long") != std::string::npos);
      CHECK(e.run("1 + 1", &error, &result) && std::string(result.c_str()) == "2");
      e.set_limits(50000000, 32u << 20);
      CHECK(e.run("var t = 0; for (var i = 0; i < 300000; i++) { var o = { a: i, b: [i, i + 1], c: 'x' + i }; t += o.b[1] - o.a; } t", &error, &result) && std::string(result.c_str()) == "300000");
      CHECK(e.vm().heap_bytes() < (24u << 20));   // (the garbage went away)
      CHECK(!e.run("var s = 'x'; while (true) { s = s + s; }", &error, &result) && !error.empty());
      CHECK(e.run("2 + 2", &error, &result) && std::string(result.c_str()) == "4");
      // Globals live on between scripts.
      CHECK(e.run("var shared = [1, 2, 3];", &error, &result) && e.run("shared.length", &error, &result) && std::string(result.c_str()) == "3");
    }
    CHECK(a.live == 0);
  }
  CHECK(is("var a = []; for (var i = 0; i < 20000; i++) a.push({ i: i }); a.length + a[19999].i", "39999"));
  CHECK(is("var s = ''; for (var i = 0; i < 2000; i++) s += 'ab'; s.length", "4000"));
  CHECK(is("function f(n) { return n == 0 ? 0 : 1 + f(n - 1); } f(150)", "150"));
  CHECK(is("var o = {}; for (var i = 0; i < 5000; i++) o['k' + i] = i; Object.keys(o).length + o.k4999", "9999"));
}


// Every prefix of a program, and the program with each character dropped, must be dealt with (compiled or refused) without crashing.
static void test_damaged_programs() {
  static const char* kProgram =
      "var o = { a: 1, b: [1, 2, { c: `t${1 + 1}x` }], f(x, ...r) { return x + r.length; }, ['k' + 1]: 5 };\n"
      "function g(a, b = 2) { try { for (var i = 0; i < a; i++) { if (i == 1) continue; o.a += i; } } catch (e) { return e; } finally { o.z = 1; } return o?.a ?? 0; }\n"
      "const h = (x, y) => x ** y; let q = [...o.b, 3]; for (const v of q) { switch (v) { case 1: break; default: h(2, 3); } }\n"
      "outer: while (true) { do { break outer; } while (0); } var s = 'a\\n' + typeof zz + (1, 2) + !0 + -1 + new g(1) + delete o.a;";
  TestAllocator a;
  {
    TestHost host;
    Engine e(a, &host);
    e.set_limits(100000, 16u << 20);
    String error(a), result(a);
    const uint64_t n = strlen(kProgram);
    for (uint64_t i = 0; i <= n; ++i) e.run(StringView(kProgram, i), &error, &result);
    std::string copy;
    for (uint64_t i = 0; i < n; ++i) {
      copy.assign(kProgram, i);
      copy.append(kProgram + i + 1, n - i - 1);
      e.run(StringView(copy.data(), copy.size()), &error, &result);
    }
    for (uint64_t i = 0; i < n; ++i) {   // and with a stray character put in
      copy.assign(kProgram, i);
      copy.push_back("{}()[];,.:?`'\"/*=<>+-"[i % 22]);
      copy.append(kProgram + i, n - i);
      e.run(StringView(copy.data(), copy.size()), &error, &result);
    }
    CHECK(e.run("1", &error, &result));
  }
  CHECK(a.live == 0);
}

// Programs nested or chained far deeper than anyone writes are refused, not run on the stack until it runs out.
static void test_deep_programs() {
  std::string deep;
  for (int i = 0; i < 5000; ++i) deep += "(";
  deep += "1";
  for (int i = 0; i < 5000; ++i) deep += ")";
  CHECK(eval(deep.c_str()).find("ERR: SyntaxError: Too much nesting") == 0);
  std::string blocks;
  for (int i = 0; i < 3000; ++i) blocks += "{";
  CHECK(eval(blocks.c_str()).find("ERR: SyntaxError") == 0);
  std::string nots(20000, '!');
  CHECK(eval((nots + "1").c_str()).find("ERR: SyntaxError: Too much nesting") == 0);
  std::string sum = "1";
  for (int i = 0; i < 6000; ++i) sum += "+1";
  CHECK(eval(sum.c_str()).find("ERR: SyntaxError: Expression too long") == 0);
  std::string chain = "var o = {a: null}; o";
  for (int i = 0; i < 6000; ++i) chain += ".a";
  CHECK(eval(chain.c_str()).find("ERR: SyntaxError: Expression too long") == 0);
  std::string ok_sum = "0";
  for (int i = 0; i < 2000; ++i) ok_sum += "+1";
  CHECK(eval(ok_sum.c_str()) == "2000");
  std::string arrays;
  for (int i = 0; i < 100; ++i) arrays += "[";
  for (int i = 0; i < 100; ++i) arrays += "]";
  CHECK(eval(arrays.c_str()).find("ERR") == std::string::npos);   // (100 nested arrays are fine)
}

static void test_host_integration() {
  TestAllocator a;
  {
    TestHost host;
    Engine e(a, &host);
    String error(a), result(a);
    CHECK(e.run("console.log('hi', 1, [1, 'a'], { x: 1 }); console.warn('w'); console.error(new Error('e'));", &error, &result));
    CHECK(host.console_lines.size() == 3 && host.console_lines[0] == "0:hi 1 [1, 'a'] {x: 1}" && host.console_lines[1] == "2:w" && host.console_lines[2] == "3:Error: e");
    CHECK(e.run("alert('careful'); document.write('<b>x</b>'); document.writeln('y', 2);", &error, &result));
    CHECK(host.alerts.size() == 1 && host.alerts[0] == "careful" && host.writes.size() == 2 && host.writes[0] == "<b>x</b>" && host.writes[1] == "y2\n");
    CHECK(e.run("document.title = 'New title';", &error, &result) && host.page_title == "New title");
    // Timers run in the order they are due, and the listeners of load events run.
    CHECK(e.run("var order = []; setTimeout(function() { order.push('b'); }, 20); setTimeout(function(x) { order.push(x); setTimeout(function() { order.push('d'); }, 5); }, 10, 'a'); "
                "setTimeout(() => order.push('c'), 20); var cancelled = setTimeout(() => order.push('X'), 1); clearTimeout(cancelled);", &error, &result));
    CHECK(e.run_timers(20, &error) == 0 && e.run("order.join('')", &error, &result) && std::string(result.c_str()) == "adbc");
    CHECK(e.run("var loaded = []; document.addEventListener('DOMContentLoaded', function(ev) { loaded.push(ev.type); }); window.addEventListener('load', () => loaded.push('load')); window.onload = function() { loaded.push('onload'); };", &error, &result));
    CHECK(e.fire_event("DOMContentLoaded", &error) == 0 && e.fire_event("load", &error) == 0);
    CHECK(e.run("loaded.join()", &error, &result) && std::string(result.c_str()) == "DOMContentLoaded,load,onload");
    // A timer that throws is reported and the others still run.
    CHECK(e.run("setTimeout(function() { throw new Error('late'); }, 1); setTimeout(function() { ran = 1; }, 2);", &error, &result));
    String first(a);
    CHECK(e.run_timers(5, &first) == 1 && std::string(first.c_str()) == "Error: late" && e.run("ran", &error, &result) && std::string(result.c_str()) == "1");
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_expressions();
  test_variables_and_functions();
  test_control_flow();
  test_objects();
  test_arrays_and_strings();
  test_builtins();
  test_exceptions();
  test_limits_and_collector();
  test_damaged_programs();
  test_deep_programs();
  test_host_integration();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("script tests passed\n");
  return 0;
}
