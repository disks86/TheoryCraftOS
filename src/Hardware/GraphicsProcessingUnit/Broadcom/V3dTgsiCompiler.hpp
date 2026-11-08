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
  uint32_t varying_input_count = 0;
  uint32_t varying_output_count = 0;
};

struct TgsiUniformBinding {
  gcn::TgsiFile file = gcn::TgsiFile::None;  // CONST is a push constant; IMM and None use the literal bits below
  uint16_t index = 0;
  uint8_t component = 0;
  uint32_t immediate_bits = 0;
};

struct TgsiInputBinding {
  uint16_t index = 0;
  uint8_t component = 0;
  uint8_t vpm_value = 0;
  bool constant = false;  // a fixed component value (e.g. the W of a float3 position)
  uint32_t constant_bits = 0;
};

// Lowers a straight-line arithmetic subset of TGSI to V3D 4.2 QPU code. It supports TEMP/CONST/IMM
// MOV/ADD/SUB/MUL/DIV/MAD/MIN/MAX/ABS/RCP and ROUND/FLOOR/TRUNC/CEIL operations, with
// source negation and absolute-value modifiers supported and saturation rejected. Fragment shaders may read one perspective GENERIC[0] vec4 and
// write OUT[0] COLOR. Vertex shaders may write OUT[0] POSITION (RF60..RF63) and optional OUT[1]
// GENERIC[0] (RF56..RF59) for V3D stage epilogues. CONST, IMM, and fixed input values become ordered
// uniform bindings. Other stage I/O, flow control, and texture operations remain unsupported. Vertex
// IN sources need an explicit location/component-to-VPM mapping. `terminate` adds the thread-switch
// and delay slots when the output is a complete standalone QPU program.
bool compile_tgsi_alu(allocators::IAllocator& allocator, StringView text, ArrayView<uint64_t> output, TgsiCompileInfo* info, const char** error, uint32_t* line,
                      ArrayView<TgsiUniformBinding> uniforms = {}, ArrayView<const TgsiInputBinding> inputs = {}, bool write_depth = false, bool terminate = true);

}  // namespace hardware::v3d_shader
