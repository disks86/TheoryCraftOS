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

#include "Hardware/GameController/Usb/UsbXbox360Wireless.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

namespace hardware {

namespace {
constexpr uint8_t kTransferInterrupt = 3;
}  // namespace

uint32_t find_xbox360_wireless_channels(ConstByteView configuration, ArrayView<UsbWirelessChannel> out) {
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbConfigurationInfo config;
  if (!walker.next(&d) || !parse_usb_configuration(d, &config)) return 0;
  uint32_t found = 0;
  UsbWirelessChannel current;
  bool in_channel = false;
  auto close = [&]() {
    if (in_channel && current.in_endpoint && current.out_endpoint && found < out.size()) out[found++] = current;
    in_channel = false;
  };
  while (walker.next(&d)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorInterface && parse_usb_interface(d, &interface)) {
      close();
      in_channel = interface.alternate_setting == 0 && interface.device_class == xbox::kInterfaceClass && interface.subclass == xbox::kXbox360Subclass &&
                   interface.protocol == xbox::kXbox360WirelessProtocol;
      current = UsbWirelessChannel{};
      current.configuration = config.value;
      current.interface_number = interface.number;
    } else if (in_channel && type == kUsbDescriptorEndpoint && parse_usb_endpoint(d, &endpoint) && endpoint.transfer_type() == kTransferInterrupt) {
      if (endpoint.is_in() && !current.in_endpoint) {
        current.in_endpoint = endpoint.address;
        current.in_max_packet = endpoint.max_packet;
        current.in_interval = endpoint.interval;
      } else if (!endpoint.is_in() && !current.out_endpoint) {
        current.out_endpoint = endpoint.address;
        current.out_max_packet = endpoint.max_packet;
        current.out_interval = endpoint.interval;
      }
    }
  }
  close();
  return found;
}

UsbXbox360WirelessPad::UsbXbox360WirelessPad(IUsbHostController& host, uint32_t port, const UsbWirelessChannel& channel, uint16_t vendor, uint16_t product)
    : host_(host), port_(port), channel_(channel), generation_(host.port_generation(port)) {
  info_.protocol = gamepad::Protocol::Xbox360;
  info_.vendor_id = vendor;
  info_.product_id = product;
  info_.capabilities = gamepad::protocol_capabilities(gamepad::Protocol::Xbox360);
  const gamepad::CatalogEntry* entry = gamepad::find_controller(vendor, product);
  const char* name = entry ? entry->name : "Xbox 360 Wireless Controller";
  uint32_t i = 0;
  for (; name[i] && i + 1 < sizeof(info_.name); ++i) info_.name[i] = name[i];
  info_.name[i] = 0;
}

bool UsbXbox360WirelessPad::initialize() {
  if (!host_.set_configuration(port_, channel_.configuration)) return false;
  if (!host_.start_interrupt_out(port_, channel_.out_endpoint, channel_.out_max_packet, channel_.out_interval)) return false;
  if (!host_.start_interrupt_in(port_, channel_.in_endpoint, channel_.in_max_packet, channel_.in_interval)) return false;
  uint8_t message[16];
  return send(ConstByteView(message).first(xbox::encode_360w_inquiry(ByteView(message))));
}

void UsbXbox360WirelessPad::shutdown() {
  if (!connected()) return;
  gamepad::Output quiet;
  quiet.player = output_.player;
  (void)set_output(quiet);
}

bool UsbXbox360WirelessPad::send(ConstByteView message) {
  return !message.empty() && host_.interrupt_out(port_, channel_.out_endpoint, message);
}

bool UsbXbox360WirelessPad::poll() {
  if (!present()) return false;
  bool changed = false;
  uint8_t report[64];
  uint16_t length = 0;
  for (uint32_t budget = 0; budget < 32 && host_.poll_interrupt_in(port_, channel_.in_endpoint, ByteView(report), &length); ++budget) {
    if (length > sizeof(report)) length = sizeof(report);
    const gamepad::State before = state_;
    switch (xbox::decode_360w(ConstByteView(report).first(length), &state_)) {
      case xbox::WirelessEvent::Connected:
        if (!connected_) {
          connected_ = true;
          changed = true;
        }
        break;
      case xbox::WirelessEvent::Disconnected:
        changed = changed || connected_;
        connected_ = false;
        output_ = gamepad::Output();     // (a pad that pairs again is shown its new number)
        break;
      case xbox::WirelessEvent::Input:
        connected_ = true;               // (input from a pad whose pairing message was missed)
        changed = changed || before.buttons != state_.buttons || before.left_x != state_.left_x || before.left_y != state_.left_y ||
                  before.right_x != state_.right_x || before.right_y != state_.right_y || before.left_trigger != state_.left_trigger ||
                  before.right_trigger != state_.right_trigger;
        break;
      case xbox::WirelessEvent::None:
        break;
    }
  }
  return changed;
}

bool UsbXbox360WirelessPad::set_output(const gamepad::Output& output) {
  if (!connected()) return false;
  const gamepad::Output before = output_;
  output_ = output;
  uint8_t message[16];
  bool ok = send(ConstByteView(message).first(xbox::encode_360w_rumble(output.low_frequency, output.high_frequency, ByteView(message))));
  if (output.player != before.player) ok = send(ConstByteView(message).first(xbox::encode_360w_led(output.player, ByteView(message)))) && ok;
  return ok;
}

}  // namespace hardware
