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

// Host test of the VEX-encoded additions: AVX-VNNI (vpdpbusd[s], vpdpwssd[s]), AVX-IFMA (vpmadd52luq / huq), AVX-VNNI-INT8 (vpdpbssd, vpdpbsud, vpdpbuud and the saturating forms), AVX-VNNI-INT16
// (vpdpwsud, vpdpwusd, vpdpwuud and the saturating forms), AVX-NE-CONVERT (vbcstnebf162ps, vbcstnesh2ps, vcvtne[eo]bf162ps, vcvtne[eo]ph2ps) and the VEX vcvtneps2bf16. No CPU the tests run on has
// the INT8, INT16 and NE-CONVERT ones, so every result is checked against a model written from the definition; the VNNI and IFMA ones share their arithmetic with the EVEX forms, which are
// compared with the hardware by Avx512Test. Both vector lengths, register and memory operands, and the encodings that must be #UD.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"

using namespace emulation;
typedef std::vector<uint8_t> Bytes;
typedef unsigned __int128 u128;

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
    end = 0x10000 + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x30000;
    cpu.reset(start);
    cpu.state().r[7] = kBase;
  }
  uint64_t end = 0;
  StopReason run(uint64_t n = 4) { GuestMemory m(space); return cpu.run(m, n); }
  bool finished(uint64_t n = 8) { return run(n) == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
};


struct Ymm { uint8_t b[32]; };
static void set_ymm(X86_64Cpu::State& st, int reg, const Ymm& y) {
  uint64_t q[4];
  memcpy(q, y.b, 32);
  st.xmm[reg] = {q[0], q[1]};
  st.ymm_high[reg].lo = q[2];
  st.ymm_high[reg].hi = q[3];
}
static Ymm get_ymm(const X86_64Cpu::State& st, int reg) {
  const uint64_t q[4] = {st.xmm[reg].lo, st.xmm[reg].hi, st.ymm_high[reg].lo, st.ymm_high[reg].hi};
  Ymm y;
  memcpy(y.b, q, 32);
  return y;
}

// vop ymm(reg), ymm(vvvv), ymm(rm) | [rdi + 64]; registers 0..7.
static Bytes encode(int pp, uint8_t op, bool w, bool l, int reg, int vvvv, int rm, bool memory) {
  Bytes b = {0xC4, 0xE2, uint8_t((w ? 0x80 : 0) | ((~vvvv & 15) << 3) | (l ? 4 : 0) | pp), op};
  if (memory) { b.push_back(uint8_t(0x80 | (reg << 3) | 7)); b.push_back(64); b.push_back(0); b.push_back(0); b.push_back(0); }
  else b.push_back(uint8_t(0xC0 | (reg << 3) | rm));
  b.push_back(0x0F); b.push_back(0x0B);      // (a ud2 ends the program)
  return b;
}

static uint8_t byte_value() {
  switch (rnd() % 6) {
    case 0: return 0x80;
    case 1: return 0x7F;
    case 2: return 0xFF;
    case 3: return 0;
    default: return uint8_t(rnd());
  }
}
static void fill(Ymm* y) { for (auto& v : y->b) v = byte_value(); }

template <typename T> static T get(const Ymm& y, int i) { T v; memcpy(&v, y.b + i * sizeof(T), sizeof(T)); return v; }
template <typename T> static void put(Ymm* y, int i, T v) { memcpy(y->b + i * sizeof(T), &v, sizeof(T)); }

struct DotOp { const char* name; int pp; uint8_t op; bool sixteen, a_signed, b_signed, saturate; };

static void model_dot(const DotOp& d, const Ymm& acc, const Ymm& a, const Ymm& b, int lanes, Ymm* out) {
  *out = acc;
  for (int i = 0; i < lanes; ++i) {
    int64_t sum = 0;
    const int n = d.sixteen ? 2 : 4;
    for (int k = 0; k < n; ++k) {
      int64_t x, y;
      if (d.sixteen) {
        x = d.a_signed ? int64_t(get<int16_t>(a, n * i + k)) : int64_t(get<uint16_t>(a, n * i + k));
        y = d.b_signed ? int64_t(get<int16_t>(b, n * i + k)) : int64_t(get<uint16_t>(b, n * i + k));
      } else {
        x = d.a_signed ? int64_t(get<int8_t>(a, n * i + k)) : int64_t(get<uint8_t>(a, n * i + k));
        y = d.b_signed ? int64_t(get<int8_t>(b, n * i + k)) : int64_t(get<uint8_t>(b, n * i + k));
      }
      sum += x * y;
    }
    const bool unsigned_acc = !d.a_signed && !d.b_signed;
    int64_t r = (unsigned_acc ? int64_t(get<uint32_t>(acc, i)) : int64_t(get<int32_t>(acc, i))) + sum;
    if (d.saturate) {
      const int64_t lo = unsigned_acc ? 0 : INT32_MIN, hi = unsigned_acc ? int64_t(UINT32_MAX) : INT32_MAX;
      r = r > hi ? hi : r < lo ? lo : r;
    }
    put<uint32_t>(out, i, uint32_t(r));
  }
}

