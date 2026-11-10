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

// The less common corners: raster positions, evaluators, attribute stacks, selection and feedback, queries, sync objects, debug output, and what a surface does.
#include "OpenGl/FixedFunction.hpp"
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlFeedback.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/generated/GlEnums.inc"
#include "OpenGl/generated/GlOpIds.inc"

namespace opengl {

// --- Raster position ---------------------------------------------------------------------------------------------------------------------------------------

void GlApi::raster_pos(float x, float y, float z, float w) {
  const float object[4] = {x, y, z, w};
  float eye[4], clip[4];
  mat_transform(modelview_.entries[modelview_.top], object, eye);
  mat_transform(projection_.entries[projection_.top], eye, clip);
  // Clipped as a point: outside the volume the position is invalid.
  fx_.raster_valid = true;
  for (int i = 0; i < 3; ++i) if (clip[i] > clip[3] || clip[i] < -clip[3]) fx_.raster_valid = false;
  if (clip[3] <= 0) fx_.raster_valid = false;
  for (uint32_t p = 0; p < kMaxClipPlanes; ++p) {
    if (!(fx_.clip_enabled & (1u << p))) continue;
    const double* pl = fx_.clip_plane[p];
    if (pl[0] * eye[0] + pl[1] * eye[1] + pl[2] * eye[2] + pl[3] * eye[3] < 0) fx_.raster_valid = false;
  }
  if (!fx_.raster_valid) return;
  const float iw = 1 / clip[3];
  fx_.raster_pos[0] = static_cast<float>(gl_.vp_[0]) + (clip[0] * iw * 0.5f + 0.5f) * static_cast<float>(gl_.vp_[2]);
  fx_.raster_pos[1] = static_cast<float>(gl_.vp_[1]) + (clip[1] * iw * 0.5f + 0.5f) * static_cast<float>(gl_.vp_[3]);
  fx_.raster_pos[2] = depth_range_values_[0] + (clip[2] * iw * 0.5f + 0.5f) * (depth_range_values_[1] - depth_range_values_[0]);
  fx_.raster_pos[3] = iw;
  fx_.raster_distance = fx_.fog_source == GL_FOG_COORD ? fx_.current_fog : shader::fsqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
  for (int i = 0; i < 4; ++i) { fx_.raster_color[i] = fx_.current_color[i]; fx_.raster_secondary[i] = fx_.current_secondary[i]; }
  fx_.raster_index = fx_.current_index;
  for (uint32_t u = 0; u < kMaxTexUnits; ++u) {
    const MatrixStack& ts = texture_stack_[u];
    mat_transform(ts.entries[ts.top], fx_.current_tex[u], fx_.raster_tex[u]);
  }
}

void GlApi::window_pos(float x, float y, float z) {
  fx_.raster_pos[0] = x;
  fx_.raster_pos[1] = y;
  const float zc = z < 0 ? 0 : (z > 1 ? 1 : z);
  fx_.raster_pos[2] = depth_range_values_[0] + zc * (depth_range_values_[1] - depth_range_values_[0]);
  fx_.raster_pos[3] = 1;
  fx_.raster_valid = true;
  fx_.raster_distance = fx_.fog_source == GL_FOG_COORD ? fx_.current_fog : 0;
  for (int i = 0; i < 4; ++i) { fx_.raster_color[i] = fx_.current_color[i]; fx_.raster_secondary[i] = fx_.current_secondary[i]; }
  fx_.raster_index = fx_.current_index;
  for (uint32_t u = 0; u < kMaxTexUnits; ++u) for (int i = 0; i < 4; ++i) fx_.raster_tex[u][i] = fx_.current_tex[u][i];
}

// --- Evaluators ---------------------------------------------------------------------------------------------------------------------------------------------

struct GlApi::EvalStore {
  struct Map {
    explicit Map(allocators::IAllocator& a) : points(a) {}
    bool defined = false;
    float u1 = 0, u2 = 1, v1 = 0, v2 = 1;
    int32_t uorder = 1, vorder = 1;
    List<float> points;
  };
  explicit EvalStore(allocators::IAllocator& a) : one{Map(a), Map(a), Map(a), Map(a), Map(a), Map(a), Map(a), Map(a), Map(a)}, two{Map(a), Map(a), Map(a), Map(a), Map(a), Map(a), Map(a), Map(a), Map(a)} {}
  Map one[9], two[9];
};

namespace {
constexpr uint32_t kEvalComponents[9] = {4, 1, 3, 1, 2, 3, 4, 3, 4};      // COLOR_4, INDEX, NORMAL, TEXTURE_COORD_1..4, VERTEX_3, VERTEX_4
constexpr int32_t kMaxOrder = 32;

// The value of a Bezier curve of `order` control points at u (de Casteljau over `n` components).
void bezier(const float* points, int32_t order, uint32_t n, uint32_t stride, float u, float* out, float* derivative) {
  float work[kMaxOrder][4];
  for (int32_t i = 0; i < order; ++i) for (uint32_t k = 0; k < n; ++k) work[i][k] = points[static_cast<uint32_t>(i) * stride + k];
  for (int32_t r = 1; r < order; ++r) {
    for (int32_t i = 0; i < order - r; ++i) {
      for (uint32_t k = 0; k < n; ++k) work[i][k] = work[i][k] + (work[i + 1][k] - work[i][k]) * u;
    }
    if (r == order - 2 && derivative) for (uint32_t k = 0; k < n; ++k) derivative[k] = static_cast<float>(order - 1) * (work[1][k] - work[0][k]);
  }
  if (derivative && order < 2) for (uint32_t k = 0; k < n; ++k) derivative[k] = 0;
  for (uint32_t k = 0; k < n; ++k) out[k] = work[0][k];
}
}  // namespace

void GlApi::eval_define(uint32_t target, bool two, double u1, double u2, double v1, double v2, int32_t ustride, int32_t uorder, int32_t vstride, int32_t vorder, UserPtr points, bool doubles) {
  const uint32_t base = two ? GL_MAP2_COLOR_4 : GL_MAP1_COLOR_4;
  if (target < base || target >= base + 9) { error(GL_INVALID_ENUM); return; }
  const uint32_t index = target - base;
  const uint32_t n = kEvalComponents[index];
  if (u1 == u2 || (two && v1 == v2)) { error(GL_INVALID_VALUE); return; }
  if (uorder < 1 || uorder > kMaxOrder || (two && (vorder < 1 || vorder > kMaxOrder))) { error(GL_INVALID_VALUE); return; }
  if (ustride < static_cast<int32_t>(n) || (two && vstride < static_cast<int32_t>(n))) { error(GL_INVALID_VALUE); return; }
  if (!eval_) { eval_ = UniquePointer<EvalStore>::make(a_, a_); if (!eval_) { error(GL_OUT_OF_MEMORY); return; } }
  EvalStore::Map& m = two ? eval_->two[index] : eval_->one[index];
  m.u1 = static_cast<float>(u1); m.u2 = static_cast<float>(u2);
  m.v1 = static_cast<float>(v1); m.v2 = static_cast<float>(v2);
  m.uorder = uorder;
  m.vorder = two ? vorder : 1;
  m.points.clear();
  const uint64_t count = uint64_t{static_cast<uint32_t>(uorder)} * static_cast<uint32_t>(m.vorder) * n;
  if (!m.points.reserve(count)) { error(GL_OUT_OF_MEMORY); return; }
  const uint64_t element = doubles ? 8 : 4;
  for (int32_t iu = 0; iu < uorder; ++iu) {
    for (int32_t iv = 0; iv < m.vorder; ++iv) {
      for (uint32_t k = 0; k < n; ++k) {
        const uint64_t offset = (uint64_t{static_cast<uint32_t>(iu)} * static_cast<uint32_t>(ustride) + (two ? uint64_t{static_cast<uint32_t>(iv)} * static_cast<uint32_t>(vstride) : 0) + k) * element;
        float value = 0;
        if (doubles) {
          double d = 0;
          if (!read_values(points.plus(offset), &d, 1)) { error(GL_INVALID_VALUE); return; }
          value = static_cast<float>(d);
        } else if (!read_values(points.plus(offset), &value, 1)) {
          error(GL_INVALID_VALUE);
          return;
        }
        m.points.push_back(value);
      }
    }
  }
  m.defined = true;
}

void GlApi::glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, UserPtr points) { eval_define(target, false, u1, u2, 0, 1, stride, order, 0, 1, points, false); }
void GlApi::glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, UserPtr points) { eval_define(target, false, u1, u2, 0, 1, stride, order, 0, 1, points, true); }
void GlApi::glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, UserPtr points) { eval_define(target, true, u1, u2, v1, v2, ustride, uorder, vstride, vorder, points, false); }
void GlApi::glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, UserPtr points) { eval_define(target, true, u1, u2, v1, v2, ustride, uorder, vstride, vorder, points, true); }
void GlApi::glMapGrid1f(GLint un, GLfloat u1, GLfloat u2) {
  if (un < 1) { error(GL_INVALID_VALUE); return; }
  fx_.grid1_n = un; fx_.grid1_u[0] = u1; fx_.grid1_u[1] = u2;
}
void GlApi::glMapGrid1d(GLint un, GLdouble u1, GLdouble u2) { glMapGrid1f(un, static_cast<float>(u1), static_cast<float>(u2)); }
void GlApi::glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2) {
  if (un < 1 || vn < 1) { error(GL_INVALID_VALUE); return; }
  fx_.grid2_n = un; fx_.grid2_m = vn; fx_.grid2_u[0] = u1; fx_.grid2_u[1] = u2; fx_.grid2_v[0] = v1; fx_.grid2_v[1] = v2;
}
void GlApi::glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2) { glMapGrid2f(un, static_cast<float>(u1), static_cast<float>(u2), vn, static_cast<float>(v1), static_cast<float>(v2)); }

