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

// Host test of the AMD FMA4, XOP and TBM instructions. The test CPUs do not have them, so each is checked against a model written from the AMD definition: the fused multiply-adds against
// the host's fma, the integer multiply-accumulates, vpcmov, vpperm, rotates, shifts, vpcom, horizontal adds and vfrcz against straightforward arithmetic on the lanes, and the trailing bit
// manipulation instructions against their defining expressions. Truncated and bit-flipped encodings must not misbehave under the sanitizers.
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
  explicit Machine(const Bytes& code, uint64_t code_at = 0x10000) {
    space.map(0x10000, code_at == 0x10000 ? 0x2000 : 0x1000, process::kProtRead | process::kProtWrite | process::kProtExecute);
    space.write_privileged(code_at, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    end = code_at + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = code_at; start.stack_pointer = 0x30000;
    cpu.reset(start);
    cpu.state().r[7] = kBase;
  }
  StopReason run(uint64_t n = 200) { process::AddressSpace& s = space; GuestMemory m(s); return cpu.run(m, n); }
  bool finished() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  bool undefined() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc != end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
};

struct V { uint8_t b[32]; };      // a YMM register
static V random_v() { V v; for (uint8_t& x : v.b) x = uint8_t(rnd()); return v; }
static void set_ymm(X86_64Cpu::State& st, int reg, const V& v) {
  uint64_t q[4];
  memcpy(q, v.b, 32);
  st.xmm[reg] = {q[0], q[1]};
  st.ymm_high[reg].lo = q[2]; st.ymm_high[reg].hi = q[3];
}
static V get_ymm(const X86_64Cpu::State& st, int reg) {
  const uint64_t q[4] = {st.xmm[reg].lo, st.xmm[reg].hi, st.ymm_high[reg].lo, st.ymm_high[reg].hi};
  V v;
  memcpy(v.b, q, 32);
  return v;
}

// XOP: 8F, map, W vvvv L pp=0, opcode, modrm (reg 0, rm = xmm2 or [rdi]), then imm.
static Bytes xop(int map, uint8_t op, bool w, int vvvv, bool l, bool memory, std::initializer_list<uint8_t> imm = {}, int reg = 0) {
  Bytes b = {0x8F, uint8_t(0xE0 | map), uint8_t((w ? 0x80 : 0) | ((~vvvv & 15) << 3) | (l ? 4 : 0)), op};
  if (memory) b.push_back(uint8_t((reg << 3) | 7)); else b.push_back(uint8_t(0xC0 | (reg << 3) | 2));
  b.insert(b.end(), imm);
  b.push_back(0x0F); b.push_back(0x0B);
  return b;
}

// Runs `code` with xmm1 = a, xmm2 = b (or the memory at kBase), xmm3 = c, xmm0 = old.
struct Ops { V old, a, b, c; };
static Ops random_ops() { return {random_v(), random_v(), random_v(), random_v()}; }
static V exec(const Bytes& code, const Ops& o, bool memory, bool* ok = nullptr) {
  Machine m(code);
  set_ymm(m.cpu.state(), 0, o.old); set_ymm(m.cpu.state(), 1, o.a); set_ymm(m.cpu.state(), 2, o.b); set_ymm(m.cpu.state(), 3, o.c);
  if (memory) m.poke(kBase, o.b.b, 32);
  const bool fine = m.finished();
  if (ok) *ok = fine; else CHECK(fine);
  return get_ymm(m.cpu.state(), 0);
}

template <typename T> static T lane(const V& v, int i) { T t; memcpy(&t, v.b + sizeof(T) * i, sizeof(T)); return t; }
template <typename T> static void put(V* v, int i, T t) { memcpy(v->b + sizeof(T) * i, &t, sizeof(T)); }
static bool same(const V& a, const V& b, int bytes) { return memcmp(a.b, b.b, bytes) == 0; }
static V zero_v() { V v; memset(v.b, 0, 32); return v; }

// --- FMA4 ------------------------------------------------------------------------------------------------------------------------------

static float rand_float() { return float(int(rnd() % 4001) - 2000) / 64.0f; }
static double rand_double() { return double(int(rnd() % 4001) - 2000) / 64.0; }

static void test_fma4() {
  struct Op { uint8_t op; bool neg_product, neg_addend, addsub, subadd, scalar, dbl; };
  std::vector<Op> ops;
  for (uint8_t op : {0x5C, 0x5D, 0x5E, 0x5F}) ops.push_back({op, false, false, op < 0x5E, op >= 0x5E, false, bool(op & 1)});
  for (uint8_t base : {0x68, 0x6C, 0x78, 0x7C})
    for (int k = 0; k < 4; ++k) ops.push_back({uint8_t(base + k), base >= 0x78, (base & 4) != 0, false, false, k >= 2, bool(k & 1)});
  for (const Op& o : ops)
    for (int w = 0; w < 2; ++w)
      for (int l = 0; l < 2; ++l) {
        if (o.scalar && l) continue;
        for (int memory = 0; memory < 2; ++memory) {
          const Ops in = random_ops();
          V a = in.a, b = in.b, c = in.c;
          const int lanes = o.dbl ? 2 : 4, count = (o.scalar ? 1 : lanes) * (l ? 2 : 1);
          for (int i = 0; i < 8; ++i) {
            if (o.dbl) { put<double>(&a, i % 4, rand_double()); put<double>(&b, i % 4, rand_double()); put<double>(&c, i % 4, rand_double()); }
            else { put<float>(&a, i, rand_float()); put<float>(&b, i, rand_float()); put<float>(&c, i, rand_float()); }
          }
          // the encoding: vfmaddps xmm0, xmm1, xmm2/m, xmm3 (W0) or xmm0, xmm1, xmm3, xmm2/m (W1)
          Bytes code = {0xC4, 0xE3, uint8_t((w ? 0x80 : 0) | ((~1 & 15) << 3) | (l ? 4 : 0) | 1), o.op};
          if (memory) code.push_back(0x07); else code.push_back(0xC2);
          code.push_back(0x30);
          code.push_back(0x0F); code.push_back(0x0B);
          Ops use = in;
          use.a = a; use.b = b; use.c = c;
          const V got = exec(code, use, memory);
          const V& mul2 = w ? c : b;      // the multiplier and the addend
          const V& add = w ? b : c;
          V want = zero_v();
          for (int i = 0; i < count; ++i) {
            if (o.dbl) {
              double p = lane<double>(a, i), q = lane<double>(mul2, i), r = lane<double>(add, i);
              if (o.neg_product) p = -p;
              bool sub = o.neg_addend;
              if (o.addsub) sub = (i & 1) == 0;
              if (o.subadd) sub = (i & 1) == 1;
              put<double>(&want, i, std::fma(p, q, sub ? -r : r));
            } else {
              float p = lane<float>(a, i), q = lane<float>(mul2, i), r = lane<float>(add, i);
              if (o.neg_product) p = -p;
              bool sub = o.neg_addend;
              if (o.addsub) sub = (i & 1) == 0;
              if (o.subadd) sub = (i & 1) == 1;
              put<float>(&want, i, std::fmaf(p, q, sub ? -r : r));
            }
          }
          if (memcmp(want.b, got.b, 32) != 0) printf("  op %02x w %d l %d memory %d\n", o.op, w, l, memory);
          CHECK(same(want, got, 32));
        }
      }
}

