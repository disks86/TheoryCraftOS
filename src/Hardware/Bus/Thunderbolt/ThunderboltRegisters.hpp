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

// The register block of a Thunderbolt host interface (NHI) in BAR 0, after the Linux thunderbolt driver's nhi_regs.h: the descriptor ring registers (one TX and one RX entry per hop),
// the ring options, the interrupt registers, the capability word, the firmware status and the mailbox to the connection manager firmware (ICM). A test supplies a model of the chip.
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"

namespace hardware {

class ITbRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

 protected:
  constexpr ITbRegisters() = default;
  ~ITbRegisters() = default;
};

class MmioTbRegisters final : public ITbRegisters {
 public:
  explicit MmioTbRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

namespace tb_reg {
// 16 bytes per hop: +0 descriptor array (64 bit bus address), +8 the ring's indices, +12 the number of descriptors (RX: the frame size in the upper half)
constexpr uint32_t kTxRingBase = 0x00000, kRxRingBase = 0x08000;
// 32 bytes per hop: +0 the flags, +4 (RX) the SOF / EOF masks
constexpr uint32_t kTxOptionsBase = 0x19800, kRxOptionsBase = 0x29800;
constexpr uint32_t kRingNotifyBase = 0x37800, kRingIntClear = 0x37808, kRingInterruptBase = 0x38200, kRingInterruptMaskClearBase = 0x38208;
constexpr uint32_t kCaps = 0x39640, kResetRegister = 0x39898, kInmailData = 0x39900, kInmailCmd = 0x39904, kOutmailCmd = 0x3990C, kFirmwareStatus = 0x39944;

constexpr uint32_t kCapsHopMask = 0x3FF, kCapsVersionShift = 16, kCapsVersionMask = 0xFF;
// ring options
constexpr uint32_t kRingFlagE2eFlowControl = 1u << 28, kRingFlagRaw = 1u << 30, kRingFlagEnable = 1u << 31;
constexpr uint32_t kRxOptionsE2eHopShift = 12, kRxOptionsE2eHopMask = 0x7FFu << 12;      // a receive ring with end to end flow control names the transmit hop its credits go out on
// REG_INMAIL_CMD / REG_OUTMAIL_CMD
constexpr uint32_t kInmailCmdMask = 0xFF, kInmailError = 1u << 30, kInmailRequest = 1u << 31, kOutmailOpModeShift = 8, kOutmailOpModeMask = 0xF00;
// REG_FW_STS
constexpr uint32_t kFwStsNvmAuthDone = 1u << 31, kFwStsCioResetRequest = 1u << 30, kFwStsIcmEnableCpu = 1u << 2, kFwStsIcmEnableInvert = 1u << 1, kFwStsIcmEnable = 1u << 0;
}  // namespace tb_reg

// Commands of the mailbox (nhi.h).
namespace tb_mailbox {
constexpr uint32_t kSaveDevices = 0x05, kDisconnectPciePaths = 0x06, kDriverUnloads = 0x07, kDisconnectPortA = 0x10, kDisconnectPortB = 0x11, kAllowAllDevices = 0x23;
}

}  // namespace hardware
