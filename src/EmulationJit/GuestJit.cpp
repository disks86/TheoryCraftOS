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
#include "EmulationJit/GuestJit.hpp"
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/RiscV64Cpu.hpp"
#include "Emulation/X86_64Cpu.hpp"
#include "EmulationJit/AArch64Jit.hpp"
#include "EmulationJit/RiscV64Jit.hpp"
#include "EmulationJit/X86_64Jit.hpp"

namespace emulation {

void enable_jit(IGuestCpu& cpu, allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options) {
  // (arch() says what the object is: create_guest_cpu is the only maker of IGuestCpus, and it makes one class per architecture)
  switch (cpu.arch()) {
    case process::UserArch::X86_64: static_cast<X86_64Cpu&>(cpu).set_jit(create_x86_jit(allocator, code_memory, options)); break;
    case process::UserArch::AArch64: static_cast<AArch64Cpu&>(cpu).set_jit(create_aarch64_jit(allocator, code_memory, options)); break;
    case process::UserArch::RiscV64: static_cast<RiscV64Cpu&>(cpu).set_jit(create_riscv64_jit(allocator, code_memory, options)); break;
    default: break;
  }
}

}  // namespace emulation
