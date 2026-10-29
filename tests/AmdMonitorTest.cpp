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

// parse_monitor_info, monitor_bpc and mode_valid against hand-made EDIDs and every branch of the driver's helpers (amdgpu_connectors.c, drm_edid.c v6.12).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdMonitor.hpp"

using namespace hardware;
using Type = AtomConnectors::Type;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A base block (version 1.`revision`, input byte `input`) with `extensions` blocks after it (each filled by the caller); checksums are set last.
static std::vector<uint8_t> edid(uint8_t revision, uint8_t input, std::vector<std::vector<uint8_t>> extensions = {}) {
  std::vector<uint8_t> e(128, 0);
  const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memcpy(e.data(), header, 8);
  e[18] = 1;
  e[19] = revision;
  e[20] = input;
  e[126] = static_cast<uint8_t>(extensions.size());
  for (auto& x : extensions) {
    x.resize(128, 0);
    x[0] = 0x02;
    x[1] = 3;
  }
  auto seal = [](std::vector<uint8_t>& b) { uint8_t s = 0; for (int i = 0; i < 127; ++i) s = static_cast<uint8_t>(s + b[i]); b[127] = static_cast<uint8_t>(0 - s); };
  seal(e);
  for (auto& x : extensions) {
    seal(x);
    e.insert(e.end(), x.begin(), x.end());
  }
  return e;
}

// A CTA extension with data blocks given as (tag, payload) pairs
static std::vector<uint8_t> cta(std::vector<std::pair<int, std::vector<uint8_t>>> blocks) {
  std::vector<uint8_t> x(128, 0);
  uint32_t at = 4;
  for (auto& b : blocks) {
    x[at++] = static_cast<uint8_t>(b.first << 5 | b.second.size());
    for (uint8_t v : b.second) x[at++] = v;
  }
  x[2] = static_cast<uint8_t>(at);
  return x;
}

static void test_parse() {
  MonitorInfo m;
  // not an EDID
  std::vector<uint8_t> bad = edid(4, 0x80);
  CHECK(!parse_monitor_info(ConstByteView(bad.data(), 100), &m));
  bad[0] = 1;
  CHECK(!parse_monitor_info(ConstByteView(bad.data(), bad.size()), &m));
  bad = edid(4, 0x80);
  bad[127] ^= 1;
  CHECK(!parse_monitor_info(ConstByteView(bad.data(), bad.size()), &m));
  // an analog sink and one older than 1.3 give nothing
  std::vector<uint8_t> e = edid(4, 0x00);
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && !m.digital && m.bpc == 0);
  e = edid(2, 0x80);
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && !m.digital);
  // EDID 1.4 digital depths
  const struct { uint8_t depth; uint32_t bpc; } depths[] = {{0, 0}, {1, 6}, {2, 8}, {3, 10}, {4, 12}, {5, 14}, {6, 16}, {7, 0}};
  for (const auto& d : depths) {
    e = edid(4, static_cast<uint8_t>(0x80 | d.depth << 4));
    CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.digital && m.bpc == d.bpc && !m.is_hdmi);
  }
  // 1.3 with DFP 1.x: 8 bpc, without: unknown
  e = edid(3, 0x81);
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.digital && m.bpc == 8);
  e = edid(3, 0x80);
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.digital && m.bpc == 0);
  // an HDMI block: 03 0C 00, source address 10.00, features, max TMDS 340 MHz (68 x 5000), deep colour 36 (and 30)
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 68}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.is_hdmi && m.bpc == 12 && m.max_tmds_clock_khz == 340000 && m.hdmi_dc_modes == (MonitorInfo::kDc30 | MonitorInfo::kDc36));
  // 48 bit wins; 30 only; none
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x40, 68}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.bpc == 16);
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x10, 68}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.bpc == 10);
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 68}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.is_hdmi && m.bpc == 8 && m.hdmi_dc_modes == 0);
  // a short block (no deep colour byte): 8 bpc, no TMDS limit
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.is_hdmi && m.bpc == 8 && m.max_tmds_clock_khz == 0);
  // the 1.4 depth overrides the extension's, even with "undefined"
  e = edid(4, 0x80 | 2 << 4, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 68}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.is_hdmi && m.bpc == 8);
  e = edid(4, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 68}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.is_hdmi && m.bpc == 0);
  // another vendor's block is not HDMI; the HDMI Forum block sets the TMDS limit above 340 MHz only
  e = edid(3, 0x80, {cta({{3, {0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}}, {1, {1, 2, 3}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && !m.is_hdmi && m.max_tmds_clock_khz == 0);
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 68}}, {3, {0xD8, 0x5D, 0xC4, 0x01, 120, 0x00, 0x00}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.max_tmds_clock_khz == 600000);
  e = edid(3, 0x80, {cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 68}}, {3, {0xD8, 0x5D, 0xC4, 0x01, 60, 0x00, 0x00}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.max_tmds_clock_khz == 340000);
  // a block that runs past its extension is ignored, and the second extension is read too
  std::vector<uint8_t> over = cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 68}}});
  over[2] = 6;
  e = edid(3, 0x80, {over, cta({{3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x10, 40}}})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &m) && m.is_hdmi && m.bpc == 10 && m.max_tmds_clock_khz == 200000);
  // an extension count past the data is not read
  e = edid(3, 0x80, {cta({}), cta({}), cta({})});
  CHECK(parse_monitor_info(ConstByteView(e.data(), 256), &m) && m.digital);
}