// --- XOP -------------------------------------------------------------------------------------------------------------------------------

static int64_t sext(uint64_t v, int bits) { return bits == 64 ? int64_t(v) : int64_t(v << (64 - bits)) >> (64 - bits); }
static uint64_t wrap(__int128 v, int bits) { return bits == 64 ? uint64_t(v) : uint64_t(v) & ((uint64_t(1) << bits) - 1); }
static __int128 sat(__int128 v, int bits) {
  const __int128 hi = (__int128(1) << (bits - 1)) - 1, lo = -(__int128(1) << (bits - 1));
  return v < lo ? lo : v > hi ? hi : v;
}
static uint64_t get_u(const V& v, int w, int i) { uint64_t r = 0; memcpy(&r, v.b + (w / 8) * i, w / 8); return r; }
static void set_u(V* v, int w, int i, uint64_t x) { memcpy(v->b + (w / 8) * i, &x, w / 8); }

static void test_mac() {
  struct M { uint8_t op; };
  for (uint8_t op : {0x85, 0x86, 0x87, 0x8E, 0x8F, 0x95, 0x96, 0x97, 0x9E, 0x9F, 0xA6, 0xB6})
    for (int iter = 0; iter < 60; ++iter) {
      const Ops in = random_ops();
      const bool memory = iter & 1, saturating = op == 0x85 || op == 0x86 || op == 0x87 || op == 0x8E || op == 0x8F || op == 0xA6;
      // vpmacs xmm0, xmm1, xmm2/m, xmm3
      const V got = exec(xop(8, op, false, 1, false, memory, {0x30}), in, memory);
      V want = zero_v();
      auto fix = [&](__int128 v, int bits) { return saturating ? uint64_t(sat(v, bits)) & (bits == 64 ? ~0ull : (1ull << bits) - 1) : wrap(v, bits); };
      switch (op) {
        case 0x85: case 0x95:
          for (int i = 0; i < 8; ++i) set_u(&want, 16, i, fix(__int128(sext(get_u(in.a, 16, i), 16)) * sext(get_u(in.b, 16, i), 16) + sext(get_u(in.c, 16, i), 16), 16));
          break;
        case 0x86: case 0x96:
          for (int i = 0; i < 4; ++i) set_u(&want, 32, i, fix(__int128(sext(get_u(in.a, 16, 2 * i + 1), 16)) * sext(get_u(in.b, 16, 2 * i + 1), 16) + sext(get_u(in.c, 32, i), 32), 32));
          break;
        case 0x8E: case 0x9E:
          for (int i = 0; i < 4; ++i) set_u(&want, 32, i, fix(__int128(sext(get_u(in.a, 32, i), 32)) * sext(get_u(in.b, 32, i), 32) + sext(get_u(in.c, 32, i), 32), 32));
          break;
        case 0x87: case 0x97: case 0x8F: case 0x9F: {
          const int base = (op & 15) == 15 ? 1 : 0;
          for (int i = 0; i < 2; ++i) set_u(&want, 64, i, fix(__int128(sext(get_u(in.a, 32, 2 * i + base), 32)) * sext(get_u(in.b, 32, 2 * i + base), 32) + sext(get_u(in.c, 64, i), 64), 64));
          break;
        }
        default:
          for (int i = 0; i < 4; ++i) set_u(&want, 32, i, fix(__int128(sext(get_u(in.a, 16, 2 * i), 16)) * sext(get_u(in.b, 16, 2 * i), 16) + __int128(sext(get_u(in.a, 16, 2 * i + 1), 16)) * sext(get_u(in.b, 16, 2 * i + 1), 16) + sext(get_u(in.c, 32, i), 32), 32));
          break;
      }
      if (!same(want, got, 32)) printf("  mac op %02x\n", op);
      CHECK(same(want, got, 32));
    }
}

static void test_select() {
  for (int iter = 0; iter < 200; ++iter) {
    const Ops in = random_ops();
    const bool w = iter & 1, l = (iter >> 1) & 1, memory = (iter >> 2) & 1;
    // vpcmov xmm0, xmm1, xmm2/m, xmm3 (W0): (xmm1 & xmm3) | (xmm2 & ~xmm3); W1: xmm1, xmm3, xmm2/m with the selector in the r/m operand
    const V got = exec(xop(8, 0xA2, w, 1, l, memory, {0x30}), in, memory);
    const V& op3 = w ? in.c : in.b;
    const V& sel = w ? in.b : in.c;
    V want = zero_v();
    for (int i = 0; i < (l ? 32 : 16); ++i) want.b[i] = uint8_t((in.a.b[i] & sel.b[i]) | (op3.b[i] & ~sel.b[i]));
    CHECK(same(want, got, 32));
  }
  for (int iter = 0; iter < 300; ++iter) {
    Ops in = random_ops();
    const bool w = iter & 1, memory = (iter >> 1) & 1;
    const V got = exec(xop(8, 0xA3, w, 1, false, memory, {0x30}), in, memory);
    const V& op3 = w ? in.c : in.b;
    const V& sel = w ? in.b : in.c;
    V want = zero_v();
    for (int i = 0; i < 16; ++i) {
      const uint8_t s = sel.b[i], idx = s & 15;
      const uint8_t byte = (s & 16) ? op3.b[idx] : in.a.b[idx];
      uint8_t rev = 0;
      for (int k = 0; k < 8; ++k) rev = uint8_t((rev << 1) | ((byte >> k) & 1));
      uint8_t r = 0;
      switch (s >> 5) {
        case 0: r = byte; break;
        case 1: r = uint8_t(~byte); break;
        case 2: r = rev; break;
        case 3: r = uint8_t(~rev); break;
        case 4: r = 0; break;
        case 5: r = 0xFF; break;
        case 6: r = (byte & 0x80) ? 0xFF : 0; break;
        default: r = (byte & 0x80) ? 0 : 0xFF; break;
      }
      want.b[i] = r;
    }
    CHECK(same(want, got, 32));
  }
}

