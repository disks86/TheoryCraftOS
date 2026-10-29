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

// Host test of the AVX-512 FP16 emulation. No CPU the tests can run on has AVX512_FP16, so there is no hardware oracle: instead every result is checked against a model written
// here from the definition, independent of the emulator's soft-float code:
//   - add, subtract, multiply, divide, square root and fused multiply-add are computed exactly in 128-bit integers and rounded to half in every rounding mode, with the exception
//     flags (inexact, underflow before rounding, overflow, invalid, divide-by-zero);
//   - the rest (min/max, comparisons, conversions, rndscale, reduce, getexp, getmant, scalef, fpclass, moves, complex arithmetic) uses the host's _Float16 / float / double.
// Each instruction runs with random operand registers (0..31), vector lengths 128/256/512, merge and zero masking, memory operands with compressed disp8, embedded broadcast and
// embedded rounding, and every result register, the mask registers and MXCSR must match. The robustness cases run every truncation and bit flip of the encodings.
// Run with tests/run_host_tests.sh Fp16Test (ASan and UBSan). FP16_ONLY=<substring> selects instructions, FP16_ITER=<n> the cases per instruction.
#include <cmath>
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
typedef _Float16 H;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct A final : allocators::IAllocator {
  void* allocate(uint64_t s) override { return malloc(s); }
  void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); }
  void deallocate(void* p) override { free(p); }
};

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

// --- half helpers --------------------------------------------------------------------------------------------------------------------------

static H as_half(uint16_t b) { H h; memcpy(&h, &b, 2); return h; }
static uint16_t bits_of(H h) { uint16_t b; memcpy(&b, &h, 2); return b; }
static bool is_nan(uint16_t b) { return (b & 0x7C00) == 0x7C00 && (b & 0x3FF); }
static bool is_snan(uint16_t b) { return is_nan(b) && !(b & 0x200); }
static bool is_inf(uint16_t b) { return (b & 0x7FFF) == 0x7C00; }
static bool is_zero(uint16_t b) { return (b & 0x7FFF) == 0; }
static float f_of(uint16_t b) { return static_cast<float>(as_half(b)); }
constexpr uint16_t kDefaultNan = 0xFE00;
enum : uint32_t { kIE = 1, kDE = 2, kZE = 4, kOE = 8, kUE = 16, kPE = 32 };

static uint16_t rnd_half() {
  const uint64_t r = rnd();
  const uint16_t any = uint16_t(rnd());
  switch (r % 20) {
    case 0: case 1: case 2: case 3: case 4: case 5: return any;
    case 6: case 7: case 8: return uint16_t((any & 0x8000) | ((10 + rnd() % 10) << 10) | (rnd() & 0x3FF));
    case 9: case 10: {
      static const uint16_t sp[] = {0, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0x7D00, 0xFE00, 0xFD01, 1, 0x8001, 0x3FF, 0x83FF, 0x400, 0x7BFF, 0xFBFF, 0x3C00, 0xBC00, 0x4000, 0x3800};
      return sp[rnd() % (sizeof(sp) / sizeof(sp[0]))];
    }
    case 11: case 12: case 13: return bits_of(static_cast<H>(static_cast<float>(int32_t(rnd() % 81) - 40)));
    case 14: case 15: return uint16_t((any & 0x8000) | (rnd() & 0x3FF));
    case 16: return uint16_t((any & 0x8000) | ((rnd() % 31) << 10));
    default: return uint16_t((any & 0x8000) | ((12 + rnd() % 6) << 10) | (rnd() & 0x3FF));
  }
}

// --- the exact reference -----------------------------------------------------------------------------------------------------------------

struct Fin { bool neg; int64_t m; int e; };      // |value| = m * 2^e, m > 0
static bool unpack_finite(uint16_t b, Fin* f) {
  const int exp = (b >> 10) & 31, frac = b & 0x3FF;
  f->neg = b >> 15;
  if (exp == 31 || (exp == 0 && frac == 0)) return false;
  if (exp == 0) { f->m = frac; f->e = -24; } else { f->m = frac | 0x400; f->e = exp - 25; }
  return true;
}
static int bitlen(u128 v) { int n = 0; while (v) { ++n; v >>= 1; } return n; }

// Rounds (N + sticky fraction) * 2^E to half in `mode` (0 nearest even, 1 down, 2 up, 3 toward zero). N > 0.
static uint16_t round_half(bool neg, u128 N, int E, bool sticky, int mode, uint32_t* flags) {
  const int L = bitlen(N), q = E + L - 1;
  const int t = q - 10 > -24 ? q - 10 : -24;
  const int shift = t - E;
  u128 mag;
  bool inexact = sticky;
  if (shift <= 0) {
    mag = N << (-shift);
  } else {
    const bool far = shift > 120;      // N is far below the smallest denormal: it can only be (much) less than half of the last place
    u128 kept = far ? 0 : N >> shift;
    const u128 rem = far ? 1 : N & ((u128(1) << shift) - 1), halfv = far ? 2 : u128(1) << (shift - 1);
    inexact = rem != 0 || sticky;
    const bool above = rem > halfv || (rem == halfv && sticky), tie = rem == halfv && !sticky;
    bool up = false;
    switch (mode) {
      case 0: up = above || (tie && (kept & 1)); break;
      case 1: up = neg && inexact; break;
      case 2: up = !neg && inexact; break;
      default: break;
    }
    mag = kept + (up ? 1 : 0);
  }
  if (inexact) { *flags |= kPE; if (q < -14) *flags |= kUE; }
  const uint16_t sign = neg ? 0x8000 : 0;
  if (mag == 0) return sign;
  const int bl = bitlen(mag);
  if (t + bl - 1 >= 16) {      // overflow
    *flags |= kOE | kPE;
    const bool to_inf = mode == 0 || (mode == 1 && neg) || (mode == 2 && !neg);
    return sign | (to_inf ? 0x7C00 : 0x7BFF);
  }
  if (t == -24 && bl <= 10) return sign | uint16_t(mag);
  const int unb = t + bl - 1;
  const uint64_t frac = uint64_t(bl >= 11 ? mag >> (bl - 11) : mag << (11 - bl)) & 0x3FF;
  return sign | uint16_t(((unb + 15) << 10) | frac);
}

static uint16_t quiet(uint16_t b) { return b | 0x200; }
static uint16_t nan_of(uint16_t a, uint16_t b, uint32_t* fl) {      // the first NaN operand, quieted
  if (is_snan(a) || is_snan(b)) *fl |= kIE;
  return quiet(is_nan(a) ? a : b);
}
static uint16_t exact_add(uint16_t a, uint16_t b, int mode, uint32_t* fl) {
  if (is_nan(a) || is_nan(b)) return nan_of(a, b, fl);
  if (is_inf(a) || is_inf(b)) {
    if (is_inf(a) && is_inf(b) && ((a ^ b) & 0x8000)) { *fl |= kIE; return kDefaultNan; }
    return is_inf(a) ? a : b;
  }
  Fin x, y;
  const bool hx = unpack_finite(a, &x), hy = unpack_finite(b, &y);
  if (!hx && !hy) return (a & b & 0x8000) ? 0x8000 : (mode == 1 && ((a | b) & 0x8000) ? 0x8000 : 0);
  if (!hx) return b;
  if (!hy) return a;
  const int e = x.e < y.e ? x.e : y.e;
  __int128 sx = __int128(x.m) << (x.e - e), sy = __int128(y.m) << (y.e - e);
  if (x.neg) sx = -sx;
  if (y.neg) sy = -sy;
  const __int128 s = sx + sy;
  if (s == 0) return mode == 1 ? 0x8000 : 0;
  return round_half(s < 0, u128(s < 0 ? -s : s), e, false, mode, fl);
}
static uint16_t exact_mul(uint16_t a, uint16_t b, int mode, uint32_t* fl) {
  if (is_nan(a) || is_nan(b)) return nan_of(a, b, fl);
  const uint16_t sign = (a ^ b) & 0x8000;
  if (is_inf(a) || is_inf(b)) {
    if (is_zero(a) || is_zero(b)) { *fl |= kIE; return kDefaultNan; }
    return sign | 0x7C00;
  }
  Fin x, y;
  if (!unpack_finite(a, &x) || !unpack_finite(b, &y)) return sign;
  return round_half(sign != 0, u128(x.m) * u128(y.m), x.e + y.e, false, mode, fl);
}
static uint16_t exact_div(uint16_t a, uint16_t b, int mode, uint32_t* fl) {
  if (is_nan(a) || is_nan(b)) return nan_of(a, b, fl);
  const uint16_t sign = (a ^ b) & 0x8000;
  if (is_inf(a)) { if (is_inf(b)) { *fl |= kIE; return kDefaultNan; } return sign | 0x7C00; }
  if (is_inf(b)) return sign;
  if (is_zero(b)) { if (is_zero(a)) { *fl |= kIE; return kDefaultNan; } *fl |= kZE; return sign | 0x7C00; }
  if (is_zero(a)) return sign;
  Fin x, y;
  unpack_finite(a, &x);
  unpack_finite(b, &y);
  const int K = 40;
  const u128 num = u128(x.m) << K, den = u128(y.m);
  return round_half(sign != 0, num / den, x.e - y.e - K, (num % den) != 0, mode, fl);
}
static uint16_t exact_sqrt(uint16_t a, int mode, uint32_t* fl) {
  if (is_nan(a)) { if (is_snan(a)) *fl |= kIE; return quiet(a); }
  if (is_zero(a)) return a;
  if (a & 0x8000) { *fl |= kIE; return kDefaultNan; }
  if (is_inf(a)) return a;
  Fin x;
  unpack_finite(a, &x);
  u128 m = x.m;
  int e = x.e;
  if (e & 1) { m <<= 1; e -= 1; }
  const int S = 30;
  m <<= 2 * S;
  u128 r = 0;
  for (int bit = 62; bit >= 0; --bit) {      // integer square root
    const u128 c = r | (u128(1) << bit);
    if (c * c <= m) r = c;
  }
  return round_half(false, r, e / 2 - S, r * r != m, mode, fl);
}
// a*b+c with one rounding
static uint16_t exact_fma(uint16_t a, uint16_t b, uint16_t c, int mode, uint32_t* fl) {
  if (is_nan(a) || is_nan(b) || is_nan(c)) {
    if (is_snan(a) || is_snan(b) || is_snan(c)) *fl |= kIE;
    return quiet(is_nan(a) ? a : is_nan(b) ? b : c);
  }
  if (is_inf(a) || is_inf(b) || is_inf(c)) {
    const bool prod_inf = is_inf(a) || is_inf(b);
    if (prod_inf && (is_zero(a) || is_zero(b))) { *fl |= kIE; return kDefaultNan; }
    const uint16_t psign = (a ^ b) & 0x8000;
    if (prod_inf && is_inf(c) && psign != (c & 0x8000)) { *fl |= kIE; return kDefaultNan; }
    return prod_inf ? (psign | 0x7C00) : c;
  }
  Fin x, y, z;
  const bool prod_zero = !unpack_finite(a, &x) || !unpack_finite(b, &y);
  const bool hz = unpack_finite(c, &z);
  const bool psign = ((a ^ b) & 0x8000) != 0;
  if (prod_zero) {
    if (!hz) return (psign && (c & 0x8000)) ? 0x8000 : (mode == 1 && (psign || (c & 0x8000)) ? 0x8000 : 0);
    return c;
  }
  const int pe = x.e + y.e;
  const int e = pe < z.e || !hz ? pe : z.e;
  __int128 sp = __int128(u128(x.m) * u128(y.m)) << (pe - e);
  if (psign) sp = -sp;
  __int128 sz = hz ? (__int128(z.m) << (z.e - e)) : 0;
  if (hz && z.neg) sz = -sz;
  const __int128 s = sp + sz;
  if (s == 0) return mode == 1 ? 0x8000 : 0;
  return round_half(s < 0, u128(s < 0 ? -s : s), e, false, mode, fl);
}
static uint16_t neg16(uint16_t x) { return x ^ 0x8000; }

