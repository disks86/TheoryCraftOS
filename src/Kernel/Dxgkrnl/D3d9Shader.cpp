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

#include "Kernel/Dxgkrnl/D3d9Decoded.hpp"
#include <utility>
#include <new>
#include "Math/FloatMath.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace dxgk {

namespace {


struct Q {
  float f[4];
};

float fabs_(float x) { return x < 0 ? -x : x; }
float floor_(float x) {
  if (!(x == x) || x > 1e9f || x < -1e9f) return x;
  const float r = static_cast<float>(static_cast<int64_t>(x));
  return r > x ? r - 1.0f : r;
}
float sqrt_(float x) { return static_cast<float>(fmath::sqrt(x)); }
float rcp_(float x) { return 1.0f / x; }   // (a zero gives infinity, as in Direct3D)
float sat_(float x) { return x > 0 ? (x < 1 ? x : 1.0f) : 0.0f; }
uint32_t bits_of(float f) { return __builtin_bit_cast(uint32_t, f); }
float from_bits(uint32_t u) { return __builtin_bit_cast(float, u); }
// A comparison's result: 1.0 / 0.0, or all ones / zero for a program made from DXBC.
float truth(bool t, bool mask) { return mask ? from_bits(t ? 0xFFFFFFFFu : 0u) : (t ? 1.0f : 0.0f); }
int32_t float_to_int(float x) {
  if (!(x == x)) return 0;
  if (x >= 2147483648.0f) return 2147483647;
  if (x <= -2147483648.0f) return -2147483647 - 1;
  return static_cast<int32_t>(x);
}
uint32_t float_to_uint(float x) {
  if (!(x == x) || x <= 0) return 0;
  if (x >= 4294967296.0f) return 0xFFFFFFFFu;
  return static_cast<uint32_t>(x);
}

}  // namespace


struct D3d9Program::State {
  Q r[kMaxTemps];
  Q a0;
  Q pos;
  Q var[graphics::kMaxVaryings];
  Q color[graphics::kMaxColorTargets];
  const graphics::Vec4* attributes = nullptr;
  const graphics::Vec4* varyings = nullptr;
  const uint8_t* constants = nullptr;
  const graphics::ITextureSet* textures = nullptr;
  const graphics::ComputeContext* compute = nullptr;
  const graphics::GeometryInputData* geometry_in = nullptr;
  const graphics::HullInputData* hull_in = nullptr;        // a hull shader's input control points
  const graphics::HullOutputData* patch_in = nullptr;      // a domain shader's patch (control points and constants)
  Q patch_out[graphics::kMaxPatchRegisters] = {};          // a hull shader's patch constant registers
  graphics::GeometryOutputData* geometry_out = nullptr;
  bool geometry_cut = false;   // a cut came after the last emit
  Q sys[kSysValueCount] = {};
  bool killed = false;
  bool at_barrier = false;     // the run stopped at a barrier ...
  uint64_t barrier_pc = 0;     // ... and goes on from this instruction
};

D3d9Program::D3d9Program(allocators::IAllocator& allocator) : allocator_(allocator), code_(allocator), defs_(allocator), bools_(allocator) {}
D3d9Program::~D3d9Program() {}

// --- Compiling ----------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

struct OpInfo {
  uint16_t op;
  uint8_t dst, src;   // how many (src: the least; Shader Model 2+ streams say how many there really are)
};
constexpr OpInfo kOps[] = {
    {kNop, 0, 0}, {kMov, 1, 1}, {kAdd, 1, 2}, {kSub, 1, 2}, {kMad, 1, 3}, {kMul, 1, 2}, {kRcp, 1, 1}, {kRsq, 1, 1}, {kDp3, 1, 2}, {kDp4, 1, 2}, {kMin, 1, 2}, {kMax, 1, 2},
    {kSlt, 1, 2}, {kSge, 1, 2}, {kExp, 1, 1}, {kLog, 1, 1}, {kLit, 1, 1}, {kDst, 1, 2}, {kLrp, 1, 3}, {kFrc, 1, 1}, {kM4x4, 1, 2}, {kM4x3, 1, 2}, {kM3x4, 1, 2}, {kM3x3, 1, 2},
    {kM3x2, 1, 2}, {kPow, 1, 2}, {kCrs, 1, 2}, {kSgn, 1, 1}, {kAbs, 1, 1}, {kNrm, 1, 1}, {kSinCos, 1, 1}, {kIf, 0, 1}, {kIfc, 0, 2}, {kElse, 0, 0}, {kEndIf, 0, 0},
    {kMova, 1, 1}, {kTexKill, 1, 0}, {kTexLd, 1, 2}, {kCmp, 1, 3}, {kDp2Add, 1, 3},
};

const OpInfo* find_op(uint16_t op) {
  for (const OpInfo& i : kOps)
    if (i.op == op) return &i;
  return nullptr;
}

uint8_t reg_type(uint32_t token) { return static_cast<uint8_t>(((token >> 28) & 7) | ((token >> 8) & 0x18)); }

// What a pixel shader's input, or a vertex shader's output, with this meaning is called between the stages: one of the four varyings (see D3d9Shader.hpp).
int8_t semantic_slot(uint32_t usage, uint32_t index) {
  if (usage == kUsageColor && index < 2) return static_cast<int8_t>(index);
  if (usage == kUsageTexCoord && index < 2) return static_cast<int8_t>(2 + index);
  return kSlotNone;
}

}  // namespace

uint32_t D3d9Program::required_features() const {
  uint32_t kinds = 0;
  for (graphics::TextureKind k : texture_kind_)
    if (k != graphics::TextureKind::Texture2D) kinds = graphics::kFeatureTextureKinds;
  if (stage_ == graphics::ShaderStage::Compute) kinds |= graphics::kFeatureCompute;
  if (stage_ == graphics::ShaderStage::Geometry) kinds |= graphics::kFeatureGeometryShaders;
  if (stage_ == graphics::ShaderStage::Hull || stage_ == graphics::ShaderStage::Domain) kinds |= graphics::kFeatureTessellation;
  return kinds | (uses_system_values_ ? graphics::kFeatureSystemInputs : 0u) | (uses_texel_access_ ? graphics::kFeatureTexelAccess : 0u) |
         (color_outputs_ > 1 ? graphics::kFeatureMultipleTargets : 0u);
}

bool d3d9_op_writes_register(uint16_t op) {
  const OpInfo* info = find_op(op);
  return info && info->dst != 0;
}

int d3d9_op_parameter_tokens(uint16_t op) {
  if (op == kDcl) return 2;
  if (op == kDef) return 5;
  const OpInfo* info = find_op(op);
  return info ? info->dst + info->src : -1;
}

