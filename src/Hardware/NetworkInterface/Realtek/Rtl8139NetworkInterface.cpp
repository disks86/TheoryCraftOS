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

#include "Hardware/NetworkInterface/Realtek/Rtl8139NetworkInterface.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"

namespace hardware {

namespace {
// Register offsets (RTL8139 datasheet).
constexpr uint32_t kIdr0 = 0x00;        // MAC address, 6 bytes
constexpr uint32_t kTsd0 = 0x10;        // transmit status of slot 0 (+4 per slot)
constexpr uint32_t kTsad0 = 0x20;       // transmit buffer address of slot 0 (+4 per slot)
constexpr uint32_t kRbStart = 0x30, kCmd = 0x37, kCapr = 0x38, kImr = 0x3C, kIsr = 0x3E;
constexpr uint32_t kTcr = 0x40, kRcr = 0x44, kConfig1 = 0x52, kMsr = 0x58;

constexpr uint8_t kCmdBufferEmpty = 1u << 0, kCmdTransmitEnable = 1u << 2, kCmdReceiveEnable = 1u << 3,
                  kCmdReset = 1u << 4;
constexpr uint32_t kTsdOwn = 1u << 13;  // the chip has finished DMAing this slot's buffer
constexpr uint16_t kRxStatusOk = 1u << 0;
constexpr uint8_t kMsrLinkInactive = 1u << 2;  // 0 = link is up

// Receive config: accept our unicast, multicast and broadcast frames; WRAP lets a
// frame run past the ring's end instead of splitting; no FIFO threshold; DMA bursts unlimited.
constexpr uint32_t kRcrAcceptPhysical = 1u << 1, kRcrAcceptMulticast = 1u << 2, kRcrAcceptBroadcast = 1u << 3,
                   kRcrWrap = 1u << 7, kRcrMaxDma = 7u << 8, kRcrNoThreshold = 7u << 13;
constexpr uint32_t kTcrDefault = (3u << 24) | (7u << 8);  // standard inter-frame gap, unlimited DMA bursts
constexpr uint32_t kRingSlack = 16 + 1536;  // the chip may write one maximum frame past the nominal end
constexpr uint32_t kPciCommand = 0x04;
}  // namespace

uint8_t Rtl8139NetworkInterface::read8(uint32_t r) const { return *reinterpret_cast<volatile uint8_t*>(registers_ + r); }
uint16_t Rtl8139NetworkInterface::read16(uint32_t r) const { return *reinterpret_cast<volatile uint16_t*>(registers_ + r); }
uint32_t Rtl8139NetworkInterface::read32(uint32_t r) const { return *reinterpret_cast<volatile uint32_t*>(registers_ + r); }
void Rtl8139NetworkInterface::write8(uint32_t r, uint8_t v) const { *reinterpret_cast<volatile uint8_t*>(registers_ + r) = v; }
void Rtl8139NetworkInterface::write16(uint32_t r, uint16_t v) const { *reinterpret_cast<volatile uint16_t*>(registers_ + r) = v; }
void Rtl8139NetworkInterface::write32(uint32_t r, uint32_t v) const { *reinterpret_cast<volatile uint32_t*>(registers_ + r) = v; }

bool Rtl8139NetworkInterface::initialize() {
  if (!supports(device_.vendor_id, device_.device_id)) return false;
  registers_ = pci_.memory_bar(device_.address, 1);  // BAR 0 is the I/O-port copy, BAR 1 the memory-mapped one
  if (!registers_) return false;

  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));

  write8(kConfig1, 0);  // power on
  write8(kCmd, kCmdReset);
  for (int i = 0; i < 1000 && (read8(kCmd) & kCmdReset); ++i) st_->boot_services->stall(1000);
  if (read8(kCmd) & kCmdReset) {
    shutdown();
    return false;
  }
  for (uint32_t i = 0; i < 6; ++i) mac_.bytes[i] = read8(kIdr0 + i);

  receive_ring_ = static_cast<uint8_t*>(allocator_.allocate_aligned(kReceiveRingSize + kRingSlack, 4096));
  transmit_buffers_ = static_cast<uint8_t*>(allocator_.allocate_aligned(kTransmitSlots * kTransmitBufferSize, 4096));
  if (!receive_ring_ || !transmit_buffers_ ||
      reinterpret_cast<uint64_t>(receive_ring_) + kReceiveRingSize + kRingSlack > 0x100000000ull ||
      reinterpret_cast<uint64_t>(transmit_buffers_) + kTransmitSlots * kTransmitBufferSize > 0x100000000ull) {
    shutdown();  // out of memory, or the buffers are out of the chip's 32-bit reach
    return false;
  }
  __builtin_memset(receive_ring_, 0, kReceiveRingSize + kRingSlack);
  receive_offset_ = 0;
  transmit_next_ = 0;
  for (bool& used : transmit_used_) used = false;

  write32(kRbStart, static_cast<uint32_t>(reinterpret_cast<uint64_t>(receive_ring_)));
  write16(kImr, 0);        // polled: no interrupts
  write16(kIsr, 0xFFFF);
  write8(kCmd, kCmdReceiveEnable | kCmdTransmitEnable);
  write32(kRcr, kRcrAcceptPhysical | kRcrAcceptMulticast | kRcrAcceptBroadcast | kRcrWrap | kRcrMaxDma | kRcrNoThreshold);
  write32(kTcr, kTcrDefault);
  started_ = true;
  return true;
}

