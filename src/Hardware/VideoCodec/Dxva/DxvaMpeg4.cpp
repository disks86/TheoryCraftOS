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
// The DXVA MPEG-4 Part 2 decoder (D3D11_DECODER_PROFILE_MPEG4PT2_VLD_SIMPLE, _ADVSIMPLE_NOGMC and _ADVSIMPLE_GMC) on the CPU decoder of Software/Mpeg4*.cpp. The
// program sends, per video object plane, DXVA_PicParams_MPEG4_PART2, DXVA_QmatrixData and the plane's data with one DXVA_SliceInfo per piece (DxvaTypesMpeg4.hpp);
// the pieces are put back to back (the video packets of the plane, with their resync markers, are the decoder's to find). The video object layer is not sent: this
// makes it from the parameters (and the matrices in force) and hands it and the plane to the decoder with the pictures the parameters name - every decoded picture is
// kept under the key of the surface it went into (DxvaSurfaces.hpp), with its macroblock modes and vectors, which a B plane's direct mode reads from its backward
// anchor - and the distances of direct mode (TRB, TRD). The picture comes back at once, at its whole coded size (whole macroblocks), as NV12.
//
// The data: when the first piece begins with the plane's start code (00 00 01 B6, as the plane is in the stream) the plane header is read from it, and must say what
// the parameters say; otherwise the data begins where the header would and the first macroblock is wMBbitOffset bits into it, and the header is the parameters'.
// A short video header picture (short_video_header 1: H.263 baseline) is sent from its picture start code and its header is read from it.
//
// Planes with vop_coded 0 repeat their forward reference. Not decoded, refused as Unsupported: reduced resolution planes, static sprites, post-processing into
// wDeblockedPictureIndex (unPicPostProc). The tools a profile does not have are refused as Invalid with that profile: B and S planes, quarter samples, interlace and
// the MPEG quantiser with the Simple profile, S planes (GMC) without the GMC profile. Xvid streams are decoded with the standard IDCT (the stream's user data that
// makes a software decoder choose Xvid's is not sent through DXVA), as hardware decoders do.
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesMpeg4.hpp"
#include "Hardware/VideoCodec/Software/Mpeg4Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {

using dxva::PicParamsMpeg4Part2;
using dxva::QmatrixMpeg2;
using dxva::SliceInfoMpeg2;

constexpr uint32_t kMaxSlices = 65536;
constexpr uint16_t kNoIndex = 0xFFFF;

// What the decoder keeps for a surface.
struct Mpeg4Surface {
  mpeg4::Picture picture;
  bool decoded = false;
  int mb_width = 0, mb_height = 0;
};

// Makes `p` a picture of `mb_width` x `mb_height` macroblocks (its contents are left as they are when it already is one).
bool size_picture(allocators::IAllocator& allocator, mpeg4::Picture& p, int mb_width, int mb_height) {
  const uint64_t w = static_cast<uint64_t>(mb_width) * 16, h = static_cast<uint64_t>(mb_height) * 16, mbs = static_cast<uint64_t>(mb_width) * static_cast<uint64_t>(mb_height);
  if (p.width == static_cast<int>(w) && p.height == static_cast<int>(h) && p.plane[0].size() == w * h && p.mbs.size() == mbs) return true;
  p.width = p.height = 0;
  if (!p.plane[0].alloc(allocator, w * h) || !p.plane[1].alloc(allocator, w / 2 * (h / 2)) || !p.plane[2].alloc(allocator, w / 2 * (h / 2)) || !p.mbs.alloc(allocator, mbs))
    return false;
  p.width = static_cast<int>(w);
  p.height = static_cast<int>(h);
  p.stride[0] = static_cast<int>(w);
  p.stride[1] = p.stride[2] = static_cast<int>(w / 2);
  return true;
}

bool copy_picture(allocators::IAllocator& allocator, mpeg4::Picture& dst, const mpeg4::Picture& src) {
  if (!size_picture(allocator, dst, src.width / 16, src.height / 16)) return false;
  for (int c = 0; c < 3; ++c)
    if (!ByteView(dst.plane[c].data(), dst.plane[c].size()).copy_from(ConstByteView(src.plane[c].data(), src.plane[c].size()))) return false;
  if (!ArrayView<mpeg4::MbInfo>(dst.mbs.data(), dst.mbs.size()).copy_from(ArrayView<const mpeg4::MbInfo>(src.mbs.data(), src.mbs.size()))) return false;
  dst.type = src.type;
  return true;
}

