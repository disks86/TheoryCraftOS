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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnEmu.hpp"
#include "Math/FloatMath.hpp"

namespace gcn {

namespace {
inline float as_float(uint32_t bits) {
  float f;
  __builtin_memcpy(&f, &bits, sizeof f);
  return f;
}
inline uint32_t as_bits(float f) {
  uint32_t b;
  __builtin_memcpy(&b, &f, sizeof b);
  return b;
}
inline bool is_nan(float f) { return f != f; }
// a denormal is flushed to a zero of the same sign
inline float ftz(float f) {
  const uint32_t b = as_bits(f);
  return (b & 0x7F800000u) == 0 ? as_float(b & 0x80000000u) : f;
}
inline float floor_f(float f) {
  if (is_nan(f) || f >= 8388608.0f || f <= -8388608.0f) return f;
  const float t = static_cast<float>(static_cast<int32_t>(f));
  return t > f ? t - 1.0f : t;
}
inline float trunc_f(float f) {
  if (is_nan(f) || f >= 8388608.0f || f <= -8388608.0f) return f;
  return static_cast<float>(static_cast<int32_t>(f));
}
inline float ceil_f(float f) { return -floor_f(-f); }
inline float rndne_f(float f) {
  if (is_nan(f) || f >= 8388608.0f || f <= -8388608.0f) return f;
  const float fl = floor_f(f);
  const float diff = f - fl;
  if (diff < 0.5f) return fl;
  if (diff > 0.5f) return fl + 1.0f;
  return (static_cast<int32_t>(fl) & 1) ? fl + 1.0f : fl;       // a tie goes to the even one
}
float exp2_f(float x) {
  if (is_nan(x)) return x;
  if (x >= 128.0f) return as_float(0x7F800000u);
  if (x <= -150.0f) return 0.0f;
  return static_cast<float>(fmath::exp(static_cast<double>(x) * 0.69314718055994530942));
}
float log2_f(float x) {
  if (is_nan(x) || x < 0.0f) return as_float(0x7FC00000u);
  if (x == 0.0f) return as_float(0xFF800000u);
  if (as_bits(x) == 0x7F800000u) return x;
  return static_cast<float>(fmath::ln(static_cast<double>(x)) * 1.44269504088896340736);
}
float sqrt_f(float x) {
  if (is_nan(x) || x < 0.0f) return as_float(0x7FC00000u);
  if (x == 0.0f || as_bits(x) == 0x7F800000u) return x;
  return static_cast<float>(fmath::sqrt(static_cast<double>(x)));
}
float rcp_f(float x) {
  if (is_nan(x)) return x;
  if (x == 0.0f) return as_float(as_bits(x) & 0x80000000u ? 0xFF800000u : 0x7F800000u);
  if (as_bits(x) == 0x7F800000u || as_bits(x) == 0xFF800000u) return as_float(as_bits(x) & 0x80000000u);
  return static_cast<float>(1.0 / static_cast<double>(x));
}
float rsq_f(float x) {
  if (is_nan(x) || x < 0.0f) return as_float(0x7FC00000u);
  if (x == 0.0f) return as_float(as_bits(x) & 0x80000000u ? 0xFF800000u : 0x7F800000u);
  if (as_bits(x) == 0x7F800000u) return 0.0f;
  return static_cast<float>(1.0 / fmath::sqrt(static_cast<double>(x)));
}
constexpr double kTwoPi = 6.28318530717958647692;
// (the hardware reduces the angle to its fraction first; the result for |turns| above 256 is not specified)
float sin_f(float turns) { return is_nan(turns) ? turns : static_cast<float>(fmath::sin(static_cast<double>(turns - floor_f(turns)) * kTwoPi)); }
float cos_f(float turns) { return is_nan(turns) ? turns : static_cast<float>(fmath::cos(static_cast<double>(turns - floor_f(turns)) * kTwoPi)); }
float fract_f(float x) {
  if (is_nan(x)) return x;
  const float f = x - floor_f(x);
  return f >= 1.0f ? as_float(0x3F7FFFFFu) : f;
}
float min_f(float a, float b) { return is_nan(a) ? b : is_nan(b) ? a : (a < b ? a : b); }
float max_f(float a, float b) { return is_nan(a) ? b : is_nan(b) ? a : (a > b ? a : b); }
float med3_f(float a, float b, float c) { return max_f(min_f(a, b), min_f(max_f(a, b), c)); }
// the conversion of a float to a signed / unsigned integer saturates; a NaN is 0
int32_t to_i32(float f) {
  if (is_nan(f)) return 0;
  if (f >= 2147483648.0f) return 0x7FFFFFFF;
  if (f <= -2147483648.0f) return static_cast<int32_t>(0x80000000u);
  return static_cast<int32_t>(f);
}
uint32_t to_u32(float f) {
  if (is_nan(f) || f <= 0.0f) return 0;
  if (f >= 4294967296.0f) return 0xFFFFFFFFu;
  return static_cast<uint32_t>(f);
}
// float32 to float16, round to zero (v_cvt_pkrtz_f16_f32)
uint16_t to_half_rtz(float f) {
  const uint32_t b = as_bits(f);
  const uint32_t sign = (b >> 16) & 0x8000u;
  const int32_t exponent = static_cast<int32_t>((b >> 23) & 0xFF) - 127 + 15;
  const uint32_t mantissa = b & 0x7FFFFFu;
  if (((b >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));       // infinity or NaN
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);       // too big: the largest finite (round to zero)
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<uint16_t>(sign);
    const uint32_t m = (mantissa | 0x800000u) >> (14 - exponent);
    return static_cast<uint16_t>(sign | m);
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}
}  // namespace

Wave::Wave() { reset(kWaveSize); }

void Wave::reset(uint32_t lanes) {
  for (uint32_t& s : sgpr) s = 0;
  for (auto& row : vgpr)
    for (uint32_t& v : row) v = 0;
  vcc = 0;
  exec = lanes >= 64 ? ~0ull : ((1ull << lanes) - 1);
  m0 = 0;
  scc = false;
  pc = 0;
  steps = 0;
  status = Status::Running;
}

float Wave::vgpr_f(uint32_t reg, uint32_t lane) const { return as_float(vgpr[reg & 255][lane & 63]); }
void Wave::set_vgpr_f(uint32_t reg, uint32_t lane, float value) { vgpr[reg & 255][lane & 63] = as_bits(value); }

uint32_t Wave::get_sreg(uint32_t index) const {
  if (index <= 101) return sgpr[index];
  switch (index) {
    case 106: return static_cast<uint32_t>(vcc);
    case 107: return static_cast<uint32_t>(vcc >> 32);
    case 124: return m0;
    case 126: return static_cast<uint32_t>(exec);
    case 127: return static_cast<uint32_t>(exec >> 32);
    default: return 0;
  }
}

// A source operand for one lane: scalar registers and constants for every lane, vector registers per lane.
bool Wave::src32(uint32_t code, uint32_t literal, uint32_t lane, uint32_t* out) const {
  if (code >= 256) {
    *out = vgpr[code - 256][lane];
    return true;
  }
  if (code <= 101 || code == 106 || code == 107 || code == 124 || code == 126 || code == 127) {
    *out = get_sreg(code);
    return true;
  }
  if (code >= 128 && code <= 192) { *out = code - 128; return true; }
  if (code >= 193 && code <= 208) { *out = static_cast<uint32_t>(-static_cast<int32_t>(code - 192)); return true; }
  switch (code) {
    case 240: *out = 0x3F000000u; return true;
    case 241: *out = 0xBF000000u; return true;
    case 242: *out = 0x3F800000u; return true;
    case 243: *out = 0xBF800000u; return true;
    case 244: *out = 0x40000000u; return true;
    case 245: *out = 0xC0000000u; return true;
    case 246: *out = 0x40800000u; return true;
    case 247: *out = 0xC0800000u; return true;
    case 251: *out = vcc == 0 ? 1 : 0; return true;
    case 252: *out = exec == 0 ? 1 : 0; return true;
    case 253: *out = scc ? 1 : 0; return true;
    case 255: *out = literal; return true;
    default: return false;
  }
}

bool Wave::src64(uint32_t code, uint32_t literal, uint64_t* out) const {
  if (code <= 100 || code == 106 || code == 126) {
    *out = uint64_t{get_sreg(code)} | (uint64_t{get_sreg(code + 1)} << 32);
    return true;
  }
  uint32_t v = 0;
  if (!src32(code, literal, 0, &v)) return false;
  // a constant used as 64 bits: sign extended for the inline integers, the literal zero extended
  *out = (code >= 128 && code <= 208) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))) : uint64_t{v};
  return true;
}

