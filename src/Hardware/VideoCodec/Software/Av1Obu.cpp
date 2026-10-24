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

#include "Hardware/VideoCodec/Software/Av1Obu.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace av1 {

namespace {

constexpr int kIntMin = -2147483647 - 1;

int imin(int a, int b) { return a < b ? a : b; }
int imax(int a, int b) { return a > b ? a : b; }
int iclip_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int tile_log2(int sz, int tgt) {
  int k = 0;
  while ((sz << k) < tgt) ++k;
  return k;
}

const LoopfilterModeRefDeltas k_default_mode_ref_deltas = {{0, 0}, {1, 0, 0, 0, -1, 0, -1, -1}};

// The part of SequenceHeader that has to be identical within a coded video sequence (everything before operating_parameter_info).
bool same_sequence(const SequenceHeader& a, const SequenceHeader& b) {
  if (a.profile != b.profile || a.max_width != b.max_width || a.max_height != b.max_height || a.layout != b.layout || a.pri != b.pri ||
      a.trc != b.trc || a.mtrx != b.mtrx || a.chr != b.chr || a.hbd != b.hbd || a.color_range != b.color_range ||
      a.num_operating_points != b.num_operating_points)
    return false;
  for (int i = 0; i < MAX_OPERATING_POINTS; ++i) {
    const auto& x = a.operating_points[i];
    const auto& y = b.operating_points[i];
    if (x.major_level != y.major_level || x.minor_level != y.minor_level || x.initial_display_delay != y.initial_display_delay ||
        x.idc != y.idc || x.tier != y.tier || x.decoder_model_param_present != y.decoder_model_param_present ||
        x.display_model_param_present != y.display_model_param_present)
      return false;
  }
  return a.still_picture == b.still_picture && a.reduced_still_picture_header == b.reduced_still_picture_header &&
         a.timing_info_present == b.timing_info_present && a.num_units_in_tick == b.num_units_in_tick && a.time_scale == b.time_scale &&
         a.equal_picture_interval == b.equal_picture_interval && a.num_ticks_per_picture == b.num_ticks_per_picture &&
         a.decoder_model_info_present == b.decoder_model_info_present &&
         a.encoder_decoder_buffer_delay_length == b.encoder_decoder_buffer_delay_length &&
         a.num_units_in_decoding_tick == b.num_units_in_decoding_tick && a.buffer_removal_delay_length == b.buffer_removal_delay_length &&
         a.frame_presentation_delay_length == b.frame_presentation_delay_length &&
         a.display_model_info_present == b.display_model_info_present && a.width_n_bits == b.width_n_bits &&
         a.height_n_bits == b.height_n_bits && a.frame_id_numbers_present == b.frame_id_numbers_present &&
         a.delta_frame_id_n_bits == b.delta_frame_id_n_bits && a.frame_id_n_bits == b.frame_id_n_bits && a.sb128 == b.sb128 &&
         a.filter_intra == b.filter_intra && a.intra_edge_filter == b.intra_edge_filter && a.inter_intra == b.inter_intra &&
         a.masked_compound == b.masked_compound && a.warped_motion == b.warped_motion && a.dual_filter == b.dual_filter &&
         a.order_hint == b.order_hint && a.jnt_comp == b.jnt_comp && a.ref_frame_mvs == b.ref_frame_mvs &&
         a.screen_content_tools == b.screen_content_tools && a.force_integer_mv == b.force_integer_mv &&
         a.order_hint_n_bits == b.order_hint_n_bits && a.super_res == b.super_res && a.cdef == b.cdef &&
         a.restoration == b.restoration && a.ss_hor == b.ss_hor && a.ss_ver == b.ss_ver && a.monochrome == b.monochrome &&
         a.color_description_present == b.color_description_present && a.separate_uv_delta_q == b.separate_uv_delta_q &&
         a.film_grain_present == b.film_grain_present;
}

bool same_operating_parameter_info(const SequenceHeader& a, const SequenceHeader& b) {
  for (int i = 0; i < MAX_OPERATING_POINTS; ++i) {
    const auto& x = a.operating_parameter_info[i];
    const auto& y = b.operating_parameter_info[i];
    if (x.decoder_buffer_delay != y.decoder_buffer_delay || x.encoder_buffer_delay != y.encoder_buffer_delay ||
        x.low_delay_mode != y.low_delay_mode)
      return false;
  }
  return true;
}

// trailing_bits(): the one bit (and, strictly, only zero bits after it) closing an OBU.
bool check_trailing_bits(BitReader& gb, bool strict) {
  const unsigned trailing_one_bit = gb.bit();
  if (gb.error()) return false;
  if (!strict) return true;
  if (!trailing_one_bit || !gb.rest_of_byte_is_zero()) return false;
  const ConstByteView data = gb.data();
  uint64_t pos = gb.byte_position();
  for (uint64_t i = pos; i < data.size(); ++i)
    if (data[i] != 0) return false;
  return true;
}

}  // namespace

WarpedMotionParams default_wm_params() {
  WarpedMotionParams wm{};
  wm.type = WM_TYPE_IDENTITY;
  wm.matrix[0] = 0;
  wm.matrix[1] = 0;
  wm.matrix[2] = 1 << 16;
  wm.matrix[3] = 0;
  wm.matrix[4] = 0;
  wm.matrix[5] = 1 << 16;
  wm.u.p.alpha = 0;
  wm.u.p.beta = 0;
  wm.u.p.gamma = 0;
  wm.u.p.delta = 0;
  return wm;
}

ObuParser::ObuParser(allocators::IAllocator& allocator) : allocator_(allocator) {}

ObuParser::~ObuParser() { release_view(allocator_, tiles_); }

void ObuParser::reset() {
  invalidate_refs();
  have_seq_ = false;
  have_frame_ = false;
  frame_open_ = false;
  have_cll_ = have_mdcv_ = false;
  n_tiles_ = 0;
  operating_point_idc_ = 0;
  max_spatial_id_ = 0;
  error_ = ObuError::None;
}

void ObuParser::invalidate_refs() {
  for (RefSlot& slot : refs_) slot.valid = false;
}

void ObuParser::ref_update(unsigned refresh_frame_flags, const FrameHeader& hdr) {
  if (!have_seq_) return;
  for (int i = 0; i < NUM_REF_FRAMES; ++i) {
    if (!(refresh_frame_flags & (1u << i))) continue;
    RefSlot& slot = refs_[i];
    slot.hdr = hdr;
    slot.valid = true;
    slot.showable = hdr.showable_frame != 0;
    slot.layout = seq_.layout;
    slot.bpc = 8 + 2 * seq_.hbd;
  }
}

bool ObuParser::reserve_tiles(uint64_t count) {
  if (tiles_.size() >= count) return true;
  release_view(allocator_, tiles_);
  tiles_ = allocate_view<TileData>(allocator_, count);
  if (tiles_.empty()) return false;
  for (TileData& tile : tiles_) tile = TileData();
  return true;
}

