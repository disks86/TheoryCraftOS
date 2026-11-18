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

#include "Hardware/GameController/Usb/UsbGameController.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/SonyProtocol.hpp"
#include "Hardware/GameController/SwitchProtocol.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassHid = 3;
constexpr uint8_t kTransferInterrupt = 3;
constexpr uint8_t kRequestGetDescriptor = 0x06, kDescriptorReport = 0x22;
constexpr uint8_t kStandardInterfaceIn = 0x81;            // device-to-host, standard, interface
constexpr uint8_t kClassInterfaceOut = 0x21;              // host-to-device, class, interface
constexpr uint8_t kHidSetReport = 0x09, kHidSetIdle = 0x0A;
constexpr uint16_t kReportTypeOutput = 2;
constexpr uint32_t kMaxReportDescriptor = 1024;

void copy_name(gamepad::Info& info, const char* name) {
  uint32_t i = 0;
  for (; name && name[i] && i + 1 < sizeof(info.name); ++i) info.name[i] = name[i];
  info.name[i] = 0;
}

// Reads a HID interface's report descriptor into `buffer`; the part read, empty on failure.
ConstByteView read_report_descriptor(IUsbHostController& host, uint32_t port, const UsbGameControllerProbe& p, ByteView buffer) {
  if (p.report_descriptor_length == 0) return ConstByteView();
  const uint16_t want = static_cast<uint16_t>(p.report_descriptor_length < buffer.size() ? p.report_descriptor_length : buffer.size());
  if (!host.control_in(port, kStandardInterfaceIn, kRequestGetDescriptor, uint16_t{kDescriptorReport} << 8, p.interface_number, buffer.first(want)))
    return ConstByteView();
  return ConstByteView(buffer.first(want));
}

// One interface of the configuration with its interrupt endpoints.
struct Candidate {
  UsbInterfaceInfo interface;
  uint8_t in = 0, out = 0;
  uint16_t in_max = 0, out_max = 0;
  uint8_t in_interval = 0, out_interval = 0;
  uint16_t report_length = 0;
};

// Picks the interface the controller is reached through: the first Xbox (vendor-specific) one, else the first HID one, each with an IN endpoint.
bool pick_interface(ConstByteView configuration, UsbGameControllerProbe* out, bool* is_hid) {
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbConfigurationInfo config;
  if (!walker.next(&d) || !parse_usb_configuration(d, &config)) return false;
  Candidate current, xbox, hid;
  bool have_current = false, have_xbox = false, have_hid = false;
  gamepad::Protocol xbox_protocol = gamepad::Protocol::None;
  auto close = [&]() {
    if (!have_current || current.in == 0) return;
    const gamepad::Protocol p = gamepad::protocol_for_interface(current.interface.device_class, current.interface.subclass, current.interface.protocol);
    if (p != gamepad::Protocol::None && !have_xbox) { xbox = current; have_xbox = true; xbox_protocol = p; }
    if (current.interface.device_class == kClassHid && !have_hid) { hid = current; have_hid = true; }
  };
  while (walker.next(&d)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorInterface && parse_usb_interface(d, &interface)) {
      close();
      current = Candidate{};
      current.interface = interface;
      have_current = interface.alternate_setting == 0;
    } else if (!have_current) {
      continue;
    } else if (type == kUsbDescriptorHid && d.size() >= 9) {
      current.report_length = static_cast<uint16_t>(d[7] | (d[8] << 8));
    } else if (type == kUsbDescriptorEndpoint && parse_usb_endpoint(d, &endpoint) && endpoint.transfer_type() == kTransferInterrupt) {
      if (endpoint.is_in() && current.in == 0) {
        current.in = endpoint.address;
        current.in_max = endpoint.max_packet;
        current.in_interval = endpoint.interval;
      } else if (!endpoint.is_in() && current.out == 0) {
        current.out = endpoint.address;
        current.out_max = endpoint.max_packet;
        current.out_interval = endpoint.interval;
      }
    }
  }
  close();
  const Candidate* chosen = have_xbox ? &xbox : have_hid ? &hid : nullptr;
  if (!chosen) return false;
  out->configuration = config.value;
  out->interface_number = chosen->interface.number;
  out->in_endpoint = chosen->in;
  out->in_max_packet = chosen->in_max;
  out->in_interval = chosen->in_interval;
  out->out_endpoint = chosen->out;
  out->out_max_packet = chosen->out_max;
  out->out_interval = chosen->out_interval;
  out->report_descriptor_length = chosen->report_length;
  out->protocol = have_xbox ? xbox_protocol : gamepad::Protocol::None;
  *is_hid = !have_xbox;
  return true;
}
}  // namespace

