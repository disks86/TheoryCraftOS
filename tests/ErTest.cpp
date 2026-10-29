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

// Host test of the Xeon Phi AVX-512 ER and PF instructions. No CPU the tests run on has them, so results are checked against what the architecture says: vexp2 to a relative error below 2^-23,
// vrcp28 and vrsqrt28 within 2^-28 (here: the correctly rounded value, or one unit in the last place from the double-precision reference), the special values (zero, infinity, NaN, negative
// numbers, overflow, denormal input and output) exactly, the exception flags, masking, the scalar forms, {sae}, and that the gather / scatter prefetches decode and do nothing.
#include <cmath>
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



struct Zmm { uint8_t b[64]; };
static void set_zmm(X86_64Cpu::State& st, int reg, const Zmm& z) {
  uint64_t q[8];
  memcpy(q, z.b, 64);
  st.xmm[reg] = {q[0], q[1]};
  st.ymm_high[reg].lo = q[2]; st.ymm_high[reg].hi = q[3];
  st.ymm_high[reg].upper[0].lo = q[4]; st.ymm_high[reg].upper[0].hi = q[5];
  st.ymm_high[reg].upper[1].lo = q[6]; st.ymm_high[reg].upper[1].hi = q[7];
}
static Zmm get_zmm(const X86_64Cpu::State& st, int reg) {
  const uint64_t q[8] = {st.xmm[reg].lo, st.xmm[reg].hi, st.ymm_high[reg].lo, st.ymm_high[reg].hi, st.ymm_high[reg].upper[0].lo, st.ymm_high[reg].upper[0].hi, st.ymm_high[reg].upper[1].lo, st.ymm_high[reg].upper[1].hi};
  Zmm z;
  memcpy(z.b, q, 64);
  return z;
}

// An EVEX instruction on registers 0..7: vop zmm(reg){k}, zmm(vvvv), zmm(rm) | [rdi + 64]; L'L = 512 unless `vl` says otherwise; `sae` sets EVEX.b with a register operand.
static Bytes encode(uint8_t map, int pp, uint8_t op, bool w, int reg, int vvvv, int rm, bool memory, int aaa = 0, bool z = false, bool sae = false, int vl_field = 2) {
  Bytes b = {0x62, uint8_t(0xF0 | map), uint8_t((w ? 0x80 : 0) | ((~vvvv & 15) << 3) | 4 | pp), uint8_t((z ? 0x80 : 0) | (vl_field << 5) | (sae ? 0x10 : 0) | 0x08 | aaa), op};
  if (memory) { b.push_back(uint8_t(0x80 | (reg << 3) | 7)); b.push_back(64); b.push_back(0); b.push_back(0); b.push_back(0); }
  else b.push_back(uint8_t(0xC0 | (reg << 3) | rm));
  b.push_back(0x0F); b.push_back(0x0B);
  return b;
}

