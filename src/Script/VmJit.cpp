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
#include "Script/VmJit.hpp"
#include "Utility/AllocatedView.hpp"

namespace script {

namespace {

using jit::IrBuilder;
using jit::IrCond;
using jit::IrMem;
using jit::IrOp;

// The state block of a region is the VM's stack from the frame's base, 256 values: operands below, and two slots the entry fills in above anything the frame can hold
// (the stack above `sp_` is dead memory): the frame's `this` and its environment.
constexpr uint32_t kStateValues = 256, kThisSlot = 254, kEnvSlot = 255, kMaxHeight = 240;
constexpr uint32_t kMaxRegionInstrs = 2500;
constexpr uint32_t kMinCompiledOps = 3;    // a region that leaves after fewer instructions is not worth its entry
constexpr uint64_t kLoopBudget = 4096;     // back edges in compiled code before the instruction budget is charged
constexpr uint64_t kInstructionsPerBackEdge = 8;

static_assert(sizeof(Value) == 16, "compiled code addresses a Value as two 8 byte words");

// Where things are in a Value, an Env and a JsObject, found from real objects (the layout is the compiler's, and this does not assume it).
struct Layout {
  uint32_t value_payload;
  uint32_t env_parent, env_slots;
  uint32_t obj_kind, obj_host, obj_env, obj_length, obj_items, obj_frozen;
  uint64_t uninitialized_bits;
  Layout() {
    Value v;
    value_payload = static_cast<uint32_t>(reinterpret_cast<const char*>(&v.u) - reinterpret_cast<const char*>(&v));
    Env e;
    env_parent = static_cast<uint32_t>(reinterpret_cast<const char*>(&e.parent) - reinterpret_cast<const char*>(&e));
    env_slots = sizeof(Env);   // (Env::slots() is the memory right after the header)
    JsObject o;
    auto at = [&](const void* field) { return static_cast<uint32_t>(reinterpret_cast<const char*>(field) - reinterpret_cast<const char*>(&o)); };
    obj_kind = at(&o.okind);
    obj_host = at(&o.host);
    obj_env = at(&o.env);
    obj_length = at(&o.length);
    obj_items = at(&o.items);
    obj_frozen = at(&o.frozen);
    Value u = Value::make_uninitialized();
    __builtin_memcpy(&uninitialized_bits, reinterpret_cast<const char*>(&u) + value_payload, 8);
  }
};

constexpr uint64_t bits_of(double d) { return __builtin_bit_cast(uint64_t, d); }

struct Pending {
  uint32_t pc, height;
};
struct Stub {
  uint32_t label, pc, height;
  uint64_t flags;
};

class Translator {
 public:
  Translator(allocators::IAllocator& allocator, IrBuilder& b, Vm& vm, const FunctionCode& code, uint32_t entry_pc, uint32_t entry_height)
      : allocator_(allocator), b_(b), vm_(vm), code_(code), instrs_(code.code.view()), entry_pc_(entry_pc), entry_height_(entry_height), work_(allocator), stubs_(allocator) {}
  ~Translator() {
    release_view(allocator_, heights_);
    release_view(allocator_, labels_);
    release_view(allocator_, state_);
    release_view(allocator_, target_);
  }
  bool run();

 private:
  enum class Flow { Next, Stop, Done };
  Flow fail() {
    failed_ = true;
    return Flow::Stop;
  }
  uint32_t c(uint64_t v) { return b_.constant(static_cast<int64_t>(v)); }
  uint64_t tag_word(Tag t) const { return static_cast<uint64_t>(t); }
  static uint64_t off(uint32_t k) { return uint64_t{16} * k; }
  uint32_t load_tag(uint32_t k) { return b_.binary(IrOp::And, b_.load_state(off(k)), c(0xFF)); }
  uint32_t load_payload(uint32_t k) { return b_.load_state(off(k) + l_.value_payload); }
  void store_value(uint32_t k, uint32_t tag, uint32_t payload) {
    b_.store_state(off(k), tag);
    b_.store_state(off(k) + l_.value_payload, payload);
  }
  void store_constant(uint32_t k, Tag tag, uint64_t payload) { store_value(k, c(tag_word(tag)), c(payload)); }
  void copy_slot(uint32_t to, uint32_t from) {
    b_.store_state(off(to), b_.load_state(off(from)));
    b_.store_state(off(to) + l_.value_payload, b_.load_state(off(from) + l_.value_payload));
  }
  uint64_t pack(uint32_t h, uint32_t pc) const { return uint64_t{h} | (uint64_t{pc} << 32); }
  uint32_t call_helper(jit::IrHelper fn, uint32_t h, uint32_t pc) { return b_.helper(fn, c(reinterpret_cast<uintptr_t>(&vm_)), c(pack(h, pc))); }
  // A helper that can fail: the region leaves with the exception pending when it returns non-zero.
  void call_checked(jit::IrHelper fn, uint32_t h, uint32_t pc) {
    const uint32_t r = call_helper(fn, h, pc);
    b_.branch_if(IrCond::Ne, r, c(0), stub(pc + 1, h, kScriptExitException));
  }
  uint32_t stub(uint32_t pc, uint32_t height, uint64_t flags) {
    const uint32_t label = b_.new_label();
    if (!stubs_.push_back(Stub{label, pc, height, flags})) failed_ = true;
    return label;
  }
  void emit_exit(uint32_t pc, uint32_t height, uint64_t flags) {
    b_.exit(static_cast<int64_t>(uint64_t{pc} | (uint64_t{height} << 32) | flags));
  }
  uint32_t request(uint32_t pc, uint32_t height);
  bool scan();
  bool emit_run(uint32_t pc, uint32_t height);
  Flow translate_one(const Instr& in, uint32_t pc, uint32_t& h);
  uint32_t env_for(uint32_t depth);
  uint32_t truthiness(uint32_t k, uint32_t h, uint32_t pc);
  bool emit_jump(uint32_t target, uint32_t height, uint32_t here);
  void binary(const Instr& in, uint32_t pc, uint32_t& h);
  void get_elem(uint32_t pc, uint32_t& h);
  void set_elem(uint32_t pc, uint32_t& h);
  uint32_t to_int32(uint32_t number_bits, uint32_t slow);

