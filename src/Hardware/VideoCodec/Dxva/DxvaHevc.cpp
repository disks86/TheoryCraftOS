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
// DXVA HEVC decoding (D3D11_DECODER_PROFILE_HEVC_VLD_MAIN) on the CPU H.265 decoder (Software/H265*.cpp).
//
// DXVA_PicParams_HEVC carries the sequence and picture parameter set fields a decoder needs, but not the short-term reference picture sets of the SPS that the slice
// headers name, nor the long-term pictures of the SPS: instead it gives the reference picture sets of the picture itself (RefPicList with the order counts, and the
// StCurrBefore / StCurrAfter / LtCurr sets as indexes into it), the picture's order count and the size of the set a slice header carries itself. So the parameter sets
// are made as structures from the parameters, and the H.265 core is driven "from outside" (h265::Core::external_*): it reads each slice segment header with those
// sets (skipping the slice's own short-term set by its size) and decodes the slice data, with the references being the pictures this decoder made earlier in the
// surfaces RefPicList names. Pictures are kept per surface key. The picture comes back at once, in decoding order, at the coded size (no conformance window), as NV12.
//
// Main profile only (8-bit 4:2:0: the core's sample paths are 8-bit): another chroma format or bit depth is Unsupported.
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesHevc.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace {

using dxva::field;
using h265::Picture;

constexpr uint8_t kNoEntry = 0xFF;

// The pictures of a surface key: the decoded picture (allocated on first use and kept for reuse) and whether it holds one.
struct HevcSurface {
  UniquePointer<Picture> picture;
  bool valid = false;
};

// 6.5.3: position i of the up-right diagonal scan of an n x n block (n = 4 or 8), as a raster index.
void diagonal_scan(int n, uint8_t* raster) {
  int i = 0, x = 0, y = 0;
  while (i < n * n) {
    while (y >= 0) {
      if (x < n && y < n) raster[i++] = static_cast<uint8_t>(y * n + x);
      --y;
      ++x;
    }
    y = x;
    x = 0;
  }
}

// The scaling lists of DXVA_Qmatrix_HEVC (coefficients in diagonal scan order) as the core keeps them (raster order).
void load_scaling(const dxva::QmatrixHevc& q, h265::ScalingList* s) {
  uint8_t scan4[16], scan8[64];
  diagonal_scan(4, scan4);
  diagonal_scan(8, scan8);
  s->set_default();
  for (int m = 0; m < 6; ++m) {
    for (int i = 0; i < 16; ++i) s->list[0][m][scan4[i]] = q.ucScalingLists0[m][i];
    for (int i = 0; i < 64; ++i) {
      s->list[1][m][scan8[i]] = q.ucScalingLists1[m][i];
      s->list[2][m][scan8[i]] = q.ucScalingLists2[m][i];
    }
    s->dc[2][m] = q.ucScalingListDCCoefSizeID2[m];
  }
  for (int k = 0; k < 2; ++k) {
    for (int i = 0; i < 64; ++i) s->list[3][k * 3][scan8[i]] = q.ucScalingLists3[k][i];
    s->dc[3][k * 3] = q.ucScalingListDCCoefSizeID3[k];
  }
  // The 32x32 chroma matrices (4:4:4 only) are those of 16x16, as the core's own parser makes them.
  const int chroma[4] = {1, 2, 4, 5};
  for (int m : chroma) {
    for (int i = 0; i < 64; ++i) s->list[3][m][i] = s->list[2][m][i];
    s->dc[3][m] = s->dc[2][m];
  }
}

class HevcDxvaDecoder final : public IDxvaDecoder {
 public:
  HevcDxvaDecoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config)
      : allocator_(allocator), config_(config), surfaces_(config.surfaces + 1), out_(allocator), chopped_(allocator) {}
  ~HevcDxvaDecoder() = default;

  [[nodiscard]] bool init() {
    core_ = UniquePointer<h265::Core>::make(allocator_, allocator_);
    return static_cast<bool>(core_);
  }

  DxvaProfile profile() const override { return DxvaProfile::HevcVldMain; }

  void reset() override {
    surfaces_.clear();
    surfaces_.for_each_state([](HevcSurface& s) { s.valid = false; });
  }

  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override;

 private:
  DxvaResult refuse(DxvaDecodedPicture* out, DxvaResult result, const char* why) {
    out->detail = why;
    return result;
  }
  // The parameter sets of the picture, from the picture parameters (and the matrices). Returns Ok, or what to refuse it with (the reason in *why).
  DxvaResult make_parameter_sets(const dxva::PicParamsHevc& pp, ConstByteView qm, h265::Sps* sps, h265::Pps* pps, const char** why);
  // The slice segments of the picture: each control's bytes, chopped pieces put together, split at start codes. False: a control is out of the bitstream.
  [[nodiscard]] bool run_slices(const DxvaPicture& picture, bool* damaged, const char** why);
  [[nodiscard]] bool output(const Picture& p, DxvaDecodedPicture* out);

  allocators::IAllocator& allocator_;
  DxvaDecoderConfig config_;
  UniquePointer<h265::Core> core_;
  dxva::SurfaceTable<HevcSurface> surfaces_;
  UniquePointer<Picture> grey_[15];
  UniquePointer<Picture> spare_;      // the picture decoded into when the output surface is also a reference   // stand-ins for references that are not there, one per RefPicList entry
  ByteBuffer out_;
  ByteBuffer chopped_;
};

