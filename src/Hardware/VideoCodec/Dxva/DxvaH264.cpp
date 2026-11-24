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
// The DXVA H.264 decoder (D3D11_DECODER_PROFILE_H264_VLD_NOFGT, short slice format) on the CPU decoder of Software/H264*.cpp. The program sends, per picture,
// DXVA_PicParams_H264, DXVA_Qmatrix_H264 and the slice NAL units with their start codes. This rebuilds the sequence and picture parameter sets the parameters stand for
// (sent to the decoder again only when they change), and hands them and the slices to the decoder as one access unit together with the program's decoded picture buffer:
// the references of RefFrameList with their frame numbers (FrameNumList), long-term status and order counts (FieldOrderCntList), and the current picture's order
// counts (CurrFieldOrderCnt). The decoder builds each slice's reference lists from those (its own reference marking and order counting are not used, so
// pic_order_cnt_type 1, whose offsets DXVA does not carry, decodes as well as the others) and keeps every decoded picture under the key of its surface. The picture
// comes back at once, at its whole coded size, as NV12.
//
// Interlaced coding: an MBAFF frame (MbaffFrameFlag), or a field picture (field_pic_flag, CurrPic's AssociatedFlag the bottom field), whose references are the fields
// UsedForReferenceFlags marks (the program lists the frame of the current picture when its first field is one). A field is decoded into the frame of its surface:
// the decoded picture says which field's lines are the picture (DxvaDecodedPicture::field), and the second field of a pair goes in beside the first. Monochrome
// (chroma_format_idc 0) comes out as NV12 with mid-grey chroma.
//
// Refused: flexible macroblock ordering, slices chopped over buffers.
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesH264.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {

using dxva::PicParamsH264;
using dxva::QmatrixH264;
using dxva::SliceH264Short;

constexpr uint32_t kMaxSlices = 36864;   // one per macroblock of the largest picture the CPU decoder takes
constexpr uint32_t kMaxMbs = 36864;      // (h264::kMaxMbs: 4096x2304)
constexpr uint32_t kMaxMbsPerSide = 1024;

// Writes the bits of a rebuilt NAL unit (most significant first) and, at the end, the NAL unit with its start code and emulation prevention bytes.
class NalWriter {
 public:
  void bits(uint32_t value, int count) {   // count <= 32
    for (int i = count - 1; i >= 0; --i) {
      acc_ = static_cast<uint8_t>((acc_ << 1) | ((value >> i) & 1));
      if (++filled_ == 8) push();
    }
  }
  void ue(uint32_t value) {
    const uint64_t v = uint64_t{value} + 1;
    int len = 0;
    while ((v >> len) > 1) ++len;
    bits(0, len);
    bits(static_cast<uint32_t>(v), len + 1);
  }
  void se(int32_t value) { ue(value > 0 ? static_cast<uint32_t>(2 * value - 1) : static_cast<uint32_t>(-2 * static_cast<int64_t>(value))); }
  // rbsp_trailing_bits, then the NAL unit (header byte `header`) into `out` (replacing what it held).
  [[nodiscard]] bool finish(uint8_t header, ByteBuffer& out) {
    bits(1, 1);
    while (filled_ != 0) bits(0, 1);
    out.clear();
    const uint8_t start[4] = {0, 0, 0, 1};
    bool ok = !overflow_ && out.append(ConstByteView(start, 4)) && out.put8(header);
    int zeros = 0;
    for (uint32_t i = 0; i < size_ && ok; ++i) {
      const uint8_t b = rbsp_[i];
      if (zeros >= 2 && b <= 3) {
        ok = out.put8(3);
        zeros = 0;
      }
      ok = ok && out.put8(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
    return ok;
  }

 private:
  void push() {
    if (size_ < sizeof(rbsp_)) rbsp_[size_++] = acc_;
    else overflow_ = true;
    acc_ = 0;
    filled_ = 0;
  }
  uint8_t rbsp_[2048] = {};   // an SPS with all eight scaling lists is well under 1 KB
  uint32_t size_ = 0;
  uint8_t acc_ = 0;
  int filled_ = 0;
  bool overflow_ = false;
};

// 7.3.2.1.1.1: one scaling list, delta coded from the values (in the bitstream's order) DXVA gives.
void write_scaling_list(NalWriter& w, ArrayView<const uint8_t> list) {
  int last = 8;
  for (uint8_t v : list) {
    int delta = static_cast<int>(v) - last;
    if (delta > 127) delta -= 256;
    if (delta < -128) delta += 256;
    w.se(delta);
    last = v;
  }
}

// The start of a slice header (7.3.3): pic_parameter_set_id, and field_pic_flag / bottom_field_flag when the frames are interlaced (`frame_num_bits` is
// log2_max_frame_num). `nal_payload` is the bytes after the header byte; emulation prevention is undone for the few bytes read.
struct SliceStart {
  uint32_t pps_id = 0;
  bool field_pic = false, bottom_field = false;
};
[[nodiscard]] bool slice_start(ConstByteView nal_payload, uint32_t frame_num_bits, bool frame_mbs_only, SliceStart* out) {
  uint8_t rbsp[24];
  uint32_t n = 0;
  int zeros = 0;
  for (uint8_t b : nal_payload) {
    if (n == sizeof(rbsp)) break;
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    rbsp[n++] = b;
    zeros = b == 0 ? zeros + 1 : 0;
  }
  uint64_t pos = 0;
  const uint64_t limit = uint64_t{n} * 8;
  auto ue = [&](uint32_t* v) {
    int lz = 0;
    for (;;) {
      if (pos >= limit || lz > 31) return false;
      const uint32_t bit = (rbsp[pos >> 3] >> (7 - (pos & 7))) & 1;
      ++pos;
      if (bit) break;
      ++lz;
    }
    uint64_t value = 0;
    for (int i = 0; i < lz; ++i) {
      if (pos >= limit) return false;
      value = (value << 1) | ((rbsp[pos >> 3] >> (7 - (pos & 7))) & 1);
      ++pos;
    }
    *v = static_cast<uint32_t>((uint64_t{1} << lz) - 1 + value);
    return true;
  };
  auto bit = [&](uint32_t* v) {
    if (pos >= limit) return false;
    *v = (rbsp[pos >> 3] >> (7 - (pos & 7))) & 1;
    ++pos;
    return true;
  };
  uint32_t first_mb = 0, type = 0;
  if (!ue(&first_mb) || !ue(&type) || !ue(&out->pps_id) || out->pps_id > 255) return false;
  if (frame_mbs_only) return true;
  if (frame_num_bits > limit || pos > limit - frame_num_bits) return false;
  pos += frame_num_bits;   // frame_num
  uint32_t field = 0, bottom = 0;
  if (!bit(&field)) return false;
  if (field && !bit(&bottom)) return false;
  out->field_pic = field != 0;
  out->bottom_field = bottom != 0;
  return true;
}

bool same_bytes(ConstByteView a, ConstByteView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}

class DxvaH264Decoder final : public IDxvaDecoder {
 public:
  DxvaH264Decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config)
      : config_(config), decoder_(allocator), au_(allocator), au_input_(allocator), sps_(allocator), pps_(allocator), sent_sps_(allocator), sent_pps_(allocator), out_(allocator) {}

  DxvaProfile profile() const override { return config_.profile; }

  void reset() override {
    decoder_.reset();
    sent_sps_.clear();
    sent_pps_.clear();
  }

  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override {
    out->detail = nullptr;
    out->frame = VideoFrame{};
    const DxvaResult r = decode_picture(picture, out);
    if (r == DxvaResult::Invalid || r == DxvaResult::Unsupported) out->frame = VideoFrame{};
    return r;
  }

 private:
  DxvaResult refuse(DxvaDecodedPicture* out, DxvaResult result, const char* detail) {
    out->detail = detail;
    return result;
  }

  DxvaResult decode_picture(const DxvaPicture& picture, DxvaDecodedPicture* out) {
    if (picture.picture_parameters.size() < sizeof(PicParamsH264)) return refuse(out, DxvaResult::Invalid, "the picture parameters are shorter than DXVA_PicParams_H264");
    PicParamsH264 pp;
    if (!dxva::read_h264_picture_parameters(picture.picture_parameters, &pp)) return refuse(out, DxvaResult::Invalid, "the picture parameters cannot be read");
    if (pp.CurrPic == dxva::kH264NoPicture) return refuse(out, DxvaResult::Invalid, "CurrPic names no surface");
    const uint32_t index = dxva::h264_pic_index(pp.CurrPic);
    uint32_t key = 0;
    if (!dxva::output_surface_key(picture, config_.surfaces, index, &key)) return refuse(out, DxvaResult::Invalid, "CurrPic is outside the surfaces");
    out->surface = index;

    const uint16_t f = pp.wBitFields;
    const uint32_t chroma_format = (f >> dxva::kH264ChromaFormatShift) & 3;
    if (chroma_format > 1 || pp.bit_depth_luma_minus8 != 0 || pp.bit_depth_chroma_minus8 != 0 || (f & dxva::kH264ResidualColourTransform))
      return refuse(out, DxvaResult::Invalid, "not 8-bit 4:2:0 or monochrome, which the profile takes");
    if (pp.ContinuationFlag != 1) return refuse(out, DxvaResult::Invalid, "ContinuationFlag is not 1");
    if (pp.num_slice_groups_minus1 != 0) return refuse(out, DxvaResult::Unsupported, "flexible macroblock ordering (num_slice_groups_minus1 > 0)");
    if (pp.num_ref_frames > 16 || pp.log2_max_frame_num_minus4 > 12 || pp.pic_order_cnt_type > 2 || pp.log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        pp.num_ref_idx_l0_active_minus1 > 31 || pp.num_ref_idx_l1_active_minus1 > 31 || pp.pic_init_qp_minus26 < -26 || pp.pic_init_qp_minus26 > 25 ||
        pp.pic_init_qs_minus26 < -26 || pp.pic_init_qs_minus26 > 25 || pp.chroma_qp_index_offset < -12 || pp.chroma_qp_index_offset > 12 ||
        pp.second_chroma_qp_index_offset < -12 || pp.second_chroma_qp_index_offset > 12 || ((f >> dxva::kH264WeightedBipredShift) & 3) == 3 ||
        pp.delta_pic_order_always_zero_flag > 1 || pp.direct_8x8_inference_flag > 1 || pp.entropy_coding_mode_flag > 1 || pp.pic_order_present_flag > 1 ||
        pp.deblocking_filter_control_present_flag > 1 || pp.redundant_pic_cnt_present_flag > 1)
      return refuse(out, DxvaResult::Invalid, "a picture parameter is out of its range");
    const bool frame_mbs_only = (f & dxva::kH264FrameMbsOnly) != 0;
    const bool field_pic = (f & dxva::kH264FieldPic) != 0, mbaff_frame = (f & dxva::kH264MbaffFrame) != 0;
    if ((field_pic || mbaff_frame) && frame_mbs_only) return refuse(out, DxvaResult::Invalid, "a field picture or an MBAFF frame where frame_mbs_only_flag is 1");
    if (field_pic && mbaff_frame) return refuse(out, DxvaResult::Invalid, "MbaffFrameFlag set for a field picture");
    const uint32_t mb_w = uint32_t{pp.wFrameWidthInMbsMinus1} + 1, mb_h = uint32_t{pp.wFrameHeightInMbsMinus1} + 1;
    if (!frame_mbs_only && (mb_h & 1)) return refuse(out, DxvaResult::Invalid, "the frame height is not a whole number of field macroblock pairs");
    if (mb_w * 16 > (config_.width + 15) / 16 * 16 || mb_h * 16 > (config_.height + 15) / 16 * 16)
      return refuse(out, DxvaResult::Invalid, "the picture is larger than the decoder's surfaces");
    if (mb_w > kMaxMbsPerSide || mb_h > kMaxMbsPerSide || mb_w * mb_h > kMaxMbs) return refuse(out, DxvaResult::Unsupported, "pictures larger than 4096x2304");
    const uint32_t max_frame_num = 1u << (pp.log2_max_frame_num_minus4 + 4);

    // The program's decoded picture buffer.
    H264Decoder::ExternalReference refs[16];
    uint32_t ref_count = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      const uint8_t entry = pp.RefFrameList[i];
      const uint32_t used = (pp.UsedForReferenceFlags >> (2 * i)) & 3;
      if (entry == dxva::kH264NoPicture || used == 0) continue;
      H264Decoder::ExternalReference& r = refs[ref_count++];
      r.fields = static_cast<uint8_t>(used);   // (a field picture can use one field of a frame, the first field of its own frame among them)
      if (!dxva::surface_key(picture, config_.surfaces, dxva::h264_pic_index(entry), &r.key)) return refuse(out, DxvaResult::Invalid, "a reference is outside the surfaces");
      r.long_term = dxva::h264_pic_flag(entry);
      r.non_existing = ((pp.NonExistingFrameFlags >> i) & 1) != 0;
      r.frame_num = pp.FrameNumList[i];
      if (r.long_term ? r.frame_num > 15 : static_cast<uint32_t>(r.frame_num) >= max_frame_num)
        return refuse(out, DxvaResult::Invalid, "a reference's FrameNumList entry is out of range");
      r.top_poc = pp.FieldOrderCntList[i][0];
      r.bottom_poc = pp.FieldOrderCntList[i][1];
    }

    // The scaling lists in force (DXVA always sends the final ones, in zigzag order; none: flat).
    QmatrixH264 qm;
    bool flat = true;
    if (!picture.quantization_matrix.empty()) {
      if (!dxva::read_h264_qmatrix(picture.quantization_matrix, &qm)) return refuse(out, DxvaResult::Invalid, "the matrix buffer is shorter than DXVA_Qmatrix_H264");
      for (const auto& list : qm.bScalingLists4x4)
        for (uint8_t v : list) {
          if (v == 0) return refuse(out, DxvaResult::Invalid, "a scaling list entry is 0");
          flat = flat && v == 16;
        }
      for (int l = 0; l < 2; ++l)
        for (uint8_t v : qm.bScalingLists8x8[l]) {
          if (v == 0) return refuse(out, DxvaResult::Invalid, "a scaling list entry is 0");
          if (f & dxva::kH264Transform8x8) flat = flat && v == 16;
        }
    }

    // The slices.
    const uint64_t slice_bytes = picture.slice_control.size();
    if (slice_bytes == 0 || slice_bytes % sizeof(SliceH264Short) != 0 || slice_bytes / sizeof(SliceH264Short) > kMaxSlices)
      return refuse(out, DxvaResult::Invalid, "the slice control buffer is not a whole number of DXVA_Slice_H264_Short");
    if (picture.bitstream.empty()) return refuse(out, DxvaResult::Invalid, "no bitstream");
    au_.clear();
    ByteReader slices(picture.slice_control);
    int64_t pps_id = -1;
    while (!slices.at_end()) {
      SliceH264Short s;
      if (!dxva::read_h264_slice(slices, &s)) return refuse(out, DxvaResult::Invalid, "the slice control buffer cannot be read");
      if (s.wBadSliceChopping != 0) return refuse(out, DxvaResult::Unsupported, "slices chopped over several bitstream buffers");
      if (s.BSNALunitDataLocation > picture.bitstream.size() || s.SliceBytesInBuffer > picture.bitstream.size() - s.BSNALunitDataLocation)
        return refuse(out, DxvaResult::Invalid, "a slice is outside the bitstream");
      ConstByteView data = picture.bitstream.subview(s.BSNALunitDataLocation, s.SliceBytesInBuffer);
      uint64_t start = 0;
      if (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) start = 4;
      else if (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) start = 3;
      else return refuse(out, DxvaResult::Invalid, "a slice does not begin with a start code");
      if (data.size() < start + 2) return refuse(out, DxvaResult::Invalid, "a slice is too short");
      const uint8_t header = data[start];
      const uint32_t type = header & 31;
      if ((header & 0x80) || (type != 1 && type != 5)) return refuse(out, DxvaResult::Invalid, "a slice entry does not hold a coded slice NAL unit");
      // Emulation prevention keeps 00 00 01 out of a NAL unit: one found further on starts another, which must be a slice too (not, say, a parameter set).
      for (uint64_t k = start + 1; k + 3 < data.size(); ++k)
        if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) {
          const uint32_t t = data[k + 3] & 31;
          if (t != 1 && t != 5) return refuse(out, DxvaResult::Invalid, "a slice entry holds a NAL unit that is not a coded slice");
        }
      SliceStart ss;
      if (!slice_start(data.subview(start + 1), pp.log2_max_frame_num_minus4 + 4u, frame_mbs_only, &ss)) return refuse(out, DxvaResult::Invalid, "a slice header cannot be read");
      if (pps_id >= 0 && static_cast<uint32_t>(pps_id) != ss.pps_id) return refuse(out, DxvaResult::Invalid, "the slices name different picture parameter sets");
      if (ss.field_pic != field_pic || (field_pic && ss.bottom_field != dxva::h264_pic_flag(pp.CurrPic)))
        return refuse(out, DxvaResult::Invalid, "a slice is of another picture structure than field_pic_flag and CurrPic say");
      pps_id = ss.pps_id;
      if (!au_.append(data)) return refuse(out, DxvaResult::Invalid, "out of memory");
    }

    // The parameter sets, sent again only when they change.
    {
      NalWriter w;
      w.bits(100, 8);   // profile_idc: High
      w.bits(0, 8);
      w.bits(51, 8);    // level_idc (the decoder sizes no output queue in this mode)
      w.ue(0);          // seq_parameter_set_id
      w.ue(chroma_format);   // chroma_format_idc: 1, or 0 (monochrome)
      w.ue(0);
      w.ue(0);
      w.bits(0, 1);     // qpprime_y_zero_transform_bypass_flag
      w.bits(flat ? 0 : 1, 1);
      if (!flat) {
        for (int i = 0; i < 6; ++i) {
          w.bits(1, 1);
          write_scaling_list(w, ArrayView<const uint8_t>(qm.bScalingLists4x4[i], 16));
        }
        for (int i = 0; i < 2; ++i) {
          w.bits(1, 1);
          write_scaling_list(w, ArrayView<const uint8_t>(qm.bScalingLists8x8[i], 64));
        }
      }
      w.ue(pp.log2_max_frame_num_minus4);
      w.ue(pp.pic_order_cnt_type);
      if (pp.pic_order_cnt_type == 0) {
        w.ue(pp.log2_max_pic_order_cnt_lsb_minus4);
      } else if (pp.pic_order_cnt_type == 1) {
        // The offsets are not in DXVA; the order counts come from the program instead.
        w.bits(pp.delta_pic_order_always_zero_flag, 1);
        w.se(0);
        w.se(0);
        w.ue(0);
      }
      w.ue(pp.num_ref_frames);
      w.bits(1, 1);     // gaps_in_frame_num_value_allowed_flag
      w.ue(mb_w - 1);
      w.ue(frame_mbs_only ? mb_h - 1 : mb_h / 2 - 1);
      w.bits(frame_mbs_only ? 1 : 0, 1);
      // mb_adaptive_frame_field_flag: DXVA says MbaffFrameFlag of a frame only (a field picture decodes the same either way), so a field picture keeps the
      // flag the last frame had and the parameter set stays the same through a sequence of fields and MBAFF frames.
      if (!field_pic) mbaff_ = mbaff_frame;
      if (!frame_mbs_only) w.bits(mbaff_ ? 1 : 0, 1);
      w.bits(pp.direct_8x8_inference_flag, 1);
      w.bits(0, 1);     // frame_cropping_flag: the whole coded size
      w.bits(0, 1);     // vui_parameters_present_flag
      if (!w.finish(0x67, sps_)) return refuse(out, DxvaResult::Invalid, "out of memory");
    }
    {
      NalWriter w;
      w.ue(static_cast<uint32_t>(pps_id));
      w.ue(0);
      w.bits(pp.entropy_coding_mode_flag, 1);
      w.bits(pp.pic_order_present_flag, 1);
      w.ue(0);          // num_slice_groups_minus1
      w.ue(pp.num_ref_idx_l0_active_minus1);
      w.ue(pp.num_ref_idx_l1_active_minus1);
      w.bits((f & dxva::kH264WeightedPred) ? 1 : 0, 1);
      w.bits((f >> dxva::kH264WeightedBipredShift) & 3, 2);
      w.se(pp.pic_init_qp_minus26);
      w.se(pp.pic_init_qs_minus26);
      w.se(pp.chroma_qp_index_offset);
      w.bits(pp.deblocking_filter_control_present_flag, 1);
      w.bits((f & dxva::kH264ConstrainedIntraPred) ? 1 : 0, 1);
      w.bits(pp.redundant_pic_cnt_present_flag, 1);
      w.bits((f & dxva::kH264Transform8x8) ? 1 : 0, 1);
      w.bits(0, 1);     // pic_scaling_matrix_present_flag: the lists are the sequence's
      w.se(pp.second_chroma_qp_index_offset);
      if (!w.finish(0x68, pps_)) return refuse(out, DxvaResult::Invalid, "out of memory");
    }
    au_input_.clear();
    const bool new_sps = !same_bytes(sps_.view(), sent_sps_.view()), new_pps = new_sps || !same_bytes(pps_.view(), sent_pps_.view());
    if ((new_sps && !au_input_.append(sps_.view())) || (new_pps && !au_input_.append(pps_.view())) || !au_input_.append(au_.view()))
      return refuse(out, DxvaResult::Invalid, "out of memory");

    H264Decoder::ExternalPicture ext;
    ext.key = key;
    ext.top_poc = pp.CurrFieldOrderCnt[0];
    ext.bottom_poc = pp.CurrFieldOrderCnt[1];
    ext.references = ArrayView<const H264Decoder::ExternalReference>(refs, ref_count);
    VideoFrame decoded;
    int structure = 3;
    const H264Decoder::ExternalStatus status = decoder_.decode_external(au_input_.view(), ext, &decoded, &structure);
    // The decoder has the parameter sets now (they were parsed even when no slice decoded).
    if (new_sps) {
      sent_sps_.clear();
      if (!sent_sps_.append(sps_.view())) sent_sps_.clear();
    }
    if (new_pps) {
      sent_pps_.clear();
      if (!sent_pps_.append(pps_.view())) sent_pps_.clear();
    }
    if (status == H264Decoder::ExternalStatus::NotDecoded) return refuse(out, DxvaResult::Invalid, "no slice of the picture could be decoded");
    if (status == H264Decoder::ExternalStatus::NoMemory) return refuse(out, DxvaResult::Invalid, "out of memory");
    const VideoFormat format{decoded.format.width, decoded.format.height, PixelFormat::Nv12};
    if (!out_.allocate(format) || !convert_frame(decoded, out_)) return refuse(out, DxvaResult::Invalid, "out of memory");
    out->frame = out_.frame(0, decoded.keyframe);
    out->frame.limited_range = decoded.limited_range;
    out->field = static_cast<uint8_t>(structure == 3 ? 0 : structure);
    if (status == H264Decoder::ExternalStatus::MissingReference) {
      out->detail = "a reference names a surface that holds no decoded picture";
      return DxvaResult::MissingReference;
    }
    return DxvaResult::Ok;
  }

  DxvaDecoderConfig config_;
  H264Decoder decoder_;
  ByteBuffer au_;          // the slices of the picture
  ByteBuffer au_input_;    // what the decoder is given: parameter sets that changed, then the slices
  ByteBuffer sps_, pps_;   // the parameter sets of this picture
  ByteBuffer sent_sps_, sent_pps_;   // the ones the decoder has
  VideoFrameBuffer out_;
  bool mbaff_ = false;     // mb_adaptive_frame_field_flag of the parameter set (see above)
};

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_h264(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::H264VldNoFgt || config.output != PixelFormat::Nv12 || config.surfaces == 0 || config.surfaces > 128 || config.width == 0 ||
      config.height == 0)
    return {};
  return UniquePointer<IDxvaDecoder>::make_as<DxvaH264Decoder>(allocator, allocator, config);
}

}  // namespace hardware
