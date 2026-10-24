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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ByteBuffer.hpp"

// Wedge and inter-intra masks of the AV1 decoder: a port of dav1d's src/wedge.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors).
// dav1d builds one global table (dav1d_masks, dav1d_init_ii_wedge_masks) and looks masks up with the macros II_MASK and WEDGE_MASK.
// Here the table is an object that owns its (allocator-backed) storage; build it once with init() and share it read-only.
//
// "layout" is the chroma layout index used by dav1d: 0 for luma and 4:4:4 chroma, 1 for 4:2:2 chroma, 2 for 4:2:0 chroma
// (recon_tmpl.c: chr_layout_idx = layout == I400 ? 0 : 3 - layout). Use mask_layout_index() to compute it. Mask sizes are those of the
// block in that plane: (bw4 * 4 >> ss_hor) * (bh4 * 4 >> ss_ver), row-major with no padding.
namespace hardware {
namespace av1 {

inline int mask_layout_index(PixelLayout layout) { return layout == PIXEL_LAYOUT_I400 ? 0 : 3 - static_cast<int>(layout); }

class WedgeMasks {
 public:
  explicit WedgeMasks(allocators::IAllocator& allocator) : pool_(allocator) {}
  WedgeMasks(const WedgeMasks&) = delete;
  WedgeMasks& operator=(const WedgeMasks&) = delete;

  // Builds the tables (dav1d_init_ii_wedge_masks). False if memory ran out. Calling it again does nothing.
  bool init();
  bool ready() const { return ready_; }

  // WEDGE_MASK(c, bs, sign, idx): the wedge mask for a block of size bs (the wedge sizes 8x8, 8x16, 16x8, 16x16, 16x32, 32x16, 32x32,
  // 8x32 and 32x8), sign (b->mask_sign, 0 or 1) and wedge index (b->wedge_idx, 0..15). Empty if init() was not called or the
  // arguments are out of range.
  ConstByteView wedge_mask(int layout, BlockSize bs, int sign, int wedge_idx) const;
  // II_MASK(c, bs, b): the inter-intra mask of a block: a smooth mask for interintra_type == INTER_INTRA_BLEND (indexed by
  // interintra_mode), the wedge mask (sign 0) of wedge_idx for INTER_INTRA_WEDGE. Block sizes 8x8 .. 32x32 (without 32x8 and 8x32).
  ConstByteView ii_mask(int layout, BlockSize bs, InterIntraType interintra_type, int interintra_mode, int wedge_idx) const;

 private:
  // The mask of the block at a pool offset (kNone gives an empty view).
  ConstByteView at(uint32_t offset, int layout, BlockSize bs) const;

  static constexpr int kSizes = BS_8x8 - BS_32x32 + 1;  // BS_32x32 .. BS_8x8
  static constexpr uint32_t kNone = 0xffffffffu;
  struct Offsets {
    Offsets() {
      for (auto& row : wedge) for (auto& o : row) o = kNone;
      for (auto& o : ii) o = kNone;
    }
    uint32_t wedge[2][16];
    uint32_t ii[N_INTER_INTRA_PRED_MODES];
  };
  ByteBuffer pool_;
  Offsets offsets_[3][kSizes];
  bool ready_ = false;
};

}  // namespace av1
}  // namespace hardware
