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
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/TgsiParse.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace hardware::v3d_shader {

struct TgsiCompileInfo {
  uint32_t instruction_count = 0;
  uint32_t register_count = 0;
  uint32_t uniform_count = 0;
  uint32_t tlb_config_count = 0;  // implicit TLB uniforms consumed by the fragment output epilogue
  uint32_t output_register_base = 0;
  uint32_t output_count = 0;
};

struct TgsiUniformBinding {
  gcn::TgsiFile file = gcn::TgsiFile::None;  // CONST is read from push constants; IMM carries its value below
  uint16_t index = 0;
  uint8_t component = 0;
  uint32_t immediate_bits = 0;
};

struct TgsiInputBinding {
  uint16_t index = 0;
  uint8_t component = 0;
  uint8_t vpm_value = 0;
  bool constant = false;
  uint32_t constant_bits = 0;
};

// Lowers the straight-line arithmetic portion of TGSI to V3D 4.2 QPU code. This first backend slice
// accepts TEMP/CONST/IMM MOV/ADD/SUB/MUL/MAD/MIN/MAX/RCP and ROUND/FLOOR/TRUNC/CEIL operations, with
// no source modifiers or saturation. Fragment shaders may write OUT[0] COLOR, and vertex shaders
// may write OUT[0] POSITION in RF60..RF63 for a later V3D stage epilogue. CONST and IMM values are
// emitted as ordered uniform bindings for the caller to place in the QPU uniform stream. Other stage
// I/O, flow control, and texture operations are reported as errors until their V3D ABI is implemented.
// Vertex IN sources need an explicit location/component-to-VPM mapping.
// The output is a QPU thread body ending in the required thread-switch and delay slots.
bool compile_tgsi_alu(allocators::IAllocator& allocator, StringView text, ArrayView<uint64_t> output, TgsiCompileInfo* info, const char** error, uint32_t* line,
                      ArrayView<TgsiUniformBinding> uniforms = {}, ArrayView<const TgsiInputBinding> inputs = {}, bool write_depth = false, bool terminate = true);

}  // namespace hardware::v3d_shader