DxvaResult HevcDxvaDecoder::make_parameter_sets(const dxva::PicParamsHevc& pp, ConstByteView qm, h265::Sps* sps_out, h265::Pps* pps_out, const char** why) {
  namespace tool = dxva::hevc_tool;
  namespace pic = dxva::hevc_pic;
  namespace fmt = dxva::hevc_format;
  const uint32_t format = pp.wFormatAndSequenceInfoFlags;
  const uint32_t tools = pp.dwCodingParamToolFlags;
  const uint32_t props = pp.dwCodingSettingPicturePropertyFlags;
  auto flag = [](uint32_t word, int bit) { return ((word >> bit) & 1) != 0; };
  if (field(format, fmt::kChromaFormatIdc, 2) != 1 || flag(format, fmt::kSeparateColourPlane)) {
    *why = "HEVC: only 4:2:0 is decoded (Main profile)";
    return DxvaResult::Unsupported;
  }
  if (field(format, fmt::kBitDepthLumaMinus8, 3) != 0 || field(format, fmt::kBitDepthChromaMinus8, 3) != 0) {
    *why = "HEVC: only 8-bit samples are decoded (Main profile)";
    return DxvaResult::Unsupported;
  }
  *why = "HEVC: picture parameters out of range";
  h265::Sps s;
  s.id = 0;
  s.max_sub_layers = 1;
  s.chroma_format_idc = 1;
  s.log2_max_poc_lsb = 4 + static_cast<int>(field(format, fmt::kLog2MaxPocLsbMinus4, 4));
  if (s.log2_max_poc_lsb > 16) return DxvaResult::Invalid;
  s.log2_min_cb = pp.log2_min_luma_coding_block_size_minus3 + 3;
  s.log2_ctb = s.log2_min_cb + pp.log2_diff_max_min_luma_coding_block_size;
  s.log2_min_tb = pp.log2_min_transform_block_size_minus2 + 2;
  s.log2_max_tb = s.log2_min_tb + pp.log2_diff_max_min_transform_block_size;
  s.max_th_depth_inter = pp.max_transform_hierarchy_depth_inter;
  s.max_th_depth_intra = pp.max_transform_hierarchy_depth_intra;
  // (the limits of 7.4.3.2.1, as the core's SPS parser checks them)
  if (s.log2_ctb < 4 || s.log2_ctb > 6 || s.log2_min_cb > s.log2_ctb || s.log2_min_tb >= s.log2_min_cb || s.log2_max_tb > 5 || s.log2_max_tb > s.log2_ctb) return DxvaResult::Invalid;
  if (s.max_th_depth_inter > s.log2_ctb - s.log2_min_tb || s.max_th_depth_intra > s.log2_ctb - s.log2_min_tb) return DxvaResult::Invalid;
  const uint32_t width = static_cast<uint32_t>(pp.PicWidthInMinCbsY) << s.log2_min_cb, height = static_cast<uint32_t>(pp.PicHeightInMinCbsY) << s.log2_min_cb;
  if (width == 0 || height == 0 || width > 8192 || height > 8192) return DxvaResult::Invalid;
  if (width > config_.width || height > config_.height) {
    *why = "HEVC: the picture is larger than the decoder's surfaces";
    return DxvaResult::Invalid;
  }
  s.width = static_cast<int>(width);
  s.height = static_cast<int>(height);
  s.bit_depth_luma = s.bit_depth_chroma = 8;
  s.scaling_list_enabled = flag(tools, tool::kScalingListEnabled);
  s.amp = flag(tools, tool::kAmp);
  s.sao = flag(tools, tool::kSao);
  s.pcm = flag(tools, tool::kPcm);
  if (s.pcm) {
    s.pcm_bit_depth_luma = static_cast<int>(field(tools, tool::kPcmBitDepthLumaMinus1, 4)) + 1;
    s.pcm_bit_depth_chroma = static_cast<int>(field(tools, tool::kPcmBitDepthChromaMinus1, 4)) + 1;
    s.log2_min_pcm = static_cast<int>(field(tools, tool::kLog2MinPcmMinus3, 2)) + 3;
    s.log2_max_pcm = s.log2_min_pcm + static_cast<int>(field(tools, tool::kLog2DiffMaxMinPcm, 2));
    s.pcm_loop_filter_disabled = flag(tools, tool::kPcmLoopFilterDisabled);
    if (s.pcm_bit_depth_luma > 8 || s.pcm_bit_depth_chroma > 8 || s.log2_max_pcm > 5 || s.log2_min_pcm < s.log2_min_cb) return DxvaResult::Invalid;
  }
  // The sets themselves come with each picture; the slice headers only need how many there are to read their index.
  if (pp.num_short_term_ref_pic_sets > 64 || pp.num_long_term_ref_pics_sps > 32) return DxvaResult::Invalid;
  s.num_st_rps = pp.num_short_term_ref_pic_sets;
  s.long_term_ref_pics_present = flag(tools, tool::kLongTermRefPicsPresent);
  s.num_lt_sps = s.long_term_ref_pics_present ? pp.num_long_term_ref_pics_sps : 0;
  s.temporal_mvp = flag(tools, tool::kSpsTemporalMvp);
  s.strong_intra_smoothing = flag(tools, tool::kStrongIntraSmoothing);
  s.scaling.set_default();
  if (s.scaling_list_enabled) {
    dxva::QmatrixHevc q;
    ByteReader in(qm);
    if (!in.read_into(bytes_of(q))) {
      *why = "HEVC: scaling lists are enabled but no (or a short) inverse quantization matrix buffer came";
      return DxvaResult::Invalid;
    }
    load_scaling(q, &s.scaling);
    for (int m = 0; m < 6; ++m)
      for (int size = 0; size < 4; ++size)
        for (int i = 0; i < (size == 0 ? 16 : 64); ++i)
          if (s.scaling.list[size][m][i] == 0 && (size < 3 || m == 0 || m == 3)) return DxvaResult::Invalid;   // (7.4.5: ScalingFactor is never 0)
  }
  s.ctb_size = 1 << s.log2_ctb;
  s.pic_w_ctb = (s.width + s.ctb_size - 1) >> s.log2_ctb;
  s.pic_h_ctb = (s.height + s.ctb_size - 1) >> s.log2_ctb;
  if (static_cast<int64_t>(s.pic_w_ctb) * s.pic_h_ctb > h265::kMaxCtbs) return DxvaResult::Invalid;
  s.valid = true;

  h265::Pps p;
  p.id = 0;
  p.sps_id = 0;
  p.dependent_slice_segments = flag(tools, tool::kDependentSliceSegments);
  p.output_flag_present = flag(tools, tool::kOutputFlagPresent);
  p.num_extra_slice_header_bits = static_cast<int>(field(tools, tool::kNumExtraSliceHeaderBits, 3));
  p.sign_data_hiding = flag(tools, tool::kSignDataHiding);
  p.cabac_init_present = flag(tools, tool::kCabacInitPresent);
  if (pp.num_ref_idx_l0_default_active_minus1 > 14 || pp.num_ref_idx_l1_default_active_minus1 > 14) return DxvaResult::Invalid;
  p.num_ref_idx_default[0] = pp.num_ref_idx_l0_default_active_minus1 + 1;
  p.num_ref_idx_default[1] = pp.num_ref_idx_l1_default_active_minus1 + 1;
  if (pp.init_qp_minus26 < -26 || pp.init_qp_minus26 > 25) return DxvaResult::Invalid;
  p.init_qp = 26 + pp.init_qp_minus26;
  p.constrained_intra_pred = flag(props, pic::kConstrainedIntraPred);
  p.transform_skip = flag(props, pic::kTransformSkip);
  p.cu_qp_delta_enabled = flag(props, pic::kCuQpDelta);
  p.diff_cu_qp_delta_depth = p.cu_qp_delta_enabled ? pp.diff_cu_qp_delta_depth : 0;
  if (p.diff_cu_qp_delta_depth > s.log2_ctb - s.log2_min_cb) return DxvaResult::Invalid;
  if (pp.pps_cb_qp_offset < -12 || pp.pps_cb_qp_offset > 12 || pp.pps_cr_qp_offset < -12 || pp.pps_cr_qp_offset > 12) return DxvaResult::Invalid;
  p.cb_qp_offset = pp.pps_cb_qp_offset;
  p.cr_qp_offset = pp.pps_cr_qp_offset;
  p.slice_chroma_qp_offsets_present = flag(props, pic::kSliceChromaQpOffsetsPresent);
  p.weighted_pred = flag(props, pic::kWeightedPred);
  p.weighted_bipred = flag(props, pic::kWeightedBipred);
  p.transquant_bypass = flag(props, pic::kTransquantBypass);
  p.tiles_enabled = flag(props, pic::kTiles);
  p.entropy_coding_sync = flag(props, pic::kEntropyCodingSync);
  if (p.tiles_enabled) {
    p.num_tile_cols = pp.num_tile_columns_minus1 + 1;
    p.num_tile_rows = pp.num_tile_rows_minus1 + 1;
    if (p.num_tile_cols > 20 || p.num_tile_rows > 22 || p.num_tile_cols > s.pic_w_ctb || p.num_tile_rows > s.pic_h_ctb) return DxvaResult::Invalid;
    p.uniform_spacing = flag(props, pic::kUniformSpacing);
    if (!p.uniform_spacing) {
      // Every column and row but the last as given; the last takes what is left, so the others must leave at least one.
      int used = 0;
      for (int i = 0; i < p.num_tile_cols - 1; ++i) {
        p.col_width_minus1[i] = pp.column_width_minus1[i];
        used += p.col_width_minus1[i] + 1;
      }
      if (used >= s.pic_w_ctb) return DxvaResult::Invalid;
      used = 0;
      for (int i = 0; i < p.num_tile_rows - 1; ++i) {
        p.row_height_minus1[i] = pp.row_height_minus1[i];
        used += p.row_height_minus1[i] + 1;
      }
      if (used >= s.pic_h_ctb) return DxvaResult::Invalid;
    }
    p.loop_filter_across_tiles = flag(props, pic::kLoopFilterAcrossTiles);
  } else if (pp.num_tile_columns_minus1 != 0 || pp.num_tile_rows_minus1 != 0) {
    return DxvaResult::Invalid;
  }
  p.loop_filter_across_slices = flag(props, pic::kLoopFilterAcrossSlices);
  p.deblocking_override_enabled = flag(props, pic::kDeblockingOverride);
  p.deblocking_disabled = flag(props, pic::kDeblockingDisabled);
  p.deblocking_control_present = p.deblocking_override_enabled || p.deblocking_disabled || pp.pps_beta_offset_div2 != 0 || pp.pps_tc_offset_div2 != 0;
  if (pp.pps_beta_offset_div2 < -6 || pp.pps_beta_offset_div2 > 6 || pp.pps_tc_offset_div2 < -6 || pp.pps_tc_offset_div2 > 6) return DxvaResult::Invalid;
  p.beta_offset_div2 = p.deblocking_disabled ? 0 : pp.pps_beta_offset_div2;
  p.tc_offset_div2 = p.deblocking_disabled ? 0 : pp.pps_tc_offset_div2;
  p.scaling.set_default();
  p.scaling_list_present = false;   // (the matrix buffer holds the lists in force: the SPS's above carries them)
  p.lists_modification_present = flag(props, pic::kListsModificationPresent);
  p.log2_parallel_merge_level = pp.log2_parallel_merge_level_minus2 + 2;
  if (p.log2_parallel_merge_level > s.log2_ctb) return DxvaResult::Invalid;
  p.slice_header_extension_present = flag(props, pic::kSliceHeaderExtensionPresent);
  p.valid = true;
  *sps_out = s;
  *pps_out = p;
  *why = nullptr;
  return DxvaResult::Ok;
}

