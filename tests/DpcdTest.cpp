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

// Dpcd against hand-built DPCD images written from the DisplayPort specification's register map (addresses and bits as literal numbers, not the code's constants): the receiver
// capabilities of an HBR2 4-lane laptop panel, of an HBR 2-lane monitor behind a dongle, and of an eDP 1.4 panel with its own rate table; the link status (sink count with its high bit,
// service interrupts, per-lane training status, lane alignment); sinks that are no DPCD (zeros, ones, no lanes); short fields; reads that fail partway; and every bit flip and
// truncation of the images under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
// A sink: the bytes of its DPCD, and which reads fail
struct Sink final : IDpcdAux {
  std::vector<uint8_t> dpcd = std::vector<uint8_t>(0x800, 0);
  uint32_t reads = 0;
  std::vector<uint32_t> failing;     // addresses whose reads fail
  bool dpcd_read(uint32_t address, ByteView out) override {
    ++reads;
    for (uint32_t f : failing)
      if (f == address) return false;
    if (address + out.size() > dpcd.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = dpcd[address + i];
    return true;
  }
};

void set(Sink& s, uint32_t at, std::initializer_list<uint8_t> bytes) {
  for (uint8_t b : bytes) s.dpcd[at++] = b;
}

// A DisplayPort 1.2 monitor: HBR2 (0x14), 4 lanes with TPS3 and enhanced framing, 0.5 % downspread, 8b/10b, one receiver port
void monitor(Sink& s) {
  set(s, 0x000, {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00});
}
}  // namespace

void test_capabilities() {
  Sink s;
  monitor(s);
  Dpcd::Capabilities c;
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.valid);
  CHECK(c.revision == 0x12 && c.max_link_rate_code == 0x14 && c.max_lanes == 4 && c.tps3 && c.enhanced_framing && c.max_downspread && !c.no_aux_handshake);
  CHECK(c.receiver_ports == 1 && !c.downstream_port_present && c.coding_8b10b && c.downstream_port_count == 0 && !c.downstream_oui && c.edp_revision == Dpcd::kNoEdp);
  CHECK(c.max_link_rate_kbps() == 5400000 && c.edp_rate_count == 0 && s.reads == 1);     // (a DisplayPort sink: one read)
  // a dongle: HBR, two lanes without TPS3 or enhanced framing, no downspread, a DVI/HDMI downstream port with format conversion, 2 ports, a count of 1 with OUI support, two receiver ports
  Sink d;
  set(d, 0x000, {0x11, 0x0A, 0x02, 0x40, 0x01, 0x05 | 0x08 | 0x04, 0x01, 0x81});
  CHECK(Dpcd::read_capabilities(d, false, &c) && c.max_link_rate_kbps() == 2700000 && c.max_lanes == 2 && !c.tps3 && !c.enhanced_framing && !c.max_downspread && c.no_aux_handshake);
  CHECK(c.receiver_ports == 2 && c.downstream_port_present && c.downstream_type == 2 && c.format_conversion && c.downstream_port_count == 1 && c.downstream_oui);
  // the other types
  set(d, 0x005, {0x01 | (1 << 1)});
  CHECK(Dpcd::read_capabilities(d, false, &c) && c.downstream_type == 1 && !c.format_conversion);
  set(d, 0x005, {0x01 | (3 << 1)});
  CHECK(Dpcd::read_capabilities(d, false, &c) && c.downstream_type == 3);
  // rates
  const struct { uint8_t code; uint32_t kbps; } rates[] = {{0x06, 1620000}, {0x0A, 2700000}, {0x14, 5400000}, {0x1E, 8100000}};
  for (const auto& r : rates) {
    monitor(s);
    s.dpcd[1] = r.code;
    CHECK(Dpcd::read_capabilities(s, false, &c) && c.max_link_rate_kbps() == r.kbps);
  }
  // lane counts: 1, 2, 4 are fine; 0 and 3 are not a DPCD; the flags in bits 7:5 are not part of the count
  monitor(s);
  for (uint8_t lanes = 0; lanes < 32; ++lanes) {
    s.dpcd[2] = lanes;
    const bool ok = Dpcd::read_capabilities(s, false, &c);
    CHECK(ok == (lanes == 1 || lanes == 2 || lanes == 4));
    CHECK(!ok == !c.valid);
  }
  s.dpcd[2] = 0xE4;
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.max_lanes == 4 && c.tps3 && c.enhanced_framing);
  s.dpcd[2] = 0x44;
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.tps3 && !c.enhanced_framing);
  s.dpcd[2] = 0x84;
  CHECK(Dpcd::read_capabilities(s, false, &c) && !c.tps3 && c.enhanced_framing);
  // the single bits of the other fields
  monitor(s);
  s.dpcd[3] = 0x02;
  s.dpcd[4] = 0x02;
  s.dpcd[5] = 0x01 | 0x04;
  s.dpcd[7] = 0x10;
  CHECK(Dpcd::read_capabilities(s, false, &c) && !c.max_downspread && !c.no_aux_handshake && c.receiver_ports == 1 && c.downstream_port_present && c.downstream_type == 2 && !c.format_conversion);
  CHECK(c.downstream_port_count == 0 && !c.downstream_oui);
  s.dpcd[3] = 0x01;
  s.dpcd[4] = 0x01;
  s.dpcd[5] = 0x01 | 0x08;
  s.dpcd[7] = 0x0F;
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.max_downspread && c.receiver_ports == 2 && c.downstream_type == 0 && c.format_conversion && c.downstream_port_count == 15);
  s.dpcd[2] = 0x24;       // (bit 5 is reserved: not a flag, not a lane)
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.max_lanes == 4 && !c.tps3 && !c.enhanced_framing);
}

