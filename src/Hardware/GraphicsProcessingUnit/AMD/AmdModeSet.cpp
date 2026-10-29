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


#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"

namespace hardware {

namespace {
constexpr uint32_t kModeMiscHSyncPolarity = 0x02, kModeMiscVSyncPolarity = 0x04, kModeMiscCompositeSync = 0x40, kModeMiscInterlace = 0x80, kModeMiscDoubleClock = 0x100;
constexpr uint32_t kMiscDviDualLink = 0x01 << 3;     // PIXEL_CLOCK_V7_MISC_DVI_DUALLINK_EN
constexpr uint8_t kEncoderRefclkDcpll = 2;           // ENCODER_REFCLK_SRC_DCPLL (V4: the DP PHY is clocked from the display clock PLL)

bool revision(AtomBios& bios, uint32_t table, uint8_t* crev) {
  uint8_t frev = 0;
  return bios.cmd_table_header(table, &frev, crev) && frev == 1;
}
bool run(AtomBios& bios, uint32_t table, uint32_t* words, uint32_t count) {
  return bios.execute_table(table, ArrayView<uint32_t>(words, count)) == AtomBios::Status::Ok;
}
}  // namespace

uint8_t AmdModeSet::dig_encoder_id(uint8_t dig) {
  static constexpr uint8_t kIds[] = {0x03, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};      // ASIC_INT_DIG1_ENCODER_ID, DIG2 ... DIG7
  return dig < sizeof kIds ? kIds[dig] : 0;
}

uint8_t AmdModeSet::pick_pll(uint8_t encoder_mode, uint8_t encoder_id, bool link_b) {
  if (is_dp(encoder_mode)) return kDpDto;
  const uint8_t link = link_b ? 1 : 0;
  switch (encoder_id) {
    case 0x1E: return static_cast<uint8_t>(kComboPhyPll0 + link);        // ENCODER_OBJECT_ID_INTERNAL_UNIPHY
    case 0x20: return static_cast<uint8_t>(kComboPhyPll0 + 2 + link);    // _UNIPHY1
    case 0x21: return static_cast<uint8_t>(kComboPhyPll0 + 4 + link);    // _UNIPHY2
    default: return kPllInvalid;
  }
}

uint8_t AmdModeSet::panel_bpc(uint32_t bits_per_color) {
  switch (bits_per_color) {
    case 0: return 0;
    case 6: return 1;
    case 10: return 3;
    case 12: return 4;
    case 16: return 5;
    default: return 2;
  }
}

// PIXEL_CLOCK_PARAMETERS_V7, 16 bytes: ulPixelClock (100 Hz), ucPpll, ucTransmitterID, ucEncoderMode, ucMiscInfo, ucCRTC, ucDeepColorRatio, 2 + 4 reserved
bool AmdModeSet::set_pixel_clock(const ModeSetOutput& out, const CrtcMode& mode) {
  uint8_t crev = 0;
  if (!revision(bios_, kSetPixelClock, &crev) || crev != 7 || (out.crtc >= kMaxCrtcs && out.crtc != kCrtcInvalid) || mode.pixel_clock_khz > 0xFFFFFFFFu / 10) return false;
  const uint32_t misc = out.encoder_mode == kModeDvi && mode.pixel_clock_khz > 165000 ? kMiscDviDualLink : 0;
  uint32_t deep = 0;                    // PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_DIS / 5_4 / 3_2 / 2_1: for HDMI only
  if (out.encoder_mode == kModeHdmi) deep = out.bits_per_color == 10 ? 1 : out.bits_per_color == 12 ? 2 : out.bits_per_color == 16 ? 3 : 0;
  uint32_t words[4] = {mode.pixel_clock_khz * 10, uint32_t{out.pll} | (uint32_t{out.encoder_id} << 8) | (uint32_t{out.encoder_mode} << 16) | (misc << 24),
                       uint32_t{out.crtc} | (deep << 8), 0};
  return run(bios_, kSetPixelClock, words, 4);
}

// SET_CRTC_USING_DTD_TIMING_PARAMETERS, 24 bytes: usH_Size, usH_Blanking_Time, usV_Size, usV_Blanking_Time, usH_SyncOffset, usH_SyncWidth, usV_SyncOffset, usV_SyncWidth,
// susModeMiscInfo, ucH_Border, ucV_Border, ucCRTC, 3 padding. The blanking is to the end of the total (drm's crtc_hblank_end = crtc_htotal).
bool AmdModeSet::set_dtd_timing(const ModeSetOutput& out, const CrtcMode& mode) {
  uint8_t crev = 0;
  if (!revision(bios_, kSetCrtcUsingDtdTiming, &crev) || out.crtc >= kMaxCrtcs) return false;
  if (mode.h_total < mode.h_display || mode.v_total < mode.v_display || mode.h_sync_start < mode.h_display || mode.v_sync_start < mode.v_display ||
      mode.h_sync_end < mode.h_sync_start || mode.v_sync_end < mode.v_sync_start) return false;
  uint32_t misc = 0;
  if (mode.v_sync_negative) misc |= kModeMiscVSyncPolarity;
  if (mode.h_sync_negative) misc |= kModeMiscHSyncPolarity;
  if (mode.composite_sync) misc |= kModeMiscCompositeSync;
  if (mode.interlaced) misc |= kModeMiscInterlace;
  if (mode.double_scan) misc |= kModeMiscDoubleClock;
  uint32_t words[6] = {uint32_t{mode.h_display} | (uint32_t(mode.h_total - mode.h_display) << 16),
                       uint32_t{mode.v_display} | (uint32_t(mode.v_total - mode.v_display) << 16),
                       uint32_t(mode.h_sync_start - mode.h_display) | (uint32_t(mode.h_sync_end - mode.h_sync_start) << 16),
                       uint32_t(mode.v_sync_start - mode.v_display) | (uint32_t(mode.v_sync_end - mode.v_sync_start) << 16),
                       misc,
                       uint32_t{out.crtc}};
  return run(bios_, kSetCrtcUsingDtdTiming, words, 6);
}

// SET_CRTC_OVERSCAN_PARAMETERS, 12 bytes: four USHORT overscans (right, left, bottom, top), ucCRTC, 3 padding
bool AmdModeSet::overscan_setup(const ModeSetOutput& out) {
  uint8_t crev = 0;
  if (!revision(bios_, kSetCrtcOverscan, &crev) || out.crtc >= kMaxCrtcs) return false;
  uint32_t words[3] = {0, 0, uint32_t{out.crtc}};
  return run(bios_, kSetCrtcOverscan, words, 3);
}

// ENABLE_SCALER_PARAMETERS, 4 bytes: ucScaler (the CRTC), ucEnable (ATOM_SCALER_DISABLE), ucTVStandard, 1 padding
bool AmdModeSet::scaler_setup(const ModeSetOutput& out) {
  uint8_t crev = 0;
  if (!revision(bios_, kEnableScaler, &crev) || out.crtc >= kMaxCrtcs) return false;
  uint32_t words[1] = {uint32_t{out.crtc}};
  return run(bios_, kEnableScaler, words, 1);
}

// SELECT_CRTC_SOURCE_PARAMETERS_V2 / _V3, 4 bytes: ucCRTC, ucEncoderID, ucEncodeMode, ucPadding / ucDstBpc
bool AmdModeSet::select_crtc_source(const ModeSetOutput& out) {
  uint8_t crev = 0;
  if (!revision(bios_, kSelectCrtcSource, &crev) || (crev != 2 && crev != 3) || out.crtc >= kMaxCrtcs || out.dig_encoder > 6) return false;
  const uint32_t bpc = crev == 3 ? panel_bpc(out.bits_per_color) : 0;
  uint32_t words[1] = {uint32_t{out.crtc} | (uint32_t{dig_encoder_id(out.dig_encoder)} << 8) | (uint32_t{out.encoder_mode} << 16) | (bpc << 24)};
  return run(bios_, kSelectCrtcSource, words, 1);
}

// ENABLE_CRTC_PARAMETERS, 4 bytes: ucCRTC, ucEnable, 2 padding (EnableCRTC and UpdateCRTC_DoubleBufferRegisters; EnableDispPowerGating is ucDispPipeId, ucEnable, 2 padding and 16 reserved)
static bool crtc_switch(AtomBios& bios, uint32_t table, uint8_t crtc, bool on) {
  uint8_t crev = 0;
  if (!revision(bios, table, &crev) || crtc >= AmdModeSet::kMaxCrtcs) return false;
  uint32_t words[1] = {uint32_t{crtc} | (uint32_t{on ? 1u : 0u} << 8)};
  return run(bios, table, words, 1);
}
bool AmdModeSet::enable_crtc(uint8_t crtc, bool enable) { return crtc_switch(bios_, kEnableCrtc, crtc, enable); }
bool AmdModeSet::lock_crtc(uint8_t crtc, bool lock) { return crtc_switch(bios_, kUpdateDoubleBuffer, crtc, lock); }
bool AmdModeSet::powergate_crtc(uint8_t crtc, bool enable) { return crtc_switch(bios_, kEnableDispPowerGating, crtc, enable); }

// BLANK_CRTC_PARAMETERS, 8 bytes: ucCRTC, ucBlanking (ATOM_BLANKING 1, ATOM_BLANKINGOFF 0), the black colour as three words (zero)
bool AmdModeSet::blank_crtc(uint8_t crtc, bool blank) {
  uint8_t crev = 0;
  if (!revision(bios_, kBlankCrtc, &crev) || crtc >= kMaxCrtcs) return false;
  uint32_t words[2] = {uint32_t{crtc} | (uint32_t{blank ? 1u : 0u} << 8), 0};
  return run(bios_, kBlankCrtc, words, 2);
}

// DIG_ENCODER_CONTROL_PARAMETERS_V3 / _V4 (8 bytes) or _V5 (the union of the command, stream, link and panel mode parameters, 12 bytes). The tables 1.2 and 1.3 take the same
// parameters. The lane count is the link's for DisplayPort, 8 for dual link and else 4 (decided on the byte the mode or, for the panel mode action, the panel mode is in, as the driver does).
bool AmdModeSet::encoder(const ModeSetOutput& out, const CrtcMode& mode, uint8_t action, uint8_t panel_mode) {
  if (out.dig_encoder > 6) return false;
  uint8_t crev = 0;
  if (!revision(bios_, kDigxEncoderControl, &crev) || crev < 2 || crev > 5) return false;
  const uint8_t mode_byte = action == kEncoderSetupPanelMode ? panel_mode : out.encoder_mode;
  const uint32_t lanes = is_dp(mode_byte) ? out.lanes : (out.dual_link ? 8u : 4u);
  const uint32_t pixel_clock = mode.pixel_clock_khz / 10;
  const uint32_t bpc = panel_bpc(out.bits_per_color);
  if (crev <= 4) {
    uint32_t rate = 0;
    if (is_dp(mode_byte)) {
      if (crev == 4) rate = out.link_rate_khz == 540000 ? 2 : out.link_rate_khz == 324000 ? 3 : out.link_rate_khz == 270000 ? 1 : 0;
      else rate = out.link_rate_khz == 270000 ? 1 : 0;
    }
    const uint32_t config = rate | (uint32_t{out.dig_encoder} << 4);
    const uint32_t hpd = crev == 4 && out.hpd < 6 ? out.hpd + 1u : 0;
    uint32_t words[2] = {(pixel_clock & 0xFFFF) | (config << 16) | (uint32_t{action} << 24), uint32_t{mode_byte} | (lanes << 8) | (bpc << 16) | (hpd << 24)};
    return run(bios_, kDigxEncoderControl, words, 2);
  }
  uint32_t words[3] = {};
  switch (action) {
    case kEncoderSetupPanelMode:
      words[0] = uint32_t{out.dig_encoder} | (uint32_t{action} << 8) | (uint32_t{panel_mode} << 16);
      break;
    case kEncoderSetup:       // ATOM_ENCODER_CMD_STREAM_SETUP
      words[0] = uint32_t{out.dig_encoder} | (uint32_t{action} << 8) | (uint32_t{out.encoder_mode} << 16) | (lanes << 24);
      words[1] = pixel_clock;
      words[2] = bpc | ((out.link_rate_khz / 27000) << 8);
      break;
    default:                  // the training commands, DP video on and off: the DIG and the action
      words[0] = uint32_t{out.dig_encoder} | (uint32_t{action} << 8);
      break;
  }
  return run(bios_, kDigxEncoderControl, words, 3);
}

// DIG_TRANSMITTER_CONTROL_PARAMETERS_V4 (8 bytes), _V1_5 (12) or _V1_6 (16)
bool AmdModeSet::transmitter(const ModeSetOutput& out, const CrtcMode& mode, uint8_t action, uint8_t lane_num, uint8_t lane_set) {
  if (out.dig_encoder > 6) return false;
  uint8_t crev = 0;
  if (!revision(bios_, kTransmitterControl, &crev) || crev < 4 || crev > 6) return false;
  const bool dp = is_dp(out.encoder_mode);
  const bool coherent = dp || out.dfp;      // (dig->coherent_mode is true)
  const uint32_t lanes = dp ? out.lanes : (out.dual_link ? 8u : 4u);
  const uint32_t hpd_sel = out.hpd < 6 ? out.hpd + 1u : 0;
  if (crev == 4) {
    if (out.dig_encoder > 5) return false;       // (three UNIPHY pairs: A/B, C/D, E/F)
    uint32_t first;
    if (action == kTransmitterSetupVsEmph) first = uint32_t{lane_num} | (uint32_t{lane_set} << 8);
    else if (dp) first = (out.link_rate_khz / 10) & 0xFFFF;
    else if (out.dual_link) first = ((mode.pixel_clock_khz / 2) / 10) & 0xFFFF;
    else first = (mode.pixel_clock_khz / 10) & 0xFFFF;
    const uint32_t refclk = dp ? kEncoderRefclkDcpll : (out.pll & 3u);
    const uint32_t config = (!dp && out.dfp && out.dual_link ? 1u : 0u) | (coherent ? 2u : 0u) | (out.link_b ? 1u << 2 : 0u) | (uint32_t{out.dig_encoder & 1u} << 3) |
                            (refclk << 4) | (uint32_t{out.dig_encoder / 2u} << 6);
    uint32_t words[2] = {first | (config << 16) | (uint32_t{action} << 24), lanes};
    return run(bios_, kTransmitterControl, words, 2);
  }
  const uint32_t symbol_clock = dp ? out.link_rate_khz / 10 : mode.pixel_clock_khz / 10;
  if (crev == 5) {
    const uint32_t config = (coherent ? 2u : 0u) | (uint32_t{out.pll & 3u} << 2) | (hpd_sel << 4);
    uint32_t words[3] = {(symbol_clock & 0xFFFF) | (uint32_t{out.dig_encoder} << 16) | (uint32_t{action} << 24),
                         lanes | (uint32_t{out.connector_object_id} << 8) | (uint32_t{out.encoder_mode} << 16) | (config << 24), (1u << out.dig_encoder) | (uint32_t{lane_set} << 8)};
    return run(bios_, kTransmitterControl, words, 3);
  }
  // V1.6: ucPhyId, ucAction, ucDPLaneSet (SETUP_VSEMPH) or ucDigMode, ucLaneNum; ulSymClock; ucHPDSel, ucDigEncoderSel, ucConnObjId, reserved; reserved
  const uint32_t third = action == kTransmitterSetupVsEmph ? lane_set : out.encoder_mode;
  uint32_t words[4] = {uint32_t{out.dig_encoder} | (uint32_t{action} << 8) | (third << 16) | (lanes << 24), symbol_clock,
                       hpd_sel | ((1u << out.dig_encoder) << 8) | (uint32_t{out.connector_object_id} << 16), 0};
  return run(bios_, kTransmitterControl, words, 4);
}

}  // namespace hardware
