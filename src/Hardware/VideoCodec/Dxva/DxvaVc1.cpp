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
// (WMV3) and Advanced profile, progressive pictures. The program sends, per picture, DXVA_PictureParameters and the frame (Advanced profile: behind the 00 00 01 0D start
// code the program puts there, then its slices behind 0B; Simple and Main profile: the frame as stored) with one DXVA_SliceInfo per piece (DxvaTypesVc1.hpp). The
// stream's sequence and entry point layers are not sent: this rebuilds them from the parameters (as DxvaMpeg2.cpp rebuilds MPEG-2's headers) and hands them and the
// frame to the decoder, which reads the picture layer and the macroblocks from the bitstream, with the pictures the parameters name: every decoded picture is kept
// under the key of the surface it went into (DxvaSurfaces.hpp), with the intensity compensation a P picture applied to its reference (the B pictures before it in
// display order predict from the compensated reference too). The rounding control of a Simple or Main profile picture is bRcontrol. The picture comes back at once,
// at its whole coded size (whole macroblocks), as NV12.
//
// Not decoded, refused as Unsupported: interlaced pictures (frame and field coding, bPicExtrapolation 2 or field structures: the CPU decoder has progressive pictures
// only), range reduction (RANGEREDFRM) and range mapping, pan-scan and progressive segmented frames, and post-processing into wDeblockedPictureIndex (deblocking and
// deringing; the in-loop filter and overlap smoothing are decoding and are done). The other copies of picture layer fields the parameters carry (4MV, the motion
// vector mode, PQUANT, BFRACTION, LUMSCALE / LUMSHIFT of a P picture) are read from the bitstream itself, as the picture type is, which must be what bPicIntra and
// bPicBackwardPrediction say.
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
  // A P picture that compensated the intensity of its reference: the B pictures that have this one as their backward anchor predict from the compensated reference.
  bool ic = false;
  int ic_scale = 32, ic_shift = 0;
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

bool copy_picture(allocators::IAllocator& allocator, vc1::Picture& dst, const vc1::Picture& src) {
  if (!size_picture(allocator, dst, src.width / 16, src.height / 16)) return false;
  for (int c = 0; c < 3; ++c)
    if (!ByteView(dst.plane[c].data(), dst.plane[c].size()).copy_from(ConstByteView(src.plane[c].data(), src.plane[c].size()))) return false;
  if (!ArrayView<int16_t>(dst.direct_mv.data(), dst.direct_mv.size()).copy_from(ArrayView<const int16_t>(src.direct_mv.data(), src.direct_mv.size()))) return false;
  dst.type = src.type;
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
  [[nodiscard]] bool to_nv12(const vc1::Picture& picture);

  allocators::IAllocator& allocator_;
  DxvaDecoderConfig config_;
  vc1::Core core_;
  dxva::SurfaceTable<Vc1Surface> surfaces_;
  vc1::Picture grey_;
  vc1::Picture scratch_;   // a picture decoded into a surface it also predicts from goes here first
  bool overlap_ = false;   // OVERLAP, as the last picture that said it had it (B pictures do not say)
  ByteBuffer data_;        // the picture's slices, back to back
  VideoFrameBuffer out_;
};

bool DxvaVc1Decoder::to_nv12(const vc1::Picture& picture) {
  const VideoFormat format{static_cast<uint32_t>(picture.width), static_cast<uint32_t>(picture.height), PixelFormat::Nv12};
  if (!format.valid() || !out_.allocate(format)) return false;
  const uint64_t w = format.width, h = format.height, cw = w / 2, ch = h / 2;
  const ConstByteView y(picture.plane[0].data(), picture.plane[0].size()), u(picture.plane[1].data(), picture.plane[1].size()),
      v(picture.plane[2].data(), picture.plane[2].size());
  if (y.size() < w * h || u.size() < cw * ch || v.size() < cw * ch) return false;
  ByteView luma = out_.plane(0), chroma = out_.plane(1);
  for (uint64_t row = 0; row < h; ++row)
    if (!luma.subview(row * format.plane_stride(0), w).copy_from(y.subview(row * w, w))) return false;
  for (uint64_t row = 0; row < ch; ++row) {
    ByteView line = chroma.subview(row * format.plane_stride(1), 2 * cw);
    const ConstByteView cb = u.subview(row * cw, cw), cr = v.subview(row * cw, cw);
    for (uint64_t x = 0; x < cw; ++x) {
      line[2 * x] = cb[x];
      line[2 * x + 1] = cr[x];
    }
  }
  return true;
}

