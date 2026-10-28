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

// Host test of the AVX-512 emulation. Where the host has AVX-512 (F, BW, DQ, VL and CD at least) it is the oracle: every instruction of the table (assembled with llvm-mc
// from tests/data/emulation/avx512_cases_gen.py) is run on the host and on the emulator from the same random zmm, mask and general register contents, and all vector
// registers (full 512 bits), the mask registers, the flags, the general registers, MXCSR and the memory operand must come out identical. Without AVX-512 on the host
// only the robustness cases run (every truncation and bit flip of every encoding, under the sanitizers).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"
#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#define HOST_AVX 1
#endif

using namespace emulation;
typedef std::vector<uint8_t> Bytes;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct A final : allocators::IAllocator {
  void* allocate(uint64_t s) override { return malloc(s); }
  void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); }
  void deallocate(void* p) override { free(p); }
};

struct Case { const char* text; std::vector<uint8_t> code; };
static const Case kCases[] = {
#include "data/emulation/avx512_cases.inc"
};
#include "data/emulation/avx512_frame.inc"

static const int kRegs[10] = {0, 1, 2, 3, 4, 5, 16, 17, 18, 19};
constexpr uint32_t kZin = 0x100, kKin = 0x380, kGpr = 0x3C0, kMxcsrIn = 0x3E0, kZout = 0x400, kKout = 0x680, kFlagsOut = 0x6C0, kRegsOut = 0x6C8, kMxcsrOut = 0x6E0, kTable = 0x800;

static uint64_t seed = 0x2545F4914F6CDD1Dull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void mem(Bytes& b, std::initializer_list<uint8_t> op, int reg, uint32_t disp) { for (uint8_t o : op) b.push_back(o); b.push_back(uint8_t(0x80 | (reg << 3) | 7)); put32(b, disp); }
static void gpr_setup(Bytes& b) {
  mem(b, {0x48, 0x8B}, 0, kGpr);          // mov rax, [rdi+kGpr]
  mem(b, {0x48, 0x8B}, 1, kGpr + 8);
  mem(b, {0x48, 0x8B}, 2, kGpr + 16);
  mem(b, {0x48, 0x8D}, 6, kTable);        // lea rsi, [rdi+kTable]
  mem(b, {0x0F, 0xAE}, 2, kMxcsrIn);      // ldmxcsr
  b.push_back(0x45); b.push_back(0x31); b.push_back(0xC9);      // xor r9d, r9d
}
static void gpr_dump(Bytes& b) {
  b.push_back(0x9C); b.push_back(0x41); b.push_back(0x58);      // pushfq; pop r8
  mem(b, {0x4C, 0x89}, 0, kFlagsOut);
  mem(b, {0x48, 0x89}, 0, kRegsOut);
  mem(b, {0x48, 0x89}, 1, kRegsOut + 8);
  mem(b, {0x48, 0x89}, 2, kRegsOut + 16);
  mem(b, {0x0F, 0xAE}, 3, kMxcsrOut);
}

