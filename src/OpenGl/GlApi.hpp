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

// One OpenGL context in the kernel. The methods named after GL commands are the commands themselves (their declarations are generated from the registry: GlApiDecl.inc), reached
// from GlService by decoding a command stream; every user-mode personality gets the same implementation. Rendering is done by webgl::GlContext, which this class extends with the
// desktop features; the fixed-function pipeline is turned into shaders (FixedFunction.cpp).
//
// Files: GlApiCore.cpp (construction, errors, stream decoding, display lists), GlApiGet.cpp (glGet*), GlApiFixed.cpp (matrices, lights, materials, texture environments, fog ...),
// GlApiImmediate.cpp (glBegin/glEnd, current values, evaluators), GlApiDraw.cpp (arrays and draw calls), GlApiBuffer.cpp, GlApiTexture.cpp, GlApiPixels.cpp (pixel transfer, glDrawPixels,
// glBitmap, glReadPixels, accumulation), GlApiProgram.cpp, GlApiFramebuffer.cpp, GlApiQuery.cpp (queries, sync, debug output), GlApiFeedback.cpp (selection and feedback),
// FixedFunction.cpp (shaders for the fixed-function state).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/List.hpp"
#include "OpenGl/GlState.hpp"
#include "OpenGl/GlStream.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "WebGl/GlContext.hpp"

namespace opengl {

// Names the program chooses -> objects of the engine. A name from glGen* with no object yet maps to 0.
class NameTable {
 public:
  explicit NameTable(allocators::IAllocator& a) : map_(a) {}
  uint32_t generate() {
    while (map_.contains(next_)) ++next_;
    const uint32_t n = next_++;
    map_.put(n, 0);
    return n;
  }
  bool known(uint32_t name) const { return map_.contains(name); }
  uint32_t object(uint32_t name) const {
    const uint32_t* o = map_.find(name);
    return o ? *o : 0;
  }
  void set(uint32_t name, uint32_t object) { map_.put(name, object); }
  void erase(uint32_t name) { map_.remove(name); }
  // The name of an object (0: none). Rare: glGet of a binding.
  uint32_t name_of(uint32_t object) const {
    if (object == 0) return 0;
    for (const auto& e : map_) if (e.value == object) return e.key;
    return 0;
  }

 private:
  Dictionary<uint32_t, uint32_t> map_;
  uint32_t next_ = 1;
};

struct DisplayList {
  explicit DisplayList(allocators::IAllocator& a) : commands(a), client(a) {}
  ByteBuffer commands;      // the stream of its commands
  ByteBuffer client;        // the client data they read, reached by addresses from kListAddressBase on
};

// What contexts sharing objects share.
struct ShareGroup {
  explicit ShareGroup(allocators::IAllocator& a) : objects(a), buffers(a), textures(a), renderbuffers(a), shaders_and_programs(a), samplers(a), lists(a) {}
  webgl::GlObjects objects;
  NameTable buffers, textures, renderbuffers, shaders_and_programs, samplers;
  Dictionary<uint32_t, SharedPointer<DisplayList>> lists;      // a name from glGenLists with no contents yet holds a null pointer
};

struct ContextConfig {
  uint32_t major = 4, minor = 6;
  bool core_profile = false;
  bool forward_compatible = false;
  bool debug = false;
  bool robust = false;
  uint32_t stencil_bits = 8, depth_bits = 24, alpha_bits = 8;
};

class GlSurface;

// A program made from the fixed-function state (FixedFunction.cpp).
struct FixedProgram;
struct FixedKey;
class FeedbackSink;

class GlApi {
 public:
  GlApi(allocators::IAllocator& allocator, ShareGroup& group, const ContextConfig& config);
  ~GlApi();
  GlApi(const GlApi&) = delete;
  GlApi& operator=(const GlApi&) = delete;

  // Runs a stream of commands. Returns the result of the last command that has one. Bad streams stop at the first thing that makes no sense.
  uint64_t execute(ConstByteView stream, IClientMemory& memory);

