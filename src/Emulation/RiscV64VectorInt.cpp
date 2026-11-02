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
#include "Emulation/RiscV64VectorUnit.hpp"
#include "Emulation/Wide.hpp"

namespace emulation {

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

__int128 multiply_signed(int64_t a, int64_t b) {
  const bool negative = (a < 0) != (b < 0);
  uint64_t hi, lo;
  multiply_unsigned(a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a), b < 0 ? 0 - static_cast<uint64_t>(b) : static_cast<uint64_t>(b), &hi, &lo);
  const __int128 magnitude = static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
  return negative ? -magnitude : magnitude;
}

// signed a times unsigned b
__int128 multiply_signed_unsigned(int64_t a, uint64_t b) {
  uint64_t hi, lo;
  multiply_unsigned(a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a), b, &hi, &lo);
  const __int128 magnitude = static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
  return a < 0 ? -magnitude : magnitude;
}

unsigned __int128 multiply_unsigned_wide(uint64_t a, uint64_t b) {
  uint64_t hi, lo;
  multiply_unsigned(a, b, &hi, &lo);
  return (static_cast<unsigned __int128>(hi) << 64) | lo;
}

// bit 0: .vv, bit 1: .vx, bit 2: .vi for each OPIVV/OPIVX/OPIVI funct6
constexpr uint8_t kAllowedInteger[64] = {
    7, 0, 3, 6, 3, 3, 3, 3, 0, 7, 7, 7, 7, 0, 7, 6,        // 0..15
    7, 7, 3, 3, 0, 0, 0, 7, 7, 7, 7, 7, 7, 7, 6, 6,        // 16..31
    7, 7, 3, 3, 0, 7, 0, 7, 7, 7, 7, 7, 7, 7, 7, 7,        // 32..47
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};       // 48..63

}  // namespace

