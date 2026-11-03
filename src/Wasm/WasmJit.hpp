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
#include "Wasm/Wasm.hpp"

namespace wasm {

// Compiles WebAssembly functions to machine code with the shared JIT (src/Jit) and runs them next to the interpreter.
//
// The unit is the region of a function that can be reached from a point where the interpreter could hand over: the function's entry, the target of a backward
// branch, or the instruction after a call or after one the compiled code does not do. The operand stack and the locals of the region are kept in virtual
// registers (one per local and per stack position, so a join of two paths needs nothing), loaded from the frame the interpreter is running on when the region is
// entered and stored back when it is left. Integer and double precision operations, loads and stores of memory 0 (bounds checked in the generated code), globals,
// select and every kind of branch are compiled; a few single precision and counting operations go through small native helpers.
//
// Anything else ends the region: the values are written back and the exit value says which instruction the interpreter continues with. That covers calls, memory.grow,
// SIMD, tables, bulk memory, exception handling, returns, and every trap: a load that is out of bounds or a division by zero is left to the interpreter, which executes
// the same instruction on the same stack and reports the trap itself. Compiled code never calls back into the machine, so it needs no stack frames of its own.
//
// Exit value: bits 0..31 the instruction to continue at, 32..59 the number of operands on the stack, bit 60 set when a loop ran for a long time (the host's
// tick must be called and the region entered again), bit 61 set when a call from the region failed (see JitCallSite), bit 62 set to return from the function.
constexpr uint64_t kJitExitTick = 1ull << 60;
constexpr uint64_t kJitExitReturn = 1ull << 62;  // the function is done: its results are the top of the stack (what a tail call from compiled code ends in)
constexpr uint64_t kJitExitFault = 1ull << 61;   // a call made from compiled code failed: the machine has what (trap, host error, exception) and the instruction is the one after the call
constexpr uint32_t kJitExitPc(uint64_t exit) { return static_cast<uint32_t>(exit); }
constexpr uint32_t kJitExitHeight(uint64_t exit) { return static_cast<uint32_t>((exit >> 32) & 0x0FFFFFFFu); }

// A call made from compiled code: the machine runs the callee in place (its frame starts at the arguments, which are already in the caller's stack slots) and the
// results are left where the caller expects them. The helper answers 0 when the callee returned, 1 when the call is better left to the interpreter (the nesting of native
// calls is limited, the table entry is not a function of the right type, the stack is full: the interpreter does the same call and reports what is wrong), 2 when the
// callee failed (the machine keeps the status; the region leaves with kJitExitFault).
struct JitCallSite {
  void* machine = nullptr;
  FuncInst* callee = nullptr;   // a direct call; null for call_indirect
  Table* table = nullptr;       // call_indirect: the table and the type the callee must have
  const FuncSig* sig = nullptr;
  uint32_t args_slot = 0;       // the slot, counted from the caller's first local, of the first argument
};
using JitCallHelper = uint64_t (*)(void* state, uint64_t site, uint64_t index);

class WasmJit final : public jit::IJitFrontend {
 public:
  explicit WasmJit(allocators::IAllocator& allocator) : allocator_(allocator), sites_(allocator) {}
  WasmJit(const WasmJit&) = delete;
  WasmJit& operator=(const WasmJit&) = delete;

  // Where compiled code calls other functions (the machine's helper and the machine it belongs to): without them calls end a region.
  void set_call_helper(JitCallHelper helper, void* machine) {
    call_helper_ = helper;
    machine_ = machine;
  }
  JitCallSite* new_call_site();
  JitCallHelper call_helper() const { return call_helper_; }
  void* machine() const { return machine_; }

  // Off: enter() always says no. The default is on when the kernel has executable code memory.
  void set_enabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }
  // How many times a point is reached before its region is compiled.
  void set_threshold(uint32_t visits) { threshold_ = visits; }

  // At instruction `pc` of `fn` with `height` operands above the locals, `fp` being the first local of the running frame: runs compiled code for the region if
  // there is any (compiling it first when the point has become hot). True: the frame was updated and `*exit` says where to continue. False: interpret.
  bool enter(FuncInst& fn, uint32_t pc, uint32_t height, Slot* fp, uint64_t* exit);

  uint64_t compiled_regions() const { return runtime_ ? runtime_->compiled() : 0; }
  uint64_t entries() const { return runtime_ ? runtime_->runs() : 0; }

  // IJitFrontend: translates the region of the function given to enter() that starts at the instruction in the low 32 bits of `key`.
  bool translate(uint64_t key, jit::IrBuilder& builder, uint64_t* guest_size) override;

 private:
  bool eligible(FuncInst& fn);

  allocators::IAllocator& allocator_;
  UniquePointer<jit::JitRuntime> runtime_;
  bool enabled_ = true;
  bool tried_ = false;   // (a runtime was asked for: there is none when the kernel has no executable memory, and asking again would not change that)
  uint32_t threshold_ = 40;
  uint32_t next_id_ = 1;
  JitCallHelper call_helper_ = nullptr;
  void* machine_ = nullptr;
  List<UniquePointer<JitCallSite>> sites_;   // (kept for as long as the compiled code that points at them)
  FuncInst* fn_ = nullptr;   // what translate() works on
  uint32_t height_ = 0;
};

}  // namespace wasm
