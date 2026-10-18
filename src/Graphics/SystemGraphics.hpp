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
// (virgl on a virtio-gpu, i.e. QEMU with a -gl display), else the software one.
// Storage is inline because there is no heap-free way to build them elsewhere.
#pragma once
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Graphics3D/Virgl/VirglDevice.hpp"
#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"

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
  // Always the CPU renderer's device (e.g. to compare against the accelerated one).
  IGraphicsDevice& software_device() { return software_; }

  // The 2D renderer for the whole screen: drawn by the GPU device where there is one, else on the
  // CPU into a back buffer that present() copies to the display. Created on first use; nullptr
  // if out of memory.
  IRenderer2D* renderer_2d();

  // Throws the 2D renderer away; the next renderer_2d() builds one for the display's size as it is then (after the display was resized for several
  // monitors). Nothing may be using the old renderer.
  void reset_renderer();

  // The display GPU everything is shown on.
  hardware::IGraphicsProcessingUnit* display() { return display_; }

 private:
  bool self_test(IGraphicsDevice& device);

  allocators::IAllocator& allocator_;
  hardware::IGraphicsProcessingUnit* display_;
  SoftwareDevice software_;
  alignas(8) unsigned char virgl_storage_[sizeof(VirglDevice)];
  Surface backbuffer_;
  SoftwareRenderer2D* software_2d_ = nullptr;
  DeviceRenderer2D* device_2d_ = nullptr;
  IGraphicsDevice* gpu_device_ = nullptr;
  IGraphicsDevice* active_ = nullptr;
};

}  // namespace graphics