bool probe_usb_game_controller(IUsbHostController& host, uint32_t port, UsbGameControllerProbe* out) {
  *out = UsbGameControllerProbe{};
  UsbDeviceDescriptor device;
  if (!host.device_descriptor(port, &device)) return false;
  uint8_t config[512];
  uint16_t length = 0;
  if (!host.configuration_descriptor(port, ByteView(config), &length)) return false;
  UsbGameControllerProbe p;
  bool is_hid = false;
  if (!pick_interface(ConstByteView(config).at_most(length), &p, &is_hid)) return false;
  p.vendor_id = device.vendor_id;
  p.product_id = device.product_id;
  const gamepad::CatalogEntry* entry = gamepad::find_controller(device.vendor_id, device.product_id);
  if (entry) p.name = entry->name;
  if (is_hid) {
    if (entry && (entry->protocol == gamepad::Protocol::DualShock4 || entry->protocol == gamepad::Protocol::DualSense ||
                  entry->protocol == gamepad::Protocol::SwitchPro)) {
      p.protocol = entry->protocol;
    } else {
      // A HID device is a game controller if its report descriptor says so.
      if (!host.set_configuration(port, p.configuration)) return false;
      uint8_t descriptor[kMaxReportDescriptor];
      const ConstByteView report = read_report_descriptor(host, port, p, ByteView(descriptor));
      hid::ReportMap map;
      if (report.empty() || !hid::parse_report_map(report, &map) || !hid::find_gamepad_report(map)) return false;
      p.protocol = gamepad::Protocol::Hid;
    }
  }
  if (p.protocol == gamepad::Protocol::None) return false;
  *out = p;
  return true;
}

UniquePointer<UsbGameController> UsbGameController::open(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator) {
  UsbGameControllerProbe probe;
  if (!probe_usb_game_controller(host, port, &probe)) return UniquePointer<UsbGameController>();
  UniquePointer<UsbGameController> controller = UniquePointer<UsbGameController>::make(allocator, host, port, probe);
  if (!controller || !controller->initialize()) return UniquePointer<UsbGameController>();
  return controller;
}

UsbGameController::UsbGameController(IUsbHostController& host, uint32_t port, const UsbGameControllerProbe& probe)
    : host_(host), port_(port), probe_(probe), generation_(host.port_generation(port)) {
  info_.protocol = probe.protocol;
  info_.vendor_id = probe.vendor_id;
  info_.product_id = probe.product_id;
  info_.capabilities = gamepad::protocol_capabilities(probe.protocol);
  const char* fallback = "USB game controller";
  switch (probe.protocol) {
    case gamepad::Protocol::Xbox360: fallback = "Xbox 360 compatible controller"; break;
    case gamepad::Protocol::XboxOne: fallback = "Xbox One compatible controller"; break;
    case gamepad::Protocol::DualShock4: fallback = "DualShock 4"; break;
    case gamepad::Protocol::DualSense: fallback = "DualSense"; break;
    case gamepad::Protocol::SwitchPro: fallback = "Switch Pro Controller"; break;
    default: break;
  }
  copy_name(info_, probe.name ? probe.name : fallback);
}

