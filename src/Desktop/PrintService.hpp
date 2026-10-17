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

// What the desktop programs know of printing: a list of printers to choose from and two ways to send them a job. The real
// service (SystemPrintService) sits on the system's printers; tests use a fake one.
#pragma once
#include "Graphics/GraphicsTypes.hpp"
#include "Hardware/Printer/Halftone.hpp"
#include "Hardware/Printer/IPrinter.hpp"
#include "Hardware/Printer/PrinterTypes.hpp"
#include "Utility/String.hpp"

namespace desktop {

// How a printer is shown in the print dialog.
struct PrinterListing {
  char name[64] = {};
  hardware::PrinterState state = hardware::PrinterState::Ready;
  bool present = true;
  bool color = false, duplex = false;
  bool pcl = true;                 // the language the jobs are written in
  uint16_t paper_sizes = 0;        // bit per hardware::PaperSize
  uint16_t native_dpi = 600, max_dpi = 600;
  uint8_t unprintable_mm10 = 42;   // the margin the engine cannot mark, on every edge
  uint8_t pages_per_minute = 0;
};

// What the user chose in the print dialog.
struct PrintOptions {
  hardware::PaperSize paper = hardware::PaperSize::A4;
  bool landscape = false;
  uint16_t dpi = 600;
  uint16_t copies = 1;
  // The pages of a text to print, counted from 1 (0 for the last means to the end). Pictures are one page.
  uint32_t first_page = 1, last_page = 0;
  bool toner_save = false;
  hardware::Halftone halftone = hardware::Halftone::ErrorDiffusion;  // pictures
  hardware::Fit fit = hardware::Fit::Contain;                        // pictures
};

class IPrintService {
 public:
  // The printers, in a stable order for as long as none comes or goes.
  virtual uint32_t printer_count() = 0;
  virtual bool printer(uint32_t index, PrinterListing* out) = 0;
  virtual uint32_t default_printer() = 0;
  // Builds a job and hands it to the printer's queue (it is delivered in the background). False, with the reason in `*error`, if that
  // could not be done: the printer cannot do what was asked, is gone, or its queue is full.
  virtual bool print_text(uint32_t printer, StringView text, StringView job_name, const PrintOptions& options, String* error) = 0;
  // `picture` is 0xAARRGGBB pixels; it is scaled to the page and halftoned.
  virtual bool print_picture(uint32_t printer, const graphics::Surface& picture, StringView job_name, const PrintOptions& options, String* error) = 0;

 protected:
  constexpr IPrintService() = default;
  ~IPrintService() = default;
};

}  // namespace desktop
