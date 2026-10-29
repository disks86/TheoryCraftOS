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



// --- vminmax, vcomx, vmovd ---------------------------------------------------------------------------------------------------------------

static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dfrom(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

// The specification's minmax() on doubles (no denormal flushing); NaNs given as bit patterns.
static uint64_t model_minmax(uint64_t abits, uint64_t bbits, int imm) {
  const double a = dfrom(abits), b = dfrom(bbits);
  const bool a_nan = std::isnan(a), b_nan = std::isnan(b);
  const bool a_snan = a_nan && !(abits & (1ull << 51)), b_snan = b_nan && !(bbits & (1ull << 51));
  const int op = imm & 3, sc = (imm >> 2) & 3;
  const bool number = (imm >> 4) & 1, want_max = op & 1, magnitude = op & 2;
  const uint64_t qa = abits | (1ull << 51), qb = bbits | (1ull << 51);
  uint64_t tmp;
  bool is_nan = false;
  if (a_nan || b_nan) {
    if (!number || (a_nan && b_nan)) {
      if (a_nan) tmp = a_snan ? qa : (b_snan ? qb : abits);
      else tmp = qb;
      is_nan = true;
    } else {
      tmp = a_nan ? bbits : abits;
    }
  } else {
    const double ka = magnitude ? std::fabs(a) : a, kb = magnitude ? std::fabs(b) : b;
    double pick_a_value;
    bool pick_a;
    if (ka == kb) {
      if (magnitude && std::fabs(a) == std::fabs(b)) { /* fall through to signed comparison */ }
      if (a == b && std::signbit(a) == std::signbit(b)) pick_a = true;
      else if (a == b) pick_a = (std::signbit(a) == !want_max);      // opposite-signed zeros
      else pick_a = want_max ? a > b : a < b;                          // equal magnitudes, opposite signs
    } else {
      pick_a = want_max ? ka > kb : ka < kb;
    }
    (void)pick_a_value;
    tmp = pick_a ? abits : bbits;
  }
  if (!is_nan) {
    if (sc == 3) tmp |= 1ull << 63;
    else if (sc == 2) tmp &= ~(1ull << 63);
    else if (sc == 1 || a_nan) {}
    else tmp = (tmp & ~(1ull << 63)) | (abits & (1ull << 63));
  }
  return tmp;
}

static void test_minmax_instruction() {
  for (int imm = 0; imm < 32; ++imm)
    for (int iter = 0; iter < 30; ++iter) {
      uint64_t a, b;
      switch (iter % 6) {
        case 0: a = dbits(double(int(rnd() % 21) - 10)); b = dbits(double(int(rnd() % 21) - 10)); break;
        case 1: a = dbits(0.0); b = dbits(-0.0); break;
        case 2: a = dbits(-0.0); b = dbits(0.0); break;
        case 3: { const double v = double(rnd() % 100 + 1); a = dbits(v); b = dbits(-v); break; }
        case 4: a = 0x7FF8000000000123ull; b = dbits(3.5); break;
        default: a = dbits(double(int64_t(rnd() % 2001) - 1000) / 8); b = dbits(double(int64_t(rnd() % 2001) - 1000) / 8); break;
      }
      if (iter == 5) { a = 0x7FF0000000000001ull; b = dbits(1.0); }
      if (iter == 11) { a = dbits(1.0); b = 0x7FF0000000000002ull; }
      if (iter == 17) { a = 0x7FF8000000000001ull; b = 0x7FF0000000000002ull; }
      if (iter == 23) { a = 0x7FF0000000000001ull; b = 0x7FF8000000000002ull; }
      Machine m(encode(3, 1, 0x52, 0, 1, false, 2, 0, false, false, true, {uint8_t(imm)}));
      for (int i = 0; i < 8; ++i) { /* all lanes the same */ }
      uint64_t q[8];
      for (int i = 0; i < 8; ++i) q[i] = a;
      Zmm za, zb;
      memcpy(za.h, q, 64);
      for (int i = 0; i < 8; ++i) q[i] = b;
      memcpy(zb.h, q, 64);
      set_zmm(m.cpu.state(), 1, za);
      set_zmm(m.cpu.state(), 2, zb);
      CHECK(m.finished());
      const Zmm got = get_zmm(m.cpu.state(), 0);
      uint64_t g;
      memcpy(&g, got.h, 8);
      const uint64_t want = model_minmax(a, b, imm);
      if (g != want) printf("  minmax imm %02x a %016llx b %016llx: want %016llx got %016llx\n", imm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)want, (unsigned long long)g);
      CHECK(g == want);
      const bool snan_in = (std::isnan(dfrom(a)) && !(a & (1ull << 51))) || (std::isnan(dfrom(b)) && !(b & (1ull << 51)));
      CHECK(((m.cpu.state().mxcsr & 1) != 0) == snan_in);
    }
  {      // the scalar form keeps the upper part of the first source, and a masked-off result merges
    Machine m(encode(3, 1, 0x53, 0, 1, false, 0, 0, false, false, true, {0x01}));      // vminmaxsd xmm0, xmm1, xmm2, max
    m.cpu.state().xmm[1] = {dbits(2.0), 0x1111};
    m.cpu.state().xmm[2] = {dbits(5.0), 0x2222};
    CHECK(m.finished());
    CHECK(m.cpu.state().xmm[0].lo == dbits(5.0) && m.cpu.state().xmm[0].hi == 0x1111);
  }
}

