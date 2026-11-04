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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIpc.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuPageTable.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint32_t kHdrStatus = 12;
constexpr uint32_t kMessageStatus = IntelNpuJsmMessage::kStatusOffset;
constexpr uint32_t kRegIoError = 0xFFFFFFFFu;  // what a register reads when the device is gone
}  // namespace

bool IntelNpuIpc::init(ByteView tx, uint32_t tx_vpu, ByteView rx, uint32_t rx_vpu) {
  if (tx.size() < kTxSlotSize || rx.size() < IntelNpuIpcHeader::kSize + IntelNpuJsmMessage::kSize) return false;
  if ((tx_vpu & 63) != 0 || (rx_vpu & 63) != 0) return false;
  if (tx.size() > UINT32_MAX - tx_vpu || rx.size() > UINT32_MAX - rx_vpu) return false;  // addresses are 32 bits
  tx_ = tx;
  rx_ = rx;
  tx_vpu_ = tx_vpu;
  rx_vpu_ = rx_vpu;
  const uint64_t slots = tx.size() / kTxSlotSize;
  tx_slots_ = static_cast<uint32_t>(slots < kMaxTxSlots ? slots : kMaxTxSlots);
  on_ = false;
  return reset();
}

bool IntelNpuIpc::reset() {
  if (on_ || tx_.empty() || rx_.empty()) return false;
  tx_.fill(0);
  rx_.fill(0);
  flush(tx_, 0, tx_.size());
  flush(rx_, 0, rx_.size());
  for (bool& used : tx_used_) used = false;
  for (IntelNpuIpcConsumer* c : consumers_) {
    if (!c) continue;
    c->tx_slot_ = -1;
    c->queue_head_ = c->queue_count_ = 0;
  }
  callback_head_ = callback_count_ = 0;
  rx_count_ = 0;
  return true;
}

void IntelNpuIpc::flush(ByteView memory, uint64_t offset, uint64_t size) {
  if (!snoop_ && offset <= memory.size() && size <= memory.size() - offset) intel_npu_flush_cache(ConstByteView(memory.subview(offset, size)));
}

void IntelNpuIpc::disable() {
  on_ = false;
  for (IntelNpuIpcConsumer* c : consumers_) {
    if (!c) continue;
    if (!c->callback) c->aborted_ = true;
    drop_queue(*c);
  }
  for (uint32_t i = 0; i < callback_count_; ++i) {
    const Callback& cb = callbacks_[(callback_head_ + i) % kMaxRxMessages];
    mark_free(cb.header_offset, cb.message_offset);
    --rx_count_;
  }
  callback_head_ = callback_count_ = 0;
}

void IntelNpuIpc::drop_queue(IntelNpuIpcConsumer& c) {
  while (c.queue_count_) {
    const IntelNpuIpcConsumer::Entry e = c.queue_[c.queue_head_];
    c.queue_head_ = (c.queue_head_ + 1) % IntelNpuIpcConsumer::kQueueDepth;
    --c.queue_count_;
    mark_free(e.header_offset, e.message_offset);
    --rx_count_;
  }
}

void IntelNpuIpc::release_slot(IntelNpuIpcConsumer& c) {
  if (c.tx_slot_ >= 0 && static_cast<uint32_t>(c.tx_slot_) < kMaxTxSlots) tx_used_[c.tx_slot_] = false;
  c.tx_slot_ = -1;
}

bool IntelNpuIpc::add_consumer(IntelNpuIpcConsumer& c, uint32_t channel, IntelNpuIpcConsumer::Callback callback, void* context) {
  if (c.added_) return false;
  for (IntelNpuIpcConsumer*& slot : consumers_) {
    if (slot) continue;
    c.channel = channel;
    c.callback = callback;
    c.callback_context = context;
    c.queue_head_ = c.queue_count_ = 0;
    c.tx_slot_ = -1;
    c.request_id_ = 0;
    c.aborted_ = false;
    c.added_ = true;
    slot = &c;
    return true;
  }
  return false;
}

