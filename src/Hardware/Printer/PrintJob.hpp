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
#include "Hardware/Printer/Halftone.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

// Builds the bytes of one print job for a printer that speaks PJL with PCL 5e (the language of nearly every laser printer) or
// PostScript: a PJL header that names the job and sets the resolution and toner saving, the page description, and the PJL
// trailer. The result goes to IPrinter::submit().
//
//   PrintJob job(allocator, printer.info(), settings);
//   job.begin();
//   job.add_page(bitmap);  or  job.add_text("Hello\n");   (PCL jobs)
//   job.finish();
//   printer.submit(job.bytes(), settings.name);
//
// A PCL job is made of pages: add_page() prints a bitmap (rows are run-length compressed with PackBits and blank rows are
// skipped, so a mostly white page is small) and add_text() prints text in a fixed-pitch font (10 characters per inch,
// 6 lines per inch, UTF-8 in, Latin-1 out, long lines wrapped, new pages as needed) starting on a fresh page. A PostScript job
// is one program given to add_postscript().
class PrintJob {
 public:
  PrintJob(allocators::IAllocator& allocator, const PrinterInfo& printer, const JobSettings& settings)
      : allocator_(allocator), printer_(printer), settings_(settings), out_(allocator) {}

  // Checks the settings against the printer (language, paper size, resolution) and writes the job header. False if the
  // printer cannot do what was asked or memory ran out.
  bool begin();
  bool add_page(const PageBitmap& page);
  bool add_text(StringView utf8);
  bool add_postscript(ConstByteView program);
  // Writes the trailer. After this bytes() is the whole job.
  bool finish();

  ConstByteView bytes() const { return out_.view(); }
  uint32_t pages() const { return pages_; }
  bool failed() const { return failed_; }

  // The size of a page of this job's bitmaps: the printable area of the paper at the job's resolution.
  uint32_t page_width() const;
  uint32_t page_height() const;

 private:
  bool put(const char* text);
  bool put_number(const char* before, uint32_t value, const char* after);
  bool page_header();
  bool fail() {
    failed_ = true;
    return false;
  }
  bool put_uel();

  allocators::IAllocator& allocator_;
  const PrinterInfo& printer_;
  JobSettings settings_;
  ByteBuffer out_;
  uint32_t pages_ = 0;
  bool begun_ = false, finished_ = false, failed_ = false;
};

}  // namespace hardware
