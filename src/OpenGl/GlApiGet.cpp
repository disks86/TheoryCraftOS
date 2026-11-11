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

// glGet*, glGetString, errors, and the parts of fixed-function state read back.
#include "OpenGl/FixedFunction.hpp"
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

namespace {
const char* const kExtensions[] = {
    "GL_ARB_multitexture", "GL_ARB_transpose_matrix", "GL_ARB_vertex_buffer_object", "GL_ARB_pixel_buffer_object", "GL_ARB_shader_objects", "GL_ARB_vertex_shader", "GL_ARB_fragment_shader",
    "GL_ARB_shading_language_100", "GL_ARB_texture_env_add", "GL_ARB_texture_env_combine", "GL_ARB_texture_env_crossbar", "GL_ARB_texture_env_dot3", "GL_ARB_texture_cube_map", "GL_ARB_depth_texture",
    "GL_ARB_shadow", "GL_ARB_point_parameters", "GL_ARB_point_sprite", "GL_ARB_occlusion_query", "GL_ARB_occlusion_query2", "GL_ARB_vertex_array_object", "GL_ARB_framebuffer_object", "GL_ARB_map_buffer_range",
    "GL_ARB_copy_buffer", "GL_ARB_draw_elements_base_vertex", "GL_ARB_draw_instanced", "GL_ARB_instanced_arrays", "GL_ARB_base_instance", "GL_ARB_sampler_objects", "GL_ARB_buffer_storage", "GL_ARB_texture_storage",
    "GL_ARB_uniform_buffer_object", "GL_ARB_program_interface_query", "GL_ARB_tessellation_shader", "GL_ARB_texture_non_power_of_two", "GL_ARB_texture_rectangle", "GL_ARB_texture_compression", "GL_ARB_texture_float", "GL_ARB_texture_rg", "GL_ARB_half_float_pixel", "GL_ARB_half_float_vertex",
    "GL_ARB_seamless_cube_map", "GL_ARB_explicit_attrib_location", "GL_ARB_provoking_vertex", "GL_ARB_draw_buffers", "GL_ARB_draw_buffers_blend", "GL_ARB_blend_func_extended_unavailable_placeholder",
    "GL_EXT_abgr", "GL_EXT_bgra", "GL_EXT_blend_color", "GL_EXT_blend_minmax", "GL_EXT_blend_subtract", "GL_EXT_blend_func_separate", "GL_EXT_draw_range_elements", "GL_EXT_multi_draw_arrays", "GL_EXT_packed_pixels",
    "GL_EXT_packed_depth_stencil", "GL_EXT_rescale_normal", "GL_EXT_secondary_color", "GL_EXT_separate_specular_color", "GL_EXT_stencil_wrap", "GL_EXT_texture_edge_clamp", "GL_EXT_texture3D", "GL_EXT_texture_env_add", "GL_EXT_texture_env_combine",
    "GL_EXT_texture_filter_anisotropic", "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_sRGB", "GL_EXT_framebuffer_object", "GL_EXT_framebuffer_blit", "GL_EXT_vertex_array", "GL_EXT_fog_coord", "GL_EXT_polygon_offset", "GL_EXT_texture_object",
    "GL_NV_blend_square", "GL_SGIS_generate_mipmap", "GL_SGIS_texture_edge_clamp", "GL_SGIS_texture_lod", "GL_IBM_texture_mirrored_repeat"};
constexpr uint32_t kExtensionCount = sizeof(kExtensions) / sizeof(kExtensions[0]) - 0;

bool real_extension(const char* e) { return StringView(e) != StringView("GL_ARB_blend_func_extended_unavailable_placeholder"); }
}  // namespace

uint64_t GlApi::glGetError() {
  return gl_.get_error();
}

uint64_t GlApi::glGetString(GLenum name) {
  switch (name) {
    case GL_VENDOR: return client_string("TheoryCraftOS");
    case GL_RENDERER: {
      // (with a 3D accelerator, the draws it can take run on it: GlAccel.cpp)
      const webgl::IAccelerator* acc = webgl::accelerator();
      if (!acc) return client_string("TheoryCraftOS kernel GL (software)");
      if (renderer_.empty()) {
        renderer_.assign("TheoryCraftOS kernel GL (");
        renderer_.append(acc->name());
        renderer_.append(", the CPU for what it cannot draw)");
      }
      return client_string(renderer_.c_str());
    }
    case GL_VERSION: return client_string(kVersionString);
    case GL_SHADING_LANGUAGE_VERSION: return client_string(kGlslVersionString);
    case GL_EXTENSIONS: {
      if (config_.core_profile) { error(GL_INVALID_ENUM); return 0; }
      if (!extensions_address_) {
        String all(a_);
        for (const char* e : kExtensions) {
          if (!real_extension(e)) continue;
          all.append(e);
          all.append(" ");
        }
        if (!memory_) return 0;
        extensions_address_ = memory_->allocate(all.view().size() + 1);
        if (extensions_address_) {
          memory_->write(extensions_address_, ConstByteView(reinterpret_cast<const uint8_t*>(all.c_str()), all.view().size() + 1));
        }
      }
      return extensions_address_;
    }
    default: error(GL_INVALID_ENUM); return 0;
  }
}

uint64_t GlApi::glGetStringi(GLenum name, GLuint index) {
  if (name != GL_EXTENSIONS) { error(GL_INVALID_ENUM); return 0; }
  uint32_t at = 0;
  for (const char* e : kExtensions) {
    if (!real_extension(e)) continue;
    if (at == index) return client_string(e);
    ++at;
  }
  error(GL_INVALID_VALUE);
  return 0;
}

