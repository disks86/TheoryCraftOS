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
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuJsm.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The header the host and the NPU firmware put in front of every message they exchange (struct ivpu_ipc_hdr): where the JSM message is, how big, on which
// channel, between which nodes, and whether the buffer is in use.
struct IntelNpuIpcHeader {
  static constexpr uint32_t kSize = 64;  // the header is padded to the cache line
  static constexpr uint32_t kStatusFree = 0, kStatusAllocated = 1;
  uint32_t data_addr = 0;   // NPU address of the JSM message
  uint32_t data_size = 0;
  uint16_t channel = 0;
  uint8_t src_node = 0, dst_node = 0;
  uint8_t status = 0;
};

enum class IntelNpuIpcStatus : uint8_t {
  Ok,
  NotOn,          // send while the channel is off
  NoBuffer,       // no free transmit buffer
  NoConsumerSlot, // too many consumers
  Empty,          // nothing received (yet)
  Aborted,        // the channel was turned off under the consumer
  BadResult,      // a reply arrived, but its result code is not success (the message is still returned)
  WrongResponse,  // a reply of another type than expected
  WrongMode,      // receive on a callback consumer
  Invalid,        // bad arguments or a malformed boot message
  Timeout,        // no reply within the given number of tries
};

class IntelNpuIpc;

// One party listening on a channel (struct ivpu_ipc_consumer). The caller owns the object and keeps it alive while it is added. With a callback, messages are
// handed to it by run_callbacks(); without, they queue up for receive().
struct IntelNpuIpcConsumer {
  using Callback = void (*)(void* context, const IntelNpuIpcHeader& header, const IntelNpuJsmMessage* message);  // message is null on the boot channel
  static constexpr uint32_t kQueueDepth = 16;

  uint32_t channel = 0;
  Callback callback = nullptr;
  void* callback_context = nullptr;

 private:
  friend class IntelNpuIpc;
  struct Entry { uint32_t header_offset, message_offset; };  // into the receive memory; message_offset is kNoMessage on the boot channel
  Entry queue_[kQueueDepth] = {};
  uint32_t queue_head_ = 0, queue_count_ = 0;
  int32_t tx_slot_ = -1;
  uint32_t request_id_ = 0;
  bool aborted_ = false;
  bool added_ = false;
};

// The message channel to the NPU firmware (ivpu_ipc.c). Two areas of shared memory in the NPU's global address range: the host writes a request into a transmit
// slot (header and message together, 192 bytes) and puts the slot's NPU address in the transmit FIFO register; the firmware puts the address of a header it
// wrote in the receive area into the receive FIFO, which raises an interrupt. The host drains that FIFO (irq_handler), sorts the messages to consumers by channel and
// request id, and frees each message by clearing its status words when the consumer has copied it out.
//
// Not thread safe: the owner serializes irq_handler(), run_callbacks() and the consumer calls (ivpu uses cons_lock for the first two and ipc->lock for send).
// Nothing here sleeps or allocates, so irq_handler() is fine to call from the interrupt handler.
class IntelNpuIpc {
 public:
  static constexpr uint32_t kBootChannel = 0x3FF;
  static constexpr uint32_t kBootMessageData = 0x424F4F54;  // "BOOT": the data_addr of the ready message
  static constexpr uint32_t kTxSlotSize = IntelNpuIpcHeader::kSize + IntelNpuJsmMessage::kSize;
  static constexpr uint32_t kMaxTxSlots = 128;
  static constexpr uint32_t kMaxConsumers = 32;
  static constexpr uint32_t kMaxRxMessages = 128;     // messages held for consumers at once; more are dropped (and freed)
  static constexpr uint32_t kMaxFifoDrain = 256;      // entries taken from the FIFO in one irq_handler()
  static constexpr uint32_t kDefaultReplyTries = 1000000;
  // Registers (IP BAR). The transmit FIFO is in the CPU subsystem, whose address depends on the generation.
  static constexpr uint64_t kRxFifoAddress = 0x000200F4, kRxFifoStatus = 0x000200FC, kTx37xx = 0x060200F0, kTxLater = 0x010200F0;
  static constexpr uint32_t kFifoFillShift = 16, kFifoFillMask = 0xFF;
  static constexpr uint32_t kNoMessage = 0xFFFFFFFFu;

