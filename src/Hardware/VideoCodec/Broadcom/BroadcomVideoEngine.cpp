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

#include "Hardware/VideoCodec/Broadcom/BroadcomVideoEngine.hpp"

namespace hardware {

bool BroadcomVideoEngine::initialize() {
  const char* text = "BCM2711 video engine: HEVC block ready, decoding on the CPU (H.264 is on the VPU)";
  switch (block_.start()) {
    case RpiHevcBlock::Result::Ready: break;
    case RpiHevcBlock::Result::NoRegisters: text = "BCM2711 video engine: HEVC decode block not looked at, H.264 on the VPU"; break;
    case RpiHevcBlock::Result::ClockRefused: text = "BCM2711 video engine: the firmware would not start the HEVC clock"; break;
    case RpiHevcBlock::Result::InterruptsRefused: text = "BCM2711 video engine: the HEVC interrupt controller did not take its setup"; break;
  }
  summary_ = FixedString<120>(StringView(text));
  return true;  // the engine is on the machine either way; sessions run in software
}

void BroadcomVideoEngine::shutdown() { block_.stop(); }

}  // namespace hardware
