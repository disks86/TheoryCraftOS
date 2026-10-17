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

#include "Audio/AudioMixer.hpp"

namespace audio {

using namespace hardware;

bool AudioMixer::set_device(SharedPointer<IAudioDevice> device) {
  if (!device) return false;
  kernel::ScopedMutex guard(mutex_);
  device->set_volume(volume_);
  device->set_muted(muted_);
  pending_bytes_ = 0;       // what was cut for the old device is lost
  pending_offset_ = 0;
  device_ = device.get();
  owner_ = util::move(device);
  return true;
}

IAudioDevice* AudioMixer::current_device() {
  kernel::ScopedMutex guard(mutex_);
  return device_;
}

SharedPointer<IAudioDevice> AudioMixer::shared_device() {
  kernel::ScopedMutex guard(mutex_);
  return owner_;
}

void AudioMixer::set_master_volume(uint8_t volume) {
  kernel::ScopedMutex guard(mutex_);
  device_->set_volume(volume);
  volume_ = device_->volume();
}

uint8_t AudioMixer::master_volume() const {
  kernel::ScopedMutex guard(mutex_);
  return volume_;
}

void AudioMixer::set_master_muted(bool muted) {
  kernel::ScopedMutex guard(mutex_);
  device_->set_muted(muted);
  muted_ = muted;
}

bool AudioMixer::master_muted() const {
  kernel::ScopedMutex guard(mutex_);
  return muted_;
}

bool AudioMixer::initialize() {
  accumulator_ = allocate_view<int32_t>(allocator_, uint64_t{kChunkFrames} * kAudioChannels, 16);
  output_ = allocate_view<uint8_t>(allocator_, uint64_t{kChunkFrames} * kAudioFrameSize, 16);
  if (accumulator_.empty() || output_.empty()) {
    release_view(allocator_, accumulator_);
    release_view(allocator_, output_);
    return false;
  }
  return true;
}

SharedPointer<AudioStream> AudioMixer::open_stream(const StreamFormat& format, uint32_t ring_bytes) {
  kernel::ScopedMutex guard(mutex_);
  if (streams_.count() >= kMaxStreams) return SharedPointer<AudioStream>();
  SharedPointer<AudioStream> stream = SharedPointer<AudioStream>::make(allocator_, allocator_, format);
  if (!stream || !stream->allocate(ring_bytes) || !streams_.push_back(stream)) return SharedPointer<AudioStream>();
  ++stats_.streams_opened;
  return stream;
}

uint32_t AudioMixer::stream_count() {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(streams_.count());
}

MixerStatistics AudioMixer::statistics() {
  kernel::ScopedMutex guard(mutex_);
  MixerStatistics copy = stats_;
  copy.active_streams = static_cast<uint32_t>(streams_.count());
  return copy;
}

// Offers the device what it did not take last time.
void AudioMixer::flush_pending() {
  if (pending_bytes_ == 0) return;
  const uint32_t taken = device_->write(ConstByteView(output_).subview(pending_offset_, pending_bytes_));
  pending_offset_ += taken;
  pending_bytes_ -= taken;
}

void AudioMixer::pump() {
  kernel::ScopedMutex guard(mutex_);
  if (accumulator_.empty()) return;

  // Streams that are over go first, so a closed stream does not hold a slot.
  for (uint64_t i = streams_.count(); i > 0; --i)
    if (streams_[i - 1]->finished()) streams_.remove_at(i - 1);

  flush_pending();
  if (pending_bytes_ > 0) return;  // the device is still behind

  // How much to add: up to the target delay, and no more than the device has room for.
  const uint32_t queued_frames = device_->queued_bytes() / kAudioFrameSize;
  if (queued_frames >= kTargetDelayFrames) return;
  uint32_t frames = kTargetDelayFrames - queued_frames;
  const uint32_t room = device_->free_bytes() / kAudioFrameSize;
  if (frames > room) frames = room;
  if (frames > kChunkFrames) frames = kChunkFrames;
  if (frames == 0) return;

  accumulator_.first(uint64_t{frames} * kAudioChannels).fill(0);
  uint32_t mixed = 0;
  for (SharedPointer<AudioStream>& stream : streams_) {
    const uint32_t taken = stream->mix_into(accumulator_.data(), frames);
    if (taken > mixed) mixed = taken;
  }
  if (mixed == 0) return;  // nothing to play: the device idles on silence

  for (uint32_t i = 0; i < mixed * kAudioChannels; ++i) {
    int32_t sample = accumulator_[i];
    if (sample > 32767) {
      sample = 32767;
      ++stats_.clipped_samples;
    } else if (sample < -32768) {
      sample = -32768;
      ++stats_.clipped_samples;
    }
    output_[2 * i] = static_cast<uint8_t>(sample);
    output_[2 * i + 1] = static_cast<uint8_t>(sample >> 8);
  }
  pending_offset_ = 0;
  pending_bytes_ = mixed * kAudioFrameSize;
  stats_.frames_mixed += mixed;
  flush_pending();
}

bool AudioMixer::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "audio");
  return service_ != nullptr;
}

void AudioMixer::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void AudioMixer::service_main(void* self) {
  AudioMixer* mixer = static_cast<AudioMixer*>(self);
  while (!__atomic_load_n(&mixer->stop_, __ATOMIC_ACQUIRE)) {
    mixer->pump();
    kernel::sleep_us(kPumpIntervalUs);
  }
}

}  // namespace audio