NtStatus D3d9Program::compile(allocators::IAllocator& allocator, ConstByteView bytes, UniquePointer<D3d9Program>* out) {
  using Instr = Instruction;
  if (bytes.size() < 8 || bytes.size() % 4 || bytes.size() / 4 > kMaxTokens) return status::kInvalidParameter;
  UniquePointer<D3d9Program> program = UniquePointer<D3d9Program>::make(allocator, allocator);
  if (!program) return status::kNoMemory;
  ByteReader in(bytes);
  const uint32_t version = in.le32();
  const uint32_t kind = version >> 16;
  if (kind != 0xFFFE && kind != 0xFFFF) return status::kInvalidParameter;
  const bool vertex = kind == 0xFFFE;
  if (!vertex && ((version >> 8) & 0xFF) == 1) return compile_ps1(allocator, bytes, out);
  program->stage_ = vertex ? graphics::ShaderStage::Vertex : graphics::ShaderStage::Fragment;
  const uint32_t major = (version >> 8) & 0xFF, minor = version & 0xFF;
  program->major_ = major;
  program->minor_ = minor;
  if (vertex ? !((major == 1 && minor == 1) || major == 2 || major == 3) : !(major == 2 || major == 3)) return status::kNotSupported;

  // Shader Model 3 declares the meaning of its inputs / outputs; the older ones have fixed registers.
  int8_t decl_slot[kMaxOutputs];
  for (int8_t& s : decl_slot) s = kSlotNone;
  List<uint32_t> open_ifs(allocator);   // instruction indices of the if / else being open
  bool ended = false;

  auto read_src = [&](ByteReader& p, Instr::Src* s) -> NtStatus {
    const uint32_t t = p.le32();
    if (!p.ok()) return status::kInvalidParameter;
    s->type = reg_type(t);
    s->number = static_cast<uint16_t>(t & 0x7FF);
    for (uint32_t i = 0; i < 4; ++i) s->swizzle[i] = static_cast<uint8_t>((t >> (16 + 2 * i)) & 3);
    s->modifier = static_cast<uint8_t>((t >> 24) & 0xF);
    if (s->modifier == kModDz || s->modifier == kModDw || s->modifier == kModNot || s->modifier > kModNot) return status::kNotSupported;
    s->relative = ((t >> 13) & 1) != 0;
    if (s->relative) {
      if (!vertex || s->type != kConst) return status::kNotSupported;
      if (major >= 2) {
        const uint32_t a = p.le32();
        if (!p.ok() || reg_type(a) != kAddrTex || (a & 0x7FF) != 0) return status::kNotSupported;
        s->relative_component = static_cast<uint8_t>((a >> 16) & 3);
      }
    }
    switch (s->type) {
      case kTemp: return s->number < kMaxTemps ? status::kSuccess : status::kInvalidParameter;
      case kConst: return status::kSuccess;   // (checked against the defs at the end)
      case kInput:
        if (s->number >= kMaxInputs) return status::kInvalidParameter;
        if (vertex) {
          if (s->number >= graphics::kMaxVertexAttributes) return status::kNotSupported;
          s->slot = static_cast<int8_t>(s->number);
        } else if (major >= 3) {
          s->slot = decl_slot[s->number];
          if (s->slot < 0) return status::kNotSupported;
        } else {
          if (s->number >= 2) return status::kNotSupported;
          s->slot = static_cast<int8_t>(s->number);
        }
        return status::kSuccess;
      case kAddrTex:
        if (vertex) return s->number == 0 ? status::kSuccess : status::kInvalidParameter;
        if (major >= 3 || s->number >= 2) return status::kNotSupported;
        s->slot = static_cast<int8_t>(2 + s->number);
        return status::kSuccess;
      case kSampler: return s->number < graphics::kMaxTextureUnits ? status::kSuccess : status::kNotSupported;
      case kConstBool: return status::kSuccess;
      default: return status::kNotSupported;
    }
  };

  auto read_dst = [&](ByteReader& p, Instr::Dst* d) -> NtStatus {
    const uint32_t t = p.le32();
    if (!p.ok()) return status::kInvalidParameter;
    d->type = reg_type(t);
    d->number = static_cast<uint16_t>(t & 0x7FF);
    if ((t >> 13) & 1) return status::kNotSupported;   // relative addressing of an output
    d->mask = static_cast<uint8_t>((t >> 16) & 0xF);
    const uint32_t mod = (t >> 20) & 0xF;   // (1 saturate; partial precision and centroid change nothing here)
    d->saturate = (mod & 1) != 0;
    int32_t shift = static_cast<int32_t>((t >> 24) & 0xF);
    if (shift >= 8) shift -= 16;
    d->shift = static_cast<int8_t>(shift);
    switch (d->type) {
      case kTemp: return d->number < kMaxTemps ? status::kSuccess : status::kInvalidParameter;
      case kAddrTex:
        if (vertex && d->number == 0) return status::kSuccess;
        return status::kNotSupported;
      case kRastOut:
        if (!vertex || d->number != 0) return status::kNotSupported;   // fog and point size
        d->slot = kSlotPosition;
        return status::kSuccess;
      case kAttrOut:
        if (!vertex || major >= 3 || d->number >= 2) return status::kNotSupported;
        d->slot = static_cast<int8_t>(d->number);
        return status::kSuccess;
      case kTexCrdOut:   // (Shader Model 3: the generic output register)
        if (!vertex || d->number >= kMaxOutputs) return status::kNotSupported;
        if (major >= 3) {
          d->slot = decl_slot[d->number];
          return d->slot == kSlotNone ? status::kNotSupported : status::kSuccess;
        }
        if (d->number >= 2) return status::kNotSupported;
        d->slot = static_cast<int8_t>(2 + d->number);
        return status::kSuccess;
      case kColorOut:
        if (vertex || d->number != 0) return status::kNotSupported;
        d->slot = 0;
        return status::kSuccess;
      default: return status::kNotSupported;
    }
  };

  while (!in.at_end() && !ended) {
    const uint32_t token = in.le32();
    if (!in.ok()) return status::kInvalidParameter;
    const uint16_t op = static_cast<uint16_t>(token & 0xFFFF);
    if (token == 0x0000FFFF) {
      ended = true;
      break;
    }
    if (op == kComment) {
      if (!in.skip(uint64_t{(token >> 16) & 0x7FFF} * 4)) return status::kInvalidParameter;
      continue;
    }
    if (op == kDcl) {
      const uint32_t usage_token = in.le32();
      const uint32_t reg = in.le32();
      if (!in.ok()) return status::kInvalidParameter;
      const uint8_t type = reg_type(reg);
      const uint16_t number = static_cast<uint16_t>(reg & 0x7FF);
      const uint32_t usage = usage_token & 0x1F, index = (usage_token >> 16) & 0xF;
      if (type == kSampler) {
        if (number >= graphics::kMaxTextureUnits || ((usage_token >> 27) & 0xF) != 2) return status::kNotSupported;   // 2D only
      } else if (major >= 3 && ((vertex && type == kTexCrdOut) || (!vertex && type == kInput))) {
        if (number >= kMaxOutputs) return status::kInvalidParameter;
        if (vertex && usage == kUsagePosition && index == 0) decl_slot[number] = kSlotPosition;
        else decl_slot[number] = semantic_slot(usage, index);
        if (decl_slot[number] == kSlotNone) return status::kNotSupported;
      }
      continue;
    }
    if (op == kDef) {
      const uint32_t reg = in.le32();
      float v[4];
      for (float& f : v) f = __builtin_bit_cast(float, in.le32());
      if (!in.ok() || reg_type(reg) != kConst) return status::kNotSupported;
      if (program->defs_.count() >= 256 || !program->defs_.push_back({static_cast<uint16_t>(reg & 0x7FF), {v[0], v[1], v[2], v[3]}})) return status::kNoMemory;
      continue;
    }
    if (op == kDefB) {
      const uint32_t reg = in.le32(), value = in.le32();
      if (!in.ok() || reg_type(reg) != kConstBool) return status::kNotSupported;
      if (!program->bools_.push_back({static_cast<uint16_t>(reg & 0x7FF), (value & 1) != 0})) return status::kNoMemory;
      continue;
    }
    const OpInfo* info = find_op(op);
    if (!info) return status::kNotSupported;
    uint32_t params = info->dst + info->src;
    if (major >= 2) {
      params = (token >> 24) & 0xF;
      if (params < info->dst + info->src || params > 5u) return status::kInvalidParameter;
    }
    if (program->code_.count() >= kMaxInstructions) return status::kInvalidParameter;
    ByteReader p(in.rest());
    Instr ins;
    ins.op = op;
    ins.control = static_cast<uint8_t>((token >> 16) & 0xFF);
    NtStatus s = status::kSuccess;
    if (info->dst) {
      if (op == kTexKill) {
        // texkill names a register to test, written like a destination.
        const uint32_t t = p.le32();
        if (!p.ok()) return status::kInvalidParameter;
        ins.src[0].type = reg_type(t);
        ins.src[0].number = static_cast<uint16_t>(t & 0x7FF);
        if (vertex || (ins.src[0].type != kTemp && ins.src[0].type != kAddrTex) || ins.src[0].number >= kMaxTemps) return status::kNotSupported;
        if (ins.src[0].type == kAddrTex) {
          if (major >= 3 || ins.src[0].number >= 2) return status::kNotSupported;
          ins.src[0].slot = static_cast<int8_t>(2 + ins.src[0].number);
        }
        ins.src_count = 1;
      } else {
        s = read_dst(p, &ins.dst);
        if (s != status::kSuccess) return s;
      }
    }
    // The sources: the ones the instruction uses are decoded; extra ones (sincos has two more in Shader Model 2) are read and dropped.
    uint32_t remaining_src = params - info->dst;
    if (major < 2) remaining_src = info->src;
    if (op == kTexKill) remaining_src = 0;
    uint32_t read = 0;
    for (uint32_t i = 0; i < remaining_src; ++i) {
      // (a relatively addressed source is followed by the token of its address register, which the length counts too)
      if (major >= 2 && p.position() >= uint64_t{params} * 4) break;
      Instr::Src tmp;
      s = read_src(p, &tmp);
      if (s != status::kSuccess) return s;
      if (i < 3) ins.src[i] = tmp;
      ++read;
    }
    if (read < (op == kTexKill ? 0u : info->src)) return status::kInvalidParameter;
    if (op != kTexKill) ins.src_count = static_cast<uint8_t>(read < 3 ? read : 3);
    // Whatever the length said must be all there was (the reader is a copy: advance the real one).
    const uint64_t used = p.position();
    if (!p.ok() || !in.skip(used)) return status::kInvalidParameter;
    if (major >= 2 && used != uint64_t{params} * 4) return status::kInvalidParameter;

    if (op == kTexLd) {
      if (vertex || ins.src[1].type != kSampler) return status::kNotSupported;
      program->uses_textures_ = true;
    }
    if (op == kIf) {
      if (ins.src[0].type != kConstBool) return status::kNotSupported;
    }
    const uint32_t index = static_cast<uint32_t>(program->code_.count());
    if (op == kIf || op == kIfc) {
      if (open_ifs.count() >= 24 || !open_ifs.push_back(index)) return status::kInvalidParameter;
    } else if (op == kElse) {
      if (!open_ifs.count()) return status::kInvalidParameter;
      Instr& open = program->code_[open_ifs[open_ifs.count() - 1]];
      if (open.op == kElse) return status::kInvalidParameter;
      open.target = index + 1;   // a failed condition continues after the else
      open_ifs.remove_at(open_ifs.count() - 1);
      open_ifs.push_back(index);
    } else if (op == kEndIf) {
      if (!open_ifs.count()) return status::kInvalidParameter;
      Instr& open = program->code_[open_ifs[open_ifs.count() - 1]];
      open.target = index + 1;
      open_ifs.remove_at(open_ifs.count() - 1);
    }
    if (!program->code_.push_back(ins)) return status::kNoMemory;
  }
  if (!ended || open_ifs.count()) return status::kInvalidParameter;

  // Constants the program reads without a relative address must exist: a push constant register or one it defines.
  for (uint64_t i = 0; i < program->code_.count(); ++i) {
    const Instr& ins = program->code_[i];
    const uint32_t reach = ins.op == kM4x4 || ins.op == kM4x3 ? 4 : ins.op == kM3x4 || ins.op == kM3x3 ? 3 : ins.op == kM3x2 ? 2 : 1;
    for (uint32_t k = 0; k < ins.src_count; ++k) {
      const Instr::Src& s = ins.src[k];
      if (s.type != kConst || s.relative) continue;
      const uint32_t n = (k == 1 && reach > 1) ? reach : 1;
      for (uint32_t j = 0; j < n; ++j) {
        const uint32_t reg = s.number + j;
        bool defined = reg < kPushConstantRegisters;
        for (const Def& d : program->defs_.view())
          if (d.number == reg) defined = true;
        if (!defined) return status::kNotSupported;
      }
    }
    if (ins.op == kIf) {
      bool defined = false;
      for (const DefBool& b : program->bools_.view())
        if (b.number == ins.src[0].number) defined = true;
      if (!defined) return status::kNotSupported;
    }
  }
  if (vertex) {
    // A vertex shader that never writes the position draws nothing useful.
    bool writes_position = false;
    for (const Instr& ins : program->code_.view())
      if (ins.dst.slot == kSlotPosition) writes_position = true;
    if (!writes_position) return status::kInvalidParameter;
  }
  *out = util::move(program);
  return status::kSuccess;
}

