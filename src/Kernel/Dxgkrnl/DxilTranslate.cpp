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

namespace dxgk {

namespace {

using namespace dxil;

constexpr uint32_t kFourDxbc = 0x43425844, kFourDxil = 0x4C495844, kFourIsg1 = 0x31475349, kFourOsg1 = 0x3147534F, kFourShex = 0x58454853, kFourIsgn = 0x4E475349, kFourOsgn = 0x4E47534F;
constexpr uint32_t kNone = ~0u;
constexpr uint32_t kSlots = 128;   // 32 temporaries of four components
constexpr uint32_t kMaxResources = 16;

enum class VK : uint8_t { None, Imm, Temp, Input, Cb, Agg, CbAgg, Handle };

// What a value of the function is, in the program being made.
struct Val {
  VK kind = VK::None;
  uint32_t a = 0, b = 0, c = 0;   // Imm: the bits; Temp / Agg: register (a), component (b); Input: register (a), component (b); Cb: slot (a), register (b), component (c); CbAgg: slot (a), register (b); Handle: class (a), register (b)
  bool is_mask = false;           // (a boolean: all ones or zero)
};

struct SigElement {
  uint32_t reg = 0, mask = 0, sysval = 0, index = 0, component_type = 0, interp = 2;
};

struct Resource {
  uint32_t id = 0, space = 0, lower = 0, size = 0, kind = 0, bytes = 0;
  bool present = false;
};

// Dxil opcodes (DxilConstants.h OpCode).
enum DxOp : uint32_t {
  kLoadInput = 4, kStoreOutput = 5, kFAbs = 6, kSaturate = 7, kCos = 12, kSin = 13, kExp = 21, kFrc = 22, kLog = 23, kSqrt = 24, kRsqrt = 25, kRoundNe = 26, kRoundNi = 27, kRoundPi = 28, kRoundZ = 29,
  kFMax = 35, kFMin = 36, kIMax = 37, kIMin = 38, kUMax = 39, kUMin = 40, kIMul = 41, kUMul = 42, kFMad = 46, kIMad = 48, kUMad = 49, kDot2 = 54, kDot3 = 55, kDot4 = 56, kCreateHandle = 57,
  kCBufferLoadLegacy = 59, kSample = 60, kSampleLevel = 62, kTextureLoad = 66, kDiscard = 82,
};

class Translator {
 public:
  Translator(allocators::IAllocator& allocator, const Module& module, const Function& function)
      : allocator_(allocator), m_(module), f_(function), out_(allocator), vals_(allocator), last_use_(allocator), alias_of_(allocator), unsupported_(allocator), marks_(allocator) {}

