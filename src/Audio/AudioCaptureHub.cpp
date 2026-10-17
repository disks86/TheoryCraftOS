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

#include "Audio/AudioCaptureHub.hpp"

namespace audio {

using namespace hardware;

void AudioCaptureHub::set_source(IAudioCapture* source, SharedPointer<IAudioDevice> owner) {
  kernel::ScopedMutex guard(mutex_);
  if (source) {
    source->set_gain(gain_);
    source->set_muted(muted_);
    source->flush();   // listeners hear the new source from now
  }
  device_ = source;
  owner_ = source ? util::move(owner) : SharedPointer<IAudioDevice>();
}

IAudioCapture* AudioCaptureHub::source() {
  kernel::ScopedMutex guard(mutex_);
  return device_;
}

IAudioDevice* AudioCaptureHub::source_device() {
  kernel::ScopedMutex guard(mutex_);
  return owner_.get();
}

void AudioCaptureHub::set_master_gain(uint8_t gain) {
  kernel::ScopedMutex guard(mutex_);
  if (device_) {
    device_->set_gain(gain);
    gain_ = device_->gain();
  } else {
    gain_ = gain < kAudioMaxVolume ? gain : kAudioMaxVolume;
  }
}

uint8_t AudioCaptureHub::master_gain() const {
  kernel::ScopedMutex guard(mutex_);
  return gain_;
}

void AudioCaptureHub::set_master_muted(bool muted) {
  kernel::ScopedMutex guard(mutex_);
  if (device_) device_->set_muted(muted);
  muted_ = muted;
}

bool AudioCaptureHub::master_muted() const {
  kernel::ScopedMutex guard(mutex_);
  return muted_;
}

bool AudioCaptureHub::initialize() {
  chunk_ = allocate_view<uint8_t>(allocator_, uint64_t{kChunkFrames} * kAudioFrameSize, 16);
  return !chunk_.empty();
}

SharedPointer<CaptureStream> AudioCaptureHub::open_stream(const StreamFormat& format, uint32_t ring_bytes) {
  kernel::ScopedMutex guard(mutex_);
  if (streams_.count() >= kMaxStreams) return SharedPointer<CaptureStream>();
  SharedPointer<CaptureStream> stream = SharedPointer<CaptureStream>::make(allocator_, allocator_, format);
  if (!stream || !stream->allocate(ring_bytes)) return SharedPointer<CaptureStream>();
  if (streams_.count() == 0 && device_) device_->flush();  // the first listener hears from now, not from earlier
  if (!streams_.push_back(stream)) return SharedPointer<CaptureStream>();
  ++stats_.streams_opened;
  return stream;
}

uint32_t AudioCaptureHub::stream_count() {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(streams_.count());
}

CaptureHubStatistics AudioCaptureHub::statistics() {
  kernel::ScopedMutex guard(mutex_);
  CaptureHubStatistics copy = stats_;
  copy.active_streams = static_cast<uint32_t>(streams_.count());
  return copy;
}

void AudioCaptureHub::pump() {
  kernel::ScopedMutex guard(mutex_);
  if (chunk_.empty()) return;
  for (uint64_t i = streams_.count(); i > 0; --i)
    if (streams_[i - 1]->closed()) streams_.remove_at(i - 1);
  if (streams_.count() == 0) {
    if (device_) device_->flush();  // nobody is listening
    return;
  }
  for (; device_;) {
    const uint32_t bytes = device_->read(chunk_);
    if (bytes == 0) break;
    stats_.frames_distributed += bytes / kAudioFrameSize;
    const ConstByteView block = ConstByteView(chunk_).first(bytes);
    for (SharedPointer<CaptureStream>& stream : streams_) stream->deliver(block);
    if (bytes < chunk_.size()) break;
  }
}

bool AudioCaptureHub::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "capture");
  return service_ != nullptr;
}

void AudioCaptureHub::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void AudioCaptureHub::service_main(void* self) {
  AudioCaptureHub* hub = static_cast<AudioCaptureHub*>(self);
  while (!__atomic_load_n(&hub->stop_, __ATOMIC_ACQUIRE)) {
    hub->pump();
    kernel::sleep_us(kPumpIntervalUs);
  }
}

}  // namespace audio