// What a value of an enabled map changes: indices as in kEvalComponents.
void GlApi::eval_apply(const float* v, uint32_t kind, uint32_t) {
  switch (kind) {
    case 0: set_color(v[0], v[1], v[2], v[3]); break;
    case 1: set_index(v[0]); break;
    case 2: set_normal(v[0], v[1], v[2]); break;
    case 3: set_texcoord(0, v[0], 0, 0, 1); break;
    case 4: set_texcoord(0, v[0], v[1], 0, 1); break;
    case 5: set_texcoord(0, v[0], v[1], v[2], 1); break;
    case 6: set_texcoord(0, v[0], v[1], v[2], v[3]); break;
    default: break;
  }
}

void GlApi::eval_coord1(float u) {
  if (!eval_) return;
  float v[4], d[4];
  for (uint32_t kind = 0; kind < 7; ++kind) {
    if (!fx_.map1[kind] || !eval_->one[kind].defined) continue;
    if ((kind >= 3 && kind <= 5) && fx_.map1[6] && eval_->one[6].defined) continue;      // the highest texture map wins
    const EvalStore::Map& m = eval_->one[kind];
    const float t = (u - m.u1) / (m.u2 - m.u1);
    bezier(m.points.view().data(), m.uorder, kEvalComponents[kind], kEvalComponents[kind], t, v, nullptr);
    eval_apply(v, kind, kEvalComponents[kind]);
  }
  for (uint32_t kind = 8; kind >= 7; --kind) {
    if (!fx_.map1[kind] || !eval_->one[kind].defined) continue;
    const EvalStore::Map& m = eval_->one[kind];
    const float t = (u - m.u1) / (m.u2 - m.u1);
    bezier(m.points.view().data(), m.uorder, kEvalComponents[kind], kEvalComponents[kind], t, v, d);
    imm_vertex(v[0], v[1], v[2], kind == 8 ? v[3] : 1.0f);
    break;
  }
}

void GlApi::eval_coord2(float u, float w) {
  if (!eval_) return;
  const auto eval2 = [&](const EvalStore::Map& m, uint32_t n, float* out, float* du, float* dv) {
    const float s = (u - m.u1) / (m.u2 - m.u1), t = (w - m.v1) / (m.v2 - m.v1);
    // Along v for each row of u, then along u.
    float row[kMaxOrder][4], row_v[kMaxOrder][4];
    for (int32_t iu = 0; iu < m.uorder; ++iu) {
      bezier(m.points.view().data() + static_cast<uint32_t>(iu) * static_cast<uint32_t>(m.vorder) * n, m.vorder, n, n, t, row[iu], row_v[iu]);
    }
    float flat[kMaxOrder * 4];
    for (int32_t iu = 0; iu < m.uorder; ++iu) for (uint32_t k = 0; k < n; ++k) flat[static_cast<uint32_t>(iu) * n + k] = row[iu][k];
    bezier(flat, m.uorder, n, n, s, out, du);
    if (dv) {
      for (int32_t iu = 0; iu < m.uorder; ++iu) for (uint32_t k = 0; k < n; ++k) flat[static_cast<uint32_t>(iu) * n + k] = row_v[iu][k];
      float scratch[4];
      bezier(flat, m.uorder, n, n, s, dv, scratch);
    }
  };
  float v[4], du[4], dv[4];
  for (uint32_t kind = 0; kind < 7; ++kind) {
    if (!fx_.map2[kind] || !eval_->two[kind].defined) continue;
    eval2(eval_->two[kind], kEvalComponents[kind], v, du, nullptr);
    eval_apply(v, kind, kEvalComponents[kind]);
  }
  for (uint32_t kind = 8; kind >= 7; --kind) {
    if (!fx_.map2[kind] || !eval_->two[kind].defined) continue;
    const uint32_t n = kEvalComponents[kind];
    eval2(eval_->two[kind], n, v, du, dv);
    if (fx_.auto_normal) {
      float a[3], b[3];
      if (n == 4) {
        const float wv = v[3] != 0 ? v[3] : 1;
        for (int i = 0; i < 3; ++i) { a[i] = (du[i] * wv - v[i] * du[3]) ; b[i] = (dv[i] * wv - v[i] * dv[3]); }
      } else {
        for (int i = 0; i < 3; ++i) { a[i] = du[i]; b[i] = dv[i]; }
      }
      const float nx = a[1] * b[2] - a[2] * b[1], ny = a[2] * b[0] - a[0] * b[2], nz = a[0] * b[1] - a[1] * b[0];
      const float len = shader::fsqrt(nx * nx + ny * ny + nz * nz);
      if (len > 0) set_normal(nx / len, ny / len, nz / len);
    }
    imm_vertex(v[0], v[1], v[2], kind == 8 ? v[3] : 1.0f);
    break;
  }
}

void GlApi::glEvalPoint1(GLint i) {
  const float du = (fx_.grid1_u[1] - fx_.grid1_u[0]) / static_cast<float>(fx_.grid1_n);
  eval_coord1(i == fx_.grid1_n ? fx_.grid1_u[1] : fx_.grid1_u[0] + static_cast<float>(i) * du);
}
void GlApi::glEvalPoint2(GLint i, GLint j) {
  const float du = (fx_.grid2_u[1] - fx_.grid2_u[0]) / static_cast<float>(fx_.grid2_n), dv = (fx_.grid2_v[1] - fx_.grid2_v[0]) / static_cast<float>(fx_.grid2_m);
  eval_coord2(i == fx_.grid2_n ? fx_.grid2_u[1] : fx_.grid2_u[0] + static_cast<float>(i) * du, j == fx_.grid2_m ? fx_.grid2_v[1] : fx_.grid2_v[0] + static_cast<float>(j) * dv);
}
void GlApi::glEvalMesh1(GLenum mode, GLint i1, GLint i2) {
  if (mode != GL_POINT && mode != GL_LINE) { error(GL_INVALID_ENUM); return; }
  glBegin(mode == GL_POINT ? GL_POINTS : GL_LINE_STRIP);
  for (int32_t i = i1; i <= i2; ++i) glEvalPoint1(i);
  glEnd();
}
void GlApi::glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2) {
  if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) { error(GL_INVALID_ENUM); return; }
  if (mode == GL_FILL) {
    for (int32_t j = j1; j < j2; ++j) {
      glBegin(GL_QUAD_STRIP);
      for (int32_t i = i1; i <= i2; ++i) { glEvalPoint2(i, j); glEvalPoint2(i, j + 1); }
      glEnd();
    }
  } else if (mode == GL_LINE) {
    for (int32_t j = j1; j <= j2; ++j) {
      glBegin(GL_LINE_STRIP);
      for (int32_t i = i1; i <= i2; ++i) glEvalPoint2(i, j);
      glEnd();
    }
    for (int32_t i = i1; i <= i2; ++i) {
      glBegin(GL_LINE_STRIP);
      for (int32_t j = j1; j <= j2; ++j) glEvalPoint2(i, j);
      glEnd();
    }
  } else {
    glBegin(GL_POINTS);
    for (int32_t j = j1; j <= j2; ++j) for (int32_t i = i1; i <= i2; ++i) glEvalPoint2(i, j);
    glEnd();
  }
}
void GlApi::glGetMapfv(GLenum target, GLenum query, UserPtr v) { map_get(target, query, v, 1); }
void GlApi::glGetMapdv(GLenum target, GLenum query, UserPtr v) { map_get(target, query, v, 2); }
void GlApi::glGetMapiv(GLenum target, GLenum query, UserPtr v) { map_get(target, query, v, 0); }
void GlApi::map_get(uint32_t target, uint32_t query, UserPtr at, uint32_t kind) {
  const bool two = target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4;
  if (!two && !(target >= GL_MAP1_COLOR_4 && target <= GL_MAP1_VERTEX_4)) { error(GL_INVALID_ENUM); return; }
  const uint32_t index = target - (two ? GL_MAP2_COLOR_4 : GL_MAP1_COLOR_4);
  const uint32_t n = kEvalComponents[index];
  const EvalStore::Map* m = eval_ ? (two ? &eval_->two[index] : &eval_->one[index]) : nullptr;
  const auto put = [&](uint64_t i, double value) {
    if (kind == 1) { const float f = static_cast<float>(value); write_values(at.plus(i * 4), &f, 1); }
    else if (kind == 2) write_values(at.plus(i * 8), &value, 1);
    else { const int32_t iv = static_cast<int32_t>(value); write_values(at.plus(i * 4), &iv, 1); }
  };
  switch (query) {
    case GL_COEFF:
      if (m && m->defined) for (uint64_t i = 0; i < m->points.count(); ++i) put(i, m->points[i]);
      break;
    case GL_ORDER:
      put(0, m && m->defined ? m->uorder : 1);
      if (two) put(1, m && m->defined ? m->vorder : 1);
      break;
    case GL_DOMAIN:
      put(0, m ? m->u1 : 0); put(1, m ? m->u2 : 1);
      if (two) { put(2, m ? m->v1 : 0); put(3, m ? m->v2 : 1); }
      break;
    default: error(GL_INVALID_ENUM); break;
  }
  (void)n;
}

