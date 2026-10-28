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
#include "Utility/ArrayView.hpp"

namespace hardware {

// A Short Audio Descriptor of a monitor (CEA-861 / CTA-861 audio data block, three bytes), as the DRM helper's struct cea_sad holds it (drm_edid_to_sad).
struct Sad {
  uint8_t format = 0;       // byte 0 bits 6:3: the audio coding type (1 PCM, 2 AC-3, 3 MPEG-1, 4 MP3, 5 MPEG-2, 6 AAC LC, 7 DTS, 8 ATRAC, 9 DSD, 10 E-AC-3, 11 DTS-HD, 12 MLP, 13 DST, 14 WMA Pro)
  uint8_t channels = 0;     // byte 0 bits 2:0: the channel count minus one
  uint8_t freq = 0;         // byte 1 bits 6:0: the sample rates (bit 0 32 kHz, 1 44.1, 2 48, 3 88.2, 4 96, 5 176.4, 6 192)
  uint8_t byte2 = 0;        // byte 2: for PCM the sample sizes, for the compressed formats the maximum bit rate / 8 kHz or the format dependent value
};

// What the audio endpoint needs from a monitor's EDID: its audio capabilities, speaker layout and the lip sync latency, found in the CTA extension blocks (tag 0x02, revision 3 and up)
// as the DRM helper reads them (drm_edid_to_sad, drm_edid_to_speaker_allocation, drm_parse_hdmi_vsdb_audio). The EDID is untrusted bytes: every block, data block and length is checked.
//
// Written from memory of the DRM helper (drm_edid.c was not at hand). One difference from the older kernels (which read only the first CTA extension block): all of them are read, as the
// current ones do.
class EdidAudio {
 public:
  static constexpr uint32_t kMaxSads = 32;
  struct Info {
    uint32_t sad_count = 0;                // the Short Audio Descriptors of every audio data block, in order (at most kMaxSads)
    Sad sads[kMaxSads];
    uint32_t speaker_count = 0;            // the speaker allocation data block (the first with the three byte payload it must have): its payload length, 3, or 0 if there is none
    uint8_t speaker_allocation = 0;        // its first byte: the speaker layout (bit 0 front left/right, 1 LFE, 2 front center, 3 rear left/right, 4 rear center, 5 front left/right center, 6 rear left/right center)
    bool latency_present[2] = {};          // the HDMI vendor specific data block's latency fields: [0] progressive, [1] interlaced
    uint8_t video_latency[2] = {};         // each (value - 1) * 2 ms
    uint8_t audio_latency[2] = {};
  };
  static void parse(ConstByteView edid, Info* out);
};

}  // namespace hardware
