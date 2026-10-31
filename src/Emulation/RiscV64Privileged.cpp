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
#include "Emulation/RiscV64Cpu.hpp"

// The RISC-V privileged architecture for RiscV64Cpu: machine, supervisor and user modes, the machine and supervisor CSRs, exceptions and interrupts with delegation, mret / sret /
// wfi / sfence.vma, Sv39 / Sv48 / Sv57 address translation (without hardware A/D updates: Svade) and PMP. The hardware outside the hart (memory, CLINT, UART) is the bus.

namespace emulation {

namespace {

constexpr uint64_t kSie = 1u << 1, kMie = 1u << 3, kSpie = 1u << 5, kMpie = 1u << 7, kSpp = 1u << 8, kMpp = 3u << 11, kMprv = 1u << 17, kSum = 1u << 18, kMxr = 1u << 19, kTvm = 1u << 20,
                   kTw = 1u << 21, kTsr = 1u << 22;
constexpr uint64_t kVsField = 3u << 9, kFsField = 3u << 13, kXsField = 3u << 15;
constexpr uint64_t kUxl = uint64_t{2} << 32, kSxl = uint64_t{2} << 34, kSd = uint64_t{1} << 63;
constexpr uint64_t kMstatusWritable = kSie | kMie | kSpie | kMpie | kSpp | kVsField | kMpp | kFsField | kMprv | kSum | kMxr | kTvm | kTw | kTsr;
constexpr uint64_t kSstatusMask = kSie | kSpie | kSpp | kVsField | kFsField | kXsField | kSum | kMxr | kUxl | kSd;
constexpr uint64_t kSupervisorInterrupts = (1u << 1) | (1u << 5) | (1u << 9);      // SSI, STI, SEI
constexpr uint64_t kMachineInterrupts = (1u << 3) | (1u << 7) | (1u << 11);
constexpr uint64_t kMedelegWritable = 0xB3FF;      // not the machine ecall (11), the reserved 10 and 14
constexpr uint64_t kMisa = (uint64_t{2} << 62) | (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 8) | (1u << 12) | (1u << 18) | (1u << 20) | (1u << 21);      // A C D F I M S U V
constexpr uint64_t kEnvcfgStce = uint64_t{1} << 63;
constexpr uint64_t kPpnMask = (uint64_t{1} << 44) - 1;

uint64_t with_sd(uint64_t status) {
  status &= ~kSd;
  if ((status & kFsField) == kFsField || (status & kVsField) == kVsField) status |= kSd;
  return status;
}

}  // namespace

void RiscV64Cpu::enable_system(IRiscVBus& bus, GuestMemory& placeholder, uint64_t pc) {
  bus_ = &bus;
  placeholder_ = &placeholder;
  sys_ = System();
  sys_.mstatus = kUxl | kSxl;
  s_ = State();
  s_.pc = pc;
  retired_ = 0;
  reservation_valid_ = false;
}

// --- physical memory protection ------------------------------------------------------------------------------------------------------------

bool RiscV64Cpu::pmp_allows(uint64_t physical, uint32_t size, Access access, uint8_t priv) const {
  bool any = false;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint8_t cfg = sys_.pmpcfg[i];
    const uint32_t mode = (cfg >> 3) & 3;
    if (mode == 0) continue;
    any = true;
    uint64_t low, high;      // [low, high)
    if (mode == 1) {      // TOR
      low = (i == 0 ? 0 : sys_.pmpaddr[i - 1]) << 2;
      high = sys_.pmpaddr[i] << 2;
    } else if (mode == 2) {      // NA4
      low = sys_.pmpaddr[i] << 2;
      high = low + 4;
    } else {      // NAPOT: the trailing ones of the address give the size
      const uint64_t a = sys_.pmpaddr[i];
      uint32_t ones = 0;
      while (ones < 54 && ((a >> ones) & 1)) ++ones;
      const uint64_t span = uint64_t{8} << ones;
      low = (a << 2) & ~(span - 1);
      high = low + span;
    }
    const uint64_t end = physical + size;
    if (end <= low || physical >= high) continue;      // (an access that straddles an entry's edge fails, as the spec says)
    if (physical < low || end > high) return false;
    if (priv == 3 && !(cfg & 0x80)) return true;
    const uint8_t need = access == Access::Fetch ? 4 : access == Access::Store ? 2 : 1;
    return (cfg & need) != 0;
  }
  return priv == 3 || !any;      // (no rule configured: everything is allowed, as in qemu)
}

