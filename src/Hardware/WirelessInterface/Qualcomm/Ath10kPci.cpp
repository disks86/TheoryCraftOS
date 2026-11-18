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

#include "Hardware/WirelessInterface/Qualcomm/Ath10kPci.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware::ath {

namespace {
constexpr uint32_t kWakeTimeoutUs = 30000;          // PCIE_WAKE_TIMEOUT
constexpr uint32_t kTargetWaitMs = 3000;            // ATH10K_PCI_TARGET_WAIT
constexpr uint32_t kTargetPollMs = 10;

uint32_t round_up_pow2(uint32_t n) {
  uint32_t p = 1;
  while (p < n && p < (1u << 16)) p <<= 1;
  return p;
}

// The device and the CPU share the descriptor rings; keep the compiler from caching what the engine may have written.
inline void dma_barrier() { asm volatile("" ::: "memory"); }
}  // namespace

bool Ath10kPci::awake() { return (local_read32(kRtcStateAddress) & kRtcStateVMask) == regs_.rtc_state_val_on; }

bool Ath10kPci::wake() {
  if (!power_save_) return true;
  local_write32(kPcieSocWakeAddress, kPcieSocWakeV);
  uint32_t waited = 0, step = 5;
  while (waited < kWakeTimeoutUs) {
    if (awake()) return true;
    delay_us(step);
    waited += step;
    if (step < 50) step += 5;
  }
  return awake();
}

void Ath10kPci::sleep() {
  if (power_save_) local_write32(kPcieSocWakeAddress, kPcieSocWakeReset);
}

void Ath10kPci::cold_reset() {
  uint32_t value = local_read32(kSocGlobalResetAddress);
  value |= 1;
  local_write32(kSocGlobalResetAddress, value);
  // the link is not stable right after the reset is asserted or released (the driver's comment): no access for 20 ms
  delay_us(20000);
  value &= ~1u;
  local_write32(kSocGlobalResetAddress, value);
  delay_us(20000);
}

Ath10kTargetInit Ath10kPci::wait_for_target_init() {
  uint32_t value = 0xFFFFFFFFu;
  for (uint32_t waited = 0; waited < kTargetWaitMs; waited += kTargetPollMs) {
    value = fw_indicator();
    if (value != 0xFFFFFFFFu) {
      if (value & kFwIndEventPending) return Ath10kTargetInit::Crashed;
      if (value & kFwIndInitialized) return Ath10kTargetInit::Ok;
    }
    delay_us(kTargetPollMs * 1000);
  }
  if (value == 0xFFFFFFFFu) return Ath10kTargetInit::Gone;
  return Ath10kTargetInit::Timeout;
}

void Ath10kPci::warm_reset_si0() {
  uint32_t value = soc_read32(kSocResetControlAddress);
  soc_write32(kSocResetControlAddress, value | regs_.soc_reset_control_si0_rst_mask);
  (void)soc_read32(kSocResetControlAddress);
  delay_us(10000);
  value = soc_read32(kSocResetControlAddress);
  soc_write32(kSocResetControlAddress, value & ~regs_.soc_reset_control_si0_rst_mask);
  (void)soc_read32(kSocResetControlAddress);
  delay_us(10000);
}

void Ath10kPci::warm_reset_cpu() {
  write32(regs_.fw_indicator_address, 0);
  const uint32_t value = soc_read32(kSocResetControlAddress);
  soc_write32(kSocResetControlAddress, value | kSocResetControlCpuWarmRst);
}

void Ath10kPci::warm_reset_ce() {
  const uint32_t value = soc_read32(kSocResetControlAddress);
  soc_write32(kSocResetControlAddress, value | regs_.soc_reset_control_ce_rst_mask);
  delay_us(10000);
  soc_write32(kSocResetControlAddress, value & ~regs_.soc_reset_control_ce_rst_mask);
}

void Ath10kPci::warm_reset_clear_lf() {
  const uint32_t value = soc_read32(kSocLfTimerControl0Address);
  soc_write32(kSocLfTimerControl0Address, value & ~kSocLfTimerControl0Enable);
}

