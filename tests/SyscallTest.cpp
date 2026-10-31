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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;
typedef std::vector<uint8_t> Bytes;

// One mapped region [kBase, kBase + size); everything else faults.
constexpr uint64_t kBase = 0x10000;
struct FakeMemory final : IUserMemory {
  Bytes bytes = Bytes(0x2000, 0);
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
  void put(uint64_t a, const std::string& s) { for (size_t i = 0; i < s.size(); ++i) bytes[a - kBase + i] = uint8_t(s[i]); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[a - kBase + i]} << (8 * i); return v; }
};

struct FakeHost final : IProcessHost {
  std::string out, err, input;
  uint64_t slept = 0, yields = 0, real = 1700000000ull * 1000000000ull + 5, mono = 42;
  bool write_console(uint32_t stream, ConstByteView d) override {
    std::string& s = stream == 2 ? err : out;
    for (uint8_t b : d) s.push_back(char(b));
    return true;
  }
  bool read_console(ByteView o, uint64_t* got) override {
    uint64_t n = input.size() < o.size() ? input.size() : o.size();
    for (uint64_t i = 0; i < n; ++i) o[i] = uint8_t(input[i]);
    input.erase(0, n);
    *got = n;
    return true;
  }
  uint64_t realtime_ns() override { return real; }
  uint64_t monotonic_ns() override { return mono; }
  void sleep_ns(uint64_t ns) override { slept += ns; }
  void yield() override { ++yields; }
};

static Bytes elf_header(uint16_t machine, uint8_t cls = 2, uint8_t data = 1, uint8_t osabi = 0, uint16_t type = 2) {
  Bytes b(64, 0);
  b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = cls; b[5] = data; b[6] = 1; b[7] = osabi;
  b[16] = uint8_t(type); b[18] = uint8_t(machine); b[19] = uint8_t(machine >> 8); b[20] = 1;
  b[52] = 64; b[54] = 56; b[58] = 64;
  return b;
}

static Bytes pe_header(uint16_t machine, uint16_t characteristics, uint16_t subsystem) {
  Bytes b(0x400, 0);
  auto u16 = [&](size_t o, uint16_t v) { b[o] = uint8_t(v); b[o + 1] = uint8_t(v >> 8); };
  auto u32 = [&](size_t o, uint32_t v) { u16(o, uint16_t(v)); u16(o + 2, uint16_t(v >> 16)); };
  b[0] = 'M'; b[1] = 'Z'; u32(0x3C, 0x40);
  b[0x40] = 'P'; b[0x41] = 'E';
  u16(0x44, machine); u16(0x46, 1); u16(0x54, 240); u16(0x56, characteristics);
  const size_t o = 0x58;
  u16(o, 0x20B); u32(o + 16, 0x1000); u32(o + 20, 0x1000); b[o + 24 + 4] = 1; u32(o + 32, 0x1000); u32(o + 36, 0x200);
  u32(o + 56, 0x2000); u32(o + 60, 0x200); u16(o + 68, subsystem); u32(o + 108, 16);
  const size_t s = 0x148;
  b[s] = '.'; b[s + 1] = 't'; u32(s + 8, 0x100); u32(s + 12, 0x1000); u32(s + 16, 0x200); u32(s + 20, 0x200);
  u32(s + 36, 0x60000020u);
  return b;
}

static ProcessBinding binding_for(SyscallAbi abi, UserArch arch) {
  return ProcessBinding{abi, arch, syscall_interface(abi, arch)};
}

static SyscallRequest req(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0) {
  SyscallRequest r;
  r.number = n; r.args[0] = a0; r.args[1] = a1; r.args[2] = a2;
  return r;
}
static int64_t sres(uint64_t v) { return int64_t(v); }

