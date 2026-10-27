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
#include "Executable/ExecutableKind.hpp"
#include "Process/Syscall.hpp"

namespace process {

// The CPU this kernel was built for: the only guest that can run directly in user mode. Any other is run by the in-kernel emulator (src/Emulation).
constexpr UserArch native_arch() {
#if defined(__x86_64__)
  return UserArch::X86_64;
#elif defined(__aarch64__)
  return UserArch::AArch64;
#elif defined(__riscv) && __riscv_xlen == 64
  return UserArch::RiscV64;
#else
  return UserArch::None;
#endif
}

// The guest CPU a header names; None for the 32-bit and unknown machines (no interface, no emulator).
constexpr UserArch user_arch_for(exe::Architecture arch) {
  switch (arch) {
    case exe::Architecture::X86_64: return UserArch::X86_64;
    case exe::Architecture::AArch64: return UserArch::AArch64;
    case exe::Architecture::RiscV64: return UserArch::RiscV64;
    default: return UserArch::None;
  }
}

// A program for `guest` cannot use the CPU's own user mode and needs the emulator.
constexpr bool needs_emulation(UserArch guest) { return guest != UserArch::None && guest != native_arch(); }

}  // namespace process