// ---- sequence header ----------------------------------------------------------------------------------------------------------------
bool ObuParser::parse_seq_hdr(BitReader& gb, SequenceHeader& hdr) {
  hdr = SequenceHeader{};
  hdr.profile = static_cast<uint8_t>(gb.bits(3));
  if (hdr.profile > 2) return fail(ObuError::Syntax);

  hdr.still_picture = static_cast<uint8_t>(gb.bit());
  hdr.reduced_still_picture_header = static_cast<uint8_t>(gb.bit());
  if (hdr.reduced_still_picture_header && !hdr.still_picture) return fail(ObuError::Syntax);

  if (hdr.reduced_still_picture_header) {
    hdr.num_operating_points = 1;
    hdr.operating_points[0].major_level = static_cast<uint8_t>(gb.bits(3));
    hdr.operating_points[0].minor_level = static_cast<uint8_t>(gb.bits(2));
    hdr.operating_points[0].initial_display_delay = 10;
  } else {
    hdr.timing_info_present = static_cast<uint8_t>(gb.bit());
    if (hdr.timing_info_present) {
      hdr.num_units_in_tick = gb.bits(32);
      hdr.time_scale = gb.bits(32);
      if (strict_ && (!hdr.num_units_in_tick || !hdr.time_scale)) return fail(ObuError::Syntax);
      hdr.equal_picture_interval = static_cast<uint8_t>(gb.bit());
      if (hdr.equal_picture_interval) {
        const unsigned num_ticks_per_picture = gb.vlc();
        if (num_ticks_per_picture == 0xffffffffu) return fail(ObuError::Syntax);
        hdr.num_ticks_per_picture = num_ticks_per_picture + 1;
      }

      hdr.decoder_model_info_present = static_cast<uint8_t>(gb.bit());
      if (hdr.decoder_model_info_present) {
        hdr.encoder_decoder_buffer_delay_length = static_cast<uint8_t>(gb.bits(5) + 1);
        hdr.num_units_in_decoding_tick = gb.bits(32);
        if (strict_ && !hdr.num_units_in_decoding_tick) return fail(ObuError::Syntax);
        hdr.buffer_removal_delay_length = static_cast<uint8_t>(gb.bits(5) + 1);
        hdr.frame_presentation_delay_length = static_cast<uint8_t>(gb.bits(5) + 1);
      }
    }

    hdr.display_model_info_present = static_cast<uint8_t>(gb.bit());
    hdr.num_operating_points = static_cast<uint8_t>(gb.bits(5) + 1);
    for (int i = 0; i < hdr.num_operating_points; i++) {
      SequenceHeader::SequenceHeaderOperatingPoint& op = hdr.operating_points[i];
      op.idc = static_cast<uint16_t>(gb.bits(12));
      if (op.idc && (!(op.idc & 0xff) || !(op.idc & 0xf00))) return fail(ObuError::Syntax);
      op.major_level = static_cast<uint8_t>(2 + gb.bits(3));
      op.minor_level = static_cast<uint8_t>(gb.bits(2));
      if (op.major_level > 3) op.tier = static_cast<uint8_t>(gb.bit());
      if (hdr.decoder_model_info_present) {
        op.decoder_model_param_present = static_cast<uint8_t>(gb.bit());
        if (op.decoder_model_param_present) {
          SequenceHeader::SequenceHeaderOperatingParameterInfo& opi = hdr.operating_parameter_info[i];
          opi.decoder_buffer_delay = gb.bits(hdr.encoder_decoder_buffer_delay_length);
          opi.encoder_buffer_delay = gb.bits(hdr.encoder_decoder_buffer_delay_length);
          opi.low_delay_mode = static_cast<uint8_t>(gb.bit());
        }
      }
      if (hdr.display_model_info_present) op.display_model_param_present = static_cast<uint8_t>(gb.bit());
      op.initial_display_delay = op.display_model_param_present ? static_cast<uint8_t>(gb.bits(4) + 1) : 10;
    }
  }

  hdr.width_n_bits = static_cast<uint8_t>(gb.bits(4) + 1);
  hdr.height_n_bits = static_cast<uint8_t>(gb.bits(4) + 1);
  hdr.max_width = static_cast<int>(gb.bits(hdr.width_n_bits)) + 1;
  hdr.max_height = static_cast<int>(gb.bits(hdr.height_n_bits)) + 1;
  if (!hdr.reduced_still_picture_header) {
    hdr.frame_id_numbers_present = static_cast<uint8_t>(gb.bit());
    if (hdr.frame_id_numbers_present) {
      hdr.delta_frame_id_n_bits = static_cast<uint8_t>(gb.bits(4) + 2);
      hdr.frame_id_n_bits = static_cast<uint8_t>(gb.bits(3) + hdr.delta_frame_id_n_bits + 1);
    }
  }

  hdr.sb128 = static_cast<uint8_t>(gb.bit());
  hdr.filter_intra = static_cast<uint8_t>(gb.bit());
  hdr.intra_edge_filter = static_cast<uint8_t>(gb.bit());
  if (hdr.reduced_still_picture_header) {
    hdr.screen_content_tools = ADAPTIVE;
    hdr.force_integer_mv = ADAPTIVE;
  } else {
    hdr.inter_intra = static_cast<uint8_t>(gb.bit());
    hdr.masked_compound = static_cast<uint8_t>(gb.bit());
    hdr.warped_motion = static_cast<uint8_t>(gb.bit());
    hdr.dual_filter = static_cast<uint8_t>(gb.bit());
    hdr.order_hint = static_cast<uint8_t>(gb.bit());
    if (hdr.order_hint) {
      hdr.jnt_comp = static_cast<uint8_t>(gb.bit());
      hdr.ref_frame_mvs = static_cast<uint8_t>(gb.bit());
    }
    hdr.screen_content_tools = gb.bit() ? ADAPTIVE : static_cast<AdaptiveBoolean>(gb.bit());
    hdr.force_integer_mv = hdr.screen_content_tools ? (gb.bit() ? ADAPTIVE : static_cast<AdaptiveBoolean>(gb.bit())) : ADAPTIVE;
    if (hdr.order_hint) hdr.order_hint_n_bits = static_cast<uint8_t>(gb.bits(3) + 1);
  }
  hdr.super_res = static_cast<uint8_t>(gb.bit());
  hdr.cdef = static_cast<uint8_t>(gb.bit());
  hdr.restoration = static_cast<uint8_t>(gb.bit());

  hdr.hbd = static_cast<uint8_t>(gb.bit());
  if (hdr.profile == 2 && hdr.hbd) hdr.hbd = static_cast<uint8_t>(hdr.hbd + gb.bit());
  if (hdr.profile != 1) hdr.monochrome = static_cast<uint8_t>(gb.bit());
  hdr.color_description_present = static_cast<uint8_t>(gb.bit());
  if (hdr.color_description_present) {
    hdr.pri = static_cast<ColorPrimaries>(gb.bits(8));
    hdr.trc = static_cast<TransferCharacteristics>(gb.bits(8));
    hdr.mtrx = static_cast<MatrixCoefficients>(gb.bits(8));
  } else {
    hdr.pri = COLOR_PRI_UNKNOWN;
    hdr.trc = TRC_UNKNOWN;
    hdr.mtrx = MC_UNKNOWN;
  }
  if (hdr.monochrome) {
    hdr.color_range = static_cast<uint8_t>(gb.bit());
    hdr.layout = PIXEL_LAYOUT_I400;
    hdr.ss_hor = hdr.ss_ver = 1;
    hdr.chr = CHR_UNKNOWN;
  } else if (hdr.pri == COLOR_PRI_BT709 && hdr.trc == TRC_SRGB && hdr.mtrx == MC_IDENTITY) {
    hdr.layout = PIXEL_LAYOUT_I444;
    hdr.color_range = 1;
    if (hdr.profile != 1 && !(hdr.profile == 2 && hdr.hbd == 2)) return fail(ObuError::Syntax);
  } else {
    hdr.color_range = static_cast<uint8_t>(gb.bit());
    switch (hdr.profile) {
      case 0:
        hdr.layout = PIXEL_LAYOUT_I420;
        hdr.ss_hor = hdr.ss_ver = 1;
        break;
      case 1:
        hdr.layout = PIXEL_LAYOUT_I444;
        break;
      case 2:
        if (hdr.hbd == 2) {
          hdr.ss_hor = static_cast<uint8_t>(gb.bit());
          if (hdr.ss_hor) hdr.ss_ver = static_cast<uint8_t>(gb.bit());
        } else {
          hdr.ss_hor = 1;
        }
        hdr.layout = hdr.ss_hor ? (hdr.ss_ver ? PIXEL_LAYOUT_I420 : PIXEL_LAYOUT_I422) : PIXEL_LAYOUT_I444;
        break;
    }
    hdr.chr = (hdr.ss_hor & hdr.ss_ver) ? static_cast<ChromaSamplePosition>(gb.bits(2)) : CHR_UNKNOWN;
  }
  if (strict_ && hdr.mtrx == MC_IDENTITY && hdr.layout != PIXEL_LAYOUT_I444) return fail(ObuError::Syntax);
  if (!hdr.monochrome) hdr.separate_uv_delta_q = static_cast<uint8_t>(gb.bit());

  hdr.film_grain_present = static_cast<uint8_t>(gb.bit());

  if (!check_trailing_bits(gb, strict_)) return fail(gb.error() ? ObuError::Truncated : ObuError::Syntax);
  return true;
}

