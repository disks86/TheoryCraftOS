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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"

namespace hardware {

// The source side of DisplayPort link training of a DIG encoder and its UNIPHY transmitter, through the video BIOS: the DIGx encoder control table with the actions
// DP_LINK_TRAINING_START, _PATTERN1, _PATTERN2, _PATTERN3 and _COMPLETE, and the transmitter control table (UNIPHYTransmitterControl) with SETUP_VSEMPH. The tables' parameters are
// built by AmdModeSet (amdgpu_atombios_encoder_setup_dig_encoder / _setup_dig_transmitter).
struct DpSourceConfig {
  uint8_t dig_encoder = 0;              // 0..6: the DIG (AtomConnectors::dig_encoder); the UNIPHY and its link follow from it on DCE 11
  bool link_b = false;                  // the transmitter's link B (AtomConnectors::link_b)
  uint32_t lanes = 4;                   // 1, 2 or 4
  uint32_t rate_khz = 270000;           // per lane: 162000, 270000, 540000 (or 324000)
  uint32_t pixel_clock_khz = 0;         // of the mode (the encoder table wants it)
  uint8_t connector_object_id = 0;      // CONNECTOR_OBJECT_ID_*, for the transmitter V1.5 and V1.6
  uint8_t hpd = 0xFF;                   // the hot plug pin 0..5 or 0xFF (the tables want 1..6, or 0 for none)
  uint32_t bits_per_color = 8;          // 6, 8, 10, 12 or 16
  uint8_t pll = AmdModeSet::kPllInvalid;       // the CRTC's PLL (ATOM_PPLL*): the transmitter V1.5 takes it as the PHY's clock source; V4 uses the display clock PLL for DisplayPort
};

class AmdDpSource final : public IDpSource {
 public:
  static constexpr uint8_t kEncoderModeDp = 0;

  AmdDpSource(IAtomHost& host, AtomBios& bios, const DpSourceConfig& config);

  bool start_training() override { return encoder(kCmdStart); }
  bool set_pattern(uint8_t pattern) override;
  bool complete_training() override { return encoder(kCmdComplete); }
  bool set_drive(uint8_t lane_set) override { return set_.transmitter(out_, mode_, AmdModeSet::kTransmitterSetupVsEmph, 0, lane_set); }
  void delay_us(uint32_t microseconds) override { host_.delay_us(microseconds); }

  // ATOM_ENCODER_CMD_DP_LINK_TRAINING_*
  static constexpr uint8_t kCmdStart = 0x08, kCmdPattern1 = 0x09, kCmdPattern2 = 0x0A, kCmdPattern3 = 0x13, kCmdComplete = 0x0B;

 private:
  bool encoder(uint8_t action) { return set_.encoder(out_, mode_, action); }

  IAtomHost& host_;
  AmdModeSet set_;
  ModeSetOutput out_;
  DisplayMode mode_;
};

}  // namespace hardware
