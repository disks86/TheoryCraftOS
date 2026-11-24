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
// Decoding as a DXVA driver does it (what Direct3D 11's ID3D11VideoDecoder and Direct3D 12's ID3D12VideoDecoder hand the kernel, src/Kernel/Dxgkrnl): the program parses the
// stream itself and gives, for each picture, the DXVA picture parameters, the inverse quantization matrices, the slice controls and the compressed data, and names the
// surface the picture goes into (an index: the CurrPic / CurrPicTextureIndex of the parameters) and the surfaces its references are in. decode() reconstructs that one
// picture and gives it back at once, in decoding order - not display order: the program displays the surfaces in the order it decides.
//
// The structures are DXVA's (dxva.h: DXVA_PicParams_H264, DXVA_Qmatrix_H264, DXVA_Slice_H264_Short, DXVA_PictureParameters / DXVA_SliceInfo for MPEG-2,
// DXVA_PicParams_VP9 / DXVA_Slice_VPx_Short, DXVA_PicParams_HEVC / DXVA_Qmatrix_HEVC / DXVA_Slice_HEVC_Short, DXVA_PicParams_AV1 / DXVA_Tile_AV1), byte for byte as a
// 64-bit or 32-bit Windows program lays them out (DxvaTypes.hpp mirrors them; they have the same layout in both). Every decoder takes the short slice format
// (ConfigBitstreamRaw 2 for H.264 and HEVC: the slice NAL units whole, with their start codes).
//
// A decoder keeps what it needs of earlier pictures itself, by the surface index each was decoded into; a reference names one of those. A program that decodes into a
// surface a picture is still referenced from gets the new picture there (as on hardware: the surface is overwritten).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The decoder profiles (the DXVA / D3D11_DECODER_PROFILE_* GUIDs, by what they are).
enum class DxvaProfile : uint8_t {
  H264VldNoFgt,        // D3D11_DECODER_PROFILE_H264_VLD_NOFGT {1b81be68-a0c7-11d3-b984-00c04f2e73c5}: 8-bit 4:2:0 -> NV12
  Mpeg2Vld,            // D3D11_DECODER_PROFILE_MPEG2_VLD {ee27417f-5e28-4e65-beea-1d26b508adc9}: -> NV12
  Mpeg2and1Vld,        // D3D11_DECODER_PROFILE_MPEG2and1_VLD {86695f12-340e-4f04-9fd3-9253dd327460}: MPEG-1 and MPEG-2 -> NV12
  Vp9VldProfile0,      // D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0 {463707f8-a1d0-4585-876d-83aa6d60b89e}: 8-bit 4:2:0 -> NV12
  Vp9Vld10BitProfile2, // D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2 {a4c749ef-6ecf-48aa-8448-50a7a1165ff7}: 10-bit 4:2:0 -> P010
  HevcVldMain,         // D3D11_DECODER_PROFILE_HEVC_VLD_MAIN {5b11d51b-2f4c-4452-bcc3-09f2a1160cc0}: 8-bit 4:2:0 -> NV12
  Av1VldProfile0,      // D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0 {b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a}: 8-bit (NV12) and 10-bit (P010) 4:2:0, monochrome
  Av1VldProfile1,      // D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1 {6936ff0f-45b1-4163-9cc1-646ef6946108}: 8-bit (AYUV) and 10-bit (Y410) 4:4:4
  Av1VldProfile2,      // D3D11_DECODER_PROFILE_AV1_VLD_PROFILE2 {0c5f2aa1-e541-4089-bb7b-98110a19d7c8}: 8-bit (YUY2) and 10-bit (Y210) 4:2:2; monochrome (NV12, P010)
  Av1Vld12BitProfile2, // D3D11_DECODER_PROFILE_AV1_VLD_12BIT_PROFILE2 {17127009-a00f-4ce1-994e-bf4081f6f3f0}: 12-bit 4:2:2 (Y216) and 4:4:4 (Y416)
  Av1Vld12BitProfile2_420,   // D3D11_DECODER_PROFILE_AV1_VLD_12BIT_PROFILE2_420 {2d80bed6-9cac-4835-9e91-327bbc4f9ee8}: 12-bit 4:2:0 and monochrome (P016)
};
constexpr uint32_t kDxvaProfileCount = 11;

// The formats of the surfaces a decoder writes (DXGI_FORMAT_NV12, P010, P016, AYUV, Y410, Y416, YUY2, Y210, Y216). The numbers are the kernel's (DxgTypes.hpp
// VideoDecoderDesc::surface_format): 0 and 1 are NV12 and P010 as before. The 16-bit formats hold the value in the top bits of each word (P016 / Y216 / Y416 with
// 12-bit samples: value << 4); the packed ones are what DXGI says: AYUV bytes V, U, Y, A; Y410 a 32-bit word U | Y << 10 | V << 20 | A << 30; Y416 16-bit U, Y, V, A;
// YUY2 bytes Y0, U, Y1, V for each pair of pixels; Y210 / Y216 16-bit Y0, U, Y1, V. Alpha is opaque (the maximum).
enum class DxvaSurfaceFormat : uint8_t { Nv12, P010, P016, Ayuv, Y410, Y416, Yuy2, Y210, Y216 };
constexpr uint32_t kDxvaSurfaceFormatCount = 9;
// Packed: one plane (DxvaDecodedPicture::frame.plane[0]); the others are NV12 / P010-style two planes.
constexpr bool dxva_surface_packed(DxvaSurfaceFormat f) { return f >= DxvaSurfaceFormat::Ayuv; }
// Bytes of a row of a packed surface `width` pixels wide (a pair of pixels for YUY2 / Y210 / Y216: an odd width has a last pair whose second luma repeats the first).
constexpr uint64_t dxva_packed_row_bytes(DxvaSurfaceFormat f, uint32_t width) {
  return f == DxvaSurfaceFormat::Ayuv || f == DxvaSurfaceFormat::Y410 ? uint64_t{width} * 4
         : f == DxvaSurfaceFormat::Y416                              ? uint64_t{width} * 8
         : f == DxvaSurfaceFormat::Yuy2                              ? uint64_t{(width + 1) / 2} * 4
         : f == DxvaSurfaceFormat::Y210 || f == DxvaSurfaceFormat::Y216 ? uint64_t{(width + 1) / 2} * 8
                                                                     : 0;
}

