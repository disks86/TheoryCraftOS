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
#include "Jit/Ir/RegisterAllocator.hpp"
#include "Utility/AllocatedView.hpp"

namespace jit {

namespace {
constexpr uint64_t kNever = ~uint64_t{0};

template <class T>
bool make(allocators::IAllocator& a, ArrayView<T>& view, uint64_t count, T value) {
  view = ArrayView<T>();
  if (count == 0) return true;
  view = allocate_view<T>(a, count);
  if (view.empty()) return false;
  view.fill(value);
  return true;
}
}  // namespace

RegisterAllocation::~RegisterAllocation() {
  release_view(allocator_, locations_);
  release_view(allocator_, zero_);
  release_view(allocator_, crosses_);
  release_view(allocator_, start_);
  release_view(allocator_, end_);
}

bool RegisterAllocation::allocate(const IrBlock& block, RegisterPool pool, uint32_t max_registers) {
  const uint32_t vregs = block.vreg_count;
  const ArrayView<const IrInst> insts = block.insts.view();
  const uint64_t n = insts.size();
  uint32_t total = pool.callee_saved + pool.caller_saved;
  if (total > 32) return false;
  if (max_registers < total) total = max_registers;
  const uint32_t callee_count = pool.callee_saved < total ? pool.callee_saved : total;
  const uint32_t callee_mask = callee_count >= 32 ? ~0u : (1u << callee_count) - 1;
  const uint32_t all_mask = total >= 32 ? ~0u : (1u << total) - 1;
  const uint32_t caller_mask = all_mask & ~callee_mask;

  ArrayView<uint64_t> first_def, label_pos;
  const bool ok = make(allocator_, locations_, vregs, VRegLocation()) && make<uint8_t>(allocator_, zero_, vregs, 0) && make<uint8_t>(allocator_, crosses_, vregs, 0) &&
                  make(allocator_, start_, vregs, kNever) && make(allocator_, end_, vregs, uint64_t{0}) && make(allocator_, first_def, vregs, kNever) &&
                  make(allocator_, label_pos, block.label_count, uint64_t{0});
  spill_slots_ = 0;
  used_ = 0;
  has_calls_ = false;
  if (!ok) {
    release_view(allocator_, first_def);
    release_view(allocator_, label_pos);
    return false;
  }

  // ---- references: first and last instruction touching each register, and the first write
  ArrayView<uint8_t> first_is_read;
  bool good = make<uint8_t>(allocator_, first_is_read, vregs, 0);
  if (good) {
    // (positions are instruction index + 1, so that 0 is "before the first instruction", where registers that must be zero are born)
    for (uint64_t i = 0; i < n; ++i) {
      const IrInst& inst = insts[i];
      const uint64_t pos = i + 1;
      if (inst.op == IrOp::Label) label_pos[static_cast<uint64_t>(inst.imm)] = pos;
      if (inst.op == IrOp::Helper) has_calls_ = true;
      const uint32_t refs[3] = {ir_reads_a(inst.op) ? inst.a : kNoVReg, ir_reads_b(inst.op) ? inst.b : kNoVReg, ir_writes_dst(inst.op) ? inst.dst : kNoVReg};
      for (int r = 0; r < 3; ++r) {
        const uint32_t v = refs[r];
        if (v == kNoVReg) continue;
        if (start_[v] == kNever) {
          start_[v] = pos;
          first_is_read[v] = r < 2;
        }
        end_[v] = pos;
        if (r == 2 && first_def[v] == kNever) first_def[v] = pos;
      }
    }

    // ---- registers read before an unconditional write start at the entry
    for (uint32_t v = 0; v < vregs; ++v) {
      if (start_[v] == kNever) continue;
      bool needs_zero = first_is_read[v] != 0;
      if (!needs_zero) {
        // Is the first write jumped over by a forward branch (q < p < target)? Then it may not have run when the register is read.
        const uint64_t p = first_def[v];
        for (uint64_t q = 0; q + 1 < p && !needs_zero; ++q) {
          const IrInst& inst = insts[q];
          if ((inst.op == IrOp::Jump || inst.op == IrOp::BranchIf) && label_pos[static_cast<uint64_t>(inst.imm)] > p) needs_zero = true;
        }
      }
      if (needs_zero) {
        zero_[v] = 1;
        start_[v] = 0;
      }
    }

    // ---- loops: a value live anywhere inside [label, back-jump] lives until the back jump
    for (bool changed = true; changed;) {
      changed = false;
      for (uint64_t qi = 0; qi < n; ++qi) {
        const IrInst& inst = insts[qi];
        if (inst.op != IrOp::Jump && inst.op != IrOp::BranchIf) continue;
        const uint64_t q = qi + 1;
        const uint64_t l = label_pos[static_cast<uint64_t>(inst.imm)];
        if (l >= q) continue;
        for (uint32_t v = 0; v < vregs; ++v) {
          if (start_[v] == kNever) continue;
          if (start_[v] <= q && end_[v] >= l && end_[v] < q) {
            end_[v] = q;
            changed = true;
          }
        }
      }
    }

    // ---- which intervals contain a call (strictly inside: an argument read at the call and a result written by it are safe)
    if (has_calls_) {
      for (uint64_t i = 0; i < n; ++i) {
        if (insts[i].op != IrOp::Helper) continue;
        const uint64_t pos = i + 1;
        for (uint32_t v = 0; v < vregs; ++v) {
          if (start_[v] != kNever && start_[v] < pos && end_[v] > pos) crosses_[v] = 1;
        }
      }
    }

    // ---- linear scan, in order of interval start (counting sort by start position)
    ArrayView<uint32_t> order;
    ArrayView<uint64_t> bucket;
    uint32_t count = 0;
    good = make<uint32_t>(allocator_, order, vregs, 0) && make(allocator_, bucket, n + 3, uint64_t{0});
    if (good) {
      for (uint32_t v = 0; v < vregs; ++v) if (start_[v] != kNever) ++bucket[start_[v] + 1];
      for (uint64_t i = 1; i < n + 3; ++i) bucket[i] += bucket[i - 1];
      for (uint32_t v = 0; v < vregs; ++v) if (start_[v] != kNever) order[bucket[start_[v]]++] = v, ++count;

      uint32_t active[32];
      uint32_t active_count = 0;
      uint32_t free_mask = all_mask;
      for (uint32_t k = 0; k < count; ++k) {
        const uint32_t v = order[k];
        const uint64_t s = start_[v], e = end_[v];
        for (uint32_t a = 0; a < active_count;) {   // expire
          const uint32_t w = active[a];
          if (end_[w] <= s) {
            free_mask |= 1u << locations_[w].index;
            active[a] = active[--active_count];
          } else {
            ++a;
          }
        }
        const uint32_t allowed = crosses_[v] ? callee_mask : all_mask;
        uint32_t candidates = free_mask & allowed;
        if (!crosses_[v] && (candidates & caller_mask)) candidates &= caller_mask;   // (cheaper: nothing to save)
        // Coalescing hint: if v is born from an operand that dies right there, take that operand's register. A Move then needs no instruction, and a two-address
        // back end (x86) can compute in place. For a commutative operation either operand will do.
        if (!zero_[v] && first_def[v] == s) {
          const IrInst& d = insts[s - 1];
          const bool commutative = d.op == IrOp::Add || d.op == IrOp::Mul || d.op == IrOp::And || d.op == IrOp::Or || d.op == IrOp::Xor;
          if (d.op != IrOp::Helper && d.op != IrOp::SetCond && d.op != IrOp::LoadState) {
            const uint32_t hints[2] = {ir_reads_a(d.op) ? d.a : kNoVReg, commutative && ir_reads_b(d.op) ? d.b : kNoVReg};
            for (uint32_t h : hints) {
              if (h == kNoVReg || h == v || start_[h] == kNever || end_[h] != s) continue;
              if (locations_[h].kind != VRegLocation::Kind::Register) continue;
              const uint32_t bit = 1u << locations_[h].index;
              if (candidates & bit) {
                candidates = bit;
                break;
              }
            }
          }
        }
        if (candidates) {
          uint32_t reg = 0;
          while (!(candidates >> reg & 1)) ++reg;
          free_mask &= ~(1u << reg);
          locations_[v] = {VRegLocation::Kind::Register, reg};
          used_ |= 1u << reg;
          active[active_count++] = v;
          continue;
        }
        // No free register: spill whichever of the active intervals (that holds a register this one may use) or this one ends last.
        uint32_t victim = kNoVReg;
        uint64_t victim_end = e;
        for (uint32_t a = 0; a < active_count; ++a) {
          const uint32_t w = active[a];
          if ((allowed >> locations_[w].index & 1) && end_[w] > victim_end) {
            victim = w;
            victim_end = end_[w];
          }
        }
        if (victim == kNoVReg) {
          locations_[v] = {VRegLocation::Kind::Spill, 0};
        } else {
          locations_[v] = locations_[victim];
          locations_[victim] = {VRegLocation::Kind::Spill, 0};
          for (uint32_t a = 0; a < active_count; ++a) {
            if (active[a] == victim) {
              active[a] = v;
              break;
            }
          }
        }
      }
    }
    // ---- spill slots: a slot is reused by a spilled interval that starts where the previous one in it has ended
    if (good) {
      uint64_t spilled = 0;
      for (uint32_t v = 0; v < vregs; ++v) spilled += locations_[v].kind == VRegLocation::Kind::Spill;
      ArrayView<uint64_t> slot_end;
      good = make(allocator_, slot_end, spilled, uint64_t{0});
      if (good) {
        for (uint32_t k = 0; k < count; ++k) {
          const uint32_t v = order[k];
          if (start_[v] == kNever || locations_[v].kind != VRegLocation::Kind::Spill) continue;
          uint32_t slot = 0;
          while (slot < spill_slots_ && slot_end[slot] > start_[v]) ++slot;
          if (slot == spill_slots_) ++spill_slots_;
          slot_end[slot] = end_[v];
          locations_[v].index = slot;
        }
      }
      release_view(allocator_, slot_end);
    }
    release_view(allocator_, order);
    release_view(allocator_, bucket);
  }
  release_view(allocator_, first_is_read);
  release_view(allocator_, first_def);
  release_view(allocator_, label_pos);
  return good;
}

}  // namespace jit
