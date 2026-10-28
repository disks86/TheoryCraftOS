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

// Pixel shaders of Shader Model 1 (ps_1_0 .. ps_1_4), decoded into the instruction form D3d9Program runs.
//
// Their model is the fixed-function texture stage: the result is r0 (no output register), a texture stage sampler and a texture coordinate set are named by the same number, and
// the texture instructions of 1.0 - 1.3 (`tex t0`) turn the texture register into the sampled colour. Mapping: t0 / t1 are the texture coordinates 0 / 1 (the varyings TEXCOORD0 /
// TEXCOORD1) until an instruction writes them, then temporaries holding the sample; v0 / v1 the colours; c0..c7 the push constants and `def`s the shader's; the program ends with
// the move of r0 to the colour output. cnd is a comparison with one half and a select. Stage 0 and 1 only (the device has two texture units and two coordinate sets).
//
// Taken: mov, add, sub, mad, mul, dp3, dp4, lrp, cnd, cmp, def, nop, tex / texcoord / texkill (1.0 - 1.3), texld / texcrd with a temporary or texture-register coordinate (1.4,
// _dw / _dz projection included), phase (a marker), the result modifiers (_x2 .. _d8, _sat), the source modifiers, coissue. Not taken (kNotSupported): texbem, texreg2*, texm3x*,
// texdp3*, texdepth, bem, and stages above 1. The fixed point ranges of the model (registers clamped to [-1, 1]) are not imitated: values are float.
#include "Kernel/Dxgkrnl/D3d9Decoded.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace dxgk {

namespace {

constexpr uint16_t kTexCoord = 64, kTexKillOp = 65, kTexOp = 66, kCndOp = 80, kPhase = 0xFFFD;
constexpr uint32_t kTextureTempBase = 8;   // t0 / t1 as temporaries (r0..r5 are the program's)
constexpr uint32_t kScratchTemp = 20;

uint8_t reg_type_of(uint32_t token) { return static_cast<uint8_t>(((token >> 28) & 7) | ((token >> 8) & 0x18)); }

}  // namespace

