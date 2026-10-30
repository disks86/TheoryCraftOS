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

#include "Hardware/Bus/Thunderbolt/ThunderboltRing.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPollStepUs = 50;

// The chip and the host share the descriptors: aligned 32 bit volatile accesses inside a view that was bounds-checked by the caller, ordered against the doorbell.
uint32_t read_shared32(const uint32_t* p) {
  const uint32_t v = *static_cast<const volatile uint32_t*>(p);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return v;
}
void write_shared32(uint32_t* p, uint32_t v) { *static_cast<volatile uint32_t*>(p) = v; }
}  // namespace

// (the only place a pointer becomes a bus address: the allocator's DMA memory is below 4 GiB and identity mapped, which the rings' 64 bit address registers can hold)
uint64_t tb_bus_address(const void* dma_memory) { return reinterpret_cast<uint64_t>(dma_memory); }

void TbNhi::disable_interrupts() {
  const uint32_t hops = hop_count();
  for (uint32_t& m : interrupt_mask_) m = 0;
  // (an Intel host interface: the enable registers are written whole, and reading a notify register clears what it holds - nhi.c's QUIRK_AUTO_CLEAR_INT)
  for (uint32_t i = 0; i < (31 + 2 * hops) / 32 && i < 64; ++i) registers_.write32(tb_reg::kRingInterruptBase + i * 4, 0);
  for (uint32_t i = 0; i < (31 + 3 * hops) / 32 && i < 96; ++i) registers_.read32(tb_reg::kRingNotifyBase + i * 4);
}

int TbNhi::allocate_hop(bool transmit) {
  const uint32_t hops = hop_count();
  uint32_t* used = hops_used_[transmit ? 1 : 0];
  for (uint32_t hop = 1; hop < hops && hop < 1024; ++hop) {
    if (used[hop / 32] >> (hop % 32) & 1) continue;
    used[hop / 32] |= 1u << (hop % 32);
    return static_cast<int>(hop);
  }
  return -1;
}

void TbNhi::release_hop(bool transmit, uint32_t hop) {
  if (hop == 0 || hop >= 1024) return;
  hops_used_[transmit ? 1 : 0][hop / 32] &= ~(1u << (hop % 32));
}

void TbNhi::set_control_interrupts(bool on, int vector) {
  const uint32_t hops = hop_count();
  if (hops == 0 || hops > 1023 || vector > 15) return;
  const uint32_t indexes[2] = {0, hops};         // hop 0, transmit and receive
  for (uint32_t index : indexes) {
    const uint32_t reg = index / 32, bit = 1u << (index % 32);
    if (reg >= 64) continue;
    if (vector >= 0) {
      // (ring_interrupt_active with an MSI-X vector: auto clear on, the ring's slot of the vector allocation names the entry, the throttling of that entry is none)
      const uint32_t misc = registers_.read32(tb_reg::kDmaMisc);
      if (!(misc & tb_reg::kDmaMiscIntAutoClear)) registers_.write32(tb_reg::kDmaMisc, misc | tb_reg::kDmaMiscIntAutoClear);
      const uint32_t step = index / tb_reg::kIntVecAllocPerRegister * 4, shift = index % tb_reg::kIntVecAllocPerRegister * tb_reg::kIntVecAllocBits;
      uint32_t allocation = registers_.read32(tb_reg::kIntVecAllocBase + step);
      allocation &= ~(tb_reg::kIntVecAllocMask << shift);
      if (on) allocation |= static_cast<uint32_t>(vector) << shift;
      registers_.write32(tb_reg::kIntVecAllocBase + step, allocation);
      registers_.write32(tb_reg::kIntThrottlingRate + static_cast<uint32_t>(vector) * 4, 0);
    }
    if (on) interrupt_mask_[reg] |= bit;
    else interrupt_mask_[reg] &= ~bit;
    registers_.write32(tb_reg::kRingInterruptBase + reg * 4, interrupt_mask_[reg]);
  }
}

bool TbNhi::acknowledge_interrupt() {
  const uint32_t hops = hop_count();
  uint32_t any = 0;
  for (uint32_t i = 0; i < (31 + 3 * hops) / 32 && i < 96; ++i) any |= registers_.read32(tb_reg::kRingNotifyBase + i * 4);
  return any != 0;
}

bool TbNhi::wait_firmware_authenticated(uint32_t timeout_us) {
  for (uint32_t waited = 0;; waited += kPollStepUs) {
    if (registers_.read32(tb_reg::kFirmwareStatus) & tb_reg::kFwStsNvmAuthDone) return true;
    if (waited >= timeout_us) return false;
    delay_us(kPollStepUs);
  }
}

TbFirmwareMode TbNhi::firmware_mode() {
  const uint32_t mode = (registers_.read32(tb_reg::kOutmailCmd) & tb_reg::kOutmailOpModeMask) >> tb_reg::kOutmailOpModeShift;
  return mode <= 3 ? static_cast<TbFirmwareMode>(mode) : TbFirmwareMode::Other;
}

TbMailboxResult TbNhi::mailbox(uint32_t command, uint32_t data, uint32_t timeout_us) {
  registers_.write32(tb_reg::kInmailData, data);
  uint32_t value = registers_.read32(tb_reg::kInmailCmd);
  value &= ~(tb_reg::kInmailCmdMask | tb_reg::kInmailError);
  value |= tb_reg::kInmailRequest | (command & tb_reg::kInmailCmdMask);
  registers_.write32(tb_reg::kInmailCmd, value);
  for (uint32_t waited = 0;; waited += kPollStepUs) {
    value = registers_.read32(tb_reg::kInmailCmd);
    if (!(value & tb_reg::kInmailRequest)) break;
    if (waited >= timeout_us) return TbMailboxResult::Timeout;
    delay_us(kPollStepUs);
  }
  return (value & tb_reg::kInmailError) ? TbMailboxResult::Error : TbMailboxResult::Ok;
}

