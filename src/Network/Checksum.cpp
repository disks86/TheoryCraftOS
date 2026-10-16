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

#include "Network/Checksum.hpp"

namespace network {

uint32_t checksum_add(uint32_t sum, ConstByteView data) {
  // 32-bit accumulator: the carries are folded back in by checksum_finish().
  // (Callers add at most a few hundred 16-bit words per call, so it cannot overflow.)
  uint64_t wide = sum;
  const uint64_t length = data.size();
  uint64_t i = 0;
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
  ByteWriter out{ByteView(pseudo)};
  source.write(out);
  destination.write(out);
  out.u8(0);
  out.u8(protocol);
  out.be16(length);
  return checksum_add(0, out.written());
}

}  // namespace network
