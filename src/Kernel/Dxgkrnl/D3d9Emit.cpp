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

// A program made from DXBC (D3d9Dxbc.cpp) written out as a Direct3D 9 token stream - vs_2_0 / ps_2_0, or vs_3_0 / ps_3_0 when it has an if on a register - in the contract of
// D3d9Shader.hpp, so that it can take the way a Direct3D 9 shader takes to the SVGA3D device (emit_svga_tokens).
//
// The instructions Direct3D 9 lacks are built from the ones it has, with the program's scratch temporaries after its own: div (a reciprocal per component and a multiply), sqrt
// (rsq, rcp), seq / sne (two sge and a multiply), floor / ceil / trunc / round (frc and arithmetic), movc (cmp in a pixel shader, two masks in a vertex shader, which has no
// cmp), dp2 (multiply and add: dp2add is a pixel shader instruction), discard (texkill of a negative), sin / cos (sincos with its two constants in Shader Model 2). Literals,
// zero / one / one half and the sincos constants become `def`s after c7. sqrt of a negative gives 0 here (the interpreter's NaN is not expressible), and the vertex shader's movc
// is not exact for infinities.
#include "Kernel/Dxgkrnl/D3d9Decoded.hpp"

namespace dxgk {

class D3d9Writer {
 public:
  using Instr = D3d9Program::Instruction;

  struct S {
    uint8_t type = kTemp;
    uint16_t number = 0;
    uint8_t swizzle[4] = {0, 1, 2, 3};
    uint8_t modifier = kModNone;
  };
  struct D {
    uint8_t type = kTemp;
    uint16_t number = 0;
    uint8_t mask = 0xF;
    bool saturate = false;
  };

  D3d9Writer(const D3d9Program& program, allocators::IAllocator& allocator) : p_(program), body_(allocator), defs_(allocator), consts_(allocator) {}

  NtStatus run(List<uint32_t>* out);

 private:
  static constexpr uint32_t kFirstConstant = 8, kLastConstant = 29;   // (c31 is the device's)

  static uint32_t type_bits(uint32_t type) { return ((type & 7) << 28) | ((type & 0x18) << 8); }
  static uint32_t source_token(const S& s) {
    return 0x80000000u | type_bits(s.type) | s.number | (static_cast<uint32_t>(s.swizzle[0] | (s.swizzle[1] << 2) | (s.swizzle[2] << 4) | (s.swizzle[3] << 6)) << 16) |
           (static_cast<uint32_t>(s.modifier) << 24);
  }
  static uint32_t destination_token(const D& d) { return 0x80000000u | type_bits(d.type) | d.number | (static_cast<uint32_t>(d.mask) << 16) | (d.saturate ? 1u << 20 : 0u); }

  void push(List<uint32_t>& to, uint32_t word) {
    if (!to.push_back(word)) oom_ = true;
  }
  void emit(uint32_t op, const D* d, const S* a = nullptr, const S* b = nullptr, const S* c = nullptr, uint32_t control = 0) {
    const uint32_t params = (d ? 1 : 0) + (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0);
    push(body_, op | (control << 16) | (params << 24));
    if (d) push(body_, destination_token(*d));
    if (a) push(body_, source_token(*a));
    if (b) push(body_, source_token(*b));
    if (c) push(body_, source_token(*c));
  }

  // --- constants
  S constant(const float v[4], const uint8_t swizzle[4] = nullptr) {
    S s;
    s.type = kConst;
    uint64_t at = 0;
    for (; at < consts_.count(); ++at) {
      bool same = true;
      for (uint32_t k = 0; k < 4; ++k) same = same && __builtin_bit_cast(uint32_t, consts_[at].v[k]) == __builtin_bit_cast(uint32_t, v[k]);
      if (same) break;
    }
    if (at == consts_.count()) {
      Const c = {{v[0], v[1], v[2], v[3]}};
      if (!consts_.push_back(c)) oom_ = true;
    }
    if (kFirstConstant + at > kLastConstant) unsupported_ = true;
    s.number = static_cast<uint16_t>(kFirstConstant + at);
    if (swizzle)
      for (uint32_t k = 0; k < 4; ++k) s.swizzle[k] = swizzle[k];
    return s;
  }
  S splat(float x) {
    const float v[4] = {x, x, x, x};
    return constant(v);
  }

