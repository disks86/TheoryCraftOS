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


// Port of dav1d src/filmgrain_tmpl.c and src/fg_apply_tmpl.c, single instantiation with a runtime bit depth.
// Original dav1d copyright:
//   Copyright (c) 2018, Niklas Haas; Copyright (c) 2018, VideoLAN and dav1d authors; Copyright (c) 2018, Two Orioles, LLC
//   All rights reserved. BSD 2-clause license (see dav1d's COPYING).

#include "Hardware/VideoCodec/Software/Av1FilmGrain.hpp"

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"

constexpr int GRAIN_WIDTH = 82;
constexpr int GRAIN_HEIGHT = 73;
constexpr int FG_BLOCK_SIZE = 32;
constexpr int SCALING_SIZE = 4096;  // 1 << 12, the largest bit depth
constexpr int SUB_GRAIN_WIDTH = 44;
constexpr int SUB_GRAIN_HEIGHT = 38;

using Entry = int16_t;

struct GrainLut {
  Entry v[GRAIN_HEIGHT + 1][GRAIN_WIDTH];
};

struct Scaling {
  uint8_t v[SCALING_SIZE];
};

int imin(int a, int b) { return a < b ? a : b; }
int iclip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int get_random_number(const int bits, unsigned& state) {
  const int r = static_cast<int>(state);
  const unsigned bit = static_cast<unsigned>((r >> 0) ^ (r >> 1) ^ (r >> 3) ^ (r >> 12)) & 1;
  state = (static_cast<unsigned>(r) >> 1) | (bit << 15);
  return static_cast<int>((state >> (16 - bits)) & ((1u << bits) - 1));
}

int round2(const int x, const uint64_t shift) {
  return (x + ((1 << shift) >> 1)) >> shift;
}

void generate_grain_y(GrainLut& lut, const FilmGrainData& data, const int bitdepth) {
  Entry(*buf)[GRAIN_WIDTH] = lut.v;
  const int bitdepth_min_8 = bitdepth - 8;
  unsigned seed = data.seed;
  const int shift = 4 - bitdepth_min_8 + data.grain_scale_shift;
  const int grain_ctr = 128 << bitdepth_min_8;
  const int grain_min = -grain_ctr, grain_max = grain_ctr - 1;

  for (int y = 0; y < GRAIN_HEIGHT; y++) {
    for (int x = 0; x < GRAIN_WIDTH; x++) {
      const int value = get_random_number(11, seed);
      buf[y][x] = static_cast<Entry>(round2(k_gaussian_sequence[value], static_cast<uint64_t>(shift)));
    }
  }

  const int ar_pad = 3;
  const int ar_lag = data.ar_coeff_lag;

  for (int y = ar_pad; y < GRAIN_HEIGHT; y++) {
    for (int x = ar_pad; x < GRAIN_WIDTH - ar_pad; x++) {
      int ci = 0;
      int sum = 0;
      for (int dy = -ar_lag; dy <= 0; dy++) {
        for (int dx = -ar_lag; dx <= ar_lag; dx++) {
          if (!dx && !dy) break;
          sum += data.ar_coeffs_y[ci++] * buf[y + dy][x + dx];
        }
      }
      const int grain = buf[y][x] + round2(sum, data.ar_coeff_shift);
      buf[y][x] = static_cast<Entry>(iclip(grain, grain_min, grain_max));
    }
  }
}

