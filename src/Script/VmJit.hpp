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
#pragma once
#include <stdint.h>
#include "Jit/JitCompiler.hpp"
#include "Jit/JitRuntime.hpp"
#include "Script/Vm.hpp"

namespace script {

// Compiles the bytecode of hot script code to machine code with the shared JIT (src/Jit) and runs it inside the interpreter loop (see Vm::execute).
//
// A region is the code reachable from a point where the interpreter offers to hand over (the entry of a function, after a call returned, the target of a backward
// jump, or the instruction after one the compiled code leaves to the interpreter). The operand stack stays in the VM's own stack, so the collector sees every value, a
// helper can be called anywhere and leaving a region needs nothing written back; the temporaries of one bytecode are virtual registers. What is compiled:
//   * constants, stack shuffles, variables (the environment is reached through pointers), `this`;
//   * arithmetic, bitwise and comparison operators on numbers (doubles) inline; every other operand type, and the operators `in` / `instanceof`, through a helper that does
//     what the interpreter does;
//   * the truthiness tests of every jump; reads and writes of array elements inline for a dense array and an integer index in range, `array.length`;
//   * property reads and writes, global reads and method lookups through helpers (the same Vm functions the interpreter calls).
// Everything else (calls, returns, object and array literals, closures, try, iteration, await ...) ends the region: the interpreter runs that instruction and offers the
// next point again. Compiled code never throws: a helper that fails makes the region exit with the exception pending and the interpreter unwinds as usual.
//
// Exit value: bits 0..31 the instruction to continue at, 32..55 the number of operands on the stack, bit 60 set when a loop ran for a long time (the instruction budget is
// charged and the region entered again), bit 61 set when an exception is pending (the instruction is the one after the failing one).
constexpr uint64_t kScriptExitTick = 1ull << 60;
constexpr uint64_t kScriptExitException = 1ull << 61;
constexpr uint32_t kScriptExitPc(uint64_t exit) { return static_cast<uint32_t>(exit); }
constexpr uint32_t kScriptExitHeight(uint64_t exit) { return static_cast<uint32_t>((exit >> 32) & 0xFFFFFFu); }

class VmJit final : public jit::IJitFrontend {
 public:
  explicit VmJit(Vm& vm) : vm_(vm) {}
  VmJit(const VmJit&) = delete;
  VmJit& operator=(const VmJit&) = delete;

  void set_enabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }
  // How many times a point is reached before its region is compiled.
  void set_threshold(uint32_t visits) { threshold_ = visits; }

  // At instruction `pc` of the top frame (the operands are the stack above its base): runs compiled code for the region if there is any (compiling it when the point has
  // become hot). True: `*exit` says where to continue (sp_ is not updated: the caller sets it from the height in the exit value).
  bool enter(uint32_t pc, uint64_t* exit);

  uint64_t compiled_regions() const { return runtime_ ? runtime_->compiled() : 0; }
  uint64_t entries() const { return runtime_ ? runtime_->runs() : 0; }

  // IJitFrontend: translates the region that starts at the instruction in the low 32 bits of `key`, of the function given to enter().
  bool translate(uint64_t key, jit::IrBuilder& builder, uint64_t* guest_size) override;

  // The helpers compiled code calls (public so that the translator in VmJit.cpp can name them; they are `uint64_t(void* state, uint64_t vm, uint64_t packed)`).
  static uint64_t helper_binary(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_unary(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_truthy(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_get_prop(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_set_prop(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_get_elem(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_set_elem(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_get_global(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_get_method(void* state, uint64_t vm, uint64_t packed);
  static uint64_t helper_get_method_elem(void* state, uint64_t vm, uint64_t packed);

 private:
  bool eligible(FunctionCode& code);

  Vm& vm_;
  UniquePointer<jit::JitRuntime> runtime_;
  bool enabled_ = true;
  bool tried_ = false;
  uint32_t threshold_ = 40;
  uint32_t next_id_ = 1;
  FunctionCode* code_ = nullptr;   // what translate() works on
  uint32_t height_ = 0;
};

}  // namespace script