  allocators::IAllocator& allocator_;
  IrBuilder& b_;
  Vm& vm_;
  const FunctionCode& code_;
  ArrayView<const Instr> instrs_;
  uint32_t entry_pc_, entry_height_;
  ArrayView<int32_t> heights_;
  ArrayView<uint32_t> labels_;
  ArrayView<uint8_t> state_;   // bit 0 emitted, bit 1 queued
  ArrayView<uint8_t> target_;
  List<Pending> work_;
  List<Stub> stubs_;
  uint32_t env0_ = 0, counter_ = 0;
  uint32_t translated_ = 0, compiled_ops_ = 0;
  uint32_t max_height_ = 0;
  bool failed_ = false;
  const Layout l_;   // (built here, not in a function static: the kernel has no thread safe static initialisation)
};

bool Translator::scan() {
  target_ = allocate_view<uint8_t>(allocator_, instrs_.size() + 1);
  if (target_.empty()) return false;
  target_.fill(0);
  for (const Instr& in : instrs_) {
    switch (in.op) {
      case Opc::Jump: case Opc::JumpIfFalse: case Opc::JumpIfTrue: case Opc::JumpIfFalseKeep: case Opc::JumpIfTrueKeep: case Opc::JumpIfNotNullishKeep:
        if (in.a >= 0 && static_cast<uint64_t>(in.a) < instrs_.size()) target_[static_cast<uint64_t>(in.a)] = 1;
        break;
      default: break;
    }
  }
  return true;
}

uint32_t Translator::request(uint32_t pc, uint32_t height) {
  if (pc >= instrs_.size() || height > kMaxHeight) {
    failed_ = true;
    return 0;
  }
  if (heights_[pc] >= 0 && heights_[pc] != static_cast<int32_t>(height)) {   // (the compiler gives every instruction one height; if some path does not, leave the function alone)
    failed_ = true;
    return 0;
  }
  heights_[pc] = static_cast<int32_t>(height);
  if (height > max_height_) max_height_ = height;
  if (!labels_[pc]) labels_[pc] = b_.new_label() + 1;
  if (!(state_[pc] & 3)) {
    state_[pc] |= 2;
    if (!work_.push_back(Pending{pc, height})) failed_ = true;
  }
  return labels_[pc] - 1;
}

// The environment `depth` functions out.
uint32_t Translator::env_for(uint32_t depth) {
  uint32_t e = env0_;
  for (uint32_t d = 0; d < depth; ++d) e = b_.load_ptr(IrMem::U64, e, l_.env_parent);
  return e;
}

// 1 or 0: is the value in slot k true? Booleans, numbers, undefined and null are decided inline; the rest by a helper (strings, objects).
uint32_t Translator::truthiness(uint32_t k, uint32_t h, uint32_t pc) {
  const uint32_t result = b_.new_vreg();
  const uint32_t done = b_.new_label(), not_bool = b_.new_label(), not_number = b_.new_label(), other = b_.new_label();
  const uint32_t tag = load_tag(k);
  b_.branch_if(IrCond::Ne, tag, c(tag_word(Tag::Boolean)), not_bool);
  b_.assign(result, b_.binary(IrOp::And, load_payload(k), c(0xFF)));
  b_.jump(done);
  b_.place_label(not_bool);
  b_.branch_if(IrCond::Ne, tag, c(tag_word(Tag::Number)), not_number);
  {
    const uint32_t x = load_payload(k);
    b_.assign(result, b_.binary(IrOp::And, b_.fsetcond(IrCond::Eq, x, x), b_.fsetcond(IrCond::Ne, x, c(0))));   // (not a NaN and not 0 or -0)
  }
  b_.jump(done);
  b_.place_label(not_number);
  b_.branch_if(IrCond::Geu, tag, c(tag_word(Tag::String)), other);
  b_.assign(result, c(0));   // undefined, null
  b_.jump(done);
  b_.place_label(other);
  b_.assign(result, call_helper(&VmJit::helper_truthy, h, pc));
  b_.place_label(done);
  return result;
}

// A jump to `target` with `height` operands: a backward one counts toward the budget.
bool Translator::emit_jump(uint32_t target, uint32_t height, uint32_t here) {
  const uint32_t label = request(target, height);
  if (failed_) return false;
  if (target <= here) {
    b_.emit(IrOp::Add, IrCond::Eq, counter_, counter_, c(1), 0);
    b_.branch_if(IrCond::Geu, counter_, c(kLoopBudget), stub(target, height, kScriptExitTick));
  }
  b_.jump(label);
  return true;
}

// The int32 of a number (the bits of a double): the value sign extended to 64 bits. Anything that does not fit in 63 bits (and a NaN) is left to the slow path.
uint32_t Translator::to_int32(uint32_t number_bits, uint32_t slow) {
  const uint32_t i = b_.unary(IrOp::F2I, number_bits);
  b_.branch_if(IrCond::Eq, i, c(static_cast<uint64_t>(INT64_MIN)), slow);
  const uint32_t sh = c(32);
  return b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, i, sh), sh);
}