// --- Attribute stacks ---------------------------------------------------------------------------------------------------------------------------------------------

struct GlApi::AttribFrame {
  uint32_t mask = 0;
  FixedState fx;
  // the engine's part
  bool blend = false, cull = false, depth_test = false, stencil_test = false, scissor_test = false, offset_fill = false, dither = false;
  float blend_color[4] = {};
  uint32_t eq_rgb = 0, eq_a = 0, sr = 0, dr = 0, sa = 0, da = 0;
  bool cmask[4] = {};
  bool dmask = true;
  uint32_t dfunc = 0;
  uint32_t sfunc[2] = {}, sfail[2] = {}, sdpfail[2] = {}, sdppass[2] = {}, swmask[2] = {};
  int32_t sref[2] = {};
  uint32_t smask[2] = {};
  uint32_t cull_mode = 0, front_face = 0;
  float po_factor = 0, po_units = 0, line_width = 1;
  float clear_color[4] = {}, clear_depth = 1;
  int32_t clear_stencil = 0;
  int32_t vp[4] = {}, sc[4] = {};
  float depth_range[2] = {0, 1};
  uint32_t draw_buffer = 0, read_buffer = 0;
  uint32_t active_unit = 0;
  webgl::TextureUnit units[webgl::kMaxTextureUnits];
  webgl::DesktopState desktop;
};

struct GlApi::ClientFrame {
  uint32_t mask = 0;
  PixelStore pack, unpack;
  webgl::Vao vao;
  uint32_t array_buffer = 0;
  uint32_t client_unit = 0;
};

void GlApi::glPushAttrib(GLbitfield mask) {
  if (attrib_stack_.count() >= kMaxAttribStack) { error(GL_STACK_OVERFLOW); return; }
  UniquePointer<AttribFrame> frame = UniquePointer<AttribFrame>::make(a_);
  if (!frame) { error(GL_OUT_OF_MEMORY); return; }
  frame->mask = mask;
  frame->fx = fx_;
  frame->blend = gl_.blend_; frame->cull = gl_.cull_; frame->depth_test = gl_.depth_test_; frame->stencil_test = gl_.stencil_test_; frame->scissor_test = gl_.scissor_test_; frame->offset_fill = gl_.offset_fill_; frame->dither = gl_.dither_;
  for (int i = 0; i < 4; ++i) { frame->blend_color[i] = gl_.blend_color_[i]; frame->cmask[i] = gl_.cmask_[i]; frame->clear_color[i] = gl_.clear_color_[i]; frame->vp[i] = gl_.vp_[i]; frame->sc[i] = gl_.sc_[i]; }
  frame->eq_rgb = gl_.blend_eq_rgb_; frame->eq_a = gl_.blend_eq_a_; frame->sr = gl_.blend_sr_; frame->dr = gl_.blend_dr_; frame->sa = gl_.blend_sa_; frame->da = gl_.blend_da_;
  frame->dmask = gl_.dmask_; frame->dfunc = gl_.dfunc_;
  for (int i = 0; i < 2; ++i) { frame->sfunc[i] = gl_.sfunc_[i]; frame->sref[i] = gl_.sref_[i]; frame->smask[i] = gl_.smask_[i]; frame->sfail[i] = gl_.sfail_[i]; frame->sdpfail[i] = gl_.sdpfail_[i]; frame->sdppass[i] = gl_.sdppass_[i]; frame->swmask[i] = gl_.swmask_[i]; }
  frame->cull_mode = gl_.cull_mode_; frame->front_face = gl_.front_face_; frame->po_factor = gl_.po_factor_; frame->po_units = gl_.po_units_; frame->line_width = gl_.line_width_;
  frame->clear_depth = gl_.clear_depth_; frame->clear_stencil = gl_.clear_stencil_;
  frame->depth_range[0] = depth_range_values_[0]; frame->depth_range[1] = depth_range_values_[1];
  frame->draw_buffer = default_draw_buffer_; frame->read_buffer = default_read_buffer_;
  frame->active_unit = gl_.active_unit_;
  for (uint32_t i = 0; i < webgl::kMaxTextureUnits; ++i) frame->units[i] = gl_.units_[i];
  frame->desktop = gl_.desktop;
  attrib_stack_.push_back(static_cast<UniquePointer<AttribFrame>&&>(frame));
}

