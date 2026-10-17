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

namespace video {

struct AviVideoInfo {
  bool present = false;
  bool supported = false;  // the codec is one this OS names (a decoder for it may still be missing)
  hardware::VideoCodec codec = hardware::VideoCodec::Mjpeg;
  hardware::PixelFormat raw_format = hardware::PixelFormat::I420;  // for Raw
  uint32_t width = 0, height = 0;
  uint32_t rate = 0, scale = 1;  // frames per second = rate / scale
  uint32_t frame_count = 0;
};

struct AviAudioInfo {
  bool present = false;  // an audio stream we can play: uncompressed 8- or 16-bit PCM, mono or stereo
  audio::StreamFormat format;
  uint64_t bytes = 0;    // all the audio in the file
};

// Reads an AVI file that is held in memory (borrowed: it must outlive the reader): one video stream and, if
// there is one, one PCM audio stream. Video may be Motion JPEG ("MJPG"), raw I420/NV12, or a compressed codec
// the decoders may not have (H.264 and so on, flagged by video().codec); the frames are found by scanning the
// "movi" list, so a missing or wrong index does no harm, and the index is only consulted for which frames
// are keyframes.
class AviFile {
 public:
  explicit AviFile(allocators::IAllocator& allocator) : video_frames_(allocator), audio_chunks_(allocator), keyframes_(allocator) {}

  // False if the file is not an AVI, or has no video stream, or is cut short before its first frame.
  bool open(ConstByteView file);
  const AviVideoInfo& video() const { return video_; }
  const AviAudioInfo& audio() const { return audio_; }

  uint32_t frame_count() const { return static_cast<uint32_t>(video_frames_.count()); }
  // Frame `index` as a packet (its data points into the file; its time follows from the frame rate).
  bool frame(uint32_t index, hardware::VideoPacket* packet) const;
  int64_t frame_time_us(uint32_t index) const;
  int64_t duration_us() const { return frame_time_us(frame_count()); }
  // The frame on screen at `time_us` (the last one at or before it); the last frame for times past the end.
  uint32_t frame_at_time(int64_t time_us) const;
  // The keyframe at or before `index`.
  uint32_t keyframe_at_or_before(uint32_t index) const;
  // True if every frame is a keyframe (Motion JPEG, raw): frames can be decoded in any order.
  bool all_keyframes() const { return all_keyframes_; }

  // The audio, gathered into `out` (which needs audio().bytes); returns the bytes copied.
  uint64_t copy_audio(ByteView out) const;

 private:
  struct Chunk {
    uint64_t offset;
    uint32_t size;
  };

  bool parse_headers(ConstByteView list);
  void scan_movi(uint64_t start, uint64_t end);
  void read_index(ConstByteView index, uint64_t movi_data_offset);

  ConstByteView file_;
  AviVideoInfo video_;
  AviAudioInfo audio_;
  uint32_t video_stream_ = 0xFFFFFFFF, audio_stream_ = 0xFFFFFFFF;
  List<Chunk> video_frames_, audio_chunks_;
  List<bool> keyframes_;
  bool all_keyframes_ = true;
};

}  // namespace video
