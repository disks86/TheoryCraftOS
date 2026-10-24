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

// The CDF contexts of the AV1 entropy decoder, a port of dav1d's src/cdf.h and src/cdf.c (BSD-2-Clause, Copyright (c) 2018
// VideoLAN and dav1d authors, Two Orioles LLC). The structures have dav1d's exact field layout (arrays of uint16_t, same
// dimensions, same alignment and padding), so the default tables in Av1Cdf.inc, which are dav1d's raw memory, load by one word copy.
// Contexts are plain values: the decoder keeps one per reference slot and copies it (assignment) where dav1d shares a reference.
// A CDF array of n symbols has n + 1 entries: the probabilities, then the adaptation counter (see Msac::symbol_adapt).

namespace hardware {
namespace av1 {

struct CdfModeContext {
  alignas(32) uint16_t uv_mode[2][N_INTRA_PRED_MODES][N_UV_INTRA_PRED_MODES + 2];
  alignas(32) uint16_t partition[N_BL_LEVELS][4][N_PARTITIONS + 6];
  alignas(32) uint16_t cfl_alpha[6][16];
  alignas(32) uint16_t txtp_inter1[2][16];
  alignas(32) uint16_t txtp_inter2[12 + 4];
  alignas(16) uint16_t txtp_intra1[2][N_INTRA_PRED_MODES][7 + 1];
  alignas(16) uint16_t txtp_intra2[3][N_INTRA_PRED_MODES][5 + 3];
  alignas(16) uint16_t cfl_sign[8];
  alignas(16) uint16_t angle_delta[8][8];
  alignas(16) uint16_t filter_intra[5 + 3];
  alignas(16) uint16_t seg_id[3][MAX_SEGMENTS];
  alignas(16) uint16_t pal_sz[2][7][7 + 1];
  alignas(16) uint16_t color_map[2][7][5][8];
  alignas(8) uint16_t txsz[N_TX_SIZES - 1][3][4];
  alignas(8) uint16_t delta_q[4];
  alignas(8) uint16_t delta_lf[5][4];
  alignas(8) uint16_t restore_switchable[3 + 1];
  alignas(4) uint16_t restore_wiener[2];
  alignas(4) uint16_t restore_sgrproj[2];
  alignas(4) uint16_t txtp_inter3[4][2];
  alignas(4) uint16_t use_filter_intra[N_BS_SIZES][2];
  alignas(4) uint16_t txpart[7][3][2];
  alignas(4) uint16_t skip[3][2];
  alignas(4) uint16_t pal_y[7][3][2];
  alignas(4) uint16_t pal_uv[2][2];

  // key/intra
  alignas(4) uint16_t intrabc[2];

  // inter/switch
  alignas(32) uint16_t y_mode[4][N_INTRA_PRED_MODES + 3];
  alignas(32) uint16_t wedge_idx[9][16];
  alignas(16) uint16_t comp_inter_mode[8][N_COMP_INTER_PRED_MODES];
  alignas(8) uint16_t filter[2][8][N_SWITCHABLE_FILTERS + 1];
  alignas(8) uint16_t interintra_mode[4][4];
  alignas(8) uint16_t motion_mode[N_BS_SIZES][3 + 1];
  alignas(4) uint16_t skip_mode[3][2];
  alignas(4) uint16_t newmv_mode[6][2];
  alignas(4) uint16_t globalmv_mode[2][2];
  alignas(4) uint16_t refmv_mode[6][2];
  alignas(4) uint16_t drl_bit[3][2];
  alignas(4) uint16_t intra[4][2];
  alignas(4) uint16_t comp[5][2];
  alignas(4) uint16_t comp_dir[5][2];
  alignas(4) uint16_t jnt_comp[6][2];
  alignas(4) uint16_t mask_comp[6][2];
  alignas(4) uint16_t wedge_comp[9][2];
  alignas(4) uint16_t ref[6][3][2];
  alignas(4) uint16_t comp_fwd_ref[3][3][2];
  alignas(4) uint16_t comp_bwd_ref[2][3][2];
  alignas(4) uint16_t comp_uni_ref[3][3][2];
  alignas(4) uint16_t seg_pred[3][2];
  alignas(4) uint16_t interintra[7][2];
  alignas(4) uint16_t interintra_wedge[7][2];
  alignas(4) uint16_t obmc[N_BS_SIZES][2];
};

struct CdfCoefContext {
  alignas(16) uint16_t eob_bin_16[2][2][5 + 3];
  alignas(16) uint16_t eob_bin_32[2][2][6 + 2];
  alignas(16) uint16_t eob_bin_64[2][2][7 + 1];
  alignas(16) uint16_t eob_bin_128[2][2][8 + 0];
  alignas(32) uint16_t eob_bin_256[2][2][9 + 7];
  alignas(32) uint16_t eob_bin_512[2][10 + 6];
  alignas(32) uint16_t eob_bin_1024[2][11 + 5];
  alignas(8) uint16_t eob_base_tok[N_TX_SIZES][2][4][4];
  alignas(8) uint16_t base_tok[N_TX_SIZES][2][41][4];
  alignas(8) uint16_t br_tok[4 /*5*/][2][21][4];
  alignas(4) uint16_t eob_hi_bit[N_TX_SIZES][2][9][2];
  alignas(4) uint16_t skip[N_TX_SIZES][13][2];
  alignas(4) uint16_t dc_sign[2][3][2];
};

struct CdfMvComponent {
  alignas(32) uint16_t classes[11 + 5];
  alignas(4) uint16_t sign[2];
  alignas(4) uint16_t class0[2];
  alignas(8) uint16_t class0_fp[2][4];
  alignas(4) uint16_t class0_hp[2];
  alignas(4) uint16_t classN[10][2];
  alignas(8) uint16_t classN_fp[4];
  alignas(4) uint16_t classN_hp[2];
};

struct CdfMvContext {
  CdfMvComponent comp[2];
  alignas(8) uint16_t joint[N_MV_JOINTS];
};

struct CdfContext {
  CdfCoefContext coef;
  CdfModeContext m;
  CdfMvContext mv;
  alignas(32) uint16_t kfym[5][5][N_INTRA_PRED_MODES + 3];
};

// Index of the coefficient CDF set for a base quantizer index: 0 for qidx <= 20, 1 for <= 60, 2 for <= 120, else 3
// (dav1d_cdf_thread_init_static).
inline unsigned cdf_qcat(unsigned qidx) { return (qidx > 20) + (qidx > 60) + (qidx > 120); }

// Loads the default CDFs for quantizer category `qcat` (0..3; larger values are clamped to 3) into `dst`: dav1d_cdf_thread_copy of a
// context made by dav1d_cdf_thread_init_static. Every byte of dst, padding included, is overwritten.
void cdf_load_default(CdfContext& dst, unsigned qcat);

// The end-of-frame context update (dav1d_cdf_thread_update): `src` is the context after decoding the frame's (largest or selected)
// tile, `dst` receives the context that is saved for later frames. The adaptation counters are reset to zero. For key and intra
// frames the inter, intrabc and kfym parts of `dst` are left as they were, exactly as dav1d does.
void cdf_update(const FrameHeader& hdr, CdfContext& dst, const CdfContext& src);

}  // namespace av1
}  // namespace hardware
