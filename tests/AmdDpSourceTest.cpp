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


// AmdDpSource against an AtomBIOS image whose DIGx encoder control and UNIPHY transmitter control tables hand their parameter words to the simulated card (a register each), with the table
// revisions the driver handles: the exact words the tables are given for each training action, link rate, lane count, DIG and link, and the refusals (revisions that are not handled,
// DIGs that do not exist, a missing table).
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

// Each table copies its parameter dwords to its own registers (0x7300 for the encoder, 0x7320 for the transmitter); the last one written ends a call.
struct Card final : IAtomHost {
  struct Call {
    uint32_t table;       // 0 encoder, 2 transmitter
    uint32_t words[3];
  };
  std::vector<Call> calls;
  uint32_t regs[0x40] = {};
  uint32_t encoder_words = 2, transmitter_words = 3;
  uint64_t delayed = 0;
  uint32_t read_register(uint32_t) override { return 0; }
  void write_register(uint32_t reg, uint32_t value) override {
    if (reg < 0x7300 || reg >= 0x7340) return;
    regs[reg - 0x7300] = value;
    const uint32_t table = (reg - 0x7300) / 0x20 * 2;
    const uint32_t last = (table == 2 ? transmitter_words : encoder_words) - 1;
    if ((reg - 0x7300) % 0x20 == last) calls.push_back({table, {regs[table / 2 * 0x20], regs[table / 2 * 0x20 + 1], regs[table / 2 * 0x20 + 2]}});
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
  // revisions of the two tables (0: the table is absent)
  Rig(uint8_t enc_crev = 5, uint8_t tx_crev = 5, uint8_t enc_frev = 1, uint8_t tx_frev = 1) {
    Image img;
    if (enc_crev) {
      card.encoder_words = enc_crev <= 4 ? 2 : 3;
      Code c;
      for (uint32_t i = 0; i < card.encoder_words; ++i) c.move(REG, 0x7300 + i, PS, i);
      c.eot();
      img.tables.push_back({4, 12, 12, c, enc_frev, enc_crev});
    }
    if (tx_crev) {
      card.transmitter_words = tx_crev == 4 ? 2 : 3;
      Code c;
      for (uint32_t i = 0; i < card.transmitter_words; ++i) c.move(REG, 0x7320 + i, PS, i);
      c.eot();
      img.tables.push_back({76, 12, 12, c, tx_frev, tx_crev});
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
  c.bits_per_color = 8;
  c.pll = AmdModeSet::kPpll0;
  return c;
}

static void test_encoder_commands() {
  // V4 (8 bytes): pixel clock | config << 16 | action << 24; mode | lanes << 8 | bpc << 16 | HPD id << 24
  {
    Rig r(4);
    CHECK(r.bios->valid());
    AmdDpSource src(r.card, *r.bios, config());
    CHECK(src.start_training() && src.set_pattern(1) && src.set_pattern(2) && src.set_pattern(3) && src.complete_training());
    const uint8_t actions[5] = {0x08, 0x09, 0x0A, 0x13, 0x0B};
    CHECK(r.card.calls.size() == 5);
    for (size_t i = 0; i < r.card.calls.size() && i < 5; ++i)
      CHECK(r.card.calls[i].table == 0 && r.card.calls[i].words[0] == (14850u | 0x01u << 16 | uint32_t{actions[i]} << 24) && r.card.calls[i].words[1] == (0u | 4u << 8 | 2u << 16 | 3u << 24));
    CHECK(!src.set_pattern(0) && !src.set_pattern(4) && r.card.calls.size() == 5);
    // DIG 3 on link B, 5.4 GHz, 2 lanes, 10 bits, no HPD
    DpSourceConfig c = config();
    c.dig_encoder = 3;
    c.rate_khz = 540000;
    c.lanes = 2;
    c.bits_per_color = 10;
    c.hpd = 0xFF;
    AmdDpSource d(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(d.start_training() && r.card.calls[0].words[0] == (14850u | 0x32u << 16 | 0x08u << 24) && r.card.calls[0].words[1] == (0u | 2u << 8 | 3u << 16 | 0u << 24));
    // DIG 7 does not exist
    c.dig_encoder = 7;
    AmdDpSource bad(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(!bad.start_training() && r.card.calls.empty());
  }
  // V3 (and V2): no HPD id; only 2.7 GHz sets the rate bit
  for (uint8_t crev : {2, 3}) {
    Rig r(crev);
    DpSourceConfig c = config();
    c.rate_khz = 540000;
    AmdDpSource src(r.card, *r.bios, c);
    CHECK(src.start_training() && r.card.calls[0].words[0] == (14850u | 0x00u << 16 | 0x08u << 24) && (r.card.calls[0].words[1] >> 24) == 0);
    AmdDpSource d(r.card, *r.bios, config());
    CHECK(d.start_training() && (r.card.calls[1].words[0] >> 16) == (0x01u | 0x08u << 8));
  }
  // V5: the DIG and the action only
  {
    Rig r(5);
    AmdDpSource src(r.card, *r.bios, config());
    CHECK(src.start_training() && src.set_pattern(3) && src.complete_training());
    CHECK(r.card.calls[0].words[0] == (0u | 0x08u << 8) && r.card.calls[1].words[0] == (0u | 0x13u << 8) && r.card.calls[2].words[0] == (0u | 0x0Bu << 8));
    DpSourceConfig c = config();
    c.dig_encoder = 5;
    AmdDpSource d(r.card, *r.bios, c);
    CHECK(d.set_pattern(1) && r.card.calls[3].words[0] == (5u | 0x09u << 8));
  }
  // refused: revisions 1 and 6, another format revision, no table
  const struct { uint8_t crev, frev; } refused[] = {{1, 1}, {6, 1}, {4, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(rf.crev, 5, rf.frev);
    AmdDpSource y(x.card, *x.bios, config());
    CHECK(!y.start_training() && x.card.calls.empty());
  }
}

static void test_transmitter_v5() {
  Rig r;
  AmdDpSource src(r.card, *r.bios, config());
  // the drive of all lanes: symbol clock = rate / 10; PHY id = DIG; SETUP_VSEMPH (11); lanes, connector object, mode DP, config (HPD 3: 0x30 | PLL0 (2) << 2 | coherent 2); DIG select bit; lane set
  CHECK(src.set_drive(0x09));
  CHECK(r.card.calls[0].table == 2 && r.card.calls[0].words[0] == (27000u | 0u << 16 | 11u << 24) && r.card.calls[0].words[1] == (4u | 19u << 8 | 0u << 16 | (0x30u | 2u << 2 | 2u) << 24) &&
        r.card.calls[0].words[2] == (1u | 9u << 8));
  // transmitter by DIG and link: the PHY id is the DIG's number, the select bit its bit
  for (uint32_t dig = 0; dig <= 6; ++dig) {
    DpSourceConfig c = config();
    c.dig_encoder = static_cast<uint8_t>(dig);
    c.link_b = (dig & 1) != 0;
    c.lanes = 2;
    c.hpd = 0xFF;
    AmdDpSource d(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(d.set_drive(0) && ((r.card.calls[0].words[0] >> 16) & 0xFF) == dig && r.card.calls[0].words[2] == (1u << dig) && (r.card.calls[0].words[1] & 0xFF) == 2 && (r.card.calls[0].words[1] >> 24) == (2u << 2 | 2u));
  }
  DpSourceConfig c = config();
  c.dig_encoder = 7;
  AmdDpSource g(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(!g.set_drive(0) && r.card.calls.empty());
}

static void test_transmitter_v4() {
  Rig r(5, 4);
  CHECK(r.bios->valid());
  AmdDpSource src(r.card, *r.bios, config());
  // asMode (lane select 0, lane set), config (coherent 2 | display clock PLL 2 << 4 | front end 0 | transmitter 0), action 11, lanes
  CHECK(src.set_drive(0x12));
  CHECK(r.card.calls[0].table == 2 && r.card.calls[0].words[0] == (0x1200u | (2u | 2u << 4) << 16 | 11u << 24) && r.card.calls[0].words[1] == 4);
  // transmitter by DIG: A/B 0, C/D 1, E/F 2; the front end is the DIG's parity; not DIG 6
  for (uint32_t dig = 0; dig <= 5; ++dig) {
    DpSourceConfig c = config();
    c.dig_encoder = static_cast<uint8_t>(dig);
    c.link_b = (dig & 1) != 0;
    AmdDpSource d(r.card, *r.bios, c);
    r.card.calls.clear();
    CHECK(d.set_drive(0) && ((r.card.calls[0].words[0] >> 16) & 0xFF) == ((dig / 2) << 6 | 2u << 4 | (dig & 1) << 3 | (dig & 1) << 2 | 2u));
  }
  DpSourceConfig c = config();
  c.dig_encoder = 6;
  AmdDpSource g(r.card, *r.bios, c);
  r.card.calls.clear();
  CHECK(!g.set_drive(0) && r.card.calls.empty());
  // other revisions are refused: 1, 2, 3, 7; another format revision; no table
  const struct { uint8_t crev, frev; } refused[] = {{1, 1}, {2, 1}, {3, 1}, {7, 1}, {5, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(5, rf.crev, 1, rf.frev);
    AmdDpSource y(x.card, *x.bios, config());
    CHECK(!y.set_drive(0) && x.card.calls.empty());
  }
}

int main() {
  test_encoder_commands();
  test_transmitter_v5();
  test_transmitter_v4();
  printf(g_failed ? "AmdDpSourceTest: %d FAILED\n" : "AmdDpSourceTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
