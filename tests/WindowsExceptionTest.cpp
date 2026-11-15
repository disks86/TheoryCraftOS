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

// Host tests of the kernel's half of structured exception handling: faults as NT exceptions, the records pushed on the program's stack, and NtContinue's CONTEXT.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsException.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A stack region [kBase, kBase + size); everything else faults.
constexpr uint64_t kBase = 0x7FFF0000ull, kSize = 0x10000;
struct FakeMemory final : IUserMemory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize, 0);
  bool read(uint64_t a, ByteView out) override {
    if (a < kBase || a - kBase > bytes.size() || out.size() > bytes.size() - (a - kBase)) return false;
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = bytes[a - kBase + i];
    return true;
  }
  bool write(uint64_t a, ConstByteView in) override {
    if (a < kBase || a - kBase > bytes.size() || in.size() > bytes.size() - (a - kBase)) return false;
    for (uint64_t i = 0; i < in.size(); ++i) bytes[a - kBase + i] = in[i];
    return true;
  }
  uint64_t u64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[a - kBase + i]} << (8 * i); return v; }
  uint32_t u32(uint64_t a) { return uint32_t(u64(a)); }
  void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
};
struct Host final : IProcessHost {
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
};

static void test_mapping() {
  X86Context at;
  at.rip = 0x1000;
  ExceptionInfo e;
  CHECK(fault_to_exception(14, 0x2, 0x1234, &at, &e) && e.code == 0xC0000005u && e.count == 2 && e.information[0] == 1 && e.information[1] == 0x1234 && e.address == 0x1000);
  CHECK(fault_to_exception(14, 0x4, 0x10, &at, &e) && e.information[0] == 0 && e.information[1] == 0x10);
  CHECK(fault_to_exception(14, 0x14, 0x1000, &at, &e) && e.information[0] == 8);      // instruction fetch
  CHECK(fault_to_exception(0, 0, 0, &at, &e) && e.code == 0xC0000094u);
  CHECK(fault_to_exception(6, 0, 0, &at, &e) && e.code == 0xC000001Du);
  CHECK(fault_to_exception(13, 0, 0, &at, &e) && e.code == 0xC0000096u);
  at.rip = 0x2001;
  CHECK(fault_to_exception(3, 0, 0, &at, &e) && e.code == 0x80000003u && at.rip == 0x2001 && e.address == 0x2000);      // reported at the INT3; the context stays after it
  CHECK(!fault_to_exception(8, 0, 0, &at, &e));      // double fault
  CHECK(!fault_to_exception(18, 0, 0, &at, &e));     // machine check
  at.fpu[24] = 0x04;      // MXCSR: the divide-by-zero flag
  CHECK(fault_to_exception(19, 0, 0, &at, &e) && e.code == 0xC000008Eu);
}

