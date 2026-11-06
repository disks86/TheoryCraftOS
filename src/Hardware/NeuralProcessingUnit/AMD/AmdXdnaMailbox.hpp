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
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaHw.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// One direction of a mailbox channel: a ring buffer in device memory (SRAM) and its head and tail pointer registers. The writer moves the tail, the reader
// the head; both are byte offsets into the ring.
struct AmdXdnaRing {
  uint32_t start = 0;     // the ring's offset in the ring BAR
  uint32_t size = 0;      // bytes
  uint32_t head_reg = 0;  // offsets in the register BAR
  uint32_t tail_reg = 0;
};

struct AmdXdnaChannelConfig {
  uint8_t ring_bar = AmdXdnaRegister::kNoBar;  // where the rings are (the SRAM BAR)
  uint8_t reg_bar = AmdXdnaRegister::kNoBar;   // where the head/tail registers are (the mailbox BAR)
  AmdXdnaRing x2i;                             // host to device ("x" to "i"): requests
  AmdXdnaRing i2x;                             // device to host: responses
  bool has_irq_reg = false;                    // AIE2: the interrupt status register (i2x head register + 4), cleared before each drain
  uint32_t irq_reg = 0;
  uint32_t msix = 0;                           // the MSI-X vector the firmware raises (for whoever wires interrupts; this class polls)
};

// The amdxdna mailbox protocol (amdxdna_mailbox.c), host side, polled. A message is a 16-byte header (payload size, size and protocol version, id, opcode)
// followed by the payload, written whole into the ring; a message that does not fit before the end is preceded by a tombstone word at the old tail and goes
// to offset 0. Requests carry an id (slot | 0x1D000000) that the response repeats.
//
// Everything the device writes is untrusted: the head and tail registers, the sizes and ids in the headers. A pointer or size that cannot be right shuts
// the channel (broken(), as the driver's bad_state) and nothing outside the rings is ever read or written. Not thread safe: the owner serializes calls.
class AmdXdnaMailbox {
 public:
  static constexpr uint32_t kHeaderSize = 16;
  static constexpr uint32_t kTombstone = 0xDEADFACEu;
  static constexpr uint32_t kIdMagic = 0x1D000000u, kIdMagicMask = 0xFF000000u;
  static constexpr uint32_t kProtocolVersion = 1;
  static constexpr uint32_t kBodySizeMask = 0x7FF;   // MSG_BODY_SZ: 11 bits
  static constexpr uint32_t kSlots = 8;              // messages that may wait for an answer at once
  static constexpr uint32_t kMinRingSize = 64;
  static constexpr uint64_t kSendTimeoutUs = 2000000;  // waiting for room in the request ring
  static constexpr uint64_t kReplyTimeoutUs = 5000000; // RX_TIMEOUT

  AmdXdnaMailbox(const AmdXdnaBars& bars, const IClock* clock) : bars_(bars), clock_(clock) {}

  // Checks the geometry (rings inside the ring BAR, not overlapping, registers inside the register BAR, pointers in range) and takes the current tail and
  // head as the starting point (xdna_mailbox_start_channel). BadChannel if anything does not fit.
  AmdXdnaError start(const AmdXdnaChannelConfig& config);
  // Ends every outstanding message (they report ChannelBroken) and stops.
  void stop();
  bool running() const { return running_; }
  bool broken() const { return broken_; }
  const AmdXdnaChannelConfig& config() const { return config_; }

  // Sends `request` with `opcode` and waits for the answer, which must be exactly response.size() bytes. Both sizes are multiples of 4; the request is
  // at least 4 bytes, and with its header at most half the request ring. A wait that runs out shuts the channel (the driver destroys it), as the late answer could land in a reused slot.
  AmdXdnaError call(uint32_t opcode, ConstByteView request, ByteView response);

  // The two halves of call(), for asynchronous use and tests. send() queues the request and gives its id; poll() consumes what the device wrote and
  // delivers it; finish() reports and frees the slot once its answer is in (BadState while it is still waiting).
  AmdXdnaError send(uint32_t opcode, ConstByteView request, ByteView response, uint32_t* id);
  AmdXdnaError poll();
  bool answered(uint32_t id) const;
  AmdXdnaError finish(uint32_t id);
  // Messages the device answered with an id no request was waiting for (ignored, as the driver does).
  uint32_t stray_responses() const { return stray_; }

 private:
  struct Slot {
    bool busy = false;
    bool done = false;
    AmdXdnaError result = AmdXdnaError::None;
    ByteView response;
  };
  enum class Read : uint8_t { Empty, Consumed, Broken };
  Read read_one();
  AmdXdnaError write_message(const uint32_t (&header)[4], ConstByteView payload);
  bool ring_read(const AmdXdnaRing& ring, uint32_t offset, uint32_t* value) const;
  bool ring_write(const AmdXdnaRing& ring, uint32_t offset, uint32_t value) const;
  void shut();
  Slot* slot_for(uint32_t id);
  const Slot* slot_for(uint32_t id) const;

  const AmdXdnaBars& bars_;
  const IClock* clock_;
  AmdXdnaChannelConfig config_;
  Slot slots_[kSlots];
  uint32_t next_slot_ = 0;
  uint32_t x2i_tail_ = 0, i2x_head_ = 0;
  uint32_t stray_ = 0;
  bool running_ = false, broken_ = false;
};

}  // namespace hardware
