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

// The 3D hardware behind WebGL and WebGPU. When the machine has a GPU whose 3D engine the OS can drive (graphics::SystemGraphics::accelerated()), the OS installs
// an IAccelerator here and the canvas contexts hand it the draws it can run (shaders that translate to TGSI, see TgsiTranslator.hpp, and state the device has);
// every other draw, and every context when there is no accelerator, runs on the software rasterizer in this directory. A context never depends on the choice
// for correctness: the accelerator may refuse any call, and the context then does that work itself.
//
// An accelerator owns render targets (a color image and a depth image). The picture stays on the device between draws - nothing is read back per draw - and
// the context reads it back with read_color() only when something on the CPU needs it (presenting the canvas, readPixels, a draw the device cannot do).
// The device's color image can only be read, not written, so a context uses a target only while the target is at least as new as its own copy of the image.
//
// Calls come from whichever thread runs the page's script; an implementation serialises them.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace webgl {

// How a texture is read: nearest or linear filtering, and for each axis wrap around or take the edge texel.
struct AccelSampler {
  bool linear = false;            // (minification's filter within a level; magnification's too unless separate_mag)
  bool repeat_s = true, repeat_t = true;
  // A mipmapped image (create_image with levels): the filter between levels (0 none: level 0 only, 1 the nearest level, 2 linear between two) and the levels' range.
  uint8_t mip = 0;
  float min_lod = 0, max_lod = 1000, lod_bias = 0;
  bool mirror_s = false, mirror_t = false;   // (with repeat: GL_MIRRORED_REPEAT)
  // AccelLimits::sampler_state: in place of the above for an axis, the border colour outside the image (GL_CLAMP_TO_BORDER; `border`, the channels as the texture
  // reads them) or mirror once then the edge (GL_MIRROR_CLAMP_TO_EDGE); magnification by its own filter; anisotropic filtering of up to `anisotropy` lookups.
  bool border_s = false, border_t = false;
  bool mirror_once_s = false, mirror_once_t = false;
  float border[4] = {0, 0, 0, 0};
  bool separate_mag = false, mag_linear = false;
  uint8_t anisotropy = 1;
};
constexpr uint32_t kAccelMaxTextures = 16;

// What an accelerator's programs may have (IAccelerator::limits): CONST rows, vertex attributes, varyings and textures. The defaults are what every device has.
struct AccelLimits {
  uint32_t const_rows = 8, attributes = 4, varyings = 4, textures = 2;
  bool blend_state = false;   // AccelState's custom blending, colour mask and depth functions
  bool lines = false;         // AccelState::topology lines and points
  bool geometry = false;      // programs with a geometry stage (AccelStages::gs), and the adjacency topologies
  bool tessellation = false;  // programs with tessellation stages (AccelStages::tcs / tes), and patches
  bool clip_distances = false;  // AccelState::clip_mask: the CLIPDIST outputs of the last stage cut the primitives
  bool float_textures = false;  // create_texture_float (what a depth texture is sent as)
  uint32_t control_varyings = 0;  // tessellation: the registers per control point from the control to the evaluation stage (0: `varyings`)
  bool color_upload = false;      // write_color
  // Device images (create_image): what draws render into (draw_images) and sample later, so a framebuffer object's pictures stay on the device. 0 colours: none.
  uint32_t image_colors = 0;      // colour images one draw_images may render into at once
  bool float_images = false;      // AccelFormat::Rgba16F / Rgba32F images
  bool srgb_images = false;       // AccelFormat::Rgba8Srgb images
  bool sampled_depth = false;     // a Depth image can be sampled too (its depth in R)
  bool mip_images = false;        // create_image with more than one level (sampled only)
  bool instancing = false;        // AccelState::instances and per-instance attributes (AccelState::instanced)
  bool independent_blend = false; // AccelState::per_target
  bool layered_textures = false;  // create_layered: 3D, cube and 2D array textures
  bool depth_bias = false;        // AccelState::depth_bias_*
  bool stencil = false;           // AccelFormat::DepthStencil images and AccelState::stencil_*
  // write_image / read_image of Depth and DepthStencil images, and write_depth / read_depth of a target: the device's depth (and stencil) can be written and read,
  // so a draw the CPU makes in between does not leave the device's depth out of date
  bool depth_transfer = false;
  bool sampler_state = false;     // AccelSampler's border, mirror-once, separate magnification and anisotropy
  uint8_t max_anisotropy = 1;
  // create_texture_levels: sampled textures of every kind with levels (and their formats: colour, float, blocks the device takes - takes_blocks)
  bool texture_levels = false;
};