// --- the machine ---------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kBase = 0x20800;      // rdi
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x30000;
    space.map(0x2F000, 0x2000, process::kProtRead | process::kProtWrite);
    cpu.reset(start);
    cpu.state().r[7] = kBase;
  }
  StopReason run(uint64_t n = 4) { GuestMemory m(space); return cpu.run(m, n); }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  void peek(uint64_t addr, void* p, size_t n) { GuestMemory m(space); m.read(addr, ByteView(static_cast<uint8_t*>(p), n)); }
};

struct Zmm { uint16_t h[32]; };
static void set_reg(X86_64Cpu::State& st, int reg, const Zmm& z) {
  uint64_t q[8];
  memcpy(q, z.h, 64);
  X86_64Cpu::Xmm& lo = reg < 16 ? st.xmm[reg] : st.evex_lo[reg - 16];
  X86_64Cpu::YmmHigh& hi = reg < 16 ? st.ymm_high[reg] : st.evex_hi[reg - 16];
  lo.lo = q[0]; lo.hi = q[1]; hi.lo = q[2]; hi.hi = q[3]; hi.upper[0].lo = q[4]; hi.upper[0].hi = q[5]; hi.upper[1].lo = q[6]; hi.upper[1].hi = q[7];
}
static Zmm get_reg(const X86_64Cpu::State& st, int reg) {
  const X86_64Cpu::Xmm& lo = reg < 16 ? st.xmm[reg] : st.evex_lo[reg - 16];
  const X86_64Cpu::YmmHigh& hi = reg < 16 ? st.ymm_high[reg] : st.evex_hi[reg - 16];
  const uint64_t q[8] = {lo.lo, lo.hi, hi.lo, hi.hi, hi.upper[0].lo, hi.upper[0].hi, hi.upper[1].lo, hi.upper[1].hi};
  Zmm z;
  memcpy(z.h, q, 64);
  return z;
}

// An EVEX instruction. `rm` is a register, or (memory) [rdi + disp] with `disp8` > 0 selecting the compressed one-byte form (the byte is disp / n, n given by the caller).
struct Enc {
  uint8_t map = 5, pp = 0, op = 0;
  bool w = false;
  int vl = 512;           // 128, 256, 512
  int reg = 0, vvvv = 0, rm = 0;
  bool memory = false;
  int32_t disp = 0;
  int disp8_factor = 0;   // N of the compressed displacement (0: a 32-bit displacement)
  int aaa = 0;
  bool z = false, bcst = false;
  int rc = -1;            // >= 0: embedded rounding / SAE (register operands only)
  int imm = -1;
};
static Bytes encode(const Enc& e) {
  Bytes b = {0x62};
  const int rm_hi = e.memory ? 0 : (e.rm >> 3) & 1, rm_x = e.memory ? 0 : (e.rm >> 4) & 1;
  b.push_back(uint8_t((((e.reg >> 3) & 1) ? 0 : 0x80) | (rm_x ? 0 : 0x40) | (rm_hi ? 0 : 0x20) | (((e.reg >> 4) & 1) ? 0 : 0x10) | e.map));
  b.push_back(uint8_t((e.w ? 0x80 : 0) | ((~e.vvvv & 15) << 3) | 4 | e.pp));
  const int ll = e.rc >= 0 ? e.rc : e.vl == 128 ? 0 : e.vl == 256 ? 1 : 2;
  b.push_back(uint8_t((e.z ? 0x80 : 0) | (ll << 5) | ((e.bcst || e.rc >= 0) ? 0x10 : 0) | (((e.vvvv >> 4) & 1) ? 0 : 8) | e.aaa));
  b.push_back(e.op);
  if (!e.memory) {
    b.push_back(uint8_t(0xC0 | ((e.reg & 7) << 3) | (e.rm & 7)));
  } else if (e.disp8_factor) {
    b.push_back(uint8_t(0x40 | ((e.reg & 7) << 3) | 7));
    b.push_back(uint8_t(e.disp / e.disp8_factor));
  } else {
    b.push_back(uint8_t(0x80 | ((e.reg & 7) << 3) | 7));
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(e.disp) >> (8 * i)));
  }
  if (e.imm >= 0) b.push_back(uint8_t(e.imm));
  return b;
}
static Bytes with_syscall(Bytes b) { b.push_back(0x0F); b.push_back(0x05); return b; }

// --- the generic element-wise driver ---------------------------------------------------------------------------------------------------------

struct Ref {      // the model of one element: a = vvvv operand, b = r/m operand, d = the old destination element
  std::function<uint16_t(uint16_t a, uint16_t b, uint16_t d, int mode, int imm, uint32_t* fl)> fn;
};
struct Op {
  std::string name;
  uint8_t map, pp, opcode;
  bool w;
  bool scalar = false;
  bool unary = false;      // the vvvv field is unused (1111b)
  bool imm = false;
  bool er = true;          // supports embedded rounding or SAE
  bool dest_is_input = false;
  bool sae_only = false;   // the embedded-rounding form only suppresses exceptions
  bool check_flags = true;
  Ref ref;
};

static uint32_t flag_bits(uint32_t mxcsr) { return mxcsr & 0x3F; }
static int g_reports = 0;
static int g_mode = 0;
static void report(const std::string& name, const char* what, int idx, uint16_t got, uint16_t want, uint16_t a, uint16_t b, const Enc& e, int aaa, bool z) {
  if (g_reports++ < 40)
    printf("MISMATCH %s %s elem %d got %04x want %04x (a=%04x b=%04x vl=%d aaa=%d z=%d mem=%d bcst=%d rc=%d imm=%d mode=%d)\n", name.c_str(), what, idx, got, want, a, b, e.vl, aaa, z, e.memory, e.bcst, e.rc, e.imm, g_mode);
  ++g_failed;
}

static void run_elementwise(const Op& op, int iterations) {
  for (int it = 0; it < iterations; ++it) {
    Enc e;
    e.map = op.map; e.pp = op.pp; e.op = op.opcode; e.w = op.w;
    e.vl = op.scalar ? 128 : (rnd() % 3 == 0 ? 128 : rnd() % 2 ? 256 : 512);
    e.reg = int(rnd() % 32);
    e.vvvv = op.unary ? 0 : int(rnd() % 32);
    e.aaa = rnd() % 3 == 0 ? 0 : int(rnd() % 8);
    e.z = e.aaa != 0 && (rnd() & 1);
    const int kind = int(rnd() % 4);      // 0 register, 1 memory, 2 broadcast, 3 embedded rounding
    int mode_override = -1;
    if (kind == 3 && op.er) {
      e.rc = int(rnd() % 4);
      if (!op.sae_only) mode_override = e.rc;
      e.vl = 512;
      e.memory = false;
      e.rm = int(rnd() % 32);
    } else if (kind == 1 || (kind == 2 && op.scalar)) {
      e.memory = true;
    } else if (kind == 2) {
      e.memory = true;
      e.bcst = true;
    } else {
      e.rm = int(rnd() % 32);
    }
    if (op.imm) e.imm = int(rnd() % 256);
    const int n = op.scalar ? 1 : e.vl / 16;
    const int vbytes = op.scalar ? 2 : e.bcst ? 2 : e.vl / 8;
    if (e.memory) {
      e.disp = (rnd() & 1) ? int(rnd() % 17) * vbytes - 8 * vbytes : int(rnd() % 400) - 200;
      if (rnd() % 3 != 0) { e.disp8_factor = vbytes; e.disp = int(int(rnd() % 17) - 8) * vbytes; }
    }
    Machine m(with_syscall(encode(e)));
    X86_64Cpu::State& st = m.cpu.state();
    Zmm regs[32];
    for (int r = 0; r < 32; ++r) {
      for (int i = 0; i < 32; ++i) regs[r].h[i] = rnd_half();
      set_reg(st, r, regs[r]);
    }
    uint16_t mem_h[32];
    for (int i = 0; i < 32; ++i) mem_h[i] = rnd_half();
    if (e.memory) m.poke(kBase + e.disp, mem_h, e.bcst ? 2 : vbytes);
    const uint64_t kmask = rnd() % 4 == 0 ? ~0ull : rnd();
    for (int k = 0; k < 8; ++k) st.k[k] = k == e.aaa ? kmask : rnd();
    int mode = (rnd() % 4);
    st.mxcsr = 0x1F80 | uint32_t(mode) << 13;
    if (mode_override >= 0) mode = mode_override;
    g_mode = mode;
    const Zmm a = regs[e.vvvv], d = regs[e.reg];
    Zmm b;
    if (e.memory) { for (int i = 0; i < 32; ++i) b.h[i] = e.bcst ? mem_h[0] : mem_h[i]; } else { b = regs[e.rm]; }
    if (m.run() != StopReason::Syscall) { report(op.name, "did not run to the syscall", 0, 0, 0, 0, 0, e, e.aaa, e.z); return; }
    const Zmm got = get_reg(st, e.reg);
    Zmm want;
    memset(&want, 0, sizeof want);
    uint32_t flags = 0;
    if (op.scalar) for (int i = 1; i < 8; ++i) want.h[i] = op.dest_is_input ? d.h[i] : a.h[i];
    for (int i = 0; i < n; ++i) {
      const bool active = e.aaa == 0 || ((kmask >> i) & 1);
      if (!active) { want.h[i] = e.z ? 0 : d.h[i]; continue; }
      uint32_t fl = 0;
      want.h[i] = op.ref.fn(a.h[i], b.h[i], d.h[i], mode, e.imm, &fl);
      flags |= fl;
    }
    for (int i = 0; i < 32; ++i) {
      const bool nan_ok = is_nan(want.h[i]) && is_nan(got.h[i]) && (want.h[i] & 0x8000) == (got.h[i] & 0x8000) && (want.h[i] & 0x200) == (got.h[i] & 0x200);
      const bool payload_free = op.name.find("getexp") != std::string::npos || op.name.find("scalef") != std::string::npos || op.name.find("vfm") != std::string::npos || op.name.find("vfnm") != std::string::npos;
      if (got.h[i] != want.h[i] && !(payload_free && nan_ok)) { report(op.name, "dest", i, got.h[i], want.h[i], i < n ? a.h[i] : 0, i < n ? b.h[i] : 0, e, e.aaa, e.z); break; }
    }
    for (int r = 0; r < 32; ++r) {
      if (r == e.reg) continue;
      const Zmm now = get_reg(st, r);
      if (memcmp(now.h, regs[r].h, 64) != 0) { report(op.name, "clobbered register", r, 0, 0, 0, 0, e, e.aaa, e.z); break; }
    }
    uint32_t want_flags = e.rc >= 0 ? 0 : flags;
    want_flags &= ~kDE;
    if (op.check_flags && flag_bits(st.mxcsr) != want_flags) report(op.name, "mxcsr flags", 0, uint16_t(flag_bits(st.mxcsr)), uint16_t(want_flags), 0, 0, e, e.aaa, e.z);
    if (g_failed > 200) return;
  }
}

