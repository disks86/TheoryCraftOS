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
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Network/NetworkTypes.hpp"
#include "ObjectContainer/Array.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace network {

// The frames an interface had no room for yet, waiting to go out, shared fairly between the flows that sent them: the scheduler of RFC 8290
// (FQ-CoDel) without its CoDel part. Each flow (a hash of the destination address, the protocol and the two ports) has its own first-in
// first-out queue; the flows take turns by deficit round robin, a turn worth kQuantum bytes. A flow that had nothing queued (an ACK, a DNS
// query, the one packet of a keystroke) starts on the "new" list, served before the "old" one, so a sparse flow does not wait behind bulk
// ones. When the queue is full, the oldest frame of the flow with the most bytes queued is dropped - the flow that caused the backlog pays
// for it - and a frame that has waited kMaxSojournMs is dropped when it comes up (a frame the interface keeps refusing, or a backlog the
// sender ignores, never holds the queue up for longer than that). A dropped TCP segment is retransmitted by its sender.
//
// Not thread-safe: the network stack's mutex guards it.
class EgressQueue {
 public:
  enum class Kind : uint8_t { Plain, Checksum, Segmented };  // which INetworkInterface transmit call the frame needs
  static constexpr uint32_t kSlots = 128;                  // frames at most
  static constexpr uint64_t kMaxBytes = 1u << 20;          // bytes at most
  static constexpr uint32_t kFlows = 64;                   // flow queues (flows hash into them)
  static constexpr uint32_t kQuantum = 1514;               // bytes a flow may send per turn (one full Ethernet frame)
  static constexpr uint64_t kMaxSojournMs = 100;

  struct Stats {
    uint64_t queued = 0, sent = 0, dropped_full = 0, dropped_stale = 0;
  };

  explicit EgressQueue(allocators::IAllocator& allocator) : allocator_(allocator) {}
  EgressQueue(const EgressQueue&) = delete;
  EgressQueue& operator=(const EgressQueue&) = delete;

  bool empty() const { return frames_ == 0; }
  uint32_t frames() const { return frames_; }
  uint64_t bytes() const { return bytes_; }
  // Frames of flow `flow` (see flow_of_frame) waiting: a sender can hold back while its own are still queued.
  uint32_t queued_in_flow(uint32_t flow) const { return flows_[flow % kFlows].frames; }
  const Stats& stats() const { return stats_; }

  // Queues a copy of `frame`. False if it could not be (out of memory, or the frame itself was the one dropped to make room).
  bool enqueue(uint32_t flow, Kind kind, ConstByteView frame, hardware::TransmitChecksum checksum, uint16_t mss, uint64_t now_ms);
  // Gives frames to `nic` in their fair order until it refuses one (that one stays first in its flow) or none are left. Returns how many
  // went out.
  uint32_t drain(hardware::INetworkInterface& nic, uint64_t now_ms);
  // Forgets everything (the link went down).
  void clear();

  // The flow of an Ethernet frame: its destination address, protocol and ports (non-IP frames, ARP say, are one flow of their own).
  static uint32_t flow_of_frame(ConstByteView frame);
  // The same, for what a TCP or UDP socket is about to send (remote address and ports): equal to flow_of_frame of its frames.
  static uint32_t flow_of(const IpAddress& destination, uint8_t protocol, uint16_t source_port, uint16_t destination_port);

 private:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  struct Slot {
    Kind kind = Kind::Plain;
    hardware::TransmitChecksum checksum;
    uint16_t mss = 0;
    uint32_t length = 0;
    uint64_t queued_ms = 0;
    uint32_t next = kNone;     // the next frame of the same flow
  };
  struct Flow {
    uint32_t head = kNone, tail = kNone;
    uint32_t frames = 0;
    uint64_t bytes = 0;
    int64_t deficit = 0;
    uint8_t list = 0;          // 0 none, 1 new, 2 old
  };
  // A list of flow numbers (the new and the old flows), first in first out.
  struct FlowList {
    Array<uint32_t, kFlows> items;
    uint32_t start = 0, count = 0;
    void push(uint32_t flow) { items[(start + count++) % kFlows] = flow; }
    uint32_t front() { return items[start]; }
    void pop() { start = (start + 1) % kFlows; --count; }
  };

  uint32_t take_slot();
  void free_slot(uint32_t slot);
  void pop_head(uint32_t flow);
  bool drop_from_fattest();
  bool send(hardware::INetworkInterface& nic, uint32_t slot);

  allocators::IAllocator& allocator_;
  Array<Slot, kSlots> slots_;
  Array<UniquePointer<ByteBuffer>, kSlots> buffers_;   // each slot's bytes (kept for reuse; big ones are let go)
  Array<uint32_t, kSlots> free_;                       // free slot numbers
  uint32_t free_count_ = 0;
  bool initialized_ = false;
  Array<Flow, kFlows> flows_;
  FlowList new_flows_, old_flows_;
  uint32_t frames_ = 0;
  uint64_t bytes_ = 0;
  Stats stats_;
};

}  // namespace network