// --- address translation -------------------------------------------------------------------------------------------------------------------

RiscV64Cpu::Translation RiscV64Cpu::translate(uint64_t address, Access access, uint32_t size) {
  uint8_t priv = sys_.priv;
  if (access != Access::Fetch && priv == 3 && (sys_.mstatus & kMprv)) priv = static_cast<uint8_t>((sys_.mstatus >> 11) & 3);
  const uint8_t access_fault = access == Access::Fetch ? 1 : access == Access::Load ? 5 : 7;
  const uint8_t page_fault = access == Access::Fetch ? 12 : access == Access::Load ? 13 : 15;
  const uint64_t mode = sys_.satp >> 60;
  uint64_t physical = address;
  if (priv != 3 && mode != 0) {
    const uint32_t levels = mode == 8 ? 3 : mode == 9 ? 4 : 5;
    const uint32_t va_bits = 12 + 9 * levels;
    const int64_t folded = static_cast<int64_t>(address << (64 - va_bits)) >> (64 - va_bits);
    if (static_cast<uint64_t>(folded) != address) return {0, page_fault, false};
    uint64_t table = (sys_.satp & kPpnMask) << 12;
    uint64_t pte = 0;
    uint32_t level = levels;
    for (;;) {
      --level;
      const uint64_t entry_address = table + (((address >> (12 + 9 * level)) & 511) << 3);
      if (!pmp_allows(entry_address, 8, Access::Load, 1)) return {0, access_fault, false};
      if (!bus_->read(entry_address, 8, &pte)) return {0, access_fault, false};
      if (!(pte & 1) || ((pte & 2) == 0 && (pte & 4) != 0) || (pte >> 54) != 0) return {0, page_fault, false};
      if (pte & 0xE) break;      // a leaf
      if (level == 0) return {0, page_fault, false};
      table = ((pte >> 10) & kPpnMask) << 12;
    }
    const bool r = pte & 2, w = pte & 4, x = pte & 8, u = pte & 0x10;
    if (priv == 0 && !u) return {0, page_fault, false};
    if (priv == 1 && u && (access == Access::Fetch || !(sys_.mstatus & kSum))) return {0, page_fault, false};
    const bool allowed = access == Access::Fetch ? x : access == Access::Load ? (r || (x && (sys_.mstatus & kMxr))) : w;
    if (!allowed) return {0, page_fault, false};
    const uint64_t ppn = (pte >> 10) & kPpnMask;
    if (level > 0 && (ppn & ((uint64_t{1} << (9 * level)) - 1)) != 0) return {0, page_fault, false};      // (a misaligned superpage)
    if (!(pte & 0x40) || (access == Access::Store && !(pte & 0x80))) return {0, page_fault, false};      // accessed / dirty clear: a fault, the software sets them
    const uint64_t offset_mask = (uint64_t{1} << (12 + 9 * level)) - 1;
    physical = ((ppn << 12) & ~offset_mask) | (address & offset_mask);
  }
  if (!pmp_allows(physical, size, access, priv)) return {0, access_fault, false};
  return {physical, 0, true};
}