void Wave::put_sdst(uint32_t code, uint32_t value) {
  if (code <= 101) sgpr[code] = value;
  else if (code == 106) vcc = (vcc & ~0xFFFFFFFFull) | value;
  else if (code == 107) vcc = (vcc & 0xFFFFFFFFull) | (uint64_t{value} << 32);
  else if (code == 124) m0 = value;
  else if (code == 126) exec = (exec & ~0xFFFFFFFFull) | value;
  else if (code == 127) exec = (exec & 0xFFFFFFFFull) | (uint64_t{value} << 32);
}

void Wave::put_sdst64(uint32_t code, uint64_t value) {
  put_sdst(code, static_cast<uint32_t>(value));
  put_sdst(code + 1, static_cast<uint32_t>(value >> 32));
}

Status Wave::exec_sop1(uint32_t w, uint32_t literal) {
  const uint32_t op = (w >> 8) & 0xFF, sdst = (w >> 16) & 0x7F, code = w & 0xFF;
  uint32_t a = 0;
  uint64_t a64 = 0;
  const Sop1 o = static_cast<Sop1>(op);
  const bool wide = o == Sop1::s_mov_b64 || o == Sop1::s_not_b64 || o == Sop1::s_and_saveexec_b64 || o == Sop1::s_or_saveexec_b64 || o == Sop1::s_xor_saveexec_b64 ||
                    o == Sop1::s_andn2_saveexec_b64 || o == Sop1::s_orn2_saveexec_b64 || o == Sop1::s_nand_saveexec_b64;
  if (wide ? !src64(code, literal, &a64) : !src32(code, literal, 0, &a)) return fail(Status::BadInstruction);
  switch (o) {
    case Sop1::s_mov_b32: put_sdst(sdst, a); break;
    case Sop1::s_not_b32: put_sdst(sdst, ~a); scc = ~a != 0; break;
    case Sop1::s_cmov_b32: if (scc) put_sdst(sdst, a); break;
    case Sop1::s_mov_b64: put_sdst64(sdst, a64); break;
    case Sop1::s_not_b64: put_sdst64(sdst, ~a64); scc = ~a64 != 0; break;
    case Sop1::s_and_saveexec_b64: { const uint64_t old = exec; put_sdst64(sdst, old); exec = a64 & old; scc = exec != 0; break; }
    case Sop1::s_or_saveexec_b64: { const uint64_t old = exec; put_sdst64(sdst, old); exec = a64 | old; scc = exec != 0; break; }
    case Sop1::s_xor_saveexec_b64: { const uint64_t old = exec; put_sdst64(sdst, old); exec = a64 ^ old; scc = exec != 0; break; }
    case Sop1::s_andn2_saveexec_b64: { const uint64_t old = exec; put_sdst64(sdst, old); exec = a64 & ~old; scc = exec != 0; break; }
    case Sop1::s_orn2_saveexec_b64: { const uint64_t old = exec; put_sdst64(sdst, old); exec = a64 | ~old; scc = exec != 0; break; }
    case Sop1::s_nand_saveexec_b64: { const uint64_t old = exec; put_sdst64(sdst, old); exec = ~(a64 & old); scc = exec != 0; break; }
    default: return fail(Status::BadInstruction);
  }
  return Status::Running;
}

