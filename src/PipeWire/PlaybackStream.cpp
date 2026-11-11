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

#include "PipeWire/PlaybackStream.hpp"

namespace pipewire {

namespace {

// SPA channel positions (spa/param/audio/raw.h)
constexpr uint32_t kMono = 2, kFL = 3, kFR = 4, kFC = 5, kLFE = 6, kSL = 7, kSR = 8, kFLC = 9, kFRC = 10, kRC = 11, kRL = 12, kRR = 13;

}  // namespace

void default_channel_positions(uint32_t channels, uint32_t* out) {
  static constexpr uint32_t k3[] = {kFL, kFR, kFC}, k4[] = {kFL, kFR, kRL, kRR}, k5[] = {kFL, kFR, kFC, kRL, kRR}, k6[] = {kFL, kFR, kFC, kLFE, kRL, kRR},
                            k7[] = {kFL, kFR, kFC, kLFE, kRC, kSL, kSR}, k8[] = {kFL, kFR, kFC, kLFE, kRL, kRR, kSL, kSR};
  const uint32_t* table = nullptr;
  switch (channels) {
    case 1: out[0] = kMono; return;
    case 2: out[0] = kFL; out[1] = kFR; return;
    case 3: table = k3; break;
    case 4: table = k4; break;
    case 5: table = k5; break;
    case 6: table = k6; break;
    case 7: table = k7; break;
    case 8: table = k8; break;
    default: break;
  }
  for (uint32_t i = 0; i < channels; ++i) out[i] = table ? table[i] : (i % 2 ? kFR : kFL);
}

void channel_side_weights(uint32_t position, int32_t* left, int32_t* right) {
  switch (position) {
    case kFL: case kSL: case kRL: case kFLC: *left = 1000; *right = 0; return;
    case kFR: case kSR: case kRR: case kFRC: *left = 0; *right = 1000; return;
    case kLFE: *left = 0; *right = 0; return;
    case kFC: case kRC: *left = 707; *right = 707; return;
    default: *left = 707; *right = 707; return;      // mono and the positions without a side
  }
}

namespace {

int32_t alaw_decode(uint8_t a) {
  a ^= 0x55;
  int32_t t = (a & 0x0F) << 4;
  const int32_t seg = (a & 0x70) >> 4;
  if (seg == 0) t += 8;
  else if (seg == 1) t += 0x108;
  else { t += 0x108; t <<= seg - 1; }
  return (a & 0x80) ? t : -t;
}

int32_t ulaw_decode(uint8_t u) {
  u = static_cast<uint8_t>(~u);
  int32_t t = ((u & 0x0F) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return (u & 0x80) ? 0x84 - t : t - 0x84;
}

int32_t clamp16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : static_cast<int32_t>(v); }

int32_t from_float(double f) {
  if (!(f == f)) return 0;      // NaN
  return clamp16(static_cast<int64_t>(f * 32767.0));
}

uint32_t le32_of(ConstByteView b, uint64_t at) { return uint32_t{b[at]} | uint32_t{b[at + 1]} << 8 | uint32_t{b[at + 2]} << 16 | uint32_t{b[at + 3]} << 24; }
uint32_t be32_of(ConstByteView b, uint64_t at) { return uint32_t{b[at]} << 24 | uint32_t{b[at + 1]} << 16 | uint32_t{b[at + 2]} << 8 | b[at + 3]; }

// One sample at `at` as a 16-bit value.
int32_t decode(SampleFormat f, ConstByteView b, uint64_t at) {
  switch (f) {
    case SampleFormat::U8: return (int32_t{b[at]} - 128) << 8;
    case SampleFormat::S16Le: return static_cast<int16_t>(b[at] | b[at + 1] << 8);
    case SampleFormat::S16Be: return static_cast<int16_t>(b[at] << 8 | b[at + 1]);
    case SampleFormat::S24Le: return static_cast<int16_t>(b[at + 1] | b[at + 2] << 8);
    case SampleFormat::S24Be: return static_cast<int16_t>(b[at] << 8 | b[at + 1]);
    case SampleFormat::S24_32Le: return static_cast<int16_t>(b[at + 1] | b[at + 2] << 8);
    case SampleFormat::S32Le: return static_cast<int16_t>(b[at + 2] | b[at + 3] << 8);
    case SampleFormat::S32Be: return static_cast<int16_t>(b[at] << 8 | b[at + 1]);
    case SampleFormat::F32Le: return from_float(__builtin_bit_cast(float, le32_of(b, at)));
    case SampleFormat::F32Be: return from_float(__builtin_bit_cast(float, be32_of(b, at)));
    case SampleFormat::F64Le: return from_float(__builtin_bit_cast(double, uint64_t{le32_of(b, at)} | uint64_t{le32_of(b, at + 4)} << 32));
    case SampleFormat::Alaw: return alaw_decode(b[at]);
    case SampleFormat::Ulaw: return ulaw_decode(b[at]);
  }
  return 0;
}

}  // namespace

uint32_t PcmFormat::sample_size() const {
  switch (format) {
    case SampleFormat::U8: case SampleFormat::Alaw: case SampleFormat::Ulaw: return 1;
    case SampleFormat::S16Le: case SampleFormat::S16Be: return 2;
    case SampleFormat::S24Le: case SampleFormat::S24Be: return 3;
    case SampleFormat::F64Le: return 8;
    default: return 4;
  }
}

uint32_t PcmFormat::spa_format() const {
  switch (format) {
    case SampleFormat::U8: return 0x102;
    case SampleFormat::S16Le: return 0x103;
    case SampleFormat::S16Be: return 0x104;
    case SampleFormat::S24_32Le: return 0x107;
    case SampleFormat::S32Le: return 0x10b;
    case SampleFormat::S32Be: return 0x10c;
    case SampleFormat::S24Le: return 0x10f;
    case SampleFormat::S24Be: return 0x110;
    case SampleFormat::F32Le: return 0x11b;
    case SampleFormat::F32Be: return 0x11c;
    case SampleFormat::F64Le: return 0x11d;
    case SampleFormat::Ulaw: return 0x11f;
    case SampleFormat::Alaw: return 0x120;
  }
  return 0;
}

const char* PcmFormat::name() const {
  switch (format) {
    case SampleFormat::U8: return "U8";
    case SampleFormat::S16Le: return "S16LE";
    case SampleFormat::S16Be: return "S16BE";
    case SampleFormat::S24Le: return "S24LE";
    case SampleFormat::S24Be: return "S24BE";
    case SampleFormat::S24_32Le: return "S24_32LE";
    case SampleFormat::S32Le: return "S32LE";
    case SampleFormat::S32Be: return "S32BE";
    case SampleFormat::F32Le: return "F32LE";
    case SampleFormat::F32Be: return "F32BE";
    case SampleFormat::F64Le: return "F64LE";
    case SampleFormat::Alaw: return "ALAW";
    case SampleFormat::Ulaw: return "ULAW";
  }
  return "UNKNOWN";
}

uint32_t convert_to_s16(const PcmFormat& in, const uint32_t* positions, ConstByteView source, ByteView out, uint32_t out_channels, uint32_t* consumed) {
  *consumed = 0;
  if (!in.valid() || (out_channels != 1 && out_channels != 2)) return 0;
  const uint32_t frame = in.frame_size(), sample = in.sample_size();
  uint64_t frames = source.size() / frame;
  if (frames > out.size() / (2 * out_channels)) frames = out.size() / (2 * out_channels);
  uint32_t order[PcmFormat::kMaxChannels];
  if (positions) for (uint32_t c = 0; c < in.channels; ++c) order[c] = positions[c];
  else default_channel_positions(in.channels, order);
  int32_t left_w[PcmFormat::kMaxChannels], right_w[PcmFormat::kMaxChannels];
  int64_t left_total = 0, right_total = 0;
  for (uint32_t c = 0; c < in.channels; ++c) {
    channel_side_weights(in.channels == 1 ? kMono : order[c], &left_w[c], &right_w[c]);
    left_total += left_w[c];
    right_total += right_w[c];
  }
  const bool straight = (in.channels == out_channels) && (in.channels == 1 || (order[0] == kFL && order[1] == kFR));
  uint64_t w = 0;
  for (uint64_t f = 0; f < frames; ++f) {
    const uint64_t base = f * frame;
    int32_t l = 0, r = 0;
    if (straight) {
      l = decode(in.format, source, base);
      r = in.channels == 2 ? decode(in.format, source, base + sample) : l;
    } else {
      int64_t ls = 0, rs = 0, all = 0;
      for (uint32_t c = 0; c < in.channels; ++c) {
        const int32_t v = decode(in.format, source, base + uint64_t{c} * sample);
        ls += int64_t{v} * left_w[c];
        rs += int64_t{v} * right_w[c];
        all += v;
      }
      l = left_total ? clamp16(ls / left_total) : clamp16(all / in.channels);
      r = right_total ? clamp16(rs / right_total) : clamp16(all / in.channels);
    }
    if (out_channels == 1) {
      const int32_t m = in.channels == 1 ? l : clamp16((int64_t{l} + r) / 2);
      out[w++] = static_cast<uint8_t>(m);
      out[w++] = static_cast<uint8_t>(m >> 8);
    } else {
      out[w++] = static_cast<uint8_t>(l);
      out[w++] = static_cast<uint8_t>(l >> 8);
      out[w++] = static_cast<uint8_t>(r);
      out[w++] = static_cast<uint8_t>(r >> 8);
    }
  }
  *consumed = static_cast<uint32_t>(frames * frame);
  return static_cast<uint32_t>(w);
}

// ---- PlaybackStream --------------------------------------------------------------------------------------------------------------------------------

PlaybackStream::PlaybackStream(allocators::IAllocator& allocator, const PcmFormat& format, SharedPointer<audio::AudioStream> stream, audio::AudioMixer* mixer)
    : format_(format), out_channels_(format.channels == 1 ? 1 : 2), stream_(util::move(stream)), carry_(allocator), scratch_(allocator), mixer_(mixer) {}

bool PlaybackStream::flush_carry_locked() {
  while (carry_offset_ < carry_.size()) {
    const uint32_t taken = stream_->write(carry_.view().subview(carry_offset_));
    if (taken == 0) return false;
    carry_offset_ += taken;
  }
  carry_.clear();
  carry_offset_ = 0;
  return true;
}

uint32_t PlaybackStream::write(ConstByteView pcm) {
  kernel::ScopedMutex guard(mutex_);
  if (stream_->closed() || !flush_carry_locked()) return 0;
  const uint32_t frame = format_.frame_size();
  if (frame == 0) return 0;
  uint32_t taken = 0;
  if (scratch_.size() == 0 && !scratch_.put_zeros(uint64_t{kChunkFrames} * 2 * out_channels_)) return 0;
  while (pcm.size() - taken >= frame) {
    // no more than the stream has room for (in the program's bytes), at least one frame so that a full ring is found out by the write
    uint32_t want = stream_->free_bytes();
    if (want == 0) break;
    ConstByteView part = pcm.subview(taken);
    // the stream's room is counted in 16-bit frames of its own format: as many of the program's frames
    const uint64_t frames_room = want / (2 * out_channels_);
    if (frames_room == 0) break;
    if (part.size() / frame > frames_room) part = part.first(frames_room * frame);
    uint32_t used = 0;
    const uint32_t made = convert_to_s16(format_, nullptr, part, scratch_.writable_view(), out_channels_, &used);
    if (used == 0) break;
    const uint32_t accepted = stream_->write(scratch_.view().first(made));
    taken += used;
    if (accepted < made) {
      // keep the rest: it is the program's and must still be played
      carry_.clear();
      carry_offset_ = 0;
      if (!carry_.append(scratch_.view().subview(accepted, made - accepted))) {
        // out of memory: those frames are lost, but the program is not told it may resend them (they count as taken)
        carry_.clear();
      }
      break;
    }
  }
  written_ += taken;
  return taken;
}

uint64_t PlaybackStream::played_bytes() {
  if (drained()) {
    kernel::ScopedMutex guard(mutex_);
    return written_;
  }
  // the mixer counts frames at the device's rate; the device's queue is still to be heard
  uint64_t device_frames = stream_->frames_mixed();
  if (mixer_) {
    const uint64_t mixed = mixer_->mixed_frames(), played = mixer_->played_frames();
    const uint64_t waiting = mixed > played ? mixed - played : 0;
    device_frames = device_frames > waiting ? device_frames - waiting : 0;
  }
  const uint64_t frames = device_frames * format_.rate / hardware::kAudioSampleRate;
  return frames * format_.frame_size();
}

uint64_t PlaybackStream::written_bytes() {
  kernel::ScopedMutex guard(mutex_);
  return written_;
}

uint64_t PlaybackStream::waiting_bytes() {
  kernel::ScopedMutex guard(mutex_);
  // (the stream and the carry hold 16-bit frames of out_channels_ at the program's rate: as many of the program's frames)
  const uint64_t converted = uint64_t{stream_->queued_bytes()} + (carry_.size() - carry_offset_);
  return converted / (2 * out_channels_) * format_.frame_size();
}

uint64_t PlaybackStream::buffered_bytes() {
  uint64_t written;
  {
    kernel::ScopedMutex guard(mutex_);
    written = written_;
  }
  const uint64_t played = played_bytes();
  return written > played ? written - played : 0;
}

bool PlaybackStream::drained() {
  bool empty;
  {
    kernel::ScopedMutex guard(mutex_);
    empty = carry_offset_ >= carry_.size() && stream_->drained();
    if (!empty) {
      drain_mark_ = 0;
      return false;
    }
    if (!mixer_) return true;
  }
  // (the mixer is asked without this stream's lock: the mixer's thread takes the stream's while it holds its own)
  const uint64_t mixed = mixer_->mixed_frames();
  uint64_t mark;
  {
    kernel::ScopedMutex guard(mutex_);
    if (drain_mark_ == 0) drain_mark_ = mixed ? mixed : 1;
    mark = drain_mark_;
  }
  return mixer_->played_frames() >= mark;
}

void PlaybackStream::flush() {
  kernel::ScopedMutex guard(mutex_);
  carry_.clear();
  carry_offset_ = 0;
  stream_->flush();
  drain_mark_ = 0;
  // what was thrown away counts as played, so that buffered_bytes() is 0 again
  const uint64_t frames = stream_->frames_mixed() * format_.rate / hardware::kAudioSampleRate;
  written_ = frames * format_.frame_size();
}

}  // namespace pipewire
