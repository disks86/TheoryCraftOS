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
#include "Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"

namespace hardware {

// What the driver learns of a monitor from its EDID (drm_edid.c update_display_info for a digital sink: the colour depth, whether it is HDMI, the maximum TMDS clock) and what it decides
// from it: the colour depth to drive (amdgpu_connector_get_monitor_bpc) and whether a mode is acceptable (the connectors' mode_valid helpers of amdgpu_connectors.c and
// amdgpu_atombios_dp_mode_valid_helper). Checked against v6.12 of those files.
struct MonitorInfo {
  bool digital = false;              // the EDID is version 1.3 or later and says the input is digital
  bool is_hdmi = false;              // an HDMI vendor specific data block (OUI 00-0C-03) in a CTA extension
  uint32_t bpc = 0;                  // display_info.bpc: 0 unknown
  uint32_t max_tmds_clock_khz = 0;   // 0 none given
  uint8_t hdmi_dc_modes = 0;         // the deep colour bits of the HDMI block (DRM_EDID_HDMI_DC_30 / 36 / 48)
  static constexpr uint8_t kDc30 = 1 << 4, kDc36 = 1 << 5, kDc48 = 1 << 6;
};

// update_display_info: false if the bytes are not an EDID base block (no header or a bad checksum). A sink that is not digital, or older than 1.3, gets nothing but `digital` false.
bool parse_monitor_info(ConstByteView edid, MonitorInfo* out);

// The detailed timing descriptor `index` (0 to 3) of the EDID's base block as the CRTC wants it (drm_mode_detailed and drm_mode_set_crtcinfo: the vertical values of an interlaced mode
// are per field, the total odd). False if the slot is not a timing (a monitor descriptor), is a stereo or analog sync one, or has no size. The first slot is the preferred mode.
bool edid_detailed_timing(ConstByteView edid, uint32_t index, CrtcMode* out);

// amdgpu_connector_get_monitor_bpc: 8 unless the sink says otherwise; HDMI is clamped to 12 and to what the TMDS clock allows for the mode (12 falls to 10 or 8, 10 to 8; deep colour
// without a TMDS limit is 8), and above 8 only if `deep_color` (the module parameter amdgpu.deep_color, which is 0 by default). `dp_sink`: the DisplayPort connector's sink is a
// DisplayPort one (not a dongle). `panel_bpc`: for LVDS / eDP without an EDID depth, the BIOS's panel depth (6 or 8), else 8.
uint32_t monitor_bpc(AtomConnectors::Type type, bool use_digital, bool dp_sink, const MonitorInfo& info, uint32_t mode_clock_khz, bool deep_color, uint32_t panel_bpc = 8);

enum class ModeStatus : uint8_t { Ok, ClockHigh, Panel };

struct ModeCheck {
  AtomConnectors::Type type = AtomConnectors::Type::DviD;
  uint8_t connector_object_id = 0;               // CONNECTOR_OBJECT_ID_*
  bool use_digital = true;
  bool dp_sink = false;                          // the sink on a DisplayPort connector is DisplayPort (a DPCD answered)
  MonitorInfo monitor;
  uint32_t clock_khz = 0, hdisplay = 0, vdisplay = 0;
  uint32_t native_hdisplay = 0, native_vdisplay = 0;       // an LVDS / eDP panel's own mode (0: not known)
  bool scaling = false;                          // RMX is not off: modes smaller than the panel are accepted
  uint32_t max_pixel_clock_10khz = 0;            // the firmware info's maximum pixel clock; 0: not checked
  Dpcd::Capabilities dpcd;                       // for a DisplayPort sink
  bool dp12_capable = false;                     // amdgpu_connector_is_dp12_capable: the display clock is at least 539 MHz and the encoder does HBR2
  bool deep_color = false;
};
ModeStatus mode_valid(const ModeCheck& check);

}  // namespace hardware