bool RiscV64Cpu::sys_read(uint64_t address, uint32_t size, Access access, uint64_t* out) {
  uint64_t value = 0;
  for (uint32_t done = 0; done < size;) {      // (an access is split at page boundaries: each part is translated on its own)
    const uint32_t room = 4096 - static_cast<uint32_t>((address + done) & 4095);
    const uint32_t part = size - done < room ? size - done : room;
    const Translation t = translate(address + done, access, part);
    uint64_t chunk = 0;
    if (!t.ok || !bus_->read(t.physical, part, &chunk)) {
      trap_cause_ = t.ok ? (access == Access::Fetch ? 1 : access == Access::Load ? 5 : 7) : t.cause;
      if (access == Access::Store && t.ok) trap_cause_ = 7;
      if (access == Access::Load && t.ok) trap_cause_ = 5;
      trap_tval_ = address + done;
      if (done == 0) trap_tval_ = address;
      s_.pc = instruction_start_;
      return false;
    }
    value |= chunk << (8 * done);
    done += part;
  }
  *out = value;
  return true;
}

bool RiscV64Cpu::sys_write(uint64_t address, uint32_t size, uint64_t value) {
  for (uint32_t done = 0; done < size;) {
    const uint32_t room = 4096 - static_cast<uint32_t>((address + done) & 4095);
    const uint32_t part = size - done < room ? size - done : room;
    const Translation t = translate(address + done, Access::Store, part);
    const uint64_t chunk = part == 8 ? value >> (8 * done) : (value >> (8 * done)) & ((uint64_t{1} << (8 * part)) - 1);
    if (!t.ok || !bus_->write(t.physical, part, chunk)) {
      trap_cause_ = t.ok ? 7 : t.cause;
      trap_tval_ = address + done;
      s_.pc = instruction_start_;
      return false;
    }
    done += part;
  }
  return true;
}

RiscV64Cpu::Step RiscV64Cpu::sys_fail(uint8_t vector, uint64_t address) {
  s_.pc = instruction_start_;
  switch (vector) {
    case 0: trap_cause_ = 0; trap_tval_ = address; break;      // instruction address misaligned
    case 3: trap_cause_ = 3; trap_tval_ = instruction_start_; break;
    case 13: trap_cause_ = (raw_instruction_ >> 27) == 2 && (raw_instruction_ & 0x7F) == 0x2F ? 4 : 6; trap_tval_ = address; break;
    default: trap_cause_ = 2; trap_tval_ = raw_instruction_; break;
  }
  return Step::Fault;
}

// --- traps ---------------------------------------------------------------------------------------------------------------------------------

void RiscV64Cpu::take_trap(uint64_t cause, uint64_t tval, bool interrupt) {
  const uint64_t pc = s_.pc;
  const bool delegated = sys_.priv <= 1 && (((interrupt ? sys_.mideleg : sys_.medeleg) >> cause) & 1);
  const uint64_t code = interrupt ? (uint64_t{1} << 63) | cause : cause;
  reservation_valid_ = false;
  if (delegated) {
    sys_.sepc = pc;
    sys_.scause = code;
    sys_.stval = tval;
    uint64_t status = sys_.mstatus;
    status = (status & kSie) ? (status | kSpie) : (status & ~kSpie);
    status &= ~kSie;
    status = sys_.priv == 1 ? (status | kSpp) : (status & ~kSpp);
    sys_.mstatus = status;
    sys_.priv = 1;
    const uint64_t base = sys_.stvec & ~uint64_t{3};
    s_.pc = (interrupt && (sys_.stvec & 3) == 1) ? base + 4 * cause : base;
  } else {
    sys_.mepc = pc;
    sys_.mcause = code;
    sys_.mtval = tval;
    uint64_t status = sys_.mstatus;
    status = (status & kMie) ? (status | kMpie) : (status & ~kMpie);
    status &= ~kMie;
    status = (status & ~kMpp) | (static_cast<uint64_t>(sys_.priv) << 11);
    sys_.mstatus = status;
    sys_.priv = 3;
    const uint64_t base = sys_.mtvec & ~uint64_t{3};
    s_.pc = (interrupt && (sys_.mtvec & 3) == 1) ? base + 4 * cause : base;
  }
}

