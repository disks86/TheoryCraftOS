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
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace {
// The link rate in the encoder control's configuration (ATOM_ENCODER_CONFIG_V4_DPLINKRATE_*: bits 1:0 and, for V3, bit 0): 1.62, 2.70, 5.40 and 3.24 GHz. -1 if there is none.
int rate_bits(uint32_t rate_khz, uint8_t crev) {
  switch (rate_khz) {
    case 162000: return 0;
    case 270000: return 1;
    case 540000: return crev >= 4 ? 2 : -1;
    case 324000: return crev >= 4 ? 3 : -1;
    default: return -1;
  }
}
}  // namespace

bool AmdDpSource::set_pattern(uint8_t pattern) {
  switch (pattern) {
    case Dpcd::kTrainingPattern1: return encoder(kCmdPattern1);
    case Dpcd::kTrainingPattern2: return encoder(kCmdPattern2);
    case Dpcd::kTrainingPattern3: return encoder(kCmdPattern3);
    default: return false;
  }
}

// DIG_ENCODER_CONTROL_PARAMETERS_V3 / _V4, 8 bytes: usPixelClock (10 kHz), ucConfig (link rate in bits 1:0, DIG in 6:4), ucAction, ucEncoderMode, ucLaneNum, ucBitPerColor, and for V4
// ucHPD_ID (1..6, 0 for none; V3: reserved)
bool AmdDpSource::encoder(uint8_t action) {
  if (config_.dig_encoder > 6) return false;
  const uint32_t table = config_.dig_encoder ? kEncoder2Table : kEncoder1Table;
  uint8_t frev = 0, crev = 0;
  if (!bios_.cmd_table_header(table, &frev, &crev) || frev != 1 || (crev != 3 && crev != 4)) return false;
  const int rate = rate_bits(config_.rate_khz, crev);
  if (rate < 0) return false;
  const uint32_t pixel_clock = config_.pixel_clock_khz / 10;
  const uint32_t hpd = crev >= 4 && config_.hpd < 6 ? config_.hpd + 1u : 0;
  const uint32_t config = static_cast<uint32_t>(rate) | (uint32_t{config_.dig_encoder} << 4);
  uint32_t params[2] = {(pixel_clock & 0xFFFF) | (config << 16) | (uint32_t{action} << 24),
                        uint32_t{kEncoderModeDp} | (config_.lanes << 8) | (uint32_t{config_.bits_per_color} << 16) | (hpd << 24)};
  return bios_.execute_table(table, ArrayView<uint32_t>(params, 2)) == AtomBios::Status::Ok;
}

// DIG_TRANSMITTER_CONTROL_PARAMETERS_V4 (asMode: lane select 0 = all lanes and the lane set, ucConfig, ucAction, ucLaneNum) or _V1_5 (usSymClock, ucPhyId, ucAction, ucLaneNum,
// ucConnObjId, ucDigMode, ucConfig, ucDigEncoderSel, ucDPLaneSet)
bool AmdDpSource::transmitter(uint8_t lane_set) {
  if (config_.dig_encoder > 6) return false;
  uint8_t frev = 0, crev = 0;
  if (!bios_.cmd_table_header(kTransmitterTable, &frev, &crev) || frev != 1) return false;
  if (crev == 4) {
    if (config_.dig_encoder > 5) return false;       // (three UNIPHY pairs: A/B, C/D, E/F)
    const uint32_t config = (uint32_t{config_.dig_encoder / 2u} << 6) | (uint32_t{config_.reference_clock_source & 3u} << 4) | (uint32_t{config_.dig_encoder & 1u} << 3) |
                            (config_.link_b ? 1u << 2 : 0u);
    uint32_t params[2] = {0u | (uint32_t{lane_set} << 8) | (config << 16) | (uint32_t{kActionSetupVsEmph} << 24), config_.lanes};
    return bios_.execute_table(kTransmitterTable, ArrayView<uint32_t>(params, 2)) == AtomBios::Status::Ok;
  }
  if (crev == 5) {
    const uint32_t hpd_sel = config_.hpd < 6 ? config_.hpd + 1u : 0;
    const uint32_t config = (hpd_sel << 4) | (uint32_t{config_.reference_clock_source & 3u} << 2);
    uint32_t params[3] = {((config_.rate_khz / 10) & 0xFFFF) | (uint32_t{config_.dig_encoder} << 16) | (uint32_t{kActionSetupVsEmph} << 24),
                          config_.lanes | (uint32_t{config_.connector_object_id} << 8) | (uint32_t{kDigModeDp} << 16) | (config << 24),
                          (1u << config_.dig_encoder) | (uint32_t{lane_set} << 8)};
    return bios_.execute_table(kTransmitterTable, ArrayView<uint32_t>(params, 3)) == AtomBios::Status::Ok;
  }
  return false;
}

}  // namespace hardware