// ---- frame header -------------------------------------------------------------------------------------------------------------------
bool ObuParser::read_frame_size(BitReader& gb, bool use_ref) {
  const SequenceHeader& seqhdr = seq_;
  FrameHeader& hdr = frame_;

  if (use_ref) {
    for (int i = 0; i < 7; i++) {
      if (gb.bit()) {
        const int idx = hdr.refidx[i];
        if (idx < 0 || idx >= NUM_REF_FRAMES || !refs_[idx].valid) return fail(ObuError::BadReference);
        const FrameHeader& ref = refs_[idx].hdr;
        hdr.width[1] = ref.width[1];
        hdr.height = ref.height;
        hdr.render_width = ref.render_width;
        hdr.render_height = ref.render_height;
        hdr.super_res.enabled = seqhdr.super_res && gb.bit();
        if (hdr.super_res.enabled) {
          const int d = hdr.super_res.width_scale_denominator = static_cast<uint8_t>(9 + gb.bits(3));
          hdr.width[0] = imax((hdr.width[1] * 8 + (d >> 1)) / d, imin(16, hdr.width[1]));
        } else {
          hdr.super_res.width_scale_denominator = 8;
          hdr.width[0] = hdr.width[1];
        }
        return true;
      }
    }
  }

  if (hdr.frame_size_override) {
    hdr.width[1] = static_cast<int>(gb.bits(seqhdr.width_n_bits)) + 1;
    hdr.height = static_cast<int>(gb.bits(seqhdr.height_n_bits)) + 1;
  } else {
    hdr.width[1] = seqhdr.max_width;
    hdr.height = seqhdr.max_height;
  }
  hdr.super_res.enabled = seqhdr.super_res && gb.bit();
  if (hdr.super_res.enabled) {
    const int d = hdr.super_res.width_scale_denominator = static_cast<uint8_t>(9 + gb.bits(3));
    hdr.width[0] = imax((hdr.width[1] * 8 + (d >> 1)) / d, imin(16, hdr.width[1]));
  } else {
    hdr.super_res.width_scale_denominator = 8;
    hdr.width[0] = hdr.width[1];
  }
  hdr.have_render_size = static_cast<uint8_t>(gb.bit());
  if (hdr.have_render_size) {
    hdr.render_width = static_cast<int>(gb.bits(16)) + 1;
    hdr.render_height = static_cast<int>(gb.bits(16)) + 1;
  } else {
    hdr.render_width = hdr.width[1];
    hdr.render_height = hdr.height;
  }
  return true;
}