bool UsbGameController::initialize() {
  if (!host_.set_configuration(port_, probe_.configuration)) return false;
  if (probe_.protocol == gamepad::Protocol::Hid) {
    uint8_t descriptor[kMaxReportDescriptor];
    const ConstByteView report = read_report_descriptor(host_, port_, probe_, ByteView(descriptor));
    hid::ReportMap map;
    if (report.empty() || !hid::parse_report_map(report, &map)) return false;
    const hid::InputReport* r = hid::find_gamepad_report(map);
    if (!r || !hid_.init(*r)) return false;
    hid_report_id_ = r->id;
    info_.capabilities |= hid_.capabilities();
  }
  if (probe_.protocol == gamepad::Protocol::Hid || probe_.protocol == gamepad::Protocol::DualShock4 || probe_.protocol == gamepad::Protocol::DualSense ||
      probe_.protocol == gamepad::Protocol::SwitchPro)
    (void)host_.control_out(port_, kClassInterfaceOut, kHidSetIdle, 0, probe_.interface_number);   // report on change only (some pads refuse: fine)
  if (probe_.out_endpoint)
    out_ready_ = host_.start_interrupt_out(port_, probe_.out_endpoint, probe_.out_max_packet, probe_.out_interval);
  if (probe_.protocol == gamepad::Protocol::XboxOne && !out_ready_) return false;   // (it would never say anything)
  if (!host_.start_interrupt_in(port_, probe_.in_endpoint, probe_.in_max_packet, probe_.in_interval)) return false;

  uint8_t message[64];
  switch (probe_.protocol) {
    case gamepad::Protocol::XboxOne: {
      const uint32_t n = xbox::encode_gip_power_on(sequence_++, ByteView(message));
      if (!send(ConstByteView(message).first(n))) return false;
      const uint32_t led = xbox::encode_gip_led(sequence_++, 0x14, ByteView(message));
      (void)send(ConstByteView(message).first(led));
      break;
    }
    case gamepad::Protocol::SwitchPro:
      if (!out_ready_) return false;     // (the handshake goes out through the OUT endpoint; without it the pad says nothing)
      handshake_step_ = 0;
      handshake_waited_ = 0;
      advance_handshake(true);
      break;
    default:
      break;
  }
  state_ = gamepad::State();
  return true;
}

void UsbGameController::shutdown() {
  if (!present()) return;
  gamepad::Output quiet;
  quiet.player = output_.player;
  (void)set_output(quiet);     // stop the motors of a pad left plugged in
}

bool UsbGameController::send(ConstByteView message) {
  if (!out_ready_ || message.empty()) return false;
  return host_.interrupt_out(port_, probe_.out_endpoint, message);
}

bool UsbGameController::send_report(ConstByteView report) {
  if (report.empty()) return false;
  if (out_ready_) return send(report);
  return host_.control_out_data(port_, kClassInterfaceOut, kHidSetReport, static_cast<uint16_t>((kReportTypeOutput << 8) | report[0]),
                                probe_.interface_number, report);
}

// The Switch Pro's start: each step is sent once the pad answered the last (0x81 ...) or did not within a few polls.
void UsbGameController::advance_handshake(bool answered) {
  if (handshake_step_ >= kHandshakeEnd) return;
  if (!answered && ++handshake_waited_ < kHandshakePolls) return;
  handshake_waited_ = 0;
  uint8_t report[64];
  uint32_t n = 0;
  if (handshake_step_ < nintendo::kHandshakeSteps) n = nintendo::encode_handshake(handshake_step_, ByteView(report));
  else if (handshake_step_ == nintendo::kHandshakeSteps) n = nintendo::encode_full_report_mode(sequence_++, ByteView(report));
  else n = nintendo::encode_player_leds(sequence_++, output_.player ? output_.player : 1, ByteView(report));
  ++handshake_step_;
  (void)send(ConstByteView(report).first(n));
}

