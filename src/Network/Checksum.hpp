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
#include "Network/NetworkTypes.hpp"

namespace network {

// The Internet checksum (RFC 1071): the ones'-complement of the ones'-complement
// sum of 16-bit big-endian words. Usage: accumulate pieces with checksum_add(),
// then checksum_finish(). Verifying a received packet is the same sum over data
// that already contains its checksum: the result is 0.
uint32_t checksum_add(uint32_t sum, ConstByteView data);
uint16_t checksum_finish(uint32_t sum);

// Sum over the pseudo-header that TCP and UDP checksums cover in addition to
// their own segment: source, destination, protocol and segment length.
uint32_t pseudo_header_sum(Ipv4Address source, Ipv4Address destination, uint8_t protocol, uint16_t length);

}  // namespace network