static uint64_t rot(uint64_t v, int n, int w) {
  n = ((n % w) + w) % w;
  const uint64_t mask = w == 64 ? ~0ull : (1ull << w) - 1;
  return n == 0 ? v : ((v << n) | (v >> (w - n))) & mask;
}

static void test_shifts() {
  for (int kind = 0; kind < 3; ++kind)      // rotate, logical shift, arithmetic shift
    for (int size = 0; size < 4; ++size)
      for (int iter = 0; iter < 60; ++iter) {
        Ops in = random_ops();
        const int w = 8 << size;
        const bool wflag = iter & 1, memory = (iter >> 1) & 1;
        const uint8_t op = uint8_t(0x90 + 4 * kind + size);
        // W0: dest = op(xmm2/m, count xmm1); W1: dest = op(xmm1, count xmm2/m)
        const V got = exec(xop(9, op, wflag, 1, false, memory), in, memory);
        const V& data = wflag ? in.a : in.b;
        const V& count = wflag ? in.b : in.a;
        V want = zero_v();
        for (int i = 0; i < 128 / w; ++i) {
          const uint64_t x = get_u(data, w, i);
          const int c = int8_t(get_u(count, w, i) & 0xFF);
          const int n = (c < 0 ? -c : c) & (w - 1);
          uint64_t r;
          if (kind == 0) r = rot(x, c >= 0 ? n : -n, w);
          else if (c >= 0) r = wrap(__int128(x) << n, w);
          else if (kind == 1) r = x >> n;
          else r = wrap(sext(x, w) >> n, w);
          set_u(&want, w, i, r);
        }
        CHECK(same(want, got, 32));
      }
  for (int size = 0; size < 4; ++size)      // rotate by an immediate
    for (int iter = 0; iter < 40; ++iter) {
      Ops in = random_ops();
      const int w = 8 << size;
      const uint8_t imm = uint8_t(rnd());
      const bool memory = iter & 1;
      const V got = exec(xop(8, uint8_t(0xC0 + size), false, 0, false, memory, {imm}), in, memory);
      V want = zero_v();
      for (int i = 0; i < 128 / w; ++i) set_u(&want, w, i, rot(get_u(in.b, w, i), int8_t(imm), w));
      CHECK(same(want, got, 32));
    }
}

static void test_compare() {
  for (int is_unsigned = 0; is_unsigned < 2; ++is_unsigned)
    for (int size = 0; size < 4; ++size)
      for (int cond = 0; cond < 8; ++cond) {
        Ops in = random_ops();
        const int w = 8 << size;
        for (int i = 0; i < 128 / w; ++i) if (rnd() % 3 == 0) set_u(&in.b, w, i, get_u(in.a, w, i));      // (equal elements)
        const bool memory = cond & 1;
        const uint8_t op = uint8_t((is_unsigned ? 0xEC : 0xCC) + size);
        const V got = exec(xop(8, op, false, 1, false, memory, {uint8_t(cond)}), in, memory);
        V want = zero_v();
        for (int i = 0; i < 128 / w; ++i) {
          const uint64_t x = get_u(in.a, w, i), y = get_u(in.b, w, i);
          const bool lt = is_unsigned ? x < y : sext(x, w) < sext(y, w), eq = x == y;
          const bool r[8] = {lt, lt || eq, !lt && !eq, !lt, eq, !eq, false, true};
          set_u(&want, w, i, r[cond] ? (w == 64 ? ~0ull : (1ull << w) - 1) : 0);
        }
        CHECK(same(want, got, 32));
      }
  Machine m(xop(8, 0xCC, false, 1, false, false, {8}));      // condition 8 is reserved
  CHECK(m.undefined());
}

static void test_horizontal() {
  struct H { uint8_t op; int from, count; bool is_unsigned, subtract; };
  const H hs[] = {{0xC1, 8, 2, 0, 0}, {0xC2, 8, 4, 0, 0}, {0xC3, 8, 8, 0, 0}, {0xC6, 16, 2, 0, 0}, {0xC7, 16, 4, 0, 0}, {0xCB, 32, 2, 0, 0},
                  {0xD1, 8, 2, 1, 0}, {0xD2, 8, 4, 1, 0}, {0xD3, 8, 8, 1, 0}, {0xD6, 16, 2, 1, 0}, {0xD7, 16, 4, 1, 0}, {0xDB, 32, 2, 1, 0},
                  {0xE1, 8, 2, 0, 1}, {0xE2, 16, 2, 0, 1}, {0xE3, 32, 2, 0, 1}};
  for (const H& h : hs)
    for (int iter = 0; iter < 30; ++iter) {
      Ops in = random_ops();
      const bool memory = iter & 1;
      const V got = exec(xop(9, h.op, false, 0, false, memory), in, memory);
      V want = zero_v();
      const int to = h.from * h.count;
      for (int i = 0; i < 128 / to; ++i) {
        __int128 sum = 0;
        for (int k = 0; k < h.count; ++k) {
          const uint64_t raw = get_u(in.b, h.from, i * h.count + k);
          const __int128 v = h.is_unsigned ? __int128(raw) : __int128(sext(raw, h.from));
          sum += (h.subtract && k == 1) ? -v : v;
        }
        set_u(&want, to, i, wrap(sum, to));
      }
      CHECK(same(want, got, 32));
    }
}

static void test_frcz() {
  for (int kind = 0; kind < 4; ++kind)      // ps, pd, ss, sd
    for (int l = 0; l < 2; ++l)
      for (int iter = 0; iter < 40; ++iter) {
        if (l && kind >= 2) continue;
        Ops in = random_ops();
        const bool dbl = kind & 1, scalar = kind >= 2, memory = iter & 1;
        for (int i = 0; i < 8; ++i) {
          if (dbl) put<double>(&in.b, i % 4, (rnd() % 5 == 0) ? double(int(rnd() % 100) - 50) : rand_double() * 3.7);
          else put<float>(&in.b, i, (rnd() % 5 == 0) ? float(int(rnd() % 100) - 50) : rand_float() * 3.7f);
        }
        const V got = exec(xop(9, uint8_t(0x80 + kind), false, 0, l, memory), in, memory);
        V want = zero_v();
        const int count = (scalar ? 1 : (dbl ? 2 : 4)) * (l ? 2 : 1);
        for (int i = 0; i < count; ++i) {
          if (dbl) { double ip; double f = std::modf(lane<double>(in.b, i), &ip); put<double>(&want, i, f); }
          else { float ip; float f = std::modf(lane<float>(in.b, i), &ip); put<float>(&want, i, f); }
        }
        if (!same(want, got, 32)) printf("  frcz kind %d l %d\n", kind, l);
        CHECK(same(want, got, 32));
      }
  {      // infinity is invalid: the default NaN and the invalid flag
    Ops in = random_ops();
    put<float>(&in.b, 0, INFINITY);
    Machine m(xop(9, 0x82, false, 0, false, false));
    set_ymm(m.cpu.state(), 2, in.b);
    CHECK(m.finished());
    CHECK(lane<uint32_t>(get_ymm(m.cpu.state(), 0), 0) == 0xFFC00000u);
    CHECK(m.cpu.state().mxcsr & 1);
  }
}