static std::vector<Op> g_ops;
static void add_op(const Op& o) { g_ops.push_back(o); }

// Every half value through a unary instruction (element 0 of a scalar form: vsqrtsh, vrndscalesh, ...), for a set of immediates and every rounding mode
static void scan_unary(const Op& op, const std::vector<int>& imms) {
  int reported = 0;
  for (int imm : imms) {
    for (int mode = 0; mode < 4; ++mode) {
      Enc e;
      e.map = op.map; e.pp = op.pp; e.op = op.opcode; e.w = op.w; e.vl = 128; e.reg = 1; e.vvvv = 2; e.rm = 3;
      if (op.imm) e.imm = imm;
      Machine m(with_syscall(encode(e)));
      for (uint32_t x = 0; x < 65536; ++x) {
        Machine mm(with_syscall(encode(e)));
        X86_64Cpu::State& st = mm.cpu.state();
        Zmm z;
        memset(&z, 0, sizeof z);
        z.h[0] = uint16_t(x);
        set_reg(st, 3, z);
        st.mxcsr = 0x1F80 | uint32_t(mode) << 13;
        if (mm.run() != StopReason::Syscall) { printf("scan %s: did not run\n", op.name.c_str()); ++g_failed; return; }
        uint32_t fl = 0;
        const uint16_t want = op.ref.fn(0, uint16_t(x), 0, mode, imm, &fl);
        const uint16_t got = get_reg(st, 1).h[0];
        const bool nan_ok = is_nan(want) && is_nan(got) && (want & 0x8000) == (got & 0x8000);
        if ((got != want && !nan_ok) || (op.check_flags && (flag_bits(st.mxcsr) != (fl & ~kDE)))) {
          if (reported++ < 12) printf("SCAN %s x=%04x imm=%02x mode=%d got %04x/%02x want %04x/%02x\n", op.name.c_str(), x, imm, mode, got, flag_bits(st.mxcsr), want, fl & ~kDE);
          ++g_failed;
        }
      }
    }
  }
}


static void build_arith_ops() {
  struct Bin { const char* n; uint8_t op; };
  static const Bin bins[] = {{"add", 0x58}, {"mul", 0x59}, {"sub", 0x5C}, {"min", 0x5D}, {"div", 0x5E}, {"max", 0x5F}};
  for (const Bin& bn : bins) {
    for (int sc = 0; sc < 2; ++sc) {
      Op o;
      o.name = std::string("v") + bn.n + (sc ? "sh" : "ph");
      o.map = 5; o.pp = sc ? 2 : 0; o.opcode = bn.op; o.w = false; o.scalar = sc;
      const uint8_t opc = bn.op;
      o.er = true;
      o.sae_only = bn.op == 0x5D || bn.op == 0x5F;
      o.ref.fn = [opc](uint16_t a, uint16_t b, uint16_t, int mode, int, uint32_t* fl) -> uint16_t {
        switch (opc) {
          case 0x58: return exact_add(a, b, mode, fl);
          case 0x59: return exact_mul(a, b, mode, fl);
          case 0x5C: return exact_add(a, neg16(b), mode, fl);      // (a NaN keeps its sign)
          case 0x5E: return exact_div(a, b, mode, fl);
          default: {      // min / max: the second operand when either is a NaN or both are zero
            if (is_nan(a) || is_nan(b)) { *fl |= kIE; return b; }
            if (is_zero(a) && is_zero(b)) return b;
            const float x = f_of(a), y = f_of(b);
            return opc == 0x5D ? (x < y ? a : b) : (x > y ? a : b);
          }
        }
      };
      if (bn.op == 0x5C) {
        o.ref.fn = [](uint16_t a, uint16_t b, uint16_t, int mode, int, uint32_t* fl) -> uint16_t {
          if (is_nan(a) || is_nan(b)) return nan_of(a, b, fl);
          return exact_add(a, neg16(b), mode, fl);
        };
      }
      add_op(o);
    }
  }
  for (int sc = 0; sc < 2; ++sc) {
    Op o;
    o.name = sc ? "vsqrtsh" : "vsqrtph";
    o.map = 5; o.pp = sc ? 2 : 0; o.opcode = 0x51; o.w = false; o.scalar = sc; o.unary = !sc;
    o.ref.fn = [](uint16_t, uint16_t b, uint16_t, int mode, int, uint32_t* fl) -> uint16_t { return exact_sqrt(b, mode, fl); };
    add_op(o);
  }
  // fused multiply-add: all 3 orders x 8 packed and 4 scalar signs
  for (int order = 0; order < 3; ++order) {
    for (int low = 6; low < 16; ++low) {
      const int opc = (order == 0 ? 0x90 : order == 1 ? 0xA0 : 0xB0) + low;
      Op o;
      static const char* nm[] = {"", "", "", "", "", "", "vfmaddsub", "vfmsubadd", "vfmadd", "vfmadd", "vfmsub", "vfmsub", "vfnmadd", "vfnmadd", "vfnmsub", "vfnmsub"};
      static const char* ord[] = {"132", "213", "231"};
      const bool scalar = low % 2 == 1 && low >= 9;
      o.name = std::string(nm[low]) + ord[order] + (scalar ? "sh" : "ph");
      o.map = 6; o.pp = 1; o.opcode = uint8_t(opc); o.w = false; o.scalar = scalar; o.dest_is_input = true;
      o.ref.fn = [order, low](uint16_t a, uint16_t b, uint16_t d, int mode, int, uint32_t* fl) -> uint16_t {
        // a = vvvv (src2), b = r/m (src3), d = destination (src1)
        uint16_t p, q, r;
        if (order == 0) { p = d; q = b; r = a; } else if (order == 1) { p = a; q = d; r = b; } else { p = a; q = b; r = d; }
        bool negp = low >= 12, negr = low == 10 || low == 11 || low == 14 || low == 15;
        return exact_fma(negp && !is_nan(p) ? neg16(p) : p, q, negr && !is_nan(r) ? neg16(r) : r, mode, fl);
      };
      if (low == 6 || low == 7) {      // addsub / subadd depend on the element index: wrapped below
        const int lowc = low;
        o.ref.fn = nullptr;
        (void)lowc;
      }
      if (low == 6 || low == 7) continue;
      add_op(o);
    }
  }
}

// vfmaddsub / vfmsubadd need the element index: run them with a dedicated driver (even elements subtract / add)
static void run_addsub(int iterations) {
  for (int order = 0; order < 3; ++order) {
    for (int low = 6; low < 8; ++low) {
      const int opc = (order == 0 ? 0x90 : order == 1 ? 0xA0 : 0xB0) + low;
      for (int it = 0; it < iterations; ++it) {
        Enc e;
        e.map = 6; e.pp = 1; e.op = uint8_t(opc);
        e.vl = rnd() % 3 == 0 ? 128 : rnd() % 2 ? 256 : 512;
        e.reg = int(rnd() % 32); e.vvvv = int(rnd() % 32); e.rm = int(rnd() % 32);
        Machine m(with_syscall(encode(e)));
        Zmm regs[32];
        for (int r = 0; r < 32; ++r) { for (int i = 0; i < 32; ++i) regs[r].h[i] = rnd_half(); set_reg(m.cpu.state(), r, regs[r]); }
        const int mode = int(rnd() % 4);
        m.cpu.state().mxcsr = 0x1F80 | uint32_t(mode) << 13;
        if (m.run() != StopReason::Syscall) { CHECK(false); return; }
        const Zmm got = get_reg(m.cpu.state(), e.reg);
        const Zmm a = regs[e.vvvv], b = regs[e.rm], d = regs[e.reg];
        for (int i = 0; i < 32; ++i) {
          uint16_t want = 0;
          if (i < e.vl / 16) {
            uint32_t fl = 0;
            uint16_t p, q, r;
            if (order == 0) { p = d.h[i]; q = b.h[i]; r = a.h[i]; } else if (order == 1) { p = a.h[i]; q = d.h[i]; r = b.h[i]; } else { p = a.h[i]; q = b.h[i]; r = d.h[i]; }
            const bool subtract = (low == 6) == ((i & 1) == 0);      // addsub: even elements subtract; subadd: even elements add
            want = exact_fma(p, q, subtract && !is_nan(r) ? neg16(r) : r, mode, &fl);
          }
          const bool nan_ok = is_nan(want) && is_nan(got.h[i]) && (want & 0x8000) == (got.h[i] & 0x8000);
          if (got.h[i] != want && !nan_ok) { printf("MISMATCH vfmaddsub/subadd %02x elem %d got %04x want %04x\n", opc, i, got.h[i], want); ++g_failed; return; }
        }
      }
    }
  }
}

// --- the other drivers ---------------------------------------------------------------------------------------------------------------------

static void prepare(Machine& m, Zmm regs[32], uint64_t* kmask, int aaa, uint16_t* mem_h, int mem_count) {
  X86_64Cpu::State& st = m.cpu.state();
  for (int r = 0; r < 32; ++r) { for (int i = 0; i < 32; ++i) regs[r].h[i] = rnd_half(); set_reg(st, r, regs[r]); }
  for (int i = 0; i < mem_count; ++i) mem_h[i] = rnd_half();
  if (mem_count) m.poke(kBase - 0x400, mem_h, size_t(mem_count) * 2);
  *kmask = rnd() % 4 == 0 ? ~0ull : rnd();
  for (int k = 0; k < 8; ++k) st.k[k] = k == aaa ? *kmask : rnd();
}