// OPIVV, OPIVX, OPIVI
RiscV64Cpu::Step VectorUnit::integer() {
  const bool vv = funct3 == 0, vi = funct3 == 3;
  if (!(kAllowedInteger[funct6] & (vv ? 1 : funct3 == 4 ? 2 : 4))) return undefined();
  const uint64_t mask = sew_mask();
  const bool unsigned_immediate = funct6 == 12 || funct6 == 14 || funct6 == 15 || funct6 == 37 || (funct6 >= 40 && funct6 <= 47) || funct6 == 39;
  const uint64_t scalar = vv ? 0 : vi ? (unsigned_immediate ? vs1 : static_cast<uint64_t>(sext(vs1, 5))) & mask : s.x[vs1] & mask;
  const uint32_t regs = regs_of(lmul_log);
  const uint32_t shift_mask = sew - 1;

  switch (funct6) {
    case 39:
      if (vi) {      // vmv<nr>r.v
        const uint32_t count = vs1 + 1;
        if (!vm || (count != 1 && count != 2 && count != 4 && count != 8)) return undefined();
        if (!group_ok(vd, count) || !group_ok(vs2, count)) return undefined();
        if (s.vstart >= count * kVlenBytes) return ok();
        uint8_t copy[8 * kVlenBytes];
        for (uint32_t k = 0; k < count * kVlenBytes; ++k) copy[k] = s.v[vs2 * kVlenBytes + k];
        for (uint32_t k = 0; k < count * kVlenBytes; ++k) s.v[vd * kVlenBytes + k] = copy[k];
        return ok();
      }
      break;
    default: break;
  }

  switch (funct6) {
    case 0: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a + b; });
    case 2: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a - b; });
    case 3: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return b - a; });
    case 4: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a < b ? a : b; });
    case 6: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a < b ? b : a; });
    case 5: return binary_loop(*this, vv, scalar, [this](uint64_t a, uint64_t b, uint64_t) { return sext(a, sew) < sext(b, sew) ? a : b; });
    case 7: return binary_loop(*this, vv, scalar, [this](uint64_t a, uint64_t b, uint64_t) { return sext(a, sew) < sext(b, sew) ? b : a; });
    case 9: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a & b; });
    case 10: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a | b; });
    case 11: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a ^ b; });
    case 37: return binary_loop(*this, vv, scalar, [shift_mask](uint64_t a, uint64_t b, uint64_t) { return a << (b & shift_mask); });
    case 40: return binary_loop(*this, vv, scalar, [shift_mask](uint64_t a, uint64_t b, uint64_t) { return a >> (b & shift_mask); });
    case 41: return binary_loop(*this, vv, scalar, [this, shift_mask](uint64_t a, uint64_t b, uint64_t) { return static_cast<uint64_t>(sext(a, sew) >> (b & shift_mask)); });
    case 42: return binary_loop(*this, vv, scalar, [this, shift_mask](uint64_t a, uint64_t b, uint64_t) { return round_shift(static_cast<__int128>(a), static_cast<uint32_t>(b & shift_mask)); });
    case 43: return binary_loop(*this, vv, scalar, [this, shift_mask](uint64_t a, uint64_t b, uint64_t) { return round_shift(static_cast<__int128>(sext(a, sew)), static_cast<uint32_t>(b & shift_mask)); });
    case 32: case 33: case 34: case 35: case 39: {      // saturating add and subtract, vsmul
      bool saturated = false;
      const int64_t max = static_cast<int64_t>(mask >> 1), min = -max - 1;
      const uint32_t op = funct6;
      const Step r = binary_loop(*this, vv, scalar, [&](uint64_t a, uint64_t b, uint64_t) -> uint64_t {
        const int64_t sa = sext(a, sew), sb = sext(b, sew);
        __int128 t;
        switch (op) {
          case 32: {
            const unsigned __int128 sum = static_cast<unsigned __int128>(a) + b;
            if (sum > mask) {
              saturated = true;
              return mask;
            }
            return static_cast<uint64_t>(sum);
          }
          case 34:
            if (a < b) {
              saturated = true;
              return 0;
            }
            return a - b;
          case 33: t = static_cast<__int128>(sa) + sb; break;
          case 35: t = static_cast<__int128>(sa) - sb; break;
          default: t = static_cast<__int128>(round_shift(multiply_signed(sa, sb), sew - 1)); break;
        }
        if (op == 39) {      // (the rounded product fits 64 bits for every SEW; its sign is in bit 63 after the cast above)
          t = static_cast<__int128>(static_cast<int64_t>(static_cast<uint64_t>(t)));
        }
        if (t > max) {
          saturated = true;
          return static_cast<uint64_t>(max);
        }
        if (t < min) {
          saturated = true;
          return static_cast<uint64_t>(min);
        }
        return static_cast<uint64_t>(t);
      });
      if (saturated) s.vxsat = 1;
      return r;
    }
    case 12: case 14: case 15: return mask_and_permute();
    case 16: case 18: {      // vadc, vsbc (the carry or borrow comes from v0)
      if (vm || vd == 0) return undefined();
      if (!group_ok(vd, regs) || !group_ok(vs2, regs) || (vv && !group_ok(vs1, regs))) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        const uint64_t a = get(vs2, sew_bytes, i), b = vv ? get(vs1, sew_bytes, i) : scalar, carry = mask_bit(0, i);
        put(vd, sew_bytes, i, (funct6 == 16 ? a + b + carry : a - b - carry) & mask);
      }
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 17: case 19: {      // vmadc, vmsbc: the carry or borrow out, as a mask
      if (!group_ok(vs2, regs) || (vv && !group_ok(vs1, regs))) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        const uint64_t a = get(vs2, sew_bytes, i), b = vv ? get(vs1, sew_bytes, i) : scalar, carry = vm ? 0 : mask_bit(0, i);
        bool out;
        if (funct6 == 17) {
          const unsigned __int128 sum = static_cast<unsigned __int128>(a) + b + carry;
          out = (sum >> sew) != 0;
        } else {
          out = static_cast<unsigned __int128>(a) < static_cast<unsigned __int128>(b) + carry;
        }
        set_mask_bit(vd, i, out);
      }
      mask_tail(vd);
      return ok();
    }
    case 23: {      // vmerge, vmv.v.*
      if (!group_ok(vd, regs) || (vm && vs2 != 0) || (!vm && !group_ok(vs2, regs)) || (vv && !group_ok(vs1, regs))) return undefined();
      if (!vm && vd == 0) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        const uint64_t b = vv ? get(vs1, sew_bytes, i) : scalar;
        put(vd, sew_bytes, i, vm ? b : (mask_bit(0, i) ? b : get(vs2, sew_bytes, i)));
      }
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: {      // vmseq ... vmsgt: a mask result
      if (!group_ok(vs2, regs) || (vv && !group_ok(vs1, regs))) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        if (!active(i)) {
          if (ma) set_mask_bit(vd, i, true);
          continue;
        }
        const uint64_t a = get(vs2, sew_bytes, i), b = vv ? get(vs1, sew_bytes, i) : scalar;
        const int64_t sa = sext(a, sew), sb = sext(b, sew);
        bool r;
        switch (funct6) {
          case 24: r = a == b; break;
          case 25: r = a != b; break;
          case 26: r = a < b; break;
          case 27: r = sa < sb; break;
          case 28: r = a <= b; break;
          case 29: r = sa <= sb; break;
          case 30: r = a > b; break;
          default: r = sa > sb; break;
        }
        set_mask_bit(vd, i, r);
      }
      mask_tail(vd);
      return ok();
    }
    case 44: case 45: case 46: case 47: {      // narrowing shifts and clips: the source is twice as wide
      if (sew > 32 || lmul_log > 2) return undefined();
      const uint32_t wide_regs = regs_of(lmul_log + 1);
      if (!group_ok(vd, regs) || !group_ok(vs2, wide_regs) || (vv && !group_ok(vs1, regs))) return undefined();
      if (!vm && vd == 0) return undefined();
      if (s.vstart >= vl) return ok();
      const uint32_t wide_bits = sew * 2;
      const int64_t max = static_cast<int64_t>(mask >> 1), min = -max - 1;
      bool saturated = false;
      for (uint64_t i = s.vstart; i < vl; ++i) {
        if (!active(i)) {
          if (ma) ones(vd, sew_bytes, i, i + 1);
          continue;
        }
        const uint64_t wide = get(vs2, sew_bytes * 2, i), b = vv ? get(vs1, sew_bytes, i) : scalar;
        const uint32_t amount = static_cast<uint32_t>(b & (wide_bits - 1));
        uint64_t r;
        if (funct6 == 44) {
          r = wide >> amount;
        } else if (funct6 == 45) {
          r = static_cast<uint64_t>(sext(wide, wide_bits) >> amount);
        } else if (funct6 == 46) {
          r = round_shift(static_cast<__int128>(wide), amount);
          if (r > mask) {
            r = mask;
            saturated = true;
          }
        } else {
          const int64_t v = static_cast<int64_t>(round_shift(static_cast<__int128>(sext(wide, wide_bits)), amount));
          if (v > max) {
            r = static_cast<uint64_t>(max);
            saturated = true;
          } else if (v < min) {
            r = static_cast<uint64_t>(min);
            saturated = true;
          } else {
            r = static_cast<uint64_t>(v);
          }
        }
        put(vd, sew_bytes, i, r & mask);
      }
      if (saturated) s.vxsat = 1;
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 48: case 49: return reductions();
    default: return undefined();
  }
}

