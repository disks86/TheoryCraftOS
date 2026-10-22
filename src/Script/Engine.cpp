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

}  // namespace

Engine::Engine(allocators::IAllocator& allocator, IHost* host) : allocator_(allocator), host_(host), vm_(allocator, host) {
  vm_.set_memory_limit(memory_);
  vm_.set_instruction_budget(budget_);
  String error(allocator_);
  on_big_stack([&] { run_here(kPrelude, &error, nullptr, nullptr, false); });
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
