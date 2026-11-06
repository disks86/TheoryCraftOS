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

#include "Hardware/VideoCodec/AMD/AmdVcnGenerations.hpp"

namespace hardware {

namespace {
using G = AmdVcnGeneration;
using C = VideoCodec;

// The codec sets of amdgpu's query_video_codecs (first instance). Named after the driver's arrays.
struct CodecSet {
  uint32_t count;
  AmdVcnCodecLimit limits[kAmdVcnMaxCodecs];
};

constexpr CodecSet kNone = {0, {}};
// vega_video_codecs_encode (VCN 1, VCN 2.2/2.5) and nv_video_codecs_encode (VCN 2.0)
constexpr CodecSet kVegaEncode = {2, {{C::H264, 4096, 4096}, {C::Hevc, 4096, 4096}}};
// rv_video_codecs_decode (VCN 1)
constexpr CodecSet kRavenDecode = {7, {{C::Mpeg2, 1920, 1088}, {C::Mpeg4, 1920, 1088}, {C::H264, 4096, 4096}, {C::Vc1, 1920, 1088}, {C::Hevc, 4096, 4096},
                                       {C::Mjpeg, 8192, 8192}, {C::Vp9, 4096, 4096}}};
// rn_video_codecs_decode (VCN 2.2, 2.5, 2.6)
constexpr CodecSet kRenoirDecode = {7, {{C::Mpeg2, 1920, 1088}, {C::Mpeg4, 1920, 1088}, {C::H264, 4096, 4096}, {C::Vc1, 1920, 1088}, {C::Hevc, 8192, 4352},
                                        {C::Mjpeg, 16384, 16384}, {C::Vp9, 8192, 4352}}};
// nv_video_codecs_decode (VCN 2.0)
constexpr CodecSet kNaviDecode = {7, {{C::Mpeg2, 1920, 1088}, {C::Mpeg4, 1920, 1088}, {C::H264, 4096, 4096}, {C::Vc1, 1920, 1088}, {C::Hevc, 8192, 4352},
                                      {C::Mjpeg, 8192, 8192}, {C::Vp9, 8192, 4352}}};
// sc_video_codecs_encode (VCN 3)
constexpr CodecSet kScEncode = {2, {{C::H264, 4096, 4096}, {C::Hevc, 8192, 4352}}};
// sc_video_codecs_decode_vcn0 (VCN 3.0.x)
constexpr CodecSet kScDecode = {8, {{C::Mpeg2, 1920, 1088}, {C::Mpeg4, 1920, 1088}, {C::H264, 4096, 4096}, {C::Vc1, 1920, 1088}, {C::Hevc, 8192, 4352},
                                    {C::Mjpeg, 16384, 16384}, {C::Vp9, 8192, 4352}, {C::Av1, 8192, 4352}}};
// bg_video_codecs_decode (VCN 3.0.33, Navi 24: no encoder)
constexpr CodecSet kBeigeGobyDecode = {3, {{C::H264, 4096, 4096}, {C::Hevc, 8192, 4352}, {C::Vp9, 8192, 4352}}};
// yc_video_codecs_decode (VCN 3.1.x: the APUs from Rembrandt on drop MPEG-2, MPEG-4 and VC-1)
constexpr CodecSet kYellowCarpDecode = {5, {{C::H264, 4096, 4096}, {C::Hevc, 8192, 4352}, {C::Vp9, 8192, 4352}, {C::Mjpeg, 16384, 16384}, {C::Av1, 8192, 4352}}};
// vcn_4_0_0_video_codecs_*_vcn0, vcn_5_0_0_*, vcn_5_3_0_* (VCN 4 and 5: AV1 encode)
constexpr CodecSet kVcn4Encode = {3, {{C::H264, 4096, 4096}, {C::Hevc, 8192, 4352}, {C::Av1, 8192, 4352}}};
constexpr CodecSet kVcn4Decode = {5, {{C::H264, 4096, 4096}, {C::Hevc, 8192, 4352}, {C::Mjpeg, 16384, 16384}, {C::Vp9, 8192, 4352}, {C::Av1, 8192, 4352}}};

struct Entry {
  AmdIpVersion version;
  G generation;
  const char* firmware;
  bool jpeg;
  const CodecSet* decode;
  const CodecSet* encode;
};

// amdgpu_discovery_set_mm_ip_blocks (which ip block a version gets, and whether a JPEG block comes with it) and amdgpu_ucode_ip_version_decode /
// amdgpu_vcn.c (the file). nullptr where the file depends on more than the version.
constexpr Entry kEntries[] = {
    {{1, 0, 0}, G::Vcn1, nullptr, true, &kRavenDecode, &kVegaEncode},
    {{1, 0, 1}, G::Vcn1, nullptr, true, &kRavenDecode, &kVegaEncode},
    {{2, 0, 0}, G::Vcn2, "navi10_vcn.bin", true, &kNaviDecode, &kVegaEncode},
    {{2, 0, 2}, G::Vcn2, nullptr, true, &kNaviDecode, &kVegaEncode},   // navi12_vcn.bin or navi14_vcn.bin
    {{2, 2, 0}, G::Vcn2, nullptr, true, &kRenoirDecode, &kVegaEncode},   // renoir_vcn.bin or green_sardine_vcn.bin (see the hint)
    {{2, 5, 0}, G::Vcn2_5, "arcturus_vcn.bin", true, &kRenoirDecode, &kVegaEncode},
    {{2, 6, 0}, G::Vcn2_5, "aldebaran_vcn.bin", true, &kRenoirDecode, &kVegaEncode},
    {{3, 0, 0}, G::Vcn3, nullptr, true, &kScDecode, &kScEncode},   // sienna_cichlid_vcn.bin or navy_flounder_vcn.bin, by the graphics version
    {{3, 0, 2}, G::Vcn3, "vangogh_vcn.bin", true, &kScDecode, &kScEncode},
    {{3, 0, 16}, G::Vcn3, "dimgrey_cavefish_vcn.bin", true, &kScDecode, &kScEncode},
    {{3, 0, 33}, G::Vcn3, "beige_goby_vcn.bin", false, &kBeigeGobyDecode, &kNone},
    {{3, 1, 1}, G::Vcn3, "yellow_carp_vcn.bin", true, &kYellowCarpDecode, &kScEncode},
    {{3, 1, 2}, G::Vcn3, "vcn_3_1_2.bin", true, &kYellowCarpDecode, &kScEncode},
    {{4, 0, 0}, G::Vcn4, "vcn_4_0_0.bin", true, &kVcn4Decode, &kVcn4Encode},
    {{4, 0, 2}, G::Vcn4, "vcn_4_0_2.bin", true, &kVcn4Decode, &kVcn4Encode},
    {{4, 0, 3}, G::Vcn4, "vcn_4_0_3.bin", true, &kVcn4Decode, &kNone},   // Instinct MI300: decode only
    {{4, 0, 4}, G::Vcn4, "vcn_4_0_4.bin", true, &kVcn4Decode, &kVcn4Encode},
    {{4, 0, 5}, G::Vcn4, "vcn_4_0_5.bin", true, &kVcn4Decode, &kVcn4Encode},
    {{4, 0, 6}, G::Vcn4, "vcn_4_0_6.bin", true, &kVcn4Decode, &kVcn4Encode},
    {{5, 0, 0}, G::Vcn5, "vcn_5_0_0.bin", true, &kVcn4Decode, &kVcn4Encode},
    {{5, 0, 1}, G::Vcn5, "vcn_5_0_1.bin", true, &kVcn4Decode, &kNone},   // Instinct MI350: decode only
    {{5, 0, 2}, G::Vcn5, "vcn_5_0_2.bin", true, &kNone, &kNone},           // amdgpu reports no codec list for it
    {{5, 3, 0}, G::Vcn5, "vcn_5_3_0.bin", true, &kVcn4Decode, &kVcn4Encode},
};

uint32_t mask_of(const AmdVcnCodecLimit* limits, uint32_t count) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < count && i < kAmdVcnMaxCodecs; ++i) mask |= codec_mask(limits[i].codec);
  return mask;
}

