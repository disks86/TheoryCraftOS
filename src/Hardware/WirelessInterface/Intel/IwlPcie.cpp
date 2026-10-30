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


#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace iwl_csr;

namespace {
constexpr uint32_t kPollInterval = 10;
constexpr uint32_t kHwReadyTimeout = 50;
}  // namespace

bool IwlPcieDevice::poll(uint32_t offset, uint32_t bits, uint32_t mask, uint32_t timeout_us) {
  uint32_t t = 0;
  do {
    if ((regs_.read32(offset) & mask) == (bits & mask)) return true;
    delay_us(kPollInterval);
    t += kPollInterval;
  } while (t < timeout_us);
  return false;
}

bool IwlPcieDevice::set_hw_ready() {
  set_bit(kHwIfConfig, kHwIfPciOwnSet);
  const bool ready = poll(kHwIfConfig, kHwIfPciOwnSet, kHwIfPciOwnSet, kHwReadyTimeout);
  if (ready) set_bit(kMboxSet, kMboxOsAlive);
  return ready;
}

bool IwlPcieDevice::prepare_card_hw() {
  if (set_hw_ready()) return true;
  set_bit(kDbgLinkPwrMgmt, kResetLinkPwrMgmtDisabled);
  delay_us(1500);
  for (uint32_t iteration = 0; iteration < 10; ++iteration) {
    set_bit(kHwIfConfig, kHwIfWakeMe);
    uint32_t t = 0;
    do {
      if (set_hw_ready()) return true;
      delay_us(500);
      t += 200;
    } while (t < 150000);
    delay_us(25000);
  }
  return false;
}

bool IwlPcieDevice::activate_nic() {
  // "initialization complete": from D0U to D0A, then the clock must become ready (once it is, the internal registers and the SRAM can be reached)
  set_bit(kGpCntrl, kGpInitDone);
  return poll(kGpCntrl, kGpMacClockReady, kGpMacClockReady, 25000);
}

bool IwlPcieDevice::apm_init() {
  // (set_bit, not write: the bits the hardware set at reset stay)
  set_bit(kGioChickenBits, kGioChickenL1aNoL0sRx);          // L0s off without L1; do not wait for the ICH's L0s
  set_bit(kDbgHpetMem, kDbgHpetMemVal);                     // the FH wait threshold at its maximum
  set_bit(kHwIfConfig, kHwIfHapWake);                       // the management bus interrupt wakes the PCIe link from L1a
  return activate_nic();                                    // (the 9000 family has no APMG block to set up)
}

void IwlPcieDevice::apm_stop() {
  // stop the bus master, wait for it, reset, and take the adapter from D0A back to D0U
  set_bit(kReset, kResetStopMaster);
  poll(kReset, kResetMasterDisabled, kResetMasterDisabled, 100);
  set_bit(kReset, kResetSwReset);
  delay_us(5500);
  clear_bit(kGpCntrl, kGpInitDone);
}

void IwlPcieDevice::disable_interrupts() {
  regs_.write32(kIntMask, 0);
  regs_.write32(kInt, 0xFFFFFFFFu);
  regs_.write32(kFhIntStatus, 0xFFFFFFFFu);
}

bool IwlPcieDevice::hw_rfkill() { return (regs_.read32(kGpCntrl) & kGpHwRfKillSw) == 0; }

bool IwlPcieDevice::grab_nic_access() {
  // the bit that wakes the NIC, then the clock must be ready and the NIC must not be going to sleep
  set_bit(kGpCntrl, kGpMacAccessReq);
  delay_us(2);
  return poll(kGpCntrl, kGpMacClockReady, kGpMacClockReady | kGpGoingToSleep, 15000);
}

void IwlPcieDevice::release_nic_access() { clear_bit(kGpCntrl, kGpMacAccessReq); }

uint32_t IwlPcieDevice::read_prph(uint32_t address) {
  regs_.write32(kHbusPrphRaddr, (address & kPrphMask) | (3u << 24));
  return regs_.read32(kHbusPrphRdat);
}

void IwlPcieDevice::write_prph(uint32_t address, uint32_t value) {
  regs_.write32(kHbusPrphWaddr, (address & kPrphMask) | (3u << 24));
  regs_.write32(kHbusPrphWdat, value);
}

void IwlPcieDevice::read_mem(uint32_t address, uint32_t* out, uint32_t dwords) {
  regs_.write32(kHbusMemRaddr, address);
  for (uint32_t i = 0; i < dwords; ++i) out[i] = regs_.read32(kHbusMemRdat);
}

void IwlPcieDevice::write_mem(uint32_t address, const uint32_t* data, uint32_t dwords) {
  regs_.write32(kHbusMemWaddr, address);
  for (uint32_t i = 0; i < dwords; ++i) regs_.write32(kHbusMemWdat, data[i]);
}

}  // namespace hardware
