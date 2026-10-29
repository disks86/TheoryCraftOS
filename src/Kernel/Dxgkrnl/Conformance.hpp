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

// Direct3D conformance scenes, run through the kernel-mode driver on one adapter and on a reference adapter (the CPU renderer) to find out what a GPU device really does with
// the things Direct3D 9, 10 and 11 programs ask of it: vertex colours, depth, blending, constant buffers, textures, render to texture, strips, scissors, copies, and (when the
// device says it has them) blend state, texel access, system values, instancing and lines. Each scene says one of: it draws what the reference draws, it draws something else, the
// device refuses it (kNotSupported: the adapter says so honestly, a program can fall back), or it breaks (a step of the services failed, or the context was lost).
//
// The system runs this at boot on each accelerated GPU and logs the verdicts (main.cpp); the host tests run it on every device they can simulate.
#pragma once
#include "Kernel/Dxgkrnl/DxgKernel.hpp"

namespace dxgk {

enum class SceneVerdict : uint32_t { Same, Differs, Refused, Broken };

struct SceneResult {
  const char* name = "";
  SceneVerdict verdict = SceneVerdict::Same;
  uint32_t bad_pixels = 0;      // Differs: how many of the 32 x 32 pixels are off by more than the tolerance
  uint32_t first_bad = 0, device_value = 0, reference_value = 0;   // Differs: the first pixel that is off (y * 32 + x) and what each side drew there
  const uint32_t* device_picture = nullptr;      // the two pictures (32 x 32 words, valid during the callback)
  const uint32_t* reference_picture = nullptr;
  NtStatus status = 0;          // Refused, Broken: the status of the step
  const char* step = "";        // Refused, Broken: the step
  bool reference_failed = false;   // the reference could not draw it either (the verdict is about the device alone)
};

// A callback gets each scene's result as soon as it is known. Returns the number of scenes that are Differs or Broken.
using SceneReport = void (*)(void* context, const SceneResult& result);
uint32_t run_conformance(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index, uint32_t reference_index, SceneReport report, void* context);

}  // namespace dxgk
