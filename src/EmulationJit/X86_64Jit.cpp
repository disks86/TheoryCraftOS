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
#include "EmulationJit/X86_64Jit.hpp"
#include "EmulationJit/JitCore.hpp"
#include "EmulationJit/JitMemoryHelpers.hpp"
#include "EmulationJit/X86_64JitDecode.hpp"

namespace emulation {

namespace {

using State = X86_64Cpu::State;
using jit::IrCond;
using jit::IrOp;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
// Where the translated code finds things in State (the state block it is handed). The general registers come first, so register n is at 8 * n.
static_assert(__builtin_offsetof(State, r) == 0, "translated code addresses the registers from the start of the state");
constexpr uint64_t kOffRip = __builtin_offsetof(State, rip);
constexpr uint64_t kOffFlags = __builtin_offsetof(State, rflags);
constexpr uint64_t kOffFs = __builtin_offsetof(State, fs_base);
constexpr uint64_t kOffGs = __builtin_offsetof(State, gs_base);
constexpr uint64_t kOffFault = __builtin_offsetof(State, jit.fault);
constexpr uint64_t kOffCount = __builtin_offsetof(State, jit.count);
constexpr uint64_t kOffFlagOp = __builtin_offsetof(State, jit.flag_op);
constexpr uint64_t kOffFlagA = __builtin_offsetof(State, jit.flag_a);
constexpr uint64_t kOffFlagB = __builtin_offsetof(State, jit.flag_b);
constexpr uint64_t kOffEnd = __builtin_offsetof(State, jit.cpu) + 8;
#pragma GCC diagnostic pop
static_assert(kOffEnd < 2040, "the back ends reach the state with short displacements");

constexpr uint32_t kMaxBlock = static_cast<uint32_t>(IX86Jit::kLongestBlock);
constexpr uint64_t kFlagNeg = 11, kFlagImul = 12, kFlagInc = 9, kFlagDec = 10, kFlagShift = 16;

// Converts the state pointer a helper is handed back to the State it was made from (the one place: it is State's own address, passed through the IR as an opaque pointer).
State& state_of(void* state) { return *static_cast<State*>(state); }

uint64_t compute_flags(void* state, uint64_t, uint64_t) {
  State& s = state_of(state);
  static_cast<X86_64Cpu*>(s.jit.cpu)->jit_compute_flags();
  return 0;
}

constexpr uint64_t mask_of(uint32_t size) { return size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (size * 8)) - 1; }
constexpr uint64_t sign_of(uint32_t size) { return uint64_t{1} << (size * 8 - 1); }

// --- the flags a block has not computed yet --------------------------------------------------------------------------------------------------

// Flag-writing instructions do not compute their flags; they leave this behind, and the flags are made only if something reads them (a conditional branch, setcc, cmov, or
// the end of the block with a successor that does). The common compare-and-branch then never computes a flag at all.
enum class Pend : uint8_t {
  None,       // the flags are in RFLAGS
  Sub,        // a - b = r (sub, cmp)
  Add,        // a + b = r
  Logic,      // a op b = r with CF = OF = 0 (and, or, xor, test)
  IncDec,     // a +- 1 = r, CF as it was
  Other,      // only the helper can say (neg, imul, shifts)
};

struct Pending {
  Pend kind = Pend::None;
  uint8_t size = 0;
  uint8_t helper_kind = 0;      // what compute_flags() is told
  bool dec = false;
  uint32_t a = jit::kNoVReg, b = jit::kNoVReg, r = jit::kNoVReg;      // operands and result, all masked to `size`
};

// One translation: builds the IR of the block at `pc`.
class Translation {
 public:
  Translation(jit::IrBuilder& builder, GuestMemory& memory, uint64_t pc, bool exact_flags, uint32_t max_block) : b_(builder), mem_(memory), start_(pc), exact_flags_(exact_flags), max_block_(max_block) {}
  bool run(uint64_t* guest_size);

 private:
  struct Stub {
    uint32_t label = 0;
    uint64_t pc = 0;
    uint64_t count = 0;
    uint32_t dirty = 0;
    Pending pend;
    bool used = false;
  };

