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

#include "Kernel/Delay.hpp"
#include "Hardware/NetworkInterface/Intel/E1000NetworkInterface.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"

namespace hardware {

namespace {
// Register offsets (Intel 8254x software developer's manual, section 13).
constexpr uint32_t kCtrl = 0x0000, kStatus = 0x0008, kImc = 0x00D8;
constexpr uint32_t kRctl = 0x0100, kTctl = 0x0400, kTipg = 0x0410;
constexpr uint32_t kRdbal = 0x2800, kRdbah = 0x2804, kRdlen = 0x2808, kRdh = 0x2810, kRdt = 0x2818;
constexpr uint32_t kTdbal = 0x3800, kTdbah = 0x3804, kTdlen = 0x3808, kTdh = 0x3810, kTdt = 0x3818;
constexpr uint32_t kMulticastTable = 0x5200, kRal0 = 0x5400, kRah0 = 0x5404;

constexpr uint32_t kCtrlReset = 1u << 26, kCtrlSetLinkUp = 1u << 6, kCtrlAutoSpeed = 1u << 5;
constexpr uint32_t kStatusLinkUp = 1u << 1;
constexpr uint32_t kRctlEnable = 1u << 1, kRctlMulticastPromiscuous = 1u << 4, kRctlBroadcast = 1u << 15, kRctlStripCrc = 1u << 26;  // 2 KiB buffers: BSIZE = 0
constexpr uint32_t kTctlEnable = 1u << 1, kTctlPadShortPackets = 1u << 3;
constexpr uint32_t kTctlCollisionThreshold = 0x0F << 4, kTctlCollisionDistance = 0x40 << 12;
constexpr uint32_t kTipgDefault = 0x0060200A;  // IPGT 10, IPGR1 8, IPGR2 6 (IEEE 802.3)
constexpr uint8_t kStatusDescriptorDone = 1u << 0;
constexpr uint8_t kCommandEndOfPacket = 1u << 0, kCommandInsertCrc = 1u << 1, kCommandReportStatus = 1u << 3;
constexpr uint32_t kPciCommand = 0x04;
}  // namespace

uint32_t E1000NetworkInterface::read(uint32_t reg) const {
  return *reinterpret_cast<volatile uint32_t*>(registers_ + reg);
}
void E1000NetworkInterface::write(uint32_t reg, uint32_t value) const {
  *reinterpret_cast<volatile uint32_t*>(registers_ + reg) = value;
}

bool E1000NetworkInterface::initialize() {
  if (!supports(device_.vendor_id, device_.device_id)) return false;
  registers_ = pci_.memory_bar(device_.address, 0);
  if (!registers_) return false;

  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));  // memory space + bus master

  // Reset, then keep interrupts off: everything here is polled.
  write(kImc, 0xFFFFFFFF);
  write(kCtrl, read(kCtrl) | kCtrlReset);
  for (int i = 0; i < 1000 && (read(kCtrl) & kCtrlReset); ++i) hardware::delay_us(1000);
  write(kImc, 0xFFFFFFFF);
  write(kCtrl, (read(kCtrl) | kCtrlSetLinkUp | kCtrlAutoSpeed) & ~kCtrlReset);

  // The MAC address was loaded from the EEPROM into receive address register 0.
  const uint32_t low = read(kRal0), high = read(kRah0);
  for (int i = 0; i < 4; ++i) mac_.bytes[i] = static_cast<uint8_t>(low >> (8 * i));
  mac_.bytes[4] = static_cast<uint8_t>(high);
  mac_.bytes[5] = static_cast<uint8_t>(high >> 8);
  write(kRah0, high | (1u << 31));  // address valid
  for (uint32_t i = 0; i < 128; ++i) write(kMulticastTable + 4 * i, 0);

  // Descriptor rings and the frame buffers (this family does 64-bit DMA).
  rx_ring_ = allocate_view<ReceiveDescriptor>(allocator_, kReceiveDescriptors, 4096);
  tx_ring_ = allocate_view<TransmitDescriptor>(allocator_, kTransmitDescriptors, 4096);
  buffers_ = allocate_view<uint8_t>(allocator_, uint64_t{kReceiveDescriptors + kTransmitDescriptors} * kBufferSize, 4096);
  if (rx_ring_.empty() || tx_ring_.empty() || buffers_.empty()) {
    shutdown();
    return false;
  }
  __builtin_memset(rx_ring_.data(), 0, rx_ring_.size_bytes());
  __builtin_memset(tx_ring_.data(), 0, tx_ring_.size_bytes());
  for (uint32_t i = 0; i < kReceiveDescriptors; ++i) rx_ring_[i].address = reinterpret_cast<uint64_t>(rx_buffer(i).data());
  for (uint32_t i = 0; i < kTransmitDescriptors; ++i) {
    tx_ring_[i].address = reinterpret_cast<uint64_t>(tx_buffer(i).data());
    tx_ring_[i].status = kStatusDescriptorDone;  // free: the "device is done with it" state
  }

  write(kRdbal, static_cast<uint32_t>(reinterpret_cast<uint64_t>(rx_ring_.data())));
  write(kRdbah, static_cast<uint32_t>(reinterpret_cast<uint64_t>(rx_ring_.data()) >> 32));
  write(kRdlen, kReceiveDescriptors * sizeof(ReceiveDescriptor));
  write(kRdh, 0);
  write(kRdt, kReceiveDescriptors - 1);  // all but one descriptor belong to the card
  rx_next_ = 0;

  write(kTdbal, static_cast<uint32_t>(reinterpret_cast<uint64_t>(tx_ring_.data())));
  write(kTdbah, static_cast<uint32_t>(reinterpret_cast<uint64_t>(tx_ring_.data()) >> 32));
  write(kTdlen, kTransmitDescriptors * sizeof(TransmitDescriptor));
  write(kTdh, 0);
  write(kTdt, 0);
  tx_next_ = 0;
  write(kTipg, kTipgDefault);

  receive_control_ = kRctlEnable | kRctlBroadcast | kRctlStripCrc;
  write(kRctl, receive_control_);
  write(kTctl, kTctlEnable | kTctlPadShortPackets | kTctlCollisionThreshold | kTctlCollisionDistance);
  started_ = true;
  return true;
}