// --- TBM -------------------------------------------------------------------------------------------------------------------------------

static void test_tbm() {
  for (int iter = 0; iter < 400; ++iter) {
    uint64_t x = rnd();
    switch (rnd() % 6) { case 0: x = 0; break; case 1: x = ~0ull; break; case 2: x &= 0xFF; break; case 3: x = ~(rnd() & 0xFF); break; default: break; }
    const bool wide = rnd() & 1;
    const uint64_t mask = wide ? ~0ull : 0xFFFFFFFFull;
    x &= mask;
    struct T { uint8_t map, op; int reg; bool carry_on_ones; uint64_t (*f)(uint64_t); };
    static const T ts[] = {
        {9, 1, 1, true, [](uint64_t v) { return v & (v + 1); }},   {9, 1, 2, false, [](uint64_t v) { return v | (v - 1); }},
        {9, 1, 3, true, [](uint64_t v) { return v | (v + 1); }},   {9, 1, 4, false, [](uint64_t v) { return ~v & (v - 1); }},
        {9, 1, 5, true, [](uint64_t v) { return ~v & (v + 1); }},  {9, 1, 6, false, [](uint64_t v) { return ~v | (v - 1); }},
        {9, 1, 7, true, [](uint64_t v) { return ~v | (v + 1); }},  {9, 2, 1, true, [](uint64_t v) { return v ^ (v + 1); }},
        {9, 2, 6, true, [](uint64_t v) { return v | ~(v + 1); }}};
    for (const T& t : ts) {
      // blcfill r(vvvv = rcx? use vvvv = 3 (rbx)), r/m = rax: 8F E9 W vvvv 0 ... modrm reg = /n, rm = rax
      Bytes code = {0x8F, uint8_t(0xE0 | t.map), uint8_t((wide ? 0x80 : 0) | ((~3 & 15) << 3)), t.op, uint8_t(0xC0 | (t.reg << 3) | 0), 0x0F, 0x0B};
      Machine m(code);
      m.cpu.state().r[0] = x;
      m.cpu.state().r[3] = 0xDEADBEEFDEADBEEFull;
      CHECK(m.finished());
      const uint64_t want = t.f(x) & mask;
      CHECK(m.cpu.state().r[3] == want);
      const uint64_t fl = m.cpu.state().rflags;
      const bool carry = t.carry_on_ones ? x == mask : x == 0;
      CHECK(((fl & 1) != 0) == carry);
      CHECK(((fl & 0x40) != 0) == (want == 0));
      CHECK(((fl & 0x80) != 0) == (((want >> (wide ? 63 : 31)) & 1) != 0));
    }
    {      // bextr r, r/m, imm32
      const uint32_t start = uint32_t(rnd() % (wide ? 70 : 38)), length = uint32_t(rnd() % (wide ? 70 : 38));
      const uint32_t imm = start | (length << 8);
      Bytes code = {0x8F, 0xEA, uint8_t((wide ? 0x80 : 0x00) | 0x78), 0x10, 0xD8, uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24), 0x0F, 0x0B};      // bextr rbx, rax, imm32
      Machine m(code);
      m.cpu.state().r[0] = x;
      CHECK(m.finished());
      const uint32_t bits = wide ? 64 : 32;
      uint64_t want = start >= bits ? 0 : x >> start;
      if (length < bits) want &= (1ull << length) - 1;
      CHECK(m.cpu.state().r[3] == (want & mask));
      CHECK(((m.cpu.state().rflags & 0x40) != 0) == ((want & mask) == 0));
    }
  }
}

// --- decoding --------------------------------------------------------------------------------------------------------------------------


// --- LWP -------------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kLwpcb = 0x21000, kRing = 0x21400;
static void put_u32(Machine& m, uint64_t at, uint32_t v) { m.poke(at, &v, 4); }
static void put_u64(Machine& m, uint64_t at, uint64_t v) { m.poke(at, &v, 8); }
static uint64_t get_mem(Machine& m, uint64_t at, int n = 8) {
  uint64_t v = 0;
  GuestMemory gm(m.space);
  uint8_t raw[8] = {};
  gm.read(at, ByteView(raw, n));
  memcpy(&v, raw, n);
  return v;
}
// A control block for the ring buffer at kRing: flags, size in bytes, interval and counter of the value sample event.
static void make_lwpcb(Machine& m, uint32_t flags, uint32_t size, int32_t interval = 0, int32_t counter = 0) {
  put_u32(m, kLwpcb + 0, flags);
  put_u32(m, kLwpcb + 4, size);
  put_u64(m, kLwpcb + 8, kRing);
  put_u32(m, kLwpcb + 88, uint32_t(interval) & 0x3FFFFFF);
  put_u32(m, kLwpcb + 92, uint32_t(counter) & 0x3FFFFFF);
}
static Bytes llwpcb(bool w = true) { return xop(9, 0x12, w, 0, false, false, {}, 0); }       // llwpcb rdx
static Bytes slwpcb(bool w = true) { return xop(9, 0x12, w, 0, false, false, {}, 1); }       // slwpcb rdx
static Bytes lwpval(bool w = true) { return xop(0xA, 0x12, w, 1, false, true, {0x78, 0x56, 0x34, 0x12}, 1); }      // lwpval rcx, [rdi], 0x12345678
static Bytes lwpins(bool w = true) { return xop(0xA, 0x12, w, 1, false, true, {0x78, 0x56, 0x34, 0x12}, 0); }      // lwpins rcx, [rdi], 0x12345678
static Bytes strip(Bytes b) { b.pop_back(); b.pop_back(); return b; }
static Bytes join(std::initializer_list<Bytes> parts) { Bytes out; for (const Bytes& p : parts) { Bytes s = strip(p); out.insert(out.end(), s.begin(), s.end()); } out.push_back(0x0F); out.push_back(0x0B); return out; }