  // IR building blocks
  uint32_t K(int64_t v) { return b_.constant(v); }
  uint32_t zero() { return zero_; }
  uint32_t op2(IrOp op, uint32_t a, uint32_t c) { return b_.binary(op, a, c); }
  uint32_t mask(uint32_t v, uint32_t size) { return size >= 8 ? v : op2(IrOp::And, v, K(static_cast<int64_t>(mask_of(size)))); }
  uint32_t sext(uint32_t v, uint32_t size) {
    if (size >= 8) return v;
    const int64_t n = 64 - 8 * static_cast<int64_t>(size);
    return op2(IrOp::Sar, op2(IrOp::Shl, v, K(n)), K(n));
  }
  uint32_t bit(uint32_t v, uint32_t index) { return op2(IrOp::And, op2(IrOp::Shr, v, K(index)), K(1)); }
  uint32_t flip(uint32_t v01) { return op2(IrOp::Xor, v01, K(1)); }

  // guest registers
  uint32_t R(uint32_t r) {
    if (!loaded_[r]) {
      gpr_[r] = b_.load_state(r * 8);
      loaded_[r] = true;
    }
    return gpr_[r];
  }
  void W(uint32_t r, uint32_t v) {
    if (!loaded_[r]) {
      gpr_[r] = b_.new_vreg();
      loaded_[r] = true;
    }
    b_.assign(gpr_[r], v);
    dirty_ |= 1u << r;
  }
  bool high_byte(uint32_t index, uint32_t size, bool rex) const { return size == 1 && !rex && index >= 4 && index < 8; }
  uint32_t get_reg(uint32_t index, uint32_t size, bool rex) {
    if (high_byte(index, size, rex)) return mask(op2(IrOp::Shr, R(index - 4), K(8)), 1);
    // (a copy for the full register: the value read must not change when the instruction then writes that register; pending flags keep their operands)
    return size >= 8 ? b_.move(R(index)) : mask(R(index), size);
  }
  void set_reg(uint32_t index, uint32_t size, uint32_t value, bool rex) {
    switch (size) {
      case 1:
        if (high_byte(index, size, rex)) {
          W(index - 4, op2(IrOp::Or, op2(IrOp::And, R(index - 4), K(~int64_t{0xFF00})), op2(IrOp::Shl, mask(value, 1), K(8))));
        } else {
          W(index, op2(IrOp::Or, op2(IrOp::And, R(index), K(~int64_t{0xFF})), mask(value, 1)));
        }
        break;
      case 2: W(index, op2(IrOp::Or, op2(IrOp::And, R(index), K(~int64_t{0xFFFF})), mask(value, 2))); break;
      case 4: W(index, mask(value, 4)); break;
      default: W(index, value); break;
    }
  }

  // guest memory
  uint32_t effective_address(const X86Insn& i, uint64_t next_pc, bool with_segment) {
    uint32_t a = jit::kNoVReg;
    if (i.rip_relative) {
      a = K(static_cast<int64_t>(next_pc + static_cast<uint64_t>(i.disp)));
    } else {
      if (i.base != 0xFF) a = R(i.base);
      if (i.index != 0xFF) {
        uint32_t scaled = R(i.index);
        if (i.scale) scaled = op2(IrOp::Shl, scaled, K(i.scale));
        a = a == jit::kNoVReg ? scaled : op2(IrOp::Add, a, scaled);
      }
      if (a == jit::kNoVReg) a = K(i.disp);
      else if (i.disp != 0) a = op2(IrOp::Add, a, K(i.disp));
    }
    if (with_segment && i.segment) a = op2(IrOp::Add, a, b_.load_state(i.segment == 1 ? kOffFs : kOffGs));
    return a;
  }
  uint32_t fault_label() {
    if (!stub_.used) {
      stub_.used = true;
      stub_.label = b_.new_label();
      stubs_[stub_count_++] = stub_;
    }
    return stubs_[stub_count_ - 1].label;
  }
  void check_fault() { b_.branch_if(IrCond::Ne, b_.load_state(kOffFault), zero(), fault_label()); }
  uint32_t load(uint32_t address, uint32_t size) {
    const uint32_t v = b_.helper(load_helper<State>(size), address, zero());
    check_fault();
    return v;
  }
  void store(uint32_t address, uint32_t size, uint32_t value) {
    b_.helper(store_helper<State>(size), address, value);
    check_fault();
  }
  // The r/m operand of the instruction: a register or the memory at its (once computed) address.
  uint32_t read_rm(const X86Insn& i, uint32_t size) {
    if (!i.mem) return get_reg(i.rm, size, i.rex);
    return load(ea_, size);
  }
  void write_rm(const X86Insn& i, uint32_t size, uint32_t value) {
    if (!i.mem) set_reg(i.rm, size, value, i.rex);
    else store(ea_, size, value);
  }

