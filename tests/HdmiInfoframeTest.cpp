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

// Hdmi against the HDMI specification's numbers: the audio clock regeneration values (the predefined rows checked against N / CTS = 128 * fs / clock, the calculated ones worked out by hand
// for 13.5, 65 and 25 MHz and the edge cases), the CEA video mode matching with the standard timings of the modes (and the half percent of tolerance of the refresh), and the AVI InfoFrame
// bytes with their checksum.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "Hardware/GraphicsProcessingUnit/AMD/HdmiInfoframe.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static HdmiMode mode(uint32_t clock, uint32_t h, uint32_t v, uint32_t ht, uint32_t vt, bool interlaced = false, bool dbl = false, uint8_t aspect = 0) {
  HdmiMode m;
  m.clock_khz = clock;
  m.hdisplay = h;
  m.vdisplay = v;
  m.htotal = ht;
  m.vtotal = vt;
  m.interlaced = interlaced;
  m.double_clock = dbl;
  m.picture_aspect = aspect;
  return m;
}

static void test_acr() {
  // the predefined rows: the same clock to the kHz, and N / CTS = 128 fs / clock within 0.02 % (the clocks of the 1.001 rows are the exact ones)
  const struct { uint32_t clock; double exact_khz; uint32_t n32, cts32, n44, cts44, n48, cts48; } rows[] = {
      {25175, 25200 / 1.001, 4096, 25175, 28224, 125875, 6144, 25175},     {25200, 25200, 4096, 25200, 6272, 28000, 6144, 25200},
      {27000, 27000, 4096, 27000, 6272, 30000, 6144, 27000},               {27027, 27027, 4096, 27027, 6272, 30030, 6144, 27027},
      {54000, 54000, 4096, 54000, 6272, 60000, 6144, 54000},               {54054, 54054, 4096, 54054, 6272, 60060, 6144, 54054},
      {74176, 74250 / 1.001, 11648, 210937, 17836, 234375, 11648, 140625}, {74250, 74250, 4096, 74250, 6272, 82500, 6144, 74250},
      {148352, 148500 / 1.001, 11648, 421875, 8918, 234375, 5824, 140625}, {148500, 148500, 4096, 148500, 6272, 165000, 6144, 148500}};
  for (const auto& r : rows) {
    const Hdmi::Acr a = Hdmi::acr(r.clock);
    CHECK(a.n_32khz == r.n32 && a.cts_32khz == r.cts32 && a.n_44_1khz == r.n44 && a.cts_44_1khz == r.cts44 && a.n_48khz == r.n48 && a.cts_48khz == r.cts48);
    const struct { double fs; uint32_t n, cts; } cols[] = {{32000, a.n_32khz, a.cts_32khz}, {44100, a.n_44_1khz, a.cts_44_1khz}, {48000, a.n_48khz, a.cts_48khz}};
    for (const auto& c : cols) {
      const double want = r.exact_khz * 1000 * c.n / (128 * c.fs);        // CTS = clock * N / (128 fs)
      const double err = (want - c.cts) / want;
      CHECK(err < 0.0002 && err > -0.0002);
    }
  }
  // calculated: 65 MHz (1024x768 at 60 Hz) gives the specification's 4096 / 65000, 7056 / 81250 and 6144 / 65000
  Hdmi::Acr a = Hdmi::acr(65000);
  CHECK(a.n_32khz == 4096 && a.cts_32khz == 65000 && a.n_44_1khz == 7056 && a.cts_44_1khz == 81250 && a.n_48khz == 6144 && a.cts_48khz == 65000);
  a = Hdmi::acr(13500);
  CHECK(a.n_32khz == 4096 && a.cts_32khz == 13500 && a.n_44_1khz == 6272 && a.cts_44_1khz == 15000 && a.n_48khz == 6144 && a.cts_48khz == 13500);
  // 25.2 MHz is in the table; 25.201 is not, and is calculated (the fraction is in lowest terms already, N is scaled up to at least 4096)
  a = Hdmi::acr(25201);
  CHECK(a.n_32khz >= 4096 && a.n_32khz < 8192 && a.n_44_1khz >= 5644 && a.n_48khz >= 6144);
  CHECK(uint64_t{a.cts_32khz} * 128 * 32000 / a.n_32khz == 25201ull * 1000 * 128 * 32000 / (128 * 32000ull) && uint64_t{a.cts_48khz} * 6144000 == 25201ull * 1000 * a.n_48khz);
  // a calculated pair always has CTS / N = clock / (128 fs) exactly
  for (uint32_t clock : {1u, 3u, 148353u, 297000u, 594000u, 593999u, 600000u, 33333u}) {
    const Hdmi::Acr c = Hdmi::acr(clock);
    CHECK(uint64_t{c.cts_32khz} * 4096000 == uint64_t{clock} * 1000 * c.n_32khz);
    CHECK(uint64_t{c.cts_44_1khz} * 5644800 == uint64_t{clock} * 1000 * c.n_44_1khz);
    CHECK(uint64_t{c.cts_48khz} * 6144000 == uint64_t{clock} * 1000 * c.n_48khz);
    CHECK(c.n_32khz >= 4096 && c.n_44_1khz >= 5644 && c.n_48khz >= 6144);
  }
  // no clock: nothing to regenerate, but N is still valid
  a = Hdmi::acr(0);
  CHECK(a.n_32khz == 4096 && a.cts_32khz == 0 && a.n_44_1khz == 5644 && a.cts_44_1khz == 0 && a.n_48khz == 6144 && a.cts_48khz == 0);
}

