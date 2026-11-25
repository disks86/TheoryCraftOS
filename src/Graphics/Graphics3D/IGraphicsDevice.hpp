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
#include "Graphics/Graphics3D/TexelBuffer.hpp"

namespace graphics {

// --- Capacities and limits ---
// The kMax constants are the sizes of the arrays in the structures shared by the devices and the shader translators: the most any device could be asked for. What a device really
// takes is what it says in DeviceInfo::limits (the CPU renderer takes all of the capacity; a GPU backend takes what its encoder and the hardware can, see kBase* below); the kernel
// checks the pipelines and bindings it is given against those, never against the capacities.
constexpr uint32_t kMaxVertexAttributes = 16;
constexpr uint32_t kMaxVaryings = 16;           // values passed from the vertex to the fragment shader
constexpr uint32_t kMaxVertexBuffers = 8;       // vertex buffers (input slots) bound at once
// The most a command buffer may carry as shader constants; what a device really takes is DeviceInfo::max_push_constant_bytes (most take 128: eight vec4s).
constexpr uint32_t kMaxPushConstantBytes = 4096;
constexpr uint32_t kMaxTextureUnits = 16;
// The colour images one render pass can draw into at once (kFeatureMultipleTargets): RenderPassBegin::color and up to seven more.
constexpr uint32_t kMaxColorTargets = 8;
// What the backends written for fixed pipelines (virgl, SVGA3D, Intel Gen9, Radeon Vega M, V3D) take: their encoders and fixed kernels were made for these numbers, so they say so in
// DeviceInfo::limits (the defaults). A backend that can take more says it.
constexpr uint32_t kBaseVertexAttributes = 4;
constexpr uint32_t kBaseVaryings = 4;
constexpr uint32_t kBaseTextureUnits = 2;
constexpr uint32_t kBaseColorTargets = 4;
constexpr uint32_t kBaseVertexBuffers = 1;

// What a device can do in the dimensions the kMax constants size. The defaults are the kBase values.
struct DeviceLimits {
  uint32_t vertex_attributes = kBaseVertexAttributes;   // vertex shader inputs (VertexAttribute::location is below this)
  uint32_t varyings = kBaseVaryings;                    // GraphicsPipelineDesc::varying_count
  uint32_t texture_units = kBaseTextureUnits;           // GraphicsPipelineDesc::texture_count, bind_texture units
  uint32_t color_targets = kBaseColorTargets;           // kFeatureMultipleTargets: colour images of a pass
  uint32_t vertex_buffers = kBaseVertexBuffers;         // bind_vertex_buffer_slot slots
  uint32_t control_varyings = 0;                        // tessellation: the vectors of each control point from the hull to the domain stage (0: `varyings`)
};

// --- Resources ---------------------------------------------------------------

enum BufferUsage : uint32_t {
  kBufferVertex = 1u << 0,
  kBufferIndex = 1u << 1,
  kBufferStorage = 1u << 2,   // kFeatureCompute: read and written by compute shaders (bind_storage_buffer)
  kBufferStreamOutput = 1u << 3,   // kFeatureStreamOutput: a geometry shader's stream output goes here (bind_stream_output); read back with IBuffer::read_back
};

class IBuffer {
 public:
  virtual uint64_t size() const = 0;
  // CPU access to the contents. Write, then unmap() before a submit that reads it.
  virtual ByteView map() = 0;
  virtual void unmap() = 0;
  // After a submit() in which the GPU may have written the buffer (a storage buffer a compute shader wrote): brings what it holds into map(). True if the contents are current.
  virtual bool read_back() { return true; }
  // A stream output buffer: the offset the last bound stream output reached (bind_stream_output with kStreamOutputAppend starts there).
  uint64_t stream_output_filled() const { return stream_output_filled_; }
  void set_stream_output_filled(uint64_t offset) { stream_output_filled_ = offset; }

 protected:
  constexpr IBuffer() = default;
  ~IBuffer() = default;

