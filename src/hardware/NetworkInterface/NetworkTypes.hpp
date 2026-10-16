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
