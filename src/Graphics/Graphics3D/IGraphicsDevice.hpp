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

// 3D rendering, loosely modelled on Vulkan: a device creates buffers, images,
// shader modules and graphics pipelines; draw work is recorded into a command
// buffer and executed by submit(); present() puts a finished image on screen.
//
// What differs from Vulkan, on purpose: no instance/queue-family/descriptor-set/
// synchronization machinery (submit() is synchronous), memory is always
// host-visible, shader state is limited to push constants and a few bound textures
// (a sampled image with its sampler state, with or without mipmaps), and shaders are not
// SPIR-V (there is no compiler in the OS yet): a ShaderModule carries a native
// C++ function for the software device and/or TGSI text for the GPU device. The
// built-in shaders (BuiltinShaders.hpp) come with both, so the same program
// runs on either device.
//
// Conventions are Vulkan's: clip-space depth 0..1, +Y down on screen, a
// triangle's winding is judged as it appears on screen.
#pragma once
#include "Graphics/GraphicsTypes.hpp"
#include "Graphics/Graphics3D/Mipmaps.hpp"

namespace graphics {

constexpr uint32_t kMaxVertexAttributes = 4;
constexpr uint32_t kMaxVaryings = 4;
constexpr uint32_t kMaxPushConstantBytes = 128;
constexpr uint32_t kMaxTextureUnits = 2;

// --- Resources ---------------------------------------------------------------

enum BufferUsage : uint32_t {
  kBufferVertex = 1u << 0,
  kBufferIndex = 1u << 1,
};

class IBuffer {
 public:
  virtual uint64_t size() const = 0;
  // CPU access to the contents. Write, then unmap() before a submit that reads it.
  virtual ByteView map() = 0;
  virtual void unmap() = 0;

 protected:
  constexpr IBuffer() = default;
  ~IBuffer() = default;
};

enum ImageUsage : uint32_t {
  kImageColorAttachment = 1u << 0,
  kImageDepthAttachment = 1u << 1,
  kImagePresent = 1u << 2,  // may be shown with present()
  kImageSampled = 1u << 3,  // may be bound as a texture with bind_texture() and filled with write_pixels()
};

struct ImageDesc {
  Extent2D extent;
  Format format = Format::B8G8R8A8Unorm;
  uint32_t usage = kImageColorAttachment;
  // A D32Float image can carry an 8-bit stencil plane next to the depth (the stencil test of a pipeline needs one). Devices without stencil refuse such an image.
  bool stencil = false;
  // The levels of a sampled colour image (Mipmaps.hpp): level 0 is `extent`, each next is half as large each way. More than one needs kImageSampled and no other usage on
  // the Intel Gen9 device; devices that cannot refuse the image. A level is filled by write_mip_pixels, or all of them from level 0 by generate_mipmaps.
  uint32_t mip_levels = 1;
};

class IImage {
 public:
  virtual Extent2D extent() const = 0;
  virtual Format format() const = 0;
  // Copies a color image to `out` (extent.width * extent.height values, 0xAARRGGBB).
  // The GPU device reads it back from the host, so this is slow: for tests and screenshots.
  virtual bool read_pixels(ArrayView<uint32_t> out) = 0;
  // Replaces the contents of a color image (extent.width * extent.height values, 0xAARRGGBB, rows top to bottom) - how a texture is filled. False if the device cannot (the
  // default), the image is not a color image or `pixels` is the wrong size. Not synchronised with a submit() that uses the image: an image belongs to one user at a time.
  virtual bool write_pixels(ArrayView<const uint32_t> pixels) {
    (void)pixels;
    return false;
  }
  // The number of levels the image was made with.
  virtual uint32_t mip_levels() const { return 1; }
  // The same for level `level` of a mipmapped image (mip_extent() of the extent, rows top to bottom). Level 0 is write_pixels.
  virtual bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) {
    (void)level;
    (void)pixels;
    return false;
  }
  // Makes every level above 0 from level 0 by the 2 x 2 box filter of mip_box_downsample, each from the one before it. False when the image has one level or cannot.
  virtual bool generate_mipmaps() { return false; }