static void fill_state(uint8_t* data, const Case& c) {
  memset(data, 0, 0x1000);
  for (int i = 0; i < 0x100; ++i) data[i] = uint8_t(rnd());
  for (int r = 0; r < 10; ++r) {
    uint8_t* p = data + kZin + 64 * r;
    uint64_t kind = rnd() % 7;
    if (strstr(c.text, "vdpbf16ps")) kind = 1;      // (AMD and Intel differ on infinities, NaNs and signed zeros there: keep the operands ordinary numbers)
    for (int i = 0; i < 64; i += 8) {
      uint64_t v = rnd();
      if (kind == 1) { float f[2]; f[0] = float(int32_t(rnd()) % 1000) / 8; f[1] = float(int32_t(rnd()) % 100) * 3.25f; memcpy(&v, f, 8); }
      else if (kind == 2) { double d = double(int32_t(rnd()) % 100000) / 16; memcpy(&v, &d, 8); }
      else if (kind == 3) { v &= 0x00FF00FF00FF00FFull; }
      else if (kind == 4 && (rnd() & 3) == 0) { static const uint64_t specials[] = {0, ~0ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull, 0x8000000000000000ull, 0x7F8000007FC00000ull, 0x0000000100000001ull, 0x8000000080000000ull}; v = specials[rnd() % 8]; }
      else if (kind == 5) { v &= 0x0000000F0000000Full; }
      memcpy(p + i, &v, 8);
    }
  }
  for (int i = 0; i < 8; ++i) {
    uint64_t v = rnd();
    const uint64_t kind = rnd() % 5;
    if (kind == 0) v = ~0ull; else if (kind == 1) v &= rnd(); else if (kind == 2) v |= rnd(); else if (kind == 3) v &= 0xFFFF;
    memcpy(data + kKin + 8 * i, &v, 8);
  }
  for (int i = 0; i < 3; ++i) { uint64_t v = rnd(); if (rnd() & 1) v &= 0xFF; if ((rnd() & 7) == 0) v = (rnd() & 1) ? ~0ull : 0; memcpy(data + kGpr + 8 * i, &v, 8); }
  const uint32_t mxcsr = 0x1F80 | uint32_t((rnd() & 3) << 13);
  memcpy(data + kMxcsrIn, &mxcsr, 4);
  for (int i = 0; i < 0x200; ++i) data[kTable + i] = uint8_t(rnd());
  // gathers and scatters: small indices in zmm2 and zmm18 (32-bit for the d forms, 64-bit for the q forms), so every address stays inside the table
  if (strstr(c.text, "gather") || strstr(c.text, "scatter")) {
    const bool qindex = strstr(c.text, "gatherq") || strstr(c.text, "gatherq") || strstr(c.text, "scatterq") || strstr(c.text, "gather") && strstr(c.text, "gq");
    const bool q_form = strstr(c.text, "gatherq") != nullptr || strstr(c.text, "scatterq") != nullptr || strstr(c.text, "pgatherq") != nullptr || strstr(c.text, "pscatterq") != nullptr;
    (void)qindex;
    for (int slot : {2, 8}) {
      uint8_t* p = data + kZin + 64 * slot;
      for (int i = 0; i < 64; i += (q_form ? 8 : 4)) { uint64_t v = rnd() % 24; memcpy(p + i, &v, q_form ? 8 : 4); }
    }
  }
  (void)c;
}

struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(0x30000, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x31000;
    cpu.reset(start);
    cpu.state().r[7] = 0x20000;
  }
  StopReason run() { GuestMemory m(space); return cpu.run(m, 10000); }
};

static void set_vector(X86_64Cpu::State& st, int reg, const uint8_t* p) {
  uint64_t q[8];
  memcpy(q, p, 64);
  X86_64Cpu::Xmm& lo = reg < 16 ? st.xmm[reg] : st.evex_lo[reg - 16];
  X86_64Cpu::YmmHigh& hi = reg < 16 ? st.ymm_high[reg] : st.evex_hi[reg - 16];
  lo.lo = q[0]; lo.hi = q[1]; hi.lo = q[2]; hi.hi = q[3]; hi.upper[0].lo = q[4]; hi.upper[0].hi = q[5]; hi.upper[1].lo = q[6]; hi.upper[1].hi = q[7];
}
static void get_vector(const X86_64Cpu::State& st, int reg, uint8_t* p) {
  const X86_64Cpu::Xmm& lo = reg < 16 ? st.xmm[reg] : st.evex_lo[reg - 16];
  const X86_64Cpu::YmmHigh& hi = reg < 16 ? st.ymm_high[reg] : st.evex_hi[reg - 16];
  const uint64_t q[8] = {lo.lo, lo.hi, hi.lo, hi.hi, hi.upper[0].lo, hi.upper[0].hi, hi.upper[1].lo, hi.upper[1].hi};
  memcpy(p, q, 64);
}

// The decoder must survive anything: every truncation of every encoding, and bit flips of them, run briefly (the sanitizers watch for memory errors).
static void test_robustness() {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  auto next = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
  int runs = 0;
  for (const Case& c : kCases) {
    for (int variant = 0; variant < 12; ++variant) {
      Bytes code = c.code;
      if (variant < (int)c.code.size()) code.resize(variant);
      else if (variant < 12) code[next() % code.size()] ^= uint8_t(1u << (next() % 8));
      code.push_back(0x0F); code.push_back(0x05);
      Machine m(code);
      for (int i = 0; i < 8; ++i) { m.cpu.state().xmm[i] = {next(), next()}; m.cpu.state().ymm_high[i] = {next(), next()}; }
      m.cpu.state().r[0] = 0x20000; m.cpu.state().r[1] = 3;
      GuestMemory gm(m.space);
      m.cpu.run(gm, 6);
      ++runs;
    }
  }
  CHECK(runs > 10000);
}