  // The window surface drawn to (null: none, draws go nowhere and reads give nothing). The first surface a context gets sets its viewport and scissor, as GL says.
  void bind_surface(GlSurface* surface);
  GlSurface* surface() const { return surface_; }
  // The surface changed its size or swapped: the engine's images are where the surface now has them.
  void surface_changed();

#include "OpenGl/generated/GlApiDecl.inc"

  static constexpr const char* kVersionString = "3.3.0 Compatibility Profile TheoryCraftOS";
  static constexpr const char* kGlslVersionString = "3.30";
  allocators::IAllocator& allocator() { return a_; }

 private:
  friend class FeedbackSink;
  friend uint64_t decode_one(GlApi& api, uint32_t op, StreamReader& rd);

  // --- GlApiCore.cpp
  allocators::IAllocator& a_;
  ShareGroup& group_;
  ContextConfig config_;
  webgl::GlContext gl_;
  IClientMemory* memory_ = nullptr;
  GlSurface* surface_ = nullptr;
  bool surface_seen_ = false;
  uint64_t surface_generation_ = 0;
  uint32_t default_draw_buffer_ = 0x0405, default_read_buffer_ = 0x0405;      // GL_BACK
  float depth_range_values_[2];
  struct ClientString { const char* text; uint64_t address; };
  List<ClientString> client_strings_;
  bool compat() const { return !config_.core_profile; }
  void error(uint32_t e) { gl_.set_error(e); }
  // Reads a value array of the program (or of the display list being replayed).
  bool read_client(UserPtr at, ByteView out);
  bool write_client(UserPtr at, ConstByteView data);
  template <class T>
  bool read_values(UserPtr at, T* out, uint64_t n) {
    // (Trivial value types through their bytes; `out` has room for n by every caller's construction.)
    return read_client(at, ByteView(reinterpret_cast<uint8_t*>(out), n * sizeof(T)));
  }
  template <class T>
  bool write_values(UserPtr at, const T* in, uint64_t n) {
    return write_client(at, ConstByteView(reinterpret_cast<const uint8_t*>(in), n * sizeof(T)));
  }
  bool read_string(UserPtr at, int64_t length, String* out);      // length < 0: up to its NUL
  bool write_string(UserPtr at, int32_t capacity, UserPtr length_out, StringView text);
  uint64_t client_string(const char* text);      // a C string the program can keep: copied once into its memory

  // Display lists
  uint32_t list_compiling_ = 0;
  bool list_execute_too_ = false;
  SharedPointer<DisplayList> list_building_;
  ConstByteView list_client_;      // of the list being replayed
  uint32_t list_depth_ = 0;
  uint32_t list_base_ = 0;
  bool list_record(uint32_t op, ConstByteView command);
  void list_run(uint32_t name);
  uint64_t run_stream(ConstByteView stream);
  uint64_t client_bytes_of(uint32_t op, ConstByteView command, uint32_t* pointer_word);
  bool list_record_draw(uint32_t op, ConstByteView command);
  static uint32_t light_value_count(uint32_t pname);
  static uint32_t model_value_count(uint32_t pname);
  static uint32_t material_value_count(uint32_t pname);

