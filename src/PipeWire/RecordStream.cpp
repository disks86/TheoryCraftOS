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

#include "PipeWire/RecordStream.hpp"

namespace pipewire {

namespace {

constexpr uint32_t kMono = 2;

int32_t clamp16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : static_cast<int32_t>(v); }

// G.711 (the inverse of PlaybackStream's decoders)
uint8_t alaw_encode(int32_t v) {
  uint8_t mask = 0xD5;
  if (v < 0) {
    mask = 0x55;
    v = -v - 1;
  }
  if (v > 32767) v = 32767;
  static constexpr int32_t kEnds[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
  int32_t seg = 0;
  while (seg < 8 && v > kEnds[seg]) ++seg;
  if (seg >= 8) return static_cast<uint8_t>(0x7F ^ mask);
  uint8_t a = static_cast<uint8_t>(seg << 4);
  a |= static_cast<uint8_t>(seg < 2 ? (v >> 4) & 0x0F : (v >> (seg + 3)) & 0x0F);
  return static_cast<uint8_t>(a ^ mask);
}

uint8_t ulaw_encode(int32_t v) {
  constexpr int32_t kBias = 0x84, kClip = 32635;
  uint8_t sign = 0;
  if (v < 0) {
    sign = 0x80;
    v = -v;
  }
  if (v > kClip) v = kClip;
  v += kBias;
  int32_t exponent = 7;
  for (int32_t mask = 0x4000; (v & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
  const int32_t mantissa = (v >> (exponent + 3)) & 0x0F;
  return static_cast<uint8_t>(~(sign | exponent << 4 | mantissa));
}

void put_le32(ByteView b, uint64_t at, uint32_t x) {
  b[at] = static_cast<uint8_t>(x);
  b[at + 1] = static_cast<uint8_t>(x >> 8);
  b[at + 2] = static_cast<uint8_t>(x >> 16);
  b[at + 3] = static_cast<uint8_t>(x >> 24);
}

void put_be32(ByteView b, uint64_t at, uint32_t x) {
  b[at] = static_cast<uint8_t>(x >> 24);
  b[at + 1] = static_cast<uint8_t>(x >> 16);
  b[at + 2] = static_cast<uint8_t>(x >> 8);
  b[at + 3] = static_cast<uint8_t>(x);
}

// One 16-bit sample written at `at` in format `f`.
void encode(SampleFormat f, int32_t v, ByteView b, uint64_t at) {
  const uint32_t u = static_cast<uint32_t>(v);
  switch (f) {
    case SampleFormat::U8: b[at] = static_cast<uint8_t>((v >> 8) + 128); return;
    case SampleFormat::S16Le: b[at] = static_cast<uint8_t>(u); b[at + 1] = static_cast<uint8_t>(u >> 8); return;
    case SampleFormat::S16Be: b[at] = static_cast<uint8_t>(u >> 8); b[at + 1] = static_cast<uint8_t>(u); return;
    case SampleFormat::S24Le: b[at] = 0; b[at + 1] = static_cast<uint8_t>(u); b[at + 2] = static_cast<uint8_t>(u >> 8); return;
    case SampleFormat::S24Be: b[at] = static_cast<uint8_t>(u >> 8); b[at + 1] = static_cast<uint8_t>(u); b[at + 2] = 0; return;
    case SampleFormat::S24_32Le: put_le32(b, at, ((u << 8) & 0x00FFFFFFu) | (v < 0 ? 0xFF000000u : 0)); return;
    case SampleFormat::S32Le: put_le32(b, at, u << 16); return;
    case SampleFormat::S32Be: put_be32(b, at, u << 16); return;
    case SampleFormat::F32Le: put_le32(b, at, __builtin_bit_cast(uint32_t, static_cast<float>(v) / 32768.0f)); return;
    case SampleFormat::F32Be: put_be32(b, at, __builtin_bit_cast(uint32_t, static_cast<float>(v) / 32768.0f)); return;
    case SampleFormat::F64Le: {
      const uint64_t d = __builtin_bit_cast(uint64_t, static_cast<double>(v) / 32768.0);
      put_le32(b, at, static_cast<uint32_t>(d));
      put_le32(b, at + 4, static_cast<uint32_t>(d >> 32));
      return;
    }
    case SampleFormat::Alaw: b[at] = alaw_encode(v); return;
    case SampleFormat::Ulaw: b[at] = ulaw_encode(v); return;
  }
}

}  // namespace

uint32_t convert_from_s16(ConstByteView source, uint32_t in_channels, const PcmFormat& out, const uint32_t* positions, ByteView target, uint32_t* consumed) {
  *consumed = 0;
  if (!out.valid() || (in_channels != 1 && in_channels != 2)) return 0;
  const uint32_t in_frame = 2 * in_channels, frame = out.frame_size(), sample = out.sample_size();
  uint64_t frames = source.size() / in_frame;
  if (frames > target.size() / frame) frames = target.size() / frame;
  uint32_t order[PcmFormat::kMaxChannels];
  if (positions) for (uint32_t c = 0; c < out.channels; ++c) order[c] = positions[c];
  else default_channel_positions(out.channels, order);
  // which side each output channel takes: 0 left, 1 right, 2 both (averaged), 3 none
  uint8_t side[PcmFormat::kMaxChannels];
  for (uint32_t c = 0; c < out.channels; ++c) {
    int32_t l = 0, r = 0;
    channel_side_weights(out.channels == 1 ? kMono : order[c], &l, &r);
    side[c] = l == 1000 ? 0 : r == 1000 ? 1 : (l == 0 && r == 0) ? 3 : 2;
  }
  for (uint64_t f = 0; f < frames; ++f) {
    const uint64_t at = f * in_frame;
    const int32_t l = static_cast<int16_t>(source[at] | source[at + 1] << 8);
    const int32_t r = in_channels == 2 ? static_cast<int16_t>(source[at + 2] | source[at + 3] << 8) : l;
    const int32_t both = clamp16((int64_t{l} + r) / 2);
    const uint64_t base = f * frame;
    for (uint32_t c = 0; c < out.channels; ++c) {
      const int32_t v = side[c] == 0 ? l : side[c] == 1 ? r : side[c] == 2 ? both : 0;
      encode(out.format, v, target, base + uint64_t{c} * sample);
    }
  }
  *consumed = static_cast<uint32_t>(frames * in_frame);
  return static_cast<uint32_t>(frames * frame);
}

// ---- RecordStream ----------------------------------------------------------------------------------------------------------------------------------

RecordStream::RecordStream(allocators::IAllocator& allocator, const PcmFormat& format, SharedPointer<audio::CaptureStream> capture)
    : format_(format), in_channels_(capture->format().channels), capture_(util::move(capture)), scratch_(allocator) {}

uint32_t RecordStream::read(ByteView out) {
  if (paused()) return 0;
  kernel::ScopedMutex guard(mutex_);
  const uint32_t frame = format_.frame_size(), in_frame = 2 * in_channels_;
  if (frame == 0) return 0;
  if (scratch_.size() == 0 && !scratch_.put_zeros(uint64_t{kChunkFrames} * in_frame)) return 0;
  uint64_t done = 0;
  while (out.size() - done >= frame) {
    uint64_t frames = (out.size() - done) / frame;
    if (frames > kChunkFrames) frames = kChunkFrames;
    const uint32_t got = capture_->read(scratch_.writable_view().first(frames * in_frame));
    if (got == 0) break;
    uint32_t used = 0;
    done += convert_from_s16(scratch_.view().first(got), in_channels_, format_, nullptr, out.subview(done), &used);
    if (got < frames * in_frame) break;
  }
  read_ += done;
  return static_cast<uint32_t>(done);
}

uint64_t RecordStream::available_bytes() {
  if (paused()) return 0;
  return uint64_t{capture_->available_bytes()} / (2 * in_channels_) * format_.frame_size();
}

uint64_t RecordStream::read_bytes() {
  kernel::ScopedMutex guard(mutex_);
  return read_;
}

uint64_t RecordStream::lost_bytes() {
  const uint64_t lost = capture_->statistics().frames_lost;
  kernel::ScopedMutex guard(mutex_);
  return (lost > lost_base_ ? lost - lost_base_ : 0) * format_.frame_size();
}

void RecordStream::flush() { capture_->flush(); }

void RecordStream::set_paused(bool paused) {
  kernel::ScopedMutex guard(mutex_);
  if (paused == __atomic_load_n(&paused_, __ATOMIC_ACQUIRE)) return;
  if (!paused) {
    // what it recorded while paused is not the program's; nor is what it lost meanwhile
    capture_->flush();
    lost_base_ = capture_->statistics().frames_lost;
  }
  __atomic_store_n(&paused_, paused, __ATOMIC_RELEASE);
}

}  // namespace pipewire