static void test_lwp() {
  {      // a value sample, then slwpcb: the record, the head pointer and the control block address
    Bytes code = join({llwpcb(), lwpval(), slwpcb()});
    Machine m(code);
    make_lwpcb(m, 0x60000002, 1024);
    put_u32(m, kBase, 0xCAFEF00D);
    m.cpu.state().r[2] = kLwpcb;
    m.cpu.state().r[1] = 0x1122334455667788ull;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[2] == kLwpcb);      // slwpcb replaced rdx
    CHECK(get_mem(m, kLwpcb, 4) == 0x60000002);      // only the available features stay: LWPVAL, continuous mode, time stamps
    CHECK(get_mem(m, kLwpcb + 16, 4) == 32 && get_mem(m, kLwpcb + 24) == 0);
    CHECK(get_mem(m, kRing + 0, 1) == 1 && get_mem(m, kRing + 1, 1) == 0 && get_mem(m, kRing + 2, 2) == 0x5678);
    CHECK(get_mem(m, kRing + 4, 4) == 0xCAFEF00D);
    CHECK(get_mem(m, kRing + 8) == 0x10000 + llwpcb().size() - 2);      // the address of the lwpval
    CHECK(get_mem(m, kRing + 16) == 0x1122334455667788ull);
    CHECK(get_mem(m, kRing + 24) != 0);      // (PTSC was requested)
  }
  {      // a smaller operand: Data2 zero-extended, the 32-bit address
    Bytes code = join({llwpcb(false), lwpval(false), slwpcb(false)});
    Machine m(code);
    make_lwpcb(m, 0x2, 1024);
    m.cpu.state().r[2] = 0xFFFFFFFF00000000ull | kLwpcb;
    m.cpu.state().r[1] = 0x1122334455667788ull;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[2] == kLwpcb);
    CHECK(get_mem(m, kRing + 16) == 0x55667788ull);
    CHECK(get_mem(m, kRing + 24) == 0);      // no time stamp without PTSC
  }
  {      // the event interval: every third value sample is stored
    Bytes code = join({llwpcb(), lwpval(), lwpval(), lwpval(), lwpval(), lwpval(), lwpval(), slwpcb()});
    Machine m(code);
    make_lwpcb(m, 0x2, 1024, 2);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(get_mem(m, kLwpcb + 16, 4) == 64);      // two records: the first and the fourth
    CHECK(get_mem(m, kRing + 8) != get_mem(m, kRing + 40));
    CHECK(get_mem(m, kRing + 40) == get_mem(m, kRing + 8) + 3 * lwpval().size() - 6);
    CHECK(get_mem(m, kLwpcb + 92, 4) == 0);      // the counter: reset to 2 by the fourth, then counted down by two samples
  }
  {      // lwpins in synchronized mode: 31 records fit, the 32nd and later ones overwrite the last record, set CF and count as missed
    Bytes code;
    std::vector<Bytes> parts = {llwpcb()};
    for (int i = 0; i < 31; ++i) parts.push_back(lwpins());
    parts.push_back(Bytes{0x0F, 0x92, 0xC3, 0x0F, 0x0B});      // setc bl
    parts.push_back(lwpins());
    parts.push_back(Bytes{0x41, 0x0F, 0x92, 0xC0, 0x0F, 0x0B});      // setc r8b
    parts.push_back(lwpins());
    parts.push_back(slwpcb());
    for (const Bytes& p : parts) { Bytes s = strip(p); code.insert(code.end(), s.begin(), s.end()); }
    code.push_back(0x0F); code.push_back(0x0B);
    Machine m(code);
    make_lwpcb(m, 0x2, 1024);
    m.cpu.state().r[2] = kLwpcb;
    m.cpu.state().rflags |= 1;
    CHECK(m.finished());
    CHECK((m.cpu.state().r[3] & 0xFF) == 0);      // the 31st fit
    CHECK((m.cpu.state().r[8] & 0xFF) == 1);      // the 32nd did not
    CHECK(get_mem(m, kLwpcb + 16, 4) == 31 * 32 && get_mem(m, kLwpcb + 24) == 2);
    CHECK(get_mem(m, kRing + 30 * 32, 1) == 255);
  }
  {      // continuous mode wraps: the head goes round and MissedEvents counts the wrap
    std::vector<Bytes> parts = {llwpcb()};
    for (int i = 0; i < 40; ++i) parts.push_back(lwpins());
    parts.push_back(slwpcb());
    Bytes code;
    for (const Bytes& p : parts) { Bytes s = strip(p); code.insert(code.end(), s.begin(), s.end()); }
    code.push_back(0x0F); code.push_back(0x0B);
    Machine m(code);
    make_lwpcb(m, 0x20000002, 1024);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(get_mem(m, kLwpcb + 16, 4) == 8 * 32 && get_mem(m, kLwpcb + 24) == 1);
  }
  {      // the software tail pointer frees space
    Bytes code = join({llwpcb(), lwpins(), slwpcb()});
    Machine m(code);
    make_lwpcb(m, 0x2, 1024);
    put_u32(m, kLwpcb + 16, 30 * 32);      // head: 30 records in
    put_u32(m, kLwpcb + 64, 31 * 32);      // tail: one past
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(get_mem(m, kLwpcb + 24) == 1);      // the next head (31) is the tail: full
    CHECK(get_mem(m, kLwpcb + 16, 4) == 30 * 32);
  }
  {      // without llwpcb, and after llwpcb with zero: lwpins clears CF, lwpval does nothing, slwpcb gives zero
    Bytes code = join({lwpins(), lwpval(), slwpcb()});
    Machine m(code);
    m.cpu.state().rflags |= 1;
    m.cpu.state().r[2] = 5;
    m.cpu.state().r[7] = 0xDEAD0000;      // (the memory operand is never touched)
    CHECK(m.finished());
    CHECK(m.cpu.state().r[2] == 0 && !(m.cpu.state().rflags & 1));
    Bytes code2 = join({llwpcb(), lwpval(), Bytes{0x48, 0x31, 0xD2, 0x0F, 0x0B}, llwpcb(), lwpins(), slwpcb()});      // (xor rdx, rdx; llwpcb rdx)
    Machine n(code2);
    make_lwpcb(n, 0x2, 1024);
    put_u32(n, kBase, 7);
    n.cpu.state().r[2] = kLwpcb;
    CHECK(n.finished());
    CHECK(n.cpu.state().r[2] == 0);
    CHECK(get_mem(n, kLwpcb + 16, 4) == 32);      // only the value sample before the stop was stored (and flushed by the llwpcb that stopped it)
  }
  {      // the value sample that is not stored does not read its operand; the stored one does
    Bytes code = join({llwpcb(), lwpval()});
    Machine m(code);
    make_lwpcb(m, 0x2, 1024, 5, 3);
    m.cpu.state().r[2] = kLwpcb;
    m.cpu.state().r[7] = 0xDEAD0000;
    CHECK(m.finished());
    Machine n(code);
    make_lwpcb(n, 0x2, 1024, 5, 0);
    n.cpu.state().r[2] = kLwpcb;
    n.cpu.state().r[7] = 0xDEAD0000;
    CHECK(n.run() == StopReason::Fault && n.cpu.fault().vector == 14);
  }
  {      // an undersized ring buffer is #GP and leaves LWP off; negative intervals and counters become zero
    Bytes code = join({llwpcb()});
    Machine m(code);
    make_lwpcb(m, 0x2, 512);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 13);
    Bytes code2 = join({llwpcb(), slwpcb()});
    Machine n(code2);
    make_lwpcb(n, 0x2, 4096, -5, -9);
    n.cpu.state().r[2] = kLwpcb;
    CHECK(n.finished());
    CHECK(n.cpu.state().r[2] == kLwpcb);
    CHECK(get_mem(n, kLwpcb + 88, 4) == 0 && get_mem(n, kLwpcb + 92, 4) == 0);
  }
  {      // llwpcb with an unreadable control block is #PF and leaves LWP off
    Machine m(join({llwpcb(), slwpcb()}));
    m.cpu.state().r[2] = 0xDEAD0000;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14);
  }
  {      // encodings: a memory operand or a nonzero vvvv on llwpcb / slwpcb, L = 1, a reserved reg field
    Machine a(xop(9, 0x12, true, 0, false, true, {}, 0));
    CHECK(a.undefined());
    Machine b(xop(9, 0x12, true, 1, false, false, {}, 0));
    CHECK(b.undefined());
    Machine c(xop(9, 0x12, true, 0, true, false, {}, 0));
    CHECK(c.undefined());
    Machine d(xop(9, 0x12, true, 0, false, false, {}, 2));
    CHECK(d.undefined());
    Machine e(xop(0xA, 0x12, true, 1, false, true, {1, 2, 3, 4}, 2));
    CHECK(e.undefined());
    Machine f(xop(0xA, 0x12, true, 1, true, true, {1, 2, 3, 4}, 0));
    CHECK(f.undefined());
  }
  {      // XSAVE and XRSTOR carry the session (component 62): stop it, restore it, and slwpcb finds it
    const Bytes save = {0x0F, 0xAE, 0x27};      // xsave [rdi]
    const Bytes stop = {0x31, 0xD2};      // xor edx, edx
    const Bytes restore = {0x0F, 0xAE, 0x2F};      // xrstor [rdi]
    Bytes full = {0x48, 0x89, 0xD6};      // mov rsi, rdx  (remember the control block address)
    Bytes l = strip(llwpcb());
    full.insert(full.end(), l.begin(), l.end());
    full.insert(full.end(), {0xB8, 0, 0, 0, 0, 0xBA, 0, 0, 0, 0x40});
    full.insert(full.end(), save.begin(), save.end());
    full.insert(full.end(), stop.begin(), stop.end());
    Bytes stop_session = strip(llwpcb());
    full.insert(full.end(), stop_session.begin(), stop_session.end());      // llwpcb rdx = 0
    Bytes sl = strip(slwpcb());
    full.insert(full.end(), sl.begin(), sl.end());      // slwpcb rdx -> 0
    full.insert(full.end(), {0x49, 0x89, 0xD0});       // mov r8, rdx
    full.insert(full.end(), {0xB8, 0, 0, 0, 0, 0xBA, 0, 0, 0, 0x40});      // (the mask again)
    full.insert(full.end(), restore.begin(), restore.end());
    full.insert(full.end(), sl.begin(), sl.end());      // slwpcb rdx -> the control block again
    full.push_back(0x0F); full.push_back(0x0B);
    Machine m(full);
    make_lwpcb(m, 0x2, 1024);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[8] == 0);
    CHECK(m.cpu.state().r[2] == kLwpcb);
    // the xsave header says LWP was in use
    CHECK(get_mem(m, kBase + 512) == (uint64_t{1} << 62));
  }
  {      // xgetbv: XINUSE reports a running session; cpuid advertises LWP
    Machine m({0xB9, 1, 0, 0, 0, 0x0F, 0x01, 0xD0, 0x0F, 0x0B});
    CHECK(m.finished());
    CHECK(m.cpu.state().r[0] == 0 && m.cpu.state().r[2] == 0);
    Machine n(join({llwpcb(), Bytes{0xB9, 1, 0, 0, 0, 0x0F, 0x01, 0xD0, 0x0F, 0x0B}}));
    make_lwpcb(n, 0x2, 1024);
    n.cpu.state().r[2] = kLwpcb;
    CHECK(n.finished());
    CHECK(n.cpu.state().r[2] == 0x40000000);
    Machine c({0xB8, 0x1C, 0, 0, 0x80, 0x0F, 0xA2, 0x0F, 0x0B});
    CHECK(c.finished());
    CHECK((c.cpu.state().r[0] & 3) == 3 && (c.cpu.state().r[2] & 3) == 3);      // available, LWPVAL
    CHECK(((c.cpu.state().r[3] >> 8) & 0xFF) == 32 && ((c.cpu.state().r[3] >> 24) & 0xFF) == 88);
  }
}


