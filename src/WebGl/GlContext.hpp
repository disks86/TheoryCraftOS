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
#include "WebGl/Accelerator.hpp"
#include "WebGl/Raster.hpp"
#include "WebGl/Shader.hpp"
#include "WebGl/TgsiTranslator.hpp"

namespace opengl { class GlApi; class FeedbackSink; }

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
  uint32_t w = 0, h = 0, d = 1;   // (for a multisampled image these are the stored samples: ss x ss of them per pixel)
  uint32_t compressed = 0;        // the compressed format it was uploaded in (it is stored decoded), or 0
  uint32_t ss = 1;                // 2 for a 4-sample image, which is kept as a 2x supersampled image and averaged when resolved
  uint32_t requested_internal = 0;   // desktop GL: the internal format as the program gave it (reads back through glGetTexLevelParameter)
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
constexpr uint32_t kMaxAttribs = 16;          // what a program may use by number (GL_MAX_VERTEX_ATTRIBS)
constexpr uint32_t kMaxAttribSlots = 32;      // what a vertex array holds: past kMaxAttribs are the fixed-function arrays of desktop GL (glNormalPointer ...)
constexpr uint32_t kMaxTextureUnits = 16;
constexpr uint32_t kMaxDrawBuffers = 8;
constexpr uint32_t kMaxUniformBindings = 24;
constexpr uint64_t kMaxTotalBytes = 512ull << 20;
constexpr uint32_t kMaxStorageBindings = 16, kMaxAtomicBindings = 8;

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
  bool immutable = false;      // glBufferStorage
  uint32_t storage_flags = 0;
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
  float border_color[4] = {0, 0, 0, 0};
  uint32_t buffer = 0;      // GL_TEXTURE_BUFFER: the buffer that holds its texels
  uint32_t buffer_format = 0;
  uint64_t buffer_offset = 0, buffer_size = 0;
  bool generate_mipmap_auto = false;      // GL_GENERATE_MIPMAP
  float lod_bias = 0;
  uint32_t depth_mode = 0x1909;           // GL_DEPTH_TEXTURE_MODE: GL_LUMINANCE
  uint32_t swizzle[4] = {0x1903, 0x1904, 0x1905, 0x1906};
  bool stencil_mode = false;              // GL_DEPTH_STENCIL_TEXTURE_MODE is GL_STENCIL_INDEX
};

struct SamplerObj {
  uint32_t min_filter = 0x2702, mag_filter = 0x2601, wrap_s = 0x2901, wrap_t = 0x2901, wrap_r = 0x2901, compare_mode = 0, compare_func = 0x0203;
  float min_lod = -1000, max_lod = 1000;
  float border[4] = {0, 0, 0, 0};
  float lod_bias = 0;
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
  bool layered = false;   // glFramebufferTexture of a 3D, array or cube texture: every layer, the geometry stage's gl_Layer picks one per primitive
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
  int32_t extra_slot[3] = {-1, -1, -1};  // ... and in the geometry, tessellation control and evaluation stages' (ProgramObj::extra)
  uint32_t elem_slots = 1;
  uint32_t location = 0;     // of the first element
  bool in_block = false;
};

struct VaryingLink {
  uint32_t vs_slot, fs_slot, count;   // private slots
  bool flat;
  uint32_t var_offset;                // position in the interpolated varyings array
  bool clamp = false;                 // a compatibility colour (gl_FrontColor ...): clamped to [0, 1] per vertex while GL_CLAMP_VERTEX_COLOR is on
};
// A vertex's value of a varying as the rasterizer takes it (NaN clamps to 0).
inline float clamp_varying(const VaryingLink& l, bool clamping, float v) {
  if (!l.clamp || !clamping) return v;
  return v >= 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f);
}

struct AttribInfo {
  explicit AttribInfo(allocators::IAllocator& a) : name(a) {}
  String name;
  uint32_t gl_type = 0;
  uint32_t size = 1;
  int32_t location = -1;
  uint32_t slot = 0;
  shader::Type type;
};

// How one stage's outputs become the next one's inputs, between the vertex, tessellation and geometry stages (GlStages.cpp). Per vertex, an output of a stage that runs once per
// vertex (vertex, evaluation) goes to element i of the next stage's input array for the primitive's vertex i; the control stage's per-vertex arrays go element by element; per-patch
// values once.
struct StageCopy {
  uint32_t src = 0, dst = 0;            // slots of the first element (src: in the producer's record, or its shared region for the control stage)
  uint32_t count = 0;                   // slots per element
  uint32_t src_stride = 0, dst_stride = 0;
  uint32_t elements = 1;                // control -> evaluation, per vertex: how many elements
  bool per_vertex = false;              // dst is a per-vertex array (index by the vertex)
};

