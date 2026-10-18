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

// USB MIDI (the MIDIStreaming interface of the audio class, "USB Device Class Definition for MIDI Devices" 1.0): finding the interface and its
// bulk endpoints in a configuration descriptor, and the 4-byte event packets the endpoints carry.
//
// A packet is a header byte - the cable number (which of the device's virtual MIDI ports) in the high nibble and a code index number in the
// low one - followed by up to three MIDI bytes. The code for a channel message is the message's kind (8 note off ... E pitch bend).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

struct UsbMidiModel {
  uint8_t configuration = 0;            // bConfigurationValue
  uint8_t interface_number = 0;
  uint8_t alternate = 0;                // the alternate setting that has the endpoints (0 for nearly every device)
  uint8_t out_endpoint = 0, in_endpoint = 0;   // bulk endpoint addresses; 0: the device has none (IN has bit 7)
  uint16_t out_max_packet = 0, in_max_packet = 0;
  uint8_t out_cables = 1, in_cables = 1;
  // The device describes an element - a synthesizer, sampler or other sound maker - in its jack topology. A keyboard or a MIDI interface (DIN
  // sockets) has none: what is sent to it makes no sound by itself.
  bool has_synth = false;

  bool valid() const { return out_endpoint != 0 || in_endpoint != 0; }
};

// Looks for a MIDIStreaming interface (audio class 1, subclass 3) with bulk endpoints in a configuration descriptor. False if there is none.
bool parse_usb_midi_configuration(ConstByteView configuration, UsbMidiModel* out);

// Builds the packet for a channel message (status 0x80-0xEF) on `cable`; false for anything else.
bool encode_usb_midi_packet(uint8_t cable, uint8_t status, uint8_t data1, uint8_t data2, uint8_t out[4]);

// Calls receiver(context, status, data1, data2) for each channel message in the 4-byte packets of `data` (a trailing partial packet is ignored).
// Packets of other kinds - system exclusive, system messages, clock - are skipped. Returns how many messages there were.
using UsbMidiReceiver = void (*)(void* context, uint8_t status, uint8_t data1, uint8_t data2);
uint32_t decode_usb_midi_packets(ConstByteView data, UsbMidiReceiver receiver, void* context);

}  // namespace hardware
