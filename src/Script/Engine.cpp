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

#include "Script/Engine.hpp"
#include "Script/Stdlib.hpp"
#include "Script/Ast.hpp"
#include "Kernel/Thread.hpp"
#include "Script/Compiler.hpp"

namespace script {

namespace {

// The parser, the compiler and the interpreter recurse, and threads of this system have small stacks (64 KiB): the work is done on a thread with a
// stack of its own where the system runs threads (and right here where it does not, as in the tests).
constexpr uint32_t kScriptStackBytes = 1u << 20;

template <class F>
void on_big_stack(F&& job) {
  struct Context {
    F* job;
  } context{&job};
  if (kernel::running()) {
    kernel::Thread* thread = kernel::spawn([](void* p) { (*static_cast<Context*>(p)->job)(); }, &context, "script", -1, -1, kScriptStackBytes);
    if (thread) {
      kernel::join(thread);
      return;
    }
  }
  job();
}

// The parts of the standard library that are easiest to write in the language itself. Run once for each engine, before any page script.
const char kPrelude[] =
    "(function () {\n"
    "  var P = Promise;\n"
    "  function list(iterable) { var a = []; for (var x of iterable) a.push(x); return a; }\n"
    "  P.all = function (iterable) {\n"
    "    return new P(function (resolve, reject) {\n"
    "      var items = list(iterable), left = items.length, out = [];\n"
    "      if (left === 0) { resolve(out); return; }\n"
    "      items.forEach(function (item, i) {\n"
    "        out.push(undefined);\n"
    "        P.resolve(item).then(function (v) { out[i] = v; if (--left === 0) resolve(out); }, reject);\n"
    "      });\n"
    "    });\n"
    "  };\n"
    "  P.allSettled = function (iterable) {\n"
    "    return new P(function (resolve) {\n"
    "      var items = list(iterable), left = items.length, out = [];\n"
    "      if (left === 0) { resolve(out); return; }\n"
    "      items.forEach(function (item, i) {\n"
    "        out.push(undefined);\n"
    "        P.resolve(item).then(function (v) { out[i] = { status: 'fulfilled', value: v }; if (--left === 0) resolve(out); },\n"
    "                             function (e) { out[i] = { status: 'rejected', reason: e }; if (--left === 0) resolve(out); });\n"
    "      });\n"
    "    });\n"
    "  };\n"
    "  P.race = function (iterable) {\n"
    "    return new P(function (resolve, reject) {\n"
    "      list(iterable).forEach(function (item) { P.resolve(item).then(resolve, reject); });\n"
    "    });\n"
    "  };\n"
    "  P.any = function (iterable) {\n"
    "    return new P(function (resolve, reject) {\n"
    "      var items = list(iterable), left = items.length, errors = [];\n"
    "      function fail() { var e = new Error('All promises were rejected'); e.name = 'AggregateError'; e.errors = errors; reject(e); }\n"
    "      if (left === 0) { fail(); return; }\n"
    "      items.forEach(function (item, i) {\n"
    "        errors.push(undefined);\n"
    "        P.resolve(item).then(resolve, function (e) { errors[i] = e; if (--left === 0) fail(); });\n"
    "      });\n"
    "    });\n"
    "  };\n"
    "  P.withResolvers = function () {\n"
    "    var r = {};\n"
    "    r.promise = new P(function (resolve, reject) { r.resolve = resolve; r.reject = reject; });\n"
    "    return r;\n"
    "  };\n"
    "  P.prototype.finally = function (f) {\n"
    "    if (typeof f !== 'function') return this.then(f, f);\n"
    "    return this.then(function (v) { return P.resolve(f()).then(function () { return v; }); },\n"
    "                     function (e) { return P.resolve(f()).then(function () { throw e; }); });\n"
    "  };\n"
    "})();\n";

// Map, Set, WeakMap, WeakSet, WeakRef and Reflect, written in the language itself.
const char kCollections[] = R"JSLIB((function (g) {
  var ITER = Symbol.iterator;
  var def = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };

  // ------------------------------------------------------------------------------------------------------------ Iterator and its helpers
  function Iterator() {
    if (new.target === undefined || new.target === Iterator) throw new TypeError('Abstract class Iterator not directly constructable');
  }
  var IP = Iterator.prototype;
  def(IP, ITER, function () { return this; });
  Object.defineProperty(IP, Symbol.toStringTag, { value: 'Iterator', configurable: true });
  def(g, 'Iterator', Iterator);
  // the iterators of arrays, typed arrays and strings: objects that walk a list
  var ArrayIteratorProto = Object.create(IP);
  def(ArrayIteratorProto, 'next', function () {
    var list = this.__list;
    if (list === undefined) return { value: undefined, done: true };
    var i = this.__at;
    if (i >= list.length) { this.__list = undefined; return { value: undefined, done: true }; }
    this.__at = i + 1;
    return { value: this.__kind === 0 ? i : this.__kind === 1 ? list[i] : [i, list[i]], done: false };
  });
  Object.defineProperty(ArrayIteratorProto, Symbol.toStringTag, { value: 'Array Iterator', configurable: true });
  def(g, '__array_iterator', function (list, kind) {
    var it = Object.create(ArrayIteratorProto);
    Object.defineProperty(it, '__list', { value: list, writable: true });
    Object.defineProperty(it, '__at', { value: 0, writable: true });
    Object.defineProperty(it, '__kind', { value: kind });
    return it;
  });
  // GeneratorFunction and AsyncFunction: the constructors of generator and async functions (not globals); they build a function from source like Function does
  (function () {
    var protos = __function_protos(), generatorProto = __generator_proto;
    [['GeneratorFunction', protos[0], 'function*'], ['AsyncFunction', protos[1], 'async function']].forEach(function (spec) {
      var proto = spec[1], prefix = spec[2];
      var C = function () {
        var a = Array.prototype.slice.call(arguments), body = a.length ? String(a.pop()) : '';
        return (0, eval)('(' + prefix + ' anonymous(' + a.join(',') + '\n) {\n' + body + '\n})');
      };
      Object.defineProperty(C, 'name', { value: spec[0] });
      Object.setPrototypeOf(C, Function);
      Object.defineProperty(C, 'prototype', { value: proto });
      Object.defineProperty(proto, 'constructor', { value: C, configurable: true });
      Object.defineProperty(proto, Symbol.toStringTag, { value: spec[0], configurable: true });
      if (spec[0] === 'GeneratorFunction') Object.defineProperty(proto, 'prototype', { value: generatorProto, configurable: true });
    });
  }());
  // the generator objects (and so the iterators of arrays and typed arrays) are iterators
  (function () {
    var generator = (function* () { })();
    Object.setPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf(generator)), IP);
  }());
  function direct(o) {
    if (o === null || (typeof o !== 'object' && typeof o !== 'function')) throw new TypeError('Iterator helper called on a non-object');
    return { it: o, next: o.next };
  }
  function step(rec) {
    var r = rec.next.call(rec.it);
    if (r === null || (typeof r !== 'object' && typeof r !== 'function')) throw new TypeError('Iterator result ' + r + ' is not an object');
    return r;
  }
  function close(rec) {
    var ret = rec.it.return;
    if (ret !== undefined && ret !== null) ret.call(rec.it);
  }
  function callable(fn) { if (typeof fn !== 'function') throw new TypeError(String(fn) + ' is not a function'); }
  function count(n, what) {
    var v = Number(n);
    if (v !== v) throw new RangeError(String(n) + ' must be positive');
    v = Math.trunc(v);
    if (v < 0) throw new RangeError(String(n) + ' must be positive');
    return v;
  }
  // an iterator helper: a generator that closes the underlying iterator when it is stopped early
  def(IP, 'map', function (fn) {
    var rec = (callable(fn), direct(this));
    return (function* () {
      var done = false, i = 0;
      try {
        for (;;) {
          var r = step(rec);
          if (r.done) { done = true; return; }
          var v;
          try { v = fn(r.value, i++); } catch (e) { done = true; close(rec); throw e; }
          yield v;
        }
      } finally { if (!done) close(rec); }
    }());
  });
  def(IP, 'filter', function (fn) {
    var rec = (callable(fn), direct(this));
    return (function* () {
      var done = false, i = 0;
      try {
        for (;;) {
          var r = step(rec);
          if (r.done) { done = true; return; }
          var keep;
          try { keep = fn(r.value, i++); } catch (e) { done = true; close(rec); throw e; }
          if (keep) yield r.value;
        }
      } finally { if (!done) close(rec); }
    }());
  });
  def(IP, 'take', function (limit) {
    var n = count(limit), rec = direct(this);
    return (function* () {
      var done = false, left = n;
      try {
        for (;;) {
          if (left === 0) return;
          left--;
          var r = step(rec);
          if (r.done) { done = true; return; }
          yield r.value;
        }
      } finally { if (!done) close(rec); }
    }());
  });
  def(IP, 'drop', function (limit) {
    var n = count(limit), rec = direct(this);
    return (function* () {
      var done = false, left = n;
      try {
        while (left > 0) { left--; var skipped = step(rec); if (skipped.done) { done = true; return; } }
        for (;;) {
          var r = step(rec);
          if (r.done) { done = true; return; }
          yield r.value;
        }
      } finally { if (!done) close(rec); }
    }());
  });
  def(IP, 'flatMap', function (fn) {
    var rec = (callable(fn), direct(this));
    return (function* () {
      var done = false, i = 0;
      try {
        for (;;) {
          var r = step(rec);
          if (r.done) { done = true; return; }
          var inner;
          try {
            var mapped = fn(r.value, i++);
            if (mapped === null || typeof mapped !== 'object' && typeof mapped !== 'function') throw new TypeError('flatMap callback must return an iterable object');
            var method = mapped[ITER];
            inner = direct(method === undefined || method === null ? mapped : method.call(mapped));
          } catch (e) { done = true; close(rec); throw e; }
          var finished = false;
          try {
            for (;;) {
              var x = step(inner);
              if (x.done) { finished = true; break; }
              yield x.value;
            }
          } finally { if (!finished) { try { close(inner); } finally { } } }
        }
      } finally { if (!done) close(rec); }
    }());
  });
  def(IP, 'reduce', function (fn) {
    callable(fn);
    var rec = direct(this), acc, i = 0;
    if (arguments.length > 1) acc = arguments[1];
    else {
      var first = step(rec);
      if (first.done) throw new TypeError('Reduce of empty iterator with no initial value');
      acc = first.value; i = 1;
    }
    for (;;) {
      var r = step(rec);
      if (r.done) return acc;
      try { acc = fn(acc, r.value, i++); } catch (e) { close(rec); throw e; }
    }
  });
  def(IP, 'toArray', function () {
    var rec = direct(this), out = [];
    for (;;) { var r = step(rec); if (r.done) return out; out.push(r.value); }
  });
  def(IP, 'forEach', function (fn) {
    callable(fn);
    var rec = direct(this), i = 0;
    for (;;) {
      var r = step(rec);
      if (r.done) return undefined;
      try { fn(r.value, i++); } catch (e) { close(rec); throw e; }
    }
  });
  function search(name, stopOn, found, notFound) {
    def(IP, name, function (fn) {
      callable(fn);
      var rec = direct(this), i = 0;
      for (;;) {
        var r = step(rec);
        if (r.done) return notFound;
        var hit;
        try { hit = !!fn(r.value, i++); } catch (e) { close(rec); throw e; }
        if (hit === stopOn) { close(rec); return found(r.value); }
      }
    });
  }
  search('some', true, function () { return true; }, false);
  search('every', false, function () { return false; }, true);
  search('find', true, function (v) { return v; }, undefined);
  ['map', 'filter', 'take', 'drop', 'flatMap', 'reduce', 'toArray', 'forEach', 'some', 'every', 'find'].forEach(function (k) { Object.defineProperty(IP[k], 'name', { value: k }); });
  var WrapProto = Object.create(IP);
  def(WrapProto, 'next', function () { return this.__rec.next.call(this.__rec.it); });
  def(WrapProto, 'return', function () {
    var ret = this.__rec.it.return;
    if (ret === undefined || ret === null) return { value: undefined, done: true };
    return ret.call(this.__rec.it);
  });
  def(Iterator, 'from', function (o) {
    if (o === null || (typeof o !== 'object' && typeof o !== 'function' && typeof o !== 'string')) throw new TypeError(String(o) + ' is not an object');
    var method = o[ITER], it;
    if (method === undefined || method === null) {
      if (typeof o === 'string') throw new TypeError('not iterable');
      it = o;
    } else {
      it = method.call(o);
    }
    if (it === null || (typeof it !== 'object' && typeof it !== 'function')) throw new TypeError('Iterator is not an object');
    for (var p = Object.getPrototypeOf(it); p; p = Object.getPrototypeOf(p)) if (p === IP) return it;
    var wrapper = Object.create(WrapProto);
    Object.defineProperty(wrapper, '__rec', { value: direct(it) });
    return wrapper;
  });
  var nextId = 1;
  // The key of a value in a Map or Set: primitives by type and text, objects by an id they carry (SameValueZero: NaN is NaN, -0 is 0).
  function keyOf(map, value) {
    var t = typeof value;
    if (t === 'string') return 's' + value;
    if (t === 'number') return 'n' + value;
    if (t === 'boolean') return value ? 'bt' : 'bf';
    if (value === undefined) return 'u';
    if (value === null) return 'l';
    if (t === 'symbol') return 'y' + __symbol_info(value, 3);
    if (t === 'bigint') return 'g' + String(value);
    var id = value.__oid;
    if (id === undefined) {
      id = nextId++;
      try { Object.defineProperty(value, '__oid', { value: id }); } catch (e) { return 'x' + (map.__loose.push(value) - 1); }
    }
    return 'o' + id;
  }
  function Table() {
    this.keys = []; this.values = []; this.alive = []; this.index = {}; this.live = 0; this.dead = 0; this.iterators = []; this.loose = [];
  }
  function find(table, value) {
    var t = typeof value;
    if (t === 'object' || t === 'function') {
      if (value === null) return table.index['l'];
      if (value.__oid === undefined) {
        for (var i = 0; i < table.loose.length; i++) if (table.loose[i] === value) return table.index['x' + i];
        try { Object.defineProperty(value, '__oid', { value: nextId++ }); } catch (e) { return undefined; }
      }
      return table.index['o' + value.__oid];
    }
    return table.index[keyOf(table, value)];
  }
  function put(table, key, value) {
    if (key === 0) key = 0;
    var at = find(table, key);
    if (at !== undefined) { table.values[at] = value; return; }
    var name;
    var t = typeof key;
    if ((t === 'object' || t === 'function') && key !== null && key.__oid === undefined) {
      try { Object.defineProperty(key, '__oid', { value: nextId++ }); name = 'o' + key.__oid; }
      catch (e) { name = 'x' + table.loose.length; table.loose.push(key); }
    } else if ((t === 'object' || t === 'function') && key !== null) {
      name = 'o' + key.__oid;
    } else {
      name = keyOf(table, key);
    }
    table.index[name] = table.keys.length;
    table.keys.push(key); table.values.push(value); table.alive.push(true); table.names = table.names || []; table.names.push(name);
    table.live++;
  }
  function remove(table, key) {
    var at = find(table, key);
    if (at === undefined) return false;
    delete table.index[table.names[at]];
    table.alive[at] = false; table.keys[at] = undefined; table.values[at] = undefined;
    table.live--; table.dead++;
    if (table.dead > 1024 && table.dead > 2 * table.live) compact(table);
    return true;
  }
  function compact(table) {
    var keys = [], values = [], alive = [], names = [], remap = [], index = {};
    for (var i = 0; i < table.keys.length; i++) {
      remap.push(keys.length);
      if (!table.alive[i]) continue;
      index[table.names[i]] = keys.length;
      keys.push(table.keys[i]); values.push(table.values[i]); alive.push(true); names.push(table.names[i]);
    }
    remap.push(keys.length);
    for (var j = 0; j < table.iterators.length; j++) table.iterators[j].at = remap[Math.min(table.iterators[j].at, remap.length - 1)];
    table.keys = keys; table.values = values; table.alive = alive; table.names = names; table.index = index; table.dead = 0;
  }
  function clearTable(table) {
    for (var j = 0; j < table.iterators.length; j++) table.iterators[j].at = 0;
    table.keys = []; table.values = []; table.alive = []; table.names = []; table.index = {}; table.live = 0; table.dead = 0; table.loose = [];
  }
  function iterator(owner, kind, name) {
    var it = { at: 0, done: false };
    var table = owner.__t;
    table.iterators.push(it);
    var result = {
      next: function () {
        if (!it.done) {
          while (it.at < table.keys.length && !table.alive[it.at]) it.at++;
          if (it.at < table.keys.length) {
            var i = it.at++;
            return { value: kind === 0 ? table.keys[i] : kind === 1 ? table.values[i] : [table.keys[i], table.values[i]], done: false };
          }
          it.done = true;
          var pos = table.iterators.indexOf(it);
          if (pos >= 0) table.iterators.splice(pos, 1);
        }
        return { value: undefined, done: true };
      }
    };
    Object.setPrototypeOf(result, IP);
    def(result, ITER, function () { return this; });
    def(result, 'toString', function () { return '[object ' + name + ']'; });
    return result;
  }
  function needNew(self, ctor, name) { if (!(self instanceof ctor)) throw new TypeError("Constructor " + name + " requires 'new'"); }
  function Map(iterable) {
    needNew(this, Map, 'Map');
    def(this, '__t', new Table());
    this.size = 0;
    if (iterable !== undefined && iterable !== null) {
      var self = this;
      for (var entry of iterable) {
        if (entry === null || (typeof entry !== 'object' && typeof entry !== 'function')) throw new TypeError('Iterator value ' + String(entry) + ' is not an entry object');
        self.set(entry[0], entry[1]);
      }
    }
  }
  var MP = Map.prototype;
  def(MP, 'get', function (k) { var at = find(this.__t, k); return at === undefined ? undefined : this.__t.values[at]; });
  def(MP, 'set', function (k, v) { put(this.__t, k, v); this.size = this.__t.live; return this; });
  def(MP, 'has', function (k) { return find(this.__t, k) !== undefined; });
  def(MP, 'delete', function (k) { var r = remove(this.__t, k); this.size = this.__t.live; return r; });
  def(MP, 'clear', function () { clearTable(this.__t); this.size = 0; });
  def(MP, 'forEach', function (fn, thisArg) {
    if (typeof fn !== 'function') throw new TypeError(String(fn) + ' is not a function');
    var it = iterator(this, 2, 'Map Iterator'), step;
    while (!(step = it.next()).done) fn.call(thisArg, step.value[1], step.value[0], this);
  });
  def(MP, 'keys', function () { return iterator(this, 0, 'Map Iterator'); });
  def(MP, 'values', function () { return iterator(this, 1, 'Map Iterator'); });
  def(MP, 'entries', function () { return iterator(this, 2, 'Map Iterator'); });
  def(MP, ITER, MP.entries);
  def(MP, 'toString', function () { return '[object Map]'; });
  def(g, 'Map', Map);

  function Set(iterable) {
    needNew(this, Set, 'Set');
    def(this, '__t', new Table());
    this.size = 0;
    if (iterable !== undefined && iterable !== null) for (var v of iterable) this.add(v);
  }
  var SP = Set.prototype;
  def(SP, 'add', function (v) { put(this.__t, v, v); this.size = this.__t.live; return this; });
  def(SP, 'has', function (v) { return find(this.__t, v) !== undefined; });
  def(SP, 'delete', function (v) { var r = remove(this.__t, v); this.size = this.__t.live; return r; });
  def(SP, 'clear', function () { clearTable(this.__t); this.size = 0; });
  def(SP, 'forEach', function (fn, thisArg) {
    if (typeof fn !== 'function') throw new TypeError(String(fn) + ' is not a function');
    var it = iterator(this, 0, 'Set Iterator'), step;
    while (!(step = it.next()).done) fn.call(thisArg, step.value, step.value, this);
  });
  def(SP, 'values', function () { return iterator(this, 0, 'Set Iterator'); });
  def(SP, 'keys', SP.values);
  def(SP, 'entries', function () {
    var it = iterator(this, 0, 'Set Iterator');
    var inner = it.next;
    it.next = function () { var s = inner(); return s.done ? s : { value: [s.value, s.value], done: false }; };
    return it;
  });
  def(SP, ITER, SP.values);
  def(SP, 'toString', function () { return '[object Set]'; });
  def(SP, 'union', function (other) { var r = new Set(this); for (var v of other.keys()) r.add(v); return r; });
  def(SP, 'intersection', function (other) { var r = new Set(); for (var v of this) if (other.has(v)) r.add(v); return r; });
  def(SP, 'difference', function (other) { var r = new Set(); for (var v of this) if (!other.has(v)) r.add(v); return r; });
  def(SP, 'symmetricDifference', function (other) { var r = new Set(this); for (var v of other.keys()) { if (this.has(v)) r.delete(v); else r.add(v); } return r; });
  def(SP, 'isSubsetOf', function (other) { for (var v of this) if (!other.has(v)) return false; return true; });
  def(SP, 'isSupersetOf', function (other) { for (var v of other.keys()) if (!this.has(v)) return false; return true; });
  def(SP, 'isDisjointFrom', function (other) { for (var v of this) if (other.has(v)) return false; return true; });
  def(g, 'Set', Set);

  // Weak collections keep their entries on the key itself, so they go when the key does.
  var weakId = 1;
  function weakKey(key, what) {
    if (key === null || (typeof key !== 'object' && typeof key !== 'function')) throw new TypeError('Invalid value used ' + what);
    return key;
  }
  function WeakMap(iterable) {
    needNew(this, WeakMap, 'WeakMap');
    def(this, '__p', '__wm' + weakId++);
    if (iterable !== undefined && iterable !== null) for (var e of iterable) this.set(e[0], e[1]);
  }
  var WMP = WeakMap.prototype;
  def(WMP, 'get', function (k) { return k !== null && (typeof k === 'object' || typeof k === 'function') && this.__p in k ? k[this.__p] : undefined; });
  def(WMP, 'set', function (k, v) {
    weakKey(k, 'as weak map key');
    if (this.__p in k) k[this.__p] = v;
    else Object.defineProperty(k, this.__p, { value: v, writable: true, configurable: true });
    return this;
  });
  def(WMP, 'has', function (k) { return k !== null && (typeof k === 'object' || typeof k === 'function') && this.__p in k; });
  def(WMP, 'delete', function (k) { if (!WMP.has.call(this, k)) return false; delete k[this.__p]; return true; });
  def(WMP, 'toString', function () { return '[object WeakMap]'; });
  def(g, 'WeakMap', WeakMap);
  function WeakSet(iterable) {
    needNew(this, WeakSet, 'WeakSet');
    def(this, '__p', '__ws' + weakId++);
    if (iterable !== undefined && iterable !== null) for (var v of iterable) this.add(v);
  }
  var WSP = WeakSet.prototype;
  def(WSP, 'add', function (k) { weakKey(k, 'in weak set'); if (!(this.__p in k)) Object.defineProperty(k, this.__p, { value: true, writable: true, configurable: true }); return this; });
  def(WSP, 'has', function (k) { return k !== null && (typeof k === 'object' || typeof k === 'function') && this.__p in k; });
  def(WSP, 'delete', function (k) { if (!WSP.has.call(this, k)) return false; delete k[this.__p]; return true; });
  def(WSP, 'toString', function () { return '[object WeakSet]'; });
  def(g, 'WeakSet', WeakSet);
  function WeakRef(target) { needNew(this, WeakRef, 'WeakRef'); weakKey(target, 'as WeakRef target'); def(this, '__target', target); }
  def(WeakRef.prototype, 'deref', function () { return this.__target; });
  def(g, 'WeakRef', WeakRef);
  function FinalizationRegistry(callback) { needNew(this, FinalizationRegistry, 'FinalizationRegistry'); }
  def(FinalizationRegistry.prototype, 'register', function () {});
  def(FinalizationRegistry.prototype, 'unregister', function () { return false; });
  def(g, 'FinalizationRegistry', FinalizationRegistry);

  // SharedArrayBuffer and Atomics: there is one thread of script, so a shared buffer is an ArrayBuffer under another name and the operations are plain reads and writes.
  function SharedArrayBuffer(length) {
    needNew(this, SharedArrayBuffer, 'SharedArrayBuffer');
    var b = new ArrayBuffer(length);
    Object.setPrototypeOf(b, SharedArrayBuffer.prototype);
    return b;
  }
  SharedArrayBuffer.prototype = Object.create(ArrayBuffer.prototype);
  def(SharedArrayBuffer.prototype, 'constructor', SharedArrayBuffer);
  Object.defineProperty(SharedArrayBuffer.prototype, '@@toStringTag', { value: 'SharedArrayBuffer', configurable: true });
  Object.defineProperty(SharedArrayBuffer.prototype, 'growable', { get: function () { return false; }, configurable: true });
  def(g, 'SharedArrayBuffer', SharedArrayBuffer);
  var Atomics = {};
  function atomicIndex(ta, index) {
    if (!ArrayBuffer.isView(ta) || ta instanceof DataView || ta instanceof Float32Array || ta instanceof Float64Array || ta instanceof Uint8ClampedArray) throw new TypeError('Atomics operation on an array that is not an integer typed array');
    var i = Number(index);
    i = i !== i ? 0 : Math.trunc(i);
    if (i < 0 || i >= ta.length) throw new RangeError('Invalid atomic access index');
    return i;
  }
  function atomicOp(name, combine) {
    def(Atomics, name, function (ta, index, value) {
      var i = atomicIndex(ta, index);
      var old = ta[i];
      ta[i] = combine(old, typeof old === 'bigint' ? BigInt(value) : Math.trunc(Number(value)));
      return old;
    });
  }
  atomicOp('add', function (a, b) { return a + b; });
  atomicOp('sub', function (a, b) { return a - b; });
  atomicOp('and', function (a, b) { return a & b; });
  atomicOp('or', function (a, b) { return a | b; });
  atomicOp('xor', function (a, b) { return a ^ b; });
  atomicOp('exchange', function (a, b) { return b; });
  def(Atomics, 'load', function (ta, index) { return ta[atomicIndex(ta, index)]; });
  def(Atomics, 'store', function (ta, index, value) {
    var i = atomicIndex(ta, index);
    var v = typeof ta[i] === 'bigint' ? BigInt(value) : Math.trunc(Number(value)) + 0;
    ta[i] = v;
    return v;
  });
  def(Atomics, 'compareExchange', function (ta, index, expected, replacement) {
    var i = atomicIndex(ta, index);
    var old = ta[i];
    var probe = new ta.constructor(1);
    probe[0] = expected;
    if (old === probe[0]) ta[i] = replacement;
    return old;
  });
  def(Atomics, 'isLockFree', function (n) { return n === 1 || n === 2 || n === 4 || n === 8; });
  def(Atomics, 'notify', function (ta, index) { atomicIndex(ta, index); return 0; });
  def(Atomics, 'wait', function (ta, index, value) { atomicIndex(ta, index); return ta[index] !== value ? 'not-equal' : 'timed-out'; });
  Object.defineProperty(Atomics, '@@toStringTag', { value: 'Atomics', configurable: true });
  def(g, 'Atomics', Atomics);

  // legacy Annex B built-ins
  def(Object.prototype, '__defineGetter__', function (key, fn) { if (typeof fn !== 'function') throw new TypeError('Object.prototype.__defineGetter__: Expecting function'); Object.defineProperty(this, key, { get: fn, enumerable: true, configurable: true }); });
  def(Object.prototype, '__defineSetter__', function (key, fn) { if (typeof fn !== 'function') throw new TypeError('Object.prototype.__defineSetter__: Expecting function'); Object.defineProperty(this, key, { set: fn, enumerable: true, configurable: true }); });
  def(Object.prototype, '__lookupGetter__', function (key) { for (var o = this; o; o = Object.getPrototypeOf(o)) { var d = Object.getOwnPropertyDescriptor(o, key); if (d) return d.get; } });
  def(Object.prototype, '__lookupSetter__', function (key) { for (var o = this; o; o = Object.getPrototypeOf(o)) { var d = Object.getOwnPropertyDescriptor(o, key); if (d) return d.set; } });
  var HEX = '0123456789ABCDEF';
  def(g, 'escape', function (value) {
    var text = String(value), out = '';
    for (var i = 0; i < text.length; i++) {
      var c = text.charCodeAt(i);
      if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || '@*_+-./'.indexOf(text[i]) >= 0) out += text[i];
      else if (c < 256) out += '%' + HEX[c >> 4] + HEX[c & 15];
      else out += '%u' + HEX[(c >> 12) & 15] + HEX[(c >> 8) & 15] + HEX[(c >> 4) & 15] + HEX[c & 15];
    }
    return out;
  });
  def(g, 'unescape', function (value) {
    var text = String(value);
    return text.replace(/%u([0-9a-fA-F]{4})|%([0-9a-fA-F]{2})/g, function (m, u, b) { return String.fromCharCode(parseInt(u || b, 16)); });
  });
  [['anchor', 'a', 'name'], ['big', 'big'], ['blink', 'blink'], ['bold', 'b'], ['fixed', 'tt'], ['fontcolor', 'font', 'color'], ['fontsize', 'font', 'size'], ['italics', 'i'],
   ['link', 'a', 'href'], ['small', 'small'], ['strike', 'strike'], ['sub', 'sub'], ['sup', 'sup']].forEach(function (spec) {
    def(String.prototype, spec[0], function (value) {
      if (this === null || this === undefined) throw new TypeError('String.prototype.' + spec[0] + ' called on null or undefined');
      var open = '<' + spec[1];
      if (spec[2]) open += ' ' + spec[2] + '="' + String(value).replace(/"/g, '&quot;') + '"';
      return open + '>' + String(this) + '</' + spec[1] + '>';
    });
  });
  [[MP, 'Map'], [SP, 'Set'], [WMP, 'WeakMap'], [WSP, 'WeakSet']].forEach(function (pair) {
    Object.defineProperty(pair[0], '@@toStringTag', { value: pair[1], configurable: true });
  });

  // Array.from / Array.of
  if (!Array.from) def(Array, 'from', function (source, mapFn, thisArg) {
    if (source === null || source === undefined) throw new TypeError(String(source) + ' is not iterable');
    if (mapFn !== undefined && typeof mapFn !== 'function') throw new TypeError(String(mapFn) + ' is not a function');
    var out = [], i = 0;
    if (typeof source[ITER] === 'function') {
      for (var v of source) { out.push(mapFn ? mapFn.call(thisArg, v, i) : v); i++; }
    } else {
      var n = source.length >>> 0;
      for (; i < n; i++) out.push(mapFn ? mapFn.call(thisArg, source[i], i) : source[i]);
    }
    return out;
  });
  if (!Array.of) def(Array, 'of', function () { var out = []; for (var i = 0; i < arguments.length; i++) out.push(arguments[i]); return out; });
  // Typed arrays: the methods that walk the elements (the natives are in TypedArray.cpp)
  (function () {
    var T = g.__typed_array_proto;
    var kinds = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'Float32Array', 'Float64Array'];
    function relative(i, n) { i = Math.trunc(i) || 0; return i < 0 ? Math.max(n + i, 0) : Math.min(i, n); }
    function same(a, n) { return new a.constructor(n); }
    function need(fn) { if (typeof fn !== 'function') throw new TypeError(String(fn) + ' is not a function'); }
    def(T, 'forEach', function (fn, thisArg) { need(fn); for (var i = 0; i < this.length; i++) fn.call(thisArg, this[i], i, this); });
    def(T, 'map', function (fn, thisArg) { need(fn); var out = same(this, this.length); for (var i = 0; i < this.length; i++) out[i] = fn.call(thisArg, this[i], i, this); return out; });
    def(T, 'filter', function (fn, thisArg) {
      need(fn);
      var kept = [];
      for (var i = 0; i < this.length; i++) if (fn.call(thisArg, this[i], i, this)) kept.push(this[i]);
      var out = same(this, kept.length);
      for (var j = 0; j < kept.length; j++) out[j] = kept[j];
      return out;
    });
    def(T, 'reduce', function (fn, init) {
      need(fn);
      var i = 0, acc;
      if (arguments.length > 1) acc = init;
      else { if (this.length === 0) throw new TypeError('Reduce of empty array with no initial value'); acc = this[0]; i = 1; }
      for (; i < this.length; i++) acc = fn(acc, this[i], i, this);
      return acc;
    });
    def(T, 'reduceRight', function (fn, init) {
      need(fn);
      var i = this.length - 1, acc;
      if (arguments.length > 1) acc = init;
      else { if (this.length === 0) throw new TypeError('Reduce of empty array with no initial value'); acc = this[i]; i--; }
      for (; i >= 0; i--) acc = fn(acc, this[i], i, this);
      return acc;
    });
    def(T, 'some', function (fn, thisArg) { need(fn); for (var i = 0; i < this.length; i++) if (fn.call(thisArg, this[i], i, this)) return true; return false; });
    def(T, 'every', function (fn, thisArg) { need(fn); for (var i = 0; i < this.length; i++) if (!fn.call(thisArg, this[i], i, this)) return false; return true; });
    def(T, 'find', function (fn, thisArg) { need(fn); for (var i = 0; i < this.length; i++) if (fn.call(thisArg, this[i], i, this)) return this[i]; return undefined; });
    def(T, 'findIndex', function (fn, thisArg) { need(fn); for (var i = 0; i < this.length; i++) if (fn.call(thisArg, this[i], i, this)) return i; return -1; });
    def(T, 'findLast', function (fn, thisArg) { need(fn); for (var i = this.length - 1; i >= 0; i--) if (fn.call(thisArg, this[i], i, this)) return this[i]; return undefined; });
    def(T, 'findLastIndex', function (fn, thisArg) { need(fn); for (var i = this.length - 1; i >= 0; i--) if (fn.call(thisArg, this[i], i, this)) return i; return -1; });
    def(T, 'indexOf', function (v, from) { for (var i = relative(from, this.length); i < this.length; i++) if (this[i] === v) return i; return -1; });
    def(T, 'lastIndexOf', function (v, from) {
      var i = arguments.length > 1 ? (from < 0 ? this.length + from : Math.min(from, this.length - 1)) : this.length - 1;
      for (; i >= 0; i--) if (this[i] === v) return i;
      return -1;
    });
    def(T, 'includes', function (v, from) {
      for (var i = relative(from, this.length); i < this.length; i++) if (this[i] === v || (v !== v && this[i] !== this[i])) return true;
      return false;
    });
    def(T, 'join', function (sep) {
      sep = sep === undefined ? ',' : String(sep);
      var out = '';
      for (var i = 0; i < this.length; i++) out += (i ? sep : '') + this[i];
      return out;
    });
    def(T, 'toString', function () { return this.join(','); });
    def(T, 'toLocaleString', function () { return this.join(','); });
    def(T, 'reverse', function () {
      for (var i = 0, j = this.length - 1; i < j; i++, j--) { var t = this[i]; this[i] = this[j]; this[j] = t; }
      return this;
    });
    def(T, 'sort', function (compare) {
      if (compare !== undefined) need(compare);
      var items = [];
      for (var i = 0; i < this.length; i++) items.push(this[i]);
      items.sort(compare || function (a, b) { return a < b ? -1 : a > b ? 1 : (a === 0 && b === 0 ? (1 / a < 1 / b ? -1 : 1 / a > 1 / b ? 1 : 0) : (a !== a ? (b !== b ? 0 : 1) : (b !== b ? -1 : 0))); });
      for (var j = 0; j < items.length; j++) this[j] = items[j];
      return this;
    });
    def(T, 'at', function (i) { i = Math.trunc(i) || 0; if (i < 0) i += this.length; return this[i]; });
    def(T, 'copyWithin', function (target, start, end) {
      var n = this.length, to = relative(target, n), from = relative(start, n), last = end === undefined ? n : relative(end, n);
      var count = Math.min(last - from, n - to), copy = [];
      for (var i = 0; i < count; i++) copy.push(this[from + i]);
      for (var j = 0; j < count; j++) this[to + j] = copy[j];
      return this;
    });
    def(T, 'with', function (index, value) { var out = this.slice(); index = Math.trunc(index) || 0; if (index < 0) index += this.length; if (index < 0 || index >= this.length) throw new RangeError('Invalid typed array index'); out[index] = value; return out; });
    def(T, 'toReversed', function () { return this.slice().reverse(); });
    def(T, 'toSorted', function (compare) { return this.slice().sort(compare); });
    function walk(a, kind) { return g.__array_iterator(a, kind); }
    def(T, 'keys', function () { return walk(this, 0); });
    def(T, 'values', function () { return walk(this, 1); });
    def(T, 'entries', function () { return walk(this, 2); });
    def(T, ITER, T.values);
    kinds.forEach(function (name) {
      var ctor = g[name];
      def(ctor, 'from', function (source, mapFn, thisArg) {
        var items = Array.from(source, mapFn, thisArg);
        return new this(items);
      });
      def(ctor, 'of', function () { var items = []; for (var i = 0; i < arguments.length; i++) items.push(arguments[i]); return new this(items); });
    });
  }());
  // Object helpers and Reflect
  if (!Object.is) def(Object, 'is', function (a, b) { return a === b ? (a !== 0 || 1 / a === 1 / b) : (a !== a && b !== b); });
  if (!Object.fromEntries) def(Object, 'fromEntries', function (iterable) { var o = {}; for (var e of iterable) o[e[0]] = e[1]; return o; });
  var Reflect = {
    apply: function (f, self, args) { return f.apply(self, args); },
    construct: function (F, args, newTarget) {
      if (typeof F !== 'function') throw new TypeError(String(F) + ' is not a constructor');
      if (newTarget === undefined) newTarget = F; else if (typeof newTarget !== 'function') throw new TypeError(String(newTarget) + ' is not a constructor');
      if (args === null || (typeof args !== 'object' && typeof args !== 'function')) throw new TypeError('CreateListFromArrayLike called on non-object');
      return __reflect_construct(F, Array.prototype.slice.call(args), newTarget);
    },
    defineProperty: function (o, k, d) {
      if (o === null || (typeof o !== 'object' && typeof o !== 'function')) throw new TypeError('Reflect.defineProperty called on non-object');
      try {
        if (d !== null && typeof d === 'object' && !('configurable' in d)) { var c = {}; for (var p in d) c[p] = d[p]; c.configurable = false; d = c; }   // (a property made here is not configurable unless the descriptor says so)
        Object.defineProperty(o, k, d);
        return true;
      } catch (e) { if (e instanceof TypeError && /Invalid property descriptor|non-object|Getter must/.test(e.message)) throw e; return false; }
    },
    deleteProperty: function (o, k) { return delete o[k]; },
    get: function (o, k) { return o[k]; },
    set: function (o, k, v) {
      'use strict';
      try { o[k] = v; return true; } catch (e) { if (e instanceof TypeError && /^Cannot (assign|add|set|create)/.test(e.message)) return false; throw e; }   // (a write the object refuses)
    },
    has: function (o, k) { return k in o; },
    getPrototypeOf: function (o) { return Object.getPrototypeOf(o); },
    ownKeys: function (o) { return Object.getOwnPropertyNames(o).concat(Object.getOwnPropertySymbols(o)); },
    isExtensible: function (o) { return Object.isExtensible(o); },
    preventExtensions: function (o) { Object.preventExtensions(o); return true; },
    setPrototypeOf: function (o, p) { try { Object.setPrototypeOf(o, p); return true; } catch (e) { return false; } }
  };
  def(g, 'Reflect', Reflect);
}(this));
)JSLIB";

}  // namespace