// --- Running --------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

Q splat(float v) { return {{v, v, v, v}}; }

float apply_modifier(uint8_t m, float x) {
  switch (m) {
    case kModNeg: return -x;
    case kModBias: return x - 0.5f;
    case kModBiasNeg: return -(x - 0.5f);
    case kModSign: return 2.0f * (x - 0.5f);
    case kModSignNeg: return -2.0f * (x - 0.5f);
    case kModComp: return 1.0f - x;
    case kModX2: return 2.0f * x;
    case kModX2Neg: return -2.0f * x;
    case kModAbs: return fabs_(x);
    case kModAbsNeg: return -fabs_(x);
    default: return x;
  }
}

}  // namespace

void D3d9Program::execute(State& s, uint64_t begin, uint64_t end) const {
  using Instr = Instruction;
  auto push_constant = [&](uint32_t index) {
    Q q = {{0, 0, 0, 0}};
    if (index < constant_registers_ && s.constants) __builtin_memcpy(q.f, s.constants + index * 16, 16);
    return q;
  };
  auto constant = [&](uint32_t index) {
    for (const Def& d : defs_.view())
      if (d.number == index) return Q{{d.value[0], d.value[1], d.value[2], d.value[3]}};
    return push_constant(index);
  };
  bool int_mode = false;   // the instruction being run works on integers: negate and abs of its sources are integer ones
  auto fetch = [&](const Instr::Src& src, uint32_t offset) {
    Q v = {{0, 0, 0, 0}};
    switch (src.type) {
      case kTemp: v = s.r[src.number]; break;
      case kSysValue: if (src.number < kSysValueCount) v = s.sys[src.number]; break;
      case kPatchIn:
        if (s.patch_in && src.number < graphics::kMaxPatchRegisters) v = {{s.patch_in->patch[src.number].x, s.patch_in->patch[src.number].y, s.patch_in->patch[src.number].z, s.patch_in->patch[src.number].w}};
        break;
      case kInput:
        if (s.hull_in || s.patch_in) {
          // a hull shader's input control point, or a domain shader's (the hull shader's output) one; the control point being made when the index is vOutputControlPointID
          uint32_t cp = src.vertex;
          if (src.vertex_from_id) cp = bits_of(s.sys[kSysOutputControlPointId].f[0]);
          if (cp < graphics::kMaxPatchControlPoints) {
            graphics::Vec4 a = {0, 0, 0, 0};
            if (s.hull_in) {
              if (src.slot == kSlotPosition) a = s.hull_in->position[cp];
              else if (src.slot >= 0 && src.slot < static_cast<int8_t>(graphics::kMaxVaryings)) a = s.hull_in->varying[cp][src.slot];
            } else {
              if (src.slot == kSlotPosition) a = s.patch_in->control_point[cp].position;
              else if (src.slot >= 0 && src.slot < static_cast<int8_t>(graphics::kMaxVaryings)) a = s.patch_in->control_point[cp].varying[src.slot];
            }
            v = {{a.x, a.y, a.z, a.w}};
          }
          break;
        }
        if (s.geometry_in) {
          // (a geometry shader: the position or a varying of one vertex of the primitive)
          if (src.vertex < graphics::kMaxGeometryInputVertices && src.slot == kSlotPosition) v = {{s.geometry_in->position[src.vertex].x, s.geometry_in->position[src.vertex].y, s.geometry_in->position[src.vertex].z, s.geometry_in->position[src.vertex].w}};
          else if (src.vertex < graphics::kMaxGeometryInputVertices && src.slot >= 0 && src.slot < static_cast<int8_t>(graphics::kMaxVaryings)) {
            const graphics::Vec4 a = s.geometry_in->varying[src.vertex][src.slot];
            v = {{a.x, a.y, a.z, a.w}};
          }
          break;
        }
        if (src.slot >= 0) {
          const graphics::Vec4 a = s.attributes ? s.attributes[src.slot] : s.varyings[src.slot];
          v = {{a.x, a.y, a.z, a.w}};
        }
        break;
      case kAddrTex:
        if (src.slot >= 0) {
          const graphics::Vec4 a = s.varyings[src.slot];
          v = {{a.x, a.y, a.z, a.w}};
        } else {
          v = s.a0;
        }
        break;
      case kConst: {
        int64_t index = int64_t{src.number} + offset;
        bool in_range = true;
        if (src.relative) {
          if (src.relative_temp >= 0) index += __builtin_bit_cast(int32_t, s.r[src.relative_temp].f[src.relative_component]);   // (an integer register)
          else index += static_cast<int64_t>(floor_(s.a0.f[src.relative_component]));
          in_range = !src.relative_limit || (index >= 0 && index < src.relative_limit);   // (past the end of a constant buffer: zero)
        }
        v = (index < 0 || !in_range) ? Q{{0, 0, 0, 0}} : constant(static_cast<uint32_t>(index > 0x7FFF ? 0x7FFF : index));
        break;
      }
      default: break;
    }
    Q r = {{v.f[src.swizzle[0]], v.f[src.swizzle[1]], v.f[src.swizzle[2]], v.f[src.swizzle[3]]}};
    if (src.modifier != kModNone) {
      for (float& c : r.f) {
        if (int_mode && (src.modifier == kModNeg || src.modifier == kModAbs || src.modifier == kModAbsNeg)) {
          int32_t x = __builtin_bit_cast(int32_t, c);
          if (src.modifier != kModNeg && x < 0) x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
          if (src.modifier != kModAbs) x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
          c = __builtin_bit_cast(float, x);
        } else {
          c = apply_modifier(src.modifier, c);
        }
      }
    }
    return r;
  };
  auto store = [&](const Instr::Dst& d, Q v) {
    if (d.shift) {
      const float scale = d.shift > 0 ? static_cast<float>(1u << d.shift) : 1.0f / static_cast<float>(1u << -d.shift);
      for (float& c : v.f) c *= scale;
    }
    if (d.saturate)
      for (float& c : v.f) c = sat_(c);
    Q* target = nullptr;
    switch (d.type) {
      case kTemp: target = &s.r[d.number]; break;
      case kAddrTex: target = &s.a0; break;
      case kPatchOut: target = d.number < graphics::kMaxPatchRegisters ? &s.patch_out[d.number] : nullptr; break;
      case kRastOut: target = &s.pos; break;
      case kAttrOut:
      case kTexCrdOut: target = d.slot == kSlotPosition ? &s.pos : d.slot >= 0 ? &s.var[d.slot] : nullptr; break;
      case kColorOut: target = d.slot >= 0 && d.slot < static_cast<int8_t>(graphics::kMaxColorTargets) ? &s.color[d.slot] : nullptr; break;
      default: break;
    }
    if (!target) return;
    for (uint32_t i = 0; i < 4; ++i)
      if (d.mask & (1u << i)) target->f[i] = v.f[i];
  };
  auto dot = [](const Q& a, const Q& b, uint32_t n) {
    float sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum += a.f[i] * b.f[i];
    return sum;
  };
  auto compare = [](uint8_t mode, float a, float b) {
    switch (mode) {
      case 1: return a > b;
      case 2: return a == b;
      case 3: return a >= b;
      case 4: return a < b;
      case 5: return a != b;
      default: return a <= b;
    }
  };

  const uint64_t count = code_.count() < end ? code_.count() : end;
  uint32_t steps = 0;   // (a loop that never ends stops the program, not the machine)
  for (uint64_t pc = begin; pc < count;) {
    if (++steps > kMaxSteps) return;
    const Instr& ins = code_[pc++];
    int_mode = ins.op >= kIntegerFirst && ins.op <= kIntegerLast;
    Q a = {{0, 0, 0, 0}}, b = a, c = a, r = a;
    if (ins.src_count > 0 && ins.op != kM4x4 && ins.op != kM4x3 && ins.op != kM3x4 && ins.op != kM3x3 && ins.op != kM3x2) a = fetch(ins.src[0], 0);
    switch (ins.op) {
      case kNop: case kElse: case kEndIf: break;
      case kIf: {
        bool value = false;
        for (const DefBool& d : bools_.view())
          if (d.number == ins.src[0].number) value = d.value;
        if (!value) pc = ins.target;
        break;
      }
      case kIfc: {
        b = fetch(ins.src[1], 0);
        if (!compare(ins.control & 7, a.f[0], b.f[0])) pc = ins.target;
        break;
      }
      case kBreakNz: case kContinueNz:
        if (bits_of(a.f[0]) != 0) pc = ins.target;
        break;
      case kBreakZ: case kContinueZ:
        if (bits_of(a.f[0]) == 0) pc = ins.target;
        break;
      case kBreak0: case kContinue0: case kLoopEnd:
        pc = ins.target;
        break;
      case kIfNz:
        if (bits_of(a.f[0]) == 0) pc = ins.target;
        break;
      case kIfZ:
        if (bits_of(a.f[0]) != 0) pc = ins.target;
        break;
      case kDiscardNz:
      case kDiscardZ:
        if ((bits_of(a.f[0]) != 0) == (ins.op == kDiscardNz)) {
          s.killed = true;
          return;
        }
        break;
      default: break;
    }
    switch (ins.op) {
      case kMov: store(ins.dst, a); break;
      case kMova: for (float& x : a.f) x = floor_(x + 0.5f); store(ins.dst, a); break;
      case kAdd: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] + b.f[i]; store(ins.dst, r); break;
      case kSub: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] - b.f[i]; store(ins.dst, r); break;
      case kMul: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] * b.f[i]; store(ins.dst, r); break;
      case kMad: b = fetch(ins.src[1], 0); c = fetch(ins.src[2], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] * b.f[i] + c.f[i]; store(ins.dst, r); break;
      case kLrp: b = fetch(ins.src[1], 0); c = fetch(ins.src[2], 0); for (int i = 0; i < 4; ++i) r.f[i] = c.f[i] + a.f[i] * (b.f[i] - c.f[i]); store(ins.dst, r); break;
      case kMin: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] < b.f[i] ? a.f[i] : b.f[i]; store(ins.dst, r); break;
      case kMax: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] > b.f[i] ? a.f[i] : b.f[i]; store(ins.dst, r); break;
      case kSlt: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = truth(a.f[i] < b.f[i], ins.mask_result); store(ins.dst, r); break;
      case kSge: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = truth(a.f[i] >= b.f[i], ins.mask_result); store(ins.dst, r); break;
      case kCmp: b = fetch(ins.src[1], 0); c = fetch(ins.src[2], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] >= 0 ? b.f[i] : c.f[i]; store(ins.dst, r); break;
      case kRcp: store(ins.dst, splat(rcp_(a.f[0]))); break;
      case kRsq:
        for (int i = 0; i < 4; ++i) {
          const float x = ins.vector ? a.f[i] : a.f[0];
          r.f[i] = x == 0 ? rcp_(0.0f) : 1.0f / sqrt_(fabs_(x));
        }
        store(ins.dst, r);
        break;
      case kDp3: b = fetch(ins.src[1], 0); store(ins.dst, splat(dot(a, b, 3))); break;
      case kDp4: b = fetch(ins.src[1], 0); store(ins.dst, splat(dot(a, b, 4))); break;
      case kGsEmit: {
        graphics::GeometryOutputData* out = s.geometry_out;
        if (!out || out->count >= geometry_max_vertices_ || out->count >= graphics::kMaxGeometryVertices) break;   // (past the maximum: dropped)
        graphics::ShaderIo& v = out->vertex[out->count];
        v.position = {s.pos.f[0], -s.pos.f[1], s.pos.f[2], s.pos.f[3]};
        for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) v.varying[i] = {s.var[i].f[0], s.var[i].f[1], s.var[i].f[2], s.var[i].f[3]};
        out->restart[out->count] = s.geometry_cut;
        s.geometry_cut = false;
        ++out->count;
        break;
      }
      case kBarrier:
        s.at_barrier = true;
        s.barrier_pc = pc;
        return;
      case kGsCut:
        if (s.geometry_out && s.geometry_out->count) s.geometry_cut = true;
        break;
      case kBufLoad: case kBufStore: case kBufAtomic: case kBufImmAtomic: {
        if (!s.compute || ins.uav >= graphics::kMaxStorageBuffers + graphics::kMaxSharedMemories) break;
        const ByteView buffer = ins.uav < graphics::kMaxStorageBuffers ? s.compute->buffers[ins.uav] : s.compute->shared[ins.uav - graphics::kMaxStorageBuffers];
        // The byte address: a raw buffer's is the address; a structured one's the structure number times the stride plus the offset (an atomic gives both in one register).
        uint64_t address = bits_of(a.f[0]);
        if (ins.stride) {
          const uint32_t offset = ins.op == kBufLoad || ins.op == kBufStore ? bits_of(fetch(ins.src[1], 0).f[0]) : bits_of(a.f[1]);
          address = address * ins.stride + offset;
        }
        auto word_at = [&](uint64_t at, uint32_t* out) {
          ConstByteView part;
          if (!ConstByteView(buffer.data(), buffer.size()).try_subview(at, 4, &part)) return false;
          *out = ByteReader(part).le32();
          return true;
        };
        auto put_word = [&](uint64_t at, uint32_t value) {
          ByteView part;
          if (!buffer.try_subview(at, 4, &part)) return;   // (a write outside the buffer is dropped)
          ByteWriter(part).le32(value);
        };
        if (ins.op == kBufLoad) {
          Q words = {{0, 0, 0, 0}};
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t w = 0;
            if (word_at(address + 4ull * k, &w)) words.f[k] = from_bits(w);   // (outside the buffer: zero)
          }
          for (uint32_t k = 0; k < 4; ++k) r.f[k] = words.f[ins.tex_swizzle[k] & 3];
          store(ins.dst, r);
        } else if (ins.op == kBufStore) {
          const Q value = fetch(ins.src[2], 0);
          uint32_t written = 0;
          for (uint32_t k = 0; k < 4; ++k)
            if (ins.dst.mask & (1u << k)) put_word(address + 4ull * written++, bits_of(value.f[k]));   // (the words packed one after the other)
        } else {
          const uint32_t operand = bits_of(fetch(ins.src[1], 0).f[0]);
          uint32_t old = 0;
          if (!word_at(address, &old)) break;
          uint32_t updated = old;
          switch (ins.control) {
            case kAtomicAdd: updated = old + operand; break;
            case kAtomicAnd: updated = old & operand; break;
            case kAtomicOr: updated = old | operand; break;
            case kAtomicXor: updated = old ^ operand; break;
            case kAtomicIMax: updated = static_cast<int32_t>(operand) > static_cast<int32_t>(old) ? operand : old; break;
            case kAtomicIMin: updated = static_cast<int32_t>(operand) < static_cast<int32_t>(old) ? operand : old; break;
            case kAtomicUMax: updated = operand > old ? operand : old; break;
            case kAtomicUMin: updated = operand < old ? operand : old; break;
            default: updated = operand; break;
          }
          put_word(address, updated);
          if (ins.op == kBufImmAtomic) store(ins.dst, splat(from_bits(old)));
        }
        break;
      }
      case kDp2Add: b = fetch(ins.src[1], 0); c = fetch(ins.src[2], 0); store(ins.dst, splat(dot(a, b, 2) + c.f[0])); break;
      case kExp:
        for (int i = 0; i < 4; ++i) r.f[i] = static_cast<float>(fmath::exp(static_cast<double>(ins.vector ? a.f[i] : a.f[0]) * 0.6931471805599453));
        store(ins.dst, r);
        break;
      case kLog:
        for (int i = 0; i < 4; ++i) {
          const float x = fabs_(ins.vector ? a.f[i] : a.f[0]);
          r.f[i] = x == 0 ? -rcp_(0.0f) : static_cast<float>(fmath::ln(x) * 1.4426950408889634);
        }
        store(ins.dst, r);
        break;
      case kDiv: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] / b.f[i]; store(ins.dst, r); break;
      case kSqrt: for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] < 0 ? __builtin_nanf("") : sqrt_(a.f[i]); store(ins.dst, r); break;
      case kSeq: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = truth(a.f[i] == b.f[i], ins.mask_result); store(ins.dst, r); break;
      case kSne: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = truth(a.f[i] != b.f[i], ins.mask_result); store(ins.dst, r); break;
      case kDp2: b = fetch(ins.src[1], 0); store(ins.dst, splat(dot(a, b, 2))); break;
      case kFloor: for (int i = 0; i < 4; ++i) r.f[i] = floor_(a.f[i]); store(ins.dst, r); break;
      case kCeil: for (int i = 0; i < 4; ++i) r.f[i] = -floor_(-a.f[i]); store(ins.dst, r); break;
      case kTrunc: for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] < 0 ? -floor_(-a.f[i]) : floor_(a.f[i]); store(ins.dst, r); break;
      case kRoundNe:
        for (int i = 0; i < 4; ++i) {
          const float f = floor_(a.f[i]), d = a.f[i] - f;
          r.f[i] = d < 0.5f ? f : d > 0.5f ? f + 1.0f : (floor_(f * 0.5f) * 2.0f == f ? f : f + 1.0f);
        }
        store(ins.dst, r);
        break;
      case kSin: for (int i = 0; i < 4; ++i) r.f[i] = static_cast<float>(fmath::sin(a.f[i])); store(ins.dst, r); break;
      case kCos: for (int i = 0; i < 4; ++i) r.f[i] = static_cast<float>(fmath::cos(a.f[i])); store(ins.dst, r); break;
      case kSelect: b = fetch(ins.src[1], 0); c = fetch(ins.src[2], 0); for (int i = 0; i < 4; ++i) r.f[i] = bits_of(a.f[i]) != 0 ? b.f[i] : c.f[i]; store(ins.dst, r); break;
      case kFrc: for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] - floor_(a.f[i]); store(ins.dst, r); break;
      case kAbs: for (int i = 0; i < 4; ++i) r.f[i] = fabs_(a.f[i]); store(ins.dst, r); break;
      case kSgn: for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] > 0 ? 1.0f : a.f[i] < 0 ? -1.0f : 0.0f; store(ins.dst, r); break;
      case kPow: {
        b = fetch(ins.src[1], 0);
        const float base = fabs_(a.f[0]);
        store(ins.dst, splat(b.f[0] == 0 ? 1.0f : base == 0 ? 0.0f : static_cast<float>(fmath::pow(base, b.f[0]))));
        break;
      }
      case kCrs: {
        b = fetch(ins.src[1], 0);
        r.f[0] = a.f[1] * b.f[2] - a.f[2] * b.f[1];
        r.f[1] = a.f[2] * b.f[0] - a.f[0] * b.f[2];
        r.f[2] = a.f[0] * b.f[1] - a.f[1] * b.f[0];
        store(ins.dst, r);
        break;
      }
      case kNrm: {
        const float len2 = dot(a, a, 3);
        const float scale = len2 == 0 ? 0.0f : 1.0f / sqrt_(len2);
        for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] * scale;
        store(ins.dst, r);
        break;
      }
      case kSinCos:
        r.f[0] = static_cast<float>(fmath::cos(a.f[0]));
        r.f[1] = static_cast<float>(fmath::sin(a.f[0]));
        store(ins.dst, r);
        break;
      case kLit: {
        const float w = a.f[3] < -127.9961f ? -127.9961f : a.f[3] > 127.9961f ? 127.9961f : a.f[3];
        r = {{1.0f, a.f[0] > 0 ? a.f[0] : 0.0f, 0.0f, 1.0f}};
        if (a.f[0] > 0 && a.f[1] > 0) r.f[2] = static_cast<float>(fmath::pow(a.f[1], w));
        store(ins.dst, r);
        break;
      }
      case kDst: b = fetch(ins.src[1], 0); store(ins.dst, Q{{1.0f, a.f[1] * b.f[1], a.f[2], b.f[3]}}); break;
      case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: {
        const uint32_t rows = ins.op == kM4x4 || ins.op == kM3x4 ? 4 : ins.op == kM4x3 || ins.op == kM3x3 ? 3 : 2;
        const uint32_t width = ins.op == kM4x4 || ins.op == kM4x3 ? 4 : 3;
        a = fetch(ins.src[0], 0);
        for (uint32_t i = 0; i < rows; ++i) r.f[i] = dot(a, fetch(ins.src[1], i), width);
        store(ins.dst, r);
        break;
      }
      case kIAdd: case kINeg: case kIMul: case kIMad: case kIMin: case kIMax: case kUMin: case kUMax: case kUMul: case kUMad: case kUDiv: case kURem: case kIShl: case kIShr: case kUShr:
      case kAnd: case kOr: case kXor: case kNot: case kFtoI: case kFtoU: case kItoF: case kUtoF: case kIEq: case kINe: case kILt: case kIGe: case kULt: case kUGe: {
        if (ins.src_count > 1) b = fetch(ins.src[1], 0);
        if (ins.src_count > 2) c = fetch(ins.src[2], 0);
        for (int i = 0; i < 4; ++i) {
          const uint32_t x = bits_of(a.f[i]), y = bits_of(b.f[i]), z = bits_of(c.f[i]);
          const int32_t sx = static_cast<int32_t>(x), sy = static_cast<int32_t>(y);
          uint32_t out = 0;
          switch (ins.op) {
            case kIAdd: out = x + y; break;
            case kINeg: out = 0u - x; break;
            case kIMul: case kUMul: out = x * y; break;
            case kIMad: case kUMad: out = x * y + z; break;
            case kIMin: out = static_cast<uint32_t>(sx < sy ? sx : sy); break;
            case kIMax: out = static_cast<uint32_t>(sx > sy ? sx : sy); break;
            case kUMin: out = x < y ? x : y; break;
            case kUMax: out = x > y ? x : y; break;
            case kUDiv: out = y ? x / y : 0xFFFFFFFFu; break;
            case kURem: out = y ? x % y : 0xFFFFFFFFu; break;
            case kIShl: out = x << (y & 31); break;
            case kIShr: out = static_cast<uint32_t>(sx >> (y & 31)); break;   // (arithmetic)
            case kUShr: out = x >> (y & 31); break;
            case kAnd: out = x & y; break;
            case kOr: out = x | y; break;
            case kXor: out = x ^ y; break;
            case kNot: out = ~x; break;
            case kFtoI: out = static_cast<uint32_t>(float_to_int(a.f[i])); break;
            case kFtoU: out = float_to_uint(a.f[i]); break;
            case kItoF: r.f[i] = static_cast<float>(sx); continue;
            case kUtoF: r.f[i] = static_cast<float>(x); continue;
            case kIEq: out = x == y ? 0xFFFFFFFFu : 0; break;
            case kINe: out = x != y ? 0xFFFFFFFFu : 0; break;
            case kILt: out = sx < sy ? 0xFFFFFFFFu : 0; break;
            case kIGe: out = sx >= sy ? 0xFFFFFFFFu : 0; break;
            case kULt: out = x < y ? 0xFFFFFFFFu : 0; break;
            case kUGe: out = x >= y ? 0xFFFFFFFFu : 0; break;
            default: break;
          }
          r.f[i] = from_bits(out);
        }
        store(ins.dst, r);
        break;
      }
      case kTexLd: {
        b = {{0, 0, 0, 0}};
        const uint32_t unit = ins.src[1].number;
        float u = a.f[0], v = a.f[1];
        if (ins.control & 1) {   // project
          const float inv = a.f[3] == 0 ? 0.0f : 1.0f / a.f[3];
          u *= inv, v *= inv;
        }
        if (s.textures) {
          const graphics::Vec4 t = (unit >= graphics::kMaxTextureUnits || texture_kind_[unit] == graphics::TextureKind::Texture2D) ? s.textures->sample(unit, u, v) : s.textures->sample_volume(unit, u, v, ins.control & 1 ? a.f[2] * (a.f[3] == 0 ? 0.0f : 1.0f / a.f[3]) : a.f[2]);
          const float texel[4] = {t.x, t.y, t.z, t.w};
          b = {{texel[ins.tex_swizzle[0]], texel[ins.tex_swizzle[1]], texel[ins.tex_swizzle[2]], texel[ins.tex_swizzle[3]]}};
        }
        store(ins.dst, b);
        break;
      }
      case kTexFetch: case kTexLod: case kTexSize: {
        b = {{0, 0, 0, 0}};
        const uint32_t unit = ins.src[1].number;
        if (s.textures) {
          graphics::Vec4 t = {0, 0, 0, 0};
          const graphics::TextureKind kind = unit < graphics::kMaxTextureUnits ? texture_kind_[unit] : graphics::TextureKind::Texture2D;
          const bool flat = kind == graphics::TextureKind::Texture2D;
          if (ins.op == kTexFetch) {
            t = flat ? s.textures->fetch(unit, __builtin_bit_cast(int32_t, a.f[0]), __builtin_bit_cast(int32_t, a.f[1]), bits_of(a.f[3]))
                     : s.textures->fetch_volume(unit, __builtin_bit_cast(int32_t, a.f[0]), __builtin_bit_cast(int32_t, a.f[1]), __builtin_bit_cast(int32_t, a.f[2]), bits_of(a.f[3]));
          } else if (ins.op == kTexLod) {
            const Q lod = fetch(ins.src[2], 0);
            t = flat ? s.textures->sample_lod(unit, a.f[0], a.f[1], lod.f[0]) : s.textures->sample_lod_volume(unit, a.f[0], a.f[1], a.f[2], lod.f[0]);
          } else {
            uint32_t w = 0, h = 0, d = 1, levels = 0;
            if (s.textures->size_volume(unit, bits_of(a.f[0]), &w, &h, &d, &levels) && bits_of(a.f[0]) < levels) {
              if (kind == graphics::TextureKind::TextureCube || flat) d = 0;   // (what resinfo leaves in the third component for the kinds with no depth)
              const uint32_t mode = ins.control;
              t = mode == 2 ? graphics::Vec4{from_bits(w), from_bits(h), from_bits(d), from_bits(levels)}
                            : mode == 1 ? graphics::Vec4{1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), d ? 1.0f / static_cast<float>(d) : 0.0f, 1.0f / static_cast<float>(levels)}
                                        : graphics::Vec4{static_cast<float>(w), static_cast<float>(h), static_cast<float>(d), static_cast<float>(levels)};
            }
          }
          const float texel[4] = {t.x, t.y, t.z, t.w};
          b = {{texel[ins.tex_swizzle[0]], texel[ins.tex_swizzle[1]], texel[ins.tex_swizzle[2]], texel[ins.tex_swizzle[3]]}};
        }
        store(ins.dst, b);
        break;
      }
      case kTexKill: {
        const Q t = fetch(ins.src[0], 0);
        if (t.f[0] < 0 || t.f[1] < 0 || t.f[2] < 0) {
          s.killed = true;
          return;
        }
        break;
      }
      default: break;
    }
    if (ins.op == kElse) pc = ins.target;   // reached by running through the true branch: leave the construct
  }
}

