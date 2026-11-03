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
#include "Jit/Backend/IJitBackend.hpp"

namespace jit {

// The LoongArch (LA64) back end, the counterpart of X86_64Backend, AArch64Backend and RiscV64Backend. Virtual registers are placed by the linear-scan
// allocator (Ir/RegisterAllocator): s1-s8 (callee-saved, for values that live across a helper call) and t3-t8, a3-a7 (caller-saved) hold them; the rest
// spill to the frame. t0-t2 and a0-a2 are scratch, s0 holds the state pointer. Helpers are called through t2 with the standard calling convention (state,
// a, b in a0..a2; the result in a0). Offsets too large for a 12-bit immediate (big frames, far state words) go through t2. `max_registers` limits the
// pool (0: everything in memory), for testing.
//
// Limits: a jump to a label or to the epilogue must be within +-128 MiB (B); conditional branches are emitted as "inverse branch over a B", so they
// share that reach. A front end ends a block long before that.
class LoongArch64Backend final : public IJitBackend {
 public:
  explicit LoongArch64Backend(allocators::IAllocator& allocator, uint32_t max_registers = 32) : allocator_(allocator), max_registers_(max_registers) {}
  HostArch arch() const override { return HostArch::LoongArch64; }
  bool compile(const IrBlock& block, CodeBuffer& out) override;

 private:
  allocators::IAllocator& allocator_;
  uint32_t max_registers_;
};

}  // namespace jit
