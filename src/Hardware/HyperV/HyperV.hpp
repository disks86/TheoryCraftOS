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

namespace hardware {

// What Hyper-V gives a guest at the processor level (the "hypervisor top-level functional specification"): the hypervisor announces
// itself through CPUID 0x40000000, the guest registers its identity, enables a page of hypercall code, and sets up the synthetic
// interrupt controller (SynIC) through which the host posts messages. Everything here is per virtual processor and is meant for the
// boot core (virtual processor 0, the one VMBus talks to).
struct HyperVMessage {
  uint32_t type;
  uint8_t payload_size;
  uint8_t flags;       // bit 0: more messages are waiting behind this one
  uint16_t reserved;
  uint64_t sender;
  uint8_t payload[240];
};
static_assert(sizeof(HyperVMessage) == 256);

class HyperV {
 public:
  static constexpr uint8_t kSynicVector = 0x44;      // delivered (and auto-acknowledged) when a message lands; the handler does nothing
  static constexpr uint32_t kMessageSint = 2;        // the SINT VMBus messages arrive on
  static constexpr uint64_t kStatusSuccess = 0, kStatusInsufficientBuffers = 0x33;

  // True if the CPUID leaves say this is Hyper-V (or something presenting its interface) with the privileges VMBus needs.
  static bool detected();

  HyperV() = default;
  HyperV(const HyperV&) = delete;
  HyperV& operator=(const HyperV&) = delete;

  // Registers the guest, enables the hypercall page and the SynIC on the calling processor. Call on the boot core.
  bool initialize(allocators::IAllocator& allocator);
  void shutdown();
  bool ready() const { return hypercall_page_ != nullptr && synic_ready_; }

  uint32_t vp_index() const { return vp_index_; }
  uint32_t max_leaf() const { return max_leaf_; }
  uint32_t build() const { return build_; }

  // HvPostMessage: sends `size` (<= 240) bytes to a connection. Returns the hypercall status (0: delivered).
  uint64_t post_message(uint32_t connection, uint32_t message_type, const void* payload, uint32_t size);
  // HvSignalEvent: tells the host something happened on an event connection.
  uint64_t signal_event(uint32_t connection, uint16_t flag);
  // Takes the next message from the SINT's slot, if there is one (and lets the hypervisor deliver the next). Boot core only.
  bool take_message(HyperVMessage* out);

  // The host's wall clock and reference counter, for time synchronization (100 ns units); 0 if the host does not offer it.
  uint64_t reference_time() const;

 private:
  uint64_t call(uint64_t control, uint64_t input, uint64_t output);

  allocators::IAllocator* allocator_ = nullptr;
  uint8_t* hypercall_page_ = nullptr;
  uint8_t* message_page_ = nullptr;
  uint8_t* event_page_ = nullptr;
  uint8_t* post_buffer_ = nullptr;   // the HvPostMessage input (a page of its own: it must not cross one)
  bool synic_ready_ = false;
  bool referenced_time_ = false;
  uint32_t vp_index_ = 0, max_leaf_ = 0, build_ = 0;
};

}  // namespace hardware
