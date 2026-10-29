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

// Host test of the AVX10.2 bfloat16 arithmetic (VADDBF16 ... VCOMISBF16). The reference is the host's double arithmetic rounded to bfloat16 (double rounding is harmless: 53 >= 2 * 8 + 2) with the
// instructions' DAZ / FTZ / round-to-nearest-even rules, plus exact checks of the special values, the NaN rules of min / max, classification, comparison, masking, broadcast and encoding errors.
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

constexpr uint64_t kBase = 0x20800;
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  uint64_t end = 0;
  explicit Machine(const Bytes& code) {
    space.map(0x10000, 0x2000, process::kProtRead | process::kProtWrite | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    end = 0x10000 + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x30000;
    cpu.reset(start);
    cpu.state().r[7] = kBase;
  }
  StopReason run(uint64_t n = 8) { GuestMemory m(space); return cpu.run(m, n); }
  bool finished() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  bool undefined() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc != end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
};

struct Zmm { uint16_t h[32]; };
static void set_zmm(X86_64Cpu::State& st, int reg, const Zmm& z) {
  uint64_t q[8];
  memcpy(q, z.h, 64);
  st.xmm[reg] = {q[0], q[1]};
  st.ymm_high[reg].lo = q[2]; st.ymm_high[reg].hi = q[3];
  st.ymm_high[reg].upper[0].lo = q[4]; st.ymm_high[reg].upper[0].hi = q[5];
  st.ymm_high[reg].upper[1].lo = q[6]; st.ymm_high[reg].upper[1].hi = q[7];
}
static Zmm get_zmm(const X86_64Cpu::State& st, int reg) {
  const uint64_t q[8] = {st.xmm[reg].lo, st.xmm[reg].hi, st.ymm_high[reg].lo, st.ymm_high[reg].hi, st.ymm_high[reg].upper[0].lo, st.ymm_high[reg].upper[0].hi, st.ymm_high[reg].upper[1].lo, st.ymm_high[reg].upper[1].hi};
  Zmm z;
  memcpy(z.h, q, 64);
  return z;
}

// EVEX map / pp / opcode with zmm0 (reg), zmm1 (vvvv), zmm2 or [rdi + 64] (rm); `bcst` sets EVEX.b with memory; `aaa` is the writemask, k1..k7.
static Bytes encode(int map, int pp, uint8_t op, int reg, int vvvv, bool memory, int vl_field = 2, int aaa = 0, bool z = false, bool bcst = false, bool w = false, std::initializer_list<uint8_t> imm = {}) {
  Bytes b = {0x62, uint8_t(0xF0 | map), uint8_t((w ? 0x80 : 0) | ((~vvvv & 15) << 3) | 4 | pp), uint8_t((z ? 0x80 : 0) | (vl_field << 5) | (bcst ? 0x10 : 0) | 0x08 | aaa), op};
  if (memory) { b.push_back(uint8_t((1 << 6) | (reg << 3) | 7)); b.push_back(64 / (bcst ? 2 : (16 << vl_field))); }
  else b.push_back(uint8_t(0xC0 | (reg << 3) | 2));
  b.insert(b.end(), imm);
  b.push_back(0x0F); b.push_back(0x0B);
  return b;
}

// --- bfloat16 helpers ------------------------------------------------------------------------------------------------------------------

static float bf_to_float(uint16_t h) { const uint32_t u = uint32_t(h) << 16; float f; memcpy(&f, &u, 4); return f; }
static bool bf_nan(uint16_t h) { return (h & 0x7F80) == 0x7F80 && (h & 0x7F); }
static double daz(uint16_t h) { return (h & 0x7F80) == 0 ? ((h & 0x8000) ? -0.0 : 0.0) : double(bf_to_float(h)); }

