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
#include "Emulation/AArch64SveCommon.hpp"
#include "Emulation/SoftFloat.hpp"

// SVE floating-point instructions (top bytes 0x64 and 0x65): arithmetic, fused multiply-add, compares, reductions, conversions, rounding, the trigonometric helpers (FTMAD, FEXPA, FTSMUL,
// FTSSEL), complex arithmetic and the by-element forms. The soft-float library does the arithmetic, so results do not depend on the host.
namespace emulation {

using namespace sve;
using namespace softfloat;

namespace {

Format format_of(uint32_t esz, bool bfloat) { return esz == 3 ? Format::F64 : esz == 2 ? Format::F32 : bfloat ? Format::BF16 : Format::F16; }

uint64_t sign_bit(uint32_t esz) { return uint64_t{1} << (element_bits(esz) - 1); }

// 0: +0.0, 1: 0.5, 2: 1.0, 3: 2.0
uint64_t constant(Format f, uint32_t which) {
  static const uint64_t k16[4] = {0, 0x3800, 0x3C00, 0x4000}, kb[4] = {0, 0x3F00, 0x3F80, 0x4000};
  static const uint64_t k32[4] = {0, 0x3F000000u, 0x3F800000u, 0x40000000u};
  static const uint64_t k64[4] = {0, 0x3FE0000000000000ull, 0x3FF0000000000000ull, 0x4000000000000000ull};
  return f == Format::F64 ? k64[which] : f == Format::F32 ? k32[which] : f == Format::BF16 ? kb[which] : k16[which];
}

// Coefficients of FTMAD (the sine series, then the cosine series) and the fractions of FEXPA, taken from the Arm architecture's tables.
const uint64_t kTrig16[16] = {0x3C00, 0xB155, 0x2030, 0, 0, 0, 0, 0, 0x3C00, 0xB800, 0x293A, 0, 0, 0, 0, 0};
const uint64_t kTrig32[16] = {0x3F800000, 0xBE2AAAAB, 0x3C088886, 0xB95008B9, 0x36369D6D, 0, 0, 0, 0x3F800000, 0xBF000000, 0x3D2AAAA6, 0xBAB60705, 0x37CD37CC, 0, 0, 0};
const uint64_t kTrig64[16] = {0x3FF0000000000000ull, 0xBFC5555555555543ull, 0x3F8111111110F30Cull, 0xBF2A01A019B92FC6ull, 0x3EC71DE351F3D22Bull, 0xBE5AE5E2B60F7B91ull, 0x3DE5D8408868552Full, 0,
                              0x3FF0000000000000ull, 0xBFE0000000000000ull, 0x3FA5555555555536ull, 0xBF56C16C16C13A0Bull, 0x3EFA01A019B1E8D8ull, 0xBE927E4F7282F468ull, 0x3E21EE96D2641B13ull, 0xBDA8F76380FBB401ull};
const uint16_t kExpa16[32] = {0x000, 0x016, 0x02D, 0x045, 0x05D, 0x075, 0x08E, 0x0A8, 0x0C2, 0x0DC, 0x0F8, 0x114, 0x130, 0x14D, 0x16B, 0x189,
                              0x1A8, 0x1C8, 0x1E8, 0x209, 0x22B, 0x24E, 0x271, 0x295, 0x2BA, 0x2E0, 0x306, 0x32E, 0x356, 0x37F, 0x3A9, 0x3D4};
const uint32_t kExpa32[64] = {0x000000, 0x0164D2, 0x02CD87, 0x043A29, 0x05AAC3, 0x071F62, 0x08980F, 0x0A14D5, 0x0B95C2, 0x0D1ADF, 0x0EA43A, 0x1031DC, 0x11C3D3, 0x135A2B, 0x14F4F0, 0x16942D,
                              0x1837F0, 0x19E046, 0x1B8D3A, 0x1D3EDA, 0x1EF532, 0x20B051, 0x227043, 0x243516, 0x25FED7, 0x27CD94, 0x29A15B, 0x2B7A3A, 0x2D583F, 0x2F3B79, 0x3123F6, 0x3311C4,
                              0x3504F3, 0x36FD92, 0x38FBAF, 0x3AFF5B, 0x3D08A4, 0x3F179A, 0x412C4D, 0x4346CD, 0x45672A, 0x478D75, 0x49B9BE, 0x4BEC15, 0x4E248C, 0x506334, 0x52A81E, 0x54F35B,
                              0x5744FD, 0x599D16, 0x5BFBB8, 0x5E60F5, 0x60CCDF, 0x633F89, 0x65B907, 0x68396A, 0x6AC0C7, 0x6D4F30, 0x6FE4BA, 0x728177, 0x75257D, 0x77D0DF, 0x7A83B3, 0x7D3E0C};
const uint64_t kExpa64[64] = {
    0x0000000000000ull, 0x2C9A3E778061ull, 0x59B0D3158574ull, 0x874518759BC8ull, 0xB5586CF9890Full, 0xE3EC32D3D1A2ull, 0x11301D0125B51ull, 0x1429AAEA92DE0ull,
    0x172B83C7D517Bull, 0x1A35BEB6FCB75ull, 0x1D4873168B9AAull, 0x2063B88628CD6ull, 0x2387A6E756238ull, 0x26B4565E27CDDull, 0x29E9DF51FDEE1ull, 0x2D285A6E4030Bull,
    0x306FE0A31B715ull, 0x33C08B26416FFull, 0x371A7373AA9CBull, 0x3A7DB34E59FF7ull, 0x3DEA64C123422ull, 0x4160A21F72E2Aull, 0x44E086061892Dull, 0x486A2B5C13CD0ull,
    0x4BFDAD5362A27ull, 0x4F9B2769D2CA7ull, 0x5342B569D4F82ull, 0x56F4736B527DAull, 0x5AB07DD485429ull, 0x5E76F15AD2148ull, 0x6247EB03A5585ull, 0x6623882552225ull,
    0x6A09E667F3BCDull, 0x6DFB23C651A2Full, 0x71F75E8EC5F74ull, 0x75FEB564267C9ull, 0x7A11473EB0187ull, 0x7E2F336CF4E62ull, 0x82589994CCE13ull, 0x868D99B4492EDull,
    0x8ACE5422AA0DBull, 0x8F1AE99157736ull, 0x93737B0CDC5E5ull, 0x97D829FDE4E50ull, 0x9C49182A3F090ull, 0xA0C667B5DE565ull, 0xA5503B23E255Dull, 0xA9E6B5579FDBFull,
    0xAE89F995AD3ADull, 0xB33A2B84F15FBull, 0xB7F76F2FB5E47ull, 0xBCC1E904BC1D2ull, 0xC199BDD85529Cull, 0xC67F12E57D14Bull, 0xCB720DCEF9069ull, 0xD072D4A07897Cull,
    0xD5818DCFBA487ull, 0xDA9E603DB3285ull, 0xDFC97337B9B5Full, 0xE502EE78B3FF6ull, 0xEA4AFA2A490DAull, 0xEFA1BEE615A27ull, 0xF50765B6E4540ull, 0xFA7C1819E90D8ull};

// FEXPA: the exponent field comes from the operand's upper bits, the fraction from the table.
uint64_t fexpa(uint32_t esz, uint64_t x) {
  switch (esz) {
    case 1: return ((x >> 5) & 0x1F) << 10 | kExpa16[x & 31];
    case 2: return ((x >> 6) & 0xFF) << 23 | kExpa32[x & 63];
    default: return ((x >> 6) & 0x7FF) << 52 | kExpa64[x & 63];
  }
}

// FSCALE: x * 2^n with a single rounding (FPScale).
uint64_t fscale(Format f, uint64_t x, int64_t n, Env& env) {
  const uint32_t E = f == Format::F64 ? 11 : f == Format::F32 ? 8 : f == Format::F16 ? 5 : 8, M = f == Format::F64 ? 52 : f == Format::F32 ? 23 : f == Format::F16 ? 10 : 7;
  const uint64_t sign = x & (uint64_t{1} << (E + M));
  const int64_t exp_field = static_cast<int64_t>((x >> M) & ones(E));
  uint64_t frac = x & ones(M);
  if (exp_field == static_cast<int64_t>(ones(E))) {      // infinity or NaN
    if (frac == 0) return x;
    if (!((frac >> (M - 1)) & 1)) env.flags |= kInvalid;
    return x | (uint64_t{1} << (M - 1));
  }
  if (exp_field == 0 && frac == 0) return x;
  const int64_t bias = (int64_t{1} << (E - 1)) - 1;
  // x = mantissa * 2^(exp - M) with the mantissa carrying the implicit bit; normalise a denormal first
  uint64_t mantissa = exp_field == 0 ? frac : (frac | (uint64_t{1} << M));
  int64_t exp = exp_field == 0 ? 1 - bias : exp_field - bias;
  while (!((mantissa >> M) & 1)) { mantissa <<= 1; --exp; }
  const int64_t limit = int64_t{1} << (E + 2);
  n = n > limit ? limit : n < -limit ? -limit : n;
  exp += n;
  // the biased exponent of the result
  int64_t biased = exp + bias;
  const bool negative = sign != 0;
  auto round_up = [&](bool lsb, bool guard, bool sticky) {
    switch (env.mode) {
      case Rounding::NearestEven: return guard && (sticky || lsb);
      case Rounding::TieAway: return guard;
      case Rounding::Up: return !negative && (guard || sticky);
      case Rounding::Down: return negative && (guard || sticky);
      default: return false;
    }
  };
  if (biased >= static_cast<int64_t>(ones(E))) {      // overflow
    env.flags |= kOverflow | kInexact;
    const bool to_inf = env.mode == Rounding::NearestEven || env.mode == Rounding::TieAway || (env.mode == Rounding::Up && !negative) || (env.mode == Rounding::Down && negative);
    return to_inf ? (sign | (ones(E) << M)) : (sign | ((ones(E) - 1) << M) | ones(M));
  }
  if (biased >= 1) return sign | (static_cast<uint64_t>(biased) << M) | (mantissa & ones(M));
  // a denormal result: shift right with rounding
  const int64_t shift = 1 - biased;
  uint64_t result = 0;
  bool guard = false, sticky = false;
  if (shift > static_cast<int64_t>(M) + 2) {
    sticky = true;
  } else {
    result = mantissa >> shift;
    guard = ((mantissa >> (shift - 1)) & 1) != 0;
    sticky = (mantissa & ones(static_cast<uint32_t>(shift - 1))) != 0;
  }
  const bool inexact = guard || sticky;
  if (inexact) env.flags |= kUnderflow | kInexact;
  if (round_up(result & 1, guard, sticky)) ++result;
  return sign | result;
}

// FLOGB: the exponent of x as an integer of the element size.
uint64_t flogb(Format f, uint64_t x, uint32_t w, Env& env) {
  const uint32_t E = f == Format::F64 ? 11 : f == Format::F32 ? 8 : 5, M = f == Format::F64 ? 52 : f == Format::F32 ? 23 : 10;
  const int64_t exp_field = static_cast<int64_t>((x >> M) & ones(E)), bias = (int64_t{1} << (E - 1)) - 1;
  uint64_t frac = x & ones(M);
  const uint64_t minimum = uint64_t{1} << (w - 1);
  if (exp_field == static_cast<int64_t>(ones(E)) || (exp_field == 0 && frac == 0)) {      // NaN, infinity, zero
    env.flags |= kInvalid;
    return minimum;
  }
  if (exp_field == 0) {
    int64_t e = 1 - bias;
    while (!((frac >> (M - 1)) & 1)) { frac <<= 1; --e; }
    return static_cast<uint64_t>(e - 1) & ones(w);
  }
  return static_cast<uint64_t>(exp_field - bias) & ones(w);
}

}  // namespace

uint64_t sve::sve_expa(uint32_t esz, uint64_t x) { return fexpa(esz, x); }

bool AArch64Cpu::sve_float(uint32_t insn) {
  const bool is65 = (insn >> 24) & 1;
  const uint32_t size = bits_of(insn, 23, 22);
  const bool bf_size = size == 0;      // (size 00 is BFloat16 in the few instructions that have it; its elements are 16 bits wide)
  const uint32_t esz = bf_size ? 1 : size;
  const uint32_t zd = insn & 31, zn = bits_of(insn, 9, 5), zm = bits_of(insn, 20, 16), pg = bits_of(insn, 12, 10);
  Vec* const dst = &s_.v[zd];
  const uint32_t elements = element_count(esz);
  Env env = fp_env();
  auto finish = [&]() { fp_finish(env); return true; };
  auto nan_fix = [&](Format f, uint64_t v) { return fp_default_nan_fix(f, v); };
  const bool b21 = (insn >> 21) & 1;

  if (is65) {
    const uint32_t group = bits_of(insn, 15, 13);
    if (b21) {      // fused multiply-add
      const Format f = format_of(esz, bf_size);
      const uint16_t gp = s_.p[pg];
      const uint32_t op = bits_of(insn, 14, 13);
      if (bf_size && (op > 1 || ((insn >> 15) & 1))) return false;
      if (!((insn >> 15) & 1)) {      // FMLA, FMLS, FNMLA, FNMLS: Zda += Zn * Zm
        const Vec a = s_.v[zn], b = s_.v[zm];
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          uint64_t x = get(a, esz, i);
          uint64_t acc = get(*dst, esz, i);
          if (op == 1 || op == 2) x = negate(f, x);
          if (op >= 2) acc = negate(f, acc);
          put(dst, esz, i, nan_fix(f, fused_multiply_add(f, x, get(b, esz, i), acc, env)));
        }
        return finish();
      }
      // FMAD, FMSB, FNMAD, FNMSB: Zdn = Za + Zdn * Zm
      const Vec m = s_.v[zn], addend = s_.v[zm];
      for (uint32_t i = 0; i < elements; ++i) {
        if (!active(gp, i, esz)) continue;
        uint64_t x = get(*dst, esz, i);
        uint64_t acc = get(addend, esz, i);
        if (op == 1 || op == 2) x = negate(f, x);
        if (op >= 2) acc = negate(f, acc);
        put(dst, esz, i, nan_fix(f, fused_multiply_add(f, x, get(m, esz, i), acc, env)));
      }
      return finish();
    }

    switch (group) {
      case 0: {      // unpredicated FADD, FSUB, FMUL, FTSMUL, FRECPS, FRSQRTS
        const uint32_t op = bits_of(insn, 12, 10);
        const Format f = format_of(esz, bf_size);
        if (bf_size && op > 2) return false;
        const Vec a = s_.v[zn], b = s_.v[zm];
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t x = get(a, esz, i), y = get(b, esz, i);
          uint64_t r;
          switch (op) {
            case 0: r = add(f, x, y, env); break;
            case 1: r = softfloat::sub(f, x, y, env); break;
            case 2: r = mul(f, x, y, env); break;
            case 3: {      // FTSMUL
              r = mul(f, x, x, env);
              if (!is_nan(f, r)) r = (r & ~sign_bit(esz)) | ((y & 1) ? sign_bit(esz) : 0);
              break;
            }
            case 6: case 7: {      // FRECPS: 2 - x*y; FRSQRTS: (3 - x*y) / 2
              const bool inf_zero = (absolute(f, x) == (esz == 3 ? 0x7FF0000000000000ull : esz == 2 ? 0x7F800000u : 0x7C00u) && absolute(f, y) == 0) ||
                                    (absolute(f, y) == (esz == 3 ? 0x7FF0000000000000ull : esz == 2 ? 0x7F800000u : 0x7C00u) && absolute(f, x) == 0);
              const uint64_t two = constant(f, 3), one = constant(f, 2), half = constant(f, 1);
              if (op == 6) {
                if (!(is_nan(f, x) || is_nan(f, y)) && inf_zero) r = two;
                else r = fused_multiply_add(f, negate(f, x), y, two, env);
              } else {
                const uint64_t three = add(f, two, one, env), one_half = add(f, one, half, env);
                if (!(is_nan(f, x) || is_nan(f, y)) && inf_zero) r = one_half;
                else {
                  // (3 - x*y) / 2 is rounded once in the architecture: when the doubled value would overflow, evaluate it as 1.5 - (x/2)*y instead
                  Env trial = env;
                  trial.flags = 0;
                  const uint64_t doubled = fused_multiply_add(f, negate(f, x), y, three, trial);
                  if (trial.flags & kOverflow) r = fused_multiply_add(f, negate(f, mul(f, x, half, env)), y, one_half, env);
                  else r = mul(f, fused_multiply_add(f, negate(f, x), y, three, env), half, env);
                  (void)doubled;
                }
              }
              break;
            }
            default: return false;
          }
          put(dst, esz, i, nan_fix(f, r));
        }
        return finish();
      }
      case 1: {      // reductions, FADDA, FRECPE / FRSQRTE, compares with zero
        const uint32_t sel = bits_of(insn, 20, 16);
        const Format f = format_of(esz, false);
        if (bf_size) return false;
        if (sel == 0 || (sel >= 4 && sel <= 7) || sel == 24) {
          if (bits_of(insn, 12, 10) > 7) return false;
          const uint16_t gp = s_.p[bits_of(insn, 12, 10)];
          const Vec src = s_.v[zn];
          if (sel == 24) {      // FADDA: strictly ordered, accumulating into the scalar
            uint64_t acc = get(s_.v[zd], esz, 0);
            for (uint32_t i = 0; i < elements; ++i)
              if (active(gp, i, esz)) acc = add(f, acc, get(src, esz, i), env);
            s_.v[zd] = Vec{nan_fix(f, acc), 0};
            return finish();
          }
          uint64_t values[16];
          const uint64_t inf = esz == 3 ? 0x7FF0000000000000ull : esz == 2 ? 0x7F800000u : 0x7C00u, qnan = esz == 3 ? 0x7FF8000000000000ull : esz == 2 ? 0x7FC00000u : 0x7E00u;
          uint64_t identity = 0;
          switch (sel) {
            case 0: identity = 0; break;
            case 4: identity = qnan; break;
            case 5: identity = qnan; break;
            case 6: identity = inf | sign_bit(esz); break;
            default: identity = inf; break;
          }
          for (uint32_t i = 0; i < elements; ++i) values[i] = active(gp, i, esz) ? get(src, esz, i) : identity;
          for (uint32_t n = elements; n > 1; n /= 2) {
            for (uint32_t i = 0; i < n / 2; ++i) {
              const uint64_t x = values[2 * i], y = values[2 * i + 1];
              switch (sel) {
                case 0: values[i] = add(f, x, y, env); break;
                case 4: values[i] = arm_minmax(f, x, y, true, true, env); break;
                case 5: values[i] = arm_minmax(f, x, y, false, true, env); break;
                case 6: values[i] = arm_minmax(f, x, y, true, false, env); break;
                default: values[i] = arm_minmax(f, x, y, false, false, env); break;
              }
            }
          }
          s_.v[zd] = Vec{nan_fix(f, values[0]), 0};
          return finish();
        }
        if (sel == 14 || sel == 15) {      // FRECPE, FRSQRTE (bits 12:10 = 100)
          if (bits_of(insn, 12, 10) != 4) return false;
          const Vec src = s_.v[zn];
          for (uint32_t i = 0; i < elements; ++i) {
            const uint64_t x = get(src, esz, i);
            put(dst, esz, i, sel == 14 ? fp_recip_estimate(f, x, env, false) : fp_recip_sqrt_estimate(f, x, env, fp_default_nan(f)));
          }
          return finish();
        }
        if (sel >= 16 && sel <= 19) {      // compares with zero: FCMGE / FCMGT, FCMLT / FCMLE, FCMEQ, FCMNE
          const bool o = (insn >> 4) & 1;
          if (sel >= 18 && o) return false;
          const uint16_t gp = s_.p[pg];
          const Vec src = s_.v[zn];
          uint16_t r = 0;
          for (uint32_t i = 0; i < elements; ++i) {
            if (!active(gp, i, esz)) continue;
            const uint64_t x = get(src, esz, i);
            bool t;
            switch (sel) {
              case 16: { const Order c = compare(f, x, 0, true, env); t = o ? c == Order::Greater : (c == Order::Greater || c == Order::Equal); break; }
              case 17: { const Order c = compare(f, x, 0, true, env); t = o ? (c == Order::Less || c == Order::Equal) : c == Order::Less; break; }
              case 18: { const Order c = compare(f, x, 0, false, env); t = c == Order::Equal; break; }
              default: { const Order c = compare(f, x, 0, false, env); t = c != Order::Equal; break; }
            }
            if (t) r = static_cast<uint16_t>(r | (1u << (i << esz)));
          }
          s_.p[insn & 15] = r;
          return finish();      // (FP compares leave the flags alone)
        }
        return false;
      }
      case 2: case 3: case 6: case 7: {      // vector compares
        if (bf_size) return false;
        const Format f = format_of(esz, false);
        const bool o = (insn >> 4) & 1;
        const uint16_t gp = s_.p[pg];
        const Vec a = s_.v[zn], b = s_.v[zm];
        uint16_t r = 0;
        if (group == 7 && !o) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const uint64_t x = get(a, esz, i), y = get(b, esz, i);
          bool t;
          switch (group) {
            case 2: { const Order c = compare(f, x, y, true, env); t = o ? c == Order::Greater : (c == Order::Greater || c == Order::Equal); break; }
            case 3: { const Order c = compare(f, x, y, false, env); t = o ? c != Order::Equal : c == Order::Equal; break; }
            case 6: {
              if (!o) { const Order c = compare(f, x, y, false, env); t = c == Order::Unordered; }
              else { const Order c = compare(f, absolute(f, x), absolute(f, y), true, env); t = c == Order::Greater || c == Order::Equal; }
              break;
            }
            default: { const Order c = compare(f, absolute(f, x), absolute(f, y), true, env); t = c == Order::Greater; break; }
          }
          if (t) r = static_cast<uint16_t>(r | (1u << (i << esz)));
        }
        s_.p[insn & 15] = r;
        return finish();
      }
      case 4: {      // predicated arithmetic, with a vector or an immediate, and FTMAD
        const uint16_t gp = s_.p[pg];
        const uint32_t kind = bits_of(insn, 20, 19);
        if (kind == 2) {      // FTMAD Zdn, Zdn, Zm, #imm3
          if (bf_size || bits_of(insn, 12, 10) != 0) return false;
          const Format f = format_of(esz, false);
          const uint32_t imm = bits_of(insn, 18, 16);
          const Vec m = s_.v[zn];
          const uint64_t* const table = esz == 1 ? kTrig16 : esz == 2 ? kTrig32 : kTrig64;
          for (uint32_t i = 0; i < elements; ++i) {
            uint64_t y = get(m, esz, i);
            uint32_t k = imm;
            if (y & sign_bit(esz)) { k += 8; y = absolute(f, y); }
            put(dst, esz, i, nan_fix(f, fused_multiply_add(f, get(*dst, esz, i), y, table[k], env)));      // coefficient + x * |y|
          }
          return finish();
        }
        const Format f = format_of(esz, bf_size);
        if (kind == 3) {      // immediate forms: bit 5 selects the second constant
          if (bits_of(insn, 9, 6) != 0 || bf_size) return false;
          const uint32_t op = bits_of(insn, 18, 16);
          const bool second = (insn >> 5) & 1;
          const uint64_t imm = op <= 1 || op == 3 ? constant(f, second ? 2 : 1) : op == 2 ? constant(f, second ? 3 : 1) : constant(f, second ? 2 : 0);
          for (uint32_t i = 0; i < elements; ++i) {
            if (!active(gp, i, esz)) continue;
            const uint64_t x = get(*dst, esz, i);
            uint64_t r;
            switch (op) {
              case 0: r = add(f, x, imm, env); break;
              case 1: r = softfloat::sub(f, x, imm, env); break;
              case 2: r = mul(f, x, imm, env); break;
              case 3: r = softfloat::sub(f, imm, x, env); break;
              case 4: r = arm_minmax(f, x, imm, true, true, env); break;
              case 5: r = arm_minmax(f, x, imm, false, true, env); break;
              case 6: r = arm_minmax(f, x, imm, true, false, env); break;
              default: r = arm_minmax(f, x, imm, false, false, env); break;
            }
            put(dst, esz, i, nan_fix(f, r));
          }
          return finish();
        }
        const uint32_t op = bits_of(insn, 19, 16);
        if (bits_of(insn, 20, 20) != 0) return false;
        const Vec b = s_.v[zn];
        if (bf_size && !(op <= 7 || op == 9)) return false;
        if (op == 11) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const uint64_t x = get(*dst, esz, i), y = get(b, esz, i);
          uint64_t r;
          switch (op) {
            case 0: r = add(f, x, y, env); break;
            case 1: r = softfloat::sub(f, x, y, env); break;
            case 2: r = mul(f, x, y, env); break;
            case 3: r = softfloat::sub(f, y, x, env); break;
            case 4: r = arm_minmax(f, x, y, true, true, env); break;
            case 5: r = arm_minmax(f, x, y, false, true, env); break;
            case 6: r = arm_minmax(f, x, y, true, false, env); break;
            case 7: r = arm_minmax(f, x, y, false, false, env); break;
            case 8: r = absolute(f, softfloat::sub(f, x, y, env)); break;
            case 9: r = fscale(f, x, sext(y, element_bits(esz)), env); break;
            case 10: {      // FMULX: 2.0 for infinity times zero
              const uint64_t inf = esz == 3 ? 0x7FF0000000000000ull : esz == 2 ? 0x7F800000u : 0x7C00u;
              const bool inf_zero = !is_nan(f, x) && !is_nan(f, y) && ((absolute(f, x) == inf && absolute(f, y) == 0) || (absolute(f, x) == 0 && absolute(f, y) == inf));
              r = inf_zero ? (((x ^ y) & sign_bit(esz)) ? negate(f, constant(f, 3)) : constant(f, 3)) : mul(f, x, y, env);
              break;
            }
            case 12: r = softfloat::div(f, y, x, env); break;
            case 13: r = softfloat::div(f, x, y, env); break;
            case 14: case 15: {      // FAMAX, FAMIN
              const bool nan = is_nan(f, x) || is_nan(f, y);
              r = arm_minmax(f, nan ? x : absolute(f, x), nan ? y : absolute(f, y), op == 14, false, env);
              break;
            }
            default: return false;
          }
          put(dst, esz, i, nan_fix(f, r));
        }
        return finish();
      }
      case 5: {      // predicated unary: rounding, square root, conversions
        const uint16_t gp = s_.p[pg];
        const uint32_t sel = bits_of(insn, 20, 16);
        const Vec src = s_.v[zn];
        // conversions between precisions
        auto convert_op = [&](uint32_t container, Format from, Format to, uint32_t from_bits, bool to_odd, bool take_odd, bool put_odd) {
          // `container` is the element size the predicate and the loop use; the narrower value sits in the low bits (or the odd half for the "top" forms)
          for (uint32_t i = 0; i < element_count(container); ++i) {
            if (!active(gp, i, container)) continue;
            uint64_t x = get(src, container, i);
            if (take_odd) x >>= from_bits;
            x &= ones(from_bits);
            uint64_t r;
            if (to_odd) {
              Env odd = env;
              odd.mode = Rounding::TowardZero;
              odd.flags = 0;
              r = convert(from, to, x, odd);
              if ((odd.flags & kInexact) && !is_nan(to, r) && (r & 0x7FFFFFFFu) != 0x7F800000u) r |= 1;
              env.flags |= odd.flags;
            } else {
              r = convert(from, to, x, env);
            }
            r = nan_fix(to, r);
            const uint32_t to_bits = element_bits(container) / (put_odd ? 2 : 1);
            if (put_odd) {
              const uint64_t old = get(*dst, container, i);
              put(dst, container, i, (old & ones(to_bits)) | (r << to_bits));
            } else {
              put(dst, container, i, r);
            }
          }
        };
        (void)convert_op;
        const uint32_t hi = bits_of(insn, 23, 16);
        // FCVT family (bits 23:16 = 10001000 .. 11001011), BFCVT (0x8A), FCVTX (0x0A)
        if (hi == 0x88 || hi == 0x89 || hi == 0xC8 || hi == 0xC9 || hi == 0xCA || hi == 0xCB || hi == 0x8A || hi == 0x0A) {
          if (hi == 0x88) convert_op(2, Format::F32, Format::F16, 32, false, false, false);
          else if (hi == 0x89) convert_op(2, Format::F16, Format::F32, 16, false, false, false);
          else if (hi == 0xC8) convert_op(3, Format::F64, Format::F16, 64, false, false, false);
          else if (hi == 0xC9) convert_op(3, Format::F16, Format::F64, 16, false, false, false);
          else if (hi == 0xCA) convert_op(3, Format::F64, Format::F32, 64, false, false, false);
          else if (hi == 0xCB) convert_op(3, Format::F32, Format::F64, 32, false, false, false);
          else if (hi == 0x8A) convert_op(2, Format::F32, Format::BF16, 32, false, false, false);
          else convert_op(3, Format::F64, Format::F32, 64, true, false, false);
          return finish();
        }
        // integer conversions: bits 20:19 = 10 (SCVTF / UCVTF) or 11 (FCVTZS / FCVTZU), bits 18:17 the size pair, bit 16 the signedness
        if (bits_of(insn, 21, 19) == 2 || bits_of(insn, 21, 19) == 3) {
          if (bits_of(insn, 21, 21) != 0) return false;
          const bool to_float = bits_of(insn, 20, 19) == 2;
          const uint32_t opc = size, opc2 = bits_of(insn, 18, 17);
          const bool is_unsigned = (insn & (1u << 16)) != 0;
          uint32_t fp_esz, int_esz;
          switch ((opc << 2) | opc2) {
            case (1u << 2) | 1: fp_esz = 1; int_esz = 1; break;
            case (1u << 2) | 2: fp_esz = 1; int_esz = 2; break;
            case (1u << 2) | 3: fp_esz = 1; int_esz = 3; break;
            case (2u << 2) | 2: fp_esz = 2; int_esz = 2; break;
            case (3u << 2) | 0: fp_esz = to_float ? 3 : 3; int_esz = 2; break;
            case (3u << 2) | 2: fp_esz = 2; int_esz = 3; break;
            case (3u << 2) | 3: fp_esz = 3; int_esz = 3; break;
            default: return false;
          }
          if (!to_float && opc == 3 && opc2 == 0) { fp_esz = 3; int_esz = 2; }
          const Format f = format_of(fp_esz, false);
          const uint32_t container = fp_esz > int_esz ? fp_esz : int_esz;
          for (uint32_t i = 0; i < element_count(container); ++i) {
            if (!active(gp, i, container)) continue;
            const uint64_t x = get(src, container, i);
            uint64_t r;
            if (to_float) {
              const uint32_t w = element_bits(int_esz);
              const uint64_t v = x & ones(w);
              if (is_unsigned) r = from_integer(f, v, false, 0, env);
              else {
                const int64_t sv = sext(v, w);
                r = from_integer(f, sv < 0 ? uint64_t{0} - static_cast<uint64_t>(sv) : static_cast<uint64_t>(sv), sv < 0, 0, env);
              }
              r = nan_fix(f, r);
            } else {
              r = to_integer_saturating(f, x & ones(element_bits(fp_esz)), Rounding::TowardZero, element_bits(int_esz), is_unsigned, 0, env);
              if (!is_unsigned) r = static_cast<uint64_t>(sext(r, element_bits(int_esz)));      // (a narrow signed result is sign-extended into the wider container)
            }
            put(dst, container, i, r & ones(element_bits(container)));
          }
          return finish();
        }
        // FLOGB: bits 23:19 = 00011, the size in bits 18:17
        if (bits_of(insn, 23, 19) == 3) {
          const uint32_t e = bits_of(insn, 18, 17);
          if (e == 0 || (insn >> 16) & 1) return false;
          const Format f = format_of(e, false);
          for (uint32_t i = 0; i < element_count(e); ++i)
            if (active(gp, i, e)) put(dst, e, i, flogb(f, get(src, e, i), element_bits(e), env));
          return finish();
        }
        // FRINT32Z .. FRINT64X: bits 23:19 = 00010
        if (bits_of(insn, 23, 19) == 2) {
          const uint32_t e = (insn >> 18) & 1 ? 3 : 2;
          const bool bits64 = (insn >> 17) & 1, exact = (insn >> 16) & 1;
          const Format f = format_of(e, false);
          for (uint32_t i = 0; i < element_count(e); ++i)
            if (active(gp, i, e)) put(dst, e, i, nan_fix(f, frint_to_width(f, get(src, e, i), bits64 ? 64 : 32, Rounding::TowardZero, exact, env)));
          return finish();
        }
        if (bf_size || bits_of(insn, 21, 20) != 0) return false;
        const Format f = format_of(esz, false);
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const uint64_t x = get(src, esz, i);
          uint64_t r;
          switch (sel) {
            case 0: r = round_to_integral(f, x, Rounding::NearestEven, false, env); break;
            case 1: r = round_to_integral(f, x, Rounding::Up, false, env); break;
            case 2: r = round_to_integral(f, x, Rounding::Down, false, env); break;
            case 3: r = round_to_integral(f, x, Rounding::TowardZero, false, env); break;
            case 4: r = round_to_integral(f, x, Rounding::TieAway, false, env); break;
            case 6: r = round_to_integral(f, x, env.mode, true, env); break;
            case 7: r = round_to_integral(f, x, env.mode, false, env); break;
            case 13: r = softfloat::sqrt(f, x, env); break;
            case 12: {      // FRECPX
              const FpShape s = shape_of(f);
              if (is_nan(f, x)) { if (!((x >> (s.frac_bits - 1)) & 1)) env.flags |= kInvalid; r = x | (uint64_t{1} << (s.frac_bits - 1)); }
              else r = (x & (uint64_t{1} << (s.exp_bits + s.frac_bits))) | ((((x >> s.frac_bits) & ones(s.exp_bits)) == 0 ? ones(s.exp_bits) - 1 : (~(x >> s.frac_bits) & ones(s.exp_bits))) << s.frac_bits);
              break;
            }
            default: return false;
          }
          put(dst, esz, i, nan_fix(f, r));
        }
        return finish();
      }
      default: return false;
    }
  }

  // 0x64
  {
    const uint16_t gp = s_.p[pg];
    if (b21) {
      const uint32_t op = bits_of(insn, 15, 10);
      if ((op & 0x3E) == 0 || op == 0x08) {      // FMLA / FMLS / FMUL by element
        uint32_t index, zmr;
        uint32_t e;
        if (size == 0 || size == 1) { e = 1; index = ((size & 1) << 2) | bits_of(insn, 20, 19); zmr = bits_of(insn, 18, 16); }
        else if (size == 2) { e = 2; index = bits_of(insn, 20, 19); zmr = bits_of(insn, 18, 16); }
        else { e = 3; index = (insn >> 20) & 1; zmr = bits_of(insn, 19, 16); }
        const Format f = format_of(e, false);
        const uint64_t y = get(s_.v[zmr], e, index);
        const Vec a = s_.v[zn];
        for (uint32_t i = 0; i < element_count(e); ++i) {
          const uint64_t x = get(a, e, i);
          uint64_t r;
          if (op == 0x08) r = mul(f, x, y, env);
          else {
            const uint64_t acc = get(*dst, e, i);
            r = op & 1 ? fused_multiply_add(f, negate(f, x), y, acc, env) : fused_multiply_add(f, x, y, acc, env);
          }
          put(dst, e, i, nan_fix(f, r));
        }
        return finish();
      }
      if ((op & 0x3C) == 0x04) {      // FCMLA by element: bits 15:12 = 0001
        return false;
      }
      return false;
    }
    const uint32_t group = bits_of(insn, 15, 13);
    if (!((insn >> 15) & 1)) {      // FCMLA (vectors): Zda, pg/m, Zn, Zm, #rot
      if (bf_size) return false;
      const Format f = format_of(esz, false);
      const uint32_t rot = bits_of(insn, 14, 13);
      const Vec a = s_.v[zn], b = s_.v[zm], old = *dst;
      for (uint32_t p = 0; p < elements / 2; ++p) {
        const uint64_t re_a = get(a, esz, 2 * p), im_a = get(a, esz, 2 * p + 1), re_b = get(b, esz, 2 * p), im_b = get(b, esz, 2 * p + 1);
        uint64_t e1, e2, e3, e4;
        switch (rot) {
          case 0: e1 = re_a; e2 = re_b; e3 = re_a; e4 = im_b; break;
          case 1: e1 = negate(f, im_a); e2 = im_b; e3 = im_a; e4 = re_b; break;
          case 2: e1 = negate(f, re_a); e2 = re_b; e3 = negate(f, re_a); e4 = im_b; break;
          default: e1 = im_a; e2 = im_b; e3 = negate(f, im_a); e4 = re_b; break;
        }
        if (active(gp, 2 * p, esz)) put(dst, esz, 2 * p, nan_fix(f, fused_multiply_add(f, e2, e1, get(old, esz, 2 * p), env)));      // (only the active parts may raise flags)
        if (active(gp, 2 * p + 1, esz)) put(dst, esz, 2 * p + 1, nan_fix(f, fused_multiply_add(f, e4, e3, get(old, esz, 2 * p + 1), env)));
      }
      return finish();
    }
    if (group == 4) {
      const uint32_t sel = bits_of(insn, 20, 16);
      if (sel == 0 || sel == 1) {      // FCADD Zdn, pg/m, Zdn, Zm, #rot
        if (bf_size) return false;
        const Format f = format_of(esz, false);
        const bool rot270 = sel == 1;
        const Vec a = *dst, b = s_.v[zn];
        for (uint32_t p = 0; p < elements / 2; ++p) {
          const uint64_t re_a = get(a, esz, 2 * p), im_a = get(a, esz, 2 * p + 1), re_b = get(b, esz, 2 * p), im_b = get(b, esz, 2 * p + 1);
          if (active(gp, 2 * p, esz)) put(dst, esz, 2 * p, nan_fix(f, !rot270 ? add(f, re_a, negate(f, im_b), env) : add(f, re_a, im_b, env)));
          if (active(gp, 2 * p + 1, esz)) put(dst, esz, 2 * p + 1, nan_fix(f, !rot270 ? add(f, im_a, re_b, env) : add(f, im_a, negate(f, re_b), env)));
        }
        return finish();
      }
      if (sel == 16 || (sel >= 20 && sel <= 23)) {      // FADDP, FMAXNMP, FMINNMP, FMAXP, FMINP
        if (bf_size) return false;
        const Format f = format_of(esz, false);
        const Vec first = *dst, second = s_.v[zn];
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const Vec& src = (i & 1) ? second : first;
          const uint64_t x = get(src, esz, i & ~1u), y = get(src, esz, i | 1u);
          uint64_t r;
          switch (sel) {
            case 16: r = add(f, x, y, env); break;
            case 20: r = arm_minmax(f, x, y, true, true, env); break;
            case 21: r = arm_minmax(f, x, y, false, true, env); break;
            case 22: r = arm_minmax(f, x, y, true, false, env); break;
            default: r = arm_minmax(f, x, y, false, false, env); break;
          }
          put(dst, esz, i, nan_fix(f, r));
        }
        return finish();
      }
      return false;
    }
    if (group == 5) {      // FCVTNT, FCVTLT, BFCVTNT, FCVTXNT
      const uint32_t hi = bits_of(insn, 23, 16);
      const Vec src = s_.v[zn];
      uint32_t container;
      Format from, to;
      bool to_odd = false, take_odd = false;
      switch (hi) {
        case 0x88: container = 2; from = Format::F32; to = Format::F16; break;
        case 0x89: container = 2; from = Format::F16; to = Format::F32; take_odd = true; break;
        case 0x8A: container = 2; from = Format::F32; to = Format::BF16; break;
        case 0xCA: container = 3; from = Format::F64; to = Format::F32; break;
        case 0xCB: container = 3; from = Format::F32; to = Format::F64; take_odd = true; break;
        case 0x0A: container = 3; from = Format::F64; to = Format::F32; to_odd = true; break;
        default: return false;
      }
      const uint32_t from_bits = from == Format::F64 ? 64 : from == Format::F32 ? 32 : 16;
      const bool narrowing = !take_odd;
      for (uint32_t i = 0; i < element_count(container); ++i) {
        if (!active(gp, i, container)) continue;
        uint64_t x = get(src, container, i);
        if (take_odd) x >>= from_bits;
        x &= ones(from_bits);
        uint64_t r;
        if (to_odd) {
          Env odd = env;
          odd.mode = Rounding::TowardZero;
          odd.flags = 0;
          r = convert(from, to, x, odd);
          if ((odd.flags & kInexact) && !is_nan(to, r) && (r & 0x7FFFFFFFu) != 0x7F800000u) r |= 1;
          env.flags |= odd.flags;
        } else {
          r = convert(from, to, x, env);
        }
        r = nan_fix(to, r);
        if (narrowing) {
          const uint32_t to_bits = element_bits(container) / 2;
          put(dst, container, i, (get(*dst, container, i) & ones(to_bits)) | (r << to_bits));
        } else {
          put(dst, container, i, r);
        }
      }
      return finish();
    }
    return false;
  }
}

}  // namespace emulation
