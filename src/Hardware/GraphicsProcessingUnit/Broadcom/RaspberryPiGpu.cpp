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

#include "Hardware/GraphicsProcessingUnit/Broadcom/RaspberryPiGpu.hpp"

namespace hardware {

bool RaspberryPiGpu::video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const {
  uint32_t base = 0, bytes = 0;
  if (!firmware_ || !firmware_->gpu_memory(&base, &bytes)) return false;
  uint64_t fb_base = 0, fb_bytes = 0;
  *total_bytes = bytes;
  *used_bytes = screen_.framebuffer_range(&fb_base, &fb_bytes) ? fb_bytes : 0;
  return true;
}

int32_t RaspberryPiGpu::temperature_millicelsius() {
  uint32_t milli = 0;
  if (!firmware_ || !firmware_->temperature(&milli) || milli > INT32_MAX) return -1;
  return static_cast<int32_t>(milli);
}

}  // namespace hardware
