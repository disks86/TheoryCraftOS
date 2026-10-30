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
  for (uint32_t i = 0; i < (31 + 2 * hops) / 32; ++i) registers_.write32(tb_reg::kRingInterruptMaskClearBase + i * 4, ~0u);
  for (uint32_t i = 0; i < (31 + 3 * hops) / 32; ++i) registers_.write32(tb_reg::kRingIntClear + i * 4, ~0u);
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
  stop();
  if (size_ < 2 || size_ > 0xFFFF || (frame_bytes != 0 && (frame_bytes < 256 || frame_bytes > 4095))) return false;
  descriptors_ = allocate_dma_view<uint32_t>(allocator_, static_cast<uint64_t>(size_) * (kDescriptorBytes / 4), 16);
  if (descriptors_.size() == 0) return false;
  descriptors_.fill(0);
  head_ = tail_ = 0;
  const uint32_t base = ring_base(), options = options_base();
  const uint64_t bus = tb_bus_address(descriptors_.data());
  registers_.write32(base + 0, static_cast<uint32_t>(bus));
  registers_.write32(base + 4, static_cast<uint32_t>(bus >> 32));
  if (transmit_) {
    registers_.write32(base + 12, size_);
    registers_.write32(options + 4, 0);
  } else {
    // (the raw mode wants the smallest frame size field: 256 bytes)
    registers_.write32(base + 12, (frame_bytes << 16) | size_);
    registers_.write32(options + 4, (static_cast<uint32_t>(sof_mask) << 16) | eof_mask);
  }
  registers_.write32(options + 0, tb_reg::kRingFlagEnable | tb_reg::kRingFlagRaw);
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
  if (!transmit_ || bytes == 0 || bytes > 4095) return false;
  // length 12 bits, EOF 4, SOF 4, flags 12
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
  out->eof = static_cast<uint8_t>((word >> 12) & 0xF);
  out->sof = static_cast<uint8_t>((word >> 16) & 0xF);
  out->flags = flags;
  out->slot = tail_;
  tail_ = (tail_ + 1) % size_;
  return true;
}

}  // namespace hardware