void E1000NetworkInterface::shutdown() {
  started_ = false;
  if (registers_) {
    write(kRctl, 0);
    write(kTctl, 0);
    write(kCtrl, read(kCtrl) | kCtrlReset);  // the card stops using our memory
  }
  release_view(allocator_, rx_ring_);
  release_view(allocator_, tx_ring_);
  release_view(allocator_, buffers_);
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

bool E1000NetworkInterface::link_up() const { return started_ && (read(kStatus) & kStatusLinkUp); }

bool E1000NetworkInterface::transmit(ConstByteView frame) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || frame.empty() || frame.size() > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  volatile TransmitDescriptor& d = tx_ring_[tx_next_];
  if (!(d.status & kStatusDescriptorDone)) {  // the card has not finished with this one yet
    ++stats_.send_failures;
    return false;
  }
  const uint32_t padded = frame.size() < kEthernetMinFrame ? kEthernetMinFrame : static_cast<uint32_t>(frame.size());
  const ByteView buffer = tx_buffer(tx_next_).first(padded);
  buffer.fill(0);
  buffer.copy_from(frame);
  d.length = static_cast<uint16_t>(padded);
  d.status = 0;
  d.command = kCommandEndOfPacket | kCommandInsertCrc | kCommandReportStatus;
  __atomic_thread_fence(__ATOMIC_RELEASE);  // descriptor contents before the tail moves
  tx_next_ = (tx_next_ + 1) % kTransmitDescriptors;
  write(kTdt, tx_next_);
  ++stats_.frames_sent;
  return true;
}

bool E1000NetworkInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !length) return false;
  volatile ReceiveDescriptor& d = rx_ring_[rx_next_];
  if (!(d.status & kStatusDescriptorDone)) return false;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);

  const uint32_t frame = d.length;
  const bool usable = d.errors == 0 && frame > 0 && frame <= kBufferSize;
  if (usable) {
    buffer.copy_from(ConstByteView(rx_buffer(rx_next_)).first(frame).at_most(buffer.size()));  // as much as fits
    *length = frame;
    ++stats_.frames_received;
  } else {
    ++stats_.frames_dropped;
  }
  d.status = 0;  // give the descriptor back to the card
  write(kRdt, rx_next_);
  rx_next_ = (rx_next_ + 1) % kReceiveDescriptors;
  return usable ? true : receive(buffer, length);  // skip a bad frame, look at the next
}

}  // namespace hardware

namespace hardware {
bool E1000NetworkInterface::accept_all_multicast(bool enable) {
  if (!started_) return false;
  receive_control_ = enable ? (receive_control_ | kRctlMulticastPromiscuous) : (receive_control_ & ~kRctlMulticastPromiscuous);
  write(kRctl, receive_control_);
  return enable;
}
}  // namespace hardware