// vcmpph / vcmpsh: all 32 predicates, the result in a mask register
static bool compare_pred(int pred, uint16_t a, uint16_t b, uint32_t* fl) {
  const bool un = is_nan(a) || is_nan(b);
  const float x = f_of(a), y = f_of(b);
  // bit 4 of the predicate inverts the quiet / signalling nature of the base predicate
  static const bool base_signaling[16] = {false, true, true, false, false, true, true, false, false, true, true, false, false, true, true, false};
  const bool sig = ((pred & 0x10) != 0) != base_signaling[pred & 15];
  if (un && (sig || is_snan(a) || is_snan(b))) *fl |= kIE;
  switch (pred & 15) {
    case 0: return !un && x == y;
    case 1: return !un && x < y;
    case 2: return !un && x <= y;
    case 3: return un;
    case 4: return un || x != y;
    case 5: return un || !(x < y);
    case 6: return un || !(x <= y);
    case 7: return !un;
    case 8: return un || x == y;
    case 9: return un || !(x >= y);
    case 10: return un || !(x > y);
    case 11: return false;
    case 12: return !un && x != y;
    case 13: return !un && x >= y;
    case 14: return !un && x > y;
    default: return true;
  }
}
static void run_compare(int iterations) {
  for (int scalar = 0; scalar < 2; ++scalar) {
    for (int it = 0; it < iterations; ++it) {
      Enc e;
      e.map = 3; e.pp = scalar ? 2 : 0; e.op = 0xC2;
      e.vl = scalar ? 128 : (rnd() % 3 == 0 ? 128 : rnd() % 2 ? 256 : 512);
      e.reg = int(rnd() % 8); e.vvvv = int(rnd() % 32);
      e.aaa = rnd() % 3 == 0 ? 0 : int(rnd() % 8);
      e.imm = int(rnd() % 32);
      const int kind = int(rnd() % 3);
      if (kind == 1) { e.memory = true; e.disp = 64; e.disp8_factor = 0; } else if (kind == 2 && !scalar) { e.memory = true; e.bcst = true; e.disp = 8; } else { e.rm = int(rnd() % 32); if (e.vl == 512 && rnd() % 4 == 0) { e.rc = 0; } }
      if (e.memory && scalar) e.bcst = false;
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint16_t mem_h[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, e.aaa, mem_h, 0);
      for (int i = 0; i < 32; ++i) mem_h[i] = rnd_half();
      if (e.memory) m.poke(kBase + e.disp, mem_h, 64);
      if (e.rc >= 0) e.vl = 512;
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      const Zmm a = regs[e.vvvv];
      Zmm b = e.memory ? Zmm() : regs[e.rm];
      if (e.memory) for (int i = 0; i < 32; ++i) b.h[i] = e.bcst ? mem_h[0] : mem_h[i];
      const int n = scalar ? 1 : e.vl / 16;
      uint64_t want = 0;
      uint32_t flags = 0;
      for (int i = 0; i < n; ++i) {
        if (e.aaa != 0 && !((kmask >> i) & 1)) continue;
        uint32_t fl = 0;
        if (compare_pred(e.imm, a.h[i], b.h[i], &fl)) want |= 1ull << i;
        flags |= fl;
      }
      if (m.cpu.state().k[e.reg] != want) { printf("MISMATCH vcmp%s pred %d got %llx want %llx\n", scalar ? "sh" : "ph", e.imm, (unsigned long long)m.cpu.state().k[e.reg], (unsigned long long)want); ++g_failed; return; }
      if (e.rc >= 0) flags = 0;      // (SAE: no exception flags)
      if (flag_bits(m.cpu.state().mxcsr) != flags) { printf("MISMATCH vcmp flags pred %d got %x want %x\n", e.imm, flag_bits(m.cpu.state().mxcsr), flags); ++g_failed; return; }
    }
  }
}

// vcomish / vucomish: ZF, PF, CF
static void run_comi(int iterations) {
  for (int signaling = 0; signaling < 2; ++signaling) {
    for (int it = 0; it < iterations; ++it) {
      Enc e;
      e.map = 5; e.pp = 0; e.op = signaling ? 0x2F : 0x2E; e.vl = 128;
      e.reg = int(rnd() % 32); e.rm = int(rnd() % 32);
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, 0, nullptr, 0);
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      const uint16_t a = regs[e.reg].h[0], b = regs[e.rm].h[0];
      const bool un = is_nan(a) || is_nan(b);
      uint64_t want = 0;
      if (un) want = 0x40 | 4 | 1; else if (f_of(a) < f_of(b)) want = 1; else if (f_of(a) == f_of(b)) want = 0x40;
      const uint64_t got = m.cpu.state().rflags & (0x40 | 4 | 1 | 0x800 | 0x10 | 0x80);
      uint32_t wf = 0;
      if (un && (signaling || is_snan(a) || is_snan(b))) wf = kIE;
      if (got != want || flag_bits(m.cpu.state().mxcsr) != wf) { printf("MISMATCH vcomish signaling=%d a=%04x b=%04x flags %llx want %llx\n", signaling, a, b, (unsigned long long)got, (unsigned long long)want); ++g_failed; return; }
    }
  }
}

// rndscale / reduce / getexp / getmant / scalef / rcp / rsqrt / fpclass use the element driver with their models
static uint16_t half_of(double x) { return bits_of(static_cast<H>(x)); }
static uint16_t model_roundscale(uint16_t x, int imm, int mode_mxcsr, uint32_t* fl, bool reduce) {
  if (is_nan(x)) { if (is_snan(x)) *fl |= kIE; return quiet(x); }
  if (is_inf(x)) return reduce ? 0 : x;
  const int scale = imm >> 4;
  const int mode = (imm & 4) ? mode_mxcsr : (imm & 3);
  const double v = f_of(x);
  const double s = std::ldexp(v, scale);
  double r;
  switch (mode) { case 0: r = nearbyint(s); break; case 1: r = floor(s); break; case 2: r = ceil(s); break; default: r = trunc(s); }
  r = std::ldexp(r, -scale);
  if (is_zero(x) && !reduce) return x;
  if (reduce) {
    // x - r computed exactly in double (both are multiples of 2^-24 within a small range), then rounded to half in the same mode
    const double diff = v - r;
    uint32_t f2 = 0;
    const bool zero = diff == 0;
    uint16_t out;
    if (zero) out = (mode == 1) ? 0x8000 : 0;
    else {
      // exact value in double -> half with `mode`
      u128 N; int E;
      int ex;
      const double fr = std::frexp(std::fabs(diff), &ex);
      N = u128(std::ldexp(fr, 53)); E = ex - 53;
      out = round_half(diff < 0, N, E, false, mode, &f2);
    }
    *fl |= f2 & kIE;
    if (!(imm & 8)) *fl |= f2 & (kPE | kUE | kOE);
    return out;
  }
  // roundscale: the value r is exact in half unless it overflows (cannot, since |r| <= |x| + 1)
  if (!(imm & 8) && r != v) *fl |= kPE;
  if (r == 0) return (x & 0x8000) ? 0x8000 : 0;
  return half_of(r);
}

static void build_unary_ops() {
  for (int sc = 0; sc < 2; ++sc) {
    Op o;
    o.name = sc ? "vrndscalesh" : "vrndscaleph";
    o.map = 3; o.pp = 0; o.opcode = sc ? 0x0A : 0x08; o.w = false; o.scalar = sc; o.unary = !sc; o.imm = true; o.sae_only = true;
    o.ref.fn = [](uint16_t, uint16_t b, uint16_t, int mode, int imm, uint32_t* fl) -> uint16_t { return model_roundscale(b, imm, mode, fl, false); };
    add_op(o);
    Op r;
    r.name = sc ? "vreducesh" : "vreduceph";
    r.map = 3; r.pp = 0; r.opcode = sc ? 0x57 : 0x56; r.w = false; r.scalar = sc; r.unary = !sc; r.imm = true; r.sae_only = true;
    r.ref.fn = [](uint16_t, uint16_t b, uint16_t, int mode, int imm, uint32_t* fl) -> uint16_t { return model_roundscale(b, imm, mode, fl, true); };
    add_op(r);
    Op g;
    g.name = sc ? "vgetexpsh" : "vgetexpph";
    g.map = 6; g.pp = 1; g.opcode = sc ? 0x43 : 0x42; g.w = false; g.scalar = sc; g.unary = !sc; g.sae_only = true;
    g.ref.fn = [](uint16_t, uint16_t b, uint16_t, int, int, uint32_t* fl) -> uint16_t {
      if (is_nan(b)) { if (is_snan(b)) *fl |= kIE; return quiet(b); }
      if (is_inf(b)) return 0x7C00;
      if (is_zero(b)) return 0xFC00;
      int ex;
      std::frexp(std::fabs(f_of(b)), &ex);
      return half_of(ex - 1);
    };
    add_op(g);
    Op gm;
    gm.name = sc ? "vgetmantsh" : "vgetmantph";
    gm.map = 3; gm.pp = 0; gm.opcode = sc ? 0x27 : 0x26; gm.w = false; gm.scalar = sc; gm.unary = !sc; gm.imm = true; gm.sae_only = true;
    gm.ref.fn = [](uint16_t, uint16_t b, uint16_t, int, int imm, uint32_t* fl) -> uint16_t {
      const int interval = imm & 3, sign_ctl = (imm >> 2) & 3;
      if (is_nan(b)) { if (is_snan(b)) *fl |= kIE; return quiet(b); }
      const bool neg = (b & 0x8000) != 0;
      if ((sign_ctl & 2) && neg && !is_zero(b)) { *fl |= kIE; return kDefaultNan; }
      const uint16_t sign = (sign_ctl & 1) ? 0 : (neg ? 0x8000 : 0);
      if (is_zero(b) || is_inf(b)) return sign | 0x3C00;
      int ex;
      const double fr = std::frexp(std::fabs(f_of(b)), &ex);      // fr in [0.5, 1); the value is fr * 2^ex
      const double m = fr * 2;                                    // in [1, 2), with exponent e
      const int e = ex - 1;
      double mant;
      switch (interval) {
        case 0: mant = m; break;                                  // [1, 2)
        case 1: mant = (e & 1) ? fr : m; break;                   // [1/2, 2): an odd unbiased exponent gives [1/2, 1)
        case 2: mant = fr; break;                                 // [1/2, 1)
        default: mant = m >= 1.5 ? fr : m; break;                 // [3/4, 3/2)
      }
      return sign | half_of(mant);
    };
    add_op(gm);
    Op sf;
    sf.name = sc ? "vscalefsh" : "vscalefph";
    sf.map = 6; sf.pp = 1; sf.opcode = sc ? 0x2D : 0x2C; sf.w = false; sf.scalar = sc;
    sf.ref.fn = [](uint16_t a, uint16_t b, uint16_t, int mode, int, uint32_t* fl) -> uint16_t {
      if (is_nan(a) || is_nan(b)) return nan_of(a, b, fl);
      const bool aneg = a & 0x8000, bneg = b & 0x8000;
      if (is_zero(a)) { if (is_inf(b) && !bneg) { *fl |= kIE; return kDefaultNan; } return a; }
      if (is_inf(a)) { if (is_inf(b) && bneg) { *fl |= kIE; return kDefaultNan; } return a; }
      if (is_inf(b)) return bneg ? (aneg ? 0x8000 : 0) : (a & 0x8000) | 0x7C00;
      const double k = std::floor(f_of(b));
      Fin x;
      unpack_finite(a, &x);
      int e = x.e + int(k > 200 ? 200 : k < -200 ? -200 : k);
      return round_half(aneg, u128(x.m), e, false, mode, fl);
    };
    add_op(sf);
  }
  for (int which = 0; which < 2; ++which) {
    for (int sc = 0; sc < 2; ++sc) {
      Op o;
      o.name = std::string(which ? "vrsqrt" : "vrcp") + (sc ? "sh" : "ph");
      o.map = 6; o.pp = 1; o.opcode = uint8_t((which ? 0x4E : 0x4C) + sc); o.w = false; o.scalar = sc; o.unary = !sc; o.er = false; o.check_flags = false;
      o.ref.fn = [which](uint16_t, uint16_t b, uint16_t, int, int, uint32_t* fl) -> uint16_t {
        // the emulator returns the correctly rounded value; the instruction promises a relative error below 2^-11, so accept within 1 ulp of the exact result
        if (is_nan(b)) { if (is_snan(b)) *fl |= kIE; return quiet(b); }
        if (which == 0) {
          if (is_zero(b)) return (b & 0x8000) | 0x7C00;
          if (is_inf(b)) return b & 0x8000;
          return exact_div(0x3C00, b, 0, fl);
        }
        if (is_zero(b)) return (b & 0x8000) | 0x7C00;
        if (b & 0x8000) { *fl |= kIE; return kDefaultNan; }
        if (is_inf(b)) return 0;
        uint32_t f2 = 0;
        const uint16_t root = exact_sqrt(b, 0, &f2);
        // 1/sqrt(x) exactly rounded: use long double
        const long double r = 1.0L / sqrtl((long double)f_of(b));
        (void)root;
        uint32_t f3 = 0;
        Fin dummy; (void)dummy;
        int ex; const long double fr = frexpl(r, &ex);
        const u128 N = u128(ldexpl(fr, 63)); const int E = ex - 63;
        return round_half(false, N, E, true, 0, &f3);
      };
      add_op(o);
    }
  }
}

