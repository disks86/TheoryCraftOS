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

// Intel AVX10.2 BFloat16 arithmetic: vadd / vsub / vmul / vdiv / vmin / vmax / vsqrt / vfmadd / vfmsub / vfnmadd / vfnmsub / vscalef / vrcp / vrsqrt / vgetexp / vgetmant / vrndscale / vreduce / vfpclass /
// vcmp / vcomis for bf16 elements. None of them reads or writes MXCSR or raises an exception: denormal inputs are zeros, denormal results are flushed, rounding is to nearest even.
// (Specification: Intel AVX10.2 Architecture Specification 361050, chapter 7. vrcp and vrsqrt are computed correctly rounded, which is within the documented error bound.)
#include "Emulation/HalfFloat.hpp"
#include "Emulation/SoftFloat.hpp"
#include "Emulation/X86_64EvexInternal.hpp"

namespace emulation {

using namespace softfloat;

// The rows: bfloat16 arithmetic is MAP5 with 66 (and vcomisbf16), the unary and fused operations MAP6, the ones with an immediate MAP3 with F2.
const EvexOp kEvexBf16Ops[] = {
    {5, 1, 0x58, 0, kBfloat, 0, 16, kFull},  {5, 1, 0x5C, 0, kBfloat, 1, 16, kFull},  {5, 1, 0x59, 0, kBfloat, 2, 16, kFull},  {5, 1, 0x5E, 0, kBfloat, 3, 16, kFull},
    {5, 1, 0x5D, 0, kBfloat, 4, 16, kFull},  {5, 1, 0x5F, 0, kBfloat, 5, 16, kFull},  {5, 1, 0x51, 0, kBfloat, 6, 16, kFull},  {5, 1, 0x2F, 0, kBfloat, 17, 16, kS2},
    {6, 0, 0x98, 0, kBfloat, 7, 16, kFull},  {6, 0, 0x9A, 0, kBfloat, 7, 16, kFull},  {6, 0, 0x9C, 0, kBfloat, 7, 16, kFull},  {6, 0, 0x9E, 0, kBfloat, 7, 16, kFull},
    {6, 0, 0xA8, 0, kBfloat, 7, 16, kFull},  {6, 0, 0xAA, 0, kBfloat, 7, 16, kFull},  {6, 0, 0xAC, 0, kBfloat, 7, 16, kFull},  {6, 0, 0xAE, 0, kBfloat, 7, 16, kFull},
    {6, 0, 0xB8, 0, kBfloat, 7, 16, kFull},  {6, 0, 0xBA, 0, kBfloat, 7, 16, kFull},  {6, 0, 0xBC, 0, kBfloat, 7, 16, kFull},  {6, 0, 0xBE, 0, kBfloat, 7, 16, kFull},
    {6, 0, 0x2C, 0, kBfloat, 8, 16, kFull},  {6, 0, 0x42, 0, kBfloat, 9, 16, kFull},  {6, 0, 0x4C, 0, kBfloat, 10, 16, kFull}, {6, 0, 0x4E, 0, kBfloat, 11, 16, kFull},
    {3, 3, 0x08, 0, kBfloat, 12, 16, kFull}, {3, 3, 0x56, 0, kBfloat, 13, 16, kFull}, {3, 3, 0x26, 0, kBfloat, 14, 16, kFull}, {3, 3, 0xC2, 0, kBfloat, 15, 16, kFull},
    {3, 3, 0x66, 0, kBfloat, 16, 16, kFull},
    {3, 1, 0x52, 1, kMinMax, 0, 64, kFull},  {3, 0, 0x52, 0, kMinMax, 0, 16, kFull},  {3, 1, 0x52, 0, kMinMax, 0, 32, kFull},  {3, 3, 0x52, 0, kMinMax, 1, 16, kFull},
    {3, 1, 0x53, 1, kMinMax, 2, 64, kS8},    {3, 0, 0x53, 0, kMinMax, 2, 16, kS2},    {3, 1, 0x53, 0, kMinMax, 2, 32, kS4},
    {1, 3, 0x2F, 1, kComx, 0, 64, kS8},      {1, 2, 0x2F, 0, kComx, 0, 32, kS4},      {5, 2, 0x2F, 0, kComx, 0, 16, kS2},
    {1, 3, 0x2E, 1, kComx, 1, 64, kS8},      {1, 2, 0x2E, 0, kComx, 1, 32, kS4},      {5, 2, 0x2E, 0, kComx, 1, 16, kS2},
    {2, 0, 0x52, 0, kDpPh, 0, 32, kFull},    {3, 2, 0x42, 0, kMpsadbw, 0, 8, kFullMem},
    // saturating conversions: sub = source type (0 f32, 1 f64, 2 f16, 3 bf16) | destination code << 2 (0: byte in a word / dword element, 1: 32 bits, 2: 64 bits) | 0x10 unsigned | 0x20 truncate | 0x40 to a GPR
    {5, 0, 0x6D, 0, kSatCvt, 0x00 | 4 | 0x20, 32, kFull}, {5, 0, 0x6C, 0, kSatCvt, 0x00 | 4 | 0x30, 32, kFull}, {5, 0, 0x6D, 1, kSatCvt, 0x01 | 4 | 0x20, 64, kFull}, {5, 0, 0x6C, 1, kSatCvt, 0x01 | 4 | 0x30, 64, kFull},
    {5, 1, 0x6D, 0, kSatCvt, 0x00 | 8 | 0x20, 32, kHalf}, {5, 1, 0x6C, 0, kSatCvt, 0x00 | 8 | 0x30, 32, kHalf}, {5, 1, 0x6D, 1, kSatCvt, 0x01 | 8 | 0x20, 64, kFull}, {5, 1, 0x6C, 1, kSatCvt, 0x01 | 8 | 0x30, 64, kFull},
    {5, 3, 0x6D, -1, kSatCvt, 0x01 | 0x20 | 0x40, 64, kS8}, {5, 3, 0x6C, -1, kSatCvt, 0x01 | 0x30 | 0x40, 64, kS8}, {5, 2, 0x6D, -1, kSatCvt, 0x00 | 0x20 | 0x40, 32, kS4}, {5, 2, 0x6C, -1, kSatCvt, 0x00 | 0x30 | 0x40, 32, kS4},
    {5, 0, 0x68, 0, kSatCvt, 0x02 | 0x20, 16, kFull}, {5, 0, 0x69, 0, kSatCvt, 0x02, 16, kFull}, {5, 0, 0x6A, 0, kSatCvt, 0x02 | 0x30, 16, kFull}, {5, 0, 0x6B, 0, kSatCvt, 0x02 | 0x10, 16, kFull},
    {5, 1, 0x68, 0, kSatCvt, 0x00 | 0x20, 32, kFull}, {5, 1, 0x69, 0, kSatCvt, 0x00, 32, kFull}, {5, 1, 0x6A, 0, kSatCvt, 0x00 | 0x30, 32, kFull}, {5, 1, 0x6B, 0, kSatCvt, 0x00 | 0x10, 32, kFull},
    {5, 3, 0x68, 0, kSatCvt, 0x03 | 0x20, 16, kFull}, {5, 3, 0x69, 0, kSatCvt, 0x03, 16, kFull}, {5, 3, 0x6A, 0, kSatCvt, 0x03 | 0x30, 16, kFull}, {5, 3, 0x6B, 0, kSatCvt, 0x03 | 0x10, 16, kFull},
    // FP8 conversions: sub = hf8 (else bf8) | 2 saturate | form << 2 (0 vcvtph2*, 1 vcvt2ph2*, 2 vcvtbiasph2*, 3 vcvthf82ph, 4 vcvt2ps2phx)
    {2, 2, 0x74, 0, kFp8, 0 | 0 | 0, 16, kFull}, {5, 2, 0x74, 0, kFp8, 0 | 2, 16, kFull}, {5, 2, 0x18, 0, kFp8, 1, 16, kFull}, {5, 2, 0x1B, 0, kFp8, 1 | 2, 16, kFull},
    {2, 3, 0x74, 0, kFp8, 4, 16, kFull}, {5, 3, 0x74, 0, kFp8, 4 | 2, 16, kFull}, {5, 3, 0x18, 0, kFp8, 4 | 1, 16, kFull}, {5, 3, 0x1B, 0, kFp8, 4 | 1 | 2, 16, kFull},
    {2, 0, 0x74, 0, kFp8, 8, 16, kFull}, {5, 0, 0x74, 0, kFp8, 8 | 2, 16, kFull}, {5, 0, 0x18, 0, kFp8, 8 | 1, 16, kFull}, {5, 0, 0x1B, 0, kFp8, 8 | 1 | 2, 16, kFull},
    {5, 3, 0x1E, 0, kFp8, 12 | 1, 8, kHalf}, {2, 1, 0x67, 0, kFp8, 16, 32, kFull},
};
const uint32_t kEvexBf16OpCount = sizeof(kEvexBf16Ops) / sizeof(kEvexBf16Ops[0]);

namespace {

constexpr Format kBf = Format::BF16;

uint64_t flush(uint64_t x) { return (x & 0x7F80) == 0 ? (x & 0x8000) : x; }
bool nan_of(uint64_t x) { return (x & 0x7F80) == 0x7F80 && (x & 0x7F) != 0; }
uint64_t quiet(uint64_t x) { return x | 0x40; }
constexpr uint64_t kIndefinite = 0xFFC0;

uint32_t to_f32(uint64_t x) { return static_cast<uint32_t>(x) << 16; }

Env make_env() {
  Env e;
  e.mode = Rounding::NearestEven;
  e.daz = e.ftz = true;
  return e;
}

// 2^k as a binary32 pattern, k in [-126, 127]
uint32_t pow2(int k) { return static_cast<uint32_t>(k + 127) << 23; }

// x * 2^k computed on binary32 (exact unless it overflows or underflows), flushed to a bf16 pattern.
uint64_t scale_by_pow2(uint64_t x, int64_t k) {
  Env e = make_env();
  e.ftz = false;
  uint64_t v = to_f32(x);
  while (k > 0) { const int step = k > 100 ? 100 : static_cast<int>(k); v = mul(Format::F32, v, pow2(step), e); k -= step; }
  while (k < 0) { const int step = k < -100 ? 100 : static_cast<int>(-k); v = mul(Format::F32, v, pow2(-step), e); k += step; }
  Env f = make_env();
  return flush(convert(Format::F32, kBf, v, f));
}

// ROUND(2^m * x, RNE) * 2^-m for a finite x (a bf16 has 8 significant bits, so a value of 2^(8-m) or more is its own result)
uint64_t round_scaled(uint64_t x, int m) {
  const int exp = static_cast<int>((x >> 7) & 0xFF) - 127;
  if (exp >= 8 - m) return x;
  Env e = make_env();
  e.ftz = false;
  const uint64_t scaled = mul(Format::F32, to_f32(x), pow2(m), e);
  const uint64_t rounded = round_to_integral(Format::F32, scaled, Rounding::NearestEven, false, e);
  const uint64_t back = mul(Format::F32, rounded, pow2(-m), e);
  Env f = make_env();
  f.ftz = false;
  return convert(Format::F32, kBf, back, f);
}

enum Op : uint8_t {
  kAddOp, kSubOp, kMulOp, kDivOp, kMinOp, kMaxOp, kSqrtOp, kFmaOp, kScalefOp, kGetExpOp, kRcpOp, kRsqrtOp, kRndOp, kReduceOp, kGetMantOp, kCmpOp, kClassOp, kComisOp
};

}  // namespace

// sub: the operation (an Op); for the fused multiply-adds the opcode says which one (9x: 132, Ax: 213, Bx: 231; x = 8 madd, A msub, C nmadd, E nmsub).
X86_64Cpu::Step X86_64Cpu::evex_bf16(GuestMemory& m, const EvexOp& def) {
  const uint32_t op = def.sub & 0x1F;
  const bool unary = op == kSqrtOp || op == kGetExpOp || op == kRcpOp || op == kRsqrtOp || op == kRndOp || op == kReduceOp || op == kGetMantOp || op == kClassOp || op == kComisOp;
  if (ev_.er || ev_.w) return fail(6);
  if (unary && ev_.vvvv != 0) return fail(6);
  const uint32_t reg = ev_.reg, vl = op == kComisOp ? 128 : ev_.vl, n = vl / 16;
  const uint32_t imm = ev_.imm & 0xFF;
  Env env = make_env();
  Vec b;
  if (op == kComisOp) {
    uint64_t v = 0;
    if (ev_.memory) {
      if (!load(m, memory_address(), 2, &v)) return Step::Fault;
    } else {
      v = element(get_vec(ev_.rm), 16, 0);
    }
    const uint64_t x = flush(element(get_vec(reg), 16, 0)), y = flush(v);
    const Order o = compare(kBf, x, y, false, env);
    uint64_t flags = s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kSign | kAdjust);
    if (o == Order::Unordered) flags |= kZero | kParity | kCarry;
    else if (o == Order::Less) flags |= kCarry;
    else if (o == Order::Equal) flags |= kZero;
    s_.rflags = flags;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (!ev_read_rm(m, &b, 16, true, false)) return Step::Fault;
  const Vec a = get_vec(ev_.vvvv);
  const Vec dest = ev_.old_dest;
  Vec result;
  uint64_t mask_bits = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t x = element(a, 16, i), y = element(b, 16, i);      // (unary operations use y)
    const uint64_t fx = flush(x), fy = flush(y);
    uint64_t r = 0;
    switch (op) {
      case kAddOp: r = add(kBf, x, y, env); break;
      case kSubOp: r = sub(kBf, x, y, env); break;
      case kMulOp: r = mul(kBf, x, y, env); break;
      case kDivOp: r = div(kBf, x, y, env); break;
      case kMinOp: case kMaxOp: {
        // the second operand when either is a NaN or both are zeros; otherwise the smaller / larger
        if (nan_of(fx) || nan_of(fy) || ((fx & 0x7FFF) == 0 && (fy & 0x7FFF) == 0)) { r = fy; break; }
        const Order o = compare(kBf, fx, fy, false, env);
        r = (op == kMaxOp ? o == Order::Greater : o == Order::Less) ? fx : fy;
        break;
      }
      case kSqrtOp: r = sqrt(kBf, y, env); break;
      case kFmaOp: {
        const uint32_t order = (def.opcode >> 4) == 9 ? 0 : (def.opcode >> 4) == 0xA ? 1 : 2;
        const uint64_t d = element(dest, 16, i);
        uint64_t p, q, c;
        if (order == 0) { p = d; q = y; c = x; }      // 132: dest * src3 + src2
        else if (order == 1) { p = x; q = d; c = y; }      // 213: src2 * dest + src3
        else { p = x; q = y; c = d; }      // 231: src2 * src3 + dest
        const uint32_t kind = def.opcode & 15;      // 8: madd, A: msub, C: nmadd, E: nmsub
        if (kind >= 0xC) p = negate(kBf, p);
        if (kind == 0xA || kind == 0xE) c = negate(kBf, c);
        r = fused_multiply_add(kBf, p, q, c, env);
        break;
      }
      case kScalefOp: {
        if (nan_of(fx) || nan_of(fy)) { r = quiet(nan_of(fx) ? fx : fy); break; }
        const bool y_inf = (fy & 0x7FFF) == 0x7F80, x_inf = (fx & 0x7FFF) == 0x7F80, x_zero = (fx & 0x7FFF) == 0;
        if (y_inf) {
          const bool y_neg = fy & 0x8000;
          if (!y_neg) r = x_zero ? kIndefinite : (x_inf ? fx : (fx & 0x8000) | 0x7F80);
          else r = x_inf ? kIndefinite : (fx & 0x8000);
          break;
        }
        if (x_inf || x_zero) { r = fx; break; }
        // floor(y)
        const int ey = static_cast<int>((fy >> 7) & 0xFF) - 127;
        int64_t k;
        if ((fy & 0x7FFF) == 0) k = 0;
        else if (ey >= 9) k = (fy & 0x8000) ? -100000 : 100000;
        else {
          Env e = make_env();
          e.ftz = false;
          const uint64_t fl = round_to_integral(Format::F32, to_f32(fy), Rounding::Down, false, e);
          k = to_int(Format::F32, fl, 32, true, e);
        }
        r = scale_by_pow2(fx, k);
        break;
      }
      case kGetExpOp: {
        if (nan_of(y)) r = quiet(y);
        else if ((y & 0x7F80) == 0x7F80) r = 0x7F80;
        else if ((y & 0x7F80) == 0) r = 0xFF80;
        else { Env e = make_env(); r = from_int(kBf, static_cast<int64_t>((y >> 7) & 0xFF) - 127, e); }
        break;
      }
      case kRcpOp: case kRsqrtOp: {
        if (nan_of(fy)) { r = quiet(fy); break; }
        const bool neg = fy & 0x8000;
        if ((fy & 0x7FFF) == 0) { r = (neg ? 0x8000 : 0) | 0x7F80; break; }
        if ((fy & 0x7FFF) == 0x7F80) { r = (op == kRsqrtOp && neg) ? kIndefinite : (neg ? 0x8000 : 0); break; }
        if (op == kRsqrtOp && neg) { r = kIndefinite; break; }
        Env e = make_env();
        e.ftz = false;
        uint64_t v = to_f32(fy);
        if (op == kRsqrtOp) v = sqrt(Format::F32, v, e);
        v = div(Format::F32, 0x3F800000, v, e);
        Env f = make_env();
        r = convert(Format::F32, kBf, v, f);
        break;
      }
      case kRndOp: case kReduceOp: {
        if (nan_of(fy)) { r = quiet(fy); break; }
        if ((fy & 0x7F80) == 0x7F80) { r = op == kRndOp ? fy : kIndefinite; break; }
        const int mm = static_cast<int>(imm >> 4);
        const uint64_t rounded = round_scaled(fy, mm);
        if (op == kRndOp) { r = ((rounded & 0x7FFF) == 0) ? (fy & 0x8000) : rounded; break; }
        r = sub(kBf, fy, rounded, env);
        break;
      }
      case kGetMantOp: {
        const uint32_t sc = (imm >> 2) & 3, interval = imm & 3;
        if (nan_of(fy)) { r = quiet(fy); break; }
        const bool neg = fy & 0x8000;
        const bool zero = (fy & 0x7FFF) == 0, inf = (fy & 0x7FFF) == 0x7F80;
        const uint64_t signed_one = (sc & 1) ? 0x3F80 : 0xBF80;
        if (!neg && (zero || inf)) { r = 0x3F80; break; }
        if (neg) {
          if (zero) { r = signed_one; break; }
          if (inf) { r = (sc & 2) ? kIndefinite : signed_one; break; }
          if (sc & 2) { r = kIndefinite; break; }
        }
        uint64_t sign = (sc & 1) ? 0 : (fy & 0x8000);
        uint64_t fraction = fy & 0x7F;
        const int unbiased = static_cast<int>((fy >> 7) & 0xFF) - 127;
        const bool odd = unbiased & 1;
        const bool signaling_bit = (fraction >> 6) & 1;
        uint64_t e;
        switch (interval) {
          case 0: e = 127; break;
          case 1: e = odd ? 126 : 127; break;
          case 2: e = 126; break;
          default: e = signaling_bit ? 126 : 127; break;
        }
        r = sign | (e << 7) | fraction;
        break;
      }
      case kCmpOp: case kClassOp: break;
      default: break;
    }
    if (op == kCmpOp) {
      const Order o = compare(kBf, fx, fy, false, env);
      const bool unord = o == Order::Unordered, eq = o == Order::Equal, lt = o == Order::Less, gt = o == Order::Greater, ord = !unord;
      bool t = false;
      switch (imm & 15) {
        case 0: t = ord && eq; break;
        case 1: t = ord && lt; break;
        case 2: t = ord && (lt || eq); break;
        case 3: t = unord; break;
        case 4: t = unord || !eq; break;
        case 5: t = unord || !lt; break;
        case 6: t = unord || !(lt || eq); break;
        case 7: t = ord; break;
        case 8: t = unord || eq; break;
        case 9: t = unord || lt; break;
        case 10: t = unord || lt || eq; break;
        case 11: t = false; break;
        case 12: t = ord && !eq; break;
        case 13: t = ord && (gt || eq); break;
        case 14: t = ord && gt; break;
        default: t = true; break;
      }
      mask_bits |= uint64_t{t} << i;
    } else if (op == kClassOp) {
      const bool negative = y & 0x8000, ones = (y & 0x7F80) == 0x7F80, zeros = (y & 0x7F80) == 0;
      const bool mant_zero = zeros || (y & 0x7F) == 0;
      const bool snan = ones && !mant_zero && !((y >> 6) & 1), qnan = ones && !mant_zero && ((y >> 6) & 1);
      const bool zero = zeros;
      const bool t = ((imm & 1) && qnan) || ((imm & 2) && !negative && zero) || ((imm & 4) && negative && zero) || ((imm & 8) && !negative && ones && mant_zero) ||
                     ((imm & 0x10) && negative && ones && mant_zero) || ((imm & 0x40) && negative && !ones && !zero) || ((imm & 0x80) && snan);
      mask_bits |= uint64_t{t} << i;
    } else {
      set_element(result, 16, i, r);
    }
  }
  if (op == kCmpOp || op == kClassOp) ev_write_k(mask_bits, n);
  else ev_write_masked(reg, result, 16);
  s_.rip += d_.position;
  return Step::Ok;
}