static void test_bpc() {
  MonitorInfo hdmi;
  hdmi.digital = true; hdmi.is_hdmi = true; hdmi.bpc = 12; hdmi.max_tmds_clock_khz = 340000; hdmi.hdmi_dc_modes = MonitorInfo::kDc30 | MonitorInfo::kDc36;
  // deep colour is off unless asked for
  CHECK(monitor_bpc(Type::HdmiA, true, false, hdmi, 148500, false) == 8);
  CHECK(monitor_bpc(Type::HdmiA, true, false, hdmi, 148500, true) == 12);          // 148500 * 3 / 2 = 222750 <= 340000
  CHECK(monitor_bpc(Type::DviD, true, false, hdmi, 148500, true) == 12);
  // 12 bits does not fit the TMDS clock: 10 if the sink does 30 bit and it fits, else 8
  CHECK(monitor_bpc(Type::HdmiA, true, false, hdmi, 240000, true) == 10);          // 360000 > 340000, 300000 <= 340000
  CHECK(monitor_bpc(Type::HdmiA, true, false, hdmi, 280000, true) == 8);           // 420000 and 350000 are both over
  MonitorInfo no30 = hdmi;
  no30.hdmi_dc_modes = MonitorInfo::kDc36;
  CHECK(monitor_bpc(Type::HdmiA, true, false, no30, 240000, true) == 8);
  MonitorInfo ten = hdmi;
  ten.bpc = 10;
  CHECK(monitor_bpc(Type::HdmiA, true, false, ten, 280000, true) == 8 && monitor_bpc(Type::HdmiA, true, false, ten, 270000, true) == 10);
  // deep colour without a TMDS limit: 8; above 12 clamps to 12
  MonitorInfo nolimit = hdmi;
  nolimit.max_tmds_clock_khz = 0;
  CHECK(monitor_bpc(Type::HdmiA, true, false, nolimit, 148500, true) == 8);
  MonitorInfo sixteen = hdmi;
  sixteen.bpc = 16;
  CHECK(monitor_bpc(Type::HdmiA, true, false, sixteen, 100000, true) == 12);
  // DVI-I and HDMI-B only with a digital connection; DVI-D and HDMI-A always
  CHECK(monitor_bpc(Type::DviI, false, false, hdmi, 148500, true) == 8 && monitor_bpc(Type::DviI, true, false, hdmi, 148500, true) == 12 && monitor_bpc(Type::HdmiB, true, false, hdmi, 148500, true) == 12);
  // DisplayPort: a DisplayPort sink or an HDMI dongle uses the EDID's depth; a DVI dongle 8
  MonitorInfo dp;
  dp.digital = true; dp.bpc = 10;
  CHECK(monitor_bpc(Type::DisplayPort, true, true, dp, 148500, true) == 10 && monitor_bpc(Type::DisplayPort, true, false, dp, 148500, true) == 8 && monitor_bpc(Type::DisplayPort, true, true, dp, 148500, false) == 8);
  MonitorInfo dongle = hdmi;
  CHECK(monitor_bpc(Type::DisplayPort, true, false, dongle, 148500, true) == 12);
  // eDP / LVDS: the EDID's, else the panel's from the BIOS (default 8)
  MonitorInfo none;
  CHECK(monitor_bpc(Type::EmbeddedDisplayPort, true, true, none, 148500, true, 6) == 6 && monitor_bpc(Type::Lvds, true, false, none, 148500, true) == 8);
  MonitorInfo six;
  six.bpc = 6;
  CHECK(monitor_bpc(Type::EmbeddedDisplayPort, true, true, six, 148500, true, 8) == 6);
  // analog connectors: 8
  CHECK(monitor_bpc(Type::Vga, false, false, hdmi, 148500, true) == 8);
}

