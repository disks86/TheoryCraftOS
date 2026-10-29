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

// DXBC (the container the Direct3D 10 and 11 runtimes hand a driver: Shader Model 4.0 / 4.1 / 5.0 / 5.1 vertex and pixel shaders) decoded into the instruction form D3d9Program runs, so
// that the interpreter and the TGSI emitter take these shaders too.
//
// The container is checked (magic, size, every chunk inside it); the checksum is not (a driver trusts the runtime to have made it). The SHDR / SHEX chunk is the program, the ISGN /
// OSGN chunks (ISG1 / OSG1 too) say what the input and output registers mean. Mapping, as for Direct3D 9: a vertex shader's input register vN is vertex attribute location N; its
// outputs go to the varyings by semantic (COLOR0 / COLOR1 to 0 / 1, TEXCOORD0 / TEXCOORD1 to 2 / 3), SV_Position is the position; a pixel shader reads the same by its input
// semantics and writes o0 (SV_Target0). cb0[0..7] are the push constants. Literals become constants of the program. Resource t0 / t1 with a 2D texture are texture units 0 / 1.
//
// Taken: the float arithmetic, dot products, comparisons (as 1.0 / 0.0), movc, rounding, sin / cos / sqrt / rsq / exp / log / div, sample, if_z / if_nz / else / endif,
// discard_z / discard_nz, saturate, negate / abs source modifiers, swizzles and write masks. Refused (kNotSupported): integer and bitwise instructions, loops and switches, calls,
// ld / sample_l / sample_b / sample_c / sample_d, derivatives, resinfo, relative addressing, constant buffers other than cb0, indexable temps, geometry / hull / domain / compute
// shaders, SV_Position, vertex or instance id as an input, depth output.
#include "Kernel/Dxgkrnl/D3d9Decoded.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace dxgk {

namespace {

constexpr uint32_t kMagic = 0x43425844, kShdr = 0x52444853, kShex = 0x58454853, kIsgn = 0x4E475349, kIsg1 = 0x31475349, kOsgn = 0x4E47534F, kOsg1 = 0x3147534F;

enum DxbcOp : uint32_t {
  kAdd4 = 0, kBreak4 = 2, kBreakc4 = 3, kContinue4 = 7, kContinuec4 = 8, kEndloop4 = 22, kLoop4 = 48, kCall4 = 4, kCallc4 = 5, kDiscard4 = 13, kDiv4 = 14, kDp2_4 = 15, kDp3_4 = 16, kDp4_4 = 17, kElse4 = 18, kEndif4 = 21, kEq4 = 24, kExp4 = 25, kFrc4 = 26,
  kGe4 = 29, kIf4 = 31, kLog4 = 47, kLt4 = 49, kMad4 = 50, kMin4 = 51, kMax4 = 52, kCustomData4 = 53, kMov4 = 54, kMovc4 = 55, kMul4 = 56, kNe4 = 57, kNop4 = 58, kRet4 = 62,
  kRoundNe4 = 64, kRoundNi4 = 65, kRoundPi4 = 66, kRoundZ4 = 67, kRsq4 = 68, kSample4 = 69, kSqrt4 = 75, kSincos4 = 77,
  kDclResource4 = 88, kDclConstantBuffer4 = 89, kDclSampler4 = 90, kDclInput4 = 95, kDclInputSgv4 = 96, kDclInputSiv4 = 97, kDclInputPs4 = 98, kDclInputPsSgv4 = 99, kDclInputPsSiv4 = 100,
  kDclOutput4 = 101, kDclOutputSgv4 = 102, kDclOutputSiv4 = 103, kDclTemps4 = 104, kDclIndexableTemp4 = 105, kDclGlobalFlags4 = 106,
};
enum OperandType : uint32_t { kOpTemp = 0, kOpInput = 1, kOpOutput = 2, kOpImmediate = 4, kOpSampler = 6, kOpResource = 7, kOpConstantBuffer = 8, kOpImmediateConstantBuffer = 9, kOpNull = 13 };

// Where a program made from DXBC keeps what it has no register for: its literals, and the immediate constant buffer (declared in the program) as constants of its own.
constexpr uint16_t kLiteralBase = 0x4000, kImmediateBufferBase = 0x6000;
constexpr uint32_t kMaxImmediateBuffer = 512, kMaxConstantBuffers = 14;

struct Operand {
  uint32_t type = 0;
  uint32_t index[3] = {0, 0, 0};
  uint32_t dims = 0;
  uint32_t components = 0;   // 0 none, 1 one, 2 four
  uint8_t mask = 0xF;
  uint8_t swizzle[4] = {0, 1, 2, 3};
  uint8_t modifier = 0;   // 0 none, 1 negate, 2 abs, 3 negated abs
  float imm[4] = {0, 0, 0, 0};
  bool relative = false;   // the last index adds the integer in a temporary's component (cb0[r1.x + 3], icb[r2.y])
  uint8_t relative_temp = 0, relative_component = 0;
};

struct SigElement {
  char name[16] = {};
  uint32_t index = 0;
  uint32_t reg = 0;
};

bool names_equal(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
    if (x != y) return false;
    if (!x) return true;
  }
}