// The kinds of device image (create_image). Rgba8 (Srgb: sRGB encoded) has 8-bit channels; Rgba16F and Rgba32F floats; Depth a float depth image. Pixels go in and out
// as w * h * 4 bytes R, G, B, A (Rgba8, Rgba8Srgb), w * h * 16 bytes of little-endian floats (Rgba16F, Rgba32F) - the row of t = 0 (window y 0) first.
enum class AccelFormat : uint8_t { Rgba8, Rgba8Srgb, Rgba16F, Rgba32F, Depth, DepthStencil };   // (DepthStencil: AccelLimits::stencil, a depth image with an 8-bit stencil plane)
enum class AccelKind : uint8_t { Volume, Cube, Array, Plain };   // (create_layered; Plain: a 2D texture - create_texture_levels)
// Block-compressed formats a device may sample as they are (IAccelerator::takes_blocks): S3TC/DXT1 (BC1) with alpha, DXT3, DXT5, RGTC1/2 unsigned and signed, BPTC.
enum class AccelBlocks : uint8_t { None, Bc1, Bc1Srgb, Bc2, Bc2Srgb, Bc3, Bc3Srgb, Bc4U, Bc4S, Bc5U, Bc5S, Bc6U, Bc6S, Bc7, Bc7Srgb };
// A sampled texture (create_texture_levels): `kind`, w x h, `layers` (a volume's slices at level 0, a cube's 6, an array's layers, 1 for Plain), `levels` levels, of
// `format` (Rgba8, Rgba8Srgb, Rgba16F, Rgba32F) or, when `blocks` is not None, of those blocks.
struct AccelTextureDesc {
  uint32_t w = 0, h = 0, layers = 1, levels = 1;
  AccelKind kind = AccelKind::Plain;
  AccelFormat format = AccelFormat::Rgba8;
  AccelBlocks blocks = AccelBlocks::None;
};
constexpr uint32_t kAccelMaxColors = 8;
// The images a draw renders into (draw_images, clear_images): colours[0 .. color_count - 1], fragment output n going to colors[n], and a Depth image (0: none). All
// have one size. color_count may be 0 (a depth-only draw).
struct AccelAttachments {
  uint32_t colors[kAccelMaxColors] = {};
  uint32_t color_count = 0;
  uint32_t depth = 0;
  // clear_images of a DepthStencil image: its stencil plane to this value (0 .. 255; -1: left as it is)
  int32_t stencil_clear = -1;
};

// The stages of a program between the vertex and the fragment shader (create_program_stages), as TGSI text, and what the device must know of them. The numbers are the
// device's: graphics::GeometryPrimitive, GeometryTopology, TessDomain, TessPartitioning and TessOutput (IGraphicsDevice.hpp).
struct AccelStages {
  const char* gs = nullptr;    // a geometry shader, or nullptr
  uint8_t gs_input = 2, gs_output = 2;   // GeometryPrimitive (2 triangles), GeometryTopology (2 a triangle strip)
  uint32_t gs_max_vertices = 0, gs_invocations = 1;
  const char* tcs = nullptr;   // the control and evaluation shaders: both or neither
  const char* tes = nullptr;
  uint32_t patch_vertices = 0;   // the vertices of a patch the draws make (the control shader's input), and of the control shader's output patch
  uint32_t tcs_vertices = 0;
  uint8_t tess_domain = 1, tess_partitioning = 0, tess_output = 1;   // TessDomain (1 triangles), TessPartitioning (0 integer), TessOutput (1 counter-clockwise triangles)
};