void IntelNpuIpc::remove_consumer(IntelNpuIpcConsumer& c) {
  if (!c.added_) return;
  for (IntelNpuIpcConsumer*& slot : consumers_)
    if (slot == &c) slot = nullptr;
  drop_queue(c);
  // Messages already queued for its callback are discarded with it.
  uint32_t kept = 0;
  for (uint32_t i = 0; i < callback_count_; ++i) {
    const Callback cb = callbacks_[(callback_head_ + i) % kMaxRxMessages];
    if (cb.consumer == &c) {
      mark_free(cb.header_offset, cb.message_offset);
      --rx_count_;
    } else {
      callbacks_[(callback_head_ + kept) % kMaxRxMessages] = cb;
      ++kept;
    }
  }
  callback_count_ = kept;
  release_slot(c);
  c.added_ = false;
}

IntelNpuIpcStatus IntelNpuIpc::send(IntelNpuIpcConsumer& c, IntelNpuJsmMessage& request) {
  if (!on_) return IntelNpuIpcStatus::NotOn;
  if (!c.added_) return IntelNpuIpcStatus::Invalid;
  release_slot(c);  // a consumer holds one request at a time
  uint32_t slot = 0;
  while (slot < tx_slots_ && tx_used_[slot]) ++slot;
  if (slot >= tx_slots_) return IntelNpuIpcStatus::NoBuffer;

  const uint64_t base = uint64_t{slot} * kTxSlotSize;
  ByteView area = tx_.subview(base, kTxSlotSize);
  area.fill(0);  // the firmware reads the full message: unused payload bytes must be zero
  const uint32_t message_vpu = tx_vpu_ + static_cast<uint32_t>(base) + IntelNpuIpcHeader::kSize;
  request.set_request_id(++next_request_id_);
  ByteWriter hdr(area.subview(0, IntelNpuIpcHeader::kSize));
  hdr.le32(message_vpu);
  hdr.le32(IntelNpuJsmMessage::kSize);
  hdr.le16(static_cast<uint16_t>(c.channel));
  hdr.u8(0);  // source node: the host
  hdr.u8(1);  // destination node: the NPU
  hdr.u8(IntelNpuIpcHeader::kStatusAllocated);
  IntelNpuJsmMessage out = request;
  out.set_header32(IntelNpuJsmMessage::kStatusOffset, IntelNpuJsmMessage::kStatusAllocated);
  out.set_result(0);
  area.subview(IntelNpuIpcHeader::kSize, IntelNpuJsmMessage::kSize).copy_from(out.view());
  flush(tx_, base, kTxSlotSize);

  tx_used_[slot] = true;
  c.tx_slot_ = static_cast<int32_t>(slot);
  c.request_id_ = request.request_id();
  ip_.write32(tx_register(), tx_vpu_ + static_cast<uint32_t>(base));
  return IntelNpuIpcStatus::Ok;
}

bool IntelNpuIpc::rx_locate(uint64_t vpu_addr, uint64_t size, uint32_t* offset) const {
  if (vpu_addr < rx_vpu_) return false;
  const uint64_t off = vpu_addr - rx_vpu_;
  if (off > rx_.size() || size > rx_.size() - off) return false;
  *offset = static_cast<uint32_t>(off);
  return true;
}

IntelNpuIpcHeader IntelNpuIpc::read_header(uint32_t offset) {
  flush(rx_, offset, IntelNpuIpcHeader::kSize);  // the firmware wrote it behind the CPU caches
  ByteReader r(ConstByteView(rx_.subview(offset, IntelNpuIpcHeader::kSize)));
  IntelNpuIpcHeader h;
  h.data_addr = r.le32();
  h.data_size = r.le32();
  h.channel = r.le16();
  h.src_node = r.u8();
  h.dst_node = r.u8();
  h.status = r.u8();
  return h;
}