uint64_t RiscV64Cpu::pending_interrupts() {
  uint64_t hardware = bus_->interrupts();
  uint64_t pending = sys_.mip | hardware;
  if ((sys_.menvcfg & kEnvcfgStce) != 0) {      // Sstc: the supervisor timer comes from stimecmp
    pending &= ~(uint64_t{1} << 5);
    if (bus_->time() >= sys_.stimecmp) pending |= uint64_t{1} << 5;
  }
  return pending;
}

bool RiscV64Cpu::take_interrupt() {
  const uint64_t pending = pending_interrupts() & sys_.mie;
  if (pending == 0) return false;
  const bool machine_open = sys_.priv < 3 || (sys_.mstatus & kMie);
  const bool supervisor_open = sys_.priv < 1 || (sys_.priv == 1 && (sys_.mstatus & kSie));
  uint64_t usable = (machine_open ? pending & ~sys_.mideleg : 0) | (supervisor_open ? pending & sys_.mideleg : 0);
  if (usable == 0) return false;
  static const uint8_t order[6] = {11, 3, 7, 9, 1, 5};
  for (uint8_t cause : order) {
    if ((usable >> cause) & 1) {
      take_trap(cause, 0, true);
      return true;
    }
  }
  return false;
}

StopReason RiscV64Cpu::run_system(uint64_t max_instructions) {
  for (uint64_t i = 0; i < max_instructions; ++i) {
    bus_->tick();
    if (take_interrupt()) continue;
    const Step r = step(*placeholder_);
    if (r == Step::Fault) {
      take_trap(trap_cause_, trap_tval_, false);
    } else {
      ++retired_;
    }
  }
  return StopReason::Budget;
}

// --- mret, sret, wfi, sfence.vma, ecall, ebreak ---------------------------------------------------------------------------------------------

RiscV64Cpu::Step RiscV64Cpu::sys_instruction(uint32_t insn) {
  const uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, funct7 = insn >> 25;
  if (insn == 0x00000073) {      // ecall
    trap_cause_ = sys_.priv == 0 ? 8 : sys_.priv == 1 ? 9 : 11;
    trap_tval_ = 0;
    s_.pc = instruction_start_;
    return Step::Fault;
  }
  if (insn == 0x00100073) return sys_fail(3, 0);      // ebreak
  if (insn == 0x30200073) {      // mret
    if (sys_.priv < 3) return undefined();
    const uint8_t target = static_cast<uint8_t>((sys_.mstatus >> 11) & 3);
    uint64_t status = sys_.mstatus;
    status = (status & kMpie) ? (status | kMie) : (status & ~kMie);
    status |= kMpie;
    status &= ~kMpp;
    if (target != 3) status &= ~kMprv;
    sys_.mstatus = status;
    sys_.priv = target;
    s_.pc = sys_.mepc;
    jumped_ = true;
    return Step::Ok;
  }
  if (insn == 0x10200073) {      // sret
    if (sys_.priv < 1 || (sys_.priv == 1 && (sys_.mstatus & kTsr))) return undefined();
    const uint8_t target = (sys_.mstatus & kSpp) ? 1 : 0;
    uint64_t status = sys_.mstatus;
    status = (status & kSpie) ? (status | kSie) : (status & ~kSie);
    status |= kSpie;
    status &= ~kSpp;
    if (target != 3) status &= ~kMprv;
    sys_.mstatus = status;
    sys_.priv = target;
    s_.pc = sys_.sepc;
    jumped_ = true;
    return Step::Ok;
  }
  if (insn == 0x10500073) {      // wfi
    if (sys_.priv == 0 || (sys_.priv == 1 && (sys_.mstatus & kTw))) return undefined();
    if ((pending_interrupts() & sys_.mie) == 0) bus_->idle((sys_.menvcfg & kEnvcfgStce) ? sys_.stimecmp : ~uint64_t{0});
    return Step::Ok;
  }
  if (rd == 0 && ((funct7 == 0x09) || (funct7 == 0x0B) || (funct7 == 0x0C && rs2 <= 1 && rs1 == 0))) {      // sfence.vma, sinval.vma, sfence.w.inval / sfence.inval.ir
    if (sys_.priv == 0 || (sys_.priv == 1 && (sys_.mstatus & kTvm) && funct7 != 0x0C)) return undefined();
    return Step::Ok;
  }
  return undefined();
}

