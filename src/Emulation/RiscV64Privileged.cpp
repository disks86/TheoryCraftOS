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

constexpr uint64_t kMpv = uint64_t{1} << 39, kGvaBit = uint64_t{1} << 38;
constexpr uint64_t kSie = 1u << 1, kMie = 1u << 3, kSpie = 1u << 5, kMpie = 1u << 7, kSpp = 1u << 8, kMpp = 3u << 11, kMprv = 1u << 17, kSum = 1u << 18, kMxr = 1u << 19, kTvm = 1u << 20,
                   kTw = 1u << 21, kTsr = 1u << 22;
constexpr uint64_t kVsField = 3u << 9, kFsField = 3u << 13, kXsField = 3u << 15;
constexpr uint64_t kUxl = uint64_t{2} << 32, kSxl = uint64_t{2} << 34, kSd = uint64_t{1} << 63;
constexpr uint64_t kMstatusWritable = kSie | kMie | kSpie | kMpie | kSpp | kVsField | kMpp | kFsField | kMprv | kSum | kMxr | kTvm | kTw | kTsr;
constexpr uint64_t kSstatusMask = kSie | kSpie | kSpp | kVsField | kFsField | kXsField | kSum | kMxr | kUxl | kSd;
constexpr uint64_t kSupervisorInterrupts = (1u << 1) | (1u << 5) | (1u << 9);      // SSI, STI, SEI
constexpr uint64_t kMachineInterrupts = (1u << 3) | (1u << 7) | (1u << 11);
constexpr uint64_t kMedelegWritable = 0xB7FF | 0xF00000;      // not the machine ecall (11), the reserved 10 and 14
constexpr uint64_t kMisa = (uint64_t{2} << 62) | (1u << 0) | (1u << 2) | (1u << 7) | (1u << 3) | (1u << 5) | (1u << 8) | (1u << 12) | (1u << 18) | (1u << 20) | (1u << 21);      // A C D F I M S U V
constexpr uint64_t kEnvcfgStce = uint64_t{1} << 63, kEnvcfgPbmte = uint64_t{1} << 62, kEnvcfgAdue = uint64_t{1} << 61;
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
  sys_.menvcfg = kEnvcfgPbmte;      // (Svpbmt is on from reset)
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

