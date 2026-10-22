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

// Opus decoder (RFC 6716): packet structure, mode and bandwidth switching, redundancy and transition cross-fades around the SILK and
// CELT layers. A port of the frame logic of libopus v1.4 (BSD-3-Clause; Copyright Xiph.Org Foundation, Skype Limited, Jean-Marc
// Valin, Koen Vos). Not done: packet loss concealment (a missing or one-byte DTX packet decodes to silence) and the in-band FEC.
#include "Media/Audio/OpusCelt.hpp"
#include "Media/Audio/OpusInternal.hpp"
#include "Media/Audio/OpusSilk.hpp"

namespace media {

namespace {
constexpr int kModeNone = 0, kModeSilk = 1, kModeHybrid = 2, kModeCelt = 3;
constexpr int kBwNarrow = 1, kBwMedium = 2, kBwWide = 3, kBwSuperWide = 4, kBwFull = 5;

int packet_mode(uint8_t toc) {
  if (toc & 0x80) return kModeCelt;
  if ((toc & 0x60) == 0x60) return kModeHybrid;
  return kModeSilk;
}

int packet_bandwidth(uint8_t toc) {
  if (toc & 0x80) {
    const int bw = kBwMedium + ((toc >> 5) & 0x3);
    return bw == kBwMedium ? kBwNarrow : bw;
  }
  if ((toc & 0x60) == 0x60) return (toc & 0x10) ? kBwFull : kBwSuperWide;
  return kBwNarrow + ((toc >> 5) & 0x3);
}

int samples_per_frame(uint8_t toc) {
  const int Fs = 48000;
  if (toc & 0x80) return (Fs << ((toc >> 3) & 0x3)) / 400;
  if ((toc & 0x60) == 0x60) return (toc & 0x08) ? Fs / 50 : Fs / 100;
  const int a = (toc >> 3) & 0x3;
  return a == 3 ? Fs * 60 / 1000 : (Fs << a) / 100;
}

int parse_size(ConstByteView data, uint64_t at, int32_t len, int* size) {
  if (len < 1) {
    *size = -1;
    return -1;
  }
  uint8_t b0 = 0;
  data.try_get(at, &b0);
  if (b0 < 252) {
    *size = b0;
    return 1;
  }
  if (len < 2) {
    *size = -1;
    return -1;
  }
  uint8_t b1 = 0;
  data.try_get(at + 1, &b1);
  *size = 4 * b1 + b0;
  return 2;
}
}  // namespace

bool parse_opus_packet(ConstByteView packet, OpusPacketInfo* info) {
  *info = OpusPacketInfo();
  if (packet.empty()) return false;
  const uint8_t toc = packet[0];
  info->toc = toc;
  info->samples_per_frame = static_cast<uint32_t>(samples_per_frame(toc));
  info->channels = (toc & 0x4) ? 2 : 1;
  info->bandwidth = static_cast<uint32_t>(packet_bandwidth(toc));
  const int m = packet_mode(toc);
  info->mode = m == kModeSilk ? 0u : m == kModeHybrid ? 1u : 2u;
  uint64_t at = 1;
  int32_t len = static_cast<int32_t>(packet.size()) - 1;
  int32_t last_size = len;
  int count, sizes[48] = {};
  bool cbr = false;
  switch (toc & 0x3) {
    case 0: count = 1; break;
    case 1:
      count = 2;
      cbr = true;
      if (len & 1) return false;
      last_size = len / 2;
      sizes[0] = last_size;
      break;
    case 2: {
      count = 2;
      const int bytes = parse_size(packet, at, len, &sizes[0]);
      len -= bytes;
      if (sizes[0] < 0 || sizes[0] > len) return false;
      at += static_cast<uint64_t>(bytes);
      last_size = len - sizes[0];
      break;
    }
    default: {
      if (len < 1) return false;
      const uint8_t ch = packet[at++];
      count = ch & 0x3F;
      if (count <= 0 || static_cast<int32_t>(info->samples_per_frame) * count > 5760) return false;
      len--;
      if (ch & 0x40) {
        int p;
        do {
          if (len <= 0) return false;
          p = packet[at++];
          len--;
          const int tmp = p == 255 ? 254 : p;
          len -= tmp;
        } while (p == 255);
      }
      if (len < 0) return false;
      cbr = !(ch & 0x80);
      if (!cbr) {
        last_size = len;
        for (int i = 0; i < count - 1; i++) {
          const int bytes = parse_size(packet, at, len, &sizes[i]);
          len -= bytes;
          if (sizes[i] < 0 || sizes[i] > len) return false;
          at += static_cast<uint64_t>(bytes);
          last_size -= bytes + sizes[i];
        }
        if (last_size < 0) return false;
      } else {
        last_size = len / count;
        if (last_size * count != len) return false;
        for (int i = 0; i < count - 1; i++) sizes[i] = last_size;
      }
      break;
    }
  }
  if (last_size > 1275) return false;
  sizes[count - 1] = last_size;
  info->frame_count = static_cast<uint32_t>(count);
  for (int i = 0; i < count; i++) {
    info->frame_size[i] = static_cast<uint32_t>(sizes[i]);
    info->frame_offset[i] = static_cast<uint32_t>(at);
    at += static_cast<uint64_t>(sizes[i]);
  }
  return at <= packet.size();
}

namespace opus {

class Decoder {
 public:
  bool init(int channels);
  void reset();
  int decode(ConstByteView packet, ArrayView<int16_t> pcm);
  int channels() const { return channels_; }