// A geometry or tessellation stage of a program: its module, the shared storage (uniforms; for the control stage the patch's outputs too), the machines, and what feeds it.
struct ExtraStage {
  explicit ExtraStage(allocators::IAllocator& a) : shared(a), machines(a), inputs(a), record(a), initial(a), output_slots(a) {}
  UniquePointer<shader::Module> module;
  List<uint32_t> shared;
  List<UniquePointer<shader::Machine>> machines;   // one; the control stage one per output vertex (they run side by side to each barrier())
  List<StageCopy> inputs;                          // from the stage before
  List<StageCopy> record;                          // a stage that runs per vertex (evaluation): its outputs the next stage reads, gathered (src: its private slot, dst: record slot)
  uint32_t record_slots = 0;
  List<uint32_t> initial;                          // the private region as its initializers leave it
  struct Range { uint32_t slot, count; };
  List<Range> output_slots;                        // control stage: its outputs in the shared region, cleared for each patch
};
enum : uint32_t { kStageGeometry = 0, kStageTessControl = 1, kStageTessEval = 2 };

// What transform feedback captures of each vertex (ProgramObj::tf_plan): `count` slots from `src` of the last pre-raster stage's private region, written at byte `offset`
// of the vertex's ProgramObj::tf_stride[binding] bytes in binding `binding`, for the vertices of stream `stream`. (The bytes no piece covers, gl_SkipComponentsN or the
// gaps between xfb_offsets, are left as they are.)
struct TfPiece {
  uint32_t src = 0, count = 0, binding = 0, offset = 0, stream = 0;
};
constexpr uint32_t kMaxTfComponents = 128;      // per vertex, every binding together (GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS)
constexpr uint32_t kMaxTfSeparateComponents = 64;

