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
#include "Jit/Ir/Ir.hpp"
#include "Utility/ArrayView.hpp"

namespace jit {

// The registers a back end offers for virtual registers, numbered 0..callee_saved+caller_saved-1: the callee-saved ones first. A virtual register that
// lives across a Helper call (which clobbers the caller-saved registers) can only be given a callee-saved one; the others may use either, and prefer a
// caller-saved one because a callee-saved register has to be saved and restored by the block. At most 32 in all.
struct RegisterPool {
  uint32_t callee_saved = 0;
  uint32_t caller_saved = 0;
};

struct VRegLocation {
  enum class Kind : uint8_t { Unused, Register, Spill };
  Kind kind = Kind::Unused;
  uint32_t index = 0;   // pool register number, or spill slot number
};

// Linear-scan register allocation over an IrBlock (Poletto & Sarkar), one location per virtual register for its whole life (no splitting):
//  - A virtual register's interval runs from its first reference (or from the entry, see below) to its last, widened over every loop it overlaps, i.e. to
//    the jump that closes the loop, so a value that has to survive the back edge does.
//  - Registers read 0 until written. A register whose first reference is a read, or whose first write can be jumped over, gets its interval from the entry
//    and zero_at_entry(): the back end clears it in the prologue.
//  - Two intervals may share a register when one ends where the other starts (an instruction reads its operands before it writes).
//  - When there are not enough registers, the interval that ends last is spilled to a stack slot.
class RegisterAllocation {
 public:
  explicit RegisterAllocation(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~RegisterAllocation();
  RegisterAllocation(const RegisterAllocation&) = delete;
  RegisterAllocation& operator=(const RegisterAllocation&) = delete;

  // `max_registers` limits the pool to its first registers (0: everything lives in memory), for testing the spill paths. False if memory ran out.
  bool allocate(const IrBlock& block, RegisterPool pool, uint32_t max_registers = 32);

  VRegLocation location(uint32_t vreg) const { return locations_[vreg]; }
  bool zero_at_entry(uint32_t vreg) const { return zero_[vreg] != 0; }
  uint32_t spill_slots() const { return spill_slots_; }
  // Bit n set: pool register n is used by some virtual register (so a callee-saved one must be saved).
  uint32_t used_registers() const { return used_; }
  bool has_calls() const { return has_calls_; }
  // The interval of a virtual register, as positions (instruction index + 1; 0 is the entry), for tests.
  uint64_t interval_start(uint32_t vreg) const { return start_[vreg]; }
  uint64_t interval_end(uint32_t vreg) const { return end_[vreg]; }
  bool crosses_call(uint32_t vreg) const { return crosses_[vreg] != 0; }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<VRegLocation> locations_;
  ArrayView<uint8_t> zero_;
  ArrayView<uint8_t> crosses_;
  ArrayView<uint64_t> start_;
  ArrayView<uint64_t> end_;
  uint32_t spill_slots_ = 0;
  uint32_t used_ = 0;
  bool has_calls_ = false;
};

}  // namespace jit
