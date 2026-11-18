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

// The PCIe register level of the MediaTek MT7921 / MT7922 / MT7920 (AMD RZ608 / RZ616), after mt7921/pci.c, mt792x_core.c, mt792x_dma.c and mt792x_regs.h of Linux mt76.
//
// The chip's registers live in a 32 bit chip address space (0x7c060010 for the own register, 0x70010200 for the chip id, ...), of which BAR0 shows a part:
// - addresses below 0x100000 are BAR offsets as they are;
// - a fixed table (fixed_map of __mt7921_reg_addr) places the busy blocks (WFDMA, CONN_INFRA, the LMAC/UMAC tops, the MCU SYSRAM) at fixed BAR offsets;
// - anything else goes through one of two movable windows: L1 (for 0x18000000-0x18bfffff, 0x70000000-0x77ffffff and 0x7c000000-0x7c3fffff, 64 KiB pages, the page
//   written to MT_HIF_REMAP_L1, seen at BAR 0x130000) and L2 (all other addresses, 4 KiB pages, the page written to MT_HIF_REMAP_L2, seen at MT_HIF_REMAP_BASE_L2).
// Mt7921Registers does that translation on top of the BAR (an INicRegisters; a host test hands in a model).
//
// Then the steps of bring-up: the own handshake (MT_CONN_ON_LPCTL: the host sets HOST_SET_OWN to give the chip to the firmware, HOST_CLR_OWN to take it, and waits on
// OWN_SYNC), the WFSYS reset, and the WFDMA0 host DMA engine: its global configuration, prefetch, and the rings - TX ring 16 for the firmware download (FW_SCATTER),
// TX ring 17 for commands to the WM MCU, RX ring 0 for the MCU's events. A ring is a table of 16-byte descriptors (struct mt76_desc: buf0, ctrl with the length and
// DMA_DONE, buf1, info) in memory below 4 GiB, and four registers (base, size, the CPU index the host advances, the DMA index the chip advances).
//
// Unverified: the register offsets were transcribed from the Linux sources from memory and have not been read off a real MT7921; especially MT_HIF_REMAP_L2 and
// MT_HIF_REMAP_BASE_L2 and the edges of the fixed map. Linux's fixed-map test is "ofs > size" (one byte past a block still maps); this uses "ofs >= size".
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace mt7921 {
// identification
constexpr uint32_t kHwChipId = 0x70010200, kHwRev = 0x70010204;
// the own handshake
constexpr uint32_t kConnOnLpctl = 0x7C060010, kLpcrHostSetOwn = 1u << 0, kLpcrHostClrOwn = 1u << 1, kLpcrHostOwnSync = 1u << 2;
constexpr uint32_t kDrvOwnRetries = 10;
// firmware running
constexpr uint32_t kConnOnMisc = 0x7C0600F0, kTopMisc2FwN9Ready = 3u << 0;
// WFSYS reset (mt792x_wfsys_reset: 0x18000140 on MT7921/MT7922/MT7920, 0x7c000140 on MT7925)
constexpr uint32_t kWfsysSwReset = 0x18000140, kWfsysSwResetMt7925 = 0x7C000140, kWfsysSwRstB = 1u << 0, kWfsysSwInitDone = 1u << 4;
// remap windows
constexpr uint32_t kHifRemapL1 = 0x155024, kHifRemapL1Mask = 0xFFFF0000u, kHifRemapBaseL1 = 0x130000;
constexpr uint32_t kHifRemapL2 = 0x0120, kHifRemapL2Mask = 0x000FFFFFu, kHifRemapBaseL2 = 0x18500;
// WFDMA0 (host side DMA)
constexpr uint32_t kWfdma0Base = 0xD4000;
constexpr uint32_t wfdma0(uint32_t offset) { return kWfdma0Base + offset; }
constexpr uint32_t kWfdma0Rst = wfdma0(0x100), kWfdma0RstLogic = 1u << 4, kWfdma0RstDmashdlAll = 1u << 5;
constexpr uint32_t kWfdma0GloCfg = wfdma0(0x208);
constexpr uint32_t kGloTxDmaEn = 1u << 0, kGloTxDmaBusy = 1u << 1, kGloRxDmaEn = 1u << 2, kGloRxDmaBusy = 1u << 3, kGloTxWbDdone = 1u << 6,
                   kGloFifoLittleEndian = 1u << 12, kGloCsrDispBasePtrChainEn = 1u << 15, kGloOmitRxInfoPfet2 = 1u << 21, kGloOmitRxInfo = 1u << 27,
                   kGloOmitTxInfo = 1u << 28, kGloClkGatDis = 1u << 30;
constexpr uint32_t kWfdma0RstDtxPtr = wfdma0(0x20C), kWfdma0RstDrxPtr = wfdma0(0x280);
constexpr uint32_t kWfdma0GloCfgExt0 = wfdma0(0x2B0), kWfdma0CsrTxDmashdlEnable = 1u << 6;
constexpr uint32_t kWfdma0PriDlyIntCfg0 = wfdma0(0x2F0);
constexpr uint32_t kWfdma0HostIntEna = wfdma0(0x204);
constexpr uint32_t kTxRingBase = wfdma0(0x300), kRxEventRingBase = wfdma0(0x500), kRingStride = 0x10;
constexpr uint32_t tx_ring_ext_ctrl(uint32_t ring) { return wfdma0(0x600 + 4 * ring); }
constexpr uint32_t rx_ring_ext_ctrl(uint32_t ring) { return wfdma0(0x680 + 4 * ring); }
constexpr uint32_t prefetch(uint32_t base, uint32_t depth) { return base << 16 | depth; }
// DMASHDL and the dummy register the firmware reads to know the host re-initialised WFDMA (chip addresses: they go through the fixed map)
constexpr uint32_t kDmashdlSwControl = 0x7C026004, kDmashdlBypass = 1u << 28;
constexpr uint32_t kWfdmaDummyCr = 0x54000120, kWfdmaNeedReinit = 1u << 1;
// rings
constexpr uint32_t kTxRingFwdl = 16, kTxRingMcuWm = 17, kRxRingMcuWm = 0;
constexpr uint32_t kTxFwdlRingSize = 128, kTxMcuRingSize = 256, kRxMcuRingSize = 8, kRxBufferBytes = 2048;
// struct mt76_desc ctrl
constexpr uint32_t kDmaCtlSdLen1Mask = 0x3FFF, kDmaCtlLastSec1 = 1u << 14, kDmaCtlBurst = 1u << 15, kDmaCtlSdLen0Shift = 16, kDmaCtlSdLen0Mask = 0x3FFFu << 16,
                   kDmaCtlLastSec0 = 1u << 30, kDmaCtlDmaDone = 1u << 31;
}  // namespace mt7921