// OPMVV, OPMVX
RiscV64Cpu::Step VectorUnit::opm() {
  const bool vv = funct3 == 2;
  const uint64_t mask = sew_mask();
  const uint64_t scalar = vv ? 0 : s.x[vs1] & mask;
  const uint32_t regs = regs_of(lmul_log);
  const uint32_t bits = sew;
  switch (funct6) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      if (!vv) return undefined();
      return reductions();
    case 8: case 9: case 10: case 11: {      // vaaddu, vaadd, vasubu, vasub
      const uint32_t op = funct6;
      return binary_loop(*this, vv, scalar, [this, op, bits](uint64_t a, uint64_t b, uint64_t) {
        __int128 t;
        if (op == 8) t = static_cast<__int128>(a) + b;
        else if (op == 9) t = static_cast<__int128>(sext(a, bits)) + sext(b, bits);
        else if (op == 10) t = static_cast<__int128>(a) - b;
        else t = static_cast<__int128>(sext(a, bits)) - sext(b, bits);
        return round_shift(t, 1);
      });
    }
    case 14: case 15:
      if (vv) return undefined();
      return mask_and_permute();
    case 16: case 18: case 20: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: return mask_and_permute();
    case 32: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return b == 0 ? ~uint64_t{0} : a / b; });
    case 34: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return b == 0 ? a : a % b; });
    case 33: return binary_loop(*this, vv, scalar, [bits](uint64_t a, uint64_t b, uint64_t) {
      const int64_t x = sext(a, bits), y = sext(b, bits), min = -static_cast<int64_t>((~uint64_t{0} >> (65 - bits)) ) - 1;
      if (y == 0) return ~uint64_t{0};
      if (x == min && y == -1) return a;
      return static_cast<uint64_t>(x / y);
    });
    case 35: return binary_loop(*this, vv, scalar, [bits](uint64_t a, uint64_t b, uint64_t) {
      const int64_t x = sext(a, bits), y = sext(b, bits), min = -static_cast<int64_t>((~uint64_t{0} >> (65 - bits))) - 1;
      if (y == 0) return a;
      if (x == min && y == -1) return uint64_t{0};
      return static_cast<uint64_t>(x % y);
    });
    case 36: return binary_loop(*this, vv, scalar, [bits](uint64_t a, uint64_t b, uint64_t) { return static_cast<uint64_t>(multiply_unsigned_wide(a, b) >> bits); });
    case 37: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t) { return a * b; });
    case 38: return binary_loop(*this, vv, scalar, [bits](uint64_t a, uint64_t b, uint64_t) { return static_cast<uint64_t>(multiply_signed_unsigned(sext(a, bits), b) >> bits); });
    case 39: return binary_loop(*this, vv, scalar, [bits](uint64_t a, uint64_t b, uint64_t) { return static_cast<uint64_t>(multiply_signed(sext(a, bits), sext(b, bits)) >> bits); });
    case 41: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t d) { return b * d + a; });      // vmadd: vd = vs1 * vd + vs2
    case 43: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t d) { return a - b * d; });      // vnmsub
    case 45: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t d) { return b * a + d; });      // vmacc: vd = vs1 * vs2 + vd
    case 47: return binary_loop(*this, vv, scalar, [](uint64_t a, uint64_t b, uint64_t d) { return d - b * a; });      // vnmsac
    default: break;
  }
  if (funct6 >= 48) return integer_wide();
  (void)regs;
  return undefined();
}

