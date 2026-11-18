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

#include "Hardware/WirelessInterface/MediaTek/Mt7921Pcie.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

using namespace mt7921;

namespace {

struct FixedMap {
  uint32_t chip;     // chip address of the block
  uint32_t bar;      // BAR offset it shows at
  uint32_t size;
};

// fixed_map of __mt7921_reg_addr (mt7921/pci.c), in its order (the first block that holds the address wins).
constexpr FixedMap kFixedMap[] = {
    {0x820D0000, 0x30000, 0x10000},   // WF_LMAC_TOP (WF_WTBLON)
    {0x820ED000, 0x24800, 0x0800},    // WF_LMAC_TOP BN0 (WF_MIB)
    {0x820E4000, 0x21000, 0x0400},    // WF_LMAC_TOP BN0 (WF_TMAC)
    {0x820E7000, 0x21E00, 0x0200},    // WF_LMAC_TOP BN0 (WF_DMA)
    {0x820EB000, 0x24200, 0x0400},    // WF_LMAC_TOP BN0 (WF_LPON)
    {0x820E2000, 0x20800, 0x0400},    // WF_LMAC_TOP BN0 (WF_AGG)
    {0x820E3000, 0x20C00, 0x0400},    // WF_LMAC_TOP BN0 (WF_ARB)
    {0x820E5000, 0x21400, 0x0800},    // WF_LMAC_TOP BN0 (WF_RMAC)
    {0x00400000, 0x80000, 0x10000},   // WF_MCU_SYSRAM
    {0x00410000, 0x90000, 0x10000},   // WF_MCU_SYSRAM (configure register)
    {0x40000000, 0x70000, 0x10000},   // WF_UMAC_SYSRAM
    {0x54000000, 0x02000, 0x1000},    // WFDMA PCIE0 MCU DMA0
    {0x55000000, 0x03000, 0x1000},    // WFDMA PCIE0 MCU DMA1
    {0x58000000, 0x06000, 0x1000},    // WFDMA PCIE1 MCU DMA0 (MEM_DMA)
    {0x59000000, 0x07000, 0x1000},    // WFDMA PCIE1 MCU DMA1
    {0x7C000000, 0xF0000, 0x10000},   // CONN_INFRA
    {0x7C020000, 0xD0000, 0x10000},   // CONN_INFRA, WFDMA
    {0x7C060000, 0xE0000, 0x10000},   // CONN_INFRA, conn_host_csr_top
    {0x80020000, 0xB0000, 0x10000},   // WF_TOP_MISC_OFF
    {0x81020000, 0xC0000, 0x10000},   // WF_TOP_MISC_ON
    {0x820C0000, 0x08000, 0x4000},    // WF_UMAC_TOP (PLE)
    {0x820C8000, 0x0C000, 0x2000},    // WF_UMAC_TOP (PSE)
    {0x820CC000, 0x0E000, 0x2000},    // WF_UMAC_TOP (PP)
    {0x820CE000, 0x21C00, 0x0200},    // WF_LMAC_TOP (WF_SEC)
    {0x820CF000, 0x22000, 0x1000},    // WF_LMAC_TOP (WF_PF)
    {0x820E0000, 0x20000, 0x0400},    // WF_LMAC_TOP BN0 (WF_CFG)
    {0x820E1000, 0x20400, 0x0200},    // WF_LMAC_TOP BN0 (WF_TRB)
    {0x820E9000, 0x23400, 0x0200},    // WF_LMAC_TOP BN0 (WF_WTBLOFF)
    {0x820EA000, 0x24000, 0x0200},    // WF_LMAC_TOP BN0 (WF_ETBF)
    {0x820EC000, 0x24600, 0x0200},    // WF_LMAC_TOP BN0 (WF_INT)
    {0x820F0000, 0xA0000, 0x0400},    // WF_LMAC_TOP BN1 (WF_CFG)
    {0x820F1000, 0xA0600, 0x0200},    // WF_LMAC_TOP BN1 (WF_TRB)
    {0x820F2000, 0xA0800, 0x0400},    // WF_LMAC_TOP BN1 (WF_AGG)
    {0x820F3000, 0xA0C00, 0x0400},    // WF_LMAC_TOP BN1 (WF_ARB)
    {0x820F4000, 0xA1000, 0x0400},    // WF_LMAC_TOP BN1 (WF_TMAC)
    {0x820F5000, 0xA1400, 0x0800},    // WF_LMAC_TOP BN1 (WF_RMAC)
    {0x820F7000, 0xA1E00, 0x0200},    // WF_LMAC_TOP BN1 (WF_DMA)
    {0x820F9000, 0xA3400, 0x0200},    // WF_LMAC_TOP BN1 (WF_WTBLOFF)
    {0x820FA000, 0xA4000, 0x0200},    // WF_LMAC_TOP BN1 (WF_ETBF)
    {0x820FB000, 0xA4200, 0x0400},    // WF_LMAC_TOP BN1 (WF_LPON)
    {0x820FC000, 0xA4600, 0x0200},    // WF_LMAC_TOP BN1 (WF_INT)
    {0x820FD000, 0xA4800, 0x0800},    // WF_LMAC_TOP BN1 (WF_MIB)
};

// The bus address of DMA memory. The kernel's DMA allocator hands out identity-mapped memory below 4 GiB (IAllocator::allocate_dma), so the pointer's value is the
// address the chip uses; nothing is accessed through the integer.
uint64_t bus_address(const void* memory) { return reinterpret_cast<uint64_t>(memory); }

}  // namespace

