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

#include "Hardware/HyperV/VmBus.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// Channel message types.
constexpr uint32_t kOfferChannel = 1, kRescindOffer = 2, kRequestOffers = 3, kAllOffersDelivered = 4, kOpenChannel = 5, kOpenResult = 6,
                   kCloseChannel = 7, kGpadlHeader = 8, kGpadlCreated = 10, kGpadlTeardown = 11, kGpadlTornDown = 12, kRelIdReleased = 13,
                   kInitiateContact = 14, kVersionResponse = 15, kUnload = 16, kUnloadResponse = 17;
constexpr uint32_t kMessageTypeChannel = 1;   // HvMessageTypeChannel... as posted by a guest: 1
constexpr uint32_t kEventConnection = 2;      // the shared connection for channels without a dedicated interrupt
constexpr uint32_t kVersions[] = {(4u << 16) | 0, (3u << 16) | 0, (2u << 16) | 4, (1u << 16) | 1, (0u << 16) | 13};
constexpr uint64_t kPage = 4096;

uint32_t rd32(const uint8_t* p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
void wr32(uint8_t* p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
void wr16(uint8_t* p, uint16_t v) { __builtin_memcpy(p, &v, 2); }
void wr64(uint8_t* p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
}  // namespace

// --- The connection -------------------------------------------------------------------------------------------------

bool VmBus::post(const void* message, uint32_t size) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const uint64_t status = hv_.post_message(message_connection_, kMessageTypeChannel, message, size);
    if (status == HyperV::kStatusSuccess) return true;
    if (status != HyperV::kStatusInsufficientBuffers) return false;
    delay_us(1000);   // the host's queue is full: wait for it to drain
  }
  return false;
}

void VmBus::handle(const HyperVMessage& m) {
  if (m.payload_size < 8) return;
  const uint32_t type = rd32(m.payload);
  if (type == kOfferChannel && m.payload_size >= 196) {
    if (offer_count_ >= kMaxOffers) return;
    VmBusOffer& o = offers_[offer_count_];
    __builtin_memcpy(o.type, m.payload + 8, 16);
    __builtin_memcpy(o.instance, m.payload + 24, 16);
    o.relid = rd32(m.payload + 184);
    o.connection_id = rd32(m.payload + 192);
    uint16_t flags16;
    __builtin_memcpy(&flags16, m.payload + 190, 2);
    o.dedicated_interrupt = flags16 & 1;
    o.open = false;
    ++offer_count_;
  } else if (type == kAllOffersDelivered) {
    all_offers_delivered_ = true;
  } else if (type == kRescindOffer && m.payload_size >= 12) {
    const uint32_t relid = rd32(m.payload + 8);
    for (uint32_t i = 0; i < offer_count_; ++i)
      if (offers_[i].relid == relid) offers_[i].relid = 0xFFFFFFFFu;   // gone
  }
}

bool VmBus::wait_for(uint32_t type, HyperVMessage* out, uint32_t key, uint32_t key_offset) {
  const uint64_t start = uptime_us();
  while (uptime_us() - start < kTimeoutUs) {
    HyperVMessage m;
    if (!hv_.take_message(&m)) {
      delay_us(100);
      continue;
    }
    if (m.payload_size >= 8 && rd32(m.payload) == type && (key == 0xFFFFFFFFu || (m.payload_size >= key_offset + 4 && rd32(m.payload + key_offset) == key))) {
      *out = m;
      return true;
    }
    handle(m);
  }
  return false;
}

void VmBus::pump() {
  HyperVMessage m;
  while (hv_.take_message(&m)) handle(m);
}

bool VmBus::connect() {
  if (connected_ || !hv_.ready()) return connected_;
  interrupt_page_ = static_cast<uint8_t*>(allocator_.allocate_dma(kPage, kPage));
  monitor_pages_ = static_cast<uint8_t*>(allocator_.allocate_dma(2 * kPage, kPage));
  if (!interrupt_page_ || !monitor_pages_) {
    disconnect();
    return false;
  }
  __builtin_memset(interrupt_page_, 0, kPage);
  __builtin_memset(monitor_pages_, 0, 2 * kPage);

  pump();   // (anything stale in the slot)
  for (uint32_t version : kVersions) {
    uint8_t message[40] = {};
    wr32(message, kInitiateContact);
    wr32(message + 8, version);
    wr32(message + 12, hv_.vp_index());
    wr64(message + 16, reinterpret_cast<uint64_t>(interrupt_page_));
    wr64(message + 24, reinterpret_cast<uint64_t>(monitor_pages_));
    wr64(message + 32, reinterpret_cast<uint64_t>(monitor_pages_) + kPage);
    if (!post(message, sizeof message)) continue;
    HyperVMessage reply;
    if (!wait_for(kVersionResponse, &reply)) {
      // No answer at all: this host does not run VMBus for us. Asking again with another version would only wait again.
      disconnect();
      return false;
    }
    if (reply.payload[8]) {   // version_supported
      version_ = version;
      connected_ = true;
      break;
    }
  }
  if (!connected_) {
    disconnect();
    return false;
  }

  uint8_t request[8] = {};
  wr32(request, kRequestOffers);
  all_offers_delivered_ = false;
  offer_count_ = 0;
  HyperVMessage done;
  if (!post(request, sizeof request) || !wait_for(kAllOffersDelivered, &done)) {
    disconnect();
    return false;
  }
  all_offers_delivered_ = true;
  return true;
}

