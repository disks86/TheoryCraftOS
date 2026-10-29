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

// MMX: the 64-bit integer vector instructions (no prefix on the SSE2 integer opcodes), their SSE additions (pshufw, pextrw, pinsrw, pmovmskb, movntq, maskmovq), the SSSE3 forms, the MMX / SSE
// conversions (cvtpi2ps, cvtps2pi...), movq2dq / movdq2q and emms. The registers MM0..MM7 are the significands of the x87 registers: any MMX instruction makes every x87 register valid and TOP
// zero, and a written register gets an all-ones sign and exponent. The operations themselves are the 128-bit SSE integer ones (X86_64Cpu::sse_integer_core) on values held in the low half.
#include "Emulation/SoftFloat.hpp"
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

using softfloat::Env;
using softfloat::Format;
using softfloat::Rounding;

namespace {
int64_t sext(uint64_t v, uint32_t width) { return width == 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - width)) >> (64 - width); }
uint64_t lane(uint64_t v, uint32_t width, uint32_t i) { return (v >> (width * i)) & (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1); }
}  // namespace

bool X86_64Cpu::mmx_opcode(uint8_t op) const {
  const bool plain = !d_.operand16 && !d_.rep && !d_.repne;
  if (op == 0xD6) return (d_.rep || d_.repne) && !d_.operand16;      // movq2dq, movdq2q
  if (op == 0x2A || op == 0x2C || op == 0x2D) return !d_.rep && !d_.repne;      // cvtpi2ps / cvtpi2pd and the conversions to MMX registers
  if (!plain) return false;
  switch (op) {
    case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x77: case 0x7E: case 0x7F:
    case 0xC4: case 0xC5: case 0xD7: case 0xE7: case 0xF7:
      return true;
    case 0x54: case 0x55: case 0x56: case 0x57: case 0x6C: case 0x6D:      // (SSE float logic, and punpck?qdq, have no MMX form)
      return false;
    default:
      return sse_integer_valid(op);
  }
}

void X86_64Cpu::mmx_enter() {
  s_.fpu_valid = 0xFF;
  s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~0x3800);      // TOP = 0
}

uint64_t X86_64Cpu::mmx_get(uint32_t index) const { return s_.st[index & 7].mant; }

void X86_64Cpu::mmx_set(uint32_t index, uint64_t value) {
  s_.st[index & 7].mant = value;
  s_.st[index & 7].se = 0xFFFF;
}

