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


// The parts of the standard library that are written in the language itself: the rest of Array, String, Object, Math and Number, Date (with parsing and
// the UTC and local accessors - the local time zone is UTC), Intl (number, date, plural, relative time and list formats in English, with a few other
// locales' orders), AggregateError and the like. Run once for each engine, after the collections (see Engine.cpp).
#include "Script/Stdlib.hpp"

namespace script {

namespace {

const char kStdlib[] = R"JSLIB((function (g) {
  var def = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var AP = Array.prototype, SP = String.prototype, OP = Object.prototype, NP = Number.prototype;
  var ITER = Symbol.iterator;
  function toInt(v) { v = Number(v); return v !== v ? 0 : Math.trunc(v); }
  function need(fn) { if (typeof fn !== 'function') throw new TypeError(String(fn) + ' is not a function'); }

  // ------------------------------------------------------------------------------------------------------------------ Array
  def(AP, 'at', function (i) { i = toInt(i); if (i < 0) i += this.length; return this[i]; });
  def(AP, 'findLast', function (fn, t) { need(fn); for (var i = this.length - 1; i >= 0; i--) if (fn.call(t, this[i], i, this)) return this[i]; return undefined; });
  def(AP, 'findLastIndex', function (fn, t) { need(fn); for (var i = this.length - 1; i >= 0; i--) if (fn.call(t, this[i], i, this)) return i; return -1; });
  def(AP, 'flatMap', function (fn, t) {
    need(fn);
    var out = [];
    for (var i = 0; i < this.length; i++) { var v = fn.call(t, this[i], i, this); if (Array.isArray(v)) for (var j = 0; j < v.length; j++) out.push(v[j]); else out.push(v); }
    return out;
  });
  def(AP, 'copyWithin', function (target, start, end) {
    var n = this.length;
    function rel(v, d) { v = v === undefined ? d : toInt(v); return v < 0 ? Math.max(n + v, 0) : Math.min(v, n); }
    var to = rel(target, 0), from = rel(start, 0), last = rel(end, n), count = Math.min(last - from, n - to), copy = [];
    for (var i = 0; i < count; i++) copy.push(this[from + i]);
    for (var j = 0; j < count; j++) this[to + j] = copy[j];
    return this;
  });
  def(AP, 'toReversed', function () { return this.slice().reverse(); });
  def(AP, 'toSorted', function (compare) { return this.slice().sort(compare); });
  def(AP, 'toSpliced', function () { var copy = this.slice(); copy.splice.apply(copy, arguments); return copy; });
  def(AP, 'with', function (index, value) {
    var copy = this.slice(); index = toInt(index); if (index < 0) index += copy.length;
    if (index < 0 || index >= copy.length) throw new RangeError('Invalid index : ' + index);
    copy[index] = value; return copy;
  });
  function arrayWalk(a, kind) { return __array_iterator(a, kind); }
  def(AP, 'keys', function () { return arrayWalk(this, 0); });
  def(AP, 'values', function () { return arrayWalk(this, 1); });
  def(AP, 'entries', function () { return arrayWalk(this, 2); });
  def(AP, ITER, AP.values);
  def(String.prototype, ITER, function () { return arrayWalk([...String(this)], 1); });
  def(AP, 'toLocaleString', function () { return this.map(function (v) { return v === null || v === undefined ? '' : v.toLocaleString(); }).join(','); });

  // ------------------------------------------------------------------------------------------------------------------ String
  // (a string is a sequence of UTF-16 code units; a character outside the BMP is a surrogate pair)
  def(SP, 'codePointAt', function (i) {
    i = toInt(i); var a = this.charCodeAt(i);
    if (a !== a) return undefined;
    if (a >= 0xD800 && a <= 0xDBFF) { var b = this.charCodeAt(i + 1); if (b >= 0xDC00 && b <= 0xDFFF) return (a - 0xD800) * 0x400 + (b - 0xDC00) + 0x10000; }
    return a;
  });
  def(String, 'raw', function (strings) {
    var raw = strings.raw, out = '';
    for (var i = 0; i < raw.length; i++) { out += raw[i]; if (i + 1 < raw.length && i + 1 < arguments.length) out += arguments[i + 1]; }
    return out;
  });
  // Turkish and Azeri keep the dot of the i: I is the dotless i when lowercased, i is the dotted I when uppercased
  function dotted(locale) { if (Array.isArray(locale)) locale = locale[0]; return typeof locale === 'string' && /^(tr|az)(-|$)/i.test(locale); }
  var DOTLESS = String.fromCharCode(0x131), DOTTED = String.fromCharCode(0x130);
  def(SP, 'toLocaleLowerCase', function (locale) {
    var s = String(this);
    return dotted(locale) ? s.replace(new RegExp(DOTTED, 'g'), 'i').replace(/I(?!\u0307)/g, DOTLESS).replace(/I\u0307/g, 'i').toLowerCase() : s.toLowerCase();
  });
  def(SP, 'toLocaleUpperCase', function (locale) {
    var s = String(this);
    return dotted(locale) ? s.replace(/i/g, DOTTED).toUpperCase() : s.toUpperCase();
  });
  function lonelySurrogate(text, i) {
    var c = text.charCodeAt(i);
    if (c >= 0xD800 && c <= 0xDBFF) { var d = text.charCodeAt(i + 1); return !(d >= 0xDC00 && d <= 0xDFFF); }
    if (c >= 0xDC00 && c <= 0xDFFF) { var p = i > 0 ? text.charCodeAt(i - 1) : 0; return !(p >= 0xD800 && p <= 0xDBFF); }
    return false;
  }
  def(SP, 'isWellFormed', function () { var t = String(this); for (var i = 0; i < t.length; i++) if (lonelySurrogate(t, i)) return false; return true; });
  def(SP, 'toWellFormed', function () { var t = String(this), out = ''; for (var i = 0; i < t.length; i++) out += lonelySurrogate(t, i) ? '\uFFFD' : t[i]; return out; });
  // Symbol.prototype: a symbol is a string cell the engine flags (see Vm::new_symbol); __symbol_info reads it
  var SymP = Symbol.prototype;
  Object.defineProperty(SymP, 'description', { get: function () { return __symbol_info(this, 0); }, configurable: true });
  def(SymP, 'toString', function () { return __symbol_info(this, 1); });
  def(SymP, 'valueOf', function () { return __symbol_info(this, 4); });
  def(SymP, Symbol.toPrimitive, function () { return __symbol_info(this, 4); });
  Object.defineProperty(SymP, Symbol.toStringTag, { value: 'Symbol', configurable: true });
  var symbolRegistry = new Map(), symbolKeys = new Map();
  def(Symbol, 'for', function (key) {
    key = String(key);
    var sym = symbolRegistry.get(key);
    if (sym === undefined) { sym = Symbol(key); symbolRegistry.set(key, sym); symbolKeys.set(sym, key); }
    return sym;
  });
  def(Symbol, 'keyFor', function (sym) {
    if (!__symbol_info(sym, 2)) throw new TypeError(String(sym) + ' is not a symbol');
    return symbolKeys.get(sym);
  });

  // the well-known symbols that change what a built-in does: the String methods ask the argument first, concat asks isConcatSpreadable
  [['match', Symbol.match], ['matchAll', Symbol.matchAll], ['replace', Symbol.replace], ['search', Symbol.search], ['split', Symbol.split]].forEach(function (pair) {
    var name = pair[0], sym = pair[1], orig = SP[name];
    var wrapper = function (arg) {
      if (new.target !== undefined) throw new TypeError('String.prototype.' + name + ' is not a constructor');
      if (arg !== null && arg !== undefined && typeof arg === 'object' && !(arg instanceof RegExp)) {
        var method = arg[sym];
        if (typeof method === 'function') return method.apply(arg, [this].concat(Array.prototype.slice.call(arguments, 1)));
      }
      return orig.apply(this, arguments);
    };
    Object.defineProperty(wrapper, 'name', { value: name });
    Object.defineProperty(wrapper, 'length', { value: orig.length });
    def(SP, name, wrapper);
  });
  (function () {
    var orig = AP.concat, spread = Symbol.isConcatSpreadable;
    var wrapper = function (item) {
      'use strict';
      if (new.target !== undefined) throw new TypeError('Array.prototype.concat is not a constructor');
      var args = [], base = this;
      if (base === null || base === undefined) throw new TypeError('Array.prototype.concat called on null or undefined');
      var flagged = typeof base === 'object' || typeof base === 'function' ? base[spread] : undefined;
      if (!(flagged !== undefined ? flagged : Array.isArray(base))) base = [typeof base === 'object' || typeof base === 'function' ? base : Object(base)];   // (not spreadable: it is the first element)
      for (var i = 0; i < arguments.length; i++) {
        var a = arguments[i];
        if (a !== null && typeof a === 'object') {
          var flag = a[spread];
          if (flag !== undefined) a = flag ? Array.prototype.slice.call(a) : [a];
        }
        args.push(a);
      }
      return orig.apply(base, args);
    };
    Object.defineProperty(wrapper, 'name', { value: 'concat' });
    Object.defineProperty(wrapper, 'length', { value: 1 });
    def(AP, 'concat', wrapper);
  }());
  var unscopables = Object.create(null);
  ['at', 'copyWithin', 'entries', 'fill', 'find', 'findIndex', 'findLast', 'findLastIndex', 'flat', 'flatMap', 'includes', 'keys', 'toReversed', 'toSorted', 'toSpliced', 'values'].forEach(function (n) { unscopables[n] = true; });
  Object.defineProperty(AP, Symbol.unscopables, { value: unscopables, configurable: true });
  [Array, Map, Set, Promise, RegExp, ArrayBuffer].forEach(function (C) {
    if (typeof C !== 'function') return;
    var getter = function () { return this; };
    Object.defineProperty(getter, 'name', { value: 'get [Symbol.species]' });
    Object.defineProperty(C, Symbol.species, { get: getter, configurable: true });
  });

  // ------------------------------------------------------------------------------------------------------------------ Object
  def(Object, 'hasOwn', function (o, k) { return OP.hasOwnProperty.call(o, k); });
  def(Object, 'getOwnPropertyDescriptors', function (o) {
    var out = {}, names = Object.getOwnPropertyNames(o);
    for (var i = 0; i < names.length; i++) out[names[i]] = Object.getOwnPropertyDescriptor(o, names[i]);
    return out;
  });
  def(Object, 'groupBy', function (items, fn) {
    var out = Object.create(null), i = 0;
    for (var item of items) { var key = String(fn(item, i++)); (out[key] = out[key] || []).push(item); }
    return out;
  });
  def(OP, 'isPrototypeOf', function (o) { for (var p = o === null || (typeof o !== 'object' && typeof o !== 'function') ? null : Object.getPrototypeOf(o); p; p = Object.getPrototypeOf(p)) if (p === this) return true; return false; });
  def(OP, 'propertyIsEnumerable', function (k) { return Object.keys(this).indexOf(String(k)) >= 0; });
  def(OP, 'toLocaleString', function () { return this.toString(); });
  if (typeof Map === 'function') def(Map, 'groupBy', function (items, fn) {
    var out = new Map(), i = 0;
    for (var item of items) { var key = fn(item, i++); if (!out.has(key)) out.set(key, []); out.get(key).push(item); }
    return out;
  });

  // ------------------------------------------------------------------------------------------------------------------ Errors
  ['EvalError', 'URIError'].forEach(function (name) {
    var E = function (message, options) { var e = new Error(message, options); Object.setPrototypeOf(e, E.prototype); return e; };
    E.prototype = Object.create(Error.prototype);
    def(E.prototype, 'constructor', E);
    def(E.prototype, 'name', name);
    Object.defineProperty(E, 'name', { value: name });
    def(g, name, E);
  });
  function AggregateError(errors, message, options) {
    var e = new Error(message, options);
    Object.setPrototypeOf(e, AggregateError.prototype);
    def(e, 'errors', Array.from(errors));
    return e;
  }
  AggregateError.prototype = Object.create(Error.prototype);
  def(AggregateError.prototype, 'constructor', AggregateError);
  def(AggregateError.prototype, 'name', 'AggregateError');
  def(g, 'AggregateError', AggregateError);
  def(Promise, 'try', function (fn) { var args = Array.prototype.slice.call(arguments, 1); return new Promise(function (resolve) { resolve(fn.apply(undefined, args)); }); });

  // ------------------------------------------------------------------------------------------------------------------ Math
  var M = Math;
  var f32 = new Float32Array(1), i32 = new Int32Array(1);
  function setm(name, fn) { if (M[name] === undefined) def(M, name, fn); }
  setm('LOG10E', 0.4342944819032518); setm('LOG2E', 1.4426950408889634); setm('SQRT1_2', 0.7071067811865476);
  def(M, 'LOG10E', 0.4342944819032518); def(M, 'LOG2E', 1.4426950408889634); def(M, 'SQRT1_2', 0.7071067811865476);
  setm('asin', function (x) { return x > 1 || x < -1 ? NaN : M.atan2(x, M.sqrt(1 - x * x)); });
  setm('acos', function (x) { return x > 1 || x < -1 ? NaN : M.atan2(M.sqrt(1 - x * x), x); });
  setm('sinh', function (x) { return (M.exp(x) - M.exp(-x)) / 2; });
  setm('cosh', function (x) { return (M.exp(x) + M.exp(-x)) / 2; });
  setm('tanh', function (x) { if (x === Infinity) return 1; if (x === -Infinity) return -1; var a = M.exp(x), b = M.exp(-x); return (a - b) / (a + b); });
  setm('asinh', function (x) { return x === -Infinity ? x : M.log(x + M.sqrt(x * x + 1)); });
  setm('acosh', function (x) { return x < 1 ? NaN : M.log(x + M.sqrt(x * x - 1)); });
  setm('atanh', function (x) { return x > 1 || x < -1 ? NaN : 0.5 * M.log((1 + x) / (1 - x)); });
  setm('expm1', function (x) { return M.abs(x) < 1e-5 ? x + x * x / 2 + x * x * x / 6 : M.exp(x) - 1; });
  setm('log1p', function (x) { return M.abs(x) < 1e-5 ? x - x * x / 2 + x * x * x / 3 : M.log(1 + x); });
  setm('clz32', function (x) { x = x >>> 0; if (x === 0) return 32; var n = 0; while ((x & 0x80000000) === 0) { x <<= 1; n++; } return n; });
  setm('imul', function (a, b) { a |= 0; b |= 0; var ah = (a >>> 16) & 0xFFFF, al = a & 0xFFFF, bh = (b >>> 16) & 0xFFFF, bl = b & 0xFFFF; return ((al * bl) + (((ah * bl + al * bh) << 16) >>> 0)) | 0; });
  setm('fround', function (x) { f32[0] = x; return f32[0]; });