  // flags
  void materialize(Pending& p) {
    if (p.kind == Pend::None) return;
    b_.store_state(kOffFlagOp, K((int64_t{p.helper_kind} << 8) | p.size));
    b_.store_state(kOffFlagA, p.a);
    b_.store_state(kOffFlagB, p.b);
    b_.helper(&compute_flags, zero(), zero());
    p.kind = Pend::None;
  }
  void set_pending(Pend kind, uint32_t size, uint32_t helper_kind, uint32_t a, uint32_t c, uint32_t r, bool dec = false) {
    pend_.kind = kind;
    pend_.size = static_cast<uint8_t>(size);
    pend_.helper_kind = static_cast<uint8_t>(helper_kind);
    pend_.dec = dec;
    pend_.a = a;
    pend_.b = c;
    pend_.r = r;
  }
  uint32_t flags_word() {
    materialize(pend_);
    return b_.load_state(kOffFlags);
  }
  uint32_t condition(uint32_t cc);
  uint32_t condition_from_word(uint32_t cc, uint32_t f);
  bool flags_dead_at(uint64_t pc);

  // block structure
  void write_back(uint32_t dirty) {
    for (uint32_t r = 0; r < 16; ++r)
      if (dirty & (1u << r)) b_.store_state(r * 8, gpr_[r]);
  }
  void finish_to(uint64_t target, uint64_t count) {
    Pending p = pend_;
    if (p.kind != Pend::None && !exact_flags_ && flags_dead_at(target)) p.kind = Pend::None;
    materialize(p);
    write_back(dirty_);
    b_.store_state(kOffRip, K(static_cast<int64_t>(target)));
    b_.store_state(kOffCount, K(static_cast<int64_t>(count)));
    b_.exit(0);
  }
  void finish_to_value(uint32_t target, uint64_t count) {
    Pending p = pend_;
    materialize(p);
    write_back(dirty_);
    b_.store_state(kOffRip, target);
    b_.store_state(kOffCount, K(static_cast<int64_t>(count)));
    b_.exit(0);
  }
  void emit_stubs() {
    for (uint32_t s = 0; s < stub_count_; ++s) {
      Stub& st = stubs_[s];
      b_.place_label(st.label);
      materialize(st.pend);
      write_back(st.dirty);
      b_.store_state(kOffRip, K(static_cast<int64_t>(st.pc)));
      b_.store_state(kOffCount, K(static_cast<int64_t>(st.count)));
      b_.exit(0);
    }
  }
  // False if the instruction ends the translation's reach (the block is finished by the caller).
  bool translate_insn(const X86Insn& i, uint64_t pc, uint64_t count, bool* ended);

