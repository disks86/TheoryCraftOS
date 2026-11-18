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

// The PCIe transport of the Broadcom FullMAC chips, after Linux brcmfmac pcie.c:
//
//   - BAR0 is a 4 KiB window onto the backplane; configuration register 0x80 (BRCMF_PCIE_BAR0_WINDOW) selects which 4 KiB (brcmf_pcie_buscore_prep_addr). Linux
//     maps the RAM separately through BAR2 ("tcm"), whose offsets are the ARM's addresses; here everything, the RAM included, goes through the BAR0 window, since
//     the RAM sits at the same addresses on the backplane (the SDIO transport reaches it only that way). Slower, but one BAR is all WifiChipBus hands over.
//   - The download (brcmf_pcie_download_fw_nvram): the ARM image at the start of RAM, its first word is the reset vector; the last RAM word is cleared; the NVRAM
//     image at the very end of RAM (its length token thus last); for chips that want one, a footer {length, 0xfeedc0de} and 256 random bytes just below the NVRAM.
//   - Once running, the firmware replaces the last RAM word with the address of its shared structure (brcmf_pcie_shared_info): flags with the protocol version
//     (5 to 7 supported), the console's address, the mailbox data addresses, the ring information address and the receive buffer limits.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfBackplane.hpp"

namespace hardware {

namespace brcmf_pcie {
constexpr uint16_t kBar0Window = 0x80;                 // BRCMF_PCIE_BAR0_WINDOW (configuration space)
constexpr uint32_t kBar0RegSize = 0x1000;              // BRCMF_PCIE_BAR0_REG_SIZE
// PCIe2 core registers (through the window at the core's base)
constexpr uint32_t kPcie2IntMask = 0x24, kPcie2MailboxInt = 0x48, kPcie2MailboxMask = 0x4c, kPcie2ConfigAddr = 0x120, kPcie2ConfigData = 0x124;
constexpr uint32_t kPcie2H2dMailbox0 = 0x140, kPcie2H2dMailbox1 = 0x144;
// The configuration registers brcmf_pcie_reset_device rewrites through CONFIGADDR/CONFIGDATA on PCIe2 cores up to revision 13 (the watchdog reset loses them).
constexpr uint16_t kResetConfigOffsets[] = {0x004, 0x04c, 0x058, 0x05c, 0x060, 0x064, 0x0bc, 0x0dc, 0x228, 0x248, 0x4e0, 0x4f4};
// The firmware's wait (BRCMF_PCIE_FW_UP_TIMEOUT), polled every 50 ms
constexpr uint32_t kFirmwareUpTimeoutMs = 5000;
// Random seed below the NVRAM (BRCMF_RANDOM_SEED_*)
constexpr uint32_t kRandomSeedMagic = 0xfeedc0de, kRandomSeedLength = 0x100, kRandomSeedFooterBytes = 8;
// brcmf_pcie_shared_info
constexpr uint32_t kSharedVersionMask = 0x00ff, kMinSharedVersion = 5, kMaxSharedVersion = 7;
constexpr uint32_t kSharedDmaIndex = 0x10000, kSharedDma2BIdx = 0x100000, kSharedHostReadyDb1 = 0x10000000;
constexpr uint32_t kSharedConsoleAddrOffset = 20, kSharedMaxRxBufPostOffset = 34, kSharedRxDataOffsetOffset = 36, kSharedHtoDMbDataOffset = 40,
                   kSharedDtoHMbDataOffset = 44, kSharedRingInfoAddrOffset = 48, kSharedBytes = 72;
constexpr uint16_t kDefaultMaxRxBufPost = 255;
// The console (struct rte_console's log part)
constexpr uint32_t kConsoleBufAddrOffset = 8, kConsoleBufSizeOffset = 12, kConsoleWriteIdxOffset = 16, kConsoleBytes = 20;
}  // namespace brcmf_pcie

class BrcmfPcieBackplane final : public IBrcmfBackplane {
 public:
  BrcmfPcieBackplane(INicRegisters& bar0, IPciBus& pci, PciAddress address) : bar0_(bar0), pci_(pci), address_(address) {}
  bool read32(uint32_t address, uint32_t* value) override;
  bool write32(uint32_t address, uint32_t value) override;
  bool write_block(uint32_t address, ConstByteView data) override;
  bool read_block(uint32_t address, ByteView data) override;
  // The window is written again on the next access (after a reset of the device).
  void forget_window() { window_valid_ = false; }
  uint32_t config_read32(uint16_t offset) { return pci_.read32(address_, offset); }
  void config_write32(uint16_t offset, uint32_t value) { pci_.write32(address_, offset, value); }

 private:
  uint32_t select(uint32_t address);
  INicRegisters& bar0_;
  IPciBus& pci_;
  PciAddress address_;
  uint32_t window_ = 0;
  bool window_valid_ = false;
};

// brcmf_pcie_reset_device after the chip is known: ASPM off, chipcommon's watchdog resets the chip (100 ms), ASPM back as it was, the configuration registers of older
// PCIe2 cores are written back, and pending mailbox interrupts are cleared (brcmf_pcie_buscore_reset).
[[nodiscard]] bool brcmf_pcie_reset_device(BrcmfPcieBackplane& bp, const BrcmfCoreList& cores);

// Where the download goes (all backplane addresses).
struct BrcmfPcieLayout {
  uint32_t firmware = 0;          // rambase
  uint32_t shared_word = 0;       // rambase + ramsize - 4
  uint32_t nvram = 0;             // rambase + ramsize - nvram length (0 length: no NVRAM)
  uint32_t seed_footer = 0;       // below the NVRAM, when a seed is wanted
  uint32_t seed = 0;
};
// False if the parts do not fit into RAM without overlapping (an image larger than the chip's RAM, an NVRAM that is not a multiple of 4 bytes, ...).
[[nodiscard]] bool brcmf_pcie_layout(const BrcmfRamInfo& ram, uint32_t firmware_bytes, uint32_t nvram_bytes, bool random_seed, BrcmfPcieLayout* out);
// The footer bytes (little endian length, magic).
void brcmf_pcie_seed_footer(uint8_t out[8]);

struct BrcmfPcieShared {
  uint32_t address = 0;
  uint32_t flags = 0;
  uint8_t version = 0;
  uint8_t dma_index_size = 0;         // 0: no DMA indices; 2 or 4 bytes
  uint16_t max_rxbufpost = 0;
  uint32_t rx_dataoffset = 0;
  uint32_t htod_mb_data_addr = 0;
  uint32_t dtoh_mb_data_addr = 0;
  uint32_t ring_info_addr = 0;
  uint32_t console_addr = 0;
};
enum class BrcmfSharedResult : uint8_t { Ok, Truncated, BadAddress, UnsupportedVersion };
// brcmf_pcie_init_share_ram_info over the structure's bytes read from `address`. The address must lie in RAM (and the structure with it); the protocol version in 5..7.
[[nodiscard]] BrcmfSharedResult brcmf_pcie_parse_shared(ConstByteView bytes, uint32_t address, const BrcmfRamInfo& ram, BrcmfPcieShared* out);

// The console's log ring as the firmware describes it at console_addr.
struct BrcmfConsole {
  uint32_t buffer = 0;
  uint32_t size = 0;
  uint32_t write_index = 0;
};
// False if the buffer is not inside RAM or its index past its size.
[[nodiscard]] bool brcmf_parse_console(ConstByteView bytes, const BrcmfRamInfo& ram, BrcmfConsole* out);

}  // namespace hardware
