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

// AmdModeSet against an AtomBIOS image whose mode setting tables hand their parameter dwords to the simulated card: the exact words of SetPixelClock, SetCRTC_UsingDTDTiming,
// SetCRTC_OverScan, EnableScaler, SelectCRTC_Source, EnableCRTC, BlankCRTC, UpdateCRTC_DoubleBufferRegisters, EnableDispPowerGating and the DIGx encoder and transmitter control
// tables of every revision handled (words as amdgpu's atombios_crtc.c and atombios_encoders.c build them), and the refusals.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"
#include "AtomBiosImage.hpp"

using namespace hardware;
using namespace atomimage;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// Table slot k copies its parameter dwords to registers 0x7400 + 0x10 * k; the last one written ends a call. The number of dwords depends on the revision.
struct Slot { uint32_t index; uint8_t frev, crev; };
static const uint32_t kIndexes[] = {4, 12, 13, 33, 34, 35, 40, 42, 44, 49, 76};
static uint32_t default_crev(uint32_t index) {
  switch (index) {
    case 4: return 5;
    case 12: return 7;
    case 42: return 3;
    case 76: return 5;
    default: return 1;
  }
}
static uint32_t words_of(uint32_t index, uint32_t crev) {
  switch (index) {
    case 4: return crev <= 4 ? 2 : 3;
    case 12: return 4;
    case 33: case 35: case 13: case 42: case 44: return 1;
    case 34: return 2;
    case 40: return 3;
    case 49: return 6;
    case 76: return crev == 4 ? 2 : crev == 5 ? 3 : 4;
    default: return 1;
  }
}

struct Card final : IAtomHost {
  struct Call {
    uint32_t table;
    std::vector<uint32_t> words;
  };
  std::vector<Call> calls;
  uint32_t regs[0x200] = {};
  uint32_t count[16] = {};
  uint32_t read_register(uint32_t) override { return 0; }
  void write_register(uint32_t reg, uint32_t value) override {
    if (reg < 0x7400 || reg >= 0x7500) return;
    regs[reg - 0x7400] = value;
    const uint32_t slot = (reg - 0x7400) / 0x10;
    if (slot >= sizeof kIndexes / sizeof kIndexes[0] || !count[slot]) return;
    if ((reg - 0x7400) % 0x10 == count[slot] - 1) {
      Call c{kIndexes[slot], {}};
      for (uint32_t i = 0; i < count[slot]; ++i) c.words.push_back(regs[slot * 0x10 + i]);
      calls.push_back(c);
    }
  }
  uint32_t read_mc(uint32_t) override { return 0; }
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t) override {}
  void delay_ms(uint32_t) override {}
  uint64_t now_us() override { return ++clock_; }
  uint64_t clock_ = 0;
};

struct Rig {
  TestAllocator alloc;
  Card card;
  std::vector<uint8_t> image;
  std::unique_ptr<AtomBios> bios;
  // `override_index`: the revision of that table replaced (0: the table is absent)
  explicit Rig(uint32_t override_index = 0xFFFF, uint8_t override_crev = 0, uint8_t override_frev = 1) {
    Image img;
    uint32_t slot = 0;
    for (uint32_t index : kIndexes) {
      uint8_t crev = static_cast<uint8_t>(default_crev(index)), frev = 1;
      if (index == override_index) { crev = override_crev; frev = override_frev; }
      if (crev) {
        const uint32_t n = words_of(index, crev);
        card.count[slot] = n;
        Code c;
        for (uint32_t i = 0; i < n; ++i) c.move(REG, 0x7400 + slot * 0x10 + i, PS, i);
        c.eot();
        img.tables.push_back({index, n * 4, n * 4, c, frev, crev});
      }
      ++slot;
    }
    image = img.build();
    bios.reset(new AtomBios(card, ConstByteView(image.data(), image.size()), alloc));
  }
};

static DisplayMode mode_1080p() {
  DisplayMode m;
  m.pixel_clock_khz = 148500;
  m.h_display = 1920; m.h_sync_start = 2008; m.h_sync_end = 2052; m.h_total = 2200;
  m.v_display = 1080; m.v_sync_start = 1084; m.v_sync_end = 1089; m.v_total = 1125;
  return m;
}

static ModeSetOutput output(uint8_t encoder_mode) {
  ModeSetOutput o;
  o.crtc = 2;
  o.pll = AmdModeSet::kPpll1;
  o.encoder_id = 0x1E;
  o.dig_encoder = 3;
  o.link_b = true;
  o.connector_object_id = 19;
  o.hpd = 2;
  o.encoder_mode = encoder_mode;
  o.lanes = 2;
  o.link_rate_khz = 270000;
  return o;
}

