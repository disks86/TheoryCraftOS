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

// A software OpenGL ES 2.0 / 3.0 context, the machinery behind WebGLRenderingContext and WebGL2RenderingContext. It keeps the GL object tables and state
// machine, compiles shaders with the shader interpreter, and draws with the rasterizer; the default framebuffer is a pixel buffer that the browser copies
// into the <canvas> (present()). Calls take GL enums and numbers exactly as GL does; the script bindings (DomBindingsGl.cpp) decode JavaScript arguments.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "WebGl/Raster.hpp"
#include "WebGl/Shader.hpp"

namespace webgl {

// --- Formats and images ------------------------------------------------------------------------------------------------------------------------------------

enum class Store : uint8_t { U8x4, F32x4, I32x4, U32x4, Depth, DepthStencil };

struct FormatInfo {
  uint32_t internal;
  Store store;
  uint32_t base;      // RED, RG, RGB, RGBA, LUMINANCE, ALPHA, LUMINANCE_ALPHA, DEPTH_COMPONENT, DEPTH_STENCIL
  bool srgb;
  bool color_renderable;
  bool filterable;
  bool is_float;      // float component type (for readback type)
  bool is_signed;     // snorm
};

const FormatInfo* find_format(uint32_t internal);
uint32_t texel_bytes(Store s);

struct Image {
  explicit Image(allocators::IAllocator& a) : px(a) {}
  uint32_t w = 0, h = 0, d = 1;
  const FormatInfo* info = nullptr;
  ByteBuffer px;
  bool defined() const { return info != nullptr && w > 0 && h > 0; }
  uint8_t* texel(uint32_t x, uint32_t y, uint32_t z) { return px.writable_view().data() + ((uint64_t{z} * h + y) * w + x) * texel_bytes(info->store); }
  const uint8_t* texel(uint32_t x, uint32_t y, uint32_t z) const { return px.view().data() + ((uint64_t{z} * h + y) * w + x) * texel_bytes(info->store); }
  bool allocate(const FormatInfo* fi, uint32_t width, uint32_t height, uint32_t depth);
};

struct Color4 {
  float f[4] = {0, 0, 0, 0};
};

void read_texel(const Image& img, uint32_t x, uint32_t y, uint32_t z, uint32_t out[4]);   // raw 32-bit lanes (floats for float stores)
void write_texel(Image& img, uint32_t x, uint32_t y, uint32_t z, const uint32_t v[4]);

constexpr uint32_t kMaxLevels = 14;
constexpr uint32_t kMaxTextureSize = 4096;
constexpr uint32_t kMaxAttribs = 16;
constexpr uint32_t kMaxTextureUnits = 16;
constexpr uint32_t kMaxDrawBuffers = 8;
constexpr uint32_t kMaxUniformBindings = 24;
constexpr uint64_t kMaxTotalBytes = 512ull << 20;

// Where a sampler finds the images of a texture: get(ctx, face, level), the kind of texture (a GL target: 0x0DE1, 0x8513 cube, 0x806F 3D, 0x8C1A 2D array) and the
// level range. A cube whose six faces are the layers of one image (WebGPU) sets faces_in_layers.
struct SampleSource {
  const Image* (*get)(const void* ctx, uint32_t face, int32_t level) = nullptr;
  const void* ctx = nullptr;
  uint32_t target = 0x0DE1;
  int32_t base_level = 0, max_level = 1000;
  bool faces_in_layers = false;
  uint32_t layer_offset = 0;   // the first layer of a view of a layered image
};

// --- Objects ---------------------------------------------------------------------------------------------------------------------------------------------

struct Buffer {
  explicit Buffer(allocators::IAllocator& a) : data(a) {}
  ByteBuffer data;
  uint32_t usage = 0x88E4;
  bool ever_bound = false;
};

struct Texture {
  explicit Texture(allocators::IAllocator& a) : images(a) {}
  uint32_t target = 0;   // 0: not bound yet
  List<UniquePointer<Image>> images;   // [face * kMaxLevels + level]
  uint32_t min_filter = 0x2702, mag_filter = 0x2601;
  uint32_t wrap_s = 0x2901, wrap_t = 0x2901, wrap_r = 0x2901;
  uint32_t compare_mode = 0, compare_func = 0x0203;
  int32_t base_level = 0, max_level = 1000;
  float min_lod = -1000, max_lod = 1000;
  bool immutable = false;
  uint32_t immutable_levels = 0;
};

struct SamplerObj {
  uint32_t min_filter = 0x2702, mag_filter = 0x2601, wrap_s = 0x2901, wrap_t = 0x2901, wrap_r = 0x2901, compare_mode = 0, compare_func = 0x0203;
  float min_lod = -1000, max_lod = 1000;
};

// Samples a texture. (GlSampler.cpp; the WebGPU device uses it too.)
void sample_images(const SampleSource& src, const SamplerObj& sampler, const shader::SampleRequest& rq, uint32_t out[4]);

struct Renderbuffer {
  explicit Renderbuffer(allocators::IAllocator& a) : image(a) {}
  Image image;
  uint32_t internal = 0x8058;
  uint32_t samples = 0;
};

struct Attachment {
  uint8_t kind = 0;   // 0 none, 1 texture, 2 renderbuffer
  uint32_t id = 0;
  uint32_t face_target = 0x0DE1;
  int32_t level = 0, layer = 0;
};

struct Framebuffer {
  Attachment color[kMaxDrawBuffers];
  Attachment depth, stencil;
  uint32_t draw_buffers[kMaxDrawBuffers] = {0x8CE0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t read_buffer = 0x8CE0;
};

struct ShaderObj {
  explicit ShaderObj(allocators::IAllocator& a) : source(a), log(a) {}
  uint32_t type = 0;
  String source, log;
  bool compiled = false;
  bool deleted = false;
  UniquePointer<shader::Module> module;
};

struct UniformLeaf {
  explicit UniformLeaf(allocators::IAllocator& a) : name(a) {}
  String name;               // as getActiveUniform reports it ("a", "b[0]", "s.x")
  uint32_t gl_type = 0;
  uint32_t size = 1;         // array length
  shader::Type type;         // the element type
  int32_t vs_slot = -1, fs_slot = -1;   // shared slots in each stage's storage
  uint32_t elem_slots = 1;
  uint32_t location = 0;     // of the first element
  bool in_block = false;
};

struct VaryingLink {
  uint32_t vs_slot, fs_slot, count;   // private slots
  bool flat;
  uint32_t var_offset;                // position in the interpolated varyings array
};

struct AttribInfo {
  explicit AttribInfo(allocators::IAllocator& a) : name(a) {}
  String name;
  uint32_t gl_type = 0;
  uint32_t size = 1;
  int32_t location = -1;
  uint32_t slot = 0;
  shader::Type type;
};

struct ProgramObj {
  explicit ProgramObj(allocators::IAllocator& a) : log(a), uniforms(a), attribs(a), links(a), vs_shared(a), fs_shared(a), bound_attribs(a), block_bindings(a), tf_varyings(a), outputs(a), locs(a) {}
  String log;
  uint32_t vs = 0, fs = 0;
  bool linked = false, deleted = false;
  UniquePointer<shader::Module> vsm, fsm;
  List<UniquePointer<UniformLeaf>> uniforms;
  List<UniquePointer<AttribInfo>> attribs;
  List<VaryingLink> links;
  List<uint32_t> vs_shared, fs_shared;
  struct Bound { String name; int32_t loc; explicit Bound(allocators::IAllocator& a) : name(a), loc(0) {} };
  List<UniquePointer<Bound>> bound_attribs;
  List<int32_t> block_bindings;        // per block of vs/fs (by index in the merged list)
  List<UniquePointer<String>> tf_varyings;
  uint32_t tf_mode = 0x8C8C;
  uint32_t nvar = 0;
  struct Output { int32_t location; uint32_t slot; shader::Type type; };
  List<Output> outputs;                // fragment outputs
  bool frag_writes_all = false;       // gl_FragColor broadcasts to every draw buffer (legacy)
  bool validate_ok = true;
  uint32_t loc_count = 0;
  struct Loc { uint32_t leaf, element; };
  List<Loc> locs;
  UniquePointer<shader::Machine> vmach, fmach;   // made when first drawn with
};

struct VertexAttrib {
  bool enabled = false;
  uint32_t buffer = 0;
  uint32_t size = 4, type = 0x1406;
  bool normalized = false, integer = false;
  uint32_t stride = 0;
  uint64_t offset = 0;
  uint32_t divisor = 0;
  float cur_f[4] = {0, 0, 0, 1};
  int32_t cur_i[4] = {0, 0, 0, 1};
  bool cur_is_int = false;
};

struct Vao {
  VertexAttrib attribs[kMaxAttribs];
  uint32_t element_buffer = 0;
};

struct QueryObj {
  uint32_t target = 0;
  uint64_t result = 0;
  bool active = false, ended = false;
};

struct TransformFeedbackObj {
  uint32_t buffers[4] = {0, 0, 0, 0};
  uint64_t offsets[4] = {0, 0, 0, 0};
  uint64_t sizes[4] = {0, 0, 0, 0};
  bool active = false, paused = false;
};

struct TextureUnit {
  uint32_t tex2d = 0, cube = 0, tex3d = 0, array = 0;
  uint32_t sampler = 0;
};

struct BufferBinding {
  uint32_t buffer = 0;
  uint64_t offset = 0, size = 0;
};

// --- The context ---------------------------------------------------------------------------------------------------------------------------------------

class GlContext final : public shader::ITextureHost {
 public:
  struct Attributes {
    bool alpha = true, depth = true, stencil = false, antialias = true, premultiplied = true, preserve = false;
  };

