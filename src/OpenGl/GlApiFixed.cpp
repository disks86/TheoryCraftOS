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

// Fixed-function state commands: matrices, lights, materials, fog, texture environments and coordinate generation, points, lines and polygons, enables, simple per-fragment state.
#include "OpenGl/FixedFunction.hpp"
#include "OpenGl/GlApi.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

Mat4 mat_multiply(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  }
  return r;
}

Mat4 mat_transpose(const Mat4& a) {
  Mat4 r;
  for (int c = 0; c < 4; ++c) for (int row = 0; row < 4; ++row) r.m[c * 4 + row] = a.m[row * 4 + c];
  return r;
}

void mat_transform(const Mat4& a, const float v[4], float out[4]) {
  for (int row = 0; row < 4; ++row) out[row] = a.m[row] * v[0] + a.m[4 + row] * v[1] + a.m[8 + row] * v[2] + a.m[12 + row] * v[3];
}

bool mat_inverse(const Mat4& in, Mat4* out) {
  // Gauss-Jordan with partial pivoting.
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = in.m[c * 4 + r];
    for (int c = 0; c < 4; ++c) a[r][c + 4] = r == c ? 1 : 0;
  }
  for (int i = 0; i < 4; ++i) {
    int pivot = i;
    double best = a[i][i] < 0 ? -a[i][i] : a[i][i];
    for (int r = i + 1; r < 4; ++r) {
      const double v = a[r][i] < 0 ? -a[r][i] : a[r][i];
      if (v > best) { best = v; pivot = r; }
    }
    if (best == 0) { *out = Mat4(); return false; }
    if (pivot != i) for (int c = 0; c < 8; ++c) { const double t = a[i][c]; a[i][c] = a[pivot][c]; a[pivot][c] = t; }
    const double scale = 1.0 / a[i][i];
    for (int c = 0; c < 8; ++c) a[i][c] *= scale;
    for (int r = 0; r < 4; ++r) {
      if (r == i) continue;
      const double f = a[r][i];
      if (f == 0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c];
    }
  }
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out->m[c * 4 + r] = static_cast<float>(a[r][c + 4]);
  return true;
}

// --- Matrices ---------------------------------------------------------------------------------------------------------------------------------------------

MatrixStack& GlApi::current_stack() {
  switch (fx_.matrix_mode) {
    case GL_PROJECTION: return projection_;
    case GL_TEXTURE: return texture_stack_[active_unit() < kMaxTexUnits ? active_unit() : 0];
    case GL_COLOR: return color_stack_;
    default: return modelview_;
  }
}

void GlApi::multiply_current(const Mat4& by) {
  MatrixStack& s = current_stack();
  s.entries[s.top] = mat_multiply(s.entries[s.top], by);
  matrix_changed();
}

void GlApi::glMatrixMode(GLenum mode) {
  if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE && mode != GL_COLOR) { error(GL_INVALID_ENUM); return; }
  fx_.matrix_mode = mode;
}

void GlApi::glLoadIdentity() {
  MatrixStack& s = current_stack();
  s.entries[s.top] = Mat4();
  matrix_changed();
}

void GlApi::glLoadMatrixf(ArrayView<const GLfloat> m) {
  MatrixStack& s = current_stack();
  for (int i = 0; i < 16; ++i) s.entries[s.top].m[i] = m[static_cast<uint64_t>(i)];
  matrix_changed();
}

void GlApi::glLoadMatrixd(ArrayView<const GLdouble> m) {
  MatrixStack& s = current_stack();
  for (int i = 0; i < 16; ++i) s.entries[s.top].m[i] = static_cast<float>(m[static_cast<uint64_t>(i)]);
  matrix_changed();
}

void GlApi::glLoadTransposeMatrixf(ArrayView<const GLfloat> m) {
  Mat4 t;
  for (int i = 0; i < 16; ++i) t.m[i] = m[static_cast<uint64_t>(i)];
  MatrixStack& s = current_stack();
  s.entries[s.top] = mat_transpose(t);
  matrix_changed();
}

void GlApi::glLoadTransposeMatrixd(ArrayView<const GLdouble> m) {
  Mat4 t;
  for (int i = 0; i < 16; ++i) t.m[i] = static_cast<float>(m[static_cast<uint64_t>(i)]);
  MatrixStack& s = current_stack();
  s.entries[s.top] = mat_transpose(t);
  matrix_changed();
}

void GlApi::glMultMatrixf(ArrayView<const GLfloat> m) {
  Mat4 t;
  for (int i = 0; i < 16; ++i) t.m[i] = m[static_cast<uint64_t>(i)];
  multiply_current(t);
}

void GlApi::glMultMatrixd(ArrayView<const GLdouble> m) {
  Mat4 t;
  for (int i = 0; i < 16; ++i) t.m[i] = static_cast<float>(m[static_cast<uint64_t>(i)]);
  multiply_current(t);
}

void GlApi::glMultTransposeMatrixf(ArrayView<const GLfloat> m) {
  Mat4 t;
  for (int i = 0; i < 16; ++i) t.m[i] = m[static_cast<uint64_t>(i)];
  multiply_current(mat_transpose(t));
}

void GlApi::glMultTransposeMatrixd(ArrayView<const GLdouble> m) {
  Mat4 t;
  for (int i = 0; i < 16; ++i) t.m[i] = static_cast<float>(m[static_cast<uint64_t>(i)]);
  multiply_current(mat_transpose(t));
}

void GlApi::glPushMatrix() {
  MatrixStack& s = current_stack();
  if (s.top + 1 >= s.depth) { error(GL_STACK_OVERFLOW); return; }
  s.entries[s.top + 1] = s.entries[s.top];
  ++s.top;
}

void GlApi::glPopMatrix() {
  MatrixStack& s = current_stack();
  if (s.top == 0) { error(GL_STACK_UNDERFLOW); return; }
  --s.top;
  matrix_changed();
}