static void test_selection() {
  CHECK(abi_for_executable(exe::ExecutableKind::Elf) == SyscallAbi::Linux);
  CHECK(abi_for_executable(exe::ExecutableKind::Pe) == SyscallAbi::Windows);
  CHECK(abi_for_executable(exe::ExecutableKind::None) == SyscallAbi::None);

  ProcessBinding b;
  Bytes e = elf_header(62);
  exe::ElfFile elf(A);
  CHECK(elf.parse(ConstByteView(e.data(), e.size())));
  CHECK(bind_executable(elf, &b) && b.abi == SyscallAbi::Linux && b.arch == UserArch::X86_64 && b.interface);

  Bytes arm = elf_header(183, 2, 1, 3, 3);       // GNU ABI, position independent
  CHECK(elf.parse(ConstByteView(arm.data(), arm.size())));
  CHECK(bind_executable(elf, &b) && b.arch == UserArch::AArch64);
  Bytes rv = elf_header(243);
  CHECK(elf.parse(ConstByteView(rv.data(), rv.size())));
  CHECK(bind_executable(elf, &b) && b.arch == UserArch::RiscV64);

  b = ProcessBinding{};
  Bytes bsd = elf_header(62, 2, 1, 9);           // FreeBSD
  CHECK(elf.parse(ConstByteView(bsd.data(), bsd.size())));
  CHECK(!bind_executable(elf, &b) && b.abi == SyscallAbi::None);
  Bytes ppc = elf_header(20, 1, 2);
  if (elf.parse(ConstByteView(ppc.data(), ppc.size()))) CHECK(!bind_executable(elf, &b));
  Bytes mips = elf_header(8);
  CHECK(elf.parse(ConstByteView(mips.data(), mips.size())));
  CHECK(!bind_executable(elf, &b));

  exe::PeFile pe(A);
  Bytes p = pe_header(0x8664, 0x0002, 3);
  CHECK(pe.parse(ConstByteView(p.data(), p.size())));
  CHECK(bind_executable(pe, &b) && b.abi == SyscallAbi::Windows && b.arch == UserArch::X86_64 && b.interface);
  Bytes dll = pe_header(0x8664, 0x2002, 3);
  CHECK(pe.parse(ConstByteView(dll.data(), dll.size())));
  b = ProcessBinding{};
  CHECK(!bind_executable(pe, &b));
  Bytes arm_pe = pe_header(0xAA64, 0x0002, 3);
  if (pe.parse(ConstByteView(arm_pe.data(), arm_pe.size()))) CHECK(!bind_executable(pe, &b));
  Bytes efi = pe_header(0x8664, 0x0002, 10);
  CHECK(pe.parse(ConstByteView(efi.data(), efi.size())));
  CHECK(!bind_executable(pe, &b));

  CHECK(syscall_interface(SyscallAbi::Windows, UserArch::AArch64) == nullptr);
  CHECK(syscall_interface(SyscallAbi::None, UserArch::X86_64) == nullptr);
}

static void test_decode() {
  X86UserRegisters r;
  r.rax = 1; r.rdi = 10; r.rsi = 20; r.rdx = 30; r.r10 = 40; r.r8 = 50; r.r9 = 60; r.rcx = 999; r.rsp = 0x7000;
  SyscallRequest q;
  CHECK(decode_x86_64(SyscallAbi::Linux, r, &q));
  CHECK(q.number == 1 && q.args[0] == 10 && q.args[1] == 20 && q.args[2] == 30 && q.args[3] == 40 && q.args[4] == 50 && q.args[5] == 60);
  r.rax = 0xABCD00000001ull;
  CHECK(decode_x86_64(SyscallAbi::Windows, r, &q));
  CHECK(q.number == 1 && q.args[0] == 40 && q.args[1] == 30 && q.args[2] == 50 && q.args[3] == 60 && q.user_stack == 0x7000);
  CHECK(!decode_x86_64(SyscallAbi::None, r, &q));

  uint64_t x[32] = {};
  for (int i = 0; i < 32; ++i) x[i] = 100 + i;
  CHECK(decode_aarch64_linux(ArrayView<const uint64_t>(x, 31), &q) && q.number == 108 && q.args[0] == 100 && q.args[5] == 105);
  CHECK(decode_riscv64_linux(ArrayView<const uint64_t>(x, 32), &q) && q.number == 117 && q.args[0] == 110 && q.args[5] == 115);
  CHECK(!decode_aarch64_linux(ArrayView<const uint64_t>(x, 8), &q));
  CHECK(!decode_riscv64_linux(ArrayView<const uint64_t>(x, 17), &q));
}