struct ProgramObj {
  explicit ProgramObj(allocators::IAllocator& a)
      : log(a), shaders(a), uniforms(a), attribs(a), links(a), vs_shared(a), fs_shared(a), bound_attribs(a), block_bindings(a), tf_varyings(a), tf_plan(a), tf_resources(a), outputs(a), locs(a), vs_record(a) {}
  String log;
  // The attached shaders, in the order attached. WebGL allows one per stage; desktop GL any number, linked together (shader::parse_glsl_units).
  struct Attached { uint32_t id; uint32_t type; };
  List<Attached> shaders;
  bool has_stage(uint32_t type) const { for (const Attached& s : shaders) if (s.type == type) return true; return false; }
  uint32_t first_of(uint32_t type) const { for (const Attached& s : shaders) if (s.type == type) return s.id; return 0; }
  bool has_shader(uint32_t id) const { for (const Attached& s : shaders) if (s.id == id) return true; return false; }
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
  // The linked capture (link_transform_feedback): the pieces, the slots of a vertex's record (the pieces one after another), each binding's bytes per vertex, the
  // primitive the last stage puts out (0 points, 1 lines, 4 triangles), and of each varying its type and number of elements (glGetTransformFeedbackVarying).
  List<TfPiece> tf_plan;
  uint32_t tf_record_slots = 0;
  uint32_t tf_stride[4] = {0, 0, 0, 0};
  uint32_t tf_primitive = 4;
  // The GL_TRANSFORM_FEEDBACK_VARYING resources: the glTransformFeedbackVaryings names (gl_NextBuffer and gl_SkipComponentsN too, with type GL_NONE) or, when the shader
  // has xfb layouts, the outputs they capture. binding is the buffer binding (-1 for gl_NextBuffer), offset the byte offset in its vertex (-1 for gl_NextBuffer).
  struct TfResource {
    explicit TfResource(allocators::IAllocator& a) : name(a) {}
    String name;
    uint32_t gl_type = 0, size = 0;
    int32_t offset = -1, binding = -1;
  };
  List<UniquePointer<TfResource>> tf_resources;
  uint32_t nvar = 0;
  struct Output { int32_t location; uint32_t slot; shader::Type type; };
  List<Output> outputs;                // fragment outputs
  bool frag_writes_all = false;       // gl_FragColor broadcasts to every draw buffer (legacy)
  bool validate_ok = true;
  uint32_t loc_count = 0;
  struct Loc { uint32_t leaf, element; };
  List<Loc> locs;
  UniquePointer<shader::Machine> vmach, fmach;   // made when first drawn with
  // The geometry, tessellation control and evaluation stages (kStage*), when the program has them; and the vertex stage's outputs the first of them reads, gathered per vertex.
  UniquePointer<ExtraStage> extra[3];
  List<StageCopy> vs_record;
  uint32_t vs_record_slots = 0;
  // Clip distances: what the last pre-raster stage writes (gl_ClipDistance, or a compatibility gl_ClipVertex the planes are applied to), as varyings clip_varying ... + 7.
  int32_t clip_varying = -1;
  int32_t clip_distance_slot = -1, clip_vertex_slot = -1;      // in the last stage's private region
  uint32_t clip_count = 0;
  // A vertex's clip distances into its varyings (from the last stage's private region).
  void fill_clip_distances(const uint32_t* privates, uint32_t private_slots, const float planes[8][4], float* varyings) const {
    if (clip_varying < 0) return;
    for (uint32_t i = 0; i < 8; ++i) {
      float d = 0;
      if (clip_distance_slot >= 0 && i < clip_count && static_cast<uint32_t>(clip_distance_slot) + i < private_slots) {
        d = __builtin_bit_cast(float, privates[static_cast<uint32_t>(clip_distance_slot) + i]);
      } else if (clip_vertex_slot >= 0 && static_cast<uint32_t>(clip_vertex_slot) + 4 <= private_slots) {
        for (uint32_t k = 0; k < 4; ++k) d += planes[i][k] * __builtin_bit_cast(float, privates[static_cast<uint32_t>(clip_vertex_slot) + k]);
      }
      varyings[static_cast<uint32_t>(clip_varying) + i] = d;
    }
  }
  // Two-sided colour (compatibility): the back colours the last stage writes (tcos_BackColor, tcos_BackSecondaryColor), carried as varyings `back` ... past nvar (not
  // interpolated), and the front ones' varyings `front` the fragment stage reads, which they replace in a back-facing triangle's vertices.
  struct BackColor { uint32_t src, front, back; bool clamp; };
  BackColor back_colors[2] = {};
  uint32_t back_count = 0;
  void fill_back_colors(const uint32_t* privates, uint32_t private_slots, bool clamping, float* varyings) const {
    for (uint32_t i = 0; i < back_count && i < 2; ++i)
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t s = back_colors[i].src + k;
        float v = s < private_slots ? __builtin_bit_cast(float, privates[s]) : 0.0f;
        if (clamping && back_colors[i].clamp) v = v >= 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f);
        varyings[back_colors[i].back + k] = v;
      }
  }
  // A vertex of a back-facing triangle: its back colours in place of the front ones.
  void use_back_colors(float* varyings) const {
    for (uint32_t i = 0; i < back_count && i < 2; ++i)
      for (uint32_t k = 0; k < 4; ++k) varyings[back_colors[i].front + k] = varyings[back_colors[i].back + k];
  }
  bool has_extra() const { return extra[0] || extra[1] || extra[2]; }
  // The stages by number: 0 vertex, 1 fragment, 2 + kStage* the others (null when absent).
  const shader::Module* stage_module(int s) const { return s == 0 ? vsm.get() : (s == 1 ? fsm.get() : (extra[s - 2] ? extra[s - 2]->module.get() : nullptr)); }
  uint32_t* stage_shared(int s) { return s == 0 ? vs_shared.view().data() : (s == 1 ? fs_shared.view().data() : (extra[s - 2] ? extra[s - 2]->shared.view().data() : nullptr)); }
  // The uniform blocks, one per name across the stages, numbered in stage order: f(index, stage module, block) for each, until it returns false.
  template <class F>
  void for_each_block(F&& f) const {
    uint32_t idx = 0;
    for (int s = 0; s < 5; ++s) {
      const shader::Module* m = stage_module(s);
      if (!m) continue;
      for (uint64_t b = 0; b < m->blocks.count(); ++b) {
        bool seen = false;
        for (int t = 0; t < s && !seen; ++t) {
          const shader::Module* o = stage_module(t);
          if (o) for (uint64_t k = 0; k < o->blocks.count(); ++k) seen = seen || o->blocks[k]->name.view().equals(m->blocks[b]->name.view());
        }
        if (seen) continue;
        if (!f(idx, *m, b)) return;
        ++idx;
      }
    }
  }
  // The stage whose vertices are rasterized: geometry, else evaluation, else vertex.
  const shader::Module& last_module() const { return extra[kStageGeometry] ? *extra[kStageGeometry]->module : (extra[kStageTessEval] ? *extra[kStageTessEval]->module : *vsm); }
  // Hardware acceleration (GlAccel.cpp): the TGSI translation and the accelerator's compiled program, made when the program is first drawn with.
  UniquePointer<TgsiProgram> tgsi;
  IAccelerator* accel = nullptr;
  uint32_t accel_program = 0;
  bool accel_tried = false;
  const char* accel_why = nullptr;   // why the shaders cannot run on the accelerator (a static string), when they cannot
  ~ProgramObj() { release_accel(); }
  void release_accel() {
    if (accel && accel_program) accel->destroy_program(accel_program);
    accel = nullptr;
    accel_program = 0;
    accel_tried = false;
    tgsi.reset();
  }
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
  uint64_t client = 0;     // desktop GL: the address in the program of an array that is not in a buffer (`buffer` is then where the draw copied it)
  bool from_client = false;
  bool bgra = false;       // size GL_BGRA: the first and third components swap
};

