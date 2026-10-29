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

// A mode as the CRTC wants it (drm_display_mode's crtc_* values: already halved for interlace and doubled for double scan).
struct CrtcMode {
  uint32_t pixel_clock_khz = 0;
  uint16_t h_display = 0, h_sync_start = 0, h_sync_end = 0, h_total = 0;
  uint16_t v_display = 0, v_sync_start = 0, v_sync_end = 0, v_total = 0;
  bool h_sync_negative = false, v_sync_negative = false, composite_sync = false, interlaced = false, double_scan = false;
};

// What the tables of one output need to know: the CRTC, PLL, encoder, transmitter, connector and the link.
struct ModeSetOutput {
  uint8_t crtc = 0;                     // 0..5
  uint8_t pll = 0xFF;                   // ATOM_PPLL1 0, ATOM_PPLL2 1, ATOM_PPLL0 2 (ATOM_PPLL_INVALID 0xFF: none)
  uint8_t encoder_id = 0;               // ENCODER_OBJECT_ID_* of the encoder (AtomConnectors::encoder_id)
  uint8_t dig_encoder = 0;              // 0..6 (AtomConnectors::dig_encoder); the PHY (UNIPHY A..G) is the same number on DCE 11
  bool link_b = false;
  uint8_t connector_object_id = 0;      // CONNECTOR_OBJECT_ID_*
  uint8_t hpd = 0xFF;                   // the hot plug pin 0..5 or 0xFF
  uint8_t encoder_mode = 0;             // ATOM_ENCODER_MODE_*: DP 0, LVDS 1, DVI 2, HDMI 3
  bool dfp = true;                      // a digital flat panel device (not the LVDS panel): coherent mode and dual link apply
  uint32_t bits_per_color = 8;          // 6, 8, 10, 12, 16
  uint32_t lanes = 0;                   // DisplayPort: dig_connector->dp_lane_count (0 until chosen)
  uint32_t link_rate_khz = 0;           // DisplayPort: dig_connector->dp_clock (0 until chosen)
  bool dual_link = false;               // amdgpu_dig_monitor_is_duallink: DVI above 165 MHz
};

// The tables of the video BIOS that set a mode, with the parameters amdgpu gives them (atombios_crtc.c and atombios_encoders.c, checked against v6.12): SetPixelClock 1.7,
// SetCRTC_UsingDTDTiming, SetCRTC_OverScan, EnableScaler, SelectCRTC_Source 1.2 / 1.3, EnableCRTC, BlankCRTC, UpdateCRTC_DoubleBufferRegisters (lock), EnableDispPowerGating, and the DIGx
// encoder control (1.2 .. 1.5) and the UNIPHY transmitter control (1.4, 1.5, 1.6). Each function is false if the table is not there, has a revision that is not handled, or fails.
// scripts/check_modeset_port.py checks the indexes, layouts and constants against atombios.h and the driver's sources.
class AmdModeSet {
 public:
  // GetIndexIntoMasterTable(COMMAND, ...)
  static constexpr uint32_t kDigxEncoderControl = 4, kSetPixelClock = 12, kEnableDispPowerGating = 13, kEnableScaler = 33, kBlankCrtc = 34, kEnableCrtc = 35, kSetCrtcOverscan = 40,
                            kSelectCrtcSource = 42, kUpdateDoubleBuffer = 44, kSetCrtcUsingDtdTiming = 49, kTransmitterControl = 76;
  // ATOM_ENCODER_CMD_*
  static constexpr uint8_t kEncoderSetup = 0x0F, kEncoderSetupPanelMode = 0x10, kEncoderDpVideoOff = 0x0C, kEncoderDpVideoOn = 0x0D;
  // ATOM_TRANSMITTER_ACTION_*
  static constexpr uint8_t kTransmitterDisable = 0, kTransmitterEnable = 1, kTransmitterSetupVsEmph = 11;
  static constexpr uint8_t kPpll1 = 0, kPpll2 = 1, kPpll0 = 2, kDpDto = 11, kComboPhyPll0 = 20, kPllInvalid = 0xFF;
  static constexpr uint8_t kCrtcInvalid = 0xFF;
  static constexpr uint8_t kModeDp = 0, kModeLvds = 1, kModeDvi = 2, kModeHdmi = 3, kModeDpMst = 5;
  static constexpr uint8_t kPanelModeExternalDp = 0, kPanelModeInternalDp2 = 1, kPanelModeInternalDp1 = 0x11;
  static constexpr uint32_t kMaxCrtcs = 6;

  AmdModeSet(IAtomHost& host, AtomBios& bios) : host_(host), bios_(bios) {}

  bool set_pixel_clock(const ModeSetOutput& out, const CrtcMode& mode);                 // amdgpu_atombios_crtc_program_pll (V7: the card computes the PLL)
  bool set_dtd_timing(const ModeSetOutput& out, const CrtcMode& mode);                  // amdgpu_atombios_crtc_set_dtd_timing (no borders)
  bool overscan_setup(const ModeSetOutput& out);                                          // ... _overscan_setup with RMX off: all zero
  bool scaler_setup(const ModeSetOutput& out);                                            // ... _scaler_setup with RMX off: disabled
  bool select_crtc_source(const ModeSetOutput& out);                                      // amdgpu_atombios_encoder_set_crtc_source (V2 / V3)
  bool enable_crtc(uint8_t crtc, bool enable);
  bool blank_crtc(uint8_t crtc, bool blank);
  bool lock_crtc(uint8_t crtc, bool lock);
  bool powergate_crtc(uint8_t crtc, bool enable);                                         // EnableDispPowerGating
  // amdgpu_atombios_encoder_setup_dig_encoder: the DIGx encoder control with an action (kEncoderSetup, the panel mode with `panel_mode`, the training commands of AmdDpSource, DP video on / off)
  bool encoder(const ModeSetOutput& out, const CrtcMode& mode, uint8_t action, uint8_t panel_mode = 0);
  // amdgpu_atombios_encoder_setup_dig_transmitter: the UNIPHY transmitter control with an action (kTransmitterEnable, kTransmitterDisable, SETUP_VSEMPH with `lane_set` for all lanes)
  bool transmitter(const ModeSetOutput& out, const CrtcMode& mode, uint8_t action, uint8_t lane_num = 0, uint8_t lane_set = 0);

  // dce_v11_0_pick_pll for Polaris and VEGAM: DisplayPort uses the DP DTO (ATOM_DP_DTO); the other outputs a combo PHY PLL by their UNIPHY (UNIPHY 0/1/2: PLL 0/2/4, link B one higher). The
  // encoder id is AtomConnectors::encoder_id; kPllInvalid for any other encoder.
  static uint8_t pick_pll(uint8_t encoder_mode, uint8_t encoder_id, bool link_b);
  // ASIC_INT_DIGn_ENCODER_ID of DIG `dig` (0..6), 0 for any other
  static uint8_t dig_encoder_id(uint8_t dig);
  // amdgpu_atombios_encoder_get_bpc: PANEL_*BIT_PER_COLOR for 6, 8, 10, 12, 16 bits (0: undefined; anything else 8)
  static uint8_t panel_bpc(uint32_t bits_per_color);
  static bool is_dp(uint8_t encoder_mode) { return encoder_mode == kModeDp || encoder_mode == kModeDpMst; }

 private:
  IAtomHost& host_;
  AtomBios& bios_;
};

}  // namespace hardware
