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


#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"

namespace hardware {

namespace {
struct Training {
  IDpcdAux& aux;
  IDpSource& source;
  const Dpcd::Capabilities& caps;
  uint32_t lanes;
  uint8_t train_set[4] = {};
  Dpcd::LinkStatus status;
};

// The pattern on the source and then on the sink (amdgpu_atombios_dp_set_tp)
bool set_pattern(Training& t, uint8_t pattern) {
  if (!t.source.set_pattern(pattern)) return false;
  return t.aux.dpcd_write(Dpcd::kTrainingPatternSet, ConstByteView(&pattern, 1));
}

// The drive on the source (all lanes by the first lane's set) and then on the sink, for each lane (amdgpu_atombios_dp_update_vs_emph)
bool update_drive(Training& t) {
  if (!t.source.set_drive(t.train_set[0])) return false;
  return t.aux.dpcd_write(Dpcd::kTrainingLane0Set, ConstByteView(t.train_set, t.lanes));
}

// dp_get_adjust_train: the highest voltage swing and pre-emphasis any lane is asked for, for every lane, with the "max reached" flags once the top level is asked
void get_adjust_train(Training& t) {
  uint8_t v = 0, p = 0;
  for (uint32_t lane = 0; lane < t.lanes; ++lane) {
    const uint8_t this_v = t.status.adjust_voltage(lane), this_p = t.status.adjust_pre_emphasis(lane);
    if (this_v > v) v = this_v;
    if (this_p > p) p = this_p;
  }
  if (v >= Dpcd::kVoltageMax) v |= Dpcd::kTrainMaxSwingReached;
  if (p >= Dpcd::kPreEmphasisMax) p |= Dpcd::kTrainMaxPreEmphasisReached;
  for (uint32_t lane = 0; lane < 4; ++lane) t.train_set[lane] = static_cast<uint8_t>(v | p);
}

// drm_dp_link_train_clock_recovery_delay / _channel_eq_delay
void wait(Training& t, uint32_t default_us) {
  const uint32_t interval = t.caps.training_read_interval;
  t.source.delay_us(interval == 0 ? default_us : interval * DpLinkTrainer::kIntervalUnitUs);
}
}  // namespace

DpLinkConfig DpLinkConfig::choose(const Dpcd::Capabilities& caps, uint32_t pixel_clock_khz, uint32_t bits_per_pixel) {
  static constexpr uint32_t kRates[] = {162000, 270000, 540000};
  DpLinkConfig c;
  const uint32_t max_rate = caps.max_link_rate_khz();
  const uint32_t max_lanes = caps.max_lanes;
  if (bits_per_pixel == 0) bits_per_pixel = 24;
  // the first rate: 2.7 Gbit/s on fewer lanes than the sink has (if the sink can do 2.7)
  for (uint32_t lanes = 1; max_rate >= 270000 && lanes < max_lanes; lanes <<= 1) {
    if (uint64_t{lanes} * 270000 * 8 / bits_per_pixel >= pixel_clock_khz) {
      c.lanes = lanes;
      c.rate_khz = 270000;
      return c;
    }
  }
  for (uint32_t rate : kRates) {
    if (rate > max_rate) break;
    for (uint32_t lanes = 1; lanes <= max_lanes; lanes <<= 1) {
      if (uint64_t{lanes} * rate * 8 / bits_per_pixel >= pixel_clock_khz) {
        c.lanes = lanes;
        c.rate_khz = rate;
        return c;
      }
    }
  }
  c.lanes = 4;
  c.rate_khz = 162000;
  return c;
}

DpLinkTrainer::Outcome DpLinkTrainer::train(IDpcdAux& aux, IDpSource& source, const Dpcd::Capabilities& caps, uint8_t bandwidth_code, uint32_t lanes) {
  Outcome out;
  out.lanes = lanes;
  out.bandwidth_code = bandwidth_code;
  if (lanes != 1 && lanes != 2 && lanes != 4) {
    out.result = Result::SinkSetup;
    return out;
  }
  Training t{aux, source, caps, lanes, {}, {}};

  // power up the sink (a sink older than DPCD 1.1 has no power state), 1 to 2 ms to wake
  if (caps.revision >= 0x11) {
    const uint8_t d0 = Dpcd::kPowerD0;
    if (!aux.dpcd_write(Dpcd::kSetPower, ConstByteView(&d0, 1))) {
      out.result = Result::SinkPower;
      return out;
    }
    source.delay_us(1000);
  }
  // possibly enable the 0.5 % downspread on the sink; the lane count (with enhanced framing if the sink has it) and the link rate
  const uint8_t spread = caps.max_downspread ? Dpcd::kSpreadAmp05 : 0;
  const uint8_t lane_count = static_cast<uint8_t>(lanes | (caps.enhanced_framing ? Dpcd::kLaneCountEnhancedFraming : 0));
  if (!aux.dpcd_write(Dpcd::kDownspreadCtrl, ConstByteView(&spread, 1)) || !aux.dpcd_write(Dpcd::kLaneCountSet, ConstByteView(&lane_count, 1)) ||
      !aux.dpcd_write(Dpcd::kLinkBwSet, ConstByteView(&bandwidth_code, 1))) {
    out.result = Result::SinkSetup;
    return out;
  }
  // start training on the source, the pattern off on the sink
  if (!source.start_training()) {
    out.result = Result::SourceStart;
    return out;
  }
  const uint8_t off = Dpcd::kTrainingPatternDisable;
  if (!aux.dpcd_write(Dpcd::kTrainingPatternSet, ConstByteView(&off, 1))) {
    out.result = Result::PatternWrite;
    return out;
  }

  // clock recovery
  Result result = Result::Trained;
  if (!set_pattern(t, Dpcd::kTrainingPattern1) || !update_drive(t)) {
    result = Result::PatternWrite;
  } else {
    source.delay_us(400);
    uint32_t tries = 0;
    uint32_t voltage = 0xFF;
    while (true) {
      wait(t, kDefaultClockRecoveryUs);
      ++out.clock_recovery_reads;
      if (!Dpcd::read_link_status(aux, &t.status)) {
        result = Result::StatusRead;
        break;
      }
      if (t.status.clock_recovery_ok(lanes)) break;
      bool all_max = true;
      for (uint32_t lane = 0; lane < lanes; ++lane)
        if (!(t.train_set[lane] & Dpcd::kTrainMaxSwingReached)) all_max = false;
      if (all_max) {
        result = Result::ClockRecoveryMaxSwing;
        break;
      }
      if ((t.train_set[0] & Dpcd::kTrainVoltageMask) == voltage) {
        if (++tries == kClockRecoveryTries) {
          result = Result::ClockRecoveryTries;
          break;
        }
      } else {
        tries = 0;
      }
      voltage = t.train_set[0] & Dpcd::kTrainVoltageMask;
      get_adjust_train(t);
      if (!update_drive(t)) {
        result = Result::DriveWrite;
        break;
      }
    }
  }

  // channel equalization (only if the clock was recovered)
  if (result == Result::Trained) {
    if (!set_pattern(t, caps.tps3 ? Dpcd::kTrainingPattern3 : Dpcd::kTrainingPattern2)) {
      result = Result::PatternWrite;
    } else {
      uint32_t tries = 0;
      bool equalized = false;
      while (true) {
        wait(t, kDefaultChannelEqUs);
        ++out.channel_eq_reads;
        if (!Dpcd::read_link_status(aux, &t.status)) {
          result = Result::StatusRead;
          break;
        }
        if (t.status.channel_eq_ok(lanes)) {
          equalized = true;
          break;
        }
        if (tries > kChannelEqTries) break;
        get_adjust_train(t);
        if (!update_drive(t)) {
          result = Result::DriveWrite;
          break;
        }
        ++tries;
      }
      if (!equalized && result == Result::Trained) result = Result::ChannelEqualization;
    }
  }

  // the pattern off on the sink and training over on the source, whatever happened
  const bool sink_off = aux.dpcd_write(Dpcd::kTrainingPatternSet, ConstByteView(&off, 1));
  const bool source_done = source.complete_training();
  if (result == Result::Trained && !sink_off) result = Result::PatternWrite;
  if (result == Result::Trained && !source_done) result = Result::SourceComplete;
  for (uint32_t i = 0; i < 4; ++i) out.train_set[i] = t.train_set[i];
  out.result = result;
  return out;
}

}  // namespace hardware