NtStatus D3d9Program::compile_ps1(allocators::IAllocator& allocator, ConstByteView bytes, UniquePointer<D3d9Program>* out) {
  using Instr = Instruction;
  UniquePointer<D3d9Program> program = UniquePointer<D3d9Program>::make(allocator, allocator);
  if (!program) return status::kNoMemory;
  ByteReader in(bytes);
  const uint32_t version = in.le32();
  const uint32_t minor = version & 0xFF;
  if ((version >> 16) != 0xFFFF || ((version >> 8) & 0xFF) != 1 || minor > 4) return status::kNotSupported;
  program->stage_ = graphics::ShaderStage::Fragment;
  program->major_ = 1;
  program->minor_ = minor;
  const bool v14 = minor == 4;

  bool texture_written[2] = {false, false};   // t0 / t1 hold a sample now
  bool ended = false;

  auto literal = [&](float value, uint16_t* number) -> NtStatus {
    for (const Def& d : program->defs_.view())
      if (d.number >= 2000 && d.value[0] == value && d.value[1] == value && d.value[2] == value && d.value[3] == value) {
        *number = d.number;
        return status::kSuccess;
      }
    if (program->defs_.count() >= 256) return status::kNotSupported;
    *number = static_cast<uint16_t>(2000 + program->defs_.count());
    return program->defs_.push_back({*number, {value, value, value, value}}) ? status::kSuccess : status::kNoMemory;
  };

  auto read_src = [&](ByteReader& p, Instr::Src* s) -> NtStatus {
    const uint32_t t = p.le32();
    if (!p.ok()) return status::kInvalidParameter;
    s->number = static_cast<uint16_t>(t & 0x7FF);
    for (uint32_t i = 0; i < 4; ++i) s->swizzle[i] = static_cast<uint8_t>((t >> (16 + 2 * i)) & 3);
    s->modifier = static_cast<uint8_t>((t >> 24) & 0xF);
    if (s->modifier > kModX2Neg && s->modifier != kModDz && s->modifier != kModDw) return status::kNotSupported;
    if ((t >> 13) & 1) return status::kNotSupported;   // relative addressing
    switch (reg_type_of(t)) {
      case kTemp:
        if (s->number >= (v14 ? 6u : 2u)) return status::kInvalidParameter;
        s->type = kTemp;
        return status::kSuccess;
      case kInput:
        if (s->number >= 2) return status::kInvalidParameter;
        s->type = kInput, s->slot = static_cast<int8_t>(s->number);
        return status::kSuccess;
      case kConst:
        s->type = kConst;
        return status::kSuccess;   // (checked against the defs at the end)
      case kAddrTex:   // t#
        if (s->number >= 2) return status::kNotSupported;
        if (texture_written[s->number]) {
          s->type = kTemp, s->number = static_cast<uint16_t>(kTextureTempBase + s->number);
        } else {
          s->type = kAddrTex, s->slot = static_cast<int8_t>(2 + s->number);
        }
        return status::kSuccess;
      default: return status::kNotSupported;
    }
  };

  auto read_dst = [&](ByteReader& p, Instr::Dst* d, uint32_t* raw_type, uint32_t* raw_number) -> NtStatus {
    const uint32_t t = p.le32();
    if (!p.ok()) return status::kInvalidParameter;
    d->number = static_cast<uint16_t>(t & 0x7FF);
    d->mask = static_cast<uint8_t>((t >> 16) & 0xF);
    d->saturate = (((t >> 20) & 0xF) & 1) != 0;
    int32_t shift = static_cast<int32_t>((t >> 24) & 0xF);
    if (shift >= 8) shift -= 16;
    d->shift = static_cast<int8_t>(shift);
    *raw_type = reg_type_of(t);
    *raw_number = t & 0x7FF;
    if (*raw_type == kTemp) {
      if (d->number >= (v14 ? 6u : 2u)) return status::kInvalidParameter;
      d->type = kTemp;
      return status::kSuccess;
    }
    return status::kSuccess;   // (a texture register: the texture instructions deal with it)
  };

  auto emit = [&](Instr& ins) -> NtStatus {
    if (program->code_.count() >= kMaxInstructions) return status::kNotSupported;
    return program->code_.push_back(ins) ? status::kSuccess : status::kNoMemory;
  };

  while (!in.at_end() && !ended) {
    const uint32_t token = in.le32();
    if (!in.ok()) return status::kInvalidParameter;
    if (token == 0x0000FFFF) {
      ended = true;
      break;
    }
    const uint16_t op = static_cast<uint16_t>(token & 0xFFFF);   // (bit 30 is coissue: the pair runs one after the other here)
    if (op == kComment) {
      if (!in.skip(uint64_t{(token >> 16) & 0x7FFF} * 4)) return status::kInvalidParameter;
      continue;
    }
    if (op == kPhase) continue;
    if (op == kDef) {
      const uint32_t reg = in.le32();
      float v[4];
      for (float& f : v) f = __builtin_bit_cast(float, in.le32());
      if (!in.ok() || reg_type_of(reg) != kConst) return status::kNotSupported;
      if (program->defs_.count() >= 256 || !program->defs_.push_back({static_cast<uint16_t>(reg & 0x7FF), {v[0], v[1], v[2], v[3]}})) return status::kNoMemory;
      continue;
    }
    if (op == kNop) continue;
    Instr ins;
    ins.op = op;
    uint32_t raw_type = 0, raw_number = 0;
    NtStatus s = status::kSuccess;
    switch (op) {
      case kMov: case kAdd: case kSub: case kMul: case kMad: case kDp3: case kDp4: case kLrp: case kCmp: case kCndOp: {
        static const struct {
          uint16_t op;
          uint8_t sources;
        } kArith[] = {{kMov, 1}, {kAdd, 2}, {kSub, 2}, {kMul, 2}, {kMad, 3}, {kDp3, 2}, {kDp4, 2}, {kLrp, 3}, {kCmp, 3}, {kCndOp, 3}};
        uint32_t sources = 0;
        for (const auto& a : kArith)
          if (a.op == op) sources = a.sources;
        s = read_dst(in, &ins.dst, &raw_type, &raw_number);
        if (s != status::kSuccess) return s;
        if (raw_type != kTemp) return status::kNotSupported;   // (the result goes to a temporary; r0 is the colour)
        for (uint32_t i = 0; i < sources; ++i) {
          s = read_src(in, &ins.src[i]);
          if (s != status::kSuccess) return s;
          if (ins.src[i].modifier == kModDz || ins.src[i].modifier == kModDw) return status::kNotSupported;
        }
        ins.src_count = static_cast<uint8_t>(sources);
        if (op == kCndOp) {
          // cnd d, a, b, c: per component, a > 0.5 ? b : c  =  select(0.5 < a, b, c)
          Instr compare;
          compare.op = kSlt;
          compare.dst.type = kTemp, compare.dst.number = kScratchTemp;
          uint16_t half = 0;
          s = literal(0.5f, &half);
          if (s != status::kSuccess) return s;
          compare.src[0].type = kConst, compare.src[0].number = half;
          compare.src[1] = ins.src[0];
          compare.src_count = 2;
          s = emit(compare);
          if (s != status::kSuccess) return s;
          ins.op = kSelect;
          ins.src[0] = Instr::Src();
          ins.src[0].type = kTemp, ins.src[0].number = kScratchTemp;
          // (the select reads the mask; b and c are the original second and third sources)
        }
        s = emit(ins);
        if (s != status::kSuccess) return s;
        break;
      }
      case kTexCoord: {   // 1.0 - 1.3 `texcoord tN`; 1.4 `texcrd rN, tM`
        s = read_dst(in, &ins.dst, &raw_type, &raw_number);
        if (s != status::kSuccess) return s;
        uint16_t one = 0;
        s = literal(1.0f, &one);
        if (s != status::kSuccess) return s;
        Instr copy;
        copy.op = kMov;
        Instr::Src coordinate;
        if (v14) {
          if (raw_type != kTemp) return status::kNotSupported;
          s = read_src(in, &coordinate);
          if (s != status::kSuccess) return s;
          copy.dst = ins.dst;
        } else {
          if (raw_type != kAddrTex || raw_number >= 2) return status::kNotSupported;
          coordinate.type = kAddrTex, coordinate.slot = static_cast<int8_t>(2 + raw_number);
          copy.dst.type = kTemp, copy.dst.number = static_cast<uint16_t>(kTextureTempBase + raw_number);
          texture_written[raw_number] = true;
        }
        copy.dst.mask = 0x7;
        copy.src[0] = coordinate;
        copy.src_count = 1;
        s = emit(copy);
        if (s != status::kSuccess) return s;
        Instr w;   // (w is 1)
        w.op = kMov;
        w.dst = copy.dst;
        w.dst.mask = 0x8;
        w.src[0].type = kConst, w.src[0].number = one;
        w.src_count = 1;
        s = emit(w);
        if (s != status::kSuccess) return s;
        break;
      }
      case kTexKillOp: {
        const uint32_t t = in.le32();
        if (!in.ok()) return status::kInvalidParameter;
        const uint32_t type = reg_type_of(t), number = t & 0x7FF;
        if (type == kTemp && number < (v14 ? 6u : 2u)) {
          ins.src[0].type = kTemp, ins.src[0].number = static_cast<uint16_t>(number);
        } else if (type == kAddrTex && number < 2) {
          if (texture_written[number]) ins.src[0].type = kTemp, ins.src[0].number = static_cast<uint16_t>(kTextureTempBase + number);
          else ins.src[0].type = kAddrTex, ins.src[0].slot = static_cast<int8_t>(2 + number);
        } else {
          return status::kNotSupported;
        }
        ins.src_count = 1;
        s = emit(ins);
        if (s != status::kSuccess) return s;
        break;
      }
      case kTexOp: {   // 1.0 - 1.3 `tex tN`; 1.4 `texld rN, tM`
        s = read_dst(in, &ins.dst, &raw_type, &raw_number);
        if (s != status::kSuccess) return s;
        ins.op = kTexLd;
        uint32_t stage = 0;
        if (v14) {
          if (raw_type != kTemp || raw_number >= graphics::kMaxTextureUnits) return status::kNotSupported;
          stage = raw_number;
          s = read_src(in, &ins.src[0]);
          if (s != status::kSuccess) return s;
          if (ins.src[0].modifier == kModDz || ins.src[0].modifier == kModDw) {
            ins.control = 1;   // projective: the divisor is the last component of the swizzle
            ins.src[0].modifier = kModNone;
          }
        } else {
          if (raw_type != kAddrTex || raw_number >= graphics::kMaxTextureUnits) return status::kNotSupported;
          stage = raw_number;
          ins.src[0].type = kAddrTex, ins.src[0].slot = static_cast<int8_t>(2 + stage);
          ins.dst.type = kTemp, ins.dst.number = static_cast<uint16_t>(kTextureTempBase + stage);
          ins.dst.mask = 0xF, ins.dst.saturate = false, ins.dst.shift = 0;
          texture_written[stage] = true;
        }
        ins.src[1].type = kSampler, ins.src[1].number = static_cast<uint16_t>(stage);
        ins.src_count = 2;
        program->uses_textures_ = true;
        s = emit(ins);
        if (s != status::kSuccess) return s;
        break;
      }
      default: return status::kNotSupported;
    }
  }
  if (!ended) return status::kInvalidParameter;

  // The constants read must exist; then the result.
  for (const Instr& ins : program->code_.view())
    for (uint32_t k = 0; k < ins.src_count; ++k) {
      const Instr::Src& src = ins.src[k];
      if (src.type != kConst) continue;
      bool defined = src.number < kPushConstantRegisters;
      for (const Def& d : program->defs_.view())
        if (d.number == src.number) defined = true;
      if (!defined) return status::kNotSupported;
    }
  Instr result;
  result.op = kMov;
  result.dst.type = kColorOut, result.dst.slot = 0;
  result.src[0].type = kTemp, result.src[0].number = 0;
  result.src_count = 1;
  const NtStatus s = emit(result);
  if (s != status::kSuccess) return s;
  *out = util::move(program);
  return status::kSuccess;
}

}  // namespace dxgk