Engine::Engine(allocators::IAllocator& allocator, IHost* host) : allocator_(allocator), host_(host), vm_(allocator, host) {
  vm_.set_memory_limit(memory_);
  vm_.set_instruction_budget(budget_);
  String error(allocator_);
  on_big_stack([&] {
    run_here(kPrelude, &error, nullptr, nullptr, false, true);
    run_here(kCollections, &error, nullptr, nullptr, false, true);
    run_here(stdlib_source(), &error, nullptr, nullptr, false, true);
    if (!error.empty() && host_) host_->console(3, error.view());
  });
}

void Engine::set_limits(uint64_t instructions, uint64_t memory_bytes) {
  budget_ = instructions;
  memory_ = memory_bytes;
  vm_.set_instruction_budget(budget_);
  vm_.set_memory_limit(memory_);
}

void Engine::sync_document() {
  if (!host_) return;
  Value document, title;
  if (!vm_.get_named(Value::make_object(vm_.global()), "document", &document) || !document.is_object()) return;
  if (!vm_.get_named(document, "title", &title) || !title.is_string()) return;
  if (!title.as_string()->view().equals(host_->title())) host_->set_title(title.as_string()->view());
}

bool Engine::run(StringView source, String* error, String* result) {
  bool ok = false;
  on_big_stack([&] { ok = run_here(source, error, result); });
  return ok;
}