bool HevcDxvaDecoder::run_slices(const DxvaPicture& picture, bool* damaged, const char** why) {
  constexpr uint64_t kControl = sizeof(dxva::SliceHevcShort);
  if (picture.slice_control.empty() || picture.slice_control.size() % kControl != 0) {
    *why = "HEVC: the slice control buffer is not an array of DXVA_Slice_HEVC_Short";
    return false;
  }
  const uint64_t count = picture.slice_control.size() / kControl;
  // First check every control, so that nothing is decoded from a picture that is refused.
  for (uint64_t i = 0; i < count; ++i) {
    ByteReader in(picture.slice_control.subview(i * kControl, kControl));
    const uint32_t location = in.le32(), size = in.le32(), chopping = in.le16();
    ConstByteView bytes;
    if (!in.ok() || chopping > 3 || size == 0 || !picture.bitstream.try_subview(location, size, &bytes)) {
      *why = "HEVC: a slice control is outside the bitstream buffer";
      return false;
    }
  }
  chopped_.clear();
  bool in_piece = false;
  for (uint64_t i = 0; i < count; ++i) {
    ByteReader in(picture.slice_control.subview(i * kControl, kControl));
    const uint32_t location = in.le32(), size = in.le32(), chopping = in.le16();
    const ConstByteView bytes = picture.bitstream.subview(location, size);
    ConstByteView whole = bytes;
    if (chopping != 0) {
      // A slice that did not fit one buffer: 1 is its start, 2 a middle part, 3 its end. The parts are put together in order.
      if (chopping == 1) {
        chopped_.clear();
        in_piece = true;
      } else if (!in_piece) {
        *damaged = true;   // the start of the slice is missing
        continue;
      }
      if (!chopped_.append(bytes)) {
        *damaged = true;
        in_piece = false;
        continue;
      }
      if (chopping != 3) continue;
      in_piece = false;
      whole = chopped_.view();
    } else if (in_piece) {
      *damaged = true;   // a chopped slice whose end never came
      in_piece = false;
    }
    bool any = false;
    h264::for_each_nal(whole, 0, [&](const uint8_t* nal, uint64_t n) {
      any = true;
      if (!core_->external_slice(ConstByteView(nal, n))) *damaged = true;
    });
    if (!any) *damaged = true;
  }
  if (in_piece) *damaged = true;
  return true;
}