bool mt7921_fixed_map(uint32_t address, uint32_t* bar_offset) {
  if (address < 0x100000) {
    *bar_offset = address;
    return true;
  }
  for (const FixedMap& block : kFixedMap) {
    if (address < block.chip) continue;
    const uint32_t offset = address - block.chip;
    if (offset >= block.size) continue;
    *bar_offset = block.bar + offset;
    return true;
  }
  return false;
}

Mt7921Window mt7921_window(uint32_t address) {
  uint32_t unused;
  if (mt7921_fixed_map(address, &unused)) return Mt7921Window::Fixed;
  if ((address >= 0x18000000 && address < 0x18C00000) || (address >= 0x70000000 && address < 0x78000000) || (address >= 0x7C000000 && address < 0x7C400000))
    return Mt7921Window::L1;
  return Mt7921Window::L2;
}

uint32_t Mt7921Registers::map(uint32_t address) {
  uint32_t offset;
  if (mt7921_fixed_map(address, &offset)) return offset;
  if (mt7921_window(address) == Mt7921Window::L1) {
    // mt7921_reg_map_l1: the 64 KiB page in MT_HIF_REMAP_L1[31:16]; the read pushes the write out
    bar_.write32(kHifRemapL1, (bar_.read32(kHifRemapL1) & ~kHifRemapL1Mask) | (address & 0xFFFF0000u));
    (void)bar_.read32(kHifRemapL1);
    return kHifRemapBaseL1 + (address & 0xFFFF);
  }
  // mt7921_reg_map_l2: the 4 KiB page number in MT_HIF_REMAP_L2[19:0]
  bar_.write32(kHifRemapL2, (bar_.read32(kHifRemapL2) & ~kHifRemapL2Mask) | ((address >> 12) & kHifRemapL2Mask));
  (void)bar_.read32(kHifRemapL2);
  return kHifRemapBaseL2 + (address & 0xFFF);
}

uint32_t Mt7921Registers::read(uint32_t address) { return bar_.read32(map(address)); }
void Mt7921Registers::write(uint32_t address, uint32_t value) { bar_.write32(map(address), value); }

bool Mt7921Registers::poll(uint32_t address, uint32_t mask, uint32_t value, uint32_t tries, uint32_t step_us) {
  for (uint32_t i = 0; i < tries; ++i) {
    if ((read(address) & mask) == value) return true;
    delay_us(step_us);
  }
  return (read(address) & mask) == value;
}

Mt7921Status mt7921_firmware_own(Mt7921Registers& regs) {
  if (regs.read(kConnOnLpctl) == 0xFFFFFFFFu) return Mt7921Status::NoResponse;
  regs.write(kConnOnLpctl, kLpcrHostSetOwn);
  return regs.poll(kConnOnLpctl, kLpcrHostOwnSync, kLpcrHostOwnSync, 50, 1000) ? Mt7921Status::Ok : Mt7921Status::Timeout;
}

Mt7921Status mt7921_driver_own(Mt7921Registers& regs) {
  if (regs.read(kConnOnLpctl) == 0xFFFFFFFFu) return Mt7921Status::NoResponse;
  for (uint32_t i = 0; i < kDrvOwnRetries; ++i) {
    regs.write(kConnOnLpctl, kLpcrHostClrOwn);
    if (regs.poll(kConnOnLpctl, kLpcrHostOwnSync, 0, 50, 1000)) return Mt7921Status::Ok;
  }
  return Mt7921Status::Timeout;
}

