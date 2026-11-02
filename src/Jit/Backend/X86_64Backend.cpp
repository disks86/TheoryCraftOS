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
#include "Jit/Backend/X86_64Backend.hpp"
#include "Jit/Emitter/X86_64Assembler.hpp"
#include "Jit/Ir/RegisterAllocator.hpp"
#include "Utility/AllocatedView.hpp"

namespace jit {

namespace {

X86Cond to_x86(IrCond cond) {
  switch (cond) {
    case IrCond::Eq: return X86Cond::E;
    case IrCond::Ne: return X86Cond::Ne;
    case IrCond::Lt: return X86Cond::L;
    case IrCond::Le: return X86Cond::Le;
    case IrCond::Gt: return X86Cond::G;
    case IrCond::Ge: return X86Cond::Ge;
    case IrCond::Ltu: return X86Cond::B;
    case IrCond::Leu: return X86Cond::Be;
    case IrCond::Gtu: return X86Cond::A;
    case IrCond::Geu: return X86Cond::Ae;
  }
  return X86Cond::E;
}

// Pool register numbers: 0..3 are callee-saved (r12-r15), 4..5 caller-saved (r10, r11).
constexpr RegisterPool kPool = {4, 2};
X86Reg pool_reg(uint32_t index) {
  static const X86Reg regs[] = {X86Reg::R12, X86Reg::R13, X86Reg::R14, X86Reg::R15, X86Reg::R10, X86Reg::R11};
  return regs[index];
}

}  // namespace

bool X86_64Backend::compile(const IrBlock& block, CodeBuffer& out) {
  if (block.vreg_count > 0x0FFFFFFFu) return false;
  RegisterAllocation alloc(allocator_);
  if (!alloc.allocate(block, kPool, max_registers_)) return false;

  // The frame, from the entry rsp: [return address][saved rbp][saved rbx][saved callee-saved pool registers...][spill slots][padding][Win64 shadow space].
  // rbp points at the saved rbp. The padding keeps rsp 16-byte aligned at calls (entry rsp is 8 mod 16).
  uint32_t saved[4];
  uint32_t m = 0;
  for (uint32_t r = 0; r < kPool.callee_saved; ++r) {
    if (alloc.used_registers() >> r & 1) saved[m++] = r;
  }
  uint64_t frame = 8ull * alloc.spill_slots();
  if ((8ull * (1 + m) + frame) % 16 != 0) frame += 8;
  if (alloc.has_calls() && convention_ == X86CallingConvention::Win64) frame += 32;
  const int32_t slots_base = -16 - 8 * static_cast<int32_t>(m);
  auto slot_disp = [&](uint32_t slot) { return slots_base - 8 * static_cast<int32_t>(slot); };

  X86_64Assembler as(out);
  const X86Reg state_arg = convention_ == X86CallingConvention::Win64 ? X86Reg::Rcx : X86Reg::Rdi;
  const X86Reg arg0 = state_arg;
  const X86Reg arg1 = convention_ == X86CallingConvention::Win64 ? X86Reg::Rdx : X86Reg::Rsi;
  const X86Reg arg2 = convention_ == X86CallingConvention::Win64 ? X86Reg::R8 : X86Reg::Rdx;
  const X86Reg rax = X86Reg::Rax, rcx = X86Reg::Rcx, rbp = X86Reg::Rbp, rbx = X86Reg::Rbx;

  // Where a virtual register lives, and moves between it and a register.
  auto in_reg = [&](uint32_t v) { return alloc.location(v).kind == VRegLocation::Kind::Register; };
  auto reg_of = [&](uint32_t v) { return pool_reg(alloc.location(v).index); };
  auto disp_of = [&](uint32_t v) { return slot_disp(alloc.location(v).index); };
  // dst <- value of v
  auto load_into = [&](X86Reg dst, uint32_t v) {
    if (in_reg(v)) {
      if (reg_of(v) != dst) as.mov_rr(dst, reg_of(v));
    } else {
      as.mov_load(dst, rbp, disp_of(v));
    }
  };
  // A register holding the value of v: its own, or `scratch` after a load.
  auto read = [&](X86Reg scratch, uint32_t v) {
    if (in_reg(v)) return reg_of(v);
    as.mov_load(scratch, rbp, disp_of(v));
    return scratch;
  };
  // v <- src
  auto write = [&](uint32_t v, X86Reg src) {
    if (in_reg(v)) {
      if (reg_of(v) != src) as.mov_rr(reg_of(v), src);
    } else {
      as.mov_store(rbp, disp_of(v), src);
    }
  };
  // The register to compute v in: its own, or rax when it lives in memory (then finish with write()).
  auto target = [&](uint32_t v) { return in_reg(v) ? reg_of(v) : rax; };

  // One CodeBuffer label per IR label, and one for the shared epilogue.
  List<Label> labels(allocator_);
  for (uint32_t i = 0; i < block.label_count; ++i) {
    if (!labels.push_back(out.new_label())) return false;
  }
  const Label epilogue = out.new_label();

  as.push(rbp);
  as.mov_rr(rbp, X86Reg::Rsp);
  as.push(rbx);
  for (uint32_t i = 0; i < m; ++i) as.push(pool_reg(saved[i]));
  as.mov_rr(rbx, state_arg);
  if (frame) as.sub_ri(X86Reg::Rsp, static_cast<int32_t>(frame));
  // Registers read 0 until written.
  bool rax_zero = false;
  for (uint32_t v = 0; v < block.vreg_count; ++v) {
    if (!alloc.zero_at_entry(v) || alloc.location(v).kind == VRegLocation::Kind::Unused) continue;
    if (in_reg(v)) {
      as.xor_rr(reg_of(v), reg_of(v));
    } else {
      if (!rax_zero) as.xor_rr(rax, rax);
      rax_zero = true;
      as.mov_store(rbp, disp_of(v), rax);
    }
  }

  for (const IrInst& inst : block.insts.view()) {
    switch (inst.op) {
      case IrOp::Const: {
        const X86Reg d = target(inst.dst);
        as.mov_ri(d, static_cast<uint64_t>(inst.imm));
        write(inst.dst, d);
        break;
      }
      case IrOp::Move: {
        const X86Reg d = target(inst.dst);
        load_into(d, inst.a);
        write(inst.dst, d);
        break;
      }
      case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
      case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: {
        uint32_t a = inst.a, b = inst.b;
        const bool commutative = inst.op == IrOp::Add || inst.op == IrOp::Mul || inst.op == IrOp::And || inst.op == IrOp::Or || inst.op == IrOp::Xor;
        if (commutative && in_reg(inst.dst) && in_reg(b) && reg_of(b) == reg_of(inst.dst) && !(in_reg(a) && reg_of(a) == reg_of(inst.dst))) {
          const uint32_t t = a;   // dst shares b's register: swap so that the operand it overwrites is the one already in place
          a = b;
          b = t;
        }
        // Two-address form: compute in the destination's own register when no other operand lives there; otherwise in rax, which is always safe.
        const bool in_place = in_reg(inst.dst) && !(in_reg(b) && reg_of(b) == reg_of(inst.dst));
        const X86Reg d = in_place ? reg_of(inst.dst) : rax;
        load_into(d, a);
        const X86Reg rb = read(rcx, b);
        switch (inst.op) {
          case IrOp::Add: as.add_rr(d, rb); break;
          case IrOp::Sub: as.sub_rr(d, rb); break;
          case IrOp::Mul: as.imul_rr(d, rb); break;
          case IrOp::And: as.and_rr(d, rb); break;
          case IrOp::Or: as.or_rr(d, rb); break;
          case IrOp::Xor: as.xor_rr(d, rb); break;
          default:   // shifts take the count in cl (the CPU masks it to 6 bits, as the IR defines)
            if (rb != rcx) as.mov_rr(rcx, rb);
            if (inst.op == IrOp::Shl) as.shl_cl(d);
            else if (inst.op == IrOp::Shr) as.shr_cl(d);
            else as.sar_cl(d);
            break;
        }
        if (!in_place) write(inst.dst, rax);
        break;
      }
      case IrOp::Not: case IrOp::Neg: {
        const X86Reg d = target(inst.dst);
        load_into(d, inst.a);
        if (inst.op == IrOp::Not) as.not_(d);
        else as.neg(d);
        write(inst.dst, d);
        break;
      }
      case IrOp::SetCond: {
        const X86Reg a = read(rax, inst.a);
        const X86Reg b = read(rcx, inst.b);
        as.cmp_rr(a, b);
        as.setcc(to_x86(inst.cond), rax);
        write(inst.dst, rax);
        break;
      }
      case IrOp::LoadState: {
        if (inst.imm < 0 || inst.imm > INT32_MAX) return false;
        const X86Reg d = target(inst.dst);
        as.mov_load(d, rbx, static_cast<int32_t>(inst.imm));
        write(inst.dst, d);
        break;
      }
      case IrOp::StoreState: {
        if (inst.imm < 0 || inst.imm > INT32_MAX) return false;
        as.mov_store(rbx, static_cast<int32_t>(inst.imm), read(rax, inst.a));
        break;
      }
      case IrOp::Helper:
        as.mov_rr(arg0, rbx);
        load_into(arg1, inst.a);
        load_into(arg2, inst.b);
        as.mov_ri(rax, static_cast<uint64_t>(inst.imm));
        as.call_r(rax);
        write(inst.dst, rax);
        break;
      case IrOp::Label:
        if (!out.bind(labels[static_cast<uint64_t>(inst.imm)])) return false;
        break;
      case IrOp::Jump:
        as.jmp(labels[static_cast<uint64_t>(inst.imm)]);
        break;
      case IrOp::BranchIf: {
        const X86Reg a = read(rax, inst.a);
        const X86Reg b = read(rcx, inst.b);
        as.cmp_rr(a, b);
        as.jcc(to_x86(inst.cond), labels[static_cast<uint64_t>(inst.imm)]);
        break;
      }
      case IrOp::Exit:
        as.mov_ri(rax, static_cast<uint64_t>(inst.imm));
        as.jmp(epilogue);
        break;
      case IrOp::ExitValue:
        load_into(rax, inst.a);
        as.jmp(epilogue);
        break;
    }
  }

  if (!out.bind(epilogue)) return false;
  as.lea(X86Reg::Rsp, rbp, -8 * static_cast<int32_t>(1 + m));
  for (uint32_t i = m; i > 0; --i) as.pop(pool_reg(saved[i - 1]));
  as.pop(rbx);
  as.pop(rbp);
  as.ret();
  return out.finish();
}

}  // namespace jit
