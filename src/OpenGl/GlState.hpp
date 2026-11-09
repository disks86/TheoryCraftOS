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

// What a context remembers beyond the engine (webgl::GlContext): the fixed-function state of OpenGL's compatibility profile. Everything here is plain data so an attribute stack
// (glPushAttrib) is a copy of a part of it.
#pragma once
#include <stdint.h>

namespace opengl {

using GLenum = uint32_t;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLbyte = int8_t;
using GLshort = int16_t;
using GLint = int32_t;
using GLclampx = int32_t;
using GLubyte = uint8_t;
using GLushort = uint16_t;
using GLuint = uint32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLclampd = double;
using GLchar = char;
using GLcharARB = char;
using GLhalf = uint16_t;
using GLhalfARB = uint16_t;
using GLhalfNV = uint16_t;
using GLfixed = int32_t;
using GLhandleARB = uint32_t;
using GLint64 = int64_t;
using GLint64EXT = int64_t;
using GLuint64 = uint64_t;
using GLuint64EXT = uint64_t;
using GLintptr = int64_t;
using GLintptrARB = int64_t;
using GLsizeiptr = int64_t;
using GLsizeiptrARB = int64_t;
using GLsync = uint64_t;
using GLeglImageOES = uint64_t;
using GLeglClientBufferEXT = uint64_t;
using GLvdpauSurfaceNV = int32_t;

constexpr uint32_t kMaxLights = 8;
constexpr uint32_t kMaxTexUnits = 8;       // fixed-function texture units (coordinates and environments)
constexpr uint32_t kMaxClipPlanes = 8;
constexpr uint32_t kModelviewDepth = 32, kProjectionDepth = 4, kTextureDepth = 10, kColorDepth = 10;
constexpr uint32_t kMaxAttribStack = 16;
constexpr uint32_t kMaxClientAttribStack = 16;

// Where the fixed-function arrays sit among the engine's attributes. Generic attribute 0 and the vertex position are one, as the compatibility profile allows.
constexpr uint32_t kAttrVertex = 0, kAttrNormal = 16, kAttrColor = 17, kAttrSecondary = 18, kAttrFog = 19, kAttrTex0 = 20, kAttrEdge = 28, kAttrIndex = 29;

struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Mat4 mat_multiply(const Mat4& a, const Mat4& b);       // a * b
bool mat_inverse(const Mat4& a, Mat4* out);
Mat4 mat_transpose(const Mat4& a);
void mat_transform(const Mat4& a, const float v[4], float out[4]);

struct MatrixStack {
  Mat4 entries[kModelviewDepth];
  uint32_t top = 0, depth = kModelviewDepth;
};

struct Light {
  float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {0, 0, 0, 1}, specular[4] = {0, 0, 0, 1};
  float position[4] = {0, 0, 1, 0};      // in eye coordinates
  float spot_direction[3] = {0, 0, -1};
  float spot_exponent = 0, spot_cutoff = 180;
  float attenuation[3] = {1, 0, 0};
};

struct Material {
  float ambient[4] = {0.2f, 0.2f, 0.2f, 1}, diffuse[4] = {0.8f, 0.8f, 0.8f, 1}, specular[4] = {0, 0, 0, 1}, emission[4] = {0, 0, 0, 1};
  float shininess = 0;
  float color_index[3] = {0, 1, 1};
};

struct TexGen {
  uint32_t mode = 0x2400;      // GL_EYE_LINEAR
  float object_plane[4] = {0, 0, 0, 0}, eye_plane[4] = {0, 0, 0, 0};
};

struct TexEnv {
  uint32_t mode = 0x2100;      // GL_MODULATE
  float color[4] = {0, 0, 0, 0};
  float lod_bias = 0;
  uint32_t combine_rgb = 0x2100, combine_alpha = 0x2100;
  uint32_t source_rgb[3] = {0x1702, 0x8576, 0x8578}, source_alpha[3] = {0x1702, 0x8576, 0x8578};      // TEXTURE, PREVIOUS, CONSTANT
  uint32_t operand_rgb[3] = {0x0300, 0x0300, 0x0302}, operand_alpha[3] = {0x0302, 0x0302, 0x0302};      // SRC_COLOR, SRC_COLOR, SRC_ALPHA / SRC_ALPHA ...
  float scale_rgb = 1, scale_alpha = 1;
  bool coord_replace = false;
};

// The fixed-function state, by attribute group where glPushAttrib cares.
struct FixedState {
  // GL_CURRENT_BIT
  float current_color[4] = {1, 1, 1, 1}, current_secondary[4] = {0, 0, 0, 1}, current_normal[3] = {0, 0, 1}, current_tex[kMaxTexUnits][4] = {};
  float current_fog = 0, current_index = 1;
  bool current_edge = true;
  float raster_pos[4] = {0, 0, 0, 1}, raster_color[4] = {1, 1, 1, 1}, raster_secondary[4] = {0, 0, 0, 1}, raster_tex[kMaxTexUnits][4] = {};
  float raster_distance = 0, raster_index = 1;
  bool raster_valid = true;
  // GL_LIGHTING_BIT
  bool lighting = false;
  uint32_t lights_enabled = 0;
  Light lights[kMaxLights];
  Material material[2];
  float model_ambient[4] = {0.2f, 0.2f, 0.2f, 1};
  bool local_viewer = false, two_side = false, separate_specular = false;
  bool color_material = false;
  uint32_t color_material_face = 0x0408, color_material_mode = 0x1602;      // FRONT_AND_BACK, AMBIENT_AND_DIFFUSE
  uint32_t shade_model = 0x1D01;      // GL_SMOOTH
  // GL_TRANSFORM_BIT
  uint32_t matrix_mode = 0x1700;
  bool normalize = false, rescale_normal = false;
  uint32_t clip_enabled = 0;
  double clip_plane[kMaxClipPlanes][4] = {};
  // GL_FOG_BIT
  bool fog = false;
  uint32_t fog_mode = 0x0800, fog_source = 0x8452;      // GL_EXP, GL_FRAGMENT_DEPTH
  float fog_color[4] = {0, 0, 0, 0}, fog_density = 1, fog_start = 0, fog_end = 1, fog_index = 0;
  // GL_POINT_BIT, GL_LINE_BIT, GL_POLYGON_BIT, GL_POLYGON_STIPPLE_BIT
  float point_size = 1, point_min = 0, point_max = 2047, point_fade = 1, point_atten[3] = {1, 0, 0};
  bool point_smooth = false, point_sprite = false, program_point_size = false;
  uint32_t point_origin = 0x8CA1;      // GL_UPPER_LEFT
  bool line_smooth = false, line_stipple = false;
  int32_t line_stipple_factor = 1;
  uint16_t line_stipple_pattern = 0xFFFF;
  float line_width = 1;
  uint32_t polygon_front = 0x1B02, polygon_back = 0x1B02;
  bool polygon_smooth = false, polygon_stipple = false, offset_point = false, offset_line = false;
  uint8_t polygon_pattern[128];
  // GL_ENABLE_BIT only: the texture coordinate units' enables live in units below.
  bool auto_normal = false, dither = true, vertex_program_two_side = false, color_sum = false;
  bool map1[9] = {}, map2[9] = {};
  // GL_TEXTURE_BIT
  struct Unit {
    uint32_t enabled = 0;      // bit 0: 1D, 1: 2D, 2: 3D, 3: cube, 4: rectangle
    uint32_t texgen_enabled = 0;      // bits S, T, R, Q
    TexGen gen[4];
    TexEnv env;
  } units[kMaxTexUnits];
  // GL_COLOR_BUFFER_BIT
  bool alpha_test = false;
  uint32_t alpha_func = 0x0207;
  float alpha_ref = 0;
  bool logic_op = false, index_logic_op = false;
  uint32_t logic_mode = 0x1503;
  float clear_index = 0;
  uint32_t index_mask = 0xFFFFFFFFu;
  bool clamp_read_color = true;
  // GL_ACCUM_BUFFER_BIT
  float accum_clear[4] = {0, 0, 0, 0};
  // GL_EVAL_BIT
  int32_t grid1_n = 1, grid2_n = 1, grid2_m = 1;
  float grid1_u[2] = {0, 1}, grid2_u[2] = {0, 1}, grid2_v[2] = {0, 1};
  // GL_HINT_BIT
  uint32_t hints[16];
  // GL_PIXEL_MODE_BIT
  float pixel_zoom[2] = {1, 1};
  float pixel_scale[5] = {1, 1, 1, 1, 1}, pixel_bias[5] = {0, 0, 0, 0, 0};      // r g b a depth
  bool map_color = false, map_stencil = false;
  int32_t index_shift = 0, index_offset = 0;
  uint32_t read_buffer_mode = 0;
};

}  // namespace opengl