void test_not_a_dpcd() {
  Sink s;
  Dpcd::Capabilities c;
  CHECK(!Dpcd::read_capabilities(s, false, &c) && !c.valid);                  // zeros
  for (size_t i = 0; i < 0x10; ++i) s.dpcd[i] = 0xFF;
  CHECK(Dpcd::read_capabilities(s, false, &c) == false && !c.valid);          // ones (0x1F lanes, not 1, 2 or 4)
  monitor(s);
  s.dpcd[0] = 0x0F;                                                           // a revision below 1.0
  CHECK(!Dpcd::read_capabilities(s, false, &c));
  s.dpcd[0] = 0x10;
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.revision == 0x10);
  s.dpcd[0] = 0x14;
  s.dpcd[1] = 0;                                                              // no rate
  CHECK(!Dpcd::read_capabilities(s, false, &c));
  monitor(s);
  s.failing.push_back(0);
  CHECK(!Dpcd::read_capabilities(s, false, &c) && !c.valid);                  // nothing answers
  // short fields
  uint8_t two[2] = {0x12, 0x14};
  CHECK(!Dpcd::parse_capabilities(ConstByteView(two, 2), Dpcd::kNoEdp, ConstByteView(), &c));
  uint8_t full[16] = {0x12, 0x14, 0x04};
  CHECK(!Dpcd::parse_capabilities(ConstByteView(full, 15), Dpcd::kNoEdp, ConstByteView(), &c));
  CHECK(Dpcd::parse_capabilities(ConstByteView(full, 16), Dpcd::kNoEdp, ConstByteView(), &c));
}