void Translator::binary(const Instr& in, uint32_t pc, uint32_t& h) {
  const uint32_t a = h - 2, bb = h - 1;
  const uint32_t slow = b_.new_label(), done = b_.new_label();
  const bool inline_ok = in.op != Opc::Mod && in.op != Opc::Pow && in.op != Opc::In && in.op != Opc::InstanceOf;
  if (inline_ok) {
    b_.branch_if(IrCond::Ne, load_tag(a), c(tag_word(Tag::Number)), slow);
    b_.branch_if(IrCond::Ne, load_tag(bb), c(tag_word(Tag::Number)), slow);
    const uint32_t x = load_payload(a), y = load_payload(bb);
    auto number = [&](uint32_t bits) { store_value(a, c(tag_word(Tag::Number)), bits); };
    auto boolean = [&](uint32_t v) { store_value(a, c(tag_word(Tag::Boolean)), v); };
    switch (in.op) {
      case Opc::Add: number(b_.binary(IrOp::FAdd, x, y)); break;
      case Opc::Sub: number(b_.binary(IrOp::FSub, x, y)); break;
      case Opc::Mul: number(b_.binary(IrOp::FMul, x, y)); break;
      case Opc::Div: number(b_.binary(IrOp::FDiv, x, y)); break;
      case Opc::Lt: boolean(b_.fsetcond(IrCond::Lt, x, y)); break;
      case Opc::Gt: boolean(b_.fsetcond(IrCond::Gt, x, y)); break;
      case Opc::Le: boolean(b_.fsetcond(IrCond::Le, x, y)); break;
      case Opc::Ge: boolean(b_.fsetcond(IrCond::Ge, x, y)); break;
      case Opc::Eq: case Opc::StrictEq: boolean(b_.fsetcond(IrCond::Eq, x, y)); break;
      case Opc::Ne: case Opc::StrictNe: boolean(b_.fsetcond(IrCond::Ne, x, y)); break;
      default: {   // & | ^ << >> >>>
        const uint32_t xi = to_int32(x, slow), yi = to_int32(y, slow);
        const uint32_t shift = b_.binary(IrOp::And, yi, c(31));
        uint32_t r;
        switch (in.op) {
          case Opc::BitAnd: r = b_.binary(IrOp::And, xi, yi); break;
          case Opc::BitOr: r = b_.binary(IrOp::Or, xi, yi); break;
          case Opc::BitXor: r = b_.binary(IrOp::Xor, xi, yi); break;
          case Opc::Shl: {
            const uint32_t sh = c(32);
            r = b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, b_.binary(IrOp::Shl, xi, shift), sh), sh);
            break;
          }
          case Opc::Shr: r = b_.binary(IrOp::Sar, xi, shift); break;
          default: r = b_.binary(IrOp::Shr, b_.binary(IrOp::And, xi, c(0xFFFFFFFFull)), shift); break;   // >>>
        }
        number(b_.unary(IrOp::I2F, r));
        break;
      }
    }
    b_.jump(done);
  }
  b_.place_label(slow);
  call_checked(&VmJit::helper_binary, h, pc);
  b_.place_label(done);
  --h;
}

void Translator::get_elem(uint32_t pc, uint32_t& h) {
  const Layout& l = l_;
  const uint32_t obj = h - 2, key = h - 1;
  const uint32_t slow = b_.new_label(), done = b_.new_label(), filled = b_.new_label();
  b_.branch_if(IrCond::Ne, load_tag(obj), c(tag_word(Tag::Object)), slow);
  b_.branch_if(IrCond::Ne, load_tag(key), c(tag_word(Tag::Number)), slow);
  const uint32_t o = load_payload(obj);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U8, o, l.obj_kind), c(static_cast<uint64_t>(ObjKind::Array)), slow);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U64, o, l.obj_host), c(0), slow);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U64, o, l.obj_env), c(0), slow);
  const uint32_t x = load_payload(key);
  const uint32_t idx = b_.unary(IrOp::F2I, x);
  b_.branch_if(IrCond::Eq, b_.fsetcond(IrCond::Eq, b_.unary(IrOp::I2F, idx), x), c(0), slow);   // (not a whole number, or a NaN)
  b_.branch_if(IrCond::Geu, idx, b_.load_ptr(IrMem::U32, o, l.obj_length), slow);              // (negative is a huge unsigned number)
  const uint32_t element = b_.binary(IrOp::Add, b_.load_ptr(IrMem::U64, o, l.obj_items), b_.binary(IrOp::Shl, idx, c(4)));
  const uint32_t t = b_.load_ptr(IrMem::U64, element, 0), p = b_.load_ptr(IrMem::U64, element, l.value_payload);
  b_.branch_if(IrCond::Ne, b_.binary(IrOp::And, t, c(0xFF)), c(0), filled);
  b_.branch_if(IrCond::Eq, p, c(l.uninitialized_bits), slow);   // (an undefined that carries the hole mark: the helper knows what a hole reads as)
  b_.place_label(filled);
  store_value(obj, t, p);
  b_.jump(done);
  b_.place_label(slow);
  call_checked(&VmJit::helper_get_elem, h, pc);
  b_.place_label(done);
  --h;
}

