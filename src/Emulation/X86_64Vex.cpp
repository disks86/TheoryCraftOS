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
#include "Emulation/X86_64Cpu.hpp"
#include "Emulation/HalfFloat.hpp"
#include "Emulation/SoftFloat.hpp"
#include "Emulation/Wide.hpp"

// AVX, AVX2, FMA3, BMI1/BMI2, F16C and the other things reached through the VEX prefix (C4 / C5).
//
// The SSE code in X86_64Cpu.cpp does the arithmetic. A VEX instruction on 128-bit lanes is "the SSE instruction on each 128-bit half, with three operands": vex_simd() copies the
// first source into the destination register, the second source into a scratch register (slot 16) and runs the SSE implementation once per half. AVX and AVX2 operations that
// work within 128-bit lanes (all the arithmetic, unpacks, packs, in-lane shuffles) are exactly that. What is new (permutes across lanes, broadcasts, blends, variable shifts,
// the SSSE3/SSE4.1 integer instructions, FMA, gathers, BMI) is written out below.
namespace emulation {

using namespace softfloat;

namespace {

typedef X86_64Cpu::Xmm Xmm;


constexpr uint32_t kNds = 1;         // the destination starts as the first source (VEX.vvvv)
constexpr uint32_t kRmXmm = 2;       // the r/m operand is an XMM register or memory (a register is handed to the SSE code in the scratch slot)
constexpr uint32_t kRegGpr = 4;      // the reg field names a general register that is written
constexpr uint32_t kRmDest = 8;      // the r/m operand is the destination (a store, or register to register)
constexpr uint32_t kRmGpr = 16;      // the r/m operand is a general register (or memory) holding a scalar
constexpr uint32_t kSingle = 32;     // one 128-bit half only, whatever VEX.L says
constexpr uint32_t kNoDest = 64;     // no register result (compares that set the flags)
constexpr uint32_t kUnsupported = 0x8000;


// How each 0F-map opcode (with its prefix) is carried out by the SSE code.
uint32_t classify_map1(uint8_t op, uint32_t pp, uint32_t mod) {
  const bool scalar = pp >= 2;      // F3 or F2
  switch (op) {
    case 0x10: return (scalar && mod == 3) ? (kNds | kRmXmm | kSingle) : (kRmXmm | (scalar ? kSingle : 0));
    case 0x11: return kRmDest | kRmXmm | (scalar ? kSingle : 0);
    case 0x12: case 0x16: return pp >= 2 ? kRmXmm : (kNds | kRmXmm | kSingle);      // (vmovddup, vmovsldup, vmovshdup work on whole lanes)
    case 0x7C: case 0x7D: case 0xD0: return kNds | kRmXmm;      // vhaddps/pd, vhsubps/pd, vaddsubps/pd
    case 0x13: case 0x17: return kRmDest | kRmXmm | kSingle;
    case 0x14: case 0x15: case 0x54: case 0x55: case 0x56: case 0x57: return kNds | kRmXmm;
    case 0x28: return kRmXmm;
    case 0x29: case 0x2B: return kRmDest | kRmXmm;
    case 0x2A: return kNds | kRmGpr | kSingle;
    case 0x2C: case 0x2D: return kRegGpr | kRmXmm | kSingle;
    case 0x2E: case 0x2F: return kRmXmm | kSingle | kNoDest;
    case 0x51: return scalar ? (kNds | kRmXmm | kSingle) : kRmXmm;
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: return kNds | kRmXmm | (scalar ? kSingle : 0);
    case 0x5A: return scalar ? (kNds | kRmXmm | kSingle) : kUnsupported;      // (the packed conversions change the width: handled apart)
    case 0x5B: return kRmXmm;
    case 0x6E: return kRmGpr | kSingle;
    case 0x6F: case 0x70: return kRmXmm;
    case 0x74: case 0x75: case 0x76: return kNds | kRmXmm;
    case 0x7E: return pp == 2 ? (kRmXmm | kSingle) : (kRmDest | kRmGpr | kSingle);
    case 0x7F: return kRmDest | kRmXmm;
    case 0xC2: return kNds | kRmXmm | (scalar ? kSingle : 0);
    case 0xC4: return kNds | kRmGpr | kSingle;
    case 0xC5: return kRegGpr | kRmXmm | kSingle;
    case 0xC6: return kNds | kRmXmm;
    case 0xD6: return kRmDest | kRmXmm | kSingle;
    case 0xE7: return kRmDest | kRmXmm;
    default: break;
  }
  if ((op >= 0x60 && op <= 0x6D) || (op >= 0xD1 && op <= 0xD5) || (op >= 0xD8 && op <= 0xDF) || (op >= 0xE0 && op <= 0xE5) || (op >= 0xE8 && op <= 0xEF) ||
      (op >= 0xF1 && op <= 0xF6) || (op >= 0xF8 && op <= 0xFE)) return kNds | kRmXmm;
  return kUnsupported;
}

uint8_t byte_of(const Xmm& x, uint32_t i) { return static_cast<uint8_t>(lane_get(x.lo, x.hi, 8, i)); }
void set_byte(Xmm* x, uint32_t i, uint64_t v) { lane_set(&x->lo, &x->hi, 8, i, v); }
int64_t sx(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

using half::half_to_single;
using half::single_to_half;

}  // namespace

// --- decoding the prefix and handing out -----------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::step_vex(GuestMemory& m, uint8_t first) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (d_.operand16 || d_.rep || d_.repne || d_.rex_present) return fail(6);      // (a VEX prefix after a legacy prefix or REX is invalid)
  const uint8_t b1 = next_u8();
  uint32_t pp, map;
  bool wide;
  uint32_t vvvv;
  d_.rex_present = true;
  if (first == 0xC5) {
    d_.rex_r = !(b1 & 0x80);
    vvvv = (~(b1 >> 3)) & 15;
    wide = (b1 >> 2) & 1;
    pp = b1 & 3;
    map = 1;
    d_.rex_w = false;
  } else {
    const uint8_t b2 = next_u8();
    d_.rex_r = !(b1 & 0x80);
    d_.rex_x = !(b1 & 0x40);
    d_.rex_b = !(b1 & 0x20);
    map = b1 & 31;
    d_.rex_w = (b2 >> 7) & 1;
    vvvv = (~(b2 >> 3)) & 15;
    wide = (b2 >> 2) & 1;
    pp = b2 & 3;
  }
  d_.operand16 = pp == 1;
  d_.rep = pp == 2;
  d_.repne = pp == 3;
  const uint8_t op = next_u8();
  if (d_.truncated) return bad_decode();
  vex_.active = true;
  vex_.wide = wide;
  vex_.vvvv = vvvv;
  switch (map) {
    case 1: {
      if (op == 0x77) {      // vzeroupper, vzeroall
        if (!finish_decode()) return bad_decode();
        for (uint32_t i = 0; i < 16; ++i) {
          s_.ymm_high[i] = Xmm();
          if (wide) s_.xmm[i] = Xmm();
        }
        s_.rip += d_.position;
        return Step::Ok;
      }
      if (op == 0x41 || op == 0x42 || op == 0x44 || op == 0x45 || op == 0x46 || op == 0x47 || op == 0x4A || op == 0x4B || (op >= 0x90 && op <= 0x93) || op == 0x98 || op == 0x99) return kmask_instruction(m, 1, op, d_.rex_w || false, vvvv);
      return vex_simd(m, op, 0, wide, vvvv);
    }
    case 2: return vex_map2(m, op, d_.rex_w, vvvv);
    case 3:
      if (op >= 0x30 && op <= 0x33) return kmask_instruction(m, 3, op, d_.rex_w, vvvv);
      return vex_map3(m, op, d_.rex_w, vvvv);
    default: return fail(6);
  }
}

bool X86_64Cpu::read_y(GuestMemory& m, uint32_t bytes, Xmm out[2]) {
  if (d_.mod == 3) {
    out[0] = s_.xmm[d_.rm];
    out[1] = s_.ymm_high[d_.rm & 15];
    return true;
  }
  const uint64_t a = memory_address();
  out[1] = Xmm();
  return load(m, a, 8, &out[0].lo) && load(m, a + 8, 8, &out[0].hi) && (bytes < 32 || (load(m, a + 16, 8, &out[1].lo) && load(m, a + 24, 8, &out[1].hi)));
}

X86_64Cpu::Step X86_64Cpu::vex_simd(GuestMemory& m, uint8_t op, uint32_t, bool wide, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  if (d_.position >= d_.available) return bad_decode();
  const uint8_t modrm = d_.bytes[d_.position];
  const uint32_t mod = modrm >> 6;
  const uint32_t reg = ((modrm >> 3) & 7) | (d_.rex_r ? 8 : 0), rm_real = (modrm & 7) | (d_.rex_b ? 8 : 0);
  if (op == 0xF0 && pp == 3) op = 0x6F, d_.rep = true, d_.repne = false;      // lddqu is an unaligned load
  const uint32_t pp2 = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;

  // --- instructions that do not fit the half-by-half scheme
  if (op == 0x50 || op == 0xD7) {      // movmskps/pd, pmovmskb: a mask of the sign bits of both halves
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    if (d_.mod != 3) return fail(6);
    const Xmm lo = s_.xmm[d_.rm], hi = s_.ymm_high[d_.rm & 15];
    const uint32_t width = op == 0xD7 ? 8 : (pp2 == 1 ? 64 : 32);
    auto mask_of = [&](const Xmm& x) { uint32_t r = 0; for (uint32_t i = 0; i < 128 / width; ++i) r |= static_cast<uint32_t>((lane_get(x.lo, x.hi, width, i) >> (width - 1)) & 1) << i; return r; };
    uint64_t mask = mask_of(lo);
    if (wide) mask |= static_cast<uint64_t>(mask_of(hi)) << (128 / width);
    set_reg(d_.reg, 4, mask);
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (op == 0x71 || op == 0x72 || op == 0x73) {      // shifts by an immediate: the destination is VEX.vvvv, the source the r/m register
    if (mod != 3 || pp2 != 1) return fail(6);
    Xmm src[2] = {s_.xmm[rm_real], s_.ymm_high[rm_real]}, out[2];
    const uint32_t start = d_.position;
    const uint64_t start_rip = s_.rip;
    const Xmm saved = s_.xmm[16];
    for (uint32_t h = 0; h < (wide ? 2u : 1u); ++h) {
      d_.position = start;
      vex_.redirect = true;
      s_.xmm[16] = src[h];
      const Step r = step_sse(m, op);
      if (r != Step::Ok) { s_.xmm[16] = saved; vex_.redirect = false; return r; }
      out[h] = s_.xmm[16];
      if (h == 0 && wide) s_.rip = start_rip;
    }
    s_.xmm[16] = saved;
    vex_.redirect = false;
    s_.xmm[vvvv] = out[0];
    s_.ymm_high[vvvv] = wide ? out[1] : Xmm();
    return Step::Ok;
  }
  if ((op == 0x5A && pp2 <= 1) || (op == 0xE6) || (op == 0x5B && false)) {      // conversions that change the width: vcvtps2pd, vcvtpd2ps, vcvtdq2pd, vcvt(t)pd2dq
    if (op == 0xE6 && pp2 == 0) return fail(6);
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    Env env;
    env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
    Xmm in[2];
    const bool widening = (op == 0x5A && pp2 == 0) || (op == 0xE6 && pp2 == 2);      // 128-bit source, up to 256-bit result
    if (!read_y(m, widening ? 16 : (wide ? 32 : 16), in)) return Step::Fault;
    Xmm out[2];
    if (widening) {
      const uint32_t n = wide ? 4 : 2;
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t r;
        if (op == 0x5A) r = convert(Format::F32, Format::F64, lane_get(in[0].lo, in[0].hi, 32, i), env);
        else r = from_int(Format::F64, sx(lane_get(in[0].lo, in[0].hi, 32, i), 32), env);
        Xmm& o = out[i / 2];
        lane_set(&o.lo, &o.hi, 64, i % 2, r);
      }
    } else {      // pd -> ps / dq: 2 or 4 doubles to 2 or 4 32-bit values in a 128-bit register
      const bool truncate = op == 0xE6 && pp2 == 1;
      const uint32_t n = wide ? 4 : 2;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t v = lane_get(in[i / 2].lo, in[i / 2].hi, 64, i % 2);
        const uint64_t r = op == 0x5A ? convert(Format::F64, Format::F32, v, env) : static_cast<uint64_t>(to_int(Format::F64, v, 32, truncate, env));
        lane_set(&out[0].lo, &out[0].hi, 32, i, r);
      }
    }
    s_.mxcsr |= env.flags & ~(op == 0xE6 ? kDenormal : 0u);      // (conversions to integers do not report denormal operands)
    s_.xmm[d_.reg] = out[0];
    s_.ymm_high[d_.reg] = widening && wide ? out[1] : Xmm();
    s_.rip += d_.position;
    return Step::Ok;
  }
  uint32_t flags = classify_map1(op, pp2, mod);
  if (flags == kUnsupported) return fail(6);