void GlApi::glPopAttrib() {
  if (attrib_stack_.count() == 0) { error(GL_STACK_UNDERFLOW); return; }
  const AttribFrame& f = *attrib_stack_[attrib_stack_.count() - 1];
  const uint32_t m = f.mask;
  FixedState& x = fx_;
  const FixedState& o = f.fx;
  webgl::DesktopState& ds = gl_.desktop;
  const webgl::DesktopState& os = f.desktop;
  if (m & GL_CURRENT_BIT) {
    for (int i = 0; i < 4; ++i) { x.current_color[i] = o.current_color[i]; x.current_secondary[i] = o.current_secondary[i]; x.raster_pos[i] = o.raster_pos[i]; x.raster_color[i] = o.raster_color[i]; x.raster_secondary[i] = o.raster_secondary[i]; }
    for (int i = 0; i < 3; ++i) x.current_normal[i] = o.current_normal[i];
    for (uint32_t u = 0; u < kMaxTexUnits; ++u) for (int i = 0; i < 4; ++i) { x.current_tex[u][i] = o.current_tex[u][i]; x.raster_tex[u][i] = o.raster_tex[u][i]; }
    x.current_fog = o.current_fog; x.current_index = o.current_index; x.current_edge = o.current_edge; x.raster_distance = o.raster_distance; x.raster_index = o.raster_index; x.raster_valid = o.raster_valid;
  }
  if (m & GL_LIGHTING_BIT) {
    x.lighting = o.lighting; x.lights_enabled = o.lights_enabled;
    for (uint32_t i = 0; i < kMaxLights; ++i) x.lights[i] = o.lights[i];
    x.material[0] = o.material[0]; x.material[1] = o.material[1];
    for (int i = 0; i < 4; ++i) x.model_ambient[i] = o.model_ambient[i];
    x.local_viewer = o.local_viewer; x.two_side = o.two_side; x.separate_specular = o.separate_specular;
    x.color_material = o.color_material; x.color_material_face = o.color_material_face; x.color_material_mode = o.color_material_mode;
    x.shade_model = o.shade_model;
    ds.flat_shade = o.shade_model == GL_FLAT;
  }
  if (m & GL_TRANSFORM_BIT) {
    x.matrix_mode = o.matrix_mode; x.normalize = o.normalize; x.rescale_normal = o.rescale_normal; x.clip_enabled = o.clip_enabled;
    for (uint32_t i = 0; i < kMaxClipPlanes; ++i) for (int k = 0; k < 4; ++k) x.clip_plane[i][k] = o.clip_plane[i][k];
    ds.depth_clamp = os.depth_clamp;
  }
  if (m & GL_FOG_BIT) {
    x.fog = o.fog; x.fog_mode = o.fog_mode; x.fog_source = o.fog_source; x.fog_density = o.fog_density; x.fog_start = o.fog_start; x.fog_end = o.fog_end; x.fog_index = o.fog_index;
    for (int i = 0; i < 4; ++i) x.fog_color[i] = o.fog_color[i];
  }
  if (m & GL_POINT_BIT) {
    x.point_size = o.point_size; x.point_min = o.point_min; x.point_max = o.point_max; x.point_fade = o.point_fade; x.point_smooth = o.point_smooth; x.point_sprite = o.point_sprite; x.point_origin = o.point_origin;
    for (int i = 0; i < 3; ++i) x.point_atten[i] = o.point_atten[i];
    ds.point_sprite = o.point_sprite;
  }
  if (m & GL_LINE_BIT) {
    x.line_smooth = o.line_smooth; x.line_stipple = o.line_stipple; x.line_stipple_factor = o.line_stipple_factor; x.line_stipple_pattern = o.line_stipple_pattern; x.line_width = o.line_width;
    gl_.line_width_ = f.line_width;
    ds.line_stipple = o.line_stipple; ds.stipple_factor = os.stipple_factor; ds.stipple_pattern = os.stipple_pattern;
  }
  if (m & GL_POLYGON_BIT) {
    x.polygon_front = o.polygon_front; x.polygon_back = o.polygon_back; x.polygon_smooth = o.polygon_smooth; x.polygon_stipple = o.polygon_stipple; x.offset_point = o.offset_point; x.offset_line = o.offset_line;
    ds.polygon_mode_front = os.polygon_mode_front; ds.polygon_mode_back = os.polygon_mode_back; ds.polygon_stipple = os.polygon_stipple; ds.offset_line = os.offset_line; ds.offset_point = os.offset_point;
    gl_.cull_ = f.cull; gl_.cull_mode_ = f.cull_mode; gl_.front_face_ = f.front_face; gl_.po_factor_ = f.po_factor; gl_.po_units_ = f.po_units; gl_.offset_fill_ = f.offset_fill;
  }
  if (m & GL_POLYGON_STIPPLE_BIT) {
    for (int i = 0; i < 128; ++i) x.polygon_pattern[i] = o.polygon_pattern[i];
    for (int i = 0; i < 32; ++i) ds.polygon_pattern[i] = os.polygon_pattern[i];
  }
  if (m & GL_TEXTURE_BIT) {
    for (uint32_t i = 0; i < kMaxTexUnits; ++i) x.units[i] = o.units[i];
    gl_.active_unit_ = f.active_unit;
    for (uint32_t i = 0; i < webgl::kMaxTextureUnits; ++i) gl_.units_[i] = f.units[i];
  }
  if (m & GL_COLOR_BUFFER_BIT) {
    x.alpha_test = o.alpha_test; x.alpha_func = o.alpha_func; x.alpha_ref = o.alpha_ref; x.logic_op = o.logic_op; x.index_logic_op = o.index_logic_op; x.logic_mode = o.logic_mode; x.clear_index = o.clear_index; x.index_mask = o.index_mask;
    ds.alpha_test = os.alpha_test; ds.alpha_func = os.alpha_func; ds.alpha_ref = os.alpha_ref; ds.logic_op = os.logic_op; ds.logic_mode = os.logic_mode; ds.per_buffer_masks = os.per_buffer_masks; ds.per_buffer_blend = os.per_buffer_blend;
    for (uint32_t i = 0; i < webgl::kMaxDrawBuffers; ++i) { ds.color_mask[i] = os.color_mask[i]; ds.blend[i] = os.blend[i]; }
    gl_.blend_ = f.blend; gl_.dither_ = f.dither;
    for (int i = 0; i < 4; ++i) { gl_.blend_color_[i] = f.blend_color[i]; gl_.cmask_[i] = f.cmask[i]; gl_.clear_color_[i] = f.clear_color[i]; }
    gl_.blend_eq_rgb_ = f.eq_rgb; gl_.blend_eq_a_ = f.eq_a; gl_.blend_sr_ = f.sr; gl_.blend_dr_ = f.dr; gl_.blend_sa_ = f.sa; gl_.blend_da_ = f.da;
    if (f.draw_buffer != default_draw_buffer_) { default_draw_buffer_ = f.draw_buffer; surface_generation_ = 0; surface_changed(); }
  }
  if (m & GL_DEPTH_BUFFER_BIT) { gl_.depth_test_ = f.depth_test; gl_.dfunc_ = f.dfunc; gl_.dmask_ = f.dmask; gl_.clear_depth_ = f.clear_depth; }
  if (m & GL_STENCIL_BUFFER_BIT) {
    gl_.stencil_test_ = f.stencil_test; gl_.clear_stencil_ = f.clear_stencil;
    for (int i = 0; i < 2; ++i) { gl_.sfunc_[i] = f.sfunc[i]; gl_.sref_[i] = f.sref[i]; gl_.smask_[i] = f.smask[i]; gl_.sfail_[i] = f.sfail[i]; gl_.sdpfail_[i] = f.sdpfail[i]; gl_.sdppass_[i] = f.sdppass[i]; gl_.swmask_[i] = f.swmask[i]; }
  }
  if (m & GL_VIEWPORT_BIT) {
    for (int i = 0; i < 4; ++i) gl_.vp_[i] = f.vp[i];
    glDepthRange(f.depth_range[0], f.depth_range[1]);
  }
  if (m & GL_SCISSOR_BIT) { gl_.scissor_test_ = f.scissor_test; for (int i = 0; i < 4; ++i) gl_.sc_[i] = f.sc[i]; }
  if (m & GL_ENABLE_BIT) {
    x.alpha_test = o.alpha_test; x.auto_normal = o.auto_normal; x.clip_enabled = o.clip_enabled; x.color_material = o.color_material; x.fog = o.fog; x.lighting = o.lighting; x.lights_enabled = o.lights_enabled; x.line_smooth = o.line_smooth; x.line_stipple = o.line_stipple; x.logic_op = o.logic_op; x.index_logic_op = o.index_logic_op;
    x.normalize = o.normalize; x.rescale_normal = o.rescale_normal; x.point_smooth = o.point_smooth; x.point_sprite = o.point_sprite; x.polygon_smooth = o.polygon_smooth; x.polygon_stipple = o.polygon_stipple; x.offset_point = o.offset_point; x.offset_line = o.offset_line; x.color_sum = o.color_sum;
    for (uint32_t i = 0; i < 9; ++i) { x.map1[i] = o.map1[i]; x.map2[i] = o.map2[i]; }
    for (uint32_t i = 0; i < kMaxTexUnits; ++i) { x.units[i].enabled = o.units[i].enabled; x.units[i].texgen_enabled = o.units[i].texgen_enabled; }
    ds.alpha_test = os.alpha_test; ds.line_stipple = os.line_stipple; ds.logic_op = os.logic_op; ds.polygon_stipple = os.polygon_stipple; ds.point_sprite = os.point_sprite; ds.offset_line = os.offset_line; ds.offset_point = os.offset_point; ds.multisample = os.multisample; ds.depth_clamp = os.depth_clamp;
    gl_.blend_ = f.blend; gl_.cull_ = f.cull; gl_.depth_test_ = f.depth_test; gl_.stencil_test_ = f.stencil_test; gl_.scissor_test_ = f.scissor_test; gl_.offset_fill_ = f.offset_fill; gl_.dither_ = f.dither;
  }
  if (m & GL_EVAL_BIT) {
    x.auto_normal = o.auto_normal; x.grid1_n = o.grid1_n; x.grid2_n = o.grid2_n; x.grid2_m = o.grid2_m;
    for (int i = 0; i < 2; ++i) { x.grid1_u[i] = o.grid1_u[i]; x.grid2_u[i] = o.grid2_u[i]; x.grid2_v[i] = o.grid2_v[i]; }
    for (uint32_t i = 0; i < 9; ++i) { x.map1[i] = o.map1[i]; x.map2[i] = o.map2[i]; }
  }
  if (m & GL_HINT_BIT) for (int i = 0; i < 16; ++i) x.hints[i] = o.hints[i];
  if (m & GL_PIXEL_MODE_BIT) {
    for (int i = 0; i < 2; ++i) x.pixel_zoom[i] = o.pixel_zoom[i];
    for (int i = 0; i < 5; ++i) { x.pixel_scale[i] = o.pixel_scale[i]; x.pixel_bias[i] = o.pixel_bias[i]; }
    x.map_color = o.map_color; x.map_stencil = o.map_stencil; x.index_shift = o.index_shift; x.index_offset = o.index_offset;
    if (f.read_buffer != default_read_buffer_) { default_read_buffer_ = f.read_buffer; surface_generation_ = 0; surface_changed(); }
  }
  if (m & GL_ACCUM_BUFFER_BIT) for (int i = 0; i < 4; ++i) x.accum_clear[i] = o.accum_clear[i];
  attrib_stack_.remove_at(attrib_stack_.count() - 1);
  ++fx_generation_;
}