// vfpclassph / vfpclasssh: the result in a mask register
static void run_fpclass(int iterations) {
  for (int scalar = 0; scalar < 2; ++scalar) {
    for (int it = 0; it < iterations; ++it) {
      Enc e;
      e.map = 3; e.pp = 0; e.op = scalar ? 0x67 : 0x66;
      e.vl = scalar ? 128 : (rnd() % 3 == 0 ? 128 : rnd() % 2 ? 256 : 512);
      e.reg = int(rnd() % 8); e.rm = int(rnd() % 32);
      e.aaa = rnd() % 3 == 0 ? 0 : int(rnd() % 8);
      e.imm = int(rnd() % 256);
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, e.aaa, nullptr, 0);
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      uint64_t want = 0;
      const int n = scalar ? 1 : e.vl / 16;
      for (int i = 0; i < n; ++i) {
        if (e.aaa != 0 && !((kmask >> i) & 1)) continue;
        const uint16_t x = regs[e.rm].h[i];
        const bool neg = x >> 15, nan = is_nan(x), inf = is_inf(x), zero = is_zero(x), den = ((x >> 10) & 31) == 0 && (x & 0x3FF);
        const bool hit = ((e.imm & 1) && nan && !is_snan(x)) || ((e.imm & 2) && zero && !neg) || ((e.imm & 4) && zero && neg) || ((e.imm & 8) && inf && !neg) || ((e.imm & 16) && inf && neg) ||
                         ((e.imm & 32) && den) || ((e.imm & 64) && neg && !nan && !inf && !zero) || ((e.imm & 128) && is_snan(x));
        if (hit) want |= 1ull << i;
      }
      if (m.cpu.state().k[e.reg] != want) { printf("MISMATCH vfpclass%s imm %02x got %llx want %llx\n", scalar ? "sh" : "ph", e.imm, (unsigned long long)m.cpu.state().k[e.reg], (unsigned long long)want); ++g_failed; return; }
    }
  }
}

// --- conversions -------------------------------------------------------------------------------------------------------------------------

enum Ty { tH, tF, tD, tI16, tU16, tI32, tU32, tI64, tU64 };
static int ty_bits(Ty t) { return t == tH || t == tI16 || t == tU16 ? 16 : t == tF || t == tI32 || t == tU32 ? 32 : 64; }
static bool ty_float(Ty t) { return t == tH || t == tF || t == tD; }

// converts a value of type `src` (raw bits) to `dst`; returns the raw bits and accumulates flags
static uint64_t convert_model(Ty src, Ty dst, uint64_t v, bool truncate, int mode, uint32_t* fl) {
  if (src == tH && dst == tF) { if (is_snan(uint16_t(v))) *fl |= kIE; float f = f_of(uint16_t(v)); if (is_nan(uint16_t(v))) { uint32_t b = 0x7FC00000u | ((uint32_t(v) & 0x8000) << 16) | ((uint32_t(v) & 0x1FF) << 13); return b; } uint32_t b; memcpy(&b, &f, 4); return b; }
  if (src == tH && dst == tD) { if (is_snan(uint16_t(v))) *fl |= kIE; double f = f_of(uint16_t(v)); if (is_nan(uint16_t(v))) { return 0x7FF8000000000000ull | ((uint64_t(v) & 0x8000) << 48) | ((uint64_t(v) & 0x1FF) << 42); } uint64_t b; memcpy(&b, &f, 8); return b; }
  if ((src == tF || src == tD) && dst == tH) {
    // exact: unpack the float/double
    bool neg; int E; u128 N; bool nan = false, inf = false, zero = false, snan = false;
    if (src == tF) {
      const uint32_t b = uint32_t(v); neg = b >> 31; const int ex = (b >> 23) & 255; const uint32_t fr = b & 0x7FFFFF;
      nan = ex == 255 && fr; inf = ex == 255 && !fr; zero = ex == 0 && !fr; snan = nan && !(fr & 0x400000);
      if (ex == 0) { N = fr; E = -149; } else { N = fr | 0x800000; E = ex - 150; }
      if (nan) { if (snan) *fl |= kIE; return uint16_t((neg ? 0x8000 : 0) | 0x7E00 | (fr >> 13 & 0x1FF)); }
    } else {
      neg = v >> 63; const int ex = (v >> 52) & 2047; const uint64_t fr = v & 0xFFFFFFFFFFFFFull;
      nan = ex == 2047 && fr; inf = ex == 2047 && !fr; zero = ex == 0 && !fr; snan = nan && !(fr & 0x8000000000000ull);
      if (ex == 0) { N = fr; E = -1074; } else { N = fr | 0x10000000000000ull; E = ex - 1075; }
      if (nan) { if (snan) *fl |= kIE; return uint16_t((neg ? 0x8000 : 0) | 0x7E00 | ((fr >> 42) & 0x1FF)); }
    }
    if (inf) return (neg ? 0x8000 : 0) | 0x7C00;
    if (zero) return neg ? 0x8000 : 0;
    return round_half(neg, N, E, false, mode, fl);
  }
  if (ty_float(src) && !ty_float(dst)) {
    const uint16_t h = uint16_t(v);
    const int dbits = ty_bits(dst);
    const bool is_signed = dst == tI16 || dst == tI32 || dst == tI64;
    const uint64_t indef = is_signed ? (uint64_t(1) << (dbits - 1)) : (dbits == 64 ? ~0ull : (1ull << dbits) - 1);
    if (is_nan(h) || is_inf(h)) { *fl |= kIE; return indef; }
    if (is_zero(h)) return 0;
    Fin x;
    unpack_finite(h, &x);
    __int128 mag;
    bool inexact = false;
    if (x.e >= 0) mag = __int128(x.m) << x.e;
    else {
      const int sh = -x.e;
      const __int128 kept = x.m >> sh, rem = x.m & ((__int128(1) << sh) - 1), halfv = __int128(1) << (sh - 1);
      inexact = rem != 0;
      bool up = false;
      if (!truncate) switch (mode) {
        case 0: up = rem > halfv || (rem == halfv && (kept & 1)); break;
        case 1: up = x.neg && inexact; break;
        case 2: up = !x.neg && inexact; break;
        default: break;
      }
      mag = kept + (up ? 1 : 0);
    }
    const __int128 val = x.neg ? -mag : mag;
    const __int128 lo = is_signed ? -(__int128(1) << (dbits - 1)) : 0, hi = is_signed ? (__int128(1) << (dbits - 1)) - 1 : (__int128(1) << dbits) - 1;
    if (val < lo || val > hi) { *fl |= kIE; return indef; }
    if (inexact) *fl |= kPE;
    return uint64_t(val) & (dbits == 64 ? ~0ull : (1ull << dbits) - 1);
  }
  if (!ty_float(src) && dst == tH) {
    const int sbits = ty_bits(src);
    const bool is_signed = src == tI16 || src == tI32 || src == tI64;
    __int128 val = v & (sbits == 64 ? ~0ull : (1ull << sbits) - 1);
    if (is_signed && (val >> (sbits - 1)) & 1) val -= __int128(1) << sbits;
    if (val == 0) return 0;
    return round_half(val < 0, u128(val < 0 ? -val : val), 0, false, mode, fl);
  }
  return 0;
}

struct Cv {
  const char* name; uint8_t map, pp, opcode; bool w; Ty src, dst; bool truncate; int mem_bits;      // mem_bits: width of one memory element (broadcast)
  int tuple;      // 0 full, 1 half, 2 quarter (relative to the vector length)
};

