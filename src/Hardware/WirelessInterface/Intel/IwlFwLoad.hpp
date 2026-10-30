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


// Loading the firmware into an Intel Wireless-AC 9000 series card, the way iwlwifi does for the 8000 and 9000 families (iwl_pcie_load_given_ucode_8000; these families do not use the
// "context information" boot of the 22000 series): the CPU reset is released, then every section of the first processor (LMAC) and of the second (UMAC) is copied through a DMA
// bounce buffer into the chip's memory by the flow handler's service channel, chunk by chunk, and the loaded sections are announced to the ROM in FH_UCODE_LOAD_STATUS (one bit
// per section, the first processor in the low half, the second in the high half). The processors then run the image and raise ALIVE. Paging sections (after the second separator)
// are not loaded here: the firmware asks for them later. Polled, no interrupts.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"

namespace hardware {

namespace iwl_fh {
constexpr uint32_t kServiceChannel = 9;
constexpr uint32_t kMemLowerBound = 0x1000;
constexpr uint32_t kTfdibLowerBound = kMemLowerBound + 0x900;
constexpr uint32_t kTcsrLowerBound = kMemLowerBound + 0xD00;
constexpr uint32_t kServiceLowerBound = kMemLowerBound + 0x9C8;
constexpr uint32_t tfdib_ctrl0(uint32_t channel) { return kTfdibLowerBound + 8 * channel; }
constexpr uint32_t tfdib_ctrl1(uint32_t channel) { return kTfdibLowerBound + 8 * channel + 4; }
constexpr uint32_t tx_config(uint32_t channel) { return kTcsrLowerBound + 0x20 * channel; }
constexpr uint32_t tx_buffer_status(uint32_t channel) { return kTcsrLowerBound + 0x20 * channel + 8; }
constexpr uint32_t service_sram_address(uint32_t channel) { return kServiceLowerBound + (channel - 9) * 4; }
constexpr uint32_t kUcodeLoadStatus = 0x1AF0;
constexpr uint32_t kTfdibAddressShift = 28;                     // the address bits above 32 sit in CTRL1 above the byte count
constexpr uint32_t kMaxTransferLength = 0x20000;
// FH_TCSR_CHNL_TX_CONFIG_REG
constexpr uint32_t kTxChannelPause = 0, kTxChannelEnable = 0x80000000u, kTxCreditDisable = 0, kTxInterruptHostEndTfd = 0x00100000;
// FH_TCSR_CHNL_TX_BUF_STS_REG
constexpr uint32_t kBufferStatusTbNumPos = 20, kBufferStatusTbIdxPos = 12, kBufferStatusValid = 3;
}  // namespace iwl_fh

namespace iwl_prph {
constexpr uint32_t kReleaseCpuReset = 0x300C, kReleaseCpuResetBit = 1u << 24;
constexpr uint32_t kWfpmGp2 = 0xA030B4, kLmpmChick = 0xA01FF8, kLmpmChickExtendedAddressSpace = 1;
constexpr uint32_t kApmgPsCtrl = 0x0C + 0x3000;                 // APMG_BASE (0x3000) + 0x0c
constexpr uint32_t kApmgPsCtrlMaskPowerSource = 0x03000000, kApmgPsCtrlPowerSourceVmain = 0;
}  // namespace iwl_prph

namespace iwl_csr {
constexpr uint32_t kIntBitAlive = 1u << 0, kIntBitWakeup = 1u << 1, kIntBitRfKill = 1u << 7, kIntBitSwError = 1u << 25, kIntBitFhTx = 1u << 27, kIntBitHwError = 1u << 29,
                   kIntBitFhRx = 1u << 31;
constexpr uint32_t kFhIntTxMask = 3;                           // transmit channels 0 and 1 (the service channel reports here)
constexpr uint32_t kUcodeDrvGp1Clr = 0x05C, kUcodeSwBitRfKill = 2, kUcodeBitCmdBlocked = 4;
}  // namespace iwl_csr

// The firmware's memory range that needs the extended address space bit while loading.
constexpr uint32_t kIwlFwMemExtendedStart = 0x40000, kIwlFwMemExtendedEnd = 0x57FFF;

class IwlFwLoad {
 public:
  IwlFwLoad(IwlPcieDevice& device, allocators::IAllocator& allocator) : device_(device), allocator_(allocator) {}

  // Releases the processors' reset and loads both processors' sections of `image`. False with a static message in *error on a section that is not whole dwords, a missing image,
  // a transfer that does not complete or memory that cannot be had.
  bool load(const IwlFirmware& firmware, IwlImage image, const char** error);
  // Waits for the ALIVE interrupt bit (acknowledging it); false on timeout or when the firmware reports an error instead (SW_ERR / HW_ERR in INT).
  bool wait_alive(uint32_t timeout_us, const char** error);

  uint32_t sections_loaded() const { return sections_loaded_; }
  uint32_t chunks_loaded() const { return chunks_loaded_; }

 private:
  bool load_cpu(const IwlSection* sections, uint32_t count, uint32_t cpu, const char** error);
  bool load_section(const IwlSection& section, const char** error);
  bool load_chunk(uint32_t device_address, uint64_t bus_address, uint32_t bytes, const char** error);

  IwlPcieDevice& device_;
  allocators::IAllocator& allocator_;
  uint32_t sections_loaded_ = 0, chunks_loaded_ = 0;
};

}  // namespace hardware
