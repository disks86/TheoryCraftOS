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

#include "IspDemo.hpp"

namespace {
using namespace hardware;

constexpr uint32_t kWidth = 256, kHeight = 192;
constexpr uint32_t kBitDepth = 12;
constexpr uint16_t kBlackLevel = 64;

// Scene in linear light, 0-1000 per channel: eight colour bars over a grey ramp.
void scene(uint32_t x, uint32_t y, uint32_t rgb[3]) {
  static const uint8_t kBars[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0},
                                      {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
  if (y < kHeight * 2 / 3) {
    const uint8_t* bar = kBars[x * 8 / kWidth];
    for (int c = 0; c < 3; ++c) rgb[c] = bar[c] ? 800 : 40;
  } else {
    rgb[0] = rgb[1] = rgb[2] = 40 + x * 760 / kWidth;
  }
}

// What a sensor under warm light records: the same scene with weak red and blue.
constexpr uint32_t kSensorGain[3] = {55, 100, 70};  // percent

void mosaic(uint16_t* raw) {
  const uint32_t max = (1u << kBitDepth) - 1;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint32_t rgb[3];
      scene(x, y, rgb);
      // RGGB tile: red top-left, blue bottom-right, green elsewhere.
      const uint32_t c = (y & 1) ? ((x & 1) ? 2 : 1) : ((x & 1) ? 1 : 0);
      const uint32_t signal = rgb[c] * kSensorGain[c] / 100;          // 0-1000 scale
      raw[y * kWidth + x] = static_cast<uint16_t>(kBlackLevel + signal * (max - kBlackLevel) / 1000);
    }
  }
}

void draw(IGraphicsProcessingUnit& gpu, const uint32_t* pixels, uint32_t left, uint32_t top) {
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x) gpu.put_pixel(left + x, top + y, pixels[y * kWidth + x]);
}
}  // namespace

bool run_isp_demo(IGraphicsProcessingUnit& gpu, IImageSignalProcessor& isp,
                  allocators::IAllocator& allocator) {
  constexpr uint32_t kMargin = 12, kLabel = 18;
  if (gpu.width() < kWidth + 2 * kMargin || gpu.height() < 2 * (kHeight + kLabel) + 2 * kMargin) return false;

  auto* raw = static_cast<uint16_t*>(allocator.allocate(kWidth * kHeight * sizeof(uint16_t)));
  auto* neutral = static_cast<uint32_t*>(allocator.allocate(kWidth * kHeight * sizeof(uint32_t)));
  auto* balanced = static_cast<uint32_t*>(allocator.allocate(kWidth * kHeight * sizeof(uint32_t)));
  bool ok = raw && neutral && balanced;
  if (ok) {
    mosaic(raw);
    const RawFrame frame{raw, kWidth, kHeight, kWidth, kBitDepth, BayerPattern::Rggb};
    ImageBuffer out_neutral{neutral, kWidth, kHeight, kWidth};
    ImageBuffer out_balanced{balanced, kWidth, kHeight, kWidth};

    IspSettings settings;
    settings.black_level = kBlackLevel;
    IspStatistics stats;
    ok = isp.set_settings(settings) && isp.process(frame, &out_neutral) && isp.analyze(frame, &stats) &&
         isp.set_settings(auto_white_balance(stats, settings)) && isp.process(frame, &out_balanced);

    if (ok) {
      const uint32_t left = gpu.width() - kWidth - kMargin;
      gpu.draw_text(left, kMargin, "ISP: neutral", 0xE0E0E0, 0x101018);
      draw(gpu, neutral, left, kMargin + kLabel);
      const uint32_t second = kMargin + kLabel + kHeight + kMargin;
      gpu.draw_text(left, second, "ISP: auto white balance", 0xE0E0E0, 0x101018);
      draw(gpu, balanced, left, second + kLabel);
    }
  }
  allocator.deallocate(raw);
  allocator.deallocate(neutral);
  allocator.deallocate(balanced);
  return ok;
}
