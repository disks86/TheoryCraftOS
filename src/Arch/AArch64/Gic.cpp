/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

#include "Arch/AArch64/Gic.hpp"
#include "Utility/ByteStream.hpp"

namespace kernel::gic {

namespace {

// ---- MMIO access: the one place an address becomes a pointer. The addresses come from the firmware's
// MADT and the firmware maps device memory 1:1 (volatile 32/64-bit aligned accesses only).
uint32_t rd32(uint64_t address) { return *reinterpret_cast<volatile uint32_t*>(address); }
void wr32(uint64_t address, uint32_t value) { *reinterpret_cast<volatile uint32_t*>(address) = value; }
uint64_t rd64(uint64_t address) { return *reinterpret_cast<volatile uint64_t*>(address); }
void wr8(uint64_t address, uint8_t value) { *reinterpret_cast<volatile uint8_t*>(address) = value; }

// ---- GIC system registers (written by encoding so the assembler needs no GIC extension).
uint64_t read_sre() { uint64_t v; __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(v)); return v; }
void write_sre(uint64_t v) { __asm__ volatile("msr S3_0_C12_C12_5, %0; isb" ::"r"(v)); }
void write_sre_el2(uint64_t v) { __asm__ volatile("msr S3_4_C12_C9_5, %0; isb" ::"r"(v)); }
void write_pmr(uint64_t v) { __asm__ volatile("msr S3_0_C4_C6_0, %0; isb" ::"r"(v)); }
uint64_t read_ctlr() { uint64_t v; __asm__ volatile("mrs %0, S3_0_C12_C12_4" : "=r"(v)); return v; }
void write_ctlr(uint64_t v) { __asm__ volatile("msr S3_0_C12_C12_4, %0; isb" ::"r"(v)); }
void write_bpr1(uint64_t v) { __asm__ volatile("msr S3_0_C12_C12_3, %0" ::"r"(v)); }
void write_igrpen1(uint64_t v) { __asm__ volatile("msr S3_0_C12_C12_7, %0; isb" ::"r"(v)); }
uint64_t read_iar1() { uint64_t v; __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); return v; }
void write_eoir1(uint64_t v) { __asm__ volatile("msr S3_0_C12_C12_1, %0; isb" ::"r"(v)); }
void write_sgi1r(uint64_t v) { __asm__ volatile("msr S3_0_C12_C11_5, %0; isb" ::"r"(v)); }

uint64_t current_el() { uint64_t v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return v >> 2; }
uint64_t mpidr() { uint64_t v; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v)); return v; }

// MPIDR's affinity as GICR_TYPER[63:32] spells it: Aff3.Aff2.Aff1.Aff0.
uint32_t affinity_of(uint64_t m) {
  return static_cast<uint32_t>((m & 0x00FFFFFF) | (((m >> 32) & 0xFF) << 24));
}

// ---- distributor registers
constexpr uint64_t kGicdCtlr = 0x000, kGicdIsEnabler = 0x100, kGicdIcEnabler = 0x180, kGicdIPriority = 0x400, kGicdSgir = 0xF00;
// ---- GICv2 CPU interface
constexpr uint64_t kGiccCtlr = 0x00, kGiccPmr = 0x04, kGiccIar = 0x0C, kGiccEoir = 0x10;
// ---- GICv3 redistributor
constexpr uint64_t kGicrTyper = 0x08, kGicrWaker = 0x14, kSgiFrame = 0x10000, kGicrIGroupr0 = 0x80, kGicrIsEnabler0 = 0x100,
                   kGicrIcEnabler0 = 0x180, kGicrIPriority = 0x400;
constexpr uint8_t kPriority = 0x80;

struct State {
  uint32_t version = 0;
  uint64_t gicd = 0;
  uint64_t gicc = 0;          // v2: the CPU interface
  uint64_t gicr_base = 0;     // v3: start of the redistributor range (0 = per-core bases only)
  uint64_t gicr_length = 0;
  uint64_t gicr_this = 0;     // the redistributor frame of the boot core (for restore_firmware)
  uint32_t firmware_enabled0 = 0;   // the firmware's enable bits for interrupts 0-31
  uint32_t firmware_ctlr_cpu = 0;   // v2: GICC_CTLR; v3: ICC_CTLR_EL1
  bool touched_distributor = false;
};
State g;

// ---- MADT
constexpr uint32_t kMadtEntries = 44;
constexpr uint8_t kTypeGicc = 0xB, kTypeGicd = 0xC, kTypeGicr = 0xE;