void generate_grain_uv(GrainLut& lut, const GrainLut& lut_y, const FilmGrainData& data, const int uv, const int subx,
                       const int suby, const int bitdepth) {
  Entry(*buf)[GRAIN_WIDTH] = lut.v;
  const Entry(*buf_y)[GRAIN_WIDTH] = lut_y.v;
  const int bitdepth_min_8 = bitdepth - 8;
  unsigned seed = data.seed ^ (uv ? 0x49d8 : 0xb524);
  const int shift = 4 - bitdepth_min_8 + data.grain_scale_shift;
  const int grain_ctr = 128 << bitdepth_min_8;
  const int grain_min = -grain_ctr, grain_max = grain_ctr - 1;

  const int chromaW = subx ? SUB_GRAIN_WIDTH : GRAIN_WIDTH;
  const int chromaH = suby ? SUB_GRAIN_HEIGHT : GRAIN_HEIGHT;

  for (int y = 0; y < chromaH; y++) {
    for (int x = 0; x < chromaW; x++) {
      const int value = get_random_number(11, seed);
      buf[y][x] = static_cast<Entry>(round2(k_gaussian_sequence[value], static_cast<uint64_t>(shift)));
    }
  }

  const int ar_pad = 3;
  const int ar_lag = data.ar_coeff_lag;

  for (int y = ar_pad; y < chromaH; y++) {
    for (int x = ar_pad; x < chromaW - ar_pad; x++) {
      int ci = 0;
      int sum = 0;
      for (int dy = -ar_lag; dy <= 0; dy++) {
        for (int dx = -ar_lag; dx <= ar_lag; dx++) {
          // For the final (current) pixel, we need to add in the contribution from the luma grain texture
          if (!dx && !dy) {
            if (!data.num_y_points) break;
            int luma = 0;
            const int lumaX = ((x - ar_pad) << subx) + ar_pad;
            const int lumaY = ((y - ar_pad) << suby) + ar_pad;
            for (int i = 0; i <= suby; i++) {
              for (int j = 0; j <= subx; j++) {
                luma += buf_y[lumaY + i][lumaX + j];
              }
            }
            luma = round2(luma, static_cast<uint64_t>(subx + suby));
            sum += luma * data.ar_coeffs_uv[uv][ci];
            break;
          }
          sum += data.ar_coeffs_uv[uv][ci++] * buf[y + dy][x + dx];
        }
      }
      const int grain = buf[y][x] + round2(sum, data.ar_coeff_shift);
      buf[y][x] = static_cast<Entry>(iclip(grain, grain_min, grain_max));
    }
  }
}

// samples from the correct block of a grain LUT, while taking into account the offsets provided by the offsets cache.
// The indices stay inside the 74 x 82 table: offx <= 3 + 2 * 18 = 39, x + 32 * bx <= 33 + 32 (and the same for y).
int sample_lut(const GrainLut& lut, const int offsets[2][2], const int subx, const int suby, const int bx, const int by,
               const int x, const int y) {
  const int randval = offsets[bx][by];
  const int offx = 3 + (2 >> subx) * (3 + (randval >> 4));
  const int offy = 3 + (2 >> suby) * (3 + (randval & 0xF));
  return lut.v[offy + y + (FG_BLOCK_SIZE >> suby) * by][offx + x + (FG_BLOCK_SIZE >> subx) * bx];
}

void seed_row(unsigned (&seed)[2], const unsigned base, const int rows, const int row_num) {
  for (int i = 0; i < rows; i++) {
    seed[i] = base;
    seed[i] ^= static_cast<unsigned>((((row_num - i) * 37 + 178) & 0xFF) << 8);
    seed[i] ^= static_cast<unsigned>(((row_num - i) * 173 + 105) & 0xFF);
  }
}