// What the program gives for one picture (DXVA's buffer types: D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, _INVERSE_QUANTIZATION_MATRIX, _SLICE_CONTROL, _BITSTREAM).
// The views are of memory the caller keeps until decode() returns. A buffer the codec does not use may be empty; one it needs and does not get makes the picture invalid.
struct DxvaPicture {
  ConstByteView picture_parameters;
  ConstByteView quantization_matrix;   // (empty: the codec's default matrices, or those the parameters say)
  ConstByteView slice_control;         // an array of the codec's slice (or AV1 tile) control structures
  ConstByteView bitstream;             // the compressed data the slice controls point into
  // The surfaces the indices of the parameters stand for (the current picture's and the references'): surface_keys[i] is what index i is - a number that stays the same for the
  // same surface from picture to picture (the kernel's allocation of it). Direct3D 12 indices are positions in the call's list of reference frames, which may change from one
  // picture to the next; Direct3D 11's are array slices of the decoder's texture. The decoder keeps each decoded picture under its key and finds a reference by its key. Empty:
  // index i is key i. An index at or past surface_keys.count() (when not empty) is Invalid.
  ArrayView<const uint32_t> surface_keys;
  // The key the current picture is kept under: the output surface's (Direct3D 12 names the output apart from the references, and the current picture's index need not be in
  // the list). kNoDxvaKey: the key of its index, as for a reference. A reference whose key is the output's is the picture that was there before this one.
  uint32_t output_key = 0xFFFFFFFFu;
};
constexpr uint32_t kNoDxvaKey = 0xFFFFFFFFu;

enum class DxvaResult : uint8_t {
  Ok,
  Invalid,       // the buffers are malformed (sizes, offsets outside the bitstream, a surface index out of range) or describe something the profile does not take
  Unsupported,   // well formed, but a feature this decoder does not have (the reason is in DxvaDecodedPicture::detail)
  MissingReference,   // a reference names a surface that holds no decoded picture (the picture is decoded with what is there, as hardware does; the output is still given)
};

// The picture decode() made: the planes of the whole coded size (not cropped), in the profile's output format, and the surface it belongs in.
struct DxvaDecodedPicture {
  VideoFrame frame;              // NV12 or P010 (frame.format.pixel_format); the planes stay valid until the next call on the decoder. A packed surface format
                                 // (surface_format): plane[0] / stride[0] alone, its rows dxva_packed_row_bytes long, frame.format the picture's size (pixel_format
                                 // Bgra8888: one plane). P016: a P010 frame whose samples are 12- (or 16-) bit.
  DxvaSurfaceFormat surface_format = DxvaSurfaceFormat::Nv12;
  uint32_t surface = 0;          // the surface index the parameters name for the current picture (an index, not its key)
  const char* detail = nullptr;  // for Unsupported / Invalid: what was refused (a constant string)
};

struct DxvaDecoderConfig {
  DxvaProfile profile = DxvaProfile::H264VldNoFgt;
  uint32_t width = 0, height = 0;     // the decoder's size (D3D11_VIDEO_DECODER_DESC SampleWidth / SampleHeight): what the surfaces are
  uint32_t surfaces = 0;              // how many surface indices there may be (the size of the texture array, or of the reference set): 1 .. 128
  PixelFormat output = PixelFormat::Nv12;   // NV12 or P010 (the profile says which it may be)
  // The surfaces' format. Left NV12 it follows `output`; create_dxva_decoder() sets the one from the other (P016 and P010 are a P010 output; a packed format leaves
  // `output` NV12), so a codec of 4:2:0 only reads `output` alone.
  DxvaSurfaceFormat surface = DxvaSurfaceFormat::Nv12;
};

class IDxvaDecoder {
 public:
  virtual DxvaProfile profile() const = 0;
  [[nodiscard]] virtual DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) = 0;
  // Forget every picture kept (the program starts over: a seek, or new surfaces).
  virtual void reset() = 0;

 protected:
  constexpr IDxvaDecoder() = default;
  ~IDxvaDecoder() = default;   // (not virtual: the kernel has no operator delete; UniquePointer destroys the concrete type it was made with)
};

// Whether `profile` may decode into `output` at `width` x `height` (the formats and sizes Direct3D's CheckVideoDecoderFormat / GetVideoDecoderConfigCount answer from).
bool dxva_profile_supports(DxvaProfile profile, PixelFormat output, uint32_t width, uint32_t height);
bool dxva_profile_supports(DxvaProfile profile, DxvaSurfaceFormat surface, uint32_t width, uint32_t height);
// A decoder of `config`; null for a profile, format or size it does not take, or when memory ran out.
UniquePointer<IDxvaDecoder> create_dxva_decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);

}  // namespace hardware
