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

#pragma once
#include <stdint.h>
#include "Audio/StreamFormat.hpp"
#include "Utility/ArrayView.hpp"

namespace audio {

// Sample rate conversion of stereo 16-bit audio, streaming: feed it blocks as they
// come and it carries what it needs between calls. Going up in rate it
// interpolates linearly between neighbouring frames; going down it averages
// the source over each output frame (a box filter, which also keeps the
// high frequencies that would alias from folding back). Equal rates copy. It is
// a plain, cheap converter, not a studio-grade one.
class Resampler {
 public:
  Resampler() = default;
  // False if either rate is outside StreamFormat::kMinRate..kMaxRate.
  bool configure(uint32_t source_rate, uint32_t target_rate);
  void reset();

  // Converts frames from `in` (interleaved left/right) into `out`, stopping when
  // the input is used up or `out_capacity` frames are produced. Returns the
  // frames produced and sets *consumed to the input frames used.
  uint32_t process(const int16_t* in, uint32_t in_frames, int16_t* out, uint32_t out_capacity, uint32_t* consumed);

  // Input frames needed to produce about `out_frames` (for sizing buffers).
  uint64_t input_for(uint64_t out_frames) const;
  // Output frames that `in_frames` of input will produce.
  uint64_t output_for(uint64_t in_frames) const;

 private:
  static constexpr uint64_t kOne = uint64_t{1} << 32;
  enum class Mode : uint8_t { Copy, Up, Down };

  Mode mode_ = Mode::Copy;
  uint32_t source_rate_ = 0, target_rate_ = 0;
  uint64_t step_ = kOne;  // input frames per output frame, in 32.32 fixed point

  // Up: the interval between frame a and frame b, and how far along it the next output is.
  int32_t a_[2] = {}, b_[2] = {};
  bool have_a_ = false, has_b_ = false;
  uint64_t fraction_ = 0;
  // Down: the running sum over the output frame being built, and how much of it is still to fill.
  int64_t sum_[2] = {};
  uint64_t remaining_ = 0;
};

// Converts between StreamFormats: sample type, channel count and rate. The
// source and target are fixed at configure(); state for the rate conversion is
// kept between calls, so audio can be fed in pieces of any size.
class FormatConverter {
 public:
  bool configure(const StreamFormat& source, const StreamFormat& target);
  void reset() { resampler_.reset(); }
  const StreamFormat& source() const { return source_; }
  const StreamFormat& target() const { return target_; }

  // Converts whole frames of `in` into `out`; stops when the input is used up or
  // `out` is full. Sets *consumed to the input bytes used (whole frames) and
  // returns the bytes produced (whole frames).
  uint32_t convert(ConstByteView in, ByteView out, uint32_t* consumed);

  // Bytes of input that fill about `out_bytes` of output (for reporting space).
  uint64_t input_bytes_for(uint64_t out_bytes) const;
  // Bytes of output that `in_bytes` of input will produce.
  uint64_t output_bytes_for(uint64_t in_bytes) const;

 private:
  static constexpr uint32_t kBlockFrames = 128;

  StreamFormat source_, target_;
  Resampler resampler_;
};

}  // namespace audio
