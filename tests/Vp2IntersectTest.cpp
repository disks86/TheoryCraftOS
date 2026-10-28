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

// Host test of AVX512_VP2INTERSECT (vp2intersectd / vp2intersectq). The host CPU has no such instruction, so the result is compared with a model written from the definition: for every
// pair of elements (i of the first source, j of the second) that are equal, bit i of the even mask register and bit j of the odd one is set, and all other bits of both are cleared.
// Runs register, memory and broadcast sources, 128/256/512 bits, every destination pair, and the encodings that must raise #UD (a write mask, zeroing, embedded rounding, truncation).
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

static void set_reg(X86_64Cpu::State& st, int reg, const uint64_t q[8]) {
  X86_64Cpu::Xmm& lo = reg < 16 ? st.xmm[reg] : st.evex_lo[reg - 16];
  X86_64Cpu::YmmHigh& hi = reg < 16 ? st.ymm_high[reg] : st.evex_hi[reg - 16];
  lo.lo = q[0]; lo.hi = q[1]; hi.lo = q[2]; hi.hi = q[3]; hi.upper[0].lo = q[4]; hi.upper[0].hi = q[5]; hi.upper[1].lo = q[6]; hi.upper[1].hi = q[7];
}

// vp2intersect k(reg), zmm(vvvv), zmm(rm) or [rdi + 64] (disp32). Registers 0..7 only.
static Bytes encode(bool q, int vl, int reg, int vvvv, int rm, bool memory, bool bcst, int aaa = 0, bool z = false) {
  Bytes b = {0x62, 0xF2, uint8_t((q ? 0x80 : 0) | ((~vvvv & 15) << 3) | 4 | 3), 0};
  b[3] = uint8_t((z ? 0x80 : 0) | ((vl == 512 ? 2 : vl == 256 ? 1 : 0) << 5) | (bcst ? 0x10 : 0) | 0x08 | aaa);
  b.push_back(0x68);
  if (memory) { b.push_back(uint8_t(0x80 | (reg << 3) | 7)); for (int i = 0; i < 4; ++i) b.push_back(uint8_t(i == 0 ? 64 : 0)); }
  else b.push_back(uint8_t(0xC0 | (reg << 3) | rm));
  b.push_back(0x0F); b.push_back(0x05);
  return b;
}

static void model(bool q, int vl, const uint64_t a[8], const uint64_t b[8], uint64_t* k1, uint64_t* k2) {
  const int n = vl / (q ? 64 : 32);
  auto el = [&](const uint64_t* v, int i) { return q ? v[i] : (v[i / 2] >> (32 * (i & 1))) & 0xFFFFFFFFu; };
  *k1 = *k2 = 0;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      if (el(a, i) == el(b, j)) { *k1 |= uint64_t{1} << i; *k2 |= uint64_t{1} << j; }
}

static void fill(uint64_t v[8], bool q) {      // few distinct values so that equal elements are common
  for (int i = 0; i < 8; ++i) {
    uint64_t x = 0;
    for (int h = 0; h < (q ? 1 : 2); ++h) {
      const uint64_t r = rnd();
      uint64_t e = r % 5 == 0 ? rnd() : (r >> 8) % 6;
      if (q && r % 3 == 0) e |= uint64_t{1} << 40;      // equal low halves, different high halves
      x |= (q ? e : (e & 0xFFFFFFFFu)) << (32 * h);
    }
    v[i] = x;
  }
}

static void test_operations() {
  int cases = 0;
  for (int iter = 0; iter < 3000; ++iter) {
    const bool q = rnd() & 1;
    const int vl = (const int[]){128, 256, 512}[rnd() % 3];
    const int form = rnd() % 3;      // 0 register, 1 memory, 2 broadcast
    const int reg = rnd() % 8, vvvv = rnd() % 8, rm = rnd() % 8;
    uint64_t a[8], b[8], mem[8];
    fill(a, q); fill(b, q); fill(mem, q);
    const Bytes code = encode(q, vl, reg, vvvv, rm, form != 0, form == 2);
    Machine m(code);
    X86_64Cpu::State& st = m.cpu.state();
    for (int r = 0; r < 8; ++r) { uint64_t t[8]; fill(t, q); set_reg(st, r, t); }
    set_reg(st, vvvv, a);
    if (form == 0 && rm != vvvv) set_reg(st, rm, b);
    const uint64_t* second = form == 0 ? (rm == vvvv ? a : b) : mem;
    uint64_t bc[8];
    if (form == 2) { for (int i = 0; i < 8; ++i) bc[i] = q ? mem[0] : (mem[0] & 0xFFFFFFFFu) * 0x100000001ull; second = bc; }
    m.poke(kBase + 64, mem, 64);
    for (int k = 0; k < 8; ++k) st.k[k] = ~uint64_t{0};
    CHECK(m.run() == StopReason::Syscall);
    uint64_t e1, e2;
    model(q, vl, a, second, &e1, &e2);
    CHECK(st.k[reg & 6] == e1);
    CHECK(st.k[(reg & 6) | 1] == e2);
    for (int k = 0; k < 8; ++k) if (k != (reg & 6) && k != ((reg & 6) | 1)) CHECK(st.k[k] == ~uint64_t{0});
    ++cases;
  }
  CHECK(cases == 3000);
}

static void test_invalid() {
  {      // a write mask, zeroing: #UD
    Machine m(encode(false, 512, 2, 1, 3, false, false, 1));
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 6);
    Machine m2(encode(true, 512, 2, 1, 3, false, false, 0, true));
    CHECK(m2.run() == StopReason::Fault && m2.cpu.fault().vector == 6);
  }
  {      // embedded rounding (EVEX.b with a register operand): #UD
    Machine m(encode(false, 512, 2, 1, 3, false, true));
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 6);
  }
  {      // cpuid.(EAX=7,ECX=0):EDX bit 8
    Machine m({0xB8, 7, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2, 0x0F, 0x05});
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.cpu.state().r[2] >> 8) & 1);
  }
}

static void test_robustness() {      // every truncation and a bit flip of each byte of each form; only the sanitizers judge it
  for (int form = 0; form < 3; ++form) {
    for (int q = 0; q < 2; ++q) {
      const Bytes code = encode(q, 512, 3, 4, 5, form != 0, form == 2);
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
}

int main() {
  test_operations();
  test_invalid();
  test_robustness();
  if (g_failed) { printf("Vp2IntersectTest: %d failed\n", g_failed); return 1; }
  printf("Vp2IntersectTest: all passed\n");
  return 0;
}
