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


// MPEG-2 video decoding, part 2: slices, macroblocks, motion vectors and the coefficients of a block (ISO/IEC 13818-2, 6.2.4 to 6.2.6 and 7.2 to 7.6.3).
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"

namespace hardware {
namespace mpeg2 {

namespace {
#include "Hardware/VideoCodec/Software/Mpeg2Tables.inc"

// B.2, B.3 and B.4 macroblock_type: the codes of each picture type and what they mean.
struct MbTypeCode {
  uint8_t code, bits, flags;
};
constexpr uint8_t kQuant = 1, kForward = 2, kBackward = 4, kPattern = 8, kIntra = 16;
constexpr MbTypeCode kTypeI[2] = {{1, 1, kIntra}, {1, 2, kIntra | kQuant}};
constexpr MbTypeCode kTypeP[7] = {{1, 1, kForward | kPattern}, {1, 2, kPattern}, {1, 3, kForward}, {3, 5, kIntra}, {2, 5, kForward | kPattern | kQuant}, {1, 5, kPattern | kQuant}, {1, 6, kIntra | kQuant}};
constexpr MbTypeCode kTypeB[11] = {{2, 2, kForward | kBackward}, {3, 2, kForward | kBackward | kPattern}, {2, 3, kBackward}, {3, 3, kBackward | kPattern}, {2, 4, kForward},
                                   {3, 4, kForward | kPattern}, {3, 5, kIntra}, {2, 5, kForward | kBackward | kPattern | kQuant}, {3, 6, kForward | kPattern | kQuant},
                                   {2, 6, kBackward | kPattern | kQuant}, {1, 6, kIntra | kQuant}};

// Reads macroblock_type; the flags of the code, or -1 if the bits are no code.
int read_macroblock_type(BitReader& br, int picture_type) {
  const uint32_t bits = br.peek(6);
  auto match = [&](const MbTypeCode* codes, int count) {
    for (int i = 0; i < count; ++i)
      if ((bits >> (6 - codes[i].bits)) == codes[i].code) {
        br.skip(codes[i].bits);
        return static_cast<int>(codes[i].flags);
      }
    return -1;
  };
  if (picture_type == kPictureI) return match(kTypeI, 2);
  if (picture_type == kPictureP) return match(kTypeP, 7);
  return match(kTypeB, 11);
}
}  // namespace

// 7.6.3.1 motion vector decoding: one component, from motion_code and motion_residual and the prediction. INT32_MIN on bad data.
int Core::read_motion_component(BitReader& br, int f_code, int prediction) {
  const uint16_t entry = lut_motion_[br.peek(11)];
  if (entry == 0 || f_code < 1 || f_code > 9) return INT32_MIN;
  const int code = entry >> 5;
  br.skip(entry & 31);
  if (code == 0) return prediction;
  const bool negative = br.bit() != 0;
  const int r_size = f_code - 1, f = 1 << r_size;
  int delta = code;
  if (f_code != 1) delta = ((code - 1) * f) + static_cast<int>(br.get(r_size)) + 1;
  if (negative) delta = -delta;
  int vector = prediction + delta;
  const int high = 16 * f - 1, low = -16 * f, range = 32 * f;
  if (vector < low) vector += range;
  if (vector > high) vector -= range;
  return vector;
}

// 6.2.5.2 motion_vectors(): the vectors of both directions (and the concealment vector of an intra macroblock).
bool Core::read_motion_vectors(BitReader& br, Mb& mb) {
  const bool frame = pic_.structure == kFramePicture;
  auto dmvector = [&br]() { return br.bit() ? 1 - (br.bit() << 1) : 0; };
  for (int d = 0; d < 2; ++d) {
    const bool used = d == 0 ? (mb.forward || (mb.intra && pic_.concealment_motion_vectors)) : mb.backward;
    if (!used) continue;
    const int fx = pic_.f_code[d][0], fy = pic_.f_code[d][1];
    int (*pmv)[2] = pmv_[d];
    if (mb.intra) {
      // Concealment vectors: parsed, and they are the predictors of the next macroblock.
      if (!frame) br.skip(1);   // field select
      const int x = read_motion_component(br, fx, pmv[0][0]);
      const int y = read_motion_component(br, fy, pmv[0][1]);
      if (x == INT32_MIN || y == INT32_MIN) return false;
      pmv[0][0] = pmv[1][0] = x;
      pmv[0][1] = pmv[1][1] = y;
      continue;
    }
    switch (mb.motion) {
      case kMvFrame: {
        const int x = read_motion_component(br, fx, pmv[0][0]);
        const int y = read_motion_component(br, fy, pmv[0][1]);
        if (x == INT32_MIN || y == INT32_MIN) return false;
        pmv[0][0] = pmv[1][0] = mb.mv[d][0][0] = x;
        pmv[0][1] = pmv[1][1] = mb.mv[d][0][1] = y;
        break;
      }
      case kMvField:
        if (frame) {   // two field vectors of a frame macroblock: the vertical predictor is in field units
          for (int j = 0; j < 2; ++j) {
            mb.field_select[d][j] = br.bit();
            const int x = read_motion_component(br, fx, pmv[j][0]);
            const int y = read_motion_component(br, fy, pmv[j][1] >> 1);
            if (x == INT32_MIN || y == INT32_MIN) return false;
            pmv[j][0] = mb.mv[d][j][0] = x;
            pmv[j][1] = 2 * y;
            mb.mv[d][j][1] = y;
          }
        } else {
          mb.field_select[d][0] = br.bit();
          const int x = read_motion_component(br, fx, pmv[0][0]);
          const int y = read_motion_component(br, fy, pmv[0][1]);
          if (x == INT32_MIN || y == INT32_MIN) return false;
          pmv[0][0] = pmv[1][0] = mb.mv[d][0][0] = x;
          pmv[0][1] = pmv[1][1] = mb.mv[d][0][1] = y;
        }
        break;
      case kMv16x8:
        for (int j = 0; j < 2; ++j) {
          mb.field_select[d][j] = br.bit();
          const int x = read_motion_component(br, fx, pmv[j][0]);
          const int y = read_motion_component(br, fy, pmv[j][1]);
          if (x == INT32_MIN || y == INT32_MIN) return false;
          pmv[j][0] = mb.mv[d][j][0] = x;
          pmv[j][1] = mb.mv[d][j][1] = y;
        }
        break;
      case kMvDmv: {
        const int shift = frame ? 1 : 0;
        const int x = read_motion_component(br, fx, pmv[0][0]);
        if (x == INT32_MIN) return false;
        const int dmx = dmvector();
        const int y = read_motion_component(br, fy, pmv[0][1] >> shift);
        if (y == INT32_MIN) return false;
        const int dmy = dmvector();
        pmv[0][0] = pmv[1][0] = x;
        pmv[0][1] = pmv[1][1] = y * (1 << shift);
        mb.mv[d][0][0] = mb.mv[d][1][0] = x;
        mb.mv[d][0][1] = mb.mv[d][1][1] = y;
        // 7.6.3.6 dual prime: the vectors for the other parity are derived from the coded one.
        if (frame) {
          int m = pic_.top_field_first ? 1 : 3;
          mb.mv[d][2][0] = ((x * m + (x > 0)) >> 1) + dmx;
          mb.mv[d][2][1] = ((y * m + (y > 0)) >> 1) + dmy - 1;
          m = 4 - m;
          mb.mv[d][3][0] = ((x * m + (x > 0)) >> 1) + dmx;
          mb.mv[d][3][1] = ((y * m + (y > 0)) >> 1) + dmy + 1;
        } else {
          mb.mv[d][2][0] = ((x + (x > 0)) >> 1) + dmx;
          mb.mv[d][2][1] = ((y + (y > 0)) >> 1) + dmy + (pic_.structure == kTopField ? -1 : 1);
        }
        break;
      }
    }
  }
  return !br.error();
}

// 7.2 the coefficients of one block: variable length decoding, inverse quantization (7.4) and the mismatch control. `out` gets them by position.
bool Core::decode_block(BitReader& br, Mb& mb, int block, int16_t* out) {
  for (int i = 0; i < 64; ++i) out[i] = 0;
  const uint8_t* scan = pic_.alternate_scan ? kAlternateScan : kZigzagScan;
  const int qscale = pic_.q_scale_type ? kNonLinearScale[quant_scale_code_] : quant_scale_code_ * 2;
  const uint8_t* matrix = mb.intra ? sequence_.intra_matrix : sequence_.non_intra_matrix;
  const uint16_t* table = lut_dct_[mb.intra && pic_.intra_vlc_format ? 1 : 0].data();
  int sum = 0;
  int i = -1;   // the scan position of the last coefficient read
  if (mb.intra) {
    const int component = block < 4 ? 0 : block - 3;   // 0 luma, 1 Cb, 2 Cr
    const uint16_t entry = (component == 0 ? lut_dc_luma_ : lut_dc_chroma_)[br.peek(component == 0 ? 9 : 10)];
    if (entry == 0) return false;
    br.skip(entry & 31);
    const int size = entry >> 5;
    int diff = 0;
    if (size > 0) {
      diff = static_cast<int>(br.get(size));
      if (!(diff & (1 << (size - 1)))) diff -= (1 << size) - 1;
    }
    dc_pred_[component] += diff;
    int dc = dc_pred_[component] * (8 >> pic_.intra_dc_precision);
    dc = dc < -2048 ? -2048 : dc > 2047 ? 2047 : dc;
    out[0] = static_cast<int16_t>(dc);
    sum = dc;
    i = 0;
  } else if (br.peek(1)) {
    // The first coefficient of a non-intra block has the short code "1s": run 0, level 1 (the end of block cannot come first).
    br.skip(1);
    const bool negative = br.bit() != 0;
    i = 0;
    int level = (((2 * 1 + 1) * qscale * matrix[scan[0]]) >> 5);
    level = level > 2047 ? 2047 : level;
    out[scan[0]] = static_cast<int16_t>(negative ? -level : level);
    sum += negative ? -level : level;
  }
  for (;;) {
    const uint16_t entry = table[br.peek(16)];
    if (entry == 0) return false;
    br.skip(entry & 31);
    const int symbol = entry >> 5;
    if (symbol == 112) break;   // end of block
    int run, level;
    bool negative;
    if (symbol == 111) {   // escape: 6 bits of run, 12 of level (two's complement)
      run = static_cast<int>(br.get(6));
      level = static_cast<int>(br.get(12));
      if (level >= 2048) level -= 4096;
      if (level == 0 || level == -2048) return false;
      negative = level < 0;
      if (negative) level = -level;
    } else {
      run = kDctRun[symbol];
      level = kDctLevel[symbol];
      negative = br.bit() != 0;
    }
    i += run + 1;
    if (i > 63 || br.error()) return false;
    const int position = scan[i];
    int value;
    if (mb.intra) value = (level * qscale * matrix[position]) >> 4;
    else value = ((2 * level + 1) * qscale * matrix[position]) >> 5;
    if (negative) value = -value;
    value = value < -2048 ? -2048 : value > 2047 ? 2047 : value;
    out[position] = static_cast<int16_t>(value);
    sum += value;
  }
  // 7.4.4 mismatch control: the sum of all the coefficients must be odd.
  if (!(sum & 1)) out[63] = static_cast<int16_t>(out[63] ^ 1);
  return !br.error();
}

// 6.2.5 macroblock()
bool Core::macroblock(BitReader& br, Mb& mb) {
  const bool frame = pic_.structure == kFramePicture;
  const int flags = read_macroblock_type(br, pic_.type);
  if (flags < 0) return false;
  mb.intra = flags & kIntra;
  mb.forward = flags & kForward;
  mb.backward = flags & kBackward;
  mb.pattern = flags & kPattern;
  mb.quant = flags & kQuant;
  mb.field_dct = false;
  mb.skipped = false;
  mb.motion = frame ? kMvFrame : kMvField;
  if (mb.forward || mb.backward) {
    int type;
    if (frame && pic_.frame_pred_frame_dct) {
      type = 2;
    } else {
      type = static_cast<int>(br.get(2));
    }
    if (frame) mb.motion = type == 1 ? kMvField : type == 2 ? kMvFrame : type == 3 ? kMvDmv : -1;
    else mb.motion = type == 1 ? kMvField : type == 2 ? kMv16x8 : type == 3 ? kMvDmv : -1;
    if (mb.motion < 0) return false;
    if (mb.motion == kMvDmv && sequence_.progressive_sequence) return false;
  }
  if (frame && !pic_.frame_pred_frame_dct && (mb.intra || mb.pattern)) mb.field_dct = br.bit() != 0;
  if (mb.quant) {
    quant_scale_code_ = static_cast<int>(br.get(5));
    if (quant_scale_code_ == 0) return false;
  }
  for (int d = 0; d < 2; ++d)
    for (int v = 0; v < 4; ++v) mb.mv[d][v][0] = mb.mv[d][v][1] = 0;
  mb.field_select[0][0] = mb.field_select[0][1] = mb.field_select[1][0] = mb.field_select[1][1] = 0;
  if (mb.intra) {
    if (pic_.concealment_motion_vectors) {
      if (!read_motion_vectors(br, mb)) return false;
      br.skip(1);   // marker_bit
    } else {
      for (auto& a : pmv_)
        for (auto& b : a) b[0] = b[1] = 0;
    }
  } else if (mb.forward || mb.backward) {
    if (!read_motion_vectors(br, mb)) return false;
  } else {
    // A predicted picture's macroblock with no motion: the forward prediction with a zero vector (from the field of the picture's own parity in a field picture).
    mb.forward = true;
    mb.motion = frame ? kMvFrame : kMvField;
    if (!frame) mb.field_select[0][0] = pic_.structure == kBottomField ? 1 : 0;
    for (auto& a : pmv_)
      for (auto& b : a) b[0] = b[1] = 0;
  }
  if (mb.pattern) {
    const uint16_t entry = lut_pattern_[br.peek(9)];
    if (entry == 0) return false;
    br.skip(entry & 31);
    mb.cbp = entry >> 5;
    if (mb.cbp == 0) return false;
  } else {
    mb.cbp = mb.intra ? 63 : 0;
  }
  if (!mb.intra) dc_pred_[0] = dc_pred_[1] = dc_pred_[2] = 128 << pic_.intra_dc_precision;
  for (int b = 0; b < 6; ++b) {
    if (!(mb.cbp & (32 >> b))) continue;
    if (!decode_block(br, mb, b, blocks_[b])) return false;
  }
  return !br.error();
}

// 6.2.4 slice()
bool Core::decode_slice(ConstByteView unit, int mb_row) {
  if (offloaded_ || observer_) {
    // Where the macroblocks begin: after the slice header (6.2.4), which the engine is told about.
    BitReader hr(unit.subview(4));
    SliceInfo info;
    int info_row = mb_row;
    if (sequence_.vertical_size > 2800) info_row += static_cast<int>(hr.get(3)) << 7;
    info.quantiser_scale_code = static_cast<int>(hr.get(5));
    while (hr.bit()) hr.skip(8);
    info.mb_row = info_row;
    info.mb_bit_offset = 32 + static_cast<uint32_t>(hr.position());
    int increment = 0;
    for (;;) {
      const uint16_t entry = lut_address_[hr.peek(11)];
      if (entry == 0 || hr.error()) break;
      hr.skip(entry & 31);
      if ((entry >> 5) == 33) {
        increment += 33;
        continue;
      }
      increment += (entry >> 5) + 1;
      break;
    }
    info.mb_col = increment > 0 ? increment - 1 : 0;
    if (!slices_.push_back(unit) || !slice_infos_.push_back(info)) return false;
    if (offloaded_) {
      ++slice_count_;
      return true;
    }
  }
  BitReader br(unit.subview(4));
  if (sequence_.vertical_size > 2800) mb_row += static_cast<int>(br.get(3)) << 7;
  quant_scale_code_ = static_cast<int>(br.get(5));
  if (quant_scale_code_ == 0) return false;
  while (br.bit()) br.skip(8);   // slice_extension_flag and the extra slice information
  const bool field = pic_.structure != kFramePicture;
  const int rows = field ? sequence_.mb_height / 2 : sequence_.mb_height;
  if (mb_row >= rows || br.error()) return false;
  mb_y_ = mb_row;
  dc_pred_[0] = dc_pred_[1] = dc_pred_[2] = 128 << pic_.intra_dc_precision;
  for (auto& a : pmv_)
    for (auto& b : a) b[0] = b[1] = 0;
  ++slice_count_;
  Mb mb, previous;
  bool first = true;
  int mb_x = -1;
  for (;;) {
    // macroblock_address_increment, with its escapes
    int increment = 0;
    for (;;) {
      const uint16_t entry = lut_address_[br.peek(11)];
      if (entry == 0) return false;
      br.skip(entry & 31);
      const int symbol = entry >> 5;
      if (symbol == 33) {
        increment += 33;
        continue;
      }
      increment += symbol + 1;
      break;
    }
    const int target = first ? increment - 1 : mb_x + increment;
    if (target >= sequence_.mb_width) return false;
    if (!first) {
      // The skipped macroblocks between this one and the one before.
      for (int x = mb_x + 1; x < target; ++x) {
        mb_x_ = x;
        Mb skipped;
        skipped.skipped = true;
        if (pic_.type == kPictureP) {
          skipped.forward = true;
          skipped.motion = field ? kMvField : kMvFrame;
          if (field) skipped.field_select[0][0] = pic_.structure == kBottomField ? 1 : 0;
          for (auto& a : pmv_)
            for (auto& b : a) b[0] = b[1] = 0;
        } else {
          // B (7.6.6.4): the directions of the macroblock before, predicted from the vectors in PMV[0] with the whole-macroblock prediction of the picture structure
          // (frame prediction in a frame picture, field prediction from the field of the picture's own parity in a field picture).
          skipped = previous;
          skipped.skipped = true;
          skipped.cbp = 0;
          skipped.pattern = false;
          skipped.intra = false;
          skipped.field_dct = false;
          skipped.motion = field ? kMvField : kMvFrame;
          for (int d = 0; d < 2; ++d) {
            skipped.mv[d][0][0] = pmv_[d][0][0];
            skipped.mv[d][0][1] = pmv_[d][0][1];
            skipped.field_select[d][0] = pic_.structure == kBottomField ? 1 : 0;
          }
        }
        dc_pred_[0] = dc_pred_[1] = dc_pred_[2] = 128 << pic_.intra_dc_precision;
        reconstruct(skipped, x, mb_y_);
        previous = skipped;
      }
    }
    mb_x = target;
    mb_x_ = mb_x;
    first = false;
    mb = Mb();
    if (!macroblock(br, mb)) return false;
    reconstruct(mb, mb_x, mb_y_);
    previous = mb;
    if (br.bits_left() < 8 || br.peek(23) == 0) break;   // the next start code (or stuffing before it)
    if (mb_x + 1 >= sequence_.mb_width) break;
  }
  return true;
}

}  // namespace mpeg2
}  // namespace hardware
