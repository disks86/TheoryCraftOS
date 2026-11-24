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
// The DXVA VC-1 decoder (D3D11_DECODER_PROFILE_VC1_VLD = DXVA_ModeVC1_D, and D3D11_DECODER_PROFILE_VC1_D2010) on the CPU decoder of Software/Vc1*.cpp: Simple, Main
// (WMV3) and Advanced profile, progressive and interlaced pictures. The program sends, per picture, DXVA_PictureParameters and the frame (Advanced profile: behind the
// 00 00 01 0D start code the program puts there, then its slices behind 0B; Simple and Main profile: the frame as stored) with one DXVA_SliceInfo per piece
// (DxvaTypesVc1.hpp). The stream's sequence and entry point layers are not sent: this rebuilds them from the parameters (as DxvaMpeg2.cpp rebuilds MPEG-2's headers)
// and hands them and the frame to the decoder, which reads the picture layer and the macroblocks from the bitstream, with the pictures the parameters name: every
// decoded picture is kept under the key of the surface it went into (DxvaSurfaces.hpp). The rounding control of a Simple or Main profile picture is bRcontrol. The
// picture comes back at once, at its whole coded size (whole macroblocks), as NV12.
//
// Interlaced pictures (as ffmpeg's dxva2_vc1.c sends them, after the DXVA specification for Windows Media Video v8, v9 and vA decoding): an interlaced frame picture
// is one picture (bPicStructure 3, bPicExtrapolation 2); a field interlace frame is two pictures into the same surface, the first field (bPicStructure 1 top or
// 2 bottom, the frame unit with the frame header and the first field's slices) and right after it the second field (bSecondField 1, the other parity, its field
// unit behind the 00 00 01 0C start code and its slices). Each field comes back as a field picture (DxvaDecodedPicture::field): only its lines go into the surface.
// A B field pair's REFDIST is bMV_RPS - 9 (it was in an earlier frame's header).
//
// Intensity compensation is reference picture modification (DXVA 3.2.16): each surface keeps the tables its samples are read through, which a P picture (or field)
// that compensates it updates; the B pictures after it in decoding order and before it in display order read the compensated reference. Range reduction
// (RANGEREDFRM, bit 5 of bPicDeblocked) and range mapping (RANGE_MAPY / RANGE_MAPUV in bPicOBMC) are done as the standard says: a range reduced picture is kept in
// reduced range, a reference in the other range is scaled for prediction, and what goes into the surface is the output (scaled up, or mapped), while the picture
// kept as the reference stays as it was reconstructed (the decoder keeps its own references by key).
//
// Values the parameters and the bitstream both carry must agree, or the picture is Invalid: the picture type (bPicIntra, bPicBackwardPrediction), the frame
// coding mode (bPicExtrapolation, bPicStructure 3 or a field), the field order (which field bPicStructure names, against TFF), RANGEREDFRM, and LUMSCALE / LUMSHIFT
// (wBitstreamFcodes / wBitstreamPCEelements, top << 8 | bottom for fields) when bBidirectionalAveragingMode says intensity compensation is on. The other copies
// (4MV, the motion vector mode, PQUANT, BFRACTION) are read from the bitstream itself. Not decoded, refused as Unsupported: pan-scan and progressive segmented
// frames, and post-processing into wDeblockedPictureIndex (deblocking and deringing, which are hints for the display: POSTPROC; the in-loop filter and overlap
// smoothing are decoding and are done).
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesVc1.hpp"
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {

using dxva::PictureParametersVc1;
using dxva::SliceInfoVc1;

constexpr uint32_t kMaxSlices = 65536;

// What the decoder keeps for a surface.
struct Vc1Surface {
  vc1::Picture picture;
  bool decoded = false;
  int mb_width = 0, mb_height = 0;
  // Intensity compensation, kept the way the reference decoder keeps it (its "last" and "next" tables), so that decoding a picture again gives the same picture:
  // `self` the tables the picture's own samples are read through (a field pair's second field compensates its first field), and the tables the picture applied to
  // its forward reference (`forward_key`), which the B pictures between the two read that reference through.
  vc1::Core::IcLuts self;
  vc1::Core::IcLuts applied;
  bool has_applied = false;
  uint32_t forward_key = 0;
};

// Makes `p` a picture of `mb_width` x `mb_height` macroblocks (its contents are left as they are when it already is one).
bool size_picture(allocators::IAllocator& allocator, vc1::Picture& p, int mb_width, int mb_height) {
  const uint64_t w = static_cast<uint64_t>(mb_width) * 16, h = static_cast<uint64_t>(mb_height) * 16;
  if (p.width == static_cast<int>(w) && p.height == static_cast<int>(h) && p.plane[0].size() == w * h && p.direct_mv.size() == w / 16 * (h / 16) * 2) return true;
  p.width = p.height = 0;
  if (!p.plane[0].alloc(allocator, w * h) || !p.plane[1].alloc(allocator, w * h / 4) || !p.plane[2].alloc(allocator, w * h / 4) ||
      !p.direct_mv.alloc(allocator, w / 16 * (h / 16) * 2))
    return false;
  p.width = static_cast<int>(w);
  p.height = static_cast<int>(h);
  p.stride[0] = static_cast<int>(w);
  p.stride[1] = p.stride[2] = static_cast<int>(w / 2);
  return true;
}

template <class T>
bool copy_mem(allocators::IAllocator& allocator, h264::Mem<T>& dst, const h264::Mem<T>& src) {
  if (src.size() == 0) {
    dst.release();
    return true;
  }
  return (dst.size() == src.size() || dst.alloc(allocator, src.size())) && ArrayView<T>(dst.data(), dst.size()).copy_from(ArrayView<const T>(src.data(), src.size()));
}

// The whole picture: its samples, what direct mode and field prediction read of it, and how it is output.
bool copy_picture(allocators::IAllocator& allocator, vc1::Picture& dst, const vc1::Picture& src) {
  for (int c = 0; c < 3; ++c) {
    if (!copy_mem(allocator, dst.plane[c], src.plane[c])) return false;
    dst.stride[c] = src.stride[c];
  }
  if (!copy_mem(allocator, dst.direct_mv, src.direct_mv) || !copy_mem(allocator, dst.mb_flags, src.mb_flags) || !copy_mem(allocator, dst.direct_mv_bottom, src.direct_mv_bottom))
    return false;
  dst.width = src.width;
  dst.height = src.height;
  dst.type = src.type;
  dst.interlaced = src.interlaced;
  dst.field_coded = src.field_coded;
  dst.range_reduced = src.range_reduced;
  dst.range_map_y = src.range_map_y;
  dst.range_map_uv = src.range_map_uv;
  return true;
}

class DxvaVc1Decoder final : public IDxvaDecoder {
 public:
  DxvaVc1Decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config)
      : allocator_(allocator), config_(config), core_(allocator), surfaces_(config.surfaces + 1), data_(allocator), out_(allocator) {}
  ~DxvaVc1Decoder() = default;

  DxvaProfile profile() const override { return config_.profile; }
  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override;
  void reset() override {
    surfaces_.clear();
    overlap_ = false;
    pair_pending_ = false;
  }

 private:
  static DxvaResult refuse(DxvaDecodedPicture* out, DxvaResult result, const char* detail) {
    out->detail = detail;
    return result;
  }
  // A picture of the size whose samples are all mid grey: the reference a picture gets for one the program names that holds nothing.
  vc1::Picture* grey(int mb_width, int mb_height) {
    if (grey_.width != mb_width * 16 || grey_.height != mb_height * 16 || grey_.plane[0].size() == 0) {
      if (!size_picture(allocator_, grey_, mb_width, mb_height)) return nullptr;
      for (int c = 0; c < 3; ++c) ByteView(grey_.plane[c].data(), grey_.plane[c].size()).fill(128);
      ArrayView<int16_t>(grey_.direct_mv.data(), grey_.direct_mv.size()).fill(0);
      grey_.type = vc1::kPictureI;
    }
    return &grey_;
  }
  [[nodiscard]] bool to_nv12(const vc1::Picture& picture, int mb_height);

  allocators::IAllocator& allocator_;
  DxvaDecoderConfig config_;
  vc1::Core core_;
  dxva::SurfaceTable<Vc1Surface> surfaces_;
  vc1::Picture grey_;
  vc1::Picture scratch_;   // a picture decoded into a surface it also predicts from goes here first
  bool overlap_ = false;   // OVERLAP, as the last picture that said it had it (B pictures do not say)
  // The first field of a field pair just decoded: the next picture may be its second field (into the same surface, the other parity, the same size).
  bool pair_pending_ = false;
  uint32_t pair_key_ = 0;
  uint8_t pair_structure_ = 0;
  int pair_mb_width_ = 0, pair_mb_height_ = 0;
  ByteBuffer data_;        // the picture's slices, back to back
  VideoFrameBuffer out_;
};