  // ------------------------------------------------------------------------------------------------------------------ Number
  def(Number, 'isSafeInteger', function (v) { return typeof v === 'number' && isFinite(v) && Math.floor(v) === v && Math.abs(v) <= 9007199254740991; });
  function digitsOf(x, count) {   // the first `count` significant digits of a positive number, and the decimal exponent
    var e = M.floor(M.log10(x));
    var m = x / M.pow(10, e);
    var digits = M.round(m * M.pow(10, count - 1));
    if (digits >= M.pow(10, count)) { digits = M.round(digits / 10); e++; }
    if (digits < M.pow(10, count - 1)) { digits = M.round(digits * 10); e--; }
    return { digits: String(digits), exponent: e };
  }
  def(NP, 'toExponential', function (fd) {
    var x = Number(this);
    if (x !== x || x === Infinity || x === -Infinity) return String(x);
    var sign = x < 0 ? '-' : ''; x = M.abs(x);
    var count = fd === undefined ? 17 : toInt(fd) + 1, parts, d, e;
    if (x === 0) { d = '0'.repeat(count); e = 0; }
    else if (fd === undefined) { var s = String(x), ex = x.toString().indexOf('e'); var p = digitsOf(x, 15); d = p.digits.replace(/0+$/, '') || '0'; e = p.exponent; }
    else { parts = digitsOf(x, count); d = parts.digits; e = parts.exponent; }
    return sign + d.charAt(0) + (d.length > 1 ? '.' + d.slice(1) : '') + 'e' + (e < 0 ? '-' : '+') + M.abs(e);
  });
  def(NP, 'toPrecision', function (p) {
    var x = Number(this);
    if (p === undefined || x !== x || x === Infinity || x === -Infinity) return String(x);
    p = toInt(p);
    if (p < 1 || p > 100) throw new RangeError('toPrecision() argument must be between 1 and 100');
    if (x === 0) return p > 1 ? '0.' + '0'.repeat(p - 1) : '0';
    var sign = x < 0 ? '-' : ''; x = M.abs(x);
    var r = digitsOf(x, p), e = r.exponent, d = r.digits;
    if (e < -6 || e >= p) return sign + d.charAt(0) + (p > 1 ? '.' + d.slice(1) : '') + 'e' + (e < 0 ? '-' : '+') + M.abs(e);
    if (e >= 0) return sign + d.slice(0, e + 1) + (p > e + 1 ? '.' + d.slice(e + 1) : '');
    return sign + '0.' + '0'.repeat(-e - 1) + d;
  });

  // ------------------------------------------------------------------------------------------------------------------ Date
  var NativeDate = g.Date;
  var DAY = 86400000;
  var MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
  var DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
  function daysFromCivil(y, m, d) {   // m: 1..12
    y -= m <= 2 ? 1 : 0;
    var era = M.floor(y / 400), yoe = y - era * 400;
    var doy = M.floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
    var doe = yoe * 365 + M.floor(yoe / 4) - M.floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
  }
  function civil(days) {
    var z = days + 719468, era = M.floor(z / 146097), doe = z - era * 146097;
    var yoe = M.floor((doe - M.floor(doe / 1460) + M.floor(doe / 36524) - M.floor(doe / 146096)) / 365);
    var y = yoe + era * 400, doy = doe - (365 * yoe + M.floor(yoe / 4) - M.floor(yoe / 100));
    var mp = M.floor((5 * doy + 2) / 153), d = doy - M.floor((153 * mp + 2) / 5) + 1, m = mp < 10 ? mp + 3 : mp - 9;
    return [m <= 2 ? y + 1 : y, m, d];
  }
  function makeDay(y, mo, d) {   // mo may be out of 0..11
    if (!isFinite(y) || !isFinite(mo) || !isFinite(d)) return NaN;
    y = M.trunc(y); mo = M.trunc(mo); d = M.trunc(d);
    y += M.floor(mo / 12); mo = ((mo % 12) + 12) % 12;
    return daysFromCivil(y, mo + 1, 1) + d - 1;
  }
  function makeTime(days, h, mi, s, ms) {
    if (days !== days || !isFinite(h) || !isFinite(mi) || !isFinite(s) || !isFinite(ms)) return NaN;
    return days * DAY + M.trunc(h) * 3600000 + M.trunc(mi) * 60000 + M.trunc(s) * 1000 + M.trunc(ms);
  }
  function clip(t) { return t !== t || M.abs(t) > 8.64e15 ? NaN : M.trunc(t) + 0; }
  function fields(t) {
    var days = M.floor(t / DAY), rest = t - days * DAY, c = civil(days);
    return { year: c[0], month: c[1] - 1, day: c[2], hour: M.floor(rest / 3600000), minute: M.floor(rest / 60000) % 60, second: M.floor(rest / 1000) % 60, ms: rest % 1000, weekday: ((days + 4) % 7 + 7) % 7 };
  }
  function pad(n, w) { var s = String(M.abs(n)); while (s.length < w) s = '0' + s; return (n < 0 ? '-' : '') + s; }
  var OFFSET_RE = /^(Z|[+-]\d\d(?::?\d\d)?)$/;
  function parseDate(text) {
    text = String(text).trim();
    var m = /^([+-]\d{6}|\d{4})(?:-(\d\d)(?:-(\d\d))?)?(?:[T ](\d\d):(\d\d)(?::(\d\d)(?:[.,](\d{1,9}))?)?)?\s*(Z|[+-]\d\d(?::?\d\d)?)?$/i.exec(text);
    if (m) {
      var y = Number(m[1]), mo = m[2] ? Number(m[2]) - 1 : 0, d = m[3] ? Number(m[3]) : 1, h = m[4] ? Number(m[4]) : 0, mi = m[5] ? Number(m[5]) : 0, s = m[6] ? Number(m[6]) : 0;
      var frac = m[7] ? Number('0.' + m[7]) * 1000 : 0;
      if (mo > 11 || d > 31 || h > 24 || mi > 59 || s > 59) return NaN;
      var t = makeTime(makeDay(y, mo, d), h, mi, s, M.floor(frac));
      if (m[8] && m[8].toUpperCase() !== 'Z') { var sign = m[8].charAt(0) === '-' ? -1 : 1; var digits = m[8].slice(1).replace(':', ''); t -= sign * (Number(digits.slice(0, 2)) * 60 + Number(digits.slice(2) || 0)) * 60000; }
      return t;
    }
    // "Tue, 15 Jan 2020 10:30:00 GMT", "Jan 15, 2020 10:30", "15 January 2020", "Tue Jan 15 2020 10:30:00 GMT+0100 (CET)", "1/15/2020"
    var clean = text.replace(/\([^)]*\)/g, ' ').replace(/,/g, ' ').replace(/\s+/g, ' ').trim();
    var year = NaN, month = NaN, day = NaN, h2 = 0, mi2 = 0, s2 = 0, ms2 = 0, offset = 0, pm = null, found = 0;
    var tokens = clean.split(' ');
    for (var i = 0; i < tokens.length; i++) {
      var tok = tokens[i], low = tok.toLowerCase(), mm;
      if ((mm = /^(\d{1,2}):(\d\d)(?::(\d\d)(?:\.(\d+))?)?$/.exec(tok))) { h2 = Number(mm[1]); mi2 = Number(mm[2]); s2 = mm[3] ? Number(mm[3]) : 0; ms2 = mm[4] ? M.floor(Number('0.' + mm[4]) * 1000) : 0; found++; continue; }
      if (low === 'am' || low === 'pm') { pm = low === 'pm'; continue; }
      if ((mm = /^(?:gmt|utc|z)?([+-])(\d\d):?(\d\d)?$/i.exec(tok))) { offset = (mm[1] === '-' ? -1 : 1) * (Number(mm[2]) * 60 + Number(mm[3] || 0)); continue; }
      if (low === 'gmt' || low === 'utc' || low === 'z' || low === 'ut') continue;
      if ((mm = /^(\d{1,2})\/(\d{1,2})\/(\d{2,4})$/.exec(tok))) { month = Number(mm[1]) - 1; day = Number(mm[2]); year = Number(mm[3]); if (year < 100) year += year < 50 ? 2000 : 1900; found += 2; continue; }
      if ((mm = /^(\d{4})\/(\d{1,2})\/(\d{1,2})$/.exec(tok))) { year = Number(mm[1]); month = Number(mm[2]) - 1; day = Number(mm[3]); found += 2; continue; }
      var named = -1;
      for (var k = 0; k < 12; k++) if (low.length >= 3 && MONTHS[k].toLowerCase().indexOf(low.replace(/\.$/, '')) === 0) named = k;
      if (named >= 0) { month = named; found++; continue; }
      var isDay = false;
      for (var w = 0; w < 7; w++) if (low.length >= 3 && DAYS[w].toLowerCase().indexOf(low.replace(/\.$/, '')) === 0) isDay = true;
      if (isDay) continue;
      if ((mm = /^(\d{1,4})(?:st|nd|rd|th)?$/.exec(low))) {
        var num = Number(mm[1]);
        if (tok.length >= 3 || num > 31) year = num < 100 && tok.length <= 2 ? (num < 50 ? 2000 + num : 1900 + num) : num;
        else if (day !== day) day = num;
        else year = num < 50 ? 2000 + num : 1900 + num;
        found++; continue;
      }
      return NaN;
    }
    if (year !== year || month !== month) return NaN;
    if (day !== day) day = 1;
    if (pm === true && h2 < 12) h2 += 12;
    if (pm === false && h2 === 12) h2 = 0;
    return makeTime(makeDay(year, month, day), h2, mi2, s2, ms2) - offset * 60000;
  }
  function Date_(a, b, c, d, e, f, h) {
    if (!(this instanceof Date_)) return new Date_().toString();
    var t;
    var n = arguments.length;
    if (n === 0) t = NativeDate.now();
    else if (n === 1) {
      if (a !== null && typeof a === 'object' && a.__date) t = a.__t;
      else {
        if (a !== null && typeof a === 'object') a = a.valueOf();
        t = typeof a === 'string' ? parseDate(a) : Number(a);
      }
    } else {
      var year = Number(a);
      if (year === year && year >= 0 && year <= 99 && M.trunc(year) === year) year += 1900;
      t = makeTime(makeDay(year, Number(b), c === undefined ? 1 : Number(c)), d === undefined ? 0 : Number(d), e === undefined ? 0 : Number(e), f === undefined ? 0 : Number(f), h === undefined ? 0 : Number(h));
    }
    Object.defineProperty(this, '__t', { value: clip(t), writable: true });
    Object.defineProperty(this, '__date', { value: true, writable: true });
  }
  var DP = NativeDate.prototype;
  Date_.prototype = DP;
  def(DP, 'constructor', Date_);
  def(Date_, 'now', NativeDate.now);
  def(Date_, 'parse', function (s) { return clip(parseDate(s)); });
  def(Date_, 'UTC', function (y, mo, d, h, mi, s, ms) {
    y = Number(y); if (y === y && y >= 0 && y <= 99) y += 1900;
    return clip(makeTime(makeDay(y, mo === undefined ? 0 : Number(mo), d === undefined ? 1 : Number(d)), h === undefined ? 0 : Number(h), mi === undefined ? 0 : Number(mi), s === undefined ? 0 : Number(s), ms === undefined ? 0 : Number(ms)));
  });
  function time(self) { if (!self || self.__date !== true) throw new TypeError('this is not a Date object.'); return self.__t; }
  var getters = { FullYear: 'year', Month: 'month', Date: 'day', Hours: 'hour', Minutes: 'minute', Seconds: 'second', Milliseconds: 'ms', Day: 'weekday' };
  Object.keys(getters).forEach(function (name) {
    var key = getters[name];
    var read = function () { var t = time(this); return t !== t ? NaN : fields(t)[key]; };
    def(DP, 'get' + name, read);
    def(DP, 'getUTC' + name, read);
  });
  def(DP, 'getTime', function () { return time(this); });
  def(DP, 'valueOf', function () { return time(this); });
  def(DP, 'getYear', function () { var t = time(this); return t !== t ? NaN : fields(t).year - 1900; });
  def(DP, 'getTimezoneOffset', function () { return 0; });
  def(DP, 'setTime', function (v) { time(this); this.__t = clip(Number(v)); return this.__t; });
  function setter(name, apply) {
    var set = function () {
      var t = time(this);
      if (t !== t && name !== 'FullYear') return NaN;
      var f = t !== t ? fields(0) : fields(t), args = [];
      for (var i = 0; i < arguments.length; i++) args.push(Number(arguments[i]));
      apply(f, args);
      this.__t = clip(makeTime(makeDay(f.year, f.month, f.day), f.hour, f.minute, f.second, f.ms));
      return this.__t;
    };
    def(DP, 'set' + name, set);
    def(DP, 'setUTC' + name, set);
  }
  function opt(args, i, f) { return i < args.length ? args[i] : f; }
  setter('Milliseconds', function (f, a) { f.ms = a[0]; });
  setter('Seconds', function (f, a) { f.second = a[0]; f.ms = opt(a, 1, f.ms); });
  setter('Minutes', function (f, a) { f.minute = a[0]; f.second = opt(a, 1, f.second); f.ms = opt(a, 2, f.ms); });
  setter('Hours', function (f, a) { f.hour = a[0]; f.minute = opt(a, 1, f.minute); f.second = opt(a, 2, f.second); f.ms = opt(a, 3, f.ms); });
  setter('Date', function (f, a) { f.day = a[0]; });
  setter('Month', function (f, a) { f.month = a[0]; f.day = opt(a, 1, f.day); });
  setter('FullYear', function (f, a) { f.year = a[0]; f.month = opt(a, 1, f.month); f.day = opt(a, 2, f.day); });
  function iso(t) {
    var f = fields(t), y = f.year;
    var ys = y >= 0 && y <= 9999 ? pad(y, 4) : (y < 0 ? '-' : '+') + pad(M.abs(y), 6);
    return ys + '-' + pad(f.month + 1, 2) + '-' + pad(f.day, 2) + 'T' + pad(f.hour, 2) + ':' + pad(f.minute, 2) + ':' + pad(f.second, 2) + '.' + pad(f.ms, 3) + 'Z';
  }
  def(DP, 'toISOString', function () { var t = time(this); if (t !== t) throw new RangeError('Invalid time value'); return iso(t); });
  def(DP, 'toJSON', function () { var t = time(this); return t !== t ? null : iso(t); });
  function dateText(f) { return DAYS[f.weekday].slice(0, 3) + ' ' + MONTHS[f.month].slice(0, 3) + ' ' + pad(f.day, 2) + ' ' + pad(f.year, 4); }
  function timeText(f) { return pad(f.hour, 2) + ':' + pad(f.minute, 2) + ':' + pad(f.second, 2) + ' GMT+0000 (Coordinated Universal Time)'; }
  def(DP, 'toString', function () { var t = time(this); if (t !== t) return 'Invalid Date'; var f = fields(t); return dateText(f) + ' ' + timeText(f); });
  def(DP, 'toDateString', function () { var t = time(this); return t !== t ? 'Invalid Date' : dateText(fields(t)); });
  def(DP, 'toTimeString', function () { var t = time(this); return t !== t ? 'Invalid Date' : timeText(fields(t)); });
  def(DP, 'toUTCString', function () {
    var t = time(this); if (t !== t) return 'Invalid Date';
    var f = fields(t);
    return DAYS[f.weekday].slice(0, 3) + ', ' + pad(f.day, 2) + ' ' + MONTHS[f.month].slice(0, 3) + ' ' + pad(f.year, 4) + ' ' + pad(f.hour, 2) + ':' + pad(f.minute, 2) + ':' + pad(f.second, 2) + ' GMT';
  });
  def(DP, 'toGMTString', DP.toUTCString);
  def(DP, '@@toPrimitive', function (hint) { return hint === 'number' ? this.valueOf() : this.toString(); });
  g.Date = Date_;

