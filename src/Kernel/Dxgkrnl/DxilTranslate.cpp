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

// DXIL (Shader Model 6) into the Shader Model 5 tokens of the same shader, which D3d9Program::compile_dxbc takes: every interpreter, TGSI and device path built for DXBC then runs it too.
//
// A DXIL container is a DXBC container with a DXIL part (the program header and the LLVM 3.7 bitcode) and the same signature parts as DXBC (ISG1 / OSG1). The bitcode has one function, the
// entry point, whose instructions are LLVM's (arithmetic, comparisons, casts, select, phi, branches) and calls of dx.op.* functions numbered by an opcode constant (loadInput, storeOutput,
// sample, the math functions, createHandle ...). Every scalar value of the function becomes a component of a temporary register (a register allocator reuses them after the last use); inputs,
// constants and constant buffer elements are used where they are, as operands. Branches are made structured again (if / else / endif) from the acyclic control flow dxc writes for if
// statements; a phi is a register the arms write before they join.
//
// Taken: float and 32-bit integer arithmetic, comparisons, select, casts, the math functions of the interpreter (abs, saturate, sin, cos, exp, log, frc, sqrt, rsqrt, rounding, min, max,
// mad, dot products), loadInput / storeOutput, constant buffer loads, createHandle with sample / sample_l / texture load, discard, if / else with phi. Not taken (kNotSupported): loops,
// switches, memory (alloca / load / store / GEP), 16 and 64-bit arithmetic, derivatives, gather, resource arrays with non-constant indices, geometry / hull / domain stages, wave
// operations. A bitcode that is not well formed is kInvalidParameter.
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "Kernel/Dxgkrnl/DxilBitcode.hpp"
#include "Utility/ByteStream.hpp"
#ifdef DXIL_TRACE
#include <cstdio>
#endif

namespace dxgk {

namespace {

using namespace dxil;

constexpr uint32_t kFourDxbc = 0x43425844, kFourDxil = 0x4C495844, kFourIsg1 = 0x31475349, kFourOsg1 = 0x3147534F, kFourShex = 0x58454853, kFourIsgn = 0x4E475349, kFourOsgn = 0x4E47534F;
constexpr uint32_t kNone = ~0u;
constexpr uint32_t kSlots = 128;   // 32 temporaries of four components
constexpr uint32_t kMaxResources = 16;

enum class VK : uint8_t { None, Imm, Temp, Input, Cb, Agg, CbAgg, Handle, Sys, Ptr, HandleVar };

// What a value of the function is, in the program being made.
struct Val {
  VK kind = VK::None;
  uint32_t a = 0, b = 0, c = 0;   // Imm: the bits; Temp / Agg: register (a), component (b); Input: register (a), component (b); Cb: slot (a), register (b), component (c); CbAgg: slot (a), register (b); Handle: class (a), register (b); HandleVar: class (a), range (b), the value of the index (c)
  bool is_mask = false;           // (a boolean: all ones or zero)
  uint8_t d = 0;                  // Input: 0 an input register, 1 an input control point of a domain shader (v[vertex][register]), 2 a patch constant register
};

struct SigElement {
  uint32_t reg = 0, mask = 0, sysval = 0, index = 0, component_type = 0, interp = 2;
};

struct Resource {
  uint32_t id = 0, space = 0, lower = 0, size = 0, kind = 0, bytes = 0, stride = 0;
  bool present = false;
};

// Dxil opcodes (DxilConstants.h OpCode).
enum DxOp : uint32_t {
  kLoadInput = 4, kStoreOutput = 5, kFAbs = 6, kSaturate = 7, kCos = 12, kSin = 13, kExp = 21, kFrc = 22, kLog = 23, kSqrt = 24, kRsqrt = 25, kRoundNe = 26, kRoundNi = 27, kRoundPi = 28, kRoundZ = 29,
  kFMax = 35, kFMin = 36, kIMax = 37, kIMin = 38, kUMax = 39, kUMin = 40, kIMul = 41, kUMul = 42, kFMad = 46, kIMad = 48, kUMad = 49, kDot2 = 54, kDot3 = 55, kDot4 = 56, kCreateHandle = 57,
  kCBufferLoadLegacy = 59, kCalculateLod = 81, kSample = 60, kSampleBias = 61, kSampleLevel = 62, kSampleGrad = 63, kTextureGather = 73, kTextureLoad = 66, kBufferLoad = 68, kBufferStore = 69, kAtomicBinOp = 78, kBarrier = 80, kDiscard = 82,
  kDerivCoarseX = 83, kDerivCoarseY = 84, kDerivFineX = 85, kDerivFineY = 86, kThreadId = 93, kGroupId = 94, kThreadIdInGroup = 95, kFlattenedThreadIdInGroup = 96,
};

class Translator {
 public:
  Translator(allocators::IAllocator& allocator, const Module& module, const Function& function)
      : allocator_(allocator), m_(module), f_(function), out_(allocator), vals_(allocator), last_use_(allocator), alias_of_(allocator), unsupported_(allocator), marks_(allocator) {}

  // Translates the entry function. `isg1` / `osg1` are the signature parts of the container. A hull shader is two functions: `phase` 1 is its control point function (the entry; the
  // words written are its declarations, its temporaries and its code), 2 the patch constant function; 0 is everything else (the whole program).
  NtStatus run(uint32_t program_kind, ConstByteView isg1, ConstByteView osg1, ConstByteView psg1, ByteBuffer* shex, uint32_t phase = 0);
  uint32_t entry_value() const { return entry_value_; }
  uint32_t patch_function() const { return pcf_value_; }
  const uint32_t* hull_state() const { return hs_; }   // control points in, out, domain, partitioning, output primitive, maximum factor (bits)
  NtStatus read_metadata_only() { return read_metadata(); }

 private:
  // --- the module's tables
  uint32_t value_type(uint32_t id) const {
    if (id < m_.module_values) return id < m_.value_types.count() ? m_.value_types[id] : 0;
    const uint64_t local = id - m_.module_values;
    return local < f_.local_count ? m_.local_types[f_.local_first + local] : 0;
  }
  const Constant* constant(uint32_t id) const {
    if (id < m_.module_values) return id < m_.constants.count() ? &m_.constants[id] : nullptr;
    const uint64_t local = id - m_.module_values;
    return local < f_.local_count ? &m_.local_constants[f_.local_first + local] : nullptr;
  }
  uint64_t total_values() const { return uint64_t{m_.module_values} + f_.local_count; }
  bool constant_int(uint32_t id, uint64_t* out) const {
    const Constant* c = constant(id);
    if (!c || (c->kind != ConstantKind::Int && c->kind != ConstantKind::Null)) return false;
    *out = c->kind == ConstantKind::Null ? 0 : c->bits;
    return true;
  }

  // --- metadata
  const Meta* meta_at(uint32_t id) const { return id < m_.meta.count() ? &m_.meta[id] : nullptr; }
  const Meta* operand_of(const Meta* node, uint32_t i) const {
    if (!node || node->kind != MetaKind::Node || i >= node->count) return nullptr;
    const uint32_t ref = m_.meta_refs[node->first + i];
    return ref == 0 ? nullptr : meta_at(ref - 1);
  }
  bool meta_int(const Meta* md, uint64_t* out) const { return md && md->kind == MetaKind::Value && constant_int(md->value, out); }
  NtStatus read_metadata();
  NtStatus read_signature(ConstByteView chunk, SigElement* out, uint32_t* count);

  // --- output
  void put(uint32_t w) {
    if (dry_) return;
    if (!out_.put32(w)) error(status::kNoMemory);
  }
  uint32_t here() const { return static_cast<uint32_t>(out_.size() / 4); }
  uint32_t begin(uint32_t opcode, uint32_t flags = 0) {
    const uint32_t at = here();
    put(opcode | flags);
    return at;
  }
  void finish(uint32_t at) {
    if (dry_) return;
    const uint32_t words = here() - at;
    if (words > 127) return void(error(status::kNotSupported));
    ByteWriter w(out_.writable_view().subview(uint64_t{at} * 4, 4));
    w.le32(word_at(at) | (words << 24));
  }
  uint32_t word_at(uint32_t at) const {
    ByteReader r(out_.view().subview(uint64_t{at} * 4, 4));
    return r.le32();
  }
  bool error(NtStatus s, uint32_t line = __builtin_LINE()) {
#ifdef DXIL_TRACE
    fprintf(stderr, "dxil error %x at line %u\n", static_cast<unsigned>(s), line);
#endif
    (void)line;
    if (status_ == status::kSuccess) status_ = s;
    return false;
  }

  void src(const Val& v, uint32_t modifier = 0);
  void dst_temp(uint32_t reg, uint32_t mask);
  void dst_null() { put(13u << 12); }
  void dst_scalar(const Val& v) { dst_temp(v.a, 1u << v.b); }
  void src_resource(uint32_t reg);
  void src_sampler(uint32_t reg);

  // --- registers
  struct Slot {
    int32_t busy_until = -1;   // the last event that reads the value in it (-1: free)
  };
  bool slot_free(uint32_t slot) const { return slots_[slot].busy_until < pos_; }
  bool alloc_scalar(int32_t until, uint32_t* reg, uint32_t* comp);
  bool alloc_reg(int32_t until, uint32_t* reg);
  Val new_scalar(uint32_t id, bool mask = false);
  Val new_aggregate(uint32_t id);
  int32_t last_use_of(uint32_t id) const { return id < last_use_.count() ? last_use_[id] : -1; }
  bool define(uint32_t id, const Val& v) {
    if (id >= vals_.count()) return error(status::kInvalidParameter);
    vals_[id] = v;
    return true;
  }
  Val use(uint32_t id) {
    if (id >= vals_.count() || vals_[id].kind == VK::None) {
      error(status::kInvalidParameter);   // (used before it is defined)
      return Val();
    }
    return vals_[id];
  }
  void note_use(uint32_t id, int32_t at) {
    if (id < last_use_.count() && at > last_use_[id]) last_use_[id] = at;
  }

  // --- the walk over the blocks (twice: once to find what lives how long, once to emit)
  bool region(uint32_t start, uint32_t stop, uint32_t pred);
  bool visit_block(uint32_t block);
  bool analyze(const Instruction& ins);
  bool translate(const Instruction& ins);
  bool phi_moves(uint32_t pred, uint32_t target);
  uint32_t find_merge(uint32_t a, uint32_t b);
  static constexpr uint32_t kMaxSuccessors = 64;
  void successors_of(uint32_t block, uint32_t* out, uint32_t* count) const;   // (out has room for kMaxSuccessors)
  uint32_t find_merge_of(const uint32_t* targets, uint32_t count);
  bool switch_region(const Instruction& t, uint32_t b, uint32_t stop);
  const Block& block_at(uint32_t i) const { return m_.blocks[f_.first_block_start + i]; }
  const Instruction& instruction_at(uint32_t i) const { return m_.instructions[i]; }
  const Instruction& terminator(uint32_t block) const {
    const Block& b = block_at(block);
    return m_.instructions[b.first + b.count - 1];
  }
  bool alias_cast(const Instruction& ins) const;

  bool dx_call(const Instruction& ins);
  bool binop(const Instruction& ins);
  bool compare(const Instruction& ins);
  bool cast(const Instruction& ins);
  bool unary(const Instruction& ins, uint32_t dxbc_opcode, uint32_t source_arg, uint32_t modifier = 0, bool saturate = false);
  bool binary(const Instruction& ins, uint32_t dxbc_opcode, uint32_t first_arg);
  bool ternary(const Instruction& ins, uint32_t dxbc_opcode, uint32_t first_arg);
  bool sincos(const Instruction& ins, bool cosine);
  bool texture_gather(const Instruction& ins);
  // A lookup whose texture (args[1]) is an element of an array chosen by a variable: the lookup is made once for each element, behind a test of the index, into one result register.
  template <typename F>
  bool with_array(const Instruction& ins, F body) {
    if (ins.arg_count < 2) return error(status::kNotSupported);
    const Val h = use(ins.args[1]);
    if (h.kind != VK::HandleVar) return body();
    const Resource& range = res_[0][h.b];
    const Val result = new_aggregate(ins.result);
    const Val index = use(h.c);
    Val mask;
    mask.kind = VK::Temp;
    if (result.kind == VK::None || !alloc_scalar(pos_ + 1, &mask.a, &mask.b)) return false;
    forced_result_ = result;
    bool ok = true;
    for (uint32_t i = 0; ok && i < range.size; ++i) {
      Val element, number;
      element.kind = VK::Handle, element.a = 0, element.b = h.b, element.c = range.lower + i;
      number.kind = VK::Imm, number.a = range.lower + i;
      {
        const uint32_t at = begin(32);   // ieq
        dst_scalar(mask);
        src(index), src(number);
        finish(at);
      }
      {
        const uint32_t at = begin(31, 1u << 18);   // if_nz
        src(mask);
        finish(at);
      }
      vals_[ins.args[1]] = element;
      ok = body();
      finish(begin(21));   // endif
    }
    vals_[ins.args[1]] = h;
    forced_result_ = Val();
    return ok && define(ins.result, result);
  }
  Val forced_result_;
  bool calculate_lod(const Instruction& ins);
  bool sample_bias_grad(const Instruction& ins, bool gradients);
  bool inverse_trig(const Instruction& ins, uint32_t opcode);
  bool trig(const Instruction& ins, uint32_t opcode);
  bool classify(const Instruction& ins, uint32_t opcode);
  bool signed_divide(const Instruction& ins, bool remainder, const Val& a, const Val& b, const Val& r);
  bool dot(const Instruction& ins, uint32_t n);
  bool sample(const Instruction& ins, bool level);
  bool texture_load(const Instruction& ins);
  bool buffer_load(const Instruction& ins);
  bool buffer_store(const Instruction& ins);
  bool atomic(const Instruction& ins);
  bool thread_value(const Instruction& ins, uint32_t type, bool dimension);
  bool pack(const uint32_t* ids, uint32_t count, uint32_t reg, bool integer);
  bool resource_of(uint32_t handle_id, uint32_t expected_class, Val* out);