void IntelNpuIpc::mark_free(uint32_t header_offset, uint32_t message_offset) {
  rx_[header_offset + kHdrStatus] = IntelNpuIpcHeader::kStatusFree;
  flush(rx_, header_offset, IntelNpuIpcHeader::kSize);
  if (message_offset != kNoMessage) {
    ByteWriter status(rx_.subview(message_offset + kMessageStatus, 4));
    status.le32(IntelNpuJsmMessage::kStatusFree);
    flush(rx_, message_offset, IntelNpuJsmMessage::kSize);
  }
}

void IntelNpuIpc::irq_handler() {
  for (uint32_t taken = 0; taken < kMaxFifoDrain; ++taken) {
    const uint32_t fill = (ip_.read32(kRxFifoStatus) >> kFifoFillShift) & kFifoFillMask;
    if (fill == 0) return;
    const uint32_t vpu_addr = ip_.read32(kRxFifoAddress);
    if (vpu_addr == kRegIoError) return;
    uint32_t header_offset = 0;
    if (!rx_locate(vpu_addr, IntelNpuIpcHeader::kSize, &header_offset)) continue;   // outside our area: nothing of ours to free
    const IntelNpuIpcHeader hdr = read_header(header_offset);

    uint32_t message_offset = kNoMessage;
    bool have_message = false;
    IntelNpuJsmMessage message;
    if (hdr.channel != kBootChannel) {
      if (!rx_locate(hdr.data_addr, IntelNpuJsmMessage::kSize, &message_offset)) {
        mark_free(header_offset, kNoMessage);
        continue;
      }
      flush(rx_, message_offset, IntelNpuJsmMessage::kSize);
      message.view().copy_from(ConstByteView(rx_.subview(message_offset, IntelNpuJsmMessage::kSize)));
      have_message = true;
    }
    if (rx_count_ >= kMaxRxMessages) {
      ++dropped_;
      mark_free(header_offset, message_offset);
      continue;
    }
    IntelNpuIpcConsumer* target = nullptr;
    for (IntelNpuIpcConsumer* c : consumers_) {
      if (!c || c->channel != hdr.channel) continue;
      if (!have_message || message.request_id() == c->request_id_) { target = c; break; }
    }
    if (!target) {
      mark_free(header_offset, message_offset);
      continue;
    }
    if (target->callback) {
      if (callback_count_ >= kMaxRxMessages) { ++dropped_; mark_free(header_offset, message_offset); continue; }
      callbacks_[(callback_head_ + callback_count_) % kMaxRxMessages] = {target, header_offset, message_offset};
      ++callback_count_;
    } else {
      if (target->queue_count_ >= IntelNpuIpcConsumer::kQueueDepth) { ++dropped_; mark_free(header_offset, message_offset); continue; }
      target->queue_[(target->queue_head_ + target->queue_count_) % IntelNpuIpcConsumer::kQueueDepth] = {header_offset, message_offset};
      ++target->queue_count_;
    }
    ++rx_count_;
  }
}

void IntelNpuIpc::run_callbacks() {
  // Take the list first: a callback may add or remove consumers, which edits it.
  while (callback_count_) {
    const Callback cb = callbacks_[callback_head_];
    callback_head_ = (callback_head_ + 1) % kMaxRxMessages;
    --callback_count_;
    const IntelNpuIpcHeader hdr = read_header(cb.header_offset);
    IntelNpuJsmMessage message;
    bool have = false;
    if (cb.message_offset != kNoMessage) {
      flush(rx_, cb.message_offset, IntelNpuJsmMessage::kSize);
      message.view().copy_from(ConstByteView(rx_.subview(cb.message_offset, IntelNpuJsmMessage::kSize)));
      have = true;
    }
    mark_free(cb.header_offset, cb.message_offset);
    --rx_count_;
    if (cb.consumer->callback) cb.consumer->callback(cb.consumer->callback_context, hdr, have ? &message : nullptr);
  }
}