static void test_comx() {
  struct C { uint64_t a, b; bool signaling; };
  const double nan = dfrom(0x7FF8000000000000ull);
  const struct { double a, b; uint64_t flags; } cases[] = {{1.0, 2.0, 0x801 | 0x0}, {2.0, 1.0, 0}, {1.0, 1.0, 0x800 | 0x80 | 0x40}, {nan, 1.0, 0x800 | 0x80 | 4 | 1}, {1.0, nan, 0x800 | 0x80 | 4 | 1}};
  for (const auto& c : cases)
    for (int unordered_variant = 0; unordered_variant < 2; ++unordered_variant) {
      Bytes code = {0x62, 0xF1, 0xFF, 0x08, uint8_t(unordered_variant ? 0x2E : 0x2F), 0xC1, 0x0F, 0x0B};      // vcomxsd / vucomxsd xmm0, xmm1
      Machine m(code);
      m.cpu.state().xmm[0] = {dbits(c.a), 0};
      m.cpu.state().xmm[1] = {dbits(c.b), 0};
      m.cpu.state().rflags = 0x202 | 0x10;
      CHECK(m.finished());
      CHECK((m.cpu.state().rflags & 0x8D5) == c.flags);
      const bool qnan = std::isnan(c.a) || std::isnan(c.b);
      CHECK(((m.cpu.state().mxcsr & 1) != 0) == (qnan && !unordered_variant));
    }
  Bytes bad = {0x62, 0xF1, 0xF7, 0x08, 0x2F, 0xC1, 0x0F, 0x0B};      // vvvv in use
  Machine m(bad);
  CHECK(m.undefined());
}

static void test_vmovd() {
  {
    Bytes code = {0x62, 0xF1, 0x7E, 0x08, 0x7E, 0xC1, 0x0F, 0x0B};      // vmovd xmm0, xmm1
    Machine m(code);
    m.cpu.state().xmm[1] = {0x1122334455667788ull, 0x99};
    m.cpu.state().xmm[0] = {~0ull, ~0ull};
    CHECK(m.finished());
    CHECK(m.cpu.state().xmm[0].lo == 0x55667788ull && m.cpu.state().xmm[0].hi == 0);
  }
  {
    Bytes code = {0x62, 0xF1, 0x7D, 0x08, 0xD6, 0x07, 0x0F, 0x0B};      // vmovd [rdi], xmm0
    Machine m(code);
    m.cpu.state().xmm[0] = {0x1122334455667788ull, 0};
    const uint64_t pattern = ~0ull;
    m.poke(kBase, &pattern, 8);
    CHECK(m.finished());
    uint64_t v = 0;
    GuestMemory gm(m.space);
    gm.read(kBase, ByteView(reinterpret_cast<uint8_t*>(&v), 8));
    CHECK(v == 0xFFFFFFFF55667788ull);
  }
}



