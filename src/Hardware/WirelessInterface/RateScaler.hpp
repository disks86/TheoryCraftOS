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

// Choosing the data rate of a Wi-Fi link from how its frames fare, for the radios whose firmware does not do it (the Intel 9000 series firmware here has no TLC offload: it sends at the
// rates of a table the host gives it, trying the entries in turn on retries). Rates are the eight OFDM rates (6 to 54 Mbit/s: every access point supports them, in both bands).
//
// The measure is the chance that one transmission succeeds, over a window of frames: successful frames divided by transmissions (a frame that went out after two retries is one success
// in three transmissions). The goodput of a rate is that chance times the rate. The rate goes down when the chance is poor or a lower rate is known to be better; it goes up, as a probe,
// when the chance is very good and the next rate is not known to be worse; a probe that does not work out is undone and that rate is left alone for longer each time. It is a reduced
// form of what iwlwifi's rs.c does (it keeps tables per mode, antenna and guard interval, which this link does not have).
#pragma once
#include <stdint.h>

namespace hardware {

class WifiRateScaler {
 public:
  static constexpr uint32_t kRateCount = 8, kTableSize = 16, kWindowFrames = 16, kMinFlushFrames = 4, kFlushMs = 1000;
  static constexpr uint32_t kRatesKbps[kRateCount] = {6000, 9000, 12000, 18000, 24000, 36000, 48000, 54000};
  // The PLCP signal of each rate (what rate_n_flags carries for OFDM in the legacy format).
  static constexpr uint8_t kPlcp[kRateCount] = {0x0D, 0x0F, 0x05, 0x07, 0x09, 0x0B, 0x01, 0x03};

  // Starts at a rate suited to the signal (0-100): strong signals start high. Forgets everything learned.
  void start(uint32_t signal_percent);
  uint32_t current() const { return current_; }
  uint32_t current_kbps() const { return kRatesKbps[current_]; }
  // The rate index of a PLCP signal, or kRateCount if it is not one of ours.
  static uint32_t index_of_plcp(uint8_t plcp);

  // The outcome of one frame: it went out first at `rate_index` (kRateCount if not known: the current rate is meant), was transmitted `attempts` times (at least 1) and in the end did
  // or did not get through. `now_ms` is only used to close a window that fills slowly.
  void on_result(bool success, uint32_t attempts, uint32_t rate_index, uint64_t now_ms);
  // Closes the window if it has been open long enough with a few frames in it.
  void tick(uint64_t now_ms);
  // The table has changed since the last call (the radio has to be told).
  bool take_changed() { const bool c = changed_; changed_ = false; return c; }
  // The rates to try in turn for one frame: the current one twice, then each lower one twice, down to the lowest, which fills the rest.
  void fill_table(uint8_t indices[kTableSize]) const;

  // For tests and logs.
  uint32_t success_chance(uint32_t rate_index) const { return rate_index < kRateCount && known_[rate_index] ? chance_[rate_index] : 0; }
  bool probing() const { return probing_; }

 private:
  void evaluate(uint64_t now_ms);
  uint32_t current_ = 0;
  uint32_t window_frames_ = 0, window_ok_ = 0, window_attempts_ = 0;
  uint64_t window_start_ms_ = 0;
  bool window_started_ = false;
  uint32_t chance_[kRateCount] = {};          // 0..1024
  bool known_[kRateCount] = {};
  uint32_t hold_[kRateCount] = {};            // windows to wait before probing this rate again
  uint32_t age_[kRateCount] = {};             // windows since the chance was measured: an old measurement is forgotten
  uint32_t backoff_[kRateCount] = {};
  bool probing_ = false;
  uint32_t probe_from_ = 0;
  bool changed_ = false;
};

}  // namespace hardware
