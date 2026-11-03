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
#include "EmulationJit/AArch64Jit.hpp"
#include "EmulationJit/JitCore.hpp"
#include "EmulationJit/JitMemoryHelpers.hpp"

namespace emulation {

namespace {

using State = AArch64Cpu::State;
using jit::IrCond;
using jit::IrOp;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(__builtin_offsetof(State, x) == 0, "translated code addresses the registers from the start of the state");
constexpr uint64_t kOffSp = __builtin_offsetof(State, sp);
constexpr uint64_t kOffPc = __builtin_offsetof(State, pc);
constexpr uint64_t kOffNzcv = __builtin_offsetof(State, nzcv);
constexpr uint64_t kOffFault = __builtin_offsetof(State, jit.fault);
constexpr uint64_t kOffCount = __builtin_offsetof(State, jit.count);
constexpr uint64_t kOffFlagOp = __builtin_offsetof(State, jit.flag_op);
constexpr uint64_t kOffFlagA = __builtin_offsetof(State, jit.flag_a);
constexpr uint64_t kOffFlagB = __builtin_offsetof(State, jit.flag_b);
constexpr uint64_t kOffEnd = __builtin_offsetof(State, jit.cpu) + 8;
#pragma GCC diagnostic pop
static_assert(kOffSp == 31 * 8, "the stack pointer follows x30: register number 31 of the sp forms");
static_assert(kOffEnd < 2040, "the back ends reach the state with short displacements");

constexpr uint32_t kMaxBlock = static_cast<uint32_t>(IAArch64Jit::kLongestBlock);
constexpr uint64_t kFlagAdd = 0, kFlagSub = 1, kFlagLogic = 2;

constexpr uint64_t ones(uint32_t n) { return n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1; }
constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t bits_of(uint32_t insn, uint32_t hi, uint32_t lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }

uint64_t rotate_right(uint64_t v, uint32_t amount, uint32_t size) {
  amount %= size;
  if (amount == 0) return v & ones(size);
  return ((v >> amount) | (v << (size - amount))) & ones(size);
}

// DecodeBitMasks (the interpreter's, for logical immediates and bitfields).
bool decode_bitmasks(bool n, uint32_t imms, uint32_t immr, bool immediate, uint32_t datasize, uint64_t* wmask, uint64_t* tmask) {
  const uint32_t combined = (n ? 0x40u : 0) | (~imms & 0x3Fu);
  if (combined == 0) return false;
  uint32_t len = 6;
  while (!((combined >> len) & 1)) --len;
  if (len < 1) return false;
  const uint32_t esize = 1u << len;
  if (esize > datasize) return false;
  const uint32_t levels = esize - 1;
  if (immediate && (imms & levels) == levels) return false;
  const uint32_t s = imms & levels, r = immr & levels;
  const uint32_t d = (s - r) & levels;
  uint64_t welem = ones(s + 1), telem = ones(d + 1);
  welem = rotate_right(welem, r, esize);
  uint64_t w = 0, t = 0;
  for (uint32_t i = 0; i < datasize; i += esize) {
    w |= welem << i;
    t |= telem << i;
  }
  *wmask = w & ones(datasize);
  *tmask = t & ones(datasize);
  return true;
}

// Top Byte Ignore, as the interpreter's untag(): bits 63:56 of a user address (bit 55 clear) are not part of it.
uint64_t untag(uint64_t address) { return (address >> 55) & 1 ? address : address & 0x00FFFFFFFFFFFFFFull; }

State& state_of(void* state) { return *static_cast<State*>(state); }

uint64_t compute_flags(void* state, uint64_t, uint64_t) {
  State& s = state_of(state);
  static_cast<AArch64Cpu*>(s.jit.cpu)->jit_compute_flags();
  return 0;
}

template <uint32_t Size>
uint64_t load_untagged(void* state, uint64_t address, uint64_t unused) { return guest_load<State, Size>(state, untag(address), unused); }
template <uint32_t Size>
uint64_t store_untagged(void* state, uint64_t address, uint64_t value) { return guest_store<State, Size>(state, untag(address), value); }

jit::IrHelper load_fn(uint32_t size) {
  switch (size) {
    case 1: return &load_untagged<1>;
    case 2: return &load_untagged<2>;
    case 4: return &load_untagged<4>;
    default: return &load_untagged<8>;
  }
}
jit::IrHelper store_fn(uint32_t size) {
  switch (size) {
    case 1: return &store_untagged<1>;
    case 2: return &store_untagged<2>;
    case 4: return &store_untagged<4>;
    default: return &store_untagged<8>;
  }
}

enum class Pend : uint8_t { None, Sub, Add, Logic };

struct Pending {
  Pend kind = Pend::None;
  bool sf = true;
  uint32_t a = jit::kNoVReg, b = jit::kNoVReg, r = jit::kNoVReg;      // operands and result, masked to the operand size (a logical operation: the result is `a`)
};

class Translation {
 public:
  Translation(jit::IrBuilder& builder, GuestMemory& memory, uint64_t pc, uint32_t max_block) : b_(builder), mem_(memory), start_(pc), max_block_(max_block) {}
  bool run(uint64_t* guest_size);