// One stage of translation: the supervisor page tables (satp or vsatp) or, with `gstage`, the hypervisor's (hgatp, whose root is four times as large and whose leaves have to be
// user pages). With `nested` the addresses of the page-table entries are guest-physical and go through the G-stage themselves.
RiscV64Cpu::Walk RiscV64Cpu::walk_stage(uint64_t address, Access access, uint64_t atp, bool gstage, uint8_t priv, bool sum, bool mxr, bool adue, bool pbmte, bool nested, uint8_t original_access) {
  const uint8_t access_fault = access == Access::Fetch ? 1 : access == Access::Load ? 5 : 7;
  const uint8_t page_fault = gstage ? (access == Access::Fetch ? 20 : access == Access::Load ? 21 : 23) : (access == Access::Fetch ? 12 : access == Access::Load ? 13 : 15);
  const uint64_t mode = atp >> 60;
  if (mode == 0) return {address, 0, true, 0};
  const uint32_t levels = mode == 8 ? 3 : mode == 9 ? 4 : 5;
  const uint32_t va_bits = 12 + 9 * levels;
  const uint64_t guest_tval = gstage ? address >> 2 : 0;
  if (gstage) {
    if (va_bits + 2 < 64 && (address >> (va_bits + 2)) != 0) return {0, page_fault, false, guest_tval};
  } else {
    const int64_t folded = static_cast<int64_t>(address << (64 - va_bits)) >> (64 - va_bits);
    if (static_cast<uint64_t>(folded) != address) return {0, page_fault, false, 0};
  }
  uint64_t table = (atp & kPpnMask) << 12;
  uint64_t pte = 0, pte_address = 0;
  uint32_t level = levels;
  for (;;) {
    --level;
    const uint64_t index_mask = (gstage && level == levels - 1) ? 2047 : 511;
    uint64_t entry_address = table + (((address >> (12 + 9 * level)) & index_mask) << 3);
    const uint64_t entry_gpa = entry_address;
    if (nested) {      // the entry's address is guest-physical
      const Walk g = walk_stage(entry_address, Access::Load, sys_.hgatp, true, 1, false, (sys_.mstatus & kMxr) != 0, (sys_.henvcfg & kEnvcfgAdue) != 0, (sys_.menvcfg & kEnvcfgPbmte) != 0, false, original_access);
      if (!g.ok) return {0, g.cause >= 20 ? static_cast<uint8_t>(original_access == 0 ? 20 : original_access == 1 ? 21 : 23) : g.cause, false, g.cause >= 20 ? entry_gpa >> 2 : 0};
      entry_address = g.address;
    }
    pte_address = entry_address;
    if (!pmp_allows(entry_address, 8, Access::Load, 1)) return {0, access_fault, false, 0};
    if (!bus_->read(entry_address, 8, &pte)) return {0, access_fault, false, 0};
    const uint64_t pbmt = (pte >> 61) & 3;
    if (!(pte & 1) || ((pte & 2) == 0 && (pte & 4) != 0) || ((pte >> 54) & 0x7F) != 0) return {0, page_fault, false, guest_tval};
    if (pbmt != 0 && (!pbmte || pbmt == 3)) return {0, page_fault, false, guest_tval};      // Svpbmt: the type bits are reserved unless enabled, and 3 always
    if ((pte & 0xE) == 0 && (pte >> 61) != 0) return {0, page_fault, false, guest_tval};      // (a pointer has no N or PBMT bits)
    if (pte & 0xE) break;      // a leaf
    if (level == 0) return {0, page_fault, false, guest_tval};
    table = ((pte >> 10) & kPpnMask) << 12;
  }
  const bool r = pte & 2, w = pte & 4, x = pte & 8, u = pte & 0x10;
  if (gstage) {
    if (!u) return {0, page_fault, false, guest_tval};      // (every G-stage access is a user access)
  } else {
    if (priv == 0 && !u) return {0, page_fault, false, 0};
    if (priv == 1 && u && (access == Access::Fetch || !sum)) return {0, page_fault, false, 0};
  }
  const bool allowed = (access == Access::Fetch || hlvx_) ? x : access == Access::Load ? (r || (x && mxr)) : w;
  if (!allowed) return {0, page_fault, false, gstage ? guest_tval : 0};
  const uint64_t ppn = (pte >> 10) & kPpnMask;
  if (level > 0 && (ppn & ((uint64_t{1} << (9 * level)) - 1)) != 0) return {0, page_fault, false, gstage ? guest_tval : 0};      // (a misaligned superpage)
  uint64_t effective_ppn = ppn;
  if (pte >> 63) {      // Svnapot: a 64 KiB region, whose PPN ends in 0b1000
    if (level != 0 || (ppn & 0xF) != 8) return {0, page_fault, false, gstage ? guest_tval : 0};
    effective_ppn = (ppn & ~uint64_t{0xF}) | ((address >> 12) & 0xF);
  }
  const bool need_dirty = access == Access::Store;
  if (!(pte & 0x40) || (need_dirty && !(pte & 0x80))) {
    if (!adue) return {0, page_fault, false, gstage ? guest_tval : 0};      // Svade: the software sets the bits
    pte |= 0x40 | (need_dirty ? 0x80 : 0);      // Svadu: the hart sets them
    if (!pmp_allows(pte_address, 8, Access::Store, 1) || !bus_->write(pte_address, 8, pte)) return {0, access_fault, false, 0};
  }
  const uint64_t offset_mask = (uint64_t{1} << (12 + 9 * level)) - 1;
  uint64_t out = ((effective_ppn << 12) & ~offset_mask) | (address & offset_mask);
  if (pte >> 63) out = (effective_ppn << 12) | (address & 0xFFF);
  return {out, 0, true, 0};
}

// The privilege and virtualization mode an access is made in: hlv / hsv force the guest's, MPRV substitutes the previous mode for data accesses.
bool RiscV64Cpu::access_context(Access access, uint8_t* priv, bool* virt) const {
  *priv = sys_.priv;
  *virt = sys_.v;
  if (hlv_) {
    *priv = hlv_priv_;
    *virt = true;
  } else if (access != Access::Fetch && sys_.priv == 3 && (sys_.mstatus & kMprv)) {
    *priv = static_cast<uint8_t>((sys_.mstatus >> 11) & 3);
    *virt = *priv != 3 && (sys_.mstatus & kMpv) != 0;
  }
  return true;
}

