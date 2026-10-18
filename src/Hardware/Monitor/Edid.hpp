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

// EDID (Extended Display Identification Data, VESA E-EDID 1.4) and its CTA-861 extension blocks: what a monitor tells about itself over the
// cable - who made it, what it is called, how big it is, and the video modes it can show, with its preferred one first among them. Monitors are
// untrusted devices, so every field is range-checked and nothing reads outside the data it was given.
//
// parse_edid() takes the whole blob (the 128-byte base block followed by any extension blocks). Blocks the parser does not know (DisplayID, ...)
// are skipped. A base block with a wrong checksum is still read - plenty of real monitors have one - and says so in `checksum_ok`.
#pragma once
#include "Hardware/Monitor/MonitorTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

struct EdidTiming {
  uint32_t width = 0, height = 0;
  uint32_t refresh_millihz = 0;
  uint32_t pixel_clock_khz = 0;   // 0 for modes that come from a table (established / standard timings, CTA video codes)
  bool interlaced = false;
  bool preferred = false;
};

struct EdidInfo {
  static constexpr uint32_t kMaxTimings = 48;

  bool valid = false;
  bool checksum_ok = false;
  uint8_t version = 0, revision = 0;
  char manufacturer[4] = {};        // the three-letter PNP id, "DEL"
  uint16_t product_code = 0;
  uint32_t serial_number = 0;       // the number in the header (0: none)
  uint8_t week = 0;                 // of manufacture (0: unknown)
  uint16_t year = 0;                // 0: unknown
  char name[14] = {};               // the monitor name descriptor, "DELL U2720Q"
  char serial_text[14] = {};        // the serial number descriptor
  bool digital = false;
  uint8_t bits_per_color = 0;       // 6, 8, 10, 12, 14, 16 (digital inputs of EDID 1.4); 0: not said
  uint16_t width_mm = 0, height_mm = 0;   // the picture's size (0: not said)
  uint8_t gamma_x100 = 0;           // 220 for 2.2 (0: not said)
  bool srgb = false;
  // Limits from the range limits descriptor (0: not given).
  uint16_t min_vertical_hz = 0, max_vertical_hz = 0, min_horizontal_khz = 0, max_horizontal_khz = 0, max_pixel_clock_mhz = 0;
  uint8_t extension_blocks = 0;
  bool has_audio = false;           // CTA basic audio
  bool hdmi = false;                // a CTA vendor block with the HDMI id
  EdidTiming timings[kMaxTimings];
  uint32_t timing_count = 0;
  int32_t preferred = -1;           // index into timings of the preferred (native) mode, -1: none

  // The preferred timing, or the one with the most pixels (the highest refresh rate among those) when none is marked. False if there are none.
  bool best_timing(EdidTiming* out) const;
  // The index of the timing for `width` x `height` with the refresh rate nearest to `refresh_millihz` (any if 0); -1 if there is none of that size.
  int32_t find_timing(uint32_t width, uint32_t height, uint32_t refresh_millihz = 0) const;
  // The diagonal in tenths of an inch; 0 if the size is not known. And the pixel density of the native mode; 0 if either is unknown.
  uint32_t diagonal_inches_x10() const;
  uint32_t dpi() const;
};

// True if `data` starts like an EDID (the 8-byte header) and holds at least the base block.
bool looks_like_edid(ConstByteView data);
// Reads EDID bytes into *out. False (with out->valid false) if the data is not an EDID.
bool parse_edid(ConstByteView data, EdidInfo* out);

// The maker of a monitor from its PNP id: "Dell", "Samsung", ...; the id itself when it is not one of the well known ones.
const char* edid_vendor_name(const char* pnp_id);
// A name for a monitor in lists: the name descriptor, or "<Vendor> <product code>" without one.
void edid_display_name(const EdidInfo& info, char* out, uint32_t capacity);

}  // namespace hardware
