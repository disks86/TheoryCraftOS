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
#include "Media/AudioStreaming.hpp"
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
  audio_player_.reset();  // before the sound it plays
  audio_stream_.reset();
  decoder_.reset();
  seq_clear();
  mp4_.clear();
  mp4_track_ = nullptr;
  source_ = nullptr;
  packet_.clear();
  images_.clear();
  frame_starts_.clear();
  image_mode_ = false;
  y4m_mode_ = false;
  y4m_frames_ = 0;
  y4m_head_.clear();
  y4m_frame_.clear();
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
  return finish_avi();
}

// The rest of opening an AVI file whose headers and tables file_ has read.
OpenResult VideoPlayer::finish_avi() {
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
  if (info.codec == VideoCodec::H264 && !seq_build_avi()) {
    release();
    return OpenResult::OutOfMemory;
  }

  // The first picture tells the real size (the stream's headers win over the container's).
  width_ = info.width;
  height_ = info.height;
  duration_us_ = seq_mode() ? seq_times_[seq_times_.count() - 1] : file_.duration_us();
  current_frame_ = 0;
  if (!(seq_mode() ? seq_show(0) : decode_frame(0))) {
    release();
    return OpenResult::NoDecoder;
  }
  have_picture_ = true;

  open_audio();
  state_ = VideoState::Stopped;
  return OpenResult::Ok;
}

// The sound of an AVI file: its audio chunks, read and decoded as they play, through an AudioPlayer. Without a mixer, or if anything fails,
// the video plays silently.
void VideoPlayer::open_audio() {
  const AviAudioInfo& info = file_.audio();
  if (!mixer_ || !info.present || info.bytes == 0) return;
  audio_stream_ = media::open_wave_data_stream(file_.audio_source(), info.wave_format, allocator_);
  if (audio_stream_) start_audio(audio_stream_.get());
}

void VideoPlayer::start_audio(audio::IPcmSource* source) {
  audio_player_ = UniquePointer<audio::AudioPlayer>::make(allocator_, *mixer_, allocator_);
  if (!source || !audio_player_ || !audio_player_->open(source)) {   // play without sound
    audio_player_.reset();
    audio_stream_.reset();
  }
}

// Decodes frame `index` into the picture. False if the decoder gave nothing (the old picture stays).
bool VideoPlayer::decode_frame(uint32_t index) {
  if (y4m_mode_) {   // read the frame, wrap it in a one-frame Y4M file and let the Y4M decoder make the picture
    if (index >= y4m_frames_ || !source_) return false;
    packet_.clear();
    if (!packet_.append(y4m_head_.view()) || !packet_.put_tag("FRAM") || !packet_.put16('E' | ('\n' << 8))) return false;
    const uint64_t at = packet_.size();
    if (!packet_.put_zeros(y4m_frame_bytes_)) return false;
    if (source_->read(y4m_first_ + uint64_t{index} * y4m_step_, ByteView(packet_.writable_view().data() + at, y4m_frame_bytes_)) != y4m_frame_bytes_) return false;
    if (!media::decode_y4m(packet_.view(), &y4m_frame_) || y4m_frame_.frame_count() != 1) return false;
    ArrayView<uint32_t> pixels = y4m_frame_.frame(0);
    if (pixels.size() * 4 != display_.plane(0).size()) return false;
    __builtin_memcpy(display_.plane(0).data(), pixels.data(), pixels.size() * 4);
    current_frame_ = index;
    ++decoded_;
    ++serial_;
    return true;
  }
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
  if (!show_decoded(frame)) return false;
  current_frame_ = index;
  return true;
}

// Makes a decoded picture the one on show.
bool VideoPlayer::show_decoded(const VideoFrame& frame) {
  const VideoFormat& f = frame.format;
  if (f.width != width_ || f.height != height_ || !display_.valid() || display_.format().width != f.width || display_.format().height != f.height) {
    if (!display_.allocate({f.width, f.height, PixelFormat::Bgra8888})) return false;
    width_ = f.width;
    height_ = f.height;
    surface_.pixels = ArrayView<uint32_t>(reinterpret_cast<uint32_t*>(display_.plane(0).data()), uint64_t{f.width} * f.height);
    surface_.width = f.width;
    surface_.height = f.height;
    surface_.pitch = f.width;
  }
  if (!convert_to_bgra(frame, reinterpret_cast<uint32_t*>(display_.plane(0).data()), width_)) return false;
  ++serial_;
  return true;
}