static void test_pixel_clock() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  DisplayMode m = mode_1080p();
  ModeSetOutput o = output(AmdModeSet::kModeDp);
  CHECK(s.set_pixel_clock(o, m));
  // 148.5 MHz in 100 Hz units; PPLL1, encoder 0x1E, mode DP, no misc; CRTC 2, no deep colour
  CHECK(r.card.calls.size() == 1 && r.card.calls[0].table == 12 && r.card.calls[0].words == std::vector<uint32_t>({1485000, 0x1Eu << 8, 2, 0}));
  // HDMI deep colour (the clock stays the mode's): 10 bits 5:4, 12 bits 3:2, 16 bits 2:1, 8 none
  o = output(AmdModeSet::kModeHdmi);
  const struct { uint32_t bpc, ratio; } depths[] = {{8, 0}, {10, 1}, {12, 2}, {16, 3}, {6, 0}};
  for (const auto& d : depths) {
    r.card.calls.clear();
    o.bits_per_color = d.bpc;
    CHECK(s.set_pixel_clock(o, m) && r.card.calls[0].words[2] == (2u | d.ratio << 8) && ((r.card.calls[0].words[1] >> 16) & 0xFF) == 3 && r.card.calls[0].words[0] == 1485000);
  }
  // DVI: dual link above 165 MHz; no deep colour ratio
  o = output(AmdModeSet::kModeDvi);
  o.bits_per_color = 12;
  r.card.calls.clear();
  CHECK(s.set_pixel_clock(o, m) && (r.card.calls[0].words[1] >> 24) == 0 && r.card.calls[0].words[2] == 2);
  m.pixel_clock_khz = 165001;
  CHECK(s.set_pixel_clock(o, m) && (r.card.calls[1].words[1] >> 24) == 8);
  m.pixel_clock_khz = 165000;
  CHECK(s.set_pixel_clock(o, m) && (r.card.calls[2].words[1] >> 24) == 0);
  // the PLL turned off: no clock, no encoder
  ModeSetOutput off;
  off.crtc = 4;
  off.pll = AmdModeSet::kPpll2;
  r.card.calls.clear();
  CHECK(s.set_pixel_clock(off, DisplayMode()) && r.card.calls[0].words == std::vector<uint32_t>({0, 1u, 4u, 0}));
  // refused: a CRTC the card does not have, a clock that does not fit
  r.card.calls.clear();
  o.crtc = 6;
  CHECK(!s.set_pixel_clock(o, m) && r.card.calls.empty());
  o.crtc = 0;
  m.pixel_clock_khz = 0xFFFFFFFFu;
  CHECK(!s.set_pixel_clock(o, m) && r.card.calls.empty());
  // other revisions: refused; no table; another format revision
  const struct { uint8_t crev, frev; } refused[] = {{6, 1}, {8, 1}, {1, 1}, {7, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(12, rf.crev, rf.frev);
    AmdModeSet y(x.card, *x.bios);
    CHECK(!y.set_pixel_clock(output(0), mode_1080p()) && x.card.calls.empty());
  }
}