void GlApi::glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)); }
void GlApi::glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  Mat4 t;
  t.m[12] = x; t.m[13] = y; t.m[14] = z;
  multiply_current(t);
}

void GlApi::glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)); }
void GlApi::glScalef(GLfloat x, GLfloat y, GLfloat z) {
  Mat4 t;
  t.m[0] = x; t.m[5] = y; t.m[10] = z;
  multiply_current(t);
}

void GlApi::glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) { glRotatef(static_cast<float>(angle), static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)); }
void GlApi::glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
  const float len = shader::fsqrt(x * x + y * y + z * z);
  if (len == 0) return;
  x /= len; y /= len; z /= len;
  const float rad = angle * 3.14159265358979f / 180.0f;
  const float c = shader::fcos(rad), s = shader::fsin(rad), n = 1 - c;
  Mat4 t;
  t.m[0] = x * x * n + c;     t.m[4] = x * y * n - z * s; t.m[8] = x * z * n + y * s;
  t.m[1] = y * x * n + z * s; t.m[5] = y * y * n + c;     t.m[9] = y * z * n - x * s;
  t.m[2] = x * z * n - y * s; t.m[6] = y * z * n + x * s; t.m[10] = z * z * n + c;
  multiply_current(t);
}

void GlApi::glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
  if (n <= 0 || f <= 0 || l == r || b == t || n == f) { error(GL_INVALID_VALUE); return; }
  Mat4 m;
  m.m[0] = static_cast<float>(2 * n / (r - l));
  m.m[5] = static_cast<float>(2 * n / (t - b));
  m.m[8] = static_cast<float>((r + l) / (r - l));
  m.m[9] = static_cast<float>((t + b) / (t - b));
  m.m[10] = static_cast<float>(-(f + n) / (f - n));
  m.m[11] = -1;
  m.m[14] = static_cast<float>(-2 * f * n / (f - n));
  m.m[15] = 0;
  multiply_current(m);
}

void GlApi::glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
  if (l == r || b == t || n == f) { error(GL_INVALID_VALUE); return; }
  Mat4 m;
  m.m[0] = static_cast<float>(2 / (r - l));
  m.m[5] = static_cast<float>(2 / (t - b));
  m.m[10] = static_cast<float>(-2 / (f - n));
  m.m[12] = static_cast<float>(-(r + l) / (r - l));
  m.m[13] = static_cast<float>(-(t + b) / (t - b));
  m.m[14] = static_cast<float>(-(f + n) / (f - n));
  multiply_current(m);
}

// --- Lights and materials ---------------------------------------------------------------------------------------------------------------------------------

uint32_t GlApi::light_value_count(uint32_t pname) {
  switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: return 4;
    case GL_SPOT_DIRECTION: return 3;
    default: return 1;
  }
}

uint32_t GlApi::model_value_count(uint32_t pname) { return pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1; }

uint32_t GlApi::material_value_count(uint32_t pname) {
  switch (pname) {
    case GL_SHININESS: return 1;
    case GL_COLOR_INDEXES: return 3;
    default: return 4;
  }
}

void GlApi::light_value(uint32_t light, uint32_t pname, const float* v, uint32_t) {
  if (light < GL_LIGHT0 || light >= GL_LIGHT0 + kMaxLights) { error(GL_INVALID_ENUM); return; }
  Light& l = fx_.lights[light - GL_LIGHT0];
  const auto copy = [&](float* dst, uint32_t n) { for (uint32_t i = 0; i < n; ++i) dst[i] = v[i]; };
  const Mat4& mv = modelview_.entries[modelview_.top];
  switch (pname) {
    case GL_AMBIENT: copy(l.ambient, 4); break;
    case GL_DIFFUSE: copy(l.diffuse, 4); break;
    case GL_SPECULAR: copy(l.specular, 4); break;
    case GL_POSITION: mat_transform(mv, v, l.position); break;
    case GL_SPOT_DIRECTION: {
      // By the upper-left 3x3 of the modelview matrix.
      const float d[4] = {v[0], v[1], v[2], 0};
      float out[4];
      mat_transform(mv, d, out);
      l.spot_direction[0] = out[0]; l.spot_direction[1] = out[1]; l.spot_direction[2] = out[2];
      const float len = shader::fsqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
      if (len > 0) { l.spot_direction[0] /= len; l.spot_direction[1] /= len; l.spot_direction[2] /= len; }
      break;
    }
    case GL_SPOT_EXPONENT:
      if (v[0] < 0 || v[0] > 128) { error(GL_INVALID_VALUE); return; }
      l.spot_exponent = v[0];
      break;
    case GL_SPOT_CUTOFF:
      if ((v[0] < 0 || v[0] > 90) && v[0] != 180) { error(GL_INVALID_VALUE); return; }
      l.spot_cutoff = v[0];
      break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
      if (v[0] < 0) { error(GL_INVALID_VALUE); return; }
      l.attenuation[pname - GL_CONSTANT_ATTENUATION] = v[0];
      break;
    default: error(GL_INVALID_ENUM); return;
  }
  ++fx_generation_;
}

void GlApi::glLightf(GLenum light, GLenum pname, GLfloat param) {
  if (light_value_count(pname) != 1) { error(GL_INVALID_ENUM); return; }
  light_value(light, pname, &param, 1);
}
void GlApi::glLighti(GLenum light, GLenum pname, GLint param) { glLightf(light, pname, static_cast<float>(param)); }
void GlApi::glLightfv(GLenum light, GLenum pname, UserPtr params) {
  float v[4];
  if (!read_values(params, v, light_value_count(pname))) { error(GL_INVALID_VALUE); return; }
  light_value(light, pname, v, 4);
}
void GlApi::glLightiv(GLenum light, GLenum pname, UserPtr params) {
  int32_t iv[4];
  const uint32_t n = light_value_count(pname);
  if (!read_values(params, iv, n)) { error(GL_INVALID_VALUE); return; }
  float v[4];
  const bool normalized = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
  for (uint32_t i = 0; i < n; ++i) v[i] = normalized ? static_cast<float>((2.0 * iv[i] + 1.0) / 4294967295.0) : static_cast<float>(iv[i]);
  light_value(light, pname, v, 4);
}