Status Wave::exec_sop2(uint32_t w, uint32_t literal) {
  const uint32_t op = (w >> 23) & 0x7F, sdst = (w >> 16) & 0x7F, c1 = (w >> 8) & 0xFF, c0 = w & 0xFF;
  const Sop2 o = static_cast<Sop2>(op);
  const bool wide = o == Sop2::s_and_b64 || o == Sop2::s_or_b64 || o == Sop2::s_xor_b64 || o == Sop2::s_andn2_b64 || o == Sop2::s_orn2_b64 || o == Sop2::s_nand_b64 ||
                    o == Sop2::s_nor_b64 || o == Sop2::s_cselect_b64;
  if (wide) {
    uint64_t a = 0, b = 0;
    if (!src64(c0, literal, &a) || !src64(c1, literal, &b)) return fail(Status::BadInstruction);
    uint64_t r = 0;
    switch (o) {
      case Sop2::s_and_b64: r = a & b; scc = r != 0; break;
      case Sop2::s_or_b64: r = a | b; scc = r != 0; break;
      case Sop2::s_xor_b64: r = a ^ b; scc = r != 0; break;
      case Sop2::s_andn2_b64: r = a & ~b; scc = r != 0; break;
      case Sop2::s_orn2_b64: r = a | ~b; scc = r != 0; break;
      case Sop2::s_nand_b64: r = ~(a & b); scc = r != 0; break;
      case Sop2::s_nor_b64: r = ~(a | b); scc = r != 0; break;
      default: r = scc ? a : b; break;       // s_cselect_b64
    }
    put_sdst64(sdst, r);
    return Status::Running;
  }
  uint32_t a = 0, b = 0;
  if (!src32(c0, literal, 0, &a) || !src32(c1, literal, 0, &b)) return fail(Status::BadInstruction);
  uint32_t r = 0;
  switch (o) {
    case Sop2::s_add_u32: { const uint64_t s = uint64_t{a} + b; r = static_cast<uint32_t>(s); scc = (s >> 32) != 0; break; }
    case Sop2::s_sub_u32: r = a - b; scc = b > a; break;
    case Sop2::s_add_i32: r = a + b; scc = ((~(a ^ b)) & (a ^ r)) >> 31; break;
    case Sop2::s_sub_i32: r = a - b; scc = ((a ^ b) & (a ^ r)) >> 31; break;
    case Sop2::s_addc_u32: { const uint64_t s = uint64_t{a} + b + (scc ? 1 : 0); r = static_cast<uint32_t>(s); scc = (s >> 32) != 0; break; }
    case Sop2::s_subb_u32: { const uint64_t s = uint64_t{b} + (scc ? 1 : 0); r = a - static_cast<uint32_t>(s); scc = s > a; break; }
    case Sop2::s_min_i32: scc = static_cast<int32_t>(a) < static_cast<int32_t>(b); r = scc ? a : b; break;
    case Sop2::s_min_u32: scc = a < b; r = scc ? a : b; break;
    case Sop2::s_max_i32: scc = static_cast<int32_t>(a) > static_cast<int32_t>(b); r = scc ? a : b; break;
    case Sop2::s_max_u32: scc = a > b; r = scc ? a : b; break;
    case Sop2::s_and_b32: r = a & b; scc = r != 0; break;
    case Sop2::s_or_b32: r = a | b; scc = r != 0; break;
    case Sop2::s_xor_b32: r = a ^ b; scc = r != 0; break;
    case Sop2::s_andn2_b32: r = a & ~b; scc = r != 0; break;
    case Sop2::s_orn2_b32: r = a | ~b; scc = r != 0; break;
    case Sop2::s_nand_b32: r = ~(a & b); scc = r != 0; break;
    case Sop2::s_nor_b32: r = ~(a | b); scc = r != 0; break;
    case Sop2::s_lshl_b32: r = a << (b & 31); scc = r != 0; break;
    case Sop2::s_lshr_b32: r = a >> (b & 31); scc = r != 0; break;
    case Sop2::s_ashr_i32: r = static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 31)); scc = r != 0; break;
    case Sop2::s_mul_i32: r = static_cast<uint32_t>(static_cast<int32_t>(a) * static_cast<int32_t>(b)); break;
    case Sop2::s_cselect_b32: r = scc ? a : b; break;
    default: return fail(Status::BadInstruction);
  }
  put_sdst(sdst, r);
  return Status::Running;
}

Status Wave::exec_sopk(uint32_t w) {
  const uint32_t op = (w >> 23) & 0x1F, sdst = (w >> 16) & 0x7F;
  const int32_t imm = static_cast<int16_t>(w & 0xFFFF);
  const uint32_t old = get_sreg(sdst);
  switch (static_cast<Sopk>(op)) {
    case Sopk::s_movk_i32: put_sdst(sdst, static_cast<uint32_t>(imm)); break;
    case Sopk::s_cmovk_i32: if (scc) put_sdst(sdst, static_cast<uint32_t>(imm)); break;
    case Sopk::s_addk_i32: { const uint32_t r = old + static_cast<uint32_t>(imm); scc = ((~(old ^ static_cast<uint32_t>(imm))) & (old ^ r)) >> 31; put_sdst(sdst, r); break; }
    case Sopk::s_mulk_i32: put_sdst(sdst, static_cast<uint32_t>(static_cast<int32_t>(old) * imm)); break;
    default: return fail(Status::BadInstruction);
  }
  return Status::Running;
}

Status Wave::exec_sopc(uint32_t w, uint32_t literal) {
  const uint32_t op = (w >> 16) & 0x7F;
  uint32_t a = 0, b = 0;
  if (!src32(w & 0xFF, literal, 0, &a) || !src32((w >> 8) & 0xFF, literal, 0, &b)) return fail(Status::BadInstruction);
  const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
  switch (static_cast<Sopc>(op)) {
    case Sopc::s_cmp_eq_i32: case Sopc::s_cmp_eq_u32: scc = a == b; break;
    case Sopc::s_cmp_lg_i32: case Sopc::s_cmp_lg_u32: scc = a != b; break;
    case Sopc::s_cmp_gt_i32: scc = sa > sb; break;
    case Sopc::s_cmp_ge_i32: scc = sa >= sb; break;
    case Sopc::s_cmp_lt_i32: scc = sa < sb; break;
    case Sopc::s_cmp_le_i32: scc = sa <= sb; break;
    case Sopc::s_cmp_gt_u32: scc = a > b; break;
    case Sopc::s_cmp_ge_u32: scc = a >= b; break;
    case Sopc::s_cmp_lt_u32: scc = a < b; break;
    case Sopc::s_cmp_le_u32: scc = a <= b; break;
    default: return fail(Status::BadInstruction);
  }
  return Status::Running;
}

