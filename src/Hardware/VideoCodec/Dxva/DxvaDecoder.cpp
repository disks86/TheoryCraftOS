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
  if (width == 0 || height == 0 || width > VideoFormat::kMaxDimension || height > VideoFormat::kMaxDimension) return false;
  switch (profile) {
    case DxvaProfile::H264VldNoFgt:
    case DxvaProfile::Mpeg2Vld:
    case DxvaProfile::Mpeg2and1Vld:
    case DxvaProfile::Vp9VldProfile0:
    case DxvaProfile::HevcVldMain: return output == PixelFormat::Nv12;
    case DxvaProfile::Vp9Vld10BitProfile2:
    case DxvaProfile::HevcVldMain10: return output == PixelFormat::P010;
    case DxvaProfile::Av1VldProfile0: return output == PixelFormat::Nv12 || output == PixelFormat::P010;
  }
  return false;
}

UniquePointer<IDxvaDecoder> create_dxva_decoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (!dxva_profile_supports(config.profile, config.output, config.width, config.height) || config.surfaces == 0 || config.surfaces > 128) return {};
  switch (config.profile) {
    case DxvaProfile::H264VldNoFgt: return create_dxva_h264(allocator, config);
    case DxvaProfile::Mpeg2Vld:
    case DxvaProfile::Mpeg2and1Vld: return create_dxva_mpeg2(allocator, config);
    case DxvaProfile::Vp9VldProfile0:
    case DxvaProfile::Vp9Vld10BitProfile2: return create_dxva_vp9(allocator, config);
    case DxvaProfile::HevcVldMain:
    case DxvaProfile::HevcVldMain10: return create_dxva_hevc(allocator, config);
    case DxvaProfile::Av1VldProfile0: return create_dxva_av1(allocator, config);
  }
  return {};
}

}  // namespace hardware