void GlApi::model_value(uint32_t pname, const float* v, uint32_t) {
  switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT: for (int i = 0; i < 4; ++i) fx_.model_ambient[i] = v[i]; break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: fx_.local_viewer = v[0] != 0; break;
    case GL_LIGHT_MODEL_TWO_SIDE: fx_.two_side = v[0] != 0; break;
    case GL_LIGHT_MODEL_COLOR_CONTROL:
      if (v[0] != static_cast<float>(GL_SINGLE_COLOR) && v[0] != static_cast<float>(GL_SEPARATE_SPECULAR_COLOR)) { error(GL_INVALID_ENUM); return; }
      fx_.separate_specular = v[0] == static_cast<float>(GL_SEPARATE_SPECULAR_COLOR);
      break;
    default: error(GL_INVALID_ENUM); return;
  }
  ++fx_generation_;
}

void GlApi::glLightModelf(GLenum pname, GLfloat param) {
  if (pname == GL_LIGHT_MODEL_AMBIENT) { error(GL_INVALID_ENUM); return; }
  model_value(pname, &param, 1);
}
void GlApi::glLightModeli(GLenum pname, GLint param) { glLightModelf(pname, static_cast<float>(param)); }
void GlApi::glLightModelfv(GLenum pname, UserPtr params) {
  float v[4];
  if (!read_values(params, v, model_value_count(pname))) { error(GL_INVALID_VALUE); return; }
  model_value(pname, v, 4);
}
void GlApi::glLightModeliv(GLenum pname, UserPtr params) {
  int32_t iv[4];
  const uint32_t n = model_value_count(pname);
  if (!read_values(params, iv, n)) { error(GL_INVALID_VALUE); return; }
  float v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = pname == GL_LIGHT_MODEL_AMBIENT ? static_cast<float>((2.0 * iv[i] + 1.0) / 4294967295.0) : static_cast<float>(iv[i]);
  model_value(pname, v, 4);
}

void GlApi::material_value(uint32_t face, uint32_t pname, const float* v, uint32_t) {
  if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) { error(GL_INVALID_ENUM); return; }
  for (uint32_t side = 0; side < 2; ++side) {
    if ((side == 0 && face == GL_BACK) || (side == 1 && face == GL_FRONT)) continue;
    Material& m = fx_.material[side];
    const auto copy = [&](float* dst, uint32_t n) { for (uint32_t i = 0; i < n; ++i) dst[i] = v[i]; };
    switch (pname) {
      case GL_AMBIENT: copy(m.ambient, 4); break;
      case GL_DIFFUSE: copy(m.diffuse, 4); break;
      case GL_SPECULAR: copy(m.specular, 4); break;
      case GL_EMISSION: copy(m.emission, 4); break;
      case GL_AMBIENT_AND_DIFFUSE: copy(m.ambient, 4); copy(m.diffuse, 4); break;
      case GL_SHININESS:
        if (v[0] < 0 || v[0] > 128) { error(GL_INVALID_VALUE); return; }
        m.shininess = v[0];
        break;
      case GL_COLOR_INDEXES: copy(m.color_index, 3); break;
      default: error(GL_INVALID_ENUM); return;
    }
  }
  ++fx_generation_;
}

void GlApi::glMaterialf(GLenum face, GLenum pname, GLfloat param) {
  if (pname != GL_SHININESS) { error(GL_INVALID_ENUM); return; }
  material_value(face, pname, &param, 1);
}
void GlApi::glMateriali(GLenum face, GLenum pname, GLint param) { glMaterialf(face, pname, static_cast<float>(param)); }
void GlApi::glMaterialfv(GLenum face, GLenum pname, UserPtr params) {
  float v[4];
  if (!read_values(params, v, material_value_count(pname))) { error(GL_INVALID_VALUE); return; }
  material_value(face, pname, v, 4);
}
void GlApi::glMaterialiv(GLenum face, GLenum pname, UserPtr params) {
  int32_t iv[4];
  const uint32_t n = material_value_count(pname);
  if (!read_values(params, iv, n)) { error(GL_INVALID_VALUE); return; }
  float v[4];
  const bool plain = pname == GL_SHININESS || pname == GL_COLOR_INDEXES;
  for (uint32_t i = 0; i < n; ++i) v[i] = plain ? static_cast<float>(iv[i]) : static_cast<float>((2.0 * iv[i] + 1.0) / 4294967295.0);
  material_value(face, pname, v, 4);
}

void GlApi::glColorMaterial(GLenum face, GLenum mode) {
  if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) { error(GL_INVALID_ENUM); return; }
  if (mode != GL_EMISSION && mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_SPECULAR && mode != GL_AMBIENT_AND_DIFFUSE) { error(GL_INVALID_ENUM); return; }
  fx_.color_material_face = face;
  fx_.color_material_mode = mode;
  if (fx_.color_material) apply_color_material(fx_.current_color);
}

// The material follows the current colour while GL_COLOR_MATERIAL is on, so that reading it back and turning it off see the right values.
void GlApi::apply_color_material(const float color[4]) {
  material_value(fx_.color_material_face, fx_.color_material_mode, color, 4);
}

void GlApi::glShadeModel(GLenum mode) {
  if (mode != GL_FLAT && mode != GL_SMOOTH) { error(GL_INVALID_ENUM); return; }
  fx_.shade_model = mode;
  gl_.desktop.flat_shade = mode == GL_FLAT;
}