static void run_conversion(const Cv& c, int iterations) {
  const int sw = ty_bits(c.src), dw = ty_bits(c.dst), big = sw > dw ? sw : dw;
  for (int it = 0; it < iterations; ++it) {
    Enc e;
    e.map = c.map; e.pp = c.pp; e.op = c.opcode; e.w = c.w;
    e.vl = rnd() % 3 == 0 ? 128 : rnd() % 2 ? 256 : 512;
    e.reg = int(rnd() % 32); e.rm = int(rnd() % 32);
    e.aaa = rnd() % 3 == 0 ? 0 : int(rnd() % 8);
    e.z = e.aaa != 0 && (rnd() & 1);
    const int kind = int(rnd() % 4);
    int mode = int(rnd() % 4);
    int rounding = mode;
    const int n = e.vl / big;
    int src_bytes = n * sw / 8;
    if (kind == 3) {
      e.rc = int(rnd() % 4); e.vl = 512; rounding = e.rc;
    } else if (kind == 1) {
      e.memory = true;
    } else if (kind == 2) {
      e.memory = true; e.bcst = true;
    }
    const int nn = e.vl / big;
    src_bytes = nn * sw / 8;
    if (e.memory) {
      const int factor = e.bcst ? sw / 8 : src_bytes;
      e.disp = int(int(rnd() % 9) - 4) * factor;
      if (rnd() % 3 != 0) e.disp8_factor = factor;
    }
    Machine m(with_syscall(encode(e)));
    Zmm regs[32];
    uint16_t mem_h[64];
    uint64_t kmask;
    prepare(m, regs, &kmask, e.aaa, nullptr, 0);
    // the source operand: floats of the source type derived from random halves, integers random
    uint64_t src_vals[32];
    for (int i = 0; i < 32; ++i) {
      uint64_t raw = rnd();
      switch (c.src) {
        case tH: raw = rnd_half(); break;
        case tF: { const uint16_t h = rnd_half(); float f = f_of(h) * (rnd() % 5 == 0 ? 1.0009765625f : 1.0f); if (rnd() % 7 == 0) f *= 1e-4f; uint32_t b; memcpy(&b, &f, 4); raw = b; break; }
        case tD: { double d = double(f_of(rnd_half())) * (rnd() % 5 == 0 ? 1.0000001 : 1.0); if (rnd() % 7 == 0) d *= 1e-9; memcpy(&raw, &d, 8); break; }
        case tI16: case tU16: raw = (rnd() % 3 == 0) ? (rnd() & 0xFFFF) : (rnd() % 4000) - 1500; break;
        case tI32: case tU32: raw = (rnd() % 3 == 0) ? rnd() : (rnd() % 100000) - 30000; break;
        default: raw = (rnd() % 3 == 0) ? rnd() : (rnd() % 1000000) - 300000; break;
      }
      src_vals[i] = raw & (sw == 64 ? ~0ull : (1ull << sw) - 1);
    }
    // place it
    if (e.memory) {
      uint8_t buf[64] = {};
      if (e.bcst) memcpy(buf, &src_vals[0], sw / 8); else for (int i = 0; i < nn; ++i) memcpy(buf + i * sw / 8, &src_vals[i], sw / 8);
      m.poke(kBase + e.disp, buf, 64);
    } else {
      Zmm z = regs[e.rm];
      uint8_t buf[64] = {};
      for (int i = 0; i < 64 / (sw / 8) && i < 64; ++i) memcpy(buf + i * sw / 8, &src_vals[i % 32], sw / 8);
      memcpy(z.h, buf, 64);
      regs[e.rm] = z;
      set_reg(m.cpu.state(), e.rm, z);
    }
    m.cpu.state().mxcsr = 0x1F80 | uint32_t(mode) << 13;
    const Zmm before = regs[e.reg];
    if (m.run() != StopReason::Syscall) { printf("MISMATCH %s: did not run\n", c.name); ++g_failed; return; }
    const Zmm got = get_reg(m.cpu.state(), e.reg);
    uint8_t want[64] = {}, gotb[64], oldb[64];
    memcpy(gotb, got.h, 64);
    memcpy(oldb, before.h, 64);
    uint32_t flags = 0;
    for (int i = 0; i < nn; ++i) {
      const bool active = e.aaa == 0 || ((kmask >> i) & 1);
      uint64_t out;
      if (active) {
        const uint64_t sv = e.memory && e.bcst ? src_vals[0] : e.memory ? src_vals[i] : (src_vals[i % 32]);
        uint64_t sraw = sv;
        if (!e.memory) { uint8_t buf[64]; memcpy(buf, regs[e.rm].h, 64); sraw = 0; memcpy(&sraw, buf + i * sw / 8, sw / 8); }
        uint32_t fl = 0;
        out = convert_model(c.src, c.dst, sraw, c.truncate, rounding, &fl);
        flags |= fl;
      } else if (e.z) {
        out = 0;
      } else {
        out = 0;
        memcpy(&out, oldb + i * dw / 8, dw / 8);
      }
      memcpy(want + i * dw / 8, &out, dw / 8);
    }
    const int result_bytes = nn * dw / 8;
    // bits above the result stay zero (VEX-like clearing of the destination register)
    bool ok = true;
    for (int i = 0; i < 64; ++i) {
      const uint8_t w = i < result_bytes ? want[i] : 0;
      if (gotb[i] != w) {
        // NaN payload of a widening conversion is checked bitwise; for the others compare exactly
        printf("MISMATCH %s byte %d got %02x want %02x (vl=%d kind=%d aaa=%d z=%d rc=%d)\n", c.name, i, gotb[i], w, e.vl, kind, e.aaa, e.z, e.rc);
        ok = false;
        break;
      }
    }
    if (!ok) { ++g_failed; return; }
    const uint32_t want_flags = e.rc >= 0 ? 0 : (flags & ~kDE);
    if ((flag_bits(m.cpu.state().mxcsr) & ~kDE) != want_flags) { printf("MISMATCH %s flags got %x want %x\n", c.name, flag_bits(m.cpu.state().mxcsr), want_flags); ++g_failed; return; }
    (void)mem_h;
  }
}

static const Cv kConversions[] = {
    {"vcvtph2pd", 5, 0, 0x5A, false, tH, tD, false, 16, 2}, {"vcvtpd2ph", 5, 1, 0x5A, true, tD, tH, false, 64, 0},
    {"vcvtdq2ph", 5, 0, 0x5B, false, tI32, tH, false, 32, 0}, {"vcvtqq2ph", 5, 0, 0x5B, true, tI64, tH, false, 64, 0},
    {"vcvtph2dq", 5, 1, 0x5B, false, tH, tI32, false, 16, 1}, {"vcvttph2dq", 5, 2, 0x5B, false, tH, tI32, true, 16, 1},
    {"vcvtps2phx", 5, 1, 0x1D, false, tF, tH, false, 32, 0}, {"vcvtph2psx", 6, 1, 0x13, false, tH, tF, false, 16, 1},
    {"vcvtph2udq", 5, 0, 0x79, false, tH, tU32, false, 16, 1}, {"vcvttph2udq", 5, 0, 0x78, false, tH, tU32, true, 16, 1},
    {"vcvtph2uqq", 5, 1, 0x79, false, tH, tU64, false, 16, 2}, {"vcvttph2uqq", 5, 1, 0x78, false, tH, tU64, true, 16, 2},
    {"vcvtph2qq", 5, 1, 0x7B, false, tH, tI64, false, 16, 2}, {"vcvttph2qq", 5, 1, 0x7A, false, tH, tI64, true, 16, 2},
    {"vcvtudq2ph", 5, 3, 0x7A, false, tU32, tH, false, 32, 0}, {"vcvtuqq2ph", 5, 3, 0x7A, true, tU64, tH, false, 64, 0},
    {"vcvtph2uw", 5, 0, 0x7D, false, tH, tU16, false, 16, 0}, {"vcvttph2uw", 5, 0, 0x7C, false, tH, tU16, true, 16, 0},
    {"vcvtph2w", 5, 1, 0x7D, false, tH, tI16, false, 16, 0}, {"vcvttph2w", 5, 1, 0x7C, false, tH, tI16, true, 16, 0},
    {"vcvtuw2ph", 5, 3, 0x7D, false, tU16, tH, false, 16, 0}, {"vcvtw2ph", 5, 2, 0x7D, false, tI16, tH, false, 16, 0},
};

// scalar conversions
struct ScalarCv { const char* name; uint8_t map, pp, opcode; bool w; Ty src, dst; };
static const ScalarCv kScalarConversions[] = {
    {"vcvtsh2ss", 6, 0, 0x13, false, tH, tF}, {"vcvtss2sh", 5, 0, 0x1D, false, tF, tH}, {"vcvtsh2sd", 5, 2, 0x5A, false, tH, tD}, {"vcvtsd2sh", 5, 3, 0x5A, true, tD, tH},
};
static void run_scalar_conversion(const ScalarCv& c, int iterations) {
  const int sw = ty_bits(c.src), dw = ty_bits(c.dst);
  for (int it = 0; it < iterations; ++it) {
    Enc e;
    e.map = c.map; e.pp = c.pp; e.op = c.opcode; e.w = c.w; e.vl = 128;
    e.reg = int(rnd() % 32); e.vvvv = int(rnd() % 32);
    e.aaa = rnd() % 3 == 0 ? 0 : int(rnd() % 8);
    e.z = e.aaa != 0 && (rnd() & 1);
    const int kind = int(rnd() % 3);
    int mode = int(rnd() % 4), rounding = mode;
    if (kind == 1) { e.memory = true; e.disp = int(int(rnd() % 9) - 4) * (sw / 8); if (rnd() & 1) e.disp8_factor = sw / 8; }
    else if (kind == 2) { e.rc = int(rnd() % 4); rounding = e.rc; e.rm = int(rnd() % 32); }
    else e.rm = int(rnd() % 32);
    Machine m(with_syscall(encode(e)));
    Zmm regs[32];
    uint64_t kmask;
    prepare(m, regs, &kmask, e.aaa, nullptr, 0);
    uint64_t sv;
    switch (c.src) {
      case tH: sv = rnd_half(); break;
      case tF: { float f = f_of(rnd_half()) * 1.0007f; memcpy(&sv, &f, 4); sv &= 0xFFFFFFFFu; break; }
      default: { double d = double(f_of(rnd_half())) * 1.0000003; memcpy(&sv, &d, 8); break; }
    }
    if (e.memory) { m.poke(kBase + e.disp, &sv, sw / 8); }
    else { Zmm z = regs[e.rm]; memcpy(z.h, &sv, sw / 8); regs[e.rm] = z; set_reg(m.cpu.state(), e.rm, z); }
    m.cpu.state().mxcsr = 0x1F80 | uint32_t(mode) << 13;
    const Zmm a = regs[e.vvvv], d = regs[e.reg];
    if (m.run() != StopReason::Syscall) { CHECK(false); return; }
    const Zmm got = get_reg(m.cpu.state(), e.reg);
    uint8_t want[64] = {};
    // bits 127:dw from vvvv
    uint8_t abytes[64];
    memcpy(abytes, a.h, 64);
    memcpy(want, abytes, 16);
    uint64_t out = 0;
    uint32_t fl = 0;
    if (e.aaa == 0 || (kmask & 1)) out = convert_model(c.src, c.dst, sv, false, rounding, &fl);
    else if (!e.z) { uint8_t dbytes[64]; memcpy(dbytes, d.h, 64); memcpy(&out, dbytes, dw / 8); }
    memcpy(want, &out, dw / 8);
    uint8_t gotb[64];
    memcpy(gotb, got.h, 64);
    for (int i = 0; i < 64; ++i) {
      const uint8_t w = i < 16 ? want[i] : 0;
      if (gotb[i] != w) { printf("MISMATCH %s byte %d got %02x want %02x (kind %d aaa %d z %d)\n", c.name, i, gotb[i], w, kind, e.aaa, e.z); ++g_failed; return; }
    }
  }
}

