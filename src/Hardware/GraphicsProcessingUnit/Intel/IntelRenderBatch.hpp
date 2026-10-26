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
// windower), and the pixel shader is one of three small kernels: copy a texel, write a constant colour, write an interpolated colour. The depth test is fixed
// function and needs no kernel. The state is complete for every job (the job does not rely on what an earlier one left).
//
// Positions are window coordinates in pixels (+Y down) with z the window depth (0 to 1). The clipper and the viewport transform are off, so the vertices carry what
// the clipper would hand the setup stage: the fourth component is 1/w of the clip-space position (1 for an orthographic scene), from which the setup interpolates
// colours perspective-correct. That the hardware reads it so is the documented behaviour of the clipper's output; it has not been seen on a GPU.

constexpr uint32_t kStateOffset = 4096;         // commands must end before this; state starts here
constexpr uint32_t kMaxImageBytes = 64 * 1024;  // what a batch buffer holds

enum class SurfaceFormat : uint16_t { B8G8R8A8Unorm = 0x0C0, R8G8B8A8Unorm = 0x0C7 };

constexpr uint32_t kMaxSurfaceLevels = 13;   // 4096 x 4096 down to 1 x 1

// Where the levels of a 2D surface lie in it, and how many bytes it takes. A linear surface has the one level (pitch a multiple of 64 bytes). A Y-tiled surface (tiles of 128
// bytes by 32 rows in 4 KiB, laid out row by row of tiles; inside a tile, columns of 16 bytes run down 32 rows, the columns side by side) holds a mip chain the way the
// hardware lays it out (the Skylake PRM's 2D surface layout, as Mesa's isl computes it for 32-bit texels and no auxiliary surface): every level's size rounded up to a
// multiple of 4 texels each way; level 0 at the top left, level 1 below it, level 2 to the right of level 1 and every level after that below the one before. Gen9 has no bit 6
// swizzling of tiled memory, so the CPU may address it as laid out here (the i915 driver programs I915_BIT_6_SWIZZLE_NONE for Gen8 and later).
struct MipLayout {
  uint32_t levels = 0;
  uint32_t pitch = 0;        // bytes per row of the surface (of tiles, when tiled)
  uint32_t rows = 0;         // rows the memory holds: a multiple of 32 when tiled
  uint32_t slice_rows = 0;   // rows the levels take (the surface's QPitch counts them): the larger of the two columns of levels, a multiple of 4
  uint32_t x[kMaxSurfaceLevels] = {}, y[kMaxSurfaceLevels] = {};   // the texel of the surface where each level starts
  uint32_t w[kMaxSurfaceLevels] = {}, h[kMaxSurfaceLevels] = {};   // the size of each level in texels
  uint64_t bytes() const { return uint64_t{pitch} * rows; }
};
// False for an empty or too big surface, more levels than the size has (down to 1 x 1 at most), or a mip chain that is not tiled.
bool mip_layout(uint32_t width, uint32_t height, uint32_t levels, bool tiled, MipLayout* out);
// The byte offset from the start of a Y-tiled surface of the byte at `x_byte` in row `y` (a 32-bit texel at x is 4 * x).
uint64_t y_tile_offset(uint32_t pitch, uint32_t x_byte, uint32_t y);

// A 32-bit surface in the context's address space: linear, or Y-tiled (and then possibly with a mip chain).
struct Surface {
  uint64_t gpu_address = 0;   // 64-byte aligned (4 KiB when tiled)
  uint32_t width = 0, height = 0;   // of level 0
  uint32_t pitch = 0;         // bytes per row, a multiple of 64 (128 when tiled), at least width * 4
  SurfaceFormat format = SurfaceFormat::B8G8R8A8Unorm;
  bool tiled_y = false;
  uint32_t levels = 1;        // mip levels (more than one needs tiled_y); a sampled surface only
  uint32_t slice_rows = 0;    // MipLayout::slice_rows; 0 means the height rounded up to 4
  bool valid() const;
};

// x0, y0 inclusive; x1, y1 exclusive.
struct Rect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

enum class Blend : uint8_t { None, SourceOver };

// A vertex as the kernels read it: the window position (x, y in pixels, z the depth, w = 1/w of the clip-space position) and the colour.
struct Vertex {
  float position[4];
  float color[4];
};

// A depth buffer of 32-bit floats. The hardware only has Y-tiled depth buffers, so the memory is opaque to the CPU: it is made by `bytes_for()` bytes of anything
// (a 1.0 in every word means nothing is in front), its rows are `pitch()` bytes apart in tile rows of 32, and the base is 4 KiB aligned.
struct DepthBuffer {
  uint64_t gpu_address = 0;
  uint32_t width = 0, height = 0;
  static uint32_t pitch_for(uint32_t width) { return (width * 4 + 127) / 128 * 128; }
  static uint64_t bytes_for(uint32_t width, uint32_t height) { return uint64_t{pitch_for(width)} * ((height + 31) / 32 * 32); }
  bool attached() const { return width != 0 || height != 0; }
  uint32_t pitch() const { return pitch_for(width); }
  bool valid() const;
};