struct AccelState {
  // Window coordinates as in GL: origin at the bottom left, rows counted from there (the device image is stored the same way, see GlAccel.cpp).
  float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  float depth_near = 0, depth_far = 1;
  bool scissor = false;
  int32_t sc_x = 0, sc_y = 0;
  uint32_t sc_w = 0, sc_h = 0;
  uint8_t cull = 0;          // 0 none, 1 front faces culled, 2 back faces culled
  bool front_ccw = true;     // counter-clockwise in GL window coordinates
  bool depth_test = false;   // passes if closer or equal
  bool depth_write = false;
  bool blend = false;        // source alpha over one minus source alpha (custom_blend false)
  // AccelLimits::blend_state: blending by these factors and equations (enabled by `blend`), the channels written (bit 0 red .. bit 3 alpha) and what the depth test
  // compares with. The numbers are the device's (graphics::BlendFactor, BlendOp, CompareOp in IGraphicsDevice.hpp); custom_blend false is source over destination,
  // every channel written and a depth test that passes when closer or equal.
  bool custom_blend = false;
  uint8_t src_rgb = 1, dst_rgb = 0, src_alpha = 1, dst_alpha = 0;   // graphics::BlendFactor
  uint8_t op_rgb = 0, op_alpha = 0;                                 // graphics::BlendOp
  uint8_t color_mask = 0xF;
  uint8_t depth_compare = 3;                                        // graphics::CompareOp (LessEqual)
  // AccelLimits::lines: what the vertices make, 0 a list of triangles, 1 of lines, 2 of points (one pixel wide); AccelLimits::tessellation: 3 a list of patches of
  // patch_vertices vertices each; AccelLimits::geometry: 4 a list of lines with adjacency (4 vertices each), 5 of triangles with adjacency (6 each). A program with a
  // geometry stage takes the primitives its gs_input names, one with tessellation stages only patches.
  uint8_t topology = 0;
  uint32_t patch_vertices = 0;
  // AccelLimits::clip_distances: the clip distances the program's last stage writes (CLIPDIST[0] .xyzw 0 .. 3, CLIPDIST[1] 4 .. 7) that cut its primitives, bit i distance i.
  uint8_t clip_mask = 0;
  // The textures the program samples, by sampler index (SAMP[i] of its TGSI): texture handles from create_texture() and how each is read. texture_count is what the
  // program was created with.
  uint32_t texture_count = 0;
  uint32_t textures[kAccelMaxTextures] = {};
  AccelSampler samplers[kAccelMaxTextures] = {};
  // AccelLimits::instancing: the instances of the draw (each runs the vertices again), and the attributes (bit i: IN[i]) that step per instance instead - those take
  // their values from the vertex data after the per-vertex ones: instances x the per-instance attributes, float4 each, in IN[] order.
  uint32_t instances = 1;
  uint32_t instanced = 0;
  // AccelLimits::independent_blend, with custom_blend: each colour image of draw_images blends and masks as targets[n] says (target 0's also in the fields above).
  struct TargetBlend {
    bool blend = false;
    uint8_t src_rgb = 1, dst_rgb = 0, src_alpha = 1, dst_alpha = 0, op_rgb = 0, op_alpha = 0;
    uint8_t color_mask = 0xF;
  };
  bool per_target = false;
  TargetBlend targets[kAccelMaxColors] = {};
  // AccelLimits::depth_bias: glPolygonOffset's factor and units, applied to the primitives `depth_bias_modes` says (bit 0 filled, 1 lines, 2 points).
  float depth_bias_factor = 0, depth_bias_units = 0;
  uint8_t depth_bias_modes = 0;
  // AccelLimits::stencil, drawing into a DepthStencil image: the stencil test, one state for both faces (graphics::CompareOp and StencilOp numbers)
  bool stencil_test = false;
  uint8_t stencil_compare = 7, stencil_fail = 0, stencil_depth_fail = 0, stencil_pass = 0;
  uint8_t stencil_reference = 0, stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
};

class IAccelerator {
 public:
  virtual const char* name() const = 0;
  virtual AccelLimits limits() const { return AccelLimits(); }

