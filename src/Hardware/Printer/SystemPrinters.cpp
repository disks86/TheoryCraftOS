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
#include "Hardware/Printer/PrinterFactory.hpp"
#include "Hardware/Printer/Usb/UsbPrinter.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

bool SystemPrinters::adopt(SharedPointer<IPrinter> printer, bool usb, bool report) {
  if (!printer) return false;
  Entry entry;
  entry.id = next_id_++;
  entry.usb = usb;
  entry.printer = printer;
  PrinterEvent event;
  event.kind = PrinterEvent::Kind::Added;
  event.id = entry.id;
  event.printer = util::move(printer);
  kernel::ScopedMutex guard(mutex_);
  if (!printers_.push_back(util::move(entry))) return false;
  if (report) pending_.push_back(util::move(event));
  return true;
}

bool SystemPrinters::add(UniquePointer<IPrinter> printer) {
  if (!printer || !printer->initialize()) return false;
  return adopt(SharedPointer<IPrinter>::from(util::move(printer)), false, false);
}

uint32_t SystemPrinters::discover_usb(IUsbBus& usb) {
  List<UniquePointer<IPrinter>> found(allocator_);
  UsbPrinter::discover(usb, allocator_, found);
  uint32_t added = 0;
  for (uint64_t i = 0; i < found.count(); ++i)
    if (adopt(SharedPointer<IPrinter>::from(util::move(found[i])), true, false)) ++added;
  usb_ = &usb;
  watcher_.attach(&usb);   // what is plugged in now has been taken over; later ports are new
  return added;
}

uint32_t SystemPrinters::count() const {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(printers_.count());
}

IPrinter* SystemPrinters::at(uint32_t index) {
  kernel::ScopedMutex guard(mutex_);
  return index < printers_.count() ? printers_[index].printer.get() : nullptr;
}

uint32_t SystemPrinters::id(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < printers_.count() ? printers_[index].id : 0;
}

SharedPointer<IPrinter> SystemPrinters::share(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < printers_.count() ? printers_[index].printer : SharedPointer<IPrinter>();
}

void SystemPrinters::set_default(uint32_t index) {
  kernel::ScopedMutex guard(mutex_);
  if (index == 0 || index >= printers_.count()) return;
  Entry chosen = printers_[index];
  for (uint32_t i = index; i > 0; --i) printers_[i] = printers_[i - 1];
  printers_[0] = chosen;
}

bool SystemPrinters::enable_alerts(network::INetworkStack& stack, AlertCallback callback, void* context) {
  if (!alerts_) alerts_ = UniquePointer<PrinterAlertListener>::make(allocator_, stack, allocator_);
  alert_callback_ = callback;
  alert_context_ = context;
  return alerts_ && alerts_->start();
}

uint32_t SystemPrinters::add_discovered(network::INetworkStack& stack, const PrinterDiscovery& discovery, const network::TlsConfig* tls) {
  uint32_t added = 0;
  for (uint32_t i = 0; i < discovery.count(); ++i) {
    const DiscoveredPrinter& found = discovery.at(i);
    bool known = false;
    for (uint32_t k = 0; k < count(); ++k) {
      SharedPointer<IPrinter> existing = share(k);
      if (existing && existing->ipv4_address() == found.address.value()) known = true;
    }
    if (known) continue;
    UniquePointer<IPrinter> printer = make_discovered_printer(stack, allocator_, found, tls);
    if (printer && add(util::move(printer))) ++added;
  }
  return added;
}

void SystemPrinters::poll() {
  if (alerts_) {
    alerts_->poll();
    PrinterAlertEvent event;
    while (alerts_->next(&event)) {
      SharedPointer<IPrinter> from;
      {
        kernel::ScopedMutex guard(mutex_);
        for (uint64_t i = 0; i < printers_.count(); ++i)
          if (printers_[i].printer->ipv4_address() != 0 && printers_[i].printer->ipv4_address() == event.source.value()) from = printers_[i].printer;
      }
      if (from && !from->report_pending()) from->request_report();   // something changed: look again
      if (alert_callback_) alert_callback_(alert_context_, from.get(), event);
      ++alerts_handled_;
    }
  }
  List<SharedPointer<IPrinter>> snapshot(allocator_);   // (printers are polled outside the lock: a USB one sends up to 16 KiB per call)
  {
    kernel::ScopedMutex guard(mutex_);
    for (const Entry& e : printers_.view()) snapshot.push_back(e.printer);
  }
  for (SharedPointer<IPrinter>& p : snapshot.view()) p->poll();
}