bool UsbGameController::decode(ConstByteView report) {
  switch (probe_.protocol) {
    case gamepad::Protocol::Xbox360:
      return xbox::decode_360(report, &state_);
    case gamepad::Protocol::XboxOne: {
      const xbox::GipResult r = xbox::decode_gip(report, &state_);
      if (r.needs_ack) {
        uint8_t ack[16];
        const uint32_t n = xbox::encode_gip_ack(r, ByteView(ack));
        (void)send(ConstByteView(ack).first(n));
      }
      return r.event == xbox::GipEvent::Input || r.event == xbox::GipEvent::Guide;
    }
    case gamepad::Protocol::DualShock4:
      return sony::decode_dualshock4(report, &state_);
    case gamepad::Protocol::DualSense:
      return sony::decode_dualsense(report, &state_);
    case gamepad::Protocol::SwitchPro:
      if (report.size() >= 1 && report[0] == 0x81) {     // the answer to a handshake report
        advance_handshake(true);
        return false;
      }
      if (report.size() >= 1 && report[0] == nintendo::kFullReport && !handshake_done()) handshake_step_ = handshake_step_ < 4 ? 4 : handshake_step_;
      return nintendo::decode_full_report(report, &state_);
    case gamepad::Protocol::Hid: {
      ConstByteView data = report;
      if (hid_report_id_ != 0) {
        if (data.empty() || data[0] != hid_report_id_) return false;
        data = data.dropped(1);
      }
      return hid_.decode(data, &state_);
    }
    case gamepad::Protocol::None:
      break;
  }
  return false;
}

bool UsbGameController::poll() {
  if (!present()) return false;
  bool changed = false;
  uint8_t report[64];
  uint16_t length = 0;
  for (uint32_t budget = 0; budget < 32 && host_.poll_interrupt_in(port_, probe_.in_endpoint, ByteView(report), &length); ++budget) {
    if (length > sizeof(report)) length = sizeof(report);
    const gamepad::State before = state_;
    if (decode(ConstByteView(report).first(length))) {
      changed = changed || before.buttons != state_.buttons || before.left_x != state_.left_x || before.left_y != state_.left_y ||
                before.right_x != state_.right_x || before.right_y != state_.right_y || before.left_trigger != state_.left_trigger ||
                before.right_trigger != state_.right_trigger;
    }
  }
  if (probe_.protocol == gamepad::Protocol::SwitchPro) advance_handshake(false);
  return changed;
}

bool UsbGameController::set_output(const gamepad::Output& output) {
  if (!present()) return false;
  const gamepad::Output before = output_;
  output_ = output;
  uint8_t message[64];
  switch (probe_.protocol) {
    case gamepad::Protocol::Xbox360: {
      bool ok = send(ConstByteView(message).first(xbox::encode_360_rumble(output.low_frequency, output.high_frequency, ByteView(message))));
      if (output.player != before.player) ok = send(ConstByteView(message).first(xbox::encode_360_led(output.player, ByteView(message)))) && ok;
      return ok;
    }
    case gamepad::Protocol::XboxOne:
      return send(ConstByteView(message).first(xbox::encode_gip_rumble(sequence_++, output.low_frequency, output.high_frequency, ByteView(message))));
    case gamepad::Protocol::DualShock4:
      return send_report(ConstByteView(message).first(sony::encode_dualshock4_output(output, ByteView(message))));
    case gamepad::Protocol::DualSense:
      return send_report(ConstByteView(message).first(sony::encode_dualsense_output(output, ByteView(message))));
    case gamepad::Protocol::SwitchPro:
      if (!handshake_done() || output.player == before.player) return output.low_frequency == 0 && output.high_frequency == 0;   // (no rumble)
      return send(ConstByteView(message).first(nintendo::encode_player_leds(sequence_++, output.player, ByteView(message))));
    case gamepad::Protocol::Hid:
    case gamepad::Protocol::None:
      break;
  }
  return false;
}

}  // namespace hardware
