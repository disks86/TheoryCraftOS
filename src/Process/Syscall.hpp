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
#include "Executable/ElfFile.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Executable/PeFile.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace process {

// Which system-call interface a user-mode process speaks. It is decided once, when the executable is loaded, from the kind of file: an ELF program
// gets the Linux interface (the Linux system-call numbers and conventions of its CPU), a PE program gets the Windows interface (the NT native
// system services that ntdll.dll's stubs invoke). The two never mix inside one process.
enum class SyscallAbi : uint8_t { None, Linux, Windows };
// The CPU the program was built for; selects the register convention and, for Linux, the number table.
enum class UserArch : uint8_t { None, X86_64, AArch64, RiscV64 };

StringView abi_name(SyscallAbi abi);
// ELF -> Linux, PE -> Windows, anything else -> None.
SyscallAbi abi_for_executable(exe::ExecutableKind kind);

// One system call as the program made it, with the registers already sorted into the order of the ABI's arguments.
struct SyscallRequest {
  uint64_t number = 0;
  uint64_t args[6] = {};
  uint64_t user_stack = 0;    // the program's stack pointer at the call: the Windows ABI passes arguments past the fourth there
};

class Process;

// The system calls of one ABI. Stateless: all state is in the Process, so one instance serves every process of its ABI and CPU.
class ISyscallInterface {
 public:
  virtual SyscallAbi abi() const = 0;
  virtual UserArch arch() const = 0;
  // Runs the call and returns the value to put in the program's result register (Linux: the result or -errno; Windows: the NTSTATUS). Never
  // trusts an argument: user pointers go through the process's memory, which fails instead of faulting.
  virtual uint64_t dispatch(Process& process, const SyscallRequest& request) const = 0;

 protected:
  ISyscallInterface() = default;
  ~ISyscallInterface() = default;
};

// The interface for this ABI on this CPU; null where there is none (Windows is x86-64 only for now).
const ISyscallInterface* syscall_interface(SyscallAbi abi, UserArch arch);

// What loading an executable decides for its process.
struct ProcessBinding {
  SyscallAbi abi = SyscallAbi::None;
  UserArch arch = UserArch::None;
  const ISyscallInterface* interface = nullptr;
};

// Chooses the interface for a parsed executable. False (binding untouched) for a file this OS cannot give system calls to: a 32-bit or big-endian
// ELF, an ELF for a foreign OS, a DLL, or a machine without an interface.
bool bind_executable(const exe::ElfFile& elf, ProcessBinding* out);
bool bind_executable(const exe::PeFile& pe, ProcessBinding* out);

// --- Registers at the system-call instruction ---------------------------------------------------------------------------------------------------
// x86-64 SYSCALL. Both ABIs use RAX for the number and RAX for the result, but not for the arguments:
//   Linux:   rdi rsi rdx r10 r8 r9
//   Windows: r10 (the stub moves RCX there) rdx r8 r9, then the stack at rsp + 0x28, 0x30, ... (past the return address and the home space)
struct X86UserRegisters {
  uint64_t rax = 0, rcx = 0, rdx = 0, rbx = 0, rsp = 0, rbp = 0, rsi = 0, rdi = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;
};
bool decode_x86_64(SyscallAbi abi, const X86UserRegisters& regs, SyscallRequest* out);
// AArch64 SVC (Linux only): number in x8, arguments x0..x5. `x` holds x0..x30.
bool decode_aarch64_linux(ArrayView<const uint64_t> x, SyscallRequest* out);
// RISC-V ECALL (Linux only): number in a7 (x17), arguments a0..a5 (x10..x15). `x` holds x0..x31.
bool decode_riscv64_linux(ArrayView<const uint64_t> x, SyscallRequest* out);

}  // namespace process