  jit::IrBuilder& b_;
  GuestMemory& mem_;
  uint64_t start_;
  bool exact_flags_;
  uint32_t max_block_;
  uint32_t zero_ = 0;
  uint32_t gpr_[16] = {};
  bool loaded_[16] = {};
  uint32_t dirty_ = 0;
  uint32_t ea_ = 0;
  Pending pend_;
  Stub stub_;
  Stub stubs_[kMaxBlock + 1];
  uint32_t stub_count_ = 0;
};

// Evaluates condition code `cc` (the low nibble of jcc / setcc / cmov) as 0 or 1 from the pending flags where they say it directly, else from the flags word.
uint32_t Translation::condition(uint32_t cc) {
  const uint32_t base = cc >> 1;
  const bool negate = cc & 1;
  const Pending& p = pend_;
  uint32_t v = jit::kNoVReg;
  if (p.kind == Pend::Sub || p.kind == Pend::Add || p.kind == Pend::Logic || p.kind == Pend::IncDec) {
    const uint32_t size = p.size, bits = size * 8;
    const bool sub = p.kind == Pend::Sub, add = p.kind == Pend::Add, logic = p.kind == Pend::Logic, incdec = p.kind == Pend::IncDec;
    auto zf = [&]() { return b_.set_cond(IrCond::Eq, p.r, zero()); };
    auto sf = [&]() { return op2(IrOp::Shr, p.r, K(bits - 1)); };
    auto of = [&]() -> uint32_t {
      if (logic) return zero();
      if (incdec) return b_.set_cond(IrCond::Eq, p.dec ? p.a : p.r, K(static_cast<int64_t>(sign_of(size))));
      const uint32_t t = sub ? op2(IrOp::And, op2(IrOp::Xor, p.a, p.b), op2(IrOp::Xor, p.a, p.r)) : op2(IrOp::And, op2(IrOp::Xor, p.a, p.r), op2(IrOp::Xor, p.b, p.r));
      return op2(IrOp::Shr, t, K(bits - 1));
    };
    switch (base) {
      case 0:
        v = of();
        break;
      case 1:      // carry
        if (sub) v = b_.set_cond(IrCond::Ltu, p.a, p.b);
        else if (add) v = b_.set_cond(IrCond::Ltu, p.r, p.a);
        else if (logic) v = zero();
        break;
      case 2:
        v = sub ? b_.set_cond(IrCond::Eq, p.a, p.b) : zf();
        break;
      case 3:      // carry or zero
        if (sub) v = b_.set_cond(IrCond::Leu, p.a, p.b);
        else if (add) v = op2(IrOp::Or, b_.set_cond(IrCond::Ltu, p.r, p.a), zf());
        else if (logic) v = zf();
        break;
      case 4:
        v = sf();
        break;
      case 6:      // sign != overflow
        if (sub) v = b_.set_cond(IrCond::Lt, sext(p.a, size), sext(p.b, size));
        else if (logic) v = sf();
        else v = op2(IrOp::Xor, sf(), of());
        break;
      case 7:      // zero or sign != overflow
        if (sub) v = b_.set_cond(IrCond::Le, sext(p.a, size), sext(p.b, size));
        else if (logic) v = op2(IrOp::Or, zf(), sf());
        else v = op2(IrOp::Or, zf(), op2(IrOp::Xor, sf(), of()));
        break;
      default:
        break;      // parity, and carry after inc / dec: from the flags word
    }
  }
  if (v == jit::kNoVReg) return condition_from_word(cc, flags_word());
  return negate ? flip(v) : v;
}

uint32_t Translation::condition_from_word(uint32_t cc, uint32_t f) {
  const uint32_t base = cc >> 1;
  uint32_t v = jit::kNoVReg;
  switch (base) {
    case 0: v = bit(f, 11); break;
    case 1: v = op2(IrOp::And, f, K(1)); break;
    case 2: v = bit(f, 6); break;
    case 3: v = op2(IrOp::Or, op2(IrOp::And, f, K(1)), bit(f, 6)); break;
    case 4: v = bit(f, 7); break;
    case 5: v = bit(f, 2); break;
    case 6: v = op2(IrOp::Xor, bit(f, 7), bit(f, 11)); break;
    default: v = op2(IrOp::Or, bit(f, 6), op2(IrOp::Xor, bit(f, 7), bit(f, 11))); break;
  }
  return (cc & 1) ? flip(v) : v;
}

// Whether the status flags are certainly overwritten before anything can read them (or observe them through a fault) when execution continues at `pc`: looks at the next
// few instructions, which must be register-only so that none of them can fault.
bool Translation::flags_dead_at(uint64_t pc) {
  for (uint32_t n = 0; n < 8; ++n) {
    uint8_t bytes[15];
    const uint64_t available = mem_.fetchable(pc, sizeof(bytes));
    if (available == 0 || !mem_.fetch(pc, ByteView(bytes, available))) return false;
    X86Insn i;
    if (!decode_x86(ConstByteView(bytes, available), &i)) return false;
    if (i.mem && i.op != X86Op::Lea && i.op != X86Op::Nop) return false;
    if (i.op == X86Op::Push || i.op == X86Op::Pop || i.op == X86Op::Call || i.op == X86Op::Ret || i.op == X86Op::Leave) return false;
    const X86FlagUse use = x86_flag_use(i);
    if (use == X86FlagUse::WritesAll) return true;
    if (use != X86FlagUse::None) return false;
    if (i.op == X86Op::Jmp) {
      if (i.indirect) return false;
      pc = pc + i.length + static_cast<uint64_t>(i.imm);
    } else {
      pc += i.length;
    }
  }
  return false;
}

bool Translation::translate_insn(const X86Insn& i, uint64_t pc, uint64_t count, bool* ended) {
  const uint64_t next = pc + i.length;
  const uint32_t size = i.size;
  // What the instruction sees if it faults: the state before it.
  stub_ = Stub();
  stub_.pc = pc;
  stub_.count = count;
  stub_.dirty = dirty_;
  stub_.pend = pend_;
  // A register-form operand has no address.
  ea_ = jit::kNoVReg;
  if (i.mem && i.op != X86Op::Lea && i.op != X86Op::Nop) ea_ = effective_address(i, next, true);
  const int64_t imm_masked = size >= 8 ? i.imm : static_cast<int64_t>(static_cast<uint64_t>(i.imm) & mask_of(size));

  switch (i.op) {
    case X86Op::Nop:
      break;
    case X86Op::Mov: {
      if (i.has_imm && i.to_reg) {      // mov r, imm
        set_reg(i.reg, size, K(imm_masked), i.rex);
      } else if (i.has_imm) {           // mov r/m, imm
        write_rm(i, size, K(imm_masked));
      } else if (i.to_reg) {            // mov reg, r/m
        const uint32_t v = read_rm(i, size);
        set_reg(i.reg, size, v, i.rex);
      } else {                          // mov r/m, reg
        write_rm(i, size, get_reg(i.reg, size, i.rex));
      }
      break;
    }
    case X86Op::Lea:
      set_reg(i.reg, size, effective_address(i, next, false), i.rex);
      break;
    case X86Op::Movzx:
    case X86Op::Movsx: {
      uint32_t v = read_rm(i, i.src_size);
      if (i.op == X86Op::Movsx) v = sext(v, i.src_size);
      set_reg(i.reg, size, v, i.rex);
      break;
    }
    case X86Op::Movsxd: {
      uint32_t v = read_rm(i, 4);
      if (size == 8) v = sext(v, 4);
      set_reg(i.reg, size, v, i.rex);
      break;
    }
    case X86Op::Alu: {
      uint32_t a, c;
      if (i.accumulator) {
        a = get_reg(0, size, i.rex);
        c = K(imm_masked);
      } else if (i.has_imm) {
        a = read_rm(i, size);
        c = K(imm_masked);
      } else if (!i.to_reg) {
        a = read_rm(i, size);
        c = get_reg(i.reg, size, i.rex);
      } else {
        c = read_rm(i, size);
        a = get_reg(i.reg, size, i.rex);
      }
      IrOp op = IrOp::Add;
      Pend flags = Pend::Add;
      switch (i.kind) {
        case 0: op = IrOp::Add; flags = Pend::Add; break;
        case 1: op = IrOp::Or; flags = Pend::Logic; break;
        case 4: op = IrOp::And; flags = Pend::Logic; break;
        case 6: op = IrOp::Xor; flags = Pend::Logic; break;
        default: op = IrOp::Sub; flags = Pend::Sub; break;      // sub, cmp
      }
      const uint32_t r = mask(op2(op, a, c), size);
      if (i.kind != 7) {
        if (i.accumulator || (i.to_reg && !i.has_imm)) set_reg(i.accumulator ? 0 : i.reg, size, r, i.rex);
        else write_rm(i, size, r);
      }
      set_pending(flags, size, i.kind, a, c, r);
      break;
    }
    case X86Op::Test: {
      uint32_t a, c;
      if (i.accumulator) {
        a = get_reg(0, size, i.rex);
        c = K(imm_masked);
      } else if (i.has_imm) {
        a = read_rm(i, size);
        c = K(imm_masked);
      } else {
        a = read_rm(i, size);
        c = get_reg(i.reg, size, i.rex);
      }
      set_pending(Pend::Logic, size, 4, a, c, op2(IrOp::And, a, c));
      break;
    }
    case X86Op::Not: {
      const uint32_t v = read_rm(i, size);
      write_rm(i, size, mask(b_.unary(IrOp::Not, v), size));
      break;
    }
    case X86Op::Neg: {
      const uint32_t v = read_rm(i, size);
      const uint32_t r = mask(b_.unary(IrOp::Neg, v), size);
      write_rm(i, size, r);
      set_pending(Pend::Other, size, kFlagNeg, v, zero(), r);
      break;
    }
    case X86Op::Inc:
    case X86Op::Dec: {
      const bool dec = i.op == X86Op::Dec;
      const uint32_t v = read_rm(i, size);
      const uint32_t r = mask(op2(IrOp::Add, v, K(dec ? -1 : 1)), size);
      write_rm(i, size, r);
      materialize(pend_);      // (the carry flag survives)
      set_pending(Pend::IncDec, size, dec ? kFlagDec : kFlagInc, v, K(1), r, dec);
      break;
    }
    case X86Op::Imul2: {
      uint32_t a, c;
      if (i.has_imm) {
        a = read_rm(i, size);
        c = K(imm_masked);
      } else {
        c = read_rm(i, size);
        a = get_reg(i.reg, size, i.rex);
      }
      const uint32_t r = mask(op2(IrOp::Mul, a, c), size);
      set_reg(i.reg, size, r, i.rex);
      materialize(pend_);      // (the adjust flag survives)
      set_pending(Pend::Other, size, kFlagImul, a, c, r);
      break;
    }
    case X86Op::Shift: {
      const uint32_t count = static_cast<uint32_t>(i.imm) & (size == 8 ? 63 : 31);
      const uint32_t v = read_rm(i, size);
      uint32_t r;
      if (i.kind == 4) r = mask(op2(IrOp::Shl, v, K(count)), size);
      else if (i.kind == 5) r = op2(IrOp::Shr, v, K(count));
      else r = mask(op2(IrOp::Sar, sext(v, size), K(count)), size);
      write_rm(i, size, r);
      set_pending(Pend::Other, size, kFlagShift + i.kind, v, K(count), r);
      break;
    }
    case X86Op::Push: {
      uint32_t v;
      if (i.has_imm) v = K(i.imm);
      else if (i.opreg) v = R(i.reg);
      else v = read_rm(i, 8);
      const uint32_t sp = op2(IrOp::Sub, R(4), K(8));
      store(sp, 8, v);
      W(4, sp);
      break;
    }
    case X86Op::Pop: {
      const uint32_t v = load(R(4), 8);
      W(4, op2(IrOp::Add, R(4), K(8)));
      W(i.reg, v);
      break;
    }
    case X86Op::Leave: {
      const uint32_t v = load(R(5), 8);
      W(4, op2(IrOp::Add, R(5), K(8)));
      W(5, v);
      break;
    }
    case X86Op::Cdqe: {
      if (size == 8) W(0, sext(R(0), 4));
      else if (size == 4) W(0, mask(sext(R(0), 2), 4));
      else set_reg(0, 2, sext(R(0), 1), i.rex);
      break;
    }
    case X86Op::Cqo: {
      const uint32_t negative = op2(IrOp::Shr, get_reg(0, size, i.rex), K(size * 8 - 1));
      set_reg(2, size, b_.unary(IrOp::Neg, negative), i.rex);
      break;
    }
    case X86Op::Setcc: {
      const uint32_t c = condition(i.kind);
      write_rm(i, 1, c);
      break;
    }
    case X86Op::Cmov: {
      const uint32_t v = read_rm(i, size);
      const uint32_t c = condition(i.kind);
      const uint32_t old = get_reg(i.reg, size, i.rex);
      const uint32_t m = b_.unary(IrOp::Neg, c);      // all ones when the condition holds
      const uint32_t chosen = op2(IrOp::Or, op2(IrOp::And, v, m), op2(IrOp::And, old, b_.unary(IrOp::Not, m)));
      set_reg(i.reg, size, chosen, i.rex);
      break;
    }
    case X86Op::Jmp: {
      *ended = true;
      if (i.indirect) {
        const uint32_t target = read_rm(i, 8);
        finish_to_value(target, count + 1);
      } else {
        finish_to(next + static_cast<uint64_t>(i.imm), count + 1);
      }
      break;
    }
    case X86Op::Call: {
      *ended = true;
      uint32_t target = jit::kNoVReg;
      if (i.indirect) target = read_rm(i, 8);
      const uint32_t sp = op2(IrOp::Sub, R(4), K(8));
      store(sp, 8, K(static_cast<int64_t>(next)));
      W(4, sp);
      if (i.indirect) finish_to_value(target, count + 1);
      else finish_to(next + static_cast<uint64_t>(i.imm), count + 1);
      break;
    }
    case X86Op::Ret: {
      *ended = true;
      const uint32_t target = load(R(4), 8);
      W(4, op2(IrOp::Add, R(4), K(8 + (i.has_imm ? i.imm : 0))));
      finish_to_value(target, count + 1);
      break;
    }
    case X86Op::Jcc: {
      *ended = true;
      const uint32_t c = condition(i.kind);
      const uint32_t taken = b_.new_label();
      b_.branch_if(IrCond::Ne, c, zero(), taken);
      finish_to(next, count + 1);
      b_.place_label(taken);
      finish_to(next + static_cast<uint64_t>(i.imm), count + 1);
      break;
    }
    default:
      return false;
  }
  return true;
}

bool Translation::run(uint64_t* guest_size) {
  zero_ = K(0);
  uint64_t pc = start_;
  uint32_t count = 0;
  bool ended = false;
  while (!ended && count < max_block_) {
    uint8_t bytes[15];
    const uint64_t available = mem_.fetchable(pc, sizeof(bytes));
    X86Insn i;
    if (available == 0 || !mem_.fetch(pc, ByteView(bytes, available)) || !decode_x86(ConstByteView(bytes, available), &i)) break;
    // Code the guest can write to is never translated: it could change under the cache.
    if (mem_.writable(pc, 1) || mem_.writable(pc + i.length - 1, 1)) break;
    if (!translate_insn(i, pc, count, &ended)) break;
    pc += i.length;
    ++count;
  }
  if (count == 0) return false;
  if (!ended) finish_to(pc, count);
  emit_stubs();
  b_.set_state_size(kOffEnd);
  *guest_size = pc - start_;
  return true;
}

class X86Frontend final : public jit::IJitFrontend {
 public:
  X86Frontend(bool exact_flags, uint32_t max_block) : exact_flags_(exact_flags), max_block_(max_block < 1 ? 1 : max_block > kMaxBlock ? kMaxBlock : max_block) {}
  void bind(GuestMemory* memory) { memory_ = memory; }
  bool translate(uint64_t guest_pc, jit::IrBuilder& builder, uint64_t* guest_size) override {
    if (!memory_) return false;
    Translation t(builder, *memory_, guest_pc, exact_flags_, max_block_);
    return t.run(guest_size);
  }

