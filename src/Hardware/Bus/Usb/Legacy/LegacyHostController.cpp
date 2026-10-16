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

#include "Hardware/Bus/Usb/Legacy/LegacyHostController.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

bool LegacyHostController::device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const {
  const DeviceState* d = state(port);
  if (!d || !out || !d->present) return false;
  *out = d->descriptor;
  return true;
}

bool LegacyHostController::configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const {
  const DeviceState* d = state(port);
  if (!d || !d->present || !d->config_length || buffer.empty()) return false;
  buffer.copy_from(ConstByteView(d->config).first(d->config_length).at_most(buffer.size()));  // as much as fits
  if (length) *length = d->config_length;
  return true;
}

bool LegacyHostController::set_configuration(uint32_t port, uint8_t configuration) {
  kernel::ScopedMutex lock_guard(lock_);
  DeviceState* d = state(port);
  if (!d || !d->present) return false;
  if (d->configuration == configuration) return true;
  if (!control_transfer(port, 0x00, 9, configuration, 0, ByteView(), false)) return false;
  d->configuration = configuration;
  return true;
}

bool LegacyHostController::control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                       uint16_t index) {
  kernel::ScopedMutex lock_guard(lock_);
  const DeviceState* d = state(port);
  if (!d || !d->present) return false;
  return control_transfer(port, request_type, request, value, index, ByteView(), false);
}

bool LegacyHostController::control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                      uint16_t index, ByteView buffer) {
  kernel::ScopedMutex lock_guard(lock_);
  const DeviceState* d = state(port);
  if (!d || !d->present || buffer.empty() || buffer.size() > 4096) return false;
  return control_transfer(port, request_type, request, value, index, buffer, true);
}

bool LegacyHostController::enumerate_device(uint32_t port, bool low_speed) {
  DeviceState* d = state(port);
  if (!d) return false;
  *d = DeviceState{};
  d->low_speed = low_speed;
  if (!open_control_pipe(port)) return false;

  auto fail = [&] {
    close_device(port);
    *d = DeviceState{};
    return false;
  };
  uint8_t buffer[18];
  // The first 8 bytes of the device descriptor tell the real packet size of the default pipe.
  if (!control_transfer(port, 0x80, 6, 0x0100, 0, ByteView(buffer).first(8), true)) return fail();
  uint16_t max_packet = buffer[7];
  if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64) max_packet = 8;
  update_control_pipe(port, 0, max_packet);
  d->max_packet0 = max_packet;

  const uint8_t address = static_cast<uint8_t>(port + 1);
  if (!control_transfer(port, 0x00, 5, address, 0, ByteView(), false)) return fail();
  delay_us(10000);  // the device needs a moment to adopt its address
  update_control_pipe(port, address, max_packet);
  d->address = address;

  if (!control_transfer(port, 0x80, 6, 0x0100, 0, ByteView(buffer).first(sizeof(UsbDeviceDescriptor)), true)) return fail();
  bytes_of(d->descriptor).copy_from(ConstByteView(buffer).first(sizeof(UsbDeviceDescriptor)));
  d->present = true;

  // Cache the first configuration for class drivers; a device without a readable
  // one is still enumerated, just not usable by them.
  uint8_t config_storage[sizeof(d->config)];
  const ByteView config(config_storage);
  if (control_transfer(port, 0x80, 6, 0x0200, 0, config.first(9), true)) {
    const uint16_t total = config[2] | (config[3] << 8);
    if (total >= 9 && total <= config.size() && control_transfer(port, 0x80, 6, 0x0200, 0, config.first(total), true)) {
      ByteView(d->config).copy_from(ConstByteView(config).first(total));
      d->config_length = total;
    }
  }
  return true;
}

}  // namespace hardware