// vcvtsh2si family and vcvtsi2sh family
static void run_gpr_conversions(int iterations) {
  struct To { const char* name; uint8_t op; bool truncate, is_unsigned; };
  static const To tos[] = {{"vcvtsh2si", 0x2D, false, false}, {"vcvttsh2si", 0x2C, true, false}, {"vcvtsh2usi", 0x79, false, true}, {"vcvttsh2usi", 0x78, true, true}};
  for (const To& t : tos) {
    for (int it = 0; it < iterations; ++it) {
      Enc e;
      e.map = 5; e.pp = 2; e.op = t.op; e.w = rnd() & 1; e.vl = 128;
      static const int gprs[] = {0, 2, 3, 5, 6, 8, 9, 10, 12, 13, 14, 15};      // (not rcx, r11: syscall clobbers them; not rsp, rdi)
      e.reg = gprs[rnd() % 12]; e.rm = int(rnd() % 32);
      const int kind = int(rnd() % 3);
      int mode = int(rnd() % 4), rounding = mode;
      if (kind == 1) { e.memory = true; e.disp = 16; if (rnd() & 1) e.disp8_factor = 2; }
      else if (kind == 2) { e.rc = int(rnd() % 4); rounding = e.rc; }
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, 0, nullptr, 0);
      const uint16_t h = rnd_half() & ((rnd() % 3) ? 0xFFFF : 0x7FFF);
      uint16_t hv = rnd() % 2 ? h : bits_of(static_cast<H>(static_cast<float>(int32_t(rnd() % 100000) - 50000)));
      if (e.memory) m.poke(kBase + e.disp, &hv, 2); else { Zmm z = regs[e.rm]; z.h[0] = hv; set_reg(m.cpu.state(), e.rm, z); }
      m.cpu.state().mxcsr = 0x1F80 | uint32_t(mode) << 13;
      m.cpu.state().r[e.reg] = 0xDEADBEEFCAFEF00Dull;
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      uint32_t fl = 0;
      uint64_t want = convert_model(tH, e.w ? (t.is_unsigned ? tU64 : tI64) : (t.is_unsigned ? tU32 : tI32), hv, t.truncate, rounding, &fl);
      if (!e.w) want &= 0xFFFFFFFFu;
      if (m.cpu.state().r[e.reg] != want) { printf("MISMATCH %s w=%d h=%04x got %llx want %llx\n", t.name, e.w, hv, (unsigned long long)m.cpu.state().r[e.reg], (unsigned long long)want); ++g_failed; return; }
    }
  }
  struct From { const char* name; uint8_t op; bool is_unsigned; };
  static const From froms[] = {{"vcvtsi2sh", 0x2A, false}, {"vcvtusi2sh", 0x7B, true}};
  for (const From& f : froms) {
    for (int it = 0; it < iterations; ++it) {
      Enc e;
      e.map = 5; e.pp = 2; e.op = f.op; e.w = rnd() & 1; e.vl = 128;
      e.reg = int(rnd() % 32); e.vvvv = int(rnd() % 32);
      static const int gprs2[] = {0, 2, 3, 5, 6, 8, 9, 10, 12, 13, 14, 15};
      const int kind = int(rnd() % 3);
      int mode = int(rnd() % 4), rounding = mode;
      uint64_t val = (rnd() % 3 == 0) ? rnd() : (rnd() % 200000) - 100000;
      if (!e.w) val &= 0xFFFFFFFFu;
      if (kind == 1) { e.memory = true; e.disp = e.w ? 8 * 3 : 4 * 3; e.disp8_factor = e.w ? 8 : 4; }
      else if (kind == 2) { e.rc = int(rnd() % 4); rounding = e.rc; e.rm = gprs2[rnd() % 12]; }
      else e.rm = gprs2[rnd() % 12];
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, 0, nullptr, 0);
      if (e.memory) m.poke(kBase + e.disp, &val, e.w ? 8 : 4); else m.cpu.state().r[e.rm] = val;
      m.cpu.state().mxcsr = 0x1F80 | uint32_t(mode) << 13;
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      uint32_t fl = 0;
      const uint64_t want = convert_model(e.w ? (f.is_unsigned ? tU64 : tI64) : (f.is_unsigned ? tU32 : tI32), tH, val, false, rounding, &fl);
      const Zmm got = get_reg(m.cpu.state(), e.reg);
      const Zmm a = regs[e.vvvv];
      bool ok = got.h[0] == uint16_t(want);
      for (int i = 1; i < 8; ++i) ok = ok && got.h[i] == a.h[i];
      for (int i = 8; i < 32; ++i) ok = ok && got.h[i] == 0;
      if (!ok) { printf("MISMATCH %s w=%d val=%llx got %04x want %04x\n", f.name, e.w, (unsigned long long)val, got.h[0], uint16_t(want)); ++g_failed; return; }
    }
  }
}

// --- moves ---------------------------------------------------------------------------------------------------------------------------------

static void run_moves(int iterations) {
  for (int it = 0; it < iterations; ++it) {
    {      // vmovsh load / store / register merge
      Enc e;
      e.map = 5; e.pp = 2; e.vl = 128;
      const int form = int(rnd() % 3);      // 0 load, 1 register merge, 2 store
      e.reg = int(rnd() % 32); e.vvvv = form == 1 ? int(rnd() % 32) : 0; e.aaa = rnd() % 2 ? int(rnd() % 8) : 0; e.z = e.aaa && (rnd() & 1) && form != 2;
      if (form == 0) { e.op = 0x10; e.memory = true; e.disp = 12; e.disp8_factor = 2; }
      else if (form == 1) { e.op = 0x10; e.rm = int(rnd() % 32); }
      else { e.op = 0x11; e.memory = true; e.disp = -6; e.disp8_factor = 2; e.z = false; }
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint16_t mem_h[2] = {rnd_half(), 0};
      uint64_t kmask;
      prepare(m, regs, &kmask, e.aaa, nullptr, 0);
      m.poke(kBase + e.disp, mem_h, 2);
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      const Zmm got = get_reg(m.cpu.state(), e.reg);
      const bool active = e.aaa == 0 || (kmask & 1);
      if (form == 2) {
        uint16_t out;
        m.peek(kBase + e.disp, &out, 2);
        const uint16_t want = active ? regs[e.reg].h[0] : mem_h[0];
        if (out != want) { printf("MISMATCH vmovsh store got %04x want %04x\n", out, want); ++g_failed; return; }
      } else {
        Zmm want;
        memset(&want, 0, sizeof want);
        const uint16_t src = form == 0 ? mem_h[0] : regs[e.rm].h[0];
        if (form == 1) for (int i = 1; i < 8; ++i) want.h[i] = regs[e.vvvv].h[i];
        want.h[0] = active ? src : e.z ? 0 : regs[e.reg].h[0];
        if (memcmp(want.h, got.h, 64) != 0) { printf("MISMATCH vmovsh form %d aaa %d z %d\n", form, e.aaa, e.z); ++g_failed; return; }
      }
    }
    {      // vmovw
      Enc e;
      e.map = 5; e.vl = 128;
      const int form = int(rnd() % 5);
      e.reg = int(rnd() % (form == 3 ? 16 : 32));
      static const int gp[] = {0, 2, 3, 5, 6, 8, 9, 10, 12, 13, 14, 15};
      if (form == 0) { e.pp = 1; e.op = 0x6E; e.rm = gp[rnd() % 12]; }
      else if (form == 1) { e.pp = 1; e.op = 0x6E; e.memory = true; e.disp = 10; e.disp8_factor = 2; }
      else if (form == 2) { e.pp = 1; e.op = 0x7E; e.memory = true; e.disp = -4; e.disp8_factor = 2; }
      else if (form == 3) { e.pp = 1; e.op = 0x7E; e.rm = gp[rnd() % 12]; }
      else { e.pp = 2; e.op = 0x6E; e.rm = int(rnd() % 32); }
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, 0, nullptr, 0);
      uint16_t v = rnd_half();
      if (e.memory) m.poke(kBase + e.disp, &v, 2);
      if (form == 0) m.cpu.state().r[e.rm] = 0xFFFF0000ull | v | (rnd() << 32);
      m.cpu.state().r[e.reg & 15] = form == 3 ? 0xAAAAAAAAAAAAAAAAull : m.cpu.state().r[e.reg & 15];
      if (m.run() != StopReason::Syscall) { CHECK(false); return; }
      if (form == 0 || form == 1 || form == 4) {
        const uint16_t src = form == 4 ? regs[e.rm].h[0] : form == 0 ? uint16_t(m.cpu.state().r[e.rm]) : v;
        const Zmm got = get_reg(m.cpu.state(), e.reg);
        bool ok = got.h[0] == src;
        for (int i = 1; i < 32; ++i) ok = ok && got.h[i] == 0;
        if (!ok) { printf("MISMATCH vmovw form %d got %04x want %04x\n", form, got.h[0], src); ++g_failed; return; }
      } else if (form == 2) {
        uint16_t out;
        m.peek(kBase + e.disp, &out, 2);
        if (out != regs[e.reg].h[0]) { printf("MISMATCH vmovw store\n"); ++g_failed; return; }
      } else {
        if (m.cpu.state().r[e.rm] != regs[e.reg].h[0]) { printf("MISMATCH vmovw to gpr got %llx want %04x\n", (unsigned long long)m.cpu.state().r[e.rm], regs[e.reg].h[0]); ++g_failed; return; }
      }
    }
  }
}

// --- complex arithmetic ----------------------------------------------------------------------------------------------------------------

