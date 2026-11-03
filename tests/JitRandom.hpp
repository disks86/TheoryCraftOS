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
// A random IR program over every operation, with forward branches only, ending in an exit. State words 0..3 are inputs, 4..7 outputs. Shared by the
// native x86-64 test and the cross-run AArch64 test (which builds the same program twice: with the host's helper for the interpreter, and with the address of
// the helper in the emulated runner for the generated code).
#pragma once
#include <stdint.h>
#include <vector>
#include "Jit/Ir/Ir.hpp"

namespace jit {

inline void build_random(IrBuilder& b, uint64_t& seed, uint32_t length, IrHelper host_helper, uint64_t helper_address = 0) {
  auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
  auto helper = [&](uint32_t x, uint32_t y) { return helper_address ? b.helper_at(helper_address, x, y) : b.helper(host_helper, x, y); };
  std::vector<uint32_t> regs;
  for (uint32_t i = 0; i < 4; ++i) regs.push_back(b.load_state(8 * i));
  regs.push_back(b.constant(static_cast<int64_t>(next())));
  regs.push_back(b.constant(-1));
  auto pick = [&] { return regs[next() % regs.size()]; };
  static const IrOp bin[] = {IrOp::Add, IrOp::Sub, IrOp::Mul, IrOp::And, IrOp::Or, IrOp::Xor, IrOp::Shl, IrOp::Shr, IrOp::Sar};
  for (uint32_t i = 0; i < length; ++i) {
    switch (next() % 9) {
      case 0: case 1: case 2: regs.push_back(b.binary(bin[next() % 9], pick(), pick())); break;
      case 3: regs.push_back(b.unary(next() & 1 ? IrOp::Not : IrOp::Neg, pick())); break;
      case 4: regs.push_back(b.set_cond(static_cast<IrCond>(next() % 10), pick(), pick())); break;
      case 5: regs.push_back(b.constant(static_cast<int64_t>(next() >> (next() % 64)))); break;
      case 6: regs.push_back(helper(pick(), pick())); break;
      case 7: b.store_state(8 * (4 + next() % 4), pick()); break;
      default: {   // if (a cond b) skip the next few operations
        const uint32_t skip = b.new_label();
        b.branch_if(static_cast<IrCond>(next() % 10), pick(), pick(), skip);
        regs.push_back(b.binary(bin[next() % 9], pick(), pick()));
        b.store_state(8 * (4 + next() % 4), pick());
        b.place_label(skip);   // (a register defined inside may be unwritten on the other path: it reads 0 there, in every implementation)
        break;
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) b.store_state(8 * (4 + i), pick());
  b.exit_value(pick());
}

// Nested loops with values that live across the back edges and across helper calls: state word 0 (masked to 0..31) is the outer trip count, word 1 the
// starting accumulator. The result is stored in word 5 and returned.
inline void build_loops(IrBuilder& b, IrHelper host_helper, uint64_t helper_address = 0) {
  auto helper = [&](uint32_t x, uint32_t y) { return helper_address ? b.helper_at(helper_address, x, y) : b.helper(host_helper, x, y); };
  const uint32_t n = b.binary(IrOp::And, b.load_state(0), b.constant(31));
  const uint32_t acc = b.new_vreg();
  b.assign(acc, b.load_state(8));
  const uint32_t i = b.new_vreg(), one = b.constant(1), three = b.constant(3);
  b.assign(i, b.constant(0));
  const uint32_t done = b.new_label(), outer = b.new_label(), inner = b.new_label();
  b.branch_if(IrCond::Geu, i, n, done);   // (zero trips)
  b.place_label(outer);
  const uint32_t j = b.new_vreg();
  b.assign(j, b.constant(0));
  b.place_label(inner);
  const uint32_t t = helper(acc, j);
  b.assign(acc, b.binary(IrOp::Add, b.binary(IrOp::Xor, acc, t), i));
  b.assign(j, b.binary(IrOp::Add, j, one));
  b.branch_if(IrCond::Ltu, j, three, inner);
  b.assign(i, b.binary(IrOp::Add, i, one));
  b.branch_if(IrCond::Ltu, i, n, outer);
  b.place_label(done);
  b.store_state(40, acc);
  b.exit_value(acc);
}

}  // namespace jit
