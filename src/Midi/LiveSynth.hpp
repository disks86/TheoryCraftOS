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

// The software synthesizer played live: a MIDI keyboard's notes go in through send() and come out of the speakers a moment later. It is the
// SoftwareSynth driven by an endless PCM source that a player keeps feeding to the sound mixer, so it mixes with everything else that plays.
// The stream only runs while start() is in effect, which is when someone has a keyboard to play.
#pragma once
#include "Audio/AudioMixer.hpp"
#include "Audio/AudioPlayer.hpp"
#include "Midi/SoftwareSynth.hpp"

namespace midi {

class LiveSynth final : public IMidiSink {
 public:
  LiveSynth(audio::AudioMixer& mixer, allocators::IAllocator& allocator);
  ~LiveSynth();

  // Builds the wavetables (once) and starts the stream. False if there is no memory, or the mixer has no stream for it.
  bool start();
  void stop();
  bool running() const { return running_; }
  // Feeds the stream once, for when no thread does it (tests): see AudioPlayer::update().
  void update() { player_.update(); }
  audio::AudioPlayer& player() { return player_; }
  SoftwareSynth& synth() { return synth_; }

  void send(uint8_t status, uint8_t data1, uint8_t data2) override { synth_.send(status, data1, data2); }
  void reset() override { synth_.reset(); }

 private:
  // Sound that never ends: whatever the synthesizer makes next, wherever the player asks for it.
  class Source final : public audio::IPcmSource {
   public:
    explicit Source(SoftwareSynth& synth) : synth_(synth) {}
    audio::StreamFormat format() const override;
    uint64_t frame_count() const override { return uint64_t{1} << 40; }
    uint32_t read(uint64_t frame, ByteView out) override;

   private:
    SoftwareSynth& synth_;
  };

  SoftwareSynth synth_;
  Source source_;
  audio::AudioPlayer player_;
  bool initialized_ = false, running_ = false;
};

}  // namespace midi