void GlApi::glPushClientAttrib(GLbitfield mask) {
  if (client_stack_.count() >= kMaxClientAttribStack) { error(GL_STACK_OVERFLOW); return; }
  UniquePointer<ClientFrame> frame = UniquePointer<ClientFrame>::make(a_);
  if (!frame) { error(GL_OUT_OF_MEMORY); return; }
  frame->mask = mask;
  frame->pack = pack_;
  frame->unpack = unpack_;
  frame->vao = *gl_.current_vao_ptr();
  frame->array_buffer = gl_.array_buffer_;
  frame->client_unit = client_active_unit_;
  client_stack_.push_back(static_cast<UniquePointer<ClientFrame>&&>(frame));
}
void GlApi::glPopClientAttrib() {
  if (client_stack_.count() == 0) { error(GL_STACK_UNDERFLOW); return; }
  const ClientFrame& f = *client_stack_[client_stack_.count() - 1];
  if (f.mask & GL_CLIENT_PIXEL_STORE_BIT) { pack_ = f.pack; unpack_ = f.unpack; }
  if (f.mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
    *gl_.current_vao_ptr() = f.vao;
    gl_.array_buffer_ = f.array_buffer;
    client_active_unit_ = f.client_unit;
  }
  client_stack_.remove_at(client_stack_.count() - 1);
}

// --- Selection and feedback -----------------------------------------------------------------------------------------------------------------------------------------

void FeedbackSink::reset(uint32_t new_mode, uint32_t new_capacity, uint64_t new_address, uint32_t new_type) {
  mode = new_mode;
  capacity = new_capacity;
  address = new_address;
  feedback_type = new_type;
  hits = 0;
  overflow = false;
  hit_pending = false;
  values_.clear();
}

void FeedbackSink::push(float v) {
  if (values_.count() >= capacity) { overflow = true; return; }
  values_.push_back(v);
}

void FeedbackSink::window(const gpu::RVertex& v, float out[4]) const {
  const float iw = v.pos[3] != 0 ? 1 / v.pos[3] : 0;
  out[0] = static_cast<float>(api_.gl_.vp_[0]) + (v.pos[0] * iw * 0.5f + 0.5f) * static_cast<float>(api_.gl_.vp_[2]);
  out[1] = static_cast<float>(api_.gl_.vp_[1]) + (v.pos[1] * iw * 0.5f + 0.5f) * static_cast<float>(api_.gl_.vp_[3]);
  out[2] = api_.depth_range_values_[0] + (v.pos[2] * iw * 0.5f + 0.5f) * (api_.depth_range_values_[1] - api_.depth_range_values_[0]);
  out[3] = iw;
}

void FeedbackSink::vertex_out(const gpu::RVertex& v) {
  float w[4];
  window(v, w);
  push(w[0]);
  push(w[1]);
  if (feedback_type != GL_2D) push(w[2]);
  if (feedback_type == GL_4D_COLOR_TEXTURE) push(w[3]);
  if (feedback_type == GL_3D_COLOR || feedback_type == GL_3D_COLOR_TEXTURE || feedback_type == GL_4D_COLOR_TEXTURE) {
    for (int i = 0; i < 4; ++i) push(color_at >= 0 ? v.var[color_at + i] : 0);
  }
  if (feedback_type == GL_3D_COLOR_TEXTURE || feedback_type == GL_4D_COLOR_TEXTURE) {
    for (int i = 0; i < 4; ++i) push(tex_at >= 0 ? v.var[tex_at + i] : (i == 3 ? 1.0f : 0.0f));
  }
}

namespace {
// Clips a polygon against the view volume, positions only (colours and coordinates by the same factor).
uint32_t clip_polygon(gpu::RVertex* in, uint32_t n, gpu::RVertex* out, uint32_t nvar, int plane) {
  const auto dist = [&](const gpu::RVertex& v) {
    switch (plane) {
      case 0: return v.pos[3] + v.pos[0];
      case 1: return v.pos[3] - v.pos[0];
      case 2: return v.pos[3] + v.pos[1];
      case 3: return v.pos[3] - v.pos[1];
      case 4: return v.pos[3] + v.pos[2];
      default: return v.pos[3] - v.pos[2];
    }
  };
  uint32_t count = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const gpu::RVertex& a = in[i];
    const gpu::RVertex& b = in[(i + 1) % n];
    const float da = dist(a), db = dist(b);
    if (da >= 0) out[count++] = a;
    if ((da >= 0) != (db >= 0)) {
      const float t = da / (da - db);
      gpu::RVertex m;
      for (int k = 0; k < 4; ++k) m.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t;
      for (uint32_t k = 0; k < nvar; ++k) m.var[k] = a.var[k] + (b.var[k] - a.var[k]) * t;
      out[count++] = m;
    }
  }
  return count;
}
}  // namespace

void FeedbackSink::hit(float z) {
  hit_pending = true;
  if (z < hit_min) hit_min = z;
  if (z > hit_max) hit_max = z;
}

void FeedbackSink::write_hit_record() {
  if (!hit_pending) return;
  ++hits;
  const auto as_float = [](uint32_t u) { return __builtin_bit_cast(float, u); };
  push(as_float(static_cast<uint32_t>(names_.count())));
  push(as_float(static_cast<uint32_t>(hit_min * 4294967295.0)));
  push(as_float(static_cast<uint32_t>(hit_max * 4294967295.0)));
  for (uint64_t i = 0; i < names_.count(); ++i) push(as_float(names_[i]));
  hit_pending = false;
  hit_min = 1;
  hit_max = 0;
}

void FeedbackSink::select_clipped(const gpu::RVertex* poly, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    float w[4];
    window(poly[i], w);
    hit(w[2]);
  }
}

void FeedbackSink::triangle(const gpu::RVertex& a, const gpu::RVertex& b, const gpu::RVertex& c) {
  // Culled as the rasterizer would.
  float wa[4], wb[4], wc[4];
  window(a, wa); window(b, wb); window(c, wc);
  const float area = (wb[0] - wa[0]) * (wc[1] - wa[1]) - (wc[0] - wa[0]) * (wb[1] - wa[1]);
  const bool front = api_.gl_.front_face_ == GL_CCW ? area > 0 : area < 0;
  if (api_.gl_.cull_) {
    const uint32_t mode_cull = api_.gl_.cull_mode_;
    if (mode_cull == GL_FRONT_AND_BACK || (mode_cull == GL_FRONT && front) || (mode_cull == GL_BACK && !front)) return;
  }
  gpu::RVertex buffers[2][12];
  buffers[0][0] = a; buffers[0][1] = b; buffers[0][2] = c;
  uint32_t n = 3;
  for (int plane = 0; plane < 6 && n; ++plane) n = clip_polygon(buffers[plane & 1], n, buffers[(plane & 1) ^ 1], gpu::kMaxVarying, plane);
  if (n < 3) return;
  const gpu::RVertex* poly = buffers[0];
  if (mode == GL_SELECT) { select_clipped(poly, n); return; }
  push(static_cast<float>(GL_POLYGON_TOKEN));
  push(static_cast<float>(n));
  for (uint32_t i = 0; i < n; ++i) vertex_out(poly[i]);
}

void FeedbackSink::line(const gpu::RVertex& a, const gpu::RVertex& b, bool first) {
  gpu::RVertex p = a, q = b;
  for (int plane = 0; plane < 6; ++plane) {
    const auto dist = [&](const gpu::RVertex& v) {
      switch (plane) {
        case 0: return v.pos[3] + v.pos[0];
        case 1: return v.pos[3] - v.pos[0];
        case 2: return v.pos[3] + v.pos[1];
        case 3: return v.pos[3] - v.pos[1];
        case 4: return v.pos[3] + v.pos[2];
        default: return v.pos[3] - v.pos[2];
      }
    };
    const float dp = dist(p), dq = dist(q);
    if (dp < 0 && dq < 0) return;
    if (dp < 0 || dq < 0) {
      const float t = dp / (dp - dq);
      gpu::RVertex m;
      for (int k = 0; k < 4; ++k) m.pos[k] = p.pos[k] + (q.pos[k] - p.pos[k]) * t;
      for (uint32_t k = 0; k < gpu::kMaxVarying; ++k) m.var[k] = p.var[k] + (q.var[k] - p.var[k]) * t;
      (dp < 0 ? p : q) = m;
    }
  }
  if (mode == GL_SELECT) {
    const gpu::RVertex both[2] = {p, q};
    select_clipped(both, 2);
    return;
  }
  push(static_cast<float>(first ? GL_LINE_RESET_TOKEN : GL_LINE_TOKEN));
  vertex_out(p);
  vertex_out(q);
}

void FeedbackSink::point(const gpu::RVertex& a) {
  for (int i = 0; i < 3; ++i) if (a.pos[i] > a.pos[3] || a.pos[i] < -a.pos[3]) return;
  if (mode == GL_SELECT) { select_clipped(&a, 1); return; }
  push(static_cast<float>(GL_POINT_TOKEN));
  vertex_out(a);
}

void FeedbackSink::pass_through(float token) {
  if (mode != GL_FEEDBACK) return;
  push(static_cast<float>(GL_PASS_THROUGH_TOKEN));
  push(token);
}

gpu::IPrimitiveSink* GlApi::feedback_sink() { return feedback_.get(); }