bool ObuParser::parse_frame_hdr(BitReader& gb) {
  const SequenceHeader& seqhdr = seq_;
  FrameHeader& hdr = frame_;

  if (!seqhdr.reduced_still_picture_header) hdr.show_existing_frame = static_cast<uint8_t>(gb.bit());
  if (hdr.show_existing_frame) {
    hdr.existing_frame_idx = static_cast<uint8_t>(gb.bits(3));
    if (seqhdr.decoder_model_info_present && !seqhdr.equal_picture_interval)
      hdr.frame_presentation_delay = gb.bits(seqhdr.frame_presentation_delay_length);
    if (seqhdr.frame_id_numbers_present) {
      hdr.frame_id = gb.bits(seqhdr.frame_id_n_bits);
      const RefSlot& ref = refs_[hdr.existing_frame_idx];
      if (!ref.valid || ref.hdr.frame_id != hdr.frame_id) return fail(ObuError::BadReference);
    }
    return !gb.error() || fail(ObuError::Truncated);
  }

  if (seqhdr.reduced_still_picture_header) {
    hdr.frame_type = FRAME_TYPE_KEY;
    hdr.show_frame = 1;
  } else {
    hdr.frame_type = static_cast<FrameType>(gb.bits(2));
    hdr.show_frame = static_cast<uint8_t>(gb.bit());
  }
  if (hdr.show_frame) {
    if (seqhdr.decoder_model_info_present && !seqhdr.equal_picture_interval)
      hdr.frame_presentation_delay = gb.bits(seqhdr.frame_presentation_delay_length);
    hdr.showable_frame = hdr.frame_type != FRAME_TYPE_KEY;
  } else {
    hdr.showable_frame = static_cast<uint8_t>(gb.bit());
  }
  hdr.error_resilient_mode = (hdr.frame_type == FRAME_TYPE_KEY && hdr.show_frame) || hdr.frame_type == FRAME_TYPE_SWITCH ||
                             seqhdr.reduced_still_picture_header || gb.bit();
  hdr.disable_cdf_update = static_cast<uint8_t>(gb.bit());
  hdr.allow_screen_content_tools =
      seqhdr.screen_content_tools == ADAPTIVE ? static_cast<uint8_t>(gb.bit()) : static_cast<uint8_t>(seqhdr.screen_content_tools);
  if (hdr.allow_screen_content_tools)
    hdr.force_integer_mv = seqhdr.force_integer_mv == ADAPTIVE ? static_cast<uint8_t>(gb.bit()) : static_cast<uint8_t>(seqhdr.force_integer_mv);

  if (is_key_or_intra(hdr)) hdr.force_integer_mv = 1;

  if (seqhdr.frame_id_numbers_present) hdr.frame_id = gb.bits(seqhdr.frame_id_n_bits);

  if (!seqhdr.reduced_still_picture_header) hdr.frame_size_override = hdr.frame_type == FRAME_TYPE_SWITCH ? 1 : static_cast<uint8_t>(gb.bit());
  if (seqhdr.order_hint) hdr.frame_offset = static_cast<uint8_t>(gb.bits(seqhdr.order_hint_n_bits));
  hdr.primary_ref_frame = !hdr.error_resilient_mode && is_inter_or_switch(hdr) ? static_cast<uint8_t>(gb.bits(3)) : PRIMARY_REF_NONE;

  if (seqhdr.decoder_model_info_present) {
    hdr.buffer_removal_time_present = static_cast<uint8_t>(gb.bit());
    if (hdr.buffer_removal_time_present) {
      for (int i = 0; i < seqhdr.num_operating_points; i++) {
        const SequenceHeader::SequenceHeaderOperatingPoint& seqop = seqhdr.operating_points[i];
        FrameHeader::FrameHeaderOperatingPoint& op = hdr.operating_points[i];
        if (seqop.decoder_model_param_present) {
          const int in_temporal_layer = (seqop.idc >> hdr.temporal_id) & 1;
          const int in_spatial_layer = (seqop.idc >> (hdr.spatial_id + 8)) & 1;
          if (!seqop.idc || (in_temporal_layer && in_spatial_layer)) op.buffer_removal_time = gb.bits(seqhdr.buffer_removal_delay_length);
        }
      }
    }
  }

  if (is_key_or_intra(hdr)) {
    hdr.refresh_frame_flags = (hdr.frame_type == FRAME_TYPE_KEY && hdr.show_frame) ? 0xff : static_cast<uint8_t>(gb.bits(8));
    if (hdr.refresh_frame_flags != 0xff && hdr.error_resilient_mode && seqhdr.order_hint)
      for (int i = 0; i < 8; i++) gb.bits(seqhdr.order_hint_n_bits);
    if (strict_ && hdr.frame_type == FRAME_TYPE_INTRA && hdr.refresh_frame_flags == 0xff) return fail(ObuError::Syntax);
    if (!read_frame_size(gb, false)) return false;
    if (hdr.allow_screen_content_tools && !hdr.super_res.enabled) hdr.allow_intrabc = static_cast<uint8_t>(gb.bit());
  } else {
    hdr.refresh_frame_flags = hdr.frame_type == FRAME_TYPE_SWITCH ? 0xff : static_cast<uint8_t>(gb.bits(8));
    if (hdr.error_resilient_mode && seqhdr.order_hint)
      for (int i = 0; i < 8; i++) gb.bits(seqhdr.order_hint_n_bits);
    if (seqhdr.order_hint) {
      hdr.frame_ref_short_signaling = static_cast<uint8_t>(gb.bit());
      if (hdr.frame_ref_short_signaling) {
        hdr.refidx[0] = static_cast<int8_t>(gb.bits(3));
        hdr.refidx[1] = hdr.refidx[2] = -1;
        hdr.refidx[3] = static_cast<int8_t>(gb.bits(3));

        // frame_offset[-1] is a dump for the unconditional stores of unused values.
        int frame_offset_mem[8 + 1];
        auto fo = [&](int index) -> int& { return frame_offset_mem[index + 1]; };
        int earliest_ref = -1;
        {
          int earliest_offset = 2147483647;
          for (int i = 0; i < 8; i++) {
            if (!refs_[i].valid) return fail(ObuError::BadReference);
            const int diff = get_poc_diff(seqhdr.order_hint_n_bits, refs_[i].hdr.frame_offset, hdr.frame_offset);
            fo(i) = diff;
            if (diff < earliest_offset) {
              earliest_offset = diff;
              earliest_ref = i;
            }
          }
        }
        fo(hdr.refidx[0]) = kIntMin;  // = reference frame is used
        fo(hdr.refidx[3]) = kIntMin;

        int refidx = -1;
        {
          int latest_offset = 0;
          for (int i = 0; i < 8; i++) {
            const int hint = fo(i);
            if (hint >= latest_offset) {
              latest_offset = hint;
              refidx = i;
            }
          }
        }
        fo(refidx) = kIntMin;
        hdr.refidx[6] = static_cast<int8_t>(refidx);

        for (int i = 4; i < 6; i++) {
          // Unsigned compares to handle negative values.
          unsigned earliest_offset = 255;
          refidx = -1;
          for (int j = 0; j < 8; j++) {
            const unsigned hint = static_cast<unsigned>(fo(j));
            if (hint < earliest_offset) {
              earliest_offset = hint;
              refidx = j;
            }
          }
          fo(refidx) = kIntMin;
          hdr.refidx[i] = static_cast<int8_t>(refidx);
        }

        for (int i = 1; i < 7; i++) {
          refidx = hdr.refidx[i];
          if (refidx < 0) {
            unsigned latest_offset = ~255u;
            for (int j = 0; j < 8; j++) {
              const unsigned hint = static_cast<unsigned>(fo(j));
              if (hint >= latest_offset) {
                latest_offset = hint;
                refidx = j;
              }
            }
            fo(refidx) = kIntMin;
            hdr.refidx[i] = static_cast<int8_t>(refidx >= 0 ? refidx : earliest_ref);
          }
        }
      }
    }
    for (int i = 0; i < 7; i++) {
      if (!hdr.frame_ref_short_signaling) hdr.refidx[i] = static_cast<int8_t>(gb.bits(3));
      if (seqhdr.frame_id_numbers_present) {
        const unsigned delta_ref_frame_id = gb.bits(seqhdr.delta_frame_id_n_bits) + 1;
        const unsigned ref_frame_id = (hdr.frame_id + (1u << seqhdr.frame_id_n_bits) - delta_ref_frame_id) & ((1u << seqhdr.frame_id_n_bits) - 1);
        const RefSlot& ref = refs_[hdr.refidx[i]];
        if (!ref.valid || ref.hdr.frame_id != ref_frame_id) return fail(ObuError::BadReference);
      }
    }
    const bool use_ref = !hdr.error_resilient_mode && hdr.frame_size_override;
    if (!read_frame_size(gb, use_ref)) return false;
    if (!hdr.force_integer_mv) hdr.hp = static_cast<uint8_t>(gb.bit());
    hdr.subpel_filter_mode = gb.bit() ? FILTER_SWITCHABLE : static_cast<FilterMode>(gb.bits(2));
    hdr.switchable_motion_mode = static_cast<uint8_t>(gb.bit());
    if (!hdr.error_resilient_mode && seqhdr.ref_frame_mvs && seqhdr.order_hint && is_inter_or_switch(hdr))
      hdr.use_ref_frame_mvs = static_cast<uint8_t>(gb.bit());
  }

  if (!seqhdr.reduced_still_picture_header && !hdr.disable_cdf_update) hdr.refresh_context = !gb.bit();

  // tile data
  hdr.tiling.uniform = static_cast<uint8_t>(gb.bit());
  const int sbsz_min1 = (64 << seqhdr.sb128) - 1;
  const int sbsz_log2 = 6 + seqhdr.sb128;
  const int sbw = (hdr.width[0] + sbsz_min1) >> sbsz_log2;
  const int sbh = (hdr.height + sbsz_min1) >> sbsz_log2;
  const int max_tile_width_sb = 4096 >> sbsz_log2;
  const int max_tile_area_sb = 4096 * 2304 >> (2 * sbsz_log2);
  hdr.tiling.min_log2_cols = static_cast<uint8_t>(tile_log2(max_tile_width_sb, sbw));
  hdr.tiling.max_log2_cols = static_cast<uint8_t>(tile_log2(1, imin(sbw, MAX_TILE_COLS)));
  hdr.tiling.max_log2_rows = static_cast<uint8_t>(tile_log2(1, imin(sbh, MAX_TILE_ROWS)));
  const int min_log2_tiles = imax(tile_log2(max_tile_area_sb, sbw * sbh), hdr.tiling.min_log2_cols);
  if (hdr.tiling.uniform) {
    for (hdr.tiling.log2_cols = hdr.tiling.min_log2_cols; hdr.tiling.log2_cols < hdr.tiling.max_log2_cols && gb.bit(); hdr.tiling.log2_cols++) {
    }
    const int tile_w = 1 + ((sbw - 1) >> hdr.tiling.log2_cols);
    hdr.tiling.cols = 0;
    for (int sbx = 0; sbx < sbw; sbx += tile_w, hdr.tiling.cols++) {
      if (hdr.tiling.cols >= MAX_TILE_COLS) return fail(ObuError::Syntax);
      hdr.tiling.col_start_sb[hdr.tiling.cols] = static_cast<uint16_t>(sbx);
    }
    hdr.tiling.min_log2_rows = static_cast<uint8_t>(imax(min_log2_tiles - hdr.tiling.log2_cols, 0));

    for (hdr.tiling.log2_rows = hdr.tiling.min_log2_rows; hdr.tiling.log2_rows < hdr.tiling.max_log2_rows && gb.bit(); hdr.tiling.log2_rows++) {
    }
    const int tile_h = 1 + ((sbh - 1) >> hdr.tiling.log2_rows);
    hdr.tiling.rows = 0;
    for (int sby = 0; sby < sbh; sby += tile_h, hdr.tiling.rows++) {
      if (hdr.tiling.rows >= MAX_TILE_ROWS) return fail(ObuError::Syntax);
      hdr.tiling.row_start_sb[hdr.tiling.rows] = static_cast<uint16_t>(sby);
    }
  } else {
    hdr.tiling.cols = 0;
    int widest_tile = 0, max_tile_area_sb2 = sbw * sbh;
    for (int sbx = 0; sbx < sbw && hdr.tiling.cols < MAX_TILE_COLS; hdr.tiling.cols++) {
      const int tile_width_sb = imin(sbw - sbx, max_tile_width_sb);
      const int tile_w = (tile_width_sb > 1) ? 1 + static_cast<int>(gb.uniform(static_cast<unsigned>(tile_width_sb))) : 1;
      hdr.tiling.col_start_sb[hdr.tiling.cols] = static_cast<uint16_t>(sbx);
      sbx += tile_w;
      widest_tile = imax(widest_tile, tile_w);
    }
    hdr.tiling.log2_cols = static_cast<uint8_t>(tile_log2(1, hdr.tiling.cols));
    if (min_log2_tiles) max_tile_area_sb2 >>= min_log2_tiles + 1;
    const int max_tile_height_sb = imax(max_tile_area_sb2 / widest_tile, 1);

    hdr.tiling.rows = 0;
    for (int sby = 0; sby < sbh && hdr.tiling.rows < MAX_TILE_ROWS; hdr.tiling.rows++) {
      const int tile_height_sb = imin(sbh - sby, max_tile_height_sb);
      const int tile_h = (tile_height_sb > 1) ? 1 + static_cast<int>(gb.uniform(static_cast<unsigned>(tile_height_sb))) : 1;
      hdr.tiling.row_start_sb[hdr.tiling.rows] = static_cast<uint16_t>(sby);
      sby += tile_h;
    }
    hdr.tiling.log2_rows = static_cast<uint8_t>(tile_log2(1, hdr.tiling.rows));
  }
  hdr.tiling.col_start_sb[hdr.tiling.cols] = static_cast<uint16_t>(sbw);
  hdr.tiling.row_start_sb[hdr.tiling.rows] = static_cast<uint16_t>(sbh);
  if (hdr.tiling.log2_cols || hdr.tiling.log2_rows) {
    hdr.tiling.update = static_cast<uint16_t>(gb.bits(hdr.tiling.log2_cols + hdr.tiling.log2_rows));
    if (hdr.tiling.update >= hdr.tiling.cols * hdr.tiling.rows) return fail(ObuError::Syntax);
    hdr.tiling.n_bytes = static_cast<uint8_t>(gb.bits(2) + 1);
  }

  // quant data
  hdr.quant.yac = static_cast<uint8_t>(gb.bits(8));
  if (gb.bit()) hdr.quant.ydc_delta = static_cast<int8_t>(gb.sbits(7));
  if (!seqhdr.monochrome) {
    // If the sequence header says that delta_q might be different for U, V, we must check whether it actually is for this frame.
    const unsigned diff_uv_delta = seqhdr.separate_uv_delta_q ? gb.bit() : 0;
    if (gb.bit()) hdr.quant.udc_delta = static_cast<int8_t>(gb.sbits(7));
    if (gb.bit()) hdr.quant.uac_delta = static_cast<int8_t>(gb.sbits(7));
    if (diff_uv_delta) {
      if (gb.bit()) hdr.quant.vdc_delta = static_cast<int8_t>(gb.sbits(7));
      if (gb.bit()) hdr.quant.vac_delta = static_cast<int8_t>(gb.sbits(7));
    } else {
      hdr.quant.vdc_delta = hdr.quant.udc_delta;
      hdr.quant.vac_delta = hdr.quant.uac_delta;
    }
  }
  hdr.quant.qm = static_cast<uint8_t>(gb.bit());
  if (hdr.quant.qm) {
    hdr.quant.qm_y = static_cast<uint8_t>(gb.bits(4));
    hdr.quant.qm_u = static_cast<uint8_t>(gb.bits(4));
    hdr.quant.qm_v = seqhdr.separate_uv_delta_q ? static_cast<uint8_t>(gb.bits(4)) : hdr.quant.qm_u;
  }

  // segmentation data
  hdr.segmentation.enabled = static_cast<uint8_t>(gb.bit());
  if (hdr.segmentation.enabled) {
    if (hdr.primary_ref_frame == PRIMARY_REF_NONE) {
      hdr.segmentation.update_map = 1;
      hdr.segmentation.update_data = 1;
    } else {
      hdr.segmentation.update_map = static_cast<uint8_t>(gb.bit());
      if (hdr.segmentation.update_map) hdr.segmentation.temporal = static_cast<uint8_t>(gb.bit());
      hdr.segmentation.update_data = static_cast<uint8_t>(gb.bit());
    }

    if (hdr.segmentation.update_data) {
      hdr.segmentation.seg_data.last_active_segid = -1;
      for (int i = 0; i < MAX_SEGMENTS; i++) {
        SegmentationData& seg = hdr.segmentation.seg_data.d[i];
        if (gb.bit()) {
          seg.delta_q = static_cast<int16_t>(gb.sbits(9));
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
        }
        if (gb.bit()) {
          seg.delta_lf_y_v = static_cast<int8_t>(gb.sbits(7));
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
        }
        if (gb.bit()) {
          seg.delta_lf_y_h = static_cast<int8_t>(gb.sbits(7));
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
        }
        if (gb.bit()) {
          seg.delta_lf_u = static_cast<int8_t>(gb.sbits(7));
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
        }
        if (gb.bit()) {
          seg.delta_lf_v = static_cast<int8_t>(gb.sbits(7));
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
        }
        if (gb.bit()) {
          seg.ref = static_cast<int8_t>(gb.bits(3));
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
          hdr.segmentation.seg_data.preskip = 1;
        } else {
          seg.ref = -1;
        }
        if ((seg.skip = static_cast<uint8_t>(gb.bit()))) {
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
          hdr.segmentation.seg_data.preskip = 1;
        }
        if ((seg.globalmv = static_cast<uint8_t>(gb.bit()))) {
          hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
          hdr.segmentation.seg_data.preskip = 1;
        }
      }
    } else {
      // segmentation.update_data was false so we should copy segmentation data from the reference frame.
      const int pri_ref = hdr.refidx[hdr.primary_ref_frame];
      if (!refs_[pri_ref].valid) return fail(ObuError::BadReference);
      hdr.segmentation.seg_data = refs_[pri_ref].hdr.segmentation.seg_data;
    }
  } else {
    for (int i = 0; i < MAX_SEGMENTS; i++) hdr.segmentation.seg_data.d[i].ref = -1;
  }

  // delta q
  if (hdr.quant.yac) {
    hdr.delta.q.present = static_cast<uint8_t>(gb.bit());
    if (hdr.delta.q.present) {
      hdr.delta.q.res_log2 = static_cast<uint8_t>(gb.bits(2));
      if (!hdr.allow_intrabc) {
        hdr.delta.lf.present = static_cast<uint8_t>(gb.bit());
        if (hdr.delta.lf.present) {
          hdr.delta.lf.res_log2 = static_cast<uint8_t>(gb.bits(2));
          hdr.delta.lf.multi = static_cast<uint8_t>(gb.bit());
        }
      }
    }
  }

  // derive lossless flags
  const bool delta_lossless = !hdr.quant.ydc_delta && !hdr.quant.udc_delta && !hdr.quant.uac_delta && !hdr.quant.vdc_delta && !hdr.quant.vac_delta;
  hdr.all_lossless = 1;
  for (int i = 0; i < MAX_SEGMENTS; i++) {
    hdr.segmentation.qidx[i] =
        hdr.segmentation.enabled ? static_cast<uint8_t>(iclip_u8(hdr.quant.yac + hdr.segmentation.seg_data.d[i].delta_q)) : hdr.quant.yac;
    hdr.segmentation.lossless[i] = !hdr.segmentation.qidx[i] && delta_lossless;
    hdr.all_lossless &= hdr.segmentation.lossless[i];
  }

  // loopfilter
  if (hdr.all_lossless || hdr.allow_intrabc) {
    hdr.loopfilter.mode_ref_delta_enabled = 1;
    hdr.loopfilter.mode_ref_delta_update = 1;
    hdr.loopfilter.mode_ref_deltas = k_default_mode_ref_deltas;
  } else {
    hdr.loopfilter.level_y[0] = static_cast<uint8_t>(gb.bits(6));
    hdr.loopfilter.level_y[1] = static_cast<uint8_t>(gb.bits(6));
    if (!seqhdr.monochrome && (hdr.loopfilter.level_y[0] || hdr.loopfilter.level_y[1])) {
      hdr.loopfilter.level_u = static_cast<uint8_t>(gb.bits(6));
      hdr.loopfilter.level_v = static_cast<uint8_t>(gb.bits(6));
    }
    hdr.loopfilter.sharpness = static_cast<uint8_t>(gb.bits(3));

    if (hdr.primary_ref_frame == PRIMARY_REF_NONE) {
      hdr.loopfilter.mode_ref_deltas = k_default_mode_ref_deltas;
    } else {
      const int ref = hdr.refidx[hdr.primary_ref_frame];
      if (!refs_[ref].valid) return fail(ObuError::BadReference);
      hdr.loopfilter.mode_ref_deltas = refs_[ref].hdr.loopfilter.mode_ref_deltas;
    }
    hdr.loopfilter.mode_ref_delta_enabled = static_cast<uint8_t>(gb.bit());
    if (hdr.loopfilter.mode_ref_delta_enabled) {
      hdr.loopfilter.mode_ref_delta_update = static_cast<uint8_t>(gb.bit());
      if (hdr.loopfilter.mode_ref_delta_update) {
        for (int i = 0; i < 8; i++)
          if (gb.bit()) hdr.loopfilter.mode_ref_deltas.ref_delta[i] = static_cast<int8_t>(gb.sbits(7));
        for (int i = 0; i < 2; i++)
          if (gb.bit()) hdr.loopfilter.mode_ref_deltas.mode_delta[i] = static_cast<int8_t>(gb.sbits(7));
      }
    }
  }

  // cdef
  if (!hdr.all_lossless && seqhdr.cdef && !hdr.allow_intrabc) {
    hdr.cdef.damping = static_cast<uint8_t>(gb.bits(2) + 3);
    hdr.cdef.n_bits = static_cast<uint8_t>(gb.bits(2));
    for (int i = 0; i < (1 << hdr.cdef.n_bits); i++) {
      hdr.cdef.y_strength[i] = static_cast<uint8_t>(gb.bits(6));
      if (!seqhdr.monochrome) hdr.cdef.uv_strength[i] = static_cast<uint8_t>(gb.bits(6));
    }
  }

  // restoration
  if ((!hdr.all_lossless || hdr.super_res.enabled) && seqhdr.restoration && !hdr.allow_intrabc) {
    hdr.restoration.type[0] = static_cast<RestorationType>(gb.bits(2));
    if (!seqhdr.monochrome) {
      hdr.restoration.type[1] = static_cast<RestorationType>(gb.bits(2));
      hdr.restoration.type[2] = static_cast<RestorationType>(gb.bits(2));
    }

    if (hdr.restoration.type[0] || hdr.restoration.type[1] || hdr.restoration.type[2]) {
      // Log2 of the restoration unit size.
      hdr.restoration.unit_size[0] = static_cast<uint8_t>(6 + seqhdr.sb128);
      if (gb.bit()) {
        hdr.restoration.unit_size[0]++;
        if (!seqhdr.sb128) hdr.restoration.unit_size[0] = static_cast<uint8_t>(hdr.restoration.unit_size[0] + gb.bit());
      }
      hdr.restoration.unit_size[1] = hdr.restoration.unit_size[0];
      if ((hdr.restoration.type[1] || hdr.restoration.type[2]) && seqhdr.ss_hor == 1 && seqhdr.ss_ver == 1)
        hdr.restoration.unit_size[1] = static_cast<uint8_t>(hdr.restoration.unit_size[1] - gb.bit());
    } else {
      hdr.restoration.unit_size[0] = 8;
    }
  }

  if (!hdr.all_lossless) hdr.txfm_mode = gb.bit() ? TX_SWITCHABLE : TX_LARGEST;
  if (is_inter_or_switch(hdr)) hdr.switchable_comp_refs = static_cast<uint8_t>(gb.bit());
  if (hdr.switchable_comp_refs && is_inter_or_switch(hdr) && seqhdr.order_hint) {
    const int poc = hdr.frame_offset;
    int off_before = -1, off_after = -1;
    int off_before_idx = 0, off_after_idx = 0;
    for (int i = 0; i < 7; i++) {
      if (!refs_[hdr.refidx[i]].valid) return fail(ObuError::BadReference);
      const int refpoc = refs_[hdr.refidx[i]].hdr.frame_offset;

      const int diff = get_poc_diff(seqhdr.order_hint_n_bits, refpoc, poc);
      if (diff > 0) {
        if (off_after < 0 || get_poc_diff(seqhdr.order_hint_n_bits, off_after, refpoc) > 0) {
          off_after = refpoc;
          off_after_idx = i;
        }
      } else if (diff < 0 && (off_before < 0 || get_poc_diff(seqhdr.order_hint_n_bits, refpoc, off_before) > 0)) {
        off_before = refpoc;
        off_before_idx = i;
      }
    }

    if ((off_before | off_after) >= 0) {
      hdr.skip_mode_refs[0] = static_cast<int8_t>(imin(off_before_idx, off_after_idx));
      hdr.skip_mode_refs[1] = static_cast<int8_t>(imax(off_before_idx, off_after_idx));
      hdr.skip_mode_allowed = 1;
    } else if (off_before >= 0) {
      int off_before2 = -1;
      int off_before2_idx = 0;
      for (int i = 0; i < 7; i++) {
        if (!refs_[hdr.refidx[i]].valid) return fail(ObuError::BadReference);
        const int refpoc = refs_[hdr.refidx[i]].hdr.frame_offset;
        if (get_poc_diff(seqhdr.order_hint_n_bits, refpoc, off_before) < 0) {
          if (off_before2 < 0 || get_poc_diff(seqhdr.order_hint_n_bits, refpoc, off_before2) > 0) {
            off_before2 = refpoc;
            off_before2_idx = i;
          }
        }
      }

      if (off_before2 >= 0) {
        hdr.skip_mode_refs[0] = static_cast<int8_t>(imin(off_before_idx, off_before2_idx));
        hdr.skip_mode_refs[1] = static_cast<int8_t>(imax(off_before_idx, off_before2_idx));
        hdr.skip_mode_allowed = 1;
      }
    }
  }
  if (hdr.skip_mode_allowed) hdr.skip_mode_enabled = static_cast<uint8_t>(gb.bit());
  if (!hdr.error_resilient_mode && is_inter_or_switch(hdr) && seqhdr.warped_motion) hdr.warp_motion = static_cast<uint8_t>(gb.bit());
  hdr.reduced_txtp_set = static_cast<uint8_t>(gb.bit());

  for (int i = 0; i < 7; i++) hdr.gmv[i] = default_wm_params();

  if (is_inter_or_switch(hdr)) {
    const WarpedMotionParams default_gmv = default_wm_params();
    for (int i = 0; i < 7; i++) {
      hdr.gmv[i].type = !gb.bit() ? WM_TYPE_IDENTITY : gb.bit() ? WM_TYPE_ROT_ZOOM : gb.bit() ? WM_TYPE_TRANSLATION : WM_TYPE_AFFINE;

      if (hdr.gmv[i].type == WM_TYPE_IDENTITY) continue;

      const WarpedMotionParams* ref_gmv;
      if (hdr.primary_ref_frame == PRIMARY_REF_NONE) {
        ref_gmv = &default_gmv;
      } else {
        const int pri_ref = hdr.refidx[hdr.primary_ref_frame];
        if (!refs_[pri_ref].valid) return fail(ObuError::BadReference);
        ref_gmv = &refs_[pri_ref].hdr.gmv[i];
      }
      int32_t* const mat = hdr.gmv[i].matrix;
      const int32_t* const ref_mat = ref_gmv->matrix;
      int bits, shift;

      if (hdr.gmv[i].type >= WM_TYPE_ROT_ZOOM) {
        mat[2] = (1 << 16) + 2 * gb.subexp((ref_mat[2] - (1 << 16)) >> 1, 12);
        mat[3] = 2 * gb.subexp(ref_mat[3] >> 1, 12);

        bits = 12;
        shift = 10;
      } else {
        bits = 9 - !hdr.hp;
        shift = 13 + !hdr.hp;
      }

      if (hdr.gmv[i].type == WM_TYPE_AFFINE) {
        mat[4] = 2 * gb.subexp(ref_mat[4] >> 1, 12);
        mat[5] = (1 << 16) + 2 * gb.subexp((ref_mat[5] - (1 << 16)) >> 1, 12);
      } else {
        mat[4] = -mat[3];
        mat[5] = mat[2];
      }

      mat[0] = gb.subexp(ref_mat[0] >> shift, bits) * (1 << shift);
      mat[1] = gb.subexp(ref_mat[1] >> shift, bits) * (1 << shift);
    }
  }

  if (seqhdr.film_grain_present && (hdr.show_frame || hdr.showable_frame)) {
    hdr.film_grain.present = static_cast<uint8_t>(gb.bit());
    if (hdr.film_grain.present) {
      const unsigned seed = gb.bits(16);
      hdr.film_grain.update = hdr.frame_type != FRAME_TYPE_INTER || gb.bit();
      if (!hdr.film_grain.update) {
        const int refidx = static_cast<int>(gb.bits(3));
        int i;
        for (i = 0; i < 7; i++)
          if (hdr.refidx[i] == refidx) break;
        if (i == 7 || !refs_[refidx].valid) return fail(ObuError::BadReference);
        hdr.film_grain.data = refs_[refidx].hdr.film_grain.data;
        hdr.film_grain.data.seed = seed;
      } else {
        FilmGrainData& fgd = hdr.film_grain.data;
        fgd.seed = seed;

        fgd.num_y_points = static_cast<int>(gb.bits(4));
        if (fgd.num_y_points > 14) return fail(ObuError::Syntax);
        for (int i = 0; i < fgd.num_y_points; i++) {
          fgd.y_points[i][0] = static_cast<uint8_t>(gb.bits(8));
          if (i && fgd.y_points[i - 1][0] >= fgd.y_points[i][0]) return fail(ObuError::Syntax);
          fgd.y_points[i][1] = static_cast<uint8_t>(gb.bits(8));
        }

        if (!seqhdr.monochrome) fgd.chroma_scaling_from_luma = static_cast<int>(gb.bit());
        if (seqhdr.monochrome || fgd.chroma_scaling_from_luma || (seqhdr.ss_ver == 1 && seqhdr.ss_hor == 1 && !fgd.num_y_points)) {
          fgd.num_uv_points[0] = fgd.num_uv_points[1] = 0;
        } else {
          for (int pl = 0; pl < 2; pl++) {
            fgd.num_uv_points[pl] = static_cast<int>(gb.bits(4));
            if (fgd.num_uv_points[pl] > 10) return fail(ObuError::Syntax);
            for (int i = 0; i < fgd.num_uv_points[pl]; i++) {
              fgd.uv_points[pl][i][0] = static_cast<uint8_t>(gb.bits(8));
              if (i && fgd.uv_points[pl][i - 1][0] >= fgd.uv_points[pl][i][0]) return fail(ObuError::Syntax);
              fgd.uv_points[pl][i][1] = static_cast<uint8_t>(gb.bits(8));
            }
          }
        }

        if (seqhdr.ss_hor == 1 && seqhdr.ss_ver == 1 && !!fgd.num_uv_points[0] != !!fgd.num_uv_points[1]) return fail(ObuError::Syntax);

        fgd.scaling_shift = static_cast<int>(gb.bits(2)) + 8;
        fgd.ar_coeff_lag = static_cast<int>(gb.bits(2));
        const int num_y_pos = 2 * fgd.ar_coeff_lag * (fgd.ar_coeff_lag + 1);
        if (fgd.num_y_points)
          for (int i = 0; i < num_y_pos; i++) fgd.ar_coeffs_y[i] = static_cast<int8_t>(static_cast<int>(gb.bits(8)) - 128);
        for (int pl = 0; pl < 2; pl++)
          if (fgd.num_uv_points[pl] || fgd.chroma_scaling_from_luma) {
            const int num_uv_pos = num_y_pos + !!fgd.num_y_points;
            for (int i = 0; i < num_uv_pos; i++) fgd.ar_coeffs_uv[pl][i] = static_cast<int8_t>(static_cast<int>(gb.bits(8)) - 128);
            if (!fgd.num_y_points) fgd.ar_coeffs_uv[pl][num_uv_pos] = 0;
          }
        fgd.ar_coeff_shift = gb.bits(2) + 6;
        fgd.grain_scale_shift = static_cast<int>(gb.bits(2));
        for (int pl = 0; pl < 2; pl++)
          if (fgd.num_uv_points[pl]) {
            fgd.uv_mult[pl] = static_cast<int>(gb.bits(8)) - 128;
            fgd.uv_luma_mult[pl] = static_cast<int>(gb.bits(8)) - 128;
            fgd.uv_offset[pl] = static_cast<int>(gb.bits(9)) - 256;
          }
        fgd.overlap_flag = static_cast<int>(gb.bit());
        fgd.clip_to_restricted_range = static_cast<int>(gb.bit());
      }
    }
  }

  return true;
}