  // Compiles a TGSI vertex/fragment pair: `attributes` float4 inputs (IN[0..]), `varyings` float4 outputs, and `textures` 2D samplers (SAMP[0..] of the fragment
  // shader, at most limits().textures). 0: the device cannot run it.
  virtual uint32_t create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures) = 0;
  // The same with geometry or tessellation stages (`varyings`: the fragment shader's inputs). The default takes only programs without them.
  virtual uint32_t create_program_stages(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures, const AccelStages& stages) {
    if (stages.gs || stages.tcs || stages.tes) return 0;
    return create_program(vs_tgsi, fs_tgsi, attributes, varyings, textures);
  }
  virtual void destroy_program(uint32_t program) = 0;

  // A texture of w x h texels (RGBA8, straight alpha, no mipmaps). 0: the device cannot have one (the default: programs with samplers are then refused).
  virtual uint32_t create_texture(uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    return 0;
  }
  // A texture of w x h texels of four 32-bit floats (AccelLimits::float_textures), read exactly as written with nearest filtering. 0: the device cannot have one.
  virtual uint32_t create_texture_float(uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    return 0;
  }
  virtual void destroy_texture(uint32_t texture) { (void)texture; }
  // Replaces the contents: w * h * 4 bytes, R, G, B, A of each texel, the first row (t = 0) first.
  virtual bool write_texture(uint32_t texture, ConstByteView rgba) {
    (void)texture;
    (void)rgba;
    return false;
  }
  // The same for a float texture: w * h * 16 bytes, R, G, B, A of each texel as little-endian floats.
  virtual bool write_texture_float(uint32_t texture, ConstByteView rgba32f) {
    (void)texture;
    (void)rgba32f;
    return false;
  }

  // A render target of w x h pixels (RGBA8 color, float depth). 0 on failure.
  virtual uint32_t create_target(uint32_t w, uint32_t h) = 0;
  virtual void destroy_target(uint32_t target) = 0;
  // Clears color (to rgba) and/or depth (to `depth`).
  virtual bool clear(uint32_t target, bool color, const float rgba[4], bool depth, float depth_value) = 0;
  // Draws `vertex_count` vertices (a list of AccelState::topology: triangles by default) of `attributes` float4 each from `vertices`, with `constants` (a multiple of 16 bytes: the CONST rows).
  virtual bool draw(uint32_t target, uint32_t program, const AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) = 0;
  // Copies the color image to `rgba` (width * height * 4 bytes: R, G, B, A of each pixel), the rows in the order the GL window has them - the bottom row first.
  virtual bool read_color(uint32_t target, ByteView rgba) = 0;
  // The other way: the color image becomes `rgba` (as read_color gives it, the bottom row first), its depth is left as it is. False if the device cannot (the default):
  // the context then keeps drawing on the CPU until the next clear.
  virtual bool write_color(uint32_t target, ConstByteView rgba) {
    (void)target;
    (void)rgba;
    return false;
  }
  // AccelLimits::depth_transfer: the depth image of a target as 4-byte floats (little-endian, in 0 .. 1), the rows in read_color's order. False if the device cannot (the default).
  virtual bool write_depth(uint32_t target, ConstByteView depth) {
    (void)target, (void)depth;
    return false;
  }
  virtual bool read_depth(uint32_t target, ByteView depth) {
    (void)target, (void)depth;
    return false;
  }

  // --- Device images (AccelLimits::image_colors). An image is a texture handle (AccelState::textures may name it) that draws can also render into.
  // A w x h image of `format` with `levels` mipmap levels (more than 1: AccelLimits::mip_images, Rgba8 / Rgba8Srgb, sampled only - not rendered into). 0: the device cannot.
  virtual uint32_t create_image(uint32_t w, uint32_t h, AccelFormat format, uint32_t levels = 1) {
    (void)w, (void)h, (void)format, (void)levels;
    return 0;
  }
  // Replaces level `level` (its size: w >> level, h >> level, at least 1) with `pixels` (as AccelFormat says). A Depth image (AccelLimits::depth_transfer only, level 0) takes
  // 4 bytes a texel - the depth as a little-endian float -, a DepthStencil image 8 - the depth, then the stencil in a little-endian word (0 .. 255).
  virtual bool write_image(uint32_t image, uint32_t level, ConstByteView pixels) {
    (void)image, (void)level, (void)pixels;
    return false;
  }
  // Copies level 0 of an image to `pixels` (as AccelFormat says; a Depth or DepthStencil image as write_image takes it, with AccelLimits::depth_transfer only).
  virtual bool read_image(uint32_t image, ByteView pixels) {
    (void)image, (void)pixels;
    return false;
  }
  // A sampled texture of layers (AccelLimits::layered_textures): a volume of `layers` slices, a cube (six faces +X, -X, +Y, -Y, +Z, -Z; `layers` is 6), or an
  // array of `layers` layers, w x h each, 8-bit RGBA, one level. Its layers are filled by write_layer (w * h * 4 bytes, the row of t = 0 first). 0: the device cannot.
  virtual uint32_t create_layered(uint32_t w, uint32_t h, uint32_t layers, AccelKind kind) {
    (void)w, (void)h, (void)layers, (void)kind;
    return 0;
  }
  // AccelLimits::texture_levels: a texture as `desc` says (0: the device cannot), filled by write_level: one layer (a volume's slice of that level, a cube's face, an
  // array's layer; 0 for Plain) of level `level`, the row of t = 0 first - 4 bytes R, G, B, A a texel (Rgba8, Rgba8Srgb), 16 bytes of floats (Rgba16F, Rgba32F), or the
  // blocks, rows of blocks from t = 0. It is destroyed with destroy_texture.
  virtual bool takes_blocks(AccelBlocks blocks) const {
    (void)blocks;
    return false;
  }
  virtual uint32_t create_texture_levels(const AccelTextureDesc& desc) {
    (void)desc;
    return 0;
  }
  virtual bool write_level(uint32_t texture, uint32_t level, uint32_t layer, ConstByteView data) {
    (void)texture, (void)level, (void)layer, (void)data;
    return false;
  }
  virtual bool write_layer(uint32_t image, uint32_t layer, ConstByteView rgba) {
    (void)image, (void)layer, (void)rgba;
    return false;
  }
  // Clears the colour images of `a` (to rgba) and/or its depth image (to depth_value).
  virtual bool clear_images(const AccelAttachments& a, bool color, const float rgba[4], bool depth, float depth_value) {
    (void)a, (void)color, (void)rgba, (void)depth, (void)depth_value;
    return false;
  }
  // draw() into images: the same, with `a` as the target. An image of `a` may not be one of the textures of `state`.
  virtual bool draw_images(const AccelAttachments& a, uint32_t program, const AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
    (void)a, (void)program, (void)state, (void)vertices, (void)vertex_count, (void)constants;
    return false;
  }

 protected:
  ~IAccelerator() = default;
};

