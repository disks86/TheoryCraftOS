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


#include "Hardware/GraphicsProcessingUnit/AMD/AmdDpSource.hpp"

namespace hardware {

AmdDpSource::AmdDpSource(IAtomHost& host, AtomBios& bios, const DpSourceConfig& config) : host_(host), set_(host, bios) {
  out_.dig_encoder = config.dig_encoder;
  out_.link_b = config.link_b;
  out_.lanes = config.lanes;
  out_.link_rate_khz = config.rate_khz;
  out_.connector_object_id = config.connector_object_id;
  out_.hpd = config.hpd;
  out_.bits_per_color = config.bits_per_color;
  out_.pll = config.pll;
  out_.encoder_mode = AmdModeSet::kModeDp;
  mode_.pixel_clock_khz = config.pixel_clock_khz;
}

bool AmdDpSource::set_pattern(uint8_t pattern) {
  switch (pattern) {
    case Dpcd::kTrainingPattern1: return encoder(kCmdPattern1);
    case Dpcd::kTrainingPattern2: return encoder(kCmdPattern2);
    case Dpcd::kTrainingPattern3: return encoder(kCmdPattern3);
    default: return false;
  }
}

}  // namespace hardware
