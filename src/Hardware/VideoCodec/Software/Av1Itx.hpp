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
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ArrayView.hpp"

// AV1 inverse transforms: a port of dav1d's src/itx_tmpl.c and src/itx_1d.c (BSD-2-Clause, Copyright (c) 2018-2019 VideoLAN and dav1d
// authors, Two Orioles LLC). The output is bit-exact with dav1d. The bit depth is a runtime parameter; there is one instantiation.
namespace hardware {
namespace av1 {

// Largest coefficient block an inverse transform reads: 32x32 (64-point transforms only code their top-left 32x32 corner).
constexpr uint32_t ITX_MAX_COEFS = 32 * 32;

// Number of int32_t a caller has to provide in `coef` for the transform size `tx` (a TxfmSize or RectTxfmSize): min(w, 32) * min(h, 32).
// Returns 0 for a value that is not a transform size.
uint32_t itx_coef_count(int tx);

// Inverse transform of one block plus reconstruction: dst(x, y) = clip(dst(x, y) + residual(x, y), 0, bitdepth_max) for the w x h block
// whose top-left sample is dst(0, 0) (w and h are the dimensions of `tx`, up to 64).
//
//   tx           TxfmSize (TX_4X4 .. TX_64X64) or RectTxfmSize (RTX_4X8 .. RTX_64X16), dav1d's `dav1d_txfm_dimensions` index.
//   txtp         TxfmType (DCT_DCT .. H_FLIPADST), or WHT_WHT for lossless (only with TX_4X4). Types a size does not allow are rejected:
//                64-point dimensions allow DCT_DCT only, 32-point dimensions DCT_DCT and IDTX only (as in the AV1 transform sets).
//   eob          the value dav1d's decode_coefs returns: the scan position of the last non-zero coefficient (0 = only the DC
//                coefficient is present). For DCT_DCT, eob == 0 takes dav1d's DC-only shortcut. The transform only looks at the
//                first rows its eob can reach; coefficients beyond the eob position must be zero (as dav1d's parser guarantees).
//                An eob larger than the block's coefficient count is clamped.
//   bitdepth_max (1 << bitdepth) - 1 for bitdepth 8, 10 or 12.
//
// Coefficient layout (exactly what dav1d's decode_coefs / read_coef_blocks leave in `cf`): let sw = min(w, 32), sh = min(h, 32).
// The coefficient at horizontal frequency x (column, 0 .. sw-1) and vertical frequency y (row, 0 .. sh-1) is coef[x * sh + y],
// i.e. column-major with `sh` entries per column (equivalently: rc = scan position value, x = rc >> log2(sh), y = rc & (sh - 1)).
// `coef` must hold at least itx_coef_count(tx) entries (more is fine). Values must lie in the range dav1d's dequantisation allows:
// [-(1 << (bitdepth + 7)), (1 << (bitdepth + 7)) - 1] (for bitdepth 8 the range of int16_t).
// Like dav1d, the function zeroes the first itx_coef_count(tx) entries of `coef` afterwards (also for the DC-only shortcut, where
// only coef[0] was used and is cleared), so the caller can reuse the buffer for the next block.
//
// Returns false (and touches nothing) if `tx`/`txtp` are not a valid combination or `coef` is too small.
bool inv_txfm_add(PixelView dst, ArrayView<int32_t> coef, int tx, int txtp, int eob, int bitdepth_max);

}  // namespace av1
}  // namespace hardware