// --- vminmax, vcomx, vucomx -------------------------------------------------------------------------------------------------------------

namespace {

struct Layout { uint32_t exp_bits, man_bits; Format format; };
Layout layout_of(uint32_t bits, bool bf16) {
  if (bits == 64) return {11, 52, Format::F64};
  if (bits == 32) return {8, 23, Format::F32};
  return bf16 ? Layout{8, 7, Format::BF16} : Layout{5, 10, Format::F16};
}

struct Num {
  uint64_t bits;
  Layout l;
  uint64_t exp() const { return (bits >> l.man_bits) & ((uint64_t{1} << l.exp_bits) - 1); }
  uint64_t man() const { return bits & ((uint64_t{1} << l.man_bits) - 1); }
  bool nan() const { return exp() == ((uint64_t{1} << l.exp_bits) - 1) && man() != 0; }
  bool snan() const { return nan() && !(man() >> (l.man_bits - 1)); }
  bool denormal() const { return exp() == 0 && man() != 0; }
  bool neg() const { return (bits >> (l.exp_bits + l.man_bits)) & 1; }
};

uint64_t quieted(const Layout& l, uint64_t bits) { return bits | (uint64_t{1} << (l.man_bits - 1)); }

}  // namespace

X86_64Cpu::Step X86_64Cpu::evex_minmax(GuestMemory& m, const EvexOp& def) {
  if (def.kind == kComx) {
    if (ev_.vvvv != 0) return fail(6);
    const uint32_t bits = def.elem;
    const Layout l = layout_of(bits, false);
    uint64_t y = 0;
    if (ev_.memory) {
      if (!load(m, memory_address(), bits / 8, &y)) return Step::Fault;
    } else {
      y = element(get_vec(ev_.rm), bits, 0);
    }
    uint64_t x = element(get_vec(ev_.reg), bits, 0);
    Env env;
    env.mode = Rounding::NearestEven;
    env.daz = bits != 16 && (s_.mxcsr & 0x40);
    Num nx{x, l}, ny{y, l};
    uint32_t flags = 0;
    if (env.daz) {
      if (nx.denormal()) x &= ~((uint64_t{1} << l.man_bits) - 1);
      if (ny.denormal()) y &= ~((uint64_t{1} << l.man_bits) - 1);
    }
    nx = Num{x, l}; ny = Num{y, l};
    const bool unordered = nx.nan() || ny.nan();
    if (nx.snan() || ny.snan() || (unordered && def.sub == 0)) flags |= kInvalid;
    else if (!unordered && (nx.denormal() || ny.denormal()) && !env.daz && bits != 16) flags |= kDenormal;
    uint64_t f = s_.rflags & ~(kOverflow | kSign | kZero | kParity | kCarry | kAdjust);
    if (unordered) f |= kOverflow | kSign | kParity | kCarry;
    else {
      const Order o = compare(l.format, x, y, false, env);
      if (o == Order::Less) f |= kOverflow | kCarry;
      else if (o == Order::Equal) f |= kOverflow | kSign | kZero;
    }
    s_.rflags = f;
    s_.mxcsr |= flags;
    s_.rip += d_.position;
    return Step::Ok;
  }
  // vminmax
  const bool bf16 = def.sub & 1, scalar = def.sub & 2;
  const uint32_t bits = def.elem, n = scalar ? 1 : ev_.vl / bits;
  const Layout l = layout_of(bits, bf16);
  const uint32_t imm = ev_.imm & 0xFF;
  Vec b;
  if (scalar) {
    if (ev_.memory) {
      uint64_t v = 0;
      if (!load(m, memory_address(), bits / 8, &v)) return Step::Fault;
      b.q[0] = v;
    } else {
      b = get_vec(ev_.rm);
    }
  } else if (!ev_read_rm(m, &b, bits, true, false)) {
    return Step::Fault;
  }
  const Vec a = get_vec(ev_.vvvv), dest = ev_.old_dest;
  Env env;
  env.mode = Rounding::NearestEven;
  const bool daz = bf16 || (bits != 16 && (s_.mxcsr & 0x40));
  uint32_t flags = 0;
  Vec result = scalar ? a : Vec();
  const uint32_t op_select = imm & 3, sign_control = (imm >> 2) & 3;
  const bool number = (imm >> 4) & 1;
  auto flush_daz = [&](uint64_t v) {
    Num nv{v, l};
    return (daz && nv.denormal()) ? v & ~((uint64_t{1} << l.man_bits) - 1) : v;
  };
  for (uint32_t i = 0; i < n; ++i) {
    if (ev_.aaa != 0 && !ev_active(i)) {
      set_element(result, bits, i, ev_.z ? 0 : element(dest, bits, i));
      continue;
    }
    uint64_t x = flush_daz(element(a, bits, i)), y = flush_daz(element(b, bits, scalar ? 0 : i));
    const Num nx{x, l}, ny{y, l};
    if (!bf16) {
      if (nx.snan() || ny.snan()) flags |= kInvalid;
      else if (!nx.nan() && !ny.nan() && (nx.denormal() || ny.denormal())) flags |= kDenormal;
    }
    uint64_t tmp;
    bool tmp_is_nan = false;
    if (nx.nan() || ny.nan()) {
      if (!number) {
        if (nx.nan()) tmp = nx.snan() ? quieted(l, x) : (ny.snan() ? quieted(l, y) : x);
        else tmp = quieted(l, y);
        tmp_is_nan = true;
      } else if (nx.nan() && ny.nan()) {
        tmp = nx.snan() ? quieted(l, x) : (ny.snan() ? quieted(l, y) : x);
        tmp_is_nan = true;
      } else {
        tmp = nx.nan() ? y : x;
      }
    } else {
      const bool want_max = op_select & 1, magnitude = op_select & 2;
      auto order_of = [&](uint64_t p, uint64_t q) { return compare(l.format, magnitude ? absolute(l.format, p) : p, magnitude ? absolute(l.format, q) : q, false, env); };
      Order o = order_of(x, y);
      uint64_t pick;
      if (o == Order::Equal && magnitude) o = compare(l.format, x, y, false, env);      // equal magnitudes: the signed comparison decides
      if (o == Order::Equal) {
        // equal values: for opposite signs (zeros, or equal magnitudes) the negative one is the minimum and the positive one the maximum
        if (nx.neg() == ny.neg()) pick = x;
        else pick = (nx.neg() == !want_max) ? x : y;
      } else {
        pick = (want_max ? o == Order::Greater : o == Order::Less) ? x : y;
      }
      tmp = pick;
    }
    if (!tmp_is_nan) {
      const uint64_t sign_bit = uint64_t{1} << (l.exp_bits + l.man_bits);
      if (sign_control == 3) tmp |= sign_bit;
      else if (sign_control == 2) tmp &= ~sign_bit;
      else if (sign_control == 1 || nx.nan()) { }
      else tmp = (tmp & ~sign_bit) | (x & sign_bit);
    }
    set_element(result, bits, i, tmp);
  }
  if (scalar) set_vec(ev_.reg, result, 128);
  else ev_write_masked(ev_.reg, result, bits);
  if (!bf16) s_.mxcsr |= flags;
  s_.rip += d_.position;
  return Step::Ok;
}