void D3d9Program::run_vertex(const graphics::Vec4* attributes, const uint8_t* constants, graphics::ShaderIo* out) const { run_vertex_ex(attributes, constants, graphics::VertexContext(), out); }

void D3d9Program::run_vertex_ex(const graphics::Vec4* attributes, const uint8_t* constants, const graphics::VertexContext& context, graphics::ShaderIo* out) const {
  State s = {};
  s.attributes = attributes;
  s.constants = constants;
  s.sys[kSysVertexId].f[0] = from_bits(context.vertex_id);
  s.sys[kSysInstanceId].f[0] = from_bits(context.instance_id);
  execute(s);
  out->position = {s.pos.f[0], -s.pos.f[1], s.pos.f[2], s.pos.f[3]};   // (Direct3D's +Y up to the device's +Y down)
  for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) out->varying[i] = {s.var[i].f[0], s.var[i].f[1], s.var[i].f[2], s.var[i].f[3]};
}

bool D3d9Program::run_fragment(const graphics::Vec4* varyings, const uint8_t* constants, const graphics::ITextureSet* textures, graphics::Vec4* color) const {
  return run_fragment_ex(varyings, graphics::FragmentContext(), constants, textures, color);
}

bool D3d9Program::run_fragment_ex(const graphics::Vec4* varyings, const graphics::FragmentContext& context, const uint8_t* constants, const graphics::ITextureSet* textures,
                                  graphics::Vec4* color) const {
  State s = {};
  s.varyings = varyings;
  s.constants = constants;
  s.textures = textures;
  s.sys[kSysPosition] = {{context.position.x, context.position.y, context.position.z, context.position.w}};
  s.sys[kSysFrontFacing].f[0] = from_bits(context.front_facing ? 0xFFFFFFFFu : 0u);
  s.sys[kSysPrimitiveId].f[0] = from_bits(context.primitive_id);
  execute(s);
  if (s.killed) return false;
  for (uint32_t n = 0; n < color_outputs_; ++n) color[n] = {s.color[n].f[0], s.color[n].f[1], s.color[n].f[2], s.color[n].f[3]};
  return true;
}