  GlContext(allocators::IAllocator& a, bool es3, uint32_t width, uint32_t height, const Attributes& attrs);
  ~GlContext();
  GlContext(const GlContext&) = delete;
  GlContext& operator=(const GlContext&) = delete;

  bool valid() const { return valid_; }
  bool es3() const { return es3_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  const Attributes& attributes() const { return attrs_; }
  void resize(uint32_t w, uint32_t h);
  bool lost() const { return lost_; }
  void lose() { lost_ = true; }
  void restore();
  uint64_t version() const { return version_; }

  // Copies the pixels drawn since the last call into `dst` (rows top-down, 0xAARRGGBB, straight alpha) and says which rectangle changed.
  bool present(uint32_t* dst, uint32_t pitch_pixels, gpu::TouchBox* changed, bool force_all);

  // --- errors
  uint32_t get_error();
  void set_error(uint32_t e) { if (!error_) error_ = e; }

  // --- objects
  uint32_t create_buffer();
  uint32_t create_texture();
  uint32_t create_framebuffer();
  uint32_t create_renderbuffer();
  uint32_t create_shader(uint32_t type);
  uint32_t create_program();
  uint32_t create_vao();
  uint32_t create_sampler();
  uint32_t create_query();
  uint32_t create_transform_feedback();
  void delete_object(uint32_t kind, uint32_t id);   // kind: 'b' buffer, 't' texture, 'f' framebuffer, 'r' renderbuffer, 's' shader, 'p' program, 'v' vao, 'a' sampler, 'q' query, 'x' tf
  bool is_object(uint32_t kind, uint32_t id) const;

  // --- buffers
  void bind_buffer(uint32_t target, uint32_t id);
  void bind_buffer_range(uint32_t target, uint32_t index, uint32_t id, uint64_t offset, uint64_t size);
  void buffer_data(uint32_t target, ConstByteView data, uint64_t size, uint32_t usage);
  void buffer_sub_data(uint32_t target, uint64_t offset, ConstByteView data);
  bool get_buffer_sub_data(uint32_t target, uint64_t offset, ByteView out);
  void copy_buffer_sub_data(uint32_t read_target, uint32_t write_target, uint64_t ro, uint64_t wo, uint64_t size);
  uint32_t bound_buffer(uint32_t target) const;
  uint64_t buffer_size(uint32_t id) const;
  uint32_t buffer_usage(uint32_t id) const;

  // --- textures
  void active_texture(uint32_t unit);
  void bind_texture(uint32_t target, uint32_t id);
  void bind_sampler(uint32_t unit, uint32_t id);
  struct Unpack {
    uint32_t alignment = 4, row_length = 0, image_height = 0, skip_pixels = 0, skip_rows = 0, skip_images = 0;
    bool flip_y = false, premultiply = false;
  };
  Unpack unpack;
  struct Pack {
    uint32_t alignment = 4, row_length = 0, skip_pixels = 0, skip_rows = 0;
  };
  Pack pack;
  // `data` empty with `has_data` false allocates zeros.
  void tex_image(uint32_t target, int32_t level, uint32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, uint32_t format, uint32_t type, ConstByteView data,
                 bool has_data, bool three_d);
  void tex_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, ConstByteView data,
                     bool three_d);
  void tex_storage(uint32_t target, uint32_t levels, uint32_t internal, uint32_t w, uint32_t h, uint32_t d);
  void copy_tex_image_2d(uint32_t target, int32_t level, uint32_t internal, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t border);
  void copy_tex_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, int32_t x, int32_t y, uint32_t w, uint32_t h);
  void tex_parameter(uint32_t target, uint32_t pname, double value);
  bool get_tex_parameter(uint32_t target, uint32_t pname, double* out);
  void generate_mipmap(uint32_t target);
  void sampler_parameter(uint32_t sampler, uint32_t pname, double value);
  bool texture_level_info(uint32_t target, int32_t level, uint32_t* w, uint32_t* h, uint32_t* d, uint32_t* internal);