// the LWPCB bytes of the hardware events: interval at 88 + 8 * (id - 1), counter 4 above it
static void set_event(Machine& m, uint32_t id, int32_t interval, int32_t counter) {
  put_u32(m, kLwpcb + 88 + 8 * (id - 1), uint32_t(interval) & 0x3FFFFFF);
  put_u32(m, kLwpcb + 92 + 8 * (id - 1), uint32_t(counter) & 0x3FFFFFF);
}
static Bytes lwp_run(const Bytes& body) {      // llwpcb rdx; body; slwpcb rdx
  Bytes code;
  for (const Bytes& p : {llwpcb(), Bytes{0x0F, 0x0B}}) (void)p;
  Bytes a = strip(llwpcb()), c = strip(slwpcb());
  code.insert(code.end(), a.begin(), a.end());
  code.insert(code.end(), body.begin(), body.end());
  code.insert(code.end(), c.begin(), c.end());
  code.push_back(0x0F); code.push_back(0x0B);
  return code;
}
static uint64_t record_field(Machine& m, int index, int offset, int n = 8) { return get_mem(m, kRing + 32 * index + offset, n); }

static void test_lwp_hardware_events() {
  {      // flags: the available events stay, the others are cleared
    Machine m(join({llwpcb()}));
    make_lwpcb(m, 0xFFFFFFFF, 1024);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(get_mem(m, kLwpcb, 4) == 0xE000006E);
  }
  {      // instructions retired: every fourth instruction (interval 3), starting with the llwpcb that starts it
    Machine m(lwp_run(Bytes(8, 0x90)));
    make_lwpcb(m, 0x4, 1024);
    set_event(m, 2, 3, 0);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(get_mem(m, kLwpcb + 16, 4) == 3 * 32);
    for (int i = 0; i < 3; ++i) CHECK(record_field(m, i, 0, 1) == 2);
    CHECK(record_field(m, 0, 8) == 0x10000 && record_field(m, 1, 8) == 0x10008 && record_field(m, 2, 8) == 0x1000C);
  }
  {      // the clock events: records for 5 and 6 alternate
    Machine m(lwp_run(Bytes(6, 0x90)));
    make_lwpcb(m, 0x60, 1024);
    set_event(m, 5, 3, 0);
    set_event(m, 6, 3, 0);
    m.cpu.state().r[2] = kLwpcb;
    CHECK(m.finished());
    CHECK(record_field(m, 0, 0, 1) == 5 && record_field(m, 1, 0, 1) == 6 && record_field(m, 2, 0, 1) == 5 && record_field(m, 3, 0, 1) == 6);
    CHECK(record_field(m, 2, 8) == 0x10008);
  }
  {      // the IP range filter: only 0x10008..0x1000C are eligible (inclusive), or everything else with IPI
    for (int invert = 0; invert < 2; ++invert) {
      Machine m(lwp_run(Bytes(8, 0x90)));
      make_lwpcb(m, 0x4, 1024);
      set_event(m, 2, 1, 0);
      put_u32(m, kLwpcb + 36, 0x80000000u | (invert ? 0x40000000u : 0));
      put_u64(m, kLwpcb + 40, 0x10008);
      put_u64(m, kLwpcb + 48, 0x1000C);
      m.cpu.state().r[2] = kLwpcb;
      CHECK(m.finished());
      if (!invert) {
        CHECK(get_mem(m, kLwpcb + 16, 4) == 3 * 32);
        CHECK(record_field(m, 0, 8) == 0x10008 && record_field(m, 1, 8) == 0x1000A && record_field(m, 2, 8) == 0x1000C);
      } else {
        CHECK(record_field(m, 0, 8) == 0x10000);      // the llwpcb is outside the range
      }
    }
  }
  {      // branches: taken and untaken conditional, relative jump, call, return, jump through a register
    const Bytes body = {0x31, 0xC0,                                  // 0x10005 xor eax, eax
                        0x74, 0x02, 0x90, 0x90,                      // 0x10007 jz +2 (taken)
                        0x75, 0x02, 0x90, 0x90,                      // 0x1000B jnz +2 (not taken)
                        0xE8, 0x02, 0x00, 0x00, 0x00,                // 0x1000F call +2 (to the ret)
                        0xEB, 0x01,                                  // 0x10014 jmp +1 (after the return)
                        0xC3,                                        // 0x10016 ret
                        0x48, 0x8D, 0x05, 0x02, 0x00, 0x00, 0x00,    // 0x10017 lea rax, [rip + 2]
                        0xFF, 0xE0};                                 // 0x1001E jmp rax
    // branch order: jz (1), jnz (2), call (3), ret (4), jmp (5), jmp rax (6)
    struct Case { uint32_t filters; int32_t counter; std::vector<std::pair<uint64_t, uint64_t>> expected; std::vector<uint32_t> tkn; };
    const Case cases[] = {
        {0, 0, {{0x10007, 0x1000B}, {0x1000F, 0x10016}, {0x10014, 0x10017}}, {1, 1, 1}},      // branches 1, 3, 5 (interval 1)
        {0x10000000u, 0, {{0x1000F, 0x10016}, {0x10014, 0x10017}}, {1, 1}},                    // NCB: the conditional ones are not counted
        {0x08000000u | 0x20000000u, 0, {{0x10007, 0x1000B}}, {1}},                           // NAB + NRB: only the two conditional branches count
        {0, 1, {{0x1000B, 0x1000D}}, {0}},                                                     // the counter starts at 1: the second branch, which is not taken, is stored first
    };
    for (const Case& c : cases) {
      Machine m(lwp_run(body));
      make_lwpcb(m, 0x8, 1024);
      set_event(m, 3, 1, c.counter);
      put_u32(m, kLwpcb + 36, c.filters);
      m.cpu.state().r[2] = kLwpcb;
      m.cpu.state().r[4] = 0x21F00;
      CHECK(m.finished());
      const size_t stored = get_mem(m, kLwpcb + 16, 4) / 32;
      CHECK(stored >= c.expected.size());
      for (size_t i = 0; i < c.expected.size() && i < stored; ++i) {
        CHECK(record_field(m, int(i), 0, 1) == 3);
        CHECK(record_field(m, int(i), 8) == c.expected[i].first);
        CHECK(record_field(m, int(i), 16) == c.expected[i].second);
        CHECK(((record_field(m, int(i), 2, 2) >> 15) & 1) == c.tkn[i]);
      }
    }
  }
  {      // the threshold: two records reach 64 bytes, and only with INT set does the host see the interrupt flag
    for (int interrupts = 0; interrupts < 2; ++interrupts) {
      Machine m(lwp_run(Bytes(10, 0x90)));
      make_lwpcb(m, 0x4 | (interrupts ? 0x80000000u : 0), 1024);
      set_event(m, 2, 1, 0);
      put_u32(m, kLwpcb + 32, 64);
      m.cpu.state().r[2] = kLwpcb;
      CHECK(m.finished());
      CHECK(m.cpu.take_lwp_interrupt() == bool(interrupts));
      CHECK(!m.cpu.take_lwp_interrupt());
    }
  }
}