static void test_vic() {
  const struct { HdmiMode m; uint8_t vic; } modes[] = {
      {mode(25175, 640, 480, 800, 525), 1},
      {mode(27000, 720, 480, 858, 525), 2},
      {mode(27000, 720, 480, 858, 525, false, false, 1), 2},
      {mode(27000, 720, 480, 858, 525, false, false, 2), 3},
      {mode(74250, 1280, 720, 1650, 750), 4},
      {mode(74250, 1920, 1080, 2200, 1125, true), 5},
      {mode(13500, 720, 480, 858, 525, true, true), 6},
      {mode(13500, 720, 480, 858, 525, true, true, 2), 7},
      {mode(148500, 1920, 1080, 2200, 1125), 16},
      {mode(27000, 720, 576, 864, 625), 17},
      {mode(27000, 720, 576, 864, 625, false, false, 2), 18},
      {mode(74250, 1280, 720, 1980, 750), 19},
      {mode(74250, 1920, 1080, 2640, 1125, true), 20},
      {mode(13500, 720, 576, 864, 625, true, true), 21},
      {mode(13500, 720, 576, 864, 625, true, true, 2), 22},
      {mode(148500, 1920, 1080, 2640, 1125), 31},
      {mode(74250, 1920, 1080, 2750, 1125), 32},
      {mode(74250, 1920, 1080, 2640, 1125), 33},
      {mode(74250, 1920, 1080, 2200, 1125), 34},
      {mode(297000, 3840, 2160, 5500, 2250), 93},
      {mode(297000, 3840, 2160, 5280, 2250), 94},
      {mode(297000, 3840, 2160, 4400, 2250), 95},
      {mode(594000, 3840, 2160, 5280, 2250), 96},
      {mode(594000, 3840, 2160, 4400, 2250), 97},
      {mode(148352, 1920, 1080, 2200, 1125), 16},         // 59.94 Hz
      {mode(74176, 1280, 720, 1650, 750), 4},
  };
  for (const auto& m : modes) CHECK(Hdmi::vic(m.m) == m.vic);
  // no CEA mode: other sizes (the panel's own, a 1366x768 one), a 1080p at 75 Hz, a flag that does not match, no timing, a wrong aspect for the mode
  const HdmiMode none[] = {mode(85500, 1366, 768, 1792, 798), mode(241500, 2560, 1440, 2720, 1481), mode(193250, 1920, 1200, 2592, 1245), mode(198000, 1920, 1080, 2200, 1125 / 1 + 0),
                           mode(148500, 1920, 1080, 2200, 1125, true), mode(148500, 1920, 1080, 2200, 1125, false, true), mode(13500, 720, 480, 858, 525, true, false),
                           mode(13500, 720, 480, 858, 525, false, true), mode(0, 1920, 1080, 2200, 1125), mode(148500, 1920, 1080, 0, 1125), mode(148500, 1920, 1080, 2200, 0),
                           mode(148500, 1920, 1080, 2200, 1125, false, false, 1), mode(74250, 1280, 720, 1650, 750, false, false, 1)};
  for (const HdmiMode& m : none) CHECK(Hdmi::vic(m) == 0);
  // the refresh rate may be half a percent off the standard's, not more: 148.5 MHz x 1.0049 and x 0.9951 are 1080p60 still, x 1.0051 and x 0.9949 are not
  CHECK(Hdmi::vic(mode(149228, 1920, 1080, 2200, 1125)) == 16 && Hdmi::vic(mode(147772, 1920, 1080, 2200, 1125)) == 16);
  CHECK(Hdmi::vic(mode(149258, 1920, 1080, 2200, 1125)) == 0 && Hdmi::vic(mode(147742, 1920, 1080, 2200, 1125)) == 0);
  // an interlaced mode's refresh is the field rate: the same clock and totals as a progressive mode of half the lines is the same 60
  CHECK(Hdmi::vic(mode(74250, 1920, 1080, 2200, 1125, true)) == 5 && Hdmi::vic(mode(37125, 1920, 1080, 2200, 1125, true)) == 0);
}