// What dav1d_submit_frame() checks before it takes the references of an inter frame.
bool ObuParser::check_references() {
  const FrameHeader& hdr = frame_;
  if (!is_inter_or_switch(hdr)) return true;
  if (hdr.primary_ref_frame != PRIMARY_REF_NONE) {
    const int pri_ref = hdr.refidx[hdr.primary_ref_frame];
    if (!refs_[pri_ref].valid) return fail(ObuError::BadReference);
  }
  const int bpc = 8 + 2 * seq_.hbd;
  for (int i = 0; i < 7; i++) {
    const RefSlot& ref = refs_[hdr.refidx[i]];
    if (!ref.valid || hdr.width[0] * 2 < ref.hdr.width[1] || hdr.height * 2 < ref.hdr.height || hdr.width[0] > ref.hdr.width[1] * 16 ||
        hdr.height > ref.hdr.height * 16 || seq_.layout != ref.layout || bpc != ref.bpc)
      return fail(ObuError::BadReference);
  }
  return true;
}

// ---- tile groups and the OBU loop ---------------------------------------------------------------------------------------------------
bool ObuParser::parse_tile_group(BitReader& gb, ConstByteView payload, ObuEvent& event) {
  const uint64_t total = static_cast<uint64_t>(frame_.tiling.cols) * frame_.tiling.rows;
  const bool have_tile_pos = total > 1 ? gb.bit() != 0 : false;
  uint64_t start = 0, end = total - 1;
  if (have_tile_pos) {
    const int n_bits = frame_.tiling.log2_cols + frame_.tiling.log2_rows;
    start = gb.bits(n_bits);
    end = gb.bits(n_bits);
  }
  gb.byte_align();
  if (gb.error()) return fail(ObuError::Truncated);

  // ensure tile groups are in order and sane, see 6.10.1
  if (start > end || start != n_tiles_ || end >= total) {
    n_tiles_ = 0;
    return fail(ObuError::TileOrder);
  }

  ConstByteView data = payload.subview(gb.byte_position());
  const uint64_t n_bytes = frame_.tiling.n_bytes;
  for (uint64_t j = start; j <= end; j++) {
    uint64_t tile_sz;
    if (j == end) {
      tile_sz = data.size();
    } else {
      if (n_bytes > data.size()) {
        n_tiles_ = 0;
        return fail(ObuError::TileOrder);
      }
      tile_sz = 0;
      for (uint64_t k = 0; k < n_bytes; k++) tile_sz |= static_cast<uint64_t>(data[k]) << (k * 8);
      tile_sz++;
      data = data.subview(n_bytes);
      if (tile_sz > data.size()) {
        n_tiles_ = 0;
        return fail(ObuError::TileOrder);
      }
    }
    TileData& tile = tiles_[j];
    tile.data = data.first(tile_sz);
    tile.tile_row = static_cast<uint16_t>(j / frame_.tiling.cols);
    tile.tile_col = static_cast<uint16_t>(j % frame_.tiling.cols);
    data = data.subview(tile_sz);
  }
  n_tiles_ += 1 + end - start;
  event.kind = n_tiles_ == total ? ObuEventKind::FrameReady : ObuEventKind::TileGroup;
  return true;
}

