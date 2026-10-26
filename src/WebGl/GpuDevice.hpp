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

// A software WebGPU device: buffers, textures, samplers, WGSL shader modules, bind groups, render and compute pipelines, and the execution of the commands a
// command encoder recorded (render passes drawn with the shared rasterizer, compute dispatches run by the shader interpreter, copies). The script bindings
// (DomBindingsGpu.cpp) decode JavaScript descriptors into the structs below and walk the recorded command lists, calling the methods of Device in order.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "WebGl/GlContext.hpp"
#include "WebGl/Accelerator.hpp"
#include "WebGl/Raster.hpp"
#include "WebGl/Shader.hpp"
#include "WebGl/TgsiTranslator.hpp"

namespace wgpu {

using webgl::AccelState;
using webgl::IAccelerator;
using webgl::TgsiConstant;
using webgl::TgsiProgram;

using webgl::Image;

// A WebGPU texture format: how it is stored (an image of the GL context's formats, in logical RGBA order) and how its texels sit in a buffer.
struct FormatDesc {
  const char* name;
  uint32_t gl;        // the webgl::FormatInfo it is stored as
  bool bgra;          // buffer order is B, G, R, A
  uint8_t comps;
  uint8_t kind;       // see the table
  uint8_t bytes;      // per texel in a buffer (the depth aspect for depth formats)
  bool depth, stencil;
  uint8_t bw = 1, bh = 1, block_bytes = 0;   // a block compressed format: texels of a block and bytes of one (`gl` is then the format it is decoded to)
  uint32_t cgl = 0;                         // ... and its WebGL enum, which names the decoder
};
const FormatDesc* find_format(StringView name);

struct Texture {
  explicit Texture(allocators::IAllocator& a) : mips(a), raw(a) {}
  List<UniquePointer<Image>> mips;
  List<UniquePointer<ByteBuffer>> raw;   // a compressed texture also keeps its blocks, per mip level, so that they can be copied out
  const FormatDesc* format = nullptr;
  uint32_t w = 1, h = 1, depth = 1, mip_count = 1, samples = 1, usage = 0;
  uint8_t dimension = 1;   // 0 = 1d, 1 = 2d, 2 = 3d
  bool destroyed = false;
};

struct TextureView {
  uint32_t texture = 0;
  const FormatDesc* format = nullptr;
  uint8_t dim = 1;   // 0 1d, 1 2d, 2 2d-array, 3 cube, 4 cube-array, 5 3d
  uint32_t base_mip = 0, mip_count = 1, base_layer = 0, layer_count = 1;
  uint8_t aspect = 0;   // 0 all, 1 depth-only, 2 stencil-only
};

struct Sampler {
  webgl::SamplerObj params;
  bool comparison = false;
  float anisotropy = 1;
};

struct Buffer {
  explicit Buffer(allocators::IAllocator& a) : data(a) {}
  ByteBuffer data;
  uint32_t usage = 0;
  bool destroyed = false;
};

struct ShaderModule {
  explicit ShaderModule(allocators::IAllocator& a) : source(a), log(a) {}
  String source, log;
  bool ok = false;
};

struct BglEntry {
  uint32_t binding = 0;
  uint32_t visibility = 7;
  uint8_t kind = 0;   // 0 buffer, 1 sampler, 2 texture, 3 storage texture, 4 external texture
  uint8_t buffer_type = 0;   // 0 uniform, 1 storage, 2 read-only-storage
  bool dynamic_offset = false;
  uint64_t min_size = 0;
  uint8_t sampler_type = 0;      // 0 filtering, 1 non-filtering, 2 comparison
  uint8_t sample_type = 0;       // 0 float, 1 unfilterable-float, 2 depth, 3 sint, 4 uint
  uint8_t view_dim = 1;
  bool multisampled = false;
  uint8_t storage_access = 0;    // 0 write-only, 1 read-only, 2 read-write
};

struct BindGroupLayout {
  explicit BindGroupLayout(allocators::IAllocator& a) : entries(a) {}
  List<BglEntry> entries;
};

struct PipelineLayout {
  explicit PipelineLayout(allocators::IAllocator& a) : bgls(a) {}
  List<uint32_t> bgls;
};

struct BgEntry {
  uint32_t binding = 0;
  uint8_t kind = 0;   // 0 buffer, 1 sampler, 2 texture view
  uint32_t id = 0;
  uint64_t offset = 0, size = 0;   // size 0: the rest of the buffer
};

struct BindGroup {
  explicit BindGroup(allocators::IAllocator& a) : entries(a) {}
  uint32_t layout = 0;
  List<BgEntry> entries;
};

struct VertexAttribute {
  uint32_t shader_location = 0;
  uint64_t offset = 0;
  uint8_t format = 0;   // see vertex_format()
};
struct VertexBufferLayout {
  explicit VertexBufferLayout(allocators::IAllocator& a) : attributes(a) {}
  uint64_t stride = 0;
  bool instance = false;
  List<VertexAttribute> attributes;
};

struct BlendComponent {
  uint8_t op = 0;       // 0 add, 1 subtract, 2 reverse-subtract, 3 min, 4 max
  uint8_t src = 1;      // 0 zero, 1 one, 2 src, 3 one-minus-src, 4 src-alpha, 5 one-minus-src-alpha, 6 dst, 7 one-minus-dst, 8 dst-alpha, 9 one-minus-dst-alpha, 10 src-alpha-saturated, 11 constant, 12 one-minus-constant
  uint8_t dst = 0;
};
struct ColorTarget {
  const FormatDesc* format = nullptr;
  bool blend = false;
  BlendComponent color, alpha;
  uint8_t write_mask = 15;
};
struct StencilFace {
  uint8_t compare = 7;   // 0 never, 1 less, 2 equal, 3 less-equal, 4 greater, 5 not-equal, 6 greater-equal, 7 always
  uint8_t fail = 0, depth_fail = 0, pass = 0;   // 0 keep, 1 zero, 2 replace, 3 invert, 4 increment-clamp, 5 decrement-clamp, 6 increment-wrap, 7 decrement-wrap
};
struct DepthStencilState {
  const FormatDesc* format = nullptr;
  bool depth_write = false;
  uint8_t depth_compare = 7;
  StencilFace front, back;
  uint32_t stencil_read_mask = 0xFFFFFFFFu, stencil_write_mask = 0xFFFFFFFFu;
  int32_t bias = 0;
  float bias_slope = 0, bias_clamp = 0;
};

struct RenderPipeline {
  explicit RenderPipeline(allocators::IAllocator& a) : vs(), fs(), buffers(a), targets(a), layouts(a) {}
  UniquePointer<shader::Module> vs, fs;
  int32_t vs_fn = -1, fs_fn = -1;
  List<UniquePointer<VertexBufferLayout>> buffers;
  List<ColorTarget> targets;
  List<uint32_t> layouts;   // bind group layouts of the pipeline layout
  bool has_depth = false;
  DepthStencilState ds;
  uint8_t topology = 3;   // 0 point-list, 1 line-list, 2 line-strip, 3 triangle-list, 4 triangle-strip
  uint8_t strip_index_format = 0;   // 0 none, 1 uint16, 2 uint32
  bool front_ccw = true;
  uint8_t cull = 0;       // 0 none, 1 front, 2 back
  bool unclipped_depth = false;
  uint32_t sample_count = 1;
  // Hardware acceleration (GpuAccel.cpp): the TGSI translation and the accelerator's program, made when the pipeline is first drawn with.
  UniquePointer<TgsiProgram> tgsi;
  IAccelerator* accel = nullptr;
  uint32_t accel_program = 0;
  bool accel_tried = false;
  ~RenderPipeline() {
    if (accel && accel_program) accel->destroy_program(accel_program);
  }
};

struct ComputePipeline {
  explicit ComputePipeline(allocators::IAllocator& a) : layouts(a) {}
  UniquePointer<shader::Module> module;
  int32_t fn = -1;
  List<uint32_t> layouts;
};

struct QuerySet {
  explicit QuerySet(allocators::IAllocator& a) : results(a) {}
  uint8_t type = 0;   // 0 occlusion, 1 timestamp
  List<uint64_t> results;
};

struct ColorAttachment {
  uint32_t view = 0, resolve = 0;
  bool load = false, store = true;
  float clear[4] = {0, 0, 0, 0};
};
struct RenderPassDesc {
  explicit RenderPassDesc(allocators::IAllocator& a) : colors(a) {}
  List<ColorAttachment> colors;
  uint32_t depth_view = 0;
  bool depth_load = false, depth_store = true, depth_read_only = false;
  float depth_clear = 1;
  bool stencil_load = false, stencil_store = true, stencil_read_only = false;
  uint32_t stencil_clear = 0;
  uint32_t occlusion_set = 0;
};

struct ImageCopy {
  uint32_t texture = 0;
  uint32_t mip = 0;
  uint32_t x = 0, y = 0, z = 0;
  uint8_t aspect = 0;   // 0 all, 1 depth, 2 stencil
};
struct BufferLayout {
  uint64_t offset = 0;
  uint32_t bytes_per_row = 0, rows_per_image = 0;
};

class Device final : public shader::ITextureHost {
 public:
  explicit Device(allocators::IAllocator& a);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // The first problem of the last call that failed.
  String error;