static uint8_t sum(const uint8_t* p, size_t n) {
  uint8_t s = 0;
  for (size_t i = 0; i < n; ++i) s = static_cast<uint8_t>(s + p[i]);
  return s;
}

static void test_avi() {
  uint8_t f[Hdmi::kAviBytes];
  // 1080p60, 16:9: header 0x82 2 13, checksum, PB1 0x10 (RGB, active format information valid), PB2 (16:9 << 4 | active aspect 8) 0x28, PB3 0, VIC 16, the rest 0
  Hdmi::avi_infoframe(mode(148500, 1920, 1080, 2200, 1125), ByteView(f, sizeof f));
  const uint8_t want[Hdmi::kAviBytes] = {0x82, 0x02, 0x0D, 0x27, 0x10, 0x28, 0x00, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(memcmp(f, want, sizeof f) == 0 && sum(f, sizeof f) == 0);
  // 480i: 4:3, VIC 6, pixel repeat 1
  Hdmi::avi_infoframe(mode(13500, 720, 480, 858, 525, true, true), ByteView(f, sizeof f));
  const uint8_t ntsc[Hdmi::kAviBytes] = {0x82, 0x02, 0x0D, 0x40, 0x10, 0x18, 0x00, 6, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(memcmp(f, ntsc, sizeof f) == 0 && sum(f, sizeof f) == 0);
  // 576p 16:9 by request: VIC 18
  Hdmi::avi_infoframe(mode(27000, 720, 576, 864, 625, false, false, 2), ByteView(f, sizeof f));
  CHECK(f[7] == 18 && f[5] == 0x28 && f[8] == 0 && sum(f, sizeof f) == 0);
  // no CEA mode: VIC 0, no picture aspect unless asked for
  Hdmi::avi_infoframe(mode(85500, 1366, 768, 1792, 798), ByteView(f, sizeof f));
  CHECK(f[7] == 0 && f[5] == 0x08 && f[4] == 0x10 && sum(f, sizeof f) == 0);
  Hdmi::avi_infoframe(mode(85500, 1366, 768, 1792, 798, false, false, 2), ByteView(f, sizeof f));
  CHECK(f[7] == 0 && f[5] == 0x28 && sum(f, sizeof f) == 0);
  Hdmi::avi_infoframe(mode(85500, 1366, 768, 1792, 798, false, false, 1), ByteView(f, sizeof f));
  CHECK(f[5] == 0x18 && sum(f, sizeof f) == 0);
  // 4K: the VIC is 7 bits wide; and a longer buffer is only written for the frame
  uint8_t big[24];
  memset(big, 0xEE, sizeof big);
  Hdmi::avi_infoframe(mode(594000, 3840, 2160, 4400, 2250), ByteView(big, sizeof big));
  CHECK(big[7] == 97 && sum(big, Hdmi::kAviBytes) == 0 && big[17] == 0xEE && big[23] == 0xEE);
  // every VIC the table has gives a frame with the checksum right
  for (const auto& m : {mode(25175, 640, 480, 800, 525), mode(74250, 1920, 1080, 2200, 1125, true), mode(297000, 3840, 2160, 5500, 2250)}) {
    Hdmi::avi_infoframe(m, ByteView(f, sizeof f));
    CHECK(sum(f, sizeof f) == 0 && f[0] == 0x82 && f[1] == 2 && f[2] == 13);
  }
}

int main() {
  test_acr();
  test_vic();
  test_avi();
  printf(g_failed ? "HdmiInfoframeTest: %d FAILED\n" : "HdmiInfoframeTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