DxvaResult DxvaVc1Decoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  out->detail = nullptr;
  out->frame = VideoFrame();
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
  if (pp.bPicStructure != 3 || pp.bSecondField || pp.bPicExtrapolation == 2)
    return refuse(out, DxvaResult::Unsupported, "interlaced VC-1 pictures (field and frame coding): the CPU decoder has progressive pictures only");
  if (pp.bPicExtrapolation != 1) return refuse(out, DxvaResult::Invalid, "bPicExtrapolation is not 1 (progressive) or 2 (interlaced)");
  if (pp.bPicIntra > 1 || pp.bPicBackwardPrediction > 1 || (pp.bPicIntra && pp.bPicBackwardPrediction)) return refuse(out, DxvaResult::Invalid, "bPicIntra / bPicBackwardPrediction");
  if (pp.bRcontrol > 1) return refuse(out, DxvaResult::Invalid, "bRcontrol is not 0 or 1");
  const bool advanced = (pp.bBidirectionalAveragingMode & dxva::kVc1AveragingAdvanced) != 0;
  if ((pp.bPicDeblocked & (dxva::kVc1DeblockedDeblocking | dxva::kVc1DeblockedDeringing)) != 0)
    return refuse(out, DxvaResult::Unsupported, "post-processing (deblocking or deringing into wDeblockedPictureIndex)");
  if (!advanced && (pp.bPicDeblocked & dxva::kVc1DeblockedRangeRedFrame) != 0) return refuse(out, DxvaResult::Unsupported, "range reduced pictures (RANGEREDFRM)");
  if ((pp.bPicSpatialResid8 & dxva::kVc1SpatialPanScan) != 0) return refuse(out, DxvaResult::Unsupported, "pan-scan windows (PANSCAN_FLAG)");
  if (advanced && (pp.bPicDeblockConfined & dxva::kVc1ConfinedPsf) != 0) return refuse(out, DxvaResult::Unsupported, "progressive segmented frames (PSF)");
  if (advanced && pp.bPicOBMC != 0) return refuse(out, DxvaResult::Unsupported, "range mapping (RANGE_MAPY / RANGE_MAPUV)");
  const bool intra = pp.bPicIntra != 0, backward_prediction = pp.bPicBackwardPrediction != 0;

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
      // The program puts the start code in front of each piece: the frame's (0D; 0C is a second field), then the slices' (0B). A frame without one is taken too.
      const bool start_code = slice.size() >= 4 && slice[0] == 0 && slice[1] == 0 && slice[2] == 1;
      if (start_code && slice[3] == 0x0C) return refuse(out, DxvaResult::Unsupported, "the second field of a field interlace frame");
      if (i == 0 && start_code && slice[3] != 0x0D) return refuse(out, DxvaResult::Invalid, "the first slice does not begin with the frame start code (00 00 01 0D)");
      if (i > 0 && (!start_code || slice[3] != 0x0B)) return refuse(out, DxvaResult::Invalid, "a slice after the first that does not begin with a slice start code (00 00 01 0B)");
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
  vc1::Core::ExternalPicture ext;
  Vc1Surface* backward_surface = nullptr;
  ext.forward = intra ? nullptr : reference(forward_key, nullptr);
  ext.backward = backward_prediction ? reference(backward_key, &backward_surface) : nullptr;
  if ((!intra && !ext.forward) || (backward_prediction && !ext.backward)) return refuse(out, DxvaResult::Invalid, "out of memory");
  if (backward_surface && backward_surface->ic) {
    ext.ic_forward = true;
    ext.ic_scale = backward_surface->ic_scale;
    ext.ic_shift = backward_surface->ic_shift;
  }
  ext.rnd = advanced ? -1 : pp.bRcontrol;
  ext.target = into_reference ? &scratch_ : &target->picture;
  if (!into_reference) target->decoded = false;

  vc1::PictureHeader header;
  const vc1::Core::ExternalStatus status = core_.decode_external(seq, data_.view(), ext, &header);
  if (status != vc1::Core::ExternalStatus::Decoded) {
    if (!into_reference) target->decoded = false;
    switch (status) {
      case vc1::Core::ExternalStatus::Interlaced: return refuse(out, DxvaResult::Unsupported, "interlaced VC-1 pictures (field and frame coding): the CPU decoder has progressive pictures only");
      case vc1::Core::ExternalStatus::BadSequence: return refuse(out, DxvaResult::Invalid, "the sequence the picture parameters describe is not one VC-1 has");
      case vc1::Core::ExternalStatus::NoMemory: return refuse(out, DxvaResult::Invalid, "out of memory");
      default: return refuse(out, DxvaResult::Invalid, "the picture layer could not be decoded (or uses multiresolution coding, range reduction or X8 intra coding)");
    }
  }
  const bool header_intra = header.type == vc1::kPictureI || header.type == vc1::kPictureBI, header_b = header.type == vc1::kPictureB;
  if (header_intra != intra || header_b != backward_prediction) {
    if (!into_reference) target->decoded = false;
    return refuse(out, DxvaResult::Invalid, "bPicIntra / bPicBackwardPrediction are not the type of the picture in the bitstream");
  }
  if (into_reference && !copy_picture(allocator_, target->picture, scratch_)) {
    target->decoded = false;
    return refuse(out, DxvaResult::Invalid, "out of memory");
  }
  target->decoded = true;
  target->mb_width = mb_width;
  target->mb_height = mb_height;
  target->ic = header.type == vc1::kPictureP && header.use_ic;
  target->ic_scale = header.lumscale;
  target->ic_shift = header.lumshift;
  if (!to_nv12(target->picture)) return refuse(out, DxvaResult::Invalid, "out of memory");
  out->frame = out_.frame();
  out->frame.limited_range = true;
  out->frame.keyframe = header.type == vc1::kPictureI;
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_vc1(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::Vc1Vld && config.profile != DxvaProfile::Vc1D2010) return {};
  if (config.output != PixelFormat::Nv12 || config.surfaces == 0 || config.surfaces > 128 || config.width == 0 || config.height == 0) return {};
  return UniquePointer<IDxvaDecoder>::make_as<DxvaVc1Decoder>(allocator, allocator, config);
}

}  // namespace hardware
