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


#include "Hardware/GraphicsProcessingUnit/AMD/AmdMonitor.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"

namespace hardware {

namespace {
constexpr uint8_t kInputDigital = 1 << 7, kDigitalDepthMask = 7 << 4;
constexpr uint8_t kObjectDualLinkDviI = 0x02, kObjectDualLinkDviD = 0x04, kObjectHdmiTypeB = 0x0D;

// the CTA data blocks of an extension: tag in bits 7:5 of the header, payload length in 4:0
template <class F>
void walk_data_blocks(ConstByteView ext, F&& block) {
  if (ext.size() < 128 || ext[0] != 0x02) return;
  const uint32_t dtd = ext[2];
  if (dtd < 4 || dtd > 127) return;
  uint32_t at = 4;
  while (at < dtd) {
    const uint8_t header = ext[at];
    const uint32_t tag = header >> 5, len = header & 0x1F;
    if (at + 1 + len > dtd) break;
    block(tag, ext.subview(at, 1 + len));
    at += 1 + len;
  }
}
}  // namespace

bool parse_monitor_info(ConstByteView edid, MonitorInfo* out) {
  *out = MonitorInfo();
  static constexpr uint8_t kHeader[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  if (edid.size() < 128) return false;
  uint8_t sum = 0;
  for (uint32_t i = 0; i < 128; ++i) {
    sum = static_cast<uint8_t>(sum + edid[i]);
    if (i < 8 && edid[i] != kHeader[i]) return false;
  }
  if (sum != 0) return false;
  const uint8_t revision = edid[19], input = edid[20];
  if (edid[18] != 1 || revision < 3 || !(input & kInputDigital)) return true;
  out->digital = true;
  // drm_parse_cea_ext: the blocks of every CTA extension
  const uint32_t extensions = edid[126];
  for (uint32_t e = 0; e < extensions && (e + 2) * 128 <= edid.size(); ++e) {
    walk_data_blocks(edid.subview((e + 1) * 128, 128), [&](uint32_t tag, ConstByteView db) {
      if (tag != 3 || db.size() < 5) return;
      const uint32_t oui = db[1] | (db[2] << 8) | (db[3] << 16);
      if (oui == 0x000C03) {         // the HDMI vendor specific block
        out->is_hdmi = true;
        out->bpc = 8;                // (HDMI supports at least 8 bpc)
        const uint32_t len = db[0] & 0x1F;
        if (len >= 7) out->max_tmds_clock_khz = db[7] * 5000u;       // (drm_parse_hdmi_vsdb_video: "if (len >= 7)")
        if (len >= 6) {
          const uint8_t dc = db[6];
          uint32_t dc_bpc = 0;
          if (dc & MonitorInfo::kDc30) dc_bpc = 10;
          if (dc & MonitorInfo::kDc36) dc_bpc = 12;
          if (dc & MonitorInfo::kDc48) dc_bpc = 16;
          out->hdmi_dc_modes = dc & (MonitorInfo::kDc30 | MonitorInfo::kDc36 | MonitorInfo::kDc48);
          if (dc_bpc != 0) out->bpc = dc_bpc;
        }
      } else if (oui == 0xC45DD8 && db.size() >= 7) {     // the HDMI Forum block: a maximum character rate above 340 MHz
        const uint32_t max_tmds = db[5] * 5000u;
        if (max_tmds > 340000) out->max_tmds_clock_khz = max_tmds;
      }
    });
  }
  // DFP 1.x compliant TMDS: 8 bpc unless an extension said otherwise (EDID 1.3 bit 0 of the input byte)
  if (out->bpc == 0 && revision == 3 && (input & 1)) out->bpc = 8;
  if (revision < 4) return true;
  switch (input & kDigitalDepthMask) {       // the EDID 1.4 depth overrides what the extensions said, undefined included
    case 1 << 4: out->bpc = 6; break;
    case 2 << 4: out->bpc = 8; break;
    case 3 << 4: out->bpc = 10; break;
    case 4 << 4: out->bpc = 12; break;
    case 5 << 4: out->bpc = 14; break;
    case 6 << 4: out->bpc = 16; break;
    default: out->bpc = 0; break;
  }
  return true;
}

bool edid_detailed_timing(ConstByteView edid, uint32_t index, CrtcMode* out) {
  if (index > 3 || edid.size() < 128) return false;
  const uint32_t at = 54 + index * 18;
  uint8_t d[18];
  for (uint32_t i = 0; i < 18; ++i) d[i] = edid[at + i];
  const uint32_t clock_10khz = d[0] | (d[1] << 8);
  if (clock_10khz == 0) return false;             // a monitor descriptor
  const uint32_t h_active = d[2] | ((d[4] >> 4) << 8), h_blank = d[3] | ((d[4] & 0xF) << 8);
  const uint32_t v_active = d[5] | ((d[7] >> 4) << 8), v_blank = d[6] | ((d[7] & 0xF) << 8);
  const uint32_t h_offset = d[8] | (((d[11] >> 6) & 3) << 8), h_width = d[9] | (((d[11] >> 4) & 3) << 8);
  const uint32_t v_offset = (d[10] >> 4) | (((d[11] >> 2) & 3) << 4), v_width = (d[10] & 0xF) | ((d[11] & 3) << 4);
  const uint8_t flags = d[17];
  if (h_active == 0 || v_active == 0 || (flags & 0x20)) return false;        // stereo modes are not supported
  if (!(flags & 0x18)) return false;                                         // (DRM_EDID_PT_SEPARATE_SYNC: neither bit is an analog sync timing)
  if (!h_width || !v_width) return false;                                    // a sync width of zero is incorrect
  if (h_active + h_blank > 0xFFFF || v_active + v_blank > 0xFFFF) return false;
  CrtcMode m;
  m.pixel_clock_khz = clock_10khz * 10;
  m.h_display = static_cast<uint16_t>(h_active);
  m.h_sync_start = static_cast<uint16_t>(h_active + h_offset);
  m.h_total = static_cast<uint16_t>(h_active + h_blank);
  m.h_sync_end = static_cast<uint16_t>(m.h_sync_start + h_width > m.h_total ? m.h_total : m.h_sync_start + h_width);      // (bogus sync ends are cut at the total)
  m.v_display = static_cast<uint16_t>(v_active);
  m.v_sync_start = static_cast<uint16_t>(v_active + v_offset);
  m.v_total = static_cast<uint16_t>(v_active + v_blank);
  m.v_sync_end = static_cast<uint16_t>(m.v_sync_start + v_width > m.v_total ? m.v_total : m.v_sync_start + v_width);
  m.interlaced = (flags & 0x80) != 0;
  m.h_sync_negative = !(flags & 0x02);
  m.v_sync_negative = !(flags & 0x04);
  if (m.interlaced) m.v_total |= 1;               // (the mode's values are per field; the total is odd)
  *out = m;
  return true;
}

uint32_t monitor_bpc(AtomConnectors::Type type, bool use_digital, bool dp_sink, const MonitorInfo& info, uint32_t mode_clock_khz, bool deep_color, uint32_t panel_bpc) {
  using Type = AtomConnectors::Type;
  uint32_t bpc = 8;
  switch (type) {
    case Type::DviI:
    case Type::HdmiB:
      if (use_digital && info.is_hdmi && info.bpc) bpc = info.bpc;
      break;
    case Type::DviD:
    case Type::HdmiA:
      if (info.is_hdmi && info.bpc) bpc = info.bpc;
      break;
    case Type::DisplayPort:
      if ((dp_sink || info.is_hdmi) && info.bpc) bpc = info.bpc;
      break;
    case Type::EmbeddedDisplayPort:
    case Type::Lvds:
      bpc = info.bpc ? info.bpc : panel_bpc;
      break;
    default:
      break;
  }
  if (info.is_hdmi) {
    if (bpc > 12) bpc = 12;       // pre DCE 8 hardware cannot do more than 12 bpc
    if (info.max_tmds_clock_khz > 0) {
      if (bpc == 12 && mode_clock_khz * 3 / 2 > info.max_tmds_clock_khz) bpc = ((info.hdmi_dc_modes & MonitorInfo::kDc30) && mode_clock_khz * 5 / 4 <= info.max_tmds_clock_khz) ? 10 : 8;
      if (bpc == 10 && mode_clock_khz * 5 / 4 > info.max_tmds_clock_khz) bpc = 8;
    } else if (bpc > 8) {
      bpc = 8;       // the maximum TMDS clock is mandatory for deep colour
    }
  }
  if (!deep_color && bpc > 8) bpc = 8;
  return bpc;
}

ModeStatus mode_valid(const ModeCheck& c) {
  using Type = AtomConnectors::Type;
  switch (c.type) {
    case Type::Lvds:
    case Type::EmbeddedDisplayPort: {
      if (c.hdisplay < 320 || c.vdisplay < 240) return ModeStatus::Panel;
      if (c.native_hdisplay != 0 || c.native_vdisplay != 0) {
        if (c.hdisplay > c.native_hdisplay || c.vdisplay > c.native_vdisplay) return ModeStatus::Panel;
        if (!c.scaling && (c.hdisplay != c.native_hdisplay || c.vdisplay != c.native_vdisplay)) return ModeStatus::Panel;
      }
      return ModeStatus::Ok;
    }
    case Type::DisplayPort: {
      if (c.dp_sink) {
        // amdgpu_atombios_dp_mode_valid_helper
        const uint32_t bpc = monitor_bpc(c.type, c.use_digital, c.dp_sink, c.monitor, c.clock_khz, c.deep_color);
        const DpLinkConfig config = DpLinkConfig::choose(c.dpcd, c.clock_khz, bpc == 0 ? 24 : bpc * 3);
        if (config.lanes == 0) return ModeStatus::ClockHigh;
        if (config.rate_khz == 540000 && !c.dp12_capable) return ModeStatus::ClockHigh;
        return ModeStatus::Ok;
      }
      if (c.monitor.is_hdmi) return c.clock_khz > 340000 ? ModeStatus::ClockHigh : ModeStatus::Ok;
      return c.clock_khz > 165000 ? ModeStatus::ClockHigh : ModeStatus::Ok;
    }
    default: {       // the DVI and HDMI connectors (amdgpu_connector_dvi_mode_valid)
      if (c.use_digital && c.clock_khz > 165000) {
        if (c.connector_object_id == kObjectDualLinkDviI || c.connector_object_id == kObjectDualLinkDviD || c.connector_object_id == kObjectHdmiTypeB) return ModeStatus::Ok;
        if (c.monitor.is_hdmi) return c.clock_khz > 340000 ? ModeStatus::ClockHigh : ModeStatus::Ok;
        return ModeStatus::ClockHigh;
      }
      if (c.max_pixel_clock_10khz != 0 && c.clock_khz / 10 > c.max_pixel_clock_10khz) return ModeStatus::ClockHigh;
      return ModeStatus::Ok;
    }
  }
}

}  // namespace hardware