  // Translates the entry function. `isg1` / `osg1` are the signature parts of the container.
  NtStatus run(uint32_t program_kind, ConstByteView isg1, ConstByteView osg1, ByteBuffer* shex);

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
  bool error(NtStatus s) {
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
  void successors_of(uint32_t block, uint32_t* out, uint32_t* count) const;
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
  bool dot(const Instruction& ins, uint32_t n);
  bool sample(const Instruction& ins, bool level);
  bool texture_load(const Instruction& ins);
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
  uint32_t depth_ = 0, steps_ = 0;
  NtStatus status_ = status::kSuccess;
  uint32_t kind_ = 0;
  Resource res_[4][kMaxResources];
  SigElement inputs_[16], outputs_[16];
  uint32_t input_count_ = 0, output_count_ = 0;
  uint32_t threads_[3] = {1, 1, 1};
  uint32_t entry_value_ = kNone;
};

// --- operands --------------------------------------------------------------------------------------------------------------------------------------------------

void Translator::src(const Val& v, uint32_t modifier) {
  switch (v.kind) {
    case VK::Imm:
      put(0x4001u);
      put(v.a);
      break;
    case VK::Temp: case VK::Input:
      put(2u | (2u << 2) | (v.b << 4) | ((v.kind == VK::Input ? 1u : 0u) << 12) | (1u << 20) | (modifier ? 1u << 31 : 0u));
      if (modifier) put(1u | (modifier << 6));
      put(v.a);
      break;
    case VK::Cb:
      put(2u | (2u << 2) | (v.c << 4) | (8u << 12) | (2u << 20) | (modifier ? 1u << 31 : 0u));
      if (modifier) put(1u | (modifier << 6));
      put(v.a);
      put(v.b);
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

bool Translator::analyze(const Instruction& ins) {
  const int32_t at = pos_;
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
    case Op::Call:
      for (uint32_t k = 0; k < ins.arg_count; ++k) note_use(ins.args[k], at);
      return true;
    case Op::Load: case Op::Alloca: case Op::Gep:
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
  for (uint32_t i = 0; i < blk.count; ++i) {
    const Instruction& phi = instruction_at(blk.first + i);
    if (phi.op != Op::Phi) break;
    uint32_t incoming = kNone;
    for (uint32_t k = 0; k < phi.phi_count; ++k)
      if (m_.phi_blocks[phi.phi_first + k] == pred) incoming = m_.phi_values[phi.phi_first + k];
    if (incoming == kNone) return error(status::kInvalidParameter);
    if (dry_) {
      note_use(incoming, pos_);
    } else {
      const Val from = use(incoming), to = vals_[phi.result];
      if (to.kind != VK::Temp) return error(status::kInvalidParameter);
      const uint32_t at = begin(54);
      dst_scalar(to);
      src(from);
      finish(at);
    }
    ++pos_;
  }
  return status_ == status::kSuccess;
}

void Translator::successors_of(uint32_t block, uint32_t* out, uint32_t* count) const {
  *count = 0;
  const Block& b = block_at(block);
  if (b.count == 0) return;
  const Instruction& t = m_.instructions[b.first + b.count - 1];
  if (t.op == Op::Br) out[(*count)++] = t.blocks[0];
  else if (t.op == Op::CondBr) out[(*count)++] = t.blocks[0], out[(*count)++] = t.blocks[1];
}

// The first block both a and b lead to (the join of an if): the lowest block number reachable from both.
uint32_t Translator::find_merge(uint32_t a, uint32_t b) {
  const uint32_t n = f_.block_count;
  marks_.clear();
  for (uint32_t i = 0; i < 2 * n; ++i)
    if (!marks_.push_back(0)) return kNone;
  for (uint32_t side = 0; side < 2; ++side) {
    List<uint32_t> stack(allocator_);
    if (!stack.push_back(side == 0 ? a : b)) return kNone;
    while (stack.count()) {
      const uint32_t x = stack[stack.count() - 1];
      stack.remove_at(stack.count() - 1);
      if (x >= n || marks_[side * n + x]) continue;
      marks_[side * n + x] = 1;
      uint32_t next[2], count = 0;
      successors_of(x, next, &count);
      for (uint32_t k = 0; k < count; ++k)
        if (next[k] > x && !stack.push_back(next[k])) return kNone;   // (forward edges only)
    }
  }
  for (uint32_t x = 0; x < n; ++x)
    if (marks_[x] && marks_[n + x]) return x;
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
  uint32_t b = start;
  for (;;) {
    if (!visit_block(b)) return false;
    const Instruction& t = terminator(b);
    if (t.op == Op::Ret) {
      if (dry_) {
        if (t.operands[0] != 0) note_use(t.operands[0], pos_);
      } else {
        const uint32_t at = begin(62);
        finish(at);
      }
      ++pos_;
      return true;
    }
    if (t.op == Op::Unreachable) return true;
    if (t.op == Op::Br) {
      const uint32_t target = t.blocks[0];
      if (target <= b) return error(status::kNotSupported);   // (a loop)
      if (!phi_moves(b, target)) return false;
      if (target == stop) return true;
      if (stop != kNone && target > stop) return error(status::kNotSupported);
      b = target;
      continue;
    }
    if (t.op != Op::CondBr) return error(status::kNotSupported);
    const uint32_t tb = t.blocks[0], fb = t.blocks[1];
    if (tb <= b || fb <= b || tb == fb) return error(status::kNotSupported);
    const uint32_t merge = find_merge(tb, fb);
    if (dry_) {
      note_use(t.operands[0], pos_);
    } else {
      const Val cond = use(t.operands[0]);
      const uint32_t at = begin(31, 1u << 18);   // if_nz
      src(cond);
      finish(at);
    }
    ++pos_;
    if (!region(tb, merge, b)) return false;
    if (!dry_) finish(begin(18));   // else
    if (!region(fb, merge, b)) return false;
    if (!dry_) finish(begin(21));   // endif
    if (merge == kNone || merge == stop) return true;
    if (stop != kNone && merge > stop) return error(status::kNotSupported);
    b = merge;
  }
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
    case Op::Load: case Op::Alloca: case Op::Gep: return true;   // (refused only if something uses the result)
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
  const Resource& t = res_[0][srv.b];
  const Resource& s = res_[3][smp.b];
  const uint32_t t_reg = t.lower + srv.c, s_reg = s.lower + smp.c;
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

bool Translator::texture_load(const Instruction& ins) {
  Val srv;
  if (ins.arg_count < 9 || !resource_of(ins.args[1], 0, &srv)) return error(status::kNotSupported);
  for (uint32_t k = 6; k < 9; ++k) {
    uint64_t v = 0;
    const Constant* c = constant(ins.args[k]);
    if (!c || (c->kind != ConstantKind::Undef && !(constant_int(ins.args[k], &v) && v == 0))) return error(status::kNotSupported);
  }
  const Resource& t = res_[0][srv.b];
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
  src_resource(t.lower + srv.c);
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
      if (ins.arg_count < 4 || !constant_int(ins.args[1], &cls) || !constant_int(ins.args[2], &range) || !constant_int(ins.args[3], &index) || cls > 3 || range >= kMaxResources || !res_[cls][range].present)
        return error(status::kNotSupported);   // (a resource array indexed by a variable)
      if (index >= (res_[cls][range].size == 0xFFFFFFFFu ? 0xFFFFu : res_[cls][range].size)) return error(status::kInvalidParameter);
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
      v.kind = VK::CbAgg, v.a = cb.lower + h.c, v.b = static_cast<uint32_t>(reg);
      return define(ins.result, v);
    }
    case kSample: return sample(ins, false);
    case kSampleLevel: return sample(ins, true);
    case kTextureLoad: return texture_load(ins);
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
      r.present = true, r.id = static_cast<uint32_t>(id), r.lower = static_cast<uint32_t>(lower), r.size = static_cast<uint32_t>(size);
      if (cls == 2) r.bytes = static_cast<uint32_t>(extra);
      else r.kind = static_cast<uint32_t>(extra);
    }
  }
  // the signature elements' interpolation modes (the container's signature parts say the rest)
  const Meta* signatures = operand_of(entry, 2);
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

NtStatus Translator::run(uint32_t program_kind, ConstByteView isg1, ConstByteView osg1, ByteBuffer* shex) {
  kind_ = program_kind;
  NtStatus s = read_metadata();
  if (s != status::kSuccess) return s;
  if (entry_value_ != f_.value) return status::kNotSupported;
  if ((s = read_signature(isg1, inputs_, &input_count_)) != status::kSuccess || (s = read_signature(osg1, outputs_, &output_count_)) != status::kSuccess) return s;
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
    if (!vals_.push_back(v) || !last_use_.push_back(-1) || !alias_of_.push_back(kNone) || !unsupported_.push_back(0)) return status::kNoMemory;
  }
  // the first pass: what is used how long
  visited_marks_.clear();
  for (uint32_t i = 0; i < f_.block_count; ++i)
    if (!visited_marks_.push_back(0)) return status::kNoMemory;
  dry_ = true;
  pos_ = 0, depth_ = 0, steps_ = 0;
  if (!region(0, kNone, kNone)) return status_ != status::kSuccess ? status_ : status::kNotSupported;
  for (uint64_t i = values; i-- > 0;)
    if (alias_of_[i] != kNone && alias_of_[i] < values && last_use_[i] > last_use_[alias_of_[i]]) last_use_[alias_of_[i]] = last_use_[i];
  for (uint64_t i = 0; i < values; ++i)
    if (unsupported_[i] && last_use_[i] >= 0) return status::kNotSupported;
  // the second pass: the code
  for (uint32_t i = 0; i < f_.block_count; ++i) visited_marks_[i] = 0;
  dry_ = false;
  pos_ = 0, depth_ = 0, steps_ = 0;
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
  const bool pixel = program_kind == 0, vertex = program_kind == 1;
  if (!pixel && !vertex) return status::kNotSupported;
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
    // (the kind: 2 texture 2D, 4 texture 3D, 5 cube, 7 2D array)
    const uint32_t dimension = r.kind == 2 ? 3 : r.kind == 4 ? 5 : r.kind == 5 ? 6 : r.kind == 7 ? 8 : 0;
    if (!dimension || r.lower >= graphics::kMaxTextureUnits) return status::kNotSupported;
    word(88u | (dimension << 11) | (4u << 24)), word(2u | (1u << 2) | (0xE4u << 4) | (7u << 12) | (1u << 20)), word(r.lower), word(0x5555u);
  }
  if (res_[1][0].present || res_[1][1].present) return status::kNotSupported;   // (unordered access views: compute shaders)
  for (uint32_t i = 0; i < input_count_; ++i) {
    const SigElement& e = inputs_[i];
    const uint32_t operand = 2u | ((e.mask & 0xF) << 4) | (1u << 12) | (1u << 20);
    if (vertex) {
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
    else if (vertex && e.sysval == 1) word(103u | (4u << 24)), word(operand), word(e.reg), word(1);
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
bool convert_signature(allocators::IAllocator& allocator, ConstByteView chunk, ByteBuffer* out) {
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
    e.le32();
    const uint32_t name = e.le32();
    if (name < table_at || name > chunk.size()) return false;
    const uint32_t index = e.le32(), sysval = e.le32(), type = e.le32(), reg = e.le32(), masks = e.le32() & 0xFFFF;
    if (!out->put32(name - static_cast<uint32_t>(uint64_t{n} * 8)) || !out->put32(index) || !out->put32(sysval) || !out->put32(type) || !out->put32(reg) || !out->put32(masks)) return false;
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
  ConstByteView program, isg1, osg1;
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
  if (kind != 0 && kind != 1) return status::kNotSupported;
  dxil::Module module(allocator);
  NtStatus s = dxil::read_module(allocator, bitcode, &module);
  if (s != status::kSuccess) return s;
  const dxil::Function* entry = nullptr;
  for (const dxil::Function& f : module.functions.view()) {
    if (f.declaration) continue;
    if (entry) return status::kNotSupported;   // (a library)
    entry = &f;
  }
  if (!entry) return status::kInvalidParameter;
  Translator translator(allocator, module, *entry);
  ByteBuffer shex(allocator);
  s = translator.run(kind, isg1, osg1, &shex);
  if (s != status::kSuccess) return s;
  // the container: the converted signatures and the program
  ByteBuffer in_sig(allocator), out_sig(allocator);
  if (!isg1.empty() && !convert_signature(allocator, isg1, &in_sig)) return status::kInvalidParameter;
  if (!osg1.empty() && !convert_signature(allocator, osg1, &out_sig)) return status::kInvalidParameter;
  struct Part {
    uint32_t fourcc;
    ConstByteView data;
  } parts[3];
  uint32_t part_count = 0;
  if (in_sig.size()) parts[part_count++] = {kFourIsgn, in_sig.view()};
  if (out_sig.size()) parts[part_count++] = {kFourOsgn, out_sig.view()};
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
