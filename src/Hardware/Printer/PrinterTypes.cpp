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

#include "Hardware/Printer/PrinterTypes.hpp"

namespace hardware {

const PaperInfo& paper_info(PaperSize size) {
  static const PaperInfo kPapers[] = {
      {"A4", 2100, 2970, 26},       {"Letter", 2159, 2794, 2}, {"Legal", 2159, 3556, 3}, {"Executive", 1841, 2667, 1},
      {"A5", 1480, 2100, 25},       {"B5 (JIS)", 1820, 2570, 45}, {"A6", 1050, 1480, 24},  {"Monarch", 984, 1905, 80},
  };
  const uint8_t i = static_cast<uint8_t>(size);
  return kPapers[i < static_cast<uint8_t>(PaperSize::Count) ? i : 0];
}

bool PageBitmap::create(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > 20000 || height > 30000) return false;
  const uint64_t bytes = uint64_t{(width + 7) / 8} * height;
  if (bytes > (64ull << 20)) return false;
  release_view(allocator_, bits_);
  bits_ = allocate_view<uint8_t>(allocator_, bytes);
  if (bits_.empty()) return false;
  bits_.fill(0);
  width_ = width;
  height_ = height;
  return true;
}

bool PageBitmap::create_for_paper(PaperSize paper, uint32_t dpi, uint32_t unprintable_mm10, bool landscape) {
  const PaperInfo& p = paper_info(paper);
  uint32_t w = p.width_mm10, h = p.height_mm10;
  if (landscape) {
    const uint32_t t = w;
    w = h;
    h = t;
  }
  if (w <= 2 * unprintable_mm10 || h <= 2 * unprintable_mm10) return false;
  return create(mm10_to_pixels(w - 2 * unprintable_mm10, dpi), mm10_to_pixels(h - 2 * unprintable_mm10, dpi));
}

uint64_t PageBitmap::black_dots() const {
  uint64_t n = 0;
  for (uint32_t y = 0; y < height_; ++y)
    for (uint32_t x = 0; x < width_; ++x) n += get(x, y);
  return n;
}

}  // namespace hardware
