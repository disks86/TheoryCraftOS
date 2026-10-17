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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/AudioDeviceBase.hpp"
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"

namespace hardware {

constexpr uint16_t kVirtioSoundDevice = 0x1059;  // 0x1040 + device type 25 (modern only)

// A virtio-sound device (QEMU's `virtio-sound-pci`). It has no ring to read a
// position from: audio goes out in "periods", each a buffer on the transmit queue
// that the device hands back when it has played it. Samples are gathered into the
// current period, which is submitted when it is full - or at once, if nothing else
// is playing, to keep the delay small. The virtio-sound specification has no volume
// control, so volume and mute are applied to the samples as they are copied in.
class VirtioSoundDevice final : public AudioDeviceBase {
 public:
  static constexpr uint32_t kPeriodBytes = 4096;  // about 21 ms
  static constexpr uint32_t kMaxPeriods = 8;

  VirtioSoundDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                    efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator) {}

  const char* name() const override { return "virtio sound"; }
  bool initialize() override;
  void shutdown() override;

  uint32_t write(ConstByteView pcm) override;
  uint32_t queued_bytes() override;
  uint32_t free_bytes() override;
  AudioStatistics statistics() override;

 private:
  static constexpr uint16_t kControlQueue = 0, kEventQueue = 1, kTransmitQueue = 2, kReceiveQueue = 3;

  bool control(uint32_t request_bytes, uint32_t response_bytes);
  bool simple_request(uint32_t code);
  bool choose_stream();
  ByteView slot_data(uint32_t slot) const;
  void submit_current();
  void reclaim();
  void apply_volume() override {}

  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  Virtqueue queues_[4];
  ByteView control_buffer_;   // request at the start, response from kResponseOffset
  ByteView slots_;            // kPeriods transmit buffers, each: header, data, status
  uint32_t periods_ = 0;
  uint32_t stream_id_ = 0;

  uint32_t current_slot_ = 0;       // the slot being filled, if filling_
  uint32_t current_bytes_ = 0;
  bool filling_ = false;
  uint32_t next_slot_ = 0;          // the slot to use after the current one
  uint32_t busy_mask_ = 0;          // slots the device holds
  uint32_t slot_bytes_[kMaxPeriods] = {};
  uint32_t in_flight_bytes_ = 0;
  bool started_ = false;
  AudioStatistics stats_;
};

}  // namespace hardware
