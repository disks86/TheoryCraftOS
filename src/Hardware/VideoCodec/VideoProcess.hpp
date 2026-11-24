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

namespace allocators { class IAllocator; }

namespace hardware {

// The formats (DXGI_FORMAT_NV12, P010, P016, B8G8R8A8_UNORM, B8G8R8X8_UNORM, R8G8B8A8_UNORM, R10G10B10A2_UNORM, R16G16B16A16_FLOAT).
enum class VpFormat : uint8_t { Nv12, P010, P016, Bgra8, Bgrx8, Rgba8, Rgb10a2, Rgba16f };
// The colour spaces: DXGI_COLOR_SPACE_TYPE values (the numbers are DXGI's). What vp_color_space_supported says is taken.
using VpColorSpace = uint32_t;
constexpr VpColorSpace kVpRgbFullG22P709 = 0, kVpRgbFullG10P709 = 1, kVpRgbStudioG22P709 = 2, kVpRgbStudioG22P2020 = 3, kVpYcbcrFullG22P709X601 = 5,
                       kVpYcbcrStudioG22LeftP601 = 6, kVpYcbcrFullG22LeftP601 = 7, kVpYcbcrStudioG22LeftP709 = 8, kVpYcbcrFullG22LeftP709 = 9, kVpYcbcrStudioG22LeftP2020 = 10,
                       kVpYcbcrFullG22LeftP2020 = 11, kVpYcbcrStudioG22TopLeftP2020 = 15, kVpRgbFullG22P2020 = 17, kVpRgbStudioG24P709 = 20, kVpRgbStudioG24P2020 = 21,
                       kVpYcbcrStudioG24LeftP709 = 22, kVpYcbcrStudioG24LeftP2020 = 23, kVpYcbcrStudioG24TopLeftP2020 = 24;
// The high dynamic range spaces: ST.2084 (PQ) and HLG (BT.2100), BT.2020 primaries.
constexpr VpColorSpace kVpRgbFullG2084P2020 = 12, kVpYcbcrStudioG2084LeftP2020 = 13, kVpRgbStudioG2084P2020 = 14, kVpYcbcrStudioG2084TopLeftP2020 = 16,
                       kVpYcbcrStudioGhlgTopLeftP2020 = 18, kVpYcbcrFullGhlgTopLeftP2020 = 19;
// Light levels: an SDR picture's 1.0 (and linear G10's 1.0) is the reference white of BT.2408, kVpReferenceWhiteNits cd/m2, when it meets PQ or HLG; HLG is shown on its
// nominal kVpHlgPeakNits display (BT.2100: OOTF gamma 1.2).
constexpr float kVpReferenceWhiteNits = 203.0f, kVpHlgPeakNits = 1000.0f;
// The colour spaces above. A YCbCr space for a YCbCr format, an RGB one for RGB.
// What they mean here: G22 is the sRGB curve (DXGI: "gamma 2.2", implemented with sRGB's linear segment), G24 the BT.1886 power 2.4, G10 linear light; P2020 primaries
// are converted to and from P709 / P601 ones in linear light (BT.2087), while P601 and P709 primaries are taken as the same (DXGI does not say which of BT.601's two
// sets is meant, and Windows' processors do not convert them either). X601 is the BT.601 matrix over P709 primaries (JPEG); LEFT chroma is co-sited horizontally and
// centred vertically (MPEG-2), TOPLEFT co-sited both ways, NONE centred both ways. Between SDR and PQ / HLG the conversion is in absolute light (see the light levels
// above); high dynamic range content brighter than what the output holds is tone mapped (VpStream::hdr, VpOutput::hdr).
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

// Interlaced pictures (D3D12_VIDEO_FIELD_TYPE, D3D11_VIDEO_FRAME_FORMAT) and how a field is made a frame: Weave shows the frame as it is; Bob interpolates the field's
// missing rows from the rows above and below; Adaptive is motion adaptive with the previous and next frames (the yadif algorithm: temporal interpolation where the picture
// is still, edge-directed spatial interpolation where it moves).
enum class VpFieldOrder : uint8_t { Progressive, TopFieldFirst, BottomFieldFirst };
enum class VpDeinterlace : uint8_t { Weave, Bob, Adaptive };
// How two views are packed in one picture (D3D11_VIDEO_PROCESSOR_STEREO_FORMAT; separate pictures are two streams' worth of images: the caller picks the view's image):
// frame 0 on the left / top / even rows / even columns / where x + y is even, frame 1 in the rest. MonoOffset: one view, shifted horizontally.
enum class VpStereoFormat : uint8_t { None, Horizontal, Vertical, RowInterleaved, ColumnInterleaved, Checkerboard, MonoOffset };
// DXGI_HDR_METADATA_HDR10's light levels in cd/m2 (0: not known).
struct VpHdrMetadata { float max_mastering = 0.0f, min_mastering = 0.0f, max_content = 0.0f, max_frame_average = 0.0f; };

struct VpStream {
  const VpImage* image = nullptr;
  VpRect source;              // a rectangle of the image (in its pixels)
  VpRect destination;         // where it goes in the output (in output pixels; clipped to the target rectangle)
  VpOrientation orientation = VpOrientation::Default;
  bool alpha_blend = false;   // blend it over what is under it with `alpha` times its own alpha (D3D12_VIDEO_PROCESS_ALPHA_BLENDING)
  float alpha = 1.0f;