static void test_dispatch() {
  FakeMemory mem;
  Host host;
  Process p(1, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, mem, host);
  X86Context at, entry;
  for (int i = 0; i < 16; ++i) at.gpr[i] = 0x1000 + uint64_t(i);
  at.gpr[X86Context::kRsp] = kBase + kSize - 0x100;
  at.rip = 0x401234;
  at.rflags = 0x246;
  at.fpu[24] = 0x80; at.fpu[25] = 0x1F;      // MXCSR 0x1F80
  at.fpu[160] = 0x77;                        // XMM0
  ExceptionInfo info;
  info.code = 0xC0000005u;
  info.address = 0x401234;
  info.count = 2;
  info.information[0] = 1;
  info.information[1] = 0xDEAD;
  CHECK(dispatch_exception(p, at, info, 0x180001000, &entry));
  CHECK(entry.rip == 0x180001000 && (entry.gpr[X86Context::kRsp] & 15) == 0);
  const uint64_t frame = entry.gpr[X86Context::kRsp], record = entry.gpr[X86Context::kRcx], context = entry.gpr[X86Context::kRdx];
  CHECK(context == frame + 0x40 && record == context + kContextBytes && record % 16 == 0 && context % 16 == 0);
  // the machine frame: rip, cs, eflags, rsp, ss
  CHECK(mem.u64(frame) == 0x401234 && mem.u64(frame + 16) == 0x246 && mem.u64(frame + 24) == kBase + kSize - 0x100);
  // the record
  CHECK(mem.u32(record) == 0xC0000005u && mem.u64(record + 0x10) == 0x401234 && mem.u32(record + 0x18) == 2 && mem.u64(record + 0x20) == 1 && mem.u64(record + 0x28) == 0xDEAD);
  // the context: flags, rax, rsp, rip, xmm0
  CHECK((mem.u32(context + 0x30) & 0x100000) && mem.u64(context + 0x78) == 0x1000 && mem.u64(context + 0x98) == kBase + kSize - 0x100 && mem.u64(context + 0xF8) == 0x401234);
  CHECK(mem.u32(context + 0x34) == 0x1F80 && mem.bytes[context + 0x1A0 - kBase] == 0x77);
  CHECK(record + kExceptionRecordBytes <= kBase + kSize - 0x100);      // everything is below the faulting stack pointer

  // a stack too small, or none: nothing is written, the program ends
  X86Context low = at;
  low.gpr[X86Context::kRsp] = kBase + 0x100;
  CHECK(!dispatch_exception(p, low, info, 0x180001000, &entry));
  low.gpr[X86Context::kRsp] = 0;
  CHECK(!dispatch_exception(p, low, info, 0x180001000, &entry));
  low.gpr[X86Context::kRsp] = 0xFFFF800000000000ull;
  CHECK(!dispatch_exception(p, low, info, 0x180001000, &entry));
  CHECK(!dispatch_exception(p, at, info, 0, &entry));      // no dispatcher
}

// Builds a CONTEXT in `mem` at `at`.
static void build_context(FakeMemory& mem, uint64_t at, uint32_t flags, uint64_t rip, uint64_t rsp, uint64_t rax, uint32_t eflags, uint32_t mxcsr) {
  for (uint32_t i = 0; i < kContextBytes; ++i) mem.bytes[at - kBase + i] = 0;
  mem.put32(at + 0x30, flags);
  mem.put32(at + 0x34, mxcsr);
  mem.put32(at + 0x44, eflags);
  mem.put64(at + 0x78, rax);
  mem.put64(at + 0x98, rsp);
  mem.put64(at + 0xA0, 0xBBBB);      // rbp
  mem.put64(at + 0xF8, rip);
  mem.put32(at + 0x118, mxcsr);
}