void D3d9Program::run_hull(const graphics::HullInputData& input, const uint8_t* constants, graphics::HullOutputData* output) const {
  if (stage_ != graphics::ShaderStage::Hull || !output) return;
  State s;
  s.constants = constants;
  // (the vertex shader's positions come with Y down; the hull and domain shaders work in Direct3D's clip space, with Y up: turned over on the way in, here, and on the way out of the domain shader)
  graphics::HullInputData upright = input;
  for (uint32_t k = 0; k < upright.control_points && k < graphics::kMaxPatchControlPoints; ++k) upright.position[k].y = -upright.position[k].y;
  s.hull_in = &upright;
  // The control point phase once for each output control point, then the fork and join phases (which make the patch constants).
  for (uint32_t cp = 0; cp < tess_output_points_ && cp < graphics::kMaxPatchControlPoints; ++cp) {
    s.sys[kSysOutputControlPointId].f[0] = from_bits(cp);
    s.pos = {{0, 0, 0, 0}};
    for (Q& v : s.var) v = {{0, 0, 0, 0}};
    execute(s, hs_cp_begin_, hs_cp_end_);
    graphics::ShaderIo& out = output->control_point[cp];
    out.position = {s.pos.f[0], s.pos.f[1], s.pos.f[2], s.pos.f[3]};
    for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) out.varying[i] = {s.var[i].f[0], s.var[i].f[1], s.var[i].f[2], s.var[i].f[3]};
  }
  if (hs_fork_end_ > hs_fork_begin_) execute(s, hs_fork_begin_, hs_fork_end_);
  if (hs_join_end_ > hs_join_begin_) execute(s, hs_join_begin_, hs_join_end_);
  for (uint32_t i = 0; i < graphics::kMaxPatchRegisters; ++i) output->patch[i] = {s.patch_out[i].f[0], s.patch_out[i].f[1], s.patch_out[i].f[2], s.patch_out[i].f[3]};
  for (uint32_t k = 0; k < 4; ++k) output->outer[k] = outer_factor_reg_[k] >= 0 ? s.patch_out[outer_factor_reg_[k]].f[0] : 0.0f;   // (a factor not written culls the patch)
  for (uint32_t k = 0; k < 2; ++k) output->inner[k] = inner_factor_reg_[k] >= 0 ? s.patch_out[inner_factor_reg_[k]].f[0] : 1.0f;      // (an inside factor not written is 1)
}

