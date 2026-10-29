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
// Implemented: 2D, 3D, 2D array and cube images (R8, R8G8B8A8 unorm, R32G32B32A32 float), with a mip chain; the component selects (DST_SEL); normalized or unnormalized coordinates;
// the address modes wrap, mirror, clamp to the last texel and clamp to the border (transparent black, opaque black, opaque white); point and bilinear filtering with separate minify
// and magnify filters; the level of detail from an explicit value (image_sample_l), a bias (image_sample_b) and the sampler's bias, clamped to its min and max, with none / point /
// linear mip filtering and trilinear filtering of 3D images between slices. There are no derivatives here (no 2x2 quads), so a plain image_sample has level of detail 0 plus the
// sampler's bias: the hardware's choice of level from the screen-space gradient is not modelled.
//
// Layouts: an image with several levels, slices (3D, array, cube) is laid out as AMD's address library does for the 1D tiled layout (GcnTiling.hpp), and only in it: the levels are
// stored one after the other (each starting at a multiple of 256 bytes) with the slices of a level together; the base level is padded to a power of two when there is more than one level
// (the pitch of the descriptor is that padded pitch), a level's pitch is the base pitch shifted down and padded to 8 pixels, its height likewise, and its slices count up to a power of two
// (a cube's 6 faces to 8), and a level's pitch is widened until its slice is a multiple of 256 bytes. A single level 2D image may be linear or 2D tiled as before. A cube is sampled with the coordinates v_cubesc / v_cubetc / v_cubema give (the face coordinates
// plus 1.5 after the division by the major axis, and the face index); its bilinear filter needs DISABLE_CUBE_WRAP in the sampler since filtering across the edges of faces is not done.
// Not done: 1D and 1D array images, arrays of cubes, mip levels or slices in the linear and 2D tiled layouts, anisotropy, depth compare, sRGB, the other formats, the border colour
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

enum class ImageType : uint8_t { Image2D, Image3D, Cube, Array2D };

struct ImageDescriptor {
  uint64_t address = 0;                // 256 byte aligned, 40 bits
  uint32_t width = 0, height = 0;      // 1 to 16384
  uint32_t pitch_pixels = 0;           // at least the width; a multiple of 8 for a tiled image; a power of two when there is more than one level (see `image_pitch`)
  TexelFormat format = TexelFormat::Rgba8Unorm;
  uint32_t tile_index = 8;             // the GB_TILE_MODE entry: 8 linear, 9 1D tiled for display
  uint8_t dst_sel[4] = {4, 5, 6, 7};   // what each result component takes: 0 zero, 1 one, 4..7 the image's x, y, z, w
  ImageType type = ImageType::Image2D;
  uint32_t depth = 1;                  // 3D: the depth; array: the layers; cube: 6 (the faces); 2D: 1
  uint32_t levels = 1;                 // mip levels, the base level first
};

struct SamplerDescriptor {
  uint8_t clamp_x = 0, clamp_y = 0, clamp_z = 0;    // SQ_TEX_WRAP 0, MIRROR 1, CLAMP_LAST_TEXEL 2, CLAMP_BORDER 6
  bool bilinear = false;               // the magnify filter (and the minify filter unless `min_bilinear_set`)
  bool min_bilinear_set = false;
  bool min_bilinear = false;           // the minify filter, when `min_bilinear_set`
  bool unnormalized = false;           // coordinates in texels
  uint8_t border = 0;                  // 0 transparent black, 1 opaque black, 2 opaque white
  uint8_t mip_filter = 0;              // 0 none (the base level), 1 point, 2 linear
  uint8_t z_filter = 0;                // 0 none (nearest slice), 1 point, 2 linear
  bool disable_cube_wrap = false;
  float min_lod = 0.0f, max_lod = 4095.0f / 256.0f;      // 4.8 fixed point in the descriptor
  float lod_bias = 0.0f;                       // signed 6.8 fixed point in the descriptor
};

// The pitch (in pixels) of the base level of an image of this size, texel size and number of levels (or slices) in the 1D tiled layout: the width padded to 8, or to a power of two first
// when there are several levels, and then widened by 8 until a slice is a whole number of 256 byte units (the library does so: an 8 bit image is at least 32 pixels wide).
uint32_t image_pitch(uint32_t width, uint32_t height, uint32_t levels, uint32_t texel_bytes);

// False if the description does not fit the descriptor (a size or address out of range, a misaligned address) or is outside what is implemented.
bool make_image_descriptor(ArrayView<uint32_t> out, const ImageDescriptor& image);
bool make_sampler_descriptor(ArrayView<uint32_t> out, const SamplerDescriptor& sampler);
bool decode_image_descriptor(ArrayView<const uint32_t> words, ImageDescriptor* out);
bool decode_sampler_descriptor(ArrayView<const uint32_t> words, SamplerDescriptor* out);

// Where a mip level of an image is: its offset from the image's address, the size of the level, its padded pitch and rows, the bytes of one slice and the slices the level takes
// (padded). False for a level the image does not have or an image whose layout does not allow levels.
struct LevelLayout {
  uint64_t offset = 0;
  uint32_t width = 0, height = 0, depth = 0;      // the level's size (depth: slices that exist)
  uint32_t pitch = 0, rows = 0;                   // padded
  uint64_t slice_bytes = 0, slices = 0;           // `slices` padded count
  uint64_t bytes() const { return slice_bytes * slices; }
};
bool image_level(const ImageDescriptor& image, uint32_t level, LevelLayout* out);
// The bytes the whole image takes (all levels)
uint64_t image_bytes(const ImageDescriptor& image);
// The byte offset of texel (x, y) of a slice of a level from the image's address
uint64_t image_texel_offset(const ImageDescriptor& image, const LevelLayout& level, uint32_t x, uint32_t y, uint32_t slice);

enum class SampleStatus : uint8_t { Ok, Unsupported, Fault };

// One lookup: the coordinates (u, v, and for 3D the third; for an array its layer in coord[2]; for a cube the face coordinates in coord[0..1], plus 1.5, and the face index in coord[2]),
// and the level of detail of the instruction.
struct SampleRequest {
  float coord[3] = {0, 0, 0};
  bool has_lod = false;
  float lod = 0.0f;                    // explicit (image_sample_l)
  float bias = 0.0f;                   // image_sample_b
};

SampleStatus sample_image(ITexelMemory& memory, ArrayView<const uint32_t> image, ArrayView<const uint32_t> sampler, const SampleRequest& request, float out[4]);
// A plain 2D lookup
SampleStatus sample_image(ITexelMemory& memory, ArrayView<const uint32_t> image, ArrayView<const uint32_t> sampler, float u, float v, float out[4]);

}  // namespace gcn
