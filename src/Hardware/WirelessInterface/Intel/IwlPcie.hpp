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


// The register level of an Intel Wireless-AC 9000 series card on PCI Express, after iwlwifi's pcie/trans.c and iwl-csr.h: taking ownership of the hardware ("prepare card"), waking
// the MAC (INIT_DONE, the clock ready), the access to the device's internal peripheral registers and memory through the HBUS window (which needs the MAC awake), the basic
// functions of the adapter (APM) and stopping them. Loading firmware is IwlFwLoad, the DMA queues come after. The registers are the 4 KiB window of BAR 0; a test supplies a model.
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"

namespace hardware {

class IIwlRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

 protected:
  constexpr IIwlRegisters() = default;
  ~IIwlRegisters() = default;
};

class MmioIwlRegisters final : public IIwlRegisters {
 public:
  explicit MmioIwlRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

namespace iwl_csr {
constexpr uint32_t kHwIfConfig = 0x000, kInt = 0x008, kIntMask = 0x00C, kFhIntStatus = 0x010, kReset = 0x020, kGpCntrl = 0x024, kHwRev = 0x028, kMboxSet = 0x088, kHwRfId = 0x09C,
                   kGioChickenBits = 0x100, kDbgHpetMem = 0x240, kDbgLinkPwrMgmt = 0x250;
constexpr uint32_t kHbusMemRaddr = 0x40C, kHbusMemWaddr = 0x410, kHbusMemWdat = 0x418, kHbusMemRdat = 0x41C, kHbusPrphWaddr = 0x444, kHbusPrphRaddr = 0x448, kHbusPrphWdat = 0x44C,
                   kHbusPrphRdat = 0x450;
// CSR_HW_IF_CONFIG_REG
constexpr uint32_t kHwIfHapWake = 0x00080000, kHwIfPciOwnSet = 0x00400000, kHwIfWakeMe = 0x08000000;
constexpr uint32_t kMboxOsAlive = 1u << 5;
// CSR_RESET
constexpr uint32_t kResetSwReset = 0x80, kResetMasterDisabled = 0x100, kResetStopMaster = 0x200, kResetLinkPwrMgmtDisabled = 0x80000000u;
// CSR_GP_CNTRL
constexpr uint32_t kGpMacClockReady = 0x1, kGpInitDone = 0x4, kGpMacAccessReq = 0x8, kGpGoingToSleep = 0x10, kGpHwRfKillSw = 0x08000000;
constexpr uint32_t kGioChickenL1aNoL0sRx = 0x00800000, kDbgHpetMemVal = 0xFFFF0000u;
constexpr uint32_t kPrphMask = 0x000FFFFF;                 // the 9000 family's peripheral address space (AX210 and later have 24 bits)
}  // namespace iwl_csr

class IwlPcieDevice {
 public:
  explicit IwlPcieDevice(IIwlRegisters& regs) : regs_(regs) {}

  uint32_t hw_rev() { return regs_.read32(iwl_csr::kHwRev); }
  uint32_t hw_rf_id() { return regs_.read32(iwl_csr::kHwRfId); }
  // The registers answer (a card that fell off the bus reads all ones).
  bool present() { return hw_rev() != 0xFFFFFFFFu; }

  // Takes the hardware from the other owner (the management engine, the firmware of the previous boot): PCI_OWN_SET until it sticks, then WAKE_ME and retrying; sets OS_ALIVE.
  bool prepare_card_hw();
  // INIT_DONE and the wait for the MAC clock: the adapter goes from D0U to D0A, internal registers can then be used.
  bool activate_nic();
  // The adapter's basic functions after a reset (iwl_pcie_apm_init for a family without the APMG block): the chicken bits, the DBG HPET value, HAP wake, then activate_nic().
  bool apm_init();
  // Stops the bus master, resets the device, clears INIT_DONE (iwl_pcie_apm_stop).
  void apm_stop();
  void disable_interrupts();
  // The hardware switch of the radio: true when the radio is killed.
  bool hw_rfkill();

  // The MAC must be awake for the peripheral and memory accesses: grab, access, release.
  bool grab_nic_access();
  void release_nic_access();
  uint32_t read_prph(uint32_t address);
  void write_prph(uint32_t address, uint32_t value);
  void set_prph_bits(uint32_t address, uint32_t bits) { write_prph(address, read_prph(address) | bits); }
  void clear_prph_bits(uint32_t address, uint32_t bits) { write_prph(address, read_prph(address) & ~bits); }
  // The device's memory: consecutive dwords (the address register auto-increments).
  void read_mem(uint32_t address, uint32_t* out, uint32_t dwords);
  void write_mem(uint32_t address, const uint32_t* data, uint32_t dwords);

  uint32_t read32(uint32_t offset) { return regs_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) { regs_.write32(offset, value); }
  void set_bit(uint32_t offset, uint32_t bits) { regs_.write32(offset, regs_.read32(offset) | bits); }
  void clear_bit(uint32_t offset, uint32_t bits) { regs_.write32(offset, regs_.read32(offset) & ~bits); }
  // Reads until (value & mask) == (bits & mask), at most `timeout_us` microseconds (10 us apart).
  bool poll(uint32_t offset, uint32_t bits, uint32_t mask, uint32_t timeout_us);

 private:
  bool set_hw_ready();
  IIwlRegisters& regs_;
};

}  // namespace hardware
