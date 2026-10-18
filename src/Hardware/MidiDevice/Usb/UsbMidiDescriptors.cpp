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

#include "Hardware/MidiDevice/Usb/UsbMidiDescriptors.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassAudio = 1, kSubclassMidiStreaming = 3;
constexpr uint8_t kCsInterface = 0x24, kCsEndpoint = 0x25;
constexpr uint8_t kSubtypeElement = 4, kSubtypeGeneral = 1;
}  // namespace

bool parse_usb_midi_configuration(ConstByteView configuration, UsbMidiModel* out) {
  *out = UsbMidiModel();
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  uint8_t configuration_value = 0;
  bool in_midi_interface = false;   // the interface being walked is a MIDIStreaming one
  bool chosen = false;              // the interface the model describes was found
  bool in_chosen = false;           // the interface (alternate setting) being walked is that one
  bool synth = false;
  int last_endpoint_direction = 0;  // 1: the last endpoint taken was OUT, 2: IN, 0: none
  while (walker.next(&d)) {
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorConfiguration) {
      UsbConfigurationInfo info;
      if (parse_usb_configuration(d, &info)) configuration_value = info.value;
    } else if (type == kUsbDescriptorInterface) {
      UsbInterfaceInfo info;
      if (!parse_usb_interface(d, &info)) continue;
      in_midi_interface = info.device_class == kClassAudio && info.subclass == kSubclassMidiStreaming;
      if (in_midi_interface && !chosen && info.endpoint_count > 0) {   // the first alternate setting that has endpoints
        chosen = true;
        out->interface_number = info.number;
        out->alternate = info.alternate_setting;
        out->configuration = configuration_value;
        in_chosen = true;
      } else {
        in_chosen = false;
      }
      last_endpoint_direction = 0;
    } else if (type == kCsInterface) {
      if (in_midi_interface && d.size() >= 3 && d[2] == kSubtypeElement) synth = true;
    } else if (type == kUsbDescriptorEndpoint) {
      last_endpoint_direction = 0;
      if (!in_chosen) continue;
      UsbEndpointInfo ep;
      if (!parse_usb_endpoint(d, &ep) || ep.transfer_type() != 2) continue;   // bulk only
      if (ep.is_in()) {
        if (out->in_endpoint) continue;   // (the first of each direction is used)
        out->in_endpoint = ep.address;
        out->in_max_packet = ep.max_packet;
        last_endpoint_direction = 2;
      } else {
        if (out->out_endpoint) continue;
        out->out_endpoint = ep.address;
        out->out_max_packet = ep.max_packet;
        last_endpoint_direction = 1;
      }
    } else if (type == kCsEndpoint) {
      if (d.size() >= 4 && d[2] == kSubtypeGeneral && d[3] > 0 && d[3] <= 16) {   // bNumEmbMIDIJack: how many cables
        if (last_endpoint_direction == 1) out->out_cables = d[3];
        else if (last_endpoint_direction == 2) out->in_cables = d[3];
      }
    }
  }
  out->has_synth = chosen && synth;
  return chosen && out->valid();
}

bool encode_usb_midi_packet(uint8_t cable, uint8_t status, uint8_t data1, uint8_t data2, uint8_t out[4]) {
  if (status < 0x80 || status >= 0xF0) return false;
  out[0] = static_cast<uint8_t>(((cable & 0x0F) << 4) | (status >> 4));
  out[1] = status;
  out[2] = data1 & 0x7F;
  const uint8_t kind = status & 0xF0;
  out[3] = kind == 0xC0 || kind == 0xD0 ? 0 : data2 & 0x7F;
  return true;
}

uint32_t decode_usb_midi_packets(ConstByteView data, UsbMidiReceiver receiver, void* context) {
  uint32_t count = 0;
  for (uint64_t at = 0; at + 4 <= data.size(); at += 4) {
    const uint8_t cin = data[at] & 0x0F;
    if (cin < 0x8 || cin > 0xE) continue;   // not a channel message
    const uint8_t status = data[at + 1];
    if ((status >> 4) != cin) continue;      // the packet contradicts itself
    receiver(context, status, data[at + 2] & 0x7F, data[at + 3] & 0x7F);
    ++count;
  }
  return count;
}

}  // namespace hardware
