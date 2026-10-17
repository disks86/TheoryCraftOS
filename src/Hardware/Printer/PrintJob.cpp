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

// Text is laid out here, a page at a time, and printed in a fixed-pitch font so the layout is what the printer prints.
bool PrintJob::add_text(StringView text) {
  if (!begun_ || finished_ || failed_) return false;
  if (settings_.language != PrinterLanguage::Pcl) return fail();
  const uint32_t width_mm10 = (settings_.landscape ? paper_info(settings_.paper).height_mm10 : paper_info(settings_.paper).width_mm10) - 2 * printer_.unprintable_mm10;
  const uint32_t height_mm10 = (settings_.landscape ? paper_info(settings_.paper).width_mm10 : paper_info(settings_.paper).height_mm10) - 2 * printer_.unprintable_mm10;
  const uint32_t columns = width_mm10 * 10 / 254, lines = height_mm10 * 6 / 254;  // 10 characters and 6 lines per inch
  if (columns < 10 || lines < 5) return fail();
  uint32_t column = 0, line = 0;
  bool page_open = false;
  auto open_page = [&]() {
    if (!page_open) {
      if (!page_header()) return false;
      // Latin-1 (Windows 19U), Courier-like fixed pitch at 10 cpi, 6 lpi, no top margin, LF as CR+LF.
      bool ok = out_.put8(kEsc) && put("(19U") && out_.put8(kEsc) && put("(s0p10h12v0s0b3T") && out_.put8(kEsc) && put("&l6D") &&
                out_.put8(kEsc) && put("&l0E") && out_.put8(kEsc) && put_number("&l", lines, "F") && out_.put8(kEsc) && put("&k2G") &&
                out_.put8(kEsc) && put("&a0L") && out_.put8(kEsc) && put("*p0X") && out_.put8(kEsc) && put("*p0Y");
      if (!ok) return false;
      page_open = true;
      column = line = 0;
    }
    return true;
  };
  auto eject = [&]() {
    if (!page_open) return true;
    page_open = false;
    ++pages_;
    return put("\f");
  };
  auto newline = [&]() {
    column = 0;
    if (++line >= lines) return eject();
    return put("\r\n");
  };
  auto emit = [&](uint32_t c) {  // one character to the current position, wrapping
    if (column >= columns && !newline()) return false;
    if (!open_page()) return false;
    ++column;
    return out_.put8(static_cast<uint8_t>(c));
  };
  const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
  for (uint64_t i = 0; i < text.size(); ++i) {
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
    bool ok = true;
    if (c == '\n') {
      if (!page_open && !open_page()) return fail();
      ok = newline();
    } else if (c == '\r') {
      if (page_open) {
        column = 0;
        ok = put("\r");
      }
    } else if (c == '\f') {
      ok = eject();
    } else if (c == '\t') {
      do ok = emit(' '); while (ok && column % 8 != 0 && column < columns);
    } else if (c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xA0)) {
      ok = emit(c);
    }
    if (!ok) return fail();
  }
  return eject() ? true : fail();
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
