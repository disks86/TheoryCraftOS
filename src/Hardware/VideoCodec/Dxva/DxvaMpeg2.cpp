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
// The DXVA MPEG-2 decoder (D3D11_DECODER_PROFILE_MPEG2_VLD) and MPEG-1/MPEG-2 decoder (D3D11_DECODER_PROFILE_MPEG2and1_VLD) on the CPU decoder of
// Software/Mpeg2*.cpp. The program sends, per picture (or field), DXVA_PictureParameters, DXVA_QmatrixData and the slices with their start codes; this rebuilds the
// headers those stand for (a sequence header carrying the matrices in force, for MPEG-2 its extension, the picture header and for MPEG-2 its coding extension),
// hands them and the slices to the decoder together with the pictures the parameters name - every decoded picture is kept under the key of the surface it went into
// (DxvaSurfaces.hpp) - and gives the picture back at once, at its whole coded size, as NV12.
//
// Field pictures: the first field starts a frame in its surface, the second (bSecondField) completes the frame of the same surface; a predicted second field may
// predict from the first field of its own frame. MPEG-1 (only with Mpeg2and1Vld): a picture with wBitstreamPCEelements 0 (see DxvaTypesMpeg2.hpp).
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesMpeg2.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {

using dxva::PictureParametersMpeg2;
using dxva::QmatrixMpeg2;
using dxva::SliceInfoMpeg2;

// The default intra quantiser matrix (ISO/IEC 13818-2 6.3.11) in zigzag scan order, as DXVA_QmatrixData holds matrices.
constexpr uint8_t kDefaultIntraZigzag[64] = {8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
                                             29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};
constexpr uint16_t kNoIndex = 0xFFFF;
constexpr uint32_t kMaxSlices = 65536;

// Writes the bits of the rebuilt headers, most significant first.
class HeaderWriter {
 public:
  explicit HeaderWriter(ByteBuffer& out) : out_(out) {}
  void bits(uint32_t value, int count) {   // count <= 24
    for (int i = count - 1; i >= 0; --i) {
      acc_ = static_cast<uint8_t>((acc_ << 1) | ((value >> i) & 1));
      if (++filled_ == 8) flush_byte();
    }
  }
  void start_code(uint8_t code) {
    align();
    const uint8_t prefix[4] = {0, 0, 1, code};
    ok_ = out_.append(ConstByteView(prefix, 4)) && ok_;
  }
  void align() {
    while (filled_ != 0) bits(0, 1);
  }
  [[nodiscard]] bool ok() const { return ok_; }

 private:
  void flush_byte() {
    ok_ = out_.put8(acc_) && ok_;
    acc_ = 0;
    filled_ = 0;
  }
  ByteBuffer& out_;
  uint8_t acc_ = 0;
  int filled_ = 0;
  bool ok_ = true;
};

// What the decoder keeps for a surface.
struct Mpeg2Surface {
  mpeg2::Picture picture;
  bool decoded = false;     // holds a decoded picture (a frame, or the first field of one)
  int open_field = 0;       // the structure of a first field whose second has not come (0: none)
  uint32_t mb_width = 0, mb_height = 0;   // of the frame
};

class DxvaMpeg2Decoder final : public IDxvaDecoder {
 public:
  DxvaMpeg2Decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config)
      : allocator_(allocator), config_(config), core_(allocator), surfaces_(config.surfaces + 1), headers_(allocator), slices_(allocator), out_(allocator) {
    reset_matrices();
  }
  ~DxvaMpeg2Decoder() = default;

  DxvaProfile profile() const override { return config_.profile; }
  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override;
  void reset() override {
    surfaces_.clear();
    reset_matrices();
  }

 private:
  void reset_matrices() {
    for (int i = 0; i < 64; ++i) {
      intra_[i] = kDefaultIntraZigzag[i];
      non_intra_[i] = 16;
    }
  }
  static DxvaResult refuse(DxvaDecodedPicture* out, DxvaResult result, const char* detail) {
    out->detail = detail;
    return result;
  }
  [[nodiscard]] bool build_headers(const PictureParametersMpeg2& pp, bool mpeg1, int type, uint32_t mb_width, uint32_t frame_mb_height, const uint8_t* intra, const uint8_t* non_intra);
  [[nodiscard]] bool to_nv12(const mpeg2::Picture& picture);

  allocators::IAllocator& allocator_;
  DxvaDecoderConfig config_;
  mpeg2::Core core_;
  dxva::SurfaceTable<Mpeg2Surface> surfaces_;
  mpeg2::Picture scratch_;   // a picture decoded into a surface it also predicts from goes here first
  uint8_t intra_[64], non_intra_[64];   // the matrices in force (zigzag order); DXVA_QmatrixData replaces them
  ByteBuffer headers_;
  List<ConstByteView> slices_;
  VideoFrameBuffer out_;
};