// --- the F/D and V units: off / dirty tracking ---------------------------------------------------------------------------------------------

uint32_t RiscV64Cpu::sys_unit(uint32_t insn) const {
  const uint32_t opcode = insn & 0x7F, funct3 = (insn >> 12) & 7;
  switch (opcode) {
    case 0x07: return (funct3 >= 1 && funct3 <= 3) ? 1 : 2;
    case 0x27: return (funct3 >= 1 && funct3 <= 3) ? 1 | 4 : 2;
    case 0x53: {
      const uint32_t funct5 = insn >> 27;
      if (funct5 == 0x1C || (funct5 == 0x14 && funct3 != 2)) return 1 | 4;      // fmv.x.*, fclass, fle and flt leave FS alone (as qemu does; the architecture allows either)
      return 1;
    }
    case 0x43: case 0x47: case 0x4B: case 0x4F: return 1;
    case 0x57: case 0x77: return 2;
    default: return 0;
  }
}

bool RiscV64Cpu::sys_unit_enabled(uint32_t unit) const {
  return ((sys_.mstatus >> ((unit & 3) == 1 ? 13 : 9)) & 3) != 0;
}

void RiscV64Cpu::sys_unit_dirty(uint32_t unit) {
  if (unit & 4) return;
  sys_.mstatus |= (unit & 3) == 1 ? kFsField : kVsField;
}

// --- the CSRs ------------------------------------------------------------------------------------------------------------------------------

bool RiscV64Cpu::sys_csr_read(uint32_t csr, uint64_t* out) {
  const uint64_t status = with_sd(sys_.mstatus);
  switch (csr) {
    case 0x001: case 0x002: case 0x003: if (!sys_unit_enabled(1)) return false; *out = csr == 1 ? s_.fcsr & 31 : csr == 2 ? (s_.fcsr >> 5) & 7 : s_.fcsr & 0xFF; return true;
    case 0x008: case 0x009: case 0x00A: case 0x00F: case 0xC20: case 0xC21: case 0xC22:
      if (!sys_unit_enabled(2)) return false;
      switch (csr) {
        case 0x008: *out = s_.vstart; break;
        case 0x009: *out = s_.vxsat; break;
        case 0x00A: *out = s_.vxrm; break;
        case 0x00F: *out = (s_.vxrm << 1) | s_.vxsat; break;
        case 0xC20: *out = s_.vl; break;
        case 0xC21: *out = s_.vtype; break;
        default: *out = 16; break;
      }
      return true;
    case 0xC00: case 0xC01: case 0xC02: {
      const uint32_t bit_index = csr - 0xC00;
      if (sys_.priv < 3 && !((sys_.mcounteren >> bit_index) & 1)) return false;
      if (sys_.priv == 0 && !((sys_.scounteren >> bit_index) & 1)) return false;
      *out = csr == 0xC00 ? retired_ : csr == 0xC01 ? bus_->time() : retired_;
      return true;
    }
    case 0xB00: *out = retired_; return true;
    case 0xB02: *out = retired_; return true;
    case 0x100: *out = status & kSstatusMask; return true;
    case 0x104: *out = sys_.mie & sys_.mideleg & kSupervisorInterrupts; return true;
    case 0x105: *out = sys_.stvec; return true;
    case 0x106: *out = sys_.scounteren; return true;
    case 0x10A: *out = sys_.senvcfg; return true;
    case 0x140: *out = sys_.sscratch; return true;
    case 0x141: *out = sys_.sepc; return true;
    case 0x142: *out = sys_.scause; return true;
    case 0x143: *out = sys_.stval; return true;
    case 0x144: *out = pending_interrupts() & sys_.mideleg & kSupervisorInterrupts; return true;
    case 0x14D:
      if (sys_.priv < 3 && (!(sys_.menvcfg & kEnvcfgStce) || !((sys_.mcounteren >> 1) & 1))) return false;
      *out = sys_.stimecmp;
      return true;
    case 0x180:
      if (sys_.priv == 1 && (sys_.mstatus & kTvm)) return false;
      *out = sys_.satp;
      return true;
    case 0x300: *out = status; return true;
    case 0x301: *out = kMisa; return true;
    case 0x302: *out = sys_.medeleg; return true;
    case 0x303: *out = sys_.mideleg; return true;
    case 0x304: *out = sys_.mie; return true;
    case 0x305: *out = sys_.mtvec; return true;
    case 0x306: *out = sys_.mcounteren; return true;
    case 0x30A: *out = sys_.menvcfg; return true;
    case 0x320: *out = sys_.mcountinhibit; return true;
    case 0x340: *out = sys_.mscratch; return true;
    case 0x341: *out = sys_.mepc; return true;
    case 0x342: *out = sys_.mcause; return true;
    case 0x343: *out = sys_.mtval; return true;
    case 0x344: *out = pending_interrupts(); return true;
    case 0xF11: case 0xF12: case 0xF13: case 0xF14: case 0xF15: *out = 0; return true;
    default: break;
  }
  if (csr >= 0x3A0 && csr <= 0x3AF) {
    if (csr & 1) return false;      // (pmpcfg1 and 3 do not exist on RV64)
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(sys_.pmpcfg[(csr - 0x3A0) * 4 + i]) << (8 * i);
    *out = v;
    return true;
  }
  if (csr >= 0x3B0 && csr <= 0x3BF) { *out = sys_.pmpaddr[csr - 0x3B0]; return true; }
  return false;
}

