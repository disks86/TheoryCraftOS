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

// Picture-level rate control for the video encoders (Direct3D 12's D3D12_VIDEO_ENCODER_RATE_CONTROL modes). It picks the QP of each picture; a picture that comes out too
// big for the buffer or the frame size limit is coded again with a higher QP (up to kMaxRetries times). Integer arithmetic only (kernel code).
//
//   CQP     the QP of the picture's type (intra, P, B), as given.
//   CBR     a leaky bucket filled at TargetBitRate / frame rate per picture (capacity VBVCapacity, starting at InitialVBVFullness when ENABLE_VBV_SIZES is set, else one second
//           of the rate, half full): each picture aims at the per-picture rate weighted by type (intra 4, P 1, B 1/2) and corrected by the bits it is ahead or behind, the QP
//           from a complexity model (bits x quantiser step of the last picture of the type); a picture that would empty the bucket is coded again. The bucket's overflow is
//           not filled with stuffing data (the stream is at most, not exactly, the rate).
//   VBR     the same aiming at TargetAvgBitRate, the bucket filled at PeakBitRate (so pictures may be big as long as the peak rate holds).
//   QVBR    the QP is ConstantQualityTarget, raised only as far as needed to keep TargetAvgBitRate (over the pictures so far) and the peak rate's bucket.
//   flags   ENABLE_QP_RANGE (MinQP .. MaxQP bound every choice), ENABLE_INITIAL_QP (the first picture's QP), ENABLE_MAX_FRAME_SIZE (MaxFrameBitSize is a limit: coded
//           again until under it or at MaxQP), ENABLE_VBV_SIZES (the bucket's size and start). ENABLE_DELTA_QP is the encoder's (a QP map added to the picture's QP).
//   ABSOLUTE_QP_MAP mode: the picture's QPs are the map's; nothing to choose.
#pragma once
#include <stdint.h>

namespace hardware {

enum class RateControlMode : uint8_t { AbsoluteQpMap = 0, Cqp = 1, Cbr = 2, Vbr = 3, Qvbr = 4 };
enum RateControlFlags : uint32_t {
  kRateControlDeltaQp = 0x1, kRateControlFrameAnalysis = 0x2, kRateControlQpRange = 0x4, kRateControlInitialQp = 0x8, kRateControlMaxFrameSize = 0x10,
  kRateControlVbvSizes = 0x20,
};
// The kinds of pictures the model keeps apart.
enum class RateControlPicture : uint8_t { Intra = 0, P = 1, B = 2 };

struct RateControlConfig {
  RateControlMode mode = RateControlMode::Cqp;
  uint32_t flags = 0;
  uint32_t fps_numerator = 30, fps_denominator = 1;
  uint32_t qp_intra = 26, qp_p = 28, qp_b = 30;      // CQP
  uint32_t initial_qp = 26, min_qp = 0, max_qp = 51;
  uint64_t target_bitrate = 0, peak_bitrate = 0;     // bits per second
  uint64_t vbv_capacity = 0, vbv_initial_fullness = 0, max_frame_bits = 0;
  uint32_t quality_target = 26;                      // QVBR
};

// What is kept from picture to picture (Direct3D 12 keeps it in the encoder heap).
struct RateControlState {
  uint64_t pictures = 0;
  int64_t fullness = 0;              // bits in the bucket
  int64_t rate_error = 0;            // bits coded minus bits the rate gave so far
  uint64_t coded_bits = 0;
  uint64_t complexity[3] = {};       // bits x quantiser step x 16 of the last picture of each kind (0: none yet)
  uint32_t last_qp[3] = {};
  uint32_t started = 0;              // the bucket was set up
};

constexpr uint32_t kRateControlMaxRetries = 4;

// Whether the configuration is one the controller takes (rates and frame rate nonzero where the mode needs them, QPs in range, the range ordered).
[[nodiscard]] bool rate_control_valid(const RateControlConfig& config);
// The QP to code the next picture of `kind` with (`pixels` luma samples).
[[nodiscard]] uint32_t rate_control_qp(const RateControlConfig& config, RateControlState& state, RateControlPicture kind, uint64_t pixels);
// Whether a picture coded with `qp` in `bits` may stand; if not, *retry is the QP to code it again with.
[[nodiscard]] bool rate_control_accept(const RateControlConfig& config, const RateControlState& state, RateControlPicture kind, uint32_t qp, uint64_t bits, uint32_t* retry);
// The picture was kept: the model and the bucket learn from it.
void rate_control_update(const RateControlConfig& config, RateControlState& state, RateControlPicture kind, uint32_t qp, uint64_t bits);

}  // namespace hardware
