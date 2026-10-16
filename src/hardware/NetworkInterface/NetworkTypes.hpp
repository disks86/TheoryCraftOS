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

namespace hardware {

struct MacAddress {
  uint8_t bytes[6];

  bool operator==(const MacAddress& other) const {
    for (int i = 0; i < 6; ++i)
      if (bytes[i] != other.bytes[i]) return false;
    return true;
  }
  bool is_broadcast() const { return *this == MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}; }
};

// Ethernet frame sizes, without the 4-byte checksum (CRC) the hardware adds.
constexpr uint32_t kEthernetHeaderSize = 14;
constexpr uint32_t kEthernetMinFrame = 60;    // shorter frames are padded up to this
constexpr uint32_t kEthernetMaxFrame = 1514;  // 14-byte header + 1500 bytes of payload

struct NetworkStatistics {
  uint64_t frames_sent = 0;
  uint64_t frames_received = 0;
  uint64_t send_failures = 0;    // frames refused (no room, too big, link down ...)
  uint64_t frames_dropped = 0;   // received frames that were lost or unusable
};

}  // namespace hardware