bool Engine::run_library(StringView source, String* error) {
  bool ok = false;
  on_big_stack([&] { ok = run_here(source, error, nullptr, nullptr, false, true); });
  return ok;
}

bool Engine::evaluate(StringView source, String* error, Value* value) {
  bool ok = false;
  on_big_stack([&] { ok = run_here(source, error, nullptr, value); });
  return ok;
}

bool Engine::run_here(StringView source, String* error, String* result, Value* out_value, bool sync, bool library) {
  if (!vm_.ok()) {
    error->assign("Error: the script engine could not start");
    return false;
  }
  vm_.recover();
  Arena arena(allocator_);
  String message(allocator_);
  Node* program = parse_program(source, arena, &message);
  if (!program) {
    error->assign("SyntaxError: ");
    error->append(message.view());
    return false;
  }
  FunctionCode* code = nullptr;
  if (!compile_program(vm_, program, &code, &message, library)) {
    error->assign("SyntaxError: ");
    error->append(message.view());
    return false;
  }
  Value value;
  if (!vm_.run(code, &value)) {
    vm_.take_exception(error);
    if (sync) sync_document();
    return false;
  }
  if (out_value) *out_value = value;
  if (result) {
    result->clear();
    vm_.describe(value, result, 0);
  }
  if (sync) sync_document();
  return true;
}

