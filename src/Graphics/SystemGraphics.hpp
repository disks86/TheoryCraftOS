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
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Graphics3D/Svga/SvgaDevice.hpp"
#include "Graphics/Graphics3D/Virgl/VirglDevice.hpp"
#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include "Utility/FixedString.hpp"

namespace graphics {

class SystemGraphics {
 public:
  // `gpu` must outlive this object. If `allow_acceleration` is false the software
  // device is used even where a GPU device would work.
  SystemGraphics(hardware::SystemGpu& gpu, allocators::IAllocator& allocator, bool allow_acceleration = true);
  ~SystemGraphics();

  // The device to render 3D with: accelerated if it passed its self-test, else software.
  IGraphicsDevice& device() { return *active_; }
  bool accelerated() const { return active_ == gpu_device_; }
  // Why the software device is in use, in a few words ("display adapter has no 3D engine: Bochs VGA", "virgl self-test failed", ...); empty when accelerated.
  const char* software_reason() const { return reason_; }
  // Always the CPU renderer's device (e.g. to compare against the accelerated one).
  IGraphicsDevice& software_device() { return software_; }

  // The 2D renderer for the whole screen: drawn by the GPU device where there is one, else on the
  // CPU into a back buffer that present() copies to the display. Created on first use; nullptr
  // if out of memory.
  IRenderer2D* renderer_2d();

  // Presents the 2D picture through `display` instead of straight to the display device (nullptr: straight again): the monitor compositor, when the
  // desktop is not laid out like the device's screen. The renderer is a software one then, drawing a picture of the size `display` reports
  // (call reset_renderer() to get it). It must outlive the renderer.
  void set_display_override(hardware::IGraphicsProcessingUnit* display) { override_ = display; }

  // Throws the 2D renderer away; the next renderer_2d() builds one for the display's size as it is then (after the display was resized for several
  // monitors). Nothing may be using the old renderer.
  void reset_renderer();

  // The display GPU everything is shown on.
  hardware::IGraphicsProcessingUnit* display() { return display_; }

 private:
  bool self_test(IGraphicsDevice& device);
  // Draws a triangle on the device and looks at the pixel: for back ends whose protocol could not be tried against a real host in advance.
  bool drawing_test(IGraphicsDevice& device);

  allocators::IAllocator& allocator_;
  hardware::IGraphicsProcessingUnit* display_;
  hardware::IGraphicsProcessingUnit* override_ = nullptr;
  SoftwareDevice software_;
  alignas(8) unsigned char virgl_storage_[sizeof(VirglDevice)];
  alignas(8) unsigned char svga_storage_[sizeof(SvgaDevice)];
  bool svga_ = false;
  const char* reason_ = "";
  FixedString<96> reason_text_;
  Surface backbuffer_;
  SoftwareRenderer2D* software_2d_ = nullptr;
  DeviceRenderer2D* device_2d_ = nullptr;
  IGraphicsDevice* gpu_device_ = nullptr;
  IGraphicsDevice* active_ = nullptr;
};

}  // namespace graphics