  // ------------------------------------------------------------------------------------------------------------------ Intl (English formats, with a few numeric orders for other locales)
  function localeOf(locales) {
    var l = Array.isArray(locales) ? locales[0] : locales;
    if (l !== null && typeof l === 'object' && typeof l.toString === 'function' && l.baseName !== undefined) l = String(l);
    if (typeof l === 'string' && l.length) { try { return canonicalTag(l); } catch (e) { return l; } }
    return 'en-US';
  }
  var NUMERIC = {
    'en-US': { order: 'mdy', sep: '/', hour12: true, group: ',', decimal: '.' }, 'en-GB': { order: 'dmy', sep: '/', hour12: false, pad: true, group: ',', decimal: '.' },
    'en-AU': { order: 'dmy', sep: '/', hour12: true, pad: true, group: ',', decimal: '.' }, 'en-CA': { order: 'ymd', sep: '-', hour12: false, pad: true, group: ',', decimal: '.' },
    'de-DE': { order: 'dmy', sep: '.', hour12: false, group: '.', decimal: ',' }, 'fr-FR': { order: 'dmy', sep: '/', hour12: false, pad: true, group: ' ', decimal: ',' },
    'es-ES': { order: 'dmy', sep: '/', hour12: false, group: '.', decimal: ',' }, 'it-IT': { order: 'dmy', sep: '/', hour12: false, group: '.', decimal: ',' },
    'ja-JP': { order: 'ymd', sep: '/', hour12: false, group: ',', decimal: '.' }, 'sv-SE': { order: 'ymd', sep: '-', hour12: false, pad: true, group: ' ', decimal: ',' },
    'pt-BR': { order: 'dmy', sep: '/', hour12: false, pad: true, group: '.', decimal: ',' }, 'nl-NL': { order: 'dmy', sep: '-', hour12: false, group: '.', decimal: ',' }
  };
  function numeric(locale) { return NUMERIC[locale] || NUMERIC[locale.slice(0, 2) + '-' + locale.slice(0, 2).toUpperCase()] || NUMERIC['en-US']; }
  function DateTimeFormat(locales, options) {
    if (!(this instanceof DateTimeFormat)) return new DateTimeFormat(locales, options);
    options = options || {};
    this.__locale = localeOf(locales);
    this.__options = {};
    for (var k in options) this.__options[k] = options[k];
  }
  function monthName(i, style) { var n = MONTHS[i]; return style === 'long' ? n : style === 'narrow' ? n.charAt(0) : n.slice(0, 3); }
  function weekdayName(i, style) { var n = DAYS[i]; return style === 'long' ? n : style === 'narrow' ? n.charAt(0) : n.slice(0, 3); }
  function formatDateTime(date, locale, o, defaults) {
    var t = date === undefined ? NativeDate.now() : (typeof date === 'number' ? date : (date && date.__date ? date.__t : Number(date)));
    if (t !== t) throw new RangeError('Invalid time value');
    var f = fields(t), n = numeric(locale), out = [];
    var style = o.dateStyle || o.timeStyle;
    var w = o.weekday, y = o.year, mo = o.month, d = o.day, h = o.hour, mi = o.minute, s = o.second;
    if (o.dateStyle) {
      if (o.dateStyle === 'full') { w = 'long'; y = 'numeric'; mo = 'long'; d = 'numeric'; }
      else if (o.dateStyle === 'long') { y = 'numeric'; mo = 'long'; d = 'numeric'; }
      else if (o.dateStyle === 'medium') { y = 'numeric'; mo = 'short'; d = 'numeric'; }
      else { y = '2-digit'; mo = 'numeric'; d = 'numeric'; }
    }
    if (o.timeStyle) { h = 'numeric'; mi = '2-digit'; if (o.timeStyle !== 'short') s = '2-digit'; }
    if (!style && w === undefined && y === undefined && mo === undefined && d === undefined && h === undefined && mi === undefined && s === undefined && o.fractionalSecondDigits === undefined) {
      if (defaults.date) { y = 'numeric'; mo = 'numeric'; d = 'numeric'; }
      if (defaults.time) { h = 'numeric'; mi = '2-digit'; s = '2-digit'; }
    }
    var textual = mo === 'long' || mo === 'short' || mo === 'narrow';
    var yy = y === undefined ? '' : y === '2-digit' ? pad(f.year % 100, 2) : String(f.year);
    var dd = d === undefined ? '' : d === '2-digit' || (n.pad && !textual) ? pad(f.day, 2) : String(f.day);
    var datePart = '';
    if (textual) {
      var mname = monthName(f.month, mo);
      if (n.order === 'dmy') datePart = (dd ? dd + ' ' : '') + mname + (yy ? ' ' + yy : '');
      else datePart = mname + (dd ? ' ' + dd : '') + (yy && dd ? ', ' + yy : yy ? ' ' + yy : '');
      if (w) datePart = weekdayName(f.weekday, w) + (n.order === 'dmy' ? ' ' : ', ') + datePart;
    } else {
      var mm = mo === undefined ? '' : mo === '2-digit' || n.pad ? pad(f.month + 1, 2) : String(f.month + 1);
      var seq = n.order === 'dmy' ? [dd, mm, yy] : n.order === 'ymd' ? [yy, mm, dd] : [mm, dd, yy];
      datePart = seq.filter(function (x) { return x !== ''; }).join(n.sep);
      if (w) datePart = weekdayName(f.weekday, w) + (datePart ? ', ' + datePart : '');
    }
    if (datePart) out.push(datePart);
    if (h !== undefined || mi !== undefined || s !== undefined) {
      var twelve = o.hour12 !== undefined ? !!o.hour12 : o.hourCycle ? (o.hourCycle === 'h12' || o.hourCycle === 'h11') : n.hour12;
      var hour = f.hour, suffix = '';
      if (twelve) { suffix = hour >= 12 ? ' PM' : ' AM'; hour = hour % 12; if (hour === 0) hour = 12; }
      var parts = [];
      if (h !== undefined) parts.push(h === '2-digit' || (!twelve && mi !== undefined) ? pad(hour, 2) : String(hour));
      if (mi !== undefined) parts.push(h === undefined && mi === 'numeric' && s === undefined ? String(f.minute) : pad(f.minute, 2));
      if (s !== undefined) parts.push(h === undefined && mi === undefined && s === 'numeric' ? String(f.second) : pad(f.second, 2));
      var timePart = parts.join(':');
      if (o.fractionalSecondDigits) timePart += '.' + pad(f.ms, 3).slice(0, o.fractionalSecondDigits);
      timePart += h !== undefined ? suffix : '';
      if (o.timeZoneName) timePart += ' UTC';
      if (o.timeStyle === 'full' || o.timeStyle === 'long') timePart += ' UTC';
      out.push(timePart);
    }
    return out.join(o.dateStyle === 'full' || o.dateStyle === 'long' ? (o.timeStyle ? ' at ' : ', ') : ', ');
  }
  def(DateTimeFormat.prototype, 'format', function (date) { return formatDateTime(date, this.__locale, this.__options, { date: true, time: false }); });
  def(DateTimeFormat.prototype, 'formatToParts', function (date) { return [{ type: 'literal', value: this.format(date) }]; });
  def(DateTimeFormat.prototype, 'resolvedOptions', function () { var r = { locale: this.__locale, calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }; for (var k in this.__options) r[k] = this.__options[k]; return r; });
  def(DateTimeFormat, 'supportedLocalesOf', function (l) { return Array.isArray(l) ? l.slice() : l ? [l] : []; });
  def(DP, 'toLocaleDateString', function (locales, options) { var t = time(this); return t !== t ? 'Invalid Date' : formatDateTime(this, localeOf(locales), options || {}, { date: true, time: false }); });
  def(DP, 'toLocaleTimeString', function (locales, options) { var t = time(this); return t !== t ? 'Invalid Date' : formatDateTime(this, localeOf(locales), options || {}, { date: false, time: true }); });
  def(DP, 'toLocaleString', function (locales, options) { var t = time(this); return t !== t ? 'Invalid Date' : formatDateTime(this, localeOf(locales), options || {}, { date: true, time: true }); });

