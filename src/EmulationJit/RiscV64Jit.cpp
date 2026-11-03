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
#include "EmulationJit/RiscV64Jit.hpp"
#include "EmulationJit/JitCore.hpp"
#include "EmulationJit/JitMemoryHelpers.hpp"

namespace emulation {

namespace {

using State = RiscV64Cpu::State;
using jit::IrCond;
using jit::IrOp;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(__builtin_offsetof(State, x) == 0, "translated code addresses the registers from the start of the state");
constexpr uint64_t kOffPc = __builtin_offsetof(State, pc);
constexpr uint64_t kOffFault = __builtin_offsetof(State, jit.fault);
constexpr uint64_t kOffCount = __builtin_offsetof(State, jit.count);
constexpr uint64_t kOffEnd = __builtin_offsetof(State, jit.cpu) + 8;
#pragma GCC diagnostic pop
static_assert(kOffEnd < 2040, "the back ends reach the state with short displacements");

constexpr uint32_t kMaxBlock = static_cast<uint32_t>(IRiscVJit::kLongestBlock);

constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t bit(uint32_t v, uint32_t i) { return (v >> i) & 1; }
constexpr uint32_t field(uint32_t v, uint32_t hi, uint32_t lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

// One translation: builds the IR of the block at `pc`. The registers x1..x31 are loaded on first use into one virtual register each and written back at every exit.
class Translation {
 public:
  Translation(jit::IrBuilder& builder, GuestMemory& memory, uint64_t pc, uint32_t max_block) : b_(builder), mem_(memory), start_(pc), max_block_(max_block) {}
  bool run(uint64_t* guest_size);

 private:
  struct Stub {
    uint32_t label = 0;
    uint64_t pc = 0, count = 0;
    uint32_t dirty = 0;
    bool used = false;
  };

  uint32_t K(int64_t v) { return b_.constant(v); }
  uint32_t op2(IrOp op, uint32_t a, uint32_t c) { return b_.binary(op, a, c); }
  uint32_t sext32(uint32_t v) { return op2(IrOp::Sar, op2(IrOp::Shl, v, K(32)), K(32)); }
  uint32_t zext32(uint32_t v) { return op2(IrOp::And, v, K(0xFFFFFFFF)); }
  uint32_t X(uint32_t r) {
    if (r == 0) return zero_;
    if (!loaded_[r]) {
      x_[r] = b_.load_state(r * 8);
      loaded_[r] = true;
    }
    return x_[r];
  }
  void W(uint32_t r, uint32_t v) {
    if (r == 0) return;
    if (!loaded_[r]) {
      x_[r] = b_.new_vreg();
      loaded_[r] = true;
    }
    b_.assign(x_[r], v);
    dirty_ |= 1u << r;
  }
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
    const uint32_t v = b_.helper(load_helper<State>(size), address, zero_);
    check_fault();
    return v;
  }
  void store(uint32_t address, uint32_t size, uint32_t value) {
    b_.helper(store_helper<State>(size), address, value);
    check_fault();
  }
  uint32_t address(uint32_t rs1, int64_t offset) { return offset == 0 ? X(rs1) : op2(IrOp::Add, X(rs1), K(offset)); }

  void write_back(uint32_t dirty) {
    for (uint32_t r = 1; r < 32; ++r)
      if (dirty & (1u << r)) b_.store_state(r * 8, x_[r]);
  }
  void finish_to(uint64_t target, uint64_t count) {
    write_back(dirty_);
    b_.store_state(kOffPc, K(static_cast<int64_t>(target)));
    b_.store_state(kOffCount, K(static_cast<int64_t>(count)));
    b_.exit(0);
  }
  void finish_to_value(uint32_t target, uint64_t count) {
    write_back(dirty_);
    b_.store_state(kOffPc, target);
    b_.store_state(kOffCount, K(static_cast<int64_t>(count)));
    b_.exit(0);
  }
  void emit_stubs() {
    for (uint32_t s = 0; s < stub_count_; ++s) {
      const Stub& st = stubs_[s];
      b_.place_label(st.label);
      write_back(st.dirty);
      b_.store_state(kOffPc, K(static_cast<int64_t>(st.pc)));
      b_.store_state(kOffCount, K(static_cast<int64_t>(st.count)));
      b_.exit(0);
    }
  }
  bool translate_insn(uint32_t insn, uint64_t pc, uint32_t length, uint64_t count, bool* ended);

  jit::IrBuilder& b_;
  GuestMemory& mem_;
  uint64_t start_;
  uint32_t max_block_;
  uint32_t zero_ = 0;
  uint32_t x_[32] = {};
  bool loaded_[32] = {};
  uint32_t dirty_ = 0;
  Stub stub_;
  Stub stubs_[kMaxBlock + 1];
  uint32_t stub_count_ = 0;
};

bool Translation::translate_insn(uint32_t insn, uint64_t pc, uint32_t length, uint64_t count, bool* ended) {
  const uint32_t opcode = insn & 0x7F, rd = field(insn, 11, 7), funct3 = field(insn, 14, 12), rs1 = field(insn, 19, 15), rs2 = field(insn, 24, 20), funct7 = insn >> 25;
  const int64_t imm_i = sext(insn >> 20, 12);
  const uint64_t next = pc + length;
  stub_ = Stub();
  stub_.pc = pc;
  stub_.count = count;
  stub_.dirty = dirty_;

  switch (opcode) {
    case 0x37:      // lui
      W(rd, K(sext(insn & 0xFFFFF000u, 32)));
      return true;
    case 0x17:      // auipc
      W(rd, K(static_cast<int64_t>(pc + static_cast<uint64_t>(sext(insn & 0xFFFFF000u, 32)))));
      return true;
    case 0x6F: {      // jal
      const int64_t off = sext((bit(insn, 31) << 20) | (field(insn, 19, 12) << 12) | (bit(insn, 20) << 11) | (field(insn, 30, 21) << 1), 21);
      W(rd, K(static_cast<int64_t>(next)));
      *ended = true;
      finish_to(pc + static_cast<uint64_t>(off), count + 1);
      return true;
    }
    case 0x67: {      // jalr
      if (funct3 != 0) return false;
      const uint32_t target = op2(IrOp::And, op2(IrOp::Add, X(rs1), K(imm_i)), K(~int64_t{1}));      // (before rd is written: rd may be rs1)
      W(rd, K(static_cast<int64_t>(next)));
      *ended = true;
      finish_to_value(target, count + 1);
      return true;
    }
    case 0x63: {      // branches
      IrCond cond;
      switch (funct3) {
        case 0: cond = IrCond::Eq; break;
        case 1: cond = IrCond::Ne; break;
        case 4: cond = IrCond::Lt; break;
        case 5: cond = IrCond::Ge; break;
        case 6: cond = IrCond::Ltu; break;
        case 7: cond = IrCond::Geu; break;
        default: return false;
      }
      const int64_t off = sext((bit(insn, 31) << 12) | (bit(insn, 7) << 11) | (field(insn, 30, 25) << 5) | (field(insn, 11, 8) << 1), 13);
      const uint32_t taken = b_.new_label();
      b_.branch_if(cond, X(rs1), X(rs2), taken);
      *ended = true;
      finish_to(next, count + 1);
      b_.place_label(taken);
      finish_to(pc + static_cast<uint64_t>(off), count + 1);
      return true;
    }
    case 0x03: {      // loads (a load into x0 still faults)
      static const uint32_t sizes[8] = {1, 2, 4, 8, 1, 2, 4, 0};
      const uint32_t size = sizes[funct3];
      if (size == 0) return false;
      uint32_t v = load(address(rs1, imm_i), size);
      if (funct3 < 4 && size < 8) {
        const int64_t n = 64 - 8 * static_cast<int64_t>(size);
        v = op2(IrOp::Sar, op2(IrOp::Shl, v, K(n)), K(n));
      }
      W(rd, v);
      return true;
    }
    case 0x23: {      // stores
      if (funct3 > 3) return false;
      const int64_t off = sext((funct7 << 5) | rd, 12);
      store(address(rs1, off), 1u << funct3, X(rs2));
      return true;
    }
    case 0x13: {      // OP-IMM
      const uint32_t shamt = field(insn, 25, 20);
      uint32_t r;
      switch (funct3) {
        case 0: r = op2(IrOp::Add, X(rs1), K(imm_i)); break;
        case 2: r = b_.set_cond(IrCond::Lt, X(rs1), K(imm_i)); break;
        case 3: r = b_.set_cond(IrCond::Ltu, X(rs1), K(imm_i)); break;
        case 4: r = op2(IrOp::Xor, X(rs1), K(imm_i)); break;
        case 6: r = op2(IrOp::Or, X(rs1), K(imm_i)); break;
        case 7: r = op2(IrOp::And, X(rs1), K(imm_i)); break;
        case 1:
          if ((insn >> 26) != 0) return false;      // (bit manipulation encodings are the interpreter's)
          r = op2(IrOp::Shl, X(rs1), K(shamt));
          break;
        default:
          if ((insn >> 26) == 0) r = op2(IrOp::Shr, X(rs1), K(shamt));
          else if ((insn >> 26) == 0x10) r = op2(IrOp::Sar, X(rs1), K(shamt));
          else return false;
          break;
      }
      W(rd, r);
      return true;
    }
    case 0x1B: {      // OP-IMM-32
      const uint32_t shamt = field(insn, 24, 20);
      uint32_t r;
      switch (funct3) {
        case 0: r = sext32(op2(IrOp::Add, X(rs1), K(imm_i))); break;
        case 1:
          if (funct7 != 0) return false;
          r = sext32(op2(IrOp::Shl, X(rs1), K(shamt)));
          break;
        case 5:
          if (funct7 == 0) r = sext32(op2(IrOp::Shr, zext32(X(rs1)), K(shamt)));
          else if (funct7 == 0x20) r = op2(IrOp::Sar, sext32(X(rs1)), K(shamt));
          else return false;
          break;
        default: return false;
      }
      W(rd, r);
      return true;
    }
    case 0x33: {      // OP
      uint32_t r;
      if (funct7 == 1) {
        if (funct3 != 0) return false;      // (mulh*, div and rem are interpreted)
        r = op2(IrOp::Mul, X(rs1), X(rs2));
      } else if (funct7 == 0 || funct7 == 0x20) {
        const bool alt = funct7 == 0x20;
        switch (funct3) {
          case 0: r = op2(alt ? IrOp::Sub : IrOp::Add, X(rs1), X(rs2)); break;
          case 5: r = op2(alt ? IrOp::Sar : IrOp::Shr, X(rs1), X(rs2)); break;
          default:
            if (alt) return false;
            switch (funct3) {
              case 1: r = op2(IrOp::Shl, X(rs1), X(rs2)); break;
              case 2: r = b_.set_cond(IrCond::Lt, X(rs1), X(rs2)); break;
              case 3: r = b_.set_cond(IrCond::Ltu, X(rs1), X(rs2)); break;
              case 4: r = op2(IrOp::Xor, X(rs1), X(rs2)); break;
              case 6: r = op2(IrOp::Or, X(rs1), X(rs2)); break;
              default: r = op2(IrOp::And, X(rs1), X(rs2)); break;
            }
            break;
        }
      } else {
        return false;
      }
      W(rd, r);
      return true;
    }
    case 0x3B: {      // OP-32
      uint32_t r;
      if (funct7 == 1) {
        if (funct3 != 0) return false;
        r = sext32(op2(IrOp::Mul, X(rs1), X(rs2)));
      } else if (funct7 == 0 || funct7 == 0x20) {
        const bool alt = funct7 == 0x20;
        switch (funct3) {
          case 0: r = sext32(op2(alt ? IrOp::Sub : IrOp::Add, X(rs1), X(rs2))); break;
          case 1:
            if (alt) return false;
            r = sext32(op2(IrOp::Shl, X(rs1), op2(IrOp::And, X(rs2), K(31))));
            break;
          case 5: {
            const uint32_t amount = op2(IrOp::And, X(rs2), K(31));
            r = alt ? op2(IrOp::Sar, sext32(X(rs1)), amount) : sext32(op2(IrOp::Shr, zext32(X(rs1)), amount));
            break;
          }
          default: return false;
        }
      } else {
        return false;
      }
      W(rd, r);
      return true;
    }
    case 0x0F:      // fence, fence.i: nothing to do here (the cache-block operations are the interpreter's)
      return funct3 <= 1;
    default:
      return false;
  }
}

bool Translation::run(uint64_t* guest_size) {
  zero_ = K(0);
  uint64_t pc = start_;
  uint32_t count = 0;
  bool ended = false;
  while (!ended && count < max_block_) {
    if (pc & 1) break;
    uint8_t raw[4];
    const uint64_t available = mem_.fetchable(pc, 4);
    if (available < 2 || !mem_.fetch(pc, ByteView(raw, available < 4 ? 2 : 4))) break;
    uint32_t insn = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8);
    uint32_t length = 2;
    if ((insn & 3) == 3) {
      if (available < 4) break;
      insn |= (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
      length = 4;
    } else {
      insn = riscv_expand_compressed(insn);
      if (insn == 0) break;
    }
    // Code the guest can write to is never translated: it could change under the cache.
    if (mem_.writable(pc, 1) || mem_.writable(pc + length - 1, 1)) break;
    if (!translate_insn(insn, pc, length, count, &ended)) break;
    pc += length;
    ++count;
  }
  if (count == 0) return false;
  if (!ended) finish_to(pc, count);
  emit_stubs();
  b_.set_state_size(kOffEnd);
  *guest_size = pc - start_;
  return true;
}

class RiscV64Frontend final : public jit::IJitFrontend {
 public:
  explicit RiscV64Frontend(uint32_t max_block) : max_block_(max_block < 1 ? 1 : max_block > kMaxBlock ? kMaxBlock : max_block) {}
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

class RiscV64JitEngine final : public IRiscVJit {
 public:
  RiscV64JitEngine(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options)
      : frontend_(options.max_block), core_(allocator, code_memory, options.hot_threshold, frontend_) {}
  bool ok() const { return core_.ok(); }
  uint64_t blocks_entered() const override { return core_.blocks_entered(); }

  Result run(RiscV64Cpu& cpu, GuestMemory& memory, uint64_t budget, uint64_t* retired) override {
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
  RiscV64Frontend frontend_;
  JitCore core_;
};

}  // namespace

SharedPointer<IRiscVJit> create_riscv64_jit(allocators::IAllocator& allocator, jit::ICodeMemory& code_memory, const GuestJitOptions& options) {
  SharedPointer<RiscV64JitEngine> engine = SharedPointer<RiscV64JitEngine>::make(allocator, allocator, code_memory, options);
  if (!engine || !engine->ok()) return SharedPointer<IRiscVJit>();
  return SharedPointer<IRiscVJit>(engine);
}

}  // namespace emulation