  // --- renderbuffers and framebuffers
  void bind_renderbuffer(uint32_t id);
  void renderbuffer_storage(uint32_t internal, uint32_t w, uint32_t h, uint32_t samples);
  bool get_renderbuffer_parameter(uint32_t pname, double* out);
  void bind_framebuffer(uint32_t target, uint32_t id);
  void framebuffer_texture(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t tex, int32_t level, int32_t layer);
  void framebuffer_renderbuffer(uint32_t target, uint32_t attachment, uint32_t rb);
  uint32_t check_framebuffer_status(uint32_t target);
  bool get_framebuffer_attachment_parameter(uint32_t target, uint32_t attachment, uint32_t pname, double* out);
  void draw_buffers(ArrayView<const uint32_t> bufs);
  void read_buffer(uint32_t src);
  void invalidate_framebuffer(uint32_t target, ArrayView<const uint32_t> attachments);
  uint32_t bound_framebuffer(uint32_t target) const { return target == 0x8CA8 ? read_fb_ : draw_fb_; }
  uint32_t bound_renderbuffer() const { return rb_bound_; }

  // --- state
  void enable(uint32_t cap, bool on);
  bool is_enabled(uint32_t cap) const;
  void viewport(int32_t x, int32_t y, int32_t w, int32_t h);
  void scissor(int32_t x, int32_t y, int32_t w, int32_t h);
  void depth_range(float n, float f);
  void blend_color(float r, float g, float b, float a);
  void blend_equation_separate(uint32_t rgb, uint32_t a);
  void blend_func_separate(uint32_t sr, uint32_t dr, uint32_t sa, uint32_t da);
  void color_mask(bool r, bool g, bool b, bool a);
  void depth_mask(bool m);
  void depth_func(uint32_t f);
  void stencil_func_separate(uint32_t face, uint32_t func, int32_t ref, uint32_t mask);
  void stencil_op_separate(uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
  void stencil_mask_separate(uint32_t face, uint32_t mask);
  void cull_face(uint32_t m) { cull_mode_ = m; }
  void front_face(uint32_t m) { front_face_ = m; }
  void polygon_offset(float factor, float units) { po_factor_ = factor; po_units_ = units; }
  void line_width(float w) { line_width_ = w; }
  void clear_color(float r, float g, float b, float a) { clear_color_[0] = r; clear_color_[1] = g; clear_color_[2] = b; clear_color_[3] = a; }
  void clear_depth(float d) { clear_depth_ = d; }
  void clear_stencil(int32_t s) { clear_stencil_ = s; }
  void sample_coverage(float v, bool invert) { sc_value_ = v; sc_invert_ = invert; }
  void hint(uint32_t, uint32_t) {}
  void pixel_store(uint32_t pname, double value);
  void use_program(uint32_t id);
  uint32_t current_program() const { return program_; }
  void bind_vao(uint32_t id);
  uint32_t current_vao() const { return vao_id_; }
  const float* clear_color_values() const { return clear_color_; }

  // --- shaders and programs
  void shader_source(uint32_t id, StringView src);
  void compile_shader(uint32_t id);
  bool shader_status(uint32_t id, uint32_t pname, double* out);
  StringView shader_log(uint32_t id);
  StringView shader_src(uint32_t id);
  void attach_shader(uint32_t program, uint32_t shader);
  void detach_shader(uint32_t program, uint32_t shader);
  void link_program(uint32_t id);
  void validate_program(uint32_t id);
  bool program_status(uint32_t id, uint32_t pname, double* out);
  StringView program_log(uint32_t id);
  void bind_attrib_location(uint32_t program, uint32_t index, StringView name);
  int32_t get_attrib_location(uint32_t program, StringView name);
  uint32_t attached_shaders(uint32_t program, uint32_t* out, uint32_t max);
  const ProgramObj* program(uint32_t id) const;
  const UniformLeaf* active_uniform(uint32_t program, uint32_t index);
  const AttribInfo* active_attrib(uint32_t program, uint32_t index);
  int32_t get_uniform_location(uint32_t program, StringView name);
  // base_kind: 'f', 'i' or 'u'; lanes are the raw 32-bit values of `count` elements of cols x rows components (column-major)
  void uniform(int32_t location, uint32_t base_kind, uint32_t cols, uint32_t rows, uint32_t count, const uint32_t* lanes);
  bool get_uniform(uint32_t program, int32_t location, double* out, uint32_t* count);
  uint32_t get_uniform_block_index(uint32_t program, StringView name);
  void uniform_block_binding(uint32_t program, uint32_t block, uint32_t binding);
  int32_t get_frag_data_location(uint32_t program, StringView name);
  uint32_t uniform_block_count(uint32_t program) const;
  bool uniform_block_info(uint32_t program, uint32_t block, uint32_t pname, double* out);
  StringView uniform_block_name(uint32_t program, uint32_t block);
  void transform_feedback_varyings(uint32_t program, ArrayView<const StringView> names, uint32_t mode);
  void begin_transform_feedback(uint32_t mode);
  void end_transform_feedback();

  // --- vertex arrays
  void enable_vertex_attrib(uint32_t index, bool on);
  void vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type, bool normalized, uint32_t stride, uint64_t offset, bool integer);
  void vertex_attrib_divisor(uint32_t index, uint32_t divisor);
  void vertex_attrib_value(uint32_t index, const float* v, const int32_t* iv);
  const VertexAttrib* attrib(uint32_t index) const;