  if (op == 0x11 && (pp2 >= 2) && mod == 3) {      // vmovss/vmovsd xmm1(r/m), xmm2(vvvv), xmm3(reg): the low element of reg, the rest from vvvv
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    Xmm r = s_.xmm[vvvv];
    if (pp2 == 2) lane_set(&r.lo, &r.hi, 32, 0, lane_get(s_.xmm[d_.reg].lo, s_.xmm[d_.reg].hi, 32, 0));
    else r.lo = s_.xmm[d_.reg].lo;
    s_.xmm[d_.rm & 15] = r;
    s_.ymm_high[d_.rm & 15] = Xmm();
    s_.rip += d_.position;
    return Step::Ok;
  }

  const uint32_t halves = (wide && !(flags & kSingle)) ? 2 : 1;
  const Xmm d0[2] = {s_.xmm[reg], s_.ymm_high[reg]};
  const Xmm s1[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  // a shift by a count in an XMM register or 128 bits of memory: both halves use the same count
  const bool count_operand = (op >= 0xD1 && op <= 0xD3) || op == 0xE1 || op == 0xE2 || (op >= 0xF1 && op <= 0xF3);
  const Xmm s2[2] = {s_.xmm[rm_real], count_operand ? s_.xmm[rm_real] : s_.ymm_high[rm_real]};
  const uint32_t start = d_.position;
  const uint64_t start_rip = s_.rip;
  Xmm result[2];
  uint32_t last_byte = 0;
  for (uint32_t h = 0; h < halves; ++h) {
    d_.position = start;
    vex_.redirect = (flags & kRmXmm) && mod == 3;
    vex_.half = count_operand ? 0 : h;
    s_.xmm[16] = s2[h];
    if (!(flags & kRegGpr)) s_.xmm[reg] = (flags & kNds) ? s1[h] : d0[h];
    uint8_t imm_saved = 0;
    uint32_t imm_at = 0;
    if (op == 0xC6 && pp2 == 1 && h == 1) {      // vshufpd: the second lane uses imm8 bits 3:2

      imm_at = last_byte;
      imm_saved = d_.bytes[imm_at];
      d_.bytes[imm_at] = static_cast<uint8_t>(imm_saved >> 2);
    }
    const Step r = step_sse(m, op);
    if (imm_at) d_.bytes[imm_at] = imm_saved;
    if (r != Step::Ok) {
      s_.xmm[reg] = d0[0];
      vex_ = VexState();
      return r;
    }
    result[h] = (flags & kRmDest) ? s_.xmm[16] : s_.xmm[reg];
    if (h == 0) last_byte = d_.position - 1;
    if (h + 1 < halves) s_.rip = start_rip;
  }
  vex_ = VexState();
  s_.xmm[reg] = d0[0];
  if (flags & kRmDest) {
    if (mod == 3 && !(flags & kRmGpr)) {
      s_.xmm[rm_real] = result[0];
      s_.ymm_high[rm_real] = halves == 2 ? result[1] : Xmm();
    }
  } else if (!(flags & (kRegGpr | kNoDest))) {
    s_.xmm[reg] = result[0];
    s_.ymm_high[reg] = halves == 2 ? result[1] : Xmm();
  }
  return Step::Ok;
}


// --- helpers for the instructions that exist only with a VEX prefix -----------------------------------------------------------------------------

namespace {

uint64_t el(const Xmm& x, uint32_t w, uint32_t i) { return lane_get(x.lo, x.hi, w, i); }
void put_el(Xmm* x, uint32_t w, uint32_t i, uint64_t v) { lane_set(&x->lo, &x->hi, w, i, v); }
int64_t sat_s(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

Xmm pshufb(const Xmm& a, const Xmm& b) {
  Xmm r;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint8_t sel = byte_of(b, i);
    set_byte(&r, i, (sel & 0x80) ? 0 : byte_of(a, sel & 15));
  }
  return r;
}

Xmm horizontal(const Xmm& a, const Xmm& b, uint32_t w, bool subtract, bool saturate) {
  Xmm r;
  const uint32_t n = 128 / w;
  for (uint32_t i = 0; i < n / 2; ++i) {
    for (uint32_t k = 0; k < 2; ++k) {
      const Xmm& src = k == 0 ? a : b;
      const int64_t x = sx(el(src, w, 2 * i), w), y = sx(el(src, w, 2 * i + 1), w);
      int64_t v = subtract ? x - y : x + y;
      if (saturate) v = sat_s(v, -32768, 32767);
      put_el(&r, w, k * (n / 2) + i, static_cast<uint64_t>(v));
    }
  }
  return r;
}

Xmm maddubsw(const Xmm& a, const Xmm& b) {
  Xmm r;
  for (uint32_t i = 0; i < 8; ++i) {
    const int64_t v = static_cast<int64_t>(byte_of(a, 2 * i)) * static_cast<int8_t>(byte_of(b, 2 * i)) + static_cast<int64_t>(byte_of(a, 2 * i + 1)) * static_cast<int8_t>(byte_of(b, 2 * i + 1));
    put_el(&r, 16, i, static_cast<uint64_t>(sat_s(v, -32768, 32767)));
  }
  return r;
}

Xmm minmax(const Xmm& a, const Xmm& b, uint32_t w, bool is_signed, bool want_max) {
  Xmm r;
  for (uint32_t i = 0; i < 128 / w; ++i) {
    const uint64_t x = el(a, w, i), y = el(b, w, i);
    const bool x_greater = is_signed ? sx(x, w) > sx(y, w) : x > y;
    put_el(&r, w, i, (want_max ? x_greater : !x_greater && x != y) ? x : y);
  }
  return r;
}

Xmm variable_shift(const Xmm& a, const Xmm& b, uint32_t w, uint32_t kind) {      // 0 left, 1 right logical, 2 right arithmetic
  Xmm r;
  for (uint32_t i = 0; i < 128 / w; ++i) {
    const uint64_t x = el(a, w, i), count = el(b, w, i);
    uint64_t v;
    if (kind == 2) v = static_cast<uint64_t>(sx(x, w) >> (count >= w ? w - 1 : count));
    else if (count >= w) v = 0;
    else v = kind == 0 ? x << count : x >> count;
    put_el(&r, w, i, v);
  }
  return r;
}

Xmm abs_of(const Xmm& b, uint32_t w) {
  Xmm r;
  for (uint32_t i = 0; i < 128 / w; ++i) {
    const int64_t v = sx(el(b, w, i), w);
    put_el(&r, w, i, static_cast<uint64_t>(v < 0 ? -v : v));
  }
  return r;
}

Xmm packusdw(const Xmm& a, const Xmm& b) {
  Xmm r;
  for (uint32_t i = 0; i < 8; ++i) {
    const int64_t v = sx(el(i < 4 ? a : b, 32, i & 3), 32);
    put_el(&r, 16, i, static_cast<uint64_t>(sat_s(v, 0, 65535)));
  }
  return r;
}

Xmm alignr(const Xmm& a, const Xmm& b, uint32_t imm) {      // (a:b) >> imm bytes
  Xmm r;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t idx = i + imm;
    set_byte(&r, i, idx < 16 ? byte_of(b, idx) : idx < 32 ? byte_of(a, idx - 16) : 0);
  }
  return r;
}

Xmm phminposuw(const Xmm& b) {
  uint64_t best = el(b, 16, 0);
  uint32_t index = 0;
  for (uint32_t i = 1; i < 8; ++i)
    if (el(b, 16, i) < best) best = el(b, 16, i), index = i;
  Xmm r;
  put_el(&r, 16, 0, best);
  put_el(&r, 16, 1, index);
  return r;
}

Xmm mpsadbw(const Xmm& a, const Xmm& b, uint32_t imm) {
  const uint32_t from_a = ((imm >> 2) & 1) * 4, from_b = (imm & 3) * 4;
  Xmm r;
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t sum = 0;
    for (uint32_t j = 0; j < 4; ++j) {
      const int32_t d = static_cast<int32_t>(byte_of(a, from_a + i + j)) - static_cast<int32_t>(byte_of(b, from_b + j));
      sum += static_cast<uint32_t>(d < 0 ? -d : d);
    }
    put_el(&r, 16, i, sum);
  }
  return r;
}