// --- FP8 (E5M2 "bf8" and E4M3 "hf8") conversions -----------------------------------------------------------------------------------------------

namespace {

// convert_fp16_to_bf8 / convert_fp16_to_bf8_bias; `bias` < 0 rounds to nearest even, otherwise it is the 8-bit rounding bias added below the fp16 LSB.
uint8_t fp16_to_bf8(uint32_t x, bool saturate, int bias) {
  const bool inf = (x & 0x7FFF) == 0x7C00, nan = (x & 0x7C00) == 0x7C00 && (x & 0x3FF) != 0;
  if (inf) return static_cast<uint8_t>(saturate ? ((x >> 8) & 0x80) | 0x7B : x >> 8);
  if (nan) return static_cast<uint8_t>((x >> 8) | 2);
  const uint32_t add = bias < 0 ? 0x7Fu + ((x >> 8) & 1) : static_cast<uint32_t>(bias);
  const uint32_t t = (x + add) & 0xFFFF;
  if (saturate && ((t >> 8) & 0x7F) == 0x7C) return static_cast<uint8_t>(((t >> 8) & 0x80) | 0x7B);
  return static_cast<uint8_t>(t >> 8);
}

// convert_fp16_to_hf8 / convert_fp16_to_hf8_bias
uint8_t fp16_to_hf8(uint32_t x, bool saturate, int bias) {
  constexpr uint32_t kRebias = 15 - 7;
  const uint32_t sign = (x & 0x8000) >> 8, e16 = (x >> 10) & 0x1F, m16 = x & 0x3FF;
  const uint32_t overflow_m = saturate ? 6 : 7;
  uint32_t e = 0, m = 0;
  if ((x & 0x7FFF) == 0x7C00) {
    e = 0xF;
    m = overflow_m;
  } else if (e16 == 0x1F) {      // NaN
    e = 0xF;
    m = 7;
  } else if (bias < 0) {
    if (e16 > kRebias + 15 || (e16 == kRebias + 15 && m16 > 0x340)) {
      e = 0xF;
      m = overflow_m;
    } else if (e16 == 0 && m16 == 0) {
      e = m = 0;
    } else if (e16 <= kRebias) {
      uint32_t t = (m16 | 0x400) >> (kRebias + 1 - e16);
      t |= ((m16 & 0x7F) + 0x7F) >> 7;      // the sticky bit
      t += 0x3F + ((t >> 7) & 1);
      e = t >> 10;
      m = (t >> 7) & 7;
    } else {
      const uint32_t r = x + 0x3F + ((m16 >> 7) & 1);
      e = ((r & 0x7C00) >> 10) - kRebias;
      m = (r & 0x3FF) >> 7;
    }
  } else {
    const uint32_t b = static_cast<uint32_t>(bias), xb = x + (b >> 1), e16b = (xb >> 10) & 0x1F, m16b = xb & 0x3FF;
    if (e16b > kRebias + 15 || (e16b == kRebias + 15 && m16b >= 0x380)) {
      e = 0xF;
      m = overflow_m;
    } else if (e16 == 0) {
      m = (m16 + (b << 7)) >> (kRebias + 7);
    } else if (e16b <= kRebias) {
      uint32_t t = (m16 | 0x400) + (b << (kRebias - e16));
      t >>= kRebias + 1 - e16;
      e = t >> 10;
      m = (t >> 7) & 7;
    } else {
      e = e16b - kRebias;
      m = m16b >> 7;
    }
  }
  return static_cast<uint8_t>((e << 3) | m | sign);
}

// convert_hf8_to_fp16
uint16_t hf8_to_fp16(uint32_t in) {
  const uint32_t s = (in & 0x80) << 8, e = (in & 0x78) >> 3;
  uint32_t m = in & 7, e_norm = e + (15 - 7);
  if (e == 0 && m != 0) {
    const uint32_t lz = m > 3 ? 0 : m > 1 ? 1 : 2;
    e_norm -= lz;
    m = (m << (lz + 1)) & 7;
  } else if (e == 0) {
    e_norm = 0;
  } else if (e == 0xF && m == 7) {
    e_norm = 0x1F;
  }
  return static_cast<uint16_t>(s | (e_norm << 10) | (m << 7));
}

}  // namespace