void copy_set(const CodecSet& set, AmdVcnCodecLimit (&out)[kAmdVcnMaxCodecs], uint32_t* count) {
  *count = set.count < kAmdVcnMaxCodecs ? set.count : kAmdVcnMaxCodecs;
  for (uint32_t i = 0; i < *count; ++i) out[i] = set.limits[i];
}
}  // namespace

uint32_t AmdVcnInfo::decode_mask() const { return mask_of(decode, decode_count); }
uint32_t AmdVcnInfo::encode_mask() const { return mask_of(encode, encode_count); }

bool AmdVcnInfo::decode_limit(VideoCodec codec, uint16_t* max_width, uint16_t* max_height) const {
  for (uint32_t i = 0; i < decode_count && i < kAmdVcnMaxCodecs; ++i) {
    if (decode[i].codec != codec) continue;
    if (max_width) *max_width = decode[i].max_width;
    if (max_height) *max_height = decode[i].max_height;
    return true;
  }
  return false;
}

AmdVcnInfo amd_vcn_info(AmdIpVersion version, AmdVcnFirmwareHint hint) {
  AmdVcnInfo info;
  info.version = version;
  for (const Entry& e : kEntries) {
    if (e.version.major != version.major || e.version.minor != version.minor || e.version.revision != version.revision) continue;
    info.generation = e.generation;
    info.firmware = e.firmware;
    if (version.major == 2 && version.minor == 2) {
      if (hint == AmdVcnFirmwareHint::Renoir) info.firmware = "renoir_vcn.bin";
      else if (hint == AmdVcnFirmwareHint::GreenSardine) info.firmware = "green_sardine_vcn.bin";
    }
    info.jpeg = e.jpeg;
    info.unified_queue = e.generation == G::Vcn4 || e.generation == G::Vcn5;
    copy_set(*e.decode, info.decode, &info.decode_count);
    copy_set(*e.encode, info.encode, &info.encode_count);
    return info;
  }
  return info;
}

const char* amd_vcn_generation_name(AmdVcnGeneration generation) {
  switch (generation) {
    case G::Vcn1: return "VCN 1";
    case G::Vcn2: return "VCN 2";
    case G::Vcn2_5: return "VCN 2.5";
    case G::Vcn3: return "VCN 3";
    case G::Vcn4: return "VCN 4";
    case G::Vcn5: return "VCN 5";
    case G::Unknown: break;
  }
  return "VCN (unknown version)";
}

}  // namespace hardware