// vwadd & co., vwmul & co., vwmacc & co.: the destination is twice as wide
RiscV64Cpu::Step VectorUnit::integer_wide() {
  const bool vv = funct3 == 2;
  if (sew > 32 || lmul_log > 2) return undefined();
  const uint32_t wide_bytes = sew_bytes * 2, wide_bits = sew * 2;
  const uint32_t regs = regs_of(lmul_log), wide_regs = regs_of(lmul_log + 1);
  const uint64_t wide_mask = wide_bits == 64 ? ~uint64_t{0} : (uint64_t{1} << wide_bits) - 1;
  bool wide_source = false, a_signed = false, b_signed = false;
  enum { kAdd, kSub, kMul, kMacc } kind;
  switch (funct6) {
    case 48: kind = kAdd; break;
    case 49: kind = kAdd; a_signed = b_signed = true; break;
    case 50: kind = kSub; break;
    case 51: kind = kSub; a_signed = b_signed = true; break;
    case 52: kind = kAdd; wide_source = true; break;
    case 53: kind = kAdd; wide_source = true; a_signed = b_signed = true; break;
    case 54: kind = kSub; wide_source = true; break;
    case 55: kind = kSub; wide_source = true; a_signed = b_signed = true; break;
    case 56: kind = kMul; break;
    case 58: kind = kMul; a_signed = true; break;
    case 59: kind = kMul; a_signed = b_signed = true; break;
    case 60: kind = kMacc; break;
    case 61: kind = kMacc; a_signed = b_signed = true; break;
    case 62: kind = kMacc; a_signed = true; if (vv) return undefined(); break;      // vwmaccus.vx
    case 63: kind = kMacc; b_signed = true; break;      // vwmaccsu
    default: return undefined();
  }
  if (!group_ok(vd, wide_regs) || !group_ok(vs2, wide_source ? wide_regs : regs) || (vv && !group_ok(vs1, regs))) return undefined();
  if (!vm && vd == 0) return undefined();
  if (s.vstart >= vl) return ok();
  const uint64_t scalar = vv ? 0 : s.x[vs1];
  for (uint64_t i = s.vstart; i < vl; ++i) {
    if (!active(i)) {
      if (ma) ones(vd, wide_bytes, i, i + 1);
      continue;
    }
    uint64_t a = wide_source ? get(vs2, wide_bytes, i) : get(vs2, sew_bytes, i);
    uint64_t b = vv ? get(vs1, sew_bytes, i) : scalar & sew_mask();
    if (!wide_source) a = a_signed ? static_cast<uint64_t>(sext(a, sew)) : a;
    else if (a_signed) a = static_cast<uint64_t>(sext(a, wide_bits));
    b = b_signed ? static_cast<uint64_t>(sext(b, sew)) : b;
    uint64_t r;
    switch (kind) {
      case kAdd: r = a + b; break;
      case kSub: r = a - b; break;
      case kMul: r = a * b; break;
      default: r = a * b + get(vd, wide_bytes, i); break;
    }
    put(vd, wide_bytes, i, r & wide_mask);
  }
  tail(vd, wide_regs, wide_bytes, vl);
  return ok();
}