static void test_crtc_timing() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  DisplayMode m = mode_1080p();
  ModeSetOutput o = output(AmdModeSet::kModeDp);
  CHECK(s.set_dtd_timing(o, m));
  // H size, H blanking (to the total: 2200 - 1920), V size, V blanking, H sync offset (2008 - 1920) and width (44), V sync offset (4) and width (5), the misc flags, the border bytes, the CRTC
  CHECK(r.card.calls[0].table == 49 && r.card.calls[0].words == std::vector<uint32_t>({1920u | 280u << 16, 1080u | 45u << 16, 88u | 44u << 16, 4u | 5u << 16, 0u, 2u}));
  // the flags: vertical sync low 4, horizontal sync low 2, composite 0x40, interlace 0x80, double clock 0x100
  m.v_sync_negative = true;
  CHECK(s.set_dtd_timing(o, m) && r.card.calls[1].words[4] == 0x04);
  m.h_sync_negative = true;
  CHECK(s.set_dtd_timing(o, m) && r.card.calls[2].words[4] == 0x06);
  m.composite_sync = true;
  CHECK(s.set_dtd_timing(o, m) && r.card.calls[3].words[4] == 0x46);
  m.interlaced = true;
  m.double_scan = true;
  CHECK(s.set_dtd_timing(o, m) && r.card.calls[4].words[4] == 0x1C6);
  // refused: totals below the display, a sync before the display or ending before it starts, a CRTC that does not exist
  const size_t n = r.card.calls.size();
  const struct { uint16_t DisplayMode::*field; uint16_t value; } bad[] = {{&DisplayMode::h_total, 1919}, {&DisplayMode::v_total, 1079}, {&DisplayMode::h_sync_start, 1919}, {&DisplayMode::v_sync_start, 1079},
                                                                         {&DisplayMode::h_sync_end, 2007}, {&DisplayMode::v_sync_end, 1083}};
  for (const auto& b : bad) {
    DisplayMode x = mode_1080p();
    x.*(b.field) = b.value;
    CHECK(!s.set_dtd_timing(o, x));
  }
  o.crtc = 6;
  CHECK(!s.set_dtd_timing(o, mode_1080p()) && r.card.calls.size() == n);
  Rig x(49, 0);
  CHECK(!AmdModeSet(x.card, *x.bios).set_dtd_timing(output(0), mode_1080p()));
}

static void test_crtc_controls() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  ModeSetOutput o = output(AmdModeSet::kModeHdmi);
  // SelectCRTC_Source V3: CRTC 2, DIG 3 is ASIC_INT_DIG4_ENCODER_ID 0x0B, mode HDMI, 8 bits is 2
  CHECK(s.select_crtc_source(o) && r.card.calls[0].table == 42 && r.card.calls[0].words == std::vector<uint32_t>({2u | 0x0Bu << 8 | 3u << 16 | 2u << 24}));
  const uint8_t ids[7] = {0x03, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
  for (uint8_t d = 0; d < 7; ++d) CHECK(AmdModeSet::dig_encoder_id(d) == ids[d]);
  CHECK(AmdModeSet::dig_encoder_id(7) == 0);
  const struct { uint32_t bpc, code; } depths[] = {{0, 0}, {6, 1}, {8, 2}, {10, 3}, {12, 4}, {16, 5}, {7, 2}};
  for (const auto& d : depths) {
    r.card.calls.clear();
    o.bits_per_color = d.bpc;
    CHECK(AmdModeSet::panel_bpc(d.bpc) == d.code && s.select_crtc_source(o) && (r.card.calls[0].words[0] >> 24) == d.code);
  }
  // V2 has no depth
  {
    Rig x(42, 2);
    CHECK(AmdModeSet(x.card, *x.bios).select_crtc_source(o) && (x.card.calls[0].words[0] >> 24) == 0);
    Rig y(42, 1);
    CHECK(!AmdModeSet(y.card, *y.bios).select_crtc_source(o));
  }
  o.dig_encoder = 7;
  CHECK(!s.select_crtc_source(o));
  o = output(0);
  o.crtc = 6;
  CHECK(!s.select_crtc_source(o));
  // EnableCRTC, BlankCRTC, UpdateCRTC_DoubleBufferRegisters, EnableDispPowerGating
  r.card.calls.clear();
  CHECK(s.enable_crtc(5, true) && s.enable_crtc(1, false) && s.blank_crtc(3, true) && s.blank_crtc(0, false) && s.lock_crtc(4, true) && s.lock_crtc(2, false) && s.powergate_crtc(1, true) &&
        s.powergate_crtc(0, false));
  CHECK(r.card.calls.size() == 8);
  CHECK(r.card.calls[0].table == 35 && r.card.calls[0].words == std::vector<uint32_t>({5u | 1u << 8}));
  CHECK(r.card.calls[1].words == std::vector<uint32_t>({1u}));
  CHECK(r.card.calls[2].table == 34 && r.card.calls[2].words == std::vector<uint32_t>({3u | 1u << 8, 0}));
  CHECK(r.card.calls[3].words == std::vector<uint32_t>({0u, 0}));
  CHECK(r.card.calls[4].table == 44 && r.card.calls[4].words == std::vector<uint32_t>({4u | 1u << 8}) && r.card.calls[5].words == std::vector<uint32_t>({2u}));
  CHECK(r.card.calls[6].table == 13 && r.card.calls[6].words == std::vector<uint32_t>({1u | 1u << 8}) && r.card.calls[7].words == std::vector<uint32_t>({0u}));
  // the overscan (all zero: RMX off) and the scaler (disabled), with the CRTC
  r.card.calls.clear();
  o = output(0);
  CHECK(s.overscan_setup(o) && s.scaler_setup(o));
  CHECK(r.card.calls[0].table == 40 && r.card.calls[0].words == std::vector<uint32_t>({0, 0, 2}) && r.card.calls[1].table == 33 && r.card.calls[1].words == std::vector<uint32_t>({2}));
  r.card.calls.clear();
  CHECK(!s.enable_crtc(6, true) && !s.blank_crtc(6, true) && !s.lock_crtc(6, true) && !s.powergate_crtc(6, true));
  o.crtc = 6;
  CHECK(!s.overscan_setup(o) && !s.scaler_setup(o) && r.card.calls.empty());
  Rig x(35, 0), y(34, 0), z(44, 0), w(13, 0), v(40, 0), u(33, 0);
  CHECK(!AmdModeSet(x.card, *x.bios).enable_crtc(0, true) && !AmdModeSet(y.card, *y.bios).blank_crtc(0, true) && !AmdModeSet(z.card, *z.bios).lock_crtc(0, true) &&
        !AmdModeSet(w.card, *w.bios).powergate_crtc(0, true) && !AmdModeSet(v.card, *v.bios).overscan_setup(output(0)) && !AmdModeSet(u.card, *u.bios).scaler_setup(output(0)));
}