struct Vao {
  VertexAttrib attribs[kMaxAttribSlots];
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
  uint64_t positions[4] = {0, 0, 0, 0};   // (while not bound: the bytes captured into each binding so far)
  bool active = false, paused = false;
  uint32_t primitive = 4;                  // glBeginTransformFeedback's mode
  uint64_t vertices[4] = {0, 0, 0, 0};     // captured on each stream since the last begin (glDrawTransformFeedbackStream draws them)
};

struct TextureUnit {
  uint32_t tex2d = 0, cube = 0, tex3d = 0, array = 0;
  uint32_t tex1d = 0, rect = 0, array1d = 0, cube_array = 0, buffer = 0, ms2d = 0, ms2d_array = 0;   // (targets desktop GL adds)
  uint32_t sampler = 0;
};

struct BufferBinding {
  uint32_t buffer = 0;
  uint64_t offset = 0, size = 0;
};

// The objects contexts of a share group have in common (wglShareLists, the share argument of wglCreateContextAttribsARB). Vertex arrays, framebuffers, queries and transform
// feedback objects stay per context, as GL says.
struct GlObjects {
  explicit GlObjects(allocators::IAllocator& a) : buffers(a), textures(a), rbos(a), shaders(a), programs(a), samplers(a) {}
  List<UniquePointer<Buffer>> buffers;
  List<UniquePointer<Texture>> textures;
  List<UniquePointer<Renderbuffer>> rbos;
  List<UniquePointer<ShaderObj>> shaders;
  List<UniquePointer<ProgramObj>> programs;
  List<UniquePointer<SamplerObj>> samplers;
};

// What desktop OpenGL adds to the per-fragment and primitive stages (src/OpenGl sets it; WebGL leaves it at the defaults, for which every test below is a single branch not taken).
struct DesktopState {
  bool active = false;                 // this is a desktop context: primitive restart is optional, more draw modes
  bool primitive_restart = false;
  bool primitive_restart_fixed = false;
  uint32_t restart_index = 0;
  int32_t base_vertex = 0;
  uint32_t base_instance = 0;
  // glPatchParameteri / glPatchParameterfv: the vertices of a patch, and the levels the evaluation stage uses without a control stage.
  uint32_t patch_vertices = 3;
  float patch_outer[4] = {1, 1, 1, 1}, patch_inner[2] = {1, 1};
  uint32_t polygon_mode_front = 0x1B02, polygon_mode_back = 0x1B02;   // GL_FILL
  int32_t edge_flag_attrib = -1;       // the attribute holding the edge flag (only read when a polygon mode is not GL_FILL)
  bool offset_line = false, offset_point = false;
  bool flat_shade = false;             // glShadeModel(GL_FLAT): the colours of the varying range below are flat
  uint32_t flat_first = 0, flat_count = 0;
  bool first_vertex_convention = false;
  bool alpha_test = false;
  uint32_t alpha_func = 0x0207;
  float alpha_ref = 0;
  bool logic_op = false;
  uint32_t logic_mode = 0x1503;        // GL_COPY
  bool line_stipple = false;
  uint32_t stipple_factor = 1;
  uint16_t stipple_pattern = 0xFFFF;
  bool polygon_stipple = false;
  uint32_t polygon_pattern[32] = {};
  bool depth_clamp = false;
  bool multisample = true;
  bool point_sprite = false;
  bool point_size_from_program = false;
  float point_size = 1;
  float point_min = 0, point_max = 64;
  float point_atten[3] = {1, 0, 0};
  bool draw_front_and_back = false;    // the default framebuffer: GL_FRONT_AND_BACK
  Image* front_also = nullptr;         // where the second copy goes
  uint32_t clip_mask = 0;              // glEnable(GL_CLIP_PLANEi) / GL_CLIP_DISTANCEi
  int32_t clip_varying = -1;           // the first of the program's clip distances among the varyings (one scalar per set bit, lowest first)
  bool clip_indexed = false;           // a program's: the distance of plane i is varying clip_varying + i (not packed by set bit)
  float clip_planes[8][4] = {};        // the planes in eye space (glClipPlane), for a program's gl_ClipVertex
  bool two_side_color = false;         // a program's back colours (ProgramObj::back_colors) replace the front ones of back-facing triangles
  bool clamp_vertex_color = true;      // glClampColor(GL_CLAMP_VERTEX_COLOR): the colours a program's last pre-raster stage writes are clamped (VaryingLink::clamp)
  uint8_t color_mask[kMaxDrawBuffers] = {15, 15, 15, 15, 15, 15, 15, 15};
  bool per_buffer_masks = false;
  // one blend state per draw buffer (glBlendFunci, glEnablei(GL_BLEND, i)) when per_buffer_blend is set
  bool per_buffer_blend = false;
  struct BufferBlend { bool enabled = false; uint32_t eq_rgb = 0x8006, eq_a = 0x8006, sr = 1, dr = 0, sa = 1, da = 0; } blend[kMaxDrawBuffers];
  // Where the vertices go instead of the rasterizer: feedback and selection modes (glRenderMode).
  gpu::IPrimitiveSink* primitive_sink = nullptr;
  uint32_t primitive_id_divisor = 1;   // gl_PrimitiveID without a geometry stage counts this many of the engine's primitives as one (quads: 2; polygons: 0, all one)
  ConstByteView triangle_edges;        // polygons cut into triangles: one byte per triangle, bits 0..2 say which edges are real ones
  uint32_t ff_1d_mask = 0, ff_rect_mask = 0;   // texture units whose sampler2D reaches the unit's 1D (rectangle) texture: fixed-function programs
};