// double -> bfloat16, round to nearest even, results below the smallest normal flushed to zero
static uint16_t to_bf(double d) {
  if (std::isnan(d)) return 0xFFC0;
  const uint16_t sign = std::signbit(d) ? 0x8000 : 0;
  const double a = std::fabs(d);
  if (a == 0) return sign;
  if (std::isinf(a)) return sign | 0x7F80;
  if (a < std::ldexp(1.0, -126)) return sign;
  int e;
  const double frac = std::frexp(a, &e);      // [0.5, 1)
  double scaled = std::ldexp(frac, 8);        // [128, 256)
  double rounded = std::nearbyint(scaled);    // RNE under the default mode
  int exp = e - 1;
  if (rounded >= 256) { rounded = 128; ++exp; }
  if (exp > 127) return sign | 0x7F80;
  return uint16_t(sign | ((exp + 127) << 7) | (uint32_t(rounded) - 128));
}

static uint16_t random_bf() {
  switch (rnd() % 10) {
    case 0: return uint16_t((rnd() & 1) << 15);                                         // zero
    case 1: return uint16_t(((rnd() & 1) << 15) | (rnd() & 0x7F));                      // denormal
    case 2: return uint16_t(((rnd() & 1) << 15) | ((1 + rnd() % 3) << 7) | (rnd() & 0x7F));      // tiny
    case 3: return uint16_t(((rnd() & 1) << 15) | ((250 + rnd() % 5) << 7) | (rnd() & 0x7F));      // huge
    default: return uint16_t(((rnd() & 1) << 15) | ((110 + rnd() % 36) << 7) | (rnd() & 0x7F));
  }
}
static Zmm random_zmm() { Zmm z; for (uint16_t& h : z.h) h = random_bf(); return z; }

static Zmm run1(const Bytes& code, const Zmm& a, const Zmm& b, const Zmm& old, bool memory, uint64_t k1 = 0, uint64_t* k_out = nullptr, int dest_k = -1) {
  Machine m(code);
  set_zmm(m.cpu.state(), 0, old);
  set_zmm(m.cpu.state(), 1, a);
  if (memory) m.poke(kBase + 64, b.h, 64); else set_zmm(m.cpu.state(), 2, b);
  m.cpu.state().k[1] = k1;
  CHECK(m.finished());
  if (k_out) *k_out = m.cpu.state().k[dest_k < 0 ? 0 : dest_k];
  return get_zmm(m.cpu.state(), 0);
}

typedef double (*Binary)(double, double);

static void test_binary() {
  struct B { uint8_t op; Binary f; const char* name; };
  const B ops[] = {{0x58, [](double x, double y) { return x + y; }, "add"}, {0x5C, [](double x, double y) { return x - y; }, "sub"},
                   {0x59, [](double x, double y) { return x * y; }, "mul"}, {0x5E, [](double x, double y) { return x / y; }, "div"}};
  for (const B& o : ops)
    for (int vl = 0; vl < 3; ++vl)
      for (int iter = 0; iter < 60; ++iter) {
        const int n = 8 << vl;
        const bool memory = iter & 1, bcst = memory && (iter & 2), z = iter & 4;
        const int aaa = (iter & 8) ? 1 : 0;
        Zmm a = random_zmm(), b = random_zmm(), old = random_zmm();
        const uint64_t k = rnd();
        const Zmm got = run1(encode(5, 1, o.op, 0, 1, memory, vl, aaa, z, bcst), a, b, old, memory, k);
        for (int i = 0; i < 32; ++i) {
          uint16_t want;
          const uint16_t bi = bcst ? b.h[0] : b.h[i];
          if (i >= n) want = 0;
          else if (aaa && !((k >> i) & 1)) want = z ? 0 : old.h[i];
          else {
            const double r = o.f(daz(a.h[i]), daz(bi));
            want = to_bf(r);
            if (bf_nan(want)) continue;
          }
          if (std::isnan(o.f(daz(a.h[i]), daz(bi))) && i < n && !(aaa && !((k >> i) & 1))) continue;
          if (got.h[i] != want) printf("  %s vl %d lane %d: %04x op %04x %04x want %04x got %04x\n", o.name, vl, i, a.h[i], a.h[i], bi, want, got.h[i]);
          CHECK(got.h[i] == want);
        }
      }
  {      // sqrt
    for (int iter = 0; iter < 80; ++iter) {
      Zmm b = random_zmm();
      const Zmm got = run1(encode(5, 1, 0x51, 0, 0, false, 2), Zmm(), b, Zmm(), false);
      for (int i = 0; i < 32; ++i) {
        const double x = daz(b.h[i]);
        if (std::isnan(std::sqrt(x))) continue;
        CHECK(got.h[i] == to_bf(std::sqrt(x)));
      }
    }
  }
}