// --- reductions --------------------------------------------------------------------------------------------------------------------------------

RiscV64Cpu::Step VectorUnit::reductions() {
  const bool widening = funct3 == 0;
  if (widening && sew > 32) return undefined();
  if (s.vstart != 0) return undefined();
  const uint32_t regs = regs_of(lmul_log);
  if (!group_ok(vs2, regs)) return undefined();
  if (vl == 0) return ok();
  const uint32_t acc_bytes = widening ? sew_bytes * 2 : sew_bytes, acc_bits = acc_bytes * 8;
  const uint64_t acc_mask = acc_bits == 64 ? ~uint64_t{0} : (uint64_t{1} << acc_bits) - 1;
  uint64_t acc = get(vs1, acc_bytes, 0);
  const uint32_t op = widening ? (funct6 == 48 ? 20 : 21) : funct6;      // 20, 21: the widening sums (unsigned, signed)
  for (uint64_t i = 0; i < vl; ++i) {
    if (!active(i)) continue;
    uint64_t x = get(vs2, sew_bytes, i);
    switch (op) {
      case 0: acc += x; break;
      case 1: acc &= x; break;
      case 2: acc |= x; break;
      case 3: acc ^= x; break;
      case 4: if (x < acc) acc = x; break;
      case 5: if (sext(x, sew) < sext(acc, sew)) acc = x; break;
      case 6: if (x > acc) acc = x; break;
      case 7: if (sext(x, sew) > sext(acc, sew)) acc = x; break;
      case 20: acc += x; break;
      default: acc += static_cast<uint64_t>(sext(x, sew)); break;
    }
    acc &= acc_mask;
  }
  put(vd, acc_bytes, 0, acc);
  tail(vd, 1, acc_bytes, 1);
  return ok();
}

// --- masks, slides, gathers, compress ----------------------------------------------------------------------------------------------------------