static void test_linux() {
  FakeMemory mem; FakeHost host;
  Process p(7, binding_for(SyscallAbi::Linux, UserArch::X86_64), mem, host);
  mem.put(kBase + 0x100, "hello");
  CHECK(p.syscall(req(1, 1, kBase + 0x100, 5)) == 5 && host.out == "hello");
  CHECK(p.syscall(req(1, 2, kBase + 0x100, 2)) == 2 && host.err == "he");
  CHECK(sres(p.syscall(req(1, 1, 0xDEAD0000, 4))) == -14);              // EFAULT
  CHECK(sres(p.syscall(req(1, 1, kBase + 0x1FFE, 8))) == 2);             // partial: the mapped part is written
  CHECK(sres(p.syscall(req(1, 1, ~uint64_t{0} - 1, 16))) == -14);        // wraps the address space
  CHECK(sres(p.syscall(req(1, 9, kBase, 1))) == -9);                     // EBADF
  CHECK(sres(p.syscall(req(1, 0, kBase, 1))) == -9);                     // stdin is not writable
  CHECK(p.syscall(req(1, 1, kBase, 0)) == 0);
  // a huge count is bounded by the mapping, not by memory
  host.out.clear();
  CHECK(sres(p.syscall(req(1, 1, kBase, 0xFFFFFFFFFFFFull))) == 0x2000 && host.out.size() == 0x2000);

  host.input = "abcdef";
  CHECK(p.syscall(req(0, 0, kBase + 0x200, 4)) == 4 && mem.bytes[0x200] == 'a' && mem.bytes[0x203] == 'd');
  CHECK(p.syscall(req(0, 0, kBase + 0x200, 100)) == 2);
  CHECK(p.syscall(req(0, 0, kBase + 0x200, 100)) == 0);                  // end of input
  host.input = "x";
  CHECK(sres(p.syscall(req(0, 0, 0xDEAD0000, 1))) == -14);
  CHECK(sres(p.syscall(req(0, 1, kBase, 1))) == -9);

  // writev: two vectors
  mem.put(kBase + 0x300, "ab"); mem.put(kBase + 0x310, "cde");
  mem.put64(kBase + 0x400, kBase + 0x300); mem.put64(kBase + 0x408, 2);
  mem.put64(kBase + 0x410, kBase + 0x310); mem.put64(kBase + 0x418, 3);
  host.out.clear();
  CHECK(p.syscall(req(20, 1, kBase + 0x400, 2)) == 5 && host.out == "abcde");
  CHECK(sres(p.syscall(req(20, 1, kBase + 0x400, 5000))) == -22);        // more than IOV_MAX
  CHECK(sres(p.syscall(req(20, 1, 0xDEAD0000, 1))) == -14);
  mem.put64(kBase + 0x408, ~uint64_t{0});                                // an absurd length
  CHECK(sres(p.syscall(req(20, 1, kBase + 0x400, 1))) == -22);           // the sum would exceed what one call may move
  mem.put64(kBase + 0x408, 2);

  CHECK(p.syscall(req(39)) == 7 && p.syscall(req(186)) == 7 && p.syscall(req(102)) == 0 && p.syscall(req(104)) == 0);
  CHECK(p.syscall(req(24)) == 0 && host.yields == 1);
  CHECK(sres(p.syscall(req(9999))) == -38);                              // ENOSYS
  CHECK(sres(p.syscall(req(0xFFFFFFFFFFFFFFFFull))) == -38);

  // uname
  CHECK(p.syscall(req(63, kBase + 0x500)) == 0);
  CHECK(std::string((const char*)&mem.bytes[0x500]) == "Linux" && std::string((const char*)&mem.bytes[0x500 + 65 * 4]) == "x86_64");
  CHECK(sres(p.syscall(req(63, 0x10))) == -14);

  // clock_gettime
  CHECK(p.syscall(req(228, 0, kBase + 0x600)) == 0 && mem.get64(kBase + 0x600) == 1700000000 && mem.get64(kBase + 0x608) == 5);
  CHECK(p.syscall(req(228, 1, kBase + 0x600)) == 0 && mem.get64(kBase + 0x600) == 0 && mem.get64(kBase + 0x608) == 42);
  CHECK(sres(p.syscall(req(228, 77, kBase + 0x600))) == -22);
  CHECK(sres(p.syscall(req(228, 0, 0))) == -14);

  // nanosleep
  mem.put64(kBase + 0x700, 2); mem.put64(kBase + 0x708, 500);
  CHECK(p.syscall(req(35, kBase + 0x700, 0)) == 0 && host.slept == 2000000500ull);
  mem.put64(kBase + 0x708, 1000000000ull);
  CHECK(sres(p.syscall(req(35, kBase + 0x700, 0))) == -22);
  mem.put64(kBase + 0x700, ~uint64_t{0});
  mem.put64(kBase + 0x708, 0);
  CHECK(sres(p.syscall(req(35, kBase + 0x700, 0))) == -22);              // negative seconds
  mem.put64(kBase + 0x700, 0x7FFFFFFFFFFFFFFFull);
  CHECK(p.syscall(req(35, kBase + 0x700, 0)) == 0);                       // clamps instead of wrapping

  // close
  CHECK(p.syscall(req(3, 1)) == 0);
  CHECK(sres(p.syscall(req(1, 1, kBase + 0x100, 5))) == -9);
  CHECK(sres(p.syscall(req(3, 1))) == -9);
  CHECK(sres(p.syscall(req(3, 99))) == -9);

  CHECK(!p.exited());
  p.syscall(req(231, 0x1FF));
  CHECK(p.exited() && p.exit_code() == 0xFF);
  CHECK(sres(p.syscall(req(39))) == -38);                                 // nothing runs after exit
}

