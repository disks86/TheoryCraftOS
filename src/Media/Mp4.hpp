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
#include "Utility/ByteBuffer.hpp"
#include "Utility/AllocatedView.hpp"

namespace media {

enum class Mp4TrackKind : uint8_t { Other, Audio, Video };

// One track of an MP4/MOV file and its sample table.
struct Mp4Track {
  Mp4TrackKind kind = Mp4TrackKind::Other;
  uint32_t id = 0;
  uint32_t timescale = 0;
  uint64_t duration = 0;          // in `timescale` units
  uint32_t codec = 0;             // the sample entry's four-character code, big-endian ('mp4a', 'avc1', ...)
  uint32_t object_type = 0;       // from the esds descriptor (0x40 AAC, 0x6B MPEG-1 audio, ...), 0 if there is none
  uint32_t width = 0, height = 0; // video
  uint32_t channels = 0, sample_rate = 0;  // audio, as the sample entry says
  ConstByteView config;           // AudioSpecificConfig (esds) or the avcC record; points into the file
  uint32_t sample_count = 0;
  ArrayView<uint64_t> offsets;    // where each sample is in the file
  ArrayView<uint32_t> sizes;
  ArrayView<int64_t> dts;         // decode time of each sample, in timescale units
  ArrayView<int32_t> cts_offsets; // presentation minus decode time (empty if the track has no reordering)
  ArrayView<uint8_t> sync;        // 1 for samples that can be decoded without earlier ones
  int64_t skip = 0;               // leading time to drop (an edit list's start), in timescale units
  int64_t play_duration = -1;     // length of the edit list's segment in timescale units, or -1 for all of it
};

// Reads an MP4 / QuickTime file held in memory (borrowed: it must outlive the reader): the tracks and where their samples are.
// Fragmented files (moof) and files whose sample tables are missing are not handled.
class Mp4File {
 public:
  explicit Mp4File(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Mp4File() { clear(); }
  Mp4File(const Mp4File&) = delete;
  Mp4File& operator=(const Mp4File&) = delete;

  bool open(ConstByteView file);
  // Reads a file through a source: only the movie box (the tables that say where the samples are) is read, into memory kept until clear();
  // the samples stay in the source and are read with read_sample().
  bool open(IByteSource& source);
  void clear();
  uint32_t track_count() const { return count_; }
  const Mp4Track& track(uint32_t index) const { return tracks_[index]; }
  // The first track of a kind, or nullptr.
  const Mp4Track* find(Mp4TrackKind kind) const;
  // The bytes of one sample (empty if it is out of the file).
  ConstByteView sample(const Mp4Track& track, uint32_t index) const;
  // Appends one sample read from `source` to `out`. False if it is outside the source or memory ran out.
  bool read_sample(IByteSource& source, const Mp4Track& track, uint32_t index, ByteBuffer* out) const;

 private:
  static constexpr uint32_t kMaxTracks = 8;
  allocators::IAllocator& allocator_;
  bool parse(ConstByteView file);

  ConstByteView file_;
  ByteView moov_;   // the movie box, when the file is read through a source
  Mp4Track tracks_[kMaxTracks];
  uint32_t count_ = 0;
};

// True if the data starts like an ISO base media file (an 'ftyp' box, or a leading 'moov'/'mdat'/'free'/'wide' box).
bool looks_like_mp4(ConstByteView data);

}  // namespace media