void Translator::set_elem(uint32_t pc, uint32_t& h) {
  const Layout& l = l_;
  const uint32_t obj = h - 3, key = h - 2, value = h - 1;
  const uint32_t slow = b_.new_label(), done = b_.new_label(), present = b_.new_label();
  b_.branch_if(IrCond::Ne, load_tag(obj), c(tag_word(Tag::Object)), slow);
  b_.branch_if(IrCond::Ne, load_tag(key), c(tag_word(Tag::Number)), slow);
  const uint32_t o = load_payload(obj);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U8, o, l.obj_kind), c(static_cast<uint64_t>(ObjKind::Array)), slow);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U64, o, l.obj_host), c(0), slow);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U64, o, l.obj_env), c(0), slow);
  b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U8, o, l.obj_frozen), c(0), slow);
  const uint32_t x = load_payload(key);
  const uint32_t idx = b_.unary(IrOp::F2I, x);
  b_.branch_if(IrCond::Eq, b_.fsetcond(IrCond::Eq, b_.unary(IrOp::I2F, idx), x), c(0), slow);
  b_.branch_if(IrCond::Geu, idx, b_.load_ptr(IrMem::U32, o, l.obj_length), slow);
  const uint32_t element = b_.binary(IrOp::Add, b_.load_ptr(IrMem::U64, o, l.obj_items), b_.binary(IrOp::Shl, idx, c(4)));
  b_.branch_if(IrCond::Ne, b_.binary(IrOp::And, b_.load_ptr(IrMem::U64, element, 0), c(0xFF)), c(0), present);
  b_.branch_if(IrCond::Eq, b_.load_ptr(IrMem::U64, element, l.value_payload), c(l.uninitialized_bits), slow);   // (writing over a hole is the helper's business)
  b_.place_label(present);
  b_.store_ptr(IrMem::U64, element, 0, b_.load_state(off(value)));
  b_.store_ptr(IrMem::U64, element, l.value_payload, b_.load_state(off(value) + l.value_payload));
  copy_slot(obj, value);
  b_.jump(done);
  b_.place_label(slow);
  call_checked(&VmJit::helper_set_elem, h, pc);
  b_.place_label(done);
  h -= 2;
}

