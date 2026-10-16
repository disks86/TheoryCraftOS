#include "Network/Checksum.hpp"

namespace network {

uint32_t checksum_add(uint32_t sum, const uint8_t* data, uint32_t length) {
  // 32-bit accumulator: the carries are folded back in by checksum_finish().
  // (Callers add at most a few hundred 16-bit words per call, so it cannot overflow.)
  uint64_t wide = sum;
  uint32_t i = 0;
  for (; i + 1 < length; i += 2) wide += (uint32_t{data[i]} << 8) | data[i + 1];
  if (i < length) wide += uint32_t{data[i]} << 8;  // odd trailing byte is padded with zero
  while (wide >> 32) wide = (wide & 0xFFFFFFFF) + (wide >> 32);
  return static_cast<uint32_t>(wide);
}

uint16_t checksum_finish(uint32_t sum) {
  uint64_t wide = sum;
  while (wide >> 16) wide = (wide & 0xFFFF) + (wide >> 16);
  return static_cast<uint16_t>(~wide);
}

uint32_t pseudo_header_sum(Ipv4Address source, Ipv4Address destination, uint8_t protocol, uint16_t length) {
  uint8_t pseudo[12];
  source.write(pseudo);
  destination.write(pseudo + 4);
  pseudo[8] = 0;
  pseudo[9] = protocol;
  store16(pseudo + 10, length);
  return checksum_add(0, pseudo, sizeof(pseudo));
}

}  // namespace network