// pcmpestri / pcmpestrm / pcmpistri / pcmpistrm: the aggregation of two strings of bytes or words (SDM, "Intel SSE4.2 string and text processing").
// la and lb are the lengths of the first (register) and second (r/m) operand, in elements, at most 16 or 8. Returns IntRes2; *result_flags gets 1 CF, 2 ZF, 4 SF, 8 OF.
uint32_t string_compare(const Xmm& first, const Xmm& second, uint32_t la, uint32_t lb, uint32_t imm, uint32_t* result_flags) {
  const uint32_t w = (imm & 1) ? 16 : 8, n = 128 / w;
  const bool is_signed = (imm & 2) != 0;
  const uint32_t mode = (imm >> 2) & 3, polarity = (imm >> 4) & 3;
  auto value = [&](const Xmm& x, uint32_t i) { const uint64_t v = el(x, w, i); return is_signed ? sx(v, w) : static_cast<int64_t>(v); };
  // BoolRes[i][j]: element i of the first operand against element j of the second, with the rules for invalid (past the end) elements per mode
  auto bool_res = [&](uint32_t i, uint32_t j) {
    const bool va = i < la, vb = j < lb;
    switch (mode) {
      case 0: return va && vb && value(first, i) == value(second, j);
      case 1: {
        if (!va || !vb) return false;
        return (i & 1) ? value(second, j) <= value(first, i) : value(second, j) >= value(first, i);
      }
      case 2: return (!va && !vb) ? true : (va && vb && value(first, i) == value(second, j));
      default: return !va ? true : !vb ? false : value(first, i) == value(second, j);
    }
  };
  uint32_t res1 = 0;
  switch (mode) {
    case 0: for (uint32_t j = 0; j < n; ++j) { bool any = false; for (uint32_t i = 0; i < n; ++i) any |= bool_res(i, j); res1 |= static_cast<uint32_t>(any) << j; } break;
    case 1: for (uint32_t j = 0; j < n; ++j) { bool any = false; for (uint32_t i = 0; i + 1 < n; i += 2) any |= bool_res(i, j) && bool_res(i + 1, j); res1 |= static_cast<uint32_t>(any) << j; } break;
    case 2: for (uint32_t j = 0; j < n; ++j) res1 |= static_cast<uint32_t>(bool_res(j, j)) << j; break;
    default:
      for (uint32_t j = 0; j < n; ++j) {
        bool all = true;
        for (uint32_t i = 0, k = j; i + j < n; ++i, ++k) all &= bool_res(i, k);
        res1 |= static_cast<uint32_t>(all) << j;
      }
      break;
  }
  const uint32_t full = n == 16 ? 0xFFFFu : 0xFFu;
  uint32_t res2 = res1;
  if (polarity == 1) res2 = ~res1 & full;
  else if (polarity == 3) for (uint32_t j = 0; j < lb; ++j) res2 ^= 1u << j;
  *result_flags = (res2 != 0 ? 1u : 0u) | (lb < n ? 2u : 0u) | (la < n ? 4u : 0u) | ((res2 & 1) ? 8u : 0u);
  return res2;
}

uint32_t crc32c_step(uint32_t crc, uint64_t value, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes; ++i) {
    crc ^= static_cast<uint32_t>((value >> (8 * i)) & 0xFF);
    for (uint32_t k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78u : 0);
  }
  return crc;
}

}  // namespace

