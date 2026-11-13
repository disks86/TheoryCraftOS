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

// Video frames from one raw format to another of the same size, for a watcher that cannot take its producer's format (what PipeWire's videoconvert
// does in the program; here the server does it). Every raw format VideoFormat carries: packed RGB of 8 bits (RGBx ... BGR), 16 (RGB16, BGR16,
// RGB15, BGR15) and 10 bits a component (xRGB_210LE ... BGRA_102LE), ARGB64, float RGBA (F16, F32 and dsp), planar and semi-planar YUV of 8, 10 and
// 12 bits (I420, YV12, Y41B, Y42B, Y444, NV12, NV21, NV16, NV61, NV24, I420_10LE, I422_10LE, Y444_10LE, I420_12LE, P010), packed YUV (YUY2, UYVY,
// YVYU, VYUY, AYUV, AYUV64) and grey (GRAY8, GRAY16). Through RGBA of 16 bits a component, so 10-, 12- and 16-bit formats keep their bits (a
// 10-bit YUV frame made 16-bit RGB and back is what it was). YUV is BT.601 limited range (as cameras send it), grey full range; chroma is averaged
// over the pixels it covers. The frames are VideoChannel's: planes with no padding, one
// after another.
#pragma once
#include <stdint.h>
#include "PipeWire/VideoChannel.hpp"
#include "PipeWire/VideoDecode.hpp"
#include "Utility/ByteBuffer.hpp"

namespace pipewire {

// Whether raw frames of SPA format `from` can be turned into `to` (both of the formats above; the same format too).
bool can_convert_video(uint32_t from, uint32_t to);
// `in`: a whole frame of `from` (VideoChannel's layout); *out becomes the frame in `to`, scaled when its size is another (averaged when shrinking,
// bilinear when enlarging). False when the formats cannot be converted, `in` is short, or there is no memory.
bool convert_video(const VideoFormat& from, ConstByteView in, const VideoFormat& to, ByteBuffer* out);

// Whether frames of `from` can be decoded and converted into raw `to`: encoded video VideoDecodeSession decodes (Motion JPEG always; the others when
// `decoders` serves their codec) into any of the formats convert_video makes.
bool can_decode_video(const VideoFormat& from, uint32_t to, const IVideoDecoders* decoders);
// An encoded frame of `from`'s subtype as a raw frame of `to` (scaled when the picture is another size), with `session` (one per stream: it keeps the
// decoder and its pictures). False when no picture came out: *failed says whether that is because the frame cannot be decoded (else the decoder is
// holding it back, or memory ran out).
bool decode_video(ConstByteView frame, const VideoFormat& from, const VideoFormat& to, VideoDecodeSession& session, IVideoDecoders* decoders,
                  ByteBuffer* out, bool* failed);

}  // namespace pipewire