static void test_fma() {
  for (int order = 0; order < 3; ++order)
    for (int kind = 0; kind < 4; ++kind)
      for (int iter = 0; iter < 20; ++iter) {
        const uint8_t op = uint8_t((0x90 + 0x10 * order) | (8 + 2 * kind));
        Zmm a = random_zmm(), b = random_zmm(), d = random_zmm();
        const bool memory = iter & 1;
        const Zmm got = run1(encode(6, 0, op, 0, 1, memory, 2), a, b, d, memory);
        for (int i = 0; i < 32; ++i) {
          const double dd = daz(d.h[i]), x = daz(a.h[i]), y = daz(b.h[i]);
          double p, q, c;
          if (order == 0) { p = dd; q = y; c = x; }
          else if (order == 1) { p = x; q = dd; c = y; }
          else { p = x; q = y; c = dd; }
          if (kind >= 2) p = -p;
          if (kind == 1 || kind == 3) c = -c;
          const double r = std::fma(p, q, c);
          if (std::isnan(r)) continue;
          {      // (the fma is rounded twice here, to double and to bfloat16: skip the cases that are within the double's rounding of a halfway point)
            int e;
            const double frac = std::frexp(std::fabs(r), &e) * 256;
            const double tie = std::fabs(frac - std::floor(frac) - 0.5);
            if (tie < 1e-9 && std::fabs(r) > std::ldexp(1.0, -120)) continue;
          }
          if (got.h[i] != to_bf(r)) printf("  fma op %02x lane %d: want %04x got %04x\n", op, i, to_bf(r), got.h[i]);
          CHECK(got.h[i] == to_bf(r));
        }
      }
}

static void test_minmax() {
  for (int is_max = 0; is_max < 2; ++is_max)
    for (int iter = 0; iter < 40; ++iter) {
      Zmm a = random_zmm(), b = random_zmm();
      if (iter == 0) { a.h[0] = 0x7FC1; b.h[0] = 0x3F80; a.h[1] = 0x3F80; b.h[1] = 0x7FC1; a.h[2] = 0x0000; b.h[2] = 0x8000; a.h[3] = 0x8000; b.h[3] = 0x0000; a.h[4] = 0x7F81; b.h[4] = 0x3F80; }
      const Zmm got = run1(encode(5, 1, is_max ? 0x5F : 0x5D, 0, 1, false, 2), a, b, Zmm(), false);
      for (int i = 0; i < 32; ++i) {
        const uint16_t x = (a.h[i] & 0x7F80) == 0 ? uint16_t(a.h[i] & 0x8000) : a.h[i], y = (b.h[i] & 0x7F80) == 0 ? uint16_t(b.h[i] & 0x8000) : b.h[i];
        uint16_t want;
        if (bf_nan(x) || bf_nan(y) || ((x & 0x7FFF) == 0 && (y & 0x7FFF) == 0)) want = y;
        else {
          const float fx = bf_to_float(x), fy = bf_to_float(y);
          want = (is_max ? fx > fy : fx < fy) ? x : y;
        }
        CHECK(got.h[i] == want);
      }
    }
}