static void test_apply() {
  FakeMemory mem;
  Host host;
  Process p(1, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, mem, host);
  const uint64_t at = kBase + 0x1000;
  X86Context c;
  for (int i = 0; i < 16; ++i) c.gpr[i] = 0x55;
  c.rip = 0x111;
  // a full context
  build_context(mem, at, 0x10000B, 0x401000, 0x7FFF8000, 77, 0x246, 0x1F80);
  CHECK(apply_context(p, at, &c) && c.rip == 0x401000 && c.gpr[X86Context::kRsp] == 0x7FFF8000 && c.gpr[X86Context::kRax] == 77 && c.gpr[X86Context::kRbp] == 0xBBBB);
  CHECK((c.rflags & 0x200) && (c.rflags & 0x3000) == 0 && (c.rflags & 0x40));      // IF on, IOPL off, ZF as asked
  // only the control part: the general registers are left alone
  c = X86Context();
  for (int i = 0; i < 16; ++i) c.gpr[i] = 0x55;
  build_context(mem, at, 0x100001, 0x402000, 0x7FFF9000, 99, 0x246, 0x1F80);
  CHECK(apply_context(p, at, &c) && c.rip == 0x402000 && c.gpr[X86Context::kRax] == 0x55 && c.gpr[X86Context::kRsp] == 0x7FFF9000);
  // privileged flag bits are masked: IOPL 3, NT, VM, RF, AC, TF
  build_context(mem, at, 0x10000B, 0x401000, 0x7FFF8000, 1, 0xFFFFFFFFu, 0x1F80);
  CHECK(apply_context(p, at, &c) && (c.rflags & 0x3000) == 0 && !(c.rflags & 0x4000) && !(c.rflags & 0x20000) && !(c.rflags & 0x10000) && !(c.rflags & 0x40000));      // (the trap flag is a program's to set: single-stepping)
  // a rip or rsp that is not a user address would fault the kernel in IRET: refused
  build_context(mem, at, 0x10000B, 0xFFFF800000001000ull, 0x7FFF8000, 1, 0x246, 0x1F80);
  CHECK(!apply_context(p, at, &c));
  build_context(mem, at, 0x10000B, 0x401000, 0xFFFF800000001000ull, 1, 0x246, 0x1F80);
  CHECK(!apply_context(p, at, &c));
  // reserved MXCSR bits would fault FXRSTOR: masked
  build_context(mem, at, 0x10000B, 0x401000, 0x7FFF8000, 1, 0x246, 0xFFFFFFFFu);
  CHECK(apply_context(p, at, &c) && c.fpu[26] == 0 && c.fpu[27] == 0 && c.fpu[24] == 0xBF);
  // not an x64 context
  build_context(mem, at, 0x10000B & ~0x100000u, 0x401000, 0x7FFF8000, 1, 0x246, 0x1F80);
  CHECK(!apply_context(p, at, &c));
  // truncated: the CONTEXT runs off the end of the memory
  build_context(mem, kBase + kSize - kContextBytes, 0x10000B, 0x401000, 0x7FFF8000, 1, 0x246, 0x1F80);
  CHECK(apply_context(p, kBase + kSize - kContextBytes, &c));
  CHECK(!apply_context(p, kBase + kSize - kContextBytes + 1, &c));
  CHECK(!apply_context(p, 0x1000, &c));
}

// Bit flips of a good context must never make the kernel accept something that would fault it.
static void test_flips() {
  FakeMemory mem;
  Host host;
  Process p(1, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, mem, host);
  const uint64_t at = kBase + 0x1000;
  for (uint32_t bit = 0; bit < kContextBytes * 8; ++bit) {
    build_context(mem, at, 0x10000B, 0x401000, 0x7FFF8000, 1, 0x246, 0x1F80);
    mem.bytes[at - kBase + bit / 8] ^= uint8_t(1u << (bit % 8));
    X86Context c;
    if (!apply_context(p, at, &c)) continue;
    CHECK(c.rip < 0x0000800000000000ull && c.gpr[X86Context::kRsp] < 0x0000800000000000ull);
    CHECK((c.rflags & ~uint64_t{0x0DD5 | 0x202}) == 0);
    uint32_t mxcsr = uint32_t(c.fpu[24]) | uint32_t(c.fpu[25]) << 8 | uint32_t(c.fpu[26]) << 16 | uint32_t(c.fpu[27]) << 24;
    CHECK((mxcsr & ~0xFFBFu) == 0);
  }
}

static void test_services() {
  FakeMemory mem;
  Host host;
  Process p(1, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, mem, host);
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  const uint64_t at = kBase + 0x1000;
  build_context(mem, at, 0x10000B, 0x401000, 0x7FFF8000, 1, 0x246, 0x1F80);
  SyscallRequest r;
  r.number = uint64_t(WindowsSyscallNumber::NtContinue);
  r.args[0] = at;
  CHECK(iface->dispatch(p, r) == 0);
  uint64_t where = 0;
  CHECK(p.take_context_restore(&where) && where == at && !p.take_context_restore(&where));
  r.args[0] = 0x1000;      // not mapped
  CHECK(iface->dispatch(p, r) == 0xC0000005u);
  CHECK(!p.take_context_restore(&where));
  // NtRaiseException is the last chance: the process ends with the exception code
  mem.put32(kBase + 0x2000, 0xE06D7363u);
  r.number = uint64_t(WindowsSyscallNumber::NtRaiseException);
  r.args[0] = kBase + 0x2000;
  CHECK(iface->dispatch(p, r) == 0 && p.exited() && p.exit_code() == 0xE06D7363u);
  p.set_exception_dispatcher(0x180001000);
  CHECK(p.exception_dispatcher() == 0x180001000);
}

