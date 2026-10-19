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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/HyperV/HyperV.hpp"

namespace hardware {

struct VmBusOffer {
  uint8_t type[16];       // the interface GUID (which kind of device), in the wire byte order
  uint8_t instance[16];
  uint32_t relid = 0;     // the channel's number
  uint32_t connection_id = 0;
  bool dedicated_interrupt = false;
  bool open = false;
};

// One open VMBus channel: a ring buffer for each direction, shared with the host. Used from one thread (the one that drives VmBus).
class VmBus;
class VmBusChannel {
 public:
  static constexpr uint16_t kPacketDataInband = 6, kPacketCompletion = 11;
  static constexpr uint32_t kRingPages = 5;   // per direction: a header page and four of data

  // Queues one packet for the host. False if the ring is full.
  bool send(const void* data, uint32_t length, uint16_t type, uint64_t transaction, uint16_t flags = 0);
  // Takes the oldest packet from the host: its payload (up to `capacity` bytes), type and transaction id. False if none is waiting.
  bool receive(void* buffer, uint32_t capacity, uint32_t* length, uint16_t* type, uint64_t* transaction);

  const VmBusOffer& offer() const { return *offer_; }

 private:
  friend class VmBus;
  struct Ring {
    volatile uint32_t* header = nullptr;   // write index, read index, interrupt mask, pending send size
    uint8_t* data = nullptr;
    uint32_t size = 0;
  };
  void signal();

  VmBus* bus_ = nullptr;
  VmBusOffer* offer_ = nullptr;
  uint8_t* memory_ = nullptr;
  uint32_t gpadl_ = 0;
  Ring out_, in_;
};

// The VMBus connection to the host: version negotiation, the list of devices the host offers, and opening channels to them. Messages go
// through the SynIC of the boot core, so every call must come from a thread on that core.
class VmBus {
 public:
  static constexpr uint32_t kMaxOffers = 24;
  static constexpr uint32_t kMaxChannels = 8;
  static constexpr uint64_t kTimeoutUs = 3000000;

  VmBus(HyperV& hv, allocators::IAllocator& allocator) : hv_(hv), allocator_(allocator) {}
  ~VmBus() { disconnect(); }
  VmBus(const VmBus&) = delete;
  VmBus& operator=(const VmBus&) = delete;

  // Negotiates a protocol version with the host and collects its offers. False if the host does not answer (this is no Hyper-V host
  // that has VMBus enabled).
  bool connect();
  void disconnect();
  bool connected() const { return connected_; }
  uint32_t version() const { return version_; }

  uint32_t offer_count() const { return offer_count_; }
  const VmBusOffer& offer(uint32_t index) const { return offers_[index]; }
  // Index of the first offer with this interface GUID (wire byte order), or -1.
  int find(const uint8_t guid[16]) const;

  // Opens a channel to an offer. Null if the host refuses.
  VmBusChannel* open(uint32_t offer_index);
  void close(VmBusChannel* channel);

  // Handles whatever messages the host has posted (new or withdrawn offers). Call regularly.
  void pump();

 private:
  friend class VmBusChannel;
  bool post(const void* message, uint32_t size);
  // Waits for a channel message of this type; others met on the way are handled. `match` is checked on the first words after the header.
  bool wait_for(uint32_t type, HyperVMessage* out, uint32_t key = 0xFFFFFFFFu, uint32_t key_offset = 0);
  void handle(const HyperVMessage& message);
  void release_channel(VmBusChannel* channel);

  HyperV& hv_;
  allocators::IAllocator& allocator_;
  bool connected_ = false;
  uint32_t version_ = 0;
  uint32_t message_connection_ = 1;
  uint8_t* interrupt_page_ = nullptr;   // the first half is the host's, the second half is ours (one bit per channel)
  uint8_t* monitor_pages_ = nullptr;    // two pages, only handed to the host
  VmBusOffer offers_[kMaxOffers];
  uint32_t offer_count_ = 0;
  bool all_offers_delivered_ = false;
  VmBusChannel channels_[kMaxChannels];
  bool channel_used_[kMaxChannels] = {};
  uint32_t next_gpadl_ = 1;
};

}  // namespace hardware