// The device copies of the textures a context (or a WebGPU device) sampled. A texture is named by a key of the caller's; its texels go to the device when it is first
// used and again when they changed (a texture changes in too many places for each to say so, so a hash of the texels is read at each use).
class AccelTextureCache {
 public:
  explicit AccelTextureCache(allocators::IAllocator& a) : entries_(a) {}
  // The device texture holding `rgba` (w * h * 4 bytes; with `floats`, w * h * 16 bytes of floats: a float texture) as texture `key`. 0: the device cannot.
  uint32_t get(IAccelerator& acc, uint64_t key, uint32_t w, uint32_t h, ConstByteView rgba, bool floats = false);
  void forget(IAccelerator& acc, uint64_t key);
  void release(IAccelerator& acc);   // every texture goes

 private:
  struct Entry {
    uint64_t key, hash;
    uint32_t handle, w, h;
    bool floats;
  };
  static constexpr uint64_t kMaxEntries = 64;   // more and the oldest goes
  List<Entry> entries_;
};

// The accelerator all canvas contexts use (nullptr: software only). It must outlive every context.
void set_accelerator(IAccelerator* accelerator);
IAccelerator* accelerator();

// Where a context says why the device refused a program's shaders (the first few of each context): the reason, the stage (0 vertex, 1 fragment, 2 geometry, 3
// control, 4 evaluation), the source line and its text. nullptr: not said.
using AcceleratorLog = void (*)(const char* why, uint32_t stage, uint32_t line, StringView text);
void set_accelerator_log(AcceleratorLog log);
AcceleratorLog accelerator_log();

}  // namespace webgl