// ---- a 32-bit program (i386): its own CONTEXT (0x2CC bytes) and EXCEPTION_RECORD (0x50 bytes) ----
static ProcessBinding binding32() { return ProcessBinding{SyscallAbi::Windows, UserArch::X86_32, syscall_interface(SyscallAbi::Windows, UserArch::X86_32)}; }
static X86Context registers32() {
  X86Context at;
  for (int i = 0; i < 8; ++i) at.gpr[i] = 0x1000 + uint64_t(i);
  at.gpr[X86Context::kRsp] = kBase + kSize - 0x104;
  at.rip = 0x401234;
  at.rflags = 0x246;
  at.fpu[0] = 0x7F; at.fpu[1] = 0x03;        // the x87 control word 0x037F
  at.fpu[2] = 0x00; at.fpu[3] = 0x38;        // status: top of stack = 7
  at.fpu[4] = 0x80;                          // the register in use is the physical one 7, which is ST(0)
  at.fpu[32 + 7] = 0xA0; at.fpu[32 + 8] = 0x00; at.fpu[32 + 9] = 0x40;      // ST(0) = 2.5
  at.fpu[24] = 0x80; at.fpu[25] = 0x1F;      // MXCSR 0x1F80
  at.fpu[160] = 0x77;                        // XMM0
  return at;
}

static void test_dispatch32() {
  FakeMemory mem;
  Host host;
  Process p(1, binding32(), mem, host);
  const X86Context at = registers32();
  X86Context entry;
  ExceptionInfo info;
  info.code = 0xC0000005u;
  info.address = 0x401234;
  info.count = 2;
  info.information[0] = 1;
  info.information[1] = 0x20;
  CHECK(dispatch_exception(p, at, info, 0x70001000, &entry));
  const uint64_t sp = entry.gpr[X86Context::kRsp];
  CHECK(entry.rip == 0x70001000 && (sp & 15) == 0 && sp < at.gpr[X86Context::kRsp] && sp >= kBase);
  const uint64_t record = mem.u32(sp), context = mem.u32(sp + 4);      // the two arguments, as a call leaves them less its return address
  CHECK(record == sp + 8 && context == record + kExceptionRecordBytes32 && context + kContextBytes32 <= at.gpr[X86Context::kRsp]);
  CHECK(mem.u32(record) == 0xC0000005u && mem.u32(record + 4) == 0 && mem.u32(record + 8) == 0 && mem.u32(record + 12) == 0x401234 && mem.u32(record + 16) == 2 &&
        mem.u32(record + 20) == 1 && mem.u32(record + 24) == 0x20);
  CHECK(mem.u32(context) == 0x1002F);      // i386: control, integer, segments, floating point, extended registers
  CHECK(mem.u32(context + 0x9C) == 0x1007 && mem.u32(context + 0xA0) == 0x1006 && mem.u32(context + 0xA4) == 0x1003 && mem.u32(context + 0xA8) == 0x1002 &&
        mem.u32(context + 0xAC) == 0x1001 && mem.u32(context + 0xB0) == 0x1000);      // Edi Esi Ebx Edx Ecx Eax
  CHECK(mem.u32(context + 0xB4) == 0x1005 && mem.u32(context + 0xB8) == 0x401234 && mem.u32(context + 0xBC) == 0x3B && mem.u32(context + 0xC0) == 0x246 &&
        mem.u32(context + 0xC4) == uint32_t(at.gpr[X86Context::kRsp]) && mem.u32(context + 0xC8) == 0x2B);      // Ebp Eip SegCs EFlags Esp SegSs
  // FloatSave: the control word, the status word, the full tag word (register 7 valid, the others empty), then the registers, ten bytes each
  CHECK(mem.u32(context + 0x1C) == 0xFFFF037Fu && mem.u32(context + 0x20) == 0xFFFF3800u && mem.u32(context + 0x24) == 0xFFFF3FFFu);
  CHECK(mem.bytes[context + 0x1C + 28 + 7 - kBase] == 0xA0 && mem.bytes[context + 0x1C + 28 + 9 - kBase] == 0x40);
  CHECK(mem.bytes[context + 0xCC + 160 - kBase] == 0x77 && mem.bytes[context + 0xCC + 24 - kBase] == 0x80);      // the FXSAVE image: XMM0, MXCSR
  // a breakpoint: the CONTEXT is at the INT3 itself
  info.code = 0x80000003u;
  info.address = 0x401233;
  info.count = 1;
  CHECK(dispatch_exception(p, at, info, 0x70001000, &entry));
  CHECK(mem.u32(mem.u32(entry.gpr[X86Context::kRsp] + 4) + 0xB8) == 0x401233);
  // no room on the stack, a stack that is not a 32-bit program's, or no dispatcher: not delivered
  X86Context bad = at;
  bad.gpr[X86Context::kRsp] = kBase + 0x100;
  CHECK(!dispatch_exception(p, bad, info, 0x70001000, &entry));
  bad.gpr[X86Context::kRsp] = 0x1000;
  CHECK(!dispatch_exception(p, bad, info, 0x70001000, &entry));
  bad.gpr[X86Context::kRsp] = 0x80000100ull;
  CHECK(!dispatch_exception(p, bad, info, 0x70001000, &entry));
  CHECK(!dispatch_exception(p, at, info, 0, &entry));
  info.count = 16;
  CHECK(!dispatch_exception(p, at, info, 0x70001000, &entry));
}