 protected:
  constexpr IImage() = default;
  ~IImage() = default;
};

// --- Shaders -----------------------------------------------------------------

enum class ShaderStage : uint8_t { Vertex, Fragment };

// --- Textures ----------------------------------------------------------------

// How a texture is read. Coordinates are normalised: (0, 0) is the top left corner of the top left texel and (1, 1) the bottom right corner of the bottom right one,
// +v down the picture. Nearest takes the texel the point lies in; Linear blends the four texels around it (their centres are half a texel in).
//
// Mipmaps: with mip_filter None only level 0 is read. Otherwise the lookup picks a level of detail from how fast the coordinates change from pixel to pixel: the derivatives
// of (u, v) along x and y are taken from the 2 x 2 pixel quad the pixel is in (the right pixel's lookup minus the left's, the lower's minus the upper's - the same for the two
// pixels of a row or column of the quad), in texels of level 0; rho is the longer of the two vectors; lambda = log2(rho) + lod_bias, clamped to [min_lod, max_lod] and to the
// levels the image has. A lambda of 0 or less is magnification and reads level 0 with `filter`. Above it, Nearest reads the level floor(lambda + 0.5) and Linear blends
// the levels floor(lambda) and the next by the fraction of lambda, each read with `filter`. Only the first lookup of a unit in a fragment shader invocation takes its level
// from the derivatives; any further lookup of the unit in the same invocation uses the same level.
enum class Filter : uint8_t { Nearest, Linear };
enum class MipFilter : uint8_t { None, Nearest, Linear };
// What a coordinate outside 0..1 means: wrap around, or take the edge texel.
enum class AddressMode : uint8_t { Repeat, ClampToEdge };
struct SamplerDesc {
  Filter filter = Filter::Nearest;
  AddressMode address_u = AddressMode::Repeat;
  AddressMode address_v = AddressMode::Repeat;
  MipFilter mip_filter = MipFilter::None;
  float lod_bias = 0.0f;
  float min_lod = 0.0f, max_lod = 1000.0f;
};

// The bound textures as a fragment shader sees them: sample(unit, u, v) gives the colour (0..1 per component, straight alpha) of the image bound to `unit` with the sampler
// it was bound with. A unit with nothing bound gives opaque black (a draw that uses a pipeline's textures without binding them is refused at submit, so a shader only
// sees this for a unit beyond the pipeline's texture_count).
class ITextureSet {
 public:
  virtual Vec4 sample(uint32_t unit, float u, float v) const = 0;

 protected:
  constexpr ITextureSet() = default;
  ~ITextureSet() = default;
};

// What a vertex shader produces and a fragment shader receives (interpolated).
struct ShaderIo {
  Vec4 position;  // clip space (vertex stage only)
  Vec4 varying[kMaxVaryings];
};

// Software shaders. `attributes` holds the pipeline's vertex attributes by location (unset
// components are 0, 0, 0, 1); `constants` is the push-constant block. A fragment shader
// returns false to discard the pixel.
using VertexShaderFn = void (*)(const Vec4* attributes, const uint8_t* constants, ShaderIo* out);
using FragmentShaderFn = bool (*)(const Vec4* varyings, const uint8_t* constants, Vec4* color);
// The same for a fragment shader that reads the pipeline's textures.
using TexturedFragmentShaderFn = bool (*)(const Vec4* varyings, const uint8_t* constants, const ITextureSet& textures, Vec4* color);

// The QPU programs the Raspberry Pi's V3D device has for the built-in shaders (there is no shader compiler for the QPUs, so a module names a program it ships
// with instead of carrying code): the coloured-vertex pipeline of BuiltinShaders.
enum class QpuProgram : uint8_t { None, ColoredVertex, ColoredFragment };