static void run_complex(int iterations) {
  struct Cx { const char* name; uint8_t pp, op; bool conj, accumulate, scalar; };
  static const Cx cxs[] = {{"vfmulcph", 2, 0xD6, false, false, false}, {"vfcmulcph", 3, 0xD6, true, false, false}, {"vfmulcsh", 2, 0xD7, false, false, true}, {"vfcmulcsh", 3, 0xD7, true, false, true},
                           {"vfmaddcph", 2, 0x56, false, true, false}, {"vfcmaddcph", 3, 0x56, true, true, false}, {"vfmaddcsh", 2, 0x57, false, true, true}, {"vfcmaddcsh", 3, 0x57, true, true, true}};
  for (const Cx& c : cxs) {
    for (int it = 0; it < iterations; ++it) {
      Enc e;
      e.map = 6; e.pp = c.pp; e.op = c.op;
      e.vl = c.scalar ? 128 : (rnd() % 3 == 0 ? 128 : rnd() % 2 ? 256 : 512);
      e.reg = int(rnd() % 32); e.vvvv = int(rnd() % 32);
      e.aaa = rnd() % 3 == 0 ? 0 : int(rnd() % 8);
      e.z = e.aaa && (rnd() & 1);
      const int kind = int(rnd() % 3);
      if (kind == 1 || (kind == 2 && c.scalar)) { e.memory = true; e.disp = 3 * (c.scalar ? 4 : e.vl / 8); e.disp8_factor = c.scalar ? 4 : e.vl / 8; }
      else if (kind == 2) { e.memory = true; e.bcst = true; e.disp = 8; e.disp8_factor = 4; }
      else { e.rm = int(rnd() % 32); }
      const bool overlap = e.reg == e.vvvv || (!e.memory && e.reg == e.rm);
      Machine m(with_syscall(encode(e)));
      Zmm regs[32];
      uint16_t mem_h[32];
      uint64_t kmask;
      prepare(m, regs, &kmask, e.aaa, mem_h, 0);
      for (int i = 0; i < 32; ++i) mem_h[i] = rnd_half();
      if (e.memory) m.poke(kBase + e.disp, mem_h, 64);
      const int mode = int(rnd() % 4);
      m.cpu.state().mxcsr = 0x1F80 | uint32_t(mode) << 13;
      const StopReason r = m.run();
      if (overlap) {
        if (r != StopReason::Fault || m.cpu.fault().vector != 6) { printf("MISMATCH %s: overlapping registers must be #UD\n", c.name); ++g_failed; return; }
        continue;
      }
      if (r != StopReason::Syscall) { printf("MISMATCH %s: did not run\n", c.name); ++g_failed; return; }
      const Zmm got = get_reg(m.cpu.state(), e.reg);
      const Zmm a = regs[e.vvvv], d = regs[e.reg];
      Zmm b;
      if (e.memory) for (int i = 0; i < 32; ++i) b.h[i] = e.bcst ? mem_h[i & 1] : mem_h[i]; else b = regs[e.rm];
      Zmm want;
      memset(&want, 0, sizeof want);
      if (c.scalar) for (int i = 2; i < 8; ++i) want.h[i] = a.h[i];
      const int n = c.scalar ? 1 : e.vl / 32;
      for (int i = 0; i < n; ++i) {
        const bool active = e.aaa == 0 || ((kmask >> i) & 1);
        if (!active) { want.h[2 * i] = e.z ? 0 : d.h[2 * i]; want.h[2 * i + 1] = e.z ? 0 : d.h[2 * i + 1]; continue; }
        const uint16_t ar = a.h[2 * i], ai = a.h[2 * i + 1], br = b.h[2 * i], bi = b.h[2 * i + 1], cr = c.accumulate ? d.h[2 * i] : 0, ci = c.accumulate ? d.h[2 * i + 1] : 0;
        uint32_t fl = 0;
        // real: ar*br -/+ ai*bi (+ cr); imaginary: ai*br +/- ar*bi (+ ci), as chained fused multiply-adds
        uint16_t t = c.accumulate ? exact_fma(ar, br, cr, mode, &fl) : exact_mul(ar, br, mode, &fl);
        const uint16_t re = exact_fma(c.conj ? ai : neg16(ai), bi, t, mode, &fl);
        t = c.accumulate ? exact_fma(ai, br, ci, mode, &fl) : exact_mul(ai, br, mode, &fl);
        const uint16_t im = exact_fma(c.conj ? neg16(ar) : ar, bi, t, mode, &fl);
        want.h[2 * i] = re;
        want.h[2 * i + 1] = im;
      }
      for (int i = 0; i < 32; ++i) {
        const bool nan_ok = is_nan(want.h[i]) && is_nan(got.h[i]);
        if (got.h[i] != want.h[i] && !nan_ok) { printf("MISMATCH %s elem %d got %04x want %04x (aaa %d z %d kind %d)\n", c.name, i, got.h[i], want.h[i], e.aaa, e.z, kind); ++g_failed; return; }
      }
    }
  }
}

// --- state and robustness ------------------------------------------------------------------------------------------------------------------

static void test_state() {
  {      // cpuid.(EAX=7,ECX=0):EDX bit 23
    Machine m(with_syscall({0xB8, 7, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2}));
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.cpu.state().r[2] >> 23) & 1);
  }
  {      // an EVEX map the emulator does not know (map 7) and a reserved bit are #UD
    Machine m(with_syscall({0x62, 0xF7, 0x7C, 0x48, 0x58, 0xC1}));
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 6);
    Machine m2(with_syscall({0x62, 0xFD, 0x7C, 0x48, 0x58, 0xC1}));      // bit 3 of P0 set: with APX it is B4, which a vector operand ignores
    CHECK(m2.run() == StopReason::Syscall);
  }
}

static void test_robustness() {
  int runs = 0;
  for (const Op& o : g_ops) {
    Enc e;
    e.map = o.map; e.pp = o.pp; e.op = o.opcode; e.w = o.w; e.vl = 512; e.reg = 3; e.vvvv = 4; e.memory = true; e.disp = 64; e.disp8_factor = 64; e.aaa = 2; e.imm = o.imm ? 5 : -1;
    const Bytes code = encode(e);
    for (int variant = 0; variant < 16; ++variant) {
      Bytes c = code;
      if (variant < int(code.size())) c.resize(variant);
      else c[rnd() % c.size()] ^= uint8_t(1u << (rnd() % 8));
      c.push_back(0x0F); c.push_back(0x05);
      Machine m(c);
      m.run(6);
      ++runs;
    }
  }
  CHECK(runs > 500);
}

int main() {
  const char* only = getenv("FP16_ONLY");
  const int iterations = getenv("FP16_ITER") ? atoi(getenv("FP16_ITER")) : 400;
  auto wanted = [&](const std::string& n) { return !only || n.find(only) != std::string::npos; };
  build_arith_ops();
  build_unary_ops();
  test_state();
  if (getenv("FP16_SCAN")) {
    for (const Op& o : g_ops) if (o.scalar && o.unary == false && wanted(o.name) && (o.name.find("rndscale") != std::string::npos || o.name.find("reduce") != std::string::npos || o.name.find("getmant") != std::string::npos || o.name.find("getexp") != std::string::npos || o.name.find("rcp") != std::string::npos || o.name.find("rsqrt") != std::string::npos || o.name.find("sqrt") != std::string::npos)) scan_unary(o, o.imm ? std::vector<int>{0x00, 0x04, 0x08, 0x0A, 0x10, 0x14, 0x1C, 0x21, 0x3D, 0x7F, 0xF3, 0xFC} : std::vector<int>{0});
    printf(g_failed ? "scan failures %d\n" : "scan clean\n", g_failed);
    return g_failed != 0;
  }
  for (const Op& o : g_ops) {
    if (!wanted(o.name)) continue;
    const int before = g_failed;
    run_elementwise(o, iterations);
    if (g_failed != before) printf("  ^ %s\n", o.name.c_str());
  }
  if (wanted("addsub") || wanted("subadd")) run_addsub(iterations / 4);
  if (wanted("vcmp")) run_compare(iterations);
  if (wanted("comi")) run_comi(iterations);
  if (wanted("fpclass")) run_fpclass(iterations);
  for (const Cv& c : kConversions) if (wanted(c.name)) { const int before = g_failed; run_conversion(c, iterations); if (g_failed != before) printf("  ^ %s\n", c.name); }
  for (const ScalarCv& c : kScalarConversions) if (wanted(c.name)) { const int before = g_failed; run_scalar_conversion(c, iterations); if (g_failed != before) printf("  ^ %s\n", c.name); }
  if (wanted("vcvtsh2si") || wanted("vcvtsi2sh") || wanted("usi")) run_gpr_conversions(iterations);
  if (wanted("vmov")) run_moves(iterations);
  if (wanted("fmulc") || wanted("fmaddc") || wanted("fcmulc") || wanted("fcmaddc")) run_complex(iterations / 2);
  if (!only) test_robustness();
  if (getenv("FP16_DUMP")) {      // the encodings the tests use, for checking against an assembler (see tests/data/emulation/fp16_check_encodings.py)
    for (const Op& o : g_ops) printf("FORM %s %d %d %02x %d\n", o.name.c_str(), o.map, o.pp, o.opcode, o.w);
    for (const Cv& c : kConversions) printf("FORM %s %d %d %02x %d\n", c.name, c.map, c.pp, c.opcode, c.w);
    for (const ScalarCv& c : kScalarConversions) printf("FORM %s %d %d %02x %d\n", c.name, c.map, c.pp, c.opcode, c.w);
    printf("FORM vcmpph 3 0 c2 0\nFORM vcmpsh 3 2 c2 0\nFORM vucomish 5 0 2e 0\nFORM vcomish 5 0 2f 0\nFORM vfpclassph 3 0 66 0\nFORM vfpclasssh 3 0 67 0\n");
    printf("FORM vmovsh_load 5 2 10 0\nFORM vmovsh_store 5 2 11 0\nFORM vmovw_to_xmm 5 1 6e 0\nFORM vmovw_from_xmm 5 1 7e 0\nFORM vmovw_xmm_xmm 5 2 6e 0\n");
    printf("FORM vfmulcph 6 2 d6 0\nFORM vfcmulcph 6 3 d6 0\nFORM vfmulcsh 6 2 d7 0\nFORM vfcmulcsh 6 3 d7 0\nFORM vfmaddcph 6 2 56 0\nFORM vfcmaddcph 6 3 56 0\nFORM vfmaddcsh 6 2 57 0\nFORM vfcmaddcsh 6 3 57 0\n");
    printf("FORM vcvtsh2si 5 2 2d 0\nFORM vcvttsh2si 5 2 2c 0\nFORM vcvtsh2usi 5 2 79 0\nFORM vcvttsh2usi 5 2 78 0\nFORM vcvtsi2sh 5 2 2a 0\nFORM vcvtusi2sh 5 2 7b 0\n");
  }
  if (g_failed) { printf("Fp16Test: %d failures\n", g_failed); return 1; }
  printf("Fp16Test: all passed\n");
  return 0;
}