 private:
  struct Stub {
    uint32_t label = 0;
    uint64_t pc = 0, count = 0;
    uint32_t dirty = 0;
    Pending pend;
    bool used = false;
  };

  uint32_t K(int64_t v) { return b_.constant(v); }
  uint32_t op2(IrOp op, uint32_t a, uint32_t c) { return b_.binary(op, a, c); }
  uint32_t mask(uint32_t v, bool sf) { return sf ? v : op2(IrOp::And, v, K(0xFFFFFFFF)); }
  uint32_t sext_to(uint32_t v, uint32_t bits) {
    if (bits >= 64) return v;
    const int64_t n = 64 - static_cast<int64_t>(bits);
    return op2(IrOp::Sar, op2(IrOp::Shl, v, K(n)), K(n));
  }

  // Register 31 is the zero register in the R / W forms, the stack pointer in the RS / WS forms. Reads give a copy: the value must not change when the instruction then writes
  // the register, and the pending flags keep their operands.
  uint32_t raw(uint32_t r) {
    if (!loaded_[r]) {
      x_[r] = b_.load_state(r * 8);
      loaded_[r] = true;
    }
    return x_[r];
  }
  uint32_t R(uint32_t r, bool sf) { return r == 31 ? zero_ : mask(b_.move(raw(r)), sf); }
  uint32_t RS(uint32_t r, bool sf) { return mask(b_.move(raw(r)), sf); }
  void put(uint32_t r, uint32_t v) {
    if (!loaded_[r]) {
      x_[r] = b_.new_vreg();
      loaded_[r] = true;
    }
    b_.assign(x_[r], v);
    dirty_ |= 1u << r;
  }
  void W(uint32_t r, uint32_t v, bool sf) {
    if (r != 31) put(r, mask(v, sf));
  }
  void WS(uint32_t r, uint32_t v, bool sf) { put(r, mask(v, sf)); }

  uint32_t fault_label() {
    if (!stub_.used) {
      stub_.used = true;
      stub_.label = b_.new_label();
      stubs_[stub_count_++] = stub_;
    }
    return stubs_[stub_count_ - 1].label;
  }
  void check_fault() { b_.branch_if(IrCond::Ne, b_.load_state(kOffFault), zero_, fault_label()); }
  uint32_t load(uint32_t address, uint32_t size) {
    const uint32_t v = b_.helper(load_fn(size), address, zero_);
    check_fault();
    return v;
  }
  void store(uint32_t address, uint32_t size, uint32_t value) {
    b_.helper(store_fn(size), address, value);
    check_fault();
  }

  // flags
  void materialize(Pending& p) {
    if (p.kind == Pend::None) return;
    const uint64_t kind = p.kind == Pend::Add ? kFlagAdd : p.kind == Pend::Sub ? kFlagSub : kFlagLogic;
    b_.store_state(kOffFlagOp, K(static_cast<int64_t>((kind << 8) | (p.sf ? 8 : 4))));
    b_.store_state(kOffFlagA, p.kind == Pend::Logic ? p.r : p.a);
    b_.store_state(kOffFlagB, p.kind == Pend::Logic ? zero_ : p.b);
    b_.helper(&compute_flags, zero_, zero_);
    p.kind = Pend::None;
  }
  void set_pending(Pend kind, bool sf, uint32_t a, uint32_t c, uint32_t r) {
    pend_.kind = kind;
    pend_.sf = sf;
    pend_.a = a;
    pend_.b = c;
    pend_.r = r;
  }
  uint32_t condition(uint32_t cond);

