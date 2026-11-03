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
#include "Jit/Emitter/CodeBuffer.hpp"
#include "Jit/Ir/Ir.hpp"

namespace jit {

// The CPU a back end generates code for.
enum class HostArch : uint8_t { X86_64, AArch64, RiscV64, LoongArch64 };

// The CPU this kernel is built for (and so the only one whose output can be entered), None for a build that has no back end to run.
constexpr HostArch native_host_arch() {
#if defined(__x86_64__)
  return HostArch::X86_64;
#elif defined(__aarch64__)
  return HostArch::AArch64;
#elif defined(__loongarch64)
  return HostArch::LoongArch64;
#else
  return HostArch::RiscV64;
#endif
}

// The signature of a compiled block: `state` is the guest state block the IR's LoadState/StoreState address (8-byte aligned); the result is the
// block's exit value (Exit / ExitValue), usually the guest address to continue at or a stop code.
using JitEntry = uint64_t (*)(void* state);

// Lowers one validated IrBlock to machine code. The generated function follows the platform's C calling convention (JitEntry), so it
// can be called from C++ and calls Helpers the same way. A back end is stateless between calls and may be used from several threads.
class IJitBackend {
 public:
  virtual HostArch arch() const = 0;
  // Appends the code to `out` (starting at its current end, which must be suitably aligned; entry is at that offset) and finishes its labels.
  // False if the block uses something this back end cannot encode, or memory ran out.
  virtual bool compile(const IrBlock& block, CodeBuffer& out) = 0;

 protected:
  constexpr IJitBackend() = default;
  ~IJitBackend() = default;
};

}  // namespace jit
