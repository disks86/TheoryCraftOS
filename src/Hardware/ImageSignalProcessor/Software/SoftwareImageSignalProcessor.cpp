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

#include "Hardware/ImageSignalProcessor/Software/SoftwareImageSignalProcessor.hpp"
#include "Hardware/ImageSignalProcessor/IspMath.hpp"

namespace hardware {

namespace {
enum Color : uint8_t { kRed = 0, kGreen = 1, kBlue = 2 };

// Colour of each site in the 2x2 tile, row-major from the top left.
constexpr Color kTiles[4][4] = {
    {kRed, kGreen, kGreen, kBlue},   // RGGB
    {kBlue, kGreen, kGreen, kRed},   // BGGR
    {kGreen, kRed, kBlue, kGreen},   // GRBG
    {kGreen, kBlue, kRed, kGreen},   // GBRG
};

bool valid(const RawFrame& f) {
  return f.pixels && f.width >= 4 && f.height >= 4 && f.stride >= f.width && f.bit_depth >= 8 &&
         f.bit_depth <= 16;
}

class Frame {
 public:
  explicit Frame(const RawFrame& f) : f_(f), tile_(kTiles[static_cast<uint8_t>(f.pattern)]) {}

  Color color(uint32_t x, uint32_t y) const { return tile_[(y & 1) * 2 + (x & 1)]; }

  // Edge samples reflect by two so the neighbour keeps the same colour as the
  // one it replaces (a one-pixel mirror would land on the wrong colour).
  uint32_t at(int32_t x, int32_t y) const {
    if (x < 0) x += 2;
    else if (x >= static_cast<int32_t>(f_.width)) x -= 2;
    if (y < 0) y += 2;
    else if (y >= static_cast<int32_t>(f_.height)) y -= 2;
    return f_.pixels[static_cast<uint64_t>(y) * f_.stride + x];
  }

 private:
  const RawFrame& f_;
  const Color* tile_;
};

struct Rgb {
  uint32_t r, g, b;
};

// Bilinear interpolation: each missing channel is the mean of the nearest
// samples of that colour.
Rgb demosaic(const Frame& f, int32_t x, int32_t y) {
  const uint32_t p = f.at(x, y);
  const uint32_t plus = (f.at(x - 1, y) + f.at(x + 1, y) + f.at(x, y - 1) + f.at(x, y + 1) + 2) / 4;
  const uint32_t diag =
      (f.at(x - 1, y - 1) + f.at(x + 1, y - 1) + f.at(x - 1, y + 1) + f.at(x + 1, y + 1) + 2) / 4;
  switch (f.color(x, y)) {
    case kRed:   return {p, plus, diag};
    case kBlue:  return {diag, plus, p};
    default: {  // green: red and blue sit left/right on one row type, above/below on the other
      const uint32_t horizontal = (f.at(x - 1, y) + f.at(x + 1, y) + 1) / 2;
      const uint32_t vertical = (f.at(x, y - 1) + f.at(x, y + 1) + 1) / 2;
      if (f.color(x + 1, y) == kRed) return {horizontal, p, vertical};
      return {vertical, p, horizontal};
    }
  }
}
}  // namespace

SoftwareImageSignalProcessor::SoftwareImageSignalProcessor() {
  build_gamma_table(gamma_, kGammaEntries, settings_.gamma_x100);
}

bool SoftwareImageSignalProcessor::set_settings(const IspSettings& settings) {
  if (settings.gamma_x100 < 50 || settings.gamma_x100 > 500) return false;
  const bool gamma_changed = settings.gamma_x100 != settings_.gamma_x100;
  settings_ = settings;
  if (gamma_changed) build_gamma_table(gamma_, kGammaEntries, settings_.gamma_x100);
  return true;
}

bool SoftwareImageSignalProcessor::process(const RawFrame& in, ImageBuffer* out) {
  if (!valid(in) || !out || !out->pixels || out->width < in.width || out->height < in.height ||
      out->stride < in.width || in.width > max_dimension() || in.height > max_dimension())
    return false;

  const Frame frame(in);
  const uint32_t max_raw = (1u << in.bit_depth) - 1;
  const uint32_t black = settings_.black_level < max_raw ? settings_.black_level : max_raw - 1;
  const uint32_t range = max_raw - black;                 // usable span after subtracting black
  const uint64_t to_table = (uint64_t{kGammaEntries - 1} << 16) / range;  // Q16 raw -> table index

  auto encode = [&](uint32_t value, uint32_t gain) -> uint32_t {
    value = value > black ? value - black : 0;
    uint64_t v = (uint64_t{value} * gain) >> 8;           // white balance
    if (v > range) v = range;
    return gamma_[(v * to_table) >> 16];
  };

  for (uint32_t y = 0; y < in.height; ++y) {
    uint32_t* row = out->pixels + uint64_t{y} * out->stride;
    for (uint32_t x = 0; x < in.width; ++x) {
      const Rgb c = demosaic(frame, static_cast<int32_t>(x), static_cast<int32_t>(y));
      row[x] = (encode(c.r, settings_.gain_red) << 16) | (encode(c.g, settings_.gain_green) << 8) |
               encode(c.b, settings_.gain_blue);
    }
  }
  return true;
}

bool SoftwareImageSignalProcessor::analyze(const RawFrame& in, IspStatistics* out) {
  if (!valid(in) || !out) return false;
  const Frame frame(in);
  uint64_t sum[3] = {0, 0, 0};
  uint64_t count[3] = {0, 0, 0};
  for (uint32_t y = 0; y < in.height; ++y) {
    for (uint32_t x = 0; x < in.width; ++x) {
      const Color c = frame.color(x, y);
      sum[c] += frame.at(x, y);
      ++count[c];
    }
  }
  const uint32_t mean[3] = {static_cast<uint32_t>(sum[0] / count[0]),
                            static_cast<uint32_t>(sum[1] / count[1]),
                            static_cast<uint32_t>(sum[2] / count[2])};
  out->mean_red = mean[kRed];
  out->mean_green = mean[kGreen];
  out->mean_blue = mean[kBlue];
  // Rec. 601 luma of the channel means, scaled down to 8 bits.
  const uint64_t luma = (299ull * mean[kRed] + 587ull * mean[kGreen] + 114ull * mean[kBlue]) / 1000;
  out->mean_luma = static_cast<uint8_t>(luma >> (in.bit_depth - 8));
  return true;
}

IspSettings auto_white_balance(const IspStatistics& s, const IspSettings& base) {
  auto gain = [&](uint32_t channel_mean) -> uint16_t {
    const uint32_t g = s.mean_green > base.black_level ? s.mean_green - base.black_level : 1;
    const uint32_t c = channel_mean > base.black_level ? channel_mean - base.black_level : 1;
    uint64_t value = (uint64_t{g} << 8) / c;  // Q8.8
    if (value < 64) value = 64;               // clamp to 0.25x .. 4x
    if (value > 1024) value = 1024;
    return static_cast<uint16_t>(value);
  };
  IspSettings result = base;
  result.gain_red = gain(s.mean_red);
  result.gain_green = 256;
  result.gain_blue = gain(s.mean_blue);
  return result;
}

}  // namespace hardware