static void test_dot() {
  static const DotOp ops[] = {
    {"vpdpbusd", 1, 0x50, false, false, true, false}, {"vpdpbusds", 1, 0x51, false, false, true, true},
    {"vpdpwssd", 1, 0x52, true, true, true, false},   {"vpdpwssds", 1, 0x53, true, true, true, true},
    {"vpdpbssd", 3, 0x50, false, true, true, false},  {"vpdpbssds", 3, 0x51, false, true, true, true},
    {"vpdpbsud", 2, 0x50, false, true, false, false}, {"vpdpbsuds", 2, 0x51, false, true, false, true},
    {"vpdpbuud", 0, 0x50, false, false, false, false}, {"vpdpbuuds", 0, 0x51, false, false, false, true},
    {"vpdpwsud", 2, 0xD2, true, true, false, false},  {"vpdpwsuds", 2, 0xD3, true, true, false, true},
    {"vpdpwusd", 1, 0xD2, true, false, true, false},  {"vpdpwusds", 1, 0xD3, true, false, true, true},
    {"vpdpwuud", 0, 0xD2, true, false, false, false}, {"vpdpwuuds", 0, 0xD3, true, false, false, true},
  };
  int cases = 0;
  for (const DotOp& d : ops) {
    for (int iter = 0; iter < 300; ++iter) {
      const bool l = rnd() & 1, memory = rnd() & 1;
      const int reg = rnd() % 8, vvvv = rnd() % 8, rm = rnd() % 8;
      Ymm regs[8], mem;
      for (auto& r : regs) fill(&r);
      fill(&mem);
      if (rnd() % 3 == 0) for (int i = 0; i < 8; ++i) put<uint32_t>(&regs[reg], i, (rnd() & 1) ? 0x7FFFFFF0u + uint32_t(rnd() % 32) : 0x80000000u + uint32_t(rnd() % 32));
      Machine m(encode(d.pp, d.op, false, l, reg, vvvv, rm, memory));
      X86_64Cpu::State& st = m.cpu.state();
      for (int r = 0; r < 8; ++r) set_ymm(st, r, regs[r]);
      m.poke(kBase + 64, mem.b, 32);
      if (!m.finished()) { printf("%s did not run (vector %d)\n", d.name, m.cpu.fault().vector); ++g_failed; continue; }
      const Ymm& second = memory ? mem : regs[rm];
      Ymm expect;
      const int lanes = l ? 8 : 4;
      model_dot(d, regs[reg], regs[vvvv], second, lanes, &expect);
      if (!l) memset(expect.b + 16, 0, 16);      // (VEX.128 clears the upper half)
      const Ymm got = get_ymm(st, reg);
      if (memcmp(got.b, expect.b, 32) != 0) {
        printf("MISMATCH %s l=%d memory=%d reg=%d vvvv=%d rm=%d\n", d.name, l, memory, reg, vvvv, rm);
        for (int i = 0; i < lanes; ++i) if (get<uint32_t>(got, i) != get<uint32_t>(expect, i)) printf("  lane %d got %08x expect %08x\n", i, get<uint32_t>(got, i), get<uint32_t>(expect, i));
        ++g_failed;
      }
      ++cases;
    }
    // W1 is #UD
    Machine bad(encode(d.pp, d.op, true, true, 1, 2, 3, false));
    CHECK(bad.run(8) == StopReason::Fault && bad.cpu.fault().vector == 6);
  }
  CHECK(cases == 16 * 300);
}

static void test_ifma() {
  for (int hi = 0; hi < 2; ++hi) {
    for (int iter = 0; iter < 300; ++iter) {
      const bool l = rnd() & 1;
      const int reg = rnd() % 8, vvvv = rnd() % 8, rm = rnd() % 8;
      Ymm regs[8];
      for (auto& r : regs) for (int i = 0; i < 4; ++i) put<uint64_t>(&r, i, (rnd() & 1) ? rnd() : (uint64_t{1} << 52) - 1 - (rnd() & 7));
      Machine m(encode(1, hi ? 0xB5 : 0xB4, true, l, reg, vvvv, rm, false));
      X86_64Cpu::State& st = m.cpu.state();
      for (int r = 0; r < 8; ++r) set_ymm(st, r, regs[r]);
      if (!m.finished()) { printf("vpmadd52 did not run\n"); ++g_failed; continue; }
      Ymm expect = regs[reg];
      const Ymm& b = regs[rm];
      const Ymm& a = regs[vvvv];
      for (int i = 0; i < (l ? 4 : 2); ++i) {
        const uint64_t mask = (uint64_t{1} << 52) - 1;
        const u128 product = u128(get<uint64_t>(a, i) & mask) * (get<uint64_t>(b, i) & mask);
        const uint64_t part = hi ? uint64_t(product >> 52) & mask : uint64_t(product) & mask;
        put<uint64_t>(&expect, i, get<uint64_t>(regs[reg], i) + part);
      }
      if (!l) memset(expect.b + 16, 0, 16);
      const Ymm got = get_ymm(st, reg);
      CHECK(memcmp(got.b, expect.b, 32) == 0);
    }
  }
}