// Brings the picture to frame `target`. A codec whose pictures all stand alone jumps there; one that builds on earlier
// pictures decodes every frame since the last keyframe it can reach.
void VideoPlayer::show_frame(uint32_t target) {
  if (seq_mode()) {
    seq_show(target);
    return;
  }
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
  if (y4m_mode_) {
    if (y4m_frames_ == 0 || time_us <= 0) return 0;
    uint64_t i = static_cast<uint64_t>(time_us) * y4m_rate_num_ / (1000000 * y4m_rate_den_);
    if (i >= y4m_frames_) i = y4m_frames_ - 1;
    while (i + 1 < y4m_frames_ && y4m_start_us(i + 1) <= time_us) ++i;
    while (i > 0 && y4m_start_us(i) > time_us) --i;
    return static_cast<uint32_t>(i);
  }
  if (!image_mode_ && !seq_mode()) return file_.frame_at_time(time_us);
  const List<int64_t>& starts = image_mode_ ? frame_starts_ : seq_times_;
  const uint64_t n = image_mode_ ? images_.frame_count() : seq_dec_of_rank_.count();
  if (n == 0 || time_us <= 0) return 0;
  uint64_t lo = 0, hi = n - 1;  // the last frame that starts at or before time_us
  while (lo < hi) {
    const uint64_t mid = (lo + hi + 1) / 2;
    if (starts[mid] <= time_us) lo = mid;
    else hi = mid - 1;
  }
  return static_cast<uint32_t>(lo);
}

// ---------------------------------------------------------------------------------------------------------------
// Streams with picture reordering (H.264)

void VideoPlayer::seq_clear() {
  seq_source_ = SeqSource::None;
  seq_dec_of_rank_.clear();
  seq_rank_of_dec_.clear();
  seq_times_.clear();
  seq_sync_.clear();
  seq_fed_ = seq_next_rank_ = 0;
  seq_started_ = false;
}

bool VideoPlayer::seq_packet(uint32_t decode_index, VideoPacket* packet) const {
  if (seq_source_ == SeqSource::Avi) return file_.frame(decode_index, packet);
  if (seq_source_ != SeqSource::Mp4 || !mp4_track_ || !source_) return false;
  packet_.clear();
  if (!mp4_.read_sample(*source_, *mp4_track_, decode_index, &packet_) || packet_.size() == 0) return false;
  packet->data = packet_.view();
  packet->pts_us = 0;
  packet->keyframe = seq_sync_[decode_index] != 0;
  return true;
}

// An AVI file's frames are stored in display order already (the codec holds pictures back by itself).
bool VideoPlayer::seq_build_avi() {
  const uint32_t n = file_.frame_count();
  if (n == 0) return false;
  for (uint32_t i = 0; i < n; ++i) {
    if (!seq_dec_of_rank_.push_back(i) || !seq_rank_of_dec_.push_back(i) || !seq_times_.push_back(file_.frame_time_us(i)) ||
        !seq_sync_.push_back(file_.keyframe_at_or_before(i) == i ? 1 : 0))
      return false;
  }
  if (!seq_times_.push_back(file_.frame_time_us(n))) return false;
  seq_source_ = SeqSource::Avi;
  return true;
}

