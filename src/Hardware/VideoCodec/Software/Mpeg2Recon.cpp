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


// MPEG-2 video decoding, part 3: the inverse transform and motion compensation, and the reconstruction of a macroblock (ISO/IEC 13818-2, 7.5 to 7.9).
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"

namespace hardware {
namespace mpeg2 {

namespace {

// 7.5 the inverse DCT: Chen and Wang's integer algorithm as the reference decoders of the standard (mpeg2decode, libmpeg2) have it. It meets the IEEE 1180 accuracy
// requirements; the 8x8 block of coefficients goes in, the samples (clipped to -256..255) come out, both by position.
constexpr int kW1 = 2841, kW2 = 2676, kW3 = 2408, kW5 = 1609, kW6 = 1108, kW7 = 565;

inline int clip_residual(int v) { return v < -256 ? -256 : v > 255 ? 255 : v; }

void idct_row(int* blk) {
  int x1 = blk[4] << 11, x2 = blk[6], x3 = blk[2], x4 = blk[1], x5 = blk[7], x6 = blk[5], x7 = blk[3];
  if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
    const int v = blk[0] << 3;
    for (int i = 0; i < 8; ++i) blk[i] = v;
    return;
  }
  int x0 = (blk[0] << 11) + 128, x8;
  x8 = kW7 * (x4 + x5);
  x4 = x8 + (kW1 - kW7) * x4;
  x5 = x8 - (kW1 + kW7) * x5;
  x8 = kW3 * (x6 + x7);
  x6 = x8 - (kW3 - kW5) * x6;
  x7 = x8 - (kW3 + kW5) * x7;
  x8 = x0 + x1;
  x0 -= x1;
  x1 = kW6 * (x3 + x2);
  x2 = x1 - (kW2 + kW6) * x2;
  x3 = x1 + (kW2 - kW6) * x3;
  x1 = x4 + x6;
  x4 -= x6;
  x6 = x5 + x7;
  x5 -= x7;
  x7 = x8 + x3;
  x8 -= x3;
  x3 = x0 + x2;
  x0 -= x2;
  x2 = (181 * (x4 + x5) + 128) >> 8;
  x4 = (181 * (x4 - x5) + 128) >> 8;
  blk[0] = (x7 + x1) >> 8;
  blk[1] = (x3 + x2) >> 8;
  blk[2] = (x0 + x4) >> 8;
  blk[3] = (x8 + x6) >> 8;
  blk[4] = (x8 - x6) >> 8;
  blk[5] = (x0 - x4) >> 8;
  blk[6] = (x3 - x2) >> 8;
  blk[7] = (x7 - x1) >> 8;
}

void idct_column(int* blk) {
  int x1 = blk[8 * 4] << 8, x2 = blk[8 * 6], x3 = blk[8 * 2], x4 = blk[8 * 1], x5 = blk[8 * 7], x6 = blk[8 * 5], x7 = blk[8 * 3];
  if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
    const int v = clip_residual((blk[0] + 32) >> 6);
    for (int i = 0; i < 8; ++i) blk[8 * i] = v;
    return;
  }
  int x0 = (blk[0] << 8) + 8192, x8;
  x8 = kW7 * (x4 + x5) + 4;
  x4 = (x8 + (kW1 - kW7) * x4) >> 3;
  x5 = (x8 - (kW1 + kW7) * x5) >> 3;
  x8 = kW3 * (x6 + x7) + 4;
  x6 = (x8 - (kW3 - kW5) * x6) >> 3;
  x7 = (x8 - (kW3 + kW5) * x7) >> 3;
  x8 = x0 + x1;
  x0 -= x1;
  x1 = kW6 * (x3 + x2) + 4;
  x2 = (x1 - (kW2 + kW6) * x2) >> 3;
  x3 = (x1 + (kW2 - kW6) * x3) >> 3;
  x1 = x4 + x6;
  x4 -= x6;
  x6 = x5 + x7;
  x5 -= x7;
  x7 = x8 + x3;
  x8 -= x3;
  x3 = x0 + x2;
  x0 -= x2;
  x2 = (181 * (x4 + x5) + 128) >> 8;
  x4 = (181 * (x4 - x5) + 128) >> 8;
  blk[8 * 0] = clip_residual((x7 + x1) >> 14);
  blk[8 * 1] = clip_residual((x3 + x2) >> 14);
  blk[8 * 2] = clip_residual((x0 + x4) >> 14);
  blk[8 * 3] = clip_residual((x8 + x6) >> 14);
  blk[8 * 4] = clip_residual((x8 - x6) >> 14);
  blk[8 * 5] = clip_residual((x0 - x4) >> 14);
  blk[8 * 6] = clip_residual((x3 - x2) >> 14);
  blk[8 * 7] = clip_residual((x7 - x1) >> 14);
}

void idct(const int16_t* coefficients, int* samples) {
  for (int i = 0; i < 64; ++i) samples[i] = coefficients[i];
  for (int row = 0; row < 8; ++row) idct_row(samples + row * 8);
  for (int column = 0; column < 8; ++column) idct_column(samples + column);
}

inline int clip255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// A plane of a reference picture, or one field of it: `stride` is between the rows of this view.
struct RefPlane {
  ConstByteView data;
  int stride = 0, width = 0, height = 0;
};

// 7.6.4 forming predictions: the w x h block of `ref` whose top left sample is (x, y), with the half-sample positions that hx and hy say, into `dst` (rows `dst_stride`
// apart, starting at `dst_offset`). The sample positions outside the plane are the ones at its edge. With `average` the prediction is averaged with what is there
// (the second direction of a bidirectional prediction, the second half of a dual prime one).
void predict_block(const RefPlane& ref, int x, int y, int hx, int hy, int w, int h, ByteView dst, uint64_t dst_offset, int dst_stride, bool average) {
  auto at = [&](int xx, int yy) {
    xx = xx < 0 ? 0 : xx >= ref.width ? ref.width - 1 : xx;
    yy = yy < 0 ? 0 : yy >= ref.height ? ref.height - 1 : yy;
    return static_cast<int>(ref.data[static_cast<uint64_t>(yy) * static_cast<uint64_t>(ref.stride) + static_cast<uint64_t>(xx)]);
  };
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      int v;
      const int p00 = at(x + c, y + r);
      if (!hx && !hy) v = p00;
      else if (hx && !hy) v = (p00 + at(x + c + 1, y + r) + 1) >> 1;
      else if (!hx && hy) v = (p00 + at(x + c, y + r + 1) + 1) >> 1;
      else v = (p00 + at(x + c + 1, y + r) + at(x + c, y + r + 1) + at(x + c + 1, y + r + 1) + 2) >> 2;
      const uint64_t index = dst_offset + static_cast<uint64_t>(r) * static_cast<uint64_t>(dst_stride) + static_cast<uint64_t>(c);
      dst[index] = static_cast<uint8_t>(average ? (dst[index] + v + 1) >> 1 : v);
    }
  }
}

}  // namespace