RiscV64Cpu::Translation RiscV64Cpu::translate(uint64_t address, Access access, uint32_t size) {
  uint8_t priv;
  bool virt;
  access_context(access, &priv, &virt);
  const uint8_t access_fault = access == Access::Fetch ? 1 : access == Access::Load ? 5 : 7;
  const uint8_t original = access == Access::Fetch ? 0 : access == Access::Load ? 1 : 2;
  uint64_t physical = address;
  bool mxr = (sys_.mstatus & kMxr) != 0;
  if (priv != 3 && !virt && (sys_.satp >> 60) != 0) {
    const Walk w = walk_stage(address, access, sys_.satp, false, priv, (sys_.mstatus & kSum) != 0, mxr, (sys_.menvcfg & kEnvcfgAdue) != 0, (sys_.menvcfg & kEnvcfgPbmte) != 0, false, original);
    if (!w.ok) return {0, w.cause, false, 0, false};
    physical = w.address;
  } else if (priv != 3 && virt) {
    uint64_t gpa = address;
    if ((sys_.vsatp >> 60) != 0) {
      const Walk w = walk_stage(address, access, sys_.vsatp, false, priv, (sys_.vsstatus & kSum) != 0, mxr || (sys_.vsstatus & kMxr), (sys_.henvcfg & kEnvcfgAdue) != 0, (sys_.henvcfg & kEnvcfgPbmte) != 0, true, original);
      if (!w.ok) return {0, w.cause, false, w.tval2, true};
      gpa = w.address;
    }
    if ((sys_.hgatp >> 60) != 0) {
      const Walk g = walk_stage(gpa, access, sys_.hgatp, true, 1, false, mxr, (sys_.henvcfg & kEnvcfgAdue) != 0, (sys_.menvcfg & kEnvcfgPbmte) != 0, false, original);
      if (!g.ok) return {0, g.cause, false, g.tval2, true};
      physical = g.address;
    } else {
      physical = gpa;
    }
  }
  if (!pmp_allows(physical, size, access, priv)) return {0, access_fault, false, 0, virt};
  return {physical, 0, true, 0, virt};
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
      trap_tval_ = done == 0 ? address : address + done;
      trap_tval2_ = t.tval2;
      trap_gva_ = t.guest;
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
      trap_tval2_ = t.tval2;
      trap_gva_ = t.guest;
      s_.pc = instruction_start_;
      return false;
    }
    done += part;
  }
  return true;
}

RiscV64Cpu::Step RiscV64Cpu::sys_fail(uint8_t vector, uint64_t address) {
  s_.pc = instruction_start_;
  trap_tval2_ = 0;
  trap_gva_ = sys_.v;      // (qemu sets GVA for every exception taken from a guest; the architecture only needs it for those with an address)
  switch (vector) {
    case 0: trap_cause_ = 0; trap_tval_ = address; break;      // instruction address misaligned
    case 3: trap_cause_ = 3; trap_tval_ = instruction_start_; break;
    case 13: trap_cause_ = (raw_instruction_ >> 27) == 2 && (raw_instruction_ & 0x7F) == 0x2F ? 4 : 6; trap_tval_ = address; break;
    case 22: trap_cause_ = 22; trap_tval_ = raw_instruction_; break;      // virtual instruction
    default: trap_cause_ = 2; trap_tval_ = raw_instruction_; break;
  }
  return Step::Fault;
}

RiscV64Cpu::Step RiscV64Cpu::virtual_instruction() { return sys_fail(22, 0); }

// --- traps ---------------------------------------------------------------------------------------------------------------------------------

