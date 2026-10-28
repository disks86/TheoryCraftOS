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

// Host test of the Xeon Phi AVX512_4FMAPS instructions (v4fmaddps, v4fnmaddps, v4fmaddss, v4fnmaddss). The host CPU has no 4FMAPS, but it has FMA: each result lane is the chain
// of four fused multiply-adds acc = fma(src[base + j][lane], mem[j], acc) done with the host's vfmadd (negated product for the fnmadd forms), under the same rounding mode, so the
// values, NaN propagation, the MXCSR flags and the mask behaviour (merge / zero) all come from the hardware. Also covers the register group alignment (vvvv & ~3), the dest
// overlapping the group, compressed disp8, the scalar forms keeping bits 127:32, and the encodings that must be #UD (register operand, broadcast, 128/256-bit packed form).
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

static uint64_t seed = 0x9E3779B97F4A7C15ull;
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

// op: 0 v4fmaddps, 1 v4fnmaddps, 2 v4fmaddss, 3 v4fnmaddss. reg 0..7, vvvv 0..7; memory [rdi + 64 + disp] with disp8 (compressed, N = 16) when `disp8`.
static Bytes encode(int op, int vl, int reg, int vvvv, bool memory, bool bcst, int aaa, bool z, bool disp8 = false) {
  const uint8_t opcode = (op & 2 ? 0x9B : 0x9A) + (op & 1 ? 0x10 : 0);
  Bytes b = {0x62, 0xF2, uint8_t(((~vvvv & 15) << 3) | 4 | 3), 0};
  b[3] = uint8_t((z ? 0x80 : 0) | ((vl == 512 ? 2 : vl == 256 ? 1 : 0) << 5) | (bcst ? 0x10 : 0) | 0x08 | aaa);
  b.push_back(opcode);
  if (memory && disp8) { b.push_back(uint8_t(0x40 | (reg << 3) | 7)); b.push_back(4); }      // [rdi + 4 * 16]
  else if (memory) { b.push_back(uint8_t(0x80 | (reg << 3) | 7)); for (int i = 0; i < 4; ++i) b.push_back(uint8_t(i == 0 ? 64 : 0)); }
  else b.push_back(uint8_t(0xC0 | (reg << 3) | 3));
  b.push_back(0x0F); b.push_back(0x05);
  return b;
}

static uint32_t rnd_float() {
  const uint64_t r = rnd();
  const uint32_t any = uint32_t(rnd());
  switch (r % 16) {
    case 0: case 1: case 2: case 3: case 4: return any;
    case 5: case 6: case 7: return (any & 0x80000000u) | ((120 + rnd() % 14) << 23) | (rnd() & 0x7FFFFF);
    case 8: {
      static const uint32_t sp[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FA00000u, 0xFFC00001u, 1, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x3F800000u, 0xBF800000u};
      return sp[rnd() % (sizeof(sp) / sizeof(sp[0]))];
    }
    case 9: case 10: return (any & 0x80000000u) | (rnd() & 0x7FFFFF);
    case 11: case 12: { const float f = float(int(rnd() % 41) - 20); uint32_t u; memcpy(&u, &f, 4); return u; }
    default: return (any & 0x80000000u) | ((127 + rnd() % 5 - 2) << 23) | (rnd() & 0x7FFFFF);
  }
}

__attribute__((target("fma")))
static uint32_t host_chain(uint32_t acc, const uint32_t p[4], const uint32_t w[4], bool negate) {
  __m128 a = _mm_castsi128_ps(_mm_cvtsi32_si128(int(acc)));
  for (int j = 0; j < 4; ++j) {
    const __m128 x = _mm_castsi128_ps(_mm_cvtsi32_si128(int(p[j]))), y = _mm_castsi128_ps(_mm_cvtsi32_si128(int(w[j])));
    a = negate ? _mm_fnmadd_ss(x, y, a) : _mm_fmadd_ss(x, y, a);
  }
  return uint32_t(_mm_cvtsi128_si32(_mm_castps_si128(a)));
}

