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

// Ogg Opus files (RFC 7845): the OpusHead / OpusTags headers, packets from Ogg pages, pre-skip and end trimming from the granule
// positions, the header's output gain. One stream with channel mapping family 0 (mono or stereo). Decoding is to 48 kHz 16-bit, which is
// what Opus decodes to. The streaming source indexes the pages and decodes a page at a time; a seek restarts a few pages (about 80 ms,
// the time Opus needs to converge) before the place wanted.
#include "Math/FloatMath.hpp"
#include "Media/Audio/OpusInternal.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint32_t kPreRoll = 3840;  // 80 ms at 48 kHz

uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }

struct OggPage {
  uint64_t offset = 0;
  uint32_t header_size = 0, body_size = 0;
  int64_t granule = -1;
  uint32_t serial = 0;
  uint8_t flags = 0;
  uint8_t segments = 0;
  uint8_t lacing[255] = {};
  uint64_t size() const { return header_size + body_size; }
};

// Reads the page header at `pos`. False at the end of the data or if there is no page there.
bool read_page_header(IByteSource& source, uint64_t pos, OggPage* page) {
  uint8_t head[27];
  if (pos > source.size() || source.size() - pos < 27 || !read_exact(source, pos, ByteView(head, 27))) return false;
  if (head[0] != 'O' || head[1] != 'g' || head[2] != 'g' || head[3] != 'S' || head[4] != 0) return false;
  page->offset = pos;
  page->flags = head[5];
  const uint64_t g = le32(head + 6) | (uint64_t{le32(head + 10)} << 32);
  page->granule = g == 0xFFFFFFFFFFFFFFFFull ? -1 : static_cast<int64_t>(g);
  page->serial = le32(head + 14);
  page->segments = head[26];
  page->header_size = 27u + page->segments;
  if (page->segments && !read_exact(source, pos + 27, ByteView(page->lacing, page->segments))) return false;
  uint32_t body = 0;
  for (uint32_t i = 0; i < page->segments; i++) body += page->lacing[i];
  page->body_size = body;
  return pos + page->size() <= source.size();
}

struct OpusHeader {
  uint32_t channels = 0, pre_skip = 0, input_rate = 0;
  int32_t gain_q8 = 0;
  uint32_t mapping_family = 0;
};

bool parse_opus_head(ConstByteView p, OpusHeader* h) {
  if (p.size() < 19) return false;
  static const char kMagic[9] = "OpusHead";
  for (int i = 0; i < 8; i++)
    if (p[i] != static_cast<uint8_t>(kMagic[i])) return false;
  if ((p[8] >> 4) != 0) return false;  // version 0 only
  h->channels = p[9];
  h->pre_skip = le16(p.data() + 10);
  h->input_rate = le32(p.data() + 12);
  h->gain_q8 = static_cast<int16_t>(le16(p.data() + 16));
  h->mapping_family = p[18];
  return h->mapping_family == 0 && (h->channels == 1 || h->channels == 2);
}

float gain_factor(int32_t gain_q8) {
  if (gain_q8 == 0) return 1.0f;
  return static_cast<float>(fmath::pow(10.0, gain_q8 / (20.0 * 256.0)));
}

// Scales 16-bit samples by the output gain of the header, in place.
void apply_gain(ArrayView<int16_t> samples, float gain) {
  if (gain == 1.0f) return;
  for (int16_t& s : samples) {
    float v = static_cast<float>(s) * gain;
    v = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v;
    s = static_cast<int16_t>(v >= 0 ? v + .5f : v - .5f);
  }
}

// Reads the packets of the audio of one Opus stream from the pages of an Ogg source, one page at a time.
class OpusPageReader {
 public:
  struct PageInfo {
    uint64_t offset = 0;
    int64_t granule = -1;
    uint64_t start_pos = 0;  // where the first packet that begins in the page starts, in 48 kHz samples from the start of the stream
    bool continued = false;
  };

  OpusPageReader(IByteSource& source, allocators::IAllocator& allocator)
      : source_(source), allocator_(allocator), pages_(allocator), partial_(allocator), body_(allocator), decoder_(allocator) {}

