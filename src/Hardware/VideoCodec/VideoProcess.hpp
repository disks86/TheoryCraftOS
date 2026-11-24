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
// A video processor (what Direct3D 11's VideoProcessorBlt and Direct3D 12's ProcessFrames do, src/Kernel/Dxgkrnl's VideoProcess command): input streams - pictures in YUV or
// RGB, each a source rectangle of it - are converted to the output's colour space and format, scaled to their destination rectangle (with an orientation), and composed over
// the output's background colour inside the target rectangle, in order (later streams on top, blended by their alpha when asked). Runs on the CPU.
//
// Images are described sample by sample, so the same code reads and writes the planes as the kernel keeps them (NV12 / P010 planes widened to four-channel pixels:
// DxgTypes.hpp VideoDecoderDesc) and as plain memory: a plane is rows `pitch` bytes apart, a sample `step` bytes apart in a row, and each channel at a byte offset in the sample.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// The formats (DXGI_FORMAT_NV12, P010, P016, B8G8R8A8_UNORM, B8G8R8X8_UNORM, R8G8B8A8_UNORM, R10G10B10A2_UNORM, R16G16B16A16_FLOAT).
enum class VpFormat : uint8_t { Nv12, P010, P016, Bgra8, Bgrx8, Rgba8, Rgb10a2, Rgba16f };
// The colour spaces: DXGI_COLOR_SPACE_TYPE values (the numbers are DXGI's). What vp_color_space_supported says is taken.
using VpColorSpace = uint32_t;
constexpr VpColorSpace kVpRgbFullG22P709 = 0, kVpRgbFullG10P709 = 1, kVpRgbStudioG22P709 = 2, kVpRgbStudioG22P2020 = 3, kVpYcbcrFullG22P709X601 = 5,
                       kVpYcbcrStudioG22LeftP601 = 6, kVpYcbcrFullG22LeftP601 = 7, kVpYcbcrStudioG22LeftP709 = 8, kVpYcbcrFullG22LeftP709 = 9, kVpYcbcrStudioG22LeftP2020 = 10,
                       kVpYcbcrFullG22LeftP2020 = 11, kVpYcbcrStudioG22TopLeftP2020 = 15, kVpRgbFullG22P2020 = 17, kVpRgbStudioG24P709 = 20, kVpRgbStudioG24P2020 = 21,
                       kVpYcbcrStudioG24LeftP709 = 22, kVpYcbcrStudioG24LeftP2020 = 23, kVpYcbcrStudioG24TopLeftP2020 = 24;
// The SDR colour spaces above (no PQ / HLG transfer: those need tone mapping, which this processor does not do). A YCbCr space for a YCbCr format, an RGB one for RGB.
// What they mean here: G22 is the sRGB curve (DXGI: "gamma 2.2", implemented with sRGB's linear segment), G24 the BT.1886 power 2.4, G10 linear light; P2020 primaries
// are converted to and from P709 / P601 ones in linear light (BT.2087), while P601 and P709 primaries are taken as the same (DXGI does not say which of BT.601's two
// sets is meant, and Windows' processors do not convert them either). X601 is the BT.601 matrix over P709 primaries (JPEG); LEFT chroma is co-sited horizontally and
// centred vertically (MPEG-2), TOPLEFT co-sited both ways, NONE centred both ways.
bool vp_color_space_supported(VpFormat format, VpColorSpace space);
constexpr bool vp_is_yuv(VpFormat f) { return f == VpFormat::Nv12 || f == VpFormat::P010 || f == VpFormat::P016; }

// One plane: memory, rows `pitch` bytes apart, samples `step` bytes apart, `width` x `height` samples. A sample's channels: `offset[c]` bytes into it, each `bits` wide
// (8 or 16; 10 for Rgb10a2, which is a packed 32-bit word and ignores offsets; 16 with `half_float` for Rgba16f). For a YUV plane 0 is Y (channel 0), plane 1 is U, V
// (channels 0 and 1); an RGB image has one plane with R, G, B, A (channels 0..3; X8 formats have no alpha: `channels` 3).
struct VpPlane {
  ByteView memory;            // (an input's is only read)
  uint32_t pitch = 0, step = 0, width = 0, height = 0;
  uint8_t offset[4] = {0, 1, 2, 3};
  uint8_t channels = 1;
  uint8_t bits = 8;           // 16-bit samples of P010 / P016 hold the value in their top bits (P010: value << 6)
  bool half_float = false;
};
struct VpImage {
  VpFormat format = VpFormat::Nv12;
  VpColorSpace color_space = kVpYcbcrStudioG22LeftP709;
  VpPlane plane[2];           // (RGB: plane[0] only)
  uint32_t width = 0, height = 0;   // the picture's size in pixels (a YUV image's chroma plane is half of it, rounded up)
};

struct VpRect { int32_t left = 0, top = 0, right = 0, bottom = 0; };   // (right and bottom exclusive)

// D3D12_VIDEO_PROCESS_ORIENTATION's values.
enum class VpOrientation : uint8_t { Default, FlipHorizontal, Clockwise90, Clockwise90FlipHorizontal, Clockwise180, FlipVertical, Clockwise270, Clockwise270FlipHorizontal };
// D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE: the output's alpha where a stream is drawn - opaque (1), the background's, the destination's (left as it was), or the stream's own.
enum class VpAlphaFill : uint8_t { Opaque, Background, Destination, SourceStream };

constexpr uint32_t kVpMaxStreams = 16;   // more streams in one call: Unsupported

struct VpStream {
  const VpImage* image = nullptr;
  VpRect source;              // a rectangle of the image (in its pixels)
  VpRect destination;         // where it goes in the output (in output pixels; clipped to the target rectangle)
  VpOrientation orientation = VpOrientation::Default;
  bool alpha_blend = false;   // blend it over what is under it with `alpha` times its own alpha (D3D12_VIDEO_PROCESS_ALPHA_BLENDING)
  float alpha = 1.0f;
};

struct VpOutput {
  VpImage* image = nullptr;
  VpRect target;              // what is written of the output; the rest is left alone
  float background[4] = {0, 0, 0, 1};   // in the output's colour space (YCbCr: Y, Cb, Cr, A; RGB: R, G, B, A), 0..1
  bool background_is_yuv = false;       // the background colour is YCbCr (D3D11's BackgroundColor YCbCr flag) rather than RGB in the output's space
  VpAlphaFill alpha_fill = VpAlphaFill::Opaque;
  uint32_t alpha_fill_stream = 0;       // for SourceStream: which stream's alpha
};

enum class VpResult : uint8_t { Ok, Invalid, Unsupported };

// Draws `streams` into `output`: the target rectangle is filled with the background, then each stream is converted (colour space and format), scaled bilinearly from its
// source rectangle to its destination rectangle as oriented, and put there - over what is there when it blends. A YUV output is written with its chroma at the
// colour space's siting (averaging the pixels a chroma sample covers). Invalid: a rectangle outside its image, a plane too small for what it describes, a format whose
// planes do not match, a stream alpha or background outside 0..1; Unsupported: a colour space vp_color_space_supported refuses, more than kVpMaxStreams streams.
// An empty source or destination rectangle leaves its stream out. Results are clamped to 0..1 (also for Rgba16f). Alpha fill SourceStream: the target's alpha is the
// background's, inside that stream's destination the stream's own (opaque if its format has none; left as it was if its source rectangle is empty).
[[nodiscard]] VpResult video_process(const VpOutput& output, ArrayView<const VpStream> streams);

}  // namespace hardware
