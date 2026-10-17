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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

// Paper sizes a printer can be asked for. `pcl_code` is the value of the PCL "page size" command (ESC & l # A).
enum class PaperSize : uint8_t { A4, Letter, Legal, Executive, A5, B5, A6, Monarch, Count };

struct PaperInfo {
  const char* name;
  uint32_t width_mm10, height_mm10;  // tenths of a millimetre, portrait
  uint8_t pcl_code;
};
const PaperInfo& paper_info(PaperSize size);
constexpr uint32_t mm10_to_pixels(uint32_t mm10, uint32_t dpi) { return static_cast<uint32_t>((uint64_t{mm10} * dpi + 127) / 254); }

enum class PaperSource : uint8_t { Auto, Tray1, Manual, Tray2 };
enum class PrinterLanguage : uint8_t { Pcl, PostScript };

// What a printer is and can do.
struct PrinterInfo {
  const char* manufacturer = "";
  const char* model = "";
  uint8_t languages = 0;          // bit per PrinterLanguage
  uint16_t native_dpi = 600;      // resolution of the engine
  uint16_t max_dpi = 600;         // highest setting the printer accepts (1200 for "HQ1200" class engines)
  uint8_t pages_per_minute = 0;
  bool color = false;
  bool duplex = false;
  uint16_t paper_sizes = 0;       // bit per PaperSize
  uint8_t unprintable_mm10 = 42;  // margin on every edge that the engine cannot mark
  bool supports(PrinterLanguage l) const { return (languages >> static_cast<uint8_t>(l)) & 1; }
  bool supports(PaperSize p) const { return (paper_sizes >> static_cast<uint8_t>(p)) & 1; }
};

struct JobSettings {
  const char* name = "TheoryCraftOS";
  PrinterLanguage language = PrinterLanguage::Pcl;
  PaperSize paper = PaperSize::A4;
  PaperSource source = PaperSource::Auto;
  uint16_t dpi = 600;
  uint16_t copies = 1;
  bool toner_save = false;
  bool landscape = false;
};

// A page as the engine prints it: one bit per dot, 1 = toner, rows from the top, most significant bit first, rows
// padded to a byte.
class PageBitmap {
 public:
  explicit PageBitmap(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~PageBitmap() { release_view(allocator_, bits_); }
  PageBitmap(const PageBitmap&) = delete;
  PageBitmap& operator=(const PageBitmap&) = delete;

  // A blank (white) page of the given size in dots. False if the size is unreasonable or memory ran out.
  bool create(uint32_t width, uint32_t height);
  // A blank page covering the printable area of `paper` at `dpi` with `unprintable_mm10` margins.
  bool create_for_paper(PaperSize paper, uint32_t dpi, uint32_t unprintable_mm10, bool landscape);
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t stride() const { return (width_ + 7) / 8; }
  uint8_t* row(uint32_t y) const { return bits_.data() + uint64_t{y} * stride(); }
  bool get(uint32_t x, uint32_t y) const { return (row(y)[x >> 3] >> (7 - (x & 7))) & 1; }
  void set(uint32_t x, uint32_t y, bool black) const {
    uint8_t& b = row(y)[x >> 3];
    const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
    b = black ? static_cast<uint8_t>(b | m) : static_cast<uint8_t>(b & ~m);
  }
  uint64_t black_dots() const;

 private:
  allocators::IAllocator& allocator_;
  ByteView bits_;
  uint32_t width_ = 0, height_ = 0;
};

}  // namespace hardware