static void test_linux_generic_table() {
  FakeMemory mem; FakeHost host;
  Process p(3, binding_for(SyscallAbi::Linux, UserArch::AArch64), mem, host);
  mem.put(kBase, "hi");
  CHECK(p.syscall(req(64, 1, kBase, 2)) == 2 && host.out == "hi");        // write is 64 here, 1 on x86-64
  CHECK(sres(p.syscall(req(1, 1, kBase, 2))) == -38);                     // x86-64's number means nothing here
  CHECK(p.syscall(req(172)) == 3);
  CHECK(p.syscall(req(160, kBase + 0x100)) == 0 && std::string((const char*)&mem.bytes[0x100 + 65 * 4]) == "aarch64");
  CHECK(p.syscall(req(113, 0, kBase + 0x200)) == 0 && mem.get64(kBase + 0x200) == 1700000000);
  p.syscall(req(94, 3));
  CHECK(p.exited() && p.exit_code() == 3);

  FakeMemory mem2; FakeHost host2;
  Process r(4, binding_for(SyscallAbi::Linux, UserArch::RiscV64), mem2, host2);
  CHECK(r.syscall(req(172)) == 4);
  CHECK(std::string("riscv64").size() == 7 && r.syscall(req(160, kBase)) == 0 && std::string((const char*)&mem2.bytes[65 * 4]) == "riscv64");
}

static const uint32_t kNtSuccess = 0, kNtInvalidHandle = 0xC0000008u, kNtAccessViolation = 0xC0000005u, kNtEof = 0xC0000011u, kNtDenied = 0xC0000022u,
                      kNtNoService = 0xC000001Cu, kNtNotImpl = 0xC0000002u;
constexpr uint64_t kStack = kBase + 0x1000;

static SyscallRequest nt(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
  SyscallRequest r;
  r.number = uint64_t(n); r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.user_stack = kStack;
  return r;
}
static uint32_t st(uint64_t v) { return uint32_t(v); }

// Sets the five stack arguments of NtWriteFile/NtReadFile: IoStatusBlock, Buffer, Length, ByteOffset, Key.
static void stack_args(FakeMemory& mem, uint64_t io, uint64_t buffer, uint64_t length) {
  mem.put64(kStack + 0x28, io); mem.put64(kStack + 0x30, buffer); mem.put64(kStack + 0x38, length);
  mem.put64(kStack + 0x40, 0); mem.put64(kStack + 0x48, 0);
}

