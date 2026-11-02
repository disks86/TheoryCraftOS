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
#include "Jit/Ir/Ir.hpp"
#include "Utility/AllocatedView.hpp"

namespace jit {

bool eval_cond(IrCond cond, uint64_t a, uint64_t b) {
  const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
  switch (cond) {
    case IrCond::Eq: return a == b;
    case IrCond::Ne: return a != b;
    case IrCond::Lt: return sa < sb;
    case IrCond::Le: return sa <= sb;
    case IrCond::Gt: return sa > sb;
    case IrCond::Ge: return sa >= sb;
    case IrCond::Ltu: return a < b;
    case IrCond::Leu: return a <= b;
    case IrCond::Gtu: return a > b;
    case IrCond::Geu: return a >= b;
  }
  return false;
}

namespace {

// The inverse of IrBuilder::helper_address: the only other place that turns a number back into a function pointer.
IrHelper helper_from_address(int64_t address) { return reinterpret_cast<IrHelper>(static_cast<uintptr_t>(address)); }

bool reads_a_impl(IrOp op) {
  switch (op) {
    case IrOp::Move: case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
    case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: case IrOp::Not: case IrOp::Neg: case IrOp::SetCond:
    case IrOp::StoreState: case IrOp::Helper: case IrOp::BranchIf: case IrOp::ExitValue:
      return true;
    default: return false;
  }
}
bool reads_b_impl(IrOp op) {
  switch (op) {
    case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
    case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: case IrOp::SetCond: case IrOp::Helper: case IrOp::BranchIf:
      return true;
    default: return false;
  }
}
bool writes_dst_impl(IrOp op) {
  switch (op) {
    case IrOp::Const: case IrOp::Move: case IrOp::Add: case IrOp::Sub: case IrOp::Mul: case IrOp::And: case IrOp::Or: case IrOp::Xor:
    case IrOp::Shl: case IrOp::Shr: case IrOp::Sar: case IrOp::Not: case IrOp::Neg: case IrOp::SetCond: case IrOp::LoadState: case IrOp::Helper:
      return true;
    default: return false;
  }
}

uint64_t apply(IrOp op, uint64_t a, uint64_t b) {
  switch (op) {
    case IrOp::Add: return a + b;
    case IrOp::Sub: return a - b;
    case IrOp::Mul: return a * b;
    case IrOp::And: return a & b;
    case IrOp::Or: return a | b;
    case IrOp::Xor: return a ^ b;
    case IrOp::Shl: return a << (b & 63);
    case IrOp::Shr: return a >> (b & 63);
    case IrOp::Sar: return static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63));
    case IrOp::Not: return ~a;
    case IrOp::Neg: return uint64_t{0} - a;
    default: return 0;
  }
}

}  // namespace

bool ir_reads_a(IrOp op) { return reads_a_impl(op); }
bool ir_reads_b(IrOp op) { return reads_b_impl(op); }
bool ir_writes_dst(IrOp op) { return writes_dst_impl(op); }

namespace {
bool reads_a(IrOp op) { return reads_a_impl(op); }
bool reads_b(IrOp op) { return reads_b_impl(op); }
bool writes_dst(IrOp op) { return writes_dst_impl(op); }
}  // namespace