void Rtl8139NetworkInterface::shutdown() {
  started_ = false;
  if (registers_) write8(kCmd, kCmdReset);  // the chip stops DMAing into our buffers
  allocator_.deallocate(receive_ring_);
  allocator_.deallocate(transmit_buffers_);
  receive_ring_ = nullptr;
  transmit_buffers_ = nullptr;
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

bool Rtl8139NetworkInterface::link_up() const { return started_ && !(read8(kMsr) & kMsrLinkInactive); }

bool Rtl8139NetworkInterface::transmit(const uint8_t* frame, uint32_t length) {
  if (!started_ || !frame || length == 0 || length > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  const uint32_t slot = transmit_next_;
  // A slot that was used earlier is free again once the chip sets OWN.
  if (transmit_used_[slot] && !(read32(kTsd0 + 4 * slot) & kTsdOwn)) {
    ++stats_.send_failures;
    return false;
  }
  const uint32_t padded = length < kEthernetMinFrame ? kEthernetMinFrame : length;
  uint8_t* buffer = transmit_buffers_ + slot * kTransmitBufferSize;
  __builtin_memset(buffer, 0, padded);
  __builtin_memcpy(buffer, frame, length);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write32(kTsad0 + 4 * slot, static_cast<uint32_t>(reinterpret_cast<uint64_t>(buffer)));
  write32(kTsd0 + 4 * slot, padded);  // writing the length (with OWN clear) starts the transmission
  transmit_used_[slot] = true;
  transmit_next_ = (slot + 1) % kTransmitSlots;
  ++stats_.frames_sent;
  return true;
}

bool Rtl8139NetworkInterface::receive(uint8_t* buffer, uint32_t capacity, uint32_t* length) {
  if (!started_ || !buffer || !length) return false;
  while (!(read8(kCmd) & kCmdBufferEmpty)) {
    const uint8_t* packet = receive_ring_ + receive_offset_;
    const uint16_t status = packet[0] | (packet[1] << 8);
    const uint32_t size = packet[2] | (packet[3] << 8);  // includes the 4-byte CRC
    const bool usable = (status & kRxStatusOk) && size > 4 && size - 4 <= kEthernetMaxFrame + 4;
    if (usable) {
      const uint32_t frame = size - 4;
      __builtin_memcpy(buffer, packet + 4, frame < capacity ? frame : capacity);
      *length = frame;
      ++stats_.frames_received;
    } else {
      ++stats_.frames_dropped;
    }
    // Next frame: past this one (4-byte header + data), rounded up to a dword. CAPR
    // trails the read position by 16 bytes - a quirk of the chip.
    receive_offset_ = (receive_offset_ + size + 4 + 3) & ~3u;
    receive_offset_ %= kReceiveRingSize;
    write16(kCapr, static_cast<uint16_t>(receive_offset_ - 16));
    if (usable) return true;
    if (!(status & kRxStatusOk) || size <= 4) {  // the ring is corrupt: restart reception
      write8(kCmd, kCmdTransmitEnable);
      receive_offset_ = 0;
      write16(kCapr, static_cast<uint16_t>(0u - 16));
      write8(kCmd, kCmdReceiveEnable | kCmdTransmitEnable);
      return false;
    }
  }
  return false;
}

}  // namespace hardware