bool read_madt(const hardware::AcpiTables& acpi) {
  const ConstByteView madt = acpi.find("APIC");
  if (madt.size() < kMadtEntries) return false;
  uint32_t hint_version = 0;
  uint64_t first_gicc_gicr = 0;
  for (uint32_t offset = kMadtEntries; offset + 2 <= madt.size();) {
    const uint8_t type = madt[offset], length = madt[offset + 1];
    if (length < 2 || length > madt.size() - offset) break;
    ByteReader r(madt.subview(offset, length));
    r.skip(2);
    if (type == kTypeGicd && length >= 24) {
      r.skip(2 + 4);  // reserved, GIC id
      g.gicd = r.le64();
      r.skip(4);  // system vector base
      hint_version = r.u8();
    } else if (type == kTypeGicc && length >= 68) {
      r.skip(2);                      // reserved
      r.skip(4 + 4 + 4 + 4 + 4);      // interface number, uid, flags, parking version, performance gsiv
      r.skip(8);                      // parked address
      const uint64_t base = r.le64(); // physical base address of the CPU interface
      r.skip(8 + 8 + 4);              // GICV, GICH, maintenance gsiv
      const uint64_t redist = r.le64();
      if (!g.gicc && base) g.gicc = base;
      if (!first_gicc_gicr && redist) first_gicc_gicr = redist;
    } else if (type == kTypeGicr && length >= 16) {
      r.skip(2);
      const uint64_t base = r.le64();
      const uint32_t len = r.le32();
      if (!g.gicr_base && base) {
        g.gicr_base = base;
        g.gicr_length = len;
      }
    }
    offset += length;
  }
  if (!g.gicd) return false;
  // The MADT's GIC version field decides; without it, redistributors mean GICv3 and a CPU interface GICv2.
  // (The distributor's own PIDR2 is not probed: its offset differs between the versions and a GICv2 distributor
  // is only 4 KiB, so reading the GICv3 offset can abort.)
  g.version = hint_version ? hint_version : (g.gicr_base || first_gicc_gicr ? 3 : 2);
  if (g.version >= 3) {
    g.version = 3;
    if (!g.gicr_base && first_gicc_gicr) {  // per-core redistributor addresses only: treat the first as a one-frame range
      g.gicr_base = first_gicc_gicr;
      g.gicr_length = 0x20000;
    }
    return g.gicr_base != 0;
  }
  g.version = 2;
  return g.gicc != 0;
}

// Finds the redistributor frame whose affinity matches this core.
uint64_t find_redistributor() {
  const uint32_t want = affinity_of(mpidr());
  for (uint64_t frame = g.gicr_base; frame - g.gicr_base < g.gicr_length; ) {
    const uint64_t typer = rd64(frame + kGicrTyper);
    if (static_cast<uint32_t>(typer >> 32) == want) return frame;
    if (typer & (1u << 4)) break;                  // Last
    frame += (typer & (1u << 1)) ? 0x40000 : 0x20000;   // VLPIS adds two more frames
  }
  return 0;
}

// The redistributor of each core that has brought its interface up, so a core finds its own frame without
// scanning the (MMIO) range again. A core claims a free entry, fills it in and publishes it by storing `frame`
// last; readers trust an entry once `frame` is non-zero.
constexpr uint32_t kMaxCachedCores = 256;
struct RedistributorEntry {
  uint32_t affinity;
  uint32_t claimed;   // set by the core that is filling the entry in
  uint64_t frame;
};
RedistributorEntry g_redistributors[kMaxCachedCores];