// --- Fog ---------------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::fog_value(uint32_t pname, const float* v, uint32_t) {
  switch (pname) {
    case GL_FOG_MODE: {
      const uint32_t m = static_cast<uint32_t>(v[0]);
      if (m != GL_EXP && m != GL_EXP2 && m != GL_LINEAR) { error(GL_INVALID_ENUM); return; }
      fx_.fog_mode = m;
      break;
    }
    case GL_FOG_DENSITY:
      if (v[0] < 0) { error(GL_INVALID_VALUE); return; }
      fx_.fog_density = v[0];
      break;
    case GL_FOG_START: fx_.fog_start = v[0]; break;
    case GL_FOG_END: fx_.fog_end = v[0]; break;
    case GL_FOG_INDEX: fx_.fog_index = v[0]; break;
    case GL_FOG_COLOR: for (int i = 0; i < 4; ++i) fx_.fog_color[i] = v[i]; break;
    case GL_FOG_COORD_SRC: {
      const uint32_t m = static_cast<uint32_t>(v[0]);
      if (m != GL_FOG_COORD && m != GL_FRAGMENT_DEPTH) { error(GL_INVALID_ENUM); return; }
      fx_.fog_source = m;
      break;
    }
    default: error(GL_INVALID_ENUM); return;
  }
  ++fx_generation_;
}

void GlApi::glFogf(GLenum pname, GLfloat param) {
  if (pname == GL_FOG_COLOR) { error(GL_INVALID_ENUM); return; }
  fog_value(pname, &param, 1);
}
void GlApi::glFogi(GLenum pname, GLint param) { glFogf(pname, static_cast<float>(param)); }
void GlApi::glFogfv(GLenum pname, UserPtr params) {
  float v[4];
  if (!read_values(params, v, pname == GL_FOG_COLOR ? 4 : 1)) { error(GL_INVALID_VALUE); return; }
  fog_value(pname, v, 4);
}
void GlApi::glFogiv(GLenum pname, UserPtr params) {
  int32_t iv[4];
  const uint32_t n = pname == GL_FOG_COLOR ? 4 : 1;
  if (!read_values(params, iv, n)) { error(GL_INVALID_VALUE); return; }
  float v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = pname == GL_FOG_COLOR ? static_cast<float>((2.0 * iv[i] + 1.0) / 4294967295.0) : static_cast<float>(iv[i]);
  fog_value(pname, v, 4);
}

// --- Texture environment and coordinate generation --------------------------------------------------------------------------------------------------------

void GlApi::texenv_value(uint32_t target, uint32_t pname, const float* v, uint32_t) {
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  TexEnv& e = fx_.units[unit].env;
  const uint32_t iv = static_cast<uint32_t>(v[0]);
  if (target == GL_TEXTURE_FILTER_CONTROL) {
    if (pname != GL_TEXTURE_LOD_BIAS) { error(GL_INVALID_ENUM); return; }
    e.lod_bias = v[0];
    ++fx_generation_;
    return;
  }
  if (target == GL_POINT_SPRITE) {
    if (pname != GL_COORD_REPLACE) { error(GL_INVALID_ENUM); return; }
    e.coord_replace = v[0] != 0;
    return;
  }
  if (target != GL_TEXTURE_ENV) { error(GL_INVALID_ENUM); return; }
  switch (pname) {
    case GL_TEXTURE_ENV_MODE:
      if (iv != GL_REPLACE && iv != GL_MODULATE && iv != GL_DECAL && iv != GL_BLEND && iv != GL_ADD && iv != GL_COMBINE) { error(GL_INVALID_ENUM); return; }
      e.mode = iv;
      break;
    case GL_TEXTURE_ENV_COLOR: for (int i = 0; i < 4; ++i) e.color[i] = v[i]; break;
    case GL_COMBINE_RGB:
      if (iv != GL_REPLACE && iv != GL_MODULATE && iv != GL_ADD && iv != GL_ADD_SIGNED && iv != GL_INTERPOLATE && iv != GL_SUBTRACT && iv != GL_DOT3_RGB && iv != GL_DOT3_RGBA) { error(GL_INVALID_ENUM); return; }
      e.combine_rgb = iv;
      break;
    case GL_COMBINE_ALPHA:
      if (iv != GL_REPLACE && iv != GL_MODULATE && iv != GL_ADD && iv != GL_ADD_SIGNED && iv != GL_INTERPOLATE && iv != GL_SUBTRACT) { error(GL_INVALID_ENUM); return; }
      e.combine_alpha = iv;
      break;
    case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB: e.source_rgb[pname - GL_SRC0_RGB] = iv; break;
    case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA: e.source_alpha[pname - GL_SRC0_ALPHA] = iv; break;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
      if (iv < GL_SRC_COLOR || iv > GL_ONE_MINUS_SRC_ALPHA) { error(GL_INVALID_ENUM); return; }
      e.operand_rgb[pname - GL_OPERAND0_RGB] = iv;
      break;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
      if (iv != GL_SRC_ALPHA && iv != GL_ONE_MINUS_SRC_ALPHA) { error(GL_INVALID_ENUM); return; }
      e.operand_alpha[pname - GL_OPERAND0_ALPHA] = iv;
      break;
    case GL_RGB_SCALE: case GL_ALPHA_SCALE:
      if (v[0] != 1 && v[0] != 2 && v[0] != 4) { error(GL_INVALID_VALUE); return; }
      (pname == GL_RGB_SCALE ? e.scale_rgb : e.scale_alpha) = v[0];
      break;
    default: error(GL_INVALID_ENUM); return;
  }
  ++fx_generation_;
}

void GlApi::glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
  if (pname == GL_TEXTURE_ENV_COLOR) { error(GL_INVALID_ENUM); return; }
  texenv_value(target, pname, &param, 1);
}
void GlApi::glTexEnvi(GLenum target, GLenum pname, GLint param) { glTexEnvf(target, pname, static_cast<float>(param)); }
void GlApi::glTexEnvfv(GLenum target, GLenum pname, UserPtr params) {
  float v[4];
  if (!read_values(params, v, pname == GL_TEXTURE_ENV_COLOR ? 4 : 1)) { error(GL_INVALID_VALUE); return; }
  texenv_value(target, pname, v, 4);
}
void GlApi::glTexEnviv(GLenum target, GLenum pname, UserPtr params) {
  int32_t iv[4];
  const uint32_t n = pname == GL_TEXTURE_ENV_COLOR ? 4 : 1;
  if (!read_values(params, iv, n)) { error(GL_INVALID_VALUE); return; }
  float v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = pname == GL_TEXTURE_ENV_COLOR ? static_cast<float>((2.0 * iv[i] + 1.0) / 4294967295.0) : static_cast<float>(iv[i]);
  texenv_value(target, pname, v, 4);
}