void RiscV64Cpu::take_trap(uint64_t cause, uint64_t tval, bool interrupt) {
  const uint64_t pc = s_.pc;
  const bool from_virtual = sys_.v;
  const bool delegated = sys_.priv <= 1 && (((interrupt ? (sys_.mideleg | 0x1444) : sys_.medeleg) >> cause) & 1);
  const bool to_vs = delegated && from_virtual && (((interrupt ? sys_.hideleg : sys_.hedeleg) >> cause) & 1);
  const bool address_cause = cause == 0 || cause == 1 || cause == 4 || cause == 5 || cause == 6 || cause == 7 || cause == 12 || cause == 13 || cause == 15 || cause == 20 || cause == 21 || cause == 23;
  const bool hs_gva = trap_gva_ && !interrupt && address_cause;
  const bool m_gva = (trap_gva_ || from_virtual) && !interrupt;      // (qemu sets GVA in machine mode for every exception taken from a guest)
  reservation_valid_ = false;
  if (to_vs) {
    const uint64_t code = interrupt ? (uint64_t{1} << 63) | (cause - 1) : cause;      // (a VS interrupt shows as the supervisor one: 2, 6, 10 become 1, 5, 9)
    sys_.vsepc = pc;
    sys_.vscause = code;
    sys_.vstval = tval;
    uint64_t status = sys_.vsstatus;
    status = (status & kSie) ? (status | kSpie) : (status & ~kSpie);
    status &= ~kSie;
    status = sys_.priv == 1 ? (status | kSpp) : (status & ~kSpp);
    sys_.vsstatus = status;
    sys_.priv = 1;
    const uint64_t base = sys_.vstvec & ~uint64_t{3};
    s_.pc = (interrupt && (sys_.vstvec & 3) == 1) ? base + 4 * (cause - 1) : base;
  } else if (delegated) {
    const uint64_t code = interrupt ? (uint64_t{1} << 63) | cause : cause;
    sys_.sepc = pc;
    sys_.scause = code;
    sys_.stval = tval;
    uint64_t status = sys_.mstatus;
    status = (status & kSie) ? (status | kSpie) : (status & ~kSpie);
    status &= ~kSie;
    status = sys_.priv == 1 ? (status | kSpp) : (status & ~kSpp);
    sys_.mstatus = status;
    uint64_t h = sys_.hstatus & ~((uint64_t{1} << 7) | (uint64_t{1} << 6));
    if (from_virtual) h |= (uint64_t{1} << 7) | (sys_.priv == 1 ? uint64_t{1} << 8 : 0);
    if (from_virtual) h = (h & ~(uint64_t{1} << 8)) | (sys_.priv == 1 ? uint64_t{1} << 8 : 0);
    if (hs_gva) h |= uint64_t{1} << 6;
    sys_.hstatus = h;
    sys_.htval = interrupt ? 0 : trap_tval2_;
    sys_.htinst = 0;
    sys_.v = false;
    sys_.priv = 1;
    const uint64_t base = sys_.stvec & ~uint64_t{3};
    s_.pc = (interrupt && (sys_.stvec & 3) == 1) ? base + 4 * cause : base;
  } else {
    const uint64_t code = interrupt ? (uint64_t{1} << 63) | cause : cause;
    sys_.mepc = pc;
    sys_.mcause = code;
    sys_.mtval = tval;
    uint64_t status = sys_.mstatus;
    status = (status & kMie) ? (status | kMpie) : (status & ~kMpie);
    const bool keep_gva = from_virtual && !interrupt && (cause == 8 || cause == 10);      // (qemu leaves GVA alone for an ecall from a guest)
    status &= ~(kMie | kMpv | (keep_gva ? 0 : kGvaBit));
    status = (status & ~kMpp) | (static_cast<uint64_t>(sys_.priv) << 11);
    if (from_virtual) status |= kMpv;
    if (m_gva && !keep_gva) status |= kGvaBit;
    sys_.mstatus = status;
    sys_.mtval2 = interrupt ? 0 : trap_tval2_;
    sys_.mtinst = 0;
    sys_.v = false;
    sys_.priv = 3;
    const uint64_t base = sys_.mtvec & ~uint64_t{3};
    s_.pc = (interrupt && (sys_.mtvec & 3) == 1) ? base + 4 * cause : base;
  }
  trap_gva_ = false;
  trap_tval2_ = 0;
}

uint64_t RiscV64Cpu::pending_interrupts() {
  uint64_t hardware = bus_->interrupts();
  uint64_t pending = sys_.mip | hardware | sys_.hvip;
  if ((sys_.menvcfg & kEnvcfgStce) != 0) {      // Sstc: the supervisor timer comes from stimecmp
    pending &= ~(uint64_t{1} << 5);
    if (bus_->time() >= sys_.stimecmp) pending |= uint64_t{1} << 5;
  }
  if ((sys_.henvcfg & kEnvcfgStce) != 0 && bus_->time() + sys_.htimedelta >= sys_.vstimecmp) pending |= uint64_t{1} << 6;      // the guest's timer
  return pending;
}

bool RiscV64Cpu::take_interrupt() {
  const uint64_t pending = pending_interrupts() & sys_.mie;
  if (pending == 0) return false;
  const bool machine_open = sys_.priv < 3 || (sys_.mstatus & kMie);
  const uint64_t vs_bits = 0x444;
  // delegated to the guest only when the hypervisor delegated them further and we are in it
  const uint64_t mideleg = sys_.mideleg | 0x1444;      // (the VS interrupts and SGEI always go below machine mode)
  const uint64_t to_machine = machine_open ? pending & ~mideleg : 0;
  const uint64_t delegated = pending & mideleg;
  const bool supervisor_open = sys_.v || sys_.priv < 1 || (sys_.priv == 1 && (sys_.mstatus & kSie));
  const uint64_t to_hs = supervisor_open ? delegated & ~(sys_.hideleg & vs_bits) : 0;
  const bool guest_open = sys_.v && (sys_.priv < 1 || (sys_.priv == 1 && (sys_.vsstatus & kSie)));
  const uint64_t to_vs = guest_open ? delegated & sys_.hideleg & vs_bits : 0;
  const uint64_t usable = to_machine | to_hs | to_vs;
  if (usable == 0) return false;
  static const uint8_t order[10] = {11, 3, 7, 9, 1, 5, 12, 10, 2, 6};
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
    trap_gva_ = false;
    trap_tval2_ = 0;
    const Step r = step(*placeholder_);
    hlv_ = false;
    hlvx_ = false;
    if (r == Step::Fault) {
      take_trap(trap_cause_, trap_tval_, false);
    } else {
      ++retired_;
    }
  }
  return StopReason::Budget;
}