Translator::Flow Translator::translate_one(const Instr& in, uint32_t pc, uint32_t& h) {
  auto need = [&](uint32_t n) { return h >= n; };
  auto room = [&]() { return h < kMaxHeight; };
  switch (in.op) {
    case Opc::PushUndef: if (!room()) return fail(); store_constant(h++, Tag::Undefined, 0); return Flow::Next;
    case Opc::PushNull: if (!room()) return fail(); store_constant(h++, Tag::Null, 0); return Flow::Next;
    case Opc::PushTrue: if (!room()) return fail(); store_constant(h++, Tag::Boolean, 1); return Flow::Next;
    case Opc::PushFalse: if (!room()) return fail(); store_constant(h++, Tag::Boolean, 0); return Flow::Next;
    case Opc::PushInt: if (!room()) return fail(); store_constant(h++, Tag::Number, bits_of(static_cast<double>(in.a))); return Flow::Next;
    case Opc::PushNum: case Opc::PushStr: {
      if (!room()) return fail();
      if (in.a < 0 || static_cast<uint64_t>(in.a) >= code_.constants.count()) return fail();
      const Value& v = code_.constants[static_cast<uint32_t>(in.a)];
      uint64_t payload = 0;
      __builtin_memcpy(&payload, reinterpret_cast<const char*>(&v) + l_.value_payload, 8);   // (a constant is a number or an interned string: rooted by the code)
      store_constant(h++, v.tag, payload);
      return Flow::Next;
    }
    case Opc::PushThis:
      if (!room()) return fail();
      copy_slot(h, kThisSlot);
      ++h;
      return Flow::Next;
    case Opc::Pop: if (!need(1)) return fail(); --h; return Flow::Next;
    case Opc::Dup: if (!need(1) || !room()) return fail(); copy_slot(h, h - 1); ++h; return Flow::Next;
    case Opc::Dup2:
      if (!need(2) || h + 2 > kMaxHeight) return fail();
      copy_slot(h, h - 2);
      copy_slot(h + 1, h - 1);
      h += 2;
      return Flow::Next;
    case Opc::DupX1:   // [a, b] -> [b, a, b]
      if (!need(2) || !room()) return fail();
      copy_slot(h, h - 1);
      copy_slot(h - 1, h - 2);
      copy_slot(h - 2, h);
      ++h;
      return Flow::Next;
    case Opc::DupX2:   // [a, b, c] -> [c, a, b, c]
      if (!need(3) || !room()) return fail();
      copy_slot(h, h - 1);
      copy_slot(h - 1, h - 2);
      copy_slot(h - 2, h - 3);
      copy_slot(h - 3, h);
      ++h;
      return Flow::Next;
    case Opc::Swap: {
      if (!need(2)) return fail();
      const uint32_t t0 = b_.load_state(off(h - 2)), p0 = b_.load_state(off(h - 2) + l_.value_payload);
      copy_slot(h - 2, h - 1);
      b_.store_state(off(h - 1), t0);
      b_.store_state(off(h - 1) + l_.value_payload, p0);
      return Flow::Next;
    }

    case Opc::GetVar: {
      if (!room()) return fail();
      const uint32_t e = env_for(in.b);
      const uint64_t slot = l_.env_slots + uint64_t{16} * static_cast<uint32_t>(in.a);
      const uint32_t t = b_.load_ptr(IrMem::U64, e, static_cast<int64_t>(slot)), p = b_.load_ptr(IrMem::U64, e, static_cast<int64_t>(slot + l_.value_payload));
      if (in.unused >= kChecked) {   // let / const / class: in its temporal dead zone the interpreter throws
        const uint32_t ok = b_.new_label();
        b_.branch_if(IrCond::Ne, b_.binary(IrOp::And, t, c(0xFF)), c(0), ok);
        b_.branch_if(IrCond::Eq, p, c(l_.uninitialized_bits), stub(pc, h, 0));
        b_.place_label(ok);
      }
      store_value(h, t, p);
      ++h;
      return Flow::Next;
    }
    case Opc::SetVar: {
      if (!need(1)) return fail();
      if (in.unused == kConstAssign) return Flow::Stop;
      const uint32_t e = env_for(in.b);
      const uint64_t slot = l_.env_slots + uint64_t{16} * static_cast<uint32_t>(in.a);
      if (in.unused != 0) {
        const uint32_t ok = b_.new_label();
        const uint32_t t0 = b_.load_ptr(IrMem::U64, e, static_cast<int64_t>(slot)), p0 = b_.load_ptr(IrMem::U64, e, static_cast<int64_t>(slot + l_.value_payload));
        b_.branch_if(IrCond::Ne, b_.binary(IrOp::And, t0, c(0xFF)), c(0), ok);
        b_.branch_if(IrCond::Eq, p0, c(l_.uninitialized_bits), stub(pc, h, 0));
        b_.place_label(ok);
      }
      b_.store_ptr(IrMem::U64, e, static_cast<int64_t>(slot), b_.load_state(off(h - 1)));
      b_.store_ptr(IrMem::U64, e, static_cast<int64_t>(slot + l_.value_payload), b_.load_state(off(h - 1) + l_.value_payload));
      return Flow::Next;
    }
    case Opc::TdzMark: {
      const uint32_t e = env_for(in.b);
      const uint64_t slot = l_.env_slots + uint64_t{16} * static_cast<uint32_t>(in.a);
      b_.store_ptr(IrMem::U64, e, static_cast<int64_t>(slot), c(0));
      b_.store_ptr(IrMem::U64, e, static_cast<int64_t>(slot + l_.value_payload), c(l_.uninitialized_bits));
      return Flow::Next;
    }

    case Opc::GetGlobal:
      if (!room()) return fail();
      call_checked(&VmJit::helper_get_global, h, pc);
      ++h;
      return Flow::Next;
    case Opc::GetProp: {
      if (!need(1)) return fail();
      const Value& name = code_.constants[static_cast<uint32_t>(in.a)];
      if (name.is_string() && name.as_string()->view().equals("length")) {   // array.length inline
        const Layout& l = l_;
        const uint32_t slow = b_.new_label(), done = b_.new_label();
        b_.branch_if(IrCond::Ne, load_tag(h - 1), c(tag_word(Tag::Object)), slow);
        const uint32_t o = load_payload(h - 1);
        b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U8, o, l.obj_kind), c(static_cast<uint64_t>(ObjKind::Array)), slow);
        b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U64, o, l.obj_host), c(0), slow);
        b_.branch_if(IrCond::Ne, b_.load_ptr(IrMem::U64, o, l.obj_env), c(0), slow);
        store_value(h - 1, c(tag_word(Tag::Number)), b_.unary(IrOp::I2F, b_.load_ptr(IrMem::U32, o, l.obj_length)));
        b_.jump(done);
        b_.place_label(slow);
        call_checked(&VmJit::helper_get_prop, h, pc);
        b_.place_label(done);
        return Flow::Next;
      }
      call_checked(&VmJit::helper_get_prop, h, pc);
      return Flow::Next;
    }
    case Opc::SetProp:
      if (!need(2)) return fail();
      call_checked(&VmJit::helper_set_prop, h, pc);
      --h;
      return Flow::Next;
    case Opc::GetElem: if (!need(2)) return fail(); get_elem(pc, h); return Flow::Next;
    case Opc::SetElem: if (!need(3)) return fail(); set_elem(pc, h); return Flow::Next;
    case Opc::GetMethod:
      if (!need(1) || !room()) return fail();
      call_checked(&VmJit::helper_get_method, h, pc);
      ++h;
      return Flow::Next;
    case Opc::GetMethodElem:
      if (!need(2)) return fail();
      call_checked(&VmJit::helper_get_method_elem, h, pc);
      return Flow::Next;

    case Opc::Add: case Opc::Sub: case Opc::Mul: case Opc::Div: case Opc::Mod: case Opc::Pow: case Opc::BitAnd: case Opc::BitOr: case Opc::BitXor:
    case Opc::Shl: case Opc::Shr: case Opc::UShr: case Opc::Eq: case Opc::Ne: case Opc::StrictEq: case Opc::StrictNe: case Opc::Lt: case Opc::Gt:
    case Opc::Le: case Opc::Ge: case Opc::In: case Opc::InstanceOf:
      if (!need(2)) return fail();
      binary(in, pc, h);
      return Flow::Next;

    case Opc::Not: {
      if (!need(1)) return fail();
      const uint32_t t = truthiness(h - 1, h, pc);
      store_value(h - 1, c(tag_word(Tag::Boolean)), b_.binary(IrOp::Xor, t, c(1)));
      return Flow::Next;
    }
    case Opc::Neg: case Opc::Plus: case Opc::ToNumber: case Opc::BitNot: case Opc::Inc: case Opc::Dec: {
      if (!need(1)) return fail();
      const uint32_t slow = b_.new_label(), done = b_.new_label();
      b_.branch_if(IrCond::Ne, load_tag(h - 1), c(tag_word(Tag::Number)), slow);
      const uint32_t x = load_payload(h - 1);
      switch (in.op) {
        case Opc::Neg: store_value(h - 1, c(tag_word(Tag::Number)), b_.binary(IrOp::Xor, x, c(0x8000000000000000ull))); break;
        case Opc::Plus: case Opc::ToNumber: break;   // (a number stays as it is)
        case Opc::BitNot: store_value(h - 1, c(tag_word(Tag::Number)), b_.unary(IrOp::I2F, b_.unary(IrOp::Not, to_int32(x, slow)))); break;
        case Opc::Inc: store_value(h - 1, c(tag_word(Tag::Number)), b_.binary(IrOp::FAdd, x, c(bits_of(1.0)))); break;
        default: store_value(h - 1, c(tag_word(Tag::Number)), b_.binary(IrOp::FSub, x, c(bits_of(1.0)))); break;
      }
      b_.jump(done);
      b_.place_label(slow);
      call_checked(&VmJit::helper_unary, h, pc);
      b_.place_label(done);
      return Flow::Next;
    }
    case Opc::Void: if (!need(1)) return fail(); store_constant(h - 1, Tag::Undefined, 0); return Flow::Next;
    case Opc::ToStr: case Opc::Typeof:
      if (!need(1)) return fail();
      call_checked(&VmJit::helper_unary, h, pc);
      return Flow::Next;

    case Opc::Jump:
      if (!emit_jump(static_cast<uint32_t>(in.a), h, pc)) return fail();
      return Flow::Done;
    case Opc::JumpIfFalse: case Opc::JumpIfTrue: {
      if (!need(1)) return fail();
      const uint32_t t = truthiness(h - 1, h, pc);
      --h;
      const uint32_t skip = b_.new_label();
      b_.branch_if(in.op == Opc::JumpIfFalse ? IrCond::Ne : IrCond::Eq, t, c(0), skip);
      if (!emit_jump(static_cast<uint32_t>(in.a), h, pc)) return fail();
      b_.place_label(skip);
      return Flow::Next;
    }
    case Opc::JumpIfFalseKeep: case Opc::JumpIfTrueKeep: {   // the value stays when it jumps and is dropped when it does not
      if (!need(1)) return fail();
      const uint32_t t = truthiness(h - 1, h, pc);
      const uint32_t skip = b_.new_label();
      b_.branch_if(in.op == Opc::JumpIfFalseKeep ? IrCond::Ne : IrCond::Eq, t, c(0), skip);
      if (!emit_jump(static_cast<uint32_t>(in.a), h, pc)) return fail();
      b_.place_label(skip);
      --h;
      return Flow::Next;
    }
    case Opc::JumpIfNotNullishKeep: {
      if (!need(1)) return fail();
      const uint32_t skip = b_.new_label();
      b_.branch_if(IrCond::Leu, load_tag(h - 1), c(tag_word(Tag::Null)), skip);   // (undefined or null: fall through, dropped)
      if (!emit_jump(static_cast<uint32_t>(in.a), h, pc)) return fail();
      b_.place_label(skip);
      --h;
      return Flow::Next;
    }
    default: break;
  }
  return Flow::Stop;
}