// The fixed part of the translation: the BAR offset of chip address `address` if it is below 0x100000 or in the fixed map. False if it needs a window.
[[nodiscard]] bool mt7921_fixed_map(uint32_t address, uint32_t* bar_offset);
// Which window an address that is not fixed goes through.
enum class Mt7921Window : uint8_t { Fixed, L1, L2 };
Mt7921Window mt7921_window(uint32_t address);

// The chip's address space over BAR0.
class Mt7921Registers {
 public:
  explicit Mt7921Registers(INicRegisters& bar) : bar_(bar) {}

  uint32_t read(uint32_t address);
  void write(uint32_t address, uint32_t value);
  void set(uint32_t address, uint32_t bits) { write(address, read(address) | bits); }
  void clear(uint32_t address, uint32_t bits) { write(address, read(address) & ~bits); }
  // Polls until (read(address) & mask) == value, `tries` times `step_us` apart. True when it got there.
  [[nodiscard]] bool poll(uint32_t address, uint32_t mask, uint32_t value, uint32_t tries, uint32_t step_us);
  INicRegisters& bar() { return bar_; }

  // The BAR offset where `address` can be reached now, moving a window when it has to (as __mt7921_reg_addr does).
  uint32_t map(uint32_t address);

 private:
  INicRegisters& bar_;
};

// --- bring-up steps

enum class Mt7921Status : uint8_t { Ok, NoResponse, Timeout };

// __mt792x_mcu_fw_pmctrl: hands the chip to the firmware (HOST_SET_OWN, wait for OWN_SYNC).
[[nodiscard]] Mt7921Status mt7921_firmware_own(Mt7921Registers& regs);
// __mt792xe_mcu_drv_pmctrl: takes it back for the driver (HOST_CLR_OWN until OWN_SYNC clears, up to ten times).
[[nodiscard]] Mt7921Status mt7921_driver_own(Mt7921Registers& regs);
// mt792x_wfsys_reset.
[[nodiscard]] Mt7921Status mt7921_wfsys_reset(Mt7921Registers& regs, uint32_t reset_register);
// mt792x_dma_disable (force: also pulse the logic and DMASHDL resets).
[[nodiscard]] Mt7921Status mt7921_dma_disable(Mt7921Registers& regs, bool force);
// mt792x_dma_prefetch (MT7921 values) and the enabling half of mt792x_dma_enable (interrupts stay off: the driver polls).
void mt7921_dma_enable(Mt7921Registers& regs);

// --- rings

// A WFDMA0 ring: descriptors and one buffer per descriptor in DMA memory, and the ring's registers. Owns its memory.
class Mt7921Ring {
 public:
  Mt7921Ring() = default;
  ~Mt7921Ring() { release(); }
  Mt7921Ring(const Mt7921Ring&) = delete;
  Mt7921Ring& operator=(const Mt7921Ring&) = delete;

  // Allocates `count` descriptors with `buffer_bytes` each, and writes the ring's registers at `registers` (base + 0x10 * index). An RX ring has every buffer given to
  // the chip. False if memory ran out or lies above 4 GiB.
  [[nodiscard]] bool create(allocators::IAllocator& allocator, Mt7921Registers& regs, uint32_t registers, uint32_t count, uint32_t buffer_bytes, bool receive);
  void release();
  bool valid() const { return !descriptors_.empty(); }

  // TX: copies `data` into the next buffer, posts its descriptor and waits until the chip took it (DMA index caught up). False if the data does not fit or the chip
  // did not take it in time.
  [[nodiscard]] bool send(Mt7921Registers& regs, ConstByteView data, uint32_t tries, uint32_t step_us);
  // RX: if the chip finished the next descriptor, its bytes (valid until release_received()); empty otherwise.
  ConstByteView peek_received();
  // Gives the descriptor peek_received() returned back to the chip.
  void release_received(Mt7921Registers& regs);

  uint32_t registers() const { return registers_; }
  uint32_t count() const { return count_; }
  uint64_t descriptor_bus_address() const;

 private:
  uint32_t word(uint32_t descriptor, uint32_t index) const;
  void set_word(uint32_t descriptor, uint32_t index, uint32_t value);
  void arm_receive(uint32_t descriptor);

  allocators::IAllocator* allocator_ = nullptr;
  ArrayView<uint32_t> descriptors_;     // 4 words per descriptor
  ByteView buffers_;
  uint32_t registers_ = 0, count_ = 0, buffer_bytes_ = 0, head_ = 0, tail_ = 0;
  bool receive_ = false;
};

}  // namespace hardware