// --- mret, sret, wfi, sfence.vma, hfence, hlv / hsv, ecall, ebreak ----------------------------------------------------------------------------

RiscV64Cpu::Step RiscV64Cpu::sys_instruction(uint32_t insn) {
  const uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, funct7 = insn >> 25;
  const bool vu_or_vs = sys_.v;
  if (insn == 0x00000073) {      // ecall
    trap_cause_ = sys_.priv == 0 ? 8 : sys_.priv == 1 ? (sys_.v ? 10 : 9) : 11;
    trap_tval_ = 0;
    trap_gva_ = sys_.v;
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
    sys_.v = target != 3 && (status & kMpv) != 0;
    status &= ~kMpv;
    if (target != 3) status &= ~kMprv;
    sys_.mstatus = status;
    sys_.priv = target;
    s_.pc = sys_.mepc;
    jumped_ = true;
    return Step::Ok;
  }
  if (insn == 0x10200073) {      // sret
    if (sys_.priv < 1) return undefined();
    if (sys_.priv == 1 && !vu_or_vs && (sys_.mstatus & kTsr)) return undefined();
    if (vu_or_vs && (sys_.priv == 0 || (sys_.hstatus & (uint64_t{1} << 22)))) return virtual_instruction();
    if (vu_or_vs) {      // in the guest: its own sstatus (vsstatus) and sepc
      uint64_t status = sys_.vsstatus;
      sys_.priv = (status & kSpp) ? 1 : 0;
      status = (status & kSpie) ? (status | kSie) : (status & ~kSie);
      status |= kSpie;
      status &= ~kSpp;
      sys_.vsstatus = status;
      s_.pc = sys_.vsepc;
    } else {
      const uint8_t target = (sys_.mstatus & kSpp) ? 1 : 0;
      uint64_t status = sys_.mstatus;
      status = (status & kSpie) ? (status | kSie) : (status & ~kSie);
      status |= kSpie;
      status &= ~kSpp;
      status &= ~kMprv;
      sys_.mstatus = status;
      sys_.priv = target;
      sys_.v = (sys_.hstatus & (uint64_t{1} << 7)) != 0;
      sys_.hstatus &= ~(uint64_t{1} << 7);
      s_.pc = sys_.sepc;
    }
    jumped_ = true;
    return Step::Ok;
  }
  if (insn == 0x10500073) {      // wfi
    if (sys_.priv == 0 && !vu_or_vs) return undefined();
    if (sys_.priv < 3 && (sys_.mstatus & kTw)) return undefined();
    if (sys_.priv == 0 && vu_or_vs) return virtual_instruction();
    if (vu_or_vs && (sys_.hstatus & (uint64_t{1} << 21))) return virtual_instruction();
    if ((pending_interrupts() & sys_.mie) == 0) bus_->idle((sys_.menvcfg & kEnvcfgStce) ? sys_.stimecmp : ~uint64_t{0});
    return Step::Ok;
  }
  if (rd == 0 && ((funct7 == 0x09) || (funct7 == 0x0B) || (funct7 == 0x0C && rs2 <= 1 && rs1 == 0))) {      // sfence.vma, sinval.vma, sfence.w.inval / sfence.inval.ir
    if (sys_.priv == 0 && !vu_or_vs) return undefined();
    if (sys_.priv == 1 && !vu_or_vs && (sys_.mstatus & kTvm) && funct7 != 0x0C) return undefined();
    if (vu_or_vs && (sys_.priv == 0 || (sys_.hstatus & (uint64_t{1} << 20)))) return virtual_instruction();
    return Step::Ok;
  }
  if (rd == 0 && (funct7 == 0x11 || funct7 == 0x31 || funct7 == 0x13 || funct7 == 0x33)) {      // hfence.vvma, hfence.gvma (and the Svinval forms hinval.vvma, hinval.gvma)
    if (vu_or_vs) return virtual_instruction();
    if (sys_.priv == 0) return undefined();
    if (sys_.priv == 1 && (funct7 & 0x20) && (sys_.mstatus & kTvm)) return undefined();      // (hfence.gvma is under TVM)
    return Step::Ok;
  }
  return undefined();
}

