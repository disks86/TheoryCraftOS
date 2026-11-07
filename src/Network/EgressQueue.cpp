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

#include "Network/EgressQueue.hpp"
#include "Utility/ByteStream.hpp"

namespace network {

namespace {
constexpr uint64_t kKeepBufferBytes = 4096;   // a slot's buffer bigger than this (a segmentation-offload frame) is let go when the slot is freed

// FNV-1a over the flow's fields: cheap, and spreads the few flows a machine has across the queues well enough.
uint32_t mix(uint32_t hash, uint8_t byte) { return (hash ^ byte) * 16777619u; }
uint32_t mix16(uint32_t hash, uint16_t v) { return mix(mix(hash, static_cast<uint8_t>(v >> 8)), static_cast<uint8_t>(v)); }
}  // namespace

uint32_t EgressQueue::flow_of(const IpAddress& destination, uint8_t protocol, uint16_t source_port, uint16_t destination_port) {
  uint32_t h = 2166136261u;
  if (destination.v6)
    for (uint8_t b : destination.ipv6.bytes) h = mix(h, b);
  else
    for (uint8_t b : destination.ipv4.octets) h = mix(h, b);
  h = mix(h, protocol);
  h = mix16(h, source_port);
  h = mix16(h, destination_port);
  return h % (kFlows - 1) + 1;   // (flow 0 is everything that is not IP)
}

uint32_t EgressQueue::flow_of_frame(ConstByteView frame) {
  ByteReader in(frame);
  in.skip(12);
  const uint16_t ether_type = in.be16();
  if (!in.ok()) return 0;
  IpAddress destination;
  uint8_t protocol = 0;
  if (ether_type == kEtherTypeIpv4) {
    const uint8_t version_ihl = in.u8();
    in.skip(8);
    protocol = in.u8();
    in.skip(6);
    Ipv4Address d;
    for (uint8_t& b : d.octets) b = in.u8();
    if (!in.ok() || (version_ihl >> 4) != 4 || (version_ihl & 15) < 5) return 0;
    in.skip(uint64_t{version_ihl & 15u} * 4 - 20);   // (options)
    destination = IpAddress(d);
  } else if (ether_type == kEtherTypeIpv6) {
    in.skip(6);
    protocol = in.u8();   // (the next header: extension headers are rare enough in what this machine sends to leave them as their own flows)
    in.skip(1 + 16);
    Ipv6Address d;
    if (!in.read_into(ByteView(d.bytes))) return 0;
    destination = IpAddress(d);
  } else {
    return 0;
  }
  uint16_t source_port = 0, destination_port = 0;
  if (protocol == kProtocolTcp || protocol == kProtocolUdp) {
    source_port = in.be16();
    destination_port = in.be16();
    if (!in.ok()) source_port = destination_port = 0;
  }
  return flow_of(destination, protocol, source_port, destination_port);
}

uint32_t EgressQueue::take_slot() {
  if (free_count_ == 0) return kNone;
  return free_[--free_count_];
}

void EgressQueue::free_slot(uint32_t slot) {
  if (slots_[slot].length > kKeepBufferBytes) buffers_[slot].reset();
  slots_[slot] = Slot{};
  free_[free_count_++] = slot;
}

void EgressQueue::pop_head(uint32_t flow_number) {
  Flow& flow = flows_[flow_number];
  const uint32_t slot = flow.head;
  if (slot == kNone) return;
  const Slot& s = slots_[slot];
  flow.head = s.next;
  if (flow.head == kNone) flow.tail = kNone;
  --flow.frames;
  flow.bytes -= s.length;
  --frames_;
  bytes_ -= s.length;
  free_slot(slot);
}

bool EgressQueue::drop_from_fattest() {
  uint32_t fattest = kNone;
  uint64_t most = 0;
  for (uint32_t f = 0; f < kFlows; ++f)
    if (flows_[f].bytes > most) {
      most = flows_[f].bytes;
      fattest = f;
    }
  if (fattest == kNone) return false;
  pop_head(fattest);   // (an emptied flow stays on its list; drain() takes it off)
  ++stats_.dropped_full;
  return true;
}

bool EgressQueue::enqueue(uint32_t flow_number, Kind kind, ConstByteView frame, hardware::TransmitChecksum checksum, uint16_t mss, uint64_t now_ms) {
  if (frame.empty() || frame.size() > kMaxBytes) return false;
  flow_number %= kFlows;
  if (!initialized_) {
    for (uint32_t i = 0; i < kSlots; ++i) free_[i] = kSlots - 1 - i;
    free_count_ = kSlots;
    initialized_ = true;
  }
  // Room first: frames of the fattest flow go until this one fits (if this flow is the fattest, its own oldest go).
  while (free_count_ == 0 || bytes_ > kMaxBytes - frame.size())
    if (!drop_from_fattest()) return false;
  const uint32_t slot = take_slot();
  UniquePointer<ByteBuffer>& buffer = buffers_[slot];
  if (!buffer) buffer = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  if (!buffer) {
    free_[free_count_++] = slot;
    return false;
  }
  buffer->clear();
  if (!buffer->append(frame)) {
    free_slot(slot);
    return false;
  }
  Slot& s = slots_[slot];
  s.kind = kind;
  s.checksum = checksum;
  s.mss = mss;
  s.length = static_cast<uint32_t>(frame.size());
  s.queued_ms = now_ms;
  s.next = kNone;
  Flow& flow = flows_[flow_number];
  if (flow.tail == kNone) flow.head = slot;
  else slots_[flow.tail].next = slot;
  flow.tail = slot;
  ++flow.frames;
  flow.bytes += s.length;
  ++frames_;
  bytes_ += s.length;
  ++stats_.queued;
  if (flow.list == 0) {   // a flow with nothing queued before: it goes first
    flow.list = 1;
    flow.deficit = kQuantum;
    new_flows_.push(flow_number);
  }
  return true;
}

bool EgressQueue::send(hardware::INetworkInterface& nic, uint32_t slot) {
  const Slot& s = slots_[slot];
  const ConstByteView frame = buffers_[slot]->view();
  switch (s.kind) {
    case Kind::Plain: return nic.transmit(frame);
    case Kind::Checksum: return nic.transmit_frame(frame, s.checksum);
    case Kind::Segmented: return nic.transmit_segmented(frame, s.checksum, s.mss);
  }
  return false;
}

uint32_t EgressQueue::drain(hardware::INetworkInterface& nic, uint64_t now_ms) {
  uint32_t sent = 0;
  // (bounded: each step sends a frame, drops one, or moves a flow along a list; a full round of the lists gives every flow a quantum)
  for (uint32_t steps = 0; steps < 4 * (kSlots + kFlows) && (new_flows_.count || old_flows_.count); ++steps) {
    const bool from_new = new_flows_.count != 0;
    FlowList& list = from_new ? new_flows_ : old_flows_;
    const uint32_t flow_number = list.front();
    Flow& flow = flows_[flow_number];
    if (flow.head == kNone) {
      // Nothing left: a new flow that emptied moves to the old list once (so it cannot keep its priority by sending one frame at a time);
      // an old one leaves.
      list.pop();
      if (from_new) {
        flow.list = 2;
        old_flows_.push(flow_number);
      } else {
        flow.list = 0;
      }
      continue;
    }
    if (flow.deficit <= 0) {   // its turn is used up: another quantum, at the back of the old flows
      flow.deficit += kQuantum;
      list.pop();
      flow.list = 2;
      old_flows_.push(flow_number);
      continue;
    }
    const uint32_t slot = flow.head;
    const Slot s = slots_[slot];
    if (now_ms - s.queued_ms > kMaxSojournMs) {   // waited too long: drop it (its sender retransmits if it cares)
      pop_head(flow_number);
      ++stats_.dropped_stale;
      continue;
    }
    if (!send(nic, slot)) break;   // the interface is full: try again later, in the same order
    flow.deficit -= s.length;
    pop_head(flow_number);
    ++stats_.sent;
    ++sent;
  }
  return sent;
}

void EgressQueue::clear() {
  for (uint32_t f = 0; f < kFlows; ++f)
    while (flows_[f].head != kNone) pop_head(f);
  for (uint32_t f = 0; f < kFlows; ++f) flows_[f] = Flow{};
  new_flows_.start = new_flows_.count = 0;
  old_flows_.start = old_flows_.count = 0;
}

}  // namespace network
