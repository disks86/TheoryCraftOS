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

// The script engine: a small JavaScript implementation made of a parser, a compiler to bytecode and a virtual machine (src/Script/). It reads a practical
// subset of ECMAScript 5 with some of ES2015 - `let` and `const`, arrow functions, template literals, spread, default and rest parameters, for-of, ?? and
// ?. - with closures, prototypes and `new`, exceptions (try / catch / finally), and the usual built-ins (Object, Array, String, Number, Math, JSON,
// Date in part, Error). Not there: classes, regular expressions, destructuring, generators, async functions, Map / Set / Symbol / Promise, getters and
// setters, `with` and `eval`. Strings are bytes: the page text is ASCII and other characters pass through unchanged.
//
//   script::Engine engine(allocator, &host);
//   String error(allocator), result(allocator);
//   if (!engine.run("var x = [1, 2, 3].map(n => n * 2); x.join('-')", &error, &result)) show(error);   // result: "2-4-6"
//
// Globals persist from one run() to the next, so the scripts of a page share them. A script is stopped (with an error) when it runs more than the
// instruction budget or uses more than the memory limit; the engine is then usable again for the next script.
#pragma once
#include "Script/Vm.hpp"

namespace script {

class Engine {
 public:
  Engine(allocators::IAllocator& allocator, IHost* host);

  bool ok() const { return vm_.ok(); }
  Vm& vm() { return vm_; }
  void set_limits(uint64_t instructions, uint64_t memory_bytes);

  // Compiles and runs a script. False with *error ("TypeError: x is not a function", "SyntaxError: ... (line 3)") if it could not be compiled or
  // threw. `result` (may be null) gets the value of the last expression statement as text.
  bool run(StringView source, String* error, String* result = nullptr);

  // Compiles and runs `source` and hands back the value of its last expression (not kept alive: store it somewhere the script engine can see it at once).
  bool evaluate(StringView source, String* error, Value* value);

  // Runs what setTimeout / setInterval were given (in the order they are due, without waiting; at most `max` of them) and the listeners of an event
  // ("DOMContentLoaded", "load"; also onload). Returns how many threw; *error gets the first message.
  uint32_t run_timers(uint32_t max, String* error);
  // Runs the timers that are due at `now_ms` of the host's clock (an interval that comes round again waits for the next call). Returns how many threw.
  uint32_t run_due_timers(double now_ms, uint32_t max, String* error);
  // When the first timer waiting is due (host clock milliseconds); false if there is none.
  bool next_timer(double* due_ms);
  uint32_t fire_event(StringView type, String* error);

 private:
  bool run_here(StringView source, String* error, String* result, Value* value = nullptr);
  uint32_t run_timers_here(double now_ms, bool ahead, uint32_t max, String* error);
  uint32_t fire_event_here(StringView type, String* error);
  void sync_document();
  uint32_t call_guarded(const Value& fn, const Value* args, uint32_t argc, String* error);

  allocators::IAllocator& allocator_;
  IHost* host_;
  Vm vm_;
  uint64_t budget_ = 20000000, memory_ = 64u << 20;
};

}  // namespace script