// Takes every USB printer that is no longer present out of the list: shut down first (which waits for a poll() running on another thread), then reported.
void SystemPrinters::sweep_removed() {
  for (uint64_t i = 0;;) {
    SharedPointer<IPrinter> gone;
    uint32_t id = 0;
    {
      kernel::ScopedMutex guard(mutex_);
      while (i < printers_.count() && (!printers_[i].usb || printers_[i].printer->present())) ++i;
      if (i >= printers_.count()) return;
      gone = printers_[i].printer;
      id = printers_[i].id;
      printers_.remove_at(i);
    }
    PrinterEvent event;
    event.kind = PrinterEvent::Kind::Removed;
    event.id = id;
    event.lost_jobs = gone->pending_jobs();
    gone->shutdown();
    event.printer = util::move(gone);
    kernel::ScopedMutex guard(mutex_);
    pending_.push_back(util::move(event));
  }
}

bool SystemPrinters::poll_usb(PrinterEvent* event) {
  for (int rounds = 0; rounds < 64; ++rounds) {
    {
      kernel::ScopedMutex guard(mutex_);
      if (pending_.count() > 0) {
        *event = util::move(pending_[0]);
        pending_.remove_at(0);
        return true;
      }
    }
    if (!usb_) return false;
    watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) {
      sweep_removed();
      List<UniquePointer<IPrinter>> found(allocator_);
      UsbPrinter::discover_port(host, port, allocator_, found);
      for (uint64_t i = 0; i < found.count(); ++i) adopt(SharedPointer<IPrinter>::from(util::move(found[i])), true, true);
    });
    sweep_removed();   // (a printer that was pulled out and nothing came: no port reports it, but the printer knows)
    kernel::ScopedMutex guard(mutex_);
    if (pending_.count() == 0) return false;
  }
  return false;
}

bool SystemPrinters::start_service(Notifier notifier, void* context) {
  if (service_) return true;
  notifier_ = notifier;
  notifier_context_ = context;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "printers");
  return service_ != nullptr;
}

void SystemPrinters::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemPrinters::service_main(void* self) {
  SystemPrinters* printers = static_cast<SystemPrinters*>(self);
  for (uint32_t tick = 0; !__atomic_load_n(&printers->stop_, __ATOMIC_ACQUIRE); ++tick) {
    if (tick % 25 == 0) {
      PrinterEvent event;
      while (printers->poll_usb(&event)) {
        if (!printers->notifier_) continue;
        FixedString<192> text;
        text.assign(event.printer->name());
        if (event.kind == PrinterEvent::Kind::Added) {
          text.append(": added");
        } else {
          text.append(": removed");
          if (event.lost_jobs > 0) {
            text.append(", ");
            text.append_uint(event.lost_jobs);
            text.append(event.lost_jobs == 1 ? " job lost" : " jobs lost");
          }
        }
        printers->notifier_(printers->notifier_context_, text.view());
      }
    }
    List<SharedPointer<IPrinter>> usb(printers->allocator_);
    {
      kernel::ScopedMutex guard(printers->mutex_);
      for (const Entry& e : printers->printers_.view())
        if (e.usb) usb.push_back(e.printer);
    }
    for (SharedPointer<IPrinter>& p : usb.view()) p->poll();
    kernel::sleep_us(10000);
  }
}

void SystemPrinters::shutdown() {
  stop_service();
  alerts_.reset();
  kernel::ScopedMutex guard(mutex_);
  pending_.clear();
  for (uint64_t i = 0; i < printers_.count(); ++i) printers_[i].printer->shutdown();
  printers_.clear();
}

}  // namespace hardware
