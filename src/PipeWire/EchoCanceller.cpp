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

#include "PipeWire/EchoCanceller.hpp"

namespace pipewire {

namespace {
constexpr float kStep = 0.5f;                   // the background filter's normalized step
constexpr float kPowerSmoothing = 0.7f;         // how much of the far power per bin is kept each block
constexpr float kEnergySmoothing = 0.8f;        // the same for the energies the filters are judged on
constexpr float kActiveFloor = 1e-6f;           // mean square of a far block that counts as playing (-60 dBFS)
constexpr float kRegularization = 1e-6f;        // added to the far power per bin, per FFT point
constexpr uint32_t kDecisionBlocks = 3;         // blocks a filter must stay better (or worse) before it is copied
constexpr uint32_t kDelayMargin = 384;          // samples of the measured delay left to the filter (8 ms at 48 kHz)
constexpr uint32_t kDelayTolerance = 144;       // a new delay closer than this to the one in use is not taken (3 ms)
constexpr uint32_t kEstimateEvery = 1000;       // envelope values between delay estimates (a second)

float finite_or_zero(float v) {
  if (!((v - v) == 0.0f)) return 0.0f;      // (NaN or infinite)
  return v > 4.0f ? 4.0f : v < -4.0f ? -4.0f : v;
}

template <class T>
bool sized(List<T>& list, uint64_t count) {
  list.clear();
  if (!list.reserve(count)) return false;
  for (uint64_t i = 0; i < count; ++i)
    if (!list.push_back(T{})) return false;
  return true;
}

float magnitude_squared(float re, float im) { return re * re + im * im; }

// A square root without the C library (Newton's method from a guess by halving the exponent; within a part in 10^7 after four steps).
float root(float x) {
  if (!(x > 0.0f)) return 0.0f;
  uint32_t bits;
  __builtin_memcpy(&bits, &x, 4);
  bits = (bits >> 1) + 0x1FC00000u;
  float r;
  __builtin_memcpy(&r, &bits, 4);
  for (int k = 0; k < 4; ++k) r = 0.5f * (r + x / r);
  return r;
}
float clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// the high pass: 80 Hz, Q 1/sqrt(2), at 48 kHz (RBJ's biquad)
constexpr float kHpB0 = 0.9926225427561189f, kHpB1 = -1.9852450855122379f, kHpB2 = 0.9926225427561189f;
constexpr float kHpA1 = -1.9851906578962613f, kHpA2 = 0.9852995131282146f;
// the post filter
constexpr float kEchoFloor = 0.0316f;           // the most the residual echo is suppressed (-30 dB in amplitude)
constexpr float kNoiseFloor = 0.1f;             // the most noise is suppressed (-20 dB)
constexpr float kOverSuppression = 4.0f;        // the residual echo taken as this much louder than estimated (it is better gone)
constexpr float kPowerSmoothingPost = 0.85f;    // how much of the error's smoothed power per bin is kept each block (the noise floor follows that)
constexpr float kNoiseBias = 2.0f;              // the smoothed power's minimum sits about this far under the noise's mean
constexpr float kNoiseRise = 1.005f;            // the noise floor's rise per block (about 4 dB a second)
constexpr float kNoiseFall = 0.3f;              // how much of a lower power the floor takes at once
constexpr float kLeakSmoothing = 0.95f;
// the gain control
constexpr float kMinGain = 0.25f, kMaxGain = 16.0f, kGainStep = 0.02f, kPeak = 0.944f;
}  // namespace

EchoCanceller::EchoCanceller(allocators::IAllocator& allocator)
    : allocator_(allocator), twiddles_(allocator), reversed_(allocator), far_history_(allocator), far_spectra_(allocator), foreground_(allocator),
      background_(allocator), power_(allocator), scratch_(allocator), scratch2_(allocator), echo_fg_(allocator), echo_bg_(allocator),
      far_envelope_(allocator), near_envelope_(allocator), window_(allocator), previous_error_(allocator), previous_echo_(allocator), overlap_(allocator),
      error_spectrum_(allocator), echo_spectrum_(allocator), noise_(allocator), smoothed_(allocator), previous_gain_(allocator), previous_snr_(allocator) {}

bool EchoCanceller::initialize(uint32_t partitions, uint32_t max_delay) {
  if (partitions < 1 || partitions > kMaxPartitions || max_delay > kMaxDelayLimit) return false;
  partitions_ = partitions;
  max_delay_ = max_delay;
  const uint64_t lags = max_delay / kEnvelopeStep + 1;
  if (!sized(twiddles_, kFftSize / 2) || !sized(reversed_, kFftSize) || !sized(far_history_, uint64_t{max_delay} + 2 * kFftSize) ||
      !sized(far_spectra_, uint64_t{partitions} * kBins) || !sized(foreground_, uint64_t{partitions} * kBins) ||
      !sized(background_, uint64_t{partitions} * kBins) || !sized(power_, kBins) || !sized(scratch_, kFftSize) || !sized(scratch2_, kFftSize) ||
      !sized(echo_fg_, kBlock) || !sized(echo_bg_, kBlock) || !sized(far_envelope_, kEnvelopeWindow + lags) ||
      !sized(near_envelope_, kEnvelopeWindow + lags) || !sized(window_, kFftSize) || !sized(previous_error_, kBlock) || !sized(previous_echo_, kBlock) ||
      !sized(overlap_, kBlock) || !sized(error_spectrum_, kFftSize) || !sized(echo_spectrum_, kFftSize) || !sized(noise_, kBins) || !sized(smoothed_, kBins) ||
      !sized(previous_gain_, kBins) || !sized(previous_snr_, kBins))
    return false;
  // sqrt-Hann: sin(pi n / N), by rotation in double (cos and sin of pi / 512); its squares of half-overlapped frames add up to 1
  {
    const double step_cos = 0.9999811752826011, step_sin = 0.006135884649154475;
    double c = 1.0, sn = 0.0;
    for (float& w : window_.view()) {
      w = static_cast<float>(sn);
      const double nc = c * step_cos - sn * step_sin;
      sn = sn * step_cos + c * step_sin;
      c = nc;
    }
  }
  // e^(-2 pi i k / N), by rotation in double (cos and sin of 2 pi / 512)
  static_assert(kFftSize == 512, "the rotation below is 2 pi / 512");
  const double step_cos = 0.9999247018391445, step_sin = 0.012271538285719925;
  double c = 1.0, s = 0.0;
  for (Complex& t : twiddles_.view()) {
    t.re = static_cast<float>(c);
    t.im = static_cast<float>(-s);
    const double nc = c * step_cos - s * step_sin;
    s = s * step_cos + c * step_sin;
    c = nc;
  }
  uint32_t bits = 0;
  while ((1u << bits) < kFftSize) ++bits;
  for (uint32_t i = 0; i < kFftSize; ++i) {
    uint32_t r = 0;
    for (uint32_t b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
    reversed_[i] = r;
  }
  reset();
  return true;
}

void EchoCanceller::reset() {
  for (float& v : far_history_.view()) v = 0.0f;
  for (Complex& v : far_spectra_.view()) v = Complex{};
  for (Complex& v : foreground_.view()) v = Complex{};
  for (Complex& v : background_.view()) v = Complex{};
  for (float& v : power_.view()) v = 0.0f;
  for (float& v : far_envelope_.view()) v = 0.0f;
  for (float& v : near_envelope_.view()) v = 0.0f;
  far_written_ = 0;
  newest_ = 0;
  constrain_next_ = 0;
  delay_ = 0;
  near_energy_ = out_energy_ = fg_error_ = bg_error_ = 0.0f;
  better_blocks_ = worse_blocks_ = 0;
  envelopes_ = 0;
  far_acc_ = near_acc_ = 0.0f;
  acc_count_ = since_estimate_ = 0;
  reset_post();
}

void EchoCanceller::reset_post() {
  hp_x1_ = hp_x2_ = hp_y1_ = hp_y2_ = 0.0f;
  for (float& v : previous_error_.view()) v = 0.0f;
  for (float& v : previous_echo_.view()) v = 0.0f;
  for (float& v : overlap_.view()) v = 0.0f;
  for (float& v : noise_.view()) v = 0.0f;
  for (float& v : smoothed_.view()) v = 0.0f;
  for (float& v : previous_gain_.view()) v = 1.0f;
  for (float& v : previous_snr_.view()) v = 0.0f;
  cov_ey_ = var_yy_ = leak_ = 0.0f;
  noise_started_ = false;
  agc_gain_ = 1.0f;
  agc_level_ = block_noise_ = 0.0f;
  voice_ = false;
}

void EchoCanceller::set_options(const Options& options) {
  options_ = options;
  reset_post();
}

void EchoCanceller::post_filter(ArrayView<float> block, ArrayView<const float> echo, bool active) {
  // the frame: the previous block and this one, windowed, for the error and for the echo estimate
  ArrayView<Complex> e = error_spectrum_.view(), y = echo_spectrum_.view();
  for (uint32_t i = 0; i < kFftSize; ++i) {
    const float ev = i < kBlock ? previous_error_[i] : block[i - kBlock], yv = i < kBlock ? previous_echo_[i] : echo[i - kBlock];
    e[i] = Complex{ev * window_[i], 0.0f};
    y[i] = Complex{yv * window_[i], 0.0f};
  }
  for (uint32_t i = 0; i < kBlock; ++i) {
    previous_error_[i] = block[i];
    previous_echo_[i] = echo[i];
  }
  fft(e, false);
  fft(y, false);
  // the leak: how the error's power spectrum follows the echo estimate's (the residual echo), measured while the far end plays
  if (options_.echo_suppression && active) {
    float mean_e = 0.0f, mean_y = 0.0f;
    for (uint32_t k = 0; k < kBins; ++k) {
      mean_e += magnitude_squared(e[k].re, e[k].im);
      mean_y += magnitude_squared(y[k].re, y[k].im);
    }
    mean_e /= kBins;
    mean_y /= kBins;
    float cov = 0.0f, var = 0.0f;
    for (uint32_t k = 0; k < kBins; ++k) {
      const float de = magnitude_squared(e[k].re, e[k].im) - mean_e, dy = magnitude_squared(y[k].re, y[k].im) - mean_y;
      cov += de * dy;
      var += dy * dy;
    }
    cov_ey_ = kLeakSmoothing * cov_ey_ + (1.0f - kLeakSmoothing) * cov;
    var_yy_ = kLeakSmoothing * var_yy_ + (1.0f - kLeakSmoothing) * var;
    leak_ = var_yy_ > 1e-20f ? clamp(cov_ey_ / var_yy_, 0.005f, 0.5f) : 0.005f;
  }
  // the echo estimate's mean power: a loudspeaker's distortion puts residual echo where the estimate itself has little (harmonics), so part of the
  // residual is taken as spread over the band
  float echo_mean = 0.0f;
  for (uint32_t k = 0; k < kBins; ++k) echo_mean += magnitude_squared(y[k].re, y[k].im);
  echo_mean /= kBins;
  // the gains
  ArrayView<Complex> out = scratch2_.view();
  const float floor = options_.noise_suppression ? kNoiseFloor : 1.0f;
  for (uint32_t k = 0; k < kBins; ++k) {
    const float power = magnitude_squared(e[k].re, e[k].im) + 1e-12f;
    const float residual = options_.echo_suppression && active ? leak_ * (0.7f * magnitude_squared(y[k].re, y[k].im) + 0.3f * echo_mean) : 0.0f;
    // the noise floor: the minimum of the smoothed power, followed down at once and up slowly (not while the residual echo is most of the power: it
    // would be taken for noise), its bias made up
    smoothed_[k] = noise_started_ ? kPowerSmoothingPost * smoothed_[k] + (1.0f - kPowerSmoothingPost) * power : power;
    if (!noise_started_) noise_[k] = smoothed_[k];
    else if (residual < 0.5f * power) noise_[k] = smoothed_[k] < noise_[k] ? smoothed_[k] : noise_[k] * kNoiseRise;
    const float noise = kNoiseBias * noise_[k];
    float gain = 1.0f;
    if (options_.echo_suppression && residual > 0.0f) gain = clamp(1.0f - kOverSuppression * residual / power, kEchoFloor, 1.0f);
    float echo_gain = gain;
    if (options_.noise_suppression) {
      // decision-directed a priori SNR (Ephraim and Malah), Wiener gain
      const float posterior = power / (noise + 1e-12f);
      const float prior = 0.98f * previous_gain_[k] * previous_gain_[k] * previous_snr_[k] + 0.02f * (posterior > 1.0f ? posterior - 1.0f : 0.0f);
      const float wiener = clamp(prior / (1.0f + prior), kNoiseFloor, 1.0f);
      previous_gain_[k] = wiener;
      previous_snr_[k] = posterior;
      if (wiener < gain) gain = wiener;
    }
    out[k] = Complex{e[k].re * gain, e[k].im * gain};
    // comfort noise where the echo was cut away: the noise floor (after its own suppression), at a random phase
    if (options_.echo_suppression && echo_gain < 1.0f) {
      const float level = root(noise) * floor * (1.0f - echo_gain) * 0.7071f;
      random_ = random_ * 1664525u + 1013904223u;
      const float re = static_cast<float>(static_cast<int32_t>(random_)) / 2147483648.0f;
      random_ = random_ * 1664525u + 1013904223u;
      const float im = static_cast<float>(static_cast<int32_t>(random_)) / 2147483648.0f;
      out[k].re += level * re;
      out[k].im += level * im;
    }
  }
  noise_started_ = true;
  out[0].im = 0.0f;
  out[kBlock].im = 0.0f;
  for (uint32_t k = 1; k < kBlock; ++k) {
    out[kFftSize - k].re = out[k].re;
    out[kFftSize - k].im = -out[k].im;
  }
  fft(out, true);
  // overlap-add: the first half with what the last frame left is the previous block, done
  for (uint32_t i = 0; i < kBlock; ++i) {
    block[i] = overlap_[i] + out[i].re * window_[i];
    overlap_[i] = out[kBlock + i].re * window_[kBlock + i];
  }
}

void EchoCanceller::gain_control(ArrayView<float> block) {
  float mean_square = 0.0f, peak = 0.0f;
  for (float v : block) {
    mean_square += v * v;
    const float a = v < 0.0f ? -v : v;
    if (a > peak) peak = a;
  }
  mean_square /= kBlock;
  // the voice detector: a block well above the floor of the blocks' powers (that floor tracked as the noise floor is)
  if (block_noise_ <= 0.0f) block_noise_ = mean_square;
  else block_noise_ = mean_square < block_noise_ ? (1.0f - kNoiseFall) * block_noise_ + kNoiseFall * mean_square
                                                 : (block_noise_ * kNoiseRise < mean_square ? block_noise_ * kNoiseRise : mean_square);
  voice_ = mean_square > 4.0f * block_noise_ + 1e-9f;
  if (voice_) {
    agc_level_ = agc_level_ <= 0.0f ? mean_square : 0.95f * agc_level_ + 0.05f * mean_square;
    const float wanted = clamp(root(kTargetLevel / (agc_level_ + 1e-12f)), kMinGain, kMaxGain);
    agc_gain_ += (wanted - agc_gain_) * kGainStep;
  }
  float gain = agc_gain_;
  if (peak * gain > kPeak) gain = kPeak / peak;      // (the limiter: this block's peak kept under -0.5 dBFS)
  for (float& v : block) v *= gain;
}

float EchoCanceller::erle() const {
  if (near_energy_ <= 0.0f) return 1.0f;
  return near_energy_ / (out_energy_ > 1e-12f ? out_energy_ : 1e-12f);
}

void EchoCanceller::fft(ArrayView<Complex> data, bool inverse) const {
  // iterative radix 2, in place (data is kFftSize long)
  for (uint32_t i = 0; i < kFftSize; ++i) {
    const uint32_t r = reversed_[i];
    if (r > i) {
      const Complex t = data[i];
      data[i] = data[r];
      data[r] = t;
    }
  }
  for (uint32_t size = 2; size <= kFftSize; size *= 2) {
    const uint32_t half = size / 2, stride = kFftSize / size;
    for (uint32_t start = 0; start < kFftSize; start += size)
      for (uint32_t k = 0; k < half; ++k) {
        Complex w = twiddles_[k * stride];
        if (inverse) w.im = -w.im;
        Complex& a = data[start + k];
        Complex& b = data[start + k + half];
        const float tr = b.re * w.re - b.im * w.im, ti = b.re * w.im + b.im * w.re;
        b.re = a.re - tr;
        b.im = a.im - ti;
        a.re += tr;
        a.im += ti;
      }
  }
  if (inverse)
    for (Complex& v : data) {
      v.re *= 1.0f / kFftSize;
      v.im *= 1.0f / kFftSize;
    }
}

void EchoCanceller::filter(ArrayView<const Complex> w, ArrayView<float> out) {
  // Y = sum over partitions of W_p X_(newest - p); y is the last kBlock samples of its inverse (overlap-save)
  ArrayView<Complex> y = scratch_.view();
  for (Complex& v : y) v = Complex{};
  for (uint32_t p = 0; p < partitions_; ++p) {
    const uint32_t slot = (newest_ + partitions_ - p) % partitions_;
    const ArrayView<const Complex> x = far_spectra_.view().subview(uint64_t{slot} * kBins, kBins);
    const ArrayView<const Complex> wp = w.subview(uint64_t{p} * kBins, kBins);
    for (uint32_t k = 0; k < kBins; ++k) {
      y[k].re += wp[k].re * x[k].re - wp[k].im * x[k].im;
      y[k].im += wp[k].re * x[k].im + wp[k].im * x[k].re;
    }
  }
  for (uint32_t k = 1; k < kBlock; ++k) {      // (a real signal's spectrum: the upper half mirrors the lower)
    y[kFftSize - k].re = y[k].re;
    y[kFftSize - k].im = -y[k].im;
  }
  fft(y, true);
  for (uint32_t i = 0; i < kBlock; ++i) out[i] = y[kBlock + i].re;
}

void EchoCanceller::constrain(uint32_t partition) {
  // the gradient constraint: the partition's filter made kBlock taps long again (its time-domain second half zeroed)
  ArrayView<Complex> w = background_.view().subview(uint64_t{partition} * kBins, kBins);
  ArrayView<Complex> t = scratch2_.view();
  for (uint32_t k = 0; k < kBins; ++k) t[k] = w[k];
  for (uint32_t k = 1; k < kBlock; ++k) {
    t[kFftSize - k].re = w[k].re;
    t[kFftSize - k].im = -w[k].im;
  }
  fft(t, true);
  for (uint32_t i = kBlock; i < kFftSize; ++i) t[i] = Complex{};
  for (uint32_t i = 0; i < kBlock; ++i) t[i].im = 0.0f;
  fft(t, false);
  for (uint32_t k = 0; k < kBins; ++k) w[k] = t[k];
}

void EchoCanceller::set_delay(uint32_t delay) {
  if (delay > max_delay_) delay = max_delay_;
  delay_ = delay;
  ++delay_updates_;
  // the filters were fitted to the old alignment: they start over (the far spectra are rebuilt as blocks come)
  for (Complex& v : foreground_.view()) v = Complex{};
  for (Complex& v : background_.view()) v = Complex{};
  for (Complex& v : far_spectra_.view()) v = Complex{};
  fg_error_ = bg_error_ = 0.0f;
  better_blocks_ = worse_blocks_ = 0;
}

void EchoCanceller::estimate_delay() {
  // the lag (in envelope steps) at which the far envelope best matches the near one over the last kEnvelopeWindow values
  const uint64_t ring = far_envelope_.count();
  const uint64_t lags = ring - kEnvelopeWindow;
  if (envelopes_ < ring) return;
  auto at = [&](const List<float>& e, uint64_t back) { return e[(envelopes_ - 1 - back) % ring]; };
  float near_mean = 0.0f;
  for (uint64_t i = 0; i < kEnvelopeWindow; ++i) near_mean += at(near_envelope_, i);
  near_mean /= kEnvelopeWindow;
  float near_var = 0.0f;
  for (uint64_t i = 0; i < kEnvelopeWindow; ++i) {
    const float d = at(near_envelope_, i) - near_mean;
    near_var += d * d;
  }
  if (near_var <= 1e-12f) return;
  float best = 0.0f;
  uint64_t best_lag = 0;
  bool found = false;
  for (uint64_t lag = 0; lag < lags; ++lag) {
    float far_mean = 0.0f;
    for (uint64_t i = 0; i < kEnvelopeWindow; ++i) far_mean += at(far_envelope_, i + lag);
    far_mean /= kEnvelopeWindow;
    float cross = 0.0f, far_var = 0.0f;
    for (uint64_t i = 0; i < kEnvelopeWindow; ++i) {
      const float f = at(far_envelope_, i + lag) - far_mean, n = at(near_envelope_, i) - near_mean;
      cross += f * n;
      far_var += f * f;
    }
    if (cross <= 0.0f || far_var <= 1e-12f) continue;
    const float score = cross * cross / (far_var * near_var);      // (the correlation squared)
    if (score > best) {
      best = score;
      best_lag = lag;
      found = true;
    }
  }
  if (!found || best < 0.25f) return;      // (a correlation under 0.5: no clear echo, the delay stays)
  const uint64_t samples = best_lag * kEnvelopeStep;
  const uint32_t target = samples > kDelayMargin ? static_cast<uint32_t>(samples - kDelayMargin) : 0;
  const uint32_t difference = target > delay_ ? target - delay_ : delay_ - target;
  if (difference >= kDelayTolerance) set_delay(target);
}

bool EchoCanceller::process(ArrayView<const float> far_in, ArrayView<const float> near_in, ArrayView<float> out) {
  if (partitions_ == 0 || far_in.size() != kBlock || near_in.size() != kBlock || out.size() != kBlock) return false;
  // the far end into its history; both into the delay estimator's envelopes
  const uint64_t ring = far_history_.count();
  float far_energy = 0.0f;
  // the microphone, high-passed when asked
  float near_block[kBlock];
  for (uint32_t i = 0; i < kBlock; ++i) {
    const float x = finite_or_zero(near_in[i]);
    if (!options_.high_pass_filter) {
      near_block[i] = x;
      continue;
    }
    const float yv = kHpB0 * x + kHpB1 * hp_x1_ + kHpB2 * hp_x2_ - kHpA1 * hp_y1_ - kHpA2 * hp_y2_;
    hp_x2_ = hp_x1_;
    hp_x1_ = x;
    hp_y2_ = hp_y1_;
    hp_y1_ = (yv - yv) == 0.0f ? yv : 0.0f;
    near_block[i] = hp_y1_;
  }
  for (uint32_t i = 0; i < kBlock; ++i) {
    const float f = finite_or_zero(far_in[i]), n = near_block[i];
    far_history_[(far_written_ + i) % ring] = f;
    far_acc_ += f < 0.0f ? -f : f;
    near_acc_ += n < 0.0f ? -n : n;
    if (++acc_count_ == kEnvelopeStep) {
      const uint64_t envelope_ring = far_envelope_.count();
      far_envelope_[envelopes_ % envelope_ring] = far_acc_ / kEnvelopeStep;
      near_envelope_[envelopes_ % envelope_ring] = near_acc_ / kEnvelopeStep;
      ++envelopes_;
      far_acc_ = near_acc_ = 0.0f;
      acc_count_ = 0;
      if (max_delay_ && ++since_estimate_ >= kEstimateEvery) {
        since_estimate_ = 0;
        estimate_delay();
      }
    }
  }
  far_written_ += kBlock;
  // the delayed far end's last two blocks, transformed: the newest partition
  ArrayView<Complex> x = scratch_.view();
  for (uint32_t i = 0; i < kFftSize; ++i) {
    const uint64_t back = uint64_t{delay_} + kFftSize - i;      // (samples before the newest written)
    const float v = back <= far_written_ ? far_history_[(far_written_ - back) % ring] : 0.0f;
    x[i] = Complex{v, 0.0f};
    if (i >= kBlock) far_energy += v * v;
  }
  fft(x, false);
  newest_ = (newest_ + 1) % partitions_;
  ArrayView<Complex> newest = far_spectra_.view().subview(uint64_t{newest_} * kBins, kBins);
  for (uint32_t k = 0; k < kBins; ++k) {
    newest[k] = x[k];
    power_[k] = kPowerSmoothing * power_[k] + (1.0f - kPowerSmoothing) * magnitude_squared(x[k].re, x[k].im);
  }
  far_energy /= kBlock;
  const bool active = far_energy > kActiveFloor;
  // both filters' echo estimates and errors
  filter(foreground_.view(), echo_fg_.view());
  filter(background_.view(), echo_bg_.view());
  float near_energy = 0.0f, fg_error = 0.0f, bg_error = 0.0f;
  ArrayView<Complex> e = scratch2_.view();
  for (uint32_t i = 0; i < kBlock; ++i) {
    const float d = near_block[i];
    const float ef = d - echo_fg_[i], eb = d - echo_bg_[i];
    out[i] = ef;
    near_energy += d * d;
    fg_error += ef * ef;
    bg_error += eb * eb;
    e[i] = Complex{};
    e[kBlock + i] = Complex{eb, 0.0f};
  }
  // the background adapts on its own error: W_p += mu conj(X_p) E / (P * power + delta)
  if (active) {
    fft(e, false);
    const float delta = kRegularization * kFftSize;
    for (uint32_t p = 0; p < partitions_; ++p) {
      const uint32_t slot = (newest_ + partitions_ - p) % partitions_;
      const ArrayView<const Complex> xp = far_spectra_.view().subview(uint64_t{slot} * kBins, kBins);
      ArrayView<Complex> wp = background_.view().subview(uint64_t{p} * kBins, kBins);
      for (uint32_t k = 0; k < kBins; ++k) {
        const float g = kStep / (partitions_ * power_[k] + delta);
        wp[k].re += g * (xp[k].re * e[k].re + xp[k].im * e[k].im);
        wp[k].im += g * (xp[k].re * e[k].im - xp[k].im * e[k].re);
      }
    }
    constrain(constrain_next_);
    constrain_next_ = (constrain_next_ + 1) % partitions_;
  }
  // judge the two filters
  if (active) {
    near_energy_ = kEnergySmoothing * near_energy_ + (1.0f - kEnergySmoothing) * near_energy;
    out_energy_ = kEnergySmoothing * out_energy_ + (1.0f - kEnergySmoothing) * fg_error;
    fg_error_ = kEnergySmoothing * fg_error_ + (1.0f - kEnergySmoothing) * fg_error;
    bg_error_ = kEnergySmoothing * bg_error_ + (1.0f - kEnergySmoothing) * bg_error;
    better_blocks_ = bg_error_ < 0.7f * fg_error_ ? better_blocks_ + 1 : 0;
    worse_blocks_ = bg_error_ > 4.0f * fg_error_ + 1e-9f ? worse_blocks_ + 1 : 0;
    if (better_blocks_ >= kDecisionBlocks) {
      for (uint64_t i = 0; i < foreground_.count(); ++i) foreground_[i] = background_[i];
      fg_error_ = bg_error_;
      better_blocks_ = 0;
      ++foreground_updates_;
    } else if (worse_blocks_ >= kDecisionBlocks && foreground_updates_) {
      for (uint64_t i = 0; i < background_.count(); ++i) background_[i] = foreground_[i];
      bg_error_ = fg_error_;
      worse_blocks_ = 0;
      ++background_resets_;
    }
  }
  // what follows the filter
  if (options_.echo_suppression || options_.noise_suppression) post_filter(out, echo_fg_.view(), active);
  if (options_.gain_control) gain_control(out);
  return true;
}

}  // namespace pipewire
