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
#include "Hardware/Mmio.hpp"
#include "Hardware/Platform/RaspberryPi/VideoCoreFirmware.hpp"
#include "Hardware/VideoCodec/Broadcom/RpiHevcBlock.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

// The video engine of the Raspberry Pi 4 and Pi 400 (BCM2711). It has two parts and neither is driven for decoding yet:
//   - the HEVC decoder block (H.265 Main and Main 10, up to 4096x2176 at 60 fps), whose registers this class finds, powers (the firmware's HEVC clock) and puts in a
//     known state (interrupts cleared and enabled), but which decodes nothing until the slice command stream for it is written (rpi_hevc registers);
//   - the H.264 decoder (up to 1080p60) and encoder, which are programs of the VideoCore's VPU that the firmware runs for the ARM cores through its message
//     queue (VCHIQ), a service this OS does not speak.
// Sessions therefore run on the CPU through the software codecs, as with the other engines; info().hardware_engine says the engine is there. Everything here has only
// been run against a pretend register file.
class BroadcomVideoEngine final : public SoftwareVideoCodec {
 public:
  // `hevc` is the decoder's register window (rpi_hevc registers) and `intc` its interrupt controller; either may be empty to skip looking at the hardware.
  // `firmware` (optional) turns the engine's clock on and off.
  BroadcomVideoEngine(allocators::IAllocator& allocator, const MmioRegion& hevc, const MmioRegion& intc, VideoCoreFirmware* firmware = nullptr)
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), block_(hevc, intc, firmware) {}

  const char* name() const override { return "Raspberry Pi video engine (BCM2711 HEVC)"; }
  bool initialize() override;
  void shutdown() override;
  const char* engine_summary() const override { return summary_.c_str(); }

  // The HEVC block answered and is idle with its interrupts enabled.
  bool hevc_block_ready() const { return block_.ready(); }
  // What the block's version register said (0 when it was not looked at).
  uint32_t hevc_version() const { return block_.version(); }
  // Codecs the engine would serve once driven: H.265 in hardware; H.264 decode and encode on the VPU.
  uint32_t engine_decode_codecs() const { return codec_mask(VideoCodec::Hevc) | codec_mask(VideoCodec::H264); }
  uint32_t engine_encode_codecs() const { return codec_mask(VideoCodec::H264); }

 private:
  RpiHevcBlock block_;
  FixedString<120> summary_;
};

}  // namespace hardware
