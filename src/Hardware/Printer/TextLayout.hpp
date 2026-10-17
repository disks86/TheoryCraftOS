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

// How text falls onto printed pages, shared by PrintJob (which prints it) and the print preview (which shows it), so what is previewed is
// what comes out. A page is a grid of fixed-pitch character cells - 10 characters per inch across, 6 lines per inch down, inside the
// margins the printer cannot mark. The text is read as UTF-8 and laid out as Latin-1 (what the printer prints; other characters
// become '?'): lines wrap at the last column, a tab goes to the next multiple of 8 columns, a form feed starts a new page, a
// line break goes to the next line (the next page after the last line) and other control characters are dropped. A page exists only
// once something is put on it, so an empty text has no pages and a text ending in a line break has no empty page after it.
#pragma once
#include "Hardware/Printer/PrinterTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

// The characters across and lines down of a page of `paper` (turned sideways for landscape) with `unprintable_mm10` margins on every edge.
struct TextGeometry {
  uint32_t columns = 0, lines = 0;
  bool valid() const { return columns >= 10 && lines >= 5; }
};
TextGeometry text_geometry(PaperSize paper, bool landscape, uint32_t unprintable_mm10);

class TextLayout {
 public:
  explicit TextLayout(allocators::IAllocator& allocator) : chars_(allocator), lines_(allocator), pages_(allocator) {}

  // Lays `utf8` out on pages of `columns` x `lines` (replacing any earlier layout). False if the page is too small or memory ran out.
  bool layout(StringView utf8, uint32_t columns, uint32_t lines);

  uint32_t page_count() const { return static_cast<uint32_t>(pages_.count()); }
  // How many lines page `page` has (a page ends after its last line break, so the last one may be empty).
  uint32_t line_count(uint32_t page) const { return page < pages_.count() ? pages_[page].count : 0; }
  // The Latin-1 text of line `line` of page `page` (empty for one that does not exist).
  ConstByteView line(uint32_t page, uint32_t line) const;
  uint32_t columns() const { return columns_; }
  uint32_t lines_per_page() const { return lines_per_page_; }

 private:
  struct Line {
    uint32_t start, length;
  };
  struct Page {
    uint32_t first, count;
  };

  ByteBuffer chars_;
  List<Line> lines_;
  List<Page> pages_;
  uint32_t columns_ = 0, lines_per_page_ = 0;
};

}  // namespace hardware
