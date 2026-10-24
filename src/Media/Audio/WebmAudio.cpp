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
// The sound of a WebM / Matroska file (Opus or Vorbis, as WebM allows): the frames of the audio track are put into an Ogg file in memory - compressed
// sound is small - and the Ogg decoders, which already read, stream and seek that, do the rest.
#include "Media/Audio/OpusInternal.hpp"
#include "Media/Audio/VorbisSizer.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "Media/OggWriter.hpp"
#include "Media/Webm.hpp"
#include "Utility/ByteStream.hpp"

namespace media {

namespace {

constexpr uint32_t kSerial = 0x5443534F;
constexpr uint64_t kMaxOggBytes = 512u << 20;

uint64_t ns_to_samples(int64_t ns, uint32_t rate) { return ns <= 0 || ns > 3600000000000ll ? 0 : static_cast<uint64_t>(ns) * rate / 1000000000ull; }   // (more than an hour is not padding)

// Collects packets and writes them as pages of a few at a time, each with the position of the end of its last packet.
class PageWriter {
 public:
  PageWriter(ByteBuffer* out, allocators::IAllocator& allocator) : out_(out), data_(allocator) {}

  // `position` is the stream position (in samples) at the end of this packet.
  bool add(ConstByteView packet, int64_t position) {
    if (count_ == kMaxPackets || (count_ > 0 && data_.size() + packet.size() > kMaxBytes)) {
      if (!flush(false)) return false;
    }
    sizes_[count_] = packet.size();
    ++count_;
    last_position_ = position;
    return data_.append(packet);
  }
  // Writes what is collected; with `last` the page ends the stream (also when nothing is collected: the position then is `end`).
  bool flush(bool last, int64_t end = 0) {
    if (count_ == 0) return true;
    ConstByteView views[kMaxPackets];
    uint64_t at = 0;
    for (uint32_t i = 0; i < count_; ++i) {
      views[i] = data_.view().subview(at, sizes_[i]);
      at += sizes_[i];
    }
    const int64_t granule = last ? end : last_position_;
    const bool ok = ogg::page(out_, last ? ogg::kLast : 0, granule, kSerial, sequence_++, views, count_);
    count_ = 0;
    data_.clear();
    return ok && out_->size() <= kMaxOggBytes;
  }
  uint32_t& sequence() { return sequence_; }
  int64_t last_position() const { return last_position_; }

 private:
  static constexpr uint32_t kMaxPackets = 64;
  static constexpr uint64_t kMaxBytes = 6000;
  ByteBuffer* out_;
  ByteBuffer data_;
  uint64_t sizes_[kMaxPackets] = {};
  uint32_t count_ = 0, sequence_ = 0;
  int64_t last_position_ = 0;
};

bool write_opus(IByteSource& source, const WebmFile& file, const WebmTrack& track, allocators::IAllocator& allocator, ByteBuffer* out) {
  ConstByteView head = track.codec_private.view();
  ByteBuffer made(allocator);
  uint32_t channels = track.channels;
  uint32_t pre_skip;
  if (head.size() >= 19) {
    if (head[0] != 'O' || head[1] != 'p' || head[2] != 'u' || head[3] != 's' || head[4] != 'H' || head[5] != 'e' || head[6] != 'a' || head[7] != 'd') return false;
    if (head[18] != 0) return false;   // (a channel mapping: surround sound)
    channels = head[9];
    pre_skip = head[10] | (uint32_t{head[11]} << 8);
  } else {
    pre_skip = static_cast<uint32_t>(ns_to_samples(track.codec_delay_ns, 48000));
    const uint8_t bytes[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<uint8_t>(channels), static_cast<uint8_t>(pre_skip), static_cast<uint8_t>(pre_skip >> 8),
                             0x80, 0xBB, 0, 0, 0, 0, 0};
    if (!made.append(ConstByteView(bytes, sizeof bytes))) return false;
    head = made.view();
  }
  if (channels != 1 && channels != 2) return false;
  static const char kVendor[] = "TheoryCraftOS";
  ByteBuffer tags(allocator);
  if (!tags.append(ConstByteView(reinterpret_cast<const uint8_t*>("OpusTags"), 8)) || !ogg::put_le(&tags, sizeof kVendor - 1, 4) ||
      !tags.append(ConstByteView(reinterpret_cast<const uint8_t*>(kVendor), sizeof kVendor - 1)) || !ogg::put_le(&tags, 0, 4))
    return false;
  const ConstByteView tags_view = tags.view();
  if (!ogg::page(out, ogg::kFirst, 0, kSerial, 0, &head, 1) || !ogg::page(out, 0, 0, kSerial, 1, &tags_view, 1)) return false;
  PageWriter pages(out, allocator);
  pages.sequence() = 2;
  ByteBuffer packet(allocator);
  int64_t total = 0;   // (the positions count the samples the decoder makes, the pre-skip ones too)
  for (uint32_t i = 0; i < track.samples.count(); ++i) {
    packet.clear();
    if (!file.read_sample(source, track, i, &packet)) return false;
    OpusPacketInfo info;
    if (!parse_opus_packet(packet.view(), &info)) return false;
    total += int64_t{info.frame_count} * info.samples_per_frame;
    if (!pages.add(packet.view(), total)) return false;
  }
  // The sound the last block says to cut from the end (DiscardPadding).
  const int64_t cut = static_cast<int64_t>(ns_to_samples(track.last_discard_ns, 48000));
  int64_t end = total - cut;
  if (end < static_cast<int64_t>(pre_skip)) end = pre_skip;
  return pages.flush(true, end);
}

bool write_vorbis(IByteSource& source, const WebmFile& file, const WebmTrack& track, allocators::IAllocator& allocator, ByteBuffer* out) {
  // CodecPrivate: the count of packets less one (2), the sizes of the first two (runs of 255), then the identification, comment and setup headers.
  ByteReader r(track.codec_private.view());
  if (r.u8() != 2) return false;
  uint64_t sizes[2];
  for (uint64_t& size : sizes) {
    size = 0;
    uint8_t b;
    do {
      b = r.u8();
      size += b;
    } while (b == 255 && r.ok());
  }
  if (!r.ok()) return false;
  const ConstByteView identification = r.take(sizes[0]), comment = r.take(sizes[1]), setup = r.rest();
  if (!r.ok() || setup.empty()) return false;
  VorbisSizer sizer(allocator);
  if (!sizer.init(identification, comment, setup)) return false;
  if (!ogg::page(out, ogg::kFirst, 0, kSerial, 0, &identification, 1)) return false;
  const ConstByteView second[2] = {comment, setup};
  if (!ogg::page(out, 0, 0, kSerial, 1, second, 2)) return false;
  PageWriter pages(out, allocator);
  pages.sequence() = 2;
  ByteBuffer packet(allocator);
  int64_t total = 0;
  for (uint32_t i = 0; i < track.samples.count(); ++i) {
    packet.clear();
    if (!file.read_sample(source, track, i, &packet)) return false;
    const int64_t n = sizer.next(packet.view());
    if (n < 0) return false;
    total += n;
    if (!pages.add(packet.view(), total)) return false;
  }
  const int64_t cut = static_cast<int64_t>(ns_to_samples(track.last_discard_ns, sizer.sample_rate()));
  return pages.flush(true, cut < total ? total - cut : total);
}

// An Ogg file in memory, played through the Ogg stream readers (which read from the memory this object keeps).
class WebmAudioStream final : public audio::IPcmSource {
 public:
  explicit WebmAudioStream(allocators::IAllocator& allocator) : ogg_(allocator) {}

