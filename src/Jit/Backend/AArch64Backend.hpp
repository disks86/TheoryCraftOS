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

// The AArch64 back end, the counterpart of X86_64Backend. Virtual registers are placed by the linear-scan allocator (Ir/RegisterAllocator): x20-x28
// (callee-saved, for values that live across a helper call) and x9-x15 (caller-saved) hold them; the rest spill to the frame. x0-x2 and x16 are scratch,
// x19 holds the state pointer. Helpers are called through x16 with the AAPCS64 convention (state, a, b in x0..x2; the result in x0), which is also the
// convention of the UEFI/PE ABI this kernel uses on AArch64. `max_registers` limits the pool (0: everything in memory), for testing.
//
// Limits: at most 4095 spill slots per block (the slot offset is a scaled 12-bit immediate; a front end ends a block long before that).
class AArch64Backend final : public IJitBackend {
 public:
  explicit AArch64Backend(allocators::IAllocator& allocator, uint32_t max_registers = 32) : allocator_(allocator), max_registers_(max_registers) {}
  HostArch arch() const override { return HostArch::AArch64; }
  bool compile(const IrBlock& block, CodeBuffer& out) override;

 private:
  allocators::IAllocator& allocator_;
  uint32_t max_registers_;
};

}  // namespace jit
