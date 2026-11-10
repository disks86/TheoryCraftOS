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

// The 3D test's cube drawn the way an OpenGL program is: through the kernel's GL (src/OpenGl), with the calls opengl32.dll's NtTcosOpenGl makes - a context and a window surface
// of the renderer's own process in the GlService, the GL command stream (GlStream.hpp: the words the library's entry points write) run by GlService::execute, the frame
// shown with a swap and read with read_front (what the window gets). So the kernel side of OpenGL - the stream decoder, GlApi, the GLSL front end, the rasterizer and the
// surface - is what the frames exercise. The program's memory the stream points into is the renderer's own buffers, reached through ClientMemory (below), never raw addresses.
//
// The kernel's GL draws on the CPU, so the frame is always read back ("readback" is the swap and read_front).
#pragma once
#include "Desktop/CubeRenderer.hpp"
#include "OpenGl/GlService.hpp"
#include "Utility/ByteBuffer.hpp"

namespace desktop {

class GlCubeRenderer final : public ICubeRenderer {
 public:
  // `pid` is the process the GL knows this renderer as; everything it holds there goes when the renderer closes.
  GlCubeRenderer(allocators::IAllocator& allocator, opengl::GlService& service, uint64_t pid);
  ~GlCubeRenderer();
  GlCubeRenderer(const GlCubeRenderer&) = delete;
  GlCubeRenderer& operator=(const GlCubeRenderer&) = delete;

  // The cube's transform for GL's clip space (+y up, the picture's top row last in GL's own buffer).
  static graphics::Mat4 transform(float angle, float aspect);

  // A context, a surface of this size, the programs, buffers and the logo texture. False (and nothing held) if the GL refuses anything essential; the logo is best effort.
  bool open(uint32_t width, uint32_t height);
  void close();

  CubeApi api() const override { return CubeApi::OpenGL; }
  bool is_open() const override { return open_; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  bool accelerated() const override { return false; }
  const char* device_name() const override { return "kernel GL rasterizer"; }
  bool texture_available() const override { return texture_available_; }
  bool last_textured() const override { return last_textured_; }
  bool render(float angle, bool textured, CubeFrameTimes* times, bool read_back) override;
  bool render(float angle, bool textured, CubeFrameTimes* times) { return render(angle, textured, times, true); }
  bool shareable() const override { return false; }
  bool acquire_target(graphics::IGraphicsDevice**, graphics::IImage**) override { return false; }
  void release_target() override {}
  bool fetch_picture() override { return open_ && read_front(); }
  const graphics::Surface& picture() const override { return picture_; }

  // The GL's own words about the last shader or program that failed to build (empty when none did).
  StringView build_log() const { return StringView(log_.view()); }

  // The memory of "the program" as the GL stream sees it: views registered with the renderer, each at an address of its own (a made-up one: the numbers only select a
  // view and an offset in it, and anything outside the views is refused, as an unmapped page would be).
  class ClientMemory final : public opengl::IClientMemory {
   public:
    static constexpr uint32_t kMaxRegions = 12;
    // The address the GL is to be given for the view (0 when no slot is free).
    uint64_t add(ConstByteView bytes);
    uint64_t add_writable(ByteView bytes);
    void clear() { count_ = 0; }
    bool read(uint64_t address, ByteView out) override;
    bool write(uint64_t address, ConstByteView data) override;
    uint64_t allocate(uint64_t) override { return 0; }   // (the cube asks for nothing the GL would keep for it)
    void release(uint64_t, uint64_t) override {}

   private:
    struct Region {
      ConstByteView bytes;
      ByteView writable;   // empty: read only
    };
    static constexpr uint64_t kSpacing = uint64_t{1} << 32;   // region n starts at (n + 1) << 32
    bool find(uint64_t address, uint64_t size, uint32_t* region, uint64_t* offset) const;
    Region regions_[kMaxRegions];
    uint32_t count_ = 0;
  };

 private:
  class Commands;
  using C = Commands;   // (the argument makers read short: C::u, C::i, C::f, C::p)
  bool build_program(StringView vertex, StringView fragment, bool textured, uint32_t* program, int32_t* mvp_location);
  bool create_buffers();
  bool create_logo();
  uint64_t submit(Commands& commands, bool* ok);
  bool read_front();

  allocators::IAllocator& allocator_;
  opengl::GlService& service_;
  uint64_t pid_;
  bool open_ = false;
  uint32_t context_ = 0;
  uint32_t width_ = 0, height_ = 0;
  uint32_t colored_program_ = 0, textured_program_ = 0;
  int32_t colored_mvp_ = -1, textured_mvp_ = -1;
  uint32_t colored_vertices_ = 0, textured_vertices_ = 0, indices_ = 0, logo_ = 0;
  bool texture_available_ = false, last_textured_ = false;
  ClientMemory memory_;
  ByteBuffer log_;
  graphics::Surface picture_;
  float mvp_[16] = {};
};

}  // namespace desktop
