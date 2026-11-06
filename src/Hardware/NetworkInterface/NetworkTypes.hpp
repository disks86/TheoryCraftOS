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
#include "Utility/ByteStream.hpp"

namespace hardware {

struct MacAddress {
  uint8_t bytes[6];

  bool operator==(const MacAddress& other) const {
    for (int i = 0; i < 6; ++i)
      if (bytes[i] != other.bytes[i]) return false;
    return true;
  }
  bool is_broadcast() const { return *this == MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}; }

  // Reads six bytes from a stream (all zero and the stream in error if short).
  static MacAddress read(ByteReader& in) {
    MacAddress mac{};
    in.read_into(ByteView(mac.bytes));
    return mac;
  }
  void write(ByteWriter& out) const { out.bytes(ConstByteView(bytes)); }
};

// Ethernet frame sizes, without the 4-byte checksum (CRC) the hardware adds.
constexpr uint32_t kEthernetHeaderSize = 14;
constexpr uint32_t kEthernetMinFrame = 60;    // shorter frames are padded up to this
constexpr uint32_t kEthernetMaxFrame = 1514;  // 14-byte header + 1500 bytes of payload

// What the hardware found when it checked a received frame's checksums. `true` only means checked and correct: a frame the hardware did not
// check, or found wrong, says false and is checked again in software (which drops it if it is wrong).
struct ReceivedFrame {
  uint32_t length = 0;               // the whole frame, as receive() reports it
  bool ipv4_header_checksum_good = false;
  bool transport_checksum_good = false;  // TCP or UDP over IPv4
};

// A transport checksum the interface is asked to finish ("partial checksum", like Linux's CHECKSUM_PARTIAL): the 16-bit field at `field` holds
// the pseudo-header sum (folded, not complemented); the ones'-complement sum of the frame from `start` to its end is complemented and stored
// there. field == 0: nothing to do, the frame is complete.
struct TransmitChecksum {
  uint16_t start = 0;  // where the TCP or UDP header begins, from the start of the frame
  uint16_t field = 0;  // where its checksum is
  bool requested() const { return field != 0; }
  // TCP keeps its checksum 16 bytes into the header, UDP 6 bytes in.
  bool tcp() const { return field == start + 16; }
  bool udp() const { return field == start + 6; }
  // The field and the range it covers lie inside a frame of `size` bytes.
  bool fits(uint64_t size) const { return start < size && field >= start && size >= 2 && field <= size - 2; }
};

// Finishes a TransmitChecksum in software, in place. False (frame unchanged) if the request does not fit the frame.
inline bool finish_transmit_checksum(ByteView frame, TransmitChecksum checksum) {
  if (!checksum.requested()) return true;
  if (!checksum.fits(frame.size())) return false;
  uint32_t sum = 0;
  const ConstByteView covered = ConstByteView(frame).dropped(checksum.start);
  for (uint64_t i = 0; i + 1 < covered.size(); i += 2) sum += (uint32_t{covered[i]} << 8) | covered[i + 1];
  if (covered.size() & 1) sum += uint32_t{covered[covered.size() - 1]} << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  uint16_t result = static_cast<uint16_t>(~sum);
  if (result == 0 && checksum.udp()) result = 0xFFFF;  // 0 means "no checksum" in UDP
  frame[checksum.field] = static_cast<uint8_t>(result >> 8);
  frame[checksum.field + 1] = static_cast<uint8_t>(result);
  return true;
}

// What an interface does in hardware beyond moving frames.
struct NetworkOffloads {
  bool receive_checksums = false;   // receive_frame() reports checksums the hardware verified
  bool transmit_checksums = false;  // transmit_frame() finishes TCP/UDP checksums in hardware
};

// Called from an interrupt handler when an interface has something new (frames arrived, the link changed). It must not block.
using NetworkWake = void (*)(void* context);

struct NetworkStatistics {
  uint64_t frames_sent = 0;
  uint64_t frames_received = 0;
  uint64_t send_failures = 0;    // frames refused (no room, too big, link down ...)
  uint64_t frames_dropped = 0;   // received frames that were lost or unusable
  uint64_t interrupts = 0;       // interrupts taken (0 for a polled interface)
};

}  // namespace hardware