  uint32_t create_buffer(uint64_t size, uint32_t usage);
  void destroy_buffer(uint32_t id);
  bool write_buffer(uint32_t id, uint64_t offset, ConstByteView data);
  bool read_buffer(uint32_t id, uint64_t offset, ByteView out);
  uint64_t buffer_size(uint32_t id) const;

  uint32_t create_texture(uint32_t w, uint32_t h, uint32_t depth_or_layers, uint32_t mips, uint32_t samples, uint8_t dimension, const FormatDesc* format, uint32_t usage);
  void destroy_texture(uint32_t id);
  uint32_t create_view(const TextureView& v);
  const TextureView* view(uint32_t id) const { return id && id <= views_.count() ? views_[id - 1].get() : nullptr; }
  const Texture* texture(uint32_t id) const { return id && id <= textures_.count() ? textures_[id - 1].get() : nullptr; }
  Texture* texture_mut(uint32_t id) { return id && id <= textures_.count() ? textures_[id - 1].get() : nullptr; }
  uint32_t create_sampler(const Sampler& s);
  uint32_t create_shader_module(StringView code, String* messages);
  uint32_t create_bind_group_layout(const List<BglEntry>& entries);
  uint32_t create_pipeline_layout(ArrayView<const uint32_t> bgls);
  uint32_t create_bind_group(uint32_t layout, const List<BgEntry>& entries);
  struct ConstantValue { String name; double value; explicit ConstantValue(allocators::IAllocator& a) : name(a), value(0) {} };
  // `layout` 0 means "auto" (derived from the shaders). Entry names empty: the only entry point of that stage.
  uint32_t create_render_pipeline(uint32_t layout, uint32_t vs_module, StringView vs_entry, uint32_t fs_module, StringView fs_entry, List<UniquePointer<VertexBufferLayout>>& buffers,
                                  List<ColorTarget>& targets, const DepthStencilState* ds, uint8_t topology, uint8_t strip_format, bool ccw, uint8_t cull, bool unclipped, uint32_t samples,
                                  const List<UniquePointer<ConstantValue>>& vs_constants, const List<UniquePointer<ConstantValue>>& fs_constants);
  uint32_t create_compute_pipeline(uint32_t layout, uint32_t module, StringView entry, const List<UniquePointer<ConstantValue>>& constants);
  uint32_t pipeline_layout_bgl(uint32_t pipeline, bool compute, uint32_t index);
  uint32_t create_query_set(uint8_t type, uint32_t count);

