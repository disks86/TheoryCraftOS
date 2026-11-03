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
#include "Utility/ArrayView.hpp"

// Where generated machine code lives. A JIT writes code into memory that is writable and not executable, then asks for it to be
// switched to executable and not writable (W^X: no page is ever both), and the instruction cache is made coherent on the way.
// The kernel implements ICodeMemory on top of its page tables, a hosted build on mmap/mprotect (tests/JitHost.hpp), and
// HeapCodeMemory below on the plain allocator for code that is only produced, inspected and never run (cross-compiling, tests).
namespace jit {

// One page-aligned run of code memory. `bytes` is the whole region; the region is a handle, whoever got it from allocate() gives it back to release().
struct CodeRegion {
  ByteView bytes;
  bool empty() const { return bytes.empty(); }
};

class ICodeMemory {
 public:
  // Granularity of allocate(), make_executable() and make_writable().
  virtual uint64_t page_size() const = 0;
  // A new region of at least `size` bytes (rounded up to whole pages), writable and NOT executable. False when out of memory.
  virtual bool allocate(uint64_t size, CodeRegion* out) = 0;
  // Switches the region to executable and read-only and makes the instruction cache coherent with what was written. False if this
  // memory cannot be executed at all (then the code can be kept and looked at, but never entered).
  virtual bool make_executable(const CodeRegion& region) = 0;
  // Back to writable and not executable, for patching a block in place. Nothing may be running in the region.
  virtual bool make_writable(const CodeRegion& region) = 0;
  // Returns the region. Nothing may be running in it or about to enter it (see CodeCache: that is what the RCU grace period is for).
  virtual void release(const CodeRegion& region) = 0;
  // True when make_executable() can succeed, i.e. compiled code can be run here.
  virtual bool can_execute() const = 0;

 protected:
  constexpr ICodeMemory() = default;
  ~ICodeMemory() = default;
};

// Makes the instruction cache of the CALLING core coherent with the data cache for `code` (a compiler barrier on x86, DC CVAU / IC IVAU / ISB on Arm,
// FENCE.I on RISC-V). Implementations of make_executable() call it after the last write.
void flush_instruction_cache(ConstByteView code);

// The same for every other core. A core that may enter freshly written code has to be told to synchronise its own instruction fetch: RISC-V's FENCE.I and
// Arm's ISB only affect the core that executes them (Arm's cache maintenance by address is broadcast, but the pipelines of the other cores may still hold
// the old instructions). CrossCoreInstructionSync does it with an inter-processor interrupt. A code memory calls it after flush_instruction_cache(), before
// the block is published.
class IInstructionCacheSync {
 public:
  // False if some core did not answer (then the code must not be entered).
  virtual bool sync(ConstByteView code) = 0;

 protected:
  constexpr IInstructionCacheSync() = default;
  ~IInstructionCacheSync() = default;
};

// Code memory from the allocator. It is never executable: make_executable() fails, so a JitCompiler on top of it produces blocks that can be
// read back (to check the generated code against an assembler) but not entered.
class HeapCodeMemory final : public ICodeMemory {
 public:
  explicit HeapCodeMemory(allocators::IAllocator& allocator) : allocator_(allocator) {}
  uint64_t page_size() const override { return kPage; }
  bool allocate(uint64_t size, CodeRegion* out) override;
  bool make_executable(const CodeRegion&) override { return false; }
  bool make_writable(const CodeRegion&) override { return true; }
  void release(const CodeRegion& region) override;
  bool can_execute() const override { return false; }

 private:
  static constexpr uint64_t kPage = 4096;
  allocators::IAllocator& allocator_;
};

}  // namespace jit