static void test_windows() {
  FakeMemory mem; FakeHost host;
  Process p(9, binding_for(SyscallAbi::Windows, UserArch::X86_64), mem, host);
  mem.put(kBase + 0x100, "hello");
  stack_args(mem, kBase + 0x200, kBase + 0x100, 5);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 8))) == kNtSuccess && host.out == "hello");      // handle 8 = slot 1 = stdout
  CHECK(mem.get64(kBase + 0x200) == 0 && mem.get64(kBase + 0x208) == 5);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 12))) == kNtSuccess && host.err == "hello");
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 4))) == kNtDenied);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 1))) == kNtInvalidHandle);            // not a handle value
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 0))) == kNtInvalidHandle);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 4000))) == kNtInvalidHandle);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 20))) == kNtInvalidHandle);           // closed slot
  stack_args(mem, kBase + 0x200, 0xDEAD0000, 5);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 8))) == kNtAccessViolation);
  stack_args(mem, 0, kBase + 0x100, 5);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 8))) == kNtAccessViolation);          // no IoStatusBlock
  stack_args(mem, kBase + 0x200, kBase + 0x100, 0x1FFFFFFFFull);                                 // upper half of Length is ignored: 0x1FFFFFFFF -> 4 GiB-1
  host.out.clear();
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 8))) == kNtSuccess && host.out.size() == 0x1F00);   // bounded by the mapping
  // an APC routine is not supported
  stack_args(mem, kBase + 0x200, kBase + 0x100, 5);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtWriteFile, 8, 0, 0x1234))) == kNtNotImpl);
  // the stack itself unreadable
  SyscallRequest bad = nt(WindowsSyscallNumber::NtWriteFile, 8);
  bad.user_stack = 0xDEAD0000;
  CHECK(st(p.syscall(bad)) == kNtAccessViolation);
  bad.user_stack = ~uint64_t{0} - 4;
  CHECK(st(p.syscall(bad)) == kNtAccessViolation);

  host.input = "xyz";
  stack_args(mem, kBase + 0x200, kBase + 0x300, 8);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtReadFile, 4))) == kNtSuccess && mem.get64(kBase + 0x208) == 3 && mem.bytes[0x300] == 'x');
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtReadFile, 4))) == kNtEof && mem.get64(kBase + 0x208) == 0);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtReadFile, 8))) == kNtDenied);

  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtClose, 8))) == kNtSuccess);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtClose, 8))) == kNtInvalidHandle);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtClose, 6))) == kNtInvalidHandle);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtYieldExecution))) == kNtSuccess && host.yields == 1);

  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtQuerySystemTime, kBase + 0x400))) == kNtSuccess);
  CHECK(mem.get64(kBase + 0x400) == (1700000000ull + 11644473600ull) * 10000000ull);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtQuerySystemTime, 0))) == kNtAccessViolation);

  mem.put64(kBase + 0x500, uint64_t(-int64_t(30000000)));        // 3 s relative
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtDelayExecution, 0, kBase + 0x500))) == kNtSuccess && host.slept == 3000000000ull);
  mem.put64(kBase + 0x500, 0x8000000000000000ull);                // the most negative value must not overflow
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtDelayExecution, 0, kBase + 0x500))) == kNtSuccess);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtDelayExecution, 0, 0))) == kNtAccessViolation);

  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(kWindowsServiceCount)))) == kNtNoService);      // the first number past the list
  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(kWindowsServiceCount - 3)))) == kNtNotImpl);      // the last service without code (the two after it are this OS's own)
  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(kWin32kServiceBase)))) == kNtNotImpl);          // win32k's first and last service
  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(kWin32kServiceEnd - 1)))) == kNtNotImpl);
  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(kWin32kServiceBase - 1)))) == kNtNoService);     // the gap between the two tables
  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(kWin32kServiceEnd)))) == kNtNoService);
  CHECK(st(p.syscall(nt(static_cast<WindowsSyscallNumber>(99)))) == kNtNotImpl);                            // the first service without code
  SyscallRequest wide = nt(WindowsSyscallNumber::NtYieldExecution);
  wide.number |= 0x100000000ull;                                 // decoders mask this; a raw caller gets "no such service"
  CHECK(st(p.syscall(wide)) == kNtNoService);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtTerminateProcess, 77, 1))) == kNtInvalidHandle && !p.exited());
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtTerminateProcess, ~uint64_t{0}, 0xC000013A))) == kNtSuccess);
  CHECK(p.exited() && p.exit_code() == 0xC000013Au);
  CHECK(st(p.syscall(nt(WindowsSyscallNumber::NtYieldExecution))) == kNtNoService);
}

