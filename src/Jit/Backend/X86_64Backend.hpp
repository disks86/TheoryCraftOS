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

// Which convention the generated code uses to take its state pointer and to call helpers: System V (rdi, rsi, rdx) or Microsoft x64 (rcx, rdx, r8 and
// 32 bytes of shadow space). The kernel is built for the Microsoft one (a PE image), hosted test builds on Linux use System V.
enum class X86CallingConvention : uint8_t { SysV, Win64 };

constexpr X86CallingConvention native_x86_calling_convention() {
#if defined(_WIN32)
  return X86CallingConvention::Win64;
#else
  return X86CallingConvention::SysV;
#endif
}

// The x86-64 back end. Virtual registers are placed by the linear-scan allocator (Ir/RegisterAllocator): r12-r15 (callee-saved, for values that live across
// a helper call) and r10/r11 (caller-saved) hold them; the rest spill to the frame. rax, rcx and rdx are scratch, rbx holds the state pointer. Operations
// compute in rax and write the result to the destination's register or slot. `max_registers` limits the pool (0: everything in memory), for testing.
class X86_64Backend final : public IJitBackend {
 public:
  explicit X86_64Backend(allocators::IAllocator& allocator, X86CallingConvention convention = native_x86_calling_convention(), uint32_t max_registers = 32)
      : allocator_(allocator), convention_(convention), max_registers_(max_registers) {}
  HostArch arch() const override { return HostArch::X86_64; }
  bool compile(const IrBlock& block, CodeBuffer& out) override;

 private:
  allocators::IAllocator& allocator_;
  X86CallingConvention convention_;
  uint32_t max_registers_;
};

}  // namespace jit