bool validate(allocators::IAllocator& allocator, const IrBlock& block) {
  const ArrayView<const IrInst> insts = block.insts.view();
  if (insts.empty()) return false;
  const IrOp last = insts[insts.size() - 1].op;
  if (last != IrOp::Exit && last != IrOp::ExitValue && last != IrOp::Jump) return false;
  // 0 = not placed, 1 = placed; a jump to a label is checked against it after the pass (the label may come later).
  ArrayView<uint8_t> placed;
  if (block.label_count) {
    placed = allocate_view<uint8_t>(allocator, block.label_count);
    if (placed.empty()) return false;
    placed.fill(0);
  }
  bool ok = true;
  for (const IrInst& inst : insts) {
    if (reads_a(inst.op) && inst.a >= block.vreg_count) ok = false;
    if (reads_b(inst.op) && inst.b >= block.vreg_count) ok = false;
    if (writes_dst(inst.op) && inst.dst >= block.vreg_count) ok = false;
    switch (inst.op) {
      case IrOp::LoadState: case IrOp::StoreState:
        if (inst.imm < 0 || inst.imm % 8 != 0 || static_cast<uint64_t>(inst.imm) > block.state_size || block.state_size - static_cast<uint64_t>(inst.imm) < 8) ok = false;
        break;
      case IrOp::Helper:
        if (inst.imm == 0) ok = false;
        break;
      case IrOp::Label:
        if (inst.imm < 0 || static_cast<uint64_t>(inst.imm) >= block.label_count || placed[static_cast<uint64_t>(inst.imm)]) ok = false;
        else placed[static_cast<uint64_t>(inst.imm)] = 1;
        break;
      case IrOp::Jump: case IrOp::BranchIf:
        if (inst.imm < 0 || static_cast<uint64_t>(inst.imm) >= block.label_count) ok = false;
        break;
      default: break;
    }
  }
  if (ok) {
    for (const IrInst& inst : insts) {
      if ((inst.op == IrOp::Jump || inst.op == IrOp::BranchIf) && !placed[static_cast<uint64_t>(inst.imm)]) ok = false;
    }
  }
  release_view(allocator, placed);
  return ok;
}

bool interpret(allocators::IAllocator& allocator, const IrBlock& block, ByteView state, uint64_t max_steps, uint64_t* exit_value) {
  const ArrayView<const IrInst> insts = block.insts.view();
  const uint64_t n = insts.size();
  // Registers (zero until written) and the position of every label.
  ArrayView<uint64_t> regs;
  ArrayView<uint64_t> label_at;
  if (block.vreg_count) {
    regs = allocate_view<uint64_t>(allocator, block.vreg_count);
    if (regs.empty()) return false;
    regs.fill(0);
  }
  if (block.label_count) {
    label_at = allocate_view<uint64_t>(allocator, block.label_count);
    if (label_at.empty()) {
      release_view(allocator, regs);
      return false;
    }
    label_at.fill(0);
  }
  for (uint64_t i = 0; i < n; ++i) {
    if (insts[i].op == IrOp::Label) label_at[static_cast<uint64_t>(insts[i].imm)] = i;
  }
  bool done = false;
  uint64_t steps = 0;
  uint64_t pc = 0;
  while (pc < n && steps++ < max_steps) {
    const IrInst& inst = insts[pc++];
    const uint64_t a = reads_a(inst.op) ? regs[inst.a] : 0;
    const uint64_t b = reads_b(inst.op) ? regs[inst.b] : 0;
    switch (inst.op) {
      case IrOp::Const: regs[inst.dst] = static_cast<uint64_t>(inst.imm); break;
      case IrOp::Move: regs[inst.dst] = a; break;
      case IrOp::SetCond: regs[inst.dst] = eval_cond(inst.cond, a, b) ? 1 : 0; break;
      case IrOp::LoadState: {
        ByteView word = state.subview(static_cast<uint64_t>(inst.imm), 8);
        uint64_t v = 0;
        for (uint64_t k = 0; k < 8; ++k) v |= static_cast<uint64_t>(word[k]) << (8 * k);
        regs[inst.dst] = v;
        break;
      }
      case IrOp::StoreState: {
        ByteView word = state.subview(static_cast<uint64_t>(inst.imm), 8);
        for (uint64_t k = 0; k < 8; ++k) word[k] = static_cast<uint8_t>(a >> (8 * k));
        break;
      }
      case IrOp::Helper: {
        const IrHelper fn = helper_from_address(inst.imm);
        regs[inst.dst] = fn(state.data(), a, b);
        break;
      }
      case IrOp::Label: break;
      case IrOp::Jump: pc = label_at[static_cast<uint64_t>(inst.imm)]; break;
      case IrOp::BranchIf:
        if (eval_cond(inst.cond, a, b)) pc = label_at[static_cast<uint64_t>(inst.imm)];
        break;
      case IrOp::Exit: *exit_value = static_cast<uint64_t>(inst.imm); done = true; break;
      case IrOp::ExitValue: *exit_value = a; done = true; break;
      default: regs[inst.dst] = apply(inst.op, a, b); break;
    }
    if (done) break;
  }
  release_view(allocator, regs);
  release_view(allocator, label_at);
  return done;
}

}  // namespace jit
