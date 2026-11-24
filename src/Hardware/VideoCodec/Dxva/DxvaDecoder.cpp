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
#include "Hardware/VideoCodec/Dxva/DxvaDecoder.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"

namespace hardware {

bool dxva_profile_supports(DxvaProfile profile, PixelFormat output, uint32_t width, uint32_t height) {
  if (output != PixelFormat::Nv12 && output != PixelFormat::P010) return false;
  return dxva_profile_supports(profile, output == PixelFormat::P010 ? DxvaSurfaceFormat::P010 : DxvaSurfaceFormat::Nv12, width, height);
}

bool dxva_profile_supports(DxvaProfile profile, DxvaSurfaceFormat surface, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > VideoFormat::kMaxDimension || height > VideoFormat::kMaxDimension) return false;
  using S = DxvaSurfaceFormat;
  // (the 4:2:0 profiles below take NV12 / P010: `output`)
  const PixelFormat output = surface == S::Nv12 ? PixelFormat::Nv12 : surface == S::P010 ? PixelFormat::P010 : PixelFormat::Bgra8888;
  switch (profile) {
    case DxvaProfile::Av1VldProfile1: return surface == S::Ayuv || surface == S::Y410;
    case DxvaProfile::Av1VldProfile2: return surface == S::Yuy2 || surface == S::Y210 || surface == S::Nv12 || surface == S::P010;
    case DxvaProfile::Av1Vld12BitProfile2: return surface == S::Y216 || surface == S::Y416;
    case DxvaProfile::Av1Vld12BitProfile2_420: return surface == S::P016;
    default: break;
  }
  switch (profile) {
    case DxvaProfile::H264VldNoFgt:
    case DxvaProfile::Mpeg2Vld:
    case DxvaProfile::Mpeg2and1Vld:
    case DxvaProfile::Vp9VldProfile0:
    case DxvaProfile::HevcVldMain:
    case DxvaProfile::Vc1Vld:
    case DxvaProfile::Vc1D2010:
    case DxvaProfile::Mpeg4Pt2Simple:
    case DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc:
    case DxvaProfile::Mpeg4Pt2AdvSimpleGmc: return output == PixelFormat::Nv12;
    case DxvaProfile::Vp9Vld10BitProfile2:
    case DxvaProfile::HevcVldMain10: return output == PixelFormat::P010;
    case DxvaProfile::Av1VldProfile0: return output == PixelFormat::Nv12 || output == PixelFormat::P010;
    default: return false;
  }
}

UniquePointer<IDxvaDecoder> create_dxva_decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& asked) {
  DxvaDecoderConfig config = asked;
  if (config.surface == DxvaSurfaceFormat::Nv12) {
    if (config.output == PixelFormat::P010) config.surface = DxvaSurfaceFormat::P010;
    else if (config.output != PixelFormat::Nv12) return {};
  } else {
    config.output = config.surface == DxvaSurfaceFormat::P010 || config.surface == DxvaSurfaceFormat::P016 ? PixelFormat::P010 : PixelFormat::Nv12;
  }
  if (!dxva_profile_supports(config.profile, config.surface, config.width, config.height) || config.surfaces == 0 || config.surfaces > 128) return {};
  switch (config.profile) {
    case DxvaProfile::H264VldNoFgt: return create_dxva_h264(allocator, config);
    case DxvaProfile::Mpeg2Vld:
    case DxvaProfile::Mpeg2and1Vld: return create_dxva_mpeg2(allocator, config);
    case DxvaProfile::Vp9VldProfile0:
    case DxvaProfile::Vp9Vld10BitProfile2: return create_dxva_vp9(allocator, config);
    case DxvaProfile::HevcVldMain:
    case DxvaProfile::HevcVldMain10: return create_dxva_hevc(allocator, config);
    case DxvaProfile::Av1VldProfile0:
    case DxvaProfile::Av1VldProfile1:
    case DxvaProfile::Av1VldProfile2:
    case DxvaProfile::Av1Vld12BitProfile2:
    case DxvaProfile::Av1Vld12BitProfile2_420: return create_dxva_av1(allocator, config);
    case DxvaProfile::Vc1Vld:
    case DxvaProfile::Vc1D2010: return create_dxva_vc1(allocator, config);
    case DxvaProfile::Mpeg4Pt2Simple:
    case DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc:
    case DxvaProfile::Mpeg4Pt2AdvSimpleGmc: return create_dxva_mpeg4(allocator, config);
  }
  return {};
}

}  // namespace hardware