  var CURRENCY = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', CNY: 'CN¥', INR: '₹', KRW: '₩', CAD: 'CA$', AUD: 'A$', MXN: 'MX$', BRL: 'R$', CHF: 'CHF ', RUB: 'RUB ' };
  function NumberFormat(locales, options) {
    if (!(this instanceof NumberFormat)) return new NumberFormat(locales, options);
    options = options || {};
    var style = options.style || 'decimal';
    var minF = options.minimumFractionDigits, maxF = options.maximumFractionDigits;
    var defMin = style === 'currency' ? (options.currency === 'JPY' || options.currency === 'KRW' ? 0 : 2) : 0, defMax = style === 'currency' ? defMin : style === 'percent' ? 0 : 3;
    if (minF === undefined && maxF === undefined) { minF = defMin; maxF = defMax; }
    else if (minF === undefined) minF = M.min(defMin, maxF);
    else if (maxF === undefined) maxF = M.max(defMax, minF);
    if (minF > maxF) throw new RangeError('maximumFractionDigits value is out of range.');
    this.__o = { locale: localeOf(locales), style: style, currency: options.currency, currencyDisplay: options.currencyDisplay || 'symbol', minF: minF, maxF: maxF, minI: options.minimumIntegerDigits || 1,
                 grouping: options.useGrouping !== false && options.useGrouping !== 'false', notation: options.notation || 'standard', maxSig: options.maximumSignificantDigits, minSig: options.minimumSignificantDigits,
                 signDisplay: options.signDisplay || 'auto', unit: options.unit, unitDisplay: options.unitDisplay || 'short', compactDisplay: options.compactDisplay || 'short' };
  }
  function group(intPart, sep) { var out = ''; for (var i = 0; i < intPart.length; i++) { if (i > 0 && (intPart.length - i) % 3 === 0) out += sep; out += intPart.charAt(i); } return out; }
  function formatNumber(o, x) {
    x = Number(x);
    var n = numeric(o.locale), suffix = '';
    if (x !== x) return 'NaN';
    var negative = x < 0 || (x === 0 && 1 / x < 0);
    x = M.abs(x);
    if (o.style === 'percent') x *= 100;
    if (x === Infinity) return (negative ? '-' : '') + '∞';
    if (o.notation === 'compact') {
      var units = o.compactDisplay === 'long' ? [[1e12, ' trillion'], [1e9, ' billion'], [1e6, ' million'], [1e3, ' thousand']] : [[1e12, 'T'], [1e9, 'B'], [1e6, 'M'], [1e3, 'K']];
      for (var u = 0; u < units.length; u++) if (x >= units[u][0]) { x /= units[u][0]; suffix = units[u][1]; break; }
    }
    var text;
    if (o.maxSig !== undefined || o.minSig !== undefined) {
      var sig = o.maxSig === undefined ? 21 : o.maxSig;
      text = x === 0 ? '0' : Number(x.toPrecision(sig)).toString();
      if (text.indexOf('e') >= 0) text = Number(text).toFixed(0);
    } else {
      var minF = o.minF, maxF = o.maxF;
      if (o.notation === 'compact' && o.minF === 0 && o.maxF === 3 && suffix !== '') { maxF = x < 100 ? 1 : 0; }
      var rounded = Number(x.toFixed(maxF));
      text = rounded.toFixed(maxF);
      if (maxF > minF && text.indexOf('.') >= 0) { var cut = text.length; while (cut > text.indexOf('.') + 1 + minF && text.charAt(cut - 1) === '0') cut--; text = text.slice(0, cut); if (text.charAt(text.length - 1) === '.') text = text.slice(0, -1); }
    }
    var dot = text.indexOf('.'), intPart = dot < 0 ? text : text.slice(0, dot), frac = dot < 0 ? '' : text.slice(dot + 1);
    while (intPart.length < o.minI) intPart = '0' + intPart;
    if (o.grouping && !(o.notation === 'compact')) intPart = group(intPart, n.group);
    var body = intPart + (frac ? n.decimal + frac : '') + suffix;
    var isZero = Number(text) === 0;
    var sign = negative && !isZero ? '-' : negative && o.signDisplay === 'negative' ? '' : '';
    if (o.signDisplay === 'always' || (o.signDisplay === 'exceptZero' && !isZero)) sign = negative ? '-' : '+';
    if (o.signDisplay === 'never') sign = '';
    if (o.style === 'percent') return sign + body + '%';
    if (o.style === 'currency') {
      var symbol = o.currencyDisplay === 'code' ? o.currency + ' ' : o.currencyDisplay === 'name' ? '' : (CURRENCY[o.currency] || o.currency + ' ');
      if (o.currencyDisplay === 'name') return sign + body + ' ' + o.currency;
      if (n.decimal === ',') return sign + body + ' ' + (CURRENCY[o.currency] ? CURRENCY[o.currency].replace(' ', '') : o.currency);
      return sign + symbol + body;
    }
    if (o.style === 'unit' && o.unit) {
      var short = { kilometer: 'km', meter: 'm', centimeter: 'cm', mile: 'mi', kilogram: 'kg', gram: 'g', liter: 'L', second: 'sec', minute: 'min', hour: 'hr', day: o.unitDisplay === 'long' ? ' days' : ' days', byte: 'byte', kilobyte: 'kB', megabyte: 'MB', gigabyte: 'GB', celsius: '°C', fahrenheit: '°F', percent: '%' };
      return sign + body + (o.unitDisplay === 'long' ? ' ' + o.unit + (Number(text) === 1 ? '' : 's') : (short[o.unit] && short[o.unit].charAt(0) !== ' ' && short[o.unit].charAt(0) !== '°' && short[o.unit] !== '%' ? ' ' : '') + (short[o.unit] || o.unit));
    }
    return sign + body;
  }
  def(NumberFormat.prototype, 'format', function (v) { return formatNumber(this.__o, v); });
  def(NumberFormat.prototype, 'formatToParts', function (v) { return [{ type: 'literal', value: formatNumber(this.__o, v) }]; });
  def(NumberFormat.prototype, 'resolvedOptions', function () { return { locale: this.__o.locale, numberingSystem: 'latn', style: this.__o.style, minimumFractionDigits: this.__o.minF, maximumFractionDigits: this.__o.maxF, useGrouping: this.__o.grouping }; });
  def(NumberFormat, 'supportedLocalesOf', DateTimeFormat.supportedLocalesOf);
  def(NP, 'toLocaleString', function (locales, options) { return formatNumber(new NumberFormat(locales, options).__o, this); });

  function PluralRules(locales, options) {
    if (!(this instanceof PluralRules)) return new PluralRules(locales, options);
    this.__ordinal = !!options && options.type === 'ordinal';
  }
  def(PluralRules.prototype, 'select', function (n) {
    n = Number(n);
    if (this.__ordinal) { var t = n % 10, h = n % 100; return t === 1 && h !== 11 ? 'one' : t === 2 && h !== 12 ? 'two' : t === 3 && h !== 13 ? 'few' : 'other'; }
    return n === 1 ? 'one' : 'other';
  });
  def(PluralRules.prototype, 'resolvedOptions', function () { return { locale: 'en-US', type: this.__ordinal ? 'ordinal' : 'cardinal' }; });
  function RelativeTimeFormat(locales, options) {
    if (!(this instanceof RelativeTimeFormat)) return new RelativeTimeFormat(locales, options);
    this.__auto = !!options && options.numeric === 'auto';
  }
  def(RelativeTimeFormat.prototype, 'format', function (value, unit) {
    value = Number(value); unit = String(unit).replace(/s$/, '');
    if (this.__auto) {
      var special = { day: { '-1': 'yesterday', '0': 'today', '1': 'tomorrow' }, year: { '-1': 'last year', '0': 'this year', '1': 'next year' }, month: { '-1': 'last month', '0': 'this month', '1': 'next month' },
                      week: { '-1': 'last week', '0': 'this week', '1': 'next week' }, hour: { '0': 'this hour' }, minute: { '0': 'this minute' }, second: { '0': 'now' } };
      if (special[unit] && special[unit][String(value)] !== undefined) return special[unit][String(value)];
    }
    var n = M.abs(value), label = n + ' ' + unit + (n === 1 ? '' : 's');
    return value < 0 || (value === 0 && 1 / value < 0) ? label + ' ago' : 'in ' + label;
  });
  function ListFormat(locales, options) {
    if (!(this instanceof ListFormat)) return new ListFormat(locales, options);
    this.__word = options && options.type === 'disjunction' ? 'or' : 'and';
    this.__unit = options && options.type === 'unit';
  }
  def(ListFormat.prototype, 'format', function (list) {
    var items = Array.from(list).map(String), n = items.length;
    if (n === 0) return '';
    if (n === 1) return items[0];
    if (this.__unit) return items.join(', ');
    if (n === 2) return items[0] + ' ' + this.__word + ' ' + items[1];
    return items.slice(0, -1).join(', ') + ', ' + this.__word + ' ' + items[n - 1];
  });
  // --- locale identifiers: parsing, canonical form, Intl.Locale
  var ALIASES = { iw: 'he', 'in': 'id', ji: 'yi', mo: 'ro', tl: 'fil', no: 'nb', sh: 'sr-Latn' };
  function title(s) { return s.charAt(0).toUpperCase() + s.slice(1).toLowerCase(); }
  function bad() { return new RangeError('Incorrect locale information provided'); }
  // { language, script, region, variants[], extensions[{singleton, subtags[]}], privateUse[] } of a tag; RangeError when it is not a well-formed tag
  function parseTag(tag) {
    if (typeof tag !== 'string') throw new TypeError('Incorrect locale information provided');
    var parts = tag.split('-'), i = 0;
    if (!parts.every(function (p) { return /^[a-z0-9]{1,8}$/i.test(p); })) throw bad();
    var out = { language: parts[i++].toLowerCase(), script: undefined, region: undefined, variants: [], extensions: [], privateUse: [] };
    if (!/^([a-z]{2,3}|[a-z]{5,8})$/.test(out.language)) throw bad();
    if (parts[i] && /^[a-z]{4}$/i.test(parts[i])) out.script = title(parts[i++]);
    if (parts[i] && /^([a-z]{2}|[0-9]{3})$/i.test(parts[i])) out.region = parts[i++].toUpperCase();
    while (parts[i] && /^([a-z0-9]{5,8}|[0-9][a-z0-9]{3})$/i.test(parts[i])) { var v = parts[i++].toLowerCase(); if (out.variants.indexOf(v) >= 0) throw bad(); out.variants.push(v); }
    var seen = {};
    while (i < parts.length) {
      var s = parts[i].toLowerCase();
      if (s.length !== 1) throw bad();
      i++;
      if (s === 'x') { if (i >= parts.length) throw bad(); while (i < parts.length) out.privateUse.push(parts[i++].toLowerCase()); break; }
      if (seen[s]) throw bad();
      seen[s] = true;
      var subtags = [];
      while (i < parts.length && parts[i].length > 1) subtags.push(parts[i++].toLowerCase());
      if (!subtags.length) throw bad();
      out.extensions.push({ singleton: s, subtags: subtags });
    }
    var alias = ALIASES[out.language];
    if (alias && !out.script && !out.region) { var ap = alias.split('-'); out.language = ap[0]; if (ap[1]) out.script = ap[1]; }
    return out;
  }
  function unicodeKeywords(ext) {   // the -u- extension as { attributes[], keywords{} } (keys sorted when written)
    var attrs = [], kw = {}, key = null;
    ext.subtags.forEach(function (t) {
      if (t.length === 2) { key = t; kw[key] = []; } else if (key) kw[key].push(t); else attrs.push(t);
    });
    return { attributes: attrs, keywords: kw };
  }
  function formatTag(p) {
    var out = [p.language];
    if (p.script) out.push(p.script);
    if (p.region) out.push(p.region);
    out = out.concat(p.variants.slice().sort());
    p.extensions.slice().sort(function (a, b) { return a.singleton < b.singleton ? -1 : 1; }).forEach(function (e) {
      out.push(e.singleton);
      if (e.singleton === 'u') {
        var u = unicodeKeywords(e);
        out = out.concat(u.attributes.slice().sort());
        Object.keys(u.keywords).sort().forEach(function (k) { out.push(k); u.keywords[k].forEach(function (v) { if (v !== 'true') out.push(v); }); });
      } else out = out.concat(e.subtags);
    });
    if (p.privateUse.length) out = out.concat(['x'], p.privateUse);
    return out.join('-');
  }
  function canonicalTag(tag) { return formatTag(parseTag(tag)); }
  function localeList(locales) {
    if (locales === undefined) return [];
    if (typeof locales === 'string' || locales instanceof Locale) locales = [locales];
    if (locales === null) throw new TypeError('Cannot convert undefined or null to object');
    var out = [];
    Array.prototype.forEach.call(Object(locales), function (l) {
      if (typeof l !== 'string' && !(l instanceof Locale)) throw new TypeError('Incorrect locale information provided');
      var c = canonicalTag(String(l));
      if (out.indexOf(c) < 0) out.push(c);
    });
    return out;
  }
  var LIKELY = { en: 'Latn-US', fr: 'Latn-FR', de: 'Latn-DE', es: 'Latn-ES', it: 'Latn-IT', pt: 'Latn-BR', nl: 'Latn-NL', sv: 'Latn-SE', da: 'Latn-DK', nb: 'Latn-NO', fi: 'Latn-FI', pl: 'Latn-PL', cs: 'Latn-CZ',
    tr: 'Latn-TR', ru: 'Cyrl-RU', uk: 'Cyrl-UA', el: 'Grek-GR', he: 'Hebr-IL', ar: 'Arab-EG', fa: 'Arab-IR', hi: 'Deva-IN', th: 'Thai-TH', ja: 'Jpan-JP', ko: 'Kore-KR', zh: 'Hans-CN', vi: 'Latn-VN',
    id: 'Latn-ID', hu: 'Latn-HU', ro: 'Latn-RO', bg: 'Cyrl-BG', hr: 'Latn-HR', sk: 'Latn-SK', sr: 'Cyrl-RS', 'zh-TW': 'Hant-TW', 'zh-HK': 'Hant-HK', 'zh-Hant': 'Hant-TW' };
  function likelyFor(p) {
    return (p.script && LIKELY[p.language + '-' + p.script]) || (p.region && LIKELY[p.language + '-' + p.region]) || LIKELY[p.language];
  }
  function Locale(tag, options) {
    if (!(this instanceof Locale)) throw new TypeError("Constructor Intl.Locale requires 'new'");
    if (typeof tag !== 'string' && !(tag instanceof Locale)) throw new TypeError('First argument to Intl.Locale constructor can\'t be empty or missing');
    var p = parseTag(String(tag));
    options = options === undefined ? {} : Object(options);
    function opt(name, test) { var v = options[name]; if (v === undefined) return undefined; v = String(v); if (test && !test.test(v)) throw new RangeError('Incorrect locale information provided'); return v; }
    var lang = opt('language', /^([a-z]{2,3}|[a-z]{5,8})$/i), script = opt('script', /^[a-z]{4}$/i), region = opt('region', /^([a-z]{2}|[0-9]{3})$/i);
    if (lang !== undefined) p.language = lang.toLowerCase();
    if (script !== undefined) p.script = title(script);
    if (region !== undefined) p.region = region.toUpperCase();
    var u = null;
    for (var k = 0; k < p.extensions.length; k++) if (p.extensions[k].singleton === 'u') u = unicodeKeywords(p.extensions[k]);
    if (!u) u = { attributes: [], keywords: {} };
    [['calendar', 'ca', /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i], ['collation', 'co', /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i], ['hourCycle', 'hc', /^(h11|h12|h23|h24)$/], ['caseFirst', 'kf', /^(upper|lower|false)$/],
     ['numberingSystem', 'nu', /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i]].forEach(function (spec) { var v = opt(spec[0], spec[2]); if (v !== undefined) u.keywords[spec[1]] = v.toLowerCase().split('-'); });
    if (options.numeric !== undefined) u.keywords.kn = Boolean(options.numeric) ? [] : ['false'];
    p.extensions = p.extensions.filter(function (e) { return e.singleton !== 'u'; });
    var sub = u.attributes.slice();
    Object.keys(u.keywords).sort().forEach(function (key) { sub.push(key); u.keywords[key].forEach(function (v) { sub.push(v); }); });
    if (sub.length) p.extensions.push({ singleton: 'u', subtags: sub });
    Object.defineProperty(this, '__p', { value: p });
    Object.defineProperty(this, '__locale', { value: formatTag(p) });
    Object.defineProperty(this, '__u', { value: u });
  }
  [['language', function (p) { return p.language; }], ['script', function (p) { return p.script; }], ['region', function (p) { return p.region; }],
   ['baseName', function (p) { return formatTag({ language: p.language, script: p.script, region: p.region, variants: p.variants, extensions: [], privateUse: [] }); }]].forEach(function (spec) {
    Object.defineProperty(Locale.prototype, spec[0], { get: function () { if (!this.__p) throw new TypeError('Illegal invocation'); return spec[1](this.__p); }, enumerable: false, configurable: true });
  });
  [['calendar', 'ca'], ['collation', 'co'], ['hourCycle', 'hc'], ['caseFirst', 'kf'], ['numberingSystem', 'nu']].forEach(function (spec) {
    Object.defineProperty(Locale.prototype, spec[0], { get: function () { var v = this.__u.keywords[spec[1]]; return v === undefined ? undefined : v.join('-'); }, enumerable: false, configurable: true });
  });
  Object.defineProperty(Locale.prototype, 'numeric', { get: function () { var v = this.__u.keywords.kn; return v !== undefined && (v.length === 0 || v[0] === 'true'); }, enumerable: false, configurable: true });
  def(Locale.prototype, 'toString', function () { return this.__locale; });
  def(Locale.prototype, 'maximize', function () {
    var p = this.__p, likely = likelyFor(p), c = { language: p.language, script: p.script, region: p.region, variants: p.variants, extensions: p.extensions, privateUse: p.privateUse };
    if (likely) { var l = likely.split('-'); if (!c.script) c.script = l[0]; if (!c.region) c.region = l[1]; }
    return new Locale(formatTag(c));
  });
  def(Locale.prototype, 'minimize', function () {
    var p = this.__p, c = { language: p.language, script: p.script, region: p.region, variants: p.variants, extensions: p.extensions, privateUse: p.privateUse }, full = this.maximize().__p;
    var likely = likelyFor({ language: p.language, script: undefined, region: undefined });
    if (likely) { var l = likely.split('-'); if (full.script === l[0]) c.script = undefined; if (full.region === l[1]) c.region = undefined; }
    return new Locale(formatTag(c));
  });
  Object.defineProperty(Locale.prototype, Symbol.toStringTag, { value: 'Intl.Locale', configurable: true });
  def(Locale, 'supportedLocalesOf', function (l) { return localeList(l); });

