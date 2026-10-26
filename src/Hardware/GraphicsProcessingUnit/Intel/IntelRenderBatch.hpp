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
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderCommands.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {
namespace gen9_3d {

// Builds the batch image of one 3D job for the Gen9 render engine (Skylake to Comet Lake graphics, which includes the Kaby Lake-G HD Graphics 630): the commands first,
// then (from kStateOffset on) the state they point to, in one buffer. The image is what the driver copies to the batch buffer, so the buffer's address in the context's
// address space is the base of the dynamic, surface and instruction state, and every state pointer is an offset into the image.
//
// The command sequence is that of the intel-gpu-tools Gen9 render copy (lib/rendercopy_gen9.c), which runs on hardware: everything the 3D pipeline has that is not used
// is switched off, the vertex shader is off (the vertices are the final screen positions), the clipper is off (a primitive is clipped to the drawing rectangle by the
// windower), and the pixel shader is one of three small kernels: copy a texel, write a constant colour, write an interpolated colour. There is no depth buffer.
// The state is complete for every job (the job does not rely on what an earlier one left).
//
// Positions are window coordinates in pixels (+Y down), and colours are interpolated linearly in screen space (the vertices carry w = 1): perspective-correct
// interpolation needs the clipper and the viewport transform, which are off. A caller with a perspective projection has to divide by w itself.

constexpr uint32_t kStateOffset = 4096;         // commands must end before this; state starts here
constexpr uint32_t kMaxImageBytes = 64 * 1024;  // what a batch buffer holds

enum class SurfaceFormat : uint16_t { B8G8R8A8Unorm = 0x0C0, R8G8B8A8Unorm = 0x0C7 };

// A linear 32-bit surface in the context's address space.
struct Surface {
  uint64_t gpu_address = 0;   // 64-byte aligned
  uint32_t width = 0, height = 0;
  uint32_t pitch = 0;         // bytes per row, a multiple of 64, at least width * 4
  SurfaceFormat format = SurfaceFormat::B8G8R8A8Unorm;
  bool valid() const;
};

// x0, y0 inclusive; x1, y1 exclusive.
struct Rect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

enum class Blend : uint8_t { None, SourceOver };

// A vertex as the kernels read it: the window position (x, y in pixels, z unused, w = 1) and the colour.
struct Vertex {
  float position[4];
  float color[4];
};

// What a draw does to the target: where, and whether it blends with what is there. Drawing is clipped to `clip` and the target.
struct DrawState {
  Surface target;
  Blend blend = Blend::None;
  Rect clip;   // empty: the whole target
};

class RenderBatch {
 public:
  // `image` is the buffer the batch is built in (a multiple of 4 KiB, at least kStateOffset + 4 KiB, at most kMaxImageBytes) whose first byte the engine addresses as
  // `image_gpu_address` (4 KiB aligned).
  RenderBatch(ArrayView<uint32_t> image, uint32_t image_gpu_address) : image_(image), base_(image_gpu_address) {}

  // How many vertices a triangle job can hold.
  uint32_t max_vertices() const;

  // The next four build a complete job, replacing what was in the image; false (and an unusable image) if the arguments are not valid or do not fit.
  // A solid rectangle (a clear is one covering the target).
  bool fill_rect(const DrawState& draw, const Rect& rect, const float rgba[4]);
  // Triangles (three vertices each). `gouraud`: the kernel interpolates the vertices' colours; else every pixel gets `flat_rgba`.
  bool triangles(const DrawState& draw, ArrayView<const Vertex> vertices, bool gouraud, const float flat_rgba[4]);
  // A copy of `src_rect` of `src` to `dst` at (dst_x, dst_y), nearest-neighbour, clipped to `dst`. The texture is read as RGBA whatever its byte order.
  bool copy(const Surface& dst, const Surface& src, const Rect& src_rect, int32_t dst_x, int32_t dst_y);

  // The prefix of the image to hand to the driver: commands, end marker, state.
  uint32_t dwords() const { return used_; }

 private:
  ArrayView<uint32_t> image_;
  uint32_t base_;
  uint32_t used_ = 0;
};

// --- The pixel kernels (Gen9 EU instructions, four dwords each) ---------------------------------------------------------------------------------

// Samples the texture at the interpolated (u, v) and writes the texel to render target 0: the kernel of the intel-gpu-tools Gen9 render copy, unchanged.
ArrayView<const uint32_t> copy_kernel();
// Writes `rgba` to render target 0 for every pixel (the write-a-constant kernel of the same test with its immediates replaced). `out` is exactly kFlatKernelDwords long.
constexpr uint32_t kFlatKernelDwords = 9 * 4;
bool flat_kernel(ArrayView<uint32_t> out, const float rgba[4]);
// Interpolates attribute 0 (four components, set up from g6) and writes it to render target 0.
ArrayView<const uint32_t> gouraud_kernel();

// The bits of a float.
uint32_t float_bits(float value);

}  // namespace gen9_3d
}  // namespace intel_gt
}  // namespace hardware