void GlApi::texgen_value(uint32_t coord, uint32_t pname, const double* v, uint32_t) {
  if (coord < GL_S || coord > GL_Q) { error(GL_INVALID_ENUM); return; }
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  TexGen& g = fx_.units[unit].gen[coord - GL_S];
  switch (pname) {
    case GL_TEXTURE_GEN_MODE: {
      const uint32_t m = static_cast<uint32_t>(v[0]);
      const bool any = m == GL_OBJECT_LINEAR || m == GL_EYE_LINEAR;
      const bool strq = (m == GL_NORMAL_MAP || m == GL_REFLECTION_MAP) && coord != GL_Q;
      const bool sphere = m == GL_SPHERE_MAP && (coord == GL_S || coord == GL_T);
      if (!any && !strq && !sphere) { error(GL_INVALID_ENUM); return; }
      g.mode = m;
      break;
    }
    case GL_OBJECT_PLANE: for (int i = 0; i < 4; ++i) g.object_plane[i] = static_cast<float>(v[i]); break;
    case GL_EYE_PLANE: {
      // Kept in eye coordinates: multiplied by the inverse of the modelview matrix when set.
      Mat4 inv;
      mat_inverse(modelview_.entries[modelview_.top], &inv);
      const Mat4 it = mat_transpose(inv);
      const float p[4] = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])};
      mat_transform(it, p, g.eye_plane);
      break;
    }
    default: error(GL_INVALID_ENUM); return;
  }
  ++fx_generation_;
}

void GlApi::glTexGend(GLenum coord, GLenum pname, GLdouble param) {
  if (pname != GL_TEXTURE_GEN_MODE) { error(GL_INVALID_ENUM); return; }
  texgen_value(coord, pname, &param, 1);
}
void GlApi::glTexGenf(GLenum coord, GLenum pname, GLfloat param) { glTexGend(coord, pname, param); }
void GlApi::glTexGeni(GLenum coord, GLenum pname, GLint param) { glTexGend(coord, pname, param); }
void GlApi::glTexGendv(GLenum coord, GLenum pname, UserPtr params) {
  double v[4];
  if (!read_values(params, v, pname == GL_TEXTURE_GEN_MODE ? 1 : 4)) { error(GL_INVALID_VALUE); return; }
  texgen_value(coord, pname, v, 4);
}
void GlApi::glTexGenfv(GLenum coord, GLenum pname, UserPtr params) {
  float f[4];
  const uint32_t n = pname == GL_TEXTURE_GEN_MODE ? 1 : 4;
  if (!read_values(params, f, n)) { error(GL_INVALID_VALUE); return; }
  double v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = f[i];
  texgen_value(coord, pname, v, 4);
}
void GlApi::glTexGeniv(GLenum coord, GLenum pname, UserPtr params) {
  int32_t f[4];
  const uint32_t n = pname == GL_TEXTURE_GEN_MODE ? 1 : 4;
  if (!read_values(params, f, n)) { error(GL_INVALID_VALUE); return; }
  double v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = f[i];
  texgen_value(coord, pname, v, 4);
}

// --- Points, lines, polygons --------------------------------------------------------------------------------------------------------------------------------

void GlApi::glPointSize(GLfloat size) {
  if (size <= 0) { error(GL_INVALID_VALUE); return; }
  fx_.point_size = size;
  ++fx_generation_;
}

void GlApi::point_value(uint32_t pname, const float* v, uint32_t) {
  switch (pname) {
    case GL_POINT_SIZE_MIN: if (v[0] < 0) { error(GL_INVALID_VALUE); return; } fx_.point_min = v[0]; break;
    case GL_POINT_SIZE_MAX: if (v[0] < 0) { error(GL_INVALID_VALUE); return; } fx_.point_max = v[0]; break;
    case GL_POINT_FADE_THRESHOLD_SIZE: if (v[0] < 0) { error(GL_INVALID_VALUE); return; } fx_.point_fade = v[0]; break;
    case GL_POINT_DISTANCE_ATTENUATION: for (int i = 0; i < 3; ++i) fx_.point_atten[i] = v[i]; break;
    case GL_POINT_SPRITE_COORD_ORIGIN: {
      const uint32_t m = static_cast<uint32_t>(v[0]);
      if (m != GL_LOWER_LEFT && m != GL_UPPER_LEFT) { error(GL_INVALID_ENUM); return; }
      fx_.point_origin = m;
      break;
    }
    default: error(GL_INVALID_ENUM); return;
  }
  ++fx_generation_;
}

void GlApi::glPointParameterf(GLenum pname, GLfloat param) {
  if (pname == GL_POINT_DISTANCE_ATTENUATION) { error(GL_INVALID_ENUM); return; }
  point_value(pname, &param, 1);
}
void GlApi::glPointParameteri(GLenum pname, GLint param) { glPointParameterf(pname, static_cast<float>(param)); }
void GlApi::glPointParameterfv(GLenum pname, UserPtr params) {
  float v[3];
  if (!read_values(params, v, pname == GL_POINT_DISTANCE_ATTENUATION ? 3 : 1)) { error(GL_INVALID_VALUE); return; }
  point_value(pname, v, 3);
}
void GlApi::glPointParameteriv(GLenum pname, UserPtr params) {
  int32_t iv[3];
  const uint32_t n = pname == GL_POINT_DISTANCE_ATTENUATION ? 3 : 1;
  if (!read_values(params, iv, n)) { error(GL_INVALID_VALUE); return; }
  float v[3];
  for (uint32_t i = 0; i < n; ++i) v[i] = static_cast<float>(iv[i]);
  point_value(pname, v, 3);
}

