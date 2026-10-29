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
constexpr uint32_t kModeMiscHSyncPolarity = 0x02, kModeMiscVSyncPolarity = 0x04, kModeMiscInterlace = 0x80, kModeMiscDoubleClock = 0x100;
constexpr uint8_t kMiscDviDualLink = 0x08;

bool revision(AtomBios& bios, uint32_t table, uint8_t* crev, uint8_t want_frev = 1) {
  uint8_t frev = 0;
  return bios.cmd_table_header(table, &frev, crev) && frev == want_frev;
}
bool run(AtomBios& bios, uint32_t table, uint32_t* words, uint32_t count) {
  return bios.execute_table(table, ArrayView<uint32_t>(words, count)) == AtomBios::Status::Ok;
}
}  // namespace

uint8_t AmdModeSet::dig_encoder_id(uint8_t dig) {
  static constexpr uint8_t kIds[] = {0x03, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};      // ASIC_INT_DIG1_ENCODER_ID, DIG2 ... DIG7
  return dig < sizeof kIds ? kIds[dig] : 0;
}

int AmdModeSet::link_rate_bits(uint32_t rate_khz, uint8_t crev) {
  switch (rate_khz) {
    case 162000: return 0;
    case 270000: return 1;
    case 540000: return crev >= 4 ? 2 : -1;
    case 324000: return crev >= 4 ? 3 : -1;
    default: return -1;
  }
}

// PIXEL_CLOCK_PARAMETERS_V7, 16 bytes: ulPixelClock (100 Hz), ucPpll, ucTransmitterID, ucEncoderMode, ucMiscInfo, ucCRTC, ucDeepColorRatio, 2 + 4 reserved
bool AmdModeSet::set_pixel_clock(const ModeSetOutput& out, const DisplayMode& mode) {
  uint8_t crev = 0;
  if (!revision(bios_, kSetPixelClock, &crev) || crev != 7 || out.crtc >= kMaxCrtcs || mode.pixel_clock_khz > 0xFFFFFFFFu / 10) return false;
  uint32_t misc = 0;
  if (out.encoder_mode == kModeDvi && mode.pixel_clock_khz > 165000) misc |= kMiscDviDualLink;
  uint32_t deep = 0;
  if (out.encoder_mode == kModeHdmi) deep = out.bits_per_color == 10 ? 1 : out.bits_per_color == 12 ? 2 : 0;
  uint32_t words[4] = {mode.pixel_clock_khz * 10, uint32_t{out.pll} | (uint32_t{out.encoder_id} << 8) | (uint32_t{out.encoder_mode} << 16) | (misc << 24),
                       uint32_t{out.crtc} | (deep << 8), 0};
  return run(bios_, kSetPixelClock, words, 4);
}

// SET_CRTC_TIMING_PARAMETERS, 24 bytes: usH_Total, usH_Disp, usH_SyncStart, usH_SyncWidth, usV_Total, usV_Disp, usV_SyncStart, usV_SyncWidth, susModeMiscInfo, ucCRTC, 4 overscan, reserved
bool AmdModeSet::set_crtc_timing(const ModeSetOutput& out, const DisplayMode& mode) {
  uint8_t crev = 0;
  if (!revision(bios_, kSetCrtcTiming, &crev) || out.crtc >= kMaxCrtcs || mode.h_sync_end < mode.h_sync_start || mode.v_sync_end < mode.v_sync_start) return false;
  uint32_t misc = 0;
  if (mode.v_sync_negative) misc |= kModeMiscVSyncPolarity;
  if (mode.h_sync_negative) misc |= kModeMiscHSyncPolarity;
  if (mode.interlaced) misc |= kModeMiscInterlace;
  if (mode.double_scan) misc |= kModeMiscDoubleClock;
  uint32_t words[6] = {uint32_t{mode.h_total} | (uint32_t{mode.h_display} << 16),
                       uint32_t{mode.h_sync_start} | (uint32_t(mode.h_sync_end - mode.h_sync_start) << 16),
                       uint32_t{mode.v_total} | (uint32_t{mode.v_display} << 16),
                       uint32_t{mode.v_sync_start} | (uint32_t(mode.v_sync_end - mode.v_sync_start) << 16),
                       misc | (uint32_t{out.crtc} << 16),
                       0};
  return run(bios_, kSetCrtcTiming, words, 6);
}

// SELECT_CRTC_SOURCE_PARAMETERS_V2 / _V3, 4 bytes: ucCRTC, ucEncoderID, ucEncodeMode, ucPadding / ucDstBpc
bool AmdModeSet::select_crtc_source(const ModeSetOutput& out) {
  uint8_t crev = 0;
  if (!revision(bios_, kSelectCrtcSource, &crev) || (crev != 2 && crev != 3) || out.crtc >= kMaxCrtcs || out.dig_encoder > 6) return false;
  const uint32_t bpc = out.bits_per_color == 6 ? 1 : out.bits_per_color == 10 ? 3 : out.bits_per_color == 12 ? 4 : out.bits_per_color == 16 ? 5 : 2;     // PANEL_*BIT_PER_COLOR
  uint32_t words[1] = {uint32_t{out.crtc} | (uint32_t{dig_encoder_id(out.dig_encoder)} << 8) | (uint32_t{out.encoder_mode} << 16) | ((crev == 3 ? bpc : 0u) << 24)};
  return run(bios_, kSelectCrtcSource, words, 1);
}