  allocators::IAllocator& allocator_;
  const Module& m_;
  const Function& f_;
  ByteBuffer out_;
  List<Val> vals_;
  List<int32_t> last_use_;
  List<uint32_t> alias_of_;
  List<uint8_t> unsupported_;     // a value that is refused if anything uses it (the dummy loads dxc writes are not used)
  List<uint8_t> marks_;
  List<uint8_t> visited_marks_{allocator_};
  Slot slots_[kSlots];
  uint32_t temps_ = 0;
  bool dry_ = true;
  int32_t pos_ = 0;
  uint32_t depth_ = 0, steps_ = 0, if_depth_ = 0;   // (if_depth_: the ifs open at this point of the walk)
  NtStatus status_ = status::kSuccess;
  uint32_t kind_ = 0;
  Resource res_[4][kMaxResources];
  SigElement inputs_[16], outputs_[16];
  uint32_t input_count_ = 0, output_count_ = 0;
  uint32_t threads_[3] = {1, 1, 1};
  uint32_t entry_value_ = kNone;
  struct LoopInfo {
    uint32_t header, latch, exit, depth;
    int32_t start;
  };
  List<LoopInfo> loops_{allocator_};
  List<uint32_t> latch_of_{allocator_};    // by block: the last block that jumps back to it (a loop header), or kNone
  List<int32_t> def_pos_{allocator_};      // by value: the event that defines it (-1: before the code)
  void end_loop_lifetimes(int32_t start, int32_t end);
  List<uint8_t> ptr_ok_{allocator_};      // by value: a pointer into group-shared memory
  uint32_t shared_bytes_[4] = {};
  bool shared_used_[4] = {};
  bool shared_slot(uint32_t global_value, uint32_t* slot, uint32_t* bytes) const;
  bool shared_access(const Instruction& ins, bool store);
  bool shared_pointer(uint32_t id, Val* out);   // a pointer into group-shared memory: a getelementptr instruction's result, or a constant one
  bool is_shared_pointer(uint32_t id) const;
  bool sys_used_[4] = {};          // vThreadID, vThreadGroupID, vThreadIDInGroup, vThreadIDInGroupFlattened
  bool uav_used_[kMaxResources] = {};
  struct PatchElement {
    uint32_t kind = 0, start_row = 0, start_col = 0, rows = 1, cols = 1;
  } pc_[16];
  uint32_t pc_count_ = 0, pcf_value_ = kNone, hs_[7] = {}, phase_ = 0;
  bool output_control_point_used_ = false;
  uint32_t ds_[2] = {};            // a domain shader's state: the domain, the input control points
  bool domain_used_ = false;
  SigElement patch_[16];
  uint32_t patch_count_ = 0;
  uint32_t gs_[5] = {};            // a geometry shader's state: input primitive, most vertices, stream mask, output topology, instances
};

// --- operands --------------------------------------------------------------------------------------------------------------------------------------------------

void Translator::src(const Val& v, uint32_t modifier) {
  switch (v.kind) {
    case VK::Imm:   // (a modifier on a literal is made in the literal: negate, absolute value, negated absolute value)
      put(0x4001u);
      put(modifier == 1 ? v.a ^ 0x80000000u : modifier == 2 ? v.a & 0x7FFFFFFFu : modifier == 3 ? v.a | 0x80000000u : v.a);
      break;
    case VK::Input:
      if (v.d == 3) {   // v[vOutputControlPointID][register]: the first index is relative to that value
        put(2u | (2u << 2) | (v.b << 4) | (1u << 12) | (2u << 20) | (2u << 22));
        put(22u << 12);
        put(v.a);
        break;
      }
      [[fallthrough]];
    case VK::Temp:
      // (an input of a geometry shader names the vertex too: v[vertex][register])
      put(2u | (2u << 2) | (v.b << 4) | ((v.kind == VK::Input ? (v.d == 1 ? 25u : v.d == 2 ? 27u : 1u) : 0u) << 12) | ((v.kind == VK::Input && v.c ? 2u : 1u) << 20) | (modifier ? 1u << 31 : 0u));
      if (modifier) put(1u | (modifier << 6));
      if (v.kind == VK::Input && v.c) put(v.c - 1);
      put(v.a);
      break;
    case VK::Cb:
      put(2u | (2u << 2) | (v.c << 4) | (8u << 12) | (2u << 20) | (modifier ? 1u << 31 : 0u));
      if (modifier) put(1u | (modifier << 6));
      put(v.a);
      put(v.b);
      break;
    case VK::Sys:
      put(2u | (2u << 2) | (v.b << 4) | (v.a << 12));
      break;
    default:
      error(status::kNotSupported);
      break;
  }
}

void Translator::dst_temp(uint32_t reg, uint32_t mask) {
  put(2u | (mask << 4) | (1u << 20));
  put(reg);
}

void Translator::src_resource(uint32_t reg) {
  put(2u | (1u << 2) | (0xE4u << 4) | (7u << 12) | (1u << 20));
  put(reg);
}

void Translator::src_sampler(uint32_t reg) {
  put((6u << 12) | (1u << 20));
  put(reg);
}

// --- registers ---------------------------------------------------------------------------------------------------------------------------------------------------

bool Translator::alloc_scalar(int32_t until, uint32_t* reg, uint32_t* comp) {
  // (a component of a register that is partly in use first, so that whole registers stay free for aggregates)
  for (int pass = 0; pass < 2; ++pass)
    for (uint32_t r = 0; r < kSlots / 4; ++r) {
      bool partly = false;
      for (uint32_t c = 0; c < 4; ++c) partly |= !slot_free(r * 4 + c);
      if (pass == 0 && !partly) continue;
      for (uint32_t c = 0; c < 4; ++c)
        if (slot_free(r * 4 + c)) {
          slots_[r * 4 + c].busy_until = until < pos_ ? pos_ : until;
          *reg = r, *comp = c;
          if (r + 1 > temps_) temps_ = r + 1;
          return true;
        }
    }
  return error(status::kNotSupported);   // (more live values than registers)
}

bool Translator::alloc_reg(int32_t until, uint32_t* reg) {
  for (uint32_t r = 0; r < kSlots / 4; ++r) {
    bool free = true;
    for (uint32_t c = 0; c < 4; ++c) free &= slot_free(r * 4 + c);
    if (!free) continue;
    for (uint32_t c = 0; c < 4; ++c) slots_[r * 4 + c].busy_until = until < pos_ ? pos_ : until;
    *reg = r;
    if (r + 1 > temps_) temps_ = r + 1;
    return true;
  }
  return error(status::kNotSupported);
}

Val Translator::new_scalar(uint32_t id, bool mask) {
  Val v;
  v.kind = VK::Temp;
  v.is_mask = mask;
  if (!alloc_scalar(last_use_of(id), &v.a, &v.b)) v.kind = VK::None;
  return v;
}

Val Translator::new_aggregate(uint32_t id) {
  if (forced_result_.kind != VK::None) return forced_result_;
  Val v;
  v.kind = VK::Agg;
  if (!alloc_reg(last_use_of(id), &v.a)) v.kind = VK::None;
  return v;
}

// --- the walk ----------------------------------------------------------------------------------------------------------------------------------------------------

bool Translator::alias_cast(const Instruction& ins) const {
  const uint32_t from = value_type(ins.operands[0]);
  switch (ins.code) {
    case 11: return true;                                   // bitcast
    case 0: return !m_.is_int(ins.type, 1);                 // trunc (to a boolean is not an alias: the low bit)
    case 1: return !m_.is_int(from, 1);                     // zext from a narrow integer (a boolean becomes 0 or 1)
    case 2: return m_.is_int(from, 1);                      // sext of a boolean: the mask already is all ones
    default: return false;
  }
}

bool Translator::is_shared_pointer(uint32_t id) const {
  if (id < ptr_ok_.count() && ptr_ok_[id]) return true;
  const Constant* c = constant(id);
  uint32_t slot = 0, bytes = 0;
  return c && c->kind == ConstantKind::Gep && shared_slot(static_cast<uint32_t>(c->bits), &slot, &bytes);
}

bool Translator::shared_slot(uint32_t global_value, uint32_t* slot, uint32_t* bytes) const {
  uint32_t ordinal = 0;
  for (const Global& g : m_.globals.view()) {
    if (g.address_space != 3) continue;
    if (g.value == global_value) {
      const Type* t = m_.type_at(g.type);
      if (!t || ordinal >= 4) return false;
      uint32_t elements = 1, element_type = g.type;
      if (t->kind == TypeKind::Array) elements = t->count, element_type = t->first;
      const Type* e = m_.type_at(element_type);
      if (!e || !((e->kind == TypeKind::Int && e->width == 32) || (e->kind == TypeKind::Float && e->width == 32)) || elements == 0 || elements > 8192) return false;
      *slot = ordinal, *bytes = elements * 4;
      return true;
    }
    ++ordinal;
  }
  return false;
}

bool Translator::analyze(const Instruction& ins) {
  const int32_t at = pos_;
  if (ins.has_value && ins.result < def_pos_.count()) def_pos_[ins.result] = at;
  switch (ins.op) {
    case Op::Binop: case Op::Cmp:
      note_use(ins.operands[0], at), note_use(ins.operands[1], at);
      return true;
    case Op::Cast:
      note_use(ins.operands[0], at);
      if (alias_cast(ins) && ins.result < alias_of_.count()) alias_of_[ins.result] = ins.operands[0];
      return true;
    case Op::Select:
      for (uint32_t k = 0; k < 3; ++k) note_use(ins.operands[k], at);
      return true;
    case Op::ExtractValue:
      note_use(ins.operands[0], at);
      if (ins.result < alias_of_.count()) alias_of_[ins.result] = ins.operands[0];
      return true;
    case Op::Call: {
      for (uint32_t k = 0; k < ins.arg_count; ++k) note_use(ins.args[k], at);
      uint64_t dx_op = 0;
      if (ins.arg_count >= 4 && constant_int(ins.args[0], &dx_op) && dx_op == kCreateHandle && ins.result < alias_of_.count()) alias_of_[ins.result] = ins.args[3];   // (a handle on a variable index keeps the index alive)
      return true;
    }
    case Op::Gep: {
      uint32_t slot = 0, bytes = 0;
      if (ins.arg_count == 2 && ins.result < ptr_ok_.count() && shared_slot(ins.operands[0], &slot, &bytes)) {
        uint64_t zero = 1;
        if (!constant_int(ins.args[0], &zero) || zero != 0) return error(status::kNotSupported);
        ptr_ok_[ins.result] = 1;
        note_use(ins.args[1], at);
        if (ins.result < alias_of_.count()) alias_of_[ins.result] = ins.args[1];   // (the pointer keeps its index alive)
        return true;
      }
      if (ins.result < unsupported_.count()) unsupported_[ins.result] = 1;
      return true;
    }
    case Op::Load:
      if (is_shared_pointer(ins.operands[0])) {
        note_use(ins.operands[0], at);
        return true;
      }
      if (ins.result < unsupported_.count()) unsupported_[ins.result] = 1;
      return true;
    case Op::Store:
      if (is_shared_pointer(ins.operands[0])) {
        note_use(ins.operands[0], at), note_use(ins.operands[1], at);
        return true;
      }
      return error(status::kNotSupported);
    case Op::Alloca:
      if (ins.result < unsupported_.count()) unsupported_[ins.result] = 1;
      return true;
    case Op::Phi:
      return true;
    default:
      return error(status::kNotSupported);   // stores, switches
  }
}

bool Translator::phi_moves(uint32_t pred, uint32_t target) {
  if (target >= f_.block_count) return error(status::kInvalidParameter);
  const Block& blk = block_at(target);
  // The moves of one edge happen together: a value that is itself a phi of this block is read before any of them writes.
  struct Move {
    Val to, from;
    uint32_t from_id;
  } moves[16];
  uint32_t count = 0;
  for (uint32_t i = 0; i < blk.count; ++i) {
    const Instruction& phi = instruction_at(blk.first + i);
    if (phi.op != Op::Phi) break;
    uint32_t incoming = kNone;
    for (uint32_t k = 0; k < phi.phi_count; ++k)
      if (m_.phi_blocks[phi.phi_first + k] == pred) incoming = m_.phi_values[phi.phi_first + k];
    if (incoming == kNone || count >= 16) return error(incoming == kNone ? status::kInvalidParameter : status::kNotSupported);
    if (dry_) {
      note_use(incoming, pos_ + static_cast<int32_t>(count));
    } else {
      moves[count].from_id = incoming;
      moves[count].to = vals_[phi.result];
      if (moves[count].to.kind != VK::Temp) return error(status::kInvalidParameter);
      moves[count].from = use(incoming);
    }
    ++count;
  }
  if (dry_) {
    pos_ += static_cast<int32_t>(count);
    return status_ == status::kSuccess;
  }
  bool hazard = false;
  for (uint32_t a = 0; a < count; ++a)
    for (uint32_t c = 0; c < count; ++c)
      if (a != c && moves[a].from.kind == VK::Temp && moves[c].to.kind == VK::Temp && moves[a].from.a == moves[c].to.a && moves[a].from.b == moves[c].to.b) hazard = true;
  if (hazard)
    for (uint32_t a = 0; a < count; ++a) {
      if (moves[a].from.kind != VK::Temp) continue;
      uint32_t reg = 0, comp = 0;
      if (!alloc_scalar(pos_ + static_cast<int32_t>(count), &reg, &comp)) return false;
      Val copy;
      copy.kind = VK::Temp, copy.a = reg, copy.b = comp;
      const uint32_t at = begin(54);
      dst_scalar(copy);
      src(moves[a].from);
      finish(at);
      moves[a].from = copy;
    }
  for (uint32_t a = 0; a < count; ++a) {
    const uint32_t at = begin(54);
    dst_scalar(moves[a].to);
    src(moves[a].from);
    finish(at);
  }
  pos_ += static_cast<int32_t>(count);
  return status_ == status::kSuccess;
}

// A loop ends: what was defined before it and is read inside it is needed until its end (the next time round reads it again).
void Translator::end_loop_lifetimes(int32_t start, int32_t end) {
  for (uint64_t i = 0; i < last_use_.count(); ++i)
    if (last_use_[i] >= start && last_use_[i] < end && def_pos_[i] < start) last_use_[i] = end;
}

void Translator::successors_of(uint32_t block, uint32_t* out, uint32_t* count) const {
  *count = 0;
  const Block& b = block_at(block);
  if (b.count == 0) return;
  const Instruction& t = m_.instructions[b.first + b.count - 1];
  if (t.op == Op::Br) out[(*count)++] = t.blocks[0];
  else if (t.op == Op::CondBr) out[(*count)++] = t.blocks[0], out[(*count)++] = t.blocks[1];
  else if (t.op == Op::Switch) {
    out[(*count)++] = t.blocks[0];
    for (uint32_t k = 0; k < t.phi_count && *count < kMaxSuccessors; ++k) out[(*count)++] = m_.phi_blocks[t.phi_first + k];
  }
}

// The first block both a and b lead to (the join of an if): the lowest block number reachable from both.
uint32_t Translator::find_merge(uint32_t a, uint32_t b) {
  const uint32_t pair[2] = {a, b};
  return find_merge_of(pair, 2);
}

// The lowest block every one of the targets leads to.
uint32_t Translator::find_merge_of(const uint32_t* targets, uint32_t count) {
  const uint32_t n = f_.block_count;
  marks_.clear();
  for (uint64_t i = 0; i < uint64_t{count} * n; ++i)
    if (!marks_.push_back(0)) return kNone;
  for (uint32_t side = 0; side < count; ++side) {
    List<uint32_t> stack(allocator_);
    if (!stack.push_back(targets[side])) return kNone;
    while (stack.count()) {
      const uint32_t x = stack[stack.count() - 1];
      stack.remove_at(stack.count() - 1);
      if (x >= n || marks_[uint64_t{side} * n + x]) continue;
      marks_[uint64_t{side} * n + x] = 1;
      uint32_t next[kMaxSuccessors], successors = 0;
      successors_of(x, next, &successors);
      for (uint32_t k = 0; k < successors; ++k)
        if (next[k] > x && !stack.push_back(next[k])) return kNone;   // (forward edges only)
    }
  }
  for (uint32_t x = 0; x < n; ++x) {
    bool all = true;
    for (uint32_t side = 0; side < count && all; ++side) all = marks_[uint64_t{side} * n + x] != 0;
    if (all) return x;
  }
  return kNone;
}

bool Translator::visit_block(uint32_t b) {
  if (b >= f_.block_count || ++steps_ > 200000) return error(status::kNotSupported);
  if (visited_marks_[b]) return error(status::kNotSupported);
  visited_marks_[b] = 1;
  const Block& blk = block_at(b);
  if (blk.count == 0) return error(status::kInvalidParameter);
  for (uint32_t i = 0; i + 1 < blk.count; ++i) {
    const Instruction& ins = instruction_at(blk.first + i);
    if (ins.op == Op::Phi) continue;
    if (!(dry_ ? analyze(ins) : translate(ins)) || status_ != status::kSuccess) return false;
    ++pos_;
  }
  return true;
}

bool Translator::region(uint32_t start, uint32_t stop, uint32_t pred) {
  if (++depth_ > 24) return error(status::kNotSupported);
  struct Leave {
    uint32_t& depth;
    ~Leave() { --depth; }
  } leave{depth_};
  if (start == stop) return phi_moves(pred, stop);
  // An `if` on a value (nonzero: if_nz, else if_z).
  auto condition = [&](uint32_t value, bool nonzero) {
    if (dry_) {
      note_use(value, pos_);
    } else {
      const Val cond = use(value);
      const uint32_t at = begin(31, nonzero ? 1u << 18 : 0u);
      src(cond);
      finish(at);
    }
    ++pos_;
    ++if_depth_;
  };
  auto simple = [&](uint32_t opcode) {
    if (!dry_) finish(begin(opcode));
  };
  auto end_if = [&]() {
    simple(21);
    --if_depth_;
  };
  // The loop on top of the stack ends here: its exit block is what comes next.
  auto end_loop = [&](uint32_t* exit) -> bool {
    const LoopInfo done = loops_[loops_.count() - 1];
    simple(22);   // endloop
    if (dry_) end_loop_lifetimes(done.start, pos_);
    ++pos_;
    loops_.remove_at(loops_.count() - 1);
    *exit = done.exit;
    return done.exit != kNone || error(status::kNotSupported);   // (a loop with no way out)
  };
  uint32_t b = start;
  for (;;) {
    if (b >= f_.block_count) return error(status::kInvalidParameter);
    if (latch_of_[b] != kNone && !(loops_.count() && loops_[loops_.count() - 1].header == b)) {
      // a loop begins: the moves into its phis were made on the edge that came here
      if (loops_.count() >= 4 || !loops_.push_back({b, latch_of_[b], kNone, depth_, pos_})) return error(status::kNotSupported);
      simple(48);   // loop
      ++pos_;
    }
    if (!visit_block(b)) return false;
    const Instruction& t = terminator(b);
    const int loop_i = static_cast<int>(loops_.count()) - 1;
    auto top = [&]() -> LoopInfo& { return loops_[static_cast<uint64_t>(loop_i)]; };
    // The block the walk goes on in after a loop; false: the region is over.
    auto after_loop = [&](uint32_t exit, bool* done) {
      *done = false;
      if (exit == stop) return *done = true, true;
      if (stop != kNone && exit > stop) return error(status::kNotSupported);
      b = exit;
      return true;
    };
    if (t.op == Op::Ret) {
      if (loop_i >= 0) return error(status::kNotSupported);   // (a return inside a loop)
      if (dry_) {
        if (t.operands[0] != 0) note_use(t.operands[0], pos_);
      } else if (if_depth_ == 0) {
        finish(begin(62));   // (a return in an arm just ends the arm: nothing follows the join of an if whose other arm goes on)
      }
      ++pos_;
      return true;
    }
    if (t.op == Op::Unreachable) return true;
    if (t.op == Op::Br) {
      const uint32_t target = t.blocks[0];
      if (loop_i >= 0 && target == top().header) {   // the back edge
        if (b != top().latch || depth_ != top().depth || !phi_moves(b, target)) return error(status::kNotSupported);
        uint32_t exit = kNone;
        bool done = false;
        if (!end_loop(&exit) || !after_loop(exit, &done)) return false;
        if (done) return true;
        continue;
      }
      if (loop_i >= 0 && target > top().latch) {   // a break
        if (top().exit != kNone && top().exit != target) return error(status::kNotSupported);
        top().exit = target;
        if (!phi_moves(b, target)) return false;
        simple(2);   // break
        return true;
      }
      if (target <= b) return error(status::kNotSupported);
      if (!phi_moves(b, target)) return false;
      if (target == stop) return true;
      if (stop != kNone && target > stop) return error(status::kNotSupported);
      b = target;
      continue;
    }
    if (t.op == Op::Switch) return switch_region(t, b, stop);
    if (t.op != Op::CondBr) return error(status::kNotSupported);
    const uint32_t tb = t.blocks[0], fb = t.blocks[1];
    if (tb == fb) return error(status::kNotSupported);
    if (loop_i >= 0) {
      const bool t_back = tb == top().header, f_back = fb == top().header, t_out = tb > top().latch, f_out = fb > top().latch;
      if (t_back || f_back) {   // the latch: one way back to the top, the other out
        const uint32_t other = t_back ? fb : tb;
        if (t_back == f_back || b != top().latch || depth_ != top().depth || other <= top().latch) return error(status::kNotSupported);
        if (top().exit != kNone && top().exit != other) return error(status::kNotSupported);
        top().exit = other;
        condition(t.operands[0], t_back);
        if (!phi_moves(b, top().header)) return false;
        simple(18);   // else
        if (!phi_moves(b, other)) return false;
        simple(2);    // break
        end_if();
        uint32_t exit = kNone;
        bool done = false;
        if (!end_loop(&exit) || !after_loop(exit, &done)) return false;
        if (done) return true;
        continue;
      }
      if (t_out && f_out) return error(status::kNotSupported);
      if (t_out || f_out) {     // one way leaves the loop: a conditional break, then the loop goes on
        const uint32_t out = t_out ? tb : fb, in = t_out ? fb : tb;
        if (in <= b || (top().exit != kNone && top().exit != out)) return error(status::kNotSupported);
        top().exit = out;
        condition(t.operands[0], t_out);
        if (!phi_moves(b, out)) return false;
        simple(2);    // break
        end_if();
        if (!phi_moves(b, in)) return false;
        if (in == stop) return true;
        if (stop != kNone && in > stop) return error(status::kNotSupported);
        b = in;
        continue;
      }
    }
    if (tb <= b || fb <= b) return error(status::kNotSupported);
    const uint32_t merge = find_merge(tb, fb);
    if (loop_i >= 0 && merge != kNone && merge > top().latch) return error(status::kNotSupported);
    condition(t.operands[0], true);
    if (!region(tb, merge, b)) return false;
    simple(18);   // else
    if (!region(fb, merge, b)) return false;
    end_if();
    if (merge == kNone || merge == stop) return true;
    if (stop != kNone && merge > stop) return error(status::kNotSupported);
    b = merge;
  }
}

// A switch as a chain of tests: the targets in order of their first case, each with the values that lead to it (one test of equalities joined by or), the default last.
bool Translator::switch_region(const Instruction& t, uint32_t b, uint32_t stop) {
  if (loops_.count()) return error(status::kNotSupported);   // (breaks and continues of a loop mix with those of a switch)
  uint32_t targets[kMaxSuccessors], count = 0;
  targets[count++] = t.blocks[0];
  for (uint32_t k = 0; k < t.phi_count; ++k) {
    const uint32_t bb = m_.phi_blocks[t.phi_first + k];
    bool seen = false;
    for (uint32_t i = 0; i < count; ++i) seen |= targets[i] == bb;
    if (!seen && count < kMaxSuccessors) targets[count++] = bb;
  }
  for (uint32_t i = 0; i < count; ++i)
    if (targets[i] <= b || targets[i] >= f_.block_count) return error(status::kNotSupported);
  const uint32_t merge = find_merge_of(targets, count);
  // (the default is the last arm: it has no test; a target the cases do not name is only the default)
  uint32_t order[kMaxSuccessors], tests = 0;
  for (uint32_t k = 0; k < t.phi_count; ++k) {
    const uint32_t bb = m_.phi_blocks[t.phi_first + k];
    bool seen = false;
    for (uint32_t i = 0; i < tests; ++i) seen |= order[i] == bb;
    if (!seen && bb != t.blocks[0]) order[tests++] = bb;
  }
  uint32_t open = 0;
  for (uint32_t i = 0; i < tests; ++i) {
    // the condition: the condition value equals one of this target's case values
    if (dry_) {
      note_use(t.operands[0], pos_);
    } else {
      const Val value = use(t.operands[0]);
      uint32_t reg = 0, comp = 0;
      if (!alloc_scalar(pos_ + 1, &reg, &comp)) return false;
      Val flag;
      flag.kind = VK::Temp, flag.a = reg, flag.b = comp;
      bool first = true;
      for (uint32_t k = 0; k < t.phi_count; ++k) {
        if (m_.phi_blocks[t.phi_first + k] != order[i]) continue;
        uint64_t number = 0;
        if (!constant_int(m_.phi_values[t.phi_first + k], &number)) return error(status::kNotSupported);
        Val c;
        c.kind = VK::Imm, c.a = static_cast<uint32_t>(number);
        if (first) {
          const uint32_t at = begin(32);   // ieq flag, value, c
          dst_scalar(flag);
          src(value), src(c);
          finish(at);
        } else {
          uint32_t r2 = 0, c2 = 0;
          if (!alloc_scalar(pos_ + 1, &r2, &c2)) return false;
          Val other;
          other.kind = VK::Temp, other.a = r2, other.b = c2;
          const uint32_t at = begin(32);
          dst_scalar(other);
          src(value), src(c);
          finish(at);
          const uint32_t or_at = begin(60);   // or flag, flag, other
          dst_scalar(flag);
          src(flag), src(other);
          finish(or_at);
        }
        first = false;
      }
      const uint32_t at = begin(31, 1u << 18);   // if_nz flag
      src(flag);
      finish(at);
    }
    ++pos_;
    ++open;
    ++if_depth_;
    if (!region(order[i], merge, b)) return false;
    if (!dry_) finish(begin(18));   // else
  }
  if (!region(t.blocks[0], merge, b)) return false;
  for (uint32_t i = 0; i < open; ++i) {
    if (!dry_) finish(begin(21));   // endif
    --if_depth_;
  }
  if (merge == kNone || merge == stop) return true;
  if (stop != kNone && merge > stop) return error(status::kNotSupported);
  // (the walk goes on at the join: the caller's loop does that when it is told, so this is the one place a region continues from inside a helper)
  return region(merge, stop, kNone);
}

// --- instructions ------------------------------------------------------------------------------------------------------------------------------------------------

bool Translator::translate(const Instruction& ins) {
  switch (ins.op) {
    case Op::Binop: return binop(ins);
    case Op::Cmp: return compare(ins);
    case Op::Cast: return cast(ins);
    case Op::Select: {
      const Val cond = use(ins.operands[2]), a = use(ins.operands[0]), b = use(ins.operands[1]);
      const Val r = new_scalar(ins.result);
      if (r.kind == VK::None) return false;
      const uint32_t at = begin(55);
      dst_scalar(r);
      src(cond), src(a), src(b);
      finish(at);
      return define(ins.result, r);
    }
    case Op::ExtractValue: {
      const Val agg = use(ins.operands[0]);
      Val v;
      if (agg.kind == VK::Agg) {
        if (ins.index >= 4) return error(status::kNotSupported);   // (the status word of a sample)
        v.kind = VK::Temp, v.a = agg.a, v.b = ins.index;
      } else if (agg.kind == VK::CbAgg) {
        if (ins.index >= 4) return error(status::kNotSupported);
        v.kind = VK::Cb, v.a = agg.a, v.b = agg.b, v.c = ins.index;
      } else {
        return error(status::kNotSupported);
      }
      return define(ins.result, v);
    }
    case Op::Call: return dx_call(ins);
    case Op::Gep: {
      // (a pointer into group-shared memory: the memory and the index; any other pointer is refused only if it is used)
      uint32_t slot = 0, bytes = 0;
      if (!(ins.result < ptr_ok_.count() && ptr_ok_[ins.result])) return true;
      if (!shared_slot(ins.operands[0], &slot, &bytes)) return error(status::kNotSupported);
      Val v;
      v.kind = VK::Ptr, v.a = slot, v.c = ins.args[1];
      shared_bytes_[slot] = bytes;
      shared_used_[slot] = true;
      return define(ins.result, v);
    }
    case Op::Load: case Op::Store: {
      if (!is_shared_pointer(ins.operands[0])) return true;   // (a load nothing uses)
      return shared_access(ins, ins.op == Op::Store);
    }
    case Op::Alloca: return true;   // (refused only if something uses the result)
    default: return error(status::kNotSupported);
  }
}

bool Translator::binop(const Instruction& ins) {
  const Val a = use(ins.operands[0]), b = use(ins.operands[1]);
  const bool floating = m_.is_float(ins.type);
  if (!floating && !m_.is_int(ins.type, 32) && !m_.is_int(ins.type, 1)) return error(status::kNotSupported);   // (16 / 64-bit)
  if (floating && m_.types[ins.type].width != 32) return error(status::kNotSupported);
  const Val r = new_scalar(ins.result, m_.is_int(ins.type, 1));
  if (r.kind == VK::None) return false;
  auto three = [&](uint32_t opcode, bool negate_b = false) {
    const uint32_t at = begin(opcode);
    dst_scalar(r);
    src(a);
    src(b, negate_b ? 1u : 0u);
    finish(at);
  };
  if (floating) {
    switch (ins.code) {
      case 0: three(0); break;          // fadd
      case 1: three(0, true); break;    // fsub: add a, -b
      case 2: three(56); break;         // fmul
      case 4: three(14); break;         // fdiv
      default: return error(status::kNotSupported);
    }
    return define(ins.result, r);
  }
  switch (ins.code) {
    case 0: three(30); break;           // add
    case 1: {                           // sub: a + -b
      Val negative = b;
      if (b.kind == VK::Imm) {
        negative.a = static_cast<uint32_t>(0u - b.a);
      } else {
        uint32_t reg = 0, comp = 0;
        if (!alloc_scalar(pos_, &reg, &comp)) return false;
        negative.kind = VK::Temp, negative.a = reg, negative.b = comp;
        const uint32_t at = begin(40);   // ineg
        dst_scalar(negative);
        src(b);
        finish(at);
      }
      const uint32_t at = begin(30);
      dst_scalar(r);
      src(a), src(negative);
      finish(at);
      break;
    }
    case 2: {                           // mul: imul high(null), low, a, b
      const uint32_t at = begin(38);
      dst_null();
      dst_scalar(r);
      src(a), src(b);
      finish(at);
      break;
    }
    case 3: case 5: {                   // udiv / urem
      const uint32_t at = begin(78);
      if (ins.code == 3) dst_scalar(r), dst_null();
      else dst_null(), dst_scalar(r);
      src(a), src(b);
      finish(at);
      break;
    }
    case 4: case 6: {                   // sdiv / srem
      if (!signed_divide(ins, ins.code == 6, a, b, r)) return false;
      break;
    }
    case 7: three(41); break;           // shl
    case 8: three(85); break;           // lshr
    case 9: three(42); break;           // ashr
    case 10: three(1); break;           // and
    case 11: three(60); break;          // or
    case 12: three(87); break;          // xor
    default: return error(status::kNotSupported);
  }
  return define(ins.result, r);
}

bool Translator::compare(const Instruction& ins) {
  const Val a = use(ins.operands[0]), b = use(ins.operands[1]);
  const uint32_t operand_type = value_type(ins.operands[0]);
  const bool floating = m_.is_float(operand_type);
  if (floating ? m_.types[operand_type].width != 32 : !m_.is_int(operand_type, 32) && !m_.is_int(operand_type, 1)) return error(status::kNotSupported);
  uint32_t opcode = 0;
  bool swap = false;
  if (floating) {
    switch (ins.code) {
      case 1: case 9: opcode = 24; break;            // eq
      case 6: case 14: opcode = 57; break;           // ne
      case 4: case 12: opcode = 49; break;           // lt
      case 5: case 13: opcode = 29, swap = true; break;   // a <= b is b >= a
      case 2: case 10: opcode = 49, swap = true; break;   // a > b is b < a
      case 3: case 11: opcode = 29; break;           // ge
      default: return error(status::kNotSupported);  // ordered / unordered tests, constants
    }
  } else {
    switch (ins.code) {
      case 32: opcode = 32; break;                   // ieq
      case 33: opcode = 39; break;                   // ine
      case 34: opcode = 79, swap = true; break;      // ugt
      case 35: opcode = 80; break;                   // uge
      case 36: opcode = 79; break;                   // ult
      case 37: opcode = 80, swap = true; break;      // ule
      case 38: opcode = 34, swap = true; break;      // sgt
      case 39: opcode = 33; break;                   // sge
      case 40: opcode = 34; break;                   // slt
      case 41: opcode = 33, swap = true; break;      // sle
      default: return error(status::kNotSupported);
    }
  }
  const Val r = new_scalar(ins.result, true);
  if (r.kind == VK::None) return false;
  const uint32_t at = begin(opcode);
  dst_scalar(r);
  src(swap ? b : a), src(swap ? a : b);
  finish(at);
  return define(ins.result, r);
}

bool Translator::cast(const Instruction& ins) {
  const Val a = use(ins.operands[0]);
  const uint32_t from = value_type(ins.operands[0]);
  if (alias_cast(ins)) {
    Val v = a;
    v.is_mask = m_.is_int(ins.type, 1);
    return define(ins.result, v);
  }
  uint32_t opcode = 0;
  switch (ins.code) {
    case 3: opcode = 28; break;   // fptoui
    case 4: opcode = 27; break;   // fptosi
    case 5: opcode = 86; break;   // uitofp
    case 6: opcode = 43; break;   // sitofp
    case 1:                       // zext of a boolean: 0 or 1
    case 0:                       // trunc to a boolean: the low bit, as a mask
      break;
    default: return error(status::kNotSupported);
  }
  if ((ins.code >= 3 && ins.code <= 6) && ((m_.is_float(from) && m_.types[from].width != 32) || (m_.is_float(ins.type) && m_.types[ins.type].width != 32) ||
                                         (m_.is_int(from) && m_.types[from].width != 32) || (m_.is_int(ins.type) && m_.types[ins.type].width != 32)))
    return error(status::kNotSupported);
  const Val r = new_scalar(ins.result, m_.is_int(ins.type, 1));
  if (r.kind == VK::None) return false;
  if (opcode) {
    const uint32_t at = begin(opcode);
    dst_scalar(r);
    src(a);
    finish(at);
  } else {
    Val one;
    one.kind = VK::Imm, one.a = 1;
    const uint32_t at = begin(1);   // and r, a, 1
    dst_scalar(r);
    src(a), src(one);
    finish(at);
    if (ins.code == 0) {            // (a mask: all ones for 1)
      const uint32_t neg = begin(40);
      dst_scalar(r);
      src(r);
      finish(neg);
    }
  }
  return define(ins.result, r);
}

// --- dx.op calls --------------------------------------------------------------------------------------------------------------------------------------------------

bool Translator::resource_of(uint32_t handle_id, uint32_t expected_class, Val* out) {
  const Val h = use(handle_id);
  if (h.kind != VK::Handle || h.a != expected_class || h.b >= kMaxResources || !res_[h.a][h.b].present) return error(status::kNotSupported);
  *out = h;
  return true;
}

bool Translator::pack(const uint32_t* ids, uint32_t count, uint32_t reg, bool) {
  for (uint32_t k = 0; k < count; ++k) {
    const Val v = use(ids[k]);
    const uint32_t at = begin(54);
    dst_temp(reg, 1u << k);
    src(v);
    finish(at);
  }
  return status_ == status::kSuccess;
}

bool Translator::unary(const Instruction& ins, uint32_t dxbc_opcode, uint32_t source_arg, uint32_t modifier, bool saturate) {
  const Val a = use(ins.args[source_arg]);
  if (!m_.is_float(ins.type)) return error(status::kNotSupported);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None) return false;
  const uint32_t at = begin(dxbc_opcode, saturate ? 1u << 13 : 0u);
  dst_scalar(r);
  src(a, modifier);
  finish(at);
  return define(ins.result, r);
}

bool Translator::binary(const Instruction& ins, uint32_t dxbc_opcode, uint32_t first_arg) {
  const Val a = use(ins.args[first_arg]), b = use(ins.args[first_arg + 1]);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None) return false;
  const uint32_t at = begin(dxbc_opcode);
  dst_scalar(r);
  src(a), src(b);
  finish(at);
  return define(ins.result, r);
}

