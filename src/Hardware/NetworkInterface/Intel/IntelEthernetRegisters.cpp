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

#include "Hardware/NetworkInterface/Intel/IntelEthernetRegisters.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {
namespace intel_eth {

bool wait_for(INicRegisters& regs, uint32_t reg, uint32_t mask, uint32_t expected, uint32_t attempts, uint32_t step_us) {
  for (uint32_t i = 0; i < attempts; ++i) {
    if ((regs.read32(reg) & mask) == expected) return true;
    delay_us(step_us);
  }
  return (regs.read32(reg) & mask) == expected;
}

void stop_dma(INicRegisters& regs) {
  regs.write32(kRctl, 0);
  regs.write32(kTctl, kTctlPadShortPackets);
  delay_us(10000);
  // GIO master disable: the device finishes its outstanding requests and starts no new ones (e1000e_disable_pcie_master), at most 80 ms.
  regs.set_bits32(kCtrl, kCtrlGioMasterDisable);
  wait_for(regs, kStatus, kStatusGioMasterEnable, 0, 800, 100);
}

bool read_mac(INicRegisters& regs, MacAddress* mac) {
  const uint32_t low = regs.read32(kRal0), high = regs.read32(kRah0);
  if (!(high & kRahAddressValid)) return false;
  for (int i = 0; i < 4; ++i) mac->bytes[i] = static_cast<uint8_t>(low >> (8 * i));
  mac->bytes[4] = static_cast<uint8_t>(high);
  mac->bytes[5] = static_cast<uint8_t>(high >> 8);
  bool zero = true;
  for (uint8_t b : mac->bytes) zero = zero && b == 0;
  return !zero && !(mac->bytes[0] & 1);
}

void write_mac(INicRegisters& regs, const MacAddress& mac) {
  const uint32_t low = mac.bytes[0] | (uint32_t{mac.bytes[1]} << 8) | (uint32_t{mac.bytes[2]} << 16) | (uint32_t{mac.bytes[3]} << 24);
  const uint32_t high = mac.bytes[4] | (uint32_t{mac.bytes[5]} << 8);
  regs.write32(kRal0, low);
  regs.write32(kRah0, high | kRahAddressValid);
}

namespace {
constexpr uint32_t kMdicReg = 16, kMdicPhy = 21, kMdicOpRead = 2u << 26, kMdicOpWrite = 1u << 26, kMdicReady = 1u << 28, kMdicError = 1u << 30;
constexpr int kMdicPolls = 640;  // 50 us apart: e1000e allows 64 * 3 * 50 us, igb 1920 * 50 us
}  // namespace

bool mdic_read(INicRegisters& regs, uint32_t phy, uint32_t reg, uint16_t* value) {
  regs.write32(kMdic, (reg << kMdicReg) | (phy << kMdicPhy) | kMdicOpRead);
  for (int i = 0; i < kMdicPolls; ++i) {
    delay_us(50);
    const uint32_t mdic = regs.read32(kMdic);
    if (mdic & kMdicReady) {
      if (mdic & kMdicError) return false;
      *value = static_cast<uint16_t>(mdic);
      return true;
    }
  }
  return false;
}

bool mdic_write(INicRegisters& regs, uint32_t phy, uint32_t reg, uint16_t value) {
  regs.write32(kMdic, value | (reg << kMdicReg) | (phy << kMdicPhy) | kMdicOpWrite);
  for (int i = 0; i < kMdicPolls; ++i) {
    delay_us(50);
    const uint32_t mdic = regs.read32(kMdic);
    if (mdic & kMdicReady) return !(mdic & kMdicError);
  }
  return false;
}

// STATUS bits 7:6: 00 is 10 Mb/s, 01 is 100 Mb/s, 10 and 11 are 1000 Mb/s; the I225 and I226 add bit 22 for 2500 Mb/s.
uint32_t status_speed_mbps(uint32_t status, bool has_2500) {
  if (!(status & kStatusLinkUp)) return 0;
  const uint32_t speed = (status >> kStatusSpeedShift) & kStatusSpeedMask;
  if (speed >= 2) return has_2500 && (status & kStatusSpeed2500) ? 2500 : 1000;
  return speed == 1 ? 100 : 10;
}

}  // namespace intel_eth
}  // namespace hardware
