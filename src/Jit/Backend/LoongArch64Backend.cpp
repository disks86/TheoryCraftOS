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
#include "Jit/Backend/LoongArch64Backend.hpp"
#include "Jit/Emitter/LoongArch64Assembler.hpp"
#include "Jit/Ir/RegisterAllocator.hpp"

namespace jit {

namespace {

constexpr LaReg kState = LaS0;   // the state pointer, for the whole block
constexpr LaReg kT0 = LaT0, kT1 = LaT1, kScratch = LaT2;

// A condition as a branch the CPU has: only Eq, Ne, Lt, Ge, Ltu, Geu exist, the others swap the operands.
struct Branch {
  LaBranch kind;
  bool swap;
};

Branch branch_for(IrCond cond) {
  switch (cond) {
    case IrCond::Eq: return {LaBranch::Eq, false};
    case IrCond::Ne: return {LaBranch::Ne, false};
    case IrCond::Lt: return {LaBranch::Lt, false};
    case IrCond::Ge: return {LaBranch::Ge, false};
    case IrCond::Gt: return {LaBranch::Lt, true};     // a > b  <=>  b < a
    case IrCond::Le: return {LaBranch::Ge, true};     // a <= b <=>  b >= a
    case IrCond::Ltu: return {LaBranch::Ltu, false};
    case IrCond::Geu: return {LaBranch::Geu, false};
    case IrCond::Gtu: return {LaBranch::Ltu, true};
    case IrCond::Leu: return {LaBranch::Geu, true};
  }
  return {LaBranch::Eq, false};
}

LaBranch negated(LaBranch kind) {
  switch (kind) {
    case LaBranch::Eq: return LaBranch::Ne;
    case LaBranch::Ne: return LaBranch::Eq;
    case LaBranch::Lt: return LaBranch::Ge;
    case LaBranch::Ge: return LaBranch::Lt;
    case LaBranch::Ltu: return LaBranch::Geu;
    case LaBranch::Geu: return LaBranch::Ltu;
  }
  return kind;
}

// Pool register numbers: 0..7 are callee-saved (s1-s8 = r24-r31), 8..18 caller-saved (t3-t8 = r15-r20, then a3-a7 = r7-r11).
constexpr RegisterPool kPool = {8, 11};
LaReg pool_reg(uint32_t index) {
  if (index < kPool.callee_saved) return static_cast<LaReg>(24 + index);
  const uint32_t k = index - kPool.callee_saved;
  return static_cast<LaReg>(k < 6 ? 15 + k : 7 + (k - 6));
}

}  // namespace

bool LoongArch64Backend::compile(const IrBlock& block, CodeBuffer& out) {
  RegisterAllocation alloc(allocator_);
  if (!alloc.allocate(block, kPool, max_registers_)) return false;
  const uint64_t frame = (8ull * alloc.spill_slots() + 15) & ~15ull;   // sp stays 16-byte aligned

  LoongArch64Assembler as(out);
  List<Label> labels(allocator_);
  for (uint32_t i = 0; i < block.label_count; ++i) {
    if (!labels.push_back(out.new_label())) return false;
  }
  const Label epilogue = out.new_label();

  // Memory at base + offset, with offsets beyond the 12-bit immediate reaching through t2.
  auto load = [&](LaReg rd, LaReg base, uint64_t offset) {
    if (offset <= 2040) return as.ld(rd, base, static_cast<int32_t>(offset));
    as.li(kScratch, static_cast<int64_t>(offset));
    as.add(kScratch, base, kScratch);
    return as.ld(rd, kScratch, 0);
  };
  auto store = [&](LaReg rs, LaReg base, uint64_t offset) {
    if (offset <= 2040) return as.sd(base, static_cast<int32_t>(offset), rs);
    as.li(kScratch, static_cast<int64_t>(offset));
    as.add(kScratch, base, kScratch);
    return as.sd(kScratch, 0, rs);
  };
  auto adjust_sp = [&](int64_t delta) {   // in steps the immediate can take
    while (delta != 0) {
      const int64_t step = delta > 2032 ? 2032 : delta < -2032 ? -2032 : delta;
      as.addi(LaSp, LaSp, static_cast<int32_t>(step));
      delta -= step;
    }
  };
  auto in_reg = [&](uint32_t v) { return alloc.location(v).kind == VRegLocation::Kind::Register; };
  auto reg_of = [&](uint32_t v) { return pool_reg(alloc.location(v).index); };
  auto off_of = [&](uint32_t v) { return 8ull * alloc.location(v).index; };
  auto load_into = [&](LaReg dst, uint32_t v) {
    if (in_reg(v)) {
      if (reg_of(v) != dst) as.mv(dst, reg_of(v));
    } else {
      load(dst, LaSp, off_of(v));
    }
  };
  auto read = [&](LaReg scratch, uint32_t v) {   // a register holding v: its own, or `scratch` after a load
    if (in_reg(v)) return reg_of(v);
    load(scratch, LaSp, off_of(v));
    return scratch;
  };
  auto write = [&](uint32_t v, LaReg src) {
    if (in_reg(v)) {
      if (reg_of(v) != src) as.mv(reg_of(v), src);
    } else {
      store(src, LaSp, off_of(v));
    }
  };
  auto target = [&](uint32_t v) { return in_reg(v) ? reg_of(v) : kT0; };

  // Save area: ra, s0 and the callee-saved pool registers in use.
  LaReg saved[8];
  uint32_t m = 0;
  for (uint32_t r = 0; r < kPool.callee_saved; ++r) {
    if (alloc.used_registers() >> r & 1) saved[m++] = pool_reg(r);
  }
  const int32_t save_bytes = static_cast<int32_t>((8 * (2 + m) + 15) & ~15u);

  as.addi(LaSp, LaSp, -save_bytes);
  as.sd(LaSp, 0, LaRa);
  as.sd(LaSp, 8, kState);
  for (uint32_t i = 0; i < m; ++i) as.sd(LaSp, static_cast<int32_t>(16 + 8 * i), saved[i]);
  as.mv(kState, LaA0);
  adjust_sp(-static_cast<int64_t>(frame));
  // Registers read 0 until written.
  for (uint32_t v = 0; v < block.vreg_count; ++v) {
    if (!alloc.zero_at_entry(v) || alloc.location(v).kind == VRegLocation::Kind::Unused) continue;
    if (in_reg(v)) as.mv(reg_of(v), LaZero);
    else store(LaZero, LaSp, off_of(v));
  }

  for (const IrInst& inst : block.insts.view()) {
    switch (inst.op) {
      case IrOp::Const: {
        const LaReg d = target(inst.dst);
        as.li(d, inst.imm);
        write(inst.dst, d);
        break;
      }
      case IrOp::Move: {
        const LaReg d = target(inst.dst);
        load_into(d, inst.a);
        write(inst.dst, d);
        break;
      }
      case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
      case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: {
        const LaReg a = read(kT0, inst.a), b = read(kT1, inst.b), d = target(inst.dst);
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
        const LaReg a = read(kT0, inst.a), d = target(inst.dst);
        if (inst.op == IrOp::Not) as.not_(d, a);
        else as.neg(d, a);
        write(inst.dst, d);
        break;
      }
      case IrOp::SetCond: {
        const LaReg a = read(kT0, inst.a), b = read(kT1, inst.b), d = target(inst.dst);
        switch (inst.cond) {
          case IrCond::Eq: as.sub(d, a, b); as.seqz(d, d); break;
          case IrCond::Ne: as.sub(d, a, b); as.sltu(d, LaZero, d); break;
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
        const LaReg d = target(inst.dst);
        load(d, kState, static_cast<uint64_t>(inst.imm));
        write(inst.dst, d);
        break;
      }
      case IrOp::StoreState:
        if (inst.imm < 0) return false;
        store(read(kT0, inst.a), kState, static_cast<uint64_t>(inst.imm));
        break;
      case IrOp::Helper:
        load_into(LaA1, inst.a);
        load_into(LaA2, inst.b);
        as.mv(LaA0, kState);
        as.li(kScratch, inst.imm);
        as.jirl(LaRa, kScratch, 0);
        write(inst.dst, LaA0);
        break;
      case IrOp::LoadPtr: case IrOp::StorePtr: case IrOp::FAdd: case IrOp::FSub: case IrOp::FMul: case IrOp::FDiv: case IrOp::FSetCond: case IrOp::I2F: case IrOp::F2I: case IrOp::F2D: case IrOp::D2F:
        return false;   // (pointer access and floating point are not lowered for LoongArch yet: the block stays interpreted)
      case IrOp::Label:
        if (!out.bind(labels[static_cast<uint64_t>(inst.imm)])) return false;
        break;
      case IrOp::Jump:
        as.j(labels[static_cast<uint64_t>(inst.imm)]);
        break;
      case IrOp::BranchIf: {
        // Conditional branches reach only +-128 KiB: branch over a B (+-128 MiB) when the condition does not hold.
        const Branch br = branch_for(inst.cond);
        const Label skip = out.new_label();
        const LaReg a = read(kT0, inst.a), b = read(kT1, inst.b);
        as.branch(negated(br.kind), br.swap ? b : a, br.swap ? a : b, skip);
        as.j(labels[static_cast<uint64_t>(inst.imm)]);
        if (!out.bind(skip)) return false;
        break;
      }
      case IrOp::Exit:
        as.li(LaA0, inst.imm);
        as.j(epilogue);
        break;
      case IrOp::ExitValue:
        load_into(LaA0, inst.a);
        as.j(epilogue);
        break;
    }
  }

  if (!out.bind(epilogue)) return false;
  adjust_sp(static_cast<int64_t>(frame));
  as.ld(LaRa, LaSp, 0);
  as.ld(kState, LaSp, 8);
  for (uint32_t i = 0; i < m; ++i) as.ld(saved[i], LaSp, static_cast<int32_t>(16 + 8 * i));
  as.addi(LaSp, LaSp, save_bytes);
  as.ret();
  return out.finish();
}

}  // namespace jit