bool HevcDxvaDecoder::output(const Picture& p, DxvaDecodedPicture* out) {
  const uint64_t w = static_cast<uint64_t>(p.width), h = static_cast<uint64_t>(p.height);
  const uint64_t cw = w / 2, ch = h / 2;
  out_.clear();
  if (!out_.put_zeros(w * h + cw * ch * 2)) return false;
  ByteView dst = out_.writable_view();
  const ConstByteView y(p.plane[0].data(), p.plane[0].size()), u(p.plane[1].data(), p.plane[1].size()), v(p.plane[2].data(), p.plane[2].size());
  for (uint64_t row = 0; row < h; ++row) dst.subview(row * w, w).copy_from(y.subview(row * static_cast<uint64_t>(p.stride[0]), w));
  ByteView uv = dst.subview(w * h);
  for (uint64_t row = 0; row < ch; ++row) {
    const ConstByteView ur = u.subview(row * static_cast<uint64_t>(p.stride[1]), cw), vr = v.subview(row * static_cast<uint64_t>(p.stride[2]), cw);
    ByteView line = uv.subview(row * cw * 2, cw * 2);
    for (uint64_t x = 0; x < cw; ++x) {
      line[2 * x] = ur[x];
      line[2 * x + 1] = vr[x];
    }
  }
  VideoFrame f;
  f.format = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), PixelFormat::Nv12};
  const ConstByteView all = out_.view();
  f.plane[0] = all.data();
  f.plane[1] = all.subview(w * h).data();
  f.stride[0] = f.stride[1] = static_cast<uint32_t>(w);
  f.keyframe = false;
  f.limited_range = true;
  out->frame = f;
  return true;
}