bool RiscV64Cpu::sys_csr_write(uint32_t csr, uint64_t v) {
  switch (csr) {
    case 0x001: s_.fcsr = (s_.fcsr & ~31u) | (static_cast<uint32_t>(v) & 31); sys_unit_dirty(1); return true;
    case 0x002: s_.fcsr = (s_.fcsr & ~0xE0u) | ((static_cast<uint32_t>(v) & 7) << 5); sys_unit_dirty(1); return true;
    case 0x003: s_.fcsr = static_cast<uint32_t>(v) & 0xFF; sys_unit_dirty(1); return true;
    case 0x008: s_.vstart = v & 127; sys_unit_dirty(2); return true;
    case 0x009: s_.vxsat = v & 1; sys_unit_dirty(2); return true;
    case 0x00A: s_.vxrm = v & 3; sys_unit_dirty(2); return true;
    case 0x00F: s_.vxsat = v & 1; s_.vxrm = (v >> 1) & 3; sys_unit_dirty(2); return true;
    case 0xB00: case 0xB02: retired_ = v; return true;
    case 0x100: sys_.mstatus = (sys_.mstatus & ~(kMstatusWritable & kSstatusMask)) | (v & kMstatusWritable & kSstatusMask); return true;
    case 0x104: {
      const uint64_t mask = sys_.mideleg & kSupervisorInterrupts;
      sys_.mie = (sys_.mie & ~mask) | (v & mask);
      return true;
    }
    case 0x105: sys_.stvec = (v & 3) > 1 ? (sys_.stvec) : (v & ~uint64_t{2}); return true;
    case 0x106: sys_.scounteren = v & 7; return true;
    case 0x10A: sys_.senvcfg = v & 0xF1; return true;
    case 0x140: sys_.sscratch = v; return true;
    case 0x141: sys_.sepc = v & ~uint64_t{1}; return true;
    case 0x142: sys_.scause = v; return true;
    case 0x143: sys_.stval = v; return true;
    case 0x144: sys_.mip = (sys_.mip & ~(sys_.mideleg & 2)) | (v & sys_.mideleg & 2); return true;      // (only SSIP is writable from S mode)
    case 0x14D:
      if (sys_.priv < 3 && (!(sys_.menvcfg & kEnvcfgStce) || !((sys_.mcounteren >> 1) & 1))) return false;
      sys_.stimecmp = v;
      return true;
    case 0x180: {
      if (sys_.priv == 1 && (sys_.mstatus & kTvm)) return false;
      const uint64_t mode = v >> 60;
      if (mode == 0 || mode == 8 || mode == 9 || mode == 10) sys_.satp = v & ((uint64_t{0xF} << 60) | (uint64_t{0xFFFF} << 44) | kPpnMask);
      return true;
    }
    case 0x300: {
      uint64_t status = (sys_.mstatus & ~kMstatusWritable) | (v & kMstatusWritable);
      if (((status >> 11) & 3) == 2) status = (status & ~kMpp) | (sys_.mstatus & kMpp);      // (a reserved MPP leaves the old one)
      sys_.mstatus = status;
      return true;
    }
    case 0x301: return true;
    case 0x302: sys_.medeleg = v & kMedelegWritable; return true;
    case 0x303: sys_.mideleg = v & kSupervisorInterrupts; return true;
    case 0x304: sys_.mie = v & (kSupervisorInterrupts | kMachineInterrupts); return true;
    case 0x305: sys_.mtvec = (v & 3) > 1 ? sys_.mtvec : (v & ~uint64_t{2}); return true;
    case 0x306: sys_.mcounteren = v & 7; return true;
    case 0x30A: sys_.menvcfg = v & (kEnvcfgStce | 0xF1); return true;
    case 0x320: sys_.mcountinhibit = v & 5; return true;
    case 0x340: sys_.mscratch = v; return true;
    case 0x341: sys_.mepc = v & ~uint64_t{1}; return true;
    case 0x342: sys_.mcause = v; return true;
    case 0x343: sys_.mtval = v; return true;
    case 0x344: sys_.mip = v & kSupervisorInterrupts; return true;
    default: break;
  }
  if (csr >= 0x3A0 && csr <= 0x3AF) {
    if (csr & 1) return false;
    for (uint32_t i = 0; i < 8; ++i) {
      const uint32_t entry = (csr - 0x3A0) * 4 + i;
      if (sys_.pmpcfg[entry] & 0x80) continue;      // locked
      uint8_t cfg = static_cast<uint8_t>(v >> (8 * i));
      if ((cfg & 3) == 2) continue;      // (writable without readable is reserved: the write is ignored)
      sys_.pmpcfg[entry] = cfg & 0x9F;
    }
    return true;
  }
  if (csr >= 0x3B0 && csr <= 0x3BF) {
    const uint32_t i = csr - 0x3B0;
    const bool next_tor_locked = i + 1 < 16 && (sys_.pmpcfg[i + 1] & 0x80) && ((sys_.pmpcfg[i + 1] >> 3) & 3) == 1;
    if (!(sys_.pmpcfg[i] & 0x80) && !next_tor_locked) sys_.pmpaddr[i] = v & ((uint64_t{1} << 54) - 1);
    return true;
  }
  return false;
}

RiscV64Cpu::Step RiscV64Cpu::sys_csr(uint32_t insn) {
  const uint32_t rd = (insn >> 7) & 31, funct3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31, csr = insn >> 20;
  if (funct3 == 4) return undefined();
  const bool immediate = funct3 >= 5;
  const uint64_t operand = immediate ? rs1 : s_.x[rs1];
  const uint32_t kind = funct3 & 3;      // 1 write, 2 set, 3 clear
  const bool writes = kind == 1 || rs1 != 0;
  if (sys_.priv < ((csr >> 8) & 3)) return undefined();
  if (writes && (csr >> 10) == 3) return undefined();
  uint64_t old = 0;
  if (!sys_csr_read(csr, &old)) return undefined();
  if (writes) {
    const uint64_t v = kind == 1 ? operand : kind == 2 ? (old | operand) : (old & ~operand);
    if (!sys_csr_write(csr, v)) return undefined();
  }
  if (rd != 0) s_.x[rd] = old;
  return Step::Ok;
}

}  // namespace emulation