void test_edp() {
  Sink s;
  monitor(s);
  set(s, 0x000, {0x14, 0x0A, 0x82});             // eDP 1.4: HBR, 2 lanes with enhanced framing
  // eDP 1.4 panel: rates 1.62, 2.16, 2.43, 2.7, 3.24, 4.32 Gbit/s in 200 kHz units: 8100, 10800, 12150, 13500, 16200, 21600
  set(s, 0x700, {0x03});
  const uint16_t table[] = {8100, 10800, 12150, 13500, 16200, 21600, 0, 0x7777};
  for (size_t i = 0; i < 8; ++i) { s.dpcd[0x10 + i * 2] = static_cast<uint8_t>(table[i]); s.dpcd[0x11 + i * 2] = static_cast<uint8_t>(table[i] >> 8); }
  Dpcd::Capabilities c;
  CHECK(Dpcd::read_capabilities(s, true, &c) && c.edp_revision == 3 && c.edp_rate_count == 6 && s.reads == 3);
  const uint32_t want[] = {1620000, 2160000, 2430000, 2700000, 3240000, 4320000};
  for (uint32_t i = 0; i < 6; ++i) CHECK(c.edp_rates_khz[i] == want[i]);
  CHECK(c.edp_rates_khz[6] == 0 && c.edp_rates_khz[7] == 0);
  // eight rates fill the table, with no terminator
  for (size_t i = 0; i < 8; ++i) { s.dpcd[0x10 + i * 2] = static_cast<uint8_t>(8100 + i); s.dpcd[0x11 + i * 2] = static_cast<uint8_t>((8100 + i) >> 8); }
  CHECK(Dpcd::read_capabilities(s, true, &c) && c.edp_rate_count == 8 && c.edp_rates_khz[7] == 8107u * 200);
  // an older panel (eDP 1.3, revision 2) has no table, even if its bytes are not zero; a DisplayPort connector never reads it
  set(s, 0x700, {0x02});
  s.reads = 0;
  CHECK(Dpcd::read_capabilities(s, true, &c) && c.edp_revision == 2 && c.edp_rate_count == 0 && s.reads == 2);
  set(s, 0x700, {0x03});
  s.reads = 0;
  CHECK(Dpcd::read_capabilities(s, false, &c) && c.edp_revision == Dpcd::kNoEdp && c.edp_rate_count == 0 && s.reads == 1);
  // the revision does not come: a panel without the eDP registers; the table does not come: no rates
  s.failing.push_back(0x700);
  CHECK(Dpcd::read_capabilities(s, true, &c) && c.edp_revision == Dpcd::kNoEdp && c.edp_rate_count == 0);
  s.failing.clear();
  s.failing.push_back(0x10);
  CHECK(Dpcd::read_capabilities(s, true, &c) && c.edp_revision == 3 && c.edp_rate_count == 0);
  // the table is read from 1.4 on only
  uint8_t rcv2[16] = {0x14, 0x0A, 0x02};
  uint8_t any_rates[4] = {0x10, 0x00, 0x20, 0x00};
  CHECK(Dpcd::parse_capabilities(ConstByteView(rcv2, 16), 2, ConstByteView(any_rates, 4), &c) && c.edp_revision == 2 && c.edp_rate_count == 0);
  CHECK(Dpcd::parse_capabilities(ConstByteView(rcv2, 16), Dpcd::kNoEdp, ConstByteView(any_rates, 4), &c) && c.edp_revision == Dpcd::kNoEdp && c.edp_rate_count == 0);     // (a DisplayPort sink)
  CHECK(Dpcd::parse_capabilities(ConstByteView(rcv2, 16), 4, ConstByteView(any_rates, 4), &c) && c.edp_rate_count == 2);
  // a table with a short view: only the whole entries are used
  uint8_t rcv[16] = {0x14, 0x0A, 0x02};
  uint8_t rates[5] = {0x10, 0x00, 0x20, 0x00, 0x30};
  CHECK(Dpcd::parse_capabilities(ConstByteView(rcv, 16), 3, ConstByteView(rates, 5), &c) && c.edp_rate_count == 2 && c.edp_rates_khz[1] == 32u * 200);
}

