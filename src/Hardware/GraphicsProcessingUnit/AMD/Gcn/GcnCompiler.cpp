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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.hpp"

namespace gcn {

namespace {

constexpr uint32_t kScratch = 12;              // vector registers for intermediate values (8) and the staged result of an instruction (4)
constexpr uint32_t kStage = 8;
constexpr uint32_t kMaxDepth = 8;              // nesting of IF
constexpr uint32_t kSgprLimit = 102;
constexpr float kInvTwoPi = 0.15915494309189533577f;

inline uint32_t popcount4(uint32_t m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

// An operand of a vector instruction for one component: where it is and its source modifiers.
struct Opd {
  Src src;
  bool neg = false, abs = false;
  bool is_vgpr = false;
  bool is_sgpr = false;           // a scalar register (not an inline constant or a literal)
};

class Compiler {
 public:
  Compiler(const TgsiShader& s, Code& c, ShaderInfo* info) : s_(s), c_(c), info_(info) {}
  bool run(const char** error);

 private:
  bool fail(const char* why) {
    if (!error_) error_ = why;
    return false;
  }
  uint32_t temp_reg(uint32_t index, uint32_t comp) const { return temp_base_ + 4 * index + comp; }
  uint32_t in_reg(uint32_t index, uint32_t comp) const { return in_base_ + 4 * index + comp; }
  uint32_t out_reg(uint32_t index, uint32_t comp) const { return out_base_ + 4 * index + comp; }
  uint32_t new_scratch() {
    if (scratch_used_ >= kStage) {
      fail("an instruction needs too many intermediate registers");
      return scratch_base_;
    }
    return scratch_base_ + scratch_used_++;
  }
  bool alloc_sgpr(uint32_t count, uint32_t align, uint32_t* out) {
    sgpr_next_ = (sgpr_next_ + align - 1) / align * align;
    if (sgpr_next_ + count > kSgprLimit) return fail("out of scalar registers");
    *out = sgpr_next_;
    sgpr_next_ += count;
    return true;
  }

  // operands and the vector instruction emitters (all in the VOP3 forms, whose operands are legalized here)
  Opd operand(const TgsiSrc& s, uint32_t comp);
  static Opd vreg_operand(uint32_t reg);
  static Opd constant(float value);
  void legalize(Opd* ops, uint32_t count);
  static Mods mods_of(const Opd* ops, uint32_t count, bool clamp = false);
  void v_mov(uint32_t d, Opd a);
  void f1(Vop1 op, uint32_t d, Opd a);
  void f2(Vop2 op, uint32_t d, Opd a, Opd b);
  void f3(Vop3 op, uint32_t d, Opd a, Opd b, Opd c);
  void compare(Vopc op, uint32_t sdst, Opd a, Opd b);
  void select(uint32_t d, Opd if_false, Opd if_true, uint32_t mask_pair);
  void set_bool(uint32_t d, Vopc op, Opd a, Opd b);
  void clamp01(uint32_t d);

  bool analyze();
  bool prologue();
  bool body();
  bool epilogue();
  bool instruction(const TgsiInst& i);
  bool tex(const TgsiInst& i);
  bool dot(const TgsiInst& i, uint32_t n);
  bool cross(const TgsiInst& i);
  bool kill(const TgsiInst& i);
  bool control(const TgsiInst& i);
  bool scalar(const TgsiInst& i);
  bool componentwise(const TgsiInst& i);
  // The destination of the instruction in progress: the register of a component, staged in scratch when the instruction reads the register it writes.
  void begin_dest(const TgsiInst& i);
  uint32_t dreg(const TgsiInst& i, uint32_t comp) const;
  void finish_dest(const TgsiInst& i);

  const TgsiShader& s_;
  Code& c_;
  ShaderInfo* info_;
  const char* error_ = nullptr;

  uint32_t in_base_ = 0, temp_base_ = 0, out_base_ = 0, scratch_base_ = 0, vgpr_total_ = 0;
  uint32_t sgpr_next_ = abi::kUserSgprs;
  uint32_t scratch_used_ = 0;
  uint32_t const_sgpr_[TgsiShader::kMaxConsts] = {};
  bool const_used_[TgsiShader::kMaxConsts] = {};
  bool in_used_[TgsiShader::kMaxInputs] = {};
  bool sampler_used_[TgsiShader::kMaxSamplers] = {};
  uint32_t image_sgpr_[TgsiShader::kMaxSamplers] = {}, sampler_sgpr_[TgsiShader::kMaxSamplers] = {}, vbuf_sgpr_[TgsiShader::kMaxInputs] = {};
  bool uses_kill_ = false;
  uint32_t live_ = 0;                    // SGPR pair: the lanes not killed
  uint32_t pair_a_ = 0, pair_b_ = 0;     // SGPR pairs for masks
  uint32_t prim_mask_sgpr_ = 0;
  uint32_t input_attr_[TgsiShader::kMaxInputs] = {};
  struct Branch {
    uint32_t if_branch, else_branch;
    uint32_t save, then;                 // SGPR pairs
    bool has_else;
  };
  Branch stack_[kMaxDepth] = {};
  uint32_t depth_ = 0;
  uint32_t cf_pairs_[kMaxDepth][2] = {};
  uint32_t max_depth_ = 0;
  bool staged_ = false;
};

// ---- operands ------------------------------------------------------------------------------------------------------------------------------------------------------------

Opd Compiler::vreg_operand(uint32_t reg) {
  Opd o;
  o.src = vgpr(reg);
  o.is_vgpr = true;
  return o;
}

Opd Compiler::constant(float value) {
  Opd o;
  o.src = imm_float(value);
  return o;
}

Opd Compiler::operand(const TgsiSrc& s, uint32_t comp) {
  const uint32_t sw = s.swizzle[comp & 3];
  Opd o;
  switch (s.file) {
    case TgsiFile::Temp: o = vreg_operand(temp_reg(s.index, sw)); break;
    case TgsiFile::In: o = vreg_operand(in_reg(s.index, sw)); break;
    case TgsiFile::Const:
      o.src = sgpr(const_sgpr_[s.index] + sw);
      o.is_sgpr = true;
      break;
    case TgsiFile::Imm: {
      float v = s.index < s_.immediates.count() ? s_.immediates[s.index].value[sw] : 0.0f;
      if (s.absolute && v < 0) v = -v;
      if (s.negate) v = -v;
      return constant(v);       // (the modifiers are part of the value)
    }
    default:
      fail("a source that is not a register");
      return o;
  }
  o.neg = s.negate;
  o.abs = s.absolute;
  return o;
}

// Makes the operands usable by one VOP3 instruction: no literal, and at most one scalar register (the constant bus carries one scalar value): the others go through a vector register.
void Compiler::legalize(Opd* ops, uint32_t count) {
  uint32_t scalar_code = 0xFFFF;
  for (uint32_t i = 0; i < count; ++i) {
    Opd& o = ops[i];
    if (o.is_vgpr) continue;
    bool move = o.src.literal;
    if (o.is_sgpr) {
      if (scalar_code == 0xFFFF) scalar_code = o.src.code;
      else if (scalar_code != o.src.code) move = true;
    }
    if (move) {
      const uint32_t t = new_scratch();
      c_.vop1(Vop1::v_mov_b32, t, o.src);
      const bool neg = o.neg, abs = o.abs;
      o = vreg_operand(t);
      o.neg = neg;
      o.abs = abs;
    }
  }
}

Mods Compiler::mods_of(const Opd* ops, uint32_t count, bool clamp) {
  Mods m;
  for (uint32_t i = 0; i < count; ++i) {
    if (ops[i].neg) m.neg = static_cast<uint8_t>(m.neg | (1u << i));
    if (ops[i].abs) m.abs = static_cast<uint8_t>(m.abs | (1u << i));
  }
  m.clamp = clamp;
  return m;
}

void Compiler::v_mov(uint32_t d, Opd a) {
  if (!a.neg && !a.abs) {
    c_.vop1(Vop1::v_mov_b32, d, a.src);
    return;
  }
  Opd two[2] = {a, a};
  legalize(two, 2);       // (both operands are the same scalar or none)
  c_.vop3_of(Vop2::v_max_f32, d, two[0].src, two[1].src, mods_of(two, 2));       // max(x, x) with the modifiers applied to both
}

void Compiler::f1(Vop1 op, uint32_t d, Opd a) {
  legalize(&a, 1);
  c_.vop3_of(op, d, a.src, mods_of(&a, 1));
}

void Compiler::f2(Vop2 op, uint32_t d, Opd a, Opd b) {
  Opd ops[2] = {a, b};
  legalize(ops, 2);
  c_.vop3_of(op, d, ops[0].src, ops[1].src, mods_of(ops, 2));
}

void Compiler::f3(Vop3 op, uint32_t d, Opd a, Opd b, Opd c) {
  Opd ops[3] = {a, b, c};
  legalize(ops, 3);
  c_.vop3(op, d, ops[0].src, ops[1].src, ops[2].src, mods_of(ops, 3));
}

void Compiler::compare(Vopc op, uint32_t sdst, Opd a, Opd b) {
  Opd ops[2] = {a, b};
  legalize(ops, 2);
  c_.vop3_of(op, sdst, ops[0].src, ops[1].src, mods_of(ops, 2));
}

// d = mask ? if_true : if_false, the mask in the SGPR pair `mask_pair` (the select operands are vector registers or inline constants: the mask takes the constant bus)
void Compiler::select(uint32_t d, Opd if_false, Opd if_true, uint32_t mask_pair) {
  Opd ops[2] = {if_false, if_true};
  for (Opd& o : ops) {
    if (!o.is_vgpr && (o.is_sgpr || o.src.literal)) {
      const uint32_t t = new_scratch();
      c_.vop1(Vop1::v_mov_b32, t, o.src);
      const bool neg = o.neg, abs = o.abs;
      o = vreg_operand(t);
      o.neg = neg;
      o.abs = abs;
    }
  }
  c_.vop3_of(Vop2::v_cndmask_b32, d, ops[0].src, ops[1].src, sgpr(mask_pair), mods_of(ops, 2));
}

void Compiler::set_bool(uint32_t d, Vopc op, Opd a, Opd b) {
  compare(op, kCodeVccLo, a, b);
  select(d, constant(0.0f), constant(1.0f), kCodeVccLo);
}

void Compiler::clamp01(uint32_t d) {
  c_.vop3_of(Vop2::v_max_f32, d, vgpr(d), vgpr(d), Mods{0, 0, true, 0});
}

// ---- destinations --------------------------------------------------------------------------------------------------------------------------------------------------------

void Compiler::begin_dest(const TgsiInst& i) {
  staged_ = false;
  if (popcount4(i.dst.mask) < 2) return;
  for (uint32_t k = 0; k < i.src_count; ++k)
    if (i.src[k].file == i.dst.file && i.src[k].index == i.dst.index) staged_ = true;
}

uint32_t Compiler::dreg(const TgsiInst& i, uint32_t comp) const {
  if (staged_) return scratch_base_ + kStage + comp;
  return i.dst.file == TgsiFile::Out ? out_reg(i.dst.index, comp) : temp_reg(i.dst.index, comp);
}

void Compiler::finish_dest(const TgsiInst& i) {
  for (uint32_t comp = 0; comp < 4; ++comp) {
    if (!(i.dst.mask >> comp & 1)) continue;
    if (i.dst.saturate) clamp01(dreg(i, comp));
    if (staged_) c_.vop1(Vop1::v_mov_b32, i.dst.file == TgsiFile::Out ? out_reg(i.dst.index, comp) : temp_reg(i.dst.index, comp), vgpr(dreg(i, comp)));
  }
  staged_ = false;
}

// ---- analysis and prologue / epilogue -------------------------------------------------------------------------------------------------------------------------------

bool Compiler::analyze() {
  if (s_.temps > TgsiShader::kMaxTemps) return fail("too many temporaries");
  uint32_t depth = 0;
  for (uint64_t n = 0; n < s_.instructions.count(); ++n) {
    const TgsiInst& i = s_.instructions[n];
    for (uint32_t k = 0; k < i.src_count; ++k) {
      const TgsiSrc& src = i.src[k];
      if (src.file == TgsiFile::In) {
        if (src.index >= s_.inputs) return fail("an input that is not declared");
        in_used_[src.index] = true;
      } else if (src.file == TgsiFile::Const) {
        if (src.index >= TgsiShader::kMaxConsts) return fail("constant row out of range");
        const_used_[src.index] = true;
        if (static_cast<uint32_t>(src.index) + 1u > info_->const_rows) info_->const_rows = static_cast<uint32_t>(src.index) + 1u;
      } else if (src.file == TgsiFile::Temp && src.index >= s_.temps) {
        return fail("a temporary that is not declared");
      } else if (src.file == TgsiFile::Imm && src.index >= s_.immediates.count()) {
        return fail("an immediate that is not declared");
      }
    }
    if (i.dst.file == TgsiFile::Temp && i.dst.index >= s_.temps) return fail("a temporary that is not declared");
    if (i.dst.file == TgsiFile::Out && i.dst.index >= s_.outputs) return fail("an output that is not declared");
    if (i.op == TgsiOp::Tex || i.op == TgsiOp::Txb || i.op == TgsiOp::Txl) {
      if (!s_.fragment) return fail("texture sampling in a vertex shader");
      if (i.sampler >= abi::kMaxSamplers) return fail("sampler index out of range");
      sampler_used_[i.sampler] = true;
    }
    if (i.op == TgsiOp::Kill || i.op == TgsiOp::KillIf) {
      if (!s_.fragment) return fail("discard in a vertex shader");
      uses_kill_ = true;
    }
    if (i.op == TgsiOp::If) {
      if (++depth > kMaxDepth) return fail("IF nested too deeply");
      if (depth > max_depth_) max_depth_ = depth;
    } else if (i.op == TgsiOp::EndIf) {
      if (depth == 0) return fail("ENDIF without IF");
      --depth;
    }
  }
  if (depth != 0) return fail("IF without ENDIF");
  if (s_.fragment) {
    for (uint32_t k = 0; k < s_.inputs; ++k)
      if (s_.input_semantic[k] == TgsiSemantic::Position || s_.input_semantic[k] == TgsiSemantic::Face) {
        if (in_used_[k]) return fail("the fragment position and face are not supported");
      }
  } else {
    for (uint32_t k = 0; k < s_.inputs; ++k)
      if (in_used_[k] && k >= abi::kMaxAttributes) return fail("too many vertex attributes");
  }
  return true;
}

bool Compiler::prologue() {
  info_->fragment = s_.fragment;
  // scalar registers
  uint32_t m0_source = 0;
  if (s_.fragment) {
    prim_mask_sgpr_ = abi::kUserSgprs;
    sgpr_next_ = abi::kUserSgprs + 1;
    m0_source = prim_mask_sgpr_;
  }
  for (uint32_t r = 0; r < TgsiShader::kMaxConsts; ++r)
    if (const_used_[r] && !alloc_sgpr(4, 4, &const_sgpr_[r])) return false;
  if (!s_.fragment) {
    for (uint32_t k = 0; k < s_.inputs; ++k)
      if (in_used_[k]) {
        if (!alloc_sgpr(4, 4, &vbuf_sgpr_[k])) return false;
        info_->attributes_used |= 1u << k;
      }
  } else {
    for (uint32_t k = 0; k < abi::kMaxSamplers; ++k)
      if (sampler_used_[k]) {
        if (!alloc_sgpr(8, 4, &image_sgpr_[k]) || !alloc_sgpr(4, 4, &sampler_sgpr_[k])) return false;
        info_->samplers_used |= 1u << k;
      }
    if (uses_kill_ && !alloc_sgpr(2, 2, &live_)) return false;
  }
  if (!alloc_sgpr(2, 2, &pair_a_) || !alloc_sgpr(2, 2, &pair_b_)) return false;
  for (uint32_t d = 0; d < max_depth_; ++d)
    if (!alloc_sgpr(2, 2, &cf_pairs_[d][0]) || !alloc_sgpr(2, 2, &cf_pairs_[d][1])) return false;
  // vector registers
  uint32_t at = s_.fragment ? 2 : 1;             // (v0, v1 hold the barycentric coordinates; v0 the vertex index)
  in_base_ = at;
  at += 4 * s_.inputs;
  temp_base_ = at;
  at += 4 * s_.temps;
  out_base_ = at;
  at += 4 * s_.outputs;
  scratch_base_ = at;
  at += kScratch;
  vgpr_total_ = at;
  if (vgpr_total_ > 256) return fail("out of vector registers");

  // loads: the constants, the descriptors
  bool loaded = false;
  for (uint32_t r = 0; r < TgsiShader::kMaxConsts; ++r)
    if (const_used_[r]) {
      c_.smem(Smem::s_load_dwordx4, const_sgpr_[r], 0, r * 16);
      loaded = true;
    }
  if (!s_.fragment) {
    for (uint32_t k = 0; k < s_.inputs; ++k)
      if (in_used_[k]) {
        c_.smem(Smem::s_load_dwordx4, vbuf_sgpr_[k], 2, k * abi::kVertexBufferBytes);
        loaded = true;
      }
    if (loaded) c_.waitcnt(15, 7, 0);
    bool fetched = false;
    for (uint32_t k = 0; k < s_.inputs; ++k)
      if (in_used_[k]) {
        c_.mubuf(Mubuf::buffer_load_format_xyzw, in_reg(k, 0), 0, vbuf_sgpr_[k], imm_int(0), Code::BufferMode{false, true, false, false, 0});
        fetched = true;
      }
    if (fetched) c_.waitcnt(0, 7, 15);
  } else {
    for (uint32_t k = 0; k < abi::kMaxSamplers; ++k)
      if (sampler_used_[k]) {
        c_.smem(Smem::s_load_dwordx8, image_sgpr_[k], 2, abi::kImageOffset + k * abi::kImageBytes);
        c_.smem(Smem::s_load_dwordx4, sampler_sgpr_[k], 2, abi::kSamplerOffset + k * abi::kSamplerBytes);
        loaded = true;
      }
    if (loaded) c_.waitcnt(15, 7, 0);
    // the interpolation: one attribute per input that is read, in the order of the inputs
    uint32_t attr = 0;
    bool any = false;
    for (uint32_t k = 0; k < s_.inputs; ++k) {
      input_attr_[k] = 0xFF;
      if (!in_used_[k]) continue;
      input_attr_[k] = attr;
      if (attr >= 32) return fail("too many interpolated inputs");
      if (!any) {
        c_.sop1(Sop1::s_mov_b32, kCodeM0, sgpr(m0_source));
        any = true;
      }
      for (uint32_t ch = 0; ch < 4; ++ch) {
        if (s_.input_flat[k]) {
          c_.vintrp(Vintrp::v_interp_mov_f32, in_reg(k, ch), 2, attr, ch);       // the first vertex's value (P0)
        } else {
          c_.vintrp(Vintrp::v_interp_p1_f32, in_reg(k, ch), 0, attr, ch);
          c_.vintrp(Vintrp::v_interp_p2_f32, in_reg(k, ch), 1, attr, ch);
        }
      }
      ++attr;
    }
    for (uint32_t k = 0; k < s_.inputs; ++k) info_->input_attribute[k] = input_attr_[k];
    info_->ps_inputs = attr;
    info_->ps_input_enable = abi::kPsInputEnablePerspCenter;
    if (uses_kill_) c_.sop1(Sop1::s_mov_b64, live_, exec());
  }
  return c_.ok();
}

bool Compiler::epilogue() {
  struct Export {
    uint32_t target, reg;
  };
  Export list[2 + TgsiShader::kMaxOutputs];
  uint32_t count = 0;
  if (s_.fragment) {
    int color[2] = {-1, -1};
    for (uint32_t k = 0; k < s_.outputs; ++k) {
      if (s_.output_semantic[k] == TgsiSemantic::Color) {
        const uint32_t index = s_.output_semantic_index[k];
        if (index > 1 || color[index] >= 0) return fail("at most two colour outputs, COLOR[0] and COLOR[1], are supported");
        color[index] = static_cast<int>(k);
      } else if (s_.output_semantic[k] == TgsiSemantic::Position) {
        return fail("depth output is not supported");
      }
    }
    if (color[0] < 0) return fail(color[1] >= 0 ? "COLOR[1] without a COLOR[0] colour output" : "no colour output");
    list[count++] = {kExpMrt0, out_reg(static_cast<uint32_t>(color[0]), 0)};
    if (color[1] >= 0) list[count++] = {kExpMrt0 + 1, out_reg(static_cast<uint32_t>(color[1]), 0)};
    info_->color_outputs = color[1] >= 0 ? 2 : 1;
  } else {
    bool position = false;
    uint32_t param = 0;
    for (uint32_t k = 0; k < s_.outputs; ++k) info_->output_param[k] = 0xFF;
    for (uint32_t k = 0; k < s_.outputs; ++k) {
      if (s_.output_semantic[k] == TgsiSemantic::Position) {
        if (position) return fail("two position outputs");
        position = true;
        list[count++] = {kExpPos0, out_reg(k, 0)};
      }
    }
    if (!position) return fail("no position output");
    for (uint32_t k = 0; k < s_.outputs; ++k) {
      if (s_.output_semantic[k] == TgsiSemantic::Position || s_.output_semantic[k] == TgsiSemantic::PointSize) continue;
      if (param >= 32) return fail("too many parameter exports");
      info_->output_param[k] = param;
      list[count++] = {kExpParam0 + param, out_reg(k, 0)};
      ++param;
    }
    info_->param_exports = param;
  }
  for (uint32_t n = 0; n < count; ++n) {
    const bool last = n + 1 == count;
    c_.exp(list[n].target, 15, list[n].reg, list[n].reg + 1, list[n].reg + 2, list[n].reg + 3, last, s_.fragment && last);
  }
  c_.sopp(Sopp::s_endpgm);
  return c_.ok();
}

// ---- the instructions -----------------------------------------------------------------------------------------------------------------------------------------------------

bool Compiler::body() {
  for (uint64_t n = 0; n < s_.instructions.count() && !error_; ++n) {
    scratch_used_ = 0;
    if (!instruction(s_.instructions[n])) return false;
    if (!c_.ok()) return fail("out of memory");
  }
  return error_ == nullptr;
}

bool Compiler::componentwise(const TgsiInst& i) {
  begin_dest(i);
  for (uint32_t comp = 0; comp < 4; ++comp) {
    if (!(i.dst.mask >> comp & 1)) continue;
    scratch_used_ = 0;
    const uint32_t d = dreg(i, comp);
    const Opd a = operand(i.src[0], comp);
    const Opd b = i.src_count > 1 ? operand(i.src[1], comp) : Opd();
    const Opd c = i.src_count > 2 ? operand(i.src[2], comp) : Opd();
    switch (i.op) {
      case TgsiOp::Mov: v_mov(d, a); break;
      case TgsiOp::Abs: { Opd t = a; t.abs = true; t.neg = false; v_mov(d, t); break; }
      case TgsiOp::Add: f2(Vop2::v_add_f32, d, a, b); break;
      case TgsiOp::Sub: f2(Vop2::v_sub_f32, d, a, b); break;
      case TgsiOp::Mul: f2(Vop2::v_mul_f32, d, a, b); break;
      case TgsiOp::Mad: f3(Vop3::v_mad_f32, d, a, b, c); break;
      case TgsiOp::Max: f2(Vop2::v_max_f32, d, a, b); break;
      case TgsiOp::Min: f2(Vop2::v_min_f32, d, a, b); break;
      case TgsiOp::Div: {
        const uint32_t t = new_scratch();
        f1(Vop1::v_rcp_f32, t, b);
        f2(Vop2::v_mul_f32, d, a, vreg_operand(t));
        break;
      }
      case TgsiOp::Lrp: {       // a * b + (1 - a) * c = a * (b - c) + c
        const uint32_t t = new_scratch();
        f2(Vop2::v_sub_f32, t, b, c);
        f3(Vop3::v_mad_f32, d, a, vreg_operand(t), c);
        break;
      }
      case TgsiOp::Frc: f1(Vop1::v_fract_f32, d, a); break;
      case TgsiOp::Flr: f1(Vop1::v_floor_f32, d, a); break;
      case TgsiOp::Ceil: f1(Vop1::v_ceil_f32, d, a); break;
      case TgsiOp::Trunc: f1(Vop1::v_trunc_f32, d, a); break;
      case TgsiOp::Round: f1(Vop1::v_rndne_f32, d, a); break;
      case TgsiOp::Ssg: {       // the sign: (a > 0) - (a < 0)
        const uint32_t t0 = new_scratch(), t1 = new_scratch();
        set_bool(t0, Vopc::v_cmp_gt_f32, a, constant(0.0f));
        set_bool(t1, Vopc::v_cmp_lt_f32, a, constant(0.0f));
        f2(Vop2::v_sub_f32, d, vreg_operand(t0), vreg_operand(t1));
        break;
      }
      case TgsiOp::Cmp: {       // a < 0 ? b : c
        compare(Vopc::v_cmp_gt_f32, kCodeVccLo, constant(0.0f), a);
        select(d, c, b, kCodeVccLo);
        break;
      }
      case TgsiOp::Seq: set_bool(d, Vopc::v_cmp_eq_f32, a, b); break;
      case TgsiOp::Sne: set_bool(d, Vopc::v_cmp_neq_f32, a, b); break;
      case TgsiOp::Sge: set_bool(d, Vopc::v_cmp_ge_f32, a, b); break;
      case TgsiOp::Slt: set_bool(d, Vopc::v_cmp_lt_f32, a, b); break;
      case TgsiOp::Sgt: set_bool(d, Vopc::v_cmp_gt_f32, a, b); break;
      case TgsiOp::Sle: set_bool(d, Vopc::v_cmp_le_f32, a, b); break;
      default: return fail("an instruction the compiler does not know");
    }
  }
  finish_dest(i);
  return error_ == nullptr;
}

// RCP, RSQ, SQRT, EX2, LG2, SIN, COS, POW: the x component of the sources, the result in every written component
bool Compiler::scalar(const TgsiInst& i) {
  begin_dest(i);
  scratch_used_ = 0;
  const uint32_t t = new_scratch();
  const Opd a = operand(i.src[0], 0);
  switch (i.op) {
    case TgsiOp::Rcp: f1(Vop1::v_rcp_f32, t, a); break;
    case TgsiOp::Rsq: { Opd m = a; m.abs = true; m.neg = false; f1(Vop1::v_rsq_f32, t, m); break; }
    case TgsiOp::Sqrt: f1(Vop1::v_sqrt_f32, t, a); break;
    case TgsiOp::Ex2: f1(Vop1::v_exp_f32, t, a); break;
    case TgsiOp::Lg2: f1(Vop1::v_log_f32, t, a); break;
    case TgsiOp::Sin: case TgsiOp::Cos: {
      const uint32_t turns = new_scratch();
      f2(Vop2::v_mul_f32, turns, a, constant(kInvTwoPi));
      f1(i.op == TgsiOp::Sin ? Vop1::v_sin_f32 : Vop1::v_cos_f32, t, vreg_operand(turns));
      break;
    }
    case TgsiOp::Pow: {       // exp2(b * log2(a))
      const Opd b = operand(i.src[1], 0);
      const uint32_t l = new_scratch();
      f1(Vop1::v_log_f32, l, a);
      f2(Vop2::v_mul_f32, l, vreg_operand(l), b);
      f1(Vop1::v_exp_f32, t, vreg_operand(l));
      break;
    }
    default: return fail("an instruction the compiler does not know");
  }
  for (uint32_t comp = 0; comp < 4; ++comp)
    if (i.dst.mask >> comp & 1) c_.vop1(Vop1::v_mov_b32, dreg(i, comp), vgpr(t));
  finish_dest(i);
  return error_ == nullptr;
}

// DP2, DP3, DP4: the dot product of the first n components, in every written component
bool Compiler::dot(const TgsiInst& i, uint32_t n) {
  begin_dest(i);
  const uint32_t acc = new_scratch();
  f2(Vop2::v_mul_f32, acc, operand(i.src[0], 0), operand(i.src[1], 0));
  for (uint32_t k = 1; k < n; ++k) f3(Vop3::v_mad_f32, acc, operand(i.src[0], k), operand(i.src[1], k), vreg_operand(acc));
  for (uint32_t comp = 0; comp < 4; ++comp)
    if (i.dst.mask >> comp & 1) c_.vop1(Vop1::v_mov_b32, dreg(i, comp), vgpr(acc));
  finish_dest(i);
  return error_ == nullptr;
}

// XPD: x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x, w = 1
bool Compiler::cross(const TgsiInst& i) {
  begin_dest(i);
  static const uint8_t kNext[3] = {1, 2, 0}, kNext2[3] = {2, 0, 1};
  for (uint32_t comp = 0; comp < 3; ++comp) {
    if (!(i.dst.mask >> comp & 1)) continue;
    scratch_used_ = 0;
    const uint32_t t = new_scratch();
    const uint32_t p = kNext[comp], q = kNext2[comp];
    f2(Vop2::v_mul_f32, t, operand(i.src[0], p), operand(i.src[1], q));
    Opd na = operand(i.src[0], q);
    na.neg = !na.neg;
    f3(Vop3::v_mad_f32, dreg(i, comp), na, operand(i.src[1], p), vreg_operand(t));
  }
  if (i.dst.mask & 8) c_.vop1(Vop1::v_mov_b32, dreg(i, 3), imm_float(1.0f));
  finish_dest(i);
  return error_ == nullptr;
}

bool Compiler::tex(const TgsiInst& i) {
  // the coordinates (and a bias or lod) into consecutive registers, the sample, the result into the destination
  const uint32_t base = scratch_base_;
  const bool bias = i.op == TgsiOp::Txb, lod = i.op == TgsiOp::Txl;
  scratch_used_ = kStage;       // (all of the intermediate registers are taken: the sample reads and writes them in place)
  uint32_t at = base;
  // layout: image_sample_b: [bias, coordinates]; image_sample_l: [coordinates, lod]; image_sample: [coordinates]. The coordinates are x, y for a 2D image; x, y, z for 3D (z the depth);
  // x, y, layer for an array; for a cube the face coordinates and the face: (sc / |ma| + 1.5, tc / |ma| + 1.5, id) from v_cubesc, v_cubetc, v_cubema and v_cubeid of the direction.
  const uint8_t target = s_.sampler_target[i.sampler];
  Mimg op = Mimg::image_sample;
  if (bias) {
    v_mov(at++, operand(i.src[0], 3));
    op = Mimg::image_sample_b;
  }
  if (target == TgsiShader::kTargetCube) {
    // (the direction is copied to registers first: the cube instructions take no constants or literals; the registers base + 4 .. 7 are the result's, free until the sample)
    const uint32_t sc = at, tc = at + 1, face = at + 2;
    v_mov(base + 5, operand(i.src[0], 0));
    v_mov(base + 6, operand(i.src[0], 1));
    v_mov(base + 7, operand(i.src[0], 2));
    f3(Vop3::v_cubeid_f32, face, vreg_operand(base + 5), vreg_operand(base + 6), vreg_operand(base + 7));
    f3(Vop3::v_cubesc_f32, sc, vreg_operand(base + 5), vreg_operand(base + 6), vreg_operand(base + 7));
    f3(Vop3::v_cubetc_f32, tc, vreg_operand(base + 5), vreg_operand(base + 6), vreg_operand(base + 7));
    f3(Vop3::v_cubema_f32, base + 4, vreg_operand(base + 5), vreg_operand(base + 6), vreg_operand(base + 7));
    Opd ma = vreg_operand(base + 4);
    ma.abs = true;
    f1(Vop1::v_rcp_f32, base + 4, ma);
    v_mov(base + 5, constant(1.5f));
    f3(Vop3::v_mad_f32, sc, vreg_operand(sc), vreg_operand(base + 4), vreg_operand(base + 5));
    f3(Vop3::v_mad_f32, tc, vreg_operand(tc), vreg_operand(base + 4), vreg_operand(base + 5));
    at += 3;
  } else {
    v_mov(at++, operand(i.src[0], 0));
    v_mov(at++, operand(i.src[0], 1));
    if (target != TgsiShader::kTarget2D) v_mov(at++, operand(i.src[0], 2));
  }
  if (lod) {
    v_mov(at++, operand(i.src[0], 3));
    op = Mimg::image_sample_l;
  }
  const uint32_t result = base + 4;
  // whole quad mode for the implicit derivatives, then the active lanes again
  c_.sop1(Sop1::s_mov_b64, pair_a_, exec());
  c_.sop1(Sop1::s_wqm_b64, kCodeExecLo, exec());
  c_.mimg(op, result, base, image_sgpr_[i.sampler], sampler_sgpr_[i.sampler], 15);
  c_.waitcnt(0, 7, 15);
  c_.sop1(Sop1::s_mov_b64, kCodeExecLo, sgpr(pair_a_));
  begin_dest(i);
  for (uint32_t comp = 0; comp < 4; ++comp)
    if (i.dst.mask >> comp & 1) c_.vop1(Vop1::v_mov_b32, dreg(i, comp), vgpr(result + comp));
  finish_dest(i);
  return error_ == nullptr;
}

bool Compiler::kill(const TgsiInst& i) {
  if (i.op == TgsiOp::Kill) {
    c_.sop2(Sop2::s_andn2_b64, live_, sgpr(live_), exec());
  } else {
    // the lanes where any component is negative
    bool first = true;
    uint8_t done_mask = 0;
    for (uint32_t comp = 0; comp < 4; ++comp) {
      const uint32_t sw = i.src[0].swizzle[comp];
      if (done_mask >> sw & 1) continue;
      done_mask = static_cast<uint8_t>(done_mask | (1u << sw));
      scratch_used_ = 0;
      compare(Vopc::v_cmp_gt_f32, first ? pair_a_ : pair_b_, constant(0.0f), operand(i.src[0], comp));
      if (!first) c_.sop2(Sop2::s_or_b64, pair_a_, sgpr(pair_a_), sgpr(pair_b_));
      first = false;
    }
    c_.sop2(Sop2::s_andn2_b64, live_, sgpr(live_), sgpr(pair_a_));
  }
  c_.sop2(Sop2::s_and_b64, kCodeExecLo, exec(), sgpr(live_));
  return error_ == nullptr;
}

// IF / ELSE / ENDIF with the execution mask: the then lanes are those where the condition (the x component, not zero) holds; the else lanes the rest of those that were active
bool Compiler::control(const TgsiInst& i) {
  if (i.op == TgsiOp::If) {
    if (depth_ >= kMaxDepth) return fail("IF nested too deeply");
    Branch& b = stack_[depth_];
    b.save = cf_pairs_[depth_][0];
    b.then = cf_pairs_[depth_][1];
    b.has_else = false;
    scratch_used_ = 0;
    compare(Vopc::v_cmp_neq_f32, b.then, operand(i.src[0], 0), constant(0.0f));       // (the lanes in exec whose condition holds)
    c_.sop1(Sop1::s_and_saveexec_b64, b.save, sgpr(b.then));                          // save = exec; exec = then
    b.if_branch = c_.branch(Sopp::s_cbranch_execz);
    b.else_branch = 0;
    ++depth_;
    return true;
  }
  if (depth_ == 0) return fail("ELSE or ENDIF without IF");
  Branch& b = stack_[depth_ - 1];
  if (i.op == TgsiOp::Else) {
    if (b.has_else) return fail("two ELSE");
    b.has_else = true;
    c_.patch_branch(b.if_branch, c_.size());                                           // no then lanes: here
    c_.sop2(Sop2::s_andn2_b64, kCodeExecLo, sgpr(b.save), sgpr(b.then));               // exec = the lanes that were active and not then
    if (uses_kill_) c_.sop2(Sop2::s_and_b64, kCodeExecLo, exec(), sgpr(live_));
    b.else_branch = c_.branch(Sopp::s_cbranch_execz);
    return true;
  }
  // ENDIF: the lanes that were active (and alive) again
  const uint32_t here = c_.size();
  if (b.has_else) c_.patch_branch(b.else_branch, here);
  else c_.patch_branch(b.if_branch, here);
  if (uses_kill_) c_.sop2(Sop2::s_and_b64, kCodeExecLo, sgpr(b.save), sgpr(live_));
  else c_.sop1(Sop1::s_mov_b64, kCodeExecLo, sgpr(b.save));
  --depth_;
  return true;
}

bool Compiler::instruction(const TgsiInst& i) {
  switch (i.op) {
    case TgsiOp::Dp2: return dot(i, 2);
    case TgsiOp::Dp3: return dot(i, 3);
    case TgsiOp::Dp4: return dot(i, 4);
    case TgsiOp::Xpd: return cross(i);
    case TgsiOp::Rcp: case TgsiOp::Rsq: case TgsiOp::Sqrt: case TgsiOp::Ex2: case TgsiOp::Lg2: case TgsiOp::Sin: case TgsiOp::Cos: case TgsiOp::Pow: return scalar(i);
    case TgsiOp::Tex: case TgsiOp::Txb: case TgsiOp::Txl: return tex(i);
    case TgsiOp::Kill: case TgsiOp::KillIf: return kill(i);
    case TgsiOp::If: case TgsiOp::Else: case TgsiOp::EndIf: return control(i);
    case TgsiOp::End: return true;
    default: return componentwise(i);
  }
}

bool Compiler::run(const char** error) {
  const bool ok = analyze() && prologue() && body() && epilogue();
  if (error) *error = ok ? nullptr : (error_ ? error_ : "out of memory");
  if (ok) {
    info_->vgprs = vgpr_total_;
    info_->sgprs = sgpr_next_;
    info_->kills = uses_kill_;
    info_->code_dwords = c_.size();
  }
  return ok && c_.ok();
}

}  // namespace

bool compile_tgsi(const TgsiShader& shader, Code& code, ShaderInfo* info, const char** error) {
  ShaderInfo local;
  Compiler compiler(shader, code, info ? info : &local);
  return compiler.run(error);
}

}  // namespace gcn
