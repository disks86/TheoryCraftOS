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

#include "Desktop/WebAudio.hpp"
#include "Utility/Move.hpp"

namespace desktop {

namespace {

constexpr audio::StreamFormat kVoice = {8000, 1, audio::SampleType::S16};

class Microphone final : public rtc::IRtcAudioSource {
 public:
  explicit Microphone(SharedPointer<audio::CaptureStream> stream) : stream_(util::move(stream)) {}
  ~Microphone() { stream_->close(); }
  uint32_t read(ArrayView<int16_t> out) override {
    // (the stream gives bytes: little-endian 16-bit samples; each pair is put together, so no cast of the buffer is needed)
    uint8_t bytes[320];
    const uint64_t want = out.size() * 2 < sizeof(bytes) ? out.size() * 2 : sizeof(bytes);
    const uint32_t got = stream_->read(ByteView(bytes, want));
    const uint32_t samples = got / 2;
    for (uint32_t i = 0; i < samples; ++i) out[i] = static_cast<int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    return samples;
  }

 private:
  SharedPointer<audio::CaptureStream> stream_;
};

class Speaker final : public rtc::IRtcAudioSink {
 public:
  explicit Speaker(SharedPointer<audio::AudioStream> stream) : stream_(util::move(stream)) {}
  ~Speaker() { stream_->close(); }
  void write(ConstByteView pcm) override { stream_->write(pcm); }

 private:
  SharedPointer<audio::AudioStream> stream_;
};

}  // namespace

UniquePointer<rtc::IRtcAudioSource> DesktopRtcAudio::open_microphone() {
  if (!capture_) return UniquePointer<rtc::IRtcAudioSource>();
  SharedPointer<audio::CaptureStream> stream = capture_->open_stream(kVoice);
  if (!stream) return UniquePointer<rtc::IRtcAudioSource>();
  UniquePointer<Microphone> mic = UniquePointer<Microphone>::make(allocator_, util::move(stream));
  return UniquePointer<rtc::IRtcAudioSource>(static_cast<UniquePointer<Microphone>&&>(mic));
}

UniquePointer<rtc::IRtcAudioSink> DesktopRtcAudio::open_speaker() {
  if (!mixer_) return UniquePointer<rtc::IRtcAudioSink>();
  SharedPointer<audio::AudioStream> stream = mixer_->open_stream(kVoice);
  if (!stream) return UniquePointer<rtc::IRtcAudioSink>();
  UniquePointer<Speaker> speaker = UniquePointer<Speaker>::make(allocator_, util::move(stream));
  return UniquePointer<rtc::IRtcAudioSink>(static_cast<UniquePointer<Speaker>&&>(speaker));
}

}  // namespace desktop