void VmBus::disconnect() {
  for (uint32_t i = 0; i < kMaxChannels; ++i)
    if (channel_used_[i]) close(&channels_[i]);
  if (connected_) {
    uint8_t message[8] = {};
    wr32(message, kUnload);
    HyperVMessage reply;
    if (post(message, sizeof message)) wait_for(kUnloadResponse, &reply);
  }
  connected_ = false;
  allocator_.deallocate(interrupt_page_);
  allocator_.deallocate(monitor_pages_);
  interrupt_page_ = monitor_pages_ = nullptr;
  offer_count_ = 0;
}

int VmBus::find(const uint8_t guid[16]) const {
  for (uint32_t i = 0; i < offer_count_; ++i)
    if (offers_[i].relid != 0xFFFFFFFFu && __builtin_memcmp(offers_[i].type, guid, 16) == 0) return static_cast<int>(i);
  return -1;
}

// --- Channels -------------------------------------------------------------------------------------------------------

VmBusChannel* VmBus::open(uint32_t offer_index) {
  if (!connected_ || offer_index >= offer_count_ || offers_[offer_index].relid == 0xFFFFFFFFu || offers_[offer_index].open) return nullptr;
  uint32_t slot = 0;
  while (slot < kMaxChannels && channel_used_[slot]) ++slot;
  if (slot == kMaxChannels) return nullptr;
  VmBusOffer& offer = offers_[offer_index];

  const uint32_t ring_pages = VmBusChannel::kRingPages, pages = 2 * ring_pages;
  uint8_t* memory = static_cast<uint8_t*>(allocator_.allocate_dma(pages * kPage, kPage));
  if (!memory) return nullptr;
  __builtin_memset(memory, 0, pages * kPage);

  // The ring memory is shared with the host as a GPADL (a list of guest pages).
  const uint32_t gpadl = next_gpadl_++;
  uint8_t message[240] = {};
  wr32(message, kGpadlHeader);
  wr32(message + 8, offer.relid);
  wr32(message + 12, gpadl);
  wr16(message + 16, static_cast<uint16_t>(8 + 8 * pages));   // the range: two words and a page number each
  wr16(message + 18, 1);
  wr32(message + 20, pages * kPage);
  wr32(message + 24, 0);
  for (uint32_t i = 0; i < pages; ++i) wr64(message + 28 + 8 * i, (reinterpret_cast<uint64_t>(memory) / kPage) + i);
  HyperVMessage reply;
  if (!post(message, 28 + 8 * pages) || !wait_for(kGpadlCreated, &reply, gpadl, 12) || rd32(reply.payload + 16) != 0) {
    allocator_.deallocate(memory);   // (if the host half made the GPADL it is left to leak rather than be reused)
    return nullptr;
  }

  uint8_t open_message[148] = {};
  wr32(open_message, kOpenChannel);
  wr32(open_message + 8, offer.relid);
  wr32(open_message + 12, offer.relid);   // the open id: any number we recognize in the reply
  wr32(open_message + 16, gpadl);
  wr32(open_message + 20, hv_.vp_index());
  wr32(open_message + 24, ring_pages);   // where the receiving ring starts
  if (!post(open_message, sizeof open_message) || !wait_for(kOpenResult, &reply, offer.relid, 8) || rd32(reply.payload + 16) != 0) {
    uint8_t teardown[16] = {};
    wr32(teardown, kGpadlTeardown);
    wr32(teardown + 8, offer.relid);
    wr32(teardown + 12, gpadl);
    if (post(teardown, sizeof teardown) && wait_for(kGpadlTornDown, &reply, gpadl, 8)) allocator_.deallocate(memory);
    return nullptr;
  }

  VmBusChannel& c = channels_[slot];
  channel_used_[slot] = true;
  offer.open = true;
  c.bus_ = this;
  c.offer_ = &offer;
  c.memory_ = memory;
  c.gpadl_ = gpadl;
  const uint32_t data_bytes = (ring_pages - 1) * kPage;
  c.out_ = {reinterpret_cast<volatile uint32_t*>(memory), memory + kPage, data_bytes};
  c.in_ = {reinterpret_cast<volatile uint32_t*>(memory + ring_pages * kPage), memory + (ring_pages + 1) * kPage, data_bytes};
  return &c;
}

