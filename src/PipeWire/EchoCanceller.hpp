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
// No residual echo suppressor or comfort noise follows (what the filter leaves is passed on as it is). Not thread safe.
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

  explicit EchoCanceller(allocators::IAllocator& allocator);
  EchoCanceller(const EchoCanceller&) = delete;
  EchoCanceller& operator=(const EchoCanceller&) = delete;

  // Sizes it: `partitions` blocks of filter (1 .. kMaxPartitions), a bulk delay of up to `max_delay` samples looked for (0: none, the filter alone;
  // at most kMaxDelayLimit). False when a size is out of range or out of memory.
  [[nodiscard]] bool initialize(uint32_t partitions = kDefaultPartitions, uint32_t max_delay = 24000);
  // Starts over: the filter and the delay forgotten.
  void reset();
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
};

}  // namespace pipewire