static void test_unary() {
  for (int iter = 0; iter < 100; ++iter) {
    Zmm b = random_zmm();
    const bool memory = iter & 1;
    // rcp, rsqrt
    Zmm rcp = run1(encode(6, 0, 0x4C, 0, 0, memory, 2), Zmm(), b, Zmm(), memory), rsq = run1(encode(6, 0, 0x4E, 0, 0, memory, 2), Zmm(), b, Zmm(), memory);
    for (int i = 0; i < 32; ++i) {
      const uint16_t y = b.h[i];
      const double x = daz(y);
      const uint16_t want_rcp = bf_nan(y) ? uint16_t(y | 0x40) : (x == 0 ? uint16_t(((y & 0x8000) ? 0x8000 : 0) | 0x7F80) : std::isinf(x) ? uint16_t(x < 0 ? 0x8000 : 0) : to_bf(1.0 / x));
      CHECK(rcp.h[i] == want_rcp);
      uint16_t want_rsq;
      if (bf_nan(y)) want_rsq = y | 0x40;
      else if (x == 0) want_rsq = ((y & 0x8000) ? 0x8000 : 0) | 0x7F80;
      else if (x < 0) want_rsq = 0xFFC0;
      else if (std::isinf(x)) want_rsq = 0;
      else want_rsq = to_bf(1.0 / std::sqrt(x));
      if (rsq.h[i] != want_rsq) printf("  rsqrt %04x: want %04x got %04x\n", y, want_rsq, rsq.h[i]);
      CHECK(rsq.h[i] == want_rsq);
    }
    // getexp
    Zmm ge = run1(encode(6, 0, 0x42, 0, 0, memory, 2), Zmm(), b, Zmm(), memory);
    for (int i = 0; i < 32; ++i) {
      const uint16_t y = b.h[i];
      uint16_t want;
      if (bf_nan(y)) want = y | 0x40;
      else if ((y & 0x7F80) == 0x7F80) want = 0x7F80;
      else if ((y & 0x7F80) == 0) want = 0xFF80;
      else want = to_bf(double(int((y >> 7) & 0xFF) - 127));
      CHECK(ge.h[i] == want);
    }
    // rndscale / reduce with M = 0..4
    const int mm = iter % 5;
    Zmm rs = run1(encode(3, 3, 0x08, 0, 0, memory, 2, 0, false, false, false, {uint8_t(mm << 4)}), Zmm(), b, Zmm(), memory);
    Zmm rd = run1(encode(3, 3, 0x56, 0, 0, memory, 2, 0, false, false, false, {uint8_t(mm << 4)}), Zmm(), b, Zmm(), memory);
    for (int i = 0; i < 32; ++i) {
      const uint16_t y = b.h[i];
      if (bf_nan(y) || (y & 0x7F80) == 0x7F80) continue;
      const double x = daz(y);
      double scaled = std::nearbyint(std::ldexp(x, mm));
      double r = std::ldexp(scaled, -mm);
      uint16_t want_rs = to_bf(r);
      if (r == 0) want_rs = (y & 0x8000) ? 0x8000 : 0;
      if (rs.h[i] != want_rs) printf("  rndscale m %d %04x: want %04x got %04x\n", mm, y, want_rs, rs.h[i]);
      CHECK(rs.h[i] == want_rs);
      uint16_t want_rd = to_bf(x - r);
      CHECK(rd.h[i] == want_rd);
    }
  }
}

