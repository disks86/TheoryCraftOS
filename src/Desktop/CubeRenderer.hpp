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

// The spinning cube of the 3D test, drawn the way a Direct3D program is: through the kernel-mode Direct3D driver (Kernel/Dxgkrnl) with the same calls a user-mode
// driver makes - open an adapter, create a device and a context, allocations (render target, depth buffer, vertex and index buffers, the logo texture), shaders and
// pipelines, then a command stream per frame handed to Kernel::render() and the picture read back with Kernel::lock(). So the 3D test exercises (and times) the whole
// kernel path - validation, residency, scheduling, the device behind the adapter - not just the device.
//
// A frame's time is split in two: "submit" is Kernel::render() (the stream is checked, queued and run on the adapter's device, which is done when the call returns) and
// "readback" is the lock that brings the picture to the CPU plus the copy out of it. Their sum is what CubeView shows as "render".
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

struct CubeFrameTimes {
  uint64_t submit_us = 0;     // Kernel::render()
  uint64_t readback_us = 0;   // lock + copy of the color target
  uint64_t total_us() const { return submit_us + readback_us; }
};

class CubeRenderer {
 public:
  static constexpr uint32_t kLogoSize = 256;   // the texture is the logo at this size (its own)

  // `pid` is the process the kernel knows this renderer as (opened here, closed with the renderer).
  CubeRenderer(allocators::IAllocator& allocator, dxgk::Kernel& kernel, dxgk::ProcessId pid);
  ~CubeRenderer();
  CubeRenderer(const CubeRenderer&) = delete;
  CubeRenderer& operator=(const CubeRenderer&) = delete;

  // The cube's transform at `angle` for a picture of this aspect ratio. Mat4::perspective turns y so that +y is up on the screen; the cube mesh (CubeMesh.hpp) is built
  // for the devices' own y-down clip space, so that is turned back - otherwise the logo would stand on its head.
  static graphics::Mat4 transform(float angle, float aspect);

  // The first adapter of the kernel that is (or is not) a GPU; dxgk::kInvalidIndex if there is none. An adapter on the `preferred` device (the one the desktop draws on) comes
  // before the others, so that the picture can stay on that GPU instead of crossing to the CPU and back.
  static uint32_t find_adapter(dxgk::Kernel& kernel, bool accelerated, const graphics::IGraphicsDevice* preferred = nullptr);

  // Creates everything for a picture of this size on the adapter. False (and nothing is held) if the kernel refuses anything essential; the logo is best effort.
  // `shareable`: the color target can also be sampled, so that a renderer on the same device can show it in place (acquire_target) and the frame need not be read back.
  bool open(uint32_t adapter_index, uint32_t width, uint32_t height, bool shareable = false);
  void close();
  bool is_open() const { return static_cast<bool>(state_); }
  uint32_t adapter_index() const { return adapter_index_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  const dxgk::AdapterCaps& caps() const { return caps_; }
  // Whether the faces can show the logo (known once open).
  bool texture_available() const { return texture_available_; }

  // Draws one frame at `angle` (radians around the cube's turn) and copies it into picture(). `textured` is honoured when texture_available(). Without `read_back` the frame stays on
  // the GPU (picture() is then the frame before): show it with acquire_target(), or fetch_picture() to have it on the CPU after all.
  bool render(float angle, bool textured, CubeFrameTimes* times, bool read_back = true);
  // Whether the color target can be lent to a renderer on the adapter's device.
  bool shareable() const { return shareable_; }
  // Lends the device and its image of the last frame's color target; release_target() when done with it (the next render() needs it back).
  bool acquire_target(graphics::IGraphicsDevice** device, graphics::IImage** image);
  void release_target();
  // The last frame, read back into picture() now (what render() does unless told not to).
  bool fetch_picture() { return state_ && read_back(); }
  // The last frame, B8G8R8A8.
  const graphics::Surface& picture() const { return picture_; }
  bool last_textured() const { return last_textured_; }

 private:
  struct State;
  bool create_pipeline(bool textured, dxgk::Handle* pipeline);
  bool create_buffer(uint32_t flags, uint64_t size, dxgk::Handle* out);
  bool write(dxgk::Handle allocation, ConstByteView bytes);
  bool create_geometry();
  bool create_logo();
  bool read_back();

  allocators::IAllocator& allocator_;
  dxgk::Kernel& kernel_;
  dxgk::ProcessId pid_;
  bool process_open_ = false;
  UniquePointer<State> state_;
  uint32_t adapter_index_ = dxgk::kInvalidIndex;
  uint32_t width_ = 0, height_ = 0;
  dxgk::AdapterCaps caps_;
  graphics::Surface picture_;
  bool texture_available_ = false;
  bool last_textured_ = false;
  bool shareable_ = false;
  bool target_lent_ = false;
};

}  // namespace desktop