static void test_context32() {
  FakeMemory mem;
  Host host;
  Process p(1, binding32(), mem, host);
  const uint64_t at = kBase + 0x1000;
  const X86Context from = registers32();
  // GetThreadContext writes the parts the flags ask for and leaves the rest of the record alone
  for (uint32_t i = 0; i < kContextBytes32; ++i) mem.bytes[at - kBase + i] = 0xEE;
  mem.put32(at, 0x10001);      // control only
  CHECK(store_context(p, at, from));
  CHECK(mem.u32(at + 0xB8) == 0x401234 && mem.u32(at + 0xC4) == uint32_t(from.gpr[X86Context::kRsp]) && mem.u32(at + 0xB4) == 0x1005);
  CHECK(mem.u32(at + 0xB0) == 0xEEEEEEEEu && mem.u32(at + 0x1C) == 0xEEEEEEEEu && mem.u32(at + 0xCC) == 0xEEEEEEEEu);
  mem.put32(at, 0x1002F);
  CHECK(store_context(p, at, from));
  // and SetThreadContext / NtContinue reads it back: the same registers
  X86Context c;
  for (uint64_t& value : c.gpr) value = 0xDEADBEEF00000000ull;
  CHECK(apply_context(p, at, &c));
  for (int i = 0; i < 8; ++i) CHECK(c.gpr[i] == from.gpr[i]);
  CHECK(c.rip == 0x401234 && c.rflags == 0x246 && c.fpu[160] == 0x77 && c.fpu[24] == 0x80 && c.fpu[25] == 0x1F);
  // the x87 state from the FloatSave area alone (no extended registers): the SSE registers stay what they were
  mem.put32(at, 0x10008);
  X86Context x87;
  x87.fpu[160] = 0x99;
  x87.fpu[24] = 0x80; x87.fpu[25] = 0x1F;
  CHECK(apply_context(p, at, &x87));
  CHECK(x87.fpu[0] == 0x7F && x87.fpu[1] == 0x03 && x87.fpu[3] == 0x38 && x87.fpu[4] == 0x80 && x87.fpu[32 + 7] == 0xA0 && x87.fpu[32 + 9] == 0x40 && x87.fpu[160] == 0x99 &&
        x87.fpu[24] == 0x80);
  // a program cannot give itself privileged flags, or addresses that are not a 32-bit program's
  mem.put32(at, 0x10001);
  mem.put32(at + 0xC0, 0x00243246u | 0x100);      // IOPL 3, NT, VM, AC, TF
  CHECK(apply_context(p, at, &c) && (c.rflags & 0x3000) == 0 && !(c.rflags & 0x4000) && !(c.rflags & 0x20000) && !(c.rflags & 0x40000) && (c.rflags & 0x200));
  mem.put32(at + 0xB8, 0x80000000u);
  CHECK(!apply_context(p, at, &c));
  mem.put32(at + 0xB8, 0x401000);
  mem.put32(at + 0xC4, 0xFFFFF000u);
  CHECK(!apply_context(p, at, &c));
  mem.put32(at + 0xC4, 0x7FFF8000u);
  CHECK(apply_context(p, at, &c));
  mem.put32(at, 0x10000B);      // an x64 CONTEXT's flags: not this program's
  CHECK(!apply_context(p, at, &c) && !store_context(p, at, from));
  // MXCSR's reserved bits (loading them would fault in the kernel)
  mem.put32(at, 0x10020);
  mem.put32(at + 0xCC + 24, 0xFFFFFFFFu);
  CHECK(apply_context(p, at, &c) && c.fpu[26] == 0 && c.fpu[27] == 0 && c.fpu[24] == 0xBF);
  // truncated: a CONTEXT that runs off the end of the mapped memory
  mem.put32(kBase + kSize - kContextBytes32, 0x10001);
  mem.put32(kBase + kSize - kContextBytes32 + 0xB8, 0x401000);
  mem.put32(kBase + kSize - kContextBytes32 + 0xC4, 0x7FFF8000u);
  CHECK(apply_context(p, kBase + kSize - kContextBytes32, &c));
  CHECK(!apply_context(p, kBase + kSize - kContextBytes32 + 1, &c) && !store_context(p, kBase + kSize - kContextBytes32 + 1, from));
  CHECK(!apply_context(p, 0x1000, &c));
}