  // Finds the stream and reads its headers. False if this is not an Opus file this decoder can play.
  bool open() {
    uint64_t pos = 0;
    OggPage page;
    bool have_serial = false;
    uint32_t serial = 0;
    uint32_t header_packets_done = 0;
    int64_t last_granule = 0;
    uint64_t guard = 0;
    while (read_page_header(source_, pos, &page)) {
      if (++guard > 1u << 24) return false;
      const uint64_t next = pos + page.size();
      if (!have_serial) {
        if (!(page.flags & 2)) return false;  // the first page must begin a stream
        serial = page.serial;
        have_serial = true;
      }
      if (page.serial != serial) {
        pos = next;
        continue;
      }
      if (header_packets_done < 2) {
        if (!read_body(page)) return false;
        uint64_t at = 0;
        for (uint32_t s = 0; s < page.segments && header_packets_done < 2; s++) {
          const uint32_t len = page.lacing[s];
          if (!partial_.append(body_.view().subview(at, len))) return false;
          at += len;
          if (len < 255) {
            if (header_packets_done == 0 && !parse_opus_head(partial_.view(), &header_)) return false;
            ++header_packets_done;
            partial_.clear();
          }
        }
        if (header_packets_done == 2) first_audio_ = pages_.count() + 1;  // audio starts on the next page, if any
        PageInfo header_page;
        header_page.offset = pos;
        header_page.granule = page.granule;
        header_page.continued = (page.flags & 1) != 0;
        if (!pages_.push_back(header_page)) return false;
        pos = next;
        if (page.flags & 4) return false;
        continue;
      }
      PageInfo info;
      info.offset = pos;
      info.granule = page.granule;
      info.continued = (page.flags & 1) != 0;
      info.start_pos = running_start_;
      if (!pages_.push_back(info)) return false;
      if (page.granule >= 0) {
        running_start_ = static_cast<uint64_t>(page.granule);
        last_granule = page.granule;
      }
      pos = next;
      if (page.flags & 4) break;
    }
    if (header_packets_done < 2 || pages_.count() <= first_audio_) return false;
    gain_ = gain_factor(header_.gain_q8);
    end_granule_ = last_granule;
    return decoder_.init(header_.channels);
  }

  uint32_t channels() const { return header_.channels; }
  uint32_t pre_skip() const { return header_.pre_skip; }
  float gain() const { return gain_; }
  uint64_t first_audio_page() const { return first_audio_; }
  uint64_t page_count() const { return pages_.count(); }
  const PageInfo& page(uint64_t i) const { return pages_[i]; }
  // The number of frames after trimming the pre-skip and the padding after the last packet.
  uint64_t frame_count() const {
    if (end_granule_ <= static_cast<int64_t>(header_.pre_skip)) return 0;
    return static_cast<uint64_t>(end_granule_) - header_.pre_skip;
  }

  void restart(uint64_t page_index) {
    decoder_.reset();
    partial_.clear();
    next_page_ = page_index;
  }
  uint64_t next_page() const { return next_page_; }

  // Decodes the packets that end in the next page, appending 16-bit samples (channels interleaved, gain applied, pre-skip not yet
  // removed) to `pcm`. False at the end or on a read failure.
  bool decode_next(ByteBuffer* pcm) {
    if (next_page_ >= pages_.count()) return false;
    OggPage page;
    if (!read_page_header(source_, pages_[next_page_].offset, &page) || !read_body(page)) return false;
    ++next_page_;
    uint64_t at = 0;
    const uint64_t pcm_start = pcm->size();
    for (uint32_t s = 0; s < page.segments; s++) {
      const uint32_t len = page.lacing[s];
      if (!partial_.append(body_.view().subview(at, len))) return false;
      at += len;
      if (len < 255) {
        if (!decode_packet(partial_.view(), pcm)) {
          partial_.clear();
          return false;
        }
        partial_.clear();
      }
    }
    const uint64_t added = pcm->size() - pcm_start;
    if (added) apply_gain(pcm->writable_view().subview(pcm_start, added).cast<int16_t>(), gain_);
    return true;
  }

 private:
  bool read_body(const OggPage& page) {
    body_.clear();
    if (page.body_size == 0) return true;
    if (!body_.put_zeros(page.body_size)) return false;
    return read_exact(source_, page.offset + page.header_size, body_.writable_view());
  }

  bool decode_packet(ConstByteView packet, ByteBuffer* pcm) {
    if (packet.empty()) return true;
    if (out_.empty()) {
      out_ = allocate_view<int16_t>(allocator_, uint64_t{OpusDecoder::kMaxFrames} * 2);
      if (out_.empty()) return false;
    }
    const int n = decoder_.decode(packet, out_);
    if (n < 0) {
      // A damaged packet: the time it should have covered is silence, when its length is known.
      OpusPacketInfo info;
      if (!parse_opus_packet(packet, &info)) return true;
      const uint64_t frames = uint64_t{info.frame_count} * info.samples_per_frame;
      return pcm->put_zeros(frames * header_.channels * 2);
    }
    return pcm->append(ArrayView<const int16_t>(out_).first(uint64_t{static_cast<uint32_t>(n)} * header_.channels).cast<const uint8_t>());
  }

 public:
  ~OpusPageReader() { release_view(allocator_, out_); }