bool GlApi::ensure_feedback() {
  if (!feedback_) feedback_ = UniquePointer<FeedbackSink>::make(a_, a_, *this);
  if (!feedback_) error(GL_OUT_OF_MEMORY);
  return feedback_.get() != nullptr;
}

void GlApi::glFeedbackBuffer(GLsizei size, GLenum type, UserPtr buffer) {
  if (render_mode_ == GL_FEEDBACK) { error(GL_INVALID_OPERATION); return; }
  if (size < 0) { error(GL_INVALID_VALUE); return; }
  if (type != GL_2D && type != GL_3D && type != GL_3D_COLOR && type != GL_3D_COLOR_TEXTURE && type != GL_4D_COLOR_TEXTURE) { error(GL_INVALID_ENUM); return; }
  if (!ensure_feedback()) return;
  feedback_->capacity = static_cast<uint32_t>(size);
  feedback_->address = buffer.address;
  feedback_->feedback_type = type;
}
void GlApi::glSelectBuffer(GLsizei size, UserPtr buffer) {
  if (render_mode_ == GL_SELECT) { error(GL_INVALID_OPERATION); return; }
  if (size < 0) { error(GL_INVALID_VALUE); return; }
  if (!ensure_feedback()) return;
  feedback_->capacity = static_cast<uint32_t>(size);
  feedback_->address = buffer.address;
}
uint64_t GlApi::glRenderMode(GLenum mode) {
  if (mode != GL_RENDER && mode != GL_SELECT && mode != GL_FEEDBACK) { error(GL_INVALID_ENUM); return 0; }
  int64_t result = 0;
  if (render_mode_ != GL_RENDER && feedback_) {
    // Leaving a mode: the buffer goes to the program.
    if (render_mode_ == GL_SELECT) feedback_->write_hit_record();
    if (feedback_->address) write_values(UserPtr{feedback_->address}, feedback_->values().view().data(), feedback_->values().count());
    result = feedback_->overflow ? -1 : (render_mode_ == GL_SELECT ? static_cast<int64_t>(feedback_->hits) : static_cast<int64_t>(feedback_->values().count()));
    feedback_->values().clear();
    feedback_->hits = 0;
    feedback_->overflow = false;
    feedback_->names().clear();
  }
  if (mode != GL_RENDER) {
    if (!ensure_feedback()) return 0;
    if (feedback_->address == 0) { error(GL_INVALID_OPERATION); return 0; }
    feedback_->mode = mode;
    feedback_->hit_pending = false;
    feedback_->hit_min = 1;
    feedback_->hit_max = 0;
  }
  render_mode_ = mode;
  gl_.desktop.primitive_sink = mode == GL_RENDER ? nullptr : feedback_.get();
  return static_cast<uint64_t>(result);
}
void GlApi::glPassThrough(GLfloat token) { if (render_mode_ == GL_FEEDBACK && feedback_) feedback_->pass_through(token); }
void GlApi::glInitNames() { if (render_mode_ == GL_SELECT && feedback_) { feedback_->write_hit_record(); feedback_->names().clear(); } }
void GlApi::glLoadName(GLuint name) {
  if (render_mode_ != GL_SELECT || !feedback_) return;
  if (feedback_->names().count() == 0) { error(GL_INVALID_OPERATION); return; }
  feedback_->write_hit_record();
  feedback_->names()[feedback_->names().count() - 1] = name;
}
void GlApi::glPushName(GLuint name) {
  if (render_mode_ != GL_SELECT || !feedback_) return;
  if (feedback_->names().count() >= 64) { error(GL_STACK_OVERFLOW); return; }
  feedback_->write_hit_record();
  feedback_->names().push_back(name);
}
void GlApi::glPopName() {
  if (render_mode_ != GL_SELECT || !feedback_) return;
  if (feedback_->names().count() == 0) { error(GL_STACK_UNDERFLOW); return; }
  feedback_->write_hit_record();
  feedback_->names().remove_at(feedback_->names().count() - 1);
}
uint32_t GlApi::name_stack_depth() const { return feedback_ ? static_cast<uint32_t>(feedback_->names().count()) : 0; }
uint32_t GlApi::feedback_capacity() const { return feedback_ ? feedback_->capacity : 0; }
uint32_t GlApi::feedback_type() const { return feedback_ ? feedback_->feedback_type : GL_2D; }
uint64_t GlApi::feedback_address() const { return feedback_ ? feedback_->address : 0; }

// --- Queries ------------------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenQueries(GLsizei n, UserPtr ids) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = query_names_.generate();
    if (!write_values(ids.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glDeleteQueries(GLsizei n, UserPtr ids) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(ids.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (!name) continue;
    const uint32_t object = query_names_.object(name);
    if (object) gl_.delete_object('q', object);
    query_names_.erase(name);
  }
}
uint64_t GlApi::glIsQuery(GLuint id) { return id && query_names_.object(id) != 0; }
void GlApi::glBeginQuery(GLenum target, GLuint id) { glBeginQueryIndexed(target, 0, id); }
void GlApi::glEndQuery(GLenum target) { gl_.end_query(target); }
void GlApi::glGetQueryiv(GLenum target, GLenum pname, UserPtr params) { glGetQueryIndexediv(target, 0, pname, params); }
// (the index: the vertex stream of GL_PRIMITIVES_GENERATED and GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN; 0 for the others)
void GlApi::glBeginQueryIndexed(GLenum target, GLuint index, GLuint id) {
  if (!id || !query_names_.known(id)) { error(GL_INVALID_OPERATION); return; }
  uint32_t object = query_names_.object(id);
  if (!object) {
    object = gl_.create_query();
    query_names_.set(id, object);
  }
  gl_.begin_query(target, object, index);
}
void GlApi::glEndQueryIndexed(GLenum target, GLuint index) { gl_.end_query(target, index); }
void GlApi::glGetQueryIndexediv(GLenum target, GLuint index, GLenum pname, UserPtr params) {
  if (index >= 4) { error(GL_INVALID_VALUE); return; }
  int32_t out = 0;
  if (pname == GL_CURRENT_QUERY) out = static_cast<int32_t>(query_names_.name_of(gl_.current_query(target, index)));
  else if (pname == GL_QUERY_COUNTER_BITS) out = 64;
  else { error(GL_INVALID_ENUM); return; }
  write_values(params, &out, 1);
}
void GlApi::query_result_get(uint32_t id, uint32_t pname, uint64_t* out) {
  const uint32_t object = query_names_.object(id);
  double v = 0;
  if (!object || !gl_.query_result(object, pname, &v)) { error(GL_INVALID_OPERATION); return; }
  *out = static_cast<uint64_t>(v);
}
void GlApi::glGetQueryObjectiv(GLuint id, GLenum pname, UserPtr params) {
  uint64_t v = 0;
  query_result_get(id, pname, &v);
  const int32_t out = static_cast<int32_t>(v);
  write_values(params, &out, 1);
}
void GlApi::glGetQueryObjectuiv(GLuint id, GLenum pname, UserPtr params) {
  uint64_t v = 0;
  query_result_get(id, pname, &v);
  const uint32_t out = static_cast<uint32_t>(v);
  write_values(params, &out, 1);
}
void GlApi::glGetQueryObjecti64v(GLuint id, GLenum pname, UserPtr params) {
  uint64_t v = 0;
  query_result_get(id, pname, &v);
  write_values(params, &v, 1);
}
void GlApi::glGetQueryObjectui64v(GLuint id, GLenum pname, UserPtr params) { glGetQueryObjecti64v(id, pname, params); }

// --- Sync objects: everything finishes before the call that started it returns, so a fence is always signalled. ---------------------------------------------------

uint64_t GlApi::glFenceSync(GLenum condition, GLbitfield flags) {
  if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) { error(GL_INVALID_ENUM); return 0; }
  if (flags != 0) { error(GL_INVALID_VALUE); return 0; }
  ++sync_serial_;
  if (!live_syncs_.push_back(sync_serial_)) { error(GL_OUT_OF_MEMORY); return 0; }
  return sync_serial_;
}
uint64_t GlApi::glIsSync(GLsync sync) {
  for (uint64_t i = 0; i < live_syncs_.count(); ++i) if (live_syncs_[i] == sync) return 1;
  return 0;
}
void GlApi::glDeleteSync(GLsync sync) {
  if (sync == 0) return;
  for (uint64_t i = 0; i < live_syncs_.count(); ++i) if (live_syncs_[i] == sync) { live_syncs_.remove_at(i); return; }
  error(GL_INVALID_VALUE);
}
uint64_t GlApi::glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64) {
  if (flags & ~GL_SYNC_FLUSH_COMMANDS_BIT) { error(GL_INVALID_VALUE); return GL_WAIT_FAILED; }
  if (!glIsSync(sync)) { error(GL_INVALID_VALUE); return GL_WAIT_FAILED; }
  return GL_ALREADY_SIGNALED;
}
void GlApi::glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
  if (flags != 0 || timeout != GL_TIMEOUT_IGNORED) { error(GL_INVALID_VALUE); return; }
  if (!glIsSync(sync)) error(GL_INVALID_VALUE);
}
void GlApi::glGetSynciv(GLsync sync, GLenum pname, GLsizei count, UserPtr length, UserPtr values) {
  if (!glIsSync(sync)) { error(GL_INVALID_VALUE); return; }
  int32_t v = 0;
  switch (pname) {
    case GL_OBJECT_TYPE: v = GL_SYNC_FENCE; break;
    case GL_SYNC_STATUS: v = GL_SIGNALED; break;
    case GL_SYNC_CONDITION: v = GL_SYNC_GPU_COMMANDS_COMPLETE; break;
    case GL_SYNC_FLAGS: v = 0; break;
    default: error(GL_INVALID_ENUM); return;
  }
  if (count > 0) write_values(values, &v, 1);
  if (!length.null()) { const int32_t n = 1; write_values(length, &n, 1); }
}

