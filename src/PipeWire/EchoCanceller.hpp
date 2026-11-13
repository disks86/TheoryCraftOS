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

// An acoustic echo canceller: takes what the speakers play (the far end, the reference) out of what the microphone hears (the near end), as
// module-echo-cancel does for a call. Mono float samples in blocks of kBlock.
//
// How it works:
//   - a bulk delay first: the far end reaches the microphone late (the output's buffers, the room, the input's buffers: tens to hundreds of
//     milliseconds). Every second the far and near energy envelopes (1 ms steps) are cross-correlated over the last two seconds, and when the peak is
//     clear the far end is delayed by it (less a margin) before the filter sees it;
//   - the echo path left after that (the room's response) is a partitioned-block frequency-domain NLMS filter (overlap-save, an FFT of 2 * kBlock,
//     `partitions` blocks long: 16 is 85 ms at 48 kHz), as Speex's MDF canceller works it, its gradient constrained one partition per block in turn;
//   - two copies of the filter guard against double talk (both ends speaking, which pulls an adapting filter away): a background one adapts all the
//     time, the foreground one makes the output, and the background is copied to the foreground only when its error has stayed clearly lower, or
//     reset from it when it has gone clearly worse.
// After the filter, by Options (all off unless set; module-echo-cancel turns on what webrtc does by default):
//   - high_pass_filter: the microphone through a 2nd-order Butterworth high pass at 80 Hz first (DC and rumble out);
//   - echo_suppression and noise_suppression: a spectral post filter (sqrt-Hann frames of 2 * kBlock, half overlapped: one block more of latency).
//     The residual echo is the echo estimate's power (partly spread over the band, for a loudspeaker's distortion) times a leak, the measured correlation between the error's and the estimate's power spectra,
//     and is suppressed to -30 dB at most, the gaps filled with comfort noise at the noise floor; the noise floor (tracked down quickly and up at
//     about 4 dB a second, on the smoothed power's minimum with its bias made up) is suppressed by a decision-directed Wiener gain to -20 dB at most;
//   - gain_control: a digital gain that brings voice (blocks well above the noise floor) towards -18 dBFS RMS, between -12 and +24 dB, moving
//     slowly, with a limiter that keeps peaks under -0.5 dBFS.
// Samples are taken at 48 kHz (the filter constants are for that rate). Not thread safe.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace pipewire {

class EchoCanceller {
 public:
  static constexpr uint32_t kBlock = 256;              // samples per block (5.3 ms at 48 kHz)
  static constexpr uint32_t kFftSize = 2 * kBlock;
  static constexpr uint32_t kBins = kBlock + 1;
  static constexpr uint32_t kMaxPartitions = 64;
  static constexpr uint32_t kDefaultPartitions = 16;
  static constexpr uint32_t kEnvelopeStep = 48;        // samples per envelope value (1 ms at 48 kHz)
  static constexpr uint32_t kEnvelopeWindow = 2048;    // envelope values correlated (about two seconds)
  static constexpr uint32_t kMaxDelayLimit = 48000;    // the most bulk delay looked for (one second at 48 kHz)

  // What follows the filter (see above).
  struct Options {
    bool high_pass_filter = false;
    bool echo_suppression = false;
    bool noise_suppression = false;
    bool gain_control = false;
  };
  static constexpr float kTargetLevel = 0.0158f;      // the gain control's target: the mean square of -18 dBFS RMS

  explicit EchoCanceller(allocators::IAllocator& allocator);
  EchoCanceller(const EchoCanceller&) = delete;
  EchoCanceller& operator=(const EchoCanceller&) = delete;

  // Sizes it: `partitions` blocks of filter (1 .. kMaxPartitions), a bulk delay of up to `max_delay` samples looked for (0: none, the filter alone;
  // at most kMaxDelayLimit). False when a size is out of range or out of memory.
  [[nodiscard]] bool initialize(uint32_t partitions = kDefaultPartitions, uint32_t max_delay = 24000);
  // Starts over: the filter and the delay forgotten.
  void reset();
  // Sets what follows the filter; the post filter and gain start over.
  void set_options(const Options& options);
  const Options& options() const { return options_; }
  // Samples between a block going in and its own samples coming out: 0, or kBlock with the spectral post filter on.
  uint32_t latency() const { return options_.echo_suppression || options_.noise_suppression ? kBlock : 0; }
  // The voice detector's last decision (the output block was voice: well above the noise floor), and the gain control's gain (linear).
  bool voice() const { return voice_; }
  float gain() const { return agc_gain_; }
  // The residual echo leak the suppressor uses (0 .. 0.5).
  float leak() const { return leak_; }
  // One block: `far` what played, `near` what the microphone heard at the same time, `out` the near end with the echo taken out (each kBlock samples;
  // false otherwise). Samples are full scale at +-1; ones that are not finite are taken as 0.
  [[nodiscard]] bool process(ArrayView<const float> far, ArrayView<const float> near, ArrayView<float> out);

