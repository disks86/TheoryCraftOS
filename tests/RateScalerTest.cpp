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

// The rate scaler against simulated channels: each rate has a chance that a transmission gets through; a frame is tried along the scaler's table (each entry in turn, up to 15 tries) and
// the result is fed back. It must settle on a rate whose goodput is near the best one, drop when the channel gets bad and climb when it gets good, ignore frames of another rate, and leave
// the rate alone when there is nothing to learn from.
#include <stdint.h>
#include <stdio.h>
#include "Hardware/WirelessInterface/RateScaler.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Channel {
  uint32_t percent[WifiRateScaler::kRateCount] = {};      // chance that one transmission gets through
  uint32_t seed = 12345;
  uint64_t now_ms = 0;
  uint32_t random() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
  // one frame; returns the goodput contribution (kbps weighted) and feeds the scaler
  void send(WifiRateScaler& s) {
    uint8_t table[WifiRateScaler::kTableSize];
    s.fill_table(table);
    uint32_t attempts = 0;
    bool ok = false;
    for (uint32_t i = 0; i < 15 && !ok; ++i) {
      ++attempts;
      ok = (random() % 100) < percent[table[i]];
    }
    now_ms += 2;
    s.on_result(ok, attempts, table[0], now_ms);
  }
  uint64_t best_goodput() const {
    uint64_t best = 0;
    for (uint32_t i = 0; i < WifiRateScaler::kRateCount; ++i) best = best < uint64_t{percent[i]} * WifiRateScaler::kRatesKbps[i] ? uint64_t{percent[i]} * WifiRateScaler::kRatesKbps[i] : best;
    return best;
  }
  uint64_t goodput_of(uint32_t rate) const { return uint64_t{percent[rate]} * WifiRateScaler::kRatesKbps[rate]; }
};

static void run(WifiRateScaler& s, Channel& c, uint32_t frames) { for (uint32_t i = 0; i < frames; ++i) c.send(s); }

static void test_start_and_table() {
  WifiRateScaler s;
  s.start(90);
  CHECK(s.current() == 6 && s.take_changed() && !s.take_changed());
  s.start(65); CHECK(s.current() == 5);
  s.start(50); CHECK(s.current() == 4);
  s.start(35); CHECK(s.current() == 2);
  s.start(5); CHECK(s.current() == 0 && s.current_kbps() == 6000);
  s.start(65);
  uint8_t t[WifiRateScaler::kTableSize];
  s.fill_table(t);
  const uint8_t expected[16] = {5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0};
  bool same = true;
  for (int i = 0; i < 16; ++i) same = same && t[i] == expected[i];
  CHECK(same);
  s.start(0);
  s.fill_table(t);
  CHECK(t[0] == 0 && t[15] == 0);
  CHECK(WifiRateScaler::index_of_plcp(0x0D) == 0 && WifiRateScaler::index_of_plcp(0x03) == 7 && WifiRateScaler::index_of_plcp(0x09) == 4 && WifiRateScaler::index_of_plcp(0x42) == WifiRateScaler::kRateCount);
  // the PLCP codes are the standard ones, in the order of the rates
  CHECK(WifiRateScaler::kPlcp[1] == 0x0F && WifiRateScaler::kPlcp[2] == 0x05 && WifiRateScaler::kPlcp[3] == 0x07 && WifiRateScaler::kPlcp[5] == 0x0B && WifiRateScaler::kPlcp[6] == 0x01);
}

static void test_settles() {
  {   // a clean channel: the top rate
    Channel c;
    for (uint32_t& p : c.percent) p = 98;
    WifiRateScaler s;
    s.start(40);
    run(s, c, 600);
    CHECK(s.current() == 7);
  }
  {   // the top rate is bad: it is tried, found bad, and left
    Channel c;
    for (uint32_t& p : c.percent) p = 98;
    c.percent[7] = 50;
    WifiRateScaler s;
    s.start(40);
    run(s, c, 800);
    CHECK(s.current() == 6 || (s.current() == 7 && s.probing()));
    uint32_t at_48 = 0;
    for (int i = 0; i < 400; ++i) { c.send(s); at_48 += s.current() == 6; }
    CHECK(at_48 > 300);                                                       // (mostly at 48, with a probe of 54 now and then)
  }
  {   // a poor channel: the best goodput is around 18 Mbit/s
    Channel c;
    const uint32_t p[8] = {95, 90, 80, 60, 40, 10, 0, 0};
    for (int i = 0; i < 8; ++i) c.percent[i] = p[i];
    WifiRateScaler s;
    s.start(80);
    run(s, c, 1200);
    CHECK(s.current() >= 2 && s.current() <= 4);
    uint64_t sum = 0;
    for (int i = 0; i < 400; ++i) { c.send(s); sum += c.goodput_of(s.current()); }
    CHECK(sum / 400 * 100 / c.best_goodput() >= 75);                           // within 25 % of the best rate's goodput
  }
  {   // a bad channel: down to the lowest
    Channel c;
    for (uint32_t i = 0; i < 8; ++i) c.percent[i] = i == 0 ? 90 : 5;
    WifiRateScaler s;
    s.start(90);
    run(s, c, 800);
    CHECK(s.current() == 0);
  }
}

static void test_changing_channel() {
  Channel c;
  for (uint32_t& p : c.percent) p = 97;
  WifiRateScaler s;
  s.start(70);
  run(s, c, 500);
  CHECK(s.current() == 7);
  for (uint32_t i = 3; i < 8; ++i) c.percent[i] = 3;                         // the high rates stop working
  run(s, c, 300);
  CHECK(s.current() <= 3);
  for (uint32_t& p : c.percent) p = 97;                                      // and come back
  run(s, c, 1500);
  CHECK(s.current() == 7);
}

static void test_inputs() {
  WifiRateScaler s;
  s.start(65);
  s.take_changed();
  // frames that started at another rate say nothing about the current one
  for (int i = 0; i < 64; ++i) s.on_result(false, 15, 0, 0);
  CHECK(s.current() == 5 && !s.take_changed());
  // unknown rate (kRateCount) counts for the current one
  for (int i = 0; i < 16; ++i) s.on_result(false, 15, WifiRateScaler::kRateCount, 0);
  CHECK(s.current() == 4 && s.take_changed());
  // attempts of 0 or too many are made sane
  s.on_result(true, 0, 4, 0);
  s.on_result(false, 1000, 4, 0);
  // a window that fills slowly is closed by the clock
  s.start(65);
  s.take_changed();
  for (int i = 0; i < 5; ++i) s.on_result(false, 15, 5, 100);
  s.tick(500);
  CHECK(s.current() == 5);                                                  // not long enough yet
  s.tick(1200);
  CHECK(s.current() == 4 && s.take_changed());
  s.tick(5000);                                                             // nothing in the window: nothing happens
  CHECK(s.current() == 4);
  // the lowest rate cannot go lower; the highest cannot go higher
  WifiRateScaler low;
  low.start(0);
  for (int i = 0; i < 64; ++i) low.on_result(false, 15, 0, 0);
  CHECK(low.current() == 0);
  WifiRateScaler high;
  high.start(100);
  high.take_changed();
  for (int i = 0; i < 160; ++i) high.on_result(true, 1, high.current(), 0);
  CHECK(high.current() == 7);
  // the success chance of a rate that was never used is unknown
  CHECK(high.success_chance(1) == 0 && high.success_chance(99) == 0 && high.success_chance(7) > 900);
}

int main() {
  test_start_and_table();
  test_settles();
  test_changing_channel();
  test_inputs();
  printf(g_failed ? "RateScalerTest: %d FAILED\n" : "RateScalerTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
