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
#include "Allocators/IAllocator.hpp"
#include <stdint.h>

namespace media {

// An inverse MDCT in the definition shared by Vorbis and AAC: n/2 coefficients X[k] in, n samples out,
//   y[i] = sum over k of X[k] * cos(2 pi / n * (i + 1/2 + n/4) * (k + 1/2)),
// unscaled (callers apply their format's factor). It runs as a DCT-IV through a complex FFT of n/4 points. `n` is a power of
// two from 16 up.
class Mdct {
 public:
  explicit Mdct(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Mdct() { release(); }
  Mdct(const Mdct&) = delete;
  Mdct& operator=(const Mdct&) = delete;

  bool init(uint32_t n);
  uint32_t size() const { return n_; }
  // `x` has size()/2 values, `y` size() values. Not thread safe (it keeps scratch space).
  void inverse(const float* x, float* y);

 private:
  void release();

  allocators::IAllocator& allocator_;
  uint32_t n_ = 0;
  float* memory_ = nullptr;
  uint32_t* bitrev_ = nullptr;
  float *pre_re_ = nullptr, *pre_im_ = nullptr, *post_re_ = nullptr, *post_im_ = nullptr, *fft_re_ = nullptr, *fft_im_ = nullptr;
  float *re_ = nullptr, *im_ = nullptr, *z_ = nullptr;
};

}  // namespace media