// dst_row/src_row are the windows that start at the first sample of the 32-row band.
void fgy_32x32xn(const PixelView dst_row, const PixelView src_row, const FilmGrainData& data, const int pw,
                 const Scaling& scaling, const GrainLut& grain_lut, const int bh, const int row_num,
                 const int bitdepth) {
  const int rows = 1 + (data.overlap_flag && row_num > 0);
  const int bitdepth_min_8 = bitdepth - 8;
  const int bitdepth_max = (1 << bitdepth) - 1;
  const int grain_ctr = 128 << bitdepth_min_8;
  const int grain_min = -grain_ctr, grain_max = grain_ctr - 1;

  int min_value, max_value;
  if (data.clip_to_restricted_range) {
    min_value = 16 << bitdepth_min_8;
    max_value = 235 << bitdepth_min_8;
  } else {
    min_value = 0;
    max_value = bitdepth_max;
  }

  // seed[0] contains the current row, seed[1] contains the previous
  unsigned seed[2] = {0, 0};
  seed_row(seed, data.seed, rows, row_num);

  int offsets[2 /* col offset */][2 /* row offset */] = {{0, 0}, {0, 0}};

  // process this row in FG_BLOCK_SIZE^2 blocks
  for (int bx = 0; bx < pw; bx += FG_BLOCK_SIZE) {
    const int bw = imin(FG_BLOCK_SIZE, pw - bx);

    if (data.overlap_flag && bx) {
      // shift previous offsets left
      for (int i = 0; i < rows; i++) offsets[1][i] = offsets[0][i];
    }

    // update current offsets
    for (int i = 0; i < rows; i++) offsets[0][i] = get_random_number(8, seed[i]);

    // x/y block offsets to compensate for overlapped regions
    const int ystart = data.overlap_flag && row_num ? imin(2, bh) : 0;
    const int xstart = data.overlap_flag && bx ? imin(2, bw) : 0;

    static const int w[2][2] = {{27, 17}, {17, 27}};

    auto add_noise_y = [&](const int x, const int y, const int grain) {
      const int s = src_row.at(x + bx, y) & bitdepth_max;
      const int noise = round2(scaling.v[s] * grain, static_cast<uint64_t>(data.scaling_shift));
      dst_row.at(x + bx, y) = static_cast<Pixel>(iclip(s + noise, min_value, max_value));
    };

    for (int y = ystart; y < bh; y++) {
      // Non-overlapped image region (straightforward)
      for (int x = xstart; x < bw; x++) {
        const int grain = sample_lut(grain_lut, offsets, 0, 0, 0, 0, x, y);
        add_noise_y(x, y, grain);
      }

      // Special case for overlapped column
      for (int x = 0; x < xstart; x++) {
        int grain = sample_lut(grain_lut, offsets, 0, 0, 0, 0, x, y);
        const int old = sample_lut(grain_lut, offsets, 0, 0, 1, 0, x, y);
        grain = round2(old * w[x][0] + grain * w[x][1], 5);
        grain = iclip(grain, grain_min, grain_max);
        add_noise_y(x, y, grain);
      }
    }

    for (int y = 0; y < ystart; y++) {
      // Special case for overlapped row (sans corner)
      for (int x = xstart; x < bw; x++) {
        int grain = sample_lut(grain_lut, offsets, 0, 0, 0, 0, x, y);
        const int old = sample_lut(grain_lut, offsets, 0, 0, 0, 1, x, y);
        grain = round2(old * w[y][0] + grain * w[y][1], 5);
        grain = iclip(grain, grain_min, grain_max);
        add_noise_y(x, y, grain);
      }

      // Special case for doubly-overlapped corner
      for (int x = 0; x < xstart; x++) {
        // Blend the top pixel with the top left block
        int top = sample_lut(grain_lut, offsets, 0, 0, 0, 1, x, y);
        int old = sample_lut(grain_lut, offsets, 0, 0, 1, 1, x, y);
        top = round2(old * w[x][0] + top * w[x][1], 5);
        top = iclip(top, grain_min, grain_max);

        // Blend the current pixel with the left block
        int grain = sample_lut(grain_lut, offsets, 0, 0, 0, 0, x, y);
        old = sample_lut(grain_lut, offsets, 0, 0, 1, 0, x, y);
        grain = round2(old * w[x][0] + grain * w[x][1], 5);
        grain = iclip(grain, grain_min, grain_max);

        // Mix the row rows together and apply grain
        grain = round2(top * w[y][0] + grain * w[y][1], 5);
        grain = iclip(grain, grain_min, grain_max);
        add_noise_y(x, y, grain);
      }
    }
  }
}

