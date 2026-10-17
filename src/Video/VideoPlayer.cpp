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

#include "Video/VideoPlayer.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"

namespace video {

using namespace hardware;

VideoPlayer::~VideoPlayer() {
  stop_service();
  release();
}

// Drops everything of the open file (the decoder, the sound, the picture).
void VideoPlayer::release() {
  audio_player_.reset();  // before the source and samples it plays from
  audio_source_.reset();
  release_view(allocator_, audio_samples_);
  decoder_.reset();
  state_ = VideoState::Empty;
  have_picture_ = false;
  width_ = height_ = 0;
  duration_us_ = 0;
  current_frame_ = 0;
  paused_position_us_ = 0;
  ++serial_;
}

void VideoPlayer::close() {
  kernel::ScopedMutex guard(mutex_);
  release();
}

OpenResult VideoPlayer::open(ConstByteView avi) {
  kernel::ScopedMutex guard(mutex_);
  release();
  if (!file_.open(avi)) return OpenResult::NotAvi;
  const AviVideoInfo& info = file_.video();
  if (!info.supported || info.width == 0 || info.height == 0) return OpenResult::NoDecoder;

  DecoderConfig config;
  config.codec = info.codec;
  config.width = info.width;
  config.height = info.height;
  config.output = PixelFormat::Bgra8888;
  config.raw_format = info.raw_format;
  decoder_ = codecs_.create_decoder(config);
  if (!decoder_) return OpenResult::NoDecoder;

  // The first picture tells the real size (the stream's headers win over the container's).
  width_ = info.width;
  height_ = info.height;
  duration_us_ = file_.duration_us();
  current_frame_ = 0;
  if (!decode_frame(0)) {
    release();
    return OpenResult::NoDecoder;
  }
  have_picture_ = true;

  // The sound: all the audio chunks gathered into one run, played by an AudioPlayer.
  if (mixer_ && file_.audio().present && file_.audio().bytes > 0) {
    audio_samples_ = allocate_view<uint8_t>(allocator_, file_.audio().bytes);
    if (!audio_samples_.empty()) {
      const uint64_t bytes = file_.copy_audio(audio_samples_);
      audio_source_ = UniquePointer<audio::MemoryPcmSource>::make(allocator_, ConstByteView(audio_samples_).first(bytes), file_.audio().format);
      audio_player_ = UniquePointer<audio::AudioPlayer>::make(allocator_, *mixer_, allocator_);
      if (!audio_source_ || !audio_player_ || !audio_player_->open(audio_source_.get())) {
        audio_player_.reset();
        audio_source_.reset();
        release_view(allocator_, audio_samples_);  // play without sound
      }
    }
  }
  state_ = VideoState::Stopped;
  return OpenResult::Ok;
}

// Decodes frame `index` into the picture. False if the decoder gave nothing (the old picture stays).
bool VideoPlayer::decode_frame(uint32_t index) {
  VideoPacket packet;
  if (!decoder_ || !file_.frame(index, &packet)) return false;
  VideoFrame frame;
  SubmitResult result = decoder_->submit(packet);
  if (result == SubmitResult::Full) {  // something is waiting to be collected: take it, then try again
    decoder_->receive(&frame);
    result = decoder_->submit(packet);
  }
  if (result != SubmitResult::Accepted || !decoder_->receive(&frame)) return false;
  ++decoded_;
  const VideoFormat& f = frame.format;
  if (f.width != width_ || f.height != height_ || !display_.valid()) {
    if (!display_.allocate({f.width, f.height, PixelFormat::Bgra8888})) return false;
    width_ = f.width;
    height_ = f.height;
    surface_.pixels = ArrayView<uint32_t>(reinterpret_cast<uint32_t*>(display_.plane(0).data()), uint64_t{f.width} * f.height);
    surface_.width = f.width;
    surface_.height = f.height;
    surface_.pitch = f.width;
  }
  if (!convert_to_bgra(frame, reinterpret_cast<uint32_t*>(display_.plane(0).data()), width_)) return false;
  current_frame_ = index;
  ++serial_;
  return true;
}

// Brings the picture to frame `target`. A codec whose pictures all stand alone jumps there; one that builds on earlier
// pictures decodes every frame since the last keyframe it can reach.
void VideoPlayer::show_frame(uint32_t target) {
  if (file_.all_keyframes()) {
    if (target > current_frame_ + 1) skipped_ += target - current_frame_ - 1;
    decode_frame(target);
    return;
  }
  uint32_t from = current_frame_ + 1;
  if (target < current_frame_ || target - current_frame_ > kMaxCatchUpFrames) {
    const uint32_t key = file_.keyframe_at_or_before(target);
    if (target < current_frame_ || key > current_frame_) {  // start again from a keyframe
      decoder_->reset();
      from = key;
    }
  }
  for (uint32_t i = from; i <= target; ++i) decode_frame(i);
}

void VideoPlayer::rebase(int64_t position_us) {
  base_position_us_ = position_us;
  base_time_us_ = clock_.now_us();
}

int64_t VideoPlayer::position_locked() {
  if (state_ == VideoState::Empty) return 0;
  if (state_ != VideoState::Playing) return paused_position_us_;
  int64_t position;
  if (audio_player_ && audio_player_->playing()) {
    position = static_cast<int64_t>(audio_player_->position() * 1000000 / audio_player_->sample_rate());
    rebase(position);  // if the sound ends first, the picture carries on from here
  } else {
    // If the sound has played to its end, the picture carries on from there by the clock.
    if (audio_player_ && audio_player_->position() >= audio_player_->duration()) {
      const int64_t audio_end = static_cast<int64_t>(audio_player_->duration() * 1000000 / audio_player_->sample_rate());
      if (base_position_us_ < audio_end) rebase(audio_end);
    }
    position = base_position_us_ + static_cast<int64_t>(clock_.now_us() - base_time_us_);
  }
  return position < 0 ? 0 : position > duration_us_ ? duration_us_ : position;
}

int64_t VideoPlayer::position_us() {
  kernel::ScopedMutex guard(mutex_);
  return position_locked();
}

bool VideoPlayer::copy_picture(uint32_t* dest, uint32_t pitch, uint32_t* serial) {
  kernel::ScopedMutex guard(mutex_);
  if (state_ == VideoState::Empty || !display_.valid() || pitch < width_ || !dest) return false;
  for (uint32_t y = 0; y < height_; ++y)
    __builtin_memcpy(dest + uint64_t{y} * pitch, display_.plane(0).data() + uint64_t{y} * width_ * 4, uint64_t{width_} * 4);
  *serial = serial_;
  return true;
}

bool VideoPlayer::play() {
  kernel::ScopedMutex guard(mutex_);
  if (state_ == VideoState::Empty) return false;
  if (state_ == VideoState::Playing) return true;
  if (paused_position_us_ >= duration_us_) {  // played to the end: from the start again
    paused_position_us_ = 0;
    show_frame(0);
    if (audio_player_) audio_player_->seek(0);
  }
  state_ = VideoState::Playing;
  rebase(paused_position_us_);
  if (audio_player_) audio_player_->play();
  return true;
}

void VideoPlayer::pause() {
  kernel::ScopedMutex guard(mutex_);
  if (state_ != VideoState::Playing) return;
  paused_position_us_ = position_locked();
  state_ = VideoState::Paused;
  if (audio_player_) audio_player_->pause();
}

void VideoPlayer::seek(int64_t time_us) {
  kernel::ScopedMutex guard(mutex_);
  if (state_ == VideoState::Empty) return;
  if (time_us < 0) time_us = 0;
  if (time_us > duration_us_) time_us = duration_us_;
  show_frame(file_.frame_at_time(time_us));
  paused_position_us_ = time_us;
  rebase(time_us);
  if (audio_player_) audio_player_->seek(static_cast<uint64_t>(time_us) * audio_player_->sample_rate() / 1000000);
}

void VideoPlayer::stop() {
  kernel::ScopedMutex guard(mutex_);
  if (state_ == VideoState::Empty) return;
  state_ = VideoState::Stopped;
  if (audio_player_) audio_player_->stop();
  show_frame(0);
  paused_position_us_ = 0;
  rebase(0);
}

void VideoPlayer::set_looping(bool looping) {
  kernel::ScopedMutex guard(mutex_);
  looping_ = looping;
  if (audio_player_) audio_player_->set_looping(looping);
}

void VideoPlayer::update() {
  kernel::ScopedMutex guard(mutex_);
  if (state_ != VideoState::Playing) return;
  if (audio_player_) audio_player_->update();
  const int64_t position = position_locked();
  const uint32_t target = file_.frame_at_time(position);
  if (target != current_frame_) show_frame(target);
  if (position >= duration_us_) {
    if (looping_) {
      show_frame(0);
      rebase(0);
      if (audio_player_) {
        audio_player_->seek(0);
        audio_player_->play();
      }
      return;
    }
    state_ = VideoState::Stopped;  // the end: the last picture stays up
    paused_position_us_ = duration_us_;
    if (audio_player_) audio_player_->stop();
  }
}

bool VideoPlayer::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "video");
  return service_ != nullptr;
}

void VideoPlayer::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void VideoPlayer::service_main(void* self) {
  VideoPlayer* player = static_cast<VideoPlayer*>(self);
  while (!__atomic_load_n(&player->stop_, __ATOMIC_ACQUIRE)) {
    player->update();
    kernel::sleep_us(5000);
  }
}

}  // namespace video