  // --- drawing
  void clear(uint32_t mask);
  void clear_buffer(uint32_t buffer, int32_t drawbuffer, const uint32_t value[4], uint32_t kind);   // kind 0 fv, 1 iv, 2 uiv
  void clear_buffer_fi(int32_t drawbuffer, float depth, int32_t stencil);
  void draw_arrays(uint32_t mode, int32_t first, int32_t count, int32_t instances);
  void draw_elements(uint32_t mode, int32_t count, uint32_t type, uint64_t offset, int32_t instances);
  void draw_range_elements(uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, uint64_t offset);
  void read_pixels(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t format, uint32_t type, ByteView out);
  void blit_framebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, uint32_t mask, uint32_t filter);
  void finish() {}

  // --- queries and sync
  void begin_query(uint32_t target, uint32_t id);
  void end_query(uint32_t target);
  bool query_result(uint32_t id, uint32_t pname, double* out);
  uint32_t current_query(uint32_t target) const;

  // --- misc queries
  bool get_integer(uint32_t pname, double* out, uint32_t* count);   // numeric parameters (arrays up to 4)
  const FormatInfo* internalformat(uint32_t i) const { return find_format(i); }
  bool get_vertex_attrib(uint32_t index, uint32_t pname, double* out, uint32_t* count);
  bool sample_range(uint32_t kind, double* out);
  uint32_t max_samples() const { return 4; }