bool ObuParser::parse_metadata(BitReader& gb, ObuEvent& event) {
  const unsigned meta_type = gb.uleb128();
  if (gb.error()) return fail(ObuError::Truncated);
  switch (meta_type) {
    case OBU_META_HDR_CLL: {
      ContentLightLevel cll{};
      cll.max_content_light_level = static_cast<uint16_t>(gb.bits(16));
      cll.max_frame_average_light_level = static_cast<uint16_t>(gb.bits(16));
      if (!check_trailing_bits(gb, strict_)) return fail(gb.error() ? ObuError::Truncated : ObuError::Syntax);
      cll_ = cll;
      have_cll_ = true;
      event.kind = ObuEventKind::ContentLightLevel;
      break;
    }
    case OBU_META_HDR_MDCV: {
      MasteringDisplay md{};
      for (int i = 0; i < 3; i++) {
        md.primaries[i][0] = static_cast<uint16_t>(gb.bits(16));
        md.primaries[i][1] = static_cast<uint16_t>(gb.bits(16));
      }
      md.white_point[0] = static_cast<uint16_t>(gb.bits(16));
      md.white_point[1] = static_cast<uint16_t>(gb.bits(16));
      md.max_luminance = gb.bits(32);
      md.min_luminance = gb.bits(32);
      if (!check_trailing_bits(gb, strict_)) return fail(gb.error() ? ObuError::Truncated : ObuError::Syntax);
      mdcv_ = md;
      have_mdcv_ = true;
      event.kind = ObuEventKind::MasteringDisplay;
      break;
    }
    case OBU_META_ITUT_T35: {
      const ConstByteView all = gb.data();
      const int64_t base0 = static_cast<int64_t>(gb.byte_position());
      int64_t payload_size = static_cast<int64_t>(all.size()) - base0;
      // Don't take into account all the trailing bits for payload_size
      while (payload_size > 0 && !all[static_cast<uint64_t>(base0 + payload_size - 1)]) payload_size--;  // trailing_zero_bit x 8
      payload_size--;  // trailing_one_bit + trailing_zero_bit x 7

      uint8_t country_code_extension_byte = 0;
      const uint8_t country_code = static_cast<uint8_t>(gb.bits(8));
      payload_size--;
      if (country_code == 0xff) {
        country_code_extension_byte = static_cast<uint8_t>(gb.bits(8));
        payload_size--;
      }
      if (gb.error()) return fail(ObuError::Truncated);
      const int64_t base = static_cast<int64_t>(gb.byte_position());
      if (payload_size <= 0 || base + payload_size >= static_cast<int64_t>(all.size()) || all[static_cast<uint64_t>(base + payload_size)] != 0x80)
        break;  // malformed message: ignored
      event.kind = ObuEventKind::ItuT35;
      event.itut_t35_country_code = country_code;
      event.itut_t35_country_code_extension = country_code_extension_byte;
      event.itut_t35_payload = all.subview(static_cast<uint64_t>(base), static_cast<uint64_t>(payload_size));
      break;
    }
    default:
      break;  // scalability, timecode, unregistered user private and unknown types are ignored
  }
  return true;
}