// ENABLE_CRTC_PARAMETERS, 4 bytes: ucCRTC, ucEnable, 2 padding (EnableCRTC and UpdateCRTC_DoubleBufferRegisters)
bool AmdModeSet::enable_crtc(uint8_t crtc, bool enable) {
  uint8_t crev = 0;
  if (!revision(bios_, kEnableCrtc, &crev) || crtc >= kMaxCrtcs) return false;
  uint32_t words[1] = {uint32_t{crtc} | (uint32_t{enable ? 1u : 0u} << 8)};
  return run(bios_, kEnableCrtc, words, 1);
}

bool AmdModeSet::lock_crtc(uint8_t crtc, bool lock) {
  uint8_t crev = 0;
  if (!revision(bios_, kUpdateDoubleBuffer, &crev) || crtc >= kMaxCrtcs) return false;
  uint32_t words[1] = {uint32_t{crtc} | (uint32_t{lock ? 1u : 0u} << 8)};
  return run(bios_, kUpdateDoubleBuffer, words, 1);
}

// BLANK_CRTC_PARAMETERS, 8 bytes: ucCRTC, ucBlanking (ATOM_BLANKING 1, ATOM_BLANKINGOFF 0), the black colour as three words (zero)
bool AmdModeSet::blank_crtc(uint8_t crtc, bool blank) {
  uint8_t crev = 0;
  if (!revision(bios_, kBlankCrtc, &crev) || crtc >= kMaxCrtcs) return false;
  uint32_t words[2] = {uint32_t{crtc} | (uint32_t{blank ? 1u : 0u} << 8), 0};
  return run(bios_, kBlankCrtc, words, 2);
}

// DIG_ENCODER_CONTROL_PARAMETERS_V3 / _V4, 8 bytes (as AmdDpSource::encoder, with the output's mode)
bool AmdModeSet::encoder(const ModeSetOutput& out, const DisplayMode& mode, uint8_t action) {
  if (out.dig_encoder > 6) return false;
  const uint32_t table = out.dig_encoder ? kEncoder2Table : kEncoder1Table;
  uint8_t crev = 0;
  if (!revision(bios_, table, &crev) || (crev != 3 && crev != 4)) return false;
  uint32_t rate = 0;
  if (out.encoder_mode == kModeDp && out.link_rate_khz != 0) {      // (no rate: an action that does not train, DP video off, disable)
    const int bits = link_rate_bits(out.link_rate_khz, crev);
    if (bits < 0) return false;
    rate = static_cast<uint32_t>(bits);
  }
  const uint32_t bpc = out.bits_per_color == 6 ? 1 : out.bits_per_color == 10 ? 3 : out.bits_per_color == 12 ? 4 : out.bits_per_color == 16 ? 5 : 2;
  const uint32_t hpd = crev >= 4 && out.hpd < 6 ? out.hpd + 1u : 0;
  const uint32_t config = rate | (uint32_t{out.dig_encoder} << 4);
  const uint32_t lanes = out.encoder_mode == kModeDp ? out.lanes : (out.dual_link ? 8u : 4u);
  uint32_t words[2] = {((mode.pixel_clock_khz / 10) & 0xFFFF) | (config << 16) | (uint32_t{action} << 24), uint32_t{out.encoder_mode} | (lanes << 8) | (bpc << 16) | (hpd << 24)};
  return run(bios_, table, words, 2);
}

// DIG_TRANSMITTER_CONTROL_PARAMETERS_V4 (8 bytes: lane select and set, ucConfig, ucAction, ucLaneNum) or _V1_5 (12 bytes, as AmdDpSource::transmitter)
bool AmdModeSet::transmitter(const ModeSetOutput& out, const DisplayMode& mode, uint8_t action) {
  if (out.dig_encoder > 6) return false;
  uint8_t crev = 0;
  if (!revision(bios_, kTransmitterTable, &crev)) return false;
  const bool dp = out.encoder_mode == kModeDp;
  const bool coherent = dp || out.encoder_mode == kModeHdmi;
  const uint32_t lanes = dp ? out.lanes : (out.dual_link ? 8u : 4u);
  // the symbol clock in 10 kHz: the link rate over 10 for DP, else the pixel clock scaled by the HDMI deep colour ratio
  uint32_t clock = mode.pixel_clock_khz;
  if (out.encoder_mode == kModeHdmi && out.bits_per_color > 8) clock = clock / 8 * out.bits_per_color;
  const uint32_t symbol_clock = dp ? out.link_rate_khz / 10 : clock / 10;
  if (crev == 4) {
    if (out.dig_encoder > 5) return false;
    const uint32_t config = (out.dual_link ? 1u : 0u) | (coherent ? 2u : 0u) | (out.link_b ? 1u << 2 : 0u) | (uint32_t{out.dig_encoder & 1u} << 3) |
                            (uint32_t{out.reference_clock_source & 3u} << 4) | (uint32_t{out.dig_encoder / 2u} << 6);
    uint32_t words[3] = {(symbol_clock & 0xFFFF) | (config << 16) | (uint32_t{action} << 24), lanes, 0};
    // V4: usPixelClock is the first word; the lane set / select word is zero here
    return run(bios_, kTransmitterTable, words, 3);
  }
  if (crev == 5) {
    const uint32_t hpd_sel = out.hpd < 6 ? out.hpd + 1u : 0;
    const uint32_t config = (coherent ? 2u : 0u) | (uint32_t{out.reference_clock_source & 3u} << 2) | (hpd_sel << 4);
    uint32_t words[3] = {(symbol_clock & 0xFFFF) | (uint32_t{out.dig_encoder} << 16) | (uint32_t{action} << 24),
                         lanes | (uint32_t{out.connector_object_id} << 8) | (uint32_t{out.encoder_mode} << 16) | (config << 24), 1u << out.dig_encoder};
    return run(bios_, kTransmitterTable, words, 3);
  }
  return false;
}

}  // namespace hardware
