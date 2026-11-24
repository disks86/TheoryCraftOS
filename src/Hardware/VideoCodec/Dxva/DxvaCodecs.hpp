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
// The codecs of DxvaDecoder.hpp, one creator each (each in its own file: DxvaH264.cpp, DxvaMpeg2.cpp, DxvaVp9.cpp, DxvaHevc.cpp, DxvaAv1.cpp, DxvaVc1.cpp, DxvaMpeg4.cpp).
#pragma once
#include "Hardware/VideoCodec/Dxva/DxvaDecoder.hpp"

namespace hardware {
UniquePointer<IDxvaDecoder> create_dxva_h264(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);
UniquePointer<IDxvaDecoder> create_dxva_mpeg2(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);   // Mpeg2Vld and Mpeg2and1Vld
UniquePointer<IDxvaDecoder> create_dxva_vp9(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);     // both VP9 profiles
UniquePointer<IDxvaDecoder> create_dxva_hevc(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);
UniquePointer<IDxvaDecoder> create_dxva_av1(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);
UniquePointer<IDxvaDecoder> create_dxva_vc1(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);     // Vc1Vld and Vc1D2010
UniquePointer<IDxvaDecoder> create_dxva_mpeg4(allocators::IAllocator& allocator, const DxvaDecoderConfig& config);   // the three MPEG-4 Part 2 profiles
}  // namespace hardware
