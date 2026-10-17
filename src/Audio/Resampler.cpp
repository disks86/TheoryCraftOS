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

#include "Audio/Resampler.hpp"

namespace audio {

bool Resampler::configure(uint32_t source_rate, uint32_t target_rate) {
  if (source_rate < StreamFormat::kMinRate || source_rate > StreamFormat::kMaxRate || target_rate < StreamFormat::kMinRate ||
      target_rate > StreamFormat::kMaxRate)
    return false;
  source_rate_ = source_rate;
  target_rate_ = target_rate;
  step_ = (uint64_t{source_rate} << 32) / target_rate;
  mode_ = source_rate == target_rate ? Mode::Copy : source_rate < target_rate ? Mode::Up : Mode::Down;
  reset();
  return true;
}

void Resampler::reset() {
  have_a_ = has_b_ = false;
  fraction_ = 0;
  a_[0] = a_[1] = b_[0] = b_[1] = 0;
  sum_[0] = sum_[1] = 0;
  remaining_ = step_;
}

uint64_t Resampler::input_for(uint64_t out_frames) const {
  if (mode_ == Mode::Copy || source_rate_ == 0) return out_frames;
  return out_frames * source_rate_ / target_rate_ + (mode_ == Mode::Up ? 1 : 0);
}

uint64_t Resampler::output_for(uint64_t in_frames) const {
  if (mode_ == Mode::Copy || source_rate_ == 0) return in_frames;
  return in_frames * target_rate_ / source_rate_;
}

uint32_t Resampler::process(const int16_t* in, uint32_t in_frames, int16_t* out, uint32_t out_capacity, uint32_t* consumed) {
  uint32_t used = 0, made = 0;
  if (mode_ == Mode::Copy) {
    used = made = in_frames < out_capacity ? in_frames : out_capacity;
    __builtin_memcpy(out, in, uint64_t{made} * 2 * sizeof(int16_t));
  } else if (mode_ == Mode::Up) {
    for (;;) {
      if (has_b_) {  // the interval from a_ to b_: emit every output that falls inside it
        while (fraction_ < kOne) {
          if (made == out_capacity) goto done;
          for (int c = 0; c < 2; ++c)
            out[2 * made + c] = static_cast<int16_t>(a_[c] + (((int64_t{b_[c]} - a_[c]) * static_cast<int64_t>(fraction_)) >> 32));
          ++made;
          fraction_ += step_;
        }
        fraction_ -= kOne;
        a_[0] = b_[0];
        a_[1] = b_[1];
        has_b_ = false;
      }
      if (used == in_frames) break;
      b_[0] = in[2 * used];
      b_[1] = in[2 * used + 1];
      ++used;
      if (!have_a_) {  // the very first frame only starts the first interval
        a_[0] = b_[0];
        a_[1] = b_[1];
        have_a_ = true;
      } else {
        has_b_ = true;
      }
    }
  } else {  // Down: average the source over each output frame
    while (used < in_frames && made < out_capacity) {
      const int16_t* frame = in + 2 * used;
      ++used;
      uint64_t left = kOne;  // of this source frame
      while (left > 0) {
        const uint64_t take = left < remaining_ ? left : remaining_;
        sum_[0] += int64_t{frame[0]} * static_cast<int64_t>(take);
        sum_[1] += int64_t{frame[1]} * static_cast<int64_t>(take);
        remaining_ -= take;
        left -= take;
        if (remaining_ == 0) {  // an output frame is complete (step_ > one frame: at most one per source frame)
          for (int c = 0; c < 2; ++c) {
            const int64_t half = static_cast<int64_t>(step_ / 2);
            const int64_t average = sum_[c] >= 0 ? (sum_[c] + half) / static_cast<int64_t>(step_)
                                                 : -((-sum_[c] + half) / static_cast<int64_t>(step_));
            out[2 * made + c] = static_cast<int16_t>(average);
            sum_[c] = 0;
          }
          ++made;
          remaining_ = step_;
        }
      }
    }
  }
done:
  *consumed = used;
  return made;
}

bool FormatConverter::configure(const StreamFormat& source, const StreamFormat& target) {
  if (!source.valid() || !target.valid() || !resampler_.configure(source.sample_rate, target.sample_rate)) return false;
  source_ = source;
  target_ = target;
  return true;
}

uint64_t FormatConverter::input_bytes_for(uint64_t out_bytes) const {
  return resampler_.input_for(out_bytes / target_.frame_size()) * source_.frame_size();
}

uint64_t FormatConverter::output_bytes_for(uint64_t in_bytes) const {
  return resampler_.output_for(in_bytes / source_.frame_size()) * target_.frame_size();
}

namespace {
// Stereo 16-bit from any format.
void expand(const StreamFormat& format, const uint8_t* in, uint32_t frames, int16_t* out) {
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t samples[2];
    for (uint32_t c = 0; c < format.channels; ++c) {
      if (format.type == SampleType::S16) {
        const uint8_t* p = in + (i * format.channels + c) * 2;
        samples[c] = static_cast<int16_t>(p[0] | (p[1] << 8));
      } else {
        samples[c] = (int32_t{in[i * format.channels + c]} - 128) * 256;
      }
    }
    if (format.channels == 1) samples[1] = samples[0];
    out[2 * i] = static_cast<int16_t>(samples[0]);
    out[2 * i + 1] = static_cast<int16_t>(samples[1]);
  }
}

// Stereo 16-bit into any format.
void compress(const StreamFormat& format, const int16_t* in, uint32_t frames, uint8_t* out) {
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t samples[2] = {in[2 * i], in[2 * i + 1]};
    if (format.channels == 1) samples[0] = (samples[0] + samples[1]) / 2;
    for (uint32_t c = 0; c < format.channels; ++c) {
      if (format.type == SampleType::S16) {
        uint8_t* p = out + (i * format.channels + c) * 2;
        p[0] = static_cast<uint8_t>(samples[c]);
        p[1] = static_cast<uint8_t>(samples[c] >> 8);
      } else {
        int32_t v = (samples[c] + 32768 + 128) >> 8;  // rounded, 0 .. 256
        out[i * format.channels + c] = static_cast<uint8_t>(v > 255 ? 255 : v);
      }
    }
  }
}
}  // namespace

uint32_t FormatConverter::convert(ConstByteView in, ByteView out, uint32_t* consumed) {
  *consumed = 0;
  const uint32_t in_size = source_.frame_size(), out_size = target_.frame_size();
  if (in_size == 0 || out_size == 0) return 0;
  const uint64_t in_frames = in.size() / in_size, out_frames = out.size() / out_size;
  constexpr uint32_t kOutBlock = 512;
  int16_t stereo_in[kBlockFrames * 2], stereo_out[kOutBlock * 2];
  uint64_t used_total = 0, made_total = 0;
  while (used_total < in_frames && made_total < out_frames) {
    const uint32_t block = in_frames - used_total < kBlockFrames ? static_cast<uint32_t>(in_frames - used_total) : kBlockFrames;
    const uint32_t room = out_frames - made_total < kOutBlock ? static_cast<uint32_t>(out_frames - made_total) : kOutBlock;
    expand(source_, in.data() + used_total * in_size, block, stereo_in);
    uint32_t used = 0;
    const uint32_t made = resampler_.process(stereo_in, block, stereo_out, room, &used);
    compress(target_, stereo_out, made, out.data() + made_total * out_size);
    used_total += used;
    made_total += made;
    if (used == 0 && made == 0) break;
  }
  *consumed = static_cast<uint32_t>(used_total * in_size);
  return static_cast<uint32_t>(made_total * out_size);
}

}  // namespace audio