// hlv.* and hsv.*: loads and stores made as the guest (two-stage translation, the privilege of hstatus.SPVP)
RiscV64Cpu::Step RiscV64Cpu::sys_hlv(uint32_t insn) {
  const uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, funct7 = insn >> 25;
  const bool store = (funct7 & 1) != 0;
  const uint32_t op = funct7 >> 1;      // 0x18 b, 0x19 h, 0x1A w, 0x1B d
  if (op < 0x18 || op > 0x1B) return undefined();
  if (op == 0x1B && false) return undefined();
  if (store && rd != 0) return undefined();
  if (!store && op == 0x1B && rs2 > 0) return undefined();
  if (!store && op == 0x18 && rs2 > 1) return undefined();
  if (!store && op == 0x19 && rs2 != 0 && rs2 != 1 && rs2 != 3) return undefined();
  if (!store && op == 0x1A && rs2 != 0 && rs2 != 1 && rs2 != 3) return undefined();
  if (store && false) return undefined();
  if (sys_.v) return virtual_instruction();
  if (sys_.priv == 0 && !(sys_.hstatus & (uint64_t{1} << 9))) return undefined();
  const uint32_t size = op == 0x18 ? 1 : op == 0x19 ? 2 : op == 0x1A ? 4 : 8;
  hlv_ = true;
  hlv_priv_ = (sys_.hstatus & (uint64_t{1} << 8)) ? 1 : 0;
  const uint64_t address = s_.x[rs1];
  if (store) {
    if (!sys_write(address, size, s_.x[rs2])) return Step::Fault;
    return Step::Ok;
  }
  uint64_t v = 0;
  const bool executable = rs2 == 3;      // hlvx: needs execute permission (and fails as a load does)
  if (!sys_read(address, size, executable ? Access::Fetch : Access::Load, &v)) return Step::Fault;
  const bool zero = rs2 == 1 || executable;
  if (!zero) v = static_cast<uint64_t>(static_cast<int64_t>(v << (64 - 8 * size)) >> (64 - 8 * size));
  if (rd != 0) s_.x[rd] = v;
  return Step::Ok;
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
  const uint32_t shift = (unit & 3) == 1 ? 13 : 9;
  if (((sys_.mstatus >> shift) & 3) == 0) return false;
  return !sys_.v || ((sys_.vsstatus >> shift) & 3) != 0;      // (a guest needs its own field on as well)
}

void RiscV64Cpu::sys_unit_dirty(uint32_t unit) {
  if (unit & 4) return;
  const uint64_t field = (unit & 3) == 1 ? kFsField : kVsField;
  sys_.mstatus |= field;
  if (sys_.v) sys_.vsstatus |= field;
}

