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

#include "Hardware/VideoCodec/Software/VideoRateControl.hpp"

namespace hardware {

namespace {

// 16 x the H.264 quantiser step of each QP (it doubles every six).
const uint32_t kQstep16[52] = {10,  11,  13,  14,  16,  18,  20,  23,  25,  29,  32,  36,  40,  45,  51,  57,  64,  72,  81,  91,  102,  114,  128,  144,  161,  181,
                               203, 228, 256, 287, 323, 362, 406, 456, 512, 575, 645, 724, 813, 912, 1024, 1149, 1290, 1448, 1625, 1825, 2048, 2299, 2580, 2896, 3251, 3649};
// The share of the per-picture rate each kind aims at, in halves: intra 4, P 1, B 1/2.
const int64_t kWeight[3] = {8, 2, 1};

uint32_t lo_qp(const RateControlConfig& c) { return (c.flags & kRateControlQpRange) ? c.min_qp : 0; }
uint32_t hi_qp(const RateControlConfig& c) { return (c.flags & kRateControlQpRange) ? c.max_qp : 51; }
uint32_t clamp_qp(const RateControlConfig& c, int64_t qp) {
  const int64_t lo = lo_qp(c), hi = hi_qp(c);
  return static_cast<uint32_t>(qp < lo ? lo : qp > hi ? hi : qp);
}
// Bits per picture at a rate.
int64_t per_picture(const RateControlConfig& c, uint64_t rate) { return static_cast<int64_t>(rate * c.fps_denominator / c.fps_numerator); }
uint64_t fill_rate(const RateControlConfig& c) { return c.mode == RateControlMode::Cbr ? c.target_bitrate : c.peak_bitrate; }
int64_t capacity(const RateControlConfig& c) {
  if ((c.flags & kRateControlVbvSizes) && c.vbv_capacity) return static_cast<int64_t>(c.vbv_capacity);
  return static_cast<int64_t>(fill_rate(c));   // (one second)
}
bool bucket_mode(const RateControlConfig& c) { return c.mode == RateControlMode::Cbr || c.mode == RateControlMode::Vbr || c.mode == RateControlMode::Qvbr; }

void start(const RateControlConfig& c, RateControlState& s) {
  if (s.started) return;
  const int64_t cap = capacity(c);
  s.fullness = (c.flags & kRateControlVbvSizes) && c.vbv_initial_fullness ? static_cast<int64_t>(c.vbv_initial_fullness) : cap / 2;
  if (s.fullness > cap) s.fullness = cap;
  s.started = 1;
}

// What the model expects a picture of `kind` to take at `qp`.
int64_t predicted_bits(const RateControlState& s, uint64_t complexity, uint32_t qp) {
  (void)s;
  return static_cast<int64_t>(complexity / kQstep16[qp]);
}

}  // namespace

bool rate_control_valid(const RateControlConfig& c) {
  if (static_cast<uint8_t>(c.mode) > 4) return false;
  if ((c.flags & kRateControlQpRange) && (c.min_qp > c.max_qp || c.max_qp > 51)) return false;
  if ((c.flags & kRateControlInitialQp) && c.initial_qp > 51) return false;
  if (c.mode == RateControlMode::Cqp) return c.qp_intra <= 51 && c.qp_p <= 51 && c.qp_b <= 51;
  if (c.mode == RateControlMode::AbsoluteQpMap) return true;
  if (c.fps_numerator == 0 || c.fps_denominator == 0 || c.fps_numerator > (1u << 24) || c.fps_denominator > (1u << 24)) return false;
  const uint64_t kMaxRate = uint64_t{1} << 38;
  if (c.target_bitrate == 0 || c.target_bitrate > kMaxRate) return false;
  if (c.mode != RateControlMode::Cbr && (c.peak_bitrate < c.target_bitrate || c.peak_bitrate > kMaxRate)) return false;
  if (c.mode == RateControlMode::Qvbr && (c.quality_target == 0 || c.quality_target > 51)) return false;
  if ((c.flags & kRateControlVbvSizes) && (c.vbv_capacity > kMaxRate * 16 || c.vbv_initial_fullness > c.vbv_capacity)) return false;
  if ((c.flags & kRateControlMaxFrameSize) && c.max_frame_bits == 0) return false;
  return true;
}

uint32_t rate_control_qp(const RateControlConfig& c, RateControlState& s, RateControlPicture kind, uint64_t pixels) {
  const int k = static_cast<int>(kind);
  switch (c.mode) {
    case RateControlMode::Cqp: return k == 0 ? c.qp_intra : k == 1 ? c.qp_p : c.qp_b;
    case RateControlMode::AbsoluteQpMap: return 26;
    default: break;
  }
  start(c, s);
  if (s.pictures == 0 && (c.flags & kRateControlInitialQp)) return clamp_qp(c, c.initial_qp + (k == 0 ? 0 : k == 1 ? 2 : 4));
  // The complexity: the kind's own, else another kind's scaled by the weights, else a guess (0.75 bits a pixel for intra at QP 26).
  uint64_t complexity = s.complexity[k];
  if (!complexity) {
    for (int o = 0; o < 3 && !complexity; ++o)
      if (s.complexity[o]) complexity = s.complexity[o] / static_cast<uint64_t>(kWeight[o]) * static_cast<uint64_t>(kWeight[k]);
    if (!complexity) complexity = pixels * kQstep16[26] * 3 / 4 * static_cast<uint64_t>(kWeight[k]) / static_cast<uint64_t>(kWeight[0]);
  }
  const int64_t rate = per_picture(c, c.target_bitrate);
  int64_t target = rate * kWeight[k] / 2 - s.rate_error / 16;
  if (target < rate / 8) target = rate / 8;
  // the bucket: never aim past what keeps it from emptying
  const int64_t room = s.fullness + per_picture(c, fill_rate(c));
  if (target > room * 7 / 8) target = room * 7 / 8;
  if (target < 1) target = 1;
  if (c.mode == RateControlMode::Qvbr) {
    const uint32_t quality = clamp_qp(c, c.quality_target);
    // (the quality QP stands unless the average is overrun or the bucket would empty)
    const int64_t allowed = s.rate_error > 0 ? target : room * 7 / 8;
    uint32_t qp = quality;
    while (qp < hi_qp(c) && predicted_bits(s, complexity, qp) > allowed) ++qp;
    return qp;
  }
  uint32_t qp = lo_qp(c);
  while (qp < hi_qp(c) && predicted_bits(s, complexity, qp) > target) ++qp;
  return qp;
}

bool rate_control_accept(const RateControlConfig& c, const RateControlState& s, RateControlPicture kind, uint32_t qp, uint64_t bits, uint32_t* retry) {
  (void)kind;
  if (c.mode == RateControlMode::Cqp || c.mode == RateControlMode::AbsoluteQpMap) {
    if (!(c.flags & kRateControlMaxFrameSize) || bits <= c.max_frame_bits || qp >= 51) return true;
  } else {
    bool fits = true;
    if ((c.flags & kRateControlMaxFrameSize) && bits > c.max_frame_bits) fits = false;
    const int64_t room = (s.started ? s.fullness : capacity(c) / 2) + per_picture(c, fill_rate(c));
    if (static_cast<int64_t>(bits) > room) fits = false;
    if (fits || qp >= hi_qp(c)) return true;
  }
  // too big: the step that the model says halves... at least two QPs, more for a bigger overshoot
  uint64_t limit = (c.flags & kRateControlMaxFrameSize) ? c.max_frame_bits : ~uint64_t{0};
  if (bucket_mode(c)) {
    const int64_t room = (s.started ? s.fullness : capacity(c) / 2) + per_picture(c, fill_rate(c));
    if (room > 0 && static_cast<uint64_t>(room) < limit) limit = static_cast<uint64_t>(room);
  }
  uint32_t step = 2;
  for (uint64_t b = bits; limit > 0 && b > limit * 2 && step < 12; b /= 2) step += 6;
  const uint32_t top = (bucket_mode(c) ? hi_qp(c) : 51);
  *retry = qp + step > top ? top : qp + step;
  return false;
}

void rate_control_update(const RateControlConfig& c, RateControlState& s, RateControlPicture kind, uint32_t qp, uint64_t bits) {
  const int k = static_cast<int>(kind);
  ++s.pictures;
  s.coded_bits += bits;
  s.last_qp[k] = qp;
  const uint64_t observed = (bits ? bits : 1) * kQstep16[qp > 51 ? 51 : qp];
  s.complexity[k] = s.complexity[k] ? (s.complexity[k] + 3 * observed) / 4 : observed;
  if (!bucket_mode(c)) return;
  start(c, s);
  s.rate_error += static_cast<int64_t>(bits) - per_picture(c, c.target_bitrate);
  s.fullness += per_picture(c, fill_rate(c)) - static_cast<int64_t>(bits);
  const int64_t cap = capacity(c);
  if (s.fullness > cap) s.fullness = cap;
  if (s.fullness < 0) s.fullness = 0;
}

}  // namespace hardware
