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
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics3D/IntelGen9/Gen9GraphicsDevice.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"
#include "Utility/UniquePointer.hpp"

namespace graphics {

// Everything the Gen9 graphics device stands on, for a real Intel GPU: GPU memory from the OS allocator, the render engine driver on the registers and the unused
// part of the global GTT that IntelGpu found, and the device itself. Built by RenderNode when the Intel render path is switched on (TCOS_INTEL_RENDER).
class IntelRenderStack {
 public:
  IntelRenderStack(allocators::IAllocator& allocator, hardware::IGraphicsProcessingUnit* display) : allocator_(allocator), display_(display), memory_(allocator), driver_(memory_) {}
  IntelRenderStack(const IntelRenderStack&) = delete;
  IntelRenderStack& operator=(const IntelRenderStack&) = delete;

  // Starts the engine and builds the device. False (and nothing left running) if the GPU is not a Gen9.5 integrated one with mapped registers, there are no spare
  // pages in the global GTT, or the engine did not start.
  bool initialize(hardware::IntelGpu& gpu);
  Gen9GraphicsDevice& device() { return *device_; }

 private:
  allocators::IAllocator& allocator_;
  hardware::IGraphicsProcessingUnit* display_;
  hardware::intel_gt::AllocatorGpuMemory memory_;
  hardware::intel_gt::VcsDriver driver_;
  UniquePointer<Gen9GraphicsDevice> device_;   // (declared last: destroyed first, which hands the engine's lease back)
};

}  // namespace graphics
