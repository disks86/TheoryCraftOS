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

#include "Hardware/GraphicsProcessingUnit/AMD/HdmiInfoframe.hpp"

namespace hardware {

namespace {
// amdgpu_afmt_predefined_acr: the clock in kHz, then N and CTS for 32, 44.1 and 48 kHz
struct AcrRow {
  uint32_t clock, n32, cts32, n44, cts44, n48, cts48;
};
// (amdgpu_afmt.c v6.12 has {74176, 4096, 74176, 5733, 75335, 6144, 74176} and {148352, 4096, 148352, 5733, 150670, 6144, 148352} for the two 1.001 rows, which are off from the HDMI
// formula by 0.1 %; the values here are the specification's, as in radeon's table.)
const AcrRow kPredefined[] = {
    {25175, 4096, 25175, 28224, 125875, 6144, 25175},     // 25.20 / 1.001 MHz
    {25200, 4096, 25200, 6272, 28000, 6144, 25200},       // 25.20 MHz
    {27000, 4096, 27000, 6272, 30000, 6144, 27000},       // 27.00 MHz
    {27027, 4096, 27027, 6272, 30030, 6144, 27027},       // 27.00 * 1.001 MHz
    {54000, 4096, 54000, 6272, 60000, 6144, 54000},       // 54.00 MHz
    {54054, 4096, 54054, 6272, 60060, 6144, 54054},       // 54.00 * 1.001 MHz
    {74176, 11648, 210937, 17836, 234375, 11648, 140625}, // 74.25 / 1.001 MHz
    {74250, 4096, 74250, 6272, 82500, 6144, 74250},       // 74.25 MHz
    {148352, 11648, 421875, 8918, 234375, 5824, 140625},  // 148.50 / 1.001 MHz
    {148500, 4096, 148500, 6272, 165000, 6144, 148500},   // 148.50 MHz
};

uint64_t gcd(uint64_t a, uint64_t b) {
  while (b != 0) {
    const uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// amdgpu_afmt_calc_cts
void calc_cts(uint32_t clock_khz, uint32_t* cts, uint32_t* n_out, uint32_t freq) {
  uint64_t n = uint64_t{128} * freq;
  uint64_t c = uint64_t{clock_khz} * 1000;
  const uint64_t div = gcd(n, c);       // the smallest valid fraction
  if (div != 0) {
    n /= div;
    c /= div;
  }
  // the optimal N is 128 * freq / 1000: the closest larger value that does not truncate any bits
  const uint64_t optimal = uint64_t{128} * freq / 1000;
  const uint64_t mul = n == 0 ? 1 : (optimal + (n - 1)) / n;
  n *= mul;
  c *= mul;
  *n_out = static_cast<uint32_t>(n);
  *cts = static_cast<uint32_t>(c);
}

// The CEA video modes that matter (CTA-861): the VIC, the active size, interlaced, the double clock, the nominal refresh (the field rate), the picture aspect (1 4:3, 2 16:9)
struct VicRow {
  uint8_t vic;
  uint16_t h, v;
  bool interlaced, double_clock;
  uint8_t hz, aspect;
};
const VicRow kVics[] = {
    {1, 640, 480, false, false, 60, 1},   {2, 720, 480, false, false, 60, 1},   {3, 720, 480, false, false, 60, 2},   {4, 1280, 720, false, false, 60, 2},
    {5, 1920, 1080, true, false, 60, 2},  {6, 720, 480, true, true, 60, 1},     {7, 720, 480, true, true, 60, 2},     {16, 1920, 1080, false, false, 60, 2},
    {17, 720, 576, false, false, 50, 1},  {18, 720, 576, false, false, 50, 2},  {19, 1280, 720, false, false, 50, 2}, {20, 1920, 1080, true, false, 50, 2},
    {21, 720, 576, true, true, 50, 1},    {22, 720, 576, true, true, 50, 2},    {31, 1920, 1080, false, false, 50, 2}, {32, 1920, 1080, false, false, 24, 2},
    {33, 1920, 1080, false, false, 25, 2}, {34, 1920, 1080, false, false, 30, 2}, {93, 3840, 2160, false, false, 24, 2}, {94, 3840, 2160, false, false, 25, 2},
    {95, 3840, 2160, false, false, 30, 2}, {96, 3840, 2160, false, false, 50, 2}, {97, 3840, 2160, false, false, 60, 2},
};

constexpr uint8_t kActiveAspectPicture = 8;       // HDMI_ACTIVE_ASPECT_PICTURE: the active format is the same as the picture's
}  // namespace

Hdmi::Acr Hdmi::acr(uint32_t clock_khz) {
  Acr r;
  for (const AcrRow& row : kPredefined) {
    if (row.clock != clock_khz) continue;
    r.n_32khz = row.n32;
    r.cts_32khz = row.cts32;
    r.n_44_1khz = row.n44;
    r.cts_44_1khz = row.cts44;
    r.n_48khz = row.n48;
    r.cts_48khz = row.cts48;
    return r;
  }
  calc_cts(clock_khz, &r.cts_32khz, &r.n_32khz, 32000);
  calc_cts(clock_khz, &r.cts_44_1khz, &r.n_44_1khz, 44100);
  calc_cts(clock_khz, &r.cts_48khz, &r.n_48khz, 48000);
  return r;
}

uint8_t Hdmi::vic(const HdmiMode& mode) {
  if (mode.htotal == 0 || mode.vtotal == 0) return 0;
  // the refresh in thousandths of a hertz: the frame rate, doubled for the field rate of an interlaced mode
  uint64_t millihz = uint64_t{mode.clock_khz} * 1000 * 1000 / (uint64_t{mode.htotal} * mode.vtotal);
  if (mode.interlaced) millihz *= 2;
  for (const VicRow& row : kVics) {
    if (row.h != mode.hdisplay || row.v != mode.vdisplay || row.interlaced != mode.interlaced || row.double_clock != mode.double_clock) continue;
    if (mode.picture_aspect != 0 && mode.picture_aspect != row.aspect) continue;
    const uint64_t nominal = uint64_t{row.hz} * 1000;
    const uint64_t diff = millihz > nominal ? millihz - nominal : nominal - millihz;
    if (diff * 200 > nominal) continue;     // more than half a percent off
    return row.vic;
  }
  return 0;
}

void Hdmi::avi_infoframe(const HdmiMode& mode, ByteView out) {
  uint8_t frame[kAviBytes] = {};
  const uint8_t vic = Hdmi::vic(mode);
  uint8_t aspect = mode.picture_aspect;      // HDMI_PICTURE_ASPECT_4_3 is 1, _16_9 2, none 0
  if (aspect == 0 && vic != 0)
    for (const VicRow& row : kVics)
      if (row.vic == vic) aspect = row.aspect;
  frame[0] = kAviType;
  frame[1] = kAviVersion;
  frame[2] = kAviLength;
  // frame[3] is the checksum
  frame[4] = 0x10;                                                      // PB1: RGB, no bar info, the active format information is valid (A0), no scan information
  frame[5] = static_cast<uint8_t>((aspect & 0x3) << 4 | kActiveAspectPicture);     // PB2: no colorimetry, the picture aspect, the active format aspect
  frame[6] = 0;                                                         // PB3: no extended colorimetry, the default quantization range, no scaling, no IT content
  frame[7] = vic & 0x7F;                                                // PB4
  frame[8] = mode.double_clock ? 1 : 0;                                 // PB5: the pixel repeat (graphics content, the default YCC quantization range)
  // PB6 to PB13: the bar positions, all zero
  uint8_t sum = 0;
  for (uint8_t b : frame) sum = static_cast<uint8_t>(sum + b);
  frame[3] = static_cast<uint8_t>(0 - sum);
  for (uint32_t i = 0; i < kAviBytes; ++i) out[i] = frame[i];
}

}  // namespace hardware