static float as_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double as_double(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t bits_of(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static bool is_denormal(uint32_t u) { return (u & 0x7F800000u) == 0 && (u & 0x7FFFFF); }

static float random_float(int kind) {
  switch (rnd() % 8) {
    case 0: return float(int(rnd() % 400) - 200) / 8;
    case 1: return as_float(uint32_t(rnd()));
    case 2: return as_float(uint32_t(rnd() & 0x807FFFFF));                          // denormals and zeros
    case 3: return float(std::ldexp(1.0 + double(rnd() % 1000) / 1000.0, int(rnd() % 40) - 20));
    case 4: return kind == 0 ? float(int(rnd() % 40) - 20) : float(std::ldexp(1.0 + double(rnd() % 1000) / 1000.0, int(rnd() % 270) - 135));
    default: return float(double(int64_t(rnd() % 20001) - 10000) / 64);
  }
}

static void test_single() {
  for (int op_kind = 0; op_kind < 3; ++op_kind) {       // 0 vexp2ps, 1 vrcp28ps, 2 vrsqrt28ps
    const uint8_t opcode = op_kind == 0 ? 0xC8 : op_kind == 1 ? 0xCA : 0xCC;
    for (int iter = 0; iter < 400; ++iter) {
      const int reg = rnd() % 8, rm = rnd() % 8, aaa = rnd() % 3 == 0 ? 0 : 1 + rnd() % 3;
      const bool z = rnd() & 1, memory = rnd() & 1, sae = !memory && (rnd() & 1);
      Zmm src, old;
      for (int i = 0; i < 16; ++i) { const float f = random_float(op_kind); memcpy(src.b + 4 * i, &f, 4); const float g = random_float(1); memcpy(old.b + 4 * i, &g, 4); }
      Machine m(encode(2, 1, opcode, false, reg, 0, rm, memory, aaa, z, sae));
      X86_64Cpu::State& st = m.cpu.state();
      set_zmm(st, reg, old);
      if (!memory && rm != reg) set_zmm(st, rm, src);
      const Zmm& used = (!memory && rm == reg) ? old : src;
      m.poke(kBase + 64, src.b, 64);
      const uint32_t mxcsr_before = st.mxcsr;
      const uint64_t kmask = rnd();
      if (aaa) st.k[aaa] = kmask;
      CHECK(m.finished());
      const Zmm got = get_zmm(st, reg);
      uint32_t expected_flags = 0;
      for (int i = 0; i < 16; ++i) {
        uint32_t g, o, x;
        memcpy(&g, got.b + 4 * i, 4); memcpy(&o, old.b + 4 * i, 4); memcpy(&x, used.b + 4 * i, 4);
        if (aaa && !((kmask >> i) & 1)) { if (g != (z ? 0 : o)) { printf("masked lane %d got %08x\n", i, g); ++g_failed; } continue; }
        const bool nan = (x & 0x7F800000u) == 0x7F800000u && (x & 0x7FFFFF);
        const double xv = is_denormal(x) ? (x >> 31 ? -0.0 : 0.0) : (double)as_float(x);
        if (nan) { if (!(g & 0x400000) || (g & 0x7F800000u) != 0x7F800000u) { printf("NaN lane %d got %08x for %08x\n", i, g, x); ++g_failed; } if (!(x & 0x400000)) expected_flags |= 1; continue; }
        double want;
        if (op_kind == 0) want = std::exp2(xv);
        else if (op_kind == 1) want = 1.0 / xv;
        else want = xv < 0 ? NAN : 1.0 / std::sqrt(xv);
        const float wf = float(want);
        const bool want_nan = std::isnan(want);
        const bool tiny = !want_nan && std::isfinite(wf) && wf != 0 && std::fabs(wf) < 1.17549435e-38f;
        if (want_nan) { if (g != 0xFFC00000u) { printf("lane %d: want default NaN got %08x (in %08x)\n", i, g, x); ++g_failed; } expected_flags |= 1; continue; }
        if (std::isinf(want) && std::isfinite(xv) && op_kind != 0 && xv == 0) expected_flags |= 4;
        if (tiny) { if ((g & 0x7FFFFFFFu) != 0) { printf("lane %d: tiny result %g should be flushed, got %08x\n", i, want, g); ++g_failed; } continue; }
        if (std::isinf(wf) || wf == 0) { if (g != bits_of(wf) && !(wf == 0 && (g & 0x7FFFFFFFu) == 0)) { printf("lane %d: got %08x want %08x for %08x\n", i, g, bits_of(wf), x); ++g_failed; } continue; }
        const double got_value = (double)as_float(g);
        const double bound = op_kind == 0 ? std::ldexp(1.0, -23) : std::ldexp(1.0, -23);       // (one unit in the last place or better)
        if (!(std::fabs(got_value - want) <= bound * std::fabs(want))) { printf("lane %d op %d: x=%g got %g want %g\n", i, op_kind, xv, got_value, want); ++g_failed; }
      }
      // flags: exception flags raised unless {sae}; the precise inexact / overflow flags are not asserted here, the invalid and divide-by-zero ones are
      const uint32_t new_flags = (st.mxcsr & ~mxcsr_before) & 0x3F;
      if (sae) CHECK((st.mxcsr & 0x3F) == (mxcsr_before & 0x3F));
      else { CHECK((new_flags & 5) == (expected_flags & 5) || (new_flags & expected_flags & 5) == (expected_flags & 5)); }
    }
  }
}

static void test_exact_values() {
  struct Case { int kind; float in; uint32_t out; uint32_t flags; };
  static const Case cases[] = {
    {0, 0.0f, 0x3F800000u, 0}, {0, 1.0f, 0x40000000u, 0}, {0, -1.0f, 0x3F000000u, 0}, {0, 10.0f, 0x44800000u, 0}, {0, -126.0f, 0x00800000u, 0},
    {0, 128.0f, 0x7F800000u, 8 | 32}, {0, INFINITY, 0x7F800000u, 0}, {0, -INFINITY, 0u, 0}, {0, -200.0f, 0u, 16 | 32},
    {1, 4.0f, 0x3E800000u, 0}, {1, 0.0f, 0x7F800000u, 4}, {1, -0.0f, 0xFF800000u, 4}, {1, INFINITY, 0u, 0}, {1, -INFINITY, 0x80000000u, 0},
    {1, as_float(0x00000001u), 0x7F800000u, 4},           // a denormal input is a zero
    {2, 4.0f, 0x3F000000u, 0}, {2, 0.0f, 0x7F800000u, 4}, {2, -0.0f, 0xFF800000u, 4}, {2, -4.0f, 0xFFC00000u, 1}, {2, INFINITY, 0u, 0},
  };
  for (const Case& c : cases) {
    Machine m(encode(2, 1, c.kind == 0 ? 0xC8 : c.kind == 1 ? 0xCA : 0xCC, false, 1, 0, 2, false));
    Zmm src;
    for (int i = 0; i < 16; ++i) memcpy(src.b + 4 * i, &c.in, 4);
    set_zmm(m.cpu.state(), 2, src);
    CHECK(m.finished());
    const Zmm got = get_zmm(m.cpu.state(), 1);
    uint32_t g;
    memcpy(&g, got.b, 4);
    if (g != c.out) { printf("kind %d in %g: got %08x want %08x\n", c.kind, c.in, g, c.out); ++g_failed; }
    if ((m.cpu.state().mxcsr & 0x3F) != c.flags) { printf("kind %d in %g: flags %x want %x\n", c.kind, c.in, m.cpu.state().mxcsr & 0x3F, c.flags); ++g_failed; }
  }
}

static void test_double_and_scalar() {
  {      // vrcp28pd, vrsqrt28pd, vexp2pd
    for (int op_kind = 0; op_kind < 3; ++op_kind) {
      Machine m(encode(2, 1, op_kind == 0 ? 0xC8 : op_kind == 1 ? 0xCA : 0xCC, true, 1, 0, 2, false));
      Zmm src;
      double values[8];
      for (int i = 0; i < 8; ++i) { values[i] = op_kind == 0 ? double(int(rnd() % 100) - 50) / 7 : double(rnd() % 100000 + 1) / 13; memcpy(src.b + 8 * i, &values[i], 8); }
      set_zmm(m.cpu.state(), 2, src);
      CHECK(m.finished());
      const Zmm got = get_zmm(m.cpu.state(), 1);
      for (int i = 0; i < 8; ++i) {
        double g;
        memcpy(&g, got.b + 8 * i, 8);
        const double want = op_kind == 0 ? std::exp2(values[i]) : op_kind == 1 ? 1.0 / values[i] : 1.0 / std::sqrt(values[i]);
        if (!(std::fabs(g - want) <= std::ldexp(1.0, -28) * std::fabs(want))) { printf("pd kind %d lane %d: got %.17g want %.17g\n", op_kind, i, g, want); ++g_failed; }
      }
    }
  }
  {      // vrcp28ss / vrsqrt28ss: element 0 computed, the rest from the first source, upper registers cleared
    for (int op_kind = 1; op_kind < 3; ++op_kind) {
      Machine m(encode(2, 1, op_kind == 1 ? 0xCB : 0xCD, false, 1, 3, 2, false));
      Zmm first, second, old;
      for (int i = 0; i < 64; ++i) { first.b[i] = uint8_t(rnd()); second.b[i] = uint8_t(rnd()); old.b[i] = uint8_t(rnd()); }
      const float x = 9.0f;
      memcpy(second.b, &x, 4);
      set_zmm(m.cpu.state(), 1, old);
      set_zmm(m.cpu.state(), 3, first);
      set_zmm(m.cpu.state(), 2, second);
      CHECK(m.finished());
      const Zmm got = get_zmm(m.cpu.state(), 1);
      float g;
      memcpy(&g, got.b, 4);
      const double want = op_kind == 1 ? 1.0 / 9.0 : 1.0 / 3.0;
      CHECK(std::fabs(g - want) <= std::ldexp(1.0, -23) * want);
      CHECK(memcmp(got.b + 4, first.b + 4, 12) == 0);
      for (int i = 16; i < 64; ++i) CHECK(got.b[i] == 0);
    }
  }
  {      // vrcp28sd with memory
    Machine m(encode(2, 1, 0xCB, true, 1, 3, 0, true));
    Zmm first;
    memset(first.b, 0x5A, 64);
    set_zmm(m.cpu.state(), 3, first);
    const double v = 8.0;
    m.poke(kBase + 64, &v, 8);
    CHECK(m.finished());
    const Zmm got = get_zmm(m.cpu.state(), 1);
    double g;
    memcpy(&g, got.b, 8);
    CHECK(g == 0.125);
    CHECK(got.b[8] == 0x5A);
  }
  {      // 128/256-bit packed forms are #UD
    Machine m(encode(2, 1, 0xCA, false, 1, 0, 2, false, 0, false, false, 1));
    CHECK(m.run(8) == StopReason::Fault && m.cpu.fault().vector == 6);
  }
}

static void test_prefetch() {
  // vgatherpf0dps [rdi + zmm1*4 + 0] {k1}: EVEX.512.66.0F38.W0 C6 /1 with a SIB byte; vscatterpf1qpd C7 /6 W1; /0, /3 and a register operand are #UD
  for (int w = 0; w < 2; ++w) {
    for (uint8_t opcode : {uint8_t(0xC6), uint8_t(0xC7)}) {
      for (int kind : {1, 2, 5, 6, 0, 3, 4, 7}) {
        Bytes b = {0x62, 0xF2, uint8_t((w ? 0x80 : 0) | 0x78 | 4 | 1), 0x49, opcode, uint8_t(0x04 | (kind << 3)), 0x8F, 0x0F, 0x0B};      // modrm: mod 00, rm 100 (SIB); SIB: scale 4, index zmm1, base rdi
        Machine m(b);
        m.cpu.state().r[7] = 0xFFFFFFFFFFFF0000ull;      // (an address that would fault: prefetches do not)
        const bool valid = kind == 1 || kind == 2 || kind == 5 || kind == 6;
        if (valid) CHECK(m.finished()); else CHECK(m.run(8) == StopReason::Fault && m.cpu.fault().vector == 6);
      }
    }
  }
  Bytes reg_form = {0x62, 0xF2, 0x7D, 0x49, 0xC6, 0xC9, 0x0F, 0x0B};
  Machine m(reg_form);
  CHECK(m.run(8) == StopReason::Fault && m.cpu.fault().vector == 6);
}

static void test_cpuid() {
  Machine m({0xB8, 7, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2, 0x0F, 0x0B});
  CHECK(m.finished());
  CHECK(((m.cpu.state().r[3] >> 26) & 1) && ((m.cpu.state().r[3] >> 27) & 1) && ((m.cpu.state().r[3] >> 28) & 1));      // AVX512PF, AVX512ER
}

int main() {
  test_single();
  test_exact_values();
  test_double_and_scalar();
  test_prefetch();
  test_cpuid();
  if (g_failed) { printf("ErTest: %d failed\n", g_failed); return 1; }
  printf("ErTest: all passed\n");
  return 0;
}
