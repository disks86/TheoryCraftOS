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

// The LoongArch control and status registers and IOCSR registers this kernel uses, and the thin wrappers around the
// compiler's builtins that read and write them. The CSR number must be a constant, so the accessors are templates.
#pragma once
#include <stdint.h>

namespace arch_loongarch64 {

// CSR numbers (LoongArch Reference Manual, volume 1, chapter 7).
constexpr uint32_t kCsrCrmd = 0x0, kCsrPrmd = 0x1, kCsrEcfg = 0x4, kCsrEstat = 0x5, kCsrEra = 0x6, kCsrBadv = 0x7;
constexpr uint32_t kCsrEentry = 0xC, kCsrCpuid = 0x20, kCsrSave0 = 0x30, kCsrSave1 = 0x31, kCsrTcfg = 0x41, kCsrTval = 0x42, kCsrTiclr = 0x44;

// CRMD / PRMD: the privilege level, interrupt enable and (CRMD) the address translation mode.
constexpr uint64_t kCrmdIe = 1u << 2, kCrmdDa = 1u << 3, kCrmdPg = 1u << 4;
constexpr uint64_t kPrmdPie = 1u << 2;
// ECFG.LIE and ESTAT.IS bit numbers: two software interrupts, eight hardware lines, the performance counter, the timer and the IPI.
constexpr uint32_t kIntTimer = 11, kIntIpi = 12;
constexpr uint64_t kIntMask = 0x1FFF;
// TCFG: enable, periodic, and the initial count in bits 63:2.
constexpr uint64_t kTcfgEnable = 1, kTcfgPeriodic = 2;
// ESTAT.Ecode of the exceptions this kernel names.
constexpr uint32_t kEcodeInterrupt = 0, kEcodeBreak = 12;

// IOCSR registers of the inter-processor interrupt unit (one per core) and of the mailbox that starts a core.
constexpr uint32_t kIocsrIpiStatus = 0x1000, kIocsrIpiEnable = 0x1004, kIocsrIpiClear = 0x100C;
constexpr uint32_t kIocsrMailbox0 = 0x1020, kIocsrMailbox1 = 0x1028, kIocsrIpiSend = 0x1040, kIocsrMailSend = 0x1048;

template <uint32_t Csr>
inline uint64_t csr_read() {
  return __builtin_loongarch_csrrd_d(Csr);
}
template <uint32_t Csr>
inline void csr_write(uint64_t value) {
  (void)__builtin_loongarch_csrwr_d(value, Csr);   // (the old value is not wanted; the write is not dropped)
}
// Replaces the bits of `mask` with those of `value` and returns the old register.
template <uint32_t Csr>
inline uint64_t csr_exchange(uint64_t value, uint64_t mask) {
  return __builtin_loongarch_csrxchg_d(value, mask, Csr);
}

inline uint32_t iocsr_read32(uint32_t reg) { return __builtin_loongarch_iocsrrd_w(reg); }
inline void iocsr_write32(uint32_t reg, uint32_t value) { __builtin_loongarch_iocsrwr_w(value, reg); }
inline uint64_t iocsr_read64(uint32_t reg) { return __builtin_loongarch_iocsrrd_d(reg); }
inline void iocsr_write64(uint32_t reg, uint64_t value) { __builtin_loongarch_iocsrwr_d(value, reg); }

// The id of the core this code runs on: CPUID.CoreID, which is also the address the IPI unit and the ACPI MADT use.
inline uint32_t this_core_id() { return static_cast<uint32_t>(csr_read<kCsrCpuid>() & 0x1FF); }

// The stable counter (constant rate, shared by all cores). Counter id 0 comes from the zero register.
inline uint64_t read_counter() {
  uint64_t v;
  __asm__ volatile("rdtime.d %0, $zero" : "=r"(v)::"memory");
  return v;
}

// Sends inter-processor interrupt vector `vector` (0..31) to the core with id `core`; returns once it is delivered.
inline void send_ipi(uint32_t core, uint32_t vector) {
  iocsr_write32(kIocsrIpiSend, (1u << 31) | (core << 16) | (vector & 31));
}
// Enables every IPI vector on this core (the interrupt itself is ECFG.LIE bit 12).
inline void enable_ipi() { iocsr_write32(kIocsrIpiEnable, 0xFFFFFFFFu); }
// Acknowledges all pending IPI vectors of this core and returns them.
inline uint32_t ack_ipi() {
  const uint32_t pending = iocsr_read32(kIocsrIpiStatus);
  if (pending) iocsr_write32(kIocsrIpiClear, pending);
  return pending;
}
// Writes one 64-bit word into mailbox `box` (0..3) of another core, which is how a parked core is told where to go.
inline void mail_send(uint32_t core, uint32_t box, uint64_t data) {
  // One 32-bit half per write: box*2 is the low half, box*2+1 the high half; bit 31 waits for the write to land.
  const uint64_t base = (uint64_t{1} << 31) | (uint64_t{core} << 16);
  iocsr_write64(kIocsrMailSend, base | (uint64_t{box * 2 + 1} << 2) | (data & 0xFFFFFFFF00000000ull));
  iocsr_write64(kIocsrMailSend, base | (uint64_t{box * 2} << 2) | (data << 32));
}

}  // namespace arch_loongarch64
