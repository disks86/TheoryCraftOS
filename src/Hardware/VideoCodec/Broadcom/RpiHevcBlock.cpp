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

#include "Hardware/VideoCodec/Broadcom/RpiHevcBlock.hpp"

namespace hardware {

RpiHevcBlock::Result RpiHevcBlock::start() {
  using namespace rpi_hevc;
  ready_ = false;
  version_ = 0;
  if (!hevc_.valid() || !intc_.valid()) return Result::NoRegisters;
  // The block is clocked by the firmware's HEVC clock, which is off until asked.
  if (firmware_) {
    uint32_t actual = 0;
    if (!firmware_->set_clock_rate(vc_clock::kHevc, kClockHz, &actual) || !firmware_->set_clock_enabled(vc_clock::kHevc, true)) return Result::ClockRefused;
    clock_on_ = true;
  }
  // Latched interrupts cleared (write one), then both decoder lines enabled on their rising edge.
  intc_.write32(kIcCtrl, kIcAllInterrupts);
  intc_.write32(kIcCtrl, kIcActive1Enable | kIcActive1Edge | kIcActive2Enable | kIcActive2Edge);
  const uint32_t control = intc_.read32(kIcCtrl);
  if ((control & kIcReservedMask) != 0 || (control & (kIcActive1Enable | kIcActive2Enable)) != (kIcActive1Enable | kIcActive2Enable)) return Result::InterruptsRefused;
  version_ = hevc_.read32(kVersion);
  ready_ = true;
  return Result::Ready;
}

void RpiHevcBlock::stop() {
  if (intc_.valid()) intc_.write32(rpi_hevc::kIcCtrl, rpi_hevc::kIcAllInterrupts);
  if (firmware_ && clock_on_) firmware_->set_clock_enabled(vc_clock::kHevc, false);
  clock_on_ = false;
  ready_ = false;
}

}  // namespace hardware