bool Translator::emit_run(uint32_t pc, uint32_t height) {
  uint32_t h = height;
  for (;;) {
    if (pc >= instrs_.size()) return false;
    if (state_[pc] & 1) {   // already translated: a join, which is a jump target with a label
      if (heights_[pc] != static_cast<int32_t>(h) || !labels_[pc]) return false;
      b_.jump(labels_[pc] - 1);
      return true;
    }
    if (heights_[pc] >= 0 && heights_[pc] != static_cast<int32_t>(h)) return false;
    heights_[pc] = static_cast<int32_t>(h);
    if (h > max_height_) max_height_ = h;
    if (target_[pc] || labels_[pc]) {
      if (!labels_[pc]) labels_[pc] = b_.new_label() + 1;
      b_.place_label(labels_[pc] - 1);
    }
    state_[pc] |= 1;
    if (++translated_ > kMaxRegionInstrs) {
      emit_exit(pc, h, 0);
      return true;
    }
    const uint32_t before = h;
    const Flow flow = translate_one(instrs_[pc], pc, h);
    if (failed_) return false;
    if (flow == Flow::Stop) {
      emit_exit(pc, before, 0);
      return true;
    }
    ++compiled_ops_;
    if (flow == Flow::Done) return true;
    ++pc;
  }
}

