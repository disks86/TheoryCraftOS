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

#include "Hardware/NetworkInterface/AMD/PcnetNetworkInterface.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// I/O register offsets in 32-bit (DWIO) mode.
constexpr uint32_t kRdp = 0x10, kRap = 0x14, kReset = 0x18, kBdp = 0x1C;

constexpr uint32_t kCsr0Init = 1u << 0, kCsr0Start = 1u << 1, kCsr0Stop = 1u << 2, kCsr0TransmitDemand = 1u << 3,
                   kCsr0InitDone = 1u << 8;
constexpr uint32_t kCsr4AutoPad = 1u << 11, kCsr4TransmitStart = 1u << 3;
constexpr uint32_t kDescriptorOwn = 1u << 31, kDescriptorError = 1u << 30, kDescriptorStart = 1u << 25, kDescriptorEnd = 1u << 24;
constexpr uint32_t kLengthMask = 0xFFF, kLengthOnes = 0xF000;   // the chip wants the 12-bit two's complement with bits 12-15 set
constexpr uint32_t kSoftwareStyle2 = 2;
constexpr uint32_t kInitBlockBytes = 28;
constexpr uint32_t kPciCommand = 0x04;

constexpr uint32_t log2_of(uint32_t n) { return n <= 1 ? 0 : 1 + log2_of(n / 2); }
}  // namespace

uint32_t PcnetNetworkInterface::read_csr(uint32_t i) const {
  IoPort(io_ + kRap).write32(i);
  return IoPort(io_ + kRdp).read32();
}
void PcnetNetworkInterface::write_csr(uint32_t i, uint32_t v) const {
  IoPort(io_ + kRap).write32(i);
  IoPort(io_ + kRdp).write32(v);
}
uint32_t PcnetNetworkInterface::read_bcr(uint32_t i) const {
  IoPort(io_ + kRap).write32(i);
  return IoPort(io_ + kBdp).read32();
}
void PcnetNetworkInterface::write_bcr(uint32_t i, uint32_t v) const {
  IoPort(io_ + kRap).write32(i);
  IoPort(io_ + kBdp).write32(v);
}

bool PcnetNetworkInterface::initialize() {
  if (!supports(device_.vendor_id, device_.device_id)) return false;
  const uint32_t bar = pci_.read32(device_.address, 0x10);
  if (!(bar & 1) || !(bar & 0xFFE0)) return false;   // BAR 0 is the I/O window
  io_ = static_cast<uint16_t>(bar & 0xFFE0);

  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  pci_.write32(device_.address, kPciCommand, (pci_.read32(device_.address, kPciCommand) & 0xFFFF) | 0x5);   // I/O and bus master

  for (uint32_t i = 0; i < 6; ++i) mac_.bytes[i] = IoPort(io_ + i).read8();

  // Reset, then a 32-bit write to RDP switches the chip to 32-bit register access.
  (void)IoPort(io_ + kReset).read32();
  IoPort(io_ + kReset).write32(0);
  hardware::delay_us(2000);
  IoPort(io_ + kRdp).write32(0);
  write_bcr(20, kSoftwareStyle2);   // 32-bit descriptors
  write_csr(58, kSoftwareStyle2);

  // One allocation: init block, receive ring, transmit ring, then the frame buffers.
  const uint32_t ring_bytes = (kReceiveBuffers + kTransmitBuffers) * sizeof(Descriptor);
  const uint32_t buffers_offset = (32 + ring_bytes + 15) & ~15u;
  const uint32_t total = buffers_offset + (kReceiveBuffers + kTransmitBuffers) * kBufferBytes;
  memory_ = allocate_dma_view<uint8_t>(allocator_, total, 16);
  if (memory_.empty() || reinterpret_cast<uint64_t>(memory_.data()) + total > 0x100000000ull) {
    shutdown();
    return false;
  }
  memory_.fill(0);
  uint8_t* base = memory_.data();
  const uint32_t block_address = static_cast<uint32_t>(reinterpret_cast<uint64_t>(base));
  receive_ring_ = reinterpret_cast<volatile Descriptor*>(base + 32);
  transmit_ring_ = receive_ring_ + kReceiveBuffers;
  receive_buffers_ = base + buffers_offset;
  transmit_buffers_ = receive_buffers_ + kReceiveBuffers * kBufferBytes;

  for (uint32_t i = 0; i < kReceiveBuffers; ++i) {
    receive_ring_[i].buffer = block_address + buffers_offset + i * kBufferBytes;
    receive_ring_[i].status_length = kLengthOnes | ((0u - kBufferBytes) & kLengthMask);
    receive_ring_[i].message_length = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    receive_ring_[i].status_length |= kDescriptorOwn;   // the chip may fill it
  }
  for (uint32_t i = 0; i < kTransmitBuffers; ++i)
    transmit_ring_[i].buffer = block_address + buffers_offset + (kReceiveBuffers + i) * kBufferBytes;
  receive_next_ = transmit_next_ = 0;

  // The initialization block (software style 2): mode, ring sizes, MAC address, multicast filter, ring addresses.
  uint32_t* block = reinterpret_cast<uint32_t*>(base);
  uint8_t* bytes = base;
  block[0] = (log2_of(kReceiveBuffers) << 20) | (log2_of(kTransmitBuffers) << 28);   // mode 0 (not promiscuous); RLEN, TLEN
  for (uint32_t i = 0; i < 6; ++i) bytes[4 + i] = mac_.bytes[i];
  for (uint32_t i = 0; i < 8; ++i) bytes[12 + i] = 0xFF;   // logical address filter: every multicast group
  block[5] = block_address + 32;
  block[6] = block_address + 32 + kReceiveBuffers * sizeof(Descriptor);

  write_csr(3, 0);   // polled: no interrupts
  write_csr(4, kCsr4AutoPad);
  write_csr(1, block_address & 0xFFFF);
  write_csr(2, block_address >> 16);
  write_csr(0, kCsr0Init);
  bool done = false;
  for (int i = 0; i < 1000 && !(done = (read_csr(0) & kCsr0InitDone) != 0); ++i) hardware::delay_us(100);
  if (!done) {
    shutdown();
    return false;
  }
  write_csr(0, kCsr0InitDone | kCsr0Start);   // (writing the done bit clears it)
  started_ = true;
  return true;
}

