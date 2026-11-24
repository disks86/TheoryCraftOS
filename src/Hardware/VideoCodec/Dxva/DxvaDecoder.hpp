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
};
constexpr uint32_t kDxvaProfileCount = 7;

// What the program gives for one picture (DXVA's buffer types: D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, _INVERSE_QUANTIZATION_MATRIX, _SLICE_CONTROL, _BITSTREAM).
// The views are of memory the caller keeps until decode() returns. A buffer the codec does not use may be empty; one it needs and does not get makes the picture invalid.
struct DxvaPicture {
  ConstByteView picture_parameters;
  ConstByteView quantization_matrix;   // (empty: the codec's default matrices, or those the parameters say)
  ConstByteView slice_control;         // an array of the codec's slice (or AV1 tile) control structures
  ConstByteView bitstream;             // the compressed data the slice controls point into
};

enum class DxvaResult : uint8_t {
  Ok,
  Invalid,       // the buffers are malformed (sizes, offsets outside the bitstream, a surface index out of range) or describe something the profile does not take
  Unsupported,   // well formed, but a feature this decoder does not have (the reason is in DxvaDecodedPicture::detail)
  MissingReference,   // a reference names a surface that holds no decoded picture (the picture is decoded with what is there, as hardware does; the output is still given)
};

// The picture decode() made: the planes of the whole coded size (not cropped), in the profile's output format, and the surface it belongs in.
struct DxvaDecodedPicture {
  VideoFrame frame;              // NV12 or P010 (frame.format.pixel_format); the planes stay valid until the next call on the decoder
  uint32_t surface = 0;          // the surface index the parameters name for the current picture
  const char* detail = nullptr;  // for Unsupported / Invalid: what was refused (a constant string)
};

struct DxvaDecoderConfig {
  DxvaProfile profile = DxvaProfile::H264VldNoFgt;
  uint32_t width = 0, height = 0;     // the decoder's size (D3D11_VIDEO_DECODER_DESC SampleWidth / SampleHeight): what the surfaces are
  uint32_t surfaces = 0;              // how many surface indices there may be (the size of the texture array, or of the reference set): 1 .. 128
  PixelFormat output = PixelFormat::Nv12;   // NV12 or P010 (the profile says which it may be)
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
// A decoder of `config`; null for a profile, format or size it does not take, or when memory ran out.
UniquePointer<IDxvaDecoder> create_dxva_decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);

}  // namespace hardware
