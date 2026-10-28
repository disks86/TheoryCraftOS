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
#include "Emulation/SoftFloat.hpp"
#include "Emulation/Wide.hpp"
#include "Emulation/X86_64EvexInternal.hpp"

namespace emulation {

namespace {

uint16_t float_to_bfloat(uint32_t x) {
  if ((x & 0x7F800000u) == 0x7F800000u && (x & 0x7FFFFF)) return static_cast<uint16_t>((x >> 16) | 0x40);      // a NaN stays a quiet NaN
  if ((x & 0x7F800000u) == 0) return static_cast<uint16_t>((x >> 16) & 0x8000);      // denormals are flushed
  return static_cast<uint16_t>((x + 0x7FFF + ((x >> 16) & 1)) >> 16);
}

uint32_t bfloat_to_float(uint16_t b) {
  uint32_t x = static_cast<uint32_t>(b) << 16;
  if ((x & 0x7F800000u) == 0) x &= 0x80000000u;      // (a denormal operand reads as zero)
  return x;
}

}  // namespace

// AVX-512 VNNI, IFMA, VBMI, VBMI2, BITALG, GFNI, BF16, VAES and VPCLMULQDQ.
X86_64Cpu::Step X86_64Cpu::evex_ext(GuestMemory& m, const EvexOp& defn) {
  const EvexOp* def = &defn;
  const uint32_t reg = ev_.reg, vvvv = ev_.vvvv, vl = ev_.vl, aaa = ev_.aaa, rm_reg = ev_.rm;
  const uint32_t imm = ev_.imm;
  const bool w = ev_.w;
  const uint32_t elem = def->elem ? def->elem : (w ? 64 : 32);
  const Vec old_dest = ev_.old_dest;
  auto finish = [&]() {
    s_.rip += d_.position;
    return Step::Ok;
  };
  Vec a = get_vec(vvvv), b, result;
  const bool broadcastable = def->tuple == kFull;

  switch (def->kind) {
    case kVnni: {
      if (!ev_read_rm(m, &b, 32, true, true)) return Step::Fault;
      for (uint32_t i = 0; i < vl / 32; ++i) {
        int64_t sum = 0;
        if (def->sub < 2) {
          for (uint32_t k = 0; k < 4; ++k) sum += static_cast<int64_t>(element(a, 8, 4 * i + k)) * signed_of(element(b, 8, 4 * i + k), 8);
        } else {
          for (uint32_t k = 0; k < 2; ++k) sum += signed_of(element(a, 16, 2 * i + k), 16) * signed_of(element(b, 16, 2 * i + k), 16);
        }
        const int64_t acc = signed_of(element(old_dest, 32, i), 32);
        int64_t r = acc + sum;
        if (def->sub & 1) r = r > 0x7FFFFFFF ? 0x7FFFFFFF : r < -0x80000000ll ? -0x80000000ll : r;
        set_element(result, 32, i, static_cast<uint64_t>(r) & 0xFFFFFFFFu);
      }
      ev_write_masked(reg, result, 32);
      return finish();
    }
    case kMadd52: {
      if (!ev_read_rm(m, &b, 64, true, true)) return Step::Fault;
      const uint64_t mask52 = (uint64_t{1} << 52) - 1;
      for (uint32_t i = 0; i < vl / 64; ++i) {
        uint64_t hi, lo;
        multiply_unsigned(a.q[i] & mask52, b.q[i] & mask52, &hi, &lo);
        const uint64_t part = def->sub == 0 ? (lo & mask52) : (((hi << 12) | (lo >> 52)) & mask52);
        result.q[i] = old_dest.q[i] + part;
      }
      ev_write_masked(reg, result, 64);
      return finish();
    }
    case kMultishift: {
      if (!ev_read_rm(m, &b, 64, true, false)) return Step::Fault;
      for (uint32_t i = 0; i < vl / 64; ++i) {
        for (uint32_t j = 0; j < 8; ++j) {
          const uint32_t c = static_cast<uint32_t>(element(a, 8, 8 * i + j)) & 63;
          const uint64_t rot = c == 0 ? b.q[i] : ((b.q[i] >> c) | (b.q[i] << (64 - c)));
          set_element(result, 8, 8 * i + j, rot & 0xFF);
        }
      }
      ev_write_masked(reg, result, 8);
      return finish();
    }
    case kShufBit: {
      if (!ev_read_rm(m, &b, 8, false, false)) return Step::Fault;
      uint64_t bits = 0;
      for (uint32_t q = 0; q < vl / 64; ++q)
        for (uint32_t j = 0; j < 8; ++j) {
          const uint32_t idx = static_cast<uint32_t>(element(b, 8, 8 * q + j)) & 63;
          if ((a.q[q] >> idx) & 1) bits |= uint64_t{1} << (8 * q + j);
        }
      ev_write_k(bits, vl / 8);
      return finish();
    }
    case kShift2: case kShift2Var: {
      if (!ev_read_rm(m, &b, elem, broadcastable, true)) return Step::Fault;
      const uint32_t n = vl / elem;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = element(a, elem, i), y = element(b, elem, i), d = element(old_dest, elem, i);
        const uint32_t s = static_cast<uint32_t>((def->kind == kShift2 ? imm : (def->sub == 0 ? y : y)) & (elem - 1));
        uint64_t hi, lo;
        if (def->kind == kShift2) {
          // vpshld: (vvvv : r/m) << s, the upper half; vpshrd: (r/m : vvvv) >> s, the lower half
          if (def->sub == 0) { hi = x; lo = y; } else { hi = y; lo = x; }
        } else if (def->sub == 0) {      // vpshldv: (dest : vvvv) << count in r/m
          hi = d; lo = x;
        } else {      // vpshrdv: (vvvv : dest) >> count in r/m
          hi = x; lo = d;
        }
        uint64_t r;
        if (s == 0) r = def->sub == 0 ? hi : lo;
        else if (def->sub == 0) r = ((hi << s) | (lo >> (elem - s))) & width_mask(elem);
        else r = ((lo >> s) | (hi << (elem - s))) & width_mask(elem);
        set_element(result, elem, i, r);
      }
      ev_write_masked(reg, result, elem);
      return finish();
    }
    case kGfAffine: case kGfMul: {
      const bool affine = def->kind == kGfAffine;
      if (!ev_read_rm(m, &b, affine ? 64 : 8, affine, !affine)) return Step::Fault;
      for (uint32_t i = 0; i < vl / 8; ++i) {
        const uint8_t x = static_cast<uint8_t>(element(a, 8, i));
        if (!affine) {
          set_element(result, 8, i, crypto_gf2p8_multiply(x, static_cast<uint8_t>(element(b, 8, i))));
          continue;
        }
        const uint64_t matrix = b.q[i / 8];
        const uint8_t input = def->sub ? crypto_gf2p8_inverse(x) : x;
        uint8_t r = 0;
        for (uint32_t k = 0; k < 8; ++k) {
          const uint8_t row = static_cast<uint8_t>(matrix >> (8 * (7 - k)));
          uint8_t t = row & input;
          t ^= t >> 4; t ^= t >> 2; t ^= t >> 1;
          r |= static_cast<uint8_t>(((t & 1) ^ ((imm >> k) & 1)) << k);
        }
        set_element(result, 8, i, r);
      }
      ev_write_masked(reg, result, 8);
      return finish();
    }
    case kBf16: {
      if (!ev_read_rm(m, &b, 32, true, false)) return Step::Fault;
      if (def->sub == 2) {      // vdpbf16ps
        softfloat::Env env;
        env.mode = softfloat::Rounding::NearestEven;
        for (uint32_t i = 0; i < vl / 32; ++i) {
          uint64_t acc = element(old_dest, 32, i);
          for (uint32_t kk = 0; kk < 2; ++kk) {
            const uint32_t k = 1 - kk;      // (the odd bf16 pair member is accumulated first)
            const uint64_t x = bfloat_to_float(static_cast<uint16_t>(element(a, 16, 2 * i + k))), y = bfloat_to_float(static_cast<uint16_t>(element(b, 16, 2 * i + k)));
            const uint64_t prod = softfloat::mul(softfloat::Format::F32, x, y, env);
            if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;      // (denormals are flushed on input and output)
            acc = softfloat::add(softfloat::Format::F32, acc, prod, env);
            if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;
          }
          set_element(result, 32, i, acc);
        }
        ev_write_masked(reg, result, 32);
        return finish();
      }
      if (def->sub == 1) {      // vcvtneps2bf16: the result is half as wide
        Vec src = b;
        const uint32_t n = vl / 32;
        for (uint32_t i = 0; i < n; ++i) set_element(result, 16, i, float_to_bfloat(static_cast<uint32_t>(element(src, 32, i))));
        Vec out = result;
        if (aaa != 0) {
          for (uint32_t i = 0; i < n; ++i)
            if (!ev_active(i)) set_element(out, 16, i, ev_.z ? 0 : element(old_dest, 16, i));
        }
        set_vec(reg, out, n * 16 < 128 ? 128 : n * 16);
        return finish();
      }
      const uint32_t n = vl / 32;      // vcvtne2ps2bf16: the low half from the r/m operand, the high half from vvvv
      for (uint32_t i = 0; i < n; ++i) {
        set_element(result, 16, i, float_to_bfloat(static_cast<uint32_t>(element(b, 32, i))));
        set_element(result, 16, n + i, float_to_bfloat(static_cast<uint32_t>(element(a, 32, i))));
      }
      ev_write_masked(reg, result, 16);
      return finish();
    }
    case kAesNi: case kClmul: {
      if (!ev_read_rm(m, &b, 64, false, false)) return Step::Fault;
      for (uint32_t l = 0; l < vl / 128; ++l) {
        Xmm x, y;
        x.lo = a.q[2 * l]; x.hi = a.q[2 * l + 1];
        y.lo = b.q[2 * l]; y.hi = b.q[2 * l + 1];
        Xmm r;
        if (def->kind == kClmul) {
          r = crypto_carryless_multiply((imm & 1) ? x.hi : x.lo, (imm & 0x10) ? y.hi : y.lo);
        } else {
          r = crypto_aes_round(x, y, def->sub >= 2, (def->sub & 1) != 0);
        }
        result.q[2 * l] = r.lo;
        result.q[2 * l + 1] = r.hi;
      }
      set_vec(reg, result, vl);
      return finish();
    }
    case kDbpsadbw: {      // double block packed sum of absolute differences
      if (!ev_read_rm(m, &b, 16, false, false)) return Step::Fault;
      for (uint32_t l = 0; l < vl / 128; ++l) {
        uint8_t x[16], y[16], t[16];
        lane_bytes(a, l, x);
        lane_bytes(b, l, y);
        for (uint32_t d = 0; d < 4; ++d) {
          const uint32_t sel = (imm >> (2 * d)) & 3;
          for (uint32_t k = 0; k < 4; ++k) t[4 * d + k] = y[4 * sel + k];
        }
        for (uint32_t h = 0; h < 2; ++h)
          for (uint32_t j = 0; j < 4; ++j) {
            uint32_t sum = 0;
            for (uint32_t k = 0; k < 4; ++k) {
              const int d = static_cast<int>(x[8 * h + 4 * (j >> 1) + k]) - static_cast<int>(t[8 * h + j + k]);
              sum += static_cast<uint32_t>(d < 0 ? -d : d);
            }
            set_element(result, 16, 8 * l + 4 * h + j, sum);
          }
      }
      ev_write_masked(reg, result, 16);
      return finish();
    }
    case kBroadcastMask: {
      if (ev_.memory) return fail(6);
      const uint64_t k = s_.k[d_.rm & 7];
      if (def->sub == 0) for (uint32_t i = 0; i < vl / 64; ++i) result.q[i] = k & 0xFF;
      else for (uint32_t i = 0; i < vl / 32; ++i) set_element(result, 32, i, k & 0xFFFF);
      set_vec(reg, result, vl);
      return finish();
    }
    default: (void)rm_reg; return fail(6);
  }
}

}  // namespace emulation
