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
#include "Utility/AllocatedView.hpp"
#include "Hardware/Bus/Usb/Xhci/XhciRegisters.hpp"

namespace hardware::xhci {

constexpr uint32_t kRingTrbs = 256;  // 4 KiB: never crosses a 64 KiB boundary

// A ring software writes and the controller reads (command and transfer
// rings). The last slot holds a Link TRB back to the start.
class ProducerRing {
 public:
  bool init(allocators::IAllocator& allocator);
  void release(allocators::IAllocator& allocator);

  uint64_t address() const { return reinterpret_cast<uint64_t>(trbs_.data()); }
  uint32_t cycle() const { return cycle_; }

  // Appends a TRB (the cycle bit is added) and returns its address.
  uint64_t push(uint64_t parameter, uint32_t status, uint32_t control);
  // Address the next TRB will be written at (after wrapping if the ring is at its
  // end), and the cycle state it will have: what Set TR Dequeue Pointer needs.
  uint64_t next_address();

 private:
  void wrap_if_needed();
  ArrayView<Trb> trbs_;  // kRingTrbs entries; every index below is checked against it
  uint32_t index_ = 0;
  uint32_t cycle_ = 1;
};

// The ring the controller writes events to and software consumes.
class EventRing {
 public:
  bool init(allocators::IAllocator& allocator);
  void release(allocators::IAllocator& allocator);

  uint64_t address() const { return reinterpret_cast<uint64_t>(trbs_.data()); }
  uint64_t segment_table_address() const { return reinterpret_cast<uint64_t>(table_.data()); }
  // Address of the next TRB to consume (the event ring dequeue pointer).
  uint64_t dequeue_address() const { return address() + uint64_t{index_} * sizeof(Trb); }

  bool pop(Trb* out);

 private:
  struct SegmentEntry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
  };

  ArrayView<Trb> trbs_;
  ArrayView<SegmentEntry> table_;  // one entry
  uint32_t index_ = 0;
  uint32_t cycle_ = 1;
};

}  // namespace hardware::xhci