// --- A single-buffered window shows what is drawn: glFlush and glFinish tell the service. ------------------------------------------------------------------------------

void GlApi::flush_to_surface() {
  gl_.hw_sync();
  if (surface_ && !surface_->double_buffered()) surface_->mark_flushed();
  else if (surface_ && default_draw_buffer_ != GL_BACK && default_draw_buffer_ != GL_BACK_LEFT) surface_->mark_flushed();
}

void GlApi::clear_accum() {
  if (!surface_) return;
  ArrayView<float> accum = surface_->accum();
  if (accum.empty()) return;
  const uint32_t w = surface_->width(), h = surface_->height();
  int32_t x0 = 0, y0 = 0, x1 = static_cast<int32_t>(w), y1 = static_cast<int32_t>(h);
  if (gl_.scissor_test_) {
    if (gl_.sc_[0] > x0) x0 = gl_.sc_[0];
    if (gl_.sc_[1] > y0) y0 = gl_.sc_[1];
    if (gl_.sc_[0] + gl_.sc_[2] < x1) x1 = gl_.sc_[0] + gl_.sc_[2];
    if (gl_.sc_[1] + gl_.sc_[3] < y1) y1 = gl_.sc_[1] + gl_.sc_[3];
  }
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      const uint64_t at = (uint64_t(y) * w + uint64_t(x)) * 4;
      for (uint64_t k = 0; k < 4; ++k) accum[at + k] = fx_.accum_clear[k];
    }
  }
}

// Draws inside display lists: the arrays as the commands they stand for. Each vertex becomes the current-value commands and a glVertex4fv, written into the list.
bool GlApi::list_expand_draw(uint32_t op, ConstByteView command) {
  const auto word = [&](uint32_t i) -> uint32_t {
    const uint64_t at = uint64_t{i} * 4;
    if (at + 4 > command.size()) return 0;
    return uint32_t{command[at]} | uint32_t{command[at + 1]} << 8 | uint32_t{command[at + 2]} << 16 | uint32_t{command[at + 3]} << 24;
  };
  const auto u64_at = [&](uint32_t i) { return uint64_t{word(i)} | uint64_t{word(i + 1)} << 32; };
  webgl::Vao* vao = gl_.current_vao_ptr();
  DisplayList& list = *list_building_;
  const auto emit_floats = [&](uint32_t which, const float* v, uint32_t n) {
    list.commands.put32(which | (n + 1) << 16);
    for (uint32_t i = 0; i < n; ++i) list.commands.put32(__builtin_bit_cast(uint32_t, v[i]));
  };
  // One element of every enabled array.
  const auto element = [&](int64_t index) -> bool {
    float v[4];
    const auto fetch = [&](uint32_t attr) -> bool {
      const webgl::VertexAttrib& at = vao->attribs[attr];
      if (!at.enabled) return false;
      const uint32_t bytes = webgl::vertex_attrib_element_bytes(at);
      const uint64_t stride = at.stride ? at.stride : bytes;
      uint8_t raw[32];
      if (bytes > sizeof(raw)) return false;
      if (at.from_client) {
        if (!read_client(UserPtr{at.client + static_cast<uint64_t>(index) * stride}, ByteView(raw, bytes))) return false;
      } else {
        const webgl::Buffer* b = gl_.buffer_at(at.buffer);
        ConstByteView part;
        if (!b || !b->data.view().try_subview(at.offset + static_cast<uint64_t>(index) * stride, bytes, &part)) return false;
        ByteView(raw, bytes).copy_from(part);
      }
      uint32_t lanes[4];
      webgl::read_vertex_attrib(at, raw, lanes);
      for (int k = 0; k < 4; ++k) v[k] = __builtin_bit_cast(float, lanes[k]);
      return true;
    };
    if (fetch(kAttrNormal)) emit_floats(kOp_glNormal3fv, v, 3);
    if (fetch(kAttrColor)) emit_floats(kOp_glColor4fv, v, 4);
    if (fetch(kAttrSecondary)) emit_floats(kOp_glSecondaryColor3fv, v, 3);
    if (fetch(kAttrFog)) emit_floats(kOp_glFogCoordfv, v, 1);
    for (uint32_t u = 0; u < kMaxTexUnits; ++u) {
      if (!fetch(kAttrTex0 + u)) continue;
      list.commands.put32(kOp_glMultiTexCoord4fv | 6u << 16);
      list.commands.put32(GL_TEXTURE0 + u);
      for (int k = 0; k < 4; ++k) list.commands.put32(__builtin_bit_cast(uint32_t, v[k]));
    }
    if (fetch(kAttrVertex)) emit_floats(kOp_glVertex4fv, v, 4);
    return true;
  };
  const auto begin = [&](uint32_t mode) { list.commands.put32(kOp_glBegin | 2u << 16); list.commands.put32(mode); };
  const auto end = [&]() { list.commands.put32(kOp_glEnd | 1u << 16); };
  switch (op) {
    case kOp_glArrayElement: element(static_cast<int32_t>(word(1))); return true;
    case kOp_glDrawArrays: {
      begin(word(1));
      for (int64_t i = 0; i < static_cast<int32_t>(word(3)); ++i) element(static_cast<int32_t>(word(2)) + i);
      end();
      return true;
    }
    case kOp_glMultiDrawArrays: return true;
    case kOp_glDrawElements: case kOp_glDrawRangeElements: {
      const uint32_t mode = word(1);
      const uint32_t first_arg = op == kOp_glDrawElements ? 2 : 4;
      const int32_t count = static_cast<int32_t>(word(first_arg));
      const uint32_t type = word(first_arg + 1);
      const uint64_t address = u64_at(first_arg + 2);
      const uint32_t size = type == GL_UNSIGNED_BYTE ? 1 : (type == GL_UNSIGNED_SHORT ? 2 : 4);
      begin(mode);
      const webgl::Buffer* eb = gl_.buffer_at(vao->element_buffer);
      for (int32_t i = 0; i < count; ++i) {
        uint8_t raw[4] = {0, 0, 0, 0};
        const uint64_t at = uint64_t{static_cast<uint32_t>(i)} * size;
        if (eb) {
          ConstByteView part;
          if (!eb->data.view().try_subview(address + at, size, &part)) break;
          ByteView(raw, size).copy_from(part);
        } else if (!read_client(UserPtr{address + at}, ByteView(raw, size))) {
          break;
        }
        element(int64_t{uint32_t{raw[0]} | uint32_t{raw[1]} << 8 | uint32_t{raw[2]} << 16 | uint32_t{raw[3]} << 24});
      }
      end();
      return true;
    }
    default: return true;
  }
}

