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
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"

namespace hardware {

// The source side of DisplayPort link training: the transmitter (the voltage swing and pre-emphasis of the lanes) and the encoder (the training patterns and the start and end of
// training). AmdDpSource does these through the video BIOS.
class IDpSource {
 public:
  virtual bool start_training() = 0;                    // the encoder begins link training (ATOM_ENCODER_CMD_DP_LINK_TRAINING_START)
  virtual bool set_pattern(uint8_t pattern) = 0;        // the encoder sends training pattern 1, 2 or 3
  virtual bool complete_training() = 0;                 // training is over (ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE): the encoder sends the video
  virtual bool set_drive(uint8_t lane_set) = 0;         // voltage swing and pre-emphasis of all lanes, as a DP_TRAINING_LANEx_SET value (ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH)
  virtual void delay_us(uint32_t microseconds) = 0;

 protected:
  IDpSource() = default;
  ~IDpSource() = default;
};

// The lanes and the link rate for a mode (amdgpu_atombios_dp_get_dp_link_config): the lowest rate of 1.62, 2.7 and 5.4 Gbit/s per lane (up to the sink's) and then the fewest lanes
// (1, 2, 4 up to the sink's) that carry the pixel clock; no lanes (0) if nothing does. (The driver treats a NUTMEG bridge as 2.7 Gbit/s only: not done.) An eDP 1.4 panel's own rate
// table, which the driver does not use, is used here when it has one.
struct DpLinkConfig {
  uint32_t lanes = 0;
  uint32_t rate_khz = 0;        // per lane: 162000, 270000 or 540000, or any rate of an eDP 1.4 panel's own table
  // An eDP 1.4 panel lists the rates it supports (Capabilities::edp_rates_khz) and is set to one by its index (DP_LINK_RATE_SET) instead of by a bandwidth code: the index in the table
  // when the rate came from it, else kNoRateIndex.
  static constexpr uint8_t kNoRateIndex = 0xFF;
  uint8_t rate_index = kNoRateIndex;
  // For a sink with the rate table (an eDP panel of revision 1.4 or later that lists rates) the lowest rate of the table, with the fewest lanes, that carries the clock and that the encoder can be told (1.62, 2.7, 3.24 and 5.4 GHz; nothing
  // does: the highest such rate on all the lanes); else the choice described above.
  static DpLinkConfig choose(const Dpcd::Capabilities& caps, uint32_t pixel_clock_khz, uint32_t bits_per_pixel);
  // drm_dp_link_rate_to_bw_code: DP_LINK_BW_SET for the rate (the rate in units of 27 MHz: 6, 10, 20)
  // (0 when the rate is set by its table index: DP_LINK_BW_SET is then written 0)
  uint8_t bandwidth_code() const { return rate_index != kNoRateIndex ? 0 : static_cast<uint8_t>(rate_khz / 27000); }
};

// DisplayPort link training as amdgpu_atombios_dp_link_train does it: power up the sink, downspread, the lane count (with enhanced framing if the sink has it) and the link rate on the
// sink, the encoder started; clock recovery with training pattern 1 (the drive set to what the sink asks for until every lane has recovered its clock, the voltage unchanged five
// times in a row or the maximum reached on every lane gives up); channel equalization with pattern 3 if the sink has it else 2 (up to six adjustments); the pattern off on the sink and
// the encoder told that training is complete. The waits between the steps are the sink's DP_TRAINING_AUX_RD_INTERVAL (100 us and 400 us for 0, else 4 ms per unit; 100 us in the clock recovery for a DPCD 1.4 sink).
//
// Follows amdgpu's atombios_dp.c and drm_dp_helper.c (v6.12); channel equalization is not tried after a failed clock recovery (as there). One difference: the driver ignores a failed
// DPCD write, here it ends the training with the result that names it. The wait before the pattern is switched off at the end is 400 us.
class DpLinkTrainer {
 public:
  enum class Result : uint8_t {
    Trained,
    SinkPower,              // the sink would not take the power state
    SinkSetup,              // the downspread, lane count or link rate could not be written
    SourceStart,            // the encoder (the video BIOS) refused to start
    PatternWrite,           // the training pattern could not be set on the sink or the source
    DriveWrite,             // the voltage swing and pre-emphasis could not be set on the transmitter or the sink
    StatusRead,             // the link status could not be read
    ClockRecoveryMaxSwing,  // every lane asked for the most swing there is and still has not recovered the clock
    ClockRecoveryTries,     // the sink asked for the same voltage five times and the clock is not recovered
    ChannelEqualization,    // six adjustments and the lanes are still not equalized and aligned
    SourceComplete,         // the encoder refused to complete
  };
  static constexpr uint32_t kClockRecoveryTries = 5, kChannelEqTries = 5;      // (the clock recovery gives up at five repeats of the voltage; equalization after tries > 5)
  static constexpr uint32_t kDefaultClockRecoveryUs = 100, kDefaultChannelEqUs = 400, kIntervalUnitUs = 4000;

  struct Outcome {
    Result result = Result::SinkPower;
    bool trained() const { return result == Result::Trained; }
    uint32_t lanes = 0;
    uint8_t bandwidth_code = 0;
    uint8_t rate_index = DpLinkConfig::kNoRateIndex;     // DP_LINK_RATE_SET, when the rate was set that way
    uint32_t clock_recovery_reads = 0;       // link status reads of the clock recovery loop
    uint32_t channel_eq_reads = 0;           // and of the channel equalization loop
    uint8_t train_set[4] = {};               // what the lanes ended up driven with (DP_TRAINING_LANEx_SET)
  };

  // `caps`: what the sink can do (the lane count and rate are the caller's choice within it, DpLinkConfig::choose). `lanes` 1, 2 or 4.
  // `alternate_scrambler`: the sink is an eDP panel in the internal DP2 mode (its DP_EDP_CONFIGURATION_SET gets the alternate scrambler seed, DP_PANEL_MODE_INTERNAL_DP2_MODE).
  // `rate_index`: not kNoRateIndex for a rate set by the eDP 1.4 table (`bandwidth_code` is then 0 and written as such, followed by DP_LINK_RATE_SET).
  static Outcome train(IDpcdAux& aux, IDpSource& source, const Dpcd::Capabilities& caps, uint8_t bandwidth_code, uint32_t lanes, uint8_t rate_index = DpLinkConfig::kNoRateIndex, bool alternate_scrambler = false);
};

}  // namespace hardware
