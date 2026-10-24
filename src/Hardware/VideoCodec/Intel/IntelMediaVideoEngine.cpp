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
  // The driver is for Gen9.5 (the generation whose layouts it follows) and needs a video box that answered the look above.
  if (setup_.registers && setup_.memory && setup_.ggtt_table.valid() && gpu_.architecture == IntelGpuArchitecture::Gen9_5 && status_.vdbox_count() > 0) {
    UniquePointer<intel_gt::VcsDriver> driver = UniquePointer<intel_gt::VcsDriver>::make(allocator_, *setup_.memory);
    intel_gt::VcsDriver::Config config;
    config.registers = setup_.registers;
    config.ggtt_table = setup_.ggtt_table;
    config.ggtt_first_page = setup_.ggtt_first_page;
    if (driver && driver->initialize(config)) {
      driver_ = util::move(driver);
      info_.accelerated_decode_codecs = codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mpeg2) | codec_mask(VideoCodec::Vc1) | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0);
    }
  }
  return true;
}

UniquePointer<IVideoDecoder> IntelMediaVideoEngine::create_decoder(const DecoderConfig& config) {
  if (config.codec == VideoCodec::H264 && hardware_decode()) {
    UniquePointer<IntelAvcDecoder> session = UniquePointer<IntelAvcDecoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));  // else the engine is busy with another session: the CPU does it
  }
  if (config.codec == VideoCodec::Vc1 && hardware_decode()) {
    UniquePointer<IntelVc1Decoder> session = UniquePointer<IntelVc1Decoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  if (config.codec == VideoCodec::Mpeg2 && hardware_decode()) {
    UniquePointer<IntelMpeg2Decoder> session = UniquePointer<IntelMpeg2Decoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  if (config.codec == VideoCodec::Hevc && hardware_decode() && gpu_.media_hevc) {
    UniquePointer<IntelHevcDecoder> session = UniquePointer<IntelHevcDecoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
  }
  return SoftwareVideoCodec::create_decoder(config);
}

}  // namespace hardware