static void test_encoder() {
  const DisplayMode m = mode_1080p();
  {
    // V4: DP on DIG 3: pixel clock 14850, link rate 2.7 GHz (1) | DIG 3 << 4, action 0x0F; mode DP, two lanes, 8 bits, HPD 3
    Rig r(4, 4);
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup));
    CHECK(r.card.calls[0].table == 4 && r.card.calls[0].words == std::vector<uint32_t>({14850u | 0x31u << 16 | 0x0Fu << 24, 0u | 2u << 8 | 2u << 16 | 3u << 24}));
    // the other rates: 5.4 is 2, 3.24 is 3, 1.62 and anything else 0
    const struct { uint32_t khz, bits; } rates[] = {{540000, 2}, {324000, 3}, {270000, 1}, {162000, 0}, {216000, 0}};
    for (const auto& rt : rates) {
      r.card.calls.clear();
      o.link_rate_khz = rt.khz;
      CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup) && ((r.card.calls[0].words[0] >> 16) & 3) == rt.bits);
    }
    // DIG 0 uses the same table; DP video on / off
    o = output(AmdModeSet::kModeDp);
    o.dig_encoder = 0;
    r.card.calls.clear();
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderDpVideoOn) && s.encoder(o, m, AmdModeSet::kEncoderDpVideoOff));
    CHECK(r.card.calls[0].table == 4 && (r.card.calls[0].words[0] >> 24) == 0x0D && (r.card.calls[1].words[0] >> 24) == 0x0C && ((r.card.calls[0].words[0] >> 16) & 0xFF) == 0x01);
    // HDMI: no link rate, four lanes, mode 3, 10 bits is 3
    o = output(AmdModeSet::kModeHdmi);
    o.bits_per_color = 10;
    r.card.calls.clear();
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({14850u | 0x30u << 16 | 0x0Fu << 24, 3u | 4u << 8 | 3u << 16 | 3u << 24}));
    // dual link DVI: eight lanes
    o = output(AmdModeSet::kModeDvi);
    o.dual_link = true;
    CHECK(s.encoder(o, m, 1) && (r.card.calls[1].words[1] & 0xFF00) == 8u << 8);
    // the panel mode: the byte that is the mode holds the panel mode, and the lanes follow it (0 is DP's, so the link's; 1 and 0x11 are not)
    o = output(AmdModeSet::kModeDp);
    r.card.calls.clear();
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetupPanelMode, 1) && r.card.calls[0].words[1] == (1u | 4u << 8 | 2u << 16 | 3u << 24) && (r.card.calls[0].words[0] >> 24) == 0x10);
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetupPanelMode, 0) && r.card.calls[1].words[1] == (0u | 2u << 8 | 2u << 16 | 3u << 24));
    // refused: DIG 7
    o.dig_encoder = 7;
    CHECK(!s.encoder(o, m, AmdModeSet::kEncoderSetup));
  }
  {
    // V3 and V2: no HPD id, only 2.7 GHz is a rate bit (bit 0)
    for (uint8_t crev : {2, 3}) {
      Rig r(4, crev);
      AmdModeSet s(r.card, *r.bios);
      ModeSetOutput o = output(AmdModeSet::kModeDp);
      CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup) && (r.card.calls[0].words[1] >> 24) == 0 && ((r.card.calls[0].words[0] >> 16) & 0xFF) == 0x31);
      o.link_rate_khz = 540000;
      CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup) && ((r.card.calls[1].words[0] >> 16) & 0xFF) == 0x30);
    }
  }
  {
    // V5: the stream setup (DIG, action, mode, lanes; the pixel clock in 10 kHz; bits per colour and the link rate in 270 MHz units), the panel mode, and the commands
    Rig r;
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({3u | 0x0Fu << 8 | 0u << 16 | 2u << 24, 14850u, 2u | 10u << 8}));
    o = output(AmdModeSet::kModeHdmi);
    o.link_rate_khz = 0;
    o.bits_per_color = 12;
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup) && r.card.calls[1].words == std::vector<uint32_t>({3u | 0x0Fu << 8 | 3u << 16 | 4u << 24, 14850u, 4u}));
    CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetupPanelMode, 1) && r.card.calls[2].words == std::vector<uint32_t>({3u | 0x10u << 8 | 1u << 16, 0, 0}));
    const uint8_t commands[] = {0x08, 0x09, 0x0A, 0x13, 0x0B, 0x0C, 0x0D};
    for (uint8_t c : commands) {
      r.card.calls.clear();
      CHECK(s.encoder(o, m, c) && r.card.calls[0].words == std::vector<uint32_t>({3u | uint32_t{c} << 8, 0, 0}));
    }
    o.dig_encoder = 7;
    CHECK(!s.encoder(o, m, AmdModeSet::kEncoderSetup));
  }
  // refused: other revisions, no table
  const struct { uint8_t crev, frev; } refused[] = {{1, 1}, {6, 1}, {5, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(4, rf.crev, rf.frev);
    CHECK(!AmdModeSet(x.card, *x.bios).encoder(output(AmdModeSet::kModeDp), m, 1) && x.card.calls.empty());
  }
}