  // --- operands
  S scratch(uint32_t k) {
    S s;
    s.type = kTemp;
    s.number = static_cast<uint16_t>(temps_ + k);
    if (k + 1 > scratch_used_) scratch_used_ = k + 1;
    return s;
  }
  static D to_d(const S& s, uint8_t mask) {
    D d;
    d.type = s.type, d.number = s.number, d.mask = mask;
    return d;
  }
  static S with_swizzle(S s, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
    s.swizzle[0] = x, s.swizzle[1] = y, s.swizzle[2] = z, s.swizzle[3] = w;
    return s;
  }
  static S rep(S s, uint8_t c) { return with_swizzle(s, c, c, c, c); }   // (the channel c of the register itself)
  static uint8_t negate_modifier(uint8_t m) { return m == kModNone ? kModNeg : m == kModNeg ? kModNone : m == kModAbs ? kModAbsNeg : kModAbs; }
  static S negated(S s) {
    s.modifier = negate_modifier(s.modifier);
    return s;
  }
  static S absolute(S s) {
    s.modifier = kModAbs;
    return s;
  }
  // The IR source as a Direct3D 9 one.
  S source(const Instr::Src& in);
  // Its channel `c` (as the source's own swizzle selects it) repeated.
  S channel(const Instr::Src& in, uint32_t c) {
    S s = source(in);
    return rep(s, s.swizzle[c & 3]);
  }
  D destination(const Instr::Dst& in);

  bool instruction(const Instr& ins);
  void scalar_op(uint32_t op, const Instr& ins);
  void select_mask(const S& a, bool nonzero, uint32_t into);   // into = 1 where a != 0 (or == 0)

  struct Const {
    float v[4];
  };

