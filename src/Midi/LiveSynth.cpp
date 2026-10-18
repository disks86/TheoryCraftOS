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

#include "Midi/LiveSynth.hpp"

namespace midi {

audio::StreamFormat LiveSynth::Source::format() const {
  audio::StreamFormat f;
  f.sample_rate = SoftwareSynth::kSampleRate;
  f.channels = 2;
  f.type = audio::SampleType::S16;
  return f;
}

uint32_t LiveSynth::Source::read(uint64_t, ByteView out) {
  const uint32_t frames = static_cast<uint32_t>(out.size() / 4);
  synth_.render(reinterpret_cast<int16_t*>(out.data()), frames);
  return frames * 4;
}

LiveSynth::LiveSynth(audio::AudioMixer& mixer, allocators::IAllocator& allocator) : synth_(allocator), source_(synth_), player_(mixer, allocator) {}

LiveSynth::~LiveSynth() { stop(); }

bool LiveSynth::start() {
  if (running_) return true;
  if (!initialized_ && !(initialized_ = synth_.initialize())) return false;
  if (!player_.open(&source_) || !player_.play()) {
    player_.close();
    return false;
  }
  player_.start_service();   // (without threads the owner calls update())
  running_ = true;
  return true;
}

void LiveSynth::stop() {
  if (!running_) return;
  player_.stop_service();
  player_.close();
  synth_.reset();
  running_ = false;
}

}  // namespace midi