// --- vpermil2ps / vpermil2pd -------------------------------------------------------------------------------------------------------------

static Bytes vpermil2(bool dbl, bool w, bool l, bool memory, int m2z) {
  Bytes code = {0xC4, 0xE3, uint8_t((w ? 0x80 : 0) | ((~1 & 15) << 3) | (l ? 4 : 0) | 1), uint8_t(dbl ? 0x49 : 0x48)};
  code.push_back(memory ? 0x07 : 0xC2);
  code.push_back(uint8_t(0x30 | m2z));
  code.push_back(0x0F); code.push_back(0x0B);
  return code;
}

static void test_vpermil2() {
  {      // the documented example of _mm256_permute2_pd: src1 = 0..3, src2 = 4..7
    V a = zero_v(), sel = zero_v();
    for (int i = 0; i < 4; ++i) put<double>(&a, i, i);
    V b = zero_v();
    for (int i = 0; i < 4; ++i) put<double>(&b, i, i + 4);
    put<int64_t>(&sel, 0, 2 << 1); put<int64_t>(&sel, 1, (1 << 1) + 8); put<int64_t>(&sel, 2, 0 << 1); put<int64_t>(&sel, 3, (3 << 1) + 8);
    const double want[3][4] = {{4, 1, 2, 7}, {4, 0, 2, 0}, {0, 1, 0, 7}};
    const int m2z[3] = {0, 2, 3};
    for (int k = 0; k < 3; ++k) {
      Ops o = random_ops();
      o.a = a; o.c = b; o.b = sel;      // W1: src2 = the register in imm8 (xmm3), the selector = the r/m operand (xmm2)
      const V got = exec(vpermil2(true, true, true, false, m2z[k]), o, false);
      for (int i = 0; i < 4; ++i) CHECK(lane<double>(got, i) == want[k][i]);
    }
  }
  for (int dbl = 0; dbl < 2; ++dbl)
    for (int w = 0; w < 2; ++w)
      for (int l = 0; l < 2; ++l)
        for (int memory = 0; memory < 2; ++memory)
          for (int m2z = 0; m2z < 4; ++m2z) {
            Ops o = random_ops();
            // W0: src2 = r/m (b), selector = xmm3 (c); W1: src2 = xmm3 (c), selector = r/m (b)
            const V& src2 = w ? o.c : o.b;
            const V& sel = w ? o.b : o.c;
            const V got = exec(vpermil2(dbl, w, l, memory, m2z), o, memory);
            const int width = dbl ? 8 : 4, per_lane = 16 / width, lanes = l ? 2 : 1;
            V want = zero_v();
            for (int ln = 0; ln < lanes; ++ln)
              for (int i = 0; i < per_lane; ++i) {
                const int at = ln * per_lane + i;
                const uint64_t s = dbl ? lane<uint64_t>(sel, at) : lane<uint32_t>(sel, at);
                const int index = dbl ? int((s >> 1) & 1) : int(s & 3);
                const bool from2 = (s >> 2) & 1, match = (s >> 3) & 1;
                const bool zero = (m2z == 2 && match) || (m2z == 3 && !match);
                const V& src = from2 ? src2 : o.a;
                if (dbl) put<uint64_t>(&want, at, zero ? 0 : lane<uint64_t>(src, ln * per_lane + index));
                else put<uint32_t>(&want, at, zero ? 0 : lane<uint32_t>(src, ln * per_lane + index));
              }
            CHECK(same(got, want, 32));
          }
  CHECK(Machine(Bytes{0xC5, 0xF1, 0x48, 0xC2, 0x30, 0x0F, 0x0B}).undefined());      // (a VEX map 1 opcode that does not exist)
  {      // other prefixes than 66 are #UD
    Bytes code = vpermil2(false, false, false, false, 0);
    code[2] = uint8_t((code[2] & ~3) | 2);
    CHECK(Machine(code).undefined());
  }
}