// --- the CSRs ------------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr uint64_t kVsInterrupts = 0x444, kHsInterrupts = 0x1444;      // VSSIP, VSTIP, VSEIP; and SGEIP too
constexpr uint64_t kHstatusWritable = (uint64_t{1} << 6) | (uint64_t{1} << 7) | (uint64_t{1} << 8) | (uint64_t{1} << 9) | (uint64_t{1} << 20) | (uint64_t{1} << 21) | (uint64_t{1} << 22);
}

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
      *out = csr == 0xC00 ? retired_ : csr == 0xC01 ? bus_->time() + (sys_.v ? sys_.htimedelta : 0) : retired_;
      return true;
    }
    case 0xB00: *out = retired_; return true;
    case 0xB02: *out = retired_; return true;
    case 0x100: *out = status & kSstatusMask; return true;
    case 0x104: *out = sys_.mie & sys_.mideleg & 0x222; return true;
    case 0x105: *out = sys_.stvec; return true;
    case 0x106: *out = sys_.scounteren; return true;
    case 0x10A: *out = sys_.senvcfg; return true;
    case 0x140: *out = sys_.sscratch; return true;
    case 0x141: *out = sys_.sepc; return true;
    case 0x142: *out = sys_.scause; return true;
    case 0x143: *out = sys_.stval; return true;
    case 0x144: *out = pending_interrupts() & sys_.mideleg & 0x222; return true;
    case 0x14D: *out = sys_.stimecmp; return true;
    case 0x180:
      if (sys_.priv == 1 && !sys_.v && (sys_.mstatus & kTvm)) return false;
      *out = sys_.satp;
      return true;
    case 0x300: *out = status; return true;
    case 0x301: *out = kMisa; return true;
    case 0x302: *out = sys_.medeleg; return true;
    case 0x303: *out = sys_.mideleg | 0x1444; return true;
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
    case 0x34A: *out = sys_.mtinst; return true;
    case 0x34B: *out = sys_.mtval2; return true;
    case 0xF11: case 0xF12: case 0xF13: case 0xF14: case 0xF15: *out = 0; return true;
    // the hypervisor
    case 0x600: *out = sys_.hstatus; return true;
    case 0x602: *out = sys_.hedeleg; return true;
    case 0x603: *out = sys_.hideleg; return true;
    case 0x604: *out = sys_.mie & kHsInterrupts; return true;
    case 0x605: *out = sys_.htimedelta; return true;
    case 0x606: *out = sys_.hcounteren; return true;
    case 0x607: *out = sys_.hgeie; return true;
    case 0x60A: *out = sys_.henvcfg; return true;
    case 0x643: *out = sys_.htval; return true;
    case 0x644: *out = pending_interrupts() & kHsInterrupts; return true;
    case 0x645: *out = sys_.hvip; return true;
    case 0x64A: *out = sys_.htinst; return true;
    case 0x680: *out = sys_.hgatp; return true;
    case 0xE12: *out = 0; return true;      // hgeip
    case 0x200: *out = sys_.vsstatus & ((kSstatusMask | kSxl) & ~kSd); return true;
    case 0x204: *out = ((sys_.mie >> 1) & (sys_.hideleg >> 1)) & 0x222; return true;
    case 0x205: *out = sys_.vstvec; return true;
    case 0x240: *out = sys_.vsscratch; return true;
    case 0x241: *out = sys_.vsepc; return true;
    case 0x242: *out = sys_.vscause; return true;
    case 0x243: *out = sys_.vstval; return true;
    case 0x244: *out = ((pending_interrupts() >> 1) & (sys_.hideleg >> 1)) & 0x222; return true;
    case 0x24D: *out = sys_.vstimecmp; return true;
    case 0x280: *out = sys_.vsatp; return true;
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
      const uint64_t mask = sys_.mideleg & 0x222;
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
    case 0x14D: sys_.stimecmp = v; return true;
    case 0x180: {
      const uint64_t mode = v >> 60;
      if (mode == 0 || mode == 8 || mode == 9 || mode == 10) sys_.satp = v & ((uint64_t{0xF} << 60) | (uint64_t{0xFFFF} << 44) | kPpnMask);
      return true;
    }
    case 0x300: {
      const uint64_t writable = kMstatusWritable | kMpv | kGvaBit;
      uint64_t status = (sys_.mstatus & ~writable) | (v & writable);
      if (((status >> 11) & 3) == 2) status = (status & ~kMpp) | (sys_.mstatus & kMpp);      // (a reserved MPP leaves the old one)
      sys_.mstatus = status;
      return true;
    }
    case 0x301: return true;
    case 0x302: sys_.medeleg = v & kMedelegWritable; return true;
    case 0x303: sys_.mideleg = v & kSupervisorInterrupts; return true;
    case 0x304: sys_.mie = v & (kSupervisorInterrupts | kMachineInterrupts | kHsInterrupts); return true;
    case 0x305: sys_.mtvec = (v & 3) > 1 ? sys_.mtvec : (v & ~uint64_t{2}); return true;
    case 0x306: sys_.mcounteren = v & 7; return true;
    case 0x30A: sys_.menvcfg = v & (kEnvcfgStce | kEnvcfgPbmte | kEnvcfgAdue | 0xF1); return true;
    case 0x320: sys_.mcountinhibit = v & 5; return true;
    case 0x340: sys_.mscratch = v; return true;
    case 0x341: sys_.mepc = v & ~uint64_t{1}; return true;
    case 0x342: sys_.mcause = v; return true;
    case 0x343: sys_.mtval = v; return true;
    case 0x344: sys_.mip = v & (kSupervisorInterrupts); sys_.hvip = v & kVsInterrupts; return true;
    case 0x34A: sys_.mtinst = v; return true;
    case 0x34B: sys_.mtval2 = v; return true;
    case 0x600: sys_.hstatus = (sys_.hstatus & ~kHstatusWritable) | (v & kHstatusWritable); return true;
    case 0x602: sys_.hedeleg = v & 0xB1FF; return true;
    case 0x603: sys_.hideleg = v & kVsInterrupts; return true;
    case 0x604: sys_.mie = (sys_.mie & ~kHsInterrupts) | (v & kHsInterrupts); return true;
    case 0x605: sys_.htimedelta = v; return true;
    case 0x606: sys_.hcounteren = v & 7; return true;
    case 0x607: return true;
    case 0x60A: sys_.henvcfg = v & ((sys_.menvcfg & (kEnvcfgStce | kEnvcfgPbmte | kEnvcfgAdue)) | 0xF1); return true;
    case 0x643: sys_.htval = v; return true;
    case 0x644: sys_.hvip = (sys_.hvip & ~uint64_t{4}) | (v & 4); return true;      // (only VSSIP is writable here)
    case 0x645: sys_.hvip = v & kVsInterrupts; return true;
    case 0x64A: return true;      // (htinst is written by traps only)
    case 0x680: {
      const uint64_t mode = v >> 60;
      if (mode == 0 || mode == 8 || mode == 9 || mode == 10) sys_.hgatp = v & ((uint64_t{0xF} << 60) | (uint64_t{0x3FFF} << 44) | (kPpnMask & ~uint64_t{3}));
      return true;
    }
    case 0x200: sys_.vsstatus = kUxl | (v & kMstatusWritable & kSstatusMask); return true;      // (qemu clears SXL on a write)
    case 0x204: {
      const uint64_t mask = (sys_.hideleg & kVsInterrupts);
      sys_.mie = (sys_.mie & ~mask) | ((v << 1) & mask);
      return true;
    }
    case 0x205: sys_.vstvec = (v & 3) > 1 ? sys_.vstvec : (v & ~uint64_t{2}); return true;
    case 0x240: sys_.vsscratch = v; return true;
    case 0x241: sys_.vsepc = v & ~uint64_t{1}; return true;
    case 0x242: sys_.vscause = v; return true;
    case 0x243: sys_.vstval = v; return true;
    case 0x244: if (sys_.hideleg & 4) sys_.hvip = (sys_.hvip & ~uint64_t{4}) | ((v << 1) & 4); return true;
    case 0x24D: sys_.vstimecmp = v; return true;
    case 0x280: {
      const uint64_t mode = v >> 60;
      if (mode == 0 || mode == 8 || mode == 9 || mode == 10) sys_.vsatp = v & ((uint64_t{0xF} << 60) | (uint64_t{0xFFFF} << 44) | kPpnMask);
      return true;
    }
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
  const uint32_t rd = (insn >> 7) & 31, funct3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31;
  uint32_t csr = insn >> 20;
  if (funct3 == 4) return undefined();
  const bool immediate = funct3 >= 5;
  const uint64_t operand = immediate ? rs1 : s_.x[rs1];
  const uint32_t kind = funct3 & 3;      // 1 write, 2 set, 3 clear
  const bool writes = kind == 1 || rs1 != 0;
  const uint32_t level = (csr >> 8) & 3;
  if (writes && (csr >> 10) == 3) return undefined();
  // privilege: the hypervisor's CSRs are for M and HS mode; a guest asking for them, or a VU asking for supervisor ones, gets a virtual-instruction exception
  const bool hypervisor_csr = (csr >= 0x600 && csr <= 0x6FF) || (csr >= 0x200 && csr <= 0x2FF) || csr == 0xE12;
  if (hypervisor_csr) {
    if (sys_.v) return virtual_instruction();
    if (sys_.priv < 1) return undefined();
    if (sys_.priv == 1 && csr == 0x680 && (sys_.mstatus & kTvm)) return undefined();
  } else if (sys_.priv < level) {
    if (sys_.v && level == 1) return virtual_instruction();
    return undefined();
  }
  // counters
  if (csr >= 0xC00 && csr <= 0xC02) {
    const uint32_t b = csr - 0xC00;
    if (sys_.priv < 3 && !((sys_.mcounteren >> b) & 1)) return undefined();
    if (sys_.v && !((sys_.hcounteren >> b) & 1)) return virtual_instruction();
    if (sys_.priv == 0 && !((sys_.scounteren >> b) & 1)) return sys_.v ? virtual_instruction() : undefined();
  }
  // stimecmp: Sstc has to be on, and the guest's has its own
  if (csr == 0x14D) {
    if (sys_.priv < 3 && !((sys_.mcounteren >> 1) & 1)) return undefined();
    if (sys_.priv < 3 && !(sys_.menvcfg & kEnvcfgStce)) return undefined();
    if (sys_.v) {
      if (!(sys_.hcounteren & 2) || !(sys_.henvcfg & kEnvcfgStce)) return virtual_instruction();
    }
  }
  if (csr == 0x180 && sys_.v && (sys_.priv == 0 || (sys_.hstatus & (uint64_t{1} << 20)))) return virtual_instruction();
  if (sys_.v && csr >= 0x100 && csr <= 0x180 && (csr == 0x100 || csr == 0x104 || csr == 0x105 || (csr >= 0x140 && csr <= 0x144) || csr == 0x14D || csr == 0x180)) csr += 0x100;      // the guest's own copies
  uint64_t old = 0;
  const bool guest_sstatus = sys_.v && (insn >> 20) == 0x100;
  if (!sys_csr_read(csr, &old)) return undefined();
  if (guest_sstatus) old &= kSstatusMask & ~kSd;      // (through sstatus the guest does not see the SXL field that vsstatus holds)
  if (writes) {
    const uint64_t v = kind == 1 ? operand : kind == 2 ? (old | operand) : (old & ~operand);
    if (!sys_csr_write(csr, v)) return undefined();
  }
  if (rd != 0) s_.x[rd] = old;
  return Step::Ok;
}

}  // namespace emulation
