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

#include "Hardware/Printer/SystemPrinters.hpp"
#include "Hardware/Printer/Usb/UsbPrinter.hpp"

namespace hardware {

bool SystemPrinters::add(UniquePointer<IPrinter> printer) {
  if (!printer || !printer->initialize()) return false;
  return printers_.push_back(util::move(printer));
}

uint32_t SystemPrinters::discover_usb(IUsbBus& usb) {
  List<UniquePointer<IPrinter>> found(allocator_);
  UsbPrinter::discover(usb, allocator_, found);
  uint32_t added = 0;
  for (uint64_t i = 0; i < found.count(); ++i)
    if (printers_.push_back(util::move(found[i]))) ++added;
  return added;
}

void SystemPrinters::set_default(uint32_t index) {
  if (index == 0 || index >= printers_.count()) return;
  UniquePointer<IPrinter> chosen = util::move(printers_[index]);
  for (uint32_t i = index; i > 0; --i) printers_[i] = util::move(printers_[i - 1]);
  printers_[0] = util::move(chosen);
}

bool SystemPrinters::enable_alerts(network::INetworkStack& stack, AlertCallback callback, void* context) {
  if (!alerts_) alerts_ = UniquePointer<PrinterAlertListener>::make(allocator_, stack, allocator_);
  alert_callback_ = callback;
  alert_context_ = context;
  return alerts_ && alerts_->start();
}

void SystemPrinters::poll() {
  if (alerts_) {
    alerts_->poll();
    PrinterAlertEvent event;
    while (alerts_->next(&event)) {
      IPrinter* from = nullptr;
      for (uint64_t i = 0; i < printers_.count(); ++i)
        if (printers_[i]->ipv4_address() != 0 && printers_[i]->ipv4_address() == event.source.value()) from = printers_[i].get();
      if (from && !from->report_pending()) from->request_report();   // something changed: look again
      if (alert_callback_) alert_callback_(alert_context_, from, event);
      ++alerts_handled_;
    }
  }
  for (uint64_t i = 0; i < printers_.count(); ++i) printers_[i]->poll();
}

void SystemPrinters::shutdown() {
  alerts_.reset();
  for (uint64_t i = 0; i < printers_.count(); ++i) printers_[i]->shutdown();
  printers_.clear();
}

}  // namespace hardware
