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

void SystemPrinters::poll() {
  for (uint64_t i = 0; i < printers_.count(); ++i) printers_[i]->poll();
}

void SystemPrinters::shutdown() {
  for (uint64_t i = 0; i < printers_.count(); ++i) printers_[i]->shutdown();
  printers_.clear();
}

}  // namespace hardware
