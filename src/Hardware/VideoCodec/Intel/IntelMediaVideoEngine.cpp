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
  driver_.reset();
  info_.accelerated_decode_codecs = 0;
  hardware_status_ = HardwareStatus::NotRequested;
  // The driver is for Gen9.5 (the generation whose layouts it follows) and needs a video box that answered the look above.
  const bool requested = setup_.registers && setup_.memory && setup_.ggtt_table.valid();
  if (requested && gpu_.architecture != IntelGpuArchitecture::Gen9_5) hardware_status_ = HardwareStatus::UnsupportedGpu;
  else if (requested && status_.vdbox_count() == 0) hardware_status_ = HardwareStatus::NoVideoBox;
  else if (requested) {
    hardware_status_ = HardwareStatus::DriverFailed;
    UniquePointer<intel_gt::VcsDriver> driver = UniquePointer<intel_gt::VcsDriver>::make(allocator_, *setup_.memory);
    intel_gt::VcsDriver::Config config;
    config.registers = setup_.registers;
    config.ggtt_table = setup_.ggtt_table;
    config.ggtt_first_page = setup_.ggtt_first_page;
    if (driver && driver->initialize(config)) {
      driver_ = util::move(driver);
      info_.accelerated_decode_codecs = codec_mask(VideoCodec::H264);
      hardware_status_ = HardwareStatus::Running;
    }
  }
  return true;
}

const char* IntelMediaVideoEngine::status_text() const {
  switch (hardware_status_) {
    case HardwareStatus::NotRequested: return status_.probed ? "video box found, decoding on the CPU" : "engine present, decoding on the CPU";
    case HardwareStatus::UnsupportedGpu: return "engine driver is for Gen9.5 graphics, decoding on the CPU";
    case HardwareStatus::NoVideoBox: return "no video box answered, decoding on the CPU";
    case HardwareStatus::DriverFailed: return "engine driver did not start, decoding on the CPU";
    case HardwareStatus::Running: return "H.264 decoding runs on the engine (experimental)";
  }
  return "";
}

UniquePointer<IVideoDecoder> IntelMediaVideoEngine::create_decoder(const DecoderConfig& config) {
  if (config.codec == VideoCodec::H264 && hardware_decode()) {
    UniquePointer<IntelAvcDecoder> session = UniquePointer<IntelAvcDecoder>::make(allocator_, allocator_, *driver_, config);
    if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));  // else the engine is busy with another session: the CPU does it
  }
  return SoftwareVideoCodec::create_decoder(config);
}

}  // namespace hardware