// --- vdpphps, vmpsadbw, saturating conversions --------------------------------------------------------------------------------------------------

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float from_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static float half_float(uint16_t h) {
  const int sign = h >> 15, exp = (h >> 10) & 31, man = h & 1023;
  double v;
  if (exp == 0) v = std::ldexp(man, -24);
  else if (exp == 31) return man ? NAN : (sign ? -INFINITY : INFINITY);
  else v = std::ldexp(1024 + man, exp - 25);
  return float(sign ? -v : v);
}
static uint16_t random_half() { return uint16_t(((rnd() & 1) << 15) | ((8 + rnd() % 14) << 10) | (rnd() & 0x3FF)); }

static void test_dpphps() {
  for (int vl = 0; vl < 3; ++vl)
    for (int iter = 0; iter < 30; ++iter) {
      Zmm a, b, d;
      for (int i = 0; i < 32; ++i) { a.h[i] = random_half(); b.h[i] = random_half(); }
      float df[16];
      for (float& f : df) f = float(int(rnd() % 2001) - 1000) / 16;
      memcpy(d.h, df, 64);
      const bool memory = iter & 1;
      const Zmm got = run1(encode(2, 0, 0x52, 0, 1, memory, vl), a, b, d, memory);
      float out[16];
      memcpy(out, got.h, 64);
      for (int i = 0; i < 4 << vl; ++i) {
        float acc = df[i];
        acc = std::fmaf(half_float(a.h[2 * i + 1]), half_float(b.h[2 * i + 1]), acc);
        acc = std::fmaf(half_float(a.h[2 * i]), half_float(b.h[2 * i]), acc);
        if ((bits(acc) & 0x7F800000u) == 0) acc = from_bits(bits(acc) & 0x80000000u);
        CHECK(bits(out[i]) == bits(acc));
      }
      for (int i = 4 << vl; i < 16; ++i) CHECK(bits(out[i]) == 0);
    }
}

static void test_mpsadbw() {
  for (int vl = 0; vl < 3; ++vl)
    for (int imm = 0; imm < 64; imm += 5) {
      Zmm a, b;
      uint8_t ab[64], bb[64];
      for (int i = 0; i < 64; ++i) { ab[i] = uint8_t(rnd()); bb[i] = uint8_t(rnd()); }
      memcpy(a.h, ab, 64); memcpy(b.h, bb, 64);
      const Zmm got = run1(encode(3, 2, 0x42, 0, 1, false, vl, 0, false, false, false, {uint8_t(imm)}), a, b, Zmm(), false);
      uint16_t out[32];
      memcpy(out, got.h, 64);
      for (int lane = 0; lane < (1 << vl); ++lane) {
        const int control = (imm >> ((lane & 1) * 3)) & 7, blk2 = (control & 3) * 4, blk1 = ((control >> 2) & 1) * 4;
        for (int i = 0; i < 8; ++i) {
          int sum = 0;
          for (int j = 0; j < 4; ++j) sum += std::abs(int(ab[lane * 16 + blk1 + i + j]) - int(bb[lane * 16 + blk2 + j]));
          CHECK(out[lane * 8 + i] == sum);
        }
      }
    }
}

