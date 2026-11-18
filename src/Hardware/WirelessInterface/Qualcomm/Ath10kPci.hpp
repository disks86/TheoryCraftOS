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

// The register level of an ath10k chip on PCI Express, after ath10k/pci.c and ce.c:
//
//  - Ath10kPci: BAR 0 holds the target's registers at their target addresses (the RTC SoC block, the SoC core, the copy engines, FW_INDICATOR) and the PCIe local
//    block at pcie_local_base_address. Power-save chips (QCA6174, QCA9377) have to be woken first: PCIE_SOC_WAKE in the local block, then RTC_STATE until it says
//    "on" (ath10k_pci_wake, ath10k_pci_wake_wait). SOC_CHIP_ID identifies the chip. The cold reset pulses SOC_GLOBAL_RESET (target and PCIe), the warm reset resets
//    SI0, the target CPU and the copy engines through SOC_RESET_CONTROL (ath10k_pci_warm_reset and its helpers); after either the boot ROM sets FW_IND_INITIALIZED in
//    FW_INDICATOR when it is ready for BMI (ath10k_pci_wait_for_target_init). QCA6174 and QCA9377 need a cold reset followed by a warm reset
//    (ath10k_pci_qca6174_chip_reset).
//  - Ath10kCePipe: one copy engine (CE). A CE has a source ring (host to target) and a destination ring (target to host) of 8-byte descriptors in host memory: the
//    buffer's bus address (32 bits), its length, and flags (gather, byte swap, and the transfer id as meta data in bits 2..15). The host writes descriptors and moves
//    SR_WR_INDEX / DST_WR_INDEX; the engine moves CURRENT_SRRI / CURRENT_DRRI as it completes them and writes the received length into the destination descriptor.
//    The ring registers are those of qcax_ce_regs (QCA988X, QCA6174, QCA9377; ath10k/hw.c). Interrupts are not used: completions are polled.
//
// The host side of the target's own CE configuration (ath10k_pci_init_config, written through the diagnostic CE) and interrupts are not done: they matter only after
// BMI, once the firmware runs. Offsets are from the Linux source and not checked on hardware here.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath10kHw.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware::ath {

enum class Ath10kTargetInit : uint8_t { Ok, Gone, Crashed, Timeout };

class Ath10kPci {
 public:
  Ath10kPci(INicRegisters& bar, const Ath10kRegs& regs, bool power_save) : bar_(bar), regs_(regs), power_save_(power_save) {}

  const Ath10kRegs& regs() const { return regs_; }
  // BAR offsets as ath10k_pci_read32/write32 use them (the caller keeps the chip awake).
  uint32_t read32(uint32_t offset) { return bar_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) { bar_.write32(offset, value); }
  uint32_t soc_read32(uint32_t address) { return read32(regs_.rtc_soc_base + address); }
  void soc_write32(uint32_t address, uint32_t value) { write32(regs_.rtc_soc_base + address, value); }
  // The PCIe local block (ath10k_pci_reg_read32/write32), reachable while the SoC sleeps.
  uint32_t local_read32(uint32_t address) { return bar_.read32(regs_.pcie_local_base_address + address); }
  void local_write32(uint32_t address, uint32_t value) { bar_.write32(regs_.pcie_local_base_address + address, value); }

  // Wakes the SoC and waits for RTC_STATE "on" (30 ms). True at once for a chip without power save.
  [[nodiscard]] bool wake();
  void sleep();
  bool awake();

  uint32_t chip_id() { return soc_read32(regs_.soc_chip_id_address); }
  uint32_t fw_indicator() { return read32(regs_.fw_indicator_address); }
  bool firmware_crashed() { const uint32_t v = fw_indicator(); return v != 0xFFFFFFFFu && (v & kFwIndEventPending); }

  // Target and PCIe through SOC_GLOBAL_RESET (20 ms in reset, 20 ms after).
  void cold_reset();
  // SI0, CPU and CE resets with the LF timer stopped; waits for the target after each CPU reset.
  [[nodiscard]] Ath10kTargetInit warm_reset();
  // Polls FW_INDICATOR for FW_IND_INITIALIZED (3 s, as ATH10K_PCI_TARGET_WAIT).
  [[nodiscard]] Ath10kTargetInit wait_for_target_init();
  // ath10k_pci_qca6174_chip_reset: cold reset, wait, warm reset.
  [[nodiscard]] Ath10kTargetInit qca6174_chip_reset();
  // Raises the CPU interrupt in CORE_CTRL so the target's ROM looks at its mailboxes (ath10k_pci_wake_target_cpu).
  void wake_target_cpu();

 private:
  void warm_reset_si0();
  void warm_reset_cpu();
  void warm_reset_ce();
  void warm_reset_clear_lf();

