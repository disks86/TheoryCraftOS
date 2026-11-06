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

#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaMailbox.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
bool aligned4(uint64_t v) { return (v & 3) == 0; }

// [a, a + as) and [b, b + bs) share a byte.
bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) { return a < b + bs && b < a + as; }
}  // namespace

bool AmdXdnaMailbox::ring_read(const AmdXdnaRing& ring, uint32_t offset, uint32_t* value) const {
  if (offset > ring.size || ring.size - offset < 4) return false;
  return bars_.read32(config_.ring_bar, uint64_t{ring.start} + offset, value);
}

bool AmdXdnaMailbox::ring_write(const AmdXdnaRing& ring, uint32_t offset, uint32_t value) const {
  if (offset > ring.size || ring.size - offset < 4) return false;
  return bars_.write32(config_.ring_bar, uint64_t{ring.start} + offset, value);
}

AmdXdnaError AmdXdnaMailbox::start(const AmdXdnaChannelConfig& config) {
  if (running_) return AmdXdnaError::BadState;
  for (Slot& s : slots_) s = Slot{};
  next_slot_ = 0;
  stray_ = 0;
  broken_ = false;
  // The driver trusts these (they come from signed firmware); here they must at least lie inside what is mapped, or the channel is not used.
  const AmdXdnaRing* rings[2] = {&config.x2i, &config.i2x};
  for (const AmdXdnaRing* r : rings) {
    if (r->size < kMinRingSize || !aligned4(r->size) || !aligned4(r->start) || !bars_.contains(config.ring_bar, r->start, r->size))
      return AmdXdnaError::BadChannel;
    if (!aligned4(r->head_reg) || !aligned4(r->tail_reg) || !bars_.contains(config.reg_bar, r->head_reg, 4) || !bars_.contains(config.reg_bar, r->tail_reg, 4))
      return AmdXdnaError::BadChannel;
  }
  if (overlaps(config.x2i.start, config.x2i.size, config.i2x.start, config.i2x.size)) return AmdXdnaError::BadChannel;
  if (config.has_irq_reg && (!aligned4(config.irq_reg) || !bars_.contains(config.reg_bar, config.irq_reg, 4))) return AmdXdnaError::BadChannel;
  uint32_t tail = 0, head = 0;
  if (!bars_.read32(config.reg_bar, config.x2i.tail_reg, &tail) || !bars_.read32(config.reg_bar, config.i2x.head_reg, &head)) return AmdXdnaError::BadChannel;
  if (!aligned4(tail) || tail > config.x2i.size - 4 || !aligned4(head) || head > config.i2x.size) return AmdXdnaError::BadChannel;
  config_ = config;
  x2i_tail_ = tail;
  i2x_head_ = head;
  running_ = true;
  if (config_.has_irq_reg) bars_.write32(config_.reg_bar, config_.irq_reg, 0);
  return AmdXdnaError::None;
}

void AmdXdnaMailbox::shut() {
  broken_ = true;
  for (Slot& s : slots_)
    if (s.busy && !s.done) {
      s.done = true;
      s.result = AmdXdnaError::ChannelBroken;
    }
}

void AmdXdnaMailbox::stop() {
  if (!running_) return;
  shut();
  for (Slot& s : slots_) s = Slot{};
  running_ = false;
}

AmdXdnaMailbox::Slot* AmdXdnaMailbox::slot_for(uint32_t id) {
  if ((id & kIdMagicMask) != kIdMagic) return nullptr;
  const uint32_t index = id & ~kIdMagicMask;
  return index < kSlots ? &slots_[index] : nullptr;
}

const AmdXdnaMailbox::Slot* AmdXdnaMailbox::slot_for(uint32_t id) const {
  if ((id & kIdMagicMask) != kIdMagic) return nullptr;
  const uint32_t index = id & ~kIdMagicMask;
  return index < kSlots ? &slots_[index] : nullptr;
}

