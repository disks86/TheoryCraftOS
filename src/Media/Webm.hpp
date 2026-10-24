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
#include "Media/ByteSource.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

enum class WebmTrackKind : uint8_t { Other, Video, Audio };
enum class WebmCodec : uint8_t { Unknown, Vp8, Vp9, Av1, Opus, Vorbis };
// What was done to a track's frames before they were stored (ContentEncodings).
enum class WebmCompression : uint8_t { None, HeaderStripping, Zlib, Unsupported };

// One frame (a laced block holds several), as found while the file was scanned: where its bytes are, and when it plays.
struct WebmSample {
  uint64_t offset = 0;        // of the stored bytes in the file
  uint32_t size = 0;          // stored bytes (a stripped header is not counted)
  uint32_t duration_us = 0;   // 0 until the track was finished: then always set
  int64_t pts_us = 0;
  bool keyframe = false;
};

struct WebmTrack {
  explicit WebmTrack(allocators::IAllocator& allocator) : codec_private(allocator), strip(allocator), samples(allocator) {}
  uint64_t number = 0;
  WebmTrackKind kind = WebmTrackKind::Other;
  WebmCodec codec = WebmCodec::Unknown;
  bool enabled = true;
  bool is_default = false;
  bool encrypted = false;                              // ContentEncryption: the frames cannot be read without a key
  WebmCompression compression = WebmCompression::None;
  uint32_t width = 0, height = 0;                      // video: coded size
  uint32_t display_width = 0, display_height = 0;      // video: the size to show it at (the coded size if the file does not say)
  uint32_t channels = 0, sample_rate = 0, bit_depth = 0;   // audio (the output rate when the file gives two)
  uint32_t default_duration_us = 0;
  int64_t codec_delay_ns = 0, seek_pre_roll_ns = 0;
  int64_t last_discard_ns = 0;                         // DiscardPadding of the last block: sound to cut from the end
  ByteBuffer codec_private;
  ByteBuffer strip;                                    // the bytes HeaderStripping took off the front of every frame
  List<WebmSample> samples;

  // Can the frames be turned into pictures or sound by the decoders of this OS?
  bool decodable() const;
};

// Reads a WebM (or Matroska) file through a source: the headers and the position, time and keyframe flag of every frame of every track. Frame
// bytes stay in the source and are read with read_sample(). Handles segments and clusters of unknown size (live streams), SimpleBlocks and
// BlockGroups, all three kinds of lacing, header-stripping and zlib content compression, and files cut short (the frames that are whole count).
class WebmFile {
 public:
  explicit WebmFile(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~WebmFile() { clear(); }
  WebmFile(const WebmFile&) = delete;
  WebmFile& operator=(const WebmFile&) = delete;

  static constexpr uint32_t kMaxTracks = 16;
  static constexpr uint64_t kMaxSamplesPerTrack = 1u << 22;

  bool open(IByteSource& source);
  void clear();
  bool is_webm() const { return webm_; }              // the DocType says "webm" (else "matroska")
  int64_t duration_us() const { return duration_us_; }
  uint32_t track_count() const { return count_; }
  const WebmTrack& track(uint32_t index) const { return *tracks_[index]; }
  // The first (enabled, preferably default) track of a kind that can be decoded, or nullptr.
  const WebmTrack* find(WebmTrackKind kind) const;
  // Appends the bytes of one frame to `out`, with the compression undone. False if it is outside the source or memory ran out.
  bool read_sample(IByteSource& source, const WebmTrack& track, uint32_t index, ByteBuffer* out) const;

 private:
  struct Scan;
  bool scan(IByteSource& source);
  allocators::IAllocator& allocator_;
  UniquePointer<WebmTrack> tracks_[kMaxTracks];
  uint32_t count_ = 0;
  int64_t duration_us_ = 0;
  bool webm_ = false;
};

// True if the data starts like an EBML document (the 1A 45 DF A3 header of Matroska and WebM).
inline bool looks_like_webm(ConstByteView d) { return d.size() >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3; }

// The sound of a track as an Ogg Opus or Ogg Vorbis file (the form the audio decoders read), built from its frames. False if the track is
// not Opus (mono or stereo) or Vorbis, or the sound is damaged. (Defined with the audio decoders, in Audio/WebmAudio.cpp.)
bool webm_audio_to_ogg(IByteSource& source, const WebmFile& file, const WebmTrack& track, allocators::IAllocator& allocator, ByteBuffer* out);

}  // namespace media
