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
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"

namespace hardware {

// The source side of DisplayPort link training of a DIG encoder and its UNIPHY transmitter, through the video BIOS: the encoder control table (DIG1EncoderControl or DIG2EncoderControl)
// with the actions DP_LINK_TRAINING_START, _PATTERN1, _PATTERN2, _PATTERN3 and _COMPLETE, and the transmitter control table (UNIPHYTransmitterControl) with SETUP_VSEMPH. Port of
// amdgpu_atombios_encoder_setup_dig_encoder and _setup_dig_transmitter (atombios_encoders.c; Copyright Advanced Micro Devices, Inc.; MIT license) for the table revisions they handle that a
// DCE 11 card has: the encoder control V3 and V4 (frev 1, crev 3 and 4: 8 bytes of parameters), the transmitter control V4 and V1.5 (crev 4 and 5). Any other revision is refused.
//
// The layouts are checked against atombios.h by scripts/check_dplink_port.py; the values the driver puts in them are from memory (atombios_encoders.c was not at hand).
struct DpSourceConfig {
  uint8_t dig_encoder = 0;              // 0..6: the DIG (AtomConnectors::dig_encoder); the UNIPHY and its link follow from it on DCE 11
  bool link_b = false;                  // the transmitter's link B (AtomConnectors::link_b)
  uint32_t lanes = 4;                   // 1, 2 or 4
  uint32_t rate_khz = 270000;           // per lane: 162000, 270000, 540000 (or 324000)
  uint32_t pixel_clock_khz = 0;         // of the mode (the table wants it)
  uint8_t connector_object_id = 0;      // CONNECTOR_OBJECT_ID_*, for the transmitter V1.5
  uint8_t hpd = 0xFF;                   // the hot plug pin 0..5 or 0xFF (the tables want 1..6, or 0 for none)
  uint8_t bits_per_color = 2;           // the table's ucBitPerColor: 1 6 bit, 2 8 bit, 3 10 bit, 4 12 bit
  uint8_t reference_clock_source = 0;   // the transmitter's PLL: V4 bits 5:4 (PPLL1 0, PPLL2 1, DCPLL 2), V1.5 bits 3:2 (P1PLL 0, P2PLL 1, P0PLL 2)
};

class AmdDpSource final : public IDpSource {
 public:
  static constexpr uint32_t kEncoder1Table = 74, kEncoder2Table = 75, kTransmitterTable = 76;       // GetIndexIntoMasterTable(COMMAND, DIG1EncoderControl / DIG2EncoderControl / UNIPHYTransmitterControl)
  // ATOM_ENCODER_CMD_DP_LINK_TRAINING_*
  static constexpr uint8_t kCmdStart = 0x08, kCmdPattern1 = 0x09, kCmdPattern2 = 0x0A, kCmdPattern3 = 0x13, kCmdComplete = 0x0B;
  static constexpr uint8_t kActionSetupVsEmph = 11;       // ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH
  static constexpr uint8_t kEncoderModeDp = 0, kDigModeDp = 0;

  AmdDpSource(IAtomHost& host, AtomBios& bios, const DpSourceConfig& config) : host_(host), bios_(bios), config_(config) {}

  bool start_training() override { return encoder(kCmdStart); }
  bool set_pattern(uint8_t pattern) override;
  bool complete_training() override { return encoder(kCmdComplete); }
  bool set_drive(uint8_t lane_set) override { return transmitter(lane_set); }
  void delay_us(uint32_t microseconds) override { host_.delay_us(microseconds); }

 private:
  bool encoder(uint8_t action);
  bool transmitter(uint8_t lane_set);

  IAtomHost& host_;
  AtomBios& bios_;
  DpSourceConfig config_;
};

}  // namespace hardware