  // --- Collator: the order of the Unicode collation algorithm in outline: spaces, punctuation, symbols, digits, then letters by their base letter; accents and case only break ties
  var MARK = /^\p{M}$/u, LETTER = /^\p{L}$/u, DIGIT = /^\p{Nd}$/u, SPACE = /^\s$/u, PUNCT = /^\p{P}$/u;
  function charClass(ch) { return SPACE.test(ch) ? 0 : PUNCT.test(ch) ? 1 : DIGIT.test(ch) ? 3 : LETTER.test(ch) ? 4 : 2; }
  var SYMBOL_ORDER = '_-,;:!?.\'"()[]{}@*/\\&#%`^+<=>|~$';
  var SWEDISH = { 'å': 0x7B, 'ä': 0x7C, 'ö': 0x7D };   // sorted after z
  function collationElements(s, tail) {
    var out = [], d = s.normalize('NFD');
    for (var ch of d) {
      if (MARK.test(ch)) { if (out.length) out[out.length - 1].a += ch; continue; }
      var low = ch.toLowerCase(), cls = charClass(ch), el = { cls: cls, p: low.codePointAt(0), up: low !== ch ? 1 : 0, a: '' };
      if (cls === 1 || cls === 2) { var rank = SYMBOL_ORDER.indexOf(ch); el.p = rank >= 0 ? rank : 1000 + el.p; }   // (punctuation and symbols in the order of the root collation, not of the code points)
      if (cls === 3) { var last = out[out.length - 1]; if (last && last.cls === 3 && last.num !== undefined) { last.num += ch; continue; } el.num = ch; }
      out.push(el);
    }
    if (tail) {   // the Nordic letters that are not "a with a mark" there
      var comp = s.normalize('NFC'), at = 0;
      out = []; 
      for (var c2 of comp) {
        var l2 = c2.toLowerCase(), t = tail[l2];
        if (t !== undefined) { out.push({ cls: 4, p: t, up: l2 !== c2 ? 1 : 0, a: '' }); continue; }
        var parts = collationElements(c2, null);
        out = out.concat(parts);
      }
    }
    return out;
  }
  function Collator(locales, options) {
    if (!(this instanceof Collator)) return new Collator(locales, options);
    var list = localeList(locales);
    options = options === undefined ? {} : Object(options);
    var locale = list[0] || 'en-US', lang = locale.split('-')[0];
    var usage = options.usage === undefined ? 'sort' : String(options.usage);
    if (usage !== 'sort' && usage !== 'search') throw new RangeError('Value ' + usage + ' out of range for Intl.Collator options property usage');
    var sensitivity = options.sensitivity === undefined ? 'variant' : String(options.sensitivity);
    if (['base', 'accent', 'case', 'variant'].indexOf(sensitivity) < 0) throw new RangeError('Value ' + sensitivity + ' out of range for Intl.Collator options property sensitivity');
    var caseFirst = options.caseFirst === undefined ? 'false' : String(options.caseFirst);
    if (['upper', 'lower', 'false'].indexOf(caseFirst) < 0) throw new RangeError('Value ' + caseFirst + ' out of range for Intl.Collator options property caseFirst');
    var numericSort = options.numeric === undefined ? /-u-(?:[a-z0-9-]*-)?kn(?:-|$)(?!false)/.test(locale) : Boolean(options.numeric);
    var ignorePunctuation = Boolean(options.ignorePunctuation);
    var tail = lang === 'sv' || lang === 'fi' ? SWEDISH : null, cache = new Map();
    var o = { locale: list[0] || 'en-US', usage: usage, sensitivity: sensitivity, ignorePunctuation: ignorePunctuation, collation: 'default', numeric: numericSort, caseFirst: caseFirst };
    Object.defineProperty(this, '__o', { value: o });
    function elements(s) {
      var e = cache.get(s);
      if (e === undefined) { e = collationElements(s, tail); if (ignorePunctuation) e = e.filter(function (x) { return x.cls > 1; }); if (cache.size > 500) cache.clear(); cache.set(s, e); }
      return e;
    }
    var accents = sensitivity === 'accent' || sensitivity === 'variant', cases = sensitivity === 'case' || sensitivity === 'variant';
    var compare = function (x, y) {
      x = String(x); y = String(y);
      var a = elements(x), b = elements(y), n = Math.min(a.length, b.length), i;
      for (i = 0; i < n; i++) {
        if (a[i].cls !== b[i].cls) return a[i].cls < b[i].cls ? -1 : 1;
        if (numericSort && a[i].num !== undefined && b[i].num !== undefined) {
          var na = a[i].num.replace(/^0+(?=.)/, ''), nb = b[i].num.replace(/^0+(?=.)/, '');
          if (na.length !== nb.length) return na.length < nb.length ? -1 : 1;
          if (na !== nb) return na < nb ? -1 : 1;
          continue;
        }
        if (a[i].p !== b[i].p) return a[i].p < b[i].p ? -1 : 1;
      }
      if (a.length !== b.length) return a.length < b.length ? -1 : 1;
      if (accents) for (i = 0; i < n; i++) if (a[i].a !== b[i].a) return a[i].a < b[i].a ? -1 : 1;
      if (cases) for (i = 0; i < n; i++) if (a[i].up !== b[i].up) return (a[i].up < b[i].up) === (caseFirst !== 'upper') ? -1 : 1;
      return 0;
    };
    Object.defineProperty(compare, 'name', { value: '' });
    Object.defineProperty(this, 'compare', { value: compare, enumerable: false, configurable: true });
  }
  def(Collator.prototype, 'resolvedOptions', function () { var o = this.__o; return { locale: o.locale, usage: o.usage, sensitivity: o.sensitivity, ignorePunctuation: o.ignorePunctuation, collation: o.collation, numeric: o.numeric, caseFirst: o.caseFirst }; });
  def(Collator, 'supportedLocalesOf', function (l) { return localeList(l); });
  Object.defineProperty(Collator.prototype, Symbol.toStringTag, { value: 'Intl.Collator', configurable: true });
  var defaultCollator = null;
  def(SP, 'localeCompare', function (that, locales, options) {
    if (this === null || this === undefined) throw new TypeError('String.prototype.localeCompare called on null or undefined');
    var c = locales === undefined && options === undefined ? (defaultCollator || (defaultCollator = new Collator())) : new Collator(locales, options);
    return c.compare(String(this), String(that));
  });

