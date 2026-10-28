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
#include "Math/FloatMath.hpp"
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

}  // namespace


struct D3d9Program::State {
  Q r[kMaxTemps];
  Q a0;
  Q pos;
  Q var[graphics::kMaxVaryings];
  Q color;
  const graphics::Vec4* attributes = nullptr;
  const graphics::Vec4* varyings = nullptr;
  const uint8_t* constants = nullptr;
  const graphics::ITextureSet* textures = nullptr;
  bool killed = false;
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

void D3d9Program::execute(State& s) const {
  using Instr = Instruction;
  auto push_constant = [&](uint32_t index) {
    Q q = {{0, 0, 0, 0}};
    if (index < kPushConstantRegisters && s.constants) __builtin_memcpy(q.f, s.constants + index * 16, 16);
    return q;
  };
  auto constant = [&](uint32_t index) {
    for (const Def& d : defs_.view())
      if (d.number == index) return Q{{d.value[0], d.value[1], d.value[2], d.value[3]}};
    return push_constant(index);
  };
  auto fetch = [&](const Instr::Src& src, uint32_t offset) {
    Q v = {{0, 0, 0, 0}};
    switch (src.type) {
      case kTemp: v = s.r[src.number]; break;
      case kInput:
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
        if (src.relative) index += static_cast<int64_t>(floor_(s.a0.f[src.relative_component]));
        v = index < 0 ? Q{{0, 0, 0, 0}} : constant(static_cast<uint32_t>(index > 4095 ? 4095 : index));
        break;
      }
      default: break;
    }
    Q r = {{v.f[src.swizzle[0]], v.f[src.swizzle[1]], v.f[src.swizzle[2]], v.f[src.swizzle[3]]}};
    if (src.modifier != kModNone)
      for (float& c : r.f) c = apply_modifier(src.modifier, c);
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
      case kRastOut: target = &s.pos; break;
      case kAttrOut:
      case kTexCrdOut: target = d.slot == kSlotPosition ? &s.pos : d.slot >= 0 ? &s.var[d.slot] : nullptr; break;
      case kColorOut: target = &s.color; break;
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

  const uint64_t count = code_.count();
  for (uint64_t pc = 0; pc < count;) {
    const Instr& ins = code_[pc++];
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
      case kIfNz:
        if (!(a.f[0] != 0)) pc = ins.target;
        break;
      case kIfZ:
        if (a.f[0] != 0) pc = ins.target;
        break;
      case kDiscardNz:
      case kDiscardZ:
        if ((a.f[0] != 0) == (ins.op == kDiscardNz)) {
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
      case kSlt: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] < b.f[i] ? 1.0f : 0.0f; store(ins.dst, r); break;
      case kSge: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] >= b.f[i] ? 1.0f : 0.0f; store(ins.dst, r); break;
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
      case kSeq: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] == b.f[i] ? 1.0f : 0.0f; store(ins.dst, r); break;
      case kSne: b = fetch(ins.src[1], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] != b.f[i] ? 1.0f : 0.0f; store(ins.dst, r); break;
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
      case kSelect: b = fetch(ins.src[1], 0); c = fetch(ins.src[2], 0); for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] != 0 ? b.f[i] : c.f[i]; store(ins.dst, r); break;
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
      case kTexLd: {
        b = {{0, 0, 0, 0}};
        const uint32_t unit = ins.src[1].number;
        float u = a.f[0], v = a.f[1];
        if (ins.control & 1) {   // project
          const float inv = a.f[3] == 0 ? 0.0f : 1.0f / a.f[3];
          u *= inv, v *= inv;
        }
        if (s.textures) {
          const graphics::Vec4 t = s.textures->sample(unit, u, v);
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

void D3d9Program::run_vertex(const graphics::Vec4* attributes, const uint8_t* constants, graphics::ShaderIo* out) const {
  State s = {};
  s.attributes = attributes;
  s.constants = constants;
  execute(s);
  out->position = {s.pos.f[0], -s.pos.f[1], s.pos.f[2], s.pos.f[3]};   // (Direct3D's +Y up to the device's +Y down)
  for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) out->varying[i] = {s.var[i].f[0], s.var[i].f[1], s.var[i].f[2], s.var[i].f[3]};
}

bool D3d9Program::run_fragment(const graphics::Vec4* varyings, const uint8_t* constants, const graphics::ITextureSet* textures, graphics::Vec4* color) const {
  State s = {};
  s.varyings = varyings;
  s.constants = constants;
  s.textures = textures;
  execute(s);
  if (s.killed) return false;
  *color = {s.color.f[0], s.color.f[1], s.color.f[2], s.color.f[3]};
  return true;
}

// --- Trampolines -----------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kSlots = 32;
const D3d9Program* g_vertex[kSlots];
const D3d9Program* g_fragment[kSlots];
const D3d9Program* g_textured[kSlots];

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

template <uint32_t... I>
struct Trampolines {
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
  program_ = util::move(program);
  return status::kSuccess;
}

void D3d9Binding::release() {
  if (slot_ >= 0) {
    const D3d9Program** table = kind_ == 0 ? g_vertex : kind_ == 1 ? g_fragment : g_textured;
    table[slot_] = nullptr;
    slot_ = -1;
  }
  program_.reset();
}

}  // namespace dxgk