Ath10kTargetInit Ath10kPci::warm_reset() {
  // ath10k_pci_warm_reset: make sure the target CPU is not doing anything dangerous (SI0, CPU), then the CEs and the CPU again. The driver re-initialises the host
  // side of the pipes between the steps; the caller sets its pipes up after the reset instead.
  warm_reset_si0();
  warm_reset_cpu();
  const Ath10kTargetInit first = wait_for_target_init();
  if (first == Ath10kTargetInit::Gone) return first;   // the driver only warns here, but a chip that reads all ones will not come back
  warm_reset_clear_lf();
  warm_reset_ce();
  warm_reset_cpu();
  return wait_for_target_init();
}

Ath10kTargetInit Ath10kPci::qca6174_chip_reset() {
  cold_reset();
  // the PCIe block (and its wake request) was reset with the target
  if (!wake()) return Ath10kTargetInit::Gone;
  const Ath10kTargetInit init = wait_for_target_init();
  if (init != Ath10kTargetInit::Ok) return init;
  return warm_reset();
}

void Ath10kPci::wake_target_cpu() {
  const uint32_t address = regs_.soc_core_base + kCoreCtrlAddress;
  write32(address, read32(address) | kCoreCtrlCpuIntr);
}

bool ath10k_ce_desc_read(ConstByteView ring, uint32_t index, Ath10kCeDesc* out) {
  ConstByteView bytes;
  if (!ring.try_subview(uint64_t{index} * kCeDescSize, kCeDescSize, &bytes)) return false;
  dma_barrier();
  ByteReader in(bytes);
  out->addr = in.le32();
  out->nbytes = in.le16();
  out->flags = in.le16();
  return in.ok();
}

bool ath10k_ce_desc_write(ByteView ring, uint32_t index, const Ath10kCeDesc& desc) {
  ByteView bytes;
  if (!ring.try_subview(uint64_t{index} * kCeDescSize, kCeDescSize, &bytes)) return false;
  ByteWriter out(bytes);
  out.le32(desc.addr);
  out.le16(desc.nbytes);
  out.le16(desc.flags);
  dma_barrier();
  return out.ok();
}

uint16_t ath10k_ce_send_flags(const Ath10kRegs& regs, uint32_t transfer_id, bool byte_swap, bool gather) {
  uint32_t flags = (transfer_id << regs.ce_desc_meta_data_lsb) & regs.ce_desc_meta_data_mask;
  if (gather) flags |= kCeDescFlagGather;
  if (byte_swap) flags |= kCeDescFlagByteSwap;
  return static_cast<uint16_t>(flags);
}

bool ath10k_dma_address32(ConstByteView memory, uint32_t* out) {
  const uint64_t address = nic_dma_address(memory);
  if (memory.empty() || address > 0xFFFFFFFFull || memory.size() > 0x100000000ull - address) return false;
  *out = static_cast<uint32_t>(address);
  return true;
}

Ath10kCePipe::~Ath10kCePipe() { release(); }

void Ath10kCePipe::release() {
  release_view(allocator_, src_ring_);
  release_view(allocator_, dest_ring_);
  src_entries_ = dest_entries_ = 0;
}

