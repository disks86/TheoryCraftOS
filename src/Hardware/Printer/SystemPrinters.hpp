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
#include "Hardware/Printer/IPrinter.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The printers the machine knows about. Network printers cannot be found by scanning buses, so they are added by whoever knows
// their address (a settings file, the user); the first one added is the default.
class SystemPrinters {
 public:
  explicit SystemPrinters(allocators::IAllocator& allocator) : printers_(allocator) {}
  ~SystemPrinters() { shutdown(); }

  bool add(UniquePointer<IPrinter> printer);
  uint32_t count() const { return static_cast<uint32_t>(printers_.count()); }
  IPrinter* at(uint32_t index) { return index < printers_.count() ? printers_[index].get() : nullptr; }
  IPrinter* default_printer() { return at(0); }
  void set_default(uint32_t index);
  // Lets every printer deliver its queue.
  void poll();
  void shutdown();

 private:
  List<UniquePointer<IPrinter>> printers_;
};

}  // namespace hardware
