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

#include "Hardware/NetworkInterface/Usb/CdcNcm.hpp"

namespace hardware {

namespace {
constexpr uint32_t kNtbParametersSize = 28;
constexpr uint32_t kNdpEntrySize = 4;
constexpr uint32_t kMinDatagram = 14;                          // an Ethernet header

uint16_t usable_divisor(uint16_t divisor) {
  return divisor == 0 || divisor > 256 || (divisor & (divisor - 1)) != 0 ? 4 : divisor;
}
}  // namespace

bool parse_ncm_ntb_parameters(ConstByteView data, NcmNtbParameters* out) {
  ByteReader in(data);
  const uint16_t length = in.le16();
  NcmNtbParameters p;
  p.formats = in.le16();
  p.in_max_size = in.le32();
  p.in_divisor = in.le16();
  p.in_remainder = in.le16();
  p.in_alignment = in.le16();
  in.skip(2);
  p.out_max_size = in.le32();
  p.out_divisor = in.le16();
  p.out_remainder = in.le16();
  p.out_alignment = in.le16();
  p.out_max_datagrams = in.le16();
  if (!in.ok() || length < kNtbParametersSize || !(p.formats & 1)) return false;
  *out = p;
  return true;
}

uint32_t build_ncm_ntb16(ByteView out, ConstByteView datagram, uint16_t sequence, uint16_t divisor, uint16_t remainder, uint32_t max_size) {
  if (datagram.empty()) return 0;
  const uint32_t table = kNcmNth16Size, table_length = kNcmNdp16HeaderSize + 2 * kNdpEntrySize;   // one entry and the terminating zero one
  const uint16_t d = usable_divisor(divisor);
  uint32_t offset = table + table_length;
  while (offset % d != remainder % d) ++offset;
  if (datagram.size() > 0xFFFF - offset) return 0;
  const uint32_t block = offset + static_cast<uint32_t>(datagram.size());
  if (block > out.size() || (max_size && block > max_size)) return 0;

  ByteWriter w(out);
  w.le32(kNcmNth16Signature);
  w.le16(kNcmNth16Size);
  w.le16(sequence);
  w.le16(static_cast<uint16_t>(block));
  w.le16(static_cast<uint16_t>(table));
  w.le32(kNcmNdp16Signature);
  w.le16(static_cast<uint16_t>(table_length));
  w.le16(0);                                                   // no next table
  w.le16(static_cast<uint16_t>(offset));
  w.le16(static_cast<uint16_t>(datagram.size()));
  w.le16(0);
  w.le16(0);
  w.zeros(offset - (table + table_length));
  w.bytes(datagram);
  return w.ok() && w.position() == block ? block : 0;
}

bool NcmNtbParser::add_used(uint32_t start, uint32_t length) {
  const ArrayView<Region> used(used_);
  if (used_count_ >= used.size()) return false;
  used[used_count_++] = Region{start, length};
  return true;
}

bool NcmNtbParser::overlaps_used(uint32_t start, uint32_t length) const {
  const ArrayView<const Region> used(used_);
  for (uint32_t i = 0; i < used_count_; ++i) {
    const Region& r = used[i];
    if (start < r.start + r.length && r.start < start + length) return true;   // (all offsets and lengths are below 64 KiB: no wrap)
  }
  return false;
}

bool NcmNtbParser::start(ConstByteView ntb) {
  *this = NcmNtbParser();
  ByteReader in(ntb);
  const uint32_t signature = in.le32();
  const uint16_t header_length = in.le16();
  in.skip(2);                                                  // wSequence
  const uint16_t block = in.le16();
  const uint16_t first_table = in.le16();
  if (!in.ok() || signature != kNcmNth16Signature || header_length != kNcmNth16Size || block < kNcmNth16Size || block > ntb.size()) {
    ++malformed_;
    return false;
  }
  block_ = ntb.first(block);
  add_used(0, kNcmNth16Size);
  if (!open_table(first_table)) {
    block_ = ConstByteView();
    return false;
  }
  return true;
}

bool NcmNtbParser::open_table(uint16_t index) {
  table_ = 0;
  if (index == 0) return false;                                // the end of the chain
  if (tables_ >= kMaxTables || index % 4 != 0 || index > block_.size() || block_.size() - index < kNcmNdp16HeaderSize) {
    ++malformed_;
    return false;
  }
  ByteReader in(block_.subview(index));
  const uint32_t signature = in.le32();
  const uint16_t length = in.le16();
  const uint16_t next = in.le16();
  if (!in.ok() || signature != kNcmNdp16Signature || length < kNcmNdp16HeaderSize + 2 * kNdpEntrySize || length % 4 != 0 ||
      length > block_.size() - index || overlaps_used(index, length) || !add_used(index, length)) {
    ++malformed_;
    return false;
  }
  ++tables_;
  table_ = index;
  next_table_ = next;
  entry_ = 0;
  entries_ = (length - kNcmNdp16HeaderSize) / kNdpEntrySize;
  return true;
}

bool NcmNtbParser::next(ConstByteView* datagram) {
  while (table_ != 0 && handed_ < kMaxDatagrams) {
    if (entry_ >= entries_) {
      open_table(next_table_);
      continue;
    }
    ByteReader in(block_.subview(table_ + kNcmNdp16HeaderSize + entry_ * kNdpEntrySize, kNdpEntrySize));
    const uint16_t index = in.le16(), length = in.le16();
    ++entry_;
    if (index == 0 || length == 0) {                           // the terminating entry: on to the next table
      entry_ = entries_;
      continue;
    }
    if (length < kMinDatagram || index > block_.size() || length > block_.size() - index || overlaps_used(index, length) || !add_used(index, length)) {
      ++malformed_;
      continue;
    }
    ++handed_;
    *datagram = block_.subview(index, length);
    return true;
  }
  return false;
}

}  // namespace hardware
