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

// A MIDI file as a stream of sound for the media player. open_midi_stream() reads the file, and gives back a PCM source (48 kHz, 16-bit,
// stereo) that plays it:
//   - in software: the file's notes drive a SoftwareSynth and the source is what it renders, so the sound goes through the audio system like
//     any other, and the position, seeking and the level meter of the player work as they do for a recording;
//   - on MIDI hardware: when a hardware MIDI output is given, the notes are sent to it as the player reads the source (which is then silent),
//     in step with the player's position. Seeking silences the output and sets the instruments and controllers the song had by then.
// Reading the source out of order (a seek) goes back to the start of the song and replays the controller and program changes up to the
// new position, but not the notes: a note that was already sounding at that moment is not heard.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Audio/PcmSource.hpp"
#include "Media/ByteSource.hpp"
#include "Midi/MidiFile.hpp"
#include "Midi/MidiTypes.hpp"
#include "Midi/SoftwareSynth.hpp"
#include "Utility/UniquePointer.hpp"

namespace midi {

constexpr uint32_t kMaxMidiFileBytes = 16u << 20;

// Null if `source` is not a MIDI file, is too big (kMaxMidiFileBytes), or memory ran out. `hardware` (may be null) must outlive the source.
UniquePointer<audio::IPcmSource> open_midi_stream(media::IByteSource& source, allocators::IAllocator& allocator, IMidiSink* hardware = nullptr);

// The pieces, for tests: the source over a parsed sequence, with the software synth or with a sink that sounds the notes.
class MidiPcmSource final : public audio::IPcmSource {
 public:
  // The sequence is taken over. With a `sink` the source is silent and sends the messages to it; without one the synth is used.
  MidiPcmSource(allocators::IAllocator& allocator, UniquePointer<Sequence> sequence, IMidiSink* sink);
  ~MidiPcmSource();

  bool initialize();   // builds the synth's tables (nothing to do for a sink)

  audio::StreamFormat format() const override;
  uint64_t frame_count() const override { return frames_; }
  uint32_t read(uint64_t frame, ByteView out) override;
  void playback_paused() override;

  bool uses_hardware() const { return sink_ != nullptr; }

 private:
  void seek_to(uint64_t frame);
  void apply(const Event& e);
  uint64_t sample_of(const Event& e) const { return e.time_us * SoftwareSynth::kSampleRate / 1000000; }

  allocators::IAllocator& allocator_;
  UniquePointer<Sequence> sequence_;
  IMidiSink* sink_;
  SoftwareSynth synth_;
  uint64_t frames_ = 0;
  uint64_t cursor_ = 0;   // the frame the next read should start at
  uint64_t next_event_ = 0;
};

}  // namespace midi
