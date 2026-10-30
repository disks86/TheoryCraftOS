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

// Segment registers and far control transfers. A user program in 64-bit mode runs in a flat address space: there are no descriptor tables to consult, so a selector is valid when it is null or has
// requested privilege level 3 and a plausible index (the CS value 0x33 is the only code segment: a transfer to 32-bit compatibility mode is #GP). The registers hold the selectors a program stored,
// but loading FS or GS does not change their base (that is what wrfsbase and the thread-pointer calls are for).
// mov Sreg, lss / lfs / lgs, push / pop fs / gs, call far and jmp far through memory (FF /3, FF /5), retf and iretq, int n.
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {

constexpr uint16_t kCodeSelector = 0x33;

bool data_selector_valid(uint16_t sel) {
  if ((sel >> 3) == 0 && (sel & 4) == 0) return true;      // null
  return (sel & 7) == 3 && (sel >> 3) <= 0x1F;
}

}  // namespace


bool X86_64Cpu::push_sized(GuestMemory& m, uint64_t value, uint32_t size) {
  const uint64_t sp = s_.r[4] - size;
  if (!store(m, sp, size, value)) return false;
  s_.r[4] = sp;
  return true;
}

bool X86_64Cpu::load_selector(uint32_t sreg, uint16_t sel) {
  if (sreg == 1) return false;      // CS is not loaded by a move
  if (sreg == 2 && (sel >> 3) == 0) return false;      // a null SS is #GP outside ring 0
  if (!data_selector_valid(sel)) return false;
  selector_[sreg] = sel;
  return true;
}

X86_64Cpu::Step X86_64Cpu::segment_move(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t sreg = d_.reg & 7;
  if (sreg > 5) return fail(6);
  if (op == 0x8C) {
    if (!write_rm(m, d_.mod == 3 ? static_cast<uint32_t>(operand_size()) : 2, selector_[sreg])) return Step::Fault;
  } else {
    uint64_t v = 0;
    if (sreg == 1) return fail(6);      // mov cs, r/m
    if (!read_rm(m, 2, &v)) return Step::Fault;
    if (!load_selector(sreg, static_cast<uint16_t>(v))) return fail(13);
  }
  s_.rip += d_.position;
  return Step::Ok;
}

// 0F B2 / B4 / B5: lss, lfs, lgs reg, m16:size
X86_64Cpu::Step X86_64Cpu::far_pointer_load(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (d_.mod == 3) return fail(6);
  const uint32_t size = static_cast<uint32_t>(operand_size()), sreg = op == 0xB2 ? 2 : op == 0xB4 ? 4 : 5;
  uint64_t offset = 0, selector = 0;
  const uint64_t address = memory_address();
  if (!load(m, address, size, &offset) || !load(m, address + size, 2, &selector)) return Step::Fault;
  if (!load_selector(sreg, static_cast<uint16_t>(selector))) return fail(13);
  set_reg(d_.reg, size, offset);
  s_.rip += d_.position;
  return Step::Ok;
}

// FF /3 (call far) and FF /5 (jmp far): the target is m16:16, m16:32 or m16:64 (REX.W).
X86_64Cpu::Step X86_64Cpu::far_branch(GuestMemory& m, bool is_call) {
  const uint32_t size = static_cast<uint32_t>(operand_size());
  if (d_.mod == 3) return fail(6);
  uint64_t offset = 0, selector = 0;
  const uint64_t address = memory_address();
  if (!load(m, address, size, &offset) || !load(m, address + size, 2, &selector)) return Step::Fault;
  if (static_cast<uint16_t>(selector) != kCodeSelector) return fail(13);
  const uint64_t return_address = s_.rip + d_.position;
  if (is_call) {
    const uint64_t saved_sp = s_.r[4];
    if (!push_sized(m, selector_[1], size) || !push_sized(m, return_address, size)) {
      s_.r[4] = saved_sp;
      return Step::Fault;
    }
    if (cet_.enabled && !cet_push_far(m, selector_[1], return_address)) {
      s_.r[4] = saved_sp;
      return Step::Fault;
    }
  }
  selector_[1] = kCodeSelector;
  s_.rip = size == 8 ? offset : offset & (size == 4 ? 0xFFFFFFFFu : 0xFFFFu);
  return Step::Ok;
}

// CA / CB: retf [imm16]
X86_64Cpu::Step X86_64Cpu::far_return(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t size = static_cast<uint32_t>(operand_size());
  const uint64_t extra = op == 0xCA ? next_imm(2, false) : 0;
  if (!finish_decode()) return bad_decode();
  uint64_t target = 0, selector = 0;
  if (!load(m, s_.r[4], size, &target) || !load(m, s_.r[4] + size, 2, &selector)) return Step::Fault;
  if (static_cast<uint16_t>(selector) != kCodeSelector) return fail(13);
  if (cet_.enabled && !cet_check_far(m, static_cast<uint16_t>(selector), target)) return Step::Fault;
  selector_[1] = kCodeSelector;
  s_.r[4] += 2 * size + extra;
  s_.rip = size == 8 ? target : target & (size == 4 ? 0xFFFFFFFFu : 0xFFFFu);
  return Step::Ok;
}

// CF: iret / iretd / iretq. In 64-bit mode the stack pointer and SS are always restored too. At ring 3 the flags that a program may change are the arithmetic ones, TF, DF, NT, AC and ID.
X86_64Cpu::Step X86_64Cpu::interrupt_return(GuestMemory& m) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!finish_decode()) return bad_decode();
  const uint32_t size = static_cast<uint32_t>(operand_size());
  uint64_t frame[5] = {};
  for (uint32_t i = 0; i < 5; ++i)
    if (!load(m, s_.r[4] + i * size, size, &frame[i])) return Step::Fault;
  if (static_cast<uint16_t>(frame[1]) != kCodeSelector) return fail(13);
  if (!data_selector_valid(static_cast<uint16_t>(frame[4])) || (frame[4] >> 3) == 0) return fail(13);
  if (cet_.enabled && !cet_check_far(m, static_cast<uint16_t>(frame[1]), frame[0])) return Step::Fault;
  constexpr uint64_t kWritable = 0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800 | 0x4000 | 0x40000 | 0x200000;
  s_.rflags = ((s_.rflags & ~kWritable) | (frame[2] & kWritable)) | 2;
  selector_[1] = kCodeSelector;
  selector_[2] = static_cast<uint16_t>(frame[4]);
  s_.rip = size == 8 ? frame[0] : frame[0] & (size == 4 ? 0xFFFFFFFFu : 0xFFFFu);
  s_.r[4] = frame[3];
  return Step::Ok;
}

}  // namespace emulation
