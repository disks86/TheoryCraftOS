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
#include "Emulation/GuestCpu.hpp"
#include "Jit/Backend/AArch64Backend.hpp"
#include "Jit/Backend/RiscV64Backend.hpp"
#include "Jit/Backend/X86_64Backend.hpp"
#include "Jit/JitCompiler.hpp"

namespace emulation {

// The part of a guest JIT that does not depend on the guest: the translation cache, the hotness counters, the back end for the CPU the kernel runs on and the
// dispatcher. A guest brings its front end (the decoder that fills an IrBlock) and the layout of its register State. One core per guest address space; the guest
// threads take turns, so it is not entered concurrently (the cache itself could be shared by several cores: it is RCU-published).
class JitCore {
 public:
  JitCore(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, uint32_t hot_threshold, jit::IJitFrontend& frontend)
      : cache_(allocator, 4096), hotness_(allocator, 4096, hot_threshold), x86_(allocator), aarch64_(allocator), riscv64_(allocator),
        compiler_(allocator, code_memory, pick_backend(), cache_), dispatcher_(compiler_, cache_, hotness_, frontend) {}
  JitCore(const JitCore&) = delete;
  JitCore& operator=(const JitCore&) = delete;
  bool ok() const { return cache_.ok() && hotness_.ok(); }

  // Runs the translated block at `pc` against the register state (translating it first if it just became hot). False if there is no translated code for it yet: the
  // caller interprets. `layout_generation` is GuestMemory::layout_generation(): when it moved, everything translated before is dropped.
  bool run(uint64_t pc, ByteView state, uint64_t layout_generation) {
    if (layout_generation != generation_) {
      cache_.flush();
      hotness_.reset_all();
      generation_ = layout_generation;
    }
    uint64_t exit_value = 0;
    if (!dispatcher_.try_run(pc, state, &exit_value)) return false;
    ++entered_;
    return true;
  }
  uint64_t blocks_entered() const { return entered_; }

 private:
  jit::IJitBackend& pick_backend() {
    switch (jit::native_host_arch()) {
      case jit::HostArch::X86_64: return x86_;
      case jit::HostArch::AArch64: return aarch64_;
      default: return riscv64_;
    }
  }

  jit::CodeCache cache_;
  jit::HotnessTracker hotness_;
  jit::X86_64Backend x86_;
  jit::AArch64Backend aarch64_;
  jit::RiscV64Backend riscv64_;
  jit::JitCompiler compiler_;
  jit::JitDispatcher dispatcher_;
  uint64_t generation_ = 0;
  uint64_t entered_ = 0;
};

// The block's view of the register state: the address of the State is the ABI of the generated code (it is handed to the block as its state pointer).
template <class State>
ByteView state_bytes(State& s) {
  return ByteView(reinterpret_cast<uint8_t*>(&s), sizeof(State));
}

}  // namespace emulation
