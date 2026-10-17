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
#include <stdint.h>
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"

namespace hardware {

// What a USB audio device's configuration says about it (USB Audio Class 1.0 and 2.0): its entities (terminals, feature units, clocks) and its audio streaming
// interfaces with the formats of their alternate settings. The descriptors come from the device, so every length is checked and a broken one just ends up
// not being understood.
constexpr uint8_t kUsbClassAudio = 1, kUsbAudioSubclassControl = 1, kUsbAudioSubclassStreaming = 2;
constexpr uint8_t kUsbCsInterface = 0x24, kUsbCsEndpoint = 0x25;

// Entity kinds (the descriptor subtypes of the control interface)
constexpr uint8_t kUacInputTerminal = 2, kUacOutputTerminal = 3, kUacMixer = 4, kUacSelector = 5, kUacFeatureUnit = 6, kUacClockSource = 0x0A, kUacClockSelector = 0x0B;
constexpr uint16_t kUacTerminalUsbStreaming = 0x0101;

struct UacEntity {
  uint8_t id = 0;
  uint8_t kind = 0;
  uint8_t source = 0;         // the entity feeding this one (the first, for a mixer or selector); 0 for none
  uint16_t terminal_type = 0;
  uint8_t clock = 0;          // UAC2 terminals: the clock entity that times them
  uint8_t channels = 0;
  // feature unit: which controls it has for the master channel (0) and the first two channels, and whether the host may set them
  bool mute[3] = {}, volume[3] = {};
  // clock source: can the host set the rate? (a clock selector's `source` is its first input)
  bool clock_programmable = false;
};

struct UacFormat {
  uint8_t alternate = 0;
  uint8_t channels = 0;
  uint8_t subslot = 0;         // bytes per sample
  uint8_t bits = 0;
  bool pcm = false;
  bool rate_48000 = false;     // UAC1: the list says so. UAC2: see rate_unknown
  bool rate_unknown = false;   // UAC2: the clock has to be asked
  uint8_t endpoint = 0;        // the data endpoint (isochronous)
  uint16_t max_packet = 0;
  uint8_t interval = 0;
  uint8_t sync = 0;            // 0 none, 1 asynchronous, 2 adaptive, 3 synchronous
  bool rate_control = false;   // UAC1: the endpoint takes a sampling frequency
  uint8_t feedback_endpoint = 0;   // asynchronous sources: where the device tells how fast it really runs
  uint16_t feedback_max_packet = 0;
  uint8_t feedback_interval = 0;
};

struct UacStream {
  static constexpr uint32_t kMaxFormats = 6;
  uint8_t interface = 0;
  bool input = false;          // the device sends audio to the host (a microphone)
  uint8_t terminal_link = 0;
  UacFormat formats[kMaxFormats];
  uint8_t format_count = 0;
};

struct UacDevice {
  static constexpr uint32_t kMaxEntities = 32, kMaxStreams = 4;
  uint8_t version = 0;         // 1 or 2; 0: no audio control interface
  uint8_t configuration = 0;
  uint8_t control_interface = 0;
  UacEntity entities[kMaxEntities];
  uint8_t entity_count = 0;
  UacStream streams[kMaxStreams];
  uint8_t stream_count = 0;

  const UacEntity* find(uint8_t id) const {
    for (uint32_t i = 0; i < entity_count; ++i)
      if (entities[i].id == id) return &entities[i];
    return nullptr;
  }
};

// Reads a whole configuration descriptor. False if it has no audio control interface of a version we know or no streaming interface.
bool parse_uac_configuration(ConstByteView configuration, UacDevice* out);

// The feature unit (volume, mute) on the way between the host and the stream's terminal, or nullptr.
const UacEntity* uac_feature_unit(const UacDevice& device, const UacStream& stream);
// UAC2: the clock source that times the stream, or nullptr (also for UAC1).
const UacEntity* uac_clock_source(const UacDevice& device, const UacStream& stream);

}  // namespace hardware