IntelNpuIpcStatus IntelNpuIpc::receive(IntelNpuIpcConsumer& c, IntelNpuIpcHeader* header, IntelNpuJsmMessage* message) {
  if (!c.added_) return IntelNpuIpcStatus::Invalid;
  if (c.callback) return IntelNpuIpcStatus::WrongMode;
  if (c.aborted_) return IntelNpuIpcStatus::Aborted;
  if (c.queue_count_ == 0) return IntelNpuIpcStatus::Empty;
  const IntelNpuIpcConsumer::Entry e = c.queue_[c.queue_head_];
  c.queue_head_ = (c.queue_head_ + 1) % IntelNpuIpcConsumer::kQueueDepth;
  --c.queue_count_;
  const IntelNpuIpcHeader hdr = read_header(e.header_offset);
  if (header) *header = hdr;
  IntelNpuIpcStatus status = IntelNpuIpcStatus::Ok;
  if (e.message_offset != kNoMessage) {
    flush(rx_, e.message_offset, IntelNpuJsmMessage::kSize);
    IntelNpuJsmMessage copy;
    // The firmware says how much it wrote; the rest of the message is zero, never stale data.
    const uint64_t size = hdr.data_size < IntelNpuJsmMessage::kSize ? hdr.data_size : IntelNpuJsmMessage::kSize;
    copy.view().first(size).copy_from(ConstByteView(rx_.subview(e.message_offset, size)));
    if (copy.result() != IntelNpuJsm::kStatusSuccess) status = IntelNpuIpcStatus::BadResult;
    if (message) *message = copy;
  }
  mark_free(e.header_offset, e.message_offset);
  --rx_count_;
  return status;
}

IntelNpuIpcStatus IntelNpuIpc::transact(IntelNpuJsmMessage request, IntelNpuJsmMessage* response, uint32_t tries) {
  const uint32_t expected = IntelNpuJsm::response_type(request.type());
  if (expected == 0 || !response) return IntelNpuIpcStatus::Invalid;
  IntelNpuIpcConsumer consumer;
  if (!add_consumer(consumer, IntelNpuJsm::channel_for(request.type()))) return IntelNpuIpcStatus::NoConsumerSlot;
  IntelNpuIpcStatus status = send(consumer, request);
  if (status == IntelNpuIpcStatus::Ok) {
    status = IntelNpuIpcStatus::Timeout;
    for (uint32_t i = 0; i < tries; ++i) {
      irq_handler();
      const IntelNpuIpcStatus got = receive(consumer, nullptr, response);
      if (got == IntelNpuIpcStatus::Empty) continue;
      status = got;
      break;
    }
    if ((status == IntelNpuIpcStatus::Ok || status == IntelNpuIpcStatus::BadResult) && response->type() != expected) status = IntelNpuIpcStatus::WrongResponse;
  }
  remove_consumer(consumer);
  return status;
}

IntelNpuIpcStatus IntelNpuIpc::wait_for_boot_message(uint32_t tries) {
  IntelNpuIpcConsumer consumer;
  if (!add_consumer(consumer, kBootChannel)) return IntelNpuIpcStatus::NoConsumerSlot;
  IntelNpuIpcStatus status = IntelNpuIpcStatus::Timeout;
  for (uint32_t i = 0; i < tries; ++i) {
    irq_handler();
    IntelNpuIpcHeader hdr;
    const IntelNpuIpcStatus got = receive(consumer, &hdr, nullptr);
    if (got == IntelNpuIpcStatus::Empty) continue;
    status = got == IntelNpuIpcStatus::Ok && hdr.data_addr != kBootMessageData ? IntelNpuIpcStatus::Invalid : got;
    break;
  }
  remove_consumer(consumer);
  return status;
}

}  // namespace hardware
