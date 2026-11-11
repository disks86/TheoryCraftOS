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

// The spinning cube of the 3D test, drawn the way a Direct3D or a Vulkan program is: through the kernel-mode Direct3D driver (Kernel/Dxgkrnl) with the same calls a user-mode
// driver makes - open an adapter, create a device and a context, allocations (render target, depth buffer, vertex and index buffers, the logo texture), shaders and
// pipelines, then a command stream per frame handed to Kernel::render() and the picture read back with Kernel::lock(). So the 3D test exercises (and times) the whole
// kernel path - validation, residency, scheduling, the device behind the adapter - not just the device.
//
// The two APIs differ in how the shaders and pipelines reach the kernel (CubeApi):
//  - Direct3D: the kernel's built-in shaders, through the typed calls (create_shader / create_pipeline).
//  - Vulkan: what vulkan-1.dll does - SPIR-V shaders (CubeShaders/*.vert, *.frag) translated to DXBC by the same translator the DLL carries (src/Vulkan/spirv.inc),
//    sent with the private-data escape (Kernel::escape: CreateShader with ShaderKind::Dxbc, CreatePipeline), so the DXBC front end and the escape ABI are exercised too.
// OpenGL goes to the kernel's GL instead (GlCubeRenderer.hpp); all three draw the same picture.
//
// A frame's time is split in two: "submit" is Kernel::render() (the stream is checked, queued and run on the adapter's device, which is done when the call returns) and
// "readback" is the lock that brings the picture to the CPU plus the copy out of it. Their sum is what CubeView shows as "render".
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/CubeMesh.hpp"
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

struct CubeFrameTimes {
  uint64_t submit_us = 0;     // Kernel::render()
  uint64_t readback_us = 0;   // lock + copy of the color target
  uint64_t total_us() const { return submit_us + readback_us; }
};

// The graphics API the cube is drawn through: which part of the kernel is exercised.
enum class CubeApi : uint32_t { Direct3D, Vulkan, OpenGL };
constexpr uint32_t kCubeApiCount = 3;
const char* cube_api_name(CubeApi api);

constexpr uint32_t kCubeLogoSize = 256;   // the texture is the logo at this size (its own)
constexpr uint32_t kCubeBackground = 0xFF10101A;   // the clear color (B8G8R8A8 word)

// What every renderer of the cube does, whatever API it draws through.
class ICubeRenderer {
 public:
  virtual CubeApi api() const = 0;
  virtual bool is_open() const = 0;
  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;
  // Whether a GPU draws the picture, and the name of what draws it.
  virtual bool accelerated() const = 0;
  virtual const char* device_name() const = 0;
  // Whether the faces can show the logo (known once open).
  virtual bool texture_available() const = 0;
  virtual bool last_textured() const = 0;
  // Draws one frame at `angle` (radians around the cube's turn) and copies it into picture(). `textured` is honoured when texture_available(). Without `read_back` the frame stays
  // on the GPU when the renderer can lend it (shareable(); picture() is then the frame before): show it with acquire_target(), or fetch_picture() to have it on the CPU after all.
  virtual bool render(float angle, bool textured, CubeFrameTimes* times, bool read_back) = 0;
  virtual bool shareable() const = 0;
  virtual bool acquire_target(graphics::IGraphicsDevice** device, graphics::IImage** image) = 0;
  virtual void release_target() = 0;
  virtual bool fetch_picture() = 0;
  // The last frame, B8G8R8A8.
  virtual const graphics::Surface& picture() const = 0;

 protected:
  ~ICubeRenderer() = default;
};

// The cube's geometry for both looks (the colored vertices have each face's own color) and the indices both share.
void build_cube_geometry(ArrayView<graphics::ColoredVertex> colored, ArrayView<graphics::TexturedVertex> textured, ArrayView<uint16_t> indices);
// The logo at kCubeLogoSize on the clear color, rows top first, 0xAARRGGBB. False if it cannot be decoded.
bool render_cube_logo(allocators::IAllocator& allocator, ArrayView<uint32_t> texels);

class CubeRenderer final : public ICubeRenderer {
 public:
  static constexpr uint32_t kLogoSize = kCubeLogoSize;

  // `pid` is the process the kernel knows this renderer as (opened here, closed with the renderer). `api` is Direct3D or Vulkan (OpenGL has its own renderer).
  CubeRenderer(allocators::IAllocator& allocator, dxgk::Kernel& kernel, dxgk::ProcessId pid, CubeApi api = CubeApi::Direct3D);
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
  CubeApi api() const override { return api_; }
  bool is_open() const override { return static_cast<bool>(state_); }
  uint32_t adapter_index() const { return adapter_index_; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  const dxgk::AdapterCaps& caps() const { return caps_; }
  bool accelerated() const override { return caps_.accelerated; }
  const char* device_name() const override { return caps_.name; }
  bool texture_available() const override { return texture_available_; }

  bool render(float angle, bool textured, CubeFrameTimes* times, bool read_back) override;
  bool render(float angle, bool textured, CubeFrameTimes* times) { return render(angle, textured, times, true); }
  // Whether the color target can be lent to a renderer on the adapter's device.
  bool shareable() const override { return shareable_; }
  // Lends the device and its image of the last frame's color target; release_target() when done with it (the next render() needs it back).
  bool acquire_target(graphics::IGraphicsDevice** device, graphics::IImage** image) override;
  void release_target() override;
  // The last frame, read back into picture() now (what render() does unless told not to).
  bool fetch_picture() override { return state_ && read_back(); }
  const graphics::Surface& picture() const override { return picture_; }
  bool last_textured() const override { return last_textured_; }

 private:
  struct State;
  bool create_pipeline(bool textured, dxgk::Handle* pipeline);
  bool create_spirv_pipeline(bool textured, dxgk::Handle* pipeline);
  bool escape_create_shader(ConstByteView dxbc, dxgk::Handle* shader);
  bool create_buffer(uint32_t flags, uint64_t size, dxgk::Handle* out);
  bool write(dxgk::Handle allocation, ConstByteView bytes);
  bool create_geometry();
  bool create_logo();
  bool read_back();

  allocators::IAllocator& allocator_;
  dxgk::Kernel& kernel_;
  dxgk::ProcessId pid_;
  CubeApi api_;
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