X86_64Cpu::Step X86_64Cpu::mmx_instruction(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (op == 0x77) {      // emms: the x87 registers are empty again
    if (!finish_decode()) return bad_decode();
    s_.fpu_valid = 0;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (!decode_modrm()) return bad_decode();
  uint32_t imm = 0;
  if (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xC4 || op == 0xC5) imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  if (op == 0x2A || op == 0x2C || op == 0x2D) return mmx_convert(m, op);
  const uint32_t reg = d_.reg & 7, rm = d_.rm & 7;
  const bool memory = d_.mod != 3;
  auto done = [&]() {
    s_.rip += d_.position;
    return Step::Ok;
  };
  uint64_t source = 0;      // the 64-bit r/m operand (an MMX register or memory)
  auto fetch = [&]() {
    if (memory) return load(m, memory_address(), 8, &source);
    source = mmx_get(rm);
    return true;
  };
  switch (op) {
    case 0x6E: {      // movd / movq mm, r/m32/64
      const uint32_t size = d_.rex_w ? 8 : 4;
      uint64_t v = 0;
      if (!read_rm(m, size, &v)) return Step::Fault;
      mmx_enter();
      mmx_set(reg, v);
      return done();
    }
    case 0x7E: {      // movd / movq r/m32/64, mm
      const uint32_t size = d_.rex_w ? 8 : 4;
      mmx_enter();
      if (!write_rm(m, size, mmx_get(reg) & (size == 8 ? ~uint64_t{0} : 0xFFFFFFFFu))) return Step::Fault;
      return done();
    }
    case 0x6F:      // movq mm, mm/m64
      if (!fetch()) return Step::Fault;
      mmx_enter();
      mmx_set(reg, source);
      return done();
    case 0x7F:      // movq mm/m64, mm
      mmx_enter();
      if (memory) {
        if (!store(m, memory_address(), 8, mmx_get(reg))) return Step::Fault;
      } else {
        mmx_set(rm, mmx_get(reg));
      }
      return done();
    case 0xE7:      // movntq m64, mm
      if (!memory) return fail(6);
      mmx_enter();
      if (!store(m, memory_address(), 8, mmx_get(reg))) return Step::Fault;
      return done();
    case 0xF7: {      // maskmovq mm, mm: the bytes of the first register whose mask byte (second register) has the top bit set go to [rdi]
      if (memory) return fail(6);
      mmx_enter();
      const uint64_t data = mmx_get(reg), mask = mmx_get(rm);
      const uint64_t base = (d_.segment == 1 ? s_.fs_base : d_.segment == 2 ? s_.gs_base : 0) + s_.r[7];
      for (uint32_t i = 0; i < 8; ++i)
        if ((mask >> (8 * i + 7)) & 1)
          if (!store(m, base + i, 1, (data >> (8 * i)) & 0xFF)) return Step::Fault;
      return done();
    }
    case 0xD7: {      // pmovmskb r32, mm
      if (memory) return fail(6);
      mmx_enter();
      uint32_t bits = 0;
      for (uint32_t i = 0; i < 8; ++i) bits |= static_cast<uint32_t>((mmx_get(rm) >> (8 * i + 7)) & 1) << i;
      set_reg(d_.reg, 4, bits);
      return done();
    }
    case 0xC5: {      // pextrw r32, mm, imm8
      if (memory) return fail(6);
      mmx_enter();
      set_reg(d_.reg, 4, lane(mmx_get(rm), 16, imm & 3));
      return done();
    }
    case 0xC4: {      // pinsrw mm, r32/m16, imm8
      uint64_t v = 0;
      if (!read_rm(m, memory ? 2 : 4, &v)) return Step::Fault;
      mmx_enter();
      const uint32_t shift = 16 * (imm & 3);
      mmx_set(reg, (mmx_get(reg) & ~(uint64_t{0xFFFF} << shift)) | ((v & 0xFFFF) << shift));
      return done();
    }
    case 0x70: {      // pshufw mm, mm/m64, imm8
      if (!fetch()) return Step::Fault;
      mmx_enter();
      uint64_t out = 0;
      for (uint32_t i = 0; i < 4; ++i) out |= lane(source, 16, (imm >> (2 * i)) & 3) << (16 * i);
      mmx_set(reg, out);
      return done();
    }
    case 0x71: case 0x72: case 0x73: {      // shifts of an MMX register by an immediate
      if (memory) return fail(6);
      const uint32_t kind = d_.reg & 7, width = op == 0x71 ? 16 : op == 0x72 ? 32 : 64;
      const bool left = kind == 6, arithmetic = kind == 4;
      if ((kind != 2 && kind != 4 && kind != 6) || (arithmetic && width == 64)) return fail(6);
      mmx_enter();
      const uint64_t value = mmx_get(rm);
      uint64_t out = 0;
      for (uint32_t i = 0; i < 64 / width; ++i) {
        const uint64_t v = lane(value, width, i);
        uint64_t r;
        if (arithmetic) r = static_cast<uint64_t>(sext(v, width) >> (imm > width - 1 ? width - 1 : imm)) & ((uint64_t{1} << width) - 1);
        else if (imm >= width) r = 0;
        else r = left ? (v << imm) : (v >> imm);
        out |= (width == 64 ? r : (r & ((uint64_t{1} << width) - 1))) << (width * i);
      }
      mmx_set(rm, out);
      return done();
    }
    case 0xD6: {      // movq2dq xmm, mm (F3) / movdq2q mm, xmm (F2): register forms only
      if (memory) return fail(6);
      mmx_enter();
      if (d_.rep) s_.xmm[d_.reg] = Xmm{mmx_get(rm), 0};
      else mmx_set(reg, s_.xmm[d_.rm].lo);
      return done();
    }
    default: break;
  }
  // The generic integer operations: the SSE core on 64-bit values.
  if (!fetch()) return Step::Fault;
  Xmm dst{mmx_get(reg), 0}, src{source, 0};
  uint8_t core_op = op;
  if (op == 0x68 || op == 0x69 || op == 0x6A) {      // punpckh: the same as punpckl of the upper halves
    dst.lo >>= 32;
    src.lo >>= 32;
    core_op = static_cast<uint8_t>(op - 8);
  } else if (op == 0x63 || op == 0x67 || op == 0x6B) {      // packs: the first source's lanes fill the low half, the second's the high half
    dst.hi = source;
  }
  if (!sse_integer_core(core_op, dst, src)) return fail(6);
  mmx_enter();
  mmx_set(reg, dst.lo);
  return done();
}

// The SSSE3 forms on MMX registers: 0F 38 00..0B and 1C..1E, 0F 3A 0F, without a prefix.
bool X86_64Cpu::mmx_ssse3_opcode(uint32_t map, uint8_t op) const {
  if (d_.operand16 || d_.rep || d_.repne) return false;
  return map == 2 ? (op <= 0x0B || (op >= 0x1C && op <= 0x1E)) : op == 0x0F;
}

X86_64Cpu::Step X86_64Cpu::mmx_ssse3(GuestMemory& m, uint32_t map, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm()) return bad_decode();
  uint32_t imm = 0;
  if (map == 3) imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  uint64_t source = 0;
  if (d_.mod != 3) {
    if (!load(m, memory_address(), 8, &source)) return Step::Fault;
  } else {
    source = mmx_get(d_.rm);
  }
  const uint32_t reg = d_.reg & 7;
  const uint64_t d = mmx_get(reg);
  auto sat16 = [](int64_t v) { return static_cast<uint64_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v) & 0xFFFF; };
  uint64_t out = 0;
  auto put = [&](uint32_t width, uint32_t i, uint64_t v) { out |= (v & (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1)) << (width * i); };
  if (map == 3) {      // palignr: (dst:src) >> (8 * imm) bytes
    const unsigned __int128 whole = (static_cast<unsigned __int128>(d) << 64) | source;
    out = imm >= 16 ? 0 : static_cast<uint64_t>(whole >> (8 * imm));
  } else {
    switch (op) {
      case 0x00:      // pshufb
        for (uint32_t i = 0; i < 8; ++i) {
          const uint64_t mask = lane(source, 8, i);
          put(8, i, (mask & 0x80) ? 0 : lane(d, 8, mask & 7));
        }
        break;
      case 0x01: case 0x05:      // phaddw, phsubw
        for (uint32_t i = 0; i < 4; ++i) {
          const uint64_t from = i < 2 ? d : source;
          const int64_t a = sext(lane(from, 16, 2 * (i & 1)), 16), b = sext(lane(from, 16, 2 * (i & 1) + 1), 16);
          put(16, i, static_cast<uint64_t>(op == 0x01 ? a + b : a - b));
        }
        break;
      case 0x03: case 0x07:      // phaddsw, phsubsw
        for (uint32_t i = 0; i < 4; ++i) {
          const uint64_t from = i < 2 ? d : source;
          const int64_t a = sext(lane(from, 16, 2 * (i & 1)), 16), b = sext(lane(from, 16, 2 * (i & 1) + 1), 16);
          put(16, i, sat16(op == 0x03 ? a + b : a - b));
        }
        break;
      case 0x02: case 0x06:      // phaddd, phsubd
        for (uint32_t i = 0; i < 2; ++i) {
          const uint64_t from = i == 0 ? d : source;
          const int64_t a = sext(lane(from, 32, 0), 32), b = sext(lane(from, 32, 1), 32);
          put(32, i, static_cast<uint64_t>(op == 0x02 ? a + b : a - b));
        }
        break;
      case 0x04:      // pmaddubsw: unsigned bytes of dst times signed bytes of src, pairs added with saturation
        for (uint32_t i = 0; i < 4; ++i) {
          const int64_t p = static_cast<int64_t>(lane(d, 8, 2 * i)) * sext(lane(source, 8, 2 * i), 8) + static_cast<int64_t>(lane(d, 8, 2 * i + 1)) * sext(lane(source, 8, 2 * i + 1), 8);
          put(16, i, sat16(p));
        }
        break;
      case 0x08: case 0x09: case 0x0A: {      // psignb / w / d: the sign of src applied to dst
        const uint32_t width = op == 0x08 ? 8 : op == 0x09 ? 16 : 32;
        for (uint32_t i = 0; i < 64 / width; ++i) {
          const int64_t s2 = sext(lane(source, width, i), width), v = sext(lane(d, width, i), width);
          put(width, i, s2 < 0 ? static_cast<uint64_t>(-v) : s2 == 0 ? 0 : static_cast<uint64_t>(v));
        }
        break;
      }
      case 0x0B:      // pmulhrsw
        for (uint32_t i = 0; i < 4; ++i) put(16, i, static_cast<uint64_t>((((sext(lane(d, 16, i), 16) * sext(lane(source, 16, i), 16)) >> 14) + 1) >> 1));
        break;
      default: {      // 1C..1E pabsb / w / d: the absolute value of src
        const uint32_t width = op == 0x1C ? 8 : op == 0x1D ? 16 : 32;
        for (uint32_t i = 0; i < 64 / width; ++i) {
          const int64_t v = sext(lane(source, width, i), width);
          put(width, i, v < 0 ? uint64_t{0} - static_cast<uint64_t>(v) : static_cast<uint64_t>(v));
        }
        break;
      }
    }
  }
  mmx_enter();
  mmx_set(reg, out);
  s_.rip += d_.position;
  return Step::Ok;
}