// Values of glGet as doubles; `kind` says how the integer form is made (0: round, 1: colours scale to the integer range).
bool GlApi::get_values(uint32_t pname, double* out, uint32_t* count, uint32_t* kind) {
  *count = 1;
  *kind = 0;
  const auto vec = [&](const float* v, uint32_t n, bool colour = false) {
    for (uint32_t i = 0; i < n; ++i) out[i] = v[i];
    *count = n;
    if (colour) *kind = 1;
    return true;
  };
  const auto mat = [&](const MatrixStack& s, bool transpose = false) {
    const Mat4 m = transpose ? mat_transpose(s.entries[s.top]) : s.entries[s.top];
    for (int i = 0; i < 16; ++i) out[i] = m.m[i];
    *count = 16;
    return true;
  };
  const auto one = [&](double v) { out[0] = v; return true; };
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  const uint32_t client_unit = client_active_unit_ < kMaxTexUnits ? client_active_unit_ : 0;
  const webgl::Vao* vao = gl_.current_vao_ptr();
  const auto array_info = [&](uint32_t attr, uint32_t what) -> double {
    const webgl::VertexAttrib& at = vao->attribs[attr];
    switch (what) {
      case 0: return at.bgra ? GL_BGRA : at.size;
      case 1: return at.type;
      case 2: return at.stride;
      default: return at.from_client ? 0 : group_->buffers.name_of(at.buffer);
    }
  };
  // Names of bound objects instead of the engine's numbers.
  const auto texture_name = [&](uint32_t object, uint32_t index) { return object == default_textures_[index] ? 0.0 : static_cast<double>(group_->textures.name_of(object)); };
  const webgl::TextureUnit& eu = gl_.units_[gl_.active_unit_];
  switch (pname) {
    // Limits and versions
    case GL_MAJOR_VERSION: return one(config_.major);
    case GL_MINOR_VERSION: return one(config_.minor);
    case GL_CONTEXT_FLAGS: return one((config_.forward_compatible ? GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT : 0) | (config_.debug ? GL_CONTEXT_FLAG_DEBUG_BIT : 0));
    case GL_CONTEXT_PROFILE_MASK: return one(config_.core_profile ? GL_CONTEXT_CORE_PROFILE_BIT : GL_CONTEXT_COMPATIBILITY_PROFILE_BIT);
    case GL_NUM_EXTENSIONS: { uint32_t n = 0; for (const char* e : kExtensions) if (real_extension(e)) ++n; return one(n); }
    case GL_MAX_LIGHTS: return one(kMaxLights);
    case GL_MAX_CLIP_PLANES: return one(kMaxClipPlanes);
    case GL_MAX_MODELVIEW_STACK_DEPTH: return one(kModelviewDepth);
    case GL_MAX_PROJECTION_STACK_DEPTH: return one(kProjectionDepth);
    case GL_MAX_TEXTURE_STACK_DEPTH: return one(kTextureDepth);
    case GL_MAX_COLOR_MATRIX_STACK_DEPTH: return one(kColorDepth);
    case GL_MAX_ATTRIB_STACK_DEPTH: return one(kMaxAttribStack);
    case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: return one(kMaxClientAttribStack);
    case GL_MAX_LIST_NESTING: return one(64);
    case GL_MAX_NAME_STACK_DEPTH: return one(64);
    case GL_MAX_EVAL_ORDER: return one(32);
    case GL_MAX_PIXEL_MAP_TABLE: return one(PixelMap::kMaxSize);
    case GL_MAX_TEXTURE_UNITS: return one(kMaxTexUnits);
    case GL_MAX_TEXTURE_COORDS: return one(kMaxTexUnits);
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: return one(webgl::kMaxTextureUnits - 2);
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: return one(webgl::kMaxTextureUnits - 2);
    case GL_MAX_TEXTURE_SIZE: return one(16384);
    case GL_MAX_RECTANGLE_TEXTURE_SIZE: return one(16384);
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: return one(16384);
    case GL_MAX_3D_TEXTURE_SIZE: return one(2048);
    case GL_MAX_ARRAY_TEXTURE_LAYERS: return one(2048);
    case GL_MAX_TEXTURE_LOD_BIAS: return one(16);
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: return one(1 << 24);
    case GL_MAX_VIEWPORT_DIMS: out[0] = out[1] = 16384; *count = 2; return true;
    case GL_MAX_RENDERBUFFER_SIZE: return one(16384);
    case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS: return one(webgl::kMaxDrawBuffers);
    case GL_MAX_VARYING_COMPONENTS: return one(gpu::kMaxVarying);
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: return one(16384);
    // geometry and tessellation (the same values the GLSL front end gives the gl_Max* constants)
    case GL_MAX_GEOMETRY_INPUT_COMPONENTS: return one(64);
    case GL_MAX_GEOMETRY_OUTPUT_COMPONENTS: return one(128);
    case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS: return one(16);
    case GL_MAX_GEOMETRY_OUTPUT_VERTICES: return one(256);
    case GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS: return one(1024);
    case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS: return one(1024);
    case GL_MAX_GEOMETRY_SHADER_INVOCATIONS: return one(32);
    case GL_MAX_PATCH_VERTICES: return one(32);
    case GL_MAX_TESS_GEN_LEVEL: return one(64);
    case GL_MAX_TESS_CONTROL_INPUT_COMPONENTS: case GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS: case GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS:
    case GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS: return one(128);
    case GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS: case GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS: return one(16);
    case GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS: case GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS: return one(1024);
    case GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS: return one(4096);
    case GL_MAX_TESS_PATCH_COMPONENTS: return one(120);
    case GL_PATCH_VERTICES: return one(gl_.desktop.patch_vertices);
    case GL_PATCH_DEFAULT_OUTER_LEVEL: for (int i = 0; i < 4; ++i) out[i] = gl_.desktop.patch_outer[i]; *count = 4; *kind = 1; return true;
    case GL_PATCH_DEFAULT_INNER_LEVEL: for (int i = 0; i < 2; ++i) out[i] = gl_.desktop.patch_inner[i]; *count = 2; *kind = 1; return true;
    case GL_MAX_VERTEX_ATTRIBS: return one(webgl::kMaxAttribs);
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: return one(webgl::kMaxUniformBindings);
    case GL_MAX_UNIFORM_BLOCK_SIZE: return one(1 << 24);
    case GL_MAX_COMBINED_UNIFORM_BLOCKS: case GL_MAX_VERTEX_UNIFORM_BLOCKS: case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: return one(12);
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: return one(16);
    case GL_POINT_SIZE_RANGE: out[0] = 1; out[1] = 2047; *count = 2; return true;
    case GL_POINT_SIZE_GRANULARITY: return one(0.125);
    case GL_LINE_WIDTH_RANGE: out[0] = 1; out[1] = 16; *count = 2; return true;
    case GL_LINE_WIDTH_GRANULARITY: return one(0.125);
    case GL_ALIASED_POINT_SIZE_RANGE: out[0] = 1; out[1] = 2047; *count = 2; return true;
    case GL_ALIASED_LINE_WIDTH_RANGE: out[0] = 1; out[1] = 16; *count = 2; return true;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: return one(8);
    case GL_ALPHA_BITS: return one(surface_ ? surface_->config().alpha_bits : 8);
    case GL_DEPTH_BITS: return one(surface_ ? surface_->config().depth_bits : 24);
    case GL_STENCIL_BITS: return one(surface_ ? surface_->config().stencil_bits : 8);
    case GL_ACCUM_RED_BITS: case GL_ACCUM_GREEN_BITS: case GL_ACCUM_BLUE_BITS: case GL_ACCUM_ALPHA_BITS: return one(surface_ && !surface_->accum().empty() ? 16 : 0);
    case GL_INDEX_BITS: return one(0);
    case GL_AUX_BUFFERS: return one(0);
    case GL_RGBA_MODE: return one(1);
    case GL_INDEX_MODE: return one(0);
    case GL_DOUBLEBUFFER: return one(surface_ ? surface_->double_buffered() : 1);
    case GL_STEREO: return one(0);
    case GL_SUBPIXEL_BITS: return one(8);
    case GL_SAMPLE_BUFFERS: case GL_SAMPLES: return one(0);
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: return one(0);
    case GL_COMPRESSED_TEXTURE_FORMATS: *count = 0; return true;
    case GL_TEXTURE_COMPRESSION_HINT: return one(fx_.hints[6]);
    // Colours and matrices
    case GL_CURRENT_COLOR: return vec(fx_.current_color, 4, true);
    case GL_CURRENT_SECONDARY_COLOR: return vec(fx_.current_secondary, 4, true);
    case GL_CURRENT_NORMAL: return vec(fx_.current_normal, 3, true);
    case GL_CURRENT_TEXTURE_COORDS: return vec(fx_.current_tex[unit], 4);
    case GL_CURRENT_INDEX: return one(fx_.current_index);
    case GL_CURRENT_FOG_COORD: return one(fx_.current_fog);
    case GL_EDGE_FLAG: return one(fx_.current_edge);
    case GL_CURRENT_RASTER_POSITION: return vec(fx_.raster_pos, 4);
    case GL_CURRENT_RASTER_COLOR: return vec(fx_.raster_color, 4, true);
    case GL_CURRENT_RASTER_SECONDARY_COLOR: return vec(fx_.raster_secondary, 4, true);
    case GL_CURRENT_RASTER_TEXTURE_COORDS: return vec(fx_.raster_tex[unit], 4);
    case GL_CURRENT_RASTER_DISTANCE: return one(fx_.raster_distance);
    case GL_CURRENT_RASTER_INDEX: return one(fx_.raster_index);
    case GL_CURRENT_RASTER_POSITION_VALID: return one(fx_.raster_valid);
    case GL_MODELVIEW_MATRIX: return mat(modelview_);
    case GL_PROJECTION_MATRIX: return mat(projection_);
    case GL_TEXTURE_MATRIX: return mat(texture_stack_[unit]);
    case GL_COLOR_MATRIX: return mat(color_stack_);
    case GL_TRANSPOSE_MODELVIEW_MATRIX: return mat(modelview_, true);
    case GL_TRANSPOSE_PROJECTION_MATRIX: return mat(projection_, true);
    case GL_TRANSPOSE_TEXTURE_MATRIX: return mat(texture_stack_[unit], true);
    case GL_TRANSPOSE_COLOR_MATRIX: return mat(color_stack_, true);
    case GL_MODELVIEW_STACK_DEPTH: return one(modelview_.top + 1);
    case GL_PROJECTION_STACK_DEPTH: return one(projection_.top + 1);
    case GL_TEXTURE_STACK_DEPTH: return one(texture_stack_[unit].top + 1);
    case GL_COLOR_MATRIX_STACK_DEPTH: return one(color_stack_.top + 1);
    case GL_ATTRIB_STACK_DEPTH: return one(attrib_stack_.count());
    case GL_CLIENT_ATTRIB_STACK_DEPTH: return one(client_stack_.count());
    case GL_MATRIX_MODE: return one(fx_.matrix_mode);
    case GL_LIGHT_MODEL_AMBIENT: return vec(fx_.model_ambient, 4, true);
    case GL_LIGHT_MODEL_LOCAL_VIEWER: return one(fx_.local_viewer);
    case GL_LIGHT_MODEL_TWO_SIDE: return one(fx_.two_side);
    case GL_LIGHT_MODEL_COLOR_CONTROL: return one(fx_.separate_specular ? GL_SEPARATE_SPECULAR_COLOR : GL_SINGLE_COLOR);
    case GL_COLOR_MATERIAL_FACE: return one(fx_.color_material_face);
    case GL_COLOR_MATERIAL_PARAMETER: return one(fx_.color_material_mode);
    case GL_SHADE_MODEL: return one(fx_.shade_model);
    case GL_FOG_COLOR: return vec(fx_.fog_color, 4, true);
    case GL_FOG_MODE: return one(fx_.fog_mode);
    case GL_FOG_DENSITY: return one(fx_.fog_density);
    case GL_FOG_START: return one(fx_.fog_start);
    case GL_FOG_END: return one(fx_.fog_end);
    case GL_FOG_INDEX: return one(fx_.fog_index);
    case GL_FOG_COORD_SRC: return one(fx_.fog_source);
    case GL_POINT_SIZE: return one(fx_.point_size);
    case GL_POINT_SIZE_MIN: return one(fx_.point_min);
    case GL_POINT_SIZE_MAX: return one(fx_.point_max);
    case GL_POINT_FADE_THRESHOLD_SIZE: return one(fx_.point_fade);
    case GL_POINT_DISTANCE_ATTENUATION: return vec(fx_.point_atten, 3);
    case GL_POINT_SPRITE_COORD_ORIGIN: return one(fx_.point_origin);
    case GL_LINE_WIDTH: return one(fx_.line_width);
    case GL_LINE_STIPPLE_PATTERN: return one(fx_.line_stipple_pattern);
    case GL_LINE_STIPPLE_REPEAT: return one(fx_.line_stipple_factor);
    case GL_POLYGON_MODE: out[0] = fx_.polygon_front; out[1] = fx_.polygon_back; *count = 2; return true;
    case GL_POLYGON_OFFSET_FACTOR: return one(gl_.po_factor_);
    case GL_POLYGON_OFFSET_UNITS: return one(gl_.po_units_);
    case GL_CULL_FACE_MODE: return one(gl_.cull_mode_);
    case GL_FRONT_FACE: return one(gl_.front_face_);
    case GL_ALPHA_TEST_FUNC: return one(fx_.alpha_func);
    case GL_ALPHA_TEST_REF: *kind = 1; return one(fx_.alpha_ref);
    case GL_LOGIC_OP_MODE: return one(fx_.logic_mode);
    case GL_INDEX_CLEAR_VALUE: return one(fx_.clear_index);
    case GL_INDEX_WRITEMASK: return one(fx_.index_mask);
    case GL_COLOR_CLEAR_VALUE: return vec(gl_.clear_color_, 4, true);
    case GL_DEPTH_CLEAR_VALUE: *kind = 1; return one(gl_.clear_depth_);
    case GL_STENCIL_CLEAR_VALUE: return one(gl_.clear_stencil_);
    case GL_ACCUM_CLEAR_VALUE: return vec(fx_.accum_clear, 4, true);
    case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; ++i) out[i] = gl_.cmask_[i]; *count = 4; return true;
    case GL_DEPTH_WRITEMASK: return one(gl_.dmask_);
    case GL_DEPTH_FUNC: return one(gl_.dfunc_);
    case GL_DEPTH_RANGE: out[0] = depth_range_values_[0]; out[1] = depth_range_values_[1]; *count = 2; *kind = 1; return true;
    case GL_VIEWPORT: for (int i = 0; i < 4; ++i) out[i] = gl_.vp_[i]; *count = 4; return true;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; ++i) out[i] = gl_.sc_[i]; *count = 4; return true;
    case GL_DRAW_BUFFER: case GL_DRAW_BUFFER0:
      if (gl_.draw_fb_ == 0) return one(default_draw_buffer_);
      break;
    case GL_READ_BUFFER:
      if (gl_.read_fb_ == 0) return one(default_read_buffer_);
      break;
    case GL_ACTIVE_TEXTURE: return one(GL_TEXTURE0 + gl_.active_unit_);
    case GL_CLIENT_ACTIVE_TEXTURE: return one(GL_TEXTURE0 + client_active_unit_);
    case GL_TEXTURE_BINDING_1D: return one(texture_name(eu.tex1d, 0));
    case GL_TEXTURE_BINDING_2D: return one(texture_name(eu.tex2d, 1));
    case GL_TEXTURE_BINDING_3D: return one(texture_name(eu.tex3d, 2));
    case GL_TEXTURE_BINDING_CUBE_MAP: return one(texture_name(eu.cube, 3));
    case GL_TEXTURE_BINDING_RECTANGLE: return one(texture_name(eu.rect, 4));
    case GL_TEXTURE_BINDING_2D_ARRAY: return one(texture_name(eu.array, 5));
    case GL_TEXTURE_BINDING_1D_ARRAY: return one(texture_name(eu.array1d, 6));
    case GL_SAMPLER_BINDING: return one(group_->samplers.name_of(eu.sampler));
    case GL_CURRENT_PROGRAM: return one(user_program_ ? group_->shaders_and_programs.name_of(user_program_ | kProgramObjectBit) : 0);
    case GL_VERTEX_ARRAY_BINDING: return one(vao_names_.name_of(gl_.vao_id_));
    case GL_TRANSFORM_FEEDBACK_BINDING: return one(tf_names_.name_of(gl_.tf_bound_));
    case GL_MAX_VERTEX_STREAMS: return one(4);
    case GL_LAYER_PROVOKING_VERTEX: return one(GL_FIRST_VERTEX_CONVENTION);      // (the layer of a primitive is its first vertex's gl_Layer)
    case GL_MAX_TRANSFORM_FEEDBACK_BUFFERS: return one(4);
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.tf_buffer_));
    case GL_CLAMP_VERTEX_COLOR: return one(gl_.desktop.clamp_vertex_color ? GL_TRUE : GL_FALSE);
    case GL_ARRAY_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.array_buffer_));
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: return one(group_->buffers.name_of(vao->element_buffer));
    case GL_PIXEL_PACK_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.pixel_pack_));
    case GL_PIXEL_UNPACK_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.pixel_unpack_));
    case GL_UNIFORM_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.uniform_buffer_));
    case GL_COPY_READ_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.copy_read_));
    case GL_COPY_WRITE_BUFFER_BINDING: return one(group_->buffers.name_of(gl_.copy_write_));
    case GL_FRAMEBUFFER_BINDING: return one(fbo_names_.name_of(gl_.draw_fb_));
    case GL_READ_FRAMEBUFFER_BINDING: return one(fbo_names_.name_of(gl_.read_fb_));
    case GL_RENDERBUFFER_BINDING: return one(group_->renderbuffers.name_of(gl_.rb_bound_));
    case GL_VERTEX_ARRAY_SIZE: return one(array_info(kAttrVertex, 0));
    case GL_VERTEX_ARRAY_TYPE: return one(array_info(kAttrVertex, 1));
    case GL_VERTEX_ARRAY_STRIDE: return one(array_info(kAttrVertex, 2));
    case GL_VERTEX_ARRAY_BUFFER_BINDING: return one(array_info(kAttrVertex, 3));
    case GL_NORMAL_ARRAY_TYPE: return one(array_info(kAttrNormal, 1));
    case GL_NORMAL_ARRAY_STRIDE: return one(array_info(kAttrNormal, 2));
    case GL_NORMAL_ARRAY_BUFFER_BINDING: return one(array_info(kAttrNormal, 3));
    case GL_COLOR_ARRAY_SIZE: return one(array_info(kAttrColor, 0));
    case GL_COLOR_ARRAY_TYPE: return one(array_info(kAttrColor, 1));
    case GL_COLOR_ARRAY_STRIDE: return one(array_info(kAttrColor, 2));
    case GL_COLOR_ARRAY_BUFFER_BINDING: return one(array_info(kAttrColor, 3));
    case GL_SECONDARY_COLOR_ARRAY_SIZE: return one(array_info(kAttrSecondary, 0));
    case GL_SECONDARY_COLOR_ARRAY_TYPE: return one(array_info(kAttrSecondary, 1));
    case GL_SECONDARY_COLOR_ARRAY_STRIDE: return one(array_info(kAttrSecondary, 2));
    case GL_FOG_COORD_ARRAY_TYPE: return one(array_info(kAttrFog, 1));
    case GL_FOG_COORD_ARRAY_STRIDE: return one(array_info(kAttrFog, 2));
    case GL_EDGE_FLAG_ARRAY_STRIDE: return one(array_info(kAttrEdge, 2));
    case GL_INDEX_ARRAY_TYPE: return one(array_info(kAttrIndex, 1));
    case GL_INDEX_ARRAY_STRIDE: return one(array_info(kAttrIndex, 2));
    case GL_TEXTURE_COORD_ARRAY_SIZE: return one(array_info(kAttrTex0 + client_unit, 0));
    case GL_TEXTURE_COORD_ARRAY_TYPE: return one(array_info(kAttrTex0 + client_unit, 1));
    case GL_TEXTURE_COORD_ARRAY_STRIDE: return one(array_info(kAttrTex0 + client_unit, 2));
    case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: return one(array_info(kAttrTex0 + client_unit, 3));
    // Pixel store and transfer
    case GL_UNPACK_SWAP_BYTES: return one(unpack_.swap_bytes);
    case GL_UNPACK_LSB_FIRST: return one(unpack_.lsb_first);
    case GL_UNPACK_ROW_LENGTH: return one(unpack_.row_length);
    case GL_UNPACK_IMAGE_HEIGHT: return one(unpack_.image_height);
    case GL_UNPACK_SKIP_PIXELS: return one(unpack_.skip_pixels);
    case GL_UNPACK_SKIP_ROWS: return one(unpack_.skip_rows);
    case GL_UNPACK_SKIP_IMAGES: return one(unpack_.skip_images);
    case GL_UNPACK_ALIGNMENT: return one(unpack_.alignment);
    case GL_PACK_SWAP_BYTES: return one(pack_.swap_bytes);
    case GL_PACK_LSB_FIRST: return one(pack_.lsb_first);
    case GL_PACK_ROW_LENGTH: return one(pack_.row_length);
    case GL_PACK_IMAGE_HEIGHT: return one(pack_.image_height);
    case GL_PACK_SKIP_PIXELS: return one(pack_.skip_pixels);
    case GL_PACK_SKIP_ROWS: return one(pack_.skip_rows);
    case GL_PACK_SKIP_IMAGES: return one(pack_.skip_images);
    case GL_PACK_ALIGNMENT: return one(pack_.alignment);
    case GL_ZOOM_X: return one(fx_.pixel_zoom[0]);
    case GL_ZOOM_Y: return one(fx_.pixel_zoom[1]);
    case GL_RED_SCALE: return one(fx_.pixel_scale[0]);
    case GL_GREEN_SCALE: return one(fx_.pixel_scale[1]);
    case GL_BLUE_SCALE: return one(fx_.pixel_scale[2]);
    case GL_ALPHA_SCALE: return one(fx_.pixel_scale[3]);
    case GL_DEPTH_SCALE: return one(fx_.pixel_scale[4]);
    case GL_RED_BIAS: return one(fx_.pixel_bias[0]);
    case GL_GREEN_BIAS: return one(fx_.pixel_bias[1]);
    case GL_BLUE_BIAS: return one(fx_.pixel_bias[2]);
    case GL_ALPHA_BIAS: return one(fx_.pixel_bias[3]);
    case GL_DEPTH_BIAS: return one(fx_.pixel_bias[4]);
    case GL_MAP_COLOR: return one(fx_.map_color);
    case GL_MAP_STENCIL: return one(fx_.map_stencil);
    case GL_INDEX_SHIFT: return one(fx_.index_shift);
    case GL_INDEX_OFFSET: return one(fx_.index_offset);
    case GL_PIXEL_MAP_I_TO_I_SIZE: case GL_PIXEL_MAP_S_TO_S_SIZE: case GL_PIXEL_MAP_I_TO_R_SIZE: case GL_PIXEL_MAP_I_TO_G_SIZE: case GL_PIXEL_MAP_I_TO_B_SIZE: case GL_PIXEL_MAP_I_TO_A_SIZE: case GL_PIXEL_MAP_R_TO_R_SIZE: case GL_PIXEL_MAP_G_TO_G_SIZE: case GL_PIXEL_MAP_B_TO_B_SIZE: case GL_PIXEL_MAP_A_TO_A_SIZE:
      return one(pixel_maps_[pname - GL_PIXEL_MAP_I_TO_I_SIZE].size);
    case GL_PERSPECTIVE_CORRECTION_HINT: return one(fx_.hints[0]);
    case GL_POINT_SMOOTH_HINT: return one(fx_.hints[1]);
    case GL_LINE_SMOOTH_HINT: return one(fx_.hints[2]);
    case GL_POLYGON_SMOOTH_HINT: return one(fx_.hints[3]);
    case GL_FOG_HINT: return one(fx_.hints[4]);
    case GL_GENERATE_MIPMAP_HINT: return one(fx_.hints[5]);
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: return one(fx_.hints[7]);
    case GL_LIST_BASE: return one(list_base_);
    case GL_LIST_INDEX: return one(list_compiling_);
    case GL_LIST_MODE: return one(list_building_ ? (list_execute_too_ ? GL_COMPILE_AND_EXECUTE : GL_COMPILE) : 0);
    case GL_RENDER_MODE: return one(render_mode_);
    case GL_NAME_STACK_DEPTH: return one(name_stack_depth());
    case GL_FEEDBACK_BUFFER_SIZE: case GL_SELECTION_BUFFER_SIZE: return one(feedback_capacity());
    case GL_FEEDBACK_BUFFER_TYPE: return one(feedback_type());
    case GL_MAP1_GRID_DOMAIN: for (int i = 0; i < 2; ++i) out[i] = fx_.grid1_u[i]; *count = 2; return true;
    case GL_MAP2_GRID_DOMAIN: out[0] = fx_.grid2_u[0]; out[1] = fx_.grid2_u[1]; out[2] = fx_.grid2_v[0]; out[3] = fx_.grid2_v[1]; *count = 4; return true;
    case GL_MAP1_GRID_SEGMENTS: return one(fx_.grid1_n);
    case GL_MAP2_GRID_SEGMENTS: out[0] = fx_.grid2_n; out[1] = fx_.grid2_m; *count = 2; return true;
    case GL_PRIMITIVE_RESTART_INDEX: return one(gl_.desktop.restart_index);
    case GL_PROVOKING_VERTEX: return one(gl_.desktop.first_vertex_convention ? GL_FIRST_VERTEX_CONVENTION : GL_LAST_VERTEX_CONVENTION);
    case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS: return one(0);
    case GL_TIMESTAMP: return one(0);
    default: break;
  }
  bool enabled = false;
  if (fixed_is_enabled(pname, &enabled)) { out[0] = enabled; return true; }
  if (gl_.get_integer(pname, out, count)) return true;
  error(GL_INVALID_ENUM);
  return false;
}