uint64_t this_redistributor() {
  const uint32_t want = affinity_of(mpidr());
  for (const RedistributorEntry& e : g_redistributors) {
    const uint64_t frame = __atomic_load_n(&e.frame, __ATOMIC_ACQUIRE);
    if (frame && e.affinity == want) return frame;
  }
  const uint64_t frame = find_redistributor();
  if (!frame) return 0;
  for (RedistributorEntry& e : g_redistributors) {
    uint32_t free_slot = 0;
    if (!__atomic_compare_exchange_n(&e.claimed, &free_slot, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
    e.affinity = want;
    __atomic_store_n(&e.frame, frame, __ATOMIC_RELEASE);
    break;
  }
  return frame;
}

void wake_redistributor(uint64_t frame) {
  uint32_t waker = rd32(frame + kGicrWaker);
  waker &= ~(1u << 1);                         // ProcessorSleep = 0
  wr32(frame + kGicrWaker, waker);
  for (uint32_t spin = 0; (rd32(frame + kGicrWaker) & (1u << 2)) && spin < 1000000; ++spin) {}   // ChildrenAsleep
}

void enable_private_v3(uint64_t frame) {
  const uint64_t sgi = frame + kSgiFrame;
  wr32(sgi + kGicrIGroupr0, 0xFFFFFFFF);
  for (uint32_t i = 0; i < 32; i += 4) wr32(sgi + kGicrIPriority + i, 0x80808080);
}

void enable_cpu_interface() {
  if (g.version >= 3) {
    if (current_el() == 2) write_sre_el2(0xF);
    write_sre(read_sre() | 1);
    write_pmr(0xFF);
    write_bpr1(0);
    write_ctlr(read_ctlr() & ~uint64_t{1u << 1});   // EOImode 0: the end-of-interrupt write also deactivates (no DIR needed)
    write_igrpen1(1);
  } else {
    wr32(g.gicc + kGiccPmr, 0xFF);
    wr32(g.gicc + kGiccCtlr, rd32(g.gicc + kGiccCtlr) | 1);
  }
}
}  // namespace

uint32_t version() { return g.version; }

bool init_boot_core(const hardware::AcpiTables& acpi) {
  if (g.version) return true;
  if (!read_madt(acpi)) return false;
  if (g.version >= 3) {
    g.gicr_this = this_redistributor();
    if (!g.gicr_this) { g.version = 0; return false; }
    g.firmware_enabled0 = rd32(g.gicr_this + kSgiFrame + kGicrIsEnabler0);
    wake_redistributor(g.gicr_this);
    // Firmware normally has the distributor on already; only turn it on (affinity routing, group 1) if it is not.
    if (!(rd32(g.gicd + kGicdCtlr) & (1u << 1))) {
      wr32(g.gicd + kGicdCtlr, (1u << 4) | (1u << 1));
      g.touched_distributor = true;
    }
    g.firmware_ctlr_cpu = static_cast<uint32_t>(read_ctlr());
    enable_private_v3(g.gicr_this);
  } else {
    g.firmware_enabled0 = rd32(g.gicd + kGicdIsEnabler);
    if (!(rd32(g.gicd + kGicdCtlr) & 1)) {
      wr32(g.gicd + kGicdCtlr, 1);
      g.touched_distributor = true;
    }
    g.firmware_ctlr_cpu = rd32(g.gicc + kGiccCtlr);
  }
  enable_cpu_interface();
  return true;
}

void enable_this_core() {
  if (g.version >= 3) {
    const uint64_t frame = this_redistributor();
    if (!frame) return;
    wake_redistributor(frame);
    enable_private_v3(frame);
  }
  enable_cpu_interface();
}

void restore_firmware() {
  if (!g.version) return;
  const uint64_t enable_reg = g.version >= 3 ? g.gicr_this + kSgiFrame + kGicrIsEnabler0 : g.gicd + kGicdIsEnabler;
  const uint64_t disable_reg = g.version >= 3 ? g.gicr_this + kSgiFrame + kGicrIcEnabler0 : g.gicd + kGicdIcEnabler;
  wr32(disable_reg, 0xFFFFFFFF);
  wr32(enable_reg, g.firmware_enabled0);
  if (g.version >= 3) write_ctlr(g.firmware_ctlr_cpu);
}

void enable_interrupt(uint32_t intid) {
  if (intid >= 1020) return;
  const bool redist = g.version >= 3 && intid < 32;
  const uint64_t base = redist ? this_redistributor() + kSgiFrame : g.gicd;
  wr8(base + (redist ? kGicrIPriority : kGicdIPriority) + intid, kPriority);
  wr32(base + (redist ? kGicrIsEnabler0 : kGicdIsEnabler) + 4 * (intid / 32), 1u << (intid % 32));
}

void disable_interrupt(uint32_t intid) {
  if (intid >= 1020) return;
  const bool redist = g.version >= 3 && intid < 32;
  const uint64_t base = redist ? this_redistributor() + kSgiFrame : g.gicd;
  wr32(base + (redist ? kGicrIcEnabler0 : kGicdIcEnabler) + 4 * (intid / 32), 1u << (intid % 32));
}

uint32_t acknowledge() {
  const uint32_t raw = g.version >= 3 ? static_cast<uint32_t>(read_iar1()) : rd32(g.gicc + kGiccIar);
  return raw & 0x3FF;
}

void end_of_interrupt(uint32_t intid) {
  if (g.version >= 3) write_eoir1(intid);
  else wr32(g.gicc + kGiccEoir, intid);
}

void send_sgi(uint64_t target_mpidr, uint32_t intid) {
  if (intid > 15) return;
  if (g.version >= 3) {
    const uint64_t aff0 = target_mpidr & 0xFF;
    // TargetList covers Aff0 values 0-15 within one Aff1/2/3 cluster; larger Aff0 values need a range selector.
    const uint64_t range = aff0 / 16;
    write_sgi1r((uint64_t{1} << (aff0 % 16)) | (((target_mpidr >> 8) & 0xFF) << 16) | (uint64_t{intid} << 24) |
                (((target_mpidr >> 16) & 0xFF) << 32) | (range << 44) | (((target_mpidr >> 32) & 0xFF) << 48));
  } else {
    wr32(g.gicd + kGicdSgir, (1u << (16 + (target_mpidr & 7))) | intid);
  }
}

}  // namespace kernel::gic
