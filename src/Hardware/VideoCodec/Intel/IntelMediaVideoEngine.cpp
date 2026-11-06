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

#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"

namespace hardware {

bool IntelMediaVideoEngine::initialize() {
  status_ = IntelMediaEngineHw::probe(registers_, gpu_.architecture);
  summary_.assign(gpu_.name);
  if (status_.probed) {
    summary_.append(", video boxes: ");
    summary_.append_uint(status_.vdbox_count());
    summary_.append(" of ");
    summary_.append_uint(IntelMediaEngineStatus::popcount(status_.vdbox_present));
    summary_.append(" awake");
  } else {
    summary_.append(", video engine registers not read");
  }
  if (gpu_.discrete && local_memory_bar_) {
    summary_.append(", local memory aperture ");
    summary_.append_uint(local_memory_bar_ >> 20);
    summary_.append(" MiB");
  }
  driver_.reset();
  info_.accelerated_decode_codecs = 0;
  info_.accelerated_encode_codecs = 0;
  generation_ = intel_gt::Generation::Gen9;
  xe_lpg_ = false;
  xe_hpg_ = false;
  xe2_ = false;
  // Three generations have a driver: Gen9.5 (its video box answers the look above) and Gen11, which run H.264, MPEG-2, VC-1, H.265 and VP9; and the integrated Xe-LP parts
  // (Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake), whose first video box must have answered, which run H.264, MPEG-2, H.265, VP9 and AV1. VC-1 stays with the CPU
  // there: the media driver has a Xe-LP path for it, but nothing says every Xe-LP part still has the hardware.
  const bool gen9 = gpu_.architecture == IntelGpuArchitecture::Gen9_5;
  const bool gen12 = gpu_.architecture == IntelGpuArchitecture::XeLp && !gpu_.discrete && gpu_.media_av1_decode && (status_.vdbox_awake & 1);
  // Ice Lake (Gen11) and its relatives: the MFX engine's commands are those of Gen9 apart from two that grew (checked against the media driver's command definitions), so
  // H.264, MPEG-2 and VC-1 run; the HCP commands (H.265, VP9) have their own Gen11 layouts, written from the same definitions.
  const bool gen11 = gpu_.architecture == IntelGpuArchitecture::Gen11 && !gpu_.discrete && (status_.vdbox_awake & 1);
  // Meteor Lake and Arrow Lake (Xe-LPG): the video boxes are on the media tile, and the setup's register bank is that tile's window (its engine, power wells, PAT and MOCS
  // are the Xe-LP ones at another place and with other values). The AVC, HEVC, VP9 and VDEnc commands are the same as Xe-LP's (checked field by field against the media
  // driver's Xe_LPM_plus definitions), and the AV1 ones (AVP) differ only in five commands that grew at the end (IntelAvpCommands.hpp). MPEG-2 is not offered there and VC-1
  // never was, so those stay with the CPU.
  const bool xe_lpg = gpu_.architecture == IntelGpuArchitecture::XeLpg && !gpu_.discrete && (status_.vdbox_awake & 1);
  // Arc Alchemist (Xe-HPG, discrete): the same engine on the main GT, driven out of system memory over PCIe (the card's own memory is not used). Its AVC, HEVC, VP9 and VDEnc
  // commands are Xe-LP's too (Xe_HPM compared field by field); AV1's are Xe-LPG's apart from the tile coding command.
  const bool xe_hpg = gpu_.architecture == IntelGpuArchitecture::XeHpg && gpu_.discrete && (status_.vdbox_awake & 1);
  // Lunar Lake and Arc Battlemage (Xe2): the media tile again, with Xe2's PAT and MOCS tables and context image. The decode commands (AVC, HEVC, VP9) are Xe-LP's with optional
  // new fields; AV1's have the Xe-LPG lengths but a 215-dword buffer command. The VDEnc ones are not (below).
  const bool xe3 = gpu_.architecture == IntelGpuArchitecture::Xe3 && !gpu_.discrete && (status_.vdbox_awake & 1);
  // Panther Lake and Wildcat Lake (Xe3): the kernel's xe driver gives the media tile Xe2's description, and the decode commands (AVC, HEVC, VP9, AV1) are Xe2's field for field
  // (checked against the media driver's Xe3_LPM definitions: only the VDEnc commands grew, to a 99-dword pipe buffer command, a 7-dword pipe mode select, 49-dword CMD1 and
  // 65-dword CMD2), so the Xe2 setup runs with the Xe3 flag for the encoders.
  // Nova Lake (Xe3p_LPM): the kernel's xe driver again gives the media tile Xe2's description (PAT entries 0 .. 4 and the video context image are Xe2's too), and the decode
  // commands equal Xe3's (the media driver's Xe3P_LPM definitions only split two header fields). Its VDEnc constant tables are closed (the open files are empty), so there is no
  // encode here: the CPU encodes.
  const bool xe3p = gpu_.architecture == IntelGpuArchitecture::Xe3p && !gpu_.discrete && (status_.vdbox_awake & 1);
  const bool xe2 = (gpu_.architecture == IntelGpuArchitecture::Xe2 && (status_.vdbox_awake & 1)) || xe3 || xe3p;
  if (setup_.registers && setup_.memory && setup_.ggtt_table.valid() && (gen9 || gen11 || gen12 || xe_lpg || xe_hpg || xe2) && status_.vdbox_count() > 0) {
    UniquePointer<intel_gt::VcsDriver> driver = UniquePointer<intel_gt::VcsDriver>::make(allocator_, *setup_.memory);
    intel_gt::VcsDriver::Config config;
    config.registers = setup_.registers;
    config.ggtt_table = setup_.ggtt_table;
    config.ggtt_first_page = setup_.ggtt_first_page;
    config.xe_lpg = xe_lpg;
    config.xe_hpg = xe_hpg;
    config.xe2 = xe2;
    config.xe2_hpm = xe2 && !xe3 && !xe3p && gpu_.discrete;
    config.xe3 = xe3;
    config.gmd_id_flush = (xe_lpg || xe2) && !gpu_.discrete;
    config.generation = (gen12 || xe_lpg || xe_hpg || xe2) ? intel_gt::Generation::Gen12 : gen11 ? intel_gt::Generation::Gen11 : intel_gt::Generation::Gen9;
    if (driver && driver->initialize(config)) {
      driver_ = util::move(driver);
      generation_ = config.generation;
      xe_lpg_ = xe_lpg;
      xe_hpg_ = xe_hpg;
      xe2_ = xe2;
      if (xe_lpg || xe_hpg || xe2) {
        info_.accelerated_decode_codecs = codec_mask(VideoCodec::H264) | (gpu_.media_av1_decode ? codec_mask(VideoCodec::Av1) : 0) | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) |
                                          (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0);
        // Encode, H.264 and H.265 with a constant quantiser: the VDEnc commands changed after Xe-LP (an 89-dword pipe buffer command, 7-dword weights, a 3-dword walker behind a 28-dword
        // HEVC/VP9 tile and slice state, longer CMD1 and CMD2, VDENC_AVC_IMG_STATE and VDENC_AVC_SLICE_STATE for H.264), and each family has its own constant tables. The images come
        // from the media driver's own code, compiled by tools/gen_intel_{hevc,avc}_xelpg.py (Xe-LPG, Lunar Lake and Battlemage; the HEVC one also does Arc) and
        // tools/gen_intel_avc_xehpg.py (Arc's older H.264 path). None of it has run on hardware.
        info_.accelerated_encode_codecs = xe3p ? 0 : codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Hevc);
      } else if (gen12) {
        info_.accelerated_decode_codecs = codec_mask(VideoCodec::Av1) | codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mpeg2) |
                                          (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0);
        info_.accelerated_encode_codecs = codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Hevc);   // (VDEnc, constant quantiser)
      } else if (gen11) {
        info_.accelerated_decode_codecs = codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mpeg2) | codec_mask(VideoCodec::Vc1) |
                                          (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0);
        info_.accelerated_encode_codecs = codec_mask(VideoCodec::H264);   // (VDEnc, constant quantiser)
      } else {
        info_.accelerated_decode_codecs = codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mpeg2) | codec_mask(VideoCodec::Vc1) |
                                          (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0);
        info_.accelerated_encode_codecs = codec_mask(VideoCodec::H264);   // (VDEnc, constant quantiser)
      }
    }
  }
  return true;
}

