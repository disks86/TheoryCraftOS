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
#include "Utility/ArrayView.hpp"

namespace process {

class Process;

// Structured exception handling, the kernel's half: a fault of a Windows program becomes an EXCEPTION_RECORD and a CONTEXT on the program's own stack, and the thread
// continues in ntdll's KiUserExceptionDispatcher, which finds the handlers (the unwinder is in ntdll.dll, as on Windows). NtContinue puts a CONTEXT back.

// The registers of a thread in user mode (x86-64), as far as a CONTEXT has them.
struct X86Context {
  enum Register : uint32_t { kRax, kRcx, kRdx, kRbx, kRsp, kRbp, kRsi, kRdi, kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15 };
  uint64_t gpr[16] = {};
  uint64_t rip = 0;
  uint64_t rflags = 0x202;
  uint8_t fpu[512] = {};      // FXSAVE image (x87 and SSE state, MXCSR at byte 24)
};

constexpr uint32_t kContextBytes = 0x4D0;           // sizeof(CONTEXT) of x64
constexpr uint32_t kExceptionRecordBytes = 0x98;    // sizeof(EXCEPTION_RECORD)
constexpr uint32_t kContextAmd64 = 0x100000, kContextControl = 1, kContextInteger = 2, kContextSegments = 4, kContextFloatingPoint = 8;

constexpr uint32_t kStatusAccessViolation = 0xC0000005u;

struct ExceptionInfo {
  uint32_t code = 0;
  uint32_t flags = 0;
  uint64_t address = 0;
  uint32_t count = 0;
  uint64_t information[15] = {};
};

// What an x86 exception means as an NT exception (STATUS_ACCESS_VIOLATION for a page fault, ...). `error` is the CPU's error code, `cr2` the faulting address. A
// breakpoint is reported at the INT3 itself (the context is moved back one byte, as Windows does). False for an exception that cannot be delivered (machine check, ...).
bool fault_to_exception(uint64_t vector, uint64_t error, uint64_t cr2, X86Context* at, ExceptionInfo* out);

// Pushes the CONTEXT of `at` and the EXCEPTION_RECORD of `info` on the program's stack and sets up `*entry` to call dispatcher(record, context): ntdll's
// KiUserExceptionDispatcher. False if the stack cannot take them (the program has run out of stack: it ends).
bool dispatch_exception(Process& process, const X86Context& at, const ExceptionInfo& info, uint64_t dispatcher, X86Context* entry);

// NtContinue: loads what the CONTEXT at `address` (in the program's memory) holds, according to its ContextFlags, into `*context`. The values are made safe first (a
// program can ask for its own registers, not for the kernel's: canonical addresses, no privileged flags, only the usable bits of MXCSR). False if the CONTEXT cannot be
// read or is not an x64 one.
bool apply_context(Process& process, uint64_t address, X86Context* context);

}  // namespace process
