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

// A Standard MIDI File (format 0, 1 or 2; also inside a RIFF "RMID" wrapper) turned into one list of channel messages in time order, with
// the times worked out from the file's tempo changes (or its SMPTE timing). System exclusive messages and the meta events other than the
// tempo are left out. In a format 2 file the songs follow each other.
#pragma once
#include "Midi/MidiTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace midi {

class Sequence {
 public:
  static constexpr uint32_t kMaxTracks = 256;
  static constexpr uint64_t kMaxEvents = 2000000;

  explicit Sequence(allocators::IAllocator& allocator) : allocator_(allocator), events_(allocator) {}

  // Reads the file; false (leaving the sequence empty) if it is not a MIDI file, is damaged beyond use, or holds no notes or messages.
  bool parse(ConstByteView file);

  const List<Event>& events() const { return events_; }
  uint64_t duration_us() const { return events_.count() ? events_[events_.count() - 1].time_us : 0; }
  // 0, 1 or 2; the number of tracks the file said it had.
  uint16_t format() const { return format_; }
  uint16_t track_count() const { return tracks_; }

 private:
  allocators::IAllocator& allocator_;
  List<Event> events_;
  uint16_t format_ = 0, tracks_ = 0;
};

// True if the data starts like a MIDI file ("MThd", or a RIFF "RMID").
bool looks_like_midi(ConstByteView data);

}  // namespace midi