  // block structure
  void write_back(uint32_t dirty) {
    for (uint32_t r = 0; r < 32; ++r)
      if (dirty & (1u << r)) b_.store_state(r * 8, x_[r]);
  }
  void finish_to(uint64_t target, uint64_t count) {
    Pending p = pend_;
    materialize(p);
    write_back(dirty_);
    b_.store_state(kOffPc, K(static_cast<int64_t>(target)));
    b_.store_state(kOffCount, K(static_cast<int64_t>(count)));
    b_.exit(0);
  }
  void finish_to_value(uint32_t target, uint64_t count) {
    Pending p = pend_;
    materialize(p);
    write_back(dirty_);
    b_.store_state(kOffPc, target);
    b_.store_state(kOffCount, K(static_cast<int64_t>(count)));
    b_.exit(0);
  }
  void emit_stubs() {
    for (uint32_t s = 0; s < stub_count_; ++s) {
      Stub& st = stubs_[s];
      b_.place_label(st.label);
      materialize(st.pend);
      write_back(st.dirty);
      b_.store_state(kOffPc, K(static_cast<int64_t>(st.pc)));
      b_.store_state(kOffCount, K(static_cast<int64_t>(st.count)));
      b_.exit(0);
    }
  }
  uint32_t shifted(uint32_t v, uint32_t type, uint32_t amount, bool sf);
  uint32_t extended(uint32_t v, uint32_t option, uint32_t shift);
  bool translate_insn(uint32_t insn, uint64_t pc, uint64_t count, bool* ended);
  bool data_immediate(uint32_t insn, uint64_t pc);
  bool data_register(uint32_t insn);
  bool load_store(uint32_t insn, uint64_t pc);
  bool branch(uint32_t insn, uint64_t pc, uint64_t count, bool* ended);