static void test_transmitter() {
  const DisplayMode m = mode_1080p();
  {
    // V1.5: DP on DIG 3: symbol clock = link rate / 10 = 27000, DIG 3 as the PHY id, action; two lanes, connector 19, mode DP, config (HPD3 sel 0x30 | coherent 2 | PLL1 << 2 = 0 for
    // PPLL1... the PLL id is the PHY's clock source); DIG 3 select bit
    Rig r;
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].table == 76 && r.card.calls[0].words == std::vector<uint32_t>({27000u | 3u << 16 | 1u << 24, 2u | 19u << 8 | 0u << 16 | 0x32u << 24, 1u << 3}));
    // the drive of all lanes (SETUP_VSEMPH): the lane set in the third word's second byte
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterSetupVsEmph, 0, 0x09) && r.card.calls[0].words == std::vector<uint32_t>({27000u | 3u << 16 | 11u << 24, 2u | 19u << 8 | 0x32u << 24, (1u << 3) | 9u << 8}));
    // the PLL as the PHY's clock source: PPLL0 (2) in bits 3:2
    o.pll = AmdModeSet::kPpll0;
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable) && (r.card.calls[0].words[1] >> 24) == (0x30u | 2u | 2u << 2));
    // HDMI: the pixel clock over ten (no deep colour scaling), mode 3, four lanes, coherent
    o = output(AmdModeSet::kModeHdmi);
    o.bits_per_color = 10;
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({14850u | 3u << 16 | 1u << 24, 4u | 19u << 8 | 3u << 16 | 0x32u << 24, 1u << 3}));
    // the LVDS panel (not a flat panel device): not coherent; DVI dual link: eight lanes (the symbol clock stays the pixel clock in V1.5)
    o = output(AmdModeSet::kModeLvds);
    o.dfp = false;
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable) && (r.card.calls[1].words[1] >> 24) == 0x30);
    o = output(AmdModeSet::kModeDvi);
    o.dual_link = true;
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterDisable) && (r.card.calls[2].words[1] & 0xFF) == 8 && (r.card.calls[2].words[0] & 0xFFFF) == 14850);
    o.dig_encoder = 7;
    CHECK(!s.transmitter(o, m, 1));
  }
  {
    // V4: symbol clock, config (dual link 1 | coherent 2 | link B 4 | front end 8 for odd DIGs | reference clock << 4 | transmitter << 6), action, lanes
    Rig r(76, 4);
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    // DisplayPort is clocked from the display clock PLL (2) whatever the PLL of the CRTC
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({27000u | (2u | 4u | 8u | 2u << 4 | 1u << 6) << 16 | 1u << 24, 2}));
    // SETUP_VSEMPH: the first two bytes are the lane select (0) and the lane set
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterSetupVsEmph, 0, 0x12) && r.card.calls[0].words[0] == (0x1200u | (2u | 4u | 8u | 2u << 4 | 1u << 6) << 16 | 11u << 24));
    // DVI on DIG 4 (link A, front end 0): dual link halves the pixel clock (74250), the PLL is the CRTC's (PPLL2 = 1), eight lanes
    o = output(AmdModeSet::kModeDvi);
    o.pll = AmdModeSet::kPpll2;
    o.dig_encoder = 4;
    o.link_b = false;
    o.dual_link = true;
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({7425u | (1u | 2u | 1u << 4 | 2u << 6) << 16 | 1u << 24, 8}));
    // the LVDS panel: not coherent, no dual link bit
    o = output(AmdModeSet::kModeLvds);
    o.dfp = false;
    o.dual_link = true;
    o.dig_encoder = 0;
    o.link_b = false;
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable) && ((r.card.calls[0].words[0] >> 16) & 0xFF) == 0 && r.card.calls[0].words[1] == 8);
    o.dig_encoder = 6;
    CHECK(!s.transmitter(o, m, 1));
  }
  {
    // V1.6: PHY id, action, DP lane set (SETUP_VSEMPH) or mode, lanes; symbol clock; HPD select, DIG select, connector id
    Rig r(76, 6);
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({3u | 1u << 8 | 0u << 16 | 2u << 24, 27000u, 3u | (1u << 3) << 8 | 19u << 16, 0}));
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterSetupVsEmph, 0, 0x09) && r.card.calls[1].words[0] == (3u | 11u << 8 | 9u << 16 | 2u << 24));
    o = output(AmdModeSet::kModeHdmi);
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable) && r.card.calls[2].words[0] == (3u | 1u << 8 | 3u << 16 | 4u << 24) && r.card.calls[2].words[1] == 14850u);
  }
  // other revisions, another format revision, no table
  const struct { uint8_t crev, frev; } refused[] = {{3, 1}, {7, 1}, {5, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(76, rf.crev, rf.frev);
    CHECK(!AmdModeSet(x.card, *x.bios).transmitter(output(0), m, 1) && x.card.calls.empty());
  }
}