UniquePointer<IVideoDecoder> IntelMediaVideoEngine::create_decoder(const DecoderConfig& config) {
  const bool xe_lp = generation_ == intel_gt::Generation::Gen12;
  if (xe_lp && config.codec == VideoCodec::Av1 && hardware_decode()) {
    UniquePointer<IntelAv1Decoder> session = UniquePointer<IntelAv1Decoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));   // else the engine is busy with another session: the CPU does it
    return SoftwareVideoCodec::create_decoder(config);
  }
  if (config.codec == VideoCodec::H264 && hardware_decode()) {
    UniquePointer<IntelAvcDecoder> session = UniquePointer<IntelAvcDecoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));  // else the engine is busy with another session: the CPU does it
  }
  if (config.codec == VideoCodec::Vc1 && hardware_decode() && !xe_lp) {  // (xe_lp: every Gen12 engine)
    UniquePointer<IntelVc1Decoder> session = UniquePointer<IntelVc1Decoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  if (config.codec == VideoCodec::Mpeg2 && hardware_decode() && !xe_lpg_ && !xe_hpg_ && !xe2_) {
    UniquePointer<IntelMpeg2Decoder> session = UniquePointer<IntelMpeg2Decoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  if (config.codec == VideoCodec::Hevc && hardware_decode() && gpu_.media_hevc) {
    UniquePointer<IntelHevcDecoder> session = UniquePointer<IntelHevcDecoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  if (config.codec == VideoCodec::Vp9 && hardware_decode() && gpu_.media_vp9) {
    UniquePointer<IntelVp9Decoder> session = UniquePointer<IntelVp9Decoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  return SoftwareVideoCodec::create_decoder(config);
}

UniquePointer<IVideoEncoder> IntelMediaVideoEngine::create_encoder(const EncoderConfig& config) {
  if (config.codec == VideoCodec::H264 && hardware_decode() && (info_.accelerated_encode_codecs & codec_mask(VideoCodec::H264))) {
    UniquePointer<IntelAvcVdencEncoder> session = UniquePointer<IntelAvcVdencEncoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoEncoder>(util::move(session));   // else the engine is busy with another session (or the size is not one it takes): the CPU does it
  }
  if (config.codec == VideoCodec::Hevc && hardware_decode() && (info_.accelerated_encode_codecs & codec_mask(VideoCodec::Hevc))) {
    UniquePointer<IntelHevcVdencEncoder> session = UniquePointer<IntelHevcVdencEncoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoEncoder>(util::move(session));   // else the engine is busy with another session (or the size is not one it takes): the CPU does it
  }
  return SoftwareVideoCodec::create_encoder(config);
}

}  // namespace hardware
