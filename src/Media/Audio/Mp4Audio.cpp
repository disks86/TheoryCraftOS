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

// Audio out of an MP4/M4A file: AAC-LC (decoded block by block), or MP3 (the samples are frames of one MPEG audio stream).
#include "Media/Audio/AacInternal.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "Media/Mp4.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {
// Copies the part of `whole` that the track's edit list plays (its start offset and length) into `out`.
bool trim_to_edit(const AudioData& whole, const Mp4Track& t, AudioData* out) {
  const uint32_t rate = whole.format().sample_rate, channels = whole.format().channels;
  uint64_t frames = whole.frame_count(), skip = 0;
  if (t.skip > 0) skip = static_cast<uint64_t>(t.skip) * rate / t.timescale;
  if (t.play_duration >= 0) {
    const uint64_t keep = static_cast<uint64_t>(t.play_duration) * rate / t.timescale;
    if (skip + keep < frames) frames = skip + keep;
  }
  if (skip >= frames) skip = 0;
  frames -= skip;
  if (frames == 0 || !out->create(whole.format(), frames)) return false;
  __builtin_memcpy(out->samples().data(), whole.samples().data() + skip * channels, frames * channels * 2);
  return true;
}
}  // namespace

bool decode_mp4_audio(ConstByteView data, AudioData* out) {
  out->clear();
  allocators::IAllocator& allocator = out->allocator();
  Mp4File file(allocator);
  if (!file.open(data)) return false;
  const Mp4Track* t = file.find(Mp4TrackKind::Audio);
  if (!t || t->sample_count == 0) return false;
  const uint32_t mp4a = 0x6d703461;
  if (t->codec == mp4a && (t->object_type == 0x6B || t->object_type == 0x69)) {  // MPEG-1/2 audio: Layer III frames in the samples
    ByteBuffer stream(allocator);
    for (uint32_t i = 0; i < t->sample_count; ++i)
      if (!stream.append(file.sample(*t, i))) return false;
    AudioData whole(allocator);
    if (!decode_mp3(stream.view(), &whole)) return false;
    return trim_to_edit(whole, *t, out);
  }
  if (t->codec != mp4a || (t->object_type != 0x40 && t->object_type != 0x66 && t->object_type != 0x67 && t->object_type != 0x68)) return false;
  uint32_t aot, index, channel_config;
  if (!parse_audio_specific_config(t->config, &aot, &index, &channel_config)) return false;  // AAC-LC only
  AacDecoder decoder(allocator);
  if (!decoder.init(index)) return false;
  ByteBuffer pcm(allocator);
  uint32_t channels = 0;
  int16_t block[2048];
  for (uint32_t i = 0; i < t->sample_count; ++i) {
    if (!decoder.decode_block(file.sample(*t, i), block)) {
      if (channels == 0) return false;
      for (int k = 0; k < 2048; ++k) block[k] = 0;  // a block that cannot be decoded is a gap
    }
    if (channels == 0) channels = decoder.channels();
    if (!pcm.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels * 2))) return false;
  }
  // The edit list drops the encoder's priming samples and the padding at the end.
  const uint32_t rate = decoder.sample_rate();
  uint64_t frames = pcm.size() / (channels * 2), skip = 0;
  if (t->skip > 0) skip = static_cast<uint64_t>(t->skip) * rate / t->timescale;
  if (t->play_duration >= 0) {
    const uint64_t keep = static_cast<uint64_t>(t->play_duration) * rate / t->timescale;
    if (skip + keep < frames) frames = skip + keep;
  }
  if (skip >= frames) skip = 0;
  frames -= skip;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  if (frames == 0 || !out->create(format, frames)) return false;
  __builtin_memcpy(out->samples().data(), pcm.view().data() + skip * channels * 2, frames * channels * 2);
  return true;
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// Streaming: only the movie box is read up front; the samples are read from the file as they are played.

namespace {

class Mp4AudioStream final : public StreamingPcmSource {
 public:
  static constexpr uint32_t kPrime = 8;   // MP3 frames decoded before the first one wanted after a jump

  Mp4AudioStream(IByteSource& source, allocators::IAllocator& allocator) : source_(source), allocator_(allocator), file_(allocator), packed_(allocator), sound_(allocator), pcm_(allocator), decoder_(allocator) {}

