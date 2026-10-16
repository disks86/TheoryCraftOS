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

#include "Hardware/Mouse/Usb/UsbHidMouse.hpp"

namespace hardware {

namespace {
constexpr uint8_t kRequestGetDescriptor = 0x06;
constexpr uint8_t kDescriptorReport = 0x22;
constexpr uint8_t kClassInterfaceIn = 0x81;  // device-to-host, standard, interface

// The fixed boot-protocol mouse report: buttons, dx, dy (+ optional wheel).
HidPointerLayout boot_layout() {
  HidPointerLayout l;
  l.valid = true;
  auto field = [](uint32_t byte, uint8_t bits) {
    HidField f;
    f.valid = true;
    f.bit_offset = byte * 8;
    f.bit_size = bits;
    f.is_signed = true;
    f.relative = true;
    f.logical_min = -127;
    f.logical_max = 127;
    return f;
  };
  l.x = field(1, 8);
  l.y = field(2, 8);
  for (int b = 0; b < 3; ++b) {
    l.buttons[b].valid = true;
    l.buttons[b].bit_offset = b;
    l.buttons[b].bit_size = 1;
  }
  return l;
}
}  // namespace

bool UsbHidMouse::initialize() {
  if (!host_.set_configuration(port_, interface_.configuration_value)) return false;

  // Prefer what the device says about its own reports.
  uint8_t descriptor[512];
  const uint16_t want = interface_.report_descriptor_length < sizeof(descriptor)
                            ? interface_.report_descriptor_length : sizeof(descriptor);
  bool parsed = false;
  if (want && host_.control_in(port_, kClassInterfaceIn, kRequestGetDescriptor,
                               uint16_t{kDescriptorReport} << 8, interface_.number, descriptor, want))
    parsed = parse_hid_pointer(descriptor, want, &layout_);
  if (!parsed) {
    if (!interface_.is_boot_mouse()) return false;
    layout_ = boot_layout();
    host_.control_out(port_, 0x21, 0x0B, 0, interface_.number);  // SET_PROTOCOL: boot
  }
  return host_.start_interrupt_in(port_, interface_.endpoint_address, interface_.max_packet,
                                  interface_.interval);
}

void UsbHidMouse::fetch() {
  uint8_t report[64];
  uint16_t length = 0;
  while (host_.poll_interrupt_in(port_, interface_.endpoint_address, report, sizeof(report), &length)) {
    if (length > sizeof(report)) length = sizeof(report);
    const uint8_t id = layout_.report_id;
    MouseEvent e{};
    for (int b = 0; b < 3; ++b)
      if (hid_extract(report, length, layout_.buttons[b], id)) e.buttons |= 1u << b;

    const int32_t x = hid_extract(report, length, layout_.x, id);
    const int32_t y = hid_extract(report, length, layout_.y, id);
    if (layout_.x.relative) {
      e.dx = x;
      e.dy = y;  // HID +y is down, like ours
      e.absolute = false;
    } else {
      e.absolute = true;
      e.x = x < 0 ? 0 : static_cast<uint32_t>(x);
      e.y = y < 0 ? 0 : static_cast<uint32_t>(y);
    }
    e.wheel = hid_extract(report, length, layout_.wheel, id);  // HID +wheel is away from the user
    push(e);
  }
}

}  // namespace hardware