static void test_operations() {
  int cases = 0;
  for (int iter = 0; iter < 4000; ++iter) {
    const int op = rnd() % 4;
    const bool scalar = op & 2, negate = op & 1;
    const int reg = rnd() % 8, vvvv = rnd() % 8;
    const int aaa = rnd() % 3 == 0 ? 0 : 1 + rnd() % 7;
    const bool z = rnd() & 1, disp8 = rnd() & 1;
    const uint32_t rc = rnd() % 4;
    const uint32_t mxcsr = 0x1F80 | (rc << 13);
    Zmm regs[8], mem_block[4];
    for (auto& r : regs) for (auto& d : r.d) d = rnd_float();
    for (auto& r : mem_block) for (auto& d : r.d) d = rnd_float();
    const uint64_t kreg = rnd();
    const Bytes code = encode(op, 512, reg, vvvv, true, false, aaa, z, disp8);
    Machine m(code);
    X86_64Cpu::State& st = m.cpu.state();
    for (int r = 0; r < 8; ++r) set_reg(st, r, regs[r]);
    m.poke(kBase + 64, mem_block, 64);
    st.k[aaa] = kreg;
    st.mxcsr = mxcsr;
    CHECK(m.run() == StopReason::Syscall);

    // the model, on the host FPU
    unsigned saved = _mm_getcsr();
    _mm_setcsr(mxcsr);
    Zmm expect = regs[reg];
    const int base = vvvv & ~3;
    const int lanes = scalar ? 1 : 16;
    for (int i = 0; i < lanes; ++i) {
      const bool on = aaa == 0 || ((kreg >> i) & 1);
      if (!on) { if (z) expect.d[i] = 0; continue; }
      const uint32_t p[4] = {regs[base].d[i], regs[base + 1].d[i], regs[base + 2].d[i], regs[base + 3].d[i]};
      expect.d[i] = host_chain(regs[reg].d[i], p, mem_block[0].d, negate);
    }
    if (scalar) for (int i = 1; i < 16; ++i) if (i >= 4) expect.d[i] = 0;
    const uint32_t flags = _mm_getcsr() & 0x3F;
    _mm_setcsr(saved);

    const Zmm got = get_reg(st, reg);
    if (memcmp(&got, &expect, 64) != 0) {
      printf("MISMATCH op %d reg %d vvvv %d aaa %d z %d rc %u\n", op, reg, vvvv, aaa, z, rc);
      for (int i = 0; i < 16; ++i) if (got.d[i] != expect.d[i]) printf("  lane %d got %08x expect %08x\n", i, got.d[i], expect.d[i]);
      ++g_failed;
    }
    // Underflow is not compared: x86 (Intel, the only maker of 4FMAPS, and the emulator) detects tininess before rounding, but an AMD host detects it after rounding, so it reports no
    // underflow for a sum just below the smallest normal number that rounds up to it.
    if (((st.mxcsr ^ flags) & 0x2F) != 0) { printf("FLAGS op %d got %x expect %x rc %u aaa %d\n", op, st.mxcsr & 0x3F, flags, rc, aaa); ++g_failed; }
    for (int r = 0; r < 8; ++r) {
      if (r == reg) continue;
      const Zmm o = get_reg(st, r);
      CHECK(memcmp(&o, &regs[r], 64) == 0);
    }
    ++cases;
  }
  CHECK(cases == 4000);
}

static void test_invalid() {
  {      // a register second operand, broadcast, and the 128/256-bit packed forms are #UD
    Machine m(encode(0, 512, 2, 4, false, false, 0, false));
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 6);
    Machine m2(encode(0, 512, 2, 4, true, true, 0, false));
    CHECK(m2.run() == StopReason::Fault && m2.cpu.fault().vector == 6);
    Machine m3(encode(0, 256, 2, 4, true, false, 0, false));
    CHECK(m3.run() == StopReason::Fault && m3.cpu.fault().vector == 6);
    Machine m4(encode(0, 128, 2, 4, true, false, 0, false));
    CHECK(m4.run() == StopReason::Fault && m4.cpu.fault().vector == 6);
  }
  {      // the scalar form ignores the vector length
    Machine m(encode(2, 128, 2, 4, true, false, 0, false));
    CHECK(m.run() == StopReason::Syscall);
  }
  {      // an m128 operand that runs off the end of the mapping faults (page fault, #PF)
    Bytes c = encode(0, 512, 2, 4, true, false, 0, false);
    c[6] = 0xF0; c[7] = 0x17; c[8] = 0; c[9] = 0;      // disp32 = 0x17F0: [rdi + 0x17F0] = 0x21FF0, the last 16 bytes of the mapped 0x20000..0x22000
    Machine m(c);
    CHECK(m.run() == StopReason::Syscall);
    Bytes d = c;
    d[6] = 0xF4; d[7] = 0x17;      // 0x21FF4: the last float is off the end
    Machine m2(d);
    CHECK(m2.run() == StopReason::Fault);
  }
  {      // cpuid.(EAX=7,ECX=0):EDX bit 3
    Machine m({0xB8, 7, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2, 0x0F, 0x05});
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.cpu.state().r[2] >> 3) & 1);
  }
}

static void test_robustness() {      // every truncation and a bit flip of each byte of each form; only the sanitizers judge it
  for (int op = 0; op < 4; ++op) {
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
  if (g_failed) { printf("Fma4Test: %d failed\n", g_failed); return 1; }
  printf("Fma4Test: all passed\n");
  return 0;
}