// --- the 0F38 map ---------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::vex_map2(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const bool wide = vex_.wide;
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  if (op >= 0x90 && op <= 0x93) return vex_gather(m, op, wide_w, vvvv);
  if ((op >= 0x96 && op <= 0x9F) || (op >= 0xA6 && op <= 0xAF) || (op >= 0xB6 && op <= 0xBF)) return vex_fma(m, op, wide_w, vvvv);
  if (op == 0xF2 || op == 0xF3 || op == 0xF5 || op == 0xF6 || op == 0xF7) return vex_bmi(m, 2, op, wide_w, vvvv);
  if ((op >= 0xDB && op <= 0xDF) || op == 0xCF || op == 0xCB || op == 0xCC || op == 0xCD || op == 0xDA) return crypto_map(m, 2, op, wide_w, vvvv);
  if (pp != 1) return fail(6);
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t reg = d_.reg;
  const uint32_t halves = wide ? 2 : 1;
  const Xmm a[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  Xmm b[2], out[2];
  auto finish_dest = [&]() {
    s_.xmm[reg] = out[0];
    s_.ymm_high[reg] = wide ? out[1] : Xmm();
    s_.rip += d_.position;
    return Step::Ok;
  };
  auto per_lane = [&](auto fn) -> Step {
    if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
    for (uint32_t h = 0; h < halves; ++h) out[h] = fn(a[h], b[h]);
    return finish_dest();
  };
  switch (op) {
    case 0x00: return per_lane([](const Xmm& x, const Xmm& y) { return pshufb(x, y); });
    case 0x01: return per_lane([](const Xmm& x, const Xmm& y) { return horizontal(x, y, 16, false, false); });
    case 0x02: return per_lane([](const Xmm& x, const Xmm& y) { return horizontal(x, y, 32, false, false); });
    case 0x03: return per_lane([](const Xmm& x, const Xmm& y) { return horizontal(x, y, 16, false, true); });
    case 0x05: return per_lane([](const Xmm& x, const Xmm& y) { return horizontal(x, y, 16, true, false); });
    case 0x06: return per_lane([](const Xmm& x, const Xmm& y) { return horizontal(x, y, 32, true, false); });
    case 0x07: return per_lane([](const Xmm& x, const Xmm& y) { return horizontal(x, y, 16, true, true); });
    case 0x04: return per_lane([](const Xmm& x, const Xmm& y) { return maddubsw(x, y); });
    case 0x08: case 0x09: case 0x0A: {      // psignb/w/d
      const uint32_t w = 8u << (op - 0x08);
      return per_lane([w](const Xmm& x, const Xmm& y) {
        Xmm r;
        for (uint32_t i = 0; i < 128 / w; ++i) {
          const int64_t s = sx(el(y, w, i), w);
          put_el(&r, w, i, s < 0 ? static_cast<uint64_t>(-sx(el(x, w, i), w)) : s == 0 ? 0 : el(x, w, i));
        }
        return r;
      });
    }
    case 0x0B: return per_lane([](const Xmm& x, const Xmm& y) {
      Xmm r;
      for (uint32_t i = 0; i < 8; ++i) put_el(&r, 16, i, static_cast<uint64_t>(((sx(el(x, 16, i), 16) * sx(el(y, 16, i), 16) >> 14) + 1) >> 1));
      return r;
    });
    case 0x0C: return per_lane([](const Xmm& x, const Xmm& y) {      // vpermilps (variable)
      Xmm r;
      for (uint32_t i = 0; i < 4; ++i) put_el(&r, 32, i, el(x, 32, el(y, 32, i) & 3));
      return r;
    });
    case 0x0D: return per_lane([](const Xmm& x, const Xmm& y) {      // vpermilpd (variable)
      Xmm r;
      for (uint32_t i = 0; i < 2; ++i) put_el(&r, 64, i, el(x, 64, (el(y, 64, i) >> 1) & 1));
      return r;
    });
    case 0x0E: case 0x0F: {      // vtestps / vtestpd
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      const Xmm x[2] = {s_.xmm[reg], s_.ymm_high[reg]};
      const uint32_t w = op == 0x0E ? 32 : 64;
      bool zf = true, cf = true;
      for (uint32_t h = 0; h < halves; ++h)
        for (uint32_t i = 0; i < 128 / w; ++i) {
          const bool xs = (el(x[h], w, i) >> (w - 1)) & 1, ys = (el(b[h], w, i) >> (w - 1)) & 1;
          if (xs && ys) zf = false;
          if (!xs && ys) cf = false;
        }
      s_.rflags = (s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kSign | kAdjust)) | (zf ? kZero : 0) | (cf ? kCarry : 0);
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x17: {      // vptest
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      const Xmm x[2] = {s_.xmm[reg], s_.ymm_high[reg]};
      uint64_t both = 0, notx = 0;
      for (uint32_t h = 0; h < halves; ++h) { both |= (x[h].lo & b[h].lo) | (x[h].hi & b[h].hi); notx |= (~x[h].lo & b[h].lo) | (~x[h].hi & b[h].hi); }
      s_.rflags = (s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kSign | kAdjust)) | (both == 0 ? kZero : 0) | (notx == 0 ? kCarry : 0);
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x16: case 0x36: {      // vpermps, vpermd: the index is the first source, the table the r/m operand (a whole ymm)
      if (!read_y(m, 32, b)) return Step::Fault;
      for (uint32_t h = 0; h < 2; ++h)
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t k = static_cast<uint32_t>(el(a[h], 32, i)) & 7;
          put_el(&out[h], 32, i, el(b[k / 4], 32, k % 4));
        }
      return finish_dest();
    }
    case 0x13: {      // vcvtph2ps: 4 or 8 halves to singles
      Xmm src[2];
      if (!read_y(m, wide ? 16 : 8, src)) return Step::Fault;
      for (uint32_t i = 0; i < (wide ? 8u : 4u); ++i) {
        const uint16_t h = static_cast<uint16_t>(el(src[0], 16, i));
        if ((h & 0x7C00) == 0x7C00 && (h & 0x3FF) && !(h & 0x200)) s_.mxcsr |= kInvalid;      // a signaling NaN
        put_el(&out[i / 4], 32, i % 4, half_to_single(h));
      }
      return finish_dest();
    }
    case 0x1C: case 0x1D: case 0x1E: {
      const uint32_t w = 8u << (op - 0x1C);
      return per_lane([w](const Xmm&, const Xmm& y) { return abs_of(y, w); });
    }
    case 0x41: {      // phminposuw: 128-bit only
      if (wide) return fail(6);
      return per_lane([](const Xmm&, const Xmm& y) { return phminposuw(y); });
    }
    case 0x28: return per_lane([](const Xmm& x, const Xmm& y) { Xmm r; for (uint32_t i = 0; i < 2; ++i) put_el(&r, 64, i, static_cast<uint64_t>(sx(el(x, 32, 2 * i), 32) * sx(el(y, 32, 2 * i), 32))); return r; });
    case 0x29: return per_lane([](const Xmm& x, const Xmm& y) { Xmm r; for (uint32_t i = 0; i < 2; ++i) put_el(&r, 64, i, el(x, 64, i) == el(y, 64, i) ? ~uint64_t{0} : 0); return r; });
    case 0x37: return per_lane([](const Xmm& x, const Xmm& y) { Xmm r; for (uint32_t i = 0; i < 2; ++i) put_el(&r, 64, i, sx(el(x, 64, i), 64) > sx(el(y, 64, i), 64) ? ~uint64_t{0} : 0); return r; });
    case 0x2B: return per_lane([](const Xmm& x, const Xmm& y) { return packusdw(x, y); });
    case 0x38: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 8, true, false); });
    case 0x39: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 32, true, false); });
    case 0x3A: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 16, false, false); });
    case 0x3B: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 32, false, false); });
    case 0x3C: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 8, true, true); });
    case 0x3D: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 32, true, true); });
    case 0x3E: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 16, false, true); });
    case 0x3F: return per_lane([](const Xmm& x, const Xmm& y) { return minmax(x, y, 32, false, true); });
    case 0x40: return per_lane([](const Xmm& x, const Xmm& y) { Xmm r; for (uint32_t i = 0; i < 4; ++i) put_el(&r, 32, i, el(x, 32, i) * el(y, 32, i)); return r; });
    case 0x45: { const uint32_t w = wide_w ? 64 : 32; return per_lane([w](const Xmm& x, const Xmm& y) { return variable_shift(x, y, w, 1); }); }
    case 0x46: return per_lane([](const Xmm& x, const Xmm& y) { return variable_shift(x, y, 32, 2); });
    case 0x47: { const uint32_t w = wide_w ? 64 : 32; return per_lane([w](const Xmm& x, const Xmm& y) { return variable_shift(x, y, w, 0); }); }
    case 0x18: case 0x19: case 0x1A: case 0x58: case 0x59: case 0x5A: case 0x78: case 0x79: {      // broadcasts from memory or the low element of a register
      const uint32_t bits = (op == 0x18 || op == 0x58) ? 32 : (op == 0x19 || op == 0x59) ? 64 : op == 0x78 ? 8 : op == 0x79 ? 16 : 128;
      Xmm src[2];
      if (!read_y(m, bits / 8 < 8 ? 8 : bits / 8, src)) return Step::Fault;
      if (d_.mod != 3 && bits < 64) {      // a memory broadcast of a small element reads only that element
        uint64_t v;
        if (!load(m, memory_address(), bits / 8, &v)) return Step::Fault;
        src[0] = Xmm();
        src[0].lo = v;
      }
      for (uint32_t h = 0; h < halves; ++h) {
        if (bits == 128) { out[h] = src[0]; continue; }
        const uint64_t v = el(src[0], bits, 0);
        for (uint32_t i = 0; i < 128 / bits; ++i) put_el(&out[h], bits, i, v);
      }
      return finish_dest();
    }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {      // vpmovsx / vpmovzx
      const uint32_t kind = op & 7;      // 0 bw, 1 bd, 2 bq, 3 wd, 4 wq, 5 dq
      static const uint32_t from[6] = {8, 8, 8, 16, 16, 32}, to[6] = {16, 32, 64, 32, 64, 64};
      const bool is_signed = op < 0x30;
      const uint32_t n = (wide ? 256 : 128) / to[kind];
      Xmm src[2];
      if (!read_y(m, (n * from[kind] + 7) / 8 < 8 ? 8 : (n * from[kind] + 7) / 8, src)) return Step::Fault;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t v = el(src[0], from[kind], i);
        put_el(&out[i / (128 / to[kind])], to[kind], i % (128 / to[kind]), is_signed ? static_cast<uint64_t>(sx(v, from[kind])) : v);
      }
      return finish_dest();
    }
    case 0x2A: {      // vmovntdqa
      if (d_.mod == 3) return fail(6);
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      out[0] = b[0]; out[1] = b[1];
      return finish_dest();
    }
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x8C: case 0x8E: {      // masked moves: elements whose mask sign bit is clear are neither read nor written
      if (d_.mod == 3) return fail(6);
      const uint32_t w = (op <= 0x2F) ? ((op & 1) ? 64 : 32) : (wide_w ? 64 : 32);
      const bool store_form = op == 0x2E || op == 0x2F || op == 0x8E;
      const uint64_t base = memory_address();
      Xmm mask[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
      Xmm data[2] = {s_.xmm[reg], s_.ymm_high[reg]};
      Xmm loaded[2];
      for (uint32_t h = 0; h < halves; ++h)
        for (uint32_t i = 0; i < 128 / w; ++i) {
          if (!((el(mask[h], w, i) >> (w - 1)) & 1)) continue;
          const uint64_t addr = base + (h * 128 + i * w) / 8;
          if (store_form) { if (!store(m, addr, w / 8, el(data[h], w, i))) return Step::Fault; }
          else { uint64_t v; if (!load(m, addr, w / 8, &v)) return Step::Fault; put_el(&loaded[h], w, i, v); }
        }
      if (store_form) { s_.rip += d_.position; return Step::Ok; }
      out[0] = loaded[0]; out[1] = loaded[1];
      return finish_dest();
    }
    default: return fail(6);
  }
}

