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


// AmdDpSource against an AtomBIOS image whose DIG1EncoderControl, DIG2EncoderControl and UNIPHYTransmitterControl tables hand their parameter words to the simulated card (a register
// each), with the table revisions the driver handles: the exact words the tables are given for each action, link rate, lane count, DIG and link, and the refusals (revisions that are
// not handled, rates a revision does not have, DIGs that do not exist, a missing table).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDpSource.hpp"
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

// Each table copies its parameter dwords to its own registers; the last one written ends a call.
struct Card final : IAtomHost {
  struct Call {
    uint32_t table;
    uint32_t words[3];
  };
  std::vector<Call> calls;
  uint32_t regs[0x40] = {};
  uint64_t delayed = 0;
  uint32_t read_register(uint32_t) override { return 0; }
  void write_register(uint32_t reg, uint32_t value) override {
    if (reg < 0x7300 || reg >= 0x7340) return;
    regs[reg - 0x7300] = value;
    const uint32_t table = (reg - 0x7300) / 0x10;      // 0: encoder 1, 1: encoder 2, 2: transmitter
    const uint32_t last = table == 2 ? 2 : 1;
    if ((reg - 0x7300) % 0x10 == last) calls.push_back({table, {regs[table * 0x10], regs[table * 0x10 + 1], regs[table * 0x10 + 2]}});
  }
  uint32_t read_mc(uint32_t) override { return 0; }
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t n) override { delayed += n; }
  void delay_ms(uint32_t n) override { delayed += uint64_t{n} * 1000; }
  uint64_t now_us() override { return ++clock_; }
  uint64_t clock_ = 0;
};

struct Rig {
  TestAllocator alloc;
  Card card;
  std::vector<uint8_t> image;
  std::unique_ptr<AtomBios> bios;
  // revisions of the three tables (0: the table is absent); transmitter dwords: 2 for V4, 3 for V1.5
  Rig(uint8_t enc_crev = 4, uint8_t tx_crev = 5, uint8_t enc_frev = 1, uint8_t tx_frev = 1, bool enc2 = true) {
    Image img;
    for (uint32_t t = 0; t < 2; ++t) {
      if (!enc_crev || (t == 1 && !enc2)) continue;
      Code c;
      c.move(REG, 0x7300 + t * 0x10, PS, 0);
      c.move(REG, 0x7301 + t * 0x10, PS, 1);
      c.eot();
      img.tables.push_back({74 + t, 8, 8, c, enc_frev, enc_crev});
    }
    if (tx_crev) {
      Code c;
      c.move(REG, 0x7320, PS, 0);
      c.move(REG, 0x7321, PS, 1);
      c.move(REG, 0x7322, PS, 2);
      c.eot();
      img.tables.push_back({76, 8, 12, c, tx_frev, tx_crev});
    }
    image = img.build();
    bios.reset(new AtomBios(card, ConstByteView(image.data(), image.size()), alloc));
  }
};

static DpSourceConfig config() {
  DpSourceConfig c;
  c.dig_encoder = 0;
  c.link_b = false;
  c.lanes = 4;
  c.rate_khz = 270000;
  c.pixel_clock_khz = 148500;
  c.connector_object_id = 19;
  c.hpd = 2;
  c.bits_per_color = 2;
  c.reference_clock_source = 0;
  return c;
}

