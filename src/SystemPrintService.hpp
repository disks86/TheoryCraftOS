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

// The print service on the system's printers: lists them for the print dialog and turns text and pictures into print jobs (PJL with PCL
// 5e, see Hardware/Printer/PrintJob.hpp) that go to the chosen printer's queue.
#pragma once
#include "Desktop/PrintService.hpp"
#include "Hardware/Printer/PrintJob.hpp"
#include "Hardware/Printer/SystemPrinters.hpp"

class SystemPrintService final : public desktop::IPrintService {
 public:
  // The printers must outlive this object.
  SystemPrintService(allocators::IAllocator& allocator, hardware::SystemPrinters& printers) : allocator_(allocator), printers_(printers) {}

  uint32_t printer_count() override { return printers_.count(); }
  bool printer(uint32_t index, desktop::PrinterListing* out) override;
  uint32_t default_printer() override { return 0; }
  bool print_text(uint32_t printer, StringView text, StringView job_name, const desktop::PrintOptions& options, String* error) override;
  bool print_picture(uint32_t printer, const graphics::Surface& picture, StringView job_name, const desktop::PrintOptions& options, String* error) override;

 private:
  // The settings of a job on `info` for these options, or false (with the reason in `*error`) if the printer cannot do them.
  bool settings_for(const hardware::PrinterInfo& info, const desktop::PrintOptions& options, const char* job_name, hardware::JobSettings* out, String* error);
  bool deliver(const SharedPointer<hardware::IPrinter>& printer, hardware::PrintJob& job, const char* job_name, String* error);

  allocators::IAllocator& allocator_;
  hardware::SystemPrinters& printers_;
};