// --- the 0F3A map ---------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::vex_map3(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const bool wide = vex_.wide;
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  if (op == 0xF0) return vex_bmi(m, 3, op, wide_w, vvvv);
  if (op == 0x44 || op == 0xDF || op == 0xCE || op == 0xCF || op == 0xDE) return crypto_map(m, 3, op, wide_w, vvvv);
  if (pp != 1) return fail(6);
  if (!decode_modrm()) return bad_decode();
  const uint32_t imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  const uint32_t reg = d_.reg;
  const uint32_t halves = wide ? 2 : 1;
  const Xmm a[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  Xmm b[2], out[2];
  Env env;
  env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  auto finish_dest = [&]() {
    s_.xmm[reg] = out[0];
    s_.ymm_high[reg] = wide ? out[1] : Xmm();
    s_.rip += d_.position;
    return Step::Ok;
  };
  switch (op) {
    case 0x00: case 0x01: {      // vpermq, vpermpd: four qwords selected by the immediate (from the r/m operand)
      if (!read_y(m, 32, b)) return Step::Fault;
      for (uint32_t i = 0; i < 4; ++i) { const uint32_t k = (imm >> (2 * i)) & 3; put_el(&out[i / 2], 64, i % 2, el(b[k / 2], 64, k % 2)); }
      return finish_dest();
    }
    case 0x02: {      // vpblendd
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      for (uint32_t h = 0; h < halves; ++h) for (uint32_t i = 0; i < 4; ++i) put_el(&out[h], 32, i, ((imm >> (h * 4 + i)) & 1) ? el(b[h], 32, i) : el(a[h], 32, i));
      return finish_dest();
    }
    case 0x04: case 0x05: {      // vpermilps / vpermilpd by immediate (from the r/m operand)
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      for (uint32_t h = 0; h < halves; ++h) {
        if (op == 0x04) for (uint32_t i = 0; i < 4; ++i) put_el(&out[h], 32, i, el(b[h], 32, (imm >> (2 * i)) & 3));
        else for (uint32_t i = 0; i < 2; ++i) put_el(&out[h], 64, i, el(b[h], 64, (imm >> (h * 2 + i)) & 1));
      }
      return finish_dest();
    }
    case 0x06: case 0x46: {      // vperm2f128, vperm2i128
      if (!read_y(m, 32, b)) return Step::Fault;
      const Xmm pool[4] = {a[0], a[1], b[0], b[1]};
      for (uint32_t h = 0; h < 2; ++h) {
        const uint32_t sel = (imm >> (4 * h)) & 15;
        out[h] = (sel & 8) ? Xmm() : pool[sel & 3];
      }
      return finish_dest();
    }
    case 0x08: case 0x09: case 0x0A: case 0x0B: {      // vroundps / pd / ss / sd
      const bool scalar = op >= 0x0A, dbl = (op & 1);
      const Format f = dbl ? Format::F64 : Format::F32;
      const uint32_t w = dbl ? 64 : 32;
      if (!read_y(m, scalar ? 8 : (wide ? 32 : 16), b)) return Step::Fault;
      Rounding mode = (imm & 4) ? env.mode : static_cast<Rounding>(imm & 3);
      Env e2;
      e2.mode = env.mode;
      auto round_one = [&](uint64_t v) { return round_to_integral(f, v, mode, !(imm & 8), e2); };
      if (scalar) {
        Xmm r = a[0];
        put_el(&r, w, 0, round_one(el(b[0], w, 0)));
        out[0] = r;
        s_.xmm[reg] = r; s_.ymm_high[reg] = Xmm();
      } else {
        for (uint32_t h = 0; h < halves; ++h) for (uint32_t i = 0; i < 128 / w; ++i) put_el(&out[h], w, i, round_one(el(b[h], w, i)));
        s_.xmm[reg] = out[0]; s_.ymm_high[reg] = wide ? out[1] : Xmm();
      }
      s_.mxcsr |= e2.flags & ~(kDenormal | ((imm & 8) ? kInexact : 0u));
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x0C: case 0x0D: {      // vblendps, vblendpd
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      const uint32_t w = op == 0x0C ? 32 : 64, n = 128 / w;
      for (uint32_t h = 0; h < halves; ++h) for (uint32_t i = 0; i < n; ++i) put_el(&out[h], w, i, ((imm >> (h * n + i)) & 1) ? el(b[h], w, i) : el(a[h], w, i));
      return finish_dest();
    }
    case 0x0E: {      // vpblendw
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      for (uint32_t h = 0; h < halves; ++h) for (uint32_t i = 0; i < 8; ++i) put_el(&out[h], 16, i, ((imm >> i) & 1) ? el(b[h], 16, i) : el(a[h], 16, i));
      return finish_dest();
    }
    case 0x0F: {      // vpalignr
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      for (uint32_t h = 0; h < halves; ++h) out[h] = alignr(a[h], b[h], imm);
      return finish_dest();
    }
    case 0x18: case 0x38: {      // vinsertf128, vinserti128
      Xmm src[2];
      if (!read_y(m, 16, src)) return Step::Fault;
      out[0] = (imm & 1) ? a[0] : src[0];
      out[1] = (imm & 1) ? src[0] : a[1];
      return finish_dest();
    }
    case 0x19: case 0x39: {      // vextractf128, vextracti128: the destination is the r/m operand
      const Xmm piece = (imm & 1) ? s_.ymm_high[reg] : s_.xmm[reg];
      if (d_.mod == 3) {
        s_.xmm[d_.rm & 15] = piece;
        s_.ymm_high[d_.rm & 15] = Xmm();
      } else {
        const uint64_t addr = memory_address();
        if (!store(m, addr, 8, piece.lo) || !store(m, addr + 8, 8, piece.hi)) return Step::Fault;
      }
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x1D: {      // vcvtps2ph: singles to halves, into the r/m operand
      const uint32_t n = wide ? 8 : 4;
      const Rounding mode = (imm & 4) ? env.mode : static_cast<Rounding>(imm & 3);
      Xmm half_values;
      uint32_t conv_flags = 0;
      const Xmm src[2] = {s_.xmm[reg], s_.ymm_high[reg]};
      for (uint32_t i = 0; i < n; ++i) put_el(&half_values, 16, i, single_to_half(static_cast<uint32_t>(el(src[i / 4], 32, i % 4)), mode, &conv_flags));
      s_.mxcsr |= conv_flags;
      if (d_.mod == 3) {
        s_.xmm[d_.rm & 15] = half_values;
        s_.ymm_high[d_.rm & 15] = Xmm();
      } else {
        const uint64_t addr = memory_address();
        if (!store(m, addr, 8, half_values.lo) || (wide && !store(m, addr + 8, 8, half_values.hi))) return Step::Fault;
      }
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {      // vpextrb, vpextrw, vpextrd/q, vextractps
      const Xmm x = s_.xmm[reg];
      const uint32_t bytes = op == 0x14 ? 1 : op == 0x15 ? 2 : (op == 0x16 && wide_w) ? 8 : 4;
      const uint64_t v = lane_get(x.lo, x.hi, bytes * 8, imm & (16 / bytes - 1));
      if (d_.mod == 3) set_reg(d_.rm, bytes == 8 ? 8 : 4, v);
      else if (!store(m, memory_address(), bytes, v)) return Step::Fault;
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x20: case 0x22: {      // vpinsrb, vpinsrd/q
      const uint32_t bytes = op == 0x20 ? 1 : wide_w ? 8 : 4;
      uint64_t v;
      if (!read_rm(m, bytes == 8 ? 8 : (bytes == 1 ? 1 : 4), &v)) return Step::Fault;
      out[0] = a[0];
      put_el(&out[0], bytes * 8, imm & (16 / bytes - 1), v);
      s_.xmm[reg] = out[0]; s_.ymm_high[reg] = Xmm();
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x21: {      // vinsertps
      Xmm src[2];
      uint32_t from = (imm >> 6) & 3;
      if (d_.mod == 3) { src[0] = s_.xmm[d_.rm]; }
      else { uint64_t v; if (!load(m, memory_address(), 4, &v)) return Step::Fault; src[0] = Xmm(); src[0].lo = v; from = 0; }
      out[0] = a[0];
      put_el(&out[0], 32, (imm >> 4) & 3, el(src[0], 32, from));
      for (uint32_t i = 0; i < 4; ++i) if ((imm >> i) & 1) put_el(&out[0], 32, i, 0);
      s_.xmm[reg] = out[0]; s_.ymm_high[reg] = Xmm();
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x40: case 0x41: {      // dpps, dppd: products selected by imm8[7:4], summed, and copied to the elements selected by imm8[3:0] (the others become zero)
      if (op == 0x41 && wide) return fail(6);
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      const bool is_double = op == 0x41;
      const Format f = is_double ? Format::F64 : Format::F32;
      const uint32_t w = is_double ? 64 : 32, n = 128 / w;
      Env e2;
      e2.mode = env.mode;
      for (uint32_t h = 0; h < halves; ++h) {
        uint64_t term[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < n; ++i) {
          if (!((imm >> (4 + i)) & 1)) continue;
          Env le = e2;
          le.flags = 0;
          term[i] = mul(f, el(a[h], w, i), el(b[h], w, i), le);
          if (le.flags & kInvalid) le.flags &= ~kDenormal;
          e2.flags |= le.flags;
        }
        Env le = e2;
        le.flags = 0;
        uint64_t sum;
        if (is_double) sum = add(f, term[0], term[1], le);
        else sum = add(f, add(f, term[0], term[1], le), add(f, term[2], term[3], le), le);
        if (le.flags & kInvalid) le.flags &= ~kDenormal;
        e2.flags |= le.flags;
        for (uint32_t i = 0; i < n; ++i) put_el(&out[h], w, i, ((imm >> i) & 1) ? sum : 0);
      }
      s_.mxcsr |= e2.flags;
      return finish_dest();
    }
    case 0x42: {      // mpsadbw
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      for (uint32_t h = 0; h < halves; ++h) out[h] = mpsadbw(a[h], b[h], imm >> (3 * h));
      return finish_dest();
    }
    case 0x60: case 0x61: case 0x62: case 0x63: {      // pcmpestrm, pcmpestri, pcmpistrm, pcmpistri
      if (wide) return fail(6);
      if (!read_y(m, 16, b)) return Step::Fault;
      const Xmm first = s_.xmm[reg];
      const uint32_t w = (imm & 1) ? 16 : 8, n = 128 / w;
      uint32_t la, lb;
      if (op < 0x62) {      // explicit lengths in rax and rdx (their absolute values, at most the element count)
        auto length = [&](uint64_t v) {
          const int64_t sv = d_.rex_w ? static_cast<int64_t>(v) : static_cast<int32_t>(v);
          const uint64_t mag = sv < 0 ? 0 - static_cast<uint64_t>(sv) : static_cast<uint64_t>(sv);
          return mag > n ? n : static_cast<uint32_t>(mag);
        };
        la = length(s_.r[0]);
        lb = length(s_.r[2]);
      } else {      // implicit: up to the first zero element
        auto length = [&](const Xmm& x) { uint32_t len = 0; while (len < n && el(x, w, len) != 0) ++len; return len; };
        la = length(first);
        lb = length(b[0]);
      }
      uint32_t result_flags = 0;
      const uint32_t res2 = string_compare(first, b[0], la, lb, imm, &result_flags);
      s_.rflags = (s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kSign | kAdjust)) | ((result_flags & 1) ? kCarry : 0) | ((result_flags & 2) ? kZero : 0) | ((result_flags & 4) ? kSign : 0) | ((result_flags & 8) ? kOverflow : 0);
      if (op & 1) {      // the index of the lowest or highest set bit; n when there is none
        uint32_t index = n;
        if (res2) {
          if (imm & 0x40) { index = 0; for (uint32_t i = 0; i < n; ++i) if ((res2 >> i) & 1) index = i; }
          else { index = 0; while (!((res2 >> index) & 1)) ++index; }
        }
        s_.r[1] = index;
      } else {      // xmm0: the bit mask, or each bit widened to an element
        Xmm mask;
        if (imm & 0x40) for (uint32_t i = 0; i < n; ++i) put_el(&mask, w, i, ((res2 >> i) & 1) ? (w == 16 ? 0xFFFFu : 0xFFu) : 0);
        else mask.lo = res2;
        s_.xmm[0] = mask;
        s_.ymm_high[0] = Xmm();
      }
      s_.rip += d_.position;
      return Step::Ok;
    }
    case 0x4A: case 0x4B: case 0x4C: {      // vblendvps, vblendvpd, vpblendvb: the selector is the register in imm8[7:4]
      if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
      const uint32_t sel = imm >> 4;
      const Xmm mask[2] = {s_.xmm[sel], s_.ymm_high[sel]};
      const uint32_t w = op == 0x4A ? 32 : op == 0x4B ? 64 : 8;
      for (uint32_t h = 0; h < halves; ++h) for (uint32_t i = 0; i < 128 / w; ++i) put_el(&out[h], w, i, ((el(mask[h], w, i) >> (w - 1)) & 1) ? el(b[h], w, i) : el(a[h], w, i));
      return finish_dest();
    }
    default: return fail(6);
  }
}

// --- the legacy (non-VEX) encodings of SSSE3, SSE4.1 and SSE4.2 -----------------------------------------------------------------------------------
//
// The same instructions as the VEX ones with the destination doubling as the first source and the upper halves of the YMM registers left alone. Only the
// opcodes that have a legacy form are accepted, so a VEX-only opcode never executes without its prefix. The MMX forms (no 66 prefix) are not emulated.

X86_64Cpu::Step X86_64Cpu::legacy_sse_map(GuestMemory& m, uint32_t map, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!d_.operand16) return fail(6);
  if (d_.position >= d_.available) return bad_decode();
  const uint32_t modrm = d_.bytes[d_.position];
  const uint32_t reg = ((modrm >> 3) & 7) | (d_.rex_r ? 8 : 0);
  if (map == 2 && (op == 0x10 || op == 0x14 || op == 0x15)) {      // pblendvb, blendvps, blendvpd: the selector is xmm0
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    Xmm src;
    if (!read_xmm_rm(m, &src)) return Step::Fault;
    const Xmm mask = s_.xmm[0], dst = s_.xmm[reg];
    Xmm out;
    const uint32_t w = op == 0x10 ? 8 : op == 0x14 ? 32 : 64;
    for (uint32_t i = 0; i < 128 / w; ++i) lane_set(&out.lo, &out.hi, w, i, ((lane_get(mask.lo, mask.hi, w, i) >> (w - 1)) & 1) ? lane_get(src.lo, src.hi, w, i) : lane_get(dst.lo, dst.hi, w, i));
    s_.xmm[reg] = out;
    s_.rip += d_.position;
    return Step::Ok;
  }
  bool allowed;
  if (map == 2) allowed = op <= 0x0B || (op >= 0x1C && op <= 0x1E) || op == 0x17 || (op >= 0x20 && op <= 0x25) || (op >= 0x28 && op <= 0x2B) || (op >= 0x30 && op <= 0x35) || (op >= 0x37 && op <= 0x41) || (op >= 0xDB && op <= 0xDF) || op == 0xCF;
  else allowed = (op >= 0x08 && op <= 0x0F) || (op >= 0x14 && op <= 0x17) || (op >= 0x20 && op <= 0x22) || (op >= 0x40 && op <= 0x42) || (op >= 0x60 && op <= 0x63) || op == 0x44 || op == 0xDF || op == 0xCE || op == 0xCF;
  if (!allowed) return fail(6);
  YmmHigh saved_high[16];
  for (uint32_t i = 0; i < 16; ++i) saved_high[i] = s_.ymm_high[i];
  const Step r = map == 2 ? vex_map2(m, op, d_.rex_w, reg) : vex_map3(m, op, d_.rex_w, reg);
  for (uint32_t i = 0; i < 16; ++i) s_.ymm_high[i] = saved_high[i];
  return r;
}

// crc32 (F2 0F 38 F0 / F1): CRC-32C of the source folded into the low 32 bits of the destination
X86_64Cpu::Step X86_64Cpu::crc32_instruction(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t bytes = op == 0xF0 ? 1 : d_.rex_w ? 8 : d_.operand16 ? 2 : 4;
  uint64_t v;
  if (!read_rm(m, bytes, &v)) return Step::Fault;
  const uint32_t crc = crc32c_step(static_cast<uint32_t>(s_.r[d_.reg]), v, bytes);
  set_reg(d_.reg, d_.rex_w ? 8 : 4, crc);
  s_.rip += d_.position;
  return Step::Ok;
}

// --- FMA3 ---------------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::vex_fma(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!d_.operand16) return fail(6);
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t order = op >> 4;      // 9: 132, A: 213, B: 231
  const uint32_t low = op & 15;
  const bool scalar = low == 9 || low == 0xB || low == 0xD || low == 0xF;
  const bool wide = vex_.wide && !scalar;
  const Format f = wide_w ? Format::F64 : Format::F32;
  const uint32_t w = wide_w ? 64 : 32;
  Xmm c[2];
  if (!read_y(m, scalar ? w / 8 : (wide ? 32 : 16), c)) return Step::Fault;
  const Xmm da[2] = {s_.xmm[d_.reg], s_.ymm_high[d_.reg]}, vb[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  Xmm out[2] = {da[0], da[1]};
  Env env;
  env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  const bool negate_product = low == 0xC || low == 0xD || low == 0xE || low == 0xF;
  const bool negate_addend = low == 0xA || low == 0xB || low == 0xE || low == 0xF;
  for (uint32_t h = 0; h < (wide ? 2u : 1u); ++h)
    for (uint32_t i = 0; i < (scalar ? 1u : 128 / w); ++i) {
      const uint64_t A = el(da[h], w, i), B = el(vb[h], w, i), C = el(c[h], w, i);
      uint64_t p, q, r;
      if (order == 9) { p = A; q = C; r = B; }
      else if (order == 0xA) { p = B; q = A; r = C; }
      else { p = B; q = C; r = A; }
      {      // NaNs: the first one in the order multiplicand, multiplicand, addend, quieted; a signaling one anywhere raises invalid
        const uint64_t vals[3] = {p, q, r}, quiet_bit = w == 64 ? uint64_t{1} << 51 : uint64_t{1} << 22;
        int pick = -1;
        bool any_signaling = false;
        for (int k = 0; k < 3; ++k) {
          if (!is_nan(f, vals[k])) continue;
          if (!(vals[k] & quiet_bit)) any_signaling = true;
          if (pick < 0) pick = k;
        }
        if (pick >= 0) {
          if (any_signaling) env.flags |= kInvalid;
          put_el(&out[h], w, i, vals[pick] | quiet_bit);
          continue;
        }
      }
      bool neg_p = negate_product, neg_r = negate_addend;
      if (low == 6) neg_r = (i & 1) == 0;      // fmaddsub: subtract in the even elements
      if (low == 7) neg_r = (i & 1) == 1;      // fmsubadd: subtract in the odd ones
      if (neg_p && !is_nan(f, p)) p = negate(f, p);      // (a NaN passes through with its own sign)
      if (neg_r && !is_nan(f, r)) r = negate(f, r);
      put_el(&out[h], w, i, fused_multiply_add(f, p, q, r, env));
    }
  if (env.flags & kInvalid) env.flags &= ~kDenormal;
  s_.mxcsr |= env.flags;
  s_.xmm[d_.reg] = out[0];
  s_.ymm_high[d_.reg] = wide ? out[1] : Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}

// --- gathers --------------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::vex_gather(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!d_.operand16) return fail(6);
  // the memory operand has a vector index (VSIB)
  const uint8_t modrm = next_u8();
  if ((modrm >> 6) == 3 || (modrm & 7) != 4) return fail(6);
  const uint32_t mod = modrm >> 6;
  const uint32_t reg = ((modrm >> 3) & 7) | (d_.rex_r ? 8 : 0);
  const uint8_t sib = next_u8();
  const uint32_t scale = sib >> 6, index = ((sib >> 3) & 7) | (d_.rex_x ? 8 : 0), base_reg = (sib & 7) | (d_.rex_b ? 8 : 0);
  const bool no_base = (sib & 7) == 5 && mod == 0;
  int64_t disp = 0;
  if (mod == 1) disp = sx(next_u8(), 8);
  else if (mod == 2 || no_base) disp = sx(next_imm(4, false), 32);
  if (d_.truncated) return bad_decode();
  const uint64_t base = (no_base ? 0 : s_.r[base_reg]) + static_cast<uint64_t>(disp);
  const bool wide = vex_.wide;
  const uint32_t elem_bits = wide_w ? 64 : 32, index_bits = (op == 0x90 || op == 0x92) ? 32 : 64;
  const uint32_t n = (wide ? 256 : 128) / (elem_bits > index_bits ? elem_bits : index_bits);
  const Xmm idx[2] = {s_.xmm[index], s_.ymm_high[index]};
  Xmm dst[2] = {s_.xmm[reg], s_.ymm_high[reg]}, mask[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  uint64_t gathered[8];
  bool take[8];
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t ib = i * index_bits, eb = i * elem_bits;
    const uint64_t index_value = lane_get(idx[ib / 128].lo, idx[ib / 128].hi, index_bits, (ib % 128) / index_bits);
    const Xmm& mk = mask[eb / 128];
    take[i] = (lane_get(mk.lo, mk.hi, elem_bits, (eb % 128) / elem_bits) >> (elem_bits - 1)) & 1;
    if (!take[i]) continue;
    const uint64_t addr = base + (static_cast<uint64_t>(sx(index_value, index_bits)) << scale);
    if (!load(m, addr, elem_bits / 8, &gathered[i])) return Step::Fault;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t eb = i * elem_bits;
    if (take[i]) lane_set(&dst[eb / 128].lo, &dst[eb / 128].hi, elem_bits, (eb % 128) / elem_bits, gathered[i]);
  }
  const bool fits_in_128 = n * elem_bits <= 128;
  for (uint32_t i = n; i * elem_bits < 128; ++i) lane_set(&dst[0].lo, &dst[0].hi, elem_bits, i, 0);      // the elements the gather does not name are cleared
  s_.xmm[reg] = dst[0];
  s_.ymm_high[reg] = (wide && !fits_in_128) ? dst[1] : Xmm();
  s_.xmm[vvvv] = Xmm();
  s_.ymm_high[vvvv] = Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}

// --- BMI1 / BMI2 -------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::vex_bmi(GuestMemory& m, int map, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  if (!decode_modrm()) return bad_decode();
  uint32_t imm = 0;
  if (map == 3) imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  const uint32_t size = wide_w ? 8 : 4, bits = size * 8;
  const uint64_t mask = size == 8 ? ~uint64_t{0} : 0xFFFFFFFFu;
  uint64_t src = 0;
  if (!read_rm(m, size, &src)) return Step::Fault;
  const uint64_t v = s_.r[vvvv] & mask;
  auto flags_zs = [&](uint64_t r, bool carry) {
    uint8_t low = static_cast<uint8_t>(r);
    low ^= low >> 4; low ^= low >> 2; low ^= low >> 1;
    s_.rflags = (s_.rflags & ~(kZero | kSign | kCarry | kOverflow | kParity | kAdjust)) | ((r & mask) == 0 ? kZero : 0) | ((r >> (bits - 1)) & 1 ? kSign : 0) | (carry ? kCarry : 0) | ((low & 1) ? 0 : kParity);      // (PF follows the result on real CPUs; AF is undefined)
  };
  auto done = [&](uint32_t dest, uint64_t r) { set_reg(dest, size, r); s_.rip += d_.position; return Step::Ok; };
  if (map == 3) {      // rorx
    if (op != 0xF0 || pp != 3) return fail(6);
    const uint32_t n = imm & (bits - 1);
    return done(d_.reg, n ? ((src >> n) | (src << (bits - n))) & mask : src);
  }
  switch (op) {
    case 0xF2:      // andn
      if (pp != 0) return fail(6);
      { const uint64_t r = ~v & src & mask; flags_zs(r, false); return done(d_.reg, r); }
    case 0xF3: {      // blsr, blsmsk, blsi
      if (pp != 0) return fail(6);
      const uint32_t kind = d_.reg & 7;
      uint64_t r;
      bool carry;
      if (kind == 1) { r = src & (src - 1); carry = src == 0; }
      else if (kind == 2) { r = src ^ (src - 1); carry = src == 0; }
      else if (kind == 3) { r = src & (~src + 1); carry = src != 0; }
      else return fail(6);
      r &= mask;
      flags_zs(r, carry);
      return done(vvvv, r);
    }
    case 0xF5: {
      if (pp == 0) {      // bzhi
        const uint32_t idx = static_cast<uint32_t>(v & 0xFF);
        const uint64_t r = idx >= bits ? src : src & ((uint64_t{1} << idx) - 1);
        flags_zs(r, idx >= bits);
        return done(d_.reg, r);
      }
      if (pp == 2 || pp == 3) {      // pext (F3), pdep (F2): v is the source, src the mask
        uint64_t r = 0;
        if (pp == 2) { uint32_t k = 0; for (uint32_t i = 0; i < bits; ++i) if ((src >> i) & 1) { r |= ((v >> i) & 1) << k; ++k; } }
        else { uint32_t k = 0; for (uint32_t i = 0; i < bits; ++i) if ((src >> i) & 1) { r |= ((v >> k) & 1) << i; ++k; } }
        return done(d_.reg, r);
      }
      return fail(6);
    }
    case 0xF6: {      // mulx: reg = high half, vvvv = low half of rdx * src
      if (pp != 3) return fail(6);
      const uint64_t rdx = s_.r[2] & mask;
      uint64_t hi, lo;
      if (size == 8) multiply_unsigned(rdx, src, &hi, &lo);
      else { const uint64_t p = rdx * src; lo = p & mask; hi = p >> 32; }
      set_reg(vvvv, size, lo);
      return done(d_.reg, hi);
    }
    case 0xF7: {
      if (pp == 0) {      // bextr
        const uint32_t start = static_cast<uint32_t>(v & 0xFF), len = static_cast<uint32_t>((v >> 8) & 0xFF);
        uint64_t r = start >= bits ? 0 : src >> start;
        if (len < bits) r &= (uint64_t{1} << len) - 1;
        flags_zs(r, false);
        s_.rflags &= ~kSign;
        return done(d_.reg, r);
      }
      const uint32_t n = static_cast<uint32_t>(v & (bits - 1));
      if (pp == 1) return done(d_.reg, (src << n) & mask);                                                   // shlx
      if (pp == 2) return done(d_.reg, static_cast<uint64_t>(sx(src, bits) >> n) & mask);                     // sarx
      return done(d_.reg, src >> n);                                                                          // shrx
    }
    default: return fail(6);
  }
}

// --- XSAVE / XRSTOR / XGETBV ---------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::xsave_family(GuestMemory& m, uint32_t kind) {
  const uint64_t address = memory_address();
  const uint64_t requested = (s_.r[0] & 0xFFFFFFFFu) | (s_.r[2] << 32);
  const uint64_t features = requested & 0xE7;      // XCR0: x87, SSE, AVX, opmask, ZMM_Hi256, Hi16_ZMM
  uint8_t image[2688] = {};
  auto put64 = [&](uint32_t at, uint64_t v) { for (uint32_t b = 0; b < 8; ++b) image[at + b] = static_cast<uint8_t>(v >> (8 * b)); };
  auto get64 = [&](uint32_t at) { uint64_t v = 0; for (uint32_t b = 0; b < 8; ++b) v |= uint64_t{image[at + b]} << (8 * b); return v; };
  const uint64_t bytes = (features & 0xE0) ? 2688 : (features & 4) ? 832 : 576;
  if (kind == 4) {      // xsave
    save_fpu_image(image);
    put64(512, features);      // XSTATE_BV: everything requested is in use
    for (uint32_t r = 0; r < 16; ++r) {
      put64(576 + 16 * r, s_.ymm_high[r].lo);
      put64(584 + 16 * r, s_.ymm_high[r].hi);
      put64(1152 + 32 * r, s_.ymm_high[r].upper[0].lo);
      put64(1160 + 32 * r, s_.ymm_high[r].upper[0].hi);
      put64(1168 + 32 * r, s_.ymm_high[r].upper[1].lo);
      put64(1176 + 32 * r, s_.ymm_high[r].upper[1].hi);
    }
    for (uint32_t k = 0; k < 8; ++k) put64(1088 + 8 * k, s_.k[k]);
    for (uint32_t r = 0; r < 16; ++r) {
      const Vec512 v = get_vec(16 + r);
      for (uint32_t q = 0; q < 8; ++q) put64(1664 + 64 * r + 8 * q, v.q[q]);
    }
    if (!m.write(address, ConstByteView(image, bytes))) return fail(14, address, 2);
    return Step::Ok;
  }
  // xrstor: components not marked present in the header are put in their initial state
  if (!m.read(address, ByteView(image, 576))) return fail(14, address, 0);
  const uint64_t bv = get64(512);
  if (bytes > 576 && !m.read(address + 576, ByteView(image + 576, bytes - 576))) return fail(14, address + 576, 0);
  if (features & 1) {
    if (!(bv & 1)) { ByteView(image, 32).fill(0); image[0] = 0x7F; image[1] = 0x03; ByteView(image + 32, 128).fill(0); }
    if (!(bv & 2)) { ByteView(image + 160, 256).fill(0); }
  }
  load_fpu_image(image);
  if (features & 4) {
    for (uint32_t r = 0; r < 16; ++r) {
      s_.ymm_high[r] = Xmm();
      if (bv & 4) {
        s_.ymm_high[r].lo = get64(576 + 16 * r);
        s_.ymm_high[r].hi = get64(584 + 16 * r);
      }
    }
  }
  if (features & 0x20) {
    for (uint32_t k = 0; k < 8; ++k) s_.k[k] = (bv & 0x20) ? get64(1088 + 8 * k) : 0;
  }
  if (features & 0x40) {
    for (uint32_t r = 0; r < 16; ++r) {
      s_.ymm_high[r].upper[0].lo = (bv & 0x40) ? get64(1152 + 32 * r) : 0;
      s_.ymm_high[r].upper[0].hi = (bv & 0x40) ? get64(1160 + 32 * r) : 0;
      s_.ymm_high[r].upper[1].lo = (bv & 0x40) ? get64(1168 + 32 * r) : 0;
      s_.ymm_high[r].upper[1].hi = (bv & 0x40) ? get64(1176 + 32 * r) : 0;
    }
  }
  if (features & 0x80) {
    for (uint32_t r = 0; r < 16; ++r) {
      Vec512 v;
      if (bv & 0x80)
        for (uint32_t q = 0; q < 8; ++q) v.q[q] = get64(1664 + 64 * r + 8 * q);
      set_vec(16 + r, v, 512);
    }
  }
  return Step::Ok;
}

}  // namespace emulation
