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
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The VideoCore firmware's "property interface" (mailbox channel 8): the ARM cores hand the firmware a message in memory and it answers in place. It owns the
// clocks, the power domains, the temperature sensor and the memory the GPU and video engines use, so every driver of the Raspberry Pi's media hardware goes
// through it. The message format and tag numbers are those of the firmware's public mailbox documentation.

constexpr uint32_t kVcMailboxPropertyChannel = 8;

// Tags this OS uses.
namespace vc_tag {
constexpr uint32_t kFirmwareRevision = 0x00000001;
constexpr uint32_t kBoardModel = 0x00010001;
constexpr uint32_t kBoardRevision = 0x00010002;
constexpr uint32_t kBoardSerial = 0x00010004;
constexpr uint32_t kArmMemory = 0x00010005;
constexpr uint32_t kVcMemory = 0x00010006;
constexpr uint32_t kGetPowerState = 0x00020001;
constexpr uint32_t kSetPowerState = 0x00028001;
constexpr uint32_t kGetClockState = 0x00030001;
constexpr uint32_t kSetClockState = 0x00038001;
constexpr uint32_t kGetClockRate = 0x00030002;
constexpr uint32_t kSetClockRate = 0x00038002;
constexpr uint32_t kGetMaxClockRate = 0x00030004;
constexpr uint32_t kGetTemperature = 0x00030006;
constexpr uint32_t kGetMinClockRate = 0x00030007;
constexpr uint32_t kGetMaxTemperature = 0x0003000A;
constexpr uint32_t kAllocateMemory = 0x0003000C;
constexpr uint32_t kLockMemory = 0x0003000D;
constexpr uint32_t kUnlockMemory = 0x0003000E;
constexpr uint32_t kReleaseMemory = 0x0003000F;
constexpr uint32_t kGetEdidBlock = 0x00030020;
constexpr uint32_t kGetMeasuredClockRate = 0x00030047;
constexpr uint32_t kGetThrottled = 0x00030046;
}  // namespace vc_tag

// Firmware clock numbers.
namespace vc_clock {
constexpr uint32_t kEmmc = 1, kUart = 2, kArm = 3, kCore = 4, kV3d = 5, kH264 = 6, kIsp = 7, kSdram = 8, kPixel = 9, kPwm = 10, kHevc = 11, kEmmc2 = 12;
}

// Memory flags of kAllocateMemory (the firmware's memory flag bits).
namespace vc_memory {
constexpr uint32_t kDiscardable = 1u << 0, kNormal = 0, kDirect = 1u << 2, kCoherent = 2u << 2, kZero = 1u << 4, kNoInit = 1u << 5, kHintPermalock = 1u << 6;
}

// What a mailbox looks like from here: a word to send and a word to receive, per channel. MmioVcMailboxPort is the real one; a test hands in a pretend firmware.
class IVcMailboxPort {
 public:
  // Sends `value` (a 16-byte aligned bus address with the channel in its low four bits). False if the mailbox stayed full.
  virtual bool send(uint32_t channel, uint32_t value) = 0;
  // Waits for the answer on `channel` (answers for other channels are dropped). False if none came.
  virtual bool receive(uint32_t channel, uint32_t* value) = 0;

 protected:
  constexpr IVcMailboxPort() = default;
  ~IVcMailboxPort() = default;
};

// The mailbox registers at 0xFE00B880: mailbox 0 is read from (word 0x00, status 0x18), mailbox 1 written to (word 0x20, status 0x38). Polls up to `attempts` times.
class MmioVcMailboxPort final : public IVcMailboxPort {
 public:
  static constexpr uint64_t kRead = 0x00, kReadStatus = 0x18, kWrite = 0x20, kWriteStatus = 0x38;
  static constexpr uint32_t kStatusFull = 1u << 31, kStatusEmpty = 1u << 30;

  explicit MmioVcMailboxPort(const MmioRegion& registers, uint32_t attempts = 10000000) : registers_(registers), attempts_(attempts) {}
  bool send(uint32_t channel, uint32_t value) override;
  bool receive(uint32_t channel, uint32_t* value) override;

 private:
  MmioRegion registers_;
  uint32_t attempts_;
};

// Memory the firmware can reach for a message: 16-byte aligned words, the bus address the firmware sees them at, and the cache maintenance that makes the two
// views agree (the ARM side caches the buffer; the firmware reads and writes it uncached).
class IVcMessageMemory {
 public:
  virtual ArrayView<uint32_t> words() = 0;
  virtual uint32_t bus_address() const = 0;
  // Writes the words back to memory before the firmware reads them.
  virtual void clean() { }
  // Discards cached copies before the CPU reads what the firmware wrote.
  virtual void invalidate() { }

 protected:
  constexpr IVcMessageMemory() = default;
  ~IVcMessageMemory() = default;
};

// One tag of a request: its number and the value words, which hold the request going in and the firmware's answer coming back (the buffer is as long as the
// larger of the two, and the firmware says how much of it it used in `response_words`).
struct VcTag {
  uint32_t id = 0;
  ArrayView<uint32_t> values;
  uint32_t response_words = 0;
};

// Builds a message from tags in `storage`, with the layout checked, and reads the answer back. Free of hardware so it can be tested alone.
class VcPropertyMessage {
 public:
  static constexpr uint32_t kRequest = 0, kResponseOk = 0x80000000, kTagResponse = 0x80000000;

  explicit VcPropertyMessage(ArrayView<uint32_t> storage) : storage_(storage) {}
  // How many words a message of these tags needs (header, each tag's three-word header and values, the end tag).
  static bool words_needed(ArrayView<const VcTag> tags, uint64_t* words);
  // Lays out the request. False if the tags do not fit.
  bool build(ArrayView<const VcTag> tags);
  // Reads the answer: the overall code, then each tag's response bit, length and values (copied into the tags). False if the firmware refused or the answer is
  // malformed (a tag that does not match, a length that does not fit).
  bool parse(ArrayView<VcTag> tags);

 private:
  ArrayView<uint32_t> storage_;
};

// Sends messages through a port and a message buffer.
class VcMailbox {
 public:
  VcMailbox(IVcMailboxPort& port, IVcMessageMemory& memory) : port_(port), memory_(memory) {}
  // Runs the tags (values are filled in with the answers). False if the mailbox timed out, the answer was not for this message, or the firmware refused.
  bool transact(ArrayView<VcTag> tags);
  // One tag: `request` words in, up to `response.size()` words out; `*response_words` (optional) says how many came back.
  bool call(uint32_t tag, ArrayView<const uint32_t> request, ArrayView<uint32_t> response, uint32_t* response_words = nullptr);

 private:
  IVcMailboxPort& port_;
  IVcMessageMemory& memory_;
};

}  // namespace hardware