Status Wave::exec_sopp(uint32_t w) {
  const uint32_t op = (w >> 16) & 0x7F;
  const int32_t simm = static_cast<int16_t>(w & 0xFFFF);
  bool take = false;
  switch (static_cast<Sopp>(op)) {
    case Sopp::s_nop: case Sopp::s_waitcnt: case Sopp::s_barrier: return Status::Running;
    case Sopp::s_endpgm: status = Status::Done; return Status::Done;
    case Sopp::s_branch: take = true; break;
    case Sopp::s_cbranch_scc0: take = !scc; break;
    case Sopp::s_cbranch_scc1: take = scc; break;
    case Sopp::s_cbranch_vccz: take = vcc == 0; break;
    case Sopp::s_cbranch_vccnz: take = vcc != 0; break;
    case Sopp::s_cbranch_execz: take = exec == 0; break;
    case Sopp::s_cbranch_execnz: take = exec != 0; break;
    default: return fail(Status::BadInstruction);
  }
  if (take) {
    const int64_t target = static_cast<int64_t>(pc) + 1 + simm;
    if (target < 0) return fail(Status::BadBranch);
    pc = static_cast<uint32_t>(target) - 1;          // (step adds the instruction's length)
  }
  return Status::Running;
}

Status Wave::exec_smem(uint32_t w0, uint32_t w1, IEnvironment& env) {
  const uint32_t op = (w0 >> 18) & 0xFF, sdata = (w0 >> 6) & 0x7F, sbase = (w0 & 0x3F) * 2;
  const bool imm = (w0 >> 17) & 1;
  uint32_t count = 0;
  bool buffer = false;
  switch (static_cast<Smem>(op)) {
    case Smem::s_load_dword: count = 1; break;
    case Smem::s_load_dwordx2: count = 2; break;
    case Smem::s_load_dwordx4: count = 4; break;
    case Smem::s_load_dwordx8: count = 8; break;
    case Smem::s_load_dwordx16: count = 16; break;
    case Smem::s_buffer_load_dword: count = 1; buffer = true; break;
    case Smem::s_buffer_load_dwordx2: count = 2; buffer = true; break;
    case Smem::s_buffer_load_dwordx4: count = 4; buffer = true; break;
    case Smem::s_buffer_load_dwordx8: count = 8; buffer = true; break;
    case Smem::s_buffer_load_dwordx16: count = 16; buffer = true; break;
    default: return fail(Status::BadInstruction);
  }
  const uint32_t offset = imm ? (w1 & 0xFFFFF) : get_sreg(w1 & 0x7F);
  uint64_t base = uint64_t{sgpr[sbase]} | (uint64_t{sgpr[sbase + 1] & 0xFFFF} << 32);        // (a 48 bit address in both the pointer and the buffer descriptor)
  if (buffer) {
    const uint32_t num_records = sgpr[sbase + 2];
    if (offset > num_records || count * 4 > num_records - offset) {      // out of range: zeros
      for (uint32_t i = 0; i < count; ++i) sgpr[sdata + i] = 0;
      return Status::Running;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    if (!env.load32(base + offset + i * 4, &v)) return fail(Status::MemoryFault);
    sgpr[sdata + i] = v;
  }
  return Status::Running;
}

namespace {
// v_cmp_class_f32: the class of the number against the mask (bit 0 signaling NaN, 1 quiet NaN, 2 -infinity, 3 -normal, 4 -denormal, 5 -zero, 6 +zero, 7 +denormal, 8 +normal, 9 +infinity)
bool float_class(uint32_t bits, uint32_t mask) {
  const bool neg = bits >> 31;
  const uint32_t e = (bits >> 23) & 0xFF, m = bits & 0x7FFFFF;
  uint32_t cls;
  if (e == 0xFF) cls = m == 0 ? (neg ? 2 : 9) : ((m & 0x400000) ? 1 : 0);
  else if (e == 0) cls = m == 0 ? (neg ? 5 : 6) : (neg ? 4 : 7);
  else cls = neg ? 3 : 8;
  return (mask >> cls) & 1;
}
// The compare predicates of v_cmp_*_f32 by the low four bits of the opcode
bool compare_f(uint32_t idx, float a, float b) {
  const bool nan = is_nan(a) || is_nan(b);
  switch (idx) {
    case 0: return false;
    case 1: return !nan && a < b;
    case 2: return !nan && a == b;
    case 3: return !nan && a <= b;
    case 4: return !nan && a > b;
    case 5: return !nan && a != b;
    case 6: return !nan && a >= b;
    case 7: return !nan;
    case 8: return nan;
    case 9: return nan || !(a >= b);
    case 10: return nan || !(a != b);
    case 11: return nan || !(a > b);
    case 12: return nan || !(a <= b);
    case 13: return nan || a != b;
    case 14: return nan || !(a < b);
    default: return true;
  }
}
bool compare_i(uint32_t idx, int64_t a, int64_t b) {
  switch (idx & 7) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
  }
}
}  // namespace