  // --- fixed function
  FixedState fx_;
  MatrixStack modelview_, projection_, color_stack_, texture_stack_[kMaxTexUnits];
  uint32_t client_active_unit_ = 0;
  uint32_t active_unit() const { return gl_.active_unit_; }
  MatrixStack& current_stack();
  void matrix_changed() { ++fx_generation_; }
  void multiply_current(const Mat4& by);
  uint64_t fx_generation_ = 1;
  void light_value(uint32_t light, uint32_t pname, const float* v, uint32_t n);
  void material_value(uint32_t face, uint32_t pname, const float* v, uint32_t n);
  void fog_value(uint32_t pname, const float* v, uint32_t n);
  void texenv_value(uint32_t target, uint32_t pname, const float* v, uint32_t n);
  void texgen_value(uint32_t coord, uint32_t pname, const double* v, uint32_t n);
  void point_value(uint32_t pname, const float* v, uint32_t n);
  void model_value(uint32_t pname, const float* v, uint32_t n);
  void apply_color_material(const float color[4]);
  bool fixed_enable(uint32_t cap, bool on);      // true if it was one of the fixed-function capabilities
  bool fixed_is_enabled(uint32_t cap, bool* out) const;
  void enable_cap(uint32_t cap, bool on);
  static uint32_t other_enable_bit(uint32_t cap);
  uint32_t other_enables_ = 0;
  bool is_light_cap(uint32_t cap) const { return cap >= 0x4000 && cap < 0x4000 + kMaxLights; }

  struct AttribFrame;
  List<UniquePointer<AttribFrame>> attrib_stack_;
  struct ClientFrame;
  List<UniquePointer<ClientFrame>> client_stack_;

  // --- FixedFunction.cpp
  List<UniquePointer<FixedProgram>> fixed_programs_;
  FixedProgram* fixed_current_ = nullptr;
  void make_fixed_key(FixedKey* key) const;
  FixedProgram* fixed_program(const FixedKey& key);
  void load_fixed_uniforms(FixedProgram& p);
  uint32_t compile_program(StringView vs, StringView fs);
  void load_compat_uniforms(uint32_t engine_program);

  // --- immediate mode
  static constexpr uint32_t kImmFloats = 4 + 3 + 4 + 4 + 1 + 4 * kMaxTexUnits + 1;      // position, normal, colour, secondary, fog, texture coordinates, edge flag
  bool in_begin_end_ = false;
  uint32_t imm_mode_ = 0;
  List<float> imm_;
  uint32_t imm_buffer_ = 0;
  void imm_vertex(float x, float y, float z, float w);
  void imm_end();
  void set_color(float r, float g, float b, float a);
  void set_secondary(float r, float g, float b);
  void set_normal(float x, float y, float z);
  void set_texcoord(uint32_t unit, float s, float t, float r, float q);
  void set_fog_coord(float f);
  void set_index(float i);
  void set_edge(bool on);
  void set_attrib(uint32_t index, float x, float y, float z, float w);
  void set_attrib_int(uint32_t index, int32_t x, int32_t y, int32_t z, int32_t w, bool is_unsigned);
  void set_attrib_double(uint32_t index, double x, double y, double z, double w);
  void raster_pos(float x, float y, float z, float w);
  void window_pos(float x, float y, float z);
  void rect(float x1, float y1, float x2, float y2);
  void eval_coord1(float u);
  void eval_coord2(float u, float v);
  float generic_current_[webgl::kMaxAttribs][4] = {};
  uint32_t generic_set_mask_ = 0;
  bool outside_begin_end() { if (in_begin_end_) { error(0x502); return false; } return true; }

  // --- evaluators
  struct EvalStore;
  UniquePointer<EvalStore> eval_;
  void eval_define(uint32_t target, bool two, double u1, double u2, double v1, double v2, int32_t ustride, int32_t uorder, int32_t vstride, int32_t vorder, UserPtr points, bool doubles);
  void eval_apply(const float* in, uint32_t kind, uint32_t comps);

  // --- arrays and draws (GlApiDraw.cpp)
  uint32_t index_buffer_ = 0;      // holds the client's indices, and those made for quads and polygons
  uint32_t client_buffers_[webgl::kMaxAttribSlots] = {};
  List<uint8_t> tri_edges_;
  bool prepare_draw(uint32_t mode, int64_t lowest, int64_t highest, uint32_t instances);
  void finish_draw();
  bool upload_client_arrays(int64_t lowest, int64_t highest, uint32_t instances);
  void draw_any(uint32_t mode, int32_t first, int32_t count, uint32_t type, UserPtr indices, bool indexed, int32_t instances, int32_t base_vertex, uint32_t base_instance);
  void client_pointer(uint32_t attr, int32_t size, uint32_t type, bool normalized, int32_t stride, UserPtr pointer, bool integer);
  uint32_t engine_buffer(uint32_t name);      // creates the object of a name the first time
  void client_state(uint32_t array, bool on);
  bool list_expand_draw(uint32_t op, ConstByteView command);
  uint32_t user_program_ = 0;      // the engine's id of the program glUseProgram chose
  bool coherent_maps_ = false;
  void sync_coherent_maps();