// One macroblock: the prediction (inter), the residual, and the result in the picture.
void Core::reconstruct(Mb& mb, int mb_x, int mb_y) {
  Picture* cur = cur_;
  if (!cur) return;
  const bool field = pic_.structure != kFramePicture;
  const int parity = pic_.structure == kBottomField ? 1 : 0;
  const int line_step = field ? 2 : 1;
  ByteView planes[3];
  for (int c = 0; c < 3; ++c) planes[c] = ByteView(cur->plane[c].data(), cur->plane[c].size());
  const int ls = cur->stride[0] * line_step, cs = cur->stride[1] * line_step;
  // Offsets (in bytes) of the macroblock's top left sample in each plane.
  const uint64_t luma_origin = static_cast<uint64_t>(field ? parity * cur->stride[0] : 0) + static_cast<uint64_t>(mb_y) * 16 * static_cast<uint64_t>(ls) + static_cast<uint64_t>(mb_x) * 16;
  const uint64_t chroma_origin = static_cast<uint64_t>(field ? parity * cur->stride[1] : 0) + static_cast<uint64_t>(mb_y) * 8 * static_cast<uint64_t>(cs) + static_cast<uint64_t>(mb_x) * 8;

  if (!mb.intra) {
    // --- prediction, into the picture, to which the residual is then added.
    auto ref_picture = [&](int d, int selected_parity) -> Picture* {
      if (pic_.type == kPictureB) return d == 0 ? anchor_[0] : anchor_[1];
      // P: the newer anchor; the second field of a frame may use the first field of its own frame.
      if (field && second_field_ && selected_parity != parity) return cur;
      return anchor_[1];
    };
    auto plane_of = [&](Picture* p, int c, int selected_parity, bool field_view) {
      RefPlane r;
      const int pw = c == 0 ? p->width : p->width / 2, ph = c == 0 ? p->height : p->height / 2;
      const ConstByteView all(p->plane[c].data(), p->plane[c].size());
      if (field_view) {
        r.data = all.subview(static_cast<uint64_t>(selected_parity) * static_cast<uint64_t>(p->stride[c]));
        r.stride = 2 * p->stride[c];
        r.height = ph / 2;
      } else {
        r.data = all;
        r.stride = p->stride[c];
        r.height = ph;
      }
      r.width = pw;
      return r;
    };
    // Predicts a block of the luma plane and the matching blocks of the chroma planes. `x`, `y`: top left of the luma block in the (field or frame) plane being
    // referenced; `w`, `h`: its size; (mvx, mvy): the vector in half samples; dest_*: where the block goes.
    auto predict_part = [&](Picture* ref, int selected_parity, bool field_view, int x, int y, int w, int h, int mvx, int mvy, uint64_t luma_dest, int luma_stride,
                            uint64_t chroma_dest, int chroma_stride, bool average) {
      if (!ref) return;   // a damaged stream: no such reference picture yet
      predict_block(plane_of(ref, 0, selected_parity, field_view), x + (mvx >> 1), y + (mvy >> 1), mvx & 1, mvy & 1, w, h, planes[0], luma_dest, luma_stride, average);
      const int cmx = mvx / 2, cmy = mvy / 2;   // the chroma vector: half the luma one, truncated toward zero
      for (int c = 1; c < 3; ++c)
        predict_block(plane_of(ref, c, selected_parity, field_view), x / 2 + (cmx >> 1), y / 2 + (cmy >> 1), cmx & 1, cmy & 1, w / 2, h / 2, planes[c], chroma_dest, chroma_stride, average);
    };
    const int dirs[2] = {mb.forward ? 1 : 0, mb.backward ? 1 : 0};
    bool first = true;
    for (int d = 0; d < 2; ++d) {
      if (!dirs[d]) continue;
      const bool average = !first;
      first = false;
      if (!field) {
        if (mb.motion == kMvFrame) {
          predict_part(ref_picture(d, 0), 0, false, mb_x * 16, mb_y * 16, 16, 16, mb.mv[d][0][0], mb.mv[d][0][1], luma_origin, ls, chroma_origin, cs, average);
        } else if (mb.motion == kMvField) {
          for (int j = 0; j < 2; ++j) {
            const int sel = mb.field_select[d][j];
            predict_part(ref_picture(d, sel), sel, true, mb_x * 16, mb_y * 8, 16, 8, mb.mv[d][j][0], mb.mv[d][j][1], luma_origin + static_cast<uint64_t>(j * cur->stride[0]), 2 * ls,
                         chroma_origin + static_cast<uint64_t>(j * cur->stride[1]), 2 * cs, average);
          }
        } else {   // dual prime: each field of the macroblock is predicted from the field of its own parity and the other one, and the two are averaged
          for (int j = 0; j < 2; ++j) {
            predict_part(anchor_[1], j, true, mb_x * 16, mb_y * 8, 16, 8, mb.mv[d][j][0], mb.mv[d][j][1], luma_origin + static_cast<uint64_t>(j * cur->stride[0]), 2 * ls,
                         chroma_origin + static_cast<uint64_t>(j * cur->stride[1]), 2 * cs, average);
            predict_part(anchor_[1], 1 - j, true, mb_x * 16, mb_y * 8, 16, 8, mb.mv[d][2 + j][0], mb.mv[d][2 + j][1], luma_origin + static_cast<uint64_t>(j * cur->stride[0]),
                         2 * ls, chroma_origin + static_cast<uint64_t>(j * cur->stride[1]), 2 * cs, true);
          }
        }
      } else if (mb.motion == kMvField) {
        const int sel = mb.field_select[d][0];
        predict_part(ref_picture(d, sel), sel, true, mb_x * 16, mb_y * 16, 16, 16, mb.mv[d][0][0], mb.mv[d][0][1], luma_origin, ls, chroma_origin, cs, average);
      } else if (mb.motion == kMv16x8) {
        for (int j = 0; j < 2; ++j) {
          const int sel = mb.field_select[d][j];
          predict_part(ref_picture(d, sel), sel, true, mb_x * 16, mb_y * 16 + j * 8, 16, 8, mb.mv[d][j][0], mb.mv[d][j][1], luma_origin + static_cast<uint64_t>(j * 8) * static_cast<uint64_t>(ls),
                       ls, chroma_origin + static_cast<uint64_t>(j * 4) * static_cast<uint64_t>(cs), cs, average);
        }
      } else {   // dual prime in a field picture: the field of the same parity and the field of the other
        predict_part(ref_picture(d, parity), parity, true, mb_x * 16, mb_y * 16, 16, 16, mb.mv[d][0][0], mb.mv[d][0][1], luma_origin, ls, chroma_origin, cs, average);
        predict_part(ref_picture(d, 1 - parity), 1 - parity, true, mb_x * 16, mb_y * 16, 16, 16, mb.mv[d][2][0], mb.mv[d][2][1], luma_origin, ls, chroma_origin, cs, true);
      }
    }
  }

  if (mb.skipped) return;
  // --- the residual (or, in an intra macroblock, the samples) of each coded block.
  for (int b = 0; b < 6; ++b) {
    if (!(mb.cbp & (32 >> b))) continue;
    int samples[64];
    idct(blocks_[b], samples);
    ByteView plane;
    uint64_t origin;
    int stride;
    if (b < 4) {
      plane = planes[0];
      if (mb.field_dct) {   // the block holds the lines of one field of the macroblock
        origin = luma_origin + static_cast<uint64_t>(b >> 1) * static_cast<uint64_t>(cur->stride[0]) + static_cast<uint64_t>(b & 1) * 8;
        stride = 2 * ls;
      } else {
        origin = luma_origin + static_cast<uint64_t>(b >> 1) * 8 * static_cast<uint64_t>(ls) + static_cast<uint64_t>(b & 1) * 8;
        stride = ls;
      }
    } else {
      plane = planes[b - 3];
      origin = chroma_origin;
      stride = cs;
    }
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        const uint64_t index = origin + static_cast<uint64_t>(r) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(c);
        plane[index] = static_cast<uint8_t>(mb.intra ? clip255(samples[r * 8 + c]) : clip255(plane[index] + samples[r * 8 + c]));
      }
  }
}

}  // namespace mpeg2
}  // namespace hardware
