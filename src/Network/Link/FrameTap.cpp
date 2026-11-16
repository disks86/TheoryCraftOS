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
#include "Network/Link/FrameTap.hpp"

namespace network {

void FrameTap::listen(uint16_t ether_type, bool all, bool wire, bool loopback, bool outgoing) {
  const uint32_t wanted = ether_type | (all ? kAll : 0) | (wire ? kWire : 0) | (loopback ? kLoopback : 0) | (outgoing ? kOutgoing : 0) |
                          ((all || ether_type != 0) && (wire || loopback) ? kListening : 0);
  __atomic_store_n(&wanted_, wanted, __ATOMIC_RELEASE);
}

void FrameTap::deliver(FrameKind kind, bool loopback, ConstByteView header, ConstByteView payload, uint32_t sequence) {
  const uint32_t wanted = __atomic_load_n(&wanted_, __ATOMIC_ACQUIRE);
  if (closed() || !(wanted & kListening) || !(wanted & (loopback ? kLoopback : kWire))) return;
  if (header.size() < hardware::kEthernetHeaderSize || payload.size() > kMaxFrame - header.size()) return;
  if (kind == FrameKind::Outgoing) {
    if (!(wanted & kAll) || !(wanted & kOutgoing)) return;
  } else if (!(wanted & kAll)) {
    const uint16_t type = static_cast<uint16_t>(header[12] << 8 | header[13]);
    if (type != (wanted & 0xFFFF)) return;
  }
  Frame f;      // (filled before the lock: only the enqueue's copy happens under it)
  f.info.kind = kind;
  f.info.loopback = loopback;
  f.info.sequence = sequence;
  f.info.length = static_cast<uint32_t>(header.size() + payload.size());
  ByteView(f.data).first(header.size()).copy_from(header);
  ByteView(f.data).subview(header.size(), payload.size()).copy_from(payload);
  kernel::ScopedMutex guard(queue_lock_);
  if (queue_.count() >= kQueueLimit) {
    __atomic_add_fetch(&dropped_, 1u, __ATOMIC_ACQ_REL);
    return;
  }
  queue_.enqueue(f);
}

bool FrameTap::receive(ByteView buffer, FrameInfo* info) {
  Frame f;
  {
    kernel::ScopedMutex guard(queue_lock_);
    if (!queue_.dequeue(&f)) return false;
  }
  buffer.copy_from(ConstByteView(f.data).first(f.info.length).at_most(buffer.size()));      // as much as fits
  if (info) *info = f.info;
  return true;
}

uint32_t FrameTap::pending() const {
  kernel::ScopedMutex guard(queue_lock_);
  return static_cast<uint32_t>(queue_.count());
}

bool FrameTap::send(ConstByteView frame) {
  if (closed() || frame.size() < hardware::kEthernetHeaderSize || frame.size() > kMaxFrame) return false;
  return link_.send_link_frame(frame, this);
}

bool FrameTap::send_looped(ConstByteView frame) {
  if (closed() || frame.size() <= hardware::kEthernetHeaderSize || frame.size() > kMaxFrame) return false;
  const uint16_t type = static_cast<uint16_t>(frame[12] << 8 | frame[13]);
  const ConstByteView packet = frame.dropped(hardware::kEthernetHeaderSize);
  if (!(type == kEtherTypeIpv4 && (packet[0] >> 4) == 4) && !(type == kEtherTypeIpv6 && (packet[0] >> 4) == 6)) return false;
  return link_.loop_ip_packet(packet);
}

void FrameTap::close() {
  __atomic_store_n(&closed_, true, __ATOMIC_RELEASE);
  kernel::ScopedMutex guard(queue_lock_);
  queue_.clear();
}

SharedPointer<FrameTap> FrameTapManager::open() {
  kernel::ScopedMutex guard(mutex_);
  SharedPointer<FrameTap> tap = SharedPointer<FrameTap>::make(allocator_, link_, allocator_);
  if (!tap || !taps_.push_back(tap)) return SharedPointer<FrameTap>();
  __atomic_store_n(&open_, static_cast<uint32_t>(taps_.count()), __ATOMIC_RELEASE);
  return tap;
}

void FrameTapManager::deliver(FrameKind kind, bool loopback, ConstByteView header, ConstByteView payload, const void* except) {
  kernel::ScopedMutex guard(mutex_);
  const uint32_t sequence = sequence_++;
  for (const SharedPointer<FrameTap>& tap : taps_)
    if (tap.get() != except) tap->deliver(kind, loopback, header, payload, sequence);
}

void FrameTapManager::received(ConstByteView frame) {
  if (!any() || frame.size() < hardware::kEthernetHeaderSize) return;
  const hardware::MacAddress to{{frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]}};
  const FrameKind kind = to == link_.link_address() ? FrameKind::Host : to.is_broadcast() ? FrameKind::Broadcast : (to.bytes[0] & 1) ? FrameKind::Multicast : FrameKind::OtherHost;
  deliver(kind, false, frame.first(hardware::kEthernetHeaderSize), frame.dropped(hardware::kEthernetHeaderSize), nullptr);
}

void FrameTapManager::sent(ConstByteView frame, const void* origin) {
  if (!any() || frame.size() < hardware::kEthernetHeaderSize) return;
  deliver(FrameKind::Outgoing, false, frame.first(hardware::kEthernetHeaderSize), frame.dropped(hardware::kEthernetHeaderSize), origin);
}

void FrameTapManager::looped(ConstByteView ip_packet) {
  if (!any() || ip_packet.empty()) return;
  uint8_t header[hardware::kEthernetHeaderSize] = {};      // (the loopback has no addresses: zeros, as Linux shows them)
  const uint16_t type = (ip_packet[0] >> 4) == 6 ? kEtherTypeIpv6 : kEtherTypeIpv4;
  header[12] = static_cast<uint8_t>(type >> 8);
  header[13] = static_cast<uint8_t>(type);
  deliver(FrameKind::Host, true, ConstByteView(header), ip_packet, nullptr);
}

void FrameTapManager::reap() {
  if (!any()) return;
  kernel::ScopedMutex guard(mutex_);
  for (uint64_t i = taps_.count(); i > 0; --i) {
    SharedPointer<FrameTap>& tap = taps_[i - 1];
    if (!tap->closed() && tap.use_count() > 1) continue;      // (held by somebody besides this list)
    taps_.remove_at(i - 1);
  }
  __atomic_store_n(&open_, static_cast<uint32_t>(taps_.count()), __ATOMIC_RELEASE);
}

}  // namespace network
