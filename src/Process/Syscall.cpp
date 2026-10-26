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
#include "Process/Syscall.hpp"
#include "Process/Process.hpp"

namespace process {

const ISyscallInterface& linux_interface(UserArch arch);       // LinuxSyscalls.cpp
const ISyscallInterface& windows_interface();                  // WindowsSyscalls.cpp

StringView abi_name(SyscallAbi abi) {
  switch (abi) {
    case SyscallAbi::Linux: return "Linux";
    case SyscallAbi::Windows: return "Windows";
    default: return "";
  }
}

SyscallAbi abi_for_executable(exe::ExecutableKind kind) {
  switch (kind) {
    case exe::ExecutableKind::Elf: return SyscallAbi::Linux;
    case exe::ExecutableKind::Pe: return SyscallAbi::Windows;
    default: return SyscallAbi::None;
  }
}

const ISyscallInterface* syscall_interface(SyscallAbi abi, UserArch arch) {
  switch (abi) {
    case SyscallAbi::Linux:
      if (arch == UserArch::X86_64 || arch == UserArch::AArch64 || arch == UserArch::RiscV64) return &linux_interface(arch);
      return nullptr;
    case SyscallAbi::Windows: return arch == UserArch::X86_64 ? &windows_interface() : nullptr;
    default: return nullptr;
  }
}

namespace {
constexpr uint8_t kOsAbiSysV = 0, kOsAbiGnu = 3;
constexpr uint16_t kElfMachineX86_64 = 62, kElfMachineAArch64 = 183, kElfMachineRiscV = 243;
}  // namespace

bool bind_executable(const exe::ElfFile& elf, ProcessBinding* out) {
  if (!elf.is_64() || elf.data_encoding() != exe::ElfData::Little) return false;
  if (elf.os_abi() != kOsAbiSysV && elf.os_abi() != kOsAbiGnu) return false;      // a BSD or other-OS binary would call a different table
  if (elf.type() != exe::kElfExecutable && elf.type() != exe::kElfShared) return false;
  UserArch arch = UserArch::None;
  switch (elf.machine()) {
    case kElfMachineX86_64: arch = UserArch::X86_64; break;
    case kElfMachineAArch64: arch = UserArch::AArch64; break;
    case kElfMachineRiscV: arch = UserArch::RiscV64; break;
    default: return false;
  }
  const ISyscallInterface* interface = syscall_interface(SyscallAbi::Linux, arch);
  if (!interface) return false;
  *out = ProcessBinding{SyscallAbi::Linux, arch, interface};
  return true;
}

bool bind_executable(const exe::PeFile& pe, ProcessBinding* out) {
  if (pe.is_dll() || !pe.is_pe32_plus() || pe.machine() != exe::kPeMachineAmd64) return false;
  if (pe.subsystem() != exe::kPeSubsystemConsole && pe.subsystem() != exe::kPeSubsystemGui) return false;
  const ISyscallInterface* interface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  if (!interface) return false;
  *out = ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, interface};
  return true;
}

bool decode_x86_64(SyscallAbi abi, const X86UserRegisters& regs, SyscallRequest* out) {
  SyscallRequest request;
  request.user_stack = regs.rsp;
  if (abi == SyscallAbi::Linux) {
    request.number = regs.rax;
    request.args[0] = regs.rdi; request.args[1] = regs.rsi; request.args[2] = regs.rdx;
    request.args[3] = regs.r10; request.args[4] = regs.r8; request.args[5] = regs.r9;
  } else if (abi == SyscallAbi::Windows) {
    request.number = regs.rax & 0xFFFFFFFFu;       // the service number is the low 32 bits of EAX; the upper half is ignored
    request.args[0] = regs.r10; request.args[1] = regs.rdx; request.args[2] = regs.r8; request.args[3] = regs.r9;
  } else {
    return false;
  }
  *out = request;
  return true;
}

bool decode_aarch64_linux(ArrayView<const uint64_t> x, SyscallRequest* out) {
  if (x.size() < 31) return false;
  SyscallRequest request;
  request.number = x[8];
  for (uint32_t i = 0; i < 6; ++i) request.args[i] = x[i];
  *out = request;
  return true;
}

bool decode_riscv64_linux(ArrayView<const uint64_t> x, SyscallRequest* out) {
  if (x.size() < 32) return false;
  SyscallRequest request;
  request.number = x[17];
  for (uint32_t i = 0; i < 6; ++i) request.args[i] = x[10 + i];
  *out = request;
  return true;
}

}  // namespace process
