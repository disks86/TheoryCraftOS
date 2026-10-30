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

#include "Hardware/Keyboard/Usb/UsbHidConsumer.hpp"

namespace hardware {

namespace {
constexpr uint8_t kRequestGetDescriptor = 0x06;
constexpr uint8_t kDescriptorReport = 0x22;
constexpr uint8_t kClassInterfaceIn = 0x81;  // device-to-host, standard, interface
}  // namespace

bool UsbHidConsumer::initialize() {
  if (interface_.report_descriptor_length == 0) return false;
  if (!host_.set_configuration(port_, interface_.configuration_value)) return false;
  uint8_t descriptor[512];
  const uint16_t want = interface_.report_descriptor_length < sizeof(descriptor) ? interface_.report_descriptor_length : sizeof(descriptor);
  if (!host_.control_in(port_, kClassInterfaceIn, kRequestGetDescriptor, uint16_t{kDescriptorReport} << 8, interface_.number, ByteView(descriptor).first(want))) return false;
  hid::ReportMap map;
  if (!hid::parse_report_map(ConstByteView(descriptor).first(want), &map)) return false;
  const hid::InputReport* report = map.find_application(0x0C, 1);
  if (!report || !decoder_.init(*report) || !decoder_.valid()) return false;
  report_id_ = report->id;
  return host_.start_interrupt_in(port_, interface_.endpoint_address, interface_.max_packet, interface_.interval);
}

void UsbHidConsumer::apply(const hid::KeyboardState& state) {
  for (uint32_t i = 0; i < held_.key_count; ++i) {                    // released: held before, not now
    bool still = false;
    for (uint32_t k = 0; k < state.key_count; ++k) still = still || state.keys[k] == held_.keys[i];
    if (!still) push(held_.keys[i], false);
  }
  for (uint32_t k = 0; k < state.key_count; ++k) {                    // pressed: now, not before
    bool was = false;
    for (uint32_t i = 0; i < held_.key_count; ++i) was = was || state.keys[k] == held_.keys[i];
    if (!was) push(state.keys[k], true);
  }
  held_ = state;
}

void UsbHidConsumer::on_release_all() {
  for (uint32_t i = 0; i < held_.key_count; ++i) push(held_.keys[i], false);
  held_ = hid::KeyboardState();
}

void UsbHidConsumer::fetch() {
  uint8_t report[64];
  uint16_t length = 0;
  while (host_.poll_interrupt_in(port_, interface_.endpoint_address, ByteView(report), &length)) {
    if (length > sizeof(report)) length = sizeof(report);
    ConstByteView received = ConstByteView(report).first(length);
    if (report_id_ != 0) {
      if (received.size() == 0 || received[0] != report_id_) continue;        // (another report of the interface)
      received = received.subview(1, received.size() - 1);
    }
    hid::KeyboardState state;
    if (decoder_.decode(received, &state)) apply(state);
  }
}

}  // namespace hardware