// One vector ALU instruction (VOP1, VOP2, VOPC, VOP3), all lanes in EXEC.
Status Wave::exec_valu(uint32_t w0, uint32_t w1, uint32_t literal) {
  enum Kind { K1, K2, KC, K3 } kind;
  uint32_t op = 0, vdst = 0, code[3] = {0, 0, 0};
  uint32_t neg = 0, abs = 0, omod = 0, sdst_code = 0;
  bool clamp = false;
  if ((w0 & 0xFE000000u) == 0x7E000000u) {
    kind = K1; op = (w0 >> 9) & 0xFF; vdst = (w0 >> 17) & 0xFF; code[0] = w0 & 0x1FF;
  } else if ((w0 & 0xFE000000u) == 0x7C000000u) {
    kind = KC; op = (w0 >> 17) & 0xFF; code[0] = w0 & 0x1FF; code[1] = 256 + ((w0 >> 9) & 0xFF);
  } else if ((w0 & 0x80000000u) == 0) {
    kind = K2; op = (w0 >> 25) & 0x3F; vdst = (w0 >> 17) & 0xFF; code[0] = w0 & 0x1FF; code[1] = 256 + ((w0 >> 9) & 0xFF);
  } else {
    kind = K3;
    op = (w0 >> 16) & 0x3FF;
    vdst = w0 & 0xFF;
    sdst_code = (w0 >> 0) & 0x7F;
    clamp = (w0 >> 15) & 1;
    abs = (w0 >> 8) & 7;
    if (op >= 0x100 && op < 0x140) {
      const Vop2 o2 = static_cast<Vop2>(op - 0x100);
      if (o2 == Vop2::v_add_u32 || o2 == Vop2::v_sub_u32 || o2 == Vop2::v_subrev_u32) abs = 0;       // (VOP3b: those bits are the carry destination)
    }
    code[0] = w1 & 0x1FF; code[1] = (w1 >> 9) & 0x1FF; code[2] = (w1 >> 18) & 0x1FF;
    omod = (w1 >> 27) & 3;
    neg = (w1 >> 29) & 7;
    // VOP3 carrying a VOP1 / VOP2 / VOPC instruction
    if (op < 0x100) { kind = KC; }
    else if (op < 0x140) { kind = K2; op -= 0x100; }
    else if (op < 0x1C0) { kind = K1; op -= 0x140; }
  }
  const bool e64 = (w0 & 0xFC000000u) == 0xD0000000u;
  const auto read = [&](int n, uint32_t lane, uint32_t* out) { return src32(code[n], literal, lane, out); };
  const auto fsrc = [&](int n, uint32_t lane, float* out) {
    uint32_t b = 0;
    if (!read(n, lane, &b)) return false;
    float f = as_float(b);
    f = ftz(f);
    if (abs >> n & 1) f = as_float(as_bits(f) & 0x7FFFFFFFu);
    if (neg >> n & 1) f = as_float(as_bits(f) ^ 0x80000000u);
    *out = f;
    return true;
  };
  const auto isrc = [&](int n, uint32_t lane, uint32_t* out) { return read(n, lane, out); };
  const uint64_t mask = exec;
  uint64_t cmp_result = 0;
  uint64_t carry_out = 0;
  // v_cndmask: the select mask is VCC (32 bit encodings) or the scalar pair named by the third source
  uint64_t select = vcc;
  if (kind == K2 && static_cast<Vop2>(op) == Vop2::v_cndmask_b32 && e64) {
    if (!src64(code[2], literal, &select)) return fail(Status::BadInstruction);
  }
  // v_readfirstlane_b32: the first active lane's value into an SGPR
  if (kind == K1 && static_cast<Vop1>(op) == Vop1::v_readfirstlane_b32) {
    uint32_t lane = 0;
    while (lane < 64 && !(mask >> lane & 1)) ++lane;
    uint32_t v = 0;
    if (lane < 64 && !isrc(0, lane, &v)) return fail(Status::BadInstruction);
    put_sdst(e64 ? sdst_code : ((w0 >> 17) & 0xFF), v);
    return Status::Running;
  }
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    if (!(mask >> lane & 1)) continue;
    float a = 0, b = 0, c = 0;
    uint32_t ia = 0, ib = 0, ic = 0, r = 0;
    bool float_result = false;
    if (kind == KC) {
      const uint32_t cop = op;
      if (cop == static_cast<uint32_t>(Vopc::v_cmp_class_f32)) {
        // (the first source is the number as it is, with the source modifiers of the VOP3 form applied to the pattern; the second the mask)
        if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib)) return fail(Status::BadInstruction);
        if (abs & 1) ia &= 0x7FFFFFFFu;
        if (neg & 1) ia ^= 0x80000000u;
        if (float_class(ia, ib)) cmp_result |= 1ull << lane;
      } else if (cop >= static_cast<uint32_t>(Vopc::v_cmp_f_f32) && cop <= static_cast<uint32_t>(Vopc::v_cmp_tru_f32)) {
        if (!fsrc(0, lane, &a) || !fsrc(1, lane, &b)) return fail(Status::BadInstruction);
        if (compare_f(cop & 0xF, a, b)) cmp_result |= 1ull << lane;
      } else if (cop >= static_cast<uint32_t>(Vopc::v_cmp_f_i32) && cop <= static_cast<uint32_t>(Vopc::v_cmp_t_u32)) {
        if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib)) return fail(Status::BadInstruction);
        const bool is_signed = cop < static_cast<uint32_t>(Vopc::v_cmp_f_u32);
        if (compare_i(cop & 7, is_signed ? static_cast<int64_t>(static_cast<int32_t>(ia)) : static_cast<int64_t>(ia), is_signed ? static_cast<int64_t>(static_cast<int32_t>(ib)) : static_cast<int64_t>(ib)))
          cmp_result |= 1ull << lane;
      } else {
        return fail(Status::BadInstruction);
      }
      continue;
    }
    if (kind == K1) {
      const Vop1 o = static_cast<Vop1>(op);
      switch (o) {
        case Vop1::v_nop: continue;
        case Vop1::v_mov_b32: if (!isrc(0, lane, &r)) return fail(Status::BadInstruction); break;
        case Vop1::v_not_b32: if (!isrc(0, lane, &ia)) return fail(Status::BadInstruction); r = ~ia; break;
        case Vop1::v_cvt_i32_f32: if (!fsrc(0, lane, &a)) return fail(Status::BadInstruction); r = static_cast<uint32_t>(to_i32(a)); break;
        case Vop1::v_cvt_u32_f32: if (!fsrc(0, lane, &a)) return fail(Status::BadInstruction); r = to_u32(a); break;
        case Vop1::v_cvt_f32_i32: if (!isrc(0, lane, &ia)) return fail(Status::BadInstruction); r = as_bits(static_cast<float>(static_cast<int32_t>(ia))); float_result = true; a = as_float(r); break;
        case Vop1::v_cvt_f32_u32: if (!isrc(0, lane, &ia)) return fail(Status::BadInstruction); r = as_bits(static_cast<float>(ia)); float_result = true; a = as_float(r); break;
        default: {
          if (!fsrc(0, lane, &a)) return fail(Status::BadInstruction);
          float f = 0;
          switch (o) {
            case Vop1::v_fract_f32: f = fract_f(a); break;
            case Vop1::v_trunc_f32: f = trunc_f(a); break;
            case Vop1::v_ceil_f32: f = ceil_f(a); break;
            case Vop1::v_rndne_f32: f = rndne_f(a); break;
            case Vop1::v_floor_f32: f = floor_f(a); break;
            case Vop1::v_exp_f32: f = exp2_f(a); break;
            case Vop1::v_log_f32: f = log2_f(a); break;
            case Vop1::v_rcp_f32: case Vop1::v_rcp_iflag_f32: f = rcp_f(a); break;
            case Vop1::v_rsq_f32: f = rsq_f(a); break;
            case Vop1::v_sqrt_f32: f = sqrt_f(a); break;
            case Vop1::v_sin_f32: f = sin_f(a); break;
            case Vop1::v_cos_f32: f = cos_f(a); break;
            default: return fail(Status::BadInstruction);
          }
          r = as_bits(f);
          float_result = true;
        }
      }
    } else if (kind == K2) {
      const Vop2 o = static_cast<Vop2>(op);
      switch (o) {
        case Vop2::v_cndmask_b32:
          if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib)) return fail(Status::BadInstruction);
          // (the VOP3 form applies the source modifiers to the sign bit of the selected value)
          if (abs & 1) ia &= 0x7FFFFFFFu;
          if (abs & 2) ib &= 0x7FFFFFFFu;
          if (neg & 1) ia ^= 0x80000000u;
          if (neg & 2) ib ^= 0x80000000u;
          r = (select >> lane & 1) ? ib : ia;
          break;
        case Vop2::v_add_f32: case Vop2::v_sub_f32: case Vop2::v_subrev_f32: case Vop2::v_mul_f32: case Vop2::v_min_f32: case Vop2::v_max_f32: case Vop2::v_mac_f32:
        case Vop2::v_madmk_f32: case Vop2::v_madak_f32: {
          if (!fsrc(0, lane, &a) || !fsrc(1, lane, &b)) return fail(Status::BadInstruction);
          float f = 0;
          switch (o) {
            case Vop2::v_add_f32: f = a + b; break;
            case Vop2::v_sub_f32: f = a - b; break;
            case Vop2::v_subrev_f32: f = b - a; break;
            case Vop2::v_mul_f32: f = a * b; break;
            case Vop2::v_min_f32: f = min_f(a, b); break;
            case Vop2::v_max_f32: f = max_f(a, b); break;
            case Vop2::v_mac_f32: f = vgpr_f(vdst, lane) + a * b; break;
            case Vop2::v_madmk_f32: f = a * as_float(literal) + b; break;
            default: f = a * b + as_float(literal); break;       // v_madak_f32
          }
          r = as_bits(ftz(f));
          float_result = true;
          break;
        }
        case Vop2::v_min_i32: case Vop2::v_max_i32: case Vop2::v_min_u32: case Vop2::v_max_u32: case Vop2::v_lshrrev_b32: case Vop2::v_ashrrev_i32: case Vop2::v_lshlrev_b32:
        case Vop2::v_and_b32: case Vop2::v_or_b32: case Vop2::v_xor_b32: case Vop2::v_mul_i32_i24: case Vop2::v_mul_u32_u24: {
          if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib)) return fail(Status::BadInstruction);
          switch (o) {
            case Vop2::v_min_i32: r = static_cast<int32_t>(ia) < static_cast<int32_t>(ib) ? ia : ib; break;
            case Vop2::v_max_i32: r = static_cast<int32_t>(ia) > static_cast<int32_t>(ib) ? ia : ib; break;
            case Vop2::v_min_u32: r = ia < ib ? ia : ib; break;
            case Vop2::v_max_u32: r = ia > ib ? ia : ib; break;
            case Vop2::v_lshrrev_b32: r = ib >> (ia & 31); break;
            case Vop2::v_ashrrev_i32: r = static_cast<uint32_t>(static_cast<int32_t>(ib) >> (ia & 31)); break;
            case Vop2::v_lshlrev_b32: r = ib << (ia & 31); break;
            case Vop2::v_and_b32: r = ia & ib; break;
            case Vop2::v_or_b32: r = ia | ib; break;
            case Vop2::v_xor_b32: r = ia ^ ib; break;
            case Vop2::v_mul_i32_i24: r = static_cast<uint32_t>(((static_cast<int32_t>(ia << 8)) >> 8) * ((static_cast<int32_t>(ib << 8)) >> 8)); break;
            default: r = (ia & 0xFFFFFF) * (ib & 0xFFFFFF); break;
          }
          break;
        }
        case Vop2::v_add_u32: case Vop2::v_sub_u32: case Vop2::v_subrev_u32: {
          if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib)) return fail(Status::BadInstruction);
          if (o == Vop2::v_add_u32) { const uint64_t s = uint64_t{ia} + ib; r = static_cast<uint32_t>(s); if (s >> 32) carry_out |= 1ull << lane; }
          else if (o == Vop2::v_sub_u32) { r = ia - ib; if (ib > ia) carry_out |= 1ull << lane; }
          else { r = ib - ia; if (ia > ib) carry_out |= 1ull << lane; }
          break;
        }
        default: return fail(Status::BadInstruction);
      }
    } else {       // native VOP3
      const Vop3 o = static_cast<Vop3>(op);
      switch (o) {
        case Vop3::v_mad_f32: case Vop3::v_fma_f32: case Vop3::v_min3_f32: case Vop3::v_max3_f32: case Vop3::v_med3_f32: {
          if (!fsrc(0, lane, &a) || !fsrc(1, lane, &b) || !fsrc(2, lane, &c)) return fail(Status::BadInstruction);
          float f = 0;
          if (o == Vop3::v_mad_f32) f = a * b + c;
          else if (o == Vop3::v_fma_f32) f = static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
          else if (o == Vop3::v_min3_f32) f = min_f(min_f(a, b), c);
          else if (o == Vop3::v_max3_f32) f = max_f(max_f(a, b), c);
          else f = med3_f(a, b, c);
          r = as_bits(ftz(f));
          float_result = true;
          break;
        }
        case Vop3::v_cvt_pkrtz_f16_f32: {
          if (!fsrc(0, lane, &a) || !fsrc(1, lane, &b)) return fail(Status::BadInstruction);
          r = static_cast<uint32_t>(to_half_rtz(a)) | (static_cast<uint32_t>(to_half_rtz(b)) << 16);
          break;
        }
        case Vop3::v_bfe_u32: case Vop3::v_bfe_i32: {
          if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib) || !isrc(2, lane, &ic)) return fail(Status::BadInstruction);
          const uint32_t offset = ib & 31, width = ic & 31;
          if (width == 0) { r = 0; break; }
          if (o == Vop3::v_bfe_u32) r = (ia >> offset) & ((1u << width) - 1);
          else { const int32_t sh = static_cast<int32_t>(ia << (32 - offset - width < 32 ? 32 - offset - width : 0)); r = static_cast<uint32_t>(sh >> (32 - width)); }
          break;
        }
        case Vop3::v_bfi_b32: {
          if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib) || !isrc(2, lane, &ic)) return fail(Status::BadInstruction);
          r = (ia & ib) | (~ia & ic);
          break;
        }
        case Vop3::v_mad_i32_i24: case Vop3::v_mad_u32_u24: {
          if (!isrc(0, lane, &ia) || !isrc(1, lane, &ib) || !isrc(2, lane, &ic)) return fail(Status::BadInstruction);
          if (o == Vop3::v_mad_i32_i24) r = static_cast<uint32_t>(((static_cast<int32_t>(ia << 8)) >> 8) * ((static_cast<int32_t>(ib << 8)) >> 8)) + ic;
          else r = (ia & 0xFFFFFF) * (ib & 0xFFFFFF) + ic;
          break;
        }
        default:
          // (v_mul_lo_u32 / v_mul_hi_u32 are VOP3 only on GFX8 but not used: they are not in the Vop3 enum)
          return fail(Status::BadInstruction);
      }
    }
    if (float_result) {
      float f = as_float(r);
      if (omod == 1) f *= 2.0f; else if (omod == 2) f *= 4.0f; else if (omod == 3) f *= 0.5f;
      if (clamp) f = is_nan(f) ? 0.0f : (f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f));
      r = as_bits(f);
    }
    vgpr[vdst][lane] = r;
  }
  if (kind == KC) {
    const uint64_t result = cmp_result & mask;
    if (e64) put_sdst64(sdst_code, result);
    else vcc = result;
  } else if (kind == K2 && (static_cast<Vop2>(op) == Vop2::v_add_u32 || static_cast<Vop2>(op) == Vop2::v_sub_u32 || static_cast<Vop2>(op) == Vop2::v_subrev_u32)) {
    if (e64) put_sdst64((w0 >> 8) & 0x7F, carry_out & mask);        // the VOP3b form: the carry mask to the SGPR pair in bits 14:8
    else vcc = carry_out & mask;
  }
  return Status::Running;
}

