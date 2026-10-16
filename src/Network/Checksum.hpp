#pragma once
#include "Network/NetworkTypes.hpp"

namespace network {

// The Internet checksum (RFC 1071): the ones'-complement of the ones'-complement
// sum of 16-bit big-endian words. Usage: accumulate pieces with checksum_add(),
// then checksum_finish(). Verifying a received packet is the same sum over data
// that already contains its checksum: the result is 0.
uint32_t checksum_add(uint32_t sum, const uint8_t* data, uint32_t length);
uint16_t checksum_finish(uint32_t sum);

// Sum over the pseudo-header that TCP and UDP checksums cover in addition to
// their own segment: source, destination, protocol and segment length.
uint32_t pseudo_header_sum(Ipv4Address source, Ipv4Address destination, uint8_t protocol, uint16_t length);

}  // namespace network