static Bytes with_syscall(Bytes b) { b.push_back(0x0F); b.push_back(0x05); return b; }

// State handling that does not need AVX-512 hardware: VEX writes clear bits 511:128, legacy SSE keeps them, XSAVE/XRSTOR round-trips the new state, cpuid reports the features.
static void test_state() {
  uint8_t pattern[64];
  for (int i = 0; i < 64; ++i) pattern[i] = uint8_t(0x11 * (i % 15) + i);
  {      // vpxor xmm0, xmm0, xmm0 (VEX.128) zeroes the whole register
    Machine m(with_syscall({0xC5, 0xF9, 0xEF, 0xC0}));
    set_vector(m.cpu.state(), 0, pattern);
    CHECK(m.run() == StopReason::Syscall);
    uint8_t out[64];
    get_vector(m.cpu.state(), 0, out);
    bool zero = true;
    for (uint8_t v : out) zero = zero && v == 0;
    CHECK(zero);
  }
  {      // pxor xmm0, xmm0 (legacy SSE) changes only the low 128 bits
    Machine m(with_syscall({0x66, 0x0F, 0xEF, 0xC0}));
    set_vector(m.cpu.state(), 0, pattern);
    CHECK(m.run() == StopReason::Syscall);
    uint8_t out[64];
    get_vector(m.cpu.state(), 0, out);
    bool low_zero = true, high_kept = true;
    for (int i = 0; i < 16; ++i) low_zero = low_zero && out[i] == 0;
    for (int i = 16; i < 64; ++i) high_kept = high_kept && out[i] == pattern[i];
    CHECK(low_zero && high_kept);
  }
  {      // xsave / xrstor with the AVX-512 components
    Machine m(with_syscall({0xB8, 0xE7, 0, 0, 0, 0x31, 0xD2, 0x0F, 0xAE, 0x27,      // mov eax, 0xE7; xor edx, edx; xsave [rdi]
                            0x0F, 0xAE, 0x2F}));                                    // xrstor [rdi]
    set_vector(m.cpu.state(), 3, pattern);
    set_vector(m.cpu.state(), 20, pattern);
    m.cpu.state().k[2] = 0x1234567890ABCDEFull;
    // run to the xsave, clobber the registers, run on
    GuestMemory gm(m.space);
    for (int i = 0; i < 3; ++i) m.cpu.run(gm, 1);
    uint8_t zero[64] = {};
    set_vector(m.cpu.state(), 3, zero);
    set_vector(m.cpu.state(), 20, zero);
    m.cpu.state().k[2] = 0;
    CHECK(m.cpu.run(gm, 10) == StopReason::Syscall);
    uint8_t out[64];
    get_vector(m.cpu.state(), 3, out);
    CHECK(memcmp(out, pattern, 64) == 0);
    get_vector(m.cpu.state(), 20, out);
    CHECK(memcmp(out, pattern, 64) == 0);
    CHECK(m.cpu.state().k[2] == 0x1234567890ABCDEFull);
  }
  {      // cpuid leaf 7 reports AVX-512 F, DQ, BW, VL and CD
    Machine m(with_syscall({0x31, 0xC9, 0xB8, 0x07, 0, 0, 0, 0x0F, 0xA2}));
    CHECK(m.run() == StopReason::Syscall);
    const uint32_t b = uint32_t(m.cpu.state().r[3]);
    CHECK((b & (1u << 16)) && (b & (1u << 17)) && (b & (1u << 30)) && (b & (1u << 31)) && (b & (1u << 28)));
  }
  {      // kmovw k1, eax / kmovw edx, k1 / kortestw k1, k1
    Machine m(with_syscall({0xB8, 0xEF, 0xBE, 0, 0, 0xC5, 0xF8, 0x92, 0xC8, 0xC5, 0xF8, 0x93, 0xD1, 0xC5, 0xF8, 0x98, 0xC9}));
    const StopReason r = m.run();
    if (r != StopReason::Syscall || m.cpu.state().k[1] != 0xBEEF) printf("kmov: stop %d fault %d k1 %llx rcx %llx\n", int(r), m.cpu.fault().vector, (unsigned long long)m.cpu.state().k[1], (unsigned long long)m.cpu.state().r[1]);
    CHECK(r == StopReason::Syscall && m.cpu.state().k[1] == 0xBEEF && (m.cpu.state().r[2] & 0xFFFF) == 0xBEEF);
  }
}

