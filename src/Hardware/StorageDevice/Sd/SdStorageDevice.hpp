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

// The storage device of an SD card in a slot (SdCard over an ISdHost), and the source that makes one when a card is put in (SystemStorageDevices asks it). The device is present for
// as long as the card that was put in is in the slot; take it out and put it back and a new device is made.
#pragma once
#include "Hardware/StorageDevice/IRemovableMediaSource.hpp"
#include "Hardware/StorageDevice/Sd/SdCard.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

class SdStorageDevice final : public StorageDeviceBase {
 public:
  SdStorageDevice(ISdHost& host, allocators::IAllocator& allocator) : StorageDeviceBase(allocator, nullptr), host_(host), card_(host) {}

  const char* name() const override { return "SD card"; }
  // Identifies the card in the slot.
  bool initialize() override;
  void shutdown() override;
  bool present() const override { return !gone_ && host_.card_present() && host_.generation() == generation_; }

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) override;

 private:
  ISdHost& host_;
  SdCard card_;
  uint32_t generation_ = 0;
  bool gone_ = false;
};

class SdSlotSource final : public IRemovableMediaSource {
 public:
  static constexpr uint32_t kMaxAttempts = 3;
  SdSlotSource(ISdHost& host, allocators::IAllocator& allocator) : host_(host), allocator_(allocator) {}
  void discover(List<UniquePointer<IStorageDevice>>& found) override;

 private:
  ISdHost& host_;
  allocators::IAllocator& allocator_;
  uint32_t seen_generation_ = 0, attempts_ = 0;
  bool have_generation_ = false;
};

}  // namespace hardware