static ModeCheck check(Type type, uint32_t clock) {
  ModeCheck c;
  c.type = type;
  c.clock_khz = clock;
  c.hdisplay = 1920;
  c.vdisplay = 1080;
  return c;
}

static void test_mode_valid() {
  // DVI / HDMI: up to 165 MHz; above, dual link connectors, else HDMI up to 340, else too high
  ModeCheck c = check(Type::DviD, 148500);
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.clock_khz = 165001;
  c.connector_object_id = 0x03;      // single link DVI-D
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  for (uint8_t dual : {0x02, 0x04, 0x0D}) { c.connector_object_id = dual; CHECK(mode_valid(c) == ModeStatus::Ok); }
  c.connector_object_id = 0x0C;      // HDMI-A
  c.monitor.is_hdmi = true;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.clock_khz = 340001;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  c.clock_khz = 340000;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  // the maximum pixel clock of the BIOS (10 kHz units) for the rest; analog DVI-I is not digital: no 165 MHz rule
  c = check(Type::DviI, 400000);
  c.use_digital = false;
  c.max_pixel_clock_10khz = 40000;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.max_pixel_clock_10khz = 39999;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  c.max_pixel_clock_10khz = 0;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  // eDP / LVDS panels: at least 320x240, not larger than the panel, and without scaling the panel's size
  c = check(Type::EmbeddedDisplayPort, 148500);
  c.native_hdisplay = 1920; c.native_vdisplay = 1080;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.hdisplay = 1280; c.vdisplay = 720;
  CHECK(mode_valid(c) == ModeStatus::Panel);
  c.scaling = true;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.hdisplay = 2560;
  CHECK(mode_valid(c) == ModeStatus::Panel);
  c.hdisplay = 1920; c.vdisplay = 1200;
  CHECK(mode_valid(c) == ModeStatus::Panel);
  c.hdisplay = 319; c.vdisplay = 480;
  CHECK(mode_valid(c) == ModeStatus::Panel);
  c.hdisplay = 640; c.vdisplay = 239;
  CHECK(mode_valid(c) == ModeStatus::Panel);
  c = check(Type::Lvds, 70000);
  CHECK(mode_valid(c) == ModeStatus::Ok);       // (no native mode known)
  // DisplayPort: the link must carry the mode (4 lanes at 5.4 Gbit/s for 4K60 at 8 bits), and HBR2 needs the display clock
  c = check(Type::DisplayPort, 594000);
  c.dp_sink = true;
  c.dpcd.valid = true; c.dpcd.revision = 0x12; c.dpcd.max_link_rate_code = 0x14; c.dpcd.max_lanes = 4;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  c.dp12_capable = true;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.dpcd.max_link_rate_code = 0x0A;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  c.clock_khz = 148500;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.dp12_capable = false;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  // the depth the monitor wants counts: 10 bits needs 30 bits per pixel (4 lanes at 2.7 carry 288000)
  c.clock_khz = 300000;
  c.deep_color = true;
  c.monitor.bpc = 10;
  c.monitor.digital = true;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  c.deep_color = false;
  CHECK(mode_valid(c) == ModeStatus::Ok);       // (8 bits: 360000)
  // a DisplayPort connector with a dongle: the HDMI or DVI limits
  c = check(Type::DisplayPort, 340000);
  c.monitor.is_hdmi = true;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.clock_khz = 340001;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
  c.monitor.is_hdmi = false;
  c.clock_khz = 165000;
  CHECK(mode_valid(c) == ModeStatus::Ok);
  c.clock_khz = 165001;
  CHECK(mode_valid(c) == ModeStatus::ClockHigh);
}

