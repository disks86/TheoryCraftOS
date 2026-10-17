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

#include "Media/Audio/Mdct.hpp"
#include "Math/FloatMath.hpp"

namespace media {

namespace {
uint32_t ilog2(uint32_t v) {
  uint32_t n = 0;
  while (v > 1) { ++n; v >>= 1; }
  return n;
}
}  // namespace

void Mdct::release() {
  allocator_.deallocate(memory_);
  allocator_.deallocate(bitrev_);
  memory_ = nullptr;
  bitrev_ = nullptr;
  n_ = 0;
}

bool Mdct::init(uint32_t n) {
  release();
  if (n < 16 || (n & (n - 1)) != 0 || n > (1u << 16)) return false;
  const uint32_t m = n / 2, h = m / 2;
  // pre/post twiddles (h each, x2 parts), FFT twiddles (h/2 + 1, x2), scratch re/im (h each), z (m).
  const uint64_t floats = uint64_t{4} * h + 2 * (h / 2 + 1) + 2 * h + m;
  memory_ = static_cast<float*>(allocator_.allocate(floats * sizeof(float)));
  bitrev_ = static_cast<uint32_t*>(allocator_.allocate(sizeof(uint32_t) * h));
  if (!memory_ || !bitrev_) {
    release();
    return false;
  }
  float* at = memory_;
  pre_re_ = at; at += h;
  pre_im_ = at; at += h;
  post_re_ = at; at += h;
  post_im_ = at; at += h;
  fft_re_ = at; at += h / 2 + 1;
  fft_im_ = at; at += h / 2 + 1;
  re_ = at; at += h;
  im_ = at; at += h;
  z_ = at;
  const double pi = 3.14159265358979323846;
  for (uint32_t i = 0; i < h; ++i) {
    const double a = -pi * (4.0 * i + 1.0) / (4.0 * m), b = -pi * i / m;
    pre_re_[i] = static_cast<float>(fmath::cos(a));
    pre_im_[i] = static_cast<float>(fmath::sin(a));
    post_re_[i] = static_cast<float>(fmath::cos(b));
    post_im_[i] = static_cast<float>(fmath::sin(b));
  }
  for (uint32_t i = 0; i <= h / 2; ++i) {
    const double a = -2.0 * pi * i / h;
    fft_re_[i] = static_cast<float>(fmath::cos(a));
    fft_im_[i] = static_cast<float>(fmath::sin(a));
  }
  const uint32_t bits = ilog2(h);
  for (uint32_t i = 0; i < h; ++i) {
    uint32_t r = 0;
    for (uint32_t k = 0; k < bits; ++k) r |= ((i >> k) & 1u) << (bits - 1 - k);
    bitrev_[i] = r;
  }
  n_ = n;
  return true;
}

void Mdct::inverse(const float* x, float* y) {
  const uint32_t m = n_ / 2, h = m / 2;
  // DCT-IV of x: pair x[2k] with x[m-1-2k], rotate, transform, rotate again, unpack.
  for (uint32_t k = 0; k < h; ++k) {
    const float a = x[2 * k], b = x[m - 1 - 2 * k];
    const uint32_t d = bitrev_[k];
    re_[d] = a * pre_re_[k] - b * pre_im_[k];
    im_[d] = a * pre_im_[k] + b * pre_re_[k];
  }
  for (uint32_t size = 2; size <= h; size <<= 1) {
    const uint32_t half = size / 2, step = h / size;
    for (uint32_t start = 0; start < h; start += size)
      for (uint32_t j = 0; j < half; ++j) {
        const float wr = fft_re_[j * step], wi = fft_im_[j * step];
        const uint32_t u = start + j, v = u + half;
        const float tr = re_[v] * wr - im_[v] * wi, ti = re_[v] * wi + im_[v] * wr;
        re_[v] = re_[u] - tr;
        im_[v] = im_[u] - ti;
        re_[u] += tr;
        im_[u] += ti;
      }
  }
  for (uint32_t k = 0; k < h; ++k) {
    const float ur = re_[k] * post_re_[k] - im_[k] * post_im_[k], ui = re_[k] * post_im_[k] + im_[k] * post_re_[k];
    z_[2 * k] = ur;
    z_[m - 1 - 2 * k] = -ui;
  }
  // The MDCT is the DCT-IV result unfolded to twice the length.
  for (uint32_t i = 0; i < m / 2; ++i) y[i] = z_[m / 2 + i];
  for (uint32_t i = m / 2; i < 3 * m / 2; ++i) y[i] = -z_[3 * m / 2 - 1 - i];
  for (uint32_t i = 3 * m / 2; i < 2 * m; ++i) y[i] = -z_[i - 3 * m / 2];
}

}  // namespace media
