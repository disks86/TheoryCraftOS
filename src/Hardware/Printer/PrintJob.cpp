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

#include "Hardware/Printer/PrintJob.hpp"

namespace hardware {

namespace {
const char kEsc = '\x1b';
}

bool PrintJob::put(const char* text) {
  uint64_t n = 0;
  while (text[n]) ++n;
  return out_.append(ConstByteView(reinterpret_cast<const uint8_t*>(text), n));
}

bool PrintJob::put_number(const char* before, uint32_t value, const char* after) {
  char digits[12];
  int n = 0;
  do {
    digits[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  if (!put(before)) return false;
  while (n) {
    const char c = digits[--n];
    if (!out_.put8(static_cast<uint8_t>(c))) return false;
  }
  return put(after);
}

bool PrintJob::put_uel() { return put("\x1b%-12345X"); }

uint32_t PrintJob::page_width() const {
  const PaperInfo& p = paper_info(settings_.paper);
  const uint32_t w = settings_.landscape ? p.height_mm10 : p.width_mm10;
  return mm10_to_pixels(w - 2 * printer_.unprintable_mm10, settings_.dpi);
}

uint32_t PrintJob::page_height() const {
  const PaperInfo& p = paper_info(settings_.paper);
  const uint32_t h = settings_.landscape ? p.width_mm10 : p.height_mm10;
  return mm10_to_pixels(h - 2 * printer_.unprintable_mm10, settings_.dpi);
}

bool PrintJob::begin() {
  if (begun_ || failed_) return false;
  if (!printer_.supports(settings_.language) || !printer_.supports(settings_.paper)) return fail();
  if (settings_.dpi != 300 && settings_.dpi != 600 && settings_.dpi != 1200) return fail();
  if (settings_.dpi > printer_.max_dpi || settings_.copies == 0 || settings_.copies > 999) return fail();
  begun_ = true;
  // The job name goes in quotes inside a PJL line: keep it printable and drop the quotes.
  char name[64];
  uint32_t n = 0;
  for (const char* c = settings_.name; c && *c && n < 63; ++c)
    if (*c >= 0x20 && *c < 0x7f && *c != '"') name[n++] = *c;
  name[n] = 0;
  bool ok = put_uel() && put("@PJL JOB NAME=\"") && put(name) && put("\"\r\n");
  ok = ok && put_number("@PJL SET RESOLUTION=", settings_.dpi, "\r\n");
  ok = ok && put(settings_.toner_save ? "@PJL SET ECONOMODE=ON\r\n" : "@PJL SET ECONOMODE=OFF\r\n");
  if (settings_.language == PrinterLanguage::Pcl) {
    ok = ok && put("@PJL ENTER LANGUAGE=PCL\r\n");
    ok = ok && out_.put8(kEsc) && put("E");                                   // reset
    ok = ok && out_.put8(kEsc) && put_number("&u", settings_.dpi, "D");       // units of measure: dots
  } else {
    ok = ok && put("@PJL ENTER LANGUAGE=POSTSCRIPT\r\n");
  }
  return ok ? true : fail();
}

// The commands that open a page: paper size, orientation, tray and number of copies.
bool PrintJob::page_header() {
  static const uint8_t kSource[] = {7, 1, 2, 4};  // Auto, Tray1, Manual, Tray2
  bool ok = out_.put8(kEsc) && put_number("&l", paper_info(settings_.paper).pcl_code, "A");
  ok = ok && out_.put8(kEsc) && put(settings_.landscape ? "&l1O" : "&l0O");
  ok = ok && out_.put8(kEsc) && put_number("&l", kSource[static_cast<uint8_t>(settings_.source) & 3], "H");
  ok = ok && out_.put8(kEsc) && put_number("&l", settings_.copies, "X");
  return ok;
}

bool PrintJob::add_page(const PageBitmap& page) {
  if (!begun_ || finished_ || failed_) return false;
  if (settings_.language != PrinterLanguage::Pcl) return fail();
  if (page.width() == 0 || page.width() > page_width() || page.height() > page_height()) return fail();
  if (!page_header()) return fail();
  bool ok = out_.put8(kEsc) && put_number("*t", settings_.dpi, "R");
  ok = ok && out_.put8(kEsc) && put("*p0X") && out_.put8(kEsc) && put("*p0Y");  // top left of the printable area
  ok = ok && out_.put8(kEsc) && put("*r1A") && out_.put8(kEsc) && put("*b2M");  // start raster graphics, PackBits rows
  if (!ok) return fail();
  const uint32_t stride = page.stride();
  ArrayView<uint8_t> packed = allocate_view<uint8_t>(allocator_, uint64_t{stride} + stride / 128 + 2);
  if (packed.empty()) return fail();
  for (uint32_t y = 0; y < page.height() && ok; ++y) {
    const uint8_t* row = page.row(y);
    uint32_t used = stride;
    while (used > 0 && row[used - 1] == 0) --used;  // the rest of the line is white anyway
    if (used == 0) {
      ok = out_.put8(kEsc) && put("*b0W");  // a blank row
      continue;
    }
    const uint32_t n = packbits(row, used, packed.data());
    ok = out_.put8(kEsc) && put_number("*b", n, "W") && out_.append(ConstByteView(packed.data(), n));
  }
  release_view(allocator_, packed);
  ok = ok && out_.put8(kEsc) && put("*rB") && put("\f");  // end raster graphics, eject the page
  if (!ok) return fail();
  ++pages_;
  return true;
}

// Text is laid out by TextLayout (the same layout the print preview shows) and printed in a fixed-pitch font so the layout is what the
// printer prints. Only the pages from `first_page` to `last_page` (counted from 1; 0 for the last means to the end) are printed.
bool PrintJob::add_text(StringView text, uint32_t first_page, uint32_t last_page) {
  if (!begun_ || finished_ || failed_) return false;
  if (settings_.language != PrinterLanguage::Pcl) return fail();
  const TextGeometry geometry = text_geometry(settings_.paper, settings_.landscape, printer_.unprintable_mm10);
  if (!geometry.valid()) return fail();
  TextLayout layout(allocator_);
  if (!layout.layout(text, geometry.columns, geometry.lines)) return fail();
  if (first_page == 0) first_page = 1;
  if (last_page == 0 || last_page > layout.page_count()) last_page = layout.page_count();
  for (uint32_t page = first_page - 1; page < last_page; ++page) {
    if (!page_header()) return fail();
    // Latin-1 (Windows 19U), Courier-like fixed pitch at 10 cpi, 6 lpi, no top margin, LF as CR+LF.
    bool ok = out_.put8(kEsc) && put("(19U") && out_.put8(kEsc) && put("(s0p10h12v0s0b3T") && out_.put8(kEsc) && put("&l6D") && out_.put8(kEsc) &&
              put("&l0E") && out_.put8(kEsc) && put_number("&l", geometry.lines, "F") && out_.put8(kEsc) && put("&k2G") && out_.put8(kEsc) &&
              put("&a0L") && out_.put8(kEsc) && put("*p0X") && out_.put8(kEsc) && put("*p0Y");
    for (uint32_t line = 0; ok && line < layout.line_count(page); ++line) {
      if (line > 0) ok = put("\r\n");
      ok = ok && out_.append(layout.line(page, line));
    }
    ok = ok && put("\f");
    if (!ok) return fail();
    ++pages_;
  }
  return true;
}

bool PrintJob::add_postscript(ConstByteView program) {
  if (!begun_ || finished_ || failed_) return false;
  if (settings_.language != PrinterLanguage::PostScript) return fail();
  if (!out_.append(program)) return fail();
  ++pages_;  // (the program decides how many pages it prints; this counts programs)
  return true;
}

bool PrintJob::finish() {
  if (!begun_ || finished_ || failed_) return false;
  finished_ = true;
  bool ok = true;
  if (settings_.language == PrinterLanguage::Pcl) ok = out_.put8(kEsc) && put("E");  // reset
  else ok = out_.put8(0x04);                                                           // end of PostScript job
  ok = ok && put_uel() && put("@PJL EOJ\r\n") && put_uel();
  return ok ? true : fail();
}

}  // namespace hardware
