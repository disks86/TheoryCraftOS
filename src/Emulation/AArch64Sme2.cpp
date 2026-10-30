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

// SME2 at a streaming vector length of 128 bits: the ZA array vector-group instructions (ADD / SUB / FADD / FSUB, FMLA / FMLS, the dot products, the vertical dot products and the widening multiply-accumulates
// with two or four ZA vectors per source vector). The encodings are matched through a table generated from LLVM's (AArch64Sme2Forms.inc): the fixed bits, the group size, the operand shape and where the
// offset and the element index sit, which differ from form to form.
namespace emulation {

using namespace sve;
using namespace softfloat;

#include "Emulation/AArch64Sme2Forms.inc"

namespace {

uint32_t gather(uint32_t insn, const uint8_t* bits, uint32_t count) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < count; ++i) v |= ((insn >> bits[i]) & 1u) << i;
  return v;
}

// The ZA arithmetic runs with the default NaN: any NaN result is the quiet NaN with an empty payload.
uint64_t default_nan(Format f, uint64_t r) { return is_nan(f, r) ? (f == Format::F32 ? 0x7FC00000ull : f == Format::F64 ? 0x7FF8000000000000ull : f == Format::BF16 ? 0x7FC0ull : 0x7E00ull) : r; }

}  // namespace