  INicRegisters& bar_;
  const Ath10kRegs& regs_;
  bool power_save_;
};

// The per-CE registers of qcax_ce_regs, relative to the engine's base (Ath10kRegs::ce_base).
namespace ce_reg {
constexpr uint32_t kSrBaseLow = 0x00, kSrSize = 0x04, kDrBaseLow = 0x08, kDrSize = 0x0C, kCtrl1 = 0x10, kCmd = 0x18, kHostIe = 0x2C, kHostIs = 0x30, kMiscIe = 0x34,
                   kMiscIs = 0x38, kSrWrIndex = 0x3C, kDstWrIndex = 0x40, kCurrentSrri = 0x44, kCurrentDrri = 0x48, kSrcWatermark = 0x4C, kDstWatermark = 0x50;
constexpr uint32_t kCtrl1DmaxLengthMask = 0x0000FFFF, kCtrl1SrcByteSwap = 0x00010000, kCtrl1DstByteSwap = 0x00020000;
constexpr uint32_t kWatermarkLowShift = 16, kWatermarkLowMask = 0xFFFF0000u, kWatermarkHighMask = 0x0000FFFFu;
}  // namespace ce_reg

// struct ce_desc (32-bit address form, QCA988X/QCA6174/QCA9377).
struct Ath10kCeDesc {
  uint32_t addr = 0;
  uint16_t nbytes = 0;
  uint16_t flags = 0;
};
constexpr uint32_t kCeDescSize = 8, kCeDescRingAlign = 8;
constexpr uint16_t kCeDescFlagGather = 1u << 0, kCeDescFlagByteSwap = 1u << 1;
// Reads / writes descriptor `index` of a ring. False if the index is outside it.
[[nodiscard]] bool ath10k_ce_desc_read(ConstByteView ring, uint32_t index, Ath10kCeDesc* out);
[[nodiscard]] bool ath10k_ce_desc_write(ByteView ring, uint32_t index, const Ath10kCeDesc& desc);
// The flags word of a sent descriptor: the transfer id in the meta data bits (ath10k_ce_send_nolock; BMI sends with id -1, i.e. all meta data bits set).
uint16_t ath10k_ce_send_flags(const Ath10kRegs& regs, uint32_t transfer_id, bool byte_swap, bool gather);

// One copy engine with host-side rings (ath10k_ce_alloc_pipe + ath10k_ce_init_pipe). Entries are rounded up to a power of two as the driver does.
class Ath10kCePipe {
 public:
  Ath10kCePipe(Ath10kPci& pci, allocators::IAllocator& allocator) : pci_(pci), allocator_(allocator) {}
  ~Ath10kCePipe();
  Ath10kCePipe(const Ath10kCePipe&) = delete;
  Ath10kCePipe& operator=(const Ath10kCePipe&) = delete;

  // Allocates the rings and programs the engine. False if memory ran out or lies above 4 GiB (the descriptors' addresses have 32 bits).
  [[nodiscard]] bool init(uint32_t ce_id, uint32_t src_entries, uint32_t src_size_max, uint32_t dest_entries);
  // Frees the rings (the engine must be stopped or reset).
  void release();

  // Queues `length` bytes at bus address `address` on the source ring. False if the ring is full or the length is above the engine's maximum.
  [[nodiscard]] bool send(uint32_t address, uint32_t length, uint32_t transfer_id);
  // True once the oldest outstanding send has been taken by the engine; false while it has not (or the chip is gone: *gone).
  [[nodiscard]] bool completed_send(bool* gone);
  // Hands a receive buffer at bus address `address` to the destination ring.
  [[nodiscard]] bool post_receive(uint32_t address);
  // True once the oldest posted buffer was filled; *length is what the target wrote.
  [[nodiscard]] bool completed_receive(uint32_t* length);

  uint32_t base() const { return pci_.regs().ce_base(ce_id_); }
  ConstByteView source_ring() const { return src_ring_; }
  ConstByteView dest_ring() const { return dest_ring_; }

 private:
  Ath10kPci& pci_;
  allocators::IAllocator& allocator_;
  uint32_t ce_id_ = 0, src_size_max_ = 0;
  ByteView src_ring_, dest_ring_;
  uint32_t src_entries_ = 0, src_sw_ = 0, src_write_ = 0, src_hw_ = 0;
  uint32_t dest_entries_ = 0, dest_sw_ = 0, dest_write_ = 0;
};

// The bus address of DMA memory as a 32-bit descriptor address; false if it lies above 4 GiB.
[[nodiscard]] bool ath10k_dma_address32(ConstByteView memory, uint32_t* out);

}  // namespace hardware::ath
