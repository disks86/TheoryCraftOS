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
#include "Hardware/VideoCodec/Broadcom/RpiHevcRegisters.hpp"

namespace hardware {

// The HEVC decoder block of the BCM2711 on its own (no codec classes): finds it, starts the firmware's HEVC clock and puts its interrupt controller in a known state.
// Decoding on it needs the slice command stream (rpi_hevc registers) and is not done; this is the part a boot check and the video engine share.
class RpiHevcBlock {
 public:
  // The rate the HEVC clock is asked for: the decoder needs 500 MHz for 4Kp60.
  static constexpr uint32_t kClockHz = 500000000;

  // `hevc` is the decoder's register window and `intc` its interrupt controller; `firmware` (optional) turns the clock on and off.
  RpiHevcBlock(const MmioRegion& hevc, const MmioRegion& intc, VideoCoreFirmware* firmware) : hevc_(hevc), intc_(intc), firmware_(firmware) {}
  enum class Result : uint8_t { Ready, NoRegisters, ClockRefused, InterruptsRefused };
  Result start();
  void stop();
  bool ready() const { return ready_; }
  uint32_t version() const { return version_; }  // the block's version register (0 until started)

 private:
  MmioRegion hevc_, intc_;
  VideoCoreFirmware* firmware_;
  bool ready_ = false, clock_on_ = false;
  uint32_t version_ = 0;
};

}  // namespace hardware
