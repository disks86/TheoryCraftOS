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

namespace hardware {

// How a driver finds the USB devices that were plugged in after start-up, without taking events away from the other drivers: it remembers the
// generation of every root port as it last looked (IUsbHostController::port_generation(), which changes whenever a device on the port is
// enumerated or released) and reports the ports whose generation moved and that hold a device now. A device that was replaced between two
// looks is therefore reported as new, and a driver's own record of the old one (its generation) tells it that one is gone.
class UsbPortWatcher {
 public:
  static constexpr uint32_t kMaxControllers = 8, kMaxPorts = 32;

  UsbPortWatcher() = default;
  explicit UsbPortWatcher(IUsbBus* bus) { attach(bus); }

  // Starts watching `bus` (may be null: nothing is ever reported). Devices present now count as seen.
  void attach(IUsbBus* bus) {
    bus_ = bus;
    snapshot();
  }
  // Treats everything present now as seen. Call it once the devices that were there at start-up have been taken over.
  void snapshot() {
    if (!bus_) return;
    for (uint32_t c = 0; c < bus_->controller_count() && c < kMaxControllers; ++c) {
      IUsbHostController* host = bus_->host_controller(c);
      for (uint32_t p = 0; host && p < host->port_count() && p < kMaxPorts; ++p) seen_[c][p] = host->port_generation(p);
    }
  }

  // Services the bus's hot-plug, then calls found(controller, host, port) for each port with a device that is new since the last scan.
  template <class F>
  void scan(F&& found) {
    if (!bus_) return;
    bus_->service_hotplug();
    for (uint32_t c = 0; c < bus_->controller_count() && c < kMaxControllers; ++c) {
      IUsbHostController* host = bus_->host_controller(c);
      for (uint32_t p = 0; host && p < host->port_count() && p < kMaxPorts; ++p) {
        const uint32_t generation = host->port_generation(p);
        if (generation == seen_[c][p]) continue;
        seen_[c][p] = generation;
        UsbDeviceDescriptor descriptor;
        if (host->device_descriptor(p, &descriptor)) found(c, *host, p);   // (a port that is empty now has only lost its device)
      }
    }
  }

 private:
  IUsbBus* bus_ = nullptr;
  uint32_t seen_[kMaxControllers][kMaxPorts] = {};
};

}  // namespace hardware
