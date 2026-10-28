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

// Host test of the Xeon Phi AVX512_4VNNIW instructions (vp4dpwssd, vp4dpwssds). The host CPU has no 4VNNIW but has AVX512_VNNI: the result is four chained host vpdpwssd / vpdpwssds
// (the register group at vvvv & ~3 against the dwords of the m128 operand, each broadcast), so wrapping and saturating arithmetic, write masks and untouched registers all come from the
// hardware. Also covers the group alignment, the destination overlapping the group, compressed disp8, and the encodings that must be #UD.
#include <immintrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"

using namespace emulation;
typedef std::vector<uint8_t> Bytes;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct A final : allocators::IAllocator {
  void* allocate(uint64_t s) override { return malloc(s); }
  void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); }
  void deallocate(void* p) override { free(p); }
};

static uint64_t seed = 0x2545F4914F6CDD1Dull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

constexpr uint64_t kBase = 0x20800;      // rdi
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(0x2F000, 0x2000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x30000;
    cpu.reset(start);
    cpu.state().r[7] = kBase;
  }
  StopReason run(uint64_t n = 4) { GuestMemory m(space); return cpu.run(m, n); }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
};

struct Zmm { uint32_t d[16]; };
static void set_reg(X86_64Cpu::State& st, int reg, const Zmm& z) {
  uint64_t q[8];
  memcpy(q, z.d, 64);
  X86_64Cpu::Xmm& lo = st.xmm[reg];
  X86_64Cpu::YmmHigh& hi = st.ymm_high[reg];
  lo.lo = q[0]; lo.hi = q[1]; hi.lo = q[2]; hi.hi = q[3]; hi.upper[0].lo = q[4]; hi.upper[0].hi = q[5]; hi.upper[1].lo = q[6]; hi.upper[1].hi = q[7];
}
static Zmm get_reg(const X86_64Cpu::State& st, int reg) {
  const X86_64Cpu::Xmm& lo = st.xmm[reg];
  const X86_64Cpu::YmmHigh& hi = st.ymm_high[reg];
  const uint64_t q[8] = {lo.lo, lo.hi, hi.lo, hi.hi, hi.upper[0].lo, hi.upper[0].hi, hi.upper[1].lo, hi.upper[1].hi};
  Zmm z;
  memcpy(z.d, q, 64);
  return z;
}

// op 0 vp4dpwssd, 1 vp4dpwssds. reg 0..7, vvvv 0..7; memory [rdi + 64] (disp32) or [rdi + 4 * 16] (disp8).
static Bytes encode(int op, int vl, int reg, int vvvv, bool memory, bool bcst, int aaa, bool z, bool disp8 = false) {
  Bytes b = {0x62, 0xF2, uint8_t(((~vvvv & 15) << 3) | 4 | 3), 0};
  b[3] = uint8_t((z ? 0x80 : 0) | ((vl == 512 ? 2 : vl == 256 ? 1 : 0) << 5) | (bcst ? 0x10 : 0) | 0x08 | aaa);
  b.push_back(op ? 0x53 : 0x52);
  if (memory && disp8) { b.push_back(uint8_t(0x40 | (reg << 3) | 7)); b.push_back(4); }
  else if (memory) { b.push_back(uint8_t(0x80 | (reg << 3) | 7)); for (int i = 0; i < 4; ++i) b.push_back(uint8_t(i == 0 ? 64 : 0)); }
  else b.push_back(uint8_t(0xC0 | (reg << 3) | 3));
  b.push_back(0x0F); b.push_back(0x05);
  return b;
}

static uint32_t rnd_dword() {
  switch (rnd() % 6) {
    case 0: return 0x80008000u;                       // -32768 * -32768 pairs push the sum to the limits
    case 1: return 0x7FFF7FFFu;
    case 2: return (rnd() & 1) ? 0x80007FFFu : 0x7FFF8000u;
    case 3: return uint32_t(rnd() % 7) | (uint32_t(rnd() % 7) << 16);
    default: return uint32_t(rnd());
  }
}

__attribute__((target("avx512f,avx512vnni")))
static Zmm host_chain(const Zmm& acc, const Zmm group[4], const uint32_t w[4], bool saturate) {
  __m512i a;
  memcpy(&a, &acc, 64);
  for (int j = 0; j < 4; ++j) {
    __m512i x;
    memcpy(&x, &group[j], 64);
    const __m512i y = _mm512_set1_epi32(int(w[j]));
    a = saturate ? _mm512_dpwssds_epi32(a, x, y) : _mm512_dpwssd_epi32(a, x, y);
  }
  Zmm r;
  memcpy(&r, &a, 64);
  return r;
}