// A Linux program cannot reach NT services and a PE program cannot reach Linux ones: the same number means different things.
static void test_isolation() {
  FakeMemory mem; FakeHost host;
  Process lin(1, binding_for(SyscallAbi::Linux, UserArch::X86_64), mem, host);
  Process win(2, binding_for(SyscallAbi::Windows, UserArch::X86_64), mem, host);
  mem.put(kBase, "A");
  stack_args(mem, kBase + 0x200, kBase, 1);
  CHECK(lin.syscall(req(1, 1, kBase, 1)) == 1);                                   // 1 = write on Linux
  CHECK(st(win.syscall(nt(WindowsSyscallNumber::NtWriteFile, 8))) == kNtSuccess);  // 1 = NtWriteFile on Windows, different arguments
  CHECK(win.abi() == SyscallAbi::Windows && lin.abi() == SyscallAbi::Linux);
  Process none(3, ProcessBinding{}, mem, host);
  CHECK(sres(none.syscall(req(1))) == -38);
}

// ntdll.dll's C code (src/Windows/tcos_syscalls.h) and the kernel (WindowsSyscallNumber) must number the services alike.
static void test_service_numbers_agree() {
  struct Pair { const char* name; WindowsSyscallNumber number; };
  const Pair table[] = {
      {"NtReadFile", WindowsSyscallNumber::NtReadFile}, {"NtWriteFile", WindowsSyscallNumber::NtWriteFile}, {"NtClose", WindowsSyscallNumber::NtClose},
      {"NtTerminateProcess", WindowsSyscallNumber::NtTerminateProcess}, {"NtYieldExecution", WindowsSyscallNumber::NtYieldExecution},
      {"NtDelayExecution", WindowsSyscallNumber::NtDelayExecution}, {"NtQuerySystemTime", WindowsSyscallNumber::NtQuerySystemTime},
      {"NtAllocateVirtualMemory", WindowsSyscallNumber::NtAllocateVirtualMemory}, {"NtFreeVirtualMemory", WindowsSyscallNumber::NtFreeVirtualMemory},
      {"NtProtectVirtualMemory", WindowsSyscallNumber::NtProtectVirtualMemory}, {"NtCreateFile", WindowsSyscallNumber::NtCreateFile},
      {"NtOpenFile", WindowsSyscallNumber::NtOpenFile}, {"NtQueryInformationFile", WindowsSyscallNumber::NtQueryInformationFile},
      {"NtSetInformationFile", WindowsSyscallNumber::NtSetInformationFile}, {"NtQueryDirectoryFile", WindowsSyscallNumber::NtQueryDirectoryFile},
      {"NtQueryAttributesFile", WindowsSyscallNumber::NtQueryAttributesFile}, {"NtDeleteFile", WindowsSyscallNumber::NtDeleteFile},
      {"NtFlushBuffersFile", WindowsSyscallNumber::NtFlushBuffersFile}, {"NtQueryVolumeInformationFile", WindowsSyscallNumber::NtQueryVolumeInformationFile},
      {"NtQueryPerformanceCounter", WindowsSyscallNumber::NtQueryPerformanceCounter}, {"NtQueryVirtualMemory", WindowsSyscallNumber::NtQueryVirtualMemory},
      {"NtQueryInformationProcess", WindowsSyscallNumber::NtQueryInformationProcess},
      {"NtContinue", WindowsSyscallNumber::NtContinue},
      {"NtRaiseException", WindowsSyscallNumber::NtRaiseException},
      {"NtCreateThreadEx", WindowsSyscallNumber::NtCreateThreadEx},
      {"NtTerminateThread", WindowsSyscallNumber::NtTerminateThread},
      {"NtWaitForSingleObject", WindowsSyscallNumber::NtWaitForSingleObject},
      {"NtResumeThread", WindowsSyscallNumber::NtResumeThread},
      {"NtQueryInformationThread", WindowsSyscallNumber::NtQueryInformationThread},
      {"NtCreateEvent", WindowsSyscallNumber::NtCreateEvent},
      {"NtOpenEvent", WindowsSyscallNumber::NtOpenEvent},
      {"NtSetEvent", WindowsSyscallNumber::NtSetEvent},
      {"NtResetEvent", WindowsSyscallNumber::NtResetEvent},
      {"NtPulseEvent", WindowsSyscallNumber::NtPulseEvent},
      {"NtCreateMutant", WindowsSyscallNumber::NtCreateMutant},
      {"NtOpenMutant", WindowsSyscallNumber::NtOpenMutant},
      {"NtReleaseMutant", WindowsSyscallNumber::NtReleaseMutant},
      {"NtCreateSemaphore", WindowsSyscallNumber::NtCreateSemaphore},
      {"NtOpenSemaphore", WindowsSyscallNumber::NtOpenSemaphore},
      {"NtReleaseSemaphore", WindowsSyscallNumber::NtReleaseSemaphore},
      {"NtWaitForMultipleObjects", WindowsSyscallNumber::NtWaitForMultipleObjects},
      {"NtDuplicateObject", WindowsSyscallNumber::NtDuplicateObject},
      {"NtSuspendThread", WindowsSyscallNumber::NtSuspendThread},
      {"NtGetContextThread", WindowsSyscallNumber::NtGetContextThread},
      {"NtSetContextThread", WindowsSyscallNumber::NtSetContextThread},
      {"NtCreateUserProcess", WindowsSyscallNumber::NtCreateUserProcess},
      {"NtQuerySystemInformation", WindowsSyscallNumber::NtQuerySystemInformation},
      {"NtCreateJobObject", WindowsSyscallNumber::NtCreateJobObject},
      {"NtOpenJobObject", WindowsSyscallNumber::NtOpenJobObject},
      {"NtAssignProcessToJobObject", WindowsSyscallNumber::NtAssignProcessToJobObject},
      {"NtTerminateJobObject", WindowsSyscallNumber::NtTerminateJobObject},
      {"NtQueryInformationJobObject", WindowsSyscallNumber::NtQueryInformationJobObject},
      {"NtSetInformationJobObject", WindowsSyscallNumber::NtSetInformationJobObject},
      {"NtIsProcessInJob", WindowsSyscallNumber::NtIsProcessInJob},
      {"NtReadVirtualMemory", WindowsSyscallNumber::NtReadVirtualMemory},
      {"NtWriteVirtualMemory", WindowsSyscallNumber::NtWriteVirtualMemory},
      {"NtCreateSymbolicLink", WindowsSyscallNumber::NtCreateSymbolicLink},
      {"NtCreateSocket", WindowsSyscallNumber::NtCreateSocket},
      {"NtBindSocket", WindowsSyscallNumber::NtBindSocket},
      {"NtConnectSocket", WindowsSyscallNumber::NtConnectSocket},
      {"NtListenSocket", WindowsSyscallNumber::NtListenSocket},
      {"NtAcceptSocket", WindowsSyscallNumber::NtAcceptSocket},
      {"NtSendSocket", WindowsSyscallNumber::NtSendSocket},
      {"NtReceiveSocket", WindowsSyscallNumber::NtReceiveSocket},
      {"NtSocketControl", WindowsSyscallNumber::NtSocketControl},
      {"NtSelectSockets", WindowsSyscallNumber::NtSelectSockets},
      {"NtCreateChangeNotification", WindowsSyscallNumber::NtCreateChangeNotification},
      {"NtCreateMailslotFile", WindowsSyscallNumber::NtCreateMailslotFile},
      {"NtNotifyChangeDirectoryFile", WindowsSyscallNumber::NtNotifyChangeDirectoryFile},
      {"NtSetInformationThread", WindowsSyscallNumber::NtSetInformationThread},
      {"NtSetInformationProcess", WindowsSyscallNumber::NtSetInformationProcess},
      {"NtGetCurrentProcessorNumber", WindowsSyscallNumber::NtGetCurrentProcessorNumber},
      {"NtLoadModule", WindowsSyscallNumber::NtLoadModule},
      {"NtUnloadModule", WindowsSyscallNumber::NtUnloadModule},
      {"NtReleaseModule", WindowsSyscallNumber::NtReleaseModule},
      {"NtQueryObject", WindowsSyscallNumber::NtQueryObject},
      {"NtSetInformationObject", WindowsSyscallNumber::NtSetInformationObject},
      {"NtCreatePipe", WindowsSyscallNumber::NtCreatePipe},
      {"NtCreateNamedPipeFile", WindowsSyscallNumber::NtCreateNamedPipeFile},
      {"NtFsControlFile", WindowsSyscallNumber::NtFsControlFile},
      {"NtQueueApcThread", WindowsSyscallNumber::NtQueueApcThread},
      {"NtDequeueApc", WindowsSyscallNumber::NtDequeueApc},
      {"NtSignalAndWaitForSingleObject", WindowsSyscallNumber::NtSignalAndWaitForSingleObject},
      {"NtCreateTimer", WindowsSyscallNumber::NtCreateTimer},
      {"NtOpenTimer", WindowsSyscallNumber::NtOpenTimer},
      {"NtSetTimer", WindowsSyscallNumber::NtSetTimer},
      {"NtCancelTimer", WindowsSyscallNumber::NtCancelTimer},
      {"NtQueryTimer", WindowsSyscallNumber::NtQueryTimer},
      {"NtQueryTimerResolution", WindowsSyscallNumber::NtQueryTimerResolution},
      {"NtSetTimerResolution", WindowsSyscallNumber::NtSetTimerResolution},
      {"NtOpenThread", WindowsSyscallNumber::NtOpenThread},
      {"NtOpenProcess", WindowsSyscallNumber::NtOpenProcess},
      {"NtCreateSection", WindowsSyscallNumber::NtCreateSection},
      {"NtOpenSection", WindowsSyscallNumber::NtOpenSection},
      {"NtMapViewOfSection", WindowsSyscallNumber::NtMapViewOfSection},
      {"NtUnmapViewOfSection", WindowsSyscallNumber::NtUnmapViewOfSection},
      {"NtFlushVirtualMemory", WindowsSyscallNumber::NtFlushVirtualMemory},
      {"NtLockFile", WindowsSyscallNumber::NtLockFile},
      {"NtUnlockFile", WindowsSyscallNumber::NtUnlockFile},
  };
  FILE* header = fopen("src/Windows/tcos_syscalls.h", "r");
  CHECK(header != nullptr);
  if (!header) return;
  std::string enum_text;
  if (FILE* hpp = fopen("src/Process/WindowsSyscalls.hpp", "r")) {
    char chunk[4096];
    for (size_t got; (got = fread(chunk, 1, sizeof(chunk), hpp)) > 0;) enum_text.append(chunk, got);
    fclose(hpp);
  }
  CHECK(!enum_text.empty());
  size_t generated = 0;
  char line[256];
  size_t found = 0;
  while (fgets(line, sizeof(line), header)) {
    char name[96];
    unsigned number;
    if (sscanf(line, "#define NT_SERVICE_%95s %u", name, &number) != 2) continue;
    if (std::string(name) == "COUNT") {
      CHECK(number == kWindowsServiceCount);
      continue;
    }
    if (std::string(name) == "WIN32K_BASE" || std::string(name) == "WIN32K_END") {
      CHECK(number == (std::string(name) == "WIN32K_BASE" ? kWin32kServiceBase : kWin32kServiceEnd));
      continue;
    }
    bool known = false;
    for (const Pair& pair : table) {
      if (std::string(pair.name) != name) continue;
      known = true;
      ++found;
      CHECK(uint32_t(pair.number) == number);
    }
    if (!known) {      // a service without code: the header and the enum of WindowsSyscallNumber must agree on it
      ++generated;
      const std::string wanted = "  " + std::string(name) + " = " + std::to_string(number) + ",";
      CHECK(enum_text.find("\n" + wanted + "\n") != std::string::npos);
    }
  }
  fclose(header);
  CHECK(found == sizeof(table) / sizeof(table[0]));
  CHECK(found + generated == kWindowsServiceCount + (kWin32kServiceEnd - kWin32kServiceBase));      // every number below the count is a service of the header
}

int main() {
  test_selection();
  test_decode();
  test_linux();
  test_linux_generic_table();
  test_windows();
  test_isolation();
  test_service_numbers_agree();
  CHECK(A.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("SyscallTest: all passed\n");
  return 0;
}