 private:
  IByteSource& source_;
  allocators::IAllocator& allocator_;
  List<PageInfo> pages_;
  ByteBuffer partial_, body_;
  OpusDecoder decoder_;
  OpusHeader header_;
  ArrayView<int16_t> out_;
  float gain_ = 1.0f;
  uint64_t first_audio_ = 0, next_page_ = 0, running_start_ = 0;
  int64_t end_granule_ = 0;
};

// ---- the stream -------------------------------------------------------------------------------------------------------------

class OpusStream final : public StreamingPcmSource {
 public:
  OpusStream(IByteSource& source, allocators::IAllocator& allocator) : cached_(source, allocator), reader_(cached_, allocator), pcm_(allocator) {}

  bool init() {
    if (!reader_.open()) return false;
    format_.sample_rate = OpusDecoder::kSampleRate;
    format_.channels = static_cast<uint8_t>(reader_.channels());
    format_.type = audio::SampleType::S16;
    frames_ = reader_.frame_count();
    reader_.restart(reader_.first_audio_page());
    abs_pos_ = 0;
    return frames_ > 0 && format_.valid();
  }

 protected:
  bool refill(uint64_t frame) override {
    const uint64_t target = frame + reader_.pre_skip();
    if (target < abs_pos_ || target - abs_pos_ > 262144) restart_before(target);
    for (;;) {
      if (reader_.next_page() >= reader_.page_count()) return false;
      pcm_.clear();
      const uint64_t first = abs_pos_;
      if (!reader_.decode_next(&pcm_)) return false;
      const uint64_t frame_size = format_.frame_size();
      const uint64_t have = pcm_.size() / frame_size;
      abs_pos_ += have;
      if (have == 0 || first + have <= target) continue;
      // Drop the pre-skip, and stop where the stream's length says.
      uint64_t skip = 0;
      if (first < reader_.pre_skip()) skip = reader_.pre_skip() - first;
      if (skip >= have) continue;
      const uint64_t out_first = first + skip - reader_.pre_skip();
      uint64_t usable = have - skip;
      if (out_first + usable > frames_) usable = out_first < frames_ ? frames_ - out_first : 0;
      if (usable == 0) return false;
      if (out_first > frame) {  // the place wanted was inside what the restart dropped
        if (restart_page_ <= reader_.first_audio_page()) return false;
        restart_page(restart_page_ - 1);
        continue;
      }
      set_window(out_first, pcm_.view().subview(skip * frame_size, usable * frame_size));
      return true;
    }
  }

 private:
  void restart_before(uint64_t target) {
    uint64_t p = reader_.first_audio_page();
    for (uint64_t i = reader_.first_audio_page(); i < reader_.page_count(); i++) {
      if (reader_.page(i).start_pos + kPreRoll <= target) p = i;
      else break;
    }
    restart_page(p);
  }
  void restart_page(uint64_t p) {
    while (p > reader_.first_audio_page() && reader_.page(p).continued) --p;
    restart_page_ = p;
    reader_.restart(p);
    abs_pos_ = p == reader_.first_audio_page() ? 0 : reader_.page(p).start_pos;
  }

  CachedByteSource cached_;
  OpusPageReader reader_;
  ByteBuffer pcm_;
  uint64_t abs_pos_ = 0, restart_page_ = 0;
};

}  // namespace

UniquePointer<audio::IPcmSource> open_opus_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<OpusStream> stream = UniquePointer<OpusStream>::make(allocator, source, allocator);
  if (!stream || !stream->init()) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

bool decode_opus(ConstByteView data, AudioData* out) {
  out->clear();
  allocators::IAllocator& allocator = out->allocator();
  MemoryByteSource source(data);
  OpusPageReader reader(source, allocator);
  if (!reader.open()) return false;
  ByteBuffer pcm(allocator);
  reader.restart(reader.first_audio_page());
  while (reader.next_page() < reader.page_count())
    if (!reader.decode_next(&pcm)) break;
  const uint64_t frame_size = uint64_t{reader.channels()} * 2;
  uint64_t frames = pcm.size() / frame_size;
  const uint64_t skip = reader.pre_skip() < frames ? reader.pre_skip() : frames;
  frames -= skip;
  const uint64_t wanted = reader.frame_count();
  if (wanted && wanted < frames) frames = wanted;
  if (frames == 0) return false;
  audio::StreamFormat format;
  format.sample_rate = OpusDecoder::kSampleRate;
  format.channels = static_cast<uint8_t>(reader.channels());
  if (!out->create(format, frames)) return false;
  return out->samples().copy_from(pcm.view().subview(skip * frame_size, frames * frame_size).cast<const int16_t>());
}

}  // namespace media