uint32_t Engine::call_guarded(const Value& fn, const Value* args, uint32_t argc, String* error) {
  vm_.reset_instruction_count();
  vm_.set_extra_root(fn);   // (recover() may collect, and a timer that has run is no longer in the list: nothing else holds the function)
  vm_.recover();
  Value ignored;
  const bool ok = vm_.call(fn, Value::make_object(vm_.global()), args, argc, &ignored);
  vm_.set_extra_root(Value());
  if (ok) return 0;
  String text(allocator_);
  vm_.take_exception(&text);
  if (error && error->empty()) error->assign(text.view());
  return 1;
}

uint32_t Engine::run_timers(uint32_t max, String* error) {
  uint32_t failed = 0;
  on_big_stack([&] { failed = run_timers_here(1e300, true, max, error); });
  return failed;
}

uint32_t Engine::run_due_timers(double now_ms, uint32_t max, String* error) {
  uint32_t failed = 0;
  on_big_stack([&] { failed = run_timers_here(now_ms, false, max, error); });
  return failed;
}

bool Engine::next_timer(double* due_ms) {
  Value list_value;
  if (!vm_.get_named(Value::make_object(vm_.global()), "__timers", &list_value) || !list_value.is_object()) return false;
  JsObject* list = list_value.as_object();
  bool any = false;
  for (uint32_t i = 0; i < list->length; ++i) {
    if (!list->items[i].is_object()) continue;
    JsObject* e = list->items[i].as_object();
    if (e->length < 4 || !e->items[1].is_number()) continue;
    if (!any || e->items[1].u.number < *due_ms) *due_ms = e->items[1].u.number;
    any = true;
  }
  return any;
}