static void test_operations() {
  for (int iter = 0; iter < 4000; ++iter) {
    const int op = rnd() % 2;
    const int reg = rnd() % 8, vvvv = rnd() % 8;
    const int aaa = rnd() % 3 == 0 ? 0 : 1 + rnd() % 7;
    const bool z = rnd() & 1, disp8 = rnd() & 1;
    Zmm regs[8], mem_block[4];
    for (auto& r : regs) for (auto& d : r.d) d = (rnd() % 4 == 0) ? 0x7FFFFFF0u + uint32_t(rnd() % 32) : rnd_dword();
    for (auto& r : mem_block) for (auto& d : r.d) d = rnd_dword();
    const uint64_t kreg = rnd();
    Machine m(encode(op, 512, reg, vvvv, true, false, aaa, z, disp8));
    X86_64Cpu::State& st = m.cpu.state();
    for (int r = 0; r < 8; ++r) set_reg(st, r, regs[r]);
    m.poke(kBase + 64, mem_block, 64);
    st.k[aaa] = kreg;
    CHECK(m.run() == StopReason::Syscall);

    const int base = vvvv & ~3;
    const Zmm group[4] = {regs[base], regs[base + 1], regs[base + 2], regs[base + 3]};
    Zmm expect = host_chain(regs[reg], group, mem_block[0].d, op);
    for (int i = 0; i < 16; ++i) {
      if (aaa != 0 && !((kreg >> i) & 1)) expect.d[i] = z ? 0 : regs[reg].d[i];
    }
    const Zmm got = get_reg(st, reg);
    if (memcmp(&got, &expect, 64) != 0) {
      printf("MISMATCH op %d reg %d vvvv %d aaa %d z %d\n", op, reg, vvvv, aaa, z);
      for (int i = 0; i < 16; ++i) if (got.d[i] != expect.d[i]) printf("  lane %d got %08x expect %08x\n", i, got.d[i], expect.d[i]);
      ++g_failed;
    }
    for (int r = 0; r < 8; ++r) {
      if (r == reg) continue;
      const Zmm o = get_reg(st, r);
      CHECK(memcmp(&o, &regs[r], 64) == 0);
    }
  }
}

static void test_invalid() {
  {      // a register second operand, broadcast, and the 128/256-bit forms are #UD
    Machine m(encode(0, 512, 2, 4, false, false, 0, false));
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 6);
    Machine m2(encode(0, 512, 2, 4, true, true, 0, false));
    CHECK(m2.run() == StopReason::Fault && m2.cpu.fault().vector == 6);
    Machine m3(encode(1, 256, 2, 4, true, false, 0, false));
    CHECK(m3.run() == StopReason::Fault && m3.cpu.fault().vector == 6);
    Machine m4(encode(1, 128, 2, 4, true, false, 0, false));
    CHECK(m4.run() == StopReason::Fault && m4.cpu.fault().vector == 6);
  }
  {      // an m128 operand that runs off the end of the mapping faults
    Bytes c = encode(0, 512, 2, 4, true, false, 0, false);
    c[6] = 0xF0; c[7] = 0x17; c[8] = 0; c[9] = 0;      // [rdi + 0x17F0] = 0x21FF0, the last 16 bytes of the mapped 0x20000..0x22000
    Machine m(c);
    CHECK(m.run() == StopReason::Syscall);
    c[6] = 0xF4;
    Machine m2(c);
    CHECK(m2.run() == StopReason::Fault);
  }
  {      // cpuid.(EAX=7,ECX=0):EDX bit 2
    Machine m({0xB8, 7, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2, 0x0F, 0x05});
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.cpu.state().r[2] >> 2) & 1);
  }
}

static void test_robustness() {      // every truncation and a bit flip of each byte of each form; only the sanitizers judge it
  for (int op = 0; op < 2; ++op) {
    const Bytes code = encode(op, 512, 3, 4, true, false, 2, false, true);
    for (size_t n = 0; n < code.size() - 2; ++n) {
      Bytes c(code.begin(), code.begin() + n);
      c.push_back(0x0F); c.push_back(0x05);
      Machine m(c);
      m.run(6);
    }
    for (size_t i = 0; i < code.size() - 2; ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        Bytes c = code;
        c[i] ^= uint8_t(1u << bit);
        Machine m(c);
        m.run(6);
      }
    }
  }
}

int main() {
  test_operations();
  test_invalid();
  test_robustness();
  if (g_failed) { printf("Vnni4Test: %d failed\n", g_failed); return 1; }
  printf("Vnni4Test: all passed\n");
  return 0;
}