  // ITextureHost
  void sample(const shader::SampleRequest& rq, uint32_t out[4]) override;

  // Allocation helpers for the bindings
  allocators::IAllocator& allocator() { return a_; }

 private:
  friend struct DrawJob;
  friend struct FragmentPipeline;

  Buffer* buffer_at(uint32_t id) { return id && id <= buffers_.count() ? buffers_[id - 1].get() : nullptr; }
  const Buffer* buffer_at(uint32_t id) const { return id && id <= buffers_.count() ? buffers_[id - 1].get() : nullptr; }
  Texture* texture_at(uint32_t id) { return id && id <= textures_.count() ? textures_[id - 1].get() : nullptr; }
  const Texture* texture_at(uint32_t id) const { return id && id <= textures_.count() ? textures_[id - 1].get() : nullptr; }
  Framebuffer* fb_at(uint32_t id) { return id && id <= fbos_.count() ? fbos_[id - 1].get() : nullptr; }
  Renderbuffer* rb_at(uint32_t id) { return id && id <= rbos_.count() ? rbos_[id - 1].get() : nullptr; }
  ShaderObj* shader_at(uint32_t id) { return id && id <= shaders_.count() ? shaders_[id - 1].get() : nullptr; }
  ProgramObj* program_at(uint32_t id) { return id && id <= programs_.count() ? programs_[id - 1].get() : nullptr; }
  Vao* vao_at(uint32_t id) { return id == 0 ? &default_vao_ : (id <= vaos_.count() ? vaos_[id - 1].get() : nullptr); }
  Vao* current_vao_ptr() { return vao_at(vao_id_); }
  SamplerObj* sampler_at(uint32_t id) { return id && id <= samplers_.count() ? samplers_[id - 1].get() : nullptr; }
  QueryObj* query_at(uint32_t id) { return id && id <= queries_.count() ? queries_[id - 1].get() : nullptr; }

