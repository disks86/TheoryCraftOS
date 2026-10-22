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
  var nextId = 1;
  // The key of a value in a Map or Set: primitives by type and text, objects by an id they carry (SameValueZero: NaN is NaN, -0 is 0).
  function keyOf(map, value) {
    var t = typeof value;
    if (t === 'string') return 's' + value;
    if (t === 'number') return 'n' + value;
    if (t === 'boolean') return value ? 'bt' : 'bf';
    if (value === undefined) return 'u';
    if (value === null) return 'l';
    if (t === 'symbol') return 'y' + String(value);
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

  // Array.from / Array.of
  if (!Array.from) def(Array, 'from', function (source, mapFn, thisArg) {
    if (source === null || source === undefined) throw new TypeError(String(source) + ' is not iterable');
    if (mapFn !== undefined && typeof mapFn !== 'function') throw new TypeError(String(mapFn) + ' is not a function');
    var out = [], i = 0;
    if (typeof source !== 'string' && typeof source[ITER] === 'function') {
      for (var v of source) { out.push(mapFn ? mapFn.call(thisArg, v, i) : v); i++; }
    } else {
      var n = source.length >>> 0;
      for (; i < n; i++) out.push(mapFn ? mapFn.call(thisArg, source[i], i) : source[i]);
    }
    return out;
  });
  if (!Array.of) def(Array, 'of', function () { var out = []; for (var i = 0; i < arguments.length; i++) out.push(arguments[i]); return out; });
  // Object helpers and Reflect
  if (!Object.is) def(Object, 'is', function (a, b) { return a === b ? (a !== 0 || 1 / a === 1 / b) : (a !== a && b !== b); });
  if (!Object.fromEntries) def(Object, 'fromEntries', function (iterable) { var o = {}; for (var e of iterable) o[e[0]] = e[1]; return o; });
  var Reflect = {
    apply: function (f, self, args) { return f.apply(self, args); },
    construct: function (F, args) {
      var a = args || [];
      switch (a.length) {
        case 0: return new F();
        case 1: return new F(a[0]);
        case 2: return new F(a[0], a[1]);
        case 3: return new F(a[0], a[1], a[2]);
        case 4: return new F(a[0], a[1], a[2], a[3]);
        case 5: return new F(a[0], a[1], a[2], a[3], a[4]);
        default: return new F(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
      }
    },
    defineProperty: function (o, k, d) { try { Object.defineProperty(o, k, d); return true; } catch (e) { return false; } },
    deleteProperty: function (o, k) { return delete o[k]; },
    get: function (o, k) { return o[k]; },
    set: function (o, k, v) { o[k] = v; return true; },
    has: function (o, k) { return k in o; },
    getPrototypeOf: function (o) { return Object.getPrototypeOf(o); },
    ownKeys: function (o) { return Object.keys(o); },
    isExtensible: function (o) { return !Object.isFrozen(o); }
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
    run_here(kPrelude, &error, nullptr, nullptr, false);
    run_here(kCollections, &error, nullptr, nullptr, false);
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
  on_big_stack([&] { ok = run_here(source, error, nullptr, nullptr, false); });
  return ok;
}

bool Engine::evaluate(StringView source, String* error, Value* value) {
  bool ok = false;
  on_big_stack([&] { ok = run_here(source, error, nullptr, value); });
  return ok;
}

bool Engine::run_here(StringView source, String* error, String* result, Value* out_value, bool sync) {
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
  if (!compile_program(vm_, program, &code, &message)) {
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
  vm_.recover();
  Value ignored;
  if (vm_.call(fn, Value::make_object(vm_.global()), args, argc, &ignored)) return 0;
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
    failed += call_guarded(fn, call_args.count() ? &call_args[0] : nullptr, static_cast<uint32_t>(call_args.count()), error);
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
