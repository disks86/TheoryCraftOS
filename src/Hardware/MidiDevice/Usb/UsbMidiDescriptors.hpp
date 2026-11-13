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
#include "Utility/ByteBuffer.hpp"

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

// The packets of a system exclusive message (F0 .. F7) on `cable`: three bytes each with CIN 4, the last one with CIN 5, 6 or 7 (one, two or three bytes).
// Calls put(const uint8_t packet[4]) for each; returns how many (0 when `message` is not one).
template <class F>
uint32_t encode_usb_midi_sysex(uint8_t cable, ConstByteView message, F&& put) {
  if (message.size() < 2 || message[0] != 0xF0 || message[message.size() - 1] != 0xF7) return 0;
  uint32_t packets = 0;
  for (uint64_t at = 0; at < message.size(); at += 3) {
    const uint64_t left = message.size() - at;
    const uint8_t count = left >= 3 ? 3 : static_cast<uint8_t>(left);
    const bool last = left <= 3;
    uint8_t packet[4] = {static_cast<uint8_t>(((cable & 0x0F) << 4) | (last ? 0x4 + count : 0x4)), 0, 0, 0};
    for (uint8_t k = 0; k < count; ++k) packet[1 + k] = message[at + k];
    put(static_cast<const uint8_t*>(packet));
    ++packets;
  }
  return packets;
}

// A device's system exclusive messages put back together from its packets (CIN 4: three bytes that start or go on with one; 5, 6, 7: the last one,
// two or three bytes; a packet of another kind in the middle of one breaks it). At most kMaxBytes (F0 and F7 counted); a longer one is dropped.
class UsbMidiSysexReader {
 public:
  static constexpr uint32_t kMaxBytes = 65536;
  explicit UsbMidiSysexReader(allocators::IAllocator& allocator) : bytes_(allocator) {}
  // One packet. True when it ended a message: *out is its bytes, F0 to F7 (good until the next call).
  bool feed(const uint8_t packet[4], ConstByteView* out);
  bool building() const { return building_; }

 private:
  ByteBuffer bytes_;
  bool building_ = false, too_long_ = false;
};
using UsbMidiSysexReceiver = void (*)(void* context, ConstByteView message);

// Calls receiver(context, status, data1, data2) for each channel message in the 4-byte packets of `data` (a trailing partial packet is ignored).
// Packets of other kinds - system exclusive, system messages, clock - are skipped. Returns how many messages there were.
using UsbMidiReceiver = void (*)(void* context, uint8_t status, uint8_t data1, uint8_t data2);
// With `sysex` (one per device: messages span calls), system exclusive messages go to sysex_receiver(context, message) too, counted as messages.
uint32_t decode_usb_midi_packets(ConstByteView data, UsbMidiReceiver receiver, void* context, UsbMidiSysexReader* sysex = nullptr,
                                 UsbMidiSysexReceiver sysex_receiver = nullptr);

}  // namespace hardware
