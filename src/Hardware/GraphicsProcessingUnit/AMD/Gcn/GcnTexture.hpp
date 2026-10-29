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


// Image and sampler descriptors as the shader reads them from the descriptor table (the T# of eight dwords, the S# of four: SQ_IMG_RSRC_WORD0..7 and SQ_IMG_SAMP_WORD0..3 of the
// kernel's headers), and the sampling the texture unit does with them. The harness and the command processor model sample through these; the compiler's ABI puts the T# of
// texture i at table offset kImageOffset + 32 * i and the S# at kSamplerOffset + 16 * i.
//
// Implemented: 2D images of one level (R8, R8G8B8A8 unorm, R32G32B32A32 float), linear or 1D tiled (GcnTiling.hpp), the component selects (DST_SEL), normalized or unnormalized
// coordinates, the address modes wrap, mirror, clamp to the last texel and clamp to the border (transparent black, opaque black, opaque white), point and bilinear filtering (the
// magnification filter: there is no level of detail). Not done: mip levels, 3D, cube and array images, anisotropy, depth compare, sRGB, the other formats, the border colour
// register, texel caches. Anything outside that is refused (Unsupported), not approximated.
#pragma once

#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace gcn {

// Memory the texture unit reads, a dword at a time (4 byte aligned addresses).
class ITexelMemory {
 public:
  virtual bool read32(uint64_t address, uint32_t* out) = 0;

 protected:
  ITexelMemory() = default;
  ~ITexelMemory() = default;
};

enum class TexelFormat : uint8_t { R8Unorm, Rgba8Unorm, Rgba32Float };
uint32_t texel_bytes(TexelFormat format);

struct ImageDescriptor {
  uint64_t address = 0;                // 256 byte aligned, 40 bits
  uint32_t width = 0, height = 0;      // 1 to 16384
  uint32_t pitch_pixels = 0;           // at least the width; a multiple of 8 for a tiled image
  TexelFormat format = TexelFormat::Rgba8Unorm;
  uint32_t tile_index = 8;             // the GB_TILE_MODE entry: 8 linear, 9 1D tiled for display
  uint8_t dst_sel[4] = {4, 5, 6, 7};   // what each result component takes: 0 zero, 1 one, 4..7 the image's x, y, z, w
};

struct SamplerDescriptor {
  uint8_t clamp_x = 0, clamp_y = 0;    // SQ_TEX_WRAP 0, MIRROR 1, CLAMP_LAST_TEXEL 2, CLAMP_BORDER 6
  bool bilinear = false;
  bool unnormalized = false;           // coordinates in texels
  uint8_t border = 0;                  // 0 transparent black, 1 opaque black, 2 opaque white
};

// False if the description does not fit the descriptor (a size or address out of range, a misaligned address) or is outside what is implemented.
bool make_image_descriptor(ArrayView<uint32_t> out, const ImageDescriptor& image);
bool make_sampler_descriptor(ArrayView<uint32_t> out, const SamplerDescriptor& sampler);
bool decode_image_descriptor(ArrayView<const uint32_t> words, ImageDescriptor* out);
bool decode_sampler_descriptor(ArrayView<const uint32_t> words, SamplerDescriptor* out);

enum class SampleStatus : uint8_t { Ok, Unsupported, Fault };

// Samples the image at (u, v) the way image_sample with the descriptors does.
SampleStatus sample_image(ITexelMemory& memory, ArrayView<const uint32_t> image, ArrayView<const uint32_t> sampler, float u, float v, float out[4]);

}  // namespace gcn
