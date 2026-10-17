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
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Printer/IPrinter.hpp"
#include "Hardware/Printer/PrinterAlertListener.hpp"
#include "Hardware/Printer/PrinterDiscovery.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The printers the machine knows about. Network printers cannot be found by scanning buses, so they are added by whoever knows
// their address (a settings file, the user), USB ones are found on the bus; the first one added is the default.
class SystemPrinters {
 public:
  explicit SystemPrinters(allocators::IAllocator& allocator) : allocator_(allocator), printers_(allocator) {}
  ~SystemPrinters() { shutdown(); }

  bool add(UniquePointer<IPrinter> printer);
  // Adds every printer found on the USB bus (printer-class interfaces); returns how many.
  uint32_t discover_usb(IUsbBus& usb);
  // Adds a printer object (make_discovered_printer) for every printer `discovery` has found that is not here yet (matched by IP address); returns how many.
  uint32_t add_discovered(network::INetworkStack& stack, const PrinterDiscovery& discovery);
  uint32_t count() const { return static_cast<uint32_t>(printers_.count()); }
  IPrinter* at(uint32_t index) { return index < printers_.count() ? printers_[index].get() : nullptr; }
  IPrinter* default_printer() { return at(0); }
  void set_default(uint32_t index);
  // Starts listening for the printers' SNMP traps (UDP 162). Each event is passed to `callback` (if any) with the printer it came from
  // (matched by IP address; null for an unknown sender), and that printer is asked for a fresh report. Events are handled in poll().
  using AlertCallback = void (*)(void* context, IPrinter* printer, const PrinterAlertEvent& event);
  bool enable_alerts(network::INetworkStack& stack, AlertCallback callback = nullptr, void* context = nullptr);
  PrinterAlertListener* alerts() { return alerts_.get(); }
  uint64_t alerts_handled() const { return alerts_handled_; }
  // Lets every printer deliver its queue.
  void poll();
  void shutdown();

 private:
  allocators::IAllocator& allocator_;
  List<UniquePointer<IPrinter>> printers_;
  UniquePointer<PrinterAlertListener> alerts_;
  AlertCallback alert_callback_ = nullptr;
  void* alert_context_ = nullptr;
  uint64_t alerts_handled_ = 0;
};

}  // namespace hardware