// `luma_pw` is the luma width: dav1d pads the luma row by one sample when it is odd (ptr[w] = ptr[w - 1]) and the
// 2:1 averaging reads that sample. We clamp the read instead so the source picture stays untouched.
void fguv_32x32xn(const PixelView dst_row, const PixelView src_row, const FilmGrainData& data, const int pw,
                  const Scaling& scaling, const GrainLut& grain_lut, const int bh, const int row_num,
                  const PixelView luma_row, const int luma_pw, const int uv, const int is_id, const int sx,
                  const int sy, const int bitdepth) {
  const int rows = 1 + (data.overlap_flag && row_num > 0);
  const int bitdepth_min_8 = bitdepth - 8;
  const int bitdepth_max = (1 << bitdepth) - 1;
  const int grain_ctr = 128 << bitdepth_min_8;
  const int grain_min = -grain_ctr, grain_max = grain_ctr - 1;

  int min_value, max_value;
  if (data.clip_to_restricted_range) {
    min_value = 16 << bitdepth_min_8;
    max_value = (is_id ? 235 : 240) << bitdepth_min_8;
  } else {
    min_value = 0;
    max_value = bitdepth_max;
  }

  unsigned seed[2] = {0, 0};
  seed_row(seed, data.seed, rows, row_num);

  int offsets[2 /* col offset */][2 /* row offset */] = {{0, 0}, {0, 0}};

  // process this row in FG_BLOCK_SIZE^2 blocks (subsampled)
  for (int bx = 0; bx < pw; bx += FG_BLOCK_SIZE >> sx) {
    const int bw = imin(FG_BLOCK_SIZE >> sx, pw - bx);
    if (data.overlap_flag && bx) {
      // shift previous offsets left
      for (int i = 0; i < rows; i++) offsets[1][i] = offsets[0][i];
    }

    // update current offsets
    for (int i = 0; i < rows; i++) offsets[0][i] = get_random_number(8, seed[i]);

    // x/y block offsets to compensate for overlapped regions
    const int ystart = data.overlap_flag && row_num ? imin(2 >> sy, bh) : 0;
    const int xstart = data.overlap_flag && bx ? imin(2 >> sx, bw) : 0;

    static const int w[2 /* sub */][2 /* off */][2] = {
        {{27, 17}, {17, 27}},
        {{23, 22}, {0, 0}},
    };

    auto add_noise_uv = [&](const int x, const int y, const int grain) {
      const int lx = (bx + x) << sx;
      const int ly = y << sy;
      int avg = luma_row.at(lx, ly) & bitdepth_max;
      if (sx) {
        const int l1 = luma_row.at(lx + 1 < luma_pw ? lx + 1 : luma_pw - 1, ly) & bitdepth_max;
        avg = (avg + l1 + 1) >> 1;
      }
      const int s = src_row.at(bx + x, y) & bitdepth_max;
      int val = avg;
      if (!data.chroma_scaling_from_luma) {
        const int combined = avg * data.uv_luma_mult[uv] + s * data.uv_mult[uv];
        val = iclip((combined >> 6) + (data.uv_offset[uv] * (1 << bitdepth_min_8)), 0, bitdepth_max);
      }
      const int noise = round2(scaling.v[val] * grain, static_cast<uint64_t>(data.scaling_shift));
      dst_row.at(bx + x, y) = static_cast<Pixel>(iclip(s + noise, min_value, max_value));
    };

    for (int y = ystart; y < bh; y++) {
      // Non-overlapped image region (straightforward)
      for (int x = xstart; x < bw; x++) {
        const int grain = sample_lut(grain_lut, offsets, sx, sy, 0, 0, x, y);
        add_noise_uv(x, y, grain);
      }

      // Special case for overlapped column
      for (int x = 0; x < xstart; x++) {
        int grain = sample_lut(grain_lut, offsets, sx, sy, 0, 0, x, y);
        const int old = sample_lut(grain_lut, offsets, sx, sy, 1, 0, x, y);
        grain = round2(old * w[sx][x][0] + grain * w[sx][x][1], 5);
        grain = iclip(grain, grain_min, grain_max);
        add_noise_uv(x, y, grain);
      }
    }

    for (int y = 0; y < ystart; y++) {
      // Special case for overlapped row (sans corner)
      for (int x = xstart; x < bw; x++) {
        int grain = sample_lut(grain_lut, offsets, sx, sy, 0, 0, x, y);
        const int old = sample_lut(grain_lut, offsets, sx, sy, 0, 1, x, y);
        grain = round2(old * w[sy][y][0] + grain * w[sy][y][1], 5);
        grain = iclip(grain, grain_min, grain_max);
        add_noise_uv(x, y, grain);
      }

      // Special case for doubly-overlapped corner
      for (int x = 0; x < xstart; x++) {
        // Blend the top pixel with the top left block
        int top = sample_lut(grain_lut, offsets, sx, sy, 0, 1, x, y);
        int old = sample_lut(grain_lut, offsets, sx, sy, 1, 1, x, y);
        top = round2(old * w[sx][x][0] + top * w[sx][x][1], 5);
        top = iclip(top, grain_min, grain_max);

        // Blend the current pixel with the left block
        int grain = sample_lut(grain_lut, offsets, sx, sy, 0, 0, x, y);
        old = sample_lut(grain_lut, offsets, sx, sy, 1, 0, x, y);
        grain = round2(old * w[sx][x][0] + grain * w[sx][x][1], 5);
        grain = iclip(grain, grain_min, grain_max);

        // Mix the row rows together and apply to image
        grain = round2(top * w[sy][y][0] + grain * w[sy][y][1], 5);
        grain = iclip(grain, grain_min, grain_max);
        add_noise_uv(x, y, grain);
      }
    }
  }
}