void D3d9Program::run_domain(const graphics::HullOutputData& patch, float u, float v, float w, const uint8_t* constants, graphics::ShaderIo* out) const {
  if (stage_ != graphics::ShaderStage::Domain || !out) return;
  State s;
  s.constants = constants;
  s.patch_in = &patch;
  s.sys[kSysDomainLocation] = {{u, v, w, 0.0f}};
  execute(s);
  out->position = {s.pos.f[0], -s.pos.f[1], s.pos.f[2], s.pos.f[3]};
  for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) out->varying[i] = {s.var[i].f[0], s.var[i].f[1], s.var[i].f[2], s.var[i].f[3]};
}

void D3d9Program::run_geometry(const graphics::GeometryInputData& input, const uint8_t* constants, graphics::GeometryOutputData* output) const {
  if (stage_ != graphics::ShaderStage::Geometry || !output) return;
  State s;
  s.constants = constants;
  // The vertex shader's positions come with Y the way the devices want it (down); the shader works in Direct3D's clip space (up), so they are turned over on the way in and out.
  graphics::GeometryInputData upright = input;
  for (graphics::Vec4& p : upright.position) p.y = -p.y;
  s.geometry_in = &upright;
  s.sys[kSysGsInstanceId].f[0] = from_bits(input.instance_id);
  s.geometry_out = output;
  output->count = 0;
  execute(s);
}