// Bit flips of a good 32-bit context must never make the kernel accept something that would fault it.
static void test_flips32() {
  FakeMemory mem;
  Host host;
  Process p(1, binding32(), mem, host);
  const uint64_t at = kBase + 0x1000;
  const X86Context from = registers32();
  std::vector<uint8_t> good;
  mem.put32(at, 0x1002F);
  CHECK(store_context(p, at, from));
  good.assign(mem.bytes.begin() + 0x1000, mem.bytes.begin() + 0x1000 + kContextBytes32);
  for (uint32_t bit = 0; bit < kContextBytes32 * 8; ++bit) {
    for (uint32_t i = 0; i < kContextBytes32; ++i) mem.bytes[0x1000 + i] = good[i];
    mem.bytes[0x1000 + bit / 8] ^= uint8_t(1u << (bit % 8));
    X86Context c;
    if (!apply_context(p, at, &c)) continue;
    CHECK(c.rip < 0x80000000ull && c.gpr[X86Context::kRsp] < 0x80000000ull);
    for (int i = 0; i < 8; ++i) CHECK((c.gpr[i] >> 32) == 0);
    CHECK((c.rflags & ~uint64_t{0x0DD5 | 0x202}) == 0);
    const uint32_t mxcsr = uint32_t(c.fpu[24]) | uint32_t(c.fpu[25]) << 8 | uint32_t(c.fpu[26]) << 16 | uint32_t(c.fpu[27]) << 24;
    CHECK((mxcsr & ~0xFFBFu) == 0);
  }
}

int main() {
  test_mapping();
  test_dispatch();
  test_apply();
  test_flips();
  test_services();
  test_dispatch32();
  test_context32();
  test_flips32();
  if (g_failed) { printf("WindowsExceptionTest: %d failed\n", g_failed); return 1; }
  printf("WindowsExceptionTest: all passed\n");
  return 0;
}
