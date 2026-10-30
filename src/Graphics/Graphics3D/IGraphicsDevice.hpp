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
// The most a command buffer may carry as shader constants; what a device really takes is DeviceInfo::max_push_constant_bytes (most take 128: eight vec4s).
constexpr uint32_t kMaxPushConstantBytes = 4096;
constexpr uint32_t kMaxTextureUnits = 2;
// The colour images one render pass can draw into at once (kFeatureMultipleTargets): RenderPassBegin::color and up to three more.
constexpr uint32_t kMaxColorTargets = 4;

// --- Resources ---------------------------------------------------------------

enum BufferUsage : uint32_t {
  kBufferVertex = 1u << 0,
  kBufferIndex = 1u << 1,
  kBufferStorage = 1u << 2,   // kFeatureCompute: read and written by compute shaders (bind_storage_buffer)
};

class IBuffer {
 public:
  virtual uint64_t size() const = 0;
  // CPU access to the contents. Write, then unmap() before a submit that reads it.
  virtual ByteView map() = 0;
  virtual void unmap() = 0;
  // After a submit() in which the GPU may have written the buffer (a storage buffer a compute shader wrote): brings what it holds into map(). True if the contents are current.
  virtual bool read_back() { return true; }

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

// What a sampled image is (kFeatureTextureKinds for anything but Texture2D). Texture3D has `layers` slices of depth, Texture2DArray `layers` layers, TextureCube six faces in the order
// +X, -X, +Y, -Y, +Z, -Z (`layers` is ignored). All but Texture2D have one mip level, a colour format of 8-bit channels and no usage but kImageSampled; they are filled and read
// layer by layer (write_layer_pixels / read_layer_pixels), each layer a 2D image of `extent`.
enum class TextureKind : uint8_t { Texture2D, Texture3D, Texture2DArray, TextureCube };
constexpr uint32_t kMaxTextureLayers = 256;

struct ImageDesc {
  Extent2D extent;
  Format format = Format::B8G8R8A8Unorm;
  uint32_t usage = kImageColorAttachment;
  // A D32Float image can carry an 8-bit stencil plane next to the depth (the stencil test of a pipeline needs one). Devices without stencil refuse such an image.
  bool stencil = false;
  // The levels of a sampled colour image (Mipmaps.hpp): level 0 is `extent`, each next is half as large each way. More than one needs kImageSampled and no other usage on
  // the Intel Gen9 device; devices that cannot refuse the image. A level is filled by write_mip_pixels, or all of them from level 0 by generate_mipmaps.
  uint32_t mip_levels = 1;
  TextureKind kind = TextureKind::Texture2D;
  uint32_t layers = 1;
  // Samples per pixel of a multisampled attachment (kFeatureMultisample; DeviceInfo::sample_counts says which counts): a colour or depth attachment alone, one mip level, not sampled, not
  // presented, not read or written by pixels - it is drawn into and then resolved (ICommandBuffer::resolve_image). All the attachments of a pass have the same count.
  uint32_t samples = 1;
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
  // The same for the float formats (kFeatureFloatFormats; Format::R16G16B16A16Float / R32G32B32A32Float): extent.width * extent.height colours, rows top to bottom, not clamped. Half-float images
  // round what they are given to half precision. False for another format, a wrong size, or a device without float formats.
  virtual bool read_pixels_float(ArrayView<Vec4> out) {
    (void)out;
    return false;
  }
  virtual bool write_pixels_float(ArrayView<const Vec4> pixels) {
    (void)pixels;
    return false;
  }
  // The layers of an image that is not Texture2D (6 for a cube; 1 for a 2D image): extent.width * extent.height values each, rows top to bottom. False for a layer that does not
  // exist, a wrong size or a device that cannot.
  virtual uint32_t layer_count() const { return 1; }
  virtual bool write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) {
    (void)layer, (void)pixels;
    return false;
  }
  virtual bool read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) {
    (void)layer, (void)out;
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

enum class ShaderStage : uint8_t { Vertex, Fragment, Compute, Geometry, Hull, Domain };

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
  // The lookup in a texture of any kind (kFeatureTextureKinds): the coordinate is (u, v) and a third value `w` that means, by the kind of the image bound to the unit: Texture3D
  // the depth coordinate (0..1 over the slices; the slices are blended by a linear filter, and w is clamped), Texture2DArray the layer (rounded to the nearest, clamped),
  // TextureCube the z of a direction (u, v, w) = (x, y, z) that picks the face and the point on it, with no filtering across faces. A 2D image ignores w.
  virtual Vec4 sample_volume(uint32_t unit, float u, float v, float w) const {
    (void)w;
    return sample(unit, u, v);
  }
  // The texel access of kFeatureTexelAccess for the images of kFeatureTextureKinds (which have one level): SampleLevel (the lod does nothing but select the only level), Load of a texel of
  // a slice of a volume or a layer of an array (x, y, and z the slice or layer; a texel outside, or a level above 0, is transparent black; a cube has no Load), and GetDimensions (width,
  // height, then the depth of a volume or the layer count of an array; 1 for the others; and the levels).
  virtual Vec4 sample_lod_volume(uint32_t unit, float u, float v, float w, float lod) const {
    (void)lod;
    return sample_volume(unit, u, v, w);
  }
  virtual Vec4 fetch_volume(uint32_t unit, int32_t x, int32_t y, int32_t z, uint32_t level) const {
    return z == 0 ? fetch(unit, x, y, level) : Vec4{0, 0, 0, 0};
  }
  virtual bool size_volume(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* depth, uint32_t* levels) const {
    *depth = 1;
    return size(unit, level, width, height, levels);
  }
  // --- kFeatureTexelAccess (DeviceInfo::features); a device without it leaves these at their defaults.
  // The lookup at an explicit level of detail (lambda: 0 is the base level), as sample() takes it from the derivatives.
  virtual Vec4 sample_lod(uint32_t unit, float u, float v, float lod) const {
    (void)lod;
    return sample(unit, u, v);
  }
  // One texel of a level, no filtering (Direct3D 10 Load): x and y are texel numbers; outside the level it is transparent black. A unit with nothing bound gives the same.
  virtual Vec4 fetch(uint32_t unit, int32_t x, int32_t y, uint32_t level) const {
    (void)unit, (void)x, (void)y, (void)level;
    return {0, 0, 0, 0};
  }
  // The size of a level and the number of levels of the image bound to the unit (Direct3D 10 GetDimensions); false when nothing is bound.
  virtual bool size(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const {
    (void)unit, (void)level, (void)width, (void)height, (void)levels;
    return false;
  }

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
// (A fragment shader is handed room for kMaxColorTargets colours, all zero; the first is the colour of target 0, and shaders that write more only run on devices with kFeatureMultipleTargets.)
using TexturedFragmentShaderFn = bool (*)(const Vec4* varyings, const uint8_t* constants, const ITextureSet& textures, Vec4* color);

// The same with the system values of Direct3D 10 (kFeatureSystemInputs). A vertex shader is told which vertex of which instance it is shading; a fragment shader where the pixel is
// (position: x and y window coordinates of the pixel's centre, z the depth, w one over the clip space w), whether the triangle faces the viewer (lines and points: true) and which
// primitive of the draw it belongs to.
struct VertexContext {
  uint32_t vertex_id = 0;     // the index of the vertex (or its number in a draw without indices), before the base vertex offset is added in
  uint32_t instance_id = 0;
};
struct FragmentContext {
  Vec4 position;
  bool front_facing = true;
  uint32_t primitive_id = 0;
};
using VertexShaderFnEx = void (*)(const Vec4* attributes, const uint8_t* constants, const VertexContext& context, ShaderIo* out);
using FragmentShaderFnEx = bool (*)(const Vec4* varyings, const FragmentContext& context, const uint8_t* constants, const ITextureSet& textures, Vec4* color);

// Geometry shaders (kFeatureGeometryShaders), between the vertex shader and the rasterizer: one invocation per primitive of the draw (a list triangle, line or point - the kernel expands
// strips and fans), given the primitive's vertices as the vertex shader made them (clip space, before the perspective divide) and emitting up to the module's maximum of vertices, which
// make a strip of the module's output topology (a cut ends a strip and starts the next). The software device calls a function; virgl takes TGSI text.
enum class GeometryPrimitive : uint8_t { Point, Line, Triangle, LineAdjacency, TriangleAdjacency };   // (the adjacency ones take 4 and 6 vertices: the PrimitiveTopology LineListAdjacency / TriangleListAdjacency draws)
constexpr uint32_t kMaxGeometryInputVertices = 6;
enum class GeometryTopology : uint8_t { PointList, LineStrip, TriangleStrip };
constexpr uint32_t kMaxGeometryVertices = 64;   // the most a module's maximum may be
struct GeometryInputData {
  Vec4 position[kMaxGeometryInputVertices];
  Vec4 varying[kMaxGeometryInputVertices][kMaxVaryings];
  uint32_t primitive_id = 0;   // the number of the primitive in the draw
  uint32_t instance_id = 0;    // SV_GSInstanceID: which of the shader's instances this run is (it runs geometry_instances times for each primitive)
};
struct GeometryOutputData {
  uint32_t count = 0;
  ShaderIo vertex[kMaxGeometryVertices];
  bool restart[kMaxGeometryVertices] = {};   // a strip ended just before this vertex (a cut)
};
using GeometryShaderFn = void (*)(const GeometryInputData& input, const uint8_t* constants, GeometryOutputData* output);

// Tessellation (kFeatureTessellation). A draw of PatchList topology takes `control_points` vertices per patch (set_patch_control_points); the vertex shader runs on each, the hull shader
// then makes the patch's output control points and its tessellation factors, the fixed-function tessellator makes a lattice of domain points from the factors, and the domain shader
// turns each into a vertex from the patch (the triangles, lines or points joining them are then drawn). The software device calls functions; virgl takes TGSI text for the two stages.
// The triangle, quad and isoline domains are made with any partitioning, by the fixed-function tessellator of Direct3D 11 (see
// Software/Tessellator.hpp): an edge factor says how many segments the edge has, the inside factors how many rings (concentric triangles, or rectangles) there are.
constexpr uint32_t kMaxPatchControlPoints = 32;
constexpr uint32_t kMaxPatchRegisters = 16;   // patch constant registers (tessellation factors included)
enum class TessDomain : uint8_t { Isoline, Triangle, Quad };
enum class TessPartitioning : uint8_t { Integer, Pow2, FractionalOdd, FractionalEven };
enum class TessOutput : uint8_t { TriangleCw, TriangleCcw, Point, Line };
struct HullInputData {
  uint32_t control_points = 0;
  uint32_t primitive_id = 0;
  Vec4 position[kMaxPatchControlPoints];
  Vec4 varying[kMaxPatchControlPoints][kMaxVaryings];
};
struct HullOutputData {
  ShaderIo control_point[kMaxPatchControlPoints];   // the output control points
  Vec4 patch[kMaxPatchRegisters];                   // the patch constant registers (the tessellation factors are in the x of theirs)
  float outer[4] = {};                              // edge factors (triangle: the u = 0, v = 0 and w = 0 edges; quad: u = 0, v = 0, u = 1, v = 1; isoline: line density, line detail)
  float inner[2] = {};                              // inside factors (triangle: the first; quad: u, v)
};
using HullShaderFn = void (*)(const HullInputData& input, const uint8_t* constants, HullOutputData* output);
using DomainShaderFn = void (*)(const HullOutputData& patch, float u, float v, float w, const uint8_t* constants, ShaderIo* out);

// Compute shaders (kFeatureCompute). A software compute shader is called once per thread; the device runs the groups of a dispatch one after the other and the threads of a group
// in order (no barriers or group-shared memory: a module that needs them is not given a function). `buffers` are the storage buffers bound at the time (empty views for slots
// that are not bound); a read or write outside a buffer must be dropped or read as zero by the shader.
constexpr uint32_t kMaxStorageBuffers = 4;
struct ComputeContext {
  uint32_t group_id[3] = {};      // SV_GroupID
  uint32_t thread_id[3] = {};     // SV_GroupThreadID
  uint32_t dispatch_id[3] = {};   // SV_DispatchThreadID: group_id * thread group size + thread_id
  uint32_t group_index = 0;       // SV_GroupIndex: the thread's number in its group
  ByteView buffers[kMaxStorageBuffers];
};
using ComputeShaderFn = void (*)(const ComputeContext& context, const uint8_t* constants);
// Most threads in a group and most groups (in each dimension) of a dispatch.
constexpr uint32_t kMaxThreadsPerGroup = 1024;
constexpr uint32_t kMaxDispatchGroups = 65535;
constexpr uint64_t kMaxDispatchThreads = 1ull << 24;

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
  // The same with system values: a device with kFeatureSystemInputs runs these when a module has them (and any of the others otherwise).
  VertexShaderFnEx vertex_function_ex = nullptr;
  FragmentShaderFnEx fragment_function_ex = nullptr;
  // A compute shader (stage Compute): the function for the software device, TGSI text for virgl, and the size of a thread group either way.
  ComputeShaderFn compute_function = nullptr;
  // A geometry shader (stage Geometry): the function for the software device (virgl: the TGSI text), what it takes and makes, and the most vertices one invocation emits.
  GeometryShaderFn geometry_function = nullptr;
  GeometryPrimitive geometry_input = GeometryPrimitive::Triangle;
  GeometryTopology geometry_output = GeometryTopology::TriangleStrip;
  uint32_t geometry_max_vertices = 0;
  uint32_t geometry_instances = 1;   // runs of a geometry shader for each primitive (1 .. 32)
  // Hull and domain shaders (stages Hull and Domain): functions for the software device (virgl: TGSI text), the control points taken and made, and the tessellator's settings (both stages say them).
  HullShaderFn hull_function = nullptr;
  DomainShaderFn domain_function = nullptr;
  uint32_t tess_input_points = 0, tess_output_points = 0;
  TessDomain tess_domain = TessDomain::Triangle;
  TessPartitioning tess_partitioning = TessPartitioning::Integer;
  TessOutput tess_output = TessOutput::TriangleCcw;
  uint32_t thread_group[3] = {1, 1, 1};
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
  // With QpuProgram::ColoredVertex: the matrix is given as rows (the transpose of what the built-in shader takes), as a Direct3D 9 vertex shader that ends in dp4s uses it.
  bool qpu_matrix_rows = false;
  // ... and the program is written for Direct3D's clip space (+Y up): the device flips the Y row of the matrix.
  bool qpu_flip_y = false;
  // The same for the Intel Gen9 device: the coloured-vertex pipeline of BuiltinShaders (the vertex transform runs on the CPU there).
  EuProgram eu_program = EuProgram::None;
  // With a ColoredFragment / TexturedFragment eu_program: which of the vertex shader's varyings the kernel reads (the colour, or the texture coordinate in its first two components).
  // A vertex shader with a vertex_function (and no eu_program) is run on the CPU by that device.
  uint8_t eu_varying = 0;
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
  bool per_instance = false;   // kFeatureInstancing: the attribute steps with the instance, not the vertex (element number = the instance, at the same stride)
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

// Blending (kFeatureBlendState): result = source * source_factor (op) destination * destination_factor, for the colour and the alpha on their own, then only the channels
// of write_mask (bit 0 red .. bit 3 alpha) are stored. Min / Max ignore the factors. With `custom` false the pipeline's alpha_blend decides (source over destination).
enum class BlendFactor : uint8_t { Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DstColor, InvDstColor, DstAlpha, InvDstAlpha, SrcAlphaSaturate };
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
struct BlendState {
  bool custom = false;
  bool enable = false;
  BlendFactor src_color = BlendFactor::One, dst_color = BlendFactor::Zero, src_alpha = BlendFactor::One, dst_alpha = BlendFactor::Zero;
  BlendOp color_op = BlendOp::Add, alpha_op = BlendOp::Add;
  uint8_t write_mask = 0xF;
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
  // kFeatureMultisample, on a pass with several samples per pixel: with multisample the triangle covers the samples whose positions it contains and depth and stencil are tested for each
  // (the shader still runs once per pixel); without it a pixel is all or nothing by its centre, as Direct3D 9 draws. Lines and points cover every sample of their pixel.
  bool multisample = false;
  // With multisample: the samples a pixel's shader result reaches are the first round(alpha * samples) of the covered ones, alpha being the first target's (which samples of a partly
  // covered pixel get it is up to the device; of a wholly covered pixel, only how many - and a real GPU may truncate where this rounds: 0.7 of 4 samples kept 2 on the one tried).
  bool alpha_to_coverage = false;
  // kFeatureGeometryShaders: a geometry shader module run between the vertex and fragment shaders (nullptr: none). What it takes must be what the draws make (triangles, lines or points).
  const IShaderModule* geometry_shader = nullptr;
  // kFeatureTessellation: the hull and domain shader modules of a pipeline for PatchList draws (both or neither).
  const IShaderModule* hull_shader = nullptr;
  const IShaderModule* domain_shader = nullptr;
  StencilState stencil;
  BlendState blend;                                   // kFeatureBlendState
  CompareOp depth_compare = CompareOp::LessEqual;     // kFeatureBlendState: what depth_test compares with (a device without it always uses LessEqual)
  // kFeatureIndependentBlend: with independent_blend, colour target 0 uses `blend` (or alpha_blend) and target n (1..3) uses target_blend[n - 1], which is taken as it is (custom or
  // not: a state that is not custom means no blending and every channel written). Without it every target uses `blend`.
  bool independent_blend = false;
  BlendState target_blend[kMaxColorTargets - 1];
  uint32_t color_target_count = 1;                    // kFeatureMultipleTargets: how many colour outputs the fragment shader writes (the render pass must have at least as many images); blend state applies to each
};

class IPipeline {
 public:
 protected:
  constexpr IPipeline() = default;
  ~IPipeline() = default;
};

// --- Commands ----------------------------------------------------------------

enum class IndexType : uint8_t { Uint16, Uint32 };

// What the vertices of a draw make (set_primitive_topology, DeviceFeature kFeatureLines for anything but triangles). Lines and points are one pixel wide.
enum class QueryKind : uint8_t { Occlusion, OcclusionPredicate, IaVertices, IaPrimitives, VsInvocations, PsInvocations };
enum class PrimitiveTopology : uint8_t { TriangleList, LineList, PointList, PatchList, LineListAdjacency, TriangleListAdjacency };   // (the adjacency lists need a geometry shader that takes them)   // (PatchList: kFeatureTessellation, with set_patch_control_points)

struct Viewport {
  float x = 0, y = 0, width = 0, height = 0;
  float min_depth = 0, max_depth = 1;
};

// The occlusion queries one command buffer can hold (kFeatureOcclusionQueries): slots 0 .. kMaxOcclusionQueries - 1.
constexpr uint32_t kMaxOcclusionQueries = 64;

struct RenderPassBegin {
  IImage* color = nullptr;
  // Further colour images (kFeatureMultipleTargets): target n (1..3) is extra_colors[n - 1], and a gap ends the list. All have the extent of `color`, none is bound as a texture
  // in the pass, and a fragment shader writes target n to colour output n. clear_color_value clears every target.
  IImage* extra_colors[kMaxColorTargets - 1] = {};
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
  // Several instances (kFeatureInstancing): the draw is made instance_count times with instance numbers first_instance ..; a device without it draws once.
  virtual void draw(uint32_t vertex_count, uint32_t first_vertex = 0, uint32_t instance_count = 1, uint32_t first_instance = 0) = 0;
  virtual void draw_indexed(uint32_t index_count, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t instance_count = 1, uint32_t first_instance = 0) = 0;
  // What the vertices of the draws that follow make (kFeatureLines for lines and points; the default is a list of triangles).
  virtual void set_primitive_topology(PrimitiveTopology topology) = 0;
  // How many control points a patch of the PatchList draws that follow has (1 .. kMaxPatchControlPoints).
  virtual void set_patch_control_points(uint32_t count) { (void)count; }
  virtual void end_render_pass() = 0;
  // Averages the samples of a multisampled colour image into a single-sample image of the same format and size (kFeatureMultisample), outside a render pass.
  virtual void resolve_image(IImage* destination, IImage* source) {
    (void)destination, (void)source;
  }
  // Compute (kFeatureCompute), outside a render pass: bind_compute_pipeline makes the pipeline of create_compute_pipeline current for the dispatches that follow, bind_storage_buffer
  // puts `size` bytes of a kBufferStorage buffer from `offset` in slot 0 .. kMaxStorageBuffers - 1, and dispatch runs x * y * z thread groups. A device without the feature ignores all three.
  virtual void bind_compute_pipeline(const IPipeline* pipeline) { (void)pipeline; }
  virtual void bind_storage_buffer(uint32_t slot, IBuffer* buffer, uint64_t offset, uint64_t size) {
    (void)slot, (void)buffer, (void)offset, (void)size;
  }
  virtual void dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) { (void)groups_x, (void)groups_y, (void)groups_z; }
  // Occlusion queries (kFeatureOcclusionQueries), inside a render pass: from begin to end, the fragments that pass the stencil and depth tests (and that the shader did not
  // discard) are counted - one per pixel, however many targets the pipeline writes and whatever the blending or write masks do. `slot` (below kMaxOcclusionQueries) names the result; one query is open at a time and
  // it ends before the pass does. A device without the feature ignores both.
  virtual void begin_occlusion_query(uint32_t slot) { (void)slot; }
  // The other queries (kFeatureStatisticsQueries), begun and ended the same way and read with occlusion_result: OcclusionPredicate is 1 if any fragment passed, else 0; IaVertices the vertices
  // the draws between begin and end took (a draw of n vertices and i instances: n * i), IaPrimitives the primitives they assembled (a patch is a primitive), VsInvocations the vertex shader
  // runs (a device may run it fewer times than there are vertices, by reusing results) and PsInvocations the fragment shader runs (a device may shade more pixels than it writes).
  virtual void begin_query(uint32_t slot, QueryKind kind) {
    if (kind == QueryKind::Occlusion) begin_occlusion_query(slot);
  }
  virtual void end_occlusion_query() {}
  // After submit(): the count of a query that was begun and ended (false for a slot that was not).
  virtual bool occlusion_result(uint32_t slot, uint64_t* samples) const {
    (void)slot, (void)samples;
    return false;
  }
  // False if recording failed (out of memory, or a command used a resource wrongly).
  virtual bool end() = 0;

 protected:
  constexpr ICommandBuffer() = default;
  ~ICommandBuffer() = default;
};

// --- Device ------------------------------------------------------------------

enum class DeviceKind : uint8_t { Software, VirtualGpu, Gpu };

// What a device does beyond the base interface (DeviceInfo::features); a device without a feature ignores the fields and calls of it, so a user of the interface asks first.
enum DeviceFeature : uint32_t {
  kFeatureBlendState = 1u << 0,    // GraphicsPipelineDesc::blend and depth_compare
  kFeatureTexelAccess = 1u << 1,   // ITextureSet::sample_lod / fetch / size
  kFeatureSystemInputs = 1u << 2,  // ShaderModuleDesc::vertex_function_ex / fragment_function_ex
  kFeatureInstancing = 1u << 3,    // draw(..., instance_count, first_instance) and VertexAttribute::per_instance
  kFeatureLines = 1u << 4,         // set_primitive_topology: lines and points
  kFeatureFloatFormats = 1u << 6,      // Format::R16G16B16A16Float / R32G32B32A32Float images: colour attachments (also several at once) and sampled textures, without mipmaps; read_pixels_float / write_pixels_float
  kFeatureTessellation = 1u << 16,     // ShaderStage::Hull / Domain, GraphicsPipelineDesc::hull_shader / domain_shader, PrimitiveTopology::PatchList
  kFeatureStatisticsQueries = 1u << 18,  // QueryKind other than Occlusion (with kFeatureOcclusionQueries)
  kFeatureSnormFormats = 1u << 17,     // Format::R16G16B16A16Snorm, R10G10B10A2Unorm
  kFeatureSrgbFormats = 1u << 13,      // Format::B8G8R8A8Srgb / R8G8B8A8Srgb
  kFeatureUnorm16Formats = 1u << 14,   // Format::R16G16B16A16Unorm (also kFeatureFloatFormats' read_pixels_float / write_pixels_float)
  kFeatureIntegerFormats = 1u << 15,   // Format::R32G32B32A32Uint, R32G32B32A32Sint, R16G16B16A16Uint / Sint, R8G8B8A8Uint
  kFeatureGeometryShaders = 1u << 12,  // GraphicsPipelineDesc::geometry_shader, ShaderStage::Geometry modules
  kFeatureMultisample = 1u << 11,      // ImageDesc::samples, GraphicsPipelineDesc::multisample, ICommandBuffer::resolve_image
  kFeatureCompute = 1u << 10,          // compute shaders: create_compute_pipeline, kBufferStorage buffers, ICommandBuffer::dispatch
  kFeatureIndependentBlend = 1u << 9,  // GraphicsPipelineDesc::independent_blend / target_blend
  kFeatureTextureKinds = 1u << 8,      // ImageDesc::kind other than Texture2D, layer_count / write_layer_pixels / read_layer_pixels, ITextureSet::sample_volume
  kFeatureOcclusionQueries = 1u << 7,  // ICommandBuffer::begin_occlusion_query / end_occlusion_query / occlusion_result
  kFeatureMultipleTargets = 1u << 5,   // RenderPassBegin::extra_colors and GraphicsPipelineDesc::color_target_count
};


struct DeviceInfo {
  const char* name;
  DeviceKind kind;
  bool accelerated;  // rendering is done by a GPU, not by the CPU
  uint32_t max_push_constant_bytes = 128;   // what push_constants() may be given (a multiple of 16, up to kMaxPushConstantBytes)
  uint32_t features = 0;                    // DeviceFeature bits
  uint32_t sample_counts = 0;               // kFeatureMultisample: bit n set: ImageDesc::samples = n is supported
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
  // A pipeline that runs a compute shader module (kFeatureCompute); nullptr when the device has none or the module is not one it can run. Freed with destroy_pipeline.
  virtual IPipeline* create_compute_pipeline(const IShaderModule* shader) {
    (void)shader;
    return nullptr;
  }
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
