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
// VP9 decoding for a DXVA program (D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0 into NV12, _VP9_VLD_10BIT_PROFILE2 into P010), on the CPU decoder of
// Software/Vp9*.cpp. The bitstream buffer holds one whole frame (its uncompressed and compressed headers and the tiles), which the decoder reads itself: the picture
// parameters are checked against it and name the surfaces - the one the frame goes into (CurrPic), the eight reference slots as they are before the frame
// (ref_frame_map) and the three pictures an inter frame predicts from (frame_refs). Those surfaces' pictures are what the decoder predicts from, whatever its own slots
// would say; a surface named that holds no picture is stood in for by a mid-grey one (MissingReference). Every frame decoded is given back at once, hidden ones too
// (the program shows them later with show_existing_frame, which it handles itself and never sends here).
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesVp9.hpp"
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {

namespace {

// What the first bits of a frame say (the start of the uncompressed header).
struct FrameStart {
  bool valid = false;
  int profile = 0;
  bool show_existing = false, key_frame = false, show_frame = false;
};

FrameStart read_frame_start(ConstByteView frame) {
  FrameStart s;
  vp9::BitReader rb(frame);
  if (rb.literal(2) != 2) return s;
  s.profile = rb.bit();
  s.profile |= rb.bit() << 1;
  if (s.profile == 3 && rb.bit() != 0) return s;   // the reserved zero bit of profile 3
  s.show_existing = rb.bit() != 0;
  if (!s.show_existing) {
    s.key_frame = rb.bit() == 0;
    s.show_frame = rb.bit() != 0;
  }
  s.valid = !rb.error();
  return s;
}

class DxvaVp9Decoder final : public IDxvaDecoder {
 public:
  DxvaVp9Decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) : config_(config), core_(allocator), out_(allocator) {}
  ~DxvaVp9Decoder() = default;
  DxvaVp9Decoder(const DxvaVp9Decoder&) = delete;
  DxvaVp9Decoder& operator=(const DxvaVp9Decoder&) = delete;

  DxvaProfile profile() const override { return config_.profile; }
  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override;
  void reset() override {
    core_.reset();   // (every hold goes with it)
    for (Surface& s : surfaces_) s = Surface();
    last_frame_ = -1;
  }

 private:
  // A surface's picture: the pool frame decoded into the surface of this key, held while a later frame may still name it.
  struct Surface {
    bool used = false;
    uint32_t key = 0;
    int frame = -1;
  };
  static constexpr int kSurfaces = 2 * vp9::Core::kPoolSize;

  [[nodiscard]] bool key_of(const DxvaPicture& picture, dxva::PicEntryVpx entry, uint32_t* key) const {
    return dxva::surface_key(picture, config_.surfaces, entry.index(), key);
  }
  Surface* find(uint32_t key) {
    for (Surface& s : surfaces_)
      if (s.used && s.key == key) return &s;
    return nullptr;
  }
  [[nodiscard]] bool convert(int index);

  DxvaDecoderConfig config_;
  vp9::Core core_;
  VideoFrameBuffer out_;
  Surface surfaces_[kSurfaces];
  int last_frame_ = -1;   // the frame decoded last, held for its motion vectors (the next frame may predict them from it)
};

DxvaResult DxvaVp9Decoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  *out = DxvaDecodedPicture();
  auto refuse = [out](DxvaResult result, const char* detail) {
    out->detail = detail;
    return result;
  };
  dxva::PicParamsVp9 pp;
  if (!dxva::read_vp9_struct(picture.picture_parameters, 0, &pp)) return refuse(DxvaResult::Invalid, "the picture parameters are shorter than DXVA_PicParams_VP9");
  dxva::SliceVpxShort slice;
  if (!dxva::read_vp9_struct(picture.slice_control, 0, &slice)) return refuse(DxvaResult::Invalid, "no DXVA_Slice_VPx_Short in the slice control buffer");
  if (slice.wBadSliceChopping != 0) return refuse(DxvaResult::Unsupported, "a frame split over several bitstream buffers (wBadSliceChopping)");
  ConstByteView frame;
  if (slice.SliceBytesInBuffer == 0 || !picture.bitstream.try_subview(slice.BSNALunitDataLocation, slice.SliceBytesInBuffer, &frame))
    return refuse(DxvaResult::Invalid, "the frame's bytes are not inside the bitstream buffer");

  // The surfaces.
  uint32_t current_key = 0;
  // (Kept under DxvaPicture::output_key when the program gives one: the index is then not looked up. A reference with that key is the picture there before this one,
  // which is what the table holds until this frame is decoded.)
  if (pp.CurrPic.none() || !dxva::output_surface_key(picture, config_.surfaces, pp.CurrPic.index(), &current_key))
    return refuse(DxvaResult::Invalid, "CurrPic names no surface");
  out->surface = pp.CurrPic.index();

  // The format: what the profile takes, and what the parameters and the frame say agree.
  const uint16_t flags = pp.wFormatAndPictureInfoFlags;
  const bool ten_bit = config_.profile == DxvaProfile::Vp9Vld10BitProfile2;
  const int bit_depth = ten_bit ? 10 : 8;
  if (pp.profile != (ten_bit ? 2 : 0) || pp.BitDepthMinus8Luma != bit_depth - 8 || pp.BitDepthMinus8Chroma != bit_depth - 8)
    return refuse(DxvaResult::Invalid, ten_bit ? "the decoder takes VP9 profile 2 at 10 bits" : "the decoder takes VP9 profile 0 (8 bits)");
  if (!(flags & dxva::kVp9SubsamplingX) || !(flags & dxva::kVp9SubsamplingY) || (flags & dxva::kVp9ExtraPlane))
    return refuse(DxvaResult::Invalid, "the decoder takes 4:2:0 only");
  const uint32_t max_w = (config_.width + 15) & ~15u, max_h = (config_.height + 15) & ~15u;
  if (pp.width == 0 || pp.height == 0 || pp.width > max_w || pp.height > max_h) return refuse(DxvaResult::Invalid, "the frame does not fit the decoder's surfaces");
  const FrameStart start = read_frame_start(frame);
  if (!start.valid) return refuse(DxvaResult::Invalid, "the bitstream does not start with a VP9 frame header");
  if (start.show_existing) return refuse(DxvaResult::Invalid, "a show_existing_frame header (the program shows that frame itself)");
  if (start.profile != pp.profile) return refuse(DxvaResult::Invalid, "the frame's profile is not the parameters' profile");
  const bool key_frame = !(flags & dxva::kVp9FrameType);
  if (start.key_frame != key_frame || start.show_frame != ((flags & dxva::kVp9ShowFrame) != 0))
    return refuse(DxvaResult::Invalid, "frame_type or show_frame differ from the frame header");
  const bool inter = !key_frame && !(flags & dxva::kVp9IntraOnly);

  // The references: the slots before this frame, and the three pictures of an inter frame (a stand-in for one whose surface holds nothing).
  vp9::Core::ExternalReferences refs;
  for (int i = 0; i < 8; ++i) {
    if (pp.ref_frame_map[i].none()) continue;
    uint32_t key = 0;
    if (!key_of(picture, pp.ref_frame_map[i], &key)) return refuse(DxvaResult::Invalid, "ref_frame_map names no surface");
    if (const Surface* s = find(key)) refs.slot[i] = s->frame;
  }
  int stand_ins[3] = {-1, -1, -1};
  bool missing = false;
  auto release_stand_ins = [&]() {
    for (int& f : stand_ins) {
      core_.release_frame_hold(f);
      f = -1;
    }
  };
  if (inter) {
    for (int i = 0; i < 3; ++i) {
      uint32_t key = 0;
      if (pp.frame_refs[i].none() || !key_of(picture, pp.frame_refs[i], &key)) {
        release_stand_ins();
        return refuse(DxvaResult::Invalid, "an inter frame's frame_refs names no surface");
      }
      if (const Surface* s = find(key)) {
        refs.frame_ref[i] = s->frame;
        continue;
      }
      // The size the program says that picture has (the slot that names the same surface), else this frame's.
      uint32_t w = pp.width, h = pp.height;
      for (int slot = 0; slot < 8; ++slot)
        if (pp.ref_frame_map[slot].bPicEntry == pp.frame_refs[i].bPicEntry && pp.ref_frame_coded_width[slot] > 0 && pp.ref_frame_coded_height[slot] > 0 &&
            pp.ref_frame_coded_width[slot] <= 2 * max_w && pp.ref_frame_coded_height[slot] <= 2 * max_h) {
          w = pp.ref_frame_coded_width[slot];
          h = pp.ref_frame_coded_height[slot];
          break;
        }
      stand_ins[i] = core_.make_blank_frame(static_cast<int>(w), static_cast<int>(h), 1, 1, bit_depth);
      if (stand_ins[i] < 0) {
        release_stand_ins();
        return refuse(DxvaResult::Invalid, "no memory for a stand-in reference picture");
      }
      refs.frame_ref[i] = stand_ins[i];
      missing = true;
    }
  }

  core_.set_external_references(&refs);
  int shown = -1;
  vp9::FrameHeaderInfo info;
  const bool decoded = core_.decode_frame(frame, &shown, &info);
  core_.set_external_references(nullptr);
  release_stand_ins();
  if (!decoded) return refuse(DxvaResult::Invalid, "the frame could not be decoded (damaged data, or an inter frame before any key frame)");
  const int index = core_.decoded_frame();
  if (info.width != static_cast<int>(pp.width) || info.height != static_cast<int>(pp.height) || info.bit_depth != bit_depth || info.profile != pp.profile)
    return refuse(DxvaResult::Invalid, "the frame header's size or bit depth differ from the picture parameters");
  if (index < 0 || index >= vp9::Core::kPoolSize || !convert(index)) return refuse(DxvaResult::Invalid, "no memory for the decoded picture");

  // Keep the picture under its surface's key, and the frame for the next one's motion vectors; let go of every surface the next frame can no longer name (those
  // neither in this frame's slots nor this frame's own: VP9 can only refer to what the slots hold).
  Surface* target = find(current_key);
  if (!target) {
    for (Surface& s : surfaces_)
      if (!s.used) {
        target = &s;
        break;
      }
  }
  if (target) {   // (always found: at most nine surfaces are kept)
    core_.release_frame_hold(target->frame);
    target->used = true;
    target->key = current_key;
    target->frame = index;
    core_.hold_frame(index);
  }
  core_.release_frame_hold(last_frame_);
  last_frame_ = index;
  core_.hold_frame(index);
  for (Surface& s : surfaces_) {
    if (!s.used || s.key == current_key) continue;
    bool named = false;
    for (const dxva::PicEntryVpx& e : pp.ref_frame_map) {
      uint32_t key = 0;
      if (!e.none() && key_of(picture, e, &key) && key == s.key) named = true;
    }
    if (!named) {
      core_.release_frame_hold(s.frame);
      s = Surface();
    }
  }

  out->frame = out_.frame(0, info.key_frame);
  out->frame.limited_range = !core_.frame(index).full_range;
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

// The pool picture, whole (the frame's size as coded), as NV12 or P010 (the 10-bit value in the top bits of each 16-bit word).
bool DxvaVp9Decoder::convert(int index) {
  const vp9::Frame& f = core_.frame(index);
  const bool p010 = config_.output == PixelFormat::P010;
  const VideoFormat format{static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height), p010 ? PixelFormat::P010 : PixelFormat::Nv12};
  if (!format.valid() || f.ss_x != 1 || f.ss_y != 1 || f.bit_depth != (p010 ? 10 : 8) || !out_.allocate(format)) return false;
  const int shift = p010 ? 6 : 0;
  auto put = [&](ByteView row, uint32_t x, uint16_t v) {
    if (p010) {
      const uint32_t w = static_cast<uint32_t>(v) << shift;
      row[2 * x] = static_cast<uint8_t>(w);
      row[2 * x + 1] = static_cast<uint8_t>(w >> 8);
    } else {
      row[x] = static_cast<uint8_t>(v);
    }
  };
  const ByteView luma = out_.plane(0), chroma = out_.plane(1);
  for (uint32_t y = 0; y < format.height; ++y) {
    const ByteView row = luma.subview(uint64_t{y} * format.plane_stride(0), format.plane_stride(0));
    const ArrayView<uint16_t> src = f.plane[0].subview(uint64_t{y} * f.stride[0], format.width);
    for (uint32_t x = 0; x < format.width; ++x) put(row, x, src[x]);
  }
  const uint32_t cw = format.chroma_width(), ch = format.chroma_height();
  for (uint32_t y = 0; y < ch; ++y) {
    const ByteView row = chroma.subview(uint64_t{y} * format.plane_stride(1), format.plane_stride(1));
    const ArrayView<uint16_t> u = f.plane[1].subview(uint64_t{y} * f.stride[1], cw), v = f.plane[2].subview(uint64_t{y} * f.stride[2], cw);
    for (uint32_t x = 0; x < cw; ++x) {
      put(row, 2 * x, u[x]);
      put(row, 2 * x + 1, v[x]);
    }
  }
  return true;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_vp9(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::Vp9VldProfile0 && config.profile != DxvaProfile::Vp9Vld10BitProfile2) return {};
  const PixelFormat wanted = config.profile == DxvaProfile::Vp9Vld10BitProfile2 ? PixelFormat::P010 : PixelFormat::Nv12;
  if (config.output != wanted || config.surfaces == 0 || config.surfaces > 128) return {};
  return UniquePointer<IDxvaDecoder>::make_as<DxvaVp9Decoder>(allocator, allocator, config);
}

}  // namespace hardware