// mailbox_send_msg(): the request goes at the tail if it fits before the end (less the word a tombstone needs); otherwise a tombstone marks the tail and
// the request goes to offset 0. Either way it must not reach the device's head, or the ring would look empty (the driver allows touching it after a wrap;
// this does not).
AmdXdnaError AmdXdnaMailbox::write_message(const uint32_t (&header)[4], ConstByteView payload) {
  const AmdXdnaRing& ring = config_.x2i;
  const uint32_t capacity = ring.size - 4;
  const uint32_t length = kHeaderSize + static_cast<uint32_t>(payload.size());
  AmdXdnaDeadline deadline(clock_, kSendTimeoutUs);
  uint32_t tail = x2i_tail_;
  while (true) {
    uint32_t head = 0;
    if (!bars_.read32(config_.reg_bar, ring.head_reg, &head)) {
      shut();
      return AmdXdnaError::ChannelBroken;
    }
    if (!aligned4(head) || head > capacity) {  // the device's head can only be where a message ends, at most the capacity
      shut();
      return AmdXdnaError::ChannelBroken;
    }
    bool fits = false;
    if (tail >= head) {
      if (length <= capacity - tail) {
        fits = true;
      } else if (length < head) {  // wrap: the tombstone tells the device to continue at 0
        if (!ring_write(ring, tail, kTombstone)) {
          shut();
          return AmdXdnaError::ChannelBroken;
        }
        tail = 0;
        fits = true;
      }
    } else if (length < head - tail) {
      fits = true;
    }
    if (fits) break;
    if (deadline.expired()) return AmdXdnaError::Timeout;  // nothing written
  }
  uint32_t offset = tail;
  for (uint32_t word : header) {
    ring_write(ring, offset, word);
    offset += 4;
  }
  ByteReader reader(payload);
  while (!reader.at_end()) {
    ring_write(ring, offset, reader.le32());
    offset += 4;
  }
  x2i_tail_ = tail + length;
  bars_.write32(config_.reg_bar, ring.tail_reg, x2i_tail_);
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMailbox::send(uint32_t opcode, ConstByteView request, ByteView response, uint32_t* id) {
  if (!running_) return AmdXdnaError::BadState;
  if (broken_) return AmdXdnaError::ChannelBroken;
  if (request.size() < 4 || !aligned4(request.size()) || !aligned4(response.size()) || request.size() > kBodySizeMask) return AmdXdnaError::BadMessage;
  // At most half the ring: then a message that does not fit before the end of an empty ring always fits before the device's head after the wrap. A
  // bigger one could wait forever (the driver allows up to the whole ring and then times out in that case).
  if (kHeaderSize + request.size() > (config_.x2i.size - 4) / 2) return AmdXdnaError::BadMessage;
  ByteReader first(request);
  if (first.le32() == kTombstone) return AmdXdnaError::BadMessage;  // the device would take it for a wrap marker
  Slot* slot = nullptr;
  uint32_t index = 0;
  for (uint32_t i = 0; i < kSlots && !slot; ++i) {
    index = (next_slot_ + i) % kSlots;
    if (!slots_[index].busy) slot = &slots_[index];
  }
  if (!slot) return AmdXdnaError::NoSlot;
  next_slot_ = (index + 1) % kSlots;
  const uint32_t message_id = index | kIdMagic;
  const uint32_t size = static_cast<uint32_t>(request.size());
  const uint32_t header[4] = {size, (size & kBodySizeMask) | (kProtocolVersion << 16), message_id, opcode};
  *slot = Slot{};
  slot->busy = true;
  slot->response = response;
  const AmdXdnaError e = write_message(header, request);
  if (e != AmdXdnaError::None) {
    *slot = Slot{};
    return e;
  }
  *id = message_id;
  return AmdXdnaError::None;
}

// mailbox_get_msg(): one message (or tombstone) from the response ring.
AmdXdnaMailbox::Read AmdXdnaMailbox::read_one() {
  const AmdXdnaRing& ring = config_.i2x;
  uint32_t tail = 0;
  if (!bars_.read32(config_.reg_bar, ring.tail_reg, &tail) || tail > ring.size || !aligned4(tail)) return Read::Broken;
  uint32_t head = i2x_head_;
  if (head == tail) return Read::Empty;
  if (head == ring.size) {  // the last message ended at the very end
    head = 0;
    i2x_head_ = 0;
    bars_.write32(config_.reg_bar, ring.head_reg, 0);
    if (head == tail) return Read::Empty;
  }
  uint32_t total = 0;
  if (!ring_read(ring, head, &total)) return Read::Broken;
  if (total == kTombstone) {
    if (head < tail) return Read::Broken;  // a wrap marker with unread messages after it
    i2x_head_ = 0;
    bars_.write32(config_.reg_bar, ring.head_reg, 0);
    return Read::Consumed;
  }
  if (total == 0 || !aligned4(total)) return Read::Broken;
  const uint64_t length = uint64_t{kHeaderSize} + total;
  const uint64_t room = ring.size - head;
  const uint64_t available = tail > head ? tail - head : room;
  if (length > room || length > available) return Read::Broken;
  uint32_t sz_ver = 0, id = 0, opcode = 0;
  if (!ring_read(ring, head + 4, &sz_ver) || !ring_read(ring, head + 8, &id) || !ring_read(ring, head + 12, &opcode)) return Read::Broken;
  if ((id & kIdMagicMask) != kIdMagic || (id & ~kIdMagicMask) >= kSlots) return Read::Broken;  // mailbox_get_resp: a bad id is a broken device
  Slot* slot = slot_for(id);
  if (!slot->busy || slot->done) {
    ++stray_;
  } else if (total != slot->response.size()) {
    slot->done = true;  // xdna_msg_cb: the answer must be exactly the size expected
    slot->result = AmdXdnaError::BadResponse;
  } else {
    ByteWriter out(slot->response);
    for (uint32_t offset = 0; offset < total; offset += 4) {
      uint32_t word = 0;
      if (!ring_read(ring, head + kHeaderSize + offset, &word)) return Read::Broken;
      out.le32(word);
    }
    slot->done = true;
    slot->result = AmdXdnaError::None;
  }
  i2x_head_ = static_cast<uint32_t>(head + length);  // may equal the ring size: the next read starts at 0
  bars_.write32(config_.reg_bar, ring.head_reg, i2x_head_);
  return Read::Consumed;
}

AmdXdnaError AmdXdnaMailbox::poll() {
  if (!running_) return AmdXdnaError::BadState;
  if (broken_) return AmdXdnaError::ChannelBroken;
  if (config_.has_irq_reg) bars_.write32(config_.reg_bar, config_.irq_reg, 0);
  // Every consumed message moves the head forward by at least a header, so a ring's worth of reads drains it; more means the device keeps writing.
  const uint32_t limit = config_.i2x.size / kHeaderSize + 2;
  for (uint32_t i = 0; i < limit; ++i) {
    switch (read_one()) {
      case Read::Empty: return AmdXdnaError::None;
      case Read::Consumed: break;
      case Read::Broken: shut(); return AmdXdnaError::ChannelBroken;
    }
  }
  return AmdXdnaError::None;
}

bool AmdXdnaMailbox::answered(uint32_t id) const {
  const Slot* slot = slot_for(id);
  return slot && slot->busy && slot->done;
}

AmdXdnaError AmdXdnaMailbox::finish(uint32_t id) {
  Slot* slot = slot_for(id);
  if (!slot || !slot->busy || !slot->done) return AmdXdnaError::BadState;
  const AmdXdnaError result = slot->result;
  *slot = Slot{};
  return result;
}

AmdXdnaError AmdXdnaMailbox::call(uint32_t opcode, ConstByteView request, ByteView response) {
  uint32_t id = 0;
  AmdXdnaError e = send(opcode, request, response, &id);
  if (e != AmdXdnaError::None) return e;
  AmdXdnaDeadline deadline(clock_, kReplyTimeoutUs);
  while (true) {
    poll();
    if (answered(id)) return finish(id);
    if (deadline.expired()) {
      shut();
      Slot* slot = slot_for(id);
      *slot = Slot{};
      return AmdXdnaError::Timeout;
    }
  }
}

}  // namespace hardware