// Reads a signature chunk into `out` (at most 16 elements); false when it is malformed.
bool read_signature(ConstByteView chunk, uint32_t element_size, SigElement* out, uint32_t* count) {
  ByteReader in(chunk);
  const uint32_t n = in.le32();
  in.le32();   // (the key)
  if (!in.ok() || n > 16) return false;
  for (uint32_t i = 0; i < n; ++i) {
    ConstByteView entry = in.take(element_size);
    if (!in.ok()) return false;
    ByteReader e(entry);
    const uint32_t name_at = e.le32(), index = e.le32();
    e.le32();
    e.le32();
    const uint32_t reg = e.le32();
    if (!e.ok() || name_at >= chunk.size()) return false;
    SigElement& s = out[i];
    uint32_t k = 0;
    for (; k < 15; ++k) {
      uint8_t c = 0;
      if (!chunk.try_get(name_at + k, &c)) return false;
      if (c == 0) break;
      s.name[k] = static_cast<char>(c);
    }
    s.name[k] = 0;
    s.index = index;
    s.reg = reg;
  }
  *count = n;
  return true;
}

// Where a signature element goes between the stages (see the top of this file); kSlotNone when it goes nowhere this device has.
int8_t signature_slot(const SigElement& s, bool output_of_vertex) {
  if (names_equal(s.name, "COLOR") && s.index < 2) return static_cast<int8_t>(s.index);
  if (names_equal(s.name, "TEXCOORD") && s.index < 2) return static_cast<int8_t>(2 + s.index);
  if (output_of_vertex && (names_equal(s.name, "SV_POSITION") || names_equal(s.name, "POSITION")) && s.index == 0) return kSlotPosition;
  return kSlotNone;
}

}  // namespace