Status Wave::exec_vintrp(uint32_t w, IEnvironment& env) {
  const uint32_t vdst = (w >> 18) & 0xFF, op = (w >> 16) & 3, attr = (w >> 10) & 0x3F, chan = (w >> 8) & 3, vsrc = w & 0xFF;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    if (!(exec >> lane & 1)) continue;
    float p0 = 0, p10 = 0, p20 = 0;
    env.interpolation(attr, chan, lane, &p0, &p10, &p20);
    float r = 0;
    switch (static_cast<Vintrp>(op)) {
      case Vintrp::v_interp_p1_f32: r = p10 * vgpr_f(vsrc, lane) + p0; break;
      case Vintrp::v_interp_p2_f32: r = p20 * vgpr_f(vsrc, lane) + vgpr_f(vdst, lane); break;
      case Vintrp::v_interp_mov_f32: r = vsrc == 0 ? p10 : (vsrc == 1 ? p20 : p0); break;       // the parameter named by vsrc: P10, P20 or P0
      default: return fail(Status::BadInstruction);
    }
    set_vgpr_f(vdst, lane, r);
  }
  return Status::Running;
}

Status Wave::exec_exp(uint32_t w0, uint32_t w1, IEnvironment& env) {
  const uint32_t en = w0 & 0xF, target = (w0 >> 4) & 0x3F;
  const bool compr = (w0 >> 10) & 1, done = (w0 >> 11) & 1;
  const uint32_t reg[4] = {w1 & 0xFF, (w1 >> 8) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 24) & 0xFF};
  uint32_t values[kWaveSize][4];
  for (uint32_t lane = 0; lane < kWaveSize; ++lane)
    for (uint32_t i = 0; i < 4; ++i) values[lane][i] = (en >> i & 1) ? vgpr[reg[i]][lane] : 0;
  env.export_data(target, en, compr, values, exec, done);
  return Status::Running;
}

