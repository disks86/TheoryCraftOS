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

#include "SystemPrintService.hpp"

using namespace hardware;
using desktop::PrinterListing;
using desktop::PrintOptions;

namespace {
// A job name is passed on as a C string.
void copy_name(StringView name, char (&out)[64]) {
  uint64_t n = name.size() < sizeof out - 1 ? name.size() : sizeof out - 1;
  for (uint64_t i = 0; i < n; ++i) out[i] = name[i];
  out[n] = 0;
}

bool fail(String* error, const char* text) {
  if (error) error->assign(text);
  return false;
}
}  // namespace

bool SystemPrintService::printer(uint32_t index, PrinterListing* out) {
  SharedPointer<IPrinter> p = printers_.share(index);
  if (!p) return false;
  const PrinterInfo& info = p->info();
  *out = PrinterListing();
  const char* name = p->name();
  uint32_t n = 0;
  while (name && name[n] && n + 1 < sizeof out->name) {
    out->name[n] = name[n];
    ++n;
  }
  out->state = p->state();
  out->present = p->present();
  out->color = info.color;
  out->duplex = info.duplex;
  out->pcl = info.supports(PrinterLanguage::Pcl);
  out->paper_sizes = info.paper_sizes;
  out->native_dpi = info.native_dpi;
  out->max_dpi = info.max_dpi;
  out->unprintable_mm10 = info.unprintable_mm10;
  out->pages_per_minute = info.pages_per_minute;
  return true;
}

bool SystemPrintService::settings_for(const PrinterInfo& info, const PrintOptions& options, const char* job_name, JobSettings* out, String* error) {
  if (!info.supports(PrinterLanguage::Pcl)) return fail(error, "This printer does not understand PCL.");
  if (info.paper_sizes && !info.supports(options.paper)) return fail(error, "This printer does not have that paper size.");
  *out = JobSettings();
  out->name = job_name;
  out->language = PrinterLanguage::Pcl;
  out->paper = options.paper;
  out->dpi = options.dpi > info.max_dpi ? info.max_dpi : options.dpi;
  out->copies = options.copies ? options.copies : 1;
  out->toner_save = options.toner_save;
  out->landscape = options.landscape;
  return true;
}

bool SystemPrintService::deliver(const SharedPointer<IPrinter>& printer, PrintJob& job, const char* job_name, String* error) {
  if (!job.finish() || job.failed()) return fail(error, "The job could not be built.");
  if (!printer->present()) return fail(error, "The printer is not connected.");
  if (!printer->submit(job.bytes(), job_name)) return fail(error, "The printer's queue is full.");
  return true;
}

bool SystemPrintService::print_text(uint32_t index, StringView text, StringView job_name, const PrintOptions& options, String* error) {
  SharedPointer<IPrinter> printer = printers_.share(index);
  if (!printer) return fail(error, "There is no such printer.");
  char name[64];
  copy_name(job_name, name);
  JobSettings settings;
  if (!settings_for(printer->info(), options, name, &settings, error)) return false;
  PrintJob job(allocator_, printer->info(), settings);
  if (!job.begin()) return fail(error, "This printer cannot print with those settings.");
  if (!job.add_text(text, options.first_page, options.last_page)) return fail(error, "The text could not be turned into a job.");
  if (job.pages() == 0) return fail(error, "There are no pages in that range.");
  return deliver(printer, job, name, error);
}

bool SystemPrintService::print_picture(uint32_t index, const graphics::Surface& picture, StringView job_name, const PrintOptions& options, String* error) {
  SharedPointer<IPrinter> printer = printers_.share(index);
  if (!printer) return fail(error, "There is no such printer.");
  char name[64];
  copy_name(job_name, name);
  JobSettings settings;
  if (!settings_for(printer->info(), options, name, &settings, error)) return false;
  PrintJob job(allocator_, printer->info(), settings);
  if (!job.begin()) return fail(error, "This printer cannot print with those settings.");
  PageBitmap page(allocator_);
  if (!page.create(job.page_width(), job.page_height())) return fail(error, "There is not enough memory for the page.");
  if (!render_surface(picture, &page, options.fit, options.halftone, allocator_)) return fail(error, "The picture could not be turned into dots.");
  if (!job.add_page(page)) return fail(error, "The page could not be turned into a job.");
  return deliver(printer, job, name, error);
}
