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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpuMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

namespace hardware {

struct V3dSelfTestResult {
  static constexpr uint32_t kWidth = 128, kHeight = 96;
  // How far it got: false means GPU memory for the test could not be had (then the rest is empty).
  bool memory_ok = false;
  V3dJobResult job = V3dJobResult::Error;
  uint32_t clear_pixels = 0;  // pixels with exactly the clear colour
  uint32_t drawn_pixels = 0;  // pixels with anything else
  // The frame cleared the image to its colour and the triangle changed a good part of it (it covers about a sixth of the image, more than 1500 pixels).
  bool passed() const { return memory_ok && job == V3dJobResult::Done && clear_pixels > 0 && drawn_pixels > 800 && clear_pixels + drawn_pixels == kWidth * kHeight; }
};

// The clear-and-draw check of the 3D core, for a boot on real hardware: renders a 128 x 96 frame on the GPU - clears it to a colour and draws one triangle with
// red, green and blue corners through the built-in QPU programs (identity matrix, no culling, no depth) - then reads the pixels back and counts what changed. If a
// display is given the image is shown in its top left corner. Uses no floating point. The V3D has to be initialised and GPU memory available.
class V3dSelfTest {
 public:
  // The colour the image is cleared to as the image holds it (0xAARRGGBB).
  static constexpr uint32_t kClearPixel = 0xFF204080;

  static V3dSelfTestResult run(V3dGpu& gpu, IV3dGpuMemory& memory, uint32_t attempts, IGraphicsProcessingUnit* display = nullptr);
  // Counts the clear colour and everything else in a width x height image whose rows are `pitch` pixels apart.
  static void count_pixels(ArrayView<const uint32_t> pixels, uint32_t pitch, uint32_t width, uint32_t height, V3dSelfTestResult* result);
};

}  // namespace hardware