  // The bulk delay in use (samples), and how often it was found.
  uint32_t delay() const { return delay_; }
  uint32_t delay_updates() const { return delay_updates_; }
  // The echo return loss enhancement the output has (the smoothed near energy over the output's while the far end plays, as a ratio: 100 is 20 dB;
  // 1 before anything has played).
  float erle() const;
  // How often the background filter was copied to the foreground, and reset from it.
  uint32_t foreground_updates() const { return foreground_updates_; }
  uint32_t background_resets() const { return background_resets_; }
  uint32_t partitions() const { return partitions_; }

 private:
  struct Complex {
    float re = 0.0f, im = 0.0f;
  };
  void fft(ArrayView<Complex> data, bool inverse) const;
  void estimate_delay();
  void set_delay(uint32_t delay);
  void constrain(uint32_t partition);
  // filter `w` applied to the far spectra: the echo estimate (time, the block's last kBlock samples of the IFFT) into `out`
  void filter(ArrayView<const Complex> w, ArrayView<float> out);
  void reset_post();
  // the post filter on one output block (in place: what comes out is a block late), `echo` the echo estimate for it, `active` the far end playing
  void post_filter(ArrayView<float> block, ArrayView<const float> echo, bool active);
  void gain_control(ArrayView<float> block);

  allocators::IAllocator& allocator_;
  uint32_t partitions_ = 0, max_delay_ = 0, delay_ = 0, delay_updates_ = 0;
  List<Complex> twiddles_;           // kFftSize / 2
  List<uint32_t> reversed_;          // kFftSize: bit-reversed index
  List<float> far_history_;          // the far end, a ring of max_delay + 2 * kBlock samples
  uint64_t far_written_ = 0;         // samples written to it in all
  List<Complex> far_spectra_;        // partitions * kBins: the newest first, a ring
  uint32_t newest_ = 0;
  List<Complex> foreground_, background_;      // partitions * kBins each
  List<float> power_;                // kBins: the far end's smoothed power per bin
  List<Complex> scratch_, scratch2_;           // kFftSize each
  List<float> echo_fg_, echo_bg_;    // kBlock each
  uint32_t constrain_next_ = 0;
  // the energies the filters are judged on (smoothed over blocks)
  float near_energy_ = 0.0f, out_energy_ = 0.0f, fg_error_ = 0.0f, bg_error_ = 0.0f;
  uint32_t better_blocks_ = 0, worse_blocks_ = 0;
  uint32_t foreground_updates_ = 0, background_resets_ = 0;
  // the delay estimator's envelopes (rings of kEnvelopeWindow + the largest lag)
  List<float> far_envelope_, near_envelope_;
  uint64_t envelopes_ = 0;
  float far_acc_ = 0.0f, near_acc_ = 0.0f;
  uint32_t acc_count_ = 0;
  uint32_t since_estimate_ = 0;
  // after the filter
  Options options_;
  float hp_x1_ = 0.0f, hp_x2_ = 0.0f, hp_y1_ = 0.0f, hp_y2_ = 0.0f;
  List<float> window_;               // kFftSize: sqrt-Hann (analysis and synthesis)
  List<float> previous_error_, previous_echo_, overlap_;      // kBlock each
  List<Complex> error_spectrum_, echo_spectrum_;              // kFftSize each
  List<float> noise_, smoothed_, previous_gain_, previous_snr_;      // kBins each
  float cov_ey_ = 0.0f, var_yy_ = 0.0f, leak_ = 0.0f;
  bool noise_started_ = false;
  uint32_t random_ = 0x1234567u;
  float agc_gain_ = 1.0f, agc_level_ = 0.0f, block_noise_ = 0.0f;
  bool voice_ = false;
};

}  // namespace pipewire