  bool init(IByteSource& source, allocators::IAllocator& allocator) {
    WebmFile file(allocator);
    if (!file.open(source)) return false;
    const WebmTrack* track = file.find(WebmTrackKind::Audio);
    if (!track || !webm_audio_to_ogg(source, file, *track, allocator, &ogg_)) return false;
    memory_.reset(ogg_.view());
    inner_ = track->codec == WebmCodec::Opus ? open_opus_stream(memory_, allocator) : open_vorbis_stream(memory_, allocator);
    return static_cast<bool>(inner_);
  }
  audio::StreamFormat format() const override { return inner_->format(); }
  uint64_t frame_count() const override { return inner_->frame_count(); }
  uint32_t read(uint64_t frame, ByteView out) override { return inner_->read(frame, out); }
  void playback_paused() override { inner_->playback_paused(); }

 private:
  ByteBuffer ogg_;
  MemoryByteSource memory_;
  UniquePointer<audio::IPcmSource> inner_;
};

}  // namespace

bool webm_audio_to_ogg(IByteSource& source, const WebmFile& file, const WebmTrack& track, allocators::IAllocator& allocator, ByteBuffer* out) {
  out->clear();
  if (track.kind != WebmTrackKind::Audio || !track.decodable() || track.samples.count() == 0) return false;
  const bool ok = track.codec == WebmCodec::Opus ? write_opus(source, file, track, allocator, out) : track.codec == WebmCodec::Vorbis ? write_vorbis(source, file, track, allocator, out) : false;
  if (!ok) out->clear();
  return ok;
}

UniquePointer<audio::IPcmSource> open_webm_audio_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<WebmAudioStream> stream = UniquePointer<WebmAudioStream>::make(allocator, allocator);
  if (!stream || !stream->init(source, allocator)) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

bool decode_webm_audio(ConstByteView data, AudioData* out) {
  out->clear();
  allocators::IAllocator& allocator = out->allocator();
  MemoryByteSource source(data);
  WebmFile file(allocator);
  if (!file.open(source)) return false;
  const WebmTrack* track = file.find(WebmTrackKind::Audio);
  ByteBuffer ogg(allocator);
  if (!track || !webm_audio_to_ogg(source, file, *track, allocator, &ogg)) return false;
  return track->codec == WebmCodec::Opus ? decode_opus(ogg.view(), out) : decode_vorbis(ogg.view(), out);
}

}  // namespace media