static int64_t sat_trunc(double x, int bits, bool is_unsigned) {
  if (std::isnan(x)) return 0;
  if (bits == 64 && std::isinf(x)) return x > 0 ? (is_unsigned ? -1 : INT64_MAX) : (is_unsigned ? 0 : INT64_MIN);
  const double lo = is_unsigned ? 0 : -std::ldexp(1.0, bits - 1), hi = is_unsigned ? std::ldexp(1.0, bits) : std::ldexp(1.0, bits - 1);
  const double t = std::trunc(x);
  if (t <= lo) return (int64_t)lo;
  if (t >= hi) return is_unsigned ? (int64_t)(uint64_t)(hi - 1 + (bits == 64 ? 0 : 0)) : (int64_t)(hi - 1);
  return (int64_t)t;
}

static void test_saturating() {
  for (int iter = 0; iter < 60; ++iter) {
    for (int unsigned_form = 0; unsigned_form < 2; ++unsigned_form) {
      // vcvttps2dqs / vcvttps2udqs: NP map5 6D / 6C, W0
      float f[16];
      Zmm a;
      for (int i = 0; i < 16; ++i) {
        switch (rnd() % 6) {
          case 0: f[i] = NAN; break;
          case 1: f[i] = (rnd() & 1) ? INFINITY : -INFINITY; break;
          case 2: f[i] = float(int(rnd() % 200) - 100) + 0.5f; break;
          case 3: f[i] = float(std::ldexp(1.0 + double(rnd() % 100) / 100, int(rnd() % 40))) * ((rnd() & 1) ? 1 : -1); break;
          default: f[i] = float(int(rnd() % 4001) - 2000) / 8; break;
        }
      }
      memcpy(a.h, f, 64);
      const Zmm got = run1(encode(5, 0, unsigned_form ? 0x6C : 0x6D, 0, 0, false, 2), Zmm(), a, Zmm(), false);
      uint32_t out[16];
      memcpy(out, got.h, 64);
      for (int i = 0; i < 16; ++i) {
        const int64_t want = sat_trunc(f[i], 32, unsigned_form);
        CHECK(out[i] == uint32_t(want));
      }
      // vcvttpd2dqs / udqs: NP map5 W1: the result is half the width
      double d[8];
      for (int i = 0; i < 8; ++i) d[i] = f[i] * 3000.0;
      Zmm ad;
      memcpy(ad.h, d, 64);
      const Zmm got2 = run1(encode(5, 0, unsigned_form ? 0x6C : 0x6D, 0, 0, false, 2, 0, false, false, true), Zmm(), ad, Zmm(), false);
      uint32_t out2[16];
      memcpy(out2, got2.h, 64);
      for (int i = 0; i < 8; ++i) CHECK(out2[i] == uint32_t(sat_trunc(d[i], 32, unsigned_form)));
      for (int i = 8; i < 16; ++i) CHECK(out2[i] == 0);
      // vcvttps2qqs / uqqs: 66 map5 W0 on a half-width source (ymm source for the zmm result)
      Zmm a3;
      memcpy(a3.h, f, 32);
      const Zmm got3 = run1(encode(5, 1, unsigned_form ? 0x6C : 0x6D, 0, 0, false, 2), Zmm(), a3, Zmm(), false);
      uint64_t out3[8];
      memcpy(out3, got3.h, 64);
      for (int i = 0; i < 8; ++i) {
        if (unsigned_form && f[i] >= 0 && f[i] >= 1.8446744073709552e19f && std::isfinite(f[i])) { CHECK(out3[i] == ~0ull); continue; }
        if (!unsigned_form && std::isfinite(f[i]) && std::fabs(f[i]) >= 9.2233720368547758e18f) { CHECK(out3[i] == (f[i] > 0 ? 0x7FFFFFFFFFFFFFFFull : 0x8000000000000000ull)); continue; }
        if (out3[i] != uint64_t(sat_trunc(f[i], 64, unsigned_form))) printf("  qq unsigned %d f %g got %llx want %llx\n", unsigned_form, f[i], (unsigned long long)out3[i], (unsigned long long)sat_trunc(f[i], 64, unsigned_form));
        CHECK(out3[i] == uint64_t(sat_trunc(f[i], 64, unsigned_form)));
      }
    }
  }
  {      // vcvtph2ibs / vcvttbf162iubs: the byte in the low half of the element
    Zmm a;
    for (int i = 0; i < 32; ++i) a.h[i] = to_bf(double(int(rnd() % 600) - 300) / 4);
    const Zmm got = run1(encode(5, 3, 0x6A, 0, 0, false, 2), Zmm(), a, Zmm(), false);      // F2 map5 6A: vcvttbf162iubs
    for (int i = 0; i < 32; ++i) CHECK(got.h[i] == uint16_t(sat_trunc(daz(a.h[i]), 8, true)));
    const Zmm got2 = run1(encode(5, 3, 0x69, 0, 0, false, 2), Zmm(), a, Zmm(), false);     // vcvtbf162ibs: rounds to nearest even
    for (int i = 0; i < 32; ++i) {
      const double x = daz(a.h[i]);
      double r = std::nearbyint(x);
      if (r > 127) r = 127;
      if (r < -128) r = -128;
      CHECK(got2.h[i] == uint16_t(uint8_t(int(r))));
    }
  }
  {      // vcvttsd2sis / vcvttss2usis into a general register
    Bytes code = {0x62, 0xF5, 0xFF, 0x08, 0x6D, 0xC1, 0x0F, 0x0B};      // vcvttsd2sis rax, xmm1  (F2, W1)
    Machine m(code);
    m.cpu.state().xmm[1] = {dbits(1e30), 0};
    CHECK(m.finished());
    CHECK(m.cpu.state().r[0] == 0x7FFFFFFFFFFFFFFFull);
    CHECK(m.cpu.state().mxcsr & 1);
    Bytes code2 = {0x62, 0xF5, 0x7E, 0x08, 0x6C, 0xC1, 0x0F, 0x0B};      // vcvttss2usis eax, xmm1  (F3, W0)
    Machine m2(code2);
    float neg = -5.5f;
    uint32_t nb; memcpy(&nb, &neg, 4);
    m2.cpu.state().xmm[1] = {nb, 0};
    CHECK(m2.finished());
    CHECK(m2.cpu.state().r[0] == 0);
  }
}