  // --- objects
  uint32_t engine_texture(uint32_t name, uint32_t target);
  uint32_t ensure_vao_default();
  NameTable vao_names_, fbo_names_, query_names_, tf_names_;

  // --- pixels
  struct PixelStore {
    int32_t swap_bytes = 0, lsb_first = 0, row_length = 0, image_height = 0, skip_pixels = 0, skip_rows = 0, skip_images = 0, alignment = 4;
    int32_t compressed_block_width = 0, compressed_block_height = 0, compressed_block_depth = 0, compressed_block_size = 0;
  } unpack_, pack_;
  bool pixels_in(uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, UserPtr at, ByteBuffer* rgba_float, const PixelStore& store);
  void pixels_transfer(ArrayView<float> rgba);
  bool pixels_in_bitmap(uint32_t w, uint32_t h, UserPtr at, ByteView out);      // a bitmap by the unpack state, to rows of (w + 7) / 8 bytes, tightly packed
  void fragment_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, ConstByteView rgba, ConstByteView depth, float zoom_x, float zoom_y, float z);
  static void store_layout(const PixelStore& s, uint32_t w, uint32_t h, uint32_t bytes_per_pixel, uint64_t* row_bytes, uint64_t* slice_bytes, uint64_t* first_byte);
  bool pixels_out(uint32_t w, uint32_t h, uint32_t format, uint32_t type, ConstByteView rgba, UserPtr at);
  const webgl::Image* read_source(uint32_t* layer);
  bool read_rectangle(int32_t x, int32_t y, uint32_t w, uint32_t h, bool depth_stencil, ByteBuffer* out);
  void pixel_map_set(uint32_t map, int32_t size, const float* values);
  struct PixelMap {
    static constexpr uint32_t kMaxSize = 256;
    uint32_t size = 1;
    float values[kMaxSize] = {};
  };
  enum { kMapItoI, kMapStoS, kMapItoR, kMapItoG, kMapItoB, kMapItoA, kMapRtoR, kMapGtoG, kMapBtoB, kMapAtoA, kMapCount };
  PixelMap pixel_maps_[kMapCount];
  uint32_t pixel_program_ = 0, pixel_texture_ = 0, pixel_depth_texture_ = 0, pixel_buffer_ = 0;

  // --- GlApiFeedback.cpp
  uint32_t render_mode_ = 0x1C00;      // GL_RENDER
  UniquePointer<FeedbackSink> feedback_;
  gpu::IPrimitiveSink* feedback_sink();

  uint32_t debug_flags_ = 0;
  void not_implemented(const char* what);