void PcnetNetworkInterface::shutdown() {
  started_ = false;
  if (io_ && owns_device_) write_csr(0, kCsr0Stop);   // the chip stops using our rings
  release_view(allocator_, memory_);
  receive_ring_ = transmit_ring_ = nullptr;
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

bool PcnetNetworkInterface::transmit(ConstByteView frame) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || frame.empty() || frame.size() > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  volatile Descriptor& d = transmit_ring_[transmit_next_];
  if (d.status_length & kDescriptorOwn) {   // the chip still holds it: the ring is full
    ++stats_.send_failures;
    return false;
  }
  const uint32_t length = frame.size() < kEthernetMinFrame ? kEthernetMinFrame : static_cast<uint32_t>(frame.size());
  const ByteView buffer = ByteView(transmit_buffers_ + transmit_next_ * kBufferBytes, kBufferBytes).first(length);
  buffer.fill(0);
  buffer.copy_from(frame);
  d.message_length = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  d.status_length = kDescriptorOwn | kDescriptorStart | kDescriptorEnd | kLengthOnes | ((0u - length) & kLengthMask);
  write_csr(0, kCsr0TransmitDemand);
  transmit_next_ = (transmit_next_ + 1) % kTransmitBuffers;
  ++stats_.frames_sent;
  return true;
}

bool PcnetNetworkInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !length) return false;
  for (uint32_t scanned = 0; scanned < kReceiveBuffers; ++scanned) {
    volatile Descriptor& d = receive_ring_[receive_next_];
    const uint32_t status = d.status_length;
    if (status & kDescriptorOwn) return false;   // nothing more
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t size = d.message_length & kLengthMask;   // includes the 4-byte CRC
    const bool usable = !(status & kDescriptorError) && (status & kDescriptorStart) && (status & kDescriptorEnd) && size > 4 &&
                        size - 4 <= kBufferBytes;
    bool delivered = false;
    if (usable) {
      const uint32_t frame = size - 4;
      buffer.copy_from(ConstByteView(receive_buffers_ + receive_next_ * kBufferBytes, frame).at_most(buffer.size()));
      *length = frame;
      ++stats_.frames_received;
      delivered = true;
    } else {
      ++stats_.frames_dropped;
    }
    d.message_length = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d.status_length = kDescriptorOwn | kLengthOnes | ((0u - kBufferBytes) & kLengthMask);   // back to the chip
    receive_next_ = (receive_next_ + 1) % kReceiveBuffers;
    if (delivered) return true;
  }
  return false;
}

}  // namespace hardware
