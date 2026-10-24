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

#include "Hardware/VideoCodec/Software/Av1SuperRes.hpp"

namespace hardware {
namespace av1 {

namespace {
#include "Hardware/VideoCodec/Software/Av1Tables.inc"

int clip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// dav1d resize_c: one row.
void resize_row(PixelView dst, PixelView src, int y, int dst_w, int src_w, int dx, int mx0, int bitdepth_max) {
  int mx = mx0, src_x = -1;
  for (int x = 0; x < dst_w; x++) {
    const int8_t* F = k_resize_filter[mx >> 8];
    int sum = 0;
    for (int k = 0; k < 8; k++) sum += F[k] * src.at(clip(src_x - 3 + k, 0, src_w - 1), y);
    dst.at(x, y) = static_cast<Pixel>(clip((-sum + 64) >> 7, 0, bitdepth_max));
    mx += dx;
    src_x += mx >> 14;
    mx &= 0x3fff;
  }
}
}  // namespace

SuperResStep super_res_step(int in_w, int out_w) {
  // scale_fac and get_upscale_x0 of dav1d's decode.c
  SuperResStep s;
  s.step = static_cast<int>(((static_cast<int64_t>(in_w) << 14) + (out_w >> 1)) / out_w);
  const int err = out_w * s.step - (in_w << 14);
  const int x0 = (-((out_w - in_w) << 13) + (out_w >> 1)) / out_w + 128 - (err / 2);
  s.start = x0 & 0x3fff;
  return s;
}

bool upscale_plane(PixelView dst, PixelView src, int dst_w, int src_w, int rows, SuperResStep step, int bitdepth_max) {
  if (dst_w < 1 || src_w < 1 || rows < 0 || dst_w > 65536 || src_w > 65536 || step.step < 1 || step.step > (1 << 15)) return false;
  if (dst.plane.data() == src.plane.data()) return false;
  for (int y = 0; y < rows; y++) resize_row(dst, src, y, dst_w, src_w, step.step, step.start, bitdepth_max);
  return true;
}

}  // namespace av1
}  // namespace hardware
