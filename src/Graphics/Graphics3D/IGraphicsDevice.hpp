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
// host-visible, shader state is limited to push constants, and shaders are not
// SPIR-V (there is no compiler in the OS yet): a ShaderModule carries a native
// C++ function for the software device and/or TGSI text for the GPU device. The
// built-in shaders (BuiltinShaders.hpp) come with both, so the same program
// runs on either device.
//
// Conventions are Vulkan's: clip-space depth 0..1, +Y down on screen, a
// triangle's winding is judged as it appears on screen.
#pragma once
#include "Graphics/GraphicsTypes.hpp"

namespace graphics {

constexpr uint32_t kMaxVertexAttributes = 4;
constexpr uint32_t kMaxVaryings = 4;
constexpr uint32_t kMaxPushConstantBytes = 128;

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
};

struct ImageDesc {
  Extent2D extent;
  Format format = Format::B8G8R8A8Unorm;
  uint32_t usage = kImageColorAttachment;
};

class IImage {
 public:
  virtual Extent2D extent() const = 0;
  virtual Format format() const = 0;
  // Copies a color image to `out` (extent.width * extent.height values, 0xAARRGGBB).
  // The GPU device reads it back from the host, so this is slow: for tests and screenshots.
  virtual bool read_pixels(ArrayView<uint32_t> out) = 0;

 protected:
  constexpr IImage() = default;
  ~IImage() = default;
};

// --- Shaders -----------------------------------------------------------------

enum class ShaderStage : uint8_t { Vertex, Fragment };

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

// The QPU programs the Raspberry Pi's V3D device has for the built-in shaders (there is no shader compiler for the QPUs, so a module names a program it ships
// with instead of carrying code): the coloured-vertex pipeline of BuiltinShaders.
enum class QpuProgram : uint8_t { None, ColoredVertex, ColoredFragment };

// The pixel kernels the Intel Gen9 device has for the built-in shaders (there is no shader compiler for the EUs either).
enum class EuProgram : uint8_t { None, ColoredVertex, ColoredFragment };

struct ShaderModuleDesc {
  ShaderStage stage = ShaderStage::Vertex;
  VertexShaderFn vertex_function = nullptr;      // used by the software device
  FragmentShaderFn fragment_function = nullptr;  // used by the software device
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

enum class CullMode : uint8_t { None, Back, Front };
enum class FrontFace : uint8_t { CounterClockwise, Clockwise };  // as seen on screen

struct GraphicsPipelineDesc {
  const IShaderModule* vertex_shader = nullptr;
  const IShaderModule* fragment_shader = nullptr;
  uint32_t vertex_stride = 0;
  VertexAttribute attributes[kMaxVertexAttributes] = {};
  uint32_t attribute_count = 0;
  uint32_t varying_count = 0;  // values passed from the vertex to the fragment shader
  CullMode cull_mode = CullMode::Back;
  FrontFace front_face = FrontFace::CounterClockwise;
  bool depth_test = false;  // pass if closer or equal (LEQUAL)
  bool depth_write = false;
  bool alpha_blend = false;  // source over destination
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