// The output of the picture (its coded size: `mb_height` macroblock rows, which a field pair's picture may have one more of): the reconstruction through the
// output mapping (range reduction scaled up, range mapping), as NV12.
bool DxvaVc1Decoder::to_nv12(const vc1::Picture& picture, int mb_height) {
  const VideoFormat format{static_cast<uint32_t>(picture.width), static_cast<uint32_t>(mb_height) * 16, PixelFormat::Nv12};
  if (!format.valid() || !out_.allocate(format) || picture.stride[0] != picture.width || picture.stride[1] != picture.width / 2) return false;
  const vc1::OutputMap map = vc1::output_map(picture);
  const ArrayView<const uint8_t> ly(map.y, 256), luv(map.uv, 256);
  const uint64_t w = format.width, h = format.height, cw = w / 2, ch = h / 2;
  const ConstByteView y(picture.plane[0].data(), picture.plane[0].size()), u(picture.plane[1].data(), picture.plane[1].size()),
      v(picture.plane[2].data(), picture.plane[2].size());
  if (y.size() < w * h || u.size() < cw * ch || v.size() < cw * ch) return false;
  ByteView luma = out_.plane(0), chroma = out_.plane(1);
  for (uint64_t row = 0; row < h; ++row) {
    ByteView line = luma.subview(row * format.plane_stride(0), w);
    if (!line.copy_from(y.subview(row * w, w))) return false;
    if (map.any)
      for (uint8_t& s : line) s = ly[s];
  }
  for (uint64_t row = 0; row < ch; ++row) {
    ByteView line = chroma.subview(row * format.plane_stride(1), 2 * cw);
    const ConstByteView cb = u.subview(row * cw, cw), cr = v.subview(row * cw, cw);
    for (uint64_t x = 0; x < cw; ++x) {
      line[2 * x] = luv[cb[x]];
      line[2 * x + 1] = luv[cr[x]];
    }
  }
  return true;
}

