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

#include "Media/AudioStreaming.hpp"
#include "Midi/MidiStream.hpp"
#include "Media/Audio/Pcm.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace media {

uint32_t StreamingPcmSource::read(uint64_t frame, ByteView out) {
  const uint32_t size = format_.frame_size();
  if (size == 0 || frame >= frames_) return 0;
  if (!(frame >= window_first_ && frame < window_end_frame())) {
    if (!refill(frame) || !(frame >= window_first_ && frame < window_end_frame())) return 0;
  }
  uint64_t bytes = window_.size() - (frame - window_first_) * size;
  if (bytes > out.size()) bytes = out.size();
  bytes -= bytes % size;
  if (bytes) out.first(bytes).copy_from(window_.subview((frame - window_first_) * size, bytes));
  return static_cast<uint32_t>(bytes);
}

namespace {

constexpr uint32_t kWindowFrames = 16384;

uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
uint32_t be16(const uint8_t* p) { return (uint32_t{p[0]} << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }

// Uncompressed (or A-law / mu-law) sound laid out in a run of the source: converted to 16 bits a window at a time.
class PcmFileStream final : public StreamingPcmSource {
 public:
  PcmFileStream(IByteSource& source, allocators::IAllocator& allocator, uint64_t offset, uint64_t bytes, pcm::Encoding encoding, bool big_endian,
                uint32_t channels, uint32_t rate)
      : source_(source), allocator_(allocator), chunk_(allocator), offset_(offset), encoding_(encoding), big_endian_(big_endian), channels_(channels) {
    in_frame_ = uint64_t{channels} * pcm::bytes_per_sample(encoding);
    frames_ = in_frame_ ? bytes / in_frame_ : 0;
    format_.sample_rate = rate;
    format_.channels = static_cast<uint8_t>(channels > 1 ? 2 : 1);
    format_.type = audio::SampleType::S16;
  }
  ~PcmFileStream() { release_view(allocator_, raw_); }
  bool valid() const { return frames_ > 0 && format_.valid() && channels_ >= 1 && channels_ <= 64; }

 protected:
  bool refill(uint64_t frame) override {
    uint64_t n = frames_ - frame;
    if (n > kWindowFrames) n = kWindowFrames;
    const uint64_t bytes = n * in_frame_;
    if (raw_.size() < bytes) {
      release_view(allocator_, raw_);
      raw_ = allocate_view<uint8_t>(allocator_, uint64_t{kWindowFrames} * in_frame_);
      if (raw_.empty()) return false;
    }
    if (!read_exact(source_, offset_ + frame * in_frame_, raw_.first(bytes))) return false;
    if (!pcm::convert(ConstByteView(raw_).first(bytes), encoding_, big_endian_, channels_, format_.sample_rate, &chunk_)) return false;
    set_window(frame, chunk_.bytes());
    return true;
  }

 private:
  IByteSource& source_;
  allocators::IAllocator& allocator_;
  AudioData chunk_;
  ByteView raw_;
  uint64_t offset_;
  uint64_t in_frame_ = 0;
  pcm::Encoding encoding_;
  bool big_endian_;
  uint32_t channels_;
};

// WAVE data in blocks (IMA and Microsoft ADPCM): a window of whole blocks is wrapped in a WAV file in memory and handed to the WAV decoder.
class WaveBlockStream final : public StreamingPcmSource {
 public:
  WaveBlockStream(IByteSource& data, ConstByteView wave_format, allocators::IAllocator& allocator)
      : data_(data), allocator_(allocator), fmt_(allocator), wav_(allocator), chunk_(allocator) {
    ok_ = fmt_.append(wave_format);
  }

  // Works out the block size and how many frames a block holds (by decoding the first one). False if the data is not usable.
  bool init() {
    if (!ok_ || fmt_.size() < 16) return false;
    const uint8_t* f = fmt_.view().data();
    const uint32_t channels = le16(f + 2), rate = le32(f + 4);
    block_ = le16(f + 12);
    if (block_ < 5 || block_ > 65536 || data_.size() < block_) return false;
    blocks_ = data_.size() / block_;
    if (!decode_blocks(0, 1)) return false;
    per_block_ = chunk_.frame_count();
    format_.sample_rate = rate;
    format_.channels = static_cast<uint8_t>(chunk_.format().channels);
    format_.type = audio::SampleType::S16;
    if (channels == 0 || per_block_ == 0) return false;
    frames_ = blocks_ * per_block_;
    return format_.valid();
  }

 protected:
  bool refill(uint64_t frame) override {
    const uint64_t first_block = frame / per_block_;
    uint64_t count = kWindowFrames / per_block_;
    if (count == 0) count = 1;
    if (count > blocks_ - first_block) count = blocks_ - first_block;
    if (!decode_blocks(first_block, count)) return false;
    set_window(first_block * per_block_, chunk_.bytes());
    return true;
  }

 private:
  bool decode_blocks(uint64_t first_block, uint64_t count) {
    wav_.clear();
    const uint64_t bytes = count * block_;
    const bool pad = (fmt_.size() & 1) != 0;
    if (!(wav_.put_tag("RIFF") && wav_.put32(static_cast<uint32_t>(4 + 8 + fmt_.size() + (pad ? 1 : 0) + 8 + bytes)) && wav_.put_tag("WAVE") &&
          wav_.put_tag("fmt ") && wav_.put32(static_cast<uint32_t>(fmt_.size())) && wav_.append(fmt_.view()) && (!pad || wav_.put8(0)) && wav_.put_tag("data") &&
          wav_.put32(static_cast<uint32_t>(bytes))))
      return false;
    const uint64_t at = wav_.size();
    if (!wav_.put_zeros(bytes)) return false;
    ByteView dest(const_cast<uint8_t*>(wav_.view().data()) + at, bytes);
    if (!read_exact(data_, first_block * block_, dest)) return false;
    return decode_wav(wav_.view(), &chunk_);
  }

  IByteSource& data_;
  allocators::IAllocator& allocator_;
  ByteBuffer fmt_, wav_;
  AudioData chunk_;
  uint64_t blocks_ = 0, per_block_ = 0;
  uint32_t block_ = 0;
  bool ok_ = false;
};

// The WAVE_FORMAT_* tag (the real one for the extensible kind), channels, rate, bits and block size of a WAVEFORMATEX.
struct WaveFormat {
  uint32_t tag = 0, channels = 0, rate = 0, block_align = 0, bits = 0;
};

bool parse_wave_format(ConstByteView fmt, WaveFormat* out) {
  if (fmt.size() < 16) return false;
  const uint8_t* f = fmt.data();
  out->tag = le16(f);
  out->channels = le16(f + 2);
  out->rate = le32(f + 4);
  out->block_align = le16(f + 12);
  out->bits = le16(f + 14);
  if (out->tag == 0xFFFE) {   // extensible: the real format is the first two bytes of the sub-format GUID
    if (fmt.size() < 18 || le16(f + 16) < 22 || fmt.size() < 18 + 22) return false;
    out->tag = le16(f + 24);
  }
  return true;
}

UniquePointer<audio::IPcmSource> make_wave_stream(IByteSource& data, ConstByteView fmt, allocators::IAllocator& allocator) {
  WaveFormat w;
  if (!parse_wave_format(fmt, &w) || w.channels == 0 || w.rate == 0) return UniquePointer<audio::IPcmSource>();
  using pcm::Encoding;
  bool have = true;
  Encoding e = Encoding::S16;
  switch (w.tag) {
    case 1:
      if (w.bits == 8) e = Encoding::U8;
      else if (w.bits == 16) e = Encoding::S16;
      else if (w.bits == 24) e = Encoding::S24;
      else if (w.bits == 32) e = Encoding::S32;
      else have = false;
      break;
    case 3:
      if (w.bits == 32) e = Encoding::F32;
      else if (w.bits == 64) e = Encoding::F64;
      else have = false;
      break;
    case 6: e = Encoding::ALaw; break;
    case 7: e = Encoding::ULaw; break;
    case 2:
    case 0x11: {
      UniquePointer<WaveBlockStream> blocks = UniquePointer<WaveBlockStream>::make(allocator, data, fmt, allocator);
      if (!blocks || !blocks->init()) return UniquePointer<audio::IPcmSource>();
      return UniquePointer<audio::IPcmSource>(util::move(blocks));
    }
    default: have = false; break;
  }
  if (!have) return UniquePointer<audio::IPcmSource>();
  UniquePointer<PcmFileStream> stream = UniquePointer<PcmFileStream>::make(allocator, data, allocator, uint64_t{0}, data.size(), e, false, w.channels, w.rate);
  if (!stream || !stream->valid()) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

}  // namespace

UniquePointer<audio::IPcmSource> open_wave_data_stream(IByteSource& data, ConstByteView wave_format, allocators::IAllocator& allocator) {
  return make_wave_stream(data, wave_format, allocator);
}

// A stream over a run of the file together with the range source it reads through (declared first, so the stream goes before it).
namespace {
class RangedStream final : public audio::IPcmSource {
 public:
  RangedStream(IByteSource& inner, uint64_t offset, uint64_t length) : range_(inner, offset, length) {}
  RangeByteSource& range() { return range_; }
  void set(UniquePointer<audio::IPcmSource> stream) { stream_ = util::move(stream); }
  audio::StreamFormat format() const override { return stream_->format(); }
  uint64_t frame_count() const override { return stream_->frame_count(); }
  uint32_t read(uint64_t frame, ByteView out) override { return stream_->read(frame, out); }

 private:
  RangeByteSource range_;
  UniquePointer<audio::IPcmSource> stream_;
};
}  // namespace

UniquePointer<audio::IPcmSource> open_wav_stream(IByteSource& source, allocators::IAllocator& allocator) {
  uint8_t head[12];
  if (!read_exact(source, 0, ByteView(head)) || __builtin_memcmp(head, "RIFF", 4) != 0 || __builtin_memcmp(head + 8, "WAVE", 4) != 0)
    return UniquePointer<audio::IPcmSource>();
  ByteBuffer fmt(allocator);
  bool have_format = false;
  uint64_t at = 12;
  while (at + 8 <= source.size()) {
    uint8_t chunk[8];
    if (!read_exact(source, at, ByteView(chunk))) break;
    const uint64_t size = le32(chunk + 4), body = at + 8;
    const uint64_t available = source.size() - body;
    const uint64_t length = size < available ? size : available;
    if (__builtin_memcmp(chunk, "fmt ", 4) == 0) {
      if (length < 16 || length > 4096) return UniquePointer<audio::IPcmSource>();
      uint8_t buffer[4096];
      if (!read_exact(source, body, ByteView(buffer, length))) return UniquePointer<audio::IPcmSource>();
      fmt.clear();
      if (!fmt.append(ConstByteView(buffer, length))) return UniquePointer<audio::IPcmSource>();
      have_format = true;
    } else if (__builtin_memcmp(chunk, "data", 4) == 0) {
      if (!have_format) return UniquePointer<audio::IPcmSource>();
      UniquePointer<RangedStream> holder = UniquePointer<RangedStream>::make(allocator, source, body, length);
      if (!holder) return UniquePointer<audio::IPcmSource>();
      UniquePointer<audio::IPcmSource> stream = make_wave_stream(holder->range(), fmt.view(), allocator);
      if (!stream) return UniquePointer<audio::IPcmSource>();
      holder->set(util::move(stream));
      return UniquePointer<audio::IPcmSource>(util::move(holder));
    }
    at = body + size + (size & 1);
  }
  return UniquePointer<audio::IPcmSource>();
}

namespace {
// The 80-bit IEEE extended number AIFF uses for the sample rate, as an integer (0 if not sensible).
uint32_t extended_to_rate(const uint8_t* v) {
  const int32_t exponent = static_cast<int32_t>(be16(v) & 0x7FFF) - 16383;
  uint64_t mantissa = (uint64_t{be32(v + 2)} << 32) | be32(v + 6);
  if (v[0] & 0x80 || exponent < 0 || exponent > 40) return 0;
  const int32_t shift = 63 - exponent;
  const uint64_t value = shift >= 0 ? mantissa >> shift : mantissa << -shift;
  return value > 0xFFFFFFFFull ? 0 : static_cast<uint32_t>(value);
}

bool tag_is(const uint8_t* p, const char* tag) { return __builtin_memcmp(p, tag, 4) == 0; }
}  // namespace

UniquePointer<audio::IPcmSource> open_aiff_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<audio::IPcmSource> none;
  uint8_t head[12];
  if (!read_exact(source, 0, ByteView(head)) || !tag_is(head, "FORM") || !(tag_is(head + 8, "AIFF") || tag_is(head + 8, "AIFC"))) return none;
  const bool aifc = tag_is(head + 8, "AIFC");
  uint32_t channels = 0, bits = 0, rate = 0;
  uint64_t frames = 0;
  bool have_comm = false, little = false;
  pcm::Encoding encoding = pcm::Encoding::S16;
  uint64_t at = 12;
  while (at + 8 <= source.size()) {
    uint8_t chunk[8];
    if (!read_exact(source, at, ByteView(chunk))) return none;
    const uint64_t size = be32(chunk + 4), body = at + 8;
    const uint64_t available = source.size() - body;
    const uint64_t length = size < available ? size : available;
    if (tag_is(chunk, "COMM")) {
      uint8_t comm[40] = {};
      if (length < 18) return none;
      read_exact(source, body, ByteView(comm, length < sizeof comm ? length : sizeof comm));
      channels = be16(comm);
      frames = be32(comm + 2);
      bits = be16(comm + 6);
      rate = extended_to_rate(comm + 8);
      pcm::Encoding e = bits <= 8 ? pcm::Encoding::S8 : bits <= 16 ? pcm::Encoding::S16 : bits <= 24 ? pcm::Encoding::S24 : pcm::Encoding::S32;
      if (aifc) {
        if (length < 22) return none;
        const uint8_t* c = comm + 18;
        if (tag_is(c, "NONE") || tag_is(c, "twos")) {
        } else if (tag_is(c, "sowt")) {
          little = true;
        } else if (tag_is(c, "raw ") && bits == 8) {
          e = pcm::Encoding::U8;
        } else if (tag_is(c, "fl32") || tag_is(c, "FL32")) {
          e = pcm::Encoding::F32;
        } else if (tag_is(c, "fl64") || tag_is(c, "FL64")) {
          e = pcm::Encoding::F64;
        } else if (tag_is(c, "ulaw") || tag_is(c, "ULAW")) {
          e = pcm::Encoding::ULaw;
        } else if (tag_is(c, "alaw") || tag_is(c, "ALAW")) {
          e = pcm::Encoding::ALaw;
        } else {
          return none;
        }
      }
      if (bits == 0 || bits > 32) return none;
      encoding = e;
      have_comm = true;
    } else if (tag_is(chunk, "SSND")) {
      if (!have_comm || length < 8 || channels == 0 || rate == 0) return none;
      uint8_t ssnd[8];
      if (!read_exact(source, body, ByteView(ssnd))) return none;
      const uint64_t offset = be32(ssnd);
      if (8 + offset > length) return none;
      uint64_t bytes = length - 8 - offset;
      const uint64_t wanted = frames * channels * pcm::bytes_per_sample(encoding);
      if (wanted < bytes) bytes = wanted;
      UniquePointer<PcmFileStream> stream =
          UniquePointer<PcmFileStream>::make(allocator, source, allocator, body + 8 + offset, bytes, encoding, !little, channels, rate);
      if (!stream || !stream->valid()) return none;
      return UniquePointer<audio::IPcmSource>(util::move(stream));
    }
    at = body + size + (size & 1);
  }
  return none;
}

UniquePointer<audio::IPcmSource> open_au_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<audio::IPcmSource> none;
  uint8_t h[24];
  if (!read_exact(source, 0, ByteView(h)) || !tag_is(h, ".snd")) return none;
  const uint32_t offset = be32(h + 4), size = be32(h + 8), encoding = be32(h + 12), rate = be32(h + 16), channels = be32(h + 20);
  if (offset < 24 || offset > source.size() || rate == 0 || channels == 0) return none;
  uint64_t length = source.size() - offset;
  if (size != 0xFFFFFFFFu && size < length) length = size;   // 0xFFFFFFFF means "until the end"
  using pcm::Encoding;
  Encoding e;
  switch (encoding) {
    case 1: e = Encoding::ULaw; break;
    case 2: e = Encoding::S8; break;
    case 3: e = Encoding::S16; break;
    case 4: e = Encoding::S24; break;
    case 5: e = Encoding::S32; break;
    case 6: e = Encoding::F32; break;
    case 7: e = Encoding::F64; break;
    case 27: e = Encoding::ALaw; break;
    default: return none;
  }
  UniquePointer<PcmFileStream> stream = UniquePointer<PcmFileStream>::make(allocator, source, allocator, uint64_t{offset}, length, e, true, channels, rate);
  if (!stream || !stream->valid()) return none;
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

UniquePointer<audio::IPcmSource> open_audio_stream(IByteSource& source, allocators::IAllocator& allocator, AudioFileFormat* detected) {
  UniquePointer<audio::IPcmSource> none;
  const uint64_t head_size = source.size() < 16384 ? source.size() : 16384;
  ByteBuffer head(allocator);
  if (head_size == 0 || !head.put_zeros(head_size)) return none;
  if (!read_exact(source, 0, ByteView(const_cast<uint8_t*>(head.view().data()), head_size))) return none;
  const AudioFileFormat format = detect_audio_format(head.view());
  if (detected) *detected = format;
  switch (format) {
    case AudioFileFormat::Wav: return open_wav_stream(source, allocator);
    case AudioFileFormat::Aiff: return open_aiff_stream(source, allocator);
    case AudioFileFormat::Au: return open_au_stream(source, allocator);
    case AudioFileFormat::Flac: return open_flac_stream(source, allocator);
    case AudioFileFormat::Vorbis: return open_vorbis_stream(source, allocator);
    case AudioFileFormat::Mp3: return open_mp3_stream(source, allocator);
    case AudioFileFormat::Aac: return open_aac_stream(source, allocator);
    case AudioFileFormat::Mp4: return open_mp4_audio_stream(source, allocator);
    case AudioFileFormat::Midi: return midi::open_midi_stream(source, allocator);   // (in software; the media player offers MIDI hardware first)
    default: return none;
  }
}

}  // namespace media