// --- The context ---------------------------------------------------------------------------------------------------------------------------------------

class GlContext final : public shader::ITextureHost {
 public:
  struct Attributes {
    bool alpha = true, depth = true, stencil = false, antialias = true, premultiplied = true, preserve = false;
  };

  // `shared`: the objects to use, which outlive this context (null: its own).
  GlContext(allocators::IAllocator& a, bool es3, uint32_t width, uint32_t height, const Attributes& attrs, GlObjects* shared = nullptr);
  ~GlContext();
  GlContext(const GlContext&) = delete;
  // True while the default framebuffer is drawn by the 3D hardware (for tests and the browser's status).
  bool hardware_drawing() const { return hw_valid_ && hw_target_ != 0; }
  // How the draws of the default framebuffer went: on the 3D hardware or on the CPU, and why the last one the hardware did not take was refused (a static string).
  struct HardwareStats {
    uint64_t gpu_draws = 0, cpu_draws = 0;
    const char* last_refusal = nullptr;
    // the reasons draws went to the CPU, and how many each (the first kReasons distinct ones)
    static constexpr uint32_t kReasons = 8;
    struct Reason { const char* why = nullptr; uint64_t count = 0; } reasons[kReasons];
    void refused(const char* why) {
      ++cpu_draws;
      last_refusal = why;
      for (Reason& r : reasons) {
        if (r.why == why || !r.why) {
          r.why = why;
          ++r.count;
          return;
        }
      }
    }
  };
  const HardwareStats& hardware_stats() const { return hw_stats_; }
  GlContext& operator=(const GlContext&) = delete;

