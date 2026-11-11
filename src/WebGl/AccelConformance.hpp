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

// Scenes that check the 3D hardware path of the GL contexts (GlAccel.cpp, TgsiTranslator.cpp) against their CPU rasterizer: each GLSL ES 3.00 program draws a
// small picture once with the accelerator and once without, and the two must agree to within a few levels per channel. The kernel runs them at boot on the
// machine's accelerator (main.cpp: the log says what each did); the host tests run them on a TGSI interpreter (tests/GlAccelTest.cpp). Between them they show
// whether the translation means on a real device what the shaders mean on the CPU.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "WebGl/Accelerator.hpp"

namespace webgl {

enum class AccelVerdict : uint8_t {
  Same,          // the device drew it as the CPU does
  Differs,       // it drew something else
  NotTaken,      // the draw stayed on the CPU (why: AccelSceneResult::why)
  Broken,        // the program did not build, or the GL failed
};

struct AccelSceneResult {
  const char* name = nullptr;
  AccelVerdict verdict = AccelVerdict::Broken;
  const char* why = nullptr;       // NotTaken: the reason the context gave
  uint32_t bad_channels = 0;       // Differs: how many channels differ by more than the tolerance, and the first of them
  uint32_t first_x = 0, first_y = 0, first_channel = 0;
  uint8_t device_value = 0, cpu_value = 0;
  uint32_t drawn_pixels = 0;       // pixels of the CPU's picture that are not the clear colour (a scene that draws nothing would agree with anything)
};

// Runs every scene with `accelerator` (installed for the call: the previous one is put back after), reporting each. Returns how many did not draw as the CPU does
// (Differs and Broken; NotTaken is not counted - a device may refuse what it cannot do). Uses the GL contexts of src/WebGl only: nothing else may draw with the
// accelerator meanwhile.
uint32_t run_accel_conformance(allocators::IAllocator& allocator, IAccelerator& accelerator, void (*report)(void* context, const AccelSceneResult& result), void* context);
uint32_t accel_scene_count();

}  // namespace webgl