// The pixel kernels the Intel Gen9 device has for the built-in shaders (there is no shader compiler for the EUs either).
enum class EuProgram : uint8_t { None, ColoredVertex, ColoredFragment, TexturedVertex, TexturedFragment };

struct ShaderModuleDesc {
  ShaderStage stage = ShaderStage::Vertex;
  VertexShaderFn vertex_function = nullptr;      // used by the software device
  FragmentShaderFn fragment_function = nullptr;  // used by the software device
  // A fragment shader that samples textures (instead of fragment_function; the pipeline then says how many in texture_count). The software, Intel Gen9 and virgl devices run these: the
  // others refuse a pipeline with textures.
  TexturedFragmentShaderFn textured_fragment_function = nullptr;
  // TGSI text for the GPU device. Inputs are the vertex attributes (vertex stage) or the
  // varyings as GENERIC[n] (fragment stage); push constants are CONST[0..7] (vec4 each).
  // Clip-space z is 0..1 and the first color output is the pixel.
  const char* tgsi = nullptr;
  // Direct3D 9 shader bytecode (vs_2_0 / ps_2_0 token stream) for the VMware SVGA3D device. A vertex shader writes clip space with +Y up (Direct3D's)
  // when it takes the c31 constant (1, -1, 1, 1) as a factor of the position; the device supplies it, so that Vulkan's +Y down comes out right.
  const uint32_t* d3d9_tokens = nullptr;
  uint32_t d3d9_token_count = 0;
  // The QPU program of the V3D device that does what this module does; None for a module that has none (the V3D device then cannot run it).
  QpuProgram qpu_program = QpuProgram::None;
  // The same for the Intel Gen9 device: the coloured-vertex pipeline of BuiltinShaders (the vertex transform runs on the CPU there).
  EuProgram eu_program = EuProgram::None;
};

class IShaderModule {
 public:
  virtual ShaderStage stage() const = 0;

 protected:
  constexpr IShaderModule() = default;
  ~IShaderModule() = default;
};

// --- Pipelines ---------------------------------------------------------------

enum class VertexFormat : uint8_t { Float1, Float2, Float3, Float4 };

struct VertexAttribute {
  uint32_t location;  // which input of the vertex shader
  VertexFormat format;
  uint32_t offset;  // bytes from the start of the vertex
};