// Returns false if the point table is not strictly increasing in x (dav1d asserts dx > 0).
bool generate_scaling(const int bitdepth, const uint8_t (*points)[2], const int num, Scaling& out) {
  uint8_t* const scaling = out.v;
  const int shift_x = bitdepth - 8;
  const int scaling_size = 1 << bitdepth;

  if (num == 0) {
    for (int i = 0; i < scaling_size; i++) scaling[i] = 0;
    return true;
  }
  for (int i = 0; i < num - 1; i++) {
    if (points[i + 1][0] <= points[i][0]) return false;
  }

  // Fill up the preceding entries with the initial value
  for (int i = 0; i < (points[0][0] << shift_x); i++) scaling[i] = points[0][1];

  // Linearly interpolate the values in the middle
  for (int i = 0; i < num - 1; i++) {
    const int bx = points[i][0];
    const int by = points[i][1];
    const int ex = points[i + 1][0];
    const int ey = points[i + 1][1];
    const int dx = ex - bx;
    const int dy = ey - by;
    const int delta = dy * ((0x10000 + (dx >> 1)) / dx);
    for (int x = 0, d = 0x8000; x < dx; x++) {
      scaling[(bx + x) << shift_x] = static_cast<uint8_t>(by + (d >> 16));
      d += delta;
    }
  }

  // Fill up the remaining entries with the final value
  const int n = points[num - 1][0] << shift_x;
  for (int i = n; i < scaling_size; i++) scaling[i] = points[num - 1][1];

  if (shift_x) {
    const int pad = 1 << shift_x, rnd = pad >> 1;
    for (int i = 0; i < num - 1; i++) {
      const int bx = points[i][0] << shift_x;
      const int ex = points[i + 1][0] << shift_x;
      const int dx = ex - bx;
      for (int x = 0; x < dx; x += pad) {
        const int range = scaling[bx + x + pad] - scaling[bx + x];
        for (int k = 1, r = rnd; k < pad; k++) {
          r += range;
          scaling[bx + x + k] = static_cast<uint8_t>(scaling[bx + x] + (r >> shift_x));
        }
      }
    }
  }
  return true;
}

void copy_plane(const PixelView dst, const PixelView src, const int w, const int h) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) dst.at(x, y) = src.at(x, y);
  }
}

}  // namespace