bool Translator::ternary(const Instruction& ins, uint32_t dxbc_opcode, uint32_t first_arg) {
  const Val a = use(ins.args[first_arg]), b = use(ins.args[first_arg + 1]), c = use(ins.args[first_arg + 2]);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None) return false;
  const uint32_t at = begin(dxbc_opcode);
  dst_scalar(r);
  src(a), src(b), src(c);
  finish(at);
  return define(ins.result, r);
}

bool Translator::sincos(const Instruction& ins, bool cosine) {
  const Val a = use(ins.args[1]);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None) return false;
  const uint32_t at = begin(77);
  if (cosine) dst_null(), dst_scalar(r);
  else dst_scalar(r), dst_null();
  src(a);
  finish(at);
  return define(ins.result, r);
}

// tan (sin / cos) and the hyperbolic functions (from 2^(x log2 e) and its inverse).
bool Translator::trig(const Instruction& ins, uint32_t opcode) {
  const Val x = use(ins.args[1]);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None || !m_.is_float(ins.type)) return error(status::kNotSupported);
  auto scratch = [&]() {
    Val v;
    v.kind = VK::Temp;
    if (!alloc_scalar(pos_ + 1, &v.a, &v.b)) v.kind = VK::None;
    return v;
  };
  auto imm = [](float f) {
    Val v;
    v.kind = VK::Imm, v.a = __builtin_bit_cast(uint32_t, f);
    return v;
  };
  auto emit3 = [&](uint32_t op, const Val& d, const Val& a, const Val& b, uint32_t mod_b = 0) {
    const uint32_t at = begin(op);
    dst_scalar(d);
    src(a), src(b, mod_b);
    finish(at);
  };
  if (opcode == 14) {   // tan
    const Val s = scratch(), c = scratch();
    if (s.kind == VK::None || c.kind == VK::None) return false;
    const uint32_t at = begin(77);
    dst_scalar(s), dst_scalar(c);
    src(x);
    finish(at);
    emit3(14, r, s, c);
    return define(ins.result, r);
  }
  // t = x * log2(e); e1 = 2^t; e2 = 2^-t
  const Val t = scratch(), e1 = scratch(), e2 = scratch();
  if (t.kind == VK::None || e1.kind == VK::None || e2.kind == VK::None) return false;
  emit3(56, t, x, imm(1.4426950408889634f));
  {
    const uint32_t at = begin(25);
    dst_scalar(e1);
    src(t);
    finish(at);
  }
  {
    const uint32_t at = begin(25);
    dst_scalar(e2);
    src(t, 1);
    finish(at);
  }
  const Val half = imm(0.5f);
  if (opcode == 18 || opcode == 19) {   // cosh / sinh: (e1 +- e2) / 2
    emit3(0, t, e1, e2, opcode == 19 ? 1u : 0u);
    emit3(56, r, t, half);
    return define(ins.result, r);
  }
  // tanh: (e1 - e2) / (e1 + e2)
  const Val numerator = scratch();
  if (numerator.kind == VK::None) return false;
  emit3(0, numerator, e1, e2, 1);
  emit3(0, t, e1, e2);
  emit3(14, r, numerator, t);
  return define(ins.result, r);
}

