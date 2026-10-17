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

#include "Hardware/NetworkInterface/Virtio/VirtioNetworkInterface.hpp"

namespace hardware {

namespace {
constexpr uint32_t kFeatureMac = 1u << 5;      // VIRTIO_NET_F_MAC: config space holds the MAC
constexpr uint32_t kFeatureStatus = 1u << 16;  // VIRTIO_NET_F_STATUS: config space reports link state
constexpr uint32_t kConfigMac = 0, kConfigStatus = 6;
constexpr uint16_t kStatusLinkUp = 1;
constexpr uint16_t kReceiveQueue = 0, kTransmitQueue = 1;
}  // namespace

bool VirtioNetworkInterface::initialize() {
  transport_.request_features(kFeatureMac | kFeatureStatus);
  if (!transport_.initialize()) {
    transport_.shutdown();
    return false;
  }
  const uint32_t accepted = transport_.accepted_features();
  has_status_ = accepted & kFeatureStatus;
  if (accepted & kFeatureMac)
    for (uint32_t i = 0; i < 6; ++i) mac_.bytes[i] = transport_.read_config8(kConfigMac + i);

  buffers_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{2 * kBuffers} * kBufferSize, 4096);
  if (buffers_.empty() || !transport_.setup_queue(kReceiveQueue, &rx_) || !transport_.setup_queue(kTransmitQueue, &tx_) ||
      rx_.size() < kBuffers || tx_.size() < kBuffers) {
    shutdown();
    return false;
  }
  for (uint16_t i = 0; i < kBuffers; ++i) rx_.post(i, rx_buffer(i).data(), kBufferSize);  // empty buffers for the device to fill
  tx_busy_ = 0;

  if (!transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  transport_.notify(kReceiveQueue);
  return true;
}

void VirtioNetworkInterface::shutdown() {
  started_ = false;
  transport_.shutdown();  // reset first: the device must stop using our memory
  rx_.release(allocator_);
  tx_.release(allocator_);
  release_view(allocator_, buffers_);
}

bool VirtioNetworkInterface::link_up() const {
  if (!started_) return false;
  if (!has_status_) return true;  // no status field: assume the link is up
  const uint16_t status = transport_.read_config8(kConfigStatus) | (transport_.read_config8(kConfigStatus + 1) << 8);
  return status & kStatusLinkUp;
}

// Takes back transmit buffers the device has finished with.
void VirtioNetworkInterface::reclaim_transmitted() {
  uint16_t id;
  uint32_t length;
  while (tx_.take_used(&id, &length))
    if (id < kBuffers) tx_busy_ &= ~(1u << id);  // the id comes from the device: ignore one that is not ours
}

bool VirtioNetworkInterface::transmit(ConstByteView frame) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || frame.empty() || frame.size() > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  reclaim_transmitted();
  uint16_t slot = 0;
  while (slot < kBuffers && (tx_busy_ & (1u << slot))) ++slot;
  if (slot == kBuffers) {  // every buffer is still with the device
    ++stats_.send_failures;
    return false;
  }

  const uint32_t padded = frame.size() < kEthernetMinFrame ? kEthernetMinFrame : static_cast<uint32_t>(frame.size());
  const ByteView buffer = tx_buffer(slot).first(kHeaderSize + padded);
  buffer.fill(0);  // zero header (no offloads) and padding
  buffer.subview(kHeaderSize).copy_from(frame);
  tx_busy_ |= 1u << slot;
  tx_.post(slot, buffer.data(), static_cast<uint32_t>(buffer.size()), /*device_writes=*/false);
  transport_.notify(kTransmitQueue);
  ++stats_.frames_sent;
  return true;
}

bool VirtioNetworkInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !length) return false;
  uint16_t id;
  uint32_t written;
  while (rx_.take_used(&id, &written)) {
    if (id >= kBuffers) continue;  // not one of our buffers: the device is confused, ignore the entry
    const bool usable = written > kHeaderSize && written <= kBufferSize;
    if (usable) {
      const uint32_t frame = written - kHeaderSize;
      buffer.copy_from(ConstByteView(rx_buffer(id)).subview(kHeaderSize, frame).at_most(buffer.size()));  // as much as fits
      *length = frame;
      ++stats_.frames_received;
    } else {
      ++stats_.frames_dropped;
    }
    rx_.post(id, rx_buffer(id).data(), kBufferSize);  // hand the buffer back
    transport_.notify(kReceiveQueue);
    if (usable) return true;
  }
  return false;
}

}  // namespace hardware