  bool valid() const { return valid_; }
  bool es3() const { return es3_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  const Attributes& attributes() const { return attrs_; }
  void resize(uint32_t w, uint32_t h);
  // Desktop GL: the default framebuffer lives in a window surface shared by the contexts drawing to it (src/OpenGl/GlSurface.hpp). `read` is the colour buffer reads take; null
  // `color` = there is no default framebuffer (an incomplete one).
  void set_default_images(Image* color, Image* read, Image* depth, uint32_t width, uint32_t height);
  // Desktop GL (wglShareLists): from now on the context uses these objects. Names of the old ones bound in the context must be dropped by the caller first.
  void set_objects(GlObjects* objects) { objects_ = objects ? objects : &own_objects_; }
  DesktopState desktop;
  bool lost() const { return lost_; }
  bool antialiased() const { return dcolor_->ss > 1; }
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
  // The bytes of the buffer bound to PIXEL_UNPACK_BUFFER from `offset` on / of PIXEL_PACK_BUFFER (false if none is bound or the offset is past its end).
  bool pixel_unpack_view(uint64_t offset, ConstByteView* out);
  bool pixel_pack_view(uint64_t offset, ByteView* out);
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
  // `data` holds the blocks (see GlCompressed.hpp). The size and offsets must be whole blocks unless they reach the edge of the level.
  void compressed_tex_image(uint32_t target, int32_t level, uint32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, ConstByteView data, bool three_d);
  void compressed_tex_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, ConstByteView data, bool three_d);
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
  // (state read back, for desktop GL's glGet and attribute stacks)
  uint32_t get_depth_func() const { return dfunc_; }
  bool get_depth_mask() const { return dmask_; }
  void cull_face(uint32_t m) { cull_mode_ = m; }
  void front_face(uint32_t m) { front_face_ = m; }
  void polygon_offset(float factor, float units) { po_factor_ = factor; po_units_ = units; }
  void line_width(float w) { if (!(w > 0)) { set_error(0x501); return; } line_width_ = w; }
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
  void release_if_unattached(uint32_t shader);
  void free_program(uint32_t id);
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
  void framebuffer_texture_layered(uint32_t target, uint32_t attachment, uint32_t tex, int32_t level);
  uint32_t uniform_block_count(uint32_t program) const;
  bool uniform_block_info(uint32_t program, uint32_t block, uint32_t pname, double* out);
  StringView uniform_block_name(uint32_t program, uint32_t block);
  void transform_feedback_varyings(uint32_t program, ArrayView<const StringView> names, uint32_t mode);
  void begin_transform_feedback(uint32_t mode);
  void end_transform_feedback();
  void pause_transform_feedback();
  void resume_transform_feedback();
  // glBindTransformFeedback: the bindings and positions of the object bound before are kept in it, the new one's become the context's.
  void bind_transform_feedback(uint32_t id);
  // glGetTransformFeedbackVarying of a linked program; false (and the error set) if there is no such one.
  bool transform_feedback_varying(uint32_t program, uint32_t index, String* name, uint32_t* size, uint32_t* gl_type);
  // glDrawTransformFeedback: the vertices object `id` (0 the default one) captured between its last begin and end; false (and the error set) if it has none to give.
  bool transform_feedback_vertices(uint32_t id, uint32_t stream, uint64_t* vertices);

  // The transform feedback and primitive queries of one draw (GlProgram.cpp). The stage that ends vertex processing hands over each vertex of a primitive (vertex(), or
  // record() with what gather() made of it earlier), then calls primitive(): the primitive is counted (GL_PRIMITIVES_GENERATED) and, while feedback is on, written
  // whole, or not at all when a buffer has no room left for it (and then not counted as written).
  class TfRecorder {
   public:
    TfRecorder(GlContext& gl, const ProgramObj& p);
    // False if out of memory.
    bool ok() const { return ok_; }
    bool capturing() const { return capturing_; }
    uint32_t record_slots() const { return p_.tf_record_slots; }
    void gather(ArrayView<const uint32_t> privates, ArrayView<uint32_t> record) const;
    void vertex(ArrayView<const uint32_t> privates);
    void record(ArrayView<const uint32_t> record);
    // A primitive of stream `stream` (only stream 0 is rasterized; the others are points of the geometry stage).
    void primitive(uint32_t stream = 0);

   private:
    GlContext& gl_;
    const ProgramObj& p_;
    bool capturing_ = false, ok_ = true;
    uint32_t pending_ = 0;      // vertices of the primitive so far
    List<uint32_t> records_;    // their records (3 * record_slots())
  };

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
  // `index`: the vertex stream of a primitive query (glBeginQueryIndexed); 0 for the others.
  void begin_query(uint32_t target, uint32_t id, uint32_t index = 0);
  void end_query(uint32_t target, uint32_t index = 0);
  bool query_result(uint32_t id, uint32_t pname, double* out);
  uint32_t current_query(uint32_t target, uint32_t index = 0) const;

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
  friend class opengl::GlApi;   // the desktop layer works on the same objects
  friend class opengl::FeedbackSink;
  friend struct FragmentPipeline;

  Buffer* buffer_at(uint32_t id) { return id && id <= objects_->buffers.count() ? objects_->buffers[id - 1].get() : nullptr; }
  const Buffer* buffer_at(uint32_t id) const { return id && id <= objects_->buffers.count() ? objects_->buffers[id - 1].get() : nullptr; }
  Texture* texture_at(uint32_t id) { return id && id <= objects_->textures.count() ? objects_->textures[id - 1].get() : nullptr; }
  const Texture* texture_at(uint32_t id) const { return id && id <= objects_->textures.count() ? objects_->textures[id - 1].get() : nullptr; }
  Framebuffer* fb_at(uint32_t id) { return id && id <= fbos_.count() ? fbos_[id - 1].get() : nullptr; }
  Renderbuffer* rb_at(uint32_t id) { return id && id <= objects_->rbos.count() ? objects_->rbos[id - 1].get() : nullptr; }
  ShaderObj* shader_at(uint32_t id) { return id && id <= objects_->shaders.count() ? objects_->shaders[id - 1].get() : nullptr; }
  ProgramObj* program_at(uint32_t id) { return id && id <= objects_->programs.count() ? objects_->programs[id - 1].get() : nullptr; }
  Vao* vao_at(uint32_t id) { return id == 0 ? &default_vao_ : (id <= vaos_.count() ? vaos_[id - 1].get() : nullptr); }
  Vao* current_vao_ptr() { return vao_at(vao_id_); }
  SamplerObj* sampler_at(uint32_t id) { return id && id <= objects_->samplers.count() ? objects_->samplers[id - 1].get() : nullptr; }
  QueryObj* query_at(uint32_t id) { return id && id <= queries_.count() ? queries_[id - 1].get() : nullptr; }