static uint16_t to_bf16(uint32_t x) {
  if ((x & 0x7F800000u) == 0x7F800000u && (x & 0x7FFFFF)) return uint16_t((x >> 16) | 0x40);
  if ((x & 0x7F800000u) == 0) return uint16_t((x >> 16) & 0x8000);
  return uint16_t((x + 0x7FFF + ((x >> 16) & 1)) >> 16);
}
static uint32_t half_to_float_model(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 31, fraction = h & 0x3FF;
  if (exponent == 31) return sign | 0x7F800000u | (fraction ? (fraction << 13) | 0x400000u : 0);      // infinity, or a NaN made quiet
  if (exponent == 0) {
    if (fraction == 0) return sign;
    int shift = 0;
    while (!(fraction & 0x400)) { fraction <<= 1; ++shift; }
    fraction &= 0x3FF;
    return sign | uint32_t(127 - 15 - shift + 1) << 23 | (fraction << 13);
  }
  return sign | ((exponent + 112) << 23) | (fraction << 13);
}

static void test_ne_convert() {
  for (int iter = 0; iter < 2000; ++iter) {
    const int kind = rnd() % 7;      // 0 bcst bf16, 1 bcst half, 2 even bf16, 3 odd bf16, 4 even half, 5 odd half, 6 cvtneps2bf16
    const bool l = rnd() & 1;
    const int reg = rnd() % 8;
    Ymm mem, regs[8];
    fill(&mem);
    for (auto& r : regs) fill(&r);
    if (kind == 6 && (rnd() & 1)) for (int i = 0; i < 8; ++i) put<uint32_t>(&mem, i, (rnd() % 5 == 0) ? 0x7FA00000u + uint32_t(rnd() % 100) : (rnd() % 7 == 0) ? uint32_t(rnd() & 0x807FFFFFu) : uint32_t(rnd()));
    static const struct { int pp; uint8_t op; } enc[] = {{2, 0xB1}, {1, 0xB1}, {2, 0xB0}, {3, 0xB0}, {1, 0xB0}, {0, 0xB0}, {2, 0x72}};
    Machine m(encode(enc[kind].pp, enc[kind].op, false, l, reg, 0, 0, true));
    X86_64Cpu::State& st = m.cpu.state();
    for (int r = 0; r < 8; ++r) set_ymm(st, r, regs[r]);
    m.poke(kBase + 64, mem.b, 32);
    if (!m.finished()) { printf("ne-convert kind %d did not run (vector %d)\n", kind, m.cpu.fault().vector); ++g_failed; continue; }
    Ymm expect;
    memset(expect.b, 0, 32);
    const int count = l ? 8 : 4;
    if (kind == 6) {
      for (int i = 0; i < count; ++i) put<uint16_t>(&expect, i, to_bf16(get<uint32_t>(mem, i)));
    } else {
      for (int i = 0; i < count; ++i) {
        const uint16_t element = kind <= 1 ? get<uint16_t>(mem, 0) : get<uint16_t>(mem, 2 * i + ((kind == 3 || kind == 5) ? 1 : 0));
        put<uint32_t>(&expect, i, (kind == 0 || kind == 2 || kind == 3) ? uint32_t(element) << 16 : half_to_float_model(element));
      }
    }
    const Ymm got = get_ymm(st, reg);
    if (memcmp(got.b, expect.b, 32) != 0) {
      printf("MISMATCH ne-convert kind %d l=%d\n", kind, l);
      for (int i = 0; i < 8; ++i) if (get<uint32_t>(got, i) != get<uint32_t>(expect, i)) printf("  dword %d got %08x expect %08x\n", i, get<uint32_t>(got, i), get<uint32_t>(expect, i));
      ++g_failed;
    }
  }
  // a register operand or W1 is #UD for the NE-CONVERT loads
  Machine reg_form(encode(2, 0xB0, false, true, 1, 0, 2, false));
  CHECK(reg_form.run(8) == StopReason::Fault && reg_form.cpu.fault().vector == 6);
  Machine w1(encode(2, 0xB1, true, true, 1, 0, 0, true));
  CHECK(w1.run(8) == StopReason::Fault && w1.cpu.fault().vector == 6);
}

static void test_cpuid() {
  Machine m({0xB8, 7, 0, 0, 0, 0xB9, 1, 0, 0, 0, 0x0F, 0xA2, 0x0F, 0x0B});
  CHECK(m.finished());
  const auto& r = m.cpu.state().r;
  CHECK(((r[0] >> 4) & 1) && ((r[0] >> 23) & 1));      // AVX-VNNI, AVX-IFMA
  CHECK(((r[2] >> 4) & 1) && ((r[2] >> 5) & 1) && ((r[2] >> 10) & 1));      // AVX-VNNI-INT8, AVX-NE-CONVERT, AVX-VNNI-INT16
}

int main() {
  test_dot();
  test_ifma();
  test_ne_convert();
  test_cpuid();
  if (g_failed) { printf("VexNewTest: %d failed\n", g_failed); return 1; }
  printf("VexNewTest: all passed\n");
  return 0;
}
