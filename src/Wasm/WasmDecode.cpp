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

// The WebAssembly binary decoder and validator. A function body is checked with the algorithm of the specification's appendix (an operand stack of types with an
// "unknown" type for code after an unconditional branch, and a stack of control frames) and translated as it is checked: blocks disappear, branches get their
// target address and the height they cut the stack back to, and code that cannot be reached is not emitted at all.
#include "Wasm/Wasm.hpp"

namespace wasm {

bool FuncSig::init(uint32_t np, uint32_t nr) {
  if (np > kMaxParams || nr > kMaxResults) return false;
  allocator_.deallocate(storage_);
  storage_ = nullptr;
  params = ArrayView<ValType>();
  results = ArrayView<ValType>();
  nparams = nresults = 0;
  const uint64_t total = uint64_t{np} + nr;
  if (total) {
    storage_ = static_cast<ValType*>(allocator_.allocate(total));
    if (!storage_) return false;
    for (uint64_t i = 0; i < total; ++i) storage_[i] = ValType::I32;
  }
  params = ArrayView<ValType>(storage_, np);
  results = ArrayView<ValType>(storage_ ? storage_ + np : nullptr, nr);
  nparams = static_cast<uint16_t>(np);
  nresults = static_cast<uint16_t>(nr);
  return true;
}

bool FuncSig::assign(const FuncSig& other) {
  if (!init(other.nparams, other.nresults)) return false;
  for (uint32_t i = 0; i < nparams; ++i) params[i] = other.params[i];
  for (uint32_t i = 0; i < nresults; ++i) results[i] = other.results[i];
  return true;
}

bool FuncSig::equals(const FuncSig& o) const {
  if (nparams != o.nparams || nresults != o.nresults) return false;
  for (uint32_t i = 0; i < nparams; ++i)
    if (params[i] != o.params[i]) return false;
  for (uint32_t i = 0; i < nresults; ++i)
    if (results[i] != o.results[i]) return false;
  return true;
}

const char* val_type_name(ValType t) {
  switch (t) {
    case ValType::I32: return "i32";
    case ValType::I64: return "i64";
    case ValType::F32: return "f32";
    case ValType::F64: return "f64";
    case ValType::V128: return "v128";
    case ValType::FuncRef: return "funcref";
    case ValType::ExternRef: return "externref";
    case ValType::ExnRef: return "exnref";
  }
  return "?";
}

Module::Module(allocators::IAllocator& allocator)
    : types(allocator),
      imports(allocator),
      func_types(allocator),
      tables(allocator),
      memories(allocator),
      globals(allocator),
      tags(allocator),
      global_defs(allocator),
      exports(allocator),
      elems(allocator),
      datas(allocator),
      codes(allocator),
      customs(allocator),
      allocator_(allocator) {}

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint8_t kUnknown = 0;
constexpr uint32_t kMaxLocals = 50000, kMaxLabels = 1u << 20, kMaxFunctions = 1000000, kMaxTypes = 1000000, kMaxEntries = 1000000, kMaxDepth = 100000;

bool valid_val_type(uint8_t b) { return b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C || b == 0x7B || b == 0x70 || b == 0x6F || b == 0x69; }

// Reads the primitives of the binary format.
class Reader {
 public:
  Reader() = default;
  Reader(ConstByteView data, uint64_t base) : data_(data), base_(base) {}
  bool ok() const { return !failed_; }
  bool at_end() const { return pos_ >= data_.size(); }
  uint64_t position() const { return base_ + pos_; }
  uint64_t remaining() const { return data_.size() - pos_; }

  uint8_t u8() {
    if (pos_ >= data_.size()) {
      failed_ = true;
      return 0;
    }
    return data_[pos_++];
  }
  bool peek(uint8_t* out) const {
    if (pos_ >= data_.size()) return false;
    *out = data_[pos_];
    return true;
  }
  uint32_t u32() { return static_cast<uint32_t>(unsigned_leb(5)); }
  int32_t s32() { return static_cast<int32_t>(signed_leb(5, 32)); }
  int64_t s33() { return signed_leb(5, 33); }
  int64_t s64() { return signed_leb(10, 64); }
  uint32_t fixed32() {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
    return v;
  }
  uint64_t fixed64() {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
    return v;
  }
  ConstByteView take(uint64_t n) {
    if (n > remaining()) {
      failed_ = true;
      return ConstByteView();
    }
    ConstByteView v = data_.subview(pos_, n);
    pos_ += n;
    return v;
  }
  void fail() { failed_ = true; }
  void fail_at(const char* why) {
    failed_ = true;
    why_ = why;
  }
  const char* why() const { return why_; }

 private:
  uint64_t unsigned_leb(uint32_t max_bytes) {
    uint64_t result = 0;
    for (uint32_t i = 0; i < max_bytes; ++i) {
      const uint8_t b = u8();
      if (failed_) return 0;
      result |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
      if (!(b & 0x80)) {
        if (i == max_bytes - 1 && max_bytes == 5 && (b & 0x70)) {   // (a 32 bit value has 4 bits in the fifth byte)
          fail_at("integer too large");
        }
        return result;
      }
    }
    fail_at("integer representation too long");
    return 0;
  }
  int64_t signed_leb(uint32_t max_bytes, uint32_t bits) {
    uint64_t result = 0;
    uint32_t shift = 0;
    for (uint32_t i = 0; i < max_bytes; ++i) {
      const uint8_t b = u8();
      if (failed_) return 0;
      result |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      if (!(b & 0x80)) {
        if (i == max_bytes - 1 && shift > bits) {   // the unused bits of the last byte must repeat the sign
          const uint32_t used = bits - 7 * (max_bytes - 1);
          const uint8_t rest = static_cast<uint8_t>(b & 0x7F) >> (used - 1);
          const uint8_t all = static_cast<uint8_t>(0x7F >> (used - 1));
          if (rest != 0 && rest != all) fail_at("integer too large");
        }
        if (shift < 64 && (b & 0x40)) result |= ~uint64_t{0} << shift;
        return static_cast<int64_t>(result);
      }
    }
    fail_at("integer representation too long");
    return 0;
  }