// acos, asin and atan from polynomials (Abramowitz and Stegun 4.4.46 for acos, a minimax one for atan after the reduction to [0, 1] by 1 / x), with the sign and range put back by movc.
bool Translator::inverse_trig(const Instruction& ins, uint32_t opcode) {
  const Val x = use(ins.args[1]);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None || !m_.is_float(ins.type)) return error(status::kNotSupported);
  auto scratch = [&]() {
    Val v;
    v.kind = VK::Temp;
    if (!alloc_scalar(pos_ + 1, &v.a, &v.b)) v.kind = VK::None;
    return v;
  };
  auto imm = [](float f) {
    Val v;
    v.kind = VK::Imm, v.a = __builtin_bit_cast(uint32_t, f);
    return v;
  };
  auto emit3 = [&](uint32_t op, const Val& d, const Val& a, const Val& b, uint32_t mod_b = 0, uint32_t mod_a = 0) {
    const uint32_t at = begin(op);
    dst_scalar(d);
    src(a, mod_a), src(b, mod_b);
    finish(at);
  };
  auto emit4 = [&](uint32_t op, const Val& d, const Val& a, const Val& b, const Val& c) {
    const uint32_t at = begin(op);
    dst_scalar(d);
    src(a), src(b), src(c);
    finish(at);
  };
  const Val a = scratch(), p = scratch(), n = scratch(), q = scratch();
  if (a.kind == VK::None || p.kind == VK::None || n.kind == VK::None || q.kind == VK::None) return false;
  const Val zero = imm(0.0f), one = imm(1.0f), half_pi = imm(1.5707963267948966f);
  {
    const uint32_t at = begin(54);   // a = |x|
    dst_scalar(a);
    src(x, 2);
    finish(at);
  }
  emit3(49, n, x, zero);   // n = x < 0
  if (opcode == 17) {
    const Val g = scratch(), inv = scratch(), r2 = scratch();
    if (g.kind == VK::None || inv.kind == VK::None || r2.kind == VK::None) return false;
    emit3(49, g, one, a);          // g = 1 < |x|
    emit3(14, inv, one, a);
    emit4(55, q, g, inv, a);       // q = g ? 1 / |x| : |x|
    emit3(56, r2, q, q);
    static const float c[5] = {0.0208351f, -0.0851330f, 0.1801410f, -0.3302995f, 0.9998660f};
    emit4(50, p, r2, imm(c[0]), imm(c[1]));
    for (int i = 2; i < 5; ++i) emit4(50, p, p, r2, imm(c[i]));
    emit3(56, p, p, q);
    emit3(0, inv, half_pi, p, 1);  // pi / 2 - p
    emit4(55, p, g, inv, p);
    emit3(0, q, zero, p, 1);       // -p
    emit4(55, r, n, q, p);
    return define(ins.result, r);
  }
  // acos(|x|) = sqrt(1 - |x|) * P(|x|)
  static const float c[8] = {-0.0012624911f, 0.0066700901f, -0.0170881256f, 0.0308918810f, -0.0501743046f, 0.0889789874f, -0.2145988016f, 1.5707963050f};
  emit4(50, p, a, imm(c[0]), imm(c[1]));
  for (int i = 2; i < 8; ++i) emit4(50, p, p, a, imm(c[i]));
  emit3(0, q, one, a, 1);
  {
    const uint32_t at = begin(75);   // sqrt
    dst_scalar(q);
    src(q);
    finish(at);
  }
  emit3(56, p, p, q);                  // acos(|x|)
  emit3(0, q, imm(3.14159265358979f), p, 1);   // pi - acos(|x|)
  emit4(55, p, n, q, p);               // acos(x)
  if (opcode == 15) return (emit3(0, r, p, zero), define(ins.result, r));
  emit3(0, r, half_pi, p, 1);          // asin(x)
  return define(ins.result, r);
}

// IsNaN, IsInf, IsFinite: masks.
bool Translator::classify(const Instruction& ins, uint32_t opcode) {
  const Val x = use(ins.args[1]);
  const Val r = new_scalar(ins.result, true);
  if (r.kind == VK::None || !m_.is_float(value_type(ins.args[1]))) return error(status::kNotSupported);
  Val infinity;
  infinity.kind = VK::Imm, infinity.a = 0x7F800000u;
  const uint32_t at = begin(opcode == 8 ? 57 : opcode == 9 ? 24 : 49);   // ne x, x / eq |x|, inf / lt |x|, inf
  dst_scalar(r);
  if (opcode == 8) src(x), src(x);
  else src(x, 2), src(infinity);
  finish(at);
  return define(ins.result, r);
}

// Signed division and remainder from the unsigned one: the magnitudes divided, the sign put back.
bool Translator::signed_divide(const Instruction&, bool remainder, const Val& a, const Val& b, const Val& r) {
  auto scratch = [&]() {
    Val v;
    v.kind = VK::Temp;
    if (!alloc_scalar(pos_ + 1, &v.a, &v.b)) v.kind = VK::None;
    return v;
  };
  Val thirty_one;
  thirty_one.kind = VK::Imm, thirty_one.a = 31;
  Val sign_a = scratch(), sign_b = scratch(), abs_a = scratch(), abs_b = scratch(), t = scratch();
  if (sign_a.kind == VK::None || sign_b.kind == VK::None || abs_a.kind == VK::None || abs_b.kind == VK::None || t.kind == VK::None) return false;
  auto three = [&](uint32_t op, const Val& d, const Val& x, const Val& y) {
    const uint32_t at = begin(op);
    dst_scalar(d);
    src(x), src(y);
    finish(at);
  };
  auto negate = [&](const Val& d, const Val& x) {   // d = -x
    const uint32_t at = begin(40);
    dst_scalar(d);
    src(x);
    finish(at);
  };
  // |v| = (v ^ s) - s with s = v >> 31
  auto magnitude = [&](const Val& sign, const Val& out, const Val& v) {
    three(42, sign, v, thirty_one);       // ishr
    three(87, t, v, sign);                // xor
    negate(out, sign);                    // out = -s
    three(30, out, t, out);               // t + -s
  };
  magnitude(sign_a, abs_a, a);
  magnitude(sign_b, abs_b, b);
  {
    const uint32_t at = begin(78);        // udiv quotient, remainder
    if (remainder) dst_null(), dst_scalar(r);
    else dst_scalar(r), dst_null();
    src(abs_a), src(abs_b);
    finish(at);
  }
  // the quotient has the sign of a xor b, the remainder that of a
  Val sign = sign_a;
  if (!remainder) {
    three(87, t, sign_a, sign_b);
    sign = t;
  }
  Val negative = abs_b;                   // (a scratch)
  three(87, negative, r, sign);           // r ^ s
  const Val s2 = abs_a;
  negate(s2, sign);                       // -s
  three(30, r, negative, s2);             // (r ^ s) + -s
  return true;
}