static void test_scalef_getmant() {
  for (int iter = 0; iter < 80; ++iter) {
    Zmm a = random_zmm(), b;
    for (uint16_t& h : b.h) h = uint16_t(((rnd() & 1) << 15) | ((120 + rnd() % 14) << 7) | (rnd() & 0x7F));
    const Zmm got = run1(encode(6, 0, 0x2C, 0, 1, false, 2), a, b, Zmm(), false);
    for (int i = 0; i < 32; ++i) {
      const double x = daz(a.h[i]), y = daz(b.h[i]);
      const double r = std::ldexp(x, int(std::floor(y)));
      if (std::isnan(r) || std::isinf(x) || x == 0) continue;
      uint16_t want = to_bf(r);
      if (got.h[i] != want) printf("  scalef %04x %04x: want %04x got %04x\n", a.h[i], b.h[i], want, got.h[i]);
      CHECK(got.h[i] == want);
    }
  }
  for (int imm = 0; imm < 16; ++imm) {
    Zmm b = random_zmm();
    const Zmm got = run1(encode(3, 3, 0x26, 0, 0, false, 2, 0, false, false, false, {uint8_t(imm)}), Zmm(), b, Zmm(), false);
    for (int i = 0; i < 32; ++i) {
      const uint16_t y = b.h[i];
      const uint32_t sc = (imm >> 2) & 3, interval = imm & 3;
      if (bf_nan(y)) { CHECK(got.h[i] == (y | 0x40)); continue; }
      const double x = daz(y);
      if (x == 0 || std::isinf(x)) continue;
      if (x < 0 && (sc & 2)) { CHECK(got.h[i] == 0xFFC0); continue; }
      // reference: mantissa in the requested interval
      int e;
      double frac = std::frexp(std::fabs(x), &e);      // [0.5, 1)
      double m = frac * 2;                             // [1, 2)
      const int unbiased = e - 1;
      double want_m;
      switch (interval) {
        case 0: want_m = m; break;
        case 1: want_m = (unbiased & 1) ? m / 2 : m; break;
        case 2: want_m = m / 2; break;
        default: want_m = (m >= 1.5) ? m / 2 : m; break;
      }
      uint16_t want = to_bf(want_m);
      if (!(sc & 1) && x < 0) want |= 0x8000;
      if (got.h[i] != want) printf("  getmant imm %d %04x: want %04x got %04x\n", imm, y, want, got.h[i]);
      CHECK(got.h[i] == want);
    }
  }
}

static void test_compare() {
  for (int pred = 0; pred < 32; ++pred) {
    Zmm a = random_zmm(), b = random_zmm();
    for (int i = 0; i < 32; i += 3) b.h[i] = a.h[i];
    a.h[1] = 0x7FC0; b.h[4] = 0x7F81;
    uint64_t k = 0;
    run1(encode(3, 3, 0xC2, 1, 1, false, 2, 0, false, false, false, {uint8_t(pred)}), a, b, Zmm(), false, 0, &k, 1);
    // dest k1 (reg field 1): the machine puts a, b into zmm1/zmm2: here vvvv = 1, rm = 2
    uint64_t want = 0;
    for (int i = 0; i < 32; ++i) {
      const double x = daz(a.h[i]), y = daz(b.h[i]);
      const bool unord = bf_nan(a.h[i]) || bf_nan(b.h[i]);
      const bool eq = !unord && x == y, lt = !unord && x < y, gt = !unord && x > y, ord = !unord;
      bool t;
      switch (pred & 15) {
        case 0: t = eq; break; case 1: t = lt; break; case 2: t = lt || eq; break; case 3: t = unord; break;
        case 4: t = !eq; break; case 5: t = !lt; break; case 6: t = !(lt || eq); break; case 7: t = ord; break;
        case 8: t = unord || eq; break; case 9: t = unord || lt; break; case 10: t = unord || lt || eq; break; case 11: t = false; break;
        case 12: t = ord && !eq; break; case 13: t = gt || eq; break; case 14: t = gt; break; default: t = true; break;
      }
      want |= uint64_t{t} << i;
    }
    if (k != want) printf("  pred %d: want %08llx got %08llx\n", pred, (unsigned long long)want, (unsigned long long)k);
    CHECK(k == want);
  }
  // fpclass
  for (int imm = 1; imm < 256; imm += 7) {
    Zmm b = random_zmm();
    b.h[0] = 0x7FC1; b.h[1] = 0x7F81; b.h[2] = 0x0000; b.h[3] = 0x8000; b.h[4] = 0x7F80; b.h[5] = 0xFF80; b.h[6] = 0x0001; b.h[7] = 0xBF80;
    uint64_t k = 0;
    run1(encode(3, 3, 0x66, 1, 0, false, 2, 0, false, false, false, {uint8_t(imm)}), Zmm(), b, Zmm(), false, 0, &k, 1);
    uint64_t want = 0;
    for (int i = 0; i < 32; ++i) {
      const uint16_t y = b.h[i];
      const bool neg = y & 0x8000, ones = (y & 0x7F80) == 0x7F80, zeros = (y & 0x7F80) == 0, mant0 = zeros || !(y & 0x7F);
      const bool snan = ones && !mant0 && !(y & 0x40), qnan = ones && !mant0 && (y & 0x40);
      const bool t = ((imm & 1) && qnan) || ((imm & 2) && !neg && zeros) || ((imm & 4) && neg && zeros) || ((imm & 8) && !neg && ones && mant0) || ((imm & 16) && neg && ones && mant0) ||
                     ((imm & 64) && neg && !ones && !zeros) || ((imm & 128) && snan);
      want |= uint64_t{t} << i;
    }
    CHECK(k == want);
  }
}

