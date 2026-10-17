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

#include "Audio/AudioPlayer.hpp"

namespace audio {

AudioPlayer::~AudioPlayer() {
  stop_service();
  close();
}

void AudioPlayer::apply_stream_settings() {
  if (!stream_) return;
  stream_->set_volume(volume_);
  stream_->set_pan(pan_);
  stream_->set_muted(muted_);
}

void AudioPlayer::change_state(PlayerState state, bool* notify) {
  if (state == state_) return;
  state_ = state;
  *notify = true;
}

bool AudioPlayer::open(IPcmSource* source) {
  if (!source) return false;
  const StreamFormat format = source->format();
  if (!format.valid()) return false;
  bool notify = false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (stream_) stream_->close();  // the old sound is cut off
    stream_ = mixer_.open_stream(format);
    if (!stream_) {
      source_ = nullptr;
      duration_ = next_frame_ = 0;
      change_state(PlayerState::Empty, &notify);
    } else {
      source_ = source;
      duration_ = source->frame_count();
      rate_ = format.sample_rate;
      frame_size_ = format.frame_size();
      next_frame_ = 0;
      apply_stream_settings();
      change_state(PlayerState::Stopped, &notify);
    }
  }
  if (notify && notify_) notify_(notify_context_);
  return source_ != nullptr;
}

void AudioPlayer::close() {
  bool notify = false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (stream_) stream_->close();
    stream_ = SharedPointer<AudioStream>();
    source_ = nullptr;
    duration_ = next_frame_ = 0;
    change_state(PlayerState::Empty, &notify);
  }
  if (notify && notify_) notify_(notify_context_);
}

bool AudioPlayer::play() {
  bool notify = false;
  bool ok;
  {
    kernel::ScopedMutex guard(mutex_);
    ok = source_ != nullptr;
    if (ok) {
      if (next_frame_ >= duration_) next_frame_ = 0;  // played to the end: start again
      change_state(PlayerState::Playing, &notify);
    }
  }
  if (notify && notify_) notify_(notify_context_);
  if (ok) update();
  return ok;
}

void AudioPlayer::pause() {
  bool notify = false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (state_ != PlayerState::Playing) return;
    // What was fed but not yet heard is thrown away, and the position goes back to where we were.
    if (stream_) {
      const uint64_t queued = frame_size_ ? stream_->queued_bytes() / frame_size_ : 0;
      next_frame_ = next_frame_ > queued ? next_frame_ - queued : 0;
      stream_->flush();
    }
    change_state(PlayerState::Paused, &notify);
  }
  if (notify && notify_) notify_(notify_context_);
}

void AudioPlayer::stop() {
  bool notify = false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (!source_) return;
    if (stream_) stream_->flush();
    next_frame_ = 0;
    change_state(PlayerState::Stopped, &notify);
  }
  if (notify && notify_) notify_(notify_context_);
}

void AudioPlayer::seek(uint64_t frame) {
  kernel::ScopedMutex guard(mutex_);
  if (!source_) return;
  next_frame_ = frame < duration_ ? frame : duration_;
  if (stream_) stream_->flush();
}

void AudioPlayer::set_looping(bool looping) {
  kernel::ScopedMutex guard(mutex_);
  looping_ = looping;
}

uint64_t AudioPlayer::position() {
  kernel::ScopedMutex guard(mutex_);
  if (!source_ || !stream_ || state_ != PlayerState::Playing) return next_frame_;
  const uint64_t queued = frame_size_ ? stream_->queued_bytes() / frame_size_ : 0;
  return next_frame_ > queued ? next_frame_ - queued : 0;
}

void AudioPlayer::set_volume(uint8_t volume) {
  kernel::ScopedMutex guard(mutex_);
  volume_ = volume < hardware::kAudioMaxVolume ? volume : hardware::kAudioMaxVolume;
  apply_stream_settings();
}

void AudioPlayer::set_pan(int pan) {
  kernel::ScopedMutex guard(mutex_);
  pan_ = pan < kPanLeft ? kPanLeft : pan > kPanRight ? kPanRight : pan;
  apply_stream_settings();
}

void AudioPlayer::set_muted(bool muted) {
  kernel::ScopedMutex guard(mutex_);
  muted_ = muted;
  apply_stream_settings();
}

uint32_t AudioPlayer::take_peak() {
  kernel::ScopedMutex guard(mutex_);
  return stream_ ? stream_->take_peak() : 0;
}

void AudioPlayer::update() {
  bool notify = false;
  {
    kernel::ScopedMutex guard(mutex_);
    if (state_ != PlayerState::Playing || !source_ || !stream_) return;
    uint8_t chunk[kChunkBytes];
    for (;;) {
      if (next_frame_ >= duration_) {
        if (!looping_ || duration_ == 0) break;
        next_frame_ = 0;  // round again
      }
      const uint32_t room = stream_->free_bytes();
      if (room < frame_size_) break;
      uint64_t want = (duration_ - next_frame_) * frame_size_;
      if (want > room) want = room;
      if (want > sizeof chunk) want = sizeof chunk;
      want -= want % frame_size_;
      if (want == 0) break;
      const uint32_t got = source_->read(next_frame_, ByteView(chunk).first(want));
      if (got == 0) {  // the source ended early
        next_frame_ = duration_;
        break;
      }
      const uint32_t written = stream_->write(ConstByteView(chunk).first(got));
      next_frame_ += written / frame_size_;
      if (written < got) break;  // the stream is full
    }
    // At the end, once the mixer has taken the last of it, the player stops.
    if (next_frame_ >= duration_ && !looping_ && stream_->drained()) change_state(PlayerState::Stopped, &notify);
  }
  if (notify && notify_) notify_(notify_context_);
}

bool AudioPlayer::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "player");
  return service_ != nullptr;
}

void AudioPlayer::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void AudioPlayer::service_main(void* self) {
  AudioPlayer* player = static_cast<AudioPlayer*>(self);
  while (!__atomic_load_n(&player->stop_, __ATOMIC_ACQUIRE)) {
    player->update();
    kernel::sleep_us(10000);
  }
}

}  // namespace audio