NtStatus D3d9Program::compile_dxbc(allocators::IAllocator& allocator, ConstByteView bytes, UniquePointer<D3d9Program>* out) {
  using Instr = Instruction;
  if (bytes.size() < 32 || bytes.size() > kMaxTokens * 4ull) return status::kInvalidParameter;
  ByteReader head(bytes);
  if (head.le32() != kMagic) return status::kInvalidParameter;
  head.skip(16);
  const uint32_t one = head.le32(), total = head.le32(), chunk_count = head.le32();
  if (!head.ok() || one != 1 || total > bytes.size() || total < 32 || chunk_count == 0 || chunk_count > 32) return status::kInvalidParameter;
  ConstByteView shader, isgn, osgn;
  uint32_t isgn_size = 24, osgn_size = 24;
  for (uint32_t i = 0; i < chunk_count; ++i) {
    const uint32_t offset = head.le32();
    if (!head.ok() || offset > total || total - offset < 8) return status::kInvalidParameter;
    ByteReader c(bytes.subview(offset, total - offset));
    const uint32_t fourcc = c.le32(), size = c.le32();
    if (!c.ok() || size > c.remaining()) return status::kInvalidParameter;
    const ConstByteView data = c.take(size);
    if (fourcc == kShdr || fourcc == kShex) shader = data;
    else if (fourcc == kIsgn || fourcc == kIsg1) isgn = data, isgn_size = fourcc == kIsg1 ? 32 : 24;
    else if (fourcc == kOsgn || fourcc == kOsg1) osgn = data, osgn_size = fourcc == kOsg1 ? 32 : 24;
  }
  if (shader.size() < 8 || shader.size() % 4) return status::kInvalidParameter;

  ByteReader in(shader);
  const uint32_t version = in.le32(), length = in.le32();
  const uint32_t program_type = version >> 16, major = (version >> 4) & 0xF, minor = version & 0xF;
  if (length != shader.size() / 4) return status::kInvalidParameter;
  if (program_type != 0 && program_type != 1) return status::kNotSupported;   // pixel and vertex shaders only
  if (!((major == 4 && minor <= 1) || (major == 5 && minor <= 1))) return status::kNotSupported;
  const bool v51 = major == 5 && minor == 1;   // (constant buffers, textures and samplers are ranges with a space: cb[ID][index][element], t[ID][index])
  int32_t cb_register[8], texture_register[8];   // by range ID: the register the range starts at, or -1
  for (int32_t& r : cb_register) r = -1;
  for (int32_t& r : texture_register) r = -1;
  uint32_t cb_size[kMaxConstantBuffers] = {};    // by slot (b#): the size declared, in vec4s; 0: not declared
  uint32_t immediate_buffer_size = 0;            // vec4s of the declared immediate constant buffer
  // The register of cb<slot>[0] in the program's constant block: the declared buffers one after the other in slot order.
  auto cb_base = [&](uint32_t slot) {
    uint32_t base = 0;
    for (uint32_t k = 0; k < slot; ++k) base += cb_size[k];
    return base;
  };
  const bool vertex = program_type == 1;

  UniquePointer<D3d9Program> program = UniquePointer<D3d9Program>::make(allocator, allocator);
  if (!program) return status::kNoMemory;
  program->stage_ = vertex ? graphics::ShaderStage::Vertex : graphics::ShaderStage::Fragment;
  program->major_ = major;
  program->minor_ = minor;

  // What the input and output registers are.
  int8_t in_slot[16], out_slot[16];
  int8_t in_sys[16];   // an input register that is a system value (SysValue), or -1
  for (int8_t& s : in_sys) s = -1;
  for (int8_t& s : in_slot) s = kSlotNone;
  for (int8_t& s : out_slot) s = kSlotNone;
  SigElement elements[16];
  uint32_t count = 0;
  if (!isgn.empty()) {
    if (!read_signature(isgn, isgn_size, elements, &count)) return status::kInvalidParameter;
    for (uint32_t i = 0; i < count; ++i)
      if (elements[i].reg < 16) in_slot[elements[i].reg] = signature_slot(elements[i], false);
  }
  if (vertex) {
    for (uint32_t i = 0; i < graphics::kMaxVertexAttributes; ++i) in_slot[i] = static_cast<int8_t>(i);   // (vN is attribute location N)
    if (osgn.empty() || !read_signature(osgn, osgn_size, elements, &count)) return status::kInvalidParameter;
    for (uint32_t i = 0; i < count; ++i)
      if (elements[i].reg < 16) out_slot[elements[i].reg] = signature_slot(elements[i], true);
  } else {
    out_slot[0] = 0;
  }

  // --- operands
  auto read_operand = [&](ByteReader& p, Operand* o) -> NtStatus {
    const uint32_t t = p.le32();
    if (!p.ok()) return status::kInvalidParameter;
    o->components = t & 3;
    const uint32_t selection = (t >> 2) & 3, bits = (t >> 4) & 0xFF;
    o->type = (t >> 12) & 0xFF;
    o->dims = (t >> 20) & 3;
    if (o->components == 3) return status::kNotSupported;   // N components
    if (o->components == 2) {
      if (selection == 0) o->mask = static_cast<uint8_t>(bits & 0xF);
      else if (selection == 1)
        for (uint32_t i = 0; i < 4; ++i) o->swizzle[i] = static_cast<uint8_t>((bits >> (2 * i)) & 3);
      else
        for (uint32_t i = 0; i < 4; ++i) o->swizzle[i] = static_cast<uint8_t>(bits & 3);
    } else {
      for (uint8_t& s : o->swizzle) s = 0;
    }
    if (t >> 31) {
      const uint32_t e = p.le32();
      if (!p.ok() || (e >> 31)) return status::kNotSupported;
      if ((e & 0x3F) == 1) o->modifier = static_cast<uint8_t>((e >> 6) & 0xF);
      if (o->modifier > 3) return status::kNotSupported;
    }
    if (o->dims > 3) return status::kInvalidParameter;
    for (uint32_t i = 0; i < o->dims; ++i) {
      const uint32_t representation = (t >> (22 + 3 * i)) & 7;
      if (representation == 0) {
        o->index[i] = p.le32();
      } else if ((representation == 2 || representation == 3) && i + 1 == o->dims && (o->type == kOpConstantBuffer || o->type == kOpImmediateConstantBuffer)) {
        // The last index is relative: an immediate (representation 3), then the operand that gives the rest - a temporary's component, as fxc writes it.
        o->index[i] = representation == 3 ? p.le32() : 0;
        const uint32_t sub = p.le32();
        if (!p.ok() || ((sub >> 12) & 0xFF) != kOpTemp || ((sub >> 20) & 3) != 1 || ((sub >> 22) & 7) != 0 || (sub >> 31)) return status::kNotSupported;
        o->relative = true;
        o->relative_component = static_cast<uint8_t>(((sub >> 2) & 3) == 2 ? (sub >> 4) & 3 : (sub >> 4) & 3);   // (select_1, or the first of a swizzle)
        const uint32_t temp = p.le32();
        if (!p.ok() || temp >= kMaxTemps) return status::kNotSupported;
        o->relative_temp = static_cast<uint8_t>(temp);
      } else {
        return status::kNotSupported;   // 64-bit indices, relative anywhere else
      }
    }
    if (o->type == kOpImmediate) {
      if (o->components == 1) {
        const float v = __builtin_bit_cast(float, p.le32());
        for (float& f : o->imm) f = v;
      } else if (o->components == 2) {
        for (float& f : o->imm) f = __builtin_bit_cast(float, p.le32());
      } else {
        return status::kInvalidParameter;
      }
    }
    return p.ok() ? status::kSuccess : status::kInvalidParameter;
  };

  auto literal = [&](const float v[4], uint16_t* number) -> NtStatus {
    for (const Def& d : program->defs_.view()) {
      if (d.number < kLiteralBase || d.number >= kImmediateBufferBase) continue;
      bool same = true;
      for (uint32_t k = 0; k < 4; ++k) same = same && __builtin_bit_cast(uint32_t, d.value[k]) == __builtin_bit_cast(uint32_t, v[k]);
      if (same) {
        *number = d.number;
        return status::kSuccess;
      }
    }
    uint32_t literals = 0;
    for (const Def& d : program->defs_.view())
      if (d.number >= kLiteralBase && d.number < kImmediateBufferBase) ++literals;
    if (literals >= 256) return status::kNotSupported;
    *number = static_cast<uint16_t>(kLiteralBase + literals);
    return program->defs_.push_back({*number, {v[0], v[1], v[2], v[3]}}) ? status::kSuccess : status::kNoMemory;
  };

  auto to_src = [&](const Operand& o, Instr::Src* s) -> NtStatus {
    for (uint32_t i = 0; i < 4; ++i) s->swizzle[i] = o.swizzle[i];
    s->modifier = o.modifier == 1 ? kModNeg : o.modifier == 2 ? kModAbs : o.modifier == 3 ? kModAbsNeg : kModNone;
    switch (o.type) {
      case kOpTemp:
        if (o.dims != 1 || o.index[0] >= kMaxTemps) return status::kNotSupported;
        s->type = kTemp, s->number = static_cast<uint16_t>(o.index[0]);
        return status::kSuccess;
      case kOpInput: {
        if (o.dims == 1 && o.index[0] < 16 && in_sys[o.index[0]] >= 0) {
          s->type = kSysValue, s->number = static_cast<uint16_t>(in_sys[o.index[0]]);
          program->uses_system_values_ = true;
          return status::kSuccess;
        }
        if (o.dims != 1 || o.index[0] >= 16 || in_slot[o.index[0]] < 0) return status::kNotSupported;
        s->type = kInput, s->number = static_cast<uint16_t>(o.index[0]), s->slot = in_slot[o.index[0]];
        return status::kSuccess;
      }
      case kOpConstantBuffer: {
        uint32_t slot, element;
        if (v51) {
          // cb[range][index][element]: the range declared as b<slot>, its one register, an element
          if (o.dims != 3 || o.index[0] >= 8 || cb_register[o.index[0]] < 0 || cb_register[o.index[0]] >= static_cast<int32_t>(kMaxConstantBuffers)) return status::kNotSupported;
          slot = static_cast<uint32_t>(cb_register[o.index[0]]);
          if (o.index[1] != slot) return status::kNotSupported;
          element = o.index[2];
        } else {
          if (o.dims != 2 || o.index[0] >= kMaxConstantBuffers) return status::kNotSupported;
          slot = o.index[0];
          element = o.index[1];
        }
        const uint32_t size = cb_size[slot];
        if (size == 0 || (!o.relative && element >= size) || element > size) return status::kNotSupported;
        const uint32_t base = cb_base(slot);
        s->type = kConst, s->number = static_cast<uint16_t>(base + element);
        if (o.relative) {
          s->relative = true, s->relative_temp = static_cast<int8_t>(o.relative_temp), s->relative_component = o.relative_component, s->relative_limit = static_cast<uint16_t>(base + size);
        }
        return status::kSuccess;
      }
      case kOpImmediateConstantBuffer:
        if (o.dims != 1 || immediate_buffer_size == 0 || (!o.relative && o.index[0] >= immediate_buffer_size) || o.index[0] > immediate_buffer_size) return status::kNotSupported;
        s->type = kConst, s->number = static_cast<uint16_t>(kImmediateBufferBase + o.index[0]);
        if (o.relative) {
          s->relative = true, s->relative_temp = static_cast<int8_t>(o.relative_temp), s->relative_component = o.relative_component;
          s->relative_limit = static_cast<uint16_t>(kImmediateBufferBase + immediate_buffer_size);
        }
        return status::kSuccess;
      case kOpImmediate: {
        uint16_t number = 0;
        const NtStatus r = literal(o.imm, &number);
        s->type = kConst, s->number = number;
        return r;
      }
      default: return status::kNotSupported;
    }
  };

  auto to_dst = [&](const Operand& o, Instr::Dst* d) -> NtStatus {
    d->mask = o.mask;
    if (o.components != 2) return status::kInvalidParameter;
    switch (o.type) {
      case kOpTemp:
        if (o.dims != 1 || o.index[0] >= kMaxTemps) return status::kNotSupported;
        d->type = kTemp, d->number = static_cast<uint16_t>(o.index[0]);
        return status::kSuccess;
      case kOpOutput: {
        if (o.dims != 1 || o.index[0] >= 16) return status::kNotSupported;
        if (!vertex) {
          if (o.index[0] != 0) return status::kNotSupported;   // one render target
          d->type = kColorOut, d->slot = 0;
          return status::kSuccess;
        }
        const int8_t slot = out_slot[o.index[0]];
        if (slot == kSlotNone) return status::kNotSupported;
        if (slot == kSlotPosition) d->type = kRastOut, d->slot = kSlotPosition;
        else d->type = kAttrOut, d->slot = slot;
        d->number = static_cast<uint16_t>(o.index[0]);
        return status::kSuccess;
      }
      default: return status::kNotSupported;
    }
  };

  struct Alu {
    uint32_t dxbc;
    uint16_t op;
    uint8_t sources;
    bool vector;
  };
  static const Alu kAlu[] = {
      {kMov4, kMov, 1, false},   {kAdd4, kAdd, 2, false},     {kMul4, kMul, 2, false},   {kMad4, kMad, 3, false},      {kDiv4, kDiv, 2, false},     {kDp2_4, kDp2, 2, false},
      {kDp3_4, kDp3, 2, false},  {kDp4_4, kDp4, 2, false},    {kMin4, kMin, 2, false},   {kMax4, kMax, 2, false},      {kEq4, kSeq, 2, false},      {kNe4, kSne, 2, false},
      {kLt4, kSlt, 2, false},    {kGe4, kSge, 2, false},      {kExp4, kExp, 1, true},    {kLog4, kLog, 1, true},       {kRsq4, kRsq, 1, true},      {kSqrt4, kSqrt, 1, false},
      {kFrc4, kFrc, 1, false},   {kRoundNe4, kRoundNe, 1, false}, {kRoundNi4, kFloor, 1, false}, {kRoundPi4, kCeil, 1, false}, {kRoundZ4, kTrunc, 1, false}, {kMovc4, kSelect, 3, false},
      // integer and bitwise (opcode numbers of d3d10tokenizedprogramformat.hpp)
      {30, kIAdd, 2, false}, {40, kINeg, 1, false}, {35, kIMad, 3, false}, {36, kIMax, 2, false}, {37, kIMin, 2, false}, {83, kUMax, 2, false}, {84, kUMin, 2, false}, {82, kUMad, 3, false},
      {41, kIShl, 2, false}, {42, kIShr, 2, false}, {85, kUShr, 2, false}, {1, kAnd, 2, false}, {60, kOr, 2, false}, {87, kXor, 2, false}, {59, kNot, 1, false},
      {27, kFtoI, 1, false}, {28, kFtoU, 1, false}, {43, kItoF, 1, false}, {86, kUtoF, 1, false}, {32, kIEq, 2, false}, {39, kINe, 2, false}, {34, kILt, 2, false}, {33, kIGe, 2, false},
      {79, kULt, 2, false}, {80, kUGe, 2, false},
  };

  List<uint32_t> open_ifs(allocator);
  bool returned = false;
  // Open loops: where each began, how many ifs were open then, and the breaks / continues waiting for their endloop.
  struct LoopFrame {
    uint32_t begin, ifs;
  };
  List<LoopFrame> loops(allocator);
  List<uint64_t> pending_exits(allocator), pending_continues(allocator);   // (loop depth << 32) | instruction index
  auto emit = [&](Instr& ins) -> NtStatus {
    if (program->code_.count() >= kMaxInstructions) return status::kNotSupported;
    const uint32_t index = static_cast<uint32_t>(program->code_.count());
    if (ins.op == kLoopBegin) {
      if (loops.count() >= 8 || !loops.push_back({index, static_cast<uint32_t>(open_ifs.count())})) return status::kNotSupported;
    } else if (ins.op == kLoopEnd) {
      if (!loops.count() || loops[loops.count() - 1].ifs != open_ifs.count()) return status::kInvalidParameter;   // (an if that straddles the end)
      const LoopFrame frame = loops[loops.count() - 1];
      const uint64_t depth = loops.count();
      loops.remove_at(loops.count() - 1);
      program->code_[frame.begin].target = index + 1;
      ins.target = frame.begin + 1;
      for (uint64_t i = pending_exits.count(); i > 0; --i)
        if ((pending_exits[i - 1] >> 32) == depth) {
          program->code_[pending_exits[i - 1] & 0xFFFFFFFFu].target = index + 1;
          pending_exits.remove_at(i - 1);
        }
      for (uint64_t i = pending_continues.count(); i > 0; --i)
        if ((pending_continues[i - 1] >> 32) == depth) {
          program->code_[pending_continues[i - 1] & 0xFFFFFFFFu].target = index;
          pending_continues.remove_at(i - 1);
        }
    } else if (ins.op == kBreak0 || ins.op == kBreakNz || ins.op == kBreakZ) {
      if (!loops.count() || !pending_exits.push_back((uint64_t{loops.count()} << 32) | index)) return status::kInvalidParameter;
    } else if (ins.op == kContinue0 || ins.op == kContinueNz || ins.op == kContinueZ) {
      if (!loops.count() || !pending_continues.push_back((uint64_t{loops.count()} << 32) | index)) return status::kInvalidParameter;
    }
    if (ins.op == kIfNz || ins.op == kIfZ) {
      if (open_ifs.count() >= 24 || !open_ifs.push_back(index)) return status::kNotSupported;
    } else if (ins.op == kElse) {
      if (!open_ifs.count()) return status::kInvalidParameter;
      Instr& open = program->code_[open_ifs[open_ifs.count() - 1]];
      if (open.op == kElse) return status::kInvalidParameter;
      open.target = index + 1;
      open_ifs.remove_at(open_ifs.count() - 1);
      if (!open_ifs.push_back(index)) return status::kNoMemory;
    } else if (ins.op == kEndIf) {
      if (!open_ifs.count()) return status::kInvalidParameter;
      program->code_[open_ifs[open_ifs.count() - 1]].target = index + 1;
      open_ifs.remove_at(open_ifs.count() - 1);
    }
    return program->code_.push_back(ins) ? status::kSuccess : status::kNoMemory;
  };

  // --- the instructions
  while (!in.at_end()) {
    if (returned) return status::kNotSupported;   // (code after a ret)
    const uint32_t token = in.le32();
    uint32_t words = (token >> 24) & 0x7F;
    const uint32_t opcode = token & 0x7FF;
    if (!in.ok()) return status::kInvalidParameter;
    if (opcode == kCustomData4) {
      // The length is the next word. Class 3 is the immediate constant buffer (vec4s, indexed by icb[i]); the other classes (comments, debug information) matter to nothing here.
      const uint32_t data_class = token >> 11, total_words = in.le32();
      if (!in.ok() || total_words < 2) return status::kInvalidParameter;
      const uint32_t payload = total_words - 2;
      if (data_class == 3) {
        if (payload % 4 || payload / 4 > kMaxImmediateBuffer || immediate_buffer_size != 0) return status::kNotSupported;
        immediate_buffer_size = payload / 4;
        for (uint32_t i = 0; i < immediate_buffer_size; ++i) {
          float v[4];
          for (float& f : v) f = __builtin_bit_cast(float, in.le32());
          if (!in.ok()) return status::kInvalidParameter;
          if (!program->defs_.push_back({static_cast<uint16_t>(kImmediateBufferBase + i), {v[0], v[1], v[2], v[3]}})) return status::kNoMemory;
        }
        continue;
      }
      if (!in.skip(uint64_t{payload} * 4)) return status::kInvalidParameter;
      continue;
    }
    if (words < 1) return status::kInvalidParameter;
    ConstByteView body = in.take(uint64_t{words - 1} * 4);
    if (!in.ok()) return status::kInvalidParameter;
    ByteReader p(body);
    // Extended opcode tokens: sample offsets must be zero; the others (resource dimension, return type) change nothing.
    for (uint32_t ext = token; (ext >> 31) != 0;) {
      ext = p.le32();
      if (!p.ok()) return status::kInvalidParameter;
      if ((ext & 0x3F) == 1 && ((ext >> 9) & 0xFFF) != 0) return status::kNotSupported;
    }
    NtStatus s = status::kSuccess;
    switch (opcode) {
      case kDclGlobalFlags4: case kDclSampler4: case kNop4: continue;
      case kDclTemps4: {
        const uint32_t n = p.le32();
        if (!p.ok() || n > kMaxTemps) return status::kNotSupported;
        continue;
      }
      case kDclConstantBuffer4: {
        Operand o;
        s = read_operand(p, &o);
        if (s != status::kSuccess) return s;
        if (v51) {
          // dcl_constant_buffer cb[ID][lower:upper], size, space
          const uint32_t size = p.le32(), space = p.le32();
          (void)size;
          if (!p.ok() || o.type != kOpConstantBuffer || o.dims != 3) return status::kInvalidParameter;
          if (o.index[0] >= 8 || o.index[1] != o.index[2] || o.index[1] >= kMaxConstantBuffers || space != 0 || size == 0 || size > kMaxConstantRegisters) return status::kNotSupported;
          cb_register[o.index[0]] = static_cast<int32_t>(o.index[1]);
          cb_size[o.index[1]] = size;
          continue;
        }
        if (o.type != kOpConstantBuffer || o.dims != 2) return status::kInvalidParameter;
        if (o.index[0] >= kMaxConstantBuffers || o.index[1] == 0 || o.index[1] > kMaxConstantRegisters) return status::kNotSupported;
        cb_size[o.index[0]] = o.index[1];
        continue;
      }
      case kDclResource4: {
        Operand o;
        s = read_operand(p, &o);
        if (s != status::kSuccess) return s;
        if (v51) {
          // dcl_resource_texture2d t[ID][lower:upper], return type, space
          const uint32_t return_type = p.le32(), space = p.le32();
          (void)return_type;
          if (!p.ok() || o.type != kOpResource || o.dims != 3) return status::kInvalidParameter;
          if (((token >> 11) & 0x1F) != 3 || o.index[0] >= 8 || o.index[1] != o.index[2] || o.index[1] >= graphics::kMaxTextureUnits || space != 0) return status::kNotSupported;
          texture_register[o.index[0]] = static_cast<int32_t>(o.index[1]);
          continue;
        }
        if (o.type != kOpResource || o.dims != 1 || ((token >> 11) & 0x1F) != 3 || o.index[0] >= graphics::kMaxTextureUnits) return status::kNotSupported;   // 2D, two units
        continue;
      }
      case kDclInput4: case kDclInputPs4: case kDclOutput4: case kDclInputSiv4: case kDclInputPsSiv4: case kDclOutputSiv4: case kDclInputSgv4: case kDclInputPsSgv4: case kDclOutputSgv4: {
        Operand o;
        s = read_operand(p, &o);
        if (s != status::kSuccess) return s;
        if ((o.type != kOpInput && o.type != kOpOutput) || o.dims != 1 || o.index[0] >= 16) return status::kNotSupported;   // (depth and the like)
        // An input that is a system value says which in its declaration (a name after the operand); the rest take their meaning from the signature.
        if (o.type == kOpInput && (opcode == kDclInputSgv4 || opcode == kDclInputSiv4 || opcode == kDclInputPsSgv4 || opcode == kDclInputPsSiv4)) {
          const uint32_t name = p.le32() & 0xFFFF;
          if (!p.ok()) return status::kInvalidParameter;
          int sys = -1;
          if (!vertex && name == 1) sys = kSysPosition;            // SV_Position
          else if (!vertex && name == 9) sys = kSysFrontFacing;    // SV_IsFrontFace
          else if (!vertex && name == 7) sys = kSysPrimitiveId;    // SV_PrimitiveID
          else if (vertex && name == 6) sys = kSysVertexId;        // SV_VertexID
          else if (vertex && name == 8) sys = kSysInstanceId;      // SV_InstanceID
          if (sys < 0) return status::kNotSupported;
          in_sys[o.index[0]] = static_cast<int8_t>(sys);
          continue;
        }
        if (o.type == kOpInput && vertex && o.index[0] >= graphics::kMaxVertexAttributes) return status::kNotSupported;
        continue;
      }
      case kDclIndexableTemp4: return status::kNotSupported;
      default: break;
    }

    Instr ins;
    if (opcode == kRet4) {
      returned = true;
      continue;
    }
    if (opcode == kIf4 || opcode == kDiscard4) {
      Operand c;
      s = read_operand(p, &c);
      if (s != status::kSuccess) return s;
      const bool nonzero = ((token >> 18) & 1) != 0;
      if (opcode == kDiscard4 && vertex) return status::kNotSupported;
      ins.op = opcode == kIf4 ? (nonzero ? kIfNz : kIfZ) : (nonzero ? kDiscardNz : kDiscardZ);
      s = to_src(c, &ins.src[0]);
      if (s != status::kSuccess) return s;
      ins.src_count = 1;
    } else if (opcode == kLoop4 || opcode == kEndloop4 || opcode == kBreak4 || opcode == kContinue4) {
      ins.op = opcode == kLoop4 ? kLoopBegin : opcode == kEndloop4 ? kLoopEnd : opcode == kBreak4 ? kBreak0 : kContinue0;
    } else if (opcode == kBreakc4 || opcode == kContinuec4) {
      Operand c;
      s = read_operand(p, &c);
      if (s != status::kSuccess) return s;
      const bool nonzero = ((token >> 18) & 1) != 0;
      ins.op = opcode == kBreakc4 ? (nonzero ? kBreakNz : kBreakZ) : (nonzero ? kContinueNz : kContinueZ);
      s = to_src(c, &ins.src[0]);
      if (s != status::kSuccess) return s;
      ins.src_count = 1;
    } else if (opcode == kElse4 || opcode == kEndif4) {
      ins.op = opcode == kElse4 ? kElse : kEndIf;
    } else if (opcode == kSincos4) {
      // sincos sin_dst, cos_dst, src: either destination may be null.
      Operand sine, cosine, a;
      if ((s = read_operand(p, &sine)) != status::kSuccess || (s = read_operand(p, &cosine)) != status::kSuccess || (s = read_operand(p, &a)) != status::kSuccess) return s;
      Instr::Src source;
      s = to_src(a, &source);
      if (s != status::kSuccess) return s;
      for (int k = 0; k < 2; ++k) {
        const Operand& d = k == 0 ? sine : cosine;
        if (d.type == kOpNull) continue;
        Instr one_ins;
        one_ins.op = k == 0 ? kSin : kCos;
        one_ins.src[0] = source;
        one_ins.src_count = 1;
        s = to_dst(d, &one_ins.dst);
        if (s != status::kSuccess) return s;
        one_ins.dst.saturate = ((token >> 13) & 1) != 0;
        s = emit(one_ins);
        if (s != status::kSuccess) return s;
      }
      if (!p.at_end()) return status::kInvalidParameter;
      continue;
    } else if (opcode == 38 || opcode == 81 || opcode == 78) {
      // imul / umul (high, low, a, b) and udiv (quotient, remainder, a, b): two destinations, either may be null; the high word of a product is not taken.
      Operand first, second, a, b;
      if ((s = read_operand(p, &first)) != status::kSuccess || (s = read_operand(p, &second)) != status::kSuccess || (s = read_operand(p, &a)) != status::kSuccess ||
          (s = read_operand(p, &b)) != status::kSuccess)
        return s;
      Instr::Src source_a, source_b;
      if ((s = to_src(a, &source_a)) != status::kSuccess || (s = to_src(b, &source_b)) != status::kSuccess) return s;
      const bool divide = opcode == 78;
      if (!divide && first.type != kOpNull) return status::kNotSupported;   // (the high word)
      for (int k = 0; k < 2; ++k) {
        const Operand& d = k == 0 ? first : second;
        if (d.type == kOpNull) continue;
        Instr one_ins;
        one_ins.op = divide ? (k == 0 ? kUDiv : kURem) : (opcode == 38 ? kIMul : kUMul);
        one_ins.src[0] = source_a, one_ins.src[1] = source_b;
        one_ins.src_count = 2;
        s = to_dst(d, &one_ins.dst);
        if (s != status::kSuccess) return s;
        s = emit(one_ins);
        if (s != status::kSuccess) return s;
      }
      if (!p.at_end()) return status::kInvalidParameter;
      continue;
    } else if (opcode == 45 || opcode == 72 || opcode == 61) {
      // ld (dst, integer address, resource), sample_l (dst, coordinate, resource, sampler, level), resinfo (dst, level, resource): Shader Model 4 texture access beyond sample.
      if (vertex) return status::kNotSupported;
      Operand d, address, resource, sampler, lod;
      if ((s = read_operand(p, &d)) != status::kSuccess || (s = read_operand(p, &address)) != status::kSuccess || (s = read_operand(p, &resource)) != status::kSuccess) return s;
      if (opcode == 72 && ((s = read_operand(p, &sampler)) != status::kSuccess || (s = read_operand(p, &lod)) != status::kSuccess)) return s;
      uint32_t unit = resource.index[0];
      if (v51) {
        if (resource.type != kOpResource || resource.dims != 2 || resource.index[0] >= 8 || texture_register[resource.index[0]] < 0 || resource.index[1] != static_cast<uint32_t>(texture_register[resource.index[0]]))
          return status::kNotSupported;
        unit = resource.index[1];
      } else if (resource.type != kOpResource || resource.dims != 1) {
        return status::kNotSupported;
      }
      if (resource.type != kOpResource || unit >= graphics::kMaxTextureUnits || (opcode == 72 && sampler.type != kOpSampler)) return status::kNotSupported;
      ins.op = opcode == 45 ? kTexFetch : opcode == 72 ? kTexLod : kTexSize;
      if (opcode == 61) {
        const uint32_t mode = (token >> 11) & 3;
        if (mode > 2) return status::kNotSupported;
        ins.control = static_cast<uint8_t>(mode);
      }
      s = to_dst(d, &ins.dst);
      if (s != status::kSuccess) return s;
      s = to_src(address, &ins.src[0]);
      if (s != status::kSuccess) return s;
      ins.src[1].type = kSampler, ins.src[1].number = static_cast<uint16_t>(unit);
      ins.src_count = 2;
      if (opcode == 72) {
        s = to_src(lod, &ins.src[2]);
        if (s != status::kSuccess) return s;
        ins.src_count = 3;
      }
      if (resource.components == 2)
        for (uint32_t k = 0; k < 4; ++k) ins.tex_swizzle[k] = resource.swizzle[k];
      program->uses_textures_ = true;
      program->uses_texel_access_ = true;
    } else if (opcode == kSample4) {
      if (vertex) return status::kNotSupported;   // (no textures in the vertex stage)
      Operand d, coordinate, resource, sampler;
      if ((s = read_operand(p, &d)) != status::kSuccess || (s = read_operand(p, &coordinate)) != status::kSuccess || (s = read_operand(p, &resource)) != status::kSuccess ||
          (s = read_operand(p, &sampler)) != status::kSuccess)
        return s;
      uint32_t unit = resource.index[0];
      if (v51) {
        // t[range][index]: the range's register, indexed from it
        if (resource.type != kOpResource || resource.dims != 2 || resource.index[0] >= 8 || texture_register[resource.index[0]] < 0 ||
            resource.index[1] != static_cast<uint32_t>(texture_register[resource.index[0]]))
          return status::kNotSupported;
        unit = resource.index[1];
      } else if (resource.type != kOpResource || resource.dims != 1) {
        return status::kNotSupported;
      }
      if (resource.type != kOpResource || unit >= graphics::kMaxTextureUnits || sampler.type != kOpSampler) return status::kNotSupported;
      ins.op = kTexLd;
      s = to_dst(d, &ins.dst);
      if (s != status::kSuccess) return s;
      s = to_src(coordinate, &ins.src[0]);
      if (s != status::kSuccess) return s;
      ins.src[1].type = kSampler, ins.src[1].number = static_cast<uint16_t>(unit);
      ins.src_count = 2;
      if (resource.components == 2)
        for (uint32_t k = 0; k < 4; ++k) ins.tex_swizzle[k] = resource.swizzle[k];
      program->uses_textures_ = true;
    } else {
      const Alu* alu = nullptr;
      for (const Alu& a : kAlu)
        if (a.dxbc == opcode) alu = &a;
      if (!alu) return status::kNotSupported;
      Operand d;
      s = read_operand(p, &d);
      if (s != status::kSuccess) return s;
      ins.op = alu->op;
      ins.vector = alu->vector;
      ins.mask_result = alu->op == kSeq || alu->op == kSne || alu->op == kSlt || alu->op == kSge;   // (all ones for true, as the integer instructions read them)
      s = to_dst(d, &ins.dst);
      if (s != status::kSuccess) return s;
      ins.dst.saturate = ((token >> 13) & 1) != 0;
      for (uint32_t k = 0; k < alu->sources; ++k) {
        Operand o;
        s = read_operand(p, &o);
        if (s != status::kSuccess) return s;
        s = to_src(o, &ins.src[k]);
        if (s != status::kSuccess) return s;
      }
      ins.src_count = alu->sources;
    }
    if (!p.at_end()) return status::kInvalidParameter;
    s = emit(ins);
    if (s != status::kSuccess) return s;
  }
  if (open_ifs.count() || loops.count()) return status::kInvalidParameter;
  {
    uint32_t total = 0;
    for (uint32_t size : cb_size) total += size;
    if (total > kMaxConstantRegisters) return status::kNotSupported;
    if (total) program->constant_registers_ = total;
  }
  if (vertex) {
    bool writes_position = false;
    for (const Instr& ins : program->code_.view())
      if (ins.dst.slot == kSlotPosition) writes_position = true;
    if (!writes_position) return status::kInvalidParameter;
  }
  *out = util::move(program);
  return status::kSuccess;
}

}  // namespace dxgk