bool DxvaMpeg2Decoder::build_headers(const PictureParametersMpeg2& pp, bool mpeg1, int type, uint32_t mb_width, uint32_t frame_mb_height, const uint8_t* intra,
                                     const uint8_t* non_intra) {
  headers_.clear();
  HeaderWriter w(headers_);
  // The sizes: the smallest that give the macroblock counts (the coded size is what is decoded; DXVA does not say the display size). An interlaced MPEG-2 sequence
  // counts its height in macroblock pairs, a progressive one (needed when the count is odd) in macroblocks.
  const bool progressive_sequence = mpeg1 || (frame_mb_height & 1) != 0;
  const uint32_t horizontal = mb_width * 16 - 15;
  const uint32_t vertical = progressive_sequence ? frame_mb_height * 16 - 15 : frame_mb_height * 16 - 31;
  // 6.2.2.1 sequence_header, with both matrices loaded (so the ones in force always apply)
  w.start_code(0xB3);
  w.bits(horizontal & 0xFFF, 12);
  w.bits(vertical & 0xFFF, 12);
  w.bits(1, 4);   // aspect_ratio_information: square samples
  w.bits(5, 4);   // frame_rate_code: 30000/1001 (not used by decoding)
  w.bits(0x3FFFF, 18);
  w.bits(1, 1);   // marker
  w.bits(0, 10);  // vbv_buffer_size
  w.bits(0, 1);   // constrained_parameters_flag
  w.bits(1, 1);
  for (int i = 0; i < 64; ++i) w.bits(intra[i], 8);
  w.bits(1, 1);
  for (int i = 0; i < 64; ++i) w.bits(non_intra[i], 8);
  if (!mpeg1) {
    // 6.2.2.3 sequence_extension
    w.start_code(0xB5);
    w.bits(1, 4);
    w.bits(0x44, 8);   // Main profile, High level
    w.bits(progressive_sequence ? 1 : 0, 1);
    w.bits(1, 2);      // 4:2:0
    w.bits(horizontal >> 12, 2);
    w.bits(vertical >> 12, 2);
    w.bits(0, 12);
    w.bits(1, 1);
    w.bits(0, 8);
    w.bits(0, 1);      // low_delay
    w.bits(0, 2);
    w.bits(0, 5);
  }
  // 6.2.3 picture_header
  w.start_code(0x00);
  w.bits(0, 10);
  w.bits(static_cast<uint32_t>(type), 3);
  w.bits(0xFFFF, 16);
  const uint32_t f = pp.wBitstreamFcodes;
  if (type == mpeg2::kPictureP || type == mpeg2::kPictureB) {
    w.bits(0, 1);
    w.bits(mpeg1 ? (f >> 12) & 7 : 7, 3);
  }
  if (type == mpeg2::kPictureB) {
    w.bits(0, 1);
    w.bits(mpeg1 ? (f >> 4) & 7 : 7, 3);
  }
  w.bits(0, 1);   // extra_bit_picture
  if (!mpeg1) {
    // 6.2.3.1 picture_coding_extension
    const uint32_t pce = pp.wBitstreamPCEelements;
    w.start_code(0xB5);
    w.bits(8, 4);
    w.bits(f, 16);                  // the four f_codes, in the same order
    w.bits(pce >> 3, 13);           // intra_dc_precision .. progressive_frame, as wBitstreamPCEelements has them
    w.bits(0, 1);                   // composite_display_flag
  }
  w.align();
  return w.ok();
}