bool ObuParser::handle_show_existing(ObuEvent& event) {
  const unsigned idx = frame_.existing_frame_idx;
  if (!refs_[idx].valid) return fail(ObuError::BadReference);
  if (strict_ && !refs_[idx].showable) return fail(ObuError::BadReference);
  event.kind = ObuEventKind::ShowExistingFrame;
  event.existing_frame_idx = static_cast<uint8_t>(idx);
  if (refs_[idx].hdr.frame_type == FRAME_TYPE_KEY) {
    event.refresh_all_refs = true;
    refs_[idx].showable = false;
    for (unsigned i = 0; i < NUM_REF_FRAMES; i++)
      if (i != idx) refs_[i] = refs_[idx];
  }
  return true;
}

bool ObuParser::parse_obu(ConstByteView data, uint64_t& consumed, ObuEvent& event) {
  event = ObuEvent();
  consumed = 0;
  error_ = ObuError::None;
  if (data.empty()) return fail(ObuError::Truncated);

  // obu header
  BitReader hb(data);
  const unsigned obu_forbidden_bit = hb.bit();
  if (strict_ && obu_forbidden_bit) return fail(ObuError::Syntax);
  const unsigned type = hb.bits(4);
  const bool has_extension = hb.bit() != 0;
  const bool has_length_field = hb.bit() != 0;
  hb.bit();  // reserved

  unsigned temporal_id = 0, spatial_id = 0;
  if (has_extension) {
    temporal_id = hb.bits(3);
    spatial_id = hb.bits(2);
    hb.bits(3);  // reserved
  }
  uint64_t payload_size;
  if (has_length_field) {
    const uint64_t len = hb.uleb128();
    if (hb.error()) return fail(ObuError::Truncated);
    if (len > data.size() - hb.byte_position()) return fail(ObuError::Truncated);
    payload_size = len;
  } else {
    if (hb.error()) return fail(ObuError::Truncated);
    payload_size = data.size() - hb.byte_position();
  }
  const uint64_t payload_offset = hb.byte_position();
  consumed = payload_offset + payload_size;
  const ConstByteView payload = data.subview(payload_offset, payload_size);

  // skip obu not belonging to the selected temporal/spatial layer
  if (type != OBU_SEQ_HDR && type != OBU_TD && has_extension && operating_point_idc_ != 0) {
    const unsigned in_temporal_layer = (operating_point_idc_ >> temporal_id) & 1;
    const unsigned in_spatial_layer = (operating_point_idc_ >> (spatial_id + 8)) & 1;
    if (!in_temporal_layer || !in_spatial_layer) return true;
  }

  // dav1d_init_get_bits() asserts on an empty OBU; the syntax elements below would then run into the end anyway.
  BitReader gb(payload);

  switch (type) {
    case OBU_SEQ_HDR: {
      SequenceHeader seq;
      if (!parse_seq_hdr(gb, seq)) return false;

      const unsigned op_idx = operating_point_ < seq.num_operating_points ? operating_point_ : 0;
      const unsigned idc = seq.operating_points[op_idx].idc;
      const unsigned spatial_mask = idc >> 8;
      unsigned max_spatial_id = 0;
      while (spatial_mask >> (max_spatial_id + 1)) ++max_spatial_id;

      // If we have read a sequence header which is different from the old one, this is a new video sequence and can't use any
      // previous state. Free that state.
      if (!have_seq_) {
        have_frame_ = frame_open_ = false;
        event.new_sequence = true;
      } else if (!same_sequence(seq, seq_)) {
        have_frame_ = frame_open_ = false;
        have_mdcv_ = have_cll_ = false;
        invalidate_refs();
        event.new_sequence = true;
      } else if (!same_operating_parameter_info(seq, seq_)) {
        event.op_params_changed = true;
      }
      operating_point_idc_ = idc;
      max_spatial_id_ = spatial_mask ? max_spatial_id : 0;
      seq_ = seq;
      have_seq_ = true;
      event.kind = ObuEventKind::SequenceHeader;
      return true;
    }
    case OBU_REDUNDANT_FRAME_HDR:
      if (frame_open_) return true;
      [[fallthrough]];
    case OBU_FRAME:
    case OBU_FRAME_HDR: {
      if (!have_seq_) return fail(ObuError::NoSequence);
      frame_ = FrameHeader{};
      have_frame_ = false;
      frame_open_ = false;
      frame_.temporal_id = static_cast<uint8_t>(temporal_id);
      frame_.spatial_id = static_cast<uint8_t>(spatial_id);
      if (!parse_frame_hdr(gb)) return false;
      if (!frame_.show_existing_frame && gb.error()) return fail(ObuError::Truncated);
      n_tiles_ = 0;
      if (type != OBU_FRAME) {
        // This is actually a frame header OBU so read the trailing bit and check for overrun.
        if (!check_trailing_bits(gb, strict_)) return fail(gb.error() ? ObuError::Truncated : ObuError::Syntax);
      }
      if (frame_size_limit_ && static_cast<int64_t>(frame_.width[1]) * frame_.height > static_cast<int64_t>(frame_size_limit_))
        return fail(ObuError::FrameSize);
      have_frame_ = true;
      if (frame_.show_existing_frame) {
        // OBU_FRAMEs shouldn't be signaled with show_existing_frame
        if (type == OBU_FRAME) return fail(ObuError::Syntax);
        return handle_show_existing(event);
      }
      if (!check_references()) return false;
      if (!reserve_tiles(static_cast<uint64_t>(frame_.tiling.cols) * frame_.tiling.rows)) return fail(ObuError::NoMemory);
      frame_open_ = true;
      event.kind = ObuEventKind::FrameHeader;
      if (type != OBU_FRAME) return true;

      // This is the frame header at the start of a frame OBU. There's no trailing bit at the end to skip, but we do need to align to the
      // next byte.
      gb.byte_align();
      break;
    }
    case OBU_TILE_GRP:
      break;
    case OBU_METADATA:
      return parse_metadata(gb, event);
    case OBU_TD:
      event.kind = ObuEventKind::TemporalDelimiter;
      return true;
    case OBU_PADDING:
    default:
      return true;
  }

  // OBU_FRAME or OBU_TILE_GRP
  if (!frame_open_) return fail(ObuError::NoSequence);
  if (!parse_tile_group(gb, payload, event)) {
    frame_open_ = false;
    return false;
  }
  if (event.kind == ObuEventKind::FrameReady) frame_open_ = false;
  return true;
}

}  // namespace av1
}  // namespace hardware