// An MP4 track: samples are in decode order; their composition times give the display order.
bool VideoPlayer::seq_build_mp4(const media::Mp4Track& track) {
  const uint32_t n = track.sample_count;
  if (n == 0 || track.timescale == 0) return false;
  List<int64_t> cts(allocator_);
  List<uint32_t> scratch(allocator_);
  if (!cts.reserve(n) || !scratch.reserve(n) || !seq_dec_of_rank_.reserve(n) || !seq_rank_of_dec_.reserve(n) || !seq_sync_.reserve(n) ||
      !seq_times_.reserve(n + 1))
    return false;
  for (uint32_t i = 0; i < n; ++i) {
    const int64_t t = track.dts[i] + (track.cts_offsets.empty() ? 0 : track.cts_offsets[i]);
    if (!cts.push_back(t) || !seq_dec_of_rank_.push_back(i) || !scratch.push_back(i) || !seq_rank_of_dec_.push_back(0) ||
        !seq_sync_.push_back(track.sync.empty() || track.sync[i] ? 1 : 0))
      return false;
  }
  // Stable merge sort of the decode indices by composition time.
  uint32_t* a = &seq_dec_of_rank_[0];
  uint32_t* b = &scratch[0];
  for (uint64_t width = 1; width < n; width *= 2) {
    for (uint64_t lo = 0; lo < n; lo += 2 * width) {
      const uint64_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
      uint64_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) b[k++] = cts[a[j]] < cts[a[i]] ? a[j++] : a[i++];
      while (i < mid) b[k++] = a[i++];
      while (j < hi) b[k++] = a[j++];
    }
    uint32_t* t = a;
    a = b;
    b = t;
  }
  if (a != &seq_dec_of_rank_[0])
    for (uint32_t i = 0; i < n; ++i) seq_dec_of_rank_[i] = a[i];
  const int64_t scale = track.timescale;
  int64_t previous = 0;
  for (uint32_t r = 0; r < n; ++r) {
    const uint32_t d = seq_dec_of_rank_[r];
    seq_rank_of_dec_[d] = r;
    int64_t t = (cts[d] - track.skip) * 1000000 / scale;
    if (t < previous) t = previous;
    previous = t;
    if (!seq_times_.push_back(t)) return false;
  }
  int64_t last = n >= 2 ? (track.dts[n - 1] - track.dts[n - 2]) * 1000000 / scale : 33333;
  if (last <= 0) last = 33333;
  int64_t end = seq_times_[n - 1] + last;
  if (track.play_duration >= 0 && track.play_duration * 1000000 / scale < end && track.play_duration * 1000000 / scale > seq_times_[n - 1])
    end = track.play_duration * 1000000 / scale;
  if (!seq_times_.push_back(end)) return false;
  seq_source_ = SeqSource::Mp4;
  mp4_track_ = &track;
  return true;
}

// Brings the picture to display position `target`: continues decoding where the decoder is, or (after going back or
// a long jump forward) restarts from the sync sample at or before it.
bool VideoPlayer::seq_show(uint32_t target) {
  const uint32_t n = static_cast<uint32_t>(seq_dec_of_rank_.count());
  if (n == 0 || !decoder_) return false;
  if (target >= n) target = n - 1;
  if (have_picture_ && seq_started_ && target == current_frame_) return true;
  auto sync_rank_at_or_before = [&](uint32_t rank) {
    while (rank > 0 && !seq_sync_[seq_dec_of_rank_[rank]]) --rank;
    return rank;
  };
  bool restart = !seq_started_ || target < seq_next_rank_;
  if (!restart && target - seq_next_rank_ > kMaxCatchUpFrames) {
    if (sync_rank_at_or_before(target) > seq_next_rank_) restart = true;
  }
  if (restart) {
    const uint32_t rank = sync_rank_at_or_before(target);
    decoder_->reset();
    seq_fed_ = seq_dec_of_rank_[rank];
    seq_next_rank_ = rank;
    seq_started_ = true;
  }
  VideoFrame frame;
  bool shown = false;
  while (seq_next_rank_ <= target) {
    if (!decoder_->receive(&frame)) {
      if (seq_fed_ >= n) {
        decoder_->flush();
        if (!decoder_->receive(&frame)) break;
      } else {
        VideoPacket packet;
        const uint32_t d = seq_fed_++;
        if (!seq_packet(d, &packet)) continue;
        if (decoder_->submit(packet) == SubmitResult::Full) break;
        continue;
      }
    }
    ++decoded_;
    if (seq_next_rank_ == target) shown = show_decoded(frame);
    else if (seq_next_rank_ > current_frame_ || !have_picture_) ++skipped_;
    ++seq_next_rank_;
  }
  current_frame_ = target;  // if no picture came, the old one stays up
  return shown;
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
  if (format == media::VideoFileFormat::Mp4) {
    memory_.reset(data);
    return open_mp4(memory_);
  }
  kernel::ScopedMutex guard(mutex_);
  release();
  // GIF, APNG and Y4M are videos; any other picture format opens as a video of one frame.
  if (format == media::VideoFileFormat::Unknown && media::detect_image_format(data) == media::ImageFormat::Unknown) return OpenResult::Unsupported;  // (GIF, APNG, animated WebP and Y4M are videos)
  if (!(format == media::VideoFileFormat::Y4m ? media::decode_y4m(data, &images_) : media::decode_image(data, &images_))) return OpenResult::Unsupported;
  const OpenResult result = open_images();
  if (result != OpenResult::Ok) release();
  return result;
}