static void test_detailed_timing() {
  // 1080p60: pixel clock 148.5 MHz, 1920 + 280 / 1080 + 45, sync offsets 88 / 4, widths 44 / 5, positive syncs
  std::vector<uint8_t> e = edid(3, 0x80);
  const uint8_t t1080[18] = {0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0, 0, 0, 0, 0, 0x1E};
  memcpy(&e[54], t1080, 18);
  CrtcMode m;
  CHECK(edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m));
  CHECK(m.pixel_clock_khz == 148500 && m.h_display == 1920 && m.h_sync_start == 2008 && m.h_sync_end == 2052 && m.h_total == 2200 && m.v_display == 1080 && m.v_sync_start == 1084 &&
        m.v_sync_end == 1089 && m.v_total == 1125 && !m.h_sync_negative && !m.v_sync_negative && !m.interlaced);
  // negative syncs (flags without the polarity bits); interlaced (1080i: 1920 x 540 per field, the total odd)
  e[54 + 17] = 0x18;
  CHECK(edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m) && m.h_sync_negative && m.v_sync_negative);
  e[54 + 17] = 0x9E;
  e[54 + 5] = 0x1C; e[54 + 6] = 0x16; e[54 + 7] = 0x20;       // 540 active (0x21C), 22 blank
  CHECK(edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m) && m.interlaced && m.v_display == 540 && m.v_total == (562 | 1));
  // monitor descriptors (clock 0), stereo, analog sync, no sync width, a slot past the fourth
  e[54 + 17] = 0x1E; e[54 + 5] = 0x38; e[54 + 6] = 0x2D; e[54 + 7] = 0x40;
  CHECK(!edid_detailed_timing(ConstByteView(e.data(), e.size()), 1, &m) && !edid_detailed_timing(ConstByteView(e.data(), e.size()), 4, &m));
  e[54 + 17] = 0x3E;
  CHECK(!edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m));
  e[54 + 17] = 0x06;
  CHECK(!edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m));
  e[54 + 17] = 0x1E; e[54 + 9] = 0;
  CHECK(!edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m));
  e[54 + 9] = 0x2C;
  // a sync that ends after the total is cut at it
  e[54 + 9] = 0xFF; e[54 + 11] = 0x30;      // width 0x3FF
  CHECK(edid_detailed_timing(ConstByteView(e.data(), e.size()), 0, &m) && m.h_sync_end == m.h_total);
  CHECK(!edid_detailed_timing(ConstByteView(e.data(), 100), 0, &m));
}

int main() {
  test_parse();
  test_bpc();
  test_mode_valid();
  test_detailed_timing();
  printf(g_failed ? "AmdMonitorTest: %d FAILED\n" : "AmdMonitorTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
