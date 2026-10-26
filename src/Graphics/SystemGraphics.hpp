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

// Picks the rendering back ends for this machine, the way SystemGpu picks a GPU:
// the accelerated 3D device if the display GPU has a 3D engine we can drive
// (virgl on a virtio-gpu, i.e. QEMU with a -gl display; SVGA3D on a VMware SVGA adapter, i.e. VMware and VirtualBox's VMSVGA), else the software one.
// Storage is inline because there is no heap-free way to build them elsewhere.
#pragma once
#include "Graphics/IDisplayGraphics.hpp"
#include "Graphics/RenderNode.hpp"
#include "Graphics/Renderer2DSlot.hpp"
#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include "Utility/FixedString.hpp"

namespace graphics {

class SystemGraphics : public IDisplayGraphics {
 public:
  // `gpu` must outlive this object. If `allow_acceleration` is false the software
  // device is used even where a GPU device would work.
  // `allow_intel_render` switches on the render engine path of an Intel Gen9.5 display GPU (untried on hardware).
  SystemGraphics(hardware::SystemGpu& gpu, allocators::IAllocator& allocator, bool allow_acceleration = true, bool allow_intel_render = false);

  // The device to render 3D with: accelerated if it passed its self-test, else software.
  IGraphicsDevice& device() { return node_.device(); }
  bool accelerated() const { return node_.accelerated(); }
  // Why the software device is in use, in a few words ("display adapter has no 3D engine: Bochs VGA", "virgl self-test failed", ...); empty when accelerated.
  const char* software_reason() const { return node_.software_reason(); }
  // Always the CPU renderer's device (e.g. to compare against the accelerated one).
  IGraphicsDevice& software_device() { return node_.software_device(); }

  // The 2D renderer for the whole screen: drawn by the GPU device where there is one, else on the
  // CPU into a back buffer that present() copies to the display. Created on first use; nullptr
  // if out of memory.
  IRenderer2D* renderer_2d() override { return renderer_.get(node_, override_ ? override_ : display_, override_ == nullptr); }

  // Presents the 2D picture through `display` instead of straight to the display device (nullptr: straight again): the monitor compositor, when the
  // desktop is not laid out like the device's screen. The renderer is a software one then, drawing a picture of the size `display` reports
  // (call reset_renderer() to get it). It must outlive the renderer.
  void set_display_override(hardware::IGraphicsProcessingUnit* display) { override_ = display; }

  // Throws the 2D renderer away; the next renderer_2d() builds one for the display's size as it is then (after the display was resized for several
  // monitors). Nothing may be using the old renderer.
  void reset_renderer() { renderer_.reset(); }

  // The display GPU everything is shown on.
  hardware::IGraphicsProcessingUnit* display() { return display_; }
  // The 3D back end of the display GPU (GraphicsPool reuses it rather than building a second device on the same GPU).
  RenderNode& render_node() override { return node_; }

 private:
  hardware::IGraphicsProcessingUnit* display_;
  hardware::IGraphicsProcessingUnit* override_ = nullptr;
  RenderNode node_;
  Renderer2DSlot renderer_;
};

}  // namespace graphics
