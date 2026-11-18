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

// The USB serial ports (CDC-ACM) of the machine, those present at start-up and those plugged in later: each becomes a port of
// SystemSerialPorts, which moves its bytes, and is taken out again when it is pulled out. A known product (the Framework Laptop 16's LED
// matrix module) is given its name. scan() looks for changes; the serial service calls it every quarter second (start_service()).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/SerialPort/SystemSerialPorts.hpp"
#include "Utility/StringView.hpp"

namespace kernel { struct Thread; }

namespace hardware {

class UsbSerialPorts {
 public:
  UsbSerialPorts(IUsbBus* usb, SystemSerialPorts& ports, allocators::IAllocator& allocator) : usb_(usb), ports_(ports), allocator_(allocator), watcher_(usb) {}
  ~UsbSerialPorts() { stop_service(); }
  UsbSerialPorts(const UsbSerialPorts&) = delete;
  UsbSerialPorts& operator=(const UsbSerialPorts&) = delete;

  using Notifier = void (*)(void* context, StringView text);
  void set_notifier(Notifier notifier, void* context) { notifier_ = notifier; notifier_context_ = context; }

  // Takes up the ports present now; returns how many.
  uint32_t initialize();
  // Takes up ports plugged in since the last look and drops those pulled out; returns how many came or went. One thread at a time.
  uint32_t scan();
  [[nodiscard]] bool start_service();     // scan() every 250 ms ("usb-serial")
  void stop_service();
  uint32_t added() const { return added_; }

 private:
  bool take_up(IUsbHostController& host, uint32_t port);
  void notify(const char* name, const char* what);
  static void service_main(void* self);

  IUsbBus* usb_;
  SystemSerialPorts& ports_;
  allocators::IAllocator& allocator_;
  UsbPortWatcher watcher_;
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
  uint32_t added_ = 0;
};

}  // namespace hardware