  // --- copies (immediate; the bindings order them with the other commands)
  bool copy_buffer_to_buffer(uint32_t src, uint64_t so, uint32_t dst, uint64_t dof, uint64_t size);
  bool clear_buffer(uint32_t buf, uint64_t offset, uint64_t size);
  bool write_texture(const ImageCopy& dst, ConstByteView data, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d);
  bool copy_buffer_to_texture(uint32_t buf, const BufferLayout& layout, const ImageCopy& dst, uint32_t w, uint32_t h, uint32_t d);
  bool copy_texture_to_buffer(const ImageCopy& src, uint32_t buf, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d);
  bool copy_texture_to_texture(const ImageCopy& src, const ImageCopy& dst, uint32_t w, uint32_t h, uint32_t d);
  // Pixels from the script (RGBA8, straight alpha, rows top to bottom) into a texture.
  bool copy_external(uint32_t texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t layer, ConstByteView rgba, uint32_t w, uint32_t h, bool flip_y, bool premultiply);

  // --- passes
  bool begin_render_pass(const RenderPassDesc& d);
  bool end_render_pass();
  bool begin_compute_pass();
  bool end_compute_pass();
  bool set_pipeline(uint32_t id);
  bool set_bind_group(uint32_t index, uint32_t group, ArrayView<const uint32_t> dynamic_offsets);
  bool set_vertex_buffer(uint32_t slot, uint32_t buffer, uint64_t offset, uint64_t size);
  bool set_index_buffer(uint32_t buffer, uint8_t format, uint64_t offset, uint64_t size);
  void set_viewport(float x, float y, float w, float h, float min_depth, float max_depth);
  void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  void set_blend_constant(const float c[4]) { for (int i = 0; i < 4; ++i) blend_constant_[i] = c[i]; }
  void set_stencil_reference(uint32_t r) { stencil_ref_ = r; }
  bool draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance);
  bool draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t base_vertex, uint32_t first_instance);
  bool dispatch(uint32_t x, uint32_t y, uint32_t z);
  bool begin_occlusion(uint32_t index);
  bool end_occlusion();
  bool write_timestamp(uint32_t query_set, uint32_t index);
  const QuerySet* query_set(uint32_t id) const { return id && id <= query_sets_.count() ? query_sets_[id - 1].get() : nullptr; }

  // The pixels of a (canvas) texture as 0xAARRGGBB rows, straight alpha (premultiplied textures are un-premultiplied when `unpremultiply`).
  bool read_canvas(uint32_t texture, uint32_t* dst, uint32_t pitch, bool opaque, bool unpremultiply);
  // Clears a texture to transparent black.
  void clear_texture(uint32_t texture);

  void sample(const shader::SampleRequest& rq, uint32_t out[4]) override;

  allocators::IAllocator& allocator() { return a_; }

 private:
  friend struct RenderSink;
  friend struct BindingTable;
  struct Bound {
    const TextureView* view = nullptr;
    const Texture* tex = nullptr;
    Texture* tex_mut = nullptr;
    const Sampler* sampler = nullptr;
  };
  Buffer* buf(uint32_t id) { return id && id <= buffers_.count() ? buffers_[id - 1].get() : nullptr; }
  const Buffer* buf(uint32_t id) const { return id && id <= buffers_.count() ? buffers_[id - 1].get() : nullptr; }
  bool fail(const char* why) { if (error.empty()) error.assign(why); return false; }
  // Sets the length of runtime-sized arrays from the bound buffers, lays the module's shared region out and fills it from the bind groups of the pass.
  bool setup_stage(shader::Module& m, List<uint32_t>* region, List<uint32_t>* gl_dummy);
  bool write_back(shader::Module& m, List<uint32_t>& region);
  bool draw_primitives(uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, bool indexed, uint32_t unused);
  bool texture_copy(const ImageCopy& t, uint32_t w, uint32_t h, uint32_t d, Image** img, const FormatDesc** fd);
  // The block compressed paths of the texture copies: `data` rows hold whole blocks.
  bool block_copy_ok(const Image& img, const FormatDesc& fd, const ImageCopy& t, uint32_t w, uint32_t h);
  bool write_blocks(const ImageCopy& dst, ConstByteView data, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d);
  bool read_blocks(const ImageCopy& src, Buffer* dst, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d);
  uint32_t register_bound(const TextureView* v, const Sampler* s);

  allocators::IAllocator& a_;
  List<UniquePointer<Buffer>> buffers_;
  List<UniquePointer<Texture>> textures_;
  List<UniquePointer<TextureView>> views_;
  List<UniquePointer<Sampler>> samplers_;
  List<UniquePointer<ShaderModule>> modules_;
  List<UniquePointer<BindGroupLayout>> bgls_;
  List<UniquePointer<PipelineLayout>> pls_;
  List<UniquePointer<BindGroup>> groups_;
  List<UniquePointer<RenderPipeline>> rps_;
  List<UniquePointer<ComputePipeline>> cps_;
  List<UniquePointer<QuerySet>> query_sets_;
  // pass state
  int pass_ = 0;   // 0 none, 1 render, 2 compute
  RenderPassDesc* pass_desc_ = nullptr;
  UniquePointer<RenderPassDesc> pass_storage_;
  uint32_t cur_pipeline_ = 0;
  uint32_t bind_groups_[8] = {};
  List<uint32_t> dyn_all_;               // the dynamic offsets of all bind groups, group g's from dyn_start_[g]
  uint32_t dyn_start_[8] = {}, dyn_count_[8] = {};
  struct VBind { uint32_t buffer = 0; uint64_t offset = 0, size = 0; };
  VBind vbuf_[16];
  VBind ibuf_;
  uint8_t ibuf_format_ = 1;
  float viewport_[6] = {0, 0, 0, 0, 0, 1};
  bool have_viewport_ = false;
  uint32_t scissor_[4] = {0, 0, 0, 0};
  bool have_scissor_ = false;
  float blend_constant_[4] = {0, 0, 0, 0};
  uint32_t stencil_ref_ = 0;
  uint32_t occlusion_index_ = 0xFFFFFFFFu;
  uint64_t occlusion_count_ = 0;
  List<Bound> bound_;       // texture/sampler handles of the draw in progress
  struct Target;
  Target* target_ = nullptr;
  uint64_t clock_ = 1000;

  // --- 3D hardware (GpuAccel.cpp). A render pass whose color attachment is cleared (not loaded) is drawn on the accelerator, one device picture per pass, and read back
  // into the texture when the pass ends or at the first draw the hardware cannot take (the rest of the pass is then drawn on the CPU on top of it).
  IAccelerator* hw_ = nullptr;
  uint32_t hw_target_ = 0, hw_w_ = 0, hw_h_ = 0;
  bool hw_pass_ = false;     // the open pass is drawn on the hardware
  bool hw_ahead_ = false;    // ... and the device picture is newer than the texture
  bool hw_depth_ = false;    // ... and has a depth buffer
  uint32_t hw_view_ = 0;     // the color attachment's view
  void hw_release();
  void hw_begin(const RenderPassDesc& d);
  void hw_sync();
  void hw_end();
  bool hw_program(RenderPipeline& rp, IAccelerator& acc);
  bool hw_draw(RenderPipeline& rp, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, bool indexed);
};

// Vertex attribute decoding shared with GpuAccel.cpp (GpuRender.cpp): the lanes (float bit patterns, or raw integers when *ints) of the attribute at p.
void decode_vertex_attribute(uint8_t fmt, const uint8_t* p, uint32_t lanes[4], bool* ints);
uint32_t vertex_attribute_bytes(uint8_t fmt);

// The buffer-order size of one texel of a format for a copy aspect (0: unsupported).
uint32_t texel_copy_bytes(const FormatDesc& f, uint8_t aspect);
// The parts of a vertex format: its name index (see GpuDevice.cpp) -> 255 for an unknown name.
uint8_t vertex_format(StringView name);

}  // namespace wgpu
