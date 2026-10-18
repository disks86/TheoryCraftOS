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
#include "Media/ByteSource.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"

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
  // An audio stream we can play: uncompressed 8- or 16-bit PCM, or A-law, mu-law, IMA ADPCM or Microsoft ADPCM (see
  // `tag`), mono or stereo.
  bool present = false;
  audio::StreamFormat format;  // what it plays as (the compressed kinds decode to 16-bit)
  uint32_t tag = 1;            // WAVE_FORMAT_*: 1 PCM, 2 MS ADPCM, 6 A-law, 7 mu-law, 0x11 IMA ADPCM
  ConstByteView wave_format;   // the stream's WAVEFORMATEX as stored (in the file), for decoding the compressed kinds
  uint64_t bytes = 0;          // all the audio in the file (compressed bytes for the compressed kinds)
};

// Reads an AVI file: one video stream and, if there is one, one audio stream (PCM, A-law, mu-law, IMA or Microsoft ADPCM). Video may be
// Motion JPEG ("MJPG"), raw I420/NV12, or a compressed codec the decoders may not have (H.264 and so on, flagged by video().codec).
// The file is read through a byte source: open() reads the headers and works out where every frame and every piece of sound is (from the
// file's index if it has one and it is right, else by walking the "movi" list, so a missing or wrong index does no harm); the frames
// and the sound themselves are only read when they are asked for. A file held in memory can be given as such (borrowed: it must
// outlive the reader), and then frames are not even copied.
class AviFile {
 public:
  explicit AviFile(allocators::IAllocator& allocator);
  AviFile(const AviFile&) = delete;
  AviFile& operator=(const AviFile&) = delete;

  // False if the file is not an AVI, or has no video stream, or is cut short before its first frame.
  bool open(ConstByteView file);
  // The same for a file in a source (borrowed: it must outlive the reader, and be read by nobody else meanwhile).
  bool open(media::IByteSource& source);
  const AviVideoInfo& video() const { return video_; }
  const AviAudioInfo& audio() const { return audio_; }

  uint32_t frame_count() const { return static_cast<uint32_t>(video_frames_.count()); }
  // Frame `index` as a packet (its time follows from the frame rate). Its data is valid until the next call of frame().
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
  // The sound track as one run of bytes - the audio chunks one after the other - for decoders that read it a piece at a time.
  media::IByteSource& audio_source() { return audio_chunks_source_; }

 private:
  struct Chunk {
    uint64_t offset;
    uint32_t size;
  };

  // The audio chunks as a byte source.
  class AudioChunks final : public media::IByteSource {
   public:
    explicit AudioChunks(AviFile& owner) : owner_(owner) {}
    uint64_t size() const override { return owner_.audio_total_; }
    uint64_t read(uint64_t offset, ByteView out) override;

   private:
    AviFile& owner_;
  };

  bool parse_headers(ConstByteView list);
  void scan_movi(media::IByteSource& source, uint64_t start, uint64_t end);
  bool load_index(media::IByteSource& source, uint64_t index_start, uint64_t index_size, uint64_t movi_tag_offset);
  void read_index_flags(media::IByteSource& source, uint64_t index_start, uint64_t index_size, uint64_t movi_tag_offset);

  allocators::IAllocator& allocator_;
  media::IByteSource* source_ = nullptr;
  media::MemoryByteSource memory_;   // when the file was given as memory
  bool in_memory_ = false;
  ByteBuffer headers_;               // the "hdrl" list (the audio format points into it)
  mutable ByteBuffer scratch_;       // the frame frame() last gave out, when it had to be read
  AviVideoInfo video_;
  AviAudioInfo audio_;
  uint32_t video_stream_ = 0xFFFFFFFF, audio_stream_ = 0xFFFFFFFF;
  List<Chunk> video_frames_, audio_chunks_;
  List<uint64_t> audio_starts_;      // where each audio chunk begins in the sound track
  uint64_t audio_total_ = 0;
  List<bool> keyframes_;
  bool all_keyframes_ = true;
  AudioChunks audio_chunks_source_{*this};
};

}  // namespace video
