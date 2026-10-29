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

// A model of the graphics command processor for the packets GcnState emits: it takes a PM4 stream, keeps the SH, context and uconfig register files the SET_*_REG packets write,
// and executes a draw by reading the registers back the way the hardware would (the shader programs and their resources, the user data, the interpolation setup, the colour
// buffer, the viewport and scissors, the vertex fetch through the descriptors in memory) and running the shaders on the wave emulator (GcnDraw). Memory is the caller's (IGpuMemory),
// so a test can place the shaders, constants and vertices anywhere and look at the colour buffer afterwards.
//
// What the model does not do it refuses: a draw with a state outside what GcnState sets (another primitive type, a stage other than the vertex shader, blending, depth or stencil,
// a colour format other than 32_32_32_32 float, an instance count other than one, interpolation setups other than perspective at the pixel centre) fails with Unsupported rather than
// drawing something else. Unknown packets fail too.
#pragma once

#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnDraw.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnPm4.hpp"
#include "Utility/ArrayView.hpp"

namespace gcn {

class IGpuMemory {
 public:
  virtual bool read(uint64_t address, ArrayView<uint32_t> out) = 0;
  virtual bool write(uint64_t address, ArrayView<const uint32_t> data) = 0;

 protected:
  IGpuMemory() = default;
  ~IGpuMemory() = default;
};

class CommandProcessorModel {
 public:
  enum class Result : uint8_t { Ok, BadPacket, Unsupported, MemoryFault, ShaderFault, OutOfMemory };
  static const char* result_name(Result result);

  CommandProcessorModel(allocators::IAllocator& allocator, IGpuMemory& memory, ITextureSource* textures = nullptr)
      : allocator_(allocator), memory_(memory), textures_(textures) {}

  // Runs the packets in order; stops at the first problem (the packets before it have taken effect).
  Result execute(ArrayView<const uint32_t> stream);

  uint32_t sh_reg(uint32_t reg) const { return reg >= pm4::kShRegStart && reg < pm4::kShRegEnd ? sh_[reg - pm4::kShRegStart] : 0; }
  uint32_t context_reg(uint32_t reg) const { return reg >= pm4::kContextRegStart && reg < pm4::kContextRegEnd ? context_[reg - pm4::kContextRegStart] : 0; }
  uint32_t uconfig_reg(uint32_t reg) const { return reg >= pm4::kUconfigRegStart && reg < pm4::kUconfigRegEnd ? uconfig_[reg - pm4::kUconfigRegStart] : 0; }

  uint32_t draws() const { return draws_; }
  uint32_t fence_writes() const { return fence_writes_; }
  const DrawStats& last_draw() const { return last_stats_; }

 private:
  Result set_registers(const Pm4Packet& p, uint32_t* file, uint32_t start, uint32_t end);
  Result draw(bool indexed, const Pm4Packet& p);
  Result read_code(uint64_t address, List<uint32_t>& code);

  allocators::IAllocator& allocator_;
  IGpuMemory& memory_;
  ITextureSource* textures_;
  uint32_t sh_[pm4::kShRegEnd - pm4::kShRegStart] = {};
  uint32_t context_[pm4::kContextRegEnd - pm4::kContextRegStart] = {};
  uint32_t uconfig_[pm4::kUconfigRegEnd - pm4::kUconfigRegStart] = {};
  uint32_t index_type_ = 0;
  uint32_t num_instances_ = 1;
  uint32_t draws_ = 0, fence_writes_ = 0;
  DrawStats last_stats_;
};

}  // namespace gcn
