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

// Video containers that are read by decoding all their pictures up front into an Image whose frames carry their own
// durations: the player shows them like any other video. (AVI is read by video::AviFile, MP4 and MOV by media::Mp4File, WebM and Matroska by media::WebmFile.)
//
//   GIF, APNG, animated WebP  animated pictures: decode_image() already yields the frames and delays.
//   Y4M        YUV4MPEG2 uncompressed video: 4:2:0, 4:2:2, 4:4:4 and mono, progressive; decoded to RGB with the BT.601
//              limited-range matrix (mono: the luma as grey, full range).
enum class VideoFileFormat : uint8_t { Unknown, Avi, Gif, Apng, Y4m, AnimatedWebp, Mp4, Webm };
const char* video_file_format_name(VideoFileFormat format);
VideoFileFormat detect_video_file(ConstByteView data);

// Every frame of a Y4M stream (stopping at a truncated final frame), with delays from the stream's frame rate: the
// delays alternate between floor and ceiling so the running time stays exact. False for a stream that is not Y4M,
// interlaced, or in a chroma format not listed above.
bool decode_y4m(ConstByteView data, Image* out);
// Writes `image` as 4:2:0 Y4M at the frame rate implied by the first frame's delay (any width and height).
bool encode_y4m(const Image& image, ByteBuffer* out);

}  // namespace media