  const D3d9Program& p_;
  List<uint32_t> body_;
  List<uint32_t> defs_;
  List<Const> consts_;
  bool oom_ = false, unsupported_ = false;
  bool vertex_ = false, sm3_ = false;
  uint32_t temps_ = 0;          // the program's own temporaries
  uint32_t scratch_used_ = 0;
  bool inputs_[16] = {};
  bool outputs_[4] = {};        // varying slots written (vertex)
  bool samplers_[graphics::kMaxTextureUnits] = {};
  int32_t output_register_[4] = {-1, -1, -1, -1};   // Shader Model 3: the output register of a varying slot
};

D3d9Writer::S D3d9Writer::source(const Instr::Src& in) {
  S s;
  for (uint32_t k = 0; k < 4; ++k) s.swizzle[k] = in.swizzle[k];
  s.modifier = in.modifier;
  switch (in.type) {
    case kTemp:
      s.type = kTemp, s.number = in.number;
      break;
    case kInput:
    case kAddrTex:
      if (in.slot < 0 || in.slot >= 4) {
        unsupported_ = true;
        break;
      }
      inputs_[in.slot] = true;
      if (vertex_ || sm3_ || in.slot < 2) s.type = kInput, s.number = static_cast<uint16_t>(in.slot);
      else s.type = kAddrTex, s.number = static_cast<uint16_t>(in.slot - 2);
      break;
    case kConst: {
      if (in.number < D3d9Program::kPushConstantRegisters) {
        s.type = kConst, s.number = in.number;
        break;
      }
      bool found = false;
      for (const D3d9Program::Def& d : p_.defs_.view()) {
        if (d.number != in.number) continue;
        const S c = constant(d.value);
        s.type = c.type, s.number = c.number;
        found = true;
        break;
      }
      if (!found) unsupported_ = true;
      break;
    }
    default: unsupported_ = true; break;
  }
  return s;
}

D3d9Writer::D D3d9Writer::destination(const Instr::Dst& in) {
  D d;
  d.mask = in.mask;
  d.saturate = in.saturate;
  switch (in.type) {
    case kTemp:
      d.type = kTemp, d.number = in.number;
      break;
    case kRastOut:
      if (sm3_) d.type = kTexCrdOut, d.number = 0;
      else d.type = kRastOut, d.number = 0;
      break;
    case kAttrOut: case kTexCrdOut:
      if (in.slot < 0 || in.slot >= 4) {
        unsupported_ = true;
        break;
      }
      outputs_[in.slot] = true;
      if (sm3_) d.type = kTexCrdOut, d.number = static_cast<uint16_t>(1 + in.slot);   // (o0 is the position)
      else if (in.slot < 2) d.type = kAttrOut, d.number = static_cast<uint16_t>(in.slot);
      else d.type = kTexCrdOut, d.number = static_cast<uint16_t>(in.slot - 2);
      break;
    case kColorOut:
      d.type = kColorOut, d.number = 0;
      break;
    default: unsupported_ = true; break;
  }
  return d;
}

// scratch(into).x... = 1.0 where a is (non)zero: slt 0 < |a|, or sge 0 >= |a|.
void D3d9Writer::select_mask(const S& a, bool nonzero, uint32_t into) {
  const S zero = splat(0.0f);
  const D d = to_d(scratch(into), 0xF);
  const S magnitude = absolute(a);
  if (nonzero) emit(kSlt, &d, &zero, &magnitude);
  else emit(kSge, &d, &zero, &magnitude);
}

bool D3d9Writer::instruction(const Instr& ins) {
  const uint16_t op = ins.op;
  switch (op) {
    case kNop: return true;
    case kElse: emit(kElse, nullptr); return true;
    case kEndIf: emit(kEndIf, nullptr); return true;
    case kIfNz: case kIfZ: {
      const S a = channel(ins.src[0], 0), zero = splat(0.0f);
      emit(kIfc, nullptr, &a, &zero, nullptr, op == kIfNz ? 5u : 2u);   // if_ne / if_eq
      return true;
    }
    case kDiscardNz: case kDiscardZ: {
      select_mask(channel(ins.src[0], 0), op == kDiscardNz, 0);
      const D k = to_d(scratch(1), 0xF);
      const S mask = negated(scratch(0));
      emit(kMov, &k, &mask);
      const D t = to_d(scratch(1), 0xF);
      push(body_, kTexKill | (1u << 24));
      push(body_, destination_token(t));
      return true;
    }
    default: break;
  }
  if (ins.dst.mask == 0) return true;
  const D dst = destination(ins.dst);
  const bool pixel = !vertex_;
  switch (op) {
    case kMov: case kFrc: {
      const S a = source(ins.src[0]);
      emit(op, &dst, &a);
      return true;
    }
    case kAdd: case kMul: case kMin: case kMax: case kSlt: case kSge: case kDp3: case kDp4: {
      const S a = source(ins.src[0]), b = source(ins.src[1]);
      emit(op, &dst, &a, &b);
      return true;
    }
    case kMad: {
      const S a = source(ins.src[0]), b = source(ins.src[1]), c = source(ins.src[2]);
      emit(kMad, &dst, &a, &b, &c);
      return true;
    }
    case kDp2: {   // (dp2add is for pixel shaders only)
      const S a = source(ins.src[0]), b = source(ins.src[1]);
      const D t = to_d(scratch(0), 0x3);
      emit(kMul, &t, &a, &b);
      const S x = rep(scratch(0), 0), y = rep(scratch(0), 1);
      emit(kAdd, &dst, &x, &y);
      return true;
    }
    case kExp: case kLog: case kRsq: {
      if (!ins.vector) {
        const S a = channel(ins.src[0], 0);
        emit(op, &dst, &a);
        return true;
      }
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(ins.dst.mask & (1u << c))) continue;
        D one = dst;
        one.mask = static_cast<uint8_t>(1u << c);
        const S a = channel(ins.src[0], c);
        emit(op, &one, &a);
      }
      return true;
    }
    case kSqrt: {   // rcp(rsq(x)): 0 for 0
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(ins.dst.mask & (1u << c))) continue;
        const S a = channel(ins.src[0], c);
        D t = to_d(scratch(0), static_cast<uint8_t>(1u << c));
        emit(kRsq, &t, &a);
        D one = dst;
        one.mask = static_cast<uint8_t>(1u << c);
        const S r = rep(scratch(0), static_cast<uint8_t>(c));
        emit(kRcp, &one, &r);
      }
      return true;
    }
    case kDiv: {
      for (uint32_t c = 0; c < 4; ++c) {
        const S b = channel(ins.src[1], c);
        D t = to_d(scratch(0), static_cast<uint8_t>(1u << c));
        emit(kRcp, &t, &b);
      }
      const S a = source(ins.src[0]), r = scratch(0);
      emit(kMul, &dst, &a, &r);
      return true;
    }
    case kSeq: case kSne: {
      const S a = source(ins.src[0]), b = source(ins.src[1]);
      const D t0 = to_d(scratch(0), 0xF), t1 = to_d(scratch(1), 0xF);
      emit(kSge, &t0, &a, &b);
      emit(kSge, &t1, &b, &a);
      const S s0 = scratch(0), s1 = scratch(1);
      emit(kMul, &t0, &s0, &s1);
      if (op == kSeq) {
        emit(kMov, &dst, &s0);
      } else {
        const S one = splat(1.0f), minus = negated(s0);
        emit(kAdd, &dst, &one, &minus);
      }
      return true;
    }
    case kFloor: case kCeil: {
      const S a = source(ins.src[0]);
      const D t = to_d(scratch(0), 0xF);
      const S s = scratch(0);
      if (op == kFloor) {   // x - frc(x)
        emit(kFrc, &t, &a);
        const S minus = negated(s);
        emit(kAdd, &dst, &a, &minus);
      } else {              // x + frc(-x)
        const S minus_a = negated(a);
        emit(kFrc, &t, &minus_a);
        emit(kAdd, &dst, &a, &s);
      }
      return true;
    }
    case kTrunc: {   // x - sign(x) * frc(|x|)
      const S a = source(ins.src[0]);
      const S magnitude = absolute(a);
      const D t0 = to_d(scratch(0), 0xF), t1 = to_d(scratch(1), 0xF);
      emit(kFrc, &t0, &magnitude);
      const S f = scratch(0), minus_f = negated(scratch(0));
      if (pixel) {
        emit(kCmp, &t1, &a, &f, &minus_f);
      } else {
        const S zero = splat(0.0f);
        const D m = to_d(scratch(2), 0xF), n = to_d(scratch(3), 0xF);
        emit(kSge, &m, &a, &zero);
        emit(kSlt, &n, &a, &zero);
        const S sm = scratch(2), sn = scratch(3);
        emit(kMul, &t1, &f, &sm);
        const S s1 = scratch(1);
        emit(kMad, &t1, &minus_f, &sn, &s1);
      }
      const S minus = negated(scratch(1));
      emit(kAdd, &dst, &a, &minus);
      return true;
    }
    case kRoundNe: {   // floor(x + 0.5), less one where x + 0.5 is a whole odd number
      const S a = source(ins.src[0]), half = splat(0.5f), quarter = splat(0.25f), zero = splat(0.0f);
      const D t0 = to_d(scratch(0), 0xF), t1 = to_d(scratch(1), 0xF), t2 = to_d(scratch(2), 0xF), t3 = to_d(scratch(3), 0xF);
      emit(kAdd, &t0, &a, &half);                  // s0 = x + 0.5
      const S s0 = scratch(0);
      emit(kFrc, &t1, &s0);                        // s1 = frc(s0)
      const S s1 = scratch(1), minus_s1 = negated(scratch(1));
      emit(kAdd, &t0, &s0, &minus_s1);             // s0 = floor(x + 0.5)
      const S r = scratch(0);
      emit(kMul, &t2, &r, &half);
      const S s2 = scratch(2);
      emit(kFrc, &t2, &s2);                        // s2 = frc(r / 2): 0.5 when r is odd
      emit(kSge, &t3, &s2, &quarter);              // s3 = r odd
      emit(kSge, &t2, &minus_s1, &zero);           // s2 = frc(...) == 0  (the tie)
      const S s3 = scratch(3), tie = scratch(2);
      emit(kMul, &t3, &s3, &tie);
      const S adjust = negated(scratch(3));
      emit(kAdd, &dst, &r, &adjust);
      (void)s1;
      return true;
    }
    case kSin: case kCos: {
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(ins.dst.mask & (1u << c))) continue;
        const S a = channel(ins.src[0], c);
        const D t = to_d(scratch(0), 0x3);
        if (sm3_) {
          emit(kSinCos, &t, &a);
        } else {
          const float k1[4] = {-1.5500992e-006f, -2.1701389e-005f, 0.0026041667f, 0.00026041668f}, k2[4] = {-0.020833334f, -0.12500000f, 1.0f, 0.5f};
          const S c1 = constant(k1), c2 = constant(k2);
          emit(kSinCos, &t, &a, &c1, &c2);
        }
        D one = dst;
        one.mask = static_cast<uint8_t>(1u << c);
        const S result = rep(scratch(0), op == kCos ? 0 : 1);
        emit(kMov, &one, &result);
      }
      return true;
    }
    case kSelect: {   // a != 0 ? b : c
      const S a = source(ins.src[0]), b = source(ins.src[1]), c = source(ins.src[2]);
      select_mask(a, true, 0);
      const S m = scratch(0);
      if (pixel) {
        const S minus = negated(m);
        emit(kCmp, &dst, &minus, &c, &b);
      } else {
        select_mask(a, false, 1);
        const D t = to_d(scratch(2), 0xF);
        emit(kMul, &t, &b, &m);
        const S n = scratch(1), partial = scratch(2);
        emit(kMad, &dst, &c, &n, &partial);
      }
      return true;
    }
    case kTexLd: {
      const uint32_t unit = ins.src[1].number;
      if (unit >= graphics::kMaxTextureUnits || vertex_) {
        unsupported_ = true;
        return true;
      }
      samplers_[unit] = true;
      S sampler;
      sampler.type = kSampler, sampler.number = static_cast<uint16_t>(unit);
      const S coordinate = source(ins.src[0]);
      const bool plain = ins.tex_swizzle[0] == 0 && ins.tex_swizzle[1] == 1 && ins.tex_swizzle[2] == 2 && ins.tex_swizzle[3] == 3;
      if (plain) {
        emit(kTexLd, &dst, &coordinate, &sampler, nullptr, ins.control & 1);
      } else {
        const D t = to_d(scratch(0), 0xF);
        emit(kTexLd, &t, &coordinate, &sampler, nullptr, ins.control & 1);
        const S picked = with_swizzle(scratch(0), ins.tex_swizzle[0], ins.tex_swizzle[1], ins.tex_swizzle[2], ins.tex_swizzle[3]);
        emit(kMov, &dst, &picked);
      }
      return true;
    }
    default: unsupported_ = true; return true;
  }
}

