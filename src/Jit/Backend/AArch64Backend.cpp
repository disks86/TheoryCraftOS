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
#include "Jit/Backend/AArch64Backend.hpp"
#include "Jit/Emitter/AArch64Assembler.hpp"
#include "Jit/Ir/RegisterAllocator.hpp"

namespace jit {

namespace {

A64Cond to_a64(IrCond cond) {
  switch (cond) {
    case IrCond::Eq: return A64Cond::Eq;
    case IrCond::Ne: return A64Cond::Ne;
    case IrCond::Lt: return A64Cond::Lt;
    case IrCond::Le: return A64Cond::Le;
    case IrCond::Gt: return A64Cond::Gt;
    case IrCond::Ge: return A64Cond::Ge;
    case IrCond::Ltu: return A64Cond::Lo;
    case IrCond::Leu: return A64Cond::Ls;
    case IrCond::Gtu: return A64Cond::Hi;
    case IrCond::Geu: return A64Cond::Hs;
  }
  return A64Cond::Eq;
}

// The condition with the opposite meaning (the encodings come in pairs that differ in the lowest bit).
A64Cond inverse(A64Cond c) { return static_cast<A64Cond>(static_cast<uint8_t>(c) ^ 1); }

constexpr A64Reg kState = 19;     // x19: the state pointer, for the whole block
constexpr A64Reg kScratch = 16;   // x16 (IP0): large offsets and the helper address
constexpr uint32_t kMaxSpills = 4095;

// Pool register numbers: 0..8 are callee-saved (x20-x28), 9..15 caller-saved (x9-x15).
constexpr RegisterPool kPool = {9, 7};
A64Reg pool_reg(uint32_t index) { return static_cast<A64Reg>(index < kPool.callee_saved ? 20 + index : 9 + (index - kPool.callee_saved)); }

}  // namespace

bool AArch64Backend::compile(const IrBlock& block, CodeBuffer& out) {
  RegisterAllocation alloc(allocator_);
  if (!alloc.allocate(block, kPool, max_registers_) || alloc.spill_slots() > kMaxSpills) return false;
  const uint32_t frame = (8 * alloc.spill_slots() + 15) & ~15u;   // sp stays 16-byte aligned

  AArch64Assembler as(out);
  auto slot_offset = [](uint32_t slot) { return 8 * slot; };
  auto in_reg = [&](uint32_t v) { return alloc.location(v).kind == VRegLocation::Kind::Register; };
  auto reg_of = [&](uint32_t v) { return pool_reg(alloc.location(v).index); };
  auto off_of = [&](uint32_t v) { return slot_offset(alloc.location(v).index); };
  auto load_into = [&](A64Reg dst, uint32_t v) {
    if (in_reg(v)) {
      if (reg_of(v) != dst) as.mov_rr(dst, reg_of(v));
    } else {
      as.ldr(dst, A64Sp, off_of(v));
    }
  };
  auto read = [&](A64Reg scratch, uint32_t v) {   // a register holding v: its own, or `scratch` after a load
    if (in_reg(v)) return reg_of(v);
    as.ldr(scratch, A64Sp, off_of(v));
    return scratch;
  };
  auto write = [&](uint32_t v, A64Reg src) {
    if (in_reg(v)) {
      if (reg_of(v) != src) as.mov_rr(reg_of(v), src);
    } else {
      as.str(src, A64Sp, off_of(v));
    }
  };
  auto target = [&](uint32_t v) { return in_reg(v) ? reg_of(v) : A64Reg{0}; };

  List<Label> labels(allocator_);
  for (uint32_t i = 0; i < block.label_count; ++i) {
    if (!labels.push_back(out.new_label())) return false;
  }
  const Label epilogue = out.new_label();

  // Registers to save: x19 and the callee-saved pool registers in use, in pairs (an odd one is paired with xzr).
  A64Reg saved[1 + 9 + 1];
  uint32_t m = 0;
  saved[m++] = kState;
  for (uint32_t r = 0; r < kPool.callee_saved; ++r) {
    if (alloc.used_registers() >> r & 1) saved[m++] = pool_reg(r);
  }
  if (m % 2) saved[m++] = A64Zr;

  as.stp_push(A64Fp, A64Lr, 16);
  as.add_ri(A64Fp, A64Sp, 0);
  for (uint32_t i = 0; i < m; i += 2) as.stp_push(saved[i], saved[i + 1], 16);
  as.mov_rr(kState, 0);
  for (uint32_t left = frame; left;) {
    const uint32_t chunk = left > 4080 ? 4080 : left;
    as.sub_ri(A64Sp, A64Sp, chunk);
    left -= chunk;
  }
  // Registers read 0 until written.
  for (uint32_t v = 0; v < block.vreg_count; ++v) {
    if (!alloc.zero_at_entry(v) || alloc.location(v).kind == VRegLocation::Kind::Unused) continue;
    if (in_reg(v)) as.mov_rr(reg_of(v), A64Zr);
    else as.str(A64Zr, A64Sp, off_of(v));
  }

  // State word `offset`, reached through x19 or, when the offset is beyond the immediate, through x16.
  auto state_load = [&](A64Reg rt, uint64_t offset) {
    if (offset <= 32760) return as.ldr(rt, kState, static_cast<uint32_t>(offset));
    as.mov_imm(kScratch, offset);
    as.add_rr(kScratch, kState, kScratch);
    return as.ldr(rt, kScratch, 0);
  };
  auto state_store = [&](A64Reg rt, uint64_t offset) {
    if (offset <= 32760) return as.str(rt, kState, static_cast<uint32_t>(offset));
    as.mov_imm(kScratch, offset);
    as.add_rr(kScratch, kState, kScratch);
    return as.str(rt, kScratch, 0);
  };

  for (const IrInst& inst : block.insts.view()) {
    switch (inst.op) {
      case IrOp::Const: {
        const A64Reg d = target(inst.dst);
        as.mov_imm(d, static_cast<uint64_t>(inst.imm));
        write(inst.dst, d);
        break;
      }
      case IrOp::Move: {
        const A64Reg d = target(inst.dst);
        load_into(d, inst.a);
        write(inst.dst, d);
        break;
      }
      case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
      case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: {
        const A64Reg a = read(0, inst.a), b = read(1, inst.b), d = target(inst.dst);
        switch (inst.op) {
          case IrOp::Add: as.add_rr(d, a, b); break;
          case IrOp::Sub: as.sub_rr(d, a, b); break;
          case IrOp::Mul: as.mul(d, a, b); break;
          case IrOp::And: as.and_rr(d, a, b); break;
          case IrOp::Or: as.orr_rr(d, a, b); break;
          case IrOp::Xor: as.eor_rr(d, a, b); break;
          case IrOp::Shl: as.lsl_rr(d, a, b); break;   // (the variable shifts take the count modulo 64, as the IR defines)
          case IrOp::Shr: as.lsr_rr(d, a, b); break;
          default: as.asr_rr(d, a, b); break;
        }
        write(inst.dst, d);
        break;
      }
      case IrOp::Not: case IrOp::Neg: {
        const A64Reg a = read(0, inst.a), d = target(inst.dst);
        if (inst.op == IrOp::Not) as.mvn(d, a);
        else as.neg(d, a);
        write(inst.dst, d);
        break;
      }
      case IrOp::SetCond: {
        const A64Reg a = read(0, inst.a), b = read(1, inst.b), d = target(inst.dst);
        as.cmp_rr(a, b);
        as.cset(d, to_a64(inst.cond));
        write(inst.dst, d);
        break;
      }
      case IrOp::LoadState: {
        if (inst.imm < 0) return false;
        const A64Reg d = target(inst.dst);
        state_load(d, static_cast<uint64_t>(inst.imm));
        write(inst.dst, d);
        break;
      }
      case IrOp::StoreState:
        if (inst.imm < 0) return false;
        state_store(read(0, inst.a), static_cast<uint64_t>(inst.imm));
        break;
      case IrOp::Helper:
        load_into(1, inst.a);
        load_into(2, inst.b);
        as.mov_rr(0, kState);
        as.mov_imm(kScratch, static_cast<uint64_t>(inst.imm));
        as.blr(kScratch);
        write(inst.dst, 0);
        break;
      case IrOp::LoadPtr: case IrOp::StorePtr: {
        // The address a + imm goes to x16 (x17 holds a large offset); the access itself has no offset.
        const A64Reg a = read(0, inst.a);
        A64Reg address = a;
        if (inst.imm != 0) {
          as.mov_imm(17, static_cast<uint64_t>(inst.imm));
          as.add_rr(kScratch, a, 17);
          address = kScratch;
        }
        if (inst.op == IrOp::StorePtr) {
          const A64Reg v = read(1, inst.b);
          switch (inst.mem) {
            case IrMem::U8: as.strb(v, address); break;
            case IrMem::U16: as.strh(v, address); break;
            case IrMem::U32: as.str_w(v, address); break;
            case IrMem::U64: as.str(v, address, 0); break;
            default: return false;
          }
          break;
        }
        const A64Reg d = target(inst.dst);
        switch (inst.mem) {
          case IrMem::U8: as.ldrb(d, address); break;
          case IrMem::U16: as.ldrh(d, address); break;
          case IrMem::U32: as.ldr_w(d, address); break;
          case IrMem::U64: as.ldr(d, address, 0); break;
          case IrMem::S8: as.ldrsb(d, address); break;
          case IrMem::S16: as.ldrsh(d, address); break;
          case IrMem::S32: as.ldrsw(d, address); break;
        }
        write(inst.dst, d);
        break;
      }
      case IrOp::FAdd: case IrOp::FSub: case IrOp::FMul: case IrOp::FDiv: {
        if (!floating_point_) return false;
        as.fmov_to_d(0, read(0, inst.a));
        as.fmov_to_d(1, read(1, inst.b));
        if (inst.op == IrOp::FAdd) as.fadd(0, 0, 1);
        else if (inst.op == IrOp::FSub) as.fsub(0, 0, 1);
        else if (inst.op == IrOp::FMul) as.fmul(0, 0, 1);
        else as.fdiv(0, 0, 1);
        as.fmov_from_d(0, 0);
        write(inst.dst, 0);
        break;
      }
      case IrOp::FSetCond: {
        if (!floating_point_) return false;
        as.fmov_to_d(0, read(0, inst.a));
        as.fmov_to_d(1, read(1, inst.b));
        as.fcmp(0, 1);
        // fcmp of a NaN sets N=0 Z=0 C=1 V=1, which makes each of these false (and "not equal" true), as the IR defines.
        A64Cond cond = A64Cond::Eq;
        switch (inst.cond) {
          case IrCond::Eq: cond = A64Cond::Eq; break;
          case IrCond::Ne: cond = A64Cond::Ne; break;
          case IrCond::Lt: cond = A64Cond::Mi; break;
          case IrCond::Le: cond = A64Cond::Ls; break;
          case IrCond::Gt: cond = A64Cond::Gt; break;
          case IrCond::Ge: cond = A64Cond::Ge; break;
          default: return false;
        }
        const A64Reg d = target(inst.dst);
        as.cset(d, cond);
        write(inst.dst, d);
        break;
      }
      case IrOp::I2F:
        if (!floating_point_) return false;
        as.scvtf(0, read(0, inst.a));
        as.fmov_from_d(0, 0);
        write(inst.dst, 0);
        break;
      case IrOp::F2I: {
        if (!floating_point_) return false;
        as.fmov_to_d(0, read(0, inst.a));
        as.fcvtzs(0, 0);
        // fcvtzs saturates and turns a NaN into 0; the IR says INT64_MIN for a NaN and for anything that does not fit (2^63-1 is not a double, so a result of INT64_MAX
        // is an overflow).
        const Label ordered = out.new_label(), fits = out.new_label();
        as.fcmp(0, 0);   // (d0 still holds the operand)
        as.b_cond(A64Cond::Vc, ordered);
        as.mov_imm(0, static_cast<uint64_t>(INT64_MIN));
        if (!out.bind(ordered)) return false;
        as.mov_imm(1, static_cast<uint64_t>(INT64_MAX));
        as.cmp_rr(0, 1);
        as.b_cond(A64Cond::Ne, fits);
        as.mov_imm(0, static_cast<uint64_t>(INT64_MIN));
        if (!out.bind(fits)) return false;
        write(inst.dst, 0);
        break;
      }
      case IrOp::F2D:
        if (!floating_point_) return false;
        as.fmov_to_s(0, read(0, inst.a));
        as.fcvt_s_to_d(0, 0);
        as.fmov_from_d(0, 0);
        write(inst.dst, 0);
        break;
      case IrOp::D2F:
        if (!floating_point_) return false;
        as.fmov_to_d(0, read(0, inst.a));
        as.fcvt_d_to_s(0, 0);
        as.fmov_from_s(0, 0);
        write(inst.dst, 0);
        break;
      case IrOp::Label:
        if (!out.bind(labels[static_cast<uint64_t>(inst.imm)])) return false;
        break;
      case IrOp::Jump:
        as.b(labels[static_cast<uint64_t>(inst.imm)]);
        break;
      case IrOp::BranchIf: {
        // B.cond reaches only +-1 MiB; "skip the jump if the condition is false" followed by an unconditional B reaches +-128 MiB.
        const Label skip = out.new_label();
        const A64Reg a = read(0, inst.a), b = read(1, inst.b);
        as.cmp_rr(a, b);
        as.b_cond(inverse(to_a64(inst.cond)), skip);
        as.b(labels[static_cast<uint64_t>(inst.imm)]);
        if (!out.bind(skip)) return false;
        break;
      }
      case IrOp::Exit:
        as.mov_imm(0, static_cast<uint64_t>(inst.imm));
        as.b(epilogue);
        break;
      case IrOp::ExitValue:
        load_into(0, inst.a);
        as.b(epilogue);
        break;
    }
  }

  if (!out.bind(epilogue)) return false;
  for (uint32_t left = frame; left;) {
    const uint32_t chunk = left > 4080 ? 4080 : left;
    as.add_ri(A64Sp, A64Sp, chunk);
    left -= chunk;
  }
  for (uint32_t i = m; i > 0; i -= 2) as.ldp_pop(saved[i - 2], saved[i - 1], 16);
  as.ldp_pop(A64Fp, A64Lr, 16);
  as.ret();
  return out.finish();
}

}  // namespace jit