static void test_pick_pll() {
  // DisplayPort: the DP DTO whatever the encoder; the others the combo PHY PLL of their UNIPHY and link
  CHECK(AmdModeSet::pick_pll(AmdModeSet::kModeDp, 0x1E, false) == 11 && AmdModeSet::pick_pll(AmdModeSet::kModeDpMst, 0x25, true) == 11 && AmdModeSet::pick_pll(AmdModeSet::kModeDp, 0, false) == 11);
  const struct { uint8_t encoder; bool link_b; uint8_t pll; } rows[] = {{0x1E, false, 20}, {0x1E, true, 21}, {0x20, false, 22}, {0x20, true, 23}, {0x21, false, 24}, {0x21, true, 25}};
  for (const auto& r : rows)
    for (uint8_t mode : {AmdModeSet::kModeHdmi, AmdModeSet::kModeDvi, AmdModeSet::kModeLvds}) CHECK(AmdModeSet::pick_pll(mode, r.encoder, r.link_b) == r.pll);
  CHECK(AmdModeSet::pick_pll(AmdModeSet::kModeHdmi, 0x25, false) == 0xFF && AmdModeSet::pick_pll(AmdModeSet::kModeHdmi, 0, false) == 0xFF);
  // the pixel clock table takes no CRTC (0xFF) for a combo PHY PLL being switched off, but no other CRTC that does not exist
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  ModeSetOutput off;
  off.crtc = 0xFF;
  off.pll = 22;
  CHECK(s.set_pixel_clock(off, DisplayMode()) && r.card.calls[0].words == std::vector<uint32_t>({0u, 22u, 0xFFu, 0u}));
  off.crtc = 7;
  CHECK(!s.set_pixel_clock(off, DisplayMode()));
}

int main() {
  test_pixel_clock();
  test_crtc_timing();
  test_crtc_controls();
  test_encoder();
  test_transmitter();
  test_pick_pll();
  printf(g_failed ? "AmdModeSetTest: %d FAILED\n" : "AmdModeSetTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
