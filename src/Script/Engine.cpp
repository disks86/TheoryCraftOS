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

}  // namespace

Engine::Engine(allocators::IAllocator& allocator, IHost* host) : allocator_(allocator), host_(host), vm_(allocator, host) {
  vm_.set_memory_limit(memory_);
  vm_.set_instruction_budget(budget_);
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

bool Engine::run_here(StringView source, String* error, String* result) {
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
    sync_document();
    return false;
  }
  if (result) {
    result->clear();
    vm_.describe(value, result, 0);
  }
  sync_document();
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
  on_big_stack([&] { failed = run_timers_here(max, error); });
  return failed;
}

uint32_t Engine::run_timers_here(uint32_t max, String* error) {
  uint32_t failed = 0;
  Value list_value;
  if (!vm_.get_named(Value::make_object(vm_.global()), "__timers", &list_value) || !list_value.is_object()) return 0;
  JsObject* list = list_value.as_object();
  for (uint32_t n = 0; n < max; ++n) {
    // the entry that is due first (entries are [function, delay, id, args...]; delay is the time it is due)
    int64_t best = -1;
    double best_due = 0;
    for (uint32_t i = 0; i < list->length; ++i) {
      if (!list->items[i].is_object()) continue;
      JsObject* e = list->items[i].as_object();
      if (e->length < 3 || !e->items[1].is_number()) continue;
      if (best < 0 || e->items[1].u.number < best_due) {
        best = i;
        best_due = e->items[1].u.number;
      }
    }
    if (best < 0) break;
    JsObject* entry = list->items[best].as_object();
    const Value fn = entry->items[0];
    if (best_due > virtual_time_) virtual_time_ = best_due;
    List<Value> call_args(allocator_);
    for (uint32_t i = 3; i < entry->length; ++i) call_args.push_back(entry->items[i]);
    list->items[best] = Value();
    const uint32_t before = list->length;
    failed += call_guarded(fn, call_args.count() ? &call_args[0] : nullptr, static_cast<uint32_t>(call_args.count()), error);
    for (uint32_t i = before; i < list->length; ++i) {   // timers set by the callback are due after it
      if (list->items[i].is_object() && list->items[i].as_object()->length >= 2 && list->items[i].as_object()->items[1].is_number())
        list->items[i].as_object()->items[1].u.number += virtual_time_;
    }
  }
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