OpenResult VideoPlayer::open_media(media::IByteSource& source) {
  uint8_t head[16] = {};
  const uint64_t got = source.read(0, ByteView(head));
  const media::VideoFileFormat format = media::detect_video_file(ConstByteView(head, got));
  if (format == media::VideoFileFormat::Avi) {
    kernel::ScopedMutex guard(mutex_);
    release();
    if (!file_.open(source)) return OpenResult::NotAvi;
    source_ = &source;
    const OpenResult result = finish_avi();
    if (result != OpenResult::Ok) source_ = nullptr;
    return result;
  }
  if (format == media::VideoFileFormat::Mp4) return open_mp4(source);
  if (format == media::VideoFileFormat::Y4m) {
    OpenResult result = OpenResult::Unsupported;
    if (open_y4m_stream(source, &result)) return result;   // (else it is read whole, below)
  }
  // The rest are decoded up front: the file is read whole, decoded, and let go. (Only pictures, though: anything else is not read at all.)
  uint8_t more[64] = {};
  const uint64_t more_got = source.read(0, ByteView(more));
  if (format == media::VideoFileFormat::Unknown && media::detect_image_format(ConstByteView(more, more_got)) == media::ImageFormat::Unknown) return OpenResult::Unsupported;
  if (source.size() == 0 || source.size() > kMaxInMemoryBytes) return OpenResult::Unsupported;
  ByteView whole = allocate_view<uint8_t>(allocator_, source.size());
  if (whole.empty()) return OpenResult::OutOfMemory;
  OpenResult result = OpenResult::Unsupported;
  if (source.read(0, whole) == whole.size()) result = open_media(ConstByteView(whole));
  release_view(allocator_, whole);
  return result;
}