// av_log2(resolution - 1) + 1, at least 1: the bits of vop_time_increment.
uint32_t time_increment_bits(uint32_t resolution) {
  uint32_t bits = 1;
  while (bits < 16 && (1u << bits) < resolution) ++bits;
  return bits;
}

class DxvaMpeg4Decoder final : public IDxvaDecoder {
 public:
  DxvaMpeg4Decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config)
      : allocator_(allocator), config_(config), core_(allocator), surfaces_(config.surfaces + 1), data_(allocator), out_(allocator) {
    reset_matrices();
  }
  ~DxvaMpeg4Decoder() = default;

  DxvaProfile profile() const override { return config_.profile; }
  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override;
  void reset() override {
    surfaces_.clear();
    reset_matrices();
  }

 private:
  void reset_matrices() {
    for (int i = 0; i < 64; ++i) {
      intra_[i] = mpeg4::kDefaultIntraMatrix[mpeg4::kZigzag[i]];
      non_intra_[i] = mpeg4::kDefaultNonIntraMatrix[mpeg4::kZigzag[i]];
    }
  }
  static DxvaResult refuse(DxvaDecodedPicture* out, DxvaResult result, const char* detail) {
    out->detail = detail;
    return result;
  }
  // A picture of the size whose samples are mid grey and whose macroblocks are all intra-less zero-vector ones: the reference for one that holds nothing.
  mpeg4::Picture* grey(int mb_width, int mb_height) {
    if (grey_.width != mb_width * 16 || grey_.height != mb_height * 16 || grey_.plane[0].size() == 0) {
      if (!size_picture(allocator_, grey_, mb_width, mb_height)) return nullptr;
      for (int c = 0; c < 3; ++c) ByteView(grey_.plane[c].data(), grey_.plane[c].size()).fill(128);
      ArrayView<mpeg4::MbInfo>(grey_.mbs.data(), grey_.mbs.size()).fill(mpeg4::MbInfo{});
      grey_.type = mpeg4::kPictureI;
    }
    return &grey_;
  }
  [[nodiscard]] bool to_nv12(const mpeg4::Picture& picture);

  allocators::IAllocator& allocator_;
  DxvaDecoderConfig config_;
  mpeg4::Core core_;
  dxva::SurfaceTable<Mpeg4Surface> surfaces_;
  mpeg4::Picture grey_;
  mpeg4::Picture scratch_;   // a picture decoded into a surface it also predicts from goes here first
  uint8_t intra_[64], non_intra_[64];   // the matrices in force (scan order); DXVA_QmatrixData replaces them
  ByteBuffer data_;          // the plane's pieces, back to back
  VideoFrameBuffer out_;
};