namespace {
struct BufferDesc {
  uint64_t base;
  uint32_t stride, num_records, data_format, num_format;
};
BufferDesc decode_buffer(const uint32_t* v) {
  BufferDesc d;
  d.base = uint64_t{v[0]} | (uint64_t{v[1] & 0xFFFF} << 32);
  d.stride = (v[1] >> 16) & 0x3FFF;
  d.num_records = v[2];
  d.num_format = (v[3] >> 12) & 7;
  d.data_format = (v[3] >> 15) & 0xF;
  return d;
}
}  // namespace

Status Wave::exec_mubuf(uint32_t w0, uint32_t w1, IEnvironment& env) {
  const uint32_t offset = w0 & 0xFFF;
  const bool offen = (w0 >> 12) & 1, idxen = (w0 >> 13) & 1;
  const uint32_t op = (w0 >> 18) & 0x7F;
  const uint32_t vaddr = w1 & 0xFF, vdata = (w1 >> 8) & 0xFF, srsrc = ((w1 >> 16) & 0x1F) * 4, soffset_code = (w1 >> 24) & 0xFF;
  uint32_t soffset = 0;
  if (!src32(soffset_code, 0, 0, &soffset)) return fail(Status::BadInstruction);
  const BufferDesc d = decode_buffer(&sgpr[srsrc]);
  uint32_t count = 0;
  bool format = false, store = false;
  switch (static_cast<Mubuf>(op)) {
    case Mubuf::buffer_load_format_x: count = 1; format = true; break;
    case Mubuf::buffer_load_format_xy: count = 2; format = true; break;
    case Mubuf::buffer_load_format_xyz: count = 3; format = true; break;
    case Mubuf::buffer_load_format_xyzw: count = 4; format = true; break;
    case Mubuf::buffer_load_dword: count = 1; break;
    case Mubuf::buffer_load_dwordx2: count = 2; break;
    case Mubuf::buffer_load_dwordx3: count = 3; break;
    case Mubuf::buffer_load_dwordx4: count = 4; break;
    case Mubuf::buffer_store_dword: count = 1; store = true; break;
    case Mubuf::buffer_store_dwordx2: count = 2; store = true; break;
    case Mubuf::buffer_store_dwordx4: count = 4; store = true; break;
    case Mubuf::buffer_store_format_xyzw: return fail(Status::BadInstruction);
    default: return fail(Status::BadInstruction);
  }
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    if (!(exec >> lane & 1)) continue;
    // the address: the index (idxen) and the offset (offen) from the VGPRs (index first when both), the stride, the immediate offset and the scalar offset
    uint32_t reg = vaddr;
    uint64_t index = 0, voffset = 0;
    if (idxen) index = vgpr[reg++][lane];
    if (offen) voffset = vgpr[reg][lane];
    const uint64_t byte = index * d.stride + voffset + offset;
    const uint64_t address_offset = byte + soffset;
    // the range check (GCN3 ISA, "buffer range checking"): with a stride the index must be inside the buffer and the offset inside the record; without, the access inside the bytes
    const uint32_t element = format ? (d.data_format == 14 ? 16 : d.data_format == 13 ? 12 : d.data_format == 11 ? 8 : d.data_format == 4 ? 4 : d.data_format == 10 ? 4 : 0) : count * 4;
    if (format && element == 0) return fail(Status::BadInstruction);
    bool out_of_range;
    if (d.stride != 0) out_of_range = index >= d.num_records || voffset + offset >= d.stride;
    else out_of_range = byte + (format ? 4 * count : element) > d.num_records;
    if (out_of_range) {
      if (!store) for (uint32_t i = 0; i < count; ++i) vgpr[vdata + i][lane] = (format && i == 3) ? 0x3F800000u : 0;
      continue;
    }
    const uint64_t address = d.base + address_offset;
    if (store) {
      for (uint32_t i = 0; i < count; ++i)
        if (!env.store32(address + i * 4, vgpr[vdata + i][lane])) return fail(Status::MemoryFault);
      continue;
    }
    if (!format) {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = 0;
        if (!env.load32(address + i * 4, &v)) return fail(Status::MemoryFault);
        vgpr[vdata + i][lane] = v;
      }
      continue;
    }
    // a typed load: the components of the format, missing ones 0, 0, 0, 1 (the destination selects in the descriptor are the identity here), the first `count` of them into the VGPRs
    float comp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (d.data_format == 10) {       // 8_8_8_8: unorm
      uint32_t v = 0;
      if (!env.load32(address, &v)) return fail(Status::MemoryFault);
      for (uint32_t i = 0; i < 4; ++i) comp[i] = static_cast<float>((v >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
    } else {
      const uint32_t n = d.data_format == 14 ? 4 : d.data_format == 13 ? 3 : d.data_format == 11 ? 2 : 1;
      for (uint32_t i = 0; i < n && i < count; ++i) {
        uint32_t v = 0;
        if (!env.load32(address + i * 4, &v)) return fail(Status::MemoryFault);
        comp[i] = as_float(v);
      }
    }
    for (uint32_t i = 0; i < count; ++i) set_vgpr_f(vdata + i, lane, comp[i]);
  }
  return Status::Running;
}