void VmBus::close(VmBusChannel* channel) {
  if (!channel || channel->bus_ != this) return;
  uint32_t slot = static_cast<uint32_t>(channel - channels_);
  if (slot >= kMaxChannels || !channel_used_[slot]) return;
  const uint32_t relid = channel->offer_->relid;
  bool released = relid == 0xFFFFFFFFu;   // a withdrawn channel is already shut by the host
  if (!released) {
    uint8_t message[16] = {};
    wr32(message, kCloseChannel);
    wr32(message + 8, relid);
    post(message, 12);
    wr32(message, kGpadlTeardown);
    wr32(message + 8, relid);
    wr32(message + 12, channel->gpadl_);
    HyperVMessage reply;
    released = post(message, 16) && wait_for(kGpadlTornDown, &reply, channel->gpadl_, 8);
  }
  if (released) allocator_.deallocate(channel->memory_);   // not freed otherwise: the host may still write into it
  channel->offer_->open = false;
  channel->memory_ = nullptr;
  channel_used_[slot] = false;
}

void VmBusChannel::signal() {
  if (offer_->dedicated_interrupt) {
    bus_->hv_.signal_event(offer_->connection_id, 0);
  } else {
    uint8_t* send_half = bus_->interrupt_page_ + 2048;
    __atomic_fetch_or(&send_half[offer_->relid / 8], static_cast<uint8_t>(1u << (offer_->relid % 8)), __ATOMIC_SEQ_CST);
    bus_->hv_.signal_event(kEventConnection, 0);
  }
}

// Ring bytes are copied around the end of the buffer.
static void ring_write(uint8_t* data, uint32_t size, uint32_t* index, const void* source, uint32_t length) {
  const uint8_t* s = static_cast<const uint8_t*>(source);
  const uint32_t first = size - *index < length ? size - *index : length;
  __builtin_memcpy(data + *index, s, first);
  if (first < length) __builtin_memcpy(data, s + first, length - first);
  *index = (*index + length) % size;
}

static void ring_read(const uint8_t* data, uint32_t size, uint32_t index, void* target, uint32_t length) {
  uint8_t* t = static_cast<uint8_t*>(target);
  const uint32_t first = size - index < length ? size - index : length;
  __builtin_memcpy(t, data + index, first);
  if (first < length) __builtin_memcpy(t + first, data, length - first);
}

bool VmBusChannel::send(const void* data, uint32_t length, uint16_t type, uint64_t transaction, uint16_t flags) {
  if (!memory_) return false;
  const uint32_t len8 = (16 + length + 7) / 8;
  const uint32_t total = len8 * 8 + 8;   // the packet and the 8-byte trailer holding the write index before it
  uint32_t write = out_.header[0] % out_.size, read = out_.header[1] % out_.size;
  const uint32_t available = write >= read ? out_.size - (write - read) : read - write;
  if (available <= total) return false;

  uint8_t descriptor[16] = {};
  wr16(descriptor, type);
  wr16(descriptor + 2, 2);   // the data starts after two 8-byte units
  wr16(descriptor + 4, static_cast<uint16_t>(len8));
  wr16(descriptor + 6, flags);
  wr64(descriptor + 8, transaction);
  const uint32_t old_write = write;
  ring_write(out_.data, out_.size, &write, descriptor, 16);
  ring_write(out_.data, out_.size, &write, data, length);
  const uint8_t zero[8] = {};
  ring_write(out_.data, out_.size, &write, zero, len8 * 8 - 16 - length);
  uint8_t trailer[8];
  wr64(trailer, uint64_t{old_write} << 32);
  ring_write(out_.data, out_.size, &write, trailer, 8);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  out_.header[0] = write;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  signal();
  return true;
}

bool VmBusChannel::receive(void* buffer, uint32_t capacity, uint32_t* length, uint16_t* type, uint64_t* transaction) {
  if (!memory_) return false;
  const uint32_t read = in_.header[1] % in_.size;
  const uint32_t write = in_.header[0] % in_.size;
  if (read == write) return false;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint32_t filled = write >= read ? write - read : in_.size - (read - write);
  uint8_t descriptor[16];
  if (filled < 16) return false;
  ring_read(in_.data, in_.size, read, descriptor, 16);
  uint16_t offset8, len8;
  __builtin_memcpy(&offset8, descriptor + 2, 2);
  __builtin_memcpy(&len8, descriptor + 4, 2);
  const uint32_t packet = uint32_t{len8} * 8, start = uint32_t{offset8} * 8;
  if (packet < 16 || start < 16 || start > packet || packet + 8 > filled) {   // the host's word is not to be trusted: skip everything
    in_.header[1] = write;
    return false;
  }
  const uint32_t payload = packet - start;
  const uint32_t copied = payload < capacity ? payload : capacity;
  ring_read(in_.data, in_.size, (read + start) % in_.size, buffer, copied);
  __builtin_memcpy(type, descriptor, 2);
  __builtin_memcpy(transaction, descriptor + 8, 8);
  *length = copied;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  in_.header[1] = (read + packet + 8) % in_.size;
  if (in_.header[3]) signal();   // the host was waiting for room in this ring
  return true;
}

}  // namespace hardware
