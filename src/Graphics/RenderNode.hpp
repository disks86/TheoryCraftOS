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

// The 3D back end of one GPU: the accelerated device if that GPU has a 3D engine we can drive and it passes its self-test (virgl on a virtio-gpu,
// SVGA3D on a VMware SVGA adapter), else the CPU renderer. SystemGraphics has one for the display GPU; GraphicsPool has one for each other GPU of
// the machine, so a frame can be rendered on any of them (see GraphicsPool).
#pragma once
#include "Graphics/Graphics3D/LockedDevice.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Graphics3D/Svga/SvgaDevice.hpp"
#include "Graphics/Graphics3D/Virgl/VirglDevice.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.hpp"
#include "Utility/FixedString.hpp"

namespace graphics {

class RenderNode {
 public:
  // `gpu` is the display driver of the GPU (null if it has none: only the CPU renders then); `virtio` / `vmware` are the same driver when it is
  // one of those (they carry the 3D command channel). All must outlive this object. With `allow_acceleration` false the CPU renderer is used
  // even where a GPU device would work.
  RenderNode(hardware::IGraphicsProcessingUnit* gpu, hardware::VirtioGpu* virtio, hardware::VmwareSvgaGpu* vmware, allocators::IAllocator& allocator,
             bool allow_acceleration = true);
  ~RenderNode();
  RenderNode(const RenderNode&) = delete;
  RenderNode& operator=(const RenderNode&) = delete;

  // The device to render 3D with: accelerated if it passed its self-test, else software.
  IGraphicsDevice& device() { return *active_; }
  bool accelerated() const { return active_ == gpu_device_; }
  // The accelerated device (through its lock), or nullptr.
  IGraphicsDevice* gpu_device() { return gpu_device_; }
  // Why the software device is in use, in a few words; empty when accelerated.
  const char* software_reason() const { return reason_; }
  IGraphicsDevice& software_device() { return software_; }
  hardware::IGraphicsProcessingUnit* gpu() { return gpu_; }

 private:
  // Look for a 3D engine on the GPU and use it if it works (sets the device, or the reason there is none); and give it back (RenderNodeAccel.cpp).
  void build_accelerated(hardware::VirtioGpu* virtio, hardware::VmwareSvgaGpu* vmware, allocators::IAllocator& allocator, bool allow_acceleration);
  void destroy_accelerated();
  bool self_test(IGraphicsDevice& device);
  // Draws a triangle on the device and looks at the pixel: for back ends whose protocol could not be tried against a real host in advance.
  bool drawing_test(IGraphicsDevice& device);

  hardware::IGraphicsProcessingUnit* gpu_;
  SoftwareDevice software_;
  alignas(8) unsigned char virgl_storage_[sizeof(VirglDevice)];
  alignas(8) unsigned char svga_storage_[sizeof(SvgaDevice)];
  // The accelerated device is used through a locked wrapper (it is shared by the 2D renderer, the 3D test window and the browser's canvases, which run on
  // different threads); raw_device_ is the real one.
  alignas(8) unsigned char locked_storage_[sizeof(LockedDevice)];
  LockedDevice* locked_ = nullptr;
  IGraphicsDevice* raw_device_ = nullptr;
  bool svga_ = false;
  const char* reason_ = "";
  FixedString<96> reason_text_;
  IGraphicsDevice* gpu_device_ = nullptr;
  IGraphicsDevice* active_ = nullptr;
};

}  // namespace graphics