void GlApi::glLineWidth(GLfloat width) {
  if (width <= 0) { error(GL_INVALID_VALUE); return; }
  fx_.line_width = width;
  gl_.line_width(width);
}

void GlApi::glLineStipple(GLint factor, GLushort pattern) {
  if (factor < 1) factor = 1;
  if (factor > 256) factor = 256;
  fx_.line_stipple_factor = factor;
  fx_.line_stipple_pattern = pattern;
  gl_.desktop.stipple_factor = static_cast<uint32_t>(factor);
  gl_.desktop.stipple_pattern = pattern;
}

void GlApi::glPolygonMode(GLenum face, GLenum mode) {
  if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) { error(GL_INVALID_ENUM); return; }
  if (face != GL_FRONT_AND_BACK && (face != GL_FRONT && face != GL_BACK)) { error(GL_INVALID_ENUM); return; }
  if (config_.core_profile && face != GL_FRONT_AND_BACK) { error(GL_INVALID_ENUM); return; }
  if (face != GL_BACK) { fx_.polygon_front = mode; gl_.desktop.polygon_mode_front = mode; }
  if (face != GL_FRONT) { fx_.polygon_back = mode; gl_.desktop.polygon_mode_back = mode; }
}

void GlApi::glPolygonStipple(UserPtr mask) {
  uint8_t raw[128];
  if (!pixels_in_bitmap(32, 32, mask, ByteView(raw, sizeof(raw)))) { error(GL_INVALID_VALUE); return; }
  for (int i = 0; i < 128; ++i) fx_.polygon_pattern[i] = raw[i];
  for (uint32_t row = 0; row < 32; ++row) {
    uint32_t bits = 0;
    for (uint32_t x = 0; x < 32; ++x) if (raw[row * 4 + x / 8] & (0x80u >> (x % 8))) bits |= 1u << x;
    gl_.desktop.polygon_pattern[row] = bits;
  }
}

void GlApi::glGetPolygonStipple(UserPtr mask) { write_values(mask, fx_.polygon_pattern, 128); }

void GlApi::glPolygonOffset(GLfloat factor, GLfloat units) { gl_.polygon_offset(factor, units); }
void GlApi::glCullFace(GLenum mode) {
  if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) { error(GL_INVALID_ENUM); return; }
  gl_.cull_face(mode);
}
void GlApi::glFrontFace(GLenum mode) {
  if (mode != GL_CW && mode != GL_CCW) { error(GL_INVALID_ENUM); return; }
  gl_.front_face(mode);
}

void GlApi::glClipPlane(GLenum plane, ArrayView<const GLdouble> equation) {
  if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + kMaxClipPlanes) { error(GL_INVALID_ENUM); return; }
  Mat4 inv;
  mat_inverse(modelview_.entries[modelview_.top], &inv);
  const Mat4 it = mat_transpose(inv);
  const float p[4] = {static_cast<float>(equation[0]), static_cast<float>(equation[1]), static_cast<float>(equation[2]), static_cast<float>(equation[3])};
  float out[4];
  mat_transform(it, p, out);
  for (int i = 0; i < 4; ++i) fx_.clip_plane[plane - GL_CLIP_PLANE0][i] = out[i];
  ++fx_generation_;
}

void GlApi::glGetClipPlane(GLenum plane, UserPtr equation) {
  if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + kMaxClipPlanes) { error(GL_INVALID_ENUM); return; }
  // Stored in eye coordinates, as read back.
  write_values(equation, fx_.clip_plane[plane - GL_CLIP_PLANE0], 4);
}

// --- Per-fragment state ------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glAlphaFunc(GLenum func, GLfloat ref) {
  if (func < GL_NEVER || func > GL_ALWAYS) { error(GL_INVALID_ENUM); return; }
  fx_.alpha_func = func;
  fx_.alpha_ref = ref < 0 ? 0 : (ref > 1 ? 1 : ref);
  gl_.desktop.alpha_func = func;
  gl_.desktop.alpha_ref = fx_.alpha_ref;
}

void GlApi::glLogicOp(GLenum opcode) {
  if (opcode < GL_CLEAR || opcode > GL_SET) { error(GL_INVALID_ENUM); return; }
  fx_.logic_mode = opcode;
  gl_.desktop.logic_mode = opcode;
}

void GlApi::glHint(GLenum target, GLenum mode) {
  if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE) { error(GL_INVALID_ENUM); return; }
  switch (target) {
    case GL_PERSPECTIVE_CORRECTION_HINT: fx_.hints[0] = mode; break;
    case GL_POINT_SMOOTH_HINT: fx_.hints[1] = mode; break;
    case GL_LINE_SMOOTH_HINT: fx_.hints[2] = mode; break;
    case GL_POLYGON_SMOOTH_HINT: fx_.hints[3] = mode; break;
    case GL_FOG_HINT: fx_.hints[4] = mode; break;
    case GL_GENERATE_MIPMAP_HINT: fx_.hints[5] = mode; break;
    case GL_TEXTURE_COMPRESSION_HINT: fx_.hints[6] = mode; break;
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: fx_.hints[7] = mode; break;
    default: error(GL_INVALID_ENUM); break;
  }
}