void test_link_status() {
  Sink s;
  // sink count 2 (bit 7 is the high bit of a 7-bit count), a service interrupt vector with an automated test request, lanes 0..3 trained, aligned, receive port 0 and 1 in sync
  set(s, 0x200, {0x02, 0x02, 0x77, 0x77, 0x81, 0x03, 0x12, 0x34});
  Dpcd::LinkStatus l;
  CHECK(Dpcd::read_link_status(s, &l));
  CHECK(l.sink_count() == 2 && l.automated_test_request() && !l.content_protection_irq() && !l.sink_specific_irq());
  CHECK(l.link_status_updated() && l.interlane_aligned() && !l.downstream_port_status_changed() && l.sink_status == 3 && l.adjust[0] == 0x12 && l.adjust[1] == 0x34);
  for (uint32_t lanes = 0; lanes <= 6; ++lanes) CHECK(l.clock_recovery_ok(lanes) && l.channel_eq_ok(lanes));
  // sink count: bits 5:0 and 7; the bit 6 is reserved
  set(s, 0x200, {0xC1});
  CHECK(Dpcd::read_link_status(s, &l) && l.sink_count() == 65);
  set(s, 0x200, {0x7F});
  CHECK(Dpcd::read_link_status(s, &l) && l.sink_count() == 0x3F);
  // the irq bits one by one
  set(s, 0x200, {0x00, 0x04});
  CHECK(Dpcd::read_link_status(s, &l) && l.content_protection_irq() && !l.automated_test_request() && !l.sink_specific_irq());
  set(s, 0x201, {0x40});
  CHECK(Dpcd::read_link_status(s, &l) && l.sink_specific_irq() && !l.content_protection_irq());
  set(s, 0x204, {0x40});
  CHECK(Dpcd::read_link_status(s, &l) && l.downstream_port_status_changed() && !l.link_status_updated() && !l.interlane_aligned());
  // per lane training status: lane n is in bits 4(n%2)+2:4(n%2) of byte n/2
  set(s, 0x200, {0x01, 0x00, 0x77, 0x77, 0x01});
  CHECK(Dpcd::read_link_status(s, &l) && l.clock_recovery_ok(4) && l.channel_eq_ok(4));
  for (uint32_t lane = 0; lane < 4; ++lane) {
    for (uint8_t bit = 0; bit < 3; ++bit) {
      set(s, 0x202, {0x77, 0x77});
      s.dpcd[0x202 + lane / 2] = static_cast<uint8_t>(0x77 & ~(1 << (lane % 2 * 4 + bit)));
      CHECK(Dpcd::read_link_status(s, &l));
      for (uint32_t n = 1; n <= 4; ++n) {
        const bool lane_in = lane < n;
        CHECK(l.channel_eq_ok(n) == !lane_in);
        CHECK(l.clock_recovery_ok(n) == !(lane_in && bit == 0));
      }
    }
  }
  // no interlane alignment: the channel is not equalized, the clock recovery is not affected
  set(s, 0x202, {0x77, 0x77, 0x00});
  CHECK(Dpcd::read_link_status(s, &l) && !l.channel_eq_ok(1) && l.clock_recovery_ok(4));
  // the reads
  s.failing.push_back(0x200);
  CHECK(!Dpcd::read_link_status(s, &l) && l.sink_count_raw == 0 && l.align == 0);
  uint8_t seven[7] = {};
  CHECK(!Dpcd::parse_link_status(ConstByteView(seven, 7), &l));
  uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(Dpcd::parse_link_status(ConstByteView(eight, 8), &l) && l.sink_count_raw == 1 && l.irq_vector == 2 && l.lane_status[0] == 3 && l.lane_status[1] == 4 && l.align == 5 && l.sink_status == 6 && l.adjust[0] == 7 && l.adjust[1] == 8);
}

void test_flips() {
  // every bit of the receiver field, the eDP revision and rates, and the status, flipped: never a crash, and valid exactly when the three essentials are
  Sink base;
  monitor(base);
  set(base, 0x700, {0x03});
  for (size_t i = 0; i < 8; ++i) { base.dpcd[0x10 + i * 2] = static_cast<uint8_t>(8100 + i * 100); base.dpcd[0x11 + i * 2] = static_cast<uint8_t>((8100 + i * 100) >> 8); }
  set(base, 0x200, {0x01, 0x02, 0x77, 0x77, 0x81, 0x03, 0x12, 0x34});
  for (uint32_t at = 0; at < 0x800; ++at) {
    if (at >= 0x30 && at < 0x200) continue;
    if (at >= 0x208 && at < 0x700) continue;
    for (int bit = 0; bit < 8; ++bit) {
      Sink s = base;
      s.dpcd[at] ^= static_cast<uint8_t>(1 << bit);
      Dpcd::Capabilities c;
      const bool ok = Dpcd::read_capabilities(s, true, &c);
      CHECK(ok == c.valid);
      CHECK(c.edp_rate_count <= Dpcd::kMaxEdpRates);
      Dpcd::LinkStatus l;
      CHECK(Dpcd::read_link_status(s, &l));
      (void)l.sink_count();
      (void)l.channel_eq_ok(4);
    }
  }
  // truncation: a field of any length below the whole one is refused, one longer is read
  for (uint32_t n = 0; n < 16; ++n) {
    Dpcd::Capabilities c;
    CHECK(!Dpcd::parse_capabilities(ConstByteView(base.dpcd.data(), n), Dpcd::kNoEdp, ConstByteView(), &c) && !c.valid);
  }
  for (uint32_t n = 0; n <= 20; ++n) {
    Dpcd::Capabilities c;
    CHECK(Dpcd::parse_capabilities(ConstByteView(base.dpcd.data(), 16), 3, ConstByteView(base.dpcd.data() + 0x10, n), &c) && c.edp_rate_count == (n / 2 < 8 ? n / 2 : 8));
    Dpcd::LinkStatus l;
    CHECK(Dpcd::parse_link_status(ConstByteView(base.dpcd.data() + 0x200, n), &l) == (n >= 8));
  }
}

int main() {
  test_capabilities();
  test_not_a_dpcd();
  test_edp();
  test_link_status();
  test_flips();
  printf(g_failed ? "DpcdTest: %d FAILED\n" : "DpcdTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