bool Translator::run() {
  const uint64_t n = instrs_.size();
  if (n == 0 || entry_pc_ >= n || entry_height_ > kMaxHeight) return false;
  heights_ = allocate_view<int32_t>(allocator_, n + 1);
  labels_ = allocate_view<uint32_t>(allocator_, n + 1);
  state_ = allocate_view<uint8_t>(allocator_, n + 1);
  if (heights_.empty() || labels_.empty() || state_.empty()) return false;
  heights_.fill(-1);
  labels_.fill(0);
  state_.fill(0);
  if (!scan()) return false;
  b_.set_state_size(uint64_t{16} * kStateValues);
  counter_ = b_.new_vreg();
  env0_ = b_.load_state(off(kEnvSlot) + l_.value_payload);
  request(entry_pc_, entry_height_);
  while (!failed_ && work_.count()) {
    const Pending p = work_[work_.count() - 1];
    work_.remove_at(work_.count() - 1);
    if (state_[p.pc] & 1) continue;
    if (!emit_run(p.pc, p.height)) return false;
  }
  if (failed_) return false;
  if (compiled_ops_ < kMinCompiledOps) return false;   // (it would leave almost at once: the interpreter does better)
  for (uint64_t i = 0; i < stubs_.count(); ++i) {
    const Stub s = stubs_[i];
    b_.place_label(s.label);
    emit_exit(s.pc, s.height, s.flags);
  }
  return !b_.failed();
}

// --- helpers --------------------------------------------------------------------------------------------------------------------------

struct Call {
  Vm& vm;
  Value* stack;   // the frame's operands: stack[0] is the slot at the frame's base
  uint32_t h;
  const Instr& in;
  const FunctionCode* code;
};

}  // namespace

// What a helper needs: the VM, the stack at the frame's base, the height, and the instruction. The VM's own stack pointer is set to the height first, so that
// whatever the helper runs (a getter, a valueOf) keeps the operands alive.
#define JIT_CALL(call)                                                                                      \
  Vm& vm_ref = *reinterpret_cast<Vm*>(static_cast<uintptr_t>(vm));                                         \
  Vm::Frame& frame_ref = vm_ref.frames_[vm_ref.frames_.count() - 1];                                       \
  const uint32_t h_ = static_cast<uint32_t>(packed);                                                        \
  Value* const stk = static_cast<Value*>(state);                                                            \
  const Instr& in_ = frame_ref.code->code[static_cast<uint32_t>(packed >> 32)];                            \
  vm_ref.sp_ = frame_ref.base + h_;                                                                         \
  Call call{vm_ref, stk, h_, in_, frame_ref.code};

uint64_t VmJit::helper_binary(void* state, uint64_t vm, uint64_t packed) {
  JIT_CALL(call)
  Value r;
  const Value a = call.stack[call.h - 2], b = call.stack[call.h - 1];
  if (!call.vm.binary_op(call.in.op, a, b, &r)) {
    if (!call.vm.has_exception_) call.vm.throw_error(ErrorKind::Error, "operation failed");
    return 1;
  }
  call.stack[call.h - 2] = r;
  return 0;
}

uint64_t VmJit::helper_unary(void* state, uint64_t vm, uint64_t packed) {
  JIT_CALL(call)
  Vm& v = call.vm;
  Value& top = call.stack[call.h - 1];
  switch (call.in.op) {
    case Opc::Neg: {
      if (Vm::is_bigint(top)) {
        Value r;
        if (!v.bigint_operation("neg", top, Value(), &r)) return 1;
        top = r;
        return 0;
      }
      double d;
      if (!v.to_number(top, &d)) return 1;
      top = Value::make_number(-d);
      return 0;
    }
    case Opc::Plus: case Opc::ToNumber: {
      if (Vm::is_bigint(top)) {
        if (call.in.op == Opc::Plus) {
          v.throw_error(ErrorKind::TypeError, "Cannot convert a BigInt value to a number");
          return 1;
        }
        return 0;
      }
      double d;
      if (!v.to_number(top, &d)) return 1;
      top = Value::make_number(d);
      return 0;
    }
    case Opc::BitNot: {
      if (Vm::is_bigint(top)) {
        Value r;
        if (!v.bigint_operation("~", top, Value(), &r)) return 1;
        top = r;
        return 0;
      }
      int32_t i;
      if (!v.to_int32(top, &i)) return 1;
      top = Value::make_number(~i);
      return 0;
    }
    case Opc::Inc: case Opc::Dec: {
      if (Vm::is_bigint(top)) {
        Value r;
        if (!v.bigint_operation(call.in.op == Opc::Inc ? "inc" : "dec", top, Value(), &r)) return 1;
        top = r;
        return 0;
      }
      double d;
      if (!v.to_number(top, &d)) return 1;
      top = Value::make_number(call.in.op == Opc::Inc ? d + 1 : d - 1);
      return 0;
    }
    case Opc::ToStr: {
      JsString* s;
      if (!v.to_string(top, &s)) return 1;
      top = Value::make_string(s);
      return 0;
    }
    case Opc::Typeof:
      top = Value::make_string(v.intern(v.type_of(top)));
      return 0;
    default: return 1;
  }
}