  Image* texture_image(Texture* t, uint32_t target, int32_t level);   // the image for a (face) target and level, or null
  Image* make_texture_image(Texture* t, uint32_t target, int32_t level);
  Texture* bound_texture_for(uint32_t target, uint32_t* face);
  uint32_t* bound_texture_slot(uint32_t target);
  static uint32_t face_index(uint32_t target);
  bool texture_complete(const Texture& t, const SamplerObj& s, bool* is_cube) const;
  void sample_texture(const Texture& t, const SamplerObj& s, const shader::SampleRequest& rq, uint32_t out[4]) const;

  // render targets
  struct Target {
    Image* color[kMaxDrawBuffers] = {};
    uint32_t color_count = 0;
    Image* depth = nullptr;        // Depth or DepthStencil image
    Image* stencil = nullptr;      // DepthStencil image (same as depth when packed)
    uint32_t w = 0, h = 0;
    uint32_t layer[kMaxDrawBuffers] = {};
    uint32_t depth_layer = 0;
    bool is_default = false;
    bool ok = false;
  };
  bool resolve_image(const Attachment& at, Image** img, uint32_t* layer);
  void resolve_target(uint32_t fb, bool draw, Target* t);
  bool framebuffer_complete(uint32_t fb, uint32_t* status);

  void link_failed(ProgramObj* p, StringView why);
  bool link_collect_uniforms(ProgramObj* p);
  void flatten_uniform(ProgramObj* p, const shader::Module& m, bool is_vs, const shader::Global& g, const shader::Type& t, StringView name, uint32_t slot);
  void setup_program_storage(ProgramObj* p);
  void load_uniform_blocks(ProgramObj* p);
  uint32_t uniform_gl_type(const shader::Type& t) const;