 private:
  uint64_t stream_output_filled_ = 0;
};

enum ImageUsage : uint32_t {
  kImageColorAttachment = 1u << 0,
  kImageDepthAttachment = 1u << 1,
  kImagePresent = 1u << 2,  // may be shown with present()
  kImageSampled = 1u << 3,  // may be bound as a texture with bind_texture() and filled with write_pixels()
  // May be bound with bind_storage_image() and read and written texel by texel by shaders (DeviceInfo::features2 kFeature2StorageImages): a colour image of one sample, not sRGB or
  // block-compressed, of any kind but a cube (a level of a mipmapped 2D image, or layers of an array or a volume). Always with kImageSampled too, so that the GPU devices keep it the
  // way a texture is kept (row 0 first).
  kImageStorage = 1u << 4,
};

// What a sampled image is (kFeatureTextureKinds for anything but Texture2D). Texture3D has `layers` slices of depth, Texture2DArray `layers` layers, TextureCube six faces in the order
// +X, -X, +Y, -Y, +Z, -Z (`layers` is ignored). All but Texture2D have one mip level and kImageSampled, and (with kFeature2LayeredTargets) may be colour or depth attachments too,
// a pass drawing into a run of their layers (RenderPassBegin::first_layer), or storage images (kImageStorage, not a cube); they are filled and read layer by layer (write_layer_pixels /
// read_layer_pixels, or the _float forms for the wide formats), each layer a 2D image of `extent`.
// TextureCubeArray (kFeature2MipmappedLayers) has `layers` layers, a multiple of six: cube n is layers 6n .. 6n + 5, each in the order of a cube's faces.
enum class TextureKind : uint8_t { Texture2D, Texture3D, Texture2DArray, TextureCube, Texture2DMS, TextureCubeArray };   // (Texture2DMS: only what a shader declares for a unit - a multisampled colour image, whose ImageDesc::kind is Texture2D, is bound to it)
constexpr uint32_t kMaxTextureLayers = 256;
// The layers of an image of `kind` made with `layers` (ImageDesc::layers): 1 for a 2D image, 6 for a cube, `layers` for the others.
constexpr uint32_t image_layer_count(TextureKind kind, uint32_t layers) { return kind == TextureKind::Texture2D || kind == TextureKind::Texture2DMS ? 1 : kind == TextureKind::TextureCube ? 6 : layers; }
// ... and of its level `level` (kFeature2MipmappedLayers): a volume's levels are half as deep each (at least one slice), an array's and a cube's levels have all the layers.
constexpr uint32_t level_layer_count(TextureKind kind, uint32_t layers, uint32_t level) {
  if (kind != TextureKind::Texture3D) return image_layer_count(kind, layers);
  const uint32_t d = level >= 32 ? 0 : layers >> level;
  return d ? d : 1;
}
// A view of an image a texture unit is bound with (ICommandBuffer::bind_texture_view, kFeature2MipmappedLayers): the levels base_level .. base_level + level_count - 1 seen as levels
// 0 .. level_count - 1 (Direct3D's MostDetailedMip and MipLevels), the layers first_layer .. first_layer + layer_count - 1 (FirstArraySlice and ArraySize; a cube array's in faces,
// 6 a cube), and the kind the shader declared: an array may be seen as a 2D image (one layer), a cube (six layers) or a cube array (a multiple of six), a cube or a cube array as
// an array or a cube, a 2D image as an array of one. A count of 0 is "to the last".
struct TextureView {
  TextureKind kind = TextureKind::Texture2D;
  uint32_t base_level = 0, level_count = 0;
  uint32_t first_layer = 0, layer_count = 0;
};
// Whether an image of `kind` (made with `layers` and `levels`) may be seen as `view` (with its counts made whole: none 0, all inside the image).
constexpr bool texture_view_fits(TextureKind kind, uint32_t layers, uint32_t levels, const TextureView& view) {
  const uint32_t have = image_layer_count(kind, layers);
  if (view.level_count == 0 || view.base_level >= levels || view.level_count > levels - view.base_level) return false;
  if (view.layer_count == 0 || view.first_layer >= have || view.layer_count > have - view.first_layer) return false;
  const bool layered_2d = kind == TextureKind::Texture2DArray || kind == TextureKind::TextureCube || kind == TextureKind::TextureCubeArray;
  switch (view.kind) {
    case TextureKind::Texture2D: return kind == TextureKind::Texture2D || (layered_2d && view.layer_count == 1);
    case TextureKind::Texture2DArray: return kind == TextureKind::Texture2D || layered_2d;
    case TextureKind::TextureCube: return layered_2d && view.layer_count == 6;
    case TextureKind::TextureCubeArray: return layered_2d && view.layer_count % 6 == 0;
    case TextureKind::Texture3D: return kind == TextureKind::Texture3D && view.first_layer == 0 && view.layer_count == have;
    default: return false;
  }
}

struct ImageDesc {
  Extent2D extent;
  Format format = Format::B8G8R8A8Unorm;
  uint32_t usage = kImageColorAttachment;
  // A D32Float image can carry an 8-bit stencil plane next to the depth (the stencil test of a pipeline needs one). Devices without stencil refuse such an image.
  bool stencil = false;
  // The levels of a sampled colour image (Mipmaps.hpp): level 0 is `extent`, each next is half as large each way. More than one needs kImageSampled and no other usage on
  // the Intel Gen9 device; devices that cannot refuse the image. A level is filled by write_mip_pixels, or all of them from level 0 by generate_mipmaps.
  // With kFeature2MipmappedLayers any image of one sample may have levels: arrays, cubes, cube arrays and volumes (the levels of a volume half as deep each, level_layer_count),
  // colour and depth attachments (a pass draws into a level: RenderPassBegin::level), storage images, the wide formats. Its levels are filled and read with write_subresource and
  // read_subresource (or the _float forms).
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
  // Replaces a rectangle of a color image and leaves the rest as it is: `pixels` begins at the rectangle's top left pixel and its rows are `pitch` pixels apart (the rectangle's
  // last row must be inside `pixels`), 0xAARRGGBB, rows top to bottom. False if the device cannot do this (the default: write_pixels the whole image then), the rectangle is empty or
  // not inside the image, `pitch` is less than its width, `pixels` is too short, or as for write_pixels.
  virtual bool write_rect(const Rect2D& area, ArrayView<const uint32_t> pixels, uint32_t pitch) {
    (void)area, (void)pixels, (void)pitch;
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
  // A layer of an array of a wide format (the float, 16-bit and integer formats write_pixels_float takes): extent.width * extent.height colours, rows top to bottom. False as for
  // write_layer_pixels, or for a format that is not wide.
  virtual bool write_layer_pixels_float(uint32_t layer, ArrayView<const Vec4> pixels) {
    (void)layer, (void)pixels;
    return false;
  }
  // ... and read (a layered image a pass drew into, or a storage image shaders wrote).
  virtual bool read_layer_pixels_float(uint32_t layer, ArrayView<Vec4> out) {
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
  // ... and read back (a level a storage image's shaders wrote). Level 0 is read_pixels.
  virtual bool read_mip_pixels(uint32_t level, ArrayView<uint32_t> out) {
    (void)level, (void)out;
    return false;
  }
  // The blocks of level `level` of layer `layer` (0 for a plain texture and a volume, whose slices come one after the other) of a block-compressed image
  // (kFeatureCompressedFormats): Graphics/BlockCompression.hpp's compressed_image_bytes of the level's extent, rows of blocks top to bottom.
  virtual bool write_compressed(uint32_t level, uint32_t layer, ConstByteView blocks) {
    (void)level, (void)layer, (void)blocks;
    return false;
  }
  // Level `level` of layer `layer` of any image of one sample (kFeature2MipmappedLayers; a volume's level has level_layer_count slices, each a layer here): mip_extent() of the
  // extent, rows top to bottom, 0xAARRGGBB for the 8-bit formats; the _float forms for the wide ones. False for a level or layer the image does not have, a wrong size, a
  // format of the other kind, or a device that cannot. Without the feature the defaults take what the forms above take (level 0 of a layer, a level of a 2D image).
  virtual bool write_subresource(uint32_t level, uint32_t layer, ArrayView<const uint32_t> pixels) {
    if (level == 0) return layer_count() > 1 || layer != 0 ? write_layer_pixels(layer, pixels) : write_pixels(pixels);
    return layer == 0 && layer_count() == 1 && write_mip_pixels(level, pixels);
  }
  virtual bool read_subresource(uint32_t level, uint32_t layer, ArrayView<uint32_t> out) {
    if (level == 0) return layer_count() > 1 || layer != 0 ? read_layer_pixels(layer, out) : read_pixels(out);
    return layer == 0 && layer_count() == 1 && read_mip_pixels(level, out);
  }
  virtual bool write_subresource_float(uint32_t level, uint32_t layer, ArrayView<const Vec4> pixels) {
    if (level != 0) return false;
    return layer_count() > 1 || layer != 0 ? write_layer_pixels_float(layer, pixels) : write_pixels_float(pixels);
  }
  virtual bool read_subresource_float(uint32_t level, uint32_t layer, ArrayView<Vec4> out) {
    if (level != 0) return false;
    return layer_count() > 1 || layer != 0 ? read_layer_pixels_float(layer, out) : read_pixels_float(out);
  }
  // The planes of a depth image of one sample (kFeature2DepthCopies): the depth of level `level`'s layer `layer` as floats, and the stencil of an image that has one (ImageDesc::stencil)
  // as bytes, mip_extent() of the extent each, rows top to bottom; an empty view leaves its plane alone. False for a level or layer the image does not have, a plane it has not, a
  // wrong size, an image that is not a depth image, or a device that cannot (the default).
  virtual bool read_depth_subresource(uint32_t level, uint32_t layer, ArrayView<float> depth, ArrayView<uint8_t> stencil) {
    (void)level, (void)layer, (void)depth, (void)stencil;
    return false;
  }
  virtual bool write_depth_subresource(uint32_t level, uint32_t layer, ArrayView<const float> depth, ArrayView<const uint8_t> stencil) {
    (void)level, (void)layer, (void)depth, (void)stencil;
    return false;
  }
  // Copies the whole of level `source_level`'s layer `source_layer` of `source` into level `level`'s layer `layer` of this image, on the device (kFeature2DepthCopies): images of one
  // format and sample count whose subresources are the same size - depth images (`planes`: bit 0 the depth, bit 1 the stencil, which both must have) and multisampled colour images
  // (bit 0: the colour), every sample. False for anything else, or a device that cannot (the default).
  virtual bool copy_subresource(IImage& source, uint32_t level, uint32_t layer, uint32_t source_level, uint32_t source_layer, uint32_t planes) {
    (void)source, (void)level, (void)layer, (void)source_level, (void)source_layer, (void)planes;
    return false;
  }
  // Makes every level above 0 from level 0 by the 2 x 2 box filter of mip_box_downsample, each from the one before it (with kFeature2MipmappedLayers: every layer of an array or a
  // cube its own chain, a volume's levels by the 2 x 2 x 2 box of its slices). False when the image has one level or cannot.
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
// What a coordinate outside 0..1 means: wrap around, or take the edge texel. With kFeatureAddressModes also: wrap around with every second copy mirrored, take the sampler's
// border colour (a linear filter blends it in within half a texel of the edge), or mirror once about zero and then take the edge texel.
enum class AddressMode : uint8_t { Repeat, ClampToEdge, MirrorRepeat, ClampToBorder, MirrorOnce };
// A comparison (the depth and stencil tests, and a sampler's: SamplerDesc::compare), numbered as Gallium's PIPE_FUNC_* and as Direct3D's COMPARISON_FUNC minus one.
enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
// Whether `a` <op> `b` holds.
constexpr bool compare_holds(CompareOp op, float a, float b) {
  switch (op) {
    case CompareOp::Never: return false;
    case CompareOp::Less: return a < b;
    case CompareOp::Equal: return a == b;
    case CompareOp::LessEqual: return a <= b;
    case CompareOp::Greater: return a > b;
    case CompareOp::NotEqual: return a != b;
    case CompareOp::GreaterEqual: return a >= b;
    default: return true;
  }
}
// Where a TGSI shader that compares finds the comparisons of the bound samplers (ShaderModuleDesc::sampler_constants): the register after the most constants a shader may have, one for each unit.
constexpr uint32_t kSamplerCompareRegister = kMaxPushConstantBytes / 16;
// The values of that register for a sampler (see ShaderModuleDesc::sampler_constants).
inline void sampler_compare_constant(CompareOp op, bool linear, float out[4]) {
  out[0] = compare_holds(op, 0.0f, 1.0f) ? 1.0f : 0.0f;
  out[1] = compare_holds(op, 1.0f, 1.0f) ? 1.0f : 0.0f;
  out[2] = compare_holds(op, 1.0f, 0.0f) ? 1.0f : 0.0f;
  out[3] = linear ? 1.0f : 0.0f;
}
// The border colours a sampler can have (those of a Direct3D 12 static sampler).
enum class BorderColor : uint8_t { TransparentBlack, OpaqueBlack, OpaqueWhite };
constexpr Vec4 border_color_value(BorderColor b) { return b == BorderColor::OpaqueWhite ? Vec4{1, 1, 1, 1} : b == BorderColor::OpaqueBlack ? Vec4{0, 0, 0, 1} : Vec4{0, 0, 0, 0}; }
// Anisotropic filtering (kFeature2SamplerState, max_anisotropy above 1, with a Linear filter): from the same derivatives, the footprint of the pixel in the texture has a longer axis
// Pmax and a shorter Pmin (the lengths of the two derivative vectors, in texels of level 0); N = min(ceil(Pmax / Pmin), max_anisotropy) lookups are taken along the longer one, evenly spaced
// over its length, at the level of detail log2(Pmax / N) (with lod_bias and the clamps), and averaged - the reference method of EXT_texture_filter_anisotropic. An explicit level (SampleLevel)
// takes one lookup.
// A border colour of any value (kFeature2SamplerState: custom_border): border_rgba, the channels as the texture keeps them (an integer texture's as the bits of its integers), in place of
// `border`.
struct SamplerDesc {
  Filter filter = Filter::Nearest;
  AddressMode address_u = AddressMode::Repeat;
  AddressMode address_v = AddressMode::Repeat;
  BorderColor border = BorderColor::TransparentBlack;   // AddressMode::ClampToBorder
  MipFilter mip_filter = MipFilter::None;
  float lod_bias = 0.0f;
  float min_lod = 0.0f, max_lod = 1000.0f;
  // The comparison of a comparison lookup (Direct3D's SampleCmp, SampleCmpLevelZero, GatherCmp: ITextureSet::sample_compare): a texel passes (1) when reference <compare> texel holds, else 0; the
  // passes of the footprint are then filtered as `filter` says (Nearest: the texel the point lies in, Linear: the four around it weighted bilinearly). Other lookups do not compare.
  CompareOp compare = CompareOp::Never;
  // The plane of a depth image (D32Float with ImageDesc::stencil) a unit reads (kFeature2DepthTextures): false the depth, as (depth, 0, 0, 1); true the stencil, as the integers (0, stencil, 0, 1)
  // (Direct3D's X24_TYPELESS_G8_UINT / X32_TYPELESS_G8X24_UINT views, read with Load). Of the view, not the sampler: carried here so that every device's bind_texture takes it. A colour image
  // ignores it.
  bool stencil = false;
  uint8_t max_anisotropy = 1;   // 1 .. kMaxAnisotropy
  bool custom_border = false;
  float border_rgba[4] = {0, 0, 0, 0};
};
constexpr uint32_t kMaxAnisotropy = 16;
// The colour a sampler gives outside the image (AddressMode::ClampToBorder).
inline Vec4 sampler_border_value(const SamplerDesc& s) { return s.custom_border ? Vec4{s.border_rgba[0], s.border_rgba[1], s.border_rgba[2], s.border_rgba[3]} : border_color_value(s.border); }

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
  // (with kFeature2MipmappedLayers these take the levels of a layered image too: the lod picks and blends them as for a 2D image, a Load reads level `level`'s layer or slice z, and
  // GetDimensions gives the level's size - a volume's depth at that level - and the view's level count)
  // A cube array (TextureCubeArray, kFeature2MipmappedLayers): (x, y, z) a direction as for a cube, `cube` which cube of the array (rounded to the nearest, clamped); the level of
  // detail as sample_volume takes it, or `lod`. size_volume gives the number of cubes as the depth.
  virtual Vec4 sample_cube_array(uint32_t unit, float x, float y, float z, float cube) const {
    (void)cube;
    return sample_volume(unit, x, y, z);
  }
  virtual Vec4 sample_lod_cube_array(uint32_t unit, float x, float y, float z, float cube, float lod) const {
    (void)cube;
    return sample_lod_volume(unit, x, y, z, lod);
  }
  virtual Vec4 fetch_volume(uint32_t unit, int32_t x, int32_t y, int32_t z, uint32_t level) const {
    return z == 0 ? fetch(unit, x, y, level) : Vec4{0, 0, 0, 0};
  }
  virtual bool size_volume(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* depth, uint32_t* levels) const {
    *depth = 1;
    return size(unit, level, width, height, levels);
  }
  // One sample of a multisampled colour image (Texture2DMS Load): x and y are texel numbers, `sample` the sample; outside the image or the samples it is transparent black.
  virtual Vec4 fetch_sample(uint32_t unit, int32_t x, int32_t y, uint32_t sample) const {
    (void)unit, (void)x, (void)y, (void)sample;
    return {0, 0, 0, 0};
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
  // A comparison lookup (SamplerDesc::compare): the passes of the texels around (u, v) - and w as sample_volume takes it: an array's layer, a cube's direction - against `reference`,
  // filtered as the sampler of the unit filters (point or bilinear), from the level the derivatives pick (as sample() picks it; `level_zero`: level 0, SampleCmpLevelZero). 0..1.
  // This default compares the texels the sample_lod / sample_volume lookups give at the middles of the footprint's texels, at level 0, by compare_op.
  virtual float sample_compare(uint32_t unit, float u, float v, float w, float reference, bool level_zero) const {
    (void)w, (void)level_zero;
    uint32_t width = 0, height = 0, levels = 0;
    if (!size(unit, 0, &width, &height, &levels) || !width || !height || !(u == u) || !(v == v)) return 0.0f;
    const CompareOp op = compare_op(unit);
    const float fw = static_cast<float>(width), fh = static_cast<float>(height);
    auto floor_of = [](float f) {
      const float limited = f < -1e6f ? -1e6f : f > 1e6f ? 1e6f : f;
      const float t = static_cast<float>(static_cast<int64_t>(limited));
      return t > limited ? t - 1.0f : t;
    };
    const float px = u * fw - 0.5f, py = v * fh - 0.5f, x = floor_of(px), y = floor_of(py), fx = px - x, fy = py - y;
    float pass[2][2];
    for (uint32_t row = 0; row < 2; ++row)
      for (uint32_t col = 0; col < 2; ++col)
        pass[row][col] = compare_holds(op, reference, sample_lod(unit, (x + static_cast<float>(col) + 0.5f) / fw, (y + static_cast<float>(row) + 0.5f) / fh, 0.0f).x) ? 1.0f : 0.0f;
    const float top = pass[0][0] * (1.0f - fx) + pass[0][1] * fx, bottom = pass[1][0] * (1.0f - fx) + pass[1][1] * fx;
    return top * (1.0f - fy) + bottom * fy;
  }
  // The comparison of the sampler bound to `unit` (GatherCmp compares each of the four texels it gathers by it).
  virtual CompareOp compare_op(uint32_t unit) const {
    (void)unit;
    return CompareOp::LessEqual;
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
  // The layer of the pass's targets the primitive goes to (SV_RenderTargetArrayIndex, kFeature2LayeredTargets): written by a geometry shader - or, with kFeature2LayerFromAnyStage, by the
  // vertex or domain shader that feeds the rasterizer -; the primitive takes its first vertex's.
  uint32_t layer = 0;
  // The viewport and scissor rectangle the primitive is drawn with (SV_ViewportArrayIndex, kFeature2Viewports; from the same stages): its first vertex's, 0 when that is not below the
  // number set (ICommandBuffer::set_viewports).
  uint32_t viewport = 0;
  // The shading rate of the primitive (SV_ShadingRate, kFeature2ShadingRate): written by a vertex or geometry shader, a D3D12_SHADING_RATE value (see ShadingRate below); the primitive takes
  // its first vertex's. It counts only where the combiners of set_shading_rate take it.
  uint32_t shading_rate = 0;
};

// --- Storage images (DeviceInfo::features2 kFeature2StorageImages) -----------
// The images shaders read and write texel by texel (Direct3D's unordered access views of textures, RWTexture1D / 2D / 2DArray / 3D): slots 0 .. kMaxStorageImages - 1, the u# registers a
// shader declares as typed views of textures (a register is a storage buffer or a storage image by its declaration). A slot shows one level of the image and a run of its layers (an array's
// layers, a volume's slices), and the format is the image's own.
constexpr uint32_t kMaxStorageImages = 64;
// Typed views of buffers (kFeature2TexelBuffers; TexelBuffer.hpp): an unordered access view of a buffer (RWBuffer<>) is a storage image of the u# slot space (bind_storage_texel_buffer
// with a slot below kMaxStorageImages); a typed shader resource view of a buffer (Buffer<>, t#) is one in the slots after them (kTexelBufferSlotBase + n), read only. Such an image is one row
// of `elements` texels (size: width elements, height 1, one layer), the texel x the element.
constexpr uint32_t kMaxTexelBufferViews = 128;
constexpr uint32_t kTexelBufferSlotBase = kMaxStorageImages;
constexpr uint32_t kMaxImageSlots = kMaxStorageImages + kMaxTexelBufferViews;
struct TexelBufferView {
  uint64_t offset = 0;      // the first element's byte (a multiple of the element's size)
  uint32_t elements = 0;    // how many elements the view has
  TexelFormat format = TexelFormat::Undefined;
};
struct StorageImageView {
  uint32_t level = 0;         // the mip level (a mipmapped 2D image; 0 for the others, which have one)
  uint32_t first_layer = 0;   // the first layer (an array, a volume's first slice) ...
  uint32_t layer_count = 1;   // ... and how many from it the shader sees as z = 0 .. layer_count - 1
  // The format the shader reads and writes the texels as: the image's own (Undefined), or - for a wide image - another wide format of the same pixel size, whose values are the image's bits
  // seen that way (the views of a typeless Direct3D resource: an R32_UINT view of an R32_FLOAT image), or R8G8B8A8Uint of an R8G8B8A8Unorm image and the other way round.
  Format format = Format::Undefined;
};
// Whether a storage image of `image` may be seen as `view` (StorageImageView::format).
constexpr bool storage_view_compatible(Format image, Format view) {
  return view == Format::Undefined || view == image || (is_wide_format(image) && is_wide_format(view) && bytes_per_pixel(image) == bytes_per_pixel(view)) ||
         (image == Format::R8G8B8A8Unorm && view == Format::R8G8B8A8Uint) || (image == Format::R8G8B8A8Uint && view == Format::R8G8B8A8Unorm);
}
enum class ImageAtomic : uint8_t { Add, And, Or, Xor, IMax, IMin, UMax, UMin, Exchange, CompareExchange };
// What a software shader reads and writes the bound storage images (and the typed views of buffers: slots up to kMaxImageSlots) through. Values travel as the textures' do: floats for the normalised and float formats (what the format keeps of a stored
// value: clamped and rounded to its steps), the bits of the integers in the Vec4's components for the integer formats; a component the format does not have reads as 0, the alpha as 1 (the
// integer 1 for an integer format). A texel outside the view (or a slot with nothing bound) reads as zero, and a write there is dropped.
class IStorageImages {
 public:
  // The width and height of the view's level and its layers (1 for a view of one layer); false when nothing is bound.
  virtual bool size(uint32_t slot, uint32_t* width, uint32_t* height, uint32_t* layers) const = 0;
  virtual Vec4 load(uint32_t slot, uint32_t x, uint32_t y, uint32_t z) const = 0;
  virtual void store(uint32_t slot, uint32_t x, uint32_t y, uint32_t z, Vec4 value) const = 0;
  // An atomic operation on the first channel of a texel of a 32-bit integer image (Format::R32Uint, R32Sint): `old` gets the value before; CompareExchange stores `operand` where the
  // texel equals `compare`. False (and `old` 0) outside the view, for another format, or nothing bound.
  virtual bool atomic(uint32_t slot, uint32_t x, uint32_t y, uint32_t z, ImageAtomic op, uint32_t operand, uint32_t compare, uint32_t* old) const = 0;

 protected:
  constexpr IStorageImages() = default;
  ~IStorageImages() = default;
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
  const IStorageImages* images = nullptr;   // the storage images bound (kFeature2StorageImagesInDraws)
  const ByteView* storage = nullptr;   // the storage buffers bound (kFeatureStorageInDraws): kMaxStorageBuffers views, the shader resource views of buffers in the slots after the unordered access ones
  const ITextureSet* textures = nullptr;   // the textures bound for the draw (kFeatureTexelAccess): a vertex shader reads them at a level it names (no derivatives)
  uint32_t vertex_id = 0;     // the index of the vertex (or its number in a draw without indices), before the base vertex offset is added in
  uint32_t instance_id = 0;
  uint32_t view_id = 0;       // SV_ViewID (kFeature2ViewInstancing: ICommandBuffer::set_view_instance; 0 without)
};
struct FragmentContext {
  const IStorageImages* images = nullptr;   // ... as for the vertex shader
  const ByteView* storage = nullptr;   // ... as for the vertex shader
  bool helper = false;                 // a run made only to learn something about a neighbour (the level of detail, a derivative): it writes no buffer
  // The pixel's centre - with coarse shading (kFeature2ShadingRate) the coarse pixel's, whose size `shading_rate` says (a ShadingRate value, 0 when each pixel is shaded) -, its depth
  // and 1 / w there.
  Vec4 position;
  bool front_facing = true;
  uint32_t primitive_id = 0;
  uint32_t view_id = 0;        // SV_ViewID (kFeature2ViewInstancing)
  uint32_t shading_rate = 0;   // SV_ShadingRate (kFeature2ShadingRate)
  // Barycentrics (kFeature2Barycentrics): the weights of the primitive's three vertices, in the order the draw gave them (SV_Barycentrics), at the shaded point - perspective-correct, and
  // linear in the screen (noperspective) -, and the varyings of those vertices as their last stage before the rasterizer made them (GetAttributeAtVertex). A line has (1 - t, t, 0) and
  // its second vertex in the third place too, a point (1, 0, 0) and its vertex in all three. Null pointers where the device gives none.
  Vec4 barycentrics = {1, 0, 0, 0};
  Vec4 barycentrics_linear = {1, 0, 0, 0};
  const Vec4* vertex_varyings[3] = {};
  // Derivatives (ddx / ddy): the pixel is one of the four of a 2 x 2 quad (`quad_corner`: x & 1 | (y & 1) << 1), and `quad_fn` gives the varyings and the position of any corner, at its
  // pixel's centre, extended out of the primitive if it is not covered. A shader asks only when it takes a derivative; without the function (points, lines) the derivatives are zero.
  // (With coarse shading the quad is one of coarse pixels.) `quad_bary_fn`, when there is one, gives the barycentrics of a corner: perspective-correct, then linear.
  using QuadFn = void (*)(const void* user, uint32_t corner, Vec4* varyings, Vec4* position);
  using QuadBaryFn = void (*)(const void* user, uint32_t corner, Vec4* barycentrics);
  QuadFn quad_fn = nullptr;
  QuadBaryFn quad_bary_fn = nullptr;
  const void* quad_user = nullptr;
  uint32_t quad_corner = 0;
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
// Stream output (kFeatureStreamOutput): a geometry shader's vertices are written to buffers as well as (or instead of) being drawn. A geometry shader may have up to kMaxGeometryStreams
// output streams (with more than one, each stream is a list of points); each entry of a pipeline's declaration takes `component_count` components from `start_component` of one output of the
// vertices of `stream` - output 0 the position (as the shader wrote it, +Y up), output n (1 .. kMaxVaryings) varying n - 1 - and puts them, one after the other with the entries before it
// for the same buffer, in each record of that buffer (a record is `stride` bytes, the components 32-bit floats). A strip is written as a list: a triangle strip as triangles, with the
// vertices of every second one swapped as for drawing, a line strip as lines; a primitive that does not fit in a buffer is not written (nor is any after it).
constexpr uint32_t kMaxGeometryStreams = 4, kMaxStreamOutputEntries = 16, kMaxStreamOutputBuffers = 4;
constexpr uint64_t kStreamOutputAppend = ~uint64_t{0};   // bind_stream_output's offset for "where the buffer's last stream output ended"
struct StreamOutputEntry {
  uint8_t stream = 0, output = 0, start_component = 0, component_count = 0, buffer = 0;
};
struct GeometryInputData {
  const ByteView* storage = nullptr;   // the views of buffers the draw can read (kFeatureStorageInDraws), as for the vertex shader
  const IStorageImages* images = nullptr;   // the storage images of the draw (kFeature2StorageImagesAllStages)
  Vec4 position[kMaxGeometryInputVertices];
  Vec4 varying[kMaxGeometryInputVertices][kMaxVaryings];
  uint32_t primitive_id = 0;   // the number of the primitive in the draw
  uint32_t instance_id = 0;    // SV_GSInstanceID: which of the shader's instances this run is (it runs geometry_instances times for each primitive)
  uint32_t view_id = 0;        // SV_ViewID (kFeature2ViewInstancing)
};
struct GeometryOutputData {
  uint32_t count = 0;
  ShaderIo vertex[kMaxGeometryVertices];
  bool restart[kMaxGeometryVertices] = {};   // a strip ended just before this vertex (a cut)
  uint8_t stream[kMaxGeometryVertices] = {};   // the stream the vertex was emitted to (strips are cut per stream)
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
  const ByteView* storage = nullptr;
  const IStorageImages* images = nullptr;   // (kFeature2StorageImagesAllStages)
  uint32_t control_points = 0;
  uint32_t primitive_id = 0;
  uint32_t view_id = 0;        // SV_ViewID (kFeature2ViewInstancing)
  Vec4 position[kMaxPatchControlPoints];
  Vec4 varying[kMaxPatchControlPoints][kMaxVaryings];
};
struct HullOutputData {
  const ByteView* storage = nullptr;                // (the domain shader's views of buffers: the draw's)
  const IStorageImages* images = nullptr;           // (and its storage images, kFeature2StorageImagesAllStages)
  uint32_t view_id = 0;                             // (and SV_ViewID, kFeature2ViewInstancing)
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
constexpr uint32_t kMaxUavBuffers = 64;                           // u0 .. u63: storage buffers shaders read and write (Direct3D 11.1's 64 unordered access views)
constexpr uint32_t kMaxSrvBuffers = 128;                          // t0 .. t127 of a buffer: read-only storage buffers, in the slots after the u# ones (slot kMaxUavBuffers + n)
// The hidden counters of u0 .. u63 (append and consume buffers, counter views: imm_atomic_alloc / imm_atomic_consume): one 32-bit word each, a storage buffer bound like the others in
// slot kCounterSlotBase + n (read and written by the shaders that have the counter of u<n>).
constexpr uint32_t kCounterSlotBase = kMaxUavBuffers + kMaxSrvBuffers;
constexpr uint32_t kMaxStorageBuffers = kCounterSlotBase + kMaxUavBuffers;
// Whether storage slot `slot` is written by shaders (an unordered access view or a counter) or only read (a shader resource view of a buffer).
constexpr bool storage_slot_writable(uint32_t slot) { return slot < kMaxUavBuffers || (slot >= kCounterSlotBase && slot < kMaxStorageBuffers); }
// A set of slot numbers (bit n: slot n), for the slot spaces above 32.
template <uint32_t N>
struct SlotMask {
  uint64_t words[(N + 63) / 64] = {};
  constexpr bool test(uint32_t n) const { return n < N && ((words[n / 64] >> (n % 64)) & 1) != 0; }
  constexpr void set(uint32_t n) {
    if (n < N) words[n / 64] |= uint64_t{1} << (n % 64);
  }
  constexpr bool any() const {
    for (uint64_t w : words)
      if (w) return true;
    return false;
  }
  constexpr SlotMask& operator|=(const SlotMask& o) {
    for (uint32_t i = 0; i < (N + 63) / 64; ++i) words[i] |= o.words[i];
    return *this;
  }
};
constexpr uint32_t kMaxSharedMemories = 4;     // group-shared memories (g0 .. g3) of a compute shader (the group runner makes them: a context's `shared`)
constexpr uint32_t kMaxSharedBytes = 32768;   // all of them together (Direct3D's limit)
constexpr uint32_t kMaxBarrierThreads = 256;  // threads of a group that uses barriers (each has its own registers while it waits)
struct ComputeContext {
  uint32_t group_id[3] = {};      // SV_GroupID
  uint32_t thread_id[3] = {};     // SV_GroupThreadID
  uint32_t dispatch_id[3] = {};   // SV_DispatchThreadID: group_id * thread group size + thread_id
  uint32_t group_index = 0;       // SV_GroupIndex: the thread's number in its group
  ByteView buffers[kMaxStorageBuffers];
  ByteView shared[kMaxSharedMemories];   // (made by a group function for the group it runs)
  const ITextureSet* textures = nullptr;   // the textures bound for the dispatch (kFeatureTexelAccess): read at a level the shader names, texel fetches and sizes (no derivatives)
  const IStorageImages* images = nullptr;  // the storage images bound for the dispatch (kFeature2StorageImages)
};
using ComputeShaderFn = void (*)(const ComputeContext& context, const uint8_t* constants);
// A compute shader that uses group-shared memory or barriers is run a whole group at a time: the function runs all the threads of the group named by group_id (thread_id, dispatch_id and
// group_index of the context are ignored), keeping them in step at the barriers, with memory of its own for the group-shared registers.
using ComputeGroupShaderFn = ComputeShaderFn;
// Most threads in a group and most groups (in each dimension) of a dispatch.
constexpr uint32_t kMaxThreadsPerGroup = 1024;
constexpr uint32_t kMaxDispatchGroups = 65535;
constexpr uint64_t kMaxDispatchThreads = 1ull << 24;

// The QPU programs the Raspberry Pi's V3D device has for the built-in shaders (there is no shader compiler for the QPUs, so a module names a program it ships
// with instead of carrying code): the coloured-vertex pipeline of BuiltinShaders.
enum class QpuProgram : uint8_t { None, ColoredVertex, ColoredFragment };

// The pixel kernels the Intel Gen9 device has for the built-in shaders (there is no shader compiler for the EUs either).
enum class EuProgram : uint8_t { None, ColoredVertex, ColoredFragment, TexturedVertex, TexturedFragment };

// The host's binding points of a TGSI shader's storage (virgl), numbered densely: BUFFER[k] is storage slot storage[k], IMAGE[k] image slot images[k] (a u# one: a texture's view or a
// buffer's), SVIEW[kTgsiTexelViewBase + k] (with SAMP of the same number) the typed view of a buffer in image slot texels[k] (a t# one). In the text, IMGFMT<k> stands for the format of
// what is bound to IMAGE[k], IMGOFS<k> / IMGLEN<k> for the first element and the element count of a view of a buffer bound there, TBOFS<k> / TBLEN<k> for the elements the device's view of
// SVIEW[kTgsiTexelViewBase + k] starts before the view's first (0 when the host's view starts there) and the view's element count: the device writes the numbers in their places.
constexpr uint32_t kTgsiMaxBuffers = 32, kTgsiMaxImages = 32, kTgsiMaxTexelViews = 16, kTgsiTexelViewBase = kMaxTextureUnits;
struct TgsiBindings {
  uint16_t storage[kTgsiMaxBuffers] = {};
  uint16_t images[kTgsiMaxImages] = {};
  uint16_t texels[kTgsiMaxTexelViews] = {};
  uint32_t storage_count = 0, image_count = 0, texel_count = 0;
};

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
  ComputeGroupShaderFn compute_group_function = nullptr;   // compute: instead of compute_function, for a shader with group-shared memory or barriers
  uint32_t shared_memory_bytes = 0;                        // ... and how much group-shared memory it declares (virgl makes it with the shader)
  uint32_t geometry_max_vertices = 0;
  uint32_t geometry_instances = 1;   // runs of a geometry shader for each primitive (1 .. 32)
  uint32_t geometry_streams = 1;     // output streams of a geometry shader (1 .. kMaxGeometryStreams)
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
  // The text reads the comparison of the sampler bound to each texture unit u it compares with (SampleCmp, GatherCmp) at CONST[kSamplerCompareRegister + u]: (1 if reference < texel passes,
  // 1 if reference == texel passes, 1 if reference > texel passes, 1 for a linear filter, else 0 each) - the device puts them there from the bound SamplerDescs (sampler_compare_constant).
  bool sampler_constants = false;
  TgsiBindings tgsi_bindings;   // (the storage the text names, and where the device binds it)
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

// What a vertex attribute is in the vertex buffer. Float1 .. Float4 are one to four 32-bit floats, which every device reads. The others need kFeatureVertexFormats: a
// VertexType of one to four components (the value is type * 4 + components - 1), and two packed ones. Normalised types reach the shader as floats in 0..1 (Unorm) or -1..1
// (Snorm, the most negative value clamped); Uint and Sint types reach it as integers (the bits of an integer in the register, as an integer input of a Direct3D 10 shader
// expects). Components the format does not have are 0, the fourth 1 (the integer 1 for an integer type).
enum class VertexType : uint8_t { Float, Unorm8, Snorm8, Uint8, Sint8, Unorm16, Snorm16, Uint16, Sint16, Half, Uint32, Sint32 };
enum class VertexFormat : uint8_t {
  Float1, Float2, Float3, Float4,
  Unorm8x1 = 4, Unorm8x2, Unorm8x3, Unorm8x4, Snorm8x1 = 8, Snorm8x2, Snorm8x3, Snorm8x4, Uint8x1 = 12, Uint8x2, Uint8x3, Uint8x4, Sint8x1 = 16, Sint8x2, Sint8x3, Sint8x4,
  Unorm16x1 = 20, Unorm16x2, Unorm16x3, Unorm16x4, Snorm16x1 = 24, Snorm16x2, Snorm16x3, Snorm16x4, Uint16x1 = 28, Uint16x2, Uint16x3, Uint16x4,
  Sint16x1 = 32, Sint16x2, Sint16x3, Sint16x4, Half1 = 36, Half2, Half3, Half4, Uint32x1 = 40, Uint32x2, Uint32x3, Uint32x4, Sint32x1 = 44, Sint32x2, Sint32x3, Sint32x4,
  Bgra8Unorm = 48,     // four bytes B, G, R, A read as the colour (R, G, B, A)
  Rgb10A2Unorm = 49,   // a 32-bit word: R in bits 0-9, G in 10-19, B in 20-29, A in 30-31
  Rgb10A2Uint = 50,    // the same word, the four fields as unsigned integers
  Rg11B10Float = 51,   // a 32-bit word of three unsigned floats without a sign: R an 11-bit one in bits 0-10 (5-bit exponent, 6-bit mantissa), G in 11-21, B a 10-bit one in 22-31; A is 1
  Bgrx8Unorm = 52,     // four bytes B, G, R and one not read: the colour (R, G, B, 1)
};
constexpr uint32_t kVertexFormatCount = 53;
constexpr bool vertex_format_packed(VertexFormat f) { return static_cast<uint32_t>(f) >= static_cast<uint32_t>(VertexFormat::Bgra8Unorm); }
constexpr VertexType vertex_format_type(VertexFormat f) { return vertex_format_packed(f) ? VertexType::Unorm8 : static_cast<VertexType>(static_cast<uint32_t>(f) / 4); }
constexpr uint32_t vertex_format_components(VertexFormat f) { return vertex_format_packed(f) ? 4 : static_cast<uint32_t>(f) % 4 + 1; }
constexpr uint32_t vertex_type_bytes(VertexType t) {
  switch (t) {
    case VertexType::Unorm8: case VertexType::Snorm8: case VertexType::Uint8: case VertexType::Sint8: return 1;
    case VertexType::Unorm16: case VertexType::Snorm16: case VertexType::Uint16: case VertexType::Sint16: case VertexType::Half: return 2;
    default: return 4;
  }
}
// The bytes of one attribute in the vertex buffer.
constexpr uint32_t vertex_format_bytes(VertexFormat f) { return vertex_format_packed(f) ? 4 : vertex_type_bytes(vertex_format_type(f)) * vertex_format_components(f); }
constexpr bool vertex_format_is_integer(VertexFormat f) {
  const VertexType t = vertex_format_type(f);
  if (f == VertexFormat::Rgb10A2Uint) return true;
  return !vertex_format_packed(f) && (t == VertexType::Uint8 || t == VertexType::Sint8 || t == VertexType::Uint16 || t == VertexType::Sint16 || t == VertexType::Uint32 || t == VertexType::Sint32);
}

struct VertexAttribute {
  uint32_t location;  // which input of the vertex shader
  VertexFormat format;
  uint32_t offset;  // bytes from the start of the vertex
  bool per_instance = false;   // kFeatureInstancing: the attribute steps with the instance, not the vertex (element number below, at the same stride)
  uint8_t slot = 0;            // the vertex buffer it is read from (below DeviceLimits::vertex_buffers); slot 0's stride is GraphicsPipelineDesc::vertex_stride
  // A per-instance attribute's step rate (Direct3D's InstanceDataStepRate): instance n of a draw from first_instance reads element first_instance + n / step_rate, and with 0
  // every instance reads element first_instance. A per-vertex attribute does not look at it.
  uint32_t step_rate = 1;
};
// The element a per-instance attribute reads for instance `instance` (counted from 0) of a draw whose first instance is `first_instance`.
constexpr uint64_t instance_element(const VertexAttribute& a, uint32_t first_instance, uint32_t instance) { return uint64_t{first_instance} + (a.step_rate ? instance / a.step_rate : 0); }

// The stencil test of a pipeline, one state for front and back faces: a fragment passes when (reference & compare_mask) <compare> (stored & compare_mask); what is written back
// is (stored & ~write_mask) | (op result & write_mask), the op depending on whether the stencil test failed, the stencil test passed but the depth test failed, or both passed.
// It needs a stencil plane in the render pass's depth image (ImageDesc::stencil). The test comes before the depth test; a fragment a shader discards changes nothing.
enum class StencilOp : uint8_t { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
struct StencilState {
  bool enable = false;
  CompareOp compare = CompareOp::Always;
  StencilOp fail_op = StencilOp::Keep, depth_fail_op = StencilOp::Keep, pass_op = StencilOp::Keep;
  uint8_t compare_mask = 0xFF, write_mask = 0xFF, reference = 0;
};

// Blending (kFeatureBlendState): result = source * source_factor (op) destination * destination_factor, for the colour and the alpha on their own, then only the channels
// of write_mask (bit 0 red .. bit 3 alpha) are stored. Min / Max ignore the factors. With `custom` false the pipeline's alpha_blend decides (source over destination).
// The Constant factors (DeviceInfo::blend_constant) are the state's `constant` colour (each channel its own) or its alpha (every channel), and one minus those.
// The Src1 factors (kFeature2DualSourceBlend: Direct3D's dual-source blending) are the fragment shader's second colour output (SV_Target1) - its colour or its alpha, and one
// minus those - for blending into colour target 0; a pipeline with them draws into one colour target (color_target_count 1) and its fragment shader writes two outputs.
enum class BlendFactor : uint8_t { Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DstColor, InvDstColor, DstAlpha, InvDstAlpha, SrcAlphaSaturate,
                                   ConstantColor, InvConstantColor, ConstantAlpha, InvConstantAlpha, Src1Color, InvSrc1Color, Src1Alpha, InvSrc1Alpha };
constexpr uint32_t kBlendFactorCount = 19;
constexpr bool is_constant_blend_factor(BlendFactor f) { return f >= BlendFactor::ConstantColor && f <= BlendFactor::InvConstantAlpha; }
constexpr bool is_dual_source_blend_factor(BlendFactor f) { return f >= BlendFactor::Src1Color && f <= BlendFactor::InvSrc1Alpha; }
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
struct BlendState {
  bool custom = false;
  bool enable = false;
  BlendFactor src_color = BlendFactor::One, dst_color = BlendFactor::Zero, src_alpha = BlendFactor::One, dst_alpha = BlendFactor::Zero;
  BlendOp color_op = BlendOp::Add, alpha_op = BlendOp::Add;
  uint8_t write_mask = 0xF;
  float constant[4] = {0, 0, 0, 0};   // red, green, blue, alpha: the colour of the Constant factors
  bool uses_constant() const { return enable && (is_constant_blend_factor(src_color) || is_constant_blend_factor(dst_color) || is_constant_blend_factor(src_alpha) || is_constant_blend_factor(dst_alpha)); }
  bool uses_dual_source() const {
    return enable && (is_dual_source_blend_factor(src_color) || is_dual_source_blend_factor(dst_color) || is_dual_source_blend_factor(src_alpha) || is_dual_source_blend_factor(dst_alpha));
  }
};

// Logic operations (kFeature2LogicOps) in place of blending: what is stored is op(source, destination) on the bits of the target's channels (the shader's colour converted to the
// target's format first; Direct3D defines them for UINT targets, and for the normalised 8-bit ones they work on the stored bytes as OpenGL's do). A float or sRGB target is written as
// if there were none. Numbered as D3D11_LOGIC_OP / D3D12_LOGIC_OP.
enum class LogicOp : uint8_t { Clear, Set, Copy, CopyInverted, Noop, Invert, And, Nand, Or, Nor, Xor, Equiv, AndReverse, AndInverted, OrReverse, OrInverted };
constexpr uint32_t kLogicOpCount = 16;
constexpr uint32_t apply_logic_op(LogicOp op, uint32_t s, uint32_t d) {
  switch (op) {
    case LogicOp::Clear: return 0;
    case LogicOp::Set: return ~0u;
    case LogicOp::Copy: return s;
    case LogicOp::CopyInverted: return ~s;
    case LogicOp::Noop: return d;
    case LogicOp::Invert: return ~d;
    case LogicOp::And: return s & d;
    case LogicOp::Nand: return ~(s & d);
    case LogicOp::Or: return s | d;
    case LogicOp::Nor: return ~(s | d);
    case LogicOp::Xor: return s ^ d;
    case LogicOp::Equiv: return ~(s ^ d);
    case LogicOp::AndReverse: return s & ~d;
    case LogicOp::AndInverted: return ~s & d;
    case LogicOp::OrReverse: return s | ~d;
    case LogicOp::OrInverted: return ~s | d;
  }
  return s;
}

// How triangles are filled (kFeature2RasterizerState): Wireframe draws the edges of each triangle that is not culled as lines (an edge the clipping or a strip's split made inside a
// polygon is not drawn), with the triangle's depth and depth bias.
enum class FillMode : uint8_t { Solid, Wireframe };

// Depth bias (kFeature2RasterizerState), as Direct3D 10+ defines it: what is added to the depth of every fragment of a triangle is
//   bias = constant * r + slope * max(|dz/dx|, |dz/dy|)   (the slope of the triangle's window-space depth, per pixel)
// limited by `clamp` (above 0: at most clamp; below 0: at least clamp; 0: no limit). r is the depth format's unit: 1 / (2^unorm_bits - 1) for a normalised format of that many bits (16
// or 24), and for a float format (unorm_bits 0) 2^(e - 23), e the exponent of the largest depth of the triangle's vertices. The biased depth is clamped to the viewport's depth range.
// Lines and points are not biased (as OpenGL's polygon offset leaves them).
struct DepthBias {
  float constant = 0.0f;   // (Direct3D 10/11's integer DepthBias, Direct3D 12's RASTERIZER1/2 float one)
  float slope = 0.0f, clamp = 0.0f;
  uint8_t unorm_bits = 0;   // 0 (float), 16 or 24
  bool enabled() const { return constant != 0.0f || slope != 0.0f; }
};
// The unit r of a depth bias for a triangle whose largest window depth is `max_z`.
inline float depth_bias_unit(const DepthBias& b, float max_z) {
  if (b.unorm_bits) return 1.0f / static_cast<float>((1u << b.unorm_bits) - 1u);
  const uint32_t bits = __builtin_bit_cast(uint32_t, max_z < 0 ? -max_z : max_z);
  const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127;
  if (exponent - 23 < -126) return 0.0f;   // (zero or a denormal: a unit too small to be a float)
  return __builtin_bit_cast(float, static_cast<uint32_t>(exponent - 23 + 127) << 23);
}
// The bias of a triangle of that slope and largest depth.
inline float depth_bias_value(const DepthBias& b, float max_slope, float max_z) {
  float bias = b.constant * depth_bias_unit(b, max_z) + b.slope * max_slope;
  if (b.clamp > 0.0f && bias > b.clamp) bias = b.clamp;
  if (b.clamp < 0.0f && bias < b.clamp) bias = b.clamp;
  return bias == bias ? bias : 0.0f;
}

enum class CullMode : uint8_t { None, Back, Front };
enum class FrontFace : uint8_t { CounterClockwise, Clockwise };  // as seen on screen

struct GraphicsPipelineDesc {
  const IShaderModule* vertex_shader = nullptr;
  const IShaderModule* fragment_shader = nullptr;
  uint32_t vertex_stride = 0;
  uint32_t slot_strides[kMaxVertexBuffers] = {};   // the stride of vertex buffer n for n >= 1 (slot_strides[0] is not used)
  VertexAttribute attributes[kMaxVertexAttributes] = {};
  uint32_t attribute_count = 0;
  uint32_t varying_count = 0;  // values passed from the vertex to the fragment shader
  bool tessellation_stores = false; // the hull or the domain shader writes storage buffers (kFeatureTessellationStores)
  bool geometry_stores = false; // the geometry shader writes storage buffers (kFeatureGeometryStores)
  bool vertex_stores = false;    // the vertex shader writes storage buffers (kFeatureVertexStores)
  bool fragment_stores = false;  // the fragment shader writes storage buffers (kFeatureFragmentStores): it runs for every fragment the rasterizer makes, also those the depth test then fails
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
  // kFeatureStreamOutput, with a geometry shader: where the vertices it emits are also written (see StreamOutputEntry), and which stream is drawn (-1: none).
  uint32_t stream_output_count = 0;
  StreamOutputEntry stream_output[kMaxStreamOutputEntries] = {};
  uint32_t stream_output_stride[kMaxStreamOutputBuffers] = {};
  int32_t rasterized_stream = 0;
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
  // kFeatureClipDistances: which clip distances the last stage before the rasterizer writes (TGSI: CLIPDIST[0] .x .. .w are 0 .. 3, CLIPDIST[1] 4 .. 7) cut the primitives:
  // bit i, distance i (what is below zero is cut away).
  uint8_t clip_distances = 0;
  // kFeature2ConservativeRaster: a triangle covers every pixel (and every sample of it) whose square its area overlaps, not only those whose centre or samples it contains - Direct3D's
  // conservative rasterization, tier 1 (the attributes and depth are those of the triangle's plane at the pixel's centre, extrapolated where the centre is outside; degenerate triangles are
  // culled). Lines and points are drawn as without it.
  bool conservative = false;
  // kFeature2RasterizerState: how triangles are filled, the depth bias, whether primitives are clipped against the near and far planes (0 <= z <= w) - without it they are not, and
  // every fragment's depth is clamped to the viewport's range instead -, and the stencil state of back faces (with separate_back_stencil: back_stencil's compare, ops, masks
  // and reference; `enable` is always stencil's; lines and points are front faces). Without it these have their defaults.
  FillMode fill_mode = FillMode::Solid;
  DepthBias depth_bias;
  bool depth_clip = true;
  bool separate_back_stencil = false;
  StencilState back_stencil;
  // kFeature2LogicOps: a logic operation in place of blending, for every colour target (the blend state is then not used).
  bool logic_op_enable = false;
  LogicOp logic_op = LogicOp::Noop;
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
enum class ResolveMode : uint8_t { Average, Sample0, Min, Max };
enum class QueryKind : uint8_t { Occlusion, OcclusionPredicate, IaVertices, IaPrimitives, VsInvocations, PsInvocations, Timestamp, SoPrimitivesWritten, SoPrimitivesNeeded, TimestampFrequency, TimestampDisjoint,
                                 PipelineStatistics };
// The values of a PipelineStatistics query (ICommandBuffer::query_value), in Direct3D's order (D3D11_QUERY_DATA_PIPELINE_STATISTICS, Gallium's pipe_query_data_pipeline_statistics).
enum class PipelineStatistic : uint8_t { IaVertices, IaPrimitives, VsInvocations, GsInvocations, GsPrimitives, ClipperInvocations, ClipperPrimitives, PsInvocations, HsInvocations, DsInvocations, CsInvocations };
constexpr uint32_t kPipelineStatisticCount = 11;
constexpr uint32_t kMaxQueryValues = kPipelineStatisticCount;   // (the most values one query has)
enum class PrimitiveTopology : uint8_t { TriangleList, LineList, PointList, PatchList, LineListAdjacency, TriangleListAdjacency };   // (the adjacency lists need a geometry shader that takes them)   // (PatchList: kFeatureTessellation, with set_patch_control_points)

struct Viewport {
  float x = 0, y = 0, width = 0, height = 0;
  float min_depth = 0, max_depth = 1;
};

// Where a sample of a pixel is (kFeature2SamplePositions): sixteenths of a pixel from its centre, -8 .. 7 each way (+y down), as D3D12_SAMPLE_POSITION.
struct SamplePosition {
  int8_t x = 0, y = 0;
};
constexpr uint32_t kMaxSamplePositions = 16;   // (the most a set_sample_positions call carries: samples x pixels)

// Coarse pixel shading (kFeature2ShadingRate). A rate is D3D12_SHADING_RATE: (x axis << 2) | y axis, an axis 0 for 1 pixel, 1 for 2, 2 for 4 (0x0 1x1, 0x1 1x2, 0x4 2x1, 0x5 2x2,
// 0x6 2x4, 0x9 4x2, 0xA 4x4). The rate of a pixel is combiner 1 of (combiner 0 of (the rate set, the primitive's), the image's): a combiner is D3D12_SHADING_RATE_COMBINER, applied to each
// axis (passthrough keeps the first, override takes the second, min, max, sum - the axes added, at most 4x). A rate the samples of the pass do not allow is made smaller: with two or more
// samples an axis is at most 2x, 4x of the other axis only with two, and a coarse pixel never covers more than 16 samples. The coarse pixels lie on a grid from the target's origin, inside
// the tiles of the shading-rate image (kShadingRateTileSize pixels square), one rate a tile for each primitive.
enum class ShadingRateCombiner : uint8_t { Passthrough = 0, Override = 1, Min = 2, Max = 3, Sum = 4 };
constexpr uint32_t kShadingRateTileSize = 16;
constexpr bool valid_shading_rate(uint32_t rate) { return rate == 0x0 || rate == 0x1 || rate == 0x4 || rate == 0x5 || rate == 0x6 || rate == 0x9 || rate == 0xA; }
constexpr uint32_t shading_rate_x(uint32_t rate) { return 1u << (rate >> 2 & 3); }   // (the coarse pixel's width in pixels)
constexpr uint32_t shading_rate_y(uint32_t rate) { return 1u << (rate & 3); }
// One combiner on two rates (each axis; an invalid rate counts as 1x1).
constexpr uint32_t combine_shading_rate(ShadingRateCombiner c, uint32_t a, uint32_t b) {
  if (!valid_shading_rate(a)) a = 0;
  if (!valid_shading_rate(b)) b = 0;
  const uint32_t ax = a >> 2 & 3, ay = a & 3, bx = b >> 2 & 3, by = b & 3;
  uint32_t x = ax, y = ay;
  switch (c) {
    case ShadingRateCombiner::Passthrough: break;
    case ShadingRateCombiner::Override: x = bx, y = by; break;
    case ShadingRateCombiner::Min: x = ax < bx ? ax : bx, y = ay < by ? ay : by; break;
    case ShadingRateCombiner::Max: x = ax > bx ? ax : bx, y = ay > by ? ay : by; break;
    case ShadingRateCombiner::Sum: x = ax + bx > 2 ? 2 : ax + bx, y = ay + by > 2 ? 2 : ay + by; break;
  }
  return x << 2 | y;
}
// The rate as the pass's `samples` take it (and a combination that is not a rate - 4x1, 1x4 - as the nearest one that is).
constexpr uint32_t sanitize_shading_rate(uint32_t rate, uint32_t samples) {
  uint32_t x = rate >> 2 & 3, y = rate & 3;
  if (x > 2) x = 2;
  if (y > 2) y = 2;
  if (samples >= 2 && x > 1) x = 1;                       // (4x across only without multisampling)
  if (samples >= 4 && y > 1) y = 1;                       // (2x4 only with two samples)
  while ((1u << x) * (1u << y) * (samples ? samples : 1) > 16) {   // (at most 16 samples a coarse pixel)
    if (y >= x && y > 0) --y;
    else if (x > 0) --x;
    else break;
  }
  if (x == 2 && y == 0) x = 1;   // (4x1 and 1x4 are not rates: the finer 2x1 and 1x2)
  if (y == 2 && x == 0) y = 1;
  return x << 2 | y;
}

// The queries one command buffer can hold (kFeatureOcclusionQueries): slots 0 .. kMaxOcclusionQueries - 1.
constexpr uint32_t kMaxOcclusionQueries = 256;

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
  // The layers drawn into (kFeature2LayeredTargets: colour and depth images that are arrays, volumes or cubes): target n (0 `color`, n `extra_colors[n - 1]`, kMaxColorTargets the depth
  // image) has the layers first_layer[n] .. first_layer[n] + layer_count[n] - 1, and a primitive goes to the one of them its ShaderIo::layer names (the first when that is not below
  // layer_count[n]). A clear clears those layers. A 2D image has layer 0 alone.
  uint32_t first_layer[kMaxColorTargets + 1] = {};
  uint32_t layer_count[kMaxColorTargets + 1] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
  // The level of each target drawn into (kFeature2MipmappedLayers; the same numbering): all the targets' levels are of one size, and the pass is that size. The layers above are
  // the layers of that level (a volume's level has level_layer_count slices).
  uint32_t level[kMaxColorTargets + 1] = {};
};

class ICommandBuffer {
 public:
  virtual void begin() = 0;
  virtual void begin_render_pass(const RenderPassBegin& pass) = 0;
  virtual void bind_pipeline(const IPipeline* pipeline) = 0;
  virtual void bind_vertex_buffer(IBuffer* buffer, uint64_t offset = 0) = 0;
  // Vertex buffer `slot` (below DeviceLimits::vertex_buffers; the attributes of a pipeline name their slot). A device with one vertex buffer takes slot 0 only.
  virtual void bind_vertex_buffer_slot(uint32_t slot, IBuffer* buffer, uint64_t offset = 0) {
    if (slot == 0) bind_vertex_buffer(buffer, offset);
  }
  virtual void bind_index_buffer(IBuffer* buffer, IndexType type, uint64_t offset = 0) = 0;
  // Sets the shader constants (at most kMaxPushConstantBytes, a multiple of 16).
  virtual void push_constants(const void* data, uint32_t size) = 0;
  // Binds a sampled image (created with kImageSampled) to a texture unit with a sampler, for the draws that follow. A render pass may not draw into an image it samples.
  virtual void bind_texture(uint32_t unit, IImage* image, const SamplerDesc& sampler) = 0;
  // The same through a view (kFeature2MipmappedLayers: texture_view_fits): the shader sees the view's levels and layers as the whole image, of the view's kind. A pass may draw into
  // levels or layers of an image a unit samples outside its view. A device without the feature binds the whole image.
  virtual void bind_texture_view(uint32_t unit, IImage* image, const SamplerDesc& sampler, const TextureView& view) {
    (void)view;
    bind_texture(unit, image, sampler);
  }
  // Defaults to the whole render target.
  virtual void set_viewport(const Viewport& viewport) = 0;
  virtual void set_scissor(const Rect2D& scissor) = 0;
  // Several viewports and scissor rectangles (kFeature2Viewports: 1 .. kMaxViewports; a primitive is drawn with those its ShaderIo::viewport names). set_viewport and set_scissor set one.
  virtual void set_viewports(ArrayView<const Viewport> viewports) {
    if (!viewports.empty()) set_viewport(viewports[0]);
  }
  virtual void set_scissors(ArrayView<const Rect2D> scissors) {
    if (!scissors.empty()) set_scissor(scissors[0]);
  }
  // The state below holds for the draws that follow until it is set again, across passes; begin() sets the defaults. A device without the feature ignores the call.
  // Depth bounds (kFeature2DepthBounds): with `enable` a sample whose depth buffer value (before the draw) is outside min .. max is not drawn - no shader result, depth or stencil is
  // written for it, and an occlusion query does not count it -; a pass without a depth buffer passes every sample. Default: off, 0 .. 1.
  virtual void set_depth_bounds(bool enable, float min, float max) { (void)enable, (void)min, (void)max; }
  // Sample positions (kFeature2SamplePositions): `samples` (1, 2, 4, 8 or 16) positions for each of `pixels` pixels - 1: every pixel has the same; 4: the pixels of each 2 x 2 quad
  // aligned to even coordinates, upper left, upper right, lower left, lower right - in place of the standard ones, for triangles' coverage and the depth at each sample. `samples` must be
  // the pass's samples (a single-sample pass then covers a pixel by its one sample's position). pixels 0: the standard positions again.
  virtual void set_sample_positions(uint32_t samples, uint32_t pixels, ArrayView<const SamplePosition> positions) { (void)samples, (void)pixels, (void)positions; }
  // View instancing (kFeature2ViewInstancing): the view the draws that follow make, which every stage reads as SV_ViewID, and what is added (as 32-bit unsigned numbers, wrapping) to the
  // viewport and the layer of each primitive (ShaderIo::viewport / layer: 0 when the shaders write neither). Default: view 0, nothing added.
  virtual void set_view_instance(uint32_t view_id, uint32_t viewport_offset, uint32_t layer_offset) { (void)view_id, (void)viewport_offset, (void)layer_offset; }
  // Coarse pixel shading (kFeature2ShadingRate, see ShadingRate above): the rate set and the two combiners. Default: 1x1, passthrough, passthrough.
  virtual void set_shading_rate(uint32_t rate, ShadingRateCombiner primitive, ShadingRateCombiner image) { (void)rate, (void)primitive, (void)image; }
  // ... and the shading-rate image (an R8Uint image of one level and one sample, kImageSampled; nullptr: none, 1x1 everywhere): texel (x, y) is the rate of the tile of pixels from
  // (x, y) * kShadingRateTileSize; a tile past the image's edge is 1x1. It is read when the draws run.
  virtual void set_shading_rate_image(IImage* image) { (void)image; }
  // Several instances (kFeatureInstancing): the draw is made instance_count times with instance numbers first_instance ..; a device without it draws once.
  virtual void draw(uint32_t vertex_count, uint32_t first_vertex = 0, uint32_t instance_count = 1, uint32_t first_instance = 0) = 0;
  virtual void draw_indexed(uint32_t index_count, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t instance_count = 1, uint32_t first_instance = 0) = 0;
  // What the vertices of the draws that follow make (kFeatureLines for lines and points; the default is a list of triangles).
  virtual void set_primitive_topology(PrimitiveTopology topology) = 0;
  // How many control points a patch of the PatchList draws that follow has (1 .. kMaxPatchControlPoints).
  virtual void set_patch_control_points(uint32_t count) { (void)count; }
  virtual void end_render_pass() = 0;
  // Averages the samples of a multisampled colour image into a single-sample image of the same format and size (kFeatureMultisample), outside a render pass.
  // The image may be a depth image (kFeatureDepthResolve): then the destination gets the depth of sample 0 (a stencil plane that comes along: sample 0's too). `mode` says how the samples are
  // combined: Average (the default, colour only, or depth with kFeatureResolveModes), Sample0, Min, Max (per channel of a colour image, of the depth of a depth image): all but the average of a colour image and
  // the sample 0 of a depth image are kFeatureResolveModes.
  virtual void resolve_image(IImage* destination, IImage* source, ResolveMode mode = ResolveMode::Average) {
    (void)destination, (void)source, (void)mode;
  }
  // Copies `region` of a multisampled image - every sample of each pixel, a depth image's depth and stencil - to (x, y) of another of the same format, sample count and kind (kFeature2SampleCopies),
  // outside a render pass; both regions inside their images, the two images not the same.
  virtual void copy_samples(IImage* destination, IImage* source, const Rect2D& region, uint32_t x, uint32_t y) { (void)destination, (void)source, (void)region, (void)x, (void)y; }
  // Compute (kFeatureCompute), outside a render pass: bind_compute_pipeline makes the pipeline of create_compute_pipeline current for the dispatches that follow, bind_storage_buffer
  // puts `size` bytes of a kBufferStorage buffer from `offset` in slot 0 .. kMaxStorageBuffers - 1, and dispatch runs x * y * z thread groups. A device without the feature ignores all three.
  virtual void bind_compute_pipeline(const IPipeline* pipeline) { (void)pipeline; }
  // Stream output (kFeatureStreamOutput): makes `buffer` (kBufferStreamOutput) from `offset` the target of stream output buffer `slot` (a null buffer unbinds it); what the draws that follow
  // write goes after what is there already (a bind starts at its offset; with kStreamOutputAppend, where the last stream output into that buffer ended, IBuffer::stream_output_filled, which a
  // submit updates). Outside a render pass or in one.
  virtual void bind_stream_output(uint32_t slot, IBuffer* buffer, uint64_t offset) { (void)slot, (void)buffer, (void)offset; }
  virtual void bind_storage_buffer(uint32_t slot, IBuffer* buffer, uint64_t offset, uint64_t size) {
    (void)slot, (void)buffer, (void)offset, (void)size;
  }
  virtual void dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) { (void)groups_x, (void)groups_y, (void)groups_z; }
  // Storage images (kFeature2StorageImages; inside a pass kFeature2StorageImagesInDraws, for pixel shaders): makes `view` of `image` (a kImageStorage image; nullptr unbinds) slot
  // 0 .. kMaxStorageImages - 1 for the dispatches and draws that follow. What the shaders write is in the image after the submit. A device without the feature ignores it.
  virtual void bind_storage_image(uint32_t slot, IImage* image, const StorageImageView& view) { (void)slot, (void)image, (void)view; }
  // Typed views of buffers (kFeature2TexelBuffers): makes `view` of `buffer` (kBufferStorage; nullptr unbinds) image slot `slot` (below kMaxImageSlots: the u# ones below kMaxStorageImages,
  // read and written, and the t# ones from kTexelBufferSlotBase, read) for the dispatches and draws that follow. The view lies in the buffer and its offset is a multiple of the element.
  virtual void bind_storage_texel_buffer(uint32_t slot, IBuffer* buffer, const TexelBufferView& view) { (void)slot, (void)buffer, (void)view; }
  // Queries (kFeatureOcclusionQueries). Occlusion counts, from begin to end, the fragments that pass the stencil and depth tests (and that the shader did not discard) - one per sample, however
  // many targets the pipeline writes and whatever the blending or write masks do. `slot` (below kMaxOcclusionQueries, each used once in a command buffer) names the result. Any number of queries
  // may be open at once, of one kind or of several, nested or overlapping, begun and ended inside or outside render passes (what the draws and dispatches between begin and end did counts, and
  // nothing else: clears, copies, resolves and mipmaps do not); every query begun is ended before end(). A device without the feature ignores them.
  virtual void begin_occlusion_query(uint32_t slot) { (void)slot; }
  // The other queries (kFeatureStatisticsQueries), read with query_value: OcclusionPredicate is 1 if any fragment passed, else 0; IaVertices the vertices
  // the draws between begin and end took (a draw of n vertices and i instances: n * i), IaPrimitives the primitives they assembled (a patch is a primitive), VsInvocations the vertex shader
  // runs (a device may run it fewer times than there are vertices, by reusing results) and PsInvocations the fragment shader runs (a device may shade more pixels than it writes).
  // Timestamp (kFeatureTimestampQueries): the time the query ends, in nanoseconds of a clock that never goes back (the software rasterizer's is a count that goes up by 1000 for each timestamp, unless the
  // kernel gives it a clock). TimestampFrequency (kFeatureTimestampQueries) is the ticks a second of the timestamps (1000000000: they are nanoseconds) and TimestampDisjoint 1 if the clock was
  // interrupted (a frequency change, a reset) while the query was open, else 0: a device that cannot tell says 0. Neither needs a draw. SoPrimitivesWritten / SoPrimitivesNeeded (kFeatureStatisticsQueries with kFeatureStreamOutput), of the output stream `stream`: the primitives the draws between begin and
  // end wrote to stream output buffers, and those the geometry shader made for that stream, whether or not they fit. PipelineStatistics has kPipelineStatisticCount values (PipelineStatistic):
  // the vertices and primitives assembled, the vertex shader runs, the geometry shader runs (once for each primitive and instance it takes) and the primitives it made (on every stream), the
  // primitives given to the clipper and those that came out of it (not wholly outside the view volume), the fragment shader runs, the hull shader runs (one a patch), the domain shader runs and
  // the compute shader threads the dispatches ran.
  virtual void begin_query(uint32_t slot, QueryKind kind, uint32_t stream = 0) {
    (void)stream;
    if (kind == QueryKind::Occlusion) begin_occlusion_query(slot);
  }
  // Ends the query most recently begun that is still open.
  virtual void end_occlusion_query() {}
  // Ends the open query of `slot`.
  virtual void end_query(uint32_t slot) {
    (void)slot;
    end_occlusion_query();
  }
  // After submit(): the count of a query that was begun and ended (false for a slot that was not).
  virtual bool occlusion_result(uint32_t slot, uint64_t* samples) const {
    (void)slot, (void)samples;
    return false;
  }
  // After submit(): value `index` of a query's result (index 0 is occlusion_result; a PipelineStatistics query has kPipelineStatisticCount). False for a slot not begun and ended, or past its values.
  virtual bool query_value(uint32_t slot, uint32_t index, uint64_t* value) const { return index == 0 && occlusion_result(slot, value); }
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
  kFeatureTessellationStores = 1u << 27, // hull and domain shaders write unordered access buffers (the same bindings as kFeatureFragmentStores); GraphicsPipelineDesc::tessellation_stores
  kFeatureGeometryStores = 1u << 26,     // geometry shaders write unordered access buffers (the same bindings as kFeatureFragmentStores); GraphicsPipelineDesc::geometry_stores
  kFeatureVertexStores = 1u << 25,       // vertex shaders write unordered access buffers (the same bindings as kFeatureFragmentStores); GraphicsPipelineDesc::vertex_stores
  kFeatureFragmentStores = 1u << 24,     // pixel shaders write unordered access buffers (bind_storage_buffer in the slots u0 .. u3, also inside a pass); GraphicsPipelineDesc::fragment_stores
  kFeatureStorageInDraws = 1u << 23,     // read-only storage buffers (bind_storage_buffer in the slots after the unordered access ones, also inside a pass) read by vertex and pixel shaders
  kFeatureTimestampQueries = 1u << 22,   // QueryKind::Timestamp
  kFeatureStreamOutput = 1u << 21,       // GraphicsPipelineDesc::stream_output, ICommandBuffer::bind_stream_output, kBufferStreamOutput buffers, ShaderModuleDesc::geometry_streams
  kFeatureDepthResolve = 1u << 19,       // resolve_image of a multisampled depth image (sample 0)
  kFeatureResolveModes = 1u << 20,       // ResolveMode other than a colour image's average and a depth image's sample 0
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
  kFeatureClipDistances = 1u << 28,    // GraphicsPipelineDesc::clip_distances (TGSI shaders' CLIPDIST outputs)
  kFeatureVertexFormats = 1u << 29,    // VertexFormat values other than Float1 .. Float4
  kFeatureCompressedFormats = 1u << 30,   // Format::Bc1Unorm .. Bc3Srgb sampled images (any kind, mip levels): IImage::write_compressed; without it the memory manager decodes them
  kFeatureAddressModes = 1u << 31,     // AddressMode::MirrorRepeat, ClampToBorder and MirrorOnce, SamplerDesc::border
};
// The second word of features (DeviceInfo::features2).
enum DeviceFeature2 : uint32_t {
  kFeature2StorageImages = 1u << 0,         // kImageStorage images, ICommandBuffer::bind_storage_image, IStorageImages in compute shaders (ComputeContext::images)
  kFeature2StorageImagesInDraws = 1u << 1,  // ... also bound inside passes and read and written by pixel shaders (FragmentContext::images): they run for every fragment the rasterizer makes
  kFeature2LayeredTargets = 1u << 2,        // colour and depth attachments that are arrays, volumes or cubes (one level), RenderPassBegin::first_layer / layer_count, ShaderIo::layer from geometry shaders
  kFeature2SingleChannelFormats = 1u << 3,  // Format::R32Uint, R32Sint, R32Float: render targets, textures and storage images (the integer ones with atomics)
  kFeature2ConservativeRaster = 1u << 4,    // GraphicsPipelineDesc::conservative: a triangle covers every pixel its area touches (Direct3D's conservative rasterization)
  // Depth images that are textures too (kImageDepthAttachment | kImageSampled, one sample and one level - any with kFeature2MipmappedLayers -, of any kind but a volume): sampled, compared (SamplerDesc::compare) and loaded, their
  // depth or - SamplerDesc::stencil - their stencil plane. (A high bit: the bits between are left for the other features being added.)
  kFeature2DepthTextures = 1u << 16,
  // ... and the depth and the stencil plane of one of them read in one draw or dispatch (two units bound to it, one with SamplerDesc::stencil). A device without it (virgl: the host's
  // texture reads one plane at a time, GL_DEPTH_STENCIL_TEXTURE_MODE) is not given such a draw: the kernel refuses it.
  kFeature2DepthAndStencilPlanes = 1u << 17,
  kFeature2TexelBuffers = 1u << 5,          // typed views of buffers (bind_storage_texel_buffer, TexelBuffer.hpp): in the shaders that take storage images
  kFeature2StorageImagesAllStages = 1u << 6,   // storage images and typed views of buffers in vertex, geometry, hull and domain shaders too (VertexContext / GeometryInputData / HullInputData::images)
  // Images of several levels of every kind and usage (ImageDesc::mip_levels: arrays, cubes, volumes, targets, depth, storage and wide images), cube arrays (TextureKind::TextureCubeArray),
  // IImage::write_subresource / read_subresource, texture views (ICommandBuffer::bind_texture_view), passes that draw into a level (RenderPassBegin::level), storage images of a cube's
  // faces (as an array) and of any level of a layered image (StorageImageView::level).
  kFeature2MipmappedLayers = 1u << 10,
  kFeature2ColorResolveModes = 1u << 14,    // resolve_image of a colour image with ResolveMode::Min / Max (what kFeatureResolveModes has, without the depth modes)
  kFeature2RasterizerState = 1u << 7,       // GraphicsPipelineDesc::fill_mode, depth_bias, depth_clip, separate_back_stencil / back_stencil
  kFeature2DualSourceBlend = 1u << 8,       // the BlendFactor Src1* factors
  kFeature2LogicOps = 1u << 9,              // GraphicsPipelineDesc::logic_op_enable / logic_op
  kFeature2SamplerState = 1u << 11,         // SamplerDesc::max_anisotropy and custom_border / border_rgba
  kFeature2Viewports = 1u << 12,            // ICommandBuffer::set_viewports / set_scissors and ShaderIo::viewport from geometry shaders
  kFeature2LayerFromAnyStage = 1u << 13,    // ShaderIo::layer and ShaderIo::viewport written by vertex and domain shaders (Direct3D 11.3's VPAndRTArrayIndexFromAnyShaderFeedingRasterizer)
  // The contents of the images the system keeps no memory of: IImage::read_depth_subresource / write_depth_subresource (a depth image's depth and stencil planes) and
  // IImage::copy_subresource (a whole subresource of a depth image or of a multisampled image into another of the same format, size and samples, on the device). (Bit 24: a high bit,
  // the ones between are left for the other features being added.)
  kFeature2DepthCopies = 1u << 24,
  // The optional rasterizer features of Direct3D 12 (high bits: the ones below are left for the other features being added).
  kFeature2DepthBounds = 1u << 30,          // ICommandBuffer::set_depth_bounds (OPTIONS2 DepthBoundsTestSupported)
  kFeature2SamplePositions = 1u << 25,      // ICommandBuffer::set_sample_positions, one pattern or one for each pixel of a 2 x 2 quad (ProgrammableSamplePositionsTier 2)
  kFeature2ViewInstancing = 1u << 26,       // ICommandBuffer::set_view_instance: the view id every stage reads (SV_ViewID) and the offsets of the primitives' viewport and layer
  kFeature2ShadingRate = 1u << 31,          // coarse pixel shading: ICommandBuffer::set_shading_rate / set_shading_rate_image, ShaderIo::shading_rate, FragmentContext::shading_rate (VariableShadingRateTier 2)
  kFeature2Barycentrics = 1u << 28,         // FragmentContext::barycentrics / barycentrics_linear / vertex_varyings (SV_Barycentrics, GetAttributeAtVertex)
  kFeature2SampleCopies = 1u << 29,         // ICommandBuffer::copy_samples: a region of a multisampled image, every sample, into another of its format and samples (D3D12_RESOLVE_MODE_DECOMPRESS)
};
constexpr uint32_t kMaxViewports = 16;


struct DeviceInfo {
  const char* name;
  DeviceKind kind;
  bool accelerated;  // rendering is done by a GPU, not by the CPU
  uint32_t max_push_constant_bytes = 128;   // what push_constants() may be given (a multiple of 16, up to kMaxPushConstantBytes)
  uint32_t features = 0;                    // DeviceFeature bits
  uint32_t sample_counts = 0;               // kFeatureMultisample: bit n set: ImageDesc::samples = n is supported
  DeviceLimits limits;                      // vertex attributes, varyings, texture units, colour targets and vertex buffers the device takes
  bool blend_constant = false;              // with kFeatureBlendState: the BlendFactor Constant* factors and BlendState::constant
  uint32_t features2 = 0;                   // DeviceFeature2 bits
};

class IGraphicsDevice {
 public:
  virtual DeviceInfo info() const = 0;
  // What the device does with images of one of the formats numbered from 64 (graphics::FormatCap bits; the formats before those are described by DeviceInfo::features). 0: images of the
  // format are refused (create_image gives nullptr). A block-compressed format without kFormatNative is not created as such: the memory manager makes an image of its decoded_format.
  virtual uint32_t format_caps(Format format) const {
    (void)format;
    return 0;
  }
  // The largest width and height of a 2D image (and of the faces of a cube); at least 4096.
  virtual uint32_t max_image_dimension() const { return 4096; }

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
  // The timestamp clock was interrupted (the device was reset, changed its power state or its clock): the next TimestampDisjoint query says 1, once, and timestamps from before are not comparable
  // with those after. Devices without kFeatureTimestampQueries ignore it.
  virtual void interrupt_clock() {}
  virtual bool submit(ICommandBuffer* commands) = 0;
  // Shows `image` (created with kImagePresent) on the screen.
  virtual bool present(IImage* image) = 0;

 protected:
  constexpr IGraphicsDevice() = default;
  ~IGraphicsDevice() = default;
};

}  // namespace graphics