  // Interlacing: the field shown (0 the first in time, 1 the second) and the frames before and after (Adaptive; none given: the current one). They are pictures of the
  // image's format, colour space and size. Ignored for a progressive picture.
  VpFieldOrder field_order = VpFieldOrder::Progressive;
  VpDeinterlace deinterlace = VpDeinterlace::Bob;
  uint8_t field = 0;
  const VpImage* past = nullptr;
  const VpImage* future = nullptr;
  // Stereo: which frame of the packed picture is this stream's view (0, 1), whether it is stored upside down, and MonoOffset's shift (pixels, positive to the right).
  // The source rectangle is in the view's pixels (half the picture's width for Horizontal, ...).
  VpStereoFormat stereo = VpStereoFormat::None;
  uint8_t stereo_frame = 0;
  bool stereo_flip = false;
  int32_t stereo_offset = 0;
  // Filters, in the DXVA-HD units: ProcAmp (brightness -100..100 in 8-bit luma steps, contrast 0..10, hue -180..180 degrees, saturation 0..10: on the YCbCr values,
  // Y' = C Y + B, (Cb', Cr') = C S R(H) (Cb, Cr)); noise reduction and edge enhancement 0..1 (strength); anamorphic scaling 0..1 (a non-linear horizontal stretch
  // that keeps the middle of the picture least stretched).
  float brightness = 0.0f, contrast = 1.0f, hue = 0.0f, saturation = 1.0f;
  float noise_reduction = 0.0f, edge_enhancement = 0.0f, anamorphic = 0.0f;
  // Luma key: pixels whose luma code is in [lower, upper] times the maximum code (D3D11's rule: both truncated to codes) are transparent.
  bool luma_key = false;
  float luma_lower = 0.0f, luma_upper = 0.0f;
  VpHdrMetadata hdr;          // the content's light levels (tone mapping)
};

struct VpOutput {
  VpImage* image = nullptr;
  VpRect target;              // what is written of the output; the rest is left alone
  float background[4] = {0, 0, 0, 1};   // in the output's colour space (YCbCr: Y, Cb, Cr, A; RGB: R, G, B, A), 0..1
  bool background_is_yuv = false;       // the background colour is YCbCr (D3D11's BackgroundColor YCbCr flag) rather than RGB in the output's space
  VpAlphaFill alpha_fill = VpAlphaFill::Opaque;
  uint32_t alpha_fill_stream = 0;       // for SourceStream: which stream's alpha
  VpHdrMetadata hdr;                    // the display's light levels, for a PQ output (max_mastering: its peak; 0: 10000)
  allocators::IAllocator* allocator = nullptr;   // scratch pictures for deinterlacing, stereo views and the spatial filters (none: those are Unsupported)
};

enum class VpResult : uint8_t { Ok, Invalid, Unsupported, NoMemory };

// Draws `streams` into `output`: the target rectangle is filled with the background, then each stream is converted (colour space and format), scaled bilinearly from its
// source rectangle to its destination rectangle as oriented, and put there - over what is there when it blends. A YUV output is written with its chroma at the
// colour space's siting (averaging the pixels a chroma sample covers). Invalid: a rectangle outside its image, a plane too small for what it describes, a format whose
// planes do not match, a stream alpha or background outside 0..1; Unsupported: a colour space vp_color_space_supported refuses, more than kVpMaxStreams streams.
// An empty source or destination rectangle leaves its stream out. Results are clamped to 0..1 (also for Rgba16f, except linear G10 out of PQ or HLG, which keeps their light: up to 10000 / kVpReferenceWhiteNits). Alpha fill SourceStream: the target's alpha is the
// background's, inside that stream's destination the stream's own (opaque if its format has none; left as it was if its source rectangle is empty).
// Before that, per stream: the view of a stereo picture is taken, an interlaced one deinterlaced (each plane on its own, its rows' parity giving their field), then
// noise reduction (3x3 sigma filter) and edge enhancement (3x3 unsharp mask) on the picture's samples; the luma key, ProcAmp and the colour conversion with tone mapping
// are applied to each sample read. Invalid also: filter values outside their ranges, reference frames that do not match, an interlaced row-interleaved or checkerboard
// picture; NoMemory: no scratch memory.
[[nodiscard]] VpResult video_process(const VpOutput& output, ArrayView<const VpStream> streams);

}  // namespace hardware
