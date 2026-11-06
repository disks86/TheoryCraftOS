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

// The transfer blocks of USB CDC-NCM 1.0 (NTBs), 16-bit format only (the format a function starts in): an NTB header (NTH16), then datagram
// pointer tables (NDP16) that point at the Ethernet frames inside the block. The blocks come from the device and are not trusted: every index
// and length is checked against the block, and a table entry that points outside it, into the headers, or into a frame already handed out ends
// the walk.
#pragma once
#include <stdint.h>
#include "Utility/ByteStream.hpp"

namespace hardware {

constexpr uint32_t kNcmNth16Signature = 0x484D434E;   // "NCMH"
constexpr uint32_t kNcmNdp16Signature = 0x304D434E;   // "NCM0" (no CRC appended)
constexpr uint32_t kNcmNth16Size = 12, kNcmNdp16HeaderSize = 8;

// GET_NTB_PARAMETERS (section 6.2.1): 28 bytes.
struct NcmNtbParameters {
  uint16_t formats = 0;                                        // bit 0: 16-bit NTBs, bit 1: 32-bit
  uint32_t in_max_size = 0;
  uint16_t in_divisor = 0, in_remainder = 0, in_alignment = 0;
  uint32_t out_max_size = 0;
  uint16_t out_divisor = 0, out_remainder = 0, out_alignment = 0;
  uint16_t out_max_datagrams = 0;
};
bool parse_ncm_ntb_parameters(ConstByteView data, NcmNtbParameters* out);

// Builds an NTB16 holding one datagram, placed as the device asked (offset % divisor == remainder; a divisor of 0 or one that is not a power
// of two up to 256 counts as 4). Returns the block's length, 0 if it does not fit in `out` or in `max_size` (0: no limit beyond `out`).
uint32_t build_ncm_ntb16(ByteView out, ConstByteView datagram, uint16_t sequence, uint16_t divisor, uint16_t remainder, uint32_t max_size);

// Walks the datagrams of one received NTB16.
class NcmNtbParser {
 public:
  static constexpr uint32_t kMaxTables = 16;                  // NDPs followed per block (a chain that loops ends here)
  static constexpr uint32_t kMaxDatagrams = 64;               // datagrams handed out per block

  // False if the block's header is malformed (it is then empty); next() hands out nothing.
  bool start(ConstByteView ntb);
  // The next datagram (a whole Ethernet frame); false when there are no more. Entries that are malformed are skipped and counted.
  bool next(ConstByteView* datagram);
  uint32_t malformed() const { return malformed_; }

 private:
  bool open_table(uint16_t index);
  bool overlaps_used(uint32_t start, uint32_t length) const;

  ConstByteView block_;
  uint16_t table_ = 0;                                         // offset of the NDP being read (0: none)
  uint16_t next_table_ = 0;
  uint32_t entry_ = 0, entries_ = 0;                           // the entry to read next, and how many the NDP has
  uint32_t tables_ = 0, handed_ = 0;
  uint32_t malformed_ = 0;
  // Regions already accounted for: the header, the tables and the frames handed out (for the overlap check).
  struct Region { uint32_t start, length; };
  Region used_[2 + kMaxTables + kMaxDatagrams] = {};
  uint32_t used_count_ = 0;
  bool add_used(uint32_t start, uint32_t length);
};

}  // namespace hardware