  jit::IrBuilder& b_;
  GuestMemory& mem_;
  uint64_t start_;
  uint32_t max_block_;
  uint32_t zero_ = 0;
  uint32_t x_[32] = {};
  bool loaded_[32] = {};
  uint32_t dirty_ = 0;
  Pending pend_;
  Stub stub_;
  Stub stubs_[kMaxBlock + 1];
  uint32_t stub_count_ = 0;
};

// The condition (0..15) as 0 or 1: straight from the pending flag operands where they say it, else from the NZCV word.
uint32_t Translation::condition(uint32_t cond) {
  const uint32_t base = cond >> 1;
  const bool negate = cond & 1;
  if (base == 7) return K(1);      // AL and NV
  const Pending& p = pend_;
  uint32_t v = jit::kNoVReg;
  if (p.kind != Pend::None) {
    const bool sf = p.sf;
    const uint32_t bits = sf ? 64 : 32;
    auto n = [&]() { return op2(IrOp::Shr, p.r, K(bits - 1)); };
    auto z = [&]() { return b_.set_cond(IrCond::Eq, p.r, zero_); };
    auto sign_bit = [&](uint32_t t) { return op2(IrOp::Shr, t, K(bits - 1)); };
    auto vflag = [&]() -> uint32_t {
      if (p.kind == Pend::Logic) return zero_;
      const uint32_t t = p.kind == Pend::Sub ? op2(IrOp::And, op2(IrOp::Xor, p.a, p.b), op2(IrOp::Xor, p.a, p.r)) : op2(IrOp::And, op2(IrOp::Xor, p.a, p.r), op2(IrOp::Xor, p.b, p.r));
      return sign_bit(t);
    };
    auto cflag = [&]() -> uint32_t {
      if (p.kind == Pend::Logic) return zero_;
      return p.kind == Pend::Sub ? b_.set_cond(IrCond::Geu, p.a, p.b) : b_.set_cond(IrCond::Ltu, p.r, p.a);
    };
    switch (base) {
      case 0: v = p.kind == Pend::Sub ? b_.set_cond(IrCond::Eq, p.a, p.b) : z(); break;      // EQ / NE
      case 1: v = cflag(); break;                                                           // CS / CC
      case 2: v = n(); break;                                                               // MI / PL
      case 3: v = vflag(); break;                                                           // VS / VC
      case 4:                                                                               // HI / LS: C and not Z
        if (p.kind == Pend::Sub) v = b_.set_cond(IrCond::Gtu, p.a, p.b);
        else if (p.kind == Pend::Logic) v = zero_;
        else v = op2(IrOp::And, cflag(), op2(IrOp::Xor, z(), K(1)));
        break;
      case 5:                                                                               // GE / LT: N == V
        if (p.kind == Pend::Sub) v = b_.set_cond(IrCond::Ge, sext_to(p.a, bits), sext_to(p.b, bits));
        else v = op2(IrOp::Xor, op2(IrOp::Xor, n(), vflag()), K(1));
        break;
      default:                                                                              // GT / LE: N == V and not Z
        if (p.kind == Pend::Sub) v = b_.set_cond(IrCond::Gt, sext_to(p.a, bits), sext_to(p.b, bits));
        else v = op2(IrOp::And, op2(IrOp::Xor, op2(IrOp::Xor, n(), vflag()), K(1)), op2(IrOp::Xor, z(), K(1)));
        break;
    }
    // (base 4 and 6 are the "positive" tests; the odd condition code inverts all of them)
  } else {
    const uint32_t word = b_.load_state(kOffNzcv);
    auto flag = [&](uint32_t index) { return op2(IrOp::And, op2(IrOp::Shr, word, K(index)), K(1)); };
    const uint32_t nf = flag(31), zf = flag(30), cf = flag(29), vf = flag(28);
    switch (base) {
      case 0: v = zf; break;
      case 1: v = cf; break;
      case 2: v = nf; break;
      case 3: v = vf; break;
      case 4: v = op2(IrOp::And, cf, op2(IrOp::Xor, zf, K(1))); break;
      case 5: v = op2(IrOp::Xor, op2(IrOp::Xor, nf, vf), K(1)); break;
      default: v = op2(IrOp::And, op2(IrOp::Xor, op2(IrOp::Xor, nf, vf), K(1)), op2(IrOp::Xor, zf, K(1))); break;
    }
  }
  return negate ? op2(IrOp::Xor, v, K(1)) : v;
}

uint32_t Translation::shifted(uint32_t v, uint32_t type, uint32_t amount, bool sf) {
  const uint32_t size = sf ? 64 : 32;
  amount %= size;
  if (amount == 0) return v;
  switch (type) {
    case 0: return mask(op2(IrOp::Shl, v, K(amount)), sf);
    case 1: return op2(IrOp::Shr, v, K(amount));
    case 2: return mask(op2(IrOp::Sar, sext_to(v, size), K(amount)), sf);
    default: return mask(op2(IrOp::Or, op2(IrOp::Shr, v, K(amount)), op2(IrOp::Shl, v, K(size - amount))), sf);
  }
}

uint32_t Translation::extended(uint32_t v, uint32_t option, uint32_t shift) {
  uint32_t r;
  switch (option) {
    case 0: r = op2(IrOp::And, v, K(0xFF)); break;
    case 1: r = op2(IrOp::And, v, K(0xFFFF)); break;
    case 2: r = op2(IrOp::And, v, K(0xFFFFFFFF)); break;
    case 4: r = sext_to(v, 8); break;
    case 5: r = sext_to(v, 16); break;
    case 6: r = sext_to(v, 32); break;
    default: r = v; break;
  }
  return shift ? op2(IrOp::Shl, r, K(shift)) : r;
}

bool Translation::data_immediate(uint32_t insn, uint64_t pc) {
  const bool sf = insn >> 31;
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31;
  switch (bits_of(insn, 25, 23)) {
    case 0: case 1: {      // ADR, ADRP
      const uint64_t imm = (uint64_t{bits_of(insn, 23, 5)} << 2) | bits_of(insn, 30, 29);
      const int64_t offset = sext(imm, 21);
      const uint64_t value = (insn >> 31) ? (pc & ~uint64_t{0xFFF}) + (static_cast<uint64_t>(offset) << 12) : pc + static_cast<uint64_t>(offset);
      W(rd, K(static_cast<int64_t>(value)), true);
      return true;
    }
    case 2: {      // add/subtract immediate
      const bool sub = (insn >> 30) & 1, set = (insn >> 29) & 1;
      uint64_t imm = bits_of(insn, 21, 10);
      if ((insn >> 22) & 1) imm <<= 12;
      const uint32_t a = RS(rn, sf), c = K(static_cast<int64_t>(imm));
      const uint32_t r = mask(op2(sub ? IrOp::Sub : IrOp::Add, a, c), sf);
      if (set) {
        W(rd, r, sf);
        set_pending(sub ? Pend::Sub : Pend::Add, sf, a, c, r);
      } else {
        WS(rd, r, sf);
      }
      return true;
    }
    case 4: {      // logical immediate
      const uint32_t opc = bits_of(insn, 30, 29);
      uint64_t wmask, tmask;
      if (!sf && ((insn >> 22) & 1)) return false;
      if (!decode_bitmasks((insn >> 22) & 1, bits_of(insn, 15, 10), bits_of(insn, 21, 16), true, sf ? 64 : 32, &wmask, &tmask)) return false;
      const uint32_t a = R(rn, sf), c = K(static_cast<int64_t>(wmask));
      const uint32_t r = op2(opc == 1 ? IrOp::Or : opc == 2 ? IrOp::Xor : IrOp::And, a, c);
      if (opc == 3) {
        W(rd, r, sf);
        set_pending(Pend::Logic, sf, r, zero_, r);
      } else {
        WS(rd, r, sf);
      }
      return true;
    }
    case 5: {      // move wide
      const uint32_t opc = bits_of(insn, 30, 29), hw = bits_of(insn, 22, 21);
      if (!sf && hw > 1) return false;
      const uint64_t imm = uint64_t{bits_of(insn, 20, 5)} << (16 * hw);
      if (opc == 0) W(rd, K(static_cast<int64_t>(~imm)), sf);
      else if (opc == 2) W(rd, K(static_cast<int64_t>(imm)), sf);
      else if (opc == 3) W(rd, op2(IrOp::Or, op2(IrOp::And, R(rd, sf), K(static_cast<int64_t>(~(uint64_t{0xFFFF} << (16 * hw))))), K(static_cast<int64_t>(imm))), sf);
      else return false;
      return true;
    }
    case 6: {      // bitfield
      const uint32_t opc = bits_of(insn, 30, 29), immr = bits_of(insn, 21, 16), imms = bits_of(insn, 15, 10);
      const bool n = (insn >> 22) & 1;
      if (opc == 3 || n != sf) return false;
      uint64_t wmask, tmask;
      const uint32_t size = sf ? 64 : 32;
      if (!decode_bitmasks(n, imms, immr, false, size, &wmask, &tmask)) return false;
      const uint32_t src = R(rn, sf);
      uint32_t rotated = src;
      if (immr % size != 0) rotated = mask(op2(IrOp::Or, op2(IrOp::Shr, src, K(immr % size)), op2(IrOp::Shl, src, K(size - immr % size))), sf);
      uint32_t result;
      if (opc == 1) {      // BFM
        const uint32_t dst = R(rd, sf);
        const uint32_t bot = op2(IrOp::Or, op2(IrOp::And, dst, K(static_cast<int64_t>(~wmask))), op2(IrOp::And, rotated, K(static_cast<int64_t>(wmask))));
        result = op2(IrOp::Or, op2(IrOp::And, dst, K(static_cast<int64_t>(~tmask))), op2(IrOp::And, bot, K(static_cast<int64_t>(tmask))));
      } else if (opc == 0) {      // SBFM
        const uint32_t top = b_.unary(IrOp::Neg, op2(IrOp::And, op2(IrOp::Shr, src, K(imms)), K(1)));      // all ones when the source bit is set
        const uint32_t bot = op2(IrOp::And, rotated, K(static_cast<int64_t>(wmask)));
        result = op2(IrOp::Or, op2(IrOp::And, top, K(static_cast<int64_t>(~tmask))), op2(IrOp::And, bot, K(static_cast<int64_t>(tmask))));
      } else {      // UBFM
        result = op2(IrOp::And, rotated, K(static_cast<int64_t>(wmask & tmask)));
      }
      W(rd, result, sf);
      return true;
    }
    default:
      return false;
  }
}

bool Translation::data_register(uint32_t insn) {
  const bool sf = insn >> 31;
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const uint32_t size = sf ? 64 : 32;

  if (bits_of(insn, 28, 24) == 0x0A) {      // logical, shifted register
    const uint32_t opc = bits_of(insn, 30, 29), shift = bits_of(insn, 23, 22), amount = bits_of(insn, 15, 10);
    const bool invert = (insn >> 21) & 1;
    if (!sf && amount >= 32) return false;
    uint32_t b = shifted(R(rm, sf), shift, amount, sf);
    if (invert) b = mask(b_.unary(IrOp::Not, b), sf);
    const uint32_t a = R(rn, sf);
    const uint32_t r = op2(opc == 1 ? IrOp::Or : opc == 2 ? IrOp::Xor : IrOp::And, a, b);
    W(rd, r, sf);
    if (opc == 3) set_pending(Pend::Logic, sf, r, zero_, r);
    return true;
  }
  if (bits_of(insn, 28, 24) == 0x0B) {      // add/subtract, shifted or extended register
    const bool sub = (insn >> 30) & 1, set = (insn >> 29) & 1, ext = (insn >> 21) & 1;
    uint32_t a, b;
    if (ext) {
      if (bits_of(insn, 23, 22) != 0) return false;
      const uint32_t amount = bits_of(insn, 12, 10);
      if (amount > 4) return false;
      b = mask(extended(R(rm, true), bits_of(insn, 15, 13), amount), sf);
      a = RS(rn, sf);
    } else {
      const uint32_t shift = bits_of(insn, 23, 22), amount = bits_of(insn, 15, 10);
      if (shift == 3 || (!sf && amount >= 32)) return false;
      b = shifted(R(rm, sf), shift, amount, sf);
      a = R(rn, sf);
    }
    const uint32_t r = mask(op2(sub ? IrOp::Sub : IrOp::Add, a, b), sf);
    if (set || !ext) W(rd, r, sf);
    else WS(rd, r, sf);
    if (set) set_pending(sub ? Pend::Sub : Pend::Add, sf, a, b, r);
    return true;
  }
  if (bits_of(insn, 28, 21) == 0xD4) {      // conditional select
    if (((insn >> 29) & 1) || (insn & 0x800)) return false;
    const bool op = (insn >> 30) & 1, o2 = (insn >> 10) & 1;
    const uint32_t vt = R(rn, sf);
    uint32_t vf = R(rm, sf);
    if (op) vf = b_.unary(IrOp::Not, vf);
    if (o2) vf = op2(IrOp::Add, vf, K(1));
    vf = mask(vf, sf);
    const uint32_t c = condition(bits_of(insn, 15, 12));
    const uint32_t m = b_.unary(IrOp::Neg, c);      // all ones when the condition holds
    W(rd, op2(IrOp::Or, op2(IrOp::And, vt, m), op2(IrOp::And, vf, b_.unary(IrOp::Not, m))), sf);
    return true;
  }
  if (bits_of(insn, 28, 21) == 0xD6 && !((insn >> 29) & 1) && !((insn >> 30) & 1)) {      // data processing, 2 sources: the variable shifts
    const uint32_t opcode = bits_of(insn, 15, 10);
    if (opcode < 8 || opcode > 11) return false;      // (divides, CRC and min / max are interpreted)
    const uint32_t a = R(rn, sf);
    const uint32_t amount = op2(IrOp::And, R(rm, sf), K(size - 1));
    uint32_t r;
    switch (opcode) {
      case 8: r = op2(IrOp::Shl, a, amount); break;
      case 9: r = op2(IrOp::Shr, a, amount); break;
      case 10: r = op2(IrOp::Sar, sext_to(a, size), amount); break;
      default: r = op2(IrOp::Or, op2(IrOp::Shr, a, amount), op2(IrOp::Shl, a, op2(IrOp::Sub, K(size), amount))); break;
    }
    W(rd, r, sf);
    return true;
  }
  if (bits_of(insn, 28, 24) == 0x1B) {      // data processing, 3 sources
    const uint32_t op31 = bits_of(insn, 23, 21), ra = bits_of(insn, 14, 10);
    const bool o0 = (insn >> 15) & 1;
    if (bits_of(insn, 30, 29) != 0) return false;
    uint32_t product;
    if (op31 == 0) {      // MADD, MSUB
      product = op2(IrOp::Mul, R(rn, sf), R(rm, sf));
      const uint32_t a = R(ra, sf);
      W(rd, op2(o0 ? IrOp::Sub : IrOp::Add, a, product), sf);
      return true;
    }
    if (!sf || (op31 != 1 && op31 != 5)) return false;      // (high multiplies are interpreted)
    if (op31 == 1) product = op2(IrOp::Mul, sext_to(R(rn, true), 32), sext_to(R(rm, true), 32));      // SMADDL, SMSUBL
    else product = op2(IrOp::Mul, op2(IrOp::And, R(rn, true), K(0xFFFFFFFF)), op2(IrOp::And, R(rm, true), K(0xFFFFFFFF)));      // UMADDL, UMSUBL
    const uint32_t a = R(ra, true);
    W(rd, op2(o0 ? IrOp::Sub : IrOp::Add, a, product), true);
    return true;
  }
  return false;
}

bool Translation::load_store(uint32_t insn, uint64_t pc) {
  if ((insn >> 26) & 1) return false;      // (vector registers)
  const uint32_t group = bits_of(insn, 29, 27);
  const uint32_t rt = insn & 31, rn = (insn >> 5) & 31;
  if (group == 3) {      // load register (literal)
    if (bits_of(insn, 25, 24) != 0) return false;
    const uint32_t opc = insn >> 30;
    if (opc == 3) return true;      // PRFM
    const uint64_t address = pc + static_cast<uint64_t>(sext(bits_of(insn, 23, 5), 19) * 4);
    uint32_t v = load(K(static_cast<int64_t>(address)), opc == 1 ? 8 : 4);
    if (opc == 2) v = sext_to(v, 32);
    W(rt, v, opc != 0);
    return true;
  }
  if (group == 5) {      // pairs
    const uint32_t opc = insn >> 30, rt2 = (insn >> 10) & 31;
    const bool load_bit = (insn >> 22) & 1;
    const uint32_t mode = bits_of(insn, 24, 23);      // 0 no-allocate, 1 post, 2 offset, 3 pre
    if (opc == 3) return false;
    const uint32_t size = opc == 2 ? 8 : 4;
    if (load_bit && rt == rt2) return false;
    const int64_t offset = sext(bits_of(insn, 21, 15), 7) * static_cast<int64_t>(size);
    const uint32_t base = RS(rn, true);
    const uint32_t access = mode == 1 ? base : op2(IrOp::Add, base, K(offset));
    if (load_bit) {
      uint32_t a = load(access, size);
      uint32_t b = load(op2(IrOp::Add, access, K(size)), size);
      if (opc == 1) {      // LDPSW
        a = sext_to(a, 32);
        b = sext_to(b, 32);
        W(rt, a, true);
        W(rt2, b, true);
      } else {
        W(rt, a, size == 8);
        W(rt2, b, size == 8);
      }
    } else {
      store(access, size, R(rt, true));
      store(op2(IrOp::Add, access, K(size)), size, R(rt2, true));
    }
    if (mode == 1) WS(rn, op2(IrOp::Add, base, K(offset)), true);
    else if (mode == 3) WS(rn, access, true);
    return true;
  }
  if (group != 7) return false;
  if (bits_of(insn, 25, 24) == 0 && ((insn >> 21) & 1) && bits_of(insn, 11, 10) == 0) return false;      // (atomics)
  if (bits_of(insn, 25, 24) > 1) return false;
  const uint32_t size = insn >> 30, opc = bits_of(insn, 23, 22);
  uint32_t address;
  uint32_t writeback = jit::kNoVReg;
  if (bits_of(insn, 25, 24) == 1) {      // unsigned offset
    address = op2(IrOp::Add, RS(rn, true), K(static_cast<int64_t>(uint64_t{bits_of(insn, 21, 10)} << size)));
  } else if (!((insn >> 21) & 1)) {      // 9-bit immediate forms
    const int64_t imm = sext(bits_of(insn, 20, 12), 9);
    const uint32_t mode = bits_of(insn, 11, 10);
    const uint32_t base = RS(rn, true);
    if (mode == 1) {
      address = base;
      writeback = op2(IrOp::Add, base, K(imm));
    } else if (mode == 3) {
      address = op2(IrOp::Add, base, K(imm));
      writeback = address;
    } else {
      address = op2(IrOp::Add, base, K(imm));
    }
  } else {      // register offset
    if (bits_of(insn, 11, 10) != 2) return false;
    const uint32_t option = bits_of(insn, 15, 13);
    if (!(option & 2)) return false;
    const uint32_t index = extended(R(bits_of(insn, 20, 16), true), option, ((insn >> 12) & 1) ? size : 0);
    address = op2(IrOp::Add, RS(rn, true), index);
  }
  const uint32_t bytes = 1u << size;
  if (opc == 0) {
    store(address, bytes, R(rt, true));
  } else if (opc == 1) {
    W(rt, load(address, bytes), true);
  } else {
    if (size == 3) return true;      // PRFM
    if (size == 2 && opc == 3) return false;
    W(rt, sext_to(load(address, bytes), bytes * 8), opc == 2);
  }
  if (writeback != jit::kNoVReg) WS(rn, writeback, true);
  return true;
}

bool Translation::branch(uint32_t insn, uint64_t pc, uint64_t count, bool* ended) {
  if ((insn & 0x7C000000u) == 0x14000000u) {      // B, BL
    if (insn >> 31) W(30, K(static_cast<int64_t>(pc + 4)), true);
    *ended = true;
    finish_to(pc + static_cast<uint64_t>(sext(insn & 0x3FFFFFF, 26) * 4), count + 1);
    return true;
  }
  if ((insn & 0xFF000010u) == 0x54000000u) {      // B.cond
    const uint32_t c = condition(insn & 15);
    const uint32_t taken = b_.new_label();
    b_.branch_if(IrCond::Ne, c, zero_, taken);
    *ended = true;
    finish_to(pc + 4, count + 1);
    b_.place_label(taken);
    finish_to(pc + static_cast<uint64_t>(sext(bits_of(insn, 23, 5), 19) * 4), count + 1);
    return true;
  }
  if ((insn & 0x7E000000u) == 0x34000000u) {      // CBZ, CBNZ
    const bool sf = insn >> 31, nonzero = (insn >> 24) & 1;
    const uint32_t taken = b_.new_label();
    b_.branch_if(nonzero ? IrCond::Ne : IrCond::Eq, R(insn & 31, sf), zero_, taken);
    *ended = true;
    finish_to(pc + 4, count + 1);
    b_.place_label(taken);
    finish_to(pc + static_cast<uint64_t>(sext(bits_of(insn, 23, 5), 19) * 4), count + 1);
    return true;
  }
  if ((insn & 0x7E000000u) == 0x36000000u) {      // TBZ, TBNZ
    const uint32_t bit = ((insn >> 31) << 5) | bits_of(insn, 23, 19);
    const bool nonzero = (insn >> 24) & 1;
    const uint32_t value = op2(IrOp::And, op2(IrOp::Shr, R(insn & 31, true), K(bit)), K(1));
    const uint32_t taken = b_.new_label();
    b_.branch_if(nonzero ? IrCond::Ne : IrCond::Eq, value, zero_, taken);
    *ended = true;
    finish_to(pc + 4, count + 1);
    b_.place_label(taken);
    finish_to(pc + static_cast<uint64_t>(sext(bits_of(insn, 18, 5), 14) * 4), count + 1);
    return true;
  }
  if ((insn & 0xFFFFFC1Fu) == 0xD61F0000u || (insn & 0xFFFFFC1Fu) == 0xD63F0000u || (insn & 0xFFFFFC1Fu) == 0xD65F0000u) {      // BR, BLR, RET
    const uint32_t target = R((insn >> 5) & 31, true);
    if (bits_of(insn, 24, 21) == 1) W(30, K(static_cast<int64_t>(pc + 4)), true);      // (after the target is read)
    *ended = true;
    finish_to_value(target, count + 1);
    return true;
  }
  return false;
}

bool Translation::translate_insn(uint32_t insn, uint64_t pc, uint64_t count, bool* ended) {
  stub_ = Stub();
  stub_.pc = pc;
  stub_.count = count;
  stub_.dirty = dirty_;
  stub_.pend = pend_;
  if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) return true;      // hints
  switch (bits_of(insn, 28, 25)) {
    case 8: case 9: return data_immediate(insn, pc);
    case 10: case 11: return branch(insn, pc, count, ended);
    case 4: case 6: case 12: case 14: return load_store(insn, pc);
    case 5: case 13: return data_register(insn);
    default: return false;
  }
}

bool Translation::run(uint64_t* guest_size) {
  zero_ = K(0);
  uint64_t pc = start_;
  uint32_t count = 0;
  bool ended = false;
  while (!ended && count < max_block_) {
    if (pc & 3) break;
    uint8_t raw[4];
    if (!mem_.fetch(pc, ByteView(raw, 4))) break;
    const uint32_t insn = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
    // Code the guest can write to is never translated: it could change under the cache.
    if (mem_.writable(pc, 4)) break;
    if (!translate_insn(insn, pc, count, &ended)) break;
    pc += 4;
    ++count;
  }
  if (count == 0) return false;
  if (!ended) finish_to(pc, count);
  emit_stubs();
  b_.set_state_size(kOffEnd);
  *guest_size = pc - start_;
  return true;
}

class AArch64Frontend final : public jit::IJitFrontend {
 public:
  explicit AArch64Frontend(uint32_t max_block) : max_block_(max_block < 1 ? 1 : max_block > kMaxBlock ? kMaxBlock : max_block) {}
  void bind(GuestMemory* memory) { memory_ = memory; }
  bool translate(uint64_t guest_pc, jit::IrBuilder& builder, uint64_t* guest_size) override {
    if (!memory_) return false;
    Translation t(builder, *memory_, guest_pc, max_block_);
    return t.run(guest_size);
  }