X86_64Cpu::Step X86_64Cpu::mmx_convert(GuestMemory& m, uint8_t op) {
  // cvtpi2ps / cvtpi2pd xmm, mm/m64 (0F 2A); cvttps2pi / cvtps2pi mm, xmm/m64 (0F 2C / 2D); with 66: cvttpd2pi / cvtpd2pi mm, xmm/m128
  const bool pd = d_.operand16, memory = d_.mod != 3;
  const uint32_t reg = d_.reg, rm = d_.rm;
  Env env;
  env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  env.daz = (s_.mxcsr & 0x40) != 0;
  env.ftz = (s_.mxcsr & 0x8000) != 0;
  if (op == 0x2A) {
    uint64_t source = 0;
    if (memory) {
      if (!load(m, memory_address(), 8, &source)) return Step::Fault;
    } else {
      source = mmx_get(rm);
    }
    mmx_enter();
    const int64_t a = static_cast<int32_t>(source), b = static_cast<int32_t>(source >> 32);
    Xmm& dst = s_.xmm[reg];
    if (pd) {
      dst.lo = softfloat::from_int(Format::F64, a, env);
      dst.hi = softfloat::from_int(Format::F64, b, env);
    } else {
      dst.lo = softfloat::from_int(Format::F32, a, env) | (softfloat::from_int(Format::F32, b, env) << 32);
    }
  } else {
    Xmm source;
    if (memory) {
      if (!load(m, memory_address(), 8, &source.lo)) return Step::Fault;
      if (pd && !load(m, memory_address() + 8, 8, &source.hi)) return Step::Fault;
    } else {
      source = s_.xmm[rm];
    }
    mmx_enter();
    const bool truncate = op == 0x2C;
    uint64_t out;
    if (pd) {
      const uint64_t a = static_cast<uint32_t>(softfloat::to_int(Format::F64, source.lo, 32, truncate, env));
      const uint64_t b = static_cast<uint32_t>(softfloat::to_int(Format::F64, source.hi, 32, truncate, env));
      out = a | (b << 32);
    } else {
      const uint64_t a = static_cast<uint32_t>(softfloat::to_int(Format::F32, source.lo & 0xFFFFFFFFu, 32, truncate, env));
      const uint64_t b = static_cast<uint32_t>(softfloat::to_int(Format::F32, source.lo >> 32, 32, truncate, env));
      out = a | (b << 32);
    }
    mmx_set(reg, out);
    env.flags &= ~softfloat::kDenormal;
  }
  s_.mxcsr |= env.flags;
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
