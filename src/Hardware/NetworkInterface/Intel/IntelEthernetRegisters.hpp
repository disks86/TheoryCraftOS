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

// The registers the Intel gigabit families share (8254x, e1000e, igb, igc), with the names of Intel's datasheets and Linux's e1000_regs.h / e1000_defines.h,
// and the steps every one of their drivers takes the same way: reading and keeping the MAC address, stopping DMA before a reset, and link status.
#pragma once
#include <stdint.h>
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"

namespace hardware {
namespace intel_eth {

constexpr uint32_t kCtrl = 0x0000, kStatus = 0x0008, kEecd = 0x0010, kCtrlExt = 0x0018, kMdic = 0x0020, kFextnvm3 = 0x003C;
constexpr uint32_t kIcr = 0x00C0, kIms = 0x00D0, kImc = 0x00D8, kRctl = 0x0100, kTctl = 0x0400, kTipg = 0x0410;
constexpr uint32_t kExtcnfCtrl = 0x0F00, kIosfpc = 0x0F28, kEimc = 0x1528;
constexpr uint32_t kMta = 0x5200, kRal0 = 0x5400, kRah0 = 0x5404, kFwsm = 0x5B54;

// CTRL
constexpr uint32_t kCtrlFullDuplex = 1u << 0, kCtrlGioMasterDisable = 1u << 2, kCtrlLinkReset = 1u << 3, kCtrlAutoSpeed = 1u << 5, kCtrlSetLinkUp = 1u << 6;
constexpr uint32_t kCtrlForceSpeed = 1u << 11, kCtrlForceDuplex = 1u << 12, kCtrlLanPhyPcOverride = 1u << 16, kCtrlLanPhyPcValue = 1u << 17;
constexpr uint32_t kCtrlReset = 1u << 26, kCtrlDeviceReset = 1u << 29, kCtrlPhyReset = 1u << 31;
// STATUS
constexpr uint32_t kStatusFullDuplex = 1u << 0, kStatusLinkUp = 1u << 1, kStatusSpeedShift = 6, kStatusSpeedMask = 3, kStatusLanInitDone = 1u << 9;
constexpr uint32_t kStatusGioMasterEnable = 1u << 19, kStatusSpeed2500 = 1u << 22;
// EECD, CTRL_EXT
constexpr uint32_t kEecdAutoReadDone = 1u << 9;
constexpr uint32_t kCtrlExtLanPhyPcDone = 1u << 2, kCtrlExtForceSmbus = 1u << 11, kCtrlExtDriverLoaded = 1u << 28;
// RCTL, TCTL
constexpr uint32_t kRctlEnable = 1u << 1, kRctlMulticastPromiscuous = 1u << 4, kRctlBroadcast = 1u << 15, kRctlStripCrc = 1u << 26;
constexpr uint32_t kTctlEnable = 1u << 1, kTctlPadShortPackets = 1u << 3, kTctlCollisionThreshold = 0x0Fu << 4, kTctlCollisionDistance = 0x3Fu << 12;
constexpr uint32_t kTctlRetransmitLateCollision = 1u << 24;
constexpr uint32_t kTipgDefault = 0x0060200A;  // IPGT 10, IPGR1 8, IPGR2 6 (IEEE 802.3)
constexpr uint32_t kRahAddressValid = 1u << 31;

// Turns DMA off and waits until the device has finished the transfers it had started (the step before every reset).
void stop_dma(INicRegisters& regs);
// The station address in receive address register 0 (where the hardware loads it from its NVM after a reset). False if it is not marked valid or is not
// a unicast address.
bool read_mac(INicRegisters& regs, MacAddress* mac);
void write_mac(INicRegisters& regs, const MacAddress& mac);
// From STATUS: 10, 100 or 1000 (2500 when `has_2500` and the I225/I226 bit says so); 0 without link.
uint32_t status_speed_mbps(uint32_t status, bool has_2500);
inline LinkDuplex status_duplex(uint32_t status) {
  if (!(status & kStatusLinkUp)) return LinkDuplex::Unknown;
  return (status & kStatusFullDuplex) ? LinkDuplex::Full : LinkDuplex::Half;
}
// The PHY's registers through the MAC's MDIO interface (MDIC). The caller holds whatever arbitration the part needs. False on a timeout or when
// the PHY does not answer.
bool mdic_read(INicRegisters& regs, uint32_t phy, uint32_t reg, uint16_t* value);
bool mdic_write(INicRegisters& regs, uint32_t phy, uint32_t reg, uint16_t value);
constexpr uint32_t kPhyControl = 0, kPhyStatusReg = 1, kPhyId1 = 2;
constexpr uint16_t kPhyAutoNegEnable = 1u << 12, kPhyRestartAutoNeg = 1u << 9, kPhyPowerDown = 1u << 11, kPhyIsolate = 1u << 10;

// Polls `reg` until (value & mask) == expected, at most `attempts` times `step_us` microseconds apart.
bool wait_for(INicRegisters& regs, uint32_t reg, uint32_t mask, uint32_t expected, uint32_t attempts, uint32_t step_us);

}  // namespace intel_eth
}  // namespace hardware