void D3d9Program::run_compute(const graphics::ComputeContext& context, const uint8_t* constants) const {
  if (stage_ != graphics::ShaderStage::Compute) return;
  State s;
  s.constants = constants;
  s.compute = &context;
  for (uint32_t k = 0; k < 3; ++k) {
    s.sys[kSysDispatchThreadId].f[k] = from_bits(context.dispatch_id[k]);
    s.sys[kSysGroupId].f[k] = from_bits(context.group_id[k]);
    s.sys[kSysGroupThreadId].f[k] = from_bits(context.thread_id[k]);
  }
  s.sys[kSysGroupIndex].f[0] = from_bits(context.group_index);
  execute(s);
}

// A group of threads that share memory or wait for each other at barriers: every thread has its own registers and runs up to its next barrier (or its end); when all have stopped, they
// all go on. Threads that have finished do not wait for the others.
void D3d9Program::run_compute_group(const graphics::ComputeContext& context, const uint8_t* constants) const {
  if (stage_ != graphics::ShaderStage::Compute) return;
  const uint32_t threads = thread_group_[0] * thread_group_[1] * thread_group_[2];
  if (threads == 0 || threads > graphics::kMaxBarrierThreads) return;
  // The memory: the registers of the threads, where each is to go on, which are done, and the group-shared memories.
  State* states = static_cast<State*>(allocator_.allocate(sizeof(State) * threads));
  ArrayView<uint64_t> resume = allocate_view<uint64_t>(allocator_, threads);
  ArrayView<uint8_t> done = allocate_view<uint8_t>(allocator_, threads);
  ArrayView<uint8_t> shared_memory[graphics::kMaxSharedMemories];
  graphics::ComputeContext group = context;
  bool ok = states && !resume.empty() && !done.empty();
  for (uint32_t k = 0; k < graphics::kMaxSharedMemories && ok; ++k) {
    if (!shared_bytes_[k]) continue;
    shared_memory[k] = allocate_view<uint8_t>(allocator_, shared_bytes_[k]);
    ok = !shared_memory[k].empty();
    if (ok) {
      shared_memory[k].fill(0);
      group.shared[k] = ByteView(shared_memory[k].data(), shared_memory[k].size());
    }
  }
  uint32_t constructed = 0;
  if (ok) {
    for (; constructed < threads; ++constructed) {
      State* s = new (&states[constructed]) State();
      s->constants = constants;
      s->compute = &group;
      const uint32_t i = constructed;
      const uint32_t tx = i % thread_group_[0], ty = (i / thread_group_[0]) % thread_group_[1], tz = i / (thread_group_[0] * thread_group_[1]);
      const uint32_t id[3] = {tx, ty, tz};
      for (uint32_t k = 0; k < 3; ++k) {
        s->sys[kSysDispatchThreadId].f[k] = from_bits(context.group_id[k] * thread_group_[k] + id[k]);
        s->sys[kSysGroupId].f[k] = from_bits(context.group_id[k]);
        s->sys[kSysGroupThreadId].f[k] = from_bits(id[k]);
      }
      s->sys[kSysGroupIndex].f[0] = from_bits(i);
      resume[i] = 0;
      done[i] = 0;
    }
    uint32_t remaining = threads;
    for (uint32_t phase = 0; remaining && phase < 100000; ++phase)
      for (uint32_t i = 0; i < threads; ++i) {
        if (done[i]) continue;
        State& s = states[i];
        s.at_barrier = false;
        execute(s, resume[i], code_.count());
        if (s.at_barrier) {
          resume[i] = s.barrier_pc;
        } else {
          done[i] = 1;
          --remaining;
        }
      }
  }
  for (uint32_t i = 0; i < constructed; ++i) states[i].~State();
  if (states) allocator_.deallocate(states);
  release_view(allocator_, resume);
  release_view(allocator_, done);
  for (ArrayView<uint8_t>& m : shared_memory) release_view(allocator_, m);
}

// --- Trampolines -----------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kSlots = 32;
const D3d9Program* g_vertex[kSlots];
const D3d9Program* g_fragment[kSlots];
const D3d9Program* g_textured[kSlots];
const D3d9Program* g_vertex_ex[kSlots];
const D3d9Program* g_fragment_ex[kSlots];
const D3d9Program* g_compute[kSlots];
const D3d9Program* g_geometry[kSlots];
const D3d9Program* g_hull[kSlots];
const D3d9Program* g_domain[kSlots];

template <uint32_t N>
void vertex_trampoline(const graphics::Vec4* attributes, const uint8_t* constants, graphics::ShaderIo* out) {
  g_vertex[N]->run_vertex(attributes, constants, out);
}
template <uint32_t N>
bool fragment_trampoline(const graphics::Vec4* varyings, const uint8_t* constants, graphics::Vec4* color) {
  return g_fragment[N]->run_fragment(varyings, constants, nullptr, color);
}
template <uint32_t N>
bool textured_trampoline(const graphics::Vec4* varyings, const uint8_t* constants, const graphics::ITextureSet& textures, graphics::Vec4* color) {
  return g_textured[N]->run_fragment(varyings, constants, &textures, color);
}