void GlApi::glGetBooleanv(GLenum pname, UserPtr data) {
  double v[16];
  uint32_t n = 0, kind = 0;
  if (!get_values(pname, v, &n, &kind)) return;
  uint8_t b[16];
  for (uint32_t i = 0; i < n; ++i) b[i] = v[i] != 0;
  write_values(data, b, n);
}
void GlApi::glGetIntegerv(GLenum pname, UserPtr data) {
  double v[16];
  uint32_t n = 0, kind = 0;
  if (!get_values(pname, v, &n, &kind)) return;
  int32_t out[16];
  for (uint32_t i = 0; i < n; ++i) {
    if (kind == 1) {
      const double s = v[i] * 2147483647.0;
      out[i] = static_cast<int32_t>(s < -2147483648.0 ? -2147483647 - 1 : s > 2147483647.0 ? 2147483647 : s);
    } else {
      const double r = v[i] < 0 ? v[i] - 0.5 : v[i] + 0.5;
      out[i] = static_cast<int32_t>(static_cast<int64_t>(r));
    }
  }
  write_values(data, out, n);
}
void GlApi::glGetInteger64v(GLenum pname, UserPtr data) {
  double v[16];
  uint32_t n = 0, kind = 0;
  if (!get_values(pname, v, &n, &kind)) return;
  int64_t out[16];
  for (uint32_t i = 0; i < n; ++i) out[i] = static_cast<int64_t>(v[i]);
  write_values(data, out, n);
}
void GlApi::glGetFloatv(GLenum pname, UserPtr data) {
  double v[16];
  uint32_t n = 0, kind = 0;
  if (!get_values(pname, v, &n, &kind)) return;
  float out[16];
  for (uint32_t i = 0; i < n; ++i) out[i] = static_cast<float>(v[i]);
  write_values(data, out, n);
}
void GlApi::glGetDoublev(GLenum pname, UserPtr data) {
  double v[16];
  uint32_t n = 0, kind = 0;
  if (!get_values(pname, v, &n, &kind)) return;
  write_values(data, v, n);
}
void GlApi::glGetIntegeri_v(GLenum target, GLuint index, UserPtr data) {
  int32_t out = 0;
  switch (target) {
    case GL_UNIFORM_BUFFER_BINDING: if (index >= webgl::kMaxUniformBindings) { error(GL_INVALID_VALUE); return; } out = static_cast<int32_t>(group_->buffers.name_of(gl_.ubo_bindings_[index].buffer)); break;
    case GL_UNIFORM_BUFFER_START: if (index >= webgl::kMaxUniformBindings) { error(GL_INVALID_VALUE); return; } out = static_cast<int32_t>(gl_.ubo_bindings_[index].offset); break;
    case GL_UNIFORM_BUFFER_SIZE: if (index >= webgl::kMaxUniformBindings) { error(GL_INVALID_VALUE); return; } out = static_cast<int32_t>(gl_.ubo_bindings_[index].size); break;
    default: error(GL_INVALID_ENUM); return;
  }
  write_values(data, &out, 1);
}
void GlApi::glGetBooleani_v(GLenum target, GLuint index, UserPtr data) {
  if (target == GL_COLOR_WRITEMASK && index < webgl::kMaxDrawBuffers) {
    const uint8_t m = gl_.desktop.color_mask[index];
    const uint8_t out[4] = {static_cast<uint8_t>(m & 1), static_cast<uint8_t>((m >> 1) & 1), static_cast<uint8_t>((m >> 2) & 1), static_cast<uint8_t>((m >> 3) & 1)};
    write_values(data, out, 4);
    return;
  }
  error(GL_INVALID_ENUM);
}
void GlApi::glGetPointerv(GLenum pname, UserPtr params) {
  const webgl::Vao* vao = gl_.current_vao_ptr();
  const auto pointer = [&](uint32_t attr) { const webgl::VertexAttrib& at = vao->attribs[attr]; return at.from_client ? at.client : at.offset; };
  uint64_t v = 0;
  switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: v = pointer(kAttrVertex); break;
    case GL_NORMAL_ARRAY_POINTER: v = pointer(kAttrNormal); break;
    case GL_COLOR_ARRAY_POINTER: v = pointer(kAttrColor); break;
    case GL_SECONDARY_COLOR_ARRAY_POINTER: v = pointer(kAttrSecondary); break;
    case GL_FOG_COORD_ARRAY_POINTER: v = pointer(kAttrFog); break;
    case GL_INDEX_ARRAY_POINTER: v = pointer(kAttrIndex); break;
    case GL_EDGE_FLAG_ARRAY_POINTER: v = pointer(kAttrEdge); break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: v = pointer(kAttrTex0 + (client_active_unit_ < kMaxTexUnits ? client_active_unit_ : 0)); break;
    case GL_FEEDBACK_BUFFER_POINTER: case GL_SELECTION_BUFFER_POINTER: v = feedback_address(); break;
    default: error(GL_INVALID_ENUM); return;
  }
  write_values(params, &v, 1);
}