  ConstByteView data_;
  uint64_t base_ = 0, pos_ = 0;
  bool failed_ = false;
  const char* why_ = nullptr;
};

bool valid_utf8(ConstByteView s) {
  uint64_t i = 0;
  while (i < s.size()) {
    const uint8_t c = s[i];
    uint32_t need;
    uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (s.size() - i <= need) return false;
    for (uint32_t k = 1; k <= need; ++k) {
      if ((s[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if ((need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
    i += need + 1;
  }
  return true;
}

void truncate_to(List<uint8_t>& list, uint64_t n) {
  while (list.count() > n) list.remove_at(list.count() - 1);
}

// --- The function compiler -----------------------------------------------------------------------------------------------------

enum class CtlKind : uint8_t { Func, Block, Loop, If, Else, TryTable, Try, Catch, CatchAll };

struct Ctl {
  CtlKind kind = CtlKind::Block;
  const FuncSig* sig = nullptr;   // a block that has a type index
  ValType single = ValType::I32;
  bool has_single = false;
  bool unreachable = false;
  bool dead = false;               // opened inside unreachable code: nothing is emitted for it
  uint32_t height = 0;
  uint32_t loop_pc = 0;
  uint32_t handler = kNone;        // the try_table's / try's entry in FuncCode::handlers
  uint32_t handlers_before = 0;    // how many handlers there were when the block began
  uint32_t last_clause = kNone;    // the last clause of the handler (legacy try: they come one by one)
  uint32_t cur_clause = kNone;     // the clause of the catch block this is
  // Where the operands of the block begin: a catch block has the exception below them.
  uint32_t base() const { return height + (kind == CtlKind::Catch || kind == CtlKind::CatchAll ? 1u : 0u); }
  uint32_t else_patch = kNone;     // the jump taken when an `if` has no true branch
  uint32_t patch_code = kNone, patch_table = kNone;   // the forward branches to the end of the block (chained through their target field)
  uint32_t nparams() const { return sig ? sig->nparams : 0; }
  uint32_t nresults() const { return sig ? sig->nresults : (has_single ? 1u : 0u); }
  ValType param(uint32_t i) const { return sig->params[i]; }
  ValType result(uint32_t i) const { return sig ? sig->results[i] : single; }
  uint32_t label_arity() const { return kind == CtlKind::Loop ? nparams() : nresults(); }
  ValType label_type(uint32_t i) const { return kind == CtlKind::Loop ? param(i) : result(i); }
};

class Decoder;

class FuncCompiler {
 public:
  FuncCompiler(Module& m, FuncCode& fc, Reader& r, const List<uint8_t>& declared, bool has_datacount, uint32_t datacount, allocators::IAllocator& a)
      : m_(m), fc_(fc), r_(r), declared_(declared), has_datacount_(has_datacount), datacount_(datacount), vals_(a), locals_(a), ctls_(a) {}

  // Compiles the body (locals and expression) that `r` is positioned at the start of. On failure `*why` says what and r.position() is near it.
  bool compile(const char** why);

 private:
  Module& m_;
  FuncCode& fc_;
  Reader& r_;
  const List<uint8_t>& declared_;
  bool has_datacount_;
  uint32_t datacount_;
  List<uint8_t> vals_;
  List<ValType> locals_;
  List<Ctl> ctls_;
  const char* why_ = nullptr;
  uint32_t max_height_ = 0;
  uint32_t cur_tag_ = 0;

  bool fail(const char* why) {
    if (!why_) why_ = why;
    return false;
  }
  Ctl& top() { return ctls_[ctls_.count() - 1]; }
  bool live() { return !top().unreachable && !top().dead; }

  bool emit(uint16_t op, uint32_t a = 0, uint64_t b = 0, uint16_t aux = 0) {
    if (!live()) return true;
    Instr i;
    i.op = op;
    i.aux = aux;
    i.a = a;
    i.b = b;
    return fc_.code.push_back(i) || fail("out of memory");
  }
  // The code index the next emitted instruction gets.
  uint32_t here() const { return static_cast<uint32_t>(fc_.code.count()); }

  bool push(uint8_t t) {
    if (vals_.count() >= (1u << 24)) return fail("operand stack too deep");
    if (!vals_.push_back(t)) return fail("out of memory");
    if (vals_.count() > max_height_) max_height_ = static_cast<uint32_t>(vals_.count());
    return true;
  }
  bool pop_any(uint8_t* out) {
    const Ctl& c = top();
    if (vals_.count() == c.base()) {
      if (c.unreachable) {
        *out = kUnknown;
        return true;
      }
      return fail("type mismatch: not enough operands");
    }
    *out = vals_[vals_.count() - 1];
    vals_.remove_at(vals_.count() - 1);
    return true;
  }
  bool pop(uint8_t expect) {
    uint8_t got;
    if (!pop_any(&got)) return false;
    if (got != expect && got != kUnknown && expect != kUnknown) return fail("type mismatch");
    return true;
  }
  bool pop_ref() {
    uint8_t got;
    if (!pop_any(&got)) return false;
    if (got != kUnknown && got != 0x70 && got != 0x6F && got != 0x69) return fail("type mismatch: reference expected");
    return true;
  }
  bool pop_t(ValType t) { return pop(static_cast<uint8_t>(t)); }
  bool push_t(ValType t) { return push(static_cast<uint8_t>(t)); }
  bool mark_unreachable() {
    truncate_to(vals_, top().base());
    top().unreachable = true;
    return true;
  }

  bool pop_sig_params(const FuncSig& sig) {
    for (uint32_t i = sig.nparams; i > 0; --i)
      if (!pop_t(sig.params[i - 1])) return false;
    return true;
  }
  bool push_sig_results(const FuncSig& sig) {
    for (uint32_t i = 0; i < sig.nresults; ++i)
      if (!push_t(sig.results[i])) return false;
    return true;
  }
  bool pop_ctl_params(const Ctl& c) {
    for (uint32_t i = c.nparams(); i > 0; --i)
      if (!pop_t(c.param(i - 1))) return false;
    return true;
  }
  bool push_ctl_params(const Ctl& c) {
    for (uint32_t i = 0; i < c.nparams(); ++i)
      if (!push_t(c.param(i))) return false;
    return true;
  }
  bool pop_ctl_results(const Ctl& c) {
    for (uint32_t i = c.nresults(); i > 0; --i)
      if (!pop_t(c.result(i - 1))) return false;
    return true;
  }
  bool push_ctl_results(const Ctl& c) {
    for (uint32_t i = 0; i < c.nresults(); ++i)
      if (!push_t(c.result(i))) return false;
    return true;
  }
  bool pop_label(const Ctl& c) {
    for (uint32_t i = c.label_arity(); i > 0; --i)
      if (!pop_t(c.label_type(i - 1))) return false;
    return true;
  }
  bool push_label(const Ctl& c) {
    for (uint32_t i = 0; i < c.label_arity(); ++i)
      if (!push_t(c.label_type(i))) return false;
    return true;
  }

  bool push_ctl(CtlKind kind, const FuncSig* sig, bool has_single, ValType single) {
    if (ctls_.count() >= kMaxDepth) return fail("blocks nested too deeply");
    Ctl c;
    c.kind = kind;
    c.sig = sig;
    c.has_single = has_single;
    c.single = single;
    c.height = static_cast<uint32_t>(vals_.count());
    c.handlers_before = static_cast<uint32_t>(fc_.handlers.count());
    c.dead = ctls_.count() > 0 && !live();
    c.loop_pc = here();
    if (!ctls_.push_back(c)) return fail("out of memory");
    return true;
  }

  bool read_block_type(const FuncSig** sig, bool* has_single, ValType* single) {
    *sig = nullptr;
    *has_single = false;
    uint8_t b;
    if (!r_.peek(&b)) return fail("unexpected end");
    if (b == 0x40) {
      r_.u8();
      return true;
    }
    if (valid_val_type(b)) {
      r_.u8();
      *has_single = true;
      *single = static_cast<ValType>(b);
      return true;
    }
    const int64_t index = r_.s33();
    if (!r_.ok()) return fail("malformed block type");
    if (index < 0 || static_cast<uint64_t>(index) >= m_.types.count()) return fail("unknown type");
    *sig = m_.types[static_cast<uint64_t>(index)].get();
    return true;
  }

  // Appends a branch to the label `depth` (0 is the innermost block): `op` is kOpBr or kOpBrIf (or a target in the br_table).
  bool label_info(uint32_t depth, Ctl** target) {
    if (depth >= ctls_.count()) return fail("unknown label");
    *target = &ctls_[ctls_.count() - 1 - depth];
    return true;
  }
  bool emit_branch(uint16_t op, uint32_t depth) {
    Ctl* t;
    if (!label_info(depth, &t)) return false;
    if (!live()) return true;
    Instr i;
    i.op = op;
    i.b = (static_cast<uint64_t>(t->label_arity()) << 32) | (fc_.nlocals + t->height);
    if (t->kind == CtlKind::Loop) {
      i.a = t->loop_pc;
    } else {
      i.a = t->patch_code;
      t->patch_code = here();
    }
    return fc_.code.push_back(i) || fail("out of memory");
  }
  bool add_table_target(uint32_t depth) {
    Ctl* t;
    if (!label_info(depth, &t)) return false;
    BrTarget bt;
    bt.arity = t->label_arity();
    bt.height = fc_.nlocals + t->height;
    if (t->kind == CtlKind::Loop) {
      bt.pc = t->loop_pc;
    } else {
      bt.pc = t->patch_table;
      t->patch_table = static_cast<uint32_t>(fc_.targets.count());
    }
    return fc_.targets.push_back(bt) || fail("out of memory");
  }
  void resolve(const Ctl& c, uint32_t target) {
    for (uint32_t i = c.patch_code; i != kNone;) {
      const uint32_t next = fc_.code[i].a;
      fc_.code[i].a = target;
      i = next;
    }
    for (uint32_t i = c.patch_table; i != kNone;) {
      const uint32_t next = fc_.targets[i].pc;
      fc_.targets[i].pc = target;
      i = next;
    }
  }

  bool memarg(uint32_t max_align, uint16_t* memidx, uint32_t* offset) {
    uint32_t align = r_.u32();
    uint32_t mem = 0;
    if (align & 0x40) {
      align &= ~0x40u;
      mem = r_.u32();
    }
    *offset = r_.u32();
    if (!r_.ok()) return fail("malformed memory argument");
    if (mem >= m_.memories.count() || mem > 0xFFFF) return fail("unknown memory");
    if (align > max_align) return fail("alignment must not be larger than natural");
    *memidx = static_cast<uint16_t>(mem);
    return true;
  }
  bool memory_index(uint16_t* memidx) {
    const uint32_t mem = r_.u32();
    if (!r_.ok()) return fail("malformed memory index");
    if (mem >= m_.memories.count() || mem > 0xFFFF) return fail("unknown memory");
    *memidx = static_cast<uint16_t>(mem);
    return true;
  }
  bool table_index(uint32_t* t) {
    *t = r_.u32();
    if (!r_.ok()) return fail("malformed table index");
    if (*t >= m_.tables.count()) return fail("unknown table");
    return true;
  }

  bool numeric(uint8_t op);
  bool simd(uint32_t sub);
  bool prefix_fc();
  bool one(uint8_t op);
  bool call_target(uint32_t func, bool tail);
  bool call_indirect(bool tail);
  bool load(uint8_t op);
  bool store(uint8_t op);
  bool end_block();
  // Appends a catch clause to the chain whose ends are *first and *last.
  bool link_clause(const CatchClause& clause, uint32_t* first, uint32_t* last) {
    const uint32_t index = static_cast<uint32_t>(fc_.catches.count());
    if (!fc_.catches.push_back(clause)) return fail("out of memory");
    if (*last != kNone) fc_.catches[*last].next = index;
    else *first = index;
    *last = index;
    return true;
  }
  bool use_legacy_eh() {
    if (m_.new_eh) return fail("module uses a mix of legacy and new exception handling instructions");
    m_.legacy_eh = true;
    return true;
  }
  bool use_new_eh() {
    if (m_.legacy_eh) return fail("module uses a mix of legacy and new exception handling instructions");
    m_.new_eh = true;
    return true;
  }
  bool legacy_catch(bool all);
  bool legacy_delegate();
};

// The operand types of a plain numeric operation (no immediates): in1 and in2 (0: none) and the result.
bool numeric_signature(uint8_t op, ValType* in1, ValType* in2, ValType* out, uint32_t* nin) {
  constexpr ValType I = ValType::I32, L = ValType::I64, F = ValType::F32, D = ValType::F64;
  *nin = 2;
  if (op == 0x45) { *nin = 1; *in1 = I; *out = I; return true; }
  if (op >= 0x46 && op <= 0x4F) { *in1 = I; *in2 = I; *out = I; return true; }
  if (op == 0x50) { *nin = 1; *in1 = L; *out = I; return true; }
  if (op >= 0x51 && op <= 0x5A) { *in1 = L; *in2 = L; *out = I; return true; }
  if (op >= 0x5B && op <= 0x60) { *in1 = F; *in2 = F; *out = I; return true; }
  if (op >= 0x61 && op <= 0x66) { *in1 = D; *in2 = D; *out = I; return true; }
  if (op >= 0x67 && op <= 0x69) { *nin = 1; *in1 = I; *out = I; return true; }
  if (op >= 0x6A && op <= 0x78) { *in1 = I; *in2 = I; *out = I; return true; }
  if (op >= 0x79 && op <= 0x7B) { *nin = 1; *in1 = L; *out = L; return true; }
  if (op >= 0x7C && op <= 0x8A) { *in1 = L; *in2 = L; *out = L; return true; }
  if (op >= 0x8B && op <= 0x91) { *nin = 1; *in1 = F; *out = F; return true; }
  if (op >= 0x92 && op <= 0x98) { *in1 = F; *in2 = F; *out = F; return true; }
  if (op >= 0x99 && op <= 0x9F) { *nin = 1; *in1 = D; *out = D; return true; }
  if (op >= 0xA0 && op <= 0xA6) { *in1 = D; *in2 = D; *out = D; return true; }
  *nin = 1;
  switch (op) {
    case 0xA7: *in1 = L; *out = I; return true;
    case 0xA8: case 0xA9: *in1 = F; *out = I; return true;
    case 0xAA: case 0xAB: *in1 = D; *out = I; return true;
    case 0xAC: case 0xAD: *in1 = I; *out = L; return true;
    case 0xAE: case 0xAF: *in1 = F; *out = L; return true;
    case 0xB0: case 0xB1: *in1 = D; *out = L; return true;
    case 0xB2: case 0xB3: *in1 = I; *out = F; return true;
    case 0xB4: case 0xB5: *in1 = L; *out = F; return true;
    case 0xB6: *in1 = D; *out = F; return true;
    case 0xB7: case 0xB8: *in1 = I; *out = D; return true;
    case 0xB9: case 0xBA: *in1 = L; *out = D; return true;
    case 0xBB: *in1 = F; *out = D; return true;
    case 0xBC: *in1 = F; *out = I; return true;
    case 0xBD: *in1 = D; *out = L; return true;
    case 0xBE: *in1 = I; *out = F; return true;
    case 0xBF: *in1 = L; *out = D; return true;
    case 0xC0: case 0xC1: *in1 = I; *out = I; return true;
    case 0xC2: case 0xC3: case 0xC4: *in1 = L; *out = L; return true;
  }
  return false;
}

bool FuncCompiler::numeric(uint8_t op) {
  ValType a = ValType::I32, b = ValType::I32, out = ValType::I32;
  uint32_t nin;
  if (!numeric_signature(op, &a, &b, &out, &nin)) return fail("illegal opcode");
  if (nin == 2 && !pop_t(b)) return false;
  if (!pop_t(a) || !push_t(out)) return false;
  return emit(op);
}

// The shape of a SIMD operation: what it pops and pushes.
enum class Shape : uint8_t {
  Invalid, Load, Store, LoadLane, StoreLane, LoadZero, Const, Shuffle, Splat_I32, Splat_I64, Splat_F32, Splat_F64, Extract_I32, Extract_I64, Extract_F32, Extract_F64,
  Replace_I32, Replace_I64, Replace_F32, Replace_F64, Unary, Binary, Ternary, Shift, ToI32
};

Shape simd_shape(uint32_t s, uint32_t* lanes) {
  *lanes = 0;
  if (s <= 10) return Shape::Load;
  if (s == 11) return Shape::Store;
  if (s == 12) return Shape::Const;
  if (s == 13) return Shape::Shuffle;
  if (s == 14) return Shape::Binary;
  if (s >= 15 && s <= 17) return Shape::Splat_I32;
  if (s == 15 || s == 16 || s == 17) return Shape::Splat_I32;
  if (s == 18) return Shape::Splat_I64;
  if (s == 19) return Shape::Splat_F32;
  if (s == 20) return Shape::Splat_F64;
  switch (s) {
    case 21: case 22: *lanes = 16; return Shape::Extract_I32;
    case 23: *lanes = 16; return Shape::Replace_I32;
    case 24: case 25: *lanes = 8; return Shape::Extract_I32;
    case 26: *lanes = 8; return Shape::Replace_I32;
    case 27: *lanes = 4; return Shape::Extract_I32;
    case 28: *lanes = 4; return Shape::Replace_I32;
    case 29: *lanes = 2; return Shape::Extract_I64;
    case 30: *lanes = 2; return Shape::Replace_I64;
    case 31: *lanes = 4; return Shape::Extract_F32;
    case 32: *lanes = 4; return Shape::Replace_F32;
    case 33: *lanes = 2; return Shape::Extract_F64;
    case 34: *lanes = 2; return Shape::Replace_F64;
  }
  if (s >= 35 && s <= 76) return Shape::Binary;
  if (s == 256) return Shape::Binary;                       // relaxed SIMD (0x100 ...)
  if (s >= 257 && s <= 260) return Shape::Unary;
  if (s >= 261 && s <= 268) return Shape::Ternary;
  if (s >= 269 && s <= 274) return Shape::Binary;
  if (s == 275) return Shape::Ternary;
  if (s == 77) return Shape::Unary;
  if (s >= 78 && s <= 81) return Shape::Binary;
  if (s == 82) return Shape::Ternary;
  if (s == 83) return Shape::ToI32;
  if (s >= 84 && s <= 87) return Shape::LoadLane;
  if (s >= 88 && s <= 91) return Shape::StoreLane;
  if (s == 92 || s == 93) return Shape::LoadZero;
  if (s == 94 || s == 95) return Shape::Unary;
  switch (s) {
    case 96: case 97: case 98: case 103: case 104: case 105: case 106: case 116: case 117: case 122: case 124: case 125: case 126: case 127: case 128: case 129:
    case 135: case 136: case 137: case 138: case 148: case 160: case 161: case 167: case 168: case 169: case 170: case 192: case 193: case 199: case 200: case 201:
    case 202: case 224: case 225: case 227: case 236: case 237: case 239: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 255:
      return Shape::Unary;
    case 99: case 100: case 131: case 132: case 163: case 164: case 195: case 196: return Shape::ToI32;
    case 107: case 108: case 109: case 139: case 140: case 141: case 171: case 172: case 173: case 203: case 204: case 205: return Shape::Shift;
    case 101: case 102: case 110: case 111: case 112: case 113: case 114: case 115: case 118: case 119: case 120: case 121: case 123: case 130: case 133: case 134:
    case 142: case 143: case 144: case 145: case 146: case 147: case 149: case 150: case 151: case 152: case 153: case 155: case 156: case 157: case 158: case 159:
    case 174: case 177: case 181: case 182: case 183: case 184: case 185: case 186: case 188: case 189: case 190: case 191: case 206: case 209: case 213: case 214:
    case 215: case 216: case 217: case 218: case 219: case 220: case 221: case 222: case 223: case 228: case 229: case 230: case 231: case 232: case 233: case 234:
    case 235: case 240: case 241: case 242: case 243: case 244: case 245: case 246: case 247:
      return Shape::Binary;
  }
  return Shape::Invalid;
}

bool FuncCompiler::simd(uint32_t sub) {
  uint32_t lanes;
  const Shape shape = simd_shape(sub, &lanes);
  const uint16_t op = static_cast<uint16_t>(0x200 + sub);
  constexpr ValType V = ValType::V128, I = ValType::I32, L = ValType::I64, F = ValType::F32, D = ValType::F64;
  uint16_t mem = 0;
  uint32_t offset = 0;
  switch (shape) {
    case Shape::Invalid: return fail("illegal opcode");
    case Shape::Load: {
      const uint32_t align = sub == 0 ? 4 : (sub <= 6 ? 3 : (sub == 7 ? 0 : (sub == 8 ? 1 : (sub == 9 ? 2 : 3))));
      if (!memarg(align, &mem, &offset) || !pop_t(I) || !push_t(V)) return false;
      return emit(op, offset, 0, mem);
    }
    case Shape::Store:
      if (!memarg(4, &mem, &offset) || !pop_t(V) || !pop_t(I)) return false;
      return emit(op, offset, 0, mem);
    case Shape::LoadLane:
    case Shape::StoreLane: {
      const uint32_t width = 1u << (sub >= 88 ? sub - 88 : sub - 84);
      if (!memarg(sub >= 88 ? sub - 88 : sub - 84, &mem, &offset)) return false;
      const uint8_t lane = r_.u8();
      if (!r_.ok() || lane >= 16 / width) return fail("invalid lane index");
      if (!pop_t(V) || !pop_t(I)) return false;
      if (shape == Shape::LoadLane && !push_t(V)) return false;
      return emit(op, offset, lane, mem);
    }
    case Shape::LoadZero:
      if (!memarg(sub == 92 ? 2 : 3, &mem, &offset) || !pop_t(I) || !push_t(V)) return false;
      return emit(op, offset, 0, mem);
    case Shape::Const: {
      ConstByteView bytes = r_.take(16);
      if (!r_.ok()) return fail("unexpected end");
      uint64_t lo = 0, hi = 0;
      for (uint32_t i = 0; i < 8; ++i) {
        lo |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        hi |= static_cast<uint64_t>(bytes[8 + i]) << (8 * i);
      }
      if (!push_t(V)) return false;
      return emit(op, 0, lo) && emit(kOpMulti, 0, hi);
    }
    case Shape::Shuffle: {
      ConstByteView bytes = r_.take(16);
      if (!r_.ok()) return fail("unexpected end");
      uint64_t lo = 0, hi = 0;
      for (uint32_t i = 0; i < 8; ++i) {
        if (bytes[i] >= 32 || bytes[8 + i] >= 32) return fail("invalid lane index");
        lo |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        hi |= static_cast<uint64_t>(bytes[8 + i]) << (8 * i);
      }
      if (!pop_t(V) || !pop_t(V) || !push_t(V)) return false;
      return emit(op, 0, lo) && emit(kOpMulti, 0, hi);
    }
    case Shape::Splat_I32:
    case Shape::Splat_I64:
    case Shape::Splat_F32:
    case Shape::Splat_F64: {
      const ValType t = shape == Shape::Splat_I32 ? I : (shape == Shape::Splat_I64 ? L : (shape == Shape::Splat_F32 ? F : D));
      if (!pop_t(t) || !push_t(V)) return false;
      return emit(op);
    }
    case Shape::Extract_I32:
    case Shape::Extract_I64:
    case Shape::Extract_F32:
    case Shape::Extract_F64: {
      const uint8_t lane = r_.u8();
      if (!r_.ok() || lane >= lanes) return fail("invalid lane index");
      const ValType t = shape == Shape::Extract_I32 ? I : (shape == Shape::Extract_I64 ? L : (shape == Shape::Extract_F32 ? F : D));
      if (!pop_t(V) || !push_t(t)) return false;
      return emit(op, lane);
    }
    case Shape::Replace_I32:
    case Shape::Replace_I64:
    case Shape::Replace_F32:
    case Shape::Replace_F64: {
      const uint8_t lane = r_.u8();
      if (!r_.ok() || lane >= lanes) return fail("invalid lane index");
      const ValType t = shape == Shape::Replace_I32 ? I : (shape == Shape::Replace_I64 ? L : (shape == Shape::Replace_F32 ? F : D));
      if (!pop_t(t) || !pop_t(V) || !push_t(V)) return false;
      return emit(op, lane);
    }
    case Shape::Unary:
      if (!pop_t(V) || !push_t(V)) return false;
      return emit(op);
    case Shape::Binary:
      if (!pop_t(V) || !pop_t(V) || !push_t(V)) return false;
      return emit(op);
    case Shape::Ternary:
      if (!pop_t(V) || !pop_t(V) || !pop_t(V) || !push_t(V)) return false;
      return emit(op);
    case Shape::Shift:
      if (!pop_t(I) || !pop_t(V) || !push_t(V)) return false;
      return emit(op);
    case Shape::ToI32:
      if (!pop_t(V) || !push_t(I)) return false;
      return emit(op);
  }
  return fail("illegal opcode");
}

bool FuncCompiler::load(uint8_t op) {
  static const ValType kTypes[] = {ValType::I32, ValType::I64, ValType::F32, ValType::F64, ValType::I32, ValType::I32, ValType::I32, ValType::I32,
                                   ValType::I64, ValType::I64, ValType::I64, ValType::I64, ValType::I64, ValType::I64};
  static const uint8_t kAlign[] = {2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2};
  const uint32_t i = op - 0x28;
  uint16_t mem;
  uint32_t offset;
  if (!memarg(kAlign[i], &mem, &offset) || !pop_t(ValType::I32) || !push_t(kTypes[i])) return false;
  return emit(op, offset, 0, mem);
}

bool FuncCompiler::store(uint8_t op) {
  static const ValType kTypes[] = {ValType::I32, ValType::I64, ValType::F32, ValType::F64, ValType::I32, ValType::I32, ValType::I64, ValType::I64, ValType::I64};
  static const uint8_t kAlign[] = {2, 3, 2, 3, 0, 1, 0, 1, 2};
  const uint32_t i = op - 0x36;
  uint16_t mem;
  uint32_t offset;
  if (!memarg(kAlign[i], &mem, &offset) || !pop_t(kTypes[i]) || !pop_t(ValType::I32)) return false;
  return emit(op, offset, 0, mem);
}

bool FuncCompiler::call_target(uint32_t func, bool tail) {
  if (func >= m_.func_types.count()) return fail("unknown function");
  const FuncSig& sig = *m_.types[m_.func_types[func]];
  if (tail) {
    const FuncSig& mine = *m_.types[fc_.type_index];
    if (sig.nresults != mine.nresults) return fail("type mismatch in return_call");
    for (uint32_t i = 0; i < sig.nresults; ++i)
      if (sig.results[i] != mine.results[i]) return fail("type mismatch in return_call");
  }
  if (!pop_sig_params(sig)) return false;
  if (tail) {
    if (!emit(0x12, func)) return false;
    return mark_unreachable();
  }
  if (!push_sig_results(sig)) return false;
  return emit(0x10, func);
}

bool FuncCompiler::call_indirect(bool tail) {
  const uint32_t type = r_.u32();
  if (!r_.ok()) return fail("malformed call_indirect");
  uint32_t table;
  if (!table_index(&table)) return false;
  if (type >= m_.types.count()) return fail("unknown type");
  if (m_.tables[table].elem != ValType::FuncRef) return fail("type mismatch: call_indirect needs a funcref table");
  const FuncSig& sig = *m_.types[type];
  if (tail) {
    const FuncSig& mine = *m_.types[fc_.type_index];
    if (sig.nresults != mine.nresults) return fail("type mismatch in return_call_indirect");
    for (uint32_t i = 0; i < sig.nresults; ++i)
      if (sig.results[i] != mine.results[i]) return fail("type mismatch in return_call_indirect");
  }
  if (!pop_t(ValType::I32) || !pop_sig_params(sig)) return false;
  if (tail) {
    if (!emit(0x13, type, table)) return false;
    return mark_unreachable();
  }
  if (!push_sig_results(sig)) return false;
  return emit(0x11, type, table);
}

bool FuncCompiler::end_block() {
  // `end`: the values on the stack are exactly the block's results
  Ctl c = top();
  if (!pop_ctl_results(c)) return false;
  if (vals_.count() != c.base()) return fail("type mismatch: values left on the stack at the end of a block");
  if (c.kind == CtlKind::Catch || c.kind == CtlKind::CatchAll) vals_.remove_at(vals_.count() - 1);   // (the exception under the block's values)
  if (c.kind == CtlKind::If) {   // an `if` without `else` passes its parameters through
    if (c.nparams() != c.nresults()) return fail("type mismatch: if without else");
    for (uint32_t i = 0; i < c.nparams(); ++i)
      if (c.param(i) != c.result(i)) return fail("type mismatch: if without else");
  }
  const bool dead = c.dead;
  ctls_.remove_at(ctls_.count() - 1);
  if (!dead) {
    const uint32_t target = here();
    if (c.handler != kNone && fc_.handlers[c.handler].end == kNone) fc_.handlers[c.handler].end = target;
    if (c.else_patch != kNone) fc_.code[c.else_patch].a = target;
    resolve(c, target);
  }
  if (c.kind == CtlKind::Func) {
    Instr ret;
    ret.op = kOpReturn;
    if (!fc_.code.push_back(ret)) return fail("out of memory");
    return true;
  }
  return push_ctl_results(c);
}

// `catch tag` / `catch_all` of a legacy try: ends the body that came before (a try body or the previous catch block) and begins a block whose operands are
// the values of the exception, above one slot that holds the exception itself.
bool FuncCompiler::legacy_catch(bool all) {
  Ctl& first = top();
  if (first.kind != CtlKind::Try && first.kind != CtlKind::Catch) return fail("catch without try");
  Ctl saved = first;
  if (!pop_ctl_results(saved)) return false;
  if (vals_.count() != saved.base()) return fail("type mismatch: values left on the stack before catch");
  Ctl& t = top();
  if (!t.dead) {
    if (t.handler != kNone && fc_.handlers[t.handler].end == kNone) fc_.handlers[t.handler].end = here();
    if (!t.unreachable) {   // the body before falls through to the end of the whole try
      Instr j;
      j.op = kOpJmp;
      j.a = t.patch_code;
      t.patch_code = here();
      if (!fc_.code.push_back(j)) return fail("out of memory");
    }
  }
  truncate_to(vals_, t.height);
  t.kind = all ? CtlKind::CatchAll : CtlKind::Catch;
  t.unreachable = false;
  if (!push_t(ValType::ExnRef)) return false;
  const FuncSig* tag_sig = all ? nullptr : m_.types[m_.tags[cur_tag_]].get();
  for (uint32_t i = 0; tag_sig && i < tag_sig->nparams; ++i)
    if (!push_t(tag_sig->params[i])) return false;
  Ctl& c = top();
  if (!c.dead && c.handler != kNone) {
    BrTarget bt;
    bt.pc = here();
    bt.height = fc_.nlocals + c.height;
    bt.arity = tag_sig ? tag_sig->nparams : 0;
    if (!fc_.targets.push_back(bt)) return fail("out of memory");
    CatchClause cc;
    cc.kind = all ? CatchKind::LegacyCatchAll : CatchKind::LegacyCatch;
    cc.tag = all ? 0 : cur_tag_;
    cc.target = static_cast<uint32_t>(fc_.targets.count() - 1);
    uint32_t head = fc_.handlers[c.handler].first, tail = c.last_clause;
    if (!link_clause(cc, &head, &tail)) return false;
    fc_.handlers[c.handler].first = head;
    c.last_clause = tail;
    c.cur_clause = tail;
  }
  return true;
}

// `delegate label`: ends a try that has no catch clauses; whatever is thrown in its body goes on to the handlers of the block `label` (counted from outside the try)
// or, for the function, to the caller.
bool FuncCompiler::legacy_delegate() {
  const uint32_t depth = r_.u32();
  if (!r_.ok()) return fail("malformed label");
  if (top().kind != CtlKind::Try) return fail("delegate without try");
  if (depth + 2 > ctls_.count()) return fail("unknown label");
  const Ctl target = ctls_[ctls_.count() - 2 - depth];
  Ctl& t = top();
  if (!t.dead && t.handler != kNone) {
    CatchClause cc;
    cc.kind = CatchKind::Delegate;
    // (a delegate to a try goes to that try's catches too, to any other block to the handlers that surround it)
    cc.tag = target.kind == CtlKind::Try && target.handler != kNone ? target.handler + 1 : target.handlers_before;
    uint32_t head = fc_.handlers[t.handler].first, tail = t.last_clause;
    if (!link_clause(cc, &head, &tail)) return false;
    fc_.handlers[t.handler].first = head;
    t.last_clause = tail;
  }
  return end_block();
}

bool FuncCompiler::prefix_fc() {
  const uint32_t sub = r_.u32();
  if (!r_.ok()) return fail("malformed opcode");
  constexpr ValType I = ValType::I32, L = ValType::I64, F = ValType::F32, D = ValType::F64;
  const uint16_t op = static_cast<uint16_t>(0x100 + sub);
  if (sub <= 7) {
    static const ValType kIn[] = {F, F, D, D, F, F, D, D};
    static const ValType kOut[] = {I, I, I, I, L, L, L, L};
    if (!pop_t(kIn[sub]) || !push_t(kOut[sub])) return false;
    return emit(op);
  }
  uint16_t mem = 0, mem2 = 0;
  uint32_t t1 = 0, t2 = 0;
  switch (sub) {
    case 8: {   // memory.init
      const uint32_t data = r_.u32();
      if (!r_.ok()) return fail("malformed opcode");
      if (!memory_index(&mem)) return false;
      if (!has_datacount_) return fail("data count section required");
      if (data >= datacount_) return fail("unknown data segment");
      if (!pop_t(I) || !pop_t(I) || !pop_t(I)) return false;
      return emit(op, data, 0, mem);
    }
    case 9: {   // data.drop
      const uint32_t data = r_.u32();
      if (!r_.ok()) return fail("malformed opcode");
      if (!has_datacount_) return fail("data count section required");
      if (data >= datacount_) return fail("unknown data segment");
      return emit(op, data);
    }
    case 10:   // memory.copy
      if (!memory_index(&mem) || !memory_index(&mem2)) return false;
      if (!pop_t(I) || !pop_t(I) || !pop_t(I)) return false;
      return emit(op, mem, mem2);
    case 11:   // memory.fill
      if (!memory_index(&mem) || !pop_t(I) || !pop_t(I) || !pop_t(I)) return false;
      return emit(op, 0, 0, mem);
    case 12: {   // table.init
      const uint32_t elem = r_.u32();
      if (!r_.ok()) return fail("malformed opcode");
      if (!table_index(&t1)) return false;
      if (elem >= m_.elems.count()) return fail("unknown elem segment");
      if (m_.elems[elem]->type != m_.tables[t1].elem) return fail("type mismatch: table.init");
      if (!pop_t(I) || !pop_t(I) || !pop_t(I)) return false;
      return emit(op, elem, t1);
    }
    case 13: {   // elem.drop
      const uint32_t elem = r_.u32();
      if (!r_.ok()) return fail("malformed opcode");
      if (elem >= m_.elems.count()) return fail("unknown elem segment");
      return emit(op, elem);
    }
    case 14:   // table.copy
      if (!table_index(&t1) || !table_index(&t2)) return false;
      if (m_.tables[t1].elem != m_.tables[t2].elem) return fail("type mismatch: table.copy");
      if (!pop_t(I) || !pop_t(I) || !pop_t(I)) return false;
      return emit(op, t1, t2);
    case 15:   // table.grow
      if (!table_index(&t1)) return false;
      if (!pop_t(I) || !pop_t(m_.tables[t1].elem) || !push_t(I)) return false;
      return emit(op, t1);
    case 16:   // table.size
      if (!table_index(&t1) || !push_t(I)) return false;
      return emit(op, t1);
    case 17:   // table.fill
      if (!table_index(&t1)) return false;
      if (!pop_t(I) || !pop_t(m_.tables[t1].elem) || !pop_t(I)) return false;
      return emit(op, t1);
  }
  return fail("illegal opcode");
}

bool FuncCompiler::one(uint8_t op) {
  constexpr ValType I = ValType::I32, L = ValType::I64, F = ValType::F32, D = ValType::F64;
  switch (op) {
    case 0x00:
      if (!emit(0x00)) return false;
      return mark_unreachable();
    case 0x01: return true;
    case 0x02:
    case 0x03: {
      const FuncSig* sig;
      bool has_single;
      ValType single = I;
      if (!read_block_type(&sig, &has_single, &single)) return false;
      if (sig) {
        Ctl probe;
        probe.sig = sig;
        if (!pop_ctl_params(probe)) return false;
      }
      if (!push_ctl(op == 0x02 ? CtlKind::Block : CtlKind::Loop, sig, has_single, single)) return false;
      return push_ctl_params(top());
    }
    case 0x04: {
      const FuncSig* sig;
      bool has_single;
      ValType single = I;
      if (!read_block_type(&sig, &has_single, &single)) return false;
      if (!pop_t(I)) return false;
      if (sig) {
        Ctl probe;
        probe.sig = sig;
        if (!pop_ctl_params(probe)) return false;
      }
      const bool was_live = live();
      const uint32_t jump = here();
      if (!emit(kOpJmpIfZero, kNone)) return false;
      if (!push_ctl(CtlKind::If, sig, has_single, single)) return false;
      if (was_live) top().else_patch = jump;
      return push_ctl_params(top());
    }
    case 0x05: {
      Ctl& c = top();
      if (c.kind != CtlKind::If) return fail("else without if");
      Ctl saved = c;
      if (!pop_ctl_results(saved)) return false;
      if (vals_.count() != saved.height) return fail("type mismatch: values left on the stack before else");
      Ctl& t = top();
      if (!t.dead) {
        if (!t.unreachable) {   // the true branch falls through to the end of the whole `if`
          Instr j;
          j.op = kOpJmp;
          j.a = t.patch_code;
          t.patch_code = here();
          if (!fc_.code.push_back(j)) return fail("out of memory");
        }
        if (t.else_patch != kNone) fc_.code[t.else_patch].a = here();
        t.else_patch = kNone;
      }
      t.kind = CtlKind::Else;
      t.unreachable = false;
      return push_ctl_params(t);
    }
    case 0x0B: return end_block();
    case 0x06: {   // try (the legacy form)
      if (!use_legacy_eh()) return false;
      const FuncSig* sig;
      bool has_single;
      ValType single = I;
      if (!read_block_type(&sig, &has_single, &single)) return false;
      if (sig) {
        Ctl probe;
        probe.sig = sig;
        if (!pop_ctl_params(probe)) return false;
      }
      const bool was_live = live();
      const uint32_t start = here();
      if (!push_ctl(CtlKind::Try, sig, has_single, single)) return false;
      if (was_live) {
        Handler h;
        h.start = start;
        if (!fc_.handlers.push_back(h)) return fail("out of memory");
        top().handler = static_cast<uint32_t>(fc_.handlers.count() - 1);
      }
      return push_ctl_params(top());
    }
    case 0x07: {   // catch
      if (!use_legacy_eh()) return false;
      const uint32_t tag = r_.u32();
      if (!r_.ok()) return fail("malformed tag index");
      if (tag >= m_.tags.count()) return fail("unknown tag");
      cur_tag_ = tag;
      return legacy_catch(false);
    }
    case 0x19: return use_legacy_eh() && legacy_catch(true);
    case 0x18: return use_legacy_eh() && legacy_delegate();
    case 0x09: {   // rethrow
      if (!use_legacy_eh()) return false;
      const uint32_t depth = r_.u32();
      if (!r_.ok()) return fail("malformed label");
      Ctl* t;
      if (!label_info(depth, &t)) return false;
      if (t->kind != CtlKind::Catch && t->kind != CtlKind::CatchAll) return fail("invalid rethrow label: not a catch block");
      if (live()) {
        if (!emit(0x09, fc_.nlocals + t->height)) return false;
        if (t->cur_clause != kNone) fc_.catches[t->cur_clause].keep = true;
      }
      return mark_unreachable();
    }
    case 0x08: {   // throw
      const uint32_t tag = r_.u32();
      if (!r_.ok()) return fail("malformed tag index");
      if (tag >= m_.tags.count()) return fail("unknown tag");
      if (!pop_sig_params(*m_.types[m_.tags[tag]]) || !emit(0x08, tag)) return false;
      return mark_unreachable();
    }
    case 0x0A:   // throw_ref
      if (!use_new_eh() || !pop_t(ValType::ExnRef) || !emit(0x0A)) return false;
      return mark_unreachable();
    case 0x1F: {   // try_table
      if (!use_new_eh()) return false;
      const FuncSig* sig;
      bool has_single;
      ValType single = I;
      if (!read_block_type(&sig, &has_single, &single)) return false;
      const uint32_t n = r_.u32();
      if (!r_.ok() || n > kMaxLabels) return fail("malformed catch list");
      const bool was_live = live();
      uint32_t first_clause = kNone, last_clause = kNone;
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t kind = r_.u8();
        uint32_t tag = 0;
        if (kind <= 1) tag = r_.u32();
        const uint32_t depth = r_.u32();
        if (!r_.ok() || kind > 3) return fail("malformed catch clause");
        Ctl* t;
        if (!label_info(depth, &t)) return false;   // (the labels outside the try_table: its own is not among them)
        uint32_t want = 0;
        const FuncSig* tag_sig = nullptr;
        if (kind <= 1) {
          if (tag >= m_.tags.count()) return fail("unknown tag");
          tag_sig = m_.types[m_.tags[tag]].get();
          want = tag_sig->nparams;
        }
        if (kind == 1 || kind == 3) ++want;
        if (t->label_arity() != want) return fail("type mismatch: catch label");
        for (uint32_t k = 0; tag_sig && k < tag_sig->nparams; ++k)
          if (t->label_type(k) != tag_sig->params[k]) return fail("type mismatch: catch label");
        if ((kind == 1 || kind == 3) && t->label_type(want - 1) != ValType::ExnRef) return fail("type mismatch: catch label");
        if (was_live) {
          if (!add_table_target(depth)) return false;
          CatchClause cc;
          cc.kind = static_cast<CatchKind>(kind);
          cc.tag = tag;
          cc.target = static_cast<uint32_t>(fc_.targets.count() - 1);
          if (!link_clause(cc, &first_clause, &last_clause)) return false;
        }
      }
      if (sig) {
        Ctl probe;
        probe.sig = sig;
        if (!pop_ctl_params(probe)) return false;
      }
      const uint32_t start = here();
      if (!push_ctl(CtlKind::TryTable, sig, has_single, single)) return false;
      if (was_live) {
        Handler h;
        h.start = start;
        h.first = first_clause;
        if (!fc_.handlers.push_back(h)) return fail("out of memory");
        top().handler = static_cast<uint32_t>(fc_.handlers.count() - 1);
      }
      return push_ctl_params(top());
    }
    case 0x0C: {
      const uint32_t depth = r_.u32();
      if (!r_.ok()) return fail("malformed label");
      Ctl* t;
      if (!label_info(depth, &t)) return false;
      Ctl copy = *t;
      if (!pop_label(copy)) return false;
      if (!emit_branch(kOpBr, depth)) return false;
      return mark_unreachable();
    }
    case 0x0D: {
      const uint32_t depth = r_.u32();
      if (!r_.ok()) return fail("malformed label");
      Ctl* t;
      if (!label_info(depth, &t)) return false;
      Ctl copy = *t;
      if (!pop_t(I) || !pop_label(copy) || !push_label(copy)) return false;
      return emit_branch(kOpBrIf, depth);
    }
    case 0x0E: {
      const uint32_t count = r_.u32();
      if (!r_.ok() || count > kMaxLabels) return fail("malformed br_table");
      if (!pop_t(I)) return false;
      const uint32_t first = static_cast<uint32_t>(fc_.targets.count());
      uint32_t arity = 0;
      for (uint32_t i = 0; i <= count; ++i) {
        const uint32_t depth = r_.u32();
        if (!r_.ok()) return fail("malformed br_table");
        Ctl* t;
        if (!label_info(depth, &t)) return false;
        Ctl copy = *t;
        if (i == 0) arity = copy.label_arity();
        else if (copy.label_arity() != arity) return fail("type mismatch: br_table targets have different arities");
        if (i < count) {
          if (!pop_label(copy) || !push_label(copy)) return false;
        } else if (!pop_label(copy)) {
          return false;
        }
        if (live() && !add_table_target(depth)) return false;
      }
      if (!emit(kOpBrTable, first, count)) return false;
      return mark_unreachable();
    }
    case 0x0F: {
      const FuncSig& sig = *m_.types[fc_.type_index];
      for (uint32_t i = sig.nresults; i > 0; --i)
        if (!pop_t(sig.results[i - 1])) return false;
      if (!emit(kOpReturn)) return false;
      return mark_unreachable();
    }
    case 0x10: {
      const uint32_t f = r_.u32();
      if (!r_.ok()) return fail("malformed function index");
      return call_target(f, false);
    }
    case 0x11: return call_indirect(false);
    case 0x12: {
      const uint32_t f = r_.u32();
      if (!r_.ok()) return fail("malformed function index");
      return call_target(f, true);
    }
    case 0x13: return call_indirect(true);
    case 0x1A: {
      uint8_t t;
      return pop_any(&t) && emit(0x1A);
    }
    case 0x1B:
    case 0x1C: {
      uint8_t want = kUnknown;
      if (op == 0x1C) {
        const uint32_t n = r_.u32();
        if (!r_.ok() || n != 1) return fail("invalid result arity");
        const uint8_t t = r_.u8();
        if (!valid_val_type(t)) return fail("invalid value type");
        want = t;
      }
      if (!pop_t(I)) return false;
      uint8_t a, b;
      if (!pop_any(&a) || !pop_any(&b)) return false;
      if (op == 0x1B) {
        if ((a == 0x70 || a == 0x6F || a == 0x69 || b == 0x70 || b == 0x6F || b == 0x69)) return fail("type mismatch: select needs a type for references");
        if (a != b && a != kUnknown && b != kUnknown) return fail("type mismatch in select");
        return push(a == kUnknown ? b : a) && emit(0x1B);
      }
      if ((a != want && a != kUnknown) || (b != want && b != kUnknown)) return fail("type mismatch in select");
      return push(want) && emit(0x1B);
    }
    case 0x20:
    case 0x21:
    case 0x22: {
      const uint32_t idx = r_.u32();
      if (!r_.ok()) return fail("malformed local index");
      if (idx >= locals_.count()) return fail("unknown local");
      const ValType t = locals_[idx];
      if (op == 0x20) {
        if (!push_t(t)) return false;
      } else if (op == 0x21) {
        if (!pop_t(t)) return false;
      } else if (!pop_t(t) || !push_t(t)) {
        return false;
      }
      return emit(op, idx);
    }
    case 0x23:
    case 0x24: {
      const uint32_t idx = r_.u32();
      if (!r_.ok()) return fail("malformed global index");
      if (idx >= m_.globals.count()) return fail("unknown global");
      const GlobalType g = m_.globals[idx];
      if (op == 0x23) {
        if (!push_t(g.type)) return false;
      } else {
        if (!g.mut) return fail("global is immutable");
        if (!pop_t(g.type)) return false;
      }
      return emit(op, idx);
    }
    case 0x25: {
      uint32_t t;
      if (!table_index(&t) || !pop_t(I) || !push_t(m_.tables[t].elem)) return false;
      return emit(op, t);
    }
    case 0x26: {
      uint32_t t;
      if (!table_index(&t) || !pop_t(m_.tables[t].elem) || !pop_t(I)) return false;
      return emit(op, t);
    }
    case 0x3F:
    case 0x40: {
      uint16_t mem;
      if (!memory_index(&mem)) return false;
      if (op == 0x40 && !pop_t(I)) return false;
      if (!push_t(I)) return false;
      return emit(op, 0, 0, mem);
    }
    case 0x41: {
      const int32_t v = r_.s32();
      if (!r_.ok()) return fail("malformed constant");
      return push_t(I) && emit(op, 0, static_cast<uint32_t>(v));
    }
    case 0x42: {
      const int64_t v = r_.s64();
      if (!r_.ok()) return fail("malformed constant");
      return push_t(L) && emit(op, 0, static_cast<uint64_t>(v));
    }
    case 0x43: {
      const uint32_t v = r_.fixed32();
      if (!r_.ok()) return fail("unexpected end");
      return push_t(F) && emit(op, 0, v);
    }
    case 0x44: {
      const uint64_t v = r_.fixed64();
      if (!r_.ok()) return fail("unexpected end");
      return push_t(D) && emit(op, 0, v);
    }
    case 0xD0: {
      const uint8_t t = r_.u8();
      if (!r_.ok() || (t != 0x70 && t != 0x6F && t != 0x69)) return fail("invalid reference type");
      return push(t) && emit(0xD0);
    }
    case 0xD1:
      return pop_ref() && push_t(I) && emit(0xD1);
    case 0xD2: {
      const uint32_t f = r_.u32();
      if (!r_.ok()) return fail("malformed function index");
      if (f >= m_.func_types.count()) return fail("unknown function");
      if (f >= declared_.count() || !declared_[f]) return fail("undeclared function reference");
      return push_t(ValType::FuncRef) && emit(0xD2, f);
    }
    case 0xFC: return prefix_fc();
    case 0xFD: {
      const uint32_t sub = r_.u32();
      if (!r_.ok()) return fail("malformed opcode");
      return simd(sub);
    }
  }
  if (op >= 0x28 && op <= 0x35) return load(op);
  if (op >= 0x36 && op <= 0x3E) return store(op);
  if (op >= 0x45 && op <= 0xC4) return numeric(op);
  return fail("illegal opcode");
}

bool FuncCompiler::compile(const char** why) {
  const FuncSig& sig = *m_.types[fc_.type_index];
  fc_.nparams = sig.nparams;
  fc_.nresults = sig.nresults;
  for (uint32_t i = 0; i < sig.nparams; ++i)
    if (!locals_.push_back(sig.params[i])) return fail("out of memory");
  const uint32_t groups = r_.u32();
  if (!r_.ok()) return fail("malformed locals");
  uint64_t total = sig.nparams;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r_.u32();
    const uint8_t t = r_.u8();
    if (!r_.ok()) return fail("malformed locals");
    if (!valid_val_type(t)) return fail("invalid value type");
    total += n;
    if (total > kMaxLocals) return fail("too many locals");
    for (uint32_t i = 0; i < n; ++i)
      if (!locals_.push_back(static_cast<ValType>(t))) return fail("out of memory");
  }
  fc_.nlocals = static_cast<uint32_t>(total);

  Ctl func;
  func.kind = CtlKind::Func;
  func.sig = &sig;
  if (!ctls_.push_back(func)) return fail("out of memory");

  while (ctls_.count() > 0) {
    if (r_.at_end()) {
      *why = "unexpected end of function body";
      return false;
    }
    const uint8_t op = r_.u8();
    if (!one(op)) {
      if (!why_) why_ = r_.why() ? r_.why() : "malformed instruction";
      *why = why_;
      return false;
    }
    if (!r_.ok()) {
      *why = r_.why() ? r_.why() : "unexpected end of function body";
      return false;
    }
  }
  if (!r_.at_end()) {
    *why = "operators remaining after end of function";
    return false;
  }
  fc_.max_stack = max_height_ + 1;
  return true;
}

// --- The module decoder ----------------------------------------------------------------------------------------------------------

class Decoder {
 public:
  Decoder(Module& m, ConstByteView bytes, String* error) : m_(m), bytes_(bytes), error_(error), declared_(m.allocator()) {}

  bool run();

 private:
  Module& m_;
  ConstByteView bytes_;
  String* error_;
  List<uint8_t> declared_;     // functions that ref.func may name
  bool has_datacount_ = false;
  uint32_t datacount_ = 0;
  uint32_t declared_funcs_ = 0;   // the number of entries in the function section
  bool saw_code_ = false;

  bool fail(const Reader& r, const char* why) {
    error_->assign(r.why() ? r.why() : why);
    error_->append(" @+");
    error_->append_uint(r.position());
    return false;
  }
  bool fail_msg(const char* why) {
    error_->assign(why);
    return false;
  }

  bool val_type(Reader& r, ValType* out) {
    const uint8_t b = r.u8();
    if (!r.ok()) return fail(r, "unexpected end");
    if (!valid_val_type(b)) return fail(r, "invalid value type");
    *out = static_cast<ValType>(b);
    return true;
  }
  bool ref_type(Reader& r, ValType* out) {
    if (!val_type(r, out)) return false;
    if (!is_ref_type(*out)) return fail(r, "malformed reference type");
    return true;
  }
  bool limits(Reader& r, Limits* out, uint32_t cap, bool memory) {
    const uint8_t flag = r.u8();
    if (!r.ok()) return fail(r, "unexpected end");
    if (flag > (memory ? 3 : 1)) return fail(r, memory ? "invalid memory limits flags" : "invalid table limits flags");
    out->has_max = flag & 1;
    out->shared = flag & 2;
    out->min = r.u32();
    if (out->has_max) out->max = r.u32();
    if (!r.ok()) return fail(r, "malformed limits");
    if (out->shared && !out->has_max) return fail(r, "shared memory must have a maximum");
    if (out->min > cap) return fail(r, memory ? "memory size must be at most 65536 pages (4GiB)" : "table size too large");
    if (out->has_max && (out->max > cap || out->max < out->min)) return fail(r, "size minimum must not be greater than maximum");
    return true;
  }
  bool name(Reader& r, String* out) {
    const uint32_t n = r.u32();
    ConstByteView s = r.take(n);
    if (!r.ok()) return fail(r, "unexpected end");
    if (!valid_utf8(s)) return fail(r, "malformed UTF-8 encoding");
    out->assign(StringView(reinterpret_cast<const char*>(s.data()), s.size()));
    return true;
  }
  bool global_type(Reader& r, GlobalType* g) {
    if (!val_type(r, &g->type)) return false;
    const uint8_t mut = r.u8();
    if (!r.ok() || mut > 1) return fail(r, "malformed mutability");
    g->mut = mut == 1;
    return true;
  }
  bool const_expr(Reader& r, ValType expected, ConstExpr* out, uint32_t globals_visible);
  bool section_type(Reader& r);
  bool section_import(Reader& r);
  bool section_function(Reader& r);
  bool section_table(Reader& r);
  bool section_memory(Reader& r);
  bool section_tag(Reader& r);
  bool tag_type(Reader& r, uint32_t* type);
  bool section_global(Reader& r);
  bool section_export(Reader& r);
  bool section_start(Reader& r);
  bool section_element(Reader& r);
  bool section_code(Reader& r);
  bool section_data(Reader& r);
};

bool Decoder::const_expr(Reader& r, ValType expected, ConstExpr* out, uint32_t globals_visible) {
  List<uint8_t> stack(m_.allocator());
  for (;;) {
    const uint8_t op = r.u8();
    if (!r.ok()) return fail(r, "unexpected end");
    ConstOp c;
    uint8_t pops = 0;
    uint8_t pushes = 0;
    switch (op) {
      case 0x0B: {
        if (stack.count() != 1 || stack[0] != static_cast<uint8_t>(expected)) return fail(r, "type mismatch in constant expression");
        out->type = expected;
        return true;
      }
      case 0x41: c.op = ConstOpKind::I32Const; c.a = static_cast<uint32_t>(r.s32()); pushes = 0x7F; break;
      case 0x42: c.op = ConstOpKind::I64Const; c.a = static_cast<uint64_t>(r.s64()); pushes = 0x7E; break;
      case 0x43: c.op = ConstOpKind::F32Const; c.a = r.fixed32(); pushes = 0x7D; break;
      case 0x44: c.op = ConstOpKind::F64Const; c.a = r.fixed64(); pushes = 0x7C; break;
      case 0xFD: {
        if (r.u32() != 12) return fail(r, "constant expression required");
        ConstByteView b = r.take(16);
        if (!r.ok()) return fail(r, "unexpected end");
        for (uint32_t i = 0; i < 8; ++i) {
          c.a |= static_cast<uint64_t>(b[i]) << (8 * i);
          c.b |= static_cast<uint64_t>(b[8 + i]) << (8 * i);
        }
        c.op = ConstOpKind::V128Const;
        pushes = 0x7B;
        break;
      }
      case 0x23: {
        const uint32_t idx = r.u32();
        if (!r.ok()) return fail(r, "malformed global index");
        if (idx >= globals_visible || idx >= m_.globals.count()) return fail(r, "unknown global");
        if (m_.globals[idx].mut) return fail(r, "constant expression required");
        c.op = ConstOpKind::GlobalGet;
        c.a = idx;
        pushes = static_cast<uint8_t>(m_.globals[idx].type);
        break;
      }
      case 0xD0: {
        const uint8_t t = r.u8();
        if (!r.ok() || (t != 0x70 && t != 0x6F && t != 0x69)) return fail(r, "invalid reference type");
        c.op = ConstOpKind::RefNull;
        pushes = t;
        break;
      }
      case 0xD2: {
        const uint32_t f = r.u32();
        if (!r.ok()) return fail(r, "malformed function index");
        if (f >= m_.func_types.count()) return fail(r, "unknown function");
        if (f < declared_.count()) declared_[f] = 1;
        c.op = ConstOpKind::RefFunc;
        c.a = f;
        pushes = 0x70;
        break;
      }
      case 0x6A: c.op = ConstOpKind::I32Add; pops = 0x7F; pushes = 0x7F; break;
      case 0x6B: c.op = ConstOpKind::I32Sub; pops = 0x7F; pushes = 0x7F; break;
      case 0x6C: c.op = ConstOpKind::I32Mul; pops = 0x7F; pushes = 0x7F; break;
      case 0x7C: c.op = ConstOpKind::I64Add; pops = 0x7E; pushes = 0x7E; break;
      case 0x7D: c.op = ConstOpKind::I64Sub; pops = 0x7E; pushes = 0x7E; break;
      case 0x7E: c.op = ConstOpKind::I64Mul; pops = 0x7E; pushes = 0x7E; break;
      default: return fail(r, "constant expression required");
    }
    if (!r.ok()) return fail(r, "malformed constant");
    if (pops) {
      if (stack.count() < 2 || stack[stack.count() - 1] != pops || stack[stack.count() - 2] != pops) return fail(r, "type mismatch in constant expression");
      stack.remove_at(stack.count() - 1);
      stack.remove_at(stack.count() - 1);
    }
    if (!stack.push_back(pushes) || !out->ops.push_back(c)) return fail_msg("out of memory");
    if (stack.count() > 16) return fail(r, "constant expression too deep");
  }
}

bool Decoder::section_type(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxTypes) return fail(r, "malformed type section");
  for (uint32_t i = 0; i < n; ++i) {
    if (r.u8() != 0x60 || !r.ok()) return fail(r, "malformed function type");
    UniquePointer<FuncSig> sig = UniquePointer<FuncSig>::make(m_.allocator(), m_.allocator());
    if (!sig) return fail_msg("out of memory");
    List<uint8_t> types(m_.allocator());   // (the parameters, then the results: the counts are known only after both lists)
    const uint32_t np = r.u32();
    if (!r.ok()) return fail(r, "malformed function type");
    if (np > kMaxParams) return fail(r, "param count exceeds the internal limit of 1000");
    for (uint32_t k = 0; k < np; ++k) {
      ValType t = ValType::I32;
      if (!val_type(r, &t) || !types.push_back(static_cast<uint8_t>(t))) return false;
    }
    const uint32_t nr = r.u32();
    if (!r.ok()) return fail(r, "malformed function type");
    if (nr > kMaxResults) return fail(r, "return count exceeds the internal limit of 1000");
    for (uint32_t k = 0; k < nr; ++k) {
      ValType t = ValType::I32;
      if (!val_type(r, &t) || !types.push_back(static_cast<uint8_t>(t))) return false;
    }
    if (!sig->init(np, nr)) return fail_msg("out of memory");
    for (uint32_t k = 0; k < np; ++k) sig->params[k] = static_cast<ValType>(types[k]);
    for (uint32_t k = 0; k < nr; ++k) sig->results[k] = static_cast<ValType>(types[np + k]);
    if (!m_.types.push_back(util::move(sig))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_import(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed import section");
  for (uint32_t i = 0; i < n; ++i) {
    UniquePointer<Import> imp = UniquePointer<Import>::make(m_.allocator(), m_.allocator());
    if (!imp) return fail_msg("out of memory");
    if (!name(r, &imp->module) || !name(r, &imp->name)) return false;
    const uint8_t kind = r.u8();
    if (!r.ok() || kind > 4) return fail(r, "malformed import kind");
    imp->kind = static_cast<ExternKind>(kind);
    switch (imp->kind) {
      case ExternKind::Func: {
        imp->type_index = r.u32();
        if (!r.ok()) return fail(r, "malformed import");
        if (imp->type_index >= m_.types.count()) return fail(r, "unknown type");
        if (m_.func_types.count() >= kMaxFunctions || !m_.func_types.push_back(imp->type_index)) return fail_msg("too many functions");
        ++m_.imported_funcs;
        break;
      }
      case ExternKind::Table:
        if (!ref_type(r, &imp->table.elem) || !limits(r, &imp->table.limits, kMaxTableSize, false)) return false;
        if (!m_.tables.push_back(imp->table)) return fail_msg("out of memory");
        ++m_.imported_tables;
        break;
      case ExternKind::Memory:
        if (!limits(r, &imp->memory.limits, kMaxPages, true)) return false;
        if (!m_.memories.push_back(imp->memory)) return fail_msg("out of memory");
        ++m_.imported_memories;
        break;
      case ExternKind::Global:
        if (!global_type(r, &imp->global)) return false;
        if (!m_.globals.push_back(imp->global)) return fail_msg("out of memory");
        ++m_.imported_globals;
        break;
      case ExternKind::Tag:
        if (!tag_type(r, &imp->type_index) || !m_.tags.push_back(imp->type_index)) return false;
        ++m_.imported_tags;
        break;
    }
    if (!m_.imports.push_back(util::move(imp))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_function(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxFunctions) return fail(r, "malformed function section");
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t t = r.u32();
    if (!r.ok()) return fail(r, "malformed function section");
    if (t >= m_.types.count()) return fail(r, "unknown type");
    if (m_.func_types.count() >= kMaxFunctions || !m_.func_types.push_back(t)) return fail_msg("too many functions");
  }
  declared_funcs_ = n;
  return true;
}

bool Decoder::section_table(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed table section");
  for (uint32_t i = 0; i < n; ++i) {
    TableType t;
    if (!ref_type(r, &t.elem) || !limits(r, &t.limits, kMaxTableSize, false)) return false;
    if (!m_.tables.push_back(t)) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_memory(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed memory section");
  for (uint32_t i = 0; i < n; ++i) {
    MemType t;
    if (!limits(r, &t.limits, kMaxPages, true)) return false;
    if (!m_.memories.push_back(t)) return fail_msg("out of memory");
  }
  return true;
}

// The type of a tag: an attribute byte (always 0) and a function type with no results.
bool Decoder::tag_type(Reader& r, uint32_t* type) {
  if (r.u8() != 0 || !r.ok()) return fail(r, "malformed tag attribute");
  *type = r.u32();
  if (!r.ok()) return fail(r, "malformed tag type");
  if (*type >= m_.types.count()) return fail(r, "unknown type");
  if (m_.types[*type]->nresults != 0) return fail(r, "tag type must not have results");
  return true;
}

bool Decoder::section_tag(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed tag section");
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t type;
    if (!tag_type(r, &type) || !m_.tags.push_back(type)) return false;
  }
  return true;
}

bool Decoder::section_global(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed global section");
  for (uint32_t i = 0; i < n; ++i) {
    UniquePointer<GlobalDef> g = UniquePointer<GlobalDef>::make(m_.allocator(), m_.allocator());
    if (!g) return fail_msg("out of memory");
    if (!global_type(r, &g->type)) return false;
    // (a global's initializer may read the globals before it)
    if (!const_expr(r, g->type.type, &g->init, static_cast<uint32_t>(m_.globals.count()))) return false;
    if (!m_.globals.push_back(g->type) || !m_.global_defs.push_back(util::move(g))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_export(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed export section");
  for (uint32_t i = 0; i < n; ++i) {
    UniquePointer<Export> e = UniquePointer<Export>::make(m_.allocator(), m_.allocator());
    if (!e) return fail_msg("out of memory");
    if (!name(r, &e->name)) return false;
    const uint8_t kind = r.u8();
    e->index = r.u32();
    if (!r.ok() || kind > 4) return fail(r, "malformed export");
    e->kind = static_cast<ExternKind>(kind);
    uint64_t limit = 0;
    switch (e->kind) {
      case ExternKind::Func: limit = m_.func_types.count(); break;
      case ExternKind::Table: limit = m_.tables.count(); break;
      case ExternKind::Memory: limit = m_.memories.count(); break;
      case ExternKind::Global: limit = m_.globals.count(); break;
      case ExternKind::Tag: limit = m_.tags.count(); break;
    }
    if (e->index >= limit) return fail(r, "unknown export index");
    for (uint64_t k = 0; k < m_.exports.count(); ++k)
      if (m_.exports[k]->name.view().equals(e->name.view())) return fail(r, "duplicate export name");
    if (e->kind == ExternKind::Func && e->index < declared_.count()) declared_[e->index] = 1;
    if (!m_.exports.push_back(util::move(e))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_start(Reader& r) {
  m_.start = r.u32();
  if (!r.ok()) return fail(r, "malformed start section");
  if (m_.start >= m_.func_types.count()) return fail(r, "unknown function");
  const FuncSig& sig = *m_.types[m_.func_types[m_.start]];
  if (sig.nparams != 0 || sig.nresults != 0) return fail(r, "start function must have type [] -> []");
  m_.has_start = true;
  return true;
}

bool Decoder::section_element(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed element section");
  for (uint32_t i = 0; i < n; ++i) {
    UniquePointer<ElemSegment> seg = UniquePointer<ElemSegment>::make(m_.allocator(), m_.allocator());
    if (!seg) return fail_msg("out of memory");
    const uint32_t flags = r.u32();
    if (!r.ok() || flags > 7) return fail(r, "malformed element segment flags");
    const bool passive_or_decl = flags & 1;
    const bool explicit_table = flags & 2;
    const bool exprs = flags & 4;
    seg->mode = !passive_or_decl ? SegmentMode::Active : (explicit_table ? SegmentMode::Declarative : SegmentMode::Passive);
    if (seg->mode == SegmentMode::Active) {
      seg->table = explicit_table ? r.u32() : 0;
      if (!r.ok()) return fail(r, "malformed element segment");
      if (seg->table >= m_.tables.count()) return fail(r, "unknown table");
      if (!const_expr(r, ValType::I32, &seg->offset, static_cast<uint32_t>(m_.globals.count()))) return false;
    }
    seg->type = ValType::FuncRef;
    if (passive_or_decl || explicit_table) {
      if (exprs) {
        if (!ref_type(r, &seg->type)) return false;
      } else if (r.u8() != 0 || !r.ok()) {
        return fail(r, "malformed element kind");
      }
    }
    if (seg->mode == SegmentMode::Active && seg->type != m_.tables[seg->table].elem) return fail(r, "type mismatch: element segment and table");
    const uint32_t count = r.u32();
    if (!r.ok() || count > kMaxEntries) return fail(r, "malformed element segment");
    for (uint32_t k = 0; k < count; ++k) {
      UniquePointer<ConstExpr> e = UniquePointer<ConstExpr>::make(m_.allocator(), m_.allocator());
      if (!e) return fail_msg("out of memory");
      if (exprs) {
        if (!const_expr(r, seg->type, e.get(), static_cast<uint32_t>(m_.globals.count()))) return false;
      } else {
        const uint32_t f = r.u32();
        if (!r.ok()) return fail(r, "malformed function index");
        if (f >= m_.func_types.count()) return fail(r, "unknown function");
        if (f < declared_.count()) declared_[f] = 1;
        ConstOp c;
        c.op = ConstOpKind::RefFunc;
        c.a = f;
        e->ops.push_back(c);
        e->type = ValType::FuncRef;
      }
      if (!seg->items.push_back(util::move(e))) return fail_msg("out of memory");
    }
    if (!m_.elems.push_back(util::move(seg))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_code(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok()) return fail(r, "malformed code section");
  if (n != declared_funcs_) return fail(r, "function and code section have inconsistent lengths");
  saw_code_ = true;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t size = r.u32();
    if (!r.ok()) return fail(r, "malformed code section");
    ConstByteView body = r.take(size);
    if (!r.ok()) return fail(r, "unexpected end of section or function");
    Reader br(body, r.position() - size);
    UniquePointer<FuncCode> fc = UniquePointer<FuncCode>::make(m_.allocator(), m_.allocator());
    if (!fc) return fail_msg("out of memory");
    fc->type_index = m_.func_types[m_.imported_funcs + i];
    FuncCompiler compiler(m_, *fc, br, declared_, has_datacount_, datacount_, m_.allocator());
    const char* why = nullptr;
    if (!compiler.compile(&why)) {
      error_->assign("function ");
      error_->append_uint(m_.imported_funcs + i);
      error_->append(": ");
      error_->append(why ? why : "invalid code");
      error_->append(" @+");
      error_->append_uint(br.position());
      return false;
    }
    if (!m_.codes.push_back(util::move(fc))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::section_data(Reader& r) {
  const uint32_t n = r.u32();
  if (!r.ok() || n > kMaxEntries) return fail(r, "malformed data section");
  if (has_datacount_ && n != datacount_) return fail(r, "data count and data section have inconsistent lengths");
  for (uint32_t i = 0; i < n; ++i) {
    UniquePointer<DataSegment> seg = UniquePointer<DataSegment>::make(m_.allocator(), m_.allocator());
    if (!seg) return fail_msg("out of memory");
    const uint32_t flags = r.u32();
    if (!r.ok() || flags > 2) return fail(r, "malformed data segment flags");
    seg->mode = flags == 1 ? SegmentMode::Passive : SegmentMode::Active;
    if (seg->mode == SegmentMode::Active) {
      seg->memory = flags == 2 ? r.u32() : 0;
      if (!r.ok()) return fail(r, "malformed data segment");
      if (seg->memory >= m_.memories.count()) return fail(r, "unknown memory");
      if (!const_expr(r, ValType::I32, &seg->offset, static_cast<uint32_t>(m_.globals.count()))) return false;
    }
    const uint32_t size = r.u32();
    ConstByteView bytes = r.take(size);
    if (!r.ok()) return fail(r, "unexpected end of section or function");
    if (!seg->bytes.append(bytes)) return fail_msg("out of memory");
    if (!m_.datas.push_back(util::move(seg))) return fail_msg("out of memory");
  }
  return true;
}

bool Decoder::run() {
  Reader top(bytes_, 0);
  const uint32_t magic = top.fixed32();
  const uint32_t version = top.fixed32();
  if (!top.ok()) return fail_msg("expected magic word 00 61 73 6d");
  if (magic != 0x6D736100u) return fail_msg("expected magic word 00 61 73 6d");
  if (version != 1) return fail_msg("expected version 01 00 00 00");
  static const uint8_t kRank[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 11, 6};   // by section id; the data count section (12) comes between element (9) and code (10), the tag section (13) between memory (5) and global (6)
  int last_rank = 0;
  while (!top.at_end()) {
    const uint8_t id = top.u8();
    const uint32_t size = top.u32();
    if (!top.ok()) return fail(top, "malformed section header");
    if (size > top.remaining()) return fail(top, "unexpected end of section or function");
    const uint64_t start = top.position();
    ConstByteView body = top.take(size);
    Reader r(body, start);
    if (id == 0) {
      UniquePointer<CustomSection> c = UniquePointer<CustomSection>::make(m_.allocator(), m_.allocator());
      if (!c) return fail_msg("out of memory");
      if (!name(r, &c->name)) return false;
      c->bytes.append(ConstByteView(body.data() + (r.position() - start), body.size() - (r.position() - start)));
      m_.customs.push_back(util::move(c));
      continue;
    }
    if (id > 13) return fail(top, "malformed section id");
    if (kRank[id] <= last_rank) return fail(top, "unexpected content after last section or sections out of order");
    last_rank = kRank[id];
    // (the functions ref.func may name are marked by the sections that declare them: globals, exports, elements)
    if (id >= 6) {
      while (declared_.count() < m_.func_types.count()) declared_.push_back(0);
    }
    bool ok = true;
    switch (id) {
      case 1: ok = section_type(r); break;
      case 2: ok = section_import(r); break;
      case 3: ok = section_function(r); break;
      case 4: ok = section_table(r); break;
      case 5: ok = section_memory(r); break;
      case 6: ok = section_global(r); break;
      case 13: ok = section_tag(r); break;
      case 7: ok = section_export(r); break;
      case 8: ok = section_start(r); break;
      case 9: ok = section_element(r); break;
      case 10: ok = section_code(r); break;
      case 11: ok = section_data(r); break;
      case 12:
        datacount_ = r.u32();
        if (!r.ok()) return fail(r, "malformed data count section");
        has_datacount_ = true;
        break;
    }
    if (!ok) return false;
    if (!r.ok()) return fail(r, "unexpected end of section or function");
    if (!r.at_end()) return fail(r, "section size mismatch");
  }
  if (declared_funcs_ > 0 && !saw_code_) return fail_msg("function and code section have inconsistent lengths");
  if (has_datacount_ && m_.datas.count() != datacount_) return fail_msg("data count and data section have inconsistent lengths");
  return true;
}

}  // namespace

bool Module::parse(ConstByteView bytes, String* error) {
  Decoder d(*this, bytes, error);
  return d.run();
}

}  // namespace wasm
