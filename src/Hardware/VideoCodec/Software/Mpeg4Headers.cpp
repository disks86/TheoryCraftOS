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

#include "Hardware/VideoCodec/Software/Mpeg4Headers.hpp"

namespace hardware {
namespace mpeg4 {

// ISO/IEC 14496-2 Table 6-... (default_intra_quant_matrix, default_nonintra_quant_matrix), in raster order.
const uint8_t kDefaultIntraMatrix[64] = {
    8,  17, 18, 19, 21, 23, 25, 27,  17, 18, 19, 21, 23, 25, 27, 28,  20, 21, 22, 23, 24, 26, 28, 30,  21, 22, 23, 24, 26, 28, 30, 32,
    22, 23, 24, 26, 28, 30, 32, 35,  23, 24, 26, 28, 30, 32, 35, 38,  25, 26, 28, 30, 32, 35, 38, 41,  27, 28, 30, 32, 35, 38, 41, 45};
const uint8_t kDefaultNonIntraMatrix[64] = {
    16, 17, 18, 19, 20, 21, 22, 23,  17, 18, 19, 20, 21, 22, 23, 24,  18, 19, 20, 21, 22, 23, 24, 25,  19, 20, 21, 22, 23, 24, 26, 27,
    20, 21, 22, 23, 25, 26, 27, 28,  21, 22, 23, 24, 26, 27, 28, 30,  22, 23, 24, 26, 27, 28, 30, 31,  23, 24, 25, 27, 28, 30, 31, 33};
// The zigzag scan: kZigzag[i] is the raster position of scan position i.
const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
                             13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
                             38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

namespace {

// Bits from the start of a byte range; reading past the end sets `overrun` and yields zeros, so a cut header is noticed at the end of a parse, never read beyond.
class Bits {
 public:
  explicit Bits(ConstByteView data) : data_(data) {}
  uint32_t u(uint32_t n) {   // n <= 32
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) v = (v << 1) | bit();
    return v;
  }
  uint32_t bit() {
    const uint64_t byte = pos_ >> 3;
    if (byte >= data_.size()) {
      overrun_ = true;
      return 0;
    }
    const uint32_t b = (data_[byte] >> (7 - (pos_ & 7))) & 1;
    ++pos_;
    return b;
  }
  // A marker bit must be 1.
  bool marker() {
    if (bit() != 1) bad_marker_ = true;
    return !bad_marker_;
  }
  bool ok() const { return !overrun_ && !bad_marker_; }

