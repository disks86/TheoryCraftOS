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
#include <stddef.h>
#include <stdint.h>
#include "Media/Audio/OpusRange.hpp"

namespace media {
namespace opus {

// The CELT layer of Opus at 48 kHz (the only mode Opus defines): 21 bands, 2.5 to 20 ms frames, float arithmetic as in the reference
// decoder's floating-point build. State is kept in the object (no allocation).
class CeltDecoder {
 public:
  static constexpr int kMaxFrame = 960;      // samples per channel in the longest CELT frame
  static constexpr int kBands = 21;
  static constexpr int kOverlap = 120;
  static constexpr int kDecodeBuffer = 2048;

  CeltDecoder();
  void init(int channels);  // output channels (1 or 2)
  void reset();
  // The first and one past the last band coded (hybrid frames start at 17; the bandwidth sets the end).
  void set_band_range(int start, int end) { start_ = start; end_ = end; }
  void set_stream_channels(int c) { stream_channels_ = c; }
  const float* window() const { return window_; }
  // Decodes one frame of `frame_size` samples per channel (120, 240, 480 or 960) whose bytes `dec` is reading (`len` of them).
  // Writes `frame_size * channels` interleaved samples scaled to +-1. Returns the frame size, or -1 if the frame is invalid.
  int decode(RangeDecoder& dec, int len, float* pcm, int frame_size);

 private:
  struct BandContext;
  void synthesis(const float* X, float* out_syn[2], const float* old_band_e, int start, int eff_end, int C, int CC, bool transient,
                 int LM, bool silence);
  void mdct_backward(const float* in, float* out, int shift, int stride);
  void deemphasis(float* in[2], float* pcm, int N);
  void comb_filter(float* y, float* x, int T0, int T1, int N, float g0, float g1, int tapset0, int tapset1);

  int channels_ = 1, stream_channels_ = 1;
  int start_ = 0, end_ = kBands;
  uint32_t rng_ = 0;
  int postfilter_period_ = 0, postfilter_period_old_ = 0;
  float postfilter_gain_ = 0, postfilter_gain_old_ = 0;
  int postfilter_tapset_ = 0, postfilter_tapset_old_ = 0;
  float preemph_mem_[2] = {};
  bool disable_inv_ = true;
  float decode_mem_[2][kDecodeBuffer + kOverlap] = {};
  float old_band_e_[2 * kBands] = {};
  float old_log_e_[2 * kBands] = {};
  float old_log_e2_[2 * kBands] = {};
  float background_log_e_[2 * kBands] = {};

 public:
  struct Cpx {
    float r, i;
  };

 private:
  // Transform tables (built once per object; they are small)
  struct FftState {
    int nfft;
    float scale;
    int shift;
    int16_t factors[32];
    int16_t bitrev[480];
  };
  void build_tables();
  void fft_impl(const FftState& st, Cpx* fout);
  void bfly2(Cpx* fout, int m, int N);
  void bfly3(Cpx* fout, size_t fstride, const FftState& st, int m, int N, int mm);
  void bfly4(Cpx* fout, size_t fstride, const FftState& st, int m, int N, int mm);
  void bfly5(Cpx* fout, size_t fstride, const FftState& st, int m, int N, int mm);
  FftState fft_[4];
  Cpx twiddles_[480];
  float trig_[1800];
  float window_[kOverlap];
  bool tables_built_ = false;
};

}  // namespace opus
}  // namespace media