DxvaResult HevcDxvaDecoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  *out = DxvaDecodedPicture();
  dxva::PicParamsHevc pp;
  {
    ByteReader in(picture.picture_parameters);
    if (!in.read_into(bytes_of(pp))) return refuse(out, DxvaResult::Invalid, "HEVC: the picture parameter buffer is shorter than DXVA_PicParams_HEVC");
  }
  const char* why = nullptr;
  h265::Sps sps;
  h265::Pps pps;
  const DxvaResult made = make_parameter_sets(pp, picture.quantization_matrix, &sps, &pps, &why);
  if (made != DxvaResult::Ok) return refuse(out, made, why);
  if (pp.wNumBitsForShortTermRPSInSlice > 16 * 2 * 16 * 32) return refuse(out, DxvaResult::Invalid, "HEVC: the slice header's reference picture set is too long");

  // The surfaces: the current picture and the references (RefPicList).
  const uint32_t cur_index = pp.CurrPic & 0x7F;
  uint32_t cur_key = 0;
  if (!dxva::output_surface_key(picture, config_.surfaces, cur_index, &cur_key)) return refuse(out, DxvaResult::Invalid, "HEVC: CurrPic names no surface");
  uint32_t ref_key[15] = {};
  bool ref_present[15] = {};
  uint32_t keep[16];
  uint32_t keep_count = 0;
  keep[keep_count++] = cur_key;
  for (int i = 0; i < 15; ++i) {
    if (pp.RefPicList[i] == kNoEntry || (pp.RefPicList[i] & 0x7F) == 0x7F) continue;
    if (!dxva::surface_key(picture, config_.surfaces, pp.RefPicList[i] & 0x7Fu, &ref_key[i])) return refuse(out, DxvaResult::Invalid, "HEVC: RefPicList names no surface");
    ref_present[i] = true;
    keep[keep_count++] = ref_key[i];
  }
  // The three sets, as indexes into RefPicList; entries of 0xFF are unused.
  int sets[3][8];
  int set_count[3] = {0, 0, 0};
  const uint8_t* lists[3] = {pp.RefPicSetStCurrBefore, pp.RefPicSetStCurrAfter, pp.RefPicSetLtCurr};
  for (int s = 0; s < 3; ++s)
    for (int i = 0; i < 8; ++i) {
      const uint8_t e = lists[s][i];
      if (e == kNoEntry) continue;
      if (e >= 15 || !ref_present[e]) return refuse(out, DxvaResult::Invalid, "HEVC: a reference picture set names an empty RefPicList entry");
      sets[s][set_count[s]++] = e;
    }
  const bool intra_only = set_count[0] + set_count[1] + set_count[2] == 0;
  if (field(pp.dwCodingSettingPicturePropertyFlags, dxva::hevc_pic::kIdrPic, 1) && !intra_only)
    return refuse(out, DxvaResult::Invalid, "HEVC: an IDR picture with references");

  // The slices must be inside the bitstream before anything changes.
  {
    constexpr uint64_t kControl = sizeof(dxva::SliceHevcShort);
    if (picture.slice_control.empty() || picture.slice_control.size() % kControl != 0)
      return refuse(out, DxvaResult::Invalid, "HEVC: the slice control buffer is not an array of DXVA_Slice_HEVC_Short");
    if (picture.bitstream.empty()) return refuse(out, DxvaResult::Invalid, "HEVC: no bitstream buffer");
  }

  core_->external_prepare(sps, pps);
  bool missing = false, self_reference = false;
  h265::Core::ExternalPicture e;
  Picture* by_entry[15] = {};
  for (int s = 0; s < 3; ++s)
    for (int i = 0; i < set_count[s]; ++i) {
      const int entry = sets[s][i];
      if (!by_entry[entry]) {
        HevcSurface* surface = surfaces_.find(ref_key[entry]);
        if (ref_key[entry] == cur_key) self_reference = true;   // the picture that was in the output surface before this one
        Picture* r = surface && surface->valid ? surface->picture.get() : nullptr;
        if (r && (r->width != sps.width || r->height != sps.height)) r = nullptr;   // a picture of another size cannot be referred to
        if (r) {
          surfaces_.touch(ref_key[entry]);
        } else {
          // 8.3.3: a reference that is not there is a grey picture.
          missing = true;
          if (!grey_[entry]) grey_[entry] = UniquePointer<Picture>::make(allocator_);
          if (!grey_[entry] || !core_->external_grey(grey_[entry].get(), pp.PicOrderCntValList[entry])) return refuse(out, DxvaResult::Invalid, "HEVC: out of memory");
          r = grey_[entry].get();
        }
        by_entry[entry] = r;
      }
      Picture** set = s == 0 ? e.st_before : s == 1 ? e.st_after : e.lt_curr;
      set[i] = by_entry[entry];
    }
  e.st_before_count = set_count[0];
  e.st_after_count = set_count[1];
  e.lt_count = set_count[2];
  e.poc = pp.CurrPicOrderCntVal;
  e.st_rps_bits = pp.wNumBitsForShortTermRPSInSlice;

  bool fresh = false;
  HevcSurface* target = surfaces_.claim(cur_key, ArrayView<const uint32_t>(keep, keep_count), &fresh);
  if (!target) return refuse(out, DxvaResult::Invalid, "HEVC: no room for the surface");
  // A picture that refers to the one its own surface held is decoded into a spare picture, which takes the surface's place when it is done.
  UniquePointer<Picture>& into = self_reference ? spare_ : target->picture;
  if (!self_reference) target->valid = false;   // (overwritten from here on, whatever it held)
  if (!into) into = UniquePointer<Picture>::make(allocator_);
  if (!into) return refuse(out, DxvaResult::Invalid, "HEVC: out of memory");
  e.target = into.get();
  if (!core_->external_begin(e)) return refuse(out, DxvaResult::Invalid, "HEVC: the picture could not be set up");
  bool damaged = false;
  if (!run_slices(picture, &damaged, &why)) {
    (void)core_->external_end();
    return refuse(out, DxvaResult::Invalid, why);
  }
  if (!core_->external_end()) return refuse(out, DxvaResult::Invalid, "HEVC: no slice segment of the picture could be decoded");
  // The picture is kept (and given out) even when some of its slices were damaged: what was decoded is there, the rest is concealed as the core leaves it.
  if (self_reference) target->picture.swap(spare_);
  target->valid = true;
  out->surface = cur_index;
  if (!output(*target->picture, out)) return refuse(out, DxvaResult::Invalid, "HEVC: out of memory");
  if (damaged) {
    out->detail = "HEVC: a slice segment was damaged";
    return DxvaResult::Invalid;
  }
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_hevc(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::HevcVldMain || config.output != PixelFormat::Nv12) return {};
  UniquePointer<HevcDxvaDecoder> decoder = UniquePointer<HevcDxvaDecoder>::make(allocator, allocator, config);
  if (!decoder || !decoder->init()) return {};
  return UniquePointer<IDxvaDecoder>(util::move(decoder));
}

}  // namespace hardware
