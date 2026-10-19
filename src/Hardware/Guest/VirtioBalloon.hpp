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
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

constexpr uint16_t kVirtioBalloonDevice = 0x1045;              // 0x1040 + device type 5
constexpr uint16_t kVirtioBalloonTransitionalDevice = 0x1002;

// A virtio-balloon device (QEMU's `virtio-balloon-pci`, driven by the monitor's `balloon` command): the host
// asks the guest to give memory back by setting a target number of 4 KiB pages. Memory is ballooned in chunks
// of kChunkPages interior pages: each chunk is one allocation with a kept page at both ends, so the allocator's
// bookkeeping (which sits next to the blocks) is never in a page the host has taken. service() moves the balloon
// towards the host's target - call it every so often. The balloon is capped (kMaxChunks); a host asking for more
// gets the cap, which actual() reports.
class VirtioBalloon final : public IHardware {
 public:
  static constexpr uint32_t kChunkPages = 64;   // 256 KiB
  static constexpr uint32_t kMaxChunks = 4096;  // 1 GiB
  static constexpr uint64_t kTimeoutUs = 500000;

  VirtioBalloon(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator) {}

  const char* name() const override { return "virtio balloon"; }
  bool initialize() override;
  void shutdown() override;

  // Moves towards the host's target; true if the balloon changed size.
  bool service();
  uint32_t target_pages() const;          // what the host wants
  uint32_t actual_pages() const { return chunks_ * kChunkPages; }  // what the guest has given

 private:
  bool exchange(uint16_t queue, uint32_t chunk);
  uint32_t read_config32(uint32_t offset) const;
  void write_actual() const;

  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  Virtqueue inflate_, deflate_;
  ArrayView<uint32_t> pfns_;        // one chunk's page frame numbers, device-visible
  ArrayView<uint8_t*> chunk_base_;  // the allocations, so they can be freed on deflate
  uint32_t chunks_ = 0;
  bool started_ = false;
};

}  // namespace hardware