// The stencil test of a pipeline, one state for front and back faces: a fragment passes when (reference & compare_mask) <compare> (stored & compare_mask); what is written back
// is (stored & ~write_mask) | (op result & write_mask), the op depending on whether the stencil test failed, the stencil test passed but the depth test failed, or both passed.
// It needs a stencil plane in the render pass's depth image (ImageDesc::stencil). The test comes before the depth test; a fragment a shader discards changes nothing.
enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilOp : uint8_t { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
struct StencilState {
  bool enable = false;
  CompareOp compare = CompareOp::Always;
  StencilOp fail_op = StencilOp::Keep, depth_fail_op = StencilOp::Keep, pass_op = StencilOp::Keep;
  uint8_t compare_mask = 0xFF, write_mask = 0xFF, reference = 0;
};

enum class CullMode : uint8_t { None, Back, Front };
enum class FrontFace : uint8_t { CounterClockwise, Clockwise };  // as seen on screen

struct GraphicsPipelineDesc {
  const IShaderModule* vertex_shader = nullptr;
  const IShaderModule* fragment_shader = nullptr;
  uint32_t vertex_stride = 0;
  VertexAttribute attributes[kMaxVertexAttributes] = {};
  uint32_t attribute_count = 0;
  uint32_t varying_count = 0;  // values passed from the vertex to the fragment shader
  uint32_t texture_count = 0;  // texture units 0 .. n-1 the fragment shader samples (at most kMaxTextureUnits); every one must be bound before a draw
  CullMode cull_mode = CullMode::Back;
  FrontFace front_face = FrontFace::CounterClockwise;
  bool depth_test = false;  // pass if closer or equal (LEQUAL)
  bool depth_write = false;
  bool alpha_blend = false;  // source over destination
  StencilState stencil;
};

class IPipeline {
 public:
 protected:
  constexpr IPipeline() = default;
  ~IPipeline() = default;
};

// --- Commands ----------------------------------------------------------------

enum class IndexType : uint8_t { Uint16, Uint32 };

struct Viewport {
  float x = 0, y = 0, width = 0, height = 0;
  float min_depth = 0, max_depth = 1;
};

struct RenderPassBegin {
  IImage* color = nullptr;
  IImage* depth = nullptr;  // optional
  bool clear_color = true;
  Vec4 clear_color_value;
  bool clear_depth = true;
  float clear_depth_value = 1.0f;
  bool clear_stencil = true;   // if the depth image has a stencil plane
  uint8_t clear_stencil_value = 0;
};

class ICommandBuffer {
 public:
  virtual void begin() = 0;
  virtual void begin_render_pass(const RenderPassBegin& pass) = 0;
  virtual void bind_pipeline(const IPipeline* pipeline) = 0;
  virtual void bind_vertex_buffer(IBuffer* buffer, uint64_t offset = 0) = 0;
  virtual void bind_index_buffer(IBuffer* buffer, IndexType type, uint64_t offset = 0) = 0;
  // Sets the shader constants (at most kMaxPushConstantBytes, a multiple of 16).
  virtual void push_constants(const void* data, uint32_t size) = 0;
  // Binds a sampled image (created with kImageSampled) to a texture unit with a sampler, for the draws that follow. A render pass may not draw into an image it samples.
  virtual void bind_texture(uint32_t unit, IImage* image, const SamplerDesc& sampler) = 0;
  // Defaults to the whole render target.
  virtual void set_viewport(const Viewport& viewport) = 0;
  virtual void set_scissor(const Rect2D& scissor) = 0;
  virtual void draw(uint32_t vertex_count, uint32_t first_vertex = 0) = 0;
  virtual void draw_indexed(uint32_t index_count, uint32_t first_index = 0, int32_t vertex_offset = 0) = 0;
  virtual void end_render_pass() = 0;
  // False if recording failed (out of memory, or a command used a resource wrongly).
  virtual bool end() = 0;

 protected:
  constexpr ICommandBuffer() = default;
  ~ICommandBuffer() = default;
};

// --- Device ------------------------------------------------------------------

enum class DeviceKind : uint8_t { Software, VirtualGpu, Gpu };

struct DeviceInfo {
  const char* name;
  DeviceKind kind;
  bool accelerated;  // rendering is done by a GPU, not by the CPU
};

class IGraphicsDevice {
 public:
  virtual DeviceInfo info() const = 0;

  // Resources come back nullptr on failure and are freed with the matching destroy_*.
  virtual IBuffer* create_buffer(uint64_t size, uint32_t usage) = 0;
  virtual void destroy_buffer(IBuffer* buffer) = 0;
  virtual IImage* create_image(const ImageDesc& desc) = 0;
  virtual void destroy_image(IImage* image) = 0;
  virtual IShaderModule* create_shader_module(const ShaderModuleDesc& desc) = 0;
  virtual void destroy_shader_module(IShaderModule* module) = 0;
  virtual IPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
  virtual void destroy_pipeline(IPipeline* pipeline) = 0;
  virtual ICommandBuffer* create_command_buffer() = 0;
  virtual void destroy_command_buffer(ICommandBuffer* commands) = 0;

  // Runs the recorded commands; returns when they are done (or failed).
  virtual bool submit(ICommandBuffer* commands) = 0;
  // Shows `image` (created with kImagePresent) on the screen.
  virtual bool present(IImage* image) = 0;

 protected:
  constexpr IGraphicsDevice() = default;
  ~IGraphicsDevice() = default;
};

}  // namespace graphics
