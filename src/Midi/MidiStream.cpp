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

#include "Midi/MidiStream.hpp"
#include "Utility/ByteBuffer.hpp"

namespace midi {

namespace {
constexpr uint64_t kTailFrames = 2 * SoftwareSynth::kSampleRate;       // room for the last notes to die away
constexpr uint64_t kHardwareTailFrames = SoftwareSynth::kSampleRate / 2;
}  // namespace

MidiPcmSource::MidiPcmSource(allocators::IAllocator& allocator, UniquePointer<Sequence> sequence, IMidiSink* sink)
    : allocator_(allocator), sequence_(util::move(sequence)), sink_(sink), synth_(allocator) {
  const uint64_t song = sequence_->duration_us() * SoftwareSynth::kSampleRate / 1000000;
  frames_ = song + (sink_ ? kHardwareTailFrames : kTailFrames);
}

MidiPcmSource::~MidiPcmSource() {
  if (sink_) silence(*sink_);   // nothing may go on sounding after the song
}

bool MidiPcmSource::initialize() { return sink_ ? true : synth_.initialize(); }

audio::StreamFormat MidiPcmSource::format() const {
  audio::StreamFormat f;
  f.sample_rate = SoftwareSynth::kSampleRate;
  f.channels = 2;
  f.type = audio::SampleType::S16;
  return f;
}

void MidiPcmSource::apply(const Event& e) {
  if (sink_) sink_->send(e.status, e.data1, e.data2);
  else synth_.send(e.status, e.data1, e.data2);
}

void MidiPcmSource::playback_paused() {
  if (sink_) silence(*sink_);   // (hardware keeps a held note sounding until it is told otherwise)
}

// Goes back to the start and replays what sets up the channels (not the notes) up to `frame`.
void MidiPcmSource::seek_to(uint64_t frame) {
  if (sink_) {
    silence(*sink_);
    sink_->reset();
  } else {
    synth_.reset();
  }
  const List<Event>& events = sequence_->events();
  next_event_ = 0;
  while (next_event_ < events.count() && sample_of(events[next_event_]) < frame) {
    const Event& e = events[next_event_++];
    const uint8_t kind = e.status & 0xF0;
    if (kind != 0x80 && kind != 0x90) apply(e);
  }
  if (sink_) sink_->flush();
  cursor_ = frame;
}

uint32_t MidiPcmSource::read(uint64_t frame, ByteView out) {
  if (frame >= frames_) return 0;
  if (frame != cursor_ || (cursor_ == 0 && next_event_ != 0)) seek_to(frame);
  uint64_t want = out.size() / 4;
  if (want > frames_ - frame) want = frames_ - frame;
  int16_t* samples = reinterpret_cast<int16_t*>(out.data());
  const List<Event>& events = sequence_->events();
  uint64_t done = 0;
  while (done < want) {
    // Events that are due at this frame go in first; then the sound up to the next one is made.
    const uint64_t now = frame + done;
    while (next_event_ < events.count() && sample_of(events[next_event_]) <= now) apply(events[next_event_++]);
    uint64_t until = frame + want;
    if (next_event_ < events.count()) {
      const uint64_t due = sample_of(events[next_event_]);
      if (due < until) until = due;
    }
    const uint64_t n = until - now;
    if (sink_) {
      for (uint64_t i = 0; i < n * 2; ++i) samples[done * 2 + i] = 0;
    } else {
      synth_.render(samples + done * 2, static_cast<uint32_t>(n));
    }
    done += n;
  }
  if (sink_) sink_->flush();
  cursor_ = frame + want;
  return static_cast<uint32_t>(want * 4);
}

UniquePointer<audio::IPcmSource> open_midi_stream(media::IByteSource& source, allocators::IAllocator& allocator, IMidiSink* hardware) {
  UniquePointer<audio::IPcmSource> none;
  const uint64_t size = source.size();
  if (size < 14 || size > kMaxMidiFileBytes) return none;
  ByteBuffer file(allocator);
  if (!file.put_zeros(size) || !media::read_exact(source, 0, ByteView(file.writable_view().data(), size))) return none;
  UniquePointer<Sequence> sequence = UniquePointer<Sequence>::make(allocator, allocator);
  if (!sequence || !sequence->parse(file.view())) return none;
  UniquePointer<MidiPcmSource> stream = UniquePointer<MidiPcmSource>::make(allocator, allocator, util::move(sequence), hardware);
  if (!stream || !stream->initialize()) return none;
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

}  // namespace midi