RiscV64Cpu::Step VectorUnit::mask_and_permute() {
  const uint64_t mask = sew_mask();
  const uint32_t regs = regs_of(lmul_log);
  const bool opm = funct3 == 2 || funct3 == 6;
  if (!opm) {
    const bool vv = funct3 == 0, vi = funct3 == 3;
    const uint64_t amount = vv ? 0 : vi ? vs1 : s.x[vs1];
    if (funct6 == 15 || (funct6 == 14 && !vv)) {      // vslidedown, vslideup
      if (!group_ok(vd, regs) || !group_ok(vs2, regs) || (!vm && vd == 0)) return undefined();
      if (funct6 == 14 && vd == vs2) return undefined();      // the groups may not overlap
      if (s.vstart >= vl) return ok();
      uint64_t start = s.vstart;
      if (funct6 == 14 && amount > start) start = amount;
      for (uint64_t i = start; i < vl; ++i) {
        if (!active(i)) {
          if (ma) ones(vd, sew_bytes, i, i + 1);
          continue;
        }
        if (funct6 == 14) {
          put(vd, sew_bytes, i, get(vs2, sew_bytes, i - amount));
        } else {
          const bool in_range = amount < vlmax && i + amount < vlmax;
          put(vd, sew_bytes, i, in_range ? get(vs2, sew_bytes, i + amount) : 0);
        }
      }
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    // vrgather, vrgatherei16
    const bool ei16 = funct6 == 14;
    const uint32_t index_bytes = ei16 ? 2 : sew_bytes;
    int32_t index_emul = lmul_log;
    if (ei16 && !emul_for(2, &index_emul)) return undefined();
    if (!group_ok(vd, regs) || !group_ok(vs2, regs) || (vv && !group_ok(vs1, regs_of(index_emul))) || (!vm && vd == 0)) return undefined();
    if (vd == vs2 || (vv && vd == vs1)) return undefined();
    if (s.vstart >= vl) return ok();
    for (uint64_t i = s.vstart; i < vl; ++i) {
      if (!active(i)) {
        if (ma) ones(vd, sew_bytes, i, i + 1);
        continue;
      }
      const uint64_t index = vv ? get(vs1, index_bytes, i) : amount;
      put(vd, sew_bytes, i, index < vlmax ? get(vs2, sew_bytes, index) : 0);
    }
    tail(vd, regs, sew_bytes, vl);
    return ok();
  }

  const bool vv = funct3 == 2;
  switch (funct6) {
    case 14: case 15: {      // vslide1up, vslide1down
      if (!group_ok(vd, regs) || !group_ok(vs2, regs) || (!vm && vd == 0)) return undefined();
      if (funct6 == 14 && vd == vs2) return undefined();
      if (s.vstart >= vl) return ok();
      const uint64_t x = s.x[vs1] & mask;
      for (uint64_t i = s.vstart; i < vl; ++i) {
        if (!active(i)) {
          if (ma) ones(vd, sew_bytes, i, i + 1);
          continue;
        }
        uint64_t v;
        if (funct6 == 14) v = i == 0 ? x : get(vs2, sew_bytes, i - 1);
        else v = i + 1 == vl ? x : get(vs2, sew_bytes, i + 1);
        put(vd, sew_bytes, i, v);
      }
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 16: {
      if (vv) {
        if (vs1 == 0) {      // vmv.x.s
          c.set_x(vd, static_cast<uint64_t>(sext(get(vs2, sew_bytes, 0), sew)));
          return ok();
        }
        if (vs1 == 0x10 || vs1 == 0x11) {      // vcpop.m, vfirst.m
          if (s.vstart != 0) return undefined();
          uint64_t count = 0, first = ~uint64_t{0};
          for (uint64_t i = 0; i < vl; ++i) {
            if (!active(i) || !mask_bit(vs2, i)) continue;
            ++count;
            if (first == ~uint64_t{0}) first = i;
          }
          c.set_x(vd, vs1 == 0x10 ? count : first);
          return ok();
        }
        return undefined();
      }
      if (vs2 != 0) return undefined();      // vmv.s.x
      if (s.vstart >= vl) return ok();
      put(vd, sew_bytes, 0, s.x[vs1] & mask);
      tail(vd, 1, sew_bytes, 1);
      return ok();
    }
    case 18: {      // vzext.vf*, vsext.vf*
      if (!vv || vs1 < 2 || vs1 > 7) return undefined();
      const uint32_t factor = vs1 >= 6 ? 2 : vs1 >= 4 ? 4 : 8;
      const bool is_signed = vs1 & 1;
      if (sew_bytes < factor) return undefined();
      const uint32_t source_bytes = sew_bytes / factor;
      int32_t source_emul = lmul_log - static_cast<int32_t>(factor == 2 ? 1 : factor == 4 ? 2 : 3);
      if (source_emul < -3) return undefined();
      if (!group_ok(vd, regs) || !group_ok(vs2, regs_of(source_emul)) || (!vm && vd == 0)) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        if (!active(i)) {
          if (ma) ones(vd, sew_bytes, i, i + 1);
          continue;
        }
        const uint64_t v = get(vs2, source_bytes, i);
        put(vd, sew_bytes, i, (is_signed ? static_cast<uint64_t>(sext(v, source_bytes * 8)) : v) & mask);
      }
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 20: {      // vmsbf.m, vmsof.m, vmsif.m, viota.m, vid.v
      if (!vv) return undefined();
      if (vs1 == 0x11) {      // vid.v
        if (vs2 != 0 || !group_ok(vd, regs) || (!vm && vd == 0)) return undefined();
        if (s.vstart >= vl) return ok();
        for (uint64_t i = s.vstart; i < vl; ++i) {
          if (!active(i)) {
            if (ma) ones(vd, sew_bytes, i, i + 1);
            continue;
          }
          put(vd, sew_bytes, i, i);
        }
        tail(vd, regs, sew_bytes, vl);
        return ok();
      }
      if (s.vstart != 0) return undefined();
      if (vs1 == 0x10) {      // viota.m
        if (!group_ok(vd, regs) || (!vm && vd == 0) || vd == vs2) return undefined();
        if (vl == 0) return ok();
        uint64_t count = 0;
        for (uint64_t i = 0; i < vl; ++i) {
          if (!active(i)) {
            if (ma) ones(vd, sew_bytes, i, i + 1);
            continue;
          }
          put(vd, sew_bytes, i, count);
          if (mask_bit(vs2, i)) ++count;
        }
        tail(vd, regs, sew_bytes, vl);
        return ok();
      }
      if (vs1 != 1 && vs1 != 2 && vs1 != 3) return undefined();
      if (vd == vs2 || (!vm && vd == 0)) return undefined();
      if (vl == 0) return ok();
      bool seen = false;
      for (uint64_t i = 0; i < vl; ++i) {
        if (!active(i)) {
          if (ma) set_mask_bit(vd, i, true);
          continue;
        }
        const bool b = mask_bit(vs2, i);
        bool r;
        if (vs1 == 1) r = !seen && !b;      // set before first
        else if (vs1 == 2) r = !seen && b;      // set only first
        else r = !seen;      // set including first
        if (b) seen = true;
        set_mask_bit(vd, i, r);
      }
      mask_tail(vd);
      return ok();
    }
    case 23: {      // vcompress.vm
      if (!vv || !vm || s.vstart != 0) return undefined();
      if (!group_ok(vd, regs) || !group_ok(vs2, regs) || vd == vs2 || vd == vs1) return undefined();
      if (vl == 0) return ok();
      uint64_t count = 0;
      for (uint64_t i = 0; i < vl; ++i) {
        if (!mask_bit(vs1, i)) continue;
        put(vd, sew_bytes, count++, get(vs2, sew_bytes, i));
      }
      tail(vd, regs, sew_bytes, count);
      return ok();
    }
    default: break;
  }
  if (funct6 >= 24 && funct6 <= 31) {      // mask-register logical instructions
    if (!vv) return undefined();
    if (s.vstart >= vl) return ok();
    for (uint64_t i = s.vstart; i < vl; ++i) {
      const bool a = mask_bit(vs2, i), b = mask_bit(vs1, i);
      bool r;
      switch (funct6) {
        case 24: r = a && !b; break;
        case 25: r = a && b; break;
        case 26: r = a || b; break;
        case 27: r = a != b; break;
        case 28: r = a || !b; break;
        case 29: r = !(a && b); break;
        case 30: r = !(a || b); break;
        default: r = a == b; break;
      }
      set_mask_bit(vd, i, r);
    }
    mask_tail(vd);
    return ok();
  }
  return undefined();
}

}  // namespace emulation

// --- the instruction entry points --------------------------------------------------------------------------------------------------------------

namespace emulation {

RiscV64Cpu::Step RiscV64Cpu::vector(GuestMemory& m, uint32_t insn) {
  VectorUnit u(*this, m, insn);
  if (u.funct3 == 7) return u.configure();
  if (!u.prepare()) return undefined();
  if (u.funct3 != 1 && u.funct3 != 5) {
    Step bits_result;
    if (u.bit_instruction(&bits_result)) return bits_result;
  }
  switch (u.funct3) {
    case 0: case 3: case 4: return u.integer();
    case 2: case 6: return u.opm();
    default: return u.floating();
  }
}

RiscV64Cpu::Step RiscV64Cpu::vector_memory(GuestMemory& m, uint32_t insn) {
  VectorUnit u(*this, m, insn);
  return u.memory_access();
}

}  // namespace emulation
