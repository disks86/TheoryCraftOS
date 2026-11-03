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
// Host test for the CPU emulator's JIT (src/EmulationJit), x86-64 and RISC-V guests. The interpreter is the reference: random instruction streams from the translated subset run on a cpu with and
// without translated code and must end in the same registers, flags, memory, fault and instruction count; whole programs run with translated code give the output they
// give natively; and the cases where translated code must stand aside (code the guest can write, a changed address space) are checked on their own.
//   EmulationJitTest <guest.elf> <expected.txt> [<threads.elf> <threads-expected.txt> <x87.elf> <x87-expected.txt> <sse4.elf> <sse4-expected.txt> <guest-riscv64.elf> <threads-riscv64.elf> <guest-aarch64.elf> <threads-aarch64.elf>]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Emulation/GuestCpu.hpp"
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/RiscV64Cpu.hpp"
#include "Emulation/X86_64Cpu.hpp"
#include "EmulationJit/GuestJit.hpp"
#include "EmulationJit/AArch64Jit.hpp"
#include "EmulationJit/RiscV64Jit.hpp"
#include "EmulationJit/X86_64Jit.hpp"
#include "Process/Process.hpp"
#include "Process/UserProgram.hpp"
#include "JitHost.hpp"

using namespace emulation;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;
static jit::HostCodeMemory g_code;
typedef std::vector<uint8_t> Bytes;

struct FakeHost final : process::IProcessHost {
  std::string out, err;
  bool write_console(uint32_t stream, ConstByteView d) override {
    std::string& s = stream == 2 ? err : out;
    for (uint8_t b : d) s.push_back(char(b));
    return true;
  }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull; }
  uint64_t monotonic_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
  void sleep_ns(uint64_t ns) override { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
  void yield() override { std::this_thread::yield(); }
};
struct HostSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable_any cv;
  void lock() override { m.lock(); }
  void unlock() override { m.unlock(); }
  bool wait(uint64_t timeout_ns) override {
    if (timeout_ns == ~uint64_t{0}) { cv.wait(m); return true; }
    return cv.wait_for(m, std::chrono::nanoseconds(timeout_ns)) != std::cv_status::timeout;
  }
  void notify_all() override { cv.notify_all(); }
  uint64_t now_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
};
struct HostThreads final : IThreadSpawner {
  struct Handle { std::thread t; };
  void* spawn(void (*fn)(void*), void* arg, const char*) override { return new Handle{std::thread(fn, arg)}; }
  void join(void* handle) override {
    Handle* h = static_cast<Handle*>(handle);
    h->t.join();
    delete h;
  }
  uint64_t self() override { return uint64_t(std::hash<std::thread::id>()(std::this_thread::get_id())); }
};
struct NeverStop final : IRunControl {
  bool stop_requested() override { return false; }
  void yield() override {}
};

constexpr uint64_t kCode = 0x10000, kData = 0x20000, kStack = 0x30000;