  bool init() {
    if (!file_.open(source_)) return false;
    track_ = file_.find(Mp4TrackKind::Audio);
    if (!track_ || track_->sample_count == 0) return false;
    const uint32_t mp4a = 0x6d703461;
    if (track_->codec != mp4a) return false;
    uint64_t total = 0;
    if (track_->object_type == 0x6B || track_->object_type == 0x69) {   // MPEG-1/2 audio: Layer III frames in the samples
      mp3_ = true;
      if (!decode_window(0, track_->sample_count < 4 ? track_->sample_count : 4)) return false;
      per_sample_ = sound_.frame_count() / (track_->sample_count < 4 ? track_->sample_count : 4);
      rate_ = sound_.format().sample_rate;
      channels_ = sound_.format().channels;
      if (per_sample_ == 0) return false;
      total = uint64_t{track_->sample_count} * per_sample_;
    } else {
      if (track_->object_type != 0x40 && track_->object_type != 0x66 && track_->object_type != 0x67 && track_->object_type != 0x68) return false;
      uint32_t aot, index, channel_config;
      if (!parse_audio_specific_config(track_->config, &aot, &index, &channel_config)) return false;   // AAC-LC only
      if (!decoder_.init(index)) return false;
      rate_ = decoder_.sample_rate();
      per_sample_ = 1024;
      int16_t block[2048];
      packed_.clear();
      if (!file_.read_sample(source_, *track_, 0, &packed_) || !decoder_.decode_block(packed_.view(), block)) return false;
      channels_ = decoder_.channels();
      total = uint64_t{track_->sample_count} * 1024;
      decoder_.reset();
    }
    // The edit list drops the encoder's priming samples and the padding at the end.
    uint64_t frames = total;
    if (track_->skip > 0) skip_ = static_cast<uint64_t>(track_->skip) * rate_ / track_->timescale;
    if (track_->play_duration >= 0) {
      const uint64_t keep = static_cast<uint64_t>(track_->play_duration) * rate_ / track_->timescale;
      if (skip_ + keep < frames) frames = skip_ + keep;
    }
    if (skip_ >= frames) skip_ = 0;
    frames_ = frames - skip_;
    format_.sample_rate = rate_;
    format_.channels = static_cast<uint8_t>(channels_);
    format_.type = audio::SampleType::S16;
    next_ = 0;
    return frames_ > 0 && channels_ >= 1 && channels_ <= 2 && format_.valid();
  }

 protected:
  bool refill(uint64_t frame) override {
    const uint64_t raw = frame + skip_, k = raw / per_sample_;
    const uint32_t count = track_->sample_count;
    uint32_t last = static_cast<uint32_t>(k + 4 < count ? k + 4 : count);   // (samples k .. last - 1)
    pcm_.clear();
    uint32_t from;
    if (mp3_) {
      from = static_cast<uint32_t>(k > kPrime ? k - kPrime : 0);
      if (!decode_window(from, last)) return false;
      pcm_.append(ConstByteView(reinterpret_cast<const uint8_t*>(sound_.samples().data()), sound_.samples().size() * 2));
    } else {
      // AAC: the transform needs the sample before; after a jump start two samples early (or at the beginning).
      if (k != next_) {
        decoder_.reset();
        next_ = k > 2 ? k - 2 : 0;
      }
      int16_t block[2048];
      while (next_ < k) {
        packed_.clear();
        if (!file_.read_sample(source_, *track_, static_cast<uint32_t>(next_), &packed_)) return false;
        decoder_.decode_block(packed_.view(), block);   // (only to bring the decoder up to date)
        ++next_;
      }
      for (uint32_t i = static_cast<uint32_t>(k); i < last; ++i) {
        packed_.clear();
        if (!file_.read_sample(source_, *track_, i, &packed_)) return false;
        if (!decoder_.decode_block(packed_.view(), block))
          for (int z = 0; z < 2048; ++z) block[z] = 0;   // a block that cannot be decoded is a gap
        if (!pcm_.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels_ * 2))) return false;
        ++next_;
      }
      from = static_cast<uint32_t>(k);
    }
    const uint64_t frame_size = format_.frame_size();
    const uint64_t window_raw_first = (mp3_ ? uint64_t{from} : k) * per_sample_;
    uint64_t lead = mp3_ ? (k - from) * per_sample_ : 0;   // (priming frames, not wanted)
    uint64_t first_raw = window_raw_first + lead;
    uint64_t have = pcm_.size() / frame_size;
    if (lead >= have) return false;
    have -= lead;
    uint64_t drop = skip_ > first_raw ? skip_ - first_raw : 0;   // before the edit list begins
    if (drop >= have) return false;
    const uint64_t first = first_raw + drop - skip_;
    have -= drop;
    if (first + have > frames_) have = first < frames_ ? frames_ - first : 0;
    if (have == 0) return false;
    set_window(first, pcm_.view().subview((lead + drop) * frame_size, have * frame_size));
    return true;
  }

 private:
  // MP3 in MP4: the samples [from, to) are frames of one stream: joined and decoded as one.
  bool decode_window(uint32_t from, uint32_t to) {
    packed_.clear();
    for (uint32_t i = from; i < to; ++i)
      if (!file_.read_sample(source_, *track_, i, &packed_)) return false;
    return decode_mp3_frames(packed_.view(), &sound_) && sound_.frame_count() > 0;
  }

  IByteSource& source_;
  allocators::IAllocator& allocator_;
  Mp4File file_;
  const Mp4Track* track_ = nullptr;
  ByteBuffer packed_;
  AudioData sound_;
  ByteBuffer pcm_;
  AacDecoder decoder_;
  bool mp3_ = false;
  uint32_t rate_ = 0, channels_ = 0;
  uint64_t per_sample_ = 0, skip_ = 0, next_ = 0;
};

}  // namespace

UniquePointer<audio::IPcmSource> open_mp4_audio_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<Mp4AudioStream> stream = UniquePointer<Mp4AudioStream>::make(allocator, source, allocator);
  if (!stream || !stream->init()) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

}  // namespace media
