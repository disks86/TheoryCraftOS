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

#include "Hardware/VideoCodec/Software/H264Parser.hpp"

namespace hardware {

uint64_t H264StreamParser::stored_bit_offset(const uint8_t* nal, uint64_t size, uint64_t rbsp_bits) {
  const uint64_t want_byte = rbsp_bits / 8;
  uint64_t unescaped = 0;
  int zeros = 0;
  uint64_t i = 1;
  for (; i < size; ++i) {
    if (zeros >= 2 && nal[i] == 3) {
      zeros = 0;
      continue;
    }
    if (unescaped == want_byte) break;
    zeros = nal[i] == 0 ? zeros + 1 : 0;
    ++unescaped;
  }
  // `i` is the stored index of the byte holding the bit (a prevention byte right before it was skipped above).
  return i * 8 + rbsp_bits % 8;
}

bool H264StreamParser::ensure_state() {
  if (!sets_) sets_ = UniquePointer<h264::ParameterSets>::make(allocator_);
  return static_cast<bool>(sets_);
}

const h264::Pps* H264StreamParser::pps_for(const h264::SliceHeader& header) const {
  if (!sets_ || header.pps_id < 0 || header.pps_id > 255 || !sets_->pps[header.pps_id].valid) return nullptr;
  return &sets_->pps[header.pps_id];
}

const h264::Sps* H264StreamParser::sps_for(const h264::SliceHeader& header) const {
  const h264::Pps* pps = pps_for(header);
  if (!pps || pps->sps_id < 0 || pps->sps_id > 31 || !sets_->sps[pps->sps_id].valid) return nullptr;
  return &sets_->sps[pps->sps_id];
}

bool H264StreamParser::configure(ConstByteView avcc) {
  if (avcc.size() < 7 || avcc[0] != 1 || !ensure_state()) return false;
  nal_length_size_ = (avcc[4] & 3) + 1;
  uint64_t pos = 5;
  const int n_sps = avcc[pos++] & 31;
  for (int pass = 0; pass < 2; ++pass) {
    const int count = pass == 0 ? n_sps : (pos < avcc.size() ? avcc[pos++] : 0);
    for (int i = 0; i < count; ++i) {
      if (pos + 2 > avcc.size()) return false;
      const uint64_t len = (static_cast<uint64_t>(avcc[pos]) << 8) | avcc[pos + 1];
      pos += 2;
      if (len > avcc.size() - pos) return false;
      List<H264Slice> none(allocator_);
      // A parameter set in an avcC record is a NAL unit behind its own two-byte length, whatever length size the stream's NAL units use.
      const int stream_length_size = nal_length_size_;
      nal_length_size_ = 2;
      const bool parsed = parse(avcc.subview(pos - 2, len + 2), &none);   // a parameter set: stored, no slices
      nal_length_size_ = stream_length_size;
      if (!parsed) return false;
      pos += len;
    }
  }
  return true;
}

bool H264StreamParser::parse(ConstByteView access_unit, List<H264Slice>* slices) {
  slices->clear();
  if (!ensure_state()) return false;
  bool any = false, oom = false;
  const uint8_t* base = access_unit.data();
  h264::for_each_nal(access_unit, nal_length_size_, [&](const uint8_t* nal, uint64_t size) {
    if (oom || size < 1 || (nal[0] & 0x80)) return;
    const int type = nal[0] & 31;
    if (type != 1 && type != 5 && type != 7 && type != 8) return;
    if (rbsp_.size() < size && !rbsp_.alloc(allocator_, size + 64)) { oom = true; return; }
    const uint64_t n = h264::remove_emulation_prevention(nal, size, rbsp_.data());
    h264::BitReader br;
    br.init(rbsp_.data(), n);
    if (type == 7) { any |= sets_->parse_sps(br); return; }
    if (type == 8) { any |= sets_->parse_pps(br); return; }
    H264Slice slice;
    slice.header = h264::SliceHeader{};
    slice.header.nal_unit_type = type;
    slice.header.nal_ref_idc = (nal[0] >> 5) & 3;
    if (!sets_->parse_slice_header(br, &slice.header) || slice.header.redundant_pic_cnt > 0) return;
    const h264::Sps* sps = sps_for(slice.header);
    if (!sps) return;
    slice.nal_offset = static_cast<uint64_t>(nal - base);
    slice.nal_size = size;
    slice.header_bits = br.position() + 8;
    slice.data_bit_offset = stored_bit_offset(nal, size, br.position());
    slice.starts_picture = !have_prev_ || h264::ParameterSets::new_picture(*sps, prev_, slice.header);
    prev_ = slice.header;
    have_prev_ = true;
    if (!slices->push_back(slice)) { oom = true; return; }
    any = true;
  });
  return any && !oom;
}

}  // namespace hardware