// code at kCode (read + execute, or read-write-execute for the self-modifying case), data and stack read-write
struct Machine {
  process::AddressSpace space{A};
  X86_64Cpu cpu;
  Machine(const Bytes& code, bool translated, uint32_t threshold = 1, uint8_t code_protection = process::kProtRead | process::kProtExecute, bool exact_flags = false, uint32_t max_block = 32) {
    space.map(kCode, 0x2000, code_protection);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(kStack, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64;
    start.entry = kCode;
    start.stack_pointer = kStack + 0x800;
    cpu.reset(start);
    if (translated) {
      GuestJitOptions options;
      options.hot_threshold = threshold;
      options.exact_flags = exact_flags;
      options.max_block = max_block;
      cpu.set_jit(create_x86_jit(A, g_code, options));
    }
  }
  StopReason run(uint64_t n) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
  uint64_t entered() const { return cpu.jit_blocks_entered(); }
};

// --- random streams ----------------------------------------------------------------------------------------------------------------------------

struct Rng {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
  bool chance(uint32_t percent) { return below(100) < percent; }
};

// Values that sit on the edges of the arithmetic: zero, one, the sign bits and carries of every width, so that equal operands and overflows happen.
static uint64_t edge_value(Rng& r) {
  static const uint64_t v[] = {0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull,
                               0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull, 0xFFFFFF80ull, 0xFFFFFFFFFFFFFF80ull};
  return v[r.below(sizeof(v) / sizeof(v[0]))];
}

static void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

// ModRM (+ SIB + displacement) with the given register field (or a random one).
static void modrm(Bytes& b, Rng& r, int reg_field = -1) {
  static const int mods[] = {0, 0, 1, 1, 1, 2, 3, 3, 3, 3};
  const int mod = mods[r.below(10)];
  const int reg = reg_field >= 0 ? reg_field : int(r.below(8));
  const int rm = int(r.below(8));
  b.push_back(uint8_t((mod << 6) | (reg << 3) | rm));
  if (mod == 3) return;
  bool disp32 = mod == 2;
  if (rm == 4) {
    const uint8_t sib = uint8_t(r.below(256));
    b.push_back(sib);
    if (mod == 0 && (sib & 7) == 5) disp32 = true;
  } else if (rm == 5 && mod == 0) {
    disp32 = true;
  }
  if (mod == 1) b.push_back(uint8_t(r.below(0x60)));
  if (disp32) put32(b, r.chance(30) ? uint32_t(kData - kCode + r.below(0x400)) : r.below(0x800));
}

static void imm(Bytes& b, Rng& r, int bytes) {
  uint64_t v = r.chance(50) ? r.below(40) : r.chance(50) ? edge_value(r) : r.next();
  if (r.chance(10)) v = uint64_t(-int64_t(r.below(40)));
  for (int i = 0; i < bytes; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

static void instruction(Bytes& b, Rng& r) {
  // prefixes
  if (r.chance(15)) b.push_back(0x66);
  if (r.chance(5)) b.push_back(r.chance(50) ? 0x64 : 0x65);
  if (r.chance(60)) b.push_back(uint8_t(0x40 | r.below(16)));
  switch (r.below(40)) {
    case 0: case 1: case 2: case 3: case 4: case 5: {      // alu in the six forms
      const uint8_t op = uint8_t((r.below(8) << 3) | r.below(6));
      b.push_back(op);
      if ((op & 7) >= 4) imm(b, r, (op & 7) == 4 ? 1 : 4);
      else modrm(b, r);
      break;
    }
    case 6: case 7: {      // group 1
      const uint8_t op = uint8_t(r.chance(30) ? 0x80 : r.chance(50) ? 0x81 : 0x83);
      b.push_back(op);
      modrm(b, r);
      imm(b, r, op == 0x81 ? 4 : 1);
      break;
    }
    case 8: b.push_back(uint8_t(0x84 + r.below(2))); modrm(b, r); break;       // test
    case 9: b.push_back(uint8_t(0x86 + r.below(2))); modrm(b, r); break;       // xchg
    case 10: case 11: case 12: b.push_back(uint8_t(0x88 + r.below(4))); modrm(b, r); break;       // mov
    case 13: b.push_back(0x8D); modrm(b, r); break;       // lea
    case 14: b.push_back(r.chance(50) ? 0x98 : 0x99); break;
    case 15: b.push_back(uint8_t(0xB0 + r.below(8))); imm(b, r, 1); break;
    case 16: b.push_back(uint8_t(0xB8 + r.below(8))); imm(b, r, 8); break;      // (the extra bytes are the next instruction's problem without REX.W)
    case 17: {      // shifts
      const uint8_t op = uint8_t(0xC0 + r.below(2));
      b.push_back(op);
      modrm(b, r);
      imm(b, r, 1);
      break;
    }
    case 18: b.push_back(uint8_t(0xD0 + r.below(4))); modrm(b, r); break;
    case 19: {      // mov r/m, imm
      const uint8_t op = r.chance(40) ? 0xC6 : 0xC7;
      b.push_back(op);
      modrm(b, r, 0);
      imm(b, r, op == 0xC6 ? 1 : 4);
      break;
    }
    case 20: case 21: {      // group F6 / F7 (test, not, neg, mul, imul, div, idiv)
      const uint8_t op = uint8_t(0xF6 + r.below(2));
      const int field = int(r.below(8));
      b.push_back(op);
      modrm(b, r, field);
      if (field < 2) imm(b, r, op == 0xF6 ? 1 : 4);
      break;
    }
    case 22: case 23: b.push_back(uint8_t(0xFE + r.below(2))); modrm(b, r); break;      // inc, dec, call, jmp, push
    case 24: case 25: b.push_back(0x0F); b.push_back(0xAF); modrm(b, r); break;      // imul
    case 26: case 27: b.push_back(0x0F); b.push_back(uint8_t(0xB6 + (r.below(2) ? 8 : 0) + r.below(2))); modrm(b, r); break;      // movzx, movsx
    case 28: case 29: b.push_back(0x0F); b.push_back(uint8_t(0x40 + r.below(16))); modrm(b, r); break;      // cmov
    case 30: case 31: b.push_back(0x0F); b.push_back(uint8_t(0x90 + r.below(16))); modrm(b, r); break;      // setcc
    case 32: case 33: b.push_back(uint8_t(0x70 + r.below(16))); b.push_back(uint8_t(int8_t(int(r.below(24)) - 6))); break;      // jcc rel8
    case 34: b.push_back(0x0F); b.push_back(uint8_t(0x80 + r.below(16))); put32(b, uint32_t(int32_t(int(r.below(60)) - 14))); break;
    case 35: b.push_back(uint8_t(0x50 + r.below(16))); break;      // push, pop
    case 36: b.push_back(r.chance(50) ? 0x69 : 0x6B); modrm(b, r); imm(b, r, b[b.size() - 1] == 0 ? 4 : 4); break;
    case 37: b.push_back(r.chance(50) ? 0x68 : 0x6A); imm(b, r, 4); break;
    case 38: b.push_back(r.chance(50) ? 0xA8 : 0xA9); imm(b, r, 4); break;
    default: {
      const uint32_t pick = r.below(8);
      if (pick == 0) b.push_back(0xC9);                      // leave
      else if (pick == 1) b.push_back(0xC3);                 // ret
      else if (pick == 2) { b.push_back(0xE8); put32(b, uint32_t(int32_t(int(r.below(80)) - 20))); }      // call
      else if (pick == 3) { b.push_back(0xE9); put32(b, uint32_t(int32_t(int(r.below(80)) - 20))); }      // jmp
      else if (pick == 4) { b.push_back(0xEB); b.push_back(uint8_t(int8_t(int(r.below(30)) - 8))); }
      else if (pick == 5) { b.push_back(0x0F); b.push_back(0x1F); modrm(b, r, 0); }                       // nop
      else if (pick == 6) { b.push_back(0x63); modrm(b, r); }                                              // movsxd
      else { b.push_back(0x0F); b.push_back(0x05); }                                                       // syscall
      break;
    }
  }
}

static void init_registers(Machine& m, Rng& r) {
  uint64_t vals[16];
  for (int i = 0; i < 16; ++i) {
    const uint32_t k = r.below(100);
    if (k < 70) vals[i] = kData + r.below(0x800);
    else if (k < 78) vals[i] = kStack + 0x400 + 8 * r.below(64);
    else if (k < 88) vals[i] = r.below(300);
    else if (k < 93) vals[i] = ~uint64_t{0} - r.below(5);
    else vals[i] = r.next();
    if (r.chance(30)) vals[i] = edge_value(r);
    else if (i > 0 && r.chance(10)) vals[i] = vals[i - 1];
  }
  vals[4] = r.chance(95) ? kStack + 0x400 + 8 * r.below(64) : vals[4];
  for (int i = 0; i < 16; ++i) m.cpu.state().r[i] = vals[i];
  m.cpu.state().rflags = 2 | (r.next() & 0x8D5);      // CF PF AF ZF SF OF as they come
  m.cpu.state().fs_base = kData + 0x100;
  m.cpu.state().gs_base = kData + 0x800;
  // data and stack contents
  Bytes fill(0x2000);
  for (auto& v : fill) v = uint8_t(r.next());
  m.space.write_privileged(kData, ConstByteView(fill.data(), fill.size()));
  Bytes stack(0x1000);
  for (auto& v : stack) v = uint8_t(r.next());
  m.space.write_privileged(kStack, ConstByteView(stack.data(), stack.size()));
}

static Bytes dump(Machine& m) {
  Bytes out(0x2000 + 0x1000);
  m.space.read_privileged(kData, ByteView(out.data(), 0x2000));
  m.space.read_privileged(kStack, ByteView(out.data() + 0x2000, 0x1000));
  return out;
}

static bool same_state(Machine& a, Machine& b, StopReason ra, StopReason rb, const char** what, bool skip_flags_at_budget = false) {
  const auto& sa = a.cpu.state();
  const auto& sb = b.cpu.state();
  if (ra != rb) { *what = "stop reason"; return false; }
  for (int i = 0; i < 16; ++i)
    if (sa.r[i] != sb.r[i]) { *what = "register"; return false; }
  if (sa.rip != sb.rip) { *what = "rip"; return false; }
  if (sa.rflags != sb.rflags && !(skip_flags_at_budget && ra == StopReason::Budget)) { *what = "rflags"; return false; }
  if (a.cpu.instructions() != b.cpu.instructions()) { *what = "instruction count"; return false; }
  if (ra == StopReason::Fault) {
    const GuestFault& fa = a.cpu.fault();
    const GuestFault& fb = b.cpu.fault();
    if (fa.vector != fb.vector || fa.address != fb.address || fa.pc != fb.pc || fa.error != fb.error) { *what = "fault"; return false; }
  }
  if (dump(a) != dump(b)) { *what = "memory"; return false; }
  return true;
}

static bool build_case(uint64_t seed, Bytes* code, uint64_t* init_seed) {
  Rng r{seed * 0x9E3779B97F4A7C15ull + 12345};
  for (int i = 0; i < 4; ++i) r.next();
  const uint32_t n = 30 + r.below(60);
  for (uint32_t i = 0; i < n; ++i) instruction(*code, r);
  if (code->size() > 0x1000) code->resize(0x1000);
  *init_seed = r.next();
  return true;
}

// JIT_BISECT=<seed>: finds the first instruction count at which the two cpus differ and shows the instruction about to run.
static void bisect(uint64_t seed) {
  Bytes code;
  uint64_t init_seed;
  build_case(seed, &code, &init_seed);
  for (uint64_t n = 1; n <= 2500; ++n) {
    Machine plain(code, false), fast(code, true, 1, process::kProtRead | process::kProtExecute, getenv("JIT_LAZY") == nullptr);
    Rng ra{init_seed | 1}, rb{init_seed | 1};
    init_registers(plain, ra);
    init_registers(fast, rb);
    const StopReason sa = plain.run(n), sb = fast.run(n);
    const char* what = "";
    if (!same_state(plain, fast, sa, sb, &what, getenv("JIT_LAZY") != nullptr)) {
      Machine before(code, false);
      Rng rc{init_seed | 1};
      init_registers(before, rc);
      before.run(n - 1);
      const uint64_t rip = before.cpu.state().rip;
      printf("seed %llu: first difference after %llu instructions (%s); the last one ran at %llx:", (unsigned long long)seed, (unsigned long long)n, what, (unsigned long long)rip);
      uint8_t raw[16] = {};
      before.space.read_privileged(rip, ByteView(raw, 16));
      for (uint8_t c : raw) printf(" %02x", c);
      printf("\n  flags before %llx; after: plain %llx fast %llx\n", (unsigned long long)before.cpu.state().rflags, (unsigned long long)plain.cpu.state().rflags, (unsigned long long)fast.cpu.state().rflags);
      return;
    }
  }
  printf("seed %llu: no difference\n", (unsigned long long)seed);
}

// JIT_BLOCKS=<seed>: with the first block cut to k instructions (the rest interpreted) the smallest k that differs names the instruction the translation gets wrong.
static void bisect_block(uint64_t seed) {
  Bytes code;
  uint64_t init_seed;
  build_case(seed, &code, &init_seed);
  for (uint32_t k = 1; k <= 32; ++k) {
    Machine plain(code, false), fast(code, true, 1, process::kProtRead | process::kProtExecute, getenv("JIT_LAZY") == nullptr, k);
    Rng ra{init_seed | 1}, rb{init_seed | 1};
    init_registers(plain, ra);
    init_registers(fast, rb);
    const StopReason sa = plain.run(32), sb = fast.run(32);
    const char* what = "";
    if (!same_state(plain, fast, sa, sb, &what, getenv("JIT_LAZY") != nullptr)) {
      Machine before(code, false);
      Rng rc{init_seed | 1};
      init_registers(before, rc);
      before.run(k - 1);
      const uint64_t rip = before.cpu.state().rip;
      printf("seed %llu: a block of %u differs (%s); instruction %u at %llx:", (unsigned long long)seed, k, what, k, (unsigned long long)rip);
      uint8_t raw[16] = {};
      before.space.read_privileged(rip, ByteView(raw, 16));
      for (uint8_t c : raw) printf(" %02x", c);
      printf("\n  flags before %llx; plain %llx fast %llx\n", (unsigned long long)before.cpu.state().rflags, (unsigned long long)plain.cpu.state().rflags, (unsigned long long)fast.cpu.state().rflags);
      return;
    }
  }
  printf("seed %llu: no block size differs\n", (unsigned long long)seed);
}

static void test_random(uint64_t seeds, uint64_t first_seed, bool exact) {
  uint64_t entered = 0, faults = 0, syscalls = 0, retired = 0;
  for (uint64_t seed = first_seed; seed < first_seed + seeds; ++seed) {
    Rng r{seed * 0x9E3779B97F4A7C15ull + 12345};
    for (int i = 0; i < 4; ++i) r.next();
    Bytes code;
    const uint32_t n = 30 + r.below(60);
    for (uint32_t i = 0; i < n; ++i) instruction(code, r);
    if (code.size() > 0x1000) code.resize(0x1000);
    const uint64_t init_seed = r.next();
    Machine plain(code, false), fast(code, true, 1 + (seed % 3), process::kProtRead | process::kProtExecute, exact);
    Rng ra{init_seed | 1}, rb{init_seed | 1};
    init_registers(plain, ra);
    init_registers(fast, rb);
    StopReason sa = StopReason::Budget, sb = StopReason::Budget;
    uint64_t left = 2500;
    const uint64_t chunk = (seed % 4 == 0) ? 77 : 2500;      // some runs stop and resume at awkward points
    while (left) {
      const uint64_t step = left < chunk ? left : chunk;
      sa = plain.run(step);
      sb = fast.run(step);
      left -= step;
      if (sa != StopReason::Budget || sb != StopReason::Budget) break;
    }
    const char* what = "";
    if (!same_state(plain, fast, sa, sb, &what, !exact)) {
      printf("FAILED seed %llu: %s differs (stop %d/%d)\n  code:", (unsigned long long)seed, what, int(sa), int(sb));
      for (uint8_t c : code) printf(" %02x", c);
      printf("\n  rip %llx/%llx rflags %llx/%llx\n", (unsigned long long)plain.cpu.state().rip, (unsigned long long)fast.cpu.state().rip, (unsigned long long)plain.cpu.state().rflags, (unsigned long long)fast.cpu.state().rflags);
      for (int i = 0; i < 16; ++i) if (plain.cpu.state().r[i] != fast.cpu.state().r[i]) printf("  r%d %llx/%llx\n", i, (unsigned long long)plain.cpu.state().r[i], (unsigned long long)fast.cpu.state().r[i]);
      ++g_failed;
      if (g_failed > 5) return;
    }
    entered += fast.entered();
    retired += fast.cpu.instructions();
    if (sb == StopReason::Fault) ++faults;
    if (sb == StopReason::Syscall) ++syscalls;
  }
  printf("random (%s flags): %llu streams, %llu instructions, %llu translated blocks entered, %llu ended in a fault, %llu in a syscall\n", exact ? "exact" : "lazy", (unsigned long long)seeds, (unsigned long long)retired, (unsigned long long)entered, (unsigned long long)faults, (unsigned long long)syscalls);
  CHECK(entered > seeds);      // (the translated code did run)
}

// --- hand-written cases ------------------------------------------------------------------------------------------------------------------------

static Bytes B(std::initializer_list<int> v) {
  Bytes b;
  for (int x : v) b.push_back(uint8_t(x));
  return b;
}

// A counting loop: the translated block must be entered many times and the result agree.
static void test_loop() {
  // mov ecx, 1000; xor eax, eax; L: add eax, ecx; dec ecx; jnz L; syscall
  const Bytes code = B({0xB9, 0xE8, 0x03, 0, 0, 0x31, 0xC0, 0x01, 0xC8, 0xFF, 0xC9, 0x75, 0xFA, 0x0F, 0x05});
  Machine plain(code, false), fast(code, true, 2);
  CHECK(plain.run(100000) == StopReason::Syscall && fast.run(100000) == StopReason::Syscall);
  CHECK(fast.cpu.state().r[0] == 500500 && fast.cpu.state().r[0] == plain.cpu.state().r[0]);
  CHECK(fast.cpu.state().rflags == plain.cpu.state().rflags && fast.cpu.instructions() == plain.cpu.instructions());
  CHECK(fast.entered() > 900);
}

// A fault in the middle of a block leaves the earlier instructions done and the faulting one not started.
static void test_fault_precision() {
  // mov eax, 5; add rbx, 7; mov [rcx], rax (rcx unmapped); mov edx, 9; syscall
  const Bytes code = B({0xB8, 5, 0, 0, 0, 0x48, 0x83, 0xC3, 7, 0x48, 0x89, 0x01, 0xBA, 9, 0, 0, 0, 0x0F, 0x05});
  Machine plain(code, false), fast(code, true);
  for (Machine* m : {&plain, &fast}) {
    m->cpu.state().r[1] = 0xDEAD0000;
    m->cpu.state().r[3] = 1;
  }
  CHECK(plain.run(100) == StopReason::Fault && fast.run(100) == StopReason::Fault);
  CHECK(fast.cpu.state().r[0] == 5 && fast.cpu.state().r[3] == 8 && fast.cpu.state().r[2] == 0);
  CHECK(fast.cpu.state().rip == kCode + 9 && fast.cpu.fault().vector == 14 && fast.cpu.fault().address == 0xDEAD0000 && (fast.cpu.fault().error & 2));
  CHECK(fast.cpu.state().rip == plain.cpu.state().rip && fast.cpu.instructions() == plain.cpu.instructions());
  CHECK(fast.entered() >= 1);
}

// Code the guest can write is never translated, so it can change freely.
static void test_self_modifying() {
  // mov eax, 1; mov byte [rip+X], 2 over the immediate of the next mov; mov ebx, 1; syscall. Simpler: patch the imm of 'mov ebx, imm' before it runs.
  Bytes code = B({0xBB, 1, 0, 0, 0, 0x0F, 0x05});
  Machine fast(code, true, 1, process::kProtRead | process::kProtWrite | process::kProtExecute);
  for (int round = 0; round < 3; ++round) {
    fast.cpu.state().rip = kCode;
    const uint8_t patch = uint8_t(10 + round);
    fast.space.write_privileged(kCode + 1, ConstByteView(&patch, 1));
    CHECK(fast.run(100) == StopReason::Syscall);
    CHECK(fast.cpu.state().r[3] == uint64_t(10 + round));
  }
  CHECK(fast.entered() == 0);
}

// The address space changes under translated code: a remapped page must not run the old translation.
static void test_remap() {
  const Bytes first = B({0xB8, 1, 0, 0, 0, 0x0F, 0x05});
  Machine fast(first, true, 1);
  CHECK(fast.run(100) == StopReason::Syscall && fast.cpu.state().r[0] == 1);
  CHECK(fast.entered() >= 1);
  CHECK(fast.space.unmap(kCode, 0x2000));
  CHECK(fast.space.map(kCode, 0x2000, process::kProtRead | process::kProtExecute));
  const Bytes second = B({0xB8, 2, 0, 0, 0, 0x0F, 0x05});
  fast.space.write_privileged(kCode, ConstByteView(second.data(), second.size()));
  fast.cpu.state().rip = kCode;
  CHECK(fast.run(100) == StopReason::Syscall && fast.cpu.state().r[0] == 2);
  // and a page that stops being executable faults, translated or not
  CHECK(fast.space.protect(kCode, 0x2000, process::kProtRead));
  fast.cpu.state().rip = kCode;
  CHECK(fast.run(100) == StopReason::Fault && fast.cpu.fault().vector == 14);
}


// --- RISC-V -----------------------------------------------------------------------------------------------------------------------------------

struct RvMachine {
  process::AddressSpace space{A};
  RiscV64Cpu cpu;
  RvMachine(const Bytes& code, bool translated, uint32_t threshold = 1, uint32_t max_block = 32) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtExecute);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(kStack, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::RiscV64;
    start.entry = kCode;
    start.stack_pointer = kStack + 0x800;
    cpu.reset(start);
    if (translated) {
      GuestJitOptions options;
      options.hot_threshold = threshold;
      options.max_block = max_block;
      cpu.set_jit(create_riscv64_jit(A, g_code, options));
    }
  }
  StopReason run(uint64_t n) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
};

static void put16(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
}
static void put32le(Bytes& b, uint32_t v) {
  put16(b, v & 0xFFFF);
  put16(b, v >> 16);
}

static uint32_t rv_reg(Rng& r) { return r.chance(70) ? 1 + r.below(8) : r.below(32); }

static void rv_instruction(Bytes& b, Rng& r) {
  const uint32_t rd = rv_reg(r), rs1 = rv_reg(r), rs2 = rv_reg(r);
  const uint32_t f3 = r.below(8);
  switch (r.below(24)) {
    case 0: case 1: case 2: case 3: {      // OP
      static const uint32_t f7s[] = {0, 0, 0x20, 1, 0, 0x20};
      put32le(b, (f7s[r.below(6)] << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33);
      break;
    }
    case 4: case 5: {      // OP-32
      static const uint32_t f7s[] = {0, 0, 0x20, 1, 0, 0x20};
      put32le(b, (f7s[r.below(6)] << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x3B);
      break;
    }
    case 6: case 7: case 8: {      // OP-IMM
      uint32_t imm = r.below(4096);
      if (f3 == 1 || f3 == 5) imm = (r.chance(25) ? 0x400 : 0) | r.below(64);
      put32le(b, (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x13);
      break;
    }
    case 9: case 10: {      // OP-IMM-32
      uint32_t imm = r.below(4096);
      if (f3 == 1 || f3 == 5) imm = (r.chance(25) ? 0x400 : 0) | r.below(32);
      put32le(b, (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x1B);
      break;
    }
    case 11: case 12: case 13: {      // loads
      const uint32_t imm = r.chance(80) ? r.below(0x100) : r.below(4096);
      put32le(b, (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x03);
      break;
    }
    case 14: case 15: {      // stores
      const uint32_t imm = r.chance(80) ? r.below(0x100) : r.below(4096);
      put32le(b, ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | ((f3 & 3) << 12) | ((imm & 31) << 7) | 0x23);
      break;
    }
    case 16: put32le(b, (r.below(0x100000) << 12) | (rd << 7) | (r.chance(50) ? 0x37 : 0x17)); break;
    case 17: case 18: {      // branches, offsets of -16..+60
      static const uint32_t f3s[] = {0, 1, 4, 5, 6, 7};
      const int32_t off = (int32_t(r.below(39)) - 8) * 2;
      const uint32_t u = uint32_t(off);
      put32le(b, (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3s[r.below(6)] << 12) | (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | 0x63);
      break;
    }
    case 19: {      // jal
      const int32_t off = (int32_t(r.below(39)) - 8) * 2;
      const uint32_t u = uint32_t(off);
      put32le(b, (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1) << 20) | (((u >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F);
      break;
    }
    case 20: put32le(b, (r.below(64) << 20) | (rs1 << 15) | (rd << 7) | 0x67); break;      // jalr (mostly to nowhere)
    case 21: put16(b, (r.below(0x10000) & ~3u) | r.below(3)); break;      // some compressed encoding, legal or not
    case 22: put32le(b, uint32_t(r.next()) | 3); break;                 // anything
    default: put32le(b, 0x00000013); break;                              // nop
  }
}

static void rv_init(RvMachine& m, Rng& r) {
  for (int i = 1; i < 32; ++i) {
    const uint32_t k = r.below(100);
    uint64_t v;
    if (k < 70) v = kData + r.below(0x800);
    else if (k < 78) v = kStack + 0x400 + 8 * r.below(64);
    else if (k < 88) v = r.below(300);
    else if (k < 93) v = ~uint64_t{0} - r.below(5);
    else v = r.next();
    if (r.chance(25)) v = edge_value(r);
    else if (i > 1 && r.chance(10)) v = m.cpu.state().x[i - 1];
    m.cpu.state().x[i] = v;
  }
  Bytes fill(0x2000);
  for (auto& v : fill) v = uint8_t(r.next());
  m.space.write_privileged(kData, ConstByteView(fill.data(), fill.size()));
  Bytes stack(0x1000);
  for (auto& v : stack) v = uint8_t(r.next());
  m.space.write_privileged(kStack, ConstByteView(stack.data(), stack.size()));
}

static bool rv_same(RvMachine& a, RvMachine& b, StopReason ra, StopReason rb, const char** what) {
  if (ra != rb) { *what = "stop reason"; return false; }
  for (int i = 0; i < 32; ++i)
    if (a.cpu.state().x[i] != b.cpu.state().x[i]) { *what = "register"; return false; }
  if (a.cpu.state().pc != b.cpu.state().pc) { *what = "pc"; return false; }
  if (a.cpu.instructions() != b.cpu.instructions()) { *what = "instruction count"; return false; }
  if (ra == StopReason::Fault) {
    const GuestFault& fa = a.cpu.fault();
    const GuestFault& fb = b.cpu.fault();
    if (fa.vector != fb.vector || fa.address != fb.address || fa.pc != fb.pc) { *what = "fault"; return false; }
  }
  Bytes da(0x3000), db(0x3000);
  a.space.read_privileged(kData, ByteView(da.data(), 0x2000));
  a.space.read_privileged(kStack, ByteView(da.data() + 0x2000, 0x1000));
  b.space.read_privileged(kData, ByteView(db.data(), 0x2000));
  b.space.read_privileged(kStack, ByteView(db.data() + 0x2000, 0x1000));
  if (da != db) { *what = "memory"; return false; }
  return true;
}

static void test_random_riscv(uint64_t seeds, uint64_t first_seed) {
  uint64_t entered = 0, faults = 0, retired = 0;
  for (uint64_t seed = first_seed; seed < first_seed + seeds; ++seed) {
    Rng r{seed * 0x9E3779B97F4A7C15ull + 777};
    for (int i = 0; i < 4; ++i) r.next();
    Bytes code;
    const uint32_t n = 40 + r.below(80);
    for (uint32_t i = 0; i < n; ++i) rv_instruction(code, r);
    if (code.size() > 0x1000) code.resize(0x1000);
    const uint64_t init_seed = r.next();
    RvMachine plain(code, false), fast(code, true, 1 + (seed % 3), (seed % 5 == 0) ? 1 + uint32_t(seed % 7) : 32);
    Rng ra{init_seed | 1}, rb{init_seed | 1};
    rv_init(plain, ra);
    rv_init(fast, rb);
    StopReason sa = StopReason::Budget, sb = StopReason::Budget;
    uint64_t left = 2500;
    const uint64_t chunk = (seed % 4 == 0) ? 77 : 2500;
    while (left) {
      const uint64_t step = left < chunk ? left : chunk;
      sa = plain.run(step);
      sb = fast.run(step);
      left -= step;
      if (sa != StopReason::Budget || sb != StopReason::Budget) break;
    }
    const char* what = "";
    if (!rv_same(plain, fast, sa, sb, &what)) {
      printf("FAILED riscv seed %llu: %s differs (stop %d/%d)\n  code:", (unsigned long long)seed, what, int(sa), int(sb));
      for (uint8_t c : code) printf(" %02x", c);
      printf("\n  pc %llx/%llx\n", (unsigned long long)plain.cpu.state().pc, (unsigned long long)fast.cpu.state().pc);
      for (int i = 0; i < 32; ++i) if (plain.cpu.state().x[i] != fast.cpu.state().x[i]) printf("  x%d %llx/%llx\n", i, (unsigned long long)plain.cpu.state().x[i], (unsigned long long)fast.cpu.state().x[i]);
      ++g_failed;
      if (g_failed > 5) return;
    }
    entered += fast.cpu.jit_blocks_entered();
    retired += fast.cpu.instructions();
    if (sb == StopReason::Fault) ++faults;
  }
  printf("random riscv: %llu streams, %llu instructions, %llu translated blocks entered, %llu ended in a fault\n", (unsigned long long)seeds, (unsigned long long)retired, (unsigned long long)entered, (unsigned long long)faults);
  CHECK(entered > seeds);
}

static void test_riscv_loop() {
  // addi x5,x0,1000; addi x6,x0,0; L: add x6,x6,x5; addi x5,x5,-1; bne x5,x0,L; ecall
  Bytes code;
  put32le(code, 0x3E800293);
  put32le(code, 0x00000313);
  put32le(code, 0x00530333);
  put32le(code, 0xFFF28293);
  put32le(code, 0xFE029CE3);
  put32le(code, 0x00000073);
  RvMachine plain(code, false), fast(code, true, 2);
  CHECK(plain.run(100000) == StopReason::Syscall && fast.run(100000) == StopReason::Syscall);
  CHECK(fast.cpu.state().x[6] == 500500 && fast.cpu.state().x[6] == plain.cpu.state().x[6]);
  CHECK(fast.cpu.instructions() == plain.cpu.instructions() && fast.cpu.jit_blocks_entered() > 900);
}


// --- AArch64 ------------------------------------------------------------------------------------------------------------------------------------

struct ArmMachine {
  process::AddressSpace space{A};
  AArch64Cpu cpu;
  ArmMachine(const Bytes& code, bool translated, uint32_t threshold = 1, uint32_t max_block = 32) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtExecute);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(kStack, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::AArch64;
    start.entry = kCode;
    start.stack_pointer = kStack + 0x800;
    cpu.reset(start);
    if (translated) {
      GuestJitOptions options;
      options.hot_threshold = threshold;
      options.max_block = max_block;
      cpu.set_jit(create_aarch64_jit(A, g_code, options));
    }
  }
  StopReason run(uint64_t n) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
};

static uint32_t a_reg(Rng& r) { return r.chance(75) ? r.below(9) : r.below(32); }

static void arm_instruction(Bytes& b, Rng& r) {
  const uint32_t rd = a_reg(r), rn = a_reg(r), rm = a_reg(r);
  const uint32_t sf = r.chance(65) ? 1u << 31 : 0;
  uint32_t insn;
  switch (r.below(26)) {
    case 0: case 1: {      // add / sub immediate, flags or not
      insn = sf | (r.below(2) << 30) | (r.below(2) << 29) | (0x11u << 24) | (r.below(2) << 22) | (r.below(4096) << 10) | (rn << 5) | rd;
      break;
    }
    case 2: case 3: {      // add / sub shifted register
      insn = sf | (r.below(2) << 30) | (r.below(2) << 29) | (0x0Bu << 24) | (r.below(3) << 22) | (rm << 16) | (r.below(sf ? 64 : 32) << 10) | (rn << 5) | rd;
      break;
    }
    case 4: {      // add / sub extended register
      insn = sf | (r.below(2) << 30) | (r.below(2) << 29) | (0x0Bu << 24) | (1u << 21) | (rm << 16) | (r.below(8) << 13) | (r.below(5) << 10) | (rn << 5) | rd;
      break;
    }
    case 5: case 6: {      // logical shifted register
      insn = sf | (r.below(4) << 29) | (0x0Au << 24) | (r.below(4) << 22) | (r.below(2) << 21) | (rm << 16) | (r.below(sf ? 64 : 32) << 10) | (rn << 5) | rd;
      break;
    }
    case 7: case 8: {      // logical immediate
      insn = sf | (r.below(4) << 29) | (0x24u << 23) | ((sf ? r.below(2) : 0) << 22) | (r.below(64) << 16) | (r.below(64) << 10) | (rn << 5) | rd;
      break;
    }
    case 9: case 10: {     // move wide
      insn = sf | (r.below(4) << 29) | (0x25u << 23) | (r.below(sf ? 4 : 2) << 21) | (r.below(0x10000) << 5) | rd;
      break;
    }
    case 11: case 12: {    // bitfield
      insn = sf | (r.below(3) << 29) | (0x26u << 23) | ((sf ? 1u : 0u) << 22) | (r.below(sf ? 64 : 32) << 16) | (r.below(sf ? 64 : 32) << 10) | (rn << 5) | rd;
      break;
    }
    case 13: insn = (r.below(2) << 31) | (r.below(4) << 29) | (0x10u << 24) | (r.below(0x80000) << 5) | rd; break;      // adr, adrp
    case 14: case 15: {    // conditional select
      insn = sf | (r.below(2) << 30) | (0xD4u << 21) | (rm << 16) | (r.below(16) << 12) | (r.below(2) << 10) | (rn << 5) | rd;
      break;
    }
    case 16: insn = sf | (0xD6u << 21) | (rm << 16) | ((8 + r.below(4)) << 10) | (rn << 5) | rd; break;      // variable shifts
    case 17: {             // madd, msub, smaddl, umaddl
      const uint32_t op31 = r.below(3) == 0 ? (r.chance(50) ? 1 : 5) : 0;
      insn = ((op31 ? 1u : sf >> 31) << 31) | (0x1Bu << 24) | (op31 << 21) | (rm << 16) | (r.below(2) << 15) | (a_reg(r) << 10) | (rn << 5) | rd;
      break;
    }
    case 18: case 19: case 20: {      // loads and stores: unsigned offset, imm9 forms, register offset
      const uint32_t size = r.below(4), kind = r.below(3);
      uint32_t base = (size << 30) | (7u << 27) | (r.below(4) << 22) | (rn << 5) | rd;
      if (kind == 0) base |= (1u << 24) | (r.below(0x40) << 10);
      else if (kind == 1) base |= (r.below(0x100) << 12) | (r.below(4) << 10);
      else base |= (1u << 21) | (rm << 16) | (((r.below(2) ? 3u : 2u) | (r.below(2) ? 4u : 0)) << 13) | (r.below(2) << 12) | (2u << 10);
      insn = base;
      break;
    }
    case 21: case 22: {    // pairs
      insn = ((r.chance(50) ? 2u : 0u) << 30) | (5u << 27) | ((1 + r.below(3)) << 23) | (r.below(2) << 22) | (r.below(0x7F) << 15) | (a_reg(r) << 10) | (rn << 5) | rd;
      break;
    }
    case 23: {             // branches: b, bl, b.cond, cbz, tbz
      const uint32_t k = r.below(5);
      const int32_t off = int32_t(r.below(30)) - 8;
      if (k == 0) insn = ((r.below(2) ? 0x94000000u : 0x14000000u)) | (uint32_t(off) & 0x3FFFFFF);
      else if (k == 1) insn = 0x54000000u | ((uint32_t(off) & 0x7FFFF) << 5) | r.below(16);
      else if (k == 2) insn = sf | (0x34000000u) | (r.below(2) << 24) | ((uint32_t(off) & 0x7FFFF) << 5) | rd;
      else if (k == 3) insn = (r.below(2) << 31) | 0x36000000u | (r.below(2) << 24) | (r.below(32) << 19) | ((uint32_t(off) & 0x3FFF) << 5) | rd;
      else insn = (r.chance(50) ? 0xD65F0000u : r.chance(50) ? 0xD61F0000u : 0xD63F0000u) | (rn << 5);
      break;
    }
    case 24: insn = 0xD503201Fu; break;
    default: insn = uint32_t(r.next()); break;
  }
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(insn >> (8 * i)));
}

static void arm_init(ArmMachine& m, Rng& r) {
  for (int i = 0; i < 31; ++i) {
    const uint32_t k = r.below(100);
    uint64_t v;
    if (k < 70) v = kData + r.below(0x800);
    else if (k < 78) v = kStack + 0x400 + 8 * r.below(64);
    else if (k < 88) v = r.below(300);
    else if (k < 93) v = ~uint64_t{0} - r.below(5);
    else if (k < 96) v = uint64_t{r.below(256)} << 56 | (kData + r.below(0x100));      // a tagged pointer
    else v = r.next();
    if (r.chance(25)) v = edge_value(r);
    else if (i > 0 && r.chance(10)) v = m.cpu.state().x[i - 1];
    m.cpu.state().x[i] = v;
  }
  m.cpu.state().nzcv = r.below(16) << 28;
  Bytes fill(0x2000);
  for (auto& v : fill) v = uint8_t(r.next());
  m.space.write_privileged(kData, ConstByteView(fill.data(), fill.size()));
  Bytes stack(0x1000);
  for (auto& v : stack) v = uint8_t(r.next());
  m.space.write_privileged(kStack, ConstByteView(stack.data(), stack.size()));
}

static bool arm_same(ArmMachine& a, ArmMachine& b, StopReason ra, StopReason rb, const char** what) {
  if (ra != rb) { *what = "stop reason"; return false; }
  for (int i = 0; i < 31; ++i)
    if (a.cpu.state().x[i] != b.cpu.state().x[i]) { *what = "register"; return false; }
  if (a.cpu.state().sp != b.cpu.state().sp) { *what = "sp"; return false; }
  if (a.cpu.state().pc != b.cpu.state().pc) { *what = "pc"; return false; }
  if (a.cpu.state().nzcv != b.cpu.state().nzcv) { *what = "nzcv"; return false; }
  if (a.cpu.instructions() != b.cpu.instructions()) { *what = "instruction count"; return false; }
  if (ra == StopReason::Fault) {
    const GuestFault& fa = a.cpu.fault();
    const GuestFault& fb = b.cpu.fault();
    if (fa.vector != fb.vector || fa.address != fb.address || fa.pc != fb.pc) { *what = "fault"; return false; }
  }
  Bytes da(0x3000), db(0x3000);
  a.space.read_privileged(kData, ByteView(da.data(), 0x2000));
  a.space.read_privileged(kStack, ByteView(da.data() + 0x2000, 0x1000));
  b.space.read_privileged(kData, ByteView(db.data(), 0x2000));
  b.space.read_privileged(kStack, ByteView(db.data() + 0x2000, 0x1000));
  if (da != db) { *what = "memory"; return false; }
  return true;
}

static void test_random_aarch64(uint64_t seeds, uint64_t first_seed) {
  uint64_t entered = 0, faults = 0, retired = 0;
  for (uint64_t seed = first_seed; seed < first_seed + seeds; ++seed) {
    Rng r{seed * 0x9E3779B97F4A7C15ull + 4242};
    for (int i = 0; i < 4; ++i) r.next();
    Bytes code;
    const uint32_t n = 40 + r.below(80);
    for (uint32_t i = 0; i < n; ++i) arm_instruction(code, r);
    if (code.size() > 0x1000) code.resize(0x1000);
    const uint64_t init_seed = r.next();
    ArmMachine plain(code, false), fast(code, true, 1 + (seed % 3), (seed % 5 == 0) ? 1 + uint32_t(seed % 7) : 32);
    Rng ra{init_seed | 1}, rb{init_seed | 1};
    arm_init(plain, ra);
    arm_init(fast, rb);
    StopReason sa = StopReason::Budget, sb = StopReason::Budget;
    uint64_t left = 2500;
    const uint64_t chunk = (seed % 4 == 0) ? 77 : 2500;
    while (left) {
      const uint64_t step = left < chunk ? left : chunk;
      sa = plain.run(step);
      sb = fast.run(step);
      left -= step;
      if (sa != StopReason::Budget || sb != StopReason::Budget) break;
    }
    const char* what = "";
    if (!arm_same(plain, fast, sa, sb, &what)) {
      printf("FAILED aarch64 seed %llu: %s differs (stop %d/%d)\n  code:", (unsigned long long)seed, what, int(sa), int(sb));
      for (size_t i = 0; i + 3 < code.size(); i += 4) printf(" %02x%02x%02x%02x", code[i + 3], code[i + 2], code[i + 1], code[i]);
      printf("\n  pc %llx/%llx nzcv %x/%x\n", (unsigned long long)plain.cpu.state().pc, (unsigned long long)fast.cpu.state().pc, plain.cpu.state().nzcv, fast.cpu.state().nzcv);
      for (int i = 0; i < 31; ++i) if (plain.cpu.state().x[i] != fast.cpu.state().x[i]) printf("  x%d %llx/%llx\n", i, (unsigned long long)plain.cpu.state().x[i], (unsigned long long)fast.cpu.state().x[i]);
      ++g_failed;
      if (g_failed > 5) return;
    }
    entered += fast.cpu.jit_blocks_entered();
    retired += fast.cpu.instructions();
    if (sb == StopReason::Fault) ++faults;
  }
  printf("random aarch64: %llu streams, %llu instructions, %llu translated blocks entered, %llu ended in a fault\n", (unsigned long long)seeds, (unsigned long long)retired, (unsigned long long)entered, (unsigned long long)faults);
  CHECK(entered > seeds);
}

static void test_aarch64_loop() {
  // mov x1, #1000; mov x0, #0; L: add x0, x0, x1; subs x1, x1, #1; b.ne L; svc #0
  Bytes code;
  for (uint32_t insn : {0xD2807D01u, 0xD2800000u, 0x8B010000u, 0xF1000421u, 0x54FFFFC1u, 0xD4000001u})
    for (int i = 0; i < 4; ++i) code.push_back(uint8_t(insn >> (8 * i)));
  ArmMachine plain(code, false), fast(code, true, 2);
  CHECK(plain.run(100000) == StopReason::Syscall && fast.run(100000) == StopReason::Syscall);
  CHECK(fast.cpu.state().x[0] == 500500 && fast.cpu.state().x[0] == plain.cpu.state().x[0] && fast.cpu.state().nzcv == plain.cpu.state().nzcv);
  CHECK(fast.cpu.instructions() == plain.cpu.instructions() && fast.cpu.jit_blocks_entered() > 900);
}


// Every condition code after every flag-setting operation, on operands at the edges of the arithmetic: the translated code evaluates most of them straight from the
// operands, and must agree with the flags the interpreter computes.
static void put64(Bytes& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

static void test_conditions_x86() {
  Rng r{0x1234567};
  uint64_t cases = 0;
  // op: 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp, 8 test, 9 inc, 10 dec, 11 neg, 12 shl 1, 13 shr 1, 14 sar 1, 15 imul
  for (uint32_t op = 0; op <= 15; ++op) {
    for (uint32_t size : {1u, 2u, 4u, 8u}) {
      for (uint32_t pair = 0; pair < 40; ++pair) {
        const uint64_t a = edge_value(r), b = r.chance(25) ? a : edge_value(r);
        Bytes code;
        code.push_back(0x48); code.push_back(0xB8); put64(code, a);      // mov rax, a
        code.push_back(0x48); code.push_back(0xBB); put64(code, b);      // mov rbx, b
        auto prefix = [&](bool rex_w) { if (size == 2) code.push_back(0x66); if (size == 8 && rex_w) code.push_back(0x48); };
        const uint32_t wide = size == 1 ? 0 : 1;      // the opcode's size bit
        if (op <= 7 && op != 2 && op != 3) {      // alu rax, rbx
          prefix(true);
          code.push_back(uint8_t((op << 3) | wide));
          code.push_back(0xD8);
        } else if (op == 8) {
          prefix(true);
          code.push_back(uint8_t(0x84 + wide));
          code.push_back(0xD8);
        } else if (op >= 9 && op <= 11) {      // inc, dec (FE/FF /0 /1), neg (F6/F7 /3)
          prefix(true);
          code.push_back(op == 11 ? uint8_t(0xF6 + wide) : uint8_t(0xFE + wide));
          code.push_back(op == 9 ? 0xC0 : op == 10 ? 0xC8 : 0xD8);
        } else if (op >= 12 && op <= 14) {      // shifts by one (D0/D1)
          prefix(true);
          code.push_back(uint8_t(0xD0 + wide));
          code.push_back(op == 12 ? 0xE0 : op == 13 ? 0xE8 : 0xF8);
        } else if (op == 15 && size > 1) {      // imul rax, rbx
          prefix(true);
          code.push_back(0x0F); code.push_back(0xAF); code.push_back(0xC3);
        } else {
          continue;
        }
        // every condition: setcc into a different byte of the stack, through memory so that no register is involved: setcc [rsp - 16 + cc]
        for (uint32_t cc = 0; cc < 16; ++cc) {
          code.push_back(0x0F); code.push_back(uint8_t(0x90 + cc)); code.push_back(0x44); code.push_back(0x24); code.push_back(uint8_t(0xE0 + cc));
        }
        code.push_back(0x0F); code.push_back(0x05);
        Machine plain(code, false), fast(code, true, 1);
        plain.run(100);
        fast.run(100);
        const char* what = "";
        if (!same_state(plain, fast, StopReason::Syscall, StopReason::Syscall, &what)) {
          printf("FAILED condition test: op %u size %u a %llx b %llx: %s\n", op, size, (unsigned long long)a, (unsigned long long)b, what);
          ++g_failed;
          if (g_failed > 8) return;
        }
        ++cases;
      }
    }
  }
  printf("x86 conditions: %llu operation / operand cases\n", (unsigned long long)cases);
}

static void test_conditions_aarch64() {
  Rng r{0x7654321};
  uint64_t cases = 0;
  for (uint32_t op = 0; op < 6; ++op) {      // adds, subs, ands, adds imm, subs imm, tst-like ands imm
    for (uint32_t sf = 0; sf < 2; ++sf) {
      for (uint32_t pair = 0; pair < 60; ++pair) {
        const uint64_t a = edge_value(r), b = r.chance(25) ? a : edge_value(r);
        Bytes code;
        auto word = [&](uint32_t insn) { for (int i = 0; i < 4; ++i) code.push_back(uint8_t(insn >> (8 * i))); };
        // x1 and x2 are loaded from the data page (the translator reads them as registers)
        const uint32_t sfb = sf << 31;
        const uint32_t imm12 = uint32_t(b) & 0xFFF;
        switch (op) {
          case 0: word(sfb | 0x2B020020); break;       // adds w/x0, w/x1, w/x2
          case 1: word(sfb | 0x6B020020); break;       // subs
          case 2: word(sfb | 0x6A020020); break;       // ands
          case 3: word(sfb | 0x31000000 | (imm12 << 10) | 0x20); break;     // adds imm
          case 4: word(sfb | 0x71000000 | (imm12 << 10) | 0x20); break;     // subs imm
          default: word(sfb | 0x72000020 | (0x0u << 22) | (0x00u << 16) | (0x07u << 10)); break;      // ands imm (#1..8 bits)
        }
        // csel x(3+cc), x5, x6, cc is only 4 free registers: store the 16 conditions as csinc into x9.. through memory instead: cset via csinc xN, xzr, xzr, !cc
        for (uint32_t cc = 0; cc < 16; ++cc) {
          const uint32_t inverted = cc ^ 1;
          word(0x9A9F07E0u | (inverted << 12) | 3);      // csinc x3, xzr, xzr, inverted: x3 = cc ? 1 : 0
          word(0xF9000000u | (4u << 5) | 3 | ((cc) << 10));      // str x3, [x4, #cc*8]
        }
        word(0xD4000001u);
        ArmMachine plain(code, false), fast(code, true, 1);
        for (ArmMachine* m : {&plain, &fast}) {
          m->cpu.state().x[1] = a;
          m->cpu.state().x[2] = op == 3 || op == 4 ? 0 : b;
          m->cpu.state().x[4] = kData;
        }
        plain.run(200);
        fast.run(200);
        const char* what = "";
        if (!arm_same(plain, fast, StopReason::Syscall, StopReason::Syscall, &what) || plain.cpu.state().nzcv != fast.cpu.state().nzcv) {
          printf("FAILED aarch64 condition test: op %u sf %u a %llx b %llx: %s\n", op, sf, (unsigned long long)a, (unsigned long long)b, what);
          ++g_failed;
          if (g_failed > 8) return;
        }
        ++cases;
      }
    }
  }
  printf("aarch64 conditions: %llu operation / operand cases\n", (unsigned long long)cases);
}

// --- whole programs --------------------------------------------------------------------------------------------------------------------------

static Bytes read_file(const char* path) {
  Bytes out;
  if (FILE* f = fopen(path, "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
  }
  return out;
}

static std::string run_elf(const Bytes& elf, bool translated, uint64_t slice, bool threads, EmulatedOutcome* outcome, uint64_t* blocks) {
  FakeHost host;
  process::UserProgram program(A, 100, host);
  const StringView argv[] = {"/guest"};
  const uint8_t random[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  if (program.load(ConstByteView(elf.data(), elf.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) != process::LoadStatus::Ok) return std::string();
  UniquePointer<IGuestCpu> cpu = create_guest_cpu(program.start().arch, A);
  if (!cpu) return std::string();
  if (translated) {
    GuestJitOptions options;
    options.hot_threshold = 2;
    enable_jit(*cpu.get(), A, g_code, options);
  }
  NeverStop control;
  if (threads) {
    HostSync sync;
    HostThreads spawner;
    *outcome = run_emulated_threads(program, *cpu.get(), control, sync, spawner, A, slice);
  } else {
    *outcome = run_emulated(program, *cpu.get(), control, slice);
  }
  *blocks = 0;
  if (translated && cpu->arch() == process::UserArch::X86_64) *blocks = static_cast<X86_64Cpu*>(cpu.get())->jit_blocks_entered();
  if (translated && cpu->arch() == process::UserArch::RiscV64) *blocks = static_cast<RiscV64Cpu*>(cpu.get())->jit_blocks_entered();
  if (translated && cpu->arch() == process::UserArch::AArch64) *blocks = static_cast<AArch64Cpu*>(cpu.get())->jit_blocks_entered();
  return host.out;
}

static void test_program(const char* elf_path, const char* expected_path, const char* what, bool threads, bool expect_translation) {
  const Bytes elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  const std::string want(expected.begin(), expected.end());
  for (uint64_t slice : {uint64_t{20000}, uint64_t{1000}, uint64_t{37}}) {
    EmulatedOutcome outcome;
    uint64_t blocks = 0;
    const std::string out = run_elf(elf, true, slice, threads, &outcome, &blocks);
    CHECK(outcome.exited && !outcome.faulted);
    CHECK(out == want);
    if (out != want) printf("--- %s (slice %llu) got\n%s--- want\n%s", what, (unsigned long long)slice, out.c_str(), want.c_str());
    if (expect_translation && slice >= 1000) CHECK(blocks > 50);
    if (slice == 20000) printf("%s: %llu translated blocks entered\n", what, (unsigned long long)blocks);
  }
}

int main(int argc, char** argv) {
  uint64_t seeds = 3000, first = 1;
  if (const char* env = getenv("JIT_SEEDS")) seeds = strtoull(env, nullptr, 10);
  if (const char* env = getenv("JIT_FIRST_SEED")) first = strtoull(env, nullptr, 10);
  if (const char* env = getenv("JIT_BLOCKS")) {
    bisect_block(strtoull(env, nullptr, 10));
    return 0;
  }
  if (const char* env = getenv("JIT_BISECT")) {
    bisect(strtoull(env, nullptr, 10));
    return 0;
  }
  test_loop();
  test_fault_precision();
  test_self_modifying();
  test_remap();
  test_conditions_x86();
  test_conditions_aarch64();
  test_aarch64_loop();
  test_random_aarch64(seeds, first);
  test_riscv_loop();
  test_random_riscv(seeds, first);
  test_random(seeds, first, true);
  test_random(seeds, first, false);
  if (argc >= 3) test_program(argv[1], argv[2], "guest", false, true);
  if (argc >= 5) test_program(argv[3], argv[4], "threads", true, false);
  if (argc >= 7) test_program(argv[5], argv[6], "x87", false, false);
  if (argc >= 9) test_program(argv[7], argv[8], "sse4", false, false);
  if (argc >= 10) test_program(argv[9], argv[2], "guest (riscv64)", false, true);
  if (argc >= 11) test_program(argv[10], argv[4], "threads (riscv64)", true, false);
  if (argc >= 12) test_program(argv[11], argv[2], "guest (aarch64)", false, true);
  if (argc >= 13) test_program(argv[12], argv[4], "threads (aarch64)", true, false);
  if (A.live.load() != 0) {
    printf("FAILED: %d allocations leaked\n", A.live.load());
    ++g_failed;
  }
  printf(g_failed ? "FAILED (%d)\n" : "OK\n", g_failed);
  return g_failed ? 1 : 0;
}
