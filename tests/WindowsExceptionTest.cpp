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
  CHECK(fault_to_exception(3, 0, 0, &at, &e) && e.code == 0x80000003u && at.rip == 0x2000 && e.address == 0x2000);      // reported at the INT3
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
  CHECK(apply_context(p, at, &c) && (c.rflags & 0x3000) == 0 && !(c.rflags & 0x4000) && !(c.rflags & 0x20000) && !(c.rflags & 0x10000) && !(c.rflags & 0x40000) && !(c.rflags & 0x100));
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
    CHECK((c.rflags & ~uint64_t{0x0CD5 | 0x202}) == 0);
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

int main() {
  test_mapping();
  test_dispatch();
  test_apply();
  test_flips();
  test_services();
  if (g_failed) { printf("WindowsExceptionTest: %d failed\n", g_failed); return 1; }
  printf("WindowsExceptionTest: all passed\n");
  return 0;
}