// --- FP8 conversions ---------------------------------------------------------------------------------------------------------------------
// The model works on values: the finite E5M2 / E4M3 numbers are enumerated, the fp16 input is placed between two of them and rounded by the instruction's rule (nearest even, or the bias added below the
// LSB). The two overflow slots are code 0x7C of E5M2 (infinity, value 2^16) and 0x7F of E4M3 (NaN, value 480): reaching either is an overflow.

static double fp8_value(int code, bool hf8) {
  if (hf8) { const int e = code >> 3, m = code & 7; return e == 0 ? m / 8.0 * std::ldexp(1.0, -6) : (1 + m / 8.0) * std::ldexp(1.0, e - 7); }
  const int e = code >> 2, m = code & 3;
  return e == 0 ? m / 4.0 * std::ldexp(1.0, -14) : (1 + m / 4.0) * std::ldexp(1.0, e - 15);
}

static uint8_t model_to_fp8(uint16_t x, bool hf8, bool sat, int bias) {
  const uint8_t sign = (x >> 8) & 0x80;
  const int e16 = (x >> 10) & 31, m16 = x & 0x3FF;
  const int max_code = hf8 ? 0x7E : 0x7B, slot = hf8 ? 0x7F : 0x7C;
  if (e16 == 31) {
    if (m16 == 0) return sign | (sat ? max_code : slot);
    return hf8 ? (sign | 0x7F) : uint8_t((x >> 8) | 2);
  }
  const double a = e16 == 0 ? m16 * std::ldexp(1.0, -24) : (1 + m16 / 1024.0) * std::ldexp(1.0, e16 - 15);
  int q = 0;
  for (int c = 0; c <= slot; ++c) if (fp8_value(c, hf8) <= a) q = c;
  int code = q;
  if (q < slot) {
    const double lo = fp8_value(q, hf8), hi = fp8_value(q + 1, hf8), f = (a - lo) / (hi - lo);
    bool up;
    if (bias < 0) up = f > 0.5 || (f == 0.5 && (q & 1));
    else if (hf8 && e16 >= 9) up = int(f * 128) + (bias >> 1) >= 128;
    else up = int(f * 256) + bias >= 256;
    code = q + up;
  }
  if (code >= slot) code = sat ? max_code : slot;
  return sign | uint8_t(code);
}