  // --- Segmenter: graphemes, words and sentences (the rules of UAX 29 in outline)
  var EXTEND = /^[\p{M}‍️]$/u;
  function isExtender(cp) { return cp === 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) || MARK.test(String.fromCodePoint(cp)); }
  function isPictographic(cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0x231A && cp <= 0x23FF) || cp === 0xA9 || cp === 0xAE || cp === 0x203C || cp === 0x2049 || cp === 0x2122 || cp === 0x2139;
  }
  function isRegional(cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
  function graphemeSegments(s) {
    var out = [], units = Array.from(s), i = 0, index = 0;
    while (i < units.length) {
      var start = i, cp = units[i].codePointAt(0);
      i++;
      if (cp === 13 && units[i] === '\n') i++;
      else if (cp !== 10 && cp !== 13) {
        if (isRegional(cp) && i < units.length && isRegional(units[i].codePointAt(0))) i++;
        for (;;) {
          while (i < units.length && isExtender(units[i].codePointAt(0))) {
            var z = units[i].codePointAt(0);
            i++;
            if (z === 0x200D && i < units.length && isPictographic(units[i].codePointAt(0))) { i++; }
          }
          break;
        }
        if (/^[ᄀ-ᅟ]$/u.test(units[start])) while (i < units.length && /^[ᅠ-ᇿ]$/u.test(units[i])) i++;
      }
      var text = units.slice(start, i).join('');
      out.push({ segment: text, index: index });
      index += text.length;
    }
    return out;
  }
  var WORD = /^[\p{L}\p{N}_]$/u, HAN = /^[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Thai}]$/u;
  function wordSegments(s) {
    var out = [], g = graphemeSegments(s), i = 0;
    while (i < g.length) {
      var start = i, first = g[i].segment, lead = Array.from(first)[0];
      i++;
      var kind;
      if (WORD.test(lead) && !HAN.test(lead)) {
        kind = 'word';
        while (i < g.length) {
          var c = Array.from(g[i].segment)[0];
          if (WORD.test(c) && !HAN.test(c)) { i++; continue; }
          if ((c === "'" || c === '.' || c === ':' || c === String.fromCharCode(0x2019)) && i + 1 < g.length) {   // an apostrophe or a dot between letters / digits stays in the word
            var next = Array.from(g[i + 1].segment)[0];
            if (WORD.test(next) && !HAN.test(next) && (c !== '.' || (/\p{N}/u.test(next) && /\p{N}/u.test(Array.from(g[i - 1].segment)[0])) || /\p{L}/u.test(next) && c !== '.')) { i += 2; continue; }
          }
          break;
        }
      } else if (/^\s$/u.test(lead) && lead !== '\n' && lead !== '\r') {
        kind = 'space';
        while (i < g.length && /^\s$/u.test(Array.from(g[i].segment)[0]) && !/^[\n\r]/.test(g[i].segment)) i++;
      } else if (WORD.test(lead)) kind = 'word';
      else kind = 'other';
      var text = g.slice(start, i).map(function (x) { return x.segment; }).join('');
      out.push({ segment: text, index: g[start].index, isWordLike: kind === 'word' });
    }
    return out;
  }
  function sentenceSegments(s) {
    var out = [], start = 0, i = 0;
    while (i < s.length) {
      var c = s.charAt(i);
      if (c === '.' || c === '!' || c === '?' || c === String.fromCharCode(0x3002)) {
        var j = i + 1;
        while (j < s.length && /[.!?)"'\]]/.test(s.charAt(j))) j++;
        if (j >= s.length) { i = j; break; }
        if (/\s/.test(s.charAt(j))) {
          var k = j;
          while (k < s.length && /\s/.test(s.charAt(k))) k++;
          var nextCh = s.charAt(k);
          if (c !== '.' || k >= s.length || !/\p{Ll}/u.test(nextCh)) { out.push({ segment: s.slice(start, k), index: start }); start = k; i = k; continue; }
        }
        i = j;
        continue;
      }
      if (c === '\n') { var e = i + 1; out.push({ segment: s.slice(start, e), index: start }); start = e; i = e; continue; }
      i++;
    }
    if (start < s.length) out.push({ segment: s.slice(start), index: start });
    return out;
  }
  function Segmenter(locales, options) {
    if (!(this instanceof Segmenter)) throw new TypeError("Constructor Intl.Segmenter requires 'new'");
    var list = localeList(locales);
    options = options === undefined ? {} : Object(options);
    var granularity = options.granularity === undefined ? 'grapheme' : String(options.granularity);
    if (['grapheme', 'word', 'sentence'].indexOf(granularity) < 0) throw new RangeError('Value ' + granularity + ' out of range for Intl.Segmenter options property granularity');
    Object.defineProperty(this, '__o', { value: { locale: list[0] || 'en-US', granularity: granularity } });
  }
  function Segments(segmenter, input) { Object.defineProperty(this, '__s', { value: segmenter }); Object.defineProperty(this, '__input', { value: input }); }
  function segmentsOf(segments) {
    var g = segments.__s.__o.granularity, input = segments.__input;
    var list = g === 'word' ? wordSegments(input) : g === 'sentence' ? sentenceSegments(input) : graphemeSegments(input);
    return list.map(function (x) { var r = { segment: x.segment, index: x.index, input: input }; if (g === 'word') r.isWordLike = x.isWordLike; return r; });
  }
  def(Segmenter.prototype, 'segment', function (input) { if (!this.__o) throw new TypeError('Illegal invocation'); return new Segments(this, String(input)); });
  def(Segmenter.prototype, 'resolvedOptions', function () { return { locale: this.__o.locale, granularity: this.__o.granularity }; });
  def(Segmenter, 'supportedLocalesOf', function (l) { return localeList(l); });
  Object.defineProperty(Segmenter.prototype, Symbol.toStringTag, { value: 'Intl.Segmenter', configurable: true });
  def(Segments.prototype, 'containing', function (index) {
    index = Number(index); index = index !== index ? 0 : Math.trunc(index);
    var list = segmentsOf(this);
    if (index < 0 || index >= this.__input.length) return undefined;
    for (var i = 0; i < list.length; i++) if (index >= list[i].index && index < list[i].index + list[i].segment.length) return list[i];
    return undefined;
  });
  def(Segments.prototype, Symbol.iterator, function () {
    var list = segmentsOf(this), at = 0;
    var it = { next: function () { return at < list.length ? { value: list[at++], done: false } : { value: undefined, done: true }; } };
    it[Symbol.iterator] = function () { return this; };
    Object.defineProperty(it, Symbol.toStringTag, { value: 'Segmenter String Iterator', configurable: true });
    return it;
  });

  // --- DisplayNames (English names; other locales get the English ones)
  var REGIONS = { US: 'United States', GB: 'United Kingdom', CA: 'Canada', AU: 'Australia', NZ: 'New Zealand', IE: 'Ireland', DE: 'Germany', FR: 'France', ES: 'Spain', IT: 'Italy', PT: 'Portugal', NL: 'Netherlands', BE: 'Belgium', CH: 'Switzerland',
    AT: 'Austria', SE: 'Sweden', NO: 'Norway', DK: 'Denmark', FI: 'Finland', IS: 'Iceland', PL: 'Poland', CZ: 'Czechia', SK: 'Slovakia', HU: 'Hungary', RO: 'Romania', BG: 'Bulgaria', GR: 'Greece', TR: 'Türkiye', RU: 'Russia', UA: 'Ukraine',
    BY: 'Belarus', RS: 'Serbia', HR: 'Croatia', SI: 'Slovenia', BA: 'Bosnia & Herzegovina', LT: 'Lithuania', LV: 'Latvia', EE: 'Estonia', CN: 'China', JP: 'Japan', KR: 'South Korea', KP: 'North Korea', TW: 'Taiwan', HK: 'Hong Kong SAR China',
    IN: 'India', PK: 'Pakistan', BD: 'Bangladesh', LK: 'Sri Lanka', NP: 'Nepal', TH: 'Thailand', VN: 'Vietnam', ID: 'Indonesia', MY: 'Malaysia', SG: 'Singapore', PH: 'Philippines', IL: 'Israel', SA: 'Saudi Arabia', AE: 'United Arab Emirates',
    EG: 'Egypt', IR: 'Iran', IQ: 'Iraq', JO: 'Jordan', LB: 'Lebanon', QA: 'Qatar', KW: 'Kuwait', MA: 'Morocco', DZ: 'Algeria', TN: 'Tunisia', NG: 'Nigeria', KE: 'Kenya', ET: 'Ethiopia', GH: 'Ghana', ZA: 'South Africa', TZ: 'Tanzania',
    UG: 'Uganda', MX: 'Mexico', BR: 'Brazil', AR: 'Argentina', CL: 'Chile', CO: 'Colombia', PE: 'Peru', VE: 'Venezuela', EC: 'Ecuador', UY: 'Uruguay', PY: 'Paraguay', BO: 'Bolivia', CU: 'Cuba', DO: 'Dominican Republic', JM: 'Jamaica',
    PR: 'Puerto Rico', CR: 'Costa Rica', PA: 'Panama', GT: 'Guatemala', KZ: 'Kazakhstan', UZ: 'Uzbekistan', AF: 'Afghanistan', MM: 'Myanmar (Burma)', KH: 'Cambodia', LA: 'Laos', MN: 'Mongolia', LU: 'Luxembourg', MT: 'Malta', CY: 'Cyprus',
    ZZ: 'Unknown Region', EU: 'European Union', UN: 'United Nations', '001': 'world', '419': 'Latin America', '150': 'Europe' };
  var LANGUAGES = { en: 'English', fr: 'French', de: 'German', es: 'Spanish', it: 'Italian', pt: 'Portuguese', nl: 'Dutch', sv: 'Swedish', da: 'Danish', nb: 'Norwegian Bokmål', no: 'Norwegian', nn: 'Norwegian Nynorsk', fi: 'Finnish',
    is: 'Icelandic', pl: 'Polish', cs: 'Czech', sk: 'Slovak', hu: 'Hungarian', ro: 'Romanian', bg: 'Bulgarian', el: 'Greek', tr: 'Turkish', ru: 'Russian', uk: 'Ukrainian', be: 'Belarusian', sr: 'Serbian', hr: 'Croatian', sl: 'Slovenian',
    lt: 'Lithuanian', lv: 'Latvian', et: 'Estonian', zh: 'Chinese', ja: 'Japanese', ko: 'Korean', hi: 'Hindi', bn: 'Bangla', ur: 'Urdu', ta: 'Tamil', te: 'Telugu', mr: 'Marathi', th: 'Thai', vi: 'Vietnamese', id: 'Indonesian', ms: 'Malay',
    fil: 'Filipino', he: 'Hebrew', ar: 'Arabic', fa: 'Persian', sw: 'Swahili', af: 'Afrikaans', zu: 'Zulu', ca: 'Catalan', eu: 'Basque', gl: 'Galician', cy: 'Welsh', ga: 'Irish', la: 'Latin', eo: 'Esperanto', mn: 'Mongolian', ne: 'Nepali', yi: 'Yiddish' };
  var SCRIPTS = { Latn: 'Latin', Cyrl: 'Cyrillic', Grek: 'Greek', Arab: 'Arabic', Hebr: 'Hebrew', Deva: 'Devanagari', Hans: 'Simplified Han', Hant: 'Traditional Han', Hani: 'Han', Hira: 'Hiragana', Kana: 'Katakana', Jpan: 'Japanese',
    Kore: 'Korean', Hang: 'Hangul', Thai: 'Thai', Beng: 'Bangla', Taml: 'Tamil', Telu: 'Telugu', Armn: 'Armenian', Geor: 'Georgian', Ethi: 'Ethiopic', Khmr: 'Khmer', Mong: 'Mongolian', Tibt: 'Tibetan', Zyyy: 'Common', Zzzz: 'Unknown Script' };
  var CURRENCIES = { USD: 'US Dollar', EUR: 'Euro', GBP: 'British Pound', JPY: 'Japanese Yen', CNY: 'Chinese Yuan', INR: 'Indian Rupee', CAD: 'Canadian Dollar', AUD: 'Australian Dollar', CHF: 'Swiss Franc', SEK: 'Swedish Krona',
    NOK: 'Norwegian Krone', DKK: 'Danish Krone', PLN: 'Polish Zloty', CZK: 'Czech Koruna', HUF: 'Hungarian Forint', RUB: 'Russian Ruble', UAH: 'Ukrainian Hryvnia', TRY: 'Turkish Lira', BRL: 'Brazilian Real', MXN: 'Mexican Peso',
    ARS: 'Argentine Peso', CLP: 'Chilean Peso', COP: 'Colombian Peso', KRW: 'South Korean Won', HKD: 'Hong Kong Dollar', SGD: 'Singapore Dollar', NZD: 'New Zealand Dollar', ZAR: 'South African Rand', ILS: 'Israeli New Shekel',
    AED: 'United Arab Emirates Dirham', SAR: 'Saudi Riyal', EGP: 'Egyptian Pound', THB: 'Thai Baht', IDR: 'Indonesian Rupiah', MYR: 'Malaysian Ringgit', PHP: 'Philippine Peso', VND: 'Vietnamese Dong', BTC: 'BTC' };
  var CALENDARS = { gregory: 'Gregorian Calendar', buddhist: 'Buddhist Calendar', chinese: 'Chinese Calendar', hebrew: 'Hebrew Calendar', islamic: 'Islamic Calendar', japanese: 'Japanese Calendar', persian: 'Persian Calendar', iso8601: 'ISO-8601 Calendar' };
  var FIELDS = { era: 'era', year: 'year', quarter: 'quarter', month: 'month', weekOfYear: 'week', weekday: 'day of the week', day: 'day', dayPeriod: 'AM/PM', hour: 'hour', minute: 'minute', second: 'second', timeZoneName: 'time zone' };
  function DisplayNames(locales, options) {
    if (!(this instanceof DisplayNames)) throw new TypeError("Constructor Intl.DisplayNames requires 'new'");
    var list = localeList(locales);
    if (options === undefined) throw new TypeError('Required option "type" is missing');
    options = Object(options);
    var type = options.type === undefined ? undefined : String(options.type);
    if (type === undefined) throw new TypeError('Required option "type" is missing');
    if (['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'].indexOf(type) < 0) throw new RangeError('Value ' + type + ' out of range for Intl.DisplayNames options property type');
    var style = options.style === undefined ? 'long' : String(options.style), fallback = options.fallback === undefined ? 'code' : String(options.fallback), dialect = options.languageDisplay === undefined ? 'dialect' : String(options.languageDisplay);
    if (['narrow', 'short', 'long'].indexOf(style) < 0) throw new RangeError('Value ' + style + ' out of range for Intl.DisplayNames options property style');
    if (['code', 'none'].indexOf(fallback) < 0) throw new RangeError('Value ' + fallback + ' out of range for Intl.DisplayNames options property fallback');
    if (['dialect', 'standard'].indexOf(dialect) < 0) throw new RangeError('Value ' + dialect + ' out of range for Intl.DisplayNames options property languageDisplay');
    Object.defineProperty(this, '__o', { value: { locale: list[0] || 'en-US', style: style, type: type, fallback: fallback, languageDisplay: dialect } });
  }
  var DIALECTS = { 'en-US': 'American English', 'en-GB': 'British English', 'en-AU': 'Australian English', 'en-CA': 'Canadian English', 'fr-CA': 'Canadian French', 'fr-CH': 'Swiss French', 'es-419': 'Latin American Spanish',
    'es-ES': 'European Spanish', 'es-MX': 'Mexican Spanish', 'pt-BR': 'Brazilian Portuguese', 'pt-PT': 'European Portuguese', 'de-AT': 'Austrian German', 'de-CH': 'Swiss High German', 'nl-BE': 'Flemish', 'zh-Hans': 'Simplified Chinese', 'zh-Hant': 'Traditional Chinese' };
  def(DisplayNames.prototype, 'of', function (code) {
    var o = this.__o;
    if (!o) throw new TypeError('Illegal invocation');
    code = String(code);
    var result;
    switch (o.type) {
      case 'region':
        if (!/^([a-z]{2}|[0-9]{3})$/i.test(code)) throw new RangeError('Invalid argument: ' + code);
        code = code.toUpperCase(); result = REGIONS[code]; break;
      case 'script':
        if (!/^[a-z]{4}$/i.test(code)) throw new RangeError('Invalid argument: ' + code);
        code = title(code); result = SCRIPTS[code]; break;
      case 'currency':
        if (!/^[a-z]{3}$/i.test(code)) throw new RangeError('Invalid argument: ' + code);
        code = code.toUpperCase(); result = CURRENCIES[code]; break;
      case 'calendar':
        if (!/^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i.test(code)) throw new RangeError('Invalid argument: ' + code);
        code = code.toLowerCase(); result = CALENDARS[code]; break;
      case 'dateTimeField':
        if (!Object.prototype.hasOwnProperty.call(FIELDS, code)) throw new RangeError('Invalid argument: ' + code);
        result = FIELDS[code]; break;
      default: {   // language: language[-script][-region]
        var p = parseTag(code);
        code = canonicalTag(code);
        var dialectName = o.languageDisplay === 'dialect' ? DIALECTS[code] || DIALECTS[p.language + '-' + p.region] : undefined;
        if (dialectName && !p.script !== !!DIALECTS[code]) { }
        if (dialectName) { result = dialectName; break; }
        var base = LANGUAGES[p.language];
        if (base) {
          var extra = [];
          if (p.script && SCRIPTS[p.script]) extra.push(SCRIPTS[p.script]);
          if (p.region && REGIONS[p.region]) extra.push(REGIONS[p.region]);
          result = extra.length ? base + ' (' + extra.join(', ') + ')' : base;
        }
      }
    }
    if (result !== undefined) return result;
    return o.fallback === 'code' ? code : undefined;
  });
  def(DisplayNames.prototype, 'resolvedOptions', function () { var o = this.__o; var r = { locale: o.locale, style: o.style, type: o.type, fallback: o.fallback }; if (o.type === 'language') r.languageDisplay = o.languageDisplay; return r; });
  def(DisplayNames, 'supportedLocalesOf', function (l) { return localeList(l); });
  Object.defineProperty(DisplayNames.prototype, Symbol.toStringTag, { value: 'Intl.DisplayNames', configurable: true });
  var Intl = {};
  def(Intl, 'DateTimeFormat', DateTimeFormat); def(Intl, 'NumberFormat', NumberFormat); def(Intl, 'PluralRules', PluralRules); def(Intl, 'Collator', Collator);
  def(Intl, 'RelativeTimeFormat', RelativeTimeFormat); def(Intl, 'ListFormat', ListFormat); def(Intl, 'Locale', Locale); def(Intl, 'Segmenter', Segmenter); def(Intl, 'DisplayNames', DisplayNames);
  Object.defineProperty(Intl, Symbol.toStringTag, { value: 'Intl', configurable: true });
  def(Intl, 'getCanonicalLocales', function (l) { return localeList(l); });
  def(Intl, 'supportedValuesOf', function () { return []; });
  def(g, 'Intl', Intl);

  // ------------------------------------------------------------------------------------------------------------------ async generators and for await
  // The body of an async generator is a plain generator in which `await x` was made `yield __aw(x)`: the driver waits for what is awaited and goes on, and
  // hands out what is yielded to the callers of next() (one at a time, in order).
  function AwaitMarker(value) { this.value = value; }
  def(g, '__aw', function (value) { return new AwaitMarker(value); });
  function AsyncGenerator(inner) {
    Object.defineProperty(this, '__inner', { value: inner });
    Object.defineProperty(this, '__queue', { value: [] });
    Object.defineProperty(this, '__state', { value: { running: false, done: false } });
  }
  function settle(gen, request, ok, value) {
    gen.__queue.shift();
    gen.__state.running = false;
    if (ok) request.resolve(value); else request.reject(value);
    drain(gen);
  }
  function step(gen, request, mode, input) {
    var r;
    try { r = mode === 0 ? gen.__inner.next(input) : mode === 1 ? gen.__inner.throw(input) : gen.__inner.return(input); }
    catch (e) { gen.__state.done = true; settle(gen, request, false, e); return; }
    if (r.done) {
      gen.__state.done = true;
      Promise.resolve(r.value).then(function (v) { settle(gen, request, true, { value: v, done: true }); }, function (e) { settle(gen, request, false, e); });
    } else if (r.value instanceof AwaitMarker) {
      Promise.resolve(r.value.value).then(function (v) { step(gen, request, 0, v); }, function (e) { step(gen, request, 1, e); });
    } else {
      Promise.resolve(r.value).then(function (v) { settle(gen, request, true, { value: v, done: false }); }, function (e) { step(gen, request, 1, e); });
    }
  }
  function drain(gen) {
    if (gen.__state.running || gen.__queue.length === 0) return;
    var request = gen.__queue[0];
    gen.__state.running = true;
    if (gen.__state.done) {
      if (request.mode === 1) settle(gen, request, false, request.value);
      else settle(gen, request, true, { value: request.mode === 2 ? request.value : undefined, done: true });
      return;
    }
    step(gen, request, request.mode, request.value);
  }
  function request(gen, mode, value) {
    return new Promise(function (resolve, reject) { gen.__queue.push({ mode: mode, value: value, resolve: resolve, reject: reject }); drain(gen); });
  }
  def(AsyncGenerator.prototype, 'next', function (v) { return request(this, 0, v); });
  def(AsyncGenerator.prototype, 'throw', function (e) { return request(this, 1, e); });
  def(AsyncGenerator.prototype, 'return', function (v) { return request(this, 2, v); });
  def(AsyncGenerator.prototype, '@@asyncIterator', function () { return this; });
  def(AsyncGenerator.prototype, '@@toStringTag', 'AsyncGenerator');
  def(g, '__async_gen', function (inner) { return new AsyncGenerator(inner); });
  // ------------------------------------------------------------------------------------------------------------------ BigInt
  // A BigInt is an object of its own kind with a sign and its decimal digits (hidden); the operators of the interpreter call __bigint_op for it. Here the
  // digits are worked on as arrays of base 10^7 limbs, least significant first.
  var LB = 10000000, LW = 7;
  function BigInt(value) {
    if (this instanceof BigInt && new.target !== undefined) throw new TypeError('BigInt is not a constructor');
    return toBigInt(value, true);
  }
  var BP = {};
  BigInt.prototype = BP;
  def(g, '__bigint_proto', BP);
  def(BP, 'constructor', BigInt);
  def(g, 'BigInt', BigInt);
  function isBig(v) { return typeof v === 'bigint'; }
  function make(negative, limbs) { return __bigint_new(negative, fromLimbs(limbs)); }
  function negOf(b) { return b.__neg; }
  function limbsOf(b) { return toLimbs(b.__mag); }
  function toLimbs(digits) {
    var out = [];
    for (var end = digits.length; end > 0; end -= LW) out.push(Number(digits.slice(M.max(0, end - LW), end)));
    return out.length ? out : [0];
  }
  function trim(a) { while (a.length > 1 && a[a.length - 1] === 0) a.pop(); return a; }
  function fromLimbs(a) {
    a = trim(a);
    var out = String(a[a.length - 1]);
    for (var i = a.length - 2; i >= 0; i--) { var piece = String(a[i]); out += '0000000'.slice(piece.length) + piece; }
    return out;
  }
  function isZero(a) { return a.length === 1 && a[0] === 0; }
  function cmpMag(a, b) {
    if (a.length !== b.length) return a.length < b.length ? -1 : 1;
    for (var i = a.length - 1; i >= 0; i--) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
  }
  function addMag(a, b) {
    var out = [], carry = 0, n = M.max(a.length, b.length);
    for (var i = 0; i < n || carry; i++) { var v = (a[i] || 0) + (b[i] || 0) + carry; carry = v >= LB ? 1 : 0; out.push(v - carry * LB); }
    return out;
  }
  function subMag(a, b) {   // a >= b
    var out = [], borrow = 0;
    for (var i = 0; i < a.length; i++) { var v = a[i] - (b[i] || 0) - borrow; borrow = v < 0 ? 1 : 0; out.push(v + borrow * LB); }
    return trim(out);
  }
  function mulSmall(a, m) {   // m < 10^7
    var out = [], carry = 0;
    for (var i = 0; i < a.length || carry; i++) { var v = (a[i] || 0) * m + carry; carry = M.floor(v / LB); out.push(v - carry * LB); }
    return trim(out);
  }
  function mulMag(a, b) {
    if (isZero(a) || isZero(b)) return [0];
    var out = [];
    for (var i = 0; i < a.length + b.length; i++) out.push(0);
    for (var x = 0; x < a.length; x++) {
      var carry = 0;
      for (var y = 0; y < b.length || carry; y++) {
        var cur = out[x + y] + a[x] * (b[y] || 0) + carry;
        carry = M.floor(cur / LB);
        out[x + y] = cur - carry * LB;
      }
    }
    return trim(out);
  }
  function divSmall(a, d) {   // d < 10^7: [quotient, remainder]
    var out = new Array(a.length), rest = 0;
    for (var i = a.length - 1; i >= 0; i--) { var cur = rest * LB + a[i]; out[i] = M.floor(cur / d); rest = cur - out[i] * d; }
    return [trim(out), rest];
  }
  function divmodMag(a, b) {   // [quotient, remainder] of magnitudes
    if (isZero(b)) throw new RangeError('Division by zero');
    if (cmpMag(a, b) < 0) return [[0], a.slice()];
    if (b.length === 1) { var r = divSmall(a, b[0]); return [r[0], [r[1]]]; }
    var quotient = new Array(a.length), rest = [0];
    for (var i = a.length - 1; i >= 0; i--) {
      rest = trim([a[i]].concat(rest));   // rest * LB + a[i]
      var lo = 0, hi = LB - 1;
      while (lo < hi) { var mid = M.ceil((lo + hi) / 2); if (cmpMag(mulSmall(b, mid), rest) <= 0) lo = mid; else hi = mid - 1; }
      quotient[i] = lo;
      if (lo) rest = subMag(rest, mulSmall(b, lo));
    }
    return [trim(quotient), rest];
  }
  function powMag(base, exponent) {
    var result = [1];
    while (exponent > 0) { if (exponent % 2 === 1) result = mulMag(result, base); exponent = M.floor(exponent / 2); if (exponent > 0) base = mulMag(base, base); }
    return result;
  }
  function toBigInt(v, explicit) {
    if (isBig(v)) return v;
    if (typeof v === 'number') {
      if (!isFinite(v) || M.floor(v) !== v) throw new RangeError('The number ' + v + ' cannot be converted to a BigInt because it is not an integer');
      var text = M.abs(v) < 1e21 ? M.abs(v).toFixed(0) : (function (x) { var s = String(x), e = s.indexOf('e'), mant = s.slice(0, e).replace('.', ''), shift = Number(s.slice(e + 1)) - (s.slice(0, e).length - 1 - (s.indexOf('.') < 0 ? 0 : 0)); var dot = s.slice(0, e).indexOf('.'); var frac = dot < 0 ? 0 : e - dot - 1; return mant + '0'.repeat(M.max(0, Number(s.slice(e + 1)) - frac)); }(M.abs(v)));
      return __bigint_new(v < 0, text);
    }
    if (typeof v === 'boolean') return __bigint_new(false, v ? '1' : '0');
    if (typeof v === 'string') {
      var t = v.trim();
      if (t === '') return __bigint_new(false, '0');
      var neg = false, radix = 10;
      if (t.charAt(0) === '-' || t.charAt(0) === '+') { neg = t.charAt(0) === '-'; t = t.slice(1); if (t.slice(0, 2).toLowerCase() === '0x' || t.slice(0, 2).toLowerCase() === '0b' || t.slice(0, 2).toLowerCase() === '0o') throw new SyntaxError('Cannot convert ' + v + ' to a BigInt'); }
      var prefix = t.slice(0, 2).toLowerCase();
      if (prefix === '0x') { radix = 16; t = t.slice(2); } else if (prefix === '0o') { radix = 8; t = t.slice(2); } else if (prefix === '0b') { radix = 2; t = t.slice(2); }
      if (t === '' || !(radix === 10 ? /^\d+$/ : radix === 16 ? /^[0-9a-f]+$/i : radix === 8 ? /^[0-7]+$/ : /^[01]+$/).test(t)) throw new SyntaxError('Cannot convert ' + v + ' to a BigInt');
      if (radix === 10) return __bigint_new(neg, t);
      var acc = [0];
      for (var i = 0; i < t.length; i++) acc = addMag(mulSmall(acc, radix), [parseInt(t.charAt(i), radix)]);
      return make(neg, trim(acc));
    }
    if (v === null || v === undefined) throw new TypeError('Cannot convert ' + v + ' to a BigInt');
    return toBigInt(typeof v === 'object' ? v.valueOf() : v, explicit);
  }
  function signed(negative, limbs) { return make(negative && !isZero(limbs), limbs); }
  function addBig(a, b) {
    var an = negOf(a), bn = negOf(b), x = limbsOf(a), y = limbsOf(b);
    if (an === bn) return signed(an, addMag(x, y));
    var c = cmpMag(x, y);
    if (c === 0) return make(false, [0]);
    return c > 0 ? signed(an, subMag(x, y)) : signed(bn, subMag(y, x));
  }
  function negate(a) { return signed(!negOf(a), limbsOf(a)); }
  // two's complement words of 24 bits for the bitwise operators
  function toWords(limbs) {
    var words = [], cur = limbs;
    while (!isZero(cur)) { var r = divSmall(cur, 16777216); words.push(r[1]); cur = r[0]; }
    return words;
  }
  function fromWords(words) {
    var acc = [0];
    for (var i = words.length - 1; i >= 0; i--) acc = addMag(mulSmall(mulSmall(acc, 4096), 4096), [words[i] % LB === words[i] ? words[i] : 0]).length ? addWords(acc, words[i]) : acc;
    return acc;
  }
  function addWords(acc, w) { var shifted = mulSmall(mulSmall(acc, 4096), 4096); var lo = w % LB, hi = M.floor(w / LB); return addMag(shifted, hi ? addMag([lo], [0, hi]) : [lo]); }
  function wordsOf(b, count) {
    var w = toWords(limbsOf(b));
    while (w.length < count) w.push(0);
    if (negOf(b)) {   // two's complement
      var carry = 1;
      for (var i = 0; i < w.length; i++) { var v = (16777215 - w[i]) + carry; carry = v >= 16777216 ? 1 : 0; w[i] = v - carry * 16777216; }
    }
    return w;
  }
  function bitwise(op, a, b) {
    var n = M.max(toWords(limbsOf(a)).length, toWords(limbsOf(b)).length) + 1;
    var x = wordsOf(a, n), y = wordsOf(b, n), out = [];
    for (var i = 0; i < n; i++) out.push(op === '&' ? (x[i] & y[i]) : op === '|' ? (x[i] | y[i]) : (x[i] ^ y[i]));
    var negative = (out[n - 1] & 8388608) !== 0;
    if (negative) {
      var carry = 1;
      for (var k = 0; k < n; k++) { var v = (16777215 - out[k]) + carry; carry = v >= 16777216 ? 1 : 0; out[k] = v - carry * 16777216; }
    }
    return signed(negative, fromWords(out));
  }
  function shiftLeft(a, count) {   // count: a JS number, may be negative
    if (count >= 0) return signed(negOf(a), mulMag(limbsOf(a), powMag([2], count)));
    var divisor = powMag([2], -count), qr = divmodMag(limbsOf(a), divisor);
    var q = qr[0];
    if (negOf(a) && !isZero(qr[1])) q = addMag(q, [1]);   // floor
    return signed(negOf(a), q);
  }
  function compare(a, b) {   // -1, 0, 1, or NaN; a BigInt against a BigInt, a number or a string
    if (isBig(a) && isBig(b)) {
      if (negOf(a) !== negOf(b)) return negOf(a) ? -1 : 1;
      var c = cmpMag(limbsOf(a), limbsOf(b));
      return negOf(a) ? -c : c;
    }
    var flip = !isBig(a);
    var big = flip ? b : a, other = flip ? a : b;
    if (typeof other === 'string') { try { other = toBigInt(other); } catch (e) { return NaN; } return flip ? compare(other, big) : compare(big, other); }
    if (typeof other === 'object' && other !== null) other = Number(other);
    if (typeof other === 'boolean') other = other ? 1 : 0;
    if (typeof other !== 'number' || other !== other) return NaN;
    if (other === Infinity) return flip ? 1 : -1;
    if (other === -Infinity) return flip ? -1 : 1;
    var whole = M.floor(other), r = compare(big, toBigInt(whole));
    if (r === 0 && other !== whole) r = -1;   // big equals floor(other) < other
    return flip ? -r : r;
  }
  function mixError() { return new TypeError('Cannot mix BigInt and other types, use explicit conversions'); }
  def(g, '__bigint_op', function (op, a, b) {
    switch (op) {
      case 'neg': return negate(a);
      case '~': return addBig(negate(a), toBigInt(-1));
      case 'inc': return addBig(a, toBigInt(1));
      case 'dec': return addBig(a, toBigInt(-1));
      case '==': { var c = compare(a, b); return c === 0; }
      case '!=': return compare(a, b) !== 0;
      case '===': return isBig(a) && isBig(b) && compare(a, b) === 0;
      case '!==': return !(isBig(a) && isBig(b) && compare(a, b) === 0);
      case '<': return compare(a, b) < 0;
      case '>': return compare(a, b) > 0;
      case '<=': { var le = compare(a, b); return le <= 0; }
      case '>=': { var ge = compare(a, b); return ge >= 0; }
    }
    if (op === '+' && (typeof a === 'string' || typeof b === 'string')) return String(a) + String(b);
    if (op === '+' && ((typeof a === 'object' && a !== null && !isBig(a)) || (typeof b === 'object' && b !== null && !isBig(b)))) {
      var pa = isBig(a) ? a : (typeof a === 'object' && a !== null ? a.valueOf() : a), pb = isBig(b) ? b : (typeof b === 'object' && b !== null ? b.valueOf() : b);
      if (typeof pa === 'string' || typeof pb === 'string') return String(pa) + String(pb);
      a = pa; b = pb;
    }
    if (!isBig(a) || !isBig(b)) throw mixError();
    switch (op) {
      case '+': return addBig(a, b);
      case '-': return addBig(a, negate(b));
      case '*': return signed(negOf(a) !== negOf(b), mulMag(limbsOf(a), limbsOf(b)));
      case '/': { var q = divmodMag(limbsOf(a), limbsOf(b))[0]; return signed(negOf(a) !== negOf(b), q); }
      case '%': { var r = divmodMag(limbsOf(a), limbsOf(b))[1]; return signed(negOf(a), r); }
      case '**': {
        if (negOf(b)) throw new RangeError('Exponent must be non-negative');
        var e = Number(b.__mag);
        if (e > 1000000) throw new RangeError('Maximum BigInt size exceeded');
        return signed(negOf(a) && e % 2 === 1, powMag(limbsOf(a), e));
      }
      case '&': case '|': case '^': return bitwise(op, a, b);
      case '<<': return shiftLeft(a, negOf(b) ? -Number(b.__mag) : Number(b.__mag));
      case '>>': return shiftLeft(a, negOf(b) ? Number(b.__mag) : -Number(b.__mag));
      case '>>>': throw new TypeError('BigInts have no unsigned right shift, use >> instead');
    }
    throw new TypeError('Unsupported BigInt operation ' + op);
  });
  def(BP, 'toString', function (radix) {
    var self = __primitive_of(this);
    if (!isBig(self)) throw new TypeError('BigInt.prototype.toString requires that \'this\' be a BigInt');
    radix = radix === undefined ? 10 : Number(radix);
    if (radix < 2 || radix > 36) throw new RangeError('toString() radix must be between 2 and 36');
    var text = self.__mag;
    if (radix !== 10) {
      var limbs = limbsOf(self), digits = '';
      if (isZero(limbs)) digits = '0';
      while (!isZero(limbs)) { var r = divSmall(limbs, radix); digits = r[1].toString(36) + digits; limbs = r[0]; }
      text = digits;
    }
    return (negOf(self) ? '-' : '') + text;
  });
  def(BP, 'toLocaleString', function () { var digits = this.__mag, out = ''; for (var i = 0; i < digits.length; i++) { if (i > 0 && (digits.length - i) % 3 === 0) out += ','; out += digits.charAt(i); } return (negOf(this) ? '-' : '') + out; });
  def(BP, 'valueOf', function () { return __primitive_of(this); });
  def(BP, 'toJSON', function () { throw new TypeError('Do not know how to serialize a BigInt'); });
  def(BP, '@@toStringTag', 'BigInt');
  def(BigInt, 'asUintN', function (bits, value) {
    var v = toBigInt(value), modulus = signed(false, powMag([2], Number(bits)));
    var r = __bigint_op('%', v, modulus);
    return negOf(r) ? addBig(r, modulus) : r;
  });
  def(BigInt, 'asIntN', function (bits, value) {
    var u = BigInt.asUintN(bits, value), half = signed(false, powMag([2], Number(bits) - 1));
    return compare(u, half) >= 0 ? addBig(u, negate(signed(false, powMag([2], Number(bits))))) : u;
  });

  // ------------------------------------------------------------------------------------------------------------------ messages between workers and pages
  // A value as text (JSON, with markers for what JSON cannot say) and back: the structured clone of postMessage for plain data, dates, maps, sets, typed arrays,
  // regular expressions and errors.
  function dataCloneError(message) { return typeof DOMException === 'function' ? new DOMException(message, 'DataCloneError') : new Error(message); }
  def(g, '__wire_encode', function (value) {
    var seen = [];
    function conv(v) {
      if (v === undefined) return { $u: 1 };
      if (typeof v === 'number') return v !== v ? { $n: 'NaN' } : v === Infinity ? { $n: 'Infinity' } : v === -Infinity ? { $n: '-Infinity' } : (v === 0 && 1 / v < 0 ? { $n: '-0' } : v);
      if (typeof v === 'function') throw dataCloneError(String(v).slice(0, 40) + ' could not be cloned.');
      if (v === null || typeof v !== 'object') return v;
      if (seen.indexOf(v) >= 0) throw dataCloneError('The object could not be cloned (it refers to itself).');
      seen.push(v);
      var out;
      if (v instanceof Date) out = { $d: v.getTime() };
      else if (v instanceof ArrayBuffer) out = { $a: Array.prototype.slice.call(new Uint8Array(v)) };
      else if (ArrayBuffer.isView(v) && !(v instanceof DataView)) out = { $t: [Object.getPrototypeOf(v).constructor.name, Array.prototype.slice.call(v).map(conv)] };
      else if (v instanceof Map) out = { $m: Array.from(v).map(function (e) { return [conv(e[0]), conv(e[1])]; }) };
      else if (v instanceof Set) out = { $s: Array.from(v).map(conv) };
      else if (v instanceof RegExp) out = { $r: [v.source, v.flags] };
      else if (v instanceof Error) out = { $e: [v.name, v.message] };
      else if (Array.isArray(v)) { out = []; for (var i = 0; i < v.length; i++) out.push(conv(v[i])); }
      else {
        out = {};
        var keys = Object.keys(v), escaped = false;
        for (var k = 0; k < keys.length; k++) { out[keys[k]] = conv(v[keys[k]]); if (keys[k].charAt(0) === '$') escaped = true; }
        if (escaped) out = { $o: out };
      }
      seen.pop();
      return out;
    }
    return JSON.stringify(conv(value));
  });
  def(g, '__wire_decode', function (text) {
    function restore(v) {
      if (v === null || typeof v !== 'object') return v;
      if (Array.isArray(v)) return v.map(restore);
      if (v.$u !== undefined) return undefined;
      if (v.$n !== undefined) return v.$n === 'NaN' ? NaN : v.$n === 'Infinity' ? Infinity : v.$n === '-Infinity' ? -Infinity : -0;
      if (v.$d !== undefined) return new Date(v.$d);
      if (v.$a !== undefined) return new Uint8Array(v.$a).buffer;
      if (v.$t !== undefined) return new g[v.$t[0]](v.$t[1].map(restore));
      if (v.$m !== undefined) return new Map(v.$m.map(function (e) { return [restore(e[0]), restore(e[1])]; }));
      if (v.$s !== undefined) return new Set(v.$s.map(restore));
      if (v.$r !== undefined) return new RegExp(v.$r[0], v.$r[1]);
      if (v.$e !== undefined) { var C = g[v.$e[0]] || Error; return new C(v.$e[1]); }
      var source = v.$o !== undefined ? v.$o : v, out = {};
      Object.keys(source).forEach(function (k) { out[k] = restore(source[k]); });
      return out;
    }
    return restore(JSON.parse(text));
  });
  def(g, 'structuredClone', function (v) { return g.__wire_decode(g.__wire_encode(v)); });

  def(g, '__async_iter', function (iterable) {
    var method = iterable === null || iterable === undefined ? undefined : iterable['@@asyncIterator'];
    if (typeof method === 'function') return method.call(iterable);
    var it;
    if (iterable !== null && iterable !== undefined && typeof iterable[Symbol.iterator] === 'function') it = iterable[Symbol.iterator]();
    else if (Array.isArray(iterable)) it = iterable.values();
    else if (typeof iterable === 'string') it = Array.from(iterable).values();
    else throw new TypeError(String(iterable) + ' is not async iterable');
    if (Array.isArray(it)) { var arr = it, at = 0; it = { next: function () { return at < arr.length ? { value: arr[at++], done: false } : { value: undefined, done: true }; } }; }   // (the engine's [Symbol.iterator]() answers an array)
    var wrapped = {
      next: function (v) { var r = it.next(v); return Promise.resolve(r.value).then(function (value) { return { value: value, done: r.done }; }); },
      return: function (v) { if (typeof it.return === 'function') it.return(v); return Promise.resolve({ value: v, done: true }); }
    };
    wrapped['@@asyncIterator'] = function () { return this; };
    return wrapped;
  });
  // Array.fromAsync: Array.from for async iterables (and iterables of promises, and array-likes): each element and each mapped value is awaited
  def(Array, 'fromAsync', function fromAsync(items, mapfn, thisArg) {
    if (new.target !== undefined) throw new TypeError('Array.fromAsync is not a constructor');
    return (async function () {
      if (mapfn !== undefined && typeof mapfn !== 'function') throw new TypeError(String(mapfn) + ' is not a function');
      var out = [], index = 0;
      var add = async function (value) { out.push(mapfn ? await mapfn.call(thisArg, value, index) : value); index++; };
      if (items === null || items === undefined) throw new TypeError(String(items) + ' is not iterable');
      if (typeof items[Symbol.asyncIterator] === 'function') { for await (var a of items) await add(a); }
      else if (typeof items[Symbol.iterator] === 'function') { for (var v of items) await add(await v); }
      else { var length = Number(items.length) || 0; for (var i = 0; i < length; i++) await add(await items[i]); }
      return out;
    }());
  });
  Object.defineProperty(Array.fromAsync, 'length', { value: 1 });
  // the value properties of the global object cannot be changed, and neither can the prototype of a built-in constructor
  ['NaN', 'Infinity', 'undefined'].forEach(function (n) {
    var value = n === 'NaN' ? NaN : n === 'Infinity' ? Infinity : undefined;
    Object.defineProperty(g, n, { value: value, writable: false, enumerable: false, configurable: false });
  });
  ['Object', 'Function', 'Array', 'String', 'Number', 'Boolean', 'Symbol', 'BigInt', 'Date', 'RegExp', 'Map', 'Set', 'WeakMap', 'WeakSet', 'WeakRef', 'Promise', 'Error', 'TypeError', 'RangeError', 'SyntaxError', 'ReferenceError',
   'EvalError', 'URIError', 'ArrayBuffer', 'DataView', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'Float32Array', 'Float64Array'].forEach(function (n) {
    var C = g[n];
    if (typeof C === 'function' && C.prototype !== undefined) { try { Object.defineProperty(C, 'prototype', { writable: false, enumerable: false, configurable: false }); } catch (e) { } }
  });
}(this));
)JSLIB";

}  // namespace

const char* stdlib_source() { return kStdlib; }

}  // namespace script
