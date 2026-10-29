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

namespace hardware {

// What a mode set of one output needs: the CRTC, PLL, encoder and transmitter it runs on and the mode.
struct DisplayMode {
  uint32_t pixel_clock_khz = 0;
  uint16_t h_display = 0, h_sync_start = 0, h_sync_end = 0, h_total = 0;
  uint16_t v_display = 0, v_sync_start = 0, v_sync_end = 0, v_total = 0;
  bool h_sync_negative = false, v_sync_negative = false, interlaced = false, double_scan = false;
};

struct ModeSetOutput {
  uint8_t crtc = 0;                     // 0..5
  uint8_t pll = 0xFF;                   // ATOM_PPLL1 0, ATOM_PPLL2 1, ATOM_PPLL0 2 (ATOM_PPLL_INVALID 0xFF: none chosen)
  uint8_t encoder_id = 0;               // the encoder object id (ENCODER_OBJECT_ID_*, AtomConnectors::encoder_id), for the pixel clock table
  uint8_t dig_encoder = 0;              // 0..6 (AtomConnectors::dig_encoder)
  bool link_b = false;
  uint8_t connector_object_id = 0;
  uint8_t hpd = 0xFF;                   // the hot plug pin 0..5 or 0xFF
  uint8_t encoder_mode = 0;             // ATOM_ENCODER_MODE_*: DP 0, LVDS 1, DVI 2, HDMI 3
  uint32_t bits_per_color = 8;
  uint32_t lanes = 4;                   // of the link (DP: the trained lanes; DVI dual link and HDMI: 4 or 8)
  uint32_t link_rate_khz = 0;           // DP: the trained rate per lane (the transmitter's symbol clock)
  bool dual_link = false;               // DVI above 165 MHz
  uint8_t reference_clock_source = 0;   // the transmitter's PLL as in DpSourceConfig
};

// The tables of the video BIOS that set a mode, with the parameters the driver gives them: SetPixelClock 1.7, SetCRTC_Timing, SelectCRTC_Source 1.2/1.3, EnableCRTC, BlankCRTC,
// UpdateCRTC_DoubleBufferRegisters, and the DIG encoder and UNIPHY transmitter control tables (1.3/1.4 and 1.4/1.5) for the actions that set up and switch the output. Port of
// amdgpu_atombios_crtc.c and amdgpu_atombios_encoder.c for DCE 11 (Copyright Advanced Micro Devices, Inc.; MIT license), written from memory of them; the table indexes and parameter
// layouts are checked against atombios.h by scripts/check_modeset_port.py. Each function is false if the table is not there, has a revision that is not handled, or fails.
class AmdModeSet {
 public:
  // GetIndexIntoMasterTable(COMMAND, ...)
  static constexpr uint32_t kBlankCrtc = 34, kEnableCrtc = 35, kSetPixelClock = 12, kSetCrtcTiming = 39, kSelectCrtcSource = 42, kUpdateDoubleBuffer = 44,
                            kEncoder1Table = 74, kEncoder2Table = 75, kTransmitterTable = 76;
  // ATOM_ENCODER_CMD_*
  static constexpr uint8_t kEncoderDisable = 0, kEncoderEnable = 1, kEncoderDpVideoOff = 0x0C, kEncoderDpVideoOn = 0x0D, kEncoderSetup = 0x0F;
  // ATOM_TRANSMITTER_ACTION_*
  static constexpr uint8_t kTransmitterDisable = 0, kTransmitterEnable = 1, kTransmitterDisableOutput = 8, kTransmitterEnableOutput = 9, kTransmitterSetup = 10;
  static constexpr uint8_t kPpll1 = 0, kPpll2 = 1, kPpll0 = 2, kPllInvalid = 0xFF;
  static constexpr uint8_t kModeDp = 0, kModeLvds = 1, kModeDvi = 2, kModeHdmi = 3;
  static constexpr uint32_t kMaxCrtcs = 6;

  AmdModeSet(IAtomHost& host, AtomBios& bios) : host_(host), bios_(bios) {}

  // SetPixelClock (V7): the pixel clock in 100 Hz units, PLL, encoder id, encoder mode, CRTC, and the deep colour ratio of an HDMI output (24, 30, 36 bit: none, 5:4, 3:2). Dual link DVI is
  // flagged above 165 MHz as the driver does.
  bool set_pixel_clock(const ModeSetOutput& out, const DisplayMode& mode);
  // SetCRTC_Timing: totals, display, sync start and width of both directions and the polarity, interlace and double clock flags.
  bool set_crtc_timing(const ModeSetOutput& out, const DisplayMode& mode);
  // SelectCRTC_Source (V2 and V3): the CRTC to the DIG encoder (ASIC_INT_DIGn_ENCODER_ID), with the encoder mode and for V3 the panel colour depth.
  bool select_crtc_source(const ModeSetOutput& out);
  bool enable_crtc(uint8_t crtc, bool enable);
  bool blank_crtc(uint8_t crtc, bool blank);
  bool lock_crtc(uint8_t crtc, bool lock);
  // The DIG encoder control table of the output's DIG with an action (kEncoder*): setup for the mode (kEncoderSetup), enable, disable, DP video on and off.
  bool encoder(const ModeSetOutput& out, const DisplayMode& mode, uint8_t action);
  // The UNIPHY transmitter control table with an action (kTransmitter*).
  bool transmitter(const ModeSetOutput& out, const DisplayMode& mode, uint8_t action);

  // ASIC_INT_DIGn_ENCODER_ID of DIG `dig` (0..6), 0 for any other
  static uint8_t dig_encoder_id(uint8_t dig);
  // ATOM_ENCODER_CONFIG link rate field for a DP rate, -1 if the table has none
  static int link_rate_bits(uint32_t rate_khz, uint8_t crev);

 private:
  bool crtc_table(uint32_t table, uint8_t crev_min, uint8_t crev_max, uint8_t crtc, uint32_t second);
  IAtomHost& host_;
  AtomBios& bios_;
};

}  // namespace hardware
