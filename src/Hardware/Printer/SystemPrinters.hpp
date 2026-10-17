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
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/Printer/IPrinter.hpp"
#include "Hardware/Printer/PrinterAlertListener.hpp"
#include "Hardware/Printer/PrinterDiscovery.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace network {
struct TlsConfig;
}

namespace hardware {

// A change to the set of printers (see SystemPrinters::poll_usb).
struct PrinterEvent {
  enum class Kind : uint8_t { Added, Removed };
  Kind kind = Kind::Added;
  uint32_t id = 0;                       // the printer's id, which stays the same for as long as it is there
  SharedPointer<IPrinter> printer;       // a removed printer is shut down and present() is false; it lives while anyone holds this
  uint32_t lost_jobs = 0;                // Removed: jobs that were still queued and did not print
};

// The printers the machine knows about. Network printers cannot be found by scanning buses, so they are added by whoever knows
// their address (a settings file, the user), USB ones are found on the bus - at start-up, and later as they are plugged in and pulled out
// (xHCI controllers); the first one in the list is the default.
//
// The list changes while the system runs, so a program that keeps a printer holds share() (not the pointer at() hands out, which is
// only good while the printer stays) and checks present().
class SystemPrinters {
 public:
  using Notifier = void (*)(void* context, StringView text);

  explicit SystemPrinters(allocators::IAllocator& allocator) : allocator_(allocator), printers_(allocator), pending_(allocator) {}
  ~SystemPrinters() { shutdown(); }

  bool add(UniquePointer<IPrinter> printer);
  // Adds every printer found on the USB bus (printer-class interfaces); returns how many. Also starts watching the bus for printers
  // plugged in or pulled out later (poll_usb()).
  uint32_t discover_usb(IUsbBus& usb);
  // Adds a printer object (make_discovered_printer, IPPS when `tls` is given and the printer offers it) for every printer `discovery` has found that is
  // not here yet (matched by IP address); returns how many.
  uint32_t add_discovered(network::INetworkStack& stack, const PrinterDiscovery& discovery, const network::TlsConfig* tls = nullptr);
  uint32_t count() const;
  IPrinter* at(uint32_t index);
  uint32_t id(uint32_t index) const;
  SharedPointer<IPrinter> share(uint32_t index) const;
  IPrinter* default_printer() { return at(0); }
  void set_default(uint32_t index);

  // Hot-plug: USB printers plugged in or pulled out since discover_usb(). One change per call (false when there is none); call it every few hundred
  // milliseconds from one thread. A new printer is already initialized and in the list (at the end); a removed one has been taken out and
  // shut down, and is freed once everyone has let go of the event's pointer. Only controllers that report hot-plug (xHCI) are watched.
  bool poll_usb(PrinterEvent* event);
  // poll_usb() and the delivery of the USB printers' jobs, every 10 ms (hot-plug every 250 ms), on a thread of its own ("printers"). Each change is
  // passed to `notifier` ("Brother HL-2070N: added", "...: removed, 2 jobs lost"). Network printers are not polled by it: whoever runs the
  // network stack polls those.
  bool start_service(Notifier notifier = nullptr, void* context = nullptr);
  void stop_service();

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
  struct Entry {
    uint32_t id = 0;
    bool usb = false;
    SharedPointer<IPrinter> printer;
  };

  bool adopt(SharedPointer<IPrinter> printer, bool usb, bool report);
  void sweep_removed();
  static void service_main(void* self);

  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;   // the list
  List<Entry> printers_;
  List<PrinterEvent> pending_;
  uint32_t next_id_ = 1;
  IUsbBus* usb_ = nullptr;
  UsbPortWatcher watcher_;
  UniquePointer<PrinterAlertListener> alerts_;
  AlertCallback alert_callback_ = nullptr;
  void* alert_context_ = nullptr;
  uint64_t alerts_handled_ = 0;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
};

}  // namespace hardware
