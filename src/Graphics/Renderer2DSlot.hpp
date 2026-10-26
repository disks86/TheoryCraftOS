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

// The 2D renderer of one GPU, created on first use: drawn by the GPU's own device where it has one, else on the CPU into a back buffer that present()
// copies to the target display. SystemGraphics has one for the display GPU, GraphicsPool one for each GPU of the machine.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/RenderNode.hpp"

namespace graphics {

class Renderer2DSlot {
 public:
  explicit Renderer2DSlot(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Renderer2DSlot() { reset(); }
  Renderer2DSlot(const Renderer2DSlot&) = delete;
  Renderer2DSlot& operator=(const Renderer2DSlot&) = delete;

  // The renderer, built for `target`'s size as it is now; nullptr if `target` is null or memory ran out. With `allow_device` and an accelerated
  // `node` the GPU draws; otherwise (or when `allow_device` is false, because the picture is shown through something other than the GPU's own
  // screen) the CPU does and `target` receives the finished picture.
  IRenderer2D* get(RenderNode& node, hardware::IGraphicsProcessingUnit* target, bool allow_device);
  // Throws the renderer away; the next get() builds one for the target's size as it is then. Nothing may be using the old renderer.
  void reset();

 private:
  allocators::IAllocator& allocator_;
  Surface backbuffer_;
  SoftwareRenderer2D* software_ = nullptr;
  DeviceRenderer2D* device_ = nullptr;
};

}  // namespace graphics