 private:
  GuestMemory* memory_ = nullptr;
  bool exact_flags_;
  uint32_t max_block_;
};

// --- the engine --------------------------------------------------------------------------------------------------------------------------

class X86JitEngine final : public IX86Jit {
 public:
  X86JitEngine(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options)
      : frontend_(options.exact_flags, options.max_block), core_(allocator, code_memory, options.hot_threshold, frontend_) {}
  bool ok() const { return core_.ok(); }
  uint64_t blocks_entered() const override { return core_.blocks_entered(); }

  Result run(X86_64Cpu& cpu, GuestMemory& memory, uint64_t budget, uint64_t* retired) override {
    (void)budget;      // (the cpu only asks with room for the longest block)
    State& s = cpu.state();
    s.jit = GuestJitLink();
    s.jit.memory = &memory;
    s.jit.cpu = &cpu;
    frontend_.bind(&memory);
    if (!core_.run(s.rip, state_bytes(s), memory.layout_generation())) return Result::NotRun;
    *retired += s.jit.count;
    if (s.jit.fault) {
      cpu.jit_fault(s.rip, s.jit.fault_address, static_cast<uint32_t>(s.jit.fault_error));
      return Result::Faulted;
    }
    return Result::Ran;
  }

 private:
  X86Frontend frontend_;
  JitCore core_;
};

}  // namespace

SharedPointer<IX86Jit> create_x86_jit(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options) {
  SharedPointer<X86JitEngine> engine = SharedPointer<X86JitEngine>::make(allocator, allocator, code_memory, options);
  if (!engine || !engine->ok()) return SharedPointer<IX86Jit>();
  return SharedPointer<IX86Jit>(engine);
}

}  // namespace emulation