// A Y4M file whose frames are all the same size, read a frame at a time. False (nothing opened) if it is not laid out that way - frames with
// parameters, a chroma format the decoder lacks - and the file should be read whole instead.
bool VideoPlayer::open_y4m_stream(media::IByteSource& source, OpenResult* result) {
  uint8_t line[1024];
  const uint64_t got = source.read(0, ByteView(line));
  uint64_t end = 0;
  while (end < got && line[end] != '\n') ++end;
  if (end >= got) return false;
  uint64_t width = 0, height = 0, num = 25, den = 1;
  enum { C420, C422, C444, Mono } chroma = C420;
  auto number = [&](uint64_t* at, uint64_t* value) {
    uint64_t v = 0, digits = 0;
    while (*at < end && line[*at] >= '0' && line[*at] <= '9' && digits < 9) v = v * 10 + (line[(*at)++] - '0'), ++digits;
    *value = v;
    return digits > 0;
  };
  for (uint64_t at = 9; at < end;) {
    if (line[at] != ' ') return false;
    ++at;
    if (at >= end) return false;
    const uint8_t key = line[at++];
    if (key == 'W') {
      if (!number(&at, &width)) return false;
    } else if (key == 'H') {
      if (!number(&at, &height)) return false;
    } else if (key == 'F') {
      if (!number(&at, &num) || at >= end || line[at++] != ':' || !number(&at, &den)) return false;
    } else if (key == 'I') {
      if (at >= end || (line[at] != 'p' && line[at] != '?')) return false;
      ++at;
    } else {
      const uint64_t start = at;
      while (at < end && line[at] != ' ') ++at;
      if (key == 'C') {
        const uint64_t n = at - start;
        if (n >= 3 && line[start] == '4' && line[start + 1] == '2' && line[start + 2] == '0') chroma = C420;
        else if (n >= 3 && line[start] == '4' && line[start + 1] == '2' && line[start + 2] == '2') chroma = C422;
        else if (n == 3 && line[start] == '4' && line[start + 1] == '4' && line[start + 2] == '4') chroma = C444;
        else if (n == 4 && line[start] == 'm' && line[start + 1] == 'o') chroma = Mono;
        else return false;
      }
    }
  }
  if (width == 0 || height == 0 || width > 65535 || height > 65535 || num == 0 || den == 0) return false;
  const uint64_t cw = chroma == C444 ? width : (width + 1) / 2;
  const uint64_t ch = chroma == C420 ? (height + 1) / 2 : height;
  const uint64_t data_bytes = chroma == Mono ? width * height : width * height + 2 * cw * ch;
  const uint64_t first = end + 1, step = 6 + data_bytes;
  if (source.size() < first + step) return false;
  const uint64_t frames = (source.size() - first) / step;
  // Every frame must begin with a plain "FRAME" line: look at the second and the last.
  const uint64_t probes[3] = {0, 1, frames - 1};
  for (uint64_t f : probes) {
    uint8_t mark[6];
    if (f >= frames) continue;
    if (source.read(first + f * step, ByteView(mark)) != 6 || __builtin_memcmp(mark, "FRAME\n", 6) != 0) return false;
  }
  kernel::ScopedMutex guard(mutex_);
  release();
  if (!y4m_head_.append(ConstByteView(line, end + 1))) return false;
  y4m_first_ = first + 6;   // (frames are read from their data, behind the "FRAME" line: stepping by `step`)
  y4m_frame_bytes_ = data_bytes;
  y4m_step_ = step;
  y4m_frames_ = frames;
  y4m_rate_num_ = num;
  y4m_rate_den_ = den;
  source_ = &source;
  if (!display_.allocate({static_cast<uint32_t>(width), static_cast<uint32_t>(height), PixelFormat::Bgra8888})) {
    release();
    *result = OpenResult::OutOfMemory;
    return true;
  }
  y4m_mode_ = true;
  width_ = static_cast<uint32_t>(width);
  height_ = static_cast<uint32_t>(height);
  surface_.pixels = ArrayView<uint32_t>(reinterpret_cast<uint32_t*>(display_.plane(0).data()), width * height);
  surface_.width = width_;
  surface_.height = height_;
  surface_.pitch = width_;
  duration_us_ = y4m_start_us(frames);
  current_frame_ = 0;
  if (!decode_frame(0)) {
    release();
    *result = OpenResult::NoDecoder;
    return true;
  }
  have_picture_ = true;
  state_ = VideoState::Stopped;
  *result = OpenResult::Ok;
  return true;
}

// An MP4 / MOV file: its H.264 or VP9 video track read sample by sample, and its audio track (AAC or MP3) decoded as it plays.
OpenResult VideoPlayer::open_mp4(media::IByteSource& source) {
  kernel::ScopedMutex guard(mutex_);
  release();
  if (!mp4_.open(source)) return OpenResult::Unsupported;
  source_ = &source;
  const media::Mp4Track* track = mp4_.find(media::Mp4TrackKind::Video);
  if (!track) {
    release();
    return OpenResult::Unsupported;
  }
  const uint32_t avc1 = 0x61766331, avc3 = 0x61766333, vp09 = 0x76703039;
  const bool vp9 = track->codec == vp09;   // (VP9 streams describe themselves: no configuration record is needed)
  if (!vp9 && ((track->codec != avc1 && track->codec != avc3) || track->config.empty())) {
    release();
    return OpenResult::NoDecoder;
  }
  DecoderConfig config;
  config.codec = vp9 ? VideoCodec::Vp9 : VideoCodec::H264;
  config.width = track->width;
  config.height = track->height;
  config.output = PixelFormat::Bgra8888;
  config.codec_config = track->config;
  decoder_ = codecs_.create_decoder(config);
  if (!decoder_) {
    release();
    return OpenResult::NoDecoder;
  }
  if (!seq_build_mp4(*track)) {
    release();
    return OpenResult::OutOfMemory;
  }
  width_ = track->width;
  height_ = track->height;
  duration_us_ = seq_times_[seq_times_.count() - 1];
  current_frame_ = 0;
  if (!seq_show(0)) {
    release();
    return OpenResult::NoDecoder;
  }
  have_picture_ = true;
  if (mixer_) {
    audio_stream_ = media::open_mp4_audio_stream(source, allocator_);
    if (audio_stream_) start_audio(audio_stream_.get());
  }
  state_ = VideoState::Stopped;
  return OpenResult::Ok;
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
