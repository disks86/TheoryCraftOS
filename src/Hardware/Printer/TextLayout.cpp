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

#include "Hardware/Printer/TextLayout.hpp"

namespace hardware {

TextGeometry text_geometry(PaperSize paper, bool landscape, uint32_t unprintable_mm10) {
  const PaperInfo& p = paper_info(paper);
  const uint32_t across = landscape ? p.height_mm10 : p.width_mm10, down = landscape ? p.width_mm10 : p.height_mm10;
  TextGeometry g;
  if (across <= 2 * unprintable_mm10 || down <= 2 * unprintable_mm10) return g;
  g.columns = (across - 2 * unprintable_mm10) * 10 / 254;
  g.lines = (down - 2 * unprintable_mm10) * 6 / 254;
  return g;
}

ConstByteView TextLayout::line(uint32_t page, uint32_t line) const {
  if (page >= pages_.count() || line >= pages_[page].count) return ConstByteView();
  const Line& l = lines_[pages_[page].first + line];
  return chars_.view().subview(l.start, l.length);
}

bool TextLayout::layout(StringView text, uint32_t columns, uint32_t lines) {
  if (columns < 10 || lines < 5) return false;
  chars_.clear();
  lines_.clear();
  pages_.clear();
  columns_ = columns;
  lines_per_page_ = lines;
  uint32_t column = 0, line = 0;
  bool page_open = false, ok = true;
  auto open_page = [&]() {
    if (page_open) return;
    page_open = true;
    column = line = 0;
    ok = ok && pages_.push_back(Page{static_cast<uint32_t>(lines_.count()), 1}) && lines_.push_back(Line{static_cast<uint32_t>(chars_.size()), 0});
  };
  auto eject = [&]() { page_open = false; };
  auto newline = [&]() {
    column = 0;
    if (++line >= lines) {
      eject();
      return;
    }
    ok = ok && lines_.push_back(Line{static_cast<uint32_t>(chars_.size()), 0});
    ++pages_[pages_.count() - 1].count;
  };
  auto emit = [&](uint32_t c) {  // one character at the current position, wrapping
    if (column >= columns) newline();
    open_page();
    ++column;
    ok = ok && chars_.put8(static_cast<uint8_t>(c));
    ++lines_[lines_.count() - 1].length;
  };
  const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
  for (uint64_t i = 0; i < text.size() && ok; ++i) {
    uint32_t c = d[i];
    if (c >= 0xC0 && c < 0xF8) {  // UTF-8 sequence -> code point
      const uint32_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      uint32_t cp = c & (0x3F >> extra);
      uint32_t k = 0;
      while (k < extra && i + 1 < text.size() && (d[i + 1] & 0xC0) == 0x80) {
        cp = (cp << 6) | (d[++i] & 0x3F);
        ++k;
      }
      c = cp < 256 ? cp : '?';
    } else if (c >= 0x80) {
      c = '?';
    }
    if (c == '\n') {
      open_page();
      newline();
    } else if (c == '\r') {
      // a carriage return alone only moves the print head back; the layout has nothing to do
    } else if (c == '\f') {
      eject();
    } else if (c == '\t') {
      do emit(' '); while (ok && column % 8 != 0 && column < columns);
    } else if (c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xA0)) {
      emit(c);
    }
  }
  return ok;
}

}  // namespace hardware
