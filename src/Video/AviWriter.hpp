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
#include "Allocators/IAllocator.hpp"
#include "Audio/StreamFormat.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"

namespace video {

// Builds an AVI file in memory: one video stream (Motion JPEG or raw I420/NV12 frames) and optionally one
// uncompressed PCM audio stream. Add the frames and audio in playing order, then finish(); the file is in file().
// Chunks are written as they arrive, so interleave the audio with the frames the way a player will want it (about
// one frame's worth after each frame).
class AviWriter {
 public:
  explicit AviWriter(allocators::IAllocator& allocator) : allocator_(allocator), movi_(allocator), file_(allocator), index_(allocator) {}

  // `codec` must be Mjpeg or Raw (with `raw_format` I420 or Nv12). `audio` is optional (nullptr: no sound).
  bool begin(hardware::VideoCodec codec, uint32_t width, uint32_t height, uint32_t fps_numerator, uint32_t fps_denominator,
             hardware::PixelFormat raw_format = hardware::PixelFormat::I420, const audio::StreamFormat* audio = nullptr);
  bool add_frame(ConstByteView data, bool keyframe = true);
  bool add_audio(ConstByteView pcm);
  // Completes the headers and the index. After this file() is the whole AVI.
  bool finish();
  ConstByteView file() const { return file_.view(); }
  uint32_t frame_count() const { return frames_; }

 private:
  struct IndexEntry {
    uint32_t id;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
  };
  bool add_chunk(const char* id, ConstByteView data, uint32_t flags);

  allocators::IAllocator& allocator_;
  ByteBuffer movi_;  // the chunks, after the "movi" tag
  ByteBuffer file_;
  List<IndexEntry> index_;
  hardware::VideoCodec codec_ = hardware::VideoCodec::Mjpeg;
  hardware::PixelFormat raw_format_ = hardware::PixelFormat::I420;
  audio::StreamFormat audio_;
  bool has_audio_ = false;
  bool begun_ = false;
  uint32_t width_ = 0, height_ = 0, fps_numerator_ = 0, fps_denominator_ = 1;
  uint32_t frames_ = 0;
  uint64_t audio_bytes_ = 0;
  uint32_t largest_chunk_ = 0;
};

}  // namespace video