  Image* texture_image(Texture* t, uint32_t target, int32_t level);   // the image for a (face) target and level, or null
  Image* make_texture_image(Texture* t, uint32_t target, int32_t level);
  Texture* bound_texture_for(uint32_t target, uint32_t* face);
  uint32_t* bound_texture_slot(uint32_t target);
  static uint32_t face_index(uint32_t target);
  bool texture_complete(const Texture& t, const SamplerObj& s, bool* is_cube) const;
  void sample_texture(const Texture& t, const SamplerObj& s, const shader::SampleRequest& rq, uint32_t out[4]) const;
  void depth_texture_mode(const Texture& t, const shader::SampleRequest& rq, uint32_t out[4]) const;

  // render targets
  struct Target {
    Image* color[kMaxDrawBuffers] = {};
    uint32_t color_count = 0;
    Image* depth = nullptr;        // Depth or DepthStencil image
    Image* stencil = nullptr;      // DepthStencil image (same as depth when packed)
    uint32_t w = 0, h = 0;
    uint32_t ss = 1;
    uint32_t layer[kMaxDrawBuffers] = {};
    uint32_t depth_layer = 0;
    bool is_default = false;
    bool ok = false;
    // layered attachments: the framebuffer's fewest layers, and the one the images above are of
    bool layered = false;
    uint32_t layers = 1, current_layer = 0, fb = 0;
  };
  bool resolve_image(const Attachment& at, Image** img, uint32_t* layer, uint32_t select = 0);
  static uint32_t attachment_layers(const Attachment& at, const Image& im);
  void resolve_target(uint32_t fb, bool draw, Target* t, uint32_t select_layer = 0);
  bool framebuffer_complete(uint32_t fb, uint32_t* status);

  void link_failed(ProgramObj* p, StringView why);
  bool link_collect_uniforms(ProgramObj* p);
  void flatten_uniform(ProgramObj* p, const shader::Module& m, int stage, const shader::Global& g, const shader::Type& t, StringView name, uint32_t slot);
  bool link_extra_stages(ProgramObj* p);
  bool link_stage_inputs(ProgramObj* p, const shader::Module& producer, bool producer_is_control, ExtraStage& consumer, List<StageCopy>* record, uint32_t* record_slots, const char* name);
  void setup_program_storage(ProgramObj* p);
  void load_uniform_blocks(ProgramObj* p);
  uint32_t uniform_gl_type(const shader::Type& t) const;

  struct VertexOut;
  bool run_draw(uint32_t mode, int32_t first, int32_t count, uint32_t index_type, uint64_t index_offset, int32_t instances, bool indexed);
  // run_draw's way through the geometry and tessellation stages (and adjacency primitives without them); the lambdas are run_draw's (GlDraw.cpp).
  template <class RunVertex, class ShadeVertex, class EmitTriangle, class EmitLine, class Pipe>
  bool draw_through_stages(ProgramObj& p, uint32_t mode, int32_t count, int32_t instances, int32_t first, const uint8_t* idx, uint32_t index_type, bool indexed, bool restart_ok,
                           uint32_t restart_val, RunVertex& run_vertex, ShadeVertex& shade_vertex, EmitTriangle& emit_triangle, EmitLine& emit_line, gpu::RasterState& rs, Pipe& pipe,
                           TfRecorder& tf);
  bool link_transform_feedback(ProgramObj* p, const shader::Module& last);
  TransformFeedbackObj* current_tf() { return tf_bound_ ? tfs_[tf_bound_ - 1].get() : &default_tf_; }
  uint32_t fetch_index(const uint8_t* idx, uint32_t type, uint32_t i) const;
  void mark_dirty(int32_t x, int32_t y);
  Image* default_color() { return dcolor_; }

  allocators::IAllocator& a_;
  bool es3_;
  bool valid_ = false;
  bool lost_ = false;
  Attributes attrs_;
  uint32_t width_, height_;
  uint32_t error_ = 0;
  uint64_t version_ = 1;
  uint64_t total_bytes_ = 0;