// A stencil buffer of 8-bit values. The hardware only has W-tiled stencil buffers, so the memory is opaque to the CPU: it is made by `bytes_for()` bytes of anything (zeros
// are zero stencil values in any layout), the base is 4 KiB aligned, and a draw whose stencil test is on needs it (the same size as the target, with a depth buffer attached). A tile
// of a W-tiled surface is 64 x 64 stencil values in 4096 bytes, and the pitch the hardware is told counts the 128 bytes a tile is wide on its physical rows: twice the aligned width.
struct StencilBuffer {
  uint64_t gpu_address = 0;
  uint32_t width = 0, height = 0;
  static uint32_t aligned_width(uint32_t width) { return (width + 63) / 64 * 64; }
  static uint32_t pitch_for(uint32_t width) { return aligned_width(width) * 2; }
  static uint64_t bytes_for(uint32_t width, uint32_t height) { return uint64_t{aligned_width(width)} * ((height + 63) / 64 * 64); }
  bool attached() const { return width != 0 || height != 0; }
  uint32_t pitch() const { return pitch_for(width); }
  bool valid() const;
};

// The values of the hardware's compare functions and stencil operations (3D_Compare_Function, 3D_Stencil_Operation).
enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilAction : uint8_t { Keep, Zero, Replace, IncrementSaturate, DecrementSaturate, Increment, Decrement, Invert };

// The stencil test of a draw: a pixel passes when (reference & compare_mask) <func> (stored & compare_mask); the stored value then becomes (stored & ~write_mask) | (the action's
// result & write_mask), the action being `fail` when the test fails, `depth_fail` when it passes and the depth test fails, `pass` when both pass.
struct StencilTest {
  bool enable = false;
  CompareFunc func = CompareFunc::Always;
  StencilAction fail = StencilAction::Keep, depth_fail = StencilAction::Keep, pass = StencilAction::Keep;
  uint8_t compare_mask = 0xFF, write_mask = 0xFF, reference = 0;
};

// What a draw does to the target: where, and whether it blends with what is there. Drawing is clipped to `clip` and the target. With a depth buffer attached
// (the size of the target), `depth_test` passes a pixel that is closer than or as close as what is there, and `depth_write` stores its depth.
struct DrawState {
  Surface target;
  Blend blend = Blend::None;
  Rect clip;   // empty: the whole target
  DepthBuffer depth;
  bool depth_test = false;
  bool depth_write = false;
  StencilBuffer stencil;
  StencilTest stencil_test;   // needs `stencil` (and `depth`) attached
};

// How a bound texture is read by textured_triangles: the filter (nearest, or bilinear), and for each axis whether it wraps around or takes the edge texel. With a mip chain
// the mip filter picks the level from the derivatives of the coordinates (none: level 0 only) and the lod fields bound it (the sampler state's bias, and its minimum and
// maximum, which the caller keeps within the levels the surface has).
enum class MipMode : uint8_t { None, Nearest, Linear };
struct Sampler {
  bool linear = false;
  bool repeat_u = false, repeat_v = false;
  MipMode mip = MipMode::None;
  float lod_bias = 0.0f, min_lod = 0.0f, max_lod = 0.0f;
};

class RenderBatch {
 public:
  // `image` is the buffer the batch is built in (a multiple of 4 KiB, at least kStateOffset + 4 KiB, at most kMaxImageBytes) whose first byte the engine addresses as
  // `image_gpu_address` (4 KiB aligned).
  RenderBatch(ArrayView<uint32_t> image, uint32_t image_gpu_address) : image_(image), base_(image_gpu_address) {}

  // How many vertices a triangle job can hold.
  uint32_t max_vertices() const;

  // Sets every pixel of the depth buffer of `draw` to `value` (0 to 1), leaving the colours alone; a job of its own.
  bool clear_depth(const DrawState& draw, float value);
  // Sets every value of the stencil buffer of `draw` to `value`, leaving colours and depths alone; a job of its own.
  bool clear_stencil(const DrawState& draw, uint8_t value);
  // The next four build a complete job, replacing what was in the image; false (and an unusable image) if the arguments are not valid or do not fit.
  // A solid rectangle (a clear is one covering the target).
  bool fill_rect(const DrawState& draw, const Rect& rect, const float rgba[4]);
  // Triangles (three vertices each). `gouraud`: the kernel interpolates the vertices' colours; else every pixel gets `flat_rgba`.
  bool triangles(const DrawState& draw, ArrayView<const Vertex> vertices, bool gouraud, const float flat_rgba[4]);
  // Textured triangles (three vertices each): the vertices' color[0] and color[1] are the texture coordinates u and v, normalised (0..1 spans the texture; (0, 0) is its top left
  // corner), interpolated perspective-correct like colours, and the pixel kernel is the copy kernel, which samples `texture` with `sampler` and writes the texel. color[2]
  // and color[3] are not used. The draw's blend applies to the texel's alpha. `texture` must not be the target.
  bool textured_triangles(const DrawState& draw, ArrayView<const Vertex> vertices, const Surface& texture, const Sampler& sampler);
  // A copy of `src_rect` of `src` to `dst` at (dst_x, dst_y), nearest-neighbour, clipped to `dst`. The texture is read as RGBA whatever its byte order.
  bool copy(const Surface& dst, const Surface& src, const Rect& src_rect, int32_t dst_x, int32_t dst_y);

  // The prefix of the image to hand to the driver: commands, end marker, state.
  uint32_t dwords() const { return used_; }

 private:
  ArrayView<uint32_t> image_;
  uint32_t base_;
  bool fill(const DrawState& draw, const Rect& rect, const float rgba[4], float z, bool depth_only, bool mask_color = false);
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