// --- Fixed-function state read back ---------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGetLightfv(GLenum light, GLenum pname, UserPtr params) {
  if (light < GL_LIGHT0 || light >= GL_LIGHT0 + kMaxLights) { error(GL_INVALID_ENUM); return; }
  const Light& l = fx_.lights[light - GL_LIGHT0];
  switch (pname) {
    case GL_AMBIENT: write_values(params, l.ambient, 4); break;
    case GL_DIFFUSE: write_values(params, l.diffuse, 4); break;
    case GL_SPECULAR: write_values(params, l.specular, 4); break;
    case GL_POSITION: write_values(params, l.position, 4); break;
    case GL_SPOT_DIRECTION: write_values(params, l.spot_direction, 3); break;
    case GL_SPOT_EXPONENT: write_values(params, &l.spot_exponent, 1); break;
    case GL_SPOT_CUTOFF: write_values(params, &l.spot_cutoff, 1); break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION: write_values(params, &l.attenuation[pname - GL_CONSTANT_ATTENUATION], 1); break;
    default: error(GL_INVALID_ENUM); break;
  }
}
void GlApi::glGetLightiv(GLenum light, GLenum pname, UserPtr params) {
  float f[4];
  const uint32_t n = light_value_count(pname);
  if (light < GL_LIGHT0 || light >= GL_LIGHT0 + kMaxLights) { error(GL_INVALID_ENUM); return; }
  const Light& l = fx_.lights[light - GL_LIGHT0];
  const float* src = nullptr;
  switch (pname) {
    case GL_AMBIENT: src = l.ambient; break;
    case GL_DIFFUSE: src = l.diffuse; break;
    case GL_SPECULAR: src = l.specular; break;
    case GL_POSITION: src = l.position; break;
    case GL_SPOT_DIRECTION: src = l.spot_direction; break;
    case GL_SPOT_EXPONENT: src = &l.spot_exponent; break;
    case GL_SPOT_CUTOFF: src = &l.spot_cutoff; break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION: src = &l.attenuation[pname - GL_CONSTANT_ATTENUATION]; break;
    default: error(GL_INVALID_ENUM); return;
  }
  for (uint32_t i = 0; i < n; ++i) f[i] = src[i];
  int32_t out[4];
  const bool colour = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
  for (uint32_t i = 0; i < n; ++i) out[i] = colour ? static_cast<int32_t>(static_cast<double>(f[i]) * 2147483647.0) : static_cast<int32_t>(f[i]);
  write_values(params, out, n);
}
const float* GlApi::material_field(uint32_t face, uint32_t pname, uint32_t* n) const {
  if (face != GL_FRONT && face != GL_BACK) return nullptr;
  const Material& m = fx_.material[face == GL_BACK ? 1 : 0];
  *n = 4;
  switch (pname) {
    case GL_AMBIENT: return m.ambient;
    case GL_DIFFUSE: return m.diffuse;
    case GL_SPECULAR: return m.specular;
    case GL_EMISSION: return m.emission;
    case GL_SHININESS: *n = 1; return &m.shininess;
    case GL_COLOR_INDEXES: *n = 3; return m.color_index;
    default: return nullptr;
  }
}
void GlApi::glGetMaterialfv(GLenum face, GLenum pname, UserPtr params) {
  uint32_t n = 0;
  const float* f = material_field(face, pname, &n);
  if (!f) { error(GL_INVALID_ENUM); return; }
  write_values(params, f, n);
}
void GlApi::glGetMaterialiv(GLenum face, GLenum pname, UserPtr params) {
  uint32_t n = 0;
  const float* f = material_field(face, pname, &n);
  if (!f) { error(GL_INVALID_ENUM); return; }
  int32_t out[4];
  const bool plain = pname == GL_SHININESS || pname == GL_COLOR_INDEXES;
  for (uint32_t i = 0; i < n; ++i) out[i] = plain ? static_cast<int32_t>(f[i]) : static_cast<int32_t>(static_cast<double>(f[i]) * 2147483647.0);
  write_values(params, out, n);
}
bool GlApi::texenv_get(uint32_t target, uint32_t pname, double* out, uint32_t* n) {
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  const TexEnv& e = fx_.units[unit].env;
  *n = 1;
  if (target == GL_TEXTURE_FILTER_CONTROL) {
    if (pname != GL_TEXTURE_LOD_BIAS) { error(GL_INVALID_ENUM); return false; }
    *out = e.lod_bias;
    return true;
  }
  if (target == GL_POINT_SPRITE) {
    if (pname != GL_COORD_REPLACE) { error(GL_INVALID_ENUM); return false; }
    *out = e.coord_replace;
    return true;
  }
  if (target != GL_TEXTURE_ENV) { error(GL_INVALID_ENUM); return false; }
  switch (pname) {
    case GL_TEXTURE_ENV_MODE: *out = e.mode; return true;
    case GL_TEXTURE_ENV_COLOR: for (int i = 0; i < 4; ++i) out[i] = e.color[i]; *n = 4; return true;
    case GL_COMBINE_RGB: *out = e.combine_rgb; return true;
    case GL_COMBINE_ALPHA: *out = e.combine_alpha; return true;
    case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB: *out = e.source_rgb[pname - GL_SRC0_RGB]; return true;
    case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA: *out = e.source_alpha[pname - GL_SRC0_ALPHA]; return true;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: *out = e.operand_rgb[pname - GL_OPERAND0_RGB]; return true;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA: *out = e.operand_alpha[pname - GL_OPERAND0_ALPHA]; return true;
    case GL_RGB_SCALE: *out = e.scale_rgb; return true;
    case GL_ALPHA_SCALE: *out = e.scale_alpha; return true;
    default: error(GL_INVALID_ENUM); return false;
  }
}
void GlApi::glGetTexEnvfv(GLenum target, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!texenv_get(target, pname, v, &n)) return;
  float f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<float>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetTexEnviv(GLenum target, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!texenv_get(target, pname, v, &n)) return;
  int32_t f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = pname == GL_TEXTURE_ENV_COLOR ? static_cast<int32_t>(v[i] * 2147483647.0) : static_cast<int32_t>(v[i]);
  write_values(params, f, n);
}
bool GlApi::texgen_get(uint32_t coord, uint32_t pname, double* out, uint32_t* n) {
  if (coord < GL_S || coord > GL_Q) { error(GL_INVALID_ENUM); return false; }
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  const TexGen& g = fx_.units[unit].gen[coord - GL_S];
  *n = 4;
  switch (pname) {
    case GL_TEXTURE_GEN_MODE: *out = g.mode; *n = 1; return true;
    case GL_OBJECT_PLANE: for (int i = 0; i < 4; ++i) out[i] = g.object_plane[i]; return true;
    case GL_EYE_PLANE: for (int i = 0; i < 4; ++i) out[i] = g.eye_plane[i]; return true;
    default: error(GL_INVALID_ENUM); return false;
  }
}
void GlApi::glGetTexGendv(GLenum coord, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!texgen_get(coord, pname, v, &n)) return;
  write_values(params, v, n);
}
void GlApi::glGetTexGenfv(GLenum coord, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!texgen_get(coord, pname, v, &n)) return;
  float f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<float>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetTexGeniv(GLenum coord, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!texgen_get(coord, pname, v, &n)) return;
  int32_t f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<int32_t>(v[i]);
  write_values(params, f, n);
}

}  // namespace opengl