 private:
  ConstByteView data_;
  uint64_t pos_ = 0;
  bool overrun_ = false, bad_marker_ = false;
};

uint32_t bits_for(uint32_t resolution) {   // av_log2(resolution - 1) + 1, at least 1
  uint32_t bits = 1;
  while (bits < 16 && (1u << bits) < resolution) ++bits;
  return bits;
}

// The studio profile object types (Simple Studio 14, Core Studio 15) have another layer header.
bool studio_object_type(uint8_t type) { return type == 14 || type == 15; }

bool parse_vol(ConstByteView body, uint8_t profile_and_level, Vol* vol) {
  Bits b(body);
  Vol v;
  v.profile_and_level = profile_and_level;
  b.u(1);   // random_accessible_vol
  const uint32_t object_type = b.u(8);
  if (studio_object_type(static_cast<uint8_t>(object_type))) return false;
  if (b.u(1)) {   // is_object_layer_identifier
    v.is_object_layer_identifier = true;
    v.verid = static_cast<uint8_t>(b.u(4));
    b.u(3);   // video_object_layer_priority
  }
  if (b.u(4) == 15) {   // aspect_ratio_info: extended_PAR
    b.u(8);
    b.u(8);
  }
  if (b.u(1)) {   // vol_control_parameters
    if (b.u(2) != 1) return false;   // chroma_format: 4:2:0 only
    v.low_delay = b.u(1) != 0;
    if (b.u(1)) {   // vbv_parameters
      b.u(15); b.marker();   // first_half_bit_rate
      b.u(15); b.marker();   // latter_half_bit_rate
      b.u(15); b.marker();   // first_half_vbv_buffer_size
      b.u(3);                // latter_half_vbv_buffer_size
      b.u(11); b.marker();   // first_half_vbv_occupancy
      b.u(15); b.marker();   // latter_half_vbv_occupancy
    }
  } else {
    // Without control parameters the Simple and Advanced Simple object types (1 and 17) are taken to have no B planes, as FFmpeg does.
    v.low_delay = object_type == 1 || object_type == 17;
  }
  if (b.u(2) != 0) return false;   // video_object_layer_shape: rectangular only
  b.marker();
  v.time_increment_resolution = b.u(16);
  if (v.time_increment_resolution == 0) return false;
  v.time_increment_bits = bits_for(v.time_increment_resolution);
  b.marker();
  if (b.u(1)) {   // fixed_vop_rate
    v.fixed_vop_rate = true;
    b.u(v.time_increment_bits);   // fixed_vop_time_increment
  }
  b.marker();
  v.width = b.u(13);
  b.marker();
  v.height = b.u(13);
  b.marker();
  if (v.width == 0 || v.height == 0) return false;
  v.interlaced = b.u(1) != 0;
  v.obmc_disable = b.u(1) != 0;
  if ((v.verid == 1 ? b.u(1) : b.u(2)) != 0) return false;   // sprite_enable: no sprites
  if (b.u(1)) return false;                                  // not_8_bit
  v.quant_type_mpeg = b.u(1) != 0;
  for (uint32_t i = 0; i < 64; ++i) {
    v.intra_matrix[i] = kDefaultIntraMatrix[kZigzag[i]];
    v.non_intra_matrix[i] = kDefaultNonIntraMatrix[kZigzag[i]];
  }
  if (v.quant_type_mpeg) {
    // Each matrix: up to 64 values in scan order; a zero ends the list and the last value is repeated.
    uint8_t* const matrices[2] = {v.intra_matrix, v.non_intra_matrix};
    for (uint8_t* matrix : matrices) {
      if (!b.u(1)) continue;   // load_intra_quant_mat / load_nonintra_quant_mat
      uint32_t i = 0, last = 0;
      for (; i < 64; ++i) {
        const uint32_t value = b.u(8);
        if (value == 0 || !b.ok()) break;
        last = value;
        matrix[i] = static_cast<uint8_t>(value);
      }
      if (last == 0) return false;
      for (; i < 64; ++i) matrix[i] = static_cast<uint8_t>(last);
    }
  }
  if (v.verid != 1) v.quarter_sample = b.u(1) != 0;
  if (!b.u(1)) return false;   // complexity_estimation_disable: the estimation headers are not understood
  v.resync_marker_disable = b.u(1) != 0;
  v.data_partitioned = b.u(1) != 0;
  if (v.data_partitioned) v.reversible_vlc = b.u(1) != 0;
  if (v.verid != 1) {
    if (b.u(1)) return false;   // newpred_enable
    if (b.u(1)) return false;   // reduced_resolution_vop_enable
  }
  if (b.u(1)) return false;     // scalability
  if (!b.ok()) return false;
  *vol = v;
  return true;
}

bool parse_vop(ConstByteView body, const Vol& vol, Vop* vop) {
  Bits b(body);
  Vop v;
  v.type = static_cast<VopType>(b.u(2));
  while (b.bit()) {   // modulo_time_base: ones, then a zero
    if (++v.modulo_time_base > 4096 || !b.ok()) return false;
  }
  b.marker();
  v.time_increment = b.u(vol.time_increment_bits);
  b.marker();
  v.coded = b.u(1) != 0;
  if (!b.ok()) return false;
  if (v.type == VopType::S) return false;   // (no sprites)
  *vop = v;
  return true;
}

}  // namespace

bool parse_packet(ConstByteView data, const Vol* current, Packet* packet) {
  Packet out;
  const Vol* vol = current;
  uint8_t profile_and_level = current ? current->profile_and_level : 0;
  const uint64_t n = data.size();
  // Start codes: 00 00 01 xx. A unit runs to the next one.
  auto next_start = [&](uint64_t from) {
    for (uint64_t i = from; i + 3 < n; ++i)
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
    return n;
  };
  uint64_t at = next_start(0);
  while (at < n) {
    const uint8_t code = data[at + 3];
    const uint64_t begin = at + 4, end = next_start(begin);
    const ConstByteView body = data.subview(begin, end - begin);
    if (code == 0xB0) {
      if (body.size() < 1) return false;
      profile_and_level = body[0];
    } else if (code >= 0x20 && code <= 0x2F) {
      Vol v;
      if (!parse_vol(body, profile_and_level, &v)) return false;
      out.vol = v;
      out.has_vol = true;
      vol = &out.vol;
    } else if (code == 0xB6) {
      if (!vol) return false;
      Vop vop;
      if (!parse_vop(body, *vol, &vop)) return false;
      if (out.vop_count++ == 0) out.vop = vop;
    }
    at = end;
  }
  if (!out.has_vol && out.vop_count == 0) return false;
  *packet = out;
  return true;
}

}  // namespace mpeg4
}  // namespace hardware
