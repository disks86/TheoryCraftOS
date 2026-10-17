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
#include "Media/AudioCodecs.hpp"
#include "Media/ImageCodecs.hpp"
#include "Media/VideoFiles.hpp"
#include "Utility/ByteBuffer.hpp"

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
  decoded_audio_.clear();
  images_.clear();
  frame_starts_.clear();
  image_mode_ = false;
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

  open_audio();
  state_ = VideoState::Stopped;
  return OpenResult::Ok;
}

// The sound of an AVI file: all its audio chunks gathered into one run (decoded first, for the compressed kinds), played by
// an AudioPlayer. Without a mixer, or if anything fails, the video plays silently.
void VideoPlayer::open_audio() {
  const AviAudioInfo& info = file_.audio();
  if (!mixer_ || !info.present || info.bytes == 0) return;
  audio::IPcmSource* source = nullptr;
  if (info.tag == 1) {
    audio_samples_ = allocate_view<uint8_t>(allocator_, info.bytes);
    if (audio_samples_.empty()) return;
    const uint64_t bytes = file_.copy_audio(audio_samples_);
    audio_source_ = UniquePointer<audio::MemoryPcmSource>::make(allocator_, ConstByteView(audio_samples_).first(bytes), info.format);
    source = audio_source_.get();
  } else {
    // Wrap the compressed bytes in a WAV file (the stream's own format header, then the data) and let the WAV decoder do it.
    ByteView packed = allocate_view<uint8_t>(allocator_, info.bytes);
    if (packed.empty()) return;
    const uint64_t got = file_.copy_audio(packed);
    ByteBuffer wav(allocator_);
    const bool built = wav.put_tag("RIFF") && wav.put32(static_cast<uint32_t>(4 + 8 + info.wave_format.size() + 8 + got)) && wav.put_tag("WAVE") &&
                       wav.put_tag("fmt ") && wav.put32(static_cast<uint32_t>(info.wave_format.size())) && wav.append(info.wave_format) &&
                       ((info.wave_format.size() & 1) == 0 || wav.put8(0)) && wav.put_tag("data") && wav.put32(static_cast<uint32_t>(got)) &&
                       wav.append(ConstByteView(packed).first(got));
    release_view(allocator_, packed);
    if (!built || !media::decode_wav(wav.view(), &decoded_audio_)) return;
    source = decoded_audio_.source();
  }
  audio_player_ = UniquePointer<audio::AudioPlayer>::make(allocator_, *mixer_, allocator_);
  if (!source || !audio_player_ || !audio_player_->open(source)) {
    audio_player_.reset();
    audio_source_.reset();
    decoded_audio_.clear();
    release_view(allocator_, audio_samples_);  // play without sound
  }
}

// Decodes frame `index` into the picture. False if the decoder gave nothing (the old picture stays).
bool VideoPlayer::decode_frame(uint32_t index) {
  if (image_mode_) {
    ArrayView<uint32_t> pixels = images_.frame(index);
    if (pixels.empty()) return false;
    __builtin_memcpy(display_.plane(0).data(), pixels.data(), pixels.size() * 4);
    current_frame_ = index;
    ++decoded_;
    ++serial_;
    return true;
  }
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
  if (all_keyframes()) {
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

uint32_t VideoPlayer::frame_at_time(int64_t time_us) const {
  if (!image_mode_) return file_.frame_at_time(time_us);
  const uint64_t n = images_.frame_count();
  if (n == 0 || time_us <= 0) return 0;
  uint64_t lo = 0, hi = n - 1;  // the last frame that starts at or before time_us
  while (lo < hi) {
    const uint64_t mid = (lo + hi + 1) / 2;
    if (frame_starts_[mid] <= time_us) lo = mid;
    else hi = mid - 1;
  }
  return static_cast<uint32_t>(lo);
}

// Takes the Y4M/GIF/APNG frames in images_ as the video: a picture surface, and when each frame starts.
OpenResult VideoPlayer::open_images() {
  if (images_.empty() || !display_.allocate({images_.width(), images_.height(), PixelFormat::Bgra8888})) return OpenResult::OutOfMemory;
  int64_t at = 0;
  for (uint32_t i = 0; i < images_.frame_count(); ++i) {
    if (!frame_starts_.push_back(at)) return OpenResult::OutOfMemory;
    const uint32_t delay = images_.delay_ms(i);
    at += int64_t{delay ? delay : 1} * 1000;
  }
  if (!frame_starts_.push_back(at)) return OpenResult::OutOfMemory;
  image_mode_ = true;
  width_ = images_.width();
  height_ = images_.height();
  surface_.pixels = ArrayView<uint32_t>(reinterpret_cast<uint32_t*>(display_.plane(0).data()), uint64_t{width_} * height_);
  surface_.width = width_;
  surface_.height = height_;
  surface_.pitch = width_;
  duration_us_ = at;
  current_frame_ = 0;
  if (!decode_frame(0)) return OpenResult::OutOfMemory;
  have_picture_ = true;
  state_ = VideoState::Stopped;
  return OpenResult::Ok;
}

OpenResult VideoPlayer::open_media(ConstByteView data) {
  const media::VideoFileFormat format = media::detect_video_file(data);
  if (format == media::VideoFileFormat::Avi) return open(data);
  kernel::ScopedMutex guard(mutex_);
  release();
  // GIF, APNG and Y4M are videos; any other picture format opens as a video of one frame.
  if (format == media::VideoFileFormat::Unknown && media::detect_image_format(data) == media::ImageFormat::Unknown) return OpenResult::Unsupported;  // (GIF, APNG, animated WebP and Y4M are videos)
  if (!(format == media::VideoFileFormat::Y4m ? media::decode_y4m(data, &images_) : media::decode_image(data, &images_))) return OpenResult::Unsupported;
  const OpenResult result = open_images();
  if (result != OpenResult::Ok) release();
  return result;
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
  show_frame(frame_at_time(time_us));
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
  const uint32_t target = frame_at_time(position);
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