DxvaResult DxvaVc1Decoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  out->detail = nullptr;
  out->frame = VideoFrame();
  out->surface_format = DxvaSurfaceFormat::Nv12;
  out->field = 0;
  // (a second field must come right after its first field: whatever this picture is, the pair ends here unless it is that second field)
  const bool pair_pending = pair_pending_;
  pair_pending_ = false;
  PictureParametersVc1 pp;
  if (picture.picture_parameters.size() < sizeof(PictureParametersVc1) || !dxva::read_mpeg2_picture_parameters(picture.picture_parameters, &pp))
    return refuse(out, DxvaResult::Invalid, "the picture parameters are missing or shorter than DXVA_PictureParameters");
  if (picture.slice_control.empty() || picture.bitstream.empty()) return refuse(out, DxvaResult::Invalid, "no slice control or bitstream buffer");
  out->surface = pp.wDecodedPictureIndex;

  // --- what the picture is
  if (pp.bChromaFormat != 1) return refuse(out, DxvaResult::Invalid, "bChromaFormat is not 4:2:0");
  if (pp.bBPPminus1 != 7 || pp.bMacroblockWidthMinus1 != 15 || pp.bMacroblockHeightMinus1 != 15 || pp.bBlockWidthMinus1 != 7 || pp.bBlockHeightMinus1 != 7)
    return refuse(out, DxvaResult::Invalid, "macroblock, block or sample sizes other than VC-1's");
  if (pp.bPicStructure < 1 || pp.bPicStructure > 3 || pp.bSecondField > 1) return refuse(out, DxvaResult::Invalid, "bPicStructure is not 1, 2 or 3");
  if (pp.bPicExtrapolation != 1 && pp.bPicExtrapolation != 2) return refuse(out, DxvaResult::Invalid, "bPicExtrapolation is not 1 (progressive) or 2 (interlaced)");
  const bool field = pp.bPicStructure != dxva::kVc1StructureFrame, interlaced = pp.bPicExtrapolation == 2;
  if (field && !interlaced) return refuse(out, DxvaResult::Invalid, "a field picture (bPicStructure 1 or 2) whose bPicExtrapolation is not 2 (interlaced)");
  if (!field && pp.bSecondField) return refuse(out, DxvaResult::Invalid, "bSecondField in a frame picture (bPicStructure 3)");
  if (pp.bPicIntra > 1 || pp.bPicBackwardPrediction > 1 || (pp.bPicIntra && pp.bPicBackwardPrediction)) return refuse(out, DxvaResult::Invalid, "bPicIntra / bPicBackwardPrediction");
  if (pp.bRcontrol > 1) return refuse(out, DxvaResult::Invalid, "bRcontrol is not 0 or 1");
  const bool advanced = (pp.bBidirectionalAveragingMode & dxva::kVc1AveragingAdvanced) != 0;
  if (interlaced && (!advanced || (pp.bPicDeblockConfined & dxva::kVc1ConfinedInterlace) == 0))
    return refuse(out, DxvaResult::Invalid, "an interlaced picture (bPicExtrapolation 2) of a sequence that is not an interlaced Advanced profile one (INTERLACE)");
  if ((pp.bPicDeblocked & (dxva::kVc1DeblockedDeblocking | dxva::kVc1DeblockedDeringing)) != 0)
    return refuse(out, DxvaResult::Unsupported, "post-processing (deblocking or deringing into wDeblockedPictureIndex)");
  if ((pp.bPicSpatialResid8 & dxva::kVc1SpatialPanScan) != 0) return refuse(out, DxvaResult::Unsupported, "pan-scan windows (PANSCAN_FLAG)");
  if (advanced && (pp.bPicDeblockConfined & dxva::kVc1ConfinedPsf) != 0) return refuse(out, DxvaResult::Unsupported, "progressive segmented frames (PSF)");
  const bool intra = pp.bPicIntra != 0, backward_prediction = pp.bPicBackwardPrediction != 0;
  const bool second_field = field && pp.bSecondField;
  // A B field pair's REFDIST (bMV_RPS - 9; 0 to 16).
  int refdist = -1;
  if (field && backward_prediction) {
    if (pp.bMV_RPS < dxva::kVc1MvRpsRefdistBias || pp.bMV_RPS > dxva::kVc1MvRpsRefdistBias + 16)
      return refuse(out, DxvaResult::Invalid, "bMV_RPS of a B field picture is not REFDIST + 9 (9 to 25)");
    refdist = pp.bMV_RPS - dxva::kVc1MvRpsRefdistBias;
  }

  // --- the size: Advanced profile in samples, the others in macroblocks
  uint32_t width = 0, height = 0;
  if (advanced) {
    width = uint32_t{pp.wPicWidthInMBminus1} + 1;
    height = uint32_t{pp.wPicHeightInMBminus1} + 1;
    if ((width & 1) || (height & 1)) return refuse(out, DxvaResult::Invalid, "an Advanced profile size that is odd");
  } else {
    width = (uint32_t{pp.wPicWidthInMBminus1} + 1) * 16;
    height = (uint32_t{pp.wPicHeightInMBminus1} + 1) * 16;
  }
  const int mb_width = static_cast<int>((width + 15) / 16), mb_height = static_cast<int>((height + 15) / 16);
  if (mb_width > vc1::kMaxMbWidth || mb_height > vc1::kMaxMbHeight || static_cast<uint32_t>(mb_width) * 16 > (config_.width + 15) / 16 * 16 ||
      static_cast<uint32_t>(mb_height) * 16 > (config_.height + 15) / 16 * 16)
    return refuse(out, DxvaResult::Invalid, "the picture is larger than the decoder's surfaces");

  // --- the sequence and entry point layers the parameters stand for
  vc1::SequenceHeader seq;
  seq.valid = true;
  seq.profile = advanced ? vc1::kAdvanced : vc1::kMain;   // (Simple and Main profile decode alike: Simple's restrictions are on what the stream uses)
  seq.loop_filter = (pp.bPicSpatialResid8 & dxva::kVc1SpatialLoopFilter) != 0;
  seq.fastuvmc = (pp.bPicSpatialResid8 & dxva::kVc1SpatialFastUvMc) != 0;
  seq.extended_mv = (pp.bPicSpatialResid8 & dxva::kVc1SpatialExtendedMv) != 0;
  seq.dquant = (pp.bPicSpatialResid8 >> dxva::kVc1SpatialDquantShift) & 3;
  seq.vstransform = (pp.bPicSpatialResid8 & dxva::kVc1SpatialVsTransform) != 0;
  seq.refdist_flag = (pp.bPicSpatialResid8 & dxva::kVc1SpatialRefDist) != 0;
  seq.quantizer_mode = pp.bPicOverflowBlocks >> dxva::kVc1OverflowQuantizerShift;
  seq.multires = (pp.bPicOverflowBlocks & dxva::kVc1OverflowMultires) != 0;
  seq.resync_marker = (pp.bPicOverflowBlocks & dxva::kVc1OverflowSyncMarker) != 0;
  seq.rangered = (pp.bPicOverflowBlocks & dxva::kVc1OverflowRangeRed) != 0;
  seq.max_b_frames = pp.bPicOverflowBlocks & dxva::kVc1OverflowMaxBFramesMask;
  if (!backward_prediction) overlap_ = (pp.bPicDeblocked & dxva::kVc1DeblockedOverlap) != 0;
  seq.overlap = overlap_;
  if (advanced) {
    seq.postprocflag = (pp.bPicDeblockConfined & dxva::kVc1ConfinedPostProc) != 0;
    seq.broadcast = (pp.bPicDeblockConfined & dxva::kVc1ConfinedPulldown) != 0;
    seq.interlace = (pp.bPicDeblockConfined & dxva::kVc1ConfinedInterlace) != 0;
    seq.tfcntrflag = (pp.bPicDeblockConfined & dxva::kVc1ConfinedTfcntr) != 0;
    seq.extended_dmv = (pp.bPicDeblockConfined & dxva::kVc1ConfinedExtendedDmv) != 0;
    // Range mapping: only the parameters have it (it is in the entry point); a value without its flag is not used (ffmpeg leaves the last one there).
    seq.range_mapy_flag = (pp.bPicOBMC & dxva::kVc1ObmcRangeMapYFlag) != 0;
    seq.range_mapy = seq.range_mapy_flag ? (pp.bPicOBMC >> dxva::kVc1ObmcRangeMapYShift) & 7 : 0;
    seq.range_mapuv_flag = (pp.bPicOBMC & dxva::kVc1ObmcRangeMapUvFlag) != 0;
    seq.range_mapuv = seq.range_mapuv_flag ? pp.bPicOBMC & 7 : 0;
    seq.max_coded_width = static_cast<int>(width);
    seq.max_coded_height = static_cast<int>(height);
  } else if (seq.max_b_frames == 0 && backward_prediction) {
    return refuse(out, DxvaResult::Invalid, "a B picture in a Simple or Main profile sequence without B pictures (MAXBFRAMES 0)");
  }
  seq.finterpflag = (pp.bPicDeblockConfined & dxva::kVc1ConfinedFinterp) != 0;
  seq.res_rtm_flag = true;
  seq.width = static_cast<int>(width);
  seq.height = static_cast<int>(height);

  // --- the surfaces
  uint32_t key = 0, forward_key = 0, backward_key = 0;
  if (!dxva::output_surface_key(picture, config_.surfaces, pp.wDecodedPictureIndex, &key)) return refuse(out, DxvaResult::Invalid, "wDecodedPictureIndex is out of range");
  if (second_field && (!pair_pending || pair_key_ != key || pair_structure_ != 3 - pp.bPicStructure || pair_mb_width_ != mb_width || pair_mb_height_ != mb_height))
    return refuse(out, DxvaResult::Invalid, "a second field (bSecondField) that does not follow its first field (the other parity, into the same surface)");
  if (!intra && !dxva::surface_key(picture, config_.surfaces, pp.wForwardRefPictureIndex, &forward_key))
    return refuse(out, DxvaResult::Invalid, "wForwardRefPictureIndex is out of range");
  if (backward_prediction && !dxva::surface_key(picture, config_.surfaces, pp.wBackwardRefPictureIndex, &backward_key))
    return refuse(out, DxvaResult::Invalid, "wBackwardRefPictureIndex is out of range");

  // --- the slices, back to back
  if (picture.slice_control.size() % sizeof(SliceInfoVc1) != 0) return refuse(out, DxvaResult::Invalid, "the slice control buffer is not a whole number of DXVA_SliceInfo");
  const uint64_t slice_count = picture.slice_control.size() / sizeof(SliceInfoVc1);
  if (slice_count > kMaxSlices) return refuse(out, DxvaResult::Invalid, "too many slices");
  data_.clear();
  ByteReader sr(picture.slice_control);
  for (uint64_t i = 0; i < slice_count; ++i) {
    SliceInfoVc1 si;
    if (!dxva::read_mpeg2_slice_info(sr, &si)) return refuse(out, DxvaResult::Invalid, "the slice control buffer is truncated");
    if (si.wBadSliceChopping != 0) return refuse(out, DxvaResult::Unsupported, "a slice split over several bitstream buffers (wBadSliceChopping)");
    if (si.bStartCodeBitOffset != 0) return refuse(out, DxvaResult::Unsupported, "a slice whose data does not begin at the first bit of a byte");
    const uint64_t bytes = (uint64_t{si.dwSliceBitsInBuffer} + 7) / 8;
    ConstByteView slice;
    if (bytes == 0 || !picture.bitstream.try_subview(si.dwSliceDataLocation, bytes, &slice)) return refuse(out, DxvaResult::Invalid, "a slice outside the bitstream buffer");
    if (advanced) {
      // The program puts the start code in front of each piece: the frame's (0D; a second field's 0C), then the slices' (0B). A first piece without one is taken too.
      const bool start_code = slice.size() >= 4 && slice[0] == 0 && slice[1] == 0 && slice[2] == 1;
      if (i == 0 && start_code && slice[3] != (second_field ? dxva::kVc1StartCodeField : dxva::kVc1StartCodeFrame))
        return refuse(out, DxvaResult::Invalid, "the first slice does not begin with the frame start code (00 00 01 0D; a second field's 00 00 01 0C)");
      if (i > 0 && (!start_code || slice[3] != dxva::kVc1StartCodeSlice)) return refuse(out, DxvaResult::Invalid, "a slice after the first that does not begin with a slice start code (00 00 01 0B)");
    }
    if (!data_.append(slice)) return refuse(out, DxvaResult::Invalid, "out of memory");
  }

  // --- the pictures: the references (missing ones are grey), and where this one goes
  bool missing = false;
  auto reference = [&](uint32_t ref_key, Vc1Surface** surface) -> vc1::Picture* {
    Vc1Surface* s = surfaces_.find(ref_key);
    if (surface) *surface = s;
    if (!s || !s->decoded || s->mb_width != mb_width || s->mb_height != mb_height) {
      missing = true;
      if (surface) *surface = nullptr;
      return grey(mb_width, mb_height);
    }
    surfaces_.touch(ref_key);
    return &s->picture;
  };
  uint32_t keep[2];
  uint32_t keep_count = 0;
  if (!intra) keep[keep_count++] = forward_key;
  if (backward_prediction) keep[keep_count++] = backward_key;
  const bool into_reference = (!intra && forward_key == key) || (backward_prediction && backward_key == key);
  bool fresh = false;
  Vc1Surface* target = surfaces_.claim(key, ArrayView<const uint32_t>(keep, keep_count), &fresh);
  if (!target) return refuse(out, DxvaResult::Invalid, "no surface entry is free");
  if (second_field && (fresh || !target->decoded)) return refuse(out, DxvaResult::Invalid, "a second field whose first field is not in the surface");
  vc1::Core::ExternalPicture ext;
  Vc1Surface* forward_surface = nullptr;
  Vc1Surface* backward_surface = nullptr;
  ext.forward = intra ? nullptr : reference(forward_key, &forward_surface);
  ext.backward = backward_prediction ? reference(backward_key, &backward_surface) : nullptr;
  if ((!intra && !ext.forward) || (backward_prediction && !ext.backward)) return refuse(out, DxvaResult::Invalid, "out of memory");
  // The intensity compensation tables (a missing reference reads its grey samples as they are). The forward reference's: what the picture after it that this
  // one depends on did to it (a B picture: its backward anchor, when that predicted from the same reference; a second field: its first field), else its own. The
  // backward reference's: its own. The target's: a second field's starts as its first field left it.
  vc1::Core::IcLuts forward_ic, backward_ic, target_ic;
  vc1::Core::identity_luts(forward_ic);
  vc1::Core::identity_luts(backward_ic);
  vc1::Core::identity_luts(target_ic);
  if (fresh) {
    vc1::Core::identity_luts(target->self);
    target->has_applied = false;
  }
  if (forward_surface) forward_ic = forward_surface->self;
  if (backward_surface) {
    backward_ic = backward_surface->self;
    if (backward_surface->has_applied && backward_surface->forward_key == forward_key) forward_ic = backward_surface->applied;
  }
  if (second_field) {
    target_ic = target->self;
    if (!intra && target->has_applied && target->forward_key == forward_key) forward_ic = target->applied;
  }
  ext.forward_ic = intra ? nullptr : &forward_ic;
  ext.backward_ic = backward_prediction ? &backward_ic : nullptr;
  ext.target_ic = &target_ic;
  ext.rnd = advanced ? -1 : pp.bRcontrol;
  ext.part = !field ? vc1::Core::ExternalPicture::Part::Frame : second_field ? vc1::Core::ExternalPicture::Part::SecondField : vc1::Core::ExternalPicture::Part::FirstField;
  ext.refdist = refdist;
  ext.target = into_reference ? &scratch_ : &target->picture;
  // (a second field decoded into a surface it also predicts from: the copy gets the first field first)
  if (second_field && into_reference && !copy_picture(allocator_, scratch_, target->picture)) return refuse(out, DxvaResult::Invalid, "out of memory");
  if (!into_reference && !second_field) target->decoded = false;
  const auto fail = [&](DxvaResult result, const char* detail) {
    if (!into_reference) target->decoded = false;
    return refuse(out, result, detail);
  };

  vc1::PictureHeader header;
  const vc1::Core::ExternalStatus status = core_.decode_external(seq, data_.view(), ext, &header);
  if (status != vc1::Core::ExternalStatus::Decoded) {
    switch (status) {
      case vc1::Core::ExternalStatus::Structure:
        return fail(DxvaResult::Invalid, "the bitstream's frame coding mode is not the picture structure the parameters give (bPicStructure, bSecondField)");
      case vc1::Core::ExternalStatus::BadSequence: return fail(DxvaResult::Invalid, "the sequence the picture parameters describe is not one VC-1 has");
      case vc1::Core::ExternalStatus::NoMemory: return fail(DxvaResult::Invalid, "out of memory");
      default: return fail(DxvaResult::Invalid, "the picture layer could not be decoded (or uses multiresolution coding or X8 intra coding)");
    }
  }
  // --- what both the parameters and the bitstream say must agree
  const bool header_intra = header.type == vc1::kPictureI || header.type == vc1::kPictureBI, header_b = header.type == vc1::kPictureB;
  if (header_intra != intra || header_b != backward_prediction) return fail(DxvaResult::Invalid, "bPicIntra / bPicBackwardPrediction are not the type of the picture in the bitstream");
  if ((header.fcm != vc1::kFcmProgressive) != interlaced) return fail(DxvaResult::Invalid, "bPicExtrapolation is not the frame coding mode (FCM) of the bitstream");
  if (field) {
    // The first field is the top one when TFF is set.
    const bool top = header.tff != second_field;
    if (pp.bPicStructure != (top ? dxva::kVc1StructureTopField : dxva::kVc1StructureBottomField))
      return fail(DxvaResult::Invalid, "bPicStructure is not the field the bitstream's field order (TFF) makes this one");
  }
  if (!advanced && ((pp.bPicDeblocked & dxva::kVc1DeblockedRangeRedFrame) != 0) != header.rangeredfrm)
    return fail(DxvaResult::Invalid, "bit 5 of bPicDeblocked is not the RANGEREDFRM of the bitstream");
  if ((pp.bBidirectionalAveragingMode & dxva::kVc1AveragingIntensityComp) != 0) {
    const uint16_t scale = static_cast<uint16_t>(field ? header.lumscale << 8 | header.lumscale2 : header.lumscale);
    const uint16_t shift = static_cast<uint16_t>(field ? header.lumshift << 8 | header.lumshift2 : header.lumshift);
    if (header.type != vc1::kPictureP || !header.use_ic || pp.wBitstreamFcodes != scale || pp.wBitstreamPCEelements != shift)
      return fail(DxvaResult::Invalid, "the intensity compensation of the parameters (wBitstreamFcodes / wBitstreamPCEelements) is not the bitstream's LUMSCALE / LUMSHIFT");
  }
  if (into_reference && !copy_picture(allocator_, target->picture, scratch_)) {
    target->decoded = false;
    return refuse(out, DxvaResult::Invalid, "out of memory");
  }
  target->decoded = true;
  target->mb_width = mb_width;
  target->mb_height = mb_height;
  // The tables after the picture: its own, and (an I or P picture) what it did to its forward reference.
  target->self = target_ic;
  if (backward_prediction) {
    target->has_applied = false;
  } else {
    if (!intra) {
      target->applied = forward_ic;
      target->has_applied = true;
      target->forward_key = forward_key;
    } else if (!second_field) {
      target->has_applied = false;   // (an intra first field: the second field may still predict)
    }
  }
  if (!to_nv12(target->picture, mb_height)) return refuse(out, DxvaResult::Invalid, "out of memory");
  out->frame = out_.frame();
  out->frame.limited_range = true;
  out->frame.keyframe = header.type == vc1::kPictureI;
  out->field = field ? pp.bPicStructure : 0;   // (a field: only its lines go into the surface)
  if (field && !second_field) {
    pair_pending_ = true;
    pair_key_ = key;
    pair_structure_ = pp.bPicStructure;
    pair_mb_width_ = mb_width;
    pair_mb_height_ = mb_height;
  }
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_vc1(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::Vc1Vld && config.profile != DxvaProfile::Vc1D2010) return {};
  if (config.output != PixelFormat::Nv12 || config.surfaces == 0 || config.surfaces > 128 || config.width == 0 || config.height == 0) return {};
  return UniquePointer<IDxvaDecoder>::make_as<DxvaVc1Decoder>(allocator, allocator, config);
}

}  // namespace hardware