bool Translator::dot(const Instruction& ins, uint32_t n) {
  if (ins.arg_count != 1 + 2 * n) return error(status::kInvalidParameter);
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None) return false;
  for (uint32_t k = 0; k < n; ++k) {
    const Val a = use(ins.args[1 + k]), b = use(ins.args[1 + n + k]);
    if (k == 0) {
      const uint32_t at = begin(56);   // mul
      dst_scalar(r);
      src(a), src(b);
      finish(at);
    } else {
      const uint32_t at = begin(50);   // mad r, a, b, r
      dst_scalar(r);
      src(a), src(b), src(r);
      finish(at);
    }
  }
  return define(ins.result, r);
}

bool Translator::sample(const Instruction& ins, bool level) {
  Val srv, smp;
  if (ins.arg_count < 11 || !resource_of(ins.args[1], 0, &srv) || !resource_of(ins.args[2], 3, &smp)) return error(status::kNotSupported);
  for (uint32_t k = 7; k < 10; ++k) {   // offsets: none
    uint64_t v = 0;
    const Constant* c = constant(ins.args[k]);
    if (!c || (c->kind != ConstantKind::Undef && !(constant_int(ins.args[k], &v) && v == 0))) return error(status::kNotSupported);
  }
  const uint32_t t_reg = srv.c, s_reg = smp.c;
  const Val r = new_aggregate(ins.result);
  uint32_t scratch = 0;
  if (r.kind == VK::None || !alloc_reg(pos_, &scratch)) return false;
  if (!pack(ins.args + 3, 4, scratch, false)) return false;
  uint32_t lod_arg = 0;
  Val lod;
  if (level) lod = use(ins.args[10]), lod_arg = 1;
  const uint32_t at = begin(level ? 72 : 69);
  dst_temp(r.a, 0xF);
  put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));   // the coordinate register, xyzw
  put(scratch);
  src_resource(t_reg);
  src_sampler(s_reg);
  if (lod_arg) src(lod);
  finish(at);
  return define(ins.result, r);
}

// CalculateLOD of a 2D texture: the lod instruction's x (clamped) or y (not).
bool Translator::calculate_lod(const Instruction& ins) {
  Val srv, smp;
  uint64_t clamped = 0;
  if (ins.arg_count < 7 || !resource_of(ins.args[1], 0, &srv) || !resource_of(ins.args[2], 3, &smp) || !constant_int(ins.args[6], &clamped)) return error(status::kNotSupported);
  const Val r = new_scalar(ins.result);
  uint32_t scratch = 0, result = 0;
  if (r.kind == VK::None || !alloc_reg(pos_, &scratch) || !alloc_reg(pos_, &result)) return false;
  if (!pack(ins.args + 3, 2, scratch, false)) return false;
  {
    const uint32_t at = begin(108);   // lod
    dst_temp(result, 0xF);
    put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
    put(scratch);
    src_resource(srv.c);
    src_sampler(smp.c);
    finish(at);
  }
  Val component;
  component.kind = VK::Temp, component.a = result, component.b = clamped ? 0u : 1u;
  const uint32_t at = begin(54);
  dst_scalar(r);
  src(component);
  finish(at);
  return define(ins.result, r);
}

// sample_b (a bias) and sample_d (two gradients) of a 2D texture.
bool Translator::sample_bias_grad(const Instruction& ins, bool gradients) {
  Val srv, smp;
  if (ins.arg_count < (gradients ? 17u : 12u) || !resource_of(ins.args[1], 0, &srv) || !resource_of(ins.args[2], 3, &smp)) return error(status::kNotSupported);
  for (uint32_t k = 7; k < 10; ++k) {   // offsets: none
    uint64_t v = 0;
    const Constant* c = constant(ins.args[k]);
    if (!c || (c->kind != ConstantKind::Undef && !(constant_int(ins.args[k], &v) && v == 0))) return error(status::kNotSupported);
  }
  const Val r = new_aggregate(ins.result);
  uint32_t scratch = 0, dx = 0, dy = 0;
  if (r.kind == VK::None || !alloc_reg(pos_, &scratch)) return false;
  if (!pack(ins.args + 3, 2, scratch, false)) return false;
  Val bias;
  if (gradients) {
    if (!alloc_reg(pos_, &dx) || !alloc_reg(pos_, &dy) || !pack(ins.args + 10, 2, dx, false) || !pack(ins.args + 13, 2, dy, false)) return false;
  } else {
    bias = use(ins.args[10]);
  }
  const uint32_t at = begin(gradients ? 73 : 74);
  dst_temp(r.a, 0xF);
  put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
  put(scratch);
  src_resource(srv.c);
  src_sampler(smp.c);
  if (gradients) {
    put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20)), put(dx);
    put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20)), put(dy);
  } else {
    src(bias);
  }
  finish(at);
  return define(ins.result, r);
}

// gather4: the component of the four texels of the footprint named by the channel argument, picked by the swizzle of the resource operand.
bool Translator::texture_gather(const Instruction& ins) {
  Val srv, smp;
  uint64_t channel = 0;
  if (ins.arg_count < 10 || !resource_of(ins.args[1], 0, &srv) || !resource_of(ins.args[2], 3, &smp) || !constant_int(ins.args[9], &channel) || channel > 3) return error(status::kNotSupported);
  for (uint32_t k = 7; k < 9; ++k) {   // offsets: none
    uint64_t v = 0;
    const Constant* c = constant(ins.args[k]);
    if (!c || (c->kind != ConstantKind::Undef && !(constant_int(ins.args[k], &v) && v == 0))) return error(status::kNotSupported);
  }
  const Val r = new_aggregate(ins.result);
  uint32_t scratch = 0;
  if (r.kind == VK::None || !alloc_reg(pos_, &scratch)) return false;
  if (!pack(ins.args + 3, 2, scratch, false)) return false;
  const uint32_t at = begin(109);
  dst_temp(r.a, 0xF);
  put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
  put(scratch);
  put(2u | (1u << 2) | (static_cast<uint32_t>(channel) * 0x55u << 4) | (7u << 12) | (1u << 20)), put(srv.c);
  src_sampler(smp.c);
  finish(at);
  return define(ins.result, r);
}

bool Translator::texture_load(const Instruction& ins) {
  Val srv;
  if (ins.arg_count < 9 || !resource_of(ins.args[1], 0, &srv)) return error(status::kNotSupported);
  for (uint32_t k = 6; k < 9; ++k) {
    uint64_t v = 0;
    const Constant* c = constant(ins.args[k]);
    if (!c || (c->kind != ConstantKind::Undef && !(constant_int(ins.args[k], &v) && v == 0))) return error(status::kNotSupported);
  }
  const Val r = new_aggregate(ins.result);
  uint32_t scratch = 0;
  if (r.kind == VK::None || !alloc_reg(pos_, &scratch)) return false;
  const uint32_t ids[3] = {ins.args[3], ins.args[4], ins.args[5]};
  if (!pack(ids, 3, scratch, true)) return false;
  const Val mip = use(ins.args[2]);
  {
    const uint32_t at = begin(54);
    dst_temp(scratch, 1u << 3);
    src(mip);
    finish(at);
  }
  const uint32_t at = begin(45);
  dst_temp(r.a, 0xF);
  put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
  put(scratch);
  src_resource(srv.c);
  finish(at);
  return define(ins.result, r);
}

bool Translator::thread_value(const Instruction& ins, uint32_t type, bool dimension) {
  uint64_t axis = 0;
  if (dimension && (ins.arg_count < 2 || !constant_int(ins.args[1], &axis) || axis > 2)) return error(status::kNotSupported);
  Val v;
  v.kind = VK::Sys, v.a = type, v.b = static_cast<uint32_t>(axis);
  sys_used_[type == 32 ? 0 : type == 33 ? 1 : type == 34 ? 2 : 3] = true;
  return define(ins.result, v);
}

// A UAV operand with a write mask (a store's destination) or a full swizzle (a load's source).
uint32_t srv_token() { return 2u | (1u << 2) | (0xE4u << 4) | (7u << 12) | (1u << 20); }

uint32_t uav_token(uint32_t mask, bool destination) {
  return destination ? (2u | (mask << 4) | (30u << 12) | (1u << 20)) : (2u | (1u << 2) | (0xE4u << 4) | (30u << 12) | (1u << 20));
}

bool Translator::buffer_load(const Instruction& ins) {
  Val h;
  if (ins.arg_count < 4) return error(status::kNotSupported);
  if (!resource_of(ins.args[1], use(ins.args[1]).a == 0 ? 0 : 1, &h)) return false;
  const bool view = h.a == 0;   // (a shader resource view: t# in place of u#)
  const Resource& r = res_[h.a][h.b];
  const bool structured = r.kind == 12;
  if (!structured && r.kind != 11) return error(status::kNotSupported);
  const Val index = use(ins.args[2]);
  const Val r_out = new_aggregate(ins.result);
  if (r_out.kind == VK::None) return false;
  const uint32_t slot = h.c;
  if (structured) {
    const Val offset = use(ins.args[3]);   // (undef: the start of the element)
    const uint32_t at = begin(167);        // ld_structured dst, index, offset, u#
    dst_temp(r_out.a, 0xF);
    src(index), src(offset);
    put(view ? srv_token() : uav_token(0, false)), put(slot);
    finish(at);
  } else {
    const uint32_t at = begin(165);        // ld_raw dst, byte address, u#
    dst_temp(r_out.a, 0xF);
    src(index);
    put(view ? srv_token() : uav_token(0, false)), put(slot);
    finish(at);
  }
  return define(ins.result, r_out);
}

bool Translator::buffer_store(const Instruction& ins) {
  Val h;
  uint64_t mask = 0;
  if (ins.arg_count < 9 || !resource_of(ins.args[1], 1, &h) || !constant_int(ins.args[8], &mask)) return error(status::kNotSupported);
  const Resource& r = res_[1][h.b];
  const bool structured = r.kind == 12;
  if (!structured && r.kind != 11) return error(status::kNotSupported);
  mask &= 0xF;
  const Val index = use(ins.args[2]);
  uint32_t scratch = 0;
  if (!alloc_reg(pos_, &scratch)) return false;
  // (the values of the components that are written, each in its own component of the register: a store writes the words of the mask in order)
  uint32_t words = 0;
  for (uint32_t k = 0; k < 4; ++k) {
    if (!(mask >> k & 1)) continue;
    const Val v = use(ins.args[4 + k]);
    const uint32_t at = begin(54);
    dst_temp(scratch, 1u << words);
    src(v);
    finish(at);
    ++words;
  }
  if (words == 0) return true;
  const uint32_t write_mask = (1u << words) - 1;
  const uint32_t slot = h.c;
  if (structured) {
    const Val offset = use(ins.args[3]);
    const uint32_t at = begin(168);        // store_structured u#.mask, index, offset, value
    put(uav_token(write_mask, true)), put(slot);
    src(index), src(offset);
    put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
    put(scratch);
    finish(at);
  } else {
    const uint32_t at = begin(166);        // store_raw u#.mask, byte address, value
    put(uav_token(write_mask, true)), put(slot);
    src(index);
    put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
    put(scratch);
    finish(at);
  }
  return status_ == status::kSuccess;
}

bool Translator::atomic(const Instruction& ins) {
  Val h;
  uint64_t op = 0;
  if (ins.arg_count < 7 || !resource_of(ins.args[1], 1, &h) || !constant_int(ins.args[2], &op)) return error(status::kNotSupported);
  const Resource& r = res_[1][h.b];
  const bool structured = r.kind == 12;
  if (!structured && r.kind != 11) return error(status::kNotSupported);
  static const uint32_t kImm[] = {180, 181, 182, 183, 187, 186, 189, 188, 184};   // add, and, or, xor, imin, imax, umin, umax, exchange
  if (op >= sizeof kImm / sizeof kImm[0]) return error(status::kNotSupported);
  const Val c0 = use(ins.args[3]), value = use(ins.args[6]);
  const Val r_out = new_scalar(ins.result);
  if (r_out.kind == VK::None) return false;
  uint32_t scratch = 0;
  if (!alloc_reg(pos_, &scratch)) return false;
  {
    const uint32_t at = begin(54);   // the address: the index in x (and the offset in y of a structured buffer)
    dst_temp(scratch, 1);
    src(c0);
    finish(at);
  }
  if (structured) {
    const Val c1 = use(ins.args[4]);
    const uint32_t at = begin(54);
    dst_temp(scratch, 2);
    src(c1);
    finish(at);
  }
  const uint32_t slot = h.c;
  const uint32_t at = begin(kImm[op]);
  dst_scalar(r_out);
  put((30u << 12) | (1u << 20)), put(slot);   // the unordered access view
  put(2u | (1u << 2) | (0xE4u << 4) | (1u << 20));
  put(scratch);
  src(value);
  finish(at);
  return define(ins.result, r_out);
}

bool Translator::shared_pointer(uint32_t id, Val* out) {
  const Constant* c = constant(id);
  uint32_t slot = 0, bytes = 0;
  if (c && c->kind == ConstantKind::Gep && shared_slot(static_cast<uint32_t>(c->bits), &slot, &bytes)) {
    out->kind = VK::Ptr, out->a = slot, out->c = static_cast<uint32_t>(c->bits >> 32);
    shared_bytes_[slot] = bytes;
    shared_used_[slot] = true;
    return true;
  }
  *out = use(id);
  return out->kind == VK::Ptr;
}

bool Translator::shared_access(const Instruction& ins, bool store) {
  Val ptr;
  if (!shared_pointer(ins.operands[0], &ptr)) return error(status::kInvalidParameter);
  // the byte address: 4 * the index
  Val address;
  const Constant* c = constant(ptr.c);
  if (c && (c->kind == ConstantKind::Int || c->kind == ConstantKind::Null)) {
    address.kind = VK::Imm, address.a = static_cast<uint32_t>((c->kind == ConstantKind::Null ? 0 : c->bits) * 4);
  } else {
    uint32_t reg = 0, comp = 0;
    if (!alloc_scalar(pos_, &reg, &comp)) return false;
    address.kind = VK::Temp, address.a = reg, address.b = comp;
    const Val index = use(ptr.c);
    Val two;
    two.kind = VK::Imm, two.a = 2;
    const uint32_t at = begin(41);   // ishl address, index, 2
    dst_scalar(address);
    src(index), src(two);
    finish(at);
  }
  const uint32_t operand = 2u | (1u << 2) | (0x00u << 4) | (31u << 12) | (1u << 20);   // (.xxxx: the first word, whichever component of the destination it goes to)
  if (store) {
    const Val value = use(ins.operands[1]);
    const uint32_t at = begin(166);   // store_raw g#.x, address, value
    put(2u | (1u << 4) | (31u << 12) | (1u << 20)), put(ptr.a);
    src(address), src(value);
    finish(at);
    return status_ == status::kSuccess;
  }
  const Val r = new_scalar(ins.result);
  if (r.kind == VK::None) return false;
  const uint32_t at = begin(165);     // ld_raw dst, address, g#
  dst_scalar(r);
  src(address);
  put(operand), put(ptr.a);
  finish(at);
  return define(ins.result, r);
}

