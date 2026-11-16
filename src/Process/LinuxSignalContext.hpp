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
#include "Process/Syscall.hpp"

namespace process::linuxabi {

// The registers of an AArch64 or RISC-V thread, as far as a signal handler's frame holds them (an x86-64 thread's are an X86Context). Whoever runs such a
// program (the emulator) fills one in, lets linuxabi::deliver_signals / signal_return / deliver_fault change it, and loads it back.
struct LinuxSignalContext {
  UserArch arch = UserArch::None;      // AArch64 or RiscV64
  // AArch64: x0..x30 in [0..30]. RISC-V: x1..x31 in [1..31] (ra, sp, gp, tp, ...; [0], the zero register, is not used).
  uint64_t x[32] = {};
  uint64_t sp = 0;          // AArch64 only (a RISC-V thread's stack pointer is x[2])
  uint64_t pc = 0;
  uint64_t pstate = 0;      // AArch64 only: N, Z, C, V in bits 31..28
  // AArch64: V0..V31, each as its low then its high half ([2n], [2n + 1]). RISC-V: f0..f31 in [0..31].
  uint64_t vector[64] = {};
  uint32_t fpcr = 0, fpsr = 0;      // AArch64; a RISC-V thread's fcsr is in fpcr

  uint64_t stack_pointer() const { return arch == UserArch::AArch64 ? sp : x[2]; }
  void set_stack_pointer(uint64_t value) { (arch == UserArch::AArch64 ? sp : x[2]) = value; }
  // The register a system call's first argument and its result share: x0, or a0.
  uint64_t& result() { return arch == UserArch::AArch64 ? x[0] : x[10]; }
};

}  // namespace process::linuxabi