// Runs the timers whose time has come (all of them in order of their time if `ahead`: the clock is then moved forward to each one).
uint32_t Engine::run_timers_here(double now_ms, bool ahead, uint32_t max, String* error) {
  uint32_t failed = 0;
  Value list_value;
  if (!vm_.get_named(Value::make_object(vm_.global()), "__timers", &list_value) || !list_value.is_object()) return 0;
  JsObject* list = list_value.as_object();
  for (uint32_t n = 0; n < max; ++n) {
    int64_t best = -1;
    double best_due = 0;
    for (uint32_t i = 0; i < list->length; ++i) {   // the entry that is due first
      if (!list->items[i].is_object()) continue;
      JsObject* e = list->items[i].as_object();
      if (e->length < 4 || !e->items[1].is_number()) continue;
      if (best < 0 || e->items[1].u.number < best_due) {
        best = i;
        best_due = e->items[1].u.number;
      }
    }
    if (best < 0 || (!ahead && best_due > now_ms)) break;
    JsObject* entry = list->items[best].as_object();
    const Value fn = entry->items[0];
    const double interval = entry->items[3].is_number() ? entry->items[3].u.number : -1;
    if (ahead) {   // the clock the script sees moves to the time of the timer
      const double host_now = vm_.clock_ms() - vm_.skew();
      if (best_due > host_now + vm_.skew()) vm_.set_skew(best_due - host_now);
    }
    List<Value> call_args(allocator_);
    for (uint32_t i = 4; i < entry->length; ++i) call_args.push_back(entry->items[i]);
    if (interval >= 0) entry->items[1] = Value::make_number((ahead ? best_due : now_ms) + interval);   // an interval comes round again
    else list->items[best] = Value();
    Value level;
    vm_.get_named(Value::make_object(entry), "n", &level);
    vm_.define_named(vm_.global(), "__timer_level", level.is_number() ? level : Value::make_number(0), 0);   // (the timers this one starts are one level deeper)
    failed += call_guarded(fn, call_args.count() ? &call_args[0] : nullptr, static_cast<uint32_t>(call_args.count()), error);
    vm_.define_named(vm_.global(), "__timer_level", Value::make_number(0), 0);
  }
  // forget the timers that have run (the array would otherwise only grow)
  uint32_t live = 0;
  for (uint32_t i = 0; i < list->length; ++i)
    if (list->items[i].is_object()) list->items[live++] = list->items[i];
  list->length = live;   // (the entries that moved up are not left behind: they would run again)
  sync_document();
  return failed;
}