bool DxvaMpeg2Decoder::to_nv12(const mpeg2::Picture& picture) {
  const VideoFormat format{static_cast<uint32_t>(picture.width), static_cast<uint32_t>(picture.height), PixelFormat::Nv12};
  if (!format.valid() || !out_.allocate(format)) return false;
  const uint64_t w = format.width, h = format.height, cw = w / 2, ch = h / 2;
  const ConstByteView y(picture.plane[0].data(), picture.plane[0].size()), u(picture.plane[1].data(), picture.plane[1].size()),
      v(picture.plane[2].data(), picture.plane[2].size());
  if (y.size() < w * h || u.size() < cw * ch || v.size() < cw * ch) return false;
  ByteView luma = out_.plane(0), chroma = out_.plane(1);
  if (!luma.first(w * h).copy_from(y.first(w * h))) return false;
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

DxvaResult DxvaMpeg2Decoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  out->detail = nullptr;
  out->frame = VideoFrame();
  PictureParametersMpeg2 pp;
  if (picture.picture_parameters.size() < sizeof(PictureParametersMpeg2) || !dxva::read_mpeg2_picture_parameters(picture.picture_parameters, &pp))
    return refuse(out, DxvaResult::Invalid, "the picture parameters are missing or shorter than DXVA_PictureParameters");
  if (picture.slice_control.empty() || picture.bitstream.empty()) return refuse(out, DxvaResult::Invalid, "no slice control or bitstream buffer");
  out->surface = pp.wDecodedPictureIndex;

  // --- what the picture is
  const bool mpeg1 = pp.wBitstreamPCEelements == 0;
  if (mpeg1 && config_.profile != DxvaProfile::Mpeg2and1Vld) return refuse(out, DxvaResult::Invalid, "picture_structure 0 in wBitstreamPCEelements (MPEG-1 needs the MPEG2and1 profile)");
  if (pp.bChromaFormat != 1) return refuse(out, DxvaResult::Invalid, "bChromaFormat is not 4:2:0");
  if (pp.bBPPminus1 != 7 || pp.bMacroblockWidthMinus1 != 15 || pp.bMacroblockHeightMinus1 != 15 || pp.bBlockWidthMinus1 != 7 || pp.bBlockHeightMinus1 != 7)
    return refuse(out, DxvaResult::Invalid, "macroblock, block or sample sizes other than MPEG-2's");
  if (pp.bBidirectionalAveragingMode != 0 || pp.bPicOBMC != 0 || pp.bPic4MVallowed != 0 || pp.bMV_RPS != 0 || pp.bPicBinPB != 0)
    return refuse(out, DxvaResult::Unsupported, "H.263 prediction modes (averaging mode, OBMC, 4MV, reference selection, PB frames)");
  if (pp.bPicScanFixed != 1 || pp.bPicScanMethod > 1) return refuse(out, DxvaResult::Unsupported, "a scan other than the zigzag and alternate scans");
  const int structure = pp.bPicStructure;
  if (structure < mpeg2::kTopField || structure > mpeg2::kFramePicture) return refuse(out, DxvaResult::Invalid, "bPicStructure is not 1, 2 or 3");
  const bool field = structure != mpeg2::kFramePicture;
  if (pp.bSecondField > 1 || (pp.bSecondField && !field)) return refuse(out, DxvaResult::Invalid, "bSecondField on a frame picture");
  if (pp.bPicIntra > 1 || pp.bPicBackwardPrediction > 1 || (pp.bPicIntra && pp.bPicBackwardPrediction)) return refuse(out, DxvaResult::Invalid, "bPicIntra / bPicBackwardPrediction");
  const int type = pp.bPicIntra ? mpeg2::kPictureI : pp.bPicBackwardPrediction ? mpeg2::kPictureB : mpeg2::kPictureP;
  const uint32_t f = pp.wBitstreamFcodes;
  if (mpeg1) {
    if (field) return refuse(out, DxvaResult::Invalid, "an MPEG-1 picture that is a field");
    if (pp.bPicScanMethod != 0) return refuse(out, DxvaResult::Invalid, "an MPEG-1 picture with the alternate scan");
    const uint32_t fwd = (f >> 12) & 15, bwd = (f >> 4) & 15;
    if (type != mpeg2::kPictureI && (fwd < 1 || fwd > 7 || ((f >> 8) & 15) != fwd)) return refuse(out, DxvaResult::Invalid, "an MPEG-1 forward_f_code outside 1..7");
    if (type == mpeg2::kPictureB && (bwd < 1 || bwd > 7 || (f & 15) != bwd)) return refuse(out, DxvaResult::Invalid, "an MPEG-1 backward_f_code outside 1..7");
  } else {
    if (static_cast<int>((pp.wBitstreamPCEelements >> 12) & 3) != structure) return refuse(out, DxvaResult::Invalid, "the picture_structure of wBitstreamPCEelements is not bPicStructure");
    if ((pp.wBitstreamPCEelements & 7) != 0) return refuse(out, DxvaResult::Invalid, "reserved bits of wBitstreamPCEelements are set");
    if (((pp.wBitstreamPCEelements & dxva::kMpeg2PceAlternateScan) != 0) != (pp.bPicScanMethod == 1)) return refuse(out, DxvaResult::Invalid, "bPicScanMethod is not the alternate_scan of the picture");
    for (int shift = 0; shift < 16; shift += 4) {
      const uint32_t code = (f >> shift) & 15;
      if (code == 0 || (code > 9 && code != 15)) return refuse(out, DxvaResult::Invalid, "an f_code outside 1..9 (or 15)");
    }
  }
  const uint32_t mb_width = uint32_t{pp.wPicWidthInMBminus1} + 1, frame_mb_height = (uint32_t{pp.wPicHeightInMBminus1} + 1) << (field ? 1 : 0);
  if (mb_width > static_cast<uint32_t>(mpeg2::kMaxMbWidth) || frame_mb_height > static_cast<uint32_t>(mpeg2::kMaxMbHeight) || (mpeg1 && mb_width * 16 - 15 > 4095) ||
      mb_width * 16 > (config_.width + 15) / 16 * 16 || frame_mb_height * 16 > (config_.height + 31) / 32 * 32)
    return refuse(out, DxvaResult::Invalid, "the picture is larger than the decoder's surfaces");

  // --- the surfaces
  uint32_t key = 0, forward_key = 0, backward_key = 0;
  // (The current picture is kept under DxvaPicture::output_key when there is one; its index is then not looked up.)
  if (!dxva::output_surface_key(picture, config_.surfaces, pp.wDecodedPictureIndex, &key)) return refuse(out, DxvaResult::Invalid, "wDecodedPictureIndex is out of range");
  // (A predicted second field of a frame whose first field is intra may have no other reference than that first field - the stream starts with such a frame, or
  // after a seek - and then names none: 0xFFFF.)
  const bool own_frame_only = type == mpeg2::kPictureP && pp.bSecondField && pp.wForwardRefPictureIndex == kNoIndex;
  const bool needs_forward = type != mpeg2::kPictureI && !own_frame_only, needs_backward = type == mpeg2::kPictureB;
  if (needs_forward && !dxva::surface_key(picture, config_.surfaces, pp.wForwardRefPictureIndex, &forward_key))
    return refuse(out, DxvaResult::Invalid, "wForwardRefPictureIndex is out of range");
  if (needs_backward && !dxva::surface_key(picture, config_.surfaces, pp.wBackwardRefPictureIndex, &backward_key))
    return refuse(out, DxvaResult::Invalid, "wBackwardRefPictureIndex is out of range");

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
    for (int m = 0; m < 4; ++m) {
      if (qm.bNewQmatrix[m] > 1) return refuse(out, DxvaResult::Invalid, "bNewQmatrix is not 0 or 1");
      if (!qm.bNewQmatrix[m]) continue;
      for (uint16_t v : qm.Qmatrix[m])
        if (v < 1 || v > 255) return refuse(out, DxvaResult::Invalid, "a quantiser matrix entry outside 1..255");
    }
    // (4:2:0 pictures use the luma matrices for chroma too: 6.3.11)
    for (int i = 0; i < 64; ++i) {
      if (qm.bNewQmatrix[0]) intra[i] = static_cast<uint8_t>(qm.Qmatrix[0][i]);
      if (qm.bNewQmatrix[1]) non_intra[i] = static_cast<uint8_t>(qm.Qmatrix[1][i]);
    }
  }

  // --- the slices
  if (picture.slice_control.size() % sizeof(SliceInfoMpeg2) != 0) return refuse(out, DxvaResult::Invalid, "the slice control buffer is not a whole number of DXVA_SliceInfo");
  const uint64_t slice_count = picture.slice_control.size() / sizeof(SliceInfoMpeg2);
  if (slice_count > kMaxSlices) return refuse(out, DxvaResult::Invalid, "too many slices");
  slices_.clear();
  ByteReader sr(picture.slice_control);
  for (uint64_t i = 0; i < slice_count; ++i) {
    SliceInfoMpeg2 si;
    if (!dxva::read_mpeg2_slice_info(sr, &si)) return refuse(out, DxvaResult::Invalid, "the slice control buffer is truncated");
    if (si.wBadSliceChopping != 0) return refuse(out, DxvaResult::Unsupported, "a slice split over several bitstream buffers (wBadSliceChopping)");
    if (si.bStartCodeBitOffset != 0) return refuse(out, DxvaResult::Unsupported, "a slice whose start code is not at the first bit of its data");
    const uint64_t bytes = (uint64_t{si.dwSliceBitsInBuffer} + 7) / 8;
    ConstByteView data;
    if (bytes < 5 || !picture.bitstream.try_subview(si.dwSliceDataLocation, bytes, &data)) return refuse(out, DxvaResult::Invalid, "a slice outside the bitstream buffer");
    if (data[0] != 0 || data[1] != 0 || data[2] != 1 || data[3] < 0x01 || data[3] > 0xAF) return refuse(out, DxvaResult::Invalid, "a slice that does not begin with a slice start code");
    if (!slices_.push_back(data)) return refuse(out, DxvaResult::Invalid, "out of memory");
  }

  // --- the pictures: the references (missing ones leave their prediction out), and where this one goes
  bool missing = false;
  auto reference = [&](uint32_t ref_key) -> mpeg2::Picture* {
    Mpeg2Surface* s = surfaces_.find(ref_key);
    if (!s || !s->decoded || s->mb_width != mb_width || s->mb_height != frame_mb_height) {
      missing = true;
      return nullptr;
    }
    surfaces_.touch(ref_key);
    return &s->picture;
  };
  uint32_t keep[2];
  uint32_t keep_count = 0;
  if (needs_forward) keep[keep_count++] = forward_key;
  if (needs_backward) keep[keep_count++] = backward_key;
  bool second_field = false;
  if (pp.bSecondField) {
    Mpeg2Surface* s = surfaces_.find(key);
    second_field = s && s->decoded && s->open_field != 0 && s->open_field != structure && s->mb_width == mb_width && s->mb_height == frame_mb_height;
    if (!second_field) missing = true;   // the first field of the frame is not there: its lines stay grey
  }
  const bool into_reference = !second_field && ((needs_forward && forward_key == key) || (needs_backward && backward_key == key));
  bool fresh = false;
  Mpeg2Surface* target = surfaces_.claim(key, ArrayView<const uint32_t>(keep, keep_count), &fresh);
  if (!target) return refuse(out, DxvaResult::Invalid, "no surface entry is free");
  mpeg2::Core::ExternalPicture ext;
  ext.forward = needs_forward ? reference(forward_key) : nullptr;
  ext.backward = needs_backward ? reference(backward_key) : nullptr;
  ext.second_field = second_field;
  ext.target = into_reference ? &scratch_ : &target->picture;
  if (!second_field) target->decoded = false;

  if (!build_headers(pp, mpeg1, type, mb_width, frame_mb_height, intra, non_intra)) return refuse(out, DxvaResult::Invalid, "out of memory");
  const mpeg2::Core::ExternalStatus status = core_.decode_external(headers_.view(), slices_.view(), ext);
  if (status != mpeg2::Core::ExternalStatus::Decoded) {
    target->decoded = false;
    target->open_field = 0;
    if (status == mpeg2::Core::ExternalStatus::NoSlices) return refuse(out, DxvaResult::Invalid, "no slice of the picture could be decoded");
    if (status == mpeg2::Core::ExternalStatus::NoMemory) return refuse(out, DxvaResult::Invalid, "out of memory");
    return refuse(out, DxvaResult::Invalid, "the picture parameters describe no decodable picture");
  }
  if (into_reference) {
    // The reference was read while decoding; now the surface holds the new picture.
    mpeg2::Picture& p = target->picture;
    const uint64_t w = static_cast<uint64_t>(scratch_.width), h = static_cast<uint64_t>(scratch_.height);
    if (p.width != scratch_.width || p.height != scratch_.height || !p.plane[0].data()) {
      if (!p.plane[0].alloc(allocator_, w * h) || !p.plane[1].alloc(allocator_, w / 2 * (h / 2)) || !p.plane[2].alloc(allocator_, w / 2 * (h / 2))) {
        target->decoded = false;
        return refuse(out, DxvaResult::Invalid, "out of memory");
      }
      p.width = scratch_.width;
      p.height = scratch_.height;
      p.stride[0] = scratch_.width;
      p.stride[1] = p.stride[2] = scratch_.width / 2;
    }
    for (int c = 0; c < 3; ++c) {
      ByteView dst(p.plane[c].data(), p.plane[c].size());
      if (!dst.copy_from(ConstByteView(scratch_.plane[c].data(), scratch_.plane[c].size()))) {
        target->decoded = false;
        return refuse(out, DxvaResult::Invalid, "out of memory");
      }
    }
  }
  target->decoded = true;
  target->mb_width = mb_width;
  target->mb_height = frame_mb_height;
  target->open_field = field && !second_field ? structure : 0;
  for (int i = 0; i < 64; ++i) {
    intra_[i] = intra[i];
    non_intra_[i] = non_intra[i];
  }
  if (!to_nv12(target->picture)) return refuse(out, DxvaResult::Invalid, "out of memory");
  out->frame = out_.frame();
  out->frame.limited_range = true;
  out->frame.keyframe = type == mpeg2::kPictureI;
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_mpeg2(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::Mpeg2Vld && config.profile != DxvaProfile::Mpeg2and1Vld) return {};
  if (config.output != PixelFormat::Nv12 || config.surfaces == 0 || config.surfaces > 128 || config.width == 0 || config.height == 0) return {};
  return UniquePointer<IDxvaDecoder>::make_as<DxvaMpeg2Decoder>(allocator, allocator, config);
}

}  // namespace hardware
