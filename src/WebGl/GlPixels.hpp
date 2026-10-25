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

// The pixel conversions behind texImage2D, readPixels and generateMipmap (GlTexture.cpp), used by the GL context and the WebGPU device.
#pragma once
#include "WebGl/GlContext.hpp"

namespace webgl {

float srgb_to_linear(float c);
float linear_to_srgb(float c);

// Components and bytes of one pixel of a (format, type) pair; `packed` for the 565/4444/... types. False for an invalid pair.
bool pixel_layout(uint32_t format, uint32_t type, uint32_t* ncomp, uint32_t* bytes, bool* packed);

// Converts decoded lanes (floats, or raw integers when integer_src) to the storage of `fi`.
void store_lanes(const FormatInfo* fi, uint32_t lanes_in[4], bool integer_src, bool premultiply, uint32_t out[4]);

// Writes a block of client pixels into an image. On failure sets *error to the GL error.
bool write_region(Image& img, uint32_t xo, uint32_t yo, uint32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, ConstByteView data,
                  const GlContext::Unpack& up, uint32_t* error);

// Reads a block of an image into client memory (rows bottom-up as stored).
bool pack_pixels(const Image& img, int32_t x0, int32_t y0, uint32_t w, uint32_t h, uint32_t z, uint32_t format, uint32_t type, ByteView out, const GlContext::Pack& pk,
                 uint32_t* error);

void downsample(const Image& src, Image* dst);

}  // namespace webgl