  // Textures
  uint32_t default_textures_[11] = {};
  void create_default_textures();
  static uint32_t default_texture_index(uint32_t target);
  webgl::Texture* texture_for_target(uint32_t target, uint32_t* face);
  void store_floats(webgl::Image& image, uint32_t xo, uint32_t yo, uint32_t zo, uint32_t w, uint32_t h, uint32_t d, ConstByteView rgba, bool srgb_source);
  void texture_image(uint32_t target, int32_t level, int32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, uint32_t format, uint32_t type, UserPtr pixels, uint32_t dims);
  void texture_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, UserPtr pixels);
  void texture_storage(uint32_t target, uint32_t levels, uint32_t internal, uint32_t w, uint32_t h, uint32_t d);
  void copy_image(uint32_t target, int32_t level, uint32_t internal, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t border);
  void copy_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, int32_t x, int32_t y, uint32_t w, uint32_t h);
  void tex_parameter_values(uint32_t target, uint32_t pname, const double* v, uint32_t n);
  static uint32_t tex_param_count(uint32_t pname);
  bool tex_parameter_get(uint32_t target, uint32_t pname, double* out, uint32_t* count);
  bool tex_level_get(uint32_t target, int32_t level, uint32_t pname, double* out);
  static bool proxy_target(uint32_t target, uint32_t* real);
  static uint32_t proxy_index(uint32_t target);
  struct ProxyLevel { uint32_t w = 0, h = 0, d = 0, internal = 0; } proxy_levels_[7];
  void compressed_image(uint32_t target, int32_t level, uint32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, int32_t size, UserPtr data, bool three_d);
  ArrayView<float> float_view(ByteBuffer& data);

  // Buffers
  struct Mapping { uint32_t object = 0; uint64_t address = 0, pages = 0, offset = 0, length = 0; uint32_t access = 0; };
  List<Mapping> mappings_;
  Mapping* find_mapping(uint32_t object);
  bool unmap_engine_buffer(uint32_t object, bool copy_back);
  uint64_t map_named(uint32_t object, int64_t offset, int64_t length, uint32_t access);
  void flush_range(uint32_t object, int64_t offset, int64_t length);
  void buffer_data_named(uint32_t object, int64_t size, UserPtr data, uint32_t usage);
  void buffer_storage_named(uint32_t object, int64_t size, UserPtr data, uint32_t flags);
  void buffer_sub_data_named(uint32_t object, int64_t offset, int64_t size, UserPtr data);
  void get_buffer_sub_data_named(uint32_t object, int64_t offset, int64_t size, UserPtr data);
  void copy_buffer_named(uint32_t from, uint32_t to, int64_t ro, int64_t wo, int64_t size);
  void buffer_param_get(uint32_t object, uint32_t pname, int64_t* out);
  void bind_range(uint32_t target, uint32_t index, uint32_t buffer, uint64_t offset, uint64_t size);
  void attach_texture(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level, int32_t layer);
  void flush_to_surface();
  void clear_accum();

  // Programs
  static constexpr uint32_t kProgramObjectBit = 0x80000000u;      // in the shared name table: this object is a program
  uint32_t shader_object(uint32_t name) const;
  uint32_t program_object(uint32_t name) const;
  uint32_t deleted_current_program_ = 0;
  uint64_t compat_generation_ = 0;
  bool vertex_attrib_get(uint32_t index, uint32_t pname, double* out, uint32_t* count);
  void uniform_values(int32_t location, uint32_t kind, uint32_t cols, uint32_t rows, int32_t count, UserPtr values, bool transpose, bool doubles);
  void uniform_scalars(int32_t location, uint32_t kind, uint32_t rows, const uint32_t* lanes);
  void with_program(uint32_t name, void (GlApi::*)(), bool*);
  uint32_t begin_program_uniform(uint32_t program);
  void end_program_uniform(uint32_t saved);

  // glGet
  uint64_t extensions_address_ = 0;
  bool get_values(uint32_t pname, double* out, uint32_t* count, uint32_t* kind);
  const float* material_field(uint32_t face, uint32_t pname, uint32_t* n) const;
  bool texenv_get(uint32_t target, uint32_t pname, double* out, uint32_t* n);
  bool texgen_get(uint32_t coord, uint32_t pname, double* out, uint32_t* n);

  // Other
  void map_get(uint32_t target, uint32_t query, UserPtr at, uint32_t kind);
  void query_result_get(uint32_t id, uint32_t pname, uint64_t* out);
  bool ensure_feedback();
  uint32_t name_stack_depth() const;
  uint32_t feedback_capacity() const;
  uint32_t feedback_type() const;
  uint64_t feedback_address() const;
  List<uint64_t> live_syncs_;
  uint64_t sync_serial_ = 0;
};

}  // namespace opengl
