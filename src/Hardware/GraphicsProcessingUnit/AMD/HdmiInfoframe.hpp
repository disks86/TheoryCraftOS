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
#include "Utility/ArrayView.hpp"

namespace hardware {

// What HDMI (and the audio of DisplayPort, which the driver sets up the same way) needs to know of a display mode for the audio clock and the packets in the blanking: the audio clock
// regeneration values (N and CTS: amdgpu_afmt.c) and the AVI InfoFrame (drm_hdmi_avi_infoframe_from_display_mode and hdmi_avi_infoframe_pack, drm_edid.c / hdmi.c).
//
// Written from memory of the DRM helpers (amdgpu_afmt.c, drm_edid.c's CEA mode table and hdmi.c were not at hand): the predefined N/CTS table is checked against the HDMI formula
// N / CTS = 128 * fs / clock by scripts/check_afmt_acr.py, the CEA mode (VIC) table is the subset of CTA-861 modes that matter and is not checked.
struct HdmiMode {
  uint32_t clock_khz = 0;                 // the pixel clock
  uint32_t hdisplay = 0, vdisplay = 0, htotal = 0, vtotal = 0;
  bool interlaced = false;                // vdisplay and vtotal are of the frame; the refresh is the field rate
  bool double_clock = false;              // DRM_MODE_FLAG_DBLCLK: each pixel is sent twice (the 480i and 576i modes: 720 pixels in a 1440 pixel line)
  uint8_t picture_aspect = 0;             // 0 take the CEA mode's (the first of the 4:3 and 16:9 pair), 1 4:3, 2 16:9
};

class Hdmi {
 public:
  // The audio clock regeneration values: N and CTS for 32, 44.1 and 48 kHz audio at the pixel clock (amdgpu_afmt_acr)
  struct Acr {
    uint32_t n_32khz = 0, cts_32khz = 0, n_44_1khz = 0, cts_44_1khz = 0, n_48khz = 0, cts_48khz = 0;
  };
  // The predefined values for the common pixel clocks (25.175 to 148.5 MHz, exactly as in kHz), else calculated: the smallest fraction of 128 * fs / clock, scaled up to the first N at or above
  // 128 * fs / 1000 (amdgpu_afmt_calc_cts).
  static Acr acr(uint32_t clock_khz);

  // The CEA video identification code of the mode (drm_match_cea_mode): the size, the scan, the double clock, and the refresh rate within half a percent of the standard's. 0 if there is none.
  // `picture_aspect` 0 takes the first of two modes that differ only in aspect.
  static uint8_t vic(const HdmiMode& mode);

  // The AVI InfoFrame as hdmi_avi_infoframe_pack makes it, for a mode as drm_hdmi_avi_infoframe_from_display_mode describes it (RGB, no overscan, the active format "as the picture", the
  // picture aspect of the CEA mode, the pixel repeat of a double clocked mode): type 0x82, version 2, length 13, the checksum, and the thirteen data bytes.
  static constexpr uint32_t kAviBytes = 17;
  static constexpr uint8_t kAviType = 0x82, kAviVersion = 2, kAviLength = 13;
  static void avi_infoframe(const HdmiMode& mode, ByteView out);     // `out` at least kAviBytes (more is left alone)
};

}  // namespace hardware
