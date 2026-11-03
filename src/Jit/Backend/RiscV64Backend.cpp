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
#include "Jit/Backend/RiscV64Backend.hpp"
#include "Jit/Emitter/RiscV64Assembler.hpp"
#include "Jit/Ir/RegisterAllocator.hpp"

namespace jit {

namespace {

constexpr RvReg kState = RvS1;   // the state pointer, for the whole block
constexpr RvReg kT0 = RvT0, kT1 = RvT1, kScratch = RvT2;

// A condition as a branch the CPU has: only Eq, Ne, Lt, Ge, Ltu, Geu exist, the others swap the operands.
struct Branch {
  RvBranch kind;
  bool swap;
};

Branch branch_for(IrCond cond) {
  switch (cond) {
    case IrCond::Eq: return {RvBranch::Eq, false};
    case IrCond::Ne: return {RvBranch::Ne, false};
    case IrCond::Lt: return {RvBranch::Lt, false};
    case IrCond::Ge: return {RvBranch::Ge, false};
    case IrCond::Gt: return {RvBranch::Lt, true};     // a > b  <=>  b < a
    case IrCond::Le: return {RvBranch::Ge, true};     // a <= b <=>  b >= a
    case IrCond::Ltu: return {RvBranch::Ltu, false};
    case IrCond::Geu: return {RvBranch::Geu, false};
    case IrCond::Gtu: return {RvBranch::Ltu, true};
    case IrCond::Leu: return {RvBranch::Geu, true};
  }
  return {RvBranch::Eq, false};
}

RvBranch negated(RvBranch kind) {
  switch (kind) {
    case RvBranch::Eq: return RvBranch::Ne;
    case RvBranch::Ne: return RvBranch::Eq;
    case RvBranch::Lt: return RvBranch::Ge;
    case RvBranch::Ge: return RvBranch::Lt;
    case RvBranch::Ltu: return RvBranch::Geu;
    case RvBranch::Geu: return RvBranch::Ltu;
  }
  return kind;
}

// Pool register numbers: 0..9 are callee-saved (s2-s11 = x18-x27), 10..18 caller-saved (t3-t6 = x28-x31, then a3-a7 = x13-x17).
constexpr RegisterPool kPool = {10, 9};
RvReg pool_reg(uint32_t index) {
  if (index < kPool.callee_saved) return static_cast<RvReg>(18 + index);
  const uint32_t k = index - kPool.callee_saved;
  return static_cast<RvReg>(k < 4 ? 28 + k : 13 + (k - 4));
}

}  // namespace

bool RiscV64Backend::compile(const IrBlock& block, CodeBuffer& out) {
  RegisterAllocation alloc(allocator_);
  if (!alloc.allocate(block, kPool, max_registers_)) return false;
  const uint64_t frame = (8ull * alloc.spill_slots() + 15) & ~15ull;   // sp stays 16-byte aligned

  RiscV64Assembler as(out);
  List<Label> labels(allocator_);
  for (uint32_t i = 0; i < block.label_count; ++i) {
    if (!labels.push_back(out.new_label())) return false;
  }
  const Label epilogue = out.new_label();

  // Memory at base + offset, with offsets beyond the 12-bit immediate reaching through t2.
  auto load = [&](RvReg rd, RvReg base, uint64_t offset) {
    if (offset <= 2040) return as.ld(rd, base, static_cast<int32_t>(offset));
    as.li(kScratch, static_cast<int64_t>(offset));
    as.add(kScratch, base, kScratch);
    return as.ld(rd, kScratch, 0);
  };
  auto store = [&](RvReg rs, RvReg base, uint64_t offset) {
    if (offset <= 2040) return as.sd(base, static_cast<int32_t>(offset), rs);
    as.li(kScratch, static_cast<int64_t>(offset));
    as.add(kScratch, base, kScratch);
    return as.sd(kScratch, 0, rs);
  };
  auto adjust_sp = [&](int64_t delta) {   // in steps the immediate can take
    while (delta != 0) {
      const int64_t step = delta > 2032 ? 2032 : delta < -2032 ? -2032 : delta;
      as.addi(RvSp, RvSp, static_cast<int32_t>(step));
      delta -= step;
    }
  };
  auto in_reg = [&](uint32_t v) { return alloc.location(v).kind == VRegLocation::Kind::Register; };
  auto reg_of = [&](uint32_t v) { return pool_reg(alloc.location(v).index); };
  auto off_of = [&](uint32_t v) { return 8ull * alloc.location(v).index; };
  auto load_into = [&](RvReg dst, uint32_t v) {
    if (in_reg(v)) {
      if (reg_of(v) != dst) as.mv(dst, reg_of(v));
    } else {
      load(dst, RvSp, off_of(v));
    }
  };
  auto read = [&](RvReg scratch, uint32_t v) {   // a register holding v: its own, or `scratch` after a load
    if (in_reg(v)) return reg_of(v);
    load(scratch, RvSp, off_of(v));
    return scratch;
  };
  auto write = [&](uint32_t v, RvReg src) {
    if (in_reg(v)) {
      if (reg_of(v) != src) as.mv(reg_of(v), src);
    } else {
      store(src, RvSp, off_of(v));
    }
  };
  auto target = [&](uint32_t v) { return in_reg(v) ? reg_of(v) : kT0; };

  // Save area: ra, s1 and the callee-saved pool registers in use.
  RvReg saved[10];
  uint32_t m = 0;
  for (uint32_t r = 0; r < kPool.callee_saved; ++r) {
    if (alloc.used_registers() >> r & 1) saved[m++] = pool_reg(r);
  }
  const int32_t save_bytes = static_cast<int32_t>((8 * (2 + m) + 15) & ~15u);

  as.addi(RvSp, RvSp, -save_bytes);
  as.sd(RvSp, 0, RvRa);
  as.sd(RvSp, 8, kState);
  for (uint32_t i = 0; i < m; ++i) as.sd(RvSp, static_cast<int32_t>(16 + 8 * i), saved[i]);
  as.mv(kState, RvA0);
  adjust_sp(-static_cast<int64_t>(frame));
  // Registers read 0 until written.
  for (uint32_t v = 0; v < block.vreg_count; ++v) {
    if (!alloc.zero_at_entry(v) || alloc.location(v).kind == VRegLocation::Kind::Unused) continue;
    if (in_reg(v)) as.mv(reg_of(v), RvZero);
    else store(RvZero, RvSp, off_of(v));
  }

  for (const IrInst& inst : block.insts.view()) {
    switch (inst.op) {
      case IrOp::Const: {
        const RvReg d = target(inst.dst);
        as.li(d, inst.imm);
        write(inst.dst, d);
        break;
      }
      case IrOp::Move: {
        const RvReg d = target(inst.dst);
        load_into(d, inst.a);
        write(inst.dst, d);
        break;
      }
      case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
      case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: {
        const RvReg a = read(kT0, inst.a), b = read(kT1, inst.b), d = target(inst.dst);
        switch (inst.op) {
          case IrOp::Add: as.add(d, a, b); break;
          case IrOp::Sub: as.sub(d, a, b); break;
          case IrOp::Mul: as.mul(d, a, b); break;
          case IrOp::And: as.and_(d, a, b); break;
          case IrOp::Or: as.or_(d, a, b); break;
          case IrOp::Xor: as.xor_(d, a, b); break;
          case IrOp::Shl: as.sll(d, a, b); break;   // (the variable shifts use the low 6 bits of the count, as the IR defines)
          case IrOp::Shr: as.srl(d, a, b); break;
          default: as.sra(d, a, b); break;
        }
        write(inst.dst, d);
        break;
      }
      case IrOp::Not: case IrOp::Neg: {
        const RvReg a = read(kT0, inst.a), d = target(inst.dst);
        if (inst.op == IrOp::Not) as.not_(d, a);
        else as.neg(d, a);
        write(inst.dst, d);
        break;
      }
      case IrOp::SetCond: {
        const RvReg a = read(kT0, inst.a), b = read(kT1, inst.b), d = target(inst.dst);
        switch (inst.cond) {
          case IrCond::Eq: as.sub(d, a, b); as.seqz(d, d); break;
          case IrCond::Ne: as.sub(d, a, b); as.sltu(d, RvZero, d); break;
          case IrCond::Lt: as.slt(d, a, b); break;
          case IrCond::Gt: as.slt(d, b, a); break;
          case IrCond::Ge: as.slt(d, a, b); as.xori(d, d, 1); break;
          case IrCond::Le: as.slt(d, b, a); as.xori(d, d, 1); break;
          case IrCond::Ltu: as.sltu(d, a, b); break;
          case IrCond::Gtu: as.sltu(d, b, a); break;
          case IrCond::Geu: as.sltu(d, a, b); as.xori(d, d, 1); break;
          case IrCond::Leu: as.sltu(d, b, a); as.xori(d, d, 1); break;
        }
        write(inst.dst, d);
        break;
      }
      case IrOp::LoadState: {
        if (inst.imm < 0) return false;
        const RvReg d = target(inst.dst);
        load(d, kState, static_cast<uint64_t>(inst.imm));
        write(inst.dst, d);
        break;
      }
      case IrOp::StoreState:
        if (inst.imm < 0) return false;
        store(read(kT0, inst.a), kState, static_cast<uint64_t>(inst.imm));
        break;
      case IrOp::Helper:
        load_into(RvA1, inst.a);
        load_into(RvA2, inst.b);
        as.mv(RvA0, kState);
        as.li(kScratch, inst.imm);
        as.jalr(RvRa, kScratch, 0);
        write(inst.dst, RvA0);
        break;
      case IrOp::LoadPtr: case IrOp::StorePtr: {
        // The address a + imm: the offset goes in the instruction when it fits, otherwise the sum is made in t2.
        const RvReg a = read(kT0, inst.a);
        RvReg address = a;
        int32_t offset = 0;
        if (inst.imm >= -2048 && inst.imm <= 2047) {
          offset = static_cast<int32_t>(inst.imm);
        } else {
          as.li(kScratch, inst.imm);
          as.add(kScratch, a, kScratch);
          address = kScratch;
        }
        if (inst.op == IrOp::StorePtr) {
          const RvReg v = read(kT1, inst.b);
          switch (inst.mem) {
            case IrMem::U8: as.sb(address, offset, v); break;
            case IrMem::U16: as.sh(address, offset, v); break;
            case IrMem::U32: as.sw(address, offset, v); break;
            case IrMem::U64: as.sd(address, offset, v); break;
            default: return false;
          }
          break;
        }
        const RvReg d = target(inst.dst);
        switch (inst.mem) {
          case IrMem::U8: as.lbu(d, address, offset); break;
          case IrMem::U16: as.lhu(d, address, offset); break;
          case IrMem::U32: as.lwu(d, address, offset); break;
          case IrMem::U64: as.ld(d, address, offset); break;
          case IrMem::S8: as.lb(d, address, offset); break;
          case IrMem::S16: as.lh(d, address, offset); break;
          case IrMem::S32: as.lw(d, address, offset); break;
        }
        write(inst.dst, d);
        break;
      }
      case IrOp::FAdd: case IrOp::FSub: case IrOp::FMul: case IrOp::FDiv: {
        if (!floating_point_) return false;
        as.fmv_d_x(0, read(kT0, inst.a));
        as.fmv_d_x(1, read(kT1, inst.b));
        if (inst.op == IrOp::FAdd) as.fadd_d(0, 0, 1);
        else if (inst.op == IrOp::FSub) as.fsub_d(0, 0, 1);
        else if (inst.op == IrOp::FMul) as.fmul_d(0, 0, 1);
        else as.fdiv_d(0, 0, 1);
        as.fmv_x_d(kT0, 0);
        write(inst.dst, kT0);
        break;
      }
      case IrOp::FSetCond: {
        if (!floating_point_) return false;
        as.fmv_d_x(0, read(kT0, inst.a));
        as.fmv_d_x(1, read(kT1, inst.b));
        const RvReg d = target(inst.dst);
        // feq / flt / fle give 0 when either operand is a NaN, which is what the IR wants for all but "not equal".
        switch (inst.cond) {
          case IrCond::Eq: as.feq_d(d, 0, 1); break;
          case IrCond::Ne: as.feq_d(d, 0, 1); as.xori(d, d, 1); break;
          case IrCond::Lt: as.flt_d(d, 0, 1); break;
          case IrCond::Le: as.fle_d(d, 0, 1); break;
          case IrCond::Gt: as.flt_d(d, 1, 0); break;
          case IrCond::Ge: as.fle_d(d, 1, 0); break;
          default: return false;
        }
        write(inst.dst, d);
        break;
      }
      case IrOp::I2F:
        if (!floating_point_) return false;
        as.fcvt_d_l(0, read(kT0, inst.a));
        as.fmv_x_d(kT0, 0);
        write(inst.dst, kT0);
        break;
      case IrOp::F2I: {
        if (!floating_point_) return false;
        as.fmv_d_x(0, read(kT0, inst.a));
        as.fcvt_l_d_rtz(kT0, 0);
        // A NaN and anything too big positive give INT64_MAX (which is not a double): the IR says INT64_MIN for those.
        const Label fits = out.new_label();
        as.li(kT1, INT64_MAX);
        as.branch(RvBranch::Ne, kT0, kT1, fits);
        as.li(kT0, INT64_MIN);
        if (!out.bind(fits)) return false;
        write(inst.dst, kT0);
        break;
      }
      case IrOp::F2D:
        if (!floating_point_) return false;
        as.fmv_w_x(0, read(kT0, inst.a));
        as.fcvt_d_s(0, 0);
        as.fmv_x_d(kT0, 0);
        write(inst.dst, kT0);
        break;
      case IrOp::D2F:
        if (!floating_point_) return false;
        as.fmv_d_x(0, read(kT0, inst.a));
        as.fcvt_s_d(0, 0);
        as.fmv_x_w(kT0, 0);
        as.slli(kT0, kT0, 32);   // (fmv.x.w sign extends: the IR wants the upper half zero)
        as.srli(kT0, kT0, 32);
        write(inst.dst, kT0);
        break;
      case IrOp::Label:
        if (!out.bind(labels[static_cast<uint64_t>(inst.imm)])) return false;
        break;
      case IrOp::Jump:
        as.j(labels[static_cast<uint64_t>(inst.imm)]);
        break;
      case IrOp::BranchIf: {
        // Conditional branches reach only +-4 KiB: branch over a JAL (+-1 MiB) when the condition does not hold.
        const Branch br = branch_for(inst.cond);
        const Label skip = out.new_label();
        const RvReg a = read(kT0, inst.a), b = read(kT1, inst.b);
        as.branch(negated(br.kind), br.swap ? b : a, br.swap ? a : b, skip);
        as.j(labels[static_cast<uint64_t>(inst.imm)]);
        if (!out.bind(skip)) return false;
        break;
      }
      case IrOp::Exit:
        as.li(RvA0, inst.imm);
        as.j(epilogue);
        break;
      case IrOp::ExitValue:
        load_into(RvA0, inst.a);
        as.j(epilogue);
        break;
    }
  }

  if (!out.bind(epilogue)) return false;
  adjust_sp(static_cast<int64_t>(frame));
  as.ld(RvRa, RvSp, 0);
  as.ld(kState, RvSp, 8);
  for (uint32_t i = 0; i < m; ++i) as.ld(saved[i], RvSp, static_cast<int32_t>(16 + 8 * i));
  as.addi(RvSp, RvSp, save_bytes);
  as.ret();
  return out.finish();
}

}  // namespace jit
