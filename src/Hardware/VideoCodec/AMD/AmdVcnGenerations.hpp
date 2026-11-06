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
#include "Hardware/VideoCodec/AMD/AmdIpDiscovery.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"

namespace hardware {

// The generations of AMD's Video Core Next engine, as the amdgpu driver groups the IP versions into ip blocks (vcn_v1_0, vcn_v2_0, ...). Every Ryzen APU
// from Raven Ridge on carries one: VCN 1 (Raven, Picasso, Dali), VCN 2.2 (Renoir, Lucienne, Cezanne, Barcelo), VCN 3 (Van Gogh, Rembrandt, Mendocino,
// Raphael, Granite Ridge), VCN 4 (Phoenix, Hawk Point, Strix, Krackan, Strix Halo).
enum class AmdVcnGeneration : uint8_t {
  Unknown,
  Vcn1,     // 1.0.x: separate decode and encode rings
  Vcn2,     // 2.0.x, 2.2.0
  Vcn2_5,   // 2.5.0, 2.6.0: two instances (Arcturus, Aldebaran)
  Vcn3,     // 3.0.x, 3.1.x
  Vcn4,     // 4.0.0 .. 4.0.6: one unified queue for decode and encode
  Vcn5,     // 5.0.x, 5.3.0
};

// What the engine of one VCN IP version does, as amdgpu reports it to user space (soc15.c, nv.c, soc21.c, soc24.c: the *_video_codecs_*_array tables,
// first instance; a second instance may lack AV1). Sizes are the largest frame, in pixels.
struct AmdVcnCodecLimit {
  VideoCodec codec;
  uint16_t max_width, max_height;
};
constexpr uint32_t kAmdVcnMaxCodecs = 8;

struct AmdVcnInfo {
  AmdVcnGeneration generation = AmdVcnGeneration::Unknown;
  AmdIpVersion version;
  const char* firmware = nullptr;     // the microcode file amdgpu loads ("vcn_4_0_5.bin" and the like, in /firmware), or nullptr when unknown
  bool jpeg = false;                  // a JPEG decoder block comes with it (jpeg_vX ip block)
  bool unified_queue = false;         // decode and encode share one ring (VCN 4 and newer)
  uint32_t decode_count = 0, encode_count = 0;
  AmdVcnCodecLimit decode[kAmdVcnMaxCodecs] = {};
  AmdVcnCodecLimit encode[kAmdVcnMaxCodecs] = {};

  bool known() const { return generation != AmdVcnGeneration::Unknown; }
  uint32_t decode_mask() const;   // codec_mask() bits
  uint32_t encode_mask() const;
  // The largest frame the engine decodes in `codec`; false if it does not decode it.
  bool decode_limit(VideoCodec codec, uint16_t* max_width, uint16_t* max_height) const;
};

// The engine of a VCN IP version (from the IP discovery table, or from the Ryzen SoC catalogue for the APUs whose table cannot be read). A version the
// driver does not list gives generation Unknown and no codecs. `apu_hint` picks between files that share a version: VCN 2.2.0 is renoir_vcn.bin on Renoir
// and Lucienne and green_sardine_vcn.bin on Cezanne and Barcelo; VCN 1.0 depends on the PCI device (amd_vcn1_firmware_name).
enum class AmdVcnFirmwareHint : uint8_t { None, Renoir, GreenSardine };
AmdVcnInfo amd_vcn_info(AmdIpVersion version, AmdVcnFirmwareHint hint = AmdVcnFirmwareHint::None);

const char* amd_vcn_generation_name(AmdVcnGeneration generation);

}  // namespace hardware