X86_64Cpu::Step X86_64Cpu::evex_fp8(GuestMemory& m, const EvexOp& def) {
  const bool hf8 = def.sub & 1, saturate = def.sub & 2;
  const uint32_t form = def.sub >> 2, vl = ev_.vl, reg = ev_.reg;
  if (ev_.er && form != 4) return fail(6);
  Vec b;
  const Vec a = get_vec(ev_.vvvv), old = ev_.old_dest;
  Vec result;
  auto to_fp8 = [&](uint32_t x, int bias) -> uint8_t { return hf8 ? fp16_to_hf8(x, saturate, bias) : fp16_to_bf8(x, saturate, bias); };
  if (form == 4) {      // vcvt2ps2phx
    if (!ev_read_rm(m, &b, 32, true, false)) return Step::Fault;
    Env env;
    env.mode = ev_.er ? static_cast<Rounding>(ev_.rc) : static_cast<Rounding>((s_.mxcsr >> 13) & 3);
    env.daz = s_.mxcsr & 0x40;
    const uint32_t n = vl / 16;
    for (uint32_t i = 0; i < n; ++i) {
      if (ev_.aaa != 0 && !ev_active(i)) {
        set_element(result, 16, i, ev_.z ? 0 : element(old, 16, i));
        continue;
      }
      const uint64_t src = i < n / 2 ? element(b, 32, i) : element(a, 32, i - n / 2);
      set_element(result, 16, i, convert(Format::F32, Format::F16, src, env));
    }
    set_vec(reg, result, vl);
    if (!ev_.er) s_.mxcsr |= env.flags;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (form == 3) {      // vcvthf82ph: the source is half the destination width
    if (ev_.vvvv != 0 || ev_.bcst) return fail(6);
    const uint32_t saved = ev_.vl;
    ev_.vl = vl / 2;
    const bool ok = ev_read_rm(m, &b, 8, false, false);
    ev_.vl = saved;
    if (!ok) return Step::Fault;
    for (uint32_t i = 0; i < vl / 16; ++i) {
      if (ev_.aaa != 0 && !ev_active(i)) set_element(result, 16, i, ev_.z ? 0 : element(old, 16, i));
      else set_element(result, 16, i, hf8_to_fp16(static_cast<uint32_t>(element(b, 8, i))));
    }
    set_vec(reg, result, vl);
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (form != 1 && form != 0 && form != 2) return fail(6);
  if (form == 0 && ev_.vvvv != 0) return fail(6);
  if (!ev_read_rm(m, &b, 16, true, false)) return Step::Fault;
  const uint32_t bytes = form == 1 ? vl / 8 : vl / 16;
  for (uint32_t i = 0; i < bytes; ++i) {
    if (ev_.aaa != 0 && !ev_active(i)) {
      set_element(result, 8, i, ev_.z ? 0 : element(old, 8, i));
      continue;
    }
    uint8_t r;
    if (form == 1) r = to_fp8(static_cast<uint32_t>(i < bytes / 2 ? element(b, 16, i) : element(a, 16, i - bytes / 2)), -1);
    else if (form == 0) r = to_fp8(static_cast<uint32_t>(element(b, 16, i)), -1);
    else r = to_fp8(static_cast<uint32_t>(element(b, 16, i)), static_cast<int>(element(a, 8, 2 * i)));
    set_element(result, 8, i, r);
  }
  set_vec(reg, result, form == 1 ? vl : (vl / 2 < 128 ? 128 : vl / 2));
  s_.rip += d_.position;
  return Step::Ok;
}


// --- vdpphps, vmpsadbw, the saturating conversions ----------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::evex_avx102(GuestMemory& m, const EvexOp& def) {
  const uint32_t reg = ev_.reg;
  if (def.kind == kDpPh) {
    if (ev_.er || ev_.w) return fail(6);
    Vec b;
    if (!ev_read_rm(m, &b, 32, true, false)) return Step::Fault;
    const Vec a = get_vec(ev_.vvvv), dest = ev_.old_dest;
    Vec result;
    Env env;
    env.mode = Rounding::NearestEven;
    env.daz = env.ftz = true;
    for (uint32_t i = 0; i < ev_.vl / 32; ++i) {
      const uint32_t acc0 = static_cast<uint32_t>(element(dest, 32, i));
      if (ev_.aaa != 0 && !ev_active(i)) {
        set_element(result, 32, i, ev_.z ? 0 : acc0);
        continue;
      }
      // the products in the order low, high; the first NaN of low*low + high*high + dest wins
      const uint32_t s1e = half::half_to_single(static_cast<uint16_t>(element(a, 16, 2 * i))), s1o = half::half_to_single(static_cast<uint16_t>(element(a, 16, 2 * i + 1)));
      const uint32_t s2e = half::half_to_single(static_cast<uint16_t>(element(b, 16, 2 * i))), s2o = half::half_to_single(static_cast<uint16_t>(element(b, 16, 2 * i + 1)));
      const uint32_t order[5] = {s1e, s2e, s1o, s2o, acc0};
      uint32_t nan = 0;
      bool have_nan = false;
      for (uint32_t v : order)
        if (!have_nan && (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFF)) { nan = v | 0x400000u; have_nan = true; }
      uint64_t acc = acc0;
      if (have_nan) {
        acc = nan;
      } else {
        if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;
        acc = fused_multiply_add(Format::F32, s1o, s2o, acc, env);
        if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;
        acc = fused_multiply_add(Format::F32, s1e, s2e, acc, env);
        if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;
      }
      set_element(result, 32, i, acc);
    }
    set_vec(reg, result, ev_.vl);
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (def.kind == kMpsadbw) {
    if (ev_.er || ev_.w) return fail(6);
    Vec b;
    if (!ev_read_rm(m, &b, 8, false, false)) return Step::Fault;
    const Vec a = get_vec(ev_.vvvv), dest = ev_.old_dest;
    Vec result;
    for (uint32_t lane = 0; lane < ev_.vl / 128; ++lane) {
      const uint32_t control = (ev_.imm >> ((lane & 1) * 3)) & 7;
      const uint32_t blk2 = (control & 3) * 4, blk1 = ((control >> 2) & 1) * 4;
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t word = lane * 8 + i;
        if (ev_.aaa != 0 && !ev_active(word)) {
          set_element(result, 16, word, ev_.z ? 0 : element(dest, 16, word));
          continue;
        }
        uint32_t sum = 0;
        for (uint32_t j = 0; j < 4; ++j) {
          const int x = static_cast<int>(element(a, 8, lane * 16 + blk1 + i + j)) - static_cast<int>(element(b, 8, lane * 16 + blk2 + j));
          sum += static_cast<uint32_t>(x < 0 ? -x : x);
        }
        set_element(result, 16, word, sum);
      }
    }
    set_vec(reg, result, ev_.vl);
    s_.rip += d_.position;
    return Step::Ok;
  }
  // saturating conversions
  const uint32_t src_type = def.sub & 3, dst_code = (def.sub >> 2) & 3;
  const bool is_unsigned = def.sub & 0x10, truncate = def.sub & 0x20, to_gpr = def.sub & 0x40;
  const uint32_t sw = src_type == 1 ? 64 : src_type == 0 ? 32 : 16;
  const Format f = src_type == 1 ? Format::F64 : src_type == 0 ? Format::F32 : src_type == 2 ? Format::F16 : Format::BF16;
  const bool bf16 = src_type == 3;
  const uint32_t dw = dst_code == 0 ? sw : dst_code == 1 ? 32 : 64;      // the destination element width
  const uint32_t int_bits = dst_code == 0 ? 8 : dw;
  Env env;
  env.mode = truncate ? Rounding::TowardZero : bf16 ? Rounding::NearestEven : (ev_.er ? static_cast<Rounding>(ev_.rc) : static_cast<Rounding>((s_.mxcsr >> 13) & 3));
  if (to_gpr) {
    if (ev_.vvvv != 0) return fail(6);
    uint64_t v = 0;
    if (ev_.memory) {
      if (!load(m, memory_address(), sw / 8, &v)) return Step::Fault;
    } else {
      v = element(get_vec(ev_.rm), sw, 0);
    }
    const uint32_t bits = ev_.w ? 64 : 32;
    const uint64_t r = to_integer_saturating(f, v, env.mode, bits, is_unsigned, 0, env);
    set_reg(d_.reg, bits == 64 ? 8 : 4, r);
    if (!bf16) s_.mxcsr |= env.flags;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (ev_.vvvv != 0) return fail(6);
  const uint32_t wide = sw > dw ? sw : dw;
  const uint32_t n = ev_.vl / wide;
  const uint32_t saved_vl = ev_.vl;
  Vec b;
  ev_.vl = n * sw < 128 ? 128 : n * sw;
  const bool ok = ev_read_rm(m, &b, sw, true, false);
  ev_.vl = saved_vl;
  if (!ok) return Step::Fault;
  Vec result;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t r = to_integer_saturating(f, element(b, sw, i), env.mode, int_bits, is_unsigned, 0, env);
    set_element(result, dw, i, r & width_mask(int_bits));
  }
  const uint32_t out_bits = n * dw < 128 ? 128 : n * dw;
  const Vec old = ev_.old_dest;
  if (ev_.aaa != 0)
    for (uint32_t i = 0; i < n; ++i)
      if (!ev_active(i)) set_element(result, dw, i, ev_.z ? 0 : element(old, dw, i));
  set_vec(reg, result, out_bits);
  if (!bf16 && !ev_.er) s_.mxcsr |= env.flags;
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