Mt7921Status mt7921_wfsys_reset(Mt7921Registers& regs, uint32_t reset_register) {
  regs.clear(reset_register, kWfsysSwRstB);
  delay_us(50000);
  regs.set(reset_register, kWfsysSwRstB);
  return regs.poll(reset_register, kWfsysSwInitDone, kWfsysSwInitDone, 500, 1000) ? Mt7921Status::Ok : Mt7921Status::Timeout;
}

Mt7921Status mt7921_dma_disable(Mt7921Registers& regs, bool force) {
  regs.clear(kWfdma0GloCfg, kGloTxDmaEn | kGloRxDmaEn | kGloCsrDispBasePtrChainEn | kGloOmitTxInfo | kGloOmitRxInfo | kGloOmitRxInfoPfet2);
  if (!regs.poll(kWfdma0GloCfg, kGloTxDmaBusy | kGloRxDmaBusy, 0, 100, 1000)) return Mt7921Status::Timeout;
  regs.clear(kWfdma0GloCfgExt0, kWfdma0CsrTxDmashdlEnable);
  regs.set(kDmashdlSwControl, kDmashdlBypass);
  if (force) {
    regs.clear(kWfdma0Rst, kWfdma0RstDmashdlAll | kWfdma0RstLogic);
    regs.set(kWfdma0Rst, kWfdma0RstDmashdlAll | kWfdma0RstLogic);
  }
  return Mt7921Status::Ok;
}

void mt7921_dma_enable(Mt7921Registers& regs) {
  // mt792x_dma_prefetch, MT7921 / MT7922 values
  regs.write(rx_ring_ext_ctrl(0), prefetch(0x000, 4));
  regs.write(rx_ring_ext_ctrl(2), prefetch(0x040, 4));
  regs.write(rx_ring_ext_ctrl(3), prefetch(0x080, 4));
  regs.write(rx_ring_ext_ctrl(4), prefetch(0x0C0, 4));
  regs.write(rx_ring_ext_ctrl(5), prefetch(0x100, 4));
  for (uint32_t ring = 0; ring <= 6; ++ring) regs.write(tx_ring_ext_ctrl(ring), prefetch(0x140 + 0x40 * ring, 4));
  regs.write(tx_ring_ext_ctrl(kTxRingFwdl), prefetch(0x340, 4));
  regs.write(tx_ring_ext_ctrl(kTxRingMcuWm), prefetch(0x380, 4));
  // reset the DMA indices, no delayed interrupts
  regs.write(kWfdma0RstDtxPtr, 0xFFFFFFFFu);
  regs.write(kWfdma0PriDlyIntCfg0, 0);
  regs.set(kWfdma0GloCfg, kGloTxWbDdone | kGloFifoLittleEndian | kGloClkGatDis | kGloOmitTxInfo | kGloCsrDispBasePtrChainEn | kGloOmitRxInfoPfet2);
  regs.set(kWfdma0GloCfg, kGloTxDmaEn | kGloRxDmaEn);
  regs.set(kWfdmaDummyCr, kWfdmaNeedReinit);
  // interrupts stay masked (MT_WFDMA0_HOST_INT_ENA 0): the download polls the rings
  regs.write(kWfdma0HostIntEna, 0);
}

// --- rings ----------------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t Mt7921Ring::word(uint32_t descriptor, uint32_t index) const { return __atomic_load_n(&descriptors_[4ull * descriptor + index], __ATOMIC_ACQUIRE); }
void Mt7921Ring::set_word(uint32_t descriptor, uint32_t index, uint32_t value) { __atomic_store_n(&descriptors_[4ull * descriptor + index], value, __ATOMIC_RELEASE); }

uint64_t Mt7921Ring::descriptor_bus_address() const { return bus_address(descriptors_.data()); }

void Mt7921Ring::arm_receive(uint32_t descriptor) {
  set_word(descriptor, 0, static_cast<uint32_t>(bus_address(buffers_.subview(static_cast<uint64_t>(descriptor) * buffer_bytes_, buffer_bytes_).data())));
  set_word(descriptor, 2, 0);
  set_word(descriptor, 3, 0);
  set_word(descriptor, 1, (buffer_bytes_ << kDmaCtlSdLen0Shift) & kDmaCtlSdLen0Mask);
}

