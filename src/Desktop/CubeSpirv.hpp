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

// The 3D test's Vulkan shaders: SPIR-V (CubeShaders/CubeSpirv.inc, made by scripts/gen-cube-spirv.py) translated to DXBC with vulkan-1.dll's translator, the way the DLL does it
// in vkCreateGraphicsPipelines: the push constant block is constant buffer slot 0 in every stage, a combined image sampler is texture unit 0.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace desktop {

enum class SpirvStage : uint32_t { Vertex, Fragment };

struct SpirvTranslation {
  uint32_t inputs = 0;          // the locations the stage reads (bit n: location n)
  uint32_t outputs = 0;         // the locations it writes
  uint32_t texture_units = 0;   // how many it samples
};

// The cube's SPIR-V of the stage, flat (colored) or textured.
ArrayView<const uint32_t> cube_spirv(bool textured, SpirvStage stage);

// Translates `words` to DXBC into `dxbc` (cleared first), with a push constant block of `push_bytes` bytes. False if the module is refused or memory runs out; `error` (if
// given) then says why.
bool translate_spirv(allocators::IAllocator& allocator, ArrayView<const uint32_t> words, SpirvStage stage, uint32_t push_bytes, ByteBuffer& dxbc, SpirvTranslation* info,
                     const char** error = nullptr);

}  // namespace desktop