bool TbRing::start(uint32_t frame_bytes, uint16_t sof_mask, uint16_t eof_mask) {
  if (frame_bytes != 0 && (frame_bytes < 256 || frame_bytes > 4095)) return false;
  Mode mode;
  mode.frame = false;
  return start_ring(mode, frame_bytes, sof_mask, eof_mask);
}

bool TbRing::start(const Mode& mode, uint16_t sof_mask, uint16_t eof_mask) { return start_ring(mode, 0, sof_mask, eof_mask); }

bool TbRing::start_ring(const Mode& mode, uint32_t frame_bytes, uint16_t sof_mask, uint16_t eof_mask) {
  stop();
  if (size_ < 2 || size_ > 0xFFFF) return false;
  descriptors_ = allocate_dma_view<uint32_t>(allocator_, static_cast<uint64_t>(size_) * (kDescriptorBytes / 4), 16);
  if (descriptors_.size() == 0) return false;
  descriptors_.fill(0);
  head_ = tail_ = 0;
  frame_mode_ = mode.frame;
  const uint32_t base = ring_base(), options = options_base();
  const uint64_t bus = tb_bus_address(descriptors_.data());
  registers_.write32(base + 0, static_cast<uint32_t>(bus));
  registers_.write32(base + 4, static_cast<uint32_t>(bus >> 32));
  // (a frame size of zero means 4096; the raw mode wants the smallest, 256)
  if (transmit_) {
    registers_.write32(base + 12, size_);
    registers_.write32(options + 4, 0);
  } else {
    registers_.write32(base + 12, ((mode.frame ? 0 : frame_bytes) << 16) | size_);
    registers_.write32(options + 4, (static_cast<uint32_t>(sof_mask) << 16) | eof_mask);
  }
  uint32_t flags = tb_reg::kRingFlagEnable | (mode.frame ? 0 : tb_reg::kRingFlagRaw);
  registers_.write32(options + 0, flags);
  // end to end flow control is switched on once the ring is valid
  if (mode.e2e) {
    if (!transmit_) flags |= (mode.e2e_tx_hop << tb_reg::kRxOptionsE2eHopShift) & tb_reg::kRxOptionsE2eHopMask;
    registers_.write32(options + 0, flags | tb_reg::kRingFlagE2eFlowControl);
  }
  return true;
}

void TbRing::stop() {
  if (descriptors_.size() == 0) return;
  const uint32_t base = ring_base();
  registers_.write32(options_base() + 0, 0);
  registers_.write32(base + 0, 0);
  registers_.write32(base + 4, 0);
  registers_.write32(base + 8, 0);
  registers_.write32(base + 12, 0);
  release_view(allocator_, descriptors_);
  head_ = tail_ = 0;
}

bool TbRing::post(uint64_t bus, uint32_t word) {
  if (descriptors_.size() == 0 || free_slots() == 0) return false;
  const uint64_t at = static_cast<uint64_t>(head_) * (kDescriptorBytes / 4);
  write_shared32(&descriptors_[at + 0], static_cast<uint32_t>(bus));
  write_shared32(&descriptors_[at + 1], static_cast<uint32_t>(bus >> 32));
  write_shared32(&descriptors_[at + 2], word);
  write_shared32(&descriptors_[at + 3], 0);                          // the time stamp word: zero
  __atomic_thread_fence(__ATOMIC_RELEASE);
  head_ = (head_ + 1) % size_;
  // the doorbell carries the absolute head: the producer index in the upper half for a transmit ring, the consumer index for a receive ring
  registers_.write32(ring_base() + 8, transmit_ ? head_ << 16 : head_);
  return true;
}

bool TbRing::post_transmit(uint64_t bus, uint32_t bytes, uint8_t sof, uint8_t eof) {
  if (!transmit_ || bytes == 0 || bytes > 4096) return false;
  // length 12 bits (zero for 4096), EOF 4, SOF 4, flags 12
  return post(bus, (bytes & 0xFFF) | (static_cast<uint32_t>(eof & 0xF) << 12) | (static_cast<uint32_t>(sof & 0xF) << 16) | (static_cast<uint32_t>(kDescPosted | kDescInterrupt) << 20));
}

bool TbRing::post_receive(uint64_t bus) {
  if (transmit_) return false;
  return post(bus, static_cast<uint32_t>(kDescPosted | kDescInterrupt) << 20);
}

bool TbRing::poll(TbCompletion* out) {
  if (descriptors_.size() == 0 || head_ == tail_) return false;
  const uint64_t at = static_cast<uint64_t>(tail_) * (kDescriptorBytes / 4);
  const uint32_t word = read_shared32(&descriptors_[at + 2]);
  const uint16_t flags = static_cast<uint16_t>(word >> 20);
  if (!(flags & kDescCompleted)) return false;
  out->length = word & 0xFFF;
  if (frame_mode_ && !transmit_ && out->length == 0) out->length = 4096;
  out->eof = static_cast<uint8_t>((word >> 12) & 0xF);
  out->sof = static_cast<uint8_t>((word >> 16) & 0xF);
  out->flags = flags;
  out->slot = tail_;
  tail_ = (tail_ + 1) % size_;
  return true;
}

}  // namespace hardware