bool GlApi::fixed_enable(uint32_t cap, bool on) {
  ++fx_generation_;
  if (is_light_cap(cap)) {
    const uint32_t bit = 1u << (cap - GL_LIGHT0);
    fx_.lights_enabled = on ? fx_.lights_enabled | bit : fx_.lights_enabled & ~bit;
    return true;
  }
  if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + kMaxClipPlanes) {
    const uint32_t bit = 1u << (cap - GL_CLIP_PLANE0);
    fx_.clip_enabled = on ? fx_.clip_enabled | bit : fx_.clip_enabled & ~bit;
    return true;
  }
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  const auto tex = [&](uint32_t bit) { fx_.units[unit].enabled = on ? fx_.units[unit].enabled | bit : fx_.units[unit].enabled & ~bit; };
  const auto gen = [&](uint32_t bit) { fx_.units[unit].texgen_enabled = on ? fx_.units[unit].texgen_enabled | bit : fx_.units[unit].texgen_enabled & ~bit; };
  switch (cap) {
    case GL_LIGHTING: fx_.lighting = on; return true;
    case GL_COLOR_MATERIAL: fx_.color_material = on; if (on) apply_color_material(fx_.current_color); return true;
    case GL_NORMALIZE: fx_.normalize = on; return true;
    case GL_RESCALE_NORMAL: fx_.rescale_normal = on; return true;
    case GL_FOG: fx_.fog = on; return true;
    case GL_COLOR_SUM: fx_.color_sum = on; return true;
    case GL_ALPHA_TEST: fx_.alpha_test = on; gl_.desktop.alpha_test = on; return true;
    case GL_COLOR_LOGIC_OP: fx_.logic_op = on; gl_.desktop.logic_op = on; return true;
    case GL_INDEX_LOGIC_OP: fx_.index_logic_op = on; return true;
    case GL_LINE_STIPPLE: fx_.line_stipple = on; gl_.desktop.line_stipple = on; return true;
    case GL_POLYGON_STIPPLE: fx_.polygon_stipple = on; gl_.desktop.polygon_stipple = on; return true;
    case GL_LINE_SMOOTH: fx_.line_smooth = on; return true;
    case GL_POINT_SMOOTH: fx_.point_smooth = on; return true;
    case GL_POLYGON_SMOOTH: fx_.polygon_smooth = on; return true;
    case GL_POINT_SPRITE: fx_.point_sprite = on; gl_.desktop.point_sprite = on; return true;
    case GL_VERTEX_PROGRAM_POINT_SIZE: fx_.program_point_size = on; gl_.desktop.point_size_from_program = on; return true;
    case GL_VERTEX_PROGRAM_TWO_SIDE: fx_.vertex_program_two_side = on; return true;
    case GL_POLYGON_OFFSET_LINE: fx_.offset_line = on; gl_.desktop.offset_line = on; return true;
    case GL_POLYGON_OFFSET_POINT: fx_.offset_point = on; gl_.desktop.offset_point = on; return true;
    case GL_AUTO_NORMAL: fx_.auto_normal = on; return true;
    case GL_DEPTH_CLAMP: gl_.desktop.depth_clamp = on; return true;
    case GL_PRIMITIVE_RESTART: gl_.desktop.primitive_restart = on; return true;
    case GL_PRIMITIVE_RESTART_FIXED_INDEX: gl_.desktop.primitive_restart_fixed = on; return true;
    case GL_MULTISAMPLE: gl_.desktop.multisample = on; return true;
    case GL_SAMPLE_ALPHA_TO_ONE: case GL_SAMPLE_SHADING: case GL_SAMPLE_MASK: case GL_TEXTURE_CUBE_MAP_SEAMLESS: case GL_FRAMEBUFFER_SRGB: case GL_DEBUG_OUTPUT: case GL_DEBUG_OUTPUT_SYNCHRONOUS:
      on ? (other_enables_ |= other_enable_bit(cap)) : (other_enables_ &= ~other_enable_bit(cap));
      return true;
    case GL_TEXTURE_1D: tex(1); return true;
    case GL_TEXTURE_2D: tex(2); return true;
    case GL_TEXTURE_3D: tex(4); return true;
    case GL_TEXTURE_CUBE_MAP: tex(8); return true;
    case GL_TEXTURE_RECTANGLE: tex(16); return true;
    case GL_TEXTURE_GEN_S: gen(1); return true;
    case GL_TEXTURE_GEN_T: gen(2); return true;
    case GL_TEXTURE_GEN_R: gen(4); return true;
    case GL_TEXTURE_GEN_Q: gen(8); return true;
    case GL_MAP1_COLOR_4: case GL_MAP1_INDEX: case GL_MAP1_NORMAL: case GL_MAP1_TEXTURE_COORD_1: case GL_MAP1_TEXTURE_COORD_2: case GL_MAP1_TEXTURE_COORD_3: case GL_MAP1_TEXTURE_COORD_4: case GL_MAP1_VERTEX_3: case GL_MAP1_VERTEX_4:
      fx_.map1[cap - GL_MAP1_COLOR_4] = on;
      return true;
    case GL_MAP2_COLOR_4: case GL_MAP2_INDEX: case GL_MAP2_NORMAL: case GL_MAP2_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_4: case GL_MAP2_VERTEX_3: case GL_MAP2_VERTEX_4:
      fx_.map2[cap - GL_MAP2_COLOR_4] = on;
      return true;
    default: return false;
  }
}