bool Translator::dx_call(const Instruction& ins) {
  const Constant* oc = ins.arg_count ? constant(ins.args[0]) : nullptr;
  uint64_t opcode = 0;
  bool is_dx = false;
  for (const Function& fn : m_.functions.view())
    if (fn.value == ins.operands[0]) is_dx = fn.name.view().starts_with(StringView("dx.op."));
  if (!is_dx || !oc || !constant_int(ins.args[0], &opcode)) return error(status::kNotSupported);
  const bool has_value = ins.has_value;
  switch (opcode) {
    case kLoadInput: {
      uint64_t id = 0, row = 0, col = 0;
      if (ins.arg_count < 4 || !constant_int(ins.args[1], &id) || !constant_int(ins.args[2], &row) || !constant_int(ins.args[3], &col) || id >= input_count_) return error(status::kNotSupported);
      const SigElement& e = inputs_[id];
      uint32_t first = 0;
      while (first < 4 && !(e.mask >> first & 1)) ++first;
      Val v;
      v.kind = VK::Input, v.a = e.reg + static_cast<uint32_t>(row), v.b = first + static_cast<uint32_t>(col);
      if (v.b >= 4 || v.a >= 16) return error(status::kInvalidParameter);
      if (kind_ == 2 || kind_ == 3 || kind_ == 4) {   // (a geometry or hull shader: the vertex of the primitive; a domain shader: the control point)
        uint64_t vertex = 0;
        if (ins.arg_count < 5) return error(status::kNotSupported);
        if (kind_ == 3 && phase_ == 1 && !constant_int(ins.args[4], &vertex)) {
          // (the control point being made: v[vOutputControlPointID][register])
          const Val which = use(ins.args[4]);
          if (which.kind != VK::Sys || which.a != 22) return error(status::kNotSupported);
          v.d = 3;
        } else {
          if (!constant_int(ins.args[4], &vertex) || vertex >= (kind_ == 2 ? 6u : 32u)) return error(status::kNotSupported);
          v.c = static_cast<uint32_t>(vertex) + 1;
          v.d = kind_ == 4 ? 1 : 0;   // (a hull shader's inputs are v[vertex][register], as a geometry shader's)
        }
      }
      return define(ins.result, v);
    }
    case kStoreOutput: {
      uint64_t id = 0, row = 0, col = 0;
      if (ins.arg_count < 5 || !constant_int(ins.args[1], &id) || !constant_int(ins.args[2], &row) || !constant_int(ins.args[3], &col) || id >= output_count_) return error(status::kNotSupported);
      const SigElement& e = outputs_[id];
      uint32_t first = 0;
      while (first < 4 && !(e.mask >> first & 1)) ++first;
      const uint32_t reg = e.reg + static_cast<uint32_t>(row), comp = first + static_cast<uint32_t>(col);
      if (comp >= 4 || reg >= 16) return error(status::kInvalidParameter);
      const Val v = use(ins.args[4]);
      const uint32_t at = begin(54);
      put(2u | ((1u << comp) << 4) | (2u << 12) | (1u << 20));
      put(reg);
      src(v);
      finish(at);
      return true;
    }
    case 8: case 9: case 10: return classify(ins, static_cast<uint32_t>(opcode));
    case 14: case 18: case 19: case 20: return trig(ins, static_cast<uint32_t>(opcode));
    case 15: case 16: case 17: return inverse_trig(ins, static_cast<uint32_t>(opcode));
    case kDerivCoarseX: return unary(ins, 122, 1);
    case kDerivCoarseY: return unary(ins, 124, 1);
    case kDerivFineX: return unary(ins, 123, 1);
    case kDerivFineY: return unary(ins, 125, 1);
    case kFAbs: return unary(ins, 54, 1, 2);
    case kSaturate: return unary(ins, 54, 1, 0, true);
    case kSin: return sincos(ins, false);
    case kCos: return sincos(ins, true);
    case kExp: return unary(ins, 25, 1);
    case kFrc: return unary(ins, 26, 1);
    case kLog: return unary(ins, 47, 1);
    case kSqrt: return unary(ins, 75, 1);
    case kRsqrt: return unary(ins, 68, 1);
    case kRoundNe: return unary(ins, 64, 1);
    case kRoundNi: return unary(ins, 65, 1);
    case kRoundPi: return unary(ins, 66, 1);
    case kRoundZ: return unary(ins, 67, 1);
    case kFMax: return binary(ins, 52, 1);
    case kFMin: return binary(ins, 51, 1);
    case kIMax: return binary(ins, 36, 1);
    case kIMin: return binary(ins, 37, 1);
    case kUMax: return binary(ins, 83, 1);
    case kUMin: return binary(ins, 84, 1);
    case kFMad: return ternary(ins, 50, 1);
    case kIMad: return ternary(ins, 35, 1);
    case kUMad: return ternary(ins, 82, 1);
    case kDot2: return dot(ins, 2);
    case kDot3: return dot(ins, 3);
    case kDot4: return dot(ins, 4);
    case kCreateHandle: {
      uint64_t cls = 0, range = 0, index = 0;
      if (ins.arg_count >= 4 && constant_int(ins.args[1], &cls) && constant_int(ins.args[2], &range) && !constant_int(ins.args[3], &index) && cls == 0 && range < kMaxResources && res_[0][range].present &&
          res_[0][range].size != 0xFFFFFFFFu && res_[0][range].size >= 2 && res_[0][range].size <= 8) {
        Val v;   // (a texture array indexed by a variable: the lookups that use it test the index)
        v.kind = VK::HandleVar, v.a = 0, v.b = static_cast<uint32_t>(range), v.c = ins.args[3];
        return define(ins.result, v);
      }
      if (ins.arg_count < 4 || !constant_int(ins.args[1], &cls) || !constant_int(ins.args[2], &range) || !constant_int(ins.args[3], &index) || cls > 3 || range >= kMaxResources || !res_[cls][range].present)
        return error(status::kNotSupported);   // (a resource array indexed by a variable: only a small array of textures)
      // (the index is the register: the range's lower bound for a single resource)
      const Resource& range_info = res_[cls][range];
      if (index < range_info.lower || (range_info.size != 0xFFFFFFFFu && index - range_info.lower >= range_info.size)) return error(status::kInvalidParameter);
      Val v;
      v.kind = VK::Handle, v.a = static_cast<uint32_t>(cls), v.b = static_cast<uint32_t>(range), v.c = static_cast<uint32_t>(index);
      return define(ins.result, v);
    }
    case kCBufferLoadLegacy: {
      Val h;
      uint64_t reg = 0;
      if (ins.arg_count < 3 || !resource_of(ins.args[1], 2, &h) || !constant_int(ins.args[2], &reg)) return error(status::kNotSupported);
      const Resource& cb = res_[2][h.b];
      if (reg >= cb.bytes / 16) return error(status::kInvalidParameter);
      Val v;
      v.kind = VK::CbAgg, v.a = h.c, v.b = static_cast<uint32_t>(reg);
      return define(ins.result, v);
    }
    case kSample: return with_array(ins, [&] { return sample(ins, false); });
    case kSampleLevel: return with_array(ins, [&] { return sample(ins, true); });
    case kSampleBias: case kSampleGrad: return with_array(ins, [&] { return sample_bias_grad(ins, opcode == kSampleGrad); });
    case kTextureLoad: return with_array(ins, [&] { return texture_load(ins); });
    case kTextureGather: return with_array(ins, [&] { return texture_gather(ins); });
    case kCalculateLod: return calculate_lod(ins);
    case kThreadId: return thread_value(ins, 32, true);
    case kGroupId: return thread_value(ins, 33, true);
    case kThreadIdInGroup: return thread_value(ins, 34, true);
    case kFlattenedThreadIdInGroup: return thread_value(ins, 36, false);
    case kBufferLoad: return buffer_load(ins);
    case kBufferStore: return buffer_store(ins);
    case kAtomicBinOp: return atomic(ins);
    case kBarrier: {
      uint64_t mode = 0;
      if (ins.arg_count < 2 || !constant_int(ins.args[1], &mode)) return error(status::kNotSupported);
      // (DXIL: 1 sync the threads, 2 fence UAVs globally, 4 fence UAVs in the group, 8 fence group-shared memory; DXBC: 1 sync, 2 shared memory, 4 UAVs in the group, 8 UAVs globally)
      const uint32_t flags = (mode & 1 ? 1u : 0u) | (mode & 8 ? 2u : 0u) | (mode & 4 ? 4u : 0u) | (mode & 2 ? 8u : 0u);
      finish(begin(190, flags << 11));
      return true;
    }
    case 107: {   // outputControlPointID (a hull shader's control point function)
      if (kind_ != 3 || phase_ != 1) return error(status::kNotSupported);
      Val v;
      v.kind = VK::Sys, v.a = 22, v.b = 0;
      output_control_point_used_ = true;
      return define(ins.result, v);
    }
    case 106: {   // storePatchConstant
      uint64_t id = 0, row = 0, col = 0;
      if (kind_ != 3 || phase_ != 2 || ins.arg_count < 5 || !constant_int(ins.args[1], &id) || !constant_int(ins.args[2], &row) || !constant_int(ins.args[3], &col) || id >= pc_count_) return error(status::kNotSupported);
      const PatchElement& e = pc_[id];
      // (a tessellation factor is read from the x of its register by the hull shader's users: dxc packs them into other components)
      const bool factor = e.kind == 25 || e.kind == 26;
      const uint32_t reg = e.start_row + static_cast<uint32_t>(row), comp = factor ? 0 : e.start_col + static_cast<uint32_t>(col);
      if (reg >= graphics::kMaxPatchRegisters || comp >= 4) return error(status::kInvalidParameter);
      const Val v = use(ins.args[4]);
      const uint32_t at = begin(54);
      put(2u | ((1u << comp) << 4) | (2u << 12) | (1u << 20));
      put(reg);
      src(v);
      finish(at);
      return true;
    }
    case 104: {   // loadPatchConstant (a domain shader)
      uint64_t id = 0, row = 0, col = 0;
      if (kind_ != 4 || ins.arg_count < 4 || !constant_int(ins.args[1], &id) || !constant_int(ins.args[2], &row) || !constant_int(ins.args[3], &col) || id >= patch_count_) return error(status::kNotSupported);
      const SigElement& e = patch_[id];
      uint32_t first = 0;
      while (first < 4 && !(e.mask >> first & 1)) ++first;
      Val v;
      v.kind = VK::Input, v.d = 2, v.a = e.reg + static_cast<uint32_t>(row), v.b = first + static_cast<uint32_t>(col);
      if (v.b >= 4 || v.a >= 32) return error(status::kInvalidParameter);
      return define(ins.result, v);
    }
    case 105: {   // domainLocation
      uint64_t axis = 0;
      if (kind_ != 4 || ins.arg_count < 2 || !constant_int(ins.args[1], &axis) || axis > 2) return error(status::kNotSupported);
      Val v;
      v.kind = VK::Sys, v.a = 28, v.b = static_cast<uint32_t>(axis);
      domain_used_ = true;
      return define(ins.result, v);
    }
    case 97: case 98: {   // emitStream / cutStream
      uint64_t stream = ~uint64_t{0};
      if (kind_ != 2 || ins.arg_count < 2 || !constant_int(ins.args[1], &stream) || stream >= graphics::kMaxGeometryStreams || !(gs_[2] >> stream & 1)) return error(status::kNotSupported);
      if (gs_[2] == 1) {
        finish(begin(opcode == 97 ? 19 : 9));
      } else {
        const uint32_t at = begin(opcode == 97 ? 117 : 118);   // emit_stream / cut_stream m#
        put((16u << 12) | (1u << 20)), put(static_cast<uint32_t>(stream));
        finish(at);
      }
      return true;
    }
    case kDiscard: {
      if (ins.arg_count < 2) return error(status::kInvalidParameter);
      const Val cond = use(ins.args[1]);
      const uint32_t at = begin(13, 1u << 18);   // discard_nz
      src(cond);
      finish(at);
      return true;
    }
    default:
      (void)has_value;
      return error(status::kNotSupported);
  }
}

// --- metadata and signatures ------------------------------------------------------------------------------------------------------------------------------------

