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
#include "Allocators/IAllocator.hpp"
#include "Jit/Backend/IJitBackend.hpp"
#include "Jit/CodeCache.hpp"
#include "Jit/CodeMemory.hpp"
#include "Jit/Ir/Ir.hpp"

// The pieces a JIT client plugs together:
//
//   guest code --IJitFrontend--> IrBlock --IJitBackend--> machine code --ICodeMemory--> CompiledBlock --CodeCache--> run
//
// A front end belongs to a guest (an x86-64 / AArch64 / RISC-V decoder in src/Emulation, the bytecode of src/Script or src/Wasm). The back end belongs
// to the host CPU. JitCompiler runs the pipeline and publishes the result; JitDispatcher is what an interpreter loop calls at every block
// boundary to run compiled code when there is some, count visits when there is not, and compile what turned hot.
namespace jit {

// Translates the guest code that starts at `guest_pc` into IR: one basic block, ending at the first branch, call, system call or other instruction
// that cannot continue in the block (or after a bounded number of instructions). The block's Exit value is what the dispatcher gets back, typically the next
// guest address. A front end reads guest memory only through the bounds-checked accessors its owner gave it.
class IJitFrontend {
 public:
  // Builds the block with `builder` and sets *guest_size to the bytes of guest code it covers (for invalidation). False if the code at guest_pc cannot be
  // translated (unreadable, an instruction the front end does not do): the caller keeps interpreting it.
  virtual bool translate(uint64_t guest_pc, IrBuilder& builder, uint64_t* guest_size) = 0;

 protected:
  constexpr IJitFrontend() = default;
  ~IJitFrontend() = default;
};

enum class CompileResult : uint8_t {
  Compiled,         // published in the cache
  Untranslatable,   // the front end refused, or produced IR that does not validate
  BackendFailed,    // the back end could not encode the block
  OutOfMemory,
  NotExecutable,    // the code memory cannot be executed (HeapCodeMemory): nothing was published
};

class JitCompiler {
 public:
  JitCompiler(allocators::IAllocator& allocator, ICodeMemory& memory, IJitBackend& backend, CodeCache& cache)
      : allocator_(allocator), memory_(memory), backend_(backend), cache_(cache) {}

  // Front end + validation + back end, into `out`; no code memory is involved. For inspecting what would be generated.
  CompileResult generate(IJitFrontend& frontend, uint64_t guest_pc, CodeBuffer& out, uint64_t* guest_size);
  // generate(), copy into code memory, make executable, publish in the cache (replacing a block for the same address).
  CompileResult compile(IJitFrontend& frontend, uint64_t guest_pc, uint32_t tier = 0);

  uint64_t compiled() const { return __atomic_load_n(&compiled_, __ATOMIC_RELAXED); }
  uint64_t failed() const { return __atomic_load_n(&failed_, __ATOMIC_RELAXED); }

 private:
  allocators::IAllocator& allocator_;
  ICodeMemory& memory_;
  IJitBackend& backend_;
  CodeCache& cache_;
  uint64_t compiled_ = 0;
  uint64_t failed_ = 0;
};

// Runs one block through compiled code: the block must stay alive for the call (inside the RcuReader it was looked up in). The one place that
// calls generated code through a function pointer.
uint64_t run_block(const CompiledBlock& block, ByteView state);

class JitDispatcher {
 public:
  JitDispatcher(JitCompiler& compiler, CodeCache& cache, HotnessTracker& hotness, IJitFrontend& frontend)
      : compiler_(compiler), cache_(cache), hotness_(hotness), frontend_(frontend) {}

  // At a block boundary: if compiled code exists for `guest_pc`, runs it against `state` and returns true with its exit value. Otherwise counts the
  // visit, compiles the block if it just became hot and runs it, or returns false (the caller interprets this block and calls again at the next one).
  bool try_run(uint64_t guest_pc, ByteView state, uint64_t* exit_value);

  // The guest changed [start, start+size): drop every block built from it and start counting again.
  void invalidate(uint64_t start, uint64_t size);

 private:
  bool run_cached(uint64_t guest_pc, ByteView state, uint64_t* exit_value);
  JitCompiler& compiler_;
  CodeCache& cache_;
  HotnessTracker& hotness_;
  IJitFrontend& frontend_;
};

}  // namespace jit