bool Ath10kCePipe::init(uint32_t ce_id, uint32_t src_entries, uint32_t src_size_max, uint32_t dest_entries) {
  release();
  ce_id_ = ce_id;
  src_size_max_ = src_size_max;
  const uint32_t base = this->base();
  if (src_entries) {
    src_entries_ = round_up_pow2(src_entries);
    src_ring_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{src_entries_} * kCeDescSize, 64);
    uint32_t address = 0;
    if (!ath10k_dma_address32(src_ring_, &address)) {
      release();
      return false;
    }
    src_ring_.fill(0);
    // ath10k_ce_init_src_ring
    src_sw_ = pci_.read32(base + ce_reg::kCurrentSrri) & (src_entries_ - 1);
    src_hw_ = src_sw_;
    src_write_ = pci_.read32(base + ce_reg::kSrWrIndex) & (src_entries_ - 1);
    pci_.write32(base + ce_reg::kSrBaseLow, address);
    pci_.write32(base + ce_reg::kSrSize, src_entries_);
    uint32_t ctrl1 = pci_.read32(base + ce_reg::kCtrl1);
    ctrl1 = (ctrl1 & ~ce_reg::kCtrl1DmaxLengthMask) | (src_size_max & ce_reg::kCtrl1DmaxLengthMask);
    pci_.write32(base + ce_reg::kCtrl1, ctrl1 & ~ce_reg::kCtrl1SrcByteSwap);
    const uint32_t watermark = pci_.read32(base + ce_reg::kSrcWatermark);
    pci_.write32(base + ce_reg::kSrcWatermark, (watermark & ~(ce_reg::kWatermarkLowMask | ce_reg::kWatermarkHighMask)) | (src_entries_ & ce_reg::kWatermarkHighMask));
  }
  if (dest_entries) {
    dest_entries_ = round_up_pow2(dest_entries);
    dest_ring_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{dest_entries_} * kCeDescSize, 64);
    uint32_t address = 0;
    if (!ath10k_dma_address32(dest_ring_, &address)) {
      release();
      return false;
    }
    dest_ring_.fill(0);
    // ath10k_ce_init_dest_ring
    dest_sw_ = pci_.read32(base + ce_reg::kCurrentDrri) & (dest_entries_ - 1);
    dest_write_ = pci_.read32(base + ce_reg::kDstWrIndex) & (dest_entries_ - 1);
    pci_.write32(base + ce_reg::kDrBaseLow, address);
    pci_.write32(base + ce_reg::kDrSize, dest_entries_);
    pci_.write32(base + ce_reg::kCtrl1, pci_.read32(base + ce_reg::kCtrl1) & ~ce_reg::kCtrl1DstByteSwap);
    const uint32_t watermark = pci_.read32(base + ce_reg::kDstWatermark);
    pci_.write32(base + ce_reg::kDstWatermark, (watermark & ~(ce_reg::kWatermarkLowMask | ce_reg::kWatermarkHighMask)) | (dest_entries_ & ce_reg::kWatermarkHighMask));
  }
  return true;
}

bool Ath10kCePipe::send(uint32_t address, uint32_t length, uint32_t transfer_id) {
  if (!src_entries_ || length == 0 || length > src_size_max_ || length > 0xFFFF) return false;
  const uint32_t mask = src_entries_ - 1;
  if (((src_sw_ - 1 - src_write_) & mask) == 0) return false;     // CE_RING_DELTA(write, sw - 1) == 0: full
  Ath10kCeDesc desc;
  desc.addr = address;
  desc.nbytes = static_cast<uint16_t>(length);
  desc.flags = ath10k_ce_send_flags(pci_.regs(), transfer_id, false, false);
  if (!ath10k_ce_desc_write(src_ring_, src_write_, desc)) return false;
  src_write_ = (src_write_ + 1) & mask;
  pci_.write32(base() + ce_reg::kSrWrIndex, src_write_);
  return true;
}

bool Ath10kCePipe::completed_send(bool* gone) {
  *gone = false;
  if (!src_entries_) return false;
  const uint32_t mask = src_entries_ - 1;
  if (src_sw_ == src_hw_) {
    const uint32_t hw = pci_.read32(base() + ce_reg::kCurrentSrri);
    if (hw == 0xFFFFFFFFu) {
      *gone = true;
      return false;
    }
    src_hw_ = hw & mask;
  }
  if (src_sw_ == src_hw_) return false;
  src_sw_ = (src_sw_ + 1) & mask;
  return true;
}

bool Ath10kCePipe::post_receive(uint32_t address) {
  if (!dest_entries_) return false;
  const uint32_t mask = dest_entries_ - 1;
  if (((dest_sw_ - 1 - dest_write_) & mask) == 0) return false;
  Ath10kCeDesc desc;
  desc.addr = address;
  if (!ath10k_ce_desc_write(dest_ring_, dest_write_, desc)) return false;
  dest_write_ = (dest_write_ + 1) & mask;
  pci_.write32(base() + ce_reg::kDstWrIndex, dest_write_);
  return true;
}

bool Ath10kCePipe::completed_receive(uint32_t* length) {
  if (!dest_entries_ || dest_sw_ == dest_write_) return false;
  Ath10kCeDesc desc;
  if (!ath10k_ce_desc_read(dest_ring_, dest_sw_, &desc) || desc.nbytes == 0) return false;   // not written yet
  *length = desc.nbytes;
  desc.nbytes = 0;
  if (!ath10k_ce_desc_write(dest_ring_, dest_sw_, desc)) return false;
  dest_sw_ = (dest_sw_ + 1) & (dest_entries_ - 1);
  return true;
}

}  // namespace hardware::ath