template <uint32_t N>
void vertex_ex_trampoline(const graphics::Vec4* attributes, const uint8_t* constants, const graphics::VertexContext& context, graphics::ShaderIo* out) {
  g_vertex_ex[N]->run_vertex_ex(attributes, constants, context, out);
}
template <uint32_t N>
bool fragment_ex_trampoline(const graphics::Vec4* varyings, const graphics::FragmentContext& context, const uint8_t* constants, const graphics::ITextureSet& textures, graphics::Vec4* color) {
  return g_fragment_ex[N]->run_fragment_ex(varyings, context, constants, &textures, color);
}

template <uint32_t N>
void compute_trampoline(const graphics::ComputeContext& context, const uint8_t* constants) {
  g_compute[N]->run_compute(context, constants);
}

template <uint32_t N>
void compute_group_trampoline(const graphics::ComputeContext& context, const uint8_t* constants) {
  g_compute[N]->run_compute_group(context, constants);
}

template <uint32_t N>
void geometry_trampoline(const graphics::GeometryInputData& input, const uint8_t* constants, graphics::GeometryOutputData* output) {
  g_geometry[N]->run_geometry(input, constants, output);
}

template <uint32_t N>
void hull_trampoline(const graphics::HullInputData& input, const uint8_t* constants, graphics::HullOutputData* output) {
  g_hull[N]->run_hull(input, constants, output);
}
template <uint32_t N>
void domain_trampoline(const graphics::HullOutputData& patch, float u, float v, float w, const uint8_t* constants, graphics::ShaderIo* out) {
  g_domain[N]->run_domain(patch, u, v, w, constants, out);
}

template <uint32_t... I>
struct Trampolines {
  static constexpr graphics::HullShaderFn hull[sizeof...(I)] = {&hull_trampoline<I>...};
  static constexpr graphics::DomainShaderFn domain[sizeof...(I)] = {&domain_trampoline<I>...};
  static constexpr graphics::GeometryShaderFn geometry[sizeof...(I)] = {&geometry_trampoline<I>...};
  static constexpr graphics::ComputeShaderFn compute[sizeof...(I)] = {&compute_trampoline<I>...};
  static constexpr graphics::ComputeShaderFn compute_group[sizeof...(I)] = {&compute_group_trampoline<I>...};
  static constexpr graphics::VertexShaderFnEx vertex_ex[sizeof...(I)] = {&vertex_ex_trampoline<I>...};
  static constexpr graphics::FragmentShaderFnEx fragment_ex[sizeof...(I)] = {&fragment_ex_trampoline<I>...};
  static constexpr graphics::VertexShaderFn vertex[sizeof...(I)] = {&vertex_trampoline<I>...};
  static constexpr graphics::FragmentShaderFn fragment[sizeof...(I)] = {&fragment_trampoline<I>...};
  static constexpr graphics::TexturedFragmentShaderFn textured[sizeof...(I)] = {&textured_trampoline<I>...};
};
template <uint32_t... I>
constexpr Trampolines<I...> make_trampolines(std::integer_sequence<uint32_t, I...>) {
  return {};
}
constexpr auto kTrampolines = make_trampolines(std::make_integer_sequence<uint32_t, kSlots>());

}  // namespace

NtStatus D3d9Binding::bind(UniquePointer<D3d9Program> program, graphics::ShaderModuleDesc* desc) {
  release();
  if (!program) return status::kInvalidParameter;
  if (program->stage() == graphics::ShaderStage::Hull || program->stage() == graphics::ShaderStage::Domain) {
    const bool is_hull = program->stage() == graphics::ShaderStage::Hull;
    const D3d9Program** table = is_hull ? g_hull : g_domain;
    uint32_t slot = 0;
    while (slot < kSlots && table[slot]) ++slot;
    if (slot == kSlots) return status::kInsufficientResources;
    table[slot] = program.get();
    slot_ = static_cast<int32_t>(slot);
    kind_ = is_hull ? 5 : 6;
    desc->stage = program->stage();
    if (is_hull) desc->hull_function = decltype(kTrampolines)::hull[slot];
    else desc->domain_function = decltype(kTrampolines)::domain[slot];
    desc->tess_input_points = program->tess_input_points();
    desc->tess_output_points = program->tess_output_points();
    desc->tess_domain = program->tess_domain();
    desc->tess_partitioning = program->tess_partitioning();
    desc->tess_output = program->tess_output();
    program_ = util::move(program);
    return status::kSuccess;
  }
  if (program->stage() == graphics::ShaderStage::Geometry) {
    uint32_t slot = 0;
    while (slot < kSlots && g_geometry[slot]) ++slot;
    if (slot == kSlots) return status::kInsufficientResources;
    g_geometry[slot] = program.get();
    slot_ = static_cast<int32_t>(slot);
    kind_ = 4;
    desc->stage = graphics::ShaderStage::Geometry;
    desc->geometry_function = decltype(kTrampolines)::geometry[slot];
    desc->geometry_input = program->geometry_input();
    desc->geometry_output = program->geometry_output();
    desc->geometry_max_vertices = program->geometry_max_vertices();
    desc->geometry_instances = program->geometry_instances();
    program_ = util::move(program);
    return status::kSuccess;
  }
  if (program->stage() == graphics::ShaderStage::Compute) {
    uint32_t slot = 0;
    while (slot < kSlots && g_compute[slot]) ++slot;
    if (slot == kSlots) return status::kInsufficientResources;
    g_compute[slot] = program.get();
    slot_ = static_cast<int32_t>(slot);
    kind_ = 3;
    desc->stage = graphics::ShaderStage::Compute;
    if (program->needs_group()) {
      desc->compute_group_function = decltype(kTrampolines)::compute_group[slot];
      desc->shared_memory_bytes = program->shared_total_bytes();
    } else {
      desc->compute_function = decltype(kTrampolines)::compute[slot];
    }
    for (uint32_t k = 0; k < 3; ++k) desc->thread_group[k] = program->thread_group(k);
    program_ = util::move(program);
    return status::kSuccess;
  }
  const D3d9Program** table = program->stage() == graphics::ShaderStage::Vertex ? g_vertex : program->uses_textures() ? g_textured : g_fragment;
  uint32_t slot = 0;
  while (slot < kSlots && table[slot]) ++slot;
  if (slot == kSlots) return status::kInsufficientResources;
  table[slot] = program.get();
  slot_ = static_cast<int32_t>(slot);
  kind_ = program->stage() == graphics::ShaderStage::Vertex ? 0 : program->uses_textures() ? 2 : 1;
  desc->stage = program->stage();
  if (kind_ == 0) desc->vertex_function = decltype(kTrampolines)::vertex[slot];
  else if (kind_ == 1) desc->fragment_function = decltype(kTrampolines)::fragment[slot];
  else desc->textured_fragment_function = decltype(kTrampolines)::textured[slot];
  if (program->uses_system_values() || program->required_features()) {
    // (a second function for the devices that pass system values and answer texel access)
    const D3d9Program** table_ex = kind_ == 0 ? g_vertex_ex : g_fragment_ex;
    uint32_t slot_ex = 0;
    while (slot_ex < kSlots && table_ex[slot_ex]) ++slot_ex;
    if (slot_ex < kSlots) {
      table_ex[slot_ex] = program.get();
      slot_ex_ = static_cast<int32_t>(slot_ex);
      if (kind_ == 0) desc->vertex_function_ex = decltype(kTrampolines)::vertex_ex[slot_ex];
      else desc->fragment_function_ex = decltype(kTrampolines)::fragment_ex[slot_ex];
    }
  }
  program_ = util::move(program);
  return status::kSuccess;
}

void D3d9Binding::release() {
  if (slot_ >= 0) {
    const D3d9Program** table = kind_ == 0 ? g_vertex : kind_ == 1 ? g_fragment : kind_ == 3 ? g_compute : kind_ == 4 ? g_geometry : kind_ == 5 ? g_hull : kind_ == 6 ? g_domain : g_textured;
    table[slot_] = nullptr;
    slot_ = -1;
  }
  if (slot_ex_ >= 0) {
    (kind_ == 0 ? g_vertex_ex : g_fragment_ex)[slot_ex_] = nullptr;
    slot_ex_ = -1;
  }
  program_.reset();
}

}  // namespace dxgk