uint64_t VmJit::helper_truthy(void* state, uint64_t vm, uint64_t packed) {
  JIT_CALL(call)
  return Vm::to_boolean(call.stack[call.h - 1]) ? 1 : 0;
}

uint64_t VmJit::helper_get_prop(void* state, uint64_t vm, uint64_t packed) {
  JIT_CALL(call)
  Value v;
  if (!call.vm.get(call.stack[call.h - 1], call.code->constants[static_cast<uint32_t>(call.in.a)].as_string(), &v)) return 1;
  call.stack[call.h - 1] = v;
  return 0;
}

uint64_t VmJit::helper_set_prop(void* state, uint64_t vm, uint64_t packed) {   // [obj, value] -> [value]
  JIT_CALL(call)
  const Value value = call.stack[call.h - 1], base = call.stack[call.h - 2];
  if (!call.vm.put_ex(base, call.code->constants[static_cast<uint32_t>(call.in.a)].as_string(), value, call.code->strict)) return 1;
  call.stack[call.h - 2] = value;
  return 0;
}

uint64_t VmJit::helper_get_elem(void* state, uint64_t vm, uint64_t packed) {   // [obj, key] -> [value]
  JIT_CALL(call)
  Value v;
  if (!call.vm.get_member(call.stack[call.h - 2], call.stack[call.h - 1], &v)) return 1;
  call.stack[call.h - 2] = v;
  return 0;
}

uint64_t VmJit::helper_set_elem(void* state, uint64_t vm, uint64_t packed) {   // [obj, key, value] -> [value]
  JIT_CALL(call)
  const Value value = call.stack[call.h - 1], key = call.stack[call.h - 2], base = call.stack[call.h - 3];
  if (!call.vm.put_member_ex(base, key, value, call.code->strict)) return 1;
  call.stack[call.h - 3] = value;
  return 0;
}

uint64_t VmJit::helper_get_global(void* state, uint64_t vm, uint64_t packed) {   // [] -> [value]
  JIT_CALL(call)
  Value v;
  if (!call.vm.lookup_global(call.code->constants[static_cast<uint32_t>(call.in.a)].as_string(), &v)) return 1;
  call.stack[call.h] = v;
  return 0;
}

uint64_t VmJit::helper_get_method(void* state, uint64_t vm, uint64_t packed) {   // [obj] -> [fn, obj]
  JIT_CALL(call)
  Value fn;
  const Value base = call.stack[call.h - 1];
  if (!call.vm.get(base, call.code->constants[static_cast<uint32_t>(call.in.a)].as_string(), &fn)) return 1;
  call.stack[call.h - 1] = fn;
  call.stack[call.h] = base;
  return 0;
}

uint64_t VmJit::helper_get_method_elem(void* state, uint64_t vm, uint64_t packed) {   // [obj, key] -> [fn, obj]
  JIT_CALL(call)
  Value fn;
  const Value base = call.stack[call.h - 2];
  if (!call.vm.get_member(base, call.stack[call.h - 1], &fn)) return 1;
  call.stack[call.h - 2] = fn;
  call.stack[call.h - 1] = base;
  return 0;
}

#undef JIT_CALL

bool VmJit::eligible(FunctionCode& code) {
  if (code.jit_state) return code.jit_state == 1;
  code.jit_state = 2;
  if (code.is_async || code.is_generator || code.code.count() == 0 || code.code.count() > 20000) return false;
  code.jit_state = 1;
  if (!code.jit_id) code.jit_id = next_id_++;
  return true;
}

bool VmJit::enter(uint32_t pc, uint64_t* exit) {
  if (!enabled_) return false;
  if (!runtime_) {
    if (tried_) return false;
    tried_ = true;
    runtime_ = jit::JitRuntime::create(vm_.allocator(), threshold_);
    if (!runtime_) {
      enabled_ = false;   // (nothing to run generated code from: the interpreter loop stops offering)
      return false;
    }
  }
  Vm::Frame& frame = vm_.frames_[vm_.frames_.count() - 1];
  if (!eligible(*frame.code)) return false;
  if (vm_.sp_ < frame.base) return false;
  const uint32_t height = vm_.sp_ - frame.base;
  if (height > kMaxHeight || frame.base > Vm::kStackValues - kStateValues) return false;
  Value* base = vm_.stack_ + frame.base;
  base[kThisSlot] = frame.this_value;
  base[kEnvSlot] = Value();
  base[kEnvSlot].u.cell = reinterpret_cast<Cell*>(frame.env);   // (the environment as a raw word: this slot is above anything the frame's stack holds)
  code_ = frame.code;
  height_ = height;
  // The stack is the state block (see the top of this file). This cast is the one place a Value array becomes bytes: a Value is 16 bytes, 8 byte aligned.
  ByteView state(reinterpret_cast<uint8_t*>(base), uint64_t{kStateValues} * sizeof(Value));
  return runtime_->try_run(*this, (uint64_t{frame.code->jit_id} << 32) | pc, state, exit);
}

bool VmJit::translate(uint64_t key, jit::IrBuilder& builder, uint64_t* guest_size) {
  if (!code_ || (key >> 32) != code_->jit_id) return false;
  Translator t(vm_.allocator(), builder, vm_, *code_, static_cast<uint32_t>(key), height_);
  *guest_size = 0;
  return t.run() && !builder.failed();
}

}  // namespace script