  // `snoop`: the NPU snoops the CPU caches, so the shared memory needs no flushing.
  IntelNpuIpc(INpuRegisters& ip, IntelNpuIp generation, bool snoop) : ip_(ip), generation_(generation), snoop_(snoop) {}

  // `tx` and `rx` are the host's views of the two areas (at least one slot / header and message), `*_vpu` their NPU addresses (64-byte aligned, below 4 GiB).
  bool init(ByteView tx, uint32_t tx_vpu, ByteView rx, uint32_t rx_vpu);
  // After a firmware reset: clears both areas. Only while the channel is off.
  bool reset();
  void enable() { on_ = true; }
  // Turns the channel off, aborts the consumers without a callback and throws away what they had not received.
  void disable();
  bool on() const { return on_; }

  bool add_consumer(IntelNpuIpcConsumer& consumer, uint32_t channel, IntelNpuIpcConsumer::Callback callback = nullptr, void* context = nullptr);
  void remove_consumer(IntelNpuIpcConsumer& consumer);

  // Writes `request` into a transmit slot, stamps it with a new request id (also stored in the consumer, so only the reply is matched) and hands it to the NPU.
  IntelNpuIpcStatus send(IntelNpuIpcConsumer& consumer, IntelNpuJsmMessage& request);
  // Takes the next message addressed to the consumer, if any. `header` and `message` may be null. A message that carries a non-success result is returned with
  // BadResult.
  IntelNpuIpcStatus receive(IntelNpuIpcConsumer& consumer, IntelNpuIpcHeader* header, IntelNpuJsmMessage* message);

  // The interrupt handler: takes everything out of the receive FIFO (which must be emptied for the next interrupt to come).
  void irq_handler();
  // Runs the callbacks of the messages irq_handler() queued for callback consumers (ivpu's threaded handler).
  void run_callbacks();

  // Send a request and wait for its reply by draining the FIFO between looks, for at most `tries` looks.
  IntelNpuIpcStatus transact(IntelNpuJsmMessage request, IntelNpuJsmMessage* response, uint32_t tries = kDefaultReplyTries);
  // Waits for the firmware's ready message (ivpu_wait_for_ready). Call before enable().
  IntelNpuIpcStatus wait_for_boot_message(uint32_t tries = kDefaultReplyTries);

  uint32_t pending_messages() const { return rx_count_; }
  uint32_t dropped_messages() const { return dropped_; }

 private:
  struct Callback { IntelNpuIpcConsumer* consumer; uint32_t header_offset, message_offset; };
  bool rx_locate(uint64_t vpu_addr, uint64_t size, uint32_t* offset) const;
  IntelNpuIpcHeader read_header(uint32_t offset);
  void mark_free(uint32_t header_offset, uint32_t message_offset);
  void flush(ByteView memory, uint64_t offset, uint64_t size);
  void drop_queue(IntelNpuIpcConsumer& consumer);
  void release_slot(IntelNpuIpcConsumer& consumer);
  uint64_t tx_register() const { return generation_ == IntelNpuIp::Ip37xx ? kTx37xx : kTxLater; }

  INpuRegisters& ip_;
  IntelNpuIp generation_;
  bool snoop_;
  bool on_ = false;
  ByteView tx_, rx_;
  uint32_t tx_vpu_ = 0, rx_vpu_ = 0;
  uint32_t tx_slots_ = 0;
  bool tx_used_[kMaxTxSlots] = {};
  IntelNpuIpcConsumer* consumers_[kMaxConsumers] = {};
  Callback callbacks_[kMaxRxMessages] = {};
  uint32_t callback_head_ = 0, callback_count_ = 0;
  uint32_t rx_count_ = 0, dropped_ = 0;
  uint32_t next_request_id_ = 0;
};

}  // namespace hardware