// The compatibility uniforms a desktop GLSL program reads (GlslDesktop.hpp), from the fixed-function state, before each of its draws: every uniform leaf named tcos_* is
// one of them (tcos_LightSource[2].diffuse, tcos_TextureMatrix with 8 elements ...).
bool GlApi::compat_value(StringView name, uint32_t element, float out[16], uint32_t* cols, uint32_t* rows) {
  // base[index].field
  uint64_t i = 0;
  while (i < name.size() && name[i] != '[' && name[i] != '.') ++i;
  const StringView base = name.substr(0, i);
  uint32_t index = element;
  if (i < name.size() && name[i] == '[') {
    index = 0;
    ++i;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') index = index * 10 + static_cast<uint32_t>(name[i++] - '0');
    if (i < name.size() && name[i] == ']') ++i;
  }
  const StringView field = i < name.size() && name[i] == '.' ? name.substr(i + 1, name.size() - i - 1) : StringView();
  const auto vec = [&](const float* v, uint32_t n) { for (uint32_t k = 0; k < n; ++k) out[k] = v[k]; *cols = 1; *rows = n; return true; };
  const auto one = [&](float v) { out[0] = v; *cols = 1; *rows = 1; return true; };
  const auto mat = [&](const Mat4& m) { for (int k = 0; k < 16; ++k) out[k] = m.m[k]; *cols = 4; *rows = 4; return true; };
  const auto variants = [&](StringView stem, const Mat4& m) -> int {      // 1 handled, 0 not this matrix
    if (!base.starts_with(stem)) return 0;
    const StringView rest = base.substr(stem.size(), base.size() - stem.size());
    Mat4 inv;
    if (rest.empty()) return mat(m), 1;
    if (rest.equals("Transpose")) return mat(mat_transpose(m)), 1;
    mat_inverse(m, &inv);
    if (rest.equals("Inverse")) return mat(inv), 1;
    if (rest.equals("InverseTranspose")) return mat(mat_transpose(inv)), 1;
    return 0;
  };
  const Mat4& mv = modelview_.entries[modelview_.top];
  const Mat4& pr = projection_.entries[projection_.top];
  if (base.starts_with("ModelViewProjectionMatrix")) return variants("ModelViewProjectionMatrix", mat_multiply(pr, mv)) == 1;
  if (base.starts_with("ModelViewMatrix")) return variants("ModelViewMatrix", mv) == 1;
  if (base.starts_with("ProjectionMatrix")) return variants("ProjectionMatrix", pr) == 1;
  if (base.starts_with("TextureMatrix")) {
    if (index >= kMaxTexUnits) return false;
    return variants("TextureMatrix", texture_stack_[index].entries[texture_stack_[index].top]) == 1;
  }
  if (base.equals("NormalMatrix") || base.equals("NormalScale")) {
    Mat4 inv;
    mat_inverse(mv, &inv);
    if (base.equals("NormalScale")) {
      const float len = shader::fsqrt(inv.m[8] * inv.m[8] + inv.m[9] * inv.m[9] + inv.m[10] * inv.m[10]);
      return one(len > 0 ? 1.0f / len : 1.0f);
    }
    const float nm[9] = {inv.m[0], inv.m[4], inv.m[8], inv.m[1], inv.m[5], inv.m[9], inv.m[2], inv.m[6], inv.m[10]};
    for (int k = 0; k < 9; ++k) out[k] = nm[k];
    *cols = 3;
    *rows = 3;
    return true;
  }
  if (base.equals("DepthRange")) {
    if (field.equals("near")) return one(depth_range_values_[0]);
    if (field.equals("far")) return one(depth_range_values_[1]);
    if (field.equals("diff")) return one(depth_range_values_[1] - depth_range_values_[0]);
    return false;
  }
  if (base.equals("Point")) {
    if (field.equals("size")) return one(fx_.point_size);
    if (field.equals("sizeMin")) return one(fx_.point_min);
    if (field.equals("sizeMax")) return one(fx_.point_max);
    if (field.equals("fadeThresholdSize")) return one(fx_.point_fade);
    if (field.equals("distanceConstantAttenuation")) return one(fx_.point_atten[0]);
    if (field.equals("distanceLinearAttenuation")) return one(fx_.point_atten[1]);
    if (field.equals("distanceQuadraticAttenuation")) return one(fx_.point_atten[2]);
    return false;
  }
  const auto material_field = [&](const Material& m) {
    if (field.equals("emission")) return vec(m.emission, 4);
    if (field.equals("ambient")) return vec(m.ambient, 4);
    if (field.equals("diffuse")) return vec(m.diffuse, 4);
    if (field.equals("specular")) return vec(m.specular, 4);
    if (field.equals("shininess")) return one(m.shininess);
    return false;
  };
  if (base.equals("FrontMaterial")) return material_field(fx_.material[0]);
  if (base.equals("BackMaterial")) return material_field(fx_.material[1]);
  if (base.equals("LightSource")) {
    if (index >= kMaxLights) return false;
    const Light& l = fx_.lights[index];
    if (field.equals("ambient")) return vec(l.ambient, 4);
    if (field.equals("diffuse")) return vec(l.diffuse, 4);
    if (field.equals("specular")) return vec(l.specular, 4);
    if (field.equals("position")) return vec(l.position, 4);
    if (field.equals("halfVector")) {
      // the half way vector of a light at infinity and the viewer at infinity (+z)
      float h[4] = {l.position[0], l.position[1], l.position[2], 0};
      const float len = shader::fsqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
      if (len > 0) for (int k = 0; k < 3; ++k) h[k] /= len;
      h[2] += 1;
      const float hl = shader::fsqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
      if (hl > 0) for (int k = 0; k < 3; ++k) h[k] /= hl;
      return vec(h, 4);
    }
    if (field.equals("spotDirection")) return vec(l.spot_direction, 3);
    if (field.equals("spotExponent")) return one(l.spot_exponent);
    if (field.equals("spotCutoff")) return one(l.spot_cutoff);
    if (field.equals("spotCosCutoff")) return one(shader::fcos(l.spot_cutoff * 3.14159265358979f / 180.0f));
    if (field.equals("constantAttenuation")) return one(l.attenuation[0]);
    if (field.equals("linearAttenuation")) return one(l.attenuation[1]);
    if (field.equals("quadraticAttenuation")) return one(l.attenuation[2]);
    return false;
  }
  if (base.equals("LightModel")) return field.equals("ambient") && vec(fx_.model_ambient, 4);
  if (base.equals("FrontLightModelProduct") || base.equals("BackLightModelProduct")) {
    const Material& m = fx_.material[base.starts_with("Back") ? 1 : 0];
    float c[4];
    for (int k = 0; k < 3; ++k) c[k] = m.emission[k] + m.ambient[k] * fx_.model_ambient[k];
    c[3] = m.diffuse[3];
    return field.equals("sceneColor") && vec(c, 4);
  }
  if (base.equals("FrontLightProduct") || base.equals("BackLightProduct")) {
    if (index >= kMaxLights) return false;
    const Material& m = fx_.material[base.starts_with("Back") ? 1 : 0];
    const Light& l = fx_.lights[index];
    const float* a = nullptr;
    const float* b = nullptr;
    if (field.equals("ambient")) { a = m.ambient; b = l.ambient; }
    else if (field.equals("diffuse")) { a = m.diffuse; b = l.diffuse; }
    else if (field.equals("specular")) { a = m.specular; b = l.specular; }
    else return false;
    float c[4];
    for (int k = 0; k < 4; ++k) c[k] = a[k] * b[k];
    return vec(c, 4);
  }
  if (base.equals("Fog")) {
    if (field.equals("color")) return vec(fx_.fog_color, 4);
    if (field.equals("density")) return one(fx_.fog_density);
    if (field.equals("start")) return one(fx_.fog_start);
    if (field.equals("end")) return one(fx_.fog_end);
    if (field.equals("scale")) return one(fx_.fog_end != fx_.fog_start ? 1.0f / (fx_.fog_end - fx_.fog_start) : 1.0f);
    return false;
  }
  if (base.equals("ClipPlane")) {
    if (index >= kMaxClipPlanes) return false;
    const float c[4] = {static_cast<float>(fx_.clip_plane[index][0]), static_cast<float>(fx_.clip_plane[index][1]), static_cast<float>(fx_.clip_plane[index][2]), static_cast<float>(fx_.clip_plane[index][3])};
    return vec(c, 4);
  }
  if (index >= kMaxTexUnits) return false;
  const FixedState::Unit& u = fx_.units[index];
  if (base.equals("TextureEnvColor")) return vec(u.env.color, 4);
  static const char* const kPlanes = "STRQ";
  for (uint32_t c = 0; c < 4; ++c) {
    if (base.size() == 10 && base.starts_with("EyePlane") && base[8] == kPlanes[c]) return vec(u.gen[c].eye_plane, 4);
    if (base.size() == 13 && base.starts_with("ObjectPlane") && base[11] == kPlanes[c]) return vec(u.gen[c].object_plane, 4);
  }
  return false;
}

void GlApi::load_compat_uniforms(uint32_t program) {
  const webgl::ProgramObj* po = gl_.program(program);
  if (!po) return;
  for (uint64_t i = 0; i < po->uniforms.count(); ++i) {
    const webgl::UniformLeaf& u = *po->uniforms[i];
    if (!u.name.view().starts_with("tcos_")) continue;
    const StringView name = u.name.view().substr(5, u.name.view().size() - 5);
    for (uint32_t e = 0; e < u.size; ++e) {
      float v[16];
      uint32_t cols = 1, rows = 1;
      if (!compat_value(name, e, v, &cols, &rows)) break;
      uint32_t lanes[16];
      for (uint32_t k = 0; k < cols * rows; ++k) lanes[k] = __builtin_bit_cast(uint32_t, v[k]);
      gl_.uniform(static_cast<int32_t>(u.location + e), 'f', cols, rows, 1, lanes);
    }
  }
}

}  // namespace opengl