NtStatus Translator::read_metadata() {
  const NamedMeta* entry_points = m_.find_named(StringView("dx.entryPoints"));
  if (!entry_points || entry_points->count < 1) return status::kInvalidParameter;
  const Meta* entry = meta_at(m_.meta_refs[entry_points->first]);
  const Meta* fn = operand_of(entry, 0);
  if (!entry || !fn || fn->kind != MetaKind::Value) return status::kInvalidParameter;
  entry_value_ = fn->value;
  // resources: srvs, uavs, cbuffers, samplers
  const Meta* resources = operand_of(entry, 3);
  for (uint32_t cls = 0; resources && cls < 4; ++cls) {
    const Meta* list = operand_of(resources, cls);
    if (!list) continue;
    for (uint32_t i = 0; i < list->count; ++i) {
      const Meta* node = operand_of(list, i);
      uint64_t id = 0, space = 0, lower = 0, size = 0, extra = 0;
      if (!meta_int(operand_of(node, 0), &id) || !meta_int(operand_of(node, 3), &space) || !meta_int(operand_of(node, 4), &lower) || !meta_int(operand_of(node, 5), &size)) return status::kInvalidParameter;
      meta_int(operand_of(node, 6), &extra);
      if (id >= kMaxResources || space != 0) return status::kNotSupported;
      Resource& r = res_[cls][id];
      // (a structured buffer's element size: the tag 1 of the node's tags, the last operand)
      const Meta* tags = operand_of(node, cls == 1 ? 10 : cls == 0 ? 8 : 99);
      for (uint32_t k = 0; tags && k + 1 < tags->count; k += 2) {
        uint64_t tag = 0, value = 0;
        if (meta_int(operand_of(tags, k), &tag) && tag == 1 && meta_int(operand_of(tags, k + 1), &value)) r.stride = static_cast<uint32_t>(value);
      }
      r.present = true, r.id = static_cast<uint32_t>(id), r.lower = static_cast<uint32_t>(lower), r.size = static_cast<uint32_t>(size);
      if (cls == 2) r.bytes = static_cast<uint32_t>(extra);
      else r.kind = static_cast<uint32_t>(extra);
    }
  }
  // the signature elements' interpolation modes (the container's signature parts say the rest)
  const Meta* signatures = operand_of(entry, 2);
  if (const Meta* patch_list = operand_of(signatures, 2)) {
    for (uint32_t i = 0; i < patch_list->count && i < 16; ++i) {
      const Meta* el = operand_of(patch_list, i);
      uint64_t kind = 0, rows = 1, cols = 1, row = 0, col = 0;
      meta_int(operand_of(el, 3), &kind), meta_int(operand_of(el, 6), &rows), meta_int(operand_of(el, 7), &cols), meta_int(operand_of(el, 8), &row), meta_int(operand_of(el, 9), &col);
      pc_[i].kind = static_cast<uint32_t>(kind), pc_[i].rows = static_cast<uint32_t>(rows), pc_[i].cols = static_cast<uint32_t>(cols), pc_[i].start_row = static_cast<uint32_t>(row), pc_[i].start_col = static_cast<uint32_t>(col);
      pc_count_ = i + 1;
    }
  }
  for (uint32_t which = 0; signatures && which < 2; ++which) {
    const Meta* list = operand_of(signatures, which);
    SigElement* elements = which == 0 ? inputs_ : outputs_;
    for (uint32_t i = 0; list && i < list->count && i < 16; ++i) {
      uint64_t interp = 2;
      if (meta_int(operand_of(operand_of(list, i), 5), &interp) && interp <= 7) elements[i].interp = static_cast<uint32_t>(interp);
    }
  }
  // properties: (tag, value) pairs; 4 is the thread group of a compute shader
  const Meta* props = operand_of(entry, 4);
  for (uint32_t i = 0; props && i + 1 < props->count; i += 2) {
    uint64_t tag = 0;
    if (meta_int(operand_of(props, i), &tag) && tag == 1) {
      const Meta* state = operand_of(props, i + 1);
      for (uint32_t k = 0; k < 5; ++k) {
        uint64_t v = 0;
        meta_int(operand_of(state, k), &v);
        gs_[k] = static_cast<uint32_t>(v);
      }
    }
    if (meta_int(operand_of(props, i), &tag) && tag == 3) {   // a hull shader's state: the patch constant function, control points in and out, domain, partitioning, output, maximum factor
      const Meta* state = operand_of(props, i + 1);
      const Meta* fn = operand_of(state, 0);
      if (fn && fn->kind == MetaKind::Value) pcf_value_ = fn->value;
      for (uint32_t k = 1; k < 7; ++k) {
        uint64_t v = 0;
        meta_int(operand_of(state, k), &v);
        hs_[k - 1] = static_cast<uint32_t>(v);
      }
    }
    if (meta_int(operand_of(props, i), &tag) && tag == 2) {
      const Meta* state = operand_of(props, i + 1);
      for (uint32_t k = 0; k < 2; ++k) {
        uint64_t v = 0;
        meta_int(operand_of(state, k), &v);
        ds_[k] = static_cast<uint32_t>(v);
      }
    }
    if (meta_int(operand_of(props, i), &tag) && tag == 4) {
      const Meta* group = operand_of(props, i + 1);
      for (uint32_t k = 0; k < 3; ++k) {
        uint64_t v = 1;
        meta_int(operand_of(group, k), &v);
        threads_[k] = static_cast<uint32_t>(v);
      }
    }
  }
  return status::kSuccess;
}

NtStatus Translator::read_signature(ConstByteView chunk, SigElement* out, uint32_t* count) {
  *count = 0;
  if (chunk.empty()) return status::kSuccess;
  ByteReader in(chunk);
  const uint32_t n = in.le32();
  in.le32();
  if (!in.ok() || n > 16) return status::kNotSupported;
  for (uint32_t i = 0; i < n; ++i) {
    ConstByteView entry = in.take(32);
    if (!in.ok()) return status::kInvalidParameter;
    ByteReader e(entry);
    e.le32();   // stream
    e.le32();   // name
    out[i].index = e.le32();
    out[i].sysval = e.le32();
    out[i].component_type = e.le32();
    out[i].reg = e.le32();
    out[i].mask = e.u8();
    if (out[i].reg >= 16) return status::kNotSupported;
  }
  *count = n;
  return status::kSuccess;
}

// --- the whole function ------------------------------------------------------------------------------------------------------------------------------------------

NtStatus Translator::run(uint32_t program_kind, ConstByteView isg1, ConstByteView osg1, ConstByteView psg1, ByteBuffer* shex, uint32_t phase) {
  kind_ = program_kind;
  phase_ = phase;
  NtStatus s = read_metadata();
  if (s != status::kSuccess) return s;
  if ((phase == 2 ? pcf_value_ : entry_value_) != f_.value) return status::kNotSupported;
  if ((s = read_signature(isg1, inputs_, &input_count_)) != status::kSuccess || (s = read_signature(osg1, outputs_, &output_count_)) != status::kSuccess || (s = read_signature(psg1, patch_, &patch_count_)) != status::kSuccess) return s;
  // (interpolation modes were read by position before the counts were known)
  const uint64_t values = total_values();
  if (values > (1u << 16) || f_.block_count == 0) return status::kInvalidParameter;
  for (uint64_t i = 0; i < values; ++i) {
    Val v;
    if (const Constant* c = constant(static_cast<uint32_t>(i))) {
      const uint32_t type = value_type(static_cast<uint32_t>(i));
      if ((c->kind == ConstantKind::Int && m_.is_int(type, 1)) ) v.kind = VK::Imm, v.a = c->bits & 1 ? 0xFFFFFFFFu : 0, v.is_mask = true;
      else if (c->kind == ConstantKind::Int && (m_.is_int(type, 32) || m_.is_int(type, 8) || m_.is_int(type, 16))) v.kind = VK::Imm, v.a = static_cast<uint32_t>(c->bits);
      else if (c->kind == ConstantKind::Float && m_.is_float(type) && m_.types[type].width == 32) v.kind = VK::Imm, v.a = static_cast<uint32_t>(c->bits);
      else if (c->kind == ConstantKind::Null || c->kind == ConstantKind::Undef) v.kind = VK::Imm, v.a = 0;
    }
    if (!vals_.push_back(v) || !last_use_.push_back(-1) || !alias_of_.push_back(kNone) || !unsupported_.push_back(0) || !ptr_ok_.push_back(0)) return status::kNoMemory;
  }
  // loops: the last block that jumps back to each block; where each value is defined
  for (uint32_t i = 0; i < f_.block_count; ++i)
    if (!latch_of_.push_back(kNone)) return status::kNoMemory;
  for (uint32_t x = 0; x < f_.block_count; ++x) {
    uint32_t next[kMaxSuccessors], count = 0;
    successors_of(x, next, &count);
    for (uint32_t k = 0; k < count; ++k)
      if (next[k] <= x && next[k] < f_.block_count && (latch_of_[next[k]] == kNone || latch_of_[next[k]] < x)) latch_of_[next[k]] = x;
  }
  for (uint64_t i = 0; i < values; ++i)
    if (!def_pos_.push_back(-1)) return status::kNoMemory;
  // the first pass: what is used how long
  visited_marks_.clear();
  for (uint32_t i = 0; i < f_.block_count; ++i)
    if (!visited_marks_.push_back(0)) return status::kNoMemory;
  dry_ = true;
  pos_ = 0, depth_ = 0, steps_ = 0, if_depth_ = 0;
  if (!region(0, kNone, kNone)) return status_ != status::kSuccess ? status_ : status::kNotSupported;
  for (uint64_t i = values; i-- > 0;)
    if (alias_of_[i] != kNone && alias_of_[i] < values && last_use_[i] > last_use_[alias_of_[i]]) last_use_[alias_of_[i]] = last_use_[i];
  for (uint64_t i = 0; i < values; ++i)
    if (unsupported_[i] && last_use_[i] >= 0) return status::kNotSupported;
  // the second pass: the code
  for (uint32_t i = 0; i < f_.block_count; ++i) visited_marks_[i] = 0;
  dry_ = false;
  pos_ = 0, depth_ = 0, steps_ = 0, if_depth_ = 0;
  for (uint32_t b = 0; b < f_.block_count; ++b) {
    const Block& blk = block_at(b);
    for (uint32_t i = 0; i < blk.count; ++i) {
      const Instruction& phi = instruction_at(blk.first + i);
      if (phi.op != Op::Phi) break;
      if (m_.is_float(phi.type) ? m_.types[phi.type].width != 32 : !m_.is_int(phi.type, 32) && !m_.is_int(phi.type, 1)) return status::kNotSupported;
      uint32_t reg = 0, comp = 0;
      if (!alloc_scalar(last_use_of(phi.result), &reg, &comp)) return status_;
      Val v;
      v.kind = VK::Temp, v.a = reg, v.b = comp, v.is_mask = m_.is_int(phi.type, 1);
      vals_[phi.result] = v;
    }
  }
  if (!region(0, kNone, kNone)) return status_ != status::kSuccess ? status_ : status::kNotSupported;
  if (status_ != status::kSuccess) return status_;

  // --- the declarations and the program
  ByteBuffer decl(allocator_);
  auto word = [&](uint32_t w) { return decl.put32(w); };
  if (program_kind == 3) {
    // a phase of a hull shader: its declarations (the control point function: the inputs, the output control point number, the outputs; the patch constant function: the outputs), the
    // temporaries and the code
    if (phase == 0) return status::kInvalidParameter;
    const uint32_t points_in = hs_[0];
    if (phase == 1) {
      for (uint32_t i = 0; i < kMaxResources; ++i) {   // (the constant buffers of both functions)
        const Resource& r = res_[2][i];
        if (!r.present) continue;
        if (r.bytes == 0 || r.bytes % 16 || r.lower >= 14) return status::kNotSupported;
        word(89u | (4u << 24)), word(2u | (1u << 2) | (0xE4u << 4) | (8u << 12) | (2u << 20)), word(r.lower), word(r.bytes / 16);
      }
      if (points_in < 1 || points_in > graphics::kMaxPatchControlPoints) return status::kNotSupported;
      for (uint32_t i = 0; i < input_count_; ++i) {
        const SigElement& e = inputs_[i];
        const uint32_t indexed = 2u | ((e.mask & 0xF) << 4) | (1u << 12) | (2u << 20);
        if (e.sysval == 0) word(95u | (4u << 24)), word(indexed), word(points_in), word(e.reg);
        else if (e.sysval == 1) word(97u | (5u << 24)), word(indexed), word(points_in), word(e.reg), word(1);
        else return status::kNotSupported;
      }
      if (output_control_point_used_) word(95u | (2u << 24)), word(2u | (1u << 4) | (22u << 12));
      for (uint32_t i = 0; i < output_count_; ++i) {
        const SigElement& e = outputs_[i];
        const uint32_t operand = 2u | ((e.mask & 0xF) << 4) | (2u << 12) | (1u << 20);
        if (e.sysval == 0) word(101u | (3u << 24)), word(operand), word(e.reg);
        else if (e.sysval == 1) word(103u | (4u << 24)), word(operand), word(e.reg), word(1);
        else return status::kNotSupported;
      }
    } else {
      // the patch constants: the tessellation factors are system values with a name by domain (3 tri, 2... see the table), the rest plain registers
      const uint32_t domain_kind = hs_[2];
      for (uint32_t i = 0; i < pc_count_; ++i) {
        const PatchElement& e = pc_[i];
        for (uint32_t r = 0; r < e.rows; ++r) {
          const uint32_t reg = e.start_row + r;
          if (reg >= graphics::kMaxPatchRegisters) return status::kNotSupported;
          uint32_t name = 0;
          if (e.kind == 25) name = domain_kind == 3 ? 11 + r : domain_kind == 2 ? 17 + r : domain_kind == 1 ? (r == 0 ? 22 : 21) : 0;   // SV_TessFactor
          else if (e.kind == 26) name = domain_kind == 3 ? 15 + r : domain_kind == 2 ? 20 : 0;                                              // SV_InsideTessFactor
          const uint32_t operand = 2u | (name ? 1u << 4 : ((1u << e.cols) - 1) << (4 + e.start_col)) | (2u << 12) | (1u << 20);
          if (name) word(103u | (4u << 24)), word(operand), word(reg), word(name);
          else word(101u | (3u << 24)), word(operand), word(reg);
        }
      }
    }
    word(104u | (2u << 24)), word(temps_);
    if (!shex->append(decl.view()) || !shex->append(out_.view())) return status::kNoMemory;
    return status::kSuccess;
  }
  const bool pixel = program_kind == 0, vertex = program_kind == 1, compute = program_kind == 5, geometry = program_kind == 2, domain = program_kind == 4;
  if (!pixel && !vertex && !compute && !geometry && !domain) return status::kNotSupported;
  if (domain) {
    if (ds_[0] < 1 || ds_[0] > 3 || ds_[1] < 1 || ds_[1] > graphics::kMaxPatchControlPoints) return status::kNotSupported;
    word(147u | (ds_[1] << 11) | (1u << 24));   // dcl_input_control_point_count
    word(149u | (ds_[0] << 11) | (1u << 24));   // dcl_tessellator_domain
    if (domain_used_) word(95u | (2u << 24)), word(2u | (7u << 4) | (28u << 12));
    for (uint32_t i = 0; i < patch_count_; ++i)
      word(95u | (3u << 24)), word(2u | ((patch_[i].mask & 0xF) << 4) | (27u << 12) | (1u << 20)), word(patch_[i].reg);
  }
  uint32_t input_vertices = 0;
  if (geometry) {
    // (D3D10_SB_PRIMITIVE: 1 point, 2 line, 3 triangle, 6 line with adjacency, 7 triangle with adjacency; the topologies: 1 point list, 3 line strip, 5 triangle strip)
    const uint32_t primitive = gs_[0];
    input_vertices = primitive == 1 ? 1 : primitive == 2 ? 2 : primitive == 3 ? 3 : primitive == 6 ? 4 : primitive == 7 ? 6 : 0;
    if (!input_vertices || gs_[1] == 0 || gs_[1] > graphics::kMaxGeometryVertices || (gs_[3] != 1 && gs_[3] != 3 && gs_[3] != 5) || gs_[2] == 0 || gs_[2] > 15) return status::kNotSupported;
    word(93u | (primitive << 11) | (1u << 24));
    word(92u | (gs_[3] << 11) | (1u << 24));
    word(94u | (2u << 24)), word(gs_[1]);
    if (gs_[2] != 1) {   // (more than the first stream: dcl_stream m0 .. m<highest>, which the other front end wants one after the other)
      uint32_t streams = 0;
      while (gs_[2] >> streams) ++streams;
      for (uint32_t k = 0; k < streams; ++k) word(143u | (3u << 24)), word((16u << 12) | (1u << 20)), word(k);
    }
    if (gs_[4] > 1) {
      if (gs_[4] > 32) return status::kNotSupported;
      word(206u | (2u << 24)), word(gs_[4]);
    }
  }
  if (compute) {
    if (threads_[0] == 0 || threads_[1] == 0 || threads_[2] == 0) return status::kInvalidParameter;
    word(155u | (4u << 24)), word(threads_[0]), word(threads_[1]), word(threads_[2]);
    static const uint32_t kSysTypes[4] = {32, 33, 34, 36};
    for (uint32_t k = 0; k < 4; ++k)
      if (sys_used_[k]) word(95u | (2u << 24)), word(2u | ((k == 3 ? 1u : 7u) << 4) | (kSysTypes[k] << 12));
    for (uint32_t k = 0; k < 4; ++k)
      if (shared_used_[k]) word(159u | (4u << 24)), word((31u << 12) | (1u << 20)), word(k), word(shared_bytes_[k]);
    for (uint32_t i = 0; i < kMaxResources; ++i) {
      const Resource& r = res_[1][i];
      if (!r.present) continue;
      if (r.lower >= graphics::kMaxStorageBuffers) return status::kNotSupported;
      if (r.kind == 12) word(158u | (4u << 24)), word((30u << 12) | (1u << 20)), word(r.lower), word(r.stride);
      else if (r.kind == 11) word(157u | (3u << 24)), word((30u << 12) | (1u << 20)), word(r.lower);
      else return status::kNotSupported;
    }
  } else if (res_[1][0].present || res_[1][1].present) {
    return status::kNotSupported;   // (unordered access views of the other stages)
  }
  // constant buffers
  for (uint32_t i = 0; i < kMaxResources; ++i) {
    const Resource& r = res_[2][i];
    if (!r.present) continue;
    if (r.bytes == 0 || r.bytes % 16 || r.lower >= 14) return status::kNotSupported;
    word(89u | (4u << 24)), word(2u | (1u << 2) | (0xE4u << 4) | (8u << 12) | (2u << 20)), word(r.lower), word(r.bytes / 16);
  }
  for (uint32_t i = 0; i < kMaxResources; ++i) {
    const Resource& r = res_[3][i];
    if (r.present) word(90u | (3u << 24)), word((6u << 12) | (1u << 20)), word(r.lower);
  }
  for (uint32_t i = 0; i < kMaxResources; ++i) {
    const Resource& r = res_[0][i];
    if (!r.present) continue;
    if (r.kind == 11 || r.kind == 12) {   // (a raw or structured buffer view: a read-only storage buffer of a compute shader)
      if (!compute || r.lower >= graphics::kMaxSrvBuffers) return status::kNotSupported;
      if (r.kind == 12) word(162u | (4u << 24)), word((7u << 12) | (1u << 20)), word(r.lower), word(r.stride);
      else word(161u | (3u << 24)), word((7u << 12) | (1u << 20)), word(r.lower);
      continue;
    }
    // (the kind: 2 texture 2D, 4 texture 3D, 5 cube, 7 2D array)
    const uint32_t dimension = r.kind == 2 ? 3 : r.kind == 4 ? 5 : r.kind == 5 ? 6 : r.kind == 7 ? 8 : 0;
    const uint32_t elements = r.size != 0xFFFFFFFFu && r.size >= 1 && r.size <= 8 ? r.size : 1;   // (an array: every element is declared)
    if (!dimension || r.lower >= graphics::kMaxTextureUnits || elements > graphics::kMaxTextureUnits - r.lower) return status::kNotSupported;
    for (uint32_t e = 0; e < elements; ++e)
      word(88u | (dimension << 11) | (4u << 24)), word(2u | (1u << 2) | (0xE4u << 4) | (7u << 12) | (1u << 20)), word(r.lower + e), word(0x5555u);
  }
  for (uint32_t i = 0; i < input_count_; ++i) {
    const SigElement& e = inputs_[i];
    const uint32_t operand = 2u | ((e.mask & 0xF) << 4) | (1u << 12) | (1u << 20);
    if (domain) {
      const uint32_t indexed = 2u | ((e.mask & 0xF) << 4) | (25u << 12) | (2u << 20);
      if (e.sysval == 0) word(95u | (4u << 24)), word(indexed), word(ds_[1]), word(e.reg);
      else if (e.sysval == 1) word(97u | (5u << 24)), word(indexed), word(ds_[1]), word(e.reg), word(1);
      else return status::kNotSupported;
    } else if (geometry) {
      // v[vertices][register]: the registers of every vertex (a position is the system value of the _siv form)
      const uint32_t indexed = 2u | ((e.mask & 0xF) << 4) | (1u << 12) | (2u << 20);
      if (e.sysval == 0) word(95u | (4u << 24)), word(indexed), word(input_vertices), word(e.reg);
      else if (e.sysval == 1) word(97u | (5u << 24)), word(indexed), word(input_vertices), word(e.reg), word(1);
      else return status::kNotSupported;
    } else if (vertex) {
      if (e.sysval == 0) word(95u | (3u << 24)), word(operand), word(e.reg);
      else if (e.sysval == 6 || e.sysval == 8) word(96u | (4u << 24)), word(operand), word(e.reg), word(e.sysval);
      else return status::kNotSupported;
    } else {
      if (e.sysval == 0) word(98u | (e.interp << 11) | (3u << 24)), word(operand), word(e.reg);
      else if (e.sysval == 1) word(100u | (4u << 11) | (4u << 24)), word(operand), word(e.reg), word(1);
      else if (e.sysval == 7 || e.sysval == 9) word(99u | (1u << 11) | (4u << 24)), word(operand), word(e.reg), word(e.sysval);
      else return status::kNotSupported;
    }
  }
  for (uint32_t i = 0; i < output_count_; ++i) {
    const SigElement& e = outputs_[i];
    const uint32_t operand = 2u | ((e.mask & 0xF) << 4) | (2u << 12) | (1u << 20);
    if (e.sysval == 0 || (pixel && e.sysval == 64)) word(101u | (3u << 24)), word(operand), word(e.reg);
    else if ((vertex || geometry || domain) && e.sysval == 1) word(103u | (4u << 24)), word(operand), word(e.reg), word(1);
    else return status::kNotSupported;
  }
  word(104u | (2u << 24)), word(temps_);
  ByteBuffer& body = out_;
  const uint32_t total = 2 + static_cast<uint32_t>(decl.size() / 4 + body.size() / 4);
  if (total > 0xFFFF) return status::kNotSupported;
  if (!shex->put32((program_kind << 16) | (5u << 4)) || !shex->put32(total) || !shex->append(decl.view()) || !shex->append(body.view())) return status::kNoMemory;
  return status::kSuccess;
}