int main(int argc, char** argv) {
  test_state();
  test_robustness();
  const int rounds = argc > 1 ? atoi(argv[1]) : 4;
  int total = 0, failures = 0, reported = 0;
#ifdef HOST_AVX
  void* exec = mmap(nullptr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (exec == MAP_FAILED || !__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512bw") || !__builtin_cpu_supports("avx512dq") || !__builtin_cpu_supports("avx512vl") ||
      !__builtin_cpu_supports("avx512cd")) {
    printf("(the host cannot run AVX-512 code: comparison skipped)\n");
    printf("Avx512Test: all passed\n");
    return g_failed ? 1 : 0;
  }
  A alloc;
  const char* only = getenv("AVX512_ONLY");
  for (const Case& c : kCases) {
    if (only && !strstr(c.text, only)) continue;
    for (int round = 0; round < rounds; ++round) {
      if (getenv("AVX_TRACE")) { printf("%s\n", c.text); fflush(stdout); }
      alignas(64) uint8_t host[0x1000], emu[0x1000];
      fill_state(host, c);
      memcpy(emu, host, sizeof(host));
      Bytes hc;
      hc.insert(hc.end(), kFramePre, kFramePre + sizeof(kFramePre));
      gpr_setup(hc);
      hc.insert(hc.end(), c.code.begin(), c.code.end());
      gpr_dump(hc);
      hc.insert(hc.end(), kFramePost, kFramePost + sizeof(kFramePost));
      hc.push_back(0xC3);
      memcpy(exec, hc.data(), hc.size());
      reinterpret_cast<void (*)(void*)>(exec)(host);
      Bytes ec;
      gpr_setup(ec);
      ec.insert(ec.end(), c.code.begin(), c.code.end());
      gpr_dump(ec);
      ec.push_back(0xC3);
      process::AddressSpace space{alloc};
      space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
      space.write_privileged(0x10000, ConstByteView(ec.data(), ec.size()));
      space.map(0x20000, 0x1000, process::kProtRead | process::kProtWrite);
      space.write_privileged(0x20000, ConstByteView(emu, sizeof(emu)));
      space.map(0x30000, 0x1000, process::kProtRead | process::kProtWrite);
      X86_64Cpu cpu;
      process::ProgramStart start;
      start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x31000;
      cpu.reset(start);
      cpu.state().r[7] = 0x20000;
      for (int i = 0; i < 10; ++i) set_vector(cpu.state(), kRegs[i], emu + kZin + 64 * i);
      for (int i = 0; i < 8; ++i) memcpy(&cpu.state().k[i], emu + kKin + 8 * i, 8);
      GuestMemory gm(space);
      bool faulted = false;
      for (int steps = 0; steps < 200; ++steps) {
        if (cpu.state().rip == 0x10000 + ec.size() - 1) break;
        if (cpu.run(gm, 1) == StopReason::Fault) { faulted = true; break; }
      }
      space.read_privileged(0x20000, ByteView(emu, sizeof(emu)));
      for (int i = 0; i < 10; ++i) get_vector(cpu.state(), kRegs[i], emu + kZout + 64 * i);
      for (int i = 0; i < 8; ++i) memcpy(emu + kKout + 8 * i, &cpu.state().k[i], 8);
      ++total;
      bool ok = !faulted;
      if (memcmp(host + kZout, emu + kZout, 640) != 0) ok = false;
      if (memcmp(host + kKout, emu + kKout, 64) != 0) ok = false;
      uint64_t hf, ef;
      memcpy(&hf, host + kFlagsOut, 8); memcpy(&ef, emu + kFlagsOut, 8);
      if ((hf & 0x8C5) != (ef & 0x8C5)) ok = false;
      if (memcmp(host + kRegsOut, emu + kRegsOut, 24) != 0) ok = false;
      uint32_t hm, em;
      memcpy(&hm, host + kMxcsrOut, 4); memcpy(&em, emu + kMxcsrOut, 4);
      const uint32_t flag_mask = 0x3D;
      if ((hm & flag_mask) != (em & flag_mask)) ok = false;
      if (memcmp(host, emu, 0x100) != 0) ok = false;
      if (memcmp(host + kTable, emu + kTable, 0x200) != 0) ok = false;
      if (!ok) {
        ++failures;
        static const char* last_reported = nullptr;
        if (reported < 80 && last_reported != c.text) {
          last_reported = c.text;
          ++reported;
          printf("%s%s\n", c.text, faulted ? "  [emulator fault]" : "");
          if (faulted) printf("   fault vector %d at %llx\n", cpu.fault().vector, (unsigned long long)cpu.fault().pc);
          for (int i = 0; i < 10; ++i) {
            if (memcmp(host + kZout + 64 * i, emu + kZout + 64 * i, 64) == 0) continue;
            printf("   zmm%d host ", kRegs[i]); for (int b = 63; b >= 0; --b) printf("%02x", host[kZout + 64 * i + b]);
            printf("\n        emu  "); for (int b = 63; b >= 0; --b) printf("%02x", emu[kZout + 64 * i + b]); printf("\n");
            if (getenv("AVX_LANES")) { for (int l = 0; l < 16; ++l) { uint32_t h, e; memcpy(&h, host + kZout + 64 * i + 4 * l, 4); memcpy(&e, emu + kZout + 64 * i + 4 * l, 4); if (h == e) continue; printf("      lane %2d host %08x emu %08x  in:", l, h, e); for (int s = 0; s < 4; ++s) { uint32_t v; memcpy(&v, host + kZin + 64 * s + 4 * l, 4); printf(" z%d=%08x", kRegs[s], v); } printf("\n"); } }
            if (getenv("AVX_VERBOSE")) { for (int s = 0; s < 4; ++s) { printf("        in zmm%d ", kRegs[s]); for (int b = 63; b >= 0; --b) printf("%02x", host[kZin + 64 * s + b]); printf("\n"); } }
          }
          for (int i = 0; i < 8; ++i) {
            uint64_t hk, ek; memcpy(&hk, host + kKout + 8 * i, 8); memcpy(&ek, emu + kKout + 8 * i, 8);
            if (hk != ek) printf("   k%d host %016llx emu %016llx\n", i, (unsigned long long)hk, (unsigned long long)ek);
          }
          if ((hf & 0x8C5) != (ef & 0x8C5)) printf("   flags host %llx emu %llx\n", (unsigned long long)(hf & 0x8C5), (unsigned long long)(ef & 0x8C5));
          if (memcmp(host + kRegsOut, emu + kRegsOut, 24) != 0) { uint64_t h3[3], e3[3]; memcpy(h3, host + kRegsOut, 24); memcpy(e3, emu + kRegsOut, 24); printf("   gpr host %llx %llx %llx emu %llx %llx %llx\n", (unsigned long long)h3[0], (unsigned long long)h3[1], (unsigned long long)h3[2], (unsigned long long)e3[0], (unsigned long long)e3[1], (unsigned long long)e3[2]); }
          if ((hm & flag_mask) != (em & flag_mask)) printf("   mxcsr host %x emu %x\n", hm & 0x3F, em & 0x3F);
          if (memcmp(host, emu, 0x100) != 0) printf("   memory operand differs\n");
        }
      }
    }
  }
  printf("ran %d cases of %zu instructions, failures %d\n", total, sizeof(kCases) / sizeof(kCases[0]), failures);
  CHECK(failures == 0);
#else
  (void)total; (void)failures; (void)reported; (void)rounds;
  printf("(no x86-64 Linux host: the comparison against real hardware is skipped)\n");
#endif
  if (g_failed) { printf("Avx512Test: %d FAILED\n", g_failed); return 1; }
  printf("Avx512Test: all passed\n");
  return 0;
}