NtStatus D3d9Writer::run(List<uint32_t>* out) {
  if (p_.major_ < 4) return status::kNotSupported;   // (only programs made from DXBC; a Direct3D 9 program is its own token stream)
  vertex_ = p_.stage_ == graphics::ShaderStage::Vertex;
  for (const Instr& ins : p_.code_.view()) {
    if (ins.op == kIfNz || ins.op == kIfZ) sm3_ = true;
    const bool writes = ins.op != kNop && ins.op != kElse && ins.op != kEndIf && ins.op != kIfNz && ins.op != kIfZ && ins.op != kDiscardNz && ins.op != kDiscardZ;
    if (writes && ins.dst.type == kTemp && ins.dst.number + 1u > temps_) temps_ = ins.dst.number + 1u;
    for (uint32_t k = 0; k < ins.src_count; ++k)
      if (ins.src[k].type == kTemp && ins.src[k].number + 1u > temps_) temps_ = ins.src[k].number + 1u;
  }
  for (const Instr& ins : p_.code_.view()) {
    instruction(ins);
    if (oom_ || unsupported_) break;
  }
  if (oom_) return status::kNoMemory;
  if (unsupported_) return status::kNotSupported;
  if (temps_ + scratch_used_ > (sm3_ ? 32u : 12u)) return status::kNotSupported;

  // The header: version, the constants, the declarations.
  push(*out, (vertex_ ? 0xFFFE0000u : 0xFFFF0000u) | (sm3_ ? 0x0300u : 0x0200u));
  for (uint64_t i = 0; i < consts_.count(); ++i) {
    push(*out, kDef | (5u << 24));
    push(*out, 0xA00F0000u | static_cast<uint32_t>(kFirstConstant + i));
    for (float f : consts_[i].v) push(*out, __builtin_bit_cast(uint32_t, f));
  }
  auto declare = [&](uint32_t usage, uint32_t index, uint32_t type, uint32_t number) {
    push(*out, kDcl | (2u << 24));
    push(*out, 0x80000000u | usage | (index << 16));
    push(*out, 0x800F0000u | type_bits(type) | number);
  };
  static const uint32_t kSlotUsage[4] = {10, 10, 5, 5}, kSlotIndex[4] = {0, 1, 0, 1};
  if (vertex_) {
    for (uint32_t i = 0; i < 4; ++i)
      if (inputs_[i]) declare(i == 0 ? 0u : i == 1 ? 10u : 5u, i >= 2 ? i - 2 : 0, kInput, i);
    if (sm3_) {
      declare(0, 0, kTexCrdOut, 0);
      for (uint32_t i = 0; i < 4; ++i)
        if (outputs_[i]) declare(kSlotUsage[i], kSlotIndex[i], kTexCrdOut, 1 + i);
    }
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!inputs_[i]) continue;
      if (sm3_) declare(kSlotUsage[i], kSlotIndex[i], kInput, i);
      else declare(0, 0, i < 2 ? kInput : kAddrTex, i < 2 ? i : i - 2);
    }
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
      if (!samplers_[u]) continue;
      push(*out, kDcl | (2u << 24));
      push(*out, 0x80000000u | (2u << 27));
      push(*out, 0x800F0000u | type_bits(kSampler) | u);
    }
  }
  for (uint64_t i = 0; i < body_.count(); ++i) push(*out, body_[i]);
  push(*out, 0xFFFF);
  return oom_ ? status::kNoMemory : status::kSuccess;
}

NtStatus D3d9Program::emit_d3d9_tokens(List<uint32_t>* out) const {
  D3d9Writer writer(*this, allocator_);
  return writer.run(out);
}

}  // namespace dxgk