static void test_decoding() {
  {      // 8F /0 is still pop
    Machine m({0x6A, 0x2A, 0x8F, 0xC3, 0x0F, 0x0B});      // push 42; pop rbx
    m.cpu.state().r[4] = 0x21000;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[3] == 42);
  }
  {      // XOP with a legacy prefix, with pp set, with a VEX map or a reserved map is #UD
    Machine a({0x66, 0x8F, 0xE9, 0x78, 0x90, 0xC2, 0x0F, 0x0B});
    CHECK(a.undefined());
    Machine b({0x8F, 0xE9, 0x79, 0x90, 0xC2, 0x0F, 0x0B});
    CHECK(b.undefined());
    Machine c({0x8F, 0xEB, 0x78, 0x90, 0xC2, 0x0F, 0x0B});
    CHECK(c.undefined());
    Machine d({0x8F, 0xE9, 0x78, 0xFF, 0xC2, 0x0F, 0x0B});      // an opcode that does not exist
    CHECK(d.undefined());
    Machine e(xop(8, 0x85, true, 1, false, false, {0x30}));      // XOP.W on a multiply-accumulate
    CHECK(e.undefined());
    Machine f(xop(8, 0x85, false, 1, true, false, {0x30}));      // XOP.L on one that has no 256-bit form
    CHECK(f.undefined());
  }
  {
    Machine m({0xB8, 0, 0, 0, 0x80, 0x0F, 0xA2, 0x0F, 0x0B});
    CHECK(m.finished());
    CHECK(m.cpu.state().r[0] >= 0x80000001u);
    Machine n({0xB8, 1, 0, 0, 0x80, 0x0F, 0xA2, 0x0F, 0x0B});
    CHECK(n.finished());
    const uint64_t ecx = n.cpu.state().r[1];
    CHECK(((ecx >> 11) & 1) && ((ecx >> 16) & 1) && ((ecx >> 21) & 1));      // XOP, FMA4, TBM
  }
}

// Every prefix of an encoding and every single-bit flip of it: whatever happens, it must happen safely.
static void test_malformed() {
  Bytes ok;
  for (const Bytes& part : {xop(8, 0xA3, false, 1, false, true, {0x30}), xop(8, 0x85, false, 1, false, false, {0x30}), xop(9, 0x80, false, 0, true, true), xop(9, 0x94, false, 1, false, false),
                            Bytes{0xC4, 0xE3, 0x71, 0x68, 0x07, 0x30, 0x0F, 0x0B}, Bytes{0xC4, 0xE3, 0x71, 0x49, 0x07, 0x32, 0x0F, 0x0B}, llwpcb(), lwpval(), slwpcb(), Bytes{0x8F, 0xE9, 0x78, 0x01, 0xCB, 0x0F, 0x0B}, Bytes{0x8F, 0xEA, 0x78, 0x10, 0xD8, 1, 2, 3, 4, 0x0F, 0x0B}})
    ok.insert(ok.end(), part.begin(), part.end() - 2);
  for (size_t cut = 0; cut < ok.size(); ++cut) {
    Bytes b(ok.begin(), ok.begin() + cut);
    Machine m(b, 0x11000 - cut);
    m.run(16);
  }
  for (size_t i = 0; i < ok.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes b = ok;
      b[i] ^= uint8_t(1 << bit);
      b.push_back(0x0F); b.push_back(0x0B);
      Machine m(b);
      m.run(64);
    }
}

int main() {
  test_fma4();
  test_mac();
  test_select();
  test_shifts();
  test_compare();
  test_horizontal();
  test_frcz();
  test_tbm();
  test_vpermil2();
  test_lwp();
  test_lwp_hardware_events();
  test_decoding();
  test_malformed();
  if (g_failed) { printf("XopTest: %d failed\n", g_failed); return 1; }
  printf("XopTest: all passed\n");
  return 0;
}
