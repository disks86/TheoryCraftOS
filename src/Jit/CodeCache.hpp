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
#include "Jit/CodeMemory.hpp"
#include "Kernel/Rcu.hpp"
#include "Utility/ArrayView.hpp"

namespace jit {

struct CompiledBlock;

// Frees a block that RCU has retired: gives its code memory back, then the block itself.
struct CompiledBlockDeleter {
  void operator()(CompiledBlock* block) const;
};

// One block of generated code and what it was made from. Immutable once published; replaced, never modified.
struct CompiledBlock : kernel::RcuObjectBase<CompiledBlock, CompiledBlockDeleter> {
  uint64_t guest_start = 0;    // first guest address (or bytecode offset) the block covers
  uint64_t guest_size = 0;     // bytes covered, for invalidation when the guest overwrites its own code
  JitEntry entry = nullptr;    // where to call it (inside `region`)
  CodeRegion region;           // the code memory, executable and read-only
  uint64_t code_size = 0;      // bytes of code actually used
  uint32_t tier = 0;           // 0 = baseline; a higher tier replaced a lower one
  ICodeMemory* memory = nullptr;
  allocators::IAllocator* allocator = nullptr;
};

// The translation cache: guest address -> compiled block. Read-mostly (every guest branch looks something up; a block is built once), so it is
// published with RCU (CLAUDE.md, "Concurrent data structures"): lookup() never blocks or writes, and the code of a block that is replaced, invalidated or
// evicted is released only after a grace period, i.e. when no thread can still be inside it or about to enter it.
//
// Direct-mapped: the guest address picks a slot, and publishing a block evicts what was there (the evicted block is simply recompiled when it is hot
// again). So the cache is bounded by construction and a lookup is one load and one compare.
//
// Rules for the thread that runs compiled code: hold a kernel::RcuReader from the lookup until the block returns, and keep blocks short (they
// end at the next branch, or after a bounded number of guest instructions) so a grace period is never held up for long.
class CodeCache {
 public:
  // `slots` is rounded up to a power of two (at least 16).
  CodeCache(allocators::IAllocator& allocator, uint64_t slots, kernel::RcuDomain& domain = kernel::rcu_default_domain());
  ~CodeCache();
  CodeCache(const CodeCache&) = delete;
  CodeCache& operator=(const CodeCache&) = delete;
  bool ok() const { return !slots_.empty(); }

  // The block for `guest_start`, or null. The caller must be inside a kernel::RcuReader for the same domain, and must not use the block after
  // leaving it.
  const CompiledBlock* lookup(uint64_t guest_start) const;

  // Takes ownership of `block` and makes it visible. Evicts (retires) whatever was in its slot. Safe from any number of writers.
  void publish(CompiledBlock* block);

  // Removes every block whose guest range overlaps [start, start+size): the guest wrote to code it had translated, or unmapped it. Returns how many.
  uint64_t invalidate(uint64_t start, uint64_t size);
  // Removes everything (the guest flushed its instruction cache, or the code memory is under pressure).
  uint64_t flush();

  uint64_t slot_count() const { return slots_.size(); }
  // Blocks currently published (approximate while other threads write).
  uint64_t block_count() const { return __atomic_load_n(&count_, __ATOMIC_RELAXED); }
  uint64_t hits() const { return __atomic_load_n(&hits_, __ATOMIC_RELAXED); }
  uint64_t misses() const { return __atomic_load_n(&misses_, __ATOMIC_RELAXED); }

 private:
  uint64_t slot_of(uint64_t guest_start) const;
  void lock() {
    while (__atomic_exchange_n(&writer_, 1, __ATOMIC_ACQUIRE)) kernel::cpu_relax();
  }
  void unlock() { __atomic_store_n(&writer_, 0, __ATOMIC_RELEASE); }

  allocators::IAllocator& allocator_;
  kernel::RcuDomain& domain_;
  ArrayView<CompiledBlock*> slots_;
  uint32_t shift_ = 0;
  // Serialises writers only (publish / invalidate / flush); held for a few pointer swaps and never across a compile. A plain flag, not a
  // SpinLock, because it must work where interrupts cannot be masked (hosted tests) and is never taken from an interrupt handler.
  uint32_t writer_ = 0;
  mutable uint64_t hits_ = 0;
  mutable uint64_t misses_ = 0;
  uint64_t count_ = 0;
};

// Counts how often each guest address is reached, to decide what is worth compiling. Approximate by design: counters are shared by addresses that hash
// alike, are updated without ordering, and a collision only compiles something a little early.
class HotnessTracker {
 public:
  // `slots` is rounded up to a power of two (at least 16); a block is hot after `threshold` visits.
  HotnessTracker(allocators::IAllocator& allocator, uint64_t slots, uint32_t threshold);
  ~HotnessTracker();
  HotnessTracker(const HotnessTracker&) = delete;
  HotnessTracker& operator=(const HotnessTracker&) = delete;
  bool ok() const { return !counters_.empty(); }

  // Counts a visit. True exactly once per crossing of the threshold (the visit that reaches it), so only one thread starts the compile.
  bool record(uint64_t guest_start);
  uint32_t count(uint64_t guest_start) const;
  void reset(uint64_t guest_start);
  void reset_all();

 private:
  uint64_t slot_of(uint64_t guest_start) const;
  allocators::IAllocator& allocator_;
  ArrayView<uint32_t> counters_;
  uint32_t shift_ = 0;
  uint32_t threshold_;
};

}  // namespace jit