static void test_comis() {
  struct C { uint16_t a, b; uint64_t flags; };
  const C cases[] = {{0x3F80, 0x3F80, 0x40}, {0x3F80, 0x4000, 0x01}, {0x4000, 0x3F80, 0x00}, {0x7FC0, 0x3F80, 0x45}, {0x3F80, 0x7F81, 0x45}, {0x0000, 0x8000, 0x40}, {0x0001, 0x0000, 0x40}};
  for (const C& c : cases)
    for (int memory = 0; memory < 2; ++memory) {
      Bytes code = {0x62, 0xF5, 0x7D, 0x08, 0x2F};
      if (memory) { code.push_back(0x47); code.push_back(0x20); } else code.push_back(0xC1);      // vcomisbf16 xmm0, xmm1 / [rdi + 64]
      code.push_back(0x0F); code.push_back(0x0B);
      Machine m(code);
      m.cpu.state().xmm[0] = {c.a, 0};
      m.cpu.state().xmm[1] = {c.b, 0};
      if (memory) m.poke(kBase + 64, &c.b, 2);
      m.cpu.state().rflags = 0x8D7 | 0x202;
      CHECK(m.finished());
      CHECK((m.cpu.state().rflags & 0x8D5) == c.flags);
    }
}

static void test_encoding() {
  Bytes ok = encode(5, 1, 0x58, 0, 1, false, 2);
  { Machine m(ok); CHECK(m.finished()); }
  { Bytes b = encode(5, 1, 0x58, 0, 1, false, 2, 0, false, false, true); Machine m(b); CHECK(m.undefined()); }      // W1
  { Bytes b = encode(6, 0, 0x4C, 0, 1, false, 2); Machine m(b); CHECK(m.undefined()); }      // unary with vvvv in use
  { Bytes b = encode(5, 1, 0x58, 0, 1, false, 3); Machine m(b); CHECK(m.undefined()); }      // L'L = 3 without b
  { Bytes b = ok; b[3] |= 0x10; b[3] = uint8_t((b[3] & ~0x60) | 0x20); Machine m(b); CHECK(m.undefined()); }      // EVEX.b with a register: embedded rounding does not exist here
}

int main() {
  test_binary();
  test_fma();
  test_minmax();
  test_unary();
  test_scalef_getmant();
  test_compare();
  test_comis();
  test_encoding();
  if (g_failed) { printf("Bf16Test: %d failed\n", g_failed); return 1; }
  printf("Bf16Test: all passed\n");
  return 0;
}
