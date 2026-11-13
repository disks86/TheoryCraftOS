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

// YUV pictures for the compositor: the layouts of the YUV formats a client may give (wl_shm and zwp_linux_dmabuf_v1 use the same four-character codes for
// them) and the conversion of one picture to 0xFFRRGGBB words, which the desktop then draws as it draws any other buffer (WaylandSurfaces.cpp).
//
// The colours are BT.601 in limited range (Y 16..235, U and V 16..240), what video uses when nothing says otherwise; this compositor has no protocol to be told
// another matrix or range (wp_color_representation is not offered). Chroma that covers several pixels is repeated over them, not filtered.
//
// Formats: semi-planar NV12, NV21 (4:2:0), NV16, NV61 (4:2:2), NV24, NV42 (4:4:4) and P010 (NV12 with 16-bit samples, the value in the top ten bits); planar
// YUV420, YVU420, YUV422, YVU422, YUV444, YVU444 (three planes); packed YUYV, YVYU, UYVY, VYUY (4:2:2, one plane). Odd sizes round the chroma up.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace wayland {

struct YuvFormat {
  uint32_t code = 0;
  uint8_t planes = 0;       // 1 (packed), 2 (semi-planar) or 3 (planar)
  uint8_t hsub = 1, vsub = 1;      // chroma subsampling across and down
  uint8_t sample = 1;       // bytes per sample (2: P010)
  bool swapped = false;     // V before U (NV21, YVU420, YVYU, VYUY)
  bool luma_first = true;   // packed: Y0 U Y1 V (YUYV, YVYU) rather than U Y0 V Y1 (UYVY, VYUY)

  // The bytes of one row of `plane` for a picture `width` pixels wide (what a stride must at least be).
  uint64_t row_bytes(uint32_t plane, uint32_t width) const;
  // The rows of `plane` for a picture `height` pixels high.
  uint32_t rows(uint32_t plane, uint32_t height) const;
  // The bytes `plane` covers from its first byte: (rows - 1) strides and one row. (Inputs below 2^31: no overflow.)
  uint64_t plane_bytes(uint32_t plane, uint32_t width, uint32_t height, uint32_t stride) const;
};

// The layout of format `code`; false when it is not a YUV format offered.
bool yuv_format(uint32_t code, YuvFormat* out);
// The codes offered, for wl_shm.format and the dmabuf format table.
ArrayView<const uint32_t> yuv_formats();

// One plane of a picture: its bytes from its first row on, and the distance between rows.
struct YuvPlane {
  ConstByteView bytes;
  uint32_t stride = 0;
};

// Where the planes of a wl_shm buffer are: the first at `offset` with `stride`; each further plane follows the one before it, its stride the first one's
// scaled to its samples (Weston's rule: semi-planar chroma rows are as long as luma rows, a planar chroma plane's stride is the luma stride / hsub). False when
// the stride does not divide (a planar format needs a stride that is a multiple of hsub). The total size of the picture goes to *bytes.
bool yuv_shm_layout(const YuvFormat& f, uint32_t width, uint32_t height, uint32_t stride, uint64_t offsets[3], uint32_t strides[3], uint64_t* bytes);

// Converts the picture (`width` x `height`, planes as `f` has them) into `out`: rows of `width` words 0xFFRRGGBB. False, with nothing written, when a plane's
// stride is shorter than its row, a plane is too small for its rows, a plane is missing or `out` is too small.
bool yuv_to_xrgb(const YuvFormat& f, uint32_t width, uint32_t height, ArrayView<const YuvPlane> planes, ArrayView<uint32_t> out);

}  // namespace wayland