AArch64Cpu::Step AArch64Cpu::sme2_array(uint32_t form, uint32_t insn) {
  const Sme2Form& f = kSme2Forms[form];
  const uint32_t n = f.group, rv = static_cast<uint32_t>(reg(8 + bits_of(insn, 14, 13), false));
  const uint32_t off = gather(insn, f.off_bits, f.off_count), idx = gather(insn, f.idx_bits, f.idx_count);
  const uint32_t zn = n == 1 ? bits_of(insn, 9, 5) : n == 2 ? (bits_of(insn, 9, 6) << 1) : (bits_of(insn, 9, 7) << 2);
  uint32_t zm = bits_of(insn, 19, 16);
  if (f.shape == 2) zm = n == 2 ? (bits_of(insn, 20, 17) << 1) : (bits_of(insn, 20, 18) << 2);
  const uint32_t scale = f.off_scale, stride = 16 / n;
  const uint32_t src = f.src, dst = f.dst;
  const uint32_t lanes = 16u >> dst;
  Env env = fp_env();
  const Sme2Op op = f.op;
  auto row_of = [&](uint32_t r, uint32_t j) { return (((rv - rv % scale) + off * scale + j) % stride) + r * stride; };      // (the selector is aligned down to the number of vectors per group)
  auto zn_vec = [&](uint32_t r) -> const Vec& { return s_.v[(zn + r) & 31]; };
  auto zm_vec = [&](uint32_t r) -> const Vec& { return s_.v[f.shape == 2 ? ((zm + r) & 31) : zm]; };
  auto ext = [&](uint64_t v, uint32_t esz, bool is_signed) -> int64_t { return is_signed ? sext(v, 8u << esz) : static_cast<int64_t>(v); };
  const bool bf_op = op == k_bfadd || op == k_bfsub || op == k_bfmla || op == k_bfmls;
  const Format fmt = dst == 2 ? Format::F32 : dst == 3 ? Format::F64 : bf_op ? Format::BF16 : Format::F16;

  switch (op) {
    case k_add: case k_sub: case k_fadd: case k_fsub: case k_fmla: case k_fmls: case k_bfadd: case k_bfsub: case k_bfmla: case k_bfmls: {
      const bool fp = op != k_add && op != k_sub;
      const bool sub = op == k_sub || op == k_fsub || op == k_fmls || op == k_bfsub || op == k_bfmls;
      const bool fma = op == k_fmla || op == k_fmls || op == k_bfmla || op == k_bfmls;
      for (uint32_t r = 0; r < n; ++r) {
        Vec& row = s_.za[row_of(r, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          const uint64_t a = get(zn_vec(r), dst, l), acc = get(row, dst, l);
          uint64_t b = 0;
          if (f.shape == 1 || f.shape == 2) b = get(zm_vec(r), dst, l);
          else if (f.shape == 3) b = get(s_.v[zm], dst, idx);
          uint64_t result;
          if (fma) result = default_nan(fmt, fused_multiply_add(fmt, sub ? negate(fmt, a) : a, b, acc, env));
          else if (fp) result = default_nan(fmt, add(fmt, acc, sub ? negate(fmt, a) : a, env));
          else if (f.shape == 0) result = sub ? acc - a : acc + a;
          else result = sub ? a - b : a + b;      // (the vector forms write the sum or difference of the two operands, they do not accumulate)
          put(&row, dst, l, result & element_ones(dst));
        }
      }
      return Step::Ok;      // (the exception flags of the ZA arithmetic are not reported through FPSR)
    }
    case k_sdot: case k_udot: case k_usdot: case k_sudot: case k_fdot: case k_bfdot: {
      const uint32_t c = (src == 0 || dst == 3) ? 4 : 2;
      const bool n_signed = op == k_sdot || op == k_sudot, m_signed = op == k_sdot || op == k_usdot;
      for (uint32_t r = 0; r < n; ++r) {
        Vec& row = s_.za[row_of(r, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          auto m_at = [&](uint32_t i) { return get(f.shape == 3 ? s_.v[zm] : zm_vec(r), src, (f.shape == 3 ? c * idx : c * l) + i); };
          if (op == k_fdot || op == k_bfdot) {
            uint64_t result;
            if (op == k_bfdot) {
              result = bfloat_dot_add(get(row, 2, l), get(zn_vec(r), 1, 2 * l), get(zn_vec(r), 1, 2 * l + 1), m_at(0), m_at(1), env);
            } else {
              auto w = [&](uint64_t v) { return convert(Format::F16, Format::F32, v, env); };
              const uint64_t t = add(Format::F32, mul(Format::F32, w(get(zn_vec(r), 1, 2 * l)), w(m_at(0)), env), mul(Format::F32, w(get(zn_vec(r), 1, 2 * l + 1)), w(m_at(1)), env), env);
              result = default_nan(Format::F32, add(Format::F32, get(row, 2, l), t, env));
            }
            put(&row, 2, l, result);
            continue;
          }
          uint64_t sum = get(row, dst, l);
          for (uint32_t i = 0; i < c; ++i) sum += static_cast<uint64_t>(ext(get(zn_vec(r), src, c * l + i), src, n_signed) * ext(m_at(i), src, m_signed));
          put(&row, dst, l, sum & element_ones(dst));
        }
      }
      return Step::Ok;
    }
    case k_svdot: case k_uvdot: case k_suvdot: case k_usvdot: case k_fvdot: case k_bfvdot: {
      const uint32_t c = n;      // (the number of source vectors, and of ZA vectors written)
      const bool n_signed = op == k_svdot || op == k_suvdot, m_signed = op == k_svdot || op == k_usvdot;
      Vec result[4];
      for (uint32_t i = 0; i < c; ++i) {
        result[i] = s_.za[row_of(i, 0)];
        for (uint32_t l = 0; l < lanes; ++l) {
          const uint32_t e = (dst == 3) ? 4 * l + i : c * l + i;
          if (op == k_fvdot || op == k_bfvdot) {
            const uint64_t n0 = get(zn_vec(0), 1, e), n1 = get(zn_vec(1), 1, e), m0 = get(s_.v[zm], 1, 2 * idx), m1 = get(s_.v[zm], 1, 2 * idx + 1);
            uint64_t r;
            if (op == k_bfvdot) {
              r = bfloat_dot_add(get(result[i], 2, l), n0, n1, m0, m1, env);
            } else {
              auto w = [&](uint64_t v) { return convert(Format::F16, Format::F32, v, env); };
              const uint64_t t = add(Format::F32, mul(Format::F32, w(n0), w(m0), env), mul(Format::F32, w(n1), w(m1), env), env);
              r = default_nan(Format::F32, add(Format::F32, get(result[i], 2, l), t, env));
            }
            put(&result[i], 2, l, r);
            continue;
          }
          uint64_t sum = get(result[i], dst, l);
          for (uint32_t j = 0; j < c; ++j) sum += static_cast<uint64_t>(ext(get(zn_vec(j), src, e), src, n_signed) * ext(get(s_.v[zm], src, c * idx + j), src, m_signed));
          put(&result[i], dst, l, sum & element_ones(dst));
        }
      }
      for (uint32_t i = 0; i < c; ++i) s_.za[row_of(i, 0)] = result[i];
      return Step::Ok;
    }
    case k_smlal: case k_umlal: case k_smlsl: case k_umlsl: case k_fmlal: case k_fmlsl: case k_bfmlal: case k_bfmlsl:
    case k_smlall: case k_umlall: case k_smlsll: case k_umlsll: case k_sumlall: case k_usmlall: {
      const bool sub = op == k_smlsl || op == k_umlsl || op == k_fmlsl || op == k_bfmlsl || op == k_smlsll || op == k_umlsll;
      const bool n_signed = op == k_smlal || op == k_smlsl || op == k_smlall || op == k_smlsll || op == k_sumlall;
      const bool m_signed = op == k_smlal || op == k_smlsl || op == k_smlall || op == k_smlsll || op == k_usmlall;
      const bool fp = op == k_fmlal || op == k_fmlsl || op == k_bfmlal || op == k_bfmlsl, bf = op == k_bfmlal || op == k_bfmlsl;
      for (uint32_t r = 0; r < n; ++r)
        for (uint32_t j = 0; j < scale; ++j) {
          Vec& row = s_.za[row_of(r, j)];
          for (uint32_t l = 0; l < lanes; ++l) {
            const uint64_t a = get(zn_vec(r), src, scale * l + j);
            const uint64_t b = (f.shape == 3 || f.shape == 5) ? get(s_.v[zm], src, idx) : get(zm_vec(r), src, scale * l + j);
            if (fp) {
              auto w = [&](uint64_t v) { return bf ? (v & 0xFFFF) << 16 : convert(Format::F16, Format::F32, v, env); };
              const uint64_t x = sub ? negate(Format::F32, w(a)) : w(a);
              put(&row, 2, l, default_nan(Format::F32, fused_multiply_add(Format::F32, x, w(b), get(row, 2, l), env)));
              continue;
            }
            const uint64_t product = static_cast<uint64_t>(ext(a, src, n_signed) * ext(b, src, m_signed));
            put(&row, dst, l, (sub ? get(row, dst, l) - product : get(row, dst, l) + product) & element_ones(dst));
          }
        }
      return Step::Ok;
    }
    default: break;
  }
  return undefined();
}

AArch64Cpu::Step AArch64Cpu::sme2(GuestMemory& m, uint32_t insn) {
  for (uint32_t i = 0; i < sizeof(kSme2Forms) / sizeof(kSme2Forms[0]); ++i)
    if ((insn & kSme2Forms[i].mask) == kSme2Forms[i].value) return sme2_array(i, insn);
  return undefined();
}

}  // namespace emulation
