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

#pragma once
#include "Media/Image.hpp"
#include "Utility/ByteBuffer.hpp"

namespace media {

// Image decoders and encoders in software, one pair per format. Decoders replace the contents of the Image and return
// false (leaving it empty) for anything they cannot decode - damaged data, an unsupported variant, a size beyond
// Image::kMaxPixels. They never read outside `data`. Encoders append the file to `out`.
//
//   PNG   decode: every colour type and bit depth, interlacing, transparency (tRNS), animated PNG (APNG).
//         encode: 8-bit RGB or RGBA with adaptive row filters and LZ77 + fixed Huffman deflate; APNG for animations.
//   JPEG  decode: baseline and progressive, grey and YCbCr, restart markers. encode: baseline 4:2:0.
//   BMP   decode: 1, 4, 8, 16, 24 and 32 bits, bit fields, RLE4/RLE8, top-down and bottom-up. encode: 24 or 32 bits.
//   GIF   decode: GIF87a/89a, LZW, interlacing, transparency, animation (disposal methods). encode: a fixed 216 + grey
//         palette with dithering, one or many frames, transparency by a single colour.
//   PNM   decode: P1-P6 and PAM (P7). encode: P6 (or P5 for a grey image).
//   QOI   decode and encode: the Quite OK Image format.
//   TGA   decode: types 1, 2, 3 and their run-length forms, 8/15/16/24/32 bits. encode: 32-bit uncompressed.
bool decode_png(ConstByteView data, Image* out);
bool encode_png(const Image& image, uint32_t level, ByteBuffer* out);
bool decode_jpeg(ConstByteView data, Image* out);
bool encode_jpeg(const Image& image, uint32_t quality, ByteBuffer* out);
bool decode_bmp(ConstByteView data, Image* out);
bool encode_bmp(const Image& image, ByteBuffer* out);
bool decode_gif(ConstByteView data, Image* out);
bool encode_gif(const Image& image, ByteBuffer* out);
bool decode_pnm(ConstByteView data, Image* out);
bool encode_pnm(const Image& image, ByteBuffer* out);
bool decode_qoi(ConstByteView data, Image* out);
bool encode_qoi(const Image& image, ByteBuffer* out);
bool decode_tga(ConstByteView data, Image* out);
bool encode_tga(const Image& image, ByteBuffer* out);

// Formats the encoder exists for (all of the above).
constexpr uint32_t kEncodableImageFormats = 0xFE;

// The whole-file entry points: detect the format and decode, or encode in a given format (`quality` 1-100 for JPEG,
// the deflate effort 1-9 for PNG is derived from it; ignored elsewhere). Only frame 0 of an animation goes to a format
// that cannot hold more.
bool decode_image(ConstByteView data, Image* out, ImageFormat* detected = nullptr);
bool encode_image(const Image& image, ImageFormat format, uint32_t quality, ByteBuffer* out);

}  // namespace media