static void test_encoder_v4() {
  Rig r;
  CHECK(r.bios->valid());
  AmdDpSource src(r.card, *r.bios, config());
  // START on DIG 0: the table DIG1EncoderControl (74): pixel clock 14850 (10 kHz), config 1 (2.7 GHz) | DIG 0 << 4, action 8; DP mode 0, 4 lanes, 8 bits per colour, HPD id 3 (pin 2)
  CHECK(src.start_training());
  CHECK(r.card.calls.size() == 1 && r.card.calls[0].table == 0);
  CHECK(r.card.calls[0].words[0] == (14850u | 0x01u << 16 | 0x08u << 24) && r.card.calls[0].words[1] == (0u | 4u << 8 | 2u << 16 | 3u << 24));
  // the patterns and the end
  r.card.calls.clear();
  CHECK(src.set_pattern(1) && src.set_pattern(2) && src.set_pattern(3) && src.complete_training());
  CHECK(r.card.calls.size() == 4);
  const uint8_t actions[4] = {0x09, 0x0A, 0x13, 0x0B};
  for (size_t i = 0; i < 4 && i < r.card.calls.size(); ++i) CHECK((r.card.calls[i].words[0] >> 24) == actions[i] && r.card.calls[i].table == 0);
  r.card.calls.clear();
  CHECK(!src.set_pattern(0) && !src.set_pattern(4) && !src.set_pattern(0xFF) && r.card.calls.empty());
  // the rates: 1.62 (0), 5.4 (2), 3.24 (3); not 8.1 or others
  DpSourceConfig c = config();
  const struct { uint32_t khz; uint32_t bits; } rates[] = {{162000, 0}, {270000, 1}, {540000, 2}, {324000, 3}};
  for (const auto& rate : rates) {
    c.rate_khz = rate.khz;
    AmdDpSource s(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(s.start_training() && ((r.card.calls[0].words[0] >> 16) & 0xFF) == rate.bits);
  }
  for (uint32_t khz : {0u, 81000u, 100000u, 810000u}) {
    c.rate_khz = khz;
    AmdDpSource s(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(!s.start_training() && r.card.calls.empty());
  }
  // DIGs 1 to 6: the second table (75), the DIG in bits 6:4 of the config
  for (uint32_t dig = 1; dig <= 6; ++dig) {
    c = config();
    c.dig_encoder = static_cast<uint8_t>(dig);
    AmdDpSource s(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(s.start_training() && r.card.calls.size() == 1 && r.card.calls[0].table == 1 && ((r.card.calls[0].words[0] >> 16) & 0xFF) == (1u | dig << 4));
  }
  c.dig_encoder = 7;
  AmdDpSource bad(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(!bad.start_training() && r.card.calls.empty());
  // lanes, bits per colour, no hot plug pin, pixel clock (in 10 kHz)
  c = config();
  c.lanes = 1;
  c.bits_per_color = 3;
  c.hpd = 0xFF;
  c.pixel_clock_khz = 25175;
  AmdDpSource s(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(s.start_training() && r.card.calls[0].words[0] == (2517u | 0x01u << 16 | 0x08u << 24) && r.card.calls[0].words[1] == (1u << 8 | 3u << 16));
  c.hpd = 5;
  AmdDpSource s5(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(s5.start_training() && (r.card.calls[0].words[1] >> 24) == 6);
  // delays go to the host
  src.delay_us(400);
  CHECK(r.card.delayed == 400);
}

static void test_encoder_v3_and_refused() {
  Rig v3(3);
  DpSourceConfig c = config();
  AmdDpSource s(v3.card, *v3.bios, c);
  // V3: the same words but no hot plug id
  CHECK(s.start_training() && v3.card.calls[0].words[0] == (14850u | 0x01u << 16 | 0x08u << 24) && v3.card.calls[0].words[1] == (4u << 8 | 2u << 16));
  // and no 5.4 or 3.24 GHz
  v3.card.calls.clear();
  c.rate_khz = 540000;
  AmdDpSource s54(v3.card, *v3.bios, c);
  CHECK(!s54.start_training());
  c.rate_khz = 324000;
  AmdDpSource s324(v3.card, *v3.bios, c);
  CHECK(!s324.start_training());
  c.rate_khz = 162000;
  AmdDpSource s162(v3.card, *v3.bios, c);
  CHECK(s162.start_training() && v3.card.calls.size() == 1);
  // other revisions of the encoder control table are refused: 1, 2, 5; another format revision; no table
  const struct { uint8_t crev, frev; } refused[] = {{1, 1}, {2, 1}, {5, 1}, {6, 1}, {4, 2}, {4, 0}};
  for (const auto& rf : refused) {
    Rig r(rf.crev, 5, rf.frev);
    AmdDpSource x(r.card, *r.bios, config());
    CHECK(!x.start_training() && !x.set_pattern(1) && !x.complete_training() && r.card.calls.empty());
  }
  Rig none(0);
  AmdDpSource x(none.card, *none.bios, config());
  CHECK(!x.start_training());
  Rig no_second(4, 5, 1, 1, false);
  c = config();
  AmdDpSource dig0(no_second.card, *no_second.bios, c);
  CHECK(dig0.start_training());
  c.dig_encoder = 2;
  AmdDpSource dig2(no_second.card, *no_second.bios, c);
  CHECK(!dig2.start_training());
}

static void test_transmitter_v5() {
  Rig r;
  DpSourceConfig c = config();
  c.dig_encoder = 2;
  c.link_b = false;
  c.lanes = 2;
  c.rate_khz = 270000;
  c.connector_object_id = 12;
  c.hpd = 1;
  c.reference_clock_source = 1;
  AmdDpSource s(r.card, *r.bios, c);
  // V1.5 words: symbol clock 27000 | PHY id (= DIG 2: UNIPHYC) << 16 | action 11 << 24; lanes 2 | connector 12 << 8 | DP mode 0 | config << 24 (HPD2 sel 0x20 | P2PLL 0x04); DIG front end 1 << 2 | the lane set
  CHECK(s.set_drive(0x09));
  CHECK(r.card.calls.size() == 1 && r.card.calls[0].table == 2);
  CHECK(r.card.calls[0].words[0] == (27000u | 2u << 16 | 11u << 24) && r.card.calls[0].words[1] == (2u | 12u << 8 | 0u << 16 | 0x24u << 24) && r.card.calls[0].words[2] == (0x04u | 0x09u << 8));
  // the lane set is passed as it is, flags and all
  r.card.calls.clear();
  CHECK(s.set_drive(0x3F) && r.card.calls[0].words[2] == (0x04u | 0x3Fu << 8));
  // each DIG is its own UNIPHY and front end; the rate is the symbol clock
  for (uint32_t dig = 0; dig <= 6; ++dig) {
    c = config();
    c.dig_encoder = static_cast<uint8_t>(dig);
    c.rate_khz = 540000;
    c.hpd = 0xFF;
    AmdDpSource d(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(d.set_drive(0) && r.card.calls[0].words[0] == (54000u | dig << 16 | 11u << 24) && (r.card.calls[0].words[1] >> 24) == 0 && r.card.calls[0].words[2] == (1u << dig));
  }
  c.dig_encoder = 7;
  AmdDpSource bad(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(!bad.set_drive(0) && r.card.calls.empty());
}

static void test_transmitter_v4() {
  Rig r(4, 4);
  DpSourceConfig c = config();
  c.dig_encoder = 3;           // UNIPHY CD (transmitter 1), DIGB side
  c.link_b = true;
  c.lanes = 4;
  c.reference_clock_source = 2;        // DCPLL
  AmdDpSource s(r.card, *r.bios, c);
  // V4: lane select 0 (all lanes) | lane set << 8 | config << 16 | action 11 << 24; the lane count; config = transmitter 1 << 6 | reference clock 2 << 4 | front end 1 << 3 | link B 1 << 2
  CHECK(s.set_drive(0x12));
  CHECK(r.card.calls.size() == 1 && r.card.calls[0].words[0] == (0u | 0x12u << 8 | 0x6Cu << 16 | 11u << 24) && r.card.calls[0].words[1] == 4);
  // transmitter by DIG: A/B 0, C/D 1, E/F 2; the front end is the DIG's parity; not DIG 6
  for (uint32_t dig = 0; dig <= 5; ++dig) {
    c = config();
    c.dig_encoder = static_cast<uint8_t>(dig);
    c.link_b = (dig & 1) != 0;
    AmdDpSource d(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(d.set_drive(0) && ((r.card.calls[0].words[0] >> 16) & 0xFF) == ((dig / 2) << 6 | (dig & 1) << 3 | (dig & 1) << 2));
  }
  c.dig_encoder = 6;
  AmdDpSource g(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(!g.set_drive(0) && r.card.calls.empty());
  // other revisions are refused: 1, 2, 3, 6; another format revision; no table
  const struct { uint8_t crev, frev; } refused[] = {{1, 1}, {2, 1}, {3, 1}, {6, 1}, {5, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(4, rf.crev, 1, rf.frev);
    AmdDpSource y(x.card, *x.bios, config());
    CHECK(!y.set_drive(0) && x.card.calls.empty());
  }
}

int main() {
  test_encoder_v4();
  test_encoder_v3_and_refused();
  test_transmitter_v5();
  test_transmitter_v4();
  printf(g_failed ? "AmdDpSourceTest: %d FAILED\n" : "AmdDpSourceTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