  GlObjects own_objects_;
  GlObjects* objects_;      // own_objects_, or those of a share group (set_objects)
  List<UniquePointer<Framebuffer>> fbos_;
  List<UniquePointer<Vao>> vaos_;
  List<UniquePointer<QueryObj>> queries_;
  List<UniquePointer<TransformFeedbackObj>> tfs_;
  Vao default_vao_;
  TransformFeedbackObj default_tf_;

  Image own_color_, own_depth_;
  Image* dcolor_ = nullptr;
  Image* dread_ = nullptr;
  Image* ddepth_ = nullptr;
  gpu::TouchBox dirty_;
  bool all_dirty_ = true;

  // bindings
  uint32_t array_buffer_ = 0, copy_read_ = 0, copy_write_ = 0, pixel_pack_ = 0, pixel_unpack_ = 0, tf_buffer_ = 0, uniform_buffer_ = 0;
  BufferBinding ubo_bindings_[kMaxUniformBindings];
  BufferBinding tf_bindings_[4];
  BufferBinding ssbo_bindings_[kMaxStorageBindings];
  BufferBinding atomic_bindings_[kMaxAtomicBindings];
  TextureUnit units_[kMaxTextureUnits];
  uint32_t active_unit_ = 0;
  uint32_t extra_buffers_[6] = {};      // the buffer targets only desktop GL has
  static int32_t extra_buffer_slot(uint32_t target);
  uint32_t rb_bound_ = 0, read_fb_ = 0, draw_fb_ = 0;
  uint32_t program_ = 0, vao_id_ = 0, tf_bound_ = 0;
  uint32_t query_active_[4] = {0, 0, 0, 0};
  uint32_t prim_queries_[2][4] = {};      // the GL_PRIMITIVES_GENERATED [0] and GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN [1] query of each vertex stream

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

  // --- 3D hardware (GlAccel.cpp). Only the default framebuffer is drawn by the accelerator; hw_target_ holds its picture on the device.
  // hw_valid_: the device picture is right, so the next accelerated draw may build on it. hw_ahead_: it is also newer than def_color_, which hw_sync() then updates.
  // Anything on the CPU that writes def_color_ goes through hw_cpu_write() first (read the device picture back, then the device picture is out of date).
  IAccelerator* hw_owner_ = nullptr;
  uint32_t hw_target_ = 0;
  bool hw_valid_ = false;
  bool hw_ahead_ = false;
  uint32_t hw_w_ = 0, hw_h_ = 0;    // the size of hw_target_
  HardwareStats hw_stats_;
  AccelTextureCache hw_textures_;   // the device copies of the textures the accelerated draws sampled, by GL name
  bool hw_texture(uint32_t unit, uint32_t* handle, AccelSampler* sampler);
  void hw_forget_texture(uint32_t name);
  void hw_drop();
  void hw_retarget(uint32_t w, uint32_t h);
  void hw_sync();
  void hw_cpu_write() { hw_sync(); hw_valid_ = false; }
  // A whole-buffer clear done on the device alone (see clear()); the CPU's depth and stencil clears it leaves for later.
  bool hw_fast_clear(uint32_t mask);
  void hw_apply_pending_clears();
  bool hw_pending_depth_ = false, hw_pending_stencil_ = false, hw_clearing_cpu_ = false;
  float hw_pending_depth_value_ = 1;
  int32_t hw_pending_stencil_value_ = 0;
  uint32_t hw_pending_stencil_mask_ = 0xFF;
  void hw_after_clear(uint32_t mask, bool whole);
  bool hw_target(IAccelerator* acc);
  bool hw_program(ProgramObj& p, IAccelerator& acc);
  bool hw_draw(ProgramObj& p, const Target& target, uint32_t mode, int32_t first, int32_t count, uint32_t index_type, const uint8_t* idx, bool indexed, int32_t instances);
  bool hw_draw_inner(ProgramObj& p, const Target& target, uint32_t mode, int32_t first, int32_t count, uint32_t index_type, const uint8_t* idx, bool indexed, int32_t instances,
                     const char** why);
};

// Vertex fetch shared by the software and accelerated draws (GlDraw.cpp): the four float lanes (as bit patterns, missing ones 0, 0, 0, 1) of the attribute element at
// `element`, its size in bytes, and an index of a given GL type read from `p`.
void read_vertex_attrib(const VertexAttrib& at, const uint8_t* element, uint32_t out[4]);
uint32_t vertex_attrib_element_bytes(const VertexAttrib& at);
uint32_t read_index_value(const uint8_t* p, uint32_t type);

}  // namespace webgl
