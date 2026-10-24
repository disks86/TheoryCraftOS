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

#include "Hardware/VideoCodec/Software/Av1Cdf.hpp"
#include <stddef.h>
#include <type_traits>

namespace hardware {
namespace av1 {

namespace {
#include "Hardware/VideoCodec/Software/Av1Cdf.inc"

static_assert(sizeof(CdfModeContext) == kCdfModeBytes, "CdfModeContext must have dav1d's layout");
static_assert(sizeof(CdfCoefContext) == kCdfCoefBytes, "CdfCoefContext must have dav1d's layout");
static_assert(sizeof(CdfMvContext) == kCdfMvBytes, "CdfMvContext must have dav1d's layout");
static_assert(sizeof(CdfContext) == kCdfContextBytes, "CdfContext must have dav1d's layout");
static_assert(offsetof(CdfContext, mv) - offsetof(CdfContext, m) == kCdfModeBytes);
static_assert(sizeof(k_default_cdf) == kDefaultCdfWords * 2, "default CDF word count");
static_assert(sizeof(k_default_coef_cdf) == 4 * kDefaultCoefCdfWords * 2, "default coefficient CDF word count");
static_assert(kDefaultCoefCdfWords * 2 == kCdfCoefBytes);
static_assert(offsetof(CdfContext, coef) == 0 && offsetof(CdfContext, m) == kCdfCoefBytes);
static_assert(std::is_standard_layout_v<CdfContext> && std::is_trivially_copyable_v<CdfContext>);

// The one place that views a context as the 16-bit words it consists of. Safe because CdfContext is standard-layout and made only of
// uint16_t arrays (alignment padding included, which is never read as anything else), so the object representation is exactly
// sizeof(CdfContext) / 2 words and uint16_t may alias it; ArrayView::cast checks size and alignment.
ArrayView<uint16_t> words_of(CdfContext& context) { return bytes_of(context).cast<uint16_t>(); }
ArrayView<const uint16_t> words_of(const CdfContext& context) { return bytes_of(context).cast<const uint16_t>(); }

// Words from the start of a member (offsetof / 2); all members start at even offsets.
constexpr uint64_t word_offset(size_t byte_offset) { return byte_offset / 2; }

// dav1d's update_cdf_*d: zero the counter (entry n) of every CDF in a nested array of CDFs.
template <size_t N>
void reset_counter(uint16_t (&cdf)[N], size_t n) {
  const ArrayView<uint16_t> view(cdf);
  view[n] = 0;
}
template <class T, size_t N>
  requires(!std::is_same_v<T, uint16_t>)
void reset_counter(T (&array)[N], size_t n) {
  for (auto& element : array) reset_counter(element, n);
}
// Only the first `rows` entries of a two-level array (dav1d resets just 4 of the 7 interintra CDFs).
template <size_t R, size_t N>
void reset_counter_rows(uint16_t (&array)[R][N], size_t rows, size_t n) {
  for (size_t i = 0; i < rows; ++i) reset_counter(array[i], n);
}
}  // namespace

void cdf_load_default(CdfContext& dst, unsigned qcat) {
  if (qcat > 3) qcat = 3;
  const ArrayView<uint16_t> out = words_of(dst);
  const ArrayView<const uint16_t> coef(k_default_coef_cdf);
  const ArrayView<const uint16_t> mode(k_default_cdf);
  const uint64_t coef_words = kDefaultCoefCdfWords;
  // dav1d: coef = default_coef_cdf[qcat]; then the mode CDFs and the first mv component (up to the joint CDF) in one copy; then the
  // remainder of the default context (component, joint, kfym) lands on mv.comp[1] and the fields after it.
  const uint64_t joint = kDefaultCdfMvJointOffset / 2, comp = kDefaultCdfMvCompOffset / 2;
  const uint64_t m_start = word_offset(offsetof(CdfContext, m));
  const uint64_t comp1_start = word_offset(offsetof(CdfContext, mv) + sizeof(CdfMvComponent));
  const bool ok = out.subview(0, coef_words).copy_from(coef.subview(qcat * coef_words, coef_words)) &&
                  out.subview(m_start, joint).copy_from(mode.first(joint)) &&
                  out.subview(comp1_start).copy_from(mode.subview(comp));
  if (!ok) bounds_violation("cdf_load_default", qcat, 4);  // cannot happen: the sizes are static_assert-ed
}

void cdf_update(const FrameHeader& hdr, CdfContext& dst, const CdfContext& src) {
  const ArrayView<uint16_t> out = words_of(dst);
  const ArrayView<const uint16_t> in = words_of(src);
  const uint64_t head = word_offset(offsetof(CdfContext, m.intrabc));
  out.first(head).copy_from(in.first(head));

  CdfCoefContext& coef = dst.coef;
  reset_counter(coef.eob_bin_16, 4);
  reset_counter(coef.eob_bin_32, 5);
  reset_counter(coef.eob_bin_64, 6);
  reset_counter(coef.eob_bin_128, 7);
  reset_counter(coef.eob_bin_256, 8);
  reset_counter(coef.eob_bin_512, 9);
  reset_counter(coef.eob_bin_1024, 10);
  reset_counter(coef.eob_base_tok, 2);
  reset_counter(coef.base_tok, 3);
  reset_counter(coef.br_tok, 3);
  reset_counter(coef.eob_hi_bit, 1);
  reset_counter(coef.skip, 1);
  reset_counter(coef.dc_sign, 1);

  CdfModeContext& m = dst.m;
  for (int k = 0; k < 2; ++k) reset_counter(m.uv_mode[k], N_UV_INTRA_PRED_MODES - 1 - !k);
  reset_counter(m.partition[BL_128X128], N_PARTITIONS - 3);
  for (int k = BL_64X64; k < BL_8X8; ++k) reset_counter(m.partition[k], N_PARTITIONS - 1);
  reset_counter(m.partition[BL_8X8], N_SUB8X8_PARTITIONS - 1);
  reset_counter(m.cfl_alpha, 15);
  reset_counter(m.txtp_inter1, 15);
  reset_counter(m.txtp_inter2, 11);
  reset_counter(m.txtp_intra1, 6);
  reset_counter(m.txtp_intra2, 4);
  reset_counter(m.cfl_sign, 7);
  reset_counter(m.angle_delta, 6);
  reset_counter(m.filter_intra, 4);
  reset_counter(m.seg_id, MAX_SEGMENTS - 1);
  reset_counter(m.pal_sz, 6);
  for (int l = 0; l < 2; ++l)
    for (int k = 0; k < 7; ++k) reset_counter(m.color_map[l][k], k + 1);
  for (int k = 0; k < N_TX_SIZES - 1; ++k) reset_counter(m.txsz[k], k + 1 < 2 ? k + 1 : 2);
  reset_counter(m.delta_q, 3);
  reset_counter(m.delta_lf, 3);
  reset_counter(m.restore_switchable, 2);
  reset_counter(m.restore_wiener, 1);
  reset_counter(m.restore_sgrproj, 1);
  reset_counter(m.txtp_inter3, 1);
  reset_counter(m.use_filter_intra, 1);
  reset_counter(m.txpart, 1);
  reset_counter(m.skip, 1);
  reset_counter(m.pal_y, 1);
  reset_counter(m.pal_uv, 1);

  if ((hdr.frame_type & 1) == 0) return;  // IS_KEY_OR_INTRA

  const uint64_t y_mode = word_offset(offsetof(CdfContext, m.y_mode)), kfym = word_offset(offsetof(CdfContext, kfym));
  out.subview(y_mode, kfym - y_mode).copy_from(in.subview(y_mode, kfym - y_mode));

  reset_counter(m.y_mode, N_INTRA_PRED_MODES - 1);
  reset_counter(m.wedge_idx, 15);
  reset_counter(m.comp_inter_mode, N_COMP_INTER_PRED_MODES - 1);
  reset_counter(m.filter, N_SWITCHABLE_FILTERS - 1);
  reset_counter(m.interintra_mode, 3);
  reset_counter(m.motion_mode, 2);
  reset_counter(m.skip_mode, 1);
  reset_counter(m.newmv_mode, 1);
  reset_counter(m.globalmv_mode, 1);
  reset_counter(m.refmv_mode, 1);
  reset_counter(m.drl_bit, 1);
  reset_counter(m.intra, 1);
  reset_counter(m.comp, 1);
  reset_counter(m.comp_dir, 1);
  reset_counter(m.jnt_comp, 1);
  reset_counter(m.mask_comp, 1);
  reset_counter(m.wedge_comp, 1);
  reset_counter(m.ref, 1);
  reset_counter(m.comp_fwd_ref, 1);
  reset_counter(m.comp_bwd_ref, 1);
  reset_counter(m.comp_uni_ref, 1);
  reset_counter(m.seg_pred, 1);
  reset_counter_rows(m.interintra, 4, 1);  // dav1d resets only 4 of the 7 entries (kept bit-exact)
  reset_counter(m.interintra_wedge, 1);
  reset_counter(m.obmc, 1);

  for (CdfMvComponent& comp : dst.mv.comp) {
    reset_counter(comp.classes, 10);
    reset_counter(comp.sign, 1);
    reset_counter(comp.class0, 1);
    reset_counter(comp.class0_fp, 3);
    reset_counter(comp.class0_hp, 1);
    reset_counter(comp.classN, 1);
    reset_counter(comp.classN_fp, 3);
    reset_counter(comp.classN_hp, 1);
  }
  reset_counter(dst.mv.joint, N_MV_JOINTS - 1);
}

}  // namespace av1
}  // namespace hardware