Status Wave::exec_mimg(uint32_t w0, uint32_t w1, IEnvironment& env) {
  const uint32_t op = (w0 >> 18) & 0x7F, dmask = (w0 >> 8) & 0xF;
  const uint32_t vaddr = w1 & 0xFF, vdata = (w1 >> 8) & 0xFF, srsrc = ((w1 >> 16) & 0x1F) * 4, ssamp = ((w1 >> 21) & 0x1F) * 4;
  const Mimg o = static_cast<Mimg>(op);
  uint32_t coords = 0;
  switch (o) {
    case Mimg::image_sample: case Mimg::image_sample_lz: case Mimg::image_load: coords = 2; break;       // 2D images
    case Mimg::image_sample_l: case Mimg::image_sample_b: coords = 3; break;
    default: return fail(Status::BadInstruction);
  }
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    if (!(exec >> lane & 1)) continue;
    float c[4] = {0, 0, 0, 0}, result[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < coords; ++i) c[i] = vgpr_f(vaddr + i, lane);
    env.sample(o, &sgpr[srsrc], &sgpr[ssamp], c, coords, result);
    uint32_t out = 0;
    for (uint32_t i = 0; i < 4; ++i)
      if (dmask >> i & 1) set_vgpr_f(vdata + out++, lane, result[i]);
  }
  return Status::Running;
}

Status Wave::step(ArrayView<const uint32_t> code, IEnvironment& env) {
  if (status != Status::Running) return status;
  if (pc >= code.size()) return fail(Status::BadBranch);
  const uint32_t w0 = code[pc];
  const uint32_t length = instruction_length(code.subview(pc));
  if (length == 0 || pc + length > code.size()) return fail(Status::BadInstruction);
  const uint32_t w1 = length > 1 ? code[pc + 1] : 0;
  Status s = Status::Running;
  if ((w0 & 0xFF800000u) == 0xBE800000u) s = exec_sop1(w0, w1);
  else if ((w0 & 0xFF800000u) == 0xBF000000u) s = exec_sopc(w0, w1);
  else if ((w0 & 0xFF800000u) == 0xBF800000u) s = exec_sopp(w0);
  else if ((w0 & 0xF0000000u) == 0xB0000000u) s = exec_sopk(w0);
  else if ((w0 & 0xC0000000u) == 0x80000000u) s = exec_sop2(w0, w1);
  else if ((w0 & 0xFC000000u) == 0xC0000000u) s = exec_smem(w0, w1, env);
  else if ((w0 & 0xFC000000u) == 0xC4000000u) s = exec_exp(w0, w1, env);
  else if ((w0 & 0xFC000000u) == 0xD4000000u) s = exec_vintrp(w0, env);
  else if ((w0 & 0xFC000000u) == 0xE0000000u) s = exec_mubuf(w0, w1, env);
  else if ((w0 & 0xFC000000u) == 0xF0000000u) s = exec_mimg(w0, w1, env);
  else s = exec_valu(w0, w1, w1);
  ++steps;
  if (s == Status::Running) pc += length;
  return s;
}

Status Wave::run(ArrayView<const uint32_t> code, IEnvironment& env, uint64_t max_steps) {
  while (status == Status::Running) {
    if (steps >= max_steps) return fail(Status::StepLimit);
    step(code, env);
  }
  return status;
}

}  // namespace gcn