bool Mt7921Ring::create(allocators::IAllocator& allocator, Mt7921Registers& regs, uint32_t registers, uint32_t count, uint32_t buffer_bytes, bool receive) {
  release();
  if (count < 2 || buffer_bytes == 0 || buffer_bytes > (kDmaCtlSdLen0Mask >> kDmaCtlSdLen0Shift)) return false;
  allocator_ = &allocator;
  descriptors_ = allocate_dma_view<uint32_t>(allocator, 4ull * count, 256);
  buffers_ = allocate_dma_view<uint8_t>(allocator, static_cast<uint64_t>(count) * buffer_bytes, 64);
  if (descriptors_.empty() || buffers_.empty()) {
    release();
    return false;
  }
  // the ring and its buffers have to be reachable with 32 bit addresses (buf0 and the base register are 32 bits wide here)
  const uint64_t last_buffer = bus_address(buffers_.data()) + buffers_.size();
  const uint64_t last_descriptor = bus_address(descriptors_.data()) + descriptors_.size_bytes();
  if (last_buffer > 0x100000000ull || last_descriptor > 0x100000000ull) {
    release();
    return false;
  }
  registers_ = registers;
  count_ = count;
  buffer_bytes_ = buffer_bytes;
  receive_ = receive;
  buffers_.fill(0);
  for (uint32_t i = 0; i < count; ++i) {
    if (receive) {
      arm_receive(i);
    } else {
      set_word(i, 0, 0);
      set_word(i, 2, 0);
      set_word(i, 3, 0);
      set_word(i, 1, kDmaCtlDmaDone);
    }
  }
  // mt76_dma_queue_reset + mt76_dma_sync_idx: indices to 0, base and size, then start where the chip is
  regs.write(registers + 0x8, 0);
  regs.write(registers + 0xC, 0);
  regs.write(registers + 0x0, static_cast<uint32_t>(descriptor_bus_address()));
  regs.write(registers + 0x4, count);
  head_ = tail_ = regs.read(registers + 0xC) % count;
  if (receive) {
    // every buffer belongs to the chip, up to the one before the CPU index
    head_ = (tail_ + count - 1) % count;
    regs.write(registers + 0x8, head_);
  }
  return true;
}

void Mt7921Ring::release() {
  if (allocator_) {
    release_view(*allocator_, descriptors_);
    release_view(*allocator_, buffers_);
  }
  allocator_ = nullptr;
  registers_ = count_ = buffer_bytes_ = head_ = tail_ = 0;
}

bool Mt7921Ring::send(Mt7921Registers& regs, ConstByteView data, uint32_t tries, uint32_t step_us) {
  if (!valid() || receive_ || data.empty() || data.size() > buffer_bytes_) return false;
  ByteView buffer = buffers_.subview(static_cast<uint64_t>(head_) * buffer_bytes_, buffer_bytes_);
  if (!buffer.copy_from(data)) return false;
  set_word(head_, 0, static_cast<uint32_t>(bus_address(buffer.data())));
  set_word(head_, 2, 0);
  set_word(head_, 3, 0);
  set_word(head_, 1, ((static_cast<uint32_t>(data.size()) << kDmaCtlSdLen0Shift) & kDmaCtlSdLen0Mask) | kDmaCtlLastSec0);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  head_ = (head_ + 1) % count_;
  regs.write(registers_ + 0x8, head_);
  // wait until the chip has fetched it (one buffer per descriptor, so later sends do not overwrite it before that)
  if (!regs.poll(registers_ + 0xC, 0xFFFFFFFFu, head_, tries, step_us)) return false;
  tail_ = head_;
  return true;
}

ConstByteView Mt7921Ring::peek_received() {
  if (!valid() || !receive_) return ConstByteView();
  const uint32_t ctrl = word(tail_, 1);
  if (!(ctrl & kDmaCtlDmaDone)) return ConstByteView();
  uint32_t length = (ctrl & kDmaCtlSdLen0Mask) >> kDmaCtlSdLen0Shift;
  if (length > buffer_bytes_) length = buffer_bytes_;
  return ConstByteView(buffers_.subview(static_cast<uint64_t>(tail_) * buffer_bytes_, length));
}

void Mt7921Ring::release_received(Mt7921Registers& regs) {
  if (!valid() || !receive_) return;
  arm_receive(tail_);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  head_ = tail_;
  regs.write(registers_ + 0x8, head_);
  tail_ = (tail_ + 1) % count_;
}

}  // namespace hardware