bool DxvaMpeg4Decoder::to_nv12(const mpeg4::Picture& picture) {
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

DxvaResult DxvaMpeg4Decoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  out->detail = nullptr;
  out->frame = VideoFrame();
  PicParamsMpeg4Part2 pp;
  if (picture.picture_parameters.size() < sizeof(PicParamsMpeg4Part2) || !dxva::read_mpeg4_picture_parameters(picture.picture_parameters, &pp))
    return refuse(out, DxvaResult::Invalid, "the picture parameters are missing or shorter than DXVA_PicParams_MPEG4_PART2");
  if (picture.slice_control.empty() || picture.bitstream.empty()) return refuse(out, DxvaResult::Invalid, "no slice control or bitstream buffer");
  out->surface = pp.wDecodedPictureIndex;

  // --- what the plane is
  const uint16_t flags = pp.wPicFlagBitFields;
  const bool short_header = pp.short_video_header != 0;
  if (pp.short_video_header > 1) return refuse(out, DxvaResult::Invalid, "short_video_header is not 0 or 1");
  if (pp.vop_coding_type > 3) return refuse(out, DxvaResult::Invalid, "vop_coding_type is not 0 .. 3");
  const mpeg4::VopType type = static_cast<mpeg4::VopType>(pp.vop_coding_type);
  if ((flags & dxva::kMpeg4PicPostProcMask) != 0) return refuse(out, DxvaResult::Unsupported, "post-processing (unPicPostProc: deblocking or deringing into wDeblockedPictureIndex)");
  const bool simple = config_.profile == DxvaProfile::Mpeg4Pt2Simple, gmc_profile = config_.profile == DxvaProfile::Mpeg4Pt2AdvSimpleGmc;
  const uint32_t sprite_enable = pp.wSpriteBitFields & dxva::kMpeg4SpriteEnableMask;
  const uint32_t warping_points = (pp.wSpriteBitFields >> dxva::kMpeg4SpritePointsShift) & 63, warping_accuracy = (pp.wSpriteBitFields >> dxva::kMpeg4SpriteAccuracyShift) & 3;
  const bool interlaced = (flags & dxva::kMpeg4PicInterlaced) != 0, quarter_sample = (flags & dxva::kMpeg4PicQuarterSample) != 0;
  const bool quant_type = (flags & dxva::kMpeg4PicQuantType) != 0, data_partitioned = (flags & dxva::kMpeg4PicDataPartitioned) != 0;
  const bool reversible_vlc = (flags & dxva::kMpeg4PicReversibleVlc) != 0;
  const uint32_t fcode_forward = pp.wFcodeBitFields & 7, fcode_backward = (pp.wFcodeBitFields >> 3) & 7;
  if (short_header) {
    if (type != mpeg4::VopType::I && type != mpeg4::VopType::P) return refuse(out, DxvaResult::Invalid, "a short video header picture that is not I or P");
  } else {
    if ((flags & dxva::kMpeg4PicReducedResolution) != 0) return refuse(out, DxvaResult::Unsupported, "reduced resolution planes (reduced_resolution_vop_enable)");
    if (sprite_enable == 1) return refuse(out, DxvaResult::Unsupported, "static sprites");
    if (sprite_enable == 3) return refuse(out, DxvaResult::Invalid, "sprite_enable 3 is reserved");
    if (sprite_enable == 2 && !gmc_profile) return refuse(out, DxvaResult::Invalid, "global motion compensation (sprite_enable 2) needs the ADVSIMPLE_GMC profile");
    if (sprite_enable == 2 && warping_points > 3) return refuse(out, DxvaResult::Invalid, "more than 3 warping points (the Advanced Simple profile has at most 3)");
    if (type == mpeg4::VopType::S && sprite_enable != 2) return refuse(out, DxvaResult::Invalid, "an S plane in a layer without global motion compensation");
    if (simple && (type == mpeg4::VopType::B || interlaced || quarter_sample || quant_type))
      return refuse(out, DxvaResult::Invalid, "a tool the Simple profile does not have (B planes, interlace, quarter samples, the MPEG quantiser)");
    // (verid 1: the first version's syntax, without quarter samples and GMC; any other (2, 4, 5 ...: what encoders write) has them; 0 is reserved)
    if (pp.video_object_layer_verid == 0 || pp.video_object_layer_verid > 15) return refuse(out, DxvaResult::Invalid, "video_object_layer_verid is not 1 .. 15");
    if (pp.video_object_layer_verid == 1 && (quarter_sample || sprite_enable == 2)) return refuse(out, DxvaResult::Invalid, "quarter samples or GMC in a version 1 layer");
    if (reversible_vlc && !data_partitioned) return refuse(out, DxvaResult::Invalid, "reversible_vlc without data_partitioned");
    if (pp.vop_time_increment_resolution == 0) return refuse(out, DxvaResult::Invalid, "vop_time_increment_resolution is 0");
  }
  const bool coded = short_header || (flags & dxva::kMpeg4PicVopCoded) != 0;
  if (coded) {
    if (pp.vop_quant < 1 || pp.vop_quant > 31) return refuse(out, DxvaResult::Invalid, "vop_quant outside 1..31");
    if (!short_header && type != mpeg4::VopType::I && fcode_forward == 0) return refuse(out, DxvaResult::Invalid, "vop_fcode_forward is 0");
    if (!short_header && type == mpeg4::VopType::B && fcode_backward == 0) return refuse(out, DxvaResult::Invalid, "vop_fcode_backward is 0");
  }
  if (pp.vop_width == 0 || pp.vop_height == 0) return refuse(out, DxvaResult::Invalid, "vop_width or vop_height is 0");
  const int mb_width = (pp.vop_width + 15) / 16, mb_height = (pp.vop_height + 15) / 16;
  if (mb_width > mpeg4::kMaxMbWidth || mb_height > mpeg4::kMaxMbHeight || static_cast<uint32_t>(mb_width) * 16 > (config_.width + 15) / 16 * 16 ||
      static_cast<uint32_t>(mb_height) * 16 > (config_.height + 15) / 16 * 16)
    return refuse(out, DxvaResult::Invalid, "the picture is larger than the decoder's surfaces");
  const bool needs_forward = type != mpeg4::VopType::I, needs_backward = type == mpeg4::VopType::B;
  if (needs_backward && (pp.TRD[0] == 0 || pp.TRB[0] == 0 || pp.TRB[0] >= pp.TRD[0] || pp.TRD[0] > 0xFFFF))
    return refuse(out, DxvaResult::Invalid, "a B plane whose TRB / TRD are not 0 < TRB < TRD < 65536");
  if (needs_backward && interlaced && (pp.TRD[1] == 0 || pp.TRB[1] == 0 || pp.TRB[1] >= pp.TRD[1] || pp.TRD[1] > 0xFFFF))
    return refuse(out, DxvaResult::Invalid, "an interlaced B plane whose field TRB / TRD are not 0 < TRB < TRD < 65536");

  // --- the surfaces
  uint32_t key = 0, forward_key = 0, backward_key = 0;
  if (!dxva::output_surface_key(picture, config_.surfaces, pp.wDecodedPictureIndex, &key)) return refuse(out, DxvaResult::Invalid, "wDecodedPictureIndex is out of range");
  if (needs_forward && !dxva::surface_key(picture, config_.surfaces, pp.wForwardRefPictureIndex, &forward_key))
    return refuse(out, DxvaResult::Invalid, "wForwardRefPictureIndex is out of range");
  if (needs_backward && !dxva::surface_key(picture, config_.surfaces, pp.wBackwardRefPictureIndex, &backward_key))
    return refuse(out, DxvaResult::Invalid, "wBackwardRefPictureIndex is out of range");
  if (type == mpeg4::VopType::I && !coded) return refuse(out, DxvaResult::Invalid, "an I plane that is not coded (it has nothing to repeat)");

  // --- the matrices (validated before any is taken)
  uint8_t intra[64], non_intra[64];
  for (int i = 0; i < 64; ++i) {
    intra[i] = intra_[i];
    non_intra[i] = non_intra_[i];
  }
  if (!picture.quantization_matrix.empty()) {
    QmatrixMpeg2 qm;
    if (picture.quantization_matrix.size() < sizeof(QmatrixMpeg2) || !dxva::read_mpeg2_qmatrix(picture.quantization_matrix, &qm))
      return refuse(out, DxvaResult::Invalid, "the quantization matrix buffer is shorter than DXVA_QmatrixData");
    for (int m = 0; m < 2; ++m) {
      if (qm.bNewQmatrix[m] > 1) return refuse(out, DxvaResult::Invalid, "bNewQmatrix is not 0 or 1");
      if (!qm.bNewQmatrix[m]) continue;
      for (uint16_t v : qm.Qmatrix[m])
        if (v < 1 || v > 255) return refuse(out, DxvaResult::Invalid, "a quantiser matrix entry outside 1..255");
    }
    for (int i = 0; i < 64; ++i) {
      if (qm.bNewQmatrix[0]) intra[i] = static_cast<uint8_t>(qm.Qmatrix[0][i]);
      if (qm.bNewQmatrix[1]) non_intra[i] = static_cast<uint8_t>(qm.Qmatrix[1][i]);
    }
  }

  // --- the pieces, back to back
  if (picture.slice_control.size() % sizeof(SliceInfoMpeg2) != 0) return refuse(out, DxvaResult::Invalid, "the slice control buffer is not a whole number of DXVA_SliceInfo");
  const uint64_t slice_count = picture.slice_control.size() / sizeof(SliceInfoMpeg2);
  if (slice_count > kMaxSlices) return refuse(out, DxvaResult::Invalid, "too many slices");
  data_.clear();
  uint32_t mb_bit_offset = 0;
  ByteReader sr(picture.slice_control);
  for (uint64_t i = 0; i < slice_count; ++i) {
    SliceInfoMpeg2 si;
    if (!dxva::read_mpeg2_slice_info(sr, &si)) return refuse(out, DxvaResult::Invalid, "the slice control buffer is truncated");
    if (si.wBadSliceChopping != 0) return refuse(out, DxvaResult::Unsupported, "a slice split over several bitstream buffers (wBadSliceChopping)");
    if (si.bStartCodeBitOffset != 0) return refuse(out, DxvaResult::Unsupported, "a slice whose data does not begin at the first bit of a byte");
    const uint64_t bytes = (uint64_t{si.dwSliceBitsInBuffer} + 7) / 8;
    ConstByteView slice;
    if (bytes == 0 || !picture.bitstream.try_subview(si.dwSliceDataLocation, bytes, &slice)) return refuse(out, DxvaResult::Invalid, "a slice outside the bitstream buffer");
    if (i == 0) {
      mb_bit_offset = si.wMBbitOffset;
      if (mb_bit_offset > si.dwSliceBitsInBuffer) return refuse(out, DxvaResult::Invalid, "wMBbitOffset is past the end of the first slice");
    }
    if (!data_.append(slice)) return refuse(out, DxvaResult::Invalid, "out of memory");
  }
  const ConstByteView data = data_.view();
  const bool with_start_code = !short_header && data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0xB6;
  if (short_header && !mpeg4::is_short_header(data)) return refuse(out, DxvaResult::Invalid, "a short video header picture that does not begin with its picture start code");

  // --- the layer the parameters stand for, and the plane header
  mpeg4::Vol vol;
  mpeg4::VopHeader header;
  ConstByteView body = data;
  if (!short_header) {
    vol.width = pp.vop_width;
    vol.height = pp.vop_height;
    vol.time_increment_resolution = pp.vop_time_increment_resolution;
    vol.time_increment_bits = time_increment_bits(pp.vop_time_increment_resolution);
    vol.verid = pp.video_object_layer_verid;
    vol.interlaced = interlaced;
    vol.obmc_disable = true;
    vol.quant_type_mpeg = quant_type;
    vol.quarter_sample = quarter_sample;
    vol.resync_marker_disable = (flags & dxva::kMpeg4PicResyncMarkerDisable) != 0;
    vol.data_partitioned = data_partitioned;
    vol.reversible_vlc = reversible_vlc;
    vol.profile_and_level = pp.profile_and_level_indication;
    vol.gmc = sprite_enable == 2;
    vol.warping_points = static_cast<uint8_t>(vol.gmc ? warping_points : 0);
    vol.warping_accuracy = static_cast<uint8_t>(vol.gmc ? warping_accuracy : 0);
    for (int i = 0; i < 64; ++i) {
      vol.intra_matrix[i] = intra[i];
      vol.non_intra_matrix[i] = non_intra[i];
    }
    header.type = type;
    header.coded = coded;
    header.rounding_type = (type == mpeg4::VopType::P || type == mpeg4::VopType::S) && (flags & dxva::kMpeg4PicRoundingType) != 0;
    header.intra_dc_vlc_thr = static_cast<uint8_t>((flags >> dxva::kMpeg4PicIntraDcVlcThrShift) & 7);
    header.top_field_first = interlaced && (flags & dxva::kMpeg4PicTopFieldFirst) != 0;
    header.alternate_vertical_scan = interlaced && (flags & dxva::kMpeg4PicAlternateVerticalScan) != 0;
    header.quant = pp.vop_quant;
    header.fcode_forward = static_cast<uint8_t>(type == mpeg4::VopType::I ? 1 : fcode_forward);
    header.fcode_backward = static_cast<uint8_t>(type == mpeg4::VopType::B ? fcode_backward : 1);
    if (type == mpeg4::VopType::S)
      for (uint32_t i = 0; i < 3; ++i)
        for (int c = 0; c < 2; ++c) header.sprite_trajectory[i][c] = i < vol.warping_points ? pp.warping_mv[i][c] : 0;
    header.header_bits = mb_bit_offset;
    if (with_start_code) {
      // The plane header is in the data: it must be the one the parameters describe.
      body = data.subview(4);
      mpeg4::VopHeader h;
      if (!mpeg4::parse_vop_header(body, vol, &h)) return refuse(out, DxvaResult::Invalid, "the plane header in the bitstream cannot be read with the layer the parameters describe");
      bool same = h.type == header.type && h.coded == header.coded;
      if (same && h.coded) {
        same = h.rounding_type == header.rounding_type && h.intra_dc_vlc_thr == header.intra_dc_vlc_thr && h.quant == header.quant &&
               (type == mpeg4::VopType::I || h.fcode_forward == header.fcode_forward) && (type != mpeg4::VopType::B || h.fcode_backward == header.fcode_backward) &&
               (!interlaced || (h.top_field_first == header.top_field_first && h.alternate_vertical_scan == header.alternate_vertical_scan));
        for (uint32_t i = 0; same && type == mpeg4::VopType::S && i < vol.warping_points; ++i)
          same = h.sprite_trajectory[i][0] == header.sprite_trajectory[i][0] && h.sprite_trajectory[i][1] == header.sprite_trajectory[i][1];
      }
      if (!same) return refuse(out, DxvaResult::Invalid, "the plane header in the bitstream is not the one the picture parameters describe");
      header = h;
    }
  }

  // --- the pictures: the references (missing ones are grey), and where this one goes
  bool missing = false;
  auto reference = [&](uint32_t ref_key) -> mpeg4::Picture* {
    Mpeg4Surface* s = surfaces_.find(ref_key);
    if (!s || !s->decoded || s->mb_width != mb_width || s->mb_height != mb_height) {
      missing = true;
      return grey(mb_width, mb_height);
    }
    surfaces_.touch(ref_key);
    return &s->picture;
  };
  uint32_t keep[2];
  uint32_t keep_count = 0;
  if (needs_forward) keep[keep_count++] = forward_key;
  if (needs_backward) keep[keep_count++] = backward_key;
  const bool into_reference = (needs_forward && forward_key == key) || (needs_backward && backward_key == key);
  bool fresh = false;
  Mpeg4Surface* target = surfaces_.claim(key, ArrayView<const uint32_t>(keep, keep_count), &fresh);
  if (!target) return refuse(out, DxvaResult::Invalid, "no surface entry is free");
  mpeg4::Core::ExternalPicture ext;
  ext.forward = needs_forward ? reference(forward_key) : nullptr;
  ext.backward = needs_backward ? reference(backward_key) : nullptr;
  if ((needs_forward && !ext.forward) || (needs_backward && !ext.backward)) return refuse(out, DxvaResult::Invalid, "out of memory");
  ext.pp_time = pp.TRD[0];
  ext.pb_time = pp.TRB[0];
  ext.pp_field_time = pp.TRD[1];
  ext.pb_field_time = pp.TRB[1];
  ext.target = into_reference ? &scratch_ : &target->picture;
  if (!into_reference) target->decoded = false;

  if (!coded) {
    // vop_coded 0: the plane repeats the picture it would predict from.
    if (ext.forward != &target->picture && !copy_picture(allocator_, target->picture, *ext.forward)) {
      target->decoded = false;
      return refuse(out, DxvaResult::Invalid, "out of memory");
    }
  } else {
    mpeg4::Core::ExternalStatus status;
    if (short_header) {
      mpeg4::ShortHeader sh;
      status = core_.decode_external_short(data, ext, &sh);
      if (status == mpeg4::Core::ExternalStatus::Decoded &&
          (sh.inter != (type == mpeg4::VopType::P) || sh.quant != pp.vop_quant || sh.width != pp.vop_width || sh.height != pp.vop_height)) {
        if (!into_reference) target->decoded = false;
        return refuse(out, DxvaResult::Invalid, "the short video header in the bitstream is not the one the picture parameters describe");
      }
    } else {
      mpeg4::VopHeader used;
      status = core_.decode_external(vol, &header, body, ext, &used);
    }
    if (status != mpeg4::Core::ExternalStatus::Decoded) {
      if (!into_reference) target->decoded = false;
      if (status == mpeg4::Core::ExternalStatus::NoMemory) return refuse(out, DxvaResult::Invalid, "out of memory");
      return refuse(out, DxvaResult::Invalid, "the plane cannot be decoded with the layer and header the parameters describe");
    }
    if (into_reference && !copy_picture(allocator_, target->picture, scratch_)) {
      target->decoded = false;
      return refuse(out, DxvaResult::Invalid, "out of memory");
    }
  }
  target->decoded = true;
  target->mb_width = mb_width;
  target->mb_height = mb_height;
  for (int i = 0; i < 64; ++i) {
    intra_[i] = intra[i];
    non_intra_[i] = non_intra[i];
  }
  if (!to_nv12(target->picture)) return refuse(out, DxvaResult::Invalid, "out of memory");
  out->frame = out_.frame();
  out->frame.limited_range = true;
  out->frame.keyframe = type == mpeg4::VopType::I;
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_mpeg4(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::Mpeg4Pt2Simple && config.profile != DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc && config.profile != DxvaProfile::Mpeg4Pt2AdvSimpleGmc) return {};
  if (config.output != PixelFormat::Nv12 || config.surfaces == 0 || config.surfaces > 128 || config.width == 0 || config.height == 0) return {};
  return UniquePointer<IDxvaDecoder>::make_as<DxvaMpeg4Decoder>(allocator, allocator, config);
}

}  // namespace hardware