bool apply_film_grain(const FilmGrainData& data, const bool matrix_is_identity, const FilmGrainPicture& src,
                      const FilmGrainPicture& dst) {
  const int bitdepth = src.bitdepth;
  if (bitdepth != 8 && bitdepth != 10 && bitdepth != 12) return false;
  if (dst.bitdepth != bitdepth || dst.layout != src.layout || dst.width != src.width || dst.height != src.height)
    return false;
  if (src.width <= 0 || src.height <= 0 || src.width > 65536 || src.height > 65536) return false;
  if (src.layout < PIXEL_LAYOUT_I400 || src.layout > PIXEL_LAYOUT_I444) return false;
  if (data.num_y_points < 0 || data.num_y_points > 14 || data.num_uv_points[0] < 0 || data.num_uv_points[0] > 10 ||
      data.num_uv_points[1] < 0 || data.num_uv_points[1] > 10)
    return false;
  if (data.ar_coeff_lag < 0 || data.ar_coeff_lag > 3 || data.scaling_shift < 0 || data.scaling_shift > 31 ||
      data.ar_coeff_shift > 31 || data.grain_scale_shift < 0 || data.grain_scale_shift > 3)
    return false;

  const int w = src.width, h = src.height;
  const bool mono = src.layout == PIXEL_LAYOUT_I400;
  const int ss_y = src.layout == PIXEL_LAYOUT_I420;
  const int ss_x = src.layout != PIXEL_LAYOUT_I444;
  const int cpw = (w + ss_x) >> ss_x;
  const int cph = (h + ss_y) >> ss_y;
  const bool chroma_noise = !mono && (data.num_uv_points[0] || data.num_uv_points[1] || data.chroma_scaling_from_luma);

  GrainLut grain_lut[3];
  Scaling scaling[3];

  // prep_grain: generate grain LUTs as needed
  generate_grain_y(grain_lut[0], data, bitdepth);
  if (!mono) {
    if (data.num_uv_points[0] || data.chroma_scaling_from_luma)
      generate_grain_uv(grain_lut[1], grain_lut[0], data, 0, ss_x, ss_y, bitdepth);
    if (data.num_uv_points[1] || data.chroma_scaling_from_luma)
      generate_grain_uv(grain_lut[2], grain_lut[0], data, 1, ss_x, ss_y, bitdepth);
  }

  // Generate scaling LUTs as needed
  if (data.num_y_points || data.chroma_scaling_from_luma) {
    if (!generate_scaling(bitdepth, data.y_points, data.num_y_points, scaling[0])) return false;
  }
  if (data.num_uv_points[0]) {
    if (!generate_scaling(bitdepth, data.uv_points[0], data.num_uv_points[0], scaling[1])) return false;
  }
  if (data.num_uv_points[1]) {
    if (!generate_scaling(bitdepth, data.uv_points[1], data.num_uv_points[1], scaling[2])) return false;
  }

  // Copy over the non-modified planes
  if (!data.num_y_points) copy_plane(dst.plane[0], src.plane[0], w, h);
  if (!mono && !data.chroma_scaling_from_luma) {
    if (!data.num_uv_points[0]) copy_plane(dst.plane[1], src.plane[1], cpw, cph);
    if (!data.num_uv_points[1]) copy_plane(dst.plane[2], src.plane[2], cpw, cph);
  }

  const int is_id = matrix_is_identity;
  const int rows = (h + FG_BLOCK_SIZE - 1) / FG_BLOCK_SIZE;
  for (int row = 0; row < rows; row++) {
    const int band_h = imin(h - row * FG_BLOCK_SIZE, FG_BLOCK_SIZE);
    const PixelView luma_src = src.plane[0].offset(0, row * FG_BLOCK_SIZE);

    if (data.num_y_points) {
      fgy_32x32xn(dst.plane[0].offset(0, row * FG_BLOCK_SIZE), luma_src, data, w, scaling[0], grain_lut[0], band_h,
                  row, bitdepth);
    }
    if (!chroma_noise) continue;

    const int bh = (band_h + ss_y) >> ss_y;
    const int uv_y = (row * FG_BLOCK_SIZE) >> ss_y;
    for (int pl = 0; pl < 2; pl++) {
      const bool csfl = data.chroma_scaling_from_luma != 0;
      if (!csfl && !data.num_uv_points[pl]) continue;
      fguv_32x32xn(dst.plane[1 + pl].offset(0, uv_y), src.plane[1 + pl].offset(0, uv_y), data, cpw,
                   csfl ? scaling[0] : scaling[1 + pl], grain_lut[1 + pl], bh, row, luma_src, w, pl, is_id, ss_x,
                   ss_y, bitdepth);
    }
  }
  return true;
}

}  // namespace av1
}  // namespace hardware
