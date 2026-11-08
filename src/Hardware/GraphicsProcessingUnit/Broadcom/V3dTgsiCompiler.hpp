/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace hardware::v3d_shader {

struct TgsiCompileInfo {
  uint32_t instruction_count = 0;
  uint32_t register_count = 0;
};

// Lowers the straight-line arithmetic portion of TGSI to V3D 4.2 QPU code. This first backend slice
// accepts TEMP-only MOV/ADD/SUB/MUL/MAD/MIN/MAX and ROUND/FLOOR/TRUNC/CEIL operations, with no source
// modifiers or saturation. Inputs, constants, stage I/O, flow control, and texture operations are
// reported as errors until their V3D stage ABI and instruction support are implemented. The output
// is a QPU thread body ending in the required thread-switch and delay slots.
bool compile_tgsi_alu(allocators::IAllocator& allocator, StringView text, ArrayView<uint64_t> output, TgsiCompileInfo* info, const char** error, uint32_t* line);

}  // namespace hardware::v3d_shader