 private:
  int decode_frame(ConstByteView data, int len, float* pcm, int frame_size);
  static void smooth_fade(const float* in1, const float* in2, float* out, int overlap, int channels, const float* window);

  int channels_ = 1, stream_channels_ = 1;
  int bandwidth_ = 0, mode_ = kModeNone, prev_mode_ = kModeNone, frame_size_ = 0;
  bool prev_redundancy_ = false;
  CeltDecoder celt_;
  SilkDecoder silk_;
  float out_f_[2 * 5760];        // one packet of output, before conversion to 16 bits
  float pcm_silk_f_[2 * 2880];   // the SILK part of a frame (up to 60 ms), scaled to +-1
  int16_t pcm_silk_[2 * 2880];
};

bool Decoder::init(int channels) {
  if (channels < 1 || channels > 2) return false;
  channels_ = stream_channels_ = channels;
  celt_.init(channels);
  reset();
  return true;
}

void Decoder::reset() {
  celt_.init(channels_);
  silk_.reset();
  stream_channels_ = channels_;
  bandwidth_ = 0;
  mode_ = prev_mode_ = kModeNone;
  frame_size_ = 960;
  prev_redundancy_ = false;
}

void Decoder::smooth_fade(const float* in1, const float* in2, float* out, int overlap, int channels, const float* window) {
  for (int c = 0; c < channels; c++)
    for (int i = 0; i < overlap; i++) {
      const float w = window[i] * window[i];
      out[i * channels + c] = w * in2[i * channels + c] + (1.f - w) * in1[i * channels + c];
    }
}

// One frame of `len` bytes (a whole Opus frame: 2.5 .. 60 ms; 40 and 60 ms SILK frames hold several SILK frames). `pcm` receives
// frame_size_ samples per channel; returns that number or -1.
int Decoder::decode_frame(ConstByteView data, int len, float* pcm, int frame_size) {
  const int F20 = 960, F10 = 480, F5 = 240, F2_5 = 120;
  const int C = channels_;
  if (frame_size < F2_5) return -1;
  if (frame_size > 2880) frame_size = 2880;
  RangeDecoder dec;
  int audiosize = frame_size_;
  const int mode = mode_;
  const int bandwidth = bandwidth_;
  if (len <= 1) {  // no packet loss concealment: silence of the frame's length
    for (int i = 0; i < frame_size * C; i++) pcm[i] = 0;
    return frame_size;
  }
  dec.init(data.first(static_cast<uint64_t>(len)));
  if (audiosize > frame_size) return -1;
  frame_size = audiosize;

  // Mode switches fade from a short stretch of concealment (here: silence) to the new signal.
  bool transition = false;
  if (prev_mode_ > 0 && ((mode == kModeCelt && prev_mode_ != kModeCelt && !prev_redundancy_) || (mode != kModeCelt && prev_mode_ == kModeCelt)))
    transition = true;
  float pcm_transition[2 * 240];
  if (transition)
    for (int i = 0; i < F5 * C; i++) pcm_transition[i] = 0;

  float* pcm_silk_f = pcm_silk_f_;
  int16_t* pcm_silk = pcm_silk_;
  if (mode != kModeCelt) {
    if (prev_mode_ == kModeCelt) silk_.reset();
    SilkDecoder::Control control;
    control.payload_ms = audiosize * 1000 / 48000 > 10 ? audiosize * 1000 / 48000 : 10;
    control.channels_internal = stream_channels_;
    control.channels_api = C;
    if (mode == kModeSilk) control.internal_rate = bandwidth == kBwNarrow ? 8000 : bandwidth == kBwMedium ? 12000 : 16000;
    else control.internal_rate = 16000;
    int decoded = 0;
    int16_t* p = pcm_silk;
    do {
      int n = 0;
      if (silk_.decode(control, dec, decoded == 0, p, &n) != 0) return -1;
      p += n * C;
      decoded += n;
    } while (decoded < frame_size);
    for (int i = 0; i < frame_size * C; i++) pcm_silk_f[i] = (1.f / 32768.f) * static_cast<float>(pcm_silk[i]);
  }

  int start_band = 0;
  bool redundancy = false, celt_to_silk = false;
  int redundancy_bytes = 0;
  if (mode != kModeCelt && dec.tell() + 17 + 20 * (mode == kModeHybrid) <= 8 * len) {
    if (mode == kModeHybrid) redundancy = dec.bit_logp(12) != 0;
    else redundancy = true;
    if (redundancy) {
      celt_to_silk = dec.bit_logp(1) != 0;
      redundancy_bytes = mode == kModeHybrid ? static_cast<int>(dec.uint_(256)) + 2 : len - ((dec.tell() + 7) >> 3);
      len -= redundancy_bytes;
      if (len * 8 < dec.tell()) {
        len = 0;
        redundancy_bytes = 0;
        redundancy = false;
      }
      dec.trim_end(static_cast<uint32_t>(redundancy_bytes));  // the redundancy bytes are not frame data
    }
  }
  if (mode != kModeCelt) start_band = 17;
  if (redundancy) transition = false;
  int endband = 21;
  switch (bandwidth) {
    case kBwNarrow: endband = 13; break;
    case kBwMedium:
    case kBwWide: endband = 17; break;
    case kBwSuperWide: endband = 19; break;
    default: endband = 21; break;
  }
  celt_.set_band_range(0, endband);
  celt_.set_stream_channels(stream_channels_);
  float redundant_audio[2 * 240];
  if (redundancy && celt_to_silk) {
    RangeDecoder rdec;
    rdec.init(data.subview(static_cast<uint64_t>(len), static_cast<uint64_t>(redundancy_bytes)));
    celt_.set_band_range(0, endband);
    celt_.decode(rdec, redundancy_bytes, redundant_audio, F5);
  }
  celt_.set_band_range(start_band, endband);
  int celt_ret = 0;
  if (mode != kModeSilk) {
    const int celt_frame_size = F20 < frame_size ? F20 : frame_size;
    if (mode != prev_mode_ && prev_mode_ > 0 && !prev_redundancy_) {
      celt_.init(channels_);  // OPUS_RESET_STATE
      celt_.set_band_range(start_band, endband);
    }
    celt_.set_stream_channels(stream_channels_);
    celt_ret = celt_.decode(dec, len, pcm, celt_frame_size);
  } else {
    for (int i = 0; i < frame_size * C; i++) pcm[i] = 0;
    if (prev_mode_ == kModeHybrid && !(redundancy && celt_to_silk && prev_redundancy_)) {
      const uint8_t silence[2] = {0xFF, 0xFF};
      RangeDecoder sdec;
      sdec.init(ConstByteView(silence, 2));
      celt_.set_band_range(0, endband);
      celt_.decode(sdec, 2, pcm, F2_5);
    }
  }
  if (celt_ret < 0) return -1;
  if (mode != kModeCelt)
    for (int i = 0; i < frame_size * C; i++) pcm[i] = pcm[i] + pcm_silk_f[i];
  const float* window = celt_.window();
  if (redundancy && !celt_to_silk) {
    celt_.init(channels_);
    celt_.set_band_range(0, endband);
    celt_.set_stream_channels(stream_channels_);
    RangeDecoder rdec;
    rdec.init(data.subview(static_cast<uint64_t>(len), static_cast<uint64_t>(redundancy_bytes)));
    celt_.decode(rdec, redundancy_bytes, redundant_audio, F5);
    smooth_fade(pcm + C * (frame_size - F2_5), redundant_audio + C * F2_5, pcm + C * (frame_size - F2_5), F2_5, C, window);
  }
  if (redundancy && celt_to_silk && (prev_mode_ != kModeSilk || prev_redundancy_)) {
    for (int c = 0; c < C; c++)
      for (int i = 0; i < F2_5; i++) pcm[C * i + c] = redundant_audio[C * i + c];
    smooth_fade(redundant_audio + C * F2_5, pcm + C * F2_5, pcm + C * F2_5, F2_5, C, window);
  }
  if (transition) {
    if (audiosize >= F5) {
      for (int i = 0; i < C * F2_5; i++) pcm[i] = pcm_transition[i];
      smooth_fade(pcm_transition + C * F2_5, pcm + C * F2_5, pcm + C * F2_5, F2_5, C, window);
    } else {
      smooth_fade(pcm_transition, pcm, pcm, F2_5, C, window);
    }
  }
  prev_mode_ = mode;
  prev_redundancy_ = redundancy && !celt_to_silk;
  return audiosize;
}

int Decoder::decode(ConstByteView packet, ArrayView<int16_t> out) {
  OpusPacketInfo info;
  if (packet.empty()) return -1;
  if (!parse_opus_packet(packet, &info)) return -1;
  const int count = static_cast<int>(info.frame_count);
  const int capacity = static_cast<int>(out.size() / static_cast<uint64_t>(channels_));
  if (count * static_cast<int>(info.samples_per_frame) > capacity || count * static_cast<int>(info.samples_per_frame) > 5760) return -1;
  float* pcm = out_f_;
  mode_ = packet_mode(info.toc);
  bandwidth_ = static_cast<int>(info.bandwidth);
  frame_size_ = static_cast<int>(info.samples_per_frame);
  stream_channels_ = static_cast<int>(info.channels);
  int nb = 0;
  for (int i = 0; i < count; i++) {
    const int ret = decode_frame(packet.subview(info.frame_offset[i]), static_cast<int>(info.frame_size[i]), pcm + nb * channels_, capacity - nb);
    if (ret < 0) return -1;
    nb += ret;
  }
  for (int i = 0; i < nb * channels_; i++) {
    float v = out_f_[i] * 32768.f;
    v = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v;
    out[static_cast<uint64_t>(i)] = static_cast<int16_t>(v >= 0 ? v + .5f : v - .5f);
  }
  return nb;
}

}  // namespace opus

OpusDecoder::~OpusDecoder() {}

bool OpusDecoder::init(uint32_t channels) {
  impl_ = UniquePointer<opus::Decoder>::make(allocator_);
  return impl_ && impl_->init(static_cast<int>(channels));
}

void OpusDecoder::reset() {
  if (impl_) impl_->reset();
}

uint32_t OpusDecoder::channels() const { return impl_ ? static_cast<uint32_t>(impl_->channels()) : 0; }

int OpusDecoder::decode(ConstByteView packet, ArrayView<int16_t> pcm) {
  if (!impl_) return -1;
  return impl_->decode(packet, pcm);
}

}  // namespace media