// The ISG1 / OSG1 signature part (32 bytes an element) as the ISGN / OSGN one (24).
// `streams`: the output signature of a geometry shader, whose entries keep the stream they belong to (the 28-byte OSG5 layout).
bool convert_signature(allocators::IAllocator& allocator, ConstByteView chunk, ByteBuffer* out, bool streams = false) {
  ByteReader in(chunk);
  const uint32_t n = in.le32();
  const uint32_t key = in.le32();
  (void)key;
  if (!in.ok() || n > 16) return false;
  if (!out->put32(n) || !out->put32(8)) return false;
  const uint64_t table_at = 8 + uint64_t{n} * 32;
  for (uint32_t i = 0; i < n; ++i) {
    ConstByteView entry = in.take(32);
    if (!in.ok()) return false;
    ByteReader e(entry);
    const uint32_t stream = e.le32();
    const uint32_t name = e.le32();
    if (name < table_at || name > chunk.size()) return false;
    const uint32_t index = e.le32(), sysval = e.le32(), type = e.le32(), reg = e.le32(), masks = e.le32() & 0xFFFF;
    if (streams && (stream >= graphics::kMaxGeometryStreams || !out->put32(stream))) return false;
    if (!out->put32(name - static_cast<uint32_t>(uint64_t{n} * (streams ? 4 : 8))) || !out->put32(index) || !out->put32(sysval) || !out->put32(type) || !out->put32(reg) || !out->put32(masks)) return false;
  }
  if (table_at > chunk.size()) return false;
  (void)allocator;
  return out->append(chunk.subview(table_at));
}

}  // namespace

NtStatus D3d9Program::compile_dxil(allocators::IAllocator& allocator, ConstByteView bytes, UniquePointer<D3d9Program>* out) {
  if (bytes.size() < 32 || bytes.size() > (1u << 20)) return status::kInvalidParameter;
  ByteReader head(bytes);
  if (head.le32() != kFourDxbc) return status::kInvalidParameter;
  head.skip(16);
  const uint32_t one = head.le32(), total = head.le32(), chunk_count = head.le32();
  if (!head.ok() || one != 1 || total > bytes.size() || total < 32 || chunk_count == 0 || chunk_count > 32) return status::kInvalidParameter;
  ConstByteView program, isg1, osg1, psg1;
  for (uint32_t i = 0; i < chunk_count; ++i) {
    const uint32_t offset = head.le32();
    if (!head.ok() || offset > total || total - offset < 8) return status::kInvalidParameter;
    ByteReader c(bytes.subview(offset, total - offset));
    const uint32_t fourcc = c.le32(), size = c.le32();
    if (!c.ok() || size > c.remaining()) return status::kInvalidParameter;
    const ConstByteView data = c.take(size);
    if (fourcc == kFourDxil) program = data;
    else if (fourcc == kFourIsg1) isg1 = data;
    else if (fourcc == kFourOsg1) osg1 = data;
    else if (fourcc == 0x31475350) psg1 = data;   // 'PSG1'
  }
  // the program header: version, size in words, then the bitcode header (magic, version, offset from it, size)
  ByteReader ph(program);
  const uint32_t version = ph.le32(), words = ph.le32(), magic = ph.le32();
  ph.le32();
  const uint32_t bitcode_offset = ph.le32(), bitcode_size = ph.le32();
  if (!ph.ok() || magic != kFourDxil || words * 4ull > program.size() + 8 || bitcode_offset < 16) return status::kInvalidParameter;
  ConstByteView bitcode;
  if (!program.try_subview(8 + uint64_t{bitcode_offset}, bitcode_size, &bitcode)) return status::kInvalidParameter;
  const uint32_t kind = version >> 16;
  if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 4 && kind != 5) return status::kNotSupported;
  dxil::Module module(allocator);
  NtStatus s = dxil::read_module(allocator, bitcode, &module);
  if (s != status::kSuccess) return s;
  const dxil::Function* entry = nullptr;
  uint32_t bodies = 0;
  for (const dxil::Function& f : module.functions.view()) {
    if (f.declaration) continue;
    ++bodies;
    if (!entry) entry = &f;
  }
  if (!entry || bodies > (kind == 3 ? 2u : 1u)) return entry ? status::kNotSupported : status::kInvalidParameter;   // (a library; a hull shader has its patch constant function too)
  ByteBuffer shex(allocator);
  if (kind == 3) {
    // The control point function (the entry the metadata names) and the patch constant function make the two phases of the Shader Model 5 hull shader.
    const dxil::Function* control = nullptr;
    const dxil::Function* constant = nullptr;
    uint32_t entry_value = kNone, patch_value = kNone;
    {
      Translator probe(allocator, module, *entry);
      if ((s = probe.read_metadata_only()) != status::kSuccess) return s;
      entry_value = probe.entry_value(), patch_value = probe.patch_function();
    }
    for (const dxil::Function& f : module.functions.view()) {
      if (f.declaration) continue;
      if (f.value == entry_value) control = &f;
      if (f.value == patch_value) constant = &f;
    }
    if (!control || !constant || control == constant) return status::kNotSupported;
    Translator first(allocator, module, *control), second(allocator, module, *constant);
    ByteBuffer phase_one(allocator), phase_two(allocator);
    if ((s = first.run(kind, isg1, osg1, psg1, &phase_one, 1)) != status::kSuccess || (s = second.run(kind, isg1, osg1, psg1, &phase_two, 2)) != status::kSuccess) return s;
    const uint32_t* hs = first.hull_state();   // (control points in, out, domain, partitioning, output primitive, maximum factor)
    if (hs[1] < 1 || hs[1] > graphics::kMaxPatchControlPoints || hs[2] < 1 || hs[2] > 3 || hs[3] < 1 || hs[3] > 4 || hs[4] < 1 || hs[4] > 4) return status::kNotSupported;
    ByteBuffer body(allocator);
    bool ok = body.put32(113u | (1u << 24)) &&                               // hs_decls
              body.put32(147u | (hs[0] << 11) | (1u << 24)) && body.put32(148u | (hs[1] << 11) | (1u << 24)) && body.put32(149u | (hs[2] << 11) | (1u << 24)) &&
              body.put32(150u | (hs[3] << 11) | (1u << 24)) && body.put32(151u | (hs[4] << 11) | (1u << 24)) && body.put32(152u | (2u << 24)) && body.put32(hs[5]) &&
              body.put32(114u | (1u << 24)) && body.append(phase_one.view()) && body.put32(115u | (1u << 24)) && body.append(phase_two.view());
    const uint64_t words = 2 + body.size() / 4;
    if (!ok || words > 0xFFFF || !shex.put32((3u << 16) | (5u << 4)) || !shex.put32(static_cast<uint32_t>(words)) || !shex.append(body.view())) return status::kNoMemory;
  } else {
    Translator translator(allocator, module, *entry);
    s = translator.run(kind, isg1, osg1, psg1, &shex);
    if (s != status::kSuccess) return s;
  }
#ifdef DXIL_TRACE
  {
    ByteReader dump(shex.view());
    fprintf(stderr, "shex:");
    while (!dump.at_end()) fprintf(stderr, " %08x", dump.le32());
    fprintf(stderr, "\n");
  }
#endif
  // the container: the converted signatures and the program
  ByteBuffer in_sig(allocator), out_sig(allocator);
  if (!isg1.empty() && !convert_signature(allocator, isg1, &in_sig)) return status::kInvalidParameter;
  if (!osg1.empty() && !convert_signature(allocator, osg1, &out_sig, kind == 2)) return status::kInvalidParameter;
  struct Part {
    uint32_t fourcc;
    ConstByteView data;
  } parts[3];
  uint32_t part_count = 0;
  if (in_sig.size()) parts[part_count++] = {kFourIsgn, in_sig.view()};
  if (out_sig.size()) parts[part_count++] = {kind == 2 ? 0x3547534Fu : kFourOsgn, out_sig.view()};   // (OSG5 for the streams)
  parts[part_count++] = {kFourShex, shex.view()};
  ByteBuffer container(allocator);
  uint64_t size = 32 + uint64_t{part_count} * 4;
  for (uint32_t i = 0; i < part_count; ++i) size += 8 + (parts[i].data.size() + 3) / 4 * 4;
  if (size > 0xFFFFFF) return status::kNotSupported;
  bool ok = container.put32(kFourDxbc) && container.put_zeros(16) && container.put32(1) && container.put32(static_cast<uint32_t>(size)) && container.put32(part_count);
  uint64_t at = 32 + uint64_t{part_count} * 4;
  for (uint32_t i = 0; i < part_count; ++i) {
    ok = ok && container.put32(static_cast<uint32_t>(at));
    at += 8 + (parts[i].data.size() + 3) / 4 * 4;
  }
  for (uint32_t i = 0; i < part_count; ++i)
    ok = ok && container.put32(parts[i].fourcc) && container.put32(static_cast<uint32_t>(parts[i].data.size())) && container.append(parts[i].data) && container.put_zeros((4 - parts[i].data.size() % 4) % 4);
  if (!ok) return status::kNoMemory;
  return compile_dxbc(allocator, container.view(), out);
}

}  // namespace dxgk