bool GlApi::fixed_is_enabled(uint32_t cap, bool* out) const {
  if (is_light_cap(cap)) { *out = (fx_.lights_enabled >> (cap - GL_LIGHT0)) & 1; return true; }
  if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + kMaxClipPlanes) { *out = (fx_.clip_enabled >> (cap - GL_CLIP_PLANE0)) & 1; return true; }
  const uint32_t unit = active_unit() < kMaxTexUnits ? active_unit() : 0;
  const FixedState::Unit& u = fx_.units[unit];
  const webgl::Vao* vao = const_cast<webgl::GlContext&>(gl_).current_vao_ptr();
  switch (cap) {
    case GL_LIGHTING: *out = fx_.lighting; return true;
    case GL_COLOR_MATERIAL: *out = fx_.color_material; return true;
    case GL_NORMALIZE: *out = fx_.normalize; return true;
    case GL_RESCALE_NORMAL: *out = fx_.rescale_normal; return true;
    case GL_FOG: *out = fx_.fog; return true;
    case GL_COLOR_SUM: *out = fx_.color_sum; return true;
    case GL_ALPHA_TEST: *out = fx_.alpha_test; return true;
    case GL_COLOR_LOGIC_OP: *out = fx_.logic_op; return true;
    case GL_INDEX_LOGIC_OP: *out = fx_.index_logic_op; return true;
    case GL_LINE_STIPPLE: *out = fx_.line_stipple; return true;
    case GL_POLYGON_STIPPLE: *out = fx_.polygon_stipple; return true;
    case GL_LINE_SMOOTH: *out = fx_.line_smooth; return true;
    case GL_POINT_SMOOTH: *out = fx_.point_smooth; return true;
    case GL_POLYGON_SMOOTH: *out = fx_.polygon_smooth; return true;
    case GL_POINT_SPRITE: *out = fx_.point_sprite; return true;
    case GL_VERTEX_PROGRAM_POINT_SIZE: *out = fx_.program_point_size; return true;
    case GL_VERTEX_PROGRAM_TWO_SIDE: *out = fx_.vertex_program_two_side; return true;
    case GL_POLYGON_OFFSET_LINE: *out = fx_.offset_line; return true;
    case GL_POLYGON_OFFSET_POINT: *out = fx_.offset_point; return true;
    case GL_AUTO_NORMAL: *out = fx_.auto_normal; return true;
    case GL_DEPTH_CLAMP: *out = gl_.desktop.depth_clamp; return true;
    case GL_PRIMITIVE_RESTART: *out = gl_.desktop.primitive_restart; return true;
    case GL_PRIMITIVE_RESTART_FIXED_INDEX: *out = gl_.desktop.primitive_restart_fixed; return true;
    case GL_MULTISAMPLE: *out = gl_.desktop.multisample; return true;
    case GL_SAMPLE_ALPHA_TO_ONE: case GL_SAMPLE_SHADING: case GL_SAMPLE_MASK: case GL_TEXTURE_CUBE_MAP_SEAMLESS: case GL_FRAMEBUFFER_SRGB: case GL_DEBUG_OUTPUT: case GL_DEBUG_OUTPUT_SYNCHRONOUS:
      *out = (other_enables_ & other_enable_bit(cap)) != 0;
      return true;
    case GL_TEXTURE_1D: *out = u.enabled & 1; return true;
    case GL_TEXTURE_2D: *out = u.enabled & 2; return true;
    case GL_TEXTURE_3D: *out = u.enabled & 4; return true;
    case GL_TEXTURE_CUBE_MAP: *out = u.enabled & 8; return true;
    case GL_TEXTURE_RECTANGLE: *out = u.enabled & 16; return true;
    case GL_TEXTURE_GEN_S: *out = u.texgen_enabled & 1; return true;
    case GL_TEXTURE_GEN_T: *out = u.texgen_enabled & 2; return true;
    case GL_TEXTURE_GEN_R: *out = u.texgen_enabled & 4; return true;
    case GL_TEXTURE_GEN_Q: *out = u.texgen_enabled & 8; return true;
    case GL_VERTEX_ARRAY: *out = vao->attribs[kAttrVertex].enabled; return true;
    case GL_NORMAL_ARRAY: *out = vao->attribs[kAttrNormal].enabled; return true;
    case GL_COLOR_ARRAY: *out = vao->attribs[kAttrColor].enabled; return true;
    case GL_SECONDARY_COLOR_ARRAY: *out = vao->attribs[kAttrSecondary].enabled; return true;
    case GL_FOG_COORD_ARRAY: *out = vao->attribs[kAttrFog].enabled; return true;
    case GL_INDEX_ARRAY: *out = vao->attribs[kAttrIndex].enabled; return true;
    case GL_EDGE_FLAG_ARRAY: *out = vao->attribs[kAttrEdge].enabled; return true;
    case GL_TEXTURE_COORD_ARRAY: *out = vao->attribs[kAttrTex0 + (client_active_unit_ < kMaxTexUnits ? client_active_unit_ : 0)].enabled; return true;
    default:
      if (cap >= GL_MAP1_COLOR_4 && cap <= GL_MAP1_VERTEX_4) { *out = fx_.map1[cap - GL_MAP1_COLOR_4]; return true; }
      if (cap >= GL_MAP2_COLOR_4 && cap <= GL_MAP2_VERTEX_4) { *out = fx_.map2[cap - GL_MAP2_COLOR_4]; return true; }
      return false;
  }
}

uint32_t GlApi::other_enable_bit(uint32_t cap) {
  switch (cap) {
    case GL_SAMPLE_ALPHA_TO_ONE: return 1;
    case GL_SAMPLE_SHADING: return 2;
    case GL_SAMPLE_MASK: return 4;
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: return 8;
    case GL_FRAMEBUFFER_SRGB: return 16;
    case GL_DEBUG_OUTPUT: return 32;
    case GL_DEBUG_OUTPUT_SYNCHRONOUS: return 64;
    default: return 0;
  }
}

void GlApi::glEnable(GLenum cap) { enable_cap(cap, true); }
void GlApi::glDisable(GLenum cap) { enable_cap(cap, false); }

void GlApi::enable_cap(uint32_t cap, bool on) {
  switch (cap) {
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_STENCIL_TEST: case GL_SCISSOR_TEST: case GL_POLYGON_OFFSET_FILL: case GL_DITHER: case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_RASTERIZER_DISCARD:
      gl_.enable(cap, on);
      if (cap == GL_BLEND) { for (auto& b : gl_.desktop.blend) b.enabled = on; }
      return;
    default: break;
  }
  if (!fixed_enable(cap, on)) error(GL_INVALID_ENUM);
}

uint64_t GlApi::glIsEnabled(GLenum cap) {
  switch (cap) {
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_STENCIL_TEST: case GL_SCISSOR_TEST: case GL_POLYGON_OFFSET_FILL: case GL_DITHER: case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_RASTERIZER_DISCARD:
      return gl_.is_enabled(cap) ? 1 : 0;
    default: break;
  }
  bool on = false;
  if (!fixed_is_enabled(cap, &on)) { error(GL_INVALID_ENUM); return 0; }
  return on ? 1 : 0;
}

}  // namespace opengl