  struct VertexOut;
  bool run_draw(uint32_t mode, int32_t first, int32_t count, uint32_t index_type, uint64_t index_offset, int32_t instances, bool indexed);
  uint32_t fetch_index(const uint8_t* idx, uint32_t type, uint32_t i) const;
  void mark_dirty(int32_t x, int32_t y);
  Image* default_color() { return &def_color_; }

  allocators::IAllocator& a_;
  bool es3_;
  bool valid_ = false;
  bool lost_ = false;
  Attributes attrs_;
  uint32_t width_, height_;
  uint32_t error_ = 0;
  uint64_t version_ = 1;
  uint64_t total_bytes_ = 0;

  List<UniquePointer<Buffer>> buffers_;
  List<UniquePointer<Texture>> textures_;
  List<UniquePointer<Framebuffer>> fbos_;
  List<UniquePointer<Renderbuffer>> rbos_;
  List<UniquePointer<ShaderObj>> shaders_;
  List<UniquePointer<ProgramObj>> programs_;
  List<UniquePointer<Vao>> vaos_;
  List<UniquePointer<SamplerObj>> samplers_;
  List<UniquePointer<QueryObj>> queries_;
  List<UniquePointer<TransformFeedbackObj>> tfs_;
  Vao default_vao_;
  TransformFeedbackObj default_tf_;

  Image def_color_, def_depth_;
  gpu::TouchBox dirty_;
  bool all_dirty_ = true;

  // bindings
  uint32_t array_buffer_ = 0, copy_read_ = 0, copy_write_ = 0, pixel_pack_ = 0, pixel_unpack_ = 0, tf_buffer_ = 0, uniform_buffer_ = 0;
  BufferBinding ubo_bindings_[kMaxUniformBindings];
  BufferBinding tf_bindings_[4];
  TextureUnit units_[kMaxTextureUnits];
  uint32_t active_unit_ = 0;
  uint32_t rb_bound_ = 0, read_fb_ = 0, draw_fb_ = 0;
  uint32_t program_ = 0, vao_id_ = 0, tf_bound_ = 0;
  uint32_t query_active_[4] = {0, 0, 0, 0};

  // state
  bool blend_ = false, cull_ = false, depth_test_ = false, stencil_test_ = false, scissor_test_ = false, offset_fill_ = false, dither_ = true, rasterizer_discard_ = false;
  bool alpha_to_coverage_ = false, sample_coverage_en_ = false;
  int32_t vp_[4] = {0, 0, 0, 0}, sc_[4] = {0, 0, 0, 0};
  float depth_n_ = 0, depth_f_ = 1;
  float blend_color_[4] = {0, 0, 0, 0};
  uint32_t blend_eq_rgb_ = 0x8006, blend_eq_a_ = 0x8006;
  uint32_t blend_sr_ = 1, blend_dr_ = 0, blend_sa_ = 1, blend_da_ = 0;
  bool cmask_[4] = {true, true, true, true};
  bool dmask_ = true;
  uint32_t dfunc_ = 0x0201;
  uint32_t sfunc_[2] = {0x0207, 0x0207};
  int32_t sref_[2] = {0, 0};
  uint32_t smask_[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  uint32_t sfail_[2] = {0x1E00, 0x1E00}, sdpfail_[2] = {0x1E00, 0x1E00}, sdppass_[2] = {0x1E00, 0x1E00};
  uint32_t swmask_[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  uint32_t cull_mode_ = 0x0405, front_face_ = 0x0901;
  float po_factor_ = 0, po_units_ = 0, line_width_ = 1;
  float clear_color_[4] = {0, 0, 0, 0};
  float clear_depth_ = 1;
  int32_t clear_stencil_ = 0;
  float sc_value_ = 1;
  bool sc_invert_ = false;

  SamplerObj default_sampler_;
  bool default_draw_back_ = true, default_read_back_ = true;
  uint64_t tf_pos_[4] = {0, 0, 0, 0};   // bytes captured so far into each transform feedback binding
  bool tf_capturing() const { return tf_active_; }
  bool tf_active_ = false;
  void tf_capture(const ProgramObj& p, const uint32_t* priv);
};

}  // namespace webgl
