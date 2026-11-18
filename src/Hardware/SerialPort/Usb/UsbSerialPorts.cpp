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

#include "Hardware/SerialPort/Usb/UsbSerialPorts.hpp"
#include "Hardware/Platform/Framework/FrameworkPlatform.hpp"
#include "Hardware/SerialPort/Usb/UsbCdcAcm.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace hardware {

void UsbSerialPorts::notify(const char* name, const char* what) {
  if (!notifier_) return;
  String text(allocator_);
  text.assign(name);
  text.append(": ");
  text.append(what);
  notifier_(notifier_context_, text.view());
}

bool UsbSerialPorts::take_up(IUsbHostController& host, uint32_t port) {
  UniquePointer<UsbCdcAcm> acm = UsbCdcAcm::open_device(host, port, allocator_);
  if (!acm) return false;
  if (const framework::Module* m = framework::find_module(acm->vendor_id(), acm->product_id())) acm->set_name(m->name);
  const char* name = acm->name();
  SharedPointer<ISerialPort> shared = SharedPointer<ISerialPort>::from(util::move(acm));
  if (!shared || !ports_.add(shared)) return false;
  ++added_;
  notify(name, "added");
  return true;
}

uint32_t UsbSerialPorts::initialize() {
  uint32_t n = 0;
  if (!usb_) return 0;
  for (uint32_t c = 0; c < usb_->controller_count(); ++c) {
    IUsbHostController* host = usb_->host_controller(c);
    for (uint32_t p = 0; host && p < host->port_count(); ++p) n += take_up(*host, p) ? 1 : 0;
  }
  watcher_.snapshot();
  return n;
}

uint32_t UsbSerialPorts::scan() {
  uint32_t changes = ports_.remove_gone();
  watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) { changes += take_up(host, port) ? 1 : 0; });
  return changes;
}

void UsbSerialPorts::service_main(void* self) {
  auto* s = static_cast<UsbSerialPorts*>(self);
  while (!__atomic_load_n(&s->stop_, __ATOMIC_ACQUIRE)) {
    s->scan();
    kernel::sleep_us(250000);
  }
}

bool UsbSerialPorts::start_service() {
  if (service_) return true;
  if (!kernel::running()) return false;
  __atomic_store_n(&stop_, false, __ATOMIC_RELEASE);
  service_ = kernel::spawn(&UsbSerialPorts::service_main, this, "usb-serial");
  return service_ != nullptr;
}

void UsbSerialPorts::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

}  // namespace hardware
