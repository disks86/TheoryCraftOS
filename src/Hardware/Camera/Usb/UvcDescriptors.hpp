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

// What a USB video class (UVC 1.0 / 1.1 / 1.5) camera's configuration says about it: the processing unit's picture controls and the video
// streaming interface with its formats, frames, frame rates and the alternate settings that carry the data. The descriptors come from the
// device, so every length is checked and a broken one just ends up not being understood.
constexpr uint8_t kUsbClassVideo = 0x0E, kUsbVideoSubclassControl = 1, kUsbVideoSubclassStreaming = 2;

enum class UvcFormatKind : uint8_t { Mjpeg, Yuy2, Uyvy, Nv12, I420 };

struct UvcFrame {
  static constexpr uint32_t kMaxIntervals = 8;
  uint8_t index = 0;
  uint16_t width = 0, height = 0;
  uint32_t max_frame_size = 0;         // dwMaxVideoFrameBufferSize
  uint32_t default_interval = 0;       // 100 ns units
  // Discrete: `interval_count` values, in the order given. Continuous (count 0): minimum, maximum and step.
  uint8_t interval_count = 0;
  uint32_t intervals[kMaxIntervals] = {};
  uint32_t interval_min = 0, interval_max = 0, interval_step = 0;
};

struct UvcFormat {
  static constexpr uint32_t kMaxFrames = 16;
  uint8_t index = 0;
  UvcFormatKind kind = UvcFormatKind::Mjpeg;
  UvcFrame frames[kMaxFrames];
  uint8_t frame_count = 0;
};

// An alternate setting of the streaming interface that carries isochronous data.
struct UvcAlternate {
  uint8_t alternate = 0;
  uint8_t endpoint = 0;
  uint16_t max_packet = 0;     // bytes per service interval (bits 10:0 of wMaxPacketSize)
  uint8_t interval = 0;
};

struct UvcStream {
  static constexpr uint32_t kMaxFormats = 8, kMaxAlternates = 16;
  uint8_t interface = 0;
  UvcFormat formats[kMaxFormats];
  uint8_t format_count = 0;
  UvcAlternate alternates[kMaxAlternates];
  uint8_t alternate_count = 0;
  bool bulk = false;           // streams over a bulk endpoint (not supported)
  bool high_bandwidth = false; // some alternate settings need more than one transaction per interval (not supported, skipped)
};

struct UvcDevice {
  uint16_t uvc_version = 0;    // bcdUVC
  uint8_t configuration = 0;
  uint8_t control_interface = 0;
  bool have_control = false;
  uint8_t processing_unit = 0;       // 0: none
  uint16_t processing_controls = 0;  // bmControls of the processing unit (bit 0 brightness, 1 contrast, 3 saturation, 4 sharpness)
  bool camera_terminal = false;
  UvcStream stream;            // the first streaming interface with formats
  bool have_stream = false;
};

// Reads a whole configuration descriptor. False if it has no video control interface or no usable streaming interface.
bool parse_uvc_configuration(ConstByteView configuration, UvcDevice* out);

// Length of the probe / commit control for a UVC version.
uint32_t uvc_probe_length(uint16_t uvc_version);

}  // namespace hardware