 private:
  GuestMemory* memory_ = nullptr;
  uint32_t max_block_;
};

class AArch64JitEngine final : public IAArch64Jit {
 public:
  AArch64JitEngine(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options)
      : frontend_(options.max_block), core_(allocator, code_memory, options.hot_threshold, frontend_) {}
  bool ok() const { return core_.ok(); }
  uint64_t blocks_entered() const override { return core_.blocks_entered(); }

  Result run(AArch64Cpu& cpu, GuestMemory& memory, uint64_t budget, uint64_t* retired) override {
    (void)budget;      // (the cpu only asks with room for the longest block)
    State& s = cpu.state();
    s.jit = GuestJitLink();
    s.jit.memory = &memory;
    s.jit.cpu = &cpu;
    frontend_.bind(&memory);
    if (!core_.run(s.pc, state_bytes(s), memory.layout_generation())) return Result::NotRun;
    *retired += s.jit.count;
    if (s.jit.fault) {
      cpu.jit_fault(s.pc, s.jit.fault_address);
      return Result::Faulted;
    }
    return Result::Ran;
  }

 private:
  AArch64Frontend frontend_;
  JitCore core_;
};

}  // namespace

SharedPointer<IAArch64Jit> create_aarch64_jit(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options) {
  SharedPointer<AArch64JitEngine> engine = SharedPointer<AArch64JitEngine>::make(allocator, allocator, code_memory, options);
  if (!engine || !engine->ok()) return SharedPointer<IAArch64Jit>();
  return SharedPointer<IAArch64Jit>(engine);
}

}  // namespace emulation