uint32_t Engine::fire_event(StringView type, String* error) {
  uint32_t failed = 0;
  on_big_stack([&] { failed = fire_event_here(type, error); });
  return failed;
}

uint32_t Engine::fire_event_here(StringView type, String* error) {
  uint32_t failed = 0;
  Value document;
  if (vm_.get_named(Value::make_object(vm_.global()), "document", &document) && document.is_object()) vm_.define_named(document.as_object(), "readyState", Value::make_string(vm_.new_string(type.equals("load") ? "complete" : "interactive")));
  JsObject* event = vm_.new_plain();
  vm_.define_named(event, "type", Value::make_string(vm_.new_string(type)));
  const Value args[1] = {Value::make_object(event)};
  Value list_value;
  if (vm_.get_named(Value::make_object(vm_.global()), "__listeners", &list_value) && list_value.is_object()) {
    JsObject* list = list_value.as_object();
    for (uint32_t i = 0; i < list->length; ++i) {   // (listeners added while this runs are run too)
      if (!list->items[i].is_object() || list->items[i].as_object()->length < 2) continue;
      JsObject* entry = list->items[i].as_object();
      if (entry->items[0].is_string() && entry->items[0].as_string()->view().equals(type)) failed += call_guarded(entry->items[1], args, 1, error);
    }
  }
  String handler_name(allocator_);
  handler_name.append("on");
  handler_name.append(type.equals("DOMContentLoaded") ? StringView("DOMContentLoaded") : type);
  Value handler;
  if (vm_.get_named(Value::make_object(vm_.global()), handler_name.view(), &handler) && Vm::is_callable(handler)) failed += call_guarded(handler, args, 1, error);
  sync_document();
  return failed;
}

}  // namespace script