static const struct Fp8Form { int map, pp; uint8_t op; bool hf8, sat; } kFp8Single[] = {{2, 2, 0x74, false, false}, {5, 2, 0x74, false, true}, {5, 2, 0x18, true, false}, {5, 2, 0x1B, true, true}};

// pp: 2 = vcvtph2*, 3 = vcvt2ph2*, 0 = vcvtbiasph2*
static Fp8Form fp8_form(int index, int pp) { Fp8Form f = kFp8Single[index]; f.pp = pp; return f; }

static void test_fp8() {
  for (int index = 0; index < 4; ++index) {
    for (int biased = 0; biased < 2; ++biased) {
      const Fp8Form one = fp8_form(index, biased ? 0 : 2), two = fp8_form(index, 3);
      (void)two;
      for (int chunk = 0; chunk < 2048; ++chunk) {
        Zmm src, bias_reg;
        for (int i = 0; i < 32; ++i) src.h[i] = uint16_t(chunk * 32 + i);
        int biases[32];
        for (int i = 0; i < 32; ++i) { biases[i] = biased ? int(rnd() & 0xFF) : -1; bias_reg.h[i] = uint16_t((rnd() & 0xFF00) | (biases[i] < 0 ? 0 : biases[i])); }
        const Zmm got = run1(encode(one.map, one.pp, one.op, 0, biased ? 1 : 0, false, 2), bias_reg, src, Zmm(), false);
        uint8_t bytes[64];
        memcpy(bytes, got.h, 64);
        for (int i = 0; i < 32; ++i) {
          const uint8_t want = model_to_fp8(src.h[i], one.hf8, one.sat, biases[i]);
          if (bytes[i] != want) printf("  single idx %d biased %d x %04x bias %d got %02x want %02x\n", index, biased, src.h[i], biases[i], bytes[i], want);
          CHECK(bytes[i] == want);
        }
        for (int i = 32; i < 64; ++i) CHECK(bytes[i] == 0);
      }
    }
    {      // the fixed biases: 0 truncates, 127 ties toward zero, 128 ties away, 255 rounds away
      const Fp8Form f = fp8_form(index, 0);
      for (int bias : {0, 127, 128, 255}) {
        for (int chunk = 0; chunk < 2048; chunk += 7) {
          Zmm src, bias_reg;
          for (int i = 0; i < 32; ++i) { src.h[i] = uint16_t(chunk * 32 + i); bias_reg.h[i] = uint16_t(bias); }
          const Zmm got = run1(encode(f.map, f.pp, f.op, 0, 1, false, 2), bias_reg, src, Zmm(), false);
          uint8_t bytes[64];
          memcpy(bytes, got.h, 64);
          for (int i = 0; i < 32; ++i) CHECK(bytes[i] == model_to_fp8(src.h[i], f.hf8, f.sat, bias));
        }
      }
    }
    {      // two sources: src2 (the r/m operand) fills the low half of the bytes, src1 the high half
      const Fp8Form f = fp8_form(index, 3);
      for (int chunk = 0; chunk < 1024; ++chunk) {
        Zmm a, b;
        for (int i = 0; i < 32; ++i) { a.h[i] = uint16_t(chunk * 64 + i); b.h[i] = uint16_t(chunk * 64 + 32 + i); }
        const Zmm got = run1(encode(f.map, f.pp, f.op, 0, 1, false, 1), a, b, Zmm(), false);      // ymm: 32 bytes
        uint8_t bytes[64];
        memcpy(bytes, got.h, 64);
        for (int i = 0; i < 16; ++i) CHECK(bytes[i] == model_to_fp8(b.h[i], f.hf8, f.sat, -1));
        for (int i = 16; i < 32; ++i) CHECK(bytes[i] == model_to_fp8(a.h[i - 16], f.hf8, f.sat, -1));
        for (int i = 32; i < 64; ++i) CHECK(bytes[i] == 0);
      }
      {      // 512 bits, a masked merge, a zeroing mask, and a broadcast memory operand
        Zmm a, b, old;
        for (int i = 0; i < 32; ++i) { a.h[i] = uint16_t(rnd()); b.h[i] = uint16_t(rnd()); old.h[i] = uint16_t(rnd()); }
        const uint64_t k = rnd();
        for (int zeroing = 0; zeroing < 2; ++zeroing) {
          const Zmm got = run1(encode(f.map, f.pp, f.op, 0, 1, false, 2, 1, zeroing), a, b, old, false, k);
          uint8_t bytes[64], oldb[64];
          memcpy(bytes, got.h, 64);
          memcpy(oldb, old.h, 64);
          for (int i = 0; i < 64; ++i) {
            const uint8_t full = model_to_fp8(i < 32 ? b.h[i] : a.h[i - 32], f.hf8, f.sat, -1);
            CHECK(bytes[i] == (((k >> i) & 1) ? full : (zeroing ? 0 : oldb[i])));
          }
        }
        const Zmm got = run1(encode(f.map, f.pp, f.op, 0, 1, true, 2, 0, false, true), a, b, Zmm(), true);
        uint8_t bytes[64];
        memcpy(bytes, got.h, 64);
        for (int i = 0; i < 32; ++i) CHECK(bytes[i] == model_to_fp8(b.h[0], f.hf8, f.sat, -1));
        for (int i = 32; i < 64; ++i) CHECK(bytes[i] == model_to_fp8(a.h[i - 32], f.hf8, f.sat, -1));
        // the single-source form with a broadcast element and a mask
        const Fp8Form s = fp8_form(index, 2);
        const Zmm got1 = run1(encode(s.map, s.pp, s.op, 0, 0, true, 2, 1, true, true), Zmm(), b, old, true, k);
        memcpy(bytes, got1.h, 64);
        for (int i = 0; i < 32; ++i) CHECK(bytes[i] == (((k >> i) & 1) ? model_to_fp8(b.h[0], s.hf8, s.sat, -1) : 0));
        for (int i = 32; i < 64; ++i) CHECK(bytes[i] == 0);
      }
    }
  }
  {      // vcvthf82ph: all 256 codes, exact
    for (int chunk = 0; chunk < 8; ++chunk) {
      Zmm src;
      uint8_t* sb = reinterpret_cast<uint8_t*>(src.h);
      for (int i = 0; i < 32; ++i) sb[i] = uint8_t(chunk * 32 + i);
      const Zmm got = run1(encode(5, 3, 0x1E, 0, 0, false, 2), Zmm(), src, Zmm(), false);
      for (int i = 0; i < 32; ++i) {
        const int code = chunk * 32 + i, mag = code & 0x7F;
        uint16_t want;
        if (mag == 0x7F) want = uint16_t(((code & 0x80) << 8) | 0x7F80);
        else {
          const double v = fp8_value(mag, true);
          int e;
          const double fr = v == 0 ? 0 : std::frexp(v, &e);
          want = v == 0 ? 0 : uint16_t(((e - 1 + 15) << 10) | (uint32_t(std::ldexp(fr * 2 - 1, 10)) & 0x3FF));
          want |= uint16_t((code & 0x80) << 8);
        }
        if (got.h[i] != want) printf("  hf82ph code %02x got %04x want %04x\n", code, got.h[i], want);
        CHECK(got.h[i] == want);
      }
    }
    // the 128-bit form reads 8 bytes and writes 8 halves; memory and a mask
    Zmm src;
    for (int i = 0; i < 32; ++i) src.h[i] = uint16_t(rnd());
    Zmm old;
    for (int i = 0; i < 32; ++i) old.h[i] = uint16_t(rnd());
    Bytes mem_code = encode(5, 3, 0x1E, 0, 0, true, 0, 1);
    mem_code[6] = 8;      // disp8 * 8 (the memory operand is half the vector)
    const Zmm got = run1(mem_code, Zmm(), src, old, true, 0x5A);
    const uint8_t* sb = reinterpret_cast<const uint8_t*>(src.h);
    for (int i = 0; i < 8; ++i) {
      const bool on = (0x5A >> i) & 1;
      Zmm one;
      (void)one;
      if (on) {
        const int mag = sb[i] & 0x7F;
        const bool nan = mag == 0x7F;
        CHECK(((got.h[i] & 0x7C00) == 0x7C00) == nan);
        CHECK((got.h[i] >> 15) == (sb[i] >> 7));
      } else {
        CHECK(got.h[i] == old.h[i]);
      }
    }
    for (int i = 8; i < 32; ++i) CHECK(got.h[i] == 0);
  }
  {      // vcvt2ps2phx against the host's float -> _Float16 conversion
    for (int iter = 0; iter < 200; ++iter) {
      float fa[16], fb[16];
      for (int i = 0; i < 16; ++i) {
        fa[i] = float(std::ldexp(double(int(rnd() % 20001) - 10000) / 7.0, int(rnd() % 40) - 25));
        fb[i] = float(std::ldexp(double(int(rnd() % 20001) - 10000) / 3.0, int(rnd() % 30) - 12));
        if (rnd() % 17 == 0) fa[i] = INFINITY;
        if (rnd() % 17 == 0) fb[i] = -INFINITY;
      }
      Zmm a, b;
      memcpy(a.h, fa, 64);
      memcpy(b.h, fb, 64);
      const Zmm got = run1(encode(2, 1, 0x67, 0, 1, false, 2), a, b, Zmm(), false);
      for (int i = 0; i < 32; ++i) {
        const _Float16 h = static_cast<_Float16>(i < 16 ? fb[i] : fa[i - 16]);
        uint16_t want;
        memcpy(&want, &h, 2);
        CHECK(got.h[i] == want);
      }
    }
    {      // embedded rounding: 1 + 2^-11 + 2^-13 rounds to 0x3C01 to nearest and to 0x3C00 toward zero
      float x = 1.0f + std::ldexp(1.0f, -11) + std::ldexp(1.0f, -13);
      Zmm b;
      for (int i = 0; i < 16; ++i) memcpy(&b.h[2 * i], &x, 4);
      CHECK(run1(encode(2, 1, 0x67, 0, 1, false, 2), Zmm(), b, Zmm(), false).h[0] == 0x3C01);
      Bytes code = {0x62, 0xF2, 0x75, 0x78, 0x67, 0xC2, 0x0F, 0x0B};      // vcvt2ps2phx zmm0, zmm1, zmm2, {rz-sae}
      Machine m(code);
      set_zmm(m.cpu.state(), 2, b);
      CHECK(m.finished());
      CHECK(get_zmm(m.cpu.state(), 0).h[0] == 0x3C00);
    }
  }
  {      // a nonzero VEX.vvvv on the single-source forms and W1 are undefined
    CHECK(Machine(encode(5, 2, 0x18, 0, 1, false, 2)).undefined());
    CHECK(Machine(encode(5, 2, 0x18, 0, 0, false, 2, 0, false, false, true)).undefined());
    CHECK(Machine(encode(5, 3, 0x1E, 0, 1, false, 2)).undefined());
  }
}

int main() {
  test_fp8();
  test_dpphps();
  test_mpsadbw();
  test_saturating();
  test_minmax_instruction();
  test_comx();
  test_vmovd();
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
